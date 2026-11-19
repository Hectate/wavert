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
static const wstr S482 = { 56, 56, 1, 0, "'fail' outside a function: there is no caller to fail to" };
static const wstr S483 = { 38, 38, 1, 0, "a for loop's step must be an int, got " };
static const wstr S484 = { 4, 4, 1, 0, "loop" };
static const wstr S485 = { 85, 85, 1, 0, "to go through an array, write ;for (x in xs), or ;for (i, x in xs) for each index too" };
static const wstr S486 = { 41, 41, 1, 0, "round it to an int, such as with ($floor)" };
static const wstr S487 = { 35, 35, 1, 0, "a for loop's range needs ints, got " };
static const wstr S488 = { 46, 46, 1, 0, "for its characters, go through (s, \"\")($split)" };
static const wstr S489 = { 42, 42, 1, 0, "for its field names, go through r($fields)" };
static const wstr S490 = { 52, 52, 1, 0, "for the ints from 0 to n - 1, write ;for (i in 0, n)" };
static const wstr S491 = { 73, 73, 1, 0, "a for loop goes through an array, a map, an enum or a range of ints, got " };
static const wstr S492 = { 2, 2, 1, 0, "it" };
static const wstr S493 = { 12, 12, 1, 0, "cannot set '" };
static const wstr S494 = { 25, 25, 1, 0, "' inside a for loop over " };
static const wstr S495 = { 50, 50, 1, 0, "build the new value in another variable, and set '" };
static const wstr S496 = { 22, 22, 1, 0, "' to it after the loop" };
static const wstr S497 = { 65, 65, 1, 0, "'onfail' handles a call, but this statement's value is not a call" };
static const wstr S498 = { 87, 87, 1, 0, "the call must be the outermost expression, e.g. ;const n is s(parse) onfail (c) { ... }" };
static const wstr S499 = { 44, 44, 1, 0, "a match cannot mix literals and enum members" };
static const wstr S500 = { 31, 31, 1, 0, "a match cannot mix members of '" };
static const wstr S501 = { 7, 7, 1, 0, "' and '" };
static const wstr S502 = { 21, 21, 1, 0, "duplicate case value " };
static const wstr S503 = { 7, 7, 1, 0, " (line " };
static const wstr S504 = { 22, 22, 1, 0, "match does not handle " };
static const wstr S505 = { 57, 57, 1, 0, "add a case for each, or end the match with 'else { ... }'" };
static const wstr S506 = { 52, 52, 1, 0, "a match used as a value must end with 'else { ... }'" };
static const wstr S507 = { 6, 6, 1, 0, "module" };
static const wstr S508 = { 19, 19, 1, 0, "' does not export '" };
static const wstr S509 = { 17, 17, 1, 0, "' is not a module" };
static const wstr S510 = { 16, 16, 1, 0, "unknown module '" };
static const wstr S511 = { 41, 41, 1, 0, "' is a function; functions are not values" };
static const wstr S512 = { 20, 20, 1, 0, "to call it, write x(" };
static const wstr S513 = { 7, 7, 1, 0, "), or (" };
static const wstr S514 = { 19, 19, 1, 0, ") with no arguments" };
static const wstr S515 = { 25, 25, 1, 0, "' is an enum, not a value" };
static const wstr S516 = { 24, 24, 1, 0, "its members are written " };
static const wstr S517 = { 7, 7, 1, 0, ".member" };
static const wstr S518 = { 54, 54, 1, 0, "case values are literals or enum members (Enum.member)" };
static const wstr S519 = { 16, 16, 1, 0, "' is not an enum" };
static const wstr S520 = { 14, 14, 1, 0, "unknown enum '" };
static const wstr S521 = { 11, 11, 1, 0, "no member '" };
static const wstr S522 = { 11, 11, 1, 0, "' in enum '" };
static const wstr S523 = { 16, 16, 1, 0, "its members are " };
static const wstr S524 = { 66, 66, 1, 0, "a conditional used as a value must end with a plain 'else { ... }'" };
static const wstr S525 = { 25, 25, 1, 0, "': it is not a var or ref" };
static const wstr S526 = { 7, 7, 1, 0, "a field" };
static const wstr S527 = { 10, 10, 1, 0, "an element" };
static const wstr S528 = { 29, 29, 1, 0, "': enum members are constants" };
static const wstr S529 = { 9, 9, 1, 0, "': only '" };
static const wstr S530 = { 12, 12, 1, 0, "' may set it" };
static const wstr S531 = { 14, 14, 1, 0, "cannot modify " };
static const wstr S532 = { 5, 5, 1, 0, " of '" };
static const wstr S533 = { 18, 18, 1, 0, "': it is not a var" };
static const wstr S534 = { 7, 7, 1, 0, "setting" };
static const wstr S535 = { 6, 6, 1, 0, "' is a" };
static const wstr S536 = { 15, 15, 1, 0, ", not a binding" };
static const wstr S537 = { 38, 38, 1, 0, "': a for loop's variable cannot be set" };
static const wstr S538 = { 121, 121, 1, 0, "to replace an element, set it through its array, such as ;set xs[i] to v; to control a counter yourself, use a while loop" };
static const wstr S539 = { 24, 24, 1, 0, "mark the parameter (ref " };
static const wstr S540 = { 24, 24, 1, 0, ") to rebind it, or (var " };
static const wstr S541 = { 20, 20, 1, 0, ") for a mutable copy" };
static const wstr S542 = { 29, 29, 1, 0, "mark the parameter var: (var " };
static const wstr S543 = { 22, 22, 1, 0, "declare it with ';ref " };
static const wstr S544 = { 32, 32, 1, 0, " is ...' to rebind it, or ';var " };
static const wstr S545 = { 27, 27, 1, 0, " is ...' for a mutable copy" };
static const wstr S546 = { 22, 22, 1, 0, "declare it with ';var " };
static const wstr S547 = { 8, 8, 1, 0, " is ...'" };
static const wstr S548 = { 32, 32, 1, 0, "unknown standard library name '$" };
static const wstr S549 = { 26, 26, 1, 0, "' is a module, not a value" };
static const wstr S550 = { 27, 27, 1, 0, "what it exports is written " };
static const wstr S551 = { 5, 5, 1, 0, ".name" };
static const wstr S552 = { 32, 32, 1, 0, "' is a function; call it with ($" };
static const wstr S553 = { 7, 7, 1, 0, "reading" };
static const wstr S554 = { 25, 25, 1, 0, "'-' cannot be applied to " };
static const wstr S555 = { 30, 30, 1, 0, "'fail' is a statement: ;fail c" };
static const wstr S556 = { 24, 24, 1, 0, "checker: unhandled node " };
static const wstr S557 = { 31, 31, 1, 0, "' is a constant, not a function" };
static const wstr S558 = { 8, 8, 1, 0, "' takes " };
static const wstr S559 = { 6, 6, 1, 0, ", got " };
static const wstr S560 = { 9, 9, 1, 0, " expects " };
static const wstr S561 = { 18, 18, 1, 0, "unknown function '" };
static const wstr S562 = { 71, 71, 1, 0, ") is a call with no arguments; to use the value, remove the parentheses" };
static const wstr S563 = { 53, 53, 1, 0, "calls are postfix: the function comes last, e.g. x(f)" };
static const wstr S564 = { 19, 19, 1, 0, "' is not a function" };
static const wstr S565 = { 21, 21, 1, 0, "' is already imported" };
static const wstr S566 = { 38, 38, 1, 0, "' is not a library: it ends with ;exit" };
static const wstr S567 = { 31, 31, 1, 0, "only a library ends with ';end'" };
static const wstr S568 = { 25, 25, 1, 0, "a program ends with ;exit" };
static const wstr S569 = { 88, 88, 1, 0, "a library's top level may contain only import, fn, const, var, ref and enum declarations" };
static const wstr S570 = { 37, 37, 1, 0, "move other statements into a function" };
static const wstr S571 = { 28, 28, 1, 0, "a library must end with ;end" };
static const wstr S572 = { 56, 56, 1, 0, "mark what it exports with 'export', and end it with ;end" };
static const wstr S573 = { 15, 15, 1, 0, "cannot export '" };
static const wstr S574 = { 38, 38, 1, 0, "': it is not declared at the top level" };
static const wstr S575 = { 17, 17, 1, 0, "': it is a module" };
static const wstr S576 = { 5, 5, 1, 0, "line " };
static const wstr S577 = { 6, 6, 1, 0, " line " };
static const wstr S578 = { 49, 49, 1, 0, "' never produces a value, but its value is used (" };
static const wstr S579 = { 65, 65, 1, 0, "end the function with its value, or with null if that is intended" };
static const wstr S580 = { 29, 29, 1, 0, "a program must end with ;exit" };
static const wstr S581 = { 50, 50, 1, 0, "end the program with ;exit, or a library with ;end" };
static const wstr S582 = { 3, 3, 1, 0, "add" };
static const wstr S583 = { 3, 3, 1, 0, "sub" };
static const wstr S584 = { 3, 3, 1, 0, "mul" };
static const wstr S585 = { 3, 3, 1, 0, "div" };
static const wstr S586 = { 3, 3, 1, 0, "mod" };
static const wstr S587 = { 3, 3, 1, 0, "pow" };
static const wstr S588 = { 2, 2, 1, 0, "eq" };
static const wstr S589 = { 2, 2, 1, 0, "ne" };
static const wstr S590 = { 2, 2, 1, 0, "lt" };
static const wstr S591 = { 2, 2, 1, 0, "gt" };
static const wstr S592 = { 2, 2, 1, 0, "le" };
static const wstr S593 = { 2, 2, 1, 0, "ge" };
static const wstr S594 = { 4, 4, 1, 0, "band" };
static const wstr S595 = { 3, 3, 1, 0, "bor" };
static const wstr S596 = { 4, 4, 1, 0, "bxor" };
static const wstr S597 = { 3, 3, 1, 0, "shl" };
static const wstr S598 = { 3, 3, 1, 0, "shr" };
static const wstr S599 = { 3, 3, 1, 0, "neg" };
static const wstr S600 = { 3, 3, 1, 0, "not" };
static const wstr S601 = { 4, 4, 1, 0, "bnot" };
static const wstr S602 = { 2, 2, 1, 0, "\\\"" };
static const wstr S603 = { 2, 2, 1, 0, "\\\\" };
static const wstr S604 = { 2, 2, 1, 0, "{ " };
static const wstr S605 = { 5, 5, 1, 0, ", 0, " };
static const wstr S606 = { 4, 4, 1, 0, "NULL" };
static const wstr S607 = { 8, 8, 1, 0, "(wv[]){ " };
static const wstr S608 = { 6, 6, 1, 0, "RT_AT(" };
static const wstr S609 = { 1, 1, 1, 0, "w" };
static const wstr S610 = { 1, 1, 1, 0, "v" };
static const wstr S611 = { 2, 2, 1, 0, "  " };
static const wstr S612 = { 2, 2, 1, 0, "&S" };
static const wstr S613 = { 3, 3, 1, 0, "&F_" };
static const wstr S614 = { 19, 19, 1, 0, "static const wmem l" };
static const wstr S615 = { 7, 7, 1, 0, "[] = { " };
static const wstr S616 = { 3, 3, 1, 0, " };" };
static const wstr S617 = { 1, 1, 1, 0, "l" };
static const wstr S618 = { 3, 3, 1, 0, "fr." };
static const wstr S619 = { 2, 2, 1, 0, "up" };
static const wstr S620 = { 4, 4, 1, 0, "->up" };
static const wstr S621 = { 2, 2, 1, 0, "->" };
static const wstr S622 = { 7, 7, 1, 0, "rt_def(" };
static const wstr S623 = { 3, 3, 1, 0, "wv " };
static const wstr S624 = { 3, 3, 1, 0, " = " };
static const wstr S625 = { 3, 3, 1, 0, "({ " };
static const wstr S626 = { 4, 4, 1, 0, "; })" };
static const wstr S627 = { 12, 12, 1, 0, " = RT_UNSET;" };
static const wstr S628 = { 6, 6, 1, 0, "(void)" };
static const wstr S629 = { 7, 7, 1, 0, "RT_NULL" };
static const wstr S630 = { 66, 66, 1, 0, "'return' inside a conditional used as a value is not supported yet" };
static const wstr S631 = { 7, 7, 1, 0, "RT_RET(" };
static const wstr S632 = { 2, 2, 1, 0, ");" };
static const wstr S633 = { 9, 9, 1, 0, "rt_panic(" };
static const wstr S634 = { 48, 48, 1, 0, "' cannot leave a conditional used as a value yet" };
static const wstr S635 = { 6, 6, 1, 0, "break;" };
static const wstr S636 = { 9, 9, 1, 0, "continue;" };
static const wstr S637 = { 7, 7, 1, 0, "while (" };
static const wstr S638 = { 8, 8, 1, 0, "rt_cond(" };
static const wstr S639 = { 4, 4, 1, 0, ")) {" };
static const wstr S640 = { 5, 5, 1, 0, "rt_i(" };
static const wstr S641 = { 8, 8, 1, 0, "rt_exit(" };
static const wstr S642 = { 7, 7, 1, 0, "rt_i(0)" };
static const wstr S643 = { 29, 29, 1, 0, "codegen: unhandled statement " };
static const wstr S644 = { 7, 7, 1, 0, "rt_i(1)" };
static const wstr S645 = { 8, 8, 1, 0, "int64_t " };
static const wstr S646 = { 16, 16, 1, 0, " = rt_for_range(" };
static const wstr S647 = { 13, 13, 1, 0, "for (int64_t " };
static const wstr S648 = { 6, 6, 1, 0, " = 0; " };
static const wstr S649 = { 3, 3, 1, 0, " < " };
static const wstr S650 = { 2, 2, 1, 0, "; " };
static const wstr S651 = { 5, 5, 1, 0, "++) {" };
static const wstr S652 = { 7, 7, 1, 0, " - 1 - " };
static const wstr S653 = { 6, 6, 1, 0, ".i + (" };
static const wstr S654 = { 4, 4, 1, 0, ") * " };
static const wstr S655 = { 3, 3, 1, 0, ".i)" };
static const wstr S656 = { 3, 3, 1, 0, " - " };
static const wstr S657 = { 6, 6, 1, 0, "rt_m(&" };
static const wstr S658 = { 2, 2, 1, 0, "])" };
static const wstr S659 = { 5, 5, 1, 0, "wfor " };
static const wstr S660 = { 14, 14, 1, 0, "rt_for_start(&" };
static const wstr S661 = { 14, 14, 1, 0, "rt_for_guard(&" };
static const wstr S662 = { 3, 3, 1, 0, "), " };
static const wstr S663 = { 13, 13, 1, 0, "rt_for_more(&" };
static const wstr S664 = { 3, 3, 1, 0, "); " };
static const wstr S665 = { 13, 13, 1, 0, "rt_for_item(&" };
static const wstr S666 = { 12, 12, 1, 0, "rt_for_key(&" };
static const wstr S667 = { 12, 12, 1, 0, "rt_for_val(&" };
static const wstr S668 = { 13, 13, 1, 0, "rt_fail_with(" };
static const wstr S669 = { 16, 16, 1, 0, "RT_RET(RT_NULL);" };
static const wstr S670 = { 9, 9, 1, 0, "struct fr" };
static const wstr S671 = { 4, 4, 1, 0, " *up" };
static const wstr S672 = { 2, 2, 1, 0, "p_" };
static const wstr S673 = { 10, 10, 1, 0, "static wv " };
static const wstr S674 = { 4, 4, 1, 0, "void" };
static const wstr S675 = { 9, 9, 1, 0, "rt_enter(" };
static const wstr S676 = { 8, 8, 1, 0, " fr = { " };
static const wstr S677 = { 8, 8, 1, 0, ".up = up" };
static const wstr S678 = { 8, 8, 1, 0, "rt_bind(" };
static const wstr S679 = { 13, 13, 1, 0, "(void)rt_def(" };
static const wstr S680 = { 3, 3, 1, 0, "rt_" };
static const wstr S681 = { 7, 7, 1, 0, "rt_set_" };
static const wstr S682 = { 7, 7, 1, 0, "rt_upd_" };
static const wstr S683 = { 5, 5, 1, 0, ", rt_" };
static const wstr S684 = { 7, 7, 1, 0, "rt_arr(" };
static const wstr S685 = { 7, 7, 1, 0, "rt_map(" };
static const wstr S686 = { 1, 1, 1, 0, "K" };
static const wstr S687 = { 7, 7, 1, 0, "rt_rec(" };
static const wstr S688 = { 18, 18, 1, 0, "if (rt_failcode) {" };
static const wstr S689 = { 17, 17, 1, 0, "rt_i(rt_failcode)" };
static const wstr S690 = { 16, 16, 1, 0, "rt_failcode = 0;" };
static const wstr S691 = { 3, 3, 1, 0, "raw" };
static const wstr S692 = { 7, 7, 1, 0, "} else " };
static const wstr S693 = { 4, 4, 1, 0, "if (" };
static const wstr S694 = { 3, 3, 1, 0, ") {" };
static const wstr S695 = { 8, 8, 1, 0, "} else {" };
static const wstr S696 = { 3, 3, 1, 0, " ? " };
static const wstr S697 = { 3, 3, 1, 0, " : " };
static const wstr S698 = { 3, 3, 1, 0, "({\n" };
static const wstr S699 = { 2, 2, 1, 0, "})" };
static const wstr S700 = { 3, 3, 1, 0, "eq(" };
static const wstr S701 = { 4, 4, 1, 0, " || " };
static const wstr S702 = { 12, 12, 1, 0, "(rt_no_case(" };
static const wstr S703 = { 11, 11, 1, 0, "), RT_NULL)" };
static const wstr S704 = { 6, 6, 1, 0, "({ wv " };
static const wstr S705 = { 4, 4, 1, 0, "std_" };
static const wstr S706 = { 3, 3, 1, 0, "&fr" };
static const wstr S707 = { 7, 7, 1, 0, "rt_chk(" };
static const wstr S708 = { 5, 5, 1, 0, "rt_f(" };
static const wstr S709 = { 5, 5, 1, 0, "rt_s(" };
static const wstr S710 = { 8, 8, 1, 0, "rt_text(" };
static const wstr S711 = { 7, 7, 1, 0, "rt_cat(" };
static const wstr S712 = { 5, 5, 1, 0, "rt_b(" };
static const wstr S713 = { 6, 6, 1, 0, "RT_NAN" };
static const wstr S714 = { 6, 6, 1, 0, "RT_INF" };
static const wstr S715 = { 2, 2, 1, 0, "()" };
static const wstr S716 = { 7, 7, 1, 0, "rt_idx(" };
static const wstr S717 = { 7, 7, 1, 0, "rt_get(" };
static const wstr S718 = { 13, 13, 1, 0, "rt_b(rt_bool(" };
static const wstr S719 = { 3, 3, 1, 0, ", \"" };
static const wstr S720 = { 3, 3, 1, 0, "\") " };
static const wstr S721 = { 9, 9, 1, 0, " rt_bool(" };
static const wstr S722 = { 3, 3, 1, 0, "\"))" };
static const wstr S723 = { 9, 9, 1, 0, "(rt_cond(" };
static const wstr S724 = { 4, 4, 1, 0, ") ? " };
static const wstr S725 = { 30, 30, 1, 0, "codegen: unhandled expression " };
static const wstr S726 = { 2, 2, 1, 0, "\r\n" };
static const wstr S727 = { 20, 20, 1, 0, "// ---- program ----" };
static const wstr S728 = { 19, 19, 1, 0, "static const wstr S" };
static const wstr S729 = { 14, 14, 1, 0, "static wstr F_" };
static const wstr S730 = { 26, 26, 1, 0, "static const wstr *const K" };
static const wstr S731 = { 5, 5, 1, 0, " *up;" };
static const wstr S732 = { 10, 10, 1, 0, "char none;" };
static const wstr S733 = { 3, 3, 1, 0, " { " };
static const wstr S734 = { 2, 2, 1, 0, " {" };
static const wstr S735 = { 39, 39, 1, 0, "static const char *const W_files[] = { " };
static const wstr S736 = { 28, 28, 1, 0, "static void wvt_main(void) {" };
static const wstr S737 = { 21, 21, 1, 0, "  rt_files = W_files;" };
static const wstr S738 = { 10, 10, 1, 0, "  rt_syms(" };
static const wstr S739 = { 19, 19, 1, 0, ", (wstr *const[]){ " };
static const wstr S740 = { 4, 4, 1, 0, " });" };
static const wstr S741 = { 11, 11, 1, 0, "  rt_roots(" };
static const wstr S742 = { 17, 17, 1, 0, ", (wv *const[]){ " };
static const wstr S743 = { 4, 4, 1, 0, "  | " };
static const wstr S744 = { 4, 4, 1, 0, "  { " };
static const wstr S745 = { 4, 4, 1, 0, "    " };
static const wstr S746 = { 3, 3, 1, 0, "  }" };
static const wstr S747 = { 2, 2, 1, 0, "[]" };
static const wstr S748 = { 52, 52, 1, 0, "cannot find the standard library: no lib/ folder in " };
static const wstr S749 = { 14, 14, 1, 0, "import cycle: " };
static const wstr S750 = { 4, 4, 1, 0, " -> " };
static const wstr S751 = { 29, 29, 1, 0, "the standard library has no '" };
static const wstr S752 = { 3, 3, 1, 0, "' (" };
static const wstr S753 = { 13, 13, 1, 0, "cannot read '" };
static const wstr S754 = { 3, 1, 0, 0, "﻿" };
static const wstr S755 = { 7, 7, 1, 0, "usage:\n" };
static const wstr S756 = { 64, 64, 1, 0, "  wvc <file.wvt> [-o <out.c>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S757 = { 90, 90, 1, 0, "  wvc build <file.wvt> [-o <program>] [--cc <compiler>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S758 = { 26, 26, 1, 0, "  wvc --tokens <file.wvt>\n" };
static const wstr S759 = { 23, 23, 1, 0, "  wvc --ast <file.wvt>\n" };
static const wstr S760 = { 54, 54, 1, 0, "  wvc --types <file.wvt> [--text <file>] [--lib <dir>]" };
static const wstr S761 = { 17, 17, 1, 0, "wvc: cannot read " };
static const wstr S762 = { 5, 5, 1, 0, "build" };
static const wstr S763 = { 8, 8, 1, 0, "--tokens" };
static const wstr S764 = { 5, 5, 1, 0, "--ast" };
static const wstr S765 = { 7, 7, 1, 0, "--types" };
static const wstr S766 = { 2, 2, 1, 0, "-o" };
static const wstr S767 = { 9, 9, 1, 0, "--runtime" };
static const wstr S768 = { 4, 4, 1, 0, "--cc" };
static const wstr S769 = { 5, 5, 1, 0, "--lib" };
static const wstr S770 = { 6, 6, 1, 0, "--text" };
static const wstr S771 = { 2, 2, 1, 0, "OS" };
static const wstr S772 = { 10, 10, 1, 0, "Windows_NT" };
static const wstr S773 = { 3, 3, 1, 0, "ZIG" };
static const wstr S774 = { 3, 3, 1, 0, "zig" };
static const wstr S775 = { 5, 5, 1, 0, "wvc: " };
static const wstr S776 = { 52, 52, 1, 0, " does not end with .wvt, so name the program with -o" };
static const wstr S777 = { 4, 4, 1, 0, ".exe" };
static const wstr S778 = { 13, 13, 1, 0, "runtime/wvt.c" };
static const wstr S779 = { 3, 3, 1, 0, "../" };
static const wstr S780 = { 3, 3, 1, 0, "lib" };
static const wstr S781 = { 6, 6, 1, 0, "../lib" };
static const wstr S782 = { 2, 2, 1, 0, ".c" };
static const wstr S783 = { 18, 18, 1, 0, "wvc: cannot write " };
static const wstr S784 = { 3, 3, 1, 0, "-O2" };
static const wstr S785 = { 7, 7, 1, 0, "zig.exe" };
static const wstr S786 = { 2, 2, 1, 0, "cc" };
static const wstr S787 = { 16, 16, 1, 0, "wvc: cannot run " };
static const wstr S788 = { 67, 67, 1, 0, ": install Zig (https://ziglang.org), or name a C compiler with --cc" };
static const wstr S789 = { 23, 23, 1, 0, " failed with exit code " };
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
struct fr176 { wv v837_modules; wv v838_runtime; wv v839_n; wv v840_tmps; wv v841_strings; wv v842_fields; wv v843_shapes; wv v844_enumDefs; wv v845_globals; wv v846_frames; wv v847_functions; wv v848_out; wv v849_indent; wv v850_loops; wv v851_iife; wv v852_decls; wv v853_fns; wv v854_tops; wv v855_slots; wv v856_curMod; wv v857_mod; wv v858_curSlot; wv v859_stolen; wv v860_ownEnums; wv v861_cur; wv v862_lineStarts; };
struct fr250 { wv v1279_libDir; wv v1280_searched; wv v1281_modules; wv v1282_libRoot; };
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
static wv v1318_a;
static wv v1333_tokens;
static wv v1334_program;
static wv v1335_p;
static wv v1336_p;
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
static wv s486_enter(struct fr119 *up, wv v568_m);
static wv s487_enumMembers(struct fr119 *up, wv v569_d);
static wv s488_firstUse(struct fr119 *up, wv v571_mod, wv v572_declPos);
static wv s489_requireValue(struct fr119 *up, wv v574_b);
static wv s490_newScope(struct fr119 *up, wv v577_parent);
static wv s491_decl(struct fr119 *up, wv v579_kind, wv v580_name, wv v581_mode, wv v582_namePos, wv v583_visibleAt, wv v584_fnId, wv v585_arity);
static wv s492_own(struct fr119 *up, wv v586_scope, wv v587_name);
static wv s493_declare(struct fr119 *up, wv v589_scope, wv v590_name, wv v591_d);
static wv s494_lookup(struct fr119 *up, wv v595_scope, wv v596_name);
static wv s495_resolve(struct fr119 *up, wv v599_name, wv v600_pos, wv v601_scope, wv v602_ctx);
static wv s496_block(struct fr119 *up, wv v604_b, wv v605_scope, wv v606_ctx);
static wv s497_stmt(struct fr119 *up, wv v611_s, wv v612_scope, wv v613_ctx);
static wv s498_forLoop(struct fr119 *up, wv v625_s, wv v626_scope, wv v627_ctx);
static wv s499_rangeBound(struct fr119 *up, wv v637_x, wv v638_ty);
static wv s500_collection(struct fr119 *up, wv v640_x, wv v641_ty);
static wv s501_pathOf(struct fr119 *up, wv v643_x);
static wv s502_guarded(struct fr119 *up, wv v646_t, wv v647_ctx);
static wv s503_onfailHandler(struct fr119 *up, wv v655_s, wv v656_scope, wv v657_ctx);
static wv s504_checkMatch(struct fr119 *up, wv v661_e, wv v662_scope, wv v663_ctx, wv v664_stmtLevel);
static wv s505_moduleNamed(struct fr119 *up, wv v685_x, wv v686_scope);
static wv s506_exported(struct fr119 *up, wv v688_lib, wv v689_name);
static wv s507_enumNamed(struct fr119 *up, wv v692_x, wv v693_scope);
static wv s508_exportOf(struct fr119 *up, wv v697_e, wv v698_m);
static wv s509_unknownModule(struct fr119 *up, wv v700_x, wv v701_scope);
static wv s510_qualified(struct fr119 *up, wv v703_e, wv v704_m);
static wv s511_caseEnum(struct fr119 *up, wv v707_v, wv v708_scope);
static wv s512_member(struct fr119 *up, wv v714_e, wv v715_d);
static wv s513_chain(struct fr119 *up, wv v720_e, wv v721_scope, wv v722_ctx, wv v723_stmtLevel);
static wv s514_lvalue(struct fr119 *up, wv v727_t, wv v728_scope, wv v729_ctx);
static wv s515_mustBeMutable(struct fr119 *up, wv v738_d, wv v739_name, wv v740_pos, wv v741_message, wv v742_rebind);
static wv s516_unknownStd(struct fr119 *up, wv v744_node);
static wv s517_expr(struct fr119 *up, wv v745_e, wv v746_scope, wv v747_ctx);
static wv s518_callee(struct fr119 *up, wv v766_e, wv v767_scope, wv v768_used, wv v769_types);
static wv s519_importNames(struct fr119 *up, wv v782_m, wv v783_scope);
static wv s520_libraryRules(struct fr119 *up, wv v792_m, wv v793_scope, wv v794_isEntry, wv v795_last);
static wv s521_checkModule(struct fr119 *up, wv v802_m, wv v803_isEntry);
static wv w6_binFn(wv v814_op);
static wv w6_modeOf(wv v816_x);
static wv w6_unFn(wv v817_op);
static wv w6_repeat(wv v818_s, wv v819_count);
static wv w6_utf8Length(wv v822_s);
static wv w6_cString(wv v827_s);
static wv w6_wstrInit(wv v833_s);
static wv w6_values(wv v836_cs);
static wv w6_generateC(wv p_v837_modules, wv p_v838_runtime);
static wv s863_atLine(struct fr176 *up, wv v928_pos);
static wv s864_atExpr(struct fr176 *up, wv v932_pos, wv v933_c);
static wv s865_modId(struct fr176 *up, wv v934_m);
static wv s866_hasFrame(struct fr176 *up, wv v935_f);
static wv s867_newScope(struct fr176 *up, wv v936_parent);
static wv s868_put(struct fr176 *up, wv v938_scope, wv v939_name, wv v940_kind, wv v941_d);
static wv s869_find(struct fr176 *up, wv v942_scope, wv v943_name);
static wv s870_kindOf(struct fr176 *up, wv v947_scope, wv v948_name);
static wv s871_newFn(struct fr176 *up, wv v950_name, wv v951_parent);
static wv s872_bind(struct fr176 *up, wv v953_kind, wv v954_name, wv v955_f, wv v956_atTop, wv v957_mode);
static wv s873_ablock(struct fr176 *up, wv v960_b, wv v961_scope, wv v962_f, wv v963_topScope);
static wv s874_astmt(struct fr176 *up, wv v971_s, wv v972_scope, wv v973_f, wv v974_topScope);
static wv s875_use(struct fr176 *up, wv v989_x, wv v990_scope, wv v991_f);
static wv s876_atarget(struct fr176 *up, wv v993_t, wv v994_scope, wv v995_f);
static wv s877_notValue(struct fr176 *up, wv v996_x, wv v997_scope);
static wv s878_aexpr(struct fr176 *up, wv v999_e, wv v1000_scope, wv v1001_f);
static wv s879_analyze(struct fr176 *up, wv v1009_m);
static wv s880_enter(struct fr176 *up, wv v1016_m);
static wv s881_emit(struct fr176 *up, wv v1023_line);
static wv s882_tmp(struct fr176 *up);
static wv s883_str(struct fr176 *up, wv v1025_s);
static wv s884_fieldRef(struct fr176 *up, wv v1027_name);
static wv s885_enumDef(struct fr176 *up, wv v1028_s, wv v1029_m);
static wv s886_stolenFrom(struct fr176 *up, wv v1033_x);
static wv s887_enumIn(struct fr176 *up, wv v1035_list, wv v1036_name);
static wv s888_enumOf(struct fr176 *up, wv v1038_x);
static wv s889_place(struct fr176 *up, wv v1043_d);
static wv s890_tdz(struct fr176 *up, wv v1045_d);
static wv s891_read(struct fr176 *up, wv v1046_d);
static wv s892_seq(struct fr176 *up, wv v1047_args);
static wv s893_wrap(struct fr176 *up, wv v1054_sq, wv v1055_inner);
static wv s894_stmts(struct fr176 *up, wv v1056_list);
static wv s895_resets(struct fr176 *up, wv v1058_b);
static wv s896_blockBody(struct fr176 *up, wv v1062_b);
static wv s897_valueOf(struct fr176 *up, wv v1063_b);
static wv s898_stmt(struct fr176 *up, wv v1064_s);
static wv s899_pathText(struct fr176 *up, wv v1066_x);
static wv s900_forStmt(struct fr176 *up, wv v1068_s);
static wv s901_signal(struct fr176 *up, wv v1083_code);
static wv s902_bindDecl(struct fr176 *up, wv v1084_d, wv v1085_value);
static wv s903_fnDef(struct fr176 *up, wv v1087_s);
static wv s904_bindRhs(struct fr176 *up, wv v1107_e, wv v1108_mode);
static wv s905_setStmt(struct fr176 *up, wv v1110_s);
static wv s906_exprs(struct fr176 *up, wv v1125_list);
static wv s907_array(struct fr176 *up, wv v1128_e, wv v1129_mutable);
static wv s908_mapLit(struct fr176 *up, wv v1131_e, wv v1132_mutable);
static wv s909_record(struct fr176 *up, wv v1134_e, wv v1135_mutable);
static wv s910_onfailStmt(struct fr176 *up, wv v1143_s);
static wv s911_chainStmt(struct fr176 *up, wv v1152_e);
static wv s912_test(struct fr176 *up, wv v1155_b);
static wv s913_branchCount(struct fr176 *up, wv v1156_e);
static wv s914_branchBody(struct fr176 *up, wv v1157_e, wv v1158_i);
static wv s915_branchTest(struct fr176 *up, wv v1159_e, wv v1160_t, wv v1161_i);
static wv s916_isPlain(struct fr176 *up, wv v1162_e);
static wv s917_ternaryChain(struct fr176 *up, wv v1166_e, wv v1167_t);
static wv s918_branchValue(struct fr176 *up, wv v1170_e, wv v1171_t, wv v1172_subject);
static wv s919_chainExpr(struct fr176 *up, wv v1181_e);
static wv s920_caseTest(struct fr176 *up, wv v1182_t, wv v1183_a);
static wv s921_matchElse(struct fr176 *up, wv v1186_e, wv v1187_t);
static wv s922_matchStmt(struct fr176 *up, wv v1188_e);
static wv s923_matchExpr(struct fr176 *up, wv v1192_e);
static wv s924_callC(struct fr176 *up, wv v1196_callee, wv v1197_args, wv v1198_checked);
static wv s925_expr(struct fr176 *up, wv v1207_e);
static wv s926_assemble(struct fr176 *up);
static wv w7_dumpTokens(wv v1239_toks, wv v1240_indent);
static wv w7_sorted(wv v1246_xs);
static wv w7_dumpValue(wv v1251_x, wv v1252_indent);
static wv w8_dirOf(wv v1261_path);
static wv w8_normalize(wv v1264_path);
static wv w8_with(wv v1269_xs, wv v1270_x);
static wv w8_failIn(wv v1273_file, wv v1274_chars, wv v1275_pos, wv v1276_message);
static wv w8_loadProgram(wv v1277_entryFile, wv v1278_entryChars, wv p_v1279_libDir, wv p_v1280_searched);
static wv s1283_traceName(struct fr250 *up, wv v1288_path);
static wv s1284_target(struct fr250 *up, wv v1289_s, wv v1290_file, wv v1291_chars, wv v1292_path);
static wv s1285_loaded(struct fr250 *up, wv v1293_path);
static wv s1286_visit(struct fr250 *up, wv v1295_file, wv v1296_chars, wv v1297_path, wv v1298_chain);
static wv w_die(void);
static wv w_load(wv v1313_file, wv v1314_from);
static wv w_onWindows(void);
static wv w_tryRead(wv v1320_path);
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
  RT_AT(4, 1410);
  fr.v522_topFn = rt_rec(0, 2, K50, (wv[]){ rt_i(0), rt_b(1) });
  RT_AT(4, 1411);
  wv v523_entry = rt_sub(std_length(fr.v462_modules), rt_i(1));
  RT_AT(4, 1412);
  wv v524_mi = rt_i(0);
  RT_AT(4, 1413);
  while (rt_cond((RT_AT(4, 1413), ({ wv t1571 = v524_mi; wv t1572 = std_length(fr.v462_modules); rt_lt(t1571, t1572); })))) {
    RT_AT(4, 1414);
    (void)({ wv t1573 = v524_mi; wv t1574 = rt_eq(v524_mi, v523_entry); rt_chk(s521_checkModule(&fr, t1573, t1574), "check"); });
    RT_AT(4, 1415);
    v524_mi = rt_add(v524_mi, rt_i(1));
  }
  RT_AT(4, 1418);
  wv v525_si = rt_i(0);
  RT_AT(4, 1419);
  while (rt_cond((RT_AT(4, 1419), ({ wv t1575 = v525_si; wv t1576 = std_length(fr.v467_functions); rt_lt(t1575, t1576); })))) {
    RT_AT(4, 1420);
    wv v810_sc = rt_bind(rt_idx(v525_si, fr.v467_functions), 0);
    RT_AT(4, 1421);
    wv v811_use = rt_bind(({ wv t1577 = rt_get(&F_mod, v810_sc); wv t1578 = rt_get(&F_namePos, v810_sc); rt_chk(s488_firstUse(&fr, t1577, t1578), "check"); }), 0);
    RT_AT(4, 1422);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v811_use, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v810_sc)), RT_NULL), "&&")), "&&") && rt_bool(rt_not(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v810_sc))), "check")), "&&")))) {
      RT_AT(4, 1423);
      (void)rt_chk(s486_enter(&fr, rt_get(&F_mod, v810_sc)), "check");
      RT_AT(4, 1424);
      wv v812_um = rt_bind(rt_get(&F_useMod, v811_use), 0);
      RT_AT(4, 1425);
      wv v813_where = rt_bind((rt_cond(({ wv t1579 = v812_um; wv t1580 = rt_get(&F_mod, v810_sc); rt_eq(t1579, t1580); })) ? rt_cat(2, (wv[]){ rt_s(&S576), rt_text(rt_chk(w5_lineOf(rt_get(&F_pos, v811_use)), "check")) }) : ({ wv t1583 = rt_text(rt_get(&F_file, rt_idx(v812_um, fr.v462_modules))); wv t1584 = rt_text(({ wv t1581 = rt_get(&F_chars, rt_idx(v812_um, fr.v462_modules)); wv t1582 = rt_get(&F_pos, v811_use); rt_chk(w5_lineIn(t1581, t1582), "check"); })); rt_cat(3, (wv[]){ t1583, rt_s(&S577), t1584 }); })), 0);
      RT_AT(4, 1426);
      (void)({ wv t1587 = rt_get(&F_namePos, v810_sc); wv t1588 = ({ wv t1585 = rt_text(rt_get(&F_name, v810_sc)); wv t1586 = rt_text(v813_where); rt_cat(5, (wv[]){ rt_s(&S114), t1585, rt_s(&S578), t1586, rt_s(&S16) }); }); rt_chk(s475_error(&fr, t1587, t1588, rt_s(&S579)), "check"); });
    }
    RT_AT(4, 1429);
    v525_si = rt_add(v525_si, rt_i(1));
  }
  RT_AT(4, 1431);
  (void)rt_chk(s486_enter(&fr, v523_entry), "check");
  RT_AT(4, 1432);
  wv v526_program = rt_bind(rt_get(&F_program, rt_idx(v523_entry, fr.v462_modules)), 0);
  RT_AT(4, 1433);
  wv v527_last = RT_NULL;
  RT_AT(4, 1434);
  wv v528_k = rt_i(0);
  RT_AT(4, 1435);
  while (rt_cond((RT_AT(4, 1435), ({ wv t1589 = v528_k; wv t1590 = std_length(rt_get(&F_stmts, v526_program)); rt_lt(t1589, t1590); })))) {
    RT_AT(4, 1436);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1591 = v528_k; wv t1592 = rt_get(&F_stmts, v526_program); rt_idx(t1591, t1592); })), rt_s(&S203)))) {
      RT_AT(4, 1436);
      v527_last = rt_bind(({ wv t1593 = v528_k; wv t1594 = rt_get(&F_stmts, v526_program); rt_idx(t1593, t1594); }), 1);
    }
    RT_AT(4, 1437);
    v528_k = rt_add(v528_k, rt_i(1));
  }
  RT_AT(4, 1440);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v527_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v527_last), rt_s(&S55)), "||")), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(fr.v463_requireExit, "||") || rt_bool(rt_eq(v527_last, RT_NULL), "||")), "||") || rt_bool(rt_ne(rt_get(&F_k, v527_last), rt_s(&S206)), "||")), "&&")))) {
    RT_AT(4, 1441);
    (void)rt_chk(s475_error(&fr, rt_get(&F_end, v526_program), rt_s(&S580), rt_s(&S581)), "check");
  }
  RT_AT(4, 1443);
  wv t1595 = rt_bind(fr.v474_allDecls, 1);
  (void)rt_def(w5_lastDecls, "lastDecls");
  w5_lastDecls = t1595;
  RT_AT(4, 1444);
  wv t1596 = rt_bind(fr.v470_bindingTypes, 1);
  (void)rt_def(w5_lastBindingTypes, "lastBindingTypes");
  w5_lastBindingTypes = t1596;
  RT_AT(4, 1445);
  wv t1597 = rt_bind(fr.v471_fnTypes, 1);
  (void)rt_def(w5_lastFnTypes, "lastFnTypes");
  w5_lastFnTypes = t1597;
  RT_RET((RT_AT(4, 1446), fr.v464_errors));
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
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&")), "&&") && rt_bool(({ wv t844 = rt_get(&F_t, v561_lt); wv t845 = rt_get(&F_t, v562_rt); rt_eq(t844, t845); }), "&&")), "&&") && rt_bool(rt_not(({ wv t846 = rt_arr(3, (wv[]){ rt_s(&S139), rt_s(&S149), rt_s(&S187) }, 0); wv t847 = rt_get(&F_t, v561_lt); rt_chk(w1_contains(t846, t847), "binary"); })), "&&")))) {
      RT_AT(4, 507);
      wv v567_what = rt_bind((rt_cond(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S47))) ? rt_s(&S464) : rt_cat(2, (wv[]){ rt_text(rt_get(&F_t, v561_lt)), rt_s(&S465) })), 0);
      RT_AT(4, 508);
      (void)({ wv t850 = rt_get(&F_pos, v560_e); wv t851 = ({ wv t848 = rt_text(v563_op); wv t849 = rt_text(v567_what); rt_cat(4, (wv[]){ rt_s(&S114), t848, rt_s(&S466), t849 }); }); rt_chk(s475_error(up, t850, t851, RT_NULL), "binary"); });
    }
    RT_AT(4, 510);
    RT_RET(rt_def(w5_tBool, "tBool"));
  }
  RT_AT(4, 512);
  if (rt_cond(({ wv t852 = rt_arr(5, (wv[]){ rt_s(&S92), rt_s(&S93), rt_s(&S32), rt_s(&S83), rt_s(&S84) }, 0); wv t853 = v563_op; rt_chk(w1_contains(t852, t853), "binary"); }))) {
    RT_AT(4, 513);
    (void)({ wv t854 = v563_op; wv t855 = rt_get(&F_l, v560_e); wv t856 = v561_lt; rt_chk(s482_needInt(up, t854, t855, t856), "binary"); });
    RT_AT(4, 514);
    (void)({ wv t857 = v563_op; wv t858 = rt_get(&F_r, v560_e); wv t859 = v562_rt; rt_chk(s482_needInt(up, t857, t858, t859), "binary"); });
    RT_AT(4, 515);
    RT_RET(rt_def(w5_tInt, "tInt"));
  }
  RT_AT(4, 518);
  wv v564_both = rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&"));
  RT_AT(4, 519);
  wv v565_shown = rt_bind((rt_cond(v564_both) ? ({ wv t860 = rt_text(rt_chk(w5_typeText(v561_lt), "binary")); wv t861 = rt_text(rt_chk(w5_typeText(v562_rt), "binary")); rt_cat(3, (wv[]){ t860, rt_s(&S467), t861 }); }) : (rt_cond(rt_chk(w5_known(v561_lt), "binary")) ? rt_chk(w5_typeText(v561_lt), "binary") : (rt_cond(rt_chk(w5_known(v562_rt), "binary")) ? rt_chk(w5_typeText(v562_rt), "binary") : rt_s(&S13)))), 0);
  RT_AT(4, 520);
  if (rt_cond(rt_b(rt_bool(rt_eq(v563_op, rt_s(&S85)), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S187)), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v562_rt), rt_s(&S187)), "&&")), "||")), "&&")))) {
    RT_AT(4, 521);
    if (rt_cond(rt_b(rt_bool(v564_both, "&&") && rt_bool(({ wv t862 = rt_get(&F_t, v561_lt); wv t863 = rt_get(&F_t, v562_rt); rt_eq(t862, t863); }), "&&")))) {
      RT_AT(4, 521);
      RT_RET(rt_def(w5_tString, "tString"));
    }
    RT_AT(4, 522);
    if (rt_cond(v564_both)) {
      RT_AT(4, 522);
      (void)({ wv t864 = rt_get(&F_pos, v560_e); wv t865 = rt_cat(2, (wv[]){ rt_s(&S468), rt_text(v565_shown) }); rt_chk(s475_error(up, t864, t865, rt_s(&S469)), "binary"); });
    }
    RT_AT(4, 523);
    RT_RET((rt_cond(v564_both) ? RT_NULL : rt_def(w5_tString, "tString")));
  }
  RT_AT(4, 525);
  wv v566_nums = rt_arr(2, (wv[]){ rt_s(&S139), rt_s(&S149) }, 0);
  RT_AT(4, 526);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_not(({ wv t866 = v566_nums; wv t867 = rt_get(&F_t, v561_lt); rt_chk(w1_contains(t866, t867), "binary"); })), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&") && rt_bool(rt_not(({ wv t868 = v566_nums; wv t869 = rt_get(&F_t, v562_rt); rt_chk(w1_contains(t868, t869), "binary"); })), "&&")), "||")))) {
    RT_AT(4, 527);
    (void)({ wv t872 = rt_get(&F_pos, v560_e); wv t873 = ({ wv t870 = rt_text(v563_op); wv t871 = rt_text(v565_shown); rt_cat(4, (wv[]){ rt_s(&S114), t870, rt_s(&S470), t871 }); }); rt_chk(s475_error(up, t872, t873, RT_NULL), "binary"); });
    RT_AT(4, 528);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 530);
  if (rt_cond(rt_eq(v563_op, rt_s(&S88)))) {
    RT_AT(4, 530);
    RT_RET(rt_def(w5_tFloat, "tFloat"));
  }
  RT_AT(4, 531);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(v564_both, "&&") && rt_bool(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S139)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, v562_rt), rt_s(&S139)), "&&")))) {
    RT_AT(4, 531);
    RT_RET((rt_cond(rt_eq(v563_op, rt_s(&S76))) ? RT_NULL : rt_def(w5_tInt, "tInt")));
  }
  RT_AT(4, 532);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w5_known(v561_lt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v561_lt), rt_s(&S149)), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_chk(w5_known(v562_rt), "binary"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v562_rt), rt_s(&S149)), "&&")), "||")))) {
    RT_AT(4, 532);
    RT_RET(rt_def(w5_tFloat, "tFloat"));
  }
  RT_RET((RT_AT(4, 533), RT_NULL));
}

static wv s486_enter(struct fr119 *up, wv v568_m) {
  rt_enter("enter");
  v568_m = rt_bind(v568_m, 0);
  RT_AT(4, 538);
  wv t874 = rt_bind(v568_m, 1);
  (void)rt_def(up->v466_curMod, "curMod");
  up->v466_curMod = t874;
  RT_AT(4, 539);
  (void)({ wv t875 = rt_get(&F_file, rt_idx(v568_m, up->v462_modules)); wv t876 = rt_get(&F_chars, rt_idx(v568_m, up->v462_modules)); rt_chk(w2_setSource(t875, t876), "enter"); });
  RT_RET(RT_NULL);
}

static wv s487_enumMembers(struct fr119 *up, wv v569_d) {
  rt_enter("enumMembers");
  v569_d = rt_bind(v569_d, 0);
  RT_AT(4, 544);
  wv v570_k = rt_i(0);
  RT_AT(4, 545);
  while (rt_cond((RT_AT(4, 545), ({ wv t877 = v570_k; wv t878 = std_length(rt_def(up->v469_enums, "enums")); rt_lt(t877, t878); })))) {
    RT_AT(4, 546);
    if (rt_cond(rt_b(rt_bool(({ wv t881 = rt_get(&F_mod, ({ wv t879 = v570_k; wv t880 = rt_def(up->v469_enums, "enums"); rt_idx(t879, t880); })); wv t882 = rt_get(&F_mod, v569_d); rt_eq(t881, t882); }), "&&") && rt_bool(({ wv t885 = rt_get(&F_name, ({ wv t883 = v570_k; wv t884 = rt_def(up->v469_enums, "enums"); rt_idx(t883, t884); })); wv t886 = rt_get(&F_name, v569_d); rt_eq(t885, t886); }), "&&")))) {
      RT_AT(4, 546);
      RT_RET(rt_get(&F_members, ({ wv t887 = v570_k; wv t888 = rt_def(up->v469_enums, "enums"); rt_idx(t887, t888); })));
    }
    RT_AT(4, 547);
    v570_k = rt_add(v570_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 549), RT_NULL));
}

static wv s488_firstUse(struct fr119 *up, wv v571_mod, wv v572_declPos) {
  rt_enter("firstUse");
  v571_mod = rt_bind(v571_mod, 0);
  v572_declPos = rt_bind(v572_declPos, 0);
  RT_AT(4, 554);
  wv v573_k = rt_i(0);
  RT_AT(4, 555);
  while (rt_cond((RT_AT(4, 555), ({ wv t889 = v573_k; wv t890 = std_length(rt_def(up->v468_uses, "uses")); rt_lt(t889, t890); })))) {
    RT_AT(4, 556);
    if (rt_cond(rt_b(rt_bool(({ wv t893 = rt_get(&F_mod, ({ wv t891 = v573_k; wv t892 = rt_def(up->v468_uses, "uses"); rt_idx(t891, t892); })); wv t894 = v571_mod; rt_eq(t893, t894); }), "&&") && rt_bool(({ wv t897 = rt_get(&F_namePos, ({ wv t895 = v573_k; wv t896 = rt_def(up->v468_uses, "uses"); rt_idx(t895, t896); })); wv t898 = v572_declPos; rt_eq(t897, t898); }), "&&")))) {
      RT_AT(4, 556);
      RT_RET(({ wv t899 = v573_k; wv t900 = rt_def(up->v468_uses, "uses"); rt_idx(t899, t900); }));
    }
    RT_AT(4, 557);
    v573_k = rt_add(v573_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 559), RT_NULL));
}

static wv s489_requireValue(struct fr119 *up, wv v574_b) {
  rt_enter("requireValue");
  v574_b = rt_bind(v574_b, 0);
  RT_AT(4, 564);
  if (rt_cond(rt_ne(rt_get(&F_value, v574_b), RT_NULL))) {
    RT_AT(4, 564);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 565);
  wv v575_last = RT_NULL;
  RT_AT(4, 566);
  wv v576_k = rt_i(0);
  RT_AT(4, 567);
  while (rt_cond((RT_AT(4, 567), ({ wv t901 = v576_k; wv t902 = std_length(rt_get(&F_stmts, v574_b)); rt_lt(t901, t902); })))) {
    RT_AT(4, 568);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t903 = v576_k; wv t904 = rt_get(&F_stmts, v574_b); rt_idx(t903, t904); })), rt_s(&S203)))) {
      RT_AT(4, 568);
      v575_last = rt_bind(({ wv t905 = v576_k; wv t906 = rt_get(&F_stmts, v574_b); rt_idx(t905, t906); }), 1);
    }
    RT_AT(4, 569);
    v576_k = rt_add(v576_k, rt_i(1));
  }
  RT_AT(4, 571);
  if (rt_cond(rt_b(rt_bool(rt_ne(v575_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t907 = rt_def(w5_neverCompletes, "neverCompletes"); wv t908 = rt_get(&F_k, v575_last); rt_chk(w1_contains(t907, t908), "requireValue"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v575_last), rt_s(&S178)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v575_last)), rt_s(&S185)), "&&")), "||")), "&&")))) {
    RT_AT(4, 572);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 574);
  (void)rt_chk(s475_error(up, rt_get(&F_close, v574_b), rt_s(&S471), rt_s(&S472)), "requireValue");
  RT_RET(RT_NULL);
}

static wv s490_newScope(struct fr119 *up, wv v577_parent) {
  rt_enter("newScope");
  v577_parent = rt_bind(v577_parent, 0);
  RT_AT(4, 579);
  wv v578_names = rt_arr(0, NULL, 1);
  RT_RET((RT_AT(4, 580), rt_rec(0, 2, K46, (wv[]){ v577_parent, v578_names })));
}

static wv s491_decl(struct fr119 *up, wv v579_kind, wv v580_name, wv v581_mode, wv v582_namePos, wv v583_visibleAt, wv v584_fnId, wv v585_arity) {
  rt_enter("decl");
  v579_kind = rt_bind(v579_kind, 0);
  v580_name = rt_bind(v580_name, 0);
  v581_mode = rt_bind(v581_mode, 0);
  v582_namePos = rt_bind(v582_namePos, 0);
  v583_visibleAt = rt_bind(v583_visibleAt, 0);
  v584_fnId = rt_bind(v584_fnId, 0);
  v585_arity = rt_bind(v585_arity, 0);
  RT_RET((RT_AT(4, 589), ({ wv t909 = v579_kind; wv t910 = v580_name; wv t911 = rt_eq(v581_mode, rt_s(&S36)); wv t912 = rt_eq(v581_mode, rt_s(&S37)); wv t913 = v582_namePos; wv t914 = v583_visibleAt; wv t915 = v584_fnId; wv t916 = v585_arity; wv t917 = rt_def(up->v466_curMod, "curMod"); rt_rec(0, 10, K47, (wv[]){ t909, t910, t911, t912, t913, t914, t915, t916, t917, RT_NULL }); })));
}

static wv s492_own(struct fr119 *up, wv v586_scope, wv v587_name) {
  rt_enter("own");
  v587_name = rt_bind(v587_name, 0);
  RT_AT(4, 597);
  wv v588_k = rt_i(0);
  RT_AT(4, 598);
  while (rt_cond((RT_AT(4, 598), ({ wv t918 = v588_k; wv t919 = std_length(rt_get(&F_names, v586_scope)); rt_lt(t918, t919); })))) {
    RT_AT(4, 599);
    if (rt_cond(({ wv t922 = rt_get(&F_name, ({ wv t920 = v588_k; wv t921 = rt_get(&F_names, v586_scope); rt_idx(t920, t921); })); wv t923 = v587_name; rt_eq(t922, t923); }))) {
      RT_AT(4, 599);
      RT_RET(({ wv t924 = v588_k; wv t925 = rt_get(&F_names, v586_scope); rt_idx(t924, t925); }));
    }
    RT_AT(4, 600);
    v588_k = rt_add(v588_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 602), RT_NULL));
}

static wv s493_declare(struct fr119 *up, wv v589_scope, wv v590_name, wv v591_d) {
  rt_enter("declare");
  v590_name = rt_bind(v590_name, 0);
  v591_d = rt_bind(v591_d, 0);
  RT_AT(4, 609);
  wv v592_dup = rt_bind(rt_chk(s492_own(up, v589_scope, v590_name), "declare"), 0);
  RT_AT(4, 610);
  if (rt_cond(rt_ne(v592_dup, RT_NULL))) {
    RT_AT(4, 611);
    (void)({ wv t928 = rt_get(&F_namePos, v591_d); wv t929 = ({ wv t926 = rt_text(v590_name); wv t927 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v592_dup)), "declare")); rt_cat(5, (wv[]){ rt_s(&S114), t926, rt_s(&S473), t927, rt_s(&S16) }); }); rt_chk(s475_error(up, t928, t929, RT_NULL), "declare"); });
    RT_AT(4, 612);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 614);
  wv v593_s = rt_get(&F_parent, v589_scope);
  RT_AT(4, 615);
  while (rt_cond((RT_AT(4, 615), rt_ne(v593_s, RT_NULL)))) {
    RT_AT(4, 616);
    wv v594_outer = rt_bind(rt_chk(s492_own(up, v593_s, v590_name), "declare"), 0);
    RT_AT(4, 617);
    if (rt_cond(rt_b(rt_bool(rt_ne(v594_outer, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_kind, v594_outer), rt_s(&S179)), "||") || rt_bool(({ wv t930 = rt_get(&F_namePos, v594_outer); wv t931 = rt_get(&F_namePos, v591_d); rt_lt(t930, t931); }), "||")), "&&")))) {
      RT_AT(4, 618);
      (void)({ wv t935 = rt_get(&F_namePos, v591_d); wv t936 = ({ wv t932 = rt_text(v590_name); wv t933 = rt_text(v590_name); wv t934 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v594_outer)), "declare")); rt_cat(7, (wv[]){ rt_s(&S114), t932, rt_s(&S474), t933, rt_s(&S475), t934, rt_s(&S16) }); }); rt_chk(s475_error(up, t935, t936, rt_s(&S476)), "declare"); });
      break;
    }
    RT_AT(4, 622);
    v593_s = rt_get(&F_parent, v593_s);
  }
  RT_AT(4, 624);
  (void)({ wv t937 = rt_get(&F_names, v589_scope); wv t938 = v591_d; std_push(t937, t938); });
  RT_AT(4, 625);
  (void)({ wv t939 = rt_def(up->v474_allDecls, "allDecls"); wv t940 = v591_d; std_push(t939, t940); });
  RT_RET(RT_NULL);
}

static wv s494_lookup(struct fr119 *up, wv v595_scope, wv v596_name) {
  rt_enter("lookup");
  v596_name = rt_bind(v596_name, 0);
  RT_AT(4, 629);
  wv v597_s = v595_scope;
  RT_AT(4, 630);
  while (rt_cond((RT_AT(4, 630), rt_ne(v597_s, RT_NULL)))) {
    RT_AT(4, 631);
    wv v598_d = rt_bind(rt_chk(s492_own(up, v597_s, v596_name), "lookup"), 0);
    RT_AT(4, 632);
    if (rt_cond(rt_ne(v598_d, RT_NULL))) {
      RT_AT(4, 632);
      RT_RET(v598_d);
    }
    RT_AT(4, 633);
    v597_s = rt_get(&F_parent, v597_s);
  }
  RT_RET((RT_AT(4, 635), RT_NULL));
}

static wv s495_resolve(struct fr119 *up, wv v599_name, wv v600_pos, wv v601_scope, wv v602_ctx) {
  rt_enter("resolve");
  v599_name = rt_bind(v599_name, 0);
  v600_pos = rt_bind(v600_pos, 0);
  v601_scope = rt_bind(v601_scope, 0);
  v602_ctx = rt_bind(v602_ctx, 0);
  RT_AT(4, 639);
  wv v603_d = rt_bind(rt_chk(s494_lookup(up, v601_scope, v599_name), "resolve"), 0);
  RT_AT(4, 640);
  if (rt_cond(rt_eq(v603_d, RT_NULL))) {
    RT_AT(4, 641);
    (void)({ wv t941 = v600_pos; wv t942 = rt_cat(3, (wv[]){ rt_s(&S477), rt_text(v599_name), rt_s(&S114) }); rt_chk(s475_error(up, t941, t942, RT_NULL), "resolve"); });
    RT_AT(4, 642);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 646);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v603_d), rt_s(&S179)), "&&") && rt_bool(({ wv t943 = rt_get(&F_fnId, v603_d); wv t944 = rt_get(&F_id, rt_get(&F_func, v602_ctx)); rt_eq(t943, t944); }), "&&")), "&&") && rt_bool(({ wv t945 = v600_pos; wv t946 = rt_get(&F_visibleAt, v603_d); rt_lt(t945, t946); }), "&&")))) {
    RT_AT(4, 647);
    (void)({ wv t949 = v600_pos; wv t950 = ({ wv t947 = rt_text(v599_name); wv t948 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v603_d)), "resolve")); rt_cat(5, (wv[]){ rt_s(&S114), t947, rt_s(&S478), t948, rt_s(&S16) }); }); rt_chk(s475_error(up, t949, t950, RT_NULL), "resolve"); });
  }
  RT_RET((RT_AT(4, 649), v603_d));
}

static wv s496_block(struct fr119 *up, wv v604_b, wv v605_scope, wv v606_ctx) {
  rt_enter("block");
  v605_scope = rt_bind(v605_scope, 0);
  v606_ctx = rt_bind(v606_ctx, 0);
  RT_AT(4, 653);
  wv v607_k = rt_i(0);
  RT_AT(4, 654);
  while (rt_cond((RT_AT(4, 654), ({ wv t951 = v607_k; wv t952 = std_length(rt_get(&F_stmts, v604_b)); rt_lt(t951, t952); })))) {
    RT_AT(4, 655);
    wv v608_s = rt_bind(({ wv t953 = v607_k; wv t954 = rt_get(&F_stmts, v604_b); rt_idx(t953, t954); }), 0);
    RT_AT(4, 656);
    if (rt_cond(rt_eq(rt_get(&F_k, v608_s), rt_s(&S33)))) {
      RT_AT(4, 657);
      wv v609_d = rt_bind(({ wv t955 = rt_get(&F_name, v608_s); wv t956 = rt_get(&F_namePos, v608_s); wv t957 = rt_get(&F_id, rt_get(&F_func, v606_ctx)); wv t958 = std_length(rt_get(&F_params, v608_s)); rt_chk(s491_decl(up, rt_s(&S33), t955, rt_s(&S13), t956, RT_NULL, t957, t958), "block"); }), 0);
      RT_AT(4, 658);
      (void)({ wv t959 = v605_scope; wv t960 = rt_get(&F_name, v608_s); wv t961 = v609_d; rt_chk(s493_declare(up, t959, t960, t961), "block"); });
      RT_AT(4, 659);
      if (rt_cond(({ wv t964 = ({ wv t962 = v605_scope; wv t963 = rt_get(&F_name, v608_s); rt_chk(s492_own(up, t962, t963), "block"); }); wv t965 = v609_d; rt_eq(t964, t965); }))) {
        RT_AT(4, 660);
        (void)({ wv t970 = rt_def(up->v467_functions, "functions"); wv t971 = ({ wv t966 = rt_def(up->v466_curMod, "curMod"); wv t967 = rt_get(&F_name, v608_s); wv t968 = rt_get(&F_namePos, v608_s); wv t969 = rt_get(&F_body, v608_s); rt_rec(0, 4, K48, (wv[]){ t966, t967, t968, t969 }); }); std_push(t970, t971); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v608_s), rt_s(&S47)))) {
      RT_AT(4, 663);
      wv v610_d = rt_bind(({ wv t972 = rt_get(&F_name, v608_s); wv t973 = rt_get(&F_namePos, v608_s); wv t974 = rt_get(&F_id, rt_get(&F_func, v606_ctx)); rt_chk(s491_decl(up, rt_s(&S47), t972, rt_s(&S13), t973, RT_NULL, t974, RT_NULL), "block"); }), 0);
      RT_AT(4, 664);
      (void)({ wv t975 = v605_scope; wv t976 = rt_get(&F_name, v608_s); wv t977 = v610_d; rt_chk(s493_declare(up, t975, t976, t977), "block"); });
      RT_AT(4, 665);
      if (rt_cond(({ wv t980 = ({ wv t978 = v605_scope; wv t979 = rt_get(&F_name, v608_s); rt_chk(s492_own(up, t978, t979), "block"); }); wv t981 = v610_d; rt_eq(t980, t981); }))) {
        RT_AT(4, 665);
        (void)({ wv t985 = rt_def(up->v469_enums, "enums"); wv t986 = ({ wv t982 = rt_def(up->v466_curMod, "curMod"); wv t983 = rt_get(&F_name, v608_s); wv t984 = rt_get(&F_members, v608_s); rt_rec(0, 3, K49, (wv[]){ t982, t983, t984 }); }); std_push(t985, t986); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v608_s), rt_s(&S179)))) {
      RT_AT(4, 667);
      (void)({ wv t992 = v605_scope; wv t993 = rt_get(&F_name, v608_s); wv t994 = ({ wv t987 = rt_get(&F_name, v608_s); wv t988 = rt_chk(w5_modeOf(v608_s), "block"); wv t989 = rt_get(&F_namePos, v608_s); wv t990 = rt_get(&F_end, v608_s); wv t991 = rt_get(&F_id, rt_get(&F_func, v606_ctx)); rt_chk(s491_decl(up, rt_s(&S179), t987, t988, t989, t990, t991, RT_NULL), "block"); }); rt_chk(s493_declare(up, t992, t993, t994), "block"); });
    }
    RT_AT(4, 669);
    v607_k = rt_add(v607_k, rt_i(1));
  }
  RT_AT(4, 671);
  v607_k = rt_i(0);
  RT_AT(4, 672);
  while (rt_cond((RT_AT(4, 672), ({ wv t995 = v607_k; wv t996 = std_length(rt_get(&F_stmts, v604_b)); rt_lt(t995, t996); })))) {
    RT_AT(4, 673);
    (void)({ wv t999 = ({ wv t997 = v607_k; wv t998 = rt_get(&F_stmts, v604_b); rt_idx(t997, t998); }); wv t1000 = v605_scope; wv t1001 = v606_ctx; rt_chk(s497_stmt(up, t999, t1000, t1001), "block"); });
    RT_AT(4, 674);
    v607_k = rt_add(v607_k, rt_i(1));
  }
  RT_AT(4, 676);
  if (rt_cond(rt_ne(rt_get(&F_value, v604_b), RT_NULL))) {
    RT_AT(4, 676);
    RT_RET(({ wv t1002 = rt_get(&F_value, v604_b); wv t1003 = v605_scope; wv t1004 = v606_ctx; rt_chk(s517_expr(up, t1002, t1003, t1004), "block"); }));
  }
  RT_RET((RT_AT(4, 677), rt_chk(w5_endType(v604_b), "block")));
}

static wv s497_stmt(struct fr119 *up, wv v611_s, wv v612_scope, wv v613_ctx) {
  rt_enter("stmt");
  v612_scope = rt_bind(v612_scope, 0);
  v613_ctx = rt_bind(v613_ctx, 0);
  RT_AT(4, 681);
  wv v614_kind = rt_bind(rt_get(&F_k, v611_s), 0);
  RT_AT(4, 682);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v614_kind, rt_s(&S203)), "||") || rt_bool(rt_eq(v614_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v614_kind, rt_s(&S48)), "||")), "||") || rt_bool(rt_eq(v614_kind, rt_s(&S206)), "||")))) {
    RT_AT(4, 682);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 683);
  if (rt_cond(rt_eq(v614_kind, rt_s(&S33)))) {
    RT_AT(4, 684);
    wv v616_func = rt_rec(0, 2, K50, (wv[]){ rt_def(up->v465_nextFn, "nextFn"), rt_b(0) });
    RT_AT(4, 685);
    up->v465_nextFn = rt_add(rt_def(up->v465_nextFn, "nextFn"), rt_i(1));
    RT_AT(4, 686);
    wv v617_ps = rt_bind(rt_chk(s490_newScope(up, v612_scope), "stmt"), 0);
    RT_AT(4, 687);
    wv v618_k = rt_i(0);
    RT_AT(4, 688);
    while (rt_cond((RT_AT(4, 688), ({ wv t1005 = v618_k; wv t1006 = std_length(rt_get(&F_params, v611_s)); rt_lt(t1005, t1006); })))) {
      RT_AT(4, 689);
      wv v621_p = rt_bind(({ wv t1007 = v618_k; wv t1008 = rt_get(&F_params, v611_s); rt_idx(t1007, t1008); }), 0);
      RT_AT(4, 690);
      (void)({ wv t1013 = v617_ps; wv t1014 = rt_get(&F_name, v621_p); wv t1015 = ({ wv t1009 = rt_get(&F_name, v621_p); wv t1010 = rt_chk(w5_modeOf(v621_p), "stmt"); wv t1011 = rt_get(&F_pos, v621_p); wv t1012 = rt_get(&F_id, v616_func); rt_chk(s491_decl(up, rt_s(&S479), t1009, t1010, t1011, RT_NULL, t1012, RT_NULL), "stmt"); }); rt_chk(s493_declare(up, t1013, t1014, t1015), "stmt"); });
      RT_AT(4, 691);
      v618_k = rt_add(v618_k, rt_i(1));
    }
    RT_AT(4, 693);
    wv v619_bodyType = rt_bind(({ wv t1018 = rt_get(&F_body, v611_s); wv t1019 = rt_chk(s490_newScope(up, v617_ps), "stmt"); wv t1020 = ({ wv t1016 = v616_func; wv t1017 = rt_get(&F_guards, v613_ctx); rt_rec(0, 3, K51, (wv[]){ t1016, rt_i(0), t1017 }); }); rt_chk(s496_block(up, t1018, t1019, t1020), "stmt"); }), 0);
    RT_AT(4, 696);
    wv v620_result = rt_bind(v619_bodyType, 1);
    RT_AT(4, 697);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v611_s)), RT_NULL), "&&") && rt_bool(rt_eq(v619_bodyType, RT_NULL), "&&")))) {
      RT_AT(4, 697);
      v620_result = rt_bind(rt_def(w5_tNull, "tNull"), 1);
    }
    RT_AT(4, 698);
    if (rt_cond(({ wv t1021 = rt_def(up->v472_returnTypes, "returnTypes"); wv t1022 = rt_get(&F_id, v616_func); std_has(t1021, t1022); }))) {
      RT_AT(4, 698);
      v620_result = rt_bind(({ wv t1025 = v620_result; wv t1026 = ({ wv t1023 = rt_get(&F_id, v616_func); wv t1024 = rt_def(up->v472_returnTypes, "returnTypes"); rt_idx(t1023, t1024); }); rt_chk(w5_join(t1025, t1026), "stmt"); }), 1);
    }
    RT_AT(4, 699);
    if (rt_cond(rt_ne(v620_result, RT_NULL))) {
      RT_AT(4, 699);
      ({ wv t1029 = v620_result; wv t1030 = ({ wv t1027 = rt_text(rt_def(up->v466_curMod, "curMod")); wv t1028 = rt_text(rt_get(&F_namePos, v611_s)); rt_cat(3, (wv[]){ t1027, rt_s(&S28), t1028 }); }); wv t1031 = rt_def(up->v471_fnTypes, "fnTypes"); rt_set_idx(t1029, t1030, t1031); });
    }
    RT_AT(4, 700);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 703);
  wv v615_ty = RT_NULL;
  RT_AT(4, 704);
  if (rt_cond(rt_eq(v614_kind, rt_s(&S34)))) {
    RT_AT(4, 705);
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v613_ctx)))) {
      RT_AT(4, 705);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v611_s), rt_s(&S480), RT_NULL), "stmt");
    }
    RT_AT(4, 706);
    v615_ty = rt_bind((rt_cond(rt_ne(rt_get(&F_e, v611_s), RT_NULL)) ? ({ wv t1032 = rt_get(&F_e, v611_s); wv t1033 = v612_scope; wv t1034 = v613_ctx; rt_chk(s517_expr(up, t1032, t1033, t1034), "stmt"); }) : rt_def(w5_tNull, "tNull")), 1);
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S58)))) {
    RT_AT(4, 708);
    (void)({ wv t1035 = rt_get(&F_e, v611_s); wv t1036 = v612_scope; wv t1037 = v613_ctx; rt_chk(s517_expr(up, t1035, t1036, t1037), "stmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v614_kind, rt_s(&S43)), "||") || rt_bool(rt_eq(v614_kind, rt_s(&S44)), "||")))) {
    RT_AT(4, 710);
    if (rt_cond(rt_eq(rt_get(&F_loops, v613_ctx), rt_i(0)))) {
      RT_AT(4, 710);
      (void)({ wv t1038 = rt_get(&F_pos, v611_s); wv t1039 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text((rt_cond(rt_eq(v614_kind, rt_s(&S43))) ? rt_s(&S43) : rt_s(&S44))), rt_s(&S481) }); rt_chk(s475_error(up, t1038, t1039, RT_NULL), "stmt"); });
    }
    RT_AT(4, 711);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S41)))) {
    RT_AT(4, 713);
    (void)({ wv t1043 = rt_get(&F_cond, v611_s); wv t1044 = ({ wv t1040 = rt_get(&F_cond, v611_s); wv t1041 = v612_scope; wv t1042 = v613_ctx; rt_chk(s517_expr(up, t1040, t1041, t1042), "stmt"); }); rt_chk(s480_condition(up, t1043, t1044), "stmt"); });
    RT_AT(4, 714);
    (void)({ wv t1048 = rt_get(&F_body, v611_s); wv t1049 = rt_chk(s490_newScope(up, v612_scope), "stmt"); wv t1050 = ({ wv t1045 = rt_get(&F_func, v613_ctx); wv t1046 = rt_add(rt_get(&F_loops, v613_ctx), rt_i(1)); wv t1047 = rt_get(&F_guards, v613_ctx); rt_rec(0, 3, K51, (wv[]){ t1045, t1046, t1047 }); }); rt_chk(s496_block(up, t1048, t1049, t1050), "stmt"); });
    RT_AT(4, 715);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S42)))) {
    RT_AT(4, 717);
    (void)rt_chk(s498_forLoop(up, v611_s, v612_scope, v613_ctx), "stmt");
    RT_AT(4, 718);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S56)))) {
    RT_AT(4, 720);
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v613_ctx)))) {
      RT_AT(4, 720);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v611_s), rt_s(&S482), RT_NULL), "stmt");
    }
    RT_AT(4, 721);
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S55)))) {
    RT_AT(4, 723);
    if (rt_cond(rt_ne(rt_get(&F_e, v611_s), RT_NULL))) {
      RT_AT(4, 723);
      (void)({ wv t1051 = rt_get(&F_e, v611_s); wv t1052 = v612_scope; wv t1053 = v613_ctx; rt_chk(s517_expr(up, t1051, t1052, t1053), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S178)))) {
    RT_AT(4, 725);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v611_s)), rt_s(&S185)))) {
      RT_AT(4, 726);
      if (rt_cond(rt_get(&F_top, rt_get(&F_func, v613_ctx)))) {
        RT_AT(4, 726);
        (void)rt_chk(s475_error(up, rt_get(&F_pos, rt_get(&F_e, v611_s)), rt_s(&S482), RT_NULL), "stmt");
      }
      RT_AT(4, 727);
      (void)({ wv t1054 = rt_get(&F_arg, rt_get(&F_e, v611_s)); wv t1055 = v612_scope; wv t1056 = v613_ctx; rt_chk(s517_expr(up, t1054, t1055, t1056), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v611_s)), rt_s(&S186)))) {
      RT_AT(4, 729);
      (void)({ wv t1057 = rt_get(&F_e, v611_s); wv t1058 = v612_scope; wv t1059 = v613_ctx; rt_chk(s513_chain(up, t1057, t1058, t1059, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v611_s)), rt_s(&S45)))) {
      RT_AT(4, 731);
      (void)({ wv t1060 = rt_get(&F_e, v611_s); wv t1061 = v612_scope; wv t1062 = v613_ctx; rt_chk(s504_checkMatch(up, t1060, t1061, t1062, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v611_s)), rt_s(&S184)))) {
      RT_AT(4, 734);
      wv v622_types = rt_arr(0, NULL, 1);
      RT_AT(4, 735);
      {
        wfor t1064;
        rt_for_start(&t1064, rt_get(&F_args, rt_get(&F_e, v611_s)), 0);
        for (int64_t t1063 = 0; rt_for_guard(&t1064, rt_get(&F_args, rt_get(&F_e, v611_s)), "s.e.args"), rt_for_more(&t1064, t1063); t1063++) {
          wv v623_a = rt_for_item(&t1064, t1063);
          RT_AT(4, 735);
          (void)({ wv t1065 = v622_types; wv t1066 = rt_chk(s517_expr(up, v623_a, v612_scope, v613_ctx), "stmt"); std_push(t1065, t1066); });
        }
      }
      RT_AT(4, 736);
      (void)({ wv t1067 = rt_get(&F_e, v611_s); wv t1068 = v612_scope; wv t1069 = v622_types; rt_chk(s518_callee(up, t1067, t1068, rt_b(0), t1069), "stmt"); });
    } else {
      RT_AT(4, 738);
      (void)({ wv t1070 = rt_get(&F_e, v611_s); wv t1071 = v612_scope; wv t1072 = v613_ctx; rt_chk(s517_expr(up, t1070, t1071, t1072), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S179)))) {
    RT_AT(4, 741);
    v615_ty = rt_bind(({ wv t1073 = rt_get(&F_e, v611_s); wv t1074 = v612_scope; wv t1075 = v613_ctx; rt_chk(s517_expr(up, t1073, t1074, t1075), "stmt"); }), 1);
  } else if (rt_cond(rt_eq(v614_kind, rt_s(&S38)))) {
    RT_AT(4, 743);
    (void)({ wv t1076 = rt_get(&F_e, v611_s); wv t1077 = v612_scope; wv t1078 = v613_ctx; rt_chk(s517_expr(up, t1076, t1077, t1078), "stmt"); });
    RT_AT(4, 744);
    (void)({ wv t1079 = rt_get(&F_target, v611_s); wv t1080 = v612_scope; wv t1081 = v613_ctx; rt_chk(s514_lvalue(up, t1079, t1080, t1081), "stmt"); });
  }
  RT_AT(4, 747);
  if (rt_cond(rt_ne(rt_get(&F_handler, v611_s), RT_NULL))) {
    RT_AT(4, 747);
    v615_ty = rt_bind(({ wv t1082 = v615_ty; wv t1083 = rt_chk(s503_onfailHandler(up, v611_s, v612_scope, v613_ctx), "stmt"); rt_chk(w5_join(t1082, t1083), "stmt"); }), 1);
  }
  RT_AT(4, 748);
  if (rt_cond(rt_b(rt_bool(rt_eq(v614_kind, rt_s(&S34)), "&&") && rt_bool(rt_not(rt_get(&F_top, rt_get(&F_func, v613_ctx))), "&&")))) {
    RT_AT(4, 749);
    wv v624_id = rt_bind(rt_get(&F_id, rt_get(&F_func, v613_ctx)), 0);
    RT_AT(4, 750);
    ({ wv t1090 = (rt_cond(({ wv t1084 = rt_def(up->v472_returnTypes, "returnTypes"); wv t1085 = v624_id; std_has(t1084, t1085); })) ? ({ wv t1088 = ({ wv t1086 = v624_id; wv t1087 = rt_def(up->v472_returnTypes, "returnTypes"); rt_idx(t1086, t1087); }); wv t1089 = v615_ty; rt_chk(w5_join(t1088, t1089), "stmt"); }) : v615_ty); wv t1091 = v624_id; wv t1092 = rt_def(up->v472_returnTypes, "returnTypes"); rt_set_idx(t1090, t1091, t1092); });
  }
  RT_AT(4, 754);
  if (rt_cond(rt_b(rt_bool(rt_eq(v614_kind, rt_s(&S179)), "&&") && rt_bool(rt_not(rt_b(rt_bool(rt_b(rt_bool(rt_get(&F_isVar, v611_s), "||") || rt_bool(rt_get(&F_isRef, v611_s), "||")), "&&") && rt_bool(({ wv t1093 = rt_def(up->v473_rebound, "rebound"); wv t1094 = rt_get(&F_name, v611_s); std_has(t1093, t1094); }), "&&"))), "&&")))) {
    RT_AT(4, 755);
    (void)({ wv t1095 = rt_get(&F_namePos, v611_s); wv t1096 = (rt_cond(rt_get(&F_isVar, v611_s)) ? rt_chk(w5_thaw(v615_ty), "stmt") : v615_ty); rt_chk(s477_bindType(up, t1095, t1096), "stmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s498_forLoop(struct fr119 *up, wv v625_s, wv v626_scope, wv v627_ctx) {
  rt_enter("forLoop");
  v625_s = rt_bind(v625_s, 0);
  v626_scope = rt_bind(v626_scope, 0);
  v627_ctx = rt_bind(v627_ctx, 0);
  RT_AT(4, 763);
  wv v628_guards = rt_arr(0, NULL, 1);
  RT_AT(4, 764);
  wv v629_k = rt_i(0);
  RT_AT(4, 765);
  while (rt_cond((RT_AT(4, 765), ({ wv t1097 = v629_k; wv t1098 = std_length(rt_get(&F_guards, v627_ctx)); rt_lt(t1097, t1098); })))) {
    RT_AT(4, 766);
    (void)({ wv t1101 = v628_guards; wv t1102 = ({ wv t1099 = v629_k; wv t1100 = rt_get(&F_guards, v627_ctx); rt_idx(t1099, t1100); }); std_push(t1101, t1102); });
    RT_AT(4, 767);
    v629_k = rt_add(v629_k, rt_i(1));
  }
  RT_AT(4, 770);
  wv v630_names = rt_arr(0, NULL, 1);
  RT_AT(4, 771);
  wv v631_en = rt_bind((rt_cond(rt_eq(rt_get(&F_coll, v625_s), RT_NULL)) ? RT_NULL : ({ wv t1103 = rt_get(&F_coll, v625_s); wv t1104 = v626_scope; rt_chk(s507_enumNamed(up, t1103, t1104), "forLoop"); })), 0);
  RT_AT(4, 772);
  if (rt_cond(rt_eq(rt_get(&F_coll, v625_s), RT_NULL))) {
    RT_AT(4, 773);
    (void)({ wv t1108 = rt_get(&F_from, v625_s); wv t1109 = ({ wv t1105 = rt_get(&F_from, v625_s); wv t1106 = v626_scope; wv t1107 = v627_ctx; rt_chk(s517_expr(up, t1105, t1106, t1107), "forLoop"); }); rt_chk(s499_rangeBound(up, t1108, t1109), "forLoop"); });
    RT_AT(4, 774);
    (void)({ wv t1113 = rt_get(&F_to, v625_s); wv t1114 = ({ wv t1110 = rt_get(&F_to, v625_s); wv t1111 = v626_scope; wv t1112 = v627_ctx; rt_chk(s517_expr(up, t1110, t1111, t1112), "forLoop"); }); rt_chk(s499_rangeBound(up, t1113, t1114), "forLoop"); });
    RT_AT(4, 775);
    if (rt_cond(rt_ne(rt_get(&F_step, v625_s), RT_NULL))) {
      RT_AT(4, 776);
      wv v633_st = rt_bind(({ wv t1115 = rt_get(&F_step, v625_s); wv t1116 = v626_scope; wv t1117 = v627_ctx; rt_chk(s517_expr(up, t1115, t1116, t1117), "forLoop"); }), 0);
      RT_AT(4, 777);
      if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v633_st), "forLoop"), "&&") && rt_bool(rt_ne(rt_get(&F_t, v633_st), rt_s(&S139)), "&&")))) {
        RT_AT(4, 777);
        (void)({ wv t1118 = rt_get(&F_pos, rt_get(&F_step, v625_s)); wv t1119 = rt_cat(2, (wv[]){ rt_s(&S483), rt_text(rt_chk(w5_typeText(v633_st), "forLoop")) }); rt_chk(s475_error(up, t1118, t1119, RT_NULL), "forLoop"); });
      }
    }
    RT_AT(4, 779);
    v630_names = rt_arr(1, (wv[]){ rt_def(w5_tInt, "tInt") }, 1);
  } else if (rt_cond(rt_ne(v631_en, RT_NULL))) {
    RT_AT(4, 781);
    v630_names = rt_arr(1, (wv[]){ rt_chk(s478_enumType(up, v631_en), "forLoop") }, 1);
  } else {
    RT_AT(4, 783);
    wv v634_ct = rt_bind(({ wv t1120 = rt_get(&F_coll, v625_s); wv t1121 = v626_scope; wv t1122 = v627_ctx; rt_chk(s517_expr(up, t1120, t1121, t1122), "forLoop"); }), 0);
    RT_AT(4, 784);
    (void)({ wv t1123 = rt_get(&F_coll, v625_s); wv t1124 = v634_ct; rt_chk(s500_collection(up, t1123, t1124), "forLoop"); });
    RT_AT(4, 785);
    if (rt_cond(rt_b(rt_bool(rt_chk(w5_known(v634_ct), "forLoop"), "&&") && rt_bool(rt_eq(rt_get(&F_t, v634_ct), rt_s(&S412)), "&&")))) {
      RT_AT(4, 785);
      v630_names = rt_bind((rt_cond(rt_eq(std_length(rt_get(&F_names, v625_s)), rt_i(2))) ? ({ wv t1125 = rt_def(w5_tInt, "tInt"); wv t1126 = rt_get(&F_elem, v634_ct); rt_arr(2, (wv[]){ t1125, t1126 }, 0); }) : rt_arr(1, (wv[]){ rt_get(&F_elem, v634_ct) }, 0)), 1);
    }
    RT_AT(4, 786);
    wv v635_path = rt_bind(rt_chk(s501_pathOf(up, rt_get(&F_coll, v625_s)), "forLoop"), 0);
    RT_AT(4, 787);
    if (rt_cond(rt_ne(v635_path, RT_NULL))) {
      RT_AT(4, 787);
      (void)std_push(v628_guards, v635_path);
    }
  }
  RT_AT(4, 789);
  wv v632_ls = rt_bind(rt_chk(s490_newScope(up, v626_scope), "forLoop"), 0);
  RT_AT(4, 790);
  v629_k = rt_i(0);
  RT_AT(4, 791);
  while (rt_cond((RT_AT(4, 791), ({ wv t1127 = v629_k; wv t1128 = std_length(rt_get(&F_names, v625_s)); rt_lt(t1127, t1128); })))) {
    RT_AT(4, 792);
    wv v636_n = rt_bind(({ wv t1129 = v629_k; wv t1130 = rt_get(&F_names, v625_s); rt_idx(t1129, t1130); }), 0);
    RT_AT(4, 793);
    (void)({ wv t1134 = v632_ls; wv t1135 = rt_get(&F_name, v636_n); wv t1136 = ({ wv t1131 = rt_get(&F_name, v636_n); wv t1132 = rt_get(&F_pos, v636_n); wv t1133 = rt_get(&F_id, rt_get(&F_func, v627_ctx)); rt_chk(s491_decl(up, rt_s(&S484), t1131, rt_s(&S13), t1132, RT_NULL, t1133, RT_NULL), "forLoop"); }); rt_chk(s493_declare(up, t1134, t1135, t1136), "forLoop"); });
    RT_AT(4, 794);
    if (rt_cond(({ wv t1137 = v629_k; wv t1138 = std_length(v630_names); rt_lt(t1137, t1138); }))) {
      RT_AT(4, 794);
      (void)({ wv t1139 = rt_get(&F_pos, v636_n); wv t1140 = rt_idx(v629_k, v630_names); rt_chk(s477_bindType(up, t1139, t1140), "forLoop"); });
    }
    RT_AT(4, 795);
    v629_k = rt_add(v629_k, rt_i(1));
  }
  RT_AT(4, 797);
  (void)({ wv t1144 = rt_get(&F_body, v625_s); wv t1145 = rt_chk(s490_newScope(up, v632_ls), "forLoop"); wv t1146 = ({ wv t1141 = rt_get(&F_func, v627_ctx); wv t1142 = rt_add(rt_get(&F_loops, v627_ctx), rt_i(1)); wv t1143 = v628_guards; rt_rec(0, 3, K51, (wv[]){ t1141, t1142, t1143 }); }); rt_chk(s496_block(up, t1144, t1145, t1146), "forLoop"); });
  RT_RET(RT_NULL);
}

static wv s499_rangeBound(struct fr119 *up, wv v637_x, wv v638_ty) {
  rt_enter("rangeBound");
  v637_x = rt_bind(v637_x, 0);
  v638_ty = rt_bind(v638_ty, 0);
  RT_AT(4, 802);
  if (rt_cond(rt_b(rt_bool(rt_not(rt_chk(w5_known(v638_ty), "rangeBound")), "||") || rt_bool(rt_eq(rt_get(&F_t, v638_ty), rt_s(&S139)), "||")))) {
    RT_AT(4, 802);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 803);
  wv v639_hint = rt_bind((rt_cond(rt_eq(rt_get(&F_t, v638_ty), rt_s(&S412))) ? rt_s(&S485) : (rt_cond(rt_eq(rt_get(&F_t, v638_ty), rt_s(&S149))) ? rt_s(&S486) : RT_NULL)), 0);
  RT_AT(4, 805);
  (void)({ wv t1147 = rt_get(&F_pos, v637_x); wv t1148 = rt_cat(2, (wv[]){ rt_s(&S487), rt_text(rt_chk(w5_typeText(v638_ty), "rangeBound")) }); wv t1149 = v639_hint; rt_chk(s475_error(up, t1147, t1148, t1149), "rangeBound"); });
  RT_RET(RT_NULL);
}

static wv s500_collection(struct fr119 *up, wv v640_x, wv v641_ty) {
  rt_enter("collection");
  v640_x = rt_bind(v640_x, 0);
  v641_ty = rt_bind(v641_ty, 0);
  RT_AT(4, 810);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_chk(w5_known(v641_ty), "collection")), "||") || rt_bool(rt_eq(rt_get(&F_t, v641_ty), rt_s(&S412)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v641_ty), rt_s(&S189)), "||")))) {
    RT_AT(4, 810);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 811);
  wv v642_hint = RT_NULL;
  RT_AT(4, 812);
  if (rt_cond(rt_eq(rt_get(&F_t, v641_ty), rt_s(&S187)))) {
    RT_AT(4, 813);
    v642_hint = rt_s(&S488);
  } else if (rt_cond(rt_eq(rt_get(&F_t, v641_ty), rt_s(&S414)))) {
    RT_AT(4, 815);
    v642_hint = rt_s(&S489);
  } else if (rt_cond(rt_eq(rt_get(&F_t, v641_ty), rt_s(&S139)))) {
    RT_AT(4, 817);
    v642_hint = rt_s(&S490);
  }
  RT_AT(4, 819);
  (void)({ wv t1150 = rt_get(&F_pos, v640_x); wv t1151 = rt_cat(2, (wv[]){ rt_s(&S491), rt_text(rt_chk(w5_typeText(v641_ty), "collection")) }); wv t1152 = v642_hint; rt_chk(s475_error(up, t1150, t1151, t1152), "collection"); });
  RT_RET(RT_NULL);
}

static wv s501_pathOf(struct fr119 *up, wv v643_x) {
  rt_enter("pathOf");
  v643_x = rt_bind(v643_x, 0);
  RT_AT(4, 824);
  if (rt_cond(rt_eq(rt_get(&F_k, v643_x), rt_s(&S214)))) {
    RT_AT(4, 824);
    RT_RET(rt_arr(1, (wv[]){ rt_get(&F_name, v643_x) }, 0));
  }
  RT_AT(4, 825);
  if (rt_cond(rt_ne(rt_get(&F_k, v643_x), rt_s(&S192)))) {
    RT_AT(4, 825);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 826);
  wv v644_p = rt_bind(rt_chk(s501_pathOf(up, rt_get(&F_rec, v643_x)), "pathOf"), 0);
  RT_AT(4, 827);
  if (rt_cond(rt_eq(v644_p, RT_NULL))) {
    RT_AT(4, 827);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 828);
  wv v645_out = rt_bind(v644_p, 1);
  RT_AT(4, 829);
  (void)({ wv t1153 = v645_out; wv t1154 = rt_get(&F_field, v643_x); std_push(t1153, t1154); });
  RT_RET((RT_AT(4, 830), v645_out));
}

static wv s502_guarded(struct fr119 *up, wv v646_t, wv v647_ctx) {
  rt_enter("guarded");
  v646_t = rt_bind(v646_t, 0);
  v647_ctx = rt_bind(v647_ctx, 0);
  RT_AT(4, 836);
  wv v648_p = rt_bind(rt_chk(s501_pathOf(up, v646_t), "guarded"), 0);
  RT_AT(4, 837);
  if (rt_cond(rt_eq(v648_p, RT_NULL))) {
    RT_AT(4, 837);
    RT_RET(rt_b(0));
  }
  RT_AT(4, 838);
  wv v649_g = rt_i(0);
  RT_AT(4, 839);
  while (rt_cond((RT_AT(4, 839), ({ wv t1155 = v649_g; wv t1156 = std_length(rt_get(&F_guards, v647_ctx)); rt_lt(t1155, t1156); })))) {
    RT_AT(4, 840);
    wv v650_gp = rt_bind(({ wv t1157 = v649_g; wv t1158 = rt_get(&F_guards, v647_ctx); rt_idx(t1157, t1158); }), 0);
    RT_AT(4, 841);
    v649_g = rt_add(v649_g, rt_i(1));
    RT_AT(4, 842);
    wv v651_prefix = ({ wv t1159 = std_length(v648_p); wv t1160 = std_length(v650_gp); rt_le(t1159, t1160); });
    RT_AT(4, 843);
    wv v652_k = rt_i(0);
    RT_AT(4, 844);
    while (rt_cond((RT_AT(4, 844), rt_b(rt_bool(v651_prefix, "&&") && rt_bool(({ wv t1161 = v652_k; wv t1162 = std_length(v648_p); rt_lt(t1161, t1162); }), "&&"))))) {
      RT_AT(4, 845);
      if (rt_cond(({ wv t1163 = rt_idx(v652_k, v648_p); wv t1164 = rt_idx(v652_k, v650_gp); rt_ne(t1163, t1164); }))) {
        RT_AT(4, 845);
        v651_prefix = rt_b(0);
      }
      RT_AT(4, 846);
      v652_k = rt_add(v652_k, rt_i(1));
    }
    RT_AT(4, 848);
    if (rt_cond(v651_prefix)) {
      RT_AT(4, 849);
      wv v653_name = rt_bind(std_join(v648_p, rt_s(&S130)), 0);
      RT_AT(4, 850);
      wv v654_over = rt_bind((rt_cond(({ wv t1165 = std_length(v648_p); wv t1166 = std_length(v650_gp); rt_eq(t1165, t1166); })) ? rt_s(&S492) : rt_cat(3, (wv[]){ rt_s(&S114), rt_text(std_join(v650_gp, rt_s(&S130))), rt_s(&S114) })), 0);
      RT_AT(4, 851);
      (void)({ wv t1169 = rt_get(&F_pos, v646_t); wv t1170 = ({ wv t1167 = rt_text(v653_name); wv t1168 = rt_text(v654_over); rt_cat(4, (wv[]){ rt_s(&S493), t1167, rt_s(&S494), t1168 }); }); wv t1171 = rt_cat(3, (wv[]){ rt_s(&S495), rt_text(v653_name), rt_s(&S496) }); rt_chk(s475_error(up, t1169, t1170, t1171), "guarded"); });
      RT_AT(4, 853);
      RT_RET(rt_b(1));
    }
  }
  RT_RET((RT_AT(4, 856), rt_b(0)));
}

static wv s503_onfailHandler(struct fr119 *up, wv v655_s, wv v656_scope, wv v657_ctx) {
  rt_enter("onfailHandler");
  v655_s = rt_bind(v655_s, 0);
  v656_scope = rt_bind(v656_scope, 0);
  v657_ctx = rt_bind(v657_ctx, 0);
  RT_AT(4, 861);
  wv v658_h = rt_bind(rt_get(&F_handler, v655_s), 0);
  RT_AT(4, 862);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_e, v655_s), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v655_s)), rt_s(&S184)), "||")))) {
    RT_AT(4, 863);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v658_h), rt_s(&S497), rt_s(&S498)), "onfailHandler");
  }
  RT_AT(4, 866);
  wv v659_hs = rt_bind(rt_chk(s490_newScope(up, v656_scope), "onfailHandler"), 0);
  RT_AT(4, 867);
  if (rt_cond(rt_ne(rt_get(&F_param, v658_h), RT_NULL))) {
    RT_AT(4, 868);
    (void)({ wv t1175 = v659_hs; wv t1176 = rt_get(&F_param, v658_h); wv t1177 = ({ wv t1172 = rt_get(&F_param, v658_h); wv t1173 = rt_get(&F_paramPos, v658_h); wv t1174 = rt_get(&F_id, rt_get(&F_func, v657_ctx)); rt_chk(s491_decl(up, rt_s(&S57), t1172, rt_s(&S13), t1173, RT_NULL, t1174, RT_NULL), "onfailHandler"); }); rt_chk(s493_declare(up, t1175, t1176, t1177), "onfailHandler"); });
    RT_AT(4, 869);
    (void)({ wv t1178 = rt_get(&F_paramPos, v658_h); wv t1179 = rt_def(w5_tInt, "tInt"); rt_chk(s477_bindType(up, t1178, t1179), "onfailHandler"); });
  }
  RT_AT(4, 871);
  wv v660_ty = rt_bind(({ wv t1180 = rt_get(&F_body, v658_h); wv t1181 = rt_chk(s490_newScope(up, v659_hs), "onfailHandler"); wv t1182 = v657_ctx; rt_chk(s496_block(up, t1180, t1181, t1182), "onfailHandler"); }), 0);
  RT_AT(4, 872);
  if (rt_cond(({ wv t1183 = rt_def(w5_onfailValueUsed, "onfailValueUsed"); wv t1184 = rt_get(&F_k, v655_s); rt_chk(w1_contains(t1183, t1184), "onfailHandler"); }))) {
    RT_AT(4, 872);
    (void)rt_chk(s489_requireValue(up, rt_get(&F_body, v658_h)), "onfailHandler");
  }
  RT_RET((RT_AT(4, 873), v660_ty));
}

static wv s504_checkMatch(struct fr119 *up, wv v661_e, wv v662_scope, wv v663_ctx, wv v664_stmtLevel) {
  rt_enter("checkMatch");
  v661_e = rt_bind(v661_e, 0);
  v662_scope = rt_bind(v662_scope, 0);
  v663_ctx = rt_bind(v663_ctx, 0);
  v664_stmtLevel = rt_bind(v664_stmtLevel, 0);
  RT_AT(4, 880);
  (void)({ wv t1185 = rt_get(&F_subject, v661_e); wv t1186 = v662_scope; wv t1187 = v663_ctx; rt_chk(s517_expr(up, t1185, t1186, t1187), "checkMatch"); });
  RT_AT(4, 882);
  wv v665_result = rt_bind(rt_def(w5_tNever, "tNever"), 1);
  RT_AT(4, 884);
  wv v666_seen = rt_arr(0, NULL, 1);
  RT_AT(4, 887);
  wv v667_sort = RT_NULL;
  RT_AT(4, 889);
  wv v668_sortEnum = RT_NULL;
  RT_AT(4, 890);
  wv v669_a = rt_i(0);
  RT_AT(4, 891);
  while (rt_cond((RT_AT(4, 891), ({ wv t1188 = v669_a; wv t1189 = std_length(rt_get(&F_cases, v661_e)); rt_lt(t1188, t1189); })))) {
    RT_AT(4, 892);
    wv v670_al = rt_bind(({ wv t1190 = v669_a; wv t1191 = rt_get(&F_cases, v661_e); rt_idx(t1190, t1191); }), 0);
    RT_AT(4, 893);
    wv v671_j = rt_i(0);
    RT_AT(4, 894);
    while (rt_cond((RT_AT(4, 894), ({ wv t1192 = v671_j; wv t1193 = std_length(rt_get(&F_values, v670_al)); rt_lt(t1192, t1193); })))) {
      RT_AT(4, 895);
      wv v672_v = rt_bind(({ wv t1194 = v671_j; wv t1195 = rt_get(&F_values, v670_al); rt_idx(t1194, t1195); }), 0);
      RT_AT(4, 896);
      wv v673_vs = rt_s(&S13);
      RT_AT(4, 897);
      if (rt_cond(rt_eq(rt_get(&F_k, v672_v), rt_s(&S192)))) {
        RT_AT(4, 898);
        wv v677_d = rt_bind(rt_chk(s511_caseEnum(up, v672_v, v662_scope), "checkMatch"), 0);
        RT_AT(4, 899);
        v673_vs = rt_bind((rt_cond(rt_ne(v677_d, RT_NULL)) ? rt_chk(w5_enumText(rt_get(&F_rec, v672_v)), "checkMatch") : RT_NULL), 1);
        RT_AT(4, 900);
        if (rt_cond(rt_eq(v667_sort, RT_NULL))) {
          RT_AT(4, 900);
          v668_sortEnum = rt_bind(v677_d, 1);
        }
      }
      RT_AT(4, 902);
      if (rt_cond(rt_eq(v667_sort, RT_NULL))) {
        RT_AT(4, 903);
        v667_sort = rt_bind(v673_vs, 1);
      } else if (rt_cond(rt_b(rt_bool(rt_ne(v673_vs, RT_NULL), "&&") && rt_bool(rt_ne(v673_vs, v667_sort), "&&")))) {
        RT_AT(4, 905);
        wv v678_message = rt_bind((rt_cond(rt_b(rt_bool(rt_eq(v667_sort, rt_s(&S13)), "||") || rt_bool(rt_eq(v673_vs, rt_s(&S13)), "||"))) ? rt_s(&S499) : ({ wv t1196 = rt_text(v667_sort); wv t1197 = rt_text(v673_vs); rt_cat(5, (wv[]){ rt_s(&S500), t1196, rt_s(&S501), t1197, rt_s(&S114) }); })), 0);
        RT_AT(4, 907);
        (void)({ wv t1198 = rt_get(&F_pos, v672_v); wv t1199 = v678_message; rt_chk(s475_error(up, t1198, t1199, RT_NULL), "checkMatch"); });
      }
      RT_AT(4, 909);
      wv v674_key = rt_bind(rt_chk(w5_caseKey(v672_v), "checkMatch"), 0);
      RT_AT(4, 910);
      wv v675_first = RT_NULL;
      RT_AT(4, 911);
      wv v676_q = rt_i(0);
      RT_AT(4, 912);
      while (rt_cond((RT_AT(4, 912), ({ wv t1200 = v676_q; wv t1201 = std_length(v666_seen); rt_lt(t1200, t1201); })))) {
        RT_AT(4, 913);
        if (rt_cond(({ wv t1202 = rt_get(&F_key, rt_idx(v676_q, v666_seen)); wv t1203 = v674_key; rt_eq(t1202, t1203); }))) {
          RT_AT(4, 913);
          v675_first = rt_bind(rt_get(&F_pos, rt_idx(v676_q, v666_seen)), 1);
        }
        RT_AT(4, 914);
        v676_q = rt_add(v676_q, rt_i(1));
      }
      RT_AT(4, 916);
      if (rt_cond(rt_ne(v675_first, RT_NULL))) {
        RT_AT(4, 917);
        (void)({ wv t1206 = rt_get(&F_pos, v672_v); wv t1207 = ({ wv t1204 = rt_text(rt_chk(w5_caseText(v672_v), "checkMatch")); wv t1205 = rt_text(rt_chk(w5_lineOf(v675_first), "checkMatch")); rt_cat(5, (wv[]){ rt_s(&S502), t1204, rt_s(&S503), t1205, rt_s(&S16) }); }); rt_chk(s475_error(up, t1206, t1207, RT_NULL), "checkMatch"); });
      } else {
        RT_AT(4, 919);
        (void)({ wv t1210 = v666_seen; wv t1211 = ({ wv t1208 = v674_key; wv t1209 = rt_get(&F_pos, v672_v); rt_rec(0, 2, K52, (wv[]){ t1208, t1209 }); }); std_push(t1210, t1211); });
      }
      RT_AT(4, 921);
      v671_j = rt_add(v671_j, rt_i(1));
    }
    RT_AT(4, 923);
    v665_result = rt_bind(({ wv t1215 = v665_result; wv t1216 = ({ wv t1212 = rt_get(&F_body, v670_al); wv t1213 = rt_chk(s490_newScope(up, v662_scope), "checkMatch"); wv t1214 = v663_ctx; rt_chk(s496_block(up, t1212, t1213, t1214), "checkMatch"); }); rt_chk(w5_join(t1215, t1216), "checkMatch"); }), 1);
    RT_AT(4, 924);
    if (rt_cond(rt_not(v664_stmtLevel))) {
      RT_AT(4, 924);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_body, v670_al)), "checkMatch");
    }
    RT_AT(4, 925);
    v669_a = rt_add(v669_a, rt_i(1));
  }
  RT_AT(4, 927);
  if (rt_cond(rt_ne(rt_get(&F_els, v661_e), RT_NULL))) {
    RT_AT(4, 928);
    v665_result = rt_bind(({ wv t1220 = v665_result; wv t1221 = ({ wv t1217 = rt_get(&F_els, v661_e); wv t1218 = rt_chk(s490_newScope(up, v662_scope), "checkMatch"); wv t1219 = v663_ctx; rt_chk(s496_block(up, t1217, t1218, t1219), "checkMatch"); }); rt_chk(w5_join(t1220, t1221), "checkMatch"); }), 1);
    RT_AT(4, 929);
    if (rt_cond(rt_not(v664_stmtLevel))) {
      RT_AT(4, 929);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_els, v661_e)), "checkMatch");
    }
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v667_sort, RT_NULL), "&&") && rt_bool(rt_ne(v667_sort, rt_s(&S13)), "&&")))) {
    RT_AT(4, 932);
    wv v679_ms = rt_bind(rt_chk(s487_enumMembers(up, v668_sortEnum), "checkMatch"), 0);
    RT_AT(4, 933);
    wv v680_missing = rt_arr(0, NULL, 1);
    RT_AT(4, 934);
    wv v681_m = rt_i(0);
    RT_AT(4, 935);
    while (rt_cond((RT_AT(4, 935), ({ wv t1222 = v681_m; wv t1223 = std_length(v679_ms); rt_lt(t1222, t1223); })))) {
      RT_AT(4, 936);
      wv v682_key = ({ wv t1224 = rt_text(rt_get(&F_name, rt_idx(v681_m, v679_ms))); wv t1225 = rt_text(v667_sort); rt_cat(4, (wv[]){ rt_s(&S410), t1224, rt_s(&S130), t1225 }); });
      RT_AT(4, 937);
      wv v683_handled = rt_b(0);
      RT_AT(4, 938);
      wv v684_q = rt_i(0);
      RT_AT(4, 939);
      while (rt_cond((RT_AT(4, 939), ({ wv t1226 = v684_q; wv t1227 = std_length(v666_seen); rt_lt(t1226, t1227); })))) {
        RT_AT(4, 940);
        if (rt_cond(({ wv t1228 = rt_get(&F_key, rt_idx(v684_q, v666_seen)); wv t1229 = v682_key; rt_eq(t1228, t1229); }))) {
          RT_AT(4, 940);
          v683_handled = rt_b(1);
        }
        RT_AT(4, 941);
        v684_q = rt_add(v684_q, rt_i(1));
      }
      RT_AT(4, 943);
      if (rt_cond(rt_not(v683_handled))) {
        RT_AT(4, 943);
        (void)({ wv t1232 = v680_missing; wv t1233 = ({ wv t1230 = rt_text(v667_sort); wv t1231 = rt_text(rt_get(&F_name, rt_idx(v681_m, v679_ms))); rt_cat(5, (wv[]){ rt_s(&S114), t1230, rt_s(&S130), t1231, rt_s(&S114) }); }); std_push(t1232, t1233); });
      }
      RT_AT(4, 944);
      v681_m = rt_add(v681_m, rt_i(1));
    }
    RT_AT(4, 946);
    if (rt_cond(rt_gt(std_length(v680_missing), rt_i(0)))) {
      RT_AT(4, 947);
      (void)({ wv t1234 = rt_get(&F_pos, v661_e); wv t1235 = rt_cat(2, (wv[]){ rt_s(&S504), rt_text(std_join(v680_missing, rt_s(&S419))) }); rt_chk(s475_error(up, t1234, t1235, rt_s(&S505)), "checkMatch"); });
    }
  } else if (rt_cond(rt_not(v664_stmtLevel))) {
    RT_AT(4, 951);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v661_e), rt_s(&S506), RT_NULL), "checkMatch");
  }
  RT_RET((RT_AT(4, 953), v665_result));
}

static wv s505_moduleNamed(struct fr119 *up, wv v685_x, wv v686_scope) {
  rt_enter("moduleNamed");
  v686_scope = rt_bind(v686_scope, 0);
  RT_AT(4, 958);
  if (rt_cond(rt_ne(rt_get(&F_k, v685_x), rt_s(&S214)))) {
    RT_AT(4, 958);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 959);
  wv v687_d = rt_bind(({ wv t1236 = v686_scope; wv t1237 = rt_get(&F_name, v685_x); rt_chk(s494_lookup(up, t1236, t1237), "moduleNamed"); }), 0);
  RT_RET((RT_AT(4, 960), (rt_cond(rt_b(rt_bool(rt_ne(v687_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v687_d), rt_s(&S507)), "&&"))) ? v687_d : RT_NULL)));
}

static wv s506_exported(struct fr119 *up, wv v688_lib, wv v689_name) {
  rt_enter("exported");
  v688_lib = rt_bind(v688_lib, 0);
  v689_name = rt_bind(v689_name, 0);
  RT_AT(4, 965);
  wv v690_xs = rt_bind(rt_get(&F_exports, rt_idx(v688_lib, up->v462_modules)), 0);
  RT_AT(4, 966);
  wv v691_k = rt_i(0);
  RT_AT(4, 967);
  while (rt_cond((RT_AT(4, 967), ({ wv t1238 = v691_k; wv t1239 = std_length(v690_xs); rt_lt(t1238, t1239); })))) {
    RT_AT(4, 968);
    if (rt_cond(({ wv t1240 = rt_get(&F_name, rt_idx(v691_k, v690_xs)); wv t1241 = v689_name; rt_eq(t1240, t1241); }))) {
      RT_AT(4, 968);
      RT_RET(rt_idx(v691_k, v690_xs));
    }
    RT_AT(4, 969);
    v691_k = rt_add(v691_k, rt_i(1));
  }
  RT_RET((RT_AT(4, 971), RT_NULL));
}

static wv s507_enumNamed(struct fr119 *up, wv v692_x, wv v693_scope) {
  rt_enter("enumNamed");
  v693_scope = rt_bind(v693_scope, 0);
  RT_AT(4, 976);
  if (rt_cond(rt_eq(rt_get(&F_k, v692_x), rt_s(&S214)))) {
    RT_AT(4, 977);
    wv v696_d = rt_bind(({ wv t1242 = v693_scope; wv t1243 = rt_get(&F_name, v692_x); rt_chk(s494_lookup(up, t1242, t1243), "enumNamed"); }), 0);
    RT_AT(4, 978);
    RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v696_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v696_d), rt_s(&S47)), "&&"))) ? v696_d : RT_NULL));
  }
  RT_AT(4, 980);
  if (rt_cond(rt_ne(rt_get(&F_k, v692_x), rt_s(&S192)))) {
    RT_AT(4, 980);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 981);
  wv v694_m = rt_bind(({ wv t1244 = rt_get(&F_rec, v692_x); wv t1245 = v693_scope; rt_chk(s505_moduleNamed(up, t1244, t1245), "enumNamed"); }), 0);
  RT_AT(4, 982);
  if (rt_cond(rt_b(rt_bool(rt_eq(v694_m, RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_lib, v694_m), RT_NULL), "||")))) {
    RT_AT(4, 982);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 983);
  wv v695_d = rt_bind(({ wv t1246 = rt_get(&F_lib, v694_m); wv t1247 = rt_get(&F_field, v692_x); rt_chk(s506_exported(up, t1246, t1247), "enumNamed"); }), 0);
  RT_RET((RT_AT(4, 984), (rt_cond(rt_b(rt_bool(rt_ne(v695_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v695_d), rt_s(&S47)), "&&"))) ? v695_d : RT_NULL)));
}

static wv s508_exportOf(struct fr119 *up, wv v697_e, wv v698_m) {
  rt_enter("exportOf");
  v697_e = rt_bind(v697_e, 0);
  v698_m = rt_bind(v698_m, 0);
  RT_AT(4, 990);
  if (rt_cond(rt_eq(rt_get(&F_lib, v698_m), RT_NULL))) {
    RT_AT(4, 990);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 991);
  wv v699_d = rt_bind(({ wv t1248 = rt_get(&F_lib, v698_m); wv t1249 = rt_get(&F_field, v697_e); rt_chk(s506_exported(up, t1248, t1249), "exportOf"); }), 0);
  RT_AT(4, 992);
  if (rt_cond(rt_eq(v699_d, RT_NULL))) {
    RT_AT(4, 992);
    (void)({ wv t1254 = rt_get(&F_pos, v697_e); wv t1255 = ({ wv t1252 = rt_text(rt_get(&F_path, ({ wv t1250 = rt_get(&F_lib, v698_m); wv t1251 = up->v462_modules; rt_idx(t1250, t1251); }))); wv t1253 = rt_text(rt_get(&F_field, v697_e)); rt_cat(5, (wv[]){ rt_s(&S114), t1252, rt_s(&S508), t1253, rt_s(&S114) }); }); rt_chk(s475_error(up, t1254, t1255, RT_NULL), "exportOf"); });
  }
  RT_RET((RT_AT(4, 993), v699_d));
}

static wv s509_unknownModule(struct fr119 *up, wv v700_x, wv v701_scope) {
  rt_enter("unknownModule");
  v700_x = rt_bind(v700_x, 0);
  v701_scope = rt_bind(v701_scope, 0);
  RT_AT(4, 997);
  wv v702_message = rt_bind((rt_cond(rt_ne(({ wv t1256 = v701_scope; wv t1257 = rt_get(&F_name, v700_x); rt_chk(s494_lookup(up, t1256, t1257), "unknownModule"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v700_x)), rt_s(&S509) }) : rt_cat(3, (wv[]){ rt_s(&S510), rt_text(rt_get(&F_name, v700_x)), rt_s(&S114) })), 0);
  RT_AT(4, 998);
  (void)({ wv t1258 = rt_get(&F_pos, v700_x); wv t1259 = v702_message; rt_chk(s475_error(up, t1258, t1259, RT_NULL), "unknownModule"); });
  RT_RET(RT_NULL);
}

static wv s510_qualified(struct fr119 *up, wv v703_e, wv v704_m) {
  rt_enter("qualified");
  v703_e = rt_bind(v703_e, 0);
  v704_m = rt_bind(v704_m, 0);
  RT_AT(4, 1003);
  wv v705_d = rt_bind(rt_chk(s508_exportOf(up, v703_e, v704_m), "qualified"), 0);
  RT_AT(4, 1004);
  wv v706_label = ({ wv t1260 = rt_text(rt_get(&F_name, rt_get(&F_rec, v703_e))); wv t1261 = rt_text(rt_get(&F_field, v703_e)); rt_cat(3, (wv[]){ t1260, rt_s(&S130), t1261 }); });
  RT_AT(4, 1005);
  if (rt_cond(rt_b(rt_bool(rt_ne(v705_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v705_d), rt_s(&S33)), "&&")))) {
    RT_AT(4, 1006);
    (void)({ wv t1264 = rt_get(&F_pos, v703_e); wv t1265 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v706_label), rt_s(&S511) }); wv t1266 = ({ wv t1262 = rt_text(v706_label); wv t1263 = rt_text(v706_label); rt_cat(5, (wv[]){ rt_s(&S512), t1262, rt_s(&S513), t1263, rt_s(&S514) }); }); rt_chk(s475_error(up, t1264, t1265, t1266), "qualified"); });
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v705_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v705_d), rt_s(&S47)), "&&")))) {
    RT_AT(4, 1008);
    (void)({ wv t1267 = rt_get(&F_pos, v703_e); wv t1268 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v706_label), rt_s(&S515) }); wv t1269 = rt_cat(3, (wv[]){ rt_s(&S516), rt_text(v706_label), rt_s(&S517) }); rt_chk(s475_error(up, t1267, t1268, t1269), "qualified"); });
  } else if (rt_cond(rt_ne(v705_d, RT_NULL))) {
    RT_AT(4, 1010);
    RT_RET(rt_chk(s476_typeOfDecl(up, v705_d), "qualified"));
  }
  RT_RET((RT_AT(4, 1012), RT_NULL));
}

static wv s511_caseEnum(struct fr119 *up, wv v707_v, wv v708_scope) {
  rt_enter("caseEnum");
  v707_v = rt_bind(v707_v, 0);
  v708_scope = rt_bind(v708_scope, 0);
  RT_AT(4, 1018);
  wv v709_x = rt_bind(rt_get(&F_rec, v707_v), 0);
  RT_AT(4, 1019);
  wv v710_d = rt_bind(rt_chk(s507_enumNamed(up, v709_x, v708_scope), "caseEnum"), 0);
  RT_AT(4, 1020);
  if (rt_cond(rt_ne(v710_d, RT_NULL))) {
    RT_AT(4, 1021);
    (void)rt_chk(s512_member(up, v707_v, v710_d), "caseEnum");
    RT_AT(4, 1022);
    RT_RET(v710_d);
  }
  RT_AT(4, 1024);
  wv v711_hint = rt_s(&S518);
  RT_AT(4, 1025);
  if (rt_cond(rt_eq(rt_get(&F_k, v709_x), rt_s(&S214)))) {
    RT_AT(4, 1026);
    wv v713_message = rt_bind((rt_cond(rt_ne(({ wv t1270 = v708_scope; wv t1271 = rt_get(&F_name, v709_x); rt_chk(s494_lookup(up, t1270, t1271), "caseEnum"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v709_x)), rt_s(&S519) }) : rt_cat(3, (wv[]){ rt_s(&S520), rt_text(rt_get(&F_name, v709_x)), rt_s(&S114) })), 0);
    RT_AT(4, 1027);
    (void)({ wv t1272 = rt_get(&F_pos, v709_x); wv t1273 = v713_message; wv t1274 = v711_hint; rt_chk(s475_error(up, t1272, t1273, t1274), "caseEnum"); });
    RT_AT(4, 1028);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1030);
  wv v712_m = rt_bind(({ wv t1275 = rt_get(&F_rec, v709_x); wv t1276 = v708_scope; rt_chk(s505_moduleNamed(up, t1275, t1276), "caseEnum"); }), 0);
  RT_AT(4, 1031);
  if (rt_cond(rt_eq(v712_m, RT_NULL))) {
    RT_AT(4, 1032);
    (void)({ wv t1277 = rt_get(&F_rec, v709_x); wv t1278 = v708_scope; rt_chk(s509_unknownModule(up, t1277, t1278), "caseEnum"); });
  } else if (rt_cond(rt_ne(rt_chk(s508_exportOf(up, v709_x, v712_m), "caseEnum"), RT_NULL))) {
    RT_AT(4, 1034);
    (void)({ wv t1279 = rt_get(&F_pos, v709_x); wv t1280 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_chk(w5_enumText(v709_x), "caseEnum")), rt_s(&S519) }); wv t1281 = v711_hint; rt_chk(s475_error(up, t1279, t1280, t1281), "caseEnum"); });
  }
  RT_RET((RT_AT(4, 1036), RT_NULL));
}

static wv s512_member(struct fr119 *up, wv v714_e, wv v715_d) {
  rt_enter("member");
  v714_e = rt_bind(v714_e, 0);
  v715_d = rt_bind(v715_d, 0);
  RT_AT(4, 1041);
  wv v716_ms = rt_bind(rt_chk(s487_enumMembers(up, v715_d), "member"), 0);
  RT_AT(4, 1042);
  wv v717_names = rt_arr(0, NULL, 1);
  RT_AT(4, 1043);
  wv v718_found = rt_b(0);
  RT_AT(4, 1044);
  wv v719_k = rt_i(0);
  RT_AT(4, 1045);
  while (rt_cond((RT_AT(4, 1045), ({ wv t1282 = v719_k; wv t1283 = std_length(v716_ms); rt_lt(t1282, t1283); })))) {
    RT_AT(4, 1046);
    (void)({ wv t1284 = v717_names; wv t1285 = rt_get(&F_name, rt_idx(v719_k, v716_ms)); std_push(t1284, t1285); });
    RT_AT(4, 1047);
    if (rt_cond(({ wv t1286 = rt_get(&F_name, rt_idx(v719_k, v716_ms)); wv t1287 = rt_get(&F_field, v714_e); rt_eq(t1286, t1287); }))) {
      RT_AT(4, 1047);
      v718_found = rt_b(1);
    }
    RT_AT(4, 1048);
    v719_k = rt_add(v719_k, rt_i(1));
  }
  RT_AT(4, 1050);
  if (rt_cond(rt_not(v718_found))) {
    RT_AT(4, 1051);
    (void)({ wv t1290 = rt_get(&F_pos, v714_e); wv t1291 = ({ wv t1288 = rt_text(rt_get(&F_field, v714_e)); wv t1289 = rt_text(rt_get(&F_name, v715_d)); rt_cat(5, (wv[]){ rt_s(&S521), t1288, rt_s(&S522), t1289, rt_s(&S114) }); }); wv t1292 = rt_cat(2, (wv[]){ rt_s(&S523), rt_text(std_join(v717_names, rt_s(&S419))) }); rt_chk(s475_error(up, t1290, t1291, t1292), "member"); });
  }
  RT_RET(RT_NULL);
}

static wv s513_chain(struct fr119 *up, wv v720_e, wv v721_scope, wv v722_ctx, wv v723_stmtLevel) {
  rt_enter("chain");
  v720_e = rt_bind(v720_e, 0);
  v721_scope = rt_bind(v721_scope, 0);
  v722_ctx = rt_bind(v722_ctx, 0);
  v723_stmtLevel = rt_bind(v723_stmtLevel, 0);
  RT_AT(4, 1057);
  wv v724_result = rt_bind(rt_def(w5_tNever, "tNever"), 1);
  RT_AT(4, 1058);
  wv v725_k = rt_i(0);
  RT_AT(4, 1059);
  while (rt_cond((RT_AT(4, 1059), ({ wv t1293 = v725_k; wv t1294 = std_length(rt_get(&F_branches, v720_e)); rt_lt(t1293, t1294); })))) {
    RT_AT(4, 1060);
    wv v726_b = rt_bind(({ wv t1295 = v725_k; wv t1296 = rt_get(&F_branches, v720_e); rt_idx(t1295, t1296); }), 0);
    RT_AT(4, 1061);
    (void)({ wv t1300 = rt_get(&F_cond, v726_b); wv t1301 = ({ wv t1297 = rt_get(&F_cond, v726_b); wv t1298 = v721_scope; wv t1299 = v722_ctx; rt_chk(s517_expr(up, t1297, t1298, t1299), "chain"); }); rt_chk(s480_condition(up, t1300, t1301), "chain"); });
    RT_AT(4, 1062);
    v724_result = rt_bind(({ wv t1305 = v724_result; wv t1306 = ({ wv t1302 = rt_get(&F_body, v726_b); wv t1303 = rt_chk(s490_newScope(up, v721_scope), "chain"); wv t1304 = v722_ctx; rt_chk(s496_block(up, t1302, t1303, t1304), "chain"); }); rt_chk(w5_join(t1305, t1306), "chain"); }), 1);
    RT_AT(4, 1063);
    if (rt_cond(rt_not(v723_stmtLevel))) {
      RT_AT(4, 1063);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_body, v726_b)), "chain");
    }
    RT_AT(4, 1064);
    v725_k = rt_add(v725_k, rt_i(1));
  }
  RT_AT(4, 1066);
  if (rt_cond(rt_ne(rt_get(&F_els, v720_e), RT_NULL))) {
    RT_AT(4, 1067);
    v724_result = rt_bind(({ wv t1310 = v724_result; wv t1311 = ({ wv t1307 = rt_get(&F_els, v720_e); wv t1308 = rt_chk(s490_newScope(up, v721_scope), "chain"); wv t1309 = v722_ctx; rt_chk(s496_block(up, t1307, t1308, t1309), "chain"); }); rt_chk(w5_join(t1310, t1311), "chain"); }), 1);
    RT_AT(4, 1068);
    if (rt_cond(rt_not(v723_stmtLevel))) {
      RT_AT(4, 1068);
      (void)rt_chk(s489_requireValue(up, rt_get(&F_els, v720_e)), "chain");
    }
  } else if (rt_cond(rt_not(v723_stmtLevel))) {
    RT_AT(4, 1070);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v720_e), rt_s(&S524), RT_NULL), "chain");
  }
  RT_RET((RT_AT(4, 1072), v724_result));
}

static wv s514_lvalue(struct fr119 *up, wv v727_t, wv v728_scope, wv v729_ctx) {
  rt_enter("lvalue");
  v728_scope = rt_bind(v728_scope, 0);
  v729_ctx = rt_bind(v729_ctx, 0);
  RT_AT(4, 1076);
  if (rt_cond(rt_chk(s502_guarded(up, v727_t, v729_ctx), "lvalue"))) {
    RT_AT(4, 1076);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1077);
  if (rt_cond(rt_eq(rt_get(&F_k, v727_t), rt_s(&S214)))) {
    RT_AT(4, 1078);
    wv v735_d = rt_bind(({ wv t1312 = rt_get(&F_name, v727_t); wv t1313 = rt_get(&F_pos, v727_t); wv t1314 = v728_scope; wv t1315 = v729_ctx; rt_chk(s495_resolve(up, t1312, t1313, t1314, t1315), "lvalue"); }), 0);
    RT_AT(4, 1079);
    if (rt_cond(rt_ne(v735_d, RT_NULL))) {
      RT_AT(4, 1080);
      (void)({ wv t1316 = v735_d; wv t1317 = rt_get(&F_name, v727_t); wv t1318 = rt_get(&F_pos, v727_t); wv t1319 = rt_cat(3, (wv[]){ rt_s(&S493), rt_text(rt_get(&F_name, v727_t)), rt_s(&S525) }); rt_chk(s515_mustBeMutable(up, t1316, t1317, t1318, t1319, rt_b(1)), "lvalue"); });
      RT_AT(4, 1082);
      if (rt_cond(rt_get(&F_isRef, v735_d))) {
        RT_AT(4, 1082);
        (void)({ wv t1322 = rt_get(&F_refTargets, ({ wv t1320 = rt_def(up->v466_curMod, "curMod"); wv t1321 = up->v462_modules; rt_idx(t1320, t1321); })); wv t1323 = rt_get(&F_pos, v727_t); std_push(t1322, t1323); });
      }
    }
    RT_AT(4, 1084);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1088);
  wv v730_isMem = rt_eq(rt_get(&F_k, v727_t), rt_s(&S192));
  RT_AT(4, 1089);
  wv v731_box = rt_bind((rt_cond(v730_isMem) ? rt_get(&F_rec, v727_t) : rt_get(&F_arr, v727_t)), 0);
  RT_AT(4, 1090);
  wv v732_what = rt_bind((rt_cond(v730_isMem) ? rt_s(&S526) : rt_s(&S527)), 0);
  RT_AT(4, 1091);
  wv v733_it = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v727_t), rt_s(&S191))) ? ({ wv t1324 = rt_get(&F_index, v727_t); wv t1325 = v728_scope; wv t1326 = v729_ctx; rt_chk(s517_expr(up, t1324, t1325, t1326), "lvalue"); }) : RT_NULL), 0);
  RT_AT(4, 1092);
  if (rt_cond(v730_isMem)) {
    RT_AT(4, 1093);
    if (rt_cond(rt_ne(rt_chk(s507_enumNamed(up, v731_box, v728_scope), "lvalue"), RT_NULL))) {
      RT_AT(4, 1094);
      (void)({ wv t1329 = rt_get(&F_pos, v727_t); wv t1330 = ({ wv t1327 = rt_text(rt_chk(w5_enumText(v731_box), "lvalue")); wv t1328 = rt_text(rt_get(&F_field, v727_t)); rt_cat(5, (wv[]){ rt_s(&S493), t1327, rt_s(&S130), t1328, rt_s(&S528) }); }); rt_chk(s475_error(up, t1329, t1330, RT_NULL), "lvalue"); });
      RT_AT(4, 1095);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1098);
    wv v736_m = rt_bind(rt_chk(s505_moduleNamed(up, v731_box, v728_scope), "lvalue"), 0);
    RT_AT(4, 1099);
    if (rt_cond(rt_ne(v736_m, RT_NULL))) {
      RT_AT(4, 1100);
      if (rt_cond(rt_ne(rt_chk(s508_exportOf(up, v727_t, v736_m), "lvalue"), RT_NULL))) {
        RT_AT(4, 1101);
        (void)({ wv t1336 = rt_get(&F_pos, v727_t); wv t1337 = ({ wv t1333 = rt_text(rt_get(&F_name, v731_box)); wv t1334 = rt_text(rt_get(&F_field, v727_t)); wv t1335 = rt_text(rt_get(&F_path, ({ wv t1331 = rt_get(&F_lib, v736_m); wv t1332 = up->v462_modules; rt_idx(t1331, t1332); }))); rt_cat(7, (wv[]){ rt_s(&S493), t1333, rt_s(&S130), t1334, rt_s(&S529), t1335, rt_s(&S530) }); }); rt_chk(s475_error(up, t1336, t1337, RT_NULL), "lvalue"); });
      }
      RT_AT(4, 1103);
      RT_RET(RT_NULL);
    }
  }
  RT_AT(4, 1107);
  wv v734_bt = RT_NULL;
  RT_AT(4, 1108);
  if (rt_cond(rt_eq(rt_get(&F_k, v731_box), rt_s(&S214)))) {
    RT_AT(4, 1109);
    wv v737_d = rt_bind(({ wv t1338 = rt_get(&F_name, v731_box); wv t1339 = rt_get(&F_pos, v731_box); wv t1340 = v728_scope; wv t1341 = v729_ctx; rt_chk(s495_resolve(up, t1338, t1339, t1340, t1341), "lvalue"); }), 0);
    RT_AT(4, 1110);
    if (rt_cond(rt_ne(v737_d, RT_NULL))) {
      RT_AT(4, 1111);
      (void)({ wv t1344 = v737_d; wv t1345 = rt_get(&F_name, v731_box); wv t1346 = rt_get(&F_pos, v731_box); wv t1347 = ({ wv t1342 = rt_text(v732_what); wv t1343 = rt_text(rt_get(&F_name, v731_box)); rt_cat(5, (wv[]){ rt_s(&S531), t1342, rt_s(&S532), t1343, rt_s(&S533) }); }); rt_chk(s515_mustBeMutable(up, t1344, t1345, t1346, t1347, rt_b(0)), "lvalue"); });
      RT_AT(4, 1112);
      v734_bt = rt_bind(rt_chk(s476_typeOfDecl(up, v737_d), "lvalue"), 1);
    }
  } else {
    RT_AT(4, 1115);
    v734_bt = rt_bind(rt_chk(s517_expr(up, v731_box, v728_scope, v729_ctx), "lvalue"), 1);
  }
  RT_AT(4, 1117);
  if (rt_cond(v730_isMem)) {
    RT_AT(4, 1118);
    (void)({ wv t1348 = v734_bt; wv t1349 = rt_get(&F_field, v727_t); wv t1350 = rt_get(&F_pos, v727_t); rt_chk(s483_fieldOf(up, t1348, t1349, t1350, rt_s(&S534)), "lvalue"); });
  } else {
    RT_AT(4, 1120);
    (void)({ wv t1351 = v734_bt; wv t1352 = rt_get(&F_index, v727_t); wv t1353 = v733_it; wv t1354 = rt_get(&F_pos, v727_t); rt_chk(s484_indexed(up, t1351, t1352, t1353, t1354, rt_s(&S534)), "lvalue"); });
  }
  RT_RET(RT_NULL);
}

static wv s515_mustBeMutable(struct fr119 *up, wv v738_d, wv v739_name, wv v740_pos, wv v741_message, wv v742_rebind) {
  rt_enter("mustBeMutable");
  v738_d = rt_bind(v738_d, 0);
  v739_name = rt_bind(v739_name, 0);
  v740_pos = rt_bind(v740_pos, 0);
  v741_message = rt_bind(v741_message, 0);
  v742_rebind = rt_bind(v742_rebind, 0);
  RT_AT(4, 1128);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v738_d), rt_s(&S33)), "||") || rt_bool(rt_eq(rt_get(&F_kind, v738_d), rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_kind, v738_d), rt_s(&S507)), "||")))) {
    RT_AT(4, 1129);
    (void)({ wv t1357 = v740_pos; wv t1358 = ({ wv t1355 = rt_text(v739_name); wv t1356 = rt_text(rt_chk(w5_kindText(rt_get(&F_kind, v738_d)), "mustBeMutable")); rt_cat(5, (wv[]){ rt_s(&S114), t1355, rt_s(&S535), t1356, rt_s(&S536) }); }); rt_chk(s475_error(up, t1357, t1358, RT_NULL), "mustBeMutable"); });
  } else if (rt_cond(rt_eq(rt_get(&F_kind, v738_d), rt_s(&S484)))) {
    RT_AT(4, 1132);
    if (rt_cond(v742_rebind)) {
      RT_AT(4, 1133);
      (void)({ wv t1359 = v740_pos; wv t1360 = rt_cat(3, (wv[]){ rt_s(&S493), rt_text(v739_name), rt_s(&S537) }); rt_chk(s475_error(up, t1359, t1360, rt_s(&S538)), "mustBeMutable"); });
    }
  } else if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_isVar, v738_d)), "&&") && rt_bool(rt_not(rt_get(&F_isRef, v738_d)), "&&")))) {
    RT_AT(4, 1137);
    wv v743_hint = RT_NULL;
    RT_AT(4, 1138);
    if (rt_cond(rt_eq(rt_get(&F_kind, v738_d), rt_s(&S479)))) {
      RT_AT(4, 1139);
      v743_hint = rt_bind((rt_cond(v742_rebind) ? ({ wv t1361 = rt_text(v739_name); wv t1362 = rt_text(v739_name); rt_cat(5, (wv[]){ rt_s(&S539), t1361, rt_s(&S540), t1362, rt_s(&S541) }); }) : rt_cat(3, (wv[]){ rt_s(&S542), rt_text(v739_name), rt_s(&S16) })), 1);
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v738_d), rt_s(&S179)))) {
      RT_AT(4, 1142);
      v743_hint = rt_bind((rt_cond(v742_rebind) ? ({ wv t1363 = rt_text(v739_name); wv t1364 = rt_text(v739_name); rt_cat(5, (wv[]){ rt_s(&S543), t1363, rt_s(&S544), t1364, rt_s(&S545) }); }) : rt_cat(3, (wv[]){ rt_s(&S546), rt_text(v739_name), rt_s(&S547) })), 1);
    }
    RT_AT(4, 1145);
    (void)rt_chk(s475_error(up, v740_pos, v741_message, v743_hint), "mustBeMutable");
  }
  RT_RET(RT_NULL);
}

static wv s516_unknownStd(struct fr119 *up, wv v744_node) {
  rt_enter("unknownStd");
  v744_node = rt_bind(v744_node, 0);
  RT_AT(4, 1150);
  (void)({ wv t1365 = rt_get(&F_pos, v744_node); wv t1366 = rt_cat(3, (wv[]){ rt_s(&S548), rt_text(rt_get(&F_name, v744_node)), rt_s(&S114) }); rt_chk(s475_error(up, t1365, t1366, rt_s(&S137)), "unknownStd"); });
  RT_RET(RT_NULL);
}

static wv s517_expr(struct fr119 *up, wv v745_e, wv v746_scope, wv v747_ctx) {
  rt_enter("expr");
  v746_scope = rt_bind(v746_scope, 0);
  v747_ctx = rt_bind(v747_ctx, 0);
  RT_AT(4, 1156);
  wv v748_kind = rt_bind(rt_get(&F_k, v745_e), 0);
  RT_AT(4, 1157);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S139)))) {
    RT_AT(4, 1157);
    RT_RET(rt_def(w5_tInt, "tInt"));
  }
  RT_AT(4, 1158);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v748_kind, rt_s(&S149)), "||") || rt_bool(rt_eq(v748_kind, rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(v748_kind, rt_s(&S54)), "||")))) {
    RT_AT(4, 1158);
    RT_RET(rt_def(w5_tFloat, "tFloat"));
  }
  RT_AT(4, 1159);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S300)))) {
    RT_AT(4, 1159);
    RT_RET(rt_def(w5_tBool, "tBool"));
  }
  RT_AT(4, 1160);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S52)))) {
    RT_AT(4, 1160);
    RT_RET(rt_def(w5_tNull, "tNull"));
  }
  RT_AT(4, 1161);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S173)))) {
    RT_AT(4, 1162);
    {
      wfor t1368;
      rt_for_start(&t1368, rt_get(&F_parts, v745_e), 0);
      for (int64_t t1367 = 0; rt_for_guard(&t1368, rt_get(&F_parts, v745_e), "e.parts"), rt_for_more(&t1368, t1367); t1367++) {
        wv v749_part = rt_for_item(&t1368, t1367);
        RT_AT(4, 1163);
        if (rt_cond(rt_ne(std_typeOf(v749_part), rt_s(&S187)))) {
          RT_AT(4, 1163);
          (void)({ wv t1369 = v749_part; wv t1370 = rt_chk(s517_expr(up, v749_part, v746_scope, v747_ctx), "expr"); rt_chk(s479_noText(up, t1369, t1370), "expr"); });
        }
      }
    }
    RT_AT(4, 1165);
    RT_RET(rt_def(w5_tString, "tString"));
  }
  RT_AT(4, 1167);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S214)))) {
    RT_AT(4, 1168);
    wv v750_d = rt_bind(({ wv t1371 = rt_get(&F_name, v745_e); wv t1372 = rt_get(&F_pos, v745_e); wv t1373 = v746_scope; wv t1374 = v747_ctx; rt_chk(s495_resolve(up, t1371, t1372, t1373, t1374), "expr"); }), 0);
    RT_AT(4, 1169);
    if (rt_cond(rt_b(rt_bool(rt_ne(v750_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v750_d), rt_s(&S33)), "&&")))) {
      RT_AT(4, 1170);
      (void)({ wv t1377 = rt_get(&F_pos, v745_e); wv t1378 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v745_e)), rt_s(&S511) }); wv t1379 = ({ wv t1375 = rt_text(rt_get(&F_name, v745_e)); wv t1376 = rt_text(rt_get(&F_name, v745_e)); rt_cat(5, (wv[]){ rt_s(&S512), t1375, rt_s(&S513), t1376, rt_s(&S514) }); }); rt_chk(s475_error(up, t1377, t1378, t1379), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v750_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v750_d), rt_s(&S47)), "&&")))) {
      RT_AT(4, 1172);
      (void)({ wv t1380 = rt_get(&F_pos, v745_e); wv t1381 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v745_e)), rt_s(&S515) }); wv t1382 = rt_cat(3, (wv[]){ rt_s(&S516), rt_text(rt_get(&F_name, v745_e)), rt_s(&S517) }); rt_chk(s475_error(up, t1380, t1381, t1382), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v750_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v750_d), rt_s(&S507)), "&&")))) {
      RT_AT(4, 1174);
      (void)({ wv t1383 = rt_get(&F_pos, v745_e); wv t1384 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(rt_get(&F_name, v745_e)), rt_s(&S549) }); wv t1385 = rt_cat(3, (wv[]){ rt_s(&S550), rt_text(rt_get(&F_name, v745_e)), rt_s(&S551) }); rt_chk(s475_error(up, t1383, t1384, t1385), "expr"); });
    } else if (rt_cond(rt_ne(v750_d, RT_NULL))) {
      RT_AT(4, 1176);
      RT_RET(rt_chk(s476_typeOfDecl(up, v750_d), "expr"));
    }
    RT_AT(4, 1178);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1180);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S157)))) {
    RT_AT(4, 1181);
    wv v751_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v745_e)), "expr"), 0);
    RT_AT(4, 1182);
    if (rt_cond(rt_eq(v751_sig, RT_NULL))) {
      RT_AT(4, 1183);
      (void)rt_chk(s516_unknownStd(up, v745_e), "expr");
    } else if (rt_cond(rt_ne(v751_sig, rt_s(&S35)))) {
      RT_AT(4, 1185);
      (void)({ wv t1388 = rt_get(&F_pos, v745_e); wv t1389 = ({ wv t1386 = rt_text(rt_get(&F_name, v745_e)); wv t1387 = rt_text(rt_get(&F_name, v745_e)); rt_cat(5, (wv[]){ rt_s(&S181), t1386, rt_s(&S552), t1387, rt_s(&S16) }); }); rt_chk(s475_error(up, t1388, t1389, RT_NULL), "expr"); });
    }
    RT_AT(4, 1187);
    RT_RET((rt_cond(rt_eq(rt_get(&F_name, v745_e), rt_s(&S367))) ? rt_chk(w5_arrayOf(rt_def(w5_tString, "tString")), "expr") : (rt_cond(rt_eq(rt_get(&F_name, v745_e), rt_s(&S369))) ? rt_def(w5_tString, "tString") : RT_NULL)));
  }
  RT_AT(4, 1190);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S188)))) {
    RT_AT(4, 1191);
    wv v752_elem = rt_bind(rt_def(w5_tNever, "tNever"), 1);
    RT_AT(4, 1192);
    {
      wfor t1391;
      rt_for_start(&t1391, rt_get(&F_items, v745_e), 0);
      for (int64_t t1390 = 0; rt_for_guard(&t1391, rt_get(&F_items, v745_e), "e.items"), rt_for_more(&t1391, t1390); t1390++) {
        wv v753_item = rt_for_item(&t1391, t1390);
        RT_AT(4, 1192);
        v752_elem = rt_bind(({ wv t1392 = v752_elem; wv t1393 = rt_chk(s517_expr(up, v753_item, v746_scope, v747_ctx), "expr"); rt_chk(w5_join(t1392, t1393), "expr"); }), 1);
      }
    }
    RT_AT(4, 1193);
    RT_RET(rt_chk(w5_arrayOf(v752_elem), "expr"));
  }
  RT_AT(4, 1195);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S189)))) {
    RT_AT(4, 1196);
    {
      wfor t1395;
      rt_for_start(&t1395, rt_get(&F_items, v745_e), 0);
      for (int64_t t1394 = 0; rt_for_guard(&t1395, rt_get(&F_items, v745_e), "e.items"), rt_for_more(&t1395, t1394); t1394++) {
        wv v754_item = rt_for_item(&t1395, t1394);
        RT_AT(4, 1196);
        (void)rt_chk(s517_expr(up, v754_item, v746_scope, v747_ctx), "expr");
      }
    }
    RT_AT(4, 1197);
    RT_RET(rt_def(w5_tMap, "tMap"));
  }
  RT_AT(4, 1200);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S190)))) {
    RT_AT(4, 1201);
    wv v755_names = rt_arr(0, NULL, 1);
    RT_AT(4, 1202);
    wv v756_types = rt_arr(0, NULL, 1);
    RT_AT(4, 1203);
    {
      wfor t1397;
      rt_for_start(&t1397, rt_get(&F_fields, v745_e), 0);
      for (int64_t t1396 = 0; rt_for_guard(&t1397, rt_get(&F_fields, v745_e), "e.fields"), rt_for_more(&t1397, t1396); t1396++) {
        wv v757_f = rt_for_item(&t1397, t1396);
        RT_AT(4, 1204);
        (void)({ wv t1398 = v755_names; wv t1399 = rt_get(&F_name, v757_f); std_push(t1398, t1399); });
        RT_AT(4, 1205);
        (void)({ wv t1403 = v756_types; wv t1404 = ({ wv t1400 = rt_get(&F_e, v757_f); wv t1401 = v746_scope; wv t1402 = v747_ctx; rt_chk(s517_expr(up, t1400, t1401, t1402), "expr"); }); std_push(t1403, t1404); });
      }
    }
    RT_AT(4, 1207);
    RT_RET(rt_chk(w5_recordType(v755_names, v756_types), "expr"));
  }
  RT_AT(4, 1209);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S191)))) {
    RT_AT(4, 1210);
    wv v758_it = rt_bind(({ wv t1405 = rt_get(&F_index, v745_e); wv t1406 = v746_scope; wv t1407 = v747_ctx; rt_chk(s517_expr(up, t1405, t1406, t1407), "expr"); }), 0);
    RT_AT(4, 1211);
    RT_RET(({ wv t1411 = ({ wv t1408 = rt_get(&F_arr, v745_e); wv t1409 = v746_scope; wv t1410 = v747_ctx; rt_chk(s517_expr(up, t1408, t1409, t1410), "expr"); }); wv t1412 = rt_get(&F_index, v745_e); wv t1413 = v758_it; wv t1414 = rt_get(&F_pos, v745_e); rt_chk(s484_indexed(up, t1411, t1412, t1413, t1414, rt_s(&S553)), "expr"); }));
  }
  RT_AT(4, 1215);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S192)))) {
    RT_AT(4, 1216);
    wv v759_lin = rt_bind(({ wv t1415 = rt_get(&F_rec, v745_e); wv t1416 = v746_scope; rt_chk(s507_enumNamed(up, t1415, t1416), "expr"); }), 0);
    RT_AT(4, 1217);
    if (rt_cond(rt_ne(v759_lin, RT_NULL))) {
      RT_AT(4, 1218);
      (void)rt_chk(s512_member(up, v745_e, v759_lin), "expr");
      RT_AT(4, 1219);
      RT_RET(rt_chk(s478_enumType(up, v759_lin), "expr"));
    }
    RT_AT(4, 1221);
    wv v760_m = rt_bind(({ wv t1417 = rt_get(&F_rec, v745_e); wv t1418 = v746_scope; rt_chk(s505_moduleNamed(up, t1417, t1418), "expr"); }), 0);
    RT_AT(4, 1222);
    if (rt_cond(rt_ne(v760_m, RT_NULL))) {
      RT_AT(4, 1222);
      RT_RET(rt_chk(s510_qualified(up, v745_e, v760_m), "expr"));
    }
    RT_AT(4, 1223);
    RT_RET(({ wv t1422 = ({ wv t1419 = rt_get(&F_rec, v745_e); wv t1420 = v746_scope; wv t1421 = v747_ctx; rt_chk(s517_expr(up, t1419, t1420, t1421), "expr"); }); wv t1423 = rt_get(&F_field, v745_e); wv t1424 = rt_get(&F_pos, v745_e); rt_chk(s483_fieldOf(up, t1422, t1423, t1424, rt_s(&S553)), "expr"); }));
  }
  RT_AT(4, 1225);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S193)))) {
    RT_AT(4, 1226);
    wv v761_ot = rt_bind(({ wv t1425 = rt_get(&F_e, v745_e); wv t1426 = v746_scope; wv t1427 = v747_ctx; rt_chk(s517_expr(up, t1425, t1426, t1427), "expr"); }), 0);
    RT_AT(4, 1227);
    if (rt_cond(rt_eq(rt_get(&F_op, v745_e), rt_s(&S95)))) {
      RT_AT(4, 1228);
      (void)({ wv t1428 = rt_get(&F_e, v745_e); wv t1429 = v761_ot; rt_chk(s481_needBool(up, rt_s(&S95), t1428, t1429), "expr"); });
      RT_AT(4, 1229);
      RT_RET(rt_def(w5_tBool, "tBool"));
    }
    RT_AT(4, 1231);
    if (rt_cond(rt_eq(rt_get(&F_op, v745_e), rt_s(&S94)))) {
      RT_AT(4, 1232);
      (void)({ wv t1430 = rt_get(&F_e, v745_e); wv t1431 = v761_ot; rt_chk(s482_needInt(up, rt_s(&S94), t1430, t1431), "expr"); });
      RT_AT(4, 1233);
      RT_RET(rt_def(w5_tInt, "tInt"));
    }
    RT_AT(4, 1235);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_chk(w5_known(v761_ot), "expr")), "||") || rt_bool(rt_eq(rt_get(&F_t, v761_ot), rt_s(&S139)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v761_ot), rt_s(&S149)), "||")))) {
      RT_AT(4, 1235);
      RT_RET(v761_ot);
    }
    RT_AT(4, 1236);
    (void)({ wv t1432 = rt_get(&F_pos, v745_e); wv t1433 = rt_cat(2, (wv[]){ rt_s(&S554), rt_text(rt_chk(w5_typeText(v761_ot), "expr")) }); rt_chk(s475_error(up, t1432, t1433, RT_NULL), "expr"); });
    RT_AT(4, 1237);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1239);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S194)))) {
    RT_AT(4, 1240);
    wv v762_lt = rt_bind(({ wv t1434 = rt_get(&F_l, v745_e); wv t1435 = v746_scope; wv t1436 = v747_ctx; rt_chk(s517_expr(up, t1434, t1435, t1436), "expr"); }), 0);
    RT_AT(4, 1241);
    RT_RET(({ wv t1440 = v745_e; wv t1441 = v762_lt; wv t1442 = ({ wv t1437 = rt_get(&F_r, v745_e); wv t1438 = v746_scope; wv t1439 = v747_ctx; rt_chk(s517_expr(up, t1437, t1438, t1439), "expr"); }); rt_chk(s485_binary(up, t1440, t1441, t1442), "expr"); }));
  }
  RT_AT(4, 1243);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S195)))) {
    RT_AT(4, 1244);
    (void)({ wv t1446 = rt_get(&F_c, v745_e); wv t1447 = ({ wv t1443 = rt_get(&F_c, v745_e); wv t1444 = v746_scope; wv t1445 = v747_ctx; rt_chk(s517_expr(up, t1443, t1444, t1445), "expr"); }); rt_chk(s480_condition(up, t1446, t1447), "expr"); });
    RT_AT(4, 1245);
    wv v763_at = rt_bind(({ wv t1448 = rt_get(&F_a, v745_e); wv t1449 = v746_scope; wv t1450 = v747_ctx; rt_chk(s517_expr(up, t1448, t1449, t1450), "expr"); }), 0);
    RT_AT(4, 1246);
    RT_RET(({ wv t1454 = v763_at; wv t1455 = ({ wv t1451 = rt_get(&F_b, v745_e); wv t1452 = v746_scope; wv t1453 = v747_ctx; rt_chk(s517_expr(up, t1451, t1452, t1453), "expr"); }); rt_chk(w5_join(t1454, t1455), "expr"); }));
  }
  RT_AT(4, 1248);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S184)))) {
    RT_AT(4, 1249);
    wv v764_types = rt_arr(0, NULL, 1);
    RT_AT(4, 1250);
    {
      wfor t1457;
      rt_for_start(&t1457, rt_get(&F_args, v745_e), 0);
      for (int64_t t1456 = 0; rt_for_guard(&t1457, rt_get(&F_args, v745_e), "e.args"), rt_for_more(&t1457, t1456); t1456++) {
        wv v765_a = rt_for_item(&t1457, t1456);
        RT_AT(4, 1250);
        (void)({ wv t1458 = v764_types; wv t1459 = rt_chk(s517_expr(up, v765_a, v746_scope, v747_ctx), "expr"); std_push(t1458, t1459); });
      }
    }
    RT_AT(4, 1251);
    RT_RET(rt_chk(s518_callee(up, v745_e, v746_scope, rt_b(1), v764_types), "expr"));
  }
  RT_AT(4, 1253);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S185)))) {
    RT_AT(4, 1254);
    (void)rt_chk(s475_error(up, rt_get(&F_pos, v745_e), rt_s(&S555), RT_NULL), "expr");
    RT_AT(4, 1255);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1257);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S45)))) {
    RT_AT(4, 1257);
    RT_RET(rt_chk(s504_checkMatch(up, v745_e, v746_scope, v747_ctx, rt_b(0)), "expr"));
  }
  RT_AT(4, 1258);
  if (rt_cond(rt_eq(v748_kind, rt_s(&S186)))) {
    RT_AT(4, 1258);
    RT_RET(rt_chk(s513_chain(up, v745_e, v746_scope, v747_ctx, rt_b(0)), "expr"));
  }
  RT_AT(4, 1259);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S556), rt_text(v748_kind) }));
  RT_RET(RT_NULL);
}

static wv s518_callee(struct fr119 *up, wv v766_e, wv v767_scope, wv v768_used, wv v769_types) {
  rt_enter("callee");
  v766_e = rt_bind(v766_e, 0);
  v767_scope = rt_bind(v767_scope, 0);
  v768_used = rt_bind(v768_used, 0);
  v769_types = rt_bind(v769_types, 0);
  RT_AT(4, 1266);
  wv v770_c = rt_bind(rt_get(&F_callee, v766_e), 0);
  RT_AT(4, 1267);
  wv v771_n = rt_bind(std_length(rt_get(&F_args, v766_e)), 0);
  RT_AT(4, 1268);
  if (rt_cond(rt_eq(rt_get(&F_k, v770_c), rt_s(&S157)))) {
    RT_AT(4, 1269);
    wv v775_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v770_c)), "callee"), 0);
    RT_AT(4, 1270);
    if (rt_cond(rt_eq(v775_sig, RT_NULL))) {
      RT_AT(4, 1270);
      (void)rt_chk(s516_unknownStd(up, v770_c), "callee");
      RT_AT(4, 1270);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1271);
    if (rt_cond(rt_eq(v775_sig, rt_s(&S35)))) {
      RT_AT(4, 1271);
      (void)({ wv t1460 = rt_get(&F_pos, v770_c); wv t1461 = rt_cat(3, (wv[]){ rt_s(&S181), rt_text(rt_get(&F_name, v770_c)), rt_s(&S557) }); rt_chk(s475_error(up, t1460, t1461, RT_NULL), "callee"); });
      RT_AT(4, 1271);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1272);
    if (rt_cond(rt_ne(v775_sig, v771_n))) {
      RT_AT(4, 1273);
      (void)({ wv t1465 = rt_get(&F_pos, v770_c); wv t1466 = ({ wv t1462 = rt_text(rt_get(&F_name, v770_c)); wv t1463 = rt_text(rt_chk(w5_plural(v775_sig), "callee")); wv t1464 = rt_text(v771_n); rt_cat(6, (wv[]){ rt_s(&S181), t1462, rt_s(&S558), t1463, rt_s(&S559), t1464 }); }); rt_chk(s475_error(up, t1465, t1466, RT_NULL), "callee"); });
      RT_AT(4, 1274);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1276);
    wv v776_wrong = rt_b(0);
    RT_AT(4, 1277);
    {
      wfor t1468;
      rt_for_start(&t1468, v769_types, 0);
      for (int64_t t1467 = 0; rt_for_guard(&t1468, v769_types, "types"), rt_for_more(&t1468, t1467); t1467++) {
        wv v777_i = rt_for_key(&t1468, t1467);
        wv v778_ty = rt_for_val(&t1468, t1467);
        RT_AT(4, 1278);
        wv v779_ok = rt_bind(({ wv t1469 = rt_get(&F_name, v770_c); wv t1470 = v777_i; rt_chk(w5_stdAccepted(t1469, t1470), "callee"); }), 0);
        RT_AT(4, 1279);
        if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v779_ok, RT_NULL), "&&") && rt_bool(rt_chk(w5_known(v778_ty), "callee"), "&&")), "&&") && rt_bool(rt_not(({ wv t1471 = v779_ok; wv t1472 = rt_get(&F_t, v778_ty); rt_chk(w1_contains(t1471, t1472), "callee"); })), "&&")))) {
          RT_AT(4, 1280);
          (void)({ wv t1478 = rt_get(&F_pos, ({ wv t1473 = v777_i; wv t1474 = rt_get(&F_args, v766_e); rt_idx(t1473, t1474); })); wv t1479 = ({ wv t1475 = rt_text(rt_get(&F_name, v770_c)); wv t1476 = rt_text(rt_chk(w5_typesText(v779_ok), "callee")); wv t1477 = rt_text(rt_chk(w5_typeText(v778_ty), "callee")); rt_cat(6, (wv[]){ rt_s(&S106), t1475, rt_s(&S560), t1476, rt_s(&S559), t1477 }); }); rt_chk(s475_error(up, t1478, t1479, RT_NULL), "callee"); });
          RT_AT(4, 1281);
          v776_wrong = rt_b(1);
        }
      }
    }
    RT_AT(4, 1284);
    if (rt_cond(v776_wrong)) {
      RT_AT(4, 1284);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1285);
    if (rt_cond(({ wv t1480 = rt_arr(4, (wv[]){ rt_s(&S353), rt_s(&S354), rt_s(&S355), rt_s(&S378) }, 0); wv t1481 = rt_get(&F_name, v770_c); rt_chk(w1_contains(t1480, t1481), "callee"); }))) {
      RT_AT(4, 1285);
      (void)({ wv t1482 = rt_idx(rt_i(0), rt_get(&F_args, v766_e)); wv t1483 = rt_idx(rt_i(0), v769_types); rt_chk(s479_noText(up, t1482, t1483), "callee"); });
    }
    RT_AT(4, 1286);
    RT_RET(({ wv t1484 = rt_get(&F_name, v770_c); wv t1485 = v769_types; rt_chk(w5_stdResult(t1484, t1485), "callee"); }));
  }
  RT_AT(4, 1288);
  wv v772_d = RT_NULL;
  RT_AT(4, 1289);
  wv v773_label = rt_s(&S13);
  RT_AT(4, 1290);
  if (rt_cond(rt_eq(rt_get(&F_k, v770_c), rt_s(&S192)))) {
    RT_AT(4, 1292);
    v773_label = ({ wv t1486 = rt_text(rt_get(&F_name, rt_get(&F_rec, v770_c))); wv t1487 = rt_text(rt_get(&F_field, v770_c)); rt_cat(3, (wv[]){ t1486, rt_s(&S130), t1487 }); });
    RT_AT(4, 1293);
    wv v780_m = rt_bind(({ wv t1488 = rt_get(&F_rec, v770_c); wv t1489 = v767_scope; rt_chk(s505_moduleNamed(up, t1488, t1489), "callee"); }), 0);
    RT_AT(4, 1294);
    if (rt_cond(rt_eq(v780_m, RT_NULL))) {
      RT_AT(4, 1294);
      (void)({ wv t1490 = rt_get(&F_rec, v770_c); wv t1491 = v767_scope; rt_chk(s509_unknownModule(up, t1490, t1491), "callee"); });
      RT_AT(4, 1294);
      RT_RET(RT_NULL);
    }
    RT_AT(4, 1295);
    v772_d = rt_chk(s508_exportOf(up, v770_c, v780_m), "callee");
    RT_AT(4, 1296);
    if (rt_cond(rt_eq(v772_d, RT_NULL))) {
      RT_AT(4, 1296);
      RT_RET(RT_NULL);
    }
  } else {
    RT_AT(4, 1298);
    v773_label = rt_bind(rt_get(&F_name, v770_c), 1);
    RT_AT(4, 1299);
    v772_d = ({ wv t1492 = v767_scope; wv t1493 = rt_get(&F_name, v770_c); rt_chk(s494_lookup(up, t1492, t1493), "callee"); });
    RT_AT(4, 1300);
    if (rt_cond(rt_eq(v772_d, RT_NULL))) {
      RT_AT(4, 1300);
      (void)({ wv t1494 = rt_get(&F_pos, v770_c); wv t1495 = rt_cat(3, (wv[]){ rt_s(&S561), rt_text(rt_get(&F_name, v770_c)), rt_s(&S114) }); rt_chk(s475_error(up, t1494, t1495, RT_NULL), "callee"); });
      RT_AT(4, 1300);
      RT_RET(RT_NULL);
    }
  }
  RT_AT(4, 1302);
  if (rt_cond(rt_ne(rt_get(&F_kind, v772_d), rt_s(&S33)))) {
    RT_AT(4, 1303);
    wv v781_hint = rt_bind((rt_cond(rt_eq(v771_n, rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S98), rt_text(v773_label), rt_s(&S562) }) : rt_s(&S563)), 0);
    RT_AT(4, 1305);
    (void)({ wv t1496 = rt_get(&F_pos, v770_c); wv t1497 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v773_label), rt_s(&S564) }); wv t1498 = v781_hint; rt_chk(s475_error(up, t1496, t1497, t1498), "callee"); });
    RT_AT(4, 1306);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1308);
  if (rt_cond(({ wv t1499 = rt_get(&F_arity, v772_d); wv t1500 = v771_n; rt_ne(t1499, t1500); }))) {
    RT_AT(4, 1308);
    (void)({ wv t1504 = rt_get(&F_pos, v770_c); wv t1505 = ({ wv t1501 = rt_text(v773_label); wv t1502 = rt_text(rt_chk(w5_plural(rt_get(&F_arity, v772_d)), "callee")); wv t1503 = rt_text(v771_n); rt_cat(6, (wv[]){ rt_s(&S114), t1501, rt_s(&S558), t1502, rt_s(&S559), t1503 }); }); rt_chk(s475_error(up, t1504, t1505, RT_NULL), "callee"); });
  }
  RT_AT(4, 1310);
  if (rt_cond(rt_b(rt_bool(v768_used, "&&") && rt_bool(rt_eq(({ wv t1506 = rt_get(&F_mod, v772_d); wv t1507 = rt_get(&F_namePos, v772_d); rt_chk(s488_firstUse(up, t1506, t1507), "callee"); }), RT_NULL), "&&")))) {
    RT_AT(4, 1311);
    (void)({ wv t1512 = rt_def(up->v468_uses, "uses"); wv t1513 = ({ wv t1508 = rt_get(&F_mod, v772_d); wv t1509 = rt_get(&F_namePos, v772_d); wv t1510 = rt_get(&F_pos, v770_c); wv t1511 = rt_def(up->v466_curMod, "curMod"); rt_rec(0, 4, K53, (wv[]){ t1508, t1509, t1510, t1511 }); }); std_push(t1512, t1513); });
  }
  RT_AT(4, 1315);
  wv v774_key = ({ wv t1514 = rt_text(rt_get(&F_mod, v772_d)); wv t1515 = rt_text(rt_get(&F_namePos, v772_d)); rt_cat(3, (wv[]){ t1514, rt_s(&S28), t1515 }); });
  RT_AT(4, 1316);
  if (rt_cond(({ wv t1516 = rt_def(up->v471_fnTypes, "fnTypes"); wv t1517 = v774_key; std_has(t1516, t1517); }))) {
    RT_AT(4, 1316);
    RT_RET(({ wv t1518 = v774_key; wv t1519 = rt_def(up->v471_fnTypes, "fnTypes"); rt_idx(t1518, t1519); }));
  }
  RT_RET((RT_AT(4, 1317), RT_NULL));
}

static wv s519_importNames(struct fr119 *up, wv v782_m, wv v783_scope) {
  rt_enter("importNames");
  v782_m = rt_bind(v782_m, 0);
  v783_scope = rt_bind(v783_scope, 0);
  RT_AT(4, 1323);
  wv v784_imports = rt_bind(rt_get(&F_imports, rt_idx(v782_m, up->v462_modules)), 0);
  RT_AT(4, 1324);
  wv v785_seen = rt_arr(0, NULL, 1);
  RT_AT(4, 1325);
  wv v786_k = rt_i(0);
  RT_AT(4, 1326);
  while (rt_cond((RT_AT(4, 1326), ({ wv t1520 = v786_k; wv t1521 = std_length(v784_imports); rt_lt(t1520, t1521); })))) {
    RT_AT(4, 1327);
    wv v787_node = rt_bind(rt_get(&F_node, rt_idx(v786_k, v784_imports)), 0);
    RT_AT(4, 1328);
    wv v788_lib = rt_bind(rt_get(&F_lib, rt_idx(v786_k, v784_imports)), 0);
    RT_AT(4, 1329);
    v786_k = rt_add(v786_k, rt_i(1));
    RT_AT(4, 1330);
    wv v789_path = rt_bind(rt_get(&F_path, rt_idx(v788_lib, up->v462_modules)), 0);
    RT_AT(4, 1331);
    wv v790_usable = rt_bind(v788_lib, 1);
    RT_AT(4, 1332);
    if (rt_cond(rt_chk(w1_contains(v785_seen, v788_lib), "importNames"))) {
      RT_AT(4, 1333);
      (void)({ wv t1522 = rt_get(&F_pathPos, v787_node); wv t1523 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v789_path), rt_s(&S565) }); rt_chk(s475_error(up, t1522, t1523, RT_NULL), "importNames"); });
      RT_AT(4, 1334);
      v790_usable = RT_NULL;
    } else if (rt_cond(rt_get(&F_notLibrary, rt_idx(v788_lib, up->v462_modules)))) {
      RT_AT(4, 1336);
      (void)({ wv t1524 = rt_get(&F_pathPos, v787_node); wv t1525 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v789_path), rt_s(&S566) }); rt_chk(s475_error(up, t1524, t1525, RT_NULL), "importNames"); });
      RT_AT(4, 1337);
      v790_usable = RT_NULL;
    }
    RT_AT(4, 1339);
    (void)std_push(v785_seen, v788_lib);
    RT_AT(4, 1340);
    wv v791_d = ({ wv t1526 = rt_get(&F_name, v787_node); wv t1527 = rt_get(&F_namePos, v787_node); wv t1528 = rt_def(up->v466_curMod, "curMod"); wv t1529 = v790_usable; rt_rec(0, 10, K47, (wv[]){ rt_s(&S507), t1526, rt_b(0), rt_b(0), t1527, RT_NULL, RT_NULL, RT_NULL, t1528, t1529 }); });
    RT_AT(4, 1344);
    (void)({ wv t1530 = v783_scope; wv t1531 = rt_get(&F_name, v787_node); wv t1532 = v791_d; rt_chk(s493_declare(up, t1530, t1531, t1532), "importNames"); });
  }
  RT_RET(RT_NULL);
}

static wv s520_libraryRules(struct fr119 *up, wv v792_m, wv v793_scope, wv v794_isEntry, wv v795_last) {
  rt_enter("libraryRules");
  v792_m = rt_bind(v792_m, 0);
  v793_scope = rt_bind(v793_scope, 0);
  v794_isEntry = rt_bind(v794_isEntry, 0);
  v795_last = rt_bind(v795_last, 0);
  RT_AT(4, 1350);
  wv v796_program = rt_bind(rt_get(&F_program, rt_idx(v792_m, up->v462_modules)), 0);
  RT_AT(4, 1351);
  wv v797_k = rt_i(0);
  RT_AT(4, 1352);
  while (rt_cond((RT_AT(4, 1352), ({ wv t1533 = v797_k; wv t1534 = std_length(rt_get(&F_stmts, v796_program)); rt_lt(t1533, t1534); })))) {
    RT_AT(4, 1353);
    wv v799_s = rt_bind(({ wv t1535 = v797_k; wv t1536 = rt_get(&F_stmts, v796_program); rt_idx(t1535, t1536); }), 0);
    RT_AT(4, 1354);
    v797_k = rt_add(v797_k, rt_i(1));
    RT_AT(4, 1356);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v799_s), rt_s(&S206)), "&&") && rt_bool(rt_b(rt_bool(v794_isEntry, "||") || rt_bool(({ wv t1537 = rt_get(&F_pos, v799_s); wv t1538 = rt_get(&F_pos, v795_last); rt_ne(t1537, t1538); }), "||")), "&&")))) {
      RT_AT(4, 1357);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v799_s), rt_s(&S567), rt_s(&S568)), "libraryRules");
    } else if (rt_cond(rt_b(rt_bool(rt_not(v794_isEntry), "&&") && rt_bool(rt_not(({ wv t1539 = rt_def(w5_libraryTop, "libraryTop"); wv t1540 = rt_get(&F_k, v799_s); rt_chk(w1_contains(t1539, t1540), "libraryRules"); })), "&&")))) {
      RT_AT(4, 1359);
      (void)rt_chk(s475_error(up, rt_get(&F_pos, v799_s), rt_s(&S569), rt_s(&S570)), "libraryRules");
    }
  }
  RT_AT(4, 1363);
  if (rt_cond(v794_isEntry)) {
    RT_AT(4, 1363);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1364);
  if (rt_cond(rt_b(rt_bool(rt_eq(v795_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v795_last), rt_s(&S206)), "||")))) {
    RT_AT(4, 1365);
    (void)rt_chk(s475_error(up, rt_get(&F_end, v796_program), rt_s(&S571), rt_s(&S572)), "libraryRules");
    RT_AT(4, 1366);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1368);
  wv v798_exports = rt_arr(0, NULL, 1);
  RT_AT(4, 1369);
  v797_k = rt_i(0);
  RT_AT(4, 1370);
  while (rt_cond((RT_AT(4, 1370), ({ wv t1541 = v797_k; wv t1542 = std_length(rt_get(&F_names, v795_last)); rt_lt(t1541, t1542); })))) {
    RT_AT(4, 1371);
    wv v800_n = rt_bind(({ wv t1543 = v797_k; wv t1544 = rt_get(&F_names, v795_last); rt_idx(t1543, t1544); }), 0);
    RT_AT(4, 1372);
    v797_k = rt_add(v797_k, rt_i(1));
    RT_AT(4, 1373);
    wv v801_d = rt_bind(({ wv t1545 = v793_scope; wv t1546 = rt_get(&F_name, v800_n); rt_chk(s492_own(up, t1545, t1546), "libraryRules"); }), 0);
    RT_AT(4, 1374);
    if (rt_cond(rt_eq(v801_d, RT_NULL))) {
      RT_AT(4, 1375);
      (void)({ wv t1547 = rt_get(&F_pos, v800_n); wv t1548 = rt_cat(3, (wv[]){ rt_s(&S573), rt_text(rt_get(&F_name, v800_n)), rt_s(&S574) }); rt_chk(s475_error(up, t1547, t1548, RT_NULL), "libraryRules"); });
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v801_d), rt_s(&S507)))) {
      RT_AT(4, 1377);
      (void)({ wv t1549 = rt_get(&F_pos, v800_n); wv t1550 = rt_cat(3, (wv[]){ rt_s(&S573), rt_text(rt_get(&F_name, v800_n)), rt_s(&S575) }); rt_chk(s475_error(up, t1549, t1550, RT_NULL), "libraryRules"); });
    } else {
      RT_AT(4, 1379);
      (void)std_push(v798_exports, v801_d);
    }
  }
  RT_AT(4, 1382);
  ({ wv t1551 = v798_exports; wv t1552 = rt_idx(v792_m, up->v462_modules); rt_set_mem(t1551, &F_exports, t1552); });
  RT_RET(RT_NULL);
}

static wv s521_checkModule(struct fr119 *up, wv v802_m, wv v803_isEntry) {
  rt_enter("checkModule");
  v802_m = rt_bind(v802_m, 0);
  v803_isEntry = rt_bind(v803_isEntry, 0);
  RT_AT(4, 1386);
  (void)rt_chk(s486_enter(up, v802_m), "checkModule");
  RT_AT(4, 1387);
  wv v804_program = rt_bind(rt_get(&F_program, rt_idx(v802_m, up->v462_modules)), 0);
  RT_AT(4, 1388);
  wv v805_last = RT_NULL;
  RT_AT(4, 1389);
  wv v806_k = rt_i(0);
  RT_AT(4, 1390);
  while (rt_cond((RT_AT(4, 1390), ({ wv t1553 = v806_k; wv t1554 = std_length(rt_get(&F_stmts, v804_program)); rt_lt(t1553, t1554); })))) {
    RT_AT(4, 1391);
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1555 = v806_k; wv t1556 = rt_get(&F_stmts, v804_program); rt_idx(t1555, t1556); })), rt_s(&S203)))) {
      RT_AT(4, 1391);
      v805_last = rt_bind(({ wv t1557 = v806_k; wv t1558 = rt_get(&F_stmts, v804_program); rt_idx(t1557, t1558); }), 1);
    }
    RT_AT(4, 1392);
    v806_k = rt_add(v806_k, rt_i(1));
  }
  RT_AT(4, 1395);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v803_isEntry), "&&") && rt_bool(rt_ne(v805_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v805_last), rt_s(&S55)), "&&")))) {
    RT_AT(4, 1396);
    rt_set_mem(rt_b(1), &F_notLibrary, rt_idx(v802_m, up->v462_modules));
    RT_AT(4, 1397);
    RT_RET(RT_NULL);
  }
  RT_AT(4, 1399);
  wv v807_found = rt_map(0, NULL, 1);
  RT_AT(4, 1400);
  (void)({ wv t1559 = rt_get(&F_stmts, v804_program); wv t1560 = v807_found; rt_chk(w5_rebinds(t1559, t1560), "checkModule"); });
  RT_AT(4, 1401);
  wv t1561 = rt_bind(v807_found, 1);
  (void)rt_def(up->v473_rebound, "rebound");
  up->v473_rebound = t1561;
  RT_AT(4, 1402);
  wv v808_scope = rt_bind(rt_chk(s490_newScope(up, RT_NULL), "checkModule"), 0);
  RT_AT(4, 1403);
  (void)rt_chk(s519_importNames(up, v802_m, v808_scope), "checkModule");
  RT_AT(4, 1404);
  (void)({ wv t1564 = rt_rec(0, 2, K54, (wv[]){ rt_get(&F_stmts, v804_program), RT_NULL }); wv t1565 = v808_scope; wv t1566 = ({ wv t1562 = rt_def(up->v522_topFn, "topFn"); wv t1563 = rt_arr(0, NULL, 0); rt_rec(0, 3, K51, (wv[]){ t1562, rt_i(0), t1563 }); }); rt_chk(s496_block(up, t1564, t1565, t1566), "checkModule"); });
  RT_AT(4, 1406);
  wv v809_asLibrary = rt_b(rt_bool(rt_b(rt_bool(rt_not(up->v463_requireExit), "&&") && rt_bool(rt_ne(v805_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v805_last), rt_s(&S206)), "&&"));
  RT_AT(4, 1407);
  (void)({ wv t1567 = v802_m; wv t1568 = v808_scope; wv t1569 = rt_b(rt_bool(v803_isEntry, "&&") && rt_bool(rt_not(v809_asLibrary), "&&")); wv t1570 = v805_last; rt_chk(s520_libraryRules(up, t1567, t1568, t1569, t1570), "checkModule"); });
  RT_RET(RT_NULL);
}

static wv w6_binFn(wv v814_op) {
  rt_enter("binFn");
  v814_op = rt_bind(v814_op, 0);
  RT_AT(5, 28);
  wv v815_k = rt_i(0);
  RT_AT(5, 29);
  while (rt_cond((RT_AT(5, 29), ({ wv t1600 = ({ wv t1598 = v815_k; wv t1599 = rt_def(w6_binOps, "binOps"); rt_idx(t1598, t1599); }); wv t1601 = v814_op; rt_ne(t1600, t1601); })))) {
    RT_AT(5, 29);
    v815_k = rt_add(v815_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 30), ({ wv t1602 = v815_k; wv t1603 = rt_def(w6_binFns, "binFns"); rt_idx(t1602, t1603); })));
}

static wv w6_modeOf(wv v816_x) {
  rt_enter("modeOf");
  v816_x = rt_bind(v816_x, 0);
  RT_RET((RT_AT(5, 33), (rt_cond(rt_get(&F_isVar, v816_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v816_x)) ? rt_s(&S37) : rt_s(&S35)))));
}

static wv w6_unFn(wv v817_op) {
  rt_enter("unFn");
  v817_op = rt_bind(v817_op, 0);
  RT_RET((RT_AT(5, 35), (rt_cond(rt_eq(v817_op, rt_s(&S86))) ? rt_s(&S599) : (rt_cond(rt_eq(v817_op, rt_s(&S95))) ? rt_s(&S600) : rt_s(&S601)))));
}

static wv w6_repeat(wv v818_s, wv v819_count) {
  rt_enter("repeat");
  v818_s = rt_bind(v818_s, 0);
  v819_count = rt_bind(v819_count, 0);
  RT_AT(5, 38);
  wv v820_out = rt_s(&S13);
  RT_AT(5, 39);
  wv v821_k = rt_i(0);
  RT_AT(5, 40);
  while (rt_cond((RT_AT(5, 40), rt_lt(v821_k, v819_count)))) {
    RT_AT(5, 41);
    v820_out = rt_add(v820_out, v818_s);
    RT_AT(5, 42);
    v821_k = rt_add(v821_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 44), v820_out));
}

static wv w6_utf8Length(wv v822_s) {
  rt_enter("utf8Length");
  v822_s = rt_bind(v822_s, 0);
  RT_AT(5, 49);
  wv v823_cs = rt_bind(std_split(v822_s, rt_s(&S13)), 0);
  RT_AT(5, 50);
  wv v824_n = rt_i(0);
  RT_AT(5, 51);
  wv v825_k = rt_i(0);
  RT_AT(5, 52);
  while (rt_cond((RT_AT(5, 52), ({ wv t1604 = v825_k; wv t1605 = std_length(v823_cs); rt_lt(t1604, t1605); })))) {
    RT_AT(5, 53);
    wv v826_c = rt_bind(std_code(rt_idx(v825_k, v823_cs)), 0);
    RT_AT(5, 54);
    wv t1606 = (rt_cond(rt_lt(v826_c, rt_i(128))) ? rt_i(1) : (rt_cond(rt_lt(v826_c, rt_i(2048))) ? rt_i(2) : (rt_cond(rt_lt(v826_c, rt_i(65536))) ? rt_i(3) : rt_i(4))));
    v824_n = rt_add(v824_n, t1606);
    RT_AT(5, 55);
    v825_k = rt_add(v825_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 57), v824_n));
}

static wv w6_cString(wv v827_s) {
  rt_enter("cString");
  v827_s = rt_bind(v827_s, 0);
  RT_AT(5, 63);
  wv v828_cs = rt_bind(std_split(v827_s, rt_s(&S13)), 0);
  RT_AT(5, 64);
  wv v829_out = rt_s(&S18);
  RT_AT(5, 65);
  wv v830_k = rt_i(0);
  RT_AT(5, 66);
  while (rt_cond((RT_AT(5, 66), ({ wv t1607 = v830_k; wv t1608 = std_length(v828_cs); rt_lt(t1607, t1608); })))) {
    RT_AT(5, 67);
    wv v831_ch = rt_bind(rt_idx(v830_k, v828_cs), 0);
    RT_AT(5, 68);
    wv v832_c = rt_bind(std_code(v831_ch), 0);
    RT_AT(5, 69);
    if (rt_cond(rt_eq(v831_ch, rt_s(&S18)))) {
      RT_AT(5, 70);
      v829_out = rt_add(v829_out, rt_s(&S602));
    } else if (rt_cond(rt_eq(v831_ch, rt_s(&S19)))) {
      RT_AT(5, 72);
      v829_out = rt_add(v829_out, rt_s(&S603));
    } else if (rt_cond(rt_eq(v831_ch, rt_s(&S2)))) {
      RT_AT(5, 74);
      v829_out = rt_add(v829_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v831_ch, rt_s(&S1)))) {
      RT_AT(5, 76);
      v829_out = rt_add(v829_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v831_ch, rt_s(&S3)))) {
      RT_AT(5, 78);
      v829_out = rt_add(v829_out, rt_s(&S21));
    } else if (rt_cond(rt_b(rt_bool(rt_lt(v832_c, rt_i(32)), "||") || rt_bool(rt_eq(v832_c, rt_i(127)), "||")))) {
      RT_AT(5, 80);
      v829_out = ({ wv t1612 = v829_out; wv t1613 = ({ wv t1609 = rt_text(std_floor(rt_div(v832_c, rt_i(64)))); wv t1610 = rt_text(rt_mod(std_floor(rt_div(v832_c, rt_i(8))), rt_i(8))); wv t1611 = rt_text(rt_mod(v832_c, rt_i(8))); rt_cat(4, (wv[]){ rt_s(&S19), t1609, t1610, t1611 }); }); rt_add(t1612, t1613); });
    } else {
      RT_AT(5, 82);
      v829_out = rt_add(v829_out, v831_ch);
    }
    RT_AT(5, 84);
    v830_k = rt_add(v830_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 86), rt_add(v829_out, rt_s(&S18))));
}

static wv w6_wstrInit(wv v833_s) {
  rt_enter("wstrInit");
  v833_s = rt_bind(v833_s, 0);
  RT_AT(5, 91);
  wv v834_bytes = rt_bind(rt_chk(w6_utf8Length(v833_s), "wstrInit"), 0);
  RT_AT(5, 92);
  wv v835_cps = rt_bind(std_length(v833_s), 0);
  RT_RET((RT_AT(5, 93), ({ wv t1614 = rt_text(v834_bytes); wv t1615 = rt_text(v835_cps); wv t1616 = rt_text((rt_cond(rt_eq(v834_bytes, v835_cps)) ? rt_i(1) : rt_i(0))); wv t1617 = rt_text(rt_chk(w6_cString(v833_s), "wstrInit")); rt_cat(9, (wv[]){ rt_s(&S604), t1614, rt_s(&S419), t1615, rt_s(&S419), t1616, rt_s(&S605), t1617, rt_s(&S420) }); })));
}

static wv w6_values(wv v836_cs) {
  rt_enter("values");
  RT_RET((RT_AT(5, 97), (rt_cond(rt_eq(std_length(v836_cs), rt_i(0))) ? rt_s(&S606) : rt_cat(3, (wv[]){ rt_s(&S607), rt_text(std_join(v836_cs, rt_s(&S419))), rt_s(&S420) }))));
}

static wv w6_generateC(wv p_v837_modules, wv p_v838_runtime) {
  rt_enter("generateC");
  struct fr176 fr = { 0 };
  fr.v837_modules = rt_bind(p_v837_modules, 0);
  fr.v838_runtime = rt_bind(p_v838_runtime, 0);
  RT_AT(5, 101);
  fr.v839_n = rt_i(0);
  RT_AT(5, 102);
  fr.v840_tmps = rt_i(0);
  RT_AT(5, 103);
  fr.v841_strings = rt_arr(0, NULL, 1);
  RT_AT(5, 104);
  fr.v842_fields = rt_arr(0, NULL, 1);
  RT_AT(5, 105);
  fr.v843_shapes = rt_arr(0, NULL, 1);
  RT_AT(5, 106);
  fr.v844_enumDefs = rt_arr(0, NULL, 1);
  RT_AT(5, 107);
  fr.v845_globals = rt_arr(0, NULL, 1);
  RT_AT(5, 108);
  fr.v846_frames = rt_arr(0, NULL, 1);
  RT_AT(5, 109);
  fr.v847_functions = rt_arr(0, NULL, 1);
  RT_AT(5, 110);
  fr.v848_out = rt_arr(0, NULL, 1);
  RT_AT(5, 111);
  fr.v849_indent = rt_i(1);
  RT_AT(5, 113);
  fr.v850_loops = rt_i(0);
  RT_AT(5, 114);
  fr.v851_iife = rt_b(0);
  RT_AT(5, 120);
  fr.v852_decls = rt_arr(0, NULL, 1);
  RT_AT(5, 121);
  fr.v853_fns = rt_arr(0, NULL, 1);
  RT_AT(5, 123);
  fr.v854_tops = rt_arr(0, NULL, 1);
  RT_AT(5, 124);
  fr.v855_slots = rt_arr(0, NULL, 1);
  RT_AT(5, 128);
  fr.v856_curMod = rt_i(0);
  RT_AT(5, 129);
  fr.v857_mod = rt_s(&S13);
  RT_AT(5, 130);
  fr.v858_curSlot = rt_arr(0, NULL, 1);
  RT_AT(5, 131);
  fr.v859_stolen = rt_arr(0, NULL, 1);
  RT_AT(5, 132);
  fr.v860_ownEnums = rt_arr(0, NULL, 1);
  RT_AT(5, 133);
  fr.v861_cur = rt_i(0);
  RT_AT(5, 135);
  fr.v862_lineStarts = rt_arr(1, (wv[]){ rt_i(0) }, 1);
  RT_AT(5, 1220);
  wv v927_mi = rt_i(0);
  RT_AT(5, 1221);
  while (rt_cond((RT_AT(5, 1221), ({ wv t2683 = v927_mi; wv t2684 = std_length(fr.v837_modules); rt_lt(t2683, t2684); })))) {
    RT_AT(5, 1222);
    (void)rt_chk(s879_analyze(&fr, v927_mi), "generateC");
    RT_AT(5, 1223);
    v927_mi = rt_add(v927_mi, rt_i(1));
  }
  RT_AT(5, 1225);
  v927_mi = rt_i(0);
  RT_AT(5, 1226);
  while (rt_cond((RT_AT(5, 1226), ({ wv t2685 = v927_mi; wv t2686 = std_length(fr.v837_modules); rt_lt(t2685, t2686); })))) {
    RT_AT(5, 1227);
    wv v1237_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v927_mi, fr.v837_modules))), 0);
    RT_AT(5, 1228);
    wv v1238_li = rt_i(0);
    RT_AT(5, 1229);
    while (rt_cond((RT_AT(5, 1229), ({ wv t2687 = v1238_li; wv t2688 = std_length(v1237_list); rt_lt(t2687, t2688); })))) {
      RT_AT(5, 1230);
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1238_li, v1237_list)), rt_s(&S47)))) {
        RT_AT(5, 1230);
        (void)({ wv t2689 = rt_idx(v1238_li, v1237_list); wv t2690 = rt_chk(s865_modId(&fr, v927_mi), "generateC"); rt_chk(s885_enumDef(&fr, t2689, t2690), "generateC"); });
      }
      RT_AT(5, 1231);
      v1238_li = rt_add(v1238_li, rt_i(1));
    }
    RT_AT(5, 1233);
    v927_mi = rt_add(v927_mi, rt_i(1));
  }
  RT_AT(5, 1235);
  v927_mi = rt_i(0);
  RT_AT(5, 1236);
  while (rt_cond((RT_AT(5, 1236), ({ wv t2691 = v927_mi; wv t2692 = std_length(fr.v837_modules); rt_lt(t2691, t2692); })))) {
    RT_AT(5, 1237);
    (void)rt_chk(s880_enter(&fr, v927_mi), "generateC");
    RT_AT(5, 1238);
    (void)rt_chk(s894_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v927_mi, fr.v837_modules)))), "generateC");
    RT_AT(5, 1239);
    v927_mi = rt_add(v927_mi, rt_i(1));
  }
  RT_RET((RT_AT(5, 1241), rt_chk(s926_assemble(&fr), "generateC")));
}

static wv s863_atLine(struct fr176 *up, wv v928_pos) {
  rt_enter("atLine");
  v928_pos = rt_bind(v928_pos, 0);
  RT_AT(5, 141);
  wv v929_lo = rt_i(0);
  RT_AT(5, 142);
  wv v930_hi = rt_sub(std_length(rt_def(up->v862_lineStarts, "lineStarts")), rt_i(1));
  RT_AT(5, 143);
  while (rt_cond((RT_AT(5, 143), rt_lt(v929_lo, v930_hi)))) {
    RT_AT(5, 144);
    wv v931_mid = rt_bind(std_floor(rt_div(rt_add(rt_add(v929_lo, v930_hi), rt_i(1)), rt_i(2))), 0);
    RT_AT(5, 145);
    if (rt_cond(({ wv t1620 = ({ wv t1618 = v931_mid; wv t1619 = rt_def(up->v862_lineStarts, "lineStarts"); rt_idx(t1618, t1619); }); wv t1621 = v928_pos; rt_le(t1620, t1621); }))) {
      RT_AT(5, 145);
      v929_lo = rt_bind(v931_mid, 1);
    } else {
      RT_AT(5, 145);
      v930_hi = rt_sub(v931_mid, rt_i(1));
    }
  }
  RT_RET((RT_AT(5, 147), ({ wv t1622 = rt_text(rt_def(up->v856_curMod, "curMod")); wv t1623 = rt_text(rt_add(v929_lo, rt_i(1))); rt_cat(5, (wv[]){ rt_s(&S608), t1622, rt_s(&S419), t1623, rt_s(&S16) }); })));
}

static wv s864_atExpr(struct fr176 *up, wv v932_pos, wv v933_c) {
  rt_enter("atExpr");
  v932_pos = rt_bind(v932_pos, 0);
  v933_c = rt_bind(v933_c, 0);
  RT_RET((RT_AT(5, 150), ({ wv t1624 = rt_text(rt_chk(s863_atLine(up, v932_pos), "atExpr")); wv t1625 = rt_text(v933_c); rt_cat(5, (wv[]){ rt_s(&S98), t1624, rt_s(&S419), t1625, rt_s(&S16) }); })));
}

static wv s865_modId(struct fr176 *up, wv v934_m) {
  rt_enter("modId");
  v934_m = rt_bind(v934_m, 0);
  RT_RET((RT_AT(5, 153), (rt_cond(({ wv t1626 = v934_m; wv t1627 = rt_sub(std_length(up->v837_modules), rt_i(1)); rt_eq(t1626, t1627); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v934_m, rt_i(1))) }))));
}

static wv s866_hasFrame(struct fr176 *up, wv v935_f) {
  rt_enter("hasFrame");
  v935_f = rt_bind(v935_f, 0);
  RT_RET((RT_AT(5, 155), rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1628 = v935_f; wv t1629 = rt_def(up->v853_fns, "fns"); rt_idx(t1628, t1629); }))), "&&") && rt_bool(rt_gt(rt_get(&F_children, ({ wv t1630 = v935_f; wv t1631 = rt_def(up->v853_fns, "fns"); rt_idx(t1630, t1631); })), rt_i(0)), "&&"))));
}

static wv s867_newScope(struct fr176 *up, wv v936_parent) {
  rt_enter("newScope");
  v936_parent = rt_bind(v936_parent, 0);
  RT_AT(5, 160);
  wv v937_names = rt_arr(0, NULL, 1);
  RT_RET((RT_AT(5, 161), rt_rec(0, 2, K46, (wv[]){ v936_parent, v937_names })));
}

static wv s868_put(struct fr176 *up, wv v938_scope, wv v939_name, wv v940_kind, wv v941_d) {
  rt_enter("put");
  v938_scope = rt_bind(v938_scope, 0);
  v939_name = rt_bind(v939_name, 0);
  v940_kind = rt_bind(v940_kind, 0);
  v941_d = rt_bind(v941_d, 0);
  RT_AT(5, 163);
  (void)({ wv t1632 = rt_get(&F_names, v938_scope); wv t1633 = rt_rec(0, 3, K55, (wv[]){ v939_name, v940_kind, v941_d }); std_push(t1632, t1633); });
  RT_RET(RT_NULL);
}

static wv s869_find(struct fr176 *up, wv v942_scope, wv v943_name) {
  rt_enter("find");
  v943_name = rt_bind(v943_name, 0);
  RT_AT(5, 166);
  wv v944_s = v942_scope;
  RT_AT(5, 167);
  while (rt_cond((RT_AT(5, 167), rt_ne(v944_s, RT_NULL)))) {
    RT_AT(5, 168);
    wv v945_ns = rt_get(&F_names, v944_s);
    RT_AT(5, 169);
    wv v946_k = rt_i(0);
    RT_AT(5, 170);
    while (rt_cond((RT_AT(5, 170), ({ wv t1634 = v946_k; wv t1635 = std_length(v945_ns); rt_lt(t1634, t1635); })))) {
      RT_AT(5, 171);
      if (rt_cond(({ wv t1636 = rt_get(&F_name, rt_idx(v946_k, v945_ns)); wv t1637 = v943_name; rt_eq(t1636, t1637); }))) {
        RT_AT(5, 171);
        RT_RET(rt_idx(v946_k, v945_ns));
      }
      RT_AT(5, 172);
      v946_k = rt_add(v946_k, rt_i(1));
    }
    RT_AT(5, 174);
    v944_s = rt_get(&F_parent, v944_s);
  }
  RT_RET((RT_AT(5, 176), RT_NULL));
}

static wv s870_kindOf(struct fr176 *up, wv v947_scope, wv v948_name) {
  rt_enter("kindOf");
  v947_scope = rt_bind(v947_scope, 0);
  v948_name = rt_bind(v948_name, 0);
  RT_AT(5, 179);
  wv v949_entry = rt_bind(rt_chk(s869_find(up, v947_scope, v948_name), "kindOf"), 0);
  RT_RET((RT_AT(5, 180), (rt_cond(rt_eq(v949_entry, RT_NULL)) ? RT_NULL : rt_get(&F_kind, v949_entry))));
}

static wv s871_newFn(struct fr176 *up, wv v950_name, wv v951_parent) {
  rt_enter("newFn");
  v950_name = rt_bind(v950_name, 0);
  v951_parent = rt_bind(v951_parent, 0);
  RT_AT(5, 184);
  wv v952_f = ({ wv t1640 = v950_name; wv t1641 = v951_parent; wv t1642 = (rt_cond(rt_eq(v951_parent, RT_NULL)) ? rt_i(0) : rt_add(rt_get(&F_depth, ({ wv t1638 = v951_parent; wv t1639 = rt_def(up->v853_fns, "fns"); rt_idx(t1638, t1639); })), rt_i(1))); wv t1643 = rt_eq(v951_parent, RT_NULL); rt_rec(1, 6, K56, (wv[]){ t1640, t1641, t1642, t1643, rt_i(0), rt_s(&S13) }); });
  RT_AT(5, 188);
  (void)({ wv t1644 = rt_def(up->v853_fns, "fns"); wv t1645 = v952_f; std_push(t1644, t1645); });
  RT_RET((RT_AT(5, 189), rt_sub(std_length(rt_def(up->v853_fns, "fns")), rt_i(1))));
}

static wv s872_bind(struct fr176 *up, wv v953_kind, wv v954_name, wv v955_f, wv v956_atTop, wv v957_mode) {
  rt_enter("bind");
  v953_kind = rt_bind(v953_kind, 0);
  v954_name = rt_bind(v954_name, 0);
  v955_f = rt_bind(v955_f, 0);
  v956_atTop = rt_bind(v956_atTop, 0);
  v957_mode = rt_bind(v957_mode, 0);
  RT_AT(5, 195);
  wv v958_cname = ({ wv t1646 = rt_text(rt_def(up->v857_mod, "mod")); wv t1647 = rt_text(v954_name); rt_cat(4, (wv[]){ rt_s(&S609), t1646, rt_s(&S12), t1647 }); });
  RT_AT(5, 196);
  if (rt_cond(rt_not(v956_atTop))) {
    RT_AT(5, 197);
    v958_cname = ({ wv t1650 = (rt_cond(rt_eq(v953_kind, rt_s(&S33))) ? rt_s(&S328) : rt_s(&S610)); wv t1651 = ({ wv t1648 = rt_text(rt_def(up->v839_n, "n")); wv t1649 = rt_text(v954_name); rt_cat(3, (wv[]){ t1648, rt_s(&S12), t1649 }); }); rt_add(t1650, t1651); });
    RT_AT(5, 198);
    up->v839_n = rt_add(rt_def(up->v839_n, "n"), rt_i(1));
  }
  RT_AT(5, 200);
  wv v959_d = ({ wv t1652 = v953_kind; wv t1653 = v954_name; wv t1654 = v955_f; wv t1655 = v958_cname; wv t1656 = rt_eq(v957_mode, rt_s(&S37)); rt_rec(1, 7, K57, (wv[]){ t1652, t1653, t1654, t1655, t1656, rt_b(0), RT_NULL }); });
  RT_AT(5, 201);
  (void)({ wv t1657 = rt_def(up->v852_decls, "decls"); wv t1658 = v959_d; std_push(t1657, t1658); });
  RT_AT(5, 202);
  if (rt_cond(rt_b(rt_bool(rt_get(&F_top, ({ wv t1659 = v955_f; wv t1660 = rt_def(up->v853_fns, "fns"); rt_idx(t1659, t1660); })), "&&") && rt_bool(rt_ne(v953_kind, rt_s(&S33)), "&&")))) {
    RT_AT(5, 202);
    (void)({ wv t1661 = rt_def(up->v845_globals, "globals"); wv t1662 = v958_cname; std_push(t1661, t1662); });
  }
  RT_RET((RT_AT(5, 203), rt_sub(std_length(rt_def(up->v852_decls, "decls")), rt_i(1))));
}

static wv s873_ablock(struct fr176 *up, wv v960_b, wv v961_scope, wv v962_f, wv v963_topScope) {
  rt_enter("ablock");
  v961_scope = rt_bind(v961_scope, 0);
  v962_f = rt_bind(v962_f, 0);
  v963_topScope = rt_bind(v963_topScope, 0);
  RT_AT(5, 207);
  wv v964_atTop = rt_eq(v961_scope, v963_topScope);
  RT_AT(5, 208);
  wv v965_list = rt_bind(rt_get(&F_stmts, v960_b), 0);
  RT_AT(5, 209);
  wv v966_k = rt_i(0);
  RT_AT(5, 210);
  while (rt_cond((RT_AT(5, 210), ({ wv t1663 = v966_k; wv t1664 = std_length(v965_list); rt_lt(t1663, t1664); })))) {
    RT_AT(5, 211);
    wv v967_s = rt_bind(rt_idx(v966_k, v965_list), 0);
    RT_AT(5, 212);
    if (rt_cond(rt_eq(rt_get(&F_k, v967_s), rt_s(&S33)))) {
      RT_AT(5, 213);
      wv v968_d = rt_bind(({ wv t1665 = rt_get(&F_name, v967_s); wv t1666 = v962_f; wv t1667 = v964_atTop; rt_chk(s872_bind(up, rt_s(&S33), t1665, t1666, t1667, rt_s(&S13)), "ablock"); }), 0);
      RT_AT(5, 214);
      wv v969_child = rt_bind(({ wv t1668 = rt_get(&F_name, v967_s); wv t1669 = v962_f; rt_chk(s871_newFn(up, t1668, t1669), "ablock"); }), 0);
      RT_AT(5, 215);
      ({ wv t1674 = rt_get(&F_cname, ({ wv t1670 = v968_d; wv t1671 = rt_def(up->v852_decls, "decls"); rt_idx(t1670, t1671); })); wv t1675 = ({ wv t1672 = v969_child; wv t1673 = rt_def(up->v853_fns, "fns"); rt_idx(t1672, t1673); }); rt_set_mem(t1674, &F_cname, t1675); });
      RT_AT(5, 216);
      ({ wv t1678 = v969_child; wv t1679 = ({ wv t1676 = v968_d; wv t1677 = rt_def(up->v852_decls, "decls"); rt_idx(t1676, t1677); }); rt_set_mem(t1678, &F_fnIndex, t1679); });
      RT_AT(5, 217);
      rt_upd_mem(rt_i(1), &F_children, ({ wv t1680 = v962_f; wv t1681 = rt_def(up->v853_fns, "fns"); rt_idx(t1680, t1681); }), rt_add);
      RT_AT(5, 218);
      ({ wv t1682 = v968_d; wv t1683 = rt_get(&F_namePos, v967_s); wv t1684 = rt_def(up->v858_curSlot, "curSlot"); rt_set_idx(t1682, t1683, t1684); });
      RT_AT(5, 219);
      (void)({ wv t1685 = v961_scope; wv t1686 = rt_get(&F_name, v967_s); wv t1687 = v968_d; rt_chk(s868_put(up, t1685, t1686, rt_s(&S33), t1687), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v967_s), rt_s(&S47)))) {
      RT_AT(5, 221);
      (void)({ wv t1688 = v961_scope; wv t1689 = rt_get(&F_name, v967_s); rt_chk(s868_put(up, t1688, t1689, rt_s(&S47), RT_NULL), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v967_s), rt_s(&S179)))) {
      RT_AT(5, 223);
      wv v970_d = rt_bind(({ wv t1690 = rt_get(&F_name, v967_s); wv t1691 = v962_f; wv t1692 = v964_atTop; wv t1693 = rt_chk(w6_modeOf(v967_s), "ablock"); rt_chk(s872_bind(up, rt_s(&S179), t1690, t1691, t1692, t1693), "ablock"); }), 0);
      RT_AT(5, 224);
      ({ wv t1694 = v970_d; wv t1695 = rt_get(&F_namePos, v967_s); wv t1696 = rt_def(up->v858_curSlot, "curSlot"); rt_set_idx(t1694, t1695, t1696); });
      RT_AT(5, 225);
      (void)({ wv t1697 = v961_scope; wv t1698 = rt_get(&F_name, v967_s); wv t1699 = v970_d; rt_chk(s868_put(up, t1697, t1698, rt_s(&S179), t1699), "ablock"); });
    }
    RT_AT(5, 227);
    v966_k = rt_add(v966_k, rt_i(1));
  }
  RT_AT(5, 229);
  v966_k = rt_i(0);
  RT_AT(5, 230);
  while (rt_cond((RT_AT(5, 230), ({ wv t1700 = v966_k; wv t1701 = std_length(v965_list); rt_lt(t1700, t1701); })))) {
    RT_AT(5, 231);
    (void)({ wv t1702 = rt_idx(v966_k, v965_list); wv t1703 = v961_scope; wv t1704 = v962_f; wv t1705 = v963_topScope; rt_chk(s874_astmt(up, t1702, t1703, t1704, t1705), "ablock"); });
    RT_AT(5, 232);
    v966_k = rt_add(v966_k, rt_i(1));
  }
  RT_AT(5, 234);
  if (rt_cond(rt_ne(rt_get(&F_value, v960_b), RT_NULL))) {
    RT_AT(5, 234);
    (void)({ wv t1706 = rt_get(&F_value, v960_b); wv t1707 = v961_scope; wv t1708 = v962_f; rt_chk(s878_aexpr(up, t1706, t1707, t1708), "ablock"); });
  }
  RT_RET(RT_NULL);
}

static wv s874_astmt(struct fr176 *up, wv v971_s, wv v972_scope, wv v973_f, wv v974_topScope) {
  rt_enter("astmt");
  v972_scope = rt_bind(v972_scope, 0);
  v973_f = rt_bind(v973_f, 0);
  v974_topScope = rt_bind(v974_topScope, 0);
  RT_AT(5, 238);
  wv v975_kind = rt_bind(rt_get(&F_k, v971_s), 0);
  RT_AT(5, 239);
  if (rt_cond(rt_eq(v975_kind, rt_s(&S33)))) {
    RT_AT(5, 240);
    wv v976_d = rt_bind(({ wv t1709 = rt_get(&F_namePos, v971_s); wv t1710 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t1709, t1710); }), 0);
    RT_AT(5, 241);
    wv v977_child = rt_bind(rt_get(&F_fnIndex, ({ wv t1711 = v976_d; wv t1712 = rt_def(up->v852_decls, "decls"); rt_idx(t1711, t1712); })), 0);
    RT_AT(5, 242);
    wv v978_ps = rt_bind(rt_chk(s867_newScope(up, v972_scope), "astmt"), 0);
    RT_AT(5, 243);
    wv v979_k = rt_i(0);
    RT_AT(5, 244);
    while (rt_cond((RT_AT(5, 244), ({ wv t1713 = v979_k; wv t1714 = std_length(rt_get(&F_params, v971_s)); rt_lt(t1713, t1714); })))) {
      RT_AT(5, 245);
      wv v980_p = rt_bind(({ wv t1715 = v979_k; wv t1716 = rt_get(&F_params, v971_s); rt_idx(t1715, t1716); }), 0);
      RT_AT(5, 246);
      wv v981_pd = rt_bind(({ wv t1717 = rt_get(&F_name, v980_p); wv t1718 = v977_child; wv t1719 = rt_chk(w6_modeOf(v980_p), "astmt"); rt_chk(s872_bind(up, rt_s(&S479), t1717, t1718, rt_b(0), t1719), "astmt"); }), 0);
      RT_AT(5, 247);
      ({ wv t1720 = v981_pd; wv t1721 = rt_get(&F_pos, v980_p); wv t1722 = rt_def(up->v858_curSlot, "curSlot"); rt_set_idx(t1720, t1721, t1722); });
      RT_AT(5, 248);
      (void)({ wv t1723 = v978_ps; wv t1724 = rt_get(&F_name, v980_p); wv t1725 = v981_pd; rt_chk(s868_put(up, t1723, t1724, rt_s(&S479), t1725), "astmt"); });
      RT_AT(5, 249);
      v979_k = rt_add(v979_k, rt_i(1));
    }
    RT_AT(5, 251);
    (void)({ wv t1726 = rt_get(&F_body, v971_s); wv t1727 = rt_chk(s867_newScope(up, v978_ps), "astmt"); wv t1728 = v977_child; wv t1729 = v974_topScope; rt_chk(s873_ablock(up, t1726, t1727, t1728, t1729), "astmt"); });
  } else if (rt_cond(rt_eq(v975_kind, rt_s(&S41)))) {
    RT_AT(5, 253);
    (void)({ wv t1730 = rt_get(&F_cond, v971_s); wv t1731 = v972_scope; wv t1732 = v973_f; rt_chk(s878_aexpr(up, t1730, t1731, t1732), "astmt"); });
    RT_AT(5, 254);
    (void)({ wv t1733 = rt_get(&F_body, v971_s); wv t1734 = rt_chk(s867_newScope(up, v972_scope), "astmt"); wv t1735 = v973_f; wv t1736 = v974_topScope; rt_chk(s873_ablock(up, t1733, t1734, t1735, t1736), "astmt"); });
  } else if (rt_cond(rt_eq(v975_kind, rt_s(&S42)))) {
    RT_AT(5, 256);
    if (rt_cond(rt_eq(rt_get(&F_coll, v971_s), RT_NULL))) {
      RT_AT(5, 257);
      (void)({ wv t1737 = rt_get(&F_from, v971_s); wv t1738 = v972_scope; wv t1739 = v973_f; rt_chk(s878_aexpr(up, t1737, t1738, t1739), "astmt"); });
      RT_AT(5, 258);
      (void)({ wv t1740 = rt_get(&F_to, v971_s); wv t1741 = v972_scope; wv t1742 = v973_f; rt_chk(s878_aexpr(up, t1740, t1741, t1742), "astmt"); });
      RT_AT(5, 259);
      if (rt_cond(rt_ne(rt_get(&F_step, v971_s), RT_NULL))) {
        RT_AT(5, 259);
        (void)({ wv t1743 = rt_get(&F_step, v971_s); wv t1744 = v972_scope; wv t1745 = v973_f; rt_chk(s878_aexpr(up, t1743, t1744, t1745), "astmt"); });
      }
    } else if (rt_cond(rt_not(({ wv t1746 = rt_get(&F_coll, v971_s); wv t1747 = v972_scope; rt_chk(s877_notValue(up, t1746, t1747), "astmt"); })))) {
      RT_AT(5, 261);
      (void)({ wv t1748 = rt_get(&F_coll, v971_s); wv t1749 = v972_scope; wv t1750 = v973_f; rt_chk(s878_aexpr(up, t1748, t1749, t1750), "astmt"); });
    }
    RT_AT(5, 264);
    wv v982_ls = rt_bind(rt_chk(s867_newScope(up, v972_scope), "astmt"), 0);
    RT_AT(5, 265);
    wv v983_k = rt_i(0);
    RT_AT(5, 266);
    while (rt_cond((RT_AT(5, 266), ({ wv t1751 = v983_k; wv t1752 = std_length(rt_get(&F_names, v971_s)); rt_lt(t1751, t1752); })))) {
      RT_AT(5, 267);
      wv v984_nm = rt_bind(({ wv t1753 = v983_k; wv t1754 = rt_get(&F_names, v971_s); rt_idx(t1753, t1754); }), 0);
      RT_AT(5, 268);
      wv v985_d = rt_bind(({ wv t1755 = rt_get(&F_name, v984_nm); wv t1756 = v973_f; rt_chk(s872_bind(up, rt_s(&S179), t1755, t1756, rt_b(0), rt_s(&S37)), "astmt"); }), 0);
      RT_AT(5, 269);
      ({ wv t1757 = v985_d; wv t1758 = rt_get(&F_pos, v984_nm); wv t1759 = rt_def(up->v858_curSlot, "curSlot"); rt_set_idx(t1757, t1758, t1759); });
      RT_AT(5, 270);
      (void)({ wv t1760 = v982_ls; wv t1761 = rt_get(&F_name, v984_nm); wv t1762 = v985_d; rt_chk(s868_put(up, t1760, t1761, rt_s(&S179), t1762), "astmt"); });
      RT_AT(5, 271);
      v983_k = rt_add(v983_k, rt_i(1));
    }
    RT_AT(5, 273);
    (void)({ wv t1763 = rt_get(&F_body, v971_s); wv t1764 = rt_chk(s867_newScope(up, v982_ls), "astmt"); wv t1765 = v973_f; wv t1766 = v974_topScope; rt_chk(s873_ablock(up, t1763, t1764, t1765, t1766), "astmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v975_kind, rt_s(&S34)), "||") || rt_bool(rt_eq(v975_kind, rt_s(&S58)), "||")), "||") || rt_bool(rt_eq(v975_kind, rt_s(&S55)), "||")), "||") || rt_bool(rt_eq(v975_kind, rt_s(&S179)), "||")))) {
    RT_AT(5, 275);
    if (rt_cond(rt_ne(rt_get(&F_e, v971_s), RT_NULL))) {
      RT_AT(5, 275);
      (void)({ wv t1767 = rt_get(&F_e, v971_s); wv t1768 = v972_scope; wv t1769 = v973_f; rt_chk(s878_aexpr(up, t1767, t1768, t1769), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v975_kind, rt_s(&S178)))) {
    RT_AT(5, 277);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v971_s)), rt_s(&S185)))) {
      RT_AT(5, 277);
      (void)({ wv t1770 = rt_get(&F_arg, rt_get(&F_e, v971_s)); wv t1771 = v972_scope; wv t1772 = v973_f; rt_chk(s878_aexpr(up, t1770, t1771, t1772), "astmt"); });
    } else {
      RT_AT(5, 277);
      (void)({ wv t1773 = rt_get(&F_e, v971_s); wv t1774 = v972_scope; wv t1775 = v973_f; rt_chk(s878_aexpr(up, t1773, t1774, t1775), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v975_kind, rt_s(&S38)))) {
    RT_AT(5, 279);
    (void)({ wv t1776 = rt_get(&F_e, v971_s); wv t1777 = v972_scope; wv t1778 = v973_f; rt_chk(s878_aexpr(up, t1776, t1777, t1778), "astmt"); });
    RT_AT(5, 280);
    (void)({ wv t1779 = rt_get(&F_target, v971_s); wv t1780 = v972_scope; wv t1781 = v973_f; rt_chk(s876_atarget(up, t1779, t1780, t1781), "astmt"); });
  }
  RT_AT(5, 282);
  if (rt_cond(rt_ne(rt_get(&F_handler, v971_s), RT_NULL))) {
    RT_AT(5, 283);
    wv v986_h = rt_bind(rt_get(&F_handler, v971_s), 0);
    RT_AT(5, 284);
    wv v987_hs = rt_bind(rt_chk(s867_newScope(up, v972_scope), "astmt"), 0);
    RT_AT(5, 285);
    if (rt_cond(rt_ne(rt_get(&F_param, v986_h), RT_NULL))) {
      RT_AT(5, 286);
      wv v988_wd = rt_bind(({ wv t1782 = rt_get(&F_param, v986_h); wv t1783 = v973_f; rt_chk(s872_bind(up, rt_s(&S57), t1782, t1783, rt_b(0), rt_s(&S13)), "astmt"); }), 0);
      RT_AT(5, 287);
      ({ wv t1784 = v988_wd; wv t1785 = rt_get(&F_paramPos, v986_h); wv t1786 = rt_def(up->v858_curSlot, "curSlot"); rt_set_idx(t1784, t1785, t1786); });
      RT_AT(5, 288);
      (void)({ wv t1787 = v987_hs; wv t1788 = rt_get(&F_param, v986_h); wv t1789 = v988_wd; rt_chk(s868_put(up, t1787, t1788, rt_s(&S57), t1789), "astmt"); });
    }
    RT_AT(5, 290);
    (void)({ wv t1790 = rt_get(&F_body, v986_h); wv t1791 = rt_chk(s867_newScope(up, v987_hs), "astmt"); wv t1792 = v973_f; wv t1793 = v974_topScope; rt_chk(s873_ablock(up, t1790, t1791, t1792, t1793), "astmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s875_use(struct fr176 *up, wv v989_x, wv v990_scope, wv v991_f) {
  rt_enter("use");
  v989_x = rt_bind(v989_x, 0);
  v990_scope = rt_bind(v990_scope, 0);
  v991_f = rt_bind(v991_f, 0);
  RT_AT(5, 296);
  wv v992_d = rt_bind(rt_get(&F_d, ({ wv t1794 = v990_scope; wv t1795 = rt_get(&F_name, v989_x); rt_chk(s869_find(up, t1794, t1795), "use"); })), 0);
  RT_AT(5, 297);
  ({ wv t1796 = v992_d; wv t1797 = rt_get(&F_pos, v989_x); wv t1798 = rt_def(up->v858_curSlot, "curSlot"); rt_set_idx(t1796, t1797, t1798); });
  RT_AT(5, 298);
  if (rt_cond(({ wv t1801 = rt_get(&F_func, ({ wv t1799 = v992_d; wv t1800 = rt_def(up->v852_decls, "decls"); rt_idx(t1799, t1800); })); wv t1802 = v991_f; rt_ne(t1801, t1802); }))) {
    RT_AT(5, 298);
    rt_set_mem(rt_b(1), &F_captured, ({ wv t1803 = v992_d; wv t1804 = rt_def(up->v852_decls, "decls"); rt_idx(t1803, t1804); }));
  }
  RT_RET(RT_NULL);
}

static wv s876_atarget(struct fr176 *up, wv v993_t, wv v994_scope, wv v995_f) {
  rt_enter("atarget");
  v993_t = rt_bind(v993_t, 0);
  v994_scope = rt_bind(v994_scope, 0);
  v995_f = rt_bind(v995_f, 0);
  RT_AT(5, 302);
  if (rt_cond(rt_eq(rt_get(&F_k, v993_t), rt_s(&S214)))) {
    RT_AT(5, 302);
    (void)rt_chk(s875_use(up, v993_t, v994_scope, v995_f), "atarget");
    RT_AT(5, 302);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 303);
  if (rt_cond(rt_eq(rt_get(&F_k, v993_t), rt_s(&S191)))) {
    RT_AT(5, 303);
    (void)({ wv t1805 = rt_get(&F_index, v993_t); wv t1806 = v994_scope; wv t1807 = v995_f; rt_chk(s878_aexpr(up, t1805, t1806, t1807), "atarget"); });
  }
  RT_AT(5, 304);
  (void)({ wv t1808 = (rt_cond(rt_eq(rt_get(&F_k, v993_t), rt_s(&S192))) ? rt_get(&F_rec, v993_t) : rt_get(&F_arr, v993_t)); wv t1809 = v994_scope; wv t1810 = v995_f; rt_chk(s878_aexpr(up, t1808, t1809, t1810), "atarget"); });
  RT_RET(RT_NULL);
}

static wv s877_notValue(struct fr176 *up, wv v996_x, wv v997_scope) {
  rt_enter("notValue");
  v997_scope = rt_bind(v997_scope, 0);
  RT_AT(5, 309);
  if (rt_cond(rt_eq(rt_get(&F_k, v996_x), rt_s(&S192)))) {
    RT_AT(5, 309);
    RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v996_x)), rt_s(&S214)), "&&") && rt_bool(rt_eq(({ wv t1811 = v997_scope; wv t1812 = rt_get(&F_name, rt_get(&F_rec, v996_x)); rt_chk(s870_kindOf(up, t1811, t1812), "notValue"); }), rt_s(&S507)), "&&")));
  }
  RT_AT(5, 310);
  if (rt_cond(rt_ne(rt_get(&F_k, v996_x), rt_s(&S214)))) {
    RT_AT(5, 310);
    RT_RET(rt_b(0));
  }
  RT_AT(5, 311);
  wv v998_kind = rt_bind(({ wv t1813 = v997_scope; wv t1814 = rt_get(&F_name, v996_x); rt_chk(s870_kindOf(up, t1813, t1814), "notValue"); }), 0);
  RT_RET((RT_AT(5, 312), rt_b(rt_bool(rt_eq(v998_kind, rt_s(&S47)), "||") || rt_bool(rt_eq(v998_kind, rt_s(&S507)), "||"))));
}

static wv s878_aexpr(struct fr176 *up, wv v999_e, wv v1000_scope, wv v1001_f) {
  rt_enter("aexpr");
  v1000_scope = rt_bind(v1000_scope, 0);
  v1001_f = rt_bind(v1001_f, 0);
  RT_AT(5, 316);
  wv v1002_kind = rt_bind(rt_get(&F_k, v999_e), 0);
  RT_AT(5, 317);
  if (rt_cond(rt_eq(v1002_kind, rt_s(&S173)))) {
    RT_AT(5, 318);
    wv v1003_k = rt_i(0);
    RT_AT(5, 319);
    while (rt_cond((RT_AT(5, 319), ({ wv t1815 = v1003_k; wv t1816 = std_length(rt_get(&F_parts, v999_e)); rt_lt(t1815, t1816); })))) {
      RT_AT(5, 320);
      if (rt_cond(rt_ne(std_typeOf(({ wv t1817 = v1003_k; wv t1818 = rt_get(&F_parts, v999_e); rt_idx(t1817, t1818); })), rt_s(&S187)))) {
        RT_AT(5, 320);
        (void)({ wv t1821 = ({ wv t1819 = v1003_k; wv t1820 = rt_get(&F_parts, v999_e); rt_idx(t1819, t1820); }); wv t1822 = v1000_scope; wv t1823 = v1001_f; rt_chk(s878_aexpr(up, t1821, t1822, t1823), "aexpr"); });
      }
      RT_AT(5, 321);
      v1003_k = rt_add(v1003_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S214)))) {
    RT_AT(5, 324);
    (void)rt_chk(s875_use(up, v999_e, v1000_scope, v1001_f), "aexpr");
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v1002_kind, rt_s(&S188)), "||") || rt_bool(rt_eq(v1002_kind, rt_s(&S189)), "||")))) {
    RT_AT(5, 326);
    wv v1004_k = rt_i(0);
    RT_AT(5, 327);
    while (rt_cond((RT_AT(5, 327), ({ wv t1824 = v1004_k; wv t1825 = std_length(rt_get(&F_items, v999_e)); rt_lt(t1824, t1825); })))) {
      RT_AT(5, 327);
      (void)({ wv t1828 = ({ wv t1826 = v1004_k; wv t1827 = rt_get(&F_items, v999_e); rt_idx(t1826, t1827); }); wv t1829 = v1000_scope; wv t1830 = v1001_f; rt_chk(s878_aexpr(up, t1828, t1829, t1830), "aexpr"); });
      RT_AT(5, 327);
      v1004_k = rt_add(v1004_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S190)))) {
    RT_AT(5, 329);
    wv v1005_k = rt_i(0);
    RT_AT(5, 330);
    while (rt_cond((RT_AT(5, 330), ({ wv t1831 = v1005_k; wv t1832 = std_length(rt_get(&F_fields, v999_e)); rt_lt(t1831, t1832); })))) {
      RT_AT(5, 330);
      (void)({ wv t1835 = rt_get(&F_e, ({ wv t1833 = v1005_k; wv t1834 = rt_get(&F_fields, v999_e); rt_idx(t1833, t1834); })); wv t1836 = v1000_scope; wv t1837 = v1001_f; rt_chk(s878_aexpr(up, t1835, t1836, t1837), "aexpr"); });
      RT_AT(5, 330);
      v1005_k = rt_add(v1005_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S191)))) {
    RT_AT(5, 332);
    (void)({ wv t1838 = rt_get(&F_index, v999_e); wv t1839 = v1000_scope; wv t1840 = v1001_f; rt_chk(s878_aexpr(up, t1838, t1839, t1840), "aexpr"); });
    RT_AT(5, 333);
    (void)({ wv t1841 = rt_get(&F_arr, v999_e); wv t1842 = v1000_scope; wv t1843 = v1001_f; rt_chk(s878_aexpr(up, t1841, t1842, t1843), "aexpr"); });
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S192)))) {
    RT_AT(5, 335);
    if (rt_cond(rt_not(({ wv t1844 = rt_get(&F_rec, v999_e); wv t1845 = v1000_scope; rt_chk(s877_notValue(up, t1844, t1845), "aexpr"); })))) {
      RT_AT(5, 335);
      (void)({ wv t1846 = rt_get(&F_rec, v999_e); wv t1847 = v1000_scope; wv t1848 = v1001_f; rt_chk(s878_aexpr(up, t1846, t1847, t1848), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S193)))) {
    RT_AT(5, 337);
    (void)({ wv t1849 = rt_get(&F_e, v999_e); wv t1850 = v1000_scope; wv t1851 = v1001_f; rt_chk(s878_aexpr(up, t1849, t1850, t1851), "aexpr"); });
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S194)))) {
    RT_AT(5, 339);
    (void)({ wv t1852 = rt_get(&F_l, v999_e); wv t1853 = v1000_scope; wv t1854 = v1001_f; rt_chk(s878_aexpr(up, t1852, t1853, t1854), "aexpr"); });
    RT_AT(5, 340);
    (void)({ wv t1855 = rt_get(&F_r, v999_e); wv t1856 = v1000_scope; wv t1857 = v1001_f; rt_chk(s878_aexpr(up, t1855, t1856, t1857), "aexpr"); });
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S195)))) {
    RT_AT(5, 342);
    (void)({ wv t1858 = rt_get(&F_c, v999_e); wv t1859 = v1000_scope; wv t1860 = v1001_f; rt_chk(s878_aexpr(up, t1858, t1859, t1860), "aexpr"); });
    RT_AT(5, 343);
    (void)({ wv t1861 = rt_get(&F_a, v999_e); wv t1862 = v1000_scope; wv t1863 = v1001_f; rt_chk(s878_aexpr(up, t1861, t1862, t1863), "aexpr"); });
    RT_AT(5, 344);
    (void)({ wv t1864 = rt_get(&F_b, v999_e); wv t1865 = v1000_scope; wv t1866 = v1001_f; rt_chk(s878_aexpr(up, t1864, t1865, t1866), "aexpr"); });
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S184)))) {
    RT_AT(5, 346);
    wv v1006_k = rt_i(0);
    RT_AT(5, 347);
    while (rt_cond((RT_AT(5, 347), ({ wv t1867 = v1006_k; wv t1868 = std_length(rt_get(&F_args, v999_e)); rt_lt(t1867, t1868); })))) {
      RT_AT(5, 347);
      (void)({ wv t1871 = ({ wv t1869 = v1006_k; wv t1870 = rt_get(&F_args, v999_e); rt_idx(t1869, t1870); }); wv t1872 = v1000_scope; wv t1873 = v1001_f; rt_chk(s878_aexpr(up, t1871, t1872, t1873), "aexpr"); });
      RT_AT(5, 347);
      v1006_k = rt_add(v1006_k, rt_i(1));
    }
    RT_AT(5, 348);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_callee, v999_e)), rt_s(&S214)))) {
      RT_AT(5, 348);
      ({ wv t1876 = rt_get(&F_d, ({ wv t1874 = v1000_scope; wv t1875 = rt_get(&F_name, rt_get(&F_callee, v999_e)); rt_chk(s869_find(up, t1874, t1875), "aexpr"); })); wv t1877 = rt_get(&F_pos, rt_get(&F_callee, v999_e)); wv t1878 = rt_def(up->v858_curSlot, "curSlot"); rt_set_idx(t1876, t1877, t1878); });
    }
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S186)))) {
    RT_AT(5, 350);
    wv v1007_k = rt_i(0);
    RT_AT(5, 351);
    while (rt_cond((RT_AT(5, 351), ({ wv t1879 = v1007_k; wv t1880 = std_length(rt_get(&F_branches, v999_e)); rt_lt(t1879, t1880); })))) {
      RT_AT(5, 352);
      (void)({ wv t1883 = rt_get(&F_cond, ({ wv t1881 = v1007_k; wv t1882 = rt_get(&F_branches, v999_e); rt_idx(t1881, t1882); })); wv t1884 = v1000_scope; wv t1885 = v1001_f; rt_chk(s878_aexpr(up, t1883, t1884, t1885), "aexpr"); });
      RT_AT(5, 353);
      (void)({ wv t1888 = rt_get(&F_body, ({ wv t1886 = v1007_k; wv t1887 = rt_get(&F_branches, v999_e); rt_idx(t1886, t1887); })); wv t1889 = rt_chk(s867_newScope(up, v1000_scope), "aexpr"); wv t1890 = v1001_f; rt_chk(s873_ablock(up, t1888, t1889, t1890, RT_NULL), "aexpr"); });
      RT_AT(5, 354);
      v1007_k = rt_add(v1007_k, rt_i(1));
    }
    RT_AT(5, 356);
    if (rt_cond(rt_ne(rt_get(&F_els, v999_e), RT_NULL))) {
      RT_AT(5, 356);
      (void)({ wv t1891 = rt_get(&F_els, v999_e); wv t1892 = rt_chk(s867_newScope(up, v1000_scope), "aexpr"); wv t1893 = v1001_f; rt_chk(s873_ablock(up, t1891, t1892, t1893, RT_NULL), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v1002_kind, rt_s(&S45)))) {
    RT_AT(5, 358);
    (void)({ wv t1894 = rt_get(&F_subject, v999_e); wv t1895 = v1000_scope; wv t1896 = v1001_f; rt_chk(s878_aexpr(up, t1894, t1895, t1896), "aexpr"); });
    RT_AT(5, 359);
    wv v1008_k = rt_i(0);
    RT_AT(5, 360);
    while (rt_cond((RT_AT(5, 360), ({ wv t1897 = v1008_k; wv t1898 = std_length(rt_get(&F_cases, v999_e)); rt_lt(t1897, t1898); })))) {
      RT_AT(5, 361);
      (void)({ wv t1901 = rt_get(&F_body, ({ wv t1899 = v1008_k; wv t1900 = rt_get(&F_cases, v999_e); rt_idx(t1899, t1900); })); wv t1902 = rt_chk(s867_newScope(up, v1000_scope), "aexpr"); wv t1903 = v1001_f; rt_chk(s873_ablock(up, t1901, t1902, t1903, RT_NULL), "aexpr"); });
      RT_AT(5, 362);
      v1008_k = rt_add(v1008_k, rt_i(1));
    }
    RT_AT(5, 364);
    if (rt_cond(rt_ne(rt_get(&F_els, v999_e), RT_NULL))) {
      RT_AT(5, 364);
      (void)({ wv t1904 = rt_get(&F_els, v999_e); wv t1905 = rt_chk(s867_newScope(up, v1000_scope), "aexpr"); wv t1906 = v1001_f; rt_chk(s873_ablock(up, t1904, t1905, t1906, RT_NULL), "aexpr"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv s879_analyze(struct fr176 *up, wv v1009_m) {
  rt_enter("analyze");
  v1009_m = rt_bind(v1009_m, 0);
  RT_AT(5, 369);
  wv t1907 = rt_bind(v1009_m, 1);
  (void)rt_def(up->v856_curMod, "curMod");
  up->v856_curMod = t1907;
  RT_AT(5, 370);
  wv t1908 = rt_bind(rt_chk(s865_modId(up, v1009_m), "analyze"), 1);
  (void)rt_def(up->v857_mod, "mod");
  up->v857_mod = t1908;
  RT_AT(5, 371);
  wv v1010_top = rt_bind(rt_chk(s871_newFn(up, RT_NULL, RT_NULL), "analyze"), 0);
  RT_AT(5, 372);
  (void)({ wv t1909 = rt_def(up->v854_tops, "tops"); wv t1910 = v1010_top; std_push(t1909, t1910); });
  RT_AT(5, 373);
  wv v1011_slot = rt_arr(0, NULL, 1);
  RT_AT(5, 374);
  wv v1012_i = rt_i(0);
  RT_AT(5, 375);
  while (rt_cond((RT_AT(5, 375), ({ wv t1911 = v1012_i; wv t1912 = std_length(rt_get(&F_chars, rt_idx(v1009_m, up->v837_modules))); rt_le(t1911, t1912); })))) {
    RT_AT(5, 376);
    (void)std_push(v1011_slot, RT_NULL);
    RT_AT(5, 377);
    v1012_i = rt_add(v1012_i, rt_i(1));
  }
  RT_AT(5, 379);
  (void)({ wv t1913 = rt_def(up->v855_slots, "slots"); wv t1914 = v1011_slot; std_push(t1913, t1914); });
  RT_AT(5, 380);
  wv t1915 = rt_bind(v1011_slot, 1);
  (void)rt_def(up->v858_curSlot, "curSlot");
  up->v858_curSlot = t1915;
  RT_AT(5, 381);
  wv v1013_scope = rt_bind(rt_chk(s867_newScope(up, RT_NULL), "analyze"), 0);
  RT_AT(5, 382);
  wv v1014_imports = rt_bind(rt_get(&F_imports, rt_idx(v1009_m, up->v837_modules)), 0);
  RT_AT(5, 383);
  wv v1015_k = rt_i(0);
  RT_AT(5, 384);
  while (rt_cond((RT_AT(5, 384), ({ wv t1916 = v1015_k; wv t1917 = std_length(v1014_imports); rt_lt(t1916, t1917); })))) {
    RT_AT(5, 385);
    (void)({ wv t1918 = v1013_scope; wv t1919 = rt_get(&F_name, rt_get(&F_node, rt_idx(v1015_k, v1014_imports))); rt_chk(s868_put(up, t1918, t1919, rt_s(&S507), RT_NULL), "analyze"); });
    RT_AT(5, 386);
    v1015_k = rt_add(v1015_k, rt_i(1));
  }
  RT_AT(5, 388);
  (void)({ wv t1920 = rt_rec(0, 2, K54, (wv[]){ rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1009_m, up->v837_modules))), RT_NULL }); wv t1921 = v1013_scope; wv t1922 = v1010_top; wv t1923 = v1013_scope; rt_chk(s873_ablock(up, t1920, t1921, t1922, t1923), "analyze"); });
  RT_RET(RT_NULL);
}

static wv s880_enter(struct fr176 *up, wv v1016_m) {
  rt_enter("enter");
  v1016_m = rt_bind(v1016_m, 0);
  RT_AT(5, 395);
  (void)({ wv t1924 = rt_get(&F_file, rt_idx(v1016_m, up->v837_modules)); wv t1925 = rt_get(&F_chars, rt_idx(v1016_m, up->v837_modules)); rt_chk(w2_setSource(t1924, t1925), "enter"); });
  RT_AT(5, 396);
  wv t1926 = rt_bind(v1016_m, 1);
  (void)rt_def(up->v856_curMod, "curMod");
  up->v856_curMod = t1926;
  RT_AT(5, 397);
  wv t1927 = rt_bind(rt_chk(s865_modId(up, v1016_m), "enter"), 1);
  (void)rt_def(up->v857_mod, "mod");
  up->v857_mod = t1927;
  RT_AT(5, 398);
  wv t1930 = rt_bind(({ wv t1928 = v1016_m; wv t1929 = rt_def(up->v855_slots, "slots"); rt_idx(t1928, t1929); }), 1);
  (void)rt_def(up->v858_curSlot, "curSlot");
  up->v858_curSlot = t1930;
  RT_AT(5, 399);
  wv t1933 = rt_bind(({ wv t1931 = v1016_m; wv t1932 = rt_def(up->v854_tops, "tops"); rt_idx(t1931, t1932); }), 1);
  (void)rt_def(up->v861_cur, "cur");
  up->v861_cur = t1933;
  RT_AT(5, 400);
  wv t1934 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v859_stolen, "stolen");
  up->v859_stolen = t1934;
  RT_AT(5, 401);
  wv t1935 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v860_ownEnums, "ownEnums");
  up->v860_ownEnums = t1935;
  RT_AT(5, 402);
  wv t1936 = rt_arr(1, (wv[]){ rt_i(0) }, 1);
  (void)rt_def(up->v862_lineStarts, "lineStarts");
  up->v862_lineStarts = t1936;
  RT_AT(5, 403);
  wv v1017_chars = rt_bind(rt_get(&F_chars, rt_idx(v1016_m, up->v837_modules)), 0);
  RT_AT(5, 404);
  {
    wfor t1938;
    rt_for_start(&t1938, v1017_chars, 0);
    for (int64_t t1937 = 0; rt_for_guard(&t1938, v1017_chars, "chars"), rt_for_more(&t1938, t1937); t1937++) {
      wv v1021_i = rt_for_key(&t1938, t1937);
      wv v1022_c = rt_for_val(&t1938, t1937);
      RT_AT(5, 405);
      if (rt_cond(rt_eq(v1022_c, rt_s(&S2)))) {
        RT_AT(5, 405);
        (void)({ wv t1939 = rt_def(up->v862_lineStarts, "lineStarts"); wv t1940 = rt_add(v1021_i, rt_i(1)); std_push(t1939, t1940); });
      }
    }
  }
  RT_AT(5, 407);
  wv v1018_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1016_m, up->v837_modules))), 0);
  RT_AT(5, 408);
  wv v1019_k = rt_i(0);
  RT_AT(5, 409);
  while (rt_cond((RT_AT(5, 409), ({ wv t1941 = v1019_k; wv t1942 = std_length(v1018_list); rt_lt(t1941, t1942); })))) {
    RT_AT(5, 410);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1019_k, v1018_list)), rt_s(&S47)))) {
      RT_AT(5, 410);
      (void)({ wv t1943 = rt_def(up->v860_ownEnums, "ownEnums"); wv t1944 = rt_idx(v1019_k, v1018_list); std_push(t1943, t1944); });
    }
    RT_AT(5, 411);
    v1019_k = rt_add(v1019_k, rt_i(1));
  }
  RT_AT(5, 413);
  wv v1020_imports = rt_bind(rt_get(&F_imports, rt_idx(v1016_m, up->v837_modules)), 0);
  RT_AT(5, 414);
  v1019_k = rt_i(0);
  RT_AT(5, 415);
  while (rt_cond((RT_AT(5, 415), ({ wv t1945 = v1019_k; wv t1946 = std_length(v1020_imports); rt_lt(t1945, t1946); })))) {
    RT_AT(5, 416);
    (void)({ wv t1949 = rt_def(up->v859_stolen, "stolen"); wv t1950 = ({ wv t1947 = rt_get(&F_name, rt_get(&F_node, rt_idx(v1019_k, v1020_imports))); wv t1948 = rt_get(&F_lib, rt_idx(v1019_k, v1020_imports)); rt_rec(0, 2, K58, (wv[]){ t1947, t1948 }); }); std_push(t1949, t1950); });
    RT_AT(5, 417);
    v1019_k = rt_add(v1019_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s881_emit(struct fr176 *up, wv v1023_line) {
  rt_enter("emit");
  v1023_line = rt_bind(v1023_line, 0);
  RT_AT(5, 421);
  (void)({ wv t1953 = rt_def(up->v848_out, "out"); wv t1954 = ({ wv t1951 = rt_chk(w6_repeat(rt_s(&S611), rt_def(up->v849_indent, "indent")), "emit"); wv t1952 = v1023_line; rt_add(t1951, t1952); }); std_push(t1953, t1954); });
  RT_RET(RT_NULL);
}

static wv s882_tmp(struct fr176 *up) {
  rt_enter("tmp");
  RT_AT(5, 423);
  wv v1024_name = rt_cat(2, (wv[]){ rt_s(&S105), rt_text(rt_def(up->v840_tmps, "tmps")) });
  RT_AT(5, 424);
  up->v840_tmps = rt_add(rt_def(up->v840_tmps, "tmps"), rt_i(1));
  RT_RET((RT_AT(5, 425), v1024_name));
}

static wv s883_str(struct fr176 *up, wv v1025_s) {
  rt_enter("str");
  v1025_s = rt_bind(v1025_s, 0);
  RT_AT(5, 429);
  wv v1026_k = rt_i(0);
  RT_AT(5, 430);
  while (rt_cond((RT_AT(5, 430), rt_b(rt_bool(({ wv t1955 = v1026_k; wv t1956 = std_length(rt_def(up->v841_strings, "strings")); rt_lt(t1955, t1956); }), "&&") && rt_bool(({ wv t1959 = ({ wv t1957 = v1026_k; wv t1958 = rt_def(up->v841_strings, "strings"); rt_idx(t1957, t1958); }); wv t1960 = v1025_s; rt_ne(t1959, t1960); }), "&&"))))) {
    RT_AT(5, 430);
    v1026_k = rt_add(v1026_k, rt_i(1));
  }
  RT_AT(5, 431);
  if (rt_cond(({ wv t1961 = v1026_k; wv t1962 = std_length(rt_def(up->v841_strings, "strings")); rt_eq(t1961, t1962); }))) {
    RT_AT(5, 431);
    (void)({ wv t1963 = rt_def(up->v841_strings, "strings"); wv t1964 = v1025_s; std_push(t1963, t1964); });
  }
  RT_RET((RT_AT(5, 432), rt_cat(2, (wv[]){ rt_s(&S612), rt_text(v1026_k) })));
}

static wv s884_fieldRef(struct fr176 *up, wv v1027_name) {
  rt_enter("fieldRef");
  v1027_name = rt_bind(v1027_name, 0);
  RT_AT(5, 435);
  if (rt_cond(rt_not(({ wv t1965 = rt_def(up->v842_fields, "fields"); wv t1966 = v1027_name; rt_chk(w1_contains(t1965, t1966), "fieldRef"); })))) {
    RT_AT(5, 435);
    (void)({ wv t1967 = rt_def(up->v842_fields, "fields"); wv t1968 = v1027_name; std_push(t1967, t1968); });
  }
  RT_RET((RT_AT(5, 436), rt_cat(2, (wv[]){ rt_s(&S613), rt_text(v1027_name) })));
}

static wv s885_enumDef(struct fr176 *up, wv v1028_s, wv v1029_m) {
  rt_enter("enumDef");
  v1028_s = rt_bind(v1028_s, 0);
  v1029_m = rt_bind(v1029_m, 0);
  RT_AT(5, 441);
  wv v1030_nm = rt_bind(rt_chk(s883_str(up, rt_get(&F_name, v1028_s)), "enumDef"), 0);
  RT_AT(5, 442);
  wv v1031_members = rt_arr(0, NULL, 1);
  RT_AT(5, 443);
  wv v1032_k = rt_i(0);
  RT_AT(5, 444);
  while (rt_cond((RT_AT(5, 444), ({ wv t1969 = v1032_k; wv t1970 = std_length(rt_get(&F_members, v1028_s)); rt_lt(t1969, t1970); })))) {
    RT_AT(5, 445);
    (void)({ wv t1975 = v1031_members; wv t1976 = ({ wv t1973 = rt_text(v1030_nm); wv t1974 = rt_text(rt_chk(s883_str(up, rt_get(&F_name, ({ wv t1971 = v1032_k; wv t1972 = rt_get(&F_members, v1028_s); rt_idx(t1971, t1972); }))), "enumDef")); rt_cat(5, (wv[]){ rt_s(&S604), t1973, rt_s(&S419), t1974, rt_s(&S420) }); }); std_push(t1975, t1976); });
    RT_AT(5, 446);
    v1032_k = rt_add(v1032_k, rt_i(1));
  }
  RT_AT(5, 448);
  (void)({ wv t1980 = rt_def(up->v844_enumDefs, "enumDefs"); wv t1981 = ({ wv t1977 = rt_text(v1029_m); wv t1978 = rt_text(rt_get(&F_name, v1028_s)); wv t1979 = rt_text(std_join(v1031_members, rt_s(&S419))); rt_cat(7, (wv[]){ rt_s(&S614), t1977, rt_s(&S12), t1978, rt_s(&S615), t1979, rt_s(&S616) }); }); std_push(t1980, t1981); });
  RT_RET(RT_NULL);
}

static wv s886_stolenFrom(struct fr176 *up, wv v1033_x) {
  rt_enter("stolenFrom");
  RT_AT(5, 453);
  if (rt_cond(rt_ne(rt_get(&F_k, v1033_x), rt_s(&S214)))) {
    RT_AT(5, 453);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 454);
  wv v1034_k = rt_i(0);
  RT_AT(5, 455);
  while (rt_cond((RT_AT(5, 455), ({ wv t1982 = v1034_k; wv t1983 = std_length(rt_def(up->v859_stolen, "stolen")); rt_lt(t1982, t1983); })))) {
    RT_AT(5, 456);
    if (rt_cond(({ wv t1986 = rt_get(&F_name, ({ wv t1984 = v1034_k; wv t1985 = rt_def(up->v859_stolen, "stolen"); rt_idx(t1984, t1985); })); wv t1987 = rt_get(&F_name, v1033_x); rt_eq(t1986, t1987); }))) {
      RT_AT(5, 456);
      RT_RET(rt_get(&F_lib, ({ wv t1988 = v1034_k; wv t1989 = rt_def(up->v859_stolen, "stolen"); rt_idx(t1988, t1989); })));
    }
    RT_AT(5, 457);
    v1034_k = rt_add(v1034_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 459), RT_NULL));
}

static wv s887_enumIn(struct fr176 *up, wv v1035_list, wv v1036_name) {
  rt_enter("enumIn");
  v1036_name = rt_bind(v1036_name, 0);
  RT_AT(5, 464);
  wv v1037_k = rt_i(0);
  RT_AT(5, 465);
  while (rt_cond((RT_AT(5, 465), ({ wv t1990 = v1037_k; wv t1991 = std_length(v1035_list); rt_lt(t1990, t1991); })))) {
    RT_AT(5, 466);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_idx(v1037_k, v1035_list)), rt_s(&S47)), "&&") && rt_bool(({ wv t1992 = rt_get(&F_name, rt_idx(v1037_k, v1035_list)); wv t1993 = v1036_name; rt_eq(t1992, t1993); }), "&&")))) {
      RT_AT(5, 466);
      RT_RET(rt_idx(v1037_k, v1035_list));
    }
    RT_AT(5, 467);
    v1037_k = rt_add(v1037_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 469), RT_NULL));
}

static wv s888_enumOf(struct fr176 *up, wv v1038_x) {
  rt_enter("enumOf");
  RT_AT(5, 474);
  if (rt_cond(rt_eq(rt_get(&F_k, v1038_x), rt_s(&S214)))) {
    RT_AT(5, 475);
    wv v1042_s = rt_bind(({ wv t1994 = rt_def(up->v860_ownEnums, "ownEnums"); wv t1995 = rt_get(&F_name, v1038_x); rt_chk(s887_enumIn(up, t1994, t1995), "enumOf"); }), 0);
    RT_AT(5, 476);
    RT_RET((rt_cond(rt_eq(v1042_s, RT_NULL)) ? RT_NULL : ({ wv t1998 = ({ wv t1996 = rt_text(rt_def(up->v857_mod, "mod")); wv t1997 = rt_text(rt_get(&F_name, v1038_x)); rt_cat(4, (wv[]){ rt_s(&S617), t1996, rt_s(&S12), t1997 }); }); wv t1999 = v1042_s; rt_rec(0, 2, K59, (wv[]){ t1998, t1999 }); })));
  }
  RT_AT(5, 478);
  if (rt_cond(rt_ne(rt_get(&F_k, v1038_x), rt_s(&S192)))) {
    RT_AT(5, 478);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 479);
  wv v1039_lib = rt_bind(rt_chk(s886_stolenFrom(up, rt_get(&F_rec, v1038_x)), "enumOf"), 0);
  RT_AT(5, 480);
  if (rt_cond(rt_eq(v1039_lib, RT_NULL))) {
    RT_AT(5, 480);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 481);
  wv v1040_xs = rt_bind(rt_get(&F_exports, rt_idx(v1039_lib, up->v837_modules)), 0);
  RT_AT(5, 482);
  wv v1041_k = rt_i(0);
  RT_AT(5, 483);
  while (rt_cond((RT_AT(5, 483), rt_b(rt_bool(({ wv t2000 = v1041_k; wv t2001 = std_length(v1040_xs); rt_lt(t2000, t2001); }), "&&") && rt_bool(({ wv t2002 = rt_get(&F_name, rt_idx(v1041_k, v1040_xs)); wv t2003 = rt_get(&F_field, v1038_x); rt_ne(t2002, t2003); }), "&&"))))) {
    RT_AT(5, 483);
    v1041_k = rt_add(v1041_k, rt_i(1));
  }
  RT_AT(5, 484);
  if (rt_cond(rt_b(rt_bool(({ wv t2004 = v1041_k; wv t2005 = std_length(v1040_xs); rt_eq(t2004, t2005); }), "||") || rt_bool(rt_ne(rt_get(&F_kind, rt_idx(v1041_k, v1040_xs)), rt_s(&S47)), "||")))) {
    RT_AT(5, 484);
    RT_RET(RT_NULL);
  }
  RT_RET((RT_AT(5, 485), ({ wv t2010 = ({ wv t2006 = rt_text(rt_chk(s865_modId(up, v1039_lib), "enumOf")); wv t2007 = rt_text(rt_get(&F_field, v1038_x)); rt_cat(4, (wv[]){ rt_s(&S617), t2006, rt_s(&S12), t2007 }); }); wv t2011 = ({ wv t2008 = rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1039_lib, up->v837_modules))); wv t2009 = rt_get(&F_field, v1038_x); rt_chk(s887_enumIn(up, t2008, t2009), "enumOf"); }); rt_rec(0, 2, K59, (wv[]){ t2010, t2011 }); })));
}

static wv s889_place(struct fr176 *up, wv v1043_d) {
  rt_enter("place");
  v1043_d = rt_bind(v1043_d, 0);
  RT_AT(5, 490);
  wv v1044_owner = rt_bind(rt_get(&F_func, ({ wv t2012 = v1043_d; wv t2013 = rt_def(up->v852_decls, "decls"); rt_idx(t2012, t2013); })), 0);
  RT_AT(5, 491);
  if (rt_cond(rt_get(&F_top, ({ wv t2014 = v1044_owner; wv t2015 = rt_def(up->v853_fns, "fns"); rt_idx(t2014, t2015); })))) {
    RT_AT(5, 491);
    RT_RET(rt_get(&F_cname, ({ wv t2016 = v1043_d; wv t2017 = rt_def(up->v852_decls, "decls"); rt_idx(t2016, t2017); })));
  }
  RT_AT(5, 492);
  if (rt_cond(({ wv t2018 = v1044_owner; wv t2019 = rt_def(up->v861_cur, "cur"); rt_eq(t2018, t2019); }))) {
    RT_AT(5, 492);
    RT_RET((rt_cond(rt_get(&F_captured, ({ wv t2020 = v1043_d; wv t2021 = rt_def(up->v852_decls, "decls"); rt_idx(t2020, t2021); }))) ? rt_cat(2, (wv[]){ rt_s(&S618), rt_text(rt_get(&F_cname, ({ wv t2022 = v1043_d; wv t2023 = rt_def(up->v852_decls, "decls"); rt_idx(t2022, t2023); }))) }) : rt_get(&F_cname, ({ wv t2024 = v1043_d; wv t2025 = rt_def(up->v852_decls, "decls"); rt_idx(t2024, t2025); }))));
  }
  RT_RET((RT_AT(5, 493), ({ wv t2034 = rt_text(rt_chk(w6_repeat(rt_s(&S620), rt_sub(({ wv t2030 = rt_get(&F_depth, ({ wv t2026 = rt_def(up->v861_cur, "cur"); wv t2027 = rt_def(up->v853_fns, "fns"); rt_idx(t2026, t2027); })); wv t2031 = rt_get(&F_depth, ({ wv t2028 = v1044_owner; wv t2029 = rt_def(up->v853_fns, "fns"); rt_idx(t2028, t2029); })); rt_sub(t2030, t2031); }), rt_i(1))), "place")); wv t2035 = rt_text(rt_get(&F_cname, ({ wv t2032 = v1043_d; wv t2033 = rt_def(up->v852_decls, "decls"); rt_idx(t2032, t2033); }))); rt_cat(4, (wv[]){ rt_s(&S619), t2034, rt_s(&S621), t2035 }); })));
}

static wv s890_tdz(struct fr176 *up, wv v1045_d) {
  rt_enter("tdz");
  v1045_d = rt_bind(v1045_d, 0);
  RT_RET((RT_AT(5, 496), rt_b(rt_bool(rt_eq(rt_get(&F_kind, ({ wv t2036 = v1045_d; wv t2037 = rt_def(up->v852_decls, "decls"); rt_idx(t2036, t2037); })), rt_s(&S179)), "&&") && rt_bool(({ wv t2040 = rt_get(&F_func, ({ wv t2038 = v1045_d; wv t2039 = rt_def(up->v852_decls, "decls"); rt_idx(t2038, t2039); })); wv t2041 = rt_def(up->v861_cur, "cur"); rt_ne(t2040, t2041); }), "&&"))));
}

static wv s891_read(struct fr176 *up, wv v1046_d) {
  rt_enter("read");
  v1046_d = rt_bind(v1046_d, 0);
  RT_RET((RT_AT(5, 498), (rt_cond(rt_chk(s890_tdz(up, v1046_d), "read")) ? ({ wv t2046 = ({ wv t2044 = rt_text(rt_chk(s889_place(up, v1046_d), "read")); wv t2045 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t2042 = v1046_d; wv t2043 = rt_def(up->v852_decls, "decls"); rt_idx(t2042, t2043); }))), "read")); rt_cat(5, (wv[]){ rt_s(&S622), t2044, rt_s(&S419), t2045, rt_s(&S16) }); }); wv t2047 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2046, t2047 }); }) : ({ wv t2048 = rt_chk(s889_place(up, v1046_d), "read"); wv t2049 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K60, (wv[]){ t2048, t2049 }); }))));
}

static wv s892_seq(struct fr176 *up, wv v1047_args) {
  rt_enter("seq");
  v1047_args = rt_bind(v1047_args, 0);
  RT_AT(5, 505);
  wv v1048_effects = rt_i(0);
  RT_AT(5, 506);
  wv v1049_anyPure = rt_b(0);
  RT_AT(5, 507);
  wv v1050_k = rt_i(0);
  RT_AT(5, 508);
  while (rt_cond((RT_AT(5, 508), ({ wv t2050 = v1050_k; wv t2051 = std_length(v1047_args); rt_lt(t2050, t2051); })))) {
    RT_AT(5, 509);
    if (rt_cond(({ wv t2052 = rt_get(&F_k, rt_idx(v1050_k, v1047_args)); wv t2053 = rt_def(w6_effectK, "effectK"); rt_eq(t2052, t2053); }))) {
      RT_AT(5, 509);
      v1048_effects = rt_add(v1048_effects, rt_i(1));
    }
    RT_AT(5, 510);
    if (rt_cond(({ wv t2054 = rt_get(&F_k, rt_idx(v1050_k, v1047_args)); wv t2055 = rt_def(w6_pureK, "pureK"); rt_eq(t2054, t2055); }))) {
      RT_AT(5, 510);
      v1049_anyPure = rt_b(1);
    }
    RT_AT(5, 511);
    v1050_k = rt_add(v1050_k, rt_i(1));
  }
  RT_AT(5, 513);
  wv v1051_cs = rt_arr(0, NULL, 1);
  RT_AT(5, 514);
  if (rt_cond(rt_b(rt_bool(rt_eq(v1048_effects, rt_i(0)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1048_effects, rt_i(1)), "&&") && rt_bool(rt_not(v1049_anyPure), "&&")), "||")))) {
    RT_AT(5, 515);
    v1050_k = rt_i(0);
    RT_AT(5, 516);
    while (rt_cond((RT_AT(5, 516), ({ wv t2056 = v1050_k; wv t2057 = std_length(v1047_args); rt_lt(t2056, t2057); })))) {
      RT_AT(5, 516);
      (void)({ wv t2058 = v1051_cs; wv t2059 = rt_get(&F_c, rt_idx(v1050_k, v1047_args)); std_push(t2058, t2059); });
      RT_AT(5, 516);
      v1050_k = rt_add(v1050_k, rt_i(1));
    }
    RT_AT(5, 517);
    RT_RET(rt_rec(0, 2, K61, (wv[]){ rt_s(&S13), v1051_cs }));
  }
  RT_AT(5, 519);
  wv v1052_pre = rt_arr(0, NULL, 1);
  RT_AT(5, 520);
  v1050_k = rt_i(0);
  RT_AT(5, 521);
  while (rt_cond((RT_AT(5, 521), ({ wv t2060 = v1050_k; wv t2061 = std_length(v1047_args); rt_lt(t2060, t2061); })))) {
    RT_AT(5, 522);
    if (rt_cond(({ wv t2062 = rt_get(&F_k, rt_idx(v1050_k, v1047_args)); wv t2063 = rt_def(w6_constK, "constK"); rt_eq(t2062, t2063); }))) {
      RT_AT(5, 523);
      (void)({ wv t2064 = v1051_cs; wv t2065 = rt_get(&F_c, rt_idx(v1050_k, v1047_args)); std_push(t2064, t2065); });
    } else {
      RT_AT(5, 525);
      wv v1053_temp = rt_bind(rt_chk(s882_tmp(up), "seq"), 0);
      RT_AT(5, 526);
      (void)({ wv t2068 = v1052_pre; wv t2069 = ({ wv t2066 = rt_text(v1053_temp); wv t2067 = rt_text(rt_get(&F_c, rt_idx(v1050_k, v1047_args))); rt_cat(5, (wv[]){ rt_s(&S623), t2066, rt_s(&S624), t2067, rt_s(&S14) }); }); std_push(t2068, t2069); });
      RT_AT(5, 527);
      (void)std_push(v1051_cs, v1053_temp);
    }
    RT_AT(5, 529);
    v1050_k = rt_add(v1050_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 531), ({ wv t2070 = std_join(v1052_pre, rt_s(&S0)); wv t2071 = v1051_cs; rt_rec(0, 2, K61, (wv[]){ t2070, t2071 }); })));
}

static wv s893_wrap(struct fr176 *up, wv v1054_sq, wv v1055_inner) {
  rt_enter("wrap");
  v1054_sq = rt_bind(v1054_sq, 0);
  v1055_inner = rt_bind(v1055_inner, 0);
  RT_RET((RT_AT(5, 533), ({ wv t2074 = (rt_cond(rt_eq(rt_get(&F_pre, v1054_sq), rt_s(&S13))) ? v1055_inner : ({ wv t2072 = rt_text(rt_get(&F_pre, v1054_sq)); wv t2073 = rt_text(v1055_inner); rt_cat(5, (wv[]){ rt_s(&S625), t2072, rt_s(&S0), t2073, rt_s(&S626) }); })); wv t2075 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2074, t2075 }); })));
}

static wv s894_stmts(struct fr176 *up, wv v1056_list) {
  rt_enter("stmts");
  v1056_list = rt_bind(v1056_list, 0);
  RT_AT(5, 536);
  wv v1057_k = rt_i(0);
  RT_AT(5, 537);
  while (rt_cond((RT_AT(5, 537), ({ wv t2076 = v1057_k; wv t2077 = std_length(v1056_list); rt_lt(t2076, t2077); })))) {
    RT_AT(5, 538);
    (void)rt_chk(s898_stmt(up, rt_idx(v1057_k, v1056_list)), "stmts");
    RT_AT(5, 539);
    v1057_k = rt_add(v1057_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s895_resets(struct fr176 *up, wv v1058_b) {
  rt_enter("resets");
  RT_AT(5, 546);
  wv v1059_k = rt_i(0);
  RT_AT(5, 547);
  while (rt_cond((RT_AT(5, 547), ({ wv t2078 = v1059_k; wv t2079 = std_length(rt_get(&F_stmts, v1058_b)); rt_lt(t2078, t2079); })))) {
    RT_AT(5, 548);
    wv v1060_s = ({ wv t2080 = v1059_k; wv t2081 = rt_get(&F_stmts, v1058_b); rt_idx(t2080, t2081); });
    RT_AT(5, 549);
    if (rt_cond(rt_eq(rt_get(&F_k, v1060_s), rt_s(&S179)))) {
      RT_AT(5, 550);
      wv v1061_d = rt_bind(({ wv t2082 = rt_get(&F_namePos, v1060_s); wv t2083 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2082, t2083); }), 0);
      RT_AT(5, 551);
      if (rt_cond(rt_get(&F_captured, ({ wv t2084 = v1061_d; wv t2085 = rt_def(up->v852_decls, "decls"); rt_idx(t2084, t2085); })))) {
        RT_AT(5, 551);
        (void)rt_chk(s881_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s889_place(up, v1061_d), "resets")), rt_s(&S627) })), "resets");
      }
    }
    RT_AT(5, 553);
    v1059_k = rt_add(v1059_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s896_blockBody(struct fr176 *up, wv v1062_b) {
  rt_enter("blockBody");
  v1062_b = rt_bind(v1062_b, 0);
  RT_AT(5, 559);
  (void)rt_chk(s895_resets(up, v1062_b), "blockBody");
  RT_AT(5, 560);
  (void)rt_chk(s894_stmts(up, rt_get(&F_stmts, v1062_b)), "blockBody");
  RT_AT(5, 561);
  if (rt_cond(rt_ne(rt_get(&F_value, v1062_b), RT_NULL))) {
    RT_AT(5, 561);
    (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S628), rt_text(rt_chk(s897_valueOf(up, v1062_b), "blockBody")), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s897_valueOf(struct fr176 *up, wv v1063_b) {
  rt_enter("valueOf");
  v1063_b = rt_bind(v1063_b, 0);
  RT_RET((RT_AT(5, 565), (rt_cond(rt_eq(rt_get(&F_value, v1063_b), RT_NULL)) ? rt_s(&S629) : ({ wv t2086 = rt_get(&F_pos, rt_get(&F_value, v1063_b)); wv t2087 = rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_value, v1063_b)), "valueOf")); rt_chk(s864_atExpr(up, t2086, t2087), "valueOf"); }))));
}

static wv s898_stmt(struct fr176 *up, wv v1064_s) {
  rt_enter("stmt");
  RT_AT(5, 568);
  wv v1065_kind = rt_bind(rt_get(&F_k, v1064_s), 0);
  RT_AT(5, 569);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1065_kind, rt_s(&S203)), "||") || rt_bool(rt_eq(v1065_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v1065_kind, rt_s(&S48)), "||")), "||") || rt_bool(rt_eq(v1065_kind, rt_s(&S206)), "||")))) {
    RT_AT(5, 569);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 570);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S33)))) {
    RT_AT(5, 570);
    (void)rt_chk(s903_fnDef(up, v1064_s), "stmt");
    RT_AT(5, 570);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 571);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1065_kind, rt_s(&S43)), "&&") && rt_bool(rt_ne(v1065_kind, rt_s(&S44)), "&&")))) {
    RT_AT(5, 571);
    (void)rt_chk(s881_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s863_atLine(up, rt_get(&F_pos, v1064_s)), "stmt")), rt_s(&S14) })), "stmt");
  }
  RT_AT(5, 572);
  if (rt_cond(rt_ne(rt_get(&F_handler, v1064_s), RT_NULL))) {
    RT_AT(5, 572);
    (void)rt_chk(s910_onfailStmt(up, v1064_s), "stmt");
    RT_AT(5, 572);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 573);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S34)))) {
    RT_AT(5, 574);
    if (rt_cond(rt_def(up->v851_iife, "iife"))) {
      RT_AT(5, 574);
      (void)rt_chk(w2_abort(rt_get(&F_pos, v1064_s), rt_s(&S630)), "stmt");
    }
    RT_AT(5, 575);
    (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S631), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1064_s), RT_NULL)) ? rt_s(&S629) : rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_e, v1064_s)), "stmt")))), rt_s(&S632) })), "stmt");
    RT_AT(5, 576);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 578);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S58)))) {
    RT_AT(5, 578);
    (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S633), rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_e, v1064_s)), "stmt"))), rt_s(&S632) })), "stmt");
    RT_AT(5, 578);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 579);
  if (rt_cond(rt_b(rt_bool(rt_eq(v1065_kind, rt_s(&S43)), "||") || rt_bool(rt_eq(v1065_kind, rt_s(&S44)), "||")))) {
    RT_AT(5, 580);
    if (rt_cond(rt_eq(rt_def(up->v850_loops, "loops"), rt_i(0)))) {
      RT_AT(5, 580);
      (void)({ wv t2088 = rt_get(&F_pos, v1064_s); wv t2089 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text((rt_cond(rt_eq(v1065_kind, rt_s(&S43))) ? rt_s(&S43) : rt_s(&S44))), rt_s(&S634) }); rt_chk(w2_abort(t2088, t2089), "stmt"); });
    }
    RT_AT(5, 581);
    (void)rt_chk(s881_emit(up, (rt_cond(rt_eq(v1065_kind, rt_s(&S43))) ? rt_s(&S635) : rt_s(&S636))), "stmt");
    RT_AT(5, 582);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 584);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S41)))) {
    RT_AT(5, 586);
    (void)rt_chk(s881_emit(up, ({ wv t2092 = rt_text((rt_cond(rt_get(&F_neg, v1064_s)) ? rt_s(&S95) : rt_s(&S13))); wv t2093 = rt_text(({ wv t2090 = rt_get(&F_pos, rt_get(&F_cond, v1064_s)); wv t2091 = rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_cond, v1064_s)), "stmt")); rt_chk(s864_atExpr(up, t2090, t2091), "stmt"); })); rt_cat(5, (wv[]){ rt_s(&S637), t2092, rt_s(&S638), t2093, rt_s(&S639) }); })), "stmt");
    RT_AT(5, 587);
    up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 588);
    up->v850_loops = rt_add(rt_def(up->v850_loops, "loops"), rt_i(1));
    RT_AT(5, 589);
    (void)rt_chk(s896_blockBody(up, rt_get(&F_body, v1064_s)), "stmt");
    RT_AT(5, 590);
    up->v850_loops = rt_sub(rt_def(up->v850_loops, "loops"), rt_i(1));
    RT_AT(5, 591);
    up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 592);
    (void)rt_chk(s881_emit(up, rt_s(&S15)), "stmt");
    RT_AT(5, 593);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 595);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S42)))) {
    RT_AT(5, 595);
    (void)rt_chk(s900_forStmt(up, v1064_s), "stmt");
    RT_AT(5, 595);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 596);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S56)))) {
    RT_AT(5, 596);
    (void)rt_chk(s901_signal(up, rt_cat(3, (wv[]){ rt_s(&S640), rt_text(rt_get(&F_code, v1064_s)), rt_s(&S16) })), "stmt");
    RT_AT(5, 596);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 597);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S55)))) {
    RT_AT(5, 597);
    (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S641), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1064_s), RT_NULL)) ? rt_s(&S642) : rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_e, v1064_s)), "stmt")))), rt_s(&S632) })), "stmt");
    RT_AT(5, 597);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 598);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S178)))) {
    RT_AT(5, 599);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1064_s)), rt_s(&S186)))) {
      RT_AT(5, 599);
      (void)rt_chk(s911_chainStmt(up, rt_get(&F_e, v1064_s)), "stmt");
      RT_AT(5, 599);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 600);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1064_s)), rt_s(&S45)))) {
      RT_AT(5, 600);
      (void)rt_chk(s922_matchStmt(up, rt_get(&F_e, v1064_s)), "stmt");
      RT_AT(5, 600);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 601);
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1064_s)), rt_s(&S185)))) {
      RT_AT(5, 601);
      (void)rt_chk(s901_signal(up, rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_arg, rt_get(&F_e, v1064_s))), "stmt"))), "stmt");
      RT_AT(5, 601);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 602);
    (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S628), rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_e, v1064_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_AT(5, 603);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 605);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S179)))) {
    RT_AT(5, 605);
    (void)({ wv t2098 = ({ wv t2094 = rt_get(&F_namePos, v1064_s); wv t2095 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2094, t2095); }); wv t2099 = ({ wv t2096 = rt_get(&F_e, v1064_s); wv t2097 = rt_chk(w6_modeOf(v1064_s), "stmt"); rt_chk(s904_bindRhs(up, t2096, t2097), "stmt"); }); rt_chk(s902_bindDecl(up, t2098, t2099), "stmt"); });
    RT_AT(5, 605);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 606);
  if (rt_cond(rt_eq(v1065_kind, rt_s(&S38)))) {
    RT_AT(5, 606);
    (void)rt_chk(s905_setStmt(up, v1064_s), "stmt");
    RT_AT(5, 606);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 607);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S643), rt_text(v1065_kind) }));
  RT_RET(RT_NULL);
}

static wv s899_pathText(struct fr176 *up, wv v1066_x) {
  rt_enter("pathText");
  v1066_x = rt_bind(v1066_x, 0);
  RT_AT(5, 612);
  if (rt_cond(rt_eq(rt_get(&F_k, v1066_x), rt_s(&S214)))) {
    RT_AT(5, 612);
    RT_RET(rt_get(&F_name, v1066_x));
  }
  RT_AT(5, 613);
  if (rt_cond(rt_ne(rt_get(&F_k, v1066_x), rt_s(&S192)))) {
    RT_AT(5, 613);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 614);
  wv v1067_p = rt_bind(rt_chk(s899_pathText(up, rt_get(&F_rec, v1066_x)), "pathText"), 0);
  RT_RET((RT_AT(5, 615), (rt_cond(rt_eq(v1067_p, RT_NULL)) ? RT_NULL : ({ wv t2100 = rt_text(v1067_p); wv t2101 = rt_text(rt_get(&F_field, v1066_x)); rt_cat(3, (wv[]){ t2100, rt_s(&S130), t2101 }); }))));
}

static wv s900_forStmt(struct fr176 *up, wv v1068_s) {
  rt_enter("forStmt");
  v1068_s = rt_bind(v1068_s, 0);
  RT_AT(5, 621);
  wv v1069_k = rt_bind(rt_chk(s882_tmp(up), "forStmt"), 0);
  RT_AT(5, 622);
  wv v1070_first = rt_bind(({ wv t2102 = rt_get(&F_pos, rt_idx(rt_i(0), rt_get(&F_names, v1068_s))); wv t2103 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2102, t2103); }), 0);
  RT_AT(5, 623);
  wv v1071_second = rt_bind((rt_cond(rt_eq(std_length(rt_get(&F_names, v1068_s)), rt_i(2))) ? ({ wv t2104 = rt_get(&F_pos, rt_idx(rt_i(1), rt_get(&F_names, v1068_s))); wv t2105 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2104, t2105); }) : RT_NULL), 0);
  RT_AT(5, 624);
  (void)rt_chk(s881_emit(up, rt_s(&S101)), "forStmt");
  RT_AT(5, 625);
  up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 626);
  if (rt_cond(rt_eq(rt_get(&F_coll, v1068_s), RT_NULL))) {
    RT_AT(5, 628);
    wv v1072_lo = rt_bind(rt_chk(s882_tmp(up), "forStmt"), 0);
    RT_AT(5, 629);
    wv v1073_hi = rt_bind(rt_chk(s882_tmp(up), "forStmt"), 0);
    RT_AT(5, 630);
    wv v1074_st = rt_bind(rt_chk(s882_tmp(up), "forStmt"), 0);
    RT_AT(5, 631);
    wv v1075_count = rt_bind(rt_chk(s882_tmp(up), "forStmt"), 0);
    RT_AT(5, 632);
    (void)rt_chk(s881_emit(up, ({ wv t2106 = rt_text(v1072_lo); wv t2107 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_from, v1068_s)), "forStmt"))); rt_cat(5, (wv[]){ rt_s(&S623), t2106, rt_s(&S624), t2107, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 633);
    (void)rt_chk(s881_emit(up, ({ wv t2108 = rt_text(v1073_hi); wv t2109 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_to, v1068_s)), "forStmt"))); rt_cat(5, (wv[]){ rt_s(&S623), t2108, rt_s(&S624), t2109, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 634);
    (void)rt_chk(s881_emit(up, ({ wv t2110 = rt_text(v1074_st); wv t2111 = rt_text((rt_cond(rt_eq(rt_get(&F_step, v1068_s), RT_NULL)) ? rt_s(&S644) : rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_step, v1068_s)), "forStmt")))); rt_cat(5, (wv[]){ rt_s(&S623), t2110, rt_s(&S624), t2111, rt_s(&S14) }); })), "forStmt");
    RT_AT(5, 635);
    (void)rt_chk(s881_emit(up, ({ wv t2112 = rt_text(v1075_count); wv t2113 = rt_text(v1072_lo); wv t2114 = rt_text(v1073_hi); wv t2115 = rt_text(v1074_st); rt_cat(9, (wv[]){ rt_s(&S645), t2112, rt_s(&S646), t2113, rt_s(&S419), t2114, rt_s(&S419), t2115, rt_s(&S632) }); })), "forStmt");
    RT_AT(5, 636);
    (void)rt_chk(s881_emit(up, ({ wv t2116 = rt_text(v1069_k); wv t2117 = rt_text(v1069_k); wv t2118 = rt_text(v1075_count); wv t2119 = rt_text(v1069_k); rt_cat(9, (wv[]){ rt_s(&S647), t2116, rt_s(&S648), t2117, rt_s(&S649), t2118, rt_s(&S650), t2119, rt_s(&S651) }); })), "forStmt");
    RT_AT(5, 637);
    up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 638);
    wv v1076_at = rt_bind((rt_cond(rt_get(&F_reversed, v1068_s)) ? ({ wv t2120 = rt_text(v1075_count); wv t2121 = rt_text(v1069_k); rt_cat(3, (wv[]){ t2120, rt_s(&S652), t2121 }); }) : v1069_k), 0);
    RT_AT(5, 639);
    (void)({ wv t2125 = v1070_first; wv t2126 = ({ wv t2122 = rt_text(v1072_lo); wv t2123 = rt_text(v1076_at); wv t2124 = rt_text(v1074_st); rt_cat(7, (wv[]){ rt_s(&S640), t2122, rt_s(&S653), t2123, rt_s(&S654), t2124, rt_s(&S655) }); }); rt_chk(s902_bindDecl(up, t2125, t2126), "forStmt"); });
  } else {
    RT_AT(5, 641);
    wv v1077_en = rt_bind(rt_chk(s888_enumOf(up, rt_get(&F_coll, v1068_s)), "forStmt"), 0);
    RT_AT(5, 642);
    if (rt_cond(rt_ne(v1077_en, RT_NULL))) {
      RT_AT(5, 644);
      wv v1078_count = rt_bind(std_length(rt_get(&F_members, rt_get(&F_node, v1077_en))), 0);
      RT_AT(5, 645);
      (void)rt_chk(s881_emit(up, ({ wv t2127 = rt_text(v1069_k); wv t2128 = rt_text(v1069_k); wv t2129 = rt_text(v1078_count); wv t2130 = rt_text(v1069_k); rt_cat(9, (wv[]){ rt_s(&S647), t2127, rt_s(&S648), t2128, rt_s(&S649), t2129, rt_s(&S650), t2130, rt_s(&S651) }); })), "forStmt");
      RT_AT(5, 646);
      up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
      RT_AT(5, 647);
      wv v1079_at = rt_bind((rt_cond(rt_get(&F_reversed, v1068_s)) ? ({ wv t2131 = rt_text(rt_sub(v1078_count, rt_i(1))); wv t2132 = rt_text(v1069_k); rt_cat(3, (wv[]){ t2131, rt_s(&S656), t2132 }); }) : v1069_k), 0);
      RT_AT(5, 648);
      if (rt_cond(rt_eq(v1071_second, RT_NULL))) {
        RT_AT(5, 649);
        (void)({ wv t2135 = v1070_first; wv t2136 = ({ wv t2133 = rt_text(rt_get(&F_c, v1077_en)); wv t2134 = rt_text(v1079_at); rt_cat(5, (wv[]){ rt_s(&S657), t2133, rt_s(&S99), t2134, rt_s(&S658) }); }); rt_chk(s902_bindDecl(up, t2135, t2136), "forStmt"); });
      } else {
        RT_AT(5, 651);
        (void)({ wv t2137 = v1070_first; wv t2138 = rt_cat(3, (wv[]){ rt_s(&S640), rt_text(v1079_at), rt_s(&S16) }); rt_chk(s902_bindDecl(up, t2137, t2138), "forStmt"); });
        RT_AT(5, 652);
        (void)({ wv t2141 = v1071_second; wv t2142 = ({ wv t2139 = rt_text(rt_get(&F_c, v1077_en)); wv t2140 = rt_text(v1079_at); rt_cat(5, (wv[]){ rt_s(&S657), t2139, rt_s(&S99), t2140, rt_s(&S658) }); }); rt_chk(s902_bindDecl(up, t2141, t2142), "forStmt"); });
      }
    } else {
      RT_AT(5, 657);
      wv v1080_fr = rt_bind(rt_chk(s882_tmp(up), "forStmt"), 0);
      RT_AT(5, 658);
      (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S659), rt_text(v1080_fr), rt_s(&S14) })), "forStmt");
      RT_AT(5, 659);
      (void)rt_chk(s881_emit(up, ({ wv t2143 = rt_text(v1080_fr); wv t2144 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_coll, v1068_s)), "forStmt"))); wv t2145 = rt_text((rt_cond(rt_get(&F_reversed, v1068_s)) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S660), t2143, rt_s(&S419), t2144, rt_s(&S419), t2145, rt_s(&S632) }); })), "forStmt");
      RT_AT(5, 660);
      wv v1081_path = rt_bind(rt_chk(s899_pathText(up, rt_get(&F_coll, v1068_s)), "forStmt"), 0);
      RT_AT(5, 661);
      wv v1082_guard = rt_bind((rt_cond(rt_eq(v1081_path, RT_NULL)) ? rt_s(&S13) : ({ wv t2146 = rt_text(v1080_fr); wv t2147 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_coll, v1068_s)), "forStmt"))); wv t2148 = rt_text(rt_chk(w6_cString(v1081_path), "forStmt")); rt_cat(7, (wv[]){ rt_s(&S661), t2146, rt_s(&S419), t2147, rt_s(&S419), t2148, rt_s(&S662) }); })), 0);
      RT_AT(5, 662);
      (void)rt_chk(s881_emit(up, ({ wv t2149 = rt_text(v1069_k); wv t2150 = rt_text(v1082_guard); wv t2151 = rt_text(v1080_fr); wv t2152 = rt_text(v1069_k); wv t2153 = rt_text(v1069_k); rt_cat(11, (wv[]){ rt_s(&S647), t2149, rt_s(&S648), t2150, rt_s(&S663), t2151, rt_s(&S419), t2152, rt_s(&S664), t2153, rt_s(&S651) }); })), "forStmt");
      RT_AT(5, 663);
      up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
      RT_AT(5, 664);
      if (rt_cond(rt_eq(v1071_second, RT_NULL))) {
        RT_AT(5, 665);
        (void)({ wv t2156 = v1070_first; wv t2157 = ({ wv t2154 = rt_text(v1080_fr); wv t2155 = rt_text(v1069_k); rt_cat(5, (wv[]){ rt_s(&S665), t2154, rt_s(&S419), t2155, rt_s(&S16) }); }); rt_chk(s902_bindDecl(up, t2156, t2157), "forStmt"); });
      } else {
        RT_AT(5, 667);
        (void)({ wv t2160 = v1070_first; wv t2161 = ({ wv t2158 = rt_text(v1080_fr); wv t2159 = rt_text(v1069_k); rt_cat(5, (wv[]){ rt_s(&S666), t2158, rt_s(&S419), t2159, rt_s(&S16) }); }); rt_chk(s902_bindDecl(up, t2160, t2161), "forStmt"); });
        RT_AT(5, 668);
        (void)({ wv t2164 = v1071_second; wv t2165 = ({ wv t2162 = rt_text(v1080_fr); wv t2163 = rt_text(v1069_k); rt_cat(5, (wv[]){ rt_s(&S667), t2162, rt_s(&S419), t2163, rt_s(&S16) }); }); rt_chk(s902_bindDecl(up, t2164, t2165), "forStmt"); });
      }
    }
  }
  RT_AT(5, 672);
  up->v850_loops = rt_add(rt_def(up->v850_loops, "loops"), rt_i(1));
  RT_AT(5, 673);
  (void)rt_chk(s896_blockBody(up, rt_get(&F_body, v1068_s)), "forStmt");
  RT_AT(5, 674);
  up->v850_loops = rt_sub(rt_def(up->v850_loops, "loops"), rt_i(1));
  RT_AT(5, 675);
  up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 676);
  (void)rt_chk(s881_emit(up, rt_s(&S15)), "forStmt");
  RT_AT(5, 677);
  up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 678);
  (void)rt_chk(s881_emit(up, rt_s(&S15)), "forStmt");
  RT_RET(RT_NULL);
}

static wv s901_signal(struct fr176 *up, wv v1083_code) {
  rt_enter("signal");
  v1083_code = rt_bind(v1083_code, 0);
  RT_AT(5, 683);
  (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S668), rt_text(v1083_code), rt_s(&S632) })), "signal");
  RT_AT(5, 684);
  (void)rt_chk(s881_emit(up, rt_s(&S669)), "signal");
  RT_RET(RT_NULL);
}

static wv s902_bindDecl(struct fr176 *up, wv v1084_d, wv v1085_value) {
  rt_enter("bindDecl");
  v1084_d = rt_bind(v1084_d, 0);
  v1085_value = rt_bind(v1085_value, 0);
  RT_AT(5, 689);
  wv v1086_owner = rt_bind(rt_get(&F_func, ({ wv t2166 = v1084_d; wv t2167 = rt_def(up->v852_decls, "decls"); rt_idx(t2166, t2167); })), 0);
  RT_AT(5, 690);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t2168 = v1086_owner; wv t2169 = rt_def(up->v853_fns, "fns"); rt_idx(t2168, t2169); }))), "&&") && rt_bool(({ wv t2170 = v1086_owner; wv t2171 = rt_def(up->v861_cur, "cur"); rt_eq(t2170, t2171); }), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_captured, ({ wv t2172 = v1084_d; wv t2173 = rt_def(up->v852_decls, "decls"); rt_idx(t2172, t2173); }))), "&&")))) {
    RT_AT(5, 691);
    (void)rt_chk(s881_emit(up, ({ wv t2176 = rt_text(rt_get(&F_cname, ({ wv t2174 = v1084_d; wv t2175 = rt_def(up->v852_decls, "decls"); rt_idx(t2174, t2175); }))); wv t2177 = rt_text(v1085_value); rt_cat(5, (wv[]){ rt_s(&S623), t2176, rt_s(&S624), t2177, rt_s(&S14) }); })), "bindDecl");
  } else {
    RT_AT(5, 693);
    (void)rt_chk(s881_emit(up, ({ wv t2178 = rt_text(rt_chk(s889_place(up, v1084_d), "bindDecl")); wv t2179 = rt_text(v1085_value); rt_cat(4, (wv[]){ t2178, rt_s(&S624), t2179, rt_s(&S14) }); })), "bindDecl");
  }
  RT_RET(RT_NULL);
}

static wv s903_fnDef(struct fr176 *up, wv v1087_s) {
  rt_enter("fnDef");
  v1087_s = rt_bind(v1087_s, 0);
  RT_AT(5, 698);
  wv v1088_d = rt_bind(({ wv t2180 = rt_get(&F_namePos, v1087_s); wv t2181 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2180, t2181); }), 0);
  RT_AT(5, 699);
  wv v1089_f = rt_bind(rt_get(&F_fnIndex, ({ wv t2182 = v1088_d; wv t2183 = rt_def(up->v852_decls, "decls"); rt_idx(t2182, t2183); })), 0);
  RT_AT(5, 700);
  wv v1090_pf = rt_bind(rt_get(&F_parent, ({ wv t2184 = v1089_f; wv t2185 = rt_def(up->v853_fns, "fns"); rt_idx(t2184, t2185); })), 0);
  RT_AT(5, 701);
  wv v1091_hasUp = rt_not(rt_get(&F_top, ({ wv t2186 = v1090_pf; wv t2187 = rt_def(up->v853_fns, "fns"); rt_idx(t2186, t2187); })));
  RT_AT(5, 702);
  wv v1092_params = rt_arr(0, NULL, 1);
  RT_AT(5, 703);
  if (rt_cond(v1091_hasUp)) {
    RT_AT(5, 703);
    (void)({ wv t2188 = v1092_params; wv t2189 = rt_cat(3, (wv[]){ rt_s(&S670), rt_text(v1090_pf), rt_s(&S671) }); std_push(t2188, t2189); });
  }
  RT_AT(5, 704);
  wv v1093_k = rt_i(0);
  RT_AT(5, 705);
  while (rt_cond((RT_AT(5, 705), ({ wv t2190 = v1093_k; wv t2191 = std_length(rt_get(&F_params, v1087_s)); rt_lt(t2190, t2191); })))) {
    RT_AT(5, 706);
    wv v1101_pd = rt_bind(({ wv t2194 = rt_get(&F_pos, ({ wv t2192 = v1093_k; wv t2193 = rt_get(&F_params, v1087_s); rt_idx(t2192, t2193); })); wv t2195 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2194, t2195); }), 0);
    RT_AT(5, 707);
    (void)({ wv t2202 = v1092_params; wv t2203 = ({ wv t2200 = rt_text((rt_cond(rt_get(&F_captured, ({ wv t2196 = v1101_pd; wv t2197 = rt_def(up->v852_decls, "decls"); rt_idx(t2196, t2197); }))) ? rt_s(&S672) : rt_s(&S13))); wv t2201 = rt_text(rt_get(&F_cname, ({ wv t2198 = v1101_pd; wv t2199 = rt_def(up->v852_decls, "decls"); rt_idx(t2198, t2199); }))); rt_cat(3, (wv[]){ rt_s(&S623), t2200, t2201 }); }); std_push(t2202, t2203); });
    RT_AT(5, 708);
    v1093_k = rt_add(v1093_k, rt_i(1));
  }
  RT_AT(5, 710);
  wv v1094_proto = ({ wv t2206 = rt_text(rt_get(&F_cname, ({ wv t2204 = v1089_f; wv t2205 = rt_def(up->v853_fns, "fns"); rt_idx(t2204, t2205); }))); wv t2207 = rt_text((rt_cond(rt_eq(std_length(v1092_params), rt_i(0))) ? rt_s(&S674) : std_join(v1092_params, rt_s(&S419)))); rt_cat(5, (wv[]){ rt_s(&S673), t2206, rt_s(&S98), t2207, rt_s(&S16) }); });
  RT_AT(5, 711);
  wv v1095_entry = ({ wv t2208 = v1094_proto; wv t2209 = rt_arr(0, NULL, 0); rt_rec(1, 2, K62, (wv[]){ t2208, t2209 }); });
  RT_AT(5, 712);
  (void)({ wv t2210 = rt_def(up->v847_functions, "functions"); wv t2211 = v1095_entry; std_push(t2210, t2211); });
  RT_AT(5, 713);
  if (rt_cond(rt_chk(s866_hasFrame(up, v1089_f), "fnDef"))) {
    RT_AT(5, 713);
    (void)({ wv t2212 = rt_def(up->v846_frames, "frames"); wv t2213 = v1089_f; std_push(t2212, t2213); });
  }
  RT_AT(5, 714);
  wv v1096_start = rt_bind(std_length(rt_def(up->v848_out, "out")), 0);
  RT_AT(5, 715);
  wv v1097_savedIndent = rt_bind(rt_def(up->v849_indent, "indent"), 0);
  RT_AT(5, 716);
  wv v1098_savedLoops = rt_bind(rt_def(up->v850_loops, "loops"), 0);
  RT_AT(5, 717);
  wv v1099_savedIife = rt_bind(rt_def(up->v851_iife, "iife"), 0);
  RT_AT(5, 718);
  wv v1100_savedCur = rt_bind(rt_def(up->v861_cur, "cur"), 0);
  RT_AT(5, 719);
  wv t2214 = rt_i(1);
  (void)rt_def(up->v849_indent, "indent");
  up->v849_indent = t2214;
  RT_AT(5, 720);
  wv t2215 = rt_i(0);
  (void)rt_def(up->v850_loops, "loops");
  up->v850_loops = t2215;
  RT_AT(5, 721);
  wv t2216 = rt_b(0);
  (void)rt_def(up->v851_iife, "iife");
  up->v851_iife = t2216;
  RT_AT(5, 722);
  wv t2217 = rt_bind(v1089_f, 1);
  (void)rt_def(up->v861_cur, "cur");
  up->v861_cur = t2217;
  RT_AT(5, 723);
  (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S675), rt_text(rt_chk(w1_quote(rt_get(&F_name, v1087_s)), "fnDef")), rt_s(&S632) })), "fnDef");
  RT_AT(5, 724);
  if (rt_cond(rt_chk(s866_hasFrame(up, v1089_f), "fnDef"))) {
    RT_AT(5, 724);
    (void)rt_chk(s881_emit(up, ({ wv t2218 = rt_text(v1089_f); wv t2219 = rt_text((rt_cond(v1091_hasUp) ? rt_s(&S677) : rt_s(&S4))); rt_cat(5, (wv[]){ rt_s(&S670), t2218, rt_s(&S676), t2219, rt_s(&S616) }); })), "fnDef");
  }
  RT_AT(5, 725);
  v1093_k = rt_i(0);
  RT_AT(5, 726);
  while (rt_cond((RT_AT(5, 726), ({ wv t2220 = v1093_k; wv t2221 = std_length(rt_get(&F_params, v1087_s)); rt_lt(t2220, t2221); })))) {
    RT_AT(5, 727);
    wv v1102_p = rt_bind(({ wv t2222 = v1093_k; wv t2223 = rt_get(&F_params, v1087_s); rt_idx(t2222, t2223); }), 0);
    RT_AT(5, 728);
    wv v1103_pd = rt_bind(({ wv t2224 = rt_get(&F_pos, v1102_p); wv t2225 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2224, t2225); }), 0);
    RT_AT(5, 729);
    wv v1104_cn = rt_bind(rt_get(&F_cname, ({ wv t2226 = v1103_pd; wv t2227 = rt_def(up->v852_decls, "decls"); rt_idx(t2226, t2227); })), 0);
    RT_AT(5, 731);
    wv v1105_arg = rt_bind((rt_cond(rt_get(&F_captured, ({ wv t2228 = v1103_pd; wv t2229 = rt_def(up->v852_decls, "decls"); rt_idx(t2228, t2229); }))) ? rt_cat(2, (wv[]){ rt_s(&S672), rt_text(v1104_cn) }) : v1104_cn), 0);
    RT_AT(5, 732);
    wv v1106_v = rt_bind((rt_cond(rt_get(&F_isRef, v1102_p)) ? v1105_arg : ({ wv t2230 = rt_text(v1105_arg); wv t2231 = rt_text((rt_cond(rt_get(&F_isVar, v1102_p)) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S678), t2230, rt_s(&S419), t2231, rt_s(&S16) }); })), 0);
    RT_AT(5, 733);
    if (rt_cond(rt_get(&F_captured, ({ wv t2232 = v1103_pd; wv t2233 = rt_def(up->v852_decls, "decls"); rt_idx(t2232, t2233); })))) {
      RT_AT(5, 733);
      (void)rt_chk(s881_emit(up, ({ wv t2234 = rt_text(v1104_cn); wv t2235 = rt_text(v1106_v); rt_cat(5, (wv[]){ rt_s(&S618), t2234, rt_s(&S624), t2235, rt_s(&S14) }); })), "fnDef");
    } else if (rt_cond(rt_not(rt_get(&F_isRef, v1102_p)))) {
      RT_AT(5, 733);
      (void)rt_chk(s881_emit(up, ({ wv t2236 = rt_text(v1104_cn); wv t2237 = rt_text(v1106_v); rt_cat(4, (wv[]){ t2236, rt_s(&S624), t2237, rt_s(&S14) }); })), "fnDef");
    }
    RT_AT(5, 734);
    v1093_k = rt_add(v1093_k, rt_i(1));
  }
  RT_AT(5, 736);
  (void)rt_chk(s894_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1087_s))), "fnDef");
  RT_AT(5, 737);
  (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S631), rt_text(rt_chk(s897_valueOf(up, rt_get(&F_body, v1087_s)), "fnDef")), rt_s(&S632) })), "fnDef");
  RT_AT(5, 738);
  ({ wv t2241 = ({ wv t2238 = rt_def(up->v848_out, "out"); wv t2239 = v1096_start; wv t2240 = std_length(rt_def(up->v848_out, "out")); std_slice(t2238, t2239, t2240); }); wv t2242 = v1095_entry; rt_set_mem(t2241, &F_lines, t2242); });
  RT_AT(5, 739);
  while (rt_cond((RT_AT(5, 739), ({ wv t2243 = std_length(rt_def(up->v848_out, "out")); wv t2244 = v1096_start; rt_gt(t2243, t2244); })))) {
    RT_AT(5, 739);
    (void)std_pop(rt_def(up->v848_out, "out"));
  }
  RT_AT(5, 740);
  wv t2245 = rt_bind(v1097_savedIndent, 1);
  (void)rt_def(up->v849_indent, "indent");
  up->v849_indent = t2245;
  RT_AT(5, 741);
  wv t2246 = rt_bind(v1098_savedLoops, 1);
  (void)rt_def(up->v850_loops, "loops");
  up->v850_loops = t2246;
  RT_AT(5, 742);
  wv t2247 = rt_bind(v1099_savedIife, 1);
  (void)rt_def(up->v851_iife, "iife");
  up->v851_iife = t2247;
  RT_AT(5, 743);
  wv t2248 = rt_bind(v1100_savedCur, 1);
  (void)rt_def(up->v861_cur, "cur");
  up->v861_cur = t2248;
  RT_RET(RT_NULL);
}

static wv s904_bindRhs(struct fr176 *up, wv v1107_e, wv v1108_mode) {
  rt_enter("bindRhs");
  v1107_e = rt_bind(v1107_e, 0);
  v1108_mode = rt_bind(v1108_mode, 0);
  RT_AT(5, 750);
  wv v1109_mutable = rt_eq(v1108_mode, rt_s(&S36));
  RT_AT(5, 751);
  if (rt_cond(rt_eq(rt_get(&F_k, v1107_e), rt_s(&S188)))) {
    RT_AT(5, 751);
    RT_RET(rt_get(&F_c, rt_chk(s907_array(up, v1107_e, v1109_mutable), "bindRhs")));
  }
  RT_AT(5, 752);
  if (rt_cond(rt_eq(rt_get(&F_k, v1107_e), rt_s(&S189)))) {
    RT_AT(5, 752);
    RT_RET(rt_get(&F_c, rt_chk(s908_mapLit(up, v1107_e, v1109_mutable), "bindRhs")));
  }
  RT_AT(5, 753);
  if (rt_cond(rt_eq(rt_get(&F_k, v1107_e), rt_s(&S190)))) {
    RT_AT(5, 753);
    RT_RET(rt_get(&F_c, rt_chk(s909_record(up, v1107_e, v1109_mutable), "bindRhs")));
  }
  RT_AT(5, 754);
  if (rt_cond(rt_b(rt_bool(({ wv t2249 = rt_def(w6_scalarKinds, "scalarKinds"); wv t2250 = rt_get(&F_k, v1107_e); rt_chk(w1_contains(t2249, t2250), "bindRhs"); }), "||") || rt_bool(rt_eq(v1108_mode, rt_s(&S37)), "||")))) {
    RT_AT(5, 754);
    RT_RET(rt_get(&F_c, rt_chk(s925_expr(up, v1107_e), "bindRhs")));
  }
  RT_RET((RT_AT(5, 755), ({ wv t2251 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, v1107_e), "bindRhs"))); wv t2252 = rt_text((rt_cond(v1109_mutable) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S678), t2251, rt_s(&S419), t2252, rt_s(&S16) }); })));
}

static wv s905_setStmt(struct fr176 *up, wv v1110_s) {
  rt_enter("setStmt");
  v1110_s = rt_bind(v1110_s, 0);
  RT_AT(5, 759);
  wv v1111_t = rt_bind(rt_get(&F_target, v1110_s), 0);
  RT_AT(5, 760);
  if (rt_cond(rt_eq(rt_get(&F_k, v1111_t), rt_s(&S214)))) {
    RT_AT(5, 761);
    wv v1118_d = rt_bind(({ wv t2253 = rt_get(&F_pos, v1111_t); wv t2254 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2253, t2254); }), 0);
    RT_AT(5, 762);
    wv v1119_at = rt_bind(rt_chk(s889_place(up, v1118_d), "setStmt"), 0);
    RT_AT(5, 763);
    if (rt_cond(rt_eq(rt_get(&F_op, v1110_s), RT_NULL))) {
      RT_AT(5, 764);
      wv v1122_v = rt_bind(({ wv t2257 = rt_get(&F_e, v1110_s); wv t2258 = (rt_cond(rt_get(&F_isRef, ({ wv t2255 = v1118_d; wv t2256 = rt_def(up->v852_decls, "decls"); rt_idx(t2255, t2256); }))) ? rt_s(&S37) : rt_s(&S36)); rt_chk(s904_bindRhs(up, t2257, t2258), "setStmt"); }), 0);
      RT_AT(5, 765);
      if (rt_cond(rt_not(rt_chk(s890_tdz(up, v1118_d), "setStmt")))) {
        RT_AT(5, 765);
        (void)rt_chk(s881_emit(up, ({ wv t2259 = rt_text(v1119_at); wv t2260 = rt_text(v1122_v); rt_cat(4, (wv[]){ t2259, rt_s(&S624), t2260, rt_s(&S14) }); })), "setStmt");
        RT_AT(5, 765);
        RT_RET(RT_NULL);
      }
      RT_AT(5, 767);
      wv v1123_temp = rt_bind(rt_chk(s882_tmp(up), "setStmt"), 0);
      RT_AT(5, 768);
      (void)rt_chk(s881_emit(up, ({ wv t2261 = rt_text(v1123_temp); wv t2262 = rt_text(v1122_v); rt_cat(5, (wv[]){ rt_s(&S623), t2261, rt_s(&S624), t2262, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 769);
      (void)rt_chk(s881_emit(up, ({ wv t2265 = rt_text(v1119_at); wv t2266 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t2263 = v1118_d; wv t2264 = rt_def(up->v852_decls, "decls"); rt_idx(t2263, t2264); }))), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S679), t2265, rt_s(&S419), t2266, rt_s(&S632) }); })), "setStmt");
      RT_AT(5, 770);
      (void)rt_chk(s881_emit(up, ({ wv t2267 = rt_text(v1119_at); wv t2268 = rt_text(v1123_temp); rt_cat(4, (wv[]){ t2267, rt_s(&S624), t2268, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 771);
      RT_RET(RT_NULL);
    }
    RT_AT(5, 774);
    wv v1120_v = rt_bind(rt_chk(s925_expr(up, rt_get(&F_e, v1110_s)), "setStmt"), 1);
    RT_AT(5, 775);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1110_s)), rt_s(&S139)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1110_s)), rt_s(&S214)), "&&")))) {
      RT_AT(5, 776);
      wv v1124_temp = rt_bind(rt_chk(s882_tmp(up), "setStmt"), 0);
      RT_AT(5, 777);
      (void)rt_chk(s881_emit(up, ({ wv t2269 = rt_text(v1124_temp); wv t2270 = rt_text(rt_get(&F_c, v1120_v)); rt_cat(5, (wv[]){ rt_s(&S623), t2269, rt_s(&S624), t2270, rt_s(&S14) }); })), "setStmt");
      RT_AT(5, 778);
      v1120_v = ({ wv t2271 = v1124_temp; wv t2272 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K60, (wv[]){ t2271, t2272 }); });
    }
    RT_AT(5, 780);
    wv v1121_sq = rt_bind(rt_chk(s892_seq(up, ({ wv t2273 = rt_chk(s891_read(up, v1118_d), "setStmt"); wv t2274 = v1120_v; rt_arr(2, (wv[]){ t2273, t2274 }, 0); })), "setStmt"), 0);
    RT_AT(5, 781);
    (void)rt_chk(s881_emit(up, ({ wv t2280 = rt_text(v1119_at); wv t2281 = rt_text(rt_get(&F_c, ({ wv t2278 = v1121_sq; wv t2279 = ({ wv t2275 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v1110_s)), "setStmt")); wv t2276 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1121_sq))); wv t2277 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1121_sq))); rt_cat(7, (wv[]){ rt_s(&S680), t2275, rt_s(&S98), t2276, rt_s(&S419), t2277, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2278, t2279), "setStmt"); }))); rt_cat(4, (wv[]){ t2280, rt_s(&S624), t2281, rt_s(&S14) }); })), "setStmt");
    RT_AT(5, 782);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 784);
  wv v1112_key = RT_NULL;
  RT_AT(5, 785);
  if (rt_cond(rt_eq(rt_get(&F_k, v1111_t), rt_s(&S192)))) {
    RT_AT(5, 785);
    v1112_key = ({ wv t2282 = rt_chk(s884_fieldRef(up, rt_get(&F_field, v1111_t)), "setStmt"); wv t2283 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K60, (wv[]){ t2282, t2283 }); });
  }
  RT_AT(5, 786);
  wv v1113_v = rt_bind(rt_chk(s925_expr(up, rt_get(&F_e, v1110_s)), "setStmt"), 0);
  RT_AT(5, 787);
  wv v1114_i = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1111_t), rt_s(&S192))) ? v1112_key : rt_chk(s925_expr(up, rt_get(&F_index, v1111_t)), "setStmt")), 0);
  RT_AT(5, 788);
  wv v1115_box = rt_bind(rt_chk(s925_expr(up, (rt_cond(rt_eq(rt_get(&F_k, v1111_t), rt_s(&S192))) ? rt_get(&F_rec, v1111_t) : rt_get(&F_arr, v1111_t))), "setStmt"), 0);
  RT_AT(5, 789);
  wv v1116_sq = rt_bind(rt_chk(s892_seq(up, rt_arr(3, (wv[]){ v1113_v, v1114_i, v1115_box }, 0)), "setStmt"), 0);
  RT_AT(5, 790);
  wv v1117_which = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1111_t), rt_s(&S192))) ? rt_s(&S192) : rt_s(&S191)), 0);
  RT_AT(5, 791);
  if (rt_cond(rt_eq(rt_get(&F_op, v1110_s), RT_NULL))) {
    RT_AT(5, 792);
    (void)rt_chk(s881_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2286 = v1116_sq; wv t2287 = ({ wv t2284 = rt_text(v1117_which); wv t2285 = rt_text(std_join(rt_get(&F_cs, v1116_sq), rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S681), t2284, rt_s(&S98), t2285, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2286, t2287), "setStmt"); }))), rt_s(&S14) })), "setStmt");
    RT_AT(5, 793);
    RT_RET(RT_NULL);
  }
  RT_AT(5, 795);
  (void)rt_chk(s881_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2291 = v1116_sq; wv t2292 = ({ wv t2288 = rt_text(v1117_which); wv t2289 = rt_text(std_join(rt_get(&F_cs, v1116_sq), rt_s(&S419))); wv t2290 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v1110_s)), "setStmt")); rt_cat(7, (wv[]){ rt_s(&S682), t2288, rt_s(&S98), t2289, rt_s(&S683), t2290, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2291, t2292), "setStmt"); }))), rt_s(&S14) })), "setStmt");
  RT_RET(RT_NULL);
}

static wv s906_exprs(struct fr176 *up, wv v1125_list) {
  rt_enter("exprs");
  v1125_list = rt_bind(v1125_list, 0);
  RT_AT(5, 799);
  wv v1126_xs = rt_arr(0, NULL, 1);
  RT_AT(5, 800);
  wv v1127_k = rt_i(0);
  RT_AT(5, 801);
  while (rt_cond((RT_AT(5, 801), ({ wv t2293 = v1127_k; wv t2294 = std_length(v1125_list); rt_lt(t2293, t2294); })))) {
    RT_AT(5, 802);
    (void)({ wv t2295 = v1126_xs; wv t2296 = rt_chk(s925_expr(up, rt_idx(v1127_k, v1125_list)), "exprs"); std_push(t2295, t2296); });
    RT_AT(5, 803);
    v1127_k = rt_add(v1127_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 805), v1126_xs));
}

static wv s907_array(struct fr176 *up, wv v1128_e, wv v1129_mutable) {
  rt_enter("array");
  v1128_e = rt_bind(v1128_e, 0);
  v1129_mutable = rt_bind(v1129_mutable, 0);
  RT_AT(5, 809);
  wv v1130_sq = rt_bind(rt_chk(s892_seq(up, rt_chk(s906_exprs(up, rt_get(&F_items, v1128_e)), "array")), "array"), 0);
  RT_RET((RT_AT(5, 810), ({ wv t2300 = v1130_sq; wv t2301 = ({ wv t2297 = rt_text(std_length(rt_get(&F_cs, v1130_sq))); wv t2298 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1130_sq)), "array")); wv t2299 = rt_text((rt_cond(v1129_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S684), t2297, rt_s(&S419), t2298, rt_s(&S419), t2299, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2300, t2301), "array"); })));
}

static wv s908_mapLit(struct fr176 *up, wv v1131_e, wv v1132_mutable) {
  rt_enter("mapLit");
  v1131_e = rt_bind(v1131_e, 0);
  v1132_mutable = rt_bind(v1132_mutable, 0);
  RT_AT(5, 815);
  wv v1133_sq = rt_bind(rt_chk(s892_seq(up, rt_chk(s906_exprs(up, rt_get(&F_items, v1131_e)), "mapLit")), "mapLit"), 0);
  RT_RET((RT_AT(5, 816), ({ wv t2305 = v1133_sq; wv t2306 = ({ wv t2302 = rt_text(std_floor(rt_div(std_length(rt_get(&F_cs, v1133_sq)), rt_i(2)))); wv t2303 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1133_sq)), "mapLit")); wv t2304 = rt_text((rt_cond(v1132_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S685), t2302, rt_s(&S419), t2303, rt_s(&S419), t2304, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2305, t2306), "mapLit"); })));
}

static wv s909_record(struct fr176 *up, wv v1134_e, wv v1135_mutable) {
  rt_enter("record");
  v1134_e = rt_bind(v1134_e, 0);
  v1135_mutable = rt_bind(v1135_mutable, 0);
  RT_AT(5, 821);
  wv v1136_shape = rt_s(&S606);
  RT_AT(5, 822);
  if (rt_cond(rt_gt(std_length(rt_get(&F_fields, v1134_e)), rt_i(0)))) {
    RT_AT(5, 823);
    wv v1140_names = rt_arr(0, NULL, 1);
    RT_AT(5, 824);
    wv v1141_k = rt_i(0);
    RT_AT(5, 825);
    while (rt_cond((RT_AT(5, 825), ({ wv t2307 = v1141_k; wv t2308 = std_length(rt_get(&F_fields, v1134_e)); rt_lt(t2307, t2308); })))) {
      RT_AT(5, 826);
      (void)({ wv t2311 = v1140_names; wv t2312 = rt_get(&F_name, ({ wv t2309 = v1141_k; wv t2310 = rt_get(&F_fields, v1134_e); rt_idx(t2309, t2310); })); std_push(t2311, t2312); });
      RT_AT(5, 827);
      v1141_k = rt_add(v1141_k, rt_i(1));
    }
    RT_AT(5, 829);
    wv v1142_key = rt_bind(std_join(v1140_names, rt_s(&S97)), 0);
    RT_AT(5, 830);
    v1141_k = rt_i(0);
    RT_AT(5, 831);
    while (rt_cond((RT_AT(5, 831), ({ wv t2313 = v1141_k; wv t2314 = std_length(v1140_names); rt_lt(t2313, t2314); })))) {
      RT_AT(5, 832);
      (void)rt_chk(s884_fieldRef(up, rt_idx(v1141_k, v1140_names)), "record");
      RT_AT(5, 833);
      v1141_k = rt_add(v1141_k, rt_i(1));
    }
    RT_AT(5, 835);
    v1141_k = rt_i(0);
    RT_AT(5, 836);
    while (rt_cond((RT_AT(5, 836), rt_b(rt_bool(({ wv t2315 = v1141_k; wv t2316 = std_length(rt_def(up->v843_shapes, "shapes")); rt_lt(t2315, t2316); }), "&&") && rt_bool(({ wv t2319 = ({ wv t2317 = v1141_k; wv t2318 = rt_def(up->v843_shapes, "shapes"); rt_idx(t2317, t2318); }); wv t2320 = v1142_key; rt_ne(t2319, t2320); }), "&&"))))) {
      RT_AT(5, 836);
      v1141_k = rt_add(v1141_k, rt_i(1));
    }
    RT_AT(5, 837);
    if (rt_cond(({ wv t2321 = v1141_k; wv t2322 = std_length(rt_def(up->v843_shapes, "shapes")); rt_eq(t2321, t2322); }))) {
      RT_AT(5, 837);
      (void)({ wv t2323 = rt_def(up->v843_shapes, "shapes"); wv t2324 = v1142_key; std_push(t2323, t2324); });
    }
    RT_AT(5, 838);
    v1136_shape = rt_cat(2, (wv[]){ rt_s(&S686), rt_text(v1141_k) });
  }
  RT_AT(5, 840);
  wv v1137_vals = rt_arr(0, NULL, 1);
  RT_AT(5, 841);
  wv v1138_k = rt_i(0);
  RT_AT(5, 842);
  while (rt_cond((RT_AT(5, 842), ({ wv t2325 = v1138_k; wv t2326 = std_length(rt_get(&F_fields, v1134_e)); rt_lt(t2325, t2326); })))) {
    RT_AT(5, 843);
    (void)({ wv t2329 = v1137_vals; wv t2330 = rt_chk(s925_expr(up, rt_get(&F_e, ({ wv t2327 = v1138_k; wv t2328 = rt_get(&F_fields, v1134_e); rt_idx(t2327, t2328); }))), "record"); std_push(t2329, t2330); });
    RT_AT(5, 844);
    v1138_k = rt_add(v1138_k, rt_i(1));
  }
  RT_AT(5, 846);
  wv v1139_sq = rt_bind(rt_chk(s892_seq(up, v1137_vals), "record"), 0);
  RT_RET((RT_AT(5, 847), ({ wv t2335 = v1139_sq; wv t2336 = ({ wv t2331 = rt_text((rt_cond(v1135_mutable) ? rt_i(1) : rt_i(0))); wv t2332 = rt_text(std_length(rt_get(&F_cs, v1139_sq))); wv t2333 = rt_text(v1136_shape); wv t2334 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1139_sq)), "record")); rt_cat(9, (wv[]){ rt_s(&S687), t2331, rt_s(&S419), t2332, rt_s(&S419), t2333, rt_s(&S419), t2334, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2335, t2336), "record"); })));
}

static wv s910_onfailStmt(struct fr176 *up, wv v1143_s) {
  rt_enter("onfailStmt");
  v1143_s = rt_bind(v1143_s, 0);
  RT_AT(5, 853);
  wv v1144_call = rt_bind(rt_get(&F_e, v1143_s), 0);
  RT_AT(5, 854);
  wv v1145_args = rt_arr(0, NULL, 1);
  RT_AT(5, 855);
  wv v1146_k = rt_i(0);
  RT_AT(5, 856);
  while (rt_cond((RT_AT(5, 856), ({ wv t2337 = v1146_k; wv t2338 = std_length(rt_get(&F_args, v1144_call)); rt_lt(t2337, t2338); })))) {
    RT_AT(5, 857);
    wv v1150_a = rt_bind(({ wv t2339 = v1146_k; wv t2340 = rt_get(&F_args, v1144_call); rt_idx(t2339, t2340); }), 0);
    RT_AT(5, 858);
    if (rt_cond(({ wv t2341 = rt_def(w6_plainKinds, "plainKinds"); wv t2342 = rt_get(&F_k, v1150_a); rt_chk(w1_contains(t2341, t2342), "onfailStmt"); }))) {
      RT_AT(5, 859);
      (void)({ wv t2343 = v1145_args; wv t2344 = rt_get(&F_c, rt_chk(s925_expr(up, v1150_a), "onfailStmt")); std_push(t2343, t2344); });
    } else {
      RT_AT(5, 861);
      wv v1151_temp = rt_bind(rt_chk(s882_tmp(up), "onfailStmt"), 0);
      RT_AT(5, 862);
      (void)rt_chk(s881_emit(up, ({ wv t2345 = rt_text(v1151_temp); wv t2346 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, v1150_a), "onfailStmt"))); rt_cat(5, (wv[]){ rt_s(&S623), t2345, rt_s(&S624), t2346, rt_s(&S14) }); })), "onfailStmt");
      RT_AT(5, 863);
      (void)std_push(v1145_args, v1151_temp);
    }
    RT_AT(5, 865);
    v1146_k = rt_add(v1146_k, rt_i(1));
  }
  RT_AT(5, 867);
  wv v1147_r = rt_bind(rt_chk(s882_tmp(up), "onfailStmt"), 0);
  RT_AT(5, 868);
  (void)rt_chk(s881_emit(up, ({ wv t2349 = rt_text(v1147_r); wv t2350 = rt_text(({ wv t2347 = rt_get(&F_callee, v1144_call); wv t2348 = v1145_args; rt_chk(s924_callC(up, t2347, t2348, rt_b(0)), "onfailStmt"); })); rt_cat(5, (wv[]){ rt_s(&S623), t2349, rt_s(&S624), t2350, rt_s(&S14) }); })), "onfailStmt");
  RT_AT(5, 869);
  (void)rt_chk(s881_emit(up, rt_s(&S688)), "onfailStmt");
  RT_AT(5, 870);
  up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 871);
  wv v1148_h = rt_bind(rt_get(&F_handler, v1143_s), 0);
  RT_AT(5, 872);
  if (rt_cond(rt_ne(rt_get(&F_param, v1148_h), RT_NULL))) {
    RT_AT(5, 872);
    (void)rt_chk(s902_bindDecl(up, ({ wv t2351 = rt_get(&F_paramPos, v1148_h); wv t2352 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2351, t2352); }), rt_s(&S689)), "onfailStmt");
  }
  RT_AT(5, 873);
  (void)rt_chk(s881_emit(up, rt_s(&S690)), "onfailStmt");
  RT_AT(5, 874);
  (void)rt_chk(s881_emit(up, rt_s(&S101)), "onfailStmt");
  RT_AT(5, 875);
  up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 876);
  (void)rt_chk(s895_resets(up, rt_get(&F_body, v1148_h)), "onfailStmt");
  RT_AT(5, 877);
  (void)rt_chk(s894_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1148_h))), "onfailStmt");
  RT_AT(5, 878);
  (void)rt_chk(s881_emit(up, ({ wv t2353 = rt_text(v1147_r); wv t2354 = rt_text(rt_chk(s897_valueOf(up, rt_get(&F_body, v1148_h)), "onfailStmt")); rt_cat(4, (wv[]){ t2353, rt_s(&S624), t2354, rt_s(&S14) }); })), "onfailStmt");
  RT_AT(5, 879);
  up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 880);
  (void)rt_chk(s881_emit(up, rt_s(&S15)), "onfailStmt");
  RT_AT(5, 881);
  up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 882);
  (void)rt_chk(s881_emit(up, rt_s(&S15)), "onfailStmt");
  RT_AT(5, 883);
  wv v1149_rest = rt_bind(v1143_s, 1);
  RT_AT(5, 884);
  rt_set_mem(RT_NULL, &F_handler, v1149_rest);
  RT_AT(5, 885);
  ({ wv t2357 = ({ wv t2355 = v1147_r; wv t2356 = rt_get(&F_pos, v1143_s); rt_rec(0, 4, K63, (wv[]){ rt_s(&S691), t2355, t2356, rt_b(0) }); }); wv t2358 = v1149_rest; rt_set_mem(t2357, &F_e, t2358); });
  RT_AT(5, 886);
  (void)rt_chk(s898_stmt(up, v1149_rest), "onfailStmt");
  RT_RET(RT_NULL);
}

static wv s911_chainStmt(struct fr176 *up, wv v1152_e) {
  rt_enter("chainStmt");
  v1152_e = rt_bind(v1152_e, 0);
  RT_AT(5, 890);
  wv v1153_k = rt_i(0);
  RT_AT(5, 891);
  while (rt_cond((RT_AT(5, 891), ({ wv t2359 = v1153_k; wv t2360 = std_length(rt_get(&F_branches, v1152_e)); rt_lt(t2359, t2360); })))) {
    RT_AT(5, 892);
    wv v1154_b = rt_bind(({ wv t2361 = v1153_k; wv t2362 = rt_get(&F_branches, v1152_e); rt_idx(t2361, t2362); }), 0);
    RT_AT(5, 893);
    (void)rt_chk(s881_emit(up, ({ wv t2363 = rt_text((rt_cond(rt_gt(v1153_k, rt_i(0))) ? rt_s(&S692) : rt_s(&S13))); wv t2364 = rt_text(rt_chk(s912_test(up, v1154_b), "chainStmt")); rt_cat(4, (wv[]){ t2363, rt_s(&S693), t2364, rt_s(&S694) }); })), "chainStmt");
    RT_AT(5, 894);
    up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 895);
    (void)rt_chk(s896_blockBody(up, rt_get(&F_body, v1154_b)), "chainStmt");
    RT_AT(5, 896);
    up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 897);
    v1153_k = rt_add(v1153_k, rt_i(1));
  }
  RT_AT(5, 899);
  if (rt_cond(rt_ne(rt_get(&F_els, v1152_e), RT_NULL))) {
    RT_AT(5, 900);
    (void)rt_chk(s881_emit(up, rt_s(&S695)), "chainStmt");
    RT_AT(5, 901);
    up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 902);
    (void)rt_chk(s896_blockBody(up, rt_get(&F_els, v1152_e)), "chainStmt");
    RT_AT(5, 903);
    up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
  }
  RT_AT(5, 905);
  (void)rt_chk(s881_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s912_test(struct fr176 *up, wv v1155_b) {
  rt_enter("test");
  v1155_b = rt_bind(v1155_b, 0);
  RT_RET((RT_AT(5, 908), ({ wv t2365 = rt_text((rt_cond(rt_get(&F_neg, v1155_b)) ? rt_s(&S95) : rt_s(&S13))); wv t2366 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_cond, v1155_b)), "test"))); rt_cat(4, (wv[]){ t2365, rt_s(&S638), t2366, rt_s(&S16) }); })));
}

static wv s913_branchCount(struct fr176 *up, wv v1156_e) {
  rt_enter("branchCount");
  v1156_e = rt_bind(v1156_e, 0);
  RT_RET((RT_AT(5, 913), (rt_cond(rt_eq(rt_get(&F_k, v1156_e), rt_s(&S186))) ? std_length(rt_get(&F_branches, v1156_e)) : std_length(rt_get(&F_cases, v1156_e)))));
}

static wv s914_branchBody(struct fr176 *up, wv v1157_e, wv v1158_i) {
  rt_enter("branchBody");
  v1157_e = rt_bind(v1157_e, 0);
  v1158_i = rt_bind(v1158_i, 0);
  RT_RET((RT_AT(5, 914), (rt_cond(rt_eq(rt_get(&F_k, v1157_e), rt_s(&S186))) ? rt_get(&F_body, ({ wv t2367 = v1158_i; wv t2368 = rt_get(&F_branches, v1157_e); rt_idx(t2367, t2368); })) : rt_get(&F_body, ({ wv t2369 = v1158_i; wv t2370 = rt_get(&F_cases, v1157_e); rt_idx(t2369, t2370); })))));
}

static wv s915_branchTest(struct fr176 *up, wv v1159_e, wv v1160_t, wv v1161_i) {
  rt_enter("branchTest");
  v1159_e = rt_bind(v1159_e, 0);
  v1160_t = rt_bind(v1160_t, 0);
  v1161_i = rt_bind(v1161_i, 0);
  RT_RET((RT_AT(5, 915), (rt_cond(rt_eq(rt_get(&F_k, v1159_e), rt_s(&S186))) ? rt_chk(s912_test(up, ({ wv t2371 = v1161_i; wv t2372 = rt_get(&F_branches, v1159_e); rt_idx(t2371, t2372); })), "branchTest") : ({ wv t2375 = v1160_t; wv t2376 = ({ wv t2373 = v1161_i; wv t2374 = rt_get(&F_cases, v1159_e); rt_idx(t2373, t2374); }); rt_chk(s920_caseTest(up, t2375, t2376), "branchTest"); }))));
}

static wv s916_isPlain(struct fr176 *up, wv v1162_e) {
  rt_enter("isPlain");
  v1162_e = rt_bind(v1162_e, 0);
  RT_AT(5, 919);
  wv v1163_k = rt_i(0);
  RT_AT(5, 920);
  while (rt_cond((RT_AT(5, 920), ({ wv t2377 = v1163_k; wv t2378 = rt_chk(s913_branchCount(up, v1162_e), "isPlain"); rt_le(t2377, t2378); })))) {
    RT_AT(5, 921);
    wv v1164_b = rt_bind((rt_cond(({ wv t2379 = v1163_k; wv t2380 = rt_chk(s913_branchCount(up, v1162_e), "isPlain"); rt_lt(t2379, t2380); })) ? rt_chk(s914_branchBody(up, v1162_e, v1163_k), "isPlain") : rt_get(&F_els, v1162_e)), 0);
    RT_AT(5, 922);
    wv v1165_j = rt_i(0);
    RT_AT(5, 923);
    while (rt_cond((RT_AT(5, 923), ({ wv t2381 = v1165_j; wv t2382 = std_length(rt_get(&F_stmts, v1164_b)); rt_lt(t2381, t2382); })))) {
      RT_AT(5, 924);
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t2383 = v1165_j; wv t2384 = rt_get(&F_stmts, v1164_b); rt_idx(t2383, t2384); })), rt_s(&S203)))) {
        RT_AT(5, 924);
        RT_RET(rt_b(0));
      }
      RT_AT(5, 925);
      v1165_j = rt_add(v1165_j, rt_i(1));
    }
    RT_AT(5, 927);
    v1163_k = rt_add(v1163_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 929), rt_b(1)));
}

static wv s917_ternaryChain(struct fr176 *up, wv v1166_e, wv v1167_t) {
  rt_enter("ternaryChain");
  v1166_e = rt_bind(v1166_e, 0);
  v1167_t = rt_bind(v1167_t, 0);
  RT_AT(5, 934);
  wv v1168_code = rt_bind(rt_chk(s897_valueOf(up, rt_get(&F_els, v1166_e)), "ternaryChain"), 1);
  RT_AT(5, 935);
  wv v1169_k = rt_sub(rt_chk(s913_branchCount(up, v1166_e), "ternaryChain"), rt_i(1));
  RT_AT(5, 936);
  while (rt_cond((RT_AT(5, 936), rt_ge(v1169_k, rt_i(0))))) {
    RT_AT(5, 937);
    v1168_code = ({ wv t2385 = rt_text(rt_chk(s915_branchTest(up, v1166_e, v1167_t, v1169_k), "ternaryChain")); wv t2386 = rt_text(rt_chk(s897_valueOf(up, rt_chk(s914_branchBody(up, v1166_e, v1169_k), "ternaryChain")), "ternaryChain")); wv t2387 = rt_text(v1168_code); rt_cat(7, (wv[]){ rt_s(&S98), t2385, rt_s(&S696), t2386, rt_s(&S697), t2387, rt_s(&S16) }); });
    RT_AT(5, 938);
    v1169_k = rt_sub(v1169_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 940), v1168_code));
}

static wv s918_branchValue(struct fr176 *up, wv v1170_e, wv v1171_t, wv v1172_subject) {
  rt_enter("branchValue");
  v1170_e = rt_bind(v1170_e, 0);
  v1171_t = rt_bind(v1171_t, 0);
  v1172_subject = rt_bind(v1172_subject, 0);
  RT_AT(5, 947);
  wv v1173_start = rt_bind(std_length(rt_def(up->v848_out, "out")), 0);
  RT_AT(5, 948);
  wv v1174_savedIndent = rt_bind(rt_def(up->v849_indent, "indent"), 0);
  RT_AT(5, 949);
  wv v1175_savedLoops = rt_bind(rt_def(up->v850_loops, "loops"), 0);
  RT_AT(5, 950);
  wv v1176_savedIife = rt_bind(rt_def(up->v851_iife, "iife"), 0);
  RT_AT(5, 951);
  wv t2388 = rt_add(v1174_savedIndent, rt_i(1));
  (void)rt_def(up->v849_indent, "indent");
  up->v849_indent = t2388;
  RT_AT(5, 952);
  wv t2389 = rt_i(0);
  (void)rt_def(up->v850_loops, "loops");
  up->v850_loops = t2389;
  RT_AT(5, 953);
  wv t2390 = rt_b(1);
  (void)rt_def(up->v851_iife, "iife");
  up->v851_iife = t2390;
  RT_AT(5, 954);
  if (rt_cond(rt_ne(v1172_subject, RT_NULL))) {
    RT_AT(5, 954);
    (void)rt_chk(s881_emit(up, ({ wv t2391 = rt_text(v1171_t); wv t2392 = rt_text(v1172_subject); rt_cat(5, (wv[]){ rt_s(&S623), t2391, rt_s(&S624), t2392, rt_s(&S14) }); })), "branchValue");
  }
  RT_AT(5, 955);
  wv v1177_r = rt_bind(rt_chk(s882_tmp(up), "branchValue"), 0);
  RT_AT(5, 956);
  (void)rt_chk(s881_emit(up, rt_cat(3, (wv[]){ rt_s(&S623), rt_text(v1177_r), rt_s(&S14) })), "branchValue");
  RT_AT(5, 957);
  wv v1178_k = rt_i(0);
  RT_AT(5, 958);
  while (rt_cond((RT_AT(5, 958), ({ wv t2393 = v1178_k; wv t2394 = rt_chk(s913_branchCount(up, v1170_e), "branchValue"); rt_lt(t2393, t2394); })))) {
    RT_AT(5, 959);
    wv v1180_b = rt_bind(rt_chk(s914_branchBody(up, v1170_e, v1178_k), "branchValue"), 0);
    RT_AT(5, 960);
    (void)rt_chk(s881_emit(up, ({ wv t2395 = rt_text((rt_cond(rt_gt(v1178_k, rt_i(0))) ? rt_s(&S692) : rt_s(&S13))); wv t2396 = rt_text(rt_chk(s915_branchTest(up, v1170_e, v1171_t, v1178_k), "branchValue")); rt_cat(4, (wv[]){ t2395, rt_s(&S693), t2396, rt_s(&S694) }); })), "branchValue");
    RT_AT(5, 961);
    up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 962);
    (void)rt_chk(s895_resets(up, v1180_b), "branchValue");
    RT_AT(5, 963);
    (void)rt_chk(s894_stmts(up, rt_get(&F_stmts, v1180_b)), "branchValue");
    RT_AT(5, 964);
    (void)rt_chk(s881_emit(up, ({ wv t2397 = rt_text(v1177_r); wv t2398 = rt_text(rt_chk(s897_valueOf(up, v1180_b), "branchValue")); rt_cat(4, (wv[]){ t2397, rt_s(&S624), t2398, rt_s(&S14) }); })), "branchValue");
    RT_AT(5, 965);
    up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 966);
    v1178_k = rt_add(v1178_k, rt_i(1));
  }
  RT_AT(5, 968);
  (void)rt_chk(s881_emit(up, rt_s(&S695)), "branchValue");
  RT_AT(5, 969);
  up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 970);
  (void)rt_chk(s895_resets(up, rt_get(&F_els, v1170_e)), "branchValue");
  RT_AT(5, 971);
  (void)rt_chk(s894_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v1170_e))), "branchValue");
  RT_AT(5, 972);
  (void)rt_chk(s881_emit(up, ({ wv t2399 = rt_text(v1177_r); wv t2400 = rt_text(rt_chk(s897_valueOf(up, rt_get(&F_els, v1170_e)), "branchValue")); rt_cat(4, (wv[]){ t2399, rt_s(&S624), t2400, rt_s(&S14) }); })), "branchValue");
  RT_AT(5, 973);
  up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
  RT_AT(5, 974);
  (void)rt_chk(s881_emit(up, rt_s(&S15)), "branchValue");
  RT_AT(5, 975);
  (void)rt_chk(s881_emit(up, rt_cat(2, (wv[]){ rt_text(v1177_r), rt_s(&S14) })), "branchValue");
  RT_AT(5, 976);
  wv v1179_body = rt_bind(std_join(({ wv t2401 = rt_def(up->v848_out, "out"); wv t2402 = v1173_start; wv t2403 = std_length(rt_def(up->v848_out, "out")); std_slice(t2401, t2402, t2403); }), rt_s(&S2)), 0);
  RT_AT(5, 977);
  while (rt_cond((RT_AT(5, 977), ({ wv t2404 = std_length(rt_def(up->v848_out, "out")); wv t2405 = v1173_start; rt_gt(t2404, t2405); })))) {
    RT_AT(5, 977);
    (void)std_pop(rt_def(up->v848_out, "out"));
  }
  RT_AT(5, 978);
  wv t2406 = rt_bind(v1174_savedIndent, 1);
  (void)rt_def(up->v849_indent, "indent");
  up->v849_indent = t2406;
  RT_AT(5, 979);
  wv t2407 = rt_bind(v1175_savedLoops, 1);
  (void)rt_def(up->v850_loops, "loops");
  up->v850_loops = t2407;
  RT_AT(5, 980);
  wv t2408 = rt_bind(v1176_savedIife, 1);
  (void)rt_def(up->v851_iife, "iife");
  up->v851_iife = t2408;
  RT_RET((RT_AT(5, 981), ({ wv t2409 = rt_text(v1179_body); wv t2410 = rt_text(rt_chk(w6_repeat(rt_s(&S611), rt_def(up->v849_indent, "indent")), "branchValue")); rt_cat(5, (wv[]){ rt_s(&S698), t2409, rt_s(&S2), t2410, rt_s(&S699) }); })));
}

static wv s919_chainExpr(struct fr176 *up, wv v1181_e) {
  rt_enter("chainExpr");
  v1181_e = rt_bind(v1181_e, 0);
  RT_AT(5, 987);
  if (rt_cond(rt_chk(s916_isPlain(up, v1181_e), "chainExpr"))) {
    RT_AT(5, 987);
    RT_RET(rt_chk(s917_ternaryChain(up, v1181_e, RT_NULL), "chainExpr"));
  }
  RT_RET((RT_AT(5, 988), rt_chk(s918_branchValue(up, v1181_e, RT_NULL, RT_NULL), "chainExpr")));
}

static wv s920_caseTest(struct fr176 *up, wv v1182_t, wv v1183_a) {
  rt_enter("caseTest");
  v1182_t = rt_bind(v1182_t, 0);
  v1183_a = rt_bind(v1183_a, 0);
  RT_AT(5, 994);
  wv v1184_tests = rt_arr(0, NULL, 1);
  RT_AT(5, 995);
  wv v1185_k = rt_i(0);
  RT_AT(5, 996);
  while (rt_cond((RT_AT(5, 996), ({ wv t2411 = v1185_k; wv t2412 = std_length(rt_get(&F_values, v1183_a)); rt_lt(t2411, t2412); })))) {
    RT_AT(5, 997);
    (void)({ wv t2417 = v1184_tests; wv t2418 = ({ wv t2415 = rt_text(v1182_t); wv t2416 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, ({ wv t2413 = v1185_k; wv t2414 = rt_get(&F_values, v1183_a); rt_idx(t2413, t2414); })), "caseTest"))); rt_cat(5, (wv[]){ rt_s(&S700), t2415, rt_s(&S419), t2416, rt_s(&S16) }); }); std_push(t2417, t2418); });
    RT_AT(5, 998);
    v1185_k = rt_add(v1185_k, rt_i(1));
  }
  RT_RET((RT_AT(5, 1000), std_join(v1184_tests, rt_s(&S701))));
}

static wv s921_matchElse(struct fr176 *up, wv v1186_e, wv v1187_t) {
  rt_enter("matchElse");
  v1186_e = rt_bind(v1186_e, 0);
  v1187_t = rt_bind(v1187_t, 0);
  RT_AT(5, 1006);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v1186_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_cases, v1186_e))))), rt_s(&S192)), "||")))) {
    RT_AT(5, 1006);
    RT_RET(rt_get(&F_els, v1186_e));
  }
  RT_RET((RT_AT(5, 1007), ({ wv t2419 = rt_arr(0, NULL, 0); wv t2420 = rt_rec(0, 4, K63, (wv[]){ rt_s(&S691), rt_cat(3, (wv[]){ rt_s(&S702), rt_text(v1187_t), rt_s(&S703) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K22, (wv[]){ t2419, t2420, RT_NULL }); })));
}

static wv s922_matchStmt(struct fr176 *up, wv v1188_e) {
  rt_enter("matchStmt");
  v1188_e = rt_bind(v1188_e, 0);
  RT_AT(5, 1011);
  wv v1189_t = rt_bind(rt_chk(s882_tmp(up), "matchStmt"), 0);
  RT_AT(5, 1012);
  (void)rt_chk(s881_emit(up, ({ wv t2421 = rt_text(v1189_t); wv t2422 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_subject, v1188_e)), "matchStmt"))); rt_cat(5, (wv[]){ rt_s(&S623), t2421, rt_s(&S624), t2422, rt_s(&S14) }); })), "matchStmt");
  RT_AT(5, 1013);
  wv v1190_k = rt_i(0);
  RT_AT(5, 1014);
  while (rt_cond((RT_AT(5, 1014), ({ wv t2423 = v1190_k; wv t2424 = std_length(rt_get(&F_cases, v1188_e)); rt_lt(t2423, t2424); })))) {
    RT_AT(5, 1015);
    (void)rt_chk(s881_emit(up, ({ wv t2429 = rt_text((rt_cond(rt_gt(v1190_k, rt_i(0))) ? rt_s(&S692) : rt_s(&S13))); wv t2430 = rt_text(({ wv t2427 = v1189_t; wv t2428 = ({ wv t2425 = v1190_k; wv t2426 = rt_get(&F_cases, v1188_e); rt_idx(t2425, t2426); }); rt_chk(s920_caseTest(up, t2427, t2428), "matchStmt"); })); rt_cat(4, (wv[]){ t2429, rt_s(&S693), t2430, rt_s(&S694) }); })), "matchStmt");
    RT_AT(5, 1016);
    up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 1017);
    (void)rt_chk(s896_blockBody(up, rt_get(&F_body, ({ wv t2431 = v1190_k; wv t2432 = rt_get(&F_cases, v1188_e); rt_idx(t2431, t2432); }))), "matchStmt");
    RT_AT(5, 1018);
    up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 1019);
    v1190_k = rt_add(v1190_k, rt_i(1));
  }
  RT_AT(5, 1021);
  wv v1191_els = rt_bind(rt_chk(s921_matchElse(up, v1188_e, v1189_t), "matchStmt"), 0);
  RT_AT(5, 1022);
  if (rt_cond(rt_ne(v1191_els, RT_NULL))) {
    RT_AT(5, 1023);
    (void)rt_chk(s881_emit(up, rt_s(&S695)), "matchStmt");
    RT_AT(5, 1024);
    up->v849_indent = rt_add(rt_def(up->v849_indent, "indent"), rt_i(1));
    RT_AT(5, 1025);
    (void)rt_chk(s896_blockBody(up, v1191_els), "matchStmt");
    RT_AT(5, 1026);
    up->v849_indent = rt_sub(rt_def(up->v849_indent, "indent"), rt_i(1));
  }
  RT_AT(5, 1028);
  (void)rt_chk(s881_emit(up, rt_s(&S15)), "matchStmt");
  RT_RET(RT_NULL);
}

static wv s923_matchExpr(struct fr176 *up, wv v1192_e) {
  rt_enter("matchExpr");
  v1192_e = rt_bind(v1192_e, 0);
  RT_AT(5, 1034);
  wv v1193_t = rt_bind(rt_chk(s882_tmp(up), "matchExpr"), 0);
  RT_AT(5, 1035);
  wv v1194_subject = rt_bind(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_subject, v1192_e)), "matchExpr")), 0);
  RT_AT(5, 1036);
  wv v1195_withElse = rt_bind(v1192_e, 1);
  RT_AT(5, 1037);
  ({ wv t2433 = rt_chk(s921_matchElse(up, v1192_e, v1193_t), "matchExpr"); wv t2434 = v1195_withElse; rt_set_mem(t2433, &F_els, t2434); });
  RT_AT(5, 1038);
  if (rt_cond(rt_chk(s916_isPlain(up, v1195_withElse), "matchExpr"))) {
    RT_AT(5, 1038);
    RT_RET(({ wv t2435 = rt_text(v1193_t); wv t2436 = rt_text(v1194_subject); wv t2437 = rt_text(rt_chk(s917_ternaryChain(up, v1195_withElse, v1193_t), "matchExpr")); rt_cat(7, (wv[]){ rt_s(&S704), t2435, rt_s(&S624), t2436, rt_s(&S650), t2437, rt_s(&S626) }); }));
  }
  RT_RET((RT_AT(5, 1039), rt_chk(s918_branchValue(up, v1195_withElse, v1193_t, v1194_subject), "matchExpr")));
}

static wv s924_callC(struct fr176 *up, wv v1196_callee, wv v1197_args, wv v1198_checked) {
  rt_enter("callC");
  v1196_callee = rt_bind(v1196_callee, 0);
  v1198_checked = rt_bind(v1198_checked, 0);
  RT_AT(5, 1044);
  wv v1199_code = rt_s(&S13);
  RT_AT(5, 1045);
  if (rt_cond(rt_eq(rt_get(&F_k, v1196_callee), rt_s(&S157)))) {
    RT_AT(5, 1046);
    v1199_code = ({ wv t2438 = rt_text(rt_get(&F_name, v1196_callee)); wv t2439 = rt_text(std_join(v1197_args, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S705), t2438, rt_s(&S98), t2439, rt_s(&S16) }); });
    RT_AT(5, 1047);
    if (rt_cond(rt_not(({ wv t2440 = rt_def(w6_signalers, "signalers"); wv t2441 = rt_get(&F_name, v1196_callee); rt_chk(w1_contains(t2440, t2441), "callC"); })))) {
      RT_AT(5, 1047);
      RT_RET(v1199_code);
    }
  } else {
    RT_AT(5, 1049);
    wv v1200_f = rt_s(&S13);
    RT_AT(5, 1050);
    wv v1201_parent = RT_NULL;
    RT_AT(5, 1051);
    if (rt_cond(rt_eq(rt_get(&F_k, v1196_callee), rt_s(&S192)))) {
      RT_AT(5, 1052);
      v1200_f = ({ wv t2442 = rt_text(rt_chk(s865_modId(up, rt_chk(s886_stolenFrom(up, rt_get(&F_rec, v1196_callee)), "callC")), "callC")); wv t2443 = rt_text(rt_get(&F_field, v1196_callee)); rt_cat(4, (wv[]){ rt_s(&S609), t2442, rt_s(&S12), t2443 }); });
    } else {
      RT_AT(5, 1054);
      wv v1204_d = rt_bind(({ wv t2444 = rt_get(&F_pos, v1196_callee); wv t2445 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2444, t2445); }), 0);
      RT_AT(5, 1055);
      wv v1205_sf = rt_bind(rt_get(&F_fnIndex, ({ wv t2446 = v1204_d; wv t2447 = rt_def(up->v852_decls, "decls"); rt_idx(t2446, t2447); })), 0);
      RT_AT(5, 1056);
      v1200_f = rt_bind(rt_get(&F_cname, ({ wv t2448 = v1205_sf; wv t2449 = rt_def(up->v853_fns, "fns"); rt_idx(t2448, t2449); })), 1);
      RT_AT(5, 1057);
      wv v1206_pf = rt_bind(rt_get(&F_parent, ({ wv t2450 = v1205_sf; wv t2451 = rt_def(up->v853_fns, "fns"); rt_idx(t2450, t2451); })), 0);
      RT_AT(5, 1058);
      if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2452 = v1206_pf; wv t2453 = rt_def(up->v853_fns, "fns"); rt_idx(t2452, t2453); }))))) {
        RT_AT(5, 1058);
        v1201_parent = rt_bind(v1206_pf, 1);
      }
    }
    RT_AT(5, 1061);
    wv v1202_all = rt_arr(0, NULL, 1);
    RT_AT(5, 1062);
    if (rt_cond(rt_ne(v1201_parent, RT_NULL))) {
      RT_AT(5, 1063);
      (void)({ wv t2462 = v1202_all; wv t2463 = (rt_cond(({ wv t2454 = v1201_parent; wv t2455 = rt_def(up->v861_cur, "cur"); rt_eq(t2454, t2455); })) ? rt_s(&S706) : rt_cat(2, (wv[]){ rt_s(&S619), rt_text(rt_chk(w6_repeat(rt_s(&S620), rt_sub(({ wv t2460 = rt_get(&F_depth, ({ wv t2456 = rt_def(up->v861_cur, "cur"); wv t2457 = rt_def(up->v853_fns, "fns"); rt_idx(t2456, t2457); })); wv t2461 = rt_get(&F_depth, ({ wv t2458 = v1201_parent; wv t2459 = rt_def(up->v853_fns, "fns"); rt_idx(t2458, t2459); })); rt_sub(t2460, t2461); }), rt_i(1))), "callC")) })); std_push(t2462, t2463); });
    }
    RT_AT(5, 1065);
    wv v1203_k = rt_i(0);
    RT_AT(5, 1066);
    while (rt_cond((RT_AT(5, 1066), ({ wv t2464 = v1203_k; wv t2465 = std_length(v1197_args); rt_lt(t2464, t2465); })))) {
      RT_AT(5, 1066);
      (void)({ wv t2466 = v1202_all; wv t2467 = rt_idx(v1203_k, v1197_args); std_push(t2466, t2467); });
      RT_AT(5, 1066);
      v1203_k = rt_add(v1203_k, rt_i(1));
    }
    RT_AT(5, 1067);
    v1199_code = ({ wv t2468 = rt_text(v1200_f); wv t2469 = rt_text(std_join(v1202_all, rt_s(&S419))); rt_cat(4, (wv[]){ t2468, rt_s(&S98), t2469, rt_s(&S16) }); });
  }
  RT_RET((RT_AT(5, 1069), (rt_cond(v1198_checked) ? ({ wv t2474 = rt_text(v1199_code); wv t2475 = rt_text((rt_cond(rt_get(&F_top, ({ wv t2470 = rt_def(up->v861_cur, "cur"); wv t2471 = rt_def(up->v853_fns, "fns"); rt_idx(t2470, t2471); }))) ? rt_s(&S606) : rt_chk(w1_quote(rt_get(&F_name, ({ wv t2472 = rt_def(up->v861_cur, "cur"); wv t2473 = rt_def(up->v853_fns, "fns"); rt_idx(t2472, t2473); }))), "callC"))); rt_cat(5, (wv[]){ rt_s(&S707), t2474, rt_s(&S419), t2475, rt_s(&S16) }); }) : v1199_code)));
}

static wv s925_expr(struct fr176 *up, wv v1207_e) {
  rt_enter("expr");
  RT_AT(5, 1073);
  wv v1208_kind = rt_bind(rt_get(&F_k, v1207_e), 0);
  RT_AT(5, 1074);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S139)))) {
    RT_AT(5, 1074);
    RT_RET(({ wv t2476 = rt_cat(3, (wv[]){ rt_s(&S640), rt_text(rt_get(&F_v, v1207_e)), rt_s(&S16) }); wv t2477 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2476, t2477 }); }));
  }
  RT_AT(5, 1075);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S149)))) {
    RT_AT(5, 1075);
    RT_RET(({ wv t2478 = rt_cat(3, (wv[]){ rt_s(&S708), rt_text(rt_get(&F_v, v1207_e)), rt_s(&S16) }); wv t2479 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2478, t2479 }); }));
  }
  RT_AT(5, 1076);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S173)))) {
    RT_AT(5, 1077);
    wv v1209_parts = rt_arr(0, NULL, 1);
    RT_AT(5, 1078);
    wv v1210_k = rt_i(0);
    RT_AT(5, 1079);
    while (rt_cond((RT_AT(5, 1079), ({ wv t2480 = v1210_k; wv t2481 = std_length(rt_get(&F_parts, v1207_e)); rt_lt(t2480, t2481); })))) {
      RT_AT(5, 1080);
      wv v1212_part = rt_bind(({ wv t2482 = v1210_k; wv t2483 = rt_get(&F_parts, v1207_e); rt_idx(t2482, t2483); }), 0);
      RT_AT(5, 1081);
      if (rt_cond(rt_eq(std_typeOf(v1212_part), rt_s(&S187)))) {
        RT_AT(5, 1082);
        if (rt_cond(rt_ne(v1212_part, rt_s(&S13)))) {
          RT_AT(5, 1082);
          (void)({ wv t2486 = v1209_parts; wv t2487 = ({ wv t2484 = rt_cat(3, (wv[]){ rt_s(&S709), rt_text(rt_chk(s883_str(up, v1212_part), "expr")), rt_s(&S16) }); wv t2485 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2484, t2485 }); }); std_push(t2486, t2487); });
        }
      } else {
        RT_AT(5, 1084);
        (void)({ wv t2490 = v1209_parts; wv t2491 = ({ wv t2488 = rt_cat(3, (wv[]){ rt_s(&S710), rt_text(rt_get(&F_c, rt_chk(s925_expr(up, v1212_part), "expr"))), rt_s(&S16) }); wv t2489 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2488, t2489 }); }); std_push(t2490, t2491); });
      }
      RT_AT(5, 1086);
      v1210_k = rt_add(v1210_k, rt_i(1));
    }
    RT_AT(5, 1088);
    if (rt_cond(rt_eq(std_length(v1209_parts), rt_i(0)))) {
      RT_AT(5, 1088);
      RT_RET(({ wv t2492 = rt_cat(3, (wv[]){ rt_s(&S709), rt_text(rt_chk(s883_str(up, rt_s(&S13)), "expr")), rt_s(&S16) }); wv t2493 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2492, t2493 }); }));
    }
    RT_AT(5, 1089);
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v1209_parts), rt_i(1)), "&&") && rt_bool(({ wv t2494 = rt_get(&F_k, rt_idx(rt_i(0), v1209_parts)); wv t2495 = rt_def(w6_constK, "constK"); rt_eq(t2494, t2495); }), "&&")))) {
      RT_AT(5, 1089);
      RT_RET(rt_idx(rt_i(0), v1209_parts));
    }
    RT_AT(5, 1090);
    wv v1211_sq = rt_bind(rt_chk(s892_seq(up, v1209_parts), "expr"), 0);
    RT_AT(5, 1091);
    RT_RET(({ wv t2498 = v1211_sq; wv t2499 = ({ wv t2496 = rt_text(std_length(rt_get(&F_cs, v1211_sq))); wv t2497 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v1211_sq)), "expr")); rt_cat(5, (wv[]){ rt_s(&S711), t2496, rt_s(&S419), t2497, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2498, t2499), "expr"); }));
  }
  RT_AT(5, 1093);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S300)))) {
    RT_AT(5, 1093);
    RT_RET(({ wv t2500 = rt_cat(3, (wv[]){ rt_s(&S712), rt_text((rt_cond(rt_get(&F_v, v1207_e)) ? rt_i(1) : rt_i(0))), rt_s(&S16) }); wv t2501 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2500, t2501 }); }));
  }
  RT_AT(5, 1094);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S52)))) {
    RT_AT(5, 1094);
    RT_RET(rt_rec(0, 2, K60, (wv[]){ rt_s(&S629), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1095);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S53)))) {
    RT_AT(5, 1095);
    RT_RET(rt_rec(0, 2, K60, (wv[]){ rt_s(&S713), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1096);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S54)))) {
    RT_AT(5, 1096);
    RT_RET(rt_rec(0, 2, K60, (wv[]){ rt_s(&S714), rt_def(w6_constK, "constK") }));
  }
  RT_AT(5, 1097);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S214)))) {
    RT_AT(5, 1097);
    RT_RET(rt_chk(s891_read(up, ({ wv t2502 = rt_get(&F_pos, v1207_e); wv t2503 = rt_def(up->v858_curSlot, "curSlot"); rt_idx(t2502, t2503); })), "expr"));
  }
  RT_AT(5, 1098);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S157)))) {
    RT_AT(5, 1098);
    RT_RET(({ wv t2504 = rt_cat(3, (wv[]){ rt_s(&S705), rt_text(rt_get(&F_name, v1207_e)), rt_s(&S715) }); wv t2505 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2504, t2505 }); }));
  }
  RT_AT(5, 1099);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S188)))) {
    RT_AT(5, 1099);
    RT_RET(rt_chk(s907_array(up, v1207_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1100);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S189)))) {
    RT_AT(5, 1100);
    RT_RET(rt_chk(s908_mapLit(up, v1207_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1101);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S190)))) {
    RT_AT(5, 1101);
    RT_RET(rt_chk(s909_record(up, v1207_e, rt_b(0)), "expr"));
  }
  RT_AT(5, 1102);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S191)))) {
    RT_AT(5, 1103);
    wv v1213_sq = rt_bind(rt_chk(s892_seq(up, ({ wv t2506 = rt_chk(s925_expr(up, rt_get(&F_index, v1207_e)), "expr"); wv t2507 = rt_chk(s925_expr(up, rt_get(&F_arr, v1207_e)), "expr"); rt_arr(2, (wv[]){ t2506, t2507 }, 0); })), "expr"), 0);
    RT_AT(5, 1104);
    RT_RET(({ wv t2510 = v1213_sq; wv t2511 = ({ wv t2508 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1213_sq))); wv t2509 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1213_sq))); rt_cat(5, (wv[]){ rt_s(&S716), t2508, rt_s(&S419), t2509, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2510, t2511), "expr"); }));
  }
  RT_AT(5, 1106);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S192)))) {
    RT_AT(5, 1109);
    wv v1214_lin = rt_bind(rt_chk(s888_enumOf(up, rt_get(&F_rec, v1207_e)), "expr"), 0);
    RT_AT(5, 1110);
    if (rt_cond(rt_ne(v1214_lin, RT_NULL))) {
      RT_AT(5, 1111);
      wv v1218_ms = rt_bind(rt_get(&F_members, rt_get(&F_node, v1214_lin)), 0);
      RT_AT(5, 1112);
      wv v1219_k = rt_i(0);
      RT_AT(5, 1113);
      while (rt_cond((RT_AT(5, 1113), ({ wv t2512 = rt_get(&F_name, rt_idx(v1219_k, v1218_ms)); wv t2513 = rt_get(&F_field, v1207_e); rt_ne(t2512, t2513); })))) {
        RT_AT(5, 1113);
        v1219_k = rt_add(v1219_k, rt_i(1));
      }
      RT_AT(5, 1114);
      RT_RET(({ wv t2516 = ({ wv t2514 = rt_text(rt_get(&F_c, v1214_lin)); wv t2515 = rt_text(v1219_k); rt_cat(5, (wv[]){ rt_s(&S657), t2514, rt_s(&S99), t2515, rt_s(&S658) }); }); wv t2517 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2516, t2517 }); }));
    }
    RT_AT(5, 1116);
    wv v1215_lib = rt_bind(rt_chk(s886_stolenFrom(up, rt_get(&F_rec, v1207_e)), "expr"), 0);
    RT_AT(5, 1117);
    if (rt_cond(rt_ne(v1215_lib, RT_NULL))) {
      RT_AT(5, 1117);
      RT_RET(({ wv t2520 = ({ wv t2518 = rt_text(rt_chk(s865_modId(up, v1215_lib), "expr")); wv t2519 = rt_text(rt_get(&F_field, v1207_e)); rt_cat(4, (wv[]){ rt_s(&S609), t2518, rt_s(&S12), t2519 }); }); wv t2521 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K60, (wv[]){ t2520, t2521 }); }));
    }
    RT_AT(5, 1118);
    wv v1216_key = ({ wv t2522 = rt_chk(s884_fieldRef(up, rt_get(&F_field, v1207_e)), "expr"); wv t2523 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2522, t2523 }); });
    RT_AT(5, 1119);
    wv v1217_sq = rt_bind(rt_chk(s892_seq(up, ({ wv t2524 = v1216_key; wv t2525 = rt_chk(s925_expr(up, rt_get(&F_rec, v1207_e)), "expr"); rt_arr(2, (wv[]){ t2524, t2525 }, 0); })), "expr"), 0);
    RT_AT(5, 1120);
    RT_RET(({ wv t2528 = v1217_sq; wv t2529 = ({ wv t2526 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1217_sq))); wv t2527 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1217_sq))); rt_cat(5, (wv[]){ rt_s(&S717), t2526, rt_s(&S419), t2527, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2528, t2529), "expr"); }));
  }
  RT_AT(5, 1122);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S193)))) {
    RT_AT(5, 1122);
    RT_RET(({ wv t2532 = ({ wv t2530 = rt_text(rt_chk(w6_unFn(rt_get(&F_op, v1207_e)), "expr")); wv t2531 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_e, v1207_e)), "expr"))); rt_cat(5, (wv[]){ rt_s(&S680), t2530, rt_s(&S98), t2531, rt_s(&S16) }); }); wv t2533 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2532, t2533 }); }));
  }
  RT_AT(5, 1123);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S194)))) {
    RT_AT(5, 1124);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v1207_e), rt_s(&S81)), "||") || rt_bool(rt_eq(rt_get(&F_op, v1207_e), rt_s(&S82)), "||")))) {
      RT_AT(5, 1125);
      RT_RET(({ wv t2539 = ({ wv t2534 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_l, v1207_e)), "expr"))); wv t2535 = rt_text(rt_get(&F_op, v1207_e)); wv t2536 = rt_text(rt_get(&F_op, v1207_e)); wv t2537 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_r, v1207_e)), "expr"))); wv t2538 = rt_text(rt_get(&F_op, v1207_e)); rt_cat(11, (wv[]){ rt_s(&S718), t2534, rt_s(&S719), t2535, rt_s(&S720), t2536, rt_s(&S721), t2537, rt_s(&S719), t2538, rt_s(&S722) }); }); wv t2540 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2539, t2540 }); }));
    }
    RT_AT(5, 1127);
    wv v1220_sq = rt_bind(rt_chk(s892_seq(up, ({ wv t2541 = rt_chk(s925_expr(up, rt_get(&F_l, v1207_e)), "expr"); wv t2542 = rt_chk(s925_expr(up, rt_get(&F_r, v1207_e)), "expr"); rt_arr(2, (wv[]){ t2541, t2542 }, 0); })), "expr"), 0);
    RT_AT(5, 1128);
    RT_RET(({ wv t2546 = v1220_sq; wv t2547 = ({ wv t2543 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v1207_e)), "expr")); wv t2544 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1220_sq))); wv t2545 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1220_sq))); rt_cat(7, (wv[]){ rt_s(&S680), t2543, rt_s(&S98), t2544, rt_s(&S419), t2545, rt_s(&S16) }); }); rt_chk(s893_wrap(up, t2546, t2547), "expr"); }));
  }
  RT_AT(5, 1130);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S195)))) {
    RT_AT(5, 1131);
    RT_RET(({ wv t2551 = ({ wv t2548 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_c, v1207_e)), "expr"))); wv t2549 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_a, v1207_e)), "expr"))); wv t2550 = rt_text(rt_get(&F_c, rt_chk(s925_expr(up, rt_get(&F_b, v1207_e)), "expr"))); rt_cat(7, (wv[]){ rt_s(&S723), t2548, rt_s(&S724), t2549, rt_s(&S697), t2550, rt_s(&S16) }); }); wv t2552 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2551, t2552 }); }));
  }
  RT_AT(5, 1133);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S184)))) {
    RT_AT(5, 1134);
    wv v1221_sq = rt_bind(rt_chk(s892_seq(up, rt_chk(s906_exprs(up, rt_get(&F_args, v1207_e)), "expr")), "expr"), 0);
    RT_AT(5, 1135);
    RT_RET(({ wv t2555 = v1221_sq; wv t2556 = ({ wv t2553 = rt_get(&F_callee, v1207_e); wv t2554 = rt_get(&F_cs, v1221_sq); rt_chk(s924_callC(up, t2553, t2554, rt_b(1)), "expr"); }); rt_chk(s893_wrap(up, t2555, t2556), "expr"); }));
  }
  RT_AT(5, 1137);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S186)))) {
    RT_AT(5, 1137);
    RT_RET(({ wv t2557 = rt_chk(s919_chainExpr(up, v1207_e), "expr"); wv t2558 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2557, t2558 }); }));
  }
  RT_AT(5, 1138);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S45)))) {
    RT_AT(5, 1138);
    RT_RET(({ wv t2559 = rt_chk(s923_matchExpr(up, v1207_e), "expr"); wv t2560 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K60, (wv[]){ t2559, t2560 }); }));
  }
  RT_AT(5, 1139);
  if (rt_cond(rt_eq(v1208_kind, rt_s(&S691)))) {
    RT_AT(5, 1139);
    RT_RET(({ wv t2561 = rt_get(&F_js, v1207_e); wv t2562 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K60, (wv[]){ t2561, t2562 }); }));
  }
  RT_AT(5, 1140);
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S725), rt_text(v1208_kind) }));
  RT_RET(RT_NULL);
}

static wv s926_assemble(struct fr176 *up) {
  rt_enter("assemble");
  RT_AT(5, 1146);
  wv v1222_all = rt_arr(3, (wv[]){ std_join(std_split(up->v838_runtime, rt_s(&S726)), rt_s(&S2)), rt_s(&S727), rt_s(&S13) }, 1);
  RT_AT(5, 1147);
  wv v1223_k = rt_i(0);
  RT_AT(5, 1148);
  while (rt_cond((RT_AT(5, 1148), ({ wv t2563 = v1223_k; wv t2564 = std_length(rt_def(up->v841_strings, "strings")); rt_lt(t2563, t2564); })))) {
    RT_AT(5, 1148);
    (void)({ wv t2569 = v1222_all; wv t2570 = ({ wv t2567 = rt_text(v1223_k); wv t2568 = rt_text(rt_chk(w6_wstrInit(({ wv t2565 = v1223_k; wv t2566 = rt_def(up->v841_strings, "strings"); rt_idx(t2565, t2566); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S728), t2567, rt_s(&S624), t2568, rt_s(&S14) }); }); std_push(t2569, t2570); });
    RT_AT(5, 1148);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1149);
  v1223_k = rt_i(0);
  RT_AT(5, 1150);
  while (rt_cond((RT_AT(5, 1150), ({ wv t2571 = v1223_k; wv t2572 = std_length(rt_def(up->v842_fields, "fields")); rt_lt(t2571, t2572); })))) {
    RT_AT(5, 1150);
    (void)({ wv t2579 = v1222_all; wv t2580 = ({ wv t2577 = rt_text(({ wv t2573 = v1223_k; wv t2574 = rt_def(up->v842_fields, "fields"); rt_idx(t2573, t2574); })); wv t2578 = rt_text(rt_chk(w6_wstrInit(({ wv t2575 = v1223_k; wv t2576 = rt_def(up->v842_fields, "fields"); rt_idx(t2575, t2576); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S729), t2577, rt_s(&S624), t2578, rt_s(&S14) }); }); std_push(t2579, t2580); });
    RT_AT(5, 1150);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1151);
  v1223_k = rt_i(0);
  RT_AT(5, 1152);
  while (rt_cond((RT_AT(5, 1152), ({ wv t2581 = v1223_k; wv t2582 = std_length(rt_def(up->v843_shapes, "shapes")); rt_lt(t2581, t2582); })))) {
    RT_AT(5, 1153);
    wv v1225_names = rt_bind(std_split(({ wv t2583 = v1223_k; wv t2584 = rt_def(up->v843_shapes, "shapes"); rt_idx(t2583, t2584); }), rt_s(&S97)), 0);
    RT_AT(5, 1154);
    wv v1226_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1155);
    wv v1227_j = rt_i(0);
    RT_AT(5, 1156);
    while (rt_cond((RT_AT(5, 1156), ({ wv t2585 = v1227_j; wv t2586 = std_length(v1225_names); rt_lt(t2585, t2586); })))) {
      RT_AT(5, 1156);
      (void)({ wv t2587 = v1226_refs; wv t2588 = rt_cat(2, (wv[]){ rt_s(&S613), rt_text(rt_idx(v1227_j, v1225_names)) }); std_push(t2587, t2588); });
      RT_AT(5, 1156);
      v1227_j = rt_add(v1227_j, rt_i(1));
    }
    RT_AT(5, 1157);
    (void)({ wv t2591 = v1222_all; wv t2592 = ({ wv t2589 = rt_text(v1223_k); wv t2590 = rt_text(std_join(v1226_refs, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S730), t2589, rt_s(&S615), t2590, rt_s(&S616) }); }); std_push(t2591, t2592); });
    RT_AT(5, 1158);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1160);
  v1223_k = rt_i(0);
  RT_AT(5, 1161);
  while (rt_cond((RT_AT(5, 1161), ({ wv t2593 = v1223_k; wv t2594 = std_length(rt_def(up->v844_enumDefs, "enumDefs")); rt_lt(t2593, t2594); })))) {
    RT_AT(5, 1161);
    (void)({ wv t2597 = v1222_all; wv t2598 = ({ wv t2595 = v1223_k; wv t2596 = rt_def(up->v844_enumDefs, "enumDefs"); rt_idx(t2595, t2596); }); std_push(t2597, t2598); });
    RT_AT(5, 1161);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1162);
  v1223_k = rt_i(0);
  RT_AT(5, 1163);
  while (rt_cond((RT_AT(5, 1163), ({ wv t2599 = v1223_k; wv t2600 = std_length(rt_def(up->v846_frames, "frames")); rt_lt(t2599, t2600); })))) {
    RT_AT(5, 1164);
    wv v1228_f = rt_bind(({ wv t2601 = v1223_k; wv t2602 = rt_def(up->v846_frames, "frames"); rt_idx(t2601, t2602); }), 0);
    RT_AT(5, 1165);
    wv v1229_members = rt_arr(0, NULL, 1);
    RT_AT(5, 1166);
    wv v1230_pf = rt_bind(rt_get(&F_parent, ({ wv t2603 = v1228_f; wv t2604 = rt_def(up->v853_fns, "fns"); rt_idx(t2603, t2604); })), 0);
    RT_AT(5, 1167);
    if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2605 = v1230_pf; wv t2606 = rt_def(up->v853_fns, "fns"); rt_idx(t2605, t2606); }))))) {
      RT_AT(5, 1167);
      (void)({ wv t2607 = v1229_members; wv t2608 = rt_cat(3, (wv[]){ rt_s(&S670), rt_text(v1230_pf), rt_s(&S731) }); std_push(t2607, t2608); });
    }
    RT_AT(5, 1168);
    wv v1231_j = rt_i(0);
    RT_AT(5, 1169);
    while (rt_cond((RT_AT(5, 1169), ({ wv t2609 = v1231_j; wv t2610 = std_length(rt_def(up->v852_decls, "decls")); rt_lt(t2609, t2610); })))) {
      RT_AT(5, 1170);
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(({ wv t2613 = rt_get(&F_func, ({ wv t2611 = v1231_j; wv t2612 = rt_def(up->v852_decls, "decls"); rt_idx(t2611, t2612); })); wv t2614 = v1228_f; rt_eq(t2613, t2614); }), "&&") && rt_bool(rt_get(&F_captured, ({ wv t2615 = v1231_j; wv t2616 = rt_def(up->v852_decls, "decls"); rt_idx(t2615, t2616); })), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_kind, ({ wv t2617 = v1231_j; wv t2618 = rt_def(up->v852_decls, "decls"); rt_idx(t2617, t2618); })), rt_s(&S33)), "&&")))) {
        RT_AT(5, 1171);
        (void)({ wv t2621 = v1229_members; wv t2622 = rt_cat(3, (wv[]){ rt_s(&S623), rt_text(rt_get(&F_cname, ({ wv t2619 = v1231_j; wv t2620 = rt_def(up->v852_decls, "decls"); rt_idx(t2619, t2620); }))), rt_s(&S14) }); std_push(t2621, t2622); });
      }
      RT_AT(5, 1173);
      v1231_j = rt_add(v1231_j, rt_i(1));
    }
    RT_AT(5, 1175);
    if (rt_cond(rt_eq(std_length(v1229_members), rt_i(0)))) {
      RT_AT(5, 1175);
      (void)std_push(v1229_members, rt_s(&S732));
    }
    RT_AT(5, 1176);
    (void)({ wv t2625 = v1222_all; wv t2626 = ({ wv t2623 = rt_text(v1228_f); wv t2624 = rt_text(std_join(v1229_members, rt_s(&S0))); rt_cat(5, (wv[]){ rt_s(&S670), t2623, rt_s(&S733), t2624, rt_s(&S616) }); }); std_push(t2625, t2626); });
    RT_AT(5, 1177);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1179);
  v1223_k = rt_i(0);
  RT_AT(5, 1180);
  while (rt_cond((RT_AT(5, 1180), ({ wv t2627 = v1223_k; wv t2628 = std_length(rt_def(up->v845_globals, "globals")); rt_lt(t2627, t2628); })))) {
    RT_AT(5, 1180);
    (void)({ wv t2631 = v1222_all; wv t2632 = rt_cat(3, (wv[]){ rt_s(&S673), rt_text(({ wv t2629 = v1223_k; wv t2630 = rt_def(up->v845_globals, "globals"); rt_idx(t2629, t2630); })), rt_s(&S14) }); std_push(t2631, t2632); });
    RT_AT(5, 1180);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1181);
  v1223_k = rt_i(0);
  RT_AT(5, 1182);
  while (rt_cond((RT_AT(5, 1182), ({ wv t2633 = v1223_k; wv t2634 = std_length(rt_def(up->v847_functions, "functions")); rt_lt(t2633, t2634); })))) {
    RT_AT(5, 1182);
    (void)({ wv t2637 = v1222_all; wv t2638 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2635 = v1223_k; wv t2636 = rt_def(up->v847_functions, "functions"); rt_idx(t2635, t2636); }))), rt_s(&S14) }); std_push(t2637, t2638); });
    RT_AT(5, 1182);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1183);
  v1223_k = rt_i(0);
  RT_AT(5, 1184);
  while (rt_cond((RT_AT(5, 1184), ({ wv t2639 = v1223_k; wv t2640 = std_length(rt_def(up->v847_functions, "functions")); rt_lt(t2639, t2640); })))) {
    RT_AT(5, 1185);
    (void)std_push(v1222_all, rt_s(&S13));
    RT_AT(5, 1186);
    (void)({ wv t2643 = v1222_all; wv t2644 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2641 = v1223_k; wv t2642 = rt_def(up->v847_functions, "functions"); rt_idx(t2641, t2642); }))), rt_s(&S734) }); std_push(t2643, t2644); });
    RT_AT(5, 1187);
    wv v1232_lines = rt_bind(rt_get(&F_lines, ({ wv t2645 = v1223_k; wv t2646 = rt_def(up->v847_functions, "functions"); rt_idx(t2645, t2646); })), 0);
    RT_AT(5, 1188);
    wv v1233_j = rt_i(0);
    RT_AT(5, 1189);
    while (rt_cond((RT_AT(5, 1189), ({ wv t2647 = v1233_j; wv t2648 = std_length(v1232_lines); rt_lt(t2647, t2648); })))) {
      RT_AT(5, 1189);
      (void)({ wv t2649 = v1222_all; wv t2650 = rt_idx(v1233_j, v1232_lines); std_push(t2649, t2650); });
      RT_AT(5, 1189);
      v1233_j = rt_add(v1233_j, rt_i(1));
    }
    RT_AT(5, 1190);
    (void)std_push(v1222_all, rt_s(&S15));
    RT_AT(5, 1191);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1193);
  (void)std_push(v1222_all, rt_s(&S13));
  RT_AT(5, 1194);
  wv v1224_files = rt_arr(0, NULL, 1);
  RT_AT(5, 1195);
  {
    wfor t2652;
    rt_for_start(&t2652, up->v837_modules, 0);
    for (int64_t t2651 = 0; rt_for_guard(&t2652, up->v837_modules, "modules"), rt_for_more(&t2652, t2651); t2651++) {
      wv v1234_m = rt_for_item(&t2652, t2651);
      RT_AT(5, 1195);
      (void)({ wv t2653 = v1224_files; wv t2654 = rt_chk(w1_quote(rt_get(&F_name, v1234_m)), "assemble"); std_push(t2653, t2654); });
    }
  }
  RT_AT(5, 1196);
  (void)({ wv t2655 = v1222_all; wv t2656 = rt_cat(3, (wv[]){ rt_s(&S735), rt_text(std_join(v1224_files, rt_s(&S419))), rt_s(&S616) }); std_push(t2655, t2656); });
  RT_AT(5, 1197);
  (void)std_push(v1222_all, rt_s(&S13));
  RT_AT(5, 1198);
  (void)std_push(v1222_all, rt_s(&S736));
  RT_AT(5, 1199);
  (void)std_push(v1222_all, rt_s(&S737));
  RT_AT(5, 1200);
  if (rt_cond(rt_gt(std_length(rt_def(up->v842_fields, "fields")), rt_i(0)))) {
    RT_AT(5, 1201);
    wv v1235_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1202);
    v1223_k = rt_i(0);
    RT_AT(5, 1203);
    while (rt_cond((RT_AT(5, 1203), ({ wv t2657 = v1223_k; wv t2658 = std_length(rt_def(up->v842_fields, "fields")); rt_lt(t2657, t2658); })))) {
      RT_AT(5, 1203);
      (void)({ wv t2661 = v1235_refs; wv t2662 = rt_cat(2, (wv[]){ rt_s(&S613), rt_text(({ wv t2659 = v1223_k; wv t2660 = rt_def(up->v842_fields, "fields"); rt_idx(t2659, t2660); })) }); std_push(t2661, t2662); });
      RT_AT(5, 1203);
      v1223_k = rt_add(v1223_k, rt_i(1));
    }
    RT_AT(5, 1204);
    (void)({ wv t2665 = v1222_all; wv t2666 = ({ wv t2663 = rt_text(std_length(rt_def(up->v842_fields, "fields"))); wv t2664 = rt_text(std_join(v1235_refs, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S738), t2663, rt_s(&S739), t2664, rt_s(&S740) }); }); std_push(t2665, t2666); });
  }
  RT_AT(5, 1207);
  if (rt_cond(rt_gt(std_length(rt_def(up->v845_globals, "globals")), rt_i(0)))) {
    RT_AT(5, 1208);
    wv v1236_refs = rt_arr(0, NULL, 1);
    RT_AT(5, 1209);
    v1223_k = rt_i(0);
    RT_AT(5, 1210);
    while (rt_cond((RT_AT(5, 1210), ({ wv t2667 = v1223_k; wv t2668 = std_length(rt_def(up->v845_globals, "globals")); rt_lt(t2667, t2668); })))) {
      RT_AT(5, 1210);
      (void)({ wv t2671 = v1236_refs; wv t2672 = rt_cat(2, (wv[]){ rt_s(&S92), rt_text(({ wv t2669 = v1223_k; wv t2670 = rt_def(up->v845_globals, "globals"); rt_idx(t2669, t2670); })) }); std_push(t2671, t2672); });
      RT_AT(5, 1210);
      v1223_k = rt_add(v1223_k, rt_i(1));
    }
    RT_AT(5, 1211);
    (void)({ wv t2675 = v1222_all; wv t2676 = ({ wv t2673 = rt_text(std_length(rt_def(up->v845_globals, "globals"))); wv t2674 = rt_text(std_join(v1236_refs, rt_s(&S419))); rt_cat(5, (wv[]){ rt_s(&S741), t2673, rt_s(&S742), t2674, rt_s(&S740) }); }); std_push(t2675, t2676); });
  }
  RT_AT(5, 1213);
  v1223_k = rt_i(0);
  RT_AT(5, 1214);
  while (rt_cond((RT_AT(5, 1214), ({ wv t2677 = v1223_k; wv t2678 = std_length(rt_def(up->v848_out, "out")); rt_lt(t2677, t2678); })))) {
    RT_AT(5, 1214);
    (void)({ wv t2681 = v1222_all; wv t2682 = ({ wv t2679 = v1223_k; wv t2680 = rt_def(up->v848_out, "out"); rt_idx(t2679, t2680); }); std_push(t2681, t2682); });
    RT_AT(5, 1214);
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_AT(5, 1215);
  (void)std_push(v1222_all, rt_s(&S15));
  RT_AT(5, 1216);
  (void)std_push(v1222_all, rt_s(&S13));
  RT_RET((RT_AT(5, 1217), std_join(v1222_all, rt_s(&S2))));
}

static wv w7_dumpTokens(wv v1239_toks, wv v1240_indent) {
  rt_enter("dumpTokens");
  v1239_toks = rt_bind(v1239_toks, 0);
  v1240_indent = rt_bind(v1240_indent, 0);
  RT_AT(6, 8);
  wv v1241_k = rt_i(0);
  RT_AT(6, 9);
  while (rt_cond((RT_AT(6, 9), ({ wv t2693 = v1241_k; wv t2694 = std_length(v1239_toks); rt_lt(t2693, t2694); })))) {
    RT_AT(6, 10);
    wv v1242_tk = rt_bind(rt_idx(v1241_k, v1239_toks), 0);
    RT_AT(6, 11);
    wv v1243_line = ({ wv t2695 = rt_text(v1240_indent); wv t2696 = rt_text(rt_get(&F_t, v1242_tk)); wv t2697 = rt_text(rt_get(&F_pos, v1242_tk)); wv t2698 = rt_text(rt_get(&F_end, v1242_tk)); wv t2699 = rt_text(rt_get(&F_sp, v1242_tk)); rt_cat(8, (wv[]){ t2695, t2696, rt_s(&S0), t2697, rt_s(&S0), t2698, rt_s(&S0), t2699 }); });
    RT_AT(6, 12);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1242_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1242_tk), rt_s(&S157)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1242_tk), rt_s(&S112)), "||")))) {
      RT_AT(6, 13);
      v1243_line = ({ wv t2700 = v1243_line; wv t2701 = rt_cat(2, (wv[]){ rt_s(&S0), rt_text(rt_get(&F_v, v1242_tk)) }); rt_add(t2700, t2701); });
    } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1242_tk), rt_s(&S139)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1242_tk), rt_s(&S149)), "||")))) {
      RT_AT(6, 15);
      v1243_line = ({ wv t2704 = v1243_line; wv t2705 = ({ wv t2702 = rt_text(rt_get(&F_text, v1242_tk)); wv t2703 = rt_text(rt_get(&F_v, v1242_tk)); rt_cat(4, (wv[]){ rt_s(&S0), t2702, rt_s(&S0), t2703 }); }); rt_add(t2704, t2705); });
    }
    RT_AT(6, 17);
    (void)std_print(v1243_line);
    RT_AT(6, 18);
    if (rt_cond(rt_eq(rt_get(&F_t, v1242_tk), rt_s(&S173)))) {
      RT_AT(6, 19);
      wv v1244_p = rt_i(0);
      RT_AT(6, 20);
      while (rt_cond((RT_AT(6, 20), ({ wv t2706 = v1244_p; wv t2707 = std_length(rt_get(&F_parts, v1242_tk)); rt_lt(t2706, t2707); })))) {
        RT_AT(6, 21);
        wv v1245_part = rt_bind(({ wv t2708 = v1244_p; wv t2709 = rt_get(&F_parts, v1242_tk); rt_idx(t2708, t2709); }), 0);
        RT_AT(6, 22);
        if (rt_cond(rt_eq(std_typeOf(v1245_part), rt_s(&S187)))) {
          RT_AT(6, 23);
          (void)std_print(({ wv t2710 = rt_text(v1240_indent); wv t2711 = rt_text(rt_chk(w1_quote(v1245_part), "dumpTokens")); rt_cat(3, (wv[]){ t2710, rt_s(&S743), t2711 }); }));
        } else {
          RT_AT(6, 25);
          (void)std_print(({ wv t2712 = rt_text(v1240_indent); wv t2713 = rt_text(rt_get(&F_pos, v1245_part)); rt_cat(3, (wv[]){ t2712, rt_s(&S744), t2713 }); }));
          RT_AT(6, 26);
          (void)({ wv t2714 = rt_get(&F_tokens, v1245_part); wv t2715 = rt_add(v1240_indent, rt_s(&S745)); rt_chk(w7_dumpTokens(t2714, t2715), "dumpTokens"); });
          RT_AT(6, 27);
          (void)std_print(rt_cat(2, (wv[]){ rt_text(v1240_indent), rt_s(&S746) }));
        }
        RT_AT(6, 29);
        v1244_p = rt_add(v1244_p, rt_i(1));
      }
    }
    RT_AT(6, 32);
    v1241_k = rt_add(v1241_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w7_sorted(wv v1246_xs) {
  rt_enter("sorted");
  v1246_xs = rt_bind(v1246_xs, 0);
  RT_AT(6, 41);
  wv v1247_out = rt_arr(0, NULL, 1);
  RT_AT(6, 42);
  wv v1248_k = rt_i(0);
  RT_AT(6, 43);
  while (rt_cond((RT_AT(6, 43), ({ wv t2716 = v1248_k; wv t2717 = std_length(v1246_xs); rt_lt(t2716, t2717); })))) {
    RT_AT(6, 44);
    (void)({ wv t2718 = v1247_out; wv t2719 = rt_idx(v1248_k, v1246_xs); std_push(t2718, t2719); });
    RT_AT(6, 45);
    wv v1249_j = rt_sub(std_length(v1247_out), rt_i(1));
    RT_AT(6, 46);
    while (rt_cond((RT_AT(6, 46), rt_b(rt_bool(rt_gt(v1249_j, rt_i(0)), "&&") && rt_bool(({ wv t2722 = ({ wv t2720 = rt_sub(v1249_j, rt_i(1)); wv t2721 = v1247_out; rt_idx(t2720, t2721); }); wv t2723 = rt_idx(v1249_j, v1247_out); rt_gt(t2722, t2723); }), "&&"))))) {
      RT_AT(6, 47);
      wv v1250_swap = rt_bind(rt_idx(v1249_j, v1247_out), 0);
      RT_AT(6, 48);
      ({ wv t2726 = ({ wv t2724 = rt_sub(v1249_j, rt_i(1)); wv t2725 = v1247_out; rt_idx(t2724, t2725); }); wv t2727 = v1249_j; wv t2728 = v1247_out; rt_set_idx(t2726, t2727, t2728); });
      RT_AT(6, 49);
      ({ wv t2729 = v1250_swap; wv t2730 = rt_sub(v1249_j, rt_i(1)); wv t2731 = v1247_out; rt_set_idx(t2729, t2730, t2731); });
      RT_AT(6, 50);
      v1249_j = rt_sub(v1249_j, rt_i(1));
    }
    RT_AT(6, 52);
    v1248_k = rt_add(v1248_k, rt_i(1));
  }
  RT_RET((RT_AT(6, 54), v1247_out));
}

static wv w7_dumpValue(wv v1251_x, wv v1252_indent) {
  rt_enter("dumpValue");
  v1251_x = rt_bind(v1251_x, 0);
  v1252_indent = rt_bind(v1252_indent, 0);
  RT_AT(6, 58);
  wv v1253_type = rt_bind(std_typeOf(v1251_x), 0);
  RT_AT(6, 59);
  if (rt_cond(rt_eq(v1253_type, rt_s(&S187)))) {
    RT_AT(6, 59);
    RT_RET(rt_chk(w1_quote(v1251_x), "dumpValue"));
  }
  RT_AT(6, 60);
  wv v1254_inner = rt_add(v1252_indent, rt_s(&S611));
  RT_AT(6, 61);
  if (rt_cond(rt_eq(v1253_type, rt_s(&S412)))) {
    RT_AT(6, 62);
    if (rt_cond(rt_eq(std_length(v1251_x), rt_i(0)))) {
      RT_AT(6, 62);
      RT_RET(rt_s(&S747));
    }
    RT_AT(6, 63);
    wv v1255_out = rt_s(&S99);
    RT_AT(6, 64);
    wv v1256_k = rt_i(0);
    RT_AT(6, 65);
    while (rt_cond((RT_AT(6, 65), ({ wv t2732 = v1256_k; wv t2733 = std_length(v1251_x); rt_lt(t2732, t2733); })))) {
      RT_AT(6, 66);
      v1255_out = ({ wv t2738 = ({ wv t2734 = rt_add(v1255_out, rt_s(&S2)); wv t2735 = v1254_inner; rt_add(t2734, t2735); }); wv t2739 = ({ wv t2736 = rt_idx(v1256_k, v1251_x); wv t2737 = v1254_inner; rt_chk(w7_dumpValue(t2736, t2737), "dumpValue"); }); rt_add(t2738, t2739); });
      RT_AT(6, 67);
      v1256_k = rt_add(v1256_k, rt_i(1));
    }
    RT_AT(6, 69);
    RT_RET(rt_add(({ wv t2740 = rt_add(v1255_out, rt_s(&S2)); wv t2741 = v1252_indent; rt_add(t2740, t2741); }), rt_s(&S100)));
  }
  RT_AT(6, 71);
  if (rt_cond(rt_eq(v1253_type, rt_s(&S414)))) {
    RT_AT(6, 72);
    wv v1257_keys = rt_bind(rt_chk(w7_sorted(std_fields(v1251_x)), "dumpValue"), 0);
    RT_AT(6, 73);
    wv v1258_out = rt_s(&S101);
    RT_AT(6, 74);
    wv v1259_k = rt_i(0);
    RT_AT(6, 75);
    while (rt_cond((RT_AT(6, 75), ({ wv t2742 = v1259_k; wv t2743 = std_length(v1257_keys); rt_lt(t2742, t2743); })))) {
      RT_AT(6, 76);
      wv v1260_key = rt_bind(rt_idx(v1259_k, v1257_keys), 0);
      RT_AT(6, 77);
      v1258_out = ({ wv t2750 = ({ wv t2746 = v1258_out; wv t2747 = ({ wv t2744 = rt_text(v1254_inner); wv t2745 = rt_text(v1260_key); rt_cat(4, (wv[]){ rt_s(&S2), t2744, t2745, rt_s(&S29) }); }); rt_add(t2746, t2747); }); wv t2751 = ({ wv t2748 = rt_idx(v1260_key, v1251_x); wv t2749 = v1254_inner; rt_chk(w7_dumpValue(t2748, t2749), "dumpValue"); }); rt_add(t2750, t2751); });
      RT_AT(6, 78);
      v1259_k = rt_add(v1259_k, rt_i(1));
    }
    RT_AT(6, 80);
    RT_RET(({ wv t2752 = v1258_out; wv t2753 = rt_cat(3, (wv[]){ rt_s(&S2), rt_text(v1252_indent), rt_s(&S15) }); rt_add(t2752, t2753); }));
  }
  RT_RET((RT_AT(6, 82), std_toString(v1251_x)));
}

static wv w8_dirOf(wv v1261_path) {
  rt_enter("dirOf");
  v1261_path = rt_bind(v1261_path, 0);
  RT_AT(7, 16);
  wv v1262_cs = rt_bind(std_split(v1261_path, rt_s(&S13)), 0);
  RT_AT(7, 17);
  wv v1263_k = rt_sub(std_length(v1262_cs), rt_i(1));
  RT_AT(7, 18);
  while (rt_cond((RT_AT(7, 18), rt_b(rt_bool(rt_ge(v1263_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1263_k, v1262_cs), rt_s(&S88)), "&&"))))) {
    RT_AT(7, 18);
    v1263_k = rt_sub(v1263_k, rt_i(1));
  }
  RT_RET((RT_AT(7, 19), std_join(({ wv t2754 = v1262_cs; wv t2755 = rt_add(v1263_k, rt_i(1)); std_slice(t2754, rt_i(0), t2755); }), rt_s(&S13))));
}

static wv w8_normalize(wv v1264_path) {
  rt_enter("normalize");
  v1264_path = rt_bind(v1264_path, 0);
  RT_AT(7, 24);
  wv v1265_segs = rt_bind(std_split(v1264_path, rt_s(&S88)), 0);
  RT_AT(7, 25);
  wv v1266_out = rt_arr(0, NULL, 1);
  RT_AT(7, 26);
  wv v1267_k = rt_i(0);
  RT_AT(7, 27);
  while (rt_cond((RT_AT(7, 27), ({ wv t2756 = v1267_k; wv t2757 = std_length(v1265_segs); rt_lt(t2756, t2757); })))) {
    RT_AT(7, 28);
    wv v1268_seg = rt_bind(rt_idx(v1267_k, v1265_segs), 0);
    RT_AT(7, 29);
    v1267_k = rt_add(v1267_k, rt_i(1));
    RT_AT(7, 30);
    if (rt_cond(rt_b(rt_bool(rt_eq(v1268_seg, rt_s(&S130)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1268_seg, rt_s(&S13)), "&&") && rt_bool(rt_gt(std_length(v1266_out), rt_i(0)), "&&")), "||")))) {
      continue;
    }
    RT_AT(7, 31);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1268_seg, rt_s(&S253)), "&&") && rt_bool(rt_gt(std_length(v1266_out), rt_i(0)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1266_out), rt_s(&S253)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1266_out), rt_s(&S13)), "&&")))) {
      RT_AT(7, 32);
      (void)std_pop(v1266_out);
    } else {
      RT_AT(7, 34);
      (void)std_push(v1266_out, v1268_seg);
    }
  }
  RT_RET((RT_AT(7, 37), std_join(v1266_out, rt_s(&S88))));
}

static wv w8_with(wv v1269_xs, wv v1270_x) {
  rt_enter("with");
  v1269_xs = rt_bind(v1269_xs, 0);
  v1270_x = rt_bind(v1270_x, 0);
  RT_AT(7, 42);
  wv v1271_out = rt_arr(0, NULL, 1);
  RT_AT(7, 43);
  wv v1272_k = rt_i(0);
  RT_AT(7, 44);
  while (rt_cond((RT_AT(7, 44), ({ wv t2758 = v1272_k; wv t2759 = std_length(v1269_xs); rt_lt(t2758, t2759); })))) {
    RT_AT(7, 45);
    (void)({ wv t2760 = v1271_out; wv t2761 = rt_idx(v1272_k, v1269_xs); std_push(t2760, t2761); });
    RT_AT(7, 46);
    v1272_k = rt_add(v1272_k, rt_i(1));
  }
  RT_AT(7, 48);
  (void)std_push(v1271_out, v1270_x);
  RT_RET((RT_AT(7, 49), v1271_out));
}

static wv w8_failIn(wv v1273_file, wv v1274_chars, wv v1275_pos, wv v1276_message) {
  rt_enter("failIn");
  v1273_file = rt_bind(v1273_file, 0);
  v1274_chars = rt_bind(v1274_chars, 0);
  v1275_pos = rt_bind(v1275_pos, 0);
  v1276_message = rt_bind(v1276_message, 0);
  RT_AT(7, 54);
  (void)rt_chk(w2_setSource(v1273_file, v1274_chars), "failIn");
  RT_AT(7, 55);
  (void)rt_chk(w2_abort(v1275_pos, v1276_message), "failIn");
  RT_RET(RT_NULL);
}

static wv w8_loadProgram(wv v1277_entryFile, wv v1278_entryChars, wv p_v1279_libDir, wv p_v1280_searched) {
  rt_enter("loadProgram");
  struct fr250 fr = { 0 };
  v1277_entryFile = rt_bind(v1277_entryFile, 0);
  fr.v1279_libDir = rt_bind(p_v1279_libDir, 0);
  fr.v1280_searched = rt_bind(p_v1280_searched, 0);
  RT_AT(7, 64);
  fr.v1281_modules = rt_arr(0, NULL, 1);
  RT_AT(7, 65);
  fr.v1282_libRoot = rt_bind((rt_cond(rt_eq(fr.v1279_libDir, RT_NULL)) ? RT_NULL : rt_add(rt_chk(w8_normalize(fr.v1279_libDir), "loadProgram"), rt_s(&S88))), 0);
  RT_AT(7, 131);
  wv v1287_path = rt_bind(rt_chk(w8_normalize(std_join(std_split(v1277_entryFile, rt_s(&S19)), rt_s(&S88))), "loadProgram"), 0);
  RT_AT(7, 132);
  (void)({ wv t2834 = v1277_entryFile; wv t2835 = v1278_entryChars; wv t2836 = v1287_path; wv t2837 = rt_arr(1, (wv[]){ v1287_path }, 0); rt_chk(s1286_visit(&fr, t2834, t2835, t2836, t2837), "loadProgram"); });
  RT_RET((RT_AT(7, 133), fr.v1281_modules));
}

static wv s1283_traceName(struct fr250 *up, wv v1288_path) {
  rt_enter("traceName");
  v1288_path = rt_bind(v1288_path, 0);
  RT_AT(7, 69);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_def(up->v1282_libRoot, "libRoot"), RT_NULL), "||") || rt_bool(({ wv t2762 = std_length(v1288_path); wv t2763 = std_length(rt_def(up->v1282_libRoot, "libRoot")); rt_le(t2762, t2763); }), "||")))) {
    RT_AT(7, 69);
    RT_RET(v1288_path);
  }
  RT_AT(7, 70);
  if (rt_cond(({ wv t2766 = ({ wv t2764 = v1288_path; wv t2765 = std_length(rt_def(up->v1282_libRoot, "libRoot")); std_slice(t2764, rt_i(0), t2765); }); wv t2767 = rt_def(up->v1282_libRoot, "libRoot"); rt_ne(t2766, t2767); }))) {
    RT_AT(7, 70);
    RT_RET(v1288_path);
  }
  RT_RET((RT_AT(7, 71), rt_add(rt_s(&S250), ({ wv t2768 = v1288_path; wv t2769 = std_length(rt_def(up->v1282_libRoot, "libRoot")); wv t2770 = std_length(v1288_path); std_slice(t2768, t2769, t2770); }))));
}

static wv s1284_target(struct fr250 *up, wv v1289_s, wv v1290_file, wv v1291_chars, wv v1292_path) {
  rt_enter("target");
  v1289_s = rt_bind(v1289_s, 0);
  v1290_file = rt_bind(v1290_file, 0);
  v1291_chars = rt_bind(v1291_chars, 0);
  v1292_path = rt_bind(v1292_path, 0);
  RT_AT(7, 77);
  if (rt_cond(rt_ne(std_at(rt_get(&F_path, v1289_s), rt_i(0)), rt_s(&S106)))) {
    RT_AT(7, 77);
    RT_RET(rt_chk(w8_normalize(({ wv t2771 = rt_chk(w8_dirOf(v1292_path), "target"); wv t2772 = rt_get(&F_path, v1289_s); rt_add(t2771, t2772); })), "target"));
  }
  RT_AT(7, 78);
  if (rt_cond(rt_eq(up->v1279_libDir, RT_NULL))) {
    RT_AT(7, 79);
    (void)({ wv t2773 = v1290_file; wv t2774 = v1291_chars; wv t2775 = rt_get(&F_pathPos, v1289_s); wv t2776 = rt_cat(2, (wv[]){ rt_s(&S748), rt_text(std_join(up->v1280_searched, rt_s(&S430))) }); rt_chk(w8_failIn(t2773, t2774, t2775, t2776), "target"); });
  }
  RT_RET((RT_AT(7, 81), rt_chk(w8_normalize(({ wv t2779 = up->v1279_libDir; wv t2780 = ({ wv t2777 = rt_get(&F_path, v1289_s); wv t2778 = std_length(rt_get(&F_path, v1289_s)); std_slice(t2777, rt_i(5), t2778); }); rt_add(t2779, t2780); })), "target")));
}

static wv s1285_loaded(struct fr250 *up, wv v1293_path) {
  rt_enter("loaded");
  v1293_path = rt_bind(v1293_path, 0);
  RT_AT(7, 85);
  wv v1294_k = rt_i(0);
  RT_AT(7, 86);
  while (rt_cond((RT_AT(7, 86), ({ wv t2781 = v1294_k; wv t2782 = std_length(rt_def(up->v1281_modules, "modules")); rt_lt(t2781, t2782); })))) {
    RT_AT(7, 87);
    if (rt_cond(({ wv t2785 = rt_get(&F_path, ({ wv t2783 = v1294_k; wv t2784 = rt_def(up->v1281_modules, "modules"); rt_idx(t2783, t2784); })); wv t2786 = v1293_path; rt_eq(t2785, t2786); }))) {
      RT_AT(7, 87);
      RT_RET(v1294_k);
    }
    RT_AT(7, 88);
    v1294_k = rt_add(v1294_k, rt_i(1));
  }
  RT_RET((RT_AT(7, 90), RT_NULL));
}

static wv s1286_visit(struct fr250 *up, wv v1295_file, wv v1296_chars, wv v1297_path, wv v1298_chain) {
  rt_enter("visit");
  v1295_file = rt_bind(v1295_file, 0);
  v1296_chars = rt_bind(v1296_chars, 0);
  v1297_path = rt_bind(v1297_path, 0);
  v1298_chain = rt_bind(v1298_chain, 0);
  RT_AT(7, 96);
  (void)rt_chk(w2_setSource(v1295_file, v1296_chars), "visit");
  RT_AT(7, 97);
  wv v1299_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(v1296_chars), "visit")), "visit"), 0);
  RT_AT(7, 98);
  wv v1300_imports = rt_arr(0, NULL, 1);
  RT_AT(7, 99);
  wv v1301_k = rt_i(0);
  RT_AT(7, 100);
  while (rt_cond((RT_AT(7, 100), ({ wv t2787 = v1301_k; wv t2788 = std_length(rt_get(&F_stmts, v1299_program)); rt_lt(t2787, t2788); })))) {
    RT_AT(7, 101);
    wv v1304_s = rt_bind(({ wv t2789 = v1301_k; wv t2790 = rt_get(&F_stmts, v1299_program); rt_idx(t2789, t2790); }), 0);
    RT_AT(7, 102);
    v1301_k = rt_add(v1301_k, rt_i(1));
    RT_AT(7, 103);
    if (rt_cond(rt_ne(rt_get(&F_k, v1304_s), rt_s(&S48)))) {
      continue;
    }
    RT_AT(7, 104);
    wv v1305_libPath = rt_bind(rt_chk(s1284_target(up, v1304_s, v1295_file, v1296_chars, v1297_path), "visit"), 0);
    RT_AT(7, 105);
    wv v1306_at = rt_i(0);
    RT_AT(7, 106);
    while (rt_cond((RT_AT(7, 106), rt_b(rt_bool(({ wv t2791 = v1306_at; wv t2792 = std_length(v1298_chain); rt_lt(t2791, t2792); }), "&&") && rt_bool(({ wv t2793 = rt_idx(v1306_at, v1298_chain); wv t2794 = v1305_libPath; rt_ne(t2793, t2794); }), "&&"))))) {
      RT_AT(7, 106);
      v1306_at = rt_add(v1306_at, rt_i(1));
    }
    RT_AT(7, 107);
    if (rt_cond(({ wv t2795 = v1306_at; wv t2796 = std_length(v1298_chain); rt_lt(t2795, t2796); }))) {
      RT_AT(7, 108);
      wv v1308_cycle = rt_bind(({ wv t2800 = ({ wv t2797 = v1298_chain; wv t2798 = v1306_at; wv t2799 = std_length(v1298_chain); std_slice(t2797, t2798, t2799); }); wv t2801 = v1305_libPath; rt_chk(w8_with(t2800, t2801), "visit"); }), 0);
      RT_AT(7, 109);
      (void)({ wv t2802 = v1295_file; wv t2803 = v1296_chars; wv t2804 = rt_get(&F_pathPos, v1304_s); wv t2805 = rt_cat(2, (wv[]){ rt_s(&S749), rt_text(std_join(v1308_cycle, rt_s(&S750))) }); rt_chk(w8_failIn(t2802, t2803, t2804, t2805), "visit"); });
    }
    RT_AT(7, 111);
    wv v1307_lib = rt_bind(rt_chk(s1285_loaded(up, v1305_libPath), "visit"), 1);
    RT_AT(7, 112);
    if (rt_cond(rt_eq(v1307_lib, RT_NULL))) {
      RT_AT(7, 113);
      wv v1309_missing = rt_bind((rt_cond(rt_eq(std_at(rt_get(&F_path, v1304_s), rt_i(0)), rt_s(&S106))) ? ({ wv t2808 = rt_text(({ wv t2806 = rt_get(&F_path, v1304_s); wv t2807 = std_length(rt_get(&F_path, v1304_s)); std_slice(t2806, rt_i(5), t2807); })); wv t2809 = rt_text(v1305_libPath); rt_cat(5, (wv[]){ rt_s(&S751), t2808, rt_s(&S752), t2809, rt_s(&S16) }); }) : rt_cat(3, (wv[]){ rt_s(&S753), rt_text(v1305_libPath), rt_s(&S114) })), 0);
      RT_AT(7, 114);
      wv t2810 = std_readFile(v1305_libPath);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          RT_AT(7, 115);
          (void)({ wv t2811 = v1295_file; wv t2812 = v1296_chars; wv t2813 = rt_get(&F_pathPos, v1304_s); wv t2814 = v1309_missing; rt_chk(w8_failIn(t2811, t2812, t2813, t2814), "visit"); });
          t2810 = (RT_AT(7, 115), RT_NULL);
        }
      }
      RT_AT(7, 114);
      wv v1310_text = rt_bind(t2810, 0);
      RT_AT(7, 116);
      wv v1311_cs = rt_bind(std_split(v1310_text, rt_s(&S13)), 0);
      RT_AT(7, 117);
      wv v1312_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1311_cs), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1311_cs), rt_s(&S754)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
      RT_AT(7, 118);
      v1307_lib = rt_bind(({ wv t2818 = v1305_libPath; wv t2819 = ({ wv t2815 = v1311_cs; wv t2816 = v1312_bom; wv t2817 = std_length(v1311_cs); std_slice(t2815, t2816, t2817); }); wv t2820 = v1305_libPath; wv t2821 = rt_chk(w8_with(v1298_chain, v1305_libPath), "visit"); rt_chk(s1286_visit(up, t2818, t2819, t2820, t2821), "visit"); }), 1);
    }
    RT_AT(7, 120);
    (void)({ wv t2822 = v1300_imports; wv t2823 = rt_rec(0, 2, K64, (wv[]){ v1304_s, v1307_lib }); std_push(t2822, t2823); });
  }
  RT_AT(7, 122);
  wv v1302_refTargets = rt_arr(0, NULL, 1);
  RT_AT(7, 123);
  wv v1303_mod = ({ wv t2824 = v1295_file; wv t2825 = v1296_chars; wv t2826 = v1297_path; wv t2827 = rt_chk(s1283_traceName(up, v1297_path), "visit"); wv t2828 = v1299_program; wv t2829 = v1300_imports; wv t2830 = rt_arr(0, NULL, 0); wv t2831 = v1302_refTargets; rt_rec(1, 9, K65, (wv[]){ t2824, t2825, t2826, t2827, t2828, t2829, t2830, rt_b(0), t2831 }); });
  RT_AT(7, 127);
  (void)({ wv t2832 = rt_def(up->v1281_modules, "modules"); wv t2833 = v1303_mod; std_push(t2832, t2833); });
  RT_RET((RT_AT(7, 128), rt_sub(std_length(rt_def(up->v1281_modules, "modules")), rt_i(1))));
}

static wv w_die(void) {
  rt_enter("die");
  RT_AT(8, 36);
  (void)std_eprint(rt_def(w_usage, "usage"));
  RT_AT(8, 37);
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w_load(wv v1313_file, wv v1314_from) {
  rt_enter("load");
  v1313_file = rt_bind(v1313_file, 0);
  v1314_from = rt_bind(v1314_from, 0);
  RT_AT(8, 43);
  wv t2838 = std_readFile(v1314_from);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 44);
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S761), rt_text(v1314_from) }));
      RT_AT(8, 44);
      rt_exit(rt_i(2));
      t2838 = RT_NULL;
    }
  }
  RT_AT(8, 43);
  wv v1315_text = rt_bind(t2838, 0);
  RT_AT(8, 45);
  wv v1316_chars = rt_bind(std_split(v1315_text, rt_s(&S13)), 0);
  RT_AT(8, 46);
  wv v1317_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1316_chars), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1316_chars), rt_s(&S754)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
  RT_AT(8, 47);
  (void)({ wv t2842 = v1313_file; wv t2843 = ({ wv t2839 = v1316_chars; wv t2840 = v1317_bom; wv t2841 = std_length(v1316_chars); std_slice(t2839, t2840, t2841); }); rt_chk(w2_setSource(t2842, t2843), "load"); });
  RT_RET(RT_NULL);
}

static wv w_onWindows(void) {
  rt_enter("onWindows");
  RT_AT(8, 93);
  wv t2848 = rt_s(&S771);
  wv t2849 = std_env(t2848);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2849 = (RT_AT(8, 94), rt_s(&S13));
    }
  }
  RT_AT(8, 93);
  wv v1319_os = rt_bind(t2849, 0);
  RT_RET((RT_AT(8, 95), rt_eq(v1319_os, rt_s(&S772))));
}

static wv w_tryRead(wv v1320_path) {
  rt_enter("tryRead");
  v1320_path = rt_bind(v1320_path, 0);
  RT_AT(8, 112);
  wv t2856 = std_readFile(v1320_path);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2856 = (RT_AT(8, 113), RT_NULL);
    }
  }
  RT_AT(8, 112);
  wv v1321_text = rt_bind(t2856, 0);
  RT_RET((RT_AT(8, 114), v1321_text));
}

static wv w_programDir(void) {
  rt_enter("programDir");
  RT_AT(8, 119);
  wv v1322_cs = rt_bind(std_split(std_programPath(), rt_s(&S13)), 0);
  RT_AT(8, 120);
  wv v1323_k = rt_sub(std_length(v1322_cs), rt_i(1));
  RT_AT(8, 121);
  while (rt_cond((RT_AT(8, 121), rt_b(rt_bool(rt_b(rt_bool(rt_ge(v1323_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1323_k, v1322_cs), rt_s(&S88)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(v1323_k, v1322_cs), rt_s(&S19)), "&&"))))) {
    RT_AT(8, 121);
    v1323_k = rt_sub(v1323_k, rt_i(1));
  }
  RT_RET((RT_AT(8, 122), std_join(({ wv t2857 = v1322_cs; wv t2858 = rt_add(v1323_k, rt_i(1)); std_slice(t2857, rt_i(0), t2858); }), rt_s(&S13))));
}

static wv w_readRuntime(void) {
  rt_enter("readRuntime");
  RT_AT(8, 129);
  wv v1324_text = rt_bind(rt_chk(w_tryRead(rt_def(w_runtimePath, "runtimePath")), "readRuntime"), 0);
  RT_AT(8, 130);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1324_text, RT_NULL), "||") || rt_bool(rt_def(w_runtimeGiven, "runtimeGiven"), "||")))) {
    RT_AT(8, 130);
    RT_RET(v1324_text);
  }
  RT_AT(8, 131);
  wv v1325_dir = rt_bind(rt_chk(w_programDir(), "readRuntime"), 0);
  RT_AT(8, 132);
  wv v1326_here = rt_bind(rt_chk(w_tryRead(({ wv t2859 = v1325_dir; wv t2860 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2859, t2860); })), "readRuntime"), 0);
  RT_RET((RT_AT(8, 133), (rt_cond(rt_ne(v1326_here, RT_NULL)) ? v1326_here : rt_chk(w_tryRead(({ wv t2861 = rt_add(v1325_dir, rt_s(&S779)); wv t2862 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2861, t2862); })), "readRuntime"))));
}

static wv w_findLib(void) {
  rt_enter("findLib");
  RT_AT(8, 142);
  wv v1327_dir = rt_bind(std_split(rt_chk(w_programDir(), "findLib"), rt_s(&S19)), 0);
  RT_AT(8, 143);
  wv v1328_slashed = rt_bind(std_join(v1327_dir, rt_s(&S88)), 0);
  RT_AT(8, 144);
  wv v1329_candidates = rt_bind((rt_cond(rt_ne(rt_def(w_libGiven, "libGiven"), rt_s(&S13))) ? rt_arr(1, (wv[]){ std_join(std_split(rt_def(w_libGiven, "libGiven"), rt_s(&S19)), rt_s(&S88)) }, 0) : ({ wv t2863 = rt_add(v1328_slashed, rt_s(&S780)); wv t2864 = rt_add(v1328_slashed, rt_s(&S781)); rt_arr(2, (wv[]){ t2863, t2864 }, 0); })), 0);
  RT_AT(8, 145);
  wv v1330_k = rt_i(0);
  RT_AT(8, 146);
  while (rt_cond((RT_AT(8, 146), ({ wv t2865 = v1330_k; wv t2866 = std_length(v1329_candidates); rt_lt(t2865, t2866); })))) {
    RT_AT(8, 147);
    wv v1331_c = rt_bind(rt_idx(v1330_k, v1329_candidates), 0);
    RT_AT(8, 148);
    (void)({ wv t2867 = rt_def(w_searched, "searched"); wv t2868 = v1331_c; std_push(t2867, t2868); });
    RT_AT(8, 149);
    wv t2869 = std_listDir(v1331_c);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        t2869 = (RT_AT(8, 150), RT_NULL);
      }
    }
    RT_AT(8, 149);
    wv v1332_names = rt_bind(t2869, 0);
    RT_AT(8, 151);
    if (rt_cond(rt_ne(v1332_names, RT_NULL))) {
      RT_AT(8, 151);
      RT_RET((rt_cond(rt_eq(({ wv t2870 = v1331_c; wv t2871 = std_length(v1331_c); std_slice(t2870, rt_i(-1), t2871); }), rt_s(&S88))) ? v1331_c : rt_add(v1331_c, rt_s(&S88))));
    }
    RT_AT(8, 152);
    v1330_k = rt_add(v1330_k, rt_i(1));
  }
  RT_RET((RT_AT(8, 154), RT_NULL));
}

static const char *const W_files[] = { "wvc/text.wvt", "wvc/diag.wvt", "wvc/lexer.wvt", "wvc/parser.wvt", "wvc/checker.wvt", "wvc/codegen_c.wvt", "wvc/dump.wvt", "wvc/loader.wvt", "wvc/wvc.wvt" };

static void wvt_main(void) {
  rt_files = W_files;
  rt_syms(96, (wstr *const[]){ &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars, &F_word, &F_t, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts, &F_tokens, &F_k, &F_e, &F_handler, &F_items, &F_fields, &F_index, &F_arr, &F_rec, &F_l, &F_r, &F_c, &F_a, &F_b, &F_stmts, &F_param, &F_paramPos, &F_body, &F_neg, &F_cond, &F_isVar, &F_isRef, &F_name, &F_namePos, &F_op, &F_target, &F_names, &F_coll, &F_from, &F_to, &F_step, &F_reversed, &F_code, &F_arg, &F_paren, &F_path, &F_pathPos, &F_params, &F_members, &F_value, &F_close, &F_field, &F_callee, &F_args, &F_values, &F_subject, &F_cases, &F_els, &F_branches, &F_arity, &F_elem, &F_types, &F_id, &F_mod, &F_kind, &F_parent, &F_visibleAt, &F_fnId, &F_lib, &F_func, &F_top, &F_loops, &F_guards, &F_key, &F_exports, &F_refTargets, &F_useMod, &F_imports, &F_node, &F_notLibrary, &F_program, &F_children, &F_d, &F_depth, &F_cname, &F_captured, &F_fnIndex, &F_pre, &F_cs, &F_proto, &F_lines, &F_js });
  rt_roots(68, (wv *const[]){ &w2_srcFile, &w2_srcChars, &w2_warnings, &w3_keywords, &w3_reserved, &w3_bangWords, &w3_setOps, &w3_ops, &w3_maxInt, &w4_exprStart, &w4_onfailOk, &w4_callees, &w4_exportable, &w5_stdlib, &w5_neverCompletes, &w5_onfailValueUsed, &w5_tInt, &w5_tFloat, &w5_tBool, &w5_tString, &w5_tNull, &w5_tArray, &w5_tMap, &w5_tNever, &w5_lastDecls, &w5_lastBindingTypes, &w5_lastFnTypes, &w5_stdAccepts, &w5_hexDigits, &w5_libraryTop, &w6_binOps, &w6_binFns, &w6_scalarKinds, &w6_plainKinds, &w6_signalers, &w6_constK, &w6_pureK, &w6_effectK, &w_usage, &w_argv, &w_mode, &w_input, &w_output, &w_runtimePath, &w_cc, &w_libGiven, &w_textGiven, &w_arg, &w_inspect, &w_dotWvt, &w_runtimeGiven, &w_searched, &w_libDir, &w_modules, &w_problems, &w_runtime, &w_code, &w_dotExe, &w_cFile, &w_parts, &w_ccName, &w_flags, &w_status, &v1318_a, &v1333_tokens, &v1334_program, &v1335_p, &v1336_p });
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
  w6_binFns = rt_arr(17, (wv[]){ rt_s(&S582), rt_s(&S583), rt_s(&S584), rt_s(&S585), rt_s(&S586), rt_s(&S587), rt_s(&S588), rt_s(&S589), rt_s(&S590), rt_s(&S591), rt_s(&S592), rt_s(&S593), rt_s(&S594), rt_s(&S595), rt_s(&S596), rt_s(&S597), rt_s(&S598) }, 0);
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
  w_usage = rt_add(rt_add(rt_add(rt_add(rt_add(rt_s(&S755), rt_s(&S756)), rt_s(&S757)), rt_s(&S758)), rt_s(&S759)), rt_s(&S760));
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
  while (rt_cond((RT_AT(8, 61), ({ wv t2844 = w_arg; wv t2845 = std_length(w_argv); rt_lt(t2844, t2845); })))) {
    RT_AT(8, 62);
    v1318_a = rt_bind(rt_idx(w_arg, w_argv), 0);
    RT_AT(8, 63);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_arg, rt_i(0)), "&&") && rt_bool(rt_eq(v1318_a, rt_s(&S762)), "&&")))) {
      RT_AT(8, 64);
      w_mode = rt_bind(v1318_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1318_a, rt_s(&S763)), "||") || rt_bool(rt_eq(v1318_a, rt_s(&S764)), "||")), "||") || rt_bool(rt_eq(v1318_a, rt_s(&S765)), "||")))) {
      RT_AT(8, 66);
      w_mode = rt_bind(v1318_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1318_a, rt_s(&S766)), "||") || rt_bool(rt_eq(v1318_a, rt_s(&S767)), "||")), "||") || rt_bool(rt_eq(v1318_a, rt_s(&S768)), "||")), "||") || rt_bool(rt_eq(v1318_a, rt_s(&S769)), "||")), "||") || rt_bool(rt_eq(v1318_a, rt_s(&S770)), "||")))) {
      RT_AT(8, 68);
      w_arg = rt_add(w_arg, rt_i(1));
      RT_AT(8, 69);
      if (rt_cond(({ wv t2846 = w_arg; wv t2847 = std_length(w_argv); rt_ge(t2846, t2847); }))) {
        RT_AT(8, 69);
        (void)rt_chk(w_die(), NULL);
      }
      RT_AT(8, 70);
      if (rt_cond(rt_eq(v1318_a, rt_s(&S766)))) {
        RT_AT(8, 71);
        w_output = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1318_a, rt_s(&S767)))) {
        RT_AT(8, 73);
        w_runtimePath = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1318_a, rt_s(&S769)))) {
        RT_AT(8, 75);
        w_libGiven = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1318_a, rt_s(&S770)))) {
        RT_AT(8, 77);
        w_textGiven = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else {
        RT_AT(8, 79);
        w_cc = rt_bind(rt_idx(w_arg, w_argv), 1);
      }
    } else if (rt_cond(rt_b(rt_bool(rt_ne(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(rt_gt(std_length(v1318_a), rt_i(0)), "&&") && rt_bool(rt_eq(std_at(v1318_a, rt_i(0)), rt_s(&S86)), "&&")), "||")))) {
      RT_AT(8, 82);
      (void)rt_chk(w_die(), NULL);
    } else {
      RT_AT(8, 84);
      w_input = rt_bind(v1318_a, 1);
    }
    RT_AT(8, 86);
    w_arg = rt_add(w_arg, rt_i(1));
  }
  RT_AT(8, 88);
  w_inspect = rt_b(rt_bool(rt_eq(w_mode, rt_s(&S763)), "||") || rt_bool(rt_eq(w_mode, rt_s(&S764)), "||"));
  RT_AT(8, 89);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(w_inspect, "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "||") || rt_bool(rt_ne(w_libGiven, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S762)), "&&") && rt_bool(rt_ne(w_cc, rt_s(&S13)), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_eq(w_mode, rt_s(&S765)), "&&") && rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S765)), "&&") && rt_bool(rt_ne(w_textGiven, rt_s(&S13)), "&&")), "||")))) {
    RT_AT(8, 90);
    (void)rt_chk(w_die(), NULL);
  }
  RT_AT(8, 97);
  w_dotWvt = rt_b(rt_bool(rt_ge(std_length(w_input), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t2850 = w_input; wv t2851 = std_length(w_input); std_slice(t2850, rt_i(-4), t2851); }), rt_s(&S248)), "&&"));
  RT_AT(8, 98);
  if (rt_cond(rt_eq(w_mode, rt_s(&S762)))) {
    RT_AT(8, 99);
    if (rt_cond(rt_eq(w_cc, rt_s(&S13)))) {
      RT_AT(8, 99);
      wv t2852 = rt_s(&S773);
      wv t2853 = std_env(t2852);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          t2853 = (RT_AT(8, 99), rt_s(&S774));
        }
      }
      RT_AT(8, 99);
      w_cc = rt_bind(t2853, 1);
    }
    RT_AT(8, 101);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_output, rt_s(&S13)), "&&") && rt_bool(rt_not(w_dotWvt), "&&")))) {
      RT_AT(8, 102);
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S775), rt_text(w_input), rt_s(&S776) }));
      RT_AT(8, 103);
      rt_exit(rt_i(2));
    }
    RT_AT(8, 105);
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      RT_AT(8, 105);
      w_output = ({ wv t2854 = std_slice(w_input, rt_i(0), rt_i(-4)); wv t2855 = (rt_cond(rt_chk(w_onWindows(), NULL)) ? rt_s(&S777) : rt_s(&S13)); rt_add(t2854, t2855); });
    }
  }
  RT_AT(8, 107);
  w_runtimeGiven = rt_ne(w_runtimePath, rt_s(&S13));
  RT_AT(8, 108);
  if (rt_cond(rt_not(w_runtimeGiven))) {
    RT_AT(8, 108);
    w_runtimePath = rt_s(&S778);
  }
  RT_AT(8, 140);
  w_searched = rt_arr(0, NULL, 1);
  RT_AT(8, 157);
  (void)({ wv t2872 = w_input; wv t2873 = (rt_cond(rt_ne(w_textGiven, rt_s(&S13))) ? w_textGiven : w_input); rt_chk(w_load(t2872, t2873), NULL); });
  RT_AT(8, 159);
  if (rt_cond(rt_eq(w_mode, rt_s(&S763)))) {
    RT_AT(8, 160);
    v1333_tokens = rt_bind(rt_chk(w3_lex(w2_srcChars), NULL), 0);
    RT_AT(8, 161);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 162);
    (void)rt_chk(w7_dumpTokens(v1333_tokens, rt_s(&S13)), NULL);
    RT_AT(8, 163);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 165);
  if (rt_cond(rt_eq(w_mode, rt_s(&S764)))) {
    RT_AT(8, 166);
    v1334_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(w2_srcChars), NULL)), NULL), 0);
    RT_AT(8, 167);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 168);
    (void)std_print(rt_chk(w7_dumpValue(v1334_program, rt_s(&S13)), NULL));
    RT_AT(8, 169);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 171);
  w_libDir = rt_bind(rt_chk(w_findLib(), NULL), 0);
  RT_AT(8, 172);
  w_modules = rt_bind(rt_chk(w8_loadProgram(w2_srcFile, w2_srcChars, w_libDir, w_searched), NULL), 0);
  RT_AT(8, 174);
  w_problems = rt_bind(({ wv t2874 = w_modules; wv t2875 = rt_ne(w_mode, rt_s(&S765)); rt_chk(w5_check(t2874, t2875), NULL); }), 0);
  RT_AT(8, 177);
  if (rt_cond(rt_eq(w_mode, rt_s(&S765)))) {
    RT_AT(8, 178);
    (void)rt_chk(w2_flushWarnings(), NULL);
    RT_AT(8, 179);
    {
      wfor t2877;
      rt_for_start(&t2877, w_problems, 0);
      for (int64_t t2876 = 0; rt_for_guard(&t2877, w_problems, "problems"), rt_for_more(&t2877, t2876); t2876++) {
        v1335_p = rt_for_item(&t2877, t2876);
        RT_AT(8, 179);
        (void)std_eprint(rt_chk(w2_formatDiag(v1335_p), NULL));
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
    v1336_p = rt_i(0);
    RT_AT(8, 186);
    while (rt_cond((RT_AT(8, 186), ({ wv t2878 = v1336_p; wv t2879 = std_length(w_problems); rt_lt(t2878, t2879); })))) {
      RT_AT(8, 187);
      (void)std_eprint(rt_chk(w2_formatDiag(rt_idx(v1336_p, w_problems)), NULL));
      RT_AT(8, 188);
      v1336_p = rt_add(v1336_p, rt_i(1));
    }
    RT_AT(8, 190);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 192);
  w_runtime = rt_bind(rt_chk(w_readRuntime(), NULL), 0);
  RT_AT(8, 193);
  if (rt_cond(rt_eq(w_runtime, RT_NULL))) {
    RT_AT(8, 194);
    (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S761), rt_text(w_runtimePath) }));
    RT_AT(8, 195);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 197);
  w_code = rt_bind(rt_chk(w6_generateC(w_modules, w_runtime), NULL), 0);
  RT_AT(8, 198);
  (void)rt_chk(w2_flushWarnings(), NULL);
  RT_AT(8, 199);
  if (rt_cond(rt_ne(w_mode, rt_s(&S762)))) {
    RT_AT(8, 200);
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      RT_AT(8, 200);
      w_output = rt_add((rt_cond(w_dotWvt) ? std_slice(w_input, rt_i(0), rt_i(-4)) : w_input), rt_s(&S782));
    }
    RT_AT(8, 201);
    wv t2880 = std_writeFile(w_output, w_code);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        RT_AT(8, 202);
        (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S783), rt_text(w_output) }));
        RT_AT(8, 202);
        rt_exit(rt_i(2));
        t2880 = RT_NULL;
      }
    }
    RT_AT(8, 201);
    (void)t2880;
    RT_AT(8, 203);
    rt_exit(rt_i(0));
  }
  RT_AT(8, 208);
  w_dotExe = rt_b(rt_bool(rt_ge(std_length(w_output), rt_i(4)), "&&") && rt_bool(rt_eq(rt_chk(w1_toLower(({ wv t2881 = w_output; wv t2882 = std_length(w_output); std_slice(t2881, rt_i(-4), t2882); })), NULL), rt_s(&S777)), "&&"));
  RT_AT(8, 209);
  w_cFile = rt_add((rt_cond(w_dotExe) ? std_slice(w_output, rt_i(0), rt_i(-4)) : w_output), rt_s(&S782));
  RT_AT(8, 210);
  wv t2883 = std_writeFile(w_cFile, w_code);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 211);
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S783), rt_text(w_cFile) }));
      RT_AT(8, 211);
      rt_exit(rt_i(2));
      t2883 = RT_NULL;
    }
  }
  RT_AT(8, 210);
  (void)t2883;
  RT_AT(8, 212);
  w_parts = rt_bind(std_split(std_join(std_split(w_cc, rt_s(&S19)), rt_s(&S88)), rt_s(&S88)), 0);
  RT_AT(8, 213);
  w_ccName = rt_bind(rt_chk(w1_toLower(rt_idx(rt_i(-1), w_parts)), NULL), 0);
  RT_AT(8, 214);
  w_flags = rt_arr(4, (wv[]){ rt_s(&S784), rt_s(&S766), w_output, w_cFile }, 0);
  RT_AT(8, 215);
  wv t2884 = (rt_cond(rt_b(rt_bool(rt_eq(w_ccName, rt_s(&S774)), "||") || rt_bool(rt_eq(w_ccName, rt_s(&S785)), "||"))) ? rt_arr(5, (wv[]){ rt_s(&S786), rt_s(&S784), rt_s(&S766), w_output, w_cFile }, 0) : w_flags);
  wv t2885 = std_run(w_cc, t2884);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      RT_AT(8, 216);
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S787), rt_text(w_cc), rt_s(&S788) }));
      RT_AT(8, 216);
      rt_exit(rt_i(2));
      t2885 = RT_NULL;
    }
  }
  RT_AT(8, 215);
  w_status = rt_bind(t2885, 0);
  RT_AT(8, 217);
  if (rt_cond(rt_ne(w_status, rt_i(0)))) {
    RT_AT(8, 218);
    (void)std_eprint(({ wv t2886 = rt_text(w_cc); wv t2887 = rt_text(w_status); rt_cat(4, (wv[]){ rt_s(&S775), t2886, rt_s(&S789), t2887 }); }));
    RT_AT(8, 219);
    rt_exit(rt_i(2));
  }
  RT_AT(8, 221);
  rt_exit(rt_i(0));
}
