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

// ---- screen state set with escape sequences (§12.2, Terminal state) ----
// A program may hide the cursor or switch to the alternate screen by writing
// escape sequences to a terminal. The runtime notes them as they are written,
// and term_restore undoes what is still in effect however the program ends.
// Output to a file or a pipe is not looked at, and nothing is added to it.
static int scr_tty[3] = { 0, -1, -1 }; // per fd: 1 if a terminal, 0 if not, -1 until known
static int scr_hidden;                 // the cursor is hidden
static const char *scr_alt;            // the alternate screen's mode ("1049", "1047" or "47"), or NULL
static int scr_fd = 1;                 // the stream the sequences were written to
static void catch_ends(void);

// Whether output fd is a terminal. WVT_TEST_TERMINAL=1 makes stdout and
// stderr count as terminals here, so that tests can see what is restored.
static int out_is_terminal(int fd) {
  if (scr_tty[fd] < 0) {
    const char *test = getenv("WVT_TEST_TERMINAL");
    if (test && strcmp(test, "1") == 0) return scr_tty[fd] = 1;
#ifdef _WIN32
    DWORD mode;
    scr_tty[fd] = GetConsoleMode(GetStdHandle(fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE), &mode) != 0;
#else
    scr_tty[fd] = isatty(fd);
#endif
  }
  return scr_tty[fd];
}

// Notes the screen state that the escape sequences in p change.
static void scr_note(int fd, const char *p, size_t n) {
  if (!memchr(p, 0x1B, n) || !out_is_terminal(fd)) return;
  static const char *const alts[] = { "1049", "1047", "47" };
  for (const char *e = p + n; (p = memchr(p, 0x1B, e - p)) != NULL; p++) {
    size_t left = (size_t)(e - p);
    if (left >= 2 && p[1] == 'c') { scr_hidden = 0; scr_alt = NULL; continue; } // a full reset
    scr_fd = fd;
    if (left < 5 || p[1] != '[' || p[2] != '?') continue;
    const char *q = p + 3;
    if (left >= 6 && memcmp(q, "25", 2) == 0 && (q[2] == 'l' || q[2] == 'h')) {
      scr_hidden = q[2] == 'l';
      catch_ends();
      continue;
    }
    for (int i = 0; i < 3; i++) {
      size_t k = strlen(alts[i]);
      if (left >= 4 + k && memcmp(q, alts[i], k) == 0 && (q[k] == 'h' || q[k] == 'l')) {
        scr_alt = q[k] == 'h' ? alts[i] : NULL;
        catch_ends();
        break;
      }
    }
  }
}

// Undoes the screen state still in effect, leaving the alternate screen first,
// so that what comes after is on the normal screen. resume: put it back, after
// a Ctrl+Z. Uses only write_raw, so it may run in a signal handler.
static void scr_restore(int resume) {
  static const char *saved_alt;
  static int saved_hidden;
  char seq[32];
  if (resume) {
    if (saved_alt) { int k = snprintf(seq, sizeof seq, "\x1b[?%sh", saved_alt); write_raw(scr_fd, seq, k); }
    if (saved_hidden) write_raw(scr_fd, "\x1b[?25l", 6);
    scr_alt = saved_alt;
    scr_hidden = saved_hidden;
    return;
  }
  saved_alt = scr_alt;
  saved_hidden = scr_hidden;
  // Cleared first: a write that fails ends the program through term_restore.
  scr_alt = NULL;
  scr_hidden = 0;
  if (saved_alt) { int k = snprintf(seq, sizeof seq, "\x1b[?%sl", saved_alt); write_raw(scr_fd, seq, k); }
  if (saved_hidden) write_raw(scr_fd, "\x1b[?25h", 6);
}

// Stdout is buffered; stderr is not, and flushes stdout first to keep order.
static void rt_write(int fd, const char *p, size_t n) {
  scr_note(fd, p, n);
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
  rt_flush();
  scr_restore(0); // leave the alternate screen first, or it would take the message with it
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

// Orders a and b: -1, 0 or 1; 2 when unordered (NaN). Only numbers, an int
// with a float too, and strings have an order (§11.1); ordering anything
// else, or values of two different types, panics.
static int order(wv a, wv b, const char *op) {
  if (a.tag == T_INT && b.tag == T_INT) return a.i < b.i ? -1 : a.i > b.i;
  if (isnum(a) && isnum(b)) {
    double x = nv(a), y = nv(b);
    return x < y ? -1 : x > y ? 1 : x == y ? 0 : 2;
  }
  if (a.tag == T_STR && b.tag == T_STR) return str_cmp(a.s, b.s);
  if (a.tag == T_MEM && b.tag == T_MEM) rt_fail("type error: '%s' cannot order enum members", op);
  if (a.tag == b.tag) rt_fail("type error: '%s' cannot order %s values", op, type_name(a));
  rt_fail("type error: '%s' cannot order %s and %s", op, type_name(a), type_name(b));
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
  scr_restore(0); // while the console still reads escape sequences
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
  scr_restore(1);
}
static void key_catch(int sig, void (*handler)(int)) {
  if (signal(sig, handler) == SIG_IGN) signal(sig, SIG_IGN); // a signal the parent ignored stays ignored
}
#endif

// Restores the terminal on Ctrl+C and the signals that end a program, once
// key mode or screen state needs it.
static void catch_ends(void) {
  static int caught;
  if (caught) return;
  caught = 1;
#ifdef _WIN32
  SetConsoleCtrlHandler(key_ctrl, TRUE);
#else
  int ends[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE };
  for (int i = 0; i < 5; i++) key_catch(ends[i], key_signal);
  key_catch(SIGTSTP, key_stop);
#endif
}

static void key_start(void) {
  if (key_started) return;
  key_started = 1;
#ifdef _WIN32
  key_in = GetStdHandle(STD_INPUT_HANDLE);
  key_tty = GetConsoleMode(key_in, &key_saved) != 0;
#else
  key_tty = isatty(0) && tcgetattr(0, &key_saved) == 0;
#endif
  if (!key_tty) return;
  catch_ends();
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
  // Pipes cannot be waited on, so they are polled every millisecond; files
  // and devices are always ready. Sleep(1) would sleep until the next 15.6 ms
  // system tick, so a key arriving just after the deadline would be taken
  // instead of the timeout; sleep_for uses the high-resolution timer.
  HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  if (GetFileType(h) != FILE_TYPE_PIPE) return 1;
  int64_t end = mono_ms() + ms;
  for (;;) {
    DWORD avail;
    if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail > 0) return 1; // data, or the end
    if (mono_ms() >= end) return 0;
    sleep_for(1);
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
static const wstr S411 = { 7, 7, 1, 0, "boolean" };
static const wstr S412 = { 5, 5, 1, 0, "array" };
static const wstr S413 = { 5, 5, 1, 0, "never" };
static const wstr S414 = { 6, 6, 1, 0, "record" };
static const wstr S415 = { 9, 9, 1, 0, "record {}" };
static const wstr S416 = { 6, 6, 1, 0, ", ... " };
static const wstr S417 = { 5, 5, 1, 0, " more" };
static const wstr S418 = { 9, 9, 1, 0, "record { " };
static const wstr S419 = { 2, 2, 1, 0, ", " };
static const wstr S420 = { 2, 2, 1, 0, " }" };
static const wstr S421 = { 85, 85, 1, 0, "join its elements with (xs, \", \")($join), or use arrays.toString from $lib/arrays.wvt" };
static const wstr S422 = { 69, 69, 1, 0, "use records.toString from $lib/records.wvt, or interpolate its fields" };
static const wstr S423 = { 36, 36, 1, 0, "use maps.toString from $lib/maps.wvt" };
static const wstr S424 = { 5, 5, 1, 0, "width" };
static const wstr S425 = { 6, 6, 1, 0, "height" };
static const wstr S426 = { 5, 5, 1, 0, "input" };
static const wstr S427 = { 6, 6, 1, 0, "output" };
static const wstr S428 = { 3, 3, 1, 0, "an " };
static const wstr S429 = { 2, 2, 1, 0, "a " };
static const wstr S430 = { 4, 4, 1, 0, " or " };
static const wstr S431 = { 1, 1, 1, 0, "k" };
static const wstr S432 = { 9, 9, 1, 0, "array of " };
static const wstr S433 = { 9, 9, 1, 0, "{\"text\": " };
static const wstr S434 = { 10, 10, 1, 0, ", \"elem\": " };
static const wstr S435 = { 9, 9, 1, 0, "{\"name\": " };
static const wstr S436 = { 10, 10, 1, 0, ", \"type\": " };
static const wstr S437 = { 13, 13, 1, 0, ", \"fields\": [" };
static const wstr S438 = { 9, 9, 1, 0, "{\"file\": " };
static const wstr S439 = { 10, 10, 1, 0, ", \"line\": " };
static const wstr S440 = { 9, 9, 1, 0, ", \"col\": " };
static const wstr S441 = { 10, 10, 1, 0, ", \"name\": " };
static const wstr S442 = { 11, 11, 1, 0, ", \"kind\": \"" };
static const wstr S443 = { 11, 11, 1, 0, "\", \"type\": " };
static const wstr S444 = { 11, 11, 1, 0, "{\"files\": [" };
static const wstr S445 = { 14, 14, 1, 0, "],\n\"names\": [\n" };
static const wstr S446 = { 2, 2, 1, 0, ",\n" };
static const wstr S447 = { 3, 3, 1, 0, "\n]}" };
static const wstr S448 = { 15, 15, 1, 0, "cannot convert " };
static const wstr S449 = { 31, 31, 1, 0, " to text; convert it explicitly" };
static const wstr S450 = { 39, 39, 1, 0, "a condition must be true or false, got " };
static const wstr S451 = { 30, 30, 1, 0, "' requires true or false, got " };
static const wstr S452 = { 25, 25, 1, 0, "' requires integers, got " };
static const wstr S453 = { 8, 8, 1, 0, " field '" };
static const wstr S454 = { 22, 22, 1, 0, "' needs a record, got " };
static const wstr S455 = { 10, 10, 1, 0, "no field '" };
static const wstr S456 = { 5, 5, 1, 0, "' in " };
static const wstr S457 = { 29, 29, 1, 0, "an index must be an int, got " };
static const wstr S458 = { 79, 79, 1, 0, "a map key must be an int, a string, a boolean, an enum member or a record, got " };
static const wstr S459 = { 44, 44, 1, 0, "a record's field name must be a string, got " };
static const wstr S460 = { 11, 11, 1, 0, ", indexing " };
static const wstr S461 = { 88, 88, 1, 0, "a record is indexed by the name of a field; for other keys, such as positions, use a map" };
static const wstr S462 = { 33, 33, 1, 0, "use $at for a string: (s, i)($at)" };
static const wstr S463 = { 48, 48, 1, 0, "indexing needs an array, a map or a record, got " };
static const wstr S464 = { 12, 12, 1, 0, "enum members" };
static const wstr S465 = { 7, 7, 1, 0, " values" };
static const wstr S466 = { 15, 15, 1, 0, "' cannot order " };
static const wstr S467 = { 5, 5, 1, 0, " and " };
static const wstr S468 = { 25, 25, 1, 0, "'+' cannot be applied to " };
static const wstr S469 = { 32, 32, 1, 0, "use interpolation, as in \"n={n}\"" };
static const wstr S470 = { 23, 23, 1, 0, "' cannot be applied to " };
static const wstr S471 = { 55, 55, 1, 0, "this block's value is used, but it does not produce one" };
static const wstr S472 = { 62, 62, 1, 0, "end the block with its value, or with null if that is intended" };
static const wstr S473 = { 42, 42, 1, 0, "' is already declared in this scope (line " };
static const wstr S474 = { 11, 11, 1, 0, "' shadows '" };
static const wstr S475 = { 32, 32, 1, 0, "' from an enclosing scope (line " };
static const wstr S476 = { 45, 45, 1, 0, "Wavert forbids shadowing; choose another name" };
static const wstr S477 = { 14, 14, 1, 0, "unknown name '" };
static const wstr S478 = { 39, 39, 1, 0, "' is used before its declaration (line " };
static const wstr S479 = { 5, 5, 1, 0, "param" };
static const wstr S480 = { 27, 27, 1, 0, "'return' outside a function" };
static const wstr S481 = { 16, 16, 1, 0, "' outside a loop" };
static const wstr S482 = { 84, 84, 1, 0, "this loop never runs: !while repeats while its condition is false, and true never is" };
static const wstr S483 = { 51, 51, 1, 0, "this loop never runs: its condition is always false" };
static const wstr S484 = { 57, 57, 1, 0, "for a loop that runs until it breaks, write ;while (true)" };
static const wstr S485 = { 56, 56, 1, 0, "'fail' outside a function: there is no caller to fail to" };
static const wstr S486 = { 38, 38, 1, 0, "a for loop's step must be an int, got " };
static const wstr S487 = { 4, 4, 1, 0, "loop" };
static const wstr S488 = { 85, 85, 1, 0, "to go through an array, write ;for (x in xs), or ;for (i, x in xs) for each index too" };
static const wstr S489 = { 41, 41, 1, 0, "round it to an int, such as with ($floor)" };
static const wstr S490 = { 35, 35, 1, 0, "a for loop's range needs ints, got " };
static const wstr S491 = { 46, 46, 1, 0, "for its characters, go through (s, \"\")($split)" };
static const wstr S492 = { 42, 42, 1, 0, "for its field names, go through r($fields)" };
static const wstr S493 = { 52, 52, 1, 0, "for the ints from 0 to n - 1, write ;for (i in 0, n)" };
static const wstr S494 = { 73, 73, 1, 0, "a for loop goes through an array, a map, an enum or a range of ints, got " };
static const wstr S495 = { 2, 2, 1, 0, "it" };
static const wstr S496 = { 12, 12, 1, 0, "cannot set '" };
static const wstr S497 = { 25, 25, 1, 0, "' inside a for loop over " };
static const wstr S498 = { 50, 50, 1, 0, "build the new value in another variable, and set '" };
static const wstr S499 = { 22, 22, 1, 0, "' to it after the loop" };
static const wstr S500 = { 65, 65, 1, 0, "'onfail' handles a call, but this statement's value is not a call" };
static const wstr S501 = { 87, 87, 1, 0, "the call must be the outermost expression, e.g. ;const n is s(parse) onfail (c) { ... }" };
static const wstr S502 = { 44, 44, 1, 0, "a match cannot mix literals and enum members" };
static const wstr S503 = { 31, 31, 1, 0, "a match cannot mix members of '" };
static const wstr S504 = { 7, 7, 1, 0, "' and '" };
static const wstr S505 = { 21, 21, 1, 0, "duplicate case value " };
static const wstr S506 = { 7, 7, 1, 0, " (line " };
static const wstr S507 = { 22, 22, 1, 0, "match does not handle " };
static const wstr S508 = { 57, 57, 1, 0, "add a case for each, or end the match with 'else { ... }'" };
static const wstr S509 = { 52, 52, 1, 0, "a match used as a value must end with 'else { ... }'" };
static const wstr S510 = { 6, 6, 1, 0, "module" };
static const wstr S511 = { 19, 19, 1, 0, "' does not export '" };
static const wstr S512 = { 17, 17, 1, 0, "' is not a module" };
static const wstr S513 = { 16, 16, 1, 0, "unknown module '" };
static const wstr S514 = { 41, 41, 1, 0, "' is a function; functions are not values" };
static const wstr S515 = { 20, 20, 1, 0, "to call it, write x(" };
static const wstr S516 = { 7, 7, 1, 0, "), or (" };
static const wstr S517 = { 19, 19, 1, 0, ") with no arguments" };
static const wstr S518 = { 25, 25, 1, 0, "' is an enum, not a value" };
static const wstr S519 = { 24, 24, 1, 0, "its members are written " };
static const wstr S520 = { 7, 7, 1, 0, ".member" };
static const wstr S521 = { 54, 54, 1, 0, "case values are literals or enum members (Enum.member)" };
static const wstr S522 = { 16, 16, 1, 0, "' is not an enum" };
static const wstr S523 = { 14, 14, 1, 0, "unknown enum '" };
static const wstr S524 = { 11, 11, 1, 0, "no member '" };
static const wstr S525 = { 11, 11, 1, 0, "' in enum '" };
static const wstr S526 = { 16, 16, 1, 0, "its members are " };
static const wstr S527 = { 66, 66, 1, 0, "a conditional used as a value must end with a plain 'else { ... }'" };
static const wstr S528 = { 25, 25, 1, 0, "': it is not a var or ref" };
static const wstr S529 = { 7, 7, 1, 0, "a field" };
static const wstr S530 = { 10, 10, 1, 0, "an element" };
static const wstr S531 = { 29, 29, 1, 0, "': enum members are constants" };
static const wstr S532 = { 9, 9, 1, 0, "': only '" };
static const wstr S533 = { 12, 12, 1, 0, "' may set it" };
static const wstr S534 = { 14, 14, 1, 0, "cannot modify " };
static const wstr S535 = { 5, 5, 1, 0, " of '" };
static const wstr S536 = { 18, 18, 1, 0, "': it is not a var" };
static const wstr S537 = { 7, 7, 1, 0, "setting" };
static const wstr S538 = { 6, 6, 1, 0, "' is a" };
static const wstr S539 = { 15, 15, 1, 0, ", not a binding" };
static const wstr S540 = { 38, 38, 1, 0, "': a for loop's variable cannot be set" };
static const wstr S541 = { 121, 121, 1, 0, "to replace an element, set it through its array, such as ;set xs[i] to v; to control a counter yourself, use a while loop" };
static const wstr S542 = { 24, 24, 1, 0, "mark the parameter (ref " };
static const wstr S543 = { 24, 24, 1, 0, ") to rebind it, or (var " };
static const wstr S544 = { 20, 20, 1, 0, ") for a mutable copy" };
static const wstr S545 = { 29, 29, 1, 0, "mark the parameter var: (var " };
static const wstr S546 = { 22, 22, 1, 0, "declare it with ';ref " };
static const wstr S547 = { 32, 32, 1, 0, " is ...' to rebind it, or ';var " };
static const wstr S548 = { 27, 27, 1, 0, " is ...' for a mutable copy" };
static const wstr S549 = { 22, 22, 1, 0, "declare it with ';var " };
static const wstr S550 = { 8, 8, 1, 0, " is ...'" };
static const wstr S551 = { 32, 32, 1, 0, "unknown standard library name '$" };
static const wstr S552 = { 26, 26, 1, 0, "' is a module, not a value" };
static const wstr S553 = { 27, 27, 1, 0, "what it exports is written " };
static const wstr S554 = { 5, 5, 1, 0, ".name" };
static const wstr S555 = { 32, 32, 1, 0, "' is a function; call it with ($" };
static const wstr S556 = { 7, 7, 1, 0, "reading" };
static const wstr S557 = { 25, 25, 1, 0, "'-' cannot be applied to " };
static const wstr S558 = { 30, 30, 1, 0, "'fail' is a statement: ;fail c" };
static const wstr S559 = { 24, 24, 1, 0, "checker: unhandled node " };
static const wstr S560 = { 31, 31, 1, 0, "' is a constant, not a function" };
static const wstr S561 = { 8, 8, 1, 0, "' takes " };
static const wstr S562 = { 6, 6, 1, 0, ", got " };
static const wstr S563 = { 9, 9, 1, 0, " expects " };
static const wstr S564 = { 18, 18, 1, 0, "unknown function '" };
static const wstr S565 = { 71, 71, 1, 0, ") is a call with no arguments; to use the value, remove the parentheses" };
static const wstr S566 = { 53, 53, 1, 0, "calls are postfix: the function comes last, e.g. x(f)" };
static const wstr S567 = { 19, 19, 1, 0, "' is not a function" };
static const wstr S568 = { 21, 21, 1, 0, "' is already imported" };
static const wstr S569 = { 38, 38, 1, 0, "' is not a library: it ends with ;exit" };
static const wstr S570 = { 31, 31, 1, 0, "only a library ends with ';end'" };
static const wstr S571 = { 25, 25, 1, 0, "a program ends with ;exit" };
static const wstr S572 = { 88, 88, 1, 0, "a library's top level may contain only import, fn, const, var, ref and enum declarations" };
static const wstr S573 = { 37, 37, 1, 0, "move other statements into a function" };
static const wstr S574 = { 28, 28, 1, 0, "a library must end with ;end" };
static const wstr S575 = { 56, 56, 1, 0, "mark what it exports with 'export', and end it with ;end" };
static const wstr S576 = { 15, 15, 1, 0, "cannot export '" };
static const wstr S577 = { 38, 38, 1, 0, "': it is not declared at the top level" };
static const wstr S578 = { 17, 17, 1, 0, "': it is a module" };
static const wstr S579 = { 5, 5, 1, 0, "line " };
static const wstr S580 = { 6, 6, 1, 0, " line " };
static const wstr S581 = { 49, 49, 1, 0, "' never produces a value, but its value is used (" };
static const wstr S582 = { 65, 65, 1, 0, "end the function with its value, or with null if that is intended" };
static const wstr S583 = { 29, 29, 1, 0, "a program must end with ;exit" };
static const wstr S584 = { 50, 50, 1, 0, "end the program with ;exit, or a library with ;end" };
static const wstr S585 = { 3, 3, 1, 0, "add" };
static const wstr S586 = { 3, 3, 1, 0, "sub" };
static const wstr S587 = { 3, 3, 1, 0, "mul" };
static const wstr S588 = { 3, 3, 1, 0, "div" };
static const wstr S589 = { 3, 3, 1, 0, "mod" };
static const wstr S590 = { 3, 3, 1, 0, "pow" };
static const wstr S591 = { 2, 2, 1, 0, "eq" };
static const wstr S592 = { 2, 2, 1, 0, "ne" };
static const wstr S593 = { 2, 2, 1, 0, "lt" };
static const wstr S594 = { 2, 2, 1, 0, "gt" };
static const wstr S595 = { 2, 2, 1, 0, "le" };
static const wstr S596 = { 2, 2, 1, 0, "ge" };
static const wstr S597 = { 4, 4, 1, 0, "band" };
static const wstr S598 = { 3, 3, 1, 0, "bor" };
static const wstr S599 = { 4, 4, 1, 0, "bxor" };
static const wstr S600 = { 3, 3, 1, 0, "shl" };
static const wstr S601 = { 3, 3, 1, 0, "shr" };
static const wstr S602 = { 3, 3, 1, 0, "neg" };
static const wstr S603 = { 3, 3, 1, 0, "not" };
static const wstr S604 = { 4, 4, 1, 0, "bnot" };
static const wstr S605 = { 2, 2, 1, 0, "\\\"" };
static const wstr S606 = { 2, 2, 1, 0, "\\\\" };
static const wstr S607 = { 2, 2, 1, 0, "{ " };
static const wstr S608 = { 5, 5, 1, 0, ", 0, " };
static const wstr S609 = { 4, 4, 1, 0, "NULL" };
static const wstr S610 = { 8, 8, 1, 0, "(wv[]){ " };
static const wstr S611 = { 6, 6, 1, 0, "RT_AT(" };
static const wstr S612 = { 1, 1, 1, 0, "w" };
static const wstr S613 = { 1, 1, 1, 0, "v" };
static const wstr S614 = { 2, 2, 1, 0, "  " };
static const wstr S615 = { 2, 2, 1, 0, "&S" };
static const wstr S616 = { 3, 3, 1, 0, "&F_" };
static const wstr S617 = { 19, 19, 1, 0, "static const wmem l" };
static const wstr S618 = { 7, 7, 1, 0, "[] = { " };
static const wstr S619 = { 3, 3, 1, 0, " };" };
static const wstr S620 = { 1, 1, 1, 0, "l" };
static const wstr S621 = { 3, 3, 1, 0, "fr." };
static const wstr S622 = { 2, 2, 1, 0, "up" };
static const wstr S623 = { 4, 4, 1, 0, "->up" };
static const wstr S624 = { 2, 2, 1, 0, "->" };
static const wstr S625 = { 7, 7, 1, 0, "rt_def(" };
static const wstr S626 = { 3, 3, 1, 0, "wv " };
static const wstr S627 = { 3, 3, 1, 0, " = " };
static const wstr S628 = { 3, 3, 1, 0, "({ " };
static const wstr S629 = { 4, 4, 1, 0, "; })" };
static const wstr S630 = { 12, 12, 1, 0, " = RT_UNSET;" };
static const wstr S631 = { 6, 6, 1, 0, "(void)" };
static const wstr S632 = { 7, 7, 1, 0, "RT_NULL" };
static const wstr S633 = { 66, 66, 1, 0, "'return' inside a conditional used as a value is not supported yet" };
static const wstr S634 = { 7, 7, 1, 0, "RT_RET(" };
static const wstr S635 = { 2, 2, 1, 0, ");" };
static const wstr S636 = { 9, 9, 1, 0, "rt_panic(" };
static const wstr S637 = { 48, 48, 1, 0, "' cannot leave a conditional used as a value yet" };
static const wstr S638 = { 6, 6, 1, 0, "break;" };
static const wstr S639 = { 9, 9, 1, 0, "continue;" };
static const wstr S640 = { 7, 7, 1, 0, "while (" };
static const wstr S641 = { 8, 8, 1, 0, "rt_cond(" };
static const wstr S642 = { 4, 4, 1, 0, ")) {" };
static const wstr S643 = { 5, 5, 1, 0, "rt_i(" };
static const wstr S644 = { 8, 8, 1, 0, "rt_exit(" };
static const wstr S645 = { 7, 7, 1, 0, "rt_i(0)" };
static const wstr S646 = { 29, 29, 1, 0, "codegen: unhandled statement " };
static const wstr S647 = { 7, 7, 1, 0, "rt_i(1)" };
static const wstr S648 = { 8, 8, 1, 0, "int64_t " };
static const wstr S649 = { 16, 16, 1, 0, " = rt_for_range(" };
static const wstr S650 = { 13, 13, 1, 0, "for (int64_t " };
static const wstr S651 = { 6, 6, 1, 0, " = 0; " };
static const wstr S652 = { 3, 3, 1, 0, " < " };
static const wstr S653 = { 2, 2, 1, 0, "; " };
static const wstr S654 = { 5, 5, 1, 0, "++) {" };
static const wstr S655 = { 7, 7, 1, 0, " - 1 - " };
static const wstr S656 = { 6, 6, 1, 0, ".i + (" };
static const wstr S657 = { 4, 4, 1, 0, ") * " };
static const wstr S658 = { 3, 3, 1, 0, ".i)" };
static const wstr S659 = { 3, 3, 1, 0, " - " };
static const wstr S660 = { 6, 6, 1, 0, "rt_m(&" };
static const wstr S661 = { 2, 2, 1, 0, "])" };
static const wstr S662 = { 5, 5, 1, 0, "wfor " };
static const wstr S663 = { 14, 14, 1, 0, "rt_for_start(&" };
static const wstr S664 = { 14, 14, 1, 0, "rt_for_guard(&" };
static const wstr S665 = { 3, 3, 1, 0, "), " };
static const wstr S666 = { 13, 13, 1, 0, "rt_for_more(&" };
static const wstr S667 = { 3, 3, 1, 0, "); " };
static const wstr S668 = { 13, 13, 1, 0, "rt_for_item(&" };
static const wstr S669 = { 12, 12, 1, 0, "rt_for_key(&" };
static const wstr S670 = { 12, 12, 1, 0, "rt_for_val(&" };
static const wstr S671 = { 13, 13, 1, 0, "rt_fail_with(" };
static const wstr S672 = { 16, 16, 1, 0, "RT_RET(RT_NULL);" };
static const wstr S673 = { 9, 9, 1, 0, "struct fr" };
static const wstr S674 = { 4, 4, 1, 0, " *up" };
static const wstr S675 = { 2, 2, 1, 0, "p_" };
static const wstr S676 = { 10, 10, 1, 0, "static wv " };
static const wstr S677 = { 4, 4, 1, 0, "void" };
static const wstr S678 = { 9, 9, 1, 0, "rt_enter(" };
static const wstr S679 = { 8, 8, 1, 0, " fr = { " };
static const wstr S680 = { 8, 8, 1, 0, ".up = up" };
static const wstr S681 = { 8, 8, 1, 0, "rt_bind(" };
static const wstr S682 = { 13, 13, 1, 0, "(void)rt_def(" };
static const wstr S683 = { 3, 3, 1, 0, "rt_" };
static const wstr S684 = { 7, 7, 1, 0, "rt_set_" };
static const wstr S685 = { 7, 7, 1, 0, "rt_upd_" };
static const wstr S686 = { 5, 5, 1, 0, ", rt_" };
static const wstr S687 = { 7, 7, 1, 0, "rt_arr(" };
static const wstr S688 = { 7, 7, 1, 0, "rt_map(" };
static const wstr S689 = { 1, 1, 1, 0, "K" };
static const wstr S690 = { 7, 7, 1, 0, "rt_rec(" };
static const wstr S691 = { 18, 18, 1, 0, "if (rt_failcode) {" };
static const wstr S692 = { 17, 17, 1, 0, "rt_i(rt_failcode)" };
static const wstr S693 = { 16, 16, 1, 0, "rt_failcode = 0;" };
static const wstr S694 = { 3, 3, 1, 0, "raw" };
static const wstr S695 = { 7, 7, 1, 0, "} else " };
static const wstr S696 = { 4, 4, 1, 0, "if (" };
static const wstr S697 = { 3, 3, 1, 0, ") {" };
static const wstr S698 = { 8, 8, 1, 0, "} else {" };
static const wstr S699 = { 3, 3, 1, 0, " ? " };
static const wstr S700 = { 3, 3, 1, 0, " : " };
static const wstr S701 = { 3, 3, 1, 0, "({\n" };
static const wstr S702 = { 2, 2, 1, 0, "})" };
static const wstr S703 = { 3, 3, 1, 0, "eq(" };
static const wstr S704 = { 4, 4, 1, 0, " || " };
static const wstr S705 = { 12, 12, 1, 0, "(rt_no_case(" };
static const wstr S706 = { 11, 11, 1, 0, "), RT_NULL)" };
static const wstr S707 = { 6, 6, 1, 0, "({ wv " };
static const wstr S708 = { 4, 4, 1, 0, "std_" };
static const wstr S709 = { 3, 3, 1, 0, "&fr" };
static const wstr S710 = { 7, 7, 1, 0, "rt_chk(" };
static const wstr S711 = { 5, 5, 1, 0, "rt_f(" };
static const wstr S712 = { 5, 5, 1, 0, "rt_s(" };
static const wstr S713 = { 8, 8, 1, 0, "rt_text(" };
static const wstr S714 = { 7, 7, 1, 0, "rt_cat(" };
static const wstr S715 = { 5, 5, 1, 0, "rt_b(" };
static const wstr S716 = { 6, 6, 1, 0, "RT_NAN" };
static const wstr S717 = { 6, 6, 1, 0, "RT_INF" };
static const wstr S718 = { 2, 2, 1, 0, "()" };
static const wstr S719 = { 7, 7, 1, 0, "rt_idx(" };
static const wstr S720 = { 7, 7, 1, 0, "rt_get(" };
static const wstr S721 = { 13, 13, 1, 0, "rt_b(rt_bool(" };
static const wstr S722 = { 3, 3, 1, 0, ", \"" };
static const wstr S723 = { 3, 3, 1, 0, "\") " };
static const wstr S724 = { 9, 9, 1, 0, " rt_bool(" };
static const wstr S725 = { 3, 3, 1, 0, "\"))" };
static const wstr S726 = { 9, 9, 1, 0, "(rt_cond(" };
static const wstr S727 = { 4, 4, 1, 0, ") ? " };
static const wstr S728 = { 30, 30, 1, 0, "codegen: unhandled expression " };
static const wstr S729 = { 2, 2, 1, 0, "\r\n" };
static const wstr S730 = { 20, 20, 1, 0, "// ---- program ----" };
static const wstr S731 = { 19, 19, 1, 0, "static const wstr S" };
static const wstr S732 = { 14, 14, 1, 0, "static wstr F_" };
static const wstr S733 = { 26, 26, 1, 0, "static const wstr *const K" };
static const wstr S734 = { 5, 5, 1, 0, " *up;" };
static const wstr S735 = { 10, 10, 1, 0, "char none;" };
static const wstr S736 = { 3, 3, 1, 0, " { " };
static const wstr S737 = { 2, 2, 1, 0, " {" };
static const wstr S738 = { 39, 39, 1, 0, "static const char *const W_files[] = { " };
static const wstr S739 = { 28, 28, 1, 0, "static void wvt_main(void) {" };
static const wstr S740 = { 21, 21, 1, 0, "  rt_files = W_files;" };
static const wstr S741 = { 10, 10, 1, 0, "  rt_syms(" };
static const wstr S742 = { 19, 19, 1, 0, ", (wstr *const[]){ " };
static const wstr S743 = { 4, 4, 1, 0, " });" };
static const wstr S744 = { 11, 11, 1, 0, "  rt_roots(" };
static const wstr S745 = { 17, 17, 1, 0, ", (wv *const[]){ " };
static const wstr S746 = { 4, 4, 1, 0, "  | " };
static const wstr S747 = { 4, 4, 1, 0, "  { " };
static const wstr S748 = { 4, 4, 1, 0, "    " };
static const wstr S749 = { 3, 3, 1, 0, "  }" };
static const wstr S750 = { 2, 2, 1, 0, "[]" };
static const wstr S751 = { 52, 52, 1, 0, "cannot find the standard library: no lib/ folder in " };
static const wstr S752 = { 14, 14, 1, 0, "import cycle: " };
static const wstr S753 = { 4, 4, 1, 0, " -> " };
static const wstr S754 = { 29, 29, 1, 0, "the standard library has no '" };
static const wstr S755 = { 3, 3, 1, 0, "' (" };
static const wstr S756 = { 13, 13, 1, 0, "cannot read '" };
static const wstr S757 = { 3, 1, 0, 0, "﻿" };
static const wstr S758 = { 7, 7, 1, 0, "usage:\n" };
static const wstr S759 = { 64, 64, 1, 0, "  wvc <file.wvt> [-o <out.c>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S760 = { 90, 90, 1, 0, "  wvc build <file.wvt> [-o <program>] [--cc <compiler>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S761 = { 26, 26, 1, 0, "  wvc --tokens <file.wvt>\n" };
static const wstr S762 = { 23, 23, 1, 0, "  wvc --ast <file.wvt>\n" };
static const wstr S763 = { 54, 54, 1, 0, "  wvc --types <file.wvt> [--text <file>] [--lib <dir>]" };
static const wstr S764 = { 17, 17, 1, 0, "wvc: cannot read " };
static const wstr S765 = { 5, 5, 1, 0, "build" };
static const wstr S766 = { 8, 8, 1, 0, "--tokens" };
static const wstr S767 = { 5, 5, 1, 0, "--ast" };
static const wstr S768 = { 7, 7, 1, 0, "--types" };
static const wstr S769 = { 2, 2, 1, 0, "-o" };
static const wstr S770 = { 9, 9, 1, 0, "--runtime" };
static const wstr S771 = { 4, 4, 1, 0, "--cc" };
static const wstr S772 = { 5, 5, 1, 0, "--lib" };
static const wstr S773 = { 6, 6, 1, 0, "--text" };
static const wstr S774 = { 2, 2, 1, 0, "OS" };
static const wstr S775 = { 10, 10, 1, 0, "Windows_NT" };
static const wstr S776 = { 3, 3, 1, 0, "ZIG" };
static const wstr S777 = { 3, 3, 1, 0, "zig" };
static const wstr S778 = { 5, 5, 1, 0, "wvc: " };
static const wstr S779 = { 52, 52, 1, 0, " does not end with .wvt, so name the program with -o" };
static const wstr S780 = { 4, 4, 1, 0, ".exe" };
static const wstr S781 = { 13, 13, 1, 0, "runtime/wvt.c" };
static const wstr S782 = { 3, 3, 1, 0, "../" };
static const wstr S783 = { 3, 3, 1, 0, "lib" };
static const wstr S784 = { 6, 6, 1, 0, "../lib" };
static const wstr S785 = { 2, 2, 1, 0, ".c" };
static const wstr S786 = { 18, 18, 1, 0, "wvc: cannot write " };
static const wstr S787 = { 3, 3, 1, 0, "-O2" };
static const wstr S788 = { 7, 7, 1, 0, "zig.exe" };
static const wstr S789 = { 2, 2, 1, 0, "cc" };
static const wstr S790 = { 16, 16, 1, 0, "wvc: cannot run " };
static const wstr S791 = { 67, 67, 1, 0, ": install Zig (https://ziglang.org), or name a C compiler with --cc" };
static const wstr S792 = { 23, 23, 1, 0, " failed with exit code " };
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
static wstr F_elem = { 4, 4, 1, 0, "elem" };
static wstr F_types = { 5, 5, 1, 0, "types" };
static wstr F_id = { 2, 2, 1, 0, "id" };
static wstr F_mod = { 3, 3, 1, 0, "mod" };
static wstr F_kind = { 4, 4, 1, 0, "kind" };
static wstr F_parent = { 6, 6, 1, 0, "parent" };
static wstr F_visibleAt = { 9, 9, 1, 0, "visibleAt" };
static wstr F_fnId = { 4, 4, 1, 0, "fnId" };
static wstr F_lib = { 3, 3, 1, 0, "lib" };
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
static const wstr *const K41[] = { &F_t };
static const wstr *const K42[] = { &F_t, &F_elem };
static const wstr *const K43[] = { &F_t, &F_names, &F_types };
static const wstr *const K44[] = { &F_name, &F_args };
static const wstr *const K45[] = { &F_t, &F_name, &F_id };
static const wstr *const K46[] = { &F_parent, &F_names };
static const wstr *const K47[] = { &F_kind, &F_name, &F_isVar, &F_isRef, &F_namePos, &F_visibleAt, &F_fnId, &F_arity, &F_mod, &F_lib };
static const wstr *const K48[] = { &F_mod, &F_name, &F_namePos, &F_body };
static const wstr *const K49[] = { &F_mod, &F_name, &F_members };
static const wstr *const K50[] = { &F_id, &F_top };
static const wstr *const K51[] = { &F_func, &F_loops, &F_guards };
static const wstr *const K52[] = { &F_key, &F_pos };
static const wstr *const K53[] = { &F_mod, &F_namePos, &F_pos, &F_useMod };
static const wstr *const K54[] = { &F_stmts, &F_value };
static const wstr *const K55[] = { &F_name, &F_kind, &F_d };
static const wstr *const K56[] = { &F_name, &F_parent, &F_depth, &F_top, &F_children, &F_cname };
static const wstr *const K57[] = { &F_kind, &F_name, &F_func, &F_cname, &F_isRef, &F_captured, &F_fnIndex };
static const wstr *const K58[] = { &F_name, &F_lib };
static const wstr *const K59[] = { &F_c, &F_node };
static const wstr *const K60[] = { &F_c, &F_k };
static const wstr *const K61[] = { &F_pre, &F_cs };
static const wstr *const K62[] = { &F_proto, &F_lines };
static const wstr *const K63[] = { &F_k, &F_js, &F_pos, &F_paren };
static const wstr *const K64[] = { &F_node, &F_lib };
static const wstr *const K65[] = { &F_file, &F_chars, &F_path, &F_name, &F_program, &F_imports, &F_exports, &F_notLibrary, &F_refTargets };
struct fr27 { wv v71_cs; wv v72_n; wv v73_i; wv v74_prevT; wv v75_nlNow; wv v76_toks; };
struct fr49 { wv v185_toks; wv v187_p; wv v188_depth; wv v189_exports; wv v190_firstExport; };
struct fr119 { wv v462_modules; wv v463_requireExit; wv v464_errors; wv v465_nextFn; wv v466_curMod; wv v467_functions; wv v468_uses; wv v469_enums; wv v470_bindingTypes; wv v471_fnTypes; wv v472_returnTypes; wv v473_rebound; wv v474_allDecls; wv v522_topFn; };
struct fr176 { wv v839_modules; wv v840_runtime; wv v841_n; wv v842_tmps; wv v843_strings; wv v844_fields; wv v845_shapes; wv v846_enumDefs; wv v847_globals; wv v848_frames; wv v849_functions; wv v850_out; wv v851_indent; wv v852_loops; wv v853_iife; wv v854_decls; wv v855_fns; wv v856_tops; wv v857_slots; wv v858_curMod; wv v859_mod; wv v860_curSlot; wv v861_stolen; wv v862_ownEnums; wv v863_cur; wv v864_lineStarts; };
struct fr250 { wv v1281_libDir; wv v1282_searched; wv v1283_modules; wv v1284_libRoot; };
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
static wv w5_tInt;
static wv w5_tFloat;
static wv w5_tBool;
static wv w5_tString;
static wv w5_tNull;
static wv w5_tArray;
static wv w5_tMap;
static wv w5_tNever;
static wv w5_lastDecls;
static wv w5_lastBindingTypes;
static wv w5_lastFnTypes;
static wv w5_stdAccepts;
static wv w5_hexDigits;
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
static wv w_textGiven;
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
static wv v1320_a;
static wv v1335_tokens;
static wv v1336_program;
static wv v1337_p;
static wv v1338_p;
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
static wv w5_recordType(wv v401_names, wv v402_types);
static wv w5_arrayOf(wv v403_elem);
static wv w5_elemOf(wv v404_x);
static wv w5_sameElems(wv v405_x);
static wv w5_known(wv v406_x);
static wv w5_typeText(wv v407_x);
static wv w5_join(wv v410_a, wv v411_b);
static wv w5_thaw(wv v415_x);
static wv w5_noTextHint(wv v416_x);
static wv w5_stdResult(wv v417_name, wv v418_args);
static wv w5_stdAccepted(wv v419_name, wv v420_i);
static wv w5_typesText(wv v422_ts);
static wv w5_rebinds(wv v425_node, wv v426_found);
static wv w5_jsonString(wv v431_s);
static wv w5_typeLabel(wv v435_x);
static wv w5_typeJson(wv v436_x);
static wv w5_typesJson(wv v442_modules);
static wv w5_endType(wv v459_b);
static wv w5_check(wv p_v462_modules, wv p_v463_requireExit);
static wv s475_error(struct fr119 *up, wv v529_pos, wv v530_message, wv v531_hint);
static wv s476_typeOfDecl(struct fr119 *up, wv v532_d);
static wv s477_bindType(struct fr119 *up, wv v534_pos, wv v535_ty);
static wv s478_enumType(struct fr119 *up, wv v536_d);
static wv s479_noText(struct fr119 *up, wv v537_x, wv v538_ty);
static wv s480_condition(struct fr119 *up, wv v540_x, wv v541_ty);
static wv s481_needBool(struct fr119 *up, wv v542_op, wv v543_x, wv v544_ty);
static wv s482_needInt(struct fr119 *up, wv v545_op, wv v546_x, wv v547_ty);
static wv s483_fieldOf(struct fr119 *up, wv v548_rt, wv v549_name, wv v550_pos, wv v551_verb);
static wv s484_indexed(struct fr119 *up, wv v554_at, wv v555_index, wv v556_it, wv v557_pos, wv v558_verb);
static wv s485_binary(struct fr119 *up, wv v560_e, wv v561_lt, wv v562_rt);
static wv s486_enter(struct fr119 *up, wv v569_m);
static wv s487_enumMembers(struct fr119 *up, wv v570_d);
static wv s488_firstUse(struct fr119 *up, wv v572_mod, wv v573_declPos);
static wv s489_requireValue(struct fr119 *up, wv v575_b);
static wv s490_newScope(struct fr119 *up, wv v578_parent);
static wv s491_decl(struct fr119 *up, wv v580_kind, wv v581_name, wv v582_mode, wv v583_namePos, wv v584_visibleAt, wv v585_fnId, wv v586_arity);
static wv s492_own(struct fr119 *up, wv v587_scope, wv v588_name);
static wv s493_declare(struct fr119 *up, wv v590_scope, wv v591_name, wv v592_d);
static wv s494_lookup(struct fr119 *up, wv v596_scope, wv v597_name);
static wv s495_resolve(struct fr119 *up, wv v600_name, wv v601_pos, wv v602_scope, wv v603_ctx);
static wv s496_block(struct fr119 *up, wv v605_b, wv v606_scope, wv v607_ctx);
static wv s497_stmt(struct fr119 *up, wv v612_s, wv v613_scope, wv v614_ctx);
static wv s498_forLoop(struct fr119 *up, wv v627_s, wv v628_scope, wv v629_ctx);
static wv s499_rangeBound(struct fr119 *up, wv v639_x, wv v640_ty);
static wv s500_collection(struct fr119 *up, wv v642_x, wv v643_ty);
static wv s501_pathOf(struct fr119 *up, wv v645_x);
static wv s502_guarded(struct fr119 *up, wv v648_t, wv v649_ctx);
static wv s503_onfailHandler(struct fr119 *up, wv v657_s, wv v658_scope, wv v659_ctx);
static wv s504_checkMatch(struct fr119 *up, wv v663_e, wv v664_scope, wv v665_ctx, wv v666_stmtLevel);
static wv s505_moduleNamed(struct fr119 *up, wv v687_x, wv v688_scope);
static wv s506_exported(struct fr119 *up, wv v690_lib, wv v691_name);
static wv s507_enumNamed(struct fr119 *up, wv v694_x, wv v695_scope);
static wv s508_exportOf(struct fr119 *up, wv v699_e, wv v700_m);
static wv s509_unknownModule(struct fr119 *up, wv v702_x, wv v703_scope);
static wv s510_qualified(struct fr119 *up, wv v705_e, wv v706_m);
static wv s511_caseEnum(struct fr119 *up, wv v709_v, wv v710_scope);
static wv s512_member(struct fr119 *up, wv v716_e, wv v717_d);
static wv s513_chain(struct fr119 *up, wv v722_e, wv v723_scope, wv v724_ctx, wv v725_stmtLevel);
static wv s514_lvalue(struct fr119 *up, wv v729_t, wv v730_scope, wv v731_ctx);
static wv s515_mustBeMutable(struct fr119 *up, wv v740_d, wv v741_name, wv v742_pos, wv v743_message, wv v744_rebind);
static wv s516_unknownStd(struct fr119 *up, wv v746_node);
static wv s517_expr(struct fr119 *up, wv v747_e, wv v748_scope, wv v749_ctx);
static wv s518_callee(struct fr119 *up, wv v768_e, wv v769_scope, wv v770_used, wv v771_types);
static wv s519_importNames(struct fr119 *up, wv v784_m, wv v785_scope);
static wv s520_libraryRules(struct fr119 *up, wv v794_m, wv v795_scope, wv v796_isEntry, wv v797_last);
static wv s521_checkModule(struct fr119 *up, wv v804_m, wv v805_isEntry);
static wv w6_binFn(wv v816_op);
static wv w6_modeOf(wv v818_x);
static wv w6_unFn(wv v819_op);
static wv w6_repeat(wv v820_s, wv v821_count);
static wv w6_utf8Length(wv v824_s);
static wv w6_cString(wv v829_s);
static wv w6_wstrInit(wv v835_s);
static wv w6_values(wv v838_cs);
static wv w6_generateC(wv p_v839_modules, wv p_v840_runtime);
static wv s865_atLine(struct fr176 *up, wv v930_pos);
static wv s866_atExpr(struct fr176 *up, wv v934_pos, wv v935_c);
static wv s867_modId(struct fr176 *up, wv v936_m);
static wv s868_hasFrame(struct fr176 *up, wv v937_f);
static wv s869_newScope(struct fr176 *up, wv v938_parent);
static wv s870_put(struct fr176 *up, wv v940_scope, wv v941_name, wv v942_kind, wv v943_d);
static wv s871_find(struct fr176 *up, wv v944_scope, wv v945_name);
static wv s872_kindOf(struct fr176 *up, wv v949_scope, wv v950_name);
static wv s873_newFn(struct fr176 *up, wv v952_name, wv v953_parent);
static wv s874_bind(struct fr176 *up, wv v955_kind, wv v956_name, wv v957_f, wv v958_atTop, wv v959_mode);
static wv s875_ablock(struct fr176 *up, wv v962_b, wv v963_scope, wv v964_f, wv v965_topScope);
static wv s876_astmt(struct fr176 *up, wv v973_s, wv v974_scope, wv v975_f, wv v976_topScope);
static wv s877_use(struct fr176 *up, wv v991_x, wv v992_scope, wv v993_f);
static wv s878_atarget(struct fr176 *up, wv v995_t, wv v996_scope, wv v997_f);
static wv s879_notValue(struct fr176 *up, wv v998_x, wv v999_scope);
static wv s880_aexpr(struct fr176 *up, wv v1001_e, wv v1002_scope, wv v1003_f);
static wv s881_analyze(struct fr176 *up, wv v1011_m);
static wv s882_enter(struct fr176 *up, wv v1018_m);
static wv s883_emit(struct fr176 *up, wv v1025_line);
static wv s884_tmp(struct fr176 *up);
static wv s885_str(struct fr176 *up, wv v1027_s);
static wv s886_fieldRef(struct fr176 *up, wv v1029_name);
static wv s887_enumDef(struct fr176 *up, wv v1030_s, wv v1031_m);
static wv s888_stolenFrom(struct fr176 *up, wv v1035_x);
static wv s889_enumIn(struct fr176 *up, wv v1037_list, wv v1038_name);
static wv s890_enumOf(struct fr176 *up, wv v1040_x);
static wv s891_place(struct fr176 *up, wv v1045_d);
static wv s892_tdz(struct fr176 *up, wv v1047_d);
static wv s893_read(struct fr176 *up, wv v1048_d);
static wv s894_seq(struct fr176 *up, wv v1049_args);
static wv s895_wrap(struct fr176 *up, wv v1056_sq, wv v1057_inner);
static wv s896_stmts(struct fr176 *up, wv v1058_list);
static wv s897_resets(struct fr176 *up, wv v1060_b);
static wv s898_blockBody(struct fr176 *up, wv v1064_b);
static wv s899_valueOf(struct fr176 *up, wv v1065_b);
static wv s900_stmt(struct fr176 *up, wv v1066_s);
static wv s901_pathText(struct fr176 *up, wv v1068_x);
static wv s902_forStmt(struct fr176 *up, wv v1070_s);
static wv s903_signal(struct fr176 *up, wv v1085_code);
static wv s904_bindDecl(struct fr176 *up, wv v1086_d, wv v1087_value);
static wv s905_fnDef(struct fr176 *up, wv v1089_s);
static wv s906_bindRhs(struct fr176 *up, wv v1109_e, wv v1110_mode);
static wv s907_setStmt(struct fr176 *up, wv v1112_s);
static wv s908_exprs(struct fr176 *up, wv v1127_list);
static wv s909_array(struct fr176 *up, wv v1130_e, wv v1131_mutable);
static wv s910_mapLit(struct fr176 *up, wv v1133_e, wv v1134_mutable);
static wv s911_record(struct fr176 *up, wv v1136_e, wv v1137_mutable);
static wv s912_onfailStmt(struct fr176 *up, wv v1145_s);
static wv s913_chainStmt(struct fr176 *up, wv v1154_e);
static wv s914_test(struct fr176 *up, wv v1157_b);
static wv s915_branchCount(struct fr176 *up, wv v1158_e);
static wv s916_branchBody(struct fr176 *up, wv v1159_e, wv v1160_i);
static wv s917_branchTest(struct fr176 *up, wv v1161_e, wv v1162_t, wv v1163_i);
static wv s918_isPlain(struct fr176 *up, wv v1164_e);
static wv s919_ternaryChain(struct fr176 *up, wv v1168_e, wv v1169_t);
static wv s920_branchValue(struct fr176 *up, wv v1172_e, wv v1173_t, wv v1174_subject);
static wv s921_chainExpr(struct fr176 *up, wv v1183_e);
static wv s922_caseTest(struct fr176 *up, wv v1184_t, wv v1185_a);
static wv s923_matchElse(struct fr176 *up, wv v1188_e, wv v1189_t);
static wv s924_matchStmt(struct fr176 *up, wv v1190_e);
static wv s925_matchExpr(struct fr176 *up, wv v1194_e);
static wv s926_callC(struct fr176 *up, wv v1198_callee, wv v1199_args, wv v1200_checked);
static wv s927_expr(struct fr176 *up, wv v1209_e);
static wv s928_assemble(struct fr176 *up);
static wv w7_dumpTokens(wv v1241_toks, wv v1242_indent);
static wv w7_sorted(wv v1248_xs);
static wv w7_dumpValue(wv v1253_x, wv v1254_indent);
static wv w8_dirOf(wv v1263_path);
static wv w8_normalize(wv v1266_path);
static wv w8_with(wv v1271_xs, wv v1272_x);
static wv w8_failIn(wv v1275_file, wv v1276_chars, wv v1277_pos, wv v1278_message);
static wv w8_loadProgram(wv v1279_entryFile, wv v1280_entryChars, wv p_v1281_libDir, wv p_v1282_searched);
static wv s1285_traceName(struct fr250 *up, wv v1290_path);
static wv s1286_target(struct fr250 *up, wv v1291_s, wv v1292_file, wv v1293_chars, wv v1294_path);
static wv s1287_loaded(struct fr250 *up, wv v1295_path);
static wv s1288_visit(struct fr250 *up, wv v1297_file, wv v1298_chars, wv v1299_path, wv v1300_chain);
static wv w_die(void);
static wv w_load(wv v1315_file, wv v1316_from);
static wv w_onWindows(void);
static wv w_tryRead(wv v1322_path);
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
  RT_AT(4, 46);
  wv v384_k = rt_i(0);
  RT_AT(4, 47);
  while (rt_cond((RT_AT(4, 47), ({ wv t605 = v384_k; wv t606 = std_length(rt_def(w5_stdlib, "stdlib")); rt_lt(t605, t606); })))) {
    RT_AT(4, 48);
    if (rt_cond(({ wv t609 = rt_get(&F_name, ({ wv t607 = v384_k; wv t608 = rt_def(w5_stdlib, "stdlib"); rt_idx(t607, t608); })); wv t610 = v383_name; rt_eq(t609, t610); }))) {
      RT_AT(4, 48);
      RT_RET(rt_get(&F_arity, ({ wv t611 = v384_k; wv t612 = rt_def(w5_stdlib, "stdlib"); rt_idx(t611, t612); })));
    }
    RT_AT(4, 49);
    v384_k = rt_add(v384_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 51), RT_NULL));
}

static wv w5_modeOf(wv v385_x) {
  rt_enter("modeOf");
  v385_x = rt_bind(v385_x, 0);
  RT_RET((RT_AT(4, 55), (rt_cond(rt_get(&F_isVar, v385_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v385_x)) ? rt_s(&S37) : rt_s(&S13)))));
}

static wv w5_kindText(wv v386_kind) {
  rt_enter("kindText");
  v386_kind = rt_bind(v386_kind, 0);
  RT_RET((RT_AT(4, 58), (rt_cond(rt_eq(v386_kind, rt_s(&S33))) ? rt_s(&S406) : (rt_cond(rt_eq(v386_kind, rt_s(&S47))) ? rt_s(&S407) : rt_cat(2, (wv[]){ rt_s(&S0), rt_text(v386_kind) })))));
}

static wv w5_plural(wv v387_n) {
  rt_enter("plural");
  v387_n = rt_bind(v387_n, 0);
  RT_RET((RT_AT(4, 60), ({ wv t613 = rt_cat(2, (wv[]){ rt_text(v387_n), rt_s(&S408) }); wv t614 = (rt_cond(rt_eq(v387_n, rt_i(1))) ? rt_s(&S13) : rt_s(&S328)); rt_add(t613, t614); })));
}

static wv w5_enumText(wv v388_x) {
  rt_enter("enumText");
  v388_x = rt_bind(v388_x, 0);
  RT_RET((RT_AT(4, 68), (rt_cond(rt_eq(rt_get(&F_k, v388_x), rt_s(&S214))) ? rt_get(&F_name, v388_x) : ({ wv t615 = rt_text(rt_get(&F_name, rt_get(&F_rec, v388_x))); wv t616 = rt_text(rt_get(&F_field, v388_x)); rt_cat(3, (wv[]){ t615, rt_s(&S130), t616 }); }))));
}

static wv w5_caseKey(wv v389_v) {
  rt_enter("caseKey");
  v389_v = rt_bind(v389_v, 0);
  RT_AT(4, 72);
  if (rt_cond(rt_eq(rt_get(&F_k, v389_v), rt_s(&S173)))) {
    RT_AT(4, 72);
    RT_RET(rt_add(rt_s(&S409), rt_idx(rt_i(0), rt_get(&F_parts, v389_v))));
  }
  RT_AT(4, 73);
  if (rt_cond(rt_eq(rt_get(&F_k, v389_v), rt_s(&S192)))) {
    RT_AT(4, 73);
    RT_RET(({ wv t617 = rt_text(rt_get(&F_field, v389_v)); wv t618 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v389_v)), "caseKey")); rt_cat(4, (wv[]){ rt_s(&S410), t617, rt_s(&S130), t618 }); }));
  }
  RT_AT(4, 74);
  if (rt_cond(rt_eq(rt_get(&F_k, v389_v), rt_s(&S52)))) {
    RT_AT(4, 74);
    RT_RET(rt_s(&S52));
  }
  RT_RET((RT_AT(4, 75), ({ wv t619 = rt_text(rt_get(&F_k, v389_v)); wv t620 = rt_text(rt_get(&F_v, v389_v)); rt_cat(3, (wv[]){ t619, rt_s(&S28), t620 }); })));
}

static wv w5_caseText(wv v390_v) {
  rt_enter("caseText");
  v390_v = rt_bind(v390_v, 0);
  RT_AT(4, 78);
  if (rt_cond(rt_eq(rt_get(&F_k, v390_v), rt_s(&S173)))) {
    RT_AT(4, 78);
    RT_RET(rt_chk(w1_quote(rt_idx(rt_i(0), rt_get(&F_parts, v390_v))), "caseText"));
  }
  RT_AT(4, 79);
  if (rt_cond(rt_eq(rt_get(&F_k, v390_v), rt_s(&S192)))) {
    RT_AT(4, 79);
    RT_RET(({ wv t621 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v390_v)), "caseText")); wv t622 = rt_text(rt_get(&F_field, v390_v)); rt_cat(3, (wv[]){ t621, rt_s(&S130), t622 }); }));
  }
  RT_AT(4, 80);
  if (rt_cond(rt_eq(rt_get(&F_k, v390_v), rt_s(&S52)))) {
    RT_AT(4, 80);
    RT_RET(rt_s(&S52));
  }
  RT_RET((RT_AT(4, 81), rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v390_v)) })));
}

static wv w5_getsAwayWithValue(wv v391_list) {
  rt_enter("getsAwayWithValue");
  RT_AT(4, 86);
  wv v392_i = rt_i(0);
  RT_AT(4, 87);
  while (rt_cond((RT_AT(4, 87), ({ wv t623 = v392_i; wv t624 = std_length(v391_list); rt_lt(t623, t624); })))) {
    RT_AT(4, 88);
    wv v393_s = rt_bind(rt_idx(v392_i, v391_list), 0);
    RT_AT(4, 89);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_handler, v393_s), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v393_s)))), "getsAwayWithValue"), "&&")))) {
      RT_AT(4, 89);
      RT_RET(rt_b(1));
    }
    RT_AT(4, 90);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S34)), "&&") && rt_bool(rt_ne(rt_get(&F_e, v393_s), RT_NULL), "&&")))) {
      RT_AT(4, 90);
      RT_RET(rt_b(1));
    }
    RT_AT(4, 91);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S41)), "||") || rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S42)), "||")), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v393_s))), "getsAwayWithValue"), "&&")))) {
      RT_AT(4, 91);
      RT_RET(rt_b(1));
    }
    RT_AT(4, 92);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v393_s)), rt_s(&S186)), "&&")))) {
      RT_AT(4, 93);
      wv v394_b = rt_i(0);
      RT_AT(4, 94);
      while (rt_cond((RT_AT(4, 94), ({ wv t625 = v394_b; wv t626 = std_length(rt_get(&F_branches, rt_get(&F_e, v393_s))); rt_lt(t625, t626); })))) {
        RT_AT(4, 95);
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t627 = v394_b; wv t628 = rt_get(&F_branches, rt_get(&F_e, v393_s)); rt_idx(t627, t628); })))), "getsAwayWithValue"))) {
          RT_AT(4, 95);
          RT_RET(rt_b(1));
        }
        RT_AT(4, 96);
        v394_b = rt_add(v394_b, rt_i(1));
      }
      RT_AT(4, 98);
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v393_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v393_s)))), "getsAwayWithValue"), "&&")))) {
        RT_AT(4, 98);
        RT_RET(rt_b(1));
      }
    }
    RT_AT(4, 100);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v393_s), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v393_s)), rt_s(&S45)), "&&")))) {
      RT_AT(4, 101);
      wv v395_a = rt_i(0);
      RT_AT(4, 102);
      while (rt_cond((RT_AT(4, 102), ({ wv t629 = v395_a; wv t630 = std_length(rt_get(&F_cases, rt_get(&F_e, v393_s))); rt_lt(t629, t630); })))) {
        RT_AT(4, 103);
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t631 = v395_a; wv t632 = rt_get(&F_cases, rt_get(&F_e, v393_s)); rt_idx(t631, t632); })))), "getsAwayWithValue"))) {
          RT_AT(4, 103);
          RT_RET(rt_b(1));
        }
        RT_AT(4, 104);
        v395_a = rt_add(v395_a, rt_i(1));
      }
      RT_AT(4, 106);
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v393_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v393_s)))), "getsAwayWithValue"), "&&")))) {
        RT_AT(4, 106);
        RT_RET(rt_b(1));
      }
    }
    RT_AT(4, 108);
    v392_i = rt_add(v392_i, rt_i(1));
  }
  RT_RET((RT_AT(4, 110), rt_b(0)));
}

static wv w5_lineIn(wv v396_chars, wv v397_pos) {
  rt_enter("lineIn");
  v396_chars = rt_bind(v396_chars, 0);
  v397_pos = rt_bind(v397_pos, 0);
  RT_AT(4, 115);
  wv v398_line = rt_i(1);
  RT_AT(4, 116);
  wv v399_k = rt_i(0);
  RT_AT(4, 117);
  while (rt_cond((RT_AT(4, 117), rt_lt(v399_k, v397_pos)))) {
    RT_AT(4, 118);
    if (rt_cond(rt_eq(rt_idx(v399_k, v396_chars), rt_s(&S2)))) {
      RT_AT(4, 118);
      v398_line = rt_add(v398_line, rt_i(1));
    }
    RT_AT(4, 119);
    v399_k = rt_add(v399_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 121), v398_line));
}

static wv w5_lineOf(wv v400_pos) {
  rt_enter("lineOf");
  v400_pos = rt_bind(v400_pos, 0);
  RT_RET((RT_AT(4, 123), rt_chk(w5_lineIn(w2_srcChars, v400_pos), "lineOf")));
}

static wv w5_recordType(wv v401_names, wv v402_types) {
  rt_enter("recordType");
  v401_names = rt_bind(v401_names, 0);
  v402_types = rt_bind(v402_types, 0);
  RT_RET((RT_AT(4, 151), rt_rec(0, 3, K43, (wv[]){ rt_s(&S414), v401_names, v402_types })));
}

static wv w5_arrayOf(wv v403_elem) {
  rt_enter("arrayOf");
  v403_elem = rt_bind(v403_elem, 0);
  RT_RET((RT_AT(4, 155), rt_rec(0, 2, K42, (wv[]){ rt_s(&S412), (rt_cond(rt_b(rt_bool(rt_ne(v403_elem, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_t, v403_elem), rt_s(&S413)), "&&"))) ? RT_NULL : v403_elem) })));
}

static wv w5_elemOf(wv v404_x) {
  rt_enter("elemOf");
  v404_x = rt_bind(v404_x, 0);
  RT_RET((RT_AT(4, 158), (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v404_x), "elemOf"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v404_x), rt_s(&S412)), "&&"))) ? rt_get(&F_elem, v404_x) : RT_NULL)));
}

static wv w5_sameElems(wv v405_x) {
  rt_enter("sameElems");
  v405_x = rt_bind(v405_x, 0);
  RT_RET((RT_AT(4, 161), (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v405_x), "sameElems"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v405_x), rt_s(&S412)), "&&"))) ? v405_x : rt_def(w5_tArray, "tArray"))));
}

static wv w5_known(wv v406_x) {
  rt_enter("known");
  v406_x = rt_bind(v406_x, 0);
  RT_RET((RT_AT(4, 170), rt_b(rt_bool(rt_ne(v406_x, RT_NULL), "&&") && rt_bool(rt_ne(rt_get(&F_t, v406_x), rt_s(&S413)), "&&"))));
}

static wv w5_typeText(wv v407_x) {
  rt_enter("typeText");
  v407_x = rt_bind(v407_x, 0);
  RT_AT(4, 174);
  if (rt_cond(rt_eq(rt_get(&F_t, v407_x), rt_s(&S47)))) {
    RT_AT(4, 174);
    RT_RET(rt_get(&F_name, v407_x));
  }
  RT_AT(4, 175);
  if (rt_cond(rt_ne(rt_get(&F_t, v407_x), rt_s(&S414)))) {
    RT_AT(4, 175);
    RT_RET(rt_get(&F_t, v407_x));
  }
  RT_AT(4, 176);
  if (rt_cond(rt_eq(std_length(rt_get(&F_names, v407_x)), rt_i(0)))) {
    RT_AT(4, 176);
    RT_RET(rt_s(&S415));
  }
  RT_AT(4, 177);
  wv v408_shown = rt_bind((rt_cond(rt_gt(std_length(rt_get(&F_names, v407_x)), rt_i(12))) ? std_slice(rt_get(&F_names, v407_x), rt_i(0), rt_i(12)) : rt_get(&F_names, v407_x)), 0);
  RT_AT(4, 178);
  wv v409_more = rt_bind((rt_cond(rt_gt(std_length(rt_get(&F_names, v407_x)), rt_i(12))) ? rt_cat(3, (wv[]){ rt_s(&S416), rt_text(rt_sub(std_length(rt_get(&F_names, v407_x)), rt_i(12))), rt_s(&S417) }) : rt_s(&S13)), 0);
  RT_RET((RT_AT(4, 179), ({ wv t633 = rt_text(std_join(v408_shown, rt_s(&S419))); wv t634 = rt_text(v409_more); rt_cat(4, (wv[]){ rt_s(&S418), t633, t634, rt_s(&S420) }); })));
}

static wv w5_join(wv v410_a, wv v411_b) {
  rt_enter("join");
  v410_a = rt_bind(v410_a, 0);
  v411_b = rt_bind(v411_b, 0);
  RT_AT(4, 185);
  if (rt_cond(rt_b(rt_bool(rt_eq(v410_a, RT_NULL), "||") || rt_bool(rt_eq(v411_b, RT_NULL), "||")))) {
    RT_AT(4, 185);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 186);
  if (rt_cond(rt_eq(rt_get(&F_t, v410_a), rt_s(&S413)))) {
    RT_AT(4, 186);
    RT_RET(v411_b);
  }
  RT_AT(4, 187);
  if (rt_cond(rt_eq(rt_get(&F_t, v411_b), rt_s(&S413)))) {
    RT_AT(4, 187);
    RT_RET(v410_a);
  }
  RT_AT(4, 188);
  if (rt_cond(({ wv t635 = rt_get(&F_t, v410_a); wv t636 = rt_get(&F_t, v411_b); rt_ne(t635, t636); }))) {
    RT_AT(4, 188);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 189);
  if (rt_cond(rt_eq(rt_get(&F_t, v410_a), rt_s(&S47)))) {
    RT_AT(4, 189);
    RT_RET((rt_cond(({ wv t637 = rt_get(&F_id, v410_a); wv t638 = rt_get(&F_id, v411_b); rt_eq(t637, t638); })) ? v410_a : RT_NULL));
  }
  RT_AT(4, 190);
  if (rt_cond(rt_eq(rt_get(&F_t, v410_a), rt_s(&S412)))) {
    RT_AT(4, 190);
    RT_RET(rt_chk(w5_arrayOf(({ wv t639 = rt_get(&F_elem, v410_a); wv t640 = rt_get(&F_elem, v411_b); rt_chk(w5_join(t639, t640), "join"); })), "join"));
  }
  RT_AT(4, 191);
  if (rt_cond(rt_ne(rt_get(&F_t, v410_a), rt_s(&S414)))) {
    RT_AT(4, 191);
    RT_RET(v410_a);
  }
  RT_AT(4, 192);
  if (rt_cond(rt_not(({ wv t641 = rt_get(&F_names, v410_a); wv t642 = rt_get(&F_names, v411_b); std_same(t641, t642); })))) {
    RT_AT(4, 192);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 193);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_types, v410_a), RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_types, v411_b), RT_NULL), "||")))) {
    RT_AT(4, 193);
    RT_RET(rt_chk(w5_recordType(rt_get(&F_names, v410_a), RT_NULL), "join"));
  }
  RT_AT(4, 194);
  wv v412_types = rt_arr(0, NULL, 1);
  RT_AT(4, 195);
  {
    wfor t644;
    rt_for_start(&t644, rt_get(&F_types, v410_a), 0);
    for (int64_t t643 = 0; rt_for_guard(&t644, rt_get(&F_types, v410_a), "a.types"), rt_for_more(&t644, t643); t643++) {
      wv v413_i = rt_for_key(&t644, t643);
      wv v414_ft = rt_for_val(&t644, t643);
      RT_AT(4, 195);
      (void)({ wv t649 = v412_types; wv t650 = ({ wv t647 = v414_ft; wv t648 = ({ wv t645 = v413_i; wv t646 = rt_get(&F_types, v411_b); rt_idx(t645, t646); }); rt_chk(w5_join(t647, t648), "join"); }); std_push(t649, t650); });
    }
  }
  RT_RET((RT_AT(4, 196), ({ wv t651 = rt_get(&F_names, v410_a); wv t652 = v412_types; rt_chk(w5_recordType(t651, t652), "join"); })));
}

static wv w5_thaw(wv v415_x) {
  rt_enter("thaw");
  v415_x = rt_bind(v415_x, 0);
  RT_AT(4, 202);
  if (rt_cond(rt_not(rt_chk(w5_known(v415_x), "thaw")))) {
    RT_AT(4, 202);
    RT_RET(v415_x);
  }
  RT_AT(4, 203);
  if (rt_cond(rt_eq(rt_get(&F_t, v415_x), rt_s(&S414)))) {
    RT_AT(4, 203);
    RT_RET(rt_chk(w5_recordType(rt_get(&F_names, v415_x), RT_NULL), "thaw"));
  }
  RT_RET((RT_AT(4, 204), (rt_cond(rt_eq(rt_get(&F_t, v415_x), rt_s(&S412))) ? rt_def(w5_tArray, "tArray") : v415_x)));
}

static wv w5_noTextHint(wv v416_x) {
  rt_enter("noTextHint");
  v416_x = rt_bind(v416_x, 0);
  RT_AT(4, 210);
  if (rt_cond(rt_not(rt_chk(w5_known(v416_x), "noTextHint")))) {
    RT_AT(4, 210);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 211);
  if (rt_cond(rt_eq(rt_get(&F_t, v416_x), rt_s(&S412)))) {
    RT_AT(4, 211);
    RT_RET(rt_s(&S421));
  }
  RT_AT(4, 212);
  if (rt_cond(rt_eq(rt_get(&F_t, v416_x), rt_s(&S414)))) {
    RT_AT(4, 212);
    RT_RET(rt_s(&S422));
  }
  RT_AT(4, 213);
  if (rt_cond(rt_eq(rt_get(&F_t, v416_x), rt_s(&S189)))) {
    RT_AT(4, 213);
    RT_RET(rt_s(&S423));
  }
  RT_RET((RT_AT(4, 214), RT_NULL));
}

static wv w5_stdResult(wv v417_name, wv v418_args) {
  rt_enter("stdResult");
  v417_name = rt_bind(v417_name, 0);
  v418_args = rt_bind(v418_args, 0);
  RT_RET((RT_AT(4, 219), ({ wv t653 = v417_name; (eq(t653, rt_s(&S371)) || eq(t653, rt_s(&S374)) || eq(t653, rt_s(&S394)) || eq(t653, rt_s(&S395)) || eq(t653, rt_s(&S396)) || eq(t653, rt_s(&S397)) || eq(t653, rt_s(&S398)) || eq(t653, rt_s(&S379)) || eq(t653, rt_s(&S400)) || eq(t653, rt_s(&S401)) || eq(t653, rt_s(&S404)) || eq(t653, rt_s(&S370)) ? (RT_AT(4, 220), rt_def(w5_tInt, "tInt")) : (eq(t653, rt_s(&S380)) || eq(t653, rt_s(&S405)) ? (RT_AT(4, 221), rt_def(w5_tFloat, "tFloat")) : (eq(t653, rt_s(&S378)) || eq(t653, rt_s(&S375)) || eq(t653, rt_s(&S377)) || eq(t653, rt_s(&S356)) || eq(t653, rt_s(&S357)) || eq(t653, rt_s(&S361)) || eq(t653, rt_s(&S368)) ? (RT_AT(4, 222), rt_def(w5_tString, "tString")) : (eq(t653, rt_s(&S383)) || eq(t653, rt_s(&S388)) || eq(t653, rt_s(&S399)) || eq(t653, rt_s(&S364)) ? (RT_AT(4, 223), rt_def(w5_tBool, "tBool")) : (eq(t653, rt_s(&S376)) || eq(t653, rt_s(&S393)) || eq(t653, rt_s(&S365)) ? (RT_AT(4, 225), rt_chk(w5_arrayOf(rt_def(w5_tString, "tString")), "stdResult")) : (eq(t653, rt_s(&S390)) || eq(t653, rt_s(&S391)) ? (RT_AT(4, 226), rt_def(w5_tArray, "tArray")) : (eq(t653, rt_s(&S384)) || eq(t653, rt_s(&S385)) || eq(t653, rt_s(&S386)) ? (RT_AT(4, 227), rt_chk(w5_sameElems(rt_idx(rt_i(0), v418_args)), "stdResult")) : (eq(t653, rt_s(&S373)) ? (RT_AT(4, 228), (rt_cond(rt_b(rt_bool(rt_chk(w5_known(rt_idx(rt_i(0), v418_args)), "stdResult"), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, rt_idx(rt_i(0), v418_args)), rt_s(&S187)), "||") || rt_bool(rt_eq(rt_get(&F_t, rt_idx(rt_i(0), v418_args)), rt_s(&S412)), "||")), "&&"))) ? rt_idx(rt_i(0), v418_args) : RT_NULL)) : (eq(t653, rt_s(&S372)) ? (RT_AT(4, 229), (rt_cond(rt_b(rt_bool(rt_chk(w5_known(rt_idx(rt_i(0), v418_args)), "stdResult"), "&&") && rt_bool(rt_eq(rt_get(&F_t, rt_idx(rt_i(0), v418_args)), rt_s(&S187)), "&&"))) ? rt_def(w5_tString, "tString") : rt_chk(w5_elemOf(rt_idx(rt_i(0), v418_args)), "stdResult"))) : (eq(t653, rt_s(&S359)) ? (RT_AT(4, 230), rt_chk(w5_recordType(rt_arr(2, (wv[]){ rt_s(&S426), rt_s(&S427) }, 0), RT_NULL), "stdResult")) : (eq(t653, rt_s(&S360)) ? (RT_AT(4, 231), rt_chk(w5_recordType(rt_arr(2, (wv[]){ rt_s(&S424), rt_s(&S425) }, 0), RT_NULL), "stdResult")) : (RT_AT(4, 232), RT_NULL)))))))))))); })));
}

static wv w5_stdAccepted(wv v419_name, wv v420_i) {
  rt_enter("stdAccepted");
  v419_name = rt_bind(v419_name, 0);
  v420_i = rt_bind(v420_i, 0);
  RT_AT(4, 261);
  {
    wfor t699;
    rt_for_start(&t699, rt_def(w5_stdAccepts, "stdAccepts"), 0);
    for (int64_t t698 = 0; rt_for_guard(&t699, rt_def(w5_stdAccepts, "stdAccepts"), "stdAccepts"), rt_for_more(&t699, t698); t698++) {
      wv v421_entry = rt_for_item(&t699, t698);
      RT_AT(4, 262);
      if (rt_cond(({ wv t700 = rt_get(&F_name, v421_entry); wv t701 = v419_name; rt_eq(t700, t701); }))) {
        RT_AT(4, 262);
        RT_RET(({ wv t702 = v420_i; wv t703 = rt_get(&F_args, v421_entry); rt_idx(t702, t703); }));
      }
    }
  }
  RT_RET((RT_AT(4, 264), RT_NULL));
}

static wv w5_typesText(wv v422_ts) {
  rt_enter("typesText");
  v422_ts = rt_bind(v422_ts, 0);
  RT_AT(4, 269);
  wv v423_words = rt_arr(0, NULL, 1);
  RT_AT(4, 270);
  {
    wfor t705;
    rt_for_start(&t705, v422_ts, 0);
    for (int64_t t704 = 0; rt_for_guard(&t705, v422_ts, "ts"), rt_for_more(&t705, t704); t704++) {
      wv v424_ty = rt_for_item(&t705, t704);
      RT_AT(4, 270);
      (void)({ wv t708 = v423_words; wv t709 = ({ wv t706 = (rt_cond(rt_b(rt_bool(rt_eq(v424_ty, rt_s(&S139)), "||") || rt_bool(rt_eq(v424_ty, rt_s(&S412)), "||"))) ? rt_s(&S428) : rt_s(&S429)); wv t707 = v424_ty; rt_add(t706, t707); }); std_push(t708, t709); });
    }
  }
  RT_AT(4, 271);
  if (rt_cond(rt_eq(std_length(v423_words), rt_i(1)))) {
    RT_AT(4, 271);
    RT_RET(rt_idx(rt_i(0), v423_words));
  }
  RT_RET((RT_AT(4, 272), ({ wv t710 = rt_text(std_join(std_slice(v423_words, rt_i(0), rt_i(-1)), rt_s(&S419))); wv t711 = rt_text(rt_idx(rt_i(-1), v423_words)); rt_cat(3, (wv[]){ t710, rt_s(&S430), t711 }); })));
}

static wv w5_rebinds(wv v425_node, wv v426_found) {
  rt_enter("rebinds");
  v425_node = rt_bind(v425_node, 0);
  RT_AT(4, 279);
  wv v427_what = rt_bind(std_typeOf(v425_node), 0);
  RT_AT(4, 280);
  if (rt_cond(rt_eq(v427_what, rt_s(&S412)))) {
    RT_AT(4, 281);
    {
      wfor t713;
      rt_for_start(&t713, v425_node, 0);
      for (int64_t t712 = 0; rt_for_guard(&t713, v425_node, "node"), rt_for_more(&t713, t712); t712++) {
        wv v429_item = rt_for_item(&t713, t712);
        RT_AT(4, 281);
        (void)rt_chk(w5_rebinds(v429_item, v426_found), "rebinds");
      }
    }
    RT_AT(4, 282);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 284);
  if (rt_cond(rt_ne(v427_what, rt_s(&S414)))) {
    RT_AT(4, 284);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 285);
  wv v428_fields = rt_bind(std_fields(v425_node), 0);
  RT_AT(4, 286);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w1_contains(v428_fields, rt_s(&S431)), "rebinds"), "&&") && rt_bool(rt_eq(rt_get(&F_k, v425_node), rt_s(&S38)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_target, v425_node)), rt_s(&S214)), "&&")))) {
    RT_AT(4, 287);
    ({ wv t714 = rt_get(&F_name, rt_get(&F_target, v425_node)); wv t715 = v426_found; rt_set_idx(rt_b(1), t714, t715); });
  }
  RT_AT(4, 289);
  {
    wfor t717;
    rt_for_start(&t717, v428_fields, 0);
    for (int64_t t716 = 0; rt_for_guard(&t717, v428_fields, "fields"), rt_for_more(&t717, t716); t716++) {
      wv v430_f = rt_for_item(&t717, t716);
      RT_AT(4, 289);
      (void)({ wv t718 = rt_idx(v430_f, v425_node); wv t719 = v426_found; rt_chk(w5_rebinds(t718, t719), "rebinds"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv w5_jsonString(wv v431_s) {
  rt_enter("jsonString");
  v431_s = rt_bind(v431_s, 0);
  RT_AT(4, 298);
  wv v432_parts = rt_arr(1, (wv[]){ rt_s(&S18) }, 1);
  RT_AT(4, 299);
  {
    wfor t721;
    rt_for_start(&t721, std_split(v431_s, rt_s(&S13)), 0);
    for (int64_t t720 = 0; rt_for_more(&t721, t720); t720++) {
      wv v433_c = rt_for_item(&t721, t720);
      RT_AT(4, 300);
      wv v434_code = rt_bind(std_code(v433_c), 0);
      RT_AT(4, 301);
      if (rt_cond(rt_b(rt_bool(rt_eq(v433_c, rt_s(&S18)), "||") || rt_bool(rt_eq(v433_c, rt_s(&S19)), "||")))) {
        RT_AT(4, 302);
        (void)({ wv t722 = v432_parts; wv t723 = rt_add(rt_s(&S19), v433_c); std_push(t722, t723); });
      } else if (rt_cond(rt_lt(v434_code, rt_i(32)))) {
        RT_AT(4, 304);
        (void)({ wv t730 = v432_parts; wv t731 = ({ wv t728 = rt_add(rt_s(&S25), ({ wv t724 = rt_def(w5_hexDigits, "hexDigits"); wv t725 = std_floor(rt_div(v434_code, rt_i(16))); std_at(t724, t725); })); wv t729 = ({ wv t726 = rt_def(w5_hexDigits, "hexDigits"); wv t727 = rt_mod(v434_code, rt_i(16)); std_at(t726, t727); }); rt_add(t728, t729); }); std_push(t730, t731); });
      } else {
        RT_AT(4, 306);
        (void)std_push(v432_parts, v433_c);
      }
    }
  }
  RT_AT(4, 309);
  (void)std_push(v432_parts, rt_s(&S18));
  RT_RET((RT_AT(4, 310), std_join(v432_parts, rt_s(&S13))));
}

static wv w5_typeLabel(wv v435_x) {
  rt_enter("typeLabel");
  v435_x = rt_bind(v435_x, 0);
  RT_AT(4, 316);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v435_x), rt_s(&S412)), "&&") && rt_bool(rt_ne(rt_get(&F_elem, v435_x), RT_NULL), "&&")))) {
    RT_AT(4, 316);
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S432), rt_text(rt_chk(w5_typeLabel(rt_get(&F_elem, v435_x)), "typeLabel")) }));
  }
  RT_RET((RT_AT(4, 317), rt_chk(w5_typeText(v435_x), "typeLabel")));
}

static wv w5_typeJson(wv v436_x) {
  rt_enter("typeJson");
  v436_x = rt_bind(v436_x, 0);
  RT_AT(4, 323);
  if (rt_cond(rt_eq(v436_x, RT_NULL))) {
    RT_AT(4, 323);
    RT_RET(rt_s(&S52));
  }
  RT_AT(4, 324);
  wv v437_out = rt_cat(2, (wv[]){ rt_s(&S433), rt_text(rt_chk(w5_jsonString(rt_chk(w5_typeLabel(v436_x), "typeJson")), "typeJson")) });
  RT_AT(4, 325);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v436_x), rt_s(&S412)), "&&") && rt_bool(rt_ne(rt_get(&F_elem, v436_x), RT_NULL), "&&")))) {
    RT_AT(4, 325);
    v437_out = ({ wv t732 = v437_out; wv t733 = rt_cat(2, (wv[]){ rt_s(&S434), rt_text(rt_chk(w5_typeJson(rt_get(&F_elem, v436_x)), "typeJson")) }); rt_add(t732, t733); });
  }
  RT_AT(4, 326);
  if (rt_cond(rt_eq(rt_get(&F_t, v436_x), rt_s(&S414)))) {
    RT_AT(4, 327);
    wv v438_fields = rt_arr(0, NULL, 1);
    RT_AT(4, 328);
    {
      wfor t735;
      rt_for_start(&t735, rt_get(&F_names, v436_x), 0);
      for (int64_t t734 = 0; rt_for_guard(&t735, rt_get(&F_names, v436_x), "x.names"), rt_for_more(&t735, t734); t734++) {
        wv v439_i = rt_for_key(&t735, t734);
        wv v440_n = rt_for_val(&t735, t734);
        RT_AT(4, 329);
        wv v441_ft = rt_bind((rt_cond(rt_eq(rt_get(&F_types, v436_x), RT_NULL)) ? RT_NULL : ({ wv t736 = v439_i; wv t737 = rt_get(&F_types, v436_x); rt_idx(t736, t737); })), 0);
        RT_AT(4, 330);
        (void)({ wv t740 = v438_fields; wv t741 = ({ wv t738 = rt_text(rt_chk(w5_jsonString(v440_n), "typeJson")); wv t739 = rt_text(rt_chk(w5_typeJson(v441_ft), "typeJson")); rt_cat(5, (wv[]){ rt_s(&S435), t738, rt_s(&S436), t739, rt_s(&S15) }); }); std_push(t740, t741); });
      }
    }
    RT_AT(4, 332);
    v437_out = ({ wv t742 = v437_out; wv t743 = rt_cat(3, (wv[]){ rt_s(&S437), rt_text(std_join(v438_fields, rt_s(&S419))), rt_s(&S100) }); rt_add(t742, t743); });
  }
  RT_RET((RT_AT(4, 334), rt_add(v437_out, rt_s(&S15))));
}

static wv w5_typesJson(wv v442_modules) {
  rt_enter("typesJson");
  v442_modules = rt_bind(v442_modules, 0);
  RT_AT(4, 344);
  wv v443_starts = rt_arr(0, NULL, 1);
  RT_AT(4, 345);
  {
    wfor t745;
    rt_for_start(&t745, v442_modules, 0);
    for (int64_t t744 = 0; rt_for_guard(&t745, v442_modules, "modules"), rt_for_more(&t745, t744); t744++) {
      wv v446_m = rt_for_item(&t745, t744);
      RT_AT(4, 346);
      wv v447_lines = rt_arr(1, (wv[]){ rt_i(0) }, 1);
      RT_AT(4, 347);
      {
        wfor t747;
        rt_for_start(&t747, rt_get(&F_chars, v446_m), 0);
        for (int64_t t746 = 0; rt_for_guard(&t747, rt_get(&F_chars, v446_m), "m.chars"), rt_for_more(&t747, t746); t746++) {
          wv v448_i = rt_for_key(&t747, t746);
          wv v449_c = rt_for_val(&t747, t746);
          RT_AT(4, 348);
          if (rt_cond(rt_eq(v449_c, rt_s(&S2)))) {
            RT_AT(4, 348);
            (void)({ wv t748 = v447_lines; wv t749 = rt_add(v448_i, rt_i(1)); std_push(t748, t749); });
          }
        }
      }
      RT_AT(4, 350);
      (void)std_push(v443_starts, v447_lines);
    }
  }
  RT_AT(4, 352);
  wv v444_names = rt_arr(0, NULL, 1);
  RT_AT(4, 353);
  {
    wfor t751;
    rt_for_start(&t751, rt_def(w5_lastDecls, "lastDecls"), 0);
    for (int64_t t750 = 0; rt_for_guard(&t751, rt_def(w5_lastDecls, "lastDecls"), "lastDecls"), rt_for_more(&t751, t750); t750++) {
      wv v450_d = rt_for_item(&t751, t750);
      RT_AT(4, 354);
      wv v451_key = ({ wv t752 = rt_text(rt_get(&F_mod, v450_d)); wv t753 = rt_text(rt_get(&F_namePos, v450_d)); rt_cat(3, (wv[]){ t752, rt_s(&S28), t753 }); });
      RT_AT(4, 355);
      wv v452_types = rt_bind((rt_cond(rt_eq(rt_get(&F_kind, v450_d), rt_s(&S33))) ? rt_def(w5_lastFnTypes, "lastFnTypes") : rt_def(w5_lastBindingTypes, "lastBindingTypes")), 0);
      RT_AT(4, 356);
      if (rt_cond(rt_not(std_has(v452_types, v451_key)))) {
        continue;
      }
      RT_AT(4, 357);
      wv v453_kind = rt_bind((rt_cond(rt_eq(rt_get(&F_kind, v450_d), rt_s(&S179))) ? (rt_cond(rt_get(&F_isVar, v450_d)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v450_d)) ? rt_s(&S37) : rt_s(&S35))) : rt_get(&F_kind, v450_d)), 0);
      RT_AT(4, 359);
      wv v454_ls = rt_bind(({ wv t754 = rt_get(&F_mod, v450_d); wv t755 = v443_starts; rt_idx(t754, t755); }), 0);
      RT_AT(4, 360);
      wv v455_lo = rt_i(0);
      RT_AT(4, 361);
      wv v456_hi = rt_sub(std_length(v454_ls), rt_i(1));
      RT_AT(4, 362);
      while (rt_cond((RT_AT(4, 362), rt_lt(v455_lo, v456_hi)))) {
        RT_AT(4, 363);
        wv v457_mid = rt_bind(std_floor(rt_div(rt_add(rt_add(v455_lo, v456_hi), rt_i(1)), rt_i(2))), 0);
        RT_AT(4, 364);
        if (rt_cond(({ wv t756 = rt_idx(v457_mid, v454_ls); wv t757 = rt_get(&F_namePos, v450_d); rt_le(t756, t757); }))) {
          RT_AT(4, 364);
          v455_lo = rt_bind(v457_mid, 1);
        } else {
          RT_AT(4, 364);
          v456_hi = rt_sub(v457_mid, rt_i(1));
        }
      }
      RT_AT(4, 366);
      (void)({ wv t766 = v444_names; wv t767 = ({ wv t760 = rt_text(rt_get(&F_mod, v450_d)); wv t761 = rt_text(rt_add(v455_lo, rt_i(1))); wv t762 = rt_text(rt_add(({ wv t758 = rt_get(&F_namePos, v450_d); wv t759 = rt_idx(v455_lo, v454_ls); rt_sub(t758, t759); }), rt_i(1))); wv t763 = rt_text(rt_chk(w5_jsonString(rt_get(&F_name, v450_d)), "typesJson")); wv t764 = rt_text(v453_kind); wv t765 = rt_text(rt_chk(w5_typeJson(rt_idx(v451_key, v452_types)), "typesJson")); rt_cat(13, (wv[]){ rt_s(&S438), t760, rt_s(&S439), t761, rt_s(&S440), t762, rt_s(&S441), t763, rt_s(&S442), t764, rt_s(&S443), t765, rt_s(&S15) }); }); std_push(t766, t767); });
    }
  }
  RT_AT(4, 368);
  wv v445_files = rt_arr(0, NULL, 1);
  RT_AT(4, 369);
  {
    wfor t769;
    rt_for_start(&t769, v442_modules, 0);
    for (int64_t t768 = 0; rt_for_guard(&t769, v442_modules, "modules"), rt_for_more(&t769, t768); t768++) {
      wv v458_m = rt_for_item(&t769, t768);
      RT_AT(4, 369);
      (void)({ wv t770 = v445_files; wv t771 = rt_chk(w5_jsonString(rt_get(&F_file, v458_m)), "typesJson"); std_push(t770, t771); });
    }
  }
  RT_RET((RT_AT(4, 370), ({ wv t772 = rt_text(std_join(v445_files, rt_s(&S419))); wv t773 = rt_text(std_join(v444_names, rt_s(&S446))); rt_cat(5, (wv[]){ rt_s(&S444), t772, rt_s(&S445), t773, rt_s(&S447) }); })));
}

static wv w5_endType(wv v459_b) {
  rt_enter("endType");
  v459_b = rt_bind(v459_b, 0);
  RT_AT(4, 376);
  wv v460_last = RT_NULL;
  RT_AT(4, 377);
  {
    wfor t775;
    rt_for_start(&t775, rt_get(&F_stmts, v459_b), 0);
    for (int64_t t774 = 0; rt_for_guard(&t775, rt_get(&F_stmts, v459_b), "b.stmts"), rt_for_more(&t775, t774); t774++) {
      wv v461_s = rt_for_item(&t775, t774);
      RT_AT(4, 378);
      if (rt_cond(rt_ne(rt_get(&F_k, v461_s), rt_s(&S203)))) {
        RT_AT(4, 378);
        v460_last = rt_bind(v461_s, 1);
      }
    }
  }
  RT_AT(4, 380);
  if (rt_cond(rt_b(rt_bool(rt_ne(v460_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t776 = rt_def(w5_neverCompletes, "neverCompletes"); wv t777 = rt_get(&F_k, v460_last); rt_chk(w1_contains(t776, t777), "endType"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v460_last), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v460_last)), rt_s(&S185)), "&&")), "||")), "&&")))) {
    RT_AT(4, 381);
    RT_RET(rt_def(w5_tNever, "tNever"));
  }
  RT_RET((RT_AT(4, 383), RT_NULL));
}

static wv w5_check(wv p_v462_modules, wv p_v463_requireExit) {
  rt_enter("check");
  struct fr119 fr = { 0 };
  fr.v462_modules = rt_bind(p_v462_modules, 0);
  fr.v463_requireExit = rt_bind(p_v463_requireExit, 0);
  RT_AT(4, 395);
  fr.v464_errors = rt_arr(0, NULL, 1);
  RT_AT(4, 396);
  fr.v465_nextFn = rt_i(1);
  RT_AT(4, 398);
  fr.v466_curMod = rt_i(0);
  RT_AT(4, 402);
  fr.v467_functions = rt_arr(0, NULL, 1);
  RT_AT(4, 403);
  fr.v468_uses = rt_arr(0, NULL, 1);
  RT_AT(4, 405);
  fr.v469_enums = rt_arr(0, NULL, 1);
  RT_AT(4, 409);
  fr.v470_bindingTypes = rt_map(0, NULL, 1);
  RT_AT(4, 410);
  fr.v471_fnTypes = rt_map(0, NULL, 1);
  RT_AT(4, 411);
  fr.v472_returnTypes = rt_map(0, NULL, 1);
  RT_AT(4, 413);
  fr.v473_rebound = rt_map(0, NULL, 1);
  RT_AT(4, 415);
  fr.v474_allDecls = rt_arr(0, NULL, 1);
  RT_AT(4, 1420);
  fr.v522_topFn = rt_rec(0, 2, K50, (wv[]){ rt_i(0), rt_b(1) });
  RT_AT(4, 1421);
  wv v523_entry = rt_sub(std_length(fr.v462_modules), rt_i(1));
  RT_AT(4, 1422);
  wv v524_mi = rt_i(0);
  RT_AT(4, 1423);
  while (rt_cond((RT_AT(4, 1423), ({ wv t1586 = v524_mi; wv t1587 = std_length(fr.v462_modules); rt_lt(t1586, t1587); })))) {
    RT_AT(4, 1424);
    (void)({ wv t1588 = v524_mi; wv t1589 = rt_eq(v524_mi, v523_entry); rt_chk(s521_checkModule(&fr, t1588, t1589), "check"); });
    RT_AT(4, 1425);
    v524_mi = rt_add(v524_mi, rt_i(1));
  }
  RT_AT(4, 1428);
  wv v525_si = rt_i(0);
  RT_AT(4, 1429);
  while (rt_cond((RT_AT(4, 1429), ({ wv t1590 = v525_si; wv t1591 = std_length(fr.v467_functions); rt_lt(t1590, t1591); })))) {
    RT_AT(4, 1430);
    wv v812_sc = rt_bind(rt_idx(v525_si, fr.v467_functions), 0);
    RT_AT(4, 1431);
    wv v813_use = rt_bind(({ wv t1592 = rt_get(&F_mod, v812_sc); wv t1593 = rt_get(&F_namePos, v812_sc); rt_chk(s488_firstUse(&fr, t1592, t1593), "check"); }), 0);
    RT_AT(4, 1432);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v813_use, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v812_sc)), RT_NULL), "&&")), "&&") && rt_bool(rt_not(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v812_sc))), "check")), "&&")))) {
      RT_AT(4, 1433);
      (void)rt_chk(s486_enter(&fr, rt_get(&F_mod, v812_sc)), "check");
      RT_AT(4, 1434);
      wv v814_um = rt_bind(rt_get(&F_useMod, v813_use), 0);
      RT_AT(4, 1435);
      wv v815_where = rt_bind((rt_cond(({ wv t1594 = v814_um; wv t1595 = rt_get(&F_mod, v812_sc); rt_eq(t1594, t1595); })) ? rt_cat(2, (wv[]){ rt_s(&S579), rt_text(rt_chk(w5_lineOf(rt_get(&F_pos, v813_use)), "check")) }) : ({ wv t1598 = rt_text(rt_get(&F_file, rt_idx(v814_um, fr.v462_modules))); wv t1599 = rt_text(({ wv t1596 = rt_get(&F_chars, rt_idx(v814_um, fr.v462_modules)); wv t1597 = rt_get(&F_pos, v813_use); rt_chk(w5_lineIn(t1596, t1597), "check"); })); rt_cat(3, (wv[]){ t1598, rt_s(&S580), t1599 }); })), 0);
      RT_AT(4, 1436);
      (void)({ wv t1602 = rt_get(&F_namePos, v812_sc); wv t1603 = ({ wv t1600 = rt_text(rt_get(&F_name, v812_sc)); wv t1601 = rt_text(v815_where); rt_cat(5, (wv[]){ rt_s(&S114), t1600, rt_s(&S581), t1601, rt_s(&S16) }); }); rt_chk(s475_error(&fr, t1602, t1603, rt_s(&S582)), "check"); });
    }
    RT_AT(4, 1439);
    v525_si = rt_add(v525_si, rt_i(1));
  }
  RT_AT(4, 1441);
  (void)rt_chk(s486_enter(&fr, v523_entry), "check");
  RT_AT(4, 1442);
  wv v526_program = rt_bind(rt_get(&F_program, rt_idx(v523_entry, fr.v462_modules)), 0);
  RT_AT(4, 1443);
  wv v527_last = RT_NULL;
  RT_AT(4, 1444);
  wv v528_k = rt_i(0);
  RT_AT(4, 1445);
  while (rt_cond((RT_AT(4, 1445), ({ wv t1604 = v528_k; wv t1605 = std_length(rt_get(&F_stmts, v526_program)); rt_lt(t1604, t1605); })))) {
    RT_AT(4, 1446);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1606 = v528_k; wv t1607 = rt_get(&F_stmts, v526_program); rt_idx(t1606, t1607); })), rt_s(&S203)))) {
      RT_AT(4, 1446);
      v527_last = rt_bind(({ wv t1608 = v528_k; wv t1609 = rt_get(&F_stmts, v526_program); rt_idx(t1608, t1609); }), 1);
    }
    RT_AT(4, 1447);
    v528_k = rt_add(v528_k, rt_i(1));
  }
  RT_AT(4, 1450);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v527_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v527_last), rt_s(&S55)), "||")), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(fr.v463_requireExit, "||") || rt_bool(rt_eq(v527_last, RT_NULL), "||")), "||") || rt_bool(rt_ne(rt_get(&F_k, v527_last), rt_s(&S206)), "||")), "&&")))) {
    RT_AT(4, 1451);
    (void)rt_chk(s475_error(&fr, rt_get(&F_end, v526_program), rt_s(&S583), rt_s(&S584)), "check");
  }
  RT_AT(4, 1453);
  wv t1610 = rt_bind(fr.v474_allDecls, 1);
  (void)rt_def(w5_lastDecls, "lastDecls");
  w5_lastDecls = t1610;
  RT_AT(4, 1454);
  wv t1611 = rt_bind(fr.v470_bindingTypes, 1);
  (void)rt_def(w5_lastBindingTypes, "lastBindingTypes");
  w5_lastBindingTypes = t1611;
  RT_AT(4, 1455);
  wv t1612 = rt_bind(fr.v471_fnTypes, 1);
  (void)rt_def(w5_lastFnTypes, "lastFnTypes");
  w5_lastFnTypes = t1612;
  RT_RET((RT_AT(4, 1456), fr.v464_errors));
}

static wv s475_error(struct fr119 *up, wv v529_pos, wv v530_message, wv v531_hint) {
  rt_enter("error");
  v529_pos = rt_bind(v529_pos, 0);
  v530_message = rt_bind(v530_message, 0);
  v531_hint = rt_bind(v531_hint, 0);
  RT_AT(4, 417);
  (void)({ wv t778 = rt_def(up->v464_errors, "errors"); wv t779 = rt_chk(w2_err(v529_pos, v530_message, v531_hint), "error"); std_push(t778, t779); });
  RT_RET(RT_NULL);
}

static wv s476_typeOfDecl(struct fr119 *up, wv v532_d) {
  rt_enter("typeOfDecl");
  v532_d = rt_bind(v532_d, 0);
  RT_AT(4, 421);
  wv v533_key = ({ wv t780 = rt_text(rt_get(&F_mod, v532_d)); wv t781 = rt_text(rt_get(&F_namePos, v532_d)); rt_cat(3, (wv[]){ t780, rt_s(&S28), t781 }); });
  RT_AT(4, 422);
  if (rt_cond(({ wv t782 = rt_def(up->v470_bindingTypes, "bindingTypes"); wv t783 = v533_key; std_has(t782, t783); }))) {
    RT_AT(4, 422);
    RT_RET(({ wv t784 = v533_key; wv t785 = rt_def(up->v470_bindingTypes, "bindingTypes"); rt_idx(t784, t785); }));
  }
  RT_RET((RT_AT(4, 423), RT_NULL));
}

static wv s477_bindType(struct fr119 *up, wv v534_pos, wv v535_ty) {
  rt_enter("bindType");
  v534_pos = rt_bind(v534_pos, 0);
  v535_ty = rt_bind(v535_ty, 0);
  RT_AT(4, 427);
  if (rt_cond(rt_ne(v535_ty, RT_NULL))) {
    RT_AT(4, 427);
    ({ wv t788 = v535_ty; wv t789 = ({ wv t786 = rt_text(rt_def(up->v466_curMod, "curMod")); wv t787 = rt_text(v534_pos); rt_cat(3, (wv[]){ t786, rt_s(&S28), t787 }); }); wv t790 = rt_def(up->v470_bindingTypes, "bindingTypes"); rt_set_idx(t788, t789, t790); });
  }
  RT_RET(RT_NULL);
}

static wv s478_enumType(struct fr119 *up, wv v536_d) {
  rt_enter("enumType");
  v536_d = rt_bind(v536_d, 0);
  RT_RET((RT_AT(4, 431), ({ wv t793 = rt_get(&F_name, v536_d); wv t794 = ({ wv t791 = rt_text(rt_get(&F_mod, v536_d)); wv t792 = rt_text(rt_get(&F_namePos, v536_d)); rt_cat(3, (wv[]){ t791, rt_s(&S28), t792 }); }); rt_rec(0, 3, K45, (wv[]){ rt_s(&S47), t793, t794 }); })));
}

static wv s479_noText(struct fr119 *up, wv v537_x, wv v538_ty) {
  rt_enter("noText");
  v537_x = rt_bind(v537_x, 0);
  v538_ty = rt_bind(v538_ty, 0);
  RT_AT(4, 435);
  wv v539_hint = rt_bind(rt_chk(w5_noTextHint(v538_ty), "noText"), 0);
  RT_AT(4, 436);
  if (rt_cond(rt_ne(v539_hint, RT_NULL))) {
    RT_AT(4, 436);
    (void)({ wv t795 = rt_get(&F_pos, v537_x); wv t796 = rt_cat(3, (wv[]){ rt_s(&S448), rt_text(rt_chk(w5_typeText(v538_ty), "noText")), rt_s(&S449) }); wv t797 = v539_hint; rt_chk(s475_error(up, t795, t796, t797), "noText"); });
  }
  RT_RET(RT_NULL);
}

static wv s480_condition(struct fr119 *up, wv v540_x, wv v541_ty) {
  rt_enter("condition");
  v540_x = rt_bind(v540_x, 0);
  v541_ty = rt_bind(v541_ty, 0);
  RT_AT(4, 441);
  if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v541_ty), "condition"), "&&") && rt_bool(rt_ne(rt_get(&F_t, v541_ty), rt_s(&S411)), "&&")))) {
    RT_AT(4, 441);
    (void)({ wv t798 = rt_get(&F_pos, v540_x); wv t799 = rt_cat(2, (wv[]){ rt_s(&S450), rt_text(rt_chk(w5_typeText(v541_ty), "condition")) }); rt_chk(s475_error(up, t798, t799, RT_NULL), "condition"); });
  }
  RT_RET(RT_NULL);
}

static wv s481_needBool(struct fr119 *up, wv v542_op, wv v543_x, wv v544_ty) {
  rt_enter("needBool");
  v542_op = rt_bind(v542_op, 0);
  v543_x = rt_bind(v543_x, 0);
  v544_ty = rt_bind(v544_ty, 0);
  RT_AT(4, 444);
  if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v544_ty), "needBool"), "&&") && rt_bool(rt_ne(rt_get(&F_t, v544_ty), rt_s(&S411)), "&&")))) {
    RT_AT(4, 444);
    (void)({ wv t802 = rt_get(&F_pos, v543_x); wv t803 = ({ wv t800 = rt_text(v542_op); wv t801 = rt_text(rt_chk(w5_typeText(v544_ty), "needBool")); rt_cat(4, (wv[]){ rt_s(&S114), t800, rt_s(&S451), t801 }); }); rt_chk(s475_error(up, t802, t803, RT_NULL), "needBool"); });
  }
  RT_RET(RT_NULL);
}

static wv s482_needInt(struct fr119 *up, wv v545_op, wv v546_x, wv v547_ty) {
  rt_enter("needInt");
  v545_op = rt_bind(v545_op, 0);
  v546_x = rt_bind(v546_x, 0);
  v547_ty = rt_bind(v547_ty, 0);
  RT_AT(4, 447);
  if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v547_ty), "needInt"), "&&") && rt_bool(rt_ne(rt_get(&F_t, v547_ty), rt_s(&S139)), "&&")))) {
    RT_AT(4, 447);
    (void)({ wv t806 = rt_get(&F_pos, v546_x); wv t807 = ({ wv t804 = rt_text(v545_op); wv t805 = rt_text(rt_chk(w5_typeText(v547_ty), "needInt")); rt_cat(4, (wv[]){ rt_s(&S114), t804, rt_s(&S452), t805 }); }); rt_chk(s475_error(up, t806, t807, RT_NULL), "needInt"); });
  }
  RT_RET(RT_NULL);
}

static wv s483_fieldOf(struct fr119 *up, wv v548_rt, wv v549_name, wv v550_pos, wv v551_verb) {
  rt_enter("fieldOf");
  v548_rt = rt_bind(v548_rt, 0);
  v549_name = rt_bind(v549_name, 0);
  v550_pos = rt_bind(v550_pos, 0);
  v551_verb = rt_bind(v551_verb, 0);
  RT_AT(4, 453);
  if (rt_cond(rt_not(rt_chk(w5_known(v548_rt), "fieldOf")))) {
    RT_AT(4, 453);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 454);
  if (rt_cond(rt_ne(rt_get(&F_t, v548_rt), rt_s(&S414)))) {
    RT_AT(4, 455);
    (void)({ wv t811 = v550_pos; wv t812 = ({ wv t808 = rt_text(v551_verb); wv t809 = rt_text(v549_name); wv t810 = rt_text(rt_chk(w5_typeText(v548_rt), "fieldOf")); rt_cat(5, (wv[]){ t808, rt_s(&S453), t809, rt_s(&S454), t810 }); }); rt_chk(s475_error(up, t811, t812, RT_NULL), "fieldOf"); });
    RT_AT(4, 456);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 458);
  {
    wfor t814;
    rt_for_start(&t814, rt_get(&F_names, v548_rt), 0);
    for (int64_t t813 = 0; rt_for_guard(&t814, rt_get(&F_names, v548_rt), "rt.names"), rt_for_more(&t814, t813); t813++) {
      wv v552_i = rt_for_key(&t814, t813);
      wv v553_n = rt_for_val(&t814, t813);
      RT_AT(4, 459);
      if (rt_cond(rt_eq(v553_n, v549_name))) {
        RT_AT(4, 459);
        RT_RET((rt_cond(rt_eq(rt_get(&F_types, v548_rt), RT_NULL)) ? RT_NULL : ({ wv t815 = v552_i; wv t816 = rt_get(&F_types, v548_rt); rt_idx(t815, t816); })));
      }
    }
  }
  RT_AT(4, 461);
  (void)({ wv t819 = v550_pos; wv t820 = ({ wv t817 = rt_text(v549_name); wv t818 = rt_text(rt_chk(w5_typeText(v548_rt), "fieldOf")); rt_cat(4, (wv[]){ rt_s(&S455), t817, rt_s(&S456), t818 }); }); rt_chk(s475_error(up, t819, t820, RT_NULL), "fieldOf"); });
  RT_RET((RT_AT(4, 462), RT_NULL));
}

static wv s484_indexed(struct fr119 *up, wv v554_at, wv v555_index, wv v556_it, wv v557_pos, wv v558_verb) {
  rt_enter("indexed");
  v554_at = rt_bind(v554_at, 0);
  v555_index = rt_bind(v555_index, 0);
  v556_it = rt_bind(v556_it, 0);
  v557_pos = rt_bind(v557_pos, 0);
  v558_verb = rt_bind(v558_verb, 0);
  RT_AT(4, 468);
  if (rt_cond(rt_not(rt_chk(w5_known(v554_at), "indexed")))) {
    RT_AT(4, 468);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 469);
  if (rt_cond(rt_eq(rt_get(&F_t, v554_at), rt_s(&S412)))) {
    RT_AT(4, 470);
    if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v556_it), "indexed"), "&&") && rt_bool(rt_ne(rt_get(&F_t, v556_it), rt_s(&S139)), "&&")))) {
      RT_AT(4, 471);
      (void)({ wv t821 = rt_get(&F_pos, v555_index); wv t822 = rt_cat(2, (wv[]){ rt_s(&S457), rt_text(rt_chk(w5_typeText(v556_it), "indexed")) }); rt_chk(s475_error(up, t821, t822, RT_NULL), "indexed"); });
      RT_AT(4, 472);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 474);
    RT_RET(rt_get(&F_elem, v554_at));
  }
  RT_AT(4, 476);
  if (rt_cond(rt_eq(rt_get(&F_t, v554_at), rt_s(&S189)))) {
    RT_AT(4, 477);
    if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v556_it), "indexed"), "&&") && rt_bool(({ wv t823 = rt_arr(4, (wv[]){ rt_s(&S149), rt_s(&S52), rt_s(&S412), rt_s(&S189) }, 0); wv t824 = rt_get(&F_t, v556_it); rt_chk(w1_contains(t823, t824), "indexed"); }), "&&")))) {
      RT_AT(4, 478);
      (void)({ wv t825 = rt_get(&F_pos, v555_index); wv t826 = rt_cat(2, (wv[]){ rt_s(&S458), rt_text(rt_chk(w5_typeText(v556_it), "indexed")) }); rt_chk(s475_error(up, t825, t826, RT_NULL), "indexed"); });
    }
    RT_AT(4, 480);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 482);
  if (rt_cond(rt_eq(rt_get(&F_t, v554_at), rt_s(&S414)))) {
    RT_AT(4, 483);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v555_index), rt_s(&S173)), "&&") && rt_bool(rt_eq(std_length(rt_get(&F_parts, v555_index)), rt_i(1)), "&&")))) {
      RT_AT(4, 483);
      RT_RET(({ wv t827 = v554_at; wv t828 = rt_idx(rt_i(0), rt_get(&F_parts, v555_index)); wv t829 = v557_pos; wv t830 = v558_verb; rt_chk(s483_fieldOf(up, t827, t828, t829, t830), "indexed"); }));
    }
    RT_AT(4, 484);
    if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v556_it), "indexed"), "&&") && rt_bool(rt_ne(rt_get(&F_t, v556_it), rt_s(&S187)), "&&")))) {
      RT_AT(4, 485);
      (void)({ wv t833 = rt_get(&F_pos, v555_index); wv t834 = ({ wv t831 = rt_text(rt_chk(w5_typeText(v556_it), "indexed")); wv t832 = rt_text(rt_chk(w5_typeText(v554_at), "indexed")); rt_cat(4, (wv[]){ rt_s(&S459), t831, rt_s(&S460), t832 }); }); rt_chk(s475_error(up, t833, t834, rt_s(&S461)), "indexed"); });
    }
    RT_AT(4, 488);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 490);
  wv v559_hint = rt_bind((rt_cond(rt_eq(rt_get(&F_t, v554_at), rt_s(&S187))) ? rt_s(&S462) : RT_NULL), 0);
  RT_AT(4, 491);
  (void)({ wv t835 = v557_pos; wv t836 = rt_cat(2, (wv[]){ rt_s(&S463), rt_text(rt_chk(w5_typeText(v554_at), "indexed")) }); wv t837 = v559_hint; rt_chk(s475_error(up, t835, t836, t837), "indexed"); });
  RT_RET((RT_AT(4, 492), RT_NULL));
}

static wv s485_binary(struct fr119 *up, wv v560_e, wv v561_lt, wv v562_rt) {
  rt_enter("binary");
  v560_e = rt_bind(v560_e, 0);
  v561_lt = rt_bind(v561_lt, 0);
  v562_rt = rt_bind(v562_rt, 0);
  RT_AT(4, 497);
  wv v563_op = rt_bind(rt_get(&F_op, v560_e), 0);
  RT_AT(4, 498);
  if (rt_cond(rt_b(rt_bool(rt_eq(v563_op, rt_s(&S81)), "||") || rt_bool(rt_eq(v563_op, rt_s(&S82)), "||")))) {
    RT_AT(4, 499);
    (void)({ wv t838 = v563_op; wv t839 = rt_get(&F_l, v560_e); wv t840 = v561_lt; rt_chk(s481_needBool(up, t838, t839, t840), "binary"); });
    RT_AT(4, 500);
    (void)({ wv t841 = v563_op; wv t842 = rt_get(&F_r, v560_e); wv t843 = v562_rt; rt_chk(s481_needBool(up, t841, t842, t843), "binary"); });
    RT_AT(4, 501);
    RT_RET(rt_def(w5_tBool, "tBool"));
  }
  RT_AT(4, 503);
  if (rt_cond(rt_b(rt_bool(rt_eq(v563_op, rt_s(&S77)), "||") || rt_bool(rt_eq(v563_op, rt_s(&S78)), "||")))) {
    RT_AT(4, 503);
    RT_RET(rt_def(w5_tBool, "tBool"));
  }
  RT_AT(4, 504);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v563_op, rt_s(&S90)), "||") || rt_bool(rt_eq(v563_op, rt_s(&S91)), "||")), "||") || rt_bool(rt_eq(v563_op, rt_s(&S79)), "||")), "||") || rt_bool(rt_eq(v563_op, rt_s(&S80)), "||")))) {
    RT_AT(4, 506);
    wv v567_numeric = rt_arr(2, (wv[]){ rt_s(&S139), rt_s(&S149) }, 0);
    RT_AT(4, 507);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&")), "&&") && rt_bool(({ wv t844 = rt_get(&F_t, v561_lt); wv t845 = rt_get(&F_t, v562_rt); rt_eq(t844, t845); }), "&&")), "&&") && rt_bool(rt_not(({ wv t846 = rt_arr(3, (wv[]){ rt_s(&S139), rt_s(&S149), rt_s(&S187) }, 0); wv t847 = rt_get(&F_t, v561_lt); rt_chk(w1_contains(t846, t847), "binary"); })), "&&")))) {
      RT_AT(4, 508);
      wv v568_what = rt_bind((rt_cond(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S47))) ? rt_s(&S464) : rt_cat(2, (wv[]){ rt_text(rt_get(&F_t, v561_lt)), rt_s(&S465) })), 0);
      RT_AT(4, 509);
      (void)({ wv t850 = rt_get(&F_pos, v560_e); wv t851 = ({ wv t848 = rt_text(v563_op); wv t849 = rt_text(v568_what); rt_cat(4, (wv[]){ rt_s(&S114), t848, rt_s(&S466), t849 }); }); rt_chk(s475_error(up, t850, t851, RT_NULL), "binary"); });
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&")), "&&") && rt_bool(({ wv t852 = rt_get(&F_t, v561_lt); wv t853 = rt_get(&F_t, v562_rt); rt_ne(t852, t853); }), "&&")), "&&") && rt_bool(rt_not(rt_b(rt_bool(({ wv t854 = v567_numeric; wv t855 = rt_get(&F_t, v561_lt); rt_chk(w1_contains(t854, t855), "binary"); }), "&&") && rt_bool(({ wv t856 = v567_numeric; wv t857 = rt_get(&F_t, v562_rt); rt_chk(w1_contains(t856, t857), "binary"); }), "&&"))), "&&")))) {
      RT_AT(4, 511);
      (void)({ wv t861 = rt_get(&F_pos, v560_e); wv t862 = ({ wv t858 = rt_text(v563_op); wv t859 = rt_text(rt_chk(w5_typeText(v561_lt), "binary")); wv t860 = rt_text(rt_chk(w5_typeText(v562_rt), "binary")); rt_cat(6, (wv[]){ rt_s(&S114), t858, rt_s(&S466), t859, rt_s(&S467), t860 }); }); rt_chk(s475_error(up, t861, t862, RT_NULL), "binary"); });
    }
    RT_AT(4, 513);
    RT_RET(rt_def(w5_tBool, "tBool"));
  }
  RT_AT(4, 515);
  if (rt_cond(({ wv t863 = rt_arr(5, (wv[]){ rt_s(&S92), rt_s(&S93), rt_s(&S32), rt_s(&S83), rt_s(&S84) }, 0); wv t864 = v563_op; rt_chk(w1_contains(t863, t864), "binary"); }))) {
    RT_AT(4, 516);
    (void)({ wv t865 = v563_op; wv t866 = rt_get(&F_l, v560_e); wv t867 = v561_lt; rt_chk(s482_needInt(up, t865, t866, t867), "binary"); });
    RT_AT(4, 517);
    (void)({ wv t868 = v563_op; wv t869 = rt_get(&F_r, v560_e); wv t870 = v562_rt; rt_chk(s482_needInt(up, t868, t869, t870), "binary"); });
    RT_AT(4, 518);
    RT_RET(rt_def(w5_tInt, "tInt"));
  }
  RT_AT(4, 521);
  wv v564_both = rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&"));
  RT_AT(4, 522);
  wv v565_shown = rt_bind((rt_cond(v564_both) ? ({ wv t871 = rt_text(rt_chk(w5_typeText(v561_lt), "binary")); wv t872 = rt_text(rt_chk(w5_typeText(v562_rt), "binary")); rt_cat(3, (wv[]){ t871, rt_s(&S467), t872 }); }) : (rt_cond(rt_chk(w5_known(v561_lt), "binary")) ? rt_chk(w5_typeText(v561_lt), "binary") : (rt_cond(rt_chk(w5_known(v562_rt), "binary")) ? rt_chk(w5_typeText(v562_rt), "binary") : rt_s(&S13)))), 0);
  RT_AT(4, 523);
  if (rt_cond(rt_b(rt_bool(rt_eq(v563_op, rt_s(&S85)), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S187)), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v562_rt), rt_s(&S187)), "&&")), "||")), "&&")))) {
    RT_AT(4, 524);
    if (rt_cond(rt_b(rt_bool(v564_both, "&&") && rt_bool(({ wv t873 = rt_get(&F_t, v561_lt); wv t874 = rt_get(&F_t, v562_rt); rt_eq(t873, t874); }), "&&")))) {
      RT_AT(4, 524);
      RT_RET(rt_def(w5_tString, "tString"));
    }
    RT_AT(4, 525);
    if (rt_cond(v564_both)) {
      RT_AT(4, 525);
      (void)({ wv t875 = rt_get(&F_pos, v560_e); wv t876 = rt_cat(2, (wv[]){ rt_s(&S468), rt_text(v565_shown) }); rt_chk(s475_error(up, t875, t876, rt_s(&S469)), "binary"); });
    }
    RT_AT(4, 526);
    RT_RET((rt_cond(v564_both) ? RT_NULL : rt_def(w5_tString, "tString")));
  }
  RT_AT(4, 528);
  wv v566_nums = rt_arr(2, (wv[]){ rt_s(&S139), rt_s(&S149) }, 0);
  RT_AT(4, 529);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_not(({ wv t877 = v566_nums; wv t878 = rt_get(&F_t, v561_lt); rt_chk(w1_contains(t877, t878), "binary"); })), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&") && rt_bool(rt_not(({ wv t879 = v566_nums; wv t880 = rt_get(&F_t, v562_rt); rt_chk(w1_contains(t879, t880), "binary"); })), "&&")), "||")))) {
    RT_AT(4, 530);
    (void)({ wv t883 = rt_get(&F_pos, v560_e); wv t884 = ({ wv t881 = rt_text(v563_op); wv t882 = rt_text(v565_shown); rt_cat(4, (wv[]){ rt_s(&S114), t881, rt_s(&S470), t882 }); }); rt_chk(s475_error(up, t883, t884, RT_NULL), "binary"); });
    RT_AT(4, 531);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 533);
  if (rt_cond(rt_eq(v563_op, rt_s(&S88)))) {
    RT_AT(4, 533);
    RT_RET(rt_def(w5_tFloat, "tFloat"));
  }
  RT_AT(4, 534);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(v564_both, "&&") && rt_bool(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S139)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, v562_rt), rt_s(&S139)), "&&")))) {
    RT_AT(4, 534);
    RT_RET((rt_cond(rt_eq(v563_op, rt_s(&S76))) ? RT_NULL : rt_def(w5_tInt, "tInt")));
  }
  RT_AT(4, 535);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S149)), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v562_rt), rt_s(&S149)), "&&")), "||")))) {
    RT_AT(4, 535);
    RT_RET(rt_def(w5_tFloat, "tFloat"));
  }
  RT_RET((RT_AT(4, 536), RT_NULL));
}

static wv s486_enter(struct fr119 *up, wv v569_m) {
  rt_enter("enter");
  v569_m = rt_bind(v569_m, 0);
  RT_AT(4, 541);
  wv t885 = rt_bind(v569_m, 1);
  (void)rt_def(up->v466_curMod, "curMod");
  up->v466_curMod = t885;
  RT_AT(4, 542);
  (void)({ wv t886 = rt_get(&F_file, rt_idx(v569_m, up->v462_modules)); wv t887 = rt_get(&F_chars, rt_idx(v569_m, up->v462_modules)); rt_chk(w2_setSource(t886, t887), "enter"); });
  RT_RET(RT_NULL);
}

static wv s487_enumMembers(struct fr119 *up, wv v570_d) {
  rt_enter("enumMembers");
  v570_d = rt_bind(v570_d, 0);
  RT_AT(4, 547);
  wv v571_k = rt_i(0);
  RT_AT(4, 548);
  while (rt_cond((RT_AT(4, 548), ({ wv t888 = v571_k; wv t889 = std_length(rt_def(up->v469_enums, "enums")); rt_lt(t888, t889); })))) {
    RT_AT(4, 549);
    if (rt_cond(rt_b(rt_bool(({ wv t892 = rt_get(&F_mod, ({ wv t890 = v571_k; wv t891 = rt_def(up->v469_enums, "enums"); rt_idx(t890, t891); })); wv t893 = rt_get(&F_mod, v570_d); rt_eq(t892, t893); }), "&&") && rt_bool(({ wv t896 = rt_get(&F_name, ({ wv t894 = v571_k; wv t895 = rt_def(up->v469_enums, "enums"); rt_idx(t894, t895); })); wv t897 = rt_get(&F_name, v570_d); rt_eq(t896, t897); }), "&&")))) {
      RT_AT(4, 549);
      RT_RET(rt_get(&F_members, ({ wv t898 = v571_k; wv t899 = rt_def(up->v469_enums, "enums"); rt_idx(t898, t899); })));
    }
    RT_AT(4, 550);
    v571_k = rt_add(v571_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 552), RT_NULL));
}

static wv s488_firstUse(struct fr119 *up, wv v572_mod, wv v573_declPos) {
  rt_enter("firstUse");
  v572_mod = rt_bind(v572_mod, 0);
  v573_declPos = rt_bind(v573_declPos, 0);
  RT_AT(4, 557);
  wv v574_k = rt_i(0);
  RT_AT(4, 558);
  while (rt_cond((RT_AT(4, 558), ({ wv t900 = v574_k; wv t901 = std_length(rt_def(up->v468_uses, "uses")); rt_lt(t900, t901); })))) {
    RT_AT(4, 559);
    if (rt_cond(rt_b(rt_bool(({ wv t904 = rt_get(&F_mod, ({ wv t902 = v574_k; wv t903 = rt_def(up->v468_uses, "uses"); rt_idx(t902, t903); })); wv t905 = v572_mod; rt_eq(t904, t905); }), "&&") && rt_bool(({ wv t908 = rt_get(&F_namePos, ({ wv t906 = v574_k; wv t907 = rt_def(up->v468_uses, "uses"); rt_idx(t906, t907); })); wv t909 = v573_declPos; rt_eq(t908, t909); }), "&&")))) {
      RT_AT(4, 559);
      RT_RET(({ wv t910 = v574_k; wv t911 = rt_def(up->v468_uses, "uses"); rt_idx(t910, t911); }));
    }
    RT_AT(4, 560);
    v574_k = rt_add(v574_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 562), RT_NULL));
}

static wv s489_requireValue(struct fr119 *up, wv v575_b) {
  rt_enter("requireValue");
  v575_b = rt_bind(v575_b, 0);
  RT_AT(4, 567);
  if (rt_cond(rt_ne(rt_get(&F_value, v575_b), RT_NULL))) {
    RT_AT(4, 567);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 568);
  wv v576_last = RT_NULL;
  RT_AT(4, 569);
  wv v577_k = rt_i(0);
  RT_AT(4, 570);
  while (rt_cond((RT_AT(4, 570), ({ wv t912 = v577_k; wv t913 = std_length(rt_get(&F_stmts, v575_b)); rt_lt(t912, t913); })))) {
    RT_AT(4, 571);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t914 = v577_k; wv t915 = rt_get(&F_stmts, v575_b); rt_idx(t914, t915); })), rt_s(&S203)))) {
      RT_AT(4, 571);
      v576_last = rt_bind(({ wv t916 = v577_k; wv t917 = rt_get(&F_stmts, v575_b); rt_idx(t916, t917); }), 1);
    }
    RT_AT(4, 572);
    v577_k = rt_add(v577_k, rt_i(1));
  }
  RT_AT(4, 574);
  if (rt_cond(rt_b(rt_bool(rt_ne(v576_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t918 = rt_def(w5_neverCompletes, "neverCompletes"); wv t919 = rt_get(&F_k, v576_last); rt_chk(w1_contains(t918, t919), "requireValue"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v576_last), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v576_last)), rt_s(&S185)), "&&")), "||")), "&&")))) {
    RT_AT(4, 575);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 577);
  (void)rt_chk(s475_error(up, rt_get(&F_close, v575_b), rt_s(&S471), rt_s(&S472)), "requireValue");
  RT_RET(RT_NULL);
}

static wv s490_newScope(struct fr119 *up, wv v578_parent) {
  rt_enter("newScope");
  v578_parent = rt_bind(v578_parent, 0);
  RT_AT(4, 582);
  wv v579_names = rt_arr(0, NULL, 1);
  RT_RET((RT_AT(4, 583), rt_rec(0, 2, K46, (wv[]){ v578_parent, v579_names })));
}

static wv s491_decl(struct fr119 *up, wv v580_kind, wv v581_name, wv v582_mode, wv v583_namePos, wv v584_visibleAt, wv v585_fnId, wv v586_arity) {
  rt_enter("decl");
  v580_kind = rt_bind(v580_kind, 0);
  v581_name = rt_bind(v581_name, 0);
  v582_mode = rt_bind(v582_mode, 0);
  v583_namePos = rt_bind(v583_namePos, 0);
  v584_visibleAt = rt_bind(v584_visibleAt, 0);
  v585_fnId = rt_bind(v585_fnId, 0);
  v586_arity = rt_bind(v586_arity, 0);
  RT_RET((RT_AT(4, 592), ({ wv t920 = v580_kind; wv t921 = v581_name; wv t922 = rt_eq(v582_mode, rt_s(&S36)); wv t923 = rt_eq(v582_mode, rt_s(&S37)); wv t924 = v583_namePos; wv t925 = v584_visibleAt; wv t926 = v585_fnId; wv t927 = v586_arity; wv t928 = rt_def(up->v466_curMod, "curMod"); rt_rec(0, 10, K47, (wv[]){ t920, t921, t922, t923, t924, t925, t926, t927, t928, RT_NULL }); })));
}

static wv s492_own(struct fr119 *up, wv v587_scope, wv v588_name) {
  rt_enter("own");
  v588_name = rt_bind(v588_name, 0);
  RT_AT(4, 600);
  wv v589_k = rt_i(0);
  RT_AT(4, 601);
  while (rt_cond((RT_AT(4, 601), ({ wv t929 = v589_k; wv t930 = std_length(rt_get(&F_names, v587_scope)); rt_lt(t929, t930); })))) {
    RT_AT(4, 602);
    if (rt_cond(({ wv t933 = rt_get(&F_name, ({ wv t931 = v589_k; wv t932 = rt_get(&F_names, v587_scope); rt_idx(t931, t932); })); wv t934 = v588_name; rt_eq(t933, t934); }))) {
      RT_AT(4, 602);
      RT_RET(({ wv t935 = v589_k; wv t936 = rt_get(&F_names, v587_scope); rt_idx(t935, t936); }));
    }
    RT_AT(4, 603);
    v589_k = rt_add(v589_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 605), RT_NULL));
}

static wv s493_declare(struct fr119 *up, wv v590_scope, wv v591_name, wv v592_d) {
  rt_enter("declare");
  v591_name = rt_bind(v591_name, 0);
  v592_d = rt_bind(v592_d, 0);
  RT_AT(4, 612);
  wv v593_dup = rt_bind(rt_chk(s492_own(up, v590_scope, v591_name), "declare"), 0);
  RT_AT(4, 613);
  if (rt_cond(rt_ne(v593_dup, RT_NULL))) {
    RT_AT(4, 614);
    (void)({ wv t939 = rt_get(&F_namePos, v592_d); wv t940 = ({ wv t937 = rt_text(v591_name); wv t938 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v593_dup)), "declare")); rt_cat(5, (wv[]){ rt_s(&S114), t937, rt_s(&S473), t938, rt_s(&S16) }); }); rt_chk(s475_error(up, t939, t940, RT_NULL), "declare"); });
    RT_AT(4, 615);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 617);
  wv v594_s = rt_get(&F_parent, v590_scope);
  RT_AT(4, 618);
  while (rt_cond((RT_AT(4, 618), rt_ne(v594_s, RT_NULL)))) {
    RT_AT(4, 619);
    wv v595_outer = rt_bind(rt_chk(s492_own(up, v594_s, v591_name), "declare"), 0);
    RT_AT(4, 620);
    if (rt_cond(rt_b(rt_bool(rt_ne(v595_outer, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_kind, v595_outer), rt_s(&S179)), "||") || rt_bool(({ wv t941 = rt_get(&F_namePos, v595_outer); wv t942 = rt_get(&F_namePos, v592_d); rt_lt(t941, t942); }), "||")), "&&")))) {
      RT_AT(4, 621);
      (void)({ wv t946 = rt_get(&F_namePos, v592_d); wv t947 = ({ wv t943 = rt_text(v591_name); wv t944 = rt_text(v591_name); wv t945 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v595_outer)), "declare")); rt_cat(7, (wv[]){ rt_s(&S114), t943, rt_s(&S474), t944, rt_s(&S475), t945, rt_s(&S16) }); }); rt_chk(s475_error(up, t946, t947, rt_s(&S476)), "declare"); });
      break;
    }
    RT_AT(4, 625);
    v594_s = rt_get(&F_parent, v594_s);
  }
  RT_AT(4, 627);
  (void)({ wv t948 = rt_get(&F_names, v590_scope); wv t949 = v592_d; std_push(t948, t949); });
  RT_AT(4, 628);
  (void)({ wv t950 = rt_def(up->v474_allDecls, "allDecls"); wv t951 = v592_d; std_push(t950, t951); });
  RT_RET(RT_NULL);
}

static wv s494_lookup(struct fr119 *up, wv v596_scope, wv v597_name) {
  rt_enter("lookup");
  v597_name = rt_bind(v597_name, 0);
  RT_AT(4, 632);
  wv v598_s = v596_scope;
  RT_AT(4, 633);
  while (rt_cond((RT_AT(4, 633), rt_ne(v598_s, RT_NULL)))) {
    RT_AT(4, 634);
    wv v599_d = rt_bind(rt_chk(s492_own(up, v598_s, v597_name), "lookup"), 0);
    RT_AT(4, 635);
    if (rt_cond(rt_ne(v599_d, RT_NULL))) {
      RT_AT(4, 635);
      RT_RET(v599_d);
    }
    RT_AT(4, 636);
    v598_s = rt_get(&F_parent, v598_s);
  }
  RT_RET((RT_AT(4, 638), RT_NULL));
}

static wv s495_resolve(struct fr119 *up, wv v600_name, wv v601_pos, wv v602_scope, wv v603_ctx) {
  rt_enter("resolve");
  v600_name = rt_bind(v600_name, 0);
  v601_pos = rt_bind(v601_pos, 0);
  v602_scope = rt_bind(v602_scope, 0);
  v603_ctx = rt_bind(v603_ctx, 0);
  RT_AT(4, 642);
  wv v604_d = rt_bind(rt_chk(s494_lookup(up, v602_scope, v600_name), "resolve"), 0);
  RT_AT(4, 643);
  if (rt_cond(rt_eq(v604_d, RT_NULL))) {
    RT_AT(4, 644);
    (void)({ wv t952 = v601_pos; wv t953 = rt_cat(3, (wv[]){ rt_s(&S477), rt_text(v600_name), rt_s(&S114) }); rt_chk(s475_error(up, t952, t953, RT_NULL), "resolve"); });
    RT_AT(4, 645);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 649);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v604_d), rt_s(&S179)), "&&") && rt_bool(({ wv t954 = rt_get(&F_fnId, v604_d); wv t955 = rt_get(&F_id, rt_get(&F_func, v603_ctx)); rt_eq(t954, t955); }), "&&")), "&&") && rt_bool(({ wv t956 = v601_pos; wv t957 = rt_get(&F_visibleAt, v604_d); rt_lt(t956, t957); }), "&&")))) {
    RT_AT(4, 650);
    (void)({ wv t960 = v601_pos; wv t961 = ({ wv t958 = rt_text(v600_name); wv t959 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v604_d)), "resolve")); rt_cat(5, (wv[]){ rt_s(&S114), t958, rt_s(&S478), t959, rt_s(&S16) }); }); rt_chk(s475_error(up, t960, t961, RT_NULL), "resolve"); });
  }
  RT_RET((RT_AT(4, 652), v604_d));
}

static wv s496_block(struct fr119 *up, wv v605_b, wv v606_scope, wv v607_ctx) {
  rt_enter("block");
  v606_scope = rt_bind(v606_scope, 0);
  v607_ctx = rt_bind(v607_ctx, 0);
  RT_AT(4, 656);
  wv v608_k = rt_i(0);
  RT_AT(4, 657);
  while (rt_cond((RT_AT(4, 657), ({ wv t962 = v608_k; wv t963 = std_length(rt_get(&F_stmts, v605_b)); rt_lt(t962, t963); })))) {
    RT_AT(4, 658);
    wv v609_s = rt_bind(({ wv t964 = v608_k; wv t965 = rt_get(&F_stmts, v605_b); rt_idx(t964, t965); }), 0);
    RT_AT(4, 659);
    if (rt_cond(rt_eq(rt_get(&F_k, v609_s), rt_s(&S33)))) {
      RT_AT(4, 660);
      wv v610_d = rt_bind(({ wv t966 = rt_get(&F_name, v609_s); wv t967 = rt_get(&F_namePos, v609_s); wv t968 = rt_get(&F_id, rt_get(&F_func, v607_ctx)); wv t969 = std_length(rt_get(&F_params, v609_s)); rt_chk(s491_decl(up, rt_s(&S33), t966, rt_s(&S13), t967, RT_NULL, t968, t969), "block"); }), 0);
      RT_AT(4, 661);
      (void)({ wv t970 = v606_scope; wv t971 = rt_get(&F_name, v609_s); wv t972 = v610_d; rt_chk(s493_declare(up, t970, t971, t972), "block"); });
      RT_AT(4, 662);
      if (rt_cond(({ wv t975 = ({ wv t973 = v606_scope; wv t974 = rt_get(&F_name, v609_s); rt_chk(s492_own(up, t973, t974), "block"); }); wv t976 = v610_d; rt_eq(t975, t976); }))) {
        RT_AT(4, 663);
        (void)({ wv t981 = rt_def(up->v467_functions, "functions"); wv t982 = ({ wv t977 = rt_def(up->v466_curMod, "curMod"); wv t978 = rt_get(&F_name, v609_s); wv t979 = rt_get(&F_namePos, v609_s); wv t980 = rt_get(&F_body, v609_s); rt_rec(0, 4, K48, (wv[]){ t977, t978, t979, t980 }); }); std_push(t981, t982); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v609_s), rt_s(&S47)))) {
      RT_AT(4, 666);
      wv v611_d = rt_bind(({ wv t983 = rt_get(&F_name, v609_s); wv t984 = rt_get(&F_namePos, v609_s); wv t985 = rt_get(&F_id, rt_get(&F_func, v607_ctx)); rt_chk(s491_decl(up, rt_s(&S47), t983, rt_s(&S13), t984, RT_NULL, t985, RT_NULL), "block"); }), 0);
      RT_AT(4, 667);
      (void)({ wv t986 = v606_scope; wv t987 = rt_get(&F_name, v609_s); wv t988 = v611_d; rt_chk(s493_declare(up, t986, t987, t988), "block"); });
      RT_AT(4, 668);
      if (rt_cond(({ wv t991 = ({ wv t989 = v606_scope; wv t990 = rt_get(&F_name, v609_s); rt_chk(s492_own(up, t989, t990), "block"); }); wv t992 = v611_d; rt_eq(t991, t992); }))) {
        RT_AT(4, 668);
        (void)({ wv t996 = rt_def(up->v469_enums, "enums"); wv t997 = ({ wv t993 = rt_def(up->v466_curMod, "curMod"); wv t994 = rt_get(&F_name, v609_s); wv t995 = rt_get(&F_members, v609_s); rt_rec(0, 3, K49, (wv[]){ t993, t994, t995 }); }); std_push(t996, t997); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v609_s), rt_s(&S179)))) {
      RT_AT(4, 670);
      (void)({ wv t1003 = v606_scope; wv t1004 = rt_get(&F_name, v609_s); wv t1005 = ({ wv t998 = rt_get(&F_name, v609_s); wv t999 = rt_chk(w5_modeOf(v609_s), "block"); wv t1000 = rt_get(&F_namePos, v609_s); wv t1001 = rt_get(&F_end, v609_s); wv t1002 = rt_get(&F_id, rt_get(&F_func, v607_ctx)); rt_chk(s491_decl(up, rt_s(&S179), t998, t999, t1000, t1001, t1002, RT_NULL), "block"); }); rt_chk(s493_declare(up, t1003, t1004, t1005), "block"); });
    }
    RT_AT(4, 672);
    v608_k = rt_add(v608_k, rt_i(1));
  }
  RT_AT(4, 674);
  v608_k = rt_i(0);
  RT_AT(4, 675);
  while (rt_cond((RT_AT(4, 675), ({ wv t1006 = v608_k; wv t1007 = std_length(rt_get(&F_stmts, v605_b)); rt_lt(t1006, t1007); })))) {
    RT_AT(4, 676);
    (void)({ wv t1010 = ({ wv t1008 = v608_k; wv t1009 = rt_get(&F_stmts, v605_b); rt_idx(t1008, t1009); }); wv t1011 = v606_scope; wv t1012 = v607_ctx; rt_chk(s497_stmt(up, t1010, t1011, t1012), "block"); });
    RT_AT(4, 677);
    v608_k = rt_add(v608_k, rt_i(1));
  }
  RT_AT(4, 679);
  if (rt_cond(rt_ne(rt_get(&F_value, v605_b), RT_NULL))) {
    RT_AT(4, 679);
    RT_RET(({ wv t1013 = rt_get(&F_value, v605_b); wv t1014 = v606_scope; wv t1015 = v607_ctx; rt_chk(s517_expr(up, t1013, t1014, t1015), "block"); }));
  }
  RT_RET((RT_AT(4, 680), rt_chk(w5_endType(v605_b), "block")));
}

static wv s497_stmt(struct fr119 *up, wv v612_s, wv v613_scope, wv v614_ctx) {
  rt_enter("stmt");
  v613_scope = rt_bind(v613_scope, 0);
  v614_ctx = rt_bind(v614_ctx, 0);
  RT_AT(4, 684);
  wv v615_kind = rt_bind(rt_get(&F_k, v612_s), 0);
  RT_AT(4, 685);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v615_kind, rt_s(&S203)), "||") || rt_bool(rt_eq(v615_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v615_kind, rt_s(&S48)), "||")), "||") || rt_bool(rt_eq(v615_kind, rt_s(&S206)), "||")))) {
    RT_AT(4, 685);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 686);
  if (rt_cond(rt_eq(v615_kind, rt_s(&S33)))) {
    RT_AT(4, 687);
    wv v617_func = rt_rec(0, 2, K50, (wv[]){ rt_def(up->v465_nextFn, "nextFn"), rt_b(0) });
    RT_AT(4, 688);
    up->v465_nextFn = rt_add(rt_def(up->v465_nextFn, "nextFn"), rt_i(1));
    RT_AT(4, 689);
    wv v618_ps = rt_bind(rt_chk(s490_newScope(up, v613_scope), "stmt"), 0);
    RT_AT(4, 690);
    wv v619_k = rt_i(0);
    RT_AT(4, 691);
    while (rt_cond((RT_AT(4, 691), ({ wv t1016 = v619_k; wv t1017 = std_length(rt_get(&F_params, v612_s)); rt_lt(t1016, t1017); })))) {
      RT_AT(4, 692);
      wv v622_p = rt_bind(({ wv t1018 = v619_k; wv t1019 = rt_get(&F_params, v612_s); rt_idx(t1018, t1019); }), 0);
      RT_AT(4, 693);
      (void)({ wv t1024 = v618_ps; wv t1025 = rt_get(&F_name, v622_p); wv t1026 = ({ wv t1020 = rt_get(&F_name, v622_p); wv t1021 = rt_chk(w5_modeOf(v622_p), "stmt"); wv t1022 = rt_get(&F_pos, v622_p); wv t1023 = rt_get(&F_id, v617_func); rt_chk(s491_decl(up, rt_s(&S479), t1020, t1021, t1022, RT_NULL, t1023, RT_NULL), "stmt"); }); rt_chk(s493_declare(up, t1024, t1025, t1026), "stmt"); });
      RT_AT(4, 694);
      v619_k = rt_add(v619_k, rt_i(1));
    }
    RT_AT(4, 696);
    wv v620_bodyType = rt_bind(({ wv t1029 = rt_get(&F_body, v612_s); wv t1030 = rt_chk(s490_newScope(up, v618_ps), "stmt"); wv t1031 = ({ wv t1027 = v617_func; wv t1028 = rt_get(&F_guards, v614_ctx); rt_rec(0, 3, K51, (wv[]){ t1027, rt_i(0), t1028 }); }); rt_chk(s496_block(up, t1029, t1030, t1031), "stmt"); }), 0);
    RT_AT(4, 699);
    wv v621_result = rt_bind(v620_bodyType, 1);
    RT_AT(4, 700);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v612_s)), RT_NULL), "&&") && rt_bool(rt_eq(v620_bodyType, RT_NULL), "&&")))) {
      RT_AT(4, 700);
      v621_result = rt_bind(rt_def(w5_tNull, "tNull"), 1);
    }
    RT_AT(4, 701);
    if (rt_cond(({ wv t1032 = rt_def(up->v472_returnTypes, "returnTypes"); wv t1033 = rt_get(&F_id, v617_func); std_has(t1032, t1033); }))) {
      RT_AT(4, 701);
      v621_result = rt_bind(({ wv t1036 = v621_result; wv t1037 = ({ wv t1034 = rt_get(&F_id, v617_func); wv t1035 = rt_def(up->v472_returnTypes, "returnTypes"); rt_idx(t1034, t1035); }); rt_chk(w5_join(t1036, t1037), "stmt"); }), 1);
    }
    RT_AT(4, 702);
    if (rt_cond(rt_ne(v621_result, RT_NULL))) {
      RT_AT(4, 702);
      ({ wv t1040 = v621_result; wv t1041 = ({ wv t1038 = rt_text(rt_def(up->v466_curMod, "curMod")); wv t1039 = rt_text(rt_get(&F_namePos, v612_s)); rt_cat(3, (wv[]){ t1038, rt_s(&S28), t1039 }); }); wv t1042 = rt_def(up->v471_fnTypes, "fnTypes"); rt_set_idx(t1040, t1041, t1042); });
    }
    RT_AT(4, 703);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 706);
  wv v616_ty = RT_NULL;
  RT_AT(4, 707);
  if (rt_cond(rt_eq(v615_kind, rt_s(&S34)))) {
    RT_AT(4, 708);
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v614_ctx)))) {
      RT_AT(4, 708);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v612_s), rt_s(&S480), RT_NULL), "stmt");
    }
    RT_AT(4, 709);
    v616_ty = rt_bind((rt_cond(rt_ne(rt_get(&F_e, v612_s), RT_NULL)) ? ({ wv t1043 = rt_get(&F_e, v612_s); wv t1044 = v613_scope; wv t1045 = v614_ctx; rt_chk(s517_expr(up, t1043, t1044, t1045), "stmt"); }) : rt_def(w5_tNull, "tNull")), 1);
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S58)))) {
    RT_AT(4, 711);
    (void)({ wv t1046 = rt_get(&F_e, v612_s); wv t1047 = v613_scope; wv t1048 = v614_ctx; rt_chk(s517_expr(up, t1046, t1047, t1048), "stmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v615_kind, rt_s(&S43)), "||") || rt_bool(rt_eq(v615_kind, rt_s(&S44)), "||")))) {
    RT_AT(4, 713);
    if (rt_cond(rt_eq(rt_get(&F_loops, v614_ctx), rt_i(0)))) {
      RT_AT(4, 713);
      (void)({ wv t1049 = rt_get(&F_pos, v612_s); wv t1050 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text((rt_cond(rt_eq(v615_kind, rt_s(&S43))) ? rt_s(&S43) : rt_s(&S44))), rt_s(&S481) }); rt_chk(s475_error(up, t1049, t1050, RT_NULL), "stmt"); });
    }
    RT_AT(4, 714);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S41)))) {
    RT_AT(4, 716);
    (void)({ wv t1054 = rt_get(&F_cond, v612_s); wv t1055 = ({ wv t1051 = rt_get(&F_cond, v612_s); wv t1052 = v613_scope; wv t1053 = v614_ctx; rt_chk(s517_expr(up, t1051, t1052, t1053), "stmt"); }); rt_chk(s480_condition(up, t1054, t1055), "stmt"); });
    RT_AT(4, 719);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_cond, v612_s)), rt_s(&S300)), "&&") && rt_bool(({ wv t1056 = rt_get(&F_v, rt_get(&F_cond, v612_s)); wv t1057 = rt_get(&F_neg, v612_s); rt_eq(t1056, t1057); }), "&&")))) {
      RT_AT(4, 720);
      wv v623_message = rt_bind((rt_cond(rt_get(&F_neg, v612_s)) ? rt_s(&S482) : rt_s(&S483)), 0);
      RT_AT(4, 722);
      (void)({ wv t1058 = rt_get(&F_pos, rt_get(&F_cond, v612_s)); wv t1059 = v623_message; rt_chk(s475_error(up, t1058, t1059, rt_s(&S484)), "stmt"); });
    }
    RT_AT(4, 724);
    (void)({ wv t1063 = rt_get(&F_body, v612_s); wv t1064 = rt_chk(s490_newScope(up, v613_scope), "stmt"); wv t1065 = ({ wv t1060 = rt_get(&F_func, v614_ctx); wv t1061 = rt_add(rt_get(&F_loops, v614_ctx), rt_i(1)); wv t1062 = rt_get(&F_guards, v614_ctx); rt_rec(0, 3, K51, (wv[]){ t1060, t1061, t1062 }); }); rt_chk(s496_block(up, t1063, t1064, t1065), "stmt"); });
    RT_AT(4, 725);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S42)))) {
    RT_AT(4, 727);
    (void)rt_chk(s498_forLoop(up, v612_s, v613_scope, v614_ctx), "stmt");
    RT_AT(4, 728);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S56)))) {
    RT_AT(4, 730);
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v614_ctx)))) {
      RT_AT(4, 730);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v612_s), rt_s(&S485), RT_NULL), "stmt");
    }
    RT_AT(4, 731);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S55)))) {
    RT_AT(4, 733);
    if (rt_cond(rt_ne(rt_get(&F_e, v612_s), RT_NULL))) {
      RT_AT(4, 733);
      (void)({ wv t1066 = rt_get(&F_e, v612_s); wv t1067 = v613_scope; wv t1068 = v614_ctx; rt_chk(s517_expr(up, t1066, t1067, t1068), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S178)))) {
    RT_AT(4, 735);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v612_s)), rt_s(&S185)))) {
      RT_AT(4, 736);
      if (rt_cond(rt_get(&F_top, rt_get(&F_func, v614_ctx)))) {
        RT_AT(4, 736);
        (void)rt_chk(s475_error(up, rt_get(&F_pos, rt_get(&F_e, v612_s)), rt_s(&S485), RT_NULL), "stmt");
      }
      RT_AT(4, 737);
      (void)({ wv t1069 = rt_get(&F_arg, rt_get(&F_e, v612_s)); wv t1070 = v613_scope; wv t1071 = v614_ctx; rt_chk(s517_expr(up, t1069, t1070, t1071), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v612_s)), rt_s(&S186)))) {
      RT_AT(4, 739);
      (void)({ wv t1072 = rt_get(&F_e, v612_s); wv t1073 = v613_scope; wv t1074 = v614_ctx; rt_chk(s513_chain(up, t1072, t1073, t1074, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v612_s)), rt_s(&S45)))) {
      RT_AT(4, 741);
      (void)({ wv t1075 = rt_get(&F_e, v612_s); wv t1076 = v613_scope; wv t1077 = v614_ctx; rt_chk(s504_checkMatch(up, t1075, t1076, t1077, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v612_s)), rt_s(&S184)))) {
      RT_AT(4, 744);
      wv v624_types = rt_arr(0, NULL, 1);
      RT_AT(4, 745);
      {
        wfor t1079;
        rt_for_start(&t1079, rt_get(&F_args, rt_get(&F_e, v612_s)), 0);
        for (int64_t t1078 = 0; rt_for_guard(&t1079, rt_get(&F_args, rt_get(&F_e, v612_s)), "s.e.args"), rt_for_more(&t1079, t1078); t1078++) {
          wv v625_a = rt_for_item(&t1079, t1078);
          RT_AT(4, 745);
          (void)({ wv t1080 = v624_types; wv t1081 = rt_chk(s517_expr(up, v625_a, v613_scope, v614_ctx), "stmt"); std_push(t1080, t1081); });
        }
      }
      RT_AT(4, 746);
      (void)({ wv t1082 = rt_get(&F_e, v612_s); wv t1083 = v613_scope; wv t1084 = v624_types; rt_chk(s518_callee(up, t1082, t1083, rt_b(0), t1084), "stmt"); });
    } else {
      RT_AT(4, 748);
      (void)({ wv t1085 = rt_get(&F_e, v612_s); wv t1086 = v613_scope; wv t1087 = v614_ctx; rt_chk(s517_expr(up, t1085, t1086, t1087), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S179)))) {
    RT_AT(4, 751);
    v616_ty = rt_bind(({ wv t1088 = rt_get(&F_e, v612_s); wv t1089 = v613_scope; wv t1090 = v614_ctx; rt_chk(s517_expr(up, t1088, t1089, t1090), "stmt"); }), 1);
  } else if (rt_cond(rt_eq(v615_kind, rt_s(&S38)))) {
    RT_AT(4, 753);
    (void)({ wv t1091 = rt_get(&F_e, v612_s); wv t1092 = v613_scope; wv t1093 = v614_ctx; rt_chk(s517_expr(up, t1091, t1092, t1093), "stmt"); });
    RT_AT(4, 754);
    (void)({ wv t1094 = rt_get(&F_target, v612_s); wv t1095 = v613_scope; wv t1096 = v614_ctx; rt_chk(s514_lvalue(up, t1094, t1095, t1096), "stmt"); });
  }
  RT_AT(4, 757);
  if (rt_cond(rt_ne(rt_get(&F_handler, v612_s), RT_NULL))) {
    RT_AT(4, 757);
    v616_ty = rt_bind(({ wv t1097 = v616_ty; wv t1098 = rt_chk(s503_onfailHandler(up, v612_s, v613_scope, v614_ctx), "stmt"); rt_chk(w5_join(t1097, t1098), "stmt"); }), 1);
  }
  RT_AT(4, 758);
  if (rt_cond(rt_b(rt_bool(rt_eq(v615_kind, rt_s(&S34)), "&&") && rt_bool(rt_not(rt_get(&F_top, rt_get(&F_func, v614_ctx))), "&&")))) {
    RT_AT(4, 759);
    wv v626_id = rt_bind(rt_get(&F_id, rt_get(&F_func, v614_ctx)), 0);
    RT_AT(4, 760);
    ({ wv t1105 = (rt_cond(({ wv t1099 = rt_def(up->v472_returnTypes, "returnTypes"); wv t1100 = v626_id; std_has(t1099, t1100); })) ? ({ wv t1103 = ({ wv t1101 = v626_id; wv t1102 = rt_def(up->v472_returnTypes, "returnTypes"); rt_idx(t1101, t1102); }); wv t1104 = v616_ty; rt_chk(w5_join(t1103, t1104), "stmt"); }) : v616_ty); wv t1106 = v626_id; wv t1107 = rt_def(up->v472_returnTypes, "returnTypes"); rt_set_idx(t1105, t1106, t1107); });
  }
  RT_AT(4, 764);
  if (rt_cond(rt_b(rt_bool(rt_eq(v615_kind, rt_s(&S179)), "&&") && rt_bool(rt_not(rt_b(rt_bool(rt_b(rt_bool(rt_get(&F_isVar, v612_s), "||") || rt_bool(rt_get(&F_isRef, v612_s), "||")), "&&") && rt_bool(({ wv t1108 = rt_def(up->v473_rebound, "rebound"); wv t1109 = rt_get(&F_name, v612_s); std_has(t1108, t1109); }), "&&"))), "&&")))) {
    RT_AT(4, 765);
    (void)({ wv t1110 = rt_get(&F_namePos, v612_s); wv t1111 = (rt_cond(rt_get(&F_isVar, v612_s)) ? rt_chk(w5_thaw(v616_ty), "stmt") : v616_ty); rt_chk(s477_bindType(up, t1110, t1111), "stmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s498_forLoop(struct fr119 *up, wv v627_s, wv v628_scope, wv v629_ctx) {
  rt_enter("forLoop");
  v627_s = rt_bind(v627_s, 0);
  v628_scope = rt_bind(v628_scope, 0);
  v629_ctx = rt_bind(v629_ctx, 0);
  RT_AT(4, 773);
  wv v630_guards = rt_arr(0, NULL, 1);
  RT_AT(4, 774);
  wv v631_k = rt_i(0);
  RT_AT(4, 775);
  while (rt_cond((RT_AT(4, 775), ({ wv t1112 = v631_k; wv t1113 = std_length(rt_get(&F_guards, v629_ctx)); rt_lt(t1112, t1113); })))) {
    RT_AT(4, 776);
    (void)({ wv t1116 = v630_guards; wv t1117 = ({ wv t1114 = v631_k; wv t1115 = rt_get(&F_guards, v629_ctx); rt_idx(t1114, t1115); }); std_push(t1116, t1117); });
    RT_AT(4, 777);
    v631_k = rt_add(v631_k, rt_i(1));
  }
  RT_AT(4, 780);
  wv v632_names = rt_arr(0, NULL, 1);
  RT_AT(4, 781);
  wv v633_en = rt_bind((rt_cond(rt_eq(rt_get(&F_coll, v627_s), RT_NULL)) ? RT_NULL : ({ wv t1118 = rt_get(&F_coll, v627_s); wv t1119 = v628_scope; rt_chk(s507_enumNamed(up, t1118, t1119), "forLoop"); })), 0);
  RT_AT(4, 782);
  if (rt_cond(rt_eq(rt_get(&F_coll, v627_s), RT_NULL))) {
    RT_AT(4, 783);
    (void)({ wv t1123 = rt_get(&F_from, v627_s); wv t1124 = ({ wv t1120 = rt_get(&F_from, v627_s); wv t1121 = v628_scope; wv t1122 = v629_ctx; rt_chk(s517_expr(up, t1120, t1121, t1122), "forLoop"); }); rt_chk(s499_rangeBound(up, t1123, t1124), "forLoop"); });
    RT_AT(4, 784);
    (void)({ wv t1128 = rt_get(&F_to, v627_s); wv t1129 = ({ wv t1125 = rt_get(&F_to, v627_s); wv t1126 = v628_scope; wv t1127 = v629_ctx; rt_chk(s517_expr(up, t1125, t1126, t1127), "forLoop"); }); rt_chk(s499_rangeBound(up, t1128, t1129), "forLoop"); });
    RT_AT(4, 785);
    if (rt_cond(rt_ne(rt_get(&F_step, v627_s), RT_NULL))) {
      RT_AT(4, 786);
      wv v635_st = rt_bind(({ wv t1130 = rt_get(&F_step, v627_s); wv t1131 = v628_scope; wv t1132 = v629_ctx; rt_chk(s517_expr(up, t1130, t1131, t1132), "forLoop"); }), 0);
      RT_AT(4, 787);
      if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v635_st), "forLoop"), "&&") && rt_bool(rt_ne(rt_get(&F_t, v635_st), rt_s(&S139)), "&&")))) {
        RT_AT(4, 787);
        (void)({ wv t1133 = rt_get(&F_pos, rt_get(&F_step, v627_s)); wv t1134 = rt_cat(2, (wv[]){ rt_s(&S486), rt_text(rt_chk(w5_typeText(v635_st), "forLoop")) }); rt_chk(s475_error(up, t1133, t1134, RT_NULL), "forLoop"); });
      }
    }
    RT_AT(4, 789);
    v632_names = rt_arr(1, (wv[]){ rt_def(w5_tInt, "tInt") }, 1);
  } else if (rt_cond(rt_ne(v633_en, RT_NULL))) {
    RT_AT(4, 791);
    v632_names = rt_arr(1, (wv[]){ rt_chk(s478_enumType(up, v633_en), "forLoop") }, 1);
  } else {
    RT_AT(4, 793);
    wv v636_ct = rt_bind(({ wv t1135 = rt_get(&F_coll, v627_s); wv t1136 = v628_scope; wv t1137 = v629_ctx; rt_chk(s517_expr(up, t1135, t1136, t1137), "forLoop"); }), 0);
    RT_AT(4, 794);
    (void)({ wv t1138 = rt_get(&F_coll, v627_s); wv t1139 = v636_ct; rt_chk(s500_collection(up, t1138, t1139), "forLoop"); });
    RT_AT(4, 795);
    if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v636_ct), "forLoop"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v636_ct), rt_s(&S412)), "&&")))) {
      RT_AT(4, 795);
      v632_names = rt_bind((rt_cond(rt_eq(std_length(rt_get(&F_names, v627_s)), rt_i(2))) ? ({ wv t1140 = rt_def(w5_tInt, "tInt"); wv t1141 = rt_get(&F_elem, v636_ct); rt_arr(2, (wv[]){ t1140, t1141 }, 0); }) : rt_arr(1, (wv[]){ rt_get(&F_elem, v636_ct) }, 0)), 1);
    }
    RT_AT(4, 796);
    wv v637_path = rt_bind(rt_chk(s501_pathOf(up, rt_get(&F_coll, v627_s)), "forLoop"), 0);
    RT_AT(4, 797);
    if (rt_cond(rt_ne(v637_path, RT_NULL))) {
      RT_AT(4, 797);
      (void)std_push(v630_guards, v637_path);
    }
  }
  RT_AT(4, 799);
  wv v634_ls = rt_bind(rt_chk(s490_newScope(up, v628_scope), "forLoop"), 0);
  RT_AT(4, 800);
  v631_k = rt_i(0);
  RT_AT(4, 801);
  while (rt_cond((RT_AT(4, 801), ({ wv t1142 = v631_k; wv t1143 = std_length(rt_get(&F_names, v627_s)); rt_lt(t1142, t1143); })))) {
    RT_AT(4, 802);
    wv v638_n = rt_bind(({ wv t1144 = v631_k; wv t1145 = rt_get(&F_names, v627_s); rt_idx(t1144, t1145); }), 0);
    RT_AT(4, 803);
    (void)({ wv t1149 = v634_ls; wv t1150 = rt_get(&F_name, v638_n); wv t1151 = ({ wv t1146 = rt_get(&F_name, v638_n); wv t1147 = rt_get(&F_pos, v638_n); wv t1148 = rt_get(&F_id, rt_get(&F_func, v629_ctx)); rt_chk(s491_decl(up, rt_s(&S487), t1146, rt_s(&S13), t1147, RT_NULL, t1148, RT_NULL), "forLoop"); }); rt_chk(s493_declare(up, t1149, t1150, t1151), "forLoop"); });
    RT_AT(4, 804);
    if (rt_cond(({ wv t1152 = v631_k; wv t1153 = std_length(v632_names); rt_lt(t1152, t1153); }))) {
      RT_AT(4, 804);
      (void)({ wv t1154 = rt_get(&F_pos, v638_n); wv t1155 = rt_idx(v631_k, v632_names); rt_chk(s477_bindType(up, t1154, t1155), "forLoop"); });
    }
    RT_AT(4, 805);
    v631_k = rt_add(v631_k, rt_i(1));
  }
  RT_AT(4, 807);
  (void)({ wv t1159 = rt_get(&F_body, v627_s); wv t1160 = rt_chk(s490_newScope(up, v634_ls), "forLoop"); wv t1161 = ({ wv t1156 = rt_get(&F_func, v629_ctx); wv t1157 = rt_add(rt_get(&F_loops, v629_ctx), rt_i(1)); wv t1158 = v630_guards; rt_rec(0, 3, K51, (wv[]){ t1156, t1157, t1158 }); }); rt_chk(s496_block(up, t1159, t1160, t1161), "forLoop"); });
  RT_RET(RT_NULL);
}

static wv s499_rangeBound(struct fr119 *up, wv v639_x, wv v640_ty) {
  rt_enter("rangeBound");
  v639_x = rt_bind(v639_x, 0);
  v640_ty = rt_bind(v640_ty, 0);
  RT_AT(4, 812);
  if (rt_cond(rt_b(rt_bool(rt_not(rt_chk(w5_known(v640_ty), "rangeBound")), "||") || rt_bool(rt_eq(rt_get(&F_t, v640_ty), rt_s(&S139)), "||")))) {
    RT_AT(4, 812);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 813);
  wv v641_hint = rt_bind((rt_cond(rt_eq(rt_get(&F_t, v640_ty), rt_s(&S412))) ? rt_s(&S488) : (rt_cond(rt_eq(rt_get(&F_t, v640_ty), rt_s(&S149))) ? rt_s(&S489) : RT_NULL)), 0);
  RT_AT(4, 815);
  (void)({ wv t1162 = rt_get(&F_pos, v639_x); wv t1163 = rt_cat(2, (wv[]){ rt_s(&S490), rt_text(rt_chk(w5_typeText(v640_ty), "rangeBound")) }); wv t1164 = v641_hint; rt_chk(s475_error(up, t1162, t1163, t1164), "rangeBound"); });
  RT_RET(RT_NULL);
}

static wv s500_collection(struct fr119 *up, wv v642_x, wv v643_ty) {
  rt_enter("collection");
  v642_x = rt_bind(v642_x, 0);
  v643_ty = rt_bind(v643_ty, 0);
  RT_AT(4, 820);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_chk(w5_known(v643_ty), "collection")), "||") || rt_bool(rt_eq(rt_get(&F_t, v643_ty), rt_s(&S412)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v643_ty), rt_s(&S189)), "||")))) {
    RT_AT(4, 820);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 821);
  wv v644_hint = RT_NULL;
  RT_AT(4, 822);
  if (rt_cond(rt_eq(rt_get(&F_t, v643_ty), rt_s(&S187)))) {
    RT_AT(4, 823);
    v644_hint = rt_s(&S491);
  } else if (rt_cond(rt_eq(rt_get(&F_t, v643_ty), rt_s(&S414)))) {
    RT_AT(4, 825);
    v644_hint = rt_s(&S492);
  } else if (rt_cond(rt_eq(rt_get(&F_t, v643_ty), rt_s(&S139)))) {
    RT_AT(4, 827);
    v644_hint = rt_s(&S493);
  }
  RT_AT(4, 829);
  (void)({ wv t1165 = rt_get(&F_pos, v642_x); wv t1166 = rt_cat(2, (wv[]){ rt_s(&S494), rt_text(rt_chk(w5_typeText(v643_ty), "collection")) }); wv t1167 = v644_hint; rt_chk(s475_error(up, t1165, t1166, t1167), "collection"); });
  RT_RET(RT_NULL);
}

static wv s501_pathOf(struct fr119 *up, wv v645_x) {
  rt_enter("pathOf");
  v645_x = rt_bind(v645_x, 0);
  RT_AT(4, 834);
  if (rt_cond(rt_eq(rt_get(&F_k, v645_x), rt_s(&S214)))) {
    RT_AT(4, 834);
    RT_RET(rt_arr(1, (wv[]){ rt_get(&F_name, v645_x) }, 0));
  }
  RT_AT(4, 835);
  if (rt_cond(rt_ne(rt_get(&F_k, v645_x), rt_s(&S192)))) {
    RT_AT(4, 835);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 836);
  wv v646_p = rt_bind(rt_chk(s501_pathOf(up, rt_get(&F_rec, v645_x)), "pathOf"), 0);
  RT_AT(4, 837);
  if (rt_cond(rt_eq(v646_p, RT_NULL))) {
    RT_AT(4, 837);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 838);
  wv v647_out = rt_bind(v646_p, 1);
  RT_AT(4, 839);
  (void)({ wv t1168 = v647_out; wv t1169 = rt_get(&F_field, v645_x); std_push(t1168, t1169); });
  RT_RET((RT_AT(4, 840), v647_out));
}

static wv s502_guarded(struct fr119 *up, wv v648_t, wv v649_ctx) {
  rt_enter("guarded");
  v648_t = rt_bind(v648_t, 0);
  v649_ctx = rt_bind(v649_ctx, 0);
  RT_AT(4, 846);
  wv v650_p = rt_bind(rt_chk(s501_pathOf(up, v648_t), "guarded"), 0);
  RT_AT(4, 847);
  if (rt_cond(rt_eq(v650_p, RT_NULL))) {
    RT_AT(4, 847);
    RT_RET(rt_b(0));
  }
  RT_AT(4, 848);
  wv v651_g = rt_i(0);
  RT_AT(4, 849);
  while (rt_cond((RT_AT(4, 849), ({ wv t1170 = v651_g; wv t1171 = std_length(rt_get(&F_guards, v649_ctx)); rt_lt(t1170, t1171); })))) {
    RT_AT(4, 850);
    wv v652_gp = rt_bind(({ wv t1172 = v651_g; wv t1173 = rt_get(&F_guards, v649_ctx); rt_idx(t1172, t1173); }), 0);
    RT_AT(4, 851);
    v651_g = rt_add(v651_g, rt_i(1));
    RT_AT(4, 852);
    wv v653_prefix = ({ wv t1174 = std_length(v650_p); wv t1175 = std_length(v652_gp); rt_le(t1174, t1175); });
    RT_AT(4, 853);
    wv v654_k = rt_i(0);
    RT_AT(4, 854);
    while (rt_cond((RT_AT(4, 854), rt_b(rt_bool(v653_prefix, "&&") && rt_bool(({ wv t1176 = v654_k; wv t1177 = std_length(v650_p); rt_lt(t1176, t1177); }), "&&"))))) {
      RT_AT(4, 855);
      if (rt_cond(({ wv t1178 = rt_idx(v654_k, v650_p); wv t1179 = rt_idx(v654_k, v652_gp); rt_ne(t1178, t1179); }))) {
        RT_AT(4, 855);
        v653_prefix = rt_b(0);
      }
      RT_AT(4, 856);
      v654_k = rt_add(v654_k, rt_i(1));
    }
    RT_AT(4, 858);
    if (rt_cond(v653_prefix)) {
      RT_AT(4, 859);
      wv v655_name = rt_bind(std_join(v650_p, rt_s(&S130)), 0);
      RT_AT(4, 860);
      wv v656_over = rt_bind((rt_cond(({ wv t1180 = std_length(v650_p); wv t1181 = std_length(v652_gp); rt_eq(t1180, t1181); })) ? rt_s(&S495) : rt_cat(3, (wv[]){ rt_s(&S114), rt_text(std_join(v652_gp, rt_s(&S130))), rt_s(&S114) })), 0);
      RT_AT(4, 861);
      (void)({ wv t1184 = rt_get(&F_pos, v648_t); wv t1185 = ({ wv t1182 = rt_text(v655_name); wv t1183 = rt_text(v656_over); rt_cat(4, (wv[]){ rt_s(&S496), t1182, rt_s(&S497), t1183 }); }); wv t1186 = rt_cat(3, (wv[]){ rt_s(&S498), rt_text(v655_name), rt_s(&S499) }); rt_chk(s475_error(up, t1184, t1185, t1186), "guarded"); });
      RT_AT(4, 863);
      RT_RET(rt_b(1));
    }
  }
  RT_RET((RT_AT(4, 866), rt_b(0)));
}

static wv s503_onfailHandler(struct fr119 *up, wv v657_s, wv v658_scope, wv v659_ctx) {
  rt_enter("onfailHandler");
  v657_s = rt_bind(v657_s, 0);
  v658_scope = rt_bind(v658_scope, 0);
  v659_ctx = rt_bind(v659_ctx, 0);
  RT_AT(4, 871);
  wv v660_h = rt_bind(rt_get(&F_handler, v657_s), 0);
  RT_AT(4, 872);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_e, v657_s), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v657_s)), rt_s(&S184)), "||")))) {
    RT_AT(4, 873);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v660_h), rt_s(&S500), rt_s(&S501)), "onfailHandler");
  }
  RT_AT(4, 876);
  wv v661_hs = rt_bind(rt_chk(s490_newScope(up, v658_scope), "onfailHandler"), 0);
  RT_AT(4, 877);
  if (rt_cond(rt_ne(rt_get(&F_param, v660_h), RT_NULL))) {
    RT_AT(4, 878);
    (void)({ wv t1190 = v661_hs; wv t1191 = rt_get(&F_param, v660_h); wv t1192 = ({ wv t1187 = rt_get(&F_param, v660_h); wv t1188 = rt_get(&F_paramPos, v660_h); wv t1189 = rt_get(&F_id, rt_get(&F_func, v659_ctx)); rt_chk(s491_decl(up, rt_s(&S57), t1187, rt_s(&S13), t1188, RT_NULL, t1189, RT_NULL), "onfailHandler"); }); rt_chk(s493_declare(up, t1190, t1191, t1192), "onfailHandler"); });
    RT_AT(4, 879);
    (void)({ wv t1193 = rt_get(&F_paramPos, v660_h); wv t1194 = rt_def(w5_tInt, "tInt"); rt_chk(s477_bindType(up, t1193, t1194), "onfailHandler"); });
  }
  RT_AT(4, 881);
  wv v662_ty = rt_bind(({ wv t1195 = rt_get(&F_body, v660_h); wv t1196 = rt_chk(s490_newScope(up, v661_hs), "onfailHandler"); wv t1197 = v659_ctx; rt_chk(s496_block(up, t1195, t1196, t1197), "onfailHandler"); }), 0);
  RT_AT(4, 882);
  if (rt_cond(({ wv t1198 = rt_def(w5_onfailValueUsed, "onfailValueUsed"); wv t1199 = rt_get(&F_k, v657_s); rt_chk(w1_contains(t1198, t1199), "onfailHandler"); }))) {
    RT_AT(4, 882);
    (void)rt_chk(s489_requireValue(up, rt_get(&F_body, v660_h)), "onfailHandler");
  }
  RT_RET((RT_AT(4, 883), v662_ty));
}

static wv s504_checkMatch(struct fr119 *up, wv v663_e, wv v664_scope, wv v665_ctx, wv v666_stmtLevel) {
  rt_enter("checkMatch");
  v663_e = rt_bind(v663_e, 0);
  v664_scope = rt_bind(v664_scope, 0);
  v665_ctx = rt_bind(v665_ctx, 0);
  v666_stmtLevel = rt_bind(v666_stmtLevel, 0);
  RT_AT(4, 890);
  (void)({ wv t1200 = rt_get(&F_subject, v663_e); wv t1201 = v664_scope; wv t1202 = v665_ctx; rt_chk(s517_expr(up, t1200, t1201, t1202), "checkMatch"); });
  RT_AT(4, 892);
  wv v667_result = rt_bind(rt_def(w5_tNever, "tNever"), 1);
  RT_AT(4, 894);
  wv v668_seen = rt_arr(0, NULL, 1);
  RT_AT(4, 897);
  wv v669_sort = RT_NULL;
  RT_AT(4, 899);
  wv v670_sortEnum = RT_NULL;
  RT_AT(4, 900);
  wv v671_a = rt_i(0);
  RT_AT(4, 901);
  while (rt_cond((RT_AT(4, 901), ({ wv t1203 = v671_a; wv t1204 = std_length(rt_get(&F_cases, v663_e)); rt_lt(t1203, t1204); })))) {
    RT_AT(4, 902);
    wv v672_al = rt_bind(({ wv t1205 = v671_a; wv t1206 = rt_get(&F_cases, v663_e); rt_idx(t1205, t1206); }), 0);
    RT_AT(4, 903);
    wv v673_j = rt_i(0);
    RT_AT(4, 904);
    while (rt_cond((RT_AT(4, 904), ({ wv t1207 = v673_j; wv t1208 = std_length(rt_get(&F_values, v672_al)); rt_lt(t1207, t1208); })))) {
      RT_AT(4, 905);
      wv v674_v = rt_bind(({ wv t1209 = v673_j; wv t1210 = rt_get(&F_values, v672_al); rt_idx(t1209, t1210); }), 0);
      RT_AT(4, 906);
      wv v675_vs = rt_s(&S13);
      RT_AT(4, 907);
      if (rt_cond(rt_eq(rt_get(&F_k, v674_v), rt_s(&S192)))) {
        RT_AT(4, 908);
        wv v679_d = rt_bind(rt_chk(s511_caseEnum(up, v674_v, v664_scope), "checkMatch"), 0);
        RT_AT(4, 909);
        v675_vs = rt_bind((rt_cond(rt_ne(v679_d, RT_NULL)) ? rt_chk(w5_enumText(rt_get(&F_rec, v674_v)), "checkMatch") : RT_NULL), 1);
        RT_AT(4, 910);
        if (rt_cond(rt_eq(v669_sort, RT_NULL))) {
          RT_AT(4, 910);
          v670_sortEnum = rt_bind(v679_d, 1);
        }
      }
      RT_AT(4, 912);
      if (rt_cond(rt_eq(v669_sort, RT_NULL))) {
        RT_AT(4, 913);
        v669_sort = rt_bind(v675_vs, 1);
      } else if (rt_cond(rt_b(rt_bool(rt_ne(v675_vs, RT_NULL), "&&") && rt_bool(rt_ne(v675_vs, v669_sort), "&&")))) {
        RT_AT(4, 915);
        wv v680_message = rt_bind((rt_cond(rt_b(rt_bool(rt_eq(v669_sort, rt_s(&S13)), "||") || rt_bool(rt_eq(v675_vs, rt_s(&S13)), "||"))) ? rt_s(&S502) : ({ wv t1211 = rt_text(v669_sort); wv t1212 = rt_text(v675_vs); rt_cat(5, (wv[]){ rt_s(&S503), t1211, rt_s(&S504), t1212, rt_s(&S114) }); })), 0);
        RT_AT(4, 917);
        (void)({ wv t1213 = rt_get(&F_pos, v674_v); wv t1214 = v680_message; rt_chk(s475_error(up, t1213, t1214, RT_NULL), "checkMatch"); });
      }
      RT_AT(4, 919);
      wv v676_key = rt_bind(rt_chk(w5_caseKey(v674_v), "checkMatch"), 0);
      RT_AT(4, 920);
      wv v677_first = RT_NULL;
      RT_AT(4, 921);
      wv v678_q = rt_i(0);
      RT_AT(4, 922);
      while (rt_cond((RT_AT(4, 922), ({ wv t1215 = v678_q; wv t1216 = std_length(v668_seen); rt_lt(t1215, t1216); })))) {
        RT_AT(4, 923);
        if (rt_cond(({ wv t1217 = rt_get(&F_key, rt_idx(v678_q, v668_seen)); wv t1218 = v676_key; rt_eq(t1217, t1218); }))) {
          RT_AT(4, 923);
          v677_first = rt_bind(rt_get(&F_pos, rt_idx(v678_q, v668_seen)), 1);
        }
        RT_AT(4, 924);
        v678_q = rt_add(v678_q, rt_i(1));
      }
      RT_AT(4, 926);
      if (rt_cond(rt_ne(v677_first, RT_NULL))) {
        RT_AT(4, 927);
        (void)({ wv t1221 = rt_get(&F_pos, v674_v); wv t1222 = ({ wv t1219 = rt_text(rt_chk(w5_caseText(v674_v), "checkMatch")); wv t1220 = rt_text(rt_chk(w5_lineOf(v677_first), "checkMatch")); rt_cat(5, (wv[]){ rt_s(&S505), t1219, rt_s(&S506), t1220, rt_s(&S16) }); }); rt_chk(s475_error(up, t1221, t1222, RT_NULL), "checkMatch"); });
      } else {
        RT_AT(4, 929);
        (void)({ wv t1225 = v668_seen; wv t1226 = ({ wv t1223 = v676_key; wv t1224 = rt_get(&F_pos, v674_v); rt_rec(0, 2, K52, (wv[]){ t1223, t1224 }); }); std_push(t1225, t1226); });
      }
      RT_AT(4, 931);
      v673_j = rt_add(v673_j, rt_i(1));
    }
    RT_AT(4, 933);
    v667_result = rt_bind(({ wv t1230 = v667_result; wv t1231 = ({ wv t1227 = rt_get(&F_body, v672_al); wv t1228 = rt_chk(s490_newScope(up, v664_scope), "checkMatch"); wv t1229 = v665_ctx; rt_chk(s496_block(up, t1227, t1228, t1229), "checkMatch"); }); rt_chk(w5_join(t1230, t1231), "checkMatch"); }), 1);
    RT_AT(4, 934);
    if (rt_cond(rt_not(v666_stmtLevel))) {
      RT_AT(4, 934);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_body, v672_al)), "checkMatch");
    }
    RT_AT(4, 935);
    v671_a = rt_add(v671_a, rt_i(1));
  }
  RT_AT(4, 937);
  if (rt_cond(rt_ne(rt_get(&F_els, v663_e), RT_NULL))) {
    RT_AT(4, 938);
    v667_result = rt_bind(({ wv t1235 = v667_result; wv t1236 = ({ wv t1232 = rt_get(&F_els, v663_e); wv t1233 = rt_chk(s490_newScope(up, v664_scope), "checkMatch"); wv t1234 = v665_ctx; rt_chk(s496_block(up, t1232, t1233, t1234), "checkMatch"); }); rt_chk(w5_join(t1235, t1236), "checkMatch"); }), 1);
    RT_AT(4, 939);
    if (rt_cond(rt_not(v666_stmtLevel))) {
      RT_AT(4, 939);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_els, v663_e)), "checkMatch");
    }
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v669_sort, RT_NULL), "&&") && rt_bool(rt_ne(v669_sort, rt_s(&S13)), "&&")))) {
    RT_AT(4, 942);
    wv v681_ms = rt_bind(rt_chk(s487_enumMembers(up, v670_sortEnum), "checkMatch"), 0);
    RT_AT(4, 943);
    wv v682_missing = rt_arr(0, NULL, 1);
    RT_AT(4, 944);
    wv v683_m = rt_i(0);
    RT_AT(4, 945);
    while (rt_cond((RT_AT(4, 945), ({ wv t1237 = v683_m; wv t1238 = std_length(v681_ms); rt_lt(t1237, t1238); })))) {
      RT_AT(4, 946);
      wv v684_key = ({ wv t1239 = rt_text(rt_get(&F_name, rt_idx(v683_m, v681_ms))); wv t1240 = rt_text(v669_sort); rt_cat(4, (wv[]){ rt_s(&S410), t1239, rt_s(&S130), t1240 }); });
      RT_AT(4, 947);
      wv v685_handled = rt_b(0);
      RT_AT(4, 948);
      wv v686_q = rt_i(0);
      RT_AT(4, 949);
      while (rt_cond((RT_AT(4, 949), ({ wv t1241 = v686_q; wv t1242 = std_length(v668_seen); rt_lt(t1241, t1242); })))) {
        RT_AT(4, 950);
        if (rt_cond(({ wv t1243 = rt_get(&F_key, rt_idx(v686_q, v668_seen)); wv t1244 = v684_key; rt_eq(t1243, t1244); }))) {
          RT_AT(4, 950);
          v685_handled = rt_b(1);
        }
        RT_AT(4, 951);
        v686_q = rt_add(v686_q, rt_i(1));
      }
      RT_AT(4, 953);
      if (rt_cond(rt_not(v685_handled))) {
        RT_AT(4, 953);
        (void)({ wv t1247 = v682_missing; wv t1248 = ({ wv t1245 = rt_text(v669_sort); wv t1246 = rt_text(rt_get(&F_name, rt_idx(v683_m, v681_ms))); rt_cat(5, (wv[]){ rt_s(&S114), t1245, rt_s(&S130), t1246, rt_s(&S114) }); }); std_push(t1247, t1248); });
      }
      RT_AT(4, 954);
      v683_m = rt_add(v683_m, rt_i(1));
    }
    RT_AT(4, 956);
    if (rt_cond(rt_gt(std_length(v682_missing), rt_i(0)))) {
      RT_AT(4, 957);
      (void)({ wv t1249 = rt_get(&F_pos, v663_e); wv t1250 = rt_cat(2, (wv[]){ rt_s(&S507), rt_text(std_join(v682_missing, rt_s(&S419))) }); rt_chk(s475_error(up, t1249, t1250, rt_s(&S508)), "checkMatch"); });
    }
  } else if (rt_cond(rt_not(v666_stmtLevel))) {
    RT_AT(4, 961);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v663_e), rt_s(&S509), RT_NULL), "checkMatch");
  }
  RT_RET((RT_AT(4, 963), v667_result));
}

static wv s505_moduleNamed(struct fr119 *up, wv v687_x, wv v688_scope) {
  rt_enter("moduleNamed");
  v688_scope = rt_bind(v688_scope, 0);
  RT_AT(4, 968);
  if (rt_cond(rt_ne(rt_get(&F_k, v687_x), rt_s(&S214)))) {
    RT_AT(4, 968);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 969);
  wv v689_d = rt_bind(({ wv t1251 = v688_scope; wv t1252 = rt_get(&F_name, v687_x); rt_chk(s494_lookup(up, t1251, t1252), "moduleNamed"); }), 0);
  RT_RET((RT_AT(4, 970), (rt_cond(rt_b(rt_bool(rt_ne(v689_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v689_d), rt_s(&S510)), "&&"))) ? v689_d : RT_NULL)));
}

static wv s506_exported(struct fr119 *up, wv v690_lib, wv v691_name) {
  rt_enter("exported");
  v690_lib = rt_bind(v690_lib, 0);
  v691_name = rt_bind(v691_name, 0);
  RT_AT(4, 975);
  wv v692_xs = rt_bind(rt_get(&F_exports, rt_idx(v690_lib, up->v462_modules)), 0);
  RT_AT(4, 976);
  wv v693_k = rt_i(0);
  RT_AT(4, 977);
  while (rt_cond((RT_AT(4, 977), ({ wv t1253 = v693_k; wv t1254 = std_length(v692_xs); rt_lt(t1253, t1254); })))) {
    RT_AT(4, 978);
    if (rt_cond(({ wv t1255 = rt_get(&F_name, rt_idx(v693_k, v692_xs)); wv t1256 = v691_name; rt_eq(t1255, t1256); }))) {
      RT_AT(4, 978);
      RT_RET(rt_idx(v693_k, v692_xs));
    }
    RT_AT(4, 979);
    v693_k = rt_add(v693_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 981), RT_NULL));
}

static wv s507_enumNamed(struct fr119 *up, wv v694_x, wv v695_scope) {
  rt_enter("enumNamed");
  v695_scope = rt_bind(v695_scope, 0);
  RT_AT(4, 986);
  if (rt_cond(rt_eq(rt_get(&F_k, v694_x), rt_s(&S214)))) {
    RT_AT(4, 987);
    wv v698_d = rt_bind(({ wv t1257 = v695_scope; wv t1258 = rt_get(&F_name, v694_x); rt_chk(s494_lookup(up, t1257, t1258), "enumNamed"); }), 0);
    RT_AT(4, 988);
    RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v698_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v698_d), rt_s(&S47)), "&&"))) ? v698_d : RT_NULL));
  }
  RT_AT(4, 990);
  if (rt_cond(rt_ne(rt_get(&F_k, v694_x), rt_s(&S192)))) {
    RT_AT(4, 990);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 991);
  wv v696_m = rt_bind(({ wv t1259 = rt_get(&F_rec, v694_x); wv t1260 = v695_scope; rt_chk(s505_moduleNamed(up, t1259, t1260), "enumNamed"); }), 0);
  RT_AT(4, 992);
  if (rt_cond(rt_b(rt_bool(rt_eq(v696_m, RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_lib, v696_m), RT_NULL), "||")))) {
    RT_AT(4, 992);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 993);
  wv v697_d = rt_bind(({ wv t1261 = rt_get(&F_lib, v696_m); wv t1262 = rt_get(&F_field, v694_x); rt_chk(s506_exported(up, t1261, t1262), "enumNamed"); }), 0);
  RT_RET((RT_AT(4, 994), (rt_cond(rt_b(rt_bool(rt_ne(v697_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v697_d), rt_s(&S47)), "&&"))) ? v697_d : RT_NULL)));
}

static wv s508_exportOf(struct fr119 *up, wv v699_e, wv v700_m) {
  rt_enter("exportOf");
  v699_e = rt_bind(v699_e, 0);
  v700_m = rt_bind(v700_m, 0);
  RT_AT(4, 1000);
  if (rt_cond(rt_eq(rt_get(&F_lib, v700_m), RT_NULL))) {
    RT_AT(4, 1000);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1001);
  wv v701_d = rt_bind(({ wv t1263 = rt_get(&F_lib, v700_m); wv t1264 = rt_get(&F_field, v699_e); rt_chk(s506_exported(up, t1263, t1264), "exportOf"); }), 0);
  RT_AT(4, 1002);
  if (rt_cond(rt_eq(v701_d, RT_NULL))) {
    RT_AT(4, 1002);
    (void)({ wv t1269 = rt_get(&F_pos, v699_e); wv t1270 = ({ wv t1267 = rt_text(rt_get(&F_path, ({ wv t1265 = rt_get(&F_lib, v700_m); wv t1266 = up->v462_modules; rt_idx(t1265, t1266); }))); wv t1268 = rt_text(rt_get(&F_field, v699_e)); rt_cat(5, (wv[]){ rt_s(&S114), t1267, rt_s(&S511), t1268, rt_s(&S114) }); }); rt_chk(s475_error(up, t1269, t1270, RT_NULL), "exportOf"); });
  }
  RT_RET((RT_AT(4, 1003), v701_d));
}

static wv s509_unknownModule(struct fr119 *up, wv v702_x, wv v703_scope) {
  rt_enter("unknownModule");
  v702_x = rt_bind(v702_x, 0);
  v703_scope = rt_bind(v703_scope, 0);
  RT_AT(4, 1007);
  wv v704_message = rt_bind((rt_cond(rt_ne(({ wv t1271 = v703_scope; wv t1272 = rt_get(&F_name, v702_x); rt_chk(s494_lookup(up, t1271, t1272), "unknownModule"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v702_x)), rt_s(&S512) }) : rt_cat(3, (wv[]){ rt_s(&S513), rt_text(rt_get(&F_name, v702_x)), rt_s(&S114) })), 0);
  RT_AT(4, 1008);
  (void)({ wv t1273 = rt_get(&F_pos, v702_x); wv t1274 = v704_message; rt_chk(s475_error(up, t1273, t1274, RT_NULL), "unknownModule"); });
  RT_RET(RT_NULL);
}

static wv s510_qualified(struct fr119 *up, wv v705_e, wv v706_m) {
  rt_enter("qualified");
  v705_e = rt_bind(v705_e, 0);
  v706_m = rt_bind(v706_m, 0);
  RT_AT(4, 1013);
  wv v707_d = rt_bind(rt_chk(s508_exportOf(up, v705_e, v706_m), "qualified"), 0);
  RT_AT(4, 1014);
  wv v708_label = ({ wv t1275 = rt_text(rt_get(&F_name, rt_get(&F_rec, v705_e))); wv t1276 = rt_text(rt_get(&F_field, v705_e)); rt_cat(3, (wv[]){ t1275, rt_s(&S130), t1276 }); });
  RT_AT(4, 1015);
  if (rt_cond(rt_b(rt_bool(rt_ne(v707_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v707_d), rt_s(&S33)), "&&")))) {
    RT_AT(4, 1016);
    (void)({ wv t1279 = rt_get(&F_pos, v705_e); wv t1280 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v708_label), rt_s(&S514) }); wv t1281 = ({ wv t1277 = rt_text(v708_label); wv t1278 = rt_text(v708_label); rt_cat(5, (wv[]){ rt_s(&S515), t1277, rt_s(&S516), t1278, rt_s(&S517) }); }); rt_chk(s475_error(up, t1279, t1280, t1281), "qualified"); });
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v707_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v707_d), rt_s(&S47)), "&&")))) {
    RT_AT(4, 1018);
    (void)({ wv t1282 = rt_get(&F_pos, v705_e); wv t1283 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v708_label), rt_s(&S518) }); wv t1284 = rt_cat(3, (wv[]){ rt_s(&S519), rt_text(v708_label), rt_s(&S520) }); rt_chk(s475_error(up, t1282, t1283, t1284), "qualified"); });
  } else if (rt_cond(rt_ne(v707_d, RT_NULL))) {
    RT_AT(4, 1020);
    RT_RET(rt_chk(s476_typeOfDecl(up, v707_d), "qualified"));
  }
  RT_RET((RT_AT(4, 1022), RT_NULL));
}

static wv s511_caseEnum(struct fr119 *up, wv v709_v, wv v710_scope) {
  rt_enter("caseEnum");
  v709_v = rt_bind(v709_v, 0);
  v710_scope = rt_bind(v710_scope, 0);
  RT_AT(4, 1028);
  wv v711_x = rt_bind(rt_get(&F_rec, v709_v), 0);
  RT_AT(4, 1029);
  wv v712_d = rt_bind(rt_chk(s507_enumNamed(up, v711_x, v710_scope), "caseEnum"), 0);
  RT_AT(4, 1030);
  if (rt_cond(rt_ne(v712_d, RT_NULL))) {
    RT_AT(4, 1031);
    (void)rt_chk(s512_member(up, v709_v, v712_d), "caseEnum");
    RT_AT(4, 1032);
    RT_RET(v712_d);
  }
  RT_AT(4, 1034);
  wv v713_hint = rt_s(&S521);
  RT_AT(4, 1035);
  if (rt_cond(rt_eq(rt_get(&F_k, v711_x), rt_s(&S214)))) {
    RT_AT(4, 1036);
    wv v715_message = rt_bind((rt_cond(rt_ne(({ wv t1285 = v710_scope; wv t1286 = rt_get(&F_name, v711_x); rt_chk(s494_lookup(up, t1285, t1286), "caseEnum"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v711_x)), rt_s(&S522) }) : rt_cat(3, (wv[]){ rt_s(&S523), rt_text(rt_get(&F_name, v711_x)), rt_s(&S114) })), 0);
    RT_AT(4, 1037);
    (void)({ wv t1287 = rt_get(&F_pos, v711_x); wv t1288 = v715_message; wv t1289 = v713_hint; rt_chk(s475_error(up, t1287, t1288, t1289), "caseEnum"); });
    RT_AT(4, 1038);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1040);
  wv v714_m = rt_bind(({ wv t1290 = rt_get(&F_rec, v711_x); wv t1291 = v710_scope; rt_chk(s505_moduleNamed(up, t1290, t1291), "caseEnum"); }), 0);
  RT_AT(4, 1041);
  if (rt_cond(rt_eq(v714_m, RT_NULL))) {
    RT_AT(4, 1042);
    (void)({ wv t1292 = rt_get(&F_rec, v711_x); wv t1293 = v710_scope; rt_chk(s509_unknownModule(up, t1292, t1293), "caseEnum"); });
  } else if (rt_cond(rt_ne(rt_chk(s508_exportOf(up, v711_x, v714_m), "caseEnum"), RT_NULL))) {
    RT_AT(4, 1044);
    (void)({ wv t1294 = rt_get(&F_pos, v711_x); wv t1295 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_chk(w5_enumText(v711_x), "caseEnum")), rt_s(&S522) }); wv t1296 = v713_hint; rt_chk(s475_error(up, t1294, t1295, t1296), "caseEnum"); });
  }
  RT_RET((RT_AT(4, 1046), RT_NULL));
}

static wv s512_member(struct fr119 *up, wv v716_e, wv v717_d) {
  rt_enter("member");
  v716_e = rt_bind(v716_e, 0);
  v717_d = rt_bind(v717_d, 0);
  RT_AT(4, 1051);
  wv v718_ms = rt_bind(rt_chk(s487_enumMembers(up, v717_d), "member"), 0);
  RT_AT(4, 1052);
  wv v719_names = rt_arr(0, NULL, 1);
  RT_AT(4, 1053);
  wv v720_found = rt_b(0);
  RT_AT(4, 1054);
  wv v721_k = rt_i(0);
  RT_AT(4, 1055);
  while (rt_cond((RT_AT(4, 1055), ({ wv t1297 = v721_k; wv t1298 = std_length(v718_ms); rt_lt(t1297, t1298); })))) {
    RT_AT(4, 1056);
    (void)({ wv t1299 = v719_names; wv t1300 = rt_get(&F_name, rt_idx(v721_k, v718_ms)); std_push(t1299, t1300); });
    RT_AT(4, 1057);
    if (rt_cond(({ wv t1301 = rt_get(&F_name, rt_idx(v721_k, v718_ms)); wv t1302 = rt_get(&F_field, v716_e); rt_eq(t1301, t1302); }))) {
      RT_AT(4, 1057);
      v720_found = rt_b(1);
    }
    RT_AT(4, 1058);
    v721_k = rt_add(v721_k, rt_i(1));
  }
  RT_AT(4, 1060);
  if (rt_cond(rt_not(v720_found))) {
    RT_AT(4, 1061);
    (void)({ wv t1305 = rt_get(&F_pos, v716_e); wv t1306 = ({ wv t1303 = rt_text(rt_get(&F_field, v716_e)); wv t1304 = rt_text(rt_get(&F_name, v717_d)); rt_cat(5, (wv[]){ rt_s(&S524), t1303, rt_s(&S525), t1304, rt_s(&S114) }); }); wv t1307 = rt_cat(2, (wv[]){ rt_s(&S526), rt_text(std_join(v719_names, rt_s(&S419))) }); rt_chk(s475_error(up, t1305, t1306, t1307), "member"); });
  }
  RT_RET(RT_NULL);
}

static wv s513_chain(struct fr119 *up, wv v722_e, wv v723_scope, wv v724_ctx, wv v725_stmtLevel) {
  rt_enter("chain");
  v722_e = rt_bind(v722_e, 0);
  v723_scope = rt_bind(v723_scope, 0);
  v724_ctx = rt_bind(v724_ctx, 0);
  v725_stmtLevel = rt_bind(v725_stmtLevel, 0);
  RT_AT(4, 1067);
  wv v726_result = rt_bind(rt_def(w5_tNever, "tNever"), 1);
  RT_AT(4, 1068);
  wv v727_k = rt_i(0);
  RT_AT(4, 1069);
  while (rt_cond((RT_AT(4, 1069), ({ wv t1308 = v727_k; wv t1309 = std_length(rt_get(&F_branches, v722_e)); rt_lt(t1308, t1309); })))) {
    RT_AT(4, 1070);
    wv v728_b = rt_bind(({ wv t1310 = v727_k; wv t1311 = rt_get(&F_branches, v722_e); rt_idx(t1310, t1311); }), 0);
    RT_AT(4, 1071);
    (void)({ wv t1315 = rt_get(&F_cond, v728_b); wv t1316 = ({ wv t1312 = rt_get(&F_cond, v728_b); wv t1313 = v723_scope; wv t1314 = v724_ctx; rt_chk(s517_expr(up, t1312, t1313, t1314), "chain"); }); rt_chk(s480_condition(up, t1315, t1316), "chain"); });
    RT_AT(4, 1072);
    v726_result = rt_bind(({ wv t1320 = v726_result; wv t1321 = ({ wv t1317 = rt_get(&F_body, v728_b); wv t1318 = rt_chk(s490_newScope(up, v723_scope), "chain"); wv t1319 = v724_ctx; rt_chk(s496_block(up, t1317, t1318, t1319), "chain"); }); rt_chk(w5_join(t1320, t1321), "chain"); }), 1);
    RT_AT(4, 1073);
    if (rt_cond(rt_not(v725_stmtLevel))) {
      RT_AT(4, 1073);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_body, v728_b)), "chain");
    }
    RT_AT(4, 1074);
    v727_k = rt_add(v727_k, rt_i(1));
  }
  RT_AT(4, 1076);
  if (rt_cond(rt_ne(rt_get(&F_els, v722_e), RT_NULL))) {
    RT_AT(4, 1077);
    v726_result = rt_bind(({ wv t1325 = v726_result; wv t1326 = ({ wv t1322 = rt_get(&F_els, v722_e); wv t1323 = rt_chk(s490_newScope(up, v723_scope), "chain"); wv t1324 = v724_ctx; rt_chk(s496_block(up, t1322, t1323, t1324), "chain"); }); rt_chk(w5_join(t1325, t1326), "chain"); }), 1);
    RT_AT(4, 1078);
    if (rt_cond(rt_not(v725_stmtLevel))) {
      RT_AT(4, 1078);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_els, v722_e)), "chain");
    }
  } else if (rt_cond(rt_not(v725_stmtLevel))) {
    RT_AT(4, 1080);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v722_e), rt_s(&S527), RT_NULL), "chain");
  }
  RT_RET((RT_AT(4, 1082), v726_result));
}

static wv s514_lvalue(struct fr119 *up, wv v729_t, wv v730_scope, wv v731_ctx) {
  rt_enter("lvalue");
  v730_scope = rt_bind(v730_scope, 0);
  v731_ctx = rt_bind(v731_ctx, 0);
  RT_AT(4, 1086);
  if (rt_cond(rt_chk(s502_guarded(up, v729_t, v731_ctx), "lvalue"))) {
    RT_AT(4, 1086);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1087);
  if (rt_cond(rt_eq(rt_get(&F_k, v729_t), rt_s(&S214)))) {
    RT_AT(4, 1088);
    wv v737_d = rt_bind(({ wv t1327 = rt_get(&F_name, v729_t); wv t1328 = rt_get(&F_pos, v729_t); wv t1329 = v730_scope; wv t1330 = v731_ctx; rt_chk(s495_resolve(up, t1327, t1328, t1329, t1330), "lvalue"); }), 0);
    RT_AT(4, 1089);
    if (rt_cond(rt_ne(v737_d, RT_NULL))) {
      RT_AT(4, 1090);
      (void)({ wv t1331 = v737_d; wv t1332 = rt_get(&F_name, v729_t); wv t1333 = rt_get(&F_pos, v729_t); wv t1334 = rt_cat(3, (wv[]){ rt_s(&S496), rt_text(rt_get(&F_name, v729_t)), rt_s(&S528) }); rt_chk(s515_mustBeMutable(up, t1331, t1332, t1333, t1334, rt_b(1)), "lvalue"); });
      RT_AT(4, 1092);
      if (rt_cond(rt_get(&F_isRef, v737_d))) {
        RT_AT(4, 1092);
        (void)({ wv t1337 = rt_get(&F_refTargets, ({ wv t1335 = rt_def(up->v466_curMod, "curMod"); wv t1336 = up->v462_modules; rt_idx(t1335, t1336); })); wv t1338 = rt_get(&F_pos, v729_t); std_push(t1337, t1338); });
      }
    }
    RT_AT(4, 1094);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1098);
  wv v732_isMem = rt_eq(rt_get(&F_k, v729_t), rt_s(&S192));
  RT_AT(4, 1099);
  wv v733_box = rt_bind((rt_cond(v732_isMem) ? rt_get(&F_rec, v729_t) : rt_get(&F_arr, v729_t)), 0);
  RT_AT(4, 1100);
  wv v734_what = rt_bind((rt_cond(v732_isMem) ? rt_s(&S529) : rt_s(&S530)), 0);
  RT_AT(4, 1101);
  wv v735_it = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v729_t), rt_s(&S191))) ? ({ wv t1339 = rt_get(&F_index, v729_t); wv t1340 = v730_scope; wv t1341 = v731_ctx; rt_chk(s517_expr(up, t1339, t1340, t1341), "lvalue"); }) : RT_NULL), 0);
  RT_AT(4, 1102);
  if (rt_cond(v732_isMem)) {
    RT_AT(4, 1103);
    if (rt_cond(rt_ne(rt_chk(s507_enumNamed(up, v733_box, v730_scope), "lvalue"), RT_NULL))) {
      RT_AT(4, 1104);
      (void)({ wv t1344 = rt_get(&F_pos, v729_t); wv t1345 = ({ wv t1342 = rt_text(rt_chk(w5_enumText(v733_box), "lvalue")); wv t1343 = rt_text(rt_get(&F_field, v729_t)); rt_cat(5, (wv[]){ rt_s(&S496), t1342, rt_s(&S130), t1343, rt_s(&S531) }); }); rt_chk(s475_error(up, t1344, t1345, RT_NULL), "lvalue"); });
      RT_AT(4, 1105);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1108);
    wv v738_m = rt_bind(rt_chk(s505_moduleNamed(up, v733_box, v730_scope), "lvalue"), 0);
    RT_AT(4, 1109);
    if (rt_cond(rt_ne(v738_m, RT_NULL))) {
      RT_AT(4, 1110);
      if (rt_cond(rt_ne(rt_chk(s508_exportOf(up, v729_t, v738_m), "lvalue"), RT_NULL))) {
        RT_AT(4, 1111);
        (void)({ wv t1351 = rt_get(&F_pos, v729_t); wv t1352 = ({ wv t1348 = rt_text(rt_get(&F_name, v733_box)); wv t1349 = rt_text(rt_get(&F_field, v729_t)); wv t1350 = rt_text(rt_get(&F_path, ({ wv t1346 = rt_get(&F_lib, v738_m); wv t1347 = up->v462_modules; rt_idx(t1346, t1347); }))); rt_cat(7, (wv[]){ rt_s(&S496), t1348, rt_s(&S130), t1349, rt_s(&S532), t1350, rt_s(&S533) }); }); rt_chk(s475_error(up, t1351, t1352, RT_NULL), "lvalue"); });
      }
      RT_AT(4, 1113);
      RT_RET(RT_NULL);
    }
  }
  RT_AT(4, 1117);
  wv v736_bt = RT_NULL;
  RT_AT(4, 1118);
  if (rt_cond(rt_eq(rt_get(&F_k, v733_box), rt_s(&S214)))) {
    RT_AT(4, 1119);
    wv v739_d = rt_bind(({ wv t1353 = rt_get(&F_name, v733_box); wv t1354 = rt_get(&F_pos, v733_box); wv t1355 = v730_scope; wv t1356 = v731_ctx; rt_chk(s495_resolve(up, t1353, t1354, t1355, t1356), "lvalue"); }), 0);
    RT_AT(4, 1120);
    if (rt_cond(rt_ne(v739_d, RT_NULL))) {
      RT_AT(4, 1121);
      (void)({ wv t1359 = v739_d; wv t1360 = rt_get(&F_name, v733_box); wv t1361 = rt_get(&F_pos, v733_box); wv t1362 = ({ wv t1357 = rt_text(v734_what); wv t1358 = rt_text(rt_get(&F_name, v733_box)); rt_cat(5, (wv[]){ rt_s(&S534), t1357, rt_s(&S535), t1358, rt_s(&S536) }); }); rt_chk(s515_mustBeMutable(up, t1359, t1360, t1361, t1362, rt_b(0)), "lvalue"); });
      RT_AT(4, 1122);
      v736_bt = rt_bind(rt_chk(s476_typeOfDecl(up, v739_d), "lvalue"), 1);
    }
  } else {
    RT_AT(4, 1125);
    v736_bt = rt_bind(rt_chk(s517_expr(up, v733_box, v730_scope, v731_ctx), "lvalue"), 1);
  }
  RT_AT(4, 1127);
  if (rt_cond(v732_isMem)) {
    RT_AT(4, 1128);
    (void)({ wv t1363 = v736_bt; wv t1364 = rt_get(&F_field, v729_t); wv t1365 = rt_get(&F_pos, v729_t); rt_chk(s483_fieldOf(up, t1363, t1364, t1365, rt_s(&S537)), "lvalue"); });
  } else {
    RT_AT(4, 1130);
    (void)({ wv t1366 = v736_bt; wv t1367 = rt_get(&F_index, v729_t); wv t1368 = v735_it; wv t1369 = rt_get(&F_pos, v729_t); rt_chk(s484_indexed(up, t1366, t1367, t1368, t1369, rt_s(&S537)), "lvalue"); });
  }
  RT_RET(RT_NULL);
}

static wv s515_mustBeMutable(struct fr119 *up, wv v740_d, wv v741_name, wv v742_pos, wv v743_message, wv v744_rebind) {
  rt_enter("mustBeMutable");
  v740_d = rt_bind(v740_d, 0);
  v741_name = rt_bind(v741_name, 0);
  v742_pos = rt_bind(v742_pos, 0);
  v743_message = rt_bind(v743_message, 0);
  v744_rebind = rt_bind(v744_rebind, 0);
  RT_AT(4, 1138);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v740_d), rt_s(&S33)), "||") || rt_bool(rt_eq(rt_get(&F_kind, v740_d), rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_kind, v740_d), rt_s(&S510)), "||")))) {
    RT_AT(4, 1139);
    (void)({ wv t1372 = v742_pos; wv t1373 = ({ wv t1370 = rt_text(v741_name); wv t1371 = rt_text(rt_chk(w5_kindText(rt_get(&F_kind, v740_d)), "mustBeMutable")); rt_cat(5, (wv[]){ rt_s(&S114), t1370, rt_s(&S538), t1371, rt_s(&S539) }); }); rt_chk(s475_error(up, t1372, t1373, RT_NULL), "mustBeMutable"); });
  } else if (rt_cond(rt_eq(rt_get(&F_kind, v740_d), rt_s(&S487)))) {
    RT_AT(4, 1142);
    if (rt_cond(v744_rebind)) {
      RT_AT(4, 1143);
      (void)({ wv t1374 = v742_pos; wv t1375 = rt_cat(3, (wv[]){ rt_s(&S496), rt_text(v741_name), rt_s(&S540) }); rt_chk(s475_error(up, t1374, t1375, rt_s(&S541)), "mustBeMutable"); });
    }
  } else if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_isVar, v740_d)), "&&") && rt_bool(rt_not(rt_get(&F_isRef, v740_d)), "&&")))) {
    RT_AT(4, 1147);
    wv v745_hint = RT_NULL;
    RT_AT(4, 1148);
    if (rt_cond(rt_eq(rt_get(&F_kind, v740_d), rt_s(&S479)))) {
      RT_AT(4, 1149);
      v745_hint = rt_bind((rt_cond(v744_rebind) ? ({ wv t1376 = rt_text(v741_name); wv t1377 = rt_text(v741_name); rt_cat(5, (wv[]){ rt_s(&S542), t1376, rt_s(&S543), t1377, rt_s(&S544) }); }) : rt_cat(3, (wv[]){ rt_s(&S545), rt_text(v741_name), rt_s(&S16) })), 1);
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v740_d), rt_s(&S179)))) {
      RT_AT(4, 1152);
      v745_hint = rt_bind((rt_cond(v744_rebind) ? ({ wv t1378 = rt_text(v741_name); wv t1379 = rt_text(v741_name); rt_cat(5, (wv[]){ rt_s(&S546), t1378, rt_s(&S547), t1379, rt_s(&S548) }); }) : rt_cat(3, (wv[]){ rt_s(&S549), rt_text(v741_name), rt_s(&S550) })), 1);
    }
    RT_AT(4, 1155);
    (void)rt_chk(s475_error(up, v742_pos, v743_message, v745_hint), "mustBeMutable");
  }
  RT_RET(RT_NULL);
}

static wv s516_unknownStd(struct fr119 *up, wv v746_node) {
  rt_enter("unknownStd");
  v746_node = rt_bind(v746_node, 0);
  RT_AT(4, 1160);
  (void)({ wv t1380 = rt_get(&F_pos, v746_node); wv t1381 = rt_cat(3, (wv[]){ rt_s(&S551), rt_text(rt_get(&F_name, v746_node)), rt_s(&S114) }); rt_chk(s475_error(up, t1380, t1381, rt_s(&S137)), "unknownStd"); });
  RT_RET(RT_NULL);
}

static wv s517_expr(struct fr119 *up, wv v747_e, wv v748_scope, wv v749_ctx) {
  rt_enter("expr");
  v748_scope = rt_bind(v748_scope, 0);
  v749_ctx = rt_bind(v749_ctx, 0);
  RT_AT(4, 1166);
  wv v750_kind = rt_bind(rt_get(&F_k, v747_e), 0);
  RT_AT(4, 1167);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S139)))) {
    RT_AT(4, 1167);
    RT_RET(rt_def(w5_tInt, "tInt"));
  }
  RT_AT(4, 1168);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v750_kind, rt_s(&S149)), "||") || rt_bool(rt_eq(v750_kind, rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(v750_kind, rt_s(&S54)), "||")))) {
    RT_AT(4, 1168);
    RT_RET(rt_def(w5_tFloat, "tFloat"));
  }
  RT_AT(4, 1169);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S300)))) {
    RT_AT(4, 1169);
    RT_RET(rt_def(w5_tBool, "tBool"));
  }
  RT_AT(4, 1170);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S52)))) {
    RT_AT(4, 1170);
    RT_RET(rt_def(w5_tNull, "tNull"));
  }
  RT_AT(4, 1171);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S173)))) {
    RT_AT(4, 1172);
    {
      wfor t1383;
      rt_for_start(&t1383, rt_get(&F_parts, v747_e), 0);
      for (int64_t t1382 = 0; rt_for_guard(&t1383, rt_get(&F_parts, v747_e), "e.parts"), rt_for_more(&t1383, t1382); t1382++) {
        wv v751_part = rt_for_item(&t1383, t1382);
        RT_AT(4, 1173);
        if (rt_cond(rt_ne(std_typeOf(v751_part), rt_s(&S187)))) {
          RT_AT(4, 1173);
          (void)({ wv t1384 = v751_part; wv t1385 = rt_chk(s517_expr(up, v751_part, v748_scope, v749_ctx), "expr"); rt_chk(s479_noText(up, t1384, t1385), "expr"); });
        }
      }
    }
    RT_AT(4, 1175);
    RT_RET(rt_def(w5_tString, "tString"));
  }
  RT_AT(4, 1177);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S214)))) {
    RT_AT(4, 1178);
    wv v752_d = rt_bind(({ wv t1386 = rt_get(&F_name, v747_e); wv t1387 = rt_get(&F_pos, v747_e); wv t1388 = v748_scope; wv t1389 = v749_ctx; rt_chk(s495_resolve(up, t1386, t1387, t1388, t1389), "expr"); }), 0);
    RT_AT(4, 1179);
    if (rt_cond(rt_b(rt_bool(rt_ne(v752_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v752_d), rt_s(&S33)), "&&")))) {
      RT_AT(4, 1180);
      (void)({ wv t1392 = rt_get(&F_pos, v747_e); wv t1393 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v747_e)), rt_s(&S514) }); wv t1394 = ({ wv t1390 = rt_text(rt_get(&F_name, v747_e)); wv t1391 = rt_text(rt_get(&F_name, v747_e)); rt_cat(5, (wv[]){ rt_s(&S515), t1390, rt_s(&S516), t1391, rt_s(&S517) }); }); rt_chk(s475_error(up, t1392, t1393, t1394), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v752_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v752_d), rt_s(&S47)), "&&")))) {
      RT_AT(4, 1182);
      (void)({ wv t1395 = rt_get(&F_pos, v747_e); wv t1396 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v747_e)), rt_s(&S518) }); wv t1397 = rt_cat(3, (wv[]){ rt_s(&S519), rt_text(rt_get(&F_name, v747_e)), rt_s(&S520) }); rt_chk(s475_error(up, t1395, t1396, t1397), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v752_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v752_d), rt_s(&S510)), "&&")))) {
      RT_AT(4, 1184);
      (void)({ wv t1398 = rt_get(&F_pos, v747_e); wv t1399 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v747_e)), rt_s(&S552) }); wv t1400 = rt_cat(3, (wv[]){ rt_s(&S553), rt_text(rt_get(&F_name, v747_e)), rt_s(&S554) }); rt_chk(s475_error(up, t1398, t1399, t1400), "expr"); });
    } else if (rt_cond(rt_ne(v752_d, RT_NULL))) {
      RT_AT(4, 1186);
      RT_RET(rt_chk(s476_typeOfDecl(up, v752_d), "expr"));
    }
    RT_AT(4, 1188);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1190);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S157)))) {
    RT_AT(4, 1191);
    wv v753_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v747_e)), "expr"), 0);
    RT_AT(4, 1192);
    if (rt_cond(rt_eq(v753_sig, RT_NULL))) {
      RT_AT(4, 1193);
      (void)rt_chk(s516_unknownStd(up, v747_e), "expr");
    } else if (rt_cond(rt_ne(v753_sig, rt_s(&S35)))) {
      RT_AT(4, 1195);
      (void)({ wv t1403 = rt_get(&F_pos, v747_e); wv t1404 = ({ wv t1401 = rt_text(rt_get(&F_name, v747_e)); wv t1402 = rt_text(rt_get(&F_name, v747_e)); rt_cat(5, (wv[]){ rt_s(&S181), t1401, rt_s(&S555), t1402, rt_s(&S16) }); }); rt_chk(s475_error(up, t1403, t1404, RT_NULL), "expr"); });
    }
    RT_AT(4, 1197);
    RT_RET((rt_cond(rt_eq(rt_get(&F_name, v747_e), rt_s(&S367))) ? rt_chk(w5_arrayOf(rt_def(w5_tString, "tString")), "expr") : (rt_cond(rt_eq(rt_get(&F_name, v747_e), rt_s(&S369))) ? rt_def(w5_tString, "tString") : RT_NULL)));
  }
  RT_AT(4, 1200);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S188)))) {
    RT_AT(4, 1201);
    wv v754_elem = rt_bind(rt_def(w5_tNever, "tNever"), 1);
    RT_AT(4, 1202);
    {
      wfor t1406;
      rt_for_start(&t1406, rt_get(&F_items, v747_e), 0);
      for (int64_t t1405 = 0; rt_for_guard(&t1406, rt_get(&F_items, v747_e), "e.items"), rt_for_more(&t1406, t1405); t1405++) {
        wv v755_item = rt_for_item(&t1406, t1405);
        RT_AT(4, 1202);
        v754_elem = rt_bind(({ wv t1407 = v754_elem; wv t1408 = rt_chk(s517_expr(up, v755_item, v748_scope, v749_ctx), "expr"); rt_chk(w5_join(t1407, t1408), "expr"); }), 1);
      }
    }
    RT_AT(4, 1203);
    RT_RET(rt_chk(w5_arrayOf(v754_elem), "expr"));
  }
  RT_AT(4, 1205);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S189)))) {
    RT_AT(4, 1206);
    {
      wfor t1410;
      rt_for_start(&t1410, rt_get(&F_items, v747_e), 0);
      for (int64_t t1409 = 0; rt_for_guard(&t1410, rt_get(&F_items, v747_e), "e.items"), rt_for_more(&t1410, t1409); t1409++) {
        wv v756_item = rt_for_item(&t1410, t1409);
        RT_AT(4, 1206);
        (void)rt_chk(s517_expr(up, v756_item, v748_scope, v749_ctx), "expr");
      }
    }
    RT_AT(4, 1207);
    RT_RET(rt_def(w5_tMap, "tMap"));
  }
  RT_AT(4, 1210);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S190)))) {
    RT_AT(4, 1211);
    wv v757_names = rt_arr(0, NULL, 1);
    RT_AT(4, 1212);
    wv v758_types = rt_arr(0, NULL, 1);
    RT_AT(4, 1213);
    {
      wfor t1412;
      rt_for_start(&t1412, rt_get(&F_fields, v747_e), 0);
      for (int64_t t1411 = 0; rt_for_guard(&t1412, rt_get(&F_fields, v747_e), "e.fields"), rt_for_more(&t1412, t1411); t1411++) {
        wv v759_f = rt_for_item(&t1412, t1411);
        RT_AT(4, 1214);
        (void)({ wv t1413 = v757_names; wv t1414 = rt_get(&F_name, v759_f); std_push(t1413, t1414); });
        RT_AT(4, 1215);
        (void)({ wv t1418 = v758_types; wv t1419 = ({ wv t1415 = rt_get(&F_e, v759_f); wv t1416 = v748_scope; wv t1417 = v749_ctx; rt_chk(s517_expr(up, t1415, t1416, t1417), "expr"); }); std_push(t1418, t1419); });
      }
    }
    RT_AT(4, 1217);
    RT_RET(rt_chk(w5_recordType(v757_names, v758_types), "expr"));
  }
  RT_AT(4, 1219);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S191)))) {
    RT_AT(4, 1220);
    wv v760_it = rt_bind(({ wv t1420 = rt_get(&F_index, v747_e); wv t1421 = v748_scope; wv t1422 = v749_ctx; rt_chk(s517_expr(up, t1420, t1421, t1422), "expr"); }), 0);
    RT_AT(4, 1221);
    RT_RET(({ wv t1426 = ({ wv t1423 = rt_get(&F_arr, v747_e); wv t1424 = v748_scope; wv t1425 = v749_ctx; rt_chk(s517_expr(up, t1423, t1424, t1425), "expr"); }); wv t1427 = rt_get(&F_index, v747_e); wv t1428 = v760_it; wv t1429 = rt_get(&F_pos, v747_e); rt_chk(s484_indexed(up, t1426, t1427, t1428, t1429, rt_s(&S556)), "expr"); }));
  }
  RT_AT(4, 1225);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S192)))) {
    RT_AT(4, 1226);
    wv v761_lin = rt_bind(({ wv t1430 = rt_get(&F_rec, v747_e); wv t1431 = v748_scope; rt_chk(s507_enumNamed(up, t1430, t1431), "expr"); }), 0);
    RT_AT(4, 1227);
    if (rt_cond(rt_ne(v761_lin, RT_NULL))) {
      RT_AT(4, 1228);
      (void)rt_chk(s512_member(up, v747_e, v761_lin), "expr");
      RT_AT(4, 1229);
      RT_RET(rt_chk(s478_enumType(up, v761_lin), "expr"));
    }
    RT_AT(4, 1231);
    wv v762_m = rt_bind(({ wv t1432 = rt_get(&F_rec, v747_e); wv t1433 = v748_scope; rt_chk(s505_moduleNamed(up, t1432, t1433), "expr"); }), 0);
    RT_AT(4, 1232);
    if (rt_cond(rt_ne(v762_m, RT_NULL))) {
      RT_AT(4, 1232);
      RT_RET(rt_chk(s510_qualified(up, v747_e, v762_m), "expr"));
    }
    RT_AT(4, 1233);
    RT_RET(({ wv t1437 = ({ wv t1434 = rt_get(&F_rec, v747_e); wv t1435 = v748_scope; wv t1436 = v749_ctx; rt_chk(s517_expr(up, t1434, t1435, t1436), "expr"); }); wv t1438 = rt_get(&F_field, v747_e); wv t1439 = rt_get(&F_pos, v747_e); rt_chk(s483_fieldOf(up, t1437, t1438, t1439, rt_s(&S556)), "expr"); }));
  }
  RT_AT(4, 1235);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S193)))) {
    RT_AT(4, 1236);
    wv v763_ot = rt_bind(({ wv t1440 = rt_get(&F_e, v747_e); wv t1441 = v748_scope; wv t1442 = v749_ctx; rt_chk(s517_expr(up, t1440, t1441, t1442), "expr"); }), 0);
    RT_AT(4, 1237);
    if (rt_cond(rt_eq(rt_get(&F_op, v747_e), rt_s(&S95)))) {
      RT_AT(4, 1238);
      (void)({ wv t1443 = rt_get(&F_e, v747_e); wv t1444 = v763_ot; rt_chk(s481_needBool(up, rt_s(&S95), t1443, t1444), "expr"); });
      RT_AT(4, 1239);
      RT_RET(rt_def(w5_tBool, "tBool"));
    }
    RT_AT(4, 1241);
    if (rt_cond(rt_eq(rt_get(&F_op, v747_e), rt_s(&S94)))) {
      RT_AT(4, 1242);
      (void)({ wv t1445 = rt_get(&F_e, v747_e); wv t1446 = v763_ot; rt_chk(s482_needInt(up, rt_s(&S94), t1445, t1446), "expr"); });
      RT_AT(4, 1243);
      RT_RET(rt_def(w5_tInt, "tInt"));
    }
    RT_AT(4, 1245);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_chk(w5_known(v763_ot), "expr")), "||") || rt_bool(rt_eq(rt_get(&F_t, v763_ot), rt_s(&S139)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v763_ot), rt_s(&S149)), "||")))) {
      RT_AT(4, 1245);
      RT_RET(v763_ot);
    }
    RT_AT(4, 1246);
    (void)({ wv t1447 = rt_get(&F_pos, v747_e); wv t1448 = rt_cat(2, (wv[]){ rt_s(&S557), rt_text(rt_chk(w5_typeText(v763_ot), "expr")) }); rt_chk(s475_error(up, t1447, t1448, RT_NULL), "expr"); });
    RT_AT(4, 1247);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1249);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S194)))) {
    RT_AT(4, 1250);
    wv v764_lt = rt_bind(({ wv t1449 = rt_get(&F_l, v747_e); wv t1450 = v748_scope; wv t1451 = v749_ctx; rt_chk(s517_expr(up, t1449, t1450, t1451), "expr"); }), 0);
    RT_AT(4, 1251);
    RT_RET(({ wv t1455 = v747_e; wv t1456 = v764_lt; wv t1457 = ({ wv t1452 = rt_get(&F_r, v747_e); wv t1453 = v748_scope; wv t1454 = v749_ctx; rt_chk(s517_expr(up, t1452, t1453, t1454), "expr"); }); rt_chk(s485_binary(up, t1455, t1456, t1457), "expr"); }));
  }
  RT_AT(4, 1253);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S195)))) {
    RT_AT(4, 1254);
    (void)({ wv t1461 = rt_get(&F_c, v747_e); wv t1462 = ({ wv t1458 = rt_get(&F_c, v747_e); wv t1459 = v748_scope; wv t1460 = v749_ctx; rt_chk(s517_expr(up, t1458, t1459, t1460), "expr"); }); rt_chk(s480_condition(up, t1461, t1462), "expr"); });
    RT_AT(4, 1255);
    wv v765_at = rt_bind(({ wv t1463 = rt_get(&F_a, v747_e); wv t1464 = v748_scope; wv t1465 = v749_ctx; rt_chk(s517_expr(up, t1463, t1464, t1465), "expr"); }), 0);
    RT_AT(4, 1256);
    RT_RET(({ wv t1469 = v765_at; wv t1470 = ({ wv t1466 = rt_get(&F_b, v747_e); wv t1467 = v748_scope; wv t1468 = v749_ctx; rt_chk(s517_expr(up, t1466, t1467, t1468), "expr"); }); rt_chk(w5_join(t1469, t1470), "expr"); }));
  }
  RT_AT(4, 1258);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S184)))) {
    RT_AT(4, 1259);
    wv v766_types = rt_arr(0, NULL, 1);
    RT_AT(4, 1260);
    {
      wfor t1472;
      rt_for_start(&t1472, rt_get(&F_args, v747_e), 0);
      for (int64_t t1471 = 0; rt_for_guard(&t1472, rt_get(&F_args, v747_e), "e.args"), rt_for_more(&t1472, t1471); t1471++) {
        wv v767_a = rt_for_item(&t1472, t1471);
        RT_AT(4, 1260);
        (void)({ wv t1473 = v766_types; wv t1474 = rt_chk(s517_expr(up, v767_a, v748_scope, v749_ctx), "expr"); std_push(t1473, t1474); });
      }
    }
    RT_AT(4, 1261);
    RT_RET(rt_chk(s518_callee(up, v747_e, v748_scope, rt_b(1), v766_types), "expr"));
  }
  RT_AT(4, 1263);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S185)))) {
    RT_AT(4, 1264);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v747_e), rt_s(&S558), RT_NULL), "expr");
    RT_AT(4, 1265);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1267);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S45)))) {
    RT_AT(4, 1267);
    RT_RET(rt_chk(s504_checkMatch(up, v747_e, v748_scope, v749_ctx, rt_b(0)), "expr"));
  }
  RT_AT(4, 1268);
  if (rt_cond(rt_eq(v750_kind, rt_s(&S186)))) {
    RT_AT(4, 1268);
    RT_RET(rt_chk(s513_chain(up, v747_e, v748_scope, v749_ctx, rt_b(0)), "expr"));
  }
  RT_AT(4, 1269);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S559), rt_text(v750_kind) }));
  RT_RET(RT_NULL);
}

static wv s518_callee(struct fr119 *up, wv v768_e, wv v769_scope, wv v770_used, wv v771_types) {
  rt_enter("callee");
  v768_e = rt_bind(v768_e, 0);
  v769_scope = rt_bind(v769_scope, 0);
  v770_used = rt_bind(v770_used, 0);
  v771_types = rt_bind(v771_types, 0);
  RT_AT(4, 1276);
  wv v772_c = rt_bind(rt_get(&F_callee, v768_e), 0);
  RT_AT(4, 1277);
  wv v773_n = rt_bind(std_length(rt_get(&F_args, v768_e)), 0);
  RT_AT(4, 1278);
  if (rt_cond(rt_eq(rt_get(&F_k, v772_c), rt_s(&S157)))) {
    RT_AT(4, 1279);
    wv v777_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v772_c)), "callee"), 0);
    RT_AT(4, 1280);
    if (rt_cond(rt_eq(v777_sig, RT_NULL))) {
      RT_AT(4, 1280);
      (void)rt_chk(s516_unknownStd(up, v772_c), "callee");
      RT_AT(4, 1280);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1281);
    if (rt_cond(rt_eq(v777_sig, rt_s(&S35)))) {
      RT_AT(4, 1281);
      (void)({ wv t1475 = rt_get(&F_pos, v772_c); wv t1476 = rt_cat(3, (wv[]){ rt_s(&S181), rt_text(rt_get(&F_name, v772_c)), rt_s(&S560) }); rt_chk(s475_error(up, t1475, t1476, RT_NULL), "callee"); });
      RT_AT(4, 1281);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1282);
    if (rt_cond(rt_ne(v777_sig, v773_n))) {
      RT_AT(4, 1283);
      (void)({ wv t1480 = rt_get(&F_pos, v772_c); wv t1481 = ({ wv t1477 = rt_text(rt_get(&F_name, v772_c)); wv t1478 = rt_text(rt_chk(w5_plural(v777_sig), "callee")); wv t1479 = rt_text(v773_n); rt_cat(6, (wv[]){ rt_s(&S181), t1477, rt_s(&S561), t1478, rt_s(&S562), t1479 }); }); rt_chk(s475_error(up, t1480, t1481, RT_NULL), "callee"); });
      RT_AT(4, 1284);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1286);
    wv v778_wrong = rt_b(0);
    RT_AT(4, 1287);
    {
      wfor t1483;
      rt_for_start(&t1483, v771_types, 0);
      for (int64_t t1482 = 0; rt_for_guard(&t1483, v771_types, "types"), rt_for_more(&t1483, t1482); t1482++) {
        wv v779_i = rt_for_key(&t1483, t1482);
        wv v780_ty = rt_for_val(&t1483, t1482);
        RT_AT(4, 1288);
        wv v781_ok = rt_bind(({ wv t1484 = rt_get(&F_name, v772_c); wv t1485 = v779_i; rt_chk(w5_stdAccepted(t1484, t1485), "callee"); }), 0);
        RT_AT(4, 1289);
        if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v781_ok, RT_NULL), "&&") && rt_bool(rt_chk(w5_known(v780_ty), "callee"), "&&")), "&&") && rt_bool(rt_not(({ wv t1486 = v781_ok; wv t1487 = rt_get(&F_t, v780_ty); rt_chk(w1_contains(t1486, t1487), "callee"); })), "&&")))) {
          RT_AT(4, 1290);
          (void)({ wv t1493 = rt_get(&F_pos, ({ wv t1488 = v779_i; wv t1489 = rt_get(&F_args, v768_e); rt_idx(t1488, t1489); })); wv t1494 = ({ wv t1490 = rt_text(rt_get(&F_name, v772_c)); wv t1491 = rt_text(rt_chk(w5_typesText(v781_ok), "callee")); wv t1492 = rt_text(rt_chk(w5_typeText(v780_ty), "callee")); rt_cat(6, (wv[]){ rt_s(&S106), t1490, rt_s(&S563), t1491, rt_s(&S562), t1492 }); }); rt_chk(s475_error(up, t1493, t1494, RT_NULL), "callee"); });
          RT_AT(4, 1291);
          v778_wrong = rt_b(1);
        }
      }
    }
    RT_AT(4, 1294);
    if (rt_cond(v778_wrong)) {
      RT_AT(4, 1294);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1295);
    if (rt_cond(({ wv t1495 = rt_arr(4, (wv[]){ rt_s(&S353), rt_s(&S354), rt_s(&S355), rt_s(&S378) }, 0); wv t1496 = rt_get(&F_name, v772_c); rt_chk(w1_contains(t1495, t1496), "callee"); }))) {
      RT_AT(4, 1295);
      (void)({ wv t1497 = rt_idx(rt_i(0), rt_get(&F_args, v768_e)); wv t1498 = rt_idx(rt_i(0), v771_types); rt_chk(s479_noText(up, t1497, t1498), "callee"); });
    }
    RT_AT(4, 1296);
    RT_RET(({ wv t1499 = rt_get(&F_name, v772_c); wv t1500 = v771_types; rt_chk(w5_stdResult(t1499, t1500), "callee"); }));
  }
  RT_AT(4, 1298);
  wv v774_d = RT_NULL;
  RT_AT(4, 1299);
  wv v775_label = rt_s(&S13);
  RT_AT(4, 1300);
  if (rt_cond(rt_eq(rt_get(&F_k, v772_c), rt_s(&S192)))) {
    RT_AT(4, 1302);
    v775_label = ({ wv t1501 = rt_text(rt_get(&F_name, rt_get(&F_rec, v772_c))); wv t1502 = rt_text(rt_get(&F_field, v772_c)); rt_cat(3, (wv[]){ t1501, rt_s(&S130), t1502 }); });
    RT_AT(4, 1303);
    wv v782_m = rt_bind(({ wv t1503 = rt_get(&F_rec, v772_c); wv t1504 = v769_scope; rt_chk(s505_moduleNamed(up, t1503, t1504), "callee"); }), 0);
    RT_AT(4, 1304);
    if (rt_cond(rt_eq(v782_m, RT_NULL))) {
      RT_AT(4, 1304);
      (void)({ wv t1505 = rt_get(&F_rec, v772_c); wv t1506 = v769_scope; rt_chk(s509_unknownModule(up, t1505, t1506), "callee"); });
      RT_AT(4, 1304);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1305);
    v774_d = rt_chk(s508_exportOf(up, v772_c, v782_m), "callee");
    RT_AT(4, 1306);
    if (rt_cond(rt_eq(v774_d, RT_NULL))) {
      RT_AT(4, 1306);
      RT_RET(RT_NULL);
    }
  } else {
    RT_AT(4, 1308);
    v775_label = rt_bind(rt_get(&F_name, v772_c), 1);
    RT_AT(4, 1309);
    v774_d = ({ wv t1507 = v769_scope; wv t1508 = rt_get(&F_name, v772_c); rt_chk(s494_lookup(up, t1507, t1508), "callee"); });
    RT_AT(4, 1310);
    if (rt_cond(rt_eq(v774_d, RT_NULL))) {
      RT_AT(4, 1310);
      (void)({ wv t1509 = rt_get(&F_pos, v772_c); wv t1510 = rt_cat(3, (wv[]){ rt_s(&S564), rt_text(rt_get(&F_name, v772_c)), rt_s(&S114) }); rt_chk(s475_error(up, t1509, t1510, RT_NULL), "callee"); });
      RT_AT(4, 1310);
      RT_RET(RT_NULL);
    }
  }
  RT_AT(4, 1312);
  if (rt_cond(rt_ne(rt_get(&F_kind, v774_d), rt_s(&S33)))) {
    RT_AT(4, 1313);
    wv v783_hint = rt_bind((rt_cond(rt_eq(v773_n, rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S98), rt_text(v775_label), rt_s(&S565) }) : rt_s(&S566)), 0);
    RT_AT(4, 1315);
    (void)({ wv t1511 = rt_get(&F_pos, v772_c); wv t1512 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v775_label), rt_s(&S567) }); wv t1513 = v783_hint; rt_chk(s475_error(up, t1511, t1512, t1513), "callee"); });
    RT_AT(4, 1316);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1318);
  if (rt_cond(({ wv t1514 = rt_get(&F_arity, v774_d); wv t1515 = v773_n; rt_ne(t1514, t1515); }))) {
    RT_AT(4, 1318);
    (void)({ wv t1519 = rt_get(&F_pos, v772_c); wv t1520 = ({ wv t1516 = rt_text(v775_label); wv t1517 = rt_text(rt_chk(w5_plural(rt_get(&F_arity, v774_d)), "callee")); wv t1518 = rt_text(v773_n); rt_cat(6, (wv[]){ rt_s(&S114), t1516, rt_s(&S561), t1517, rt_s(&S562), t1518 }); }); rt_chk(s475_error(up, t1519, t1520, RT_NULL), "callee"); });
  }
  RT_AT(4, 1320);
  if (rt_cond(rt_b(rt_bool(v770_used, "&&") && rt_bool(rt_eq(({ wv t1521 = rt_get(&F_mod, v774_d); wv t1522 = rt_get(&F_namePos, v774_d); rt_chk(s488_firstUse(up, t1521, t1522), "callee"); }), RT_NULL), "&&")))) {
    RT_AT(4, 1321);
    (void)({ wv t1527 = rt_def(up->v468_uses, "uses"); wv t1528 = ({ wv t1523 = rt_get(&F_mod, v774_d); wv t1524 = rt_get(&F_namePos, v774_d); wv t1525 = rt_get(&F_pos, v772_c); wv t1526 = rt_def(up->v466_curMod, "curMod"); rt_rec(0, 4, K53, (wv[]){ t1523, t1524, t1525, t1526 }); }); std_push(t1527, t1528); });
  }
  RT_AT(4, 1325);
  wv v776_key = ({ wv t1529 = rt_text(rt_get(&F_mod, v774_d)); wv t1530 = rt_text(rt_get(&F_namePos, v774_d)); rt_cat(3, (wv[]){ t1529, rt_s(&S28), t1530 }); });
  RT_AT(4, 1326);
  if (rt_cond(({ wv t1531 = rt_def(up->v471_fnTypes, "fnTypes"); wv t1532 = v776_key; std_has(t1531, t1532); }))) {
    RT_AT(4, 1326);
    RT_RET(({ wv t1533 = v776_key; wv t1534 = rt_def(up->v471_fnTypes, "fnTypes"); rt_idx(t1533, t1534); }));
  }
  RT_RET((RT_AT(4, 1327), RT_NULL));
}

static wv s519_importNames(struct fr119 *up, wv v784_m, wv v785_scope) {
  rt_enter("importNames");
  v784_m = rt_bind(v784_m, 0);
  v785_scope = rt_bind(v785_scope, 0);
  RT_AT(4, 1333);
  wv v786_imports = rt_bind(rt_get(&F_imports, rt_idx(v784_m, up->v462_modules)), 0);
  RT_AT(4, 1334);
  wv v787_seen = rt_arr(0, NULL, 1);
  RT_AT(4, 1335);
  wv v788_k = rt_i(0);
  RT_AT(4, 1336);
  while (rt_cond((RT_AT(4, 1336), ({ wv t1535 = v788_k; wv t1536 = std_length(v786_imports); rt_lt(t1535, t1536); })))) {
    RT_AT(4, 1337);
    wv v789_node = rt_bind(rt_get(&F_node, rt_idx(v788_k, v786_imports)), 0);
    RT_AT(4, 1338);
    wv v790_lib = rt_bind(rt_get(&F_lib, rt_idx(v788_k, v786_imports)), 0);
    RT_AT(4, 1339);
    v788_k = rt_add(v788_k, rt_i(1));
    RT_AT(4, 1340);
    wv v791_path = rt_bind(rt_get(&F_path, rt_idx(v790_lib, up->v462_modules)), 0);
    RT_AT(4, 1341);
    wv v792_usable = rt_bind(v790_lib, 1);
    RT_AT(4, 1342);
    if (rt_cond(rt_chk(w1_contains(v787_seen, v790_lib), "importNames"))) {
      RT_AT(4, 1343);
      (void)({ wv t1537 = rt_get(&F_pathPos, v789_node); wv t1538 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v791_path), rt_s(&S568) }); rt_chk(s475_error(up, t1537, t1538, RT_NULL), "importNames"); });
      RT_AT(4, 1344);
      v792_usable = RT_NULL;
    } else if (rt_cond(rt_get(&F_notLibrary, rt_idx(v790_lib, up->v462_modules)))) {
      RT_AT(4, 1346);
      (void)({ wv t1539 = rt_get(&F_pathPos, v789_node); wv t1540 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v791_path), rt_s(&S569) }); rt_chk(s475_error(up, t1539, t1540, RT_NULL), "importNames"); });
      RT_AT(4, 1347);
      v792_usable = RT_NULL;
    }
    RT_AT(4, 1349);
    (void)std_push(v787_seen, v790_lib);
    RT_AT(4, 1350);
    wv v793_d = ({ wv t1541 = rt_get(&F_name, v789_node); wv t1542 = rt_get(&F_namePos, v789_node); wv t1543 = rt_def(up->v466_curMod, "curMod"); wv t1544 = v792_usable; rt_rec(0, 10, K47, (wv[]){ rt_s(&S510), t1541, rt_b(0), rt_b(0), t1542, RT_NULL, RT_NULL, RT_NULL, t1543, t1544 }); });
    RT_AT(4, 1354);
    (void)({ wv t1545 = v785_scope; wv t1546 = rt_get(&F_name, v789_node); wv t1547 = v793_d; rt_chk(s493_declare(up, t1545, t1546, t1547), "importNames"); });
  }
  RT_RET(RT_NULL);
}

static wv s520_libraryRules(struct fr119 *up, wv v794_m, wv v795_scope, wv v796_isEntry, wv v797_last) {
  rt_enter("libraryRules");
  v794_m = rt_bind(v794_m, 0);
  v795_scope = rt_bind(v795_scope, 0);
  v796_isEntry = rt_bind(v796_isEntry, 0);
  v797_last = rt_bind(v797_last, 0);
  RT_AT(4, 1360);
  wv v798_program = rt_bind(rt_get(&F_program, rt_idx(v794_m, up->v462_modules)), 0);
  RT_AT(4, 1361);
  wv v799_k = rt_i(0);
  RT_AT(4, 1362);
  while (rt_cond((RT_AT(4, 1362), ({ wv t1548 = v799_k; wv t1549 = std_length(rt_get(&F_stmts, v798_program)); rt_lt(t1548, t1549); })))) {
    RT_AT(4, 1363);
    wv v801_s = rt_bind(({ wv t1550 = v799_k; wv t1551 = rt_get(&F_stmts, v798_program); rt_idx(t1550, t1551); }), 0);
    RT_AT(4, 1364);
    v799_k = rt_add(v799_k, rt_i(1));
    RT_AT(4, 1366);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v801_s), rt_s(&S206)), "&&") && rt_bool(rt_b(rt_bool(v796_isEntry, "||") || rt_bool(({ wv t1552 = rt_get(&F_pos, v801_s); wv t1553 = rt_get(&F_pos, v797_last); rt_ne(t1552, t1553); }), "||")), "&&")))) {
      RT_AT(4, 1367);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v801_s), rt_s(&S570), rt_s(&S571)), "libraryRules");
    } else if (rt_cond(rt_b(rt_bool(rt_not(v796_isEntry), "&&") && rt_bool(rt_not(({ wv t1554 = rt_def(w5_libraryTop, "libraryTop"); wv t1555 = rt_get(&F_k, v801_s); rt_chk(w1_contains(t1554, t1555), "libraryRules"); })), "&&")))) {
      RT_AT(4, 1369);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v801_s), rt_s(&S572), rt_s(&S573)), "libraryRules");
    }
  }
  RT_AT(4, 1373);
  if (rt_cond(v796_isEntry)) {
    RT_AT(4, 1373);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1374);
  if (rt_cond(rt_b(rt_bool(rt_eq(v797_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v797_last), rt_s(&S206)), "||")))) {
    RT_AT(4, 1375);
    (void)rt_chk(s475_error(up, rt_get(&F_end, v798_program), rt_s(&S574), rt_s(&S575)), "libraryRules");
    RT_AT(4, 1376);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1378);
  wv v800_exports = rt_arr(0, NULL, 1);
  RT_AT(4, 1379);
  v799_k = rt_i(0);
  RT_AT(4, 1380);
  while (rt_cond((RT_AT(4, 1380), ({ wv t1556 = v799_k; wv t1557 = std_length(rt_get(&F_names, v797_last)); rt_lt(t1556, t1557); })))) {
    RT_AT(4, 1381);
    wv v802_n = rt_bind(({ wv t1558 = v799_k; wv t1559 = rt_get(&F_names, v797_last); rt_idx(t1558, t1559); }), 0);
    RT_AT(4, 1382);
    v799_k = rt_add(v799_k, rt_i(1));
    RT_AT(4, 1383);
    wv v803_d = rt_bind(({ wv t1560 = v795_scope; wv t1561 = rt_get(&F_name, v802_n); rt_chk(s492_own(up, t1560, t1561), "libraryRules"); }), 0);
    RT_AT(4, 1384);
    if (rt_cond(rt_eq(v803_d, RT_NULL))) {
      RT_AT(4, 1385);
      (void)({ wv t1562 = rt_get(&F_pos, v802_n); wv t1563 = rt_cat(3, (wv[]){ rt_s(&S576), rt_text(rt_get(&F_name, v802_n)), rt_s(&S577) }); rt_chk(s475_error(up, t1562, t1563, RT_NULL), "libraryRules"); });
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v803_d), rt_s(&S510)))) {
      RT_AT(4, 1387);
      (void)({ wv t1564 = rt_get(&F_pos, v802_n); wv t1565 = rt_cat(3, (wv[]){ rt_s(&S576), rt_text(rt_get(&F_name, v802_n)), rt_s(&S578) }); rt_chk(s475_error(up, t1564, t1565, RT_NULL), "libraryRules"); });
    } else {
      RT_AT(4, 1389);
      (void)std_push(v800_exports, v803_d);
    }
  }
  RT_AT(4, 1392);
  ({ wv t1566 = v800_exports; wv t1567 = rt_idx(v794_m, up->v462_modules); rt_set_mem(t1566, &F_exports, t1567); });
  RT_RET(RT_NULL);
}

static wv s521_checkModule(struct fr119 *up, wv v804_m, wv v805_isEntry) {
  rt_enter("checkModule");
  v804_m = rt_bind(v804_m, 0);
  v805_isEntry = rt_bind(v805_isEntry, 0);
  RT_AT(4, 1396);
  (void)rt_chk(s486_enter(up, v804_m), "checkModule");
  RT_AT(4, 1397);
  wv v806_program = rt_bind(rt_get(&F_program, rt_idx(v804_m, up->v462_modules)), 0);
  RT_AT(4, 1398);
  wv v807_last = RT_NULL;
  RT_AT(4, 1399);
  wv v808_k = rt_i(0);
  RT_AT(4, 1400);
  while (rt_cond((RT_AT(4, 1400), ({ wv t1568 = v808_k; wv t1569 = std_length(rt_get(&F_stmts, v806_program)); rt_lt(t1568, t1569); })))) {
    RT_AT(4, 1401);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1570 = v808_k; wv t1571 = rt_get(&F_stmts, v806_program); rt_idx(t1570, t1571); })), rt_s(&S203)))) {
      RT_AT(4, 1401);
      v807_last = rt_bind(({ wv t1572 = v808_k; wv t1573 = rt_get(&F_stmts, v806_program); rt_idx(t1572, t1573); }), 1);
    }
    RT_AT(4, 1402);
    v808_k = rt_add(v808_k, rt_i(1));
  }
  RT_AT(4, 1405);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v805_isEntry), "&&") && rt_bool(rt_ne(v807_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v807_last), rt_s(&S55)), "&&")))) {
    RT_AT(4, 1406);
    rt_set_mem(rt_b(1), &F_notLibrary, rt_idx(v804_m, up->v462_modules));
    RT_AT(4, 1407);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1409);
  wv v809_found = rt_map(0, NULL, 1);
  RT_AT(4, 1410);
  (void)({ wv t1574 = rt_get(&F_stmts, v806_program); wv t1575 = v809_found; rt_chk(w5_rebinds(t1574, t1575), "checkModule"); });
  RT_AT(4, 1411);
  wv t1576 = rt_bind(v809_found, 1);
  (void)rt_def(up->v473_rebound, "rebound");
  up->v473_rebound = t1576;
  RT_AT(4, 1412);
  wv v810_scope = rt_bind(rt_chk(s490_newScope(up, RT_NULL), "checkModule"), 0);
  RT_AT(4, 1413);
  (void)rt_chk(s519_importNames(up, v804_m, v810_scope), "checkModule");
  RT_AT(4, 1414);
  (void)({ wv t1579 = rt_rec(0, 2, K54, (wv[]){ rt_get(&F_stmts, v806_program), RT_NULL }); wv t1580 = v810_scope; wv t1581 = ({ wv t1577 = rt_def(up->v522_topFn, "topFn"); wv t1578 = rt_arr(0, NULL, 0); rt_rec(0, 3, K51, (wv[]){ t1577, rt_i(0), t1578 }); }); rt_chk(s496_block(up, t1579, t1580, t1581), "checkModule"); });
  RT_AT(4, 1416);
  wv v811_asLibrary = rt_b(rt_bool(rt_b(rt_bool(rt_not(up->v463_requireExit), "&&") && rt_bool(rt_ne(v807_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v807_last), rt_s(&S206)), "&&"));
  RT_AT(4, 1417);
  (void)({ wv t1582 = v804_m; wv t1583 = v810_scope; wv t1584 = rt_b(rt_bool(v805_isEntry, "&&") && rt_bool(rt_not(v811_asLibrary), "&&")); wv t1585 = v807_last; rt_chk(s520_libraryRules(up, t1582, t1583, t1584, t1585), "checkModule"); });
  RT_RET(RT_NULL);
}

static wv w6_binFn(wv v816_op) {
  rt_enter("binFn");
  v816_op = rt_bind(v816_op, 0);
  RT_AT(5, 28);
  wv v817_k = rt_i(0);
  RT_AT(5, 29);
  while (rt_cond((RT_AT(5, 29), ({ wv t1615 = ({ wv t1613 = v817_k; wv t1614 = rt_def(w6_binOps, "binOps"); rt_idx(t1613, t1614); }); wv t1616 = v816_op; rt_ne(t1615, t1616); })))) {
    RT_AT(5, 29);
    v817_k = rt_add(v817_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 30), ({ wv t1617 = v817_k; wv t1618 = rt_def(w6_binFns, "binFns"); rt_idx(t1617, t1618); })));
}

static wv w6_modeOf(wv v818_x) {
  rt_enter("modeOf");
  v818_x = rt_bind(v818_x, 0);
  RT_RET((RT_AT(5, 33), (rt_cond(rt_get(&F_isVar, v818_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v818_x)) ? rt_s(&S37) : rt_s(&S35)))));
}

static wv w6_unFn(wv v819_op) {
  rt_enter("unFn");
  v819_op = rt_bind(v819_op, 0);
  RT_RET((RT_AT(5, 35), (rt_cond(rt_eq(v819_op, rt_s(&S86))) ? rt_s(&S602) : (rt_cond(rt_eq(v819_op, rt_s(&S95))) ? rt_s(&S603) : rt_s(&S604)))));
}

static wv w6_repeat(wv v820_s, wv v821_count) {
  rt_enter("repeat");
  v820_s = rt_bind(v820_s, 0);
  v821_count = rt_bind(v821_count, 0);
  RT_AT(5, 38);
  wv v822_out = rt_s(&S13);
  RT_AT(5, 39);
  wv v823_k = rt_i(0);
  RT_AT(5, 40);
  while (rt_cond((RT_AT(5, 40), rt_lt(v823_k, v821_count)))) {
    RT_AT(5, 41);
    v822_out = rt_add(v822_out, v820_s);
    RT_AT(5, 42);
    v823_k = rt_add(v823_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 44), v822_out));
}

static wv w6_utf8Length(wv v824_s) {
  rt_enter("utf8Length");
  v824_s = rt_bind(v824_s, 0);
  RT_AT(5, 49);
  wv v825_cs = rt_bind(std_split(v824_s, rt_s(&S13)), 0);
  RT_AT(5, 50);
  wv v826_n = rt_i(0);
  RT_AT(5, 51);
  wv v827_k = rt_i(0);
  RT_AT(5, 52);
  while (rt_cond((RT_AT(5, 52), ({ wv t1619 = v827_k; wv t1620 = std_length(v825_cs); rt_lt(t1619, t1620); })))) {
    RT_AT(5, 53);
    wv v828_c = rt_bind(std_code(rt_idx(v827_k, v825_cs)), 0);
    RT_AT(5, 54);
    wv t1621 = (rt_cond(rt_lt(v828_c, rt_i(128))) ? rt_i(1) : (rt_cond(rt_lt(v828_c, rt_i(2048))) ? rt_i(2) : (rt_cond(rt_lt(v828_c, rt_i(65536))) ? rt_i(3) : rt_i(4))));
    v826_n = rt_add(v826_n, t1621);
    RT_AT(5, 55);
    v827_k = rt_add(v827_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 57), v826_n));
}

static wv w6_cString(wv v829_s) {
  rt_enter("cString");
  v829_s = rt_bind(v829_s, 0);
  RT_AT(5, 63);
  wv v830_cs = rt_bind(std_split(v829_s, rt_s(&S13)), 0);
  RT_AT(5, 64);
  wv v831_out = rt_s(&S18);
  RT_AT(5, 65);
  wv v832_k = rt_i(0);
  RT_AT(5, 66);
  while (rt_cond((RT_AT(5, 66), ({ wv t1622 = v832_k; wv t1623 = std_length(v830_cs); rt_lt(t1622, t1623); })))) {
    RT_AT(5, 67);
    wv v833_ch = rt_bind(rt_idx(v832_k, v830_cs), 0);
    RT_AT(5, 68);
    wv v834_c = rt_bind(std_code(v833_ch), 0);
    RT_AT(5, 69);
    if (rt_cond(rt_eq(v833_ch, rt_s(&S18)))) {
      RT_AT(5, 70);
      v831_out = rt_add(v831_out, rt_s(&S605));
    } else if (rt_cond(rt_eq(v833_ch, rt_s(&S19)))) {
      RT_AT(5, 72);
      v831_out = rt_add(v831_out, rt_s(&S606));
    } else if (rt_cond(rt_eq(v833_ch, rt_s(&S2)))) {
      RT_AT(5, 74);
      v831_out = rt_add(v831_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v833_ch, rt_s(&S1)))) {
      RT_AT(5, 76);
      v831_out = rt_add(v831_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v833_ch, rt_s(&S3)))) {
      RT_AT(5, 78);
      v831_out = rt_add(v831_out, rt_s(&S21));
    } else if (rt_cond(rt_b(rt_bool(rt_lt(v834_c, rt_i(32)), "||") || rt_bool(rt_eq(v834_c, rt_i(127)), "||")))) {
      RT_AT(5, 80);
      v831_out = ({ wv t1627 = v831_out; wv t1628 = ({ wv t1624 = rt_text(std_floor(rt_div(v834_c, rt_i(64)))); wv t1625 = rt_text(rt_mod(std_floor(rt_div(v834_c, rt_i(8))), rt_i(8))); wv t1626 = rt_text(rt_mod(v834_c, rt_i(8))); rt_cat(4, (wv[]){ rt_s(&S19), t1624, t1625, t1626 }); }); rt_add(t1627, t1628); });
    } else {
      RT_AT(5, 82);
      v831_out = rt_add(v831_out, v833_ch);
    }
    RT_AT(5, 84);
    v832_k = rt_add(v832_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 86), rt_add(v831_out, rt_s(&S18))));
}

static wv w6_wstrInit(wv v835_s) {
  rt_enter("wstrInit");
  v835_s = rt_bind(v835_s, 0);
  RT_AT(5, 91);
  wv v836_bytes = rt_bind(rt_chk(w6_utf8Length(v835_s), "wstrInit"), 0);
  RT_AT(5, 92);
  wv v837_cps = rt_bind(std_length(v835_s), 0);
  RT_RET((RT_AT(5, 93), ({ wv t1629 = rt_text(v836_bytes); wv t1630 = rt_text(v837_cps); wv t1631 = rt_text((rt_cond(rt_eq(v836_bytes, v837_cps)) ? rt_i(1) : rt_i(0))); wv t1632 = rt_text(rt_chk(w6_cString(v835_s), "wstrInit")); rt_cat(9, (wv[]){ rt_s(&S607), t1629, rt_s(&S419), t1630, rt_s(&S419), t1631, rt_s(&S608), t1632, rt_s(&S420) }); })));
}

static wv w6_values(wv v838_cs) {
  rt_enter("values");
  RT_RET((RT_AT(5, 97), (rt_cond(rt_eq(std_length(v838_cs), rt_i(0))) ? rt_s(&S609) : rt_cat(3, (wv[]){ rt_s(&S610), rt_text(std_join(v838_cs, rt_s(&S419))), rt_s(&S420) }))));
}

static wv w6_generateC(wv p_v839_modules, wv p_v840_runtime) {
  rt_enter("generateC");
  struct fr176 fr = { 0 };
  fr.v839_modules = rt_bind(p_v839_modules, 0);
  fr.v840_runtime = rt_bind(p_v840_runtime, 0);
  RT_AT(5, 101);
  fr.v841_n = rt_i(0);
  RT_AT(5, 102);
  fr.v842_tmps = rt_i(0);
  RT_AT(5, 103);
  fr.v843_strings = rt_arr(0, NULL, 1);
  RT_AT(5, 104);
  fr.v844_fields = rt_arr(0, NULL, 1);
  RT_AT(5, 105);
  fr.v845_shapes = rt_arr(0, NULL, 1);
  RT_AT(5, 106);
  fr.v846_enumDefs = rt_arr(0, NULL, 1);
  RT_AT(5, 107);
  fr.v847_globals = rt_arr(0, NULL, 1);
  RT_AT(5, 108);
  fr.v848_frames = rt_arr(0, NULL, 1);
  RT_AT(5, 109);
  fr.v849_functions = rt_arr(0, NULL, 1);
  RT_AT(5, 110);
  fr.v850_out = rt_arr(0, NULL, 1);
  RT_AT(5, 111);
  fr.v851_indent = rt_i(1);
  RT_AT(5, 113);
  fr.v852_loops = rt_i(0);
  RT_AT(5, 114);
  fr.v853_iife = rt_b(0);
  RT_AT(5, 120);
  fr.v854_decls = rt_arr(0, NULL, 1);
  RT_AT(5, 121);
  fr.v855_fns = rt_arr(0, NULL, 1);
  RT_AT(5, 123);
  fr.v856_tops = rt_arr(0, NULL, 1);
  RT_AT(5, 124);
  fr.v857_slots = rt_arr(0, NULL, 1);
  RT_AT(5, 128);
  fr.v858_curMod = rt_i(0);
  RT_AT(5, 129);
  fr.v859_mod = rt_s(&S13);
  RT_AT(5, 130);
  fr.v860_curSlot = rt_arr(0, NULL, 1);
  RT_AT(5, 131);
  fr.v861_stolen = rt_arr(0, NULL, 1);
  RT_AT(5, 132);
  fr.v862_ownEnums = rt_arr(0, NULL, 1);
  RT_AT(5, 133);
  fr.v863_cur = rt_i(0);
  RT_AT(5, 135);
  fr.v864_lineStarts = rt_arr(1, (wv[]){ rt_i(0) }, 1);
  RT_AT(5, 1221);
  wv v929_mi = rt_i(0);
  RT_AT(5, 1222);
  while (rt_cond((RT_AT(5, 1222), ({ wv t2700 = v929_mi; wv t2701 = std_length(fr.v839_modules); rt_lt(t2700, t2701); })))) {
    RT_AT(5, 1223);
    (void)rt_chk(s881_analyze(&fr, v929_mi), "generateC");
    RT_AT(5, 1224);
    v929_mi = rt_add(v929_mi, rt_i(1));
  }
  RT_AT(5, 1226);
  v929_mi = rt_i(0);
  RT_AT(5, 1227);
  while (rt_cond((RT_AT(5, 1227), ({ wv t2702 = v929_mi; wv t2703 = std_length(fr.v839_modules); rt_lt(t2702, t2703); })))) {
    RT_AT(5, 1228);
    wv v1239_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v929_mi, fr.v839_modules))), 0);
    RT_AT(5, 1229);
    wv v1240_li = rt_i(0);
    RT_AT(5, 1230);
    while (rt_cond((RT_AT(5, 1230), ({ wv t2704 = v1240_li; wv t2705 = std_length(v1239_list); rt_lt(t2704, t2705); })))) {
      RT_AT(5, 1231);
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1240_li, v1239_list)), rt_s(&S47)))) {
        RT_AT(5, 1231);
        (void)({ wv t2706 = rt_idx(v1240_li, v1239_list); wv t2707 = rt_chk(s867_modId(&fr, v929_mi), "generateC"); rt_chk(s887_enumDef(&fr, t2706, t2707), "generateC"); });
      }
      RT_AT(5, 1232);
      v1240_li = rt_add(v1240_li, rt_i(1));
    }
    RT_AT(5, 1234);
    v929_mi = rt_add(v929_mi, rt_i(1));
  }
  RT_AT(5, 1236);
  v929_mi = rt_i(0);
  RT_AT(5, 1237);
  while (rt_cond((RT_AT(5, 1237), ({ wv t2708 = v929_mi; wv t2709 = std_length(fr.v839_modules); rt_lt(t2708, t2709); })))) {
    RT_AT(5, 1238);
    (void)rt_chk(s882_enter(&fr, v929_mi), "generateC");
    RT_AT(5, 1239);
    (void)rt_chk(s896_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v929_mi, fr.v839_modules)))), "generateC");
    RT_AT(5, 1240);
    v929_mi = rt_add(v929_mi, rt_i(1));
  }
  RT_RET((RT_AT(5, 1242), rt_chk(s928_assemble(&fr), "generateC")));
}

static wv s865_atLine(struct fr176 *up, wv v930_pos) {
  rt_enter("atLine");
  v930_pos = rt_bind(v930_pos, 0);
  RT_AT(5, 141);
  wv v931_lo = rt_i(0);
  RT_AT(5, 142);
  wv v932_hi = rt_sub(std_length(rt_def(up->v864_lineStarts, "lineStarts")), rt_i(1));
  RT_AT(5, 143);
  while (rt_cond((RT_AT(5, 143), rt_lt(v931_lo, v932_hi)))) {
    RT_AT(5, 144);
    wv v933_mid = rt_bind(std_floor(rt_div(rt_add(rt_add(v931_lo, v932_hi), rt_i(1)), rt_i(2))), 0);
    RT_AT(5, 145);
    if (rt_cond(({ wv t1635 = ({ wv t1633 = v933_mid; wv t1634 = rt_def(up->v864_lineStarts, "lineStarts"); rt_idx(t1633, t1634); }); wv t1636 = v930_pos; rt_le(t1635, t1636); }))) {
      RT_AT(5, 145);
      v931_lo = rt_bind(v933_mid, 1);
    } else {
      RT_AT(5, 145);
      v932_hi = rt_sub(v933_mid, rt_i(1));
    }
  }
  RT_RET((RT_AT(5, 147), ({ wv t1637 = rt_text(rt_def(up->v858_curMod, "curMod")); wv t1638 = rt_text(rt_add(v931_lo, rt_i(1))); rt_cat(5, (wv[]){ rt_s(&S611), t1637, rt_s(&S419), t1638, rt_s(&S16) }); })));
}

static wv s866_atExpr(struct fr176 *up, wv v934_pos, wv v935_c) {
  rt_enter("atExpr");
  v934_pos = rt_bind(v934_pos, 0);
  v935_c = rt_bind(v935_c, 0);
  RT_RET((RT_AT(5, 150), ({ wv t1639 = rt_text(rt_chk(s865_atLine(up, v934_pos), "atExpr")); wv t1640 = rt_text(v935_c); rt_cat(5, (wv[]){ rt_s(&S98), t1639, rt_s(&S419), t1640, rt_s(&S16) }); })));
}

static wv s867_modId(struct fr176 *up, wv v936_m) {
  rt_enter("modId");
  v936_m = rt_bind(v936_m, 0);
  RT_RET((RT_AT(5, 153), (rt_cond(({ wv t1641 = v936_m; wv t1642 = rt_sub(std_length(up->v839_modules), rt_i(1)); rt_eq(t1641, t1642); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v936_m, rt_i(1))) }))));
}

static wv s868_hasFrame(struct fr176 *up, wv v937_f) {
  rt_enter("hasFrame");
  v937_f = rt_bind(v937_f, 0);
  RT_RET((RT_AT(5, 155), rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1643 = v937_f; wv t1644 = rt_def(up->v855_fns, "fns"); rt_idx(t1643, t1644); }))), "&&") && rt_bool(rt_gt(rt_get(&F_children, ({ wv t1645 = v937_f; wv t1646 = rt_def(up->v855_fns, "fns"); rt_idx(t1645, t1646); })), rt_i(0)), "&&"))));
}

static wv s869_newScope(struct fr176 *up, wv v938_parent) {
  rt_enter("newScope");
  v938_parent = rt_bind(v938_parent, 0);
  RT_AT(5, 160);
  wv v939_names = rt_arr(0, NULL, 1);
  RT_RET((RT_AT(5, 161), rt_rec(0, 2, K46, (wv[]){ v938_parent, v939_names })));
}

static wv s870_put(struct fr176 *up, wv v940_scope, wv v941_name, wv v942_kind, wv v943_d) {
  rt_enter("put");
  v940_scope = rt_bind(v940_scope, 0);
  v941_name = rt_bind(v941_name, 0);
  v942_kind = rt_bind(v942_kind, 0);
  v943_d = rt_bind(v943_d, 0);
  RT_AT(5, 163);
  (void)({ wv t1647 = rt_get(&F_names, v940_scope); wv t1648 = rt_rec(0, 3, K55, (wv[]){ v941_name, v942_kind, v943_d }); std_push(t1647, t1648); });
  RT_RET(RT_NULL);
}

static wv s871_find(struct fr176 *up, wv v944_scope, wv v945_name) {
  rt_enter("find");
  v945_name = rt_bind(v945_name, 0);
  RT_AT(5, 166);
  wv v946_s = v944_scope;
  RT_AT(5, 167);
  while (rt_cond((RT_AT(5, 167), rt_ne(v946_s, RT_NULL)))) {
    RT_AT(5, 168);
    wv v947_ns = rt_get(&F_names, v946_s);
    RT_AT(5, 169);
    wv v948_k = rt_i(0);
    RT_AT(5, 170);
    while (rt_cond((RT_AT(5, 170), ({ wv t1649 = v948_k; wv t1650 = std_length(v947_ns); rt_lt(t1649, t1650); })))) {
      RT_AT(5, 171);
      if (rt_cond(({ wv t1651 = rt_get(&F_name, rt_idx(v948_k, v947_ns)); wv t1652 = v945_name; rt_eq(t1651, t1652); }))) {
        RT_AT(5, 171);
        RT_RET(rt_idx(v948_k, v947_ns));
      }
      RT_AT(5, 172);
      v948_k = rt_add(v948_k, rt_i(1));
    }
    RT_AT(5, 174);
    v946_s = rt_get(&F_parent, v946_s);
  }
  RT_RET((RT_AT(5, 176), RT_NULL));
}

static wv s872_kindOf(struct fr176 *up, wv v949_scope, wv v950_name) {
  rt_enter("kindOf");
  v949_scope = rt_bind(v949_scope, 0);
  v950_name = rt_bind(v950_name, 0);
  RT_AT(5, 179);
  wv v951_entry = rt_bind(rt_chk(s871_find(up, v949_scope, v950_name), "kindOf"), 0);
  RT_RET((RT_AT(5, 180), (rt_cond(rt_eq(v951_entry, RT_NULL)) ? RT_NULL : rt_get(&F_kind, v951_entry))));
}

static wv s873_newFn(struct fr176 *up, wv v952_name, wv v953_parent) {
  rt_enter("newFn");
  v952_name = rt_bind(v952_name, 0);
  v953_parent = rt_bind(v953_parent, 0);
  RT_AT(5, 184);
  wv v954_f = ({ wv t1655 = v952_name; wv t1656 = v953_parent; wv t1657 = (rt_cond(rt_eq(v953_parent, RT_NULL)) ? rt_i(0) : rt_add(rt_get(&F_depth, ({ wv t1653 = v953_parent; wv t1654 = rt_def(up->v855_fns, "fns"); rt_idx(t1653, t1654); })), rt_i(1))); wv t1658 = rt_eq(v953_parent, RT_NULL); rt_rec(1, 6, K56, (wv[]){ t1655, t1656, t1657, t1658, rt_i(0), rt_s(&S13) }); });
  RT_AT(5, 188);
  (void)({ wv t1659 = rt_def(up->v855_fns, "fns"); wv t1660 = v954_f; std_push(t1659, t1660); });
  RT_RET((RT_AT(5, 189), rt_sub(std_length(rt_def(up->v855_fns, "fns")), rt_i(1))));
}

static wv s874_bind(struct fr176 *up, wv v955_kind, wv v956_name, wv v957_f, wv v958_atTop, wv v959_mode) {
  rt_enter("bind");
  v955_kind = rt_bind(v955_kind, 0);
  v956_name = rt_bind(v956_name, 0);
  v957_f = rt_bind(v957_f, 0);
  v958_atTop = rt_bind(v958_atTop, 0);
  v959_mode = rt_bind(v959_mode, 0);
  RT_AT(5, 195);
  wv v960_cname = ({ wv t1661 = rt_text(rt_def(up->v859_mod, "mod")); wv t1662 = rt_text(v956_name); rt_cat(4, (wv[]){ rt_s(&S612), t1661, rt_s(&S12), t1662 }); });
  RT_AT(5, 196);
  if (rt_cond(rt_not(v958_atTop))) {
    RT_AT(5, 197);
    v960_cname = ({ wv t1665 = (rt_cond(rt_eq(v955_kind, rt_s(&S33))) ? rt_s(&S328) : rt_s(&S613)); wv t1666 = ({ wv t1663 = rt_text(rt_def(up->v841_n, "n")); wv t1664 = rt_text(v956_name); rt_cat(3, (wv[]){ t1663, rt_s(&S12), t1664 }); }); rt_add(t1665, t1666); });
    RT_AT(5, 198);
    up->v841_n = rt_add(rt_def(up->v841_n, "n"), rt_i(1));
  }
  RT_AT(5, 200);
  wv v961_d = ({ wv t1667 = v955_kind; wv t1668 = v956_name; wv t1669 = v957_f; wv t1670 = v960_cname; wv t1671 = rt_eq(v959_mode, rt_s(&S37)); rt_rec(1, 7, K57, (wv[]){ t1667, t1668, t1669, t1670, t1671, rt_b(0), RT_NULL }); });
  RT_AT(5, 201);
  (void)({ wv t1672 = rt_def(up->v854_decls, "decls"); wv t1673 = v961_d; std_push(t1672, t1673); });
  RT_AT(5, 202);
  if (rt_cond(rt_b(rt_bool(rt_get(&F_top, ({ wv t1674 = v957_f; wv t1675 = rt_def(up->v855_fns, "fns"); rt_idx(t1674, t1675); })), "&&") && rt_bool(rt_ne(v955_kind, rt_s(&S33)), "&&")))) {
    RT_AT(5, 202);
    (void)({ wv t1676 = rt_def(up->v847_globals, "globals"); wv t1677 = v960_cname; std_push(t1676, t1677); });
  }
  RT_RET((RT_AT(5, 203), rt_sub(std_length(rt_def(up->v854_decls, "decls")), rt_i(1))));
}

static wv s875_ablock(struct fr176 *up, wv v962_b, wv v963_scope, wv v964_f, wv v965_topScope) {
  rt_enter("ablock");
  v963_scope = rt_bind(v963_scope, 0);
  v964_f = rt_bind(v964_f, 0);
  v965_topScope = rt_bind(v965_topScope, 0);
  RT_AT(5, 207);
  wv v966_atTop = rt_eq(v963_scope, v965_topScope);
  RT_AT(5, 208);
  wv v967_list = rt_bind(rt_get(&F_stmts, v962_b), 0);
  RT_AT(5, 209);
  wv v968_k = rt_i(0);
  RT_AT(5, 210);
  while (rt_cond((RT_AT(5, 210), ({ wv t1678 = v968_k; wv t1679 = std_length(v967_list); rt_lt(t1678, t1679); })))) {
    RT_AT(5, 211);
    wv v969_s = rt_bind(rt_idx(v968_k, v967_list), 0);
    RT_AT(5, 212);
    if (rt_cond(rt_eq(rt_get(&F_k, v969_s), rt_s(&S33)))) {
      RT_AT(5, 213);
      wv v970_d = rt_bind(({ wv t1680 = rt_get(&F_name, v969_s); wv t1681 = v964_f; wv t1682 = v966_atTop; rt_chk(s874_bind(up, rt_s(&S33), t1680, t1681, t1682, rt_s(&S13)), "ablock"); }), 0);
      RT_AT(5, 214);
      wv v971_child = rt_bind(({ wv t1683 = rt_get(&F_name, v969_s); wv t1684 = v964_f; rt_chk(s873_newFn(up, t1683, t1684), "ablock"); }), 0);
      RT_AT(5, 215);
      ({ wv t1689 = rt_get(&F_cname, ({ wv t1685 = v970_d; wv t1686 = rt_def(up->v854_decls, "decls"); rt_idx(t1685, t1686); })); wv t1690 = ({ wv t1687 = v971_child; wv t1688 = rt_def(up->v855_fns, "fns"); rt_idx(t1687, t1688); }); rt_set_mem(t1689, &F_cname, t1690); });
      RT_AT(5, 216);
      ({ wv t1693 = v971_child; wv t1694 = ({ wv t1691 = v970_d; wv t1692 = rt_def(up->v854_decls, "decls"); rt_idx(t1691, t1692); }); rt_set_mem(t1693, &F_fnIndex, t1694); });
      RT_AT(5, 217);
      rt_upd_mem(rt_i(1), &F_children, ({ wv t1695 = v964_f; wv t1696 = rt_def(up->v855_fns, "fns"); rt_idx(t1695, t1696); }), rt_add);
      RT_AT(5, 218);
      ({ wv t1697 = v970_d; wv t1698 = rt_get(&F_namePos, v969_s); wv t1699 = rt_def(up->v860_curSlot, "curSlot"); rt_set_idx(t1697, t1698, t1699); });
      RT_AT(5, 219);
      (void)({ wv t1700 = v963_scope; wv t1701 = rt_get(&F_name, v969_s); wv t1702 = v970_d; rt_chk(s870_put(up, t1700, t1701, rt_s(&S33), t1702), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v969_s), rt_s(&S47)))) {
      RT_AT(5, 221);
      (void)({ wv t1703 = v963_scope; wv t1704 = rt_get(&F_name, v969_s); rt_chk(s870_put(up, t1703, t1704, rt_s(&S47), RT_NULL), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v969_s), rt_s(&S179)))) {
      RT_AT(5, 223);
      wv v972_d = rt_bind(({ wv t1705 = rt_get(&F_name, v969_s); wv t1706 = v964_f; wv t1707 = v966_atTop; wv t1708 = rt_chk(w6_modeOf(v969_s), "ablock"); rt_chk(s874_bind(up, rt_s(&S179), t1705, t1706, t1707, t1708), "ablock"); }), 0);
      RT_AT(5, 224);
      ({ wv t1709 = v972_d; wv t1710 = rt_get(&F_namePos, v969_s); wv t1711 = rt_def(up->v860_curSlot, "curSlot"); rt_set_idx(t1709, t1710, t1711); });
      RT_AT(5, 225);
      (void)({ wv t1712 = v963_scope; wv t1713 = rt_get(&F_name, v969_s); wv t1714 = v972_d; rt_chk(s870_put(up, t1712, t1713, rt_s(&S179), t1714), "ablock"); });
    }
    RT_AT(5, 227);
    v968_k = rt_add(v968_k, rt_i(1));
  }
  RT_AT(5, 229);
  v968_k = rt_i(0);
  RT_AT(5, 230);
  while (rt_cond((RT_AT(5, 230), ({ wv t1715 = v968_k; wv t1716 = std_length(v967_list); rt_lt(t1715, t1716); })))) {
    RT_AT(5, 231);
    (void)({ wv t1717 = rt_idx(v968_k, v967_list); wv t1718 = v963_scope; wv t1719 = v964_f; wv t1720 = v965_topScope; rt_chk(s876_astmt(up, t1717, t1718, t1719, t1720), "ablock"); });
    RT_AT(5, 232);
    v968_k = rt_add(v968_k, rt_i(1));
  }
  RT_AT(5, 234);
  if (rt_cond(rt_ne(rt_get(&F_value, v962_b), RT_NULL))) {
    RT_AT(5, 234);
    (void)({ wv t1721 = rt_get(&F_value, v962_b); wv t1722 = v963_scope; wv t1723 = v964_f; rt_chk(s880_aexpr(up, t1721, t1722, t1723), "ablock"); });
  }
  RT_RET(RT_NULL);
}

static wv s876_astmt(struct fr176 *up, wv v973_s, wv v974_scope, wv v975_f, wv v976_topScope) {
  rt_enter("astmt");
  v974_scope = rt_bind(v974_scope, 0);
  v975_f = rt_bind(v975_f, 0);
  v976_topScope = rt_bind(v976_topScope, 0);
  RT_AT(5, 238);
  wv v977_kind = rt_bind(rt_get(&F_k, v973_s), 0);
  RT_AT(5, 239);
  if (rt_cond(rt_eq(v977_kind, rt_s(&S33)))) {
    RT_AT(5, 240);
    wv v978_d = rt_bind(({ wv t1724 = rt_get(&F_namePos, v973_s); wv t1725 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t1724, t1725); }), 0);
    RT_AT(5, 241);
    wv v979_child = rt_bind(rt_get(&F_fnIndex, ({ wv t1726 = v978_d; wv t1727 = rt_def(up->v854_decls, "decls"); rt_idx(t1726, t1727); })), 0);
    RT_AT(5, 242);
    wv v980_ps = rt_bind(rt_chk(s869_newScope(up, v974_scope), "astmt"), 0);
    RT_AT(5, 243);
    wv v981_k = rt_i(0);
    RT_AT(5, 244);
    while (rt_cond((RT_AT(5, 244), ({ wv t1728 = v981_k; wv t1729 = std_length(rt_get(&F_params, v973_s)); rt_lt(t1728, t1729); })))) {
      RT_AT(5, 245);
      wv v982_p = rt_bind(({ wv t1730 = v981_k; wv t1731 = rt_get(&F_params, v973_s); rt_idx(t1730, t1731); }), 0);
      RT_AT(5, 246);
      wv v983_pd = rt_bind(({ wv t1732 = rt_get(&F_name, v982_p); wv t1733 = v979_child; wv t1734 = rt_chk(w6_modeOf(v982_p), "astmt"); rt_chk(s874_bind(up, rt_s(&S479), t1732, t1733, rt_b(0), t1734), "astmt"); }), 0);
      RT_AT(5, 247);
      ({ wv t1735 = v983_pd; wv t1736 = rt_get(&F_pos, v982_p); wv t1737 = rt_def(up->v860_curSlot, "curSlot"); rt_set_idx(t1735, t1736, t1737); });
      RT_AT(5, 248);
      (void)({ wv t1738 = v980_ps; wv t1739 = rt_get(&F_name, v982_p); wv t1740 = v983_pd; rt_chk(s870_put(up, t1738, t1739, rt_s(&S479), t1740), "astmt"); });
      RT_AT(5, 249);
      v981_k = rt_add(v981_k, rt_i(1));
    }
    RT_AT(5, 251);
    (void)({ wv t1741 = rt_get(&F_body, v973_s); wv t1742 = rt_chk(s869_newScope(up, v980_ps), "astmt"); wv t1743 = v979_child; wv t1744 = v976_topScope; rt_chk(s875_ablock(up, t1741, t1742, t1743, t1744), "astmt"); });
  } else if (rt_cond(rt_eq(v977_kind, rt_s(&S41)))) {
    RT_AT(5, 253);
    (void)({ wv t1745 = rt_get(&F_cond, v973_s); wv t1746 = v974_scope; wv t1747 = v975_f; rt_chk(s880_aexpr(up, t1745, t1746, t1747), "astmt"); });
    RT_AT(5, 254);
    (void)({ wv t1748 = rt_get(&F_body, v973_s); wv t1749 = rt_chk(s869_newScope(up, v974_scope), "astmt"); wv t1750 = v975_f; wv t1751 = v976_topScope; rt_chk(s875_ablock(up, t1748, t1749, t1750, t1751), "astmt"); });
  } else if (rt_cond(rt_eq(v977_kind, rt_s(&S42)))) {
    RT_AT(5, 256);
    if (rt_cond(rt_eq(rt_get(&F_coll, v973_s), RT_NULL))) {
      RT_AT(5, 257);
      (void)({ wv t1752 = rt_get(&F_from, v973_s); wv t1753 = v974_scope; wv t1754 = v975_f; rt_chk(s880_aexpr(up, t1752, t1753, t1754), "astmt"); });
      RT_AT(5, 258);
      (void)({ wv t1755 = rt_get(&F_to, v973_s); wv t1756 = v974_scope; wv t1757 = v975_f; rt_chk(s880_aexpr(up, t1755, t1756, t1757), "astmt"); });
      RT_AT(5, 259);
      if (rt_cond(rt_ne(rt_get(&F_step, v973_s), RT_NULL))) {
        RT_AT(5, 259);
        (void)({ wv t1758 = rt_get(&F_step, v973_s); wv t1759 = v974_scope; wv t1760 = v975_f; rt_chk(s880_aexpr(up, t1758, t1759, t1760), "astmt"); });
      }
    } else if (rt_cond(rt_not(({ wv t1761 = rt_get(&F_coll, v973_s); wv t1762 = v974_scope; rt_chk(s879_notValue(up, t1761, t1762), "astmt"); })))) {
      RT_AT(5, 261);
      (void)({ wv t1763 = rt_get(&F_coll, v973_s); wv t1764 = v974_scope; wv t1765 = v975_f; rt_chk(s880_aexpr(up, t1763, t1764, t1765), "astmt"); });
    }
    RT_AT(5, 264);
    wv v984_ls = rt_bind(rt_chk(s869_newScope(up, v974_scope), "astmt"), 0);
    RT_AT(5, 265);
    wv v985_k = rt_i(0);
    RT_AT(5, 266);
    while (rt_cond((RT_AT(5, 266), ({ wv t1766 = v985_k; wv t1767 = std_length(rt_get(&F_names, v973_s)); rt_lt(t1766, t1767); })))) {
      RT_AT(5, 267);
      wv v986_nm = rt_bind(({ wv t1768 = v985_k; wv t1769 = rt_get(&F_names, v973_s); rt_idx(t1768, t1769); }), 0);
      RT_AT(5, 268);
      wv v987_d = rt_bind(({ wv t1770 = rt_get(&F_name, v986_nm); wv t1771 = v975_f; rt_chk(s874_bind(up, rt_s(&S179), t1770, t1771, rt_b(0), rt_s(&S37)), "astmt"); }), 0);
      RT_AT(5, 269);
      ({ wv t1772 = v987_d; wv t1773 = rt_get(&F_pos, v986_nm); wv t1774 = rt_def(up->v860_curSlot, "curSlot"); rt_set_idx(t1772, t1773, t1774); });
      RT_AT(5, 270);
      (void)({ wv t1775 = v984_ls; wv t1776 = rt_get(&F_name, v986_nm); wv t1777 = v987_d; rt_chk(s870_put(up, t1775, t1776, rt_s(&S179), t1777), "astmt"); });
      RT_AT(5, 271);
      v985_k = rt_add(v985_k, rt_i(1));
    }
    RT_AT(5, 273);
    (void)({ wv t1778 = rt_get(&F_body, v973_s); wv t1779 = rt_chk(s869_newScope(up, v984_ls), "astmt"); wv t1780 = v975_f; wv t1781 = v976_topScope; rt_chk(s875_ablock(up, t1778, t1779, t1780, t1781), "astmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v977_kind, rt_s(&S34)), "||") || rt_bool(rt_eq(v977_kind, rt_s(&S58)), "||")), "||") || rt_bool(rt_eq(v977_kind, rt_s(&S55)), "||")), "||") || rt_bool(rt_eq(v977_kind, rt_s(&S179)), "||")))) {
    RT_AT(5, 275);
    if (rt_cond(rt_ne(rt_get(&F_e, v973_s), RT_NULL))) {
      RT_AT(5, 275);
      (void)({ wv t1782 = rt_get(&F_e, v973_s); wv t1783 = v974_scope; wv t1784 = v975_f; rt_chk(s880_aexpr(up, t1782, t1783, t1784), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v977_kind, rt_s(&S178)))) {
    RT_AT(5, 277);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v973_s)), rt_s(&S185)))) {
      RT_AT(5, 277);
      (void)({ wv t1785 = rt_get(&F_arg, rt_get(&F_e, v973_s)); wv t1786 = v974_scope; wv t1787 = v975_f; rt_chk(s880_aexpr(up, t1785, t1786, t1787), "astmt"); });
    } else {
      RT_AT(5, 277);
      (void)({ wv t1788 = rt_get(&F_e, v973_s); wv t1789 = v974_scope; wv t1790 = v975_f; rt_chk(s880_aexpr(up, t1788, t1789, t1790), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v977_kind, rt_s(&S38)))) {
    RT_AT(5, 279);
    (void)({ wv t1791 = rt_get(&F_e, v973_s); wv t1792 = v974_scope; wv t1793 = v975_f; rt_chk(s880_aexpr(up, t1791, t1792, t1793), "astmt"); });
    RT_AT(5, 280);
    (void)({ wv t1794 = rt_get(&F_target, v973_s); wv t1795 = v974_scope; wv t1796 = v975_f; rt_chk(s878_atarget(up, t1794, t1795, t1796), "astmt"); });
  }
  RT_AT(5, 282);
  if (rt_cond(rt_ne(rt_get(&F_handler, v973_s), RT_NULL))) {
    RT_AT(5, 283);
    wv v988_h = rt_bind(rt_get(&F_handler, v973_s), 0);
    RT_AT(5, 284);
    wv v989_hs = rt_bind(rt_chk(s869_newScope(up, v974_scope), "astmt"), 0);
    RT_AT(5, 285);
    if (rt_cond(rt_ne(rt_get(&F_param, v988_h), RT_NULL))) {
      RT_AT(5, 286);
      wv v990_wd = rt_bind(({ wv t1797 = rt_get(&F_param, v988_h); wv t1798 = v975_f; rt_chk(s874_bind(up, rt_s(&S57), t1797, t1798, rt_b(0), rt_s(&S13)), "astmt"); }), 0);
      RT_AT(5, 287);
      ({ wv t1799 = v990_wd; wv t1800 = rt_get(&F_paramPos, v988_h); wv t1801 = rt_def(up->v860_curSlot, "curSlot"); rt_set_idx(t1799, t1800, t1801); });
      RT_AT(5, 288);
      (void)({ wv t1802 = v989_hs; wv t1803 = rt_get(&F_param, v988_h); wv t1804 = v990_wd; rt_chk(s870_put(up, t1802, t1803, rt_s(&S57), t1804), "astmt"); });
    }
    RT_AT(5, 290);
    (void)({ wv t1805 = rt_get(&F_body, v988_h); wv t1806 = rt_chk(s869_newScope(up, v989_hs), "astmt"); wv t1807 = v975_f; wv t1808 = v976_topScope; rt_chk(s875_ablock(up, t1805, t1806, t1807, t1808), "astmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s877_use(struct fr176 *up, wv v991_x, wv v992_scope, wv v993_f) {
  rt_enter("use");
  v991_x = rt_bind(v991_x, 0);
  v992_scope = rt_bind(v992_scope, 0);
  v993_f = rt_bind(v993_f, 0);
  RT_AT(5, 296);
  wv v994_d = rt_bind(rt_get(&F_d, ({ wv t1809 = v992_scope; wv t1810 = rt_get(&F_name, v991_x); rt_chk(s871_find(up, t1809, t1810), "use"); })), 0);
  RT_AT(5, 297);
  ({ wv t1811 = v994_d; wv t1812 = rt_get(&F_pos, v991_x); wv t1813 = rt_def(up->v860_curSlot, "curSlot"); rt_set_idx(t1811, t1812, t1813); });
  RT_AT(5, 298);
  if (rt_cond(({ wv t1816 = rt_get(&F_func, ({ wv t1814 = v994_d; wv t1815 = rt_def(up->v854_decls, "decls"); rt_idx(t1814, t1815); })); wv t1817 = v993_f; rt_ne(t1816, t1817); }))) {
    RT_AT(5, 298);
    rt_set_mem(rt_b(1), &F_captured, ({ wv t1818 = v994_d; wv t1819 = rt_def(up->v854_decls, "decls"); rt_idx(t1818, t1819); }));
  }
  RT_RET(RT_NULL);
}

static wv s878_atarget(struct fr176 *up, wv v995_t, wv v996_scope, wv v997_f) {
  rt_enter("atarget");
  v995_t = rt_bind(v995_t, 0);
  v996_scope = rt_bind(v996_scope, 0);
  v997_f = rt_bind(v997_f, 0);
  RT_AT(5, 302);
  if (rt_cond(rt_eq(rt_get(&F_k, v995_t), rt_s(&S214)))) {
    RT_AT(5, 302);
    (void)rt_chk(s877_use(up, v995_t, v996_scope, v997_f), "atarget");
    RT_AT(5, 302);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 303);
  if (rt_cond(rt_eq(rt_get(&F_k, v995_t), rt_s(&S191)))) {
    RT_AT(5, 303);
    (void)({ wv t1820 = rt_get(&F_index, v995_t); wv t1821 = v996_scope; wv t1822 = v997_f; rt_chk(s880_aexpr(up, t1820, t1821, t1822), "atarget"); });
  }
  RT_AT(5, 304);
  (void)({ wv t1823 = (rt_cond(rt_eq(rt_get(&F_k, v995_t), rt_s(&S192))) ? rt_get(&F_rec, v995_t) : rt_get(&F_arr, v995_t)); wv t1824 = v996_scope; wv t1825 = v997_f; rt_chk(s880_aexpr(up, t1823, t1824, t1825), "atarget"); });
  RT_RET(RT_NULL);
}

static wv s879_notValue(struct fr176 *up, wv v998_x, wv v999_scope) {
  rt_enter("notValue");
  v999_scope = rt_bind(v999_scope, 0);
  RT_AT(5, 309);
  if (rt_cond(rt_eq(rt_get(&F_k, v998_x), rt_s(&S192)))) {
    RT_AT(5, 309);
    RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v998_x)), rt_s(&S214)), "&&") && rt_bool(rt_eq(({ wv t1826 = v999_scope; wv t1827 = rt_get(&F_name, rt_get(&F_rec, v998_x)); rt_chk(s872_kindOf(up, t1826, t1827), "notValue"); }), rt_s(&S510)), "&&")));
  }
  RT_AT(5, 310);
  if (rt_cond(rt_ne(rt_get(&F_k, v998_x), rt_s(&S214)))) {
    RT_AT(5, 310);
    RT_RET(rt_b(0));
  }
  RT_AT(5, 311);
  wv v1000_kind = rt_bind(({ wv t1828 = v999_scope; wv t1829 = rt_get(&F_name, v998_x); rt_chk(s872_kindOf(up, t1828, t1829), "notValue"); }), 0);
  RT_RET((RT_AT(5, 312), rt_b(rt_bool(rt_eq(v1000_kind, rt_s(&S47)), "||") || rt_bool(rt_eq(v1000_kind, rt_s(&S510)), "||"))));
}

static wv s880_aexpr(struct fr176 *up, wv v1001_e, wv v1002_scope, wv v1003_f) {
  rt_enter("aexpr");
  v1002_scope = rt_bind(v1002_scope, 0);
  v1003_f = rt_bind(v1003_f, 0);
  RT_AT(5, 316);
  wv v1004_kind = rt_bind(rt_get(&F_k, v1001_e), 0);
  RT_AT(5, 317);
  if (rt_cond(rt_eq(v1004_kind, rt_s(&S173)))) {
    RT_AT(5, 318);
    wv v1005_k = rt_i(0);
    RT_AT(5, 319);
    while (rt_cond((RT_AT(5, 319), ({ wv t1830 = v1005_k; wv t1831 = std_length(rt_get(&F_parts, v1001_e)); rt_lt(t1830, t1831); })))) {
      RT_AT(5, 320);
      if (rt_cond(rt_ne(std_typeOf(({ wv t1832 = v1005_k; wv t1833 = rt_get(&F_parts, v1001_e); rt_idx(t1832, t1833); })), rt_s(&S187)))) {
        RT_AT(5, 320);
        (void)({ wv t1836 = ({ wv t1834 = v1005_k; wv t1835 = rt_get(&F_parts, v1001_e); rt_idx(t1834, t1835); }); wv t1837 = v1002_scope; wv t1838 = v1003_f; rt_chk(s880_aexpr(up, t1836, t1837, t1838), "aexpr"); });
      }
      RT_AT(5, 321);
      v1005_k = rt_add(v1005_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S214)))) {
    RT_AT(5, 324);
    (void)rt_chk(s877_use(up, v1001_e, v1002_scope, v1003_f), "aexpr");
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v1004_kind, rt_s(&S188)), "||") || rt_bool(rt_eq(v1004_kind, rt_s(&S189)), "||")))) {
    RT_AT(5, 326);
    wv v1006_k = rt_i(0);
    RT_AT(5, 327);
    while (rt_cond((RT_AT(5, 327), ({ wv t1839 = v1006_k; wv t1840 = std_length(rt_get(&F_items, v1001_e)); rt_lt(t1839, t1840); })))) {
      RT_AT(5, 327);
      (void)({ wv t1843 = ({ wv t1841 = v1006_k; wv t1842 = rt_get(&F_items, v1001_e); rt_idx(t1841, t1842); }); wv t1844 = v1002_scope; wv t1845 = v1003_f; rt_chk(s880_aexpr(up, t1843, t1844, t1845), "aexpr"); });
      RT_AT(5, 327);
      v1006_k = rt_add(v1006_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S190)))) {
    RT_AT(5, 329);
    wv v1007_k = rt_i(0);
    RT_AT(5, 330);
    while (rt_cond((RT_AT(5, 330), ({ wv t1846 = v1007_k; wv t1847 = std_length(rt_get(&F_fields, v1001_e)); rt_lt(t1846, t1847); })))) {
      RT_AT(5, 330);
      (void)({ wv t1850 = rt_get(&F_e, ({ wv t1848 = v1007_k; wv t1849 = rt_get(&F_fields, v1001_e); rt_idx(t1848, t1849); })); wv t1851 = v1002_scope; wv t1852 = v1003_f; rt_chk(s880_aexpr(up, t1850, t1851, t1852), "aexpr"); });
      RT_AT(5, 330);
      v1007_k = rt_add(v1007_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S191)))) {
    RT_AT(5, 332);
    (void)({ wv t1853 = rt_get(&F_index, v1001_e); wv t1854 = v1002_scope; wv t1855 = v1003_f; rt_chk(s880_aexpr(up, t1853, t1854, t1855), "aexpr"); });
    RT_AT(5, 333);
    (void)({ wv t1856 = rt_get(&F_arr, v1001_e); wv t1857 = v1002_scope; wv t1858 = v1003_f; rt_chk(s880_aexpr(up, t1856, t1857, t1858), "aexpr"); });
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S192)))) {
    RT_AT(5, 335);
    if (rt_cond(rt_not(({ wv t1859 = rt_get(&F_rec, v1001_e); wv t1860 = v1002_scope; rt_chk(s879_notValue(up, t1859, t1860), "aexpr"); })))) {
      RT_AT(5, 335);
      (void)({ wv t1861 = rt_get(&F_rec, v1001_e); wv t1862 = v1002_scope; wv t1863 = v1003_f; rt_chk(s880_aexpr(up, t1861, t1862, t1863), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S193)))) {
    RT_AT(5, 337);
    (void)({ wv t1864 = rt_get(&F_e, v1001_e); wv t1865 = v1002_scope; wv t1866 = v1003_f; rt_chk(s880_aexpr(up, t1864, t1865, t1866), "aexpr"); });
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S194)))) {
    RT_AT(5, 339);
    (void)({ wv t1867 = rt_get(&F_l, v1001_e); wv t1868 = v1002_scope; wv t1869 = v1003_f; rt_chk(s880_aexpr(up, t1867, t1868, t1869), "aexpr"); });
    RT_AT(5, 340);
    (void)({ wv t1870 = rt_get(&F_r, v1001_e); wv t1871 = v1002_scope; wv t1872 = v1003_f; rt_chk(s880_aexpr(up, t1870, t1871, t1872), "aexpr"); });
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S195)))) {
    RT_AT(5, 342);
    (void)({ wv t1873 = rt_get(&F_c, v1001_e); wv t1874 = v1002_scope; wv t1875 = v1003_f; rt_chk(s880_aexpr(up, t1873, t1874, t1875), "aexpr"); });
    RT_AT(5, 343);
    (void)({ wv t1876 = rt_get(&F_a, v1001_e); wv t1877 = v1002_scope; wv t1878 = v1003_f; rt_chk(s880_aexpr(up, t1876, t1877, t1878), "aexpr"); });
    RT_AT(5, 344);
    (void)({ wv t1879 = rt_get(&F_b, v1001_e); wv t1880 = v1002_scope; wv t1881 = v1003_f; rt_chk(s880_aexpr(up, t1879, t1880, t1881), "aexpr"); });
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S184)))) {
    RT_AT(5, 346);
    wv v1008_k = rt_i(0);
    RT_AT(5, 347);
    while (rt_cond((RT_AT(5, 347), ({ wv t1882 = v1008_k; wv t1883 = std_length(rt_get(&F_args, v1001_e)); rt_lt(t1882, t1883); })))) {
      RT_AT(5, 347);
      (void)({ wv t1886 = ({ wv t1884 = v1008_k; wv t1885 = rt_get(&F_args, v1001_e); rt_idx(t1884, t1885); }); wv t1887 = v1002_scope; wv t1888 = v1003_f; rt_chk(s880_aexpr(up, t1886, t1887, t1888), "aexpr"); });
      RT_AT(5, 347);
      v1008_k = rt_add(v1008_k, rt_i(1));
    }
    RT_AT(5, 348);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_callee, v1001_e)), rt_s(&S214)))) {
      RT_AT(5, 348);
      ({ wv t1891 = rt_get(&F_d, ({ wv t1889 = v1002_scope; wv t1890 = rt_get(&F_name, rt_get(&F_callee, v1001_e)); rt_chk(s871_find(up, t1889, t1890), "aexpr"); })); wv t1892 = rt_get(&F_pos, rt_get(&F_callee, v1001_e)); wv t1893 = rt_def(up->v860_curSlot, "curSlot"); rt_set_idx(t1891, t1892, t1893); });
    }
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S186)))) {
    RT_AT(5, 350);
    wv v1009_k = rt_i(0);
    RT_AT(5, 351);
    while (rt_cond((RT_AT(5, 351), ({ wv t1894 = v1009_k; wv t1895 = std_length(rt_get(&F_branches, v1001_e)); rt_lt(t1894, t1895); })))) {
      RT_AT(5, 352);
      (void)({ wv t1898 = rt_get(&F_cond, ({ wv t1896 = v1009_k; wv t1897 = rt_get(&F_branches, v1001_e); rt_idx(t1896, t1897); })); wv t1899 = v1002_scope; wv t1900 = v1003_f; rt_chk(s880_aexpr(up, t1898, t1899, t1900), "aexpr"); });
      RT_AT(5, 353);
      (void)({ wv t1903 = rt_get(&F_body, ({ wv t1901 = v1009_k; wv t1902 = rt_get(&F_branches, v1001_e); rt_idx(t1901, t1902); })); wv t1904 = rt_chk(s869_newScope(up, v1002_scope), "aexpr"); wv t1905 = v1003_f; rt_chk(s875_ablock(up, t1903, t1904, t1905, RT_NULL), "aexpr"); });
      RT_AT(5, 354);
      v1009_k = rt_add(v1009_k, rt_i(1));
    }
    RT_AT(5, 356);
    if (rt_cond(rt_ne(rt_get(&F_els, v1001_e), RT_NULL))) {
      RT_AT(5, 356);
      (void)({ wv t1906 = rt_get(&F_els, v1001_e); wv t1907 = rt_chk(s869_newScope(up, v1002_scope), "aexpr"); wv t1908 = v1003_f; rt_chk(s875_ablock(up, t1906, t1907, t1908, RT_NULL), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v1004_kind, rt_s(&S45)))) {
    RT_AT(5, 358);
    (void)({ wv t1909 = rt_get(&F_subject, v1001_e); wv t1910 = v1002_scope; wv t1911 = v1003_f; rt_chk(s880_aexpr(up, t1909, t1910, t1911), "aexpr"); });
    RT_AT(5, 359);
    wv v1010_k = rt_i(0);
    RT_AT(5, 360);
    while (rt_cond((RT_AT(5, 360), ({ wv t1912 = v1010_k; wv t1913 = std_length(rt_get(&F_cases, v1001_e)); rt_lt(t1912, t1913); })))) {
      RT_AT(5, 361);
      (void)({ wv t1916 = rt_get(&F_body, ({ wv t1914 = v1010_k; wv t1915 = rt_get(&F_cases, v1001_e); rt_idx(t1914, t1915); })); wv t1917 = rt_chk(s869_newScope(up, v1002_scope), "aexpr"); wv t1918 = v1003_f; rt_chk(s875_ablock(up, t1916, t1917, t1918, RT_NULL), "aexpr"); });
      RT_AT(5, 362);
      v1010_k = rt_add(v1010_k, rt_i(1));
    }
    RT_AT(5, 364);
    if (rt_cond(rt_ne(rt_get(&F_els, v1001_e), RT_NULL))) {
      RT_AT(5, 364);
      (void)({ wv t1919 = rt_get(&F_els, v1001_e); wv t1920 = rt_chk(s869_newScope(up, v1002_scope), "aexpr"); wv t1921 = v1003_f; rt_chk(s875_ablock(up, t1919, t1920, t1921, RT_NULL), "aexpr"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv s881_analyze(struct fr176 *up, wv v1011_m) {
  rt_enter("analyze");
  v1011_m = rt_bind(v1011_m, 0);
  RT_AT(5, 369);
  wv t1922 = rt_bind(v1011_m, 1);
  (void)rt_def(up->v858_curMod, "curMod");
  up->v858_curMod = t1922;
  RT_AT(5, 370);
  wv t1923 = rt_bind(rt_chk(s867_modId(up, v1011_m), "analyze"), 1);
  (void)rt_def(up->v859_mod, "mod");
  up->v859_mod = t1923;
  RT_AT(5, 371);
  wv v1012_top = rt_bind(rt_chk(s873_newFn(up, RT_NULL, RT_NULL), "analyze"), 0);
  RT_AT(5, 372);
  (void)({ wv t1924 = rt_def(up->v856_tops, "tops"); wv t1925 = v1012_top; std_push(t1924, t1925); });
  RT_AT(5, 373);
  wv v1013_slot = rt_arr(0, NULL, 1);
  RT_AT(5, 374);
  wv v1014_i = rt_i(0);
  RT_AT(5, 375);
  while (rt_cond((RT_AT(5, 375), ({ wv t1926 = v1014_i; wv t1927 = std_length(rt_get(&F_chars, rt_idx(v1011_m, up->v839_modules))); rt_le(t1926, t1927); })))) {
    RT_AT(5, 376);
    (void)std_push(v1013_slot, RT_NULL);
    RT_AT(5, 377);
    v1014_i = rt_add(v1014_i, rt_i(1));
  }
  RT_AT(5, 379);
  (void)({ wv t1928 = rt_def(up->v857_slots, "slots"); wv t1929 = v1013_slot; std_push(t1928, t1929); });
  RT_AT(5, 380);
  wv t1930 = rt_bind(v1013_slot, 1);
  (void)rt_def(up->v860_curSlot, "curSlot");
  up->v860_curSlot = t1930;
  RT_AT(5, 381);
  wv v1015_scope = rt_bind(rt_chk(s869_newScope(up, RT_NULL), "analyze"), 0);
  RT_AT(5, 382);
  wv v1016_imports = rt_bind(rt_get(&F_imports, rt_idx(v1011_m, up->v839_modules)), 0);
  RT_AT(5, 383);
  wv v1017_k = rt_i(0);
  RT_AT(5, 384);
  while (rt_cond((RT_AT(5, 384), ({ wv t1931 = v1017_k; wv t1932 = std_length(v1016_imports); rt_lt(t1931, t1932); })))) {
    RT_AT(5, 385);
    (void)({ wv t1933 = v1015_scope; wv t1934 = rt_get(&F_name, rt_get(&F_node, rt_idx(v1017_k, v1016_imports))); rt_chk(s870_put(up, t1933, t1934, rt_s(&S510), RT_NULL), "analyze"); });
    RT_AT(5, 386);
    v1017_k = rt_add(v1017_k, rt_i(1));
  }
  RT_AT(5, 388);
  (void)({ wv t1935 = rt_rec(0, 2, K54, (wv[]){ rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1011_m, up->v839_modules))), RT_NULL }); wv t1936 = v1015_scope; wv t1937 = v1012_top; wv t1938 = v1015_scope; rt_chk(s875_ablock(up, t1935, t1936, t1937, t1938), "analyze"); });
  RT_RET(RT_NULL);
}

static wv s882_enter(struct fr176 *up, wv v1018_m) {
  rt_enter("enter");
  v1018_m = rt_bind(v1018_m, 0);
  RT_AT(5, 395);
  (void)({ wv t1939 = rt_get(&F_file, rt_idx(v1018_m, up->v839_modules)); wv t1940 = rt_get(&F_chars, rt_idx(v1018_m, up->v839_modules)); rt_chk(w2_setSource(t1939, t1940), "enter"); });
  RT_AT(5, 396);
  wv t1941 = rt_bind(v1018_m, 1);
  (void)rt_def(up->v858_curMod, "curMod");
  up->v858_curMod = t1941;
  RT_AT(5, 397);
  wv t1942 = rt_bind(rt_chk(s867_modId(up, v1018_m), "enter"), 1);
  (void)rt_def(up->v859_mod, "mod");
  up->v859_mod = t1942;
  RT_AT(5, 398);
  wv t1945 = rt_bind(({ wv t1943 = v1018_m; wv t1944 = rt_def(up->v857_slots, "slots"); rt_idx(t1943, t1944); }), 1);
  (void)rt_def(up->v860_curSlot, "curSlot");
  up->v860_curSlot = t1945;
  RT_AT(5, 399);
  wv t1948 = rt_bind(({ wv t1946 = v1018_m; wv t1947 = rt_def(up->v856_tops, "tops"); rt_idx(t1946, t1947); }), 1);
  (void)rt_def(up->v863_cur, "cur");
  up->v863_cur = t1948;
  RT_AT(5, 400);
  wv t1949 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v861_stolen, "stolen");
  up->v861_stolen = t1949;
  RT_AT(5, 401);
  wv t1950 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v862_ownEnums, "ownEnums");
  up->v862_ownEnums = t1950;
  RT_AT(5, 402);
  wv t1951 = rt_arr(1, (wv[]){ rt_i(0) }, 1);
  (void)rt_def(up->v864_lineStarts, "lineStarts");
  up->v864_lineStarts = t1951;
  RT_AT(5, 403);
  wv v1019_chars = rt_bind(rt_get(&F_chars, rt_idx(v1018_m, up->v839_modules)), 0);
  RT_AT(5, 404);
  {
    wfor t1953;
    rt_for_start(&t1953, v1019_chars, 0);
    for (int64_t t1952 = 0; rt_for_guard(&t1953, v1019_chars, "chars"), rt_for_more(&t1953, t1952); t1952++) {
      wv v1023_i = rt_for_key(&t1953, t1952);
      wv v1024_c = rt_for_val(&t1953, t1952);
      RT_AT(5, 405);
      if (rt_cond(rt_eq(v1024_c, rt_s(&S2)))) {
        RT_AT(5, 405);
        (void)({ wv t1954 = rt_def(up->v864_lineStarts, "lineStarts"); wv t1955 = rt_add(v1023_i, rt_i(1)); std_push(t1954, t1955); });
      }
    }
  }
  RT_AT(5, 407);
  wv v1020_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1018_m, up->v839_modules))), 0);
  RT_AT(5, 408);
  wv v1021_k = rt_i(0);
  RT_AT(5, 409);
  while (rt_cond((RT_AT(5, 409), ({ wv t1956 = v1021_k; wv t1957 = std_length(v1020_list); rt_lt(t1956, t1957); })))) {
    RT_AT(5, 410);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1021_k, v1020_list)), rt_s(&S47)))) {
      RT_AT(5, 410);
      (void)({ wv t1958 = rt_def(up->v862_ownEnums, "ownEnums"); wv t1959 = rt_idx(v1021_k, v1020_list); std_push(t1958, t1959); });
    }
    RT_AT(5, 411);
    v1021_k = rt_add(v1021_k, rt_i(1));
  }
  RT_AT(5, 413);
  wv v1022_imports = rt_bind(rt_get(&F_imports, rt_idx(v1018_m, up->v839_modules)), 0);
  RT_AT(5, 414);
  v1021_k = rt_i(0);
  RT_AT(5, 415);
  while (rt_cond((RT_AT(5, 415), ({ wv t1960 = v1021_k; wv t1961 = std_length(v1022_imports); rt_lt(t1960, t1961); })))) {
    RT_AT(5, 416);
    (void)({ wv t1964 = rt_def(up->v861_stolen, "stolen"); wv t1965 = ({ wv t1962 = rt_get(&F_name, rt_get(&F_node, rt_idx(v1021_k, v1022_imports))); wv t1963 = rt_get(&F_lib, rt_idx(v1021_k, v1022_imports)); rt_rec(0, 2, K58, (wv[]){ t1962, t1963 }); }); std_push(t1964, t1965); });
    RT_AT(5, 417);
    v1021_k = rt_add(v1021_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s883_emit(struct fr176 *up, wv v1025_line) {
  rt_enter("emit");
  v1025_line = rt_bind(v1025_line, 0);
  RT_AT(5, 421);
  (void)({ wv t1968 = rt_def(up->v850_out, "out"); wv t1969 = ({ wv t1966 = rt_chk(w6_repeat(rt_s(&S614), rt_def(up->v851_indent, "indent")), "emit"); wv t1967 = v1025_line; rt_add(t1966, t1967); }); std_push(t1968, t1969); });
  RT_RET(RT_NULL);
}

static wv s884_tmp(struct fr176 *up) {
  rt_enter("tmp");
  RT_AT(5, 423);
  wv v1026_name = rt_cat(2, (wv[]){ rt_s(&S105), rt_text(rt_def(up->v842_tmps, "tmps")) });
  RT_AT(5, 424);
  up->v842_tmps = rt_add(rt_def(up->v842_tmps, "tmps"), rt_i(1));
  RT_RET((RT_AT(5, 425), v1026_name));
}

static wv s885_str(struct fr176 *up, wv v1027_s) {
  rt_enter("str");
  v1027_s = rt_bind(v1027_s, 0);
  RT_AT(5, 429);
  wv v1028_k = rt_i(0);
  RT_AT(5, 430);
  while (rt_cond((RT_AT(5, 430), rt_b(rt_bool(({ wv t1970 = v1028_k; wv t1971 = std_length(rt_def(up->v843_strings, "strings")); rt_lt(t1970, t1971); }), "&&") && rt_bool(({ wv t1974 = ({ wv t1972 = v1028_k; wv t1973 = rt_def(up->v843_strings, "strings"); rt_idx(t1972, t1973); }); wv t1975 = v1027_s; rt_ne(t1974, t1975); }), "&&"))))) {
    RT_AT(5, 430);
    v1028_k = rt_add(v1028_k, rt_i(1));
  }
  RT_AT(5, 431);
  if (rt_cond(({ wv t1976 = v1028_k; wv t1977 = std_length(rt_def(up->v843_strings, "strings")); rt_eq(t1976, t1977); }))) {
    RT_AT(5, 431);
    (void)({ wv t1978 = rt_def(up->v843_strings, "strings"); wv t1979 = v1027_s; std_push(t1978, t1979); });
  }
  RT_RET((RT_AT(5, 432), rt_cat(2, (wv[]){ rt_s(&S615), rt_text(v1028_k) })));
}

static wv s886_fieldRef(struct fr176 *up, wv v1029_name) {
  rt_enter("fieldRef");
  v1029_name = rt_bind(v1029_name, 0);
  RT_AT(5, 435);
  if (rt_cond(rt_not(({ wv t1980 = rt_def(up->v844_fields, "fields"); wv t1981 = v1029_name; rt_chk(w1_contains(t1980, t1981), "fieldRef"); })))) {
    RT_AT(5, 435);
    (void)({ wv t1982 = rt_def(up->v844_fields, "fields"); wv t1983 = v1029_name; std_push(t1982, t1983); });
  }
  RT_RET((RT_AT(5, 436), rt_cat(2, (wv[]){ rt_s(&S616), rt_text(v1029_name) })));
}

static wv s887_enumDef(struct fr176 *up, wv v1030_s, wv v1031_m) {
  rt_enter("enumDef");
  v1030_s = rt_bind(v1030_s, 0);
  v1031_m = rt_bind(v1031_m, 0);
  RT_AT(5, 441);
  wv v1032_nm = rt_bind(rt_chk(s885_str(up, rt_get(&F_name, v1030_s)), "enumDef"), 0);
  RT_AT(5, 442);
  wv v1033_members = rt_arr(0, NULL, 1);
  RT_AT(5, 443);
  wv v1034_k = rt_i(0);
  RT_AT(5, 444);
  while (rt_cond((RT_AT(5, 444), ({ wv t1984 = v1034_k; wv t1985 = std_length(rt_get(&F_members, v1030_s)); rt_lt(t1984, t1985); })))) {
    RT_AT(5, 445);
    (void)({ wv t1990 = v1033_members; wv t1991 = ({ wv t1988 = rt_text(v1032_nm); wv t1989 = rt_text(rt_chk(s885_str(up, rt_get(&F_name, ({ wv t1986 = v1034_k; wv t1987 = rt_get(&F_members, v1030_s); rt_idx(t1986, t1987); }))), "enumDef")); rt_cat(5, (wv[]){ rt_s(&S607), t1988, rt_s(&S419), t1989, rt_s(&S420) }); }); std_push(t1990, t1991); });
    RT_AT(5, 446);
    v1034_k = rt_add(v1034_k, rt_i(1));
  }
  RT_AT(5, 448);
  (void)({ wv t1995 = rt_def(up->v846_enumDefs, "enumDefs"); wv t1996 = ({ wv t1992 = rt_text(v1031_m); wv t1993 = rt_text(rt_get(&F_name, v1030_s)); wv t1994 = rt_text(std_join(v1033_members, rt_s(&S419))); rt_cat(7, (wv[]){ rt_s(&S617), t1992, rt_s(&S12), t1993, rt_s(&S618), t1994, rt_s(&S619) }); }); std_push(t1995, t1996); });
  RT_RET(RT_NULL);
}

static wv s888_stolenFrom(struct fr176 *up, wv v1035_x) {
  rt_enter("stolenFrom");
  RT_AT(5, 453);
  if (rt_cond(rt_ne(rt_get(&F_k, v1035_x), rt_s(&S214)))) {
    RT_AT(5, 453);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 454);
  wv v1036_k = rt_i(0);
  RT_AT(5, 455);
  while (rt_cond((RT_AT(5, 455), ({ wv t1997 = v1036_k; wv t1998 = std_length(rt_def(up->v861_stolen, "stolen")); rt_lt(t1997, t1998); })))) {
    RT_AT(5, 456);
    if (rt_cond(({ wv t2001 = rt_get(&F_name, ({ wv t1999 = v1036_k; wv t2000 = rt_def(up->v861_stolen, "stolen"); rt_idx(t1999, t2000); })); wv t2002 = rt_get(&F_name, v1035_x); rt_eq(t2001, t2002); }))) {
      RT_AT(5, 456);
      RT_RET(rt_get(&F_lib, ({ wv t2003 = v1036_k; wv t2004 = rt_def(up->v861_stolen, "stolen"); rt_idx(t2003, t2004); })));
    }
    RT_AT(5, 457);
    v1036_k = rt_add(v1036_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 459), RT_NULL));
}

static wv s889_enumIn(struct fr176 *up, wv v1037_list, wv v1038_name) {
  rt_enter("enumIn");
  v1038_name = rt_bind(v1038_name, 0);
  RT_AT(5, 464);
  wv v1039_k = rt_i(0);
  RT_AT(5, 465);
  while (rt_cond((RT_AT(5, 465), ({ wv t2005 = v1039_k; wv t2006 = std_length(v1037_list); rt_lt(t2005, t2006); })))) {
    RT_AT(5, 466);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_idx(v1039_k, v1037_list)), rt_s(&S47)), "&&") && rt_bool(({ wv t2007 = rt_get(&F_name, rt_idx(v1039_k, v1037_list)); wv t2008 = v1038_name; rt_eq(t2007, t2008); }), "&&")))) {
      RT_AT(5, 466);
      RT_RET(rt_idx(v1039_k, v1037_list));
    }
    RT_AT(5, 467);
    v1039_k = rt_add(v1039_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 469), RT_NULL));
}

static wv s890_enumOf(struct fr176 *up, wv v1040_x) {
  rt_enter("enumOf");
  RT_AT(5, 474);
  if (rt_cond(rt_eq(rt_get(&F_k, v1040_x), rt_s(&S214)))) {
    RT_AT(5, 475);
    wv v1044_s = rt_bind(({ wv t2009 = rt_def(up->v862_ownEnums, "ownEnums"); wv t2010 = rt_get(&F_name, v1040_x); rt_chk(s889_enumIn(up, t2009, t2010), "enumOf"); }), 0);
    RT_AT(5, 476);
    RT_RET((rt_cond(rt_eq(v1044_s, RT_NULL)) ? RT_NULL : ({ wv t2013 = ({ wv t2011 = rt_text(rt_def(up->v859_mod, "mod")); wv t2012 = rt_text(rt_get(&F_name, v1040_x)); rt_cat(4, (wv[]){ rt_s(&S620), t2011, rt_s(&S12), t2012 }); }); wv t2014 = v1044_s; rt_rec(0, 2, K59, (wv[]){ t2013, t2014 }); })));
  }
  RT_AT(5, 478);
  if (rt_cond(rt_ne(rt_get(&F_k, v1040_x), rt_s(&S192)))) {
    RT_AT(5, 478);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 479);
  wv v1041_lib = rt_bind(rt_chk(s888_stolenFrom(up, rt_get(&F_rec, v1040_x)), "enumOf"), 0);
  RT_AT(5, 480);
  if (rt_cond(rt_eq(v1041_lib, RT_NULL))) {
    RT_AT(5, 480);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 481);
  wv v1042_xs = rt_bind(rt_get(&F_exports, rt_idx(v1041_lib, up->v839_modules)), 0);
  RT_AT(5, 482);
  wv v1043_k = rt_i(0);
  RT_AT(5, 483);
  while (rt_cond((RT_AT(5, 483), rt_b(rt_bool(({ wv t2015 = v1043_k; wv t2016 = std_length(v1042_xs); rt_lt(t2015, t2016); }), "&&") && rt_bool(({ wv t2017 = rt_get(&F_name, rt_idx(v1043_k, v1042_xs)); wv t2018 = rt_get(&F_field, v1040_x); rt_ne(t2017, t2018); }), "&&"))))) {
    RT_AT(5, 483);
    v1043_k = rt_add(v1043_k, rt_i(1));
  }
  RT_AT(5, 484);
  if (rt_cond(rt_b(rt_bool(({ wv t2019 = v1043_k; wv t2020 = std_length(v1042_xs); rt_eq(t2019, t2020); }), "||") || rt_bool(rt_ne(rt_get(&F_kind, rt_idx(v1043_k, v1042_xs)), rt_s(&S47)), "||")))) {
    RT_AT(5, 484);
    RT_RET(RT_NULL);
  }
  RT_RET((RT_AT(5, 485), ({ wv t2025 = ({ wv t2021 = rt_text(rt_chk(s867_modId(up, v1041_lib), "enumOf")); wv t2022 = rt_text(rt_get(&F_field, v1040_x)); rt_cat(4, (wv[]){ rt_s(&S620), t2021, rt_s(&S12), t2022 }); }); wv t2026 = ({ wv t2023 = rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1041_lib, up->v839_modules))); wv t2024 = rt_get(&F_field, v1040_x); rt_chk(s889_enumIn(up, t2023, t2024), "enumOf"); }); rt_rec(0, 2, K59, (wv[]){ t2025, t2026 }); })));
}

static wv s891_place(struct fr176 *up, wv v1045_d) {
  rt_enter("place");
  v1045_d = rt_bind(v1045_d, 0);
  RT_AT(5, 490);
  wv v1046_owner = rt_bind(rt_get(&F_func, ({ wv t2027 = v1045_d; wv t2028 = rt_def(up->v854_decls, "decls"); rt_idx(t2027, t2028); })), 0);
  RT_AT(5, 491);
  if (rt_cond(rt_get(&F_top, ({ wv t2029 = v1046_owner; wv t2030 = rt_def(up->v855_fns, "fns"); rt_idx(t2029, t2030); })))) {
    RT_AT(5, 491);
    RT_RET(rt_get(&F_cname, ({ wv t2031 = v1045_d; wv t2032 = rt_def(up->v854_decls, "decls"); rt_idx(t2031, t2032); })));
  }
  RT_AT(5, 492);
  if (rt_cond(({ wv t2033 = v1046_owner; wv t2034 = rt_def(up->v863_cur, "cur"); rt_eq(t2033, t2034); }))) {
    RT_AT(5, 492);
    RT_RET((rt_cond(rt_get(&F_captured, ({ wv t2035 = v1045_d; wv t2036 = rt_def(up->v854_decls, "decls"); rt_idx(t2035, t2036); }))) ? rt_cat(2, (wv[]){ rt_s(&S621), rt_text(rt_get(&F_cname, ({ wv t2037 = v1045_d; wv t2038 = rt_def(up->v854_decls, "decls"); rt_idx(t2037, t2038); }))) }) : rt_get(&F_cname, ({ wv t2039 = v1045_d; wv t2040 = rt_def(up->v854_decls, "decls"); rt_idx(t2039, t2040); }))));
  }
  RT_RET((RT_AT(5, 493), ({ wv t2049 = rt_text(rt_chk(w6_repeat(rt_s(&S623), rt_sub(({ wv t2045 = rt_get(&F_depth, ({ wv t2041 = rt_def(up->v863_cur, "cur"); wv t2042 = rt_def(up->v855_fns, "fns"); rt_idx(t2041, t2042); })); wv t2046 = rt_get(&F_depth, ({ wv t2043 = v1046_owner; wv t2044 = rt_def(up->v855_fns, "fns"); rt_idx(t2043, t2044); })); rt_sub(t2045, t2046); }), rt_i(1))), "place")); wv t2050 = rt_text(rt_get(&F_cname, ({ wv t2047 = v1045_d; wv t2048 = rt_def(up->v854_decls, "decls"); rt_idx(t2047, t2048); }))); rt_cat(4, (wv[]){ rt_s(&S622), t2049, rt_s(&S624), t2050 }); })));
}

static wv s892_tdz(struct fr176 *up, wv v1047_d) {
  rt_enter("tdz");
  v1047_d = rt_bind(v1047_d, 0);
  RT_RET((RT_AT(5, 496), rt_b(rt_bool(rt_eq(rt_get(&F_kind, ({ wv t2051 = v1047_d; wv t2052 = rt_def(up->v854_decls, "decls"); rt_idx(t2051, t2052); })), rt_s(&S179)), "&&") && rt_bool(({ wv t2055 = rt_get(&F_func, ({ wv t2053 = v1047_d; wv t2054 = rt_def(up->v854_decls, "decls"); rt_idx(t2053, t2054); })); wv t2056 = rt_def(up->v863_cur, "cur"); rt_ne(t2055, t2056); }), "&&"))));
}

static wv s893_read(struct fr176 *up, wv v1048_d) {
  rt_enter("read");
  v1048_d = rt_bind(v1048_d, 0);
  RT_RET((RT_AT(5, 498), (rt_cond(rt_chk(s892_tdz(up, v1048_d), "read")) ? ({ wv t2061 = ({ wv t2059 = rt_text(rt_chk(s891_place(up, v1048_d), "read")); wv t2060 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t2057 = v1048_d; wv t2058 = rt_def(up->v854_decls, "decls"); rt_idx(t2057, t2058); }))), "read")); rt_cat(5, (wv[]){ rt_s(&S625), t2059, rt_s(&S419), t2060, rt_s(&S16) }); }); wv t2062 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2061, t2062 }); }) : ({ wv t2063 = rt_chk(s891_place(up, v1048_d), "read"); wv t2064 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K60, (wv[]){ t2063, t2064 }); }))));
}

static wv s894_seq(struct fr176 *up, wv v1049_args) {
  rt_enter("seq");
  v1049_args = rt_bind(v1049_args, 0);
  RT_AT(5, 505);
  wv v1050_effects = rt_i(0);
  RT_AT(5, 506);
  wv v1051_anyPure = rt_b(0);
  RT_AT(5, 507);
  wv v1052_k = rt_i(0);
  RT_AT(5, 508);
  while (rt_cond((RT_AT(5, 508), ({ wv t2065 = v1052_k; wv t2066 = std_length(v1049_args); rt_lt(t2065, t2066); })))) {
    RT_AT(5, 509);
    if (rt_cond(({ wv t2067 = rt_get(&F_k, rt_idx(v1052_k, v1049_args)); wv t2068 = rt_def(w6_effectK, "effectK"); rt_eq(t2067, t2068); }))) {
      RT_AT(5, 509);
      v1050_effects = rt_add(v1050_effects, rt_i(1));
    }
    RT_AT(5, 510);
    if (rt_cond(({ wv t2069 = rt_get(&F_k, rt_idx(v1052_k, v1049_args)); wv t2070 = rt_def(w6_pureK, "pureK"); rt_eq(t2069, t2070); }))) {
      RT_AT(5, 510);
      v1051_anyPure = rt_b(1);
    }
    RT_AT(5, 511);
    v1052_k = rt_add(v1052_k, rt_i(1));
  }
  RT_AT(5, 513);
  wv v1053_cs = rt_arr(0, NULL, 1);
  RT_AT(5, 514);
  if (rt_cond(rt_b(rt_bool(rt_eq(v1050_effects, rt_i(0)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1050_effects, rt_i(1)), "&&") && rt_bool(rt_not(v1051_anyPure), "&&")), "||")))) {
    RT_AT(5, 515);
    v1052_k = rt_i(0);
    RT_AT(5, 516);
    while (rt_cond((RT_AT(5, 516), ({ wv t2071 = v1052_k; wv t2072 = std_length(v1049_args); rt_lt(t2071, t2072); })))) {
      RT_AT(5, 516);
      (void)({ wv t2073 = v1053_cs; wv t2074 = rt_get(&F_c, rt_idx(v1052_k, v1049_args)); std_push(t2073, t2074); });
      RT_AT(5, 516);
      v1052_k = rt_add(v1052_k, rt_i(1));
    }
    RT_AT(5, 517);
    RT_RET(rt_rec(0, 2, K61, (wv[]){ rt_s(&S13), v1053_cs }));
  }
  RT_AT(5, 519);
  wv v1054_pre = rt_arr(0, NULL, 1);
  RT_AT(5, 520);
  v1052_k = rt_i(0);
  RT_AT(5, 521);
  while (rt_cond((RT_AT(5, 521), ({ wv t2075 = v1052_k; wv t2076 = std_length(v1049_args); rt_lt(t2075, t2076); })))) {
    RT_AT(5, 522);
    if (rt_cond(({ wv t2077 = rt_get(&F_k, rt_idx(v1052_k, v1049_args)); wv t2078 = rt_def(w6_constK, "constK"); rt_eq(t2077, t2078); }))) {
      RT_AT(5, 523);
      (void)({ wv t2079 = v1053_cs; wv t2080 = rt_get(&F_c, rt_idx(v1052_k, v1049_args)); std_push(t2079, t2080); });
    } else {
      RT_AT(5, 525);
      wv v1055_temp = rt_bind(rt_chk(s884_tmp(up), "seq"), 0);
      RT_AT(5, 526);
      (void)({ wv t2083 = v1054_pre; wv t2084 = ({ wv t2081 = rt_text(v1055_temp); wv t2082 = rt_text(rt_get(&F_c, rt_idx(v1052_k, v1049_args))); rt_cat(5, (wv[]){ rt_s(&S626), t2081, rt_s(&S627), t2082, rt_s(&S14) }); }); std_push(t2083, t2084); });
      RT_AT(5, 527);
      (void)std_push(v1053_cs, v1055_temp);
    }
    RT_AT(5, 529);
    v1052_k = rt_add(v1052_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 531), ({ wv t2085 = std_join(v1054_pre, rt_s(&S0)); wv t2086 = v1053_cs; rt_rec(0, 2, K61, (wv[]){ t2085, t2086 }); })));
}

static wv s895_wrap(struct fr176 *up, wv v1056_sq, wv v1057_inner) {
  rt_enter("wrap");
  v1056_sq = rt_bind(v1056_sq, 0);
  v1057_inner = rt_bind(v1057_inner, 0);
  RT_RET((RT_AT(5, 533), ({ wv t2089 = (rt_cond(rt_eq(rt_get(&F_pre, v1056_sq), rt_s(&S13))) ? v1057_inner : ({ wv t2087 = rt_text(rt_get(&F_pre, v1056_sq)); wv t2088 = rt_text(v1057_inner); rt_cat(5, (wv[]){ rt_s(&S628), t2087, rt_s(&S0), t2088, rt_s(&S629) }); })); wv t2090 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2089, t2090 }); })));
}

static wv s896_stmts(struct fr176 *up, wv v1058_list) {
  rt_enter("stmts");
  v1058_list = rt_bind(v1058_list, 0);
  RT_AT(5, 536);
  wv v1059_k = rt_i(0);
  RT_AT(5, 537);
  while (rt_cond((RT_AT(5, 537), ({ wv t2091 = v1059_k; wv t2092 = std_length(v1058_list); rt_lt(t2091, t2092); })))) {
    RT_AT(5, 538);
    (void)rt_chk(s900_stmt(up, rt_idx(v1059_k, v1058_list)), "stmts");
    RT_AT(5, 539);
    v1059_k = rt_add(v1059_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s897_resets(struct fr176 *up, wv v1060_b) {
  rt_enter("resets");
  RT_AT(5, 546);
  wv v1061_k = rt_i(0);
  RT_AT(5, 547);
  while (rt_cond((RT_AT(5, 547), ({ wv t2093 = v1061_k; wv t2094 = std_length(rt_get(&F_stmts, v1060_b)); rt_lt(t2093, t2094); })))) {
    RT_AT(5, 548);
    wv v1062_s = ({ wv t2095 = v1061_k; wv t2096 = rt_get(&F_stmts, v1060_b); rt_idx(t2095, t2096); });
    RT_AT(5, 549);
    if (rt_cond(rt_eq(rt_get(&F_k, v1062_s), rt_s(&S179)))) {
      RT_AT(5, 550);
      wv v1063_d = rt_bind(({ wv t2097 = rt_get(&F_namePos, v1062_s); wv t2098 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2097, t2098); }), 0);
      RT_AT(5, 551);
      if (rt_cond(rt_get(&F_captured, ({ wv t2099 = v1063_d; wv t2100 = rt_def(up->v854_decls, "decls"); rt_idx(t2099, t2100); })))) {
        RT_AT(5, 551);
        (void)rt_chk(s883_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s891_place(up, v1063_d), "resets")), rt_s(&S630) })), "resets");
      }
    }
    RT_AT(5, 553);
    v1061_k = rt_add(v1061_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s898_blockBody(struct fr176 *up, wv v1064_b) {
  rt_enter("blockBody");
  v1064_b = rt_bind(v1064_b, 0);
  RT_AT(5, 559);
  (void)rt_chk(s897_resets(up, v1064_b), "blockBody");
  RT_AT(5, 560);
  (void)rt_chk(s896_stmts(up, rt_get(&F_stmts, v1064_b)), "blockBody");
  RT_AT(5, 561);
  if (rt_cond(rt_ne(rt_get(&F_value, v1064_b), RT_NULL))) {
    RT_AT(5, 561);
    (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S631), rt_text(rt_chk(s899_valueOf(up, v1064_b), "blockBody")), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s899_valueOf(struct fr176 *up, wv v1065_b) {
  rt_enter("valueOf");
  v1065_b = rt_bind(v1065_b, 0);
  RT_RET((RT_AT(5, 565), (rt_cond(rt_eq(rt_get(&F_value, v1065_b), RT_NULL)) ? rt_s(&S632) : ({ wv t2101 = rt_get(&F_pos, rt_get(&F_value, v1065_b)); wv t2102 = rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_value, v1065_b)), "valueOf")); rt_chk(s866_atExpr(up, t2101, t2102), "valueOf"); }))));
}

static wv s900_stmt(struct fr176 *up, wv v1066_s) {
  rt_enter("stmt");
  RT_AT(5, 568);
  wv v1067_kind = rt_bind(rt_get(&F_k, v1066_s), 0);
  RT_AT(5, 569);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1067_kind, rt_s(&S203)), "||") || rt_bool(rt_eq(v1067_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v1067_kind, rt_s(&S48)), "||")), "||") || rt_bool(rt_eq(v1067_kind, rt_s(&S206)), "||")))) {
    RT_AT(5, 569);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 570);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S33)))) {
    RT_AT(5, 570);
    (void)rt_chk(s905_fnDef(up, v1066_s), "stmt");
    RT_AT(5, 570);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 571);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1067_kind, rt_s(&S43)), "&&") && rt_bool(rt_ne(v1067_kind, rt_s(&S44)), "&&")))) {
    RT_AT(5, 571);
    (void)rt_chk(s883_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s865_atLine(up, rt_get(&F_pos, v1066_s)), "stmt")), rt_s(&S14) })), "stmt");
  }
  RT_AT(5, 572);
  if (rt_cond(rt_ne(rt_get(&F_handler, v1066_s), RT_NULL))) {
    RT_AT(5, 572);
    (void)rt_chk(s912_onfailStmt(up, v1066_s), "stmt");
    RT_AT(5, 572);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 573);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S34)))) {
    RT_AT(5, 574);
    if (rt_cond(rt_def(up->v853_iife, "iife"))) {
      RT_AT(5, 574);
      (void)rt_chk(w2_abort(rt_get(&F_pos, v1066_s), rt_s(&S633)), "stmt");
    }
    RT_AT(5, 575);
    (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S634), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1066_s), RT_NULL)) ? rt_s(&S632) : rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_e, v1066_s)), "stmt")))), rt_s(&S635) })), "stmt");
    RT_AT(5, 576);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 578);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S58)))) {
    RT_AT(5, 578);
    (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S636), rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_e, v1066_s)), "stmt"))), rt_s(&S635) })), "stmt");
    RT_AT(5, 578);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 579);
  if (rt_cond(rt_b(rt_bool(rt_eq(v1067_kind, rt_s(&S43)), "||") || rt_bool(rt_eq(v1067_kind, rt_s(&S44)), "||")))) {
    RT_AT(5, 580);
    if (rt_cond(rt_eq(rt_def(up->v852_loops, "loops"), rt_i(0)))) {
      RT_AT(5, 580);
      (void)({ wv t2103 = rt_get(&F_pos, v1066_s); wv t2104 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text((rt_cond(rt_eq(v1067_kind, rt_s(&S43))) ? rt_s(&S43) : rt_s(&S44))), rt_s(&S637) }); rt_chk(w2_abort(t2103, t2104), "stmt"); });
    }
    RT_AT(5, 581);
    (void)rt_chk(s883_emit(up, (rt_cond(rt_eq(v1067_kind, rt_s(&S43))) ? rt_s(&S638) : rt_s(&S639))), "stmt");
    RT_AT(5, 582);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 584);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S41)))) {
    RT_AT(5, 586);
    (void)rt_chk(s883_emit(up, ({ wv t2107 = rt_text((rt_cond(rt_get(&F_neg, v1066_s)) ? rt_s(&S95) : rt_s(&S13))); wv t2108 = rt_text(({ wv t2105 = rt_get(&F_pos, rt_get(&F_cond, v1066_s)); wv t2106 = rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_cond, v1066_s)), "stmt")); rt_chk(s866_atExpr(up, t2105, t2106), "stmt"); })); rt_cat(5, (wv[]){ rt_s(&S640), t2107, rt_s(&S641), t2108, rt_s(&S642) }); })), "stmt");
    RT_AT(5, 587);
    up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 588);
    up->v852_loops = rt_add(rt_def(up->v852_loops, "loops"), rt_i(1));
    RT_AT(5, 589);
    (void)rt_chk(s898_blockBody(up, rt_get(&F_body, v1066_s)), "stmt");
    RT_AT(5, 590);
    up->v852_loops = rt_sub(rt_def(up->v852_loops, "loops"), rt_i(1));
    RT_AT(5, 591);
    up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 592);
    (void)rt_chk(s883_emit(up, rt_s(&S15)), "stmt");
    RT_AT(5, 593);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 595);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S42)))) {
    RT_AT(5, 595);
    (void)rt_chk(s902_forStmt(up, v1066_s), "stmt");
    RT_AT(5, 595);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 596);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S56)))) {
    RT_AT(5, 596);
    (void)rt_chk(s903_signal(up, rt_cat(3, (wv[]){ rt_s(&S643), rt_text(rt_get(&F_code, v1066_s)), rt_s(&S16) })), "stmt");
    RT_AT(5, 596);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 597);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S55)))) {
    RT_AT(5, 597);
    (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S644), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1066_s), RT_NULL)) ? rt_s(&S645) : rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_e, v1066_s)), "stmt")))), rt_s(&S635) })), "stmt");
    RT_AT(5, 597);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 598);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S178)))) {
    RT_AT(5, 599);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1066_s)), rt_s(&S186)))) {
      RT_AT(5, 599);
      (void)rt_chk(s913_chainStmt(up, rt_get(&F_e, v1066_s)), "stmt");
      RT_AT(5, 599);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 600);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1066_s)), rt_s(&S45)))) {
      RT_AT(5, 600);
      (void)rt_chk(s924_matchStmt(up, rt_get(&F_e, v1066_s)), "stmt");
      RT_AT(5, 600);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 601);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1066_s)), rt_s(&S185)))) {
      RT_AT(5, 601);
      (void)rt_chk(s903_signal(up, rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_arg, rt_get(&F_e, v1066_s))), "stmt"))), "stmt");
      RT_AT(5, 601);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 602);
    (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S631), rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_e, v1066_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_AT(5, 603);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 605);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S179)))) {
    RT_AT(5, 605);
    (void)({ wv t2113 = ({ wv t2109 = rt_get(&F_namePos, v1066_s); wv t2110 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2109, t2110); }); wv t2114 = ({ wv t2111 = rt_get(&F_e, v1066_s); wv t2112 = rt_chk(w6_modeOf(v1066_s), "stmt"); rt_chk(s906_bindRhs(up, t2111, t2112), "stmt"); }); rt_chk(s904_bindDecl(up, t2113, t2114), "stmt"); });
    RT_AT(5, 605);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 606);
  if (rt_cond(rt_eq(v1067_kind, rt_s(&S38)))) {
    RT_AT(5, 606);
    (void)rt_chk(s907_setStmt(up, v1066_s), "stmt");
    RT_AT(5, 606);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 607);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S646), rt_text(v1067_kind) }));
  RT_RET(RT_NULL);
}

static wv s901_pathText(struct fr176 *up, wv v1068_x) {
  rt_enter("pathText");
  v1068_x = rt_bind(v1068_x, 0);
  RT_AT(5, 612);
  if (rt_cond(rt_eq(rt_get(&F_k, v1068_x), rt_s(&S214)))) {
    RT_AT(5, 612);
    RT_RET(rt_get(&F_name, v1068_x));
  }
  RT_AT(5, 613);
  if (rt_cond(rt_ne(rt_get(&F_k, v1068_x), rt_s(&S192)))) {
    RT_AT(5, 613);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 614);
  wv v1069_p = rt_bind(rt_chk(s901_pathText(up, rt_get(&F_rec, v1068_x)), "pathText"), 0);
  RT_RET((RT_AT(5, 615), (rt_cond(rt_eq(v1069_p, RT_NULL)) ? RT_NULL : ({ wv t2115 = rt_text(v1069_p); wv t2116 = rt_text(rt_get(&F_field, v1068_x)); rt_cat(3, (wv[]){ t2115, rt_s(&S130), t2116 }); }))));
}

static wv s902_forStmt(struct fr176 *up, wv v1070_s) {
  rt_enter("forStmt");
  v1070_s = rt_bind(v1070_s, 0);
  RT_AT(5, 621);
  wv v1071_k = rt_bind(rt_chk(s884_tmp(up), "forStmt"), 0);
  RT_AT(5, 622);
  wv v1072_first = rt_bind(({ wv t2117 = rt_get(&F_pos, rt_idx(rt_i(0), rt_get(&F_names, v1070_s))); wv t2118 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2117, t2118); }), 0);
  RT_AT(5, 623);
  wv v1073_second = rt_bind((rt_cond(rt_eq(std_length(rt_get(&F_names, v1070_s)), rt_i(2))) ? ({ wv t2119 = rt_get(&F_pos, rt_idx(rt_i(1), rt_get(&F_names, v1070_s))); wv t2120 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2119, t2120); }) : RT_NULL), 0);
  RT_AT(5, 624);
  (void)rt_chk(s883_emit(up, rt_s(&S101)), "forStmt");
  RT_AT(5, 625);
  up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 626);
  if (rt_cond(rt_eq(rt_get(&F_coll, v1070_s), RT_NULL))) {
    RT_AT(5, 628);
    wv v1074_lo = rt_bind(rt_chk(s884_tmp(up), "forStmt"), 0);
    RT_AT(5, 629);
    wv v1075_hi = rt_bind(rt_chk(s884_tmp(up), "forStmt"), 0);
    RT_AT(5, 630);
    wv v1076_st = rt_bind(rt_chk(s884_tmp(up), "forStmt"), 0);
    RT_AT(5, 631);
    wv v1077_count = rt_bind(rt_chk(s884_tmp(up), "forStmt"), 0);
    RT_AT(5, 632);
    (void)rt_chk(s883_emit(up, ({ wv t2121 = rt_text(v1074_lo); wv t2122 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_from, v1070_s)), "forStmt"))); rt_cat(5, (wv[]){ rt_s(&S626), t2121, rt_s(&S627), t2122, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 633);
    (void)rt_chk(s883_emit(up, ({ wv t2123 = rt_text(v1075_hi); wv t2124 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_to, v1070_s)), "forStmt"))); rt_cat(5, (wv[]){ rt_s(&S626), t2123, rt_s(&S627), t2124, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 634);
    (void)rt_chk(s883_emit(up, ({ wv t2125 = rt_text(v1076_st); wv t2126 = rt_text((rt_cond(rt_eq(rt_get(&F_step, v1070_s), RT_NULL)) ? rt_s(&S647) : rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_step, v1070_s)), "forStmt")))); rt_cat(5, (wv[]){ rt_s(&S626), t2125, rt_s(&S627), t2126, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 635);
    (void)rt_chk(s883_emit(up, ({ wv t2127 = rt_text(v1077_count); wv t2128 = rt_text(v1074_lo); wv t2129 = rt_text(v1075_hi); wv t2130 = rt_text(v1076_st); rt_cat(9, (wv[]){ rt_s(&S648), t2127, rt_s(&S649), t2128, rt_s(&S419), t2129, rt_s(&S419), t2130, rt_s(&S635) }); })), "forStmt");
    RT_AT(5, 636);
    (void)rt_chk(s883_emit(up, ({ wv t2131 = rt_text(v1071_k); wv t2132 = rt_text(v1071_k); wv t2133 = rt_text(v1077_count); wv t2134 = rt_text(v1071_k); rt_cat(9, (wv[]){ rt_s(&S650), t2131, rt_s(&S651), t2132, rt_s(&S652), t2133, rt_s(&S653), t2134, rt_s(&S654) }); })), "forStmt");
    RT_AT(5, 637);
    up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 638);
    wv v1078_at = rt_bind((rt_cond(rt_get(&F_reversed, v1070_s)) ? ({ wv t2135 = rt_text(v1077_count); wv t2136 = rt_text(v1071_k); rt_cat(3, (wv[]){ t2135, rt_s(&S655), t2136 }); }) : v1071_k), 0);
    RT_AT(5, 639);
    (void)({ wv t2140 = v1072_first; wv t2141 = ({ wv t2137 = rt_text(v1074_lo); wv t2138 = rt_text(v1078_at); wv t2139 = rt_text(v1076_st); rt_cat(7, (wv[]){ rt_s(&S643), t2137, rt_s(&S656), t2138, rt_s(&S657), t2139, rt_s(&S658) }); }); rt_chk(s904_bindDecl(up, t2140, t2141), "forStmt"); });
  } else {
    RT_AT(5, 641);
    wv v1079_en = rt_bind(rt_chk(s890_enumOf(up, rt_get(&F_coll, v1070_s)), "forStmt"), 0);
    RT_AT(5, 642);
    if (rt_cond(rt_ne(v1079_en, RT_NULL))) {
      RT_AT(5, 644);
      wv v1080_count = rt_bind(std_length(rt_get(&F_members, rt_get(&F_node, v1079_en))), 0);
      RT_AT(5, 645);
      (void)rt_chk(s883_emit(up, ({ wv t2142 = rt_text(v1071_k); wv t2143 = rt_text(v1071_k); wv t2144 = rt_text(v1080_count); wv t2145 = rt_text(v1071_k); rt_cat(9, (wv[]){ rt_s(&S650), t2142, rt_s(&S651), t2143, rt_s(&S652), t2144, rt_s(&S653), t2145, rt_s(&S654) }); })), "forStmt");
      RT_AT(5, 646);
      up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
      RT_AT(5, 647);
      wv v1081_at = rt_bind((rt_cond(rt_get(&F_reversed, v1070_s)) ? ({ wv t2146 = rt_text(rt_sub(v1080_count, rt_i(1))); wv t2147 = rt_text(v1071_k); rt_cat(3, (wv[]){ t2146, rt_s(&S659), t2147 }); }) : v1071_k), 0);
      RT_AT(5, 648);
      if (rt_cond(rt_eq(v1073_second, RT_NULL))) {
        RT_AT(5, 649);
        (void)({ wv t2150 = v1072_first; wv t2151 = ({ wv t2148 = rt_text(rt_get(&F_c, v1079_en)); wv t2149 = rt_text(v1081_at); rt_cat(5, (wv[]){ rt_s(&S660), t2148, rt_s(&S99), t2149, rt_s(&S661) }); }); rt_chk(s904_bindDecl(up, t2150, t2151), "forStmt"); });
      } else {
        RT_AT(5, 651);
        (void)({ wv t2152 = v1072_first; wv t2153 = rt_cat(3, (wv[]){ rt_s(&S643), rt_text(v1081_at), rt_s(&S16) }); rt_chk(s904_bindDecl(up, t2152, t2153), "forStmt"); });
        RT_AT(5, 652);
        (void)({ wv t2156 = v1073_second; wv t2157 = ({ wv t2154 = rt_text(rt_get(&F_c, v1079_en)); wv t2155 = rt_text(v1081_at); rt_cat(5, (wv[]){ rt_s(&S660), t2154, rt_s(&S99), t2155, rt_s(&S661) }); }); rt_chk(s904_bindDecl(up, t2156, t2157), "forStmt"); });
      }
    } else {
      RT_AT(5, 657);
      wv v1082_fr = rt_bind(rt_chk(s884_tmp(up), "forStmt"), 0);
      RT_AT(5, 658);
      (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S662), rt_text(v1082_fr), rt_s(&S14) })), "forStmt");
      RT_AT(5, 659);
      (void)rt_chk(s883_emit(up, ({ wv t2158 = rt_text(v1082_fr); wv t2159 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_coll, v1070_s)), "forStmt"))); wv t2160 = rt_text((rt_cond(rt_get(&F_reversed, v1070_s)) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S663), t2158, rt_s(&S419), t2159, rt_s(&S419), t2160, rt_s(&S635) }); })), "forStmt");
      RT_AT(5, 660);
      wv v1083_path = rt_bind(rt_chk(s901_pathText(up, rt_get(&F_coll, v1070_s)), "forStmt"), 0);
      RT_AT(5, 661);
      wv v1084_guard = rt_bind((rt_cond(rt_eq(v1083_path, RT_NULL)) ? rt_s(&S13) : ({ wv t2161 = rt_text(v1082_fr); wv t2162 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_coll, v1070_s)), "forStmt"))); wv t2163 = rt_text(rt_chk(w6_cString(v1083_path), "forStmt")); rt_cat(7, (wv[]){ rt_s(&S664), t2161, rt_s(&S419), t2162, rt_s(&S419), t2163, rt_s(&S665) }); })), 0);
      RT_AT(5, 662);
      (void)rt_chk(s883_emit(up, ({ wv t2164 = rt_text(v1071_k); wv t2165 = rt_text(v1084_guard); wv t2166 = rt_text(v1082_fr); wv t2167 = rt_text(v1071_k); wv t2168 = rt_text(v1071_k); rt_cat(11, (wv[]){ rt_s(&S650), t2164, rt_s(&S651), t2165, rt_s(&S666), t2166, rt_s(&S419), t2167, rt_s(&S667), t2168, rt_s(&S654) }); })), "forStmt");
      RT_AT(5, 663);
      up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
      RT_AT(5, 664);
      if (rt_cond(rt_eq(v1073_second, RT_NULL))) {
        RT_AT(5, 665);
        (void)({ wv t2171 = v1072_first; wv t2172 = ({ wv t2169 = rt_text(v1082_fr); wv t2170 = rt_text(v1071_k); rt_cat(5, (wv[]){ rt_s(&S668), t2169, rt_s(&S419), t2170, rt_s(&S16) }); }); rt_chk(s904_bindDecl(up, t2171, t2172), "forStmt"); });
      } else {
        RT_AT(5, 667);
        (void)({ wv t2175 = v1072_first; wv t2176 = ({ wv t2173 = rt_text(v1082_fr); wv t2174 = rt_text(v1071_k); rt_cat(5, (wv[]){ rt_s(&S669), t2173, rt_s(&S419), t2174, rt_s(&S16) }); }); rt_chk(s904_bindDecl(up, t2175, t2176), "forStmt"); });
        RT_AT(5, 668);
        (void)({ wv t2179 = v1073_second; wv t2180 = ({ wv t2177 = rt_text(v1082_fr); wv t2178 = rt_text(v1071_k); rt_cat(5, (wv[]){ rt_s(&S670), t2177, rt_s(&S419), t2178, rt_s(&S16) }); }); rt_chk(s904_bindDecl(up, t2179, t2180), "forStmt"); });
      }
    }
  }
  RT_AT(5, 672);
  up->v852_loops = rt_add(rt_def(up->v852_loops, "loops"), rt_i(1));
  RT_AT(5, 673);
  (void)rt_chk(s898_blockBody(up, rt_get(&F_body, v1070_s)), "forStmt");
  RT_AT(5, 674);
  up->v852_loops = rt_sub(rt_def(up->v852_loops, "loops"), rt_i(1));
  RT_AT(5, 675);
  up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 676);
  (void)rt_chk(s883_emit(up, rt_s(&S15)), "forStmt");
  RT_AT(5, 677);
  up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 678);
  (void)rt_chk(s883_emit(up, rt_s(&S15)), "forStmt");
  RT_RET(RT_NULL);
}

static wv s903_signal(struct fr176 *up, wv v1085_code) {
  rt_enter("signal");
  v1085_code = rt_bind(v1085_code, 0);
  RT_AT(5, 683);
  (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S671), rt_text(v1085_code), rt_s(&S635) })), "signal");
  RT_AT(5, 684);
  (void)rt_chk(s883_emit(up, rt_s(&S672)), "signal");
  RT_RET(RT_NULL);
}

static wv s904_bindDecl(struct fr176 *up, wv v1086_d, wv v1087_value) {
  rt_enter("bindDecl");
  v1086_d = rt_bind(v1086_d, 0);
  v1087_value = rt_bind(v1087_value, 0);
  RT_AT(5, 689);
  wv v1088_owner = rt_bind(rt_get(&F_func, ({ wv t2181 = v1086_d; wv t2182 = rt_def(up->v854_decls, "decls"); rt_idx(t2181, t2182); })), 0);
  RT_AT(5, 690);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t2183 = v1088_owner; wv t2184 = rt_def(up->v855_fns, "fns"); rt_idx(t2183, t2184); }))), "&&") && rt_bool(({ wv t2185 = v1088_owner; wv t2186 = rt_def(up->v863_cur, "cur"); rt_eq(t2185, t2186); }), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_captured, ({ wv t2187 = v1086_d; wv t2188 = rt_def(up->v854_decls, "decls"); rt_idx(t2187, t2188); }))), "&&")))) {
    RT_AT(5, 691);
    (void)rt_chk(s883_emit(up, ({ wv t2191 = rt_text(rt_get(&F_cname, ({ wv t2189 = v1086_d; wv t2190 = rt_def(up->v854_decls, "decls"); rt_idx(t2189, t2190); }))); wv t2192 = rt_text(v1087_value); rt_cat(5, (wv[]){ rt_s(&S626), t2191, rt_s(&S627), t2192, rt_s(&S14) }); })), "bindDecl");
  } else {
    RT_AT(5, 693);
    (void)rt_chk(s883_emit(up, ({ wv t2193 = rt_text(rt_chk(s891_place(up, v1086_d), "bindDecl")); wv t2194 = rt_text(v1087_value); rt_cat(4, (wv[]){ t2193, rt_s(&S627), t2194, rt_s(&S14) }); })), "bindDecl");
  }
  RT_RET(RT_NULL);
}

static wv s905_fnDef(struct fr176 *up, wv v1089_s) {
  rt_enter("fnDef");
  v1089_s = rt_bind(v1089_s, 0);
  RT_AT(5, 698);
  wv v1090_d = rt_bind(({ wv t2195 = rt_get(&F_namePos, v1089_s); wv t2196 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2195, t2196); }), 0);
  RT_AT(5, 699);
  wv v1091_f = rt_bind(rt_get(&F_fnIndex, ({ wv t2197 = v1090_d; wv t2198 = rt_def(up->v854_decls, "decls"); rt_idx(t2197, t2198); })), 0);
  RT_AT(5, 700);
  wv v1092_pf = rt_bind(rt_get(&F_parent, ({ wv t2199 = v1091_f; wv t2200 = rt_def(up->v855_fns, "fns"); rt_idx(t2199, t2200); })), 0);
  RT_AT(5, 701);
  wv v1093_hasUp = rt_not(rt_get(&F_top, ({ wv t2201 = v1092_pf; wv t2202 = rt_def(up->v855_fns, "fns"); rt_idx(t2201, t2202); })));
  RT_AT(5, 702);
  wv v1094_params = rt_arr(0, NULL, 1);
  RT_AT(5, 703);
  if (rt_cond(v1093_hasUp)) {
    RT_AT(5, 703);
    (void)({ wv t2203 = v1094_params; wv t2204 = rt_cat(3, (wv[]){ rt_s(&S673), rt_text(v1092_pf), rt_s(&S674) }); std_push(t2203, t2204); });
  }
  RT_AT(5, 704);
  wv v1095_k = rt_i(0);
  RT_AT(5, 705);
  while (rt_cond((RT_AT(5, 705), ({ wv t2205 = v1095_k; wv t2206 = std_length(rt_get(&F_params, v1089_s)); rt_lt(t2205, t2206); })))) {
    RT_AT(5, 706);
    wv v1103_pd = rt_bind(({ wv t2209 = rt_get(&F_pos, ({ wv t2207 = v1095_k; wv t2208 = rt_get(&F_params, v1089_s); rt_idx(t2207, t2208); })); wv t2210 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2209, t2210); }), 0);
    RT_AT(5, 707);
    (void)({ wv t2217 = v1094_params; wv t2218 = ({ wv t2215 = rt_text((rt_cond(rt_get(&F_captured, ({ wv t2211 = v1103_pd; wv t2212 = rt_def(up->v854_decls, "decls"); rt_idx(t2211, t2212); }))) ? rt_s(&S675) : rt_s(&S13))); wv t2216 = rt_text(rt_get(&F_cname, ({ wv t2213 = v1103_pd; wv t2214 = rt_def(up->v854_decls, "decls"); rt_idx(t2213, t2214); }))); rt_cat(3, (wv[]){ rt_s(&S626), t2215, t2216 }); }); std_push(t2217, t2218); });
    RT_AT(5, 708);
    v1095_k = rt_add(v1095_k, rt_i(1));
  }
  RT_AT(5, 710);
  wv v1096_proto = ({ wv t2221 = rt_text(rt_get(&F_cname, ({ wv t2219 = v1091_f; wv t2220 = rt_def(up->v855_fns, "fns"); rt_idx(t2219, t2220); }))); wv t2222 = rt_text((rt_cond(rt_eq(std_length(v1094_params), rt_i(0))) ? rt_s(&S677) : std_join(v1094_params, rt_s(&S419)))); rt_cat(5, (wv[]){ rt_s(&S676), t2221, rt_s(&S98), t2222, rt_s(&S16) }); });
  RT_AT(5, 711);
  wv v1097_entry = ({ wv t2223 = v1096_proto; wv t2224 = rt_arr(0, NULL, 0); rt_rec(1, 2, K62, (wv[]){ t2223, t2224 }); });
  RT_AT(5, 712);
  (void)({ wv t2225 = rt_def(up->v849_functions, "functions"); wv t2226 = v1097_entry; std_push(t2225, t2226); });
  RT_AT(5, 713);
  if (rt_cond(rt_chk(s868_hasFrame(up, v1091_f), "fnDef"))) {
    RT_AT(5, 713);
    (void)({ wv t2227 = rt_def(up->v848_frames, "frames"); wv t2228 = v1091_f; std_push(t2227, t2228); });
  }
  RT_AT(5, 714);
  wv v1098_start = rt_bind(std_length(rt_def(up->v850_out, "out")), 0);
  RT_AT(5, 715);
  wv v1099_savedIndent = rt_bind(rt_def(up->v851_indent, "indent"), 0);
  RT_AT(5, 716);
  wv v1100_savedLoops = rt_bind(rt_def(up->v852_loops, "loops"), 0);
  RT_AT(5, 717);
  wv v1101_savedIife = rt_bind(rt_def(up->v853_iife, "iife"), 0);
  RT_AT(5, 718);
  wv v1102_savedCur = rt_bind(rt_def(up->v863_cur, "cur"), 0);
  RT_AT(5, 719);
  wv t2229 = rt_i(1);
  (void)rt_def(up->v851_indent, "indent");
  up->v851_indent = t2229;
  RT_AT(5, 720);
  wv t2230 = rt_i(0);
  (void)rt_def(up->v852_loops, "loops");
  up->v852_loops = t2230;
  RT_AT(5, 721);
  wv t2231 = rt_b(0);
  (void)rt_def(up->v853_iife, "iife");
  up->v853_iife = t2231;
  RT_AT(5, 722);
  wv t2232 = rt_bind(v1091_f, 1);
  (void)rt_def(up->v863_cur, "cur");
  up->v863_cur = t2232;
  RT_AT(5, 723);
  (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S678), rt_text(rt_chk(w1_quote(rt_get(&F_name, v1089_s)), "fnDef")), rt_s(&S635) })), "fnDef");
  RT_AT(5, 724);
  if (rt_cond(rt_chk(s868_hasFrame(up, v1091_f), "fnDef"))) {
    RT_AT(5, 724);
    (void)rt_chk(s883_emit(up, ({ wv t2233 = rt_text(v1091_f); wv t2234 = rt_text((rt_cond(v1093_hasUp) ? rt_s(&S680) : rt_s(&S4))); rt_cat(5, (wv[]){ rt_s(&S673), t2233, rt_s(&S679), t2234, rt_s(&S619) }); })), "fnDef");
  }
  RT_AT(5, 725);
  v1095_k = rt_i(0);
  RT_AT(5, 726);
  while (rt_cond((RT_AT(5, 726), ({ wv t2235 = v1095_k; wv t2236 = std_length(rt_get(&F_params, v1089_s)); rt_lt(t2235, t2236); })))) {
    RT_AT(5, 727);
    wv v1104_p = rt_bind(({ wv t2237 = v1095_k; wv t2238 = rt_get(&F_params, v1089_s); rt_idx(t2237, t2238); }), 0);
    RT_AT(5, 728);
    wv v1105_pd = rt_bind(({ wv t2239 = rt_get(&F_pos, v1104_p); wv t2240 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2239, t2240); }), 0);
    RT_AT(5, 729);
    wv v1106_cn = rt_bind(rt_get(&F_cname, ({ wv t2241 = v1105_pd; wv t2242 = rt_def(up->v854_decls, "decls"); rt_idx(t2241, t2242); })), 0);
    RT_AT(5, 731);
    wv v1107_arg = rt_bind((rt_cond(rt_get(&F_captured, ({ wv t2243 = v1105_pd; wv t2244 = rt_def(up->v854_decls, "decls"); rt_idx(t2243, t2244); }))) ? rt_cat(2, (wv[]){ rt_s(&S675), rt_text(v1106_cn) }) : v1106_cn), 0);
    RT_AT(5, 732);
    wv v1108_v = rt_bind((rt_cond(rt_get(&F_isRef, v1104_p)) ? v1107_arg : ({ wv t2245 = rt_text(v1107_arg); wv t2246 = rt_text((rt_cond(rt_get(&F_isVar, v1104_p)) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S681), t2245, rt_s(&S419), t2246, rt_s(&S16) }); })), 0);
    RT_AT(5, 733);
    if (rt_cond(rt_get(&F_captured, ({ wv t2247 = v1105_pd; wv t2248 = rt_def(up->v854_decls, "decls"); rt_idx(t2247, t2248); })))) {
      RT_AT(5, 733);
      (void)rt_chk(s883_emit(up, ({ wv t2249 = rt_text(v1106_cn); wv t2250 = rt_text(v1108_v); rt_cat(5, (wv[]){ rt_s(&S621), t2249, rt_s(&S627), t2250, rt_s(&S14) }); })), "fnDef");
    } else if (rt_cond(rt_not(rt_get(&F_isRef, v1104_p)))) {
      RT_AT(5, 733);
      (void)rt_chk(s883_emit(up, ({ wv t2251 = rt_text(v1106_cn); wv t2252 = rt_text(v1108_v); rt_cat(4, (wv[]){ t2251, rt_s(&S627), t2252, rt_s(&S14) }); })), "fnDef");
    }
    RT_AT(5, 734);
    v1095_k = rt_add(v1095_k, rt_i(1));
  }
  RT_AT(5, 736);
  (void)rt_chk(s896_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1089_s))), "fnDef");
  RT_AT(5, 737);
  (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S634), rt_text(rt_chk(s899_valueOf(up, rt_get(&F_body, v1089_s)), "fnDef")), rt_s(&S635) })), "fnDef");
  RT_AT(5, 738);
  ({ wv t2256 = ({ wv t2253 = rt_def(up->v850_out, "out"); wv t2254 = v1098_start; wv t2255 = std_length(rt_def(up->v850_out, "out")); std_slice(t2253, t2254, t2255); }); wv t2257 = v1097_entry; rt_set_mem(t2256, &F_lines, t2257); });
  RT_AT(5, 739);
  while (rt_cond((RT_AT(5, 739), ({ wv t2258 = std_length(rt_def(up->v850_out, "out")); wv t2259 = v1098_start; rt_gt(t2258, t2259); })))) {
    RT_AT(5, 739);
    (void)std_pop(rt_def(up->v850_out, "out"));
  }
  RT_AT(5, 740);
  wv t2260 = rt_bind(v1099_savedIndent, 1);
  (void)rt_def(up->v851_indent, "indent");
  up->v851_indent = t2260;
  RT_AT(5, 741);
  wv t2261 = rt_bind(v1100_savedLoops, 1);
  (void)rt_def(up->v852_loops, "loops");
  up->v852_loops = t2261;
  RT_AT(5, 742);
  wv t2262 = rt_bind(v1101_savedIife, 1);
  (void)rt_def(up->v853_iife, "iife");
  up->v853_iife = t2262;
  RT_AT(5, 743);
  wv t2263 = rt_bind(v1102_savedCur, 1);
  (void)rt_def(up->v863_cur, "cur");
  up->v863_cur = t2263;
  RT_RET(RT_NULL);
}

static wv s906_bindRhs(struct fr176 *up, wv v1109_e, wv v1110_mode) {
  rt_enter("bindRhs");
  v1109_e = rt_bind(v1109_e, 0);
  v1110_mode = rt_bind(v1110_mode, 0);
  RT_AT(5, 750);
  wv v1111_mutable = rt_eq(v1110_mode, rt_s(&S36));
  RT_AT(5, 751);
  if (rt_cond(rt_eq(rt_get(&F_k, v1109_e), rt_s(&S188)))) {
    RT_AT(5, 751);
    RT_RET(rt_get(&F_c, rt_chk(s909_array(up, v1109_e, v1111_mutable), "bindRhs")));
  }
  RT_AT(5, 752);
  if (rt_cond(rt_eq(rt_get(&F_k, v1109_e), rt_s(&S189)))) {
    RT_AT(5, 752);
    RT_RET(rt_get(&F_c, rt_chk(s910_mapLit(up, v1109_e, v1111_mutable), "bindRhs")));
  }
  RT_AT(5, 753);
  if (rt_cond(rt_eq(rt_get(&F_k, v1109_e), rt_s(&S190)))) {
    RT_AT(5, 753);
    RT_RET(rt_get(&F_c, rt_chk(s911_record(up, v1109_e, v1111_mutable), "bindRhs")));
  }
  RT_AT(5, 754);
  if (rt_cond(rt_b(rt_bool(({ wv t2264 = rt_def(w6_scalarKinds, "scalarKinds"); wv t2265 = rt_get(&F_k, v1109_e); rt_chk(w1_contains(t2264, t2265), "bindRhs"); }), "||") || rt_bool(rt_eq(v1110_mode, rt_s(&S37)), "||")))) {
    RT_AT(5, 754);
    RT_RET(rt_get(&F_c, rt_chk(s927_expr(up, v1109_e), "bindRhs")));
  }
  RT_RET((RT_AT(5, 755), ({ wv t2266 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, v1109_e), "bindRhs"))); wv t2267 = rt_text((rt_cond(v1111_mutable) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S681), t2266, rt_s(&S419), t2267, rt_s(&S16) }); })));
}

static wv s907_setStmt(struct fr176 *up, wv v1112_s) {
  rt_enter("setStmt");
  v1112_s = rt_bind(v1112_s, 0);
  RT_AT(5, 759);
  wv v1113_t = rt_bind(rt_get(&F_target, v1112_s), 0);
  RT_AT(5, 760);
  if (rt_cond(rt_eq(rt_get(&F_k, v1113_t), rt_s(&S214)))) {
    RT_AT(5, 761);
    wv v1120_d = rt_bind(({ wv t2268 = rt_get(&F_pos, v1113_t); wv t2269 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2268, t2269); }), 0);
    RT_AT(5, 762);
    wv v1121_at = rt_bind(rt_chk(s891_place(up, v1120_d), "setStmt"), 0);
    RT_AT(5, 763);
    if (rt_cond(rt_eq(rt_get(&F_op, v1112_s), RT_NULL))) {
      RT_AT(5, 764);
      wv v1124_v = rt_bind(({ wv t2272 = rt_get(&F_e, v1112_s); wv t2273 = (rt_cond(rt_get(&F_isRef, ({ wv t2270 = v1120_d; wv t2271 = rt_def(up->v854_decls, "decls"); rt_idx(t2270, t2271); }))) ? rt_s(&S37) : rt_s(&S36)); rt_chk(s906_bindRhs(up, t2272, t2273), "setStmt"); }), 0);
      RT_AT(5, 765);
      if (rt_cond(rt_not(rt_chk(s892_tdz(up, v1120_d), "setStmt")))) {
        RT_AT(5, 765);
        (void)rt_chk(s883_emit(up, ({ wv t2274 = rt_text(v1121_at); wv t2275 = rt_text(v1124_v); rt_cat(4, (wv[]){ t2274, rt_s(&S627), t2275, rt_s(&S14) }); })), "setStmt");
        RT_AT(5, 765);
        RT_RET(RT_NULL);
      }
      RT_AT(5, 767);
      wv v1125_temp = rt_bind(rt_chk(s884_tmp(up), "setStmt"), 0);
      RT_AT(5, 768);
      (void)rt_chk(s883_emit(up, ({ wv t2276 = rt_text(v1125_temp); wv t2277 = rt_text(v1124_v); rt_cat(5, (wv[]){ rt_s(&S626), t2276, rt_s(&S627), t2277, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 769);
      (void)rt_chk(s883_emit(up, ({ wv t2280 = rt_text(v1121_at); wv t2281 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t2278 = v1120_d; wv t2279 = rt_def(up->v854_decls, "decls"); rt_idx(t2278, t2279); }))), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S682), t2280, rt_s(&S419), t2281, rt_s(&S635) }); })), "setStmt");
      RT_AT(5, 770);
      (void)rt_chk(s883_emit(up, ({ wv t2282 = rt_text(v1121_at); wv t2283 = rt_text(v1125_temp); rt_cat(4, (wv[]){ t2282, rt_s(&S627), t2283, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 771);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 774);
    wv v1122_v = rt_bind(rt_chk(s927_expr(up, rt_get(&F_e, v1112_s)), "setStmt"), 1);
    RT_AT(5, 775);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1112_s)), rt_s(&S139)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1112_s)), rt_s(&S214)), "&&")))) {
      RT_AT(5, 776);
      wv v1126_temp = rt_bind(rt_chk(s884_tmp(up), "setStmt"), 0);
      RT_AT(5, 777);
      (void)rt_chk(s883_emit(up, ({ wv t2284 = rt_text(v1126_temp); wv t2285 = rt_text(rt_get(&F_c, v1122_v)); rt_cat(5, (wv[]){ rt_s(&S626), t2284, rt_s(&S627), t2285, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 778);
      v1122_v = ({ wv t2286 = v1126_temp; wv t2287 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K60, (wv[]){ t2286, t2287 }); });
    }
    RT_AT(5, 780);
    wv v1123_sq = rt_bind(rt_chk(s894_seq(up, ({ wv t2288 = rt_chk(s893_read(up, v1120_d), "setStmt"); wv t2289 = v1122_v; rt_arr(2, (wv[]){ t2288, t2289 }, 0); })), "setStmt"), 0);
    RT_AT(5, 781);
    (void)rt_chk(s883_emit(up, ({ wv t2295 = rt_text(v1121_at); wv t2296 = rt_text(rt_get(&F_c, ({ wv t2293 = v1123_sq; wv t2294 = ({ wv t2290 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v1112_s)), "setStmt")); wv t2291 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1123_sq))); wv t2292 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1123_sq))); rt_cat(7, (wv[]){ rt_s(&S683), t2290, rt_s(&S98), t2291, rt_s(&S419), t2292, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2293, t2294), "setStmt"); }))); rt_cat(4, (wv[]){ t2295, rt_s(&S627), t2296, rt_s(&S14) }); })), "setStmt");
    RT_AT(5, 782);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 784);
  wv v1114_key = RT_NULL;
  RT_AT(5, 785);
  if (rt_cond(rt_eq(rt_get(&F_k, v1113_t), rt_s(&S192)))) {
    RT_AT(5, 785);
    v1114_key = ({ wv t2297 = rt_chk(s886_fieldRef(up, rt_get(&F_field, v1113_t)), "setStmt"); wv t2298 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K60, (wv[]){ t2297, t2298 }); });
  }
  RT_AT(5, 786);
  wv v1115_v = rt_bind(rt_chk(s927_expr(up, rt_get(&F_e, v1112_s)), "setStmt"), 0);
  RT_AT(5, 787);
  wv v1116_i = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1113_t), rt_s(&S192))) ? v1114_key : rt_chk(s927_expr(up, rt_get(&F_index, v1113_t)), "setStmt")), 0);
  RT_AT(5, 788);
  wv v1117_box = rt_bind(rt_chk(s927_expr(up, (rt_cond(rt_eq(rt_get(&F_k, v1113_t), rt_s(&S192))) ? rt_get(&F_rec, v1113_t) : rt_get(&F_arr, v1113_t))), "setStmt"), 0);
  RT_AT(5, 789);
  wv v1118_sq = rt_bind(rt_chk(s894_seq(up, rt_arr(3, (wv[]){ v1115_v, v1116_i, v1117_box }, 0)), "setStmt"), 0);
  RT_AT(5, 790);
  wv v1119_which = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1113_t), rt_s(&S192))) ? rt_s(&S192) : rt_s(&S191)), 0);
  RT_AT(5, 791);
  if (rt_cond(rt_eq(rt_get(&F_op, v1112_s), RT_NULL))) {
    RT_AT(5, 792);
    (void)rt_chk(s883_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2301 = v1118_sq; wv t2302 = ({ wv t2299 = rt_text(v1119_which); wv t2300 = rt_text(std_join(rt_get(&F_cs, v1118_sq), rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S684), t2299, rt_s(&S98), t2300, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2301, t2302), "setStmt"); }))), rt_s(&S14) })), "setStmt");
    RT_AT(5, 793);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 795);
  (void)rt_chk(s883_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2306 = v1118_sq; wv t2307 = ({ wv t2303 = rt_text(v1119_which); wv t2304 = rt_text(std_join(rt_get(&F_cs, v1118_sq), rt_s(&S419))); wv t2305 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v1112_s)), "setStmt")); rt_cat(7, (wv[]){ rt_s(&S685), t2303, rt_s(&S98), t2304, rt_s(&S686), t2305, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2306, t2307), "setStmt"); }))), rt_s(&S14) })), "setStmt");
  RT_RET(RT_NULL);
}

static wv s908_exprs(struct fr176 *up, wv v1127_list) {
  rt_enter("exprs");
  v1127_list = rt_bind(v1127_list, 0);
  RT_AT(5, 799);
  wv v1128_xs = rt_arr(0, NULL, 1);
  RT_AT(5, 800);
  wv v1129_k = rt_i(0);
  RT_AT(5, 801);
  while (rt_cond((RT_AT(5, 801), ({ wv t2308 = v1129_k; wv t2309 = std_length(v1127_list); rt_lt(t2308, t2309); })))) {
    RT_AT(5, 802);
    (void)({ wv t2310 = v1128_xs; wv t2311 = rt_chk(s927_expr(up, rt_idx(v1129_k, v1127_list)), "exprs"); std_push(t2310, t2311); });
    RT_AT(5, 803);
    v1129_k = rt_add(v1129_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 805), v1128_xs));
}

static wv s909_array(struct fr176 *up, wv v1130_e, wv v1131_mutable) {
  rt_enter("array");
  v1130_e = rt_bind(v1130_e, 0);
  v1131_mutable = rt_bind(v1131_mutable, 0);
  RT_AT(5, 809);
  wv v1132_sq = rt_bind(rt_chk(s894_seq(up, rt_chk(s908_exprs(up, rt_get(&F_items, v1130_e)), "array")), "array"), 0);
  RT_RET((RT_AT(5, 810), ({ wv t2315 = v1132_sq; wv t2316 = ({ wv t2312 = rt_text(std_length(rt_get(&F_cs, v1132_sq))); wv t2313 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1132_sq)), "array")); wv t2314 = rt_text((rt_cond(v1131_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S687), t2312, rt_s(&S419), t2313, rt_s(&S419), t2314, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2315, t2316), "array"); })));
}

static wv s910_mapLit(struct fr176 *up, wv v1133_e, wv v1134_mutable) {
  rt_enter("mapLit");
  v1133_e = rt_bind(v1133_e, 0);
  v1134_mutable = rt_bind(v1134_mutable, 0);
  RT_AT(5, 815);
  wv v1135_sq = rt_bind(rt_chk(s894_seq(up, rt_chk(s908_exprs(up, rt_get(&F_items, v1133_e)), "mapLit")), "mapLit"), 0);
  RT_RET((RT_AT(5, 816), ({ wv t2320 = v1135_sq; wv t2321 = ({ wv t2317 = rt_text(std_floor(rt_div(std_length(rt_get(&F_cs, v1135_sq)), rt_i(2)))); wv t2318 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1135_sq)), "mapLit")); wv t2319 = rt_text((rt_cond(v1134_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S688), t2317, rt_s(&S419), t2318, rt_s(&S419), t2319, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2320, t2321), "mapLit"); })));
}

static wv s911_record(struct fr176 *up, wv v1136_e, wv v1137_mutable) {
  rt_enter("record");
  v1136_e = rt_bind(v1136_e, 0);
  v1137_mutable = rt_bind(v1137_mutable, 0);
  RT_AT(5, 821);
  wv v1138_shape = rt_s(&S609);
  RT_AT(5, 822);
  if (rt_cond(rt_gt(std_length(rt_get(&F_fields, v1136_e)), rt_i(0)))) {
    RT_AT(5, 823);
    wv v1142_names = rt_arr(0, NULL, 1);
    RT_AT(5, 824);
    wv v1143_k = rt_i(0);
    RT_AT(5, 825);
    while (rt_cond((RT_AT(5, 825), ({ wv t2322 = v1143_k; wv t2323 = std_length(rt_get(&F_fields, v1136_e)); rt_lt(t2322, t2323); })))) {
      RT_AT(5, 826);
      (void)({ wv t2326 = v1142_names; wv t2327 = rt_get(&F_name, ({ wv t2324 = v1143_k; wv t2325 = rt_get(&F_fields, v1136_e); rt_idx(t2324, t2325); })); std_push(t2326, t2327); });
      RT_AT(5, 827);
      v1143_k = rt_add(v1143_k, rt_i(1));
    }
    RT_AT(5, 829);
    wv v1144_key = rt_bind(std_join(v1142_names, rt_s(&S97)), 0);
    RT_AT(5, 830);
    v1143_k = rt_i(0);
    RT_AT(5, 831);
    while (rt_cond((RT_AT(5, 831), ({ wv t2328 = v1143_k; wv t2329 = std_length(v1142_names); rt_lt(t2328, t2329); })))) {
      RT_AT(5, 832);
      (void)rt_chk(s886_fieldRef(up, rt_idx(v1143_k, v1142_names)), "record");
      RT_AT(5, 833);
      v1143_k = rt_add(v1143_k, rt_i(1));
    }
    RT_AT(5, 835);
    v1143_k = rt_i(0);
    RT_AT(5, 836);
    while (rt_cond((RT_AT(5, 836), rt_b(rt_bool(({ wv t2330 = v1143_k; wv t2331 = std_length(rt_def(up->v845_shapes, "shapes")); rt_lt(t2330, t2331); }), "&&") && rt_bool(({ wv t2334 = ({ wv t2332 = v1143_k; wv t2333 = rt_def(up->v845_shapes, "shapes"); rt_idx(t2332, t2333); }); wv t2335 = v1144_key; rt_ne(t2334, t2335); }), "&&"))))) {
      RT_AT(5, 836);
      v1143_k = rt_add(v1143_k, rt_i(1));
    }
    RT_AT(5, 837);
    if (rt_cond(({ wv t2336 = v1143_k; wv t2337 = std_length(rt_def(up->v845_shapes, "shapes")); rt_eq(t2336, t2337); }))) {
      RT_AT(5, 837);
      (void)({ wv t2338 = rt_def(up->v845_shapes, "shapes"); wv t2339 = v1144_key; std_push(t2338, t2339); });
    }
    RT_AT(5, 838);
    v1138_shape = rt_cat(2, (wv[]){ rt_s(&S689), rt_text(v1143_k) });
  }
  RT_AT(5, 840);
  wv v1139_vals = rt_arr(0, NULL, 1);
  RT_AT(5, 841);
  wv v1140_k = rt_i(0);
  RT_AT(5, 842);
  while (rt_cond((RT_AT(5, 842), ({ wv t2340 = v1140_k; wv t2341 = std_length(rt_get(&F_fields, v1136_e)); rt_lt(t2340, t2341); })))) {
    RT_AT(5, 843);
    (void)({ wv t2344 = v1139_vals; wv t2345 = rt_chk(s927_expr(up, rt_get(&F_e, ({ wv t2342 = v1140_k; wv t2343 = rt_get(&F_fields, v1136_e); rt_idx(t2342, t2343); }))), "record"); std_push(t2344, t2345); });
    RT_AT(5, 844);
    v1140_k = rt_add(v1140_k, rt_i(1));
  }
  RT_AT(5, 846);
  wv v1141_sq = rt_bind(rt_chk(s894_seq(up, v1139_vals), "record"), 0);
  RT_RET((RT_AT(5, 847), ({ wv t2350 = v1141_sq; wv t2351 = ({ wv t2346 = rt_text((rt_cond(v1137_mutable) ? rt_i(1) : rt_i(0))); wv t2347 = rt_text(std_length(rt_get(&F_cs, v1141_sq))); wv t2348 = rt_text(v1138_shape); wv t2349 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1141_sq)), "record")); rt_cat(9, (wv[]){ rt_s(&S690), t2346, rt_s(&S419), t2347, rt_s(&S419), t2348, rt_s(&S419), t2349, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2350, t2351), "record"); })));
}

static wv s912_onfailStmt(struct fr176 *up, wv v1145_s) {
  rt_enter("onfailStmt");
  v1145_s = rt_bind(v1145_s, 0);
  RT_AT(5, 853);
  wv v1146_call = rt_bind(rt_get(&F_e, v1145_s), 0);
  RT_AT(5, 854);
  wv v1147_args = rt_arr(0, NULL, 1);
  RT_AT(5, 855);
  wv v1148_k = rt_i(0);
  RT_AT(5, 856);
  while (rt_cond((RT_AT(5, 856), ({ wv t2352 = v1148_k; wv t2353 = std_length(rt_get(&F_args, v1146_call)); rt_lt(t2352, t2353); })))) {
    RT_AT(5, 857);
    wv v1152_a = rt_bind(({ wv t2354 = v1148_k; wv t2355 = rt_get(&F_args, v1146_call); rt_idx(t2354, t2355); }), 0);
    RT_AT(5, 858);
    if (rt_cond(({ wv t2356 = rt_def(w6_plainKinds, "plainKinds"); wv t2357 = rt_get(&F_k, v1152_a); rt_chk(w1_contains(t2356, t2357), "onfailStmt"); }))) {
      RT_AT(5, 859);
      (void)({ wv t2358 = v1147_args; wv t2359 = rt_get(&F_c, rt_chk(s927_expr(up, v1152_a), "onfailStmt")); std_push(t2358, t2359); });
    } else {
      RT_AT(5, 861);
      wv v1153_temp = rt_bind(rt_chk(s884_tmp(up), "onfailStmt"), 0);
      RT_AT(5, 862);
      (void)rt_chk(s883_emit(up, ({ wv t2360 = rt_text(v1153_temp); wv t2361 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, v1152_a), "onfailStmt"))); rt_cat(5, (wv[]){ rt_s(&S626), t2360, rt_s(&S627), t2361, rt_s(&S14) }); })), "onfailStmt");
      RT_AT(5, 863);
      (void)std_push(v1147_args, v1153_temp);
    }
    RT_AT(5, 865);
    v1148_k = rt_add(v1148_k, rt_i(1));
  }
  RT_AT(5, 867);
  wv v1149_r = rt_bind(rt_chk(s884_tmp(up), "onfailStmt"), 0);
  RT_AT(5, 868);
  (void)rt_chk(s883_emit(up, ({ wv t2364 = rt_text(v1149_r); wv t2365 = rt_text(({ wv t2362 = rt_get(&F_callee, v1146_call); wv t2363 = v1147_args; rt_chk(s926_callC(up, t2362, t2363, rt_b(0)), "onfailStmt"); })); rt_cat(5, (wv[]){ rt_s(&S626), t2364, rt_s(&S627), t2365, rt_s(&S14) }); })), "onfailStmt");
  RT_AT(5, 869);
  (void)rt_chk(s883_emit(up, rt_s(&S691)), "onfailStmt");
  RT_AT(5, 870);
  up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 871);
  wv v1150_h = rt_bind(rt_get(&F_handler, v1145_s), 0);
  RT_AT(5, 872);
  if (rt_cond(rt_ne(rt_get(&F_param, v1150_h), RT_NULL))) {
    RT_AT(5, 872);
    (void)rt_chk(s904_bindDecl(up, ({ wv t2366 = rt_get(&F_paramPos, v1150_h); wv t2367 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2366, t2367); }), rt_s(&S692)), "onfailStmt");
  }
  RT_AT(5, 873);
  (void)rt_chk(s883_emit(up, rt_s(&S693)), "onfailStmt");
  RT_AT(5, 874);
  (void)rt_chk(s883_emit(up, rt_s(&S101)), "onfailStmt");
  RT_AT(5, 875);
  up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 876);
  (void)rt_chk(s897_resets(up, rt_get(&F_body, v1150_h)), "onfailStmt");
  RT_AT(5, 877);
  (void)rt_chk(s896_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1150_h))), "onfailStmt");
  RT_AT(5, 878);
  (void)rt_chk(s883_emit(up, ({ wv t2368 = rt_text(v1149_r); wv t2369 = rt_text(rt_chk(s899_valueOf(up, rt_get(&F_body, v1150_h)), "onfailStmt")); rt_cat(4, (wv[]){ t2368, rt_s(&S627), t2369, rt_s(&S14) }); })), "onfailStmt");
  RT_AT(5, 879);
  up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 880);
  (void)rt_chk(s883_emit(up, rt_s(&S15)), "onfailStmt");
  RT_AT(5, 881);
  up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 882);
  (void)rt_chk(s883_emit(up, rt_s(&S15)), "onfailStmt");
  RT_AT(5, 883);
  wv v1151_rest = rt_bind(v1145_s, 1);
  RT_AT(5, 884);
  rt_set_mem(RT_NULL, &F_handler, v1151_rest);
  RT_AT(5, 885);
  ({ wv t2372 = ({ wv t2370 = v1149_r; wv t2371 = rt_get(&F_pos, v1145_s); rt_rec(0, 4, K63, (wv[]){ rt_s(&S694), t2370, t2371, rt_b(0) }); }); wv t2373 = v1151_rest; rt_set_mem(t2372, &F_e, t2373); });
  RT_AT(5, 886);
  (void)rt_chk(s900_stmt(up, v1151_rest), "onfailStmt");
  RT_RET(RT_NULL);
}

static wv s913_chainStmt(struct fr176 *up, wv v1154_e) {
  rt_enter("chainStmt");
  v1154_e = rt_bind(v1154_e, 0);
  RT_AT(5, 890);
  wv v1155_k = rt_i(0);
  RT_AT(5, 891);
  while (rt_cond((RT_AT(5, 891), ({ wv t2374 = v1155_k; wv t2375 = std_length(rt_get(&F_branches, v1154_e)); rt_lt(t2374, t2375); })))) {
    RT_AT(5, 892);
    wv v1156_b = rt_bind(({ wv t2376 = v1155_k; wv t2377 = rt_get(&F_branches, v1154_e); rt_idx(t2376, t2377); }), 0);
    RT_AT(5, 893);
    (void)rt_chk(s883_emit(up, ({ wv t2378 = rt_text((rt_cond(rt_gt(v1155_k, rt_i(0))) ? rt_s(&S695) : rt_s(&S13))); wv t2379 = rt_text(rt_chk(s914_test(up, v1156_b), "chainStmt")); rt_cat(4, (wv[]){ t2378, rt_s(&S696), t2379, rt_s(&S697) }); })), "chainStmt");
    RT_AT(5, 894);
    up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 895);
    (void)rt_chk(s898_blockBody(up, rt_get(&F_body, v1156_b)), "chainStmt");
    RT_AT(5, 896);
    up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 897);
    v1155_k = rt_add(v1155_k, rt_i(1));
  }
  RT_AT(5, 899);
  if (rt_cond(rt_ne(rt_get(&F_els, v1154_e), RT_NULL))) {
    RT_AT(5, 900);
    (void)rt_chk(s883_emit(up, rt_s(&S698)), "chainStmt");
    RT_AT(5, 901);
    up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 902);
    (void)rt_chk(s898_blockBody(up, rt_get(&F_els, v1154_e)), "chainStmt");
    RT_AT(5, 903);
    up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
  }
  RT_AT(5, 905);
  (void)rt_chk(s883_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s914_test(struct fr176 *up, wv v1157_b) {
  rt_enter("test");
  v1157_b = rt_bind(v1157_b, 0);
  RT_RET((RT_AT(5, 908), ({ wv t2380 = rt_text((rt_cond(rt_get(&F_neg, v1157_b)) ? rt_s(&S95) : rt_s(&S13))); wv t2381 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_cond, v1157_b)), "test"))); rt_cat(4, (wv[]){ t2380, rt_s(&S641), t2381, rt_s(&S16) }); })));
}

static wv s915_branchCount(struct fr176 *up, wv v1158_e) {
  rt_enter("branchCount");
  v1158_e = rt_bind(v1158_e, 0);
  RT_RET((RT_AT(5, 913), (rt_cond(rt_eq(rt_get(&F_k, v1158_e), rt_s(&S186))) ? std_length(rt_get(&F_branches, v1158_e)) : std_length(rt_get(&F_cases, v1158_e)))));
}

static wv s916_branchBody(struct fr176 *up, wv v1159_e, wv v1160_i) {
  rt_enter("branchBody");
  v1159_e = rt_bind(v1159_e, 0);
  v1160_i = rt_bind(v1160_i, 0);
  RT_RET((RT_AT(5, 914), (rt_cond(rt_eq(rt_get(&F_k, v1159_e), rt_s(&S186))) ? rt_get(&F_body, ({ wv t2382 = v1160_i; wv t2383 = rt_get(&F_branches, v1159_e); rt_idx(t2382, t2383); })) : rt_get(&F_body, ({ wv t2384 = v1160_i; wv t2385 = rt_get(&F_cases, v1159_e); rt_idx(t2384, t2385); })))));
}

static wv s917_branchTest(struct fr176 *up, wv v1161_e, wv v1162_t, wv v1163_i) {
  rt_enter("branchTest");
  v1161_e = rt_bind(v1161_e, 0);
  v1162_t = rt_bind(v1162_t, 0);
  v1163_i = rt_bind(v1163_i, 0);
  RT_RET((RT_AT(5, 915), (rt_cond(rt_eq(rt_get(&F_k, v1161_e), rt_s(&S186))) ? rt_chk(s914_test(up, ({ wv t2386 = v1163_i; wv t2387 = rt_get(&F_branches, v1161_e); rt_idx(t2386, t2387); })), "branchTest") : ({ wv t2390 = v1162_t; wv t2391 = ({ wv t2388 = v1163_i; wv t2389 = rt_get(&F_cases, v1161_e); rt_idx(t2388, t2389); }); rt_chk(s922_caseTest(up, t2390, t2391), "branchTest"); }))));
}

static wv s918_isPlain(struct fr176 *up, wv v1164_e) {
  rt_enter("isPlain");
  v1164_e = rt_bind(v1164_e, 0);
  RT_AT(5, 919);
  wv v1165_k = rt_i(0);
  RT_AT(5, 920);
  while (rt_cond((RT_AT(5, 920), ({ wv t2392 = v1165_k; wv t2393 = rt_chk(s915_branchCount(up, v1164_e), "isPlain"); rt_le(t2392, t2393); })))) {
    RT_AT(5, 921);
    wv v1166_b = rt_bind((rt_cond(({ wv t2394 = v1165_k; wv t2395 = rt_chk(s915_branchCount(up, v1164_e), "isPlain"); rt_lt(t2394, t2395); })) ? rt_chk(s916_branchBody(up, v1164_e, v1165_k), "isPlain") : rt_get(&F_els, v1164_e)), 0);
    RT_AT(5, 922);
    wv v1167_j = rt_i(0);
    RT_AT(5, 923);
    while (rt_cond((RT_AT(5, 923), ({ wv t2396 = v1167_j; wv t2397 = std_length(rt_get(&F_stmts, v1166_b)); rt_lt(t2396, t2397); })))) {
      RT_AT(5, 924);
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t2398 = v1167_j; wv t2399 = rt_get(&F_stmts, v1166_b); rt_idx(t2398, t2399); })), rt_s(&S203)))) {
        RT_AT(5, 924);
        RT_RET(rt_b(0));
      }
      RT_AT(5, 925);
      v1167_j = rt_add(v1167_j, rt_i(1));
    }
    RT_AT(5, 927);
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 929), rt_b(1)));
}

static wv s919_ternaryChain(struct fr176 *up, wv v1168_e, wv v1169_t) {
  rt_enter("ternaryChain");
  v1168_e = rt_bind(v1168_e, 0);
  v1169_t = rt_bind(v1169_t, 0);
  RT_AT(5, 934);
  wv v1170_code = rt_bind(rt_chk(s899_valueOf(up, rt_get(&F_els, v1168_e)), "ternaryChain"), 1);
  RT_AT(5, 935);
  wv v1171_k = rt_sub(rt_chk(s915_branchCount(up, v1168_e), "ternaryChain"), rt_i(1));
  RT_AT(5, 936);
  while (rt_cond((RT_AT(5, 936), rt_ge(v1171_k, rt_i(0))))) {
    RT_AT(5, 937);
    v1170_code = ({ wv t2400 = rt_text(rt_chk(s917_branchTest(up, v1168_e, v1169_t, v1171_k), "ternaryChain")); wv t2401 = rt_text(rt_chk(s899_valueOf(up, rt_chk(s916_branchBody(up, v1168_e, v1171_k), "ternaryChain")), "ternaryChain")); wv t2402 = rt_text(v1170_code); rt_cat(7, (wv[]){ rt_s(&S98), t2400, rt_s(&S699), t2401, rt_s(&S700), t2402, rt_s(&S16) }); });
    RT_AT(5, 938);
    v1171_k = rt_sub(v1171_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 940), v1170_code));
}

static wv s920_branchValue(struct fr176 *up, wv v1172_e, wv v1173_t, wv v1174_subject) {
  rt_enter("branchValue");
  v1172_e = rt_bind(v1172_e, 0);
  v1173_t = rt_bind(v1173_t, 0);
  v1174_subject = rt_bind(v1174_subject, 0);
  RT_AT(5, 947);
  wv v1175_start = rt_bind(std_length(rt_def(up->v850_out, "out")), 0);
  RT_AT(5, 948);
  wv v1176_savedIndent = rt_bind(rt_def(up->v851_indent, "indent"), 0);
  RT_AT(5, 949);
  wv v1177_savedLoops = rt_bind(rt_def(up->v852_loops, "loops"), 0);
  RT_AT(5, 950);
  wv v1178_savedIife = rt_bind(rt_def(up->v853_iife, "iife"), 0);
  RT_AT(5, 951);
  wv t2403 = rt_add(v1176_savedIndent, rt_i(1));
  (void)rt_def(up->v851_indent, "indent");
  up->v851_indent = t2403;
  RT_AT(5, 952);
  wv t2404 = rt_i(0);
  (void)rt_def(up->v852_loops, "loops");
  up->v852_loops = t2404;
  RT_AT(5, 953);
  wv t2405 = rt_b(1);
  (void)rt_def(up->v853_iife, "iife");
  up->v853_iife = t2405;
  RT_AT(5, 954);
  if (rt_cond(rt_ne(v1174_subject, RT_NULL))) {
    RT_AT(5, 954);
    (void)rt_chk(s883_emit(up, ({ wv t2406 = rt_text(v1173_t); wv t2407 = rt_text(v1174_subject); rt_cat(5, (wv[]){ rt_s(&S626), t2406, rt_s(&S627), t2407, rt_s(&S14) }); })), "branchValue");
  }
  RT_AT(5, 955);
  wv v1179_r = rt_bind(rt_chk(s884_tmp(up), "branchValue"), 0);
  RT_AT(5, 956);
  (void)rt_chk(s883_emit(up, rt_cat(3, (wv[]){ rt_s(&S626), rt_text(v1179_r), rt_s(&S14) })), "branchValue");
  RT_AT(5, 957);
  wv v1180_k = rt_i(0);
  RT_AT(5, 958);
  while (rt_cond((RT_AT(5, 958), ({ wv t2408 = v1180_k; wv t2409 = rt_chk(s915_branchCount(up, v1172_e), "branchValue"); rt_lt(t2408, t2409); })))) {
    RT_AT(5, 959);
    wv v1182_b = rt_bind(rt_chk(s916_branchBody(up, v1172_e, v1180_k), "branchValue"), 0);
    RT_AT(5, 960);
    (void)rt_chk(s883_emit(up, ({ wv t2410 = rt_text((rt_cond(rt_gt(v1180_k, rt_i(0))) ? rt_s(&S695) : rt_s(&S13))); wv t2411 = rt_text(rt_chk(s917_branchTest(up, v1172_e, v1173_t, v1180_k), "branchValue")); rt_cat(4, (wv[]){ t2410, rt_s(&S696), t2411, rt_s(&S697) }); })), "branchValue");
    RT_AT(5, 961);
    up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 962);
    (void)rt_chk(s897_resets(up, v1182_b), "branchValue");
    RT_AT(5, 963);
    (void)rt_chk(s896_stmts(up, rt_get(&F_stmts, v1182_b)), "branchValue");
    RT_AT(5, 964);
    (void)rt_chk(s883_emit(up, ({ wv t2412 = rt_text(v1179_r); wv t2413 = rt_text(rt_chk(s899_valueOf(up, v1182_b), "branchValue")); rt_cat(4, (wv[]){ t2412, rt_s(&S627), t2413, rt_s(&S14) }); })), "branchValue");
    RT_AT(5, 965);
    up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 966);
    v1180_k = rt_add(v1180_k, rt_i(1));
  }
  RT_AT(5, 968);
  (void)rt_chk(s883_emit(up, rt_s(&S698)), "branchValue");
  RT_AT(5, 969);
  up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 970);
  (void)rt_chk(s897_resets(up, rt_get(&F_els, v1172_e)), "branchValue");
  RT_AT(5, 971);
  (void)rt_chk(s896_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v1172_e))), "branchValue");
  RT_AT(5, 972);
  (void)rt_chk(s883_emit(up, ({ wv t2414 = rt_text(v1179_r); wv t2415 = rt_text(rt_chk(s899_valueOf(up, rt_get(&F_els, v1172_e)), "branchValue")); rt_cat(4, (wv[]){ t2414, rt_s(&S627), t2415, rt_s(&S14) }); })), "branchValue");
  RT_AT(5, 973);
  up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
  RT_AT(5, 974);
  (void)rt_chk(s883_emit(up, rt_s(&S15)), "branchValue");
  RT_AT(5, 975);
  (void)rt_chk(s883_emit(up, rt_cat(2, (wv[]){ rt_text(v1179_r), rt_s(&S14) })), "branchValue");
  RT_AT(5, 976);
  wv v1181_body = rt_bind(std_join(({ wv t2416 = rt_def(up->v850_out, "out"); wv t2417 = v1175_start; wv t2418 = std_length(rt_def(up->v850_out, "out")); std_slice(t2416, t2417, t2418); }), rt_s(&S2)), 0);
  RT_AT(5, 977);
  while (rt_cond((RT_AT(5, 977), ({ wv t2419 = std_length(rt_def(up->v850_out, "out")); wv t2420 = v1175_start; rt_gt(t2419, t2420); })))) {
    RT_AT(5, 977);
    (void)std_pop(rt_def(up->v850_out, "out"));
  }
  RT_AT(5, 978);
  wv t2421 = rt_bind(v1176_savedIndent, 1);
  (void)rt_def(up->v851_indent, "indent");
  up->v851_indent = t2421;
  RT_AT(5, 979);
  wv t2422 = rt_bind(v1177_savedLoops, 1);
  (void)rt_def(up->v852_loops, "loops");
  up->v852_loops = t2422;
  RT_AT(5, 980);
  wv t2423 = rt_bind(v1178_savedIife, 1);
  (void)rt_def(up->v853_iife, "iife");
  up->v853_iife = t2423;
  RT_RET((RT_AT(5, 981), ({ wv t2424 = rt_text(v1181_body); wv t2425 = rt_text(rt_chk(w6_repeat(rt_s(&S614), rt_def(up->v851_indent, "indent")), "branchValue")); rt_cat(5, (wv[]){ rt_s(&S701), t2424, rt_s(&S2), t2425, rt_s(&S702) }); })));
}

static wv s921_chainExpr(struct fr176 *up, wv v1183_e) {
  rt_enter("chainExpr");
  v1183_e = rt_bind(v1183_e, 0);
  RT_AT(5, 987);
  if (rt_cond(rt_chk(s918_isPlain(up, v1183_e), "chainExpr"))) {
    RT_AT(5, 987);
    RT_RET(rt_chk(s919_ternaryChain(up, v1183_e, RT_NULL), "chainExpr"));
  }
  RT_RET((RT_AT(5, 988), rt_chk(s920_branchValue(up, v1183_e, RT_NULL, RT_NULL), "chainExpr")));
}

static wv s922_caseTest(struct fr176 *up, wv v1184_t, wv v1185_a) {
  rt_enter("caseTest");
  v1184_t = rt_bind(v1184_t, 0);
  v1185_a = rt_bind(v1185_a, 0);
  RT_AT(5, 994);
  wv v1186_tests = rt_arr(0, NULL, 1);
  RT_AT(5, 995);
  wv v1187_k = rt_i(0);
  RT_AT(5, 996);
  while (rt_cond((RT_AT(5, 996), ({ wv t2426 = v1187_k; wv t2427 = std_length(rt_get(&F_values, v1185_a)); rt_lt(t2426, t2427); })))) {
    RT_AT(5, 997);
    (void)({ wv t2432 = v1186_tests; wv t2433 = ({ wv t2430 = rt_text(v1184_t); wv t2431 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, ({ wv t2428 = v1187_k; wv t2429 = rt_get(&F_values, v1185_a); rt_idx(t2428, t2429); })), "caseTest"))); rt_cat(5, (wv[]){ rt_s(&S703), t2430, rt_s(&S419), t2431, rt_s(&S16) }); }); std_push(t2432, t2433); });
    RT_AT(5, 998);
    v1187_k = rt_add(v1187_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 1000), std_join(v1186_tests, rt_s(&S704))));
}

static wv s923_matchElse(struct fr176 *up, wv v1188_e, wv v1189_t) {
  rt_enter("matchElse");
  v1188_e = rt_bind(v1188_e, 0);
  v1189_t = rt_bind(v1189_t, 0);
  RT_AT(5, 1006);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v1188_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_cases, v1188_e))))), rt_s(&S192)), "||")))) {
    RT_AT(5, 1006);
    RT_RET(rt_get(&F_els, v1188_e));
  }
  RT_RET((RT_AT(5, 1008), ({ wv t2436 = rt_arr(0, NULL, 0); wv t2437 = ({ wv t2434 = rt_cat(3, (wv[]){ rt_s(&S705), rt_text(v1189_t), rt_s(&S706) }); wv t2435 = rt_get(&F_pos, v1188_e); rt_rec(0, 4, K63, (wv[]){ rt_s(&S694), t2434, t2435, rt_b(0) }); }); rt_rec(0, 3, K22, (wv[]){ t2436, t2437, RT_NULL }); })));
}

static wv s924_matchStmt(struct fr176 *up, wv v1190_e) {
  rt_enter("matchStmt");
  v1190_e = rt_bind(v1190_e, 0);
  RT_AT(5, 1012);
  wv v1191_t = rt_bind(rt_chk(s884_tmp(up), "matchStmt"), 0);
  RT_AT(5, 1013);
  (void)rt_chk(s883_emit(up, ({ wv t2438 = rt_text(v1191_t); wv t2439 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_subject, v1190_e)), "matchStmt"))); rt_cat(5, (wv[]){ rt_s(&S626), t2438, rt_s(&S627), t2439, rt_s(&S14) }); })), "matchStmt");
  RT_AT(5, 1014);
  wv v1192_k = rt_i(0);
  RT_AT(5, 1015);
  while (rt_cond((RT_AT(5, 1015), ({ wv t2440 = v1192_k; wv t2441 = std_length(rt_get(&F_cases, v1190_e)); rt_lt(t2440, t2441); })))) {
    RT_AT(5, 1016);
    (void)rt_chk(s883_emit(up, ({ wv t2446 = rt_text((rt_cond(rt_gt(v1192_k, rt_i(0))) ? rt_s(&S695) : rt_s(&S13))); wv t2447 = rt_text(({ wv t2444 = v1191_t; wv t2445 = ({ wv t2442 = v1192_k; wv t2443 = rt_get(&F_cases, v1190_e); rt_idx(t2442, t2443); }); rt_chk(s922_caseTest(up, t2444, t2445), "matchStmt"); })); rt_cat(4, (wv[]){ t2446, rt_s(&S696), t2447, rt_s(&S697) }); })), "matchStmt");
    RT_AT(5, 1017);
    up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 1018);
    (void)rt_chk(s898_blockBody(up, rt_get(&F_body, ({ wv t2448 = v1192_k; wv t2449 = rt_get(&F_cases, v1190_e); rt_idx(t2448, t2449); }))), "matchStmt");
    RT_AT(5, 1019);
    up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 1020);
    v1192_k = rt_add(v1192_k, rt_i(1));
  }
  RT_AT(5, 1022);
  wv v1193_els = rt_bind(rt_chk(s923_matchElse(up, v1190_e, v1191_t), "matchStmt"), 0);
  RT_AT(5, 1023);
  if (rt_cond(rt_ne(v1193_els, RT_NULL))) {
    RT_AT(5, 1024);
    (void)rt_chk(s883_emit(up, rt_s(&S698)), "matchStmt");
    RT_AT(5, 1025);
    up->v851_indent = rt_add(rt_def(up->v851_indent, "indent"), rt_i(1));
    RT_AT(5, 1026);
    (void)rt_chk(s898_blockBody(up, v1193_els), "matchStmt");
    RT_AT(5, 1027);
    up->v851_indent = rt_sub(rt_def(up->v851_indent, "indent"), rt_i(1));
  }
  RT_AT(5, 1029);
  (void)rt_chk(s883_emit(up, rt_s(&S15)), "matchStmt");
  RT_RET(RT_NULL);
}

static wv s925_matchExpr(struct fr176 *up, wv v1194_e) {
  rt_enter("matchExpr");
  v1194_e = rt_bind(v1194_e, 0);
  RT_AT(5, 1035);
  wv v1195_t = rt_bind(rt_chk(s884_tmp(up), "matchExpr"), 0);
  RT_AT(5, 1036);
  wv v1196_subject = rt_bind(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_subject, v1194_e)), "matchExpr")), 0);
  RT_AT(5, 1037);
  wv v1197_withElse = rt_bind(v1194_e, 1);
  RT_AT(5, 1038);
  ({ wv t2450 = rt_chk(s923_matchElse(up, v1194_e, v1195_t), "matchExpr"); wv t2451 = v1197_withElse; rt_set_mem(t2450, &F_els, t2451); });
  RT_AT(5, 1039);
  if (rt_cond(rt_chk(s918_isPlain(up, v1197_withElse), "matchExpr"))) {
    RT_AT(5, 1039);
    RT_RET(({ wv t2452 = rt_text(v1195_t); wv t2453 = rt_text(v1196_subject); wv t2454 = rt_text(rt_chk(s919_ternaryChain(up, v1197_withElse, v1195_t), "matchExpr")); rt_cat(7, (wv[]){ rt_s(&S707), t2452, rt_s(&S627), t2453, rt_s(&S653), t2454, rt_s(&S629) }); }));
  }
  RT_RET((RT_AT(5, 1040), rt_chk(s920_branchValue(up, v1197_withElse, v1195_t, v1196_subject), "matchExpr")));
}

static wv s926_callC(struct fr176 *up, wv v1198_callee, wv v1199_args, wv v1200_checked) {
  rt_enter("callC");
  v1198_callee = rt_bind(v1198_callee, 0);
  v1200_checked = rt_bind(v1200_checked, 0);
  RT_AT(5, 1045);
  wv v1201_code = rt_s(&S13);
  RT_AT(5, 1046);
  if (rt_cond(rt_eq(rt_get(&F_k, v1198_callee), rt_s(&S157)))) {
    RT_AT(5, 1047);
    v1201_code = ({ wv t2455 = rt_text(rt_get(&F_name, v1198_callee)); wv t2456 = rt_text(std_join(v1199_args, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S708), t2455, rt_s(&S98), t2456, rt_s(&S16) }); });
    RT_AT(5, 1048);
    if (rt_cond(rt_not(({ wv t2457 = rt_def(w6_signalers, "signalers"); wv t2458 = rt_get(&F_name, v1198_callee); rt_chk(w1_contains(t2457, t2458), "callC"); })))) {
      RT_AT(5, 1048);
      RT_RET(v1201_code);
    }
  } else {
    RT_AT(5, 1050);
    wv v1202_f = rt_s(&S13);
    RT_AT(5, 1051);
    wv v1203_parent = RT_NULL;
    RT_AT(5, 1052);
    if (rt_cond(rt_eq(rt_get(&F_k, v1198_callee), rt_s(&S192)))) {
      RT_AT(5, 1053);
      v1202_f = ({ wv t2459 = rt_text(rt_chk(s867_modId(up, rt_chk(s888_stolenFrom(up, rt_get(&F_rec, v1198_callee)), "callC")), "callC")); wv t2460 = rt_text(rt_get(&F_field, v1198_callee)); rt_cat(4, (wv[]){ rt_s(&S612), t2459, rt_s(&S12), t2460 }); });
    } else {
      RT_AT(5, 1055);
      wv v1206_d = rt_bind(({ wv t2461 = rt_get(&F_pos, v1198_callee); wv t2462 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2461, t2462); }), 0);
      RT_AT(5, 1056);
      wv v1207_sf = rt_bind(rt_get(&F_fnIndex, ({ wv t2463 = v1206_d; wv t2464 = rt_def(up->v854_decls, "decls"); rt_idx(t2463, t2464); })), 0);
      RT_AT(5, 1057);
      v1202_f = rt_bind(rt_get(&F_cname, ({ wv t2465 = v1207_sf; wv t2466 = rt_def(up->v855_fns, "fns"); rt_idx(t2465, t2466); })), 1);
      RT_AT(5, 1058);
      wv v1208_pf = rt_bind(rt_get(&F_parent, ({ wv t2467 = v1207_sf; wv t2468 = rt_def(up->v855_fns, "fns"); rt_idx(t2467, t2468); })), 0);
      RT_AT(5, 1059);
      if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2469 = v1208_pf; wv t2470 = rt_def(up->v855_fns, "fns"); rt_idx(t2469, t2470); }))))) {
        RT_AT(5, 1059);
        v1203_parent = rt_bind(v1208_pf, 1);
      }
    }
    RT_AT(5, 1062);
    wv v1204_all = rt_arr(0, NULL, 1);
    RT_AT(5, 1063);
    if (rt_cond(rt_ne(v1203_parent, RT_NULL))) {
      RT_AT(5, 1064);
      (void)({ wv t2479 = v1204_all; wv t2480 = (rt_cond(({ wv t2471 = v1203_parent; wv t2472 = rt_def(up->v863_cur, "cur"); rt_eq(t2471, t2472); })) ? rt_s(&S709) : rt_cat(2, (wv[]){ rt_s(&S622), rt_text(rt_chk(w6_repeat(rt_s(&S623), rt_sub(({ wv t2477 = rt_get(&F_depth, ({ wv t2473 = rt_def(up->v863_cur, "cur"); wv t2474 = rt_def(up->v855_fns, "fns"); rt_idx(t2473, t2474); })); wv t2478 = rt_get(&F_depth, ({ wv t2475 = v1203_parent; wv t2476 = rt_def(up->v855_fns, "fns"); rt_idx(t2475, t2476); })); rt_sub(t2477, t2478); }), rt_i(1))), "callC")) })); std_push(t2479, t2480); });
    }
    RT_AT(5, 1066);
    wv v1205_k = rt_i(0);
    RT_AT(5, 1067);
    while (rt_cond((RT_AT(5, 1067), ({ wv t2481 = v1205_k; wv t2482 = std_length(v1199_args); rt_lt(t2481, t2482); })))) {
      RT_AT(5, 1067);
      (void)({ wv t2483 = v1204_all; wv t2484 = rt_idx(v1205_k, v1199_args); std_push(t2483, t2484); });
      RT_AT(5, 1067);
      v1205_k = rt_add(v1205_k, rt_i(1));
    }
    RT_AT(5, 1068);
    v1201_code = ({ wv t2485 = rt_text(v1202_f); wv t2486 = rt_text(std_join(v1204_all, rt_s(&S419))); rt_cat(4, (wv[]){ t2485, rt_s(&S98), t2486, rt_s(&S16) }); });
  }
  RT_RET((RT_AT(5, 1070), (rt_cond(v1200_checked) ? ({ wv t2491 = rt_text(v1201_code); wv t2492 = rt_text((rt_cond(rt_get(&F_top, ({ wv t2487 = rt_def(up->v863_cur, "cur"); wv t2488 = rt_def(up->v855_fns, "fns"); rt_idx(t2487, t2488); }))) ? rt_s(&S609) : rt_chk(w1_quote(rt_get(&F_name, ({ wv t2489 = rt_def(up->v863_cur, "cur"); wv t2490 = rt_def(up->v855_fns, "fns"); rt_idx(t2489, t2490); }))), "callC"))); rt_cat(5, (wv[]){ rt_s(&S710), t2491, rt_s(&S419), t2492, rt_s(&S16) }); }) : v1201_code)));
}

static wv s927_expr(struct fr176 *up, wv v1209_e) {
  rt_enter("expr");
  RT_AT(5, 1074);
  wv v1210_kind = rt_bind(rt_get(&F_k, v1209_e), 0);
  RT_AT(5, 1075);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S139)))) {
    RT_AT(5, 1075);
    RT_RET(({ wv t2493 = rt_cat(3, (wv[]){ rt_s(&S643), rt_text(rt_get(&F_v, v1209_e)), rt_s(&S16) }); wv t2494 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2493, t2494 }); }));
  }
  RT_AT(5, 1076);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S149)))) {
    RT_AT(5, 1076);
    RT_RET(({ wv t2495 = rt_cat(3, (wv[]){ rt_s(&S711), rt_text(rt_get(&F_v, v1209_e)), rt_s(&S16) }); wv t2496 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2495, t2496 }); }));
  }
  RT_AT(5, 1077);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S173)))) {
    RT_AT(5, 1078);
    wv v1211_parts = rt_arr(0, NULL, 1);
    RT_AT(5, 1079);
    wv v1212_k = rt_i(0);
    RT_AT(5, 1080);
    while (rt_cond((RT_AT(5, 1080), ({ wv t2497 = v1212_k; wv t2498 = std_length(rt_get(&F_parts, v1209_e)); rt_lt(t2497, t2498); })))) {
      RT_AT(5, 1081);
      wv v1214_part = rt_bind(({ wv t2499 = v1212_k; wv t2500 = rt_get(&F_parts, v1209_e); rt_idx(t2499, t2500); }), 0);
      RT_AT(5, 1082);
      if (rt_cond(rt_eq(std_typeOf(v1214_part), rt_s(&S187)))) {
        RT_AT(5, 1083);
        if (rt_cond(rt_ne(v1214_part, rt_s(&S13)))) {
          RT_AT(5, 1083);
          (void)({ wv t2503 = v1211_parts; wv t2504 = ({ wv t2501 = rt_cat(3, (wv[]){ rt_s(&S712), rt_text(rt_chk(s885_str(up, v1214_part), "expr")), rt_s(&S16) }); wv t2502 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2501, t2502 }); }); std_push(t2503, t2504); });
        }
      } else {
        RT_AT(5, 1085);
        (void)({ wv t2507 = v1211_parts; wv t2508 = ({ wv t2505 = rt_cat(3, (wv[]){ rt_s(&S713), rt_text(rt_get(&F_c, rt_chk(s927_expr(up, v1214_part), "expr"))), rt_s(&S16) }); wv t2506 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2505, t2506 }); }); std_push(t2507, t2508); });
      }
      RT_AT(5, 1087);
      v1212_k = rt_add(v1212_k, rt_i(1));
    }
    RT_AT(5, 1089);
    if (rt_cond(rt_eq(std_length(v1211_parts), rt_i(0)))) {
      RT_AT(5, 1089);
      RT_RET(({ wv t2509 = rt_cat(3, (wv[]){ rt_s(&S712), rt_text(rt_chk(s885_str(up, rt_s(&S13)), "expr")), rt_s(&S16) }); wv t2510 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2509, t2510 }); }));
    }
    RT_AT(5, 1090);
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v1211_parts), rt_i(1)), "&&") && rt_bool(({ wv t2511 = rt_get(&F_k, rt_idx(rt_i(0), v1211_parts)); wv t2512 = rt_def(w6_constK, "constK"); rt_eq(t2511, t2512); }), "&&")))) {
      RT_AT(5, 1090);
      RT_RET(rt_idx(rt_i(0), v1211_parts));
    }
    RT_AT(5, 1091);
    wv v1213_sq = rt_bind(rt_chk(s894_seq(up, v1211_parts), "expr"), 0);
    RT_AT(5, 1092);
    RT_RET(({ wv t2515 = v1213_sq; wv t2516 = ({ wv t2513 = rt_text(std_length(rt_get(&F_cs, v1213_sq))); wv t2514 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1213_sq)), "expr")); rt_cat(5, (wv[]){ rt_s(&S714), t2513, rt_s(&S419), t2514, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2515, t2516), "expr"); }));
  }
  RT_AT(5, 1094);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S300)))) {
    RT_AT(5, 1094);
    RT_RET(({ wv t2517 = rt_cat(3, (wv[]){ rt_s(&S715), rt_text((rt_cond(rt_get(&F_v, v1209_e)) ? rt_i(1) : rt_i(0))), rt_s(&S16) }); wv t2518 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2517, t2518 }); }));
  }
  RT_AT(5, 1095);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S52)))) {
    RT_AT(5, 1095);
    RT_RET(rt_rec(0, 2, K60, (wv[]){ rt_s(&S632), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1096);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S53)))) {
    RT_AT(5, 1096);
    RT_RET(rt_rec(0, 2, K60, (wv[]){ rt_s(&S716), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1097);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S54)))) {
    RT_AT(5, 1097);
    RT_RET(rt_rec(0, 2, K60, (wv[]){ rt_s(&S717), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1098);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S214)))) {
    RT_AT(5, 1098);
    RT_RET(rt_chk(s893_read(up, ({ wv t2519 = rt_get(&F_pos, v1209_e); wv t2520 = rt_def(up->v860_curSlot, "curSlot"); rt_idx(t2519, t2520); })), "expr"));
  }
  RT_AT(5, 1099);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S157)))) {
    RT_AT(5, 1099);
    RT_RET(({ wv t2521 = rt_cat(3, (wv[]){ rt_s(&S708), rt_text(rt_get(&F_name, v1209_e)), rt_s(&S718) }); wv t2522 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2521, t2522 }); }));
  }
  RT_AT(5, 1100);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S188)))) {
    RT_AT(5, 1100);
    RT_RET(rt_chk(s909_array(up, v1209_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1101);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S189)))) {
    RT_AT(5, 1101);
    RT_RET(rt_chk(s910_mapLit(up, v1209_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1102);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S190)))) {
    RT_AT(5, 1102);
    RT_RET(rt_chk(s911_record(up, v1209_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1103);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S191)))) {
    RT_AT(5, 1104);
    wv v1215_sq = rt_bind(rt_chk(s894_seq(up, ({ wv t2523 = rt_chk(s927_expr(up, rt_get(&F_index, v1209_e)), "expr"); wv t2524 = rt_chk(s927_expr(up, rt_get(&F_arr, v1209_e)), "expr"); rt_arr(2, (wv[]){ t2523, t2524 }, 0); })), "expr"), 0);
    RT_AT(5, 1105);
    RT_RET(({ wv t2527 = v1215_sq; wv t2528 = ({ wv t2525 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1215_sq))); wv t2526 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1215_sq))); rt_cat(5, (wv[]){ rt_s(&S719), t2525, rt_s(&S419), t2526, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2527, t2528), "expr"); }));
  }
  RT_AT(5, 1107);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S192)))) {
    RT_AT(5, 1110);
    wv v1216_lin = rt_bind(rt_chk(s890_enumOf(up, rt_get(&F_rec, v1209_e)), "expr"), 0);
    RT_AT(5, 1111);
    if (rt_cond(rt_ne(v1216_lin, RT_NULL))) {
      RT_AT(5, 1112);
      wv v1220_ms = rt_bind(rt_get(&F_members, rt_get(&F_node, v1216_lin)), 0);
      RT_AT(5, 1113);
      wv v1221_k = rt_i(0);
      RT_AT(5, 1114);
      while (rt_cond((RT_AT(5, 1114), ({ wv t2529 = rt_get(&F_name, rt_idx(v1221_k, v1220_ms)); wv t2530 = rt_get(&F_field, v1209_e); rt_ne(t2529, t2530); })))) {
        RT_AT(5, 1114);
        v1221_k = rt_add(v1221_k, rt_i(1));
      }
      RT_AT(5, 1115);
      RT_RET(({ wv t2533 = ({ wv t2531 = rt_text(rt_get(&F_c, v1216_lin)); wv t2532 = rt_text(v1221_k); rt_cat(5, (wv[]){ rt_s(&S660), t2531, rt_s(&S99), t2532, rt_s(&S661) }); }); wv t2534 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2533, t2534 }); }));
    }
    RT_AT(5, 1117);
    wv v1217_lib = rt_bind(rt_chk(s888_stolenFrom(up, rt_get(&F_rec, v1209_e)), "expr"), 0);
    RT_AT(5, 1118);
    if (rt_cond(rt_ne(v1217_lib, RT_NULL))) {
      RT_AT(5, 1118);
      RT_RET(({ wv t2537 = ({ wv t2535 = rt_text(rt_chk(s867_modId(up, v1217_lib), "expr")); wv t2536 = rt_text(rt_get(&F_field, v1209_e)); rt_cat(4, (wv[]){ rt_s(&S612), t2535, rt_s(&S12), t2536 }); }); wv t2538 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K60, (wv[]){ t2537, t2538 }); }));
    }
    RT_AT(5, 1119);
    wv v1218_key = ({ wv t2539 = rt_chk(s886_fieldRef(up, rt_get(&F_field, v1209_e)), "expr"); wv t2540 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2539, t2540 }); });
    RT_AT(5, 1120);
    wv v1219_sq = rt_bind(rt_chk(s894_seq(up, ({ wv t2541 = v1218_key; wv t2542 = rt_chk(s927_expr(up, rt_get(&F_rec, v1209_e)), "expr"); rt_arr(2, (wv[]){ t2541, t2542 }, 0); })), "expr"), 0);
    RT_AT(5, 1121);
    RT_RET(({ wv t2545 = v1219_sq; wv t2546 = ({ wv t2543 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1219_sq))); wv t2544 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1219_sq))); rt_cat(5, (wv[]){ rt_s(&S720), t2543, rt_s(&S419), t2544, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2545, t2546), "expr"); }));
  }
  RT_AT(5, 1123);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S193)))) {
    RT_AT(5, 1123);
    RT_RET(({ wv t2549 = ({ wv t2547 = rt_text(rt_chk(w6_unFn(rt_get(&F_op, v1209_e)), "expr")); wv t2548 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_e, v1209_e)), "expr"))); rt_cat(5, (wv[]){ rt_s(&S683), t2547, rt_s(&S98), t2548, rt_s(&S16) }); }); wv t2550 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2549, t2550 }); }));
  }
  RT_AT(5, 1124);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S194)))) {
    RT_AT(5, 1125);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v1209_e), rt_s(&S81)), "||") || rt_bool(rt_eq(rt_get(&F_op, v1209_e), rt_s(&S82)), "||")))) {
      RT_AT(5, 1126);
      RT_RET(({ wv t2556 = ({ wv t2551 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_l, v1209_e)), "expr"))); wv t2552 = rt_text(rt_get(&F_op, v1209_e)); wv t2553 = rt_text(rt_get(&F_op, v1209_e)); wv t2554 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_r, v1209_e)), "expr"))); wv t2555 = rt_text(rt_get(&F_op, v1209_e)); rt_cat(11, (wv[]){ rt_s(&S721), t2551, rt_s(&S722), t2552, rt_s(&S723), t2553, rt_s(&S724), t2554, rt_s(&S722), t2555, rt_s(&S725) }); }); wv t2557 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2556, t2557 }); }));
    }
    RT_AT(5, 1128);
    wv v1222_sq = rt_bind(rt_chk(s894_seq(up, ({ wv t2558 = rt_chk(s927_expr(up, rt_get(&F_l, v1209_e)), "expr"); wv t2559 = rt_chk(s927_expr(up, rt_get(&F_r, v1209_e)), "expr"); rt_arr(2, (wv[]){ t2558, t2559 }, 0); })), "expr"), 0);
    RT_AT(5, 1129);
    RT_RET(({ wv t2563 = v1222_sq; wv t2564 = ({ wv t2560 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v1209_e)), "expr")); wv t2561 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1222_sq))); wv t2562 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1222_sq))); rt_cat(7, (wv[]){ rt_s(&S683), t2560, rt_s(&S98), t2561, rt_s(&S419), t2562, rt_s(&S16) }); }); rt_chk(s895_wrap(up, t2563, t2564), "expr"); }));
  }
  RT_AT(5, 1131);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S195)))) {
    RT_AT(5, 1132);
    RT_RET(({ wv t2568 = ({ wv t2565 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_c, v1209_e)), "expr"))); wv t2566 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_a, v1209_e)), "expr"))); wv t2567 = rt_text(rt_get(&F_c, rt_chk(s927_expr(up, rt_get(&F_b, v1209_e)), "expr"))); rt_cat(7, (wv[]){ rt_s(&S726), t2565, rt_s(&S727), t2566, rt_s(&S700), t2567, rt_s(&S16) }); }); wv t2569 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2568, t2569 }); }));
  }
  RT_AT(5, 1134);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S184)))) {
    RT_AT(5, 1135);
    wv v1223_sq = rt_bind(rt_chk(s894_seq(up, rt_chk(s908_exprs(up, rt_get(&F_args, v1209_e)), "expr")), "expr"), 0);
    RT_AT(5, 1136);
    RT_RET(({ wv t2572 = v1223_sq; wv t2573 = ({ wv t2570 = rt_get(&F_callee, v1209_e); wv t2571 = rt_get(&F_cs, v1223_sq); rt_chk(s926_callC(up, t2570, t2571, rt_b(1)), "expr"); }); rt_chk(s895_wrap(up, t2572, t2573), "expr"); }));
  }
  RT_AT(5, 1138);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S186)))) {
    RT_AT(5, 1138);
    RT_RET(({ wv t2574 = rt_chk(s921_chainExpr(up, v1209_e), "expr"); wv t2575 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2574, t2575 }); }));
  }
  RT_AT(5, 1139);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S45)))) {
    RT_AT(5, 1139);
    RT_RET(({ wv t2576 = rt_chk(s925_matchExpr(up, v1209_e), "expr"); wv t2577 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2576, t2577 }); }));
  }
  RT_AT(5, 1140);
  if (rt_cond(rt_eq(v1210_kind, rt_s(&S694)))) {
    RT_AT(5, 1140);
    RT_RET(({ wv t2578 = rt_get(&F_js, v1209_e); wv t2579 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2578, t2579 }); }));
  }
  RT_AT(5, 1141);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S728), rt_text(v1210_kind) }));
  RT_RET(RT_NULL);
}

static wv s928_assemble(struct fr176 *up) {
  rt_enter("assemble");
  RT_AT(5, 1147);
  wv v1224_all = rt_arr(3, (wv[]){ std_join(std_split(up->v840_runtime, rt_s(&S729)), rt_s(&S2)), rt_s(&S730), rt_s(&S13) }, 1);
  RT_AT(5, 1148);
  wv v1225_k = rt_i(0);
  RT_AT(5, 1149);
  while (rt_cond((RT_AT(5, 1149), ({ wv t2580 = v1225_k; wv t2581 = std_length(rt_def(up->v843_strings, "strings")); rt_lt(t2580, t2581); })))) {
    RT_AT(5, 1149);
    (void)({ wv t2586 = v1224_all; wv t2587 = ({ wv t2584 = rt_text(v1225_k); wv t2585 = rt_text(rt_chk(w6_wstrInit(({ wv t2582 = v1225_k; wv t2583 = rt_def(up->v843_strings, "strings"); rt_idx(t2582, t2583); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S731), t2584, rt_s(&S627), t2585, rt_s(&S14) }); }); std_push(t2586, t2587); });
    RT_AT(5, 1149);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1150);
  v1225_k = rt_i(0);
  RT_AT(5, 1151);
  while (rt_cond((RT_AT(5, 1151), ({ wv t2588 = v1225_k; wv t2589 = std_length(rt_def(up->v844_fields, "fields")); rt_lt(t2588, t2589); })))) {
    RT_AT(5, 1151);
    (void)({ wv t2596 = v1224_all; wv t2597 = ({ wv t2594 = rt_text(({ wv t2590 = v1225_k; wv t2591 = rt_def(up->v844_fields, "fields"); rt_idx(t2590, t2591); })); wv t2595 = rt_text(rt_chk(w6_wstrInit(({ wv t2592 = v1225_k; wv t2593 = rt_def(up->v844_fields, "fields"); rt_idx(t2592, t2593); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S732), t2594, rt_s(&S627), t2595, rt_s(&S14) }); }); std_push(t2596, t2597); });
    RT_AT(5, 1151);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1152);
  v1225_k = rt_i(0);
  RT_AT(5, 1153);
  while (rt_cond((RT_AT(5, 1153), ({ wv t2598 = v1225_k; wv t2599 = std_length(rt_def(up->v845_shapes, "shapes")); rt_lt(t2598, t2599); })))) {
    RT_AT(5, 1154);
    wv v1227_names = rt_bind(std_split(({ wv t2600 = v1225_k; wv t2601 = rt_def(up->v845_shapes, "shapes"); rt_idx(t2600, t2601); }), rt_s(&S97)), 0);
    RT_AT(5, 1155);
    wv v1228_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1156);
    wv v1229_j = rt_i(0);
    RT_AT(5, 1157);
    while (rt_cond((RT_AT(5, 1157), ({ wv t2602 = v1229_j; wv t2603 = std_length(v1227_names); rt_lt(t2602, t2603); })))) {
      RT_AT(5, 1157);
      (void)({ wv t2604 = v1228_refs; wv t2605 = rt_cat(2, (wv[]){ rt_s(&S616), rt_text(rt_idx(v1229_j, v1227_names)) }); std_push(t2604, t2605); });
      RT_AT(5, 1157);
      v1229_j = rt_add(v1229_j, rt_i(1));
    }
    RT_AT(5, 1158);
    (void)({ wv t2608 = v1224_all; wv t2609 = ({ wv t2606 = rt_text(v1225_k); wv t2607 = rt_text(std_join(v1228_refs, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S733), t2606, rt_s(&S618), t2607, rt_s(&S619) }); }); std_push(t2608, t2609); });
    RT_AT(5, 1159);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1161);
  v1225_k = rt_i(0);
  RT_AT(5, 1162);
  while (rt_cond((RT_AT(5, 1162), ({ wv t2610 = v1225_k; wv t2611 = std_length(rt_def(up->v846_enumDefs, "enumDefs")); rt_lt(t2610, t2611); })))) {
    RT_AT(5, 1162);
    (void)({ wv t2614 = v1224_all; wv t2615 = ({ wv t2612 = v1225_k; wv t2613 = rt_def(up->v846_enumDefs, "enumDefs"); rt_idx(t2612, t2613); }); std_push(t2614, t2615); });
    RT_AT(5, 1162);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1163);
  v1225_k = rt_i(0);
  RT_AT(5, 1164);
  while (rt_cond((RT_AT(5, 1164), ({ wv t2616 = v1225_k; wv t2617 = std_length(rt_def(up->v848_frames, "frames")); rt_lt(t2616, t2617); })))) {
    RT_AT(5, 1165);
    wv v1230_f = rt_bind(({ wv t2618 = v1225_k; wv t2619 = rt_def(up->v848_frames, "frames"); rt_idx(t2618, t2619); }), 0);
    RT_AT(5, 1166);
    wv v1231_members = rt_arr(0, NULL, 1);
    RT_AT(5, 1167);
    wv v1232_pf = rt_bind(rt_get(&F_parent, ({ wv t2620 = v1230_f; wv t2621 = rt_def(up->v855_fns, "fns"); rt_idx(t2620, t2621); })), 0);
    RT_AT(5, 1168);
    if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2622 = v1232_pf; wv t2623 = rt_def(up->v855_fns, "fns"); rt_idx(t2622, t2623); }))))) {
      RT_AT(5, 1168);
      (void)({ wv t2624 = v1231_members; wv t2625 = rt_cat(3, (wv[]){ rt_s(&S673), rt_text(v1232_pf), rt_s(&S734) }); std_push(t2624, t2625); });
    }
    RT_AT(5, 1169);
    wv v1233_j = rt_i(0);
    RT_AT(5, 1170);
    while (rt_cond((RT_AT(5, 1170), ({ wv t2626 = v1233_j; wv t2627 = std_length(rt_def(up->v854_decls, "decls")); rt_lt(t2626, t2627); })))) {
      RT_AT(5, 1171);
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(({ wv t2630 = rt_get(&F_func, ({ wv t2628 = v1233_j; wv t2629 = rt_def(up->v854_decls, "decls"); rt_idx(t2628, t2629); })); wv t2631 = v1230_f; rt_eq(t2630, t2631); }), "&&") && rt_bool(rt_get(&F_captured, ({ wv t2632 = v1233_j; wv t2633 = rt_def(up->v854_decls, "decls"); rt_idx(t2632, t2633); })), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_kind, ({ wv t2634 = v1233_j; wv t2635 = rt_def(up->v854_decls, "decls"); rt_idx(t2634, t2635); })), rt_s(&S33)), "&&")))) {
        RT_AT(5, 1172);
        (void)({ wv t2638 = v1231_members; wv t2639 = rt_cat(3, (wv[]){ rt_s(&S626), rt_text(rt_get(&F_cname, ({ wv t2636 = v1233_j; wv t2637 = rt_def(up->v854_decls, "decls"); rt_idx(t2636, t2637); }))), rt_s(&S14) }); std_push(t2638, t2639); });
      }
      RT_AT(5, 1174);
      v1233_j = rt_add(v1233_j, rt_i(1));
    }
    RT_AT(5, 1176);
    if (rt_cond(rt_eq(std_length(v1231_members), rt_i(0)))) {
      RT_AT(5, 1176);
      (void)std_push(v1231_members, rt_s(&S735));
    }
    RT_AT(5, 1177);
    (void)({ wv t2642 = v1224_all; wv t2643 = ({ wv t2640 = rt_text(v1230_f); wv t2641 = rt_text(std_join(v1231_members, rt_s(&S0))); rt_cat(5, (wv[]){ rt_s(&S673), t2640, rt_s(&S736), t2641, rt_s(&S619) }); }); std_push(t2642, t2643); });
    RT_AT(5, 1178);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1180);
  v1225_k = rt_i(0);
  RT_AT(5, 1181);
  while (rt_cond((RT_AT(5, 1181), ({ wv t2644 = v1225_k; wv t2645 = std_length(rt_def(up->v847_globals, "globals")); rt_lt(t2644, t2645); })))) {
    RT_AT(5, 1181);
    (void)({ wv t2648 = v1224_all; wv t2649 = rt_cat(3, (wv[]){ rt_s(&S676), rt_text(({ wv t2646 = v1225_k; wv t2647 = rt_def(up->v847_globals, "globals"); rt_idx(t2646, t2647); })), rt_s(&S14) }); std_push(t2648, t2649); });
    RT_AT(5, 1181);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1182);
  v1225_k = rt_i(0);
  RT_AT(5, 1183);
  while (rt_cond((RT_AT(5, 1183), ({ wv t2650 = v1225_k; wv t2651 = std_length(rt_def(up->v849_functions, "functions")); rt_lt(t2650, t2651); })))) {
    RT_AT(5, 1183);
    (void)({ wv t2654 = v1224_all; wv t2655 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2652 = v1225_k; wv t2653 = rt_def(up->v849_functions, "functions"); rt_idx(t2652, t2653); }))), rt_s(&S14) }); std_push(t2654, t2655); });
    RT_AT(5, 1183);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1184);
  v1225_k = rt_i(0);
  RT_AT(5, 1185);
  while (rt_cond((RT_AT(5, 1185), ({ wv t2656 = v1225_k; wv t2657 = std_length(rt_def(up->v849_functions, "functions")); rt_lt(t2656, t2657); })))) {
    RT_AT(5, 1186);
    (void)std_push(v1224_all, rt_s(&S13));
    RT_AT(5, 1187);
    (void)({ wv t2660 = v1224_all; wv t2661 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2658 = v1225_k; wv t2659 = rt_def(up->v849_functions, "functions"); rt_idx(t2658, t2659); }))), rt_s(&S737) }); std_push(t2660, t2661); });
    RT_AT(5, 1188);
    wv v1234_lines = rt_bind(rt_get(&F_lines, ({ wv t2662 = v1225_k; wv t2663 = rt_def(up->v849_functions, "functions"); rt_idx(t2662, t2663); })), 0);
    RT_AT(5, 1189);
    wv v1235_j = rt_i(0);
    RT_AT(5, 1190);
    while (rt_cond((RT_AT(5, 1190), ({ wv t2664 = v1235_j; wv t2665 = std_length(v1234_lines); rt_lt(t2664, t2665); })))) {
      RT_AT(5, 1190);
      (void)({ wv t2666 = v1224_all; wv t2667 = rt_idx(v1235_j, v1234_lines); std_push(t2666, t2667); });
      RT_AT(5, 1190);
      v1235_j = rt_add(v1235_j, rt_i(1));
    }
    RT_AT(5, 1191);
    (void)std_push(v1224_all, rt_s(&S15));
    RT_AT(5, 1192);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1194);
  (void)std_push(v1224_all, rt_s(&S13));
  RT_AT(5, 1195);
  wv v1226_files = rt_arr(0, NULL, 1);
  RT_AT(5, 1196);
  {
    wfor t2669;
    rt_for_start(&t2669, up->v839_modules, 0);
    for (int64_t t2668 = 0; rt_for_guard(&t2669, up->v839_modules, "modules"), rt_for_more(&t2669, t2668); t2668++) {
      wv v1236_m = rt_for_item(&t2669, t2668);
      RT_AT(5, 1196);
      (void)({ wv t2670 = v1226_files; wv t2671 = rt_chk(w1_quote(rt_get(&F_name, v1236_m)), "assemble"); std_push(t2670, t2671); });
    }
  }
  RT_AT(5, 1197);
  (void)({ wv t2672 = v1224_all; wv t2673 = rt_cat(3, (wv[]){ rt_s(&S738), rt_text(std_join(v1226_files, rt_s(&S419))), rt_s(&S619) }); std_push(t2672, t2673); });
  RT_AT(5, 1198);
  (void)std_push(v1224_all, rt_s(&S13));
  RT_AT(5, 1199);
  (void)std_push(v1224_all, rt_s(&S739));
  RT_AT(5, 1200);
  (void)std_push(v1224_all, rt_s(&S740));
  RT_AT(5, 1201);
  if (rt_cond(rt_gt(std_length(rt_def(up->v844_fields, "fields")), rt_i(0)))) {
    RT_AT(5, 1202);
    wv v1237_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1203);
    v1225_k = rt_i(0);
    RT_AT(5, 1204);
    while (rt_cond((RT_AT(5, 1204), ({ wv t2674 = v1225_k; wv t2675 = std_length(rt_def(up->v844_fields, "fields")); rt_lt(t2674, t2675); })))) {
      RT_AT(5, 1204);
      (void)({ wv t2678 = v1237_refs; wv t2679 = rt_cat(2, (wv[]){ rt_s(&S616), rt_text(({ wv t2676 = v1225_k; wv t2677 = rt_def(up->v844_fields, "fields"); rt_idx(t2676, t2677); })) }); std_push(t2678, t2679); });
      RT_AT(5, 1204);
      v1225_k = rt_add(v1225_k, rt_i(1));
    }
    RT_AT(5, 1205);
    (void)({ wv t2682 = v1224_all; wv t2683 = ({ wv t2680 = rt_text(std_length(rt_def(up->v844_fields, "fields"))); wv t2681 = rt_text(std_join(v1237_refs, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S741), t2680, rt_s(&S742), t2681, rt_s(&S743) }); }); std_push(t2682, t2683); });
  }
  RT_AT(5, 1208);
  if (rt_cond(rt_gt(std_length(rt_def(up->v847_globals, "globals")), rt_i(0)))) {
    RT_AT(5, 1209);
    wv v1238_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1210);
    v1225_k = rt_i(0);
    RT_AT(5, 1211);
    while (rt_cond((RT_AT(5, 1211), ({ wv t2684 = v1225_k; wv t2685 = std_length(rt_def(up->v847_globals, "globals")); rt_lt(t2684, t2685); })))) {
      RT_AT(5, 1211);
      (void)({ wv t2688 = v1238_refs; wv t2689 = rt_cat(2, (wv[]){ rt_s(&S92), rt_text(({ wv t2686 = v1225_k; wv t2687 = rt_def(up->v847_globals, "globals"); rt_idx(t2686, t2687); })) }); std_push(t2688, t2689); });
      RT_AT(5, 1211);
      v1225_k = rt_add(v1225_k, rt_i(1));
    }
    RT_AT(5, 1212);
    (void)({ wv t2692 = v1224_all; wv t2693 = ({ wv t2690 = rt_text(std_length(rt_def(up->v847_globals, "globals"))); wv t2691 = rt_text(std_join(v1238_refs, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S744), t2690, rt_s(&S745), t2691, rt_s(&S743) }); }); std_push(t2692, t2693); });
  }
  RT_AT(5, 1214);
  v1225_k = rt_i(0);
  RT_AT(5, 1215);
  while (rt_cond((RT_AT(5, 1215), ({ wv t2694 = v1225_k; wv t2695 = std_length(rt_def(up->v850_out, "out")); rt_lt(t2694, t2695); })))) {
    RT_AT(5, 1215);
    (void)({ wv t2698 = v1224_all; wv t2699 = ({ wv t2696 = v1225_k; wv t2697 = rt_def(up->v850_out, "out"); rt_idx(t2696, t2697); }); std_push(t2698, t2699); });
    RT_AT(5, 1215);
    v1225_k = rt_add(v1225_k, rt_i(1));
  }
  RT_AT(5, 1216);
  (void)std_push(v1224_all, rt_s(&S15));
  RT_AT(5, 1217);
  (void)std_push(v1224_all, rt_s(&S13));
  RT_RET((RT_AT(5, 1218), std_join(v1224_all, rt_s(&S2))));
}

static wv w7_dumpTokens(wv v1241_toks, wv v1242_indent) {
  rt_enter("dumpTokens");
  v1241_toks = rt_bind(v1241_toks, 0);
  v1242_indent = rt_bind(v1242_indent, 0);
  RT_AT(6, 8);
  wv v1243_k = rt_i(0);
  RT_AT(6, 9);
  while (rt_cond((RT_AT(6, 9), ({ wv t2710 = v1243_k; wv t2711 = std_length(v1241_toks); rt_lt(t2710, t2711); })))) {
    RT_AT(6, 10);
    wv v1244_tk = rt_bind(rt_idx(v1243_k, v1241_toks), 0);
    RT_AT(6, 11);
    wv v1245_line = ({ wv t2712 = rt_text(v1242_indent); wv t2713 = rt_text(rt_get(&F_t, v1244_tk)); wv t2714 = rt_text(rt_get(&F_pos, v1244_tk)); wv t2715 = rt_text(rt_get(&F_end, v1244_tk)); wv t2716 = rt_text(rt_get(&F_sp, v1244_tk)); rt_cat(8, (wv[]){ t2712, t2713, rt_s(&S0), t2714, rt_s(&S0), t2715, rt_s(&S0), t2716 }); });
    RT_AT(6, 12);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1244_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1244_tk), rt_s(&S157)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1244_tk), rt_s(&S112)), "||")))) {
      RT_AT(6, 13);
      v1245_line = ({ wv t2717 = v1245_line; wv t2718 = rt_cat(2, (wv[]){ rt_s(&S0), rt_text(rt_get(&F_v, v1244_tk)) }); rt_add(t2717, t2718); });
    } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1244_tk), rt_s(&S139)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1244_tk), rt_s(&S149)), "||")))) {
      RT_AT(6, 15);
      v1245_line = ({ wv t2721 = v1245_line; wv t2722 = ({ wv t2719 = rt_text(rt_get(&F_text, v1244_tk)); wv t2720 = rt_text(rt_get(&F_v, v1244_tk)); rt_cat(4, (wv[]){ rt_s(&S0), t2719, rt_s(&S0), t2720 }); }); rt_add(t2721, t2722); });
    }
    RT_AT(6, 17);
    (void)std_print(v1245_line);
    RT_AT(6, 18);
    if (rt_cond(rt_eq(rt_get(&F_t, v1244_tk), rt_s(&S173)))) {
      RT_AT(6, 19);
      wv v1246_p = rt_i(0);
      RT_AT(6, 20);
      while (rt_cond((RT_AT(6, 20), ({ wv t2723 = v1246_p; wv t2724 = std_length(rt_get(&F_parts, v1244_tk)); rt_lt(t2723, t2724); })))) {
        RT_AT(6, 21);
        wv v1247_part = rt_bind(({ wv t2725 = v1246_p; wv t2726 = rt_get(&F_parts, v1244_tk); rt_idx(t2725, t2726); }), 0);
        RT_AT(6, 22);
        if (rt_cond(rt_eq(std_typeOf(v1247_part), rt_s(&S187)))) {
          RT_AT(6, 23);
          (void)std_print(({ wv t2727 = rt_text(v1242_indent); wv t2728 = rt_text(rt_chk(w1_quote(v1247_part), "dumpTokens")); rt_cat(3, (wv[]){ t2727, rt_s(&S746), t2728 }); }));
        } else {
          RT_AT(6, 25);
          (void)std_print(({ wv t2729 = rt_text(v1242_indent); wv t2730 = rt_text(rt_get(&F_pos, v1247_part)); rt_cat(3, (wv[]){ t2729, rt_s(&S747), t2730 }); }));
          RT_AT(6, 26);
          (void)({ wv t2731 = rt_get(&F_tokens, v1247_part); wv t2732 = rt_add(v1242_indent, rt_s(&S748)); rt_chk(w7_dumpTokens(t2731, t2732), "dumpTokens"); });
          RT_AT(6, 27);
          (void)std_print(rt_cat(2, (wv[]){ rt_text(v1242_indent), rt_s(&S749) }));
        }
        RT_AT(6, 29);
        v1246_p = rt_add(v1246_p, rt_i(1));
      }
    }
    RT_AT(6, 32);
    v1243_k = rt_add(v1243_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w7_sorted(wv v1248_xs) {
  rt_enter("sorted");
  v1248_xs = rt_bind(v1248_xs, 0);
  RT_AT(6, 41);
  wv v1249_out = rt_arr(0, NULL, 1);
  RT_AT(6, 42);
  wv v1250_k = rt_i(0);
  RT_AT(6, 43);
  while (rt_cond((RT_AT(6, 43), ({ wv t2733 = v1250_k; wv t2734 = std_length(v1248_xs); rt_lt(t2733, t2734); })))) {
    RT_AT(6, 44);
    (void)({ wv t2735 = v1249_out; wv t2736 = rt_idx(v1250_k, v1248_xs); std_push(t2735, t2736); });
    RT_AT(6, 45);
    wv v1251_j = rt_sub(std_length(v1249_out), rt_i(1));
    RT_AT(6, 46);
    while (rt_cond((RT_AT(6, 46), rt_b(rt_bool(rt_gt(v1251_j, rt_i(0)), "&&") && rt_bool(({ wv t2739 = ({ wv t2737 = rt_sub(v1251_j, rt_i(1)); wv t2738 = v1249_out; rt_idx(t2737, t2738); }); wv t2740 = rt_idx(v1251_j, v1249_out); rt_gt(t2739, t2740); }), "&&"))))) {
      RT_AT(6, 47);
      wv v1252_swap = rt_bind(rt_idx(v1251_j, v1249_out), 0);
      RT_AT(6, 48);
      ({ wv t2743 = ({ wv t2741 = rt_sub(v1251_j, rt_i(1)); wv t2742 = v1249_out; rt_idx(t2741, t2742); }); wv t2744 = v1251_j; wv t2745 = v1249_out; rt_set_idx(t2743, t2744, t2745); });
      RT_AT(6, 49);
      ({ wv t2746 = v1252_swap; wv t2747 = rt_sub(v1251_j, rt_i(1)); wv t2748 = v1249_out; rt_set_idx(t2746, t2747, t2748); });
      RT_AT(6, 50);
      v1251_j = rt_sub(v1251_j, rt_i(1));
    }
    RT_AT(6, 52);
    v1250_k = rt_add(v1250_k, rt_i(1));
  }
  RT_RET((RT_AT(6, 54), v1249_out));
}

static wv w7_dumpValue(wv v1253_x, wv v1254_indent) {
  rt_enter("dumpValue");
  v1253_x = rt_bind(v1253_x, 0);
  v1254_indent = rt_bind(v1254_indent, 0);
  RT_AT(6, 58);
  wv v1255_type = rt_bind(std_typeOf(v1253_x), 0);
  RT_AT(6, 59);
  if (rt_cond(rt_eq(v1255_type, rt_s(&S187)))) {
    RT_AT(6, 59);
    RT_RET(rt_chk(w1_quote(v1253_x), "dumpValue"));
  }
  RT_AT(6, 60);
  wv v1256_inner = rt_add(v1254_indent, rt_s(&S614));
  RT_AT(6, 61);
  if (rt_cond(rt_eq(v1255_type, rt_s(&S412)))) {
    RT_AT(6, 62);
    if (rt_cond(rt_eq(std_length(v1253_x), rt_i(0)))) {
      RT_AT(6, 62);
      RT_RET(rt_s(&S750));
    }
    RT_AT(6, 63);
    wv v1257_out = rt_s(&S99);
    RT_AT(6, 64);
    wv v1258_k = rt_i(0);
    RT_AT(6, 65);
    while (rt_cond((RT_AT(6, 65), ({ wv t2749 = v1258_k; wv t2750 = std_length(v1253_x); rt_lt(t2749, t2750); })))) {
      RT_AT(6, 66);
      v1257_out = ({ wv t2755 = ({ wv t2751 = rt_add(v1257_out, rt_s(&S2)); wv t2752 = v1256_inner; rt_add(t2751, t2752); }); wv t2756 = ({ wv t2753 = rt_idx(v1258_k, v1253_x); wv t2754 = v1256_inner; rt_chk(w7_dumpValue(t2753, t2754), "dumpValue"); }); rt_add(t2755, t2756); });
      RT_AT(6, 67);
      v1258_k = rt_add(v1258_k, rt_i(1));
    }
    RT_AT(6, 69);
    RT_RET(rt_add(({ wv t2757 = rt_add(v1257_out, rt_s(&S2)); wv t2758 = v1254_indent; rt_add(t2757, t2758); }), rt_s(&S100)));
  }
  RT_AT(6, 71);
  if (rt_cond(rt_eq(v1255_type, rt_s(&S414)))) {
    RT_AT(6, 72);
    wv v1259_keys = rt_bind(rt_chk(w7_sorted(std_fields(v1253_x)), "dumpValue"), 0);
    RT_AT(6, 73);
    wv v1260_out = rt_s(&S101);
    RT_AT(6, 74);
    wv v1261_k = rt_i(0);
    RT_AT(6, 75);
    while (rt_cond((RT_AT(6, 75), ({ wv t2759 = v1261_k; wv t2760 = std_length(v1259_keys); rt_lt(t2759, t2760); })))) {
      RT_AT(6, 76);
      wv v1262_key = rt_bind(rt_idx(v1261_k, v1259_keys), 0);
      RT_AT(6, 77);
      v1260_out = ({ wv t2767 = ({ wv t2763 = v1260_out; wv t2764 = ({ wv t2761 = rt_text(v1256_inner); wv t2762 = rt_text(v1262_key); rt_cat(4, (wv[]){ rt_s(&S2), t2761, t2762, rt_s(&S29) }); }); rt_add(t2763, t2764); }); wv t2768 = ({ wv t2765 = rt_idx(v1262_key, v1253_x); wv t2766 = v1256_inner; rt_chk(w7_dumpValue(t2765, t2766), "dumpValue"); }); rt_add(t2767, t2768); });
      RT_AT(6, 78);
      v1261_k = rt_add(v1261_k, rt_i(1));
    }
    RT_AT(6, 80);
    RT_RET(({ wv t2769 = v1260_out; wv t2770 = rt_cat(3, (wv[]){ rt_s(&S2), rt_text(v1254_indent), rt_s(&S15) }); rt_add(t2769, t2770); }));
  }
  RT_RET((RT_AT(6, 82), std_toString(v1253_x)));
}

static wv w8_dirOf(wv v1263_path) {
  rt_enter("dirOf");
  v1263_path = rt_bind(v1263_path, 0);
  RT_AT(7, 16);
  wv v1264_cs = rt_bind(std_split(v1263_path, rt_s(&S13)), 0);
  RT_AT(7, 17);
  wv v1265_k = rt_sub(std_length(v1264_cs), rt_i(1));
  RT_AT(7, 18);
  while (rt_cond((RT_AT(7, 18), rt_b(rt_bool(rt_ge(v1265_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1265_k, v1264_cs), rt_s(&S88)), "&&"))))) {
    RT_AT(7, 18);
    v1265_k = rt_sub(v1265_k, rt_i(1));
  }
  RT_RET((RT_AT(7, 19), std_join(({ wv t2771 = v1264_cs; wv t2772 = rt_add(v1265_k, rt_i(1)); std_slice(t2771, rt_i(0), t2772); }), rt_s(&S13))));
}

static wv w8_normalize(wv v1266_path) {
  rt_enter("normalize");
  v1266_path = rt_bind(v1266_path, 0);
  RT_AT(7, 24);
  wv v1267_segs = rt_bind(std_split(v1266_path, rt_s(&S88)), 0);
  RT_AT(7, 25);
  wv v1268_out = rt_arr(0, NULL, 1);
  RT_AT(7, 26);
  wv v1269_k = rt_i(0);
  RT_AT(7, 27);
  while (rt_cond((RT_AT(7, 27), ({ wv t2773 = v1269_k; wv t2774 = std_length(v1267_segs); rt_lt(t2773, t2774); })))) {
    RT_AT(7, 28);
    wv v1270_seg = rt_bind(rt_idx(v1269_k, v1267_segs), 0);
    RT_AT(7, 29);
    v1269_k = rt_add(v1269_k, rt_i(1));
    RT_AT(7, 30);
    if (rt_cond(rt_b(rt_bool(rt_eq(v1270_seg, rt_s(&S130)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1270_seg, rt_s(&S13)), "&&") && rt_bool(rt_gt(std_length(v1268_out), rt_i(0)), "&&")), "||")))) {
      continue;
    }
    RT_AT(7, 31);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1270_seg, rt_s(&S253)), "&&") && rt_bool(rt_gt(std_length(v1268_out), rt_i(0)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1268_out), rt_s(&S253)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1268_out), rt_s(&S13)), "&&")))) {
      RT_AT(7, 32);
      (void)std_pop(v1268_out);
    } else {
      RT_AT(7, 34);
      (void)std_push(v1268_out, v1270_seg);
    }
  }
  RT_RET((RT_AT(7, 37), std_join(v1268_out, rt_s(&S88))));
}

static wv w8_with(wv v1271_xs, wv v1272_x) {
  rt_enter("with");
  v1271_xs = rt_bind(v1271_xs, 0);
  v1272_x = rt_bind(v1272_x, 0);
  RT_AT(7, 42);
  wv v1273_out = rt_arr(0, NULL, 1);
  RT_AT(7, 43);
  wv v1274_k = rt_i(0);
  RT_AT(7, 44);
  while (rt_cond((RT_AT(7, 44), ({ wv t2775 = v1274_k; wv t2776 = std_length(v1271_xs); rt_lt(t2775, t2776); })))) {
    RT_AT(7, 45);
    (void)({ wv t2777 = v1273_out; wv t2778 = rt_idx(v1274_k, v1271_xs); std_push(t2777, t2778); });
    RT_AT(7, 46);
    v1274_k = rt_add(v1274_k, rt_i(1));
  }
  RT_AT(7, 48);
  (void)std_push(v1273_out, v1272_x);
  RT_RET((RT_AT(7, 49), v1273_out));
}

static wv w8_failIn(wv v1275_file, wv v1276_chars, wv v1277_pos, wv v1278_message) {
  rt_enter("failIn");
  v1275_file = rt_bind(v1275_file, 0);
  v1276_chars = rt_bind(v1276_chars, 0);
  v1277_pos = rt_bind(v1277_pos, 0);
  v1278_message = rt_bind(v1278_message, 0);
  RT_AT(7, 54);
  (void)rt_chk(w2_setSource(v1275_file, v1276_chars), "failIn");
  RT_AT(7, 55);
  (void)rt_chk(w2_abort(v1277_pos, v1278_message), "failIn");
  RT_RET(RT_NULL);
}

static wv w8_loadProgram(wv v1279_entryFile, wv v1280_entryChars, wv p_v1281_libDir, wv p_v1282_searched) {
  rt_enter("loadProgram");
  struct fr250 fr = { 0 };
  v1279_entryFile = rt_bind(v1279_entryFile, 0);
  fr.v1281_libDir = rt_bind(p_v1281_libDir, 0);
  fr.v1282_searched = rt_bind(p_v1282_searched, 0);
  RT_AT(7, 64);
  fr.v1283_modules = rt_arr(0, NULL, 1);
  RT_AT(7, 65);
  fr.v1284_libRoot = rt_bind((rt_cond(rt_eq(fr.v1281_libDir, RT_NULL)) ? RT_NULL : rt_add(rt_chk(w8_normalize(fr.v1281_libDir), "loadProgram"), rt_s(&S88))), 0);
  RT_AT(7, 131);
  wv v1289_path = rt_bind(rt_chk(w8_normalize(std_join(std_split(v1279_entryFile, rt_s(&S19)), rt_s(&S88))), "loadProgram"), 0);
  RT_AT(7, 132);
  (void)({ wv t2851 = v1279_entryFile; wv t2852 = v1280_entryChars; wv t2853 = v1289_path; wv t2854 = rt_arr(1, (wv[]){ v1289_path }, 0); rt_chk(s1288_visit(&fr, t2851, t2852, t2853, t2854), "loadProgram"); });
  RT_RET((RT_AT(7, 133), fr.v1283_modules));
}

static wv s1285_traceName(struct fr250 *up, wv v1290_path) {
  rt_enter("traceName");
  v1290_path = rt_bind(v1290_path, 0);
  RT_AT(7, 69);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_def(up->v1284_libRoot, "libRoot"), RT_NULL), "||") || rt_bool(({ wv t2779 = std_length(v1290_path); wv t2780 = std_length(rt_def(up->v1284_libRoot, "libRoot")); rt_le(t2779, t2780); }), "||")))) {
    RT_AT(7, 69);
    RT_RET(v1290_path);
  }
  RT_AT(7, 70);
  if (rt_cond(({ wv t2783 = ({ wv t2781 = v1290_path; wv t2782 = std_length(rt_def(up->v1284_libRoot, "libRoot")); std_slice(t2781, rt_i(0), t2782); }); wv t2784 = rt_def(up->v1284_libRoot, "libRoot"); rt_ne(t2783, t2784); }))) {
    RT_AT(7, 70);
    RT_RET(v1290_path);
  }
  RT_RET((RT_AT(7, 71), rt_add(rt_s(&S250), ({ wv t2785 = v1290_path; wv t2786 = std_length(rt_def(up->v1284_libRoot, "libRoot")); wv t2787 = std_length(v1290_path); std_slice(t2785, t2786, t2787); }))));
}

static wv s1286_target(struct fr250 *up, wv v1291_s, wv v1292_file, wv v1293_chars, wv v1294_path) {
  rt_enter("target");
  v1291_s = rt_bind(v1291_s, 0);
  v1292_file = rt_bind(v1292_file, 0);
  v1293_chars = rt_bind(v1293_chars, 0);
  v1294_path = rt_bind(v1294_path, 0);
  RT_AT(7, 77);
  if (rt_cond(rt_ne(std_at(rt_get(&F_path, v1291_s), rt_i(0)), rt_s(&S106)))) {
    RT_AT(7, 77);
    RT_RET(rt_chk(w8_normalize(({ wv t2788 = rt_chk(w8_dirOf(v1294_path), "target"); wv t2789 = rt_get(&F_path, v1291_s); rt_add(t2788, t2789); })), "target"));
  }
  RT_AT(7, 78);
  if (rt_cond(rt_eq(up->v1281_libDir, RT_NULL))) {
    RT_AT(7, 79);
    (void)({ wv t2790 = v1292_file; wv t2791 = v1293_chars; wv t2792 = rt_get(&F_pathPos, v1291_s); wv t2793 = rt_cat(2, (wv[]){ rt_s(&S751), rt_text(std_join(up->v1282_searched, rt_s(&S430))) }); rt_chk(w8_failIn(t2790, t2791, t2792, t2793), "target"); });
  }
  RT_RET((RT_AT(7, 81), rt_chk(w8_normalize(({ wv t2796 = up->v1281_libDir; wv t2797 = ({ wv t2794 = rt_get(&F_path, v1291_s); wv t2795 = std_length(rt_get(&F_path, v1291_s)); std_slice(t2794, rt_i(5), t2795); }); rt_add(t2796, t2797); })), "target")));
}

static wv s1287_loaded(struct fr250 *up, wv v1295_path) {
  rt_enter("loaded");
  v1295_path = rt_bind(v1295_path, 0);
  RT_AT(7, 85);
  wv v1296_k = rt_i(0);
  RT_AT(7, 86);
  while (rt_cond((RT_AT(7, 86), ({ wv t2798 = v1296_k; wv t2799 = std_length(rt_def(up->v1283_modules, "modules")); rt_lt(t2798, t2799); })))) {
    RT_AT(7, 87);
    if (rt_cond(({ wv t2802 = rt_get(&F_path, ({ wv t2800 = v1296_k; wv t2801 = rt_def(up->v1283_modules, "modules"); rt_idx(t2800, t2801); })); wv t2803 = v1295_path; rt_eq(t2802, t2803); }))) {
      RT_AT(7, 87);
      RT_RET(v1296_k);
    }
    RT_AT(7, 88);
    v1296_k = rt_add(v1296_k, rt_i(1));
  }
  RT_RET((RT_AT(7, 90), RT_NULL));
}

static wv s1288_visit(struct fr250 *up, wv v1297_file, wv v1298_chars, wv v1299_path, wv v1300_chain) {
  rt_enter("visit");
  v1297_file = rt_bind(v1297_file, 0);
  v1298_chars = rt_bind(v1298_chars, 0);
  v1299_path = rt_bind(v1299_path, 0);
  v1300_chain = rt_bind(v1300_chain, 0);
  RT_AT(7, 96);
  (void)rt_chk(w2_setSource(v1297_file, v1298_chars), "visit");
  RT_AT(7, 97);
  wv v1301_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(v1298_chars), "visit")), "visit"), 0);
  RT_AT(7, 98);
  wv v1302_imports = rt_arr(0, NULL, 1);
  RT_AT(7, 99);
  wv v1303_k = rt_i(0);
  RT_AT(7, 100);
  while (rt_cond((RT_AT(7, 100), ({ wv t2804 = v1303_k; wv t2805 = std_length(rt_get(&F_stmts, v1301_program)); rt_lt(t2804, t2805); })))) {
    RT_AT(7, 101);
    wv v1306_s = rt_bind(({ wv t2806 = v1303_k; wv t2807 = rt_get(&F_stmts, v1301_program); rt_idx(t2806, t2807); }), 0);
    RT_AT(7, 102);
    v1303_k = rt_add(v1303_k, rt_i(1));
    RT_AT(7, 103);
    if (rt_cond(rt_ne(rt_get(&F_k, v1306_s), rt_s(&S48)))) {
      continue;
    }
    RT_AT(7, 104);
    wv v1307_libPath = rt_bind(rt_chk(s1286_target(up, v1306_s, v1297_file, v1298_chars, v1299_path), "visit"), 0);
    RT_AT(7, 105);
    wv v1308_at = rt_i(0);
    RT_AT(7, 106);
    while (rt_cond((RT_AT(7, 106), rt_b(rt_bool(({ wv t2808 = v1308_at; wv t2809 = std_length(v1300_chain); rt_lt(t2808, t2809); }), "&&") && rt_bool(({ wv t2810 = rt_idx(v1308_at, v1300_chain); wv t2811 = v1307_libPath; rt_ne(t2810, t2811); }), "&&"))))) {
      RT_AT(7, 106);
      v1308_at = rt_add(v1308_at, rt_i(1));
    }
    RT_AT(7, 107);
    if (rt_cond(({ wv t2812 = v1308_at; wv t2813 = std_length(v1300_chain); rt_lt(t2812, t2813); }))) {
      RT_AT(7, 108);
      wv v1310_cycle = rt_bind(({ wv t2817 = ({ wv t2814 = v1300_chain; wv t2815 = v1308_at; wv t2816 = std_length(v1300_chain); std_slice(t2814, t2815, t2816); }); wv t2818 = v1307_libPath; rt_chk(w8_with(t2817, t2818), "visit"); }), 0);
      RT_AT(7, 109);
      (void)({ wv t2819 = v1297_file; wv t2820 = v1298_chars; wv t2821 = rt_get(&F_pathPos, v1306_s); wv t2822 = rt_cat(2, (wv[]){ rt_s(&S752), rt_text(std_join(v1310_cycle, rt_s(&S753))) }); rt_chk(w8_failIn(t2819, t2820, t2821, t2822), "visit"); });
    }
    RT_AT(7, 111);
    wv v1309_lib = rt_bind(rt_chk(s1287_loaded(up, v1307_libPath), "visit"), 1);
    RT_AT(7, 112);
    if (rt_cond(rt_eq(v1309_lib, RT_NULL))) {
      RT_AT(7, 113);
      wv v1311_missing = rt_bind((rt_cond(rt_eq(std_at(rt_get(&F_path, v1306_s), rt_i(0)), rt_s(&S106))) ? ({ wv t2825 = rt_text(({ wv t2823 = rt_get(&F_path, v1306_s); wv t2824 = std_length(rt_get(&F_path, v1306_s)); std_slice(t2823, rt_i(5), t2824); })); wv t2826 = rt_text(v1307_libPath); rt_cat(5, (wv[]){ rt_s(&S754), t2825, rt_s(&S755), t2826, rt_s(&S16) }); }) : rt_cat(3, (wv[]){ rt_s(&S756), rt_text(v1307_libPath), rt_s(&S114) })), 0);
      RT_AT(7, 114);
      wv t2827 = std_readFile(v1307_libPath);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          RT_AT(7, 115);
          (void)({ wv t2828 = v1297_file; wv t2829 = v1298_chars; wv t2830 = rt_get(&F_pathPos, v1306_s); wv t2831 = v1311_missing; rt_chk(w8_failIn(t2828, t2829, t2830, t2831), "visit"); });
          t2827 = (RT_AT(7, 115), RT_NULL);
        }
      }
      RT_AT(7, 114);
      wv v1312_text = rt_bind(t2827, 0);
      RT_AT(7, 116);
      wv v1313_cs = rt_bind(std_split(v1312_text, rt_s(&S13)), 0);
      RT_AT(7, 117);
      wv v1314_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1313_cs), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1313_cs), rt_s(&S757)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
      RT_AT(7, 118);
      v1309_lib = rt_bind(({ wv t2835 = v1307_libPath; wv t2836 = ({ wv t2832 = v1313_cs; wv t2833 = v1314_bom; wv t2834 = std_length(v1313_cs); std_slice(t2832, t2833, t2834); }); wv t2837 = v1307_libPath; wv t2838 = rt_chk(w8_with(v1300_chain, v1307_libPath), "visit"); rt_chk(s1288_visit(up, t2835, t2836, t2837, t2838), "visit"); }), 1);
    }
    RT_AT(7, 120);
    (void)({ wv t2839 = v1302_imports; wv t2840 = rt_rec(0, 2, K64, (wv[]){ v1306_s, v1309_lib }); std_push(t2839, t2840); });
  }
  RT_AT(7, 122);
  wv v1304_refTargets = rt_arr(0, NULL, 1);
  RT_AT(7, 123);
  wv v1305_mod = ({ wv t2841 = v1297_file; wv t2842 = v1298_chars; wv t2843 = v1299_path; wv t2844 = rt_chk(s1285_traceName(up, v1299_path), "visit"); wv t2845 = v1301_program; wv t2846 = v1302_imports; wv t2847 = rt_arr(0, NULL, 0); wv t2848 = v1304_refTargets; rt_rec(1, 9, K65, (wv[]){ t2841, t2842, t2843, t2844, t2845, t2846, t2847, rt_b(0), t2848 }); });
  RT_AT(7, 127);
  (void)({ wv t2849 = rt_def(up->v1283_modules, "modules"); wv t2850 = v1305_mod; std_push(t2849, t2850); });
  RT_RET((RT_AT(7, 128), rt_sub(std_length(rt_def(up->v1283_modules, "modules")), rt_i(1))));
}

static wv w_die(void) {
  rt_enter("die");
  RT_AT(8, 36);
  (void)std_eprint(rt_def(w_usage, "usage"));
  RT_AT(8, 37);
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w_load(wv v1315_file, wv v1316_from) {
  rt_enter("load");
  v1315_file = rt_bind(v1315_file, 0);
  v1316_from = rt_bind(v1316_from, 0);
  RT_AT(8, 43);
  wv t2855 = std_readFile(v1316_from);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 44);
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S764), rt_text(v1316_from) }));
      RT_AT(8, 44);
      rt_exit(rt_i(2));
      t2855 = RT_NULL;
    }
  }
  RT_AT(8, 43);
  wv v1317_text = rt_bind(t2855, 0);
  RT_AT(8, 45);
  wv v1318_chars = rt_bind(std_split(v1317_text, rt_s(&S13)), 0);
  RT_AT(8, 46);
  wv v1319_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1318_chars), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1318_chars), rt_s(&S757)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
  RT_AT(8, 47);
  (void)({ wv t2859 = v1315_file; wv t2860 = ({ wv t2856 = v1318_chars; wv t2857 = v1319_bom; wv t2858 = std_length(v1318_chars); std_slice(t2856, t2857, t2858); }); rt_chk(w2_setSource(t2859, t2860), "load"); });
  RT_RET(RT_NULL);
}

static wv w_onWindows(void) {
  rt_enter("onWindows");
  RT_AT(8, 93);
  wv t2865 = rt_s(&S774);
  wv t2866 = std_env(t2865);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2866 = (RT_AT(8, 94), rt_s(&S13));
    }
  }
  RT_AT(8, 93);
  wv v1321_os = rt_bind(t2866, 0);
  RT_RET((RT_AT(8, 95), rt_eq(v1321_os, rt_s(&S775))));
}

static wv w_tryRead(wv v1322_path) {
  rt_enter("tryRead");
  v1322_path = rt_bind(v1322_path, 0);
  RT_AT(8, 112);
  wv t2873 = std_readFile(v1322_path);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2873 = (RT_AT(8, 113), RT_NULL);
    }
  }
  RT_AT(8, 112);
  wv v1323_text = rt_bind(t2873, 0);
  RT_RET((RT_AT(8, 114), v1323_text));
}

static wv w_programDir(void) {
  rt_enter("programDir");
  RT_AT(8, 119);
  wv v1324_cs = rt_bind(std_split(std_programPath(), rt_s(&S13)), 0);
  RT_AT(8, 120);
  wv v1325_k = rt_sub(std_length(v1324_cs), rt_i(1));
  RT_AT(8, 121);
  while (rt_cond((RT_AT(8, 121), rt_b(rt_bool(rt_b(rt_bool(rt_ge(v1325_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1325_k, v1324_cs), rt_s(&S88)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(v1325_k, v1324_cs), rt_s(&S19)), "&&"))))) {
    RT_AT(8, 121);
    v1325_k = rt_sub(v1325_k, rt_i(1));
  }
  RT_RET((RT_AT(8, 122), std_join(({ wv t2874 = v1324_cs; wv t2875 = rt_add(v1325_k, rt_i(1)); std_slice(t2874, rt_i(0), t2875); }), rt_s(&S13))));
}

static wv w_readRuntime(void) {
  rt_enter("readRuntime");
  RT_AT(8, 129);
  wv v1326_text = rt_bind(rt_chk(w_tryRead(rt_def(w_runtimePath, "runtimePath")), "readRuntime"), 0);
  RT_AT(8, 130);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1326_text, RT_NULL), "||") || rt_bool(rt_def(w_runtimeGiven, "runtimeGiven"), "||")))) {
    RT_AT(8, 130);
    RT_RET(v1326_text);
  }
  RT_AT(8, 131);
  wv v1327_dir = rt_bind(rt_chk(w_programDir(), "readRuntime"), 0);
  RT_AT(8, 132);
  wv v1328_here = rt_bind(rt_chk(w_tryRead(({ wv t2876 = v1327_dir; wv t2877 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2876, t2877); })), "readRuntime"), 0);
  RT_RET((RT_AT(8, 133), (rt_cond(rt_ne(v1328_here, RT_NULL)) ? v1328_here : rt_chk(w_tryRead(({ wv t2878 = rt_add(v1327_dir, rt_s(&S782)); wv t2879 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2878, t2879); })), "readRuntime"))));
}

static wv w_findLib(void) {
  rt_enter("findLib");
  RT_AT(8, 142);
  wv v1329_dir = rt_bind(std_split(rt_chk(w_programDir(), "findLib"), rt_s(&S19)), 0);
  RT_AT(8, 143);
  wv v1330_slashed = rt_bind(std_join(v1329_dir, rt_s(&S88)), 0);
  RT_AT(8, 144);
  wv v1331_candidates = rt_bind((rt_cond(rt_ne(rt_def(w_libGiven, "libGiven"), rt_s(&S13))) ? rt_arr(1, (wv[]){ std_join(std_split(rt_def(w_libGiven, "libGiven"), rt_s(&S19)), rt_s(&S88)) }, 0) : ({ wv t2880 = rt_add(v1330_slashed, rt_s(&S783)); wv t2881 = rt_add(v1330_slashed, rt_s(&S784)); rt_arr(2, (wv[]){ t2880, t2881 }, 0); })), 0);
  RT_AT(8, 145);
  wv v1332_k = rt_i(0);
  RT_AT(8, 146);
  while (rt_cond((RT_AT(8, 146), ({ wv t2882 = v1332_k; wv t2883 = std_length(v1331_candidates); rt_lt(t2882, t2883); })))) {
    RT_AT(8, 147);
    wv v1333_c = rt_bind(rt_idx(v1332_k, v1331_candidates), 0);
    RT_AT(8, 148);
    (void)({ wv t2884 = rt_def(w_searched, "searched"); wv t2885 = v1333_c; std_push(t2884, t2885); });
    RT_AT(8, 149);
    wv t2886 = std_listDir(v1333_c);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        t2886 = (RT_AT(8, 150), RT_NULL);
      }
    }
    RT_AT(8, 149);
    wv v1334_names = rt_bind(t2886, 0);
    RT_AT(8, 151);
    if (rt_cond(rt_ne(v1334_names, RT_NULL))) {
      RT_AT(8, 151);
      RT_RET((rt_cond(rt_eq(({ wv t2887 = v1333_c; wv t2888 = std_length(v1333_c); std_slice(t2887, rt_i(-1), t2888); }), rt_s(&S88))) ? v1333_c : rt_add(v1333_c, rt_s(&S88))));
    }
    RT_AT(8, 152);
    v1332_k = rt_add(v1332_k, rt_i(1));
  }
  RT_RET((RT_AT(8, 154), RT_NULL));
}

static const char *const W_files[] = { "wvc/text.wvt", "wvc/diag.wvt", "wvc/lexer.wvt", "wvc/parser.wvt", "wvc/checker.wvt", "wvc/codegen_c.wvt", "wvc/dump.wvt", "wvc/loader.wvt", "wvc/wvc.wvt" };

static void wvt_main(void) {
  rt_files = W_files;
  rt_syms(96, (wstr *const[]){ &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars, &F_word, &F_t, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts, &F_tokens, &F_k, &F_e, &F_handler, &F_items, &F_fields, &F_index, &F_arr, &F_rec, &F_l, &F_r, &F_c, &F_a, &F_b, &F_stmts, &F_param, &F_paramPos, &F_body, &F_neg, &F_cond, &F_isVar, &F_isRef, &F_name, &F_namePos, &F_op, &F_target, &F_names, &F_coll, &F_from, &F_to, &F_step, &F_reversed, &F_code, &F_arg, &F_paren, &F_path, &F_pathPos, &F_params, &F_members, &F_value, &F_close, &F_field, &F_callee, &F_args, &F_values, &F_subject, &F_cases, &F_els, &F_branches, &F_arity, &F_elem, &F_types, &F_id, &F_mod, &F_kind, &F_parent, &F_visibleAt, &F_fnId, &F_lib, &F_func, &F_top, &F_loops, &F_guards, &F_key, &F_exports, &F_refTargets, &F_useMod, &F_imports, &F_node, &F_notLibrary, &F_program, &F_children, &F_d, &F_depth, &F_cname, &F_captured, &F_fnIndex, &F_pre, &F_cs, &F_proto, &F_lines, &F_js });
  rt_roots(68, (wv *const[]){ &w2_srcFile, &w2_srcChars, &w2_warnings, &w3_keywords, &w3_reserved, &w3_bangWords, &w3_setOps, &w3_ops, &w3_maxInt, &w4_exprStart, &w4_onfailOk, &w4_callees, &w4_exportable, &w5_stdlib, &w5_neverCompletes, &w5_onfailValueUsed, &w5_tInt, &w5_tFloat, &w5_tBool, &w5_tString, &w5_tNull, &w5_tArray, &w5_tMap, &w5_tNever, &w5_lastDecls, &w5_lastBindingTypes, &w5_lastFnTypes, &w5_stdAccepts, &w5_hexDigits, &w5_libraryTop, &w6_binOps, &w6_binFns, &w6_scalarKinds, &w6_plainKinds, &w6_signalers, &w6_constK, &w6_pureK, &w6_effectK, &w_usage, &w_argv, &w_mode, &w_input, &w_output, &w_runtimePath, &w_cc, &w_libGiven, &w_textGiven, &w_arg, &w_inspect, &w_dotWvt, &w_runtimeGiven, &w_searched, &w_libDir, &w_modules, &w_problems, &w_runtime, &w_code, &w_dotExe, &w_cFile, &w_parts, &w_ccName, &w_flags, &w_status, &v1320_a, &v1335_tokens, &v1336_program, &v1337_p, &v1338_p });
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
  RT_AT(4, 20);
  w5_stdlib = ({ wv t552 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S353), rt_i(1) }); wv t553 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S354), rt_i(1) }); wv t554 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S355), rt_i(1) }); wv t555 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S356), rt_i(0) }); wv t556 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S357), rt_i(0) }); wv t557 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S358), rt_i(1) }); wv t558 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S359), rt_i(0) }); wv t559 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S360), rt_i(0) }); wv t560 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S361), rt_i(1) }); wv t561 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S362), rt_i(2) }); wv t562 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S363), rt_i(2) }); wv t563 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S364), rt_i(1) }); wv t564 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S365), rt_i(1) }); wv t565 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S366), rt_i(1) }); wv t566 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S367), rt_s(&S35) }); wv t567 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S368), rt_i(1) }); wv t568 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S369), rt_s(&S35) }); wv t569 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S370), rt_i(2) }); wv t570 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S371), rt_i(1) }); wv t571 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S372), rt_i(2) }); wv t572 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S373), rt_i(3) }); wv t573 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S374), rt_i(1) }); wv t574 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S375), rt_i(1) }); wv t575 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S376), rt_i(2) }); wv t576 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S377), rt_i(2) }); wv t577 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S378), rt_i(1) }); wv t578 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S379), rt_i(1) }); wv t579 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S380), rt_i(1) }); wv t580 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S381), rt_i(2) }); wv t581 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S382), rt_i(1) }); wv t582 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S383), rt_i(2) }); wv t583 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S384), rt_i(1) }); wv t584 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S385), rt_i(2) }); wv t585 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S386), rt_i(1) }); wv t586 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S387), rt_i(2) }); wv t587 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S388), rt_i(2) }); wv t588 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S389), rt_i(2) }); wv t589 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S390), rt_i(1) }); wv t590 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S391), rt_i(1) }); wv t591 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S392), rt_i(1) }); wv t592 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S393), rt_i(1) }); wv t593 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S394), rt_i(1) }); wv t594 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S395), rt_i(1) }); wv t595 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S396), rt_i(1) }); wv t596 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S397), rt_i(1) }); wv t597 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S398), rt_i(1) }); wv t598 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S399), rt_i(1) }); wv t599 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S400), rt_i(0) }); wv t600 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S401), rt_i(0) }); wv t601 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S402), rt_i(1) }); wv t602 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S403), rt_i(1) }); wv t603 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S404), rt_i(2) }); wv t604 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S405), rt_i(1) }); rt_arr(53, (wv[]){ t552, t553, t554, t555, t556, t557, t558, t559, t560, t561, t562, t563, t564, t565, t566, t567, t568, t569, t570, t571, t572, t573, t574, t575, t576, t577, t578, t579, t580, t581, t582, t583, t584, t585, t586, t587, t588, t589, t590, t591, t592, t593, t594, t595, t596, t597, t598, t599, t600, t601, t602, t603, t604 }, 0); });
  RT_AT(4, 63);
  w5_neverCompletes = rt_arr(6, (wv[]){ rt_s(&S34), rt_s(&S58), rt_s(&S55), rt_s(&S56), rt_s(&S43), rt_s(&S44) }, 0);
  RT_AT(4, 65);
  w5_onfailValueUsed = rt_arr(5, (wv[]){ rt_s(&S179), rt_s(&S38), rt_s(&S34), rt_s(&S58), rt_s(&S55) }, 0);
  RT_AT(4, 142);
  w5_tInt = rt_rec(0, 1, K41, (wv[]){ rt_s(&S139) });
  RT_AT(4, 143);
  w5_tFloat = rt_rec(0, 1, K41, (wv[]){ rt_s(&S149) });
  RT_AT(4, 144);
  w5_tBool = rt_rec(0, 1, K41, (wv[]){ rt_s(&S411) });
  RT_AT(4, 145);
  w5_tString = rt_rec(0, 1, K41, (wv[]){ rt_s(&S187) });
  RT_AT(4, 146);
  w5_tNull = rt_rec(0, 1, K41, (wv[]){ rt_s(&S52) });
  RT_AT(4, 147);
  w5_tArray = rt_rec(0, 2, K42, (wv[]){ rt_s(&S412), RT_NULL });
  RT_AT(4, 148);
  w5_tMap = rt_rec(0, 1, K41, (wv[]){ rt_s(&S189) });
  RT_AT(4, 149);
  w5_tNever = rt_rec(0, 1, K41, (wv[]){ rt_s(&S413) });
  RT_AT(4, 165);
  w5_lastDecls = rt_arr(0, NULL, 1);
  RT_AT(4, 166);
  w5_lastBindingTypes = rt_map(0, NULL, 1);
  RT_AT(4, 167);
  w5_lastFnTypes = rt_map(0, NULL, 1);
  RT_AT(4, 238);
  w5_stdAccepts = ({ wv t665 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S371), rt_arr(1, (wv[]){ rt_arr(3, (wv[]){ rt_s(&S187), rt_s(&S412), rt_s(&S189) }, 0) }, 0) }); wv t666 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S372), ({ wv t654 = rt_arr(2, (wv[]){ rt_s(&S187), rt_s(&S412) }, 0); wv t655 = rt_arr(1, (wv[]){ rt_s(&S139) }, 0); rt_arr(2, (wv[]){ t654, t655 }, 0); }) }); wv t667 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S373), ({ wv t656 = rt_arr(2, (wv[]){ rt_s(&S187), rt_s(&S412) }, 0); wv t657 = rt_arr(1, (wv[]){ rt_s(&S139) }, 0); wv t658 = rt_arr(1, (wv[]){ rt_s(&S139) }, 0); rt_arr(3, (wv[]){ t656, t657, t658 }, 0); }) }); wv t668 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S374), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S187) }, 0) }, 0) }); wv t669 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S375), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S139) }, 0) }, 0) }); wv t670 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S376), ({ wv t659 = rt_arr(1, (wv[]){ rt_s(&S187) }, 0); wv t660 = rt_arr(1, (wv[]){ rt_s(&S187) }, 0); rt_arr(2, (wv[]){ t659, t660 }, 0); }) }); wv t671 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S377), ({ wv t661 = rt_arr(1, (wv[]){ rt_s(&S412) }, 0); wv t662 = rt_arr(1, (wv[]){ rt_s(&S187) }, 0); rt_arr(2, (wv[]){ t661, t662 }, 0); }) }); wv t672 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S381), rt_arr(2, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S412) }, 0), RT_NULL }, 0) }); wv t673 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S382), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S412) }, 0) }, 0) }); wv t674 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S384), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S412) }, 0) }, 0) }); wv t675 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S385), rt_arr(2, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S412) }, 0), RT_NULL }, 0) }); wv t676 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S386), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S412) }, 0) }, 0) }); wv t677 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S387), rt_arr(2, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S189) }, 0), RT_NULL }, 0) }); wv t678 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S388), rt_arr(2, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S189) }, 0), RT_NULL }, 0) }); wv t679 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S389), rt_arr(2, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S189) }, 0), RT_NULL }, 0) }); wv t680 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S390), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S189) }, 0) }, 0) }); wv t681 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S391), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S189) }, 0) }, 0) }); wv t682 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S393), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S414) }, 0) }, 0) }); wv t683 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S394), rt_arr(1, (wv[]){ rt_arr(2, (wv[]){ rt_s(&S139), rt_s(&S149) }, 0) }, 0) }); wv t684 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S395), rt_arr(1, (wv[]){ rt_arr(2, (wv[]){ rt_s(&S139), rt_s(&S149) }, 0) }, 0) }); wv t685 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S396), rt_arr(1, (wv[]){ rt_arr(2, (wv[]){ rt_s(&S139), rt_s(&S149) }, 0) }, 0) }); wv t686 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S397), rt_arr(1, (wv[]){ rt_arr(2, (wv[]){ rt_s(&S139), rt_s(&S149) }, 0) }, 0) }); wv t687 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S398), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S139) }, 0) }, 0) }); wv t688 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S361), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S187) }, 0) }, 0) }); wv t689 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S364), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S187) }, 0) }, 0) }); wv t690 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S365), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S187) }, 0) }, 0) }); wv t691 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S366), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S187) }, 0) }, 0) }); wv t692 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S368), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S187) }, 0) }, 0) }); wv t693 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S370), ({ wv t663 = rt_arr(1, (wv[]){ rt_s(&S187) }, 0); wv t664 = rt_arr(1, (wv[]){ rt_s(&S412) }, 0); rt_arr(2, (wv[]){ t663, t664 }, 0); }) }); wv t694 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S402), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S139) }, 0) }, 0) }); wv t695 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S358), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S139) }, 0) }, 0) }); wv t696 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S403), rt_arr(1, (wv[]){ rt_arr(1, (wv[]){ rt_s(&S139) }, 0) }, 0) }); wv t697 = rt_rec(0, 2, K44, (wv[]){ rt_s(&S404), rt_arr(2, (wv[]){ RT_NULL, rt_arr(1, (wv[]){ rt_s(&S139) }, 0) }, 0) }); rt_arr(33, (wv[]){ t665, t666, t667, t668, t669, t670, t671, t672, t673, t674, t675, t676, t677, t678, t679, t680, t681, t682, t683, t684, t685, t686, t687, t688, t689, t690, t691, t692, t693, t694, t695, t696, t697 }, 0); });
  RT_AT(4, 294);
  w5_hexDigits = rt_s(&S17);
  RT_AT(4, 387);
  w5_libraryTop = rt_arr(6, (wv[]){ rt_s(&S203), rt_s(&S48), rt_s(&S33), rt_s(&S179), rt_s(&S47), rt_s(&S206) }, 0);
  RT_AT(5, 13);
  w6_binOps = rt_arr(17, (wv[]){ rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S89), rt_s(&S76), rt_s(&S77), rt_s(&S78), rt_s(&S90), rt_s(&S91), rt_s(&S79), rt_s(&S80), rt_s(&S92), rt_s(&S93), rt_s(&S32), rt_s(&S83), rt_s(&S84) }, 0);
  RT_AT(5, 14);
  w6_binFns = rt_arr(17, (wv[]){ rt_s(&S585), rt_s(&S586), rt_s(&S587), rt_s(&S588), rt_s(&S589), rt_s(&S590), rt_s(&S591), rt_s(&S592), rt_s(&S593), rt_s(&S594), rt_s(&S595), rt_s(&S596), rt_s(&S597), rt_s(&S598), rt_s(&S599), rt_s(&S600), rt_s(&S601) }, 0);
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
  RT_AT(8, 28);
  w_usage = rt_add(rt_add(rt_add(rt_add(rt_add(rt_s(&S758), rt_s(&S759)), rt_s(&S760)), rt_s(&S761)), rt_s(&S762)), rt_s(&S763));
  RT_AT(8, 50);
  w_argv = rt_bind(std_args(), 0);
  RT_AT(8, 51);
  w_mode = rt_s(&S13);
  RT_AT(8, 52);
  w_input = rt_s(&S13);
  RT_AT(8, 53);
  w_output = rt_s(&S13);
  RT_AT(8, 55);
  w_runtimePath = rt_s(&S13);
  RT_AT(8, 56);
  w_cc = rt_s(&S13);
  RT_AT(8, 57);
  w_libGiven = rt_s(&S13);
  RT_AT(8, 59);
  w_textGiven = rt_s(&S13);
  RT_AT(8, 60);
  w_arg = rt_i(0);
  RT_AT(8, 61);
  while (rt_cond((RT_AT(8, 61), ({ wv t2861 = w_arg; wv t2862 = std_length(w_argv); rt_lt(t2861, t2862); })))) {
    RT_AT(8, 62);
    v1320_a = rt_bind(rt_idx(w_arg, w_argv), 0);
    RT_AT(8, 63);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_arg, rt_i(0)), "&&") && rt_bool(rt_eq(v1320_a, rt_s(&S765)), "&&")))) {
      RT_AT(8, 64);
      w_mode = rt_bind(v1320_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1320_a, rt_s(&S766)), "||") || rt_bool(rt_eq(v1320_a, rt_s(&S767)), "||")), "||") || rt_bool(rt_eq(v1320_a, rt_s(&S768)), "||")))) {
      RT_AT(8, 66);
      w_mode = rt_bind(v1320_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1320_a, rt_s(&S769)), "||") || rt_bool(rt_eq(v1320_a, rt_s(&S770)), "||")), "||") || rt_bool(rt_eq(v1320_a, rt_s(&S771)), "||")), "||") || rt_bool(rt_eq(v1320_a, rt_s(&S772)), "||")), "||") || rt_bool(rt_eq(v1320_a, rt_s(&S773)), "||")))) {
      RT_AT(8, 68);
      w_arg = rt_add(w_arg, rt_i(1));
      RT_AT(8, 69);
      if (rt_cond(({ wv t2863 = w_arg; wv t2864 = std_length(w_argv); rt_ge(t2863, t2864); }))) {
        RT_AT(8, 69);
        (void)rt_chk(w_die(), NULL);
      }
      RT_AT(8, 70);
      if (rt_cond(rt_eq(v1320_a, rt_s(&S769)))) {
        RT_AT(8, 71);
        w_output = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1320_a, rt_s(&S770)))) {
        RT_AT(8, 73);
        w_runtimePath = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1320_a, rt_s(&S772)))) {
        RT_AT(8, 75);
        w_libGiven = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1320_a, rt_s(&S773)))) {
        RT_AT(8, 77);
        w_textGiven = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else {
        RT_AT(8, 79);
        w_cc = rt_bind(rt_idx(w_arg, w_argv), 1);
      }
    } else if (rt_cond(rt_b(rt_bool(rt_ne(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(rt_gt(std_length(v1320_a), rt_i(0)), "&&") && rt_bool(rt_eq(std_at(v1320_a, rt_i(0)), rt_s(&S86)), "&&")), "||")))) {
      RT_AT(8, 82);
      (void)rt_chk(w_die(), NULL);
    } else {
      RT_AT(8, 84);
      w_input = rt_bind(v1320_a, 1);
    }
    RT_AT(8, 86);
    w_arg = rt_add(w_arg, rt_i(1));
  }
  RT_AT(8, 88);
  w_inspect = rt_b(rt_bool(rt_eq(w_mode, rt_s(&S766)), "||") || rt_bool(rt_eq(w_mode, rt_s(&S767)), "||"));
  RT_AT(8, 89);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(w_inspect, "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "||") || rt_bool(rt_ne(w_libGiven, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S765)), "&&") && rt_bool(rt_ne(w_cc, rt_s(&S13)), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_eq(w_mode, rt_s(&S768)), "&&") && rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S768)), "&&") && rt_bool(rt_ne(w_textGiven, rt_s(&S13)), "&&")), "||")))) {
    RT_AT(8, 90);
    (void)rt_chk(w_die(), NULL);
  }
  RT_AT(8, 97);
  w_dotWvt = rt_b(rt_bool(rt_ge(std_length(w_input), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t2867 = w_input; wv t2868 = std_length(w_input); std_slice(t2867, rt_i(-4), t2868); }), rt_s(&S248)), "&&"));
  RT_AT(8, 98);
  if (rt_cond(rt_eq(w_mode, rt_s(&S765)))) {
    RT_AT(8, 99);
    if (rt_cond(rt_eq(w_cc, rt_s(&S13)))) {
      RT_AT(8, 99);
      wv t2869 = rt_s(&S776);
      wv t2870 = std_env(t2869);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          t2870 = (RT_AT(8, 99), rt_s(&S777));
        }
      }
      RT_AT(8, 99);
      w_cc = rt_bind(t2870, 1);
    }
    RT_AT(8, 101);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_output, rt_s(&S13)), "&&") && rt_bool(rt_not(w_dotWvt), "&&")))) {
      RT_AT(8, 102);
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S778), rt_text(w_input), rt_s(&S779) }));
      RT_AT(8, 103);
      rt_exit(rt_i(2));
    }
    RT_AT(8, 105);
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      RT_AT(8, 105);
      w_output = ({ wv t2871 = std_slice(w_input, rt_i(0), rt_i(-4)); wv t2872 = (rt_cond(rt_chk(w_onWindows(), NULL)) ? rt_s(&S780) : rt_s(&S13)); rt_add(t2871, t2872); });
    }
  }
  RT_AT(8, 107);
  w_runtimeGiven = rt_ne(w_runtimePath, rt_s(&S13));
  RT_AT(8, 108);
  if (rt_cond(rt_not(w_runtimeGiven))) {
    RT_AT(8, 108);
    w_runtimePath = rt_s(&S781);
  }
  RT_AT(8, 140);
  w_searched = rt_arr(0, NULL, 1);
  RT_AT(8, 157);
  (void)({ wv t2889 = w_input; wv t2890 = (rt_cond(rt_ne(w_textGiven, rt_s(&S13))) ? w_textGiven : w_input); rt_chk(w_load(t2889, t2890), NULL); });
  RT_AT(8, 159);
  if (rt_cond(rt_eq(w_mode, rt_s(&S766)))) {
    RT_AT(8, 160);
    v1335_tokens = rt_bind(rt_chk(w3_lex(w2_srcChars), NULL), 0);
    RT_AT(8, 161);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 162);
    (void)rt_chk(w7_dumpTokens(v1335_tokens, rt_s(&S13)), NULL);
    RT_AT(8, 163);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 165);
  if (rt_cond(rt_eq(w_mode, rt_s(&S767)))) {
    RT_AT(8, 166);
    v1336_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(w2_srcChars), NULL)), NULL), 0);
    RT_AT(8, 167);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 168);
    (void)std_print(rt_chk(w7_dumpValue(v1336_program, rt_s(&S13)), NULL));
    RT_AT(8, 169);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 171);
  w_libDir = rt_bind(rt_chk(w_findLib(), NULL), 0);
  RT_AT(8, 172);
  w_modules = rt_bind(rt_chk(w8_loadProgram(w2_srcFile, w2_srcChars, w_libDir, w_searched), NULL), 0);
  RT_AT(8, 174);
  w_problems = rt_bind(({ wv t2891 = w_modules; wv t2892 = rt_ne(w_mode, rt_s(&S768)); rt_chk(w5_check(t2891, t2892), NULL); }), 0);
  RT_AT(8, 177);
  if (rt_cond(rt_eq(w_mode, rt_s(&S768)))) {
    RT_AT(8, 178);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 179);
    {
      wfor t2894;
      rt_for_start(&t2894, w_problems, 0);
      for (int64_t t2893 = 0; rt_for_guard(&t2894, w_problems, "problems"), rt_for_more(&t2894, t2893); t2893++) {
        v1337_p = rt_for_item(&t2894, t2893);
        RT_AT(8, 179);
        (void)std_eprint(rt_chk(w2_formatDiag(v1337_p), NULL));
      }
    }
    RT_AT(8, 180);
    (void)std_print(rt_chk(w5_typesJson(w_modules), NULL));
    RT_AT(8, 181);
    rt_exit((rt_cond(rt_gt(std_length(w_problems), rt_i(0))) ? rt_i(2) : rt_i(0)));
  }
  RT_AT(8, 183);
  if (rt_cond(rt_gt(std_length(w_problems), rt_i(0)))) {
    RT_AT(8, 184);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 185);
    v1338_p = rt_i(0);
    RT_AT(8, 186);
    while (rt_cond((RT_AT(8, 186), ({ wv t2895 = v1338_p; wv t2896 = std_length(w_problems); rt_lt(t2895, t2896); })))) {
      RT_AT(8, 187);
      (void)std_eprint(rt_chk(w2_formatDiag(rt_idx(v1338_p, w_problems)), NULL));
      RT_AT(8, 188);
      v1338_p = rt_add(v1338_p, rt_i(1));
    }
    RT_AT(8, 190);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 192);
  w_runtime = rt_bind(rt_chk(w_readRuntime(), NULL), 0);
  RT_AT(8, 193);
  if (rt_cond(rt_eq(w_runtime, RT_NULL))) {
    RT_AT(8, 194);
    (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S764), rt_text(w_runtimePath) }));
    RT_AT(8, 195);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 197);
  w_code = rt_bind(rt_chk(w6_generateC(w_modules, w_runtime), NULL), 0);
  RT_AT(8, 198);
  (void)rt_chk(w2_flushWarnings(), NULL);
  RT_AT(8, 199);
  if (rt_cond(rt_ne(w_mode, rt_s(&S765)))) {
    RT_AT(8, 200);
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      RT_AT(8, 200);
      w_output = rt_add((rt_cond(w_dotWvt) ? std_slice(w_input, rt_i(0), rt_i(-4)) : w_input), rt_s(&S785));
    }
    RT_AT(8, 201);
    wv t2897 = std_writeFile(w_output, w_code);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        RT_AT(8, 202);
        (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S786), rt_text(w_output) }));
        RT_AT(8, 202);
        rt_exit(rt_i(2));
        t2897 = RT_NULL;
      }
    }
    RT_AT(8, 201);
    (void)t2897;
    RT_AT(8, 203);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 208);
  w_dotExe = rt_b(rt_bool(rt_ge(std_length(w_output), rt_i(4)), "&&") && rt_bool(rt_eq(rt_chk(w1_toLower(({ wv t2898 = w_output; wv t2899 = std_length(w_output); std_slice(t2898, rt_i(-4), t2899); })), NULL), rt_s(&S780)), "&&"));
  RT_AT(8, 209);
  w_cFile = rt_add((rt_cond(w_dotExe) ? std_slice(w_output, rt_i(0), rt_i(-4)) : w_output), rt_s(&S785));
  RT_AT(8, 210);
  wv t2900 = std_writeFile(w_cFile, w_code);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 211);
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S786), rt_text(w_cFile) }));
      RT_AT(8, 211);
      rt_exit(rt_i(2));
      t2900 = RT_NULL;
    }
  }
  RT_AT(8, 210);
  (void)t2900;
  RT_AT(8, 212);
  w_parts = rt_bind(std_split(std_join(std_split(w_cc, rt_s(&S19)), rt_s(&S88)), rt_s(&S88)), 0);
  RT_AT(8, 213);
  w_ccName = rt_bind(rt_chk(w1_toLower(rt_idx(rt_i(-1), w_parts)), NULL), 0);
  RT_AT(8, 214);
  w_flags = rt_arr(4, (wv[]){ rt_s(&S787), rt_s(&S769), w_output, w_cFile }, 0);
  RT_AT(8, 215);
  wv t2901 = (rt_cond(rt_b(rt_bool(rt_eq(w_ccName, rt_s(&S777)), "||") || rt_bool(rt_eq(w_ccName, rt_s(&S788)), "||"))) ? rt_arr(5, (wv[]){ rt_s(&S789), rt_s(&S787), rt_s(&S769), w_output, w_cFile }, 0) : w_flags);
  wv t2902 = std_run(w_cc, t2901);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 216);
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S790), rt_text(w_cc), rt_s(&S791) }));
      RT_AT(8, 216);
      rt_exit(rt_i(2));
      t2902 = RT_NULL;
    }
  }
  RT_AT(8, 215);
  w_status = rt_bind(t2902, 0);
  RT_AT(8, 217);
  if (rt_cond(rt_ne(w_status, rt_i(0)))) {
    RT_AT(8, 218);
    (void)std_eprint(({ wv t2903 = rt_text(w_cc); wv t2904 = rt_text(w_status); rt_cat(4, (wv[]){ rt_s(&S778), t2903, rt_s(&S792), t2904 }); }));
    RT_AT(8, 219);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 221);
  rt_exit(rt_i(0));
}
