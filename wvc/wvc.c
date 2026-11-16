// Wavert native runtime, forked from Γ--'s runtime/wah.c (wah/runtime). This file is inlined
// verbatim at the top of every program wvc compiles.
// It needs a GNU C compiler (clang or gcc; `zig cc` is clang).
//
// Values: a 16-byte tagged wv. int -> int64_t (checked to ±(2^53-1)),
// float -> double, string -> immutable UTF-8 wstr, array -> warr, record ->
// wrec (keys shared by every record of one literal), enum member -> pointer
// to a static wmem, null -> T_NULL. T_UNSET marks a binding before its
// declaration (temporal dead zone).
//
// Memory is reclaimed by a conservative mark-and-sweep collector (see "memory").
//
// A fail is a pending code in rt_failcode: a function or stdlib function sets
// it and returns, and the caller either handles it (onfail) or turns it into
// a panic with rt_chk (the one-frame rule, §8.2). A panic ends the program:
// panics cannot be caught yet, so nothing unwinds.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW: zig cc links shell32 by default
#include <malloc.h> // _aligned_malloc
#include <wchar.h>
#else
#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
extern char **environ;
#endif
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

// ---- values ----

enum { T_UNSET, T_NULL, T_INT, T_FLOAT, T_BOOL, T_STR, T_ARR, T_REC, T_MEM, T_MAP };

// A string. data holds bytes valid UTF-8 and is followed by a NUL.
typedef struct wstr {
  int64_t bytes, cps;
  int32_t ascii, sym; // sym: interned as a record key
  const char *data;
} wstr;
typedef struct warr { int64_t len, cap; int is_mut; struct wv *items; } warr;
typedef struct wrec { int64_t n; int is_mut; const wstr *const *keys; struct wv *vals; } wrec;
typedef struct wmem { const wstr *enum_name, *name; } wmem;
struct wmap;
typedef struct wv {
  uint32_t tag;
  union { int64_t i; double f; int b; const wstr *s; warr *a; wrec *r; const wmem *m; struct wmap *mp; };
} wv;
// A map (§10.11): entries in insertion order, some of them removed (live 0),
// and a hash index of entry numbers (-1 for an empty slot), icap a power of two.
typedef struct wment { wv key, val; int live; } wment;
typedef struct wmap { int64_t n, len, cap, icap; int is_mut; wment *e; int64_t *idx; } wmap;

#define RT_MAX 9007199254740991LL
static const wv RT_NULL = { .tag = T_NULL };
static const wv RT_UNSET = { .tag = T_UNSET };
static inline wv rt_i(int64_t x) { return (wv){ .tag = T_INT, .i = x }; }
static inline wv rt_f(double x) { return (wv){ .tag = T_FLOAT, .f = x }; }
static inline wv rt_b(int x) { return (wv){ .tag = T_BOOL, .b = !!x }; }
static inline wv rt_s(const wstr *s) { return (wv){ .tag = T_STR, .s = s }; }
static inline wv rt_m(const wmem *m) { return (wv){ .tag = T_MEM, .m = m }; }
#define RT_NAN rt_f(NAN)
#define RT_INF rt_f(INFINITY)
static inline int isnum(wv x) { return x.tag == T_INT || x.tag == T_FLOAT; }
static inline double nv(wv x) { return x.tag == T_INT ? (double)x.i : x.f; }

// ---- memory: a mark-and-sweep collector ----
// Values are allocated with rt_alloc, or rt_alloc_raw for objects that hold no
// references (string data). Objects never move. Small objects live in 256 KiB
// blocks aligned to their size, each holding objects of one size class, so any
// address maps to its block; objects over GC_SMALL bytes are allocated one by one.
//
// A collection marks everything reachable from the roots, then frees the rest.
// The roots are the program thread's stack and registers, and the ranges given
// to rt_root: the program's globals and the runtime's own caches. Roots and
// objects are scanned conservatively: every aligned word that points into an
// object, or just past its end, keeps it. Raw objects are not scanned.
// Collections happen only in rt_alloc, on the program thread, which is the only
// thread that allocates. Memory from malloc (xmalloc) is not scanned, so it must
// never hold the only reference to an object while anything is allocated.
//
// WVT_GC_STRESS=n collects every n allocations and fills freed objects with
// 0xDD, to expose missing roots. WVT_GC_STATS prints statistics at exit.

static void rt_flush(void);
static void term_restore(void);
static _Noreturn void out_of_memory(void) {
  rt_flush(); // output written before the failure still appears
  term_restore();
  fputs("out of memory\n", stderr);
  exit(1);
}
static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) out_of_memory();
  return p;
}

#define GC_BLOCK ((uintptr_t)1 << 18)
#define GC_SMALL 16384
#define GC_MAXSLOTS (GC_BLOCK / 16)
#define GC_MIN ((size_t)8 << 20) // a collection happens after allocating max(GC_MIN, live) bytes

typedef struct gc_block {
  struct gc_block *next; // in the pool of empty blocks
  char *first;           // the first slot
  uint32_t size, slots;
  uint64_t inv;          // ceil(2^32 / size): offset * inv >> 32 is offset / size within a block
  int cls, raw;          // cls is -1 for an empty block
  uint64_t alloc[GC_MAXSLOTS / 64], mark[GC_MAXSLOTS / 64];
} gc_block;
typedef struct gc_big { size_t size; int raw, mark; } gc_big; // followed by the object
typedef struct { char *p; size_t n; } gc_range;

static uint32_t gc_sizes[64];
static int gc_ncls, gc_ready, gc_stats;
static uint8_t gc_cls_of[GC_SMALL / 16 + 1];
static void *gc_free[2][64];                       // free slots, by raw and class
static gc_block **gc_blocks, **gc_set, *gc_pool;   // every block; the blocks by address; the empty ones
static size_t gc_nblocks, gc_capblocks, gc_setcap;
static gc_big **gc_bigs;                           // the big objects, by address
static size_t gc_nbigs, gc_capbigs, gc_big_bytes;
static uintptr_t gc_lo = UINTPTR_MAX, gc_hi;       // every object is between these
static size_t gc_since, gc_next = GC_MIN, gc_live, gc_runs, gc_peak, gc_stress, gc_count;
static char *gc_stack_base;                        // the top of the program thread's stack
static gc_range *gc_roots, *gc_marks;
static size_t gc_nroots, gc_caproots, gc_nmarks, gc_capmarks;

static void gc_init(void) {
  gc_ready = 1;
  // 16 to 128 bytes in steps of 16, then four sizes for each doubling
  for (uint32_t s = 16; s <= GC_SMALL; s += s < 128 ? 16 : ((uint32_t)1 << (31 - __builtin_clz(s))) / 4) gc_sizes[gc_ncls++] = s;
  for (int c = 0, i = 0; i <= GC_SMALL / 16; i++) {
    while (gc_sizes[c] < (uint32_t)i * 16) c++;
    gc_cls_of[i] = (uint8_t)c;
  }
  const char *s = getenv("WVT_GC_STRESS");
  if (s) gc_stress = strtoull(s, NULL, 10) ? strtoull(s, NULL, 10) : 1;
  gc_stats = getenv("WVT_GC_STATS") != NULL;
}

static void gc_grow(void *list, size_t *cap, size_t need, size_t item) {
  if (need <= *cap) return;
  size_t c = *cap ? *cap * 2 : 64;
  while (c < need) c *= 2;
  void *p = realloc(*(void **)list, c * item);
  if (!p) out_of_memory();
  *(void **)list = p;
  *cap = c;
}
static void gc_span(uintptr_t lo, uintptr_t hi) {
  if (lo < gc_lo) gc_lo = lo;
  if (hi > gc_hi) gc_hi = hi;
  size_t heap = gc_nblocks * GC_BLOCK + gc_big_bytes;
  if (heap > gc_peak) gc_peak = heap;
}

// The blocks are found by address in an open-addressing hash set.
static size_t gc_hash(uintptr_t b) { return (size_t)((b / GC_BLOCK) * 0x9E3779B97F4A7C15ULL); }
static void gc_set_put(gc_block *b) {
  size_t j = gc_hash((uintptr_t)b) & (gc_setcap - 1);
  while (gc_set[j]) j = (j + 1) & (gc_setcap - 1);
  gc_set[j] = b;
}
static gc_block *gc_block_of(uintptr_t p) {
  if (!gc_setcap) return NULL;
  uintptr_t b = p & ~(GC_BLOCK - 1);
  for (size_t j = gc_hash(b) & (gc_setcap - 1); gc_set[j]; j = (j + 1) & (gc_setcap - 1)) {
    if ((uintptr_t)gc_set[j] == b) return gc_set[j];
  }
  return NULL;
}
static gc_block *gc_new_block(void) {
  void *m;
#ifdef _WIN32
  m = _aligned_malloc(GC_BLOCK, GC_BLOCK);
#else
  if (posix_memalign(&m, GC_BLOCK, GC_BLOCK)) m = NULL;
#endif
  if (!m) out_of_memory();
  gc_block *b = m;
  gc_grow(&gc_blocks, &gc_capblocks, gc_nblocks + 1, sizeof *gc_blocks);
  gc_blocks[gc_nblocks++] = b;
  if (gc_nblocks * 2 > gc_setcap) {
    free(gc_set);
    gc_setcap = gc_setcap ? gc_setcap * 2 : 64;
    gc_set = xmalloc(gc_setcap * sizeof *gc_set);
    memset(gc_set, 0, gc_setcap * sizeof *gc_set);
    for (size_t i = 0; i < gc_nblocks; i++) gc_set_put(gc_blocks[i]);
  } else {
    gc_set_put(b);
  }
  gc_span((uintptr_t)b, (uintptr_t)b + GC_BLOCK);
  return b;
}
// A block for class c, its slots added to the class's free list.
static void gc_refill(int raw, int c) {
  gc_block *b = gc_pool;
  if (b) gc_pool = b->next;
  else b = gc_new_block();
  size_t header = (sizeof(gc_block) + 15) & ~(size_t)15;
  b->next = NULL;
  b->first = (char *)b + header;
  b->size = gc_sizes[c];
  b->inv = (((uint64_t)1 << 32) + b->size - 1) / b->size;
  b->slots = (uint32_t)((GC_BLOCK - header) / b->size);
  b->cls = c;
  b->raw = raw;
  memset(b->alloc, 0, sizeof b->alloc);
  memset(b->mark, 0, sizeof b->mark);
  void *head = gc_free[raw][c];
  for (uint32_t i = b->slots; i-- > 0;) {
    void *p = b->first + (size_t)i * b->size;
    *(void **)p = head;
    head = p;
  }
  gc_free[raw][c] = head;
}

// The big object that p points into or just past, or NULL.
static gc_big *gc_big_of(uintptr_t p) {
  size_t lo = 0, hi = gc_nbigs;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if ((uintptr_t)gc_bigs[mid] <= p) lo = mid + 1;
    else hi = mid;
  }
  if (!lo) return NULL;
  gc_big *h = gc_bigs[lo - 1];
  uintptr_t start = (uintptr_t)(h + 1);
  return p >= start && p <= start + h->size ? h : NULL;
}
static void *gc_big_alloc(size_t n, int raw) {
  gc_big *h = malloc(sizeof(gc_big) + n);
  if (!h) out_of_memory();
  *h = (gc_big){ n, raw, 0 };
  if (!raw) memset(h + 1, 0, n);
  size_t lo = 0, hi = gc_nbigs;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (gc_bigs[mid] < h) lo = mid + 1;
    else hi = mid;
  }
  gc_grow(&gc_bigs, &gc_capbigs, gc_nbigs + 1, sizeof *gc_bigs);
  memmove(gc_bigs + lo + 1, gc_bigs + lo, (gc_nbigs - lo) * sizeof *gc_bigs);
  gc_bigs[lo] = h;
  gc_nbigs++;
  gc_big_bytes += n;
  gc_since += n;
  gc_span((uintptr_t)h, (uintptr_t)(h + 1) + n);
  return h + 1;
}

// ---- marking ----

static void gc_mark_slot(gc_block *b, size_t i) {
  uint64_t bit = (uint64_t)1 << (i & 63);
  if (!(b->alloc[i >> 6] & bit) || (b->mark[i >> 6] & bit)) return;
  b->mark[i >> 6] |= bit;
  if (b->raw) return;
  gc_grow(&gc_marks, &gc_capmarks, gc_nmarks + 1, sizeof *gc_marks);
  gc_marks[gc_nmarks++] = (gc_range){ b->first + i * b->size, b->size };
}
// Marks the object that p points into, or just past.
static void gc_mark(uintptr_t p) {
  if (p < gc_lo || p > gc_hi) return;
  gc_block *b = gc_block_of(p);
  if (b) {
    if (b->cls < 0 || p < (uintptr_t)b->first) return;
    size_t off = p - (uintptr_t)b->first, i = (size_t)((off * b->inv) >> 32);
    if (i < b->slots) gc_mark_slot(b, i);
    if (off == i * b->size && i > 0 && i <= b->slots) gc_mark_slot(b, i - 1);
    return;
  }
  gc_big *h = gc_big_of(p);
  if (h && !h->mark) {
    h->mark = 1;
    if (h->raw) return;
    gc_grow(&gc_marks, &gc_capmarks, gc_nmarks + 1, sizeof *gc_marks);
    gc_marks[gc_nmarks++] = (gc_range){ (char *)(h + 1), h->size };
  }
}
static void gc_scan(const char *p, size_t n) {
  const uintptr_t *w = (const uintptr_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7);
  for (; (const char *)(w + 1) <= p + n; w++) gc_mark(*w);
}

// ---- sweeping ----

static void gc_sweep(void) {
  size_t live = 0;
  memset(gc_free, 0, sizeof gc_free);
  for (size_t k = 0; k < gc_nblocks; k++) {
    gc_block *b = gc_blocks[k];
    if (b->cls < 0) continue;
    uint32_t used = 0;
    for (uint32_t j = 0; j < (b->slots + 63) / 64; j++) {
      uint64_t dead = b->alloc[j] & ~b->mark[j];
      if (gc_stress) {
        for (; dead; dead &= dead - 1) memset(b->first + ((size_t)j * 64 + __builtin_ctzll(dead)) * b->size, 0xDD, b->size);
      }
      b->alloc[j] = b->mark[j];
      used += (uint32_t)__builtin_popcountll(b->mark[j]);
      b->mark[j] = 0;
    }
    if (!used) {
      b->cls = -1;
      b->next = gc_pool;
      gc_pool = b;
      continue;
    }
    live += (size_t)used * b->size;
    for (uint32_t i = b->slots; i-- > 0;) {
      if (b->alloc[i >> 6] >> (i & 63) & 1) continue;
      void *p = b->first + (size_t)i * b->size;
      *(void **)p = gc_free[b->raw][b->cls];
      gc_free[b->raw][b->cls] = p;
    }
  }
  size_t n = 0;
  for (size_t k = 0; k < gc_nbigs; k++) {
    gc_big *h = gc_bigs[k];
    if (h->mark) {
      h->mark = 0;
      live += h->size;
      gc_bigs[n++] = h;
    } else {
      gc_big_bytes -= h->size;
      if (gc_stress) memset(h + 1, 0xDD, h->size);
      free(h);
    }
  }
  gc_nbigs = n;
  gc_live = live;
  gc_since = 0;
  gc_next = live > GC_MIN ? live : GC_MIN;
}

// ---- collecting ----

static __attribute__((noinline)) void gc_collect_from(const char *sp) {
  gc_runs++;
  gc_scan(sp, (size_t)(gc_stack_base - sp));
  for (size_t i = 0; i < gc_nroots; i++) gc_scan(gc_roots[i].p, gc_roots[i].n);
  while (gc_nmarks) {
    gc_range r = gc_marks[--gc_nmarks];
    gc_scan(r.p, r.n);
  }
  gc_sweep();
}
static __attribute__((noinline)) void gc_collect(void) {
  // setjmp spills the registers into regs, on the stack, where the scan finds them.
  jmp_buf regs;
  __builtin_unwind_init();
  setjmp(regs);
  gc_collect_from((const char *)&regs);
}

static void *gc_alloc(size_t n, int raw) {
  if (!gc_ready) gc_init();
  if (gc_stack_base && (gc_stress ? ++gc_count % gc_stress == 0 : gc_since >= gc_next)) gc_collect();
  if (n > GC_SMALL) return gc_big_alloc(n, raw);
  int c = gc_cls_of[(n + 15) / 16];
  void *p = gc_free[raw][c];
  if (!p) {
    gc_refill(raw, c);
    p = gc_free[raw][c];
  }
  gc_free[raw][c] = *(void **)p;
  gc_block *b = (gc_block *)((uintptr_t)p & ~(GC_BLOCK - 1));
  size_t i = (size_t)(((uint64_t)((char *)p - b->first) * b->inv) >> 32);
  b->alloc[i >> 6] |= (uint64_t)1 << (i & 63);
  gc_since += b->size;
  if (raw) *(void **)p = NULL;
  else memset(p, 0, b->size);
  return p;
}
// A new object of n bytes, zeroed, which may hold references.
static void *rt_alloc(size_t n) { return gc_alloc(n, 0); }
// A new object of n bytes that holds no references: it is not scanned or zeroed.
static void *rt_alloc_raw(size_t n) { return gc_alloc(n, 1); }
// Registers n bytes at p as a root, scanned in every collection.
static void rt_root(void *p, size_t n) {
  gc_grow(&gc_roots, &gc_caproots, gc_nroots + 1, sizeof *gc_roots);
  gc_roots[gc_nroots++] = (gc_range){ p, n };
}

// A growable byte buffer for building text (malloc'd, freed by the user).
typedef struct { char *p; size_t n, cap; } sb;
static void sb_need(sb *b, size_t more) {
  if (b->n + more + 1 <= b->cap) return;
  size_t cap = b->cap ? b->cap * 2 : 64;
  while (cap < b->n + more + 1) cap *= 2;
  char *p = xmalloc(cap);
  if (b->n) memcpy(p, b->p, b->n);
  free(b->p);
  b->p = p;
  b->cap = cap;
}
static void sb_putn(sb *b, const char *s, size_t n) { sb_need(b, n); memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0; }
static void sb_puts(sb *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_putc(sb *b, char c) { sb_putn(b, &c, 1); }
static void sb_printf(sb *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmp[256];
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) { sb_putn(b, tmp, n); return; }
  sb_need(b, n);
  va_start(ap, fmt);
  vsnprintf(b->p + b->n, n + 1, fmt, ap);
  va_end(ap);
  b->n += n;
}

// ---- strings ----

static void utf8_put(sb *b, uint32_t c) {
  char t[4];
  if (c < 0x80) { t[0] = (char)c; sb_putn(b, t, 1); }
  else if (c < 0x800) { t[0] = 0xC0 | (c >> 6); t[1] = 0x80 | (c & 0x3F); sb_putn(b, t, 2); }
  else if (c < 0x10000) { t[0] = 0xE0 | (c >> 12); t[1] = 0x80 | ((c >> 6) & 0x3F); t[2] = 0x80 | (c & 0x3F); sb_putn(b, t, 3); }
  else { t[0] = 0xF0 | (c >> 18); t[1] = 0x80 | ((c >> 12) & 0x3F); t[2] = 0x80 | ((c >> 6) & 0x3F); t[3] = 0x80 | (c & 0x3F); sb_putn(b, t, 4); }
}
// Decodes the code point at p (valid UTF-8); sets *len to its byte length.
static uint32_t utf8_get(const unsigned char *p, int *len) {
  if (p[0] < 0x80) { *len = 1; return p[0]; }
  if (p[0] < 0xE0) { *len = 2; return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); }
  if (p[0] < 0xF0) { *len = 3; return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
  *len = 4;
  return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

// A new string from n bytes of valid UTF-8.
static const wstr *str_new(const char *p, int64_t n) {
  wstr *s = rt_alloc_raw(sizeof(wstr) + n + 1);
  char *d = (char *)(s + 1);
  memcpy(d, p, n);
  d[n] = 0;
  int64_t cps = 0;
  for (int64_t i = 0; i < n; i++) cps += ((unsigned char)p[i] & 0xC0) != 0x80;
  *s = (wstr){ n, cps, cps == n, 0, d };
  return s;
}
static wv rt_str(const char *p, int64_t n) { return rt_s(str_new(p, n)); }
static wv rt_cstr(const char *p) { return rt_str(p, strlen(p)); }
static wv sb_str(sb *b) { wv v = rt_str(b->p ? b->p : "", b->n); free(b->p); *b = (sb){ 0 }; return v; }

// Decodes bytes as UTF-8, replacing each invalid sequence with U+FFFD by the
// WHATWG maximal-subpart rule (§12.1). A byte-order mark is kept.
static wv rt_decode(const char *src, int64_t n) {
  const unsigned char *p = (const unsigned char *)src;
  int64_t i = 0;
  // Fast path: valid input is copied as it is.
  for (;;) {
    if (i == n) return rt_str(src, n);
    unsigned b = p[i];
    if (b < 0x80) { i++; continue; }
    int need = b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 : b >= 0xF0 && b <= 0xF4 ? 3 : -1;
    if (need < 0 || i + need >= n) break;
    unsigned lo = b == 0xE0 ? 0xA0 : b == 0xF0 ? 0x90 : 0x80, hi = b == 0xED ? 0x9F : b == 0xF4 ? 0x8F : 0xBF;
    if (p[i + 1] < lo || p[i + 1] > hi) break;
    int ok = 1;
    for (int k = 2; k <= need; k++) if ((p[i + k] & 0xC0) != 0x80) ok = 0;
    if (!ok) break;
    i += need + 1;
  }
  sb b = { 0 };
  sb_putn(&b, src, i);
  int needed = 0, seen = 0;
  uint32_t cp = 0;
  unsigned lower = 0x80, upper = 0xBF;
  for (; i < n; i++) {
    unsigned c = p[i];
    if (needed == 0) {
      if (c <= 0x7F) { sb_putc(&b, (char)c); continue; }
      if (c >= 0xC2 && c <= 0xDF) { needed = 1; cp = c & 0x1F; }
      else if (c >= 0xE0 && c <= 0xEF) {
        if (c == 0xE0) lower = 0xA0;
        if (c == 0xED) upper = 0x9F;
        needed = 2;
        cp = c & 0xF;
      } else if (c >= 0xF0 && c <= 0xF4) {
        if (c == 0xF0) lower = 0x90;
        if (c == 0xF4) upper = 0x8F;
        needed = 3;
        cp = c & 7;
      } else utf8_put(&b, 0xFFFD);
      continue;
    }
    if (c < lower || c > upper) {
      cp = needed = seen = 0;
      lower = 0x80;
      upper = 0xBF;
      utf8_put(&b, 0xFFFD);
      i--; // reprocess this byte
      continue;
    }
    lower = 0x80;
    upper = 0xBF;
    cp = (cp << 6) | (c & 0x3F);
    if (++seen == needed) {
      utf8_put(&b, cp);
      cp = needed = seen = 0;
    }
  }
  if (needed) utf8_put(&b, 0xFFFD);
  return sb_str(&b);
}

// Byte offsets of code points, for $at and $slice on non-ASCII strings. Long
// strings are cached so loops over $at stay O(1) per call.
static const wstr *cache_s;
static int64_t *cache_off;
static int64_t cp_offset(const wstr *s, int64_t k) {
  if (s->ascii) return k;
  if (k == s->cps) return s->bytes;
  if (s->cps < 64) {
    int64_t off = 0;
    for (int64_t i = 0; i < k; i++) {
      int len;
      utf8_get((const unsigned char *)s->data + off, &len);
      off += len;
    }
    return off;
  }
  if (s != cache_s) {
    cache_off = rt_alloc_raw(sizeof(int64_t) * s->cps);
    int64_t off = 0;
    for (int64_t i = 0; i < s->cps; i++) {
      int len;
      cache_off[i] = off;
      utf8_get((const unsigned char *)s->data + off, &len);
      off += len;
    }
    cache_s = s;
  }
  return cache_off[k];
}

static int str_eq(const wstr *a, const wstr *b) { return a == b || (a->bytes == b->bytes && memcmp(a->data, b->data, a->bytes) == 0); }
// Code point order is UTF-8 byte order.
static int str_cmp(const wstr *a, const wstr *b) {
  int64_t n = a->bytes < b->bytes ? a->bytes : b->bytes;
  int c = memcmp(a->data, b->data, n);
  if (c) return c < 0 ? -1 : 1;
  return a->bytes < b->bytes ? -1 : a->bytes > b->bytes;
}

// ---- record keys ----
// Field names in a program are static wstrs, registered at startup by
// rt_syms, so a field read compares pointers. A computed key is looked up here.

static const wstr **sym_tab;
static size_t sym_cap, sym_n;
static uint64_t hash_bytes(const char *p, int64_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (int64_t i = 0; i < n; i++) h = (h ^ (unsigned char)p[i]) * 1099511628211ULL;
  return h;
}
static void sym_add(const wstr *s) {
  if ((sym_n + 1) * 2 > sym_cap) {
    size_t cap = sym_cap ? sym_cap * 2 : 256;
    const wstr **t = xmalloc(cap * sizeof *t);
    memset(t, 0, cap * sizeof *t);
    for (size_t i = 0; i < sym_cap; i++) {
      if (!sym_tab[i]) continue;
      size_t j = hash_bytes(sym_tab[i]->data, sym_tab[i]->bytes) & (cap - 1);
      while (t[j]) j = (j + 1) & (cap - 1);
      t[j] = sym_tab[i];
    }
    free(sym_tab);
    sym_tab = t;
    sym_cap = cap;
  }
  size_t j = hash_bytes(s->data, s->bytes) & (sym_cap - 1);
  while (sym_tab[j]) j = (j + 1) & (sym_cap - 1);
  sym_tab[j] = s;
  sym_n++;
}
static void rt_syms(int n, wstr *const *syms) {
  for (int i = 0; i < n; i++) { syms[i]->sym = 1; sym_add(syms[i]); }
}
// The registered key equal to s, or NULL if no record can have that field.
static const wstr *sym_find(const wstr *s) {
  if (s->sym) return s;
  if (!sym_cap) return NULL;
  size_t j = hash_bytes(s->data, s->bytes) & (sym_cap - 1);
  for (; sym_tab[j]; j = (j + 1) & (sym_cap - 1)) if (str_eq(sym_tab[j], s)) return sym_tab[j];
  return NULL;
}

// ---- output ----

#define BROKEN_PIPE 141
static void rt_exit_code(int64_t code);
static char out_buf[1 << 16];
static size_t out_n;

#ifdef _WIN32
static wchar_t *to_wide(const char *s, int64_t n);
static void write_raw(int fd, const char *p, size_t n) {
  HANDLE h = GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
  DWORD mode;
  if (GetConsoleMode(h, &mode)) {
    // A console takes UTF-16; whole strings arrive here, so no sequence is split.
    wchar_t *w = to_wide(p, n);
    DWORD len = (DWORD)wcslen(w), done;
    for (DWORD off = 0; off < len; off += done) {
      if (!WriteConsoleW(h, w + off, len - off, &done, NULL)) break;
    }
    free(w);
    return;
  }
  while (n > 0) {
    DWORD done;
    if (!WriteFile(h, p, n > 0x40000000 ? 0x40000000 : (DWORD)n, &done, NULL)) {
      DWORD e = GetLastError();
      if (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE) { term_restore(); ExitProcess(BROKEN_PIPE); }
      return;
    }
    p += done;
    n -= done;
  }
}
#else
// SIGPIPE stays at its default, so a write to a closed pipe ends the program (§12.2).
static void write_raw(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t k = write(fd, p, n);
    if (k < 0) {
      if (errno == EINTR || errno == EAGAIN) continue;
      if (errno == EPIPE) { term_restore(); _exit(BROKEN_PIPE); }
      return;
    }
    p += k;
    n -= k;
  }
}
#endif

static void rt_flush(void) {
  if (out_n) write_raw(1, out_buf, out_n);
  out_n = 0;
}
// Stdout is buffered; stderr is not, and flushes stdout first to keep order.
static void rt_write(int fd, const char *p, size_t n) {
  if (fd == 2) { rt_flush(); write_raw(2, p, n); return; }
  if (out_n + n > sizeof out_buf) rt_flush();
  if (n > sizeof out_buf) { write_raw(1, p, n); return; }
  memcpy(out_buf + out_n, p, n);
  out_n += n;
}

// ---- function stack, panics, fails (§8) ----

#define RT_DEPTH 100000
static const char *rt_stack[RT_DEPTH];
static int rt_depth;
static int64_t rt_failcode;     // a pending fail's code, or 0
static const char *rt_failfrom; // the function or stdlib function that failed

// Where the running statement is: its file's index in rt_files, shifted left
// by 20, or'ed with its line; 0 before the first. Every statement sets it
// (RT_AT). rt_enter saves the caller's in rt_ats, and RT_RET puts it back, so
// rt_ats[i] is where frame i - 1 (or main, for i = 0) made its call.
#define RT_AT(file, line) (rt_at = ((file) << 20) | (line))
static int32_t rt_at;
static int32_t rt_ats[RT_DEPTH];
static const char *const *rt_files; // the program's source files, set by wvt_main

// One line of a panic's stack trace: "    file:line in name".
static void trace_line(sb *b, const char *name, int32_t at) {
  int line = at & 0xFFFFF;
  if (line && rt_files) sb_printf(b, "    %s:%d in %s\n", rt_files[at >> 20], line, name);
  else sb_printf(b, "    in %s\n", name);
}

static _Noreturn void rt_die(const char *msg, size_t n) {
#ifdef WVT_TEST_DIE
  WVT_TEST_DIE(msg, n); // the runtime tests report a panic and go on
#endif
  sb b = { 0 };
  sb_puts(&b, "uncaught panic: ");
  sb_putn(&b, msg, n);
  sb_putc(&b, '\n');
  // Innermost first; a deep stack shows its 40 innermost frames and its 8 outermost.
  for (int i = rt_depth - 1; i >= 0; i--) {
    if (rt_depth > 48 && i == rt_depth - 41) {
      sb_printf(&b, "    ... %d more\n", rt_depth - 48);
      i = 7;
    }
    trace_line(&b, rt_stack[i], i == rt_depth - 1 ? rt_at : rt_ats[i + 1]);
  }
  trace_line(&b, "main", rt_depth ? rt_ats[0] : rt_at);
  rt_write(2, b.p, b.n);
  rt_exit_code(1);
  for (;;) {}
}
static _Noreturn void rt_fail(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char tmp[512];
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n < (int)sizeof tmp) rt_die(tmp, n);
  char *p = xmalloc(n + 1);
  va_start(ap, fmt);
  vsnprintf(p, n + 1, fmt, ap);
  va_end(ap);
  rt_die(p, n);
}
static _Noreturn void rt_fail_sb(sb *b) { rt_die(b->p ? b->p : "", b->n); }

// Called on entry to every function; RT_RET pops.
static inline void rt_enter(const char *name) {
  if (rt_depth == RT_DEPTH) rt_fail("stack overflow");
  rt_ats[rt_depth] = rt_at;
  rt_stack[rt_depth++] = name;
}
#define RT_RET(x) do { wv r_ = (x); rt_at = rt_ats[--rt_depth]; return r_; } while (0)

static const char *type_name(wv x) {
  switch (x.tag) {
    case T_INT: return "int";
    case T_FLOAT: return "float";
    case T_BOOL: return "boolean";
    case T_STR: return "string";
    case T_NULL: return "null";
    case T_ARR: return "array";
    case T_REC: return "record";
    case T_MEM: return x.m->enum_name->data;
    case T_MAP: return "map";
  }
  return "unknown";
}

// ---- decimal text to double, correctly rounded (§9.5) ----
// strtod gives a first guess (UCRT's is sometimes one ulp off). An exact
// comparison with the midpoints to the neighboring doubles then corrects it,
// with ties going to the even significand.

typedef struct { uint32_t *d; int64_t n, cap; } big; // little-endian 32-bit limbs
static void big_need(big *b, int64_t n) {
  if (n <= b->cap) return;
  int64_t cap = b->cap ? b->cap * 2 : 16;
  while (cap < n) cap *= 2;
  uint32_t *d = xmalloc(sizeof(uint32_t) * cap);
  if (b->n) memcpy(d, b->d, sizeof(uint32_t) * b->n);
  free(b->d);
  b->d = d;
  b->cap = cap;
}
static void big_mul_add(big *b, uint32_t m, uint32_t add) {
  uint64_t carry = add;
  for (int64_t i = 0; i < b->n; i++) {
    uint64_t x = (uint64_t)b->d[i] * m + carry;
    b->d[i] = (uint32_t)x;
    carry = x >> 32;
  }
  if (carry) { big_need(b, b->n + 1); b->d[b->n++] = (uint32_t)carry; }
}
static void big_pow5(big *b, int64_t e) {
  for (; e >= 13; e -= 13) big_mul_add(b, 1220703125u, 0); // 5^13
  uint32_t m = 1;
  while (e-- > 0) m *= 5;
  big_mul_add(b, m, 0);
}
static void big_shl(big *b, int64_t s) {
  if (b->n == 0) return;
  int64_t limbs = s / 32;
  int bits = (int)(s % 32);
  big_need(b, b->n + limbs + 1);
  b->d[b->n + limbs] = 0;
  for (int64_t i = b->n - 1; i >= 0; i--) {
    uint64_t x = (uint64_t)b->d[i] << bits;
    b->d[i + limbs + 1] |= (uint32_t)(x >> 32);
    b->d[i + limbs] = (uint32_t)x;
  }
  for (int64_t i = 0; i < limbs; i++) b->d[i] = 0;
  b->n += limbs + 1;
}
static int big_cmp(big *a, big *b) {
  while (a->n && !a->d[a->n - 1]) a->n--;
  while (b->n && !b->d[b->n - 1]) b->n--;
  if (a->n != b->n) return a->n < b->n ? -1 : 1;
  for (int64_t i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
  return 0;
}
static void big_set(big *b, const big *src) {
  big_need(b, src->n);
  if (src->n) memcpy(b->d, src->d, sizeof(uint32_t) * src->n);
  b->n = src->n;
}

// Compares digits * 10^e10 with the midpoint between the positive doubles with bits k and k + 1.
static int cmp_mid(const big *digits, int64_t e10, uint64_t k) {
  uint64_t frac = k & ((1ULL << 52) - 1);
  int64_t ex = (int64_t)(k >> 52), m, e2;
  if (ex == 0) { m = frac; e2 = -1074; }
  else { m = frac | (1ULL << 52); e2 = ex - 1075; }
  // midpoint = (2m + 1) * 2^(e2 - 1)
  big l = { 0 }, r = { 0 };
  big_set(&l, digits);
  big_need(&r, 2);
  uint64_t mid = 2 * (uint64_t)m + 1;
  r.d[0] = (uint32_t)mid;
  r.d[1] = (uint32_t)(mid >> 32);
  r.n = 2;
  if (e10 >= 0) big_pow5(&l, e10); else big_pow5(&r, -e10);
  int64_t p2 = e10 - (e2 - 1);
  if (p2 >= 0) big_shl(&l, p2); else big_shl(&r, -p2);
  int c = big_cmp(&l, &r);
  free(l.d);
  free(r.d);
  return c;
}

// The double nearest to decimal text matching -?\d+(\.\d+)?([eE][+-]?\d+)?
static double to_double(const char *p, int64_t n) {
  int neg = n > 0 && p[0] == '-';
  big digits = { 0 };
  int64_t i = neg, e10 = 0, nd = 0;
  int point = 0;
  for (; i < n && p[i] != 'e' && p[i] != 'E'; i++) {
    if (p[i] == '.') { point = 1; continue; }
    if (point) e10--;
    if (nd == 0 && p[i] == '0') continue;
    if (nd == 0) { big_need(&digits, 1); digits.d[0] = 0; digits.n = 1; }
    big_mul_add(&digits, 10, p[i] - '0');
    nd++;
  }
  if (i < n) { // exponent, saturated far beyond any double
    int esign = 1;
    int64_t ev = 0;
    i++;
    if (p[i] == '+' || p[i] == '-') esign = p[i++] == '-' ? -1 : 1;
    for (; i < n; i++) if (ev < 1000000000) ev = ev * 10 + (p[i] - '0');
    e10 += esign * ev;
  }
  double v;
  if (nd == 0 || nd + e10 < -324) v = 0; // below half the smallest subnormal
  else if (nd + e10 > 310) v = INFINITY;
  else {
    char tmp[64], *t = n < 64 ? tmp : xmalloc(n + 1);
    memcpy(t, p + neg, n - neg);
    t[n - neg] = 0;
    double g = fabs(strtod(t, NULL));
    if (t != tmp) free(t);
    const uint64_t INF = 0x7ff0000000000000ULL;
    uint64_t k;
    memcpy(&k, &g, 8);
    for (;;) {
      int c;
      if (k < INF && ((c = cmp_mid(&digits, e10, k)) > 0 || (c == 0 && (k & 1)))) { k++; continue; }
      if (k > 0 && ((c = cmp_mid(&digits, e10, k - 1)) < 0 || (c == 0 && (k & 1)))) { k--; continue; }
      break;
    }
    memcpy(&v, &k, 8);
  }
  free(digits.d);
  return neg ? -v : v;
}

// ---- text (§9.3) ----

// The shortest digits that read back as v (v finite, > 0): v = 0.d1...dk * 10^*n.
static int shortest(double v, char *digits, int *n) {
  char buf[40];
  for (int p = 1; p <= 17; p++) {
    snprintf(buf, sizeof buf, "%.*e", p - 1, v);
    char *e = strchr(buf, 'e');
    int exp = atoi(e + 1);
    uint64_t d = 0;
    for (char *q = buf; q < e; q++) if (*q != '.') d = d * 10 + (*q - '0');
    // The correctly rounded p digits first; at a power of two the round-trip
    // interval is lopsided, so a neighbor of the same length may work instead.
    uint64_t cand[3] = { d, d + 1, d - 1 };
    for (int c = 0; c < 3; c++) {
      if (cand[c] == 0) continue;
      char t[48];
      snprintf(t, sizeof t, "%llue%d", (unsigned long long)cand[c], exp - (p - 1));
      if (to_double(t, strlen(t)) != v) continue;
      int k = snprintf(digits, 24, "%llu", (unsigned long long)cand[c]);
      *n = k + exp - (p - 1);
      while (k > 1 && digits[k - 1] == '0') digits[--k] = 0;
      return k;
    }
  }
  return 0; // unreachable: 17 digits always round-trip
}
static void fmt_float(sb *b, double v) {
  if (isnan(v)) { sb_puts(b, "nan"); return; }
  if (isinf(v)) { sb_puts(b, v > 0 ? "inf" : "-inf"); return; }
  if (v == 0) { sb_puts(b, signbit(v) ? "-0.0" : "0.0"); return; }
  if (v < 0) { sb_putc(b, '-'); v = -v; }
  char d[24];
  int n, k = shortest(v, d, &n);
  if (k <= n && n <= 21) {
    sb_puts(b, d);
    for (int i = k; i < n; i++) sb_putc(b, '0');
    sb_puts(b, ".0");
  } else if (0 < n && n <= 21) {
    sb_putn(b, d, n);
    sb_putc(b, '.');
    sb_puts(b, d + n);
  } else if (-6 < n && n <= 0) {
    sb_puts(b, "0.");
    for (int i = n; i < 0; i++) sb_putc(b, '0');
    sb_puts(b, d);
  } else {
    sb_putc(b, d[0]);
    sb_putc(b, '.');
    sb_puts(b, k > 1 ? d + 1 : "0");
    sb_printf(b, "e%c%d", n - 1 < 0 ? '-' : '+', abs(n - 1));
  }
}

// Appends v's interpolation text; returns 0 if it has none.
static int text_to(sb *b, wv v) {
  switch (v.tag) {
    case T_INT: sb_printf(b, "%lld", (long long)v.i); return 1;
    case T_FLOAT: fmt_float(b, v.f); return 1;
    case T_STR: sb_putn(b, v.s->data, v.s->bytes); return 1;
    case T_BOOL: sb_puts(b, v.b ? "true" : "false"); return 1;
    case T_NULL: sb_puts(b, "null"); return 1;
    case T_MEM: sb_putn(b, v.m->name->data, v.m->name->bytes); return 1;
  }
  return 0;
}
static _Noreturn void no_text(wv v) { rt_fail("type error: cannot convert %s to text; convert it explicitly", type_name(v)); }
static wv rt_text(wv v) {
  if (v.tag == T_STR) return v;
  sb b = { 0 };
  if (!text_to(&b, v)) no_text(v);
  return sb_str(&b);
}
// Joins n strings (interpolation).
static wv rt_cat(int n, const wv *parts) {
  int64_t len = 0;
  for (int i = 0; i < n; i++) len += parts[i].s->bytes;
  wstr *s = rt_alloc_raw(sizeof(wstr) + len + 1);
  char *d = (char *)(s + 1);
  int64_t cps = 0, off = 0;
  for (int i = 0; i < n; i++) {
    memcpy(d + off, parts[i].s->data, parts[i].s->bytes);
    off += parts[i].s->bytes;
    cps += parts[i].s->cps;
  }
  d[len] = 0;
  *s = (wstr){ len, cps, cps == len, 0, d };
  return rt_s(s);
}

static void show_to(sb *b, wv v) {
  switch (v.tag) {
    case T_STR: {
      sb_putc(b, '"');
      for (int64_t i = 0; i < v.s->bytes; i++) {
        unsigned char c = v.s->data[i];
        switch (c) {
          case '"': sb_puts(b, "\\\""); break;
          case '\\': sb_puts(b, "\\\\"); break;
          case '\b': sb_puts(b, "\\b"); break;
          case '\f': sb_puts(b, "\\f"); break;
          case '\n': sb_puts(b, "\\n"); break;
          case '\r': sb_puts(b, "\\r"); break;
          case '\t': sb_puts(b, "\\t"); break;
          default:
            if (c < 0x20) sb_printf(b, "\\u%04x", c);
            else sb_putc(b, (char)c);
        }
      }
      sb_putc(b, '"');
      return;
    }
    case T_ARR:
      sb_putc(b, '[');
      for (int64_t i = 0; i < v.a->len; i++) {
        if (i) sb_puts(b, ", ");
        show_to(b, v.a->items[i]);
      }
      sb_putc(b, ']');
      return;
    case T_REC:
      sb_putc(b, '{');
      for (int64_t i = 0; i < v.r->n; i++) {
        if (i) sb_puts(b, ", ");
        sb_putn(b, v.r->keys[i]->data, v.r->keys[i]->bytes);
        sb_puts(b, ": ");
        show_to(b, v.r->vals[i]);
      }
      sb_putc(b, '}');
      return;
    case T_MAP: {
      sb_putc(b, '[');
      int first = 1;
      for (int64_t i = 0; i < v.mp->len; i++) {
        if (!v.mp->e[i].live) continue;
        if (!first) sb_puts(b, ", ");
        first = 0;
        show_to(b, v.mp->e[i].key);
        sb_puts(b, ": ");
        show_to(b, v.mp->e[i].val);
      }
      sb_puts(b, first ? ":]" : "]");
      return;
    }
  }
  if (!text_to(b, v)) sb_puts(b, "unknown");
}

// ---- arithmetic (§11.2) ----

static wv rt_int(int64_t r, const char *op) {
  if (r > RT_MAX || r < -RT_MAX) rt_fail("integer overflow in '%s'", op);
  return rt_i(r);
}
static _Noreturn void bad2(const char *op, wv a, wv b) {
  const char *hint = op[0] == '+' && !op[1] && (a.tag == T_STR || b.tag == T_STR) ? " (use interpolation: \"{x}\")" : "";
  rt_fail("type error: '%s' cannot be applied to %s and %s%s", op, type_name(a), type_name(b), hint);
}
static _Noreturn void bad1(const char *op, wv a) { rt_fail("type error: '%s' cannot be applied to %s", op, type_name(a)); }

static wv rt_add(wv a, wv b) {
  if (a.tag == T_INT && b.tag == T_INT) return rt_int(a.i + b.i, "+");
  if (a.tag == T_STR && b.tag == T_STR) return rt_cat(2, (wv[]){ a, b });
  if (isnum(a) && isnum(b)) return rt_f(nv(a) + nv(b));
  bad2("+", a, b);
}
static wv rt_sub(wv a, wv b) {
  if (a.tag == T_INT && b.tag == T_INT) return rt_int(a.i - b.i, "-");
  if (isnum(a) && isnum(b)) return rt_f(nv(a) - nv(b));
  bad2("-", a, b);
}
static wv rt_mul(wv a, wv b) {
  if (a.tag == T_INT && b.tag == T_INT) {
    int64_t r;
    if (__builtin_mul_overflow(a.i, b.i, &r)) rt_fail("integer overflow in '*'");
    return rt_int(r, "*");
  }
  if (isnum(a) && isnum(b)) return rt_f(nv(a) * nv(b));
  bad2("*", a, b);
}
static wv rt_div(wv a, wv b) {
  if (isnum(a) && isnum(b)) return rt_f(nv(a) / nv(b));
  bad2("/", a, b);
}
// Floored modulo: the result takes the divisor's sign.
static double fmod_floor(double x, double y) {
  double r = fmod(x, y);
  return r != 0 && (r < 0) != (y < 0) ? r + y : r;
}
static wv rt_mod(wv a, wv b) {
  if (a.tag == T_INT && b.tag == T_INT) {
    if (b.i == 0) rt_fail("integer modulo by zero");
    int64_t r = a.i % b.i;
    return rt_i(r != 0 && (r < 0) != (b.i < 0) ? r + b.i : r);
  }
  if (isnum(a) && isnum(b)) return rt_f(fmod_floor(nv(a), nv(b)));
  bad2("%", a, b);
}
// ** on floats: fdlibm's __ieee754_pow, which V8 uses for Math.pow and **,
// so results match JavaScript's bit for bit (§11.2). It also gives JavaScript's NaN results
// for 1 ** nan and 1 ** inf, where C's pow returns 1.
static inline int32_t dhi(double x) { uint64_t b; memcpy(&b, &x, 8); return (int32_t)(b >> 32); }
static inline uint32_t dlo(double x) { uint64_t b; memcpy(&b, &x, 8); return (uint32_t)b; }
static inline double dset(int32_t hi, uint32_t lo) {
  uint64_t b = (uint64_t)(uint32_t)hi << 32 | lo;
  double x;
  memcpy(&x, &b, 8);
  return x;
}
static double js_pow(double x, double y) {
  static const double bp[] = { 1.0, 1.5 }, dp_h[] = { 0.0, 5.84962487220764160156e-01 },
    dp_l[] = { 0.0, 1.35003920212974897128e-08 }, two53 = 9007199254740992.0, huge = 1.0e300, tiny = 1.0e-300,
    L1 = 5.99999999999994648725e-01, L2 = 4.28571428578550184252e-01, L3 = 3.33333329818377432918e-01,
    L4 = 2.72728123808534006489e-01, L5 = 2.30660745775561754067e-01, L6 = 2.06975017800338417784e-01,
    P1 = 1.66666666666666019037e-01, P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
    P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08, lg2 = 6.93147180559945286227e-01,
    lg2_h = 6.93147182464599609375e-01, lg2_l = -1.90465429995776804525e-09, ovt = 8.0085662595372944372e-17,
    cp = 9.61796693925975554329e-01, cp_h = 9.61796700954437255859e-01, cp_l = -7.02846165095275826516e-09,
    ivln2 = 1.44269504088896338700e+00, ivln2_h = 1.44269502162933349609e+00, ivln2_l = 1.92596299112661746887e-08;
  double z, ax, z_h, z_l, p_h, p_l, y1, t1, t2, r, s, t, u, v, w;
  int32_t i, j, k, yisint, n, hx, hy, ix, iy;
  uint32_t lx, ly;

  hx = dhi(x); lx = dlo(x);
  hy = dhi(y); ly = dlo(y);
  ix = hx & 0x7fffffff; iy = hy & 0x7fffffff;

  if ((iy | ly) == 0) return 1.0; // x ** 0 = 1
  if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lx != 0) || iy > 0x7ff00000 || (iy == 0x7ff00000 && ly != 0)) return x + y;

  // yisint: 0 if y is not an integer, 1 if odd, 2 if even (only when x < 0).
  yisint = 0;
  if (hx < 0) {
    if (iy >= 0x43400000) yisint = 2;
    else if (iy >= 0x3ff00000) {
      k = (iy >> 20) - 0x3ff;
      if (k > 20) {
        uint32_t jj = ly >> (52 - k);
        if ((jj << (52 - k)) == ly) yisint = 2 - (jj & 1);
      } else if (ly == 0) {
        j = iy >> (20 - k);
        if ((j << (20 - k)) == iy) yisint = 2 - (j & 1);
      }
    }
  }

  if (ly == 0) {
    if (iy == 0x7ff00000) { // y is ±inf
      if (((ix - 0x3ff00000) | lx) == 0) return y - y; // ±1 ** ±inf is NaN
      if (ix >= 0x3ff00000) return hy >= 0 ? y : 0.0;
      return hy < 0 ? -y : 0.0;
    }
    if (iy == 0x3ff00000) return hy < 0 ? 1.0 / x : x;
    if (hy == 0x40000000) return x * x;
    if (hy == 0x3fe00000 && hx >= 0) return sqrt(x);
  }

  ax = fabs(x);
  if (lx == 0 && (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000)) { // x is ±0, ±inf, ±1
    z = ax;
    if (hy < 0) z = 1.0 / z;
    if (hx < 0) {
      if (((ix - 0x3ff00000) | yisint) == 0) z = (z - z) / (z - z); // (-1) ** non-int is NaN
      else if (yisint == 1) z = -z;
    }
    return z;
  }

  n = hx < 0 ? 0 : 1;
  if ((n | yisint) == 0) return (x - x) / (x - x); // (x < 0) ** non-int is NaN

  s = 1.0;
  if ((n | (yisint - 1)) == 0) s = -1.0; // (x < 0) ** odd int

  if (iy > 0x41e00000) { // |y| > 2^31
    if (iy > 0x43f00000) { // |y| > 2^64: must overflow or underflow
      if (ix <= 0x3fefffff) return hy < 0 ? huge * huge : tiny * tiny;
      if (ix >= 0x3ff00000) return hy > 0 ? huge * huge : tiny * tiny;
    }
    if (ix < 0x3fefffff) return hy < 0 ? s * huge * huge : s * tiny * tiny;
    if (ix > 0x3ff00000) return hy > 0 ? s * huge * huge : s * tiny * tiny;
    // |1 - x| <= 2^-20: log(x) by x - x^2/2 + x^3/3 - x^4/4
    t = ax - 1.0;
    w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
    u = ivln2_h * t;
    v = t * ivln2_l - w * ivln2;
    t1 = dset(dhi(u + v), 0);
    t2 = v - (t1 - u);
  } else {
    double ss, s2, s_h, s_l, t_h, t_l;
    n = 0;
    if (ix < 0x00100000) { ax *= two53; n -= 53; ix = dhi(ax); } // subnormal x
    n += (ix >> 20) - 0x3ff;
    j = ix & 0x000fffff;
    ix = j | 0x3ff00000;
    if (j <= 0x3988E) k = 0;      // |x| < sqrt(3/2)
    else if (j < 0xBB67A) k = 1;  // |x| < sqrt(3)
    else { k = 0; n += 1; ix -= 0x00100000; }
    ax = dset(ix, dlo(ax));

    // ss = s_h + s_l = (x - 1) / (x + 1) or (x - 1.5) / (x + 1.5)
    u = ax - bp[k];
    v = 1.0 / (ax + bp[k]);
    ss = u * v;
    s_h = dset(dhi(ss), 0);
    t_h = dset(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18), 0);
    t_l = ax - (t_h - bp[k]);
    s_l = v * ((u - s_h * t_h) - s_h * t_l);
    // log(ax)
    s2 = ss * ss;
    r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
    r += s_l * (s_h + ss);
    s2 = s_h * s_h;
    t_h = dset(dhi(3.0 + s2 + r), 0);
    t_l = r - ((t_h - 3.0) - s2);
    u = s_h * t_h;
    v = s_l * t_h + t_l * ss;
    p_h = dset(dhi(u + v), 0);
    p_l = v - (p_h - u);
    z_h = cp_h * p_h;
    z_l = cp_l * p_h + p_l * cp + dp_l[k];
    // log2(ax) = n + dp_h + z_h + z_l
    t = (double)n;
    t1 = dset(dhi(((z_h + z_l) + dp_h[k]) + t), 0);
    t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
  }

  // (y1 + y2) * (t1 + t2)
  y1 = dset(dhi(y), 0);
  p_l = (y - y1) * t1 + y * t2;
  p_h = y1 * t1;
  z = p_l + p_h;
  j = dhi(z);
  i = (int32_t)dlo(z);
  if (j >= 0x40900000) { // z >= 1024
    if (((j - 0x40900000) | i) != 0) return s * huge * huge;
    if (p_l + ovt > z - p_h) return s * huge * huge;
  } else if ((j & 0x7fffffff) >= 0x4090cc00) { // z <= -1075
    if (((uint32_t)j - 0xc090cc00u | (uint32_t)i) != 0) return s * tiny * tiny;
    if (p_l <= z - p_h) return s * tiny * tiny;
  }

  // 2 ** (p_h + p_l)
  i = j & 0x7fffffff;
  k = (i >> 20) - 0x3ff;
  n = 0;
  if (i > 0x3fe00000) { // |z| > 0.5: n = [z + 0.5]
    n = j + (0x00100000 >> (k + 1));
    k = ((n & 0x7fffffff) >> 20) - 0x3ff;
    t = dset(n & ~(0x000fffff >> k), 0);
    n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
    if (j < 0) n = -n;
    p_h -= t;
  }
  t = dset(dhi(p_l + p_h), 0);
  u = t * lg2_h;
  v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
  z = u + v;
  w = v - (z - u);
  t = z * z;
  t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
  r = (z * t1) / (t1 - 2.0) - (w + z * w);
  z = 1.0 - (r - z);
  j = (int32_t)((uint32_t)dhi(z) + ((uint32_t)n << 20));
  if ((j >> 20) <= 0) z = scalbn(z, n); // subnormal result
  else z = dset(j, dlo(z));
  return s * z;
}
static wv rt_pow(wv a, wv b) {
  if (a.tag == T_INT && b.tag == T_INT) {
    if (b.i < 0) return rt_f(js_pow((double)a.i, (double)b.i));
    int64_t r = 1, base = a.i, e = b.i;
    while (e > 0) {
      if (e % 2 == 1) {
        if (__builtin_mul_overflow(r, base, &r)) rt_fail("integer overflow in '**'");
        rt_int(r, "**");
      }
      e /= 2;
      if (e > 0) {
        if (__builtin_mul_overflow(base, base, &base)) rt_fail("integer overflow in '**'");
        rt_int(base, "**");
      }
    }
    return rt_i(r);
  }
  if (isnum(a) && isnum(b)) return rt_f(js_pow(nv(a), nv(b)));
  bad2("**", a, b);
}
static wv rt_neg(wv a) {
  if (a.tag == T_INT) return rt_i(-a.i);
  if (a.tag == T_FLOAT) return rt_f(-a.f);
  bad1("-", a);
}

// ---- comparison (§11.1) ----

static int eq(wv a, wv b) {
  if (isnum(a)) return isnum(b) && nv(a) == nv(b);
  if (isnum(b) || a.tag != b.tag) return 0;
  switch (a.tag) {
    case T_NULL: return 1;
    case T_BOOL: return a.b == b.b;
    case T_STR: return str_eq(a.s, b.s);
    case T_ARR: return a.a == b.a;
    case T_REC: return a.r == b.r;
    case T_MEM: return a.m == b.m;
    case T_MAP: return a.mp == b.mp;
  }
  return 0;
}
static wv rt_eq(wv a, wv b) { return rt_b(eq(a, b)); }
static wv rt_ne(wv a, wv b) { return rt_b(!eq(a, b)); }

// Orders a and b: -1, 0 or 1; 2 when unordered (NaN or different types).
static int order(wv a, wv b, const char *op) {
  if (a.tag == T_INT && b.tag == T_INT) return a.i < b.i ? -1 : a.i > b.i;
  if (isnum(a) && isnum(b)) {
    double x = nv(a), y = nv(b);
    return x < y ? -1 : x > y ? 1 : x == y ? 0 : 2;
  }
  if (a.tag == T_STR && b.tag == T_STR) return str_cmp(a.s, b.s);
  if (a.tag == T_MEM && b.tag == T_MEM) rt_fail("type error: '%s' cannot order enum members", op);
  if (a.tag == b.tag) rt_fail("type error: '%s' cannot order %s values", op, type_name(a));
  return 2;
}
static wv rt_lt(wv a, wv b) { return rt_b(order(a, b, "<") == -1); }
static wv rt_le(wv a, wv b) { int c = order(a, b, "<="); return rt_b(c == -1 || c == 0); }
static wv rt_gt(wv a, wv b) { return rt_b(order(a, b, ">") == 1); }
static wv rt_ge(wv a, wv b) { int c = order(a, b, ">="); return rt_b(c == 1 || c == 0); }

// ---- bitwise: integers in int32 range ----

static int32_t i32(wv x, const char *op) {
  if (x.tag != T_INT) rt_fail("type error: '%s' requires integers, got %s", op, type_name(x));
  if (x.i < INT32_MIN || x.i > INT32_MAX) rt_fail("'%s' operand %lld is outside the int32 range", op, (long long)x.i);
  return (int32_t)x.i;
}
static wv rt_band(wv a, wv b) { int32_t x = i32(a, "&"); return rt_i(x & i32(b, "&")); }
static wv rt_bor(wv a, wv b) { int32_t x = i32(a, "|"); return rt_i(x | i32(b, "|")); }
static wv rt_bxor(wv a, wv b) { int32_t x = i32(a, "^"); return rt_i(x ^ i32(b, "^")); }
static wv rt_bnot(wv a) { return rt_i(~i32(a, "~")); }
static wv shift(wv a, wv b, const char *op) {
  int32_t x = i32(a, op);
  if (b.tag != T_INT) rt_fail("type error: '%s' requires integers, got %s", op, type_name(b));
  if (b.i < 0 || b.i > 31) rt_fail("shift amount %lld is outside 0-31", (long long)b.i);
  return rt_i(op[0] == '<' ? (int32_t)((uint32_t)x << b.i) : x >> b.i);
}
static wv rt_shl(wv a, wv b) { return shift(a, b, "<<"); }
static wv rt_shr(wv a, wv b) { return shift(a, b, ">>"); }

// ---- booleans ----

static int rt_bool(wv x, const char *op) {
  if (x.tag != T_BOOL) rt_fail("type error: '%s' requires true or false, got %s", op, type_name(x));
  return x.b;
}
static wv rt_not(wv x) { return rt_b(!rt_bool(x, "!")); }
static int rt_cond(wv x) {
  if (x.tag != T_BOOL) rt_fail("condition must be true or false, got %s", type_name(x));
  return x.b;
}

// ---- enums (§4.1, §7.5) ----

static _Noreturn void rt_no_case(wv v) {
  sb b = { 0 };
  sb_puts(&b, "match: no case for ");
  show_to(&b, v);
  rt_fail_sb(&b);
}

// ---- arrays (§10) ----

static warr *arr_new(int64_t n, int is_mut) {
  warr *a = rt_alloc(sizeof(warr));
  *a = (warr){ n, n, is_mut, n ? rt_alloc(sizeof(wv) * n) : NULL };
  return a;
}
// An array literal: n values, frozen unless mutable (var).
static wv rt_arr(int64_t n, const wv *items, int is_mut) {
  warr *a = arr_new(n, is_mut);
  if (n) memcpy(a->items, items, sizeof(wv) * n);
  return (wv){ .tag = T_ARR, .a = a };
}
static wv arr_copy(const warr *src, int64_t from, int64_t to, int is_mut) {
  return rt_arr(to - from, src->items + from, is_mut);
}

static wv rec_copy(const wrec *src, int is_mut);
static wv map_copy(const wmap *src, int is_mut);
// Copy happens only when mutability changes (§10.4, §10.9, §10.11).
static wv rt_bind(wv v, int is_mut) {
  if (v.tag == T_ARR) return v.a->is_mut == is_mut ? v : arr_copy(v.a, 0, v.a->len, is_mut);
  if (v.tag == T_REC) return v.r->is_mut == is_mut ? v : rec_copy(v.r, is_mut);
  if (v.tag == T_MAP) return v.mp->is_mut == is_mut ? v : map_copy(v.mp, is_mut);
  return v;
}

static int64_t slot(wv i, wv arr) {
  if (arr.tag != T_ARR) {
    rt_fail("type error: indexing needs an array, a map or a record, got %s%s", type_name(arr),
      arr.tag == T_STR ? " (use $at for a string)" : "");
  }
  if (i.tag != T_INT) rt_fail("type error: an index must be an int, got %s", type_name(i));
  int64_t n = arr.a->len;
  if (i.i < -n || i.i >= n) rt_fail("index %lld is out of range for an array of length %lld", (long long)i.i, (long long)n);
  return i.i < 0 ? i.i + n : i.i;
}
static void mutable(const warr *a, const char *what) {
  if (!a->is_mut) rt_fail("cannot modify a frozen array%s%s", what ? " with " : "", what ? what : "");
}

// ---- records (§10.7-§10.10) ----

// A record literal: keys is shared by every record of the literal.
static wv rt_rec(int is_mut, int64_t n, const wstr *const *keys, const wv *vals) {
  wrec *r = rt_alloc(sizeof(wrec) + sizeof(wv) * n);
  *r = (wrec){ n, is_mut, keys, (wv *)(r + 1) };
  if (n) memcpy(r->vals, vals, sizeof(wv) * n);
  return (wv){ .tag = T_REC, .r = r };
}
static wv rec_copy(const wrec *src, int is_mut) { return rt_rec(is_mut, src->n, src->keys, src->vals); }
// Describes x for an error message: its type, and a record's field names,
// "record { x, y }", so that a wrong record is told apart from the right one.
static void describe(sb *b, wv x) {
  sb_puts(b, type_name(x));
  if (x.tag != T_REC) return;
  if (x.r->n == 0) { sb_puts(b, " {}"); return; }
  sb_puts(b, " { ");
  for (int64_t i = 0; i < x.r->n; i++) {
    if (i == 12) { sb_printf(b, ", ... %lld more", (long long)(x.r->n - 12)); break; }
    if (i) sb_puts(b, ", ");
    sb_putn(b, x.r->keys[i]->data, x.r->keys[i]->bytes);
  }
  sb_puts(b, " }");
}
// The slot of field key in r; key is registered, or NULL for a key no record has.
static wv *field(const wstr *key, const wstr *name, wv r) {
  if (r.tag != T_REC) {
    sb b = { 0 };
    sb_puts(&b, "type error: reading field '");
    sb_putn(&b, name->data, name->bytes);
    sb_puts(&b, "' needs a record, got ");
    describe(&b, r);
    rt_fail_sb(&b);
  }
  if (key) for (int64_t i = 0; i < r.r->n; i++) if (r.r->keys[i] == key) return &r.r->vals[i];
  sb b = { 0 };
  sb_puts(&b, "no field '");
  sb_putn(&b, name->data, name->bytes);
  sb_puts(&b, "' in ");
  describe(&b, r);
  rt_fail_sb(&b);
}
static wv rt_get(const wstr *key, wv r) { return *field(key, key, r); }
static void writable(const wstr *name, wv r) {
  if (r.r->is_mut) return;
  sb b = { 0 };
  sb_puts(&b, "cannot set field '");
  sb_putn(&b, name->data, name->bytes);
  sb_puts(&b, "' of a frozen ");
  describe(&b, r);
  rt_fail_sb(&b);
}
static void rt_set_mem(wv v, const wstr *key, wv r) {
  wv *p = field(key, key, r);
  writable(key, r);
  *p = v;
}
static void rt_upd_mem(wv v, const wstr *key, wv r, wv (*op)(wv, wv)) {
  wv *p = field(key, key, r);
  writable(key, r);
  *p = op(*p, v);
}
// Indexing record r with k, which names a field. A non-string k is most often
// a map key given to a record, so the message shows both.
static wv *computed(wv k, wv r) {
  if (k.tag != T_STR) {
    sb b = { 0 };
    sb_puts(&b, "type error: a record's field name must be a string, got ");
    describe(&b, k);
    sb_puts(&b, ", indexing ");
    describe(&b, r);
    rt_fail_sb(&b);
  }
  return field(sym_find(k.s), k.s, r);
}

// ---- maps (§10.11) ----

// A key is a scalar (an int, a string, a boolean or an enum member), or a flat
// record: one whose fields all hold scalars.
static int scalar_key(wv k) { return k.tag == T_INT || k.tag == T_STR || k.tag == T_BOOL || k.tag == T_MEM; }
static void key_ok(wv k) {
  if (k.tag == T_REC) {
    for (int64_t i = 0; i < k.r->n; i++) {
      if (scalar_key(k.r->vals[i])) continue;
      sb b = { 0 };
      sb_puts(&b, "type error: field '");
      sb_putn(&b, k.r->keys[i]->data, k.r->keys[i]->bytes);
      sb_puts(&b, "' of a map key must be an int, a string, a boolean or an enum member, got ");
      sb_puts(&b, type_name(k.r->vals[i]));
      rt_fail_sb(&b);
    }
    return;
  }
  if (!scalar_key(k)) {
    rt_fail("type error: a map key must be an int, a string, a boolean, an enum member or a record, got %s", type_name(k));
  }
}
static uint64_t mix64(uint64_t h) {
  h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL; // splitmix64's finalizer
  h ^= h >> 27; h *= 0x94D049BB133111EBULL;
  return h ^ (h >> 31);
}
static uint64_t key_hash(wv k) {
  switch (k.tag) {
    case T_STR: return hash_bytes(k.s->data, k.s->bytes);
    case T_INT: return mix64((uint64_t)k.i);
    case T_BOOL: return mix64((uint64_t)k.b + 0x51ED27);
    case T_REC: {
      // Field order is ignored (§10.11), so the fields' hashes are summed.
      // Field names are interned, so a name is hashed by its address.
      uint64_t h = (uint64_t)k.r->n;
      for (int64_t i = 0; i < k.r->n; i++) h += mix64((uint64_t)(uintptr_t)k.r->keys[i] ^ key_hash(k.r->vals[i]));
      return mix64(h);
    }
    default: return mix64((uint64_t)(uintptr_t)k.m);
  }
}
static int key_eq(wv a, wv b) {
  if (a.tag != b.tag) return 0;
  switch (a.tag) {
    case T_INT: return a.i == b.i;
    case T_STR: return str_eq(a.s, b.s);
    case T_BOOL: return a.b == b.b;
    case T_REC:
      // The same field names, in any order, with equal values: $same (§10.10).
      if (a.r == b.r) return 1;
      if (a.r->n != b.r->n) return 0;
      for (int64_t i = 0; i < a.r->n; i++) {
        int64_t j = 0;
        while (j < b.r->n && b.r->keys[j] != a.r->keys[i]) j++;
        if (j == b.r->n || !key_eq(a.r->vals[i], b.r->vals[j])) return 0;
      }
      return 1;
    default: return a.m == b.m;
  }
}

static wmap *map_new(int is_mut) {
  wmap *m = rt_alloc(sizeof(wmap));
  *m = (wmap){ 0, 0, 0, 0, is_mut, NULL, NULL };
  return m;
}
// The entry holding key k, or -1.
static int64_t map_find(const wmap *m, wv k) {
  if (!m->icap) return -1;
  uint64_t h = key_hash(k);
  for (int64_t j = h & (m->icap - 1);; j = (j + 1) & (m->icap - 1)) {
    int64_t e = m->idx[j];
    if (e < 0) return -1;
    if (m->e[e].live && key_eq(m->e[e].key, k)) return e;
  }
}
// Drops removed entries and makes room for at least cap entries.
static void map_rehash(wmap *m, int64_t cap) {
  if (cap < 8) cap = 8;
  wment *e = rt_alloc(sizeof(wment) * cap);
  int64_t len = 0;
  for (int64_t i = 0; i < m->len; i++) if (m->e[i].live) e[len++] = m->e[i];
  int64_t icap = 16;
  while (icap < cap * 2) icap *= 2;
  int64_t *idx = rt_alloc_raw(sizeof(int64_t) * icap);
  memset(idx, 0xFF, sizeof(int64_t) * icap);
  for (int64_t i = 0; i < len; i++) {
    int64_t j = key_hash(e[i].key) & (icap - 1);
    while (idx[j] >= 0) j = (j + 1) & (icap - 1);
    idx[j] = i;
  }
  *m = (wmap){ len, len, cap, icap, m->is_mut, e, idx };
}
// Sets key k to v, adding it at the end if it is new.
static void map_put(wmap *m, wv k, wv v) {
  int64_t at = map_find(m, k);
  if (at >= 0) { m->e[at].val = v; return; }
  // A mutable record key is snapshotted, so that changing the record later
  // cannot change the key; shallow is enough, since its fields are scalars.
  if (k.tag == T_REC && k.r->is_mut) k = rec_copy(k.r, 0);
  if (m->len == m->cap) map_rehash(m, m->n * 2 > m->cap ? m->cap * 2 : m->cap);
  int64_t j = key_hash(k) & (m->icap - 1);
  while (m->idx[j] >= 0) j = (j + 1) & (m->icap - 1);
  m->idx[j] = m->len;
  m->e[m->len++] = (wment){ k, v, 1 };
  m->n++;
}
// A map literal: n keys and values in turn, frozen unless mutable (var). A key
// given twice keeps its first place and its last value.
static wv rt_map(int64_t n, const wv *kv, int is_mut) {
  wmap *m = map_new(is_mut);
  if (n) map_rehash(m, n);
  for (int64_t i = 0; i < n; i++) {
    key_ok(kv[2 * i]);
    map_put(m, kv[2 * i], kv[2 * i + 1]);
  }
  return (wv){ .tag = T_MAP, .mp = m };
}
static wv map_copy(const wmap *src, int is_mut) {
  wmap *m = map_new(is_mut);
  m->e = src->e;
  m->len = src->len;
  m->n = src->n;
  map_rehash(m, src->n); // copies the live entries into new storage
  return (wv){ .tag = T_MAP, .mp = m };
}
static void map_writable(const wmap *m, const char *what) {
  if (!m->is_mut) rt_fail("cannot modify a frozen map%s%s", what ? " with " : "", what ? what : "");
}
static _Noreturn void no_key(wv k) {
  sb b = { 0 };
  sb_puts(&b, "no key ");
  show_to(&b, k);
  sb_puts(&b, " in map");
  rt_fail_sb(&b);
}
// The entry holding k, which must be there.
static wment *map_entry(wv k, wv m) {
  key_ok(k);
  int64_t at = map_find(m.mp, k);
  if (at < 0) no_key(k);
  return &m.mp->e[at];
}

// Indexing a record is computed field access with a string key (§10.8).
static wv rt_idx(wv i, wv arr) {
  if (arr.tag == T_MAP) return map_entry(i, arr)->val;
  if (arr.tag == T_REC) return *computed(i, arr);
  int64_t k = slot(i, arr); // checks arr before it is read
  return arr.a->items[k];
}
static void rt_set_idx(wv v, wv i, wv arr) {
  if (arr.tag == T_MAP) {
    key_ok(i);
    map_writable(arr.mp, NULL);
    map_put(arr.mp, i, v);
    return;
  }
  if (arr.tag == T_REC) {
    wv *p = computed(i, arr);
    writable(i.s, arr); // computed checked that i is a string
    *p = v;
    return;
  }
  int64_t k = slot(i, arr);
  mutable(arr.a, NULL);
  arr.a->items[k] = v;
}
static void rt_upd_idx(wv v, wv i, wv arr, wv (*op)(wv, wv)) {
  if (arr.tag == T_MAP) {
    wment *e = map_entry(i, arr);
    map_writable(arr.mp, NULL);
    e->val = op(e->val, v);
    return;
  }
  if (arr.tag == T_REC) {
    wv *p = computed(i, arr);
    writable(i.s, arr); // computed checked that i is a string
    *p = op(*p, v);
    return;
  }
  int64_t k = slot(i, arr);
  mutable(arr.a, NULL);
  arr.a->items[k] = op(arr.a->items[k], v);
}

// ---- bindings, fails, exit ----

// A captured binding read before its declaration has executed (§5.1).
static inline wv rt_def(wv v, const char *name) {
  if (v.tag == T_UNSET) rt_fail("'%s' was used before its declaration", name);
  return v;
}

// ;fail c inside a function: the caller sees code c from this function.
static void rt_fail_with(wv code) {
  if (code.tag != T_INT || code.i < 1) {
    sb b = { 0 };
    sb_puts(&b, "a fail code must be an integer >= 1, got ");
    show_to(&b, code);
    rt_fail_sb(&b);
  }
  rt_failcode = code.i;
  rt_failfrom = rt_stack[rt_depth - 1];
}
// A stdlib function fails with code 1.
static wv rt_std_fail(const char *from) {
  rt_failcode = 1;
  rt_failfrom = from;
  return RT_NULL;
}
// After a call that is not handled with onfail: a pending fail becomes a
// panic in the caller (one-frame rule). caller is NULL at the top level.
static inline wv rt_chk(wv v, const char *caller) {
  if (__builtin_expect(rt_failcode != 0, 0)) {
    if (caller) rt_fail("unhandled fail (code %lld) from '%s' in '%s'", (long long)rt_failcode, rt_failfrom, caller);
    rt_fail("unhandled fail (code %lld) from '%s'", (long long)rt_failcode, rt_failfrom);
  }
  return v;
}

static _Noreturn void rt_panic(wv v) {
  if (v.tag == T_STR) rt_die(v.s->data, v.s->bytes);
  sb b = { 0 };
  show_to(&b, v);
  rt_fail_sb(&b);
}

static void rt_exit_code(int64_t code) {
  rt_flush();
  term_restore();
  if (gc_stats) {
    fprintf(stderr, "gc: %zu collections, live %zu bytes, peak heap %zu bytes\n", gc_runs, gc_live, gc_peak);
  }
#ifdef _WIN32
  ExitProcess((UINT)code);
#else
  exit((int)code);
#endif
}
static void rt_exit(wv code) {
  if (code.tag != T_INT) rt_fail("an exit code must be an int, got %s", type_name(code));
  rt_exit_code(code.i);
}

// ---- host: UTF-8 <-> UTF-16 (Windows) ----

#ifdef _WIN32
// A NUL-terminated UTF-16 copy of n bytes of UTF-8 (malloc'd).
static wchar_t *to_wide(const char *s, int64_t n) {
  int len = n ? MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0) : 0;
  wchar_t *w = xmalloc(sizeof(wchar_t) * (len + 1));
  if (len) MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w, len);
  w[len] = 0;
  return w;
}
// UTF-16 to a string; an unpaired surrogate becomes U+FFFD (§12.1).
static wv from_wide(const wchar_t *w, int64_t n) {
  sb b = { 0 };
  for (int64_t i = 0; i < n; i++) {
    uint32_t c = w[i];
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
      c = 0x10000 + ((c - 0xD800) << 10) + (w[++i] - 0xDC00);
    } else if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
    utf8_put(&b, c);
  }
  return sb_str(&b);
}
#endif

// A path for the host; NULL if it contains a NUL, which no file can match.
#ifdef _WIN32
typedef wchar_t pchar;
static pchar *host_path(const wstr *p) { return memchr(p->data, 0, p->bytes) ? NULL : to_wide(p->data, p->bytes); }
#else
typedef char pchar;
static pchar *host_path(const wstr *p) {
  if (memchr(p->data, 0, p->bytes)) return NULL;
  char *c = xmalloc(p->bytes + 1);
  memcpy(c, p->data, p->bytes + 1);
  return c;
}
#endif

// ---- input ----

static char *in_buf;
static size_t in_n, in_cap, in_pos;
static int in_end;
// Reads more of stdin into in_buf; returns 0 at end of input.
static int read_more(void) {
  rt_flush(); // a prompt written with $write appears before the program waits
  if (in_pos > 0) {
    memmove(in_buf, in_buf + in_pos, in_n - in_pos);
    in_n -= in_pos;
    in_pos = 0;
  }
  if (in_cap - in_n < 65536) {
    in_cap = in_cap ? in_cap * 2 : 131072;
    char *p = xmalloc(in_cap);
    if (in_n) memcpy(p, in_buf, in_n);
    free(in_buf);
    in_buf = p;
  }
#ifdef _WIN32
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode, got = 0;
  if (GetConsoleMode(h, &mode)) {
    // A console gives UTF-16. A high surrogate at the end of one read waits
    // for its pair; at end of input, an unpaired one becomes U+FFFD.
    static wchar_t w[8192], carried;
    DWORD start = 0;
    if (carried) { w[0] = carried; carried = 0; start = 1; }
    if (!ReadConsoleW(h, w + start, 8000, &got, NULL)) got = 0;
    DWORD total = start + got;
    if (got > 0 && w[total - 1] >= 0xD800 && w[total - 1] <= 0xDBFF) carried = w[--total];
    int len = total ? WideCharToMultiByte(CP_UTF8, 0, w, total, in_buf + in_n, (int)(in_cap - in_n), NULL, NULL) : 0;
    in_n += len;
    return got > 0 || len > 0;
  }
  if (!ReadFile(h, in_buf + in_n, (DWORD)(in_cap - in_n), &got, NULL) || got == 0) return 0;
  in_n += got;
  return 1;
#else
  for (;;) {
    ssize_t k = read(0, in_buf + in_n, in_cap - in_n);
    if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
    if (k <= 0) return 0;
    in_n += k;
    return 1;
  }
#endif
}
// One line of stdin without its line ending (§12.2); 0 at end of input.
static int read_line(wv *out) {
  size_t scanned = 0; // bytes after in_pos known to hold no '\n' (read_more moves them)
  for (;;) {
    size_t avail = in_n - in_pos;
    char *nl = avail > scanned ? memchr(in_buf + in_pos + scanned, '\n', avail - scanned) : NULL;
    if (nl) {
      size_t end = nl - in_buf, len = end - in_pos;
      if (len > 0 && in_buf[end - 1] == '\r') len--;
      *out = rt_decode(in_buf + in_pos, len);
      in_pos = end + 1;
      return 1;
    }
    scanned = avail;
    if (in_end || !read_more()) {
      in_end = 1;
      if (in_n == in_pos) return 0;
      *out = rt_decode(in_buf + in_pos, in_n - in_pos);
      in_pos = in_n;
      return 1;
    }
  }
}

// ---- standard library (§12) ----

static const wstr *a_string(wv x, const char *fn) {
  if (x.tag != T_STR) rt_fail("type error: %s expects a string, got %s", fn, type_name(x));
  return x.s;
}
static int64_t an_int(wv x, const char *fn) {
  if (x.tag != T_INT) rt_fail("type error: %s expects an int, got %s", fn, type_name(x));
  return x.i;
}
static warr *an_array(wv x, const char *fn) {
  if (x.tag != T_ARR) rt_fail("type error: %s expects an array, got %s", fn, type_name(x));
  return x.a;
}

static wv print_to(int fd, wv v, int nl) {
  if (v.tag == T_STR && !nl) { rt_write(fd, v.s->data, v.s->bytes); return RT_NULL; }
  sb b = { 0 };
  if (!text_to(&b, v)) no_text(v);
  if (nl) sb_putc(&b, '\n');
  rt_write(fd, b.p, b.n);
  free(b.p);
  return RT_NULL;
}
static wv std_print(wv v) { return print_to(1, v, 1); }
static wv std_write(wv v) { return print_to(1, v, 0); }
static wv std_eprint(wv v) { return print_to(2, v, 1); }

// ---- the terminal and keys (§12.2) ----
// The first $readKey on a terminal turns on key mode: no echo and no line
// buffering. $readLine and $run turn it off while they run, and term_restore
// turns it off however the program ends. On Windows, console output also gets
// virtual terminal processing from startup, so escape sequences work.

#ifdef _WIN32
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif
static HANDLE key_in, vt_out[2];
static DWORD key_saved, vt_saved[2];
#else
static struct termios key_saved;
#endif
static int key_started, key_tty, key_on, key_applied; // key_on: key mode is in use

static void key_apply(int on) {
  if (!key_on || key_applied == on) return;
#ifdef _WIN32
  DWORD raw = (key_saved & ~(DWORD)(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT)) | ENABLE_PROCESSED_INPUT;
  SetConsoleMode(key_in, on ? raw : key_saved);
#else
  struct termios t = key_saved;
  if (on) {
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
  }
  tcsetattr(0, TCSANOW, &t);
#endif
  key_applied = on;
}

static void term_restore(void) {
  key_apply(0);
#ifdef _WIN32
  for (int i = 1; i >= 0; i--) if (vt_out[i]) SetConsoleMode(vt_out[i], vt_saved[i]);
#endif
}

#ifdef _WIN32
static void vt_start(void) {
  DWORD ids[2] = { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE }, mode;
  for (int i = 0; i < 2; i++) {
    HANDLE h = GetStdHandle(ids[i]);
    if (!GetConsoleMode(h, &mode)) continue;
    vt_out[i] = h;
    vt_saved[i] = mode;
    SetConsoleMode(h, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
}
// Ctrl+C ends the program with 130 (§12.2); the console calls this on its own thread.
static BOOL WINAPI key_ctrl(DWORD type) {
  term_restore();
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) ExitProcess(130);
  return FALSE;
}
#else
// A signal that ends the program: restore the terminal, then take the default action.
static void key_signal(int sig) {
  term_restore();
  signal(sig, SIG_DFL);
  raise(sig); // delivered when the handler returns
}
// Ctrl+Z: restore the terminal, stop, and turn key mode back on when resumed.
static void key_stop(int sig) {
  int was = key_applied;
  term_restore();
  signal(SIGTSTP, SIG_DFL);
  sigset_t s;
  sigemptyset(&s);
  sigaddset(&s, SIGTSTP);
  pthread_sigmask(SIG_UNBLOCK, &s, NULL);
  raise(SIGTSTP);
  signal(SIGTSTP, key_stop);
  if (was) key_apply(1);
}
static void key_catch(int sig, void (*handler)(int)) {
  if (signal(sig, handler) == SIG_IGN) signal(sig, SIG_IGN); // a signal the parent ignored stays ignored
}
#endif

static void key_start(void) {
  if (key_started) return;
  key_started = 1;
#ifdef _WIN32
  key_in = GetStdHandle(STD_INPUT_HANDLE);
  key_tty = GetConsoleMode(key_in, &key_saved) != 0;
  if (!key_tty) return;
  SetConsoleCtrlHandler(key_ctrl, TRUE);
#else
  key_tty = isatty(0) && tcgetattr(0, &key_saved) == 0;
  if (!key_tty) return;
  int ends[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE };
  for (int i = 0; i < 5; i++) key_catch(ends[i], key_signal);
  key_catch(SIGTSTP, key_stop);
#endif
  key_on = 1;
}

static wv std_readLine(void) {
  key_apply(0);
  wv line;
  int ok = read_line(&line);
  key_apply(1);
  return ok ? line : rt_std_fail("$readLine");
}

// How long to wait for the bytes of a key: until they come, at most 50 ms in a
// terminal, or not at all (only bytes already read).
enum { K_WAIT, K_BRIEF, K_HAVE };

// Byte i after in_pos, or -1 if it does not come.
static int key_byte(size_t i, int mode) {
  while (in_n - in_pos <= i) {
    if (in_end || mode == K_HAVE) return -1;
#ifndef _WIN32
    if (mode == K_BRIEF && key_tty) {
      struct pollfd p = { .fd = 0, .events = POLLIN };
      if (poll(&p, 1, 50) <= 0) return -1;
    }
#endif
    if (!read_more()) { in_end = 1; return -1; }
  }
  return (unsigned char)in_buf[in_pos + i];
}

// The code point that starts at byte i after in_pos, decoded by the
// maximal-subpart rule (§12.1); *len is set to its length in bytes. -1 if there
// is no byte at i. The rest of a character comes with its first byte, so mode
// only decides whether to wait for the first.
static int64_t key_cp(size_t i, int mode, int *len) {
  int b = key_byte(i, mode);
  if (b < 0) return -1;
  *len = 1;
  if (b < 0x80) return b;
  int need = b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 : b >= 0xF0 && b <= 0xF4 ? 3 : 0;
  if (!need) return 0xFFFD;
  uint32_t cp = b & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
  int lo = b == 0xE0 ? 0xA0 : b == 0xF0 ? 0x90 : 0x80, hi = b == 0xED ? 0x9F : b == 0xF4 ? 0x8F : 0xBF;
  for (int k = 1; k <= need; k++) {
    int c = key_byte(i + k, mode == K_HAVE ? K_HAVE : K_WAIT);
    if (c < lo || c > hi) return 0xFFFD;
    cp = (cp << 6) | (c & 0x3F);
    lo = 0x80;
    hi = 0xBF;
    *len = k + 1;
  }
  return cp;
}

// A code point that types a character: not a control character.
static int is_char(int64_t c) { return c >= 0x20 && !(c >= 0x7F && c <= 0x9F); }

static wv key_char(const char *prefix, uint32_t c) {
  sb b = { 0 };
  sb_puts(&b, prefix);
  utf8_put(&b, c);
  return sb_str(&b);
}

// Ctrl with a letter, c from 1 (Ctrl+A) to 26; 0 if it is discarded.
static int ctrl_key(int c, wv *out) {
  if (c == 3 || c == 26) return 0; // Ctrl+C and Ctrl+Z are signals, never keys
  const char *name = c == 8 ? "backspace" : c == 9 ? "tab" : c == 10 || c == 13 ? "enter" : NULL;
  *out = name ? rt_cstr(name) : key_char("ctrl+", 'a' + c - 1);
  return 1;
}

static const char *const key_fs[] = { "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12" };

// The key named by ESC [ params final, with no intermediate bytes; NULL if none.
static const char *csi_key(const char *p, size_t n, int final) {
  int num = -1; // the first parameter; -1 if it is empty
  for (size_t k = 0; k < n && p[k] != ';'; k++) {
    if (p[k] < '0' || p[k] > '9' || num > 1000) return NULL;
    num = (num < 0 ? 0 : num * 10) + (p[k] - '0');
  }
  if (final == '~') {
    switch (num) {
      case 1: case 7: return "home";
      case 2: return "insert";
      case 3: return "delete";
      case 4: case 8: return "end";
      case 5: return "pageup";
      case 6: return "pagedown";
      case 23: return "f11";
      case 24: return "f12";
    }
    if (num >= 11 && num <= 15) return key_fs[num - 11];
    if (num >= 17 && num <= 21) return key_fs[num - 12];
    return NULL;
  }
  if (num != -1 && num != 1) return NULL;
  switch (final) {
    case 'A': return "up";
    case 'B': return "down";
    case 'C': return "right";
    case 'D': return "left";
    case 'H': return "home";
    case 'F': return "end";
    case 'E': case 'G': return "clear";
    case 'Z': return "tab";
  }
  return NULL;
}
// The key named by ESC O final.
static const char *ss3_key(int final) {
  switch (final) {
    case 'A': return "up";
    case 'B': return "down";
    case 'C': return "right";
    case 'D': return "left";
    case 'H': return "home";
    case 'F': return "end";
    case 'E': return "clear";
    case 'M': return "enter";
    case 'P': return "f1";
    case 'Q': return "f2";
    case 'R': return "f3";
    case 'S': return "f4";
  }
  return NULL;
}

// A key that starts with ESC: 1 with *out set, or 0 if it is discarded.
static int esc_key(wv *out, int brief) {
  int b1 = key_byte(1, brief);
  if (b1 == '[' || b1 == 'O') {
    int b2 = key_byte(2, brief);
    if (b1 == 'O' || b2 == '[') {
      // ESC O x, or the Linux console's ESC [[ x for F1 to F5
      size_t at = b1 == 'O' ? 2 : 3;
      int f = b1 == 'O' ? b2 : key_byte(3, brief);
      if (f >= 0) {
        in_pos += at + 1;
        const char *name = b1 == 'O' ? ss3_key(f) : f >= 'A' && f <= 'E' ? key_fs[f - 'A'] : NULL;
        if (!name) return 0;
        *out = rt_cstr(name);
        return 1;
      }
    } else {
      // ESC [ parameter bytes, intermediate bytes, final byte
      size_t i = 2, params;
      int b = b2;
      while (b >= 0x30 && b <= 0x3F) b = key_byte(++i, brief);
      params = i;
      while (b >= 0x20 && b <= 0x2F) b = key_byte(++i, brief);
      if (b >= 0x40 && b <= 0x7E) {
        const char *name = params == i ? csi_key(in_buf + in_pos + 2, params - 2, b) : NULL;
        in_pos += i + 1;
        if (!name) return 0;
        *out = rt_cstr(name);
        return 1;
      }
    }
  } else if (b1 >= 0) {
    int len;
    int64_t c = key_cp(1, brief, &len);
    if (is_char(c)) {
      in_pos += 1 + len;
      *out = key_char("alt+", (uint32_t)c);
      return 1;
    }
  }
  // Anything else: Escape, and what follows is read as the next keys.
  in_pos += 1;
  *out = rt_cstr("escape");
  return 1;
}

// The next key from the bytes of stdin: 1 with *out set, 0 if it was
// discarded, -1 at the end of input. wait is for a key's first byte, brief for
// the rest of an escape sequence, and crlf for a "\n" after "\r".
static int byte_key(wv *out, int wait, int brief, int crlf) {
  int len;
  int64_t c = key_cp(0, wait, &len);
  if (c < 0) return -1;
  if (c == 0x1B) return esc_key(out, brief);
  in_pos += len;
  if (c == '\r') {
    if (key_byte(0, crlf) == '\n') in_pos++;
    *out = rt_cstr("enter");
    return 1;
  }
  if (c == 0x7F) { *out = rt_cstr("backspace"); return 1; }
  if (c >= 1 && c <= 26) return ctrl_key((int)c, out);
  if (!is_char(c)) return 0;
  *out = key_char("", (uint32_t)c);
  return 1;
}

#ifdef _WIN32
// The next key from the console's key events, with the same results as byte_key.
static wv key_repeat;
static int key_repeats;
static wchar_t key_high;
static int console_key(wv *out) {
  if (key_repeats > 0) {
    key_repeats--;
    *out = key_repeat;
    return 1;
  }
  rt_flush();
  INPUT_RECORD r;
  DWORD got;
  if (!ReadConsoleInputW(key_in, &r, 1, &got) || got == 0) return -1;
  if (r.EventType != KEY_EVENT || !r.Event.KeyEvent.bKeyDown) return 0;
  const KEY_EVENT_RECORD *k = &r.Event.KeyEvent;
  int ctrl = (k->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
  int alt = (k->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
  WORD vk = k->wVirtualKeyCode;
  uint32_t ch = k->uChar.UnicodeChar;
  const char *name = NULL;
  switch (vk) {
    case VK_RETURN: name = "enter"; break;
    case VK_TAB: name = "tab"; break;
    case VK_BACK: name = "backspace"; break;
    case VK_ESCAPE: name = "escape"; break;
    case VK_UP: name = "up"; break;
    case VK_DOWN: name = "down"; break;
    case VK_LEFT: name = "left"; break;
    case VK_RIGHT: name = "right"; break;
    case VK_HOME: name = "home"; break;
    case VK_END: name = "end"; break;
    case VK_PRIOR: name = "pageup"; break;
    case VK_NEXT: name = "pagedown"; break;
    case VK_INSERT: name = "insert"; break;
    case VK_DELETE: name = "delete"; break;
    case VK_CLEAR: name = "clear"; break;
  }
  if (vk >= VK_F1 && vk <= VK_F12) name = key_fs[vk - VK_F1];
  wv v;
  if (name) {
    v = rt_cstr(name);
  } else if (ctrl && !alt && vk >= 'A' && vk <= 'Z') {
    if (!ctrl_key(vk - 'A' + 1, &v)) return 0;
  } else {
    // A character outside the BMP comes as two events, one for each surrogate.
    if (ch >= 0xD800 && ch <= 0xDBFF) { key_high = (wchar_t)ch; return 0; }
    if (ch >= 0xDC00 && ch <= 0xDFFF) {
      if (!key_high) return 0;
      ch = 0x10000 + ((uint32_t)(key_high - 0xD800) << 10) + (ch - 0xDC00);
      key_high = 0;
    }
    if (!is_char(ch)) return 0;
    v = key_char(alt && !ctrl ? "alt+" : "", ch); // Ctrl+Alt is AltGr when it types a character
  }
  if (k->wRepeatCount > 1) {
    key_repeat = v;
    key_repeats = k->wRepeatCount - 1;
  }
  *out = v;
  return 1;
}
#endif

// The next key, waiting for it: 1 with *out set, 0 if it was discarded, -1 at
// the end of input.
static int next_key(wv *out) {
  // Waiting: a terminal waits only briefly for the rest of an escape sequence,
  // and does not wait for a "\n" after "\r", since Enter sends "\r" alone.
  int wait = K_WAIT, brief = key_tty ? K_BRIEF : K_WAIT, crlf = key_tty ? K_HAVE : K_WAIT;
#ifdef _WIN32
  if (key_tty) wait = brief = crlf = K_HAVE; // only text left over from $readLine
  if (key_tty && in_pos == in_n) return console_key(out);
#endif
  return byte_key(out, wait, brief, crlf);
}

static wv std_readKey(void) {
  key_start();
  key_apply(1);
  for (;;) {
    wv k;
    int r = next_key(&k);
    if (r < 0) return rt_std_fail("$readKey");
    if (r > 0) return k;
  }
}

// ---- waiting and the clock (§12.2, §12.8) ----
// Times are milliseconds on a monotonic clock. A wait ends at a deadline, and
// is recomputed after each wake-up, so a signal or a stray console event only
// makes it wait again for what is left.

static int64_t mono_ms(void) {
#ifdef _WIN32
  static LARGE_INTEGER f;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart / f.QuadPart * 1000 + t.QuadPart % f.QuadPart * 1000 / f.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
static int64_t clock_start; // mono_ms() when the program started

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
// A timer set to go off in ms, or NULL if there is none. It is a
// high-resolution timer where Windows has them (10 1803 and later), so short
// waits are not rounded up to the 15.6 ms system tick.
static HANDLE wait_timer(int64_t ms) {
  static HANDLE t;
  if (!t) t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!t) t = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
  LARGE_INTEGER due = { .QuadPart = -ms * 10000 }; // relative, in 100 ns units
  if (t && !SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE)) return NULL;
  return t;
}
#endif

// Waits for up to ms (at most INT32_MAX); it may end early.
static void sleep_for(int64_t ms) {
#ifdef _WIN32
  HANDLE t = wait_timer(ms);
  if (t) WaitForSingleObject(t, INFINITE);
  else Sleep((DWORD)ms);
#else
  struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };
  nanosleep(&ts, NULL);
#endif
}

// Waits for up to ms (at most INT32_MAX) for stdin to have something to read,
// or to end: 1 if it does, 0 if not (it may give up early).
static int input_wait(int64_t ms) {
#ifdef _WIN32
  if (key_tty) {
    if (key_repeats > 0) return 1;
    HANDLE t = ms > 0 ? wait_timer(ms) : NULL, hs[2] = { key_in, t };
    DWORD r = t ? WaitForMultipleObjects(2, hs, FALSE, INFINITE) : WaitForSingleObject(key_in, (DWORD)ms);
    return r == WAIT_OBJECT_0;
  }
  // Pipes cannot be waited on, so they are polled; files and devices are always ready.
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  if (GetFileType(h) != FILE_TYPE_PIPE) return 1;
  int64_t end = mono_ms() + ms;
  for (;;) {
    DWORD avail;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail > 0) return 1; // data, or the end
    if (mono_ms() >= end) return 0;
    Sleep(1);
  }
#else
  struct pollfd p = { .fd = 0, .events = POLLIN };
  return poll(&p, 1, (int)ms) > 0;
#endif
}

// Whether a key can be read without waiting past the deadline end: 1 if input
// is there or has ended, 0 if end comes first.
static int key_ready(int64_t end) {
  rt_flush(); // what the program drew appears before it waits
  for (;;) {
    if (in_pos < in_n || in_end) return 1;
    int64_t left = end - mono_ms();
    if (input_wait(left <= 0 ? 0 : left > INT32_MAX ? INT32_MAX : left)) return 1;
    if (mono_ms() >= end) return 0;
  }
}

static wv std_waitKey(wv x) {
  int64_t ms = an_int(x, "$waitKey");
  key_start();
  key_apply(1);
  int64_t end = mono_ms() + (ms > 0 ? ms : 0);
  for (;;) {
    if (!key_ready(end)) return RT_NULL;
    wv k;
    int r = next_key(&k);
    if (r < 0) return rt_std_fail("$waitKey");
    if (r > 0) return k;
  }
}

static wv std_sleep(wv x) {
  int64_t ms = an_int(x, "$sleep");
  rt_flush();
  int64_t end = mono_ms() + (ms > 0 ? ms : 0);
  for (int64_t left; (left = end - mono_ms()) > 0;) sleep_for(left > INT32_MAX ? INT32_MAX : left);
  return RT_NULL;
}

static wv std_elapsed(void) { return rt_i(mono_ms() - clock_start); }

// Reads a whole file; returns 0 on failure.
static int read_file(const wstr *path, char **data, int64_t *n) {
  pchar *p = host_path(path);
  if (!p) return 0;
#ifdef _WIN32
  FILE *f = _wfopen(p, L"rb");
#else
  FILE *f = fopen(p, "rb");
#endif
  free(p);
  if (!f) return 0;
  sb b = { 0 };
  char tmp[65536];
  size_t k;
  while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) sb_putn(&b, tmp, k);
  int ok = !ferror(f);
  fclose(f);
  *data = b.p;
  *n = b.n;
  return ok;
}
static wv std_readFile(wv p) {
  const wstr *path = a_string(p, "$readFile");
  char *data;
  int64_t n;
  if (!read_file(path, &data, &n)) return rt_std_fail("$readFile");
  wv s = rt_decode(data ? data : "", n);
  free(data);
  return s;
}
static wv put_file(wv p, wv t, const char *fn, int append) {
  const wstr *path = a_string(p, fn), *text = a_string(t, fn);
  pchar *hp = host_path(path);
  if (!hp) return rt_std_fail(fn);
#ifdef _WIN32
  FILE *f = _wfopen(hp, append ? L"ab" : L"wb");
#else
  FILE *f = fopen(hp, append ? "ab" : "wb");
#endif
  free(hp);
  if (!f) return rt_std_fail(fn);
  int ok = fwrite(text->data, 1, text->bytes, f) == (size_t)text->bytes;
  if (fclose(f) != 0) ok = 0;
  return ok ? RT_NULL : rt_std_fail(fn);
}
static wv std_writeFile(wv p, wv t) { return put_file(p, t, "$writeFile", 0); }
static wv std_appendFile(wv p, wv t) { return put_file(p, t, "$appendFile", 1); }
static wv std_fileExists(wv p) {
  pchar *hp = host_path(a_string(p, "$fileExists"));
  if (!hp) return rt_b(0);
#ifdef _WIN32
  int ok = GetFileAttributesW(hp) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  int ok = stat(hp, &st) == 0;
#endif
  free(hp);
  return rt_b(ok);
}
static int cmp_wv_str(const void *a, const void *b) { return str_cmp(((const wv *)a)->s, ((const wv *)b)->s); }
static wv std_listDir(wv p) {
  const wstr *path = a_string(p, "$listDir");
  if (path->bytes == 0) return rt_std_fail("$listDir");
  pchar *hp = host_path(path);
  if (!hp) return rt_std_fail("$listDir");
  warr *a = arr_new(0, 0);
  int64_t cap = 0;
#ifdef _WIN32
  size_t len = wcslen(hp);
  wchar_t *pat = xmalloc(sizeof(wchar_t) * (len + 3));
  wcscpy(pat, hp);
  wcscpy(pat + len, L"\\*");
  free(hp);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(pat, &fd);
  free(pat);
  if (h == INVALID_HANDLE_VALUE) return rt_std_fail("$listDir");
  do {
    const wchar_t *n = fd.cFileName;
    if (!wcscmp(n, L".") || !wcscmp(n, L"..")) continue;
    wv name = from_wide(n, wcslen(n));
#else
  DIR *d = opendir(hp);
  free(hp);
  if (!d) return rt_std_fail("$listDir");
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    wv name = rt_decode(e->d_name, strlen(e->d_name));
#endif
    if (a->len == cap) {
      cap = cap ? cap * 2 : 16;
      wv *items = rt_alloc(sizeof(wv) * cap);
      if (a->len) memcpy(items, a->items, sizeof(wv) * a->len);
      a->items = items;
    }
    a->items[a->len++] = name;
#ifdef _WIN32
  } while (FindNextFileW(h, &fd));
  FindClose(h);
#else
  }
  closedir(d);
#endif
  a->cap = cap;
  qsort(a->items, a->len, sizeof(wv), cmp_wv_str);
  return (wv){ .tag = T_ARR, .a = a };
}
static wv std_deleteFile(wv p) {
  pchar *hp = host_path(a_string(p, "$deleteFile"));
  if (!hp) return rt_std_fail("$deleteFile");
#ifdef _WIN32
  int ok = DeleteFileW(hp);
#else
  int ok = unlink(hp) == 0;
#endif
  free(hp);
  return ok ? RT_NULL : rt_std_fail("$deleteFile");
}

static int rt_argc;
static char **rt_argv;
static wv args_cache = { .tag = T_UNSET };
static wv std_args(void) {
  if (args_cache.tag != T_UNSET) return args_cache;
#ifdef _WIN32
  int n;
  wchar_t **w = CommandLineToArgvW(GetCommandLineW(), &n);
  warr *a = arr_new(n > 1 ? n - 1 : 0, 0);
  for (int i = 1; i < n; i++) a->items[i - 1] = from_wide(w[i], wcslen(w[i]));
  LocalFree(w);
#else
  warr *a = arr_new(rt_argc > 1 ? rt_argc - 1 : 0, 0);
  for (int i = 1; i < rt_argc; i++) a->items[i - 1] = rt_decode(rt_argv[i], strlen(rt_argv[i]));
#endif
  args_cache = (wv){ .tag = T_ARR, .a = a };
  return args_cache;
}
static wv std_env(wv name) {
  const wstr *n = a_string(name, "$env");
  if (n->bytes == 0 || memchr(n->data, 0, n->bytes)) return rt_std_fail("$env");
#ifdef _WIN32
  wchar_t *w = to_wide(n->data, n->bytes);
  const wchar_t *v = _wgetenv(w);
  free(w);
  return v ? from_wide(v, wcslen(v)) : rt_std_fail("$env");
#else
  const char *v = getenv(n->data);
  return v ? rt_decode(v, strlen(v)) : rt_std_fail("$env");
#endif
}
static wv program_path_cache = { .tag = T_UNSET };
static wv std_programPath(void) {
  if (program_path_cache.tag != T_UNSET) return program_path_cache;
#ifdef _WIN32
  static wchar_t buf[32768];
  DWORD n = GetModuleFileNameW(NULL, buf, 32768);
  program_path_cache = from_wide(buf, n);
#else
  char buf[PATH_MAX + 1];
  ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX);
  if (n > 0) program_path_cache = rt_decode(buf, n);
  else if (realpath(rt_argv[0], buf)) program_path_cache = rt_decode(buf, strlen(buf));
  else program_path_cache = rt_cstr(rt_argv[0]);
#endif
  return program_path_cache;
}

#ifdef _WIN32
// Quotes one argument so the program's C runtime parses it back unchanged
// (the rules libuv uses for Node's spawn).
static void quote_arg(sb *b, const wstr *a) {
  if (a->bytes == 0) { sb_puts(b, "\"\""); return; }
  if (!strpbrk(a->data, " \t\"")) { sb_putn(b, a->data, a->bytes); return; }
  if (!strpbrk(a->data, "\"\\")) { sb_putc(b, '"'); sb_putn(b, a->data, a->bytes); sb_putc(b, '"'); return; }
  sb_putc(b, '"');
  for (int64_t i = 0;; i++) {
    int64_t slashes = 0;
    while (i < a->bytes && a->data[i] == '\\') { slashes++; i++; }
    if (i == a->bytes) {
      for (int64_t k = 0; k < slashes * 2; k++) sb_putc(b, '\\');
      break;
    }
    if (a->data[i] == '"') {
      for (int64_t k = 0; k < slashes * 2 + 1; k++) sb_putc(b, '\\');
    } else {
      for (int64_t k = 0; k < slashes; k++) sb_putc(b, '\\');
    }
    sb_putc(b, a->data[i]);
  }
  sb_putc(b, '"');
}
static int is_file(const wchar_t *p) {
  DWORD a = GetFileAttributesW(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
// Tries dir\name as it is (if it has an extension), then with .com and .exe.
static wchar_t *try_exts(const wchar_t *dir, size_t dlen, const wchar_t *name) {
  size_t nlen = wcslen(name);
  wchar_t *p = xmalloc(sizeof(wchar_t) * (dlen + nlen + 6));
  wmemcpy(p, dir, dlen);
  size_t off = dlen;
  if (dlen && dir[dlen - 1] != L'\\' && dir[dlen - 1] != L'/') p[off++] = L'\\';
  wmemcpy(p + off, name, nlen);
  off += nlen;
  const wchar_t *base = name;
  for (const wchar_t *q = name; *q; q++) if (*q == L'\\' || *q == L'/') base = q + 1;
  p[off] = 0;
  if (wcschr(base, L'.') && is_file(p)) return p;
  static const wchar_t *exts[] = { L".com", L".exe" };
  for (int i = 0; i < 2; i++) {
    wcscpy(p + off, exts[i]);
    if (is_file(p)) return p;
  }
  free(p);
  return NULL;
}
// Finds the program as libuv does: a name with a path is used as it is;
// otherwise the current directory, then each PATH entry.
static wchar_t *find_program(const wchar_t *name) {
  if (wcspbrk(name, L"\\/:")) return try_exts(L"", 0, name);
  wchar_t *r = try_exts(L".", 1, name);
  if (r) return r;
  const wchar_t *path = _wgetenv(L"PATH");
  if (!path) return NULL;
  for (const wchar_t *s = path; *s;) {
    const wchar_t *e = wcschr(s, L';');
    size_t len = e ? (size_t)(e - s) : wcslen(s);
    const wchar_t *d = s;
    if (len >= 2 && d[0] == L'"' && d[len - 1] == L'"') { d++; len -= 2; }
    if (len && (r = try_exts(d, len, name))) return r;
    if (!e) break;
    s = e + 1;
  }
  return NULL;
}
#endif

// Runs a program with this program's stdio and no shell; returns its exit code (§12.4).
static wv run_program(wv program, wv args) {
  const wstr *prog = a_string(program, "$run");
  warr *a = an_array(args, "$run");
  for (int64_t i = 0; i < a->len; i++) {
    if (a->items[i].tag != T_STR) {
      rt_fail("type error: $run expects an array of strings, got an element of type %s", type_name(a->items[i]));
    }
  }
  rt_flush();
  if (prog->bytes == 0 || memchr(prog->data, 0, prog->bytes)) return rt_std_fail("$run");
#ifdef _WIN32
  wchar_t *wname = to_wide(prog->data, prog->bytes);
  wchar_t *app = find_program(wname);
  free(wname);
  if (!app) return rt_std_fail("$run");
  sb cmd = { 0 };
  quote_arg(&cmd, prog);
  for (int64_t i = 0; i < a->len; i++) {
    sb_putc(&cmd, ' ');
    quote_arg(&cmd, a->items[i].s);
  }
  wchar_t *wcmd = to_wide(cmd.p, cmd.n);
  free(cmd.p);
  STARTUPINFOW si = { .cb = sizeof si };
  si.dwFlags = STARTF_USESTDHANDLES;
  HANDLE self = GetCurrentProcess(), std[3] = { 0 };
  DWORD ids[3] = { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
  for (int i = 0; i < 3; i++) {
    HANDLE h = GetStdHandle(ids[i]);
    if (h && h != INVALID_HANDLE_VALUE) DuplicateHandle(self, h, self, &std[i], 0, TRUE, DUPLICATE_SAME_ACCESS);
  }
  si.hStdInput = std[0];
  si.hStdOutput = std[1];
  si.hStdError = std[2];
  PROCESS_INFORMATION pi;
  BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
  for (int i = 0; i < 3; i++) if (std[i]) CloseHandle(std[i]);
  free(app);
  free(wcmd);
  if (!ok) return rt_std_fail("$run");
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return rt_i(code);
#else
  char **argv = xmalloc(sizeof(char *) * (a->len + 2));
  argv[0] = (char *)prog->data;
  for (int64_t i = 0; i < a->len; i++) argv[i + 1] = (char *)a->items[i].s->data;
  argv[a->len + 1] = NULL;
  pid_t pid;
  int err = posix_spawnp(&pid, prog->data, NULL, NULL, argv, environ);
  free(argv);
  if (err) return rt_std_fail("$run");
  int status;
  while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return rt_std_fail("$run");
  if (WIFSIGNALED(status)) return rt_i(128 + WTERMSIG(status));
  return rt_i(WEXITSTATUS(status));
#endif
}
// The program gets the terminal as it normally is (§12.2).
static wv std_run(wv program, wv args) {
  key_apply(0);
  wv r = run_program(program, args);
  key_apply(1);
  return r;
}

static wv std_length(wv x) {
  if (x.tag == T_STR) return rt_i(x.s->cps);
  if (x.tag == T_ARR) return rt_i(x.a->len);
  if (x.tag == T_MAP) return rt_i(x.mp->n);
  rt_fail("type error: $length expects a string, an array or a map, got %s", type_name(x));
}
static int64_t cp_index(wv i, int64_t n, const char *fn) {
  int64_t k = an_int(i, fn);
  if (k < -n || k >= n) rt_fail("%s: index %lld is out of range for length %lld", fn, (long long)k, (long long)n);
  return k < 0 ? k + n : k;
}
static wv std_at(wv x, wv i) {
  if (x.tag == T_ARR) return rt_idx(i, x);
  const wstr *s = a_string(x, "$at");
  int64_t k = cp_index(i, s->cps, "$at");
  int64_t off = cp_offset(s, k), end = cp_offset(s, k + 1);
  return rt_str(s->data + off, end - off);
}
static wv std_slice(wv x, wv a, wv b) {
  int is_arr = x.tag == T_ARR;
  if (!is_arr) a_string(x, "$slice");
  int64_t ai = an_int(a, "$slice"), bi = an_int(b, "$slice");
  int64_t n = is_arr ? x.a->len : x.s->cps;
  int64_t s = ai < 0 ? ai + n : ai, e = bi < 0 ? bi + n : bi;
  if (!(s >= 0 && s <= e && e <= n)) rt_fail("$slice: %lld..%lld is out of range for length %lld", (long long)ai, (long long)bi, (long long)n);
  if (is_arr) return arr_copy(x.a, s, e, 0);
  int64_t so = cp_offset(x.s, s), eo = cp_offset(x.s, e);
  return rt_str(x.s->data + so, eo - so);
}
static wv std_code(wv x) {
  const wstr *s = a_string(x, "$code");
  if (s->cps != 1) {
    sb b = { 0 };
    sb_puts(&b, "$code expects a single character, got ");
    show_to(&b, x);
    rt_fail_sb(&b);
  }
  int len;
  return rt_i(utf8_get((const unsigned char *)s->data, &len));
}
static wv std_fromCode(wv c) {
  if (c.tag != T_INT || c.i < 0 || c.i > 0x10FFFF || (c.i >= 0xD800 && c.i <= 0xDFFF)) {
    sb b = { 0 };
    sb_puts(&b, "$fromCode: ");
    show_to(&b, c);
    sb_puts(&b, " is not a valid code point");
    rt_fail_sb(&b);
  }
  sb b = { 0 };
  utf8_put(&b, (uint32_t)c.i);
  return sb_str(&b);
}
static wv std_split(wv x, wv y) {
  const wstr *s = a_string(x, "$split"), *sep = a_string(y, "$split");
  int64_t n = sep->bytes == 0 ? s->cps : 1;
  if (sep->bytes) {
    for (int64_t i = 0; i + sep->bytes <= s->bytes;) {
      if (memcmp(s->data + i, sep->data, sep->bytes) == 0) { n++; i += sep->bytes; }
      else i++;
    }
  }
  warr *a = arr_new(n, 0);
  if (sep->bytes == 0) {
    for (int64_t i = 0, off = 0; i < n; i++) {
      int len;
      utf8_get((const unsigned char *)s->data + off, &len);
      a->items[i] = rt_str(s->data + off, len);
      off += len;
    }
  } else {
    int64_t k = 0, start = 0;
    for (int64_t i = 0; i + sep->bytes <= s->bytes;) {
      if (memcmp(s->data + i, sep->data, sep->bytes) == 0) {
        a->items[k++] = rt_str(s->data + start, i - start);
        i += sep->bytes;
        start = i;
      } else i++;
    }
    a->items[k] = rt_str(s->data + start, s->bytes - start);
  }
  return (wv){ .tag = T_ARR, .a = a };
}
static wv std_join(wv arr, wv sep) {
  warr *a = an_array(arr, "$join");
  const wstr *s = a_string(sep, "$join");
  sb b = { 0 };
  for (int64_t i = 0; i < a->len; i++) {
    if (i) sb_putn(&b, s->data, s->bytes);
    if (!text_to(&b, a->items[i])) no_text(a->items[i]);
  }
  return sb_str(&b);
}
static wv std_toString(wv v) { return rt_text(v); }

// ---- numbers as text ($toInt, $toFloat) ----

static int all_digits(const char *p, int64_t n, int base) {
  if (n == 0) return 0;
  for (int64_t i = 0; i < n; i++) {
    char c = p[i];
    int ok = base == 2 ? c == '0' || c == '1'
      : base == 10 ? c >= '0' && c <= '9'
      : (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) return 0;
  }
  return 1;
}
// Digits in base to an int; 0 if beyond the safe range.
static int parse_uint(const char *p, int64_t n, int base, int64_t *out) {
  int64_t v = 0;
  for (int64_t i = 0; i < n; i++) {
    char c = p[i];
    int d = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
    v = v * base + d;
    if (v > RT_MAX) return 0;
  }
  *out = v;
  return 1;
}
// Decimal float text: -?\d+(\.\d+)?([eE][+-]?\d+)? (the sign only if sign is set).
static int float_text(const char *p, int64_t n, int sign) {
  int64_t i = 0;
  if (sign && i < n && p[i] == '-') i++;
  int64_t s = i;
  while (i < n && p[i] >= '0' && p[i] <= '9') i++;
  if (i == s) return 0;
  if (i < n && p[i] == '.') {
    s = ++i;
    while (i < n && p[i] >= '0' && p[i] <= '9') i++;
    if (i == s) return 0;
  }
  if (i < n && (p[i] == 'e' || p[i] == 'E')) {
    i++;
    if (i < n && (p[i] == '+' || p[i] == '-')) i++;
    s = i;
    while (i < n && p[i] >= '0' && p[i] <= '9') i++;
    if (i == s) return 0;
  }
  return i == n;
}
static wv std_toInt(wv x) {
  if (x.tag == T_INT) return x;
  if (x.tag != T_STR) {
    rt_fail("type error: $toInt expects a string, got %s%s", type_name(x),
      x.tag == T_FLOAT ? " (use $floor, $ceil, $round or $trunc)" : "");
  }
  const char *p = x.s->data;
  int64_t n = x.s->bytes, neg = n > 0 && p[0] == '-';
  if (all_digits(p + neg, n - neg, 10)) {
    int64_t v;
    if (!parse_uint(p + neg, n - neg, 10, &v)) return rt_std_fail("$toInt");
    return rt_i(neg ? -v : v);
  }
  return rt_std_fail("$toInt");
}
static wv std_toFloat(wv x) {
  if (x.tag == T_INT) return rt_f((double)x.i);
  if (x.tag == T_FLOAT) return x;
  const wstr *s = a_string(x, "$toFloat");
  if (float_text(s->data, s->bytes, 1)) {
    // A result too large for a double signals (§9.5).
    double v = to_double(s->data, s->bytes);
    return isfinite(v) ? rt_f(v) : rt_std_fail("$toFloat");
  }
  return rt_std_fail("$toFloat");
}

static wv rounder(wv x, const char *name, double (*fn)(double)) {
  if (x.tag == T_INT) return x;
  if (x.tag != T_FLOAT) rt_fail("type error: $%s expects a number, got %s", name, type_name(x));
  if (!isfinite(x.f)) {
    sb b = { 0 };
    sb_printf(&b, "$%s: cannot convert ", name);
    fmt_float(&b, x.f);
    sb_puts(&b, " to an int");
    rt_fail_sb(&b);
  }
  double r = fn(x.f);
  char op[16];
  snprintf(op, sizeof op, "$%s", name);
  if (!(r <= (double)RT_MAX && r >= -(double)RT_MAX)) rt_fail("integer overflow in '%s'", op);
  return rt_i((int64_t)r);
}
static wv std_floor(wv x) { return rounder(x, "floor", floor); }
static wv std_ceil(wv x) { return rounder(x, "ceil", ceil); }
static wv std_round(wv x) { return rounder(x, "round", round); } // half away from zero
static wv std_trunc(wv x) { return rounder(x, "trunc", trunc); }
static wv std_toUint32(wv x) { return rt_i((uint32_t)an_int(x, "$toUint32")); }
static wv std_isNan(wv x) { return rt_b(x.tag == T_FLOAT && isnan(x.f)); }

static wv std_push(wv arr, wv v) {
  warr *a = an_array(arr, "$push");
  mutable(a, "$push");
  if (a->len == a->cap) {
    int64_t cap = a->cap ? a->cap * 2 : 8;
    wv *items = rt_alloc(sizeof(wv) * cap);
    if (a->len) memcpy(items, a->items, sizeof(wv) * a->len);
    a->items = items;
    a->cap = cap;
  }
  a->items[a->len++] = v;
  return RT_NULL;
}
static wv std_pop(wv arr) {
  warr *a = an_array(arr, "$pop");
  mutable(a, "$pop");
  if (a->len == 0) rt_fail("$pop: the array is empty");
  wv v = a->items[--a->len];
  a->items[a->len] = RT_UNSET; // so that the collector can free it
  return v;
}
static int same(wv a, wv b) {
  if (a.tag == T_ARR && b.tag == T_ARR) {
    if (a.a->len != b.a->len) return 0;
    for (int64_t i = 0; i < a.a->len; i++) if (!same(a.a->items[i], b.a->items[i])) return 0;
    return 1;
  }
  if (a.tag == T_REC && b.tag == T_REC) {
    if (a.r->n != b.r->n) return 0;
    for (int64_t i = 0; i < a.r->n; i++) {
      int64_t j = 0;
      while (j < b.r->n && b.r->keys[j] != a.r->keys[i]) j++;
      if (j == b.r->n || !same(a.r->vals[i], b.r->vals[j])) return 0;
    }
    return 1;
  }
  if (a.tag == T_MAP && b.tag == T_MAP) {
    // The same keys, in any order, with the same values.
    if (a.mp->n != b.mp->n) return 0;
    for (int64_t i = 0; i < a.mp->len; i++) {
      if (!a.mp->e[i].live) continue;
      int64_t j = map_find(b.mp, a.mp->e[i].key);
      if (j < 0 || !same(a.mp->e[i].val, b.mp->e[j].val)) return 0;
    }
    return 1;
  }
  return eq(a, b);
}
static wv std_same(wv a, wv b) { return rt_b(same(a, b)); }
static wv std_typeOf(wv x) { return x.tag == T_MEM ? rt_s(x.m->enum_name) : rt_cstr(type_name(x)); }
static wv std_fields(wv r) {
  if (r.tag != T_REC) rt_fail("type error: $fields expects a record, got %s", type_name(r));
  warr *a = arr_new(r.r->n, 0);
  for (int64_t i = 0; i < r.r->n; i++) a->items[i] = rt_s(r.r->keys[i]);
  return (wv){ .tag = T_ARR, .a = a };
}

// ---- map stdlib (§10.11) ----

static wmap *a_map(wv x, const char *fn) {
  if (x.tag != T_MAP) rt_fail("type error: %s expects a map, got %s", fn, type_name(x));
  return x.mp;
}
static wv std_get(wv m, wv k) {
  a_map(m, "$get");
  key_ok(k);
  int64_t at = map_find(m.mp, k);
  return at < 0 ? rt_std_fail("$get") : m.mp->e[at].val;
}
static wv std_has(wv m, wv k) {
  a_map(m, "$has");
  key_ok(k);
  return rt_b(map_find(m.mp, k) >= 0);
}
static wv std_remove(wv m, wv k) {
  wmap *p = a_map(m, "$remove");
  key_ok(k);
  map_writable(p, "$remove");
  int64_t at = map_find(p, k);
  if (at < 0) return rt_std_fail("$remove");
  wv v = p->e[at].val;
  p->e[at] = (wment){ 0 }; // dead: compacted away later, and not keeping its key and value alive
  p->n--;
  return v;
}
// The keys (which 1) or values (0) of a map, in insertion order.
static wv map_column(wv m, int which, const char *fn) {
  wmap *p = a_map(m, fn);
  warr *a = arr_new(p->n, 0);
  for (int64_t i = 0, k = 0; i < p->len; i++) if (p->e[i].live) a->items[k++] = which ? p->e[i].key : p->e[i].val;
  return (wv){ .tag = T_ARR, .a = a };
}
static wv std_keys(wv m) { return map_column(m, 1, "$keys"); }
static wv std_values(wv m) { return map_column(m, 0, "$values"); }

// ---- for loops (§7.4) ----
// A loop over an array or a map. src is the collection, evaluated once; a map
// is walked through snapshots of its keys and values, so the body may change it.
// Pass k reads position k, or n - 1 - k when reversed. Loops over ranges and
// enums need no state beyond their pass number.
typedef struct wfor { wv src, keys, vals; int64_t n; int map, rev; } wfor;

// The number of passes over the ints from lo up to hi, not including hi, by st.
static int64_t rt_for_range(wv lo, wv hi, wv st) {
  if (lo.tag != T_INT || hi.tag != T_INT) {
    rt_fail("type error: a for loop's range needs ints, got %s and %s", type_name(lo), type_name(hi));
  }
  if (st.tag != T_INT) rt_fail("type error: a for loop's step must be an int, got %s", type_name(st));
  if (st.i < 1) rt_fail("a for loop's step must be greater than 0, got %lld", (long long)st.i);
  return hi.i <= lo.i ? 0 : (hi.i - lo.i + st.i - 1) / st.i;
}
static void rt_for_start(wfor *f, wv c, int rev) {
  *f = (wfor){ .src = c, .keys = RT_NULL, .vals = RT_NULL, .rev = rev };
  if (c.tag == T_ARR) {
    f->n = c.a->len;
  } else if (c.tag == T_MAP) {
    f->map = 1;
    f->keys = std_keys(c);
    f->vals = std_values(c);
    f->n = f->keys.a->len;
  } else {
    rt_fail("type error: a for loop goes through an array, a map, an enum or a range of ints, got %s%s", type_name(c),
      c.tag == T_STR ? " (for its characters, loop over (s, \"\")($split))"
      : c.tag == T_REC ? " (for its field names, loop over r($fields))" : "");
  }
}
// Before each pass, and after the last: is there another? An array's length must not change.
static int rt_for_more(wfor *f, int64_t k) {
  if (!f->map && f->src.a->len != f->n) {
    rt_fail("an array's length changed during a for loop over it: it was %lld, and is now %lld",
      (long long)f->n, (long long)f->src.a->len);
  }
  return k < f->n;
}
// The collection's path (xs, state.items) must still hold the collection being walked.
static void rt_for_guard(wfor *f, wv now, const char *path) {
  const void *was = f->map ? (const void *)f->src.mp : (const void *)f->src.a;
  const void *is = now.tag == T_ARR ? (const void *)now.a : now.tag == T_MAP ? (const void *)now.mp : NULL;
  if (now.tag != f->src.tag || is != was) {
    rt_fail("'%s' was set to another value during a for loop over it", path);
  }
}
static inline int64_t for_at(const wfor *f, int64_t k) { return f->rev ? f->n - 1 - k : k; }
// The variable of a one-name loop: an element, or a map's key.
static wv rt_for_item(wfor *f, int64_t k) {
  int64_t i = for_at(f, k);
  return f->map ? f->keys.a->items[i] : f->src.a->items[i];
}
// The first and second variables of a two-name loop: index and element, or key and value.
static wv rt_for_key(wfor *f, int64_t k) {
  int64_t i = for_at(f, k);
  return f->map ? f->keys.a->items[i] : rt_i(i);
}
static wv rt_for_val(wfor *f, int64_t k) {
  int64_t i = for_at(f, k);
  return f->map ? f->vals.a->items[i] : f->src.a->items[i];
}

// ---- sorting (§10.6) ----
// A stable merge sort of positions by their keys, so that every backend gives
// the same order. The keys are checked before sorting, so that the same key
// panics whatever order the sort compares them in.

static int key_cmp(wv a, wv b) {
  if (a.tag == T_INT && b.tag == T_INT) return a.i < b.i ? -1 : a.i > b.i;
  if (a.tag == T_STR) return str_cmp(a.s, b.s);
  double x = nv(a), y = nv(b);
  return x < y ? -1 : x > y;
}
static void merge_sort(int64_t *ix, int64_t *tmp, int64_t n, const wv *keys) {
  if (n <= 16) {
    for (int64_t i = 1; i < n; i++) {
      int64_t x = ix[i], j = i;
      for (; j > 0 && key_cmp(keys[ix[j - 1]], keys[x]) > 0; j--) ix[j] = ix[j - 1];
      ix[j] = x;
    }
    return;
  }
  int64_t h = n / 2;
  merge_sort(ix, tmp, h, keys);
  merge_sort(ix + h, tmp, n - h, keys);
  if (key_cmp(keys[ix[h - 1]], keys[ix[h]]) <= 0) return;
  memcpy(tmp, ix, sizeof(int64_t) * h);
  int64_t i = 0, j = h, o = 0;
  while (i < h && j < n) ix[o++] = key_cmp(keys[ix[j]], keys[tmp[i]]) < 0 ? ix[j++] : tmp[i++];
  while (i < h) ix[o++] = tmp[i++];
}
// The items of a in the order of their keys, as a new frozen array.
static wv sort_by_keys(const warr *a, const wv *keys, const char *fn) {
  int64_t n = a->len;
  for (int64_t i = 0; i < n; i++) {
    wv k = keys[i];
    if (!isnum(k) && k.tag != T_STR) rt_fail("type error: %s can sort only numbers and strings, got %s", fn, type_name(k));
    if (i > 0 && (k.tag == T_STR) != (keys[0].tag == T_STR)) {
      rt_fail("type error: %s cannot compare %s with %s", fn, type_name(keys[0]), type_name(k));
    }
    if (k.tag == T_FLOAT && isnan(k.f)) rt_fail("%s cannot sort nan", fn);
  }
  int64_t *ix = xmalloc(sizeof(int64_t) * (n ? n : 1)), *tmp = xmalloc(sizeof(int64_t) * (n / 2 + 1));
  for (int64_t i = 0; i < n; i++) ix[i] = i;
  merge_sort(ix, tmp, n, keys);
  warr *out = arr_new(n, 0);
  for (int64_t i = 0; i < n; i++) out->items[i] = a->items[ix[i]];
  free(ix);
  free(tmp);
  return (wv){ .tag = T_ARR, .a = out };
}
static wv std_sort(wv arr) {
  warr *a = an_array(arr, "$sort");
  return sort_by_keys(a, a->items, "$sort");
}
// by: the name of a field of every item, or an array with one key for each item.
static wv std_sortBy(wv arr, wv by) {
  warr *a = an_array(arr, "$sortBy");
  if (by.tag == T_ARR) {
    if (by.a->len != a->len) {
      rt_fail("$sortBy: there are %lld keys for %lld items", (long long)by.a->len, (long long)a->len);
    }
    return sort_by_keys(a, by.a->items, "$sortBy");
  }
  if (by.tag != T_STR) rt_fail("type error: $sortBy expects a field name or an array of keys, got %s", type_name(by));
  const wstr *key = sym_find(by.s);
  wv *keys = rt_alloc(sizeof(wv) * (a->len ? a->len : 1));
  for (int64_t i = 0; i < a->len; i++) keys[i] = *field(key, by.s, a->items[i]);
  wv r = sort_by_keys(a, keys, "$sortBy");
  return r;
}
static wv std_reverse(wv arr) {
  warr *a = an_array(arr, "$reverse");
  warr *out = arr_new(a->len, 0);
  for (int64_t i = 0; i < a->len; i++) out->items[i] = a->items[a->len - 1 - i];
  return (wv){ .tag = T_ARR, .a = out };
}

// Fills keys with the record keys for names: the program's own keys for those
// names, if it has them, so that r.name and { name: ... } work as for any
// record. keys must be registered with rt_root.
static void rec_keys(const char *const *names, int n, const wstr **keys) {
  if (keys[0]) return;
  for (int i = 0; i < n; i++) {
    int64_t len = (int64_t)strlen(names[i]);
    wstr probe = { len, len, 1, 0, names[i] };
    const wstr *k = sym_find(&probe);
    if (!k) {
      wstr *s = (wstr *)str_new(names[i], len);
      s->sym = 1;
      sym_add(s);
      k = s;
    }
    keys[i] = k;
  }
}

// ---- the terminal (§12.2) ----

static const wstr *terminal_keys[2], *size_keys[2];

static int is_terminal(int fd) {
#ifdef _WIN32
  DWORD mode;
  return GetConsoleMode(GetStdHandle(fd == 0 ? STD_INPUT_HANDLE : STD_OUTPUT_HANDLE), &mode) != 0;
#else
  return isatty(fd);
#endif
}

static wv std_terminal(void) {
  static const char *const names[2] = { "input", "output" };
  rec_keys(names, 2, terminal_keys);
  wv vals[2] = { rt_b(is_terminal(0)), rt_b(is_terminal(1)) };
  return rt_rec(1, 2, terminal_keys, vals);
}

static wv std_terminalSize(void) {
  int64_t w = 0, h = 0;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
    w = info.srWindow.Right - info.srWindow.Left + 1;
    h = info.srWindow.Bottom - info.srWindow.Top + 1;
  }
#else
  struct winsize ws;
  if (isatty(1) && ioctl(1, TIOCGWINSZ, &ws) == 0) {
    w = ws.ws_col;
    h = ws.ws_row;
  }
#endif
  if (w <= 0 || h <= 0) return rt_std_fail("$terminalSize");
  static const char *const names[2] = { "width", "height" };
  rec_keys(names, 2, size_keys);
  wv vals[2] = { rt_i(w), rt_i(h) };
  return rt_rec(1, 2, size_keys, vals);
}

// ---- time and random numbers (§12.8) ----

static wv std_time(void) {
#ifdef _WIN32
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; // 100 ns units since 1601
  return rt_i((int64_t)(t / 10000) - 11644473600000LL);
#else
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return rt_i((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

// A generator is a mutable record { s0, s1, s2, s3 } of 32-bit words.
static const wstr *rng_keys[4];
static void rng_start(void) {
  static const char *const names[4] = { "s0", "s1", "s2", "s3" };
  rec_keys(names, 4, rng_keys);
}

// MurmurHash3's finalizer.
static uint32_t rng_mix(uint32_t z) {
  z ^= z >> 16;
  z *= 0x85EBCA6Bu;
  z ^= z >> 13;
  z *= 0xC2B2AE35u;
  z ^= z >> 16;
  return z;
}
static inline uint32_t rotl32(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

static wv std_newRandom(wv seed) {
  int64_t v = an_int(seed, "$newRandom");
  uint64_t a = v < 0 ? (uint64_t)-v : (uint64_t)v;
  uint32_t lo = (uint32_t)a, hi = (uint32_t)(a >> 32) | (v < 0 ? 1u << 21 : 0);
  uint32_t k = rng_mix(hi), c = lo;
  wv vals[4];
  for (int i = 0; i < 4; i++) {
    c += 0x9E3779B9u;
    vals[i] = rt_i(rng_mix(c) ^ k);
  }
  rng_start();
  return rt_rec(1, 4, rng_keys, vals);
}

// The four state words of a generator, in s0..s3 order; panics unless rng is one.
static void rng_words(wv rng, const char *fn, wv **s) {
  rng_start();
  int ok = rng.tag == T_REC && rng.r->n == 4;
  int64_t any = 0;
  for (int i = 0; ok && i < 4; i++) {
    s[i] = NULL;
    for (int j = 0; j < 4; j++) if (rng.r->keys[j] == rng_keys[i]) s[i] = &rng.r->vals[j];
    ok = s[i] && s[i]->tag == T_INT && s[i]->i >= 0 && s[i]->i <= 0xFFFFFFFFLL;
    if (ok) any |= s[i]->i;
  }
  if (!ok || !any) rt_fail("%s: the argument is not a random generator", fn);
  if (!rng.r->is_mut) rt_fail("random generator is frozen; hold it in a var or ref binding");
}

// The next word, by xoshiro128**.
static uint32_t rng_next(wv **s) {
  uint32_t s0 = (uint32_t)s[0]->i, s1 = (uint32_t)s[1]->i, s2 = (uint32_t)s[2]->i, s3 = (uint32_t)s[3]->i;
  uint32_t result = rotl32(s1 * 5, 7) * 9, t = s1 << 9;
  s2 ^= s0;
  s3 ^= s1;
  s1 ^= s2;
  s0 ^= s3;
  s2 ^= t;
  s3 = rotl32(s3, 11);
  *s[0] = rt_i(s0);
  *s[1] = rt_i(s1);
  *s[2] = rt_i(s2);
  *s[3] = rt_i(s3);
  return result;
}

static wv std_random(wv rng, wv n) {
  wv *s[4];
  rng_words(rng, "$random", s);
  int64_t m = an_int(n, "$random");
  if (m < 1 || m > 0x100000000LL) rt_fail("$random: n must be from 1 to 4294967296, got %lld", (long long)m);
  if (m == 0x100000000LL) return rt_i(rng_next(s));
  // Rejecting the top partial range keeps every result equally likely.
  uint64_t limit = 0x100000000ULL - 0x100000000ULL % (uint64_t)m;
  for (;;) {
    uint32_t x = rng_next(s);
    if (x < limit) return rt_i(x % (uint64_t)m);
  }
}
static wv std_randomFloat(wv rng) {
  wv *s[4];
  rng_words(rng, "$randomFloat", s);
  uint32_t a = rng_next(s) >> 5, b = rng_next(s) >> 6;
  return rt_f(((double)a * 67108864.0 + b) / 9007199254740992.0); // (a * 2^26 + b) / 2^53, exactly
}

// ---- startup ----

// The compiled program: library initialization, then the entry file's body.
static void wvt_main(void);

// Registers the globals of the compiled program as roots (rt_root).
static void rt_roots(int n, wv *const *globals) {
  for (int i = 0; i < n; i++) rt_root(globals[i], sizeof(wv));
}

// Runs the program. It is not inlined, so that all of its stack, and that of
// what it calls, is below the stack base its caller sets for the collector. (A
// frame pointer may point into the middle of its own frame.)
static __attribute__((noinline)) void rt_run(void) {
  // The runtime's own references to values.
  rt_root(&cache_s, sizeof cache_s);
  rt_root(&cache_off, sizeof cache_off);
  rt_root(&args_cache, sizeof args_cache);
  rt_root(&program_path_cache, sizeof program_path_cache);
  rt_root(rng_keys, sizeof rng_keys);
  rt_root(terminal_keys, sizeof terminal_keys);
  rt_root(size_keys, sizeof size_keys);
#ifdef _WIN32
  rt_root(&key_repeat, sizeof key_repeat);
#endif
  wvt_main();
  rt_exit_code(0);
}

#ifdef _WIN32
static DWORD WINAPI rt_thread(LPVOID arg) {
  gc_stack_base = __builtin_frame_address(0);
  (void)arg;
  rt_run();
  return 0;
}
#else
static void *rt_thread(void *arg) {
  gc_stack_base = __builtin_frame_address(0);
  (void)arg;
  rt_run();
  return NULL;
}
#endif

// The program runs on a thread with a large stack, since functions recurse deeply.
int main(int argc, char **argv) {
  clock_start = mono_ms();
  rt_argc = argc;
  rt_argv = argv;
  size_t stack = (size_t)1 << 30;
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  vt_start();
  HANDLE t = CreateThread(NULL, stack, rt_thread, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
  if (!t) { rt_thread(NULL); return 0; }
  WaitForSingleObject(t, INFINITE);
#else
  pthread_attr_t attr;
  pthread_t t;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stack);
  if (pthread_create(&t, &attr, rt_thread, NULL) != 0) { rt_thread(NULL); return 0; }
  pthread_join(t, NULL);
#endif
  return 0;
}

// ---- program ----

static const wstr S0 = { 1, 1, 1, 0, " " };
static const wstr S1 = { 1, 1, 1, 0, "\t" };
static const wstr S2 = { 1, 1, 1, 0, "\n" };
static const wstr S3 = { 1, 1, 1, 0, "\r" };
static const wstr S4 = { 1, 1, 1, 0, "0" };
static const wstr S5 = { 1, 1, 1, 0, "9" };
static const wstr S6 = { 1, 1, 1, 0, "a" };
static const wstr S7 = { 1, 1, 1, 0, "f" };
static const wstr S8 = { 1, 1, 1, 0, "A" };
static const wstr S9 = { 1, 1, 1, 0, "F" };
static const wstr S10 = { 1, 1, 1, 0, "z" };
static const wstr S11 = { 1, 1, 1, 0, "Z" };
static const wstr S12 = { 1, 1, 1, 0, "_" };
static const wstr S13 = { 0, 0, 1, 0, "" };
static const wstr S14 = { 1, 1, 1, 0, ";" };
static const wstr S15 = { 1, 1, 1, 0, "}" };
static const wstr S16 = { 1, 1, 1, 0, ")" };
static const wstr S17 = { 16, 16, 1, 0, "0123456789abcdef" };
static const wstr S18 = { 1, 1, 1, 0, "\"" };
static const wstr S19 = { 1, 1, 1, 0, "\\" };
static const wstr S20 = { 2, 2, 1, 0, "\\n" };
static const wstr S21 = { 2, 2, 1, 0, "\\r" };
static const wstr S22 = { 2, 2, 1, 0, "\\t" };
static const wstr S23 = { 2, 2, 1, 0, "\\b" };
static const wstr S24 = { 2, 2, 1, 0, "\\f" };
static const wstr S25 = { 4, 4, 1, 0, "\\u00" };
static const wstr S26 = { 7, 7, 1, 0, "warning" };
static const wstr S27 = { 5, 5, 1, 0, "error" };
static const wstr S28 = { 1, 1, 1, 0, ":" };
static const wstr S29 = { 2, 2, 1, 0, ": " };
static const wstr S30 = { 9, 9, 1, 0, "\n  hint: " };
static const wstr S31 = { 5, 5, 1, 0, "\n    " };
static const wstr S32 = { 1, 1, 1, 0, "^" };
static const wstr S33 = { 2, 2, 1, 0, "fn" };
static const wstr S34 = { 6, 6, 1, 0, "return" };
static const wstr S35 = { 5, 5, 1, 0, "const" };
static const wstr S36 = { 3, 3, 1, 0, "var" };
static const wstr S37 = { 3, 3, 1, 0, "ref" };
static const wstr S38 = { 3, 3, 1, 0, "set" };
static const wstr S39 = { 2, 2, 1, 0, "if" };
static const wstr S40 = { 4, 4, 1, 0, "else" };
static const wstr S41 = { 5, 5, 1, 0, "while" };
static const wstr S42 = { 3, 3, 1, 0, "for" };
static const wstr S43 = { 5, 5, 1, 0, "break" };
static const wstr S44 = { 8, 8, 1, 0, "continue" };
static const wstr S45 = { 5, 5, 1, 0, "match" };
static const wstr S46 = { 4, 4, 1, 0, "case" };
static const wstr S47 = { 4, 4, 1, 0, "enum" };
static const wstr S48 = { 6, 6, 1, 0, "import" };
static const wstr S49 = { 6, 6, 1, 0, "export" };
static const wstr S50 = { 4, 4, 1, 0, "true" };
static const wstr S51 = { 5, 5, 1, 0, "false" };
static const wstr S52 = { 4, 4, 1, 0, "null" };
static const wstr S53 = { 3, 3, 1, 0, "nan" };
static const wstr S54 = { 3, 3, 1, 0, "inf" };
static const wstr S55 = { 4, 4, 1, 0, "exit" };
static const wstr S56 = { 4, 4, 1, 0, "fail" };
static const wstr S57 = { 6, 6, 1, 0, "onfail" };
static const wstr S58 = { 5, 5, 1, 0, "panic" };
static const wstr S59 = { 5, 5, 1, 0, "fails" };
static const wstr S60 = { 79, 79, 1, 0, "'fails' is reserved for marking functions that can fail; it is not designed yet" };
static const wstr S61 = { 7, 7, 1, 0, "recover" };
static const wstr S62 = { 58, 58, 1, 0, "'recover' is reserved: catching panics is not designed yet" };
static const wstr S63 = { 7, 7, 1, 0, "finally" };
static const wstr S64 = { 58, 58, 1, 0, "'finally' is reserved: catching panics is not designed yet" };
static const wstr S65 = { 3, 3, 1, 0, "**=" };
static const wstr S66 = { 3, 3, 1, 0, "<<=" };
static const wstr S67 = { 3, 3, 1, 0, ">>=" };
static const wstr S68 = { 2, 2, 1, 0, "+=" };
static const wstr S69 = { 2, 2, 1, 0, "-=" };
static const wstr S70 = { 2, 2, 1, 0, "*=" };
static const wstr S71 = { 2, 2, 1, 0, "/=" };
static const wstr S72 = { 2, 2, 1, 0, "%=" };
static const wstr S73 = { 2, 2, 1, 0, "&=" };
static const wstr S74 = { 2, 2, 1, 0, "|=" };
static const wstr S75 = { 2, 2, 1, 0, "^=" };
static const wstr S76 = { 2, 2, 1, 0, "**" };
static const wstr S77 = { 2, 2, 1, 0, "==" };
static const wstr S78 = { 2, 2, 1, 0, "!=" };
static const wstr S79 = { 2, 2, 1, 0, "<=" };
static const wstr S80 = { 2, 2, 1, 0, ">=" };
static const wstr S81 = { 2, 2, 1, 0, "&&" };
static const wstr S82 = { 2, 2, 1, 0, "||" };
static const wstr S83 = { 2, 2, 1, 0, "<<" };
static const wstr S84 = { 2, 2, 1, 0, ">>" };
static const wstr S85 = { 1, 1, 1, 0, "+" };
static const wstr S86 = { 1, 1, 1, 0, "-" };
static const wstr S87 = { 1, 1, 1, 0, "*" };
static const wstr S88 = { 1, 1, 1, 0, "/" };
static const wstr S89 = { 1, 1, 1, 0, "%" };
static const wstr S90 = { 1, 1, 1, 0, "<" };
static const wstr S91 = { 1, 1, 1, 0, ">" };
static const wstr S92 = { 1, 1, 1, 0, "&" };
static const wstr S93 = { 1, 1, 1, 0, "|" };
static const wstr S94 = { 1, 1, 1, 0, "~" };
static const wstr S95 = { 1, 1, 1, 0, "!" };
static const wstr S96 = { 1, 1, 1, 0, "?" };
static const wstr S97 = { 1, 1, 1, 0, "," };
static const wstr S98 = { 1, 1, 1, 0, "(" };
static const wstr S99 = { 1, 1, 1, 0, "[" };
static const wstr S100 = { 1, 1, 1, 0, "]" };
static const wstr S101 = { 1, 1, 1, 0, "{" };
static const wstr S102 = { 1, 1, 1, 0, "1" };
static const wstr S103 = { 1, 1, 1, 0, "n" };
static const wstr S104 = { 1, 1, 1, 0, "r" };
static const wstr S105 = { 1, 1, 1, 0, "t" };
static const wstr S106 = { 1, 1, 1, 0, "$" };
static const wstr S107 = { 62, 62, 1, 0, "'//' starts a comment; Wavert has no integer-division operator" };
static const wstr S108 = { 42, 42, 1, 0, "for integer division write (a / b)($floor)" };
static const wstr S109 = { 2, 2, 1, 0, "*/" };
static const wstr S110 = { 26, 26, 1, 0, "unterminated block comment" };
static const wstr S111 = { 3, 3, 1, 0, "eof" };
static const wstr S112 = { 5, 5, 1, 0, "opset" };
static const wstr S113 = { 34, 34, 1, 0, "no whitespace is allowed inside '!" };
static const wstr S114 = { 1, 1, 1, 0, "'" };
static const wstr S115 = { 8, 8, 1, 0, "write '!" };
static const wstr S116 = { 3, 3, 1, 0, ">>>" };
static const wstr S117 = { 26, 26, 1, 0, "there is no '>>>' operator" };
static const wstr S118 = { 47, 47, 1, 0, "for an unsigned interpretation use x($toUint32)" };
static const wstr S119 = { 2, 2, 1, 0, "--" };
static const wstr S120 = { 23, 23, 1, 0, "'--' is not an operator" };
static const wstr S121 = { 74, 74, 1, 0, "write '- -x' or '-(-x)' for double negation, or ';set x -= 1' to decrement" };
static const wstr S122 = { 2, 2, 1, 0, "++" };
static const wstr S123 = { 23, 23, 1, 0, "'++' is not an operator" };
static const wstr S124 = { 32, 32, 1, 0, "write ';set x += 1' to increment" };
static const wstr S125 = { 59, 59, 1, 0, "';;': likely a habitual trailing ';' from C-style languages" };
static const wstr S126 = { 61, 61, 1, 0, "Wavert statements begin with ;, they are not terminated by it" };
static const wstr S127 = { 1, 1, 1, 0, "=" };
static const wstr S128 = { 22, 22, 1, 0, "'=' is not an operator" };
static const wstr S129 = { 86, 86, 1, 0, "declare with ';const x is value', assign with ';set x to value', and compare with '=='" };
static const wstr S130 = { 1, 1, 1, 0, "." };
static const wstr S131 = { 2, 2, 1, 0, "//" };
static const wstr S132 = { 2, 2, 1, 0, "/*" };
static const wstr S133 = { 35, 35, 1, 0, "no whitespace is allowed around '.'" };
static const wstr S134 = { 53, 53, 1, 0, "member access is written record.field, e.g. node.kind" };
static const wstr S135 = { 22, 22, 1, 0, "unexpected character '" };
static const wstr S136 = { 32, 32, 1, 0, "'$' cannot appear in identifiers" };
static const wstr S137 = { 66, 66, 1, 0, "'$' is reserved for the standard library; user names have no sigil" };
static const wstr S138 = { 2, 2, 1, 0, "id" };
static const wstr S139 = { 3, 3, 1, 0, "int" };
static const wstr S140 = { 1, 1, 1, 0, "x" };
static const wstr S141 = { 1, 1, 1, 0, "b" };
static const wstr S142 = { 22, 22, 1, 0, "' must be followed by " };
static const wstr S143 = { 6, 6, 1, 0, "binary" };
static const wstr S144 = { 3, 3, 1, 0, "hex" };
static const wstr S145 = { 7, 7, 1, 0, " digits" };
static const wstr S146 = { 40, 40, 1, 0, "digits are required on both sides of '.'" };
static const wstr S147 = { 6, 6, 1, 0, "write " };
static const wstr S148 = { 2, 2, 1, 0, ".0" };
static const wstr S149 = { 5, 5, 1, 0, "float" };
static const wstr S150 = { 1, 1, 1, 0, "e" };
static const wstr S151 = { 1, 1, 1, 0, "E" };
static const wstr S152 = { 37, 37, 1, 0, "an exponent needs digits: 1e9, 2.5e-3" };
static const wstr S153 = { 26, 26, 1, 0, "' is not a numeric literal" };
static const wstr S154 = { 29, 29, 1, 0, "float literal is out of range" };
static const wstr S155 = { 45, 45, 1, 0, "integer literal exceeds the 53-bit safe range" };
static const wstr S156 = { 59, 59, 1, 0, "'$' must be immediately followed by a standard library name" };
static const wstr S157 = { 3, 3, 1, 0, "std" };
static const wstr S158 = { 4, 4, 1, 0, "from" };
static const wstr S159 = { 19, 19, 1, 0, "unterminated string" };
static const wstr S160 = { 27, 27, 1, 0, "raw newline inside a string" };
static const wstr S161 = { 10, 10, 1, 0, "write '\\n'" };
static const wstr S162 = { 1, 1, 1, 0, "u" };
static const wstr S163 = { 22, 22, 1, 0, "invalid \\u{...} escape" };
static const wstr S164 = { 69, 69, 1, 0, "write 1 to 6 hex digits naming a Unicode scalar value, e.g. \\u{1F608}" };
static const wstr S165 = { 17, 17, 1, 0, "unknown escape '\\" };
static const wstr S166 = { 43, 43, 1, 0, "valid escapes are \\n \\r \\t \\\\ \\\" \\$ \\u{...}" };
static const wstr S167 = { 37, 37, 1, 0, "Wavert interpolation is {x}, not ${x}" };
static const wstr S168 = { 23, 23, 1, 0, "unmatched '}' in string" };
static const wstr S169 = { 30, 30, 1, 0, "write '}}' for a literal brace" };
static const wstr S170 = { 14, 14, 1, 0, "did you mean {" };
static const wstr S171 = { 2, 2, 1, 0, "}?" };
static const wstr S172 = { 53, 53, 1, 0, "Wavert interpolation is {x}; write \\$ for a literal $" };
static const wstr S173 = { 3, 3, 1, 0, "str" };
static const wstr S174 = { 36, 36, 1, 0, "unterminated interpolation in string" };
static const wstr S175 = { 19, 19, 1, 0, "empty interpolation" };
static const wstr S176 = { 30, 30, 1, 0, "write '{{' for a literal brace" };
static const wstr S177 = { 3, 3, 1, 0, "!if" };
static const wstr S178 = { 4, 4, 1, 0, "expr" };
static const wstr S179 = { 4, 4, 1, 0, "decl" };
static const wstr S180 = { 12, 12, 1, 0, "end of input" };
static const wstr S181 = { 2, 2, 1, 0, "'$" };
static const wstr S182 = { 8, 8, 1, 0, "a string" };
static const wstr S183 = { 2, 2, 1, 0, "='" };
static const wstr S184 = { 4, 4, 1, 0, "call" };
static const wstr S185 = { 8, 8, 1, 0, "failWith" };
static const wstr S186 = { 5, 5, 1, 0, "chain" };
static const wstr S187 = { 6, 6, 1, 0, "string" };
static const wstr S188 = { 3, 3, 1, 0, "arr" };
static const wstr S189 = { 3, 3, 1, 0, "map" };
static const wstr S190 = { 3, 3, 1, 0, "rec" };
static const wstr S191 = { 3, 3, 1, 0, "idx" };
static const wstr S192 = { 3, 3, 1, 0, "mem" };
static const wstr S193 = { 2, 2, 1, 0, "un" };
static const wstr S194 = { 3, 3, 1, 0, "bin" };
static const wstr S195 = { 4, 4, 1, 0, "tern" };
static const wstr S196 = { 10, 10, 1, 0, "expected '" };
static const wstr S197 = { 8, 8, 1, 0, ", found " };
static const wstr S198 = { 9, 9, 1, 0, "', found " };
static const wstr S199 = { 27, 27, 1, 0, "missing whitespace around '" };
static const wstr S200 = { 79, 79, 1, 0, "calls are postfix: write the arguments first, then the function, e.g. (a, b)(f)" };
static const wstr S201 = { 36, 36, 1, 0, "every Wavert statement begins with ;" };
static const wstr S202 = { 41, 41, 1, 0, "expected ';' to start a statement, found " };
static const wstr S203 = { 5, 5, 1, 0, "empty" };
static const wstr S204 = { 46, 46, 1, 0, "';end' must be the last statement of a library" };
static const wstr S205 = { 47, 47, 1, 0, "'import' must come before every other statement" };
static const wstr S206 = { 3, 3, 1, 0, "end" };
static const wstr S207 = { 59, 59, 1, 0, "'export' is only allowed in a library, which ends with ;end" };
static const wstr S208 = { 62, 62, 1, 0, "'onfail' must follow a statement whose value comes from a call" };
static const wstr S209 = { 33, 33, 1, 0, "expected a name for the fail code" };
static const wstr S210 = { 6, 6, 1, 0, "!while" };
static const wstr S211 = { 36, 36, 1, 0, "'else' without a preceding if or !if" };
static const wstr S212 = { 22, 22, 1, 0, "'case' outside a match" };
static const wstr S213 = { 2, 2, 1, 0, "to" };
static const wstr S214 = { 4, 4, 1, 0, "name" };
static const wstr S215 = { 31, 31, 1, 0, "an assignment starts with 'set'" };
static const wstr S216 = { 15, 15, 1, 0, ";set x to value" };
static const wstr S217 = { 54, 54, 1, 0, "to make it the block's value, remove the ';' before it" };
static const wstr S218 = { 28, 28, 1, 0, "this statement has no effect" };
static const wstr S219 = { 23, 23, 1, 0, "expected a name after '" };
static const wstr S220 = { 14, 14, 1, 0, " name is value" };
static const wstr S221 = { 2, 2, 1, 0, "is" };
static const wstr S222 = { 9, 9, 1, 0, " is value" };
static const wstr S223 = { 76, 76, 1, 0, "expected 'to' or a compound assignment such as '+=' after the target, found " };
static const wstr S224 = { 50, 50, 1, 0, ";for (x in xs) { ... } or ;for (i in 0, n) { ... }" };
static const wstr S225 = { 24, 24, 1, 0, "expected '(' after 'for'" };
static const wstr S226 = { 44, 44, 1, 0, "' is a keyword and cannot be a loop variable" };
static const wstr S227 = { 45, 45, 1, 0, "expected a name for the loop variable, found " };
static const wstr S228 = { 2, 2, 1, 0, "in" };
static const wstr S229 = { 40, 40, 1, 0, "a for loop's step must be greater than 0" };
static const wstr S230 = { 57, 57, 1, 0, "to count down, add reversed: ;for (i in 0, n, 2 reversed)" };
static const wstr S231 = { 58, 58, 1, 0, "a range gives one value on each pass, so it takes one name" };
static const wstr S232 = { 6, 6, 1, 0, ";for (" };
static const wstr S233 = { 17, 17, 1, 0, " in a, b) { ... }" };
static const wstr S234 = { 8, 8, 1, 0, "reversed" };
static const wstr S235 = { 23, 23, 1, 0, ";for (x in xs reversed)" };
static const wstr S236 = { 25, 25, 1, 0, ";for (i in 0, n reversed)" };
static const wstr S237 = { 33, 33, 1, 0, "to go backwards, write reversed: " };
static const wstr S238 = { 42, 42, 1, 0, "expected ')' to close the for loop, found " };
static const wstr S239 = { 35, 35, 1, 0, "a fail code must be an integer >= 1" };
static const wstr S240 = { 51, 51, 1, 0, "'import' is only allowed at the top level of a file" };
static const wstr S241 = { 45, 45, 1, 0, "expected a name for the module after 'import'" };
static const wstr S242 = { 26, 26, 1, 0, ";import lib from \"lib.wvt\"" };
static const wstr S243 = { 8, 8, 1, 0, ";import " };
static const wstr S244 = { 7, 7, 1, 0, " from \"" };
static const wstr S245 = { 5, 5, 1, 0, ".wvt\"" };
static const wstr S246 = { 45, 45, 1, 0, "an import path must be a plain string literal" };
static const wstr S247 = { 37, 37, 1, 0, "import paths use '/' as the separator" };
static const wstr S248 = { 4, 4, 1, 0, ".wvt" };
static const wstr S249 = { 42, 42, 1, 0, "an import path includes the .wvt extension" };
static const wstr S250 = { 5, 5, 1, 0, "$lib/" };
static const wstr S251 = { 81, 81, 1, 0, "an import path that starts with '$' must start with '$lib/', the standard library" };
static const wstr S252 = { 68, 68, 1, 0, "a file of your own is imported by a relative path, such as \"lib.wvt\"" };
static const wstr S253 = { 2, 2, 1, 0, ".." };
static const wstr S254 = { 42, 42, 1, 0, "a '$lib/' path cannot leave lib/ with '..'" };
static const wstr S255 = { 43, 43, 1, 0, "only top-level declarations can be exported" };
static const wstr S256 = { 70, 70, 1, 0, "only fn, const, var, ref and enum declarations can be exported, found " };
static const wstr S257 = { 52, 52, 1, 0, "';end' is only allowed at the top level of a library" };
static const wstr S258 = { 46, 46, 1, 0, "a library must export at least one declaration" };
static const wstr S259 = { 55, 55, 1, 0, "mark one with 'export', e.g. ;export fn name(x) { ... }" };
static const wstr S260 = { 36, 36, 1, 0, "the name comes before the parameters" };
static const wstr S261 = { 22, 22, 1, 0, ";fn name(a, b) { ... }" };
static const wstr S262 = { 37, 37, 1, 0, "expected the function name after 'fn'" };
static const wstr S263 = { 39, 39, 1, 0, "expected '(' to open the parameter list" };
static const wstr S264 = { 4, 4, 1, 0, ";fn " };
static const wstr S265 = { 14, 14, 1, 0, "(a, b) { ... }" };
static const wstr S266 = { 25, 25, 1, 0, "expected a parameter name" };
static const wstr S267 = { 43, 43, 1, 0, "a parameter is a name, var name or ref name" };
static const wstr S268 = { 40, 40, 1, 0, "expected ')' to close the parameter list" };
static const wstr S269 = { 45, 45, 1, 0, "an enum can only be declared at the top level" };
static const wstr S270 = { 33, 33, 1, 0, "the name comes before the members" };
static const wstr S271 = { 17, 17, 1, 0, ";enum Name (a, b)" };
static const wstr S272 = { 35, 35, 1, 0, "expected the enum name after 'enum'" };
static const wstr S273 = { 36, 36, 1, 0, "expected '(' to open the member list" };
static const wstr S274 = { 6, 6, 1, 0, ";enum " };
static const wstr S275 = { 7, 7, 1, 0, " (a, b)" };
static const wstr S276 = { 33, 33, 1, 0, "an enum needs at least one member" };
static const wstr S277 = { 42, 42, 1, 0, "' is a keyword and cannot be a member name" };
static const wstr S278 = { 30, 30, 1, 0, "expected a member name, found " };
static const wstr S279 = { 18, 18, 1, 0, "duplicate member '" };
static const wstr S280 = { 9, 9, 1, 0, "' in enum" };
static const wstr S281 = { 38, 38, 1, 0, "expected ',' or ')' in the member list" };
static const wstr S282 = { 20, 20, 1, 0, "expected '(' after '" };
static const wstr S283 = { 30, 30, 1, 0, "conditions are parenthesized: " };
static const wstr S284 = { 12, 12, 1, 0, " (x) { ... }" };
static const wstr S285 = { 35, 35, 1, 0, "expected ')' to close the condition" };
static const wstr S286 = { 12, 12, 1, 0, "expected '{'" };
static const wstr S287 = { 32, 32, 1, 0, "unterminated block: expected '}'" };
static const wstr S288 = { 68, 68, 1, 0, "a record as a block's value needs its own braces: { { key: value } }" };
static const wstr S289 = { 75, 75, 1, 0, "an expression without a leading ';' is the block's value and must come last" };
static const wstr S290 = { 44, 44, 1, 0, "expected '}' after the block's value, found " };
static const wstr S291 = { 53, 53, 1, 0, "expected a name, an indexed element or a field to set" };
static const wstr S292 = { 38, 38, 1, 0, "expected ':' in conditional expression" };
static const wstr S293 = { 22, 22, 1, 0, "'-x ** y' is ambiguous" };
static const wstr S294 = { 28, 28, 1, 0, "write -(x ** y) or (-x) ** y" };
static const wstr S295 = { 59, 59, 1, 0, "'[' at the start of a line indexes the expression before it" };
static const wstr S296 = { 71, 71, 1, 0, "if it starts the block's value, wrap the value in parentheses: ([a, b])" };
static const wstr S297 = { 31, 31, 1, 0, "expected ']' to close the index" };
static const wstr S298 = { 41, 41, 1, 0, "' is a keyword and cannot be a field name" };
static const wstr S299 = { 39, 39, 1, 0, "expected a field name after '.', found " };
static const wstr S300 = { 4, 4, 1, 0, "bool" };
static const wstr S301 = { 17, 17, 1, 0, "empty parentheses" };
static const wstr S302 = { 39, 39, 1, 0, "a call with no arguments is written (f)" };
static const wstr S303 = { 34, 34, 1, 0, "trailing comma in an argument list" };
static const wstr S304 = { 40, 40, 1, 0, "expected ',' or ')' in the argument list" };
static const wstr S305 = { 69, 69, 1, 0, "a parenthesized list must be followed by the function it is passed to" };
static const wstr S306 = { 30, 30, 1, 0, "(a, b)(f) calls f with a and b" };
static const wstr S307 = { 23, 23, 1, 0, "expected ']' after '[:'" };
static const wstr S308 = { 27, 27, 1, 0, "an empty map is written [:]" };
static const wstr S309 = { 31, 31, 1, 0, "trailing comma in array literal" };
static const wstr S310 = { 32, 32, 1, 0, "expected ',' or ']' in the array" };
static const wstr S311 = { 12, 12, 1, 0, "unexpected '" };
static const wstr S312 = { 46, 46, 1, 0, "declarations and assignments are statements: ;" };
static const wstr S313 = { 4, 4, 1, 0, " ..." };
static const wstr S314 = { 21, 21, 1, 0, "'fail' is a statement" };
static const wstr S315 = { 25, 25, 1, 0, ";fail c fails with code c" };
static const wstr S316 = { 37, 37, 1, 0, "an assignment is a statement: ;set x " };
static const wstr S317 = { 7, 7, 1, 0, "= value" };
static const wstr S318 = { 30, 30, 1, 0, "expected an expression, found " };
static const wstr S319 = { 29, 29, 1, 0, "expected a field name, found " };
static const wstr S320 = { 47, 47, 1, 0, "record literals are written { key: value, ... }" };
static const wstr S321 = { 17, 17, 1, 0, "duplicate field '" };
static const wstr S322 = { 19, 19, 1, 0, "' in record literal" };
static const wstr S323 = { 29, 29, 1, 0, "there is no shorthand; write " };
static const wstr S324 = { 31, 31, 1, 0, "expected ':' after field name '" };
static const wstr S325 = { 32, 32, 1, 0, "trailing comma in record literal" };
static const wstr S326 = { 37, 37, 1, 0, "expected ',' or '}' in record literal" };
static const wstr S327 = { 1, 1, 1, 0, "i" };
static const wstr S328 = { 1, 1, 1, 0, "s" };
static const wstr S329 = { 73, 73, 1, 0, "a map key must be an int, a string, a boolean, an enum member or a record" };
static const wstr S330 = { 28, 28, 1, 0, "duplicate key in map literal" };
static const wstr S331 = { 43, 43, 1, 0, "expected ':' after a key in the map literal" };
static const wstr S332 = { 42, 42, 1, 0, "map literals are written [key: value, ...]" };
static const wstr S333 = { 29, 29, 1, 0, "trailing comma in map literal" };
static const wstr S334 = { 38, 38, 1, 0, "expected ',' or ']' in the map literal" };
static const wstr S335 = { 38, 38, 1, 0, "expected '{' after the match's subject" };
static const wstr S336 = { 32, 32, 1, 0, "'else' must come last in a match" };
static const wstr S337 = { 31, 31, 1, 0, "a case needs at least one value" };
static const wstr S338 = { 21, 21, 1, 0, "case \"a\", \"b\" { ... }" };
static const wstr S339 = { 27, 27, 1, 0, "a match has only one 'else'" };
static const wstr S340 = { 45, 45, 1, 0, "a match needs at least one case before 'else'" };
static const wstr S341 = { 31, 31, 1, 0, "a match needs at least one case" };
static const wstr S342 = { 30, 30, 1, 0, "match (x) { case \"a\" { ... } }" };
static const wstr S343 = { 41, 41, 1, 0, "expected 'case', 'else' or '}' in a match" };
static const wstr S344 = { 35, 35, 1, 0, "a case value cannot be interpolated" };
static const wstr S345 = { 28, 28, 1, 0, "floats cannot be case values" };
static const wstr S346 = { 58, 58, 1, 0, "float equality makes a poor match; compare with if instead" };
static const wstr S347 = { 48, 48, 1, 0, "a case value must be a literal or an enum member" };
static const wstr S348 = { 67, 67, 1, 0, "case values are integers, strings, true, false, null or Enum.member" };
static const wstr S349 = { 35, 35, 1, 0, "'!if (a != b)' is a double negative" };
static const wstr S350 = { 19, 19, 1, 0, "write 'if (a == b)'" };
static const wstr S351 = { 11, 11, 1, 0, "unexpected " };
static const wstr S352 = { 17, 17, 1, 0, " in interpolation" };
static const wstr S353 = { 5, 5, 1, 0, "print" };
static const wstr S354 = { 5, 5, 1, 0, "write" };
static const wstr S355 = { 6, 6, 1, 0, "eprint" };
static const wstr S356 = { 8, 8, 1, 0, "readLine" };
static const wstr S357 = { 7, 7, 1, 0, "readKey" };
static const wstr S358 = { 7, 7, 1, 0, "waitKey" };
static const wstr S359 = { 8, 8, 1, 0, "terminal" };
static const wstr S360 = { 12, 12, 1, 0, "terminalSize" };
static const wstr S361 = { 8, 8, 1, 0, "readFile" };
static const wstr S362 = { 9, 9, 1, 0, "writeFile" };
static const wstr S363 = { 10, 10, 1, 0, "appendFile" };
static const wstr S364 = { 10, 10, 1, 0, "fileExists" };
static const wstr S365 = { 7, 7, 1, 0, "listDir" };
static const wstr S366 = { 10, 10, 1, 0, "deleteFile" };
static const wstr S367 = { 4, 4, 1, 0, "args" };
static const wstr S368 = { 3, 3, 1, 0, "env" };
static const wstr S369 = { 11, 11, 1, 0, "programPath" };
static const wstr S370 = { 3, 3, 1, 0, "run" };
static const wstr S371 = { 6, 6, 1, 0, "length" };
static const wstr S372 = { 2, 2, 1, 0, "at" };
static const wstr S373 = { 5, 5, 1, 0, "slice" };
static const wstr S374 = { 4, 4, 1, 0, "code" };
static const wstr S375 = { 8, 8, 1, 0, "fromCode" };
static const wstr S376 = { 5, 5, 1, 0, "split" };
static const wstr S377 = { 4, 4, 1, 0, "join" };
static const wstr S378 = { 8, 8, 1, 0, "toString" };
static const wstr S379 = { 5, 5, 1, 0, "toInt" };
static const wstr S380 = { 7, 7, 1, 0, "toFloat" };
static const wstr S381 = { 4, 4, 1, 0, "push" };
static const wstr S382 = { 3, 3, 1, 0, "pop" };
static const wstr S383 = { 4, 4, 1, 0, "same" };
static const wstr S384 = { 4, 4, 1, 0, "sort" };
static const wstr S385 = { 6, 6, 1, 0, "sortBy" };
static const wstr S386 = { 7, 7, 1, 0, "reverse" };
static const wstr S387 = { 3, 3, 1, 0, "get" };
static const wstr S388 = { 3, 3, 1, 0, "has" };
static const wstr S389 = { 6, 6, 1, 0, "remove" };
static const wstr S390 = { 4, 4, 1, 0, "keys" };
static const wstr S391 = { 6, 6, 1, 0, "values" };
static const wstr S392 = { 6, 6, 1, 0, "typeOf" };
static const wstr S393 = { 6, 6, 1, 0, "fields" };
static const wstr S394 = { 5, 5, 1, 0, "floor" };
static const wstr S395 = { 4, 4, 1, 0, "ceil" };
static const wstr S396 = { 5, 5, 1, 0, "round" };
static const wstr S397 = { 5, 5, 1, 0, "trunc" };
static const wstr S398 = { 8, 8, 1, 0, "toUint32" };
static const wstr S399 = { 5, 5, 1, 0, "isNan" };
static const wstr S400 = { 4, 4, 1, 0, "time" };
static const wstr S401 = { 7, 7, 1, 0, "elapsed" };
static const wstr S402 = { 5, 5, 1, 0, "sleep" };
static const wstr S403 = { 9, 9, 1, 0, "newRandom" };
static const wstr S404 = { 6, 6, 1, 0, "random" };
static const wstr S405 = { 11, 11, 1, 0, "randomFloat" };
static const wstr S406 = { 9, 9, 1, 0, " function" };
static const wstr S407 = { 6, 6, 1, 0, "n enum" };
static const wstr S408 = { 9, 9, 1, 0, " argument" };
static const wstr S409 = { 4, 4, 1, 0, "str:" };
static const wstr S410 = { 4, 4, 1, 0, "mem:" };
static const wstr S411 = { 55, 55, 1, 0, "this block's value is used, but it does not produce one" };
static const wstr S412 = { 62, 62, 1, 0, "end the block with its value, or with null if that is intended" };
static const wstr S413 = { 42, 42, 1, 0, "' is already declared in this scope (line " };
static const wstr S414 = { 11, 11, 1, 0, "' shadows '" };
static const wstr S415 = { 32, 32, 1, 0, "' from an enclosing scope (line " };
static const wstr S416 = { 45, 45, 1, 0, "Wavert forbids shadowing; choose another name" };
static const wstr S417 = { 14, 14, 1, 0, "unknown name '" };
static const wstr S418 = { 39, 39, 1, 0, "' is used before its declaration (line " };
static const wstr S419 = { 5, 5, 1, 0, "param" };
static const wstr S420 = { 27, 27, 1, 0, "'return' outside a function" };
static const wstr S421 = { 16, 16, 1, 0, "' outside a loop" };
static const wstr S422 = { 56, 56, 1, 0, "'fail' outside a function: there is no caller to fail to" };
static const wstr S423 = { 4, 4, 1, 0, "loop" };
static const wstr S424 = { 2, 2, 1, 0, "it" };
static const wstr S425 = { 12, 12, 1, 0, "cannot set '" };
static const wstr S426 = { 25, 25, 1, 0, "' inside a for loop over " };
static const wstr S427 = { 50, 50, 1, 0, "build the new value in another variable, and set '" };
static const wstr S428 = { 22, 22, 1, 0, "' to it after the loop" };
static const wstr S429 = { 65, 65, 1, 0, "'onfail' handles a call, but this statement's value is not a call" };
static const wstr S430 = { 87, 87, 1, 0, "the call must be the outermost expression, e.g. ;const n is s(parse) onfail (c) { ... }" };
static const wstr S431 = { 44, 44, 1, 0, "a match cannot mix literals and enum members" };
static const wstr S432 = { 31, 31, 1, 0, "a match cannot mix members of '" };
static const wstr S433 = { 7, 7, 1, 0, "' and '" };
static const wstr S434 = { 21, 21, 1, 0, "duplicate case value " };
static const wstr S435 = { 7, 7, 1, 0, " (line " };
static const wstr S436 = { 22, 22, 1, 0, "match does not handle " };
static const wstr S437 = { 2, 2, 1, 0, ", " };
static const wstr S438 = { 57, 57, 1, 0, "add a case for each, or end the match with 'else { ... }'" };
static const wstr S439 = { 52, 52, 1, 0, "a match used as a value must end with 'else { ... }'" };
static const wstr S440 = { 6, 6, 1, 0, "module" };
static const wstr S441 = { 19, 19, 1, 0, "' does not export '" };
static const wstr S442 = { 17, 17, 1, 0, "' is not a module" };
static const wstr S443 = { 16, 16, 1, 0, "unknown module '" };
static const wstr S444 = { 41, 41, 1, 0, "' is a function; functions are not values" };
static const wstr S445 = { 20, 20, 1, 0, "to call it, write x(" };
static const wstr S446 = { 7, 7, 1, 0, "), or (" };
static const wstr S447 = { 19, 19, 1, 0, ") with no arguments" };
static const wstr S448 = { 25, 25, 1, 0, "' is an enum, not a value" };
static const wstr S449 = { 24, 24, 1, 0, "its members are written " };
static const wstr S450 = { 7, 7, 1, 0, ".member" };
static const wstr S451 = { 54, 54, 1, 0, "case values are literals or enum members (Enum.member)" };
static const wstr S452 = { 16, 16, 1, 0, "' is not an enum" };
static const wstr S453 = { 14, 14, 1, 0, "unknown enum '" };
static const wstr S454 = { 11, 11, 1, 0, "no member '" };
static const wstr S455 = { 11, 11, 1, 0, "' in enum '" };
static const wstr S456 = { 16, 16, 1, 0, "its members are " };
static const wstr S457 = { 66, 66, 1, 0, "a conditional used as a value must end with a plain 'else { ... }'" };
static const wstr S458 = { 25, 25, 1, 0, "': it is not a var or ref" };
static const wstr S459 = { 7, 7, 1, 0, "a field" };
static const wstr S460 = { 10, 10, 1, 0, "an element" };
static const wstr S461 = { 29, 29, 1, 0, "': enum members are constants" };
static const wstr S462 = { 9, 9, 1, 0, "': only '" };
static const wstr S463 = { 12, 12, 1, 0, "' may set it" };
static const wstr S464 = { 14, 14, 1, 0, "cannot modify " };
static const wstr S465 = { 5, 5, 1, 0, " of '" };
static const wstr S466 = { 18, 18, 1, 0, "': it is not a var" };
static const wstr S467 = { 6, 6, 1, 0, "' is a" };
static const wstr S468 = { 15, 15, 1, 0, ", not a binding" };
static const wstr S469 = { 38, 38, 1, 0, "': a for loop's variable cannot be set" };
static const wstr S470 = { 121, 121, 1, 0, "to replace an element, set it through its array, such as ;set xs[i] to v; to control a counter yourself, use a while loop" };
static const wstr S471 = { 24, 24, 1, 0, "mark the parameter (ref " };
static const wstr S472 = { 24, 24, 1, 0, ") to rebind it, or (var " };
static const wstr S473 = { 20, 20, 1, 0, ") for a mutable copy" };
static const wstr S474 = { 29, 29, 1, 0, "mark the parameter var: (var " };
static const wstr S475 = { 22, 22, 1, 0, "declare it with ';ref " };
static const wstr S476 = { 32, 32, 1, 0, " is ...' to rebind it, or ';var " };
static const wstr S477 = { 27, 27, 1, 0, " is ...' for a mutable copy" };
static const wstr S478 = { 22, 22, 1, 0, "declare it with ';var " };
static const wstr S479 = { 8, 8, 1, 0, " is ...'" };
static const wstr S480 = { 32, 32, 1, 0, "unknown standard library name '$" };
static const wstr S481 = { 26, 26, 1, 0, "' is a module, not a value" };
static const wstr S482 = { 27, 27, 1, 0, "what it exports is written " };
static const wstr S483 = { 5, 5, 1, 0, ".name" };
static const wstr S484 = { 32, 32, 1, 0, "' is a function; call it with ($" };
static const wstr S485 = { 30, 30, 1, 0, "'fail' is a statement: ;fail c" };
static const wstr S486 = { 24, 24, 1, 0, "checker: unhandled node " };
static const wstr S487 = { 31, 31, 1, 0, "' is a constant, not a function" };
static const wstr S488 = { 8, 8, 1, 0, "' takes " };
static const wstr S489 = { 6, 6, 1, 0, ", got " };
static const wstr S490 = { 18, 18, 1, 0, "unknown function '" };
static const wstr S491 = { 71, 71, 1, 0, ") is a call with no arguments; to use the value, remove the parentheses" };
static const wstr S492 = { 53, 53, 1, 0, "calls are postfix: the function comes last, e.g. x(f)" };
static const wstr S493 = { 19, 19, 1, 0, "' is not a function" };
static const wstr S494 = { 21, 21, 1, 0, "' is already imported" };
static const wstr S495 = { 38, 38, 1, 0, "' is not a library: it ends with ;exit" };
static const wstr S496 = { 31, 31, 1, 0, "only a library ends with ';end'" };
static const wstr S497 = { 25, 25, 1, 0, "a program ends with ;exit" };
static const wstr S498 = { 88, 88, 1, 0, "a library's top level may contain only import, fn, const, var, ref and enum declarations" };
static const wstr S499 = { 37, 37, 1, 0, "move other statements into a function" };
static const wstr S500 = { 28, 28, 1, 0, "a library must end with ;end" };
static const wstr S501 = { 56, 56, 1, 0, "mark what it exports with 'export', and end it with ;end" };
static const wstr S502 = { 15, 15, 1, 0, "cannot export '" };
static const wstr S503 = { 38, 38, 1, 0, "': it is not declared at the top level" };
static const wstr S504 = { 17, 17, 1, 0, "': it is a module" };
static const wstr S505 = { 5, 5, 1, 0, "line " };
static const wstr S506 = { 6, 6, 1, 0, " line " };
static const wstr S507 = { 49, 49, 1, 0, "' never produces a value, but its value is used (" };
static const wstr S508 = { 65, 65, 1, 0, "end the function with its value, or with null if that is intended" };
static const wstr S509 = { 29, 29, 1, 0, "a program must end with ;exit" };
static const wstr S510 = { 50, 50, 1, 0, "end the program with ;exit, or a library with ;end" };
static const wstr S511 = { 3, 3, 1, 0, "add" };
static const wstr S512 = { 3, 3, 1, 0, "sub" };
static const wstr S513 = { 3, 3, 1, 0, "mul" };
static const wstr S514 = { 3, 3, 1, 0, "div" };
static const wstr S515 = { 3, 3, 1, 0, "mod" };
static const wstr S516 = { 3, 3, 1, 0, "pow" };
static const wstr S517 = { 2, 2, 1, 0, "eq" };
static const wstr S518 = { 2, 2, 1, 0, "ne" };
static const wstr S519 = { 2, 2, 1, 0, "lt" };
static const wstr S520 = { 2, 2, 1, 0, "gt" };
static const wstr S521 = { 2, 2, 1, 0, "le" };
static const wstr S522 = { 2, 2, 1, 0, "ge" };
static const wstr S523 = { 4, 4, 1, 0, "band" };
static const wstr S524 = { 3, 3, 1, 0, "bor" };
static const wstr S525 = { 4, 4, 1, 0, "bxor" };
static const wstr S526 = { 3, 3, 1, 0, "shl" };
static const wstr S527 = { 3, 3, 1, 0, "shr" };
static const wstr S528 = { 3, 3, 1, 0, "neg" };
static const wstr S529 = { 3, 3, 1, 0, "not" };
static const wstr S530 = { 4, 4, 1, 0, "bnot" };
static const wstr S531 = { 2, 2, 1, 0, "\\\"" };
static const wstr S532 = { 2, 2, 1, 0, "\\\\" };
static const wstr S533 = { 2, 2, 1, 0, "{ " };
static const wstr S534 = { 5, 5, 1, 0, ", 0, " };
static const wstr S535 = { 2, 2, 1, 0, " }" };
static const wstr S536 = { 4, 4, 1, 0, "NULL" };
static const wstr S537 = { 8, 8, 1, 0, "(wv[]){ " };
static const wstr S538 = { 6, 6, 1, 0, "RT_AT(" };
static const wstr S539 = { 1, 1, 1, 0, "w" };
static const wstr S540 = { 1, 1, 1, 0, "v" };
static const wstr S541 = { 2, 2, 1, 0, "  " };
static const wstr S542 = { 2, 2, 1, 0, "&S" };
static const wstr S543 = { 3, 3, 1, 0, "&F_" };
static const wstr S544 = { 19, 19, 1, 0, "static const wmem l" };
static const wstr S545 = { 7, 7, 1, 0, "[] = { " };
static const wstr S546 = { 3, 3, 1, 0, " };" };
static const wstr S547 = { 1, 1, 1, 0, "l" };
static const wstr S548 = { 3, 3, 1, 0, "fr." };
static const wstr S549 = { 2, 2, 1, 0, "up" };
static const wstr S550 = { 4, 4, 1, 0, "->up" };
static const wstr S551 = { 2, 2, 1, 0, "->" };
static const wstr S552 = { 7, 7, 1, 0, "rt_def(" };
static const wstr S553 = { 3, 3, 1, 0, "wv " };
static const wstr S554 = { 3, 3, 1, 0, " = " };
static const wstr S555 = { 3, 3, 1, 0, "({ " };
static const wstr S556 = { 4, 4, 1, 0, "; })" };
static const wstr S557 = { 12, 12, 1, 0, " = RT_UNSET;" };
static const wstr S558 = { 6, 6, 1, 0, "(void)" };
static const wstr S559 = { 7, 7, 1, 0, "RT_NULL" };
static const wstr S560 = { 66, 66, 1, 0, "'return' inside a conditional used as a value is not supported yet" };
static const wstr S561 = { 7, 7, 1, 0, "RT_RET(" };
static const wstr S562 = { 2, 2, 1, 0, ");" };
static const wstr S563 = { 9, 9, 1, 0, "rt_panic(" };
static const wstr S564 = { 48, 48, 1, 0, "' cannot leave a conditional used as a value yet" };
static const wstr S565 = { 6, 6, 1, 0, "break;" };
static const wstr S566 = { 9, 9, 1, 0, "continue;" };
static const wstr S567 = { 7, 7, 1, 0, "while (" };
static const wstr S568 = { 8, 8, 1, 0, "rt_cond(" };
static const wstr S569 = { 4, 4, 1, 0, ")) {" };
static const wstr S570 = { 5, 5, 1, 0, "rt_i(" };
static const wstr S571 = { 8, 8, 1, 0, "rt_exit(" };
static const wstr S572 = { 7, 7, 1, 0, "rt_i(0)" };
static const wstr S573 = { 29, 29, 1, 0, "codegen: unhandled statement " };
static const wstr S574 = { 7, 7, 1, 0, "rt_i(1)" };
static const wstr S575 = { 8, 8, 1, 0, "int64_t " };
static const wstr S576 = { 16, 16, 1, 0, " = rt_for_range(" };
static const wstr S577 = { 13, 13, 1, 0, "for (int64_t " };
static const wstr S578 = { 6, 6, 1, 0, " = 0; " };
static const wstr S579 = { 3, 3, 1, 0, " < " };
static const wstr S580 = { 2, 2, 1, 0, "; " };
static const wstr S581 = { 5, 5, 1, 0, "++) {" };
static const wstr S582 = { 7, 7, 1, 0, " - 1 - " };
static const wstr S583 = { 6, 6, 1, 0, ".i + (" };
static const wstr S584 = { 4, 4, 1, 0, ") * " };
static const wstr S585 = { 3, 3, 1, 0, ".i)" };
static const wstr S586 = { 3, 3, 1, 0, " - " };
static const wstr S587 = { 6, 6, 1, 0, "rt_m(&" };
static const wstr S588 = { 2, 2, 1, 0, "])" };
static const wstr S589 = { 5, 5, 1, 0, "wfor " };
static const wstr S590 = { 14, 14, 1, 0, "rt_for_start(&" };
static const wstr S591 = { 14, 14, 1, 0, "rt_for_guard(&" };
static const wstr S592 = { 3, 3, 1, 0, "), " };
static const wstr S593 = { 13, 13, 1, 0, "rt_for_more(&" };
static const wstr S594 = { 3, 3, 1, 0, "); " };
static const wstr S595 = { 13, 13, 1, 0, "rt_for_item(&" };
static const wstr S596 = { 12, 12, 1, 0, "rt_for_key(&" };
static const wstr S597 = { 12, 12, 1, 0, "rt_for_val(&" };
static const wstr S598 = { 13, 13, 1, 0, "rt_fail_with(" };
static const wstr S599 = { 16, 16, 1, 0, "RT_RET(RT_NULL);" };
static const wstr S600 = { 9, 9, 1, 0, "struct fr" };
static const wstr S601 = { 4, 4, 1, 0, " *up" };
static const wstr S602 = { 2, 2, 1, 0, "p_" };
static const wstr S603 = { 10, 10, 1, 0, "static wv " };
static const wstr S604 = { 4, 4, 1, 0, "void" };
static const wstr S605 = { 9, 9, 1, 0, "rt_enter(" };
static const wstr S606 = { 8, 8, 1, 0, " fr = { " };
static const wstr S607 = { 8, 8, 1, 0, ".up = up" };
static const wstr S608 = { 8, 8, 1, 0, "rt_bind(" };
static const wstr S609 = { 13, 13, 1, 0, "(void)rt_def(" };
static const wstr S610 = { 3, 3, 1, 0, "rt_" };
static const wstr S611 = { 7, 7, 1, 0, "rt_set_" };
static const wstr S612 = { 7, 7, 1, 0, "rt_upd_" };
static const wstr S613 = { 5, 5, 1, 0, ", rt_" };
static const wstr S614 = { 7, 7, 1, 0, "rt_arr(" };
static const wstr S615 = { 7, 7, 1, 0, "rt_map(" };
static const wstr S616 = { 1, 1, 1, 0, "K" };
static const wstr S617 = { 7, 7, 1, 0, "rt_rec(" };
static const wstr S618 = { 18, 18, 1, 0, "if (rt_failcode) {" };
static const wstr S619 = { 17, 17, 1, 0, "rt_i(rt_failcode)" };
static const wstr S620 = { 16, 16, 1, 0, "rt_failcode = 0;" };
static const wstr S621 = { 3, 3, 1, 0, "raw" };
static const wstr S622 = { 7, 7, 1, 0, "} else " };
static const wstr S623 = { 4, 4, 1, 0, "if (" };
static const wstr S624 = { 3, 3, 1, 0, ") {" };
static const wstr S625 = { 8, 8, 1, 0, "} else {" };
static const wstr S626 = { 3, 3, 1, 0, " ? " };
static const wstr S627 = { 3, 3, 1, 0, " : " };
static const wstr S628 = { 3, 3, 1, 0, "({\n" };
static const wstr S629 = { 2, 2, 1, 0, "})" };
static const wstr S630 = { 3, 3, 1, 0, "eq(" };
static const wstr S631 = { 4, 4, 1, 0, " || " };
static const wstr S632 = { 12, 12, 1, 0, "(rt_no_case(" };
static const wstr S633 = { 11, 11, 1, 0, "), RT_NULL)" };
static const wstr S634 = { 6, 6, 1, 0, "({ wv " };
static const wstr S635 = { 4, 4, 1, 0, "std_" };
static const wstr S636 = { 3, 3, 1, 0, "&fr" };
static const wstr S637 = { 7, 7, 1, 0, "rt_chk(" };
static const wstr S638 = { 5, 5, 1, 0, "rt_f(" };
static const wstr S639 = { 5, 5, 1, 0, "rt_s(" };
static const wstr S640 = { 8, 8, 1, 0, "rt_text(" };
static const wstr S641 = { 7, 7, 1, 0, "rt_cat(" };
static const wstr S642 = { 5, 5, 1, 0, "rt_b(" };
static const wstr S643 = { 6, 6, 1, 0, "RT_NAN" };
static const wstr S644 = { 6, 6, 1, 0, "RT_INF" };
static const wstr S645 = { 2, 2, 1, 0, "()" };
static const wstr S646 = { 7, 7, 1, 0, "rt_idx(" };
static const wstr S647 = { 7, 7, 1, 0, "rt_get(" };
static const wstr S648 = { 13, 13, 1, 0, "rt_b(rt_bool(" };
static const wstr S649 = { 3, 3, 1, 0, ", \"" };
static const wstr S650 = { 3, 3, 1, 0, "\") " };
static const wstr S651 = { 9, 9, 1, 0, " rt_bool(" };
static const wstr S652 = { 3, 3, 1, 0, "\"))" };
static const wstr S653 = { 9, 9, 1, 0, "(rt_cond(" };
static const wstr S654 = { 4, 4, 1, 0, ") ? " };
static const wstr S655 = { 30, 30, 1, 0, "codegen: unhandled expression " };
static const wstr S656 = { 2, 2, 1, 0, "\r\n" };
static const wstr S657 = { 20, 20, 1, 0, "// ---- program ----" };
static const wstr S658 = { 19, 19, 1, 0, "static const wstr S" };
static const wstr S659 = { 14, 14, 1, 0, "static wstr F_" };
static const wstr S660 = { 26, 26, 1, 0, "static const wstr *const K" };
static const wstr S661 = { 5, 5, 1, 0, " *up;" };
static const wstr S662 = { 10, 10, 1, 0, "char none;" };
static const wstr S663 = { 3, 3, 1, 0, " { " };
static const wstr S664 = { 2, 2, 1, 0, " {" };
static const wstr S665 = { 39, 39, 1, 0, "static const char *const W_files[] = { " };
static const wstr S666 = { 28, 28, 1, 0, "static void wvt_main(void) {" };
static const wstr S667 = { 21, 21, 1, 0, "  rt_files = W_files;" };
static const wstr S668 = { 10, 10, 1, 0, "  rt_syms(" };
static const wstr S669 = { 19, 19, 1, 0, ", (wstr *const[]){ " };
static const wstr S670 = { 4, 4, 1, 0, " });" };
static const wstr S671 = { 11, 11, 1, 0, "  rt_roots(" };
static const wstr S672 = { 17, 17, 1, 0, ", (wv *const[]){ " };
static const wstr S673 = { 4, 4, 1, 0, "  | " };
static const wstr S674 = { 4, 4, 1, 0, "  { " };
static const wstr S675 = { 4, 4, 1, 0, "    " };
static const wstr S676 = { 3, 3, 1, 0, "  }" };
static const wstr S677 = { 5, 5, 1, 0, "array" };
static const wstr S678 = { 2, 2, 1, 0, "[]" };
static const wstr S679 = { 6, 6, 1, 0, "record" };
static const wstr S680 = { 52, 52, 1, 0, "cannot find the standard library: no lib/ folder in " };
static const wstr S681 = { 4, 4, 1, 0, " or " };
static const wstr S682 = { 14, 14, 1, 0, "import cycle: " };
static const wstr S683 = { 4, 4, 1, 0, " -> " };
static const wstr S684 = { 29, 29, 1, 0, "the standard library has no '" };
static const wstr S685 = { 3, 3, 1, 0, "' (" };
static const wstr S686 = { 13, 13, 1, 0, "cannot read '" };
static const wstr S687 = { 3, 1, 0, 0, "﻿" };
static const wstr S688 = { 7, 7, 1, 0, "usage:\n" };
static const wstr S689 = { 64, 64, 1, 0, "  wvc <file.wvt> [-o <out.c>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S690 = { 90, 90, 1, 0, "  wvc build <file.wvt> [-o <program>] [--cc <compiler>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S691 = { 26, 26, 1, 0, "  wvc --tokens <file.wvt>\n" };
static const wstr S692 = { 22, 22, 1, 0, "  wvc --ast <file.wvt>" };
static const wstr S693 = { 17, 17, 1, 0, "wvc: cannot read " };
static const wstr S694 = { 5, 5, 1, 0, "build" };
static const wstr S695 = { 8, 8, 1, 0, "--tokens" };
static const wstr S696 = { 5, 5, 1, 0, "--ast" };
static const wstr S697 = { 2, 2, 1, 0, "-o" };
static const wstr S698 = { 9, 9, 1, 0, "--runtime" };
static const wstr S699 = { 4, 4, 1, 0, "--cc" };
static const wstr S700 = { 5, 5, 1, 0, "--lib" };
static const wstr S701 = { 2, 2, 1, 0, "OS" };
static const wstr S702 = { 10, 10, 1, 0, "Windows_NT" };
static const wstr S703 = { 3, 3, 1, 0, "ZIG" };
static const wstr S704 = { 3, 3, 1, 0, "zig" };
static const wstr S705 = { 5, 5, 1, 0, "wvc: " };
static const wstr S706 = { 52, 52, 1, 0, " does not end with .wvt, so name the program with -o" };
static const wstr S707 = { 4, 4, 1, 0, ".exe" };
static const wstr S708 = { 13, 13, 1, 0, "runtime/wvt.c" };
static const wstr S709 = { 3, 3, 1, 0, "../" };
static const wstr S710 = { 3, 3, 1, 0, "lib" };
static const wstr S711 = { 6, 6, 1, 0, "../lib" };
static const wstr S712 = { 2, 2, 1, 0, ".c" };
static const wstr S713 = { 18, 18, 1, 0, "wvc: cannot write " };
static const wstr S714 = { 3, 3, 1, 0, "-O2" };
static const wstr S715 = { 7, 7, 1, 0, "zig.exe" };
static const wstr S716 = { 2, 2, 1, 0, "cc" };
static const wstr S717 = { 16, 16, 1, 0, "wvc: cannot run " };
static const wstr S718 = { 67, 67, 1, 0, ": install Zig (https://ziglang.org), or name a C compiler with --cc" };
static const wstr S719 = { 23, 23, 1, 0, " failed with exit code " };
static wstr F_severity = { 8, 8, 1, 0, "severity" };
static wstr F_pos = { 3, 3, 1, 0, "pos" };
static wstr F_message = { 7, 7, 1, 0, "message" };
static wstr F_hint = { 4, 4, 1, 0, "hint" };
static wstr F_file = { 4, 4, 1, 0, "file" };
static wstr F_chars = { 5, 5, 1, 0, "chars" };
static wstr F_word = { 4, 4, 1, 0, "word" };
static wstr F_t = { 1, 1, 1, 0, "t" };
static wstr F_end = { 3, 3, 1, 0, "end" };
static wstr F_sp = { 2, 2, 1, 0, "sp" };
static wstr F_nl = { 2, 2, 1, 0, "nl" };
static wstr F_v = { 1, 1, 1, 0, "v" };
static wstr F_text = { 4, 4, 1, 0, "text" };
static wstr F_parts = { 5, 5, 1, 0, "parts" };
static wstr F_tokens = { 6, 6, 1, 0, "tokens" };
static wstr F_k = { 1, 1, 1, 0, "k" };
static wstr F_e = { 1, 1, 1, 0, "e" };
static wstr F_handler = { 7, 7, 1, 0, "handler" };
static wstr F_items = { 5, 5, 1, 0, "items" };
static wstr F_fields = { 6, 6, 1, 0, "fields" };
static wstr F_index = { 5, 5, 1, 0, "index" };
static wstr F_arr = { 3, 3, 1, 0, "arr" };
static wstr F_rec = { 3, 3, 1, 0, "rec" };
static wstr F_l = { 1, 1, 1, 0, "l" };
static wstr F_r = { 1, 1, 1, 0, "r" };
static wstr F_c = { 1, 1, 1, 0, "c" };
static wstr F_a = { 1, 1, 1, 0, "a" };
static wstr F_b = { 1, 1, 1, 0, "b" };
static wstr F_stmts = { 5, 5, 1, 0, "stmts" };
static wstr F_param = { 5, 5, 1, 0, "param" };
static wstr F_paramPos = { 8, 8, 1, 0, "paramPos" };
static wstr F_body = { 4, 4, 1, 0, "body" };
static wstr F_neg = { 3, 3, 1, 0, "neg" };
static wstr F_cond = { 4, 4, 1, 0, "cond" };
static wstr F_isVar = { 5, 5, 1, 0, "isVar" };
static wstr F_isRef = { 5, 5, 1, 0, "isRef" };
static wstr F_name = { 4, 4, 1, 0, "name" };
static wstr F_namePos = { 7, 7, 1, 0, "namePos" };
static wstr F_op = { 2, 2, 1, 0, "op" };
static wstr F_target = { 6, 6, 1, 0, "target" };
static wstr F_names = { 5, 5, 1, 0, "names" };
static wstr F_coll = { 4, 4, 1, 0, "coll" };
static wstr F_from = { 4, 4, 1, 0, "from" };
static wstr F_to = { 2, 2, 1, 0, "to" };
static wstr F_step = { 4, 4, 1, 0, "step" };
static wstr F_reversed = { 8, 8, 1, 0, "reversed" };
static wstr F_code = { 4, 4, 1, 0, "code" };
static wstr F_arg = { 3, 3, 1, 0, "arg" };
static wstr F_paren = { 5, 5, 1, 0, "paren" };
static wstr F_path = { 4, 4, 1, 0, "path" };
static wstr F_pathPos = { 7, 7, 1, 0, "pathPos" };
static wstr F_params = { 6, 6, 1, 0, "params" };
static wstr F_members = { 7, 7, 1, 0, "members" };
static wstr F_value = { 5, 5, 1, 0, "value" };
static wstr F_close = { 5, 5, 1, 0, "close" };
static wstr F_field = { 5, 5, 1, 0, "field" };
static wstr F_callee = { 6, 6, 1, 0, "callee" };
static wstr F_args = { 4, 4, 1, 0, "args" };
static wstr F_values = { 6, 6, 1, 0, "values" };
static wstr F_subject = { 7, 7, 1, 0, "subject" };
static wstr F_cases = { 5, 5, 1, 0, "cases" };
static wstr F_els = { 3, 3, 1, 0, "els" };
static wstr F_branches = { 8, 8, 1, 0, "branches" };
static wstr F_arity = { 5, 5, 1, 0, "arity" };
static wstr F_mod = { 3, 3, 1, 0, "mod" };
static wstr F_parent = { 6, 6, 1, 0, "parent" };
static wstr F_kind = { 4, 4, 1, 0, "kind" };
static wstr F_visibleAt = { 9, 9, 1, 0, "visibleAt" };
static wstr F_fnId = { 4, 4, 1, 0, "fnId" };
static wstr F_lib = { 3, 3, 1, 0, "lib" };
static wstr F_id = { 2, 2, 1, 0, "id" };
static wstr F_func = { 4, 4, 1, 0, "func" };
static wstr F_top = { 3, 3, 1, 0, "top" };
static wstr F_loops = { 5, 5, 1, 0, "loops" };
static wstr F_guards = { 6, 6, 1, 0, "guards" };
static wstr F_key = { 3, 3, 1, 0, "key" };
static wstr F_exports = { 7, 7, 1, 0, "exports" };
static wstr F_refTargets = { 10, 10, 1, 0, "refTargets" };
static wstr F_useMod = { 6, 6, 1, 0, "useMod" };
static wstr F_imports = { 7, 7, 1, 0, "imports" };
static wstr F_node = { 4, 4, 1, 0, "node" };
static wstr F_notLibrary = { 10, 10, 1, 0, "notLibrary" };
static wstr F_program = { 7, 7, 1, 0, "program" };
static wstr F_children = { 8, 8, 1, 0, "children" };
static wstr F_d = { 1, 1, 1, 0, "d" };
static wstr F_depth = { 5, 5, 1, 0, "depth" };
static wstr F_cname = { 5, 5, 1, 0, "cname" };
static wstr F_captured = { 8, 8, 1, 0, "captured" };
static wstr F_fnIndex = { 7, 7, 1, 0, "fnIndex" };
static wstr F_pre = { 3, 3, 1, 0, "pre" };
static wstr F_cs = { 2, 2, 1, 0, "cs" };
static wstr F_proto = { 5, 5, 1, 0, "proto" };
static wstr F_lines = { 5, 5, 1, 0, "lines" };
static wstr F_js = { 2, 2, 1, 0, "js" };
static const wstr *const K0[] = { &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars };
static const wstr *const K1[] = { &F_word, &F_message };
static const wstr *const K2[] = { &F_t, &F_pos, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts };
static const wstr *const K3[] = { &F_tokens, &F_pos };
static const wstr *const K4[] = { &F_tokens, &F_end };
static const wstr *const K5[] = { &F_k, &F_e, &F_pos, &F_end, &F_handler };
static const wstr *const K6[] = { &F_pos };
static const wstr *const K7[] = { &F_stmts, &F_end };
static const wstr *const K8[] = { &F_k, &F_pos, &F_end, &F_handler };
static const wstr *const K9[] = { &F_pos, &F_param, &F_paramPos, &F_body };
static const wstr *const K10[] = { &F_k, &F_neg, &F_cond, &F_body, &F_pos, &F_end, &F_handler };
static const wstr *const K11[] = { &F_k, &F_e, &F_isVar, &F_isRef, &F_name, &F_namePos, &F_pos, &F_end, &F_handler };
static const wstr *const K12[] = { &F_k, &F_e, &F_op, &F_target, &F_pos, &F_end, &F_handler };
static const wstr *const K13[] = { &F_name, &F_pos };
static const wstr *const K14[] = { &F_k, &F_names, &F_coll, &F_from, &F_to, &F_step, &F_reversed, &F_body, &F_pos, &F_end, &F_handler };
static const wstr *const K15[] = { &F_k, &F_code, &F_pos, &F_end, &F_handler };
static const wstr *const K16[] = { &F_k, &F_arg, &F_pos, &F_paren };
static const wstr *const K17[] = { &F_k, &F_path, &F_pathPos, &F_name, &F_namePos, &F_pos, &F_end, &F_handler };
static const wstr *const K18[] = { &F_k, &F_names, &F_pos, &F_end, &F_handler };
static const wstr *const K19[] = { &F_name, &F_isVar, &F_isRef, &F_pos };
static const wstr *const K20[] = { &F_k, &F_name, &F_namePos, &F_params, &F_body, &F_pos, &F_end, &F_handler };
static const wstr *const K21[] = { &F_k, &F_name, &F_namePos, &F_members, &F_pos, &F_end, &F_handler };
static const wstr *const K22[] = { &F_stmts, &F_value, &F_close };
static const wstr *const K23[] = { &F_k, &F_c, &F_a, &F_b, &F_pos, &F_paren };
static const wstr *const K24[] = { &F_k, &F_op, &F_l, &F_r, &F_pos, &F_paren };
static const wstr *const K25[] = { &F_k, &F_v, &F_pos, &F_paren };
static const wstr *const K26[] = { &F_k, &F_op, &F_e, &F_pos, &F_paren };
static const wstr *const K27[] = { &F_k, &F_index, &F_arr, &F_pos, &F_paren };
static const wstr *const K28[] = { &F_k, &F_field, &F_rec, &F_pos, &F_paren };
static const wstr *const K29[] = { &F_k, &F_name, &F_pos, &F_paren };
static const wstr *const K30[] = { &F_k, &F_callee, &F_args, &F_pos, &F_paren };
static const wstr *const K31[] = { &F_k, &F_parts, &F_pos, &F_paren };
static const wstr *const K32[] = { &F_k, &F_pos, &F_paren };
static const wstr *const K33[] = { &F_k, &F_items, &F_pos, &F_paren };
static const wstr *const K34[] = { &F_name, &F_e, &F_pos };
static const wstr *const K35[] = { &F_k, &F_fields, &F_pos, &F_paren };
static const wstr *const K36[] = { &F_values, &F_body, &F_pos };
static const wstr *const K37[] = { &F_k, &F_subject, &F_cases, &F_els, &F_pos, &F_paren };
static const wstr *const K38[] = { &F_neg, &F_cond, &F_body, &F_pos };
static const wstr *const K39[] = { &F_k, &F_branches, &F_els, &F_pos, &F_paren };
static const wstr *const K40[] = { &F_name, &F_arity };
static const wstr *const K41[] = { &F_parent, &F_names };
static const wstr *const K42[] = { &F_kind, &F_name, &F_isVar, &F_isRef, &F_namePos, &F_visibleAt, &F_fnId, &F_arity, &F_mod, &F_lib };
static const wstr *const K43[] = { &F_mod, &F_name, &F_namePos, &F_body };
static const wstr *const K44[] = { &F_mod, &F_name, &F_members };
static const wstr *const K45[] = { &F_id, &F_top };
static const wstr *const K46[] = { &F_func, &F_loops, &F_guards };
static const wstr *const K47[] = { &F_key, &F_pos };
static const wstr *const K48[] = { &F_mod, &F_namePos, &F_pos, &F_useMod };
static const wstr *const K49[] = { &F_stmts, &F_value };
static const wstr *const K50[] = { &F_name, &F_kind, &F_d };
static const wstr *const K51[] = { &F_name, &F_parent, &F_depth, &F_top, &F_children, &F_cname };
static const wstr *const K52[] = { &F_kind, &F_name, &F_func, &F_cname, &F_isRef, &F_captured, &F_fnIndex };
static const wstr *const K53[] = { &F_name, &F_lib };
static const wstr *const K54[] = { &F_c, &F_node };
static const wstr *const K55[] = { &F_c, &F_k };
static const wstr *const K56[] = { &F_pre, &F_cs };
static const wstr *const K57[] = { &F_proto, &F_lines };
static const wstr *const K58[] = { &F_k, &F_js, &F_pos, &F_paren };
static const wstr *const K59[] = { &F_node, &F_lib };
static const wstr *const K60[] = { &F_file, &F_chars, &F_path, &F_name, &F_program, &F_imports, &F_exports, &F_notLibrary, &F_refTargets };
struct fr27 { wv v71_cs; wv v72_n; wv v73_i; wv v74_prevT; wv v75_nlNow; wv v76_toks; };
struct fr49 { wv v185_toks; wv v187_p; wv v188_depth; wv v189_exports; wv v190_firstExport; };
struct fr101 { wv v401_modules; wv v403_errors; wv v404_nextFn; wv v405_curMod; wv v406_functions; wv v407_uses; wv v408_enums; wv v444_topFn; };
struct fr146 { wv v686_modules; wv v687_runtime; wv v688_n; wv v689_tmps; wv v690_strings; wv v691_fields; wv v692_shapes; wv v693_enumDefs; wv v694_globals; wv v695_frames; wv v696_functions; wv v697_out; wv v698_indent; wv v699_loops; wv v700_iife; wv v701_decls; wv v702_fns; wv v703_tops; wv v704_slots; wv v705_curMod; wv v706_mod; wv v707_curSlot; wv v708_stolen; wv v709_ownEnums; wv v710_cur; wv v711_lineStarts; };
struct fr220 { wv v1128_libDir; wv v1129_searched; wv v1130_modules; wv v1131_libRoot; };
static wv w2_srcFile;
static wv w2_srcChars;
static wv w2_warnings;
static wv w3_keywords;
static wv w3_reserved;
static wv w3_bangWords;
static wv w3_setOps;
static wv w3_ops;
static wv w3_maxInt;
static wv w4_exprStart;
static wv w4_onfailOk;
static wv w4_callees;
static wv w4_exportable;
static wv w5_stdlib;
static wv w5_neverCompletes;
static wv w5_onfailValueUsed;
static wv w5_libraryTop;
static wv w6_binOps;
static wv w6_binFns;
static wv w6_scalarKinds;
static wv w6_plainKinds;
static wv w6_signalers;
static wv w6_constK;
static wv w6_pureK;
static wv w6_effectK;
static wv w_usage;
static wv w_argv;
static wv w_mode;
static wv w_input;
static wv w_output;
static wv w_runtimePath;
static wv w_cc;
static wv w_libGiven;
static wv w_arg;
static wv w_inspect;
static wv w_dotWvt;
static wv w_runtimeGiven;
static wv w_searched;
static wv w_libDir;
static wv w_modules;
static wv w_problems;
static wv w_runtime;
static wv w_code;
static wv w_dotExe;
static wv w_cFile;
static wv w_parts;
static wv w_ccName;
static wv w_flags;
static wv w_status;
static wv v1166_a;
static wv v1181_tokens;
static wv v1182_program;
static wv v1183_p;
static wv w1_isSpace(wv v0_c);
static wv w1_isDigit(wv v1_c);
static wv w1_isHexDigit(wv v2_c);
static wv w1_isIdStart(wv v3_c);
static wv w1_isIdChar(wv v4_c);
static wv w1_isJsSpace(wv v5_c);
static wv w1_isSigTerm(wv v7_c);
static wv w1_contains(wv v8_xs, wv v9_x);
static wv w1_shiftCase(wv v11_s, wv v12_from, wv v13_shift);
static wv w1_toUpper(wv v18_s);
static wv w1_toLower(wv v19_s);
static wv w1_hexValue(wv v20_s);
static wv w1_quote(wv v27_s);
static wv w2_setSource(wv v34_file, wv v35_chars);
static wv w2_warn(wv v36_pos, wv v37_message, wv v38_hint);
static wv w2_err(wv v39_pos, wv v40_message, wv v41_hint);
static wv w2_formatDiag(wv v42_d);
static wv w2_flushWarnings(void);
static wv w2_failHint(wv v52_pos, wv v53_message, wv v54_hint);
static wv w2_abort(wv v55_pos, wv v56_message);
static wv w3_digitRun(wv v57_cs, wv v58_k, wv v59_base);
static wv w3_isDigitIn(wv v61_c, wv v62_base);
static wv w3_digitsValue(wv v63_digits, wv v64_base);
static wv w3_simpleEscape(wv v70_e);
static wv w3_lex(wv p_v71_cs);
static wv s77_ch(struct fr27 *up, wv v90_k);
static wv s78_cut(struct fr27 *up, wv v91_a, wv v92_b);
static wv s79_startsWith(struct fr27 *up, wv v93_k, wv v94_s);
static wv s80_find(struct fr27 *up, wv v97_s, wv v98_from);
static wv s81_tok(struct fr27 *up, wv v100_t, wv v101_pos, wv v102_sp, wv v103_v, wv v104_spelling, wv v105_parts);
static wv s82_plain(struct fr27 *up, wv v106_t, wv v107_pos, wv v108_sp);
static wv s83_skip(struct fr27 *up);
static wv s84_next(struct fr27 *up);
static wv s85_word(struct fr27 *up, wv v125_start, wv v126_sp);
static wv s86_number(struct fr27 *up, wv v130_start, wv v131_sp);
static wv s87_sigil(struct fr27 *up, wv v142_start, wv v143_sp);
static wv s88_string(struct fr27 *up, wv v145_start, wv v146_sp);
static wv s89_interpolation(struct fr27 *up, wv v161_open);
static wv w4_precedence(wv v170_t);
static wv w4_describe(wv v171_tk);
static wv w4_failAtHint(wv v172_tk, wv v173_message, wv v174_hint);
static wv w4_failAt(wv v175_tk, wv v176_message);
static wv w4_simpleStatement(wv v177_k, wv v178_e);
static wv w4_parse(wv v179_toks);
static wv w4_hasEffect(wv v180_e);
static wv w4_parser(wv p_v185_toks, wv v186_inner);
static wv s191_peek(struct fr49 *up);
static wv s192_at(struct fr49 *up, wv v231_type);
static wv s193_atWord(struct fr49 *up, wv v232_word);
static wv s194_atCall(struct fr49 *up);
static wv s195_next(struct fr49 *up);
static wv s196_accept(struct fr49 *up, wv v234_type);
static wv s197_expect(struct fr49 *up, wv v235_type, wv v236_msg, wv v237_hint);
static wv s198_expectWord(struct fr49 *up, wv v240_word, wv v241_hint);
static wv s199_lintSpace(struct fr49 *up, wv v243_op);
static wv s200_program(struct fr49 *up);
static wv s201_statement(struct fr49 *up);
static wv s202_statementBody(struct fr49 *up, wv v256_tk);
static wv s203_declaration(struct fr49 *up);
static wv s204_setStatement(struct fr49 *up);
static wv s205_forStatement(struct fr49 *up);
static wv s206_failStatement(struct fr49 *up);
static wv s207_importStatement(struct fr49 *up);
static wv s208_exportStatement(struct fr49 *up);
static wv s209_endStatement(struct fr49 *up);
static wv s210_fnStatement(struct fr49 *up);
static wv s211_enumStatement(struct fr49 *up);
static wv s212_cond(struct fr49 *up, wv v306_kw);
static wv s213_block(struct fr49 *up);
static wv s214_lvalue(struct fr49 *up);
static wv s215_expr(struct fr49 *up);
static wv s216_ternary(struct fr49 *up);
static wv s217_ternaryTail(struct fr49 *up, wv v316_c);
static wv s218_binary(struct fr49 *up, wv v321_min);
static wv s219_binaryFrom(struct fr49 *up, wv v322_left, wv v323_min);
static wv s220_unary(struct fr49 *up);
static wv s221_powerTail(struct fr49 *up, wv v330_base);
static wv s222_postfix(struct fr49 *up);
static wv s223_call(struct fr49 *up, wv v337_args);
static wv s224_primary(struct fr49 *up);
static wv s225_record(struct fr49 *up, wv v351_open);
static wv s226_constantKey(struct fr49 *up, wv v359_e);
static wv s227_mapLiteral(struct fr49 *up, wv v360_open, wv v361_first);
static wv s228_parseMatch(struct fr49 *up);
static wv s229_caseValue(struct fr49 *up);
static wv s230_chain(struct fr49 *up);
static wv w5_stdSig(wv v383_name);
static wv w5_modeOf(wv v385_x);
static wv w5_kindText(wv v386_kind);
static wv w5_plural(wv v387_n);
static wv w5_enumText(wv v388_x);
static wv w5_caseKey(wv v389_v);
static wv w5_caseText(wv v390_v);
static wv w5_getsAwayWithValue(wv v391_list);
static wv w5_lineIn(wv v396_chars, wv v397_pos);
static wv w5_lineOf(wv v400_pos);
static wv w5_check(wv p_v401_modules, wv v402_requireExit);
static wv s409_error(struct fr101 *up, wv v449_pos, wv v450_message, wv v451_hint);
static wv s410_enter(struct fr101 *up, wv v452_m);
static wv s411_enumMembers(struct fr101 *up, wv v453_d);
static wv s412_firstUse(struct fr101 *up, wv v455_mod, wv v456_declPos);
static wv s413_requireValue(struct fr101 *up, wv v458_b);
static wv s414_newScope(struct fr101 *up, wv v461_parent);
static wv s415_decl(struct fr101 *up, wv v463_kind, wv v464_name, wv v465_mode, wv v466_namePos, wv v467_visibleAt, wv v468_fnId, wv v469_arity);
static wv s416_own(struct fr101 *up, wv v470_scope, wv v471_name);
static wv s417_declare(struct fr101 *up, wv v473_scope, wv v474_name, wv v475_d);
static wv s418_lookup(struct fr101 *up, wv v479_scope, wv v480_name);
static wv s419_resolve(struct fr101 *up, wv v483_name, wv v484_pos, wv v485_scope, wv v486_ctx);
static wv s420_block(struct fr101 *up, wv v488_b, wv v489_scope, wv v490_ctx);
static wv s421_stmt(struct fr101 *up, wv v495_s, wv v496_scope, wv v497_ctx);
static wv s422_forLoop(struct fr101 *up, wv v504_s, wv v505_scope, wv v506_ctx);
static wv s423_pathOf(struct fr101 *up, wv v512_x);
static wv s424_guarded(struct fr101 *up, wv v515_t, wv v516_ctx);
static wv s425_onfailHandler(struct fr101 *up, wv v524_s, wv v525_scope, wv v526_ctx);
static wv s426_checkMatch(struct fr101 *up, wv v529_e, wv v530_scope, wv v531_ctx, wv v532_stmtLevel);
static wv s427_moduleNamed(struct fr101 *up, wv v552_x, wv v553_scope);
static wv s428_exported(struct fr101 *up, wv v555_lib, wv v556_name);
static wv s429_enumNamed(struct fr101 *up, wv v559_x, wv v560_scope);
static wv s430_exportOf(struct fr101 *up, wv v564_e, wv v565_m);
static wv s431_unknownModule(struct fr101 *up, wv v567_x, wv v568_scope);
static wv s432_qualified(struct fr101 *up, wv v570_e, wv v571_m);
static wv s433_caseEnum(struct fr101 *up, wv v574_v, wv v575_scope);
static wv s434_member(struct fr101 *up, wv v581_e, wv v582_d);
static wv s435_chain(struct fr101 *up, wv v587_e, wv v588_scope, wv v589_ctx, wv v590_stmtLevel);
static wv s436_lvalue(struct fr101 *up, wv v593_t, wv v594_scope, wv v595_ctx);
static wv s437_mustBeMutable(struct fr101 *up, wv v602_d, wv v603_name, wv v604_pos, wv v605_message, wv v606_rebind);
static wv s438_unknownStd(struct fr101 *up, wv v608_node);
static wv s439_expr(struct fr101 *up, wv v609_e, wv v610_scope, wv v611_ctx);
static wv s440_callee(struct fr101 *up, wv v621_e, wv v622_scope, wv v623_used);
static wv s441_importNames(struct fr101 *up, wv v631_m, wv v632_scope);
static wv s442_libraryRules(struct fr101 *up, wv v641_m, wv v642_scope, wv v643_isEntry, wv v644_last);
static wv s443_checkModule(struct fr101 *up, wv v651_m, wv v652_isEntry);
static wv w6_binFn(wv v663_op);
static wv w6_modeOf(wv v665_x);
static wv w6_unFn(wv v666_op);
static wv w6_repeat(wv v667_s, wv v668_count);
static wv w6_utf8Length(wv v671_s);
static wv w6_cString(wv v676_s);
static wv w6_wstrInit(wv v682_s);
static wv w6_values(wv v685_cs);
static wv w6_generateC(wv p_v686_modules, wv p_v687_runtime);
static wv s712_atLine(struct fr146 *up, wv v777_pos);
static wv s713_atExpr(struct fr146 *up, wv v781_pos, wv v782_c);
static wv s714_modId(struct fr146 *up, wv v783_m);
static wv s715_hasFrame(struct fr146 *up, wv v784_f);
static wv s716_newScope(struct fr146 *up, wv v785_parent);
static wv s717_put(struct fr146 *up, wv v787_scope, wv v788_name, wv v789_kind, wv v790_d);
static wv s718_find(struct fr146 *up, wv v791_scope, wv v792_name);
static wv s719_kindOf(struct fr146 *up, wv v796_scope, wv v797_name);
static wv s720_newFn(struct fr146 *up, wv v799_name, wv v800_parent);
static wv s721_bind(struct fr146 *up, wv v802_kind, wv v803_name, wv v804_f, wv v805_atTop, wv v806_mode);
static wv s722_ablock(struct fr146 *up, wv v809_b, wv v810_scope, wv v811_f, wv v812_topScope);
static wv s723_astmt(struct fr146 *up, wv v820_s, wv v821_scope, wv v822_f, wv v823_topScope);
static wv s724_use(struct fr146 *up, wv v838_x, wv v839_scope, wv v840_f);
static wv s725_atarget(struct fr146 *up, wv v842_t, wv v843_scope, wv v844_f);
static wv s726_notValue(struct fr146 *up, wv v845_x, wv v846_scope);
static wv s727_aexpr(struct fr146 *up, wv v848_e, wv v849_scope, wv v850_f);
static wv s728_analyze(struct fr146 *up, wv v858_m);
static wv s729_enter(struct fr146 *up, wv v865_m);
static wv s730_emit(struct fr146 *up, wv v872_line);
static wv s731_tmp(struct fr146 *up);
static wv s732_str(struct fr146 *up, wv v874_s);
static wv s733_fieldRef(struct fr146 *up, wv v876_name);
static wv s734_enumDef(struct fr146 *up, wv v877_s, wv v878_m);
static wv s735_stolenFrom(struct fr146 *up, wv v882_x);
static wv s736_enumIn(struct fr146 *up, wv v884_list, wv v885_name);
static wv s737_enumOf(struct fr146 *up, wv v887_x);
static wv s738_place(struct fr146 *up, wv v892_d);
static wv s739_tdz(struct fr146 *up, wv v894_d);
static wv s740_read(struct fr146 *up, wv v895_d);
static wv s741_seq(struct fr146 *up, wv v896_args);
static wv s742_wrap(struct fr146 *up, wv v903_sq, wv v904_inner);
static wv s743_stmts(struct fr146 *up, wv v905_list);
static wv s744_resets(struct fr146 *up, wv v907_b);
static wv s745_blockBody(struct fr146 *up, wv v911_b);
static wv s746_valueOf(struct fr146 *up, wv v912_b);
static wv s747_stmt(struct fr146 *up, wv v913_s);
static wv s748_pathText(struct fr146 *up, wv v915_x);
static wv s749_forStmt(struct fr146 *up, wv v917_s);
static wv s750_signal(struct fr146 *up, wv v932_code);
static wv s751_bindDecl(struct fr146 *up, wv v933_d, wv v934_value);
static wv s752_fnDef(struct fr146 *up, wv v936_s);
static wv s753_bindRhs(struct fr146 *up, wv v956_e, wv v957_mode);
static wv s754_setStmt(struct fr146 *up, wv v959_s);
static wv s755_exprs(struct fr146 *up, wv v974_list);
static wv s756_array(struct fr146 *up, wv v977_e, wv v978_mutable);
static wv s757_mapLit(struct fr146 *up, wv v980_e, wv v981_mutable);
static wv s758_record(struct fr146 *up, wv v983_e, wv v984_mutable);
static wv s759_onfailStmt(struct fr146 *up, wv v992_s);
static wv s760_chainStmt(struct fr146 *up, wv v1001_e);
static wv s761_test(struct fr146 *up, wv v1004_b);
static wv s762_branchCount(struct fr146 *up, wv v1005_e);
static wv s763_branchBody(struct fr146 *up, wv v1006_e, wv v1007_i);
static wv s764_branchTest(struct fr146 *up, wv v1008_e, wv v1009_t, wv v1010_i);
static wv s765_isPlain(struct fr146 *up, wv v1011_e);
static wv s766_ternaryChain(struct fr146 *up, wv v1015_e, wv v1016_t);
static wv s767_branchValue(struct fr146 *up, wv v1019_e, wv v1020_t, wv v1021_subject);
static wv s768_chainExpr(struct fr146 *up, wv v1030_e);
static wv s769_caseTest(struct fr146 *up, wv v1031_t, wv v1032_a);
static wv s770_matchElse(struct fr146 *up, wv v1035_e, wv v1036_t);
static wv s771_matchStmt(struct fr146 *up, wv v1037_e);
static wv s772_matchExpr(struct fr146 *up, wv v1041_e);
static wv s773_callC(struct fr146 *up, wv v1045_callee, wv v1046_args, wv v1047_checked);
static wv s774_expr(struct fr146 *up, wv v1056_e);
static wv s775_assemble(struct fr146 *up);
static wv w7_dumpTokens(wv v1088_toks, wv v1089_indent);
static wv w7_sorted(wv v1095_xs);
static wv w7_dumpValue(wv v1100_x, wv v1101_indent);
static wv w8_dirOf(wv v1110_path);
static wv w8_normalize(wv v1113_path);
static wv w8_with(wv v1118_xs, wv v1119_x);
static wv w8_failIn(wv v1122_file, wv v1123_chars, wv v1124_pos, wv v1125_message);
static wv w8_loadProgram(wv v1126_entryFile, wv v1127_entryChars, wv p_v1128_libDir, wv p_v1129_searched);
static wv s1132_traceName(struct fr220 *up, wv v1137_path);
static wv s1133_target(struct fr220 *up, wv v1138_s, wv v1139_file, wv v1140_chars, wv v1141_path);
static wv s1134_loaded(struct fr220 *up, wv v1142_path);
static wv s1135_visit(struct fr220 *up, wv v1144_file, wv v1145_chars, wv v1146_path, wv v1147_chain);
static wv w_die(void);
static wv w_load(wv v1162_file);
static wv w_onWindows(void);
static wv w_tryRead(wv v1168_path);
static wv w_programDir(void);
static wv w_readRuntime(void);
static wv w_findLib(void);

static wv w1_isSpace(wv v0_c) {
  rt_enter("isSpace");
  v0_c = rt_bind(v0_c, 0);
  RT_RET((RT_AT(0, 3), rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v0_c, rt_s(&S0)), "||") || rt_bool(rt_eq(v0_c, rt_s(&S1)), "||")), "||") || rt_bool(rt_eq(v0_c, rt_s(&S2)), "||")), "||") || rt_bool(rt_eq(v0_c, rt_s(&S3)), "||"))));
}

static wv w1_isDigit(wv v1_c) {
  rt_enter("isDigit");
  v1_c = rt_bind(v1_c, 0);
  RT_RET((RT_AT(0, 4), rt_b(rt_bool(rt_ge(v1_c, rt_s(&S4)), "&&") && rt_bool(rt_le(v1_c, rt_s(&S5)), "&&"))));
}

static wv w1_isHexDigit(wv v2_c) {
  rt_enter("isHexDigit");
  v2_c = rt_bind(v2_c, 0);
  RT_RET((RT_AT(0, 5), rt_b(rt_bool(rt_b(rt_bool(rt_chk(w1_isDigit(v2_c), "isHexDigit"), "||") || rt_bool(rt_b(rt_bool(rt_ge(v2_c, rt_s(&S6)), "&&") && rt_bool(rt_le(v2_c, rt_s(&S7)), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v2_c, rt_s(&S8)), "&&") && rt_bool(rt_le(v2_c, rt_s(&S9)), "&&")), "||"))));
}

static wv w1_isIdStart(wv v3_c) {
  rt_enter("isIdStart");
  v3_c = rt_bind(v3_c, 0);
  RT_RET((RT_AT(0, 6), rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v3_c, rt_s(&S6)), "&&") && rt_bool(rt_le(v3_c, rt_s(&S10)), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v3_c, rt_s(&S8)), "&&") && rt_bool(rt_le(v3_c, rt_s(&S11)), "&&")), "||")), "||") || rt_bool(rt_eq(v3_c, rt_s(&S12)), "||"))));
}

static wv w1_isIdChar(wv v4_c) {
  rt_enter("isIdChar");
  v4_c = rt_bind(v4_c, 0);
  RT_RET((RT_AT(0, 7), rt_b(rt_bool(rt_chk(w1_isIdStart(v4_c), "isIdChar"), "||") || rt_bool(rt_chk(w1_isDigit(v4_c), "isIdChar"), "||"))));
}

static wv w1_isJsSpace(wv v5_c) {
  rt_enter("isJsSpace");
  v5_c = rt_bind(v5_c, 0);
  RT_AT(0, 11);
  if (rt_cond(rt_eq(v5_c, rt_s(&S13)))) {
    RT_AT(0, 11);
    RT_RET(rt_b(0));
  }
  RT_AT(0, 12);
  wv v6_k = rt_bind(std_code(v5_c), 0);
  RT_RET((RT_AT(0, 15), rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v6_k, rt_i(9)), "&&") && rt_bool(rt_le(v6_k, rt_i(13)), "&&")), "||") || rt_bool(rt_eq(v6_k, rt_i(32)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(160)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(5760)), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v6_k, rt_i(8192)), "&&") && rt_bool(rt_le(v6_k, rt_i(8202)), "&&")), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8232)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8233)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8239)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8287)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(12288)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(65279)), "||"))));
}

static wv w1_isSigTerm(wv v7_c) {
  rt_enter("isSigTerm");
  v7_c = rt_bind(v7_c, 0);
  RT_RET((RT_AT(0, 19), rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v7_c, rt_s(&S13)), "||") || rt_bool(rt_chk(w1_isSpace(v7_c), "isSigTerm"), "||")), "||") || rt_bool(rt_eq(v7_c, rt_s(&S14)), "||")), "||") || rt_bool(rt_eq(v7_c, rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(v7_c, rt_s(&S16)), "||"))));
}

static wv w1_contains(wv v8_xs, wv v9_x) {
  rt_enter("contains");
  v9_x = rt_bind(v9_x, 0);
  RT_AT(0, 22);
  wv v10_k = rt_i(0);
  RT_AT(0, 23);
  while (rt_cond((RT_AT(0, 23), ({ wv t0 = v10_k; wv t1 = std_length(v8_xs); rt_lt(t0, t1); })))) {
    RT_AT(0, 24);
    if (rt_cond(({ wv t2 = rt_idx(v10_k, v8_xs); wv t3 = v9_x; rt_eq(t2, t3); }))) {
      RT_AT(0, 24);
      RT_RET(rt_b(1));
    }
    RT_AT(0, 25);
    v10_k = rt_add(v10_k, rt_i(1));
  }
  RT_RET((RT_AT(0, 27), rt_b(0)));
}

static wv w1_shiftCase(wv v11_s, wv v12_from, wv v13_shift) {
  rt_enter("shiftCase");
  v11_s = rt_bind(v11_s, 0);
  v12_from = rt_bind(v12_from, 0);
  v13_shift = rt_bind(v13_shift, 0);
  RT_AT(0, 32);
  wv v14_cs = rt_bind(std_split(v11_s, rt_s(&S13)), 0);
  RT_AT(0, 33);
  wv v15_out = rt_s(&S13);
  RT_AT(0, 34);
  wv v16_k = rt_i(0);
  RT_AT(0, 35);
  while (rt_cond((RT_AT(0, 35), ({ wv t4 = v16_k; wv t5 = std_length(v14_cs); rt_lt(t4, t5); })))) {
    RT_AT(0, 36);
    wv v17_c = rt_bind(rt_idx(v16_k, v14_cs), 0);
    RT_AT(0, 37);
    if (rt_cond(rt_b(rt_bool(rt_ge(v17_c, v12_from), "&&") && rt_bool(({ wv t6 = v17_c; wv t7 = std_fromCode(rt_add(std_code(v12_from), rt_i(25))); rt_le(t6, t7); }), "&&")))) {
      RT_AT(0, 38);
      v15_out = ({ wv t10 = v15_out; wv t11 = std_fromCode(({ wv t8 = std_code(v17_c); wv t9 = v13_shift; rt_add(t8, t9); })); rt_add(t10, t11); });
    } else {
      RT_AT(0, 40);
      v15_out = rt_add(v15_out, v17_c);
    }
    RT_AT(0, 42);
    v16_k = rt_add(v16_k, rt_i(1));
  }
  RT_RET((RT_AT(0, 44), v15_out));
}

static wv w1_toUpper(wv v18_s) {
  rt_enter("toUpper");
  v18_s = rt_bind(v18_s, 0);
  RT_RET((RT_AT(0, 46), rt_chk(w1_shiftCase(v18_s, rt_s(&S6), rt_i(-32)), "toUpper")));
}

static wv w1_toLower(wv v19_s) {
  rt_enter("toLower");
  v19_s = rt_bind(v19_s, 0);
  RT_RET((RT_AT(0, 47), rt_chk(w1_shiftCase(v19_s, rt_s(&S8), rt_i(32)), "toLower")));
}

static wv w1_hexValue(wv v20_s) {
  rt_enter("hexValue");
  v20_s = rt_bind(v20_s, 0);
  RT_AT(0, 51);
  wv v21_cs = rt_bind(std_split(v20_s, rt_s(&S13)), 0);
  RT_AT(0, 52);
  if (rt_cond(rt_b(rt_bool(rt_lt(std_length(v21_cs), rt_i(1)), "||") || rt_bool(rt_gt(std_length(v21_cs), rt_i(6)), "||")))) {
    RT_AT(0, 52);
    RT_RET(rt_i(-1));
  }
  RT_AT(0, 53);
  wv v22_v = rt_i(0);
  RT_AT(0, 54);
  wv v23_k = rt_i(0);
  RT_AT(0, 55);
  while (rt_cond((RT_AT(0, 55), ({ wv t12 = v23_k; wv t13 = std_length(v21_cs); rt_lt(t12, t13); })))) {
    RT_AT(0, 56);
    wv v24_c = rt_bind(rt_idx(v23_k, v21_cs), 0);
    RT_AT(0, 57);
    if (rt_cond(rt_not(rt_chk(w1_isHexDigit(v24_c), "hexValue")))) {
      RT_AT(0, 57);
      RT_RET(rt_i(-1));
    }
    RT_AT(0, 58);
    wv v25_digits = rt_bind(std_split(rt_s(&S17), rt_s(&S13)), 0);
    RT_AT(0, 59);
    wv v26_d = rt_i(0);
    RT_AT(0, 60);
    while (rt_cond((RT_AT(0, 60), ({ wv t14 = rt_chk(w1_toUpper(rt_idx(v26_d, v25_digits)), "hexValue"); wv t15 = rt_chk(w1_toUpper(v24_c), "hexValue"); rt_ne(t14, t15); })))) {
      RT_AT(0, 60);
      v26_d = rt_add(v26_d, rt_i(1));
    }
    RT_AT(0, 61);
    v22_v = ({ wv t16 = rt_mul(v22_v, rt_i(16)); wv t17 = v26_d; rt_add(t16, t17); });
    RT_AT(0, 62);
    v23_k = rt_add(v23_k, rt_i(1));
  }
  RT_RET((RT_AT(0, 64), v22_v));
}

static wv w1_quote(wv v27_s) {
  rt_enter("quote");
  v27_s = rt_bind(v27_s, 0);
  RT_AT(0, 69);
  wv v28_cs = rt_bind(std_split(v27_s, rt_s(&S13)), 0);
  RT_AT(0, 70);
  wv v29_out = rt_s(&S18);
  RT_AT(0, 71);
  wv v30_k = rt_i(0);
  RT_AT(0, 72);
  while (rt_cond((RT_AT(0, 72), ({ wv t18 = v30_k; wv t19 = std_length(v28_cs); rt_lt(t18, t19); })))) {
    RT_AT(0, 73);
    wv v31_c = rt_bind(rt_idx(v30_k, v28_cs), 0);
    RT_AT(0, 74);
    wv v32_code = rt_bind(std_code(v31_c), 0);
    RT_AT(0, 75);
    if (rt_cond(rt_b(rt_bool(rt_eq(v31_c, rt_s(&S18)), "||") || rt_bool(rt_eq(v31_c, rt_s(&S19)), "||")))) {
      RT_AT(0, 76);
      v29_out = ({ wv t20 = rt_add(v29_out, rt_s(&S19)); wv t21 = v31_c; rt_add(t20, t21); });
    } else if (rt_cond(rt_eq(v31_c, rt_s(&S2)))) {
      RT_AT(0, 78);
      v29_out = rt_add(v29_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v31_c, rt_s(&S3)))) {
      RT_AT(0, 80);
      v29_out = rt_add(v29_out, rt_s(&S21));
    } else if (rt_cond(rt_eq(v31_c, rt_s(&S1)))) {
      RT_AT(0, 82);
      v29_out = rt_add(v29_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v32_code, rt_i(8)))) {
      RT_AT(0, 84);
      v29_out = rt_add(v29_out, rt_s(&S23));
    } else if (rt_cond(rt_eq(v32_code, rt_i(12)))) {
      RT_AT(0, 86);
      v29_out = rt_add(v29_out, rt_s(&S24));
    } else if (rt_cond(rt_lt(v32_code, rt_i(32)))) {
      RT_AT(0, 88);
      wv v33_hex = rt_bind(std_split(rt_s(&S17), rt_s(&S13)), 0);
      RT_AT(0, 89);
      v29_out = ({ wv t28 = ({ wv t24 = rt_add(v29_out, rt_s(&S25)); wv t25 = ({ wv t22 = std_floor(rt_div(v32_code, rt_i(16))); wv t23 = v33_hex; rt_idx(t22, t23); }); rt_add(t24, t25); }); wv t29 = ({ wv t26 = rt_mod(v32_code, rt_i(16)); wv t27 = v33_hex; rt_idx(t26, t27); }); rt_add(t28, t29); });
    } else {
      RT_AT(0, 91);
      v29_out = rt_add(v29_out, v31_c);
    }
    RT_AT(0, 93);
    v30_k = rt_add(v30_k, rt_i(1));
  }
  RT_RET((RT_AT(0, 95), rt_add(v29_out, rt_s(&S18))));
}

static wv w2_setSource(wv v34_file, wv v35_chars) {
  rt_enter("setSource");
  v34_file = rt_bind(v34_file, 0);
  RT_AT(1, 12);
  wv t30 = rt_bind(v34_file, 1);
  (void)rt_def(w2_srcFile, "srcFile");
  w2_srcFile = t30;
  RT_AT(1, 13);
  wv t31 = v35_chars;
  (void)rt_def(w2_srcChars, "srcChars");
  w2_srcChars = t31;
  RT_RET(RT_NULL);
}

static wv w2_warn(wv v36_pos, wv v37_message, wv v38_hint) {
  rt_enter("warn");
  v36_pos = rt_bind(v36_pos, 0);
  v37_message = rt_bind(v37_message, 0);
  v38_hint = rt_bind(v38_hint, 0);
  RT_RET((RT_AT(1, 17), ({ wv t32 = v36_pos; wv t33 = v37_message; wv t34 = v38_hint; wv t35 = rt_def(w2_srcFile, "srcFile"); wv t36 = rt_def(w2_srcChars, "srcChars"); rt_rec(0, 6, K0, (wv[]){ rt_s(&S26), t32, t33, t34, t35, t36 }); })));
}

static wv w2_err(wv v39_pos, wv v40_message, wv v41_hint) {
  rt_enter("err");
  v39_pos = rt_bind(v39_pos, 0);
  v40_message = rt_bind(v40_message, 0);
  v41_hint = rt_bind(v41_hint, 0);
  RT_RET((RT_AT(1, 20), ({ wv t37 = v39_pos; wv t38 = v40_message; wv t39 = v41_hint; wv t40 = rt_def(w2_srcFile, "srcFile"); wv t41 = rt_def(w2_srcChars, "srcChars"); rt_rec(0, 6, K0, (wv[]){ rt_s(&S27), t37, t38, t39, t40, t41 }); })));
}

static wv w2_formatDiag(wv v42_d) {
  rt_enter("formatDiag");
  v42_d = rt_bind(v42_d, 0);
  RT_AT(1, 25);
  wv v43_chars = rt_bind(rt_get(&F_chars, v42_d), 0);
  RT_AT(1, 26);
  wv v44_line = rt_i(1);
  RT_AT(1, 27);
  wv v45_lineStart = rt_i(0);
  RT_AT(1, 28);
  wv v46_k = rt_i(0);
  RT_AT(1, 29);
  while (rt_cond((RT_AT(1, 29), ({ wv t42 = v46_k; wv t43 = rt_get(&F_pos, v42_d); rt_lt(t42, t43); })))) {
    RT_AT(1, 30);
    if (rt_cond(rt_eq(rt_idx(v46_k, v43_chars), rt_s(&S2)))) {
      RT_AT(1, 31);
      v44_line = rt_add(v44_line, rt_i(1));
      RT_AT(1, 32);
      v45_lineStart = rt_add(v46_k, rt_i(1));
    }
    RT_AT(1, 34);
    v46_k = rt_add(v46_k, rt_i(1));
  }
  RT_AT(1, 36);
  wv v47_lineEnd = rt_bind(v45_lineStart, 1);
  RT_AT(1, 37);
  while (rt_cond((RT_AT(1, 37), rt_b(rt_bool(({ wv t44 = v47_lineEnd; wv t45 = std_length(v43_chars); rt_lt(t44, t45); }), "&&") && rt_bool(rt_ne(rt_idx(v47_lineEnd, v43_chars), rt_s(&S2)), "&&"))))) {
    RT_AT(1, 37);
    v47_lineEnd = rt_add(v47_lineEnd, rt_i(1));
  }
  RT_AT(1, 38);
  if (rt_cond(rt_b(rt_bool(rt_gt(v47_lineEnd, v45_lineStart), "&&") && rt_bool(rt_eq(({ wv t46 = rt_sub(v47_lineEnd, rt_i(1)); wv t47 = v43_chars; rt_idx(t46, t47); }), rt_s(&S3)), "&&")))) {
    RT_AT(1, 38);
    v47_lineEnd = rt_sub(v47_lineEnd, rt_i(1));
  }
  RT_AT(1, 39);
  wv v48_pad = rt_s(&S13);
  RT_AT(1, 40);
  v46_k = rt_bind(v45_lineStart, 1);
  RT_AT(1, 41);
  while (rt_cond((RT_AT(1, 41), rt_b(rt_bool(({ wv t48 = v46_k; wv t49 = rt_get(&F_pos, v42_d); rt_lt(t48, t49); }), "&&") && rt_bool(rt_lt(v46_k, v47_lineEnd), "&&"))))) {
    RT_AT(1, 42);
    v48_pad = ({ wv t50 = v48_pad; wv t51 = (rt_cond(rt_eq(rt_idx(v46_k, v43_chars), rt_s(&S1))) ? rt_s(&S1) : rt_s(&S0)); rt_add(t50, t51); });
    RT_AT(1, 43);
    v46_k = rt_add(v46_k, rt_i(1));
  }
  RT_AT(1, 45);
  wv v49_text = rt_bind(std_join(std_slice(v43_chars, v45_lineStart, v47_lineEnd), rt_s(&S13)), 0);
  RT_AT(1, 46);
  wv v50_out = ({ wv t54 = rt_text(rt_get(&F_file, v42_d)); wv t55 = rt_text(v44_line); wv t56 = rt_text(rt_add(({ wv t52 = rt_get(&F_pos, v42_d); wv t53 = v45_lineStart; rt_sub(t52, t53); }), rt_i(1))); wv t57 = rt_text(rt_get(&F_severity, v42_d)); wv t58 = rt_text(rt_get(&F_message, v42_d)); rt_cat(9, (wv[]){ t54, rt_s(&S28), t55, rt_s(&S28), t56, rt_s(&S29), t57, rt_s(&S29), t58 }); });
  RT_AT(1, 47);
  if (rt_cond(rt_ne(rt_get(&F_hint, v42_d), RT_NULL))) {
    RT_AT(1, 47);
    v50_out = ({ wv t59 = v50_out; wv t60 = rt_cat(2, (wv[]){ rt_s(&S30), rt_text(rt_get(&F_hint, v42_d)) }); rt_add(t59, t60); });
  }
  RT_RET((RT_AT(1, 48), ({ wv t61 = rt_text(v50_out); wv t62 = rt_text(v49_text); wv t63 = rt_text(v48_pad); rt_cat(6, (wv[]){ t61, rt_s(&S31), t62, rt_s(&S31), t63, rt_s(&S32) }); })));
}

static wv w2_flushWarnings(void) {
  rt_enter("flushWarnings");
  RT_AT(1, 52);
  wv v51_k = rt_i(0);
  RT_AT(1, 53);
  while (rt_cond((RT_AT(1, 53), ({ wv t64 = v51_k; wv t65 = std_length(rt_def(w2_warnings, "warnings")); rt_lt(t64, t65); })))) {
    RT_AT(1, 54);
    (void)std_eprint(rt_chk(w2_formatDiag(({ wv t66 = v51_k; wv t67 = rt_def(w2_warnings, "warnings"); rt_idx(t66, t67); })), "flushWarnings"));
    RT_AT(1, 55);
    v51_k = rt_add(v51_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w2_failHint(wv v52_pos, wv v53_message, wv v54_hint) {
  rt_enter("failHint");
  v52_pos = rt_bind(v52_pos, 0);
  v53_message = rt_bind(v53_message, 0);
  v54_hint = rt_bind(v54_hint, 0);
  RT_AT(1, 60);
  (void)rt_chk(w2_flushWarnings(), "failHint");
  RT_AT(1, 61);
  (void)std_eprint(rt_chk(w2_formatDiag(rt_chk(w2_err(v52_pos, v53_message, v54_hint), "failHint")), "failHint"));
  RT_AT(1, 62);
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w2_abort(wv v55_pos, wv v56_message) {
  rt_enter("abort");
  v55_pos = rt_bind(v55_pos, 0);
  v56_message = rt_bind(v56_message, 0);
  RT_AT(1, 64);
  (void)rt_chk(w2_failHint(v55_pos, v56_message, RT_NULL), "abort");
  RT_RET(RT_NULL);
}

static wv w3_digitRun(wv v57_cs, wv v58_k, wv v59_base) {
  rt_enter("digitRun");
  v57_cs = rt_bind(v57_cs, 0);
  v58_k = rt_bind(v58_k, 0);
  v59_base = rt_bind(v59_base, 0);
  RT_AT(2, 36);
  wv v60_j = rt_bind(v58_k, 1);
  RT_AT(2, 37);
  while (rt_cond((RT_AT(2, 37), rt_b(rt_bool(({ wv t71 = v60_j; wv t72 = std_length(v57_cs); rt_lt(t71, t72); }), "&&") && rt_bool(({ wv t73 = rt_idx(v60_j, v57_cs); wv t74 = v59_base; rt_chk(w3_isDigitIn(t73, t74), "digitRun"); }), "&&"))))) {
    RT_AT(2, 37);
    v60_j = rt_add(v60_j, rt_i(1));
  }
  RT_RET((RT_AT(2, 38), v60_j));
}

static wv w3_isDigitIn(wv v61_c, wv v62_base) {
  rt_enter("isDigitIn");
  v61_c = rt_bind(v61_c, 0);
  v62_base = rt_bind(v62_base, 0);
  RT_AT(2, 42);
  if (rt_cond(rt_eq(v62_base, rt_i(16)))) {
    RT_AT(2, 42);
    RT_RET(rt_chk(w1_isHexDigit(v61_c), "isDigitIn"));
  }
  RT_AT(2, 43);
  if (rt_cond(rt_eq(v62_base, rt_i(2)))) {
    RT_AT(2, 43);
    RT_RET(rt_b(rt_bool(rt_eq(v61_c, rt_s(&S4)), "||") || rt_bool(rt_eq(v61_c, rt_s(&S102)), "||")));
  }
  RT_RET((RT_AT(2, 44), rt_chk(w1_isDigit(v61_c), "isDigitIn")));
}

static wv w3_digitsValue(wv v63_digits, wv v64_base) {
  rt_enter("digitsValue");
  v63_digits = rt_bind(v63_digits, 0);
  v64_base = rt_bind(v64_base, 0);
  RT_AT(2, 49);
  wv v65_hex = rt_bind(std_split(rt_s(&S17), rt_s(&S13)), 0);
  RT_AT(2, 50);
  wv v66_v = rt_i(0);
  RT_AT(2, 51);
  wv v67_k = rt_i(0);
  RT_AT(2, 52);
  while (rt_cond((RT_AT(2, 52), ({ wv t75 = v67_k; wv t76 = std_length(v63_digits); rt_lt(t75, t76); })))) {
    RT_AT(2, 53);
    wv v68_c = rt_bind(rt_chk(w1_toLower(rt_idx(v67_k, v63_digits)), "digitsValue"), 0);
    RT_AT(2, 54);
    wv v69_d = rt_i(0);
    RT_AT(2, 55);
    while (rt_cond((RT_AT(2, 55), ({ wv t77 = rt_idx(v69_d, v65_hex); wv t78 = v68_c; rt_ne(t77, t78); })))) {
      RT_AT(2, 55);
      v69_d = rt_add(v69_d, rt_i(1));
    }
    RT_AT(2, 56);
    if (rt_cond(({ wv t83 = v66_v; wv t84 = ({ wv t81 = ({ wv t79 = rt_def(w3_maxInt, "maxInt"); wv t80 = v69_d; rt_sub(t79, t80); }); wv t82 = v64_base; rt_div(t81, t82); }); rt_gt(t83, t84); }))) {
      RT_AT(2, 56);
      RT_RET(rt_i(-1));
    }
    RT_AT(2, 57);
    v66_v = ({ wv t85 = rt_mul(v66_v, v64_base); wv t86 = v69_d; rt_add(t85, t86); });
    RT_AT(2, 58);
    v67_k = rt_add(v67_k, rt_i(1));
  }
  RT_RET((RT_AT(2, 60), v66_v));
}

static wv w3_simpleEscape(wv v70_e) {
  rt_enter("simpleEscape");
  v70_e = rt_bind(v70_e, 0);
  RT_AT(2, 65);
  if (rt_cond(rt_eq(v70_e, rt_s(&S103)))) {
    RT_AT(2, 65);
    RT_RET(rt_s(&S2));
  }
  RT_AT(2, 66);
  if (rt_cond(rt_eq(v70_e, rt_s(&S104)))) {
    RT_AT(2, 66);
    RT_RET(rt_s(&S3));
  }
  RT_AT(2, 67);
  if (rt_cond(rt_eq(v70_e, rt_s(&S105)))) {
    RT_AT(2, 67);
    RT_RET(rt_s(&S1));
  }
  RT_AT(2, 68);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v70_e, rt_s(&S19)), "||") || rt_bool(rt_eq(v70_e, rt_s(&S18)), "||")), "||") || rt_bool(rt_eq(v70_e, rt_s(&S106)), "||")))) {
    RT_AT(2, 68);
    RT_RET(v70_e);
  }
  RT_RET((RT_AT(2, 69), RT_NULL));
}

static wv w3_lex(wv p_v71_cs) {
  rt_enter("lex");
  struct fr27 fr = { 0 };
  fr.v71_cs = rt_bind(p_v71_cs, 0);
  RT_AT(2, 74);
  fr.v72_n = rt_bind(std_length(fr.v71_cs), 0);
  RT_AT(2, 75);
  fr.v73_i = rt_i(0);
  RT_AT(2, 76);
  fr.v74_prevT = rt_s(&S13);
  RT_AT(2, 78);
  fr.v75_nlNow = rt_b(0);
  RT_AT(2, 79);
  fr.v76_toks = rt_arr(0, NULL, 1);
  RT_AT(2, 411);
  while (rt_cond((RT_AT(2, 411), rt_b(1)))) {
    RT_AT(2, 412);
    wv v169_t0 = rt_bind(rt_chk(s84_next(&fr), "lex"), 0);
    RT_AT(2, 413);
    (void)std_push(fr.v76_toks, v169_t0);
    RT_AT(2, 414);
    if (rt_cond(rt_eq(rt_get(&F_t, v169_t0), rt_s(&S111)))) {
      break;
    }
  }
  RT_RET((RT_AT(2, 416), fr.v76_toks));
}

static wv s77_ch(struct fr27 *up, wv v90_k) {
  rt_enter("ch");
  v90_k = rt_bind(v90_k, 0);
  RT_RET((RT_AT(2, 82), (rt_cond(rt_b(rt_bool(rt_ge(v90_k, rt_i(0)), "&&") && rt_bool(({ wv t87 = v90_k; wv t88 = rt_def(up->v72_n, "n"); rt_lt(t87, t88); }), "&&"))) ? rt_idx(v90_k, up->v71_cs) : rt_s(&S13))));
}

static wv s78_cut(struct fr27 *up, wv v91_a, wv v92_b) {
  rt_enter("cut");
  v91_a = rt_bind(v91_a, 0);
  v92_b = rt_bind(v92_b, 0);
  RT_RET((RT_AT(2, 83), std_join(std_slice(up->v71_cs, v91_a, v92_b), rt_s(&S13))));
}

static wv s79_startsWith(struct fr27 *up, wv v93_k, wv v94_s) {
  rt_enter("startsWith");
  v93_k = rt_bind(v93_k, 0);
  v94_s = rt_bind(v94_s, 0);
  RT_AT(2, 87);
  wv v95_sc = rt_bind(std_split(v94_s, rt_s(&S13)), 0);
  RT_AT(2, 88);
  if (rt_cond(({ wv t91 = ({ wv t89 = v93_k; wv t90 = std_length(v95_sc); rt_add(t89, t90); }); wv t92 = rt_def(up->v72_n, "n"); rt_gt(t91, t92); }))) {
    RT_AT(2, 88);
    RT_RET(rt_b(0));
  }
  RT_AT(2, 89);
  wv v96_q = rt_i(0);
  RT_AT(2, 90);
  while (rt_cond((RT_AT(2, 90), ({ wv t93 = v96_q; wv t94 = std_length(v95_sc); rt_lt(t93, t94); })))) {
    RT_AT(2, 91);
    if (rt_cond(({ wv t97 = ({ wv t95 = rt_add(v93_k, v96_q); wv t96 = up->v71_cs; rt_idx(t95, t96); }); wv t98 = rt_idx(v96_q, v95_sc); rt_ne(t97, t98); }))) {
      RT_AT(2, 91);
      RT_RET(rt_b(0));
    }
    RT_AT(2, 92);
    v96_q = rt_add(v96_q, rt_i(1));
  }
  RT_RET((RT_AT(2, 94), rt_b(1)));
}

static wv s80_find(struct fr27 *up, wv v97_s, wv v98_from) {
  rt_enter("find");
  v97_s = rt_bind(v97_s, 0);
  v98_from = rt_bind(v98_from, 0);
  RT_AT(2, 99);
  wv v99_q = rt_bind(v98_from, 1);
  RT_AT(2, 100);
  while (rt_cond((RT_AT(2, 100), ({ wv t99 = v99_q; wv t100 = rt_def(up->v72_n, "n"); rt_lt(t99, t100); })))) {
    RT_AT(2, 101);
    if (rt_cond(rt_chk(s79_startsWith(up, v99_q, v97_s), "find"))) {
      RT_AT(2, 101);
      RT_RET(v99_q);
    }
    RT_AT(2, 102);
    v99_q = rt_add(v99_q, rt_i(1));
  }
  RT_RET((RT_AT(2, 104), rt_i(-1)));
}

static wv s81_tok(struct fr27 *up, wv v100_t, wv v101_pos, wv v102_sp, wv v103_v, wv v104_spelling, wv v105_parts) {
  rt_enter("tok");
  v100_t = rt_bind(v100_t, 0);
  v101_pos = rt_bind(v101_pos, 0);
  v102_sp = rt_bind(v102_sp, 0);
  v103_v = rt_bind(v103_v, 0);
  v104_spelling = rt_bind(v104_spelling, 0);
  v105_parts = rt_bind(v105_parts, 0);
  RT_AT(2, 108);
  wv t101 = rt_bind(v100_t, 1);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t101;
  RT_RET((RT_AT(2, 109), ({ wv t102 = v100_t; wv t103 = v101_pos; wv t104 = rt_def(up->v73_i, "i"); wv t105 = v102_sp; wv t106 = rt_def(up->v75_nlNow, "nlNow"); wv t107 = v103_v; wv t108 = v104_spelling; wv t109 = v105_parts; rt_rec(0, 8, K2, (wv[]){ t102, t103, t104, t105, t106, t107, t108, t109 }); })));
}

static wv s82_plain(struct fr27 *up, wv v106_t, wv v107_pos, wv v108_sp) {
  rt_enter("plain");
  v106_t = rt_bind(v106_t, 0);
  v107_pos = rt_bind(v107_pos, 0);
  v108_sp = rt_bind(v108_sp, 0);
  RT_RET((RT_AT(2, 111), rt_chk(s81_tok(up, v106_t, v107_pos, v108_sp, RT_NULL, RT_NULL, RT_NULL), "plain")));
}

static wv s83_skip(struct fr27 *up) {
  rt_enter("skip");
  RT_AT(2, 115);
  wv v109_skipped = rt_b(0);
  RT_AT(2, 116);
  while (rt_cond((RT_AT(2, 116), rt_b(1)))) {
    RT_AT(2, 117);
    wv v110_c = rt_bind(rt_chk(s77_ch(up, rt_def(up->v73_i, "i")), "skip"), 0);
    RT_AT(2, 118);
    if (rt_cond(rt_chk(w1_isSpace(v110_c), "skip"))) {
      RT_AT(2, 119);
      up->v73_i = rt_add(rt_def(up->v73_i, "i"), rt_i(1));
      RT_AT(2, 120);
      v109_skipped = rt_b(1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v110_c, rt_s(&S88)), "&&") && rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(rt_def(up->v73_i, "i"), rt_i(1))), "skip"), rt_s(&S88)), "&&")))) {
      RT_AT(2, 122);
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v109_skipped), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, rt_sub(rt_def(up->v73_i, "i"), rt_i(1))), "skip")), "skip"), "&&")), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, rt_add(rt_def(up->v73_i, "i"), rt_i(2))), "skip")), "skip"), "&&")))) {
        RT_AT(2, 123);
        (void)({ wv t110 = w2_warnings; wv t111 = rt_chk(w2_warn(rt_def(up->v73_i, "i"), rt_s(&S107), rt_s(&S108)), "skip"); std_push(t110, t111); });
      }
      RT_AT(2, 126);
      while (rt_cond((RT_AT(2, 126), rt_b(rt_bool(({ wv t112 = rt_def(up->v73_i, "i"); wv t113 = rt_def(up->v72_n, "n"); rt_lt(t112, t113); }), "&&") && rt_bool(rt_ne(({ wv t114 = rt_def(up->v73_i, "i"); wv t115 = up->v71_cs; rt_idx(t114, t115); }), rt_s(&S2)), "&&"))))) {
        RT_AT(2, 126);
        up->v73_i = rt_add(rt_def(up->v73_i, "i"), rt_i(1));
      }
      RT_AT(2, 127);
      v109_skipped = rt_b(1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v110_c, rt_s(&S88)), "&&") && rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(rt_def(up->v73_i, "i"), rt_i(1))), "skip"), rt_s(&S87)), "&&")))) {
      RT_AT(2, 129);
      wv v111_close = rt_bind(rt_chk(s80_find(up, rt_s(&S109), rt_add(rt_def(up->v73_i, "i"), rt_i(2))), "skip"), 0);
      RT_AT(2, 130);
      if (rt_cond(rt_lt(v111_close, rt_i(0)))) {
        RT_AT(2, 130);
        (void)rt_chk(w2_abort(rt_def(up->v73_i, "i"), rt_s(&S110)), "skip");
      }
      RT_AT(2, 131);
      wv t116 = rt_add(v111_close, rt_i(2));
      (void)rt_def(up->v73_i, "i");
      up->v73_i = t116;
      RT_AT(2, 132);
      v109_skipped = rt_b(1);
    } else {
      RT_AT(2, 134);
      RT_RET(v109_skipped);
    }
  }
  RT_RET(RT_NULL);
}

static wv s84_next(struct fr27 *up) {
  rt_enter("next");
  RT_AT(2, 140);
  wv v112_from = rt_bind(rt_def(up->v73_i, "i"), 0);
  RT_AT(2, 141);
  wv v113_sp = rt_b(rt_bool(rt_chk(s83_skip(up), "next"), "||") || rt_bool(rt_eq(rt_def(up->v73_i, "i"), rt_i(0)), "||"));
  RT_AT(2, 142);
  wv t117 = rt_b(0);
  (void)rt_def(up->v75_nlNow, "nlNow");
  up->v75_nlNow = t117;
  RT_AT(2, 143);
  wv v114_q = rt_bind(v112_from, 1);
  RT_AT(2, 144);
  while (rt_cond((RT_AT(2, 144), ({ wv t118 = v114_q; wv t119 = rt_def(up->v73_i, "i"); rt_lt(t118, t119); })))) {
    RT_AT(2, 145);
    if (rt_cond(rt_eq(rt_idx(v114_q, up->v71_cs), rt_s(&S2)))) {
      RT_AT(2, 145);
      wv t120 = rt_b(1);
      (void)rt_def(up->v75_nlNow, "nlNow");
      up->v75_nlNow = t120;
    }
    RT_AT(2, 146);
    v114_q = rt_add(v114_q, rt_i(1));
  }
  RT_AT(2, 148);
  wv v115_start = rt_bind(rt_def(up->v73_i, "i"), 0);
  RT_AT(2, 149);
  wv v116_c = rt_bind(rt_chk(s77_ch(up, v115_start), "next"), 0);
  RT_AT(2, 150);
  if (rt_cond(rt_eq(v116_c, rt_s(&S13)))) {
    RT_AT(2, 150);
    RT_RET(rt_chk(s82_plain(up, rt_s(&S111), v115_start, v113_sp), "next"));
  }
  RT_AT(2, 151);
  if (rt_cond(rt_chk(w1_isIdStart(v116_c), "next"))) {
    RT_AT(2, 151);
    RT_RET(rt_chk(s85_word(up, v115_start, v113_sp), "next"));
  }
  RT_AT(2, 152);
  if (rt_cond(rt_eq(v116_c, rt_s(&S18)))) {
    RT_AT(2, 152);
    RT_RET(rt_chk(s88_string(up, v115_start, v113_sp), "next"));
  }
  RT_AT(2, 153);
  if (rt_cond(rt_eq(v116_c, rt_s(&S106)))) {
    RT_AT(2, 153);
    RT_RET(rt_chk(s87_sigil(up, v115_start, v113_sp), "next"));
  }
  RT_AT(2, 154);
  if (rt_cond(rt_chk(w1_isDigit(v116_c), "next"))) {
    RT_AT(2, 154);
    RT_RET(rt_chk(s86_number(up, v115_start, v113_sp), "next"));
  }
  RT_AT(2, 155);
  wv v117_k = rt_i(0);
  RT_AT(2, 156);
  while (rt_cond((RT_AT(2, 156), ({ wv t121 = v117_k; wv t122 = std_length(rt_def(w3_setOps, "setOps")); rt_lt(t121, t122); })))) {
    RT_AT(2, 157);
    wv v118_op = rt_bind(({ wv t123 = v117_k; wv t124 = rt_def(w3_setOps, "setOps"); rt_idx(t123, t124); }), 0);
    RT_AT(2, 158);
    if (rt_cond(rt_chk(s79_startsWith(up, v115_start, v118_op), "next"))) {
      RT_AT(2, 159);
      wv t127 = ({ wv t125 = v115_start; wv t126 = std_length(v118_op); rt_add(t125, t126); });
      (void)rt_def(up->v73_i, "i");
      up->v73_i = t127;
      RT_AT(2, 160);
      RT_RET(({ wv t130 = v115_start; wv t131 = v113_sp; wv t132 = ({ wv t128 = v118_op; wv t129 = rt_sub(std_length(v118_op), rt_i(1)); std_slice(t128, rt_i(0), t129); }); rt_chk(s81_tok(up, rt_s(&S112), t130, t131, t132, RT_NULL, RT_NULL), "next"); }));
    }
    RT_AT(2, 162);
    v117_k = rt_add(v117_k, rt_i(1));
  }
  RT_AT(2, 164);
  if (rt_cond(rt_eq(v116_c, rt_s(&S95)))) {
    RT_AT(2, 166);
    wv v119_limit = rt_add(v115_start, rt_i(64));
    RT_AT(2, 167);
    wv v120_j = rt_add(v115_start, rt_i(1));
    RT_AT(2, 168);
    while (rt_cond((RT_AT(2, 168), rt_b(rt_bool(rt_lt(v120_j, v119_limit), "&&") && rt_bool(rt_chk(w1_isJsSpace(rt_chk(s77_ch(up, v120_j), "next")), "next"), "&&"))))) {
      RT_AT(2, 168);
      v120_j = rt_add(v120_j, rt_i(1));
    }
    RT_AT(2, 169);
    wv v121_wordStart = rt_bind(v120_j, 0);
    RT_AT(2, 170);
    if (rt_cond(rt_b(rt_bool(rt_lt(v120_j, v119_limit), "&&") && rt_bool(rt_chk(w1_isIdStart(rt_chk(s77_ch(up, v120_j), "next")), "next"), "&&")))) {
      RT_AT(2, 171);
      while (rt_cond((RT_AT(2, 171), rt_b(rt_bool(rt_lt(v120_j, v119_limit), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v120_j), "next")), "next"), "&&"))))) {
        RT_AT(2, 171);
        v120_j = rt_add(v120_j, rt_i(1));
      }
      RT_AT(2, 172);
      wv v122_w = rt_bind(rt_chk(s78_cut(up, v121_wordStart, v120_j), "next"), 0);
      RT_AT(2, 173);
      if (rt_cond(({ wv t133 = rt_def(w3_bangWords, "bangWords"); wv t134 = v122_w; rt_chk(w1_contains(t133, t134), "next"); }))) {
        RT_AT(2, 174);
        if (rt_cond(({ wv t135 = v121_wordStart; wv t136 = rt_add(v115_start, rt_i(1)); rt_gt(t135, t136); }))) {
          RT_AT(2, 175);
          (void)({ wv t137 = v115_start; wv t138 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v122_w), rt_s(&S114) }); wv t139 = rt_cat(3, (wv[]){ rt_s(&S115), rt_text(v122_w), rt_s(&S114) }); rt_chk(w2_failHint(t137, t138, t139), "next"); });
        }
        RT_AT(2, 177);
        wv t140 = rt_bind(v120_j, 1);
        (void)rt_def(up->v73_i, "i");
        up->v73_i = t140;
        RT_AT(2, 178);
        RT_RET(({ wv t141 = rt_add(rt_s(&S95), v122_w); wv t142 = v115_start; wv t143 = v113_sp; rt_chk(s82_plain(up, t141, t142, t143), "next"); }));
      }
    }
  }
  RT_AT(2, 182);
  if (rt_cond(rt_chk(s79_startsWith(up, v115_start, rt_s(&S116)), "next"))) {
    RT_AT(2, 183);
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S117), rt_s(&S118)), "next");
  }
  RT_AT(2, 185);
  if (rt_cond(rt_chk(s79_startsWith(up, v115_start, rt_s(&S119)), "next"))) {
    RT_AT(2, 186);
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S120), rt_s(&S121)), "next");
  }
  RT_AT(2, 189);
  if (rt_cond(rt_chk(s79_startsWith(up, v115_start, rt_s(&S122)), "next"))) {
    RT_AT(2, 190);
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S123), rt_s(&S124)), "next");
  }
  RT_AT(2, 192);
  v117_k = rt_i(0);
  RT_AT(2, 193);
  while (rt_cond((RT_AT(2, 193), ({ wv t144 = v117_k; wv t145 = std_length(rt_def(w3_ops, "ops")); rt_lt(t144, t145); })))) {
    RT_AT(2, 194);
    wv v123_op = rt_bind(({ wv t146 = v117_k; wv t147 = rt_def(w3_ops, "ops"); rt_idx(t146, t147); }), 0);
    RT_AT(2, 195);
    if (rt_cond(rt_chk(s79_startsWith(up, v115_start, v123_op), "next"))) {
      RT_AT(2, 196);
      wv t150 = ({ wv t148 = v115_start; wv t149 = std_length(v123_op); rt_add(t148, t149); });
      (void)rt_def(up->v73_i, "i");
      up->v73_i = t150;
      RT_AT(2, 197);
      if (rt_cond(rt_b(rt_bool(rt_eq(v123_op, rt_s(&S14)), "&&") && rt_bool(rt_eq(rt_def(up->v74_prevT, "prevT"), rt_s(&S14)), "&&")))) {
        RT_AT(2, 198);
        (void)({ wv t151 = w2_warnings; wv t152 = rt_chk(w2_warn(v115_start, rt_s(&S125), rt_s(&S126)), "next"); std_push(t151, t152); });
      }
      RT_AT(2, 201);
      RT_RET(rt_chk(s82_plain(up, v123_op, v115_start, v113_sp), "next"));
    }
    RT_AT(2, 203);
    v117_k = rt_add(v117_k, rt_i(1));
  }
  RT_AT(2, 205);
  if (rt_cond(rt_eq(v116_c, rt_s(&S127)))) {
    RT_AT(2, 206);
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S128), rt_s(&S129)), "next");
  }
  RT_AT(2, 210);
  if (rt_cond(rt_eq(v116_c, rt_s(&S130)))) {
    RT_AT(2, 211);
    wv v124_after = rt_bind(rt_chk(s77_ch(up, rt_add(v115_start, rt_i(1))), "next"), 0);
    RT_AT(2, 212);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(v113_sp, "||") || rt_bool(rt_eq(v124_after, rt_s(&S13)), "||")), "||") || rt_bool(rt_chk(w1_isSpace(v124_after), "next"), "||")), "||") || rt_bool(rt_chk(s79_startsWith(up, rt_add(v115_start, rt_i(1)), rt_s(&S131)), "next"), "||")), "||") || rt_bool(rt_chk(s79_startsWith(up, rt_add(v115_start, rt_i(1)), rt_s(&S132)), "next"), "||")))) {
      RT_AT(2, 213);
      (void)rt_chk(w2_failHint(v115_start, rt_s(&S133), rt_s(&S134)), "next");
    }
    RT_AT(2, 215);
    wv t153 = rt_add(v115_start, rt_i(1));
    (void)rt_def(up->v73_i, "i");
    up->v73_i = t153;
    RT_AT(2, 216);
    RT_RET(rt_chk(s82_plain(up, rt_s(&S130), v115_start, v113_sp), "next"));
  }
  RT_AT(2, 218);
  (void)({ wv t154 = v115_start; wv t155 = rt_cat(3, (wv[]){ rt_s(&S135), rt_text(v116_c), rt_s(&S114) }); rt_chk(w2_abort(t154, t155), "next"); });
  RT_RET(RT_NULL);
}

static wv s85_word(struct fr27 *up, wv v125_start, wv v126_sp) {
  rt_enter("word");
  v125_start = rt_bind(v125_start, 0);
  v126_sp = rt_bind(v126_sp, 0);
  RT_AT(2, 222);
  wv v127_j = rt_bind(v125_start, 1);
  RT_AT(2, 223);
  while (rt_cond((RT_AT(2, 223), rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v127_j), "word")), "word")))) {
    RT_AT(2, 223);
    v127_j = rt_add(v127_j, rt_i(1));
  }
  RT_AT(2, 224);
  wv v128_w = rt_bind(rt_chk(s78_cut(up, v125_start, v127_j), "word"), 0);
  RT_AT(2, 225);
  wv t156 = rt_bind(v127_j, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t156;
  RT_AT(2, 226);
  if (rt_cond(rt_eq(rt_chk(s77_ch(up, v127_j), "word"), rt_s(&S106)))) {
    RT_AT(2, 227);
    (void)rt_chk(w2_failHint(v127_j, rt_s(&S136), rt_s(&S137)), "word");
  }
  RT_AT(2, 229);
  if (rt_cond(({ wv t157 = rt_def(w3_keywords, "keywords"); wv t158 = v128_w; rt_chk(w1_contains(t157, t158), "word"); }))) {
    RT_AT(2, 229);
    RT_RET(rt_chk(s82_plain(up, v128_w, v125_start, v126_sp), "word"));
  }
  RT_AT(2, 230);
  wv v129_k = rt_i(0);
  RT_AT(2, 231);
  while (rt_cond((RT_AT(2, 231), ({ wv t159 = v129_k; wv t160 = std_length(rt_def(w3_reserved, "reserved")); rt_lt(t159, t160); })))) {
    RT_AT(2, 232);
    if (rt_cond(({ wv t163 = rt_get(&F_word, ({ wv t161 = v129_k; wv t162 = rt_def(w3_reserved, "reserved"); rt_idx(t161, t162); })); wv t164 = v128_w; rt_eq(t163, t164); }))) {
      RT_AT(2, 232);
      (void)({ wv t167 = v125_start; wv t168 = rt_get(&F_message, ({ wv t165 = v129_k; wv t166 = rt_def(w3_reserved, "reserved"); rt_idx(t165, t166); })); rt_chk(w2_abort(t167, t168), "word"); });
    }
    RT_AT(2, 233);
    v129_k = rt_add(v129_k, rt_i(1));
  }
  RT_RET((RT_AT(2, 235), rt_chk(s81_tok(up, rt_s(&S138), v125_start, v126_sp, v128_w, RT_NULL, RT_NULL), "word")));
}

static wv s86_number(struct fr27 *up, wv v130_start, wv v131_sp) {
  rt_enter("number");
  v130_start = rt_bind(v130_start, 0);
  v131_sp = rt_bind(v131_sp, 0);
  RT_AT(2, 240);
  wv v132_kind = rt_s(&S139);
  RT_AT(2, 241);
  wv v133_j = rt_bind(v130_start, 1);
  RT_AT(2, 242);
  wv v134_base = rt_i(10);
  RT_AT(2, 243);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v130_start), "number"), rt_s(&S4)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(v130_start, rt_i(1))), "number"), rt_s(&S140)), "||") || rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(v130_start, rt_i(1))), "number"), rt_s(&S141)), "||")), "&&")))) {
    RT_AT(2, 244);
    v134_base = rt_bind((rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v130_start, rt_i(1))), "number"), rt_s(&S140))) ? rt_i(16) : rt_i(2)), 1);
    RT_AT(2, 245);
    v133_j = rt_bind(({ wv t169 = up->v71_cs; wv t170 = rt_add(v130_start, rt_i(2)); wv t171 = v134_base; rt_chk(w3_digitRun(t169, t170, t171), "number"); }), 1);
    RT_AT(2, 246);
    if (rt_cond(({ wv t172 = v133_j; wv t173 = rt_add(v130_start, rt_i(2)); rt_eq(t172, t173); }))) {
      RT_AT(2, 247);
      (void)({ wv t178 = v130_start; wv t179 = ({ wv t176 = rt_text(({ wv t174 = v130_start; wv t175 = rt_add(v130_start, rt_i(2)); rt_chk(s78_cut(up, t174, t175), "number"); })); wv t177 = rt_text((rt_cond(rt_eq(v134_base, rt_i(2))) ? rt_s(&S143) : rt_s(&S144))); rt_cat(5, (wv[]){ rt_s(&S114), t176, rt_s(&S142), t177, rt_s(&S145) }); }); rt_chk(w2_abort(t178, t179), "number"); });
    }
  } else {
    RT_AT(2, 250);
    v133_j = rt_bind(rt_chk(w3_digitRun(up->v71_cs, v130_start, rt_i(10)), "number"), 1);
    RT_AT(2, 251);
    if (rt_cond(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S130)))) {
      RT_AT(2, 252);
      if (rt_cond(rt_not(rt_chk(w1_isDigit(rt_chk(s77_ch(up, rt_add(v133_j, rt_i(1))), "number")), "number")))) {
        RT_AT(2, 253);
        (void)({ wv t180 = v130_start; wv t181 = rt_cat(3, (wv[]){ rt_s(&S147), rt_text(rt_chk(s78_cut(up, v130_start, v133_j), "number")), rt_s(&S148) }); rt_chk(w2_failHint(t180, rt_s(&S146), t181), "number"); });
      }
      RT_AT(2, 255);
      v132_kind = rt_s(&S149);
      RT_AT(2, 256);
      v133_j = rt_bind(({ wv t182 = up->v71_cs; wv t183 = rt_add(v133_j, rt_i(1)); rt_chk(w3_digitRun(t182, t183, rt_i(10)), "number"); }), 1);
    }
    RT_AT(2, 258);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S150)), "||") || rt_bool(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S151)), "||")))) {
      RT_AT(2, 259);
      wv v137_e = rt_add(v133_j, rt_i(1));
      RT_AT(2, 260);
      if (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v137_e), "number"), rt_s(&S85)), "||") || rt_bool(rt_eq(rt_chk(s77_ch(up, v137_e), "number"), rt_s(&S86)), "||")))) {
        RT_AT(2, 260);
        v137_e = rt_add(v137_e, rt_i(1));
      }
      RT_AT(2, 261);
      wv v138_after = rt_bind(rt_chk(w3_digitRun(up->v71_cs, v137_e, rt_i(10)), "number"), 0);
      RT_AT(2, 262);
      if (rt_cond(rt_eq(v138_after, v137_e))) {
        RT_AT(2, 262);
        (void)rt_chk(w2_abort(v130_start, rt_s(&S152)), "number");
      }
      RT_AT(2, 263);
      v132_kind = rt_s(&S149);
      RT_AT(2, 264);
      v133_j = rt_bind(v138_after, 1);
    }
  }
  RT_AT(2, 267);
  if (rt_cond(rt_b(rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v133_j), "number")), "number"), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S130)), "&&") && rt_bool(rt_chk(w1_isDigit(rt_chk(s77_ch(up, rt_add(v133_j, rt_i(1))), "number")), "number"), "&&")), "||")))) {
    RT_AT(2, 268);
    wv v139_bad = rt_bind(v130_start, 1);
    RT_AT(2, 269);
    while (rt_cond((RT_AT(2, 269), rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v139_bad), "number"), rt_s(&S130)), "||") || rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v139_bad), "number")), "number"), "||"))))) {
      RT_AT(2, 269);
      v139_bad = rt_add(v139_bad, rt_i(1));
    }
    RT_AT(2, 270);
    (void)({ wv t184 = v130_start; wv t185 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_chk(s78_cut(up, v130_start, v139_bad), "number")), rt_s(&S153) }); rt_chk(w2_abort(t184, t185), "number"); });
  }
  RT_AT(2, 272);
  wv t186 = rt_bind(v133_j, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t186;
  RT_AT(2, 273);
  wv v135_spelling = rt_bind(rt_chk(s78_cut(up, v130_start, v133_j), "number"), 0);
  RT_AT(2, 274);
  if (rt_cond(rt_eq(v132_kind, rt_s(&S149)))) {
    RT_AT(2, 275);
    wv t187 = std_toFloat(v135_spelling);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        RT_AT(2, 276);
        (void)rt_chk(w2_abort(v130_start, rt_s(&S154)), "number");
        t187 = (RT_AT(2, 276), rt_i(0));
      }
    }
    RT_AT(2, 275);
    wv v140_f = rt_bind(t187, 0);
    RT_AT(2, 277);
    RT_RET(rt_chk(s81_tok(up, rt_s(&S149), v130_start, v131_sp, v140_f, v135_spelling, RT_NULL), "number"));
  }
  RT_AT(2, 279);
  if (rt_cond(rt_eq(v134_base, rt_i(10)))) {
    RT_AT(2, 280);
    wv t188 = std_toInt(v135_spelling);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        RT_AT(2, 281);
        (void)rt_chk(w2_abort(v130_start, rt_s(&S155)), "number");
        t188 = (RT_AT(2, 281), rt_i(0));
      }
    }
    RT_AT(2, 280);
    wv v141_v = rt_bind(t188, 0);
    RT_AT(2, 282);
    RT_RET(rt_chk(s81_tok(up, rt_s(&S139), v130_start, v131_sp, v141_v, v135_spelling, RT_NULL), "number"));
  }
  RT_AT(2, 284);
  wv v136_v = rt_bind(({ wv t191 = ({ wv t189 = std_split(v135_spelling, rt_s(&S13)); wv t190 = std_length(v135_spelling); std_slice(t189, rt_i(2), t190); }); wv t192 = v134_base; rt_chk(w3_digitsValue(t191, t192), "number"); }), 0);
  RT_AT(2, 285);
  if (rt_cond(rt_lt(v136_v, rt_i(0)))) {
    RT_AT(2, 285);
    (void)rt_chk(w2_abort(v130_start, rt_s(&S155)), "number");
  }
  RT_RET((RT_AT(2, 286), rt_chk(s81_tok(up, rt_s(&S139), v130_start, v131_sp, v136_v, v135_spelling, RT_NULL), "number")));
}

static wv s87_sigil(struct fr27 *up, wv v142_start, wv v143_sp) {
  rt_enter("sigil");
  v142_start = rt_bind(v142_start, 0);
  v143_sp = rt_bind(v143_sp, 0);
  RT_AT(2, 290);
  if (rt_cond(rt_not(rt_chk(w1_isIdStart(rt_chk(s77_ch(up, rt_add(v142_start, rt_i(1))), "sigil")), "sigil")))) {
    RT_AT(2, 291);
    (void)rt_chk(w2_failHint(v142_start, rt_s(&S156), rt_s(&S137)), "sigil");
  }
  RT_AT(2, 294);
  wv v144_j = rt_add(v142_start, rt_i(1));
  RT_AT(2, 295);
  while (rt_cond((RT_AT(2, 295), rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v144_j), "sigil")), "sigil")))) {
    RT_AT(2, 295);
    v144_j = rt_add(v144_j, rt_i(1));
  }
  RT_AT(2, 296);
  wv t193 = rt_bind(v144_j, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t193;
  RT_RET((RT_AT(2, 297), ({ wv t196 = v142_start; wv t197 = v143_sp; wv t198 = ({ wv t194 = rt_add(v142_start, rt_i(1)); wv t195 = v144_j; rt_chk(s78_cut(up, t194, t195), "sigil"); }); rt_chk(s81_tok(up, rt_s(&S157), t196, t197, t198, RT_NULL, RT_NULL), "sigil"); })));
}

static wv s88_string(struct fr27 *up, wv v145_start, wv v146_sp) {
  rt_enter("string");
  v145_start = rt_bind(v145_start, 0);
  v146_sp = rt_bind(v146_sp, 0);
  RT_AT(2, 301);
  wv v147_parts = rt_arr(0, NULL, 1);
  RT_AT(2, 302);
  wv v148_buf = rt_s(&S13);
  RT_AT(2, 303);
  wv v149_rawDollar = rt_i(-1);
  RT_AT(2, 306);
  wv v150_importPath = rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ge(std_length(rt_def(up->v76_toks, "toks")), rt_i(3)), "&&") && rt_bool(rt_eq(rt_get(&F_t, rt_idx(rt_i(-1), rt_def(up->v76_toks, "toks"))), rt_s(&S138)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_v, rt_idx(rt_i(-1), rt_def(up->v76_toks, "toks"))), rt_s(&S158)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, rt_idx(rt_i(-3), rt_def(up->v76_toks, "toks"))), rt_s(&S48)), "&&"));
  RT_AT(2, 307);
  wv v151_k = rt_add(v145_start, rt_i(1));
  RT_AT(2, 308);
  while (rt_cond((RT_AT(2, 308), rt_b(1)))) {
    RT_AT(2, 309);
    wv v152_c = rt_bind(rt_chk(s77_ch(up, v151_k), "string"), 0);
    RT_AT(2, 310);
    if (rt_cond(rt_eq(v152_c, rt_s(&S13)))) {
      RT_AT(2, 310);
      (void)rt_chk(w2_abort(v145_start, rt_s(&S159)), "string");
    }
    RT_AT(2, 311);
    if (rt_cond(rt_b(rt_bool(rt_eq(v152_c, rt_s(&S2)), "||") || rt_bool(rt_eq(v152_c, rt_s(&S3)), "||")))) {
      RT_AT(2, 311);
      (void)rt_chk(w2_failHint(v151_k, rt_s(&S160), rt_s(&S161)), "string");
    }
    RT_AT(2, 312);
    if (rt_cond(rt_eq(v152_c, rt_s(&S18)))) {
      RT_AT(2, 313);
      v151_k = rt_add(v151_k, rt_i(1));
      break;
    }
    RT_AT(2, 316);
    if (rt_cond(rt_eq(v152_c, rt_s(&S19)))) {
      RT_AT(2, 317);
      wv v153_e = rt_bind(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string"), 0);
      RT_AT(2, 318);
      wv v154_simple = rt_bind(rt_chk(w3_simpleEscape(v153_e), "string"), 0);
      RT_AT(2, 319);
      if (rt_cond(rt_ne(v154_simple, RT_NULL))) {
        RT_AT(2, 320);
        v148_buf = rt_add(v148_buf, v154_simple);
        RT_AT(2, 321);
        v151_k = rt_add(v151_k, rt_i(2));
        continue;
      }
      RT_AT(2, 324);
      if (rt_cond(rt_b(rt_bool(rt_eq(v153_e, rt_s(&S162)), "&&") && rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(2))), "string"), rt_s(&S101)), "&&")))) {
        RT_AT(2, 325);
        wv v155_close = rt_bind(rt_chk(s80_find(up, rt_s(&S15), rt_add(v151_k, rt_i(3))), "string"), 0);
        RT_AT(2, 326);
        wv v156_hex = rt_bind((rt_cond(rt_lt(v155_close, rt_i(0))) ? rt_s(&S13) : ({ wv t199 = rt_add(v151_k, rt_i(3)); wv t200 = v155_close; rt_chk(s78_cut(up, t199, t200), "string"); })), 0);
        RT_AT(2, 327);
        wv v157_cp = rt_bind(rt_chk(w1_hexValue(v156_hex), "string"), 0);
        RT_AT(2, 328);
        if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_lt(v157_cp, rt_i(0)), "||") || rt_bool(rt_gt(v157_cp, rt_i(1114111)), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v157_cp, rt_i(55296)), "&&") && rt_bool(rt_le(v157_cp, rt_i(57343)), "&&")), "||")))) {
          RT_AT(2, 329);
          (void)rt_chk(w2_failHint(v151_k, rt_s(&S163), rt_s(&S164)), "string");
        }
        RT_AT(2, 332);
        v148_buf = ({ wv t201 = v148_buf; wv t202 = std_fromCode(v157_cp); rt_add(t201, t202); });
        RT_AT(2, 333);
        v151_k = rt_add(v155_close, rt_i(1));
        continue;
      }
      RT_AT(2, 336);
      (void)({ wv t203 = v151_k; wv t204 = rt_cat(3, (wv[]){ rt_s(&S165), rt_text(v153_e), rt_s(&S114) }); rt_chk(w2_failHint(t203, t204, rt_s(&S166)), "string"); });
    }
    RT_AT(2, 338);
    if (rt_cond(rt_eq(v152_c, rt_s(&S101)))) {
      RT_AT(2, 339);
      if (rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string"), rt_s(&S101)))) {
        RT_AT(2, 340);
        v148_buf = rt_add(v148_buf, rt_s(&S101));
        RT_AT(2, 341);
        v151_k = rt_add(v151_k, rt_i(2));
        continue;
      }
      RT_AT(2, 344);
      if (rt_cond(({ wv t205 = v149_rawDollar; wv t206 = rt_sub(v151_k, rt_i(1)); rt_eq(t205, t206); }))) {
        RT_AT(2, 345);
        (void)({ wv t207 = w2_warnings; wv t208 = rt_chk(w2_warn(v149_rawDollar, rt_s(&S167), RT_NULL), "string"); std_push(t207, t208); });
      }
      RT_AT(2, 347);
      (void)std_push(v147_parts, v148_buf);
      RT_AT(2, 348);
      v148_buf = rt_s(&S13);
      RT_AT(2, 349);
      wv v158_sub = rt_bind(rt_chk(s89_interpolation(up, v151_k), "string"), 0);
      RT_AT(2, 350);
      (void)({ wv t211 = v147_parts; wv t212 = ({ wv t209 = rt_get(&F_tokens, v158_sub); wv t210 = v151_k; rt_rec(0, 2, K3, (wv[]){ t209, t210 }); }); std_push(t211, t212); });
      RT_AT(2, 351);
      v151_k = rt_bind(rt_get(&F_end, v158_sub), 1);
      continue;
    }
    RT_AT(2, 354);
    if (rt_cond(rt_eq(v152_c, rt_s(&S15)))) {
      RT_AT(2, 355);
      if (rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string"), rt_s(&S15)))) {
        RT_AT(2, 356);
        v148_buf = rt_add(v148_buf, rt_s(&S15));
        RT_AT(2, 357);
        v151_k = rt_add(v151_k, rt_i(2));
        continue;
      }
      RT_AT(2, 360);
      (void)rt_chk(w2_failHint(v151_k, rt_s(&S168), rt_s(&S169)), "string");
    }
    RT_AT(2, 362);
    if (rt_cond(rt_eq(v152_c, rt_s(&S106)))) {
      RT_AT(2, 363);
      v149_rawDollar = rt_bind(v151_k, 1);
      RT_AT(2, 364);
      if (rt_cond(rt_b(rt_bool(rt_not(v150_importPath), "&&") && rt_bool(rt_chk(w1_isIdStart(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string")), "string"), "&&")))) {
        RT_AT(2, 365);
        wv v159_j = rt_add(v151_k, rt_i(1));
        RT_AT(2, 366);
        while (rt_cond((RT_AT(2, 366), rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v159_j), "string")), "string")))) {
          RT_AT(2, 366);
          v159_j = rt_add(v159_j, rt_i(1));
        }
        RT_AT(2, 367);
        wv v160_name = rt_bind(({ wv t213 = rt_add(v151_k, rt_i(1)); wv t214 = v159_j; rt_chk(s78_cut(up, t213, t214), "string"); }), 0);
        RT_AT(2, 368);
        (void)({ wv t217 = w2_warnings; wv t218 = ({ wv t215 = v151_k; wv t216 = rt_cat(3, (wv[]){ rt_s(&S170), rt_text(v160_name), rt_s(&S171) }); rt_chk(w2_warn(t215, t216, rt_s(&S172)), "string"); }); std_push(t217, t218); });
      }
    }
    RT_AT(2, 371);
    v148_buf = rt_add(v148_buf, v152_c);
    RT_AT(2, 372);
    v151_k = rt_add(v151_k, rt_i(1));
  }
  RT_AT(2, 374);
  (void)std_push(v147_parts, v148_buf);
  RT_AT(2, 375);
  wv t219 = rt_bind(v151_k, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t219;
  RT_RET((RT_AT(2, 376), rt_chk(s81_tok(up, rt_s(&S173), v145_start, v146_sp, RT_NULL, RT_NULL, v147_parts), "string")));
}

static wv s89_interpolation(struct fr27 *up, wv v161_open) {
  rt_enter("interpolation");
  v161_open = rt_bind(v161_open, 0);
  RT_AT(2, 382);
  wv v162_saveI = rt_bind(rt_def(up->v73_i, "i"), 0);
  RT_AT(2, 383);
  wv v163_savePrev = rt_bind(rt_def(up->v74_prevT, "prevT"), 0);
  RT_AT(2, 384);
  wv v164_saveNl = rt_bind(rt_def(up->v75_nlNow, "nlNow"), 0);
  RT_AT(2, 385);
  wv t220 = rt_add(v161_open, rt_i(1));
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t220;
  RT_AT(2, 386);
  wv t221 = rt_s(&S13);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t221;
  RT_AT(2, 387);
  wv v165_sub = rt_arr(0, NULL, 1);
  RT_AT(2, 388);
  wv v166_depth = rt_i(0);
  RT_AT(2, 389);
  while (rt_cond((RT_AT(2, 389), rt_b(1)))) {
    RT_AT(2, 390);
    wv v168_tk = rt_bind(rt_chk(s84_next(up), "interpolation"), 0);
    RT_AT(2, 391);
    if (rt_cond(rt_eq(rt_get(&F_t, v168_tk), rt_s(&S111)))) {
      RT_AT(2, 391);
      (void)rt_chk(w2_abort(v161_open, rt_s(&S174)), "interpolation");
    }
    RT_AT(2, 392);
    if (rt_cond(rt_eq(rt_get(&F_t, v168_tk), rt_s(&S15)))) {
      RT_AT(2, 393);
      if (rt_cond(rt_eq(v166_depth, rt_i(0)))) {
        RT_AT(2, 394);
        (void)({ wv t226 = v165_sub; wv t227 = ({ wv t222 = rt_get(&F_pos, v168_tk); wv t223 = rt_get(&F_end, v168_tk); wv t224 = rt_get(&F_sp, v168_tk); wv t225 = rt_get(&F_nl, v168_tk); rt_rec(0, 8, K2, (wv[]){ rt_s(&S111), t222, t223, t224, t225, RT_NULL, RT_NULL, RT_NULL }); }); std_push(t226, t227); });
        break;
      }
      RT_AT(2, 397);
      v166_depth = rt_sub(v166_depth, rt_i(1));
    } else if (rt_cond(rt_eq(rt_get(&F_t, v168_tk), rt_s(&S101)))) {
      RT_AT(2, 399);
      v166_depth = rt_add(v166_depth, rt_i(1));
    }
    RT_AT(2, 401);
    (void)std_push(v165_sub, v168_tk);
  }
  RT_AT(2, 403);
  wv v167_endI = rt_bind(rt_def(up->v73_i, "i"), 0);
  RT_AT(2, 404);
  wv t228 = rt_bind(v162_saveI, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t228;
  RT_AT(2, 405);
  wv t229 = rt_bind(v163_savePrev, 1);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t229;
  RT_AT(2, 406);
  wv t230 = rt_bind(v164_saveNl, 1);
  (void)rt_def(up->v75_nlNow, "nlNow");
  up->v75_nlNow = t230;
  RT_AT(2, 407);
  if (rt_cond(rt_eq(std_length(v165_sub), rt_i(1)))) {
    RT_AT(2, 407);
    (void)rt_chk(w2_failHint(v161_open, rt_s(&S175), rt_s(&S176)), "interpolation");
  }
  RT_RET((RT_AT(2, 408), rt_rec(0, 2, K4, (wv[]){ v165_sub, v167_endI })));
}

static wv w4_precedence(wv v170_t) {
  rt_enter("precedence");
  v170_t = rt_bind(v170_t, 0);
  RT_AT(3, 34);
  if (rt_cond(rt_eq(v170_t, rt_s(&S82)))) {
    RT_AT(3, 34);
    RT_RET(rt_i(1));
  }
  RT_AT(3, 35);
  if (rt_cond(rt_eq(v170_t, rt_s(&S81)))) {
    RT_AT(3, 35);
    RT_RET(rt_i(2));
  }
  RT_AT(3, 36);
  if (rt_cond(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S77)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S78)), "||")))) {
    RT_AT(3, 36);
    RT_RET(rt_i(3));
  }
  RT_AT(3, 37);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S90)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S91)), "||")), "||") || rt_bool(rt_eq(v170_t, rt_s(&S79)), "||")), "||") || rt_bool(rt_eq(v170_t, rt_s(&S80)), "||")))) {
    RT_AT(3, 37);
    RT_RET(rt_i(4));
  }
  RT_AT(3, 38);
  if (rt_cond(rt_eq(v170_t, rt_s(&S93)))) {
    RT_AT(3, 38);
    RT_RET(rt_i(5));
  }
  RT_AT(3, 39);
  if (rt_cond(rt_eq(v170_t, rt_s(&S32)))) {
    RT_AT(3, 39);
    RT_RET(rt_i(6));
  }
  RT_AT(3, 40);
  if (rt_cond(rt_eq(v170_t, rt_s(&S92)))) {
    RT_AT(3, 40);
    RT_RET(rt_i(7));
  }
  RT_AT(3, 41);
  if (rt_cond(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S83)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S84)), "||")))) {
    RT_AT(3, 41);
    RT_RET(rt_i(8));
  }
  RT_AT(3, 42);
  if (rt_cond(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S85)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S86)), "||")))) {
    RT_AT(3, 42);
    RT_RET(rt_i(9));
  }
  RT_AT(3, 43);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S87)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S88)), "||")), "||") || rt_bool(rt_eq(v170_t, rt_s(&S89)), "||")))) {
    RT_AT(3, 43);
    RT_RET(rt_i(10));
  }
  RT_RET((RT_AT(3, 44), rt_i(0)));
}

static wv w4_describe(wv v171_tk) {
  rt_enter("describe");
  v171_tk = rt_bind(v171_tk, 0);
  RT_AT(3, 48);
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S111)))) {
    RT_AT(3, 48);
    RT_RET(rt_s(&S180));
  }
  RT_AT(3, 49);
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S138)))) {
    RT_AT(3, 49);
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_v, v171_tk)), rt_s(&S114) }));
  }
  RT_AT(3, 50);
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S157)))) {
    RT_AT(3, 50);
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S181), rt_text(rt_get(&F_v, v171_tk)), rt_s(&S114) }));
  }
  RT_AT(3, 51);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S139)), "||") || rt_bool(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S149)), "||")))) {
    RT_AT(3, 51);
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_text, v171_tk)), rt_s(&S114) }));
  }
  RT_AT(3, 52);
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S173)))) {
    RT_AT(3, 52);
    RT_RET(rt_s(&S182));
  }
  RT_AT(3, 53);
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S112)))) {
    RT_AT(3, 53);
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_v, v171_tk)), rt_s(&S183) }));
  }
  RT_RET((RT_AT(3, 54), rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_t, v171_tk)), rt_s(&S114) })));
}

static wv w4_failAtHint(wv v172_tk, wv v173_message, wv v174_hint) {
  rt_enter("failAtHint");
  v172_tk = rt_bind(v172_tk, 0);
  v173_message = rt_bind(v173_message, 0);
  v174_hint = rt_bind(v174_hint, 0);
  RT_AT(3, 57);
  (void)({ wv t231 = rt_get(&F_pos, v172_tk); wv t232 = v173_message; wv t233 = v174_hint; rt_chk(w2_failHint(t231, t232, t233), "failAtHint"); });
  RT_RET(RT_NULL);
}

static wv w4_failAt(wv v175_tk, wv v176_message) {
  rt_enter("failAt");
  v175_tk = rt_bind(v175_tk, 0);
  v176_message = rt_bind(v176_message, 0);
  RT_AT(3, 58);
  (void)({ wv t234 = rt_get(&F_pos, v175_tk); wv t235 = v176_message; rt_chk(w2_failHint(t234, t235, RT_NULL), "failAt"); });
  RT_RET(RT_NULL);
}

static wv w4_simpleStatement(wv v177_k, wv v178_e) {
  rt_enter("simpleStatement");
  v177_k = rt_bind(v177_k, 0);
  v178_e = rt_bind(v178_e, 0);
  RT_RET((RT_AT(3, 60), rt_rec(0, 5, K5, (wv[]){ v177_k, v178_e, RT_NULL, RT_NULL, RT_NULL })));
}

static wv w4_parse(wv v179_toks) {
  rt_enter("parse");
  RT_RET((RT_AT(3, 62), rt_chk(w4_parser(v179_toks, rt_b(0)), "parse")));
}

static wv w4_hasEffect(wv v180_e) {
  rt_enter("hasEffect");
  v180_e = rt_bind(v180_e, 0);
  RT_AT(3, 67);
  wv v181_kind = rt_bind(rt_get(&F_k, v180_e), 0);
  RT_AT(3, 68);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v181_kind, rt_s(&S184)), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S185)), "||")), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S186)), "||")), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S45)), "||")))) {
    RT_AT(3, 68);
    RT_RET(rt_b(1));
  }
  RT_AT(3, 69);
  if (rt_cond(rt_eq(v181_kind, rt_s(&S173)))) {
    RT_AT(3, 70);
    wv v182_i = rt_i(0);
    RT_AT(3, 71);
    while (rt_cond((RT_AT(3, 71), ({ wv t236 = v182_i; wv t237 = std_length(rt_get(&F_parts, v180_e)); rt_lt(t236, t237); })))) {
      RT_AT(3, 72);
      if (rt_cond(rt_b(rt_bool(rt_ne(std_typeOf(({ wv t238 = v182_i; wv t239 = rt_get(&F_parts, v180_e); rt_idx(t238, t239); })), rt_s(&S187)), "&&") && rt_bool(rt_chk(w4_hasEffect(({ wv t240 = v182_i; wv t241 = rt_get(&F_parts, v180_e); rt_idx(t240, t241); })), "hasEffect"), "&&")))) {
        RT_AT(3, 72);
        RT_RET(rt_b(1));
      }
      RT_AT(3, 73);
      v182_i = rt_add(v182_i, rt_i(1));
    }
    RT_AT(3, 75);
    RT_RET(rt_b(0));
  }
  RT_AT(3, 77);
  if (rt_cond(rt_b(rt_bool(rt_eq(v181_kind, rt_s(&S188)), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S189)), "||")))) {
    RT_AT(3, 78);
    wv v183_i = rt_i(0);
    RT_AT(3, 79);
    while (rt_cond((RT_AT(3, 79), ({ wv t242 = v183_i; wv t243 = std_length(rt_get(&F_items, v180_e)); rt_lt(t242, t243); })))) {
      RT_AT(3, 80);
      if (rt_cond(rt_chk(w4_hasEffect(({ wv t244 = v183_i; wv t245 = rt_get(&F_items, v180_e); rt_idx(t244, t245); })), "hasEffect"))) {
        RT_AT(3, 80);
        RT_RET(rt_b(1));
      }
      RT_AT(3, 81);
      v183_i = rt_add(v183_i, rt_i(1));
    }
    RT_AT(3, 83);
    RT_RET(rt_b(0));
  }
  RT_AT(3, 85);
  if (rt_cond(rt_eq(v181_kind, rt_s(&S190)))) {
    RT_AT(3, 86);
    wv v184_i = rt_i(0);
    RT_AT(3, 87);
    while (rt_cond((RT_AT(3, 87), ({ wv t246 = v184_i; wv t247 = std_length(rt_get(&F_fields, v180_e)); rt_lt(t246, t247); })))) {
      RT_AT(3, 88);
      if (rt_cond(rt_chk(w4_hasEffect(rt_get(&F_e, ({ wv t248 = v184_i; wv t249 = rt_get(&F_fields, v180_e); rt_idx(t248, t249); }))), "hasEffect"))) {
        RT_AT(3, 88);
        RT_RET(rt_b(1));
      }
      RT_AT(3, 89);
      v184_i = rt_add(v184_i, rt_i(1));
    }
    RT_AT(3, 91);
    RT_RET(rt_b(0));
  }
  RT_AT(3, 93);
  if (rt_cond(rt_eq(v181_kind, rt_s(&S191)))) {
    RT_AT(3, 93);
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_index, v180_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_arr, v180_e)), "hasEffect"), "||")));
  }
  RT_AT(3, 94);
  if (rt_cond(rt_eq(v181_kind, rt_s(&S192)))) {
    RT_AT(3, 94);
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_rec, v180_e)), "hasEffect"));
  }
  RT_AT(3, 95);
  if (rt_cond(rt_eq(v181_kind, rt_s(&S193)))) {
    RT_AT(3, 95);
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_e, v180_e)), "hasEffect"));
  }
  RT_AT(3, 96);
  if (rt_cond(rt_eq(v181_kind, rt_s(&S194)))) {
    RT_AT(3, 96);
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_l, v180_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_r, v180_e)), "hasEffect"), "||")));
  }
  RT_AT(3, 97);
  if (rt_cond(rt_eq(v181_kind, rt_s(&S195)))) {
    RT_AT(3, 97);
    RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_c, v180_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_a, v180_e)), "hasEffect"), "||")), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_b, v180_e)), "hasEffect"), "||")));
  }
  RT_RET((RT_AT(3, 98), rt_b(0)));
}

static wv w4_parser(wv p_v185_toks, wv v186_inner) {
  rt_enter("parser");
  struct fr49 fr = { 0 };
  fr.v185_toks = p_v185_toks;
  v186_inner = rt_bind(v186_inner, 0);
  RT_AT(3, 103);
  fr.v187_p = rt_i(0);
  RT_AT(3, 105);
  fr.v188_depth = rt_i(0);
  RT_AT(3, 107);
  fr.v189_exports = rt_arr(0, NULL, 1);
  RT_AT(3, 108);
  fr.v190_firstExport = RT_NULL;
  RT_AT(3, 851);
  if (rt_cond(v186_inner)) {
    RT_AT(3, 852);
    wv v381_e = rt_bind(rt_chk(s215_expr(&fr), "parser"), 0);
    RT_AT(3, 853);
    if (rt_cond(rt_not(rt_chk(s192_at(&fr, rt_s(&S111)), "parser")))) {
      RT_AT(3, 854);
      wv v382_tk = rt_bind(rt_chk(s191_peek(&fr), "parser"), 0);
      RT_AT(3, 855);
      (void)({ wv t550 = v382_tk; wv t551 = rt_cat(3, (wv[]){ rt_s(&S351), rt_text(rt_chk(w4_describe(v382_tk), "parser")), rt_s(&S352) }); rt_chk(w4_failAt(t550, t551), "parser"); });
    }
    RT_AT(3, 857);
    RT_RET(v381_e);
  }
  RT_RET((RT_AT(3, 859), rt_chk(s200_program(&fr), "parser")));
}

static wv s191_peek(struct fr49 *up) {
  rt_enter("peek");
  RT_RET((RT_AT(3, 110), ({ wv t250 = rt_def(up->v187_p, "p"); wv t251 = up->v185_toks; rt_idx(t250, t251); })));
}

static wv s192_at(struct fr49 *up, wv v231_type) {
  rt_enter("at");
  v231_type = rt_bind(v231_type, 0);
  RT_RET((RT_AT(3, 111), ({ wv t254 = rt_get(&F_t, ({ wv t252 = rt_def(up->v187_p, "p"); wv t253 = up->v185_toks; rt_idx(t252, t253); })); wv t255 = v231_type; rt_eq(t254, t255); })));
}

static wv s193_atWord(struct fr49 *up, wv v232_word) {
  rt_enter("atWord");
  v232_word = rt_bind(v232_word, 0);
  RT_RET((RT_AT(3, 113), rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t256 = rt_def(up->v187_p, "p"); wv t257 = up->v185_toks; rt_idx(t256, t257); })), rt_s(&S138)), "&&") && rt_bool(({ wv t260 = rt_get(&F_v, ({ wv t258 = rt_def(up->v187_p, "p"); wv t259 = up->v185_toks; rt_idx(t258, t259); })); wv t261 = v232_word; rt_eq(t260, t261); }), "&&"))));
}

static wv s194_atCall(struct fr49 *up) {
  rt_enter("atCall");
  RT_AT(3, 119);
  if (rt_cond(rt_b(rt_bool(({ wv t262 = rt_add(rt_def(up->v187_p, "p"), rt_i(2)); wv t263 = std_length(up->v185_toks); rt_ge(t262, t263); }), "||") || rt_bool(rt_ne(rt_get(&F_t, ({ wv t264 = rt_def(up->v187_p, "p"); wv t265 = up->v185_toks; rt_idx(t264, t265); })), rt_s(&S98)), "||")))) {
    RT_AT(3, 119);
    RT_RET(rt_b(0));
  }
  RT_AT(3, 120);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t266 = rt_add(rt_def(up->v187_p, "p"), rt_i(2)); wv t267 = up->v185_toks; rt_idx(t266, t267); })), rt_s(&S16)), "&&") && rt_bool(({ wv t270 = rt_def(w4_callees, "callees"); wv t271 = rt_get(&F_t, ({ wv t268 = rt_add(rt_def(up->v187_p, "p"), rt_i(1)); wv t269 = up->v185_toks; rt_idx(t268, t269); })); rt_chk(w1_contains(t270, t271), "atCall"); }), "&&")))) {
    RT_AT(3, 120);
    RT_RET(rt_b(1));
  }
  RT_RET((RT_AT(3, 123), rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(({ wv t272 = rt_add(rt_def(up->v187_p, "p"), rt_i(4)); wv t273 = std_length(up->v185_toks); rt_lt(t272, t273); }), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t274 = rt_add(rt_def(up->v187_p, "p"), rt_i(1)); wv t275 = up->v185_toks; rt_idx(t274, t275); })), rt_s(&S138)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t276 = rt_add(rt_def(up->v187_p, "p"), rt_i(2)); wv t277 = up->v185_toks; rt_idx(t276, t277); })), rt_s(&S130)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t278 = rt_add(rt_def(up->v187_p, "p"), rt_i(3)); wv t279 = up->v185_toks; rt_idx(t278, t279); })), rt_s(&S138)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t280 = rt_add(rt_def(up->v187_p, "p"), rt_i(4)); wv t281 = up->v185_toks; rt_idx(t280, t281); })), rt_s(&S16)), "&&"))));
}

static wv s195_next(struct fr49 *up) {
  rt_enter("next");
  RT_AT(3, 126);
  wv v233_tk = rt_bind(({ wv t282 = rt_def(up->v187_p, "p"); wv t283 = up->v185_toks; rt_idx(t282, t283); }), 0);
  RT_AT(3, 127);
  if (rt_cond(rt_ne(rt_get(&F_t, v233_tk), rt_s(&S111)))) {
    RT_AT(3, 127);
    up->v187_p = rt_add(rt_def(up->v187_p, "p"), rt_i(1));
  }
  RT_RET((RT_AT(3, 128), v233_tk));
}

static wv s196_accept(struct fr49 *up, wv v234_type) {
  rt_enter("accept");
  v234_type = rt_bind(v234_type, 0);
  RT_RET((RT_AT(3, 130), (rt_cond(rt_chk(s192_at(up, v234_type), "accept")) ? rt_chk(s195_next(up), "accept") : RT_NULL)));
}

static wv s197_expect(struct fr49 *up, wv v235_type, wv v236_msg, wv v237_hint) {
  rt_enter("expect");
  v235_type = rt_bind(v235_type, 0);
  v236_msg = rt_bind(v236_msg, 0);
  v237_hint = rt_bind(v237_hint, 0);
  RT_AT(3, 133);
  if (rt_cond(rt_chk(s192_at(up, v235_type), "expect"))) {
    RT_AT(3, 133);
    RT_RET(rt_chk(s195_next(up), "expect"));
  }
  RT_AT(3, 134);
  wv v238_found = rt_bind(rt_chk(s191_peek(up), "expect"), 0);
  RT_AT(3, 135);
  wv v239_m = rt_bind((rt_cond(rt_eq(v236_msg, RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S196), rt_text(v235_type), rt_s(&S114) }) : v236_msg), 0);
  RT_AT(3, 136);
  (void)({ wv t286 = v238_found; wv t287 = ({ wv t284 = rt_text(v239_m); wv t285 = rt_text(rt_chk(w4_describe(v238_found), "expect")); rt_cat(3, (wv[]){ t284, rt_s(&S197), t285 }); }); wv t288 = v237_hint; rt_chk(w4_failAtHint(t286, t287, t288), "expect"); });
  RT_RET(RT_NULL);
}

static wv s198_expectWord(struct fr49 *up, wv v240_word, wv v241_hint) {
  rt_enter("expectWord");
  v240_word = rt_bind(v240_word, 0);
  v241_hint = rt_bind(v241_hint, 0);
  RT_AT(3, 140);
  if (rt_cond(rt_chk(s193_atWord(up, v240_word), "expectWord"))) {
    RT_AT(3, 140);
    RT_RET(rt_chk(s195_next(up), "expectWord"));
  }
  RT_AT(3, 141);
  wv v242_found = rt_bind(rt_chk(s191_peek(up), "expectWord"), 0);
  RT_AT(3, 142);
  (void)({ wv t291 = v242_found; wv t292 = ({ wv t289 = rt_text(v240_word); wv t290 = rt_text(rt_chk(w4_describe(v242_found), "expectWord")); rt_cat(4, (wv[]){ rt_s(&S196), t289, rt_s(&S198), t290 }); }); wv t293 = v241_hint; rt_chk(w4_failAtHint(t291, t292, t293), "expectWord"); });
  RT_RET(RT_NULL);
}

static wv s199_lintSpace(struct fr49 *up, wv v243_op) {
  rt_enter("lintSpace");
  v243_op = rt_bind(v243_op, 0);
  RT_AT(3, 145);
  if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_sp, v243_op)), "||") || rt_bool(rt_not(rt_get(&F_sp, rt_chk(s191_peek(up), "lintSpace"))), "||")))) {
    RT_AT(3, 146);
    (void)({ wv t296 = w2_warnings; wv t297 = ({ wv t294 = rt_get(&F_pos, v243_op); wv t295 = rt_cat(3, (wv[]){ rt_s(&S199), rt_text(rt_get(&F_t, v243_op)), rt_s(&S114) }); rt_chk(w2_warn(t294, t295, RT_NULL), "lintSpace"); }); std_push(t296, t297); });
  }
  RT_RET(RT_NULL);
}

static wv s200_program(struct fr49 *up) {
  rt_enter("program");
  RT_AT(3, 153);
  wv v244_stmts = rt_arr(0, NULL, 1);
  RT_AT(3, 155);
  wv v245_started = rt_b(0);
  RT_AT(3, 157);
  wv v246_ended = RT_NULL;
  RT_AT(3, 158);
  while (rt_cond((RT_AT(3, 158), rt_not(rt_chk(s192_at(up, rt_s(&S111)), "program"))))) {
    RT_AT(3, 159);
    if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S14)), "program")))) {
      RT_AT(3, 160);
      wv v248_tk = rt_bind(rt_chk(s191_peek(up), "program"), 0);
      RT_AT(3, 161);
      wv v249_hint = rt_bind((rt_cond(rt_chk(s192_at(up, rt_s(&S98)), "program")) ? rt_s(&S200) : rt_s(&S201)), 0);
      RT_AT(3, 163);
      (void)({ wv t298 = v248_tk; wv t299 = rt_cat(2, (wv[]){ rt_s(&S202), rt_text(rt_chk(w4_describe(v248_tk), "program")) }); wv t300 = v249_hint; rt_chk(w4_failAtHint(t298, t299, t300), "program"); });
    }
    RT_AT(3, 165);
    wv v247_s = rt_bind(rt_chk(s201_statement(up), "program"), 0);
    RT_AT(3, 166);
    if (rt_cond(rt_b(rt_bool(rt_ne(v246_ended, RT_NULL), "&&") && rt_bool(rt_ne(rt_get(&F_k, v247_s), rt_s(&S203)), "&&")))) {
      RT_AT(3, 167);
      (void)rt_chk(w4_failAt(v246_ended, rt_s(&S204)), "program");
    }
    RT_AT(3, 169);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v247_s), rt_s(&S48)), "&&") && rt_bool(v245_started, "&&")))) {
      RT_AT(3, 169);
      (void)rt_chk(w2_abort(rt_get(&F_pos, v247_s), rt_s(&S205)), "program");
    }
    RT_AT(3, 170);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, v247_s), rt_s(&S48)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v247_s), rt_s(&S203)), "&&")))) {
      RT_AT(3, 170);
      v245_started = rt_b(1);
    }
    RT_AT(3, 171);
    if (rt_cond(rt_eq(rt_get(&F_k, v247_s), rt_s(&S206)))) {
      RT_AT(3, 171);
      v246_ended = rt_rec(1, 1, K6, (wv[]){ rt_get(&F_pos, v247_s) });
    }
    RT_AT(3, 172);
    (void)std_push(v244_stmts, v247_s);
  }
  RT_AT(3, 174);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_def(up->v190_firstExport, "firstExport"), RT_NULL), "&&") && rt_bool(rt_eq(v246_ended, RT_NULL), "&&")))) {
    RT_AT(3, 175);
    (void)rt_chk(w4_failAt(rt_def(up->v190_firstExport, "firstExport"), rt_s(&S207)), "program");
  }
  RT_RET((RT_AT(3, 177), ({ wv t301 = v244_stmts; wv t302 = rt_get(&F_pos, rt_chk(s191_peek(up), "program")); rt_rec(0, 2, K7, (wv[]){ t301, t302 }); })));
}

static wv s201_statement(struct fr49 *up) {
  rt_enter("statement");
  RT_AT(3, 181);
  wv v250_semi = rt_bind(rt_chk(s195_next(up), "statement"), 0);
  RT_AT(3, 182);
  wv v251_tk = rt_bind(rt_chk(s191_peek(up), "statement"), 0);
  RT_AT(3, 183);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v251_tk), rt_s(&S14)), "||") || rt_bool(rt_eq(rt_get(&F_t, v251_tk), rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v251_tk), rt_s(&S111)), "||")))) {
    RT_AT(3, 184);
    RT_RET(({ wv t303 = rt_get(&F_pos, v250_semi); wv t304 = rt_get(&F_pos, v251_tk); rt_rec(0, 4, K8, (wv[]){ rt_s(&S203), t303, t304, RT_NULL }); }));
  }
  RT_AT(3, 186);
  wv v252_s = rt_bind(rt_chk(s202_statementBody(up, v251_tk), "statement"), 1);
  RT_AT(3, 187);
  ({ wv t305 = rt_get(&F_pos, v251_tk); wv t306 = v252_s; rt_set_mem(t305, &F_pos, t306); });
  RT_AT(3, 188);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S57)), "statement"))) {
    RT_AT(3, 189);
    wv v253_w = rt_bind(rt_chk(s195_next(up), "statement"), 0);
    RT_AT(3, 190);
    if (rt_cond(rt_not(({ wv t307 = rt_def(w4_onfailOk, "onfailOk"); wv t308 = rt_get(&F_k, v252_s); rt_chk(w1_contains(t307, t308), "statement"); })))) {
      RT_AT(3, 191);
      (void)rt_chk(w4_failAt(v253_w, rt_s(&S208)), "statement");
    }
    RT_AT(3, 193);
    wv v254_param = RT_NULL;
    RT_AT(3, 194);
    if (rt_cond(rt_ne(rt_chk(s196_accept(up, rt_s(&S98)), "statement"), RT_NULL))) {
      RT_AT(3, 195);
      v254_param = rt_bind(rt_chk(s197_expect(up, rt_s(&S138), rt_s(&S209), RT_NULL), "statement"), 1);
      RT_AT(3, 196);
      (void)rt_chk(s197_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "statement");
    }
    RT_AT(3, 198);
    wv v255_noParam = rt_eq(v254_param, RT_NULL);
    RT_AT(3, 199);
    ({ wv t313 = ({ wv t309 = rt_get(&F_pos, v253_w); wv t310 = (rt_cond(v255_noParam) ? RT_NULL : rt_get(&F_v, v254_param)); wv t311 = (rt_cond(v255_noParam) ? RT_NULL : rt_get(&F_pos, v254_param)); wv t312 = rt_chk(s213_block(up), "statement"); rt_rec(0, 4, K9, (wv[]){ t309, t310, t311, t312 }); }); wv t314 = v252_s; rt_set_mem(t313, &F_handler, t314); });
  }
  RT_AT(3, 206);
  ({ wv t315 = rt_get(&F_pos, rt_chk(s191_peek(up), "statement")); wv t316 = v252_s; rt_set_mem(t315, &F_end, t316); });
  RT_RET((RT_AT(3, 207), v252_s));
}

static wv s202_statementBody(struct fr49 *up, wv v256_tk) {
  rt_enter("statementBody");
  v256_tk = rt_bind(v256_tk, 0);
  RT_AT(3, 212);
  wv v257_kind = rt_bind(rt_get(&F_t, v256_tk), 0);
  RT_AT(3, 213);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S33)))) {
    RT_AT(3, 213);
    RT_RET(rt_chk(s210_fnStatement(up), "statementBody"));
  }
  RT_AT(3, 214);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S47)))) {
    RT_AT(3, 214);
    RT_RET(rt_chk(s211_enumStatement(up), "statementBody"));
  }
  RT_AT(3, 215);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S48)))) {
    RT_AT(3, 215);
    RT_RET(rt_chk(s207_importStatement(up), "statementBody"));
  }
  RT_AT(3, 216);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S49)))) {
    RT_AT(3, 216);
    RT_RET(rt_chk(s208_exportStatement(up), "statementBody"));
  }
  RT_AT(3, 217);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v257_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v257_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v257_kind, rt_s(&S37)), "||")))) {
    RT_AT(3, 217);
    RT_RET(rt_chk(s203_declaration(up), "statementBody"));
  }
  RT_AT(3, 218);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S38)))) {
    RT_AT(3, 218);
    RT_RET(rt_chk(s204_setStatement(up), "statementBody"));
  }
  RT_AT(3, 219);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S34)))) {
    RT_AT(3, 220);
    (void)rt_chk(s195_next(up), "statementBody");
    RT_AT(3, 221);
    wv v259_e = rt_bind((rt_cond(({ wv t317 = rt_def(w4_exprStart, "exprStart"); wv t318 = rt_get(&F_t, rt_chk(s191_peek(up), "statementBody")); rt_chk(w1_contains(t317, t318), "statementBody"); })) ? rt_chk(s215_expr(up), "statementBody") : RT_NULL), 0);
    RT_AT(3, 222);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S34), v259_e), "statementBody"));
  }
  RT_AT(3, 224);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S58)))) {
    RT_AT(3, 225);
    (void)rt_chk(s195_next(up), "statementBody");
    RT_AT(3, 226);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S58), rt_chk(s215_expr(up), "statementBody")), "statementBody"));
  }
  RT_AT(3, 228);
  if (rt_cond(rt_b(rt_bool(rt_eq(v257_kind, rt_s(&S43)), "||") || rt_bool(rt_eq(v257_kind, rt_s(&S44)), "||")))) {
    RT_AT(3, 229);
    (void)rt_chk(s195_next(up), "statementBody");
    RT_AT(3, 230);
    RT_RET(rt_rec(0, 4, K8, (wv[]){ (rt_cond(rt_eq(v257_kind, rt_s(&S43))) ? rt_s(&S43) : rt_s(&S44)), RT_NULL, RT_NULL, RT_NULL }));
  }
  RT_AT(3, 232);
  if (rt_cond(rt_b(rt_bool(rt_eq(v257_kind, rt_s(&S41)), "||") || rt_bool(rt_eq(v257_kind, rt_s(&S210)), "||")))) {
    RT_AT(3, 233);
    (void)rt_chk(s195_next(up), "statementBody");
    RT_AT(3, 234);
    RT_RET(({ wv t319 = rt_eq(v257_kind, rt_s(&S210)); wv t320 = rt_chk(s212_cond(up, v257_kind), "statementBody"); wv t321 = rt_chk(s213_block(up), "statementBody"); rt_rec(0, 7, K10, (wv[]){ rt_s(&S41), t319, t320, t321, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  RT_AT(3, 239);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S42)))) {
    RT_AT(3, 239);
    RT_RET(rt_chk(s205_forStatement(up), "statementBody"));
  }
  RT_AT(3, 240);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S56)))) {
    RT_AT(3, 240);
    RT_RET(rt_chk(s206_failStatement(up), "statementBody"));
  }
  RT_AT(3, 241);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S55)))) {
    RT_AT(3, 242);
    (void)rt_chk(s195_next(up), "statementBody");
    RT_AT(3, 243);
    wv v260_e = rt_bind((rt_cond(({ wv t322 = rt_def(w4_exprStart, "exprStart"); wv t323 = rt_get(&F_t, rt_chk(s191_peek(up), "statementBody")); rt_chk(w1_contains(t322, t323), "statementBody"); })) ? rt_chk(s215_expr(up), "statementBody") : RT_NULL), 0);
    RT_AT(3, 244);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S55), v260_e), "statementBody"));
  }
  RT_AT(3, 246);
  if (rt_cond(rt_b(rt_bool(rt_eq(v257_kind, rt_s(&S138)), "&&") && rt_bool(rt_eq(rt_get(&F_v, v256_tk), rt_s(&S206)), "&&")))) {
    RT_AT(3, 247);
    wv v261_after = rt_bind(rt_get(&F_t, ({ wv t324 = rt_add(rt_def(up->v187_p, "p"), rt_i(1)); wv t325 = up->v185_toks; rt_idx(t324, t325); })), 0);
    RT_AT(3, 248);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v261_after, rt_s(&S14)), "||") || rt_bool(rt_eq(v261_after, rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(v261_after, rt_s(&S111)), "||")))) {
      RT_AT(3, 248);
      RT_RET(rt_chk(s209_endStatement(up), "statementBody"));
    }
  }
  RT_AT(3, 250);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S57)))) {
    RT_AT(3, 250);
    (void)rt_chk(w4_failAt(v256_tk, rt_s(&S208)), "statementBody");
  }
  RT_AT(3, 251);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S40)))) {
    RT_AT(3, 251);
    (void)rt_chk(w4_failAt(v256_tk, rt_s(&S211)), "statementBody");
  }
  RT_AT(3, 252);
  if (rt_cond(rt_eq(v257_kind, rt_s(&S46)))) {
    RT_AT(3, 252);
    (void)rt_chk(w4_failAt(v256_tk, rt_s(&S212)), "statementBody");
  }
  RT_AT(3, 253);
  wv v258_e = rt_bind(rt_chk(s215_expr(up), "statementBody"), 0);
  RT_AT(3, 254);
  if (rt_cond(rt_b(rt_bool(rt_chk(s192_at(up, rt_s(&S112)), "statementBody"), "||") || rt_bool(rt_b(rt_bool(rt_chk(s193_atWord(up, rt_s(&S213)), "statementBody"), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v258_e), rt_s(&S214)), "||") || rt_bool(rt_eq(rt_get(&F_k, v258_e), rt_s(&S191)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v258_e), rt_s(&S192)), "||")), "&&")), "||")))) {
    RT_AT(3, 255);
    (void)rt_chk(w4_failAtHint(v256_tk, rt_s(&S215), rt_s(&S216)), "statementBody");
  }
  RT_AT(3, 257);
  if (rt_cond(rt_not(rt_chk(w4_hasEffect(v258_e), "statementBody")))) {
    RT_AT(3, 258);
    wv v262_hint = rt_bind((rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0))) ? rt_s(&S217) : RT_NULL), 0);
    RT_AT(3, 259);
    (void)({ wv t328 = w2_warnings; wv t329 = ({ wv t326 = rt_get(&F_pos, v256_tk); wv t327 = v262_hint; rt_chk(w2_warn(t326, rt_s(&S218), t327), "statementBody"); }); std_push(t328, t329); });
  }
  RT_RET((RT_AT(3, 261), rt_chk(w4_simpleStatement(rt_s(&S178), v258_e), "statementBody")));
}

static wv s203_declaration(struct fr49 *up) {
  rt_enter("declaration");
  RT_AT(3, 266);
  wv v263_kw = rt_bind(rt_chk(s195_next(up), "declaration"), 0);
  RT_AT(3, 267);
  wv v264_name = rt_bind(({ wv t330 = rt_cat(3, (wv[]){ rt_s(&S219), rt_text(rt_get(&F_t, v263_kw)), rt_s(&S114) }); wv t331 = rt_cat(3, (wv[]){ rt_s(&S14), rt_text(rt_get(&F_t, v263_kw)), rt_s(&S220) }); rt_chk(s197_expect(up, rt_s(&S138), t330, t331), "declaration"); }), 0);
  RT_AT(3, 268);
  (void)rt_chk(s198_expectWord(up, rt_s(&S221), ({ wv t332 = rt_text(rt_get(&F_t, v263_kw)); wv t333 = rt_text(rt_get(&F_v, v264_name)); rt_cat(5, (wv[]){ rt_s(&S14), t332, rt_s(&S0), t333, rt_s(&S222) }); })), "declaration");
  RT_AT(3, 269);
  wv v265_e = rt_bind(rt_chk(s215_expr(up), "declaration"), 0);
  RT_RET((RT_AT(3, 270), ({ wv t334 = v265_e; wv t335 = rt_eq(rt_get(&F_t, v263_kw), rt_s(&S36)); wv t336 = rt_eq(rt_get(&F_t, v263_kw), rt_s(&S37)); wv t337 = rt_get(&F_v, v264_name); wv t338 = rt_get(&F_pos, v264_name); rt_rec(0, 9, K11, (wv[]){ rt_s(&S179), t334, t335, t336, t337, t338, RT_NULL, RT_NULL, RT_NULL }); })));
}

static wv s204_setStatement(struct fr49 *up) {
  rt_enter("setStatement");
  RT_AT(3, 278);
  (void)rt_chk(s195_next(up), "setStatement");
  RT_AT(3, 279);
  wv v266_target = rt_bind(rt_chk(s214_lvalue(up), "setStatement"), 0);
  RT_AT(3, 280);
  if (rt_cond(rt_chk(s193_atWord(up, rt_s(&S213)), "setStatement"))) {
    RT_AT(3, 281);
    (void)rt_chk(s195_next(up), "setStatement");
    RT_AT(3, 282);
    RT_RET(({ wv t339 = rt_chk(s215_expr(up), "setStatement"); wv t340 = v266_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t339, RT_NULL, t340, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  RT_AT(3, 284);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S112)), "setStatement"))) {
    RT_AT(3, 285);
    wv v268_op = rt_bind(rt_chk(s195_next(up), "setStatement"), 0);
    RT_AT(3, 286);
    RT_RET(({ wv t341 = rt_chk(s215_expr(up), "setStatement"); wv t342 = rt_get(&F_v, v268_op); wv t343 = v266_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t341, t342, t343, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  RT_AT(3, 288);
  wv v267_found = rt_bind(rt_chk(s191_peek(up), "setStatement"), 0);
  RT_AT(3, 289);
  (void)({ wv t344 = v267_found; wv t345 = rt_cat(2, (wv[]){ rt_s(&S223), rt_text(rt_chk(w4_describe(v267_found), "setStatement")) }); rt_chk(w4_failAtHint(t344, t345, rt_s(&S216)), "setStatement"); });
  RT_RET(RT_NULL);
}

static wv s205_forStatement(struct fr49 *up) {
  rt_enter("forStatement");
  RT_AT(3, 296);
  (void)rt_chk(s195_next(up), "forStatement");
  RT_AT(3, 297);
  wv v269_form = rt_s(&S224);
  RT_AT(3, 298);
  (void)rt_chk(s197_expect(up, rt_s(&S98), rt_s(&S225), v269_form), "forStatement");
  RT_AT(3, 299);
  wv v270_names = rt_arr(0, NULL, 1);
  RT_AT(3, 300);
  while (rt_cond((RT_AT(3, 300), rt_b(1)))) {
    RT_AT(3, 301);
    wv v277_tk = rt_bind(rt_chk(s191_peek(up), "forStatement"), 0);
    RT_AT(3, 302);
    if (rt_cond(rt_ne(rt_get(&F_t, v277_tk), rt_s(&S138)))) {
      RT_AT(3, 303);
      wv v278_message = rt_bind((rt_cond(({ wv t346 = w3_keywords; wv t347 = rt_get(&F_t, v277_tk); rt_chk(w1_contains(t346, t347), "forStatement"); })) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_t, v277_tk)), rt_s(&S226) }) : rt_cat(2, (wv[]){ rt_s(&S227), rt_text(rt_chk(w4_describe(v277_tk), "forStatement")) })), 0);
      RT_AT(3, 305);
      (void)rt_chk(w4_failAtHint(v277_tk, v278_message, v269_form), "forStatement");
    }
    RT_AT(3, 307);
    (void)rt_chk(s195_next(up), "forStatement");
    RT_AT(3, 308);
    (void)({ wv t350 = v270_names; wv t351 = ({ wv t348 = rt_get(&F_v, v277_tk); wv t349 = rt_get(&F_pos, v277_tk); rt_rec(0, 2, K13, (wv[]){ t348, t349 }); }); std_push(t350, t351); });
    RT_AT(3, 309);
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v270_names), rt_i(2)), "||") || rt_bool(rt_eq(rt_chk(s196_accept(up, rt_s(&S97)), "forStatement"), RT_NULL), "||")))) {
      break;
    }
  }
  RT_AT(3, 311);
  (void)rt_chk(s198_expectWord(up, rt_s(&S228), v269_form), "forStatement");
  RT_AT(3, 312);
  wv v271_first = rt_bind(rt_chk(s215_expr(up), "forStatement"), 0);
  RT_AT(3, 313);
  wv v272_coll = rt_bind(v271_first, 1);
  RT_AT(3, 314);
  wv v273_from = RT_NULL;
  RT_AT(3, 315);
  wv v274_to = RT_NULL;
  RT_AT(3, 316);
  wv v275_step = RT_NULL;
  RT_AT(3, 317);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S97)), "forStatement"))) {
    RT_AT(3, 318);
    (void)rt_chk(s195_next(up), "forStatement");
    RT_AT(3, 319);
    v272_coll = RT_NULL;
    RT_AT(3, 320);
    v273_from = rt_bind(v271_first, 1);
    RT_AT(3, 321);
    v274_to = rt_bind(rt_chk(s215_expr(up), "forStatement"), 1);
    RT_AT(3, 322);
    if (rt_cond(rt_ne(rt_chk(s196_accept(up, rt_s(&S97)), "forStatement"), RT_NULL))) {
      RT_AT(3, 323);
      wv v279_st = rt_bind(rt_chk(s191_peek(up), "forStatement"), 0);
      RT_AT(3, 324);
      v275_step = rt_bind(rt_chk(s215_expr(up), "forStatement"), 1);
      RT_AT(3, 325);
      wv v280_negative = rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v275_step), rt_s(&S193)), "&&") && rt_bool(rt_eq(rt_get(&F_op, v275_step), rt_s(&S86)), "&&")), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v275_step)), rt_s(&S139)), "||") || rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v275_step)), rt_s(&S149)), "||")), "&&"));
      RT_AT(3, 326);
      if (rt_cond(rt_b(rt_bool(v280_negative, "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v275_step), rt_s(&S139)), "&&") && rt_bool(rt_lt(rt_get(&F_v, v275_step), rt_i(1)), "&&")), "||")))) {
        RT_AT(3, 327);
        (void)rt_chk(w4_failAtHint(v279_st, rt_s(&S229), rt_s(&S230)), "forStatement");
      }
    }
    RT_AT(3, 331);
    if (rt_cond(rt_eq(std_length(v270_names), rt_i(2)))) {
      RT_AT(3, 332);
      (void)({ wv t352 = rt_get(&F_pos, rt_idx(rt_i(1), v270_names)); wv t353 = rt_cat(3, (wv[]){ rt_s(&S232), rt_text(rt_get(&F_name, rt_idx(rt_i(0), v270_names))), rt_s(&S233) }); rt_chk(w2_failHint(t352, rt_s(&S231), t353), "forStatement"); });
    }
  }
  RT_AT(3, 336);
  wv v276_rev = rt_bind(rt_chk(s193_atWord(up, rt_s(&S234)), "forStatement"), 0);
  RT_AT(3, 337);
  if (rt_cond(v276_rev)) {
    RT_AT(3, 337);
    (void)rt_chk(s195_next(up), "forStatement");
  }
  RT_AT(3, 338);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S16)), "forStatement")))) {
    RT_AT(3, 339);
    wv v281_found = rt_bind(rt_chk(s191_peek(up), "forStatement"), 0);
    RT_AT(3, 340);
    wv v282_backwards = rt_bind((rt_cond(rt_ne(v272_coll, RT_NULL)) ? rt_s(&S235) : rt_s(&S236)), 0);
    RT_AT(3, 341);
    wv v283_hint = rt_bind((rt_cond(rt_eq(rt_get(&F_t, v281_found), rt_s(&S138))) ? rt_cat(2, (wv[]){ rt_s(&S237), rt_text(v282_backwards) }) : v269_form), 0);
    RT_AT(3, 342);
    (void)({ wv t354 = v281_found; wv t355 = rt_cat(2, (wv[]){ rt_s(&S238), rt_text(rt_chk(w4_describe(v281_found), "forStatement")) }); wv t356 = v283_hint; rt_chk(w4_failAtHint(t354, t355, t356), "forStatement"); });
  }
  RT_AT(3, 344);
  (void)rt_chk(s195_next(up), "forStatement");
  RT_RET((RT_AT(3, 345), ({ wv t357 = v270_names; wv t358 = v272_coll; wv t359 = v273_from; wv t360 = v274_to; wv t361 = v275_step; wv t362 = v276_rev; wv t363 = rt_chk(s213_block(up), "forStatement"); rt_rec(0, 11, K14, (wv[]){ rt_s(&S42), t357, t358, t359, t360, t361, t362, t363, RT_NULL, RT_NULL, RT_NULL }); })));
}

static wv s206_failStatement(struct fr49 *up) {
  rt_enter("failStatement");
  RT_AT(3, 353);
  wv v284_kw = rt_bind(rt_chk(s195_next(up), "failStatement"), 0);
  RT_AT(3, 354);
  if (rt_cond(rt_not(({ wv t364 = rt_def(w4_exprStart, "exprStart"); wv t365 = rt_get(&F_t, rt_chk(s191_peek(up), "failStatement")); rt_chk(w1_contains(t364, t365), "failStatement"); })))) {
    RT_AT(3, 355);
    RT_RET(rt_rec(0, 5, K15, (wv[]){ rt_s(&S56), rt_i(1), RT_NULL, RT_NULL, RT_NULL }));
  }
  RT_AT(3, 357);
  wv v285_first = rt_bind(rt_chk(s191_peek(up), "failStatement"), 0);
  RT_AT(3, 358);
  wv v286_e = rt_bind(rt_chk(s215_expr(up), "failStatement"), 0);
  RT_AT(3, 359);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v286_e), rt_s(&S149)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v286_e), rt_s(&S139)), "&&") && rt_bool(rt_lt(rt_get(&F_v, v286_e), rt_i(1)), "&&")), "||")))) {
    RT_AT(3, 360);
    (void)rt_chk(w4_failAt(v285_first, rt_s(&S239)), "failStatement");
  }
  RT_AT(3, 362);
  if (rt_cond(rt_eq(rt_get(&F_k, v286_e), rt_s(&S139)))) {
    RT_AT(3, 363);
    RT_RET(rt_rec(0, 5, K15, (wv[]){ rt_s(&S56), rt_get(&F_v, v286_e), RT_NULL, RT_NULL, RT_NULL }));
  }
  RT_RET((RT_AT(3, 365), rt_chk(w4_simpleStatement(rt_s(&S178), ({ wv t366 = v286_e; wv t367 = rt_get(&F_pos, v284_kw); rt_rec(0, 4, K16, (wv[]){ rt_s(&S185), t366, t367, rt_b(0) }); })), "failStatement")));
}

static wv s207_importStatement(struct fr49 *up) {
  rt_enter("importStatement");
  RT_AT(3, 370);
  wv v287_kw = rt_bind(rt_chk(s195_next(up), "importStatement"), 0);
  RT_AT(3, 371);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    RT_AT(3, 371);
    (void)rt_chk(w4_failAt(v287_kw, rt_s(&S240)), "importStatement");
  }
  RT_AT(3, 372);
  wv v288_name = rt_bind(rt_chk(s197_expect(up, rt_s(&S138), rt_s(&S241), rt_s(&S242)), "importStatement"), 0);
  RT_AT(3, 373);
  (void)rt_chk(s198_expectWord(up, rt_s(&S158), ({ wv t368 = rt_text(rt_get(&F_v, v288_name)); wv t369 = rt_text(rt_get(&F_v, v288_name)); rt_cat(5, (wv[]){ rt_s(&S243), t368, rt_s(&S244), t369, rt_s(&S245) }); })), "importStatement");
  RT_AT(3, 374);
  wv v289_e = rt_bind(rt_chk(s191_peek(up), "importStatement"), 0);
  RT_AT(3, 375);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_t, v289_e), rt_s(&S173)), "||") || rt_bool(rt_ne(std_length(rt_get(&F_parts, v289_e)), rt_i(1)), "||")))) {
    RT_AT(3, 376);
    (void)rt_chk(w4_failAtHint(v289_e, rt_s(&S246), rt_s(&S242)), "importStatement");
  }
  RT_AT(3, 378);
  (void)rt_chk(s195_next(up), "importStatement");
  RT_AT(3, 379);
  wv v290_path = rt_bind(rt_idx(rt_i(0), rt_get(&F_parts, v289_e)), 0);
  RT_AT(3, 380);
  if (rt_cond(rt_chk(w1_contains(std_split(v290_path, rt_s(&S13)), rt_s(&S19)), "importStatement"))) {
    RT_AT(3, 380);
    (void)rt_chk(w4_failAt(v289_e, rt_s(&S247)), "importStatement");
  }
  RT_AT(3, 381);
  if (rt_cond(rt_not(rt_b(rt_bool(rt_ge(std_length(v290_path), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t370 = v290_path; wv t371 = std_length(v290_path); std_slice(t370, rt_i(-4), t371); }), rt_s(&S248)), "&&"))))) {
    RT_AT(3, 382);
    (void)rt_chk(w4_failAt(v289_e, rt_s(&S249)), "importStatement");
  }
  RT_AT(3, 385);
  if (rt_cond(rt_eq(std_at(v290_path, rt_i(0)), rt_s(&S106)))) {
    RT_AT(3, 386);
    if (rt_cond(rt_b(rt_bool(rt_lt(std_length(v290_path), rt_i(5)), "||") || rt_bool(rt_ne(std_slice(v290_path, rt_i(0), rt_i(5)), rt_s(&S250)), "||")))) {
      RT_AT(3, 387);
      (void)rt_chk(w4_failAtHint(v289_e, rt_s(&S251), rt_s(&S252)), "importStatement");
    }
    RT_AT(3, 390);
    if (rt_cond(rt_chk(w1_contains(std_split(v290_path, rt_s(&S88)), rt_s(&S253)), "importStatement"))) {
      RT_AT(3, 390);
      (void)rt_chk(w4_failAt(v289_e, rt_s(&S254)), "importStatement");
    }
  }
  RT_RET((RT_AT(3, 392), ({ wv t372 = v290_path; wv t373 = rt_get(&F_pos, v289_e); wv t374 = rt_get(&F_v, v288_name); wv t375 = rt_get(&F_pos, v288_name); rt_rec(0, 8, K17, (wv[]){ rt_s(&S48), t372, t373, t374, t375, RT_NULL, RT_NULL, RT_NULL }); })));
}

static wv s208_exportStatement(struct fr49 *up) {
  rt_enter("exportStatement");
  RT_AT(3, 397);
  wv v291_kw = rt_bind(rt_chk(s195_next(up), "exportStatement"), 0);
  RT_AT(3, 398);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    RT_AT(3, 398);
    (void)rt_chk(w4_failAt(v291_kw, rt_s(&S255)), "exportStatement");
  }
  RT_AT(3, 399);
  wv v292_tk = rt_bind(rt_chk(s191_peek(up), "exportStatement"), 0);
  RT_AT(3, 400);
  if (rt_cond(rt_not(({ wv t376 = rt_def(w4_exportable, "exportable"); wv t377 = rt_get(&F_t, v292_tk); rt_chk(w1_contains(t376, t377), "exportStatement"); })))) {
    RT_AT(3, 401);
    (void)({ wv t378 = v292_tk; wv t379 = rt_cat(2, (wv[]){ rt_s(&S256), rt_text(rt_chk(w4_describe(v292_tk), "exportStatement")) }); rt_chk(w4_failAt(t378, t379), "exportStatement"); });
  }
  RT_AT(3, 403);
  wv v293_s = rt_bind(rt_chk(s202_statementBody(up, v292_tk), "exportStatement"), 0);
  RT_AT(3, 404);
  if (rt_cond(rt_eq(rt_def(up->v190_firstExport, "firstExport"), RT_NULL))) {
    RT_AT(3, 404);
    wv t380 = rt_bind(v291_kw, 1);
    (void)rt_def(up->v190_firstExport, "firstExport");
    up->v190_firstExport = t380;
  }
  RT_AT(3, 405);
  (void)({ wv t383 = rt_def(up->v189_exports, "exports"); wv t384 = ({ wv t381 = rt_get(&F_name, v293_s); wv t382 = rt_get(&F_namePos, v293_s); rt_rec(0, 2, K13, (wv[]){ t381, t382 }); }); std_push(t383, t384); });
  RT_RET((RT_AT(3, 406), v293_s));
}

static wv s209_endStatement(struct fr49 *up) {
  rt_enter("endStatement");
  RT_AT(3, 411);
  wv v294_kw = rt_bind(rt_chk(s195_next(up), "endStatement"), 0);
  RT_AT(3, 412);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    RT_AT(3, 412);
    (void)rt_chk(w4_failAt(v294_kw, rt_s(&S257)), "endStatement");
  }
  RT_AT(3, 413);
  if (rt_cond(rt_eq(std_length(rt_def(up->v189_exports, "exports")), rt_i(0)))) {
    RT_AT(3, 414);
    (void)rt_chk(w4_failAtHint(v294_kw, rt_s(&S258), rt_s(&S259)), "endStatement");
  }
  RT_RET((RT_AT(3, 416), rt_rec(0, 5, K18, (wv[]){ rt_s(&S206), rt_def(up->v189_exports, "exports"), RT_NULL, RT_NULL, RT_NULL })));
}

static wv s210_fnStatement(struct fr49 *up) {
  rt_enter("fnStatement");
  RT_AT(3, 421);
  (void)rt_chk(s195_next(up), "fnStatement");
  RT_AT(3, 422);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S98)), "fnStatement"))) {
    RT_AT(3, 423);
    (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "fnStatement"), rt_s(&S260), rt_s(&S261)), "fnStatement");
  }
  RT_AT(3, 425);
  wv v295_name = rt_bind(rt_chk(s197_expect(up, rt_s(&S138), rt_s(&S262), rt_s(&S261)), "fnStatement"), 0);
  RT_AT(3, 426);
  (void)rt_chk(s197_expect(up, rt_s(&S98), rt_s(&S263), rt_cat(3, (wv[]){ rt_s(&S264), rt_text(rt_get(&F_v, v295_name)), rt_s(&S265) })), "fnStatement");
  RT_AT(3, 427);
  wv v296_params = rt_arr(0, NULL, 1);
  RT_AT(3, 428);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S16)), "fnStatement")))) {
    RT_AT(3, 429);
    while (rt_cond((RT_AT(3, 429), rt_b(1)))) {
      RT_AT(3, 430);
      wv v297_isVar = rt_ne(rt_chk(s196_accept(up, rt_s(&S36)), "fnStatement"), RT_NULL);
      RT_AT(3, 431);
      wv v298_isRef = rt_b(rt_bool(rt_not(v297_isVar), "&&") && rt_bool(rt_ne(rt_chk(s196_accept(up, rt_s(&S37)), "fnStatement"), RT_NULL), "&&"));
      RT_AT(3, 432);
      wv v299_pname = rt_bind(rt_chk(s197_expect(up, rt_s(&S138), rt_s(&S266), rt_s(&S267)), "fnStatement"), 0);
      RT_AT(3, 433);
      (void)({ wv t389 = v296_params; wv t390 = ({ wv t385 = rt_get(&F_v, v299_pname); wv t386 = v297_isVar; wv t387 = v298_isRef; wv t388 = rt_get(&F_pos, v299_pname); rt_rec(0, 4, K19, (wv[]){ t385, t386, t387, t388 }); }); std_push(t389, t390); });
      RT_AT(3, 434);
      if (rt_cond(rt_eq(rt_chk(s196_accept(up, rt_s(&S97)), "fnStatement"), RT_NULL))) {
        break;
      }
    }
  }
  RT_AT(3, 437);
  (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S268), RT_NULL), "fnStatement");
  RT_RET((RT_AT(3, 438), ({ wv t391 = rt_get(&F_v, v295_name); wv t392 = rt_get(&F_pos, v295_name); wv t393 = v296_params; wv t394 = rt_chk(s213_block(up), "fnStatement"); rt_rec(0, 8, K20, (wv[]){ rt_s(&S33), t391, t392, t393, t394, RT_NULL, RT_NULL, RT_NULL }); })));
}

static wv s211_enumStatement(struct fr49 *up) {
  rt_enter("enumStatement");
  RT_AT(3, 446);
  wv v300_kw = rt_bind(rt_chk(s195_next(up), "enumStatement"), 0);
  RT_AT(3, 447);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    RT_AT(3, 447);
    (void)rt_chk(w4_failAt(v300_kw, rt_s(&S269)), "enumStatement");
  }
  RT_AT(3, 448);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S98)), "enumStatement"))) {
    RT_AT(3, 449);
    (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "enumStatement"), rt_s(&S270), rt_s(&S271)), "enumStatement");
  }
  RT_AT(3, 451);
  wv v301_name = rt_bind(rt_chk(s197_expect(up, rt_s(&S138), rt_s(&S272), rt_s(&S271)), "enumStatement"), 0);
  RT_AT(3, 452);
  (void)rt_chk(s197_expect(up, rt_s(&S98), rt_s(&S273), rt_cat(3, (wv[]){ rt_s(&S274), rt_text(rt_get(&F_v, v301_name)), rt_s(&S275) })), "enumStatement");
  RT_AT(3, 453);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S16)), "enumStatement"))) {
    RT_AT(3, 454);
    (void)({ wv t395 = rt_chk(s191_peek(up), "enumStatement"); wv t396 = rt_cat(3, (wv[]){ rt_s(&S274), rt_text(rt_get(&F_v, v301_name)), rt_s(&S275) }); rt_chk(w4_failAtHint(t395, rt_s(&S276), t396), "enumStatement"); });
  }
  RT_AT(3, 456);
  wv v302_members = rt_arr(0, NULL, 1);
  RT_AT(3, 457);
  while (rt_cond((RT_AT(3, 457), rt_b(1)))) {
    RT_AT(3, 458);
    wv v303_m = rt_bind(rt_chk(s191_peek(up), "enumStatement"), 0);
    RT_AT(3, 459);
    if (rt_cond(rt_ne(rt_get(&F_t, v303_m), rt_s(&S138)))) {
      RT_AT(3, 460);
      wv v305_message = rt_bind((rt_cond(({ wv t397 = w3_keywords; wv t398 = rt_get(&F_t, v303_m); rt_chk(w1_contains(t397, t398), "enumStatement"); })) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_t, v303_m)), rt_s(&S277) }) : rt_cat(2, (wv[]){ rt_s(&S278), rt_text(rt_chk(w4_describe(v303_m), "enumStatement")) })), 0);
      RT_AT(3, 462);
      (void)rt_chk(w4_failAt(v303_m, v305_message), "enumStatement");
    }
    RT_AT(3, 464);
    (void)rt_chk(s195_next(up), "enumStatement");
    RT_AT(3, 465);
    wv v304_k = rt_i(0);
    RT_AT(3, 466);
    while (rt_cond((RT_AT(3, 466), ({ wv t399 = v304_k; wv t400 = std_length(v302_members); rt_lt(t399, t400); })))) {
      RT_AT(3, 467);
      if (rt_cond(({ wv t401 = rt_get(&F_name, rt_idx(v304_k, v302_members)); wv t402 = rt_get(&F_v, v303_m); rt_eq(t401, t402); }))) {
        RT_AT(3, 467);
        (void)({ wv t403 = v303_m; wv t404 = rt_cat(3, (wv[]){ rt_s(&S279), rt_text(rt_get(&F_v, v303_m)), rt_s(&S280) }); rt_chk(w4_failAt(t403, t404), "enumStatement"); });
      }
      RT_AT(3, 468);
      v304_k = rt_add(v304_k, rt_i(1));
    }
    RT_AT(3, 470);
    (void)({ wv t407 = v302_members; wv t408 = ({ wv t405 = rt_get(&F_v, v303_m); wv t406 = rt_get(&F_pos, v303_m); rt_rec(0, 2, K13, (wv[]){ t405, t406 }); }); std_push(t407, t408); });
    RT_AT(3, 471);
    if (rt_cond(rt_eq(rt_chk(s196_accept(up, rt_s(&S97)), "enumStatement"), RT_NULL))) {
      break;
    }
  }
  RT_AT(3, 473);
  (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S281), RT_NULL), "enumStatement");
  RT_RET((RT_AT(3, 474), ({ wv t409 = rt_get(&F_v, v301_name); wv t410 = rt_get(&F_pos, v301_name); wv t411 = v302_members; rt_rec(0, 7, K21, (wv[]){ rt_s(&S47), t409, t410, t411, RT_NULL, RT_NULL, RT_NULL }); })));
}

static wv s212_cond(struct fr49 *up, wv v306_kw) {
  rt_enter("cond");
  v306_kw = rt_bind(v306_kw, 0);
  RT_AT(3, 481);
  (void)({ wv t412 = rt_cat(3, (wv[]){ rt_s(&S282), rt_text(v306_kw), rt_s(&S114) }); wv t413 = rt_cat(3, (wv[]){ rt_s(&S283), rt_text(v306_kw), rt_s(&S284) }); rt_chk(s197_expect(up, rt_s(&S98), t412, t413), "cond"); });
  RT_AT(3, 482);
  wv v307_e = rt_bind(rt_chk(s215_expr(up), "cond"), 0);
  RT_AT(3, 483);
  (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S285), RT_NULL), "cond");
  RT_RET((RT_AT(3, 484), v307_e));
}

static wv s213_block(struct fr49 *up) {
  rt_enter("block");
  RT_AT(3, 488);
  (void)rt_chk(s197_expect(up, rt_s(&S101), rt_s(&S286), RT_NULL), "block");
  RT_AT(3, 489);
  up->v188_depth = rt_add(rt_def(up->v188_depth, "depth"), rt_i(1));
  RT_AT(3, 490);
  wv v308_stmts = rt_arr(0, NULL, 1);
  RT_AT(3, 491);
  while (rt_cond((RT_AT(3, 491), rt_chk(s192_at(up, rt_s(&S14)), "block")))) {
    RT_AT(3, 491);
    (void)({ wv t414 = v308_stmts; wv t415 = rt_chk(s201_statement(up), "block"); std_push(t414, t415); });
  }
  RT_AT(3, 492);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S15)), "block"))) {
    RT_AT(3, 493);
    wv v311_close = rt_bind(rt_chk(s195_next(up), "block"), 0);
    RT_AT(3, 494);
    up->v188_depth = rt_sub(rt_def(up->v188_depth, "depth"), rt_i(1));
    RT_AT(3, 495);
    RT_RET(({ wv t416 = v308_stmts; wv t417 = rt_get(&F_pos, v311_close); rt_rec(0, 3, K22, (wv[]){ t416, RT_NULL, t417 }); }));
  }
  RT_AT(3, 497);
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S111)), "block"))) {
    RT_AT(3, 497);
    (void)rt_chk(w4_failAt(rt_chk(s191_peek(up), "block"), rt_s(&S287)), "block");
  }
  RT_AT(3, 498);
  wv v309_value = rt_bind(rt_chk(s215_expr(up), "block"), 0);
  RT_AT(3, 499);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S15)), "block")))) {
    RT_AT(3, 500);
    wv v312_hint = rt_bind((rt_cond(rt_chk(s192_at(up, rt_s(&S28)), "block")) ? rt_s(&S288) : rt_s(&S289)), 0);
    RT_AT(3, 502);
    wv v313_found = rt_bind(rt_chk(s191_peek(up), "block"), 0);
    RT_AT(3, 503);
    (void)({ wv t418 = v313_found; wv t419 = rt_cat(2, (wv[]){ rt_s(&S290), rt_text(rt_chk(w4_describe(v313_found), "block")) }); wv t420 = v312_hint; rt_chk(w4_failAtHint(t418, t419, t420), "block"); });
  }
  RT_AT(3, 505);
  wv v310_close = rt_bind(rt_chk(s195_next(up), "block"), 0);
  RT_AT(3, 506);
  up->v188_depth = rt_sub(rt_def(up->v188_depth, "depth"), rt_i(1));
  RT_RET((RT_AT(3, 507), ({ wv t421 = v308_stmts; wv t422 = v309_value; wv t423 = rt_get(&F_pos, v310_close); rt_rec(0, 3, K22, (wv[]){ t421, t422, t423 }); })));
}

static wv s214_lvalue(struct fr49 *up) {
  rt_enter("lvalue");
  RT_AT(3, 511);
  wv v314_tk = rt_bind(rt_chk(s191_peek(up), "lvalue"), 0);
  RT_AT(3, 512);
  wv v315_e = rt_bind(rt_chk(s222_postfix(up), "lvalue"), 0);
  RT_AT(3, 513);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_k, v315_e), rt_s(&S214)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v315_e), rt_s(&S191)), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_k, v315_e), rt_s(&S192)), "&&")))) {
    RT_AT(3, 514);
    (void)rt_chk(w4_failAt(v314_tk, rt_s(&S291)), "lvalue");
  }
  RT_RET((RT_AT(3, 516), v315_e));
}

static wv s215_expr(struct fr49 *up) {
  rt_enter("expr");
  RT_RET((RT_AT(3, 521), rt_chk(s216_ternary(up), "expr")));
}

static wv s216_ternary(struct fr49 *up) {
  rt_enter("ternary");
  RT_RET((RT_AT(3, 523), rt_chk(s217_ternaryTail(up, rt_chk(s218_binary(up, rt_i(1)), "ternary")), "ternary")));
}

static wv s217_ternaryTail(struct fr49 *up, wv v316_c) {
  rt_enter("ternaryTail");
  RT_AT(3, 526);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S96)), "ternaryTail")))) {
    RT_AT(3, 526);
    RT_RET(v316_c);
  }
  RT_AT(3, 527);
  wv v317_q = rt_bind(rt_chk(s195_next(up), "ternaryTail"), 0);
  RT_AT(3, 528);
  (void)rt_chk(s199_lintSpace(up, v317_q), "ternaryTail");
  RT_AT(3, 529);
  wv v318_a = rt_bind(rt_chk(s216_ternary(up), "ternaryTail"), 0);
  RT_AT(3, 530);
  wv v319_colon = rt_bind(rt_chk(s197_expect(up, rt_s(&S28), rt_s(&S292), RT_NULL), "ternaryTail"), 0);
  RT_AT(3, 531);
  (void)rt_chk(s199_lintSpace(up, v319_colon), "ternaryTail");
  RT_AT(3, 532);
  wv v320_b = rt_bind(rt_chk(s216_ternary(up), "ternaryTail"), 0);
  RT_RET((RT_AT(3, 533), ({ wv t424 = v316_c; wv t425 = v318_a; wv t426 = v320_b; wv t427 = rt_get(&F_pos, v317_q); rt_rec(0, 6, K23, (wv[]){ rt_s(&S195), t424, t425, t426, t427, rt_b(0) }); })));
}

static wv s218_binary(struct fr49 *up, wv v321_min) {
  rt_enter("binary");
  v321_min = rt_bind(v321_min, 0);
  RT_RET((RT_AT(3, 536), ({ wv t428 = rt_chk(s220_unary(up), "binary"); wv t429 = v321_min; rt_chk(s219_binaryFrom(up, t428, t429), "binary"); })));
}

static wv s219_binaryFrom(struct fr49 *up, wv v322_left, wv v323_min) {
  rt_enter("binaryFrom");
  v322_left = rt_bind(v322_left, 0);
  v323_min = rt_bind(v323_min, 0);
  RT_AT(3, 539);
  wv v324_acc = v322_left;
  RT_AT(3, 540);
  while (rt_cond((RT_AT(3, 540), rt_b(1)))) {
    RT_AT(3, 541);
    wv v325_op = rt_bind(rt_chk(s191_peek(up), "binaryFrom"), 0);
    RT_AT(3, 542);
    wv v326_prec = rt_bind(rt_chk(w4_precedence(rt_get(&F_t, v325_op)), "binaryFrom"), 0);
    RT_AT(3, 543);
    if (rt_cond(rt_lt(v326_prec, v323_min))) {
      RT_AT(3, 543);
      RT_RET(v324_acc);
    }
    RT_AT(3, 544);
    (void)rt_chk(s195_next(up), "binaryFrom");
    RT_AT(3, 545);
    (void)rt_chk(s199_lintSpace(up, v325_op), "binaryFrom");
    RT_AT(3, 546);
    wv v327_right = rt_chk(s218_binary(up, rt_add(v326_prec, rt_i(1))), "binaryFrom");
    RT_AT(3, 547);
    v324_acc = ({ wv t430 = rt_get(&F_t, v325_op); wv t431 = v324_acc; wv t432 = v327_right; wv t433 = rt_get(&F_pos, v325_op); rt_rec(0, 6, K24, (wv[]){ rt_s(&S194), t430, t431, t432, t433, rt_b(0) }); });
  }
  RT_RET(RT_NULL);
}

static wv s220_unary(struct fr49 *up) {
  rt_enter("unary");
  RT_AT(3, 552);
  wv v328_tk = rt_bind(rt_chk(s191_peek(up), "unary"), 0);
  RT_AT(3, 553);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S86)), "||") || rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S95)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S94)), "||")))) {
    RT_AT(3, 554);
    (void)rt_chk(s195_next(up), "unary");
    RT_AT(3, 555);
    wv v329_e = rt_bind(rt_chk(s220_unary(up), "unary"), 0);
    RT_AT(3, 556);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S86)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v329_e), rt_s(&S194)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v329_e), rt_s(&S76)), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v329_e)), "&&")))) {
      RT_AT(3, 557);
      (void)rt_chk(w4_failAtHint(v328_tk, rt_s(&S293), rt_s(&S294)), "unary");
    }
    RT_AT(3, 560);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S86)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v329_e), rt_s(&S139)), "||") || rt_bool(rt_eq(rt_get(&F_k, v329_e), rt_s(&S149)), "||")), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v329_e)), "&&")))) {
      RT_AT(3, 561);
      RT_RET(({ wv t434 = rt_get(&F_k, v329_e); wv t435 = rt_neg(rt_get(&F_v, v329_e)); wv t436 = rt_get(&F_pos, v328_tk); rt_rec(0, 4, K25, (wv[]){ t434, t435, t436, rt_b(0) }); }));
    }
    RT_AT(3, 563);
    RT_RET(({ wv t437 = rt_get(&F_t, v328_tk); wv t438 = v329_e; wv t439 = rt_get(&F_pos, v328_tk); rt_rec(0, 5, K26, (wv[]){ rt_s(&S193), t437, t438, t439, rt_b(0) }); }));
  }
  RT_RET((RT_AT(3, 565), rt_chk(s221_powerTail(up, rt_chk(s222_postfix(up), "unary")), "unary")));
}

static wv s221_powerTail(struct fr49 *up, wv v330_base) {
  rt_enter("powerTail");
  RT_AT(3, 569);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S76)), "powerTail")))) {
    RT_AT(3, 569);
    RT_RET(v330_base);
  }
  RT_AT(3, 570);
  wv v331_op = rt_bind(rt_chk(s195_next(up), "powerTail"), 0);
  RT_AT(3, 571);
  (void)rt_chk(s199_lintSpace(up, v331_op), "powerTail");
  RT_RET((RT_AT(3, 572), ({ wv t440 = v330_base; wv t441 = rt_chk(s220_unary(up), "powerTail"); wv t442 = rt_get(&F_pos, v331_op); rt_rec(0, 6, K24, (wv[]){ rt_s(&S194), rt_s(&S76), t440, t441, t442, rt_b(0) }); })));
}

static wv s222_postfix(struct fr49 *up) {
  rt_enter("postfix");
  RT_AT(3, 578);
  wv v332_e = rt_chk(s224_primary(up), "postfix");
  RT_AT(3, 579);
  while (rt_cond((RT_AT(3, 579), rt_b(1)))) {
    RT_AT(3, 580);
    if (rt_cond(rt_chk(s194_atCall(up), "postfix"))) {
      RT_AT(3, 581);
      v332_e = rt_chk(s223_call(up, rt_arr(1, (wv[]){ v332_e }, 0)), "postfix");
    } else if (rt_cond(rt_chk(s192_at(up, rt_s(&S99)), "postfix"))) {
      RT_AT(3, 583);
      wv v333_open = rt_bind(rt_chk(s195_next(up), "postfix"), 0);
      RT_AT(3, 584);
      if (rt_cond(rt_b(rt_bool(rt_get(&F_nl, v333_open), "&&") && rt_bool(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)), "&&")))) {
        RT_AT(3, 585);
        (void)({ wv t443 = w2_warnings; wv t444 = rt_chk(w2_warn(rt_get(&F_pos, v333_open), rt_s(&S295), rt_s(&S296)), "postfix"); std_push(t443, t444); });
      }
      RT_AT(3, 588);
      wv v334_index = rt_bind(rt_chk(s215_expr(up), "postfix"), 0);
      RT_AT(3, 589);
      (void)rt_chk(s197_expect(up, rt_s(&S100), rt_s(&S297), RT_NULL), "postfix");
      RT_AT(3, 590);
      v332_e = ({ wv t445 = v334_index; wv t446 = v332_e; wv t447 = rt_get(&F_pos, v333_open); rt_rec(0, 5, K27, (wv[]){ rt_s(&S191), t445, t446, t447, rt_b(0) }); });
    } else if (rt_cond(rt_chk(s192_at(up, rt_s(&S130)), "postfix"))) {
      RT_AT(3, 592);
      (void)rt_chk(s195_next(up), "postfix");
      RT_AT(3, 593);
      wv v335_tk = rt_bind(rt_chk(s191_peek(up), "postfix"), 0);
      RT_AT(3, 594);
      if (rt_cond(rt_ne(rt_get(&F_t, v335_tk), rt_s(&S138)))) {
        RT_AT(3, 595);
        wv v336_message = rt_bind((rt_cond(({ wv t448 = w3_keywords; wv t449 = rt_get(&F_t, v335_tk); rt_chk(w1_contains(t448, t449), "postfix"); })) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_t, v335_tk)), rt_s(&S298) }) : rt_cat(2, (wv[]){ rt_s(&S299), rt_text(rt_chk(w4_describe(v335_tk), "postfix")) })), 0);
        RT_AT(3, 597);
        (void)rt_chk(w4_failAtHint(v335_tk, v336_message, rt_s(&S134)), "postfix");
      }
      RT_AT(3, 599);
      (void)rt_chk(s195_next(up), "postfix");
      RT_AT(3, 600);
      v332_e = ({ wv t450 = rt_get(&F_v, v335_tk); wv t451 = v332_e; wv t452 = rt_get(&F_pos, v335_tk); rt_rec(0, 5, K28, (wv[]){ rt_s(&S192), t450, t451, t452, rt_b(0) }); });
    } else {
      break;
    }
  }
  RT_RET((RT_AT(3, 605), v332_e));
}

static wv s223_call(struct fr49 *up, wv v337_args) {
  rt_enter("call");
  RT_AT(3, 610);
  (void)rt_chk(s195_next(up), "call");
  RT_AT(3, 611);
  wv v338_c = rt_bind(rt_chk(s195_next(up), "call"), 0);
  RT_AT(3, 612);
  if (rt_cond(rt_ne(rt_chk(s196_accept(up, rt_s(&S130)), "call"), RT_NULL))) {
    RT_AT(3, 613);
    wv v339_m = rt_bind(rt_chk(s195_next(up), "call"), 0);
    RT_AT(3, 614);
    (void)rt_chk(s195_next(up), "call");
    RT_AT(3, 615);
    wv v340_rec = ({ wv t453 = rt_get(&F_v, v338_c); wv t454 = rt_get(&F_pos, v338_c); rt_rec(0, 4, K29, (wv[]){ rt_s(&S214), t453, t454, rt_b(0) }); });
    RT_AT(3, 616);
    RT_RET(({ wv t458 = ({ wv t455 = rt_get(&F_v, v339_m); wv t456 = v340_rec; wv t457 = rt_get(&F_pos, v339_m); rt_rec(0, 5, K28, (wv[]){ rt_s(&S192), t455, t456, t457, rt_b(0) }); }); wv t459 = v337_args; wv t460 = rt_get(&F_pos, v339_m); rt_rec(0, 5, K30, (wv[]){ rt_s(&S184), t458, t459, t460, rt_b(0) }); }));
  }
  RT_AT(3, 621);
  (void)rt_chk(s195_next(up), "call");
  RT_RET((RT_AT(3, 622), ({ wv t464 = ({ wv t461 = (rt_cond(rt_eq(rt_get(&F_t, v338_c), rt_s(&S138))) ? rt_s(&S214) : rt_s(&S157)); wv t462 = rt_get(&F_v, v338_c); wv t463 = rt_get(&F_pos, v338_c); rt_rec(0, 4, K29, (wv[]){ t461, t462, t463, rt_b(0) }); }); wv t465 = v337_args; wv t466 = rt_get(&F_pos, v338_c); rt_rec(0, 5, K30, (wv[]){ rt_s(&S184), t464, t465, t466, rt_b(0) }); })));
}

static wv s224_primary(struct fr49 *up) {
  rt_enter("primary");
  RT_AT(3, 629);
  wv v341_tk = rt_bind(rt_chk(s191_peek(up), "primary"), 0);
  RT_AT(3, 630);
  wv v342_at0 = rt_bind(rt_get(&F_pos, v341_tk), 0);
  RT_AT(3, 631);
  wv v343_kind = rt_bind(rt_get(&F_t, v341_tk), 0);
  RT_AT(3, 632);
  if (rt_cond(rt_b(rt_bool(rt_eq(v343_kind, rt_s(&S39)), "||") || rt_bool(rt_eq(v343_kind, rt_s(&S177)), "||")))) {
    RT_AT(3, 632);
    RT_RET(rt_chk(s230_chain(up), "primary"));
  }
  RT_AT(3, 633);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S45)))) {
    RT_AT(3, 633);
    RT_RET(rt_chk(s228_parseMatch(up), "primary"));
  }
  RT_AT(3, 635);
  if (rt_cond(rt_chk(s194_atCall(up), "primary"))) {
    RT_AT(3, 635);
    RT_RET(rt_chk(s223_call(up, rt_arr(0, NULL, 0)), "primary"));
  }
  RT_AT(3, 636);
  (void)rt_chk(s195_next(up), "primary");
  RT_AT(3, 637);
  if (rt_cond(rt_b(rt_bool(rt_eq(v343_kind, rt_s(&S139)), "||") || rt_bool(rt_eq(v343_kind, rt_s(&S149)), "||")))) {
    RT_AT(3, 637);
    RT_RET(({ wv t467 = v343_kind; wv t468 = rt_get(&F_v, v341_tk); wv t469 = v342_at0; rt_rec(0, 4, K25, (wv[]){ t467, t468, t469, rt_b(0) }); }));
  }
  RT_AT(3, 638);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S173)))) {
    RT_AT(3, 639);
    wv v344_parts = rt_arr(0, NULL, 1);
    RT_AT(3, 640);
    wv v345_k = rt_i(0);
    RT_AT(3, 641);
    while (rt_cond((RT_AT(3, 641), ({ wv t470 = v345_k; wv t471 = std_length(rt_get(&F_parts, v341_tk)); rt_lt(t470, t471); })))) {
      RT_AT(3, 642);
      wv v346_part = rt_bind(({ wv t472 = v345_k; wv t473 = rt_get(&F_parts, v341_tk); rt_idx(t472, t473); }), 0);
      RT_AT(3, 643);
      if (rt_cond(rt_eq(std_typeOf(v346_part), rt_s(&S187)))) {
        RT_AT(3, 644);
        (void)std_push(v344_parts, v346_part);
      } else {
        RT_AT(3, 646);
        (void)({ wv t474 = v344_parts; wv t475 = rt_chk(w4_parser(rt_get(&F_tokens, v346_part), rt_b(1)), "primary"); std_push(t474, t475); });
      }
      RT_AT(3, 648);
      v345_k = rt_add(v345_k, rt_i(1));
    }
    RT_AT(3, 650);
    RT_RET(rt_rec(0, 4, K31, (wv[]){ rt_s(&S173), v344_parts, v342_at0, rt_b(0) }));
  }
  RT_AT(3, 652);
  if (rt_cond(rt_b(rt_bool(rt_eq(v343_kind, rt_s(&S50)), "||") || rt_bool(rt_eq(v343_kind, rt_s(&S51)), "||")))) {
    RT_AT(3, 652);
    RT_RET(({ wv t476 = rt_eq(v343_kind, rt_s(&S50)); wv t477 = v342_at0; rt_rec(0, 4, K25, (wv[]){ rt_s(&S300), t476, t477, rt_b(0) }); }));
  }
  RT_AT(3, 653);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S52)))) {
    RT_AT(3, 653);
    RT_RET(rt_rec(0, 3, K32, (wv[]){ rt_s(&S52), v342_at0, rt_b(0) }));
  }
  RT_AT(3, 654);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S53)))) {
    RT_AT(3, 654);
    RT_RET(rt_rec(0, 3, K32, (wv[]){ rt_s(&S53), v342_at0, rt_b(0) }));
  }
  RT_AT(3, 655);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S54)))) {
    RT_AT(3, 655);
    RT_RET(rt_rec(0, 3, K32, (wv[]){ rt_s(&S54), v342_at0, rt_b(0) }));
  }
  RT_AT(3, 656);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S138)))) {
    RT_AT(3, 656);
    RT_RET(({ wv t478 = rt_get(&F_v, v341_tk); wv t479 = v342_at0; rt_rec(0, 4, K29, (wv[]){ rt_s(&S214), t478, t479, rt_b(0) }); }));
  }
  RT_AT(3, 657);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S157)))) {
    RT_AT(3, 657);
    RT_RET(({ wv t480 = rt_get(&F_v, v341_tk); wv t481 = v342_at0; rt_rec(0, 4, K29, (wv[]){ rt_s(&S157), t480, t481, rt_b(0) }); }));
  }
  RT_AT(3, 658);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S98)))) {
    RT_AT(3, 659);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S16)), "primary"))) {
      RT_AT(3, 659);
      (void)rt_chk(w4_failAtHint(v341_tk, rt_s(&S301), rt_s(&S302)), "primary");
    }
    RT_AT(3, 660);
    wv v347_e = rt_bind(rt_chk(s215_expr(up), "primary"), 1);
    RT_AT(3, 661);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S97)), "primary"))) {
      RT_AT(3, 663);
      wv v348_args = rt_arr(1, (wv[]){ v347_e }, 1);
      RT_AT(3, 664);
      while (rt_cond((RT_AT(3, 664), rt_ne(rt_chk(s196_accept(up, rt_s(&S97)), "primary"), RT_NULL)))) {
        RT_AT(3, 665);
        if (rt_cond(rt_chk(s192_at(up, rt_s(&S16)), "primary"))) {
          RT_AT(3, 665);
          (void)rt_chk(w4_failAt(rt_chk(s191_peek(up), "primary"), rt_s(&S303)), "primary");
        }
        RT_AT(3, 666);
        (void)({ wv t482 = v348_args; wv t483 = rt_chk(s215_expr(up), "primary"); std_push(t482, t483); });
      }
      RT_AT(3, 668);
      (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S304), RT_NULL), "primary");
      RT_AT(3, 669);
      if (rt_cond(rt_not(rt_chk(s194_atCall(up), "primary")))) {
        RT_AT(3, 670);
        (void)rt_chk(w4_failAtHint(v341_tk, rt_s(&S305), rt_s(&S306)), "primary");
      }
      RT_AT(3, 673);
      RT_RET(rt_chk(s223_call(up, v348_args), "primary"));
    }
    RT_AT(3, 675);
    (void)rt_chk(s197_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "primary");
    RT_AT(3, 676);
    rt_set_mem(rt_b(1), &F_paren, v347_e);
    RT_AT(3, 677);
    RT_RET(v347_e);
  }
  RT_AT(3, 679);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S99)))) {
    RT_AT(3, 680);
    wv v349_items = rt_arr(0, NULL, 1);
    RT_AT(3, 682);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S28)), "primary"))) {
      RT_AT(3, 683);
      (void)rt_chk(s195_next(up), "primary");
      RT_AT(3, 684);
      (void)rt_chk(s197_expect(up, rt_s(&S100), rt_s(&S307), rt_s(&S308)), "primary");
      RT_AT(3, 685);
      RT_RET(({ wv t484 = rt_arr(0, NULL, 0); wv t485 = v342_at0; rt_rec(0, 4, K33, (wv[]){ rt_s(&S189), t484, t485, rt_b(0) }); }));
    }
    RT_AT(3, 687);
    if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S100)), "primary")))) {
      RT_AT(3, 688);
      (void)({ wv t486 = v349_items; wv t487 = rt_chk(s215_expr(up), "primary"); std_push(t486, t487); });
      RT_AT(3, 689);
      if (rt_cond(rt_chk(s192_at(up, rt_s(&S28)), "primary"))) {
        RT_AT(3, 689);
        RT_RET(({ wv t488 = v341_tk; wv t489 = rt_idx(rt_i(0), v349_items); rt_chk(s227_mapLiteral(up, t488, t489), "primary"); }));
      }
      RT_AT(3, 690);
      while (rt_cond((RT_AT(3, 690), rt_chk(s192_at(up, rt_s(&S97)), "primary")))) {
        RT_AT(3, 691);
        wv v350_comma = rt_bind(rt_chk(s195_next(up), "primary"), 0);
        RT_AT(3, 692);
        if (rt_cond(rt_chk(s192_at(up, rt_s(&S100)), "primary"))) {
          RT_AT(3, 692);
          (void)rt_chk(w4_failAt(v350_comma, rt_s(&S309)), "primary");
        }
        RT_AT(3, 693);
        (void)({ wv t490 = v349_items; wv t491 = rt_chk(s215_expr(up), "primary"); std_push(t490, t491); });
      }
    }
    RT_AT(3, 696);
    (void)rt_chk(s197_expect(up, rt_s(&S100), rt_s(&S310), RT_NULL), "primary");
    RT_AT(3, 697);
    RT_RET(rt_rec(0, 4, K33, (wv[]){ rt_s(&S188), v349_items, v342_at0, rt_b(0) }));
  }
  RT_AT(3, 699);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v343_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v343_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v343_kind, rt_s(&S37)), "||")), "||") || rt_bool(rt_eq(v343_kind, rt_s(&S38)), "||")))) {
    RT_AT(3, 700);
    (void)({ wv t492 = v341_tk; wv t493 = rt_cat(3, (wv[]){ rt_s(&S311), rt_text(v343_kind), rt_s(&S114) }); wv t494 = rt_cat(3, (wv[]){ rt_s(&S312), rt_text(v343_kind), rt_s(&S313) }); rt_chk(w4_failAtHint(t492, t493, t494), "primary"); });
  }
  RT_AT(3, 702);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S56)))) {
    RT_AT(3, 702);
    (void)rt_chk(w4_failAtHint(v341_tk, rt_s(&S314), rt_s(&S315)), "primary");
  }
  RT_AT(3, 703);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S112)))) {
    RT_AT(3, 704);
    (void)({ wv t495 = v341_tk; wv t496 = rt_cat(3, (wv[]){ rt_s(&S311), rt_text(rt_get(&F_v, v341_tk)), rt_s(&S183) }); wv t497 = rt_cat(3, (wv[]){ rt_s(&S316), rt_text(rt_get(&F_v, v341_tk)), rt_s(&S317) }); rt_chk(w4_failAtHint(t495, t496, t497), "primary"); });
  }
  RT_AT(3, 706);
  if (rt_cond(rt_eq(v343_kind, rt_s(&S101)))) {
    RT_AT(3, 706);
    RT_RET(rt_chk(s225_record(up, v341_tk), "primary"));
  }
  RT_AT(3, 707);
  (void)({ wv t498 = v341_tk; wv t499 = rt_cat(2, (wv[]){ rt_s(&S318), rt_text(rt_chk(w4_describe(v341_tk), "primary")) }); rt_chk(w4_failAt(t498, t499), "primary"); });
  RT_RET(RT_NULL);
}

static wv s225_record(struct fr49 *up, wv v351_open) {
  rt_enter("record");
  v351_open = rt_bind(v351_open, 0);
  RT_AT(3, 712);
  wv v352_fields = rt_arr(0, NULL, 1);
  RT_AT(3, 713);
  wv v353_seen = rt_arr(0, NULL, 1);
  RT_AT(3, 714);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S15)), "record")))) {
    RT_AT(3, 715);
    while (rt_cond((RT_AT(3, 715), rt_b(1)))) {
      RT_AT(3, 716);
      wv v354_key = rt_bind(rt_chk(s191_peek(up), "record"), 0);
      RT_AT(3, 717);
      if (rt_cond(rt_ne(rt_get(&F_t, v354_key), rt_s(&S138)))) {
        RT_AT(3, 718);
        wv v356_message = rt_bind((rt_cond(({ wv t500 = w3_keywords; wv t501 = rt_get(&F_t, v354_key); rt_chk(w1_contains(t500, t501), "record"); })) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_t, v354_key)), rt_s(&S298) }) : rt_cat(2, (wv[]){ rt_s(&S319), rt_text(rt_chk(w4_describe(v354_key), "record")) })), 0);
        RT_AT(3, 720);
        (void)rt_chk(w4_failAtHint(v354_key, v356_message, rt_s(&S320)), "record");
      }
      RT_AT(3, 722);
      (void)rt_chk(s195_next(up), "record");
      RT_AT(3, 723);
      if (rt_cond(({ wv t502 = v353_seen; wv t503 = rt_get(&F_v, v354_key); rt_chk(w1_contains(t502, t503), "record"); }))) {
        RT_AT(3, 723);
        (void)({ wv t504 = v354_key; wv t505 = rt_cat(3, (wv[]){ rt_s(&S321), rt_text(rt_get(&F_v, v354_key)), rt_s(&S322) }); rt_chk(w4_failAt(t504, t505), "record"); });
      }
      RT_AT(3, 724);
      (void)({ wv t506 = v353_seen; wv t507 = rt_get(&F_v, v354_key); std_push(t506, t507); });
      RT_AT(3, 725);
      if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S28)), "record")))) {
        RT_AT(3, 726);
        wv v357_found = rt_bind(rt_chk(s191_peek(up), "record"), 0);
        RT_AT(3, 727);
        wv v358_hint = rt_bind((rt_cond(rt_b(rt_bool(rt_chk(s192_at(up, rt_s(&S97)), "record"), "||") || rt_bool(rt_chk(s192_at(up, rt_s(&S15)), "record"), "||"))) ? ({ wv t508 = rt_text(rt_get(&F_v, v354_key)); wv t509 = rt_text(rt_get(&F_v, v354_key)); rt_cat(4, (wv[]){ rt_s(&S323), t508, rt_s(&S29), t509 }); }) : RT_NULL), 0);
        RT_AT(3, 728);
        (void)({ wv t512 = v357_found; wv t513 = ({ wv t510 = rt_text(rt_get(&F_v, v354_key)); wv t511 = rt_text(rt_chk(w4_describe(v357_found), "record")); rt_cat(4, (wv[]){ rt_s(&S324), t510, rt_s(&S198), t511 }); }); wv t514 = v358_hint; rt_chk(w4_failAtHint(t512, t513, t514), "record"); });
      }
      RT_AT(3, 730);
      (void)rt_chk(s195_next(up), "record");
      RT_AT(3, 731);
      (void)({ wv t518 = v352_fields; wv t519 = ({ wv t515 = rt_get(&F_v, v354_key); wv t516 = rt_chk(s215_expr(up), "record"); wv t517 = rt_get(&F_pos, v354_key); rt_rec(0, 3, K34, (wv[]){ t515, t516, t517 }); }); std_push(t518, t519); });
      RT_AT(3, 732);
      if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S97)), "record")))) {
        break;
      }
      RT_AT(3, 733);
      wv v355_comma = rt_bind(rt_chk(s195_next(up), "record"), 0);
      RT_AT(3, 734);
      if (rt_cond(rt_chk(s192_at(up, rt_s(&S15)), "record"))) {
        RT_AT(3, 734);
        (void)rt_chk(w4_failAt(v355_comma, rt_s(&S325)), "record");
      }
    }
  }
  RT_AT(3, 737);
  (void)rt_chk(s197_expect(up, rt_s(&S15), rt_s(&S326), RT_NULL), "record");
  RT_RET((RT_AT(3, 738), ({ wv t520 = v352_fields; wv t521 = rt_get(&F_pos, v351_open); rt_rec(0, 4, K35, (wv[]){ rt_s(&S190), t520, t521, rt_b(0) }); })));
}

static wv s226_constantKey(struct fr49 *up, wv v359_e) {
  rt_enter("constantKey");
  v359_e = rt_bind(v359_e, 0);
  RT_AT(3, 744);
  if (rt_cond(rt_eq(rt_get(&F_k, v359_e), rt_s(&S139)))) {
    RT_AT(3, 744);
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S327), rt_text(rt_get(&F_v, v359_e)) }));
  }
  RT_AT(3, 745);
  if (rt_cond(rt_eq(rt_get(&F_k, v359_e), rt_s(&S300)))) {
    RT_AT(3, 745);
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S141), rt_text(rt_get(&F_v, v359_e)) }));
  }
  RT_AT(3, 746);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v359_e), rt_s(&S173)), "&&") && rt_bool(rt_le(std_length(rt_get(&F_parts, v359_e)), rt_i(1)), "&&")), "&&") && rt_bool(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v359_e)), rt_i(0)), "||") || rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v359_e))), rt_s(&S187)), "||")), "&&")))) {
    RT_AT(3, 747);
    RT_RET(rt_add(rt_s(&S328), (rt_cond(rt_eq(std_length(rt_get(&F_parts, v359_e)), rt_i(0))) ? rt_s(&S13) : rt_idx(rt_i(0), rt_get(&F_parts, v359_e)))));
  }
  RT_AT(3, 749);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v359_e), rt_s(&S149)), "||") || rt_bool(rt_eq(rt_get(&F_k, v359_e), rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v359_e), rt_s(&S54)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v359_e), rt_s(&S52)), "||")))) {
    RT_AT(3, 750);
    (void)rt_chk(w2_failHint(rt_get(&F_pos, v359_e), rt_s(&S329), RT_NULL), "constantKey");
  }
  RT_RET((RT_AT(3, 752), RT_NULL));
}

static wv s227_mapLiteral(struct fr49 *up, wv v360_open, wv v361_first) {
  rt_enter("mapLiteral");
  v360_open = rt_bind(v360_open, 0);
  v361_first = rt_bind(v361_first, 0);
  RT_AT(3, 758);
  wv v362_items = rt_arr(0, NULL, 1);
  RT_AT(3, 759);
  wv v363_seen = rt_arr(0, NULL, 1);
  RT_AT(3, 760);
  wv v364_key = rt_bind(v361_first, 1);
  RT_AT(3, 761);
  while (rt_cond((RT_AT(3, 761), rt_b(1)))) {
    RT_AT(3, 762);
    wv v365_c = rt_bind(rt_chk(s226_constantKey(up, v364_key), "mapLiteral"), 0);
    RT_AT(3, 763);
    if (rt_cond(rt_ne(v365_c, RT_NULL))) {
      RT_AT(3, 764);
      if (rt_cond(rt_chk(w1_contains(v363_seen, v365_c), "mapLiteral"))) {
        RT_AT(3, 764);
        (void)rt_chk(w2_failHint(rt_get(&F_pos, v364_key), rt_s(&S330), RT_NULL), "mapLiteral");
      }
      RT_AT(3, 765);
      (void)std_push(v363_seen, v365_c);
    }
    RT_AT(3, 767);
    (void)rt_chk(s197_expect(up, rt_s(&S28), rt_s(&S331), rt_s(&S332)), "mapLiteral");
    RT_AT(3, 768);
    (void)std_push(v362_items, v364_key);
    RT_AT(3, 769);
    (void)({ wv t522 = v362_items; wv t523 = rt_chk(s215_expr(up), "mapLiteral"); std_push(t522, t523); });
    RT_AT(3, 770);
    if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S97)), "mapLiteral")))) {
      break;
    }
    RT_AT(3, 771);
    wv v366_comma = rt_bind(rt_chk(s195_next(up), "mapLiteral"), 0);
    RT_AT(3, 772);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S100)), "mapLiteral"))) {
      RT_AT(3, 772);
      (void)rt_chk(w4_failAt(v366_comma, rt_s(&S333)), "mapLiteral");
    }
    RT_AT(3, 773);
    v364_key = rt_bind(rt_chk(s215_expr(up), "mapLiteral"), 1);
  }
  RT_AT(3, 775);
  (void)rt_chk(s197_expect(up, rt_s(&S100), rt_s(&S334), RT_NULL), "mapLiteral");
  RT_RET((RT_AT(3, 776), ({ wv t524 = v362_items; wv t525 = rt_get(&F_pos, v360_open); rt_rec(0, 4, K33, (wv[]){ rt_s(&S189), t524, t525, rt_b(0) }); })));
}

static wv s228_parseMatch(struct fr49 *up) {
  rt_enter("parseMatch");
  RT_AT(3, 781);
  wv v367_kw = rt_bind(rt_chk(s195_next(up), "parseMatch"), 0);
  RT_AT(3, 782);
  wv v368_subject = rt_bind(rt_chk(s212_cond(up, rt_s(&S45)), "parseMatch"), 0);
  RT_AT(3, 783);
  (void)rt_chk(s197_expect(up, rt_s(&S101), rt_s(&S335), RT_NULL), "parseMatch");
  RT_AT(3, 784);
  wv v369_cases = rt_arr(0, NULL, 1);
  RT_AT(3, 785);
  wv v370_els = RT_NULL;
  RT_AT(3, 786);
  while (rt_cond((RT_AT(3, 786), rt_b(1)))) {
    RT_AT(3, 787);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S46)), "parseMatch"))) {
      RT_AT(3, 788);
      wv v371_a = rt_bind(rt_chk(s195_next(up), "parseMatch"), 0);
      RT_AT(3, 789);
      if (rt_cond(rt_ne(v370_els, RT_NULL))) {
        RT_AT(3, 789);
        (void)rt_chk(w4_failAt(v371_a, rt_s(&S336)), "parseMatch");
      }
      RT_AT(3, 790);
      if (rt_cond(rt_chk(s192_at(up, rt_s(&S101)), "parseMatch"))) {
        RT_AT(3, 791);
        (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "parseMatch"), rt_s(&S337), rt_s(&S338)), "parseMatch");
      }
      RT_AT(3, 793);
      wv v372_values = rt_arr(0, NULL, 1);
      RT_AT(3, 794);
      (void)({ wv t526 = v372_values; wv t527 = rt_chk(s229_caseValue(up), "parseMatch"); std_push(t526, t527); });
      RT_AT(3, 795);
      while (rt_cond((RT_AT(3, 795), rt_ne(rt_chk(s196_accept(up, rt_s(&S97)), "parseMatch"), RT_NULL)))) {
        RT_AT(3, 795);
        (void)({ wv t528 = v372_values; wv t529 = rt_chk(s229_caseValue(up), "parseMatch"); std_push(t528, t529); });
      }
      RT_AT(3, 796);
      (void)({ wv t533 = v369_cases; wv t534 = ({ wv t530 = v372_values; wv t531 = rt_chk(s213_block(up), "parseMatch"); wv t532 = rt_get(&F_pos, v371_a); rt_rec(0, 3, K36, (wv[]){ t530, t531, t532 }); }); std_push(t533, t534); });
    } else if (rt_cond(rt_chk(s192_at(up, rt_s(&S40)), "parseMatch"))) {
      RT_AT(3, 798);
      wv v373_a = rt_bind(rt_chk(s195_next(up), "parseMatch"), 0);
      RT_AT(3, 799);
      if (rt_cond(rt_ne(v370_els, RT_NULL))) {
        RT_AT(3, 799);
        (void)rt_chk(w4_failAt(v373_a, rt_s(&S339)), "parseMatch");
      }
      RT_AT(3, 800);
      if (rt_cond(rt_eq(std_length(v369_cases), rt_i(0)))) {
        RT_AT(3, 800);
        (void)rt_chk(w4_failAt(v373_a, rt_s(&S340)), "parseMatch");
      }
      RT_AT(3, 801);
      v370_els = rt_bind(rt_chk(s213_block(up), "parseMatch"), 1);
    } else {
      break;
    }
  }
  RT_AT(3, 806);
  if (rt_cond(rt_eq(std_length(v369_cases), rt_i(0)))) {
    RT_AT(3, 807);
    (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "parseMatch"), rt_s(&S341), rt_s(&S342)), "parseMatch");
  }
  RT_AT(3, 809);
  (void)rt_chk(s197_expect(up, rt_s(&S15), rt_s(&S343), RT_NULL), "parseMatch");
  RT_RET((RT_AT(3, 810), ({ wv t535 = v368_subject; wv t536 = v369_cases; wv t537 = v370_els; wv t538 = rt_get(&F_pos, v367_kw); rt_rec(0, 6, K37, (wv[]){ rt_s(&S45), t535, t536, t537, t538, rt_b(0) }); })));
}

static wv s229_caseValue(struct fr49 *up) {
  rt_enter("caseValue");
  RT_AT(3, 815);
  wv v374_tk = rt_bind(rt_chk(s191_peek(up), "caseValue"), 0);
  RT_AT(3, 816);
  wv v375_v = rt_bind(rt_chk(s220_unary(up), "caseValue"), 0);
  RT_AT(3, 817);
  wv v376_kind = rt_bind(rt_get(&F_k, v375_v), 0);
  RT_AT(3, 818);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v376_kind, rt_s(&S139)), "||") || rt_bool(rt_eq(v376_kind, rt_s(&S300)), "||")), "||") || rt_bool(rt_eq(v376_kind, rt_s(&S52)), "||")))) {
    RT_AT(3, 818);
    RT_RET(v375_v);
  }
  RT_AT(3, 820);
  if (rt_cond(rt_b(rt_bool(rt_eq(v376_kind, rt_s(&S192)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v375_v)), rt_s(&S214)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v375_v)), rt_s(&S192)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, rt_get(&F_rec, v375_v))), rt_s(&S214)), "&&")), "||")), "&&")))) {
    RT_AT(3, 820);
    RT_RET(v375_v);
  }
  RT_AT(3, 821);
  if (rt_cond(rt_eq(v376_kind, rt_s(&S173)))) {
    RT_AT(3, 822);
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v375_v)), rt_i(1)), "&&") && rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v375_v))), rt_s(&S187)), "&&")))) {
      RT_AT(3, 822);
      RT_RET(v375_v);
    }
    RT_AT(3, 823);
    (void)rt_chk(w4_failAt(v374_tk, rt_s(&S344)), "caseValue");
  }
  RT_AT(3, 825);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v376_kind, rt_s(&S149)), "||") || rt_bool(rt_eq(v376_kind, rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(v376_kind, rt_s(&S54)), "||")))) {
    RT_AT(3, 826);
    (void)rt_chk(w4_failAtHint(v374_tk, rt_s(&S345), rt_s(&S346)), "caseValue");
  }
  RT_AT(3, 828);
  (void)rt_chk(w4_failAtHint(v374_tk, rt_s(&S347), rt_s(&S348)), "caseValue");
  RT_RET((RT_AT(3, 830), RT_NULL));
}

static wv s230_chain(struct fr49 *up) {
  rt_enter("chain");
  RT_AT(3, 834);
  wv v377_branches = rt_arr(0, NULL, 1);
  RT_AT(3, 835);
  wv v378_els = RT_NULL;
  RT_AT(3, 836);
  while (rt_cond((RT_AT(3, 836), rt_b(1)))) {
    RT_AT(3, 837);
    wv v379_kw = rt_bind(rt_chk(s195_next(up), "chain"), 0);
    RT_AT(3, 838);
    wv v380_c = rt_bind(rt_chk(s212_cond(up, rt_get(&F_t, v379_kw)), "chain"), 0);
    RT_AT(3, 839);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v379_kw), rt_s(&S177)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v380_c), rt_s(&S194)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v380_c), rt_s(&S78)), "&&")))) {
      RT_AT(3, 840);
      (void)({ wv t539 = w2_warnings; wv t540 = rt_chk(w2_warn(rt_get(&F_pos, v379_kw), rt_s(&S349), rt_s(&S350)), "chain"); std_push(t539, t540); });
    }
    RT_AT(3, 842);
    (void)({ wv t545 = v377_branches; wv t546 = ({ wv t541 = rt_eq(rt_get(&F_t, v379_kw), rt_s(&S177)); wv t542 = v380_c; wv t543 = rt_chk(s213_block(up), "chain"); wv t544 = rt_get(&F_pos, v379_kw); rt_rec(0, 4, K38, (wv[]){ t541, t542, t543, t544 }); }); std_push(t545, t546); });
    RT_AT(3, 843);
    if (rt_cond(rt_eq(rt_chk(s196_accept(up, rt_s(&S40)), "chain"), RT_NULL))) {
      break;
    }
    RT_AT(3, 844);
    if (rt_cond(rt_b(rt_bool(rt_chk(s192_at(up, rt_s(&S39)), "chain"), "||") || rt_bool(rt_chk(s192_at(up, rt_s(&S177)), "chain"), "||")))) {
      continue;
    }
    RT_AT(3, 845);
    v378_els = rt_bind(rt_chk(s213_block(up), "chain"), 1);
    break;
  }
  RT_RET((RT_AT(3, 848), ({ wv t547 = v377_branches; wv t548 = v378_els; wv t549 = rt_get(&F_pos, rt_idx(rt_i(0), v377_branches)); rt_rec(0, 5, K39, (wv[]){ rt_s(&S186), t547, t548, t549, rt_b(0) }); })));
}

static wv w5_stdSig(wv v383_name) {
  rt_enter("stdSig");
  v383_name = rt_bind(v383_name, 0);
  RT_AT(4, 45);
  wv v384_k = rt_i(0);
  RT_AT(4, 46);
  while (rt_cond((RT_AT(4, 46), ({ wv t605 = v384_k; wv t606 = std_length(rt_def(w5_stdlib, "stdlib")); rt_lt(t605, t606); })))) {
    RT_AT(4, 47);
    if (rt_cond(({ wv t609 = rt_get(&F_name, ({ wv t607 = v384_k; wv t608 = rt_def(w5_stdlib, "stdlib"); rt_idx(t607, t608); })); wv t610 = v383_name; rt_eq(t609, t610); }))) {
      RT_AT(4, 47);
      RT_RET(rt_get(&F_arity, ({ wv t611 = v384_k; wv t612 = rt_def(w5_stdlib, "stdlib"); rt_idx(t611, t612); })));
    }
    RT_AT(4, 48);
    v384_k = rt_add(v384_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 50), RT_NULL));
}

static wv w5_modeOf(wv v385_x) {
  rt_enter("modeOf");
  v385_x = rt_bind(v385_x, 0);
  RT_RET((RT_AT(4, 54), (rt_cond(rt_get(&F_isVar, v385_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v385_x)) ? rt_s(&S37) : rt_s(&S13)))));
}

static wv w5_kindText(wv v386_kind) {
  rt_enter("kindText");
  v386_kind = rt_bind(v386_kind, 0);
  RT_RET((RT_AT(4, 57), (rt_cond(rt_eq(v386_kind, rt_s(&S33))) ? rt_s(&S406) : (rt_cond(rt_eq(v386_kind, rt_s(&S47))) ? rt_s(&S407) : rt_cat(2, (wv[]){ rt_s(&S0), rt_text(v386_kind) })))));
}

static wv w5_plural(wv v387_n) {
  rt_enter("plural");
  v387_n = rt_bind(v387_n, 0);
  RT_RET((RT_AT(4, 59), ({ wv t613 = rt_cat(2, (wv[]){ rt_text(v387_n), rt_s(&S408) }); wv t614 = (rt_cond(rt_eq(v387_n, rt_i(1))) ? rt_s(&S13) : rt_s(&S328)); rt_add(t613, t614); })));
}

static wv w5_enumText(wv v388_x) {
  rt_enter("enumText");
  v388_x = rt_bind(v388_x, 0);
  RT_RET((RT_AT(4, 67), (rt_cond(rt_eq(rt_get(&F_k, v388_x), rt_s(&S214))) ? rt_get(&F_name, v388_x) : ({ wv t615 = rt_text(rt_get(&F_name, rt_get(&F_rec, v388_x))); wv t616 = rt_text(rt_get(&F_field, v388_x)); rt_cat(3, (wv[]){ t615, rt_s(&S130), t616 }); }))));
}

static wv w5_caseKey(wv v389_v) {
  rt_enter("caseKey");
  v389_v = rt_bind(v389_v, 0);
  RT_AT(4, 71);
  if (rt_cond(rt_eq(rt_get(&F_k, v389_v), rt_s(&S173)))) {
    RT_AT(4, 71);
    RT_RET(rt_add(rt_s(&S409), rt_idx(rt_i(0), rt_get(&F_parts, v389_v))));
  }
  RT_AT(4, 72);
  if (rt_cond(rt_eq(rt_get(&F_k, v389_v), rt_s(&S192)))) {
    RT_AT(4, 72);
    RT_RET(({ wv t617 = rt_text(rt_get(&F_field, v389_v)); wv t618 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v389_v)), "caseKey")); rt_cat(4, (wv[]){ rt_s(&S410), t617, rt_s(&S130), t618 }); }));
  }
  RT_AT(4, 73);
  if (rt_cond(rt_eq(rt_get(&F_k, v389_v), rt_s(&S52)))) {
    RT_AT(4, 73);
    RT_RET(rt_s(&S52));
  }
  RT_RET((RT_AT(4, 74), ({ wv t619 = rt_text(rt_get(&F_k, v389_v)); wv t620 = rt_text(rt_get(&F_v, v389_v)); rt_cat(3, (wv[]){ t619, rt_s(&S28), t620 }); })));
}

static wv w5_caseText(wv v390_v) {
  rt_enter("caseText");
  v390_v = rt_bind(v390_v, 0);
  RT_AT(4, 77);
  if (rt_cond(rt_eq(rt_get(&F_k, v390_v), rt_s(&S173)))) {
    RT_AT(4, 77);
    RT_RET(rt_chk(w1_quote(rt_idx(rt_i(0), rt_get(&F_parts, v390_v))), "caseText"));
  }
  RT_AT(4, 78);
  if (rt_cond(rt_eq(rt_get(&F_k, v390_v), rt_s(&S192)))) {
    RT_AT(4, 78);
    RT_RET(({ wv t621 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v390_v)), "caseText")); wv t622 = rt_text(rt_get(&F_field, v390_v)); rt_cat(3, (wv[]){ t621, rt_s(&S130), t622 }); }));
  }
  RT_AT(4, 79);
  if (rt_cond(rt_eq(rt_get(&F_k, v390_v), rt_s(&S52)))) {
    RT_AT(4, 79);
    RT_RET(rt_s(&S52));
  }
  RT_RET((RT_AT(4, 80), rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v390_v)) })));
}

static wv w5_getsAwayWithValue(wv v391_list) {
  rt_enter("getsAwayWithValue");
  RT_AT(4, 85);
  wv v392_i = rt_i(0);
  RT_AT(4, 86);
  while (rt_cond((RT_AT(4, 86), ({ wv t623 = v392_i; wv t624 = std_length(v391_list); rt_lt(t623, t624); })))) {
    RT_AT(4, 87);
    wv v393_s = rt_bind(rt_idx(v392_i, v391_list), 0);
    RT_AT(4, 88);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_handler, v393_s), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v393_s)))), "getsAwayWithValue"), "&&")))) {
      RT_AT(4, 88);
      RT_RET(rt_b(1));
    }
    RT_AT(4, 89);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S34)), "&&") && rt_bool(rt_ne(rt_get(&F_e, v393_s), RT_NULL), "&&")))) {
      RT_AT(4, 89);
      RT_RET(rt_b(1));
    }
    RT_AT(4, 90);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S41)), "||") || rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S42)), "||")), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v393_s))), "getsAwayWithValue"), "&&")))) {
      RT_AT(4, 90);
      RT_RET(rt_b(1));
    }
    RT_AT(4, 91);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v393_s)), rt_s(&S186)), "&&")))) {
      RT_AT(4, 92);
      wv v394_b = rt_i(0);
      RT_AT(4, 93);
      while (rt_cond((RT_AT(4, 93), ({ wv t625 = v394_b; wv t626 = std_length(rt_get(&F_branches, rt_get(&F_e, v393_s))); rt_lt(t625, t626); })))) {
        RT_AT(4, 94);
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t627 = v394_b; wv t628 = rt_get(&F_branches, rt_get(&F_e, v393_s)); rt_idx(t627, t628); })))), "getsAwayWithValue"))) {
          RT_AT(4, 94);
          RT_RET(rt_b(1));
        }
        RT_AT(4, 95);
        v394_b = rt_add(v394_b, rt_i(1));
      }
      RT_AT(4, 97);
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v393_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v393_s)))), "getsAwayWithValue"), "&&")))) {
        RT_AT(4, 97);
        RT_RET(rt_b(1));
      }
    }
    RT_AT(4, 99);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v393_s)), rt_s(&S45)), "&&")))) {
      RT_AT(4, 100);
      wv v395_a = rt_i(0);
      RT_AT(4, 101);
      while (rt_cond((RT_AT(4, 101), ({ wv t629 = v395_a; wv t630 = std_length(rt_get(&F_cases, rt_get(&F_e, v393_s))); rt_lt(t629, t630); })))) {
        RT_AT(4, 102);
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t631 = v395_a; wv t632 = rt_get(&F_cases, rt_get(&F_e, v393_s)); rt_idx(t631, t632); })))), "getsAwayWithValue"))) {
          RT_AT(4, 102);
          RT_RET(rt_b(1));
        }
        RT_AT(4, 103);
        v395_a = rt_add(v395_a, rt_i(1));
      }
      RT_AT(4, 105);
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v393_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v393_s)))), "getsAwayWithValue"), "&&")))) {
        RT_AT(4, 105);
        RT_RET(rt_b(1));
      }
    }
    RT_AT(4, 107);
    v392_i = rt_add(v392_i, rt_i(1));
  }
  RT_RET((RT_AT(4, 109), rt_b(0)));
}

static wv w5_lineIn(wv v396_chars, wv v397_pos) {
  rt_enter("lineIn");
  v396_chars = rt_bind(v396_chars, 0);
  v397_pos = rt_bind(v397_pos, 0);
  RT_AT(4, 114);
  wv v398_line = rt_i(1);
  RT_AT(4, 115);
  wv v399_k = rt_i(0);
  RT_AT(4, 116);
  while (rt_cond((RT_AT(4, 116), rt_lt(v399_k, v397_pos)))) {
    RT_AT(4, 117);
    if (rt_cond(rt_eq(rt_idx(v399_k, v396_chars), rt_s(&S2)))) {
      RT_AT(4, 117);
      v398_line = rt_add(v398_line, rt_i(1));
    }
    RT_AT(4, 118);
    v399_k = rt_add(v399_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 120), v398_line));
}

static wv w5_lineOf(wv v400_pos) {
  rt_enter("lineOf");
  v400_pos = rt_bind(v400_pos, 0);
  RT_RET((RT_AT(4, 122), rt_chk(w5_lineIn(w2_srcChars, v400_pos), "lineOf")));
}

static wv w5_check(wv p_v401_modules, wv v402_requireExit) {
  rt_enter("check");
  struct fr101 fr = { 0 };
  fr.v401_modules = rt_bind(p_v401_modules, 0);
  v402_requireExit = rt_bind(v402_requireExit, 0);
  RT_AT(4, 132);
  fr.v403_errors = rt_arr(0, NULL, 1);
  RT_AT(4, 133);
  fr.v404_nextFn = rt_i(1);
  RT_AT(4, 135);
  fr.v405_curMod = rt_i(0);
  RT_AT(4, 139);
  fr.v406_functions = rt_arr(0, NULL, 1);
  RT_AT(4, 140);
  fr.v407_uses = rt_arr(0, NULL, 1);
  RT_AT(4, 142);
  fr.v408_enums = rt_arr(0, NULL, 1);
  RT_AT(4, 922);
  fr.v444_topFn = rt_rec(0, 2, K45, (wv[]){ rt_i(0), rt_b(1) });
  RT_AT(4, 923);
  wv v445_entry = rt_sub(std_length(fr.v401_modules), rt_i(1));
  RT_AT(4, 924);
  wv v446_mi = rt_i(0);
  RT_AT(4, 925);
  while (rt_cond((RT_AT(4, 925), ({ wv t1220 = v446_mi; wv t1221 = std_length(fr.v401_modules); rt_lt(t1220, t1221); })))) {
    RT_AT(4, 926);
    (void)({ wv t1222 = v446_mi; wv t1223 = rt_eq(v446_mi, v445_entry); rt_chk(s443_checkModule(&fr, t1222, t1223), "check"); });
    RT_AT(4, 927);
    v446_mi = rt_add(v446_mi, rt_i(1));
  }
  RT_AT(4, 930);
  wv v447_si = rt_i(0);
  RT_AT(4, 931);
  while (rt_cond((RT_AT(4, 931), ({ wv t1224 = v447_si; wv t1225 = std_length(fr.v406_functions); rt_lt(t1224, t1225); })))) {
    RT_AT(4, 932);
    wv v657_sc = rt_bind(rt_idx(v447_si, fr.v406_functions), 0);
    RT_AT(4, 933);
    wv v658_use = rt_bind(({ wv t1226 = rt_get(&F_mod, v657_sc); wv t1227 = rt_get(&F_namePos, v657_sc); rt_chk(s412_firstUse(&fr, t1226, t1227), "check"); }), 0);
    RT_AT(4, 934);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v658_use, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v657_sc)), RT_NULL), "&&")), "&&") && rt_bool(rt_not(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v657_sc))), "check")), "&&")))) {
      RT_AT(4, 935);
      (void)rt_chk(s410_enter(&fr, rt_get(&F_mod, v657_sc)), "check");
      RT_AT(4, 936);
      wv v659_um = rt_bind(rt_get(&F_useMod, v658_use), 0);
      RT_AT(4, 937);
      wv v660_where = rt_bind((rt_cond(({ wv t1228 = v659_um; wv t1229 = rt_get(&F_mod, v657_sc); rt_eq(t1228, t1229); })) ? rt_cat(2, (wv[]){ rt_s(&S505), rt_text(rt_chk(w5_lineOf(rt_get(&F_pos, v658_use)), "check")) }) : ({ wv t1232 = rt_text(rt_get(&F_file, rt_idx(v659_um, fr.v401_modules))); wv t1233 = rt_text(({ wv t1230 = rt_get(&F_chars, rt_idx(v659_um, fr.v401_modules)); wv t1231 = rt_get(&F_pos, v658_use); rt_chk(w5_lineIn(t1230, t1231), "check"); })); rt_cat(3, (wv[]){ t1232, rt_s(&S506), t1233 }); })), 0);
      RT_AT(4, 938);
      (void)({ wv t1236 = rt_get(&F_namePos, v657_sc); wv t1237 = ({ wv t1234 = rt_text(rt_get(&F_name, v657_sc)); wv t1235 = rt_text(v660_where); rt_cat(5, (wv[]){ rt_s(&S114), t1234, rt_s(&S507), t1235, rt_s(&S16) }); }); rt_chk(s409_error(&fr, t1236, t1237, rt_s(&S508)), "check"); });
    }
    RT_AT(4, 941);
    v447_si = rt_add(v447_si, rt_i(1));
  }
  RT_AT(4, 943);
  (void)rt_chk(s410_enter(&fr, v445_entry), "check");
  RT_AT(4, 944);
  wv v448_program = rt_bind(rt_get(&F_program, rt_idx(v445_entry, fr.v401_modules)), 0);
  RT_AT(4, 945);
  if (rt_cond(v402_requireExit)) {
    RT_AT(4, 946);
    wv v661_last = RT_NULL;
    RT_AT(4, 947);
    wv v662_k = rt_i(0);
    RT_AT(4, 948);
    while (rt_cond((RT_AT(4, 948), ({ wv t1238 = v662_k; wv t1239 = std_length(rt_get(&F_stmts, v448_program)); rt_lt(t1238, t1239); })))) {
      RT_AT(4, 949);
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1240 = v662_k; wv t1241 = rt_get(&F_stmts, v448_program); rt_idx(t1240, t1241); })), rt_s(&S203)))) {
        RT_AT(4, 949);
        v661_last = rt_bind(({ wv t1242 = v662_k; wv t1243 = rt_get(&F_stmts, v448_program); rt_idx(t1242, t1243); }), 1);
      }
      RT_AT(4, 950);
      v662_k = rt_add(v662_k, rt_i(1));
    }
    RT_AT(4, 952);
    if (rt_cond(rt_b(rt_bool(rt_eq(v661_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v661_last), rt_s(&S55)), "||")))) {
      RT_AT(4, 953);
      (void)rt_chk(s409_error(&fr, rt_get(&F_end, v448_program), rt_s(&S509), rt_s(&S510)), "check");
    }
  }
  RT_RET((RT_AT(4, 956), fr.v403_errors));
}

static wv s409_error(struct fr101 *up, wv v449_pos, wv v450_message, wv v451_hint) {
  rt_enter("error");
  v449_pos = rt_bind(v449_pos, 0);
  v450_message = rt_bind(v450_message, 0);
  v451_hint = rt_bind(v451_hint, 0);
  RT_AT(4, 144);
  (void)({ wv t633 = rt_def(up->v403_errors, "errors"); wv t634 = rt_chk(w2_err(v449_pos, v450_message, v451_hint), "error"); std_push(t633, t634); });
  RT_RET(RT_NULL);
}

static wv s410_enter(struct fr101 *up, wv v452_m) {
  rt_enter("enter");
  v452_m = rt_bind(v452_m, 0);
  RT_AT(4, 148);
  wv t635 = rt_bind(v452_m, 1);
  (void)rt_def(up->v405_curMod, "curMod");
  up->v405_curMod = t635;
  RT_AT(4, 149);
  (void)({ wv t636 = rt_get(&F_file, rt_idx(v452_m, up->v401_modules)); wv t637 = rt_get(&F_chars, rt_idx(v452_m, up->v401_modules)); rt_chk(w2_setSource(t636, t637), "enter"); });
  RT_RET(RT_NULL);
}

static wv s411_enumMembers(struct fr101 *up, wv v453_d) {
  rt_enter("enumMembers");
  v453_d = rt_bind(v453_d, 0);
  RT_AT(4, 154);
  wv v454_k = rt_i(0);
  RT_AT(4, 155);
  while (rt_cond((RT_AT(4, 155), ({ wv t638 = v454_k; wv t639 = std_length(rt_def(up->v408_enums, "enums")); rt_lt(t638, t639); })))) {
    RT_AT(4, 156);
    if (rt_cond(rt_b(rt_bool(({ wv t642 = rt_get(&F_mod, ({ wv t640 = v454_k; wv t641 = rt_def(up->v408_enums, "enums"); rt_idx(t640, t641); })); wv t643 = rt_get(&F_mod, v453_d); rt_eq(t642, t643); }), "&&") && rt_bool(({ wv t646 = rt_get(&F_name, ({ wv t644 = v454_k; wv t645 = rt_def(up->v408_enums, "enums"); rt_idx(t644, t645); })); wv t647 = rt_get(&F_name, v453_d); rt_eq(t646, t647); }), "&&")))) {
      RT_AT(4, 156);
      RT_RET(rt_get(&F_members, ({ wv t648 = v454_k; wv t649 = rt_def(up->v408_enums, "enums"); rt_idx(t648, t649); })));
    }
    RT_AT(4, 157);
    v454_k = rt_add(v454_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 159), RT_NULL));
}

static wv s412_firstUse(struct fr101 *up, wv v455_mod, wv v456_declPos) {
  rt_enter("firstUse");
  v455_mod = rt_bind(v455_mod, 0);
  v456_declPos = rt_bind(v456_declPos, 0);
  RT_AT(4, 164);
  wv v457_k = rt_i(0);
  RT_AT(4, 165);
  while (rt_cond((RT_AT(4, 165), ({ wv t650 = v457_k; wv t651 = std_length(rt_def(up->v407_uses, "uses")); rt_lt(t650, t651); })))) {
    RT_AT(4, 166);
    if (rt_cond(rt_b(rt_bool(({ wv t654 = rt_get(&F_mod, ({ wv t652 = v457_k; wv t653 = rt_def(up->v407_uses, "uses"); rt_idx(t652, t653); })); wv t655 = v455_mod; rt_eq(t654, t655); }), "&&") && rt_bool(({ wv t658 = rt_get(&F_namePos, ({ wv t656 = v457_k; wv t657 = rt_def(up->v407_uses, "uses"); rt_idx(t656, t657); })); wv t659 = v456_declPos; rt_eq(t658, t659); }), "&&")))) {
      RT_AT(4, 166);
      RT_RET(({ wv t660 = v457_k; wv t661 = rt_def(up->v407_uses, "uses"); rt_idx(t660, t661); }));
    }
    RT_AT(4, 167);
    v457_k = rt_add(v457_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 169), RT_NULL));
}

static wv s413_requireValue(struct fr101 *up, wv v458_b) {
  rt_enter("requireValue");
  v458_b = rt_bind(v458_b, 0);
  RT_AT(4, 174);
  if (rt_cond(rt_ne(rt_get(&F_value, v458_b), RT_NULL))) {
    RT_AT(4, 174);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 175);
  wv v459_last = RT_NULL;
  RT_AT(4, 176);
  wv v460_k = rt_i(0);
  RT_AT(4, 177);
  while (rt_cond((RT_AT(4, 177), ({ wv t662 = v460_k; wv t663 = std_length(rt_get(&F_stmts, v458_b)); rt_lt(t662, t663); })))) {
    RT_AT(4, 178);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t664 = v460_k; wv t665 = rt_get(&F_stmts, v458_b); rt_idx(t664, t665); })), rt_s(&S203)))) {
      RT_AT(4, 178);
      v459_last = rt_bind(({ wv t666 = v460_k; wv t667 = rt_get(&F_stmts, v458_b); rt_idx(t666, t667); }), 1);
    }
    RT_AT(4, 179);
    v460_k = rt_add(v460_k, rt_i(1));
  }
  RT_AT(4, 181);
  if (rt_cond(rt_b(rt_bool(rt_ne(v459_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t668 = rt_def(w5_neverCompletes, "neverCompletes"); wv t669 = rt_get(&F_k, v459_last); rt_chk(w1_contains(t668, t669), "requireValue"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v459_last), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v459_last)), rt_s(&S185)), "&&")), "||")), "&&")))) {
    RT_AT(4, 182);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 184);
  (void)rt_chk(s409_error(up, rt_get(&F_close, v458_b), rt_s(&S411), rt_s(&S412)), "requireValue");
  RT_RET(RT_NULL);
}

static wv s414_newScope(struct fr101 *up, wv v461_parent) {
  rt_enter("newScope");
  v461_parent = rt_bind(v461_parent, 0);
  RT_AT(4, 189);
  wv v462_names = rt_arr(0, NULL, 1);
  RT_RET((RT_AT(4, 190), rt_rec(0, 2, K41, (wv[]){ v461_parent, v462_names })));
}

static wv s415_decl(struct fr101 *up, wv v463_kind, wv v464_name, wv v465_mode, wv v466_namePos, wv v467_visibleAt, wv v468_fnId, wv v469_arity) {
  rt_enter("decl");
  v463_kind = rt_bind(v463_kind, 0);
  v464_name = rt_bind(v464_name, 0);
  v465_mode = rt_bind(v465_mode, 0);
  v466_namePos = rt_bind(v466_namePos, 0);
  v467_visibleAt = rt_bind(v467_visibleAt, 0);
  v468_fnId = rt_bind(v468_fnId, 0);
  v469_arity = rt_bind(v469_arity, 0);
  RT_RET((RT_AT(4, 199), ({ wv t670 = v463_kind; wv t671 = v464_name; wv t672 = rt_eq(v465_mode, rt_s(&S36)); wv t673 = rt_eq(v465_mode, rt_s(&S37)); wv t674 = v466_namePos; wv t675 = v467_visibleAt; wv t676 = v468_fnId; wv t677 = v469_arity; wv t678 = rt_def(up->v405_curMod, "curMod"); rt_rec(0, 10, K42, (wv[]){ t670, t671, t672, t673, t674, t675, t676, t677, t678, RT_NULL }); })));
}

static wv s416_own(struct fr101 *up, wv v470_scope, wv v471_name) {
  rt_enter("own");
  v471_name = rt_bind(v471_name, 0);
  RT_AT(4, 207);
  wv v472_k = rt_i(0);
  RT_AT(4, 208);
  while (rt_cond((RT_AT(4, 208), ({ wv t679 = v472_k; wv t680 = std_length(rt_get(&F_names, v470_scope)); rt_lt(t679, t680); })))) {
    RT_AT(4, 209);
    if (rt_cond(({ wv t683 = rt_get(&F_name, ({ wv t681 = v472_k; wv t682 = rt_get(&F_names, v470_scope); rt_idx(t681, t682); })); wv t684 = v471_name; rt_eq(t683, t684); }))) {
      RT_AT(4, 209);
      RT_RET(({ wv t685 = v472_k; wv t686 = rt_get(&F_names, v470_scope); rt_idx(t685, t686); }));
    }
    RT_AT(4, 210);
    v472_k = rt_add(v472_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 212), RT_NULL));
}

static wv s417_declare(struct fr101 *up, wv v473_scope, wv v474_name, wv v475_d) {
  rt_enter("declare");
  v474_name = rt_bind(v474_name, 0);
  v475_d = rt_bind(v475_d, 0);
  RT_AT(4, 219);
  wv v476_dup = rt_bind(rt_chk(s416_own(up, v473_scope, v474_name), "declare"), 0);
  RT_AT(4, 220);
  if (rt_cond(rt_ne(v476_dup, RT_NULL))) {
    RT_AT(4, 221);
    (void)({ wv t689 = rt_get(&F_namePos, v475_d); wv t690 = ({ wv t687 = rt_text(v474_name); wv t688 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v476_dup)), "declare")); rt_cat(5, (wv[]){ rt_s(&S114), t687, rt_s(&S413), t688, rt_s(&S16) }); }); rt_chk(s409_error(up, t689, t690, RT_NULL), "declare"); });
    RT_AT(4, 222);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 224);
  wv v477_s = rt_get(&F_parent, v473_scope);
  RT_AT(4, 225);
  while (rt_cond((RT_AT(4, 225), rt_ne(v477_s, RT_NULL)))) {
    RT_AT(4, 226);
    wv v478_outer = rt_bind(rt_chk(s416_own(up, v477_s, v474_name), "declare"), 0);
    RT_AT(4, 227);
    if (rt_cond(rt_b(rt_bool(rt_ne(v478_outer, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_kind, v478_outer), rt_s(&S179)), "||") || rt_bool(({ wv t691 = rt_get(&F_namePos, v478_outer); wv t692 = rt_get(&F_namePos, v475_d); rt_lt(t691, t692); }), "||")), "&&")))) {
      RT_AT(4, 228);
      (void)({ wv t696 = rt_get(&F_namePos, v475_d); wv t697 = ({ wv t693 = rt_text(v474_name); wv t694 = rt_text(v474_name); wv t695 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v478_outer)), "declare")); rt_cat(7, (wv[]){ rt_s(&S114), t693, rt_s(&S414), t694, rt_s(&S415), t695, rt_s(&S16) }); }); rt_chk(s409_error(up, t696, t697, rt_s(&S416)), "declare"); });
      break;
    }
    RT_AT(4, 232);
    v477_s = rt_get(&F_parent, v477_s);
  }
  RT_AT(4, 234);
  (void)({ wv t698 = rt_get(&F_names, v473_scope); wv t699 = v475_d; std_push(t698, t699); });
  RT_RET(RT_NULL);
}

static wv s418_lookup(struct fr101 *up, wv v479_scope, wv v480_name) {
  rt_enter("lookup");
  v480_name = rt_bind(v480_name, 0);
  RT_AT(4, 238);
  wv v481_s = v479_scope;
  RT_AT(4, 239);
  while (rt_cond((RT_AT(4, 239), rt_ne(v481_s, RT_NULL)))) {
    RT_AT(4, 240);
    wv v482_d = rt_bind(rt_chk(s416_own(up, v481_s, v480_name), "lookup"), 0);
    RT_AT(4, 241);
    if (rt_cond(rt_ne(v482_d, RT_NULL))) {
      RT_AT(4, 241);
      RT_RET(v482_d);
    }
    RT_AT(4, 242);
    v481_s = rt_get(&F_parent, v481_s);
  }
  RT_RET((RT_AT(4, 244), RT_NULL));
}

static wv s419_resolve(struct fr101 *up, wv v483_name, wv v484_pos, wv v485_scope, wv v486_ctx) {
  rt_enter("resolve");
  v483_name = rt_bind(v483_name, 0);
  v484_pos = rt_bind(v484_pos, 0);
  v485_scope = rt_bind(v485_scope, 0);
  v486_ctx = rt_bind(v486_ctx, 0);
  RT_AT(4, 248);
  wv v487_d = rt_bind(rt_chk(s418_lookup(up, v485_scope, v483_name), "resolve"), 0);
  RT_AT(4, 249);
  if (rt_cond(rt_eq(v487_d, RT_NULL))) {
    RT_AT(4, 250);
    (void)({ wv t700 = v484_pos; wv t701 = rt_cat(3, (wv[]){ rt_s(&S417), rt_text(v483_name), rt_s(&S114) }); rt_chk(s409_error(up, t700, t701, RT_NULL), "resolve"); });
    RT_AT(4, 251);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 255);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v487_d), rt_s(&S179)), "&&") && rt_bool(({ wv t702 = rt_get(&F_fnId, v487_d); wv t703 = rt_get(&F_id, rt_get(&F_func, v486_ctx)); rt_eq(t702, t703); }), "&&")), "&&") && rt_bool(({ wv t704 = v484_pos; wv t705 = rt_get(&F_visibleAt, v487_d); rt_lt(t704, t705); }), "&&")))) {
    RT_AT(4, 256);
    (void)({ wv t708 = v484_pos; wv t709 = ({ wv t706 = rt_text(v483_name); wv t707 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v487_d)), "resolve")); rt_cat(5, (wv[]){ rt_s(&S114), t706, rt_s(&S418), t707, rt_s(&S16) }); }); rt_chk(s409_error(up, t708, t709, RT_NULL), "resolve"); });
  }
  RT_RET((RT_AT(4, 258), v487_d));
}

static wv s420_block(struct fr101 *up, wv v488_b, wv v489_scope, wv v490_ctx) {
  rt_enter("block");
  v489_scope = rt_bind(v489_scope, 0);
  v490_ctx = rt_bind(v490_ctx, 0);
  RT_AT(4, 262);
  wv v491_k = rt_i(0);
  RT_AT(4, 263);
  while (rt_cond((RT_AT(4, 263), ({ wv t710 = v491_k; wv t711 = std_length(rt_get(&F_stmts, v488_b)); rt_lt(t710, t711); })))) {
    RT_AT(4, 264);
    wv v492_s = rt_bind(({ wv t712 = v491_k; wv t713 = rt_get(&F_stmts, v488_b); rt_idx(t712, t713); }), 0);
    RT_AT(4, 265);
    if (rt_cond(rt_eq(rt_get(&F_k, v492_s), rt_s(&S33)))) {
      RT_AT(4, 266);
      wv v493_d = rt_bind(({ wv t714 = rt_get(&F_name, v492_s); wv t715 = rt_get(&F_namePos, v492_s); wv t716 = rt_get(&F_id, rt_get(&F_func, v490_ctx)); wv t717 = std_length(rt_get(&F_params, v492_s)); rt_chk(s415_decl(up, rt_s(&S33), t714, rt_s(&S13), t715, RT_NULL, t716, t717), "block"); }), 0);
      RT_AT(4, 267);
      (void)({ wv t718 = v489_scope; wv t719 = rt_get(&F_name, v492_s); wv t720 = v493_d; rt_chk(s417_declare(up, t718, t719, t720), "block"); });
      RT_AT(4, 268);
      if (rt_cond(({ wv t723 = ({ wv t721 = v489_scope; wv t722 = rt_get(&F_name, v492_s); rt_chk(s416_own(up, t721, t722), "block"); }); wv t724 = v493_d; rt_eq(t723, t724); }))) {
        RT_AT(4, 269);
        (void)({ wv t729 = rt_def(up->v406_functions, "functions"); wv t730 = ({ wv t725 = rt_def(up->v405_curMod, "curMod"); wv t726 = rt_get(&F_name, v492_s); wv t727 = rt_get(&F_namePos, v492_s); wv t728 = rt_get(&F_body, v492_s); rt_rec(0, 4, K43, (wv[]){ t725, t726, t727, t728 }); }); std_push(t729, t730); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v492_s), rt_s(&S47)))) {
      RT_AT(4, 272);
      wv v494_d = rt_bind(({ wv t731 = rt_get(&F_name, v492_s); wv t732 = rt_get(&F_namePos, v492_s); wv t733 = rt_get(&F_id, rt_get(&F_func, v490_ctx)); rt_chk(s415_decl(up, rt_s(&S47), t731, rt_s(&S13), t732, RT_NULL, t733, RT_NULL), "block"); }), 0);
      RT_AT(4, 273);
      (void)({ wv t734 = v489_scope; wv t735 = rt_get(&F_name, v492_s); wv t736 = v494_d; rt_chk(s417_declare(up, t734, t735, t736), "block"); });
      RT_AT(4, 274);
      if (rt_cond(({ wv t739 = ({ wv t737 = v489_scope; wv t738 = rt_get(&F_name, v492_s); rt_chk(s416_own(up, t737, t738), "block"); }); wv t740 = v494_d; rt_eq(t739, t740); }))) {
        RT_AT(4, 274);
        (void)({ wv t744 = rt_def(up->v408_enums, "enums"); wv t745 = ({ wv t741 = rt_def(up->v405_curMod, "curMod"); wv t742 = rt_get(&F_name, v492_s); wv t743 = rt_get(&F_members, v492_s); rt_rec(0, 3, K44, (wv[]){ t741, t742, t743 }); }); std_push(t744, t745); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v492_s), rt_s(&S179)))) {
      RT_AT(4, 276);
      (void)({ wv t751 = v489_scope; wv t752 = rt_get(&F_name, v492_s); wv t753 = ({ wv t746 = rt_get(&F_name, v492_s); wv t747 = rt_chk(w5_modeOf(v492_s), "block"); wv t748 = rt_get(&F_namePos, v492_s); wv t749 = rt_get(&F_end, v492_s); wv t750 = rt_get(&F_id, rt_get(&F_func, v490_ctx)); rt_chk(s415_decl(up, rt_s(&S179), t746, t747, t748, t749, t750, RT_NULL), "block"); }); rt_chk(s417_declare(up, t751, t752, t753), "block"); });
    }
    RT_AT(4, 278);
    v491_k = rt_add(v491_k, rt_i(1));
  }
  RT_AT(4, 280);
  v491_k = rt_i(0);
  RT_AT(4, 281);
  while (rt_cond((RT_AT(4, 281), ({ wv t754 = v491_k; wv t755 = std_length(rt_get(&F_stmts, v488_b)); rt_lt(t754, t755); })))) {
    RT_AT(4, 282);
    (void)({ wv t758 = ({ wv t756 = v491_k; wv t757 = rt_get(&F_stmts, v488_b); rt_idx(t756, t757); }); wv t759 = v489_scope; wv t760 = v490_ctx; rt_chk(s421_stmt(up, t758, t759, t760), "block"); });
    RT_AT(4, 283);
    v491_k = rt_add(v491_k, rt_i(1));
  }
  RT_AT(4, 285);
  if (rt_cond(rt_ne(rt_get(&F_value, v488_b), RT_NULL))) {
    RT_AT(4, 285);
    (void)({ wv t761 = rt_get(&F_value, v488_b); wv t762 = v489_scope; wv t763 = v490_ctx; rt_chk(s439_expr(up, t761, t762, t763), "block"); });
  }
  RT_RET(RT_NULL);
}

static wv s421_stmt(struct fr101 *up, wv v495_s, wv v496_scope, wv v497_ctx) {
  rt_enter("stmt");
  v496_scope = rt_bind(v496_scope, 0);
  v497_ctx = rt_bind(v497_ctx, 0);
  RT_AT(4, 289);
  wv v498_kind = rt_bind(rt_get(&F_k, v495_s), 0);
  RT_AT(4, 290);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v498_kind, rt_s(&S203)), "||") || rt_bool(rt_eq(v498_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v498_kind, rt_s(&S48)), "||")), "||") || rt_bool(rt_eq(v498_kind, rt_s(&S206)), "||")))) {
    RT_AT(4, 290);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 291);
  if (rt_cond(rt_eq(v498_kind, rt_s(&S33)))) {
    RT_AT(4, 292);
    wv v499_func = rt_rec(0, 2, K45, (wv[]){ rt_def(up->v404_nextFn, "nextFn"), rt_b(0) });
    RT_AT(4, 293);
    up->v404_nextFn = rt_add(rt_def(up->v404_nextFn, "nextFn"), rt_i(1));
    RT_AT(4, 294);
    wv v500_ps = rt_bind(rt_chk(s414_newScope(up, v496_scope), "stmt"), 0);
    RT_AT(4, 295);
    wv v501_k = rt_i(0);
    RT_AT(4, 296);
    while (rt_cond((RT_AT(4, 296), ({ wv t764 = v501_k; wv t765 = std_length(rt_get(&F_params, v495_s)); rt_lt(t764, t765); })))) {
      RT_AT(4, 297);
      wv v502_p = rt_bind(({ wv t766 = v501_k; wv t767 = rt_get(&F_params, v495_s); rt_idx(t766, t767); }), 0);
      RT_AT(4, 298);
      (void)({ wv t772 = v500_ps; wv t773 = rt_get(&F_name, v502_p); wv t774 = ({ wv t768 = rt_get(&F_name, v502_p); wv t769 = rt_chk(w5_modeOf(v502_p), "stmt"); wv t770 = rt_get(&F_pos, v502_p); wv t771 = rt_get(&F_id, v499_func); rt_chk(s415_decl(up, rt_s(&S419), t768, t769, t770, RT_NULL, t771, RT_NULL), "stmt"); }); rt_chk(s417_declare(up, t772, t773, t774), "stmt"); });
      RT_AT(4, 299);
      v501_k = rt_add(v501_k, rt_i(1));
    }
    RT_AT(4, 301);
    (void)({ wv t777 = rt_get(&F_body, v495_s); wv t778 = rt_chk(s414_newScope(up, v500_ps), "stmt"); wv t779 = ({ wv t775 = v499_func; wv t776 = rt_get(&F_guards, v497_ctx); rt_rec(0, 3, K46, (wv[]){ t775, rt_i(0), t776 }); }); rt_chk(s420_block(up, t777, t778, t779), "stmt"); });
    RT_AT(4, 302);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 304);
  if (rt_cond(rt_eq(v498_kind, rt_s(&S34)))) {
    RT_AT(4, 305);
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v497_ctx)))) {
      RT_AT(4, 305);
      (void)rt_chk(s409_error(up, rt_get(&F_pos, v495_s), rt_s(&S420), RT_NULL), "stmt");
    }
    RT_AT(4, 306);
    if (rt_cond(rt_ne(rt_get(&F_e, v495_s), RT_NULL))) {
      RT_AT(4, 306);
      (void)({ wv t780 = rt_get(&F_e, v495_s); wv t781 = v496_scope; wv t782 = v497_ctx; rt_chk(s439_expr(up, t780, t781, t782), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S58)))) {
    RT_AT(4, 308);
    (void)({ wv t783 = rt_get(&F_e, v495_s); wv t784 = v496_scope; wv t785 = v497_ctx; rt_chk(s439_expr(up, t783, t784, t785), "stmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v498_kind, rt_s(&S43)), "||") || rt_bool(rt_eq(v498_kind, rt_s(&S44)), "||")))) {
    RT_AT(4, 310);
    if (rt_cond(rt_eq(rt_get(&F_loops, v497_ctx), rt_i(0)))) {
      RT_AT(4, 310);
      (void)({ wv t786 = rt_get(&F_pos, v495_s); wv t787 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text((rt_cond(rt_eq(v498_kind, rt_s(&S43))) ? rt_s(&S43) : rt_s(&S44))), rt_s(&S421) }); rt_chk(s409_error(up, t786, t787, RT_NULL), "stmt"); });
    }
    RT_AT(4, 311);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S41)))) {
    RT_AT(4, 313);
    (void)({ wv t788 = rt_get(&F_cond, v495_s); wv t789 = v496_scope; wv t790 = v497_ctx; rt_chk(s439_expr(up, t788, t789, t790), "stmt"); });
    RT_AT(4, 314);
    (void)({ wv t794 = rt_get(&F_body, v495_s); wv t795 = rt_chk(s414_newScope(up, v496_scope), "stmt"); wv t796 = ({ wv t791 = rt_get(&F_func, v497_ctx); wv t792 = rt_add(rt_get(&F_loops, v497_ctx), rt_i(1)); wv t793 = rt_get(&F_guards, v497_ctx); rt_rec(0, 3, K46, (wv[]){ t791, t792, t793 }); }); rt_chk(s420_block(up, t794, t795, t796), "stmt"); });
    RT_AT(4, 315);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S42)))) {
    RT_AT(4, 317);
    (void)rt_chk(s422_forLoop(up, v495_s, v496_scope, v497_ctx), "stmt");
    RT_AT(4, 318);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S56)))) {
    RT_AT(4, 320);
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v497_ctx)))) {
      RT_AT(4, 320);
      (void)rt_chk(s409_error(up, rt_get(&F_pos, v495_s), rt_s(&S422), RT_NULL), "stmt");
    }
    RT_AT(4, 321);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S55)))) {
    RT_AT(4, 323);
    if (rt_cond(rt_ne(rt_get(&F_e, v495_s), RT_NULL))) {
      RT_AT(4, 323);
      (void)({ wv t797 = rt_get(&F_e, v495_s); wv t798 = v496_scope; wv t799 = v497_ctx; rt_chk(s439_expr(up, t797, t798, t799), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S178)))) {
    RT_AT(4, 325);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v495_s)), rt_s(&S185)))) {
      RT_AT(4, 326);
      if (rt_cond(rt_get(&F_top, rt_get(&F_func, v497_ctx)))) {
        RT_AT(4, 326);
        (void)rt_chk(s409_error(up, rt_get(&F_pos, rt_get(&F_e, v495_s)), rt_s(&S422), RT_NULL), "stmt");
      }
      RT_AT(4, 327);
      (void)({ wv t800 = rt_get(&F_arg, rt_get(&F_e, v495_s)); wv t801 = v496_scope; wv t802 = v497_ctx; rt_chk(s439_expr(up, t800, t801, t802), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v495_s)), rt_s(&S186)))) {
      RT_AT(4, 329);
      (void)({ wv t803 = rt_get(&F_e, v495_s); wv t804 = v496_scope; wv t805 = v497_ctx; rt_chk(s435_chain(up, t803, t804, t805, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v495_s)), rt_s(&S45)))) {
      RT_AT(4, 331);
      (void)({ wv t806 = rt_get(&F_e, v495_s); wv t807 = v496_scope; wv t808 = v497_ctx; rt_chk(s426_checkMatch(up, t806, t807, t808, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v495_s)), rt_s(&S184)))) {
      RT_AT(4, 334);
      wv v503_k = rt_i(0);
      RT_AT(4, 335);
      while (rt_cond((RT_AT(4, 335), ({ wv t809 = v503_k; wv t810 = std_length(rt_get(&F_args, rt_get(&F_e, v495_s))); rt_lt(t809, t810); })))) {
        RT_AT(4, 336);
        (void)({ wv t813 = ({ wv t811 = v503_k; wv t812 = rt_get(&F_args, rt_get(&F_e, v495_s)); rt_idx(t811, t812); }); wv t814 = v496_scope; wv t815 = v497_ctx; rt_chk(s439_expr(up, t813, t814, t815), "stmt"); });
        RT_AT(4, 337);
        v503_k = rt_add(v503_k, rt_i(1));
      }
      RT_AT(4, 339);
      (void)({ wv t816 = rt_get(&F_e, v495_s); wv t817 = v496_scope; rt_chk(s440_callee(up, t816, t817, rt_b(0)), "stmt"); });
    } else {
      RT_AT(4, 341);
      (void)({ wv t818 = rt_get(&F_e, v495_s); wv t819 = v496_scope; wv t820 = v497_ctx; rt_chk(s439_expr(up, t818, t819, t820), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S179)))) {
    RT_AT(4, 344);
    (void)({ wv t821 = rt_get(&F_e, v495_s); wv t822 = v496_scope; wv t823 = v497_ctx; rt_chk(s439_expr(up, t821, t822, t823), "stmt"); });
  } else if (rt_cond(rt_eq(v498_kind, rt_s(&S38)))) {
    RT_AT(4, 346);
    (void)({ wv t824 = rt_get(&F_e, v495_s); wv t825 = v496_scope; wv t826 = v497_ctx; rt_chk(s439_expr(up, t824, t825, t826), "stmt"); });
    RT_AT(4, 347);
    (void)({ wv t827 = rt_get(&F_target, v495_s); wv t828 = v496_scope; wv t829 = v497_ctx; rt_chk(s436_lvalue(up, t827, t828, t829), "stmt"); });
  }
  RT_AT(4, 349);
  if (rt_cond(rt_ne(rt_get(&F_handler, v495_s), RT_NULL))) {
    RT_AT(4, 349);
    (void)rt_chk(s425_onfailHandler(up, v495_s, v496_scope, v497_ctx), "stmt");
  }
  RT_RET(RT_NULL);
}

static wv s422_forLoop(struct fr101 *up, wv v504_s, wv v505_scope, wv v506_ctx) {
  rt_enter("forLoop");
  v504_s = rt_bind(v504_s, 0);
  v505_scope = rt_bind(v505_scope, 0);
  v506_ctx = rt_bind(v506_ctx, 0);
  RT_AT(4, 356);
  wv v507_guards = rt_arr(0, NULL, 1);
  RT_AT(4, 357);
  wv v508_k = rt_i(0);
  RT_AT(4, 358);
  while (rt_cond((RT_AT(4, 358), ({ wv t830 = v508_k; wv t831 = std_length(rt_get(&F_guards, v506_ctx)); rt_lt(t830, t831); })))) {
    RT_AT(4, 359);
    (void)({ wv t834 = v507_guards; wv t835 = ({ wv t832 = v508_k; wv t833 = rt_get(&F_guards, v506_ctx); rt_idx(t832, t833); }); std_push(t834, t835); });
    RT_AT(4, 360);
    v508_k = rt_add(v508_k, rt_i(1));
  }
  RT_AT(4, 362);
  if (rt_cond(rt_eq(rt_get(&F_coll, v504_s), RT_NULL))) {
    RT_AT(4, 363);
    (void)({ wv t836 = rt_get(&F_from, v504_s); wv t837 = v505_scope; wv t838 = v506_ctx; rt_chk(s439_expr(up, t836, t837, t838), "forLoop"); });
    RT_AT(4, 364);
    (void)({ wv t839 = rt_get(&F_to, v504_s); wv t840 = v505_scope; wv t841 = v506_ctx; rt_chk(s439_expr(up, t839, t840, t841), "forLoop"); });
    RT_AT(4, 365);
    if (rt_cond(rt_ne(rt_get(&F_step, v504_s), RT_NULL))) {
      RT_AT(4, 365);
      (void)({ wv t842 = rt_get(&F_step, v504_s); wv t843 = v505_scope; wv t844 = v506_ctx; rt_chk(s439_expr(up, t842, t843, t844), "forLoop"); });
    }
  } else if (rt_cond(rt_eq(({ wv t845 = rt_get(&F_coll, v504_s); wv t846 = v505_scope; rt_chk(s429_enumNamed(up, t845, t846), "forLoop"); }), RT_NULL))) {
    RT_AT(4, 367);
    (void)({ wv t847 = rt_get(&F_coll, v504_s); wv t848 = v505_scope; wv t849 = v506_ctx; rt_chk(s439_expr(up, t847, t848, t849), "forLoop"); });
    RT_AT(4, 368);
    wv v510_path = rt_bind(rt_chk(s423_pathOf(up, rt_get(&F_coll, v504_s)), "forLoop"), 0);
    RT_AT(4, 369);
    if (rt_cond(rt_ne(v510_path, RT_NULL))) {
      RT_AT(4, 369);
      (void)std_push(v507_guards, v510_path);
    }
  }
  RT_AT(4, 371);
  wv v509_ls = rt_bind(rt_chk(s414_newScope(up, v505_scope), "forLoop"), 0);
  RT_AT(4, 372);
  v508_k = rt_i(0);
  RT_AT(4, 373);
  while (rt_cond((RT_AT(4, 373), ({ wv t850 = v508_k; wv t851 = std_length(rt_get(&F_names, v504_s)); rt_lt(t850, t851); })))) {
    RT_AT(4, 374);
    wv v511_n = rt_bind(({ wv t852 = v508_k; wv t853 = rt_get(&F_names, v504_s); rt_idx(t852, t853); }), 0);
    RT_AT(4, 375);
    (void)({ wv t857 = v509_ls; wv t858 = rt_get(&F_name, v511_n); wv t859 = ({ wv t854 = rt_get(&F_name, v511_n); wv t855 = rt_get(&F_pos, v511_n); wv t856 = rt_get(&F_id, rt_get(&F_func, v506_ctx)); rt_chk(s415_decl(up, rt_s(&S423), t854, rt_s(&S13), t855, RT_NULL, t856, RT_NULL), "forLoop"); }); rt_chk(s417_declare(up, t857, t858, t859), "forLoop"); });
    RT_AT(4, 376);
    v508_k = rt_add(v508_k, rt_i(1));
  }
  RT_AT(4, 378);
  (void)({ wv t863 = rt_get(&F_body, v504_s); wv t864 = rt_chk(s414_newScope(up, v509_ls), "forLoop"); wv t865 = ({ wv t860 = rt_get(&F_func, v506_ctx); wv t861 = rt_add(rt_get(&F_loops, v506_ctx), rt_i(1)); wv t862 = v507_guards; rt_rec(0, 3, K46, (wv[]){ t860, t861, t862 }); }); rt_chk(s420_block(up, t863, t864, t865), "forLoop"); });
  RT_RET(RT_NULL);
}

static wv s423_pathOf(struct fr101 *up, wv v512_x) {
  rt_enter("pathOf");
  v512_x = rt_bind(v512_x, 0);
  RT_AT(4, 383);
  if (rt_cond(rt_eq(rt_get(&F_k, v512_x), rt_s(&S214)))) {
    RT_AT(4, 383);
    RT_RET(rt_arr(1, (wv[]){ rt_get(&F_name, v512_x) }, 0));
  }
  RT_AT(4, 384);
  if (rt_cond(rt_ne(rt_get(&F_k, v512_x), rt_s(&S192)))) {
    RT_AT(4, 384);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 385);
  wv v513_p = rt_bind(rt_chk(s423_pathOf(up, rt_get(&F_rec, v512_x)), "pathOf"), 0);
  RT_AT(4, 386);
  if (rt_cond(rt_eq(v513_p, RT_NULL))) {
    RT_AT(4, 386);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 387);
  wv v514_out = rt_bind(v513_p, 1);
  RT_AT(4, 388);
  (void)({ wv t866 = v514_out; wv t867 = rt_get(&F_field, v512_x); std_push(t866, t867); });
  RT_RET((RT_AT(4, 389), v514_out));
}

static wv s424_guarded(struct fr101 *up, wv v515_t, wv v516_ctx) {
  rt_enter("guarded");
  v515_t = rt_bind(v515_t, 0);
  v516_ctx = rt_bind(v516_ctx, 0);
  RT_AT(4, 395);
  wv v517_p = rt_bind(rt_chk(s423_pathOf(up, v515_t), "guarded"), 0);
  RT_AT(4, 396);
  if (rt_cond(rt_eq(v517_p, RT_NULL))) {
    RT_AT(4, 396);
    RT_RET(rt_b(0));
  }
  RT_AT(4, 397);
  wv v518_g = rt_i(0);
  RT_AT(4, 398);
  while (rt_cond((RT_AT(4, 398), ({ wv t868 = v518_g; wv t869 = std_length(rt_get(&F_guards, v516_ctx)); rt_lt(t868, t869); })))) {
    RT_AT(4, 399);
    wv v519_gp = rt_bind(({ wv t870 = v518_g; wv t871 = rt_get(&F_guards, v516_ctx); rt_idx(t870, t871); }), 0);
    RT_AT(4, 400);
    v518_g = rt_add(v518_g, rt_i(1));
    RT_AT(4, 401);
    wv v520_prefix = ({ wv t872 = std_length(v517_p); wv t873 = std_length(v519_gp); rt_le(t872, t873); });
    RT_AT(4, 402);
    wv v521_k = rt_i(0);
    RT_AT(4, 403);
    while (rt_cond((RT_AT(4, 403), rt_b(rt_bool(v520_prefix, "&&") && rt_bool(({ wv t874 = v521_k; wv t875 = std_length(v517_p); rt_lt(t874, t875); }), "&&"))))) {
      RT_AT(4, 404);
      if (rt_cond(({ wv t876 = rt_idx(v521_k, v517_p); wv t877 = rt_idx(v521_k, v519_gp); rt_ne(t876, t877); }))) {
        RT_AT(4, 404);
        v520_prefix = rt_b(0);
      }
      RT_AT(4, 405);
      v521_k = rt_add(v521_k, rt_i(1));
    }
    RT_AT(4, 407);
    if (rt_cond(v520_prefix)) {
      RT_AT(4, 408);
      wv v522_name = rt_bind(std_join(v517_p, rt_s(&S130)), 0);
      RT_AT(4, 409);
      wv v523_over = rt_bind((rt_cond(({ wv t878 = std_length(v517_p); wv t879 = std_length(v519_gp); rt_eq(t878, t879); })) ? rt_s(&S424) : rt_cat(3, (wv[]){ rt_s(&S114), rt_text(std_join(v519_gp, rt_s(&S130))), rt_s(&S114) })), 0);
      RT_AT(4, 410);
      (void)({ wv t882 = rt_get(&F_pos, v515_t); wv t883 = ({ wv t880 = rt_text(v522_name); wv t881 = rt_text(v523_over); rt_cat(4, (wv[]){ rt_s(&S425), t880, rt_s(&S426), t881 }); }); wv t884 = rt_cat(3, (wv[]){ rt_s(&S427), rt_text(v522_name), rt_s(&S428) }); rt_chk(s409_error(up, t882, t883, t884), "guarded"); });
      RT_AT(4, 412);
      RT_RET(rt_b(1));
    }
  }
  RT_RET((RT_AT(4, 415), rt_b(0)));
}

static wv s425_onfailHandler(struct fr101 *up, wv v524_s, wv v525_scope, wv v526_ctx) {
  rt_enter("onfailHandler");
  v524_s = rt_bind(v524_s, 0);
  v525_scope = rt_bind(v525_scope, 0);
  v526_ctx = rt_bind(v526_ctx, 0);
  RT_AT(4, 420);
  wv v527_h = rt_bind(rt_get(&F_handler, v524_s), 0);
  RT_AT(4, 421);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_e, v524_s), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v524_s)), rt_s(&S184)), "||")))) {
    RT_AT(4, 422);
    (void)rt_chk(s409_error(up, rt_get(&F_pos, v527_h), rt_s(&S429), rt_s(&S430)), "onfailHandler");
  }
  RT_AT(4, 425);
  wv v528_hs = rt_bind(rt_chk(s414_newScope(up, v525_scope), "onfailHandler"), 0);
  RT_AT(4, 426);
  if (rt_cond(rt_ne(rt_get(&F_param, v527_h), RT_NULL))) {
    RT_AT(4, 427);
    (void)({ wv t888 = v528_hs; wv t889 = rt_get(&F_param, v527_h); wv t890 = ({ wv t885 = rt_get(&F_param, v527_h); wv t886 = rt_get(&F_paramPos, v527_h); wv t887 = rt_get(&F_id, rt_get(&F_func, v526_ctx)); rt_chk(s415_decl(up, rt_s(&S57), t885, rt_s(&S13), t886, RT_NULL, t887, RT_NULL), "onfailHandler"); }); rt_chk(s417_declare(up, t888, t889, t890), "onfailHandler"); });
  }
  RT_AT(4, 429);
  (void)({ wv t891 = rt_get(&F_body, v527_h); wv t892 = rt_chk(s414_newScope(up, v528_hs), "onfailHandler"); wv t893 = v526_ctx; rt_chk(s420_block(up, t891, t892, t893), "onfailHandler"); });
  RT_AT(4, 430);
  if (rt_cond(({ wv t894 = rt_def(w5_onfailValueUsed, "onfailValueUsed"); wv t895 = rt_get(&F_k, v524_s); rt_chk(w1_contains(t894, t895), "onfailHandler"); }))) {
    RT_AT(4, 430);
    (void)rt_chk(s413_requireValue(up, rt_get(&F_body, v527_h)), "onfailHandler");
  }
  RT_RET(RT_NULL);
}

static wv s426_checkMatch(struct fr101 *up, wv v529_e, wv v530_scope, wv v531_ctx, wv v532_stmtLevel) {
  rt_enter("checkMatch");
  v529_e = rt_bind(v529_e, 0);
  v530_scope = rt_bind(v530_scope, 0);
  v531_ctx = rt_bind(v531_ctx, 0);
  v532_stmtLevel = rt_bind(v532_stmtLevel, 0);
  RT_AT(4, 437);
  (void)({ wv t896 = rt_get(&F_subject, v529_e); wv t897 = v530_scope; wv t898 = v531_ctx; rt_chk(s439_expr(up, t896, t897, t898), "checkMatch"); });
  RT_AT(4, 439);
  wv v533_seen = rt_arr(0, NULL, 1);
  RT_AT(4, 442);
  wv v534_sort = RT_NULL;
  RT_AT(4, 444);
  wv v535_sortEnum = RT_NULL;
  RT_AT(4, 445);
  wv v536_a = rt_i(0);
  RT_AT(4, 446);
  while (rt_cond((RT_AT(4, 446), ({ wv t899 = v536_a; wv t900 = std_length(rt_get(&F_cases, v529_e)); rt_lt(t899, t900); })))) {
    RT_AT(4, 447);
    wv v537_al = rt_bind(({ wv t901 = v536_a; wv t902 = rt_get(&F_cases, v529_e); rt_idx(t901, t902); }), 0);
    RT_AT(4, 448);
    wv v538_j = rt_i(0);
    RT_AT(4, 449);
    while (rt_cond((RT_AT(4, 449), ({ wv t903 = v538_j; wv t904 = std_length(rt_get(&F_values, v537_al)); rt_lt(t903, t904); })))) {
      RT_AT(4, 450);
      wv v539_v = rt_bind(({ wv t905 = v538_j; wv t906 = rt_get(&F_values, v537_al); rt_idx(t905, t906); }), 0);
      RT_AT(4, 451);
      wv v540_vs = rt_s(&S13);
      RT_AT(4, 452);
      if (rt_cond(rt_eq(rt_get(&F_k, v539_v), rt_s(&S192)))) {
        RT_AT(4, 453);
        wv v544_d = rt_bind(rt_chk(s433_caseEnum(up, v539_v, v530_scope), "checkMatch"), 0);
        RT_AT(4, 454);
        v540_vs = rt_bind((rt_cond(rt_ne(v544_d, RT_NULL)) ? rt_chk(w5_enumText(rt_get(&F_rec, v539_v)), "checkMatch") : RT_NULL), 1);
        RT_AT(4, 455);
        if (rt_cond(rt_eq(v534_sort, RT_NULL))) {
          RT_AT(4, 455);
          v535_sortEnum = rt_bind(v544_d, 1);
        }
      }
      RT_AT(4, 457);
      if (rt_cond(rt_eq(v534_sort, RT_NULL))) {
        RT_AT(4, 458);
        v534_sort = rt_bind(v540_vs, 1);
      } else if (rt_cond(rt_b(rt_bool(rt_ne(v540_vs, RT_NULL), "&&") && rt_bool(rt_ne(v540_vs, v534_sort), "&&")))) {
        RT_AT(4, 460);
        wv v545_message = rt_bind((rt_cond(rt_b(rt_bool(rt_eq(v534_sort, rt_s(&S13)), "||") || rt_bool(rt_eq(v540_vs, rt_s(&S13)), "||"))) ? rt_s(&S431) : ({ wv t907 = rt_text(v534_sort); wv t908 = rt_text(v540_vs); rt_cat(5, (wv[]){ rt_s(&S432), t907, rt_s(&S433), t908, rt_s(&S114) }); })), 0);
        RT_AT(4, 462);
        (void)({ wv t909 = rt_get(&F_pos, v539_v); wv t910 = v545_message; rt_chk(s409_error(up, t909, t910, RT_NULL), "checkMatch"); });
      }
      RT_AT(4, 464);
      wv v541_key = rt_bind(rt_chk(w5_caseKey(v539_v), "checkMatch"), 0);
      RT_AT(4, 465);
      wv v542_first = RT_NULL;
      RT_AT(4, 466);
      wv v543_q = rt_i(0);
      RT_AT(4, 467);
      while (rt_cond((RT_AT(4, 467), ({ wv t911 = v543_q; wv t912 = std_length(v533_seen); rt_lt(t911, t912); })))) {
        RT_AT(4, 468);
        if (rt_cond(({ wv t913 = rt_get(&F_key, rt_idx(v543_q, v533_seen)); wv t914 = v541_key; rt_eq(t913, t914); }))) {
          RT_AT(4, 468);
          v542_first = rt_bind(rt_get(&F_pos, rt_idx(v543_q, v533_seen)), 1);
        }
        RT_AT(4, 469);
        v543_q = rt_add(v543_q, rt_i(1));
      }
      RT_AT(4, 471);
      if (rt_cond(rt_ne(v542_first, RT_NULL))) {
        RT_AT(4, 472);
        (void)({ wv t917 = rt_get(&F_pos, v539_v); wv t918 = ({ wv t915 = rt_text(rt_chk(w5_caseText(v539_v), "checkMatch")); wv t916 = rt_text(rt_chk(w5_lineOf(v542_first), "checkMatch")); rt_cat(5, (wv[]){ rt_s(&S434), t915, rt_s(&S435), t916, rt_s(&S16) }); }); rt_chk(s409_error(up, t917, t918, RT_NULL), "checkMatch"); });
      } else {
        RT_AT(4, 474);
        (void)({ wv t921 = v533_seen; wv t922 = ({ wv t919 = v541_key; wv t920 = rt_get(&F_pos, v539_v); rt_rec(0, 2, K47, (wv[]){ t919, t920 }); }); std_push(t921, t922); });
      }
      RT_AT(4, 476);
      v538_j = rt_add(v538_j, rt_i(1));
    }
    RT_AT(4, 478);
    (void)({ wv t923 = rt_get(&F_body, v537_al); wv t924 = rt_chk(s414_newScope(up, v530_scope), "checkMatch"); wv t925 = v531_ctx; rt_chk(s420_block(up, t923, t924, t925), "checkMatch"); });
    RT_AT(4, 479);
    if (rt_cond(rt_not(v532_stmtLevel))) {
      RT_AT(4, 479);
      (void)rt_chk(s413_requireValue(up, rt_get(&F_body, v537_al)), "checkMatch");
    }
    RT_AT(4, 480);
    v536_a = rt_add(v536_a, rt_i(1));
  }
  RT_AT(4, 482);
  if (rt_cond(rt_ne(rt_get(&F_els, v529_e), RT_NULL))) {
    RT_AT(4, 483);
    (void)({ wv t926 = rt_get(&F_els, v529_e); wv t927 = rt_chk(s414_newScope(up, v530_scope), "checkMatch"); wv t928 = v531_ctx; rt_chk(s420_block(up, t926, t927, t928), "checkMatch"); });
    RT_AT(4, 484);
    if (rt_cond(rt_not(v532_stmtLevel))) {
      RT_AT(4, 484);
      (void)rt_chk(s413_requireValue(up, rt_get(&F_els, v529_e)), "checkMatch");
    }
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v534_sort, RT_NULL), "&&") && rt_bool(rt_ne(v534_sort, rt_s(&S13)), "&&")))) {
    RT_AT(4, 487);
    wv v546_ms = rt_bind(rt_chk(s411_enumMembers(up, v535_sortEnum), "checkMatch"), 0);
    RT_AT(4, 488);
    wv v547_missing = rt_arr(0, NULL, 1);
    RT_AT(4, 489);
    wv v548_m = rt_i(0);
    RT_AT(4, 490);
    while (rt_cond((RT_AT(4, 490), ({ wv t929 = v548_m; wv t930 = std_length(v546_ms); rt_lt(t929, t930); })))) {
      RT_AT(4, 491);
      wv v549_key = ({ wv t931 = rt_text(rt_get(&F_name, rt_idx(v548_m, v546_ms))); wv t932 = rt_text(v534_sort); rt_cat(4, (wv[]){ rt_s(&S410), t931, rt_s(&S130), t932 }); });
      RT_AT(4, 492);
      wv v550_handled = rt_b(0);
      RT_AT(4, 493);
      wv v551_q = rt_i(0);
      RT_AT(4, 494);
      while (rt_cond((RT_AT(4, 494), ({ wv t933 = v551_q; wv t934 = std_length(v533_seen); rt_lt(t933, t934); })))) {
        RT_AT(4, 495);
        if (rt_cond(({ wv t935 = rt_get(&F_key, rt_idx(v551_q, v533_seen)); wv t936 = v549_key; rt_eq(t935, t936); }))) {
          RT_AT(4, 495);
          v550_handled = rt_b(1);
        }
        RT_AT(4, 496);
        v551_q = rt_add(v551_q, rt_i(1));
      }
      RT_AT(4, 498);
      if (rt_cond(rt_not(v550_handled))) {
        RT_AT(4, 498);
        (void)({ wv t939 = v547_missing; wv t940 = ({ wv t937 = rt_text(v534_sort); wv t938 = rt_text(rt_get(&F_name, rt_idx(v548_m, v546_ms))); rt_cat(5, (wv[]){ rt_s(&S114), t937, rt_s(&S130), t938, rt_s(&S114) }); }); std_push(t939, t940); });
      }
      RT_AT(4, 499);
      v548_m = rt_add(v548_m, rt_i(1));
    }
    RT_AT(4, 501);
    if (rt_cond(rt_gt(std_length(v547_missing), rt_i(0)))) {
      RT_AT(4, 502);
      (void)({ wv t941 = rt_get(&F_pos, v529_e); wv t942 = rt_cat(2, (wv[]){ rt_s(&S436), rt_text(std_join(v547_missing, rt_s(&S437))) }); rt_chk(s409_error(up, t941, t942, rt_s(&S438)), "checkMatch"); });
    }
  } else if (rt_cond(rt_not(v532_stmtLevel))) {
    RT_AT(4, 506);
    (void)rt_chk(s409_error(up, rt_get(&F_pos, v529_e), rt_s(&S439), RT_NULL), "checkMatch");
  }
  RT_RET(RT_NULL);
}

static wv s427_moduleNamed(struct fr101 *up, wv v552_x, wv v553_scope) {
  rt_enter("moduleNamed");
  v553_scope = rt_bind(v553_scope, 0);
  RT_AT(4, 512);
  if (rt_cond(rt_ne(rt_get(&F_k, v552_x), rt_s(&S214)))) {
    RT_AT(4, 512);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 513);
  wv v554_d = rt_bind(({ wv t943 = v553_scope; wv t944 = rt_get(&F_name, v552_x); rt_chk(s418_lookup(up, t943, t944), "moduleNamed"); }), 0);
  RT_RET((RT_AT(4, 514), (rt_cond(rt_b(rt_bool(rt_ne(v554_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v554_d), rt_s(&S440)), "&&"))) ? v554_d : RT_NULL)));
}

static wv s428_exported(struct fr101 *up, wv v555_lib, wv v556_name) {
  rt_enter("exported");
  v555_lib = rt_bind(v555_lib, 0);
  v556_name = rt_bind(v556_name, 0);
  RT_AT(4, 519);
  wv v557_xs = rt_bind(rt_get(&F_exports, rt_idx(v555_lib, up->v401_modules)), 0);
  RT_AT(4, 520);
  wv v558_k = rt_i(0);
  RT_AT(4, 521);
  while (rt_cond((RT_AT(4, 521), ({ wv t945 = v558_k; wv t946 = std_length(v557_xs); rt_lt(t945, t946); })))) {
    RT_AT(4, 522);
    if (rt_cond(({ wv t947 = rt_get(&F_name, rt_idx(v558_k, v557_xs)); wv t948 = v556_name; rt_eq(t947, t948); }))) {
      RT_AT(4, 522);
      RT_RET(rt_idx(v558_k, v557_xs));
    }
    RT_AT(4, 523);
    v558_k = rt_add(v558_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 525), RT_NULL));
}

static wv s429_enumNamed(struct fr101 *up, wv v559_x, wv v560_scope) {
  rt_enter("enumNamed");
  v560_scope = rt_bind(v560_scope, 0);
  RT_AT(4, 530);
  if (rt_cond(rt_eq(rt_get(&F_k, v559_x), rt_s(&S214)))) {
    RT_AT(4, 531);
    wv v563_d = rt_bind(({ wv t949 = v560_scope; wv t950 = rt_get(&F_name, v559_x); rt_chk(s418_lookup(up, t949, t950), "enumNamed"); }), 0);
    RT_AT(4, 532);
    RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v563_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v563_d), rt_s(&S47)), "&&"))) ? v563_d : RT_NULL));
  }
  RT_AT(4, 534);
  if (rt_cond(rt_ne(rt_get(&F_k, v559_x), rt_s(&S192)))) {
    RT_AT(4, 534);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 535);
  wv v561_m = rt_bind(({ wv t951 = rt_get(&F_rec, v559_x); wv t952 = v560_scope; rt_chk(s427_moduleNamed(up, t951, t952), "enumNamed"); }), 0);
  RT_AT(4, 536);
  if (rt_cond(rt_b(rt_bool(rt_eq(v561_m, RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_lib, v561_m), RT_NULL), "||")))) {
    RT_AT(4, 536);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 537);
  wv v562_d = rt_bind(({ wv t953 = rt_get(&F_lib, v561_m); wv t954 = rt_get(&F_field, v559_x); rt_chk(s428_exported(up, t953, t954), "enumNamed"); }), 0);
  RT_RET((RT_AT(4, 538), (rt_cond(rt_b(rt_bool(rt_ne(v562_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v562_d), rt_s(&S47)), "&&"))) ? v562_d : RT_NULL)));
}

static wv s430_exportOf(struct fr101 *up, wv v564_e, wv v565_m) {
  rt_enter("exportOf");
  v564_e = rt_bind(v564_e, 0);
  v565_m = rt_bind(v565_m, 0);
  RT_AT(4, 544);
  if (rt_cond(rt_eq(rt_get(&F_lib, v565_m), RT_NULL))) {
    RT_AT(4, 544);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 545);
  wv v566_d = rt_bind(({ wv t955 = rt_get(&F_lib, v565_m); wv t956 = rt_get(&F_field, v564_e); rt_chk(s428_exported(up, t955, t956), "exportOf"); }), 0);
  RT_AT(4, 546);
  if (rt_cond(rt_eq(v566_d, RT_NULL))) {
    RT_AT(4, 546);
    (void)({ wv t961 = rt_get(&F_pos, v564_e); wv t962 = ({ wv t959 = rt_text(rt_get(&F_path, ({ wv t957 = rt_get(&F_lib, v565_m); wv t958 = up->v401_modules; rt_idx(t957, t958); }))); wv t960 = rt_text(rt_get(&F_field, v564_e)); rt_cat(5, (wv[]){ rt_s(&S114), t959, rt_s(&S441), t960, rt_s(&S114) }); }); rt_chk(s409_error(up, t961, t962, RT_NULL), "exportOf"); });
  }
  RT_RET((RT_AT(4, 547), v566_d));
}

static wv s431_unknownModule(struct fr101 *up, wv v567_x, wv v568_scope) {
  rt_enter("unknownModule");
  v567_x = rt_bind(v567_x, 0);
  v568_scope = rt_bind(v568_scope, 0);
  RT_AT(4, 551);
  wv v569_message = rt_bind((rt_cond(rt_ne(({ wv t963 = v568_scope; wv t964 = rt_get(&F_name, v567_x); rt_chk(s418_lookup(up, t963, t964), "unknownModule"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v567_x)), rt_s(&S442) }) : rt_cat(3, (wv[]){ rt_s(&S443), rt_text(rt_get(&F_name, v567_x)), rt_s(&S114) })), 0);
  RT_AT(4, 552);
  (void)({ wv t965 = rt_get(&F_pos, v567_x); wv t966 = v569_message; rt_chk(s409_error(up, t965, t966, RT_NULL), "unknownModule"); });
  RT_RET(RT_NULL);
}

static wv s432_qualified(struct fr101 *up, wv v570_e, wv v571_m) {
  rt_enter("qualified");
  v570_e = rt_bind(v570_e, 0);
  v571_m = rt_bind(v571_m, 0);
  RT_AT(4, 557);
  wv v572_d = rt_bind(rt_chk(s430_exportOf(up, v570_e, v571_m), "qualified"), 0);
  RT_AT(4, 558);
  wv v573_label = ({ wv t967 = rt_text(rt_get(&F_name, rt_get(&F_rec, v570_e))); wv t968 = rt_text(rt_get(&F_field, v570_e)); rt_cat(3, (wv[]){ t967, rt_s(&S130), t968 }); });
  RT_AT(4, 559);
  if (rt_cond(rt_b(rt_bool(rt_ne(v572_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v572_d), rt_s(&S33)), "&&")))) {
    RT_AT(4, 560);
    (void)({ wv t971 = rt_get(&F_pos, v570_e); wv t972 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v573_label), rt_s(&S444) }); wv t973 = ({ wv t969 = rt_text(v573_label); wv t970 = rt_text(v573_label); rt_cat(5, (wv[]){ rt_s(&S445), t969, rt_s(&S446), t970, rt_s(&S447) }); }); rt_chk(s409_error(up, t971, t972, t973), "qualified"); });
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v572_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v572_d), rt_s(&S47)), "&&")))) {
    RT_AT(4, 562);
    (void)({ wv t974 = rt_get(&F_pos, v570_e); wv t975 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v573_label), rt_s(&S448) }); wv t976 = rt_cat(3, (wv[]){ rt_s(&S449), rt_text(v573_label), rt_s(&S450) }); rt_chk(s409_error(up, t974, t975, t976), "qualified"); });
  }
  RT_RET(RT_NULL);
}

static wv s433_caseEnum(struct fr101 *up, wv v574_v, wv v575_scope) {
  rt_enter("caseEnum");
  v574_v = rt_bind(v574_v, 0);
  v575_scope = rt_bind(v575_scope, 0);
  RT_AT(4, 569);
  wv v576_x = rt_bind(rt_get(&F_rec, v574_v), 0);
  RT_AT(4, 570);
  wv v577_d = rt_bind(rt_chk(s429_enumNamed(up, v576_x, v575_scope), "caseEnum"), 0);
  RT_AT(4, 571);
  if (rt_cond(rt_ne(v577_d, RT_NULL))) {
    RT_AT(4, 572);
    (void)rt_chk(s434_member(up, v574_v, v577_d), "caseEnum");
    RT_AT(4, 573);
    RT_RET(v577_d);
  }
  RT_AT(4, 575);
  wv v578_hint = rt_s(&S451);
  RT_AT(4, 576);
  if (rt_cond(rt_eq(rt_get(&F_k, v576_x), rt_s(&S214)))) {
    RT_AT(4, 577);
    wv v580_message = rt_bind((rt_cond(rt_ne(({ wv t977 = v575_scope; wv t978 = rt_get(&F_name, v576_x); rt_chk(s418_lookup(up, t977, t978), "caseEnum"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v576_x)), rt_s(&S452) }) : rt_cat(3, (wv[]){ rt_s(&S453), rt_text(rt_get(&F_name, v576_x)), rt_s(&S114) })), 0);
    RT_AT(4, 578);
    (void)({ wv t979 = rt_get(&F_pos, v576_x); wv t980 = v580_message; wv t981 = v578_hint; rt_chk(s409_error(up, t979, t980, t981), "caseEnum"); });
    RT_AT(4, 579);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 581);
  wv v579_m = rt_bind(({ wv t982 = rt_get(&F_rec, v576_x); wv t983 = v575_scope; rt_chk(s427_moduleNamed(up, t982, t983), "caseEnum"); }), 0);
  RT_AT(4, 582);
  if (rt_cond(rt_eq(v579_m, RT_NULL))) {
    RT_AT(4, 583);
    (void)({ wv t984 = rt_get(&F_rec, v576_x); wv t985 = v575_scope; rt_chk(s431_unknownModule(up, t984, t985), "caseEnum"); });
  } else if (rt_cond(rt_ne(rt_chk(s430_exportOf(up, v576_x, v579_m), "caseEnum"), RT_NULL))) {
    RT_AT(4, 585);
    (void)({ wv t986 = rt_get(&F_pos, v576_x); wv t987 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_chk(w5_enumText(v576_x), "caseEnum")), rt_s(&S452) }); wv t988 = v578_hint; rt_chk(s409_error(up, t986, t987, t988), "caseEnum"); });
  }
  RT_RET((RT_AT(4, 587), RT_NULL));
}

static wv s434_member(struct fr101 *up, wv v581_e, wv v582_d) {
  rt_enter("member");
  v581_e = rt_bind(v581_e, 0);
  v582_d = rt_bind(v582_d, 0);
  RT_AT(4, 592);
  wv v583_ms = rt_bind(rt_chk(s411_enumMembers(up, v582_d), "member"), 0);
  RT_AT(4, 593);
  wv v584_names = rt_arr(0, NULL, 1);
  RT_AT(4, 594);
  wv v585_found = rt_b(0);
  RT_AT(4, 595);
  wv v586_k = rt_i(0);
  RT_AT(4, 596);
  while (rt_cond((RT_AT(4, 596), ({ wv t989 = v586_k; wv t990 = std_length(v583_ms); rt_lt(t989, t990); })))) {
    RT_AT(4, 597);
    (void)({ wv t991 = v584_names; wv t992 = rt_get(&F_name, rt_idx(v586_k, v583_ms)); std_push(t991, t992); });
    RT_AT(4, 598);
    if (rt_cond(({ wv t993 = rt_get(&F_name, rt_idx(v586_k, v583_ms)); wv t994 = rt_get(&F_field, v581_e); rt_eq(t993, t994); }))) {
      RT_AT(4, 598);
      v585_found = rt_b(1);
    }
    RT_AT(4, 599);
    v586_k = rt_add(v586_k, rt_i(1));
  }
  RT_AT(4, 601);
  if (rt_cond(rt_not(v585_found))) {
    RT_AT(4, 602);
    (void)({ wv t997 = rt_get(&F_pos, v581_e); wv t998 = ({ wv t995 = rt_text(rt_get(&F_field, v581_e)); wv t996 = rt_text(rt_get(&F_name, v582_d)); rt_cat(5, (wv[]){ rt_s(&S454), t995, rt_s(&S455), t996, rt_s(&S114) }); }); wv t999 = rt_cat(2, (wv[]){ rt_s(&S456), rt_text(std_join(v584_names, rt_s(&S437))) }); rt_chk(s409_error(up, t997, t998, t999), "member"); });
  }
  RT_RET(RT_NULL);
}

static wv s435_chain(struct fr101 *up, wv v587_e, wv v588_scope, wv v589_ctx, wv v590_stmtLevel) {
  rt_enter("chain");
  v587_e = rt_bind(v587_e, 0);
  v588_scope = rt_bind(v588_scope, 0);
  v589_ctx = rt_bind(v589_ctx, 0);
  v590_stmtLevel = rt_bind(v590_stmtLevel, 0);
  RT_AT(4, 607);
  wv v591_k = rt_i(0);
  RT_AT(4, 608);
  while (rt_cond((RT_AT(4, 608), ({ wv t1000 = v591_k; wv t1001 = std_length(rt_get(&F_branches, v587_e)); rt_lt(t1000, t1001); })))) {
    RT_AT(4, 609);
    wv v592_b = rt_bind(({ wv t1002 = v591_k; wv t1003 = rt_get(&F_branches, v587_e); rt_idx(t1002, t1003); }), 0);
    RT_AT(4, 610);
    (void)({ wv t1004 = rt_get(&F_cond, v592_b); wv t1005 = v588_scope; wv t1006 = v589_ctx; rt_chk(s439_expr(up, t1004, t1005, t1006), "chain"); });
    RT_AT(4, 611);
    (void)({ wv t1007 = rt_get(&F_body, v592_b); wv t1008 = rt_chk(s414_newScope(up, v588_scope), "chain"); wv t1009 = v589_ctx; rt_chk(s420_block(up, t1007, t1008, t1009), "chain"); });
    RT_AT(4, 612);
    if (rt_cond(rt_not(v590_stmtLevel))) {
      RT_AT(4, 612);
      (void)rt_chk(s413_requireValue(up, rt_get(&F_body, v592_b)), "chain");
    }
    RT_AT(4, 613);
    v591_k = rt_add(v591_k, rt_i(1));
  }
  RT_AT(4, 615);
  if (rt_cond(rt_ne(rt_get(&F_els, v587_e), RT_NULL))) {
    RT_AT(4, 616);
    (void)({ wv t1010 = rt_get(&F_els, v587_e); wv t1011 = rt_chk(s414_newScope(up, v588_scope), "chain"); wv t1012 = v589_ctx; rt_chk(s420_block(up, t1010, t1011, t1012), "chain"); });
    RT_AT(4, 617);
    if (rt_cond(rt_not(v590_stmtLevel))) {
      RT_AT(4, 617);
      (void)rt_chk(s413_requireValue(up, rt_get(&F_els, v587_e)), "chain");
    }
  } else if (rt_cond(rt_not(v590_stmtLevel))) {
    RT_AT(4, 619);
    (void)rt_chk(s409_error(up, rt_get(&F_pos, v587_e), rt_s(&S457), RT_NULL), "chain");
  }
  RT_RET(RT_NULL);
}

static wv s436_lvalue(struct fr101 *up, wv v593_t, wv v594_scope, wv v595_ctx) {
  rt_enter("lvalue");
  v594_scope = rt_bind(v594_scope, 0);
  v595_ctx = rt_bind(v595_ctx, 0);
  RT_AT(4, 624);
  if (rt_cond(rt_chk(s424_guarded(up, v593_t, v595_ctx), "lvalue"))) {
    RT_AT(4, 624);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 625);
  if (rt_cond(rt_eq(rt_get(&F_k, v593_t), rt_s(&S214)))) {
    RT_AT(4, 626);
    wv v599_d = rt_bind(({ wv t1013 = rt_get(&F_name, v593_t); wv t1014 = rt_get(&F_pos, v593_t); wv t1015 = v594_scope; wv t1016 = v595_ctx; rt_chk(s419_resolve(up, t1013, t1014, t1015, t1016), "lvalue"); }), 0);
    RT_AT(4, 627);
    if (rt_cond(rt_ne(v599_d, RT_NULL))) {
      RT_AT(4, 628);
      (void)({ wv t1017 = v599_d; wv t1018 = rt_get(&F_name, v593_t); wv t1019 = rt_get(&F_pos, v593_t); wv t1020 = rt_cat(3, (wv[]){ rt_s(&S425), rt_text(rt_get(&F_name, v593_t)), rt_s(&S458) }); rt_chk(s437_mustBeMutable(up, t1017, t1018, t1019, t1020, rt_b(1)), "lvalue"); });
      RT_AT(4, 630);
      if (rt_cond(rt_get(&F_isRef, v599_d))) {
        RT_AT(4, 630);
        (void)({ wv t1023 = rt_get(&F_refTargets, ({ wv t1021 = rt_def(up->v405_curMod, "curMod"); wv t1022 = up->v401_modules; rt_idx(t1021, t1022); })); wv t1024 = rt_get(&F_pos, v593_t); std_push(t1023, t1024); });
      }
    }
    RT_AT(4, 632);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 636);
  wv v596_isMem = rt_eq(rt_get(&F_k, v593_t), rt_s(&S192));
  RT_AT(4, 637);
  wv v597_box = rt_bind((rt_cond(v596_isMem) ? rt_get(&F_rec, v593_t) : rt_get(&F_arr, v593_t)), 0);
  RT_AT(4, 638);
  wv v598_what = rt_bind((rt_cond(v596_isMem) ? rt_s(&S459) : rt_s(&S460)), 0);
  RT_AT(4, 639);
  if (rt_cond(rt_eq(rt_get(&F_k, v593_t), rt_s(&S191)))) {
    RT_AT(4, 639);
    (void)({ wv t1025 = rt_get(&F_index, v593_t); wv t1026 = v594_scope; wv t1027 = v595_ctx; rt_chk(s439_expr(up, t1025, t1026, t1027), "lvalue"); });
  }
  RT_AT(4, 640);
  if (rt_cond(v596_isMem)) {
    RT_AT(4, 641);
    if (rt_cond(rt_ne(rt_chk(s429_enumNamed(up, v597_box, v594_scope), "lvalue"), RT_NULL))) {
      RT_AT(4, 642);
      (void)({ wv t1030 = rt_get(&F_pos, v593_t); wv t1031 = ({ wv t1028 = rt_text(rt_chk(w5_enumText(v597_box), "lvalue")); wv t1029 = rt_text(rt_get(&F_field, v593_t)); rt_cat(5, (wv[]){ rt_s(&S425), t1028, rt_s(&S130), t1029, rt_s(&S461) }); }); rt_chk(s409_error(up, t1030, t1031, RT_NULL), "lvalue"); });
      RT_AT(4, 643);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 646);
    wv v600_m = rt_bind(rt_chk(s427_moduleNamed(up, v597_box, v594_scope), "lvalue"), 0);
    RT_AT(4, 647);
    if (rt_cond(rt_ne(v600_m, RT_NULL))) {
      RT_AT(4, 648);
      if (rt_cond(rt_ne(rt_chk(s430_exportOf(up, v593_t, v600_m), "lvalue"), RT_NULL))) {
        RT_AT(4, 649);
        (void)({ wv t1037 = rt_get(&F_pos, v593_t); wv t1038 = ({ wv t1034 = rt_text(rt_get(&F_name, v597_box)); wv t1035 = rt_text(rt_get(&F_field, v593_t)); wv t1036 = rt_text(rt_get(&F_path, ({ wv t1032 = rt_get(&F_lib, v600_m); wv t1033 = up->v401_modules; rt_idx(t1032, t1033); }))); rt_cat(7, (wv[]){ rt_s(&S425), t1034, rt_s(&S130), t1035, rt_s(&S462), t1036, rt_s(&S463) }); }); rt_chk(s409_error(up, t1037, t1038, RT_NULL), "lvalue"); });
      }
      RT_AT(4, 651);
      RT_RET(RT_NULL);
    }
  }
  RT_AT(4, 654);
  if (rt_cond(rt_eq(rt_get(&F_k, v597_box), rt_s(&S214)))) {
    RT_AT(4, 655);
    wv v601_d = rt_bind(({ wv t1039 = rt_get(&F_name, v597_box); wv t1040 = rt_get(&F_pos, v597_box); wv t1041 = v594_scope; wv t1042 = v595_ctx; rt_chk(s419_resolve(up, t1039, t1040, t1041, t1042), "lvalue"); }), 0);
    RT_AT(4, 656);
    if (rt_cond(rt_ne(v601_d, RT_NULL))) {
      RT_AT(4, 657);
      (void)({ wv t1045 = v601_d; wv t1046 = rt_get(&F_name, v597_box); wv t1047 = rt_get(&F_pos, v597_box); wv t1048 = ({ wv t1043 = rt_text(v598_what); wv t1044 = rt_text(rt_get(&F_name, v597_box)); rt_cat(5, (wv[]){ rt_s(&S464), t1043, rt_s(&S465), t1044, rt_s(&S466) }); }); rt_chk(s437_mustBeMutable(up, t1045, t1046, t1047, t1048, rt_b(0)), "lvalue"); });
    }
  } else {
    RT_AT(4, 660);
    (void)rt_chk(s439_expr(up, v597_box, v594_scope, v595_ctx), "lvalue");
  }
  RT_RET(RT_NULL);
}

static wv s437_mustBeMutable(struct fr101 *up, wv v602_d, wv v603_name, wv v604_pos, wv v605_message, wv v606_rebind) {
  rt_enter("mustBeMutable");
  v602_d = rt_bind(v602_d, 0);
  v603_name = rt_bind(v603_name, 0);
  v604_pos = rt_bind(v604_pos, 0);
  v605_message = rt_bind(v605_message, 0);
  v606_rebind = rt_bind(v606_rebind, 0);
  RT_AT(4, 668);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v602_d), rt_s(&S33)), "||") || rt_bool(rt_eq(rt_get(&F_kind, v602_d), rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_kind, v602_d), rt_s(&S440)), "||")))) {
    RT_AT(4, 669);
    (void)({ wv t1051 = v604_pos; wv t1052 = ({ wv t1049 = rt_text(v603_name); wv t1050 = rt_text(rt_chk(w5_kindText(rt_get(&F_kind, v602_d)), "mustBeMutable")); rt_cat(5, (wv[]){ rt_s(&S114), t1049, rt_s(&S467), t1050, rt_s(&S468) }); }); rt_chk(s409_error(up, t1051, t1052, RT_NULL), "mustBeMutable"); });
  } else if (rt_cond(rt_eq(rt_get(&F_kind, v602_d), rt_s(&S423)))) {
    RT_AT(4, 672);
    if (rt_cond(v606_rebind)) {
      RT_AT(4, 673);
      (void)({ wv t1053 = v604_pos; wv t1054 = rt_cat(3, (wv[]){ rt_s(&S425), rt_text(v603_name), rt_s(&S469) }); rt_chk(s409_error(up, t1053, t1054, rt_s(&S470)), "mustBeMutable"); });
    }
  } else if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_isVar, v602_d)), "&&") && rt_bool(rt_not(rt_get(&F_isRef, v602_d)), "&&")))) {
    RT_AT(4, 677);
    wv v607_hint = RT_NULL;
    RT_AT(4, 678);
    if (rt_cond(rt_eq(rt_get(&F_kind, v602_d), rt_s(&S419)))) {
      RT_AT(4, 679);
      v607_hint = rt_bind((rt_cond(v606_rebind) ? ({ wv t1055 = rt_text(v603_name); wv t1056 = rt_text(v603_name); rt_cat(5, (wv[]){ rt_s(&S471), t1055, rt_s(&S472), t1056, rt_s(&S473) }); }) : rt_cat(3, (wv[]){ rt_s(&S474), rt_text(v603_name), rt_s(&S16) })), 1);
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v602_d), rt_s(&S179)))) {
      RT_AT(4, 682);
      v607_hint = rt_bind((rt_cond(v606_rebind) ? ({ wv t1057 = rt_text(v603_name); wv t1058 = rt_text(v603_name); rt_cat(5, (wv[]){ rt_s(&S475), t1057, rt_s(&S476), t1058, rt_s(&S477) }); }) : rt_cat(3, (wv[]){ rt_s(&S478), rt_text(v603_name), rt_s(&S479) })), 1);
    }
    RT_AT(4, 685);
    (void)rt_chk(s409_error(up, v604_pos, v605_message, v607_hint), "mustBeMutable");
  }
  RT_RET(RT_NULL);
}

static wv s438_unknownStd(struct fr101 *up, wv v608_node) {
  rt_enter("unknownStd");
  v608_node = rt_bind(v608_node, 0);
  RT_AT(4, 690);
  (void)({ wv t1059 = rt_get(&F_pos, v608_node); wv t1060 = rt_cat(3, (wv[]){ rt_s(&S480), rt_text(rt_get(&F_name, v608_node)), rt_s(&S114) }); rt_chk(s409_error(up, t1059, t1060, rt_s(&S137)), "unknownStd"); });
  RT_RET(RT_NULL);
}

static wv s439_expr(struct fr101 *up, wv v609_e, wv v610_scope, wv v611_ctx) {
  rt_enter("expr");
  v610_scope = rt_bind(v610_scope, 0);
  v611_ctx = rt_bind(v611_ctx, 0);
  RT_AT(4, 695);
  wv v612_kind = rt_bind(rt_get(&F_k, v609_e), 0);
  RT_AT(4, 696);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v612_kind, rt_s(&S139)), "||") || rt_bool(rt_eq(v612_kind, rt_s(&S149)), "||")), "||") || rt_bool(rt_eq(v612_kind, rt_s(&S300)), "||")), "||") || rt_bool(rt_eq(v612_kind, rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(v612_kind, rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(v612_kind, rt_s(&S54)), "||")))) {
    RT_AT(4, 697);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 699);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S173)))) {
    RT_AT(4, 700);
    wv v613_k = rt_i(0);
    RT_AT(4, 701);
    while (rt_cond((RT_AT(4, 701), ({ wv t1061 = v613_k; wv t1062 = std_length(rt_get(&F_parts, v609_e)); rt_lt(t1061, t1062); })))) {
      RT_AT(4, 702);
      if (rt_cond(rt_ne(std_typeOf(({ wv t1063 = v613_k; wv t1064 = rt_get(&F_parts, v609_e); rt_idx(t1063, t1064); })), rt_s(&S187)))) {
        RT_AT(4, 702);
        (void)({ wv t1067 = ({ wv t1065 = v613_k; wv t1066 = rt_get(&F_parts, v609_e); rt_idx(t1065, t1066); }); wv t1068 = v610_scope; wv t1069 = v611_ctx; rt_chk(s439_expr(up, t1067, t1068, t1069), "expr"); });
      }
      RT_AT(4, 703);
      v613_k = rt_add(v613_k, rt_i(1));
    }
    RT_AT(4, 705);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 707);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S214)))) {
    RT_AT(4, 708);
    wv v614_d = rt_bind(({ wv t1070 = rt_get(&F_name, v609_e); wv t1071 = rt_get(&F_pos, v609_e); wv t1072 = v610_scope; wv t1073 = v611_ctx; rt_chk(s419_resolve(up, t1070, t1071, t1072, t1073), "expr"); }), 0);
    RT_AT(4, 709);
    if (rt_cond(rt_b(rt_bool(rt_ne(v614_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v614_d), rt_s(&S33)), "&&")))) {
      RT_AT(4, 710);
      (void)({ wv t1076 = rt_get(&F_pos, v609_e); wv t1077 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v609_e)), rt_s(&S444) }); wv t1078 = ({ wv t1074 = rt_text(rt_get(&F_name, v609_e)); wv t1075 = rt_text(rt_get(&F_name, v609_e)); rt_cat(5, (wv[]){ rt_s(&S445), t1074, rt_s(&S446), t1075, rt_s(&S447) }); }); rt_chk(s409_error(up, t1076, t1077, t1078), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v614_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v614_d), rt_s(&S47)), "&&")))) {
      RT_AT(4, 712);
      (void)({ wv t1079 = rt_get(&F_pos, v609_e); wv t1080 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v609_e)), rt_s(&S448) }); wv t1081 = rt_cat(3, (wv[]){ rt_s(&S449), rt_text(rt_get(&F_name, v609_e)), rt_s(&S450) }); rt_chk(s409_error(up, t1079, t1080, t1081), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v614_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v614_d), rt_s(&S440)), "&&")))) {
      RT_AT(4, 714);
      (void)({ wv t1082 = rt_get(&F_pos, v609_e); wv t1083 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v609_e)), rt_s(&S481) }); wv t1084 = rt_cat(3, (wv[]){ rt_s(&S482), rt_text(rt_get(&F_name, v609_e)), rt_s(&S483) }); rt_chk(s409_error(up, t1082, t1083, t1084), "expr"); });
    }
    RT_AT(4, 716);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 718);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S157)))) {
    RT_AT(4, 719);
    wv v615_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v609_e)), "expr"), 0);
    RT_AT(4, 720);
    if (rt_cond(rt_eq(v615_sig, RT_NULL))) {
      RT_AT(4, 721);
      (void)rt_chk(s438_unknownStd(up, v609_e), "expr");
    } else if (rt_cond(rt_ne(v615_sig, rt_s(&S35)))) {
      RT_AT(4, 723);
      (void)({ wv t1087 = rt_get(&F_pos, v609_e); wv t1088 = ({ wv t1085 = rt_text(rt_get(&F_name, v609_e)); wv t1086 = rt_text(rt_get(&F_name, v609_e)); rt_cat(5, (wv[]){ rt_s(&S181), t1085, rt_s(&S484), t1086, rt_s(&S16) }); }); rt_chk(s409_error(up, t1087, t1088, RT_NULL), "expr"); });
    }
    RT_AT(4, 725);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 727);
  if (rt_cond(rt_b(rt_bool(rt_eq(v612_kind, rt_s(&S188)), "||") || rt_bool(rt_eq(v612_kind, rt_s(&S189)), "||")))) {
    RT_AT(4, 728);
    wv v616_k = rt_i(0);
    RT_AT(4, 729);
    while (rt_cond((RT_AT(4, 729), ({ wv t1089 = v616_k; wv t1090 = std_length(rt_get(&F_items, v609_e)); rt_lt(t1089, t1090); })))) {
      RT_AT(4, 730);
      (void)({ wv t1093 = ({ wv t1091 = v616_k; wv t1092 = rt_get(&F_items, v609_e); rt_idx(t1091, t1092); }); wv t1094 = v610_scope; wv t1095 = v611_ctx; rt_chk(s439_expr(up, t1093, t1094, t1095), "expr"); });
      RT_AT(4, 731);
      v616_k = rt_add(v616_k, rt_i(1));
    }
    RT_AT(4, 733);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 735);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S190)))) {
    RT_AT(4, 736);
    wv v617_k = rt_i(0);
    RT_AT(4, 737);
    while (rt_cond((RT_AT(4, 737), ({ wv t1096 = v617_k; wv t1097 = std_length(rt_get(&F_fields, v609_e)); rt_lt(t1096, t1097); })))) {
      RT_AT(4, 738);
      (void)({ wv t1100 = rt_get(&F_e, ({ wv t1098 = v617_k; wv t1099 = rt_get(&F_fields, v609_e); rt_idx(t1098, t1099); })); wv t1101 = v610_scope; wv t1102 = v611_ctx; rt_chk(s439_expr(up, t1100, t1101, t1102), "expr"); });
      RT_AT(4, 739);
      v617_k = rt_add(v617_k, rt_i(1));
    }
    RT_AT(4, 741);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 743);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S191)))) {
    RT_AT(4, 744);
    (void)({ wv t1103 = rt_get(&F_index, v609_e); wv t1104 = v610_scope; wv t1105 = v611_ctx; rt_chk(s439_expr(up, t1103, t1104, t1105), "expr"); });
    RT_AT(4, 745);
    (void)({ wv t1106 = rt_get(&F_arr, v609_e); wv t1107 = v610_scope; wv t1108 = v611_ctx; rt_chk(s439_expr(up, t1106, t1107, t1108), "expr"); });
    RT_AT(4, 746);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 750);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S192)))) {
    RT_AT(4, 751);
    wv v618_lin = rt_bind(({ wv t1109 = rt_get(&F_rec, v609_e); wv t1110 = v610_scope; rt_chk(s429_enumNamed(up, t1109, t1110), "expr"); }), 0);
    RT_AT(4, 752);
    if (rt_cond(rt_ne(v618_lin, RT_NULL))) {
      RT_AT(4, 753);
      (void)rt_chk(s434_member(up, v609_e, v618_lin), "expr");
      RT_AT(4, 754);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 756);
    wv v619_m = rt_bind(({ wv t1111 = rt_get(&F_rec, v609_e); wv t1112 = v610_scope; rt_chk(s427_moduleNamed(up, t1111, t1112), "expr"); }), 0);
    RT_AT(4, 757);
    if (rt_cond(rt_ne(v619_m, RT_NULL))) {
      RT_AT(4, 757);
      (void)rt_chk(s432_qualified(up, v609_e, v619_m), "expr");
    } else {
      RT_AT(4, 757);
      (void)({ wv t1113 = rt_get(&F_rec, v609_e); wv t1114 = v610_scope; wv t1115 = v611_ctx; rt_chk(s439_expr(up, t1113, t1114, t1115), "expr"); });
    }
    RT_AT(4, 758);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 760);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S193)))) {
    RT_AT(4, 761);
    (void)({ wv t1116 = rt_get(&F_e, v609_e); wv t1117 = v610_scope; wv t1118 = v611_ctx; rt_chk(s439_expr(up, t1116, t1117, t1118), "expr"); });
    RT_AT(4, 762);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 764);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S194)))) {
    RT_AT(4, 765);
    (void)({ wv t1119 = rt_get(&F_l, v609_e); wv t1120 = v610_scope; wv t1121 = v611_ctx; rt_chk(s439_expr(up, t1119, t1120, t1121), "expr"); });
    RT_AT(4, 766);
    (void)({ wv t1122 = rt_get(&F_r, v609_e); wv t1123 = v610_scope; wv t1124 = v611_ctx; rt_chk(s439_expr(up, t1122, t1123, t1124), "expr"); });
    RT_AT(4, 767);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 769);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S195)))) {
    RT_AT(4, 770);
    (void)({ wv t1125 = rt_get(&F_c, v609_e); wv t1126 = v610_scope; wv t1127 = v611_ctx; rt_chk(s439_expr(up, t1125, t1126, t1127), "expr"); });
    RT_AT(4, 771);
    (void)({ wv t1128 = rt_get(&F_a, v609_e); wv t1129 = v610_scope; wv t1130 = v611_ctx; rt_chk(s439_expr(up, t1128, t1129, t1130), "expr"); });
    RT_AT(4, 772);
    (void)({ wv t1131 = rt_get(&F_b, v609_e); wv t1132 = v610_scope; wv t1133 = v611_ctx; rt_chk(s439_expr(up, t1131, t1132, t1133), "expr"); });
    RT_AT(4, 773);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 775);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S184)))) {
    RT_AT(4, 776);
    wv v620_k = rt_i(0);
    RT_AT(4, 777);
    while (rt_cond((RT_AT(4, 777), ({ wv t1134 = v620_k; wv t1135 = std_length(rt_get(&F_args, v609_e)); rt_lt(t1134, t1135); })))) {
      RT_AT(4, 778);
      (void)({ wv t1138 = ({ wv t1136 = v620_k; wv t1137 = rt_get(&F_args, v609_e); rt_idx(t1136, t1137); }); wv t1139 = v610_scope; wv t1140 = v611_ctx; rt_chk(s439_expr(up, t1138, t1139, t1140), "expr"); });
      RT_AT(4, 779);
      v620_k = rt_add(v620_k, rt_i(1));
    }
    RT_AT(4, 781);
    (void)rt_chk(s440_callee(up, v609_e, v610_scope, rt_b(1)), "expr");
    RT_AT(4, 782);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 784);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S185)))) {
    RT_AT(4, 785);
    (void)rt_chk(s409_error(up, rt_get(&F_pos, v609_e), rt_s(&S485), RT_NULL), "expr");
    RT_AT(4, 786);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 788);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S45)))) {
    RT_AT(4, 789);
    (void)rt_chk(s426_checkMatch(up, v609_e, v610_scope, v611_ctx, rt_b(0)), "expr");
    RT_AT(4, 790);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 792);
  if (rt_cond(rt_eq(v612_kind, rt_s(&S186)))) {
    RT_AT(4, 793);
    (void)rt_chk(s435_chain(up, v609_e, v610_scope, v611_ctx, rt_b(0)), "expr");
    RT_AT(4, 794);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 796);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S486), rt_text(v612_kind) }));
  RT_RET(RT_NULL);
}

static wv s440_callee(struct fr101 *up, wv v621_e, wv v622_scope, wv v623_used) {
  rt_enter("callee");
  v621_e = rt_bind(v621_e, 0);
  v622_scope = rt_bind(v622_scope, 0);
  v623_used = rt_bind(v623_used, 0);
  RT_AT(4, 801);
  wv v624_c = rt_bind(rt_get(&F_callee, v621_e), 0);
  RT_AT(4, 802);
  wv v625_n = rt_bind(std_length(rt_get(&F_args, v621_e)), 0);
  RT_AT(4, 803);
  if (rt_cond(rt_eq(rt_get(&F_k, v624_c), rt_s(&S157)))) {
    RT_AT(4, 804);
    wv v628_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v624_c)), "callee"), 0);
    RT_AT(4, 805);
    if (rt_cond(rt_eq(v628_sig, RT_NULL))) {
      RT_AT(4, 805);
      (void)rt_chk(s438_unknownStd(up, v624_c), "callee");
      RT_AT(4, 805);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 806);
    if (rt_cond(rt_eq(v628_sig, rt_s(&S35)))) {
      RT_AT(4, 806);
      (void)({ wv t1141 = rt_get(&F_pos, v624_c); wv t1142 = rt_cat(3, (wv[]){ rt_s(&S181), rt_text(rt_get(&F_name, v624_c)), rt_s(&S487) }); rt_chk(s409_error(up, t1141, t1142, RT_NULL), "callee"); });
      RT_AT(4, 806);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 807);
    if (rt_cond(rt_ne(v628_sig, v625_n))) {
      RT_AT(4, 807);
      (void)({ wv t1146 = rt_get(&F_pos, v624_c); wv t1147 = ({ wv t1143 = rt_text(rt_get(&F_name, v624_c)); wv t1144 = rt_text(rt_chk(w5_plural(v628_sig), "callee")); wv t1145 = rt_text(v625_n); rt_cat(6, (wv[]){ rt_s(&S181), t1143, rt_s(&S488), t1144, rt_s(&S489), t1145 }); }); rt_chk(s409_error(up, t1146, t1147, RT_NULL), "callee"); });
    }
    RT_AT(4, 808);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 810);
  wv v626_d = RT_NULL;
  RT_AT(4, 811);
  wv v627_label = rt_s(&S13);
  RT_AT(4, 812);
  if (rt_cond(rt_eq(rt_get(&F_k, v624_c), rt_s(&S192)))) {
    RT_AT(4, 814);
    v627_label = ({ wv t1148 = rt_text(rt_get(&F_name, rt_get(&F_rec, v624_c))); wv t1149 = rt_text(rt_get(&F_field, v624_c)); rt_cat(3, (wv[]){ t1148, rt_s(&S130), t1149 }); });
    RT_AT(4, 815);
    wv v629_m = rt_bind(({ wv t1150 = rt_get(&F_rec, v624_c); wv t1151 = v622_scope; rt_chk(s427_moduleNamed(up, t1150, t1151), "callee"); }), 0);
    RT_AT(4, 816);
    if (rt_cond(rt_eq(v629_m, RT_NULL))) {
      RT_AT(4, 816);
      (void)({ wv t1152 = rt_get(&F_rec, v624_c); wv t1153 = v622_scope; rt_chk(s431_unknownModule(up, t1152, t1153), "callee"); });
      RT_AT(4, 816);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 817);
    v626_d = rt_chk(s430_exportOf(up, v624_c, v629_m), "callee");
    RT_AT(4, 818);
    if (rt_cond(rt_eq(v626_d, RT_NULL))) {
      RT_AT(4, 818);
      RT_RET(RT_NULL);
    }
  } else {
    RT_AT(4, 820);
    v627_label = rt_bind(rt_get(&F_name, v624_c), 1);
    RT_AT(4, 821);
    v626_d = ({ wv t1154 = v622_scope; wv t1155 = rt_get(&F_name, v624_c); rt_chk(s418_lookup(up, t1154, t1155), "callee"); });
    RT_AT(4, 822);
    if (rt_cond(rt_eq(v626_d, RT_NULL))) {
      RT_AT(4, 822);
      (void)({ wv t1156 = rt_get(&F_pos, v624_c); wv t1157 = rt_cat(3, (wv[]){ rt_s(&S490), rt_text(rt_get(&F_name, v624_c)), rt_s(&S114) }); rt_chk(s409_error(up, t1156, t1157, RT_NULL), "callee"); });
      RT_AT(4, 822);
      RT_RET(RT_NULL);
    }
  }
  RT_AT(4, 824);
  if (rt_cond(rt_ne(rt_get(&F_kind, v626_d), rt_s(&S33)))) {
    RT_AT(4, 825);
    wv v630_hint = rt_bind((rt_cond(rt_eq(v625_n, rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S98), rt_text(v627_label), rt_s(&S491) }) : rt_s(&S492)), 0);
    RT_AT(4, 827);
    (void)({ wv t1158 = rt_get(&F_pos, v624_c); wv t1159 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v627_label), rt_s(&S493) }); wv t1160 = v630_hint; rt_chk(s409_error(up, t1158, t1159, t1160), "callee"); });
    RT_AT(4, 828);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 830);
  if (rt_cond(({ wv t1161 = rt_get(&F_arity, v626_d); wv t1162 = v625_n; rt_ne(t1161, t1162); }))) {
    RT_AT(4, 830);
    (void)({ wv t1166 = rt_get(&F_pos, v624_c); wv t1167 = ({ wv t1163 = rt_text(v627_label); wv t1164 = rt_text(rt_chk(w5_plural(rt_get(&F_arity, v626_d)), "callee")); wv t1165 = rt_text(v625_n); rt_cat(6, (wv[]){ rt_s(&S114), t1163, rt_s(&S488), t1164, rt_s(&S489), t1165 }); }); rt_chk(s409_error(up, t1166, t1167, RT_NULL), "callee"); });
  }
  RT_AT(4, 832);
  if (rt_cond(rt_b(rt_bool(v623_used, "&&") && rt_bool(rt_eq(({ wv t1168 = rt_get(&F_mod, v626_d); wv t1169 = rt_get(&F_namePos, v626_d); rt_chk(s412_firstUse(up, t1168, t1169), "callee"); }), RT_NULL), "&&")))) {
    RT_AT(4, 833);
    (void)({ wv t1174 = rt_def(up->v407_uses, "uses"); wv t1175 = ({ wv t1170 = rt_get(&F_mod, v626_d); wv t1171 = rt_get(&F_namePos, v626_d); wv t1172 = rt_get(&F_pos, v624_c); wv t1173 = rt_def(up->v405_curMod, "curMod"); rt_rec(0, 4, K48, (wv[]){ t1170, t1171, t1172, t1173 }); }); std_push(t1174, t1175); });
  }
  RT_RET(RT_NULL);
}

static wv s441_importNames(struct fr101 *up, wv v631_m, wv v632_scope) {
  rt_enter("importNames");
  v631_m = rt_bind(v631_m, 0);
  v632_scope = rt_bind(v632_scope, 0);
  RT_AT(4, 840);
  wv v633_imports = rt_bind(rt_get(&F_imports, rt_idx(v631_m, up->v401_modules)), 0);
  RT_AT(4, 841);
  wv v634_seen = rt_arr(0, NULL, 1);
  RT_AT(4, 842);
  wv v635_k = rt_i(0);
  RT_AT(4, 843);
  while (rt_cond((RT_AT(4, 843), ({ wv t1176 = v635_k; wv t1177 = std_length(v633_imports); rt_lt(t1176, t1177); })))) {
    RT_AT(4, 844);
    wv v636_node = rt_bind(rt_get(&F_node, rt_idx(v635_k, v633_imports)), 0);
    RT_AT(4, 845);
    wv v637_lib = rt_bind(rt_get(&F_lib, rt_idx(v635_k, v633_imports)), 0);
    RT_AT(4, 846);
    v635_k = rt_add(v635_k, rt_i(1));
    RT_AT(4, 847);
    wv v638_path = rt_bind(rt_get(&F_path, rt_idx(v637_lib, up->v401_modules)), 0);
    RT_AT(4, 848);
    wv v639_usable = rt_bind(v637_lib, 1);
    RT_AT(4, 849);
    if (rt_cond(rt_chk(w1_contains(v634_seen, v637_lib), "importNames"))) {
      RT_AT(4, 850);
      (void)({ wv t1178 = rt_get(&F_pathPos, v636_node); wv t1179 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v638_path), rt_s(&S494) }); rt_chk(s409_error(up, t1178, t1179, RT_NULL), "importNames"); });
      RT_AT(4, 851);
      v639_usable = RT_NULL;
    } else if (rt_cond(rt_get(&F_notLibrary, rt_idx(v637_lib, up->v401_modules)))) {
      RT_AT(4, 853);
      (void)({ wv t1180 = rt_get(&F_pathPos, v636_node); wv t1181 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v638_path), rt_s(&S495) }); rt_chk(s409_error(up, t1180, t1181, RT_NULL), "importNames"); });
      RT_AT(4, 854);
      v639_usable = RT_NULL;
    }
    RT_AT(4, 856);
    (void)std_push(v634_seen, v637_lib);
    RT_AT(4, 857);
    wv v640_d = ({ wv t1182 = rt_get(&F_name, v636_node); wv t1183 = rt_get(&F_namePos, v636_node); wv t1184 = rt_def(up->v405_curMod, "curMod"); wv t1185 = v639_usable; rt_rec(0, 10, K42, (wv[]){ rt_s(&S440), t1182, rt_b(0), rt_b(0), t1183, RT_NULL, RT_NULL, RT_NULL, t1184, t1185 }); });
    RT_AT(4, 861);
    (void)({ wv t1186 = v632_scope; wv t1187 = rt_get(&F_name, v636_node); wv t1188 = v640_d; rt_chk(s417_declare(up, t1186, t1187, t1188), "importNames"); });
  }
  RT_RET(RT_NULL);
}

static wv s442_libraryRules(struct fr101 *up, wv v641_m, wv v642_scope, wv v643_isEntry, wv v644_last) {
  rt_enter("libraryRules");
  v641_m = rt_bind(v641_m, 0);
  v642_scope = rt_bind(v642_scope, 0);
  v643_isEntry = rt_bind(v643_isEntry, 0);
  v644_last = rt_bind(v644_last, 0);
  RT_AT(4, 867);
  wv v645_program = rt_bind(rt_get(&F_program, rt_idx(v641_m, up->v401_modules)), 0);
  RT_AT(4, 868);
  wv v646_k = rt_i(0);
  RT_AT(4, 869);
  while (rt_cond((RT_AT(4, 869), ({ wv t1189 = v646_k; wv t1190 = std_length(rt_get(&F_stmts, v645_program)); rt_lt(t1189, t1190); })))) {
    RT_AT(4, 870);
    wv v648_s = rt_bind(({ wv t1191 = v646_k; wv t1192 = rt_get(&F_stmts, v645_program); rt_idx(t1191, t1192); }), 0);
    RT_AT(4, 871);
    v646_k = rt_add(v646_k, rt_i(1));
    RT_AT(4, 873);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v648_s), rt_s(&S206)), "&&") && rt_bool(rt_b(rt_bool(v643_isEntry, "||") || rt_bool(({ wv t1193 = rt_get(&F_pos, v648_s); wv t1194 = rt_get(&F_pos, v644_last); rt_ne(t1193, t1194); }), "||")), "&&")))) {
      RT_AT(4, 874);
      (void)rt_chk(s409_error(up, rt_get(&F_pos, v648_s), rt_s(&S496), rt_s(&S497)), "libraryRules");
    } else if (rt_cond(rt_b(rt_bool(rt_not(v643_isEntry), "&&") && rt_bool(rt_not(({ wv t1195 = rt_def(w5_libraryTop, "libraryTop"); wv t1196 = rt_get(&F_k, v648_s); rt_chk(w1_contains(t1195, t1196), "libraryRules"); })), "&&")))) {
      RT_AT(4, 876);
      (void)rt_chk(s409_error(up, rt_get(&F_pos, v648_s), rt_s(&S498), rt_s(&S499)), "libraryRules");
    }
  }
  RT_AT(4, 880);
  if (rt_cond(v643_isEntry)) {
    RT_AT(4, 880);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 881);
  if (rt_cond(rt_b(rt_bool(rt_eq(v644_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v644_last), rt_s(&S206)), "||")))) {
    RT_AT(4, 882);
    (void)rt_chk(s409_error(up, rt_get(&F_end, v645_program), rt_s(&S500), rt_s(&S501)), "libraryRules");
    RT_AT(4, 883);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 885);
  wv v647_exports = rt_arr(0, NULL, 1);
  RT_AT(4, 886);
  v646_k = rt_i(0);
  RT_AT(4, 887);
  while (rt_cond((RT_AT(4, 887), ({ wv t1197 = v646_k; wv t1198 = std_length(rt_get(&F_names, v644_last)); rt_lt(t1197, t1198); })))) {
    RT_AT(4, 888);
    wv v649_n = rt_bind(({ wv t1199 = v646_k; wv t1200 = rt_get(&F_names, v644_last); rt_idx(t1199, t1200); }), 0);
    RT_AT(4, 889);
    v646_k = rt_add(v646_k, rt_i(1));
    RT_AT(4, 890);
    wv v650_d = rt_bind(({ wv t1201 = v642_scope; wv t1202 = rt_get(&F_name, v649_n); rt_chk(s416_own(up, t1201, t1202), "libraryRules"); }), 0);
    RT_AT(4, 891);
    if (rt_cond(rt_eq(v650_d, RT_NULL))) {
      RT_AT(4, 892);
      (void)({ wv t1203 = rt_get(&F_pos, v649_n); wv t1204 = rt_cat(3, (wv[]){ rt_s(&S502), rt_text(rt_get(&F_name, v649_n)), rt_s(&S503) }); rt_chk(s409_error(up, t1203, t1204, RT_NULL), "libraryRules"); });
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v650_d), rt_s(&S440)))) {
      RT_AT(4, 894);
      (void)({ wv t1205 = rt_get(&F_pos, v649_n); wv t1206 = rt_cat(3, (wv[]){ rt_s(&S502), rt_text(rt_get(&F_name, v649_n)), rt_s(&S504) }); rt_chk(s409_error(up, t1205, t1206, RT_NULL), "libraryRules"); });
    } else {
      RT_AT(4, 896);
      (void)std_push(v647_exports, v650_d);
    }
  }
  RT_AT(4, 899);
  ({ wv t1207 = v647_exports; wv t1208 = rt_idx(v641_m, up->v401_modules); rt_set_mem(t1207, &F_exports, t1208); });
  RT_RET(RT_NULL);
}

static wv s443_checkModule(struct fr101 *up, wv v651_m, wv v652_isEntry) {
  rt_enter("checkModule");
  v651_m = rt_bind(v651_m, 0);
  v652_isEntry = rt_bind(v652_isEntry, 0);
  RT_AT(4, 903);
  (void)rt_chk(s410_enter(up, v651_m), "checkModule");
  RT_AT(4, 904);
  wv v653_program = rt_bind(rt_get(&F_program, rt_idx(v651_m, up->v401_modules)), 0);
  RT_AT(4, 905);
  wv v654_last = RT_NULL;
  RT_AT(4, 906);
  wv v655_k = rt_i(0);
  RT_AT(4, 907);
  while (rt_cond((RT_AT(4, 907), ({ wv t1209 = v655_k; wv t1210 = std_length(rt_get(&F_stmts, v653_program)); rt_lt(t1209, t1210); })))) {
    RT_AT(4, 908);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1211 = v655_k; wv t1212 = rt_get(&F_stmts, v653_program); rt_idx(t1211, t1212); })), rt_s(&S203)))) {
      RT_AT(4, 908);
      v654_last = rt_bind(({ wv t1213 = v655_k; wv t1214 = rt_get(&F_stmts, v653_program); rt_idx(t1213, t1214); }), 1);
    }
    RT_AT(4, 909);
    v655_k = rt_add(v655_k, rt_i(1));
  }
  RT_AT(4, 912);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v652_isEntry), "&&") && rt_bool(rt_ne(v654_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v654_last), rt_s(&S55)), "&&")))) {
    RT_AT(4, 913);
    rt_set_mem(rt_b(1), &F_notLibrary, rt_idx(v651_m, up->v401_modules));
    RT_AT(4, 914);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 916);
  wv v656_scope = rt_bind(rt_chk(s414_newScope(up, RT_NULL), "checkModule"), 0);
  RT_AT(4, 917);
  (void)rt_chk(s441_importNames(up, v651_m, v656_scope), "checkModule");
  RT_AT(4, 918);
  (void)({ wv t1217 = rt_rec(0, 2, K49, (wv[]){ rt_get(&F_stmts, v653_program), RT_NULL }); wv t1218 = v656_scope; wv t1219 = ({ wv t1215 = rt_def(up->v444_topFn, "topFn"); wv t1216 = rt_arr(0, NULL, 0); rt_rec(0, 3, K46, (wv[]){ t1215, rt_i(0), t1216 }); }); rt_chk(s420_block(up, t1217, t1218, t1219), "checkModule"); });
  RT_AT(4, 919);
  (void)rt_chk(s442_libraryRules(up, v651_m, v656_scope, v652_isEntry, v654_last), "checkModule");
  RT_RET(RT_NULL);
}

static wv w6_binFn(wv v663_op) {
  rt_enter("binFn");
  v663_op = rt_bind(v663_op, 0);
  RT_AT(5, 28);
  wv v664_k = rt_i(0);
  RT_AT(5, 29);
  while (rt_cond((RT_AT(5, 29), ({ wv t1246 = ({ wv t1244 = v664_k; wv t1245 = rt_def(w6_binOps, "binOps"); rt_idx(t1244, t1245); }); wv t1247 = v663_op; rt_ne(t1246, t1247); })))) {
    RT_AT(5, 29);
    v664_k = rt_add(v664_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 30), ({ wv t1248 = v664_k; wv t1249 = rt_def(w6_binFns, "binFns"); rt_idx(t1248, t1249); })));
}

static wv w6_modeOf(wv v665_x) {
  rt_enter("modeOf");
  v665_x = rt_bind(v665_x, 0);
  RT_RET((RT_AT(5, 33), (rt_cond(rt_get(&F_isVar, v665_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v665_x)) ? rt_s(&S37) : rt_s(&S35)))));
}

static wv w6_unFn(wv v666_op) {
  rt_enter("unFn");
  v666_op = rt_bind(v666_op, 0);
  RT_RET((RT_AT(5, 35), (rt_cond(rt_eq(v666_op, rt_s(&S86))) ? rt_s(&S528) : (rt_cond(rt_eq(v666_op, rt_s(&S95))) ? rt_s(&S529) : rt_s(&S530)))));
}

static wv w6_repeat(wv v667_s, wv v668_count) {
  rt_enter("repeat");
  v667_s = rt_bind(v667_s, 0);
  v668_count = rt_bind(v668_count, 0);
  RT_AT(5, 38);
  wv v669_out = rt_s(&S13);
  RT_AT(5, 39);
  wv v670_k = rt_i(0);
  RT_AT(5, 40);
  while (rt_cond((RT_AT(5, 40), rt_lt(v670_k, v668_count)))) {
    RT_AT(5, 41);
    v669_out = rt_add(v669_out, v667_s);
    RT_AT(5, 42);
    v670_k = rt_add(v670_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 44), v669_out));
}

static wv w6_utf8Length(wv v671_s) {
  rt_enter("utf8Length");
  v671_s = rt_bind(v671_s, 0);
  RT_AT(5, 49);
  wv v672_cs = rt_bind(std_split(v671_s, rt_s(&S13)), 0);
  RT_AT(5, 50);
  wv v673_n = rt_i(0);
  RT_AT(5, 51);
  wv v674_k = rt_i(0);
  RT_AT(5, 52);
  while (rt_cond((RT_AT(5, 52), ({ wv t1250 = v674_k; wv t1251 = std_length(v672_cs); rt_lt(t1250, t1251); })))) {
    RT_AT(5, 53);
    wv v675_c = rt_bind(std_code(rt_idx(v674_k, v672_cs)), 0);
    RT_AT(5, 54);
    wv t1252 = (rt_cond(rt_lt(v675_c, rt_i(128))) ? rt_i(1) : (rt_cond(rt_lt(v675_c, rt_i(2048))) ? rt_i(2) : (rt_cond(rt_lt(v675_c, rt_i(65536))) ? rt_i(3) : rt_i(4))));
    v673_n = rt_add(v673_n, t1252);
    RT_AT(5, 55);
    v674_k = rt_add(v674_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 57), v673_n));
}

static wv w6_cString(wv v676_s) {
  rt_enter("cString");
  v676_s = rt_bind(v676_s, 0);
  RT_AT(5, 63);
  wv v677_cs = rt_bind(std_split(v676_s, rt_s(&S13)), 0);
  RT_AT(5, 64);
  wv v678_out = rt_s(&S18);
  RT_AT(5, 65);
  wv v679_k = rt_i(0);
  RT_AT(5, 66);
  while (rt_cond((RT_AT(5, 66), ({ wv t1253 = v679_k; wv t1254 = std_length(v677_cs); rt_lt(t1253, t1254); })))) {
    RT_AT(5, 67);
    wv v680_ch = rt_bind(rt_idx(v679_k, v677_cs), 0);
    RT_AT(5, 68);
    wv v681_c = rt_bind(std_code(v680_ch), 0);
    RT_AT(5, 69);
    if (rt_cond(rt_eq(v680_ch, rt_s(&S18)))) {
      RT_AT(5, 70);
      v678_out = rt_add(v678_out, rt_s(&S531));
    } else if (rt_cond(rt_eq(v680_ch, rt_s(&S19)))) {
      RT_AT(5, 72);
      v678_out = rt_add(v678_out, rt_s(&S532));
    } else if (rt_cond(rt_eq(v680_ch, rt_s(&S2)))) {
      RT_AT(5, 74);
      v678_out = rt_add(v678_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v680_ch, rt_s(&S1)))) {
      RT_AT(5, 76);
      v678_out = rt_add(v678_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v680_ch, rt_s(&S3)))) {
      RT_AT(5, 78);
      v678_out = rt_add(v678_out, rt_s(&S21));
    } else if (rt_cond(rt_b(rt_bool(rt_lt(v681_c, rt_i(32)), "||") || rt_bool(rt_eq(v681_c, rt_i(127)), "||")))) {
      RT_AT(5, 80);
      v678_out = ({ wv t1258 = v678_out; wv t1259 = ({ wv t1255 = rt_text(std_floor(rt_div(v681_c, rt_i(64)))); wv t1256 = rt_text(rt_mod(std_floor(rt_div(v681_c, rt_i(8))), rt_i(8))); wv t1257 = rt_text(rt_mod(v681_c, rt_i(8))); rt_cat(4, (wv[]){ rt_s(&S19), t1255, t1256, t1257 }); }); rt_add(t1258, t1259); });
    } else {
      RT_AT(5, 82);
      v678_out = rt_add(v678_out, v680_ch);
    }
    RT_AT(5, 84);
    v679_k = rt_add(v679_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 86), rt_add(v678_out, rt_s(&S18))));
}

static wv w6_wstrInit(wv v682_s) {
  rt_enter("wstrInit");
  v682_s = rt_bind(v682_s, 0);
  RT_AT(5, 91);
  wv v683_bytes = rt_bind(rt_chk(w6_utf8Length(v682_s), "wstrInit"), 0);
  RT_AT(5, 92);
  wv v684_cps = rt_bind(std_length(v682_s), 0);
  RT_RET((RT_AT(5, 93), ({ wv t1260 = rt_text(v683_bytes); wv t1261 = rt_text(v684_cps); wv t1262 = rt_text((rt_cond(rt_eq(v683_bytes, v684_cps)) ? rt_i(1) : rt_i(0))); wv t1263 = rt_text(rt_chk(w6_cString(v682_s), "wstrInit")); rt_cat(9, (wv[]){ rt_s(&S533), t1260, rt_s(&S437), t1261, rt_s(&S437), t1262, rt_s(&S534), t1263, rt_s(&S535) }); })));
}

static wv w6_values(wv v685_cs) {
  rt_enter("values");
  RT_RET((RT_AT(5, 97), (rt_cond(rt_eq(std_length(v685_cs), rt_i(0))) ? rt_s(&S536) : rt_cat(3, (wv[]){ rt_s(&S537), rt_text(std_join(v685_cs, rt_s(&S437))), rt_s(&S535) }))));
}

static wv w6_generateC(wv p_v686_modules, wv p_v687_runtime) {
  rt_enter("generateC");
  struct fr146 fr = { 0 };
  fr.v686_modules = rt_bind(p_v686_modules, 0);
  fr.v687_runtime = rt_bind(p_v687_runtime, 0);
  RT_AT(5, 101);
  fr.v688_n = rt_i(0);
  RT_AT(5, 102);
  fr.v689_tmps = rt_i(0);
  RT_AT(5, 103);
  fr.v690_strings = rt_arr(0, NULL, 1);
  RT_AT(5, 104);
  fr.v691_fields = rt_arr(0, NULL, 1);
  RT_AT(5, 105);
  fr.v692_shapes = rt_arr(0, NULL, 1);
  RT_AT(5, 106);
  fr.v693_enumDefs = rt_arr(0, NULL, 1);
  RT_AT(5, 107);
  fr.v694_globals = rt_arr(0, NULL, 1);
  RT_AT(5, 108);
  fr.v695_frames = rt_arr(0, NULL, 1);
  RT_AT(5, 109);
  fr.v696_functions = rt_arr(0, NULL, 1);
  RT_AT(5, 110);
  fr.v697_out = rt_arr(0, NULL, 1);
  RT_AT(5, 111);
  fr.v698_indent = rt_i(1);
  RT_AT(5, 113);
  fr.v699_loops = rt_i(0);
  RT_AT(5, 114);
  fr.v700_iife = rt_b(0);
  RT_AT(5, 120);
  fr.v701_decls = rt_arr(0, NULL, 1);
  RT_AT(5, 121);
  fr.v702_fns = rt_arr(0, NULL, 1);
  RT_AT(5, 123);
  fr.v703_tops = rt_arr(0, NULL, 1);
  RT_AT(5, 124);
  fr.v704_slots = rt_arr(0, NULL, 1);
  RT_AT(5, 128);
  fr.v705_curMod = rt_i(0);
  RT_AT(5, 129);
  fr.v706_mod = rt_s(&S13);
  RT_AT(5, 130);
  fr.v707_curSlot = rt_arr(0, NULL, 1);
  RT_AT(5, 131);
  fr.v708_stolen = rt_arr(0, NULL, 1);
  RT_AT(5, 132);
  fr.v709_ownEnums = rt_arr(0, NULL, 1);
  RT_AT(5, 133);
  fr.v710_cur = rt_i(0);
  RT_AT(5, 135);
  fr.v711_lineStarts = rt_arr(1, (wv[]){ rt_i(0) }, 1);
  RT_AT(5, 1220);
  wv v776_mi = rt_i(0);
  RT_AT(5, 1221);
  while (rt_cond((RT_AT(5, 1221), ({ wv t2329 = v776_mi; wv t2330 = std_length(fr.v686_modules); rt_lt(t2329, t2330); })))) {
    RT_AT(5, 1222);
    (void)rt_chk(s728_analyze(&fr, v776_mi), "generateC");
    RT_AT(5, 1223);
    v776_mi = rt_add(v776_mi, rt_i(1));
  }
  RT_AT(5, 1225);
  v776_mi = rt_i(0);
  RT_AT(5, 1226);
  while (rt_cond((RT_AT(5, 1226), ({ wv t2331 = v776_mi; wv t2332 = std_length(fr.v686_modules); rt_lt(t2331, t2332); })))) {
    RT_AT(5, 1227);
    wv v1086_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v776_mi, fr.v686_modules))), 0);
    RT_AT(5, 1228);
    wv v1087_li = rt_i(0);
    RT_AT(5, 1229);
    while (rt_cond((RT_AT(5, 1229), ({ wv t2333 = v1087_li; wv t2334 = std_length(v1086_list); rt_lt(t2333, t2334); })))) {
      RT_AT(5, 1230);
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1087_li, v1086_list)), rt_s(&S47)))) {
        RT_AT(5, 1230);
        (void)({ wv t2335 = rt_idx(v1087_li, v1086_list); wv t2336 = rt_chk(s714_modId(&fr, v776_mi), "generateC"); rt_chk(s734_enumDef(&fr, t2335, t2336), "generateC"); });
      }
      RT_AT(5, 1231);
      v1087_li = rt_add(v1087_li, rt_i(1));
    }
    RT_AT(5, 1233);
    v776_mi = rt_add(v776_mi, rt_i(1));
  }
  RT_AT(5, 1235);
  v776_mi = rt_i(0);
  RT_AT(5, 1236);
  while (rt_cond((RT_AT(5, 1236), ({ wv t2337 = v776_mi; wv t2338 = std_length(fr.v686_modules); rt_lt(t2337, t2338); })))) {
    RT_AT(5, 1237);
    (void)rt_chk(s729_enter(&fr, v776_mi), "generateC");
    RT_AT(5, 1238);
    (void)rt_chk(s743_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v776_mi, fr.v686_modules)))), "generateC");
    RT_AT(5, 1239);
    v776_mi = rt_add(v776_mi, rt_i(1));
  }
  RT_RET((RT_AT(5, 1241), rt_chk(s775_assemble(&fr), "generateC")));
}

static wv s712_atLine(struct fr146 *up, wv v777_pos) {
  rt_enter("atLine");
  v777_pos = rt_bind(v777_pos, 0);
  RT_AT(5, 141);
  wv v778_lo = rt_i(0);
  RT_AT(5, 142);
  wv v779_hi = rt_sub(std_length(rt_def(up->v711_lineStarts, "lineStarts")), rt_i(1));
  RT_AT(5, 143);
  while (rt_cond((RT_AT(5, 143), rt_lt(v778_lo, v779_hi)))) {
    RT_AT(5, 144);
    wv v780_mid = rt_bind(std_floor(rt_div(rt_add(rt_add(v778_lo, v779_hi), rt_i(1)), rt_i(2))), 0);
    RT_AT(5, 145);
    if (rt_cond(({ wv t1266 = ({ wv t1264 = v780_mid; wv t1265 = rt_def(up->v711_lineStarts, "lineStarts"); rt_idx(t1264, t1265); }); wv t1267 = v777_pos; rt_le(t1266, t1267); }))) {
      RT_AT(5, 145);
      v778_lo = rt_bind(v780_mid, 1);
    } else {
      RT_AT(5, 145);
      v779_hi = rt_sub(v780_mid, rt_i(1));
    }
  }
  RT_RET((RT_AT(5, 147), ({ wv t1268 = rt_text(rt_def(up->v705_curMod, "curMod")); wv t1269 = rt_text(rt_add(v778_lo, rt_i(1))); rt_cat(5, (wv[]){ rt_s(&S538), t1268, rt_s(&S437), t1269, rt_s(&S16) }); })));
}

static wv s713_atExpr(struct fr146 *up, wv v781_pos, wv v782_c) {
  rt_enter("atExpr");
  v781_pos = rt_bind(v781_pos, 0);
  v782_c = rt_bind(v782_c, 0);
  RT_RET((RT_AT(5, 150), ({ wv t1270 = rt_text(rt_chk(s712_atLine(up, v781_pos), "atExpr")); wv t1271 = rt_text(v782_c); rt_cat(5, (wv[]){ rt_s(&S98), t1270, rt_s(&S437), t1271, rt_s(&S16) }); })));
}

static wv s714_modId(struct fr146 *up, wv v783_m) {
  rt_enter("modId");
  v783_m = rt_bind(v783_m, 0);
  RT_RET((RT_AT(5, 153), (rt_cond(({ wv t1272 = v783_m; wv t1273 = rt_sub(std_length(up->v686_modules), rt_i(1)); rt_eq(t1272, t1273); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v783_m, rt_i(1))) }))));
}

static wv s715_hasFrame(struct fr146 *up, wv v784_f) {
  rt_enter("hasFrame");
  v784_f = rt_bind(v784_f, 0);
  RT_RET((RT_AT(5, 155), rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1274 = v784_f; wv t1275 = rt_def(up->v702_fns, "fns"); rt_idx(t1274, t1275); }))), "&&") && rt_bool(rt_gt(rt_get(&F_children, ({ wv t1276 = v784_f; wv t1277 = rt_def(up->v702_fns, "fns"); rt_idx(t1276, t1277); })), rt_i(0)), "&&"))));
}

static wv s716_newScope(struct fr146 *up, wv v785_parent) {
  rt_enter("newScope");
  v785_parent = rt_bind(v785_parent, 0);
  RT_AT(5, 160);
  wv v786_names = rt_arr(0, NULL, 1);
  RT_RET((RT_AT(5, 161), rt_rec(0, 2, K41, (wv[]){ v785_parent, v786_names })));
}

static wv s717_put(struct fr146 *up, wv v787_scope, wv v788_name, wv v789_kind, wv v790_d) {
  rt_enter("put");
  v787_scope = rt_bind(v787_scope, 0);
  v788_name = rt_bind(v788_name, 0);
  v789_kind = rt_bind(v789_kind, 0);
  v790_d = rt_bind(v790_d, 0);
  RT_AT(5, 163);
  (void)({ wv t1278 = rt_get(&F_names, v787_scope); wv t1279 = rt_rec(0, 3, K50, (wv[]){ v788_name, v789_kind, v790_d }); std_push(t1278, t1279); });
  RT_RET(RT_NULL);
}

static wv s718_find(struct fr146 *up, wv v791_scope, wv v792_name) {
  rt_enter("find");
  v792_name = rt_bind(v792_name, 0);
  RT_AT(5, 166);
  wv v793_s = v791_scope;
  RT_AT(5, 167);
  while (rt_cond((RT_AT(5, 167), rt_ne(v793_s, RT_NULL)))) {
    RT_AT(5, 168);
    wv v794_ns = rt_get(&F_names, v793_s);
    RT_AT(5, 169);
    wv v795_k = rt_i(0);
    RT_AT(5, 170);
    while (rt_cond((RT_AT(5, 170), ({ wv t1280 = v795_k; wv t1281 = std_length(v794_ns); rt_lt(t1280, t1281); })))) {
      RT_AT(5, 171);
      if (rt_cond(({ wv t1282 = rt_get(&F_name, rt_idx(v795_k, v794_ns)); wv t1283 = v792_name; rt_eq(t1282, t1283); }))) {
        RT_AT(5, 171);
        RT_RET(rt_idx(v795_k, v794_ns));
      }
      RT_AT(5, 172);
      v795_k = rt_add(v795_k, rt_i(1));
    }
    RT_AT(5, 174);
    v793_s = rt_get(&F_parent, v793_s);
  }
  RT_RET((RT_AT(5, 176), RT_NULL));
}

static wv s719_kindOf(struct fr146 *up, wv v796_scope, wv v797_name) {
  rt_enter("kindOf");
  v796_scope = rt_bind(v796_scope, 0);
  v797_name = rt_bind(v797_name, 0);
  RT_AT(5, 179);
  wv v798_entry = rt_bind(rt_chk(s718_find(up, v796_scope, v797_name), "kindOf"), 0);
  RT_RET((RT_AT(5, 180), (rt_cond(rt_eq(v798_entry, RT_NULL)) ? RT_NULL : rt_get(&F_kind, v798_entry))));
}

static wv s720_newFn(struct fr146 *up, wv v799_name, wv v800_parent) {
  rt_enter("newFn");
  v799_name = rt_bind(v799_name, 0);
  v800_parent = rt_bind(v800_parent, 0);
  RT_AT(5, 184);
  wv v801_f = ({ wv t1286 = v799_name; wv t1287 = v800_parent; wv t1288 = (rt_cond(rt_eq(v800_parent, RT_NULL)) ? rt_i(0) : rt_add(rt_get(&F_depth, ({ wv t1284 = v800_parent; wv t1285 = rt_def(up->v702_fns, "fns"); rt_idx(t1284, t1285); })), rt_i(1))); wv t1289 = rt_eq(v800_parent, RT_NULL); rt_rec(1, 6, K51, (wv[]){ t1286, t1287, t1288, t1289, rt_i(0), rt_s(&S13) }); });
  RT_AT(5, 188);
  (void)({ wv t1290 = rt_def(up->v702_fns, "fns"); wv t1291 = v801_f; std_push(t1290, t1291); });
  RT_RET((RT_AT(5, 189), rt_sub(std_length(rt_def(up->v702_fns, "fns")), rt_i(1))));
}

static wv s721_bind(struct fr146 *up, wv v802_kind, wv v803_name, wv v804_f, wv v805_atTop, wv v806_mode) {
  rt_enter("bind");
  v802_kind = rt_bind(v802_kind, 0);
  v803_name = rt_bind(v803_name, 0);
  v804_f = rt_bind(v804_f, 0);
  v805_atTop = rt_bind(v805_atTop, 0);
  v806_mode = rt_bind(v806_mode, 0);
  RT_AT(5, 195);
  wv v807_cname = ({ wv t1292 = rt_text(rt_def(up->v706_mod, "mod")); wv t1293 = rt_text(v803_name); rt_cat(4, (wv[]){ rt_s(&S539), t1292, rt_s(&S12), t1293 }); });
  RT_AT(5, 196);
  if (rt_cond(rt_not(v805_atTop))) {
    RT_AT(5, 197);
    v807_cname = ({ wv t1296 = (rt_cond(rt_eq(v802_kind, rt_s(&S33))) ? rt_s(&S328) : rt_s(&S540)); wv t1297 = ({ wv t1294 = rt_text(rt_def(up->v688_n, "n")); wv t1295 = rt_text(v803_name); rt_cat(3, (wv[]){ t1294, rt_s(&S12), t1295 }); }); rt_add(t1296, t1297); });
    RT_AT(5, 198);
    up->v688_n = rt_add(rt_def(up->v688_n, "n"), rt_i(1));
  }
  RT_AT(5, 200);
  wv v808_d = ({ wv t1298 = v802_kind; wv t1299 = v803_name; wv t1300 = v804_f; wv t1301 = v807_cname; wv t1302 = rt_eq(v806_mode, rt_s(&S37)); rt_rec(1, 7, K52, (wv[]){ t1298, t1299, t1300, t1301, t1302, rt_b(0), RT_NULL }); });
  RT_AT(5, 201);
  (void)({ wv t1303 = rt_def(up->v701_decls, "decls"); wv t1304 = v808_d; std_push(t1303, t1304); });
  RT_AT(5, 202);
  if (rt_cond(rt_b(rt_bool(rt_get(&F_top, ({ wv t1305 = v804_f; wv t1306 = rt_def(up->v702_fns, "fns"); rt_idx(t1305, t1306); })), "&&") && rt_bool(rt_ne(v802_kind, rt_s(&S33)), "&&")))) {
    RT_AT(5, 202);
    (void)({ wv t1307 = rt_def(up->v694_globals, "globals"); wv t1308 = v807_cname; std_push(t1307, t1308); });
  }
  RT_RET((RT_AT(5, 203), rt_sub(std_length(rt_def(up->v701_decls, "decls")), rt_i(1))));
}

static wv s722_ablock(struct fr146 *up, wv v809_b, wv v810_scope, wv v811_f, wv v812_topScope) {
  rt_enter("ablock");
  v810_scope = rt_bind(v810_scope, 0);
  v811_f = rt_bind(v811_f, 0);
  v812_topScope = rt_bind(v812_topScope, 0);
  RT_AT(5, 207);
  wv v813_atTop = rt_eq(v810_scope, v812_topScope);
  RT_AT(5, 208);
  wv v814_list = rt_bind(rt_get(&F_stmts, v809_b), 0);
  RT_AT(5, 209);
  wv v815_k = rt_i(0);
  RT_AT(5, 210);
  while (rt_cond((RT_AT(5, 210), ({ wv t1309 = v815_k; wv t1310 = std_length(v814_list); rt_lt(t1309, t1310); })))) {
    RT_AT(5, 211);
    wv v816_s = rt_bind(rt_idx(v815_k, v814_list), 0);
    RT_AT(5, 212);
    if (rt_cond(rt_eq(rt_get(&F_k, v816_s), rt_s(&S33)))) {
      RT_AT(5, 213);
      wv v817_d = rt_bind(({ wv t1311 = rt_get(&F_name, v816_s); wv t1312 = v811_f; wv t1313 = v813_atTop; rt_chk(s721_bind(up, rt_s(&S33), t1311, t1312, t1313, rt_s(&S13)), "ablock"); }), 0);
      RT_AT(5, 214);
      wv v818_child = rt_bind(({ wv t1314 = rt_get(&F_name, v816_s); wv t1315 = v811_f; rt_chk(s720_newFn(up, t1314, t1315), "ablock"); }), 0);
      RT_AT(5, 215);
      ({ wv t1320 = rt_get(&F_cname, ({ wv t1316 = v817_d; wv t1317 = rt_def(up->v701_decls, "decls"); rt_idx(t1316, t1317); })); wv t1321 = ({ wv t1318 = v818_child; wv t1319 = rt_def(up->v702_fns, "fns"); rt_idx(t1318, t1319); }); rt_set_mem(t1320, &F_cname, t1321); });
      RT_AT(5, 216);
      ({ wv t1324 = v818_child; wv t1325 = ({ wv t1322 = v817_d; wv t1323 = rt_def(up->v701_decls, "decls"); rt_idx(t1322, t1323); }); rt_set_mem(t1324, &F_fnIndex, t1325); });
      RT_AT(5, 217);
      rt_upd_mem(rt_i(1), &F_children, ({ wv t1326 = v811_f; wv t1327 = rt_def(up->v702_fns, "fns"); rt_idx(t1326, t1327); }), rt_add);
      RT_AT(5, 218);
      ({ wv t1328 = v817_d; wv t1329 = rt_get(&F_namePos, v816_s); wv t1330 = rt_def(up->v707_curSlot, "curSlot"); rt_set_idx(t1328, t1329, t1330); });
      RT_AT(5, 219);
      (void)({ wv t1331 = v810_scope; wv t1332 = rt_get(&F_name, v816_s); wv t1333 = v817_d; rt_chk(s717_put(up, t1331, t1332, rt_s(&S33), t1333), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v816_s), rt_s(&S47)))) {
      RT_AT(5, 221);
      (void)({ wv t1334 = v810_scope; wv t1335 = rt_get(&F_name, v816_s); rt_chk(s717_put(up, t1334, t1335, rt_s(&S47), RT_NULL), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v816_s), rt_s(&S179)))) {
      RT_AT(5, 223);
      wv v819_d = rt_bind(({ wv t1336 = rt_get(&F_name, v816_s); wv t1337 = v811_f; wv t1338 = v813_atTop; wv t1339 = rt_chk(w6_modeOf(v816_s), "ablock"); rt_chk(s721_bind(up, rt_s(&S179), t1336, t1337, t1338, t1339), "ablock"); }), 0);
      RT_AT(5, 224);
      ({ wv t1340 = v819_d; wv t1341 = rt_get(&F_namePos, v816_s); wv t1342 = rt_def(up->v707_curSlot, "curSlot"); rt_set_idx(t1340, t1341, t1342); });
      RT_AT(5, 225);
      (void)({ wv t1343 = v810_scope; wv t1344 = rt_get(&F_name, v816_s); wv t1345 = v819_d; rt_chk(s717_put(up, t1343, t1344, rt_s(&S179), t1345), "ablock"); });
    }
    RT_AT(5, 227);
    v815_k = rt_add(v815_k, rt_i(1));
  }
  RT_AT(5, 229);
  v815_k = rt_i(0);
  RT_AT(5, 230);
  while (rt_cond((RT_AT(5, 230), ({ wv t1346 = v815_k; wv t1347 = std_length(v814_list); rt_lt(t1346, t1347); })))) {
    RT_AT(5, 231);
    (void)({ wv t1348 = rt_idx(v815_k, v814_list); wv t1349 = v810_scope; wv t1350 = v811_f; wv t1351 = v812_topScope; rt_chk(s723_astmt(up, t1348, t1349, t1350, t1351), "ablock"); });
    RT_AT(5, 232);
    v815_k = rt_add(v815_k, rt_i(1));
  }
  RT_AT(5, 234);
  if (rt_cond(rt_ne(rt_get(&F_value, v809_b), RT_NULL))) {
    RT_AT(5, 234);
    (void)({ wv t1352 = rt_get(&F_value, v809_b); wv t1353 = v810_scope; wv t1354 = v811_f; rt_chk(s727_aexpr(up, t1352, t1353, t1354), "ablock"); });
  }
  RT_RET(RT_NULL);
}

static wv s723_astmt(struct fr146 *up, wv v820_s, wv v821_scope, wv v822_f, wv v823_topScope) {
  rt_enter("astmt");
  v821_scope = rt_bind(v821_scope, 0);
  v822_f = rt_bind(v822_f, 0);
  v823_topScope = rt_bind(v823_topScope, 0);
  RT_AT(5, 238);
  wv v824_kind = rt_bind(rt_get(&F_k, v820_s), 0);
  RT_AT(5, 239);
  if (rt_cond(rt_eq(v824_kind, rt_s(&S33)))) {
    RT_AT(5, 240);
    wv v825_d = rt_bind(({ wv t1355 = rt_get(&F_namePos, v820_s); wv t1356 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1355, t1356); }), 0);
    RT_AT(5, 241);
    wv v826_child = rt_bind(rt_get(&F_fnIndex, ({ wv t1357 = v825_d; wv t1358 = rt_def(up->v701_decls, "decls"); rt_idx(t1357, t1358); })), 0);
    RT_AT(5, 242);
    wv v827_ps = rt_bind(rt_chk(s716_newScope(up, v821_scope), "astmt"), 0);
    RT_AT(5, 243);
    wv v828_k = rt_i(0);
    RT_AT(5, 244);
    while (rt_cond((RT_AT(5, 244), ({ wv t1359 = v828_k; wv t1360 = std_length(rt_get(&F_params, v820_s)); rt_lt(t1359, t1360); })))) {
      RT_AT(5, 245);
      wv v829_p = rt_bind(({ wv t1361 = v828_k; wv t1362 = rt_get(&F_params, v820_s); rt_idx(t1361, t1362); }), 0);
      RT_AT(5, 246);
      wv v830_pd = rt_bind(({ wv t1363 = rt_get(&F_name, v829_p); wv t1364 = v826_child; wv t1365 = rt_chk(w6_modeOf(v829_p), "astmt"); rt_chk(s721_bind(up, rt_s(&S419), t1363, t1364, rt_b(0), t1365), "astmt"); }), 0);
      RT_AT(5, 247);
      ({ wv t1366 = v830_pd; wv t1367 = rt_get(&F_pos, v829_p); wv t1368 = rt_def(up->v707_curSlot, "curSlot"); rt_set_idx(t1366, t1367, t1368); });
      RT_AT(5, 248);
      (void)({ wv t1369 = v827_ps; wv t1370 = rt_get(&F_name, v829_p); wv t1371 = v830_pd; rt_chk(s717_put(up, t1369, t1370, rt_s(&S419), t1371), "astmt"); });
      RT_AT(5, 249);
      v828_k = rt_add(v828_k, rt_i(1));
    }
    RT_AT(5, 251);
    (void)({ wv t1372 = rt_get(&F_body, v820_s); wv t1373 = rt_chk(s716_newScope(up, v827_ps), "astmt"); wv t1374 = v826_child; wv t1375 = v823_topScope; rt_chk(s722_ablock(up, t1372, t1373, t1374, t1375), "astmt"); });
  } else if (rt_cond(rt_eq(v824_kind, rt_s(&S41)))) {
    RT_AT(5, 253);
    (void)({ wv t1376 = rt_get(&F_cond, v820_s); wv t1377 = v821_scope; wv t1378 = v822_f; rt_chk(s727_aexpr(up, t1376, t1377, t1378), "astmt"); });
    RT_AT(5, 254);
    (void)({ wv t1379 = rt_get(&F_body, v820_s); wv t1380 = rt_chk(s716_newScope(up, v821_scope), "astmt"); wv t1381 = v822_f; wv t1382 = v823_topScope; rt_chk(s722_ablock(up, t1379, t1380, t1381, t1382), "astmt"); });
  } else if (rt_cond(rt_eq(v824_kind, rt_s(&S42)))) {
    RT_AT(5, 256);
    if (rt_cond(rt_eq(rt_get(&F_coll, v820_s), RT_NULL))) {
      RT_AT(5, 257);
      (void)({ wv t1383 = rt_get(&F_from, v820_s); wv t1384 = v821_scope; wv t1385 = v822_f; rt_chk(s727_aexpr(up, t1383, t1384, t1385), "astmt"); });
      RT_AT(5, 258);
      (void)({ wv t1386 = rt_get(&F_to, v820_s); wv t1387 = v821_scope; wv t1388 = v822_f; rt_chk(s727_aexpr(up, t1386, t1387, t1388), "astmt"); });
      RT_AT(5, 259);
      if (rt_cond(rt_ne(rt_get(&F_step, v820_s), RT_NULL))) {
        RT_AT(5, 259);
        (void)({ wv t1389 = rt_get(&F_step, v820_s); wv t1390 = v821_scope; wv t1391 = v822_f; rt_chk(s727_aexpr(up, t1389, t1390, t1391), "astmt"); });
      }
    } else if (rt_cond(rt_not(({ wv t1392 = rt_get(&F_coll, v820_s); wv t1393 = v821_scope; rt_chk(s726_notValue(up, t1392, t1393), "astmt"); })))) {
      RT_AT(5, 261);
      (void)({ wv t1394 = rt_get(&F_coll, v820_s); wv t1395 = v821_scope; wv t1396 = v822_f; rt_chk(s727_aexpr(up, t1394, t1395, t1396), "astmt"); });
    }
    RT_AT(5, 264);
    wv v831_ls = rt_bind(rt_chk(s716_newScope(up, v821_scope), "astmt"), 0);
    RT_AT(5, 265);
    wv v832_k = rt_i(0);
    RT_AT(5, 266);
    while (rt_cond((RT_AT(5, 266), ({ wv t1397 = v832_k; wv t1398 = std_length(rt_get(&F_names, v820_s)); rt_lt(t1397, t1398); })))) {
      RT_AT(5, 267);
      wv v833_nm = rt_bind(({ wv t1399 = v832_k; wv t1400 = rt_get(&F_names, v820_s); rt_idx(t1399, t1400); }), 0);
      RT_AT(5, 268);
      wv v834_d = rt_bind(({ wv t1401 = rt_get(&F_name, v833_nm); wv t1402 = v822_f; rt_chk(s721_bind(up, rt_s(&S179), t1401, t1402, rt_b(0), rt_s(&S37)), "astmt"); }), 0);
      RT_AT(5, 269);
      ({ wv t1403 = v834_d; wv t1404 = rt_get(&F_pos, v833_nm); wv t1405 = rt_def(up->v707_curSlot, "curSlot"); rt_set_idx(t1403, t1404, t1405); });
      RT_AT(5, 270);
      (void)({ wv t1406 = v831_ls; wv t1407 = rt_get(&F_name, v833_nm); wv t1408 = v834_d; rt_chk(s717_put(up, t1406, t1407, rt_s(&S179), t1408), "astmt"); });
      RT_AT(5, 271);
      v832_k = rt_add(v832_k, rt_i(1));
    }
    RT_AT(5, 273);
    (void)({ wv t1409 = rt_get(&F_body, v820_s); wv t1410 = rt_chk(s716_newScope(up, v831_ls), "astmt"); wv t1411 = v822_f; wv t1412 = v823_topScope; rt_chk(s722_ablock(up, t1409, t1410, t1411, t1412), "astmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v824_kind, rt_s(&S34)), "||") || rt_bool(rt_eq(v824_kind, rt_s(&S58)), "||")), "||") || rt_bool(rt_eq(v824_kind, rt_s(&S55)), "||")), "||") || rt_bool(rt_eq(v824_kind, rt_s(&S179)), "||")))) {
    RT_AT(5, 275);
    if (rt_cond(rt_ne(rt_get(&F_e, v820_s), RT_NULL))) {
      RT_AT(5, 275);
      (void)({ wv t1413 = rt_get(&F_e, v820_s); wv t1414 = v821_scope; wv t1415 = v822_f; rt_chk(s727_aexpr(up, t1413, t1414, t1415), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v824_kind, rt_s(&S178)))) {
    RT_AT(5, 277);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v820_s)), rt_s(&S185)))) {
      RT_AT(5, 277);
      (void)({ wv t1416 = rt_get(&F_arg, rt_get(&F_e, v820_s)); wv t1417 = v821_scope; wv t1418 = v822_f; rt_chk(s727_aexpr(up, t1416, t1417, t1418), "astmt"); });
    } else {
      RT_AT(5, 277);
      (void)({ wv t1419 = rt_get(&F_e, v820_s); wv t1420 = v821_scope; wv t1421 = v822_f; rt_chk(s727_aexpr(up, t1419, t1420, t1421), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v824_kind, rt_s(&S38)))) {
    RT_AT(5, 279);
    (void)({ wv t1422 = rt_get(&F_e, v820_s); wv t1423 = v821_scope; wv t1424 = v822_f; rt_chk(s727_aexpr(up, t1422, t1423, t1424), "astmt"); });
    RT_AT(5, 280);
    (void)({ wv t1425 = rt_get(&F_target, v820_s); wv t1426 = v821_scope; wv t1427 = v822_f; rt_chk(s725_atarget(up, t1425, t1426, t1427), "astmt"); });
  }
  RT_AT(5, 282);
  if (rt_cond(rt_ne(rt_get(&F_handler, v820_s), RT_NULL))) {
    RT_AT(5, 283);
    wv v835_h = rt_bind(rt_get(&F_handler, v820_s), 0);
    RT_AT(5, 284);
    wv v836_hs = rt_bind(rt_chk(s716_newScope(up, v821_scope), "astmt"), 0);
    RT_AT(5, 285);
    if (rt_cond(rt_ne(rt_get(&F_param, v835_h), RT_NULL))) {
      RT_AT(5, 286);
      wv v837_wd = rt_bind(({ wv t1428 = rt_get(&F_param, v835_h); wv t1429 = v822_f; rt_chk(s721_bind(up, rt_s(&S57), t1428, t1429, rt_b(0), rt_s(&S13)), "astmt"); }), 0);
      RT_AT(5, 287);
      ({ wv t1430 = v837_wd; wv t1431 = rt_get(&F_paramPos, v835_h); wv t1432 = rt_def(up->v707_curSlot, "curSlot"); rt_set_idx(t1430, t1431, t1432); });
      RT_AT(5, 288);
      (void)({ wv t1433 = v836_hs; wv t1434 = rt_get(&F_param, v835_h); wv t1435 = v837_wd; rt_chk(s717_put(up, t1433, t1434, rt_s(&S57), t1435), "astmt"); });
    }
    RT_AT(5, 290);
    (void)({ wv t1436 = rt_get(&F_body, v835_h); wv t1437 = rt_chk(s716_newScope(up, v836_hs), "astmt"); wv t1438 = v822_f; wv t1439 = v823_topScope; rt_chk(s722_ablock(up, t1436, t1437, t1438, t1439), "astmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s724_use(struct fr146 *up, wv v838_x, wv v839_scope, wv v840_f) {
  rt_enter("use");
  v838_x = rt_bind(v838_x, 0);
  v839_scope = rt_bind(v839_scope, 0);
  v840_f = rt_bind(v840_f, 0);
  RT_AT(5, 296);
  wv v841_d = rt_bind(rt_get(&F_d, ({ wv t1440 = v839_scope; wv t1441 = rt_get(&F_name, v838_x); rt_chk(s718_find(up, t1440, t1441), "use"); })), 0);
  RT_AT(5, 297);
  ({ wv t1442 = v841_d; wv t1443 = rt_get(&F_pos, v838_x); wv t1444 = rt_def(up->v707_curSlot, "curSlot"); rt_set_idx(t1442, t1443, t1444); });
  RT_AT(5, 298);
  if (rt_cond(({ wv t1447 = rt_get(&F_func, ({ wv t1445 = v841_d; wv t1446 = rt_def(up->v701_decls, "decls"); rt_idx(t1445, t1446); })); wv t1448 = v840_f; rt_ne(t1447, t1448); }))) {
    RT_AT(5, 298);
    rt_set_mem(rt_b(1), &F_captured, ({ wv t1449 = v841_d; wv t1450 = rt_def(up->v701_decls, "decls"); rt_idx(t1449, t1450); }));
  }
  RT_RET(RT_NULL);
}

static wv s725_atarget(struct fr146 *up, wv v842_t, wv v843_scope, wv v844_f) {
  rt_enter("atarget");
  v842_t = rt_bind(v842_t, 0);
  v843_scope = rt_bind(v843_scope, 0);
  v844_f = rt_bind(v844_f, 0);
  RT_AT(5, 302);
  if (rt_cond(rt_eq(rt_get(&F_k, v842_t), rt_s(&S214)))) {
    RT_AT(5, 302);
    (void)rt_chk(s724_use(up, v842_t, v843_scope, v844_f), "atarget");
    RT_AT(5, 302);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 303);
  if (rt_cond(rt_eq(rt_get(&F_k, v842_t), rt_s(&S191)))) {
    RT_AT(5, 303);
    (void)({ wv t1451 = rt_get(&F_index, v842_t); wv t1452 = v843_scope; wv t1453 = v844_f; rt_chk(s727_aexpr(up, t1451, t1452, t1453), "atarget"); });
  }
  RT_AT(5, 304);
  (void)({ wv t1454 = (rt_cond(rt_eq(rt_get(&F_k, v842_t), rt_s(&S192))) ? rt_get(&F_rec, v842_t) : rt_get(&F_arr, v842_t)); wv t1455 = v843_scope; wv t1456 = v844_f; rt_chk(s727_aexpr(up, t1454, t1455, t1456), "atarget"); });
  RT_RET(RT_NULL);
}

static wv s726_notValue(struct fr146 *up, wv v845_x, wv v846_scope) {
  rt_enter("notValue");
  v846_scope = rt_bind(v846_scope, 0);
  RT_AT(5, 309);
  if (rt_cond(rt_eq(rt_get(&F_k, v845_x), rt_s(&S192)))) {
    RT_AT(5, 309);
    RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v845_x)), rt_s(&S214)), "&&") && rt_bool(rt_eq(({ wv t1457 = v846_scope; wv t1458 = rt_get(&F_name, rt_get(&F_rec, v845_x)); rt_chk(s719_kindOf(up, t1457, t1458), "notValue"); }), rt_s(&S440)), "&&")));
  }
  RT_AT(5, 310);
  if (rt_cond(rt_ne(rt_get(&F_k, v845_x), rt_s(&S214)))) {
    RT_AT(5, 310);
    RT_RET(rt_b(0));
  }
  RT_AT(5, 311);
  wv v847_kind = rt_bind(({ wv t1459 = v846_scope; wv t1460 = rt_get(&F_name, v845_x); rt_chk(s719_kindOf(up, t1459, t1460), "notValue"); }), 0);
  RT_RET((RT_AT(5, 312), rt_b(rt_bool(rt_eq(v847_kind, rt_s(&S47)), "||") || rt_bool(rt_eq(v847_kind, rt_s(&S440)), "||"))));
}

static wv s727_aexpr(struct fr146 *up, wv v848_e, wv v849_scope, wv v850_f) {
  rt_enter("aexpr");
  v849_scope = rt_bind(v849_scope, 0);
  v850_f = rt_bind(v850_f, 0);
  RT_AT(5, 316);
  wv v851_kind = rt_bind(rt_get(&F_k, v848_e), 0);
  RT_AT(5, 317);
  if (rt_cond(rt_eq(v851_kind, rt_s(&S173)))) {
    RT_AT(5, 318);
    wv v852_k = rt_i(0);
    RT_AT(5, 319);
    while (rt_cond((RT_AT(5, 319), ({ wv t1461 = v852_k; wv t1462 = std_length(rt_get(&F_parts, v848_e)); rt_lt(t1461, t1462); })))) {
      RT_AT(5, 320);
      if (rt_cond(rt_ne(std_typeOf(({ wv t1463 = v852_k; wv t1464 = rt_get(&F_parts, v848_e); rt_idx(t1463, t1464); })), rt_s(&S187)))) {
        RT_AT(5, 320);
        (void)({ wv t1467 = ({ wv t1465 = v852_k; wv t1466 = rt_get(&F_parts, v848_e); rt_idx(t1465, t1466); }); wv t1468 = v849_scope; wv t1469 = v850_f; rt_chk(s727_aexpr(up, t1467, t1468, t1469), "aexpr"); });
      }
      RT_AT(5, 321);
      v852_k = rt_add(v852_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S214)))) {
    RT_AT(5, 324);
    (void)rt_chk(s724_use(up, v848_e, v849_scope, v850_f), "aexpr");
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v851_kind, rt_s(&S188)), "||") || rt_bool(rt_eq(v851_kind, rt_s(&S189)), "||")))) {
    RT_AT(5, 326);
    wv v853_k = rt_i(0);
    RT_AT(5, 327);
    while (rt_cond((RT_AT(5, 327), ({ wv t1470 = v853_k; wv t1471 = std_length(rt_get(&F_items, v848_e)); rt_lt(t1470, t1471); })))) {
      RT_AT(5, 327);
      (void)({ wv t1474 = ({ wv t1472 = v853_k; wv t1473 = rt_get(&F_items, v848_e); rt_idx(t1472, t1473); }); wv t1475 = v849_scope; wv t1476 = v850_f; rt_chk(s727_aexpr(up, t1474, t1475, t1476), "aexpr"); });
      RT_AT(5, 327);
      v853_k = rt_add(v853_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S190)))) {
    RT_AT(5, 329);
    wv v854_k = rt_i(0);
    RT_AT(5, 330);
    while (rt_cond((RT_AT(5, 330), ({ wv t1477 = v854_k; wv t1478 = std_length(rt_get(&F_fields, v848_e)); rt_lt(t1477, t1478); })))) {
      RT_AT(5, 330);
      (void)({ wv t1481 = rt_get(&F_e, ({ wv t1479 = v854_k; wv t1480 = rt_get(&F_fields, v848_e); rt_idx(t1479, t1480); })); wv t1482 = v849_scope; wv t1483 = v850_f; rt_chk(s727_aexpr(up, t1481, t1482, t1483), "aexpr"); });
      RT_AT(5, 330);
      v854_k = rt_add(v854_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S191)))) {
    RT_AT(5, 332);
    (void)({ wv t1484 = rt_get(&F_index, v848_e); wv t1485 = v849_scope; wv t1486 = v850_f; rt_chk(s727_aexpr(up, t1484, t1485, t1486), "aexpr"); });
    RT_AT(5, 333);
    (void)({ wv t1487 = rt_get(&F_arr, v848_e); wv t1488 = v849_scope; wv t1489 = v850_f; rt_chk(s727_aexpr(up, t1487, t1488, t1489), "aexpr"); });
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S192)))) {
    RT_AT(5, 335);
    if (rt_cond(rt_not(({ wv t1490 = rt_get(&F_rec, v848_e); wv t1491 = v849_scope; rt_chk(s726_notValue(up, t1490, t1491), "aexpr"); })))) {
      RT_AT(5, 335);
      (void)({ wv t1492 = rt_get(&F_rec, v848_e); wv t1493 = v849_scope; wv t1494 = v850_f; rt_chk(s727_aexpr(up, t1492, t1493, t1494), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S193)))) {
    RT_AT(5, 337);
    (void)({ wv t1495 = rt_get(&F_e, v848_e); wv t1496 = v849_scope; wv t1497 = v850_f; rt_chk(s727_aexpr(up, t1495, t1496, t1497), "aexpr"); });
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S194)))) {
    RT_AT(5, 339);
    (void)({ wv t1498 = rt_get(&F_l, v848_e); wv t1499 = v849_scope; wv t1500 = v850_f; rt_chk(s727_aexpr(up, t1498, t1499, t1500), "aexpr"); });
    RT_AT(5, 340);
    (void)({ wv t1501 = rt_get(&F_r, v848_e); wv t1502 = v849_scope; wv t1503 = v850_f; rt_chk(s727_aexpr(up, t1501, t1502, t1503), "aexpr"); });
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S195)))) {
    RT_AT(5, 342);
    (void)({ wv t1504 = rt_get(&F_c, v848_e); wv t1505 = v849_scope; wv t1506 = v850_f; rt_chk(s727_aexpr(up, t1504, t1505, t1506), "aexpr"); });
    RT_AT(5, 343);
    (void)({ wv t1507 = rt_get(&F_a, v848_e); wv t1508 = v849_scope; wv t1509 = v850_f; rt_chk(s727_aexpr(up, t1507, t1508, t1509), "aexpr"); });
    RT_AT(5, 344);
    (void)({ wv t1510 = rt_get(&F_b, v848_e); wv t1511 = v849_scope; wv t1512 = v850_f; rt_chk(s727_aexpr(up, t1510, t1511, t1512), "aexpr"); });
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S184)))) {
    RT_AT(5, 346);
    wv v855_k = rt_i(0);
    RT_AT(5, 347);
    while (rt_cond((RT_AT(5, 347), ({ wv t1513 = v855_k; wv t1514 = std_length(rt_get(&F_args, v848_e)); rt_lt(t1513, t1514); })))) {
      RT_AT(5, 347);
      (void)({ wv t1517 = ({ wv t1515 = v855_k; wv t1516 = rt_get(&F_args, v848_e); rt_idx(t1515, t1516); }); wv t1518 = v849_scope; wv t1519 = v850_f; rt_chk(s727_aexpr(up, t1517, t1518, t1519), "aexpr"); });
      RT_AT(5, 347);
      v855_k = rt_add(v855_k, rt_i(1));
    }
    RT_AT(5, 348);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_callee, v848_e)), rt_s(&S214)))) {
      RT_AT(5, 348);
      ({ wv t1522 = rt_get(&F_d, ({ wv t1520 = v849_scope; wv t1521 = rt_get(&F_name, rt_get(&F_callee, v848_e)); rt_chk(s718_find(up, t1520, t1521), "aexpr"); })); wv t1523 = rt_get(&F_pos, rt_get(&F_callee, v848_e)); wv t1524 = rt_def(up->v707_curSlot, "curSlot"); rt_set_idx(t1522, t1523, t1524); });
    }
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S186)))) {
    RT_AT(5, 350);
    wv v856_k = rt_i(0);
    RT_AT(5, 351);
    while (rt_cond((RT_AT(5, 351), ({ wv t1525 = v856_k; wv t1526 = std_length(rt_get(&F_branches, v848_e)); rt_lt(t1525, t1526); })))) {
      RT_AT(5, 352);
      (void)({ wv t1529 = rt_get(&F_cond, ({ wv t1527 = v856_k; wv t1528 = rt_get(&F_branches, v848_e); rt_idx(t1527, t1528); })); wv t1530 = v849_scope; wv t1531 = v850_f; rt_chk(s727_aexpr(up, t1529, t1530, t1531), "aexpr"); });
      RT_AT(5, 353);
      (void)({ wv t1534 = rt_get(&F_body, ({ wv t1532 = v856_k; wv t1533 = rt_get(&F_branches, v848_e); rt_idx(t1532, t1533); })); wv t1535 = rt_chk(s716_newScope(up, v849_scope), "aexpr"); wv t1536 = v850_f; rt_chk(s722_ablock(up, t1534, t1535, t1536, RT_NULL), "aexpr"); });
      RT_AT(5, 354);
      v856_k = rt_add(v856_k, rt_i(1));
    }
    RT_AT(5, 356);
    if (rt_cond(rt_ne(rt_get(&F_els, v848_e), RT_NULL))) {
      RT_AT(5, 356);
      (void)({ wv t1537 = rt_get(&F_els, v848_e); wv t1538 = rt_chk(s716_newScope(up, v849_scope), "aexpr"); wv t1539 = v850_f; rt_chk(s722_ablock(up, t1537, t1538, t1539, RT_NULL), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v851_kind, rt_s(&S45)))) {
    RT_AT(5, 358);
    (void)({ wv t1540 = rt_get(&F_subject, v848_e); wv t1541 = v849_scope; wv t1542 = v850_f; rt_chk(s727_aexpr(up, t1540, t1541, t1542), "aexpr"); });
    RT_AT(5, 359);
    wv v857_k = rt_i(0);
    RT_AT(5, 360);
    while (rt_cond((RT_AT(5, 360), ({ wv t1543 = v857_k; wv t1544 = std_length(rt_get(&F_cases, v848_e)); rt_lt(t1543, t1544); })))) {
      RT_AT(5, 361);
      (void)({ wv t1547 = rt_get(&F_body, ({ wv t1545 = v857_k; wv t1546 = rt_get(&F_cases, v848_e); rt_idx(t1545, t1546); })); wv t1548 = rt_chk(s716_newScope(up, v849_scope), "aexpr"); wv t1549 = v850_f; rt_chk(s722_ablock(up, t1547, t1548, t1549, RT_NULL), "aexpr"); });
      RT_AT(5, 362);
      v857_k = rt_add(v857_k, rt_i(1));
    }
    RT_AT(5, 364);
    if (rt_cond(rt_ne(rt_get(&F_els, v848_e), RT_NULL))) {
      RT_AT(5, 364);
      (void)({ wv t1550 = rt_get(&F_els, v848_e); wv t1551 = rt_chk(s716_newScope(up, v849_scope), "aexpr"); wv t1552 = v850_f; rt_chk(s722_ablock(up, t1550, t1551, t1552, RT_NULL), "aexpr"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv s728_analyze(struct fr146 *up, wv v858_m) {
  rt_enter("analyze");
  v858_m = rt_bind(v858_m, 0);
  RT_AT(5, 369);
  wv t1553 = rt_bind(v858_m, 1);
  (void)rt_def(up->v705_curMod, "curMod");
  up->v705_curMod = t1553;
  RT_AT(5, 370);
  wv t1554 = rt_bind(rt_chk(s714_modId(up, v858_m), "analyze"), 1);
  (void)rt_def(up->v706_mod, "mod");
  up->v706_mod = t1554;
  RT_AT(5, 371);
  wv v859_top = rt_bind(rt_chk(s720_newFn(up, RT_NULL, RT_NULL), "analyze"), 0);
  RT_AT(5, 372);
  (void)({ wv t1555 = rt_def(up->v703_tops, "tops"); wv t1556 = v859_top; std_push(t1555, t1556); });
  RT_AT(5, 373);
  wv v860_slot = rt_arr(0, NULL, 1);
  RT_AT(5, 374);
  wv v861_i = rt_i(0);
  RT_AT(5, 375);
  while (rt_cond((RT_AT(5, 375), ({ wv t1557 = v861_i; wv t1558 = std_length(rt_get(&F_chars, rt_idx(v858_m, up->v686_modules))); rt_le(t1557, t1558); })))) {
    RT_AT(5, 376);
    (void)std_push(v860_slot, RT_NULL);
    RT_AT(5, 377);
    v861_i = rt_add(v861_i, rt_i(1));
  }
  RT_AT(5, 379);
  (void)({ wv t1559 = rt_def(up->v704_slots, "slots"); wv t1560 = v860_slot; std_push(t1559, t1560); });
  RT_AT(5, 380);
  wv t1561 = rt_bind(v860_slot, 1);
  (void)rt_def(up->v707_curSlot, "curSlot");
  up->v707_curSlot = t1561;
  RT_AT(5, 381);
  wv v862_scope = rt_bind(rt_chk(s716_newScope(up, RT_NULL), "analyze"), 0);
  RT_AT(5, 382);
  wv v863_imports = rt_bind(rt_get(&F_imports, rt_idx(v858_m, up->v686_modules)), 0);
  RT_AT(5, 383);
  wv v864_k = rt_i(0);
  RT_AT(5, 384);
  while (rt_cond((RT_AT(5, 384), ({ wv t1562 = v864_k; wv t1563 = std_length(v863_imports); rt_lt(t1562, t1563); })))) {
    RT_AT(5, 385);
    (void)({ wv t1564 = v862_scope; wv t1565 = rt_get(&F_name, rt_get(&F_node, rt_idx(v864_k, v863_imports))); rt_chk(s717_put(up, t1564, t1565, rt_s(&S440), RT_NULL), "analyze"); });
    RT_AT(5, 386);
    v864_k = rt_add(v864_k, rt_i(1));
  }
  RT_AT(5, 388);
  (void)({ wv t1566 = rt_rec(0, 2, K49, (wv[]){ rt_get(&F_stmts, rt_get(&F_program, rt_idx(v858_m, up->v686_modules))), RT_NULL }); wv t1567 = v862_scope; wv t1568 = v859_top; wv t1569 = v862_scope; rt_chk(s722_ablock(up, t1566, t1567, t1568, t1569), "analyze"); });
  RT_RET(RT_NULL);
}

static wv s729_enter(struct fr146 *up, wv v865_m) {
  rt_enter("enter");
  v865_m = rt_bind(v865_m, 0);
  RT_AT(5, 395);
  (void)({ wv t1570 = rt_get(&F_file, rt_idx(v865_m, up->v686_modules)); wv t1571 = rt_get(&F_chars, rt_idx(v865_m, up->v686_modules)); rt_chk(w2_setSource(t1570, t1571), "enter"); });
  RT_AT(5, 396);
  wv t1572 = rt_bind(v865_m, 1);
  (void)rt_def(up->v705_curMod, "curMod");
  up->v705_curMod = t1572;
  RT_AT(5, 397);
  wv t1573 = rt_bind(rt_chk(s714_modId(up, v865_m), "enter"), 1);
  (void)rt_def(up->v706_mod, "mod");
  up->v706_mod = t1573;
  RT_AT(5, 398);
  wv t1576 = rt_bind(({ wv t1574 = v865_m; wv t1575 = rt_def(up->v704_slots, "slots"); rt_idx(t1574, t1575); }), 1);
  (void)rt_def(up->v707_curSlot, "curSlot");
  up->v707_curSlot = t1576;
  RT_AT(5, 399);
  wv t1579 = rt_bind(({ wv t1577 = v865_m; wv t1578 = rt_def(up->v703_tops, "tops"); rt_idx(t1577, t1578); }), 1);
  (void)rt_def(up->v710_cur, "cur");
  up->v710_cur = t1579;
  RT_AT(5, 400);
  wv t1580 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v708_stolen, "stolen");
  up->v708_stolen = t1580;
  RT_AT(5, 401);
  wv t1581 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v709_ownEnums, "ownEnums");
  up->v709_ownEnums = t1581;
  RT_AT(5, 402);
  wv t1582 = rt_arr(1, (wv[]){ rt_i(0) }, 1);
  (void)rt_def(up->v711_lineStarts, "lineStarts");
  up->v711_lineStarts = t1582;
  RT_AT(5, 403);
  wv v866_chars = rt_bind(rt_get(&F_chars, rt_idx(v865_m, up->v686_modules)), 0);
  RT_AT(5, 404);
  {
    wfor t1584;
    rt_for_start(&t1584, v866_chars, 0);
    for (int64_t t1583 = 0; rt_for_guard(&t1584, v866_chars, "chars"), rt_for_more(&t1584, t1583); t1583++) {
      wv v870_i = rt_for_key(&t1584, t1583);
      wv v871_c = rt_for_val(&t1584, t1583);
      RT_AT(5, 405);
      if (rt_cond(rt_eq(v871_c, rt_s(&S2)))) {
        RT_AT(5, 405);
        (void)({ wv t1585 = rt_def(up->v711_lineStarts, "lineStarts"); wv t1586 = rt_add(v870_i, rt_i(1)); std_push(t1585, t1586); });
      }
    }
  }
  RT_AT(5, 407);
  wv v867_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v865_m, up->v686_modules))), 0);
  RT_AT(5, 408);
  wv v868_k = rt_i(0);
  RT_AT(5, 409);
  while (rt_cond((RT_AT(5, 409), ({ wv t1587 = v868_k; wv t1588 = std_length(v867_list); rt_lt(t1587, t1588); })))) {
    RT_AT(5, 410);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v868_k, v867_list)), rt_s(&S47)))) {
      RT_AT(5, 410);
      (void)({ wv t1589 = rt_def(up->v709_ownEnums, "ownEnums"); wv t1590 = rt_idx(v868_k, v867_list); std_push(t1589, t1590); });
    }
    RT_AT(5, 411);
    v868_k = rt_add(v868_k, rt_i(1));
  }
  RT_AT(5, 413);
  wv v869_imports = rt_bind(rt_get(&F_imports, rt_idx(v865_m, up->v686_modules)), 0);
  RT_AT(5, 414);
  v868_k = rt_i(0);
  RT_AT(5, 415);
  while (rt_cond((RT_AT(5, 415), ({ wv t1591 = v868_k; wv t1592 = std_length(v869_imports); rt_lt(t1591, t1592); })))) {
    RT_AT(5, 416);
    (void)({ wv t1595 = rt_def(up->v708_stolen, "stolen"); wv t1596 = ({ wv t1593 = rt_get(&F_name, rt_get(&F_node, rt_idx(v868_k, v869_imports))); wv t1594 = rt_get(&F_lib, rt_idx(v868_k, v869_imports)); rt_rec(0, 2, K53, (wv[]){ t1593, t1594 }); }); std_push(t1595, t1596); });
    RT_AT(5, 417);
    v868_k = rt_add(v868_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s730_emit(struct fr146 *up, wv v872_line) {
  rt_enter("emit");
  v872_line = rt_bind(v872_line, 0);
  RT_AT(5, 421);
  (void)({ wv t1599 = rt_def(up->v697_out, "out"); wv t1600 = ({ wv t1597 = rt_chk(w6_repeat(rt_s(&S541), rt_def(up->v698_indent, "indent")), "emit"); wv t1598 = v872_line; rt_add(t1597, t1598); }); std_push(t1599, t1600); });
  RT_RET(RT_NULL);
}

static wv s731_tmp(struct fr146 *up) {
  rt_enter("tmp");
  RT_AT(5, 423);
  wv v873_name = rt_cat(2, (wv[]){ rt_s(&S105), rt_text(rt_def(up->v689_tmps, "tmps")) });
  RT_AT(5, 424);
  up->v689_tmps = rt_add(rt_def(up->v689_tmps, "tmps"), rt_i(1));
  RT_RET((RT_AT(5, 425), v873_name));
}

static wv s732_str(struct fr146 *up, wv v874_s) {
  rt_enter("str");
  v874_s = rt_bind(v874_s, 0);
  RT_AT(5, 429);
  wv v875_k = rt_i(0);
  RT_AT(5, 430);
  while (rt_cond((RT_AT(5, 430), rt_b(rt_bool(({ wv t1601 = v875_k; wv t1602 = std_length(rt_def(up->v690_strings, "strings")); rt_lt(t1601, t1602); }), "&&") && rt_bool(({ wv t1605 = ({ wv t1603 = v875_k; wv t1604 = rt_def(up->v690_strings, "strings"); rt_idx(t1603, t1604); }); wv t1606 = v874_s; rt_ne(t1605, t1606); }), "&&"))))) {
    RT_AT(5, 430);
    v875_k = rt_add(v875_k, rt_i(1));
  }
  RT_AT(5, 431);
  if (rt_cond(({ wv t1607 = v875_k; wv t1608 = std_length(rt_def(up->v690_strings, "strings")); rt_eq(t1607, t1608); }))) {
    RT_AT(5, 431);
    (void)({ wv t1609 = rt_def(up->v690_strings, "strings"); wv t1610 = v874_s; std_push(t1609, t1610); });
  }
  RT_RET((RT_AT(5, 432), rt_cat(2, (wv[]){ rt_s(&S542), rt_text(v875_k) })));
}

static wv s733_fieldRef(struct fr146 *up, wv v876_name) {
  rt_enter("fieldRef");
  v876_name = rt_bind(v876_name, 0);
  RT_AT(5, 435);
  if (rt_cond(rt_not(({ wv t1611 = rt_def(up->v691_fields, "fields"); wv t1612 = v876_name; rt_chk(w1_contains(t1611, t1612), "fieldRef"); })))) {
    RT_AT(5, 435);
    (void)({ wv t1613 = rt_def(up->v691_fields, "fields"); wv t1614 = v876_name; std_push(t1613, t1614); });
  }
  RT_RET((RT_AT(5, 436), rt_cat(2, (wv[]){ rt_s(&S543), rt_text(v876_name) })));
}

static wv s734_enumDef(struct fr146 *up, wv v877_s, wv v878_m) {
  rt_enter("enumDef");
  v877_s = rt_bind(v877_s, 0);
  v878_m = rt_bind(v878_m, 0);
  RT_AT(5, 441);
  wv v879_nm = rt_bind(rt_chk(s732_str(up, rt_get(&F_name, v877_s)), "enumDef"), 0);
  RT_AT(5, 442);
  wv v880_members = rt_arr(0, NULL, 1);
  RT_AT(5, 443);
  wv v881_k = rt_i(0);
  RT_AT(5, 444);
  while (rt_cond((RT_AT(5, 444), ({ wv t1615 = v881_k; wv t1616 = std_length(rt_get(&F_members, v877_s)); rt_lt(t1615, t1616); })))) {
    RT_AT(5, 445);
    (void)({ wv t1621 = v880_members; wv t1622 = ({ wv t1619 = rt_text(v879_nm); wv t1620 = rt_text(rt_chk(s732_str(up, rt_get(&F_name, ({ wv t1617 = v881_k; wv t1618 = rt_get(&F_members, v877_s); rt_idx(t1617, t1618); }))), "enumDef")); rt_cat(5, (wv[]){ rt_s(&S533), t1619, rt_s(&S437), t1620, rt_s(&S535) }); }); std_push(t1621, t1622); });
    RT_AT(5, 446);
    v881_k = rt_add(v881_k, rt_i(1));
  }
  RT_AT(5, 448);
  (void)({ wv t1626 = rt_def(up->v693_enumDefs, "enumDefs"); wv t1627 = ({ wv t1623 = rt_text(v878_m); wv t1624 = rt_text(rt_get(&F_name, v877_s)); wv t1625 = rt_text(std_join(v880_members, rt_s(&S437))); rt_cat(7, (wv[]){ rt_s(&S544), t1623, rt_s(&S12), t1624, rt_s(&S545), t1625, rt_s(&S546) }); }); std_push(t1626, t1627); });
  RT_RET(RT_NULL);
}

static wv s735_stolenFrom(struct fr146 *up, wv v882_x) {
  rt_enter("stolenFrom");
  RT_AT(5, 453);
  if (rt_cond(rt_ne(rt_get(&F_k, v882_x), rt_s(&S214)))) {
    RT_AT(5, 453);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 454);
  wv v883_k = rt_i(0);
  RT_AT(5, 455);
  while (rt_cond((RT_AT(5, 455), ({ wv t1628 = v883_k; wv t1629 = std_length(rt_def(up->v708_stolen, "stolen")); rt_lt(t1628, t1629); })))) {
    RT_AT(5, 456);
    if (rt_cond(({ wv t1632 = rt_get(&F_name, ({ wv t1630 = v883_k; wv t1631 = rt_def(up->v708_stolen, "stolen"); rt_idx(t1630, t1631); })); wv t1633 = rt_get(&F_name, v882_x); rt_eq(t1632, t1633); }))) {
      RT_AT(5, 456);
      RT_RET(rt_get(&F_lib, ({ wv t1634 = v883_k; wv t1635 = rt_def(up->v708_stolen, "stolen"); rt_idx(t1634, t1635); })));
    }
    RT_AT(5, 457);
    v883_k = rt_add(v883_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 459), RT_NULL));
}

static wv s736_enumIn(struct fr146 *up, wv v884_list, wv v885_name) {
  rt_enter("enumIn");
  v885_name = rt_bind(v885_name, 0);
  RT_AT(5, 464);
  wv v886_k = rt_i(0);
  RT_AT(5, 465);
  while (rt_cond((RT_AT(5, 465), ({ wv t1636 = v886_k; wv t1637 = std_length(v884_list); rt_lt(t1636, t1637); })))) {
    RT_AT(5, 466);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_idx(v886_k, v884_list)), rt_s(&S47)), "&&") && rt_bool(({ wv t1638 = rt_get(&F_name, rt_idx(v886_k, v884_list)); wv t1639 = v885_name; rt_eq(t1638, t1639); }), "&&")))) {
      RT_AT(5, 466);
      RT_RET(rt_idx(v886_k, v884_list));
    }
    RT_AT(5, 467);
    v886_k = rt_add(v886_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 469), RT_NULL));
}

static wv s737_enumOf(struct fr146 *up, wv v887_x) {
  rt_enter("enumOf");
  RT_AT(5, 474);
  if (rt_cond(rt_eq(rt_get(&F_k, v887_x), rt_s(&S214)))) {
    RT_AT(5, 475);
    wv v891_s = rt_bind(({ wv t1640 = rt_def(up->v709_ownEnums, "ownEnums"); wv t1641 = rt_get(&F_name, v887_x); rt_chk(s736_enumIn(up, t1640, t1641), "enumOf"); }), 0);
    RT_AT(5, 476);
    RT_RET((rt_cond(rt_eq(v891_s, RT_NULL)) ? RT_NULL : ({ wv t1644 = ({ wv t1642 = rt_text(rt_def(up->v706_mod, "mod")); wv t1643 = rt_text(rt_get(&F_name, v887_x)); rt_cat(4, (wv[]){ rt_s(&S547), t1642, rt_s(&S12), t1643 }); }); wv t1645 = v891_s; rt_rec(0, 2, K54, (wv[]){ t1644, t1645 }); })));
  }
  RT_AT(5, 478);
  if (rt_cond(rt_ne(rt_get(&F_k, v887_x), rt_s(&S192)))) {
    RT_AT(5, 478);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 479);
  wv v888_lib = rt_bind(rt_chk(s735_stolenFrom(up, rt_get(&F_rec, v887_x)), "enumOf"), 0);
  RT_AT(5, 480);
  if (rt_cond(rt_eq(v888_lib, RT_NULL))) {
    RT_AT(5, 480);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 481);
  wv v889_xs = rt_bind(rt_get(&F_exports, rt_idx(v888_lib, up->v686_modules)), 0);
  RT_AT(5, 482);
  wv v890_k = rt_i(0);
  RT_AT(5, 483);
  while (rt_cond((RT_AT(5, 483), rt_b(rt_bool(({ wv t1646 = v890_k; wv t1647 = std_length(v889_xs); rt_lt(t1646, t1647); }), "&&") && rt_bool(({ wv t1648 = rt_get(&F_name, rt_idx(v890_k, v889_xs)); wv t1649 = rt_get(&F_field, v887_x); rt_ne(t1648, t1649); }), "&&"))))) {
    RT_AT(5, 483);
    v890_k = rt_add(v890_k, rt_i(1));
  }
  RT_AT(5, 484);
  if (rt_cond(rt_b(rt_bool(({ wv t1650 = v890_k; wv t1651 = std_length(v889_xs); rt_eq(t1650, t1651); }), "||") || rt_bool(rt_ne(rt_get(&F_kind, rt_idx(v890_k, v889_xs)), rt_s(&S47)), "||")))) {
    RT_AT(5, 484);
    RT_RET(RT_NULL);
  }
  RT_RET((RT_AT(5, 485), ({ wv t1656 = ({ wv t1652 = rt_text(rt_chk(s714_modId(up, v888_lib), "enumOf")); wv t1653 = rt_text(rt_get(&F_field, v887_x)); rt_cat(4, (wv[]){ rt_s(&S547), t1652, rt_s(&S12), t1653 }); }); wv t1657 = ({ wv t1654 = rt_get(&F_stmts, rt_get(&F_program, rt_idx(v888_lib, up->v686_modules))); wv t1655 = rt_get(&F_field, v887_x); rt_chk(s736_enumIn(up, t1654, t1655), "enumOf"); }); rt_rec(0, 2, K54, (wv[]){ t1656, t1657 }); })));
}

static wv s738_place(struct fr146 *up, wv v892_d) {
  rt_enter("place");
  v892_d = rt_bind(v892_d, 0);
  RT_AT(5, 490);
  wv v893_owner = rt_bind(rt_get(&F_func, ({ wv t1658 = v892_d; wv t1659 = rt_def(up->v701_decls, "decls"); rt_idx(t1658, t1659); })), 0);
  RT_AT(5, 491);
  if (rt_cond(rt_get(&F_top, ({ wv t1660 = v893_owner; wv t1661 = rt_def(up->v702_fns, "fns"); rt_idx(t1660, t1661); })))) {
    RT_AT(5, 491);
    RT_RET(rt_get(&F_cname, ({ wv t1662 = v892_d; wv t1663 = rt_def(up->v701_decls, "decls"); rt_idx(t1662, t1663); })));
  }
  RT_AT(5, 492);
  if (rt_cond(({ wv t1664 = v893_owner; wv t1665 = rt_def(up->v710_cur, "cur"); rt_eq(t1664, t1665); }))) {
    RT_AT(5, 492);
    RT_RET((rt_cond(rt_get(&F_captured, ({ wv t1666 = v892_d; wv t1667 = rt_def(up->v701_decls, "decls"); rt_idx(t1666, t1667); }))) ? rt_cat(2, (wv[]){ rt_s(&S548), rt_text(rt_get(&F_cname, ({ wv t1668 = v892_d; wv t1669 = rt_def(up->v701_decls, "decls"); rt_idx(t1668, t1669); }))) }) : rt_get(&F_cname, ({ wv t1670 = v892_d; wv t1671 = rt_def(up->v701_decls, "decls"); rt_idx(t1670, t1671); }))));
  }
  RT_RET((RT_AT(5, 493), ({ wv t1680 = rt_text(rt_chk(w6_repeat(rt_s(&S550), rt_sub(({ wv t1676 = rt_get(&F_depth, ({ wv t1672 = rt_def(up->v710_cur, "cur"); wv t1673 = rt_def(up->v702_fns, "fns"); rt_idx(t1672, t1673); })); wv t1677 = rt_get(&F_depth, ({ wv t1674 = v893_owner; wv t1675 = rt_def(up->v702_fns, "fns"); rt_idx(t1674, t1675); })); rt_sub(t1676, t1677); }), rt_i(1))), "place")); wv t1681 = rt_text(rt_get(&F_cname, ({ wv t1678 = v892_d; wv t1679 = rt_def(up->v701_decls, "decls"); rt_idx(t1678, t1679); }))); rt_cat(4, (wv[]){ rt_s(&S549), t1680, rt_s(&S551), t1681 }); })));
}

static wv s739_tdz(struct fr146 *up, wv v894_d) {
  rt_enter("tdz");
  v894_d = rt_bind(v894_d, 0);
  RT_RET((RT_AT(5, 496), rt_b(rt_bool(rt_eq(rt_get(&F_kind, ({ wv t1682 = v894_d; wv t1683 = rt_def(up->v701_decls, "decls"); rt_idx(t1682, t1683); })), rt_s(&S179)), "&&") && rt_bool(({ wv t1686 = rt_get(&F_func, ({ wv t1684 = v894_d; wv t1685 = rt_def(up->v701_decls, "decls"); rt_idx(t1684, t1685); })); wv t1687 = rt_def(up->v710_cur, "cur"); rt_ne(t1686, t1687); }), "&&"))));
}

static wv s740_read(struct fr146 *up, wv v895_d) {
  rt_enter("read");
  v895_d = rt_bind(v895_d, 0);
  RT_RET((RT_AT(5, 498), (rt_cond(rt_chk(s739_tdz(up, v895_d), "read")) ? ({ wv t1692 = ({ wv t1690 = rt_text(rt_chk(s738_place(up, v895_d), "read")); wv t1691 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1688 = v895_d; wv t1689 = rt_def(up->v701_decls, "decls"); rt_idx(t1688, t1689); }))), "read")); rt_cat(5, (wv[]){ rt_s(&S552), t1690, rt_s(&S437), t1691, rt_s(&S16) }); }); wv t1693 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t1692, t1693 }); }) : ({ wv t1694 = rt_chk(s738_place(up, v895_d), "read"); wv t1695 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K55, (wv[]){ t1694, t1695 }); }))));
}

static wv s741_seq(struct fr146 *up, wv v896_args) {
  rt_enter("seq");
  v896_args = rt_bind(v896_args, 0);
  RT_AT(5, 505);
  wv v897_effects = rt_i(0);
  RT_AT(5, 506);
  wv v898_anyPure = rt_b(0);
  RT_AT(5, 507);
  wv v899_k = rt_i(0);
  RT_AT(5, 508);
  while (rt_cond((RT_AT(5, 508), ({ wv t1696 = v899_k; wv t1697 = std_length(v896_args); rt_lt(t1696, t1697); })))) {
    RT_AT(5, 509);
    if (rt_cond(({ wv t1698 = rt_get(&F_k, rt_idx(v899_k, v896_args)); wv t1699 = rt_def(w6_effectK, "effectK"); rt_eq(t1698, t1699); }))) {
      RT_AT(5, 509);
      v897_effects = rt_add(v897_effects, rt_i(1));
    }
    RT_AT(5, 510);
    if (rt_cond(({ wv t1700 = rt_get(&F_k, rt_idx(v899_k, v896_args)); wv t1701 = rt_def(w6_pureK, "pureK"); rt_eq(t1700, t1701); }))) {
      RT_AT(5, 510);
      v898_anyPure = rt_b(1);
    }
    RT_AT(5, 511);
    v899_k = rt_add(v899_k, rt_i(1));
  }
  RT_AT(5, 513);
  wv v900_cs = rt_arr(0, NULL, 1);
  RT_AT(5, 514);
  if (rt_cond(rt_b(rt_bool(rt_eq(v897_effects, rt_i(0)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v897_effects, rt_i(1)), "&&") && rt_bool(rt_not(v898_anyPure), "&&")), "||")))) {
    RT_AT(5, 515);
    v899_k = rt_i(0);
    RT_AT(5, 516);
    while (rt_cond((RT_AT(5, 516), ({ wv t1702 = v899_k; wv t1703 = std_length(v896_args); rt_lt(t1702, t1703); })))) {
      RT_AT(5, 516);
      (void)({ wv t1704 = v900_cs; wv t1705 = rt_get(&F_c, rt_idx(v899_k, v896_args)); std_push(t1704, t1705); });
      RT_AT(5, 516);
      v899_k = rt_add(v899_k, rt_i(1));
    }
    RT_AT(5, 517);
    RT_RET(rt_rec(0, 2, K56, (wv[]){ rt_s(&S13), v900_cs }));
  }
  RT_AT(5, 519);
  wv v901_pre = rt_arr(0, NULL, 1);
  RT_AT(5, 520);
  v899_k = rt_i(0);
  RT_AT(5, 521);
  while (rt_cond((RT_AT(5, 521), ({ wv t1706 = v899_k; wv t1707 = std_length(v896_args); rt_lt(t1706, t1707); })))) {
    RT_AT(5, 522);
    if (rt_cond(({ wv t1708 = rt_get(&F_k, rt_idx(v899_k, v896_args)); wv t1709 = rt_def(w6_constK, "constK"); rt_eq(t1708, t1709); }))) {
      RT_AT(5, 523);
      (void)({ wv t1710 = v900_cs; wv t1711 = rt_get(&F_c, rt_idx(v899_k, v896_args)); std_push(t1710, t1711); });
    } else {
      RT_AT(5, 525);
      wv v902_temp = rt_bind(rt_chk(s731_tmp(up), "seq"), 0);
      RT_AT(5, 526);
      (void)({ wv t1714 = v901_pre; wv t1715 = ({ wv t1712 = rt_text(v902_temp); wv t1713 = rt_text(rt_get(&F_c, rt_idx(v899_k, v896_args))); rt_cat(5, (wv[]){ rt_s(&S553), t1712, rt_s(&S554), t1713, rt_s(&S14) }); }); std_push(t1714, t1715); });
      RT_AT(5, 527);
      (void)std_push(v900_cs, v902_temp);
    }
    RT_AT(5, 529);
    v899_k = rt_add(v899_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 531), ({ wv t1716 = std_join(v901_pre, rt_s(&S0)); wv t1717 = v900_cs; rt_rec(0, 2, K56, (wv[]){ t1716, t1717 }); })));
}

static wv s742_wrap(struct fr146 *up, wv v903_sq, wv v904_inner) {
  rt_enter("wrap");
  v903_sq = rt_bind(v903_sq, 0);
  v904_inner = rt_bind(v904_inner, 0);
  RT_RET((RT_AT(5, 533), ({ wv t1720 = (rt_cond(rt_eq(rt_get(&F_pre, v903_sq), rt_s(&S13))) ? v904_inner : ({ wv t1718 = rt_text(rt_get(&F_pre, v903_sq)); wv t1719 = rt_text(v904_inner); rt_cat(5, (wv[]){ rt_s(&S555), t1718, rt_s(&S0), t1719, rt_s(&S556) }); })); wv t1721 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t1720, t1721 }); })));
}

static wv s743_stmts(struct fr146 *up, wv v905_list) {
  rt_enter("stmts");
  v905_list = rt_bind(v905_list, 0);
  RT_AT(5, 536);
  wv v906_k = rt_i(0);
  RT_AT(5, 537);
  while (rt_cond((RT_AT(5, 537), ({ wv t1722 = v906_k; wv t1723 = std_length(v905_list); rt_lt(t1722, t1723); })))) {
    RT_AT(5, 538);
    (void)rt_chk(s747_stmt(up, rt_idx(v906_k, v905_list)), "stmts");
    RT_AT(5, 539);
    v906_k = rt_add(v906_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s744_resets(struct fr146 *up, wv v907_b) {
  rt_enter("resets");
  RT_AT(5, 546);
  wv v908_k = rt_i(0);
  RT_AT(5, 547);
  while (rt_cond((RT_AT(5, 547), ({ wv t1724 = v908_k; wv t1725 = std_length(rt_get(&F_stmts, v907_b)); rt_lt(t1724, t1725); })))) {
    RT_AT(5, 548);
    wv v909_s = ({ wv t1726 = v908_k; wv t1727 = rt_get(&F_stmts, v907_b); rt_idx(t1726, t1727); });
    RT_AT(5, 549);
    if (rt_cond(rt_eq(rt_get(&F_k, v909_s), rt_s(&S179)))) {
      RT_AT(5, 550);
      wv v910_d = rt_bind(({ wv t1728 = rt_get(&F_namePos, v909_s); wv t1729 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1728, t1729); }), 0);
      RT_AT(5, 551);
      if (rt_cond(rt_get(&F_captured, ({ wv t1730 = v910_d; wv t1731 = rt_def(up->v701_decls, "decls"); rt_idx(t1730, t1731); })))) {
        RT_AT(5, 551);
        (void)rt_chk(s730_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s738_place(up, v910_d), "resets")), rt_s(&S557) })), "resets");
      }
    }
    RT_AT(5, 553);
    v908_k = rt_add(v908_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s745_blockBody(struct fr146 *up, wv v911_b) {
  rt_enter("blockBody");
  v911_b = rt_bind(v911_b, 0);
  RT_AT(5, 559);
  (void)rt_chk(s744_resets(up, v911_b), "blockBody");
  RT_AT(5, 560);
  (void)rt_chk(s743_stmts(up, rt_get(&F_stmts, v911_b)), "blockBody");
  RT_AT(5, 561);
  if (rt_cond(rt_ne(rt_get(&F_value, v911_b), RT_NULL))) {
    RT_AT(5, 561);
    (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S558), rt_text(rt_chk(s746_valueOf(up, v911_b), "blockBody")), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s746_valueOf(struct fr146 *up, wv v912_b) {
  rt_enter("valueOf");
  v912_b = rt_bind(v912_b, 0);
  RT_RET((RT_AT(5, 565), (rt_cond(rt_eq(rt_get(&F_value, v912_b), RT_NULL)) ? rt_s(&S559) : ({ wv t1732 = rt_get(&F_pos, rt_get(&F_value, v912_b)); wv t1733 = rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_value, v912_b)), "valueOf")); rt_chk(s713_atExpr(up, t1732, t1733), "valueOf"); }))));
}

static wv s747_stmt(struct fr146 *up, wv v913_s) {
  rt_enter("stmt");
  RT_AT(5, 568);
  wv v914_kind = rt_bind(rt_get(&F_k, v913_s), 0);
  RT_AT(5, 569);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v914_kind, rt_s(&S203)), "||") || rt_bool(rt_eq(v914_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v914_kind, rt_s(&S48)), "||")), "||") || rt_bool(rt_eq(v914_kind, rt_s(&S206)), "||")))) {
    RT_AT(5, 569);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 570);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S33)))) {
    RT_AT(5, 570);
    (void)rt_chk(s752_fnDef(up, v913_s), "stmt");
    RT_AT(5, 570);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 571);
  if (rt_cond(rt_b(rt_bool(rt_ne(v914_kind, rt_s(&S43)), "&&") && rt_bool(rt_ne(v914_kind, rt_s(&S44)), "&&")))) {
    RT_AT(5, 571);
    (void)rt_chk(s730_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s712_atLine(up, rt_get(&F_pos, v913_s)), "stmt")), rt_s(&S14) })), "stmt");
  }
  RT_AT(5, 572);
  if (rt_cond(rt_ne(rt_get(&F_handler, v913_s), RT_NULL))) {
    RT_AT(5, 572);
    (void)rt_chk(s759_onfailStmt(up, v913_s), "stmt");
    RT_AT(5, 572);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 573);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S34)))) {
    RT_AT(5, 574);
    if (rt_cond(rt_def(up->v700_iife, "iife"))) {
      RT_AT(5, 574);
      (void)rt_chk(w2_abort(rt_get(&F_pos, v913_s), rt_s(&S560)), "stmt");
    }
    RT_AT(5, 575);
    (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S561), rt_text((rt_cond(rt_eq(rt_get(&F_e, v913_s), RT_NULL)) ? rt_s(&S559) : rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_e, v913_s)), "stmt")))), rt_s(&S562) })), "stmt");
    RT_AT(5, 576);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 578);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S58)))) {
    RT_AT(5, 578);
    (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S563), rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_e, v913_s)), "stmt"))), rt_s(&S562) })), "stmt");
    RT_AT(5, 578);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 579);
  if (rt_cond(rt_b(rt_bool(rt_eq(v914_kind, rt_s(&S43)), "||") || rt_bool(rt_eq(v914_kind, rt_s(&S44)), "||")))) {
    RT_AT(5, 580);
    if (rt_cond(rt_eq(rt_def(up->v699_loops, "loops"), rt_i(0)))) {
      RT_AT(5, 580);
      (void)({ wv t1734 = rt_get(&F_pos, v913_s); wv t1735 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text((rt_cond(rt_eq(v914_kind, rt_s(&S43))) ? rt_s(&S43) : rt_s(&S44))), rt_s(&S564) }); rt_chk(w2_abort(t1734, t1735), "stmt"); });
    }
    RT_AT(5, 581);
    (void)rt_chk(s730_emit(up, (rt_cond(rt_eq(v914_kind, rt_s(&S43))) ? rt_s(&S565) : rt_s(&S566))), "stmt");
    RT_AT(5, 582);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 584);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S41)))) {
    RT_AT(5, 586);
    (void)rt_chk(s730_emit(up, ({ wv t1738 = rt_text((rt_cond(rt_get(&F_neg, v913_s)) ? rt_s(&S95) : rt_s(&S13))); wv t1739 = rt_text(({ wv t1736 = rt_get(&F_pos, rt_get(&F_cond, v913_s)); wv t1737 = rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_cond, v913_s)), "stmt")); rt_chk(s713_atExpr(up, t1736, t1737), "stmt"); })); rt_cat(5, (wv[]){ rt_s(&S567), t1738, rt_s(&S568), t1739, rt_s(&S569) }); })), "stmt");
    RT_AT(5, 587);
    up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 588);
    up->v699_loops = rt_add(rt_def(up->v699_loops, "loops"), rt_i(1));
    RT_AT(5, 589);
    (void)rt_chk(s745_blockBody(up, rt_get(&F_body, v913_s)), "stmt");
    RT_AT(5, 590);
    up->v699_loops = rt_sub(rt_def(up->v699_loops, "loops"), rt_i(1));
    RT_AT(5, 591);
    up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 592);
    (void)rt_chk(s730_emit(up, rt_s(&S15)), "stmt");
    RT_AT(5, 593);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 595);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S42)))) {
    RT_AT(5, 595);
    (void)rt_chk(s749_forStmt(up, v913_s), "stmt");
    RT_AT(5, 595);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 596);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S56)))) {
    RT_AT(5, 596);
    (void)rt_chk(s750_signal(up, rt_cat(3, (wv[]){ rt_s(&S570), rt_text(rt_get(&F_code, v913_s)), rt_s(&S16) })), "stmt");
    RT_AT(5, 596);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 597);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S55)))) {
    RT_AT(5, 597);
    (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S571), rt_text((rt_cond(rt_eq(rt_get(&F_e, v913_s), RT_NULL)) ? rt_s(&S572) : rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_e, v913_s)), "stmt")))), rt_s(&S562) })), "stmt");
    RT_AT(5, 597);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 598);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S178)))) {
    RT_AT(5, 599);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v913_s)), rt_s(&S186)))) {
      RT_AT(5, 599);
      (void)rt_chk(s760_chainStmt(up, rt_get(&F_e, v913_s)), "stmt");
      RT_AT(5, 599);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 600);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v913_s)), rt_s(&S45)))) {
      RT_AT(5, 600);
      (void)rt_chk(s771_matchStmt(up, rt_get(&F_e, v913_s)), "stmt");
      RT_AT(5, 600);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 601);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v913_s)), rt_s(&S185)))) {
      RT_AT(5, 601);
      (void)rt_chk(s750_signal(up, rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_arg, rt_get(&F_e, v913_s))), "stmt"))), "stmt");
      RT_AT(5, 601);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 602);
    (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S558), rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_e, v913_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_AT(5, 603);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 605);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S179)))) {
    RT_AT(5, 605);
    (void)({ wv t1744 = ({ wv t1740 = rt_get(&F_namePos, v913_s); wv t1741 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1740, t1741); }); wv t1745 = ({ wv t1742 = rt_get(&F_e, v913_s); wv t1743 = rt_chk(w6_modeOf(v913_s), "stmt"); rt_chk(s753_bindRhs(up, t1742, t1743), "stmt"); }); rt_chk(s751_bindDecl(up, t1744, t1745), "stmt"); });
    RT_AT(5, 605);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 606);
  if (rt_cond(rt_eq(v914_kind, rt_s(&S38)))) {
    RT_AT(5, 606);
    (void)rt_chk(s754_setStmt(up, v913_s), "stmt");
    RT_AT(5, 606);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 607);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S573), rt_text(v914_kind) }));
  RT_RET(RT_NULL);
}

static wv s748_pathText(struct fr146 *up, wv v915_x) {
  rt_enter("pathText");
  v915_x = rt_bind(v915_x, 0);
  RT_AT(5, 612);
  if (rt_cond(rt_eq(rt_get(&F_k, v915_x), rt_s(&S214)))) {
    RT_AT(5, 612);
    RT_RET(rt_get(&F_name, v915_x));
  }
  RT_AT(5, 613);
  if (rt_cond(rt_ne(rt_get(&F_k, v915_x), rt_s(&S192)))) {
    RT_AT(5, 613);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 614);
  wv v916_p = rt_bind(rt_chk(s748_pathText(up, rt_get(&F_rec, v915_x)), "pathText"), 0);
  RT_RET((RT_AT(5, 615), (rt_cond(rt_eq(v916_p, RT_NULL)) ? RT_NULL : ({ wv t1746 = rt_text(v916_p); wv t1747 = rt_text(rt_get(&F_field, v915_x)); rt_cat(3, (wv[]){ t1746, rt_s(&S130), t1747 }); }))));
}

static wv s749_forStmt(struct fr146 *up, wv v917_s) {
  rt_enter("forStmt");
  v917_s = rt_bind(v917_s, 0);
  RT_AT(5, 621);
  wv v918_k = rt_bind(rt_chk(s731_tmp(up), "forStmt"), 0);
  RT_AT(5, 622);
  wv v919_first = rt_bind(({ wv t1748 = rt_get(&F_pos, rt_idx(rt_i(0), rt_get(&F_names, v917_s))); wv t1749 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1748, t1749); }), 0);
  RT_AT(5, 623);
  wv v920_second = rt_bind((rt_cond(rt_eq(std_length(rt_get(&F_names, v917_s)), rt_i(2))) ? ({ wv t1750 = rt_get(&F_pos, rt_idx(rt_i(1), rt_get(&F_names, v917_s))); wv t1751 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1750, t1751); }) : RT_NULL), 0);
  RT_AT(5, 624);
  (void)rt_chk(s730_emit(up, rt_s(&S101)), "forStmt");
  RT_AT(5, 625);
  up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 626);
  if (rt_cond(rt_eq(rt_get(&F_coll, v917_s), RT_NULL))) {
    RT_AT(5, 628);
    wv v921_lo = rt_bind(rt_chk(s731_tmp(up), "forStmt"), 0);
    RT_AT(5, 629);
    wv v922_hi = rt_bind(rt_chk(s731_tmp(up), "forStmt"), 0);
    RT_AT(5, 630);
    wv v923_st = rt_bind(rt_chk(s731_tmp(up), "forStmt"), 0);
    RT_AT(5, 631);
    wv v924_count = rt_bind(rt_chk(s731_tmp(up), "forStmt"), 0);
    RT_AT(5, 632);
    (void)rt_chk(s730_emit(up, ({ wv t1752 = rt_text(v921_lo); wv t1753 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_from, v917_s)), "forStmt"))); rt_cat(5, (wv[]){ rt_s(&S553), t1752, rt_s(&S554), t1753, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 633);
    (void)rt_chk(s730_emit(up, ({ wv t1754 = rt_text(v922_hi); wv t1755 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_to, v917_s)), "forStmt"))); rt_cat(5, (wv[]){ rt_s(&S553), t1754, rt_s(&S554), t1755, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 634);
    (void)rt_chk(s730_emit(up, ({ wv t1756 = rt_text(v923_st); wv t1757 = rt_text((rt_cond(rt_eq(rt_get(&F_step, v917_s), RT_NULL)) ? rt_s(&S574) : rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_step, v917_s)), "forStmt")))); rt_cat(5, (wv[]){ rt_s(&S553), t1756, rt_s(&S554), t1757, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 635);
    (void)rt_chk(s730_emit(up, ({ wv t1758 = rt_text(v924_count); wv t1759 = rt_text(v921_lo); wv t1760 = rt_text(v922_hi); wv t1761 = rt_text(v923_st); rt_cat(9, (wv[]){ rt_s(&S575), t1758, rt_s(&S576), t1759, rt_s(&S437), t1760, rt_s(&S437), t1761, rt_s(&S562) }); })), "forStmt");
    RT_AT(5, 636);
    (void)rt_chk(s730_emit(up, ({ wv t1762 = rt_text(v918_k); wv t1763 = rt_text(v918_k); wv t1764 = rt_text(v924_count); wv t1765 = rt_text(v918_k); rt_cat(9, (wv[]){ rt_s(&S577), t1762, rt_s(&S578), t1763, rt_s(&S579), t1764, rt_s(&S580), t1765, rt_s(&S581) }); })), "forStmt");
    RT_AT(5, 637);
    up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 638);
    wv v925_at = rt_bind((rt_cond(rt_get(&F_reversed, v917_s)) ? ({ wv t1766 = rt_text(v924_count); wv t1767 = rt_text(v918_k); rt_cat(3, (wv[]){ t1766, rt_s(&S582), t1767 }); }) : v918_k), 0);
    RT_AT(5, 639);
    (void)({ wv t1771 = v919_first; wv t1772 = ({ wv t1768 = rt_text(v921_lo); wv t1769 = rt_text(v925_at); wv t1770 = rt_text(v923_st); rt_cat(7, (wv[]){ rt_s(&S570), t1768, rt_s(&S583), t1769, rt_s(&S584), t1770, rt_s(&S585) }); }); rt_chk(s751_bindDecl(up, t1771, t1772), "forStmt"); });
  } else {
    RT_AT(5, 641);
    wv v926_en = rt_bind(rt_chk(s737_enumOf(up, rt_get(&F_coll, v917_s)), "forStmt"), 0);
    RT_AT(5, 642);
    if (rt_cond(rt_ne(v926_en, RT_NULL))) {
      RT_AT(5, 644);
      wv v927_count = rt_bind(std_length(rt_get(&F_members, rt_get(&F_node, v926_en))), 0);
      RT_AT(5, 645);
      (void)rt_chk(s730_emit(up, ({ wv t1773 = rt_text(v918_k); wv t1774 = rt_text(v918_k); wv t1775 = rt_text(v927_count); wv t1776 = rt_text(v918_k); rt_cat(9, (wv[]){ rt_s(&S577), t1773, rt_s(&S578), t1774, rt_s(&S579), t1775, rt_s(&S580), t1776, rt_s(&S581) }); })), "forStmt");
      RT_AT(5, 646);
      up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
      RT_AT(5, 647);
      wv v928_at = rt_bind((rt_cond(rt_get(&F_reversed, v917_s)) ? ({ wv t1777 = rt_text(rt_sub(v927_count, rt_i(1))); wv t1778 = rt_text(v918_k); rt_cat(3, (wv[]){ t1777, rt_s(&S586), t1778 }); }) : v918_k), 0);
      RT_AT(5, 648);
      if (rt_cond(rt_eq(v920_second, RT_NULL))) {
        RT_AT(5, 649);
        (void)({ wv t1781 = v919_first; wv t1782 = ({ wv t1779 = rt_text(rt_get(&F_c, v926_en)); wv t1780 = rt_text(v928_at); rt_cat(5, (wv[]){ rt_s(&S587), t1779, rt_s(&S99), t1780, rt_s(&S588) }); }); rt_chk(s751_bindDecl(up, t1781, t1782), "forStmt"); });
      } else {
        RT_AT(5, 651);
        (void)({ wv t1783 = v919_first; wv t1784 = rt_cat(3, (wv[]){ rt_s(&S570), rt_text(v928_at), rt_s(&S16) }); rt_chk(s751_bindDecl(up, t1783, t1784), "forStmt"); });
        RT_AT(5, 652);
        (void)({ wv t1787 = v920_second; wv t1788 = ({ wv t1785 = rt_text(rt_get(&F_c, v926_en)); wv t1786 = rt_text(v928_at); rt_cat(5, (wv[]){ rt_s(&S587), t1785, rt_s(&S99), t1786, rt_s(&S588) }); }); rt_chk(s751_bindDecl(up, t1787, t1788), "forStmt"); });
      }
    } else {
      RT_AT(5, 657);
      wv v929_fr = rt_bind(rt_chk(s731_tmp(up), "forStmt"), 0);
      RT_AT(5, 658);
      (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S589), rt_text(v929_fr), rt_s(&S14) })), "forStmt");
      RT_AT(5, 659);
      (void)rt_chk(s730_emit(up, ({ wv t1789 = rt_text(v929_fr); wv t1790 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_coll, v917_s)), "forStmt"))); wv t1791 = rt_text((rt_cond(rt_get(&F_reversed, v917_s)) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S590), t1789, rt_s(&S437), t1790, rt_s(&S437), t1791, rt_s(&S562) }); })), "forStmt");
      RT_AT(5, 660);
      wv v930_path = rt_bind(rt_chk(s748_pathText(up, rt_get(&F_coll, v917_s)), "forStmt"), 0);
      RT_AT(5, 661);
      wv v931_guard = rt_bind((rt_cond(rt_eq(v930_path, RT_NULL)) ? rt_s(&S13) : ({ wv t1792 = rt_text(v929_fr); wv t1793 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_coll, v917_s)), "forStmt"))); wv t1794 = rt_text(rt_chk(w6_cString(v930_path), "forStmt")); rt_cat(7, (wv[]){ rt_s(&S591), t1792, rt_s(&S437), t1793, rt_s(&S437), t1794, rt_s(&S592) }); })), 0);
      RT_AT(5, 662);
      (void)rt_chk(s730_emit(up, ({ wv t1795 = rt_text(v918_k); wv t1796 = rt_text(v931_guard); wv t1797 = rt_text(v929_fr); wv t1798 = rt_text(v918_k); wv t1799 = rt_text(v918_k); rt_cat(11, (wv[]){ rt_s(&S577), t1795, rt_s(&S578), t1796, rt_s(&S593), t1797, rt_s(&S437), t1798, rt_s(&S594), t1799, rt_s(&S581) }); })), "forStmt");
      RT_AT(5, 663);
      up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
      RT_AT(5, 664);
      if (rt_cond(rt_eq(v920_second, RT_NULL))) {
        RT_AT(5, 665);
        (void)({ wv t1802 = v919_first; wv t1803 = ({ wv t1800 = rt_text(v929_fr); wv t1801 = rt_text(v918_k); rt_cat(5, (wv[]){ rt_s(&S595), t1800, rt_s(&S437), t1801, rt_s(&S16) }); }); rt_chk(s751_bindDecl(up, t1802, t1803), "forStmt"); });
      } else {
        RT_AT(5, 667);
        (void)({ wv t1806 = v919_first; wv t1807 = ({ wv t1804 = rt_text(v929_fr); wv t1805 = rt_text(v918_k); rt_cat(5, (wv[]){ rt_s(&S596), t1804, rt_s(&S437), t1805, rt_s(&S16) }); }); rt_chk(s751_bindDecl(up, t1806, t1807), "forStmt"); });
        RT_AT(5, 668);
        (void)({ wv t1810 = v920_second; wv t1811 = ({ wv t1808 = rt_text(v929_fr); wv t1809 = rt_text(v918_k); rt_cat(5, (wv[]){ rt_s(&S597), t1808, rt_s(&S437), t1809, rt_s(&S16) }); }); rt_chk(s751_bindDecl(up, t1810, t1811), "forStmt"); });
      }
    }
  }
  RT_AT(5, 672);
  up->v699_loops = rt_add(rt_def(up->v699_loops, "loops"), rt_i(1));
  RT_AT(5, 673);
  (void)rt_chk(s745_blockBody(up, rt_get(&F_body, v917_s)), "forStmt");
  RT_AT(5, 674);
  up->v699_loops = rt_sub(rt_def(up->v699_loops, "loops"), rt_i(1));
  RT_AT(5, 675);
  up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 676);
  (void)rt_chk(s730_emit(up, rt_s(&S15)), "forStmt");
  RT_AT(5, 677);
  up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 678);
  (void)rt_chk(s730_emit(up, rt_s(&S15)), "forStmt");
  RT_RET(RT_NULL);
}

static wv s750_signal(struct fr146 *up, wv v932_code) {
  rt_enter("signal");
  v932_code = rt_bind(v932_code, 0);
  RT_AT(5, 683);
  (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S598), rt_text(v932_code), rt_s(&S562) })), "signal");
  RT_AT(5, 684);
  (void)rt_chk(s730_emit(up, rt_s(&S599)), "signal");
  RT_RET(RT_NULL);
}

static wv s751_bindDecl(struct fr146 *up, wv v933_d, wv v934_value) {
  rt_enter("bindDecl");
  v933_d = rt_bind(v933_d, 0);
  v934_value = rt_bind(v934_value, 0);
  RT_AT(5, 689);
  wv v935_owner = rt_bind(rt_get(&F_func, ({ wv t1812 = v933_d; wv t1813 = rt_def(up->v701_decls, "decls"); rt_idx(t1812, t1813); })), 0);
  RT_AT(5, 690);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1814 = v935_owner; wv t1815 = rt_def(up->v702_fns, "fns"); rt_idx(t1814, t1815); }))), "&&") && rt_bool(({ wv t1816 = v935_owner; wv t1817 = rt_def(up->v710_cur, "cur"); rt_eq(t1816, t1817); }), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_captured, ({ wv t1818 = v933_d; wv t1819 = rt_def(up->v701_decls, "decls"); rt_idx(t1818, t1819); }))), "&&")))) {
    RT_AT(5, 691);
    (void)rt_chk(s730_emit(up, ({ wv t1822 = rt_text(rt_get(&F_cname, ({ wv t1820 = v933_d; wv t1821 = rt_def(up->v701_decls, "decls"); rt_idx(t1820, t1821); }))); wv t1823 = rt_text(v934_value); rt_cat(5, (wv[]){ rt_s(&S553), t1822, rt_s(&S554), t1823, rt_s(&S14) }); })), "bindDecl");
  } else {
    RT_AT(5, 693);
    (void)rt_chk(s730_emit(up, ({ wv t1824 = rt_text(rt_chk(s738_place(up, v933_d), "bindDecl")); wv t1825 = rt_text(v934_value); rt_cat(4, (wv[]){ t1824, rt_s(&S554), t1825, rt_s(&S14) }); })), "bindDecl");
  }
  RT_RET(RT_NULL);
}

static wv s752_fnDef(struct fr146 *up, wv v936_s) {
  rt_enter("fnDef");
  v936_s = rt_bind(v936_s, 0);
  RT_AT(5, 698);
  wv v937_d = rt_bind(({ wv t1826 = rt_get(&F_namePos, v936_s); wv t1827 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1826, t1827); }), 0);
  RT_AT(5, 699);
  wv v938_f = rt_bind(rt_get(&F_fnIndex, ({ wv t1828 = v937_d; wv t1829 = rt_def(up->v701_decls, "decls"); rt_idx(t1828, t1829); })), 0);
  RT_AT(5, 700);
  wv v939_pf = rt_bind(rt_get(&F_parent, ({ wv t1830 = v938_f; wv t1831 = rt_def(up->v702_fns, "fns"); rt_idx(t1830, t1831); })), 0);
  RT_AT(5, 701);
  wv v940_hasUp = rt_not(rt_get(&F_top, ({ wv t1832 = v939_pf; wv t1833 = rt_def(up->v702_fns, "fns"); rt_idx(t1832, t1833); })));
  RT_AT(5, 702);
  wv v941_params = rt_arr(0, NULL, 1);
  RT_AT(5, 703);
  if (rt_cond(v940_hasUp)) {
    RT_AT(5, 703);
    (void)({ wv t1834 = v941_params; wv t1835 = rt_cat(3, (wv[]){ rt_s(&S600), rt_text(v939_pf), rt_s(&S601) }); std_push(t1834, t1835); });
  }
  RT_AT(5, 704);
  wv v942_k = rt_i(0);
  RT_AT(5, 705);
  while (rt_cond((RT_AT(5, 705), ({ wv t1836 = v942_k; wv t1837 = std_length(rt_get(&F_params, v936_s)); rt_lt(t1836, t1837); })))) {
    RT_AT(5, 706);
    wv v950_pd = rt_bind(({ wv t1840 = rt_get(&F_pos, ({ wv t1838 = v942_k; wv t1839 = rt_get(&F_params, v936_s); rt_idx(t1838, t1839); })); wv t1841 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1840, t1841); }), 0);
    RT_AT(5, 707);
    (void)({ wv t1848 = v941_params; wv t1849 = ({ wv t1846 = rt_text((rt_cond(rt_get(&F_captured, ({ wv t1842 = v950_pd; wv t1843 = rt_def(up->v701_decls, "decls"); rt_idx(t1842, t1843); }))) ? rt_s(&S602) : rt_s(&S13))); wv t1847 = rt_text(rt_get(&F_cname, ({ wv t1844 = v950_pd; wv t1845 = rt_def(up->v701_decls, "decls"); rt_idx(t1844, t1845); }))); rt_cat(3, (wv[]){ rt_s(&S553), t1846, t1847 }); }); std_push(t1848, t1849); });
    RT_AT(5, 708);
    v942_k = rt_add(v942_k, rt_i(1));
  }
  RT_AT(5, 710);
  wv v943_proto = ({ wv t1852 = rt_text(rt_get(&F_cname, ({ wv t1850 = v938_f; wv t1851 = rt_def(up->v702_fns, "fns"); rt_idx(t1850, t1851); }))); wv t1853 = rt_text((rt_cond(rt_eq(std_length(v941_params), rt_i(0))) ? rt_s(&S604) : std_join(v941_params, rt_s(&S437)))); rt_cat(5, (wv[]){ rt_s(&S603), t1852, rt_s(&S98), t1853, rt_s(&S16) }); });
  RT_AT(5, 711);
  wv v944_entry = ({ wv t1854 = v943_proto; wv t1855 = rt_arr(0, NULL, 0); rt_rec(1, 2, K57, (wv[]){ t1854, t1855 }); });
  RT_AT(5, 712);
  (void)({ wv t1856 = rt_def(up->v696_functions, "functions"); wv t1857 = v944_entry; std_push(t1856, t1857); });
  RT_AT(5, 713);
  if (rt_cond(rt_chk(s715_hasFrame(up, v938_f), "fnDef"))) {
    RT_AT(5, 713);
    (void)({ wv t1858 = rt_def(up->v695_frames, "frames"); wv t1859 = v938_f; std_push(t1858, t1859); });
  }
  RT_AT(5, 714);
  wv v945_start = rt_bind(std_length(rt_def(up->v697_out, "out")), 0);
  RT_AT(5, 715);
  wv v946_savedIndent = rt_bind(rt_def(up->v698_indent, "indent"), 0);
  RT_AT(5, 716);
  wv v947_savedLoops = rt_bind(rt_def(up->v699_loops, "loops"), 0);
  RT_AT(5, 717);
  wv v948_savedIife = rt_bind(rt_def(up->v700_iife, "iife"), 0);
  RT_AT(5, 718);
  wv v949_savedCur = rt_bind(rt_def(up->v710_cur, "cur"), 0);
  RT_AT(5, 719);
  wv t1860 = rt_i(1);
  (void)rt_def(up->v698_indent, "indent");
  up->v698_indent = t1860;
  RT_AT(5, 720);
  wv t1861 = rt_i(0);
  (void)rt_def(up->v699_loops, "loops");
  up->v699_loops = t1861;
  RT_AT(5, 721);
  wv t1862 = rt_b(0);
  (void)rt_def(up->v700_iife, "iife");
  up->v700_iife = t1862;
  RT_AT(5, 722);
  wv t1863 = rt_bind(v938_f, 1);
  (void)rt_def(up->v710_cur, "cur");
  up->v710_cur = t1863;
  RT_AT(5, 723);
  (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S605), rt_text(rt_chk(w1_quote(rt_get(&F_name, v936_s)), "fnDef")), rt_s(&S562) })), "fnDef");
  RT_AT(5, 724);
  if (rt_cond(rt_chk(s715_hasFrame(up, v938_f), "fnDef"))) {
    RT_AT(5, 724);
    (void)rt_chk(s730_emit(up, ({ wv t1864 = rt_text(v938_f); wv t1865 = rt_text((rt_cond(v940_hasUp) ? rt_s(&S607) : rt_s(&S4))); rt_cat(5, (wv[]){ rt_s(&S600), t1864, rt_s(&S606), t1865, rt_s(&S546) }); })), "fnDef");
  }
  RT_AT(5, 725);
  v942_k = rt_i(0);
  RT_AT(5, 726);
  while (rt_cond((RT_AT(5, 726), ({ wv t1866 = v942_k; wv t1867 = std_length(rt_get(&F_params, v936_s)); rt_lt(t1866, t1867); })))) {
    RT_AT(5, 727);
    wv v951_p = rt_bind(({ wv t1868 = v942_k; wv t1869 = rt_get(&F_params, v936_s); rt_idx(t1868, t1869); }), 0);
    RT_AT(5, 728);
    wv v952_pd = rt_bind(({ wv t1870 = rt_get(&F_pos, v951_p); wv t1871 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1870, t1871); }), 0);
    RT_AT(5, 729);
    wv v953_cn = rt_bind(rt_get(&F_cname, ({ wv t1872 = v952_pd; wv t1873 = rt_def(up->v701_decls, "decls"); rt_idx(t1872, t1873); })), 0);
    RT_AT(5, 731);
    wv v954_arg = rt_bind((rt_cond(rt_get(&F_captured, ({ wv t1874 = v952_pd; wv t1875 = rt_def(up->v701_decls, "decls"); rt_idx(t1874, t1875); }))) ? rt_cat(2, (wv[]){ rt_s(&S602), rt_text(v953_cn) }) : v953_cn), 0);
    RT_AT(5, 732);
    wv v955_v = rt_bind((rt_cond(rt_get(&F_isRef, v951_p)) ? v954_arg : ({ wv t1876 = rt_text(v954_arg); wv t1877 = rt_text((rt_cond(rt_get(&F_isVar, v951_p)) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S608), t1876, rt_s(&S437), t1877, rt_s(&S16) }); })), 0);
    RT_AT(5, 733);
    if (rt_cond(rt_get(&F_captured, ({ wv t1878 = v952_pd; wv t1879 = rt_def(up->v701_decls, "decls"); rt_idx(t1878, t1879); })))) {
      RT_AT(5, 733);
      (void)rt_chk(s730_emit(up, ({ wv t1880 = rt_text(v953_cn); wv t1881 = rt_text(v955_v); rt_cat(5, (wv[]){ rt_s(&S548), t1880, rt_s(&S554), t1881, rt_s(&S14) }); })), "fnDef");
    } else if (rt_cond(rt_not(rt_get(&F_isRef, v951_p)))) {
      RT_AT(5, 733);
      (void)rt_chk(s730_emit(up, ({ wv t1882 = rt_text(v953_cn); wv t1883 = rt_text(v955_v); rt_cat(4, (wv[]){ t1882, rt_s(&S554), t1883, rt_s(&S14) }); })), "fnDef");
    }
    RT_AT(5, 734);
    v942_k = rt_add(v942_k, rt_i(1));
  }
  RT_AT(5, 736);
  (void)rt_chk(s743_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v936_s))), "fnDef");
  RT_AT(5, 737);
  (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S561), rt_text(rt_chk(s746_valueOf(up, rt_get(&F_body, v936_s)), "fnDef")), rt_s(&S562) })), "fnDef");
  RT_AT(5, 738);
  ({ wv t1887 = ({ wv t1884 = rt_def(up->v697_out, "out"); wv t1885 = v945_start; wv t1886 = std_length(rt_def(up->v697_out, "out")); std_slice(t1884, t1885, t1886); }); wv t1888 = v944_entry; rt_set_mem(t1887, &F_lines, t1888); });
  RT_AT(5, 739);
  while (rt_cond((RT_AT(5, 739), ({ wv t1889 = std_length(rt_def(up->v697_out, "out")); wv t1890 = v945_start; rt_gt(t1889, t1890); })))) {
    RT_AT(5, 739);
    (void)std_pop(rt_def(up->v697_out, "out"));
  }
  RT_AT(5, 740);
  wv t1891 = rt_bind(v946_savedIndent, 1);
  (void)rt_def(up->v698_indent, "indent");
  up->v698_indent = t1891;
  RT_AT(5, 741);
  wv t1892 = rt_bind(v947_savedLoops, 1);
  (void)rt_def(up->v699_loops, "loops");
  up->v699_loops = t1892;
  RT_AT(5, 742);
  wv t1893 = rt_bind(v948_savedIife, 1);
  (void)rt_def(up->v700_iife, "iife");
  up->v700_iife = t1893;
  RT_AT(5, 743);
  wv t1894 = rt_bind(v949_savedCur, 1);
  (void)rt_def(up->v710_cur, "cur");
  up->v710_cur = t1894;
  RT_RET(RT_NULL);
}

static wv s753_bindRhs(struct fr146 *up, wv v956_e, wv v957_mode) {
  rt_enter("bindRhs");
  v956_e = rt_bind(v956_e, 0);
  v957_mode = rt_bind(v957_mode, 0);
  RT_AT(5, 750);
  wv v958_mutable = rt_eq(v957_mode, rt_s(&S36));
  RT_AT(5, 751);
  if (rt_cond(rt_eq(rt_get(&F_k, v956_e), rt_s(&S188)))) {
    RT_AT(5, 751);
    RT_RET(rt_get(&F_c, rt_chk(s756_array(up, v956_e, v958_mutable), "bindRhs")));
  }
  RT_AT(5, 752);
  if (rt_cond(rt_eq(rt_get(&F_k, v956_e), rt_s(&S189)))) {
    RT_AT(5, 752);
    RT_RET(rt_get(&F_c, rt_chk(s757_mapLit(up, v956_e, v958_mutable), "bindRhs")));
  }
  RT_AT(5, 753);
  if (rt_cond(rt_eq(rt_get(&F_k, v956_e), rt_s(&S190)))) {
    RT_AT(5, 753);
    RT_RET(rt_get(&F_c, rt_chk(s758_record(up, v956_e, v958_mutable), "bindRhs")));
  }
  RT_AT(5, 754);
  if (rt_cond(rt_b(rt_bool(({ wv t1895 = rt_def(w6_scalarKinds, "scalarKinds"); wv t1896 = rt_get(&F_k, v956_e); rt_chk(w1_contains(t1895, t1896), "bindRhs"); }), "||") || rt_bool(rt_eq(v957_mode, rt_s(&S37)), "||")))) {
    RT_AT(5, 754);
    RT_RET(rt_get(&F_c, rt_chk(s774_expr(up, v956_e), "bindRhs")));
  }
  RT_RET((RT_AT(5, 755), ({ wv t1897 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, v956_e), "bindRhs"))); wv t1898 = rt_text((rt_cond(v958_mutable) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S608), t1897, rt_s(&S437), t1898, rt_s(&S16) }); })));
}

static wv s754_setStmt(struct fr146 *up, wv v959_s) {
  rt_enter("setStmt");
  v959_s = rt_bind(v959_s, 0);
  RT_AT(5, 759);
  wv v960_t = rt_bind(rt_get(&F_target, v959_s), 0);
  RT_AT(5, 760);
  if (rt_cond(rt_eq(rt_get(&F_k, v960_t), rt_s(&S214)))) {
    RT_AT(5, 761);
    wv v967_d = rt_bind(({ wv t1899 = rt_get(&F_pos, v960_t); wv t1900 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1899, t1900); }), 0);
    RT_AT(5, 762);
    wv v968_at = rt_bind(rt_chk(s738_place(up, v967_d), "setStmt"), 0);
    RT_AT(5, 763);
    if (rt_cond(rt_eq(rt_get(&F_op, v959_s), RT_NULL))) {
      RT_AT(5, 764);
      wv v971_v = rt_bind(({ wv t1903 = rt_get(&F_e, v959_s); wv t1904 = (rt_cond(rt_get(&F_isRef, ({ wv t1901 = v967_d; wv t1902 = rt_def(up->v701_decls, "decls"); rt_idx(t1901, t1902); }))) ? rt_s(&S37) : rt_s(&S36)); rt_chk(s753_bindRhs(up, t1903, t1904), "setStmt"); }), 0);
      RT_AT(5, 765);
      if (rt_cond(rt_not(rt_chk(s739_tdz(up, v967_d), "setStmt")))) {
        RT_AT(5, 765);
        (void)rt_chk(s730_emit(up, ({ wv t1905 = rt_text(v968_at); wv t1906 = rt_text(v971_v); rt_cat(4, (wv[]){ t1905, rt_s(&S554), t1906, rt_s(&S14) }); })), "setStmt");
        RT_AT(5, 765);
        RT_RET(RT_NULL);
      }
      RT_AT(5, 767);
      wv v972_temp = rt_bind(rt_chk(s731_tmp(up), "setStmt"), 0);
      RT_AT(5, 768);
      (void)rt_chk(s730_emit(up, ({ wv t1907 = rt_text(v972_temp); wv t1908 = rt_text(v971_v); rt_cat(5, (wv[]){ rt_s(&S553), t1907, rt_s(&S554), t1908, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 769);
      (void)rt_chk(s730_emit(up, ({ wv t1911 = rt_text(v968_at); wv t1912 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1909 = v967_d; wv t1910 = rt_def(up->v701_decls, "decls"); rt_idx(t1909, t1910); }))), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S609), t1911, rt_s(&S437), t1912, rt_s(&S562) }); })), "setStmt");
      RT_AT(5, 770);
      (void)rt_chk(s730_emit(up, ({ wv t1913 = rt_text(v968_at); wv t1914 = rt_text(v972_temp); rt_cat(4, (wv[]){ t1913, rt_s(&S554), t1914, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 771);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 774);
    wv v969_v = rt_bind(rt_chk(s774_expr(up, rt_get(&F_e, v959_s)), "setStmt"), 1);
    RT_AT(5, 775);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v959_s)), rt_s(&S139)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v959_s)), rt_s(&S214)), "&&")))) {
      RT_AT(5, 776);
      wv v973_temp = rt_bind(rt_chk(s731_tmp(up), "setStmt"), 0);
      RT_AT(5, 777);
      (void)rt_chk(s730_emit(up, ({ wv t1915 = rt_text(v973_temp); wv t1916 = rt_text(rt_get(&F_c, v969_v)); rt_cat(5, (wv[]){ rt_s(&S553), t1915, rt_s(&S554), t1916, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 778);
      v969_v = ({ wv t1917 = v973_temp; wv t1918 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K55, (wv[]){ t1917, t1918 }); });
    }
    RT_AT(5, 780);
    wv v970_sq = rt_bind(rt_chk(s741_seq(up, ({ wv t1919 = rt_chk(s740_read(up, v967_d), "setStmt"); wv t1920 = v969_v; rt_arr(2, (wv[]){ t1919, t1920 }, 0); })), "setStmt"), 0);
    RT_AT(5, 781);
    (void)rt_chk(s730_emit(up, ({ wv t1926 = rt_text(v968_at); wv t1927 = rt_text(rt_get(&F_c, ({ wv t1924 = v970_sq; wv t1925 = ({ wv t1921 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v959_s)), "setStmt")); wv t1922 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v970_sq))); wv t1923 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v970_sq))); rt_cat(7, (wv[]){ rt_s(&S610), t1921, rt_s(&S98), t1922, rt_s(&S437), t1923, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t1924, t1925), "setStmt"); }))); rt_cat(4, (wv[]){ t1926, rt_s(&S554), t1927, rt_s(&S14) }); })), "setStmt");
    RT_AT(5, 782);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 784);
  wv v961_key = RT_NULL;
  RT_AT(5, 785);
  if (rt_cond(rt_eq(rt_get(&F_k, v960_t), rt_s(&S192)))) {
    RT_AT(5, 785);
    v961_key = ({ wv t1928 = rt_chk(s733_fieldRef(up, rt_get(&F_field, v960_t)), "setStmt"); wv t1929 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K55, (wv[]){ t1928, t1929 }); });
  }
  RT_AT(5, 786);
  wv v962_v = rt_bind(rt_chk(s774_expr(up, rt_get(&F_e, v959_s)), "setStmt"), 0);
  RT_AT(5, 787);
  wv v963_i = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v960_t), rt_s(&S192))) ? v961_key : rt_chk(s774_expr(up, rt_get(&F_index, v960_t)), "setStmt")), 0);
  RT_AT(5, 788);
  wv v964_box = rt_bind(rt_chk(s774_expr(up, (rt_cond(rt_eq(rt_get(&F_k, v960_t), rt_s(&S192))) ? rt_get(&F_rec, v960_t) : rt_get(&F_arr, v960_t))), "setStmt"), 0);
  RT_AT(5, 789);
  wv v965_sq = rt_bind(rt_chk(s741_seq(up, rt_arr(3, (wv[]){ v962_v, v963_i, v964_box }, 0)), "setStmt"), 0);
  RT_AT(5, 790);
  wv v966_which = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v960_t), rt_s(&S192))) ? rt_s(&S192) : rt_s(&S191)), 0);
  RT_AT(5, 791);
  if (rt_cond(rt_eq(rt_get(&F_op, v959_s), RT_NULL))) {
    RT_AT(5, 792);
    (void)rt_chk(s730_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t1932 = v965_sq; wv t1933 = ({ wv t1930 = rt_text(v966_which); wv t1931 = rt_text(std_join(rt_get(&F_cs, v965_sq), rt_s(&S437))); rt_cat(5, (wv[]){ rt_s(&S611), t1930, rt_s(&S98), t1931, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t1932, t1933), "setStmt"); }))), rt_s(&S14) })), "setStmt");
    RT_AT(5, 793);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 795);
  (void)rt_chk(s730_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t1937 = v965_sq; wv t1938 = ({ wv t1934 = rt_text(v966_which); wv t1935 = rt_text(std_join(rt_get(&F_cs, v965_sq), rt_s(&S437))); wv t1936 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v959_s)), "setStmt")); rt_cat(7, (wv[]){ rt_s(&S612), t1934, rt_s(&S98), t1935, rt_s(&S613), t1936, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t1937, t1938), "setStmt"); }))), rt_s(&S14) })), "setStmt");
  RT_RET(RT_NULL);
}

static wv s755_exprs(struct fr146 *up, wv v974_list) {
  rt_enter("exprs");
  v974_list = rt_bind(v974_list, 0);
  RT_AT(5, 799);
  wv v975_xs = rt_arr(0, NULL, 1);
  RT_AT(5, 800);
  wv v976_k = rt_i(0);
  RT_AT(5, 801);
  while (rt_cond((RT_AT(5, 801), ({ wv t1939 = v976_k; wv t1940 = std_length(v974_list); rt_lt(t1939, t1940); })))) {
    RT_AT(5, 802);
    (void)({ wv t1941 = v975_xs; wv t1942 = rt_chk(s774_expr(up, rt_idx(v976_k, v974_list)), "exprs"); std_push(t1941, t1942); });
    RT_AT(5, 803);
    v976_k = rt_add(v976_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 805), v975_xs));
}

static wv s756_array(struct fr146 *up, wv v977_e, wv v978_mutable) {
  rt_enter("array");
  v977_e = rt_bind(v977_e, 0);
  v978_mutable = rt_bind(v978_mutable, 0);
  RT_AT(5, 809);
  wv v979_sq = rt_bind(rt_chk(s741_seq(up, rt_chk(s755_exprs(up, rt_get(&F_items, v977_e)), "array")), "array"), 0);
  RT_RET((RT_AT(5, 810), ({ wv t1946 = v979_sq; wv t1947 = ({ wv t1943 = rt_text(std_length(rt_get(&F_cs, v979_sq))); wv t1944 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v979_sq)), "array")); wv t1945 = rt_text((rt_cond(v978_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S614), t1943, rt_s(&S437), t1944, rt_s(&S437), t1945, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t1946, t1947), "array"); })));
}

static wv s757_mapLit(struct fr146 *up, wv v980_e, wv v981_mutable) {
  rt_enter("mapLit");
  v980_e = rt_bind(v980_e, 0);
  v981_mutable = rt_bind(v981_mutable, 0);
  RT_AT(5, 815);
  wv v982_sq = rt_bind(rt_chk(s741_seq(up, rt_chk(s755_exprs(up, rt_get(&F_items, v980_e)), "mapLit")), "mapLit"), 0);
  RT_RET((RT_AT(5, 816), ({ wv t1951 = v982_sq; wv t1952 = ({ wv t1948 = rt_text(std_floor(rt_div(std_length(rt_get(&F_cs, v982_sq)), rt_i(2)))); wv t1949 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v982_sq)), "mapLit")); wv t1950 = rt_text((rt_cond(v981_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S615), t1948, rt_s(&S437), t1949, rt_s(&S437), t1950, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t1951, t1952), "mapLit"); })));
}

static wv s758_record(struct fr146 *up, wv v983_e, wv v984_mutable) {
  rt_enter("record");
  v983_e = rt_bind(v983_e, 0);
  v984_mutable = rt_bind(v984_mutable, 0);
  RT_AT(5, 821);
  wv v985_shape = rt_s(&S536);
  RT_AT(5, 822);
  if (rt_cond(rt_gt(std_length(rt_get(&F_fields, v983_e)), rt_i(0)))) {
    RT_AT(5, 823);
    wv v989_names = rt_arr(0, NULL, 1);
    RT_AT(5, 824);
    wv v990_k = rt_i(0);
    RT_AT(5, 825);
    while (rt_cond((RT_AT(5, 825), ({ wv t1953 = v990_k; wv t1954 = std_length(rt_get(&F_fields, v983_e)); rt_lt(t1953, t1954); })))) {
      RT_AT(5, 826);
      (void)({ wv t1957 = v989_names; wv t1958 = rt_get(&F_name, ({ wv t1955 = v990_k; wv t1956 = rt_get(&F_fields, v983_e); rt_idx(t1955, t1956); })); std_push(t1957, t1958); });
      RT_AT(5, 827);
      v990_k = rt_add(v990_k, rt_i(1));
    }
    RT_AT(5, 829);
    wv v991_key = rt_bind(std_join(v989_names, rt_s(&S97)), 0);
    RT_AT(5, 830);
    v990_k = rt_i(0);
    RT_AT(5, 831);
    while (rt_cond((RT_AT(5, 831), ({ wv t1959 = v990_k; wv t1960 = std_length(v989_names); rt_lt(t1959, t1960); })))) {
      RT_AT(5, 832);
      (void)rt_chk(s733_fieldRef(up, rt_idx(v990_k, v989_names)), "record");
      RT_AT(5, 833);
      v990_k = rt_add(v990_k, rt_i(1));
    }
    RT_AT(5, 835);
    v990_k = rt_i(0);
    RT_AT(5, 836);
    while (rt_cond((RT_AT(5, 836), rt_b(rt_bool(({ wv t1961 = v990_k; wv t1962 = std_length(rt_def(up->v692_shapes, "shapes")); rt_lt(t1961, t1962); }), "&&") && rt_bool(({ wv t1965 = ({ wv t1963 = v990_k; wv t1964 = rt_def(up->v692_shapes, "shapes"); rt_idx(t1963, t1964); }); wv t1966 = v991_key; rt_ne(t1965, t1966); }), "&&"))))) {
      RT_AT(5, 836);
      v990_k = rt_add(v990_k, rt_i(1));
    }
    RT_AT(5, 837);
    if (rt_cond(({ wv t1967 = v990_k; wv t1968 = std_length(rt_def(up->v692_shapes, "shapes")); rt_eq(t1967, t1968); }))) {
      RT_AT(5, 837);
      (void)({ wv t1969 = rt_def(up->v692_shapes, "shapes"); wv t1970 = v991_key; std_push(t1969, t1970); });
    }
    RT_AT(5, 838);
    v985_shape = rt_cat(2, (wv[]){ rt_s(&S616), rt_text(v990_k) });
  }
  RT_AT(5, 840);
  wv v986_vals = rt_arr(0, NULL, 1);
  RT_AT(5, 841);
  wv v987_k = rt_i(0);
  RT_AT(5, 842);
  while (rt_cond((RT_AT(5, 842), ({ wv t1971 = v987_k; wv t1972 = std_length(rt_get(&F_fields, v983_e)); rt_lt(t1971, t1972); })))) {
    RT_AT(5, 843);
    (void)({ wv t1975 = v986_vals; wv t1976 = rt_chk(s774_expr(up, rt_get(&F_e, ({ wv t1973 = v987_k; wv t1974 = rt_get(&F_fields, v983_e); rt_idx(t1973, t1974); }))), "record"); std_push(t1975, t1976); });
    RT_AT(5, 844);
    v987_k = rt_add(v987_k, rt_i(1));
  }
  RT_AT(5, 846);
  wv v988_sq = rt_bind(rt_chk(s741_seq(up, v986_vals), "record"), 0);
  RT_RET((RT_AT(5, 847), ({ wv t1981 = v988_sq; wv t1982 = ({ wv t1977 = rt_text((rt_cond(v984_mutable) ? rt_i(1) : rt_i(0))); wv t1978 = rt_text(std_length(rt_get(&F_cs, v988_sq))); wv t1979 = rt_text(v985_shape); wv t1980 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v988_sq)), "record")); rt_cat(9, (wv[]){ rt_s(&S617), t1977, rt_s(&S437), t1978, rt_s(&S437), t1979, rt_s(&S437), t1980, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t1981, t1982), "record"); })));
}

static wv s759_onfailStmt(struct fr146 *up, wv v992_s) {
  rt_enter("onfailStmt");
  v992_s = rt_bind(v992_s, 0);
  RT_AT(5, 853);
  wv v993_call = rt_bind(rt_get(&F_e, v992_s), 0);
  RT_AT(5, 854);
  wv v994_args = rt_arr(0, NULL, 1);
  RT_AT(5, 855);
  wv v995_k = rt_i(0);
  RT_AT(5, 856);
  while (rt_cond((RT_AT(5, 856), ({ wv t1983 = v995_k; wv t1984 = std_length(rt_get(&F_args, v993_call)); rt_lt(t1983, t1984); })))) {
    RT_AT(5, 857);
    wv v999_a = rt_bind(({ wv t1985 = v995_k; wv t1986 = rt_get(&F_args, v993_call); rt_idx(t1985, t1986); }), 0);
    RT_AT(5, 858);
    if (rt_cond(({ wv t1987 = rt_def(w6_plainKinds, "plainKinds"); wv t1988 = rt_get(&F_k, v999_a); rt_chk(w1_contains(t1987, t1988), "onfailStmt"); }))) {
      RT_AT(5, 859);
      (void)({ wv t1989 = v994_args; wv t1990 = rt_get(&F_c, rt_chk(s774_expr(up, v999_a), "onfailStmt")); std_push(t1989, t1990); });
    } else {
      RT_AT(5, 861);
      wv v1000_temp = rt_bind(rt_chk(s731_tmp(up), "onfailStmt"), 0);
      RT_AT(5, 862);
      (void)rt_chk(s730_emit(up, ({ wv t1991 = rt_text(v1000_temp); wv t1992 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, v999_a), "onfailStmt"))); rt_cat(5, (wv[]){ rt_s(&S553), t1991, rt_s(&S554), t1992, rt_s(&S14) }); })), "onfailStmt");
      RT_AT(5, 863);
      (void)std_push(v994_args, v1000_temp);
    }
    RT_AT(5, 865);
    v995_k = rt_add(v995_k, rt_i(1));
  }
  RT_AT(5, 867);
  wv v996_r = rt_bind(rt_chk(s731_tmp(up), "onfailStmt"), 0);
  RT_AT(5, 868);
  (void)rt_chk(s730_emit(up, ({ wv t1995 = rt_text(v996_r); wv t1996 = rt_text(({ wv t1993 = rt_get(&F_callee, v993_call); wv t1994 = v994_args; rt_chk(s773_callC(up, t1993, t1994, rt_b(0)), "onfailStmt"); })); rt_cat(5, (wv[]){ rt_s(&S553), t1995, rt_s(&S554), t1996, rt_s(&S14) }); })), "onfailStmt");
  RT_AT(5, 869);
  (void)rt_chk(s730_emit(up, rt_s(&S618)), "onfailStmt");
  RT_AT(5, 870);
  up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 871);
  wv v997_h = rt_bind(rt_get(&F_handler, v992_s), 0);
  RT_AT(5, 872);
  if (rt_cond(rt_ne(rt_get(&F_param, v997_h), RT_NULL))) {
    RT_AT(5, 872);
    (void)rt_chk(s751_bindDecl(up, ({ wv t1997 = rt_get(&F_paramPos, v997_h); wv t1998 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t1997, t1998); }), rt_s(&S619)), "onfailStmt");
  }
  RT_AT(5, 873);
  (void)rt_chk(s730_emit(up, rt_s(&S620)), "onfailStmt");
  RT_AT(5, 874);
  (void)rt_chk(s730_emit(up, rt_s(&S101)), "onfailStmt");
  RT_AT(5, 875);
  up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 876);
  (void)rt_chk(s744_resets(up, rt_get(&F_body, v997_h)), "onfailStmt");
  RT_AT(5, 877);
  (void)rt_chk(s743_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v997_h))), "onfailStmt");
  RT_AT(5, 878);
  (void)rt_chk(s730_emit(up, ({ wv t1999 = rt_text(v996_r); wv t2000 = rt_text(rt_chk(s746_valueOf(up, rt_get(&F_body, v997_h)), "onfailStmt")); rt_cat(4, (wv[]){ t1999, rt_s(&S554), t2000, rt_s(&S14) }); })), "onfailStmt");
  RT_AT(5, 879);
  up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 880);
  (void)rt_chk(s730_emit(up, rt_s(&S15)), "onfailStmt");
  RT_AT(5, 881);
  up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 882);
  (void)rt_chk(s730_emit(up, rt_s(&S15)), "onfailStmt");
  RT_AT(5, 883);
  wv v998_rest = rt_bind(v992_s, 1);
  RT_AT(5, 884);
  rt_set_mem(RT_NULL, &F_handler, v998_rest);
  RT_AT(5, 885);
  ({ wv t2003 = ({ wv t2001 = v996_r; wv t2002 = rt_get(&F_pos, v992_s); rt_rec(0, 4, K58, (wv[]){ rt_s(&S621), t2001, t2002, rt_b(0) }); }); wv t2004 = v998_rest; rt_set_mem(t2003, &F_e, t2004); });
  RT_AT(5, 886);
  (void)rt_chk(s747_stmt(up, v998_rest), "onfailStmt");
  RT_RET(RT_NULL);
}

static wv s760_chainStmt(struct fr146 *up, wv v1001_e) {
  rt_enter("chainStmt");
  v1001_e = rt_bind(v1001_e, 0);
  RT_AT(5, 890);
  wv v1002_k = rt_i(0);
  RT_AT(5, 891);
  while (rt_cond((RT_AT(5, 891), ({ wv t2005 = v1002_k; wv t2006 = std_length(rt_get(&F_branches, v1001_e)); rt_lt(t2005, t2006); })))) {
    RT_AT(5, 892);
    wv v1003_b = rt_bind(({ wv t2007 = v1002_k; wv t2008 = rt_get(&F_branches, v1001_e); rt_idx(t2007, t2008); }), 0);
    RT_AT(5, 893);
    (void)rt_chk(s730_emit(up, ({ wv t2009 = rt_text((rt_cond(rt_gt(v1002_k, rt_i(0))) ? rt_s(&S622) : rt_s(&S13))); wv t2010 = rt_text(rt_chk(s761_test(up, v1003_b), "chainStmt")); rt_cat(4, (wv[]){ t2009, rt_s(&S623), t2010, rt_s(&S624) }); })), "chainStmt");
    RT_AT(5, 894);
    up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 895);
    (void)rt_chk(s745_blockBody(up, rt_get(&F_body, v1003_b)), "chainStmt");
    RT_AT(5, 896);
    up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 897);
    v1002_k = rt_add(v1002_k, rt_i(1));
  }
  RT_AT(5, 899);
  if (rt_cond(rt_ne(rt_get(&F_els, v1001_e), RT_NULL))) {
    RT_AT(5, 900);
    (void)rt_chk(s730_emit(up, rt_s(&S625)), "chainStmt");
    RT_AT(5, 901);
    up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 902);
    (void)rt_chk(s745_blockBody(up, rt_get(&F_els, v1001_e)), "chainStmt");
    RT_AT(5, 903);
    up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
  }
  RT_AT(5, 905);
  (void)rt_chk(s730_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s761_test(struct fr146 *up, wv v1004_b) {
  rt_enter("test");
  v1004_b = rt_bind(v1004_b, 0);
  RT_RET((RT_AT(5, 908), ({ wv t2011 = rt_text((rt_cond(rt_get(&F_neg, v1004_b)) ? rt_s(&S95) : rt_s(&S13))); wv t2012 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_cond, v1004_b)), "test"))); rt_cat(4, (wv[]){ t2011, rt_s(&S568), t2012, rt_s(&S16) }); })));
}

static wv s762_branchCount(struct fr146 *up, wv v1005_e) {
  rt_enter("branchCount");
  v1005_e = rt_bind(v1005_e, 0);
  RT_RET((RT_AT(5, 913), (rt_cond(rt_eq(rt_get(&F_k, v1005_e), rt_s(&S186))) ? std_length(rt_get(&F_branches, v1005_e)) : std_length(rt_get(&F_cases, v1005_e)))));
}

static wv s763_branchBody(struct fr146 *up, wv v1006_e, wv v1007_i) {
  rt_enter("branchBody");
  v1006_e = rt_bind(v1006_e, 0);
  v1007_i = rt_bind(v1007_i, 0);
  RT_RET((RT_AT(5, 914), (rt_cond(rt_eq(rt_get(&F_k, v1006_e), rt_s(&S186))) ? rt_get(&F_body, ({ wv t2013 = v1007_i; wv t2014 = rt_get(&F_branches, v1006_e); rt_idx(t2013, t2014); })) : rt_get(&F_body, ({ wv t2015 = v1007_i; wv t2016 = rt_get(&F_cases, v1006_e); rt_idx(t2015, t2016); })))));
}

static wv s764_branchTest(struct fr146 *up, wv v1008_e, wv v1009_t, wv v1010_i) {
  rt_enter("branchTest");
  v1008_e = rt_bind(v1008_e, 0);
  v1009_t = rt_bind(v1009_t, 0);
  v1010_i = rt_bind(v1010_i, 0);
  RT_RET((RT_AT(5, 915), (rt_cond(rt_eq(rt_get(&F_k, v1008_e), rt_s(&S186))) ? rt_chk(s761_test(up, ({ wv t2017 = v1010_i; wv t2018 = rt_get(&F_branches, v1008_e); rt_idx(t2017, t2018); })), "branchTest") : ({ wv t2021 = v1009_t; wv t2022 = ({ wv t2019 = v1010_i; wv t2020 = rt_get(&F_cases, v1008_e); rt_idx(t2019, t2020); }); rt_chk(s769_caseTest(up, t2021, t2022), "branchTest"); }))));
}

static wv s765_isPlain(struct fr146 *up, wv v1011_e) {
  rt_enter("isPlain");
  v1011_e = rt_bind(v1011_e, 0);
  RT_AT(5, 919);
  wv v1012_k = rt_i(0);
  RT_AT(5, 920);
  while (rt_cond((RT_AT(5, 920), ({ wv t2023 = v1012_k; wv t2024 = rt_chk(s762_branchCount(up, v1011_e), "isPlain"); rt_le(t2023, t2024); })))) {
    RT_AT(5, 921);
    wv v1013_b = rt_bind((rt_cond(({ wv t2025 = v1012_k; wv t2026 = rt_chk(s762_branchCount(up, v1011_e), "isPlain"); rt_lt(t2025, t2026); })) ? rt_chk(s763_branchBody(up, v1011_e, v1012_k), "isPlain") : rt_get(&F_els, v1011_e)), 0);
    RT_AT(5, 922);
    wv v1014_j = rt_i(0);
    RT_AT(5, 923);
    while (rt_cond((RT_AT(5, 923), ({ wv t2027 = v1014_j; wv t2028 = std_length(rt_get(&F_stmts, v1013_b)); rt_lt(t2027, t2028); })))) {
      RT_AT(5, 924);
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t2029 = v1014_j; wv t2030 = rt_get(&F_stmts, v1013_b); rt_idx(t2029, t2030); })), rt_s(&S203)))) {
        RT_AT(5, 924);
        RT_RET(rt_b(0));
      }
      RT_AT(5, 925);
      v1014_j = rt_add(v1014_j, rt_i(1));
    }
    RT_AT(5, 927);
    v1012_k = rt_add(v1012_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 929), rt_b(1)));
}

static wv s766_ternaryChain(struct fr146 *up, wv v1015_e, wv v1016_t) {
  rt_enter("ternaryChain");
  v1015_e = rt_bind(v1015_e, 0);
  v1016_t = rt_bind(v1016_t, 0);
  RT_AT(5, 934);
  wv v1017_code = rt_bind(rt_chk(s746_valueOf(up, rt_get(&F_els, v1015_e)), "ternaryChain"), 1);
  RT_AT(5, 935);
  wv v1018_k = rt_sub(rt_chk(s762_branchCount(up, v1015_e), "ternaryChain"), rt_i(1));
  RT_AT(5, 936);
  while (rt_cond((RT_AT(5, 936), rt_ge(v1018_k, rt_i(0))))) {
    RT_AT(5, 937);
    v1017_code = ({ wv t2031 = rt_text(rt_chk(s764_branchTest(up, v1015_e, v1016_t, v1018_k), "ternaryChain")); wv t2032 = rt_text(rt_chk(s746_valueOf(up, rt_chk(s763_branchBody(up, v1015_e, v1018_k), "ternaryChain")), "ternaryChain")); wv t2033 = rt_text(v1017_code); rt_cat(7, (wv[]){ rt_s(&S98), t2031, rt_s(&S626), t2032, rt_s(&S627), t2033, rt_s(&S16) }); });
    RT_AT(5, 938);
    v1018_k = rt_sub(v1018_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 940), v1017_code));
}

static wv s767_branchValue(struct fr146 *up, wv v1019_e, wv v1020_t, wv v1021_subject) {
  rt_enter("branchValue");
  v1019_e = rt_bind(v1019_e, 0);
  v1020_t = rt_bind(v1020_t, 0);
  v1021_subject = rt_bind(v1021_subject, 0);
  RT_AT(5, 947);
  wv v1022_start = rt_bind(std_length(rt_def(up->v697_out, "out")), 0);
  RT_AT(5, 948);
  wv v1023_savedIndent = rt_bind(rt_def(up->v698_indent, "indent"), 0);
  RT_AT(5, 949);
  wv v1024_savedLoops = rt_bind(rt_def(up->v699_loops, "loops"), 0);
  RT_AT(5, 950);
  wv v1025_savedIife = rt_bind(rt_def(up->v700_iife, "iife"), 0);
  RT_AT(5, 951);
  wv t2034 = rt_add(v1023_savedIndent, rt_i(1));
  (void)rt_def(up->v698_indent, "indent");
  up->v698_indent = t2034;
  RT_AT(5, 952);
  wv t2035 = rt_i(0);
  (void)rt_def(up->v699_loops, "loops");
  up->v699_loops = t2035;
  RT_AT(5, 953);
  wv t2036 = rt_b(1);
  (void)rt_def(up->v700_iife, "iife");
  up->v700_iife = t2036;
  RT_AT(5, 954);
  if (rt_cond(rt_ne(v1021_subject, RT_NULL))) {
    RT_AT(5, 954);
    (void)rt_chk(s730_emit(up, ({ wv t2037 = rt_text(v1020_t); wv t2038 = rt_text(v1021_subject); rt_cat(5, (wv[]){ rt_s(&S553), t2037, rt_s(&S554), t2038, rt_s(&S14) }); })), "branchValue");
  }
  RT_AT(5, 955);
  wv v1026_r = rt_bind(rt_chk(s731_tmp(up), "branchValue"), 0);
  RT_AT(5, 956);
  (void)rt_chk(s730_emit(up, rt_cat(3, (wv[]){ rt_s(&S553), rt_text(v1026_r), rt_s(&S14) })), "branchValue");
  RT_AT(5, 957);
  wv v1027_k = rt_i(0);
  RT_AT(5, 958);
  while (rt_cond((RT_AT(5, 958), ({ wv t2039 = v1027_k; wv t2040 = rt_chk(s762_branchCount(up, v1019_e), "branchValue"); rt_lt(t2039, t2040); })))) {
    RT_AT(5, 959);
    wv v1029_b = rt_bind(rt_chk(s763_branchBody(up, v1019_e, v1027_k), "branchValue"), 0);
    RT_AT(5, 960);
    (void)rt_chk(s730_emit(up, ({ wv t2041 = rt_text((rt_cond(rt_gt(v1027_k, rt_i(0))) ? rt_s(&S622) : rt_s(&S13))); wv t2042 = rt_text(rt_chk(s764_branchTest(up, v1019_e, v1020_t, v1027_k), "branchValue")); rt_cat(4, (wv[]){ t2041, rt_s(&S623), t2042, rt_s(&S624) }); })), "branchValue");
    RT_AT(5, 961);
    up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 962);
    (void)rt_chk(s744_resets(up, v1029_b), "branchValue");
    RT_AT(5, 963);
    (void)rt_chk(s743_stmts(up, rt_get(&F_stmts, v1029_b)), "branchValue");
    RT_AT(5, 964);
    (void)rt_chk(s730_emit(up, ({ wv t2043 = rt_text(v1026_r); wv t2044 = rt_text(rt_chk(s746_valueOf(up, v1029_b), "branchValue")); rt_cat(4, (wv[]){ t2043, rt_s(&S554), t2044, rt_s(&S14) }); })), "branchValue");
    RT_AT(5, 965);
    up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 966);
    v1027_k = rt_add(v1027_k, rt_i(1));
  }
  RT_AT(5, 968);
  (void)rt_chk(s730_emit(up, rt_s(&S625)), "branchValue");
  RT_AT(5, 969);
  up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 970);
  (void)rt_chk(s744_resets(up, rt_get(&F_els, v1019_e)), "branchValue");
  RT_AT(5, 971);
  (void)rt_chk(s743_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v1019_e))), "branchValue");
  RT_AT(5, 972);
  (void)rt_chk(s730_emit(up, ({ wv t2045 = rt_text(v1026_r); wv t2046 = rt_text(rt_chk(s746_valueOf(up, rt_get(&F_els, v1019_e)), "branchValue")); rt_cat(4, (wv[]){ t2045, rt_s(&S554), t2046, rt_s(&S14) }); })), "branchValue");
  RT_AT(5, 973);
  up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
  RT_AT(5, 974);
  (void)rt_chk(s730_emit(up, rt_s(&S15)), "branchValue");
  RT_AT(5, 975);
  (void)rt_chk(s730_emit(up, rt_cat(2, (wv[]){ rt_text(v1026_r), rt_s(&S14) })), "branchValue");
  RT_AT(5, 976);
  wv v1028_body = rt_bind(std_join(({ wv t2047 = rt_def(up->v697_out, "out"); wv t2048 = v1022_start; wv t2049 = std_length(rt_def(up->v697_out, "out")); std_slice(t2047, t2048, t2049); }), rt_s(&S2)), 0);
  RT_AT(5, 977);
  while (rt_cond((RT_AT(5, 977), ({ wv t2050 = std_length(rt_def(up->v697_out, "out")); wv t2051 = v1022_start; rt_gt(t2050, t2051); })))) {
    RT_AT(5, 977);
    (void)std_pop(rt_def(up->v697_out, "out"));
  }
  RT_AT(5, 978);
  wv t2052 = rt_bind(v1023_savedIndent, 1);
  (void)rt_def(up->v698_indent, "indent");
  up->v698_indent = t2052;
  RT_AT(5, 979);
  wv t2053 = rt_bind(v1024_savedLoops, 1);
  (void)rt_def(up->v699_loops, "loops");
  up->v699_loops = t2053;
  RT_AT(5, 980);
  wv t2054 = rt_bind(v1025_savedIife, 1);
  (void)rt_def(up->v700_iife, "iife");
  up->v700_iife = t2054;
  RT_RET((RT_AT(5, 981), ({ wv t2055 = rt_text(v1028_body); wv t2056 = rt_text(rt_chk(w6_repeat(rt_s(&S541), rt_def(up->v698_indent, "indent")), "branchValue")); rt_cat(5, (wv[]){ rt_s(&S628), t2055, rt_s(&S2), t2056, rt_s(&S629) }); })));
}

static wv s768_chainExpr(struct fr146 *up, wv v1030_e) {
  rt_enter("chainExpr");
  v1030_e = rt_bind(v1030_e, 0);
  RT_AT(5, 987);
  if (rt_cond(rt_chk(s765_isPlain(up, v1030_e), "chainExpr"))) {
    RT_AT(5, 987);
    RT_RET(rt_chk(s766_ternaryChain(up, v1030_e, RT_NULL), "chainExpr"));
  }
  RT_RET((RT_AT(5, 988), rt_chk(s767_branchValue(up, v1030_e, RT_NULL, RT_NULL), "chainExpr")));
}

static wv s769_caseTest(struct fr146 *up, wv v1031_t, wv v1032_a) {
  rt_enter("caseTest");
  v1031_t = rt_bind(v1031_t, 0);
  v1032_a = rt_bind(v1032_a, 0);
  RT_AT(5, 994);
  wv v1033_tests = rt_arr(0, NULL, 1);
  RT_AT(5, 995);
  wv v1034_k = rt_i(0);
  RT_AT(5, 996);
  while (rt_cond((RT_AT(5, 996), ({ wv t2057 = v1034_k; wv t2058 = std_length(rt_get(&F_values, v1032_a)); rt_lt(t2057, t2058); })))) {
    RT_AT(5, 997);
    (void)({ wv t2063 = v1033_tests; wv t2064 = ({ wv t2061 = rt_text(v1031_t); wv t2062 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, ({ wv t2059 = v1034_k; wv t2060 = rt_get(&F_values, v1032_a); rt_idx(t2059, t2060); })), "caseTest"))); rt_cat(5, (wv[]){ rt_s(&S630), t2061, rt_s(&S437), t2062, rt_s(&S16) }); }); std_push(t2063, t2064); });
    RT_AT(5, 998);
    v1034_k = rt_add(v1034_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 1000), std_join(v1033_tests, rt_s(&S631))));
}

static wv s770_matchElse(struct fr146 *up, wv v1035_e, wv v1036_t) {
  rt_enter("matchElse");
  v1035_e = rt_bind(v1035_e, 0);
  v1036_t = rt_bind(v1036_t, 0);
  RT_AT(5, 1006);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v1035_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_cases, v1035_e))))), rt_s(&S192)), "||")))) {
    RT_AT(5, 1006);
    RT_RET(rt_get(&F_els, v1035_e));
  }
  RT_RET((RT_AT(5, 1007), ({ wv t2065 = rt_arr(0, NULL, 0); wv t2066 = rt_rec(0, 4, K58, (wv[]){ rt_s(&S621), rt_cat(3, (wv[]){ rt_s(&S632), rt_text(v1036_t), rt_s(&S633) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K22, (wv[]){ t2065, t2066, RT_NULL }); })));
}

static wv s771_matchStmt(struct fr146 *up, wv v1037_e) {
  rt_enter("matchStmt");
  v1037_e = rt_bind(v1037_e, 0);
  RT_AT(5, 1011);
  wv v1038_t = rt_bind(rt_chk(s731_tmp(up), "matchStmt"), 0);
  RT_AT(5, 1012);
  (void)rt_chk(s730_emit(up, ({ wv t2067 = rt_text(v1038_t); wv t2068 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_subject, v1037_e)), "matchStmt"))); rt_cat(5, (wv[]){ rt_s(&S553), t2067, rt_s(&S554), t2068, rt_s(&S14) }); })), "matchStmt");
  RT_AT(5, 1013);
  wv v1039_k = rt_i(0);
  RT_AT(5, 1014);
  while (rt_cond((RT_AT(5, 1014), ({ wv t2069 = v1039_k; wv t2070 = std_length(rt_get(&F_cases, v1037_e)); rt_lt(t2069, t2070); })))) {
    RT_AT(5, 1015);
    (void)rt_chk(s730_emit(up, ({ wv t2075 = rt_text((rt_cond(rt_gt(v1039_k, rt_i(0))) ? rt_s(&S622) : rt_s(&S13))); wv t2076 = rt_text(({ wv t2073 = v1038_t; wv t2074 = ({ wv t2071 = v1039_k; wv t2072 = rt_get(&F_cases, v1037_e); rt_idx(t2071, t2072); }); rt_chk(s769_caseTest(up, t2073, t2074), "matchStmt"); })); rt_cat(4, (wv[]){ t2075, rt_s(&S623), t2076, rt_s(&S624) }); })), "matchStmt");
    RT_AT(5, 1016);
    up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 1017);
    (void)rt_chk(s745_blockBody(up, rt_get(&F_body, ({ wv t2077 = v1039_k; wv t2078 = rt_get(&F_cases, v1037_e); rt_idx(t2077, t2078); }))), "matchStmt");
    RT_AT(5, 1018);
    up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 1019);
    v1039_k = rt_add(v1039_k, rt_i(1));
  }
  RT_AT(5, 1021);
  wv v1040_els = rt_bind(rt_chk(s770_matchElse(up, v1037_e, v1038_t), "matchStmt"), 0);
  RT_AT(5, 1022);
  if (rt_cond(rt_ne(v1040_els, RT_NULL))) {
    RT_AT(5, 1023);
    (void)rt_chk(s730_emit(up, rt_s(&S625)), "matchStmt");
    RT_AT(5, 1024);
    up->v698_indent = rt_add(rt_def(up->v698_indent, "indent"), rt_i(1));
    RT_AT(5, 1025);
    (void)rt_chk(s745_blockBody(up, v1040_els), "matchStmt");
    RT_AT(5, 1026);
    up->v698_indent = rt_sub(rt_def(up->v698_indent, "indent"), rt_i(1));
  }
  RT_AT(5, 1028);
  (void)rt_chk(s730_emit(up, rt_s(&S15)), "matchStmt");
  RT_RET(RT_NULL);
}

static wv s772_matchExpr(struct fr146 *up, wv v1041_e) {
  rt_enter("matchExpr");
  v1041_e = rt_bind(v1041_e, 0);
  RT_AT(5, 1034);
  wv v1042_t = rt_bind(rt_chk(s731_tmp(up), "matchExpr"), 0);
  RT_AT(5, 1035);
  wv v1043_subject = rt_bind(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_subject, v1041_e)), "matchExpr")), 0);
  RT_AT(5, 1036);
  wv v1044_withElse = rt_bind(v1041_e, 1);
  RT_AT(5, 1037);
  ({ wv t2079 = rt_chk(s770_matchElse(up, v1041_e, v1042_t), "matchExpr"); wv t2080 = v1044_withElse; rt_set_mem(t2079, &F_els, t2080); });
  RT_AT(5, 1038);
  if (rt_cond(rt_chk(s765_isPlain(up, v1044_withElse), "matchExpr"))) {
    RT_AT(5, 1038);
    RT_RET(({ wv t2081 = rt_text(v1042_t); wv t2082 = rt_text(v1043_subject); wv t2083 = rt_text(rt_chk(s766_ternaryChain(up, v1044_withElse, v1042_t), "matchExpr")); rt_cat(7, (wv[]){ rt_s(&S634), t2081, rt_s(&S554), t2082, rt_s(&S580), t2083, rt_s(&S556) }); }));
  }
  RT_RET((RT_AT(5, 1039), rt_chk(s767_branchValue(up, v1044_withElse, v1042_t, v1043_subject), "matchExpr")));
}

static wv s773_callC(struct fr146 *up, wv v1045_callee, wv v1046_args, wv v1047_checked) {
  rt_enter("callC");
  v1045_callee = rt_bind(v1045_callee, 0);
  v1047_checked = rt_bind(v1047_checked, 0);
  RT_AT(5, 1044);
  wv v1048_code = rt_s(&S13);
  RT_AT(5, 1045);
  if (rt_cond(rt_eq(rt_get(&F_k, v1045_callee), rt_s(&S157)))) {
    RT_AT(5, 1046);
    v1048_code = ({ wv t2084 = rt_text(rt_get(&F_name, v1045_callee)); wv t2085 = rt_text(std_join(v1046_args, rt_s(&S437))); rt_cat(5, (wv[]){ rt_s(&S635), t2084, rt_s(&S98), t2085, rt_s(&S16) }); });
    RT_AT(5, 1047);
    if (rt_cond(rt_not(({ wv t2086 = rt_def(w6_signalers, "signalers"); wv t2087 = rt_get(&F_name, v1045_callee); rt_chk(w1_contains(t2086, t2087), "callC"); })))) {
      RT_AT(5, 1047);
      RT_RET(v1048_code);
    }
  } else {
    RT_AT(5, 1049);
    wv v1049_f = rt_s(&S13);
    RT_AT(5, 1050);
    wv v1050_parent = RT_NULL;
    RT_AT(5, 1051);
    if (rt_cond(rt_eq(rt_get(&F_k, v1045_callee), rt_s(&S192)))) {
      RT_AT(5, 1052);
      v1049_f = ({ wv t2088 = rt_text(rt_chk(s714_modId(up, rt_chk(s735_stolenFrom(up, rt_get(&F_rec, v1045_callee)), "callC")), "callC")); wv t2089 = rt_text(rt_get(&F_field, v1045_callee)); rt_cat(4, (wv[]){ rt_s(&S539), t2088, rt_s(&S12), t2089 }); });
    } else {
      RT_AT(5, 1054);
      wv v1053_d = rt_bind(({ wv t2090 = rt_get(&F_pos, v1045_callee); wv t2091 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t2090, t2091); }), 0);
      RT_AT(5, 1055);
      wv v1054_sf = rt_bind(rt_get(&F_fnIndex, ({ wv t2092 = v1053_d; wv t2093 = rt_def(up->v701_decls, "decls"); rt_idx(t2092, t2093); })), 0);
      RT_AT(5, 1056);
      v1049_f = rt_bind(rt_get(&F_cname, ({ wv t2094 = v1054_sf; wv t2095 = rt_def(up->v702_fns, "fns"); rt_idx(t2094, t2095); })), 1);
      RT_AT(5, 1057);
      wv v1055_pf = rt_bind(rt_get(&F_parent, ({ wv t2096 = v1054_sf; wv t2097 = rt_def(up->v702_fns, "fns"); rt_idx(t2096, t2097); })), 0);
      RT_AT(5, 1058);
      if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2098 = v1055_pf; wv t2099 = rt_def(up->v702_fns, "fns"); rt_idx(t2098, t2099); }))))) {
        RT_AT(5, 1058);
        v1050_parent = rt_bind(v1055_pf, 1);
      }
    }
    RT_AT(5, 1061);
    wv v1051_all = rt_arr(0, NULL, 1);
    RT_AT(5, 1062);
    if (rt_cond(rt_ne(v1050_parent, RT_NULL))) {
      RT_AT(5, 1063);
      (void)({ wv t2108 = v1051_all; wv t2109 = (rt_cond(({ wv t2100 = v1050_parent; wv t2101 = rt_def(up->v710_cur, "cur"); rt_eq(t2100, t2101); })) ? rt_s(&S636) : rt_cat(2, (wv[]){ rt_s(&S549), rt_text(rt_chk(w6_repeat(rt_s(&S550), rt_sub(({ wv t2106 = rt_get(&F_depth, ({ wv t2102 = rt_def(up->v710_cur, "cur"); wv t2103 = rt_def(up->v702_fns, "fns"); rt_idx(t2102, t2103); })); wv t2107 = rt_get(&F_depth, ({ wv t2104 = v1050_parent; wv t2105 = rt_def(up->v702_fns, "fns"); rt_idx(t2104, t2105); })); rt_sub(t2106, t2107); }), rt_i(1))), "callC")) })); std_push(t2108, t2109); });
    }
    RT_AT(5, 1065);
    wv v1052_k = rt_i(0);
    RT_AT(5, 1066);
    while (rt_cond((RT_AT(5, 1066), ({ wv t2110 = v1052_k; wv t2111 = std_length(v1046_args); rt_lt(t2110, t2111); })))) {
      RT_AT(5, 1066);
      (void)({ wv t2112 = v1051_all; wv t2113 = rt_idx(v1052_k, v1046_args); std_push(t2112, t2113); });
      RT_AT(5, 1066);
      v1052_k = rt_add(v1052_k, rt_i(1));
    }
    RT_AT(5, 1067);
    v1048_code = ({ wv t2114 = rt_text(v1049_f); wv t2115 = rt_text(std_join(v1051_all, rt_s(&S437))); rt_cat(4, (wv[]){ t2114, rt_s(&S98), t2115, rt_s(&S16) }); });
  }
  RT_RET((RT_AT(5, 1069), (rt_cond(v1047_checked) ? ({ wv t2120 = rt_text(v1048_code); wv t2121 = rt_text((rt_cond(rt_get(&F_top, ({ wv t2116 = rt_def(up->v710_cur, "cur"); wv t2117 = rt_def(up->v702_fns, "fns"); rt_idx(t2116, t2117); }))) ? rt_s(&S536) : rt_chk(w1_quote(rt_get(&F_name, ({ wv t2118 = rt_def(up->v710_cur, "cur"); wv t2119 = rt_def(up->v702_fns, "fns"); rt_idx(t2118, t2119); }))), "callC"))); rt_cat(5, (wv[]){ rt_s(&S637), t2120, rt_s(&S437), t2121, rt_s(&S16) }); }) : v1048_code)));
}

static wv s774_expr(struct fr146 *up, wv v1056_e) {
  rt_enter("expr");
  RT_AT(5, 1073);
  wv v1057_kind = rt_bind(rt_get(&F_k, v1056_e), 0);
  RT_AT(5, 1074);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S139)))) {
    RT_AT(5, 1074);
    RT_RET(({ wv t2122 = rt_cat(3, (wv[]){ rt_s(&S570), rt_text(rt_get(&F_v, v1056_e)), rt_s(&S16) }); wv t2123 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2122, t2123 }); }));
  }
  RT_AT(5, 1075);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S149)))) {
    RT_AT(5, 1075);
    RT_RET(({ wv t2124 = rt_cat(3, (wv[]){ rt_s(&S638), rt_text(rt_get(&F_v, v1056_e)), rt_s(&S16) }); wv t2125 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2124, t2125 }); }));
  }
  RT_AT(5, 1076);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S173)))) {
    RT_AT(5, 1077);
    wv v1058_parts = rt_arr(0, NULL, 1);
    RT_AT(5, 1078);
    wv v1059_k = rt_i(0);
    RT_AT(5, 1079);
    while (rt_cond((RT_AT(5, 1079), ({ wv t2126 = v1059_k; wv t2127 = std_length(rt_get(&F_parts, v1056_e)); rt_lt(t2126, t2127); })))) {
      RT_AT(5, 1080);
      wv v1061_part = rt_bind(({ wv t2128 = v1059_k; wv t2129 = rt_get(&F_parts, v1056_e); rt_idx(t2128, t2129); }), 0);
      RT_AT(5, 1081);
      if (rt_cond(rt_eq(std_typeOf(v1061_part), rt_s(&S187)))) {
        RT_AT(5, 1082);
        if (rt_cond(rt_ne(v1061_part, rt_s(&S13)))) {
          RT_AT(5, 1082);
          (void)({ wv t2132 = v1058_parts; wv t2133 = ({ wv t2130 = rt_cat(3, (wv[]){ rt_s(&S639), rt_text(rt_chk(s732_str(up, v1061_part), "expr")), rt_s(&S16) }); wv t2131 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2130, t2131 }); }); std_push(t2132, t2133); });
        }
      } else {
        RT_AT(5, 1084);
        (void)({ wv t2136 = v1058_parts; wv t2137 = ({ wv t2134 = rt_cat(3, (wv[]){ rt_s(&S640), rt_text(rt_get(&F_c, rt_chk(s774_expr(up, v1061_part), "expr"))), rt_s(&S16) }); wv t2135 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2134, t2135 }); }); std_push(t2136, t2137); });
      }
      RT_AT(5, 1086);
      v1059_k = rt_add(v1059_k, rt_i(1));
    }
    RT_AT(5, 1088);
    if (rt_cond(rt_eq(std_length(v1058_parts), rt_i(0)))) {
      RT_AT(5, 1088);
      RT_RET(({ wv t2138 = rt_cat(3, (wv[]){ rt_s(&S639), rt_text(rt_chk(s732_str(up, rt_s(&S13)), "expr")), rt_s(&S16) }); wv t2139 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2138, t2139 }); }));
    }
    RT_AT(5, 1089);
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v1058_parts), rt_i(1)), "&&") && rt_bool(({ wv t2140 = rt_get(&F_k, rt_idx(rt_i(0), v1058_parts)); wv t2141 = rt_def(w6_constK, "constK"); rt_eq(t2140, t2141); }), "&&")))) {
      RT_AT(5, 1089);
      RT_RET(rt_idx(rt_i(0), v1058_parts));
    }
    RT_AT(5, 1090);
    wv v1060_sq = rt_bind(rt_chk(s741_seq(up, v1058_parts), "expr"), 0);
    RT_AT(5, 1091);
    RT_RET(({ wv t2144 = v1060_sq; wv t2145 = ({ wv t2142 = rt_text(std_length(rt_get(&F_cs, v1060_sq))); wv t2143 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1060_sq)), "expr")); rt_cat(5, (wv[]){ rt_s(&S641), t2142, rt_s(&S437), t2143, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t2144, t2145), "expr"); }));
  }
  RT_AT(5, 1093);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S300)))) {
    RT_AT(5, 1093);
    RT_RET(({ wv t2146 = rt_cat(3, (wv[]){ rt_s(&S642), rt_text((rt_cond(rt_get(&F_v, v1056_e)) ? rt_i(1) : rt_i(0))), rt_s(&S16) }); wv t2147 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2146, t2147 }); }));
  }
  RT_AT(5, 1094);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S52)))) {
    RT_AT(5, 1094);
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S559), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1095);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S53)))) {
    RT_AT(5, 1095);
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S643), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1096);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S54)))) {
    RT_AT(5, 1096);
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S644), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1097);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S214)))) {
    RT_AT(5, 1097);
    RT_RET(rt_chk(s740_read(up, ({ wv t2148 = rt_get(&F_pos, v1056_e); wv t2149 = rt_def(up->v707_curSlot, "curSlot"); rt_idx(t2148, t2149); })), "expr"));
  }
  RT_AT(5, 1098);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S157)))) {
    RT_AT(5, 1098);
    RT_RET(({ wv t2150 = rt_cat(3, (wv[]){ rt_s(&S635), rt_text(rt_get(&F_name, v1056_e)), rt_s(&S645) }); wv t2151 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2150, t2151 }); }));
  }
  RT_AT(5, 1099);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S188)))) {
    RT_AT(5, 1099);
    RT_RET(rt_chk(s756_array(up, v1056_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1100);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S189)))) {
    RT_AT(5, 1100);
    RT_RET(rt_chk(s757_mapLit(up, v1056_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1101);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S190)))) {
    RT_AT(5, 1101);
    RT_RET(rt_chk(s758_record(up, v1056_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1102);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S191)))) {
    RT_AT(5, 1103);
    wv v1062_sq = rt_bind(rt_chk(s741_seq(up, ({ wv t2152 = rt_chk(s774_expr(up, rt_get(&F_index, v1056_e)), "expr"); wv t2153 = rt_chk(s774_expr(up, rt_get(&F_arr, v1056_e)), "expr"); rt_arr(2, (wv[]){ t2152, t2153 }, 0); })), "expr"), 0);
    RT_AT(5, 1104);
    RT_RET(({ wv t2156 = v1062_sq; wv t2157 = ({ wv t2154 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1062_sq))); wv t2155 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1062_sq))); rt_cat(5, (wv[]){ rt_s(&S646), t2154, rt_s(&S437), t2155, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t2156, t2157), "expr"); }));
  }
  RT_AT(5, 1106);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S192)))) {
    RT_AT(5, 1109);
    wv v1063_lin = rt_bind(rt_chk(s737_enumOf(up, rt_get(&F_rec, v1056_e)), "expr"), 0);
    RT_AT(5, 1110);
    if (rt_cond(rt_ne(v1063_lin, RT_NULL))) {
      RT_AT(5, 1111);
      wv v1067_ms = rt_bind(rt_get(&F_members, rt_get(&F_node, v1063_lin)), 0);
      RT_AT(5, 1112);
      wv v1068_k = rt_i(0);
      RT_AT(5, 1113);
      while (rt_cond((RT_AT(5, 1113), ({ wv t2158 = rt_get(&F_name, rt_idx(v1068_k, v1067_ms)); wv t2159 = rt_get(&F_field, v1056_e); rt_ne(t2158, t2159); })))) {
        RT_AT(5, 1113);
        v1068_k = rt_add(v1068_k, rt_i(1));
      }
      RT_AT(5, 1114);
      RT_RET(({ wv t2162 = ({ wv t2160 = rt_text(rt_get(&F_c, v1063_lin)); wv t2161 = rt_text(v1068_k); rt_cat(5, (wv[]){ rt_s(&S587), t2160, rt_s(&S99), t2161, rt_s(&S588) }); }); wv t2163 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2162, t2163 }); }));
    }
    RT_AT(5, 1116);
    wv v1064_lib = rt_bind(rt_chk(s735_stolenFrom(up, rt_get(&F_rec, v1056_e)), "expr"), 0);
    RT_AT(5, 1117);
    if (rt_cond(rt_ne(v1064_lib, RT_NULL))) {
      RT_AT(5, 1117);
      RT_RET(({ wv t2166 = ({ wv t2164 = rt_text(rt_chk(s714_modId(up, v1064_lib), "expr")); wv t2165 = rt_text(rt_get(&F_field, v1056_e)); rt_cat(4, (wv[]){ rt_s(&S539), t2164, rt_s(&S12), t2165 }); }); wv t2167 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K55, (wv[]){ t2166, t2167 }); }));
    }
    RT_AT(5, 1118);
    wv v1065_key = ({ wv t2168 = rt_chk(s733_fieldRef(up, rt_get(&F_field, v1056_e)), "expr"); wv t2169 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2168, t2169 }); });
    RT_AT(5, 1119);
    wv v1066_sq = rt_bind(rt_chk(s741_seq(up, ({ wv t2170 = v1065_key; wv t2171 = rt_chk(s774_expr(up, rt_get(&F_rec, v1056_e)), "expr"); rt_arr(2, (wv[]){ t2170, t2171 }, 0); })), "expr"), 0);
    RT_AT(5, 1120);
    RT_RET(({ wv t2174 = v1066_sq; wv t2175 = ({ wv t2172 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1066_sq))); wv t2173 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1066_sq))); rt_cat(5, (wv[]){ rt_s(&S647), t2172, rt_s(&S437), t2173, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t2174, t2175), "expr"); }));
  }
  RT_AT(5, 1122);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S193)))) {
    RT_AT(5, 1122);
    RT_RET(({ wv t2178 = ({ wv t2176 = rt_text(rt_chk(w6_unFn(rt_get(&F_op, v1056_e)), "expr")); wv t2177 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_e, v1056_e)), "expr"))); rt_cat(5, (wv[]){ rt_s(&S610), t2176, rt_s(&S98), t2177, rt_s(&S16) }); }); wv t2179 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2178, t2179 }); }));
  }
  RT_AT(5, 1123);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S194)))) {
    RT_AT(5, 1124);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v1056_e), rt_s(&S81)), "||") || rt_bool(rt_eq(rt_get(&F_op, v1056_e), rt_s(&S82)), "||")))) {
      RT_AT(5, 1125);
      RT_RET(({ wv t2185 = ({ wv t2180 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_l, v1056_e)), "expr"))); wv t2181 = rt_text(rt_get(&F_op, v1056_e)); wv t2182 = rt_text(rt_get(&F_op, v1056_e)); wv t2183 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_r, v1056_e)), "expr"))); wv t2184 = rt_text(rt_get(&F_op, v1056_e)); rt_cat(11, (wv[]){ rt_s(&S648), t2180, rt_s(&S649), t2181, rt_s(&S650), t2182, rt_s(&S651), t2183, rt_s(&S649), t2184, rt_s(&S652) }); }); wv t2186 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2185, t2186 }); }));
    }
    RT_AT(5, 1127);
    wv v1069_sq = rt_bind(rt_chk(s741_seq(up, ({ wv t2187 = rt_chk(s774_expr(up, rt_get(&F_l, v1056_e)), "expr"); wv t2188 = rt_chk(s774_expr(up, rt_get(&F_r, v1056_e)), "expr"); rt_arr(2, (wv[]){ t2187, t2188 }, 0); })), "expr"), 0);
    RT_AT(5, 1128);
    RT_RET(({ wv t2192 = v1069_sq; wv t2193 = ({ wv t2189 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v1056_e)), "expr")); wv t2190 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1069_sq))); wv t2191 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1069_sq))); rt_cat(7, (wv[]){ rt_s(&S610), t2189, rt_s(&S98), t2190, rt_s(&S437), t2191, rt_s(&S16) }); }); rt_chk(s742_wrap(up, t2192, t2193), "expr"); }));
  }
  RT_AT(5, 1130);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S195)))) {
    RT_AT(5, 1131);
    RT_RET(({ wv t2197 = ({ wv t2194 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_c, v1056_e)), "expr"))); wv t2195 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_a, v1056_e)), "expr"))); wv t2196 = rt_text(rt_get(&F_c, rt_chk(s774_expr(up, rt_get(&F_b, v1056_e)), "expr"))); rt_cat(7, (wv[]){ rt_s(&S653), t2194, rt_s(&S654), t2195, rt_s(&S627), t2196, rt_s(&S16) }); }); wv t2198 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2197, t2198 }); }));
  }
  RT_AT(5, 1133);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S184)))) {
    RT_AT(5, 1134);
    wv v1070_sq = rt_bind(rt_chk(s741_seq(up, rt_chk(s755_exprs(up, rt_get(&F_args, v1056_e)), "expr")), "expr"), 0);
    RT_AT(5, 1135);
    RT_RET(({ wv t2201 = v1070_sq; wv t2202 = ({ wv t2199 = rt_get(&F_callee, v1056_e); wv t2200 = rt_get(&F_cs, v1070_sq); rt_chk(s773_callC(up, t2199, t2200, rt_b(1)), "expr"); }); rt_chk(s742_wrap(up, t2201, t2202), "expr"); }));
  }
  RT_AT(5, 1137);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S186)))) {
    RT_AT(5, 1137);
    RT_RET(({ wv t2203 = rt_chk(s768_chainExpr(up, v1056_e), "expr"); wv t2204 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2203, t2204 }); }));
  }
  RT_AT(5, 1138);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S45)))) {
    RT_AT(5, 1138);
    RT_RET(({ wv t2205 = rt_chk(s772_matchExpr(up, v1056_e), "expr"); wv t2206 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2205, t2206 }); }));
  }
  RT_AT(5, 1139);
  if (rt_cond(rt_eq(v1057_kind, rt_s(&S621)))) {
    RT_AT(5, 1139);
    RT_RET(({ wv t2207 = rt_get(&F_js, v1056_e); wv t2208 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2207, t2208 }); }));
  }
  RT_AT(5, 1140);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S655), rt_text(v1057_kind) }));
  RT_RET(RT_NULL);
}

static wv s775_assemble(struct fr146 *up) {
  rt_enter("assemble");
  RT_AT(5, 1146);
  wv v1071_all = rt_arr(3, (wv[]){ std_join(std_split(up->v687_runtime, rt_s(&S656)), rt_s(&S2)), rt_s(&S657), rt_s(&S13) }, 1);
  RT_AT(5, 1147);
  wv v1072_k = rt_i(0);
  RT_AT(5, 1148);
  while (rt_cond((RT_AT(5, 1148), ({ wv t2209 = v1072_k; wv t2210 = std_length(rt_def(up->v690_strings, "strings")); rt_lt(t2209, t2210); })))) {
    RT_AT(5, 1148);
    (void)({ wv t2215 = v1071_all; wv t2216 = ({ wv t2213 = rt_text(v1072_k); wv t2214 = rt_text(rt_chk(w6_wstrInit(({ wv t2211 = v1072_k; wv t2212 = rt_def(up->v690_strings, "strings"); rt_idx(t2211, t2212); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S658), t2213, rt_s(&S554), t2214, rt_s(&S14) }); }); std_push(t2215, t2216); });
    RT_AT(5, 1148);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1149);
  v1072_k = rt_i(0);
  RT_AT(5, 1150);
  while (rt_cond((RT_AT(5, 1150), ({ wv t2217 = v1072_k; wv t2218 = std_length(rt_def(up->v691_fields, "fields")); rt_lt(t2217, t2218); })))) {
    RT_AT(5, 1150);
    (void)({ wv t2225 = v1071_all; wv t2226 = ({ wv t2223 = rt_text(({ wv t2219 = v1072_k; wv t2220 = rt_def(up->v691_fields, "fields"); rt_idx(t2219, t2220); })); wv t2224 = rt_text(rt_chk(w6_wstrInit(({ wv t2221 = v1072_k; wv t2222 = rt_def(up->v691_fields, "fields"); rt_idx(t2221, t2222); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S659), t2223, rt_s(&S554), t2224, rt_s(&S14) }); }); std_push(t2225, t2226); });
    RT_AT(5, 1150);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1151);
  v1072_k = rt_i(0);
  RT_AT(5, 1152);
  while (rt_cond((RT_AT(5, 1152), ({ wv t2227 = v1072_k; wv t2228 = std_length(rt_def(up->v692_shapes, "shapes")); rt_lt(t2227, t2228); })))) {
    RT_AT(5, 1153);
    wv v1074_names = rt_bind(std_split(({ wv t2229 = v1072_k; wv t2230 = rt_def(up->v692_shapes, "shapes"); rt_idx(t2229, t2230); }), rt_s(&S97)), 0);
    RT_AT(5, 1154);
    wv v1075_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1155);
    wv v1076_j = rt_i(0);
    RT_AT(5, 1156);
    while (rt_cond((RT_AT(5, 1156), ({ wv t2231 = v1076_j; wv t2232 = std_length(v1074_names); rt_lt(t2231, t2232); })))) {
      RT_AT(5, 1156);
      (void)({ wv t2233 = v1075_refs; wv t2234 = rt_cat(2, (wv[]){ rt_s(&S543), rt_text(rt_idx(v1076_j, v1074_names)) }); std_push(t2233, t2234); });
      RT_AT(5, 1156);
      v1076_j = rt_add(v1076_j, rt_i(1));
    }
    RT_AT(5, 1157);
    (void)({ wv t2237 = v1071_all; wv t2238 = ({ wv t2235 = rt_text(v1072_k); wv t2236 = rt_text(std_join(v1075_refs, rt_s(&S437))); rt_cat(5, (wv[]){ rt_s(&S660), t2235, rt_s(&S545), t2236, rt_s(&S546) }); }); std_push(t2237, t2238); });
    RT_AT(5, 1158);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1160);
  v1072_k = rt_i(0);
  RT_AT(5, 1161);
  while (rt_cond((RT_AT(5, 1161), ({ wv t2239 = v1072_k; wv t2240 = std_length(rt_def(up->v693_enumDefs, "enumDefs")); rt_lt(t2239, t2240); })))) {
    RT_AT(5, 1161);
    (void)({ wv t2243 = v1071_all; wv t2244 = ({ wv t2241 = v1072_k; wv t2242 = rt_def(up->v693_enumDefs, "enumDefs"); rt_idx(t2241, t2242); }); std_push(t2243, t2244); });
    RT_AT(5, 1161);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1162);
  v1072_k = rt_i(0);
  RT_AT(5, 1163);
  while (rt_cond((RT_AT(5, 1163), ({ wv t2245 = v1072_k; wv t2246 = std_length(rt_def(up->v695_frames, "frames")); rt_lt(t2245, t2246); })))) {
    RT_AT(5, 1164);
    wv v1077_f = rt_bind(({ wv t2247 = v1072_k; wv t2248 = rt_def(up->v695_frames, "frames"); rt_idx(t2247, t2248); }), 0);
    RT_AT(5, 1165);
    wv v1078_members = rt_arr(0, NULL, 1);
    RT_AT(5, 1166);
    wv v1079_pf = rt_bind(rt_get(&F_parent, ({ wv t2249 = v1077_f; wv t2250 = rt_def(up->v702_fns, "fns"); rt_idx(t2249, t2250); })), 0);
    RT_AT(5, 1167);
    if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2251 = v1079_pf; wv t2252 = rt_def(up->v702_fns, "fns"); rt_idx(t2251, t2252); }))))) {
      RT_AT(5, 1167);
      (void)({ wv t2253 = v1078_members; wv t2254 = rt_cat(3, (wv[]){ rt_s(&S600), rt_text(v1079_pf), rt_s(&S661) }); std_push(t2253, t2254); });
    }
    RT_AT(5, 1168);
    wv v1080_j = rt_i(0);
    RT_AT(5, 1169);
    while (rt_cond((RT_AT(5, 1169), ({ wv t2255 = v1080_j; wv t2256 = std_length(rt_def(up->v701_decls, "decls")); rt_lt(t2255, t2256); })))) {
      RT_AT(5, 1170);
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(({ wv t2259 = rt_get(&F_func, ({ wv t2257 = v1080_j; wv t2258 = rt_def(up->v701_decls, "decls"); rt_idx(t2257, t2258); })); wv t2260 = v1077_f; rt_eq(t2259, t2260); }), "&&") && rt_bool(rt_get(&F_captured, ({ wv t2261 = v1080_j; wv t2262 = rt_def(up->v701_decls, "decls"); rt_idx(t2261, t2262); })), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_kind, ({ wv t2263 = v1080_j; wv t2264 = rt_def(up->v701_decls, "decls"); rt_idx(t2263, t2264); })), rt_s(&S33)), "&&")))) {
        RT_AT(5, 1171);
        (void)({ wv t2267 = v1078_members; wv t2268 = rt_cat(3, (wv[]){ rt_s(&S553), rt_text(rt_get(&F_cname, ({ wv t2265 = v1080_j; wv t2266 = rt_def(up->v701_decls, "decls"); rt_idx(t2265, t2266); }))), rt_s(&S14) }); std_push(t2267, t2268); });
      }
      RT_AT(5, 1173);
      v1080_j = rt_add(v1080_j, rt_i(1));
    }
    RT_AT(5, 1175);
    if (rt_cond(rt_eq(std_length(v1078_members), rt_i(0)))) {
      RT_AT(5, 1175);
      (void)std_push(v1078_members, rt_s(&S662));
    }
    RT_AT(5, 1176);
    (void)({ wv t2271 = v1071_all; wv t2272 = ({ wv t2269 = rt_text(v1077_f); wv t2270 = rt_text(std_join(v1078_members, rt_s(&S0))); rt_cat(5, (wv[]){ rt_s(&S600), t2269, rt_s(&S663), t2270, rt_s(&S546) }); }); std_push(t2271, t2272); });
    RT_AT(5, 1177);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1179);
  v1072_k = rt_i(0);
  RT_AT(5, 1180);
  while (rt_cond((RT_AT(5, 1180), ({ wv t2273 = v1072_k; wv t2274 = std_length(rt_def(up->v694_globals, "globals")); rt_lt(t2273, t2274); })))) {
    RT_AT(5, 1180);
    (void)({ wv t2277 = v1071_all; wv t2278 = rt_cat(3, (wv[]){ rt_s(&S603), rt_text(({ wv t2275 = v1072_k; wv t2276 = rt_def(up->v694_globals, "globals"); rt_idx(t2275, t2276); })), rt_s(&S14) }); std_push(t2277, t2278); });
    RT_AT(5, 1180);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1181);
  v1072_k = rt_i(0);
  RT_AT(5, 1182);
  while (rt_cond((RT_AT(5, 1182), ({ wv t2279 = v1072_k; wv t2280 = std_length(rt_def(up->v696_functions, "functions")); rt_lt(t2279, t2280); })))) {
    RT_AT(5, 1182);
    (void)({ wv t2283 = v1071_all; wv t2284 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2281 = v1072_k; wv t2282 = rt_def(up->v696_functions, "functions"); rt_idx(t2281, t2282); }))), rt_s(&S14) }); std_push(t2283, t2284); });
    RT_AT(5, 1182);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1183);
  v1072_k = rt_i(0);
  RT_AT(5, 1184);
  while (rt_cond((RT_AT(5, 1184), ({ wv t2285 = v1072_k; wv t2286 = std_length(rt_def(up->v696_functions, "functions")); rt_lt(t2285, t2286); })))) {
    RT_AT(5, 1185);
    (void)std_push(v1071_all, rt_s(&S13));
    RT_AT(5, 1186);
    (void)({ wv t2289 = v1071_all; wv t2290 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2287 = v1072_k; wv t2288 = rt_def(up->v696_functions, "functions"); rt_idx(t2287, t2288); }))), rt_s(&S664) }); std_push(t2289, t2290); });
    RT_AT(5, 1187);
    wv v1081_lines = rt_bind(rt_get(&F_lines, ({ wv t2291 = v1072_k; wv t2292 = rt_def(up->v696_functions, "functions"); rt_idx(t2291, t2292); })), 0);
    RT_AT(5, 1188);
    wv v1082_j = rt_i(0);
    RT_AT(5, 1189);
    while (rt_cond((RT_AT(5, 1189), ({ wv t2293 = v1082_j; wv t2294 = std_length(v1081_lines); rt_lt(t2293, t2294); })))) {
      RT_AT(5, 1189);
      (void)({ wv t2295 = v1071_all; wv t2296 = rt_idx(v1082_j, v1081_lines); std_push(t2295, t2296); });
      RT_AT(5, 1189);
      v1082_j = rt_add(v1082_j, rt_i(1));
    }
    RT_AT(5, 1190);
    (void)std_push(v1071_all, rt_s(&S15));
    RT_AT(5, 1191);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1193);
  (void)std_push(v1071_all, rt_s(&S13));
  RT_AT(5, 1194);
  wv v1073_files = rt_arr(0, NULL, 1);
  RT_AT(5, 1195);
  {
    wfor t2298;
    rt_for_start(&t2298, up->v686_modules, 0);
    for (int64_t t2297 = 0; rt_for_guard(&t2298, up->v686_modules, "modules"), rt_for_more(&t2298, t2297); t2297++) {
      wv v1083_m = rt_for_item(&t2298, t2297);
      RT_AT(5, 1195);
      (void)({ wv t2299 = v1073_files; wv t2300 = rt_chk(w1_quote(rt_get(&F_name, v1083_m)), "assemble"); std_push(t2299, t2300); });
    }
  }
  RT_AT(5, 1196);
  (void)({ wv t2301 = v1071_all; wv t2302 = rt_cat(3, (wv[]){ rt_s(&S665), rt_text(std_join(v1073_files, rt_s(&S437))), rt_s(&S546) }); std_push(t2301, t2302); });
  RT_AT(5, 1197);
  (void)std_push(v1071_all, rt_s(&S13));
  RT_AT(5, 1198);
  (void)std_push(v1071_all, rt_s(&S666));
  RT_AT(5, 1199);
  (void)std_push(v1071_all, rt_s(&S667));
  RT_AT(5, 1200);
  if (rt_cond(rt_gt(std_length(rt_def(up->v691_fields, "fields")), rt_i(0)))) {
    RT_AT(5, 1201);
    wv v1084_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1202);
    v1072_k = rt_i(0);
    RT_AT(5, 1203);
    while (rt_cond((RT_AT(5, 1203), ({ wv t2303 = v1072_k; wv t2304 = std_length(rt_def(up->v691_fields, "fields")); rt_lt(t2303, t2304); })))) {
      RT_AT(5, 1203);
      (void)({ wv t2307 = v1084_refs; wv t2308 = rt_cat(2, (wv[]){ rt_s(&S543), rt_text(({ wv t2305 = v1072_k; wv t2306 = rt_def(up->v691_fields, "fields"); rt_idx(t2305, t2306); })) }); std_push(t2307, t2308); });
      RT_AT(5, 1203);
      v1072_k = rt_add(v1072_k, rt_i(1));
    }
    RT_AT(5, 1204);
    (void)({ wv t2311 = v1071_all; wv t2312 = ({ wv t2309 = rt_text(std_length(rt_def(up->v691_fields, "fields"))); wv t2310 = rt_text(std_join(v1084_refs, rt_s(&S437))); rt_cat(5, (wv[]){ rt_s(&S668), t2309, rt_s(&S669), t2310, rt_s(&S670) }); }); std_push(t2311, t2312); });
  }
  RT_AT(5, 1207);
  if (rt_cond(rt_gt(std_length(rt_def(up->v694_globals, "globals")), rt_i(0)))) {
    RT_AT(5, 1208);
    wv v1085_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1209);
    v1072_k = rt_i(0);
    RT_AT(5, 1210);
    while (rt_cond((RT_AT(5, 1210), ({ wv t2313 = v1072_k; wv t2314 = std_length(rt_def(up->v694_globals, "globals")); rt_lt(t2313, t2314); })))) {
      RT_AT(5, 1210);
      (void)({ wv t2317 = v1085_refs; wv t2318 = rt_cat(2, (wv[]){ rt_s(&S92), rt_text(({ wv t2315 = v1072_k; wv t2316 = rt_def(up->v694_globals, "globals"); rt_idx(t2315, t2316); })) }); std_push(t2317, t2318); });
      RT_AT(5, 1210);
      v1072_k = rt_add(v1072_k, rt_i(1));
    }
    RT_AT(5, 1211);
    (void)({ wv t2321 = v1071_all; wv t2322 = ({ wv t2319 = rt_text(std_length(rt_def(up->v694_globals, "globals"))); wv t2320 = rt_text(std_join(v1085_refs, rt_s(&S437))); rt_cat(5, (wv[]){ rt_s(&S671), t2319, rt_s(&S672), t2320, rt_s(&S670) }); }); std_push(t2321, t2322); });
  }
  RT_AT(5, 1213);
  v1072_k = rt_i(0);
  RT_AT(5, 1214);
  while (rt_cond((RT_AT(5, 1214), ({ wv t2323 = v1072_k; wv t2324 = std_length(rt_def(up->v697_out, "out")); rt_lt(t2323, t2324); })))) {
    RT_AT(5, 1214);
    (void)({ wv t2327 = v1071_all; wv t2328 = ({ wv t2325 = v1072_k; wv t2326 = rt_def(up->v697_out, "out"); rt_idx(t2325, t2326); }); std_push(t2327, t2328); });
    RT_AT(5, 1214);
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_AT(5, 1215);
  (void)std_push(v1071_all, rt_s(&S15));
  RT_AT(5, 1216);
  (void)std_push(v1071_all, rt_s(&S13));
  RT_RET((RT_AT(5, 1217), std_join(v1071_all, rt_s(&S2))));
}

static wv w7_dumpTokens(wv v1088_toks, wv v1089_indent) {
  rt_enter("dumpTokens");
  v1088_toks = rt_bind(v1088_toks, 0);
  v1089_indent = rt_bind(v1089_indent, 0);
  RT_AT(6, 8);
  wv v1090_k = rt_i(0);
  RT_AT(6, 9);
  while (rt_cond((RT_AT(6, 9), ({ wv t2339 = v1090_k; wv t2340 = std_length(v1088_toks); rt_lt(t2339, t2340); })))) {
    RT_AT(6, 10);
    wv v1091_tk = rt_bind(rt_idx(v1090_k, v1088_toks), 0);
    RT_AT(6, 11);
    wv v1092_line = ({ wv t2341 = rt_text(v1089_indent); wv t2342 = rt_text(rt_get(&F_t, v1091_tk)); wv t2343 = rt_text(rt_get(&F_pos, v1091_tk)); wv t2344 = rt_text(rt_get(&F_end, v1091_tk)); wv t2345 = rt_text(rt_get(&F_sp, v1091_tk)); rt_cat(8, (wv[]){ t2341, t2342, rt_s(&S0), t2343, rt_s(&S0), t2344, rt_s(&S0), t2345 }); });
    RT_AT(6, 12);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1091_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1091_tk), rt_s(&S157)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1091_tk), rt_s(&S112)), "||")))) {
      RT_AT(6, 13);
      v1092_line = ({ wv t2346 = v1092_line; wv t2347 = rt_cat(2, (wv[]){ rt_s(&S0), rt_text(rt_get(&F_v, v1091_tk)) }); rt_add(t2346, t2347); });
    } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1091_tk), rt_s(&S139)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1091_tk), rt_s(&S149)), "||")))) {
      RT_AT(6, 15);
      v1092_line = ({ wv t2350 = v1092_line; wv t2351 = ({ wv t2348 = rt_text(rt_get(&F_text, v1091_tk)); wv t2349 = rt_text(rt_get(&F_v, v1091_tk)); rt_cat(4, (wv[]){ rt_s(&S0), t2348, rt_s(&S0), t2349 }); }); rt_add(t2350, t2351); });
    }
    RT_AT(6, 17);
    (void)std_print(v1092_line);
    RT_AT(6, 18);
    if (rt_cond(rt_eq(rt_get(&F_t, v1091_tk), rt_s(&S173)))) {
      RT_AT(6, 19);
      wv v1093_p = rt_i(0);
      RT_AT(6, 20);
      while (rt_cond((RT_AT(6, 20), ({ wv t2352 = v1093_p; wv t2353 = std_length(rt_get(&F_parts, v1091_tk)); rt_lt(t2352, t2353); })))) {
        RT_AT(6, 21);
        wv v1094_part = rt_bind(({ wv t2354 = v1093_p; wv t2355 = rt_get(&F_parts, v1091_tk); rt_idx(t2354, t2355); }), 0);
        RT_AT(6, 22);
        if (rt_cond(rt_eq(std_typeOf(v1094_part), rt_s(&S187)))) {
          RT_AT(6, 23);
          (void)std_print(({ wv t2356 = rt_text(v1089_indent); wv t2357 = rt_text(rt_chk(w1_quote(v1094_part), "dumpTokens")); rt_cat(3, (wv[]){ t2356, rt_s(&S673), t2357 }); }));
        } else {
          RT_AT(6, 25);
          (void)std_print(({ wv t2358 = rt_text(v1089_indent); wv t2359 = rt_text(rt_get(&F_pos, v1094_part)); rt_cat(3, (wv[]){ t2358, rt_s(&S674), t2359 }); }));
          RT_AT(6, 26);
          (void)({ wv t2360 = rt_get(&F_tokens, v1094_part); wv t2361 = rt_add(v1089_indent, rt_s(&S675)); rt_chk(w7_dumpTokens(t2360, t2361), "dumpTokens"); });
          RT_AT(6, 27);
          (void)std_print(rt_cat(2, (wv[]){ rt_text(v1089_indent), rt_s(&S676) }));
        }
        RT_AT(6, 29);
        v1093_p = rt_add(v1093_p, rt_i(1));
      }
    }
    RT_AT(6, 32);
    v1090_k = rt_add(v1090_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w7_sorted(wv v1095_xs) {
  rt_enter("sorted");
  v1095_xs = rt_bind(v1095_xs, 0);
  RT_AT(6, 41);
  wv v1096_out = rt_arr(0, NULL, 1);
  RT_AT(6, 42);
  wv v1097_k = rt_i(0);
  RT_AT(6, 43);
  while (rt_cond((RT_AT(6, 43), ({ wv t2362 = v1097_k; wv t2363 = std_length(v1095_xs); rt_lt(t2362, t2363); })))) {
    RT_AT(6, 44);
    (void)({ wv t2364 = v1096_out; wv t2365 = rt_idx(v1097_k, v1095_xs); std_push(t2364, t2365); });
    RT_AT(6, 45);
    wv v1098_j = rt_sub(std_length(v1096_out), rt_i(1));
    RT_AT(6, 46);
    while (rt_cond((RT_AT(6, 46), rt_b(rt_bool(rt_gt(v1098_j, rt_i(0)), "&&") && rt_bool(({ wv t2368 = ({ wv t2366 = rt_sub(v1098_j, rt_i(1)); wv t2367 = v1096_out; rt_idx(t2366, t2367); }); wv t2369 = rt_idx(v1098_j, v1096_out); rt_gt(t2368, t2369); }), "&&"))))) {
      RT_AT(6, 47);
      wv v1099_swap = rt_bind(rt_idx(v1098_j, v1096_out), 0);
      RT_AT(6, 48);
      ({ wv t2372 = ({ wv t2370 = rt_sub(v1098_j, rt_i(1)); wv t2371 = v1096_out; rt_idx(t2370, t2371); }); wv t2373 = v1098_j; wv t2374 = v1096_out; rt_set_idx(t2372, t2373, t2374); });
      RT_AT(6, 49);
      ({ wv t2375 = v1099_swap; wv t2376 = rt_sub(v1098_j, rt_i(1)); wv t2377 = v1096_out; rt_set_idx(t2375, t2376, t2377); });
      RT_AT(6, 50);
      v1098_j = rt_sub(v1098_j, rt_i(1));
    }
    RT_AT(6, 52);
    v1097_k = rt_add(v1097_k, rt_i(1));
  }
  RT_RET((RT_AT(6, 54), v1096_out));
}

static wv w7_dumpValue(wv v1100_x, wv v1101_indent) {
  rt_enter("dumpValue");
  v1100_x = rt_bind(v1100_x, 0);
  v1101_indent = rt_bind(v1101_indent, 0);
  RT_AT(6, 58);
  wv v1102_type = rt_bind(std_typeOf(v1100_x), 0);
  RT_AT(6, 59);
  if (rt_cond(rt_eq(v1102_type, rt_s(&S187)))) {
    RT_AT(6, 59);
    RT_RET(rt_chk(w1_quote(v1100_x), "dumpValue"));
  }
  RT_AT(6, 60);
  wv v1103_inner = rt_add(v1101_indent, rt_s(&S541));
  RT_AT(6, 61);
  if (rt_cond(rt_eq(v1102_type, rt_s(&S677)))) {
    RT_AT(6, 62);
    if (rt_cond(rt_eq(std_length(v1100_x), rt_i(0)))) {
      RT_AT(6, 62);
      RT_RET(rt_s(&S678));
    }
    RT_AT(6, 63);
    wv v1104_out = rt_s(&S99);
    RT_AT(6, 64);
    wv v1105_k = rt_i(0);
    RT_AT(6, 65);
    while (rt_cond((RT_AT(6, 65), ({ wv t2378 = v1105_k; wv t2379 = std_length(v1100_x); rt_lt(t2378, t2379); })))) {
      RT_AT(6, 66);
      v1104_out = ({ wv t2384 = ({ wv t2380 = rt_add(v1104_out, rt_s(&S2)); wv t2381 = v1103_inner; rt_add(t2380, t2381); }); wv t2385 = ({ wv t2382 = rt_idx(v1105_k, v1100_x); wv t2383 = v1103_inner; rt_chk(w7_dumpValue(t2382, t2383), "dumpValue"); }); rt_add(t2384, t2385); });
      RT_AT(6, 67);
      v1105_k = rt_add(v1105_k, rt_i(1));
    }
    RT_AT(6, 69);
    RT_RET(rt_add(({ wv t2386 = rt_add(v1104_out, rt_s(&S2)); wv t2387 = v1101_indent; rt_add(t2386, t2387); }), rt_s(&S100)));
  }
  RT_AT(6, 71);
  if (rt_cond(rt_eq(v1102_type, rt_s(&S679)))) {
    RT_AT(6, 72);
    wv v1106_keys = rt_bind(rt_chk(w7_sorted(std_fields(v1100_x)), "dumpValue"), 0);
    RT_AT(6, 73);
    wv v1107_out = rt_s(&S101);
    RT_AT(6, 74);
    wv v1108_k = rt_i(0);
    RT_AT(6, 75);
    while (rt_cond((RT_AT(6, 75), ({ wv t2388 = v1108_k; wv t2389 = std_length(v1106_keys); rt_lt(t2388, t2389); })))) {
      RT_AT(6, 76);
      wv v1109_key = rt_bind(rt_idx(v1108_k, v1106_keys), 0);
      RT_AT(6, 77);
      v1107_out = ({ wv t2396 = ({ wv t2392 = v1107_out; wv t2393 = ({ wv t2390 = rt_text(v1103_inner); wv t2391 = rt_text(v1109_key); rt_cat(4, (wv[]){ rt_s(&S2), t2390, t2391, rt_s(&S29) }); }); rt_add(t2392, t2393); }); wv t2397 = ({ wv t2394 = rt_idx(v1109_key, v1100_x); wv t2395 = v1103_inner; rt_chk(w7_dumpValue(t2394, t2395), "dumpValue"); }); rt_add(t2396, t2397); });
      RT_AT(6, 78);
      v1108_k = rt_add(v1108_k, rt_i(1));
    }
    RT_AT(6, 80);
    RT_RET(({ wv t2398 = v1107_out; wv t2399 = rt_cat(3, (wv[]){ rt_s(&S2), rt_text(v1101_indent), rt_s(&S15) }); rt_add(t2398, t2399); }));
  }
  RT_RET((RT_AT(6, 82), std_toString(v1100_x)));
}

static wv w8_dirOf(wv v1110_path) {
  rt_enter("dirOf");
  v1110_path = rt_bind(v1110_path, 0);
  RT_AT(7, 16);
  wv v1111_cs = rt_bind(std_split(v1110_path, rt_s(&S13)), 0);
  RT_AT(7, 17);
  wv v1112_k = rt_sub(std_length(v1111_cs), rt_i(1));
  RT_AT(7, 18);
  while (rt_cond((RT_AT(7, 18), rt_b(rt_bool(rt_ge(v1112_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1112_k, v1111_cs), rt_s(&S88)), "&&"))))) {
    RT_AT(7, 18);
    v1112_k = rt_sub(v1112_k, rt_i(1));
  }
  RT_RET((RT_AT(7, 19), std_join(({ wv t2400 = v1111_cs; wv t2401 = rt_add(v1112_k, rt_i(1)); std_slice(t2400, rt_i(0), t2401); }), rt_s(&S13))));
}

static wv w8_normalize(wv v1113_path) {
  rt_enter("normalize");
  v1113_path = rt_bind(v1113_path, 0);
  RT_AT(7, 24);
  wv v1114_segs = rt_bind(std_split(v1113_path, rt_s(&S88)), 0);
  RT_AT(7, 25);
  wv v1115_out = rt_arr(0, NULL, 1);
  RT_AT(7, 26);
  wv v1116_k = rt_i(0);
  RT_AT(7, 27);
  while (rt_cond((RT_AT(7, 27), ({ wv t2402 = v1116_k; wv t2403 = std_length(v1114_segs); rt_lt(t2402, t2403); })))) {
    RT_AT(7, 28);
    wv v1117_seg = rt_bind(rt_idx(v1116_k, v1114_segs), 0);
    RT_AT(7, 29);
    v1116_k = rt_add(v1116_k, rt_i(1));
    RT_AT(7, 30);
    if (rt_cond(rt_b(rt_bool(rt_eq(v1117_seg, rt_s(&S130)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1117_seg, rt_s(&S13)), "&&") && rt_bool(rt_gt(std_length(v1115_out), rt_i(0)), "&&")), "||")))) {
      continue;
    }
    RT_AT(7, 31);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1117_seg, rt_s(&S253)), "&&") && rt_bool(rt_gt(std_length(v1115_out), rt_i(0)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1115_out), rt_s(&S253)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1115_out), rt_s(&S13)), "&&")))) {
      RT_AT(7, 32);
      (void)std_pop(v1115_out);
    } else {
      RT_AT(7, 34);
      (void)std_push(v1115_out, v1117_seg);
    }
  }
  RT_RET((RT_AT(7, 37), std_join(v1115_out, rt_s(&S88))));
}

static wv w8_with(wv v1118_xs, wv v1119_x) {
  rt_enter("with");
  v1118_xs = rt_bind(v1118_xs, 0);
  v1119_x = rt_bind(v1119_x, 0);
  RT_AT(7, 42);
  wv v1120_out = rt_arr(0, NULL, 1);
  RT_AT(7, 43);
  wv v1121_k = rt_i(0);
  RT_AT(7, 44);
  while (rt_cond((RT_AT(7, 44), ({ wv t2404 = v1121_k; wv t2405 = std_length(v1118_xs); rt_lt(t2404, t2405); })))) {
    RT_AT(7, 45);
    (void)({ wv t2406 = v1120_out; wv t2407 = rt_idx(v1121_k, v1118_xs); std_push(t2406, t2407); });
    RT_AT(7, 46);
    v1121_k = rt_add(v1121_k, rt_i(1));
  }
  RT_AT(7, 48);
  (void)std_push(v1120_out, v1119_x);
  RT_RET((RT_AT(7, 49), v1120_out));
}

static wv w8_failIn(wv v1122_file, wv v1123_chars, wv v1124_pos, wv v1125_message) {
  rt_enter("failIn");
  v1122_file = rt_bind(v1122_file, 0);
  v1123_chars = rt_bind(v1123_chars, 0);
  v1124_pos = rt_bind(v1124_pos, 0);
  v1125_message = rt_bind(v1125_message, 0);
  RT_AT(7, 54);
  (void)rt_chk(w2_setSource(v1122_file, v1123_chars), "failIn");
  RT_AT(7, 55);
  (void)rt_chk(w2_abort(v1124_pos, v1125_message), "failIn");
  RT_RET(RT_NULL);
}

static wv w8_loadProgram(wv v1126_entryFile, wv v1127_entryChars, wv p_v1128_libDir, wv p_v1129_searched) {
  rt_enter("loadProgram");
  struct fr220 fr = { 0 };
  v1126_entryFile = rt_bind(v1126_entryFile, 0);
  fr.v1128_libDir = rt_bind(p_v1128_libDir, 0);
  fr.v1129_searched = rt_bind(p_v1129_searched, 0);
  RT_AT(7, 64);
  fr.v1130_modules = rt_arr(0, NULL, 1);
  RT_AT(7, 65);
  fr.v1131_libRoot = rt_bind((rt_cond(rt_eq(fr.v1128_libDir, RT_NULL)) ? RT_NULL : rt_add(rt_chk(w8_normalize(fr.v1128_libDir), "loadProgram"), rt_s(&S88))), 0);
  RT_AT(7, 131);
  wv v1136_path = rt_bind(rt_chk(w8_normalize(std_join(std_split(v1126_entryFile, rt_s(&S19)), rt_s(&S88))), "loadProgram"), 0);
  RT_AT(7, 132);
  (void)({ wv t2480 = v1126_entryFile; wv t2481 = v1127_entryChars; wv t2482 = v1136_path; wv t2483 = rt_arr(1, (wv[]){ v1136_path }, 0); rt_chk(s1135_visit(&fr, t2480, t2481, t2482, t2483), "loadProgram"); });
  RT_RET((RT_AT(7, 133), fr.v1130_modules));
}

static wv s1132_traceName(struct fr220 *up, wv v1137_path) {
  rt_enter("traceName");
  v1137_path = rt_bind(v1137_path, 0);
  RT_AT(7, 69);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_def(up->v1131_libRoot, "libRoot"), RT_NULL), "||") || rt_bool(({ wv t2408 = std_length(v1137_path); wv t2409 = std_length(rt_def(up->v1131_libRoot, "libRoot")); rt_le(t2408, t2409); }), "||")))) {
    RT_AT(7, 69);
    RT_RET(v1137_path);
  }
  RT_AT(7, 70);
  if (rt_cond(({ wv t2412 = ({ wv t2410 = v1137_path; wv t2411 = std_length(rt_def(up->v1131_libRoot, "libRoot")); std_slice(t2410, rt_i(0), t2411); }); wv t2413 = rt_def(up->v1131_libRoot, "libRoot"); rt_ne(t2412, t2413); }))) {
    RT_AT(7, 70);
    RT_RET(v1137_path);
  }
  RT_RET((RT_AT(7, 71), rt_add(rt_s(&S250), ({ wv t2414 = v1137_path; wv t2415 = std_length(rt_def(up->v1131_libRoot, "libRoot")); wv t2416 = std_length(v1137_path); std_slice(t2414, t2415, t2416); }))));
}

static wv s1133_target(struct fr220 *up, wv v1138_s, wv v1139_file, wv v1140_chars, wv v1141_path) {
  rt_enter("target");
  v1138_s = rt_bind(v1138_s, 0);
  v1139_file = rt_bind(v1139_file, 0);
  v1140_chars = rt_bind(v1140_chars, 0);
  v1141_path = rt_bind(v1141_path, 0);
  RT_AT(7, 77);
  if (rt_cond(rt_ne(std_at(rt_get(&F_path, v1138_s), rt_i(0)), rt_s(&S106)))) {
    RT_AT(7, 77);
    RT_RET(rt_chk(w8_normalize(({ wv t2417 = rt_chk(w8_dirOf(v1141_path), "target"); wv t2418 = rt_get(&F_path, v1138_s); rt_add(t2417, t2418); })), "target"));
  }
  RT_AT(7, 78);
  if (rt_cond(rt_eq(up->v1128_libDir, RT_NULL))) {
    RT_AT(7, 79);
    (void)({ wv t2419 = v1139_file; wv t2420 = v1140_chars; wv t2421 = rt_get(&F_pathPos, v1138_s); wv t2422 = rt_cat(2, (wv[]){ rt_s(&S680), rt_text(std_join(up->v1129_searched, rt_s(&S681))) }); rt_chk(w8_failIn(t2419, t2420, t2421, t2422), "target"); });
  }
  RT_RET((RT_AT(7, 81), rt_chk(w8_normalize(({ wv t2425 = up->v1128_libDir; wv t2426 = ({ wv t2423 = rt_get(&F_path, v1138_s); wv t2424 = std_length(rt_get(&F_path, v1138_s)); std_slice(t2423, rt_i(5), t2424); }); rt_add(t2425, t2426); })), "target")));
}

static wv s1134_loaded(struct fr220 *up, wv v1142_path) {
  rt_enter("loaded");
  v1142_path = rt_bind(v1142_path, 0);
  RT_AT(7, 85);
  wv v1143_k = rt_i(0);
  RT_AT(7, 86);
  while (rt_cond((RT_AT(7, 86), ({ wv t2427 = v1143_k; wv t2428 = std_length(rt_def(up->v1130_modules, "modules")); rt_lt(t2427, t2428); })))) {
    RT_AT(7, 87);
    if (rt_cond(({ wv t2431 = rt_get(&F_path, ({ wv t2429 = v1143_k; wv t2430 = rt_def(up->v1130_modules, "modules"); rt_idx(t2429, t2430); })); wv t2432 = v1142_path; rt_eq(t2431, t2432); }))) {
      RT_AT(7, 87);
      RT_RET(v1143_k);
    }
    RT_AT(7, 88);
    v1143_k = rt_add(v1143_k, rt_i(1));
  }
  RT_RET((RT_AT(7, 90), RT_NULL));
}

static wv s1135_visit(struct fr220 *up, wv v1144_file, wv v1145_chars, wv v1146_path, wv v1147_chain) {
  rt_enter("visit");
  v1144_file = rt_bind(v1144_file, 0);
  v1145_chars = rt_bind(v1145_chars, 0);
  v1146_path = rt_bind(v1146_path, 0);
  v1147_chain = rt_bind(v1147_chain, 0);
  RT_AT(7, 96);
  (void)rt_chk(w2_setSource(v1144_file, v1145_chars), "visit");
  RT_AT(7, 97);
  wv v1148_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(v1145_chars), "visit")), "visit"), 0);
  RT_AT(7, 98);
  wv v1149_imports = rt_arr(0, NULL, 1);
  RT_AT(7, 99);
  wv v1150_k = rt_i(0);
  RT_AT(7, 100);
  while (rt_cond((RT_AT(7, 100), ({ wv t2433 = v1150_k; wv t2434 = std_length(rt_get(&F_stmts, v1148_program)); rt_lt(t2433, t2434); })))) {
    RT_AT(7, 101);
    wv v1153_s = rt_bind(({ wv t2435 = v1150_k; wv t2436 = rt_get(&F_stmts, v1148_program); rt_idx(t2435, t2436); }), 0);
    RT_AT(7, 102);
    v1150_k = rt_add(v1150_k, rt_i(1));
    RT_AT(7, 103);
    if (rt_cond(rt_ne(rt_get(&F_k, v1153_s), rt_s(&S48)))) {
      continue;
    }
    RT_AT(7, 104);
    wv v1154_libPath = rt_bind(rt_chk(s1133_target(up, v1153_s, v1144_file, v1145_chars, v1146_path), "visit"), 0);
    RT_AT(7, 105);
    wv v1155_at = rt_i(0);
    RT_AT(7, 106);
    while (rt_cond((RT_AT(7, 106), rt_b(rt_bool(({ wv t2437 = v1155_at; wv t2438 = std_length(v1147_chain); rt_lt(t2437, t2438); }), "&&") && rt_bool(({ wv t2439 = rt_idx(v1155_at, v1147_chain); wv t2440 = v1154_libPath; rt_ne(t2439, t2440); }), "&&"))))) {
      RT_AT(7, 106);
      v1155_at = rt_add(v1155_at, rt_i(1));
    }
    RT_AT(7, 107);
    if (rt_cond(({ wv t2441 = v1155_at; wv t2442 = std_length(v1147_chain); rt_lt(t2441, t2442); }))) {
      RT_AT(7, 108);
      wv v1157_cycle = rt_bind(({ wv t2446 = ({ wv t2443 = v1147_chain; wv t2444 = v1155_at; wv t2445 = std_length(v1147_chain); std_slice(t2443, t2444, t2445); }); wv t2447 = v1154_libPath; rt_chk(w8_with(t2446, t2447), "visit"); }), 0);
      RT_AT(7, 109);
      (void)({ wv t2448 = v1144_file; wv t2449 = v1145_chars; wv t2450 = rt_get(&F_pathPos, v1153_s); wv t2451 = rt_cat(2, (wv[]){ rt_s(&S682), rt_text(std_join(v1157_cycle, rt_s(&S683))) }); rt_chk(w8_failIn(t2448, t2449, t2450, t2451), "visit"); });
    }
    RT_AT(7, 111);
    wv v1156_lib = rt_bind(rt_chk(s1134_loaded(up, v1154_libPath), "visit"), 1);
    RT_AT(7, 112);
    if (rt_cond(rt_eq(v1156_lib, RT_NULL))) {
      RT_AT(7, 113);
      wv v1158_missing = rt_bind((rt_cond(rt_eq(std_at(rt_get(&F_path, v1153_s), rt_i(0)), rt_s(&S106))) ? ({ wv t2454 = rt_text(({ wv t2452 = rt_get(&F_path, v1153_s); wv t2453 = std_length(rt_get(&F_path, v1153_s)); std_slice(t2452, rt_i(5), t2453); })); wv t2455 = rt_text(v1154_libPath); rt_cat(5, (wv[]){ rt_s(&S684), t2454, rt_s(&S685), t2455, rt_s(&S16) }); }) : rt_cat(3, (wv[]){ rt_s(&S686), rt_text(v1154_libPath), rt_s(&S114) })), 0);
      RT_AT(7, 114);
      wv t2456 = std_readFile(v1154_libPath);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          RT_AT(7, 115);
          (void)({ wv t2457 = v1144_file; wv t2458 = v1145_chars; wv t2459 = rt_get(&F_pathPos, v1153_s); wv t2460 = v1158_missing; rt_chk(w8_failIn(t2457, t2458, t2459, t2460), "visit"); });
          t2456 = (RT_AT(7, 115), RT_NULL);
        }
      }
      RT_AT(7, 114);
      wv v1159_text = rt_bind(t2456, 0);
      RT_AT(7, 116);
      wv v1160_cs = rt_bind(std_split(v1159_text, rt_s(&S13)), 0);
      RT_AT(7, 117);
      wv v1161_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1160_cs), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1160_cs), rt_s(&S687)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
      RT_AT(7, 118);
      v1156_lib = rt_bind(({ wv t2464 = v1154_libPath; wv t2465 = ({ wv t2461 = v1160_cs; wv t2462 = v1161_bom; wv t2463 = std_length(v1160_cs); std_slice(t2461, t2462, t2463); }); wv t2466 = v1154_libPath; wv t2467 = rt_chk(w8_with(v1147_chain, v1154_libPath), "visit"); rt_chk(s1135_visit(up, t2464, t2465, t2466, t2467), "visit"); }), 1);
    }
    RT_AT(7, 120);
    (void)({ wv t2468 = v1149_imports; wv t2469 = rt_rec(0, 2, K59, (wv[]){ v1153_s, v1156_lib }); std_push(t2468, t2469); });
  }
  RT_AT(7, 122);
  wv v1151_refTargets = rt_arr(0, NULL, 1);
  RT_AT(7, 123);
  wv v1152_mod = ({ wv t2470 = v1144_file; wv t2471 = v1145_chars; wv t2472 = v1146_path; wv t2473 = rt_chk(s1132_traceName(up, v1146_path), "visit"); wv t2474 = v1148_program; wv t2475 = v1149_imports; wv t2476 = rt_arr(0, NULL, 0); wv t2477 = v1151_refTargets; rt_rec(1, 9, K60, (wv[]){ t2470, t2471, t2472, t2473, t2474, t2475, t2476, rt_b(0), t2477 }); });
  RT_AT(7, 127);
  (void)({ wv t2478 = rt_def(up->v1130_modules, "modules"); wv t2479 = v1152_mod; std_push(t2478, t2479); });
  RT_RET((RT_AT(7, 128), rt_sub(std_length(rt_def(up->v1130_modules, "modules")), rt_i(1))));
}

static wv w_die(void) {
  rt_enter("die");
  RT_AT(8, 34);
  (void)std_eprint(rt_def(w_usage, "usage"));
  RT_AT(8, 35);
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w_load(wv v1162_file) {
  rt_enter("load");
  v1162_file = rt_bind(v1162_file, 0);
  RT_AT(8, 40);
  wv t2484 = std_readFile(v1162_file);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 41);
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S693), rt_text(v1162_file) }));
      RT_AT(8, 41);
      rt_exit(rt_i(2));
      t2484 = RT_NULL;
    }
  }
  RT_AT(8, 40);
  wv v1163_text = rt_bind(t2484, 0);
  RT_AT(8, 42);
  wv v1164_chars = rt_bind(std_split(v1163_text, rt_s(&S13)), 0);
  RT_AT(8, 43);
  wv v1165_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1164_chars), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1164_chars), rt_s(&S687)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
  RT_AT(8, 44);
  (void)({ wv t2488 = v1162_file; wv t2489 = ({ wv t2485 = v1164_chars; wv t2486 = v1165_bom; wv t2487 = std_length(v1164_chars); std_slice(t2485, t2486, t2487); }); rt_chk(w2_setSource(t2488, t2489), "load"); });
  RT_RET(RT_NULL);
}

static wv w_onWindows(void) {
  rt_enter("onWindows");
  RT_AT(8, 85);
  wv t2494 = rt_s(&S701);
  wv t2495 = std_env(t2494);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2495 = (RT_AT(8, 86), rt_s(&S13));
    }
  }
  RT_AT(8, 85);
  wv v1167_os = rt_bind(t2495, 0);
  RT_RET((RT_AT(8, 87), rt_eq(v1167_os, rt_s(&S702))));
}

static wv w_tryRead(wv v1168_path) {
  rt_enter("tryRead");
  v1168_path = rt_bind(v1168_path, 0);
  RT_AT(8, 104);
  wv t2502 = std_readFile(v1168_path);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2502 = (RT_AT(8, 105), RT_NULL);
    }
  }
  RT_AT(8, 104);
  wv v1169_text = rt_bind(t2502, 0);
  RT_RET((RT_AT(8, 106), v1169_text));
}

static wv w_programDir(void) {
  rt_enter("programDir");
  RT_AT(8, 111);
  wv v1170_cs = rt_bind(std_split(std_programPath(), rt_s(&S13)), 0);
  RT_AT(8, 112);
  wv v1171_k = rt_sub(std_length(v1170_cs), rt_i(1));
  RT_AT(8, 113);
  while (rt_cond((RT_AT(8, 113), rt_b(rt_bool(rt_b(rt_bool(rt_ge(v1171_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1171_k, v1170_cs), rt_s(&S88)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(v1171_k, v1170_cs), rt_s(&S19)), "&&"))))) {
    RT_AT(8, 113);
    v1171_k = rt_sub(v1171_k, rt_i(1));
  }
  RT_RET((RT_AT(8, 114), std_join(({ wv t2503 = v1170_cs; wv t2504 = rt_add(v1171_k, rt_i(1)); std_slice(t2503, rt_i(0), t2504); }), rt_s(&S13))));
}

static wv w_readRuntime(void) {
  rt_enter("readRuntime");
  RT_AT(8, 121);
  wv v1172_text = rt_bind(rt_chk(w_tryRead(rt_def(w_runtimePath, "runtimePath")), "readRuntime"), 0);
  RT_AT(8, 122);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1172_text, RT_NULL), "||") || rt_bool(rt_def(w_runtimeGiven, "runtimeGiven"), "||")))) {
    RT_AT(8, 122);
    RT_RET(v1172_text);
  }
  RT_AT(8, 123);
  wv v1173_dir = rt_bind(rt_chk(w_programDir(), "readRuntime"), 0);
  RT_AT(8, 124);
  wv v1174_here = rt_bind(rt_chk(w_tryRead(({ wv t2505 = v1173_dir; wv t2506 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2505, t2506); })), "readRuntime"), 0);
  RT_RET((RT_AT(8, 125), (rt_cond(rt_ne(v1174_here, RT_NULL)) ? v1174_here : rt_chk(w_tryRead(({ wv t2507 = rt_add(v1173_dir, rt_s(&S709)); wv t2508 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2507, t2508); })), "readRuntime"))));
}

static wv w_findLib(void) {
  rt_enter("findLib");
  RT_AT(8, 134);
  wv v1175_dir = rt_bind(std_split(rt_chk(w_programDir(), "findLib"), rt_s(&S19)), 0);
  RT_AT(8, 135);
  wv v1176_slashed = rt_bind(std_join(v1175_dir, rt_s(&S88)), 0);
  RT_AT(8, 136);
  wv v1177_candidates = rt_bind((rt_cond(rt_ne(rt_def(w_libGiven, "libGiven"), rt_s(&S13))) ? rt_arr(1, (wv[]){ std_join(std_split(rt_def(w_libGiven, "libGiven"), rt_s(&S19)), rt_s(&S88)) }, 0) : ({ wv t2509 = rt_add(v1176_slashed, rt_s(&S710)); wv t2510 = rt_add(v1176_slashed, rt_s(&S711)); rt_arr(2, (wv[]){ t2509, t2510 }, 0); })), 0);
  RT_AT(8, 137);
  wv v1178_k = rt_i(0);
  RT_AT(8, 138);
  while (rt_cond((RT_AT(8, 138), ({ wv t2511 = v1178_k; wv t2512 = std_length(v1177_candidates); rt_lt(t2511, t2512); })))) {
    RT_AT(8, 139);
    wv v1179_c = rt_bind(rt_idx(v1178_k, v1177_candidates), 0);
    RT_AT(8, 140);
    (void)({ wv t2513 = rt_def(w_searched, "searched"); wv t2514 = v1179_c; std_push(t2513, t2514); });
    RT_AT(8, 141);
    wv t2515 = std_listDir(v1179_c);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        t2515 = (RT_AT(8, 142), RT_NULL);
      }
    }
    RT_AT(8, 141);
    wv v1180_names = rt_bind(t2515, 0);
    RT_AT(8, 143);
    if (rt_cond(rt_ne(v1180_names, RT_NULL))) {
      RT_AT(8, 143);
      RT_RET((rt_cond(rt_eq(({ wv t2516 = v1179_c; wv t2517 = std_length(v1179_c); std_slice(t2516, rt_i(-1), t2517); }), rt_s(&S88))) ? v1179_c : rt_add(v1179_c, rt_s(&S88))));
    }
    RT_AT(8, 144);
    v1178_k = rt_add(v1178_k, rt_i(1));
  }
  RT_RET((RT_AT(8, 146), RT_NULL));
}

static const char *const W_files[] = { "wvc/text.wvt", "wvc/diag.wvt", "wvc/lexer.wvt", "wvc/parser.wvt", "wvc/checker.wvt", "wvc/codegen_c.wvt", "wvc/dump.wvt", "wvc/loader.wvt", "wvc/wvc.wvt" };

static void wvt_main(void) {
  rt_files = W_files;
  rt_syms(94, (wstr *const[]){ &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars, &F_word, &F_t, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts, &F_tokens, &F_k, &F_e, &F_handler, &F_items, &F_fields, &F_index, &F_arr, &F_rec, &F_l, &F_r, &F_c, &F_a, &F_b, &F_stmts, &F_param, &F_paramPos, &F_body, &F_neg, &F_cond, &F_isVar, &F_isRef, &F_name, &F_namePos, &F_op, &F_target, &F_names, &F_coll, &F_from, &F_to, &F_step, &F_reversed, &F_code, &F_arg, &F_paren, &F_path, &F_pathPos, &F_params, &F_members, &F_value, &F_close, &F_field, &F_callee, &F_args, &F_values, &F_subject, &F_cases, &F_els, &F_branches, &F_arity, &F_mod, &F_parent, &F_kind, &F_visibleAt, &F_fnId, &F_lib, &F_id, &F_func, &F_top, &F_loops, &F_guards, &F_key, &F_exports, &F_refTargets, &F_useMod, &F_imports, &F_node, &F_notLibrary, &F_program, &F_children, &F_d, &F_depth, &F_cname, &F_captured, &F_fnIndex, &F_pre, &F_cs, &F_proto, &F_lines, &F_js });
  rt_roots(53, (wv *const[]){ &w2_srcFile, &w2_srcChars, &w2_warnings, &w3_keywords, &w3_reserved, &w3_bangWords, &w3_setOps, &w3_ops, &w3_maxInt, &w4_exprStart, &w4_onfailOk, &w4_callees, &w4_exportable, &w5_stdlib, &w5_neverCompletes, &w5_onfailValueUsed, &w5_libraryTop, &w6_binOps, &w6_binFns, &w6_scalarKinds, &w6_plainKinds, &w6_signalers, &w6_constK, &w6_pureK, &w6_effectK, &w_usage, &w_argv, &w_mode, &w_input, &w_output, &w_runtimePath, &w_cc, &w_libGiven, &w_arg, &w_inspect, &w_dotWvt, &w_runtimeGiven, &w_searched, &w_libDir, &w_modules, &w_problems, &w_runtime, &w_code, &w_dotExe, &w_cFile, &w_parts, &w_ccName, &w_flags, &w_status, &v1166_a, &v1181_tokens, &v1182_program, &v1183_p });
  RT_AT(1, 6);
  w2_srcFile = rt_s(&S13);
  RT_AT(1, 7);
  w2_srcChars = rt_arr(0, NULL, 0);
  RT_AT(1, 8);
  w2_warnings = rt_arr(0, NULL, 1);
  RT_AT(2, 16);
  w3_keywords = rt_arr(26, (wv[]){ rt_s(&S33), rt_s(&S34), rt_s(&S35), rt_s(&S36), rt_s(&S37), rt_s(&S38), rt_s(&S39), rt_s(&S40), rt_s(&S41), rt_s(&S42), rt_s(&S43), rt_s(&S44), rt_s(&S45), rt_s(&S46), rt_s(&S47), rt_s(&S48), rt_s(&S49), rt_s(&S50), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S54), rt_s(&S55), rt_s(&S56), rt_s(&S57), rt_s(&S58) }, 0);
  RT_AT(2, 19);
  w3_reserved = ({ wv t68 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S59), rt_s(&S60) }); wv t69 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S61), rt_s(&S62) }); wv t70 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S63), rt_s(&S64) }); rt_arr(3, (wv[]){ t68, t69, t70 }, 0); });
  RT_AT(2, 24);
  w3_bangWords = rt_arr(2, (wv[]){ rt_s(&S39), rt_s(&S41) }, 0);
  RT_AT(2, 26);
  w3_setOps = rt_arr(11, (wv[]){ rt_s(&S65), rt_s(&S66), rt_s(&S67), rt_s(&S68), rt_s(&S69), rt_s(&S70), rt_s(&S71), rt_s(&S72), rt_s(&S73), rt_s(&S74), rt_s(&S75) }, 0);
  RT_AT(2, 27);
  w3_ops = rt_arr(31, (wv[]){ rt_s(&S76), rt_s(&S77), rt_s(&S78), rt_s(&S79), rt_s(&S80), rt_s(&S81), rt_s(&S82), rt_s(&S83), rt_s(&S84), rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S89), rt_s(&S90), rt_s(&S91), rt_s(&S92), rt_s(&S93), rt_s(&S32), rt_s(&S94), rt_s(&S95), rt_s(&S96), rt_s(&S28), rt_s(&S14), rt_s(&S97), rt_s(&S98), rt_s(&S16), rt_s(&S99), rt_s(&S100), rt_s(&S101), rt_s(&S15) }, 0);
  RT_AT(2, 32);
  w3_maxInt = rt_i(9007199254740991);
  RT_AT(3, 26);
  w4_exprStart = rt_arr(19, (wv[]){ rt_s(&S139), rt_s(&S149), rt_s(&S173), rt_s(&S50), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S54), rt_s(&S138), rt_s(&S157), rt_s(&S98), rt_s(&S99), rt_s(&S101), rt_s(&S39), rt_s(&S177), rt_s(&S45), rt_s(&S86), rt_s(&S95), rt_s(&S94) }, 0);
  RT_AT(3, 28);
  w4_onfailOk = rt_arr(6, (wv[]){ rt_s(&S178), rt_s(&S179), rt_s(&S38), rt_s(&S34), rt_s(&S58), rt_s(&S55) }, 0);
  RT_AT(3, 29);
  w4_callees = rt_arr(2, (wv[]){ rt_s(&S138), rt_s(&S157) }, 0);
  RT_AT(3, 30);
  w4_exportable = rt_arr(5, (wv[]){ rt_s(&S33), rt_s(&S35), rt_s(&S36), rt_s(&S37), rt_s(&S47) }, 0);
  RT_AT(4, 19);
  w5_stdlib = ({ wv t552 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S353), rt_i(1) }); wv t553 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S354), rt_i(1) }); wv t554 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S355), rt_i(1) }); wv t555 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S356), rt_i(0) }); wv t556 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S357), rt_i(0) }); wv t557 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S358), rt_i(1) }); wv t558 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S359), rt_i(0) }); wv t559 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S360), rt_i(0) }); wv t560 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S361), rt_i(1) }); wv t561 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S362), rt_i(2) }); wv t562 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S363), rt_i(2) }); wv t563 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S364), rt_i(1) }); wv t564 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S365), rt_i(1) }); wv t565 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S366), rt_i(1) }); wv t566 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S367), rt_s(&S35) }); wv t567 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S368), rt_i(1) }); wv t568 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S369), rt_s(&S35) }); wv t569 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S370), rt_i(2) }); wv t570 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S371), rt_i(1) }); wv t571 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S372), rt_i(2) }); wv t572 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S373), rt_i(3) }); wv t573 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S374), rt_i(1) }); wv t574 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S375), rt_i(1) }); wv t575 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S376), rt_i(2) }); wv t576 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S377), rt_i(2) }); wv t577 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S378), rt_i(1) }); wv t578 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S379), rt_i(1) }); wv t579 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S380), rt_i(1) }); wv t580 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S381), rt_i(2) }); wv t581 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S382), rt_i(1) }); wv t582 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S383), rt_i(2) }); wv t583 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S384), rt_i(1) }); wv t584 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S385), rt_i(2) }); wv t585 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S386), rt_i(1) }); wv t586 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S387), rt_i(2) }); wv t587 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S388), rt_i(2) }); wv t588 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S389), rt_i(2) }); wv t589 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S390), rt_i(1) }); wv t590 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S391), rt_i(1) }); wv t591 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S392), rt_i(1) }); wv t592 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S393), rt_i(1) }); wv t593 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S394), rt_i(1) }); wv t594 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S395), rt_i(1) }); wv t595 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S396), rt_i(1) }); wv t596 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S397), rt_i(1) }); wv t597 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S398), rt_i(1) }); wv t598 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S399), rt_i(1) }); wv t599 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S400), rt_i(0) }); wv t600 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S401), rt_i(0) }); wv t601 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S402), rt_i(1) }); wv t602 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S403), rt_i(1) }); wv t603 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S404), rt_i(2) }); wv t604 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S405), rt_i(1) }); rt_arr(53, (wv[]){ t552, t553, t554, t555, t556, t557, t558, t559, t560, t561, t562, t563, t564, t565, t566, t567, t568, t569, t570, t571, t572, t573, t574, t575, t576, t577, t578, t579, t580, t581, t582, t583, t584, t585, t586, t587, t588, t589, t590, t591, t592, t593, t594, t595, t596, t597, t598, t599, t600, t601, t602, t603, t604 }, 0); });
  RT_AT(4, 62);
  w5_neverCompletes = rt_arr(6, (wv[]){ rt_s(&S34), rt_s(&S58), rt_s(&S55), rt_s(&S56), rt_s(&S43), rt_s(&S44) }, 0);
  RT_AT(4, 64);
  w5_onfailValueUsed = rt_arr(5, (wv[]){ rt_s(&S179), rt_s(&S38), rt_s(&S34), rt_s(&S58), rt_s(&S55) }, 0);
  RT_AT(4, 125);
  w5_libraryTop = rt_arr(6, (wv[]){ rt_s(&S203), rt_s(&S48), rt_s(&S33), rt_s(&S179), rt_s(&S47), rt_s(&S206) }, 0);
  RT_AT(5, 13);
  w6_binOps = rt_arr(17, (wv[]){ rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S89), rt_s(&S76), rt_s(&S77), rt_s(&S78), rt_s(&S90), rt_s(&S91), rt_s(&S79), rt_s(&S80), rt_s(&S92), rt_s(&S93), rt_s(&S32), rt_s(&S83), rt_s(&S84) }, 0);
  RT_AT(5, 14);
  w6_binFns = rt_arr(17, (wv[]){ rt_s(&S511), rt_s(&S512), rt_s(&S513), rt_s(&S514), rt_s(&S515), rt_s(&S516), rt_s(&S517), rt_s(&S518), rt_s(&S519), rt_s(&S520), rt_s(&S521), rt_s(&S522), rt_s(&S523), rt_s(&S524), rt_s(&S525), rt_s(&S526), rt_s(&S527) }, 0);
  RT_AT(5, 16);
  w6_scalarKinds = rt_arr(9, (wv[]){ rt_s(&S139), rt_s(&S149), rt_s(&S173), rt_s(&S300), rt_s(&S52), rt_s(&S53), rt_s(&S54), rt_s(&S194), rt_s(&S193) }, 0);
  RT_AT(5, 18);
  w6_plainKinds = rt_arr(8, (wv[]){ rt_s(&S139), rt_s(&S149), rt_s(&S300), rt_s(&S52), rt_s(&S53), rt_s(&S54), rt_s(&S214), rt_s(&S157) }, 0);
  RT_AT(5, 20);
  w6_signalers = rt_arr(15, (wv[]){ rt_s(&S356), rt_s(&S357), rt_s(&S358), rt_s(&S360), rt_s(&S387), rt_s(&S389), rt_s(&S361), rt_s(&S362), rt_s(&S363), rt_s(&S365), rt_s(&S366), rt_s(&S368), rt_s(&S370), rt_s(&S379), rt_s(&S380) }, 0);
  RT_AT(5, 23);
  w6_constK = rt_i(0);
  RT_AT(5, 24);
  w6_pureK = rt_i(1);
  RT_AT(5, 25);
  w6_effectK = rt_i(2);
  RT_AT(8, 27);
  w_usage = rt_add(rt_add(rt_add(rt_add(rt_s(&S688), rt_s(&S689)), rt_s(&S690)), rt_s(&S691)), rt_s(&S692));
  RT_AT(8, 47);
  w_argv = rt_bind(std_args(), 0);
  RT_AT(8, 48);
  w_mode = rt_s(&S13);
  RT_AT(8, 49);
  w_input = rt_s(&S13);
  RT_AT(8, 50);
  w_output = rt_s(&S13);
  RT_AT(8, 52);
  w_runtimePath = rt_s(&S13);
  RT_AT(8, 53);
  w_cc = rt_s(&S13);
  RT_AT(8, 54);
  w_libGiven = rt_s(&S13);
  RT_AT(8, 55);
  w_arg = rt_i(0);
  RT_AT(8, 56);
  while (rt_cond((RT_AT(8, 56), ({ wv t2490 = w_arg; wv t2491 = std_length(w_argv); rt_lt(t2490, t2491); })))) {
    RT_AT(8, 57);
    v1166_a = rt_bind(rt_idx(w_arg, w_argv), 0);
    RT_AT(8, 58);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_arg, rt_i(0)), "&&") && rt_bool(rt_eq(v1166_a, rt_s(&S694)), "&&")))) {
      RT_AT(8, 59);
      w_mode = rt_bind(v1166_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v1166_a, rt_s(&S695)), "||") || rt_bool(rt_eq(v1166_a, rt_s(&S696)), "||")))) {
      RT_AT(8, 61);
      w_mode = rt_bind(v1166_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1166_a, rt_s(&S697)), "||") || rt_bool(rt_eq(v1166_a, rt_s(&S698)), "||")), "||") || rt_bool(rt_eq(v1166_a, rt_s(&S699)), "||")), "||") || rt_bool(rt_eq(v1166_a, rt_s(&S700)), "||")))) {
      RT_AT(8, 63);
      w_arg = rt_add(w_arg, rt_i(1));
      RT_AT(8, 64);
      if (rt_cond(({ wv t2492 = w_arg; wv t2493 = std_length(w_argv); rt_ge(t2492, t2493); }))) {
        RT_AT(8, 64);
        (void)rt_chk(w_die(), NULL);
      }
      RT_AT(8, 65);
      if (rt_cond(rt_eq(v1166_a, rt_s(&S697)))) {
        RT_AT(8, 66);
        w_output = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1166_a, rt_s(&S698)))) {
        RT_AT(8, 68);
        w_runtimePath = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1166_a, rt_s(&S700)))) {
        RT_AT(8, 70);
        w_libGiven = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else {
        RT_AT(8, 72);
        w_cc = rt_bind(rt_idx(w_arg, w_argv), 1);
      }
    } else if (rt_cond(rt_b(rt_bool(rt_ne(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(rt_gt(std_length(v1166_a), rt_i(0)), "&&") && rt_bool(rt_eq(std_at(v1166_a, rt_i(0)), rt_s(&S86)), "&&")), "||")))) {
      RT_AT(8, 75);
      (void)rt_chk(w_die(), NULL);
    } else {
      RT_AT(8, 77);
      w_input = rt_bind(v1166_a, 1);
    }
    RT_AT(8, 79);
    w_arg = rt_add(w_arg, rt_i(1));
  }
  RT_AT(8, 81);
  w_inspect = rt_b(rt_bool(rt_eq(w_mode, rt_s(&S695)), "||") || rt_bool(rt_eq(w_mode, rt_s(&S696)), "||"));
  RT_AT(8, 82);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(w_inspect, "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "||") || rt_bool(rt_ne(w_libGiven, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S694)), "&&") && rt_bool(rt_ne(w_cc, rt_s(&S13)), "&&")), "||")))) {
    RT_AT(8, 82);
    (void)rt_chk(w_die(), NULL);
  }
  RT_AT(8, 89);
  w_dotWvt = rt_b(rt_bool(rt_ge(std_length(w_input), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t2496 = w_input; wv t2497 = std_length(w_input); std_slice(t2496, rt_i(-4), t2497); }), rt_s(&S248)), "&&"));
  RT_AT(8, 90);
  if (rt_cond(rt_eq(w_mode, rt_s(&S694)))) {
    RT_AT(8, 91);
    if (rt_cond(rt_eq(w_cc, rt_s(&S13)))) {
      RT_AT(8, 91);
      wv t2498 = rt_s(&S703);
      wv t2499 = std_env(t2498);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          t2499 = (RT_AT(8, 91), rt_s(&S704));
        }
      }
      RT_AT(8, 91);
      w_cc = rt_bind(t2499, 1);
    }
    RT_AT(8, 93);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_output, rt_s(&S13)), "&&") && rt_bool(rt_not(w_dotWvt), "&&")))) {
      RT_AT(8, 94);
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S705), rt_text(w_input), rt_s(&S706) }));
      RT_AT(8, 95);
      rt_exit(rt_i(2));
    }
    RT_AT(8, 97);
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      RT_AT(8, 97);
      w_output = ({ wv t2500 = std_slice(w_input, rt_i(0), rt_i(-4)); wv t2501 = (rt_cond(rt_chk(w_onWindows(), NULL)) ? rt_s(&S707) : rt_s(&S13)); rt_add(t2500, t2501); });
    }
  }
  RT_AT(8, 99);
  w_runtimeGiven = rt_ne(w_runtimePath, rt_s(&S13));
  RT_AT(8, 100);
  if (rt_cond(rt_not(w_runtimeGiven))) {
    RT_AT(8, 100);
    w_runtimePath = rt_s(&S708);
  }
  RT_AT(8, 132);
  w_searched = rt_arr(0, NULL, 1);
  RT_AT(8, 149);
  (void)rt_chk(w_load(w_input), NULL);
  RT_AT(8, 151);
  if (rt_cond(rt_eq(w_mode, rt_s(&S695)))) {
    RT_AT(8, 152);
    v1181_tokens = rt_bind(rt_chk(w3_lex(w2_srcChars), NULL), 0);
    RT_AT(8, 153);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 154);
    (void)rt_chk(w7_dumpTokens(v1181_tokens, rt_s(&S13)), NULL);
    RT_AT(8, 155);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 157);
  if (rt_cond(rt_eq(w_mode, rt_s(&S696)))) {
    RT_AT(8, 158);
    v1182_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(w2_srcChars), NULL)), NULL), 0);
    RT_AT(8, 159);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 160);
    (void)std_print(rt_chk(w7_dumpValue(v1182_program, rt_s(&S13)), NULL));
    RT_AT(8, 161);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 163);
  w_libDir = rt_bind(rt_chk(w_findLib(), NULL), 0);
  RT_AT(8, 164);
  w_modules = rt_bind(rt_chk(w8_loadProgram(w2_srcFile, w2_srcChars, w_libDir, w_searched), NULL), 0);
  RT_AT(8, 165);
  w_problems = rt_bind(rt_chk(w5_check(w_modules, rt_b(1)), NULL), 0);
  RT_AT(8, 166);
  if (rt_cond(rt_gt(std_length(w_problems), rt_i(0)))) {
    RT_AT(8, 167);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 168);
    v1183_p = rt_i(0);
    RT_AT(8, 169);
    while (rt_cond((RT_AT(8, 169), ({ wv t2518 = v1183_p; wv t2519 = std_length(w_problems); rt_lt(t2518, t2519); })))) {
      RT_AT(8, 170);
      (void)std_eprint(rt_chk(w2_formatDiag(rt_idx(v1183_p, w_problems)), NULL));
      RT_AT(8, 171);
      v1183_p = rt_add(v1183_p, rt_i(1));
    }
    RT_AT(8, 173);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 175);
  w_runtime = rt_bind(rt_chk(w_readRuntime(), NULL), 0);
  RT_AT(8, 176);
  if (rt_cond(rt_eq(w_runtime, RT_NULL))) {
    RT_AT(8, 177);
    (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S693), rt_text(w_runtimePath) }));
    RT_AT(8, 178);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 180);
  w_code = rt_bind(rt_chk(w6_generateC(w_modules, w_runtime), NULL), 0);
  RT_AT(8, 181);
  (void)rt_chk(w2_flushWarnings(), NULL);
  RT_AT(8, 182);
  if (rt_cond(rt_ne(w_mode, rt_s(&S694)))) {
    RT_AT(8, 183);
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      RT_AT(8, 183);
      w_output = rt_add((rt_cond(w_dotWvt) ? std_slice(w_input, rt_i(0), rt_i(-4)) : w_input), rt_s(&S712));
    }
    RT_AT(8, 184);
    wv t2520 = std_writeFile(w_output, w_code);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        RT_AT(8, 185);
        (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S713), rt_text(w_output) }));
        RT_AT(8, 185);
        rt_exit(rt_i(2));
        t2520 = RT_NULL;
      }
    }
    RT_AT(8, 184);
    (void)t2520;
    RT_AT(8, 186);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 191);
  w_dotExe = rt_b(rt_bool(rt_ge(std_length(w_output), rt_i(4)), "&&") && rt_bool(rt_eq(rt_chk(w1_toLower(({ wv t2521 = w_output; wv t2522 = std_length(w_output); std_slice(t2521, rt_i(-4), t2522); })), NULL), rt_s(&S707)), "&&"));
  RT_AT(8, 192);
  w_cFile = rt_add((rt_cond(w_dotExe) ? std_slice(w_output, rt_i(0), rt_i(-4)) : w_output), rt_s(&S712));
  RT_AT(8, 193);
  wv t2523 = std_writeFile(w_cFile, w_code);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 194);
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S713), rt_text(w_cFile) }));
      RT_AT(8, 194);
      rt_exit(rt_i(2));
      t2523 = RT_NULL;
    }
  }
  RT_AT(8, 193);
  (void)t2523;
  RT_AT(8, 195);
  w_parts = rt_bind(std_split(std_join(std_split(w_cc, rt_s(&S19)), rt_s(&S88)), rt_s(&S88)), 0);
  RT_AT(8, 196);
  w_ccName = rt_bind(rt_chk(w1_toLower(rt_idx(rt_i(-1), w_parts)), NULL), 0);
  RT_AT(8, 197);
  w_flags = rt_arr(4, (wv[]){ rt_s(&S714), rt_s(&S697), w_output, w_cFile }, 0);
  RT_AT(8, 198);
  wv t2524 = (rt_cond(rt_b(rt_bool(rt_eq(w_ccName, rt_s(&S704)), "||") || rt_bool(rt_eq(w_ccName, rt_s(&S715)), "||"))) ? rt_arr(5, (wv[]){ rt_s(&S716), rt_s(&S714), rt_s(&S697), w_output, w_cFile }, 0) : w_flags);
  wv t2525 = std_run(w_cc, t2524);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 199);
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S717), rt_text(w_cc), rt_s(&S718) }));
      RT_AT(8, 199);
      rt_exit(rt_i(2));
      t2525 = RT_NULL;
    }
  }
  RT_AT(8, 198);
  w_status = rt_bind(t2525, 0);
  RT_AT(8, 200);
  if (rt_cond(rt_ne(w_status, rt_i(0)))) {
    RT_AT(8, 201);
    (void)std_eprint(({ wv t2526 = rt_text(w_cc); wv t2527 = rt_text(w_status); rt_cat(4, (wv[]){ rt_s(&S705), t2526, rt_s(&S719), t2527 }); }));
    RT_AT(8, 202);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 204);
  rt_exit(rt_i(0));
}
