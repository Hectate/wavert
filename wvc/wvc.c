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

static _Noreturn void rt_die(const char *msg, size_t n) {
#ifdef WVT_TEST_DIE
  WVT_TEST_DIE(msg, n); // the runtime tests report a panic and go on
#endif
  sb b = { 0 };
  sb_puts(&b, "uncaught panic: ");
  sb_putn(&b, msg, n);
  sb_putc(&b, '\n');
  int shown = 0;
  for (int i = rt_depth - 1; i >= 0 && shown < 48; i--, shown++) sb_printf(&b, "    in %s\n", rt_stack[i]);
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
  rt_stack[rt_depth++] = name;
}
#define RT_RET(x) do { wv r_ = (x); rt_depth--; return r_; } while (0)

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
static const wstr *rec_key(wv k) {
  if (k.tag != T_STR) rt_fail("type error: a record key must be a string, got %s", type_name(k));
  return k.s;
}
// The slot of field key in r; key is registered, or NULL for a key no record has.
static wv *field(const wstr *key, const wstr *name, wv r) {
  if (r.tag != T_REC) rt_fail("type error: member access needs a record, got %s", type_name(r));
  if (key) for (int64_t i = 0; i < r.r->n; i++) if (r.r->keys[i] == key) return &r.r->vals[i];
  sb b = { 0 };
  sb_puts(&b, "no field '");
  sb_putn(&b, name->data, name->bytes);
  sb_puts(&b, "' in record");
  rt_fail_sb(&b);
}
static wv rt_get(const wstr *key, wv r) { return *field(key, key, r); }
static void writable(wv r) {
  if (!r.r->is_mut) rt_fail("cannot modify a field of a frozen record");
}
static void rt_set_mem(wv v, const wstr *key, wv r) {
  wv *p = field(key, key, r);
  writable(r);
  *p = v;
}
static void rt_upd_mem(wv v, const wstr *key, wv r, wv (*op)(wv, wv)) {
  wv *p = field(key, key, r);
  writable(r);
  *p = op(*p, v);
}
static wv *computed(wv k, wv r) {
  const wstr *name = rec_key(k);
  return field(sym_find(name), name, r);
}

// ---- maps (§10.11) ----

static void key_ok(wv k) {
  if (k.tag != T_INT && k.tag != T_STR && k.tag != T_BOOL && k.tag != T_MEM) {
    rt_fail("type error: a map key must be an int, a string, a boolean or an enum member, got %s", type_name(k));
  }
}
static uint64_t key_hash(wv k) {
  uint64_t h;
  switch (k.tag) {
    case T_STR: return hash_bytes(k.s->data, k.s->bytes);
    case T_INT: h = (uint64_t)k.i; break;
    case T_BOOL: h = (uint64_t)k.b + 0x51ED27; break;
    default: h = (uint64_t)(uintptr_t)k.m; break;
  }
  h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ULL; // splitmix64's finalizer
  h ^= h >> 27; h *= 0x94D049BB133111EBULL;
  return h ^ (h >> 31);
}
static int key_eq(wv a, wv b) {
  if (a.tag != b.tag) return 0;
  switch (a.tag) {
    case T_INT: return a.i == b.i;
    case T_STR: return str_eq(a.s, b.s);
    case T_BOOL: return a.b == b.b;
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
    writable(arr);
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
    writable(arr);
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

static wv std_readKey(void) {
  key_start();
  key_apply(1);
  // Waiting: a terminal waits only briefly for the rest of an escape sequence,
  // and does not wait for a "\n" after "\r", since Enter sends "\r" alone.
  int wait = K_WAIT, brief = key_tty ? K_BRIEF : K_WAIT, crlf = key_tty ? K_HAVE : K_WAIT;
#ifdef _WIN32
  if (key_tty) wait = brief = crlf = K_HAVE; // only text left over from $readLine
#endif
  for (;;) {
    wv k;
    int r;
#ifdef _WIN32
    if (key_tty && in_pos == in_n) r = console_key(&k);
    else
#endif
    r = byte_key(&k, wait, brief, crlf);
    if (r < 0) return rt_std_fail("$readKey");
    if (r > 0) return k;
  }
}

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

// A generator is a mutable record { s0, s1, s2, s3 } of 32-bit words. Its keys
// are the program's own keys for those names, if it has them, so that rng.s0
// and { s0: ... } work as for any record.
static const wstr *rng_keys[4];
static void rng_start(void) {
  if (rng_keys[0]) return;
  static const char *names[4] = { "s0", "s1", "s2", "s3" };
  for (int i = 0; i < 4; i++) {
    wstr probe = { 2, 2, 1, 0, names[i] };
    const wstr *k = sym_find(&probe);
    if (!k) {
      wstr *s = (wstr *)str_new(names[i], 2);
      s->sym = 1;
      sym_add(s);
      k = s;
    }
    rng_keys[i] = k;
  }
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
static const wstr S42 = { 5, 5, 1, 0, "break" };
static const wstr S43 = { 8, 8, 1, 0, "continue" };
static const wstr S44 = { 5, 5, 1, 0, "match" };
static const wstr S45 = { 4, 4, 1, 0, "case" };
static const wstr S46 = { 4, 4, 1, 0, "enum" };
static const wstr S47 = { 6, 6, 1, 0, "import" };
static const wstr S48 = { 6, 6, 1, 0, "export" };
static const wstr S49 = { 4, 4, 1, 0, "true" };
static const wstr S50 = { 5, 5, 1, 0, "false" };
static const wstr S51 = { 4, 4, 1, 0, "null" };
static const wstr S52 = { 3, 3, 1, 0, "nan" };
static const wstr S53 = { 3, 3, 1, 0, "inf" };
static const wstr S54 = { 4, 4, 1, 0, "exit" };
static const wstr S55 = { 4, 4, 1, 0, "fail" };
static const wstr S56 = { 6, 6, 1, 0, "onfail" };
static const wstr S57 = { 5, 5, 1, 0, "panic" };
static const wstr S58 = { 5, 5, 1, 0, "fails" };
static const wstr S59 = { 79, 79, 1, 0, "'fails' is reserved for marking functions that can fail; it is not designed yet" };
static const wstr S60 = { 7, 7, 1, 0, "recover" };
static const wstr S61 = { 58, 58, 1, 0, "'recover' is reserved: catching panics is not designed yet" };
static const wstr S62 = { 7, 7, 1, 0, "finally" };
static const wstr S63 = { 58, 58, 1, 0, "'finally' is reserved: catching panics is not designed yet" };
static const wstr S64 = { 3, 3, 1, 0, "**=" };
static const wstr S65 = { 3, 3, 1, 0, "<<=" };
static const wstr S66 = { 3, 3, 1, 0, ">>=" };
static const wstr S67 = { 2, 2, 1, 0, "+=" };
static const wstr S68 = { 2, 2, 1, 0, "-=" };
static const wstr S69 = { 2, 2, 1, 0, "*=" };
static const wstr S70 = { 2, 2, 1, 0, "/=" };
static const wstr S71 = { 2, 2, 1, 0, "%=" };
static const wstr S72 = { 2, 2, 1, 0, "&=" };
static const wstr S73 = { 2, 2, 1, 0, "|=" };
static const wstr S74 = { 2, 2, 1, 0, "^=" };
static const wstr S75 = { 2, 2, 1, 0, "**" };
static const wstr S76 = { 2, 2, 1, 0, "==" };
static const wstr S77 = { 2, 2, 1, 0, "!=" };
static const wstr S78 = { 2, 2, 1, 0, "<=" };
static const wstr S79 = { 2, 2, 1, 0, ">=" };
static const wstr S80 = { 2, 2, 1, 0, "&&" };
static const wstr S81 = { 2, 2, 1, 0, "||" };
static const wstr S82 = { 2, 2, 1, 0, "<<" };
static const wstr S83 = { 2, 2, 1, 0, ">>" };
static const wstr S84 = { 1, 1, 1, 0, "+" };
static const wstr S85 = { 1, 1, 1, 0, "-" };
static const wstr S86 = { 1, 1, 1, 0, "*" };
static const wstr S87 = { 1, 1, 1, 0, "/" };
static const wstr S88 = { 1, 1, 1, 0, "%" };
static const wstr S89 = { 1, 1, 1, 0, "<" };
static const wstr S90 = { 1, 1, 1, 0, ">" };
static const wstr S91 = { 1, 1, 1, 0, "&" };
static const wstr S92 = { 1, 1, 1, 0, "|" };
static const wstr S93 = { 1, 1, 1, 0, "~" };
static const wstr S94 = { 1, 1, 1, 0, "!" };
static const wstr S95 = { 1, 1, 1, 0, "?" };
static const wstr S96 = { 1, 1, 1, 0, "," };
static const wstr S97 = { 1, 1, 1, 0, "(" };
static const wstr S98 = { 1, 1, 1, 0, "[" };
static const wstr S99 = { 1, 1, 1, 0, "]" };
static const wstr S100 = { 1, 1, 1, 0, "{" };
static const wstr S101 = { 1, 1, 1, 0, "1" };
static const wstr S102 = { 1, 1, 1, 0, "n" };
static const wstr S103 = { 1, 1, 1, 0, "r" };
static const wstr S104 = { 1, 1, 1, 0, "t" };
static const wstr S105 = { 1, 1, 1, 0, "$" };
static const wstr S106 = { 62, 62, 1, 0, "'//' starts a comment; Wavert has no integer-division operator" };
static const wstr S107 = { 42, 42, 1, 0, "for integer division write (a / b)($floor)" };
static const wstr S108 = { 2, 2, 1, 0, "*/" };
static const wstr S109 = { 26, 26, 1, 0, "unterminated block comment" };
static const wstr S110 = { 3, 3, 1, 0, "eof" };
static const wstr S111 = { 5, 5, 1, 0, "opset" };
static const wstr S112 = { 34, 34, 1, 0, "no whitespace is allowed inside '!" };
static const wstr S113 = { 1, 1, 1, 0, "'" };
static const wstr S114 = { 8, 8, 1, 0, "write '!" };
static const wstr S115 = { 3, 3, 1, 0, ">>>" };
static const wstr S116 = { 26, 26, 1, 0, "there is no '>>>' operator" };
static const wstr S117 = { 47, 47, 1, 0, "for an unsigned interpretation use x($toUint32)" };
static const wstr S118 = { 2, 2, 1, 0, "--" };
static const wstr S119 = { 23, 23, 1, 0, "'--' is not an operator" };
static const wstr S120 = { 74, 74, 1, 0, "write '- -x' or '-(-x)' for double negation, or ';set x -= 1' to decrement" };
static const wstr S121 = { 2, 2, 1, 0, "++" };
static const wstr S122 = { 23, 23, 1, 0, "'++' is not an operator" };
static const wstr S123 = { 32, 32, 1, 0, "write ';set x += 1' to increment" };
static const wstr S124 = { 59, 59, 1, 0, "';;': likely a habitual trailing ';' from C-style languages" };
static const wstr S125 = { 61, 61, 1, 0, "Wavert statements begin with ;, they are not terminated by it" };
static const wstr S126 = { 1, 1, 1, 0, "=" };
static const wstr S127 = { 22, 22, 1, 0, "'=' is not an operator" };
static const wstr S128 = { 86, 86, 1, 0, "declare with ';const x is value', assign with ';set x to value', and compare with '=='" };
static const wstr S129 = { 1, 1, 1, 0, "." };
static const wstr S130 = { 2, 2, 1, 0, "//" };
static const wstr S131 = { 2, 2, 1, 0, "/*" };
static const wstr S132 = { 35, 35, 1, 0, "no whitespace is allowed around '.'" };
static const wstr S133 = { 53, 53, 1, 0, "member access is written record.field, e.g. node.kind" };
static const wstr S134 = { 22, 22, 1, 0, "unexpected character '" };
static const wstr S135 = { 32, 32, 1, 0, "'$' cannot appear in identifiers" };
static const wstr S136 = { 66, 66, 1, 0, "'$' is reserved for the standard library; user names have no sigil" };
static const wstr S137 = { 2, 2, 1, 0, "id" };
static const wstr S138 = { 3, 3, 1, 0, "int" };
static const wstr S139 = { 1, 1, 1, 0, "x" };
static const wstr S140 = { 1, 1, 1, 0, "b" };
static const wstr S141 = { 22, 22, 1, 0, "' must be followed by " };
static const wstr S142 = { 6, 6, 1, 0, "binary" };
static const wstr S143 = { 3, 3, 1, 0, "hex" };
static const wstr S144 = { 7, 7, 1, 0, " digits" };
static const wstr S145 = { 40, 40, 1, 0, "digits are required on both sides of '.'" };
static const wstr S146 = { 6, 6, 1, 0, "write " };
static const wstr S147 = { 2, 2, 1, 0, ".0" };
static const wstr S148 = { 5, 5, 1, 0, "float" };
static const wstr S149 = { 1, 1, 1, 0, "e" };
static const wstr S150 = { 1, 1, 1, 0, "E" };
static const wstr S151 = { 37, 37, 1, 0, "an exponent needs digits: 1e9, 2.5e-3" };
static const wstr S152 = { 26, 26, 1, 0, "' is not a numeric literal" };
static const wstr S153 = { 29, 29, 1, 0, "float literal is out of range" };
static const wstr S154 = { 45, 45, 1, 0, "integer literal exceeds the 53-bit safe range" };
static const wstr S155 = { 59, 59, 1, 0, "'$' must be immediately followed by a standard library name" };
static const wstr S156 = { 3, 3, 1, 0, "std" };
static const wstr S157 = { 19, 19, 1, 0, "unterminated string" };
static const wstr S158 = { 27, 27, 1, 0, "raw newline inside a string" };
static const wstr S159 = { 10, 10, 1, 0, "write '\\n'" };
static const wstr S160 = { 1, 1, 1, 0, "u" };
static const wstr S161 = { 22, 22, 1, 0, "invalid \\u{...} escape" };
static const wstr S162 = { 69, 69, 1, 0, "write 1 to 6 hex digits naming a Unicode scalar value, e.g. \\u{1F608}" };
static const wstr S163 = { 17, 17, 1, 0, "unknown escape '\\" };
static const wstr S164 = { 43, 43, 1, 0, "valid escapes are \\n \\r \\t \\\\ \\\" \\$ \\u{...}" };
static const wstr S165 = { 37, 37, 1, 0, "Wavert interpolation is {x}, not ${x}" };
static const wstr S166 = { 23, 23, 1, 0, "unmatched '}' in string" };
static const wstr S167 = { 30, 30, 1, 0, "write '}}' for a literal brace" };
static const wstr S168 = { 14, 14, 1, 0, "did you mean {" };
static const wstr S169 = { 2, 2, 1, 0, "}?" };
static const wstr S170 = { 53, 53, 1, 0, "Wavert interpolation is {x}; write \\$ for a literal $" };
static const wstr S171 = { 3, 3, 1, 0, "str" };
static const wstr S172 = { 36, 36, 1, 0, "unterminated interpolation in string" };
static const wstr S173 = { 19, 19, 1, 0, "empty interpolation" };
static const wstr S174 = { 30, 30, 1, 0, "write '{{' for a literal brace" };
static const wstr S175 = { 3, 3, 1, 0, "!if" };
static const wstr S176 = { 4, 4, 1, 0, "expr" };
static const wstr S177 = { 4, 4, 1, 0, "decl" };
static const wstr S178 = { 12, 12, 1, 0, "end of input" };
static const wstr S179 = { 2, 2, 1, 0, "'$" };
static const wstr S180 = { 8, 8, 1, 0, "a string" };
static const wstr S181 = { 2, 2, 1, 0, "='" };
static const wstr S182 = { 4, 4, 1, 0, "call" };
static const wstr S183 = { 8, 8, 1, 0, "failWith" };
static const wstr S184 = { 5, 5, 1, 0, "chain" };
static const wstr S185 = { 6, 6, 1, 0, "string" };
static const wstr S186 = { 3, 3, 1, 0, "arr" };
static const wstr S187 = { 3, 3, 1, 0, "map" };
static const wstr S188 = { 3, 3, 1, 0, "rec" };
static const wstr S189 = { 3, 3, 1, 0, "idx" };
static const wstr S190 = { 3, 3, 1, 0, "mem" };
static const wstr S191 = { 2, 2, 1, 0, "un" };
static const wstr S192 = { 3, 3, 1, 0, "bin" };
static const wstr S193 = { 4, 4, 1, 0, "tern" };
static const wstr S194 = { 10, 10, 1, 0, "expected '" };
static const wstr S195 = { 8, 8, 1, 0, ", found " };
static const wstr S196 = { 9, 9, 1, 0, "', found " };
static const wstr S197 = { 27, 27, 1, 0, "missing whitespace around '" };
static const wstr S198 = { 79, 79, 1, 0, "calls are postfix: write the arguments first, then the function, e.g. (a, b)(f)" };
static const wstr S199 = { 36, 36, 1, 0, "every Wavert statement begins with ;" };
static const wstr S200 = { 41, 41, 1, 0, "expected ';' to start a statement, found " };
static const wstr S201 = { 5, 5, 1, 0, "empty" };
static const wstr S202 = { 46, 46, 1, 0, "';end' must be the last statement of a library" };
static const wstr S203 = { 47, 47, 1, 0, "'import' must come before every other statement" };
static const wstr S204 = { 3, 3, 1, 0, "end" };
static const wstr S205 = { 59, 59, 1, 0, "'export' is only allowed in a library, which ends with ;end" };
static const wstr S206 = { 62, 62, 1, 0, "'onfail' must follow a statement whose value comes from a call" };
static const wstr S207 = { 33, 33, 1, 0, "expected a name for the fail code" };
static const wstr S208 = { 6, 6, 1, 0, "!while" };
static const wstr S209 = { 36, 36, 1, 0, "'else' without a preceding if or !if" };
static const wstr S210 = { 22, 22, 1, 0, "'case' outside a match" };
static const wstr S211 = { 2, 2, 1, 0, "to" };
static const wstr S212 = { 4, 4, 1, 0, "name" };
static const wstr S213 = { 31, 31, 1, 0, "an assignment starts with 'set'" };
static const wstr S214 = { 15, 15, 1, 0, ";set x to value" };
static const wstr S215 = { 54, 54, 1, 0, "to make it the block's value, remove the ';' before it" };
static const wstr S216 = { 28, 28, 1, 0, "this statement has no effect" };
static const wstr S217 = { 23, 23, 1, 0, "expected a name after '" };
static const wstr S218 = { 14, 14, 1, 0, " name is value" };
static const wstr S219 = { 2, 2, 1, 0, "is" };
static const wstr S220 = { 9, 9, 1, 0, " is value" };
static const wstr S221 = { 76, 76, 1, 0, "expected 'to' or a compound assignment such as '+=' after the target, found " };
static const wstr S222 = { 35, 35, 1, 0, "a fail code must be an integer >= 1" };
static const wstr S223 = { 51, 51, 1, 0, "'import' is only allowed at the top level of a file" };
static const wstr S224 = { 45, 45, 1, 0, "expected a name for the module after 'import'" };
static const wstr S225 = { 26, 26, 1, 0, ";import lib from \"lib.wvt\"" };
static const wstr S226 = { 4, 4, 1, 0, "from" };
static const wstr S227 = { 8, 8, 1, 0, ";import " };
static const wstr S228 = { 7, 7, 1, 0, " from \"" };
static const wstr S229 = { 5, 5, 1, 0, ".wvt\"" };
static const wstr S230 = { 45, 45, 1, 0, "an import path must be a plain string literal" };
static const wstr S231 = { 37, 37, 1, 0, "import paths use '/' as the separator" };
static const wstr S232 = { 4, 4, 1, 0, ".wvt" };
static const wstr S233 = { 42, 42, 1, 0, "an import path includes the .wvt extension" };
static const wstr S234 = { 43, 43, 1, 0, "only top-level declarations can be exported" };
static const wstr S235 = { 70, 70, 1, 0, "only fn, const, var, ref and enum declarations can be exported, found " };
static const wstr S236 = { 52, 52, 1, 0, "';end' is only allowed at the top level of a library" };
static const wstr S237 = { 46, 46, 1, 0, "a library must export at least one declaration" };
static const wstr S238 = { 55, 55, 1, 0, "mark one with 'export', e.g. ;export fn name(x) { ... }" };
static const wstr S239 = { 36, 36, 1, 0, "the name comes before the parameters" };
static const wstr S240 = { 22, 22, 1, 0, ";fn name(a, b) { ... }" };
static const wstr S241 = { 37, 37, 1, 0, "expected the function name after 'fn'" };
static const wstr S242 = { 39, 39, 1, 0, "expected '(' to open the parameter list" };
static const wstr S243 = { 4, 4, 1, 0, ";fn " };
static const wstr S244 = { 14, 14, 1, 0, "(a, b) { ... }" };
static const wstr S245 = { 25, 25, 1, 0, "expected a parameter name" };
static const wstr S246 = { 43, 43, 1, 0, "a parameter is a name, var name or ref name" };
static const wstr S247 = { 40, 40, 1, 0, "expected ')' to close the parameter list" };
static const wstr S248 = { 45, 45, 1, 0, "an enum can only be declared at the top level" };
static const wstr S249 = { 33, 33, 1, 0, "the name comes before the members" };
static const wstr S250 = { 17, 17, 1, 0, ";enum Name (a, b)" };
static const wstr S251 = { 35, 35, 1, 0, "expected the enum name after 'enum'" };
static const wstr S252 = { 36, 36, 1, 0, "expected '(' to open the member list" };
static const wstr S253 = { 6, 6, 1, 0, ";enum " };
static const wstr S254 = { 7, 7, 1, 0, " (a, b)" };
static const wstr S255 = { 33, 33, 1, 0, "an enum needs at least one member" };
static const wstr S256 = { 42, 42, 1, 0, "' is a keyword and cannot be a member name" };
static const wstr S257 = { 30, 30, 1, 0, "expected a member name, found " };
static const wstr S258 = { 18, 18, 1, 0, "duplicate member '" };
static const wstr S259 = { 9, 9, 1, 0, "' in enum" };
static const wstr S260 = { 38, 38, 1, 0, "expected ',' or ')' in the member list" };
static const wstr S261 = { 20, 20, 1, 0, "expected '(' after '" };
static const wstr S262 = { 30, 30, 1, 0, "conditions are parenthesized: " };
static const wstr S263 = { 12, 12, 1, 0, " (x) { ... }" };
static const wstr S264 = { 35, 35, 1, 0, "expected ')' to close the condition" };
static const wstr S265 = { 12, 12, 1, 0, "expected '{'" };
static const wstr S266 = { 32, 32, 1, 0, "unterminated block: expected '}'" };
static const wstr S267 = { 68, 68, 1, 0, "a record as a block's value needs its own braces: { { key: value } }" };
static const wstr S268 = { 75, 75, 1, 0, "an expression without a leading ';' is the block's value and must come last" };
static const wstr S269 = { 44, 44, 1, 0, "expected '}' after the block's value, found " };
static const wstr S270 = { 53, 53, 1, 0, "expected a name, an indexed element or a field to set" };
static const wstr S271 = { 38, 38, 1, 0, "expected ':' in conditional expression" };
static const wstr S272 = { 22, 22, 1, 0, "'-x ** y' is ambiguous" };
static const wstr S273 = { 28, 28, 1, 0, "write -(x ** y) or (-x) ** y" };
static const wstr S274 = { 59, 59, 1, 0, "'[' at the start of a line indexes the expression before it" };
static const wstr S275 = { 71, 71, 1, 0, "if it starts the block's value, wrap the value in parentheses: ([a, b])" };
static const wstr S276 = { 31, 31, 1, 0, "expected ']' to close the index" };
static const wstr S277 = { 41, 41, 1, 0, "' is a keyword and cannot be a field name" };
static const wstr S278 = { 39, 39, 1, 0, "expected a field name after '.', found " };
static const wstr S279 = { 4, 4, 1, 0, "bool" };
static const wstr S280 = { 17, 17, 1, 0, "empty parentheses" };
static const wstr S281 = { 39, 39, 1, 0, "a call with no arguments is written (f)" };
static const wstr S282 = { 34, 34, 1, 0, "trailing comma in an argument list" };
static const wstr S283 = { 40, 40, 1, 0, "expected ',' or ')' in the argument list" };
static const wstr S284 = { 69, 69, 1, 0, "a parenthesized list must be followed by the function it is passed to" };
static const wstr S285 = { 30, 30, 1, 0, "(a, b)(f) calls f with a and b" };
static const wstr S286 = { 23, 23, 1, 0, "expected ']' after '[:'" };
static const wstr S287 = { 27, 27, 1, 0, "an empty map is written [:]" };
static const wstr S288 = { 31, 31, 1, 0, "trailing comma in array literal" };
static const wstr S289 = { 32, 32, 1, 0, "expected ',' or ']' in the array" };
static const wstr S290 = { 12, 12, 1, 0, "unexpected '" };
static const wstr S291 = { 46, 46, 1, 0, "declarations and assignments are statements: ;" };
static const wstr S292 = { 4, 4, 1, 0, " ..." };
static const wstr S293 = { 21, 21, 1, 0, "'fail' is a statement" };
static const wstr S294 = { 25, 25, 1, 0, ";fail c fails with code c" };
static const wstr S295 = { 37, 37, 1, 0, "an assignment is a statement: ;set x " };
static const wstr S296 = { 7, 7, 1, 0, "= value" };
static const wstr S297 = { 30, 30, 1, 0, "expected an expression, found " };
static const wstr S298 = { 29, 29, 1, 0, "expected a field name, found " };
static const wstr S299 = { 47, 47, 1, 0, "record literals are written { key: value, ... }" };
static const wstr S300 = { 17, 17, 1, 0, "duplicate field '" };
static const wstr S301 = { 19, 19, 1, 0, "' in record literal" };
static const wstr S302 = { 29, 29, 1, 0, "there is no shorthand; write " };
static const wstr S303 = { 31, 31, 1, 0, "expected ':' after field name '" };
static const wstr S304 = { 32, 32, 1, 0, "trailing comma in record literal" };
static const wstr S305 = { 37, 37, 1, 0, "expected ',' or '}' in record literal" };
static const wstr S306 = { 1, 1, 1, 0, "i" };
static const wstr S307 = { 1, 1, 1, 0, "s" };
static const wstr S308 = { 63, 63, 1, 0, "a map key must be an int, a string, a boolean or an enum member" };
static const wstr S309 = { 28, 28, 1, 0, "duplicate key in map literal" };
static const wstr S310 = { 43, 43, 1, 0, "expected ':' after a key in the map literal" };
static const wstr S311 = { 42, 42, 1, 0, "map literals are written [key: value, ...]" };
static const wstr S312 = { 29, 29, 1, 0, "trailing comma in map literal" };
static const wstr S313 = { 38, 38, 1, 0, "expected ',' or ']' in the map literal" };
static const wstr S314 = { 38, 38, 1, 0, "expected '{' after the match's subject" };
static const wstr S315 = { 32, 32, 1, 0, "'else' must come last in a match" };
static const wstr S316 = { 31, 31, 1, 0, "a case needs at least one value" };
static const wstr S317 = { 21, 21, 1, 0, "case \"a\", \"b\" { ... }" };
static const wstr S318 = { 27, 27, 1, 0, "a match has only one 'else'" };
static const wstr S319 = { 45, 45, 1, 0, "a match needs at least one case before 'else'" };
static const wstr S320 = { 31, 31, 1, 0, "a match needs at least one case" };
static const wstr S321 = { 30, 30, 1, 0, "match (x) { case \"a\" { ... } }" };
static const wstr S322 = { 41, 41, 1, 0, "expected 'case', 'else' or '}' in a match" };
static const wstr S323 = { 35, 35, 1, 0, "a case value cannot be interpolated" };
static const wstr S324 = { 28, 28, 1, 0, "floats cannot be case values" };
static const wstr S325 = { 58, 58, 1, 0, "float equality makes a poor match; compare with if instead" };
static const wstr S326 = { 48, 48, 1, 0, "a case value must be a literal or an enum member" };
static const wstr S327 = { 67, 67, 1, 0, "case values are integers, strings, true, false, null or Enum.member" };
static const wstr S328 = { 35, 35, 1, 0, "'!if (a != b)' is a double negative" };
static const wstr S329 = { 19, 19, 1, 0, "write 'if (a == b)'" };
static const wstr S330 = { 11, 11, 1, 0, "unexpected " };
static const wstr S331 = { 17, 17, 1, 0, " in interpolation" };
static const wstr S332 = { 5, 5, 1, 0, "print" };
static const wstr S333 = { 5, 5, 1, 0, "write" };
static const wstr S334 = { 6, 6, 1, 0, "eprint" };
static const wstr S335 = { 8, 8, 1, 0, "readLine" };
static const wstr S336 = { 7, 7, 1, 0, "readKey" };
static const wstr S337 = { 8, 8, 1, 0, "readFile" };
static const wstr S338 = { 9, 9, 1, 0, "writeFile" };
static const wstr S339 = { 10, 10, 1, 0, "appendFile" };
static const wstr S340 = { 10, 10, 1, 0, "fileExists" };
static const wstr S341 = { 7, 7, 1, 0, "listDir" };
static const wstr S342 = { 10, 10, 1, 0, "deleteFile" };
static const wstr S343 = { 4, 4, 1, 0, "args" };
static const wstr S344 = { 3, 3, 1, 0, "env" };
static const wstr S345 = { 11, 11, 1, 0, "programPath" };
static const wstr S346 = { 3, 3, 1, 0, "run" };
static const wstr S347 = { 6, 6, 1, 0, "length" };
static const wstr S348 = { 2, 2, 1, 0, "at" };
static const wstr S349 = { 5, 5, 1, 0, "slice" };
static const wstr S350 = { 4, 4, 1, 0, "code" };
static const wstr S351 = { 8, 8, 1, 0, "fromCode" };
static const wstr S352 = { 5, 5, 1, 0, "split" };
static const wstr S353 = { 4, 4, 1, 0, "join" };
static const wstr S354 = { 8, 8, 1, 0, "toString" };
static const wstr S355 = { 5, 5, 1, 0, "toInt" };
static const wstr S356 = { 7, 7, 1, 0, "toFloat" };
static const wstr S357 = { 4, 4, 1, 0, "push" };
static const wstr S358 = { 3, 3, 1, 0, "pop" };
static const wstr S359 = { 4, 4, 1, 0, "same" };
static const wstr S360 = { 4, 4, 1, 0, "sort" };
static const wstr S361 = { 6, 6, 1, 0, "sortBy" };
static const wstr S362 = { 7, 7, 1, 0, "reverse" };
static const wstr S363 = { 3, 3, 1, 0, "get" };
static const wstr S364 = { 3, 3, 1, 0, "has" };
static const wstr S365 = { 6, 6, 1, 0, "remove" };
static const wstr S366 = { 4, 4, 1, 0, "keys" };
static const wstr S367 = { 6, 6, 1, 0, "values" };
static const wstr S368 = { 6, 6, 1, 0, "typeOf" };
static const wstr S369 = { 6, 6, 1, 0, "fields" };
static const wstr S370 = { 5, 5, 1, 0, "floor" };
static const wstr S371 = { 4, 4, 1, 0, "ceil" };
static const wstr S372 = { 5, 5, 1, 0, "round" };
static const wstr S373 = { 5, 5, 1, 0, "trunc" };
static const wstr S374 = { 8, 8, 1, 0, "toUint32" };
static const wstr S375 = { 5, 5, 1, 0, "isNan" };
static const wstr S376 = { 4, 4, 1, 0, "time" };
static const wstr S377 = { 9, 9, 1, 0, "newRandom" };
static const wstr S378 = { 6, 6, 1, 0, "random" };
static const wstr S379 = { 11, 11, 1, 0, "randomFloat" };
static const wstr S380 = { 9, 9, 1, 0, " function" };
static const wstr S381 = { 6, 6, 1, 0, "n enum" };
static const wstr S382 = { 9, 9, 1, 0, " argument" };
static const wstr S383 = { 4, 4, 1, 0, "str:" };
static const wstr S384 = { 4, 4, 1, 0, "mem:" };
static const wstr S385 = { 55, 55, 1, 0, "this block's value is used, but it does not produce one" };
static const wstr S386 = { 62, 62, 1, 0, "end the block with its value, or with null if that is intended" };
static const wstr S387 = { 42, 42, 1, 0, "' is already declared in this scope (line " };
static const wstr S388 = { 11, 11, 1, 0, "' shadows '" };
static const wstr S389 = { 32, 32, 1, 0, "' from an enclosing scope (line " };
static const wstr S390 = { 45, 45, 1, 0, "Wavert forbids shadowing; choose another name" };
static const wstr S391 = { 14, 14, 1, 0, "unknown name '" };
static const wstr S392 = { 39, 39, 1, 0, "' is used before its declaration (line " };
static const wstr S393 = { 5, 5, 1, 0, "param" };
static const wstr S394 = { 27, 27, 1, 0, "'return' outside a function" };
static const wstr S395 = { 16, 16, 1, 0, "' outside a loop" };
static const wstr S396 = { 56, 56, 1, 0, "'fail' outside a function: there is no caller to fail to" };
static const wstr S397 = { 65, 65, 1, 0, "'onfail' handles a call, but this statement's value is not a call" };
static const wstr S398 = { 87, 87, 1, 0, "the call must be the outermost expression, e.g. ;const n is s(parse) onfail (c) { ... }" };
static const wstr S399 = { 44, 44, 1, 0, "a match cannot mix literals and enum members" };
static const wstr S400 = { 31, 31, 1, 0, "a match cannot mix members of '" };
static const wstr S401 = { 7, 7, 1, 0, "' and '" };
static const wstr S402 = { 21, 21, 1, 0, "duplicate case value " };
static const wstr S403 = { 7, 7, 1, 0, " (line " };
static const wstr S404 = { 22, 22, 1, 0, "match does not handle " };
static const wstr S405 = { 2, 2, 1, 0, ", " };
static const wstr S406 = { 57, 57, 1, 0, "add a case for each, or end the match with 'else { ... }'" };
static const wstr S407 = { 52, 52, 1, 0, "a match used as a value must end with 'else { ... }'" };
static const wstr S408 = { 6, 6, 1, 0, "module" };
static const wstr S409 = { 19, 19, 1, 0, "' does not export '" };
static const wstr S410 = { 17, 17, 1, 0, "' is not a module" };
static const wstr S411 = { 16, 16, 1, 0, "unknown module '" };
static const wstr S412 = { 41, 41, 1, 0, "' is a function; functions are not values" };
static const wstr S413 = { 20, 20, 1, 0, "to call it, write x(" };
static const wstr S414 = { 7, 7, 1, 0, "), or (" };
static const wstr S415 = { 19, 19, 1, 0, ") with no arguments" };
static const wstr S416 = { 25, 25, 1, 0, "' is an enum, not a value" };
static const wstr S417 = { 24, 24, 1, 0, "its members are written " };
static const wstr S418 = { 7, 7, 1, 0, ".member" };
static const wstr S419 = { 54, 54, 1, 0, "case values are literals or enum members (Enum.member)" };
static const wstr S420 = { 16, 16, 1, 0, "' is not an enum" };
static const wstr S421 = { 14, 14, 1, 0, "unknown enum '" };
static const wstr S422 = { 11, 11, 1, 0, "no member '" };
static const wstr S423 = { 11, 11, 1, 0, "' in enum '" };
static const wstr S424 = { 16, 16, 1, 0, "its members are " };
static const wstr S425 = { 66, 66, 1, 0, "a conditional used as a value must end with a plain 'else { ... }'" };
static const wstr S426 = { 12, 12, 1, 0, "cannot set '" };
static const wstr S427 = { 25, 25, 1, 0, "': it is not a var or ref" };
static const wstr S428 = { 7, 7, 1, 0, "a field" };
static const wstr S429 = { 10, 10, 1, 0, "an element" };
static const wstr S430 = { 29, 29, 1, 0, "': enum members are constants" };
static const wstr S431 = { 9, 9, 1, 0, "': only '" };
static const wstr S432 = { 12, 12, 1, 0, "' may set it" };
static const wstr S433 = { 14, 14, 1, 0, "cannot modify " };
static const wstr S434 = { 5, 5, 1, 0, " of '" };
static const wstr S435 = { 18, 18, 1, 0, "': it is not a var" };
static const wstr S436 = { 6, 6, 1, 0, "' is a" };
static const wstr S437 = { 15, 15, 1, 0, ", not a binding" };
static const wstr S438 = { 24, 24, 1, 0, "mark the parameter (ref " };
static const wstr S439 = { 24, 24, 1, 0, ") to rebind it, or (var " };
static const wstr S440 = { 20, 20, 1, 0, ") for a mutable copy" };
static const wstr S441 = { 29, 29, 1, 0, "mark the parameter var: (var " };
static const wstr S442 = { 22, 22, 1, 0, "declare it with ';ref " };
static const wstr S443 = { 32, 32, 1, 0, " is ...' to rebind it, or ';var " };
static const wstr S444 = { 27, 27, 1, 0, " is ...' for a mutable copy" };
static const wstr S445 = { 22, 22, 1, 0, "declare it with ';var " };
static const wstr S446 = { 8, 8, 1, 0, " is ...'" };
static const wstr S447 = { 32, 32, 1, 0, "unknown standard library name '$" };
static const wstr S448 = { 26, 26, 1, 0, "' is a module, not a value" };
static const wstr S449 = { 27, 27, 1, 0, "what it exports is written " };
static const wstr S450 = { 5, 5, 1, 0, ".name" };
static const wstr S451 = { 32, 32, 1, 0, "' is a function; call it with ($" };
static const wstr S452 = { 30, 30, 1, 0, "'fail' is a statement: ;fail c" };
static const wstr S453 = { 24, 24, 1, 0, "checker: unhandled node " };
static const wstr S454 = { 31, 31, 1, 0, "' is a constant, not a function" };
static const wstr S455 = { 8, 8, 1, 0, "' takes " };
static const wstr S456 = { 6, 6, 1, 0, ", got " };
static const wstr S457 = { 18, 18, 1, 0, "unknown function '" };
static const wstr S458 = { 71, 71, 1, 0, ") is a call with no arguments; to use the value, remove the parentheses" };
static const wstr S459 = { 53, 53, 1, 0, "calls are postfix: the function comes last, e.g. x(f)" };
static const wstr S460 = { 19, 19, 1, 0, "' is not a function" };
static const wstr S461 = { 21, 21, 1, 0, "' is already imported" };
static const wstr S462 = { 38, 38, 1, 0, "' is not a library: it ends with ;exit" };
static const wstr S463 = { 31, 31, 1, 0, "only a library ends with ';end'" };
static const wstr S464 = { 25, 25, 1, 0, "a program ends with ;exit" };
static const wstr S465 = { 88, 88, 1, 0, "a library's top level may contain only import, fn, const, var, ref and enum declarations" };
static const wstr S466 = { 37, 37, 1, 0, "move other statements into a function" };
static const wstr S467 = { 28, 28, 1, 0, "a library must end with ;end" };
static const wstr S468 = { 56, 56, 1, 0, "mark what it exports with 'export', and end it with ;end" };
static const wstr S469 = { 15, 15, 1, 0, "cannot export '" };
static const wstr S470 = { 38, 38, 1, 0, "': it is not declared at the top level" };
static const wstr S471 = { 17, 17, 1, 0, "': it is a module" };
static const wstr S472 = { 5, 5, 1, 0, "line " };
static const wstr S473 = { 6, 6, 1, 0, " line " };
static const wstr S474 = { 49, 49, 1, 0, "' never produces a value, but its value is used (" };
static const wstr S475 = { 65, 65, 1, 0, "end the function with its value, or with null if that is intended" };
static const wstr S476 = { 29, 29, 1, 0, "a program must end with ;exit" };
static const wstr S477 = { 50, 50, 1, 0, "end the program with ;exit, or a library with ;end" };
static const wstr S478 = { 3, 3, 1, 0, "add" };
static const wstr S479 = { 3, 3, 1, 0, "sub" };
static const wstr S480 = { 3, 3, 1, 0, "mul" };
static const wstr S481 = { 3, 3, 1, 0, "div" };
static const wstr S482 = { 3, 3, 1, 0, "mod" };
static const wstr S483 = { 3, 3, 1, 0, "pow" };
static const wstr S484 = { 2, 2, 1, 0, "eq" };
static const wstr S485 = { 2, 2, 1, 0, "ne" };
static const wstr S486 = { 2, 2, 1, 0, "lt" };
static const wstr S487 = { 2, 2, 1, 0, "gt" };
static const wstr S488 = { 2, 2, 1, 0, "le" };
static const wstr S489 = { 2, 2, 1, 0, "ge" };
static const wstr S490 = { 4, 4, 1, 0, "band" };
static const wstr S491 = { 3, 3, 1, 0, "bor" };
static const wstr S492 = { 4, 4, 1, 0, "bxor" };
static const wstr S493 = { 3, 3, 1, 0, "shl" };
static const wstr S494 = { 3, 3, 1, 0, "shr" };
static const wstr S495 = { 3, 3, 1, 0, "neg" };
static const wstr S496 = { 3, 3, 1, 0, "not" };
static const wstr S497 = { 4, 4, 1, 0, "bnot" };
static const wstr S498 = { 2, 2, 1, 0, "\\\"" };
static const wstr S499 = { 2, 2, 1, 0, "\\\\" };
static const wstr S500 = { 2, 2, 1, 0, "{ " };
static const wstr S501 = { 5, 5, 1, 0, ", 0, " };
static const wstr S502 = { 2, 2, 1, 0, " }" };
static const wstr S503 = { 4, 4, 1, 0, "NULL" };
static const wstr S504 = { 8, 8, 1, 0, "(wv[]){ " };
static const wstr S505 = { 1, 1, 1, 0, "w" };
static const wstr S506 = { 1, 1, 1, 0, "v" };
static const wstr S507 = { 2, 2, 1, 0, "  " };
static const wstr S508 = { 2, 2, 1, 0, "&S" };
static const wstr S509 = { 3, 3, 1, 0, "&F_" };
static const wstr S510 = { 19, 19, 1, 0, "static const wmem l" };
static const wstr S511 = { 7, 7, 1, 0, "[] = { " };
static const wstr S512 = { 3, 3, 1, 0, " };" };
static const wstr S513 = { 1, 1, 1, 0, "l" };
static const wstr S514 = { 3, 3, 1, 0, "fr." };
static const wstr S515 = { 2, 2, 1, 0, "up" };
static const wstr S516 = { 4, 4, 1, 0, "->up" };
static const wstr S517 = { 2, 2, 1, 0, "->" };
static const wstr S518 = { 7, 7, 1, 0, "rt_def(" };
static const wstr S519 = { 3, 3, 1, 0, "wv " };
static const wstr S520 = { 3, 3, 1, 0, " = " };
static const wstr S521 = { 3, 3, 1, 0, "({ " };
static const wstr S522 = { 4, 4, 1, 0, "; })" };
static const wstr S523 = { 12, 12, 1, 0, " = RT_UNSET;" };
static const wstr S524 = { 6, 6, 1, 0, "(void)" };
static const wstr S525 = { 7, 7, 1, 0, "RT_NULL" };
static const wstr S526 = { 66, 66, 1, 0, "'return' inside a conditional used as a value is not supported yet" };
static const wstr S527 = { 7, 7, 1, 0, "RT_RET(" };
static const wstr S528 = { 2, 2, 1, 0, ");" };
static const wstr S529 = { 9, 9, 1, 0, "rt_panic(" };
static const wstr S530 = { 48, 48, 1, 0, "' cannot leave a conditional used as a value yet" };
static const wstr S531 = { 6, 6, 1, 0, "break;" };
static const wstr S532 = { 9, 9, 1, 0, "continue;" };
static const wstr S533 = { 7, 7, 1, 0, "while (" };
static const wstr S534 = { 8, 8, 1, 0, "rt_cond(" };
static const wstr S535 = { 4, 4, 1, 0, ")) {" };
static const wstr S536 = { 5, 5, 1, 0, "rt_i(" };
static const wstr S537 = { 8, 8, 1, 0, "rt_exit(" };
static const wstr S538 = { 7, 7, 1, 0, "rt_i(0)" };
static const wstr S539 = { 29, 29, 1, 0, "codegen: unhandled statement " };
static const wstr S540 = { 13, 13, 1, 0, "rt_fail_with(" };
static const wstr S541 = { 16, 16, 1, 0, "RT_RET(RT_NULL);" };
static const wstr S542 = { 9, 9, 1, 0, "struct fr" };
static const wstr S543 = { 4, 4, 1, 0, " *up" };
static const wstr S544 = { 2, 2, 1, 0, "p_" };
static const wstr S545 = { 10, 10, 1, 0, "static wv " };
static const wstr S546 = { 4, 4, 1, 0, "void" };
static const wstr S547 = { 9, 9, 1, 0, "rt_enter(" };
static const wstr S548 = { 8, 8, 1, 0, " fr = { " };
static const wstr S549 = { 8, 8, 1, 0, ".up = up" };
static const wstr S550 = { 8, 8, 1, 0, "rt_bind(" };
static const wstr S551 = { 13, 13, 1, 0, "(void)rt_def(" };
static const wstr S552 = { 3, 3, 1, 0, "rt_" };
static const wstr S553 = { 7, 7, 1, 0, "rt_set_" };
static const wstr S554 = { 7, 7, 1, 0, "rt_upd_" };
static const wstr S555 = { 5, 5, 1, 0, ", rt_" };
static const wstr S556 = { 7, 7, 1, 0, "rt_arr(" };
static const wstr S557 = { 7, 7, 1, 0, "rt_map(" };
static const wstr S558 = { 1, 1, 1, 0, "K" };
static const wstr S559 = { 7, 7, 1, 0, "rt_rec(" };
static const wstr S560 = { 18, 18, 1, 0, "if (rt_failcode) {" };
static const wstr S561 = { 17, 17, 1, 0, "rt_i(rt_failcode)" };
static const wstr S562 = { 16, 16, 1, 0, "rt_failcode = 0;" };
static const wstr S563 = { 3, 3, 1, 0, "raw" };
static const wstr S564 = { 7, 7, 1, 0, "} else " };
static const wstr S565 = { 4, 4, 1, 0, "if (" };
static const wstr S566 = { 3, 3, 1, 0, ") {" };
static const wstr S567 = { 8, 8, 1, 0, "} else {" };
static const wstr S568 = { 3, 3, 1, 0, " ? " };
static const wstr S569 = { 3, 3, 1, 0, " : " };
static const wstr S570 = { 3, 3, 1, 0, "({\n" };
static const wstr S571 = { 2, 2, 1, 0, "})" };
static const wstr S572 = { 3, 3, 1, 0, "eq(" };
static const wstr S573 = { 4, 4, 1, 0, " || " };
static const wstr S574 = { 12, 12, 1, 0, "(rt_no_case(" };
static const wstr S575 = { 11, 11, 1, 0, "), RT_NULL)" };
static const wstr S576 = { 6, 6, 1, 0, "({ wv " };
static const wstr S577 = { 2, 2, 1, 0, "; " };
static const wstr S578 = { 4, 4, 1, 0, "std_" };
static const wstr S579 = { 3, 3, 1, 0, "&fr" };
static const wstr S580 = { 7, 7, 1, 0, "rt_chk(" };
static const wstr S581 = { 5, 5, 1, 0, "rt_f(" };
static const wstr S582 = { 5, 5, 1, 0, "rt_s(" };
static const wstr S583 = { 8, 8, 1, 0, "rt_text(" };
static const wstr S584 = { 7, 7, 1, 0, "rt_cat(" };
static const wstr S585 = { 5, 5, 1, 0, "rt_b(" };
static const wstr S586 = { 6, 6, 1, 0, "RT_NAN" };
static const wstr S587 = { 6, 6, 1, 0, "RT_INF" };
static const wstr S588 = { 2, 2, 1, 0, "()" };
static const wstr S589 = { 7, 7, 1, 0, "rt_idx(" };
static const wstr S590 = { 6, 6, 1, 0, "rt_m(&" };
static const wstr S591 = { 2, 2, 1, 0, "])" };
static const wstr S592 = { 7, 7, 1, 0, "rt_get(" };
static const wstr S593 = { 13, 13, 1, 0, "rt_b(rt_bool(" };
static const wstr S594 = { 3, 3, 1, 0, ", \"" };
static const wstr S595 = { 3, 3, 1, 0, "\") " };
static const wstr S596 = { 9, 9, 1, 0, " rt_bool(" };
static const wstr S597 = { 3, 3, 1, 0, "\"))" };
static const wstr S598 = { 9, 9, 1, 0, "(rt_cond(" };
static const wstr S599 = { 4, 4, 1, 0, ") ? " };
static const wstr S600 = { 30, 30, 1, 0, "codegen: unhandled expression " };
static const wstr S601 = { 2, 2, 1, 0, "\r\n" };
static const wstr S602 = { 20, 20, 1, 0, "// ---- program ----" };
static const wstr S603 = { 19, 19, 1, 0, "static const wstr S" };
static const wstr S604 = { 14, 14, 1, 0, "static wstr F_" };
static const wstr S605 = { 26, 26, 1, 0, "static const wstr *const K" };
static const wstr S606 = { 5, 5, 1, 0, " *up;" };
static const wstr S607 = { 10, 10, 1, 0, "char none;" };
static const wstr S608 = { 3, 3, 1, 0, " { " };
static const wstr S609 = { 2, 2, 1, 0, " {" };
static const wstr S610 = { 28, 28, 1, 0, "static void wvt_main(void) {" };
static const wstr S611 = { 10, 10, 1, 0, "  rt_syms(" };
static const wstr S612 = { 19, 19, 1, 0, ", (wstr *const[]){ " };
static const wstr S613 = { 4, 4, 1, 0, " });" };
static const wstr S614 = { 11, 11, 1, 0, "  rt_roots(" };
static const wstr S615 = { 17, 17, 1, 0, ", (wv *const[]){ " };
static const wstr S616 = { 4, 4, 1, 0, "  | " };
static const wstr S617 = { 4, 4, 1, 0, "  { " };
static const wstr S618 = { 4, 4, 1, 0, "    " };
static const wstr S619 = { 3, 3, 1, 0, "  }" };
static const wstr S620 = { 5, 5, 1, 0, "array" };
static const wstr S621 = { 2, 2, 1, 0, "[]" };
static const wstr S622 = { 6, 6, 1, 0, "record" };
static const wstr S623 = { 2, 2, 1, 0, ".." };
static const wstr S624 = { 14, 14, 1, 0, "import cycle: " };
static const wstr S625 = { 4, 4, 1, 0, " -> " };
static const wstr S626 = { 13, 13, 1, 0, "cannot read '" };
static const wstr S627 = { 3, 1, 0, 0, "﻿" };
static const wstr S628 = { 7, 7, 1, 0, "usage:\n" };
static const wstr S629 = { 50, 50, 1, 0, "  wvc <file.wvt> [-o <out.c>] [--runtime <wvt.c>]\n" };
static const wstr S630 = { 76, 76, 1, 0, "  wvc build <file.wvt> [-o <program>] [--cc <compiler>] [--runtime <wvt.c>]\n" };
static const wstr S631 = { 26, 26, 1, 0, "  wvc --tokens <file.wvt>\n" };
static const wstr S632 = { 22, 22, 1, 0, "  wvc --ast <file.wvt>" };
static const wstr S633 = { 17, 17, 1, 0, "wvc: cannot read " };
static const wstr S634 = { 5, 5, 1, 0, "build" };
static const wstr S635 = { 8, 8, 1, 0, "--tokens" };
static const wstr S636 = { 5, 5, 1, 0, "--ast" };
static const wstr S637 = { 2, 2, 1, 0, "-o" };
static const wstr S638 = { 9, 9, 1, 0, "--runtime" };
static const wstr S639 = { 4, 4, 1, 0, "--cc" };
static const wstr S640 = { 2, 2, 1, 0, "OS" };
static const wstr S641 = { 10, 10, 1, 0, "Windows_NT" };
static const wstr S642 = { 3, 3, 1, 0, "ZIG" };
static const wstr S643 = { 3, 3, 1, 0, "zig" };
static const wstr S644 = { 5, 5, 1, 0, "wvc: " };
static const wstr S645 = { 52, 52, 1, 0, " does not end with .wvt, so name the program with -o" };
static const wstr S646 = { 4, 4, 1, 0, ".exe" };
static const wstr S647 = { 13, 13, 1, 0, "runtime/wvt.c" };
static const wstr S648 = { 3, 3, 1, 0, "../" };
static const wstr S649 = { 2, 2, 1, 0, ".c" };
static const wstr S650 = { 18, 18, 1, 0, "wvc: cannot write " };
static const wstr S651 = { 3, 3, 1, 0, "-O2" };
static const wstr S652 = { 7, 7, 1, 0, "zig.exe" };
static const wstr S653 = { 2, 2, 1, 0, "cc" };
static const wstr S654 = { 16, 16, 1, 0, "wvc: cannot run " };
static const wstr S655 = { 67, 67, 1, 0, ": install Zig (https://ziglang.org), or name a C compiler with --cc" };
static const wstr S656 = { 23, 23, 1, 0, " failed with exit code " };
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
static wstr F_code = { 4, 4, 1, 0, "code" };
static wstr F_arg = { 3, 3, 1, 0, "arg" };
static wstr F_paren = { 5, 5, 1, 0, "paren" };
static wstr F_path = { 4, 4, 1, 0, "path" };
static wstr F_pathPos = { 7, 7, 1, 0, "pathPos" };
static wstr F_names = { 5, 5, 1, 0, "names" };
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
static const wstr *const K13[] = { &F_k, &F_code, &F_pos, &F_end, &F_handler };
static const wstr *const K14[] = { &F_k, &F_arg, &F_pos, &F_paren };
static const wstr *const K15[] = { &F_k, &F_path, &F_pathPos, &F_name, &F_namePos, &F_pos, &F_end, &F_handler };
static const wstr *const K16[] = { &F_name, &F_pos };
static const wstr *const K17[] = { &F_k, &F_names, &F_pos, &F_end, &F_handler };
static const wstr *const K18[] = { &F_name, &F_isVar, &F_isRef, &F_pos };
static const wstr *const K19[] = { &F_k, &F_name, &F_namePos, &F_params, &F_body, &F_pos, &F_end, &F_handler };
static const wstr *const K20[] = { &F_k, &F_name, &F_namePos, &F_members, &F_pos, &F_end, &F_handler };
static const wstr *const K21[] = { &F_stmts, &F_value, &F_close };
static const wstr *const K22[] = { &F_k, &F_c, &F_a, &F_b, &F_pos, &F_paren };
static const wstr *const K23[] = { &F_k, &F_op, &F_l, &F_r, &F_pos, &F_paren };
static const wstr *const K24[] = { &F_k, &F_v, &F_pos, &F_paren };
static const wstr *const K25[] = { &F_k, &F_op, &F_e, &F_pos, &F_paren };
static const wstr *const K26[] = { &F_k, &F_index, &F_arr, &F_pos, &F_paren };
static const wstr *const K27[] = { &F_k, &F_field, &F_rec, &F_pos, &F_paren };
static const wstr *const K28[] = { &F_k, &F_name, &F_pos, &F_paren };
static const wstr *const K29[] = { &F_k, &F_callee, &F_args, &F_pos, &F_paren };
static const wstr *const K30[] = { &F_k, &F_parts, &F_pos, &F_paren };
static const wstr *const K31[] = { &F_k, &F_pos, &F_paren };
static const wstr *const K32[] = { &F_k, &F_items, &F_pos, &F_paren };
static const wstr *const K33[] = { &F_name, &F_e, &F_pos };
static const wstr *const K34[] = { &F_k, &F_fields, &F_pos, &F_paren };
static const wstr *const K35[] = { &F_values, &F_body, &F_pos };
static const wstr *const K36[] = { &F_k, &F_subject, &F_cases, &F_els, &F_pos, &F_paren };
static const wstr *const K37[] = { &F_neg, &F_cond, &F_body, &F_pos };
static const wstr *const K38[] = { &F_k, &F_branches, &F_els, &F_pos, &F_paren };
static const wstr *const K39[] = { &F_name, &F_arity };
static const wstr *const K40[] = { &F_parent, &F_names };
static const wstr *const K41[] = { &F_kind, &F_name, &F_isVar, &F_isRef, &F_namePos, &F_visibleAt, &F_fnId, &F_arity, &F_mod, &F_lib };
static const wstr *const K42[] = { &F_mod, &F_name, &F_namePos, &F_body };
static const wstr *const K43[] = { &F_mod, &F_name, &F_members };
static const wstr *const K44[] = { &F_id, &F_top };
static const wstr *const K45[] = { &F_func, &F_loops };
static const wstr *const K46[] = { &F_key, &F_pos };
static const wstr *const K47[] = { &F_mod, &F_namePos, &F_pos, &F_useMod };
static const wstr *const K48[] = { &F_stmts, &F_value };
static const wstr *const K49[] = { &F_name, &F_kind, &F_d };
static const wstr *const K50[] = { &F_name, &F_parent, &F_depth, &F_top, &F_children, &F_cname };
static const wstr *const K51[] = { &F_kind, &F_name, &F_func, &F_cname, &F_isRef, &F_captured, &F_fnIndex };
static const wstr *const K52[] = { &F_name, &F_lib };
static const wstr *const K53[] = { &F_c, &F_node };
static const wstr *const K54[] = { &F_c, &F_k };
static const wstr *const K55[] = { &F_pre, &F_cs };
static const wstr *const K56[] = { &F_proto, &F_lines };
static const wstr *const K57[] = { &F_k, &F_js, &F_pos, &F_paren };
static const wstr *const K58[] = { &F_node, &F_lib };
static const wstr *const K59[] = { &F_file, &F_chars, &F_path, &F_program, &F_imports, &F_exports, &F_notLibrary, &F_refTargets };
struct fr27 { wv v71_cs; wv v72_n; wv v73_i; wv v74_prevT; wv v75_nlNow; };
struct fr49 { wv v184_toks; wv v186_p; wv v187_depth; wv v188_exports; wv v189_firstExport; };
struct fr100 { wv v384_modules; wv v386_errors; wv v387_nextFn; wv v388_curMod; wv v389_functions; wv v390_uses; wv v391_enums; wv v424_topFn; };
struct fr142 { wv v646_modules; wv v647_runtime; wv v648_n; wv v649_tmps; wv v650_strings; wv v651_fields; wv v652_shapes; wv v653_enumDefs; wv v654_globals; wv v655_frames; wv v656_functions; wv v657_out; wv v658_indent; wv v659_loops; wv v660_iife; wv v661_decls; wv v662_fns; wv v663_tops; wv v664_slots; wv v665_curMod; wv v666_mod; wv v667_curSlot; wv v668_stolen; wv v669_ownEnums; wv v670_cur; };
struct fr212 { wv v1051_modules; };
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
static wv w_arg;
static wv w_inspect;
static wv w_dotWvt;
static wv w_runtimeGiven;
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
static wv v1078_a;
static wv v1087_tokens;
static wv v1088_program;
static wv v1089_p;
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
static wv s89_interpolation(struct fr27 *up, wv v160_open);
static wv w4_precedence(wv v169_t);
static wv w4_describe(wv v170_tk);
static wv w4_failAtHint(wv v171_tk, wv v172_message, wv v173_hint);
static wv w4_failAt(wv v174_tk, wv v175_message);
static wv w4_simpleStatement(wv v176_k, wv v177_e);
static wv w4_parse(wv v178_toks);
static wv w4_hasEffect(wv v179_e);
static wv w4_parser(wv p_v184_toks, wv v185_inner);
static wv s190_peek(struct fr49 *up);
static wv s191_at(struct fr49 *up, wv v229_type);
static wv s192_atWord(struct fr49 *up, wv v230_word);
static wv s193_atCall(struct fr49 *up);
static wv s194_next(struct fr49 *up);
static wv s195_accept(struct fr49 *up, wv v232_type);
static wv s196_expect(struct fr49 *up, wv v233_type, wv v234_msg, wv v235_hint);
static wv s197_expectWord(struct fr49 *up, wv v238_word, wv v239_hint);
static wv s198_lintSpace(struct fr49 *up, wv v241_op);
static wv s199_program(struct fr49 *up);
static wv s200_statement(struct fr49 *up);
static wv s201_statementBody(struct fr49 *up, wv v254_tk);
static wv s202_declaration(struct fr49 *up);
static wv s203_setStatement(struct fr49 *up);
static wv s204_failStatement(struct fr49 *up);
static wv s205_importStatement(struct fr49 *up);
static wv s206_exportStatement(struct fr49 *up);
static wv s207_endStatement(struct fr49 *up);
static wv s208_fnStatement(struct fr49 *up);
static wv s209_enumStatement(struct fr49 *up);
static wv s210_cond(struct fr49 *up, wv v289_kw);
static wv s211_block(struct fr49 *up);
static wv s212_lvalue(struct fr49 *up);
static wv s213_expr(struct fr49 *up);
static wv s214_ternary(struct fr49 *up);
static wv s215_ternaryTail(struct fr49 *up, wv v299_c);
static wv s216_binary(struct fr49 *up, wv v304_min);
static wv s217_binaryFrom(struct fr49 *up, wv v305_left, wv v306_min);
static wv s218_unary(struct fr49 *up);
static wv s219_powerTail(struct fr49 *up, wv v313_base);
static wv s220_postfix(struct fr49 *up);
static wv s221_call(struct fr49 *up, wv v320_args);
static wv s222_primary(struct fr49 *up);
static wv s223_record(struct fr49 *up, wv v334_open);
static wv s224_constantKey(struct fr49 *up, wv v342_e);
static wv s225_mapLiteral(struct fr49 *up, wv v343_open, wv v344_first);
static wv s226_parseMatch(struct fr49 *up);
static wv s227_caseValue(struct fr49 *up);
static wv s228_chain(struct fr49 *up);
static wv w5_stdSig(wv v366_name);
static wv w5_modeOf(wv v368_x);
static wv w5_kindText(wv v369_kind);
static wv w5_plural(wv v370_n);
static wv w5_enumText(wv v371_x);
static wv w5_caseKey(wv v372_v);
static wv w5_caseText(wv v373_v);
static wv w5_getsAwayWithValue(wv v374_list);
static wv w5_lineIn(wv v379_chars, wv v380_pos);
static wv w5_lineOf(wv v383_pos);
static wv w5_check(wv p_v384_modules, wv v385_requireExit);
static wv s392_error(struct fr100 *up, wv v429_pos, wv v430_message, wv v431_hint);
static wv s393_enter(struct fr100 *up, wv v432_m);
static wv s394_enumMembers(struct fr100 *up, wv v433_d);
static wv s395_firstUse(struct fr100 *up, wv v435_mod, wv v436_declPos);
static wv s396_requireValue(struct fr100 *up, wv v438_b);
static wv s397_newScope(struct fr100 *up, wv v441_parent);
static wv s398_decl(struct fr100 *up, wv v443_kind, wv v444_name, wv v445_mode, wv v446_namePos, wv v447_visibleAt, wv v448_fnId, wv v449_arity);
static wv s399_own(struct fr100 *up, wv v450_scope, wv v451_name);
static wv s400_declare(struct fr100 *up, wv v453_scope, wv v454_name, wv v455_d);
static wv s401_lookup(struct fr100 *up, wv v459_scope, wv v460_name);
static wv s402_resolve(struct fr100 *up, wv v463_name, wv v464_pos, wv v465_scope, wv v466_ctx);
static wv s403_block(struct fr100 *up, wv v468_b, wv v469_scope, wv v470_ctx);
static wv s404_stmt(struct fr100 *up, wv v475_s, wv v476_scope, wv v477_ctx);
static wv s405_onfailHandler(struct fr100 *up, wv v484_s, wv v485_scope, wv v486_ctx);
static wv s406_checkMatch(struct fr100 *up, wv v489_e, wv v490_scope, wv v491_ctx, wv v492_stmtLevel);
static wv s407_moduleNamed(struct fr100 *up, wv v512_x, wv v513_scope);
static wv s408_exported(struct fr100 *up, wv v515_lib, wv v516_name);
static wv s409_enumNamed(struct fr100 *up, wv v519_x, wv v520_scope);
static wv s410_exportOf(struct fr100 *up, wv v524_e, wv v525_m);
static wv s411_unknownModule(struct fr100 *up, wv v527_x, wv v528_scope);
static wv s412_qualified(struct fr100 *up, wv v530_e, wv v531_m);
static wv s413_caseEnum(struct fr100 *up, wv v534_v, wv v535_scope);
static wv s414_member(struct fr100 *up, wv v541_e, wv v542_d);
static wv s415_chain(struct fr100 *up, wv v547_e, wv v548_scope, wv v549_ctx, wv v550_stmtLevel);
static wv s416_lvalue(struct fr100 *up, wv v553_t, wv v554_scope, wv v555_ctx);
static wv s417_mustBeMutable(struct fr100 *up, wv v562_d, wv v563_name, wv v564_pos, wv v565_message, wv v566_rebind);
static wv s418_unknownStd(struct fr100 *up, wv v568_node);
static wv s419_expr(struct fr100 *up, wv v569_e, wv v570_scope, wv v571_ctx);
static wv s420_callee(struct fr100 *up, wv v581_e, wv v582_scope, wv v583_used);
static wv s421_importNames(struct fr100 *up, wv v591_m, wv v592_scope);
static wv s422_libraryRules(struct fr100 *up, wv v601_m, wv v602_scope, wv v603_isEntry, wv v604_last);
static wv s423_checkModule(struct fr100 *up, wv v611_m, wv v612_isEntry);
static wv w6_binFn(wv v623_op);
static wv w6_modeOf(wv v625_x);
static wv w6_unFn(wv v626_op);
static wv w6_repeat(wv v627_s, wv v628_count);
static wv w6_utf8Length(wv v631_s);
static wv w6_cString(wv v636_s);
static wv w6_wstrInit(wv v642_s);
static wv w6_values(wv v645_cs);
static wv w6_generateC(wv p_v646_modules, wv p_v647_runtime);
static wv s671_modId(struct fr142 *up, wv v732_m);
static wv s672_hasFrame(struct fr142 *up, wv v733_f);
static wv s673_newScope(struct fr142 *up, wv v734_parent);
static wv s674_put(struct fr142 *up, wv v736_scope, wv v737_name, wv v738_kind, wv v739_d);
static wv s675_find(struct fr142 *up, wv v740_scope, wv v741_name);
static wv s676_kindOf(struct fr142 *up, wv v745_scope, wv v746_name);
static wv s677_newFn(struct fr142 *up, wv v748_name, wv v749_parent);
static wv s678_bind(struct fr142 *up, wv v751_kind, wv v752_name, wv v753_f, wv v754_atTop, wv v755_mode);
static wv s679_ablock(struct fr142 *up, wv v758_b, wv v759_scope, wv v760_f, wv v761_topScope);
static wv s680_astmt(struct fr142 *up, wv v769_s, wv v770_scope, wv v771_f, wv v772_topScope);
static wv s681_use(struct fr142 *up, wv v783_x, wv v784_scope, wv v785_f);
static wv s682_atarget(struct fr142 *up, wv v787_t, wv v788_scope, wv v789_f);
static wv s683_notValue(struct fr142 *up, wv v790_x, wv v791_scope);
static wv s684_aexpr(struct fr142 *up, wv v793_e, wv v794_scope, wv v795_f);
static wv s685_analyze(struct fr142 *up, wv v803_m);
static wv s686_enter(struct fr142 *up, wv v810_m);
static wv s687_emit(struct fr142 *up, wv v814_line);
static wv s688_tmp(struct fr142 *up);
static wv s689_str(struct fr142 *up, wv v816_s);
static wv s690_fieldRef(struct fr142 *up, wv v818_name);
static wv s691_enumDef(struct fr142 *up, wv v819_s, wv v820_m);
static wv s692_stolenFrom(struct fr142 *up, wv v824_x);
static wv s693_enumIn(struct fr142 *up, wv v826_list, wv v827_name);
static wv s694_enumOf(struct fr142 *up, wv v829_x);
static wv s695_place(struct fr142 *up, wv v834_d);
static wv s696_tdz(struct fr142 *up, wv v836_d);
static wv s697_read(struct fr142 *up, wv v837_d);
static wv s698_seq(struct fr142 *up, wv v838_args);
static wv s699_wrap(struct fr142 *up, wv v845_sq, wv v846_inner);
static wv s700_stmts(struct fr142 *up, wv v847_list);
static wv s701_resets(struct fr142 *up, wv v849_b);
static wv s702_blockBody(struct fr142 *up, wv v853_b);
static wv s703_valueOf(struct fr142 *up, wv v854_b);
static wv s704_stmt(struct fr142 *up, wv v855_s);
static wv s705_signal(struct fr142 *up, wv v857_code);
static wv s706_bindDecl(struct fr142 *up, wv v858_d, wv v859_value);
static wv s707_fnDef(struct fr142 *up, wv v861_s);
static wv s708_bindRhs(struct fr142 *up, wv v881_e, wv v882_mode);
static wv s709_setStmt(struct fr142 *up, wv v884_s);
static wv s710_exprs(struct fr142 *up, wv v899_list);
static wv s711_array(struct fr142 *up, wv v902_e, wv v903_mutable);
static wv s712_mapLit(struct fr142 *up, wv v905_e, wv v906_mutable);
static wv s713_record(struct fr142 *up, wv v908_e, wv v909_mutable);
static wv s714_onfailStmt(struct fr142 *up, wv v917_s);
static wv s715_chainStmt(struct fr142 *up, wv v926_e);
static wv s716_test(struct fr142 *up, wv v929_b);
static wv s717_branchCount(struct fr142 *up, wv v930_e);
static wv s718_branchBody(struct fr142 *up, wv v931_e, wv v932_i);
static wv s719_branchTest(struct fr142 *up, wv v933_e, wv v934_t, wv v935_i);
static wv s720_isPlain(struct fr142 *up, wv v936_e);
static wv s721_ternaryChain(struct fr142 *up, wv v940_e, wv v941_t);
static wv s722_branchValue(struct fr142 *up, wv v944_e, wv v945_t, wv v946_subject);
static wv s723_chainExpr(struct fr142 *up, wv v955_e);
static wv s724_caseTest(struct fr142 *up, wv v956_t, wv v957_a);
static wv s725_matchElse(struct fr142 *up, wv v960_e, wv v961_t);
static wv s726_matchStmt(struct fr142 *up, wv v962_e);
static wv s727_matchExpr(struct fr142 *up, wv v966_e);
static wv s728_callC(struct fr142 *up, wv v970_callee, wv v971_args, wv v972_checked);
static wv s729_expr(struct fr142 *up, wv v981_e);
static wv s730_assemble(struct fr142 *up);
static wv w7_dumpTokens(wv v1011_toks, wv v1012_indent);
static wv w7_sorted(wv v1018_xs);
static wv w7_dumpValue(wv v1023_x, wv v1024_indent);
static wv w8_dirOf(wv v1033_path);
static wv w8_normalize(wv v1036_path);
static wv w8_with(wv v1041_xs, wv v1042_x);
static wv w8_failIn(wv v1045_file, wv v1046_chars, wv v1047_pos, wv v1048_message);
static wv w8_loadProgram(wv v1049_entryFile, wv v1050_entryChars);
static wv s1052_loaded(struct fr212 *up, wv v1055_path);
static wv s1053_visit(struct fr212 *up, wv v1057_file, wv v1058_chars, wv v1059_path, wv v1060_chain);
static wv w_die(void);
static wv w_load(wv v1074_file);
static wv w_onWindows(void);
static wv w_tryRead(wv v1080_path);
static wv w_programDir(void);
static wv w_readRuntime(void);

static wv w1_isSpace(wv v0_c) {
  rt_enter("isSpace");
  v0_c = rt_bind(v0_c, 0);
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v0_c, rt_s(&S0)), "||") || rt_bool(rt_eq(v0_c, rt_s(&S1)), "||")), "||") || rt_bool(rt_eq(v0_c, rt_s(&S2)), "||")), "||") || rt_bool(rt_eq(v0_c, rt_s(&S3)), "||")));
}

static wv w1_isDigit(wv v1_c) {
  rt_enter("isDigit");
  v1_c = rt_bind(v1_c, 0);
  RT_RET(rt_b(rt_bool(rt_ge(v1_c, rt_s(&S4)), "&&") && rt_bool(rt_le(v1_c, rt_s(&S5)), "&&")));
}

static wv w1_isHexDigit(wv v2_c) {
  rt_enter("isHexDigit");
  v2_c = rt_bind(v2_c, 0);
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w1_isDigit(v2_c), "isHexDigit"), "||") || rt_bool(rt_b(rt_bool(rt_ge(v2_c, rt_s(&S6)), "&&") && rt_bool(rt_le(v2_c, rt_s(&S7)), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v2_c, rt_s(&S8)), "&&") && rt_bool(rt_le(v2_c, rt_s(&S9)), "&&")), "||")));
}

static wv w1_isIdStart(wv v3_c) {
  rt_enter("isIdStart");
  v3_c = rt_bind(v3_c, 0);
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v3_c, rt_s(&S6)), "&&") && rt_bool(rt_le(v3_c, rt_s(&S10)), "&&")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v3_c, rt_s(&S8)), "&&") && rt_bool(rt_le(v3_c, rt_s(&S11)), "&&")), "||")), "||") || rt_bool(rt_eq(v3_c, rt_s(&S12)), "||")));
}

static wv w1_isIdChar(wv v4_c) {
  rt_enter("isIdChar");
  v4_c = rt_bind(v4_c, 0);
  RT_RET(rt_b(rt_bool(rt_chk(w1_isIdStart(v4_c), "isIdChar"), "||") || rt_bool(rt_chk(w1_isDigit(v4_c), "isIdChar"), "||")));
}

static wv w1_isJsSpace(wv v5_c) {
  rt_enter("isJsSpace");
  v5_c = rt_bind(v5_c, 0);
  if (rt_cond(rt_eq(v5_c, rt_s(&S13)))) {
    RT_RET(rt_b(0));
  }
  wv v6_k = rt_bind(std_code(v5_c), 0);
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v6_k, rt_i(9)), "&&") && rt_bool(rt_le(v6_k, rt_i(13)), "&&")), "||") || rt_bool(rt_eq(v6_k, rt_i(32)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(160)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(5760)), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v6_k, rt_i(8192)), "&&") && rt_bool(rt_le(v6_k, rt_i(8202)), "&&")), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8232)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8233)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8239)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(8287)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(12288)), "||")), "||") || rt_bool(rt_eq(v6_k, rt_i(65279)), "||")));
}

static wv w1_isSigTerm(wv v7_c) {
  rt_enter("isSigTerm");
  v7_c = rt_bind(v7_c, 0);
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v7_c, rt_s(&S13)), "||") || rt_bool(rt_chk(w1_isSpace(v7_c), "isSigTerm"), "||")), "||") || rt_bool(rt_eq(v7_c, rt_s(&S14)), "||")), "||") || rt_bool(rt_eq(v7_c, rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(v7_c, rt_s(&S16)), "||")));
}

static wv w1_contains(wv v8_xs, wv v9_x) {
  rt_enter("contains");
  v9_x = rt_bind(v9_x, 0);
  wv v10_k = rt_i(0);
  while (rt_cond(({ wv t0 = v10_k; wv t1 = std_length(v8_xs); rt_lt(t0, t1); }))) {
    if (rt_cond(({ wv t2 = rt_idx(v10_k, v8_xs); wv t3 = v9_x; rt_eq(t2, t3); }))) {
      RT_RET(rt_b(1));
    }
    v10_k = rt_add(v10_k, rt_i(1));
  }
  RT_RET(rt_b(0));
}

static wv w1_shiftCase(wv v11_s, wv v12_from, wv v13_shift) {
  rt_enter("shiftCase");
  v11_s = rt_bind(v11_s, 0);
  v12_from = rt_bind(v12_from, 0);
  v13_shift = rt_bind(v13_shift, 0);
  wv v14_cs = rt_bind(std_split(v11_s, rt_s(&S13)), 0);
  wv v15_out = rt_s(&S13);
  wv v16_k = rt_i(0);
  while (rt_cond(({ wv t4 = v16_k; wv t5 = std_length(v14_cs); rt_lt(t4, t5); }))) {
    wv v17_c = rt_bind(rt_idx(v16_k, v14_cs), 0);
    if (rt_cond(rt_b(rt_bool(rt_ge(v17_c, v12_from), "&&") && rt_bool(({ wv t6 = v17_c; wv t7 = std_fromCode(rt_add(std_code(v12_from), rt_i(25))); rt_le(t6, t7); }), "&&")))) {
      v15_out = ({ wv t10 = v15_out; wv t11 = std_fromCode(({ wv t8 = std_code(v17_c); wv t9 = v13_shift; rt_add(t8, t9); })); rt_add(t10, t11); });
    } else {
      v15_out = rt_add(v15_out, v17_c);
    }
    v16_k = rt_add(v16_k, rt_i(1));
  }
  RT_RET(v15_out);
}

static wv w1_toUpper(wv v18_s) {
  rt_enter("toUpper");
  v18_s = rt_bind(v18_s, 0);
  RT_RET(rt_chk(w1_shiftCase(v18_s, rt_s(&S6), rt_i(-32)), "toUpper"));
}

static wv w1_toLower(wv v19_s) {
  rt_enter("toLower");
  v19_s = rt_bind(v19_s, 0);
  RT_RET(rt_chk(w1_shiftCase(v19_s, rt_s(&S8), rt_i(32)), "toLower"));
}

static wv w1_hexValue(wv v20_s) {
  rt_enter("hexValue");
  v20_s = rt_bind(v20_s, 0);
  wv v21_cs = rt_bind(std_split(v20_s, rt_s(&S13)), 0);
  if (rt_cond(rt_b(rt_bool(rt_lt(std_length(v21_cs), rt_i(1)), "||") || rt_bool(rt_gt(std_length(v21_cs), rt_i(6)), "||")))) {
    RT_RET(rt_i(-1));
  }
  wv v22_v = rt_i(0);
  wv v23_k = rt_i(0);
  while (rt_cond(({ wv t12 = v23_k; wv t13 = std_length(v21_cs); rt_lt(t12, t13); }))) {
    wv v24_c = rt_bind(rt_idx(v23_k, v21_cs), 0);
    if (rt_cond(rt_not(rt_chk(w1_isHexDigit(v24_c), "hexValue")))) {
      RT_RET(rt_i(-1));
    }
    wv v25_digits = rt_bind(std_split(rt_s(&S17), rt_s(&S13)), 0);
    wv v26_d = rt_i(0);
    while (rt_cond(({ wv t14 = rt_chk(w1_toUpper(rt_idx(v26_d, v25_digits)), "hexValue"); wv t15 = rt_chk(w1_toUpper(v24_c), "hexValue"); rt_ne(t14, t15); }))) {
      v26_d = rt_add(v26_d, rt_i(1));
    }
    v22_v = ({ wv t16 = rt_mul(v22_v, rt_i(16)); wv t17 = v26_d; rt_add(t16, t17); });
    v23_k = rt_add(v23_k, rt_i(1));
  }
  RT_RET(v22_v);
}

static wv w1_quote(wv v27_s) {
  rt_enter("quote");
  v27_s = rt_bind(v27_s, 0);
  wv v28_cs = rt_bind(std_split(v27_s, rt_s(&S13)), 0);
  wv v29_out = rt_s(&S18);
  wv v30_k = rt_i(0);
  while (rt_cond(({ wv t18 = v30_k; wv t19 = std_length(v28_cs); rt_lt(t18, t19); }))) {
    wv v31_c = rt_bind(rt_idx(v30_k, v28_cs), 0);
    wv v32_code = rt_bind(std_code(v31_c), 0);
    if (rt_cond(rt_b(rt_bool(rt_eq(v31_c, rt_s(&S18)), "||") || rt_bool(rt_eq(v31_c, rt_s(&S19)), "||")))) {
      v29_out = ({ wv t20 = rt_add(v29_out, rt_s(&S19)); wv t21 = v31_c; rt_add(t20, t21); });
    } else if (rt_cond(rt_eq(v31_c, rt_s(&S2)))) {
      v29_out = rt_add(v29_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v31_c, rt_s(&S3)))) {
      v29_out = rt_add(v29_out, rt_s(&S21));
    } else if (rt_cond(rt_eq(v31_c, rt_s(&S1)))) {
      v29_out = rt_add(v29_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v32_code, rt_i(8)))) {
      v29_out = rt_add(v29_out, rt_s(&S23));
    } else if (rt_cond(rt_eq(v32_code, rt_i(12)))) {
      v29_out = rt_add(v29_out, rt_s(&S24));
    } else if (rt_cond(rt_lt(v32_code, rt_i(32)))) {
      wv v33_hex = rt_bind(std_split(rt_s(&S17), rt_s(&S13)), 0);
      v29_out = ({ wv t28 = ({ wv t24 = rt_add(v29_out, rt_s(&S25)); wv t25 = ({ wv t22 = std_floor(rt_div(v32_code, rt_i(16))); wv t23 = v33_hex; rt_idx(t22, t23); }); rt_add(t24, t25); }); wv t29 = ({ wv t26 = rt_mod(v32_code, rt_i(16)); wv t27 = v33_hex; rt_idx(t26, t27); }); rt_add(t28, t29); });
    } else {
      v29_out = rt_add(v29_out, v31_c);
    }
    v30_k = rt_add(v30_k, rt_i(1));
  }
  RT_RET(rt_add(v29_out, rt_s(&S18)));
}

static wv w2_setSource(wv v34_file, wv v35_chars) {
  rt_enter("setSource");
  v34_file = rt_bind(v34_file, 0);
  wv t30 = rt_bind(v34_file, 1);
  (void)rt_def(w2_srcFile, "srcFile");
  w2_srcFile = t30;
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
  RT_RET(({ wv t32 = v36_pos; wv t33 = v37_message; wv t34 = v38_hint; wv t35 = rt_def(w2_srcFile, "srcFile"); wv t36 = rt_def(w2_srcChars, "srcChars"); rt_rec(0, 6, K0, (wv[]){ rt_s(&S26), t32, t33, t34, t35, t36 }); }));
}

static wv w2_err(wv v39_pos, wv v40_message, wv v41_hint) {
  rt_enter("err");
  v39_pos = rt_bind(v39_pos, 0);
  v40_message = rt_bind(v40_message, 0);
  v41_hint = rt_bind(v41_hint, 0);
  RT_RET(({ wv t37 = v39_pos; wv t38 = v40_message; wv t39 = v41_hint; wv t40 = rt_def(w2_srcFile, "srcFile"); wv t41 = rt_def(w2_srcChars, "srcChars"); rt_rec(0, 6, K0, (wv[]){ rt_s(&S27), t37, t38, t39, t40, t41 }); }));
}

static wv w2_formatDiag(wv v42_d) {
  rt_enter("formatDiag");
  v42_d = rt_bind(v42_d, 0);
  wv v43_chars = rt_bind(rt_get(&F_chars, v42_d), 0);
  wv v44_line = rt_i(1);
  wv v45_lineStart = rt_i(0);
  wv v46_k = rt_i(0);
  while (rt_cond(({ wv t42 = v46_k; wv t43 = rt_get(&F_pos, v42_d); rt_lt(t42, t43); }))) {
    if (rt_cond(rt_eq(rt_idx(v46_k, v43_chars), rt_s(&S2)))) {
      v44_line = rt_add(v44_line, rt_i(1));
      v45_lineStart = rt_add(v46_k, rt_i(1));
    }
    v46_k = rt_add(v46_k, rt_i(1));
  }
  wv v47_lineEnd = rt_bind(v45_lineStart, 1);
  while (rt_cond(rt_b(rt_bool(({ wv t44 = v47_lineEnd; wv t45 = std_length(v43_chars); rt_lt(t44, t45); }), "&&") && rt_bool(rt_ne(rt_idx(v47_lineEnd, v43_chars), rt_s(&S2)), "&&")))) {
    v47_lineEnd = rt_add(v47_lineEnd, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_gt(v47_lineEnd, v45_lineStart), "&&") && rt_bool(rt_eq(({ wv t46 = rt_sub(v47_lineEnd, rt_i(1)); wv t47 = v43_chars; rt_idx(t46, t47); }), rt_s(&S3)), "&&")))) {
    v47_lineEnd = rt_sub(v47_lineEnd, rt_i(1));
  }
  wv v48_pad = rt_s(&S13);
  v46_k = rt_bind(v45_lineStart, 1);
  while (rt_cond(rt_b(rt_bool(({ wv t48 = v46_k; wv t49 = rt_get(&F_pos, v42_d); rt_lt(t48, t49); }), "&&") && rt_bool(rt_lt(v46_k, v47_lineEnd), "&&")))) {
    v48_pad = ({ wv t50 = v48_pad; wv t51 = (rt_cond(rt_eq(rt_idx(v46_k, v43_chars), rt_s(&S1))) ? rt_s(&S1) : rt_s(&S0)); rt_add(t50, t51); });
    v46_k = rt_add(v46_k, rt_i(1));
  }
  wv v49_text = rt_bind(std_join(std_slice(v43_chars, v45_lineStart, v47_lineEnd), rt_s(&S13)), 0);
  wv v50_out = ({ wv t54 = rt_text(rt_get(&F_file, v42_d)); wv t55 = rt_text(v44_line); wv t56 = rt_text(rt_add(({ wv t52 = rt_get(&F_pos, v42_d); wv t53 = v45_lineStart; rt_sub(t52, t53); }), rt_i(1))); wv t57 = rt_text(rt_get(&F_severity, v42_d)); wv t58 = rt_text(rt_get(&F_message, v42_d)); rt_cat(9, (wv[]){ t54, rt_s(&S28), t55, rt_s(&S28), t56, rt_s(&S29), t57, rt_s(&S29), t58 }); });
  if (rt_cond(rt_ne(rt_get(&F_hint, v42_d), RT_NULL))) {
    v50_out = ({ wv t59 = v50_out; wv t60 = rt_cat(2, (wv[]){ rt_s(&S30), rt_text(rt_get(&F_hint, v42_d)) }); rt_add(t59, t60); });
  }
  RT_RET(({ wv t61 = rt_text(v50_out); wv t62 = rt_text(v49_text); wv t63 = rt_text(v48_pad); rt_cat(6, (wv[]){ t61, rt_s(&S31), t62, rt_s(&S31), t63, rt_s(&S32) }); }));
}

static wv w2_flushWarnings(void) {
  rt_enter("flushWarnings");
  wv v51_k = rt_i(0);
  while (rt_cond(({ wv t64 = v51_k; wv t65 = std_length(rt_def(w2_warnings, "warnings")); rt_lt(t64, t65); }))) {
    (void)std_eprint(rt_chk(w2_formatDiag(({ wv t66 = v51_k; wv t67 = rt_def(w2_warnings, "warnings"); rt_idx(t66, t67); })), "flushWarnings"));
    v51_k = rt_add(v51_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w2_failHint(wv v52_pos, wv v53_message, wv v54_hint) {
  rt_enter("failHint");
  v52_pos = rt_bind(v52_pos, 0);
  v53_message = rt_bind(v53_message, 0);
  v54_hint = rt_bind(v54_hint, 0);
  (void)rt_chk(w2_flushWarnings(), "failHint");
  (void)std_eprint(rt_chk(w2_formatDiag(rt_chk(w2_err(v52_pos, v53_message, v54_hint), "failHint")), "failHint"));
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w2_abort(wv v55_pos, wv v56_message) {
  rt_enter("abort");
  v55_pos = rt_bind(v55_pos, 0);
  v56_message = rt_bind(v56_message, 0);
  (void)rt_chk(w2_failHint(v55_pos, v56_message, RT_NULL), "abort");
  RT_RET(RT_NULL);
}

static wv w3_digitRun(wv v57_cs, wv v58_k, wv v59_base) {
  rt_enter("digitRun");
  v57_cs = rt_bind(v57_cs, 0);
  v58_k = rt_bind(v58_k, 0);
  v59_base = rt_bind(v59_base, 0);
  wv v60_j = rt_bind(v58_k, 1);
  while (rt_cond(rt_b(rt_bool(({ wv t71 = v60_j; wv t72 = std_length(v57_cs); rt_lt(t71, t72); }), "&&") && rt_bool(({ wv t73 = rt_idx(v60_j, v57_cs); wv t74 = v59_base; rt_chk(w3_isDigitIn(t73, t74), "digitRun"); }), "&&")))) {
    v60_j = rt_add(v60_j, rt_i(1));
  }
  RT_RET(v60_j);
}

static wv w3_isDigitIn(wv v61_c, wv v62_base) {
  rt_enter("isDigitIn");
  v61_c = rt_bind(v61_c, 0);
  v62_base = rt_bind(v62_base, 0);
  if (rt_cond(rt_eq(v62_base, rt_i(16)))) {
    RT_RET(rt_chk(w1_isHexDigit(v61_c), "isDigitIn"));
  }
  if (rt_cond(rt_eq(v62_base, rt_i(2)))) {
    RT_RET(rt_b(rt_bool(rt_eq(v61_c, rt_s(&S4)), "||") || rt_bool(rt_eq(v61_c, rt_s(&S101)), "||")));
  }
  RT_RET(rt_chk(w1_isDigit(v61_c), "isDigitIn"));
}

static wv w3_digitsValue(wv v63_digits, wv v64_base) {
  rt_enter("digitsValue");
  v63_digits = rt_bind(v63_digits, 0);
  v64_base = rt_bind(v64_base, 0);
  wv v65_hex = rt_bind(std_split(rt_s(&S17), rt_s(&S13)), 0);
  wv v66_v = rt_i(0);
  wv v67_k = rt_i(0);
  while (rt_cond(({ wv t75 = v67_k; wv t76 = std_length(v63_digits); rt_lt(t75, t76); }))) {
    wv v68_c = rt_bind(rt_chk(w1_toLower(rt_idx(v67_k, v63_digits)), "digitsValue"), 0);
    wv v69_d = rt_i(0);
    while (rt_cond(({ wv t77 = rt_idx(v69_d, v65_hex); wv t78 = v68_c; rt_ne(t77, t78); }))) {
      v69_d = rt_add(v69_d, rt_i(1));
    }
    if (rt_cond(({ wv t83 = v66_v; wv t84 = ({ wv t81 = ({ wv t79 = rt_def(w3_maxInt, "maxInt"); wv t80 = v69_d; rt_sub(t79, t80); }); wv t82 = v64_base; rt_div(t81, t82); }); rt_gt(t83, t84); }))) {
      RT_RET(rt_i(-1));
    }
    v66_v = ({ wv t85 = rt_mul(v66_v, v64_base); wv t86 = v69_d; rt_add(t85, t86); });
    v67_k = rt_add(v67_k, rt_i(1));
  }
  RT_RET(v66_v);
}

static wv w3_simpleEscape(wv v70_e) {
  rt_enter("simpleEscape");
  v70_e = rt_bind(v70_e, 0);
  if (rt_cond(rt_eq(v70_e, rt_s(&S102)))) {
    RT_RET(rt_s(&S2));
  }
  if (rt_cond(rt_eq(v70_e, rt_s(&S103)))) {
    RT_RET(rt_s(&S3));
  }
  if (rt_cond(rt_eq(v70_e, rt_s(&S104)))) {
    RT_RET(rt_s(&S1));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v70_e, rt_s(&S19)), "||") || rt_bool(rt_eq(v70_e, rt_s(&S18)), "||")), "||") || rt_bool(rt_eq(v70_e, rt_s(&S105)), "||")))) {
    RT_RET(v70_e);
  }
  RT_RET(RT_NULL);
}

static wv w3_lex(wv p_v71_cs) {
  rt_enter("lex");
  struct fr27 fr = { 0 };
  fr.v71_cs = rt_bind(p_v71_cs, 0);
  fr.v72_n = rt_bind(std_length(fr.v71_cs), 0);
  fr.v73_i = rt_i(0);
  fr.v74_prevT = rt_s(&S13);
  fr.v75_nlNow = rt_b(0);
  wv v76_toks = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v168_t0 = rt_bind(rt_chk(s84_next(&fr), "lex"), 0);
    (void)std_push(v76_toks, v168_t0);
    if (rt_cond(rt_eq(rt_get(&F_t, v168_t0), rt_s(&S110)))) {
      break;
    }
  }
  RT_RET(v76_toks);
}

static wv s77_ch(struct fr27 *up, wv v90_k) {
  rt_enter("ch");
  v90_k = rt_bind(v90_k, 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ge(v90_k, rt_i(0)), "&&") && rt_bool(({ wv t87 = v90_k; wv t88 = rt_def(up->v72_n, "n"); rt_lt(t87, t88); }), "&&"))) ? rt_idx(v90_k, up->v71_cs) : rt_s(&S13)));
}

static wv s78_cut(struct fr27 *up, wv v91_a, wv v92_b) {
  rt_enter("cut");
  v91_a = rt_bind(v91_a, 0);
  v92_b = rt_bind(v92_b, 0);
  RT_RET(std_join(std_slice(up->v71_cs, v91_a, v92_b), rt_s(&S13)));
}

static wv s79_startsWith(struct fr27 *up, wv v93_k, wv v94_s) {
  rt_enter("startsWith");
  v93_k = rt_bind(v93_k, 0);
  v94_s = rt_bind(v94_s, 0);
  wv v95_sc = rt_bind(std_split(v94_s, rt_s(&S13)), 0);
  if (rt_cond(({ wv t91 = ({ wv t89 = v93_k; wv t90 = std_length(v95_sc); rt_add(t89, t90); }); wv t92 = rt_def(up->v72_n, "n"); rt_gt(t91, t92); }))) {
    RT_RET(rt_b(0));
  }
  wv v96_q = rt_i(0);
  while (rt_cond(({ wv t93 = v96_q; wv t94 = std_length(v95_sc); rt_lt(t93, t94); }))) {
    if (rt_cond(({ wv t97 = ({ wv t95 = rt_add(v93_k, v96_q); wv t96 = up->v71_cs; rt_idx(t95, t96); }); wv t98 = rt_idx(v96_q, v95_sc); rt_ne(t97, t98); }))) {
      RT_RET(rt_b(0));
    }
    v96_q = rt_add(v96_q, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s80_find(struct fr27 *up, wv v97_s, wv v98_from) {
  rt_enter("find");
  v97_s = rt_bind(v97_s, 0);
  v98_from = rt_bind(v98_from, 0);
  wv v99_q = rt_bind(v98_from, 1);
  while (rt_cond(({ wv t99 = v99_q; wv t100 = rt_def(up->v72_n, "n"); rt_lt(t99, t100); }))) {
    if (rt_cond(rt_chk(s79_startsWith(up, v99_q, v97_s), "find"))) {
      RT_RET(v99_q);
    }
    v99_q = rt_add(v99_q, rt_i(1));
  }
  RT_RET(rt_i(-1));
}

static wv s81_tok(struct fr27 *up, wv v100_t, wv v101_pos, wv v102_sp, wv v103_v, wv v104_spelling, wv v105_parts) {
  rt_enter("tok");
  v100_t = rt_bind(v100_t, 0);
  v101_pos = rt_bind(v101_pos, 0);
  v102_sp = rt_bind(v102_sp, 0);
  v103_v = rt_bind(v103_v, 0);
  v104_spelling = rt_bind(v104_spelling, 0);
  v105_parts = rt_bind(v105_parts, 0);
  wv t101 = rt_bind(v100_t, 1);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t101;
  RT_RET(({ wv t102 = v100_t; wv t103 = v101_pos; wv t104 = rt_def(up->v73_i, "i"); wv t105 = v102_sp; wv t106 = rt_def(up->v75_nlNow, "nlNow"); wv t107 = v103_v; wv t108 = v104_spelling; wv t109 = v105_parts; rt_rec(0, 8, K2, (wv[]){ t102, t103, t104, t105, t106, t107, t108, t109 }); }));
}

static wv s82_plain(struct fr27 *up, wv v106_t, wv v107_pos, wv v108_sp) {
  rt_enter("plain");
  v106_t = rt_bind(v106_t, 0);
  v107_pos = rt_bind(v107_pos, 0);
  v108_sp = rt_bind(v108_sp, 0);
  RT_RET(rt_chk(s81_tok(up, v106_t, v107_pos, v108_sp, RT_NULL, RT_NULL, RT_NULL), "plain"));
}

static wv s83_skip(struct fr27 *up) {
  rt_enter("skip");
  wv v109_skipped = rt_b(0);
  while (rt_cond(rt_b(1))) {
    wv v110_c = rt_bind(rt_chk(s77_ch(up, rt_def(up->v73_i, "i")), "skip"), 0);
    if (rt_cond(rt_chk(w1_isSpace(v110_c), "skip"))) {
      up->v73_i = rt_add(rt_def(up->v73_i, "i"), rt_i(1));
      v109_skipped = rt_b(1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v110_c, rt_s(&S87)), "&&") && rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(rt_def(up->v73_i, "i"), rt_i(1))), "skip"), rt_s(&S87)), "&&")))) {
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v109_skipped), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, rt_sub(rt_def(up->v73_i, "i"), rt_i(1))), "skip")), "skip"), "&&")), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, rt_add(rt_def(up->v73_i, "i"), rt_i(2))), "skip")), "skip"), "&&")))) {
        (void)({ wv t110 = w2_warnings; wv t111 = rt_chk(w2_warn(rt_def(up->v73_i, "i"), rt_s(&S106), rt_s(&S107)), "skip"); std_push(t110, t111); });
      }
      while (rt_cond(rt_b(rt_bool(({ wv t112 = rt_def(up->v73_i, "i"); wv t113 = rt_def(up->v72_n, "n"); rt_lt(t112, t113); }), "&&") && rt_bool(rt_ne(({ wv t114 = rt_def(up->v73_i, "i"); wv t115 = up->v71_cs; rt_idx(t114, t115); }), rt_s(&S2)), "&&")))) {
        up->v73_i = rt_add(rt_def(up->v73_i, "i"), rt_i(1));
      }
      v109_skipped = rt_b(1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v110_c, rt_s(&S87)), "&&") && rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(rt_def(up->v73_i, "i"), rt_i(1))), "skip"), rt_s(&S86)), "&&")))) {
      wv v111_close = rt_bind(rt_chk(s80_find(up, rt_s(&S108), rt_add(rt_def(up->v73_i, "i"), rt_i(2))), "skip"), 0);
      if (rt_cond(rt_lt(v111_close, rt_i(0)))) {
        (void)rt_chk(w2_abort(rt_def(up->v73_i, "i"), rt_s(&S109)), "skip");
      }
      wv t116 = rt_add(v111_close, rt_i(2));
      (void)rt_def(up->v73_i, "i");
      up->v73_i = t116;
      v109_skipped = rt_b(1);
    } else {
      RT_RET(v109_skipped);
    }
  }
  RT_RET(RT_NULL);
}

static wv s84_next(struct fr27 *up) {
  rt_enter("next");
  wv v112_from = rt_bind(rt_def(up->v73_i, "i"), 0);
  wv v113_sp = rt_b(rt_bool(rt_chk(s83_skip(up), "next"), "||") || rt_bool(rt_eq(rt_def(up->v73_i, "i"), rt_i(0)), "||"));
  wv t117 = rt_b(0);
  (void)rt_def(up->v75_nlNow, "nlNow");
  up->v75_nlNow = t117;
  wv v114_q = rt_bind(v112_from, 1);
  while (rt_cond(({ wv t118 = v114_q; wv t119 = rt_def(up->v73_i, "i"); rt_lt(t118, t119); }))) {
    if (rt_cond(rt_eq(rt_idx(v114_q, up->v71_cs), rt_s(&S2)))) {
      wv t120 = rt_b(1);
      (void)rt_def(up->v75_nlNow, "nlNow");
      up->v75_nlNow = t120;
    }
    v114_q = rt_add(v114_q, rt_i(1));
  }
  wv v115_start = rt_bind(rt_def(up->v73_i, "i"), 0);
  wv v116_c = rt_bind(rt_chk(s77_ch(up, v115_start), "next"), 0);
  if (rt_cond(rt_eq(v116_c, rt_s(&S13)))) {
    RT_RET(rt_chk(s82_plain(up, rt_s(&S110), v115_start, v113_sp), "next"));
  }
  if (rt_cond(rt_chk(w1_isIdStart(v116_c), "next"))) {
    RT_RET(rt_chk(s85_word(up, v115_start, v113_sp), "next"));
  }
  if (rt_cond(rt_eq(v116_c, rt_s(&S18)))) {
    RT_RET(rt_chk(s88_string(up, v115_start, v113_sp), "next"));
  }
  if (rt_cond(rt_eq(v116_c, rt_s(&S105)))) {
    RT_RET(rt_chk(s87_sigil(up, v115_start, v113_sp), "next"));
  }
  if (rt_cond(rt_chk(w1_isDigit(v116_c), "next"))) {
    RT_RET(rt_chk(s86_number(up, v115_start, v113_sp), "next"));
  }
  wv v117_k = rt_i(0);
  while (rt_cond(({ wv t121 = v117_k; wv t122 = std_length(rt_def(w3_setOps, "setOps")); rt_lt(t121, t122); }))) {
    wv v118_op = rt_bind(({ wv t123 = v117_k; wv t124 = rt_def(w3_setOps, "setOps"); rt_idx(t123, t124); }), 0);
    if (rt_cond(rt_chk(s79_startsWith(up, v115_start, v118_op), "next"))) {
      wv t127 = ({ wv t125 = v115_start; wv t126 = std_length(v118_op); rt_add(t125, t126); });
      (void)rt_def(up->v73_i, "i");
      up->v73_i = t127;
      RT_RET(({ wv t130 = v115_start; wv t131 = v113_sp; wv t132 = ({ wv t128 = v118_op; wv t129 = rt_sub(std_length(v118_op), rt_i(1)); std_slice(t128, rt_i(0), t129); }); rt_chk(s81_tok(up, rt_s(&S111), t130, t131, t132, RT_NULL, RT_NULL), "next"); }));
    }
    v117_k = rt_add(v117_k, rt_i(1));
  }
  if (rt_cond(rt_eq(v116_c, rt_s(&S94)))) {
    wv v119_limit = rt_add(v115_start, rt_i(64));
    wv v120_j = rt_add(v115_start, rt_i(1));
    while (rt_cond(rt_b(rt_bool(rt_lt(v120_j, v119_limit), "&&") && rt_bool(rt_chk(w1_isJsSpace(rt_chk(s77_ch(up, v120_j), "next")), "next"), "&&")))) {
      v120_j = rt_add(v120_j, rt_i(1));
    }
    wv v121_wordStart = rt_bind(v120_j, 0);
    if (rt_cond(rt_b(rt_bool(rt_lt(v120_j, v119_limit), "&&") && rt_bool(rt_chk(w1_isIdStart(rt_chk(s77_ch(up, v120_j), "next")), "next"), "&&")))) {
      while (rt_cond(rt_b(rt_bool(rt_lt(v120_j, v119_limit), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v120_j), "next")), "next"), "&&")))) {
        v120_j = rt_add(v120_j, rt_i(1));
      }
      wv v122_w = rt_bind(rt_chk(s78_cut(up, v121_wordStart, v120_j), "next"), 0);
      if (rt_cond(({ wv t133 = rt_def(w3_bangWords, "bangWords"); wv t134 = v122_w; rt_chk(w1_contains(t133, t134), "next"); }))) {
        if (rt_cond(({ wv t135 = v121_wordStart; wv t136 = rt_add(v115_start, rt_i(1)); rt_gt(t135, t136); }))) {
          (void)({ wv t137 = v115_start; wv t138 = rt_cat(3, (wv[]){ rt_s(&S112), rt_text(v122_w), rt_s(&S113) }); wv t139 = rt_cat(3, (wv[]){ rt_s(&S114), rt_text(v122_w), rt_s(&S113) }); rt_chk(w2_failHint(t137, t138, t139), "next"); });
        }
        wv t140 = rt_bind(v120_j, 1);
        (void)rt_def(up->v73_i, "i");
        up->v73_i = t140;
        RT_RET(({ wv t141 = rt_add(rt_s(&S94), v122_w); wv t142 = v115_start; wv t143 = v113_sp; rt_chk(s82_plain(up, t141, t142, t143), "next"); }));
      }
    }
  }
  if (rt_cond(rt_chk(s79_startsWith(up, v115_start, rt_s(&S115)), "next"))) {
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S116), rt_s(&S117)), "next");
  }
  if (rt_cond(rt_chk(s79_startsWith(up, v115_start, rt_s(&S118)), "next"))) {
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S119), rt_s(&S120)), "next");
  }
  if (rt_cond(rt_chk(s79_startsWith(up, v115_start, rt_s(&S121)), "next"))) {
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S122), rt_s(&S123)), "next");
  }
  v117_k = rt_i(0);
  while (rt_cond(({ wv t144 = v117_k; wv t145 = std_length(rt_def(w3_ops, "ops")); rt_lt(t144, t145); }))) {
    wv v123_op = rt_bind(({ wv t146 = v117_k; wv t147 = rt_def(w3_ops, "ops"); rt_idx(t146, t147); }), 0);
    if (rt_cond(rt_chk(s79_startsWith(up, v115_start, v123_op), "next"))) {
      wv t150 = ({ wv t148 = v115_start; wv t149 = std_length(v123_op); rt_add(t148, t149); });
      (void)rt_def(up->v73_i, "i");
      up->v73_i = t150;
      if (rt_cond(rt_b(rt_bool(rt_eq(v123_op, rt_s(&S14)), "&&") && rt_bool(rt_eq(rt_def(up->v74_prevT, "prevT"), rt_s(&S14)), "&&")))) {
        (void)({ wv t151 = w2_warnings; wv t152 = rt_chk(w2_warn(v115_start, rt_s(&S124), rt_s(&S125)), "next"); std_push(t151, t152); });
      }
      RT_RET(rt_chk(s82_plain(up, v123_op, v115_start, v113_sp), "next"));
    }
    v117_k = rt_add(v117_k, rt_i(1));
  }
  if (rt_cond(rt_eq(v116_c, rt_s(&S126)))) {
    (void)rt_chk(w2_failHint(v115_start, rt_s(&S127), rt_s(&S128)), "next");
  }
  if (rt_cond(rt_eq(v116_c, rt_s(&S129)))) {
    wv v124_after = rt_bind(rt_chk(s77_ch(up, rt_add(v115_start, rt_i(1))), "next"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(v113_sp, "||") || rt_bool(rt_eq(v124_after, rt_s(&S13)), "||")), "||") || rt_bool(rt_chk(w1_isSpace(v124_after), "next"), "||")), "||") || rt_bool(rt_chk(s79_startsWith(up, rt_add(v115_start, rt_i(1)), rt_s(&S130)), "next"), "||")), "||") || rt_bool(rt_chk(s79_startsWith(up, rt_add(v115_start, rt_i(1)), rt_s(&S131)), "next"), "||")))) {
      (void)rt_chk(w2_failHint(v115_start, rt_s(&S132), rt_s(&S133)), "next");
    }
    wv t153 = rt_add(v115_start, rt_i(1));
    (void)rt_def(up->v73_i, "i");
    up->v73_i = t153;
    RT_RET(rt_chk(s82_plain(up, rt_s(&S129), v115_start, v113_sp), "next"));
  }
  (void)({ wv t154 = v115_start; wv t155 = rt_cat(3, (wv[]){ rt_s(&S134), rt_text(v116_c), rt_s(&S113) }); rt_chk(w2_abort(t154, t155), "next"); });
  RT_RET(RT_NULL);
}

static wv s85_word(struct fr27 *up, wv v125_start, wv v126_sp) {
  rt_enter("word");
  v125_start = rt_bind(v125_start, 0);
  v126_sp = rt_bind(v126_sp, 0);
  wv v127_j = rt_bind(v125_start, 1);
  while (rt_cond(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v127_j), "word")), "word"))) {
    v127_j = rt_add(v127_j, rt_i(1));
  }
  wv v128_w = rt_bind(rt_chk(s78_cut(up, v125_start, v127_j), "word"), 0);
  wv t156 = rt_bind(v127_j, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t156;
  if (rt_cond(rt_eq(rt_chk(s77_ch(up, v127_j), "word"), rt_s(&S105)))) {
    (void)rt_chk(w2_failHint(v127_j, rt_s(&S135), rt_s(&S136)), "word");
  }
  if (rt_cond(({ wv t157 = rt_def(w3_keywords, "keywords"); wv t158 = v128_w; rt_chk(w1_contains(t157, t158), "word"); }))) {
    RT_RET(rt_chk(s82_plain(up, v128_w, v125_start, v126_sp), "word"));
  }
  wv v129_k = rt_i(0);
  while (rt_cond(({ wv t159 = v129_k; wv t160 = std_length(rt_def(w3_reserved, "reserved")); rt_lt(t159, t160); }))) {
    if (rt_cond(({ wv t163 = rt_get(&F_word, ({ wv t161 = v129_k; wv t162 = rt_def(w3_reserved, "reserved"); rt_idx(t161, t162); })); wv t164 = v128_w; rt_eq(t163, t164); }))) {
      (void)({ wv t167 = v125_start; wv t168 = rt_get(&F_message, ({ wv t165 = v129_k; wv t166 = rt_def(w3_reserved, "reserved"); rt_idx(t165, t166); })); rt_chk(w2_abort(t167, t168), "word"); });
    }
    v129_k = rt_add(v129_k, rt_i(1));
  }
  RT_RET(rt_chk(s81_tok(up, rt_s(&S137), v125_start, v126_sp, v128_w, RT_NULL, RT_NULL), "word"));
}

static wv s86_number(struct fr27 *up, wv v130_start, wv v131_sp) {
  rt_enter("number");
  v130_start = rt_bind(v130_start, 0);
  v131_sp = rt_bind(v131_sp, 0);
  wv v132_kind = rt_s(&S138);
  wv v133_j = rt_bind(v130_start, 1);
  wv v134_base = rt_i(10);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v130_start), "number"), rt_s(&S4)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(v130_start, rt_i(1))), "number"), rt_s(&S139)), "||") || rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(v130_start, rt_i(1))), "number"), rt_s(&S140)), "||")), "&&")))) {
    v134_base = rt_bind((rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v130_start, rt_i(1))), "number"), rt_s(&S139))) ? rt_i(16) : rt_i(2)), 1);
    v133_j = rt_bind(({ wv t169 = up->v71_cs; wv t170 = rt_add(v130_start, rt_i(2)); wv t171 = v134_base; rt_chk(w3_digitRun(t169, t170, t171), "number"); }), 1);
    if (rt_cond(({ wv t172 = v133_j; wv t173 = rt_add(v130_start, rt_i(2)); rt_eq(t172, t173); }))) {
      (void)({ wv t178 = v130_start; wv t179 = ({ wv t176 = rt_text(({ wv t174 = v130_start; wv t175 = rt_add(v130_start, rt_i(2)); rt_chk(s78_cut(up, t174, t175), "number"); })); wv t177 = rt_text((rt_cond(rt_eq(v134_base, rt_i(2))) ? rt_s(&S142) : rt_s(&S143))); rt_cat(5, (wv[]){ rt_s(&S113), t176, rt_s(&S141), t177, rt_s(&S144) }); }); rt_chk(w2_abort(t178, t179), "number"); });
    }
  } else {
    v133_j = rt_bind(rt_chk(w3_digitRun(up->v71_cs, v130_start, rt_i(10)), "number"), 1);
    if (rt_cond(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S129)))) {
      if (rt_cond(rt_not(rt_chk(w1_isDigit(rt_chk(s77_ch(up, rt_add(v133_j, rt_i(1))), "number")), "number")))) {
        (void)({ wv t180 = v130_start; wv t181 = rt_cat(3, (wv[]){ rt_s(&S146), rt_text(rt_chk(s78_cut(up, v130_start, v133_j), "number")), rt_s(&S147) }); rt_chk(w2_failHint(t180, rt_s(&S145), t181), "number"); });
      }
      v132_kind = rt_s(&S148);
      v133_j = rt_bind(({ wv t182 = up->v71_cs; wv t183 = rt_add(v133_j, rt_i(1)); rt_chk(w3_digitRun(t182, t183, rt_i(10)), "number"); }), 1);
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S149)), "||") || rt_bool(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S150)), "||")))) {
      wv v137_e = rt_add(v133_j, rt_i(1));
      if (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v137_e), "number"), rt_s(&S84)), "||") || rt_bool(rt_eq(rt_chk(s77_ch(up, v137_e), "number"), rt_s(&S85)), "||")))) {
        v137_e = rt_add(v137_e, rt_i(1));
      }
      wv v138_after = rt_bind(rt_chk(w3_digitRun(up->v71_cs, v137_e, rt_i(10)), "number"), 0);
      if (rt_cond(rt_eq(v138_after, v137_e))) {
        (void)rt_chk(w2_abort(v130_start, rt_s(&S151)), "number");
      }
      v132_kind = rt_s(&S148);
      v133_j = rt_bind(v138_after, 1);
    }
  }
  if (rt_cond(rt_b(rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v133_j), "number")), "number"), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v133_j), "number"), rt_s(&S129)), "&&") && rt_bool(rt_chk(w1_isDigit(rt_chk(s77_ch(up, rt_add(v133_j, rt_i(1))), "number")), "number"), "&&")), "||")))) {
    wv v139_bad = rt_bind(v130_start, 1);
    while (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s77_ch(up, v139_bad), "number"), rt_s(&S129)), "||") || rt_bool(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v139_bad), "number")), "number"), "||")))) {
      v139_bad = rt_add(v139_bad, rt_i(1));
    }
    (void)({ wv t184 = v130_start; wv t185 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_chk(s78_cut(up, v130_start, v139_bad), "number")), rt_s(&S152) }); rt_chk(w2_abort(t184, t185), "number"); });
  }
  wv t186 = rt_bind(v133_j, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t186;
  wv v135_spelling = rt_bind(rt_chk(s78_cut(up, v130_start, v133_j), "number"), 0);
  if (rt_cond(rt_eq(v132_kind, rt_s(&S148)))) {
    wv t187 = std_toFloat(v135_spelling);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        (void)rt_chk(w2_abort(v130_start, rt_s(&S153)), "number");
        t187 = rt_i(0);
      }
    }
    wv v140_f = rt_bind(t187, 0);
    RT_RET(rt_chk(s81_tok(up, rt_s(&S148), v130_start, v131_sp, v140_f, v135_spelling, RT_NULL), "number"));
  }
  if (rt_cond(rt_eq(v134_base, rt_i(10)))) {
    wv t188 = std_toInt(v135_spelling);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        (void)rt_chk(w2_abort(v130_start, rt_s(&S154)), "number");
        t188 = rt_i(0);
      }
    }
    wv v141_v = rt_bind(t188, 0);
    RT_RET(rt_chk(s81_tok(up, rt_s(&S138), v130_start, v131_sp, v141_v, v135_spelling, RT_NULL), "number"));
  }
  wv v136_v = rt_bind(({ wv t191 = ({ wv t189 = std_split(v135_spelling, rt_s(&S13)); wv t190 = std_length(v135_spelling); std_slice(t189, rt_i(2), t190); }); wv t192 = v134_base; rt_chk(w3_digitsValue(t191, t192), "number"); }), 0);
  if (rt_cond(rt_lt(v136_v, rt_i(0)))) {
    (void)rt_chk(w2_abort(v130_start, rt_s(&S154)), "number");
  }
  RT_RET(rt_chk(s81_tok(up, rt_s(&S138), v130_start, v131_sp, v136_v, v135_spelling, RT_NULL), "number"));
}

static wv s87_sigil(struct fr27 *up, wv v142_start, wv v143_sp) {
  rt_enter("sigil");
  v142_start = rt_bind(v142_start, 0);
  v143_sp = rt_bind(v143_sp, 0);
  if (rt_cond(rt_not(rt_chk(w1_isIdStart(rt_chk(s77_ch(up, rt_add(v142_start, rt_i(1))), "sigil")), "sigil")))) {
    (void)rt_chk(w2_failHint(v142_start, rt_s(&S155), rt_s(&S136)), "sigil");
  }
  wv v144_j = rt_add(v142_start, rt_i(1));
  while (rt_cond(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v144_j), "sigil")), "sigil"))) {
    v144_j = rt_add(v144_j, rt_i(1));
  }
  wv t193 = rt_bind(v144_j, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t193;
  RT_RET(({ wv t196 = v142_start; wv t197 = v143_sp; wv t198 = ({ wv t194 = rt_add(v142_start, rt_i(1)); wv t195 = v144_j; rt_chk(s78_cut(up, t194, t195), "sigil"); }); rt_chk(s81_tok(up, rt_s(&S156), t196, t197, t198, RT_NULL, RT_NULL), "sigil"); }));
}

static wv s88_string(struct fr27 *up, wv v145_start, wv v146_sp) {
  rt_enter("string");
  v145_start = rt_bind(v145_start, 0);
  v146_sp = rt_bind(v146_sp, 0);
  wv v147_parts = rt_arr(0, NULL, 1);
  wv v148_buf = rt_s(&S13);
  wv v149_rawDollar = rt_i(-1);
  wv v150_k = rt_add(v145_start, rt_i(1));
  while (rt_cond(rt_b(1))) {
    wv v151_c = rt_bind(rt_chk(s77_ch(up, v150_k), "string"), 0);
    if (rt_cond(rt_eq(v151_c, rt_s(&S13)))) {
      (void)rt_chk(w2_abort(v145_start, rt_s(&S157)), "string");
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(v151_c, rt_s(&S2)), "||") || rt_bool(rt_eq(v151_c, rt_s(&S3)), "||")))) {
      (void)rt_chk(w2_failHint(v150_k, rt_s(&S158), rt_s(&S159)), "string");
    }
    if (rt_cond(rt_eq(v151_c, rt_s(&S18)))) {
      v150_k = rt_add(v150_k, rt_i(1));
      break;
    }
    if (rt_cond(rt_eq(v151_c, rt_s(&S19)))) {
      wv v152_e = rt_bind(rt_chk(s77_ch(up, rt_add(v150_k, rt_i(1))), "string"), 0);
      wv v153_simple = rt_bind(rt_chk(w3_simpleEscape(v152_e), "string"), 0);
      if (rt_cond(rt_ne(v153_simple, RT_NULL))) {
        v148_buf = rt_add(v148_buf, v153_simple);
        v150_k = rt_add(v150_k, rt_i(2));
        continue;
      }
      if (rt_cond(rt_b(rt_bool(rt_eq(v152_e, rt_s(&S160)), "&&") && rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(v150_k, rt_i(2))), "string"), rt_s(&S100)), "&&")))) {
        wv v154_close = rt_bind(rt_chk(s80_find(up, rt_s(&S15), rt_add(v150_k, rt_i(3))), "string"), 0);
        wv v155_hex = rt_bind((rt_cond(rt_lt(v154_close, rt_i(0))) ? rt_s(&S13) : ({ wv t199 = rt_add(v150_k, rt_i(3)); wv t200 = v154_close; rt_chk(s78_cut(up, t199, t200), "string"); })), 0);
        wv v156_cp = rt_bind(rt_chk(w1_hexValue(v155_hex), "string"), 0);
        if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_lt(v156_cp, rt_i(0)), "||") || rt_bool(rt_gt(v156_cp, rt_i(1114111)), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v156_cp, rt_i(55296)), "&&") && rt_bool(rt_le(v156_cp, rt_i(57343)), "&&")), "||")))) {
          (void)rt_chk(w2_failHint(v150_k, rt_s(&S161), rt_s(&S162)), "string");
        }
        v148_buf = ({ wv t201 = v148_buf; wv t202 = std_fromCode(v156_cp); rt_add(t201, t202); });
        v150_k = rt_add(v154_close, rt_i(1));
        continue;
      }
      (void)({ wv t203 = v150_k; wv t204 = rt_cat(3, (wv[]){ rt_s(&S163), rt_text(v152_e), rt_s(&S113) }); rt_chk(w2_failHint(t203, t204, rt_s(&S164)), "string"); });
    }
    if (rt_cond(rt_eq(v151_c, rt_s(&S100)))) {
      if (rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v150_k, rt_i(1))), "string"), rt_s(&S100)))) {
        v148_buf = rt_add(v148_buf, rt_s(&S100));
        v150_k = rt_add(v150_k, rt_i(2));
        continue;
      }
      if (rt_cond(({ wv t205 = v149_rawDollar; wv t206 = rt_sub(v150_k, rt_i(1)); rt_eq(t205, t206); }))) {
        (void)({ wv t207 = w2_warnings; wv t208 = rt_chk(w2_warn(v149_rawDollar, rt_s(&S165), RT_NULL), "string"); std_push(t207, t208); });
      }
      (void)std_push(v147_parts, v148_buf);
      v148_buf = rt_s(&S13);
      wv v157_sub = rt_bind(rt_chk(s89_interpolation(up, v150_k), "string"), 0);
      (void)({ wv t211 = v147_parts; wv t212 = ({ wv t209 = rt_get(&F_tokens, v157_sub); wv t210 = v150_k; rt_rec(0, 2, K3, (wv[]){ t209, t210 }); }); std_push(t211, t212); });
      v150_k = rt_bind(rt_get(&F_end, v157_sub), 1);
      continue;
    }
    if (rt_cond(rt_eq(v151_c, rt_s(&S15)))) {
      if (rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v150_k, rt_i(1))), "string"), rt_s(&S15)))) {
        v148_buf = rt_add(v148_buf, rt_s(&S15));
        v150_k = rt_add(v150_k, rt_i(2));
        continue;
      }
      (void)rt_chk(w2_failHint(v150_k, rt_s(&S166), rt_s(&S167)), "string");
    }
    if (rt_cond(rt_eq(v151_c, rt_s(&S105)))) {
      v149_rawDollar = rt_bind(v150_k, 1);
      if (rt_cond(rt_chk(w1_isIdStart(rt_chk(s77_ch(up, rt_add(v150_k, rt_i(1))), "string")), "string"))) {
        wv v158_j = rt_add(v150_k, rt_i(1));
        while (rt_cond(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v158_j), "string")), "string"))) {
          v158_j = rt_add(v158_j, rt_i(1));
        }
        wv v159_name = rt_bind(({ wv t213 = rt_add(v150_k, rt_i(1)); wv t214 = v158_j; rt_chk(s78_cut(up, t213, t214), "string"); }), 0);
        (void)({ wv t217 = w2_warnings; wv t218 = ({ wv t215 = v150_k; wv t216 = rt_cat(3, (wv[]){ rt_s(&S168), rt_text(v159_name), rt_s(&S169) }); rt_chk(w2_warn(t215, t216, rt_s(&S170)), "string"); }); std_push(t217, t218); });
      }
    }
    v148_buf = rt_add(v148_buf, v151_c);
    v150_k = rt_add(v150_k, rt_i(1));
  }
  (void)std_push(v147_parts, v148_buf);
  wv t219 = rt_bind(v150_k, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t219;
  RT_RET(rt_chk(s81_tok(up, rt_s(&S171), v145_start, v146_sp, RT_NULL, RT_NULL, v147_parts), "string"));
}

static wv s89_interpolation(struct fr27 *up, wv v160_open) {
  rt_enter("interpolation");
  v160_open = rt_bind(v160_open, 0);
  wv v161_saveI = rt_bind(rt_def(up->v73_i, "i"), 0);
  wv v162_savePrev = rt_bind(rt_def(up->v74_prevT, "prevT"), 0);
  wv v163_saveNl = rt_bind(rt_def(up->v75_nlNow, "nlNow"), 0);
  wv t220 = rt_add(v160_open, rt_i(1));
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t220;
  wv t221 = rt_s(&S13);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t221;
  wv v164_sub = rt_arr(0, NULL, 1);
  wv v165_depth = rt_i(0);
  while (rt_cond(rt_b(1))) {
    wv v167_tk = rt_bind(rt_chk(s84_next(up), "interpolation"), 0);
    if (rt_cond(rt_eq(rt_get(&F_t, v167_tk), rt_s(&S110)))) {
      (void)rt_chk(w2_abort(v160_open, rt_s(&S172)), "interpolation");
    }
    if (rt_cond(rt_eq(rt_get(&F_t, v167_tk), rt_s(&S15)))) {
      if (rt_cond(rt_eq(v165_depth, rt_i(0)))) {
        (void)({ wv t226 = v164_sub; wv t227 = ({ wv t222 = rt_get(&F_pos, v167_tk); wv t223 = rt_get(&F_end, v167_tk); wv t224 = rt_get(&F_sp, v167_tk); wv t225 = rt_get(&F_nl, v167_tk); rt_rec(0, 8, K2, (wv[]){ rt_s(&S110), t222, t223, t224, t225, RT_NULL, RT_NULL, RT_NULL }); }); std_push(t226, t227); });
        break;
      }
      v165_depth = rt_sub(v165_depth, rt_i(1));
    } else if (rt_cond(rt_eq(rt_get(&F_t, v167_tk), rt_s(&S100)))) {
      v165_depth = rt_add(v165_depth, rt_i(1));
    }
    (void)std_push(v164_sub, v167_tk);
  }
  wv v166_endI = rt_bind(rt_def(up->v73_i, "i"), 0);
  wv t228 = rt_bind(v161_saveI, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t228;
  wv t229 = rt_bind(v162_savePrev, 1);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t229;
  wv t230 = rt_bind(v163_saveNl, 1);
  (void)rt_def(up->v75_nlNow, "nlNow");
  up->v75_nlNow = t230;
  if (rt_cond(rt_eq(std_length(v164_sub), rt_i(1)))) {
    (void)rt_chk(w2_failHint(v160_open, rt_s(&S173), rt_s(&S174)), "interpolation");
  }
  RT_RET(rt_rec(0, 2, K4, (wv[]){ v164_sub, v166_endI }));
}

static wv w4_precedence(wv v169_t) {
  rt_enter("precedence");
  v169_t = rt_bind(v169_t, 0);
  if (rt_cond(rt_eq(v169_t, rt_s(&S81)))) {
    RT_RET(rt_i(1));
  }
  if (rt_cond(rt_eq(v169_t, rt_s(&S80)))) {
    RT_RET(rt_i(2));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v169_t, rt_s(&S76)), "||") || rt_bool(rt_eq(v169_t, rt_s(&S77)), "||")))) {
    RT_RET(rt_i(3));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v169_t, rt_s(&S89)), "||") || rt_bool(rt_eq(v169_t, rt_s(&S90)), "||")), "||") || rt_bool(rt_eq(v169_t, rt_s(&S78)), "||")), "||") || rt_bool(rt_eq(v169_t, rt_s(&S79)), "||")))) {
    RT_RET(rt_i(4));
  }
  if (rt_cond(rt_eq(v169_t, rt_s(&S92)))) {
    RT_RET(rt_i(5));
  }
  if (rt_cond(rt_eq(v169_t, rt_s(&S32)))) {
    RT_RET(rt_i(6));
  }
  if (rt_cond(rt_eq(v169_t, rt_s(&S91)))) {
    RT_RET(rt_i(7));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v169_t, rt_s(&S82)), "||") || rt_bool(rt_eq(v169_t, rt_s(&S83)), "||")))) {
    RT_RET(rt_i(8));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v169_t, rt_s(&S84)), "||") || rt_bool(rt_eq(v169_t, rt_s(&S85)), "||")))) {
    RT_RET(rt_i(9));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v169_t, rt_s(&S86)), "||") || rt_bool(rt_eq(v169_t, rt_s(&S87)), "||")), "||") || rt_bool(rt_eq(v169_t, rt_s(&S88)), "||")))) {
    RT_RET(rt_i(10));
  }
  RT_RET(rt_i(0));
}

static wv w4_describe(wv v170_tk) {
  rt_enter("describe");
  v170_tk = rt_bind(v170_tk, 0);
  if (rt_cond(rt_eq(rt_get(&F_t, v170_tk), rt_s(&S110)))) {
    RT_RET(rt_s(&S178));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v170_tk), rt_s(&S137)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_v, v170_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v170_tk), rt_s(&S156)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S179), rt_text(rt_get(&F_v, v170_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v170_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v170_tk), rt_s(&S148)), "||")))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_text, v170_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v170_tk), rt_s(&S171)))) {
    RT_RET(rt_s(&S180));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v170_tk), rt_s(&S111)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_v, v170_tk)), rt_s(&S181) }));
  }
  RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v170_tk)), rt_s(&S113) }));
}

static wv w4_failAtHint(wv v171_tk, wv v172_message, wv v173_hint) {
  rt_enter("failAtHint");
  v171_tk = rt_bind(v171_tk, 0);
  v172_message = rt_bind(v172_message, 0);
  v173_hint = rt_bind(v173_hint, 0);
  (void)({ wv t231 = rt_get(&F_pos, v171_tk); wv t232 = v172_message; wv t233 = v173_hint; rt_chk(w2_failHint(t231, t232, t233), "failAtHint"); });
  RT_RET(RT_NULL);
}

static wv w4_failAt(wv v174_tk, wv v175_message) {
  rt_enter("failAt");
  v174_tk = rt_bind(v174_tk, 0);
  v175_message = rt_bind(v175_message, 0);
  (void)({ wv t234 = rt_get(&F_pos, v174_tk); wv t235 = v175_message; rt_chk(w2_failHint(t234, t235, RT_NULL), "failAt"); });
  RT_RET(RT_NULL);
}

static wv w4_simpleStatement(wv v176_k, wv v177_e) {
  rt_enter("simpleStatement");
  v176_k = rt_bind(v176_k, 0);
  v177_e = rt_bind(v177_e, 0);
  RT_RET(rt_rec(0, 5, K5, (wv[]){ v176_k, v177_e, RT_NULL, RT_NULL, RT_NULL }));
}

static wv w4_parse(wv v178_toks) {
  rt_enter("parse");
  RT_RET(rt_chk(w4_parser(v178_toks, rt_b(0)), "parse"));
}

static wv w4_hasEffect(wv v179_e) {
  rt_enter("hasEffect");
  v179_e = rt_bind(v179_e, 0);
  wv v180_kind = rt_bind(rt_get(&F_k, v179_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v180_kind, rt_s(&S182)), "||") || rt_bool(rt_eq(v180_kind, rt_s(&S183)), "||")), "||") || rt_bool(rt_eq(v180_kind, rt_s(&S184)), "||")), "||") || rt_bool(rt_eq(v180_kind, rt_s(&S44)), "||")))) {
    RT_RET(rt_b(1));
  }
  if (rt_cond(rt_eq(v180_kind, rt_s(&S171)))) {
    wv v181_i = rt_i(0);
    while (rt_cond(({ wv t236 = v181_i; wv t237 = std_length(rt_get(&F_parts, v179_e)); rt_lt(t236, t237); }))) {
      if (rt_cond(rt_b(rt_bool(rt_ne(std_typeOf(({ wv t238 = v181_i; wv t239 = rt_get(&F_parts, v179_e); rt_idx(t238, t239); })), rt_s(&S185)), "&&") && rt_bool(rt_chk(w4_hasEffect(({ wv t240 = v181_i; wv t241 = rt_get(&F_parts, v179_e); rt_idx(t240, t241); })), "hasEffect"), "&&")))) {
        RT_RET(rt_b(1));
      }
      v181_i = rt_add(v181_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v180_kind, rt_s(&S186)), "||") || rt_bool(rt_eq(v180_kind, rt_s(&S187)), "||")))) {
    wv v182_i = rt_i(0);
    while (rt_cond(({ wv t242 = v182_i; wv t243 = std_length(rt_get(&F_items, v179_e)); rt_lt(t242, t243); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(({ wv t244 = v182_i; wv t245 = rt_get(&F_items, v179_e); rt_idx(t244, t245); })), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v182_i = rt_add(v182_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v180_kind, rt_s(&S188)))) {
    wv v183_i = rt_i(0);
    while (rt_cond(({ wv t246 = v183_i; wv t247 = std_length(rt_get(&F_fields, v179_e)); rt_lt(t246, t247); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(rt_get(&F_e, ({ wv t248 = v183_i; wv t249 = rt_get(&F_fields, v179_e); rt_idx(t248, t249); }))), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v183_i = rt_add(v183_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v180_kind, rt_s(&S189)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_index, v179_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_arr, v179_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v180_kind, rt_s(&S190)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_rec, v179_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v180_kind, rt_s(&S191)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_e, v179_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v180_kind, rt_s(&S192)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_l, v179_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_r, v179_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v180_kind, rt_s(&S193)))) {
    RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_c, v179_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_a, v179_e)), "hasEffect"), "||")), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_b, v179_e)), "hasEffect"), "||")));
  }
  RT_RET(rt_b(0));
}

static wv w4_parser(wv p_v184_toks, wv v185_inner) {
  rt_enter("parser");
  struct fr49 fr = { 0 };
  fr.v184_toks = p_v184_toks;
  v185_inner = rt_bind(v185_inner, 0);
  fr.v186_p = rt_i(0);
  fr.v187_depth = rt_i(0);
  fr.v188_exports = rt_arr(0, NULL, 1);
  fr.v189_firstExport = RT_NULL;
  if (rt_cond(v185_inner)) {
    wv v364_e = rt_bind(rt_chk(s213_expr(&fr), "parser"), 0);
    if (rt_cond(rt_not(rt_chk(s191_at(&fr, rt_s(&S110)), "parser")))) {
      wv v365_tk = rt_bind(rt_chk(s190_peek(&fr), "parser"), 0);
      (void)({ wv t532 = v365_tk; wv t533 = rt_cat(3, (wv[]){ rt_s(&S330), rt_text(rt_chk(w4_describe(v365_tk), "parser")), rt_s(&S331) }); rt_chk(w4_failAt(t532, t533), "parser"); });
    }
    RT_RET(v364_e);
  }
  RT_RET(rt_chk(s199_program(&fr), "parser"));
}

static wv s190_peek(struct fr49 *up) {
  rt_enter("peek");
  RT_RET(({ wv t250 = rt_def(up->v186_p, "p"); wv t251 = up->v184_toks; rt_idx(t250, t251); }));
}

static wv s191_at(struct fr49 *up, wv v229_type) {
  rt_enter("at");
  v229_type = rt_bind(v229_type, 0);
  RT_RET(({ wv t254 = rt_get(&F_t, ({ wv t252 = rt_def(up->v186_p, "p"); wv t253 = up->v184_toks; rt_idx(t252, t253); })); wv t255 = v229_type; rt_eq(t254, t255); }));
}

static wv s192_atWord(struct fr49 *up, wv v230_word) {
  rt_enter("atWord");
  v230_word = rt_bind(v230_word, 0);
  RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t256 = rt_def(up->v186_p, "p"); wv t257 = up->v184_toks; rt_idx(t256, t257); })), rt_s(&S137)), "&&") && rt_bool(({ wv t260 = rt_get(&F_v, ({ wv t258 = rt_def(up->v186_p, "p"); wv t259 = up->v184_toks; rt_idx(t258, t259); })); wv t261 = v230_word; rt_eq(t260, t261); }), "&&")));
}

static wv s193_atCall(struct fr49 *up) {
  rt_enter("atCall");
  if (rt_cond(rt_b(rt_bool(({ wv t262 = rt_add(rt_def(up->v186_p, "p"), rt_i(2)); wv t263 = std_length(up->v184_toks); rt_ge(t262, t263); }), "||") || rt_bool(rt_ne(rt_get(&F_t, ({ wv t264 = rt_def(up->v186_p, "p"); wv t265 = up->v184_toks; rt_idx(t264, t265); })), rt_s(&S97)), "||")))) {
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t266 = rt_add(rt_def(up->v186_p, "p"), rt_i(2)); wv t267 = up->v184_toks; rt_idx(t266, t267); })), rt_s(&S16)), "&&") && rt_bool(({ wv t270 = rt_def(w4_callees, "callees"); wv t271 = rt_get(&F_t, ({ wv t268 = rt_add(rt_def(up->v186_p, "p"), rt_i(1)); wv t269 = up->v184_toks; rt_idx(t268, t269); })); rt_chk(w1_contains(t270, t271), "atCall"); }), "&&")))) {
    RT_RET(rt_b(1));
  }
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(({ wv t272 = rt_add(rt_def(up->v186_p, "p"), rt_i(4)); wv t273 = std_length(up->v184_toks); rt_lt(t272, t273); }), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t274 = rt_add(rt_def(up->v186_p, "p"), rt_i(1)); wv t275 = up->v184_toks; rt_idx(t274, t275); })), rt_s(&S137)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t276 = rt_add(rt_def(up->v186_p, "p"), rt_i(2)); wv t277 = up->v184_toks; rt_idx(t276, t277); })), rt_s(&S129)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t278 = rt_add(rt_def(up->v186_p, "p"), rt_i(3)); wv t279 = up->v184_toks; rt_idx(t278, t279); })), rt_s(&S137)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t280 = rt_add(rt_def(up->v186_p, "p"), rt_i(4)); wv t281 = up->v184_toks; rt_idx(t280, t281); })), rt_s(&S16)), "&&")));
}

static wv s194_next(struct fr49 *up) {
  rt_enter("next");
  wv v231_tk = rt_bind(({ wv t282 = rt_def(up->v186_p, "p"); wv t283 = up->v184_toks; rt_idx(t282, t283); }), 0);
  if (rt_cond(rt_ne(rt_get(&F_t, v231_tk), rt_s(&S110)))) {
    up->v186_p = rt_add(rt_def(up->v186_p, "p"), rt_i(1));
  }
  RT_RET(v231_tk);
}

static wv s195_accept(struct fr49 *up, wv v232_type) {
  rt_enter("accept");
  v232_type = rt_bind(v232_type, 0);
  RT_RET((rt_cond(rt_chk(s191_at(up, v232_type), "accept")) ? rt_chk(s194_next(up), "accept") : RT_NULL));
}

static wv s196_expect(struct fr49 *up, wv v233_type, wv v234_msg, wv v235_hint) {
  rt_enter("expect");
  v233_type = rt_bind(v233_type, 0);
  v234_msg = rt_bind(v234_msg, 0);
  v235_hint = rt_bind(v235_hint, 0);
  if (rt_cond(rt_chk(s191_at(up, v233_type), "expect"))) {
    RT_RET(rt_chk(s194_next(up), "expect"));
  }
  wv v236_found = rt_bind(rt_chk(s190_peek(up), "expect"), 0);
  wv v237_m = rt_bind((rt_cond(rt_eq(v234_msg, RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S194), rt_text(v233_type), rt_s(&S113) }) : v234_msg), 0);
  (void)({ wv t286 = v236_found; wv t287 = ({ wv t284 = rt_text(v237_m); wv t285 = rt_text(rt_chk(w4_describe(v236_found), "expect")); rt_cat(3, (wv[]){ t284, rt_s(&S195), t285 }); }); wv t288 = v235_hint; rt_chk(w4_failAtHint(t286, t287, t288), "expect"); });
  RT_RET(RT_NULL);
}

static wv s197_expectWord(struct fr49 *up, wv v238_word, wv v239_hint) {
  rt_enter("expectWord");
  v238_word = rt_bind(v238_word, 0);
  v239_hint = rt_bind(v239_hint, 0);
  if (rt_cond(rt_chk(s192_atWord(up, v238_word), "expectWord"))) {
    RT_RET(rt_chk(s194_next(up), "expectWord"));
  }
  wv v240_found = rt_bind(rt_chk(s190_peek(up), "expectWord"), 0);
  (void)({ wv t291 = v240_found; wv t292 = ({ wv t289 = rt_text(v238_word); wv t290 = rt_text(rt_chk(w4_describe(v240_found), "expectWord")); rt_cat(4, (wv[]){ rt_s(&S194), t289, rt_s(&S196), t290 }); }); wv t293 = v239_hint; rt_chk(w4_failAtHint(t291, t292, t293), "expectWord"); });
  RT_RET(RT_NULL);
}

static wv s198_lintSpace(struct fr49 *up, wv v241_op) {
  rt_enter("lintSpace");
  v241_op = rt_bind(v241_op, 0);
  if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_sp, v241_op)), "||") || rt_bool(rt_not(rt_get(&F_sp, rt_chk(s190_peek(up), "lintSpace"))), "||")))) {
    (void)({ wv t296 = w2_warnings; wv t297 = ({ wv t294 = rt_get(&F_pos, v241_op); wv t295 = rt_cat(3, (wv[]){ rt_s(&S197), rt_text(rt_get(&F_t, v241_op)), rt_s(&S113) }); rt_chk(w2_warn(t294, t295, RT_NULL), "lintSpace"); }); std_push(t296, t297); });
  }
  RT_RET(RT_NULL);
}

static wv s199_program(struct fr49 *up) {
  rt_enter("program");
  wv v242_stmts = rt_arr(0, NULL, 1);
  wv v243_started = rt_b(0);
  wv v244_ended = RT_NULL;
  while (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S110)), "program")))) {
    if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S14)), "program")))) {
      wv v246_tk = rt_bind(rt_chk(s190_peek(up), "program"), 0);
      wv v247_hint = rt_bind((rt_cond(rt_chk(s191_at(up, rt_s(&S97)), "program")) ? rt_s(&S198) : rt_s(&S199)), 0);
      (void)({ wv t298 = v246_tk; wv t299 = rt_cat(2, (wv[]){ rt_s(&S200), rt_text(rt_chk(w4_describe(v246_tk), "program")) }); wv t300 = v247_hint; rt_chk(w4_failAtHint(t298, t299, t300), "program"); });
    }
    wv v245_s = rt_bind(rt_chk(s200_statement(up), "program"), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v244_ended, RT_NULL), "&&") && rt_bool(rt_ne(rt_get(&F_k, v245_s), rt_s(&S201)), "&&")))) {
      (void)rt_chk(w4_failAt(v244_ended, rt_s(&S202)), "program");
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v245_s), rt_s(&S47)), "&&") && rt_bool(v243_started, "&&")))) {
      (void)rt_chk(w2_abort(rt_get(&F_pos, v245_s), rt_s(&S203)), "program");
    }
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, v245_s), rt_s(&S47)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v245_s), rt_s(&S201)), "&&")))) {
      v243_started = rt_b(1);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, v245_s), rt_s(&S204)))) {
      v244_ended = rt_rec(1, 1, K6, (wv[]){ rt_get(&F_pos, v245_s) });
    }
    (void)std_push(v242_stmts, v245_s);
  }
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_def(up->v189_firstExport, "firstExport"), RT_NULL), "&&") && rt_bool(rt_eq(v244_ended, RT_NULL), "&&")))) {
    (void)rt_chk(w4_failAt(rt_def(up->v189_firstExport, "firstExport"), rt_s(&S205)), "program");
  }
  RT_RET(({ wv t301 = v242_stmts; wv t302 = rt_get(&F_pos, rt_chk(s190_peek(up), "program")); rt_rec(0, 2, K7, (wv[]){ t301, t302 }); }));
}

static wv s200_statement(struct fr49 *up) {
  rt_enter("statement");
  wv v248_semi = rt_bind(rt_chk(s194_next(up), "statement"), 0);
  wv v249_tk = rt_bind(rt_chk(s190_peek(up), "statement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v249_tk), rt_s(&S14)), "||") || rt_bool(rt_eq(rt_get(&F_t, v249_tk), rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v249_tk), rt_s(&S110)), "||")))) {
    RT_RET(({ wv t303 = rt_get(&F_pos, v248_semi); wv t304 = rt_get(&F_pos, v249_tk); rt_rec(0, 4, K8, (wv[]){ rt_s(&S201), t303, t304, RT_NULL }); }));
  }
  wv v250_s = rt_bind(rt_chk(s201_statementBody(up, v249_tk), "statement"), 1);
  ({ wv t305 = rt_get(&F_pos, v249_tk); wv t306 = v250_s; rt_set_mem(t305, &F_pos, t306); });
  if (rt_cond(rt_chk(s191_at(up, rt_s(&S56)), "statement"))) {
    wv v251_w = rt_bind(rt_chk(s194_next(up), "statement"), 0);
    if (rt_cond(rt_not(({ wv t307 = rt_def(w4_onfailOk, "onfailOk"); wv t308 = rt_get(&F_k, v250_s); rt_chk(w1_contains(t307, t308), "statement"); })))) {
      (void)rt_chk(w4_failAt(v251_w, rt_s(&S206)), "statement");
    }
    wv v252_param = RT_NULL;
    if (rt_cond(rt_ne(rt_chk(s195_accept(up, rt_s(&S97)), "statement"), RT_NULL))) {
      v252_param = rt_bind(rt_chk(s196_expect(up, rt_s(&S137), rt_s(&S207), RT_NULL), "statement"), 1);
      (void)rt_chk(s196_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "statement");
    }
    wv v253_noParam = rt_eq(v252_param, RT_NULL);
    ({ wv t313 = ({ wv t309 = rt_get(&F_pos, v251_w); wv t310 = (rt_cond(v253_noParam) ? RT_NULL : rt_get(&F_v, v252_param)); wv t311 = (rt_cond(v253_noParam) ? RT_NULL : rt_get(&F_pos, v252_param)); wv t312 = rt_chk(s211_block(up), "statement"); rt_rec(0, 4, K9, (wv[]){ t309, t310, t311, t312 }); }); wv t314 = v250_s; rt_set_mem(t313, &F_handler, t314); });
  }
  ({ wv t315 = rt_get(&F_pos, rt_chk(s190_peek(up), "statement")); wv t316 = v250_s; rt_set_mem(t315, &F_end, t316); });
  RT_RET(v250_s);
}

static wv s201_statementBody(struct fr49 *up, wv v254_tk) {
  rt_enter("statementBody");
  v254_tk = rt_bind(v254_tk, 0);
  wv v255_kind = rt_bind(rt_get(&F_t, v254_tk), 0);
  if (rt_cond(rt_eq(v255_kind, rt_s(&S33)))) {
    RT_RET(rt_chk(s208_fnStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S46)))) {
    RT_RET(rt_chk(s209_enumStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S47)))) {
    RT_RET(rt_chk(s205_importStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S48)))) {
    RT_RET(rt_chk(s206_exportStatement(up), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v255_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v255_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v255_kind, rt_s(&S37)), "||")))) {
    RT_RET(rt_chk(s202_declaration(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S38)))) {
    RT_RET(rt_chk(s203_setStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S34)))) {
    (void)rt_chk(s194_next(up), "statementBody");
    wv v257_e = rt_bind((rt_cond(({ wv t317 = rt_def(w4_exprStart, "exprStart"); wv t318 = rt_get(&F_t, rt_chk(s190_peek(up), "statementBody")); rt_chk(w1_contains(t317, t318), "statementBody"); })) ? rt_chk(s213_expr(up), "statementBody") : RT_NULL), 0);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S34), v257_e), "statementBody"));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S57)))) {
    (void)rt_chk(s194_next(up), "statementBody");
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S57), rt_chk(s213_expr(up), "statementBody")), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v255_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v255_kind, rt_s(&S43)), "||")))) {
    (void)rt_chk(s194_next(up), "statementBody");
    RT_RET(rt_rec(0, 4, K8, (wv[]){ (rt_cond(rt_eq(v255_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43)), RT_NULL, RT_NULL, RT_NULL }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v255_kind, rt_s(&S41)), "||") || rt_bool(rt_eq(v255_kind, rt_s(&S208)), "||")))) {
    (void)rt_chk(s194_next(up), "statementBody");
    RT_RET(({ wv t319 = rt_eq(v255_kind, rt_s(&S208)); wv t320 = rt_chk(s210_cond(up, v255_kind), "statementBody"); wv t321 = rt_chk(s211_block(up), "statementBody"); rt_rec(0, 7, K10, (wv[]){ rt_s(&S41), t319, t320, t321, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S55)))) {
    RT_RET(rt_chk(s204_failStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S54)))) {
    (void)rt_chk(s194_next(up), "statementBody");
    wv v258_e = rt_bind((rt_cond(({ wv t322 = rt_def(w4_exprStart, "exprStart"); wv t323 = rt_get(&F_t, rt_chk(s190_peek(up), "statementBody")); rt_chk(w1_contains(t322, t323), "statementBody"); })) ? rt_chk(s213_expr(up), "statementBody") : RT_NULL), 0);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S54), v258_e), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v255_kind, rt_s(&S137)), "&&") && rt_bool(rt_eq(rt_get(&F_v, v254_tk), rt_s(&S204)), "&&")))) {
    wv v259_after = rt_bind(rt_get(&F_t, ({ wv t324 = rt_add(rt_def(up->v186_p, "p"), rt_i(1)); wv t325 = up->v184_toks; rt_idx(t324, t325); })), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v259_after, rt_s(&S14)), "||") || rt_bool(rt_eq(v259_after, rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(v259_after, rt_s(&S110)), "||")))) {
      RT_RET(rt_chk(s207_endStatement(up), "statementBody"));
    }
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S56)))) {
    (void)rt_chk(w4_failAt(v254_tk, rt_s(&S206)), "statementBody");
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S40)))) {
    (void)rt_chk(w4_failAt(v254_tk, rt_s(&S209)), "statementBody");
  }
  if (rt_cond(rt_eq(v255_kind, rt_s(&S45)))) {
    (void)rt_chk(w4_failAt(v254_tk, rt_s(&S210)), "statementBody");
  }
  wv v256_e = rt_bind(rt_chk(s213_expr(up), "statementBody"), 0);
  if (rt_cond(rt_b(rt_bool(rt_chk(s191_at(up, rt_s(&S111)), "statementBody"), "||") || rt_bool(rt_b(rt_bool(rt_chk(s192_atWord(up, rt_s(&S211)), "statementBody"), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v256_e), rt_s(&S212)), "||") || rt_bool(rt_eq(rt_get(&F_k, v256_e), rt_s(&S189)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v256_e), rt_s(&S190)), "||")), "&&")), "||")))) {
    (void)rt_chk(w4_failAtHint(v254_tk, rt_s(&S213), rt_s(&S214)), "statementBody");
  }
  if (rt_cond(rt_not(rt_chk(w4_hasEffect(v256_e), "statementBody")))) {
    wv v260_hint = rt_bind((rt_cond(rt_gt(rt_def(up->v187_depth, "depth"), rt_i(0))) ? rt_s(&S215) : RT_NULL), 0);
    (void)({ wv t328 = w2_warnings; wv t329 = ({ wv t326 = rt_get(&F_pos, v254_tk); wv t327 = v260_hint; rt_chk(w2_warn(t326, rt_s(&S216), t327), "statementBody"); }); std_push(t328, t329); });
  }
  RT_RET(rt_chk(w4_simpleStatement(rt_s(&S176), v256_e), "statementBody"));
}

static wv s202_declaration(struct fr49 *up) {
  rt_enter("declaration");
  wv v261_kw = rt_bind(rt_chk(s194_next(up), "declaration"), 0);
  wv v262_name = rt_bind(({ wv t330 = rt_cat(3, (wv[]){ rt_s(&S217), rt_text(rt_get(&F_t, v261_kw)), rt_s(&S113) }); wv t331 = rt_cat(3, (wv[]){ rt_s(&S14), rt_text(rt_get(&F_t, v261_kw)), rt_s(&S218) }); rt_chk(s196_expect(up, rt_s(&S137), t330, t331), "declaration"); }), 0);
  (void)rt_chk(s197_expectWord(up, rt_s(&S219), ({ wv t332 = rt_text(rt_get(&F_t, v261_kw)); wv t333 = rt_text(rt_get(&F_v, v262_name)); rt_cat(5, (wv[]){ rt_s(&S14), t332, rt_s(&S0), t333, rt_s(&S220) }); })), "declaration");
  wv v263_e = rt_bind(rt_chk(s213_expr(up), "declaration"), 0);
  RT_RET(({ wv t334 = v263_e; wv t335 = rt_eq(rt_get(&F_t, v261_kw), rt_s(&S36)); wv t336 = rt_eq(rt_get(&F_t, v261_kw), rt_s(&S37)); wv t337 = rt_get(&F_v, v262_name); wv t338 = rt_get(&F_pos, v262_name); rt_rec(0, 9, K11, (wv[]){ rt_s(&S177), t334, t335, t336, t337, t338, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s203_setStatement(struct fr49 *up) {
  rt_enter("setStatement");
  (void)rt_chk(s194_next(up), "setStatement");
  wv v264_target = rt_bind(rt_chk(s212_lvalue(up), "setStatement"), 0);
  if (rt_cond(rt_chk(s192_atWord(up, rt_s(&S211)), "setStatement"))) {
    (void)rt_chk(s194_next(up), "setStatement");
    RT_RET(({ wv t339 = rt_chk(s213_expr(up), "setStatement"); wv t340 = v264_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t339, RT_NULL, t340, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_chk(s191_at(up, rt_s(&S111)), "setStatement"))) {
    wv v266_op = rt_bind(rt_chk(s194_next(up), "setStatement"), 0);
    RT_RET(({ wv t341 = rt_chk(s213_expr(up), "setStatement"); wv t342 = rt_get(&F_v, v266_op); wv t343 = v264_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t341, t342, t343, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  wv v265_found = rt_bind(rt_chk(s190_peek(up), "setStatement"), 0);
  (void)({ wv t344 = v265_found; wv t345 = rt_cat(2, (wv[]){ rt_s(&S221), rt_text(rt_chk(w4_describe(v265_found), "setStatement")) }); rt_chk(w4_failAtHint(t344, t345, rt_s(&S214)), "setStatement"); });
  RT_RET(RT_NULL);
}

static wv s204_failStatement(struct fr49 *up) {
  rt_enter("failStatement");
  wv v267_kw = rt_bind(rt_chk(s194_next(up), "failStatement"), 0);
  if (rt_cond(rt_not(({ wv t346 = rt_def(w4_exprStart, "exprStart"); wv t347 = rt_get(&F_t, rt_chk(s190_peek(up), "failStatement")); rt_chk(w1_contains(t346, t347), "failStatement"); })))) {
    RT_RET(rt_rec(0, 5, K13, (wv[]){ rt_s(&S55), rt_i(1), RT_NULL, RT_NULL, RT_NULL }));
  }
  wv v268_first = rt_bind(rt_chk(s190_peek(up), "failStatement"), 0);
  wv v269_e = rt_bind(rt_chk(s213_expr(up), "failStatement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v269_e), rt_s(&S148)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v269_e), rt_s(&S138)), "&&") && rt_bool(rt_lt(rt_get(&F_v, v269_e), rt_i(1)), "&&")), "||")))) {
    (void)rt_chk(w4_failAt(v268_first, rt_s(&S222)), "failStatement");
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v269_e), rt_s(&S138)))) {
    RT_RET(rt_rec(0, 5, K13, (wv[]){ rt_s(&S55), rt_get(&F_v, v269_e), RT_NULL, RT_NULL, RT_NULL }));
  }
  RT_RET(rt_chk(w4_simpleStatement(rt_s(&S176), ({ wv t348 = v269_e; wv t349 = rt_get(&F_pos, v267_kw); rt_rec(0, 4, K14, (wv[]){ rt_s(&S183), t348, t349, rt_b(0) }); })), "failStatement"));
}

static wv s205_importStatement(struct fr49 *up) {
  rt_enter("importStatement");
  wv v270_kw = rt_bind(rt_chk(s194_next(up), "importStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v187_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v270_kw, rt_s(&S223)), "importStatement");
  }
  wv v271_name = rt_bind(rt_chk(s196_expect(up, rt_s(&S137), rt_s(&S224), rt_s(&S225)), "importStatement"), 0);
  (void)rt_chk(s197_expectWord(up, rt_s(&S226), ({ wv t350 = rt_text(rt_get(&F_v, v271_name)); wv t351 = rt_text(rt_get(&F_v, v271_name)); rt_cat(5, (wv[]){ rt_s(&S227), t350, rt_s(&S228), t351, rt_s(&S229) }); })), "importStatement");
  wv v272_e = rt_bind(rt_chk(s190_peek(up), "importStatement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_t, v272_e), rt_s(&S171)), "||") || rt_bool(rt_ne(std_length(rt_get(&F_parts, v272_e)), rt_i(1)), "||")))) {
    (void)rt_chk(w4_failAtHint(v272_e, rt_s(&S230), rt_s(&S225)), "importStatement");
  }
  (void)rt_chk(s194_next(up), "importStatement");
  wv v273_path = rt_bind(rt_idx(rt_i(0), rt_get(&F_parts, v272_e)), 0);
  if (rt_cond(rt_chk(w1_contains(std_split(v273_path, rt_s(&S13)), rt_s(&S19)), "importStatement"))) {
    (void)rt_chk(w4_failAt(v272_e, rt_s(&S231)), "importStatement");
  }
  if (rt_cond(rt_not(rt_b(rt_bool(rt_ge(std_length(v273_path), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t352 = v273_path; wv t353 = std_length(v273_path); std_slice(t352, rt_i(-4), t353); }), rt_s(&S232)), "&&"))))) {
    (void)rt_chk(w4_failAt(v272_e, rt_s(&S233)), "importStatement");
  }
  RT_RET(({ wv t354 = v273_path; wv t355 = rt_get(&F_pos, v272_e); wv t356 = rt_get(&F_v, v271_name); wv t357 = rt_get(&F_pos, v271_name); rt_rec(0, 8, K15, (wv[]){ rt_s(&S47), t354, t355, t356, t357, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s206_exportStatement(struct fr49 *up) {
  rt_enter("exportStatement");
  wv v274_kw = rt_bind(rt_chk(s194_next(up), "exportStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v187_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v274_kw, rt_s(&S234)), "exportStatement");
  }
  wv v275_tk = rt_bind(rt_chk(s190_peek(up), "exportStatement"), 0);
  if (rt_cond(rt_not(({ wv t358 = rt_def(w4_exportable, "exportable"); wv t359 = rt_get(&F_t, v275_tk); rt_chk(w1_contains(t358, t359), "exportStatement"); })))) {
    (void)({ wv t360 = v275_tk; wv t361 = rt_cat(2, (wv[]){ rt_s(&S235), rt_text(rt_chk(w4_describe(v275_tk), "exportStatement")) }); rt_chk(w4_failAt(t360, t361), "exportStatement"); });
  }
  wv v276_s = rt_bind(rt_chk(s201_statementBody(up, v275_tk), "exportStatement"), 0);
  if (rt_cond(rt_eq(rt_def(up->v189_firstExport, "firstExport"), RT_NULL))) {
    wv t362 = rt_bind(v274_kw, 1);
    (void)rt_def(up->v189_firstExport, "firstExport");
    up->v189_firstExport = t362;
  }
  (void)({ wv t365 = rt_def(up->v188_exports, "exports"); wv t366 = ({ wv t363 = rt_get(&F_name, v276_s); wv t364 = rt_get(&F_namePos, v276_s); rt_rec(0, 2, K16, (wv[]){ t363, t364 }); }); std_push(t365, t366); });
  RT_RET(v276_s);
}

static wv s207_endStatement(struct fr49 *up) {
  rt_enter("endStatement");
  wv v277_kw = rt_bind(rt_chk(s194_next(up), "endStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v187_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v277_kw, rt_s(&S236)), "endStatement");
  }
  if (rt_cond(rt_eq(std_length(rt_def(up->v188_exports, "exports")), rt_i(0)))) {
    (void)rt_chk(w4_failAtHint(v277_kw, rt_s(&S237), rt_s(&S238)), "endStatement");
  }
  RT_RET(rt_rec(0, 5, K17, (wv[]){ rt_s(&S204), rt_def(up->v188_exports, "exports"), RT_NULL, RT_NULL, RT_NULL }));
}

static wv s208_fnStatement(struct fr49 *up) {
  rt_enter("fnStatement");
  (void)rt_chk(s194_next(up), "fnStatement");
  if (rt_cond(rt_chk(s191_at(up, rt_s(&S97)), "fnStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s190_peek(up), "fnStatement"), rt_s(&S239), rt_s(&S240)), "fnStatement");
  }
  wv v278_name = rt_bind(rt_chk(s196_expect(up, rt_s(&S137), rt_s(&S241), rt_s(&S240)), "fnStatement"), 0);
  (void)rt_chk(s196_expect(up, rt_s(&S97), rt_s(&S242), rt_cat(3, (wv[]){ rt_s(&S243), rt_text(rt_get(&F_v, v278_name)), rt_s(&S244) })), "fnStatement");
  wv v279_params = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S16)), "fnStatement")))) {
    while (rt_cond(rt_b(1))) {
      wv v280_isVar = rt_ne(rt_chk(s195_accept(up, rt_s(&S36)), "fnStatement"), RT_NULL);
      wv v281_isRef = rt_b(rt_bool(rt_not(v280_isVar), "&&") && rt_bool(rt_ne(rt_chk(s195_accept(up, rt_s(&S37)), "fnStatement"), RT_NULL), "&&"));
      wv v282_pname = rt_bind(rt_chk(s196_expect(up, rt_s(&S137), rt_s(&S245), rt_s(&S246)), "fnStatement"), 0);
      (void)({ wv t371 = v279_params; wv t372 = ({ wv t367 = rt_get(&F_v, v282_pname); wv t368 = v280_isVar; wv t369 = v281_isRef; wv t370 = rt_get(&F_pos, v282_pname); rt_rec(0, 4, K18, (wv[]){ t367, t368, t369, t370 }); }); std_push(t371, t372); });
      if (rt_cond(rt_eq(rt_chk(s195_accept(up, rt_s(&S96)), "fnStatement"), RT_NULL))) {
        break;
      }
    }
  }
  (void)rt_chk(s196_expect(up, rt_s(&S16), rt_s(&S247), RT_NULL), "fnStatement");
  RT_RET(({ wv t373 = rt_get(&F_v, v278_name); wv t374 = rt_get(&F_pos, v278_name); wv t375 = v279_params; wv t376 = rt_chk(s211_block(up), "fnStatement"); rt_rec(0, 8, K19, (wv[]){ rt_s(&S33), t373, t374, t375, t376, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s209_enumStatement(struct fr49 *up) {
  rt_enter("enumStatement");
  wv v283_kw = rt_bind(rt_chk(s194_next(up), "enumStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v187_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v283_kw, rt_s(&S248)), "enumStatement");
  }
  if (rt_cond(rt_chk(s191_at(up, rt_s(&S97)), "enumStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s190_peek(up), "enumStatement"), rt_s(&S249), rt_s(&S250)), "enumStatement");
  }
  wv v284_name = rt_bind(rt_chk(s196_expect(up, rt_s(&S137), rt_s(&S251), rt_s(&S250)), "enumStatement"), 0);
  (void)rt_chk(s196_expect(up, rt_s(&S97), rt_s(&S252), rt_cat(3, (wv[]){ rt_s(&S253), rt_text(rt_get(&F_v, v284_name)), rt_s(&S254) })), "enumStatement");
  if (rt_cond(rt_chk(s191_at(up, rt_s(&S16)), "enumStatement"))) {
    (void)({ wv t377 = rt_chk(s190_peek(up), "enumStatement"); wv t378 = rt_cat(3, (wv[]){ rt_s(&S253), rt_text(rt_get(&F_v, v284_name)), rt_s(&S254) }); rt_chk(w4_failAtHint(t377, rt_s(&S255), t378), "enumStatement"); });
  }
  wv v285_members = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v286_m = rt_bind(rt_chk(s190_peek(up), "enumStatement"), 0);
    if (rt_cond(rt_ne(rt_get(&F_t, v286_m), rt_s(&S137)))) {
      wv v288_message = rt_bind((rt_cond(({ wv t379 = w3_keywords; wv t380 = rt_get(&F_t, v286_m); rt_chk(w1_contains(t379, t380), "enumStatement"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v286_m)), rt_s(&S256) }) : rt_cat(2, (wv[]){ rt_s(&S257), rt_text(rt_chk(w4_describe(v286_m), "enumStatement")) })), 0);
      (void)rt_chk(w4_failAt(v286_m, v288_message), "enumStatement");
    }
    (void)rt_chk(s194_next(up), "enumStatement");
    wv v287_k = rt_i(0);
    while (rt_cond(({ wv t381 = v287_k; wv t382 = std_length(v285_members); rt_lt(t381, t382); }))) {
      if (rt_cond(({ wv t383 = rt_get(&F_name, rt_idx(v287_k, v285_members)); wv t384 = rt_get(&F_v, v286_m); rt_eq(t383, t384); }))) {
        (void)({ wv t385 = v286_m; wv t386 = rt_cat(3, (wv[]){ rt_s(&S258), rt_text(rt_get(&F_v, v286_m)), rt_s(&S259) }); rt_chk(w4_failAt(t385, t386), "enumStatement"); });
      }
      v287_k = rt_add(v287_k, rt_i(1));
    }
    (void)({ wv t389 = v285_members; wv t390 = ({ wv t387 = rt_get(&F_v, v286_m); wv t388 = rt_get(&F_pos, v286_m); rt_rec(0, 2, K16, (wv[]){ t387, t388 }); }); std_push(t389, t390); });
    if (rt_cond(rt_eq(rt_chk(s195_accept(up, rt_s(&S96)), "enumStatement"), RT_NULL))) {
      break;
    }
  }
  (void)rt_chk(s196_expect(up, rt_s(&S16), rt_s(&S260), RT_NULL), "enumStatement");
  RT_RET(({ wv t391 = rt_get(&F_v, v284_name); wv t392 = rt_get(&F_pos, v284_name); wv t393 = v285_members; rt_rec(0, 7, K20, (wv[]){ rt_s(&S46), t391, t392, t393, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s210_cond(struct fr49 *up, wv v289_kw) {
  rt_enter("cond");
  v289_kw = rt_bind(v289_kw, 0);
  (void)({ wv t394 = rt_cat(3, (wv[]){ rt_s(&S261), rt_text(v289_kw), rt_s(&S113) }); wv t395 = rt_cat(3, (wv[]){ rt_s(&S262), rt_text(v289_kw), rt_s(&S263) }); rt_chk(s196_expect(up, rt_s(&S97), t394, t395), "cond"); });
  wv v290_e = rt_bind(rt_chk(s213_expr(up), "cond"), 0);
  (void)rt_chk(s196_expect(up, rt_s(&S16), rt_s(&S264), RT_NULL), "cond");
  RT_RET(v290_e);
}

static wv s211_block(struct fr49 *up) {
  rt_enter("block");
  (void)rt_chk(s196_expect(up, rt_s(&S100), rt_s(&S265), RT_NULL), "block");
  up->v187_depth = rt_add(rt_def(up->v187_depth, "depth"), rt_i(1));
  wv v291_stmts = rt_arr(0, NULL, 1);
  while (rt_cond(rt_chk(s191_at(up, rt_s(&S14)), "block"))) {
    (void)({ wv t396 = v291_stmts; wv t397 = rt_chk(s200_statement(up), "block"); std_push(t396, t397); });
  }
  if (rt_cond(rt_chk(s191_at(up, rt_s(&S15)), "block"))) {
    wv v294_close = rt_bind(rt_chk(s194_next(up), "block"), 0);
    up->v187_depth = rt_sub(rt_def(up->v187_depth, "depth"), rt_i(1));
    RT_RET(({ wv t398 = v291_stmts; wv t399 = rt_get(&F_pos, v294_close); rt_rec(0, 3, K21, (wv[]){ t398, RT_NULL, t399 }); }));
  }
  if (rt_cond(rt_chk(s191_at(up, rt_s(&S110)), "block"))) {
    (void)rt_chk(w4_failAt(rt_chk(s190_peek(up), "block"), rt_s(&S266)), "block");
  }
  wv v292_value = rt_bind(rt_chk(s213_expr(up), "block"), 0);
  if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S15)), "block")))) {
    wv v295_hint = rt_bind((rt_cond(rt_chk(s191_at(up, rt_s(&S28)), "block")) ? rt_s(&S267) : rt_s(&S268)), 0);
    wv v296_found = rt_bind(rt_chk(s190_peek(up), "block"), 0);
    (void)({ wv t400 = v296_found; wv t401 = rt_cat(2, (wv[]){ rt_s(&S269), rt_text(rt_chk(w4_describe(v296_found), "block")) }); wv t402 = v295_hint; rt_chk(w4_failAtHint(t400, t401, t402), "block"); });
  }
  wv v293_close = rt_bind(rt_chk(s194_next(up), "block"), 0);
  up->v187_depth = rt_sub(rt_def(up->v187_depth, "depth"), rt_i(1));
  RT_RET(({ wv t403 = v291_stmts; wv t404 = v292_value; wv t405 = rt_get(&F_pos, v293_close); rt_rec(0, 3, K21, (wv[]){ t403, t404, t405 }); }));
}

static wv s212_lvalue(struct fr49 *up) {
  rt_enter("lvalue");
  wv v297_tk = rt_bind(rt_chk(s190_peek(up), "lvalue"), 0);
  wv v298_e = rt_bind(rt_chk(s220_postfix(up), "lvalue"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_k, v298_e), rt_s(&S212)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v298_e), rt_s(&S189)), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_k, v298_e), rt_s(&S190)), "&&")))) {
    (void)rt_chk(w4_failAt(v297_tk, rt_s(&S270)), "lvalue");
  }
  RT_RET(v298_e);
}

static wv s213_expr(struct fr49 *up) {
  rt_enter("expr");
  RT_RET(rt_chk(s214_ternary(up), "expr"));
}

static wv s214_ternary(struct fr49 *up) {
  rt_enter("ternary");
  RT_RET(rt_chk(s215_ternaryTail(up, rt_chk(s216_binary(up, rt_i(1)), "ternary")), "ternary"));
}

static wv s215_ternaryTail(struct fr49 *up, wv v299_c) {
  rt_enter("ternaryTail");
  if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S95)), "ternaryTail")))) {
    RT_RET(v299_c);
  }
  wv v300_q = rt_bind(rt_chk(s194_next(up), "ternaryTail"), 0);
  (void)rt_chk(s198_lintSpace(up, v300_q), "ternaryTail");
  wv v301_a = rt_bind(rt_chk(s214_ternary(up), "ternaryTail"), 0);
  wv v302_colon = rt_bind(rt_chk(s196_expect(up, rt_s(&S28), rt_s(&S271), RT_NULL), "ternaryTail"), 0);
  (void)rt_chk(s198_lintSpace(up, v302_colon), "ternaryTail");
  wv v303_b = rt_bind(rt_chk(s214_ternary(up), "ternaryTail"), 0);
  RT_RET(({ wv t406 = v299_c; wv t407 = v301_a; wv t408 = v303_b; wv t409 = rt_get(&F_pos, v300_q); rt_rec(0, 6, K22, (wv[]){ rt_s(&S193), t406, t407, t408, t409, rt_b(0) }); }));
}

static wv s216_binary(struct fr49 *up, wv v304_min) {
  rt_enter("binary");
  v304_min = rt_bind(v304_min, 0);
  RT_RET(({ wv t410 = rt_chk(s218_unary(up), "binary"); wv t411 = v304_min; rt_chk(s217_binaryFrom(up, t410, t411), "binary"); }));
}

static wv s217_binaryFrom(struct fr49 *up, wv v305_left, wv v306_min) {
  rt_enter("binaryFrom");
  v305_left = rt_bind(v305_left, 0);
  v306_min = rt_bind(v306_min, 0);
  wv v307_acc = v305_left;
  while (rt_cond(rt_b(1))) {
    wv v308_op = rt_bind(rt_chk(s190_peek(up), "binaryFrom"), 0);
    wv v309_prec = rt_bind(rt_chk(w4_precedence(rt_get(&F_t, v308_op)), "binaryFrom"), 0);
    if (rt_cond(rt_lt(v309_prec, v306_min))) {
      RT_RET(v307_acc);
    }
    (void)rt_chk(s194_next(up), "binaryFrom");
    (void)rt_chk(s198_lintSpace(up, v308_op), "binaryFrom");
    wv v310_right = rt_chk(s216_binary(up, rt_add(v309_prec, rt_i(1))), "binaryFrom");
    v307_acc = ({ wv t412 = rt_get(&F_t, v308_op); wv t413 = v307_acc; wv t414 = v310_right; wv t415 = rt_get(&F_pos, v308_op); rt_rec(0, 6, K23, (wv[]){ rt_s(&S192), t412, t413, t414, t415, rt_b(0) }); });
  }
  RT_RET(RT_NULL);
}

static wv s218_unary(struct fr49 *up) {
  rt_enter("unary");
  wv v311_tk = rt_bind(rt_chk(s190_peek(up), "unary"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v311_tk), rt_s(&S85)), "||") || rt_bool(rt_eq(rt_get(&F_t, v311_tk), rt_s(&S94)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v311_tk), rt_s(&S93)), "||")))) {
    (void)rt_chk(s194_next(up), "unary");
    wv v312_e = rt_bind(rt_chk(s218_unary(up), "unary"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v311_tk), rt_s(&S85)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v312_e), rt_s(&S192)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v312_e), rt_s(&S75)), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v312_e)), "&&")))) {
      (void)rt_chk(w4_failAtHint(v311_tk, rt_s(&S272), rt_s(&S273)), "unary");
    }
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v311_tk), rt_s(&S85)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v312_e), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_k, v312_e), rt_s(&S148)), "||")), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v312_e)), "&&")))) {
      RT_RET(({ wv t416 = rt_get(&F_k, v312_e); wv t417 = rt_neg(rt_get(&F_v, v312_e)); wv t418 = rt_get(&F_pos, v311_tk); rt_rec(0, 4, K24, (wv[]){ t416, t417, t418, rt_b(0) }); }));
    }
    RT_RET(({ wv t419 = rt_get(&F_t, v311_tk); wv t420 = v312_e; wv t421 = rt_get(&F_pos, v311_tk); rt_rec(0, 5, K25, (wv[]){ rt_s(&S191), t419, t420, t421, rt_b(0) }); }));
  }
  RT_RET(rt_chk(s219_powerTail(up, rt_chk(s220_postfix(up), "unary")), "unary"));
}

static wv s219_powerTail(struct fr49 *up, wv v313_base) {
  rt_enter("powerTail");
  if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S75)), "powerTail")))) {
    RT_RET(v313_base);
  }
  wv v314_op = rt_bind(rt_chk(s194_next(up), "powerTail"), 0);
  (void)rt_chk(s198_lintSpace(up, v314_op), "powerTail");
  RT_RET(({ wv t422 = v313_base; wv t423 = rt_chk(s218_unary(up), "powerTail"); wv t424 = rt_get(&F_pos, v314_op); rt_rec(0, 6, K23, (wv[]){ rt_s(&S192), rt_s(&S75), t422, t423, t424, rt_b(0) }); }));
}

static wv s220_postfix(struct fr49 *up) {
  rt_enter("postfix");
  wv v315_e = rt_chk(s222_primary(up), "postfix");
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s193_atCall(up), "postfix"))) {
      v315_e = rt_chk(s221_call(up, rt_arr(1, (wv[]){ v315_e }, 0)), "postfix");
    } else if (rt_cond(rt_chk(s191_at(up, rt_s(&S98)), "postfix"))) {
      wv v316_open = rt_bind(rt_chk(s194_next(up), "postfix"), 0);
      if (rt_cond(rt_b(rt_bool(rt_get(&F_nl, v316_open), "&&") && rt_bool(rt_gt(rt_def(up->v187_depth, "depth"), rt_i(0)), "&&")))) {
        (void)({ wv t425 = w2_warnings; wv t426 = rt_chk(w2_warn(rt_get(&F_pos, v316_open), rt_s(&S274), rt_s(&S275)), "postfix"); std_push(t425, t426); });
      }
      wv v317_index = rt_bind(rt_chk(s213_expr(up), "postfix"), 0);
      (void)rt_chk(s196_expect(up, rt_s(&S99), rt_s(&S276), RT_NULL), "postfix");
      v315_e = ({ wv t427 = v317_index; wv t428 = v315_e; wv t429 = rt_get(&F_pos, v316_open); rt_rec(0, 5, K26, (wv[]){ rt_s(&S189), t427, t428, t429, rt_b(0) }); });
    } else if (rt_cond(rt_chk(s191_at(up, rt_s(&S129)), "postfix"))) {
      (void)rt_chk(s194_next(up), "postfix");
      wv v318_tk = rt_bind(rt_chk(s190_peek(up), "postfix"), 0);
      if (rt_cond(rt_ne(rt_get(&F_t, v318_tk), rt_s(&S137)))) {
        wv v319_message = rt_bind((rt_cond(({ wv t430 = w3_keywords; wv t431 = rt_get(&F_t, v318_tk); rt_chk(w1_contains(t430, t431), "postfix"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v318_tk)), rt_s(&S277) }) : rt_cat(2, (wv[]){ rt_s(&S278), rt_text(rt_chk(w4_describe(v318_tk), "postfix")) })), 0);
        (void)rt_chk(w4_failAtHint(v318_tk, v319_message, rt_s(&S133)), "postfix");
      }
      (void)rt_chk(s194_next(up), "postfix");
      v315_e = ({ wv t432 = rt_get(&F_v, v318_tk); wv t433 = v315_e; wv t434 = rt_get(&F_pos, v318_tk); rt_rec(0, 5, K27, (wv[]){ rt_s(&S190), t432, t433, t434, rt_b(0) }); });
    } else {
      break;
    }
  }
  RT_RET(v315_e);
}

static wv s221_call(struct fr49 *up, wv v320_args) {
  rt_enter("call");
  (void)rt_chk(s194_next(up), "call");
  wv v321_c = rt_bind(rt_chk(s194_next(up), "call"), 0);
  if (rt_cond(rt_ne(rt_chk(s195_accept(up, rt_s(&S129)), "call"), RT_NULL))) {
    wv v322_m = rt_bind(rt_chk(s194_next(up), "call"), 0);
    (void)rt_chk(s194_next(up), "call");
    wv v323_rec = ({ wv t435 = rt_get(&F_v, v321_c); wv t436 = rt_get(&F_pos, v321_c); rt_rec(0, 4, K28, (wv[]){ rt_s(&S212), t435, t436, rt_b(0) }); });
    RT_RET(({ wv t440 = ({ wv t437 = rt_get(&F_v, v322_m); wv t438 = v323_rec; wv t439 = rt_get(&F_pos, v322_m); rt_rec(0, 5, K27, (wv[]){ rt_s(&S190), t437, t438, t439, rt_b(0) }); }); wv t441 = v320_args; wv t442 = rt_get(&F_pos, v322_m); rt_rec(0, 5, K29, (wv[]){ rt_s(&S182), t440, t441, t442, rt_b(0) }); }));
  }
  (void)rt_chk(s194_next(up), "call");
  RT_RET(({ wv t446 = ({ wv t443 = (rt_cond(rt_eq(rt_get(&F_t, v321_c), rt_s(&S137))) ? rt_s(&S212) : rt_s(&S156)); wv t444 = rt_get(&F_v, v321_c); wv t445 = rt_get(&F_pos, v321_c); rt_rec(0, 4, K28, (wv[]){ t443, t444, t445, rt_b(0) }); }); wv t447 = v320_args; wv t448 = rt_get(&F_pos, v321_c); rt_rec(0, 5, K29, (wv[]){ rt_s(&S182), t446, t447, t448, rt_b(0) }); }));
}

static wv s222_primary(struct fr49 *up) {
  rt_enter("primary");
  wv v324_tk = rt_bind(rt_chk(s190_peek(up), "primary"), 0);
  wv v325_at0 = rt_bind(rt_get(&F_pos, v324_tk), 0);
  wv v326_kind = rt_bind(rt_get(&F_t, v324_tk), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v326_kind, rt_s(&S39)), "||") || rt_bool(rt_eq(v326_kind, rt_s(&S175)), "||")))) {
    RT_RET(rt_chk(s228_chain(up), "primary"));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S44)))) {
    RT_RET(rt_chk(s226_parseMatch(up), "primary"));
  }
  if (rt_cond(rt_chk(s193_atCall(up), "primary"))) {
    RT_RET(rt_chk(s221_call(up, rt_arr(0, NULL, 0)), "primary"));
  }
  (void)rt_chk(s194_next(up), "primary");
  if (rt_cond(rt_b(rt_bool(rt_eq(v326_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v326_kind, rt_s(&S148)), "||")))) {
    RT_RET(({ wv t449 = v326_kind; wv t450 = rt_get(&F_v, v324_tk); wv t451 = v325_at0; rt_rec(0, 4, K24, (wv[]){ t449, t450, t451, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S171)))) {
    wv v327_parts = rt_arr(0, NULL, 1);
    wv v328_k = rt_i(0);
    while (rt_cond(({ wv t452 = v328_k; wv t453 = std_length(rt_get(&F_parts, v324_tk)); rt_lt(t452, t453); }))) {
      wv v329_part = rt_bind(({ wv t454 = v328_k; wv t455 = rt_get(&F_parts, v324_tk); rt_idx(t454, t455); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v329_part), rt_s(&S185)))) {
        (void)std_push(v327_parts, v329_part);
      } else {
        (void)({ wv t456 = v327_parts; wv t457 = rt_chk(w4_parser(rt_get(&F_tokens, v329_part), rt_b(1)), "primary"); std_push(t456, t457); });
      }
      v328_k = rt_add(v328_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 4, K30, (wv[]){ rt_s(&S171), v327_parts, v325_at0, rt_b(0) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v326_kind, rt_s(&S49)), "||") || rt_bool(rt_eq(v326_kind, rt_s(&S50)), "||")))) {
    RT_RET(({ wv t458 = rt_eq(v326_kind, rt_s(&S49)); wv t459 = v325_at0; rt_rec(0, 4, K24, (wv[]){ rt_s(&S279), t458, t459, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S51)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S51), v325_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S52)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S52), v325_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S53)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S53), v325_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S137)))) {
    RT_RET(({ wv t460 = rt_get(&F_v, v324_tk); wv t461 = v325_at0; rt_rec(0, 4, K28, (wv[]){ rt_s(&S212), t460, t461, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S156)))) {
    RT_RET(({ wv t462 = rt_get(&F_v, v324_tk); wv t463 = v325_at0; rt_rec(0, 4, K28, (wv[]){ rt_s(&S156), t462, t463, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S97)))) {
    if (rt_cond(rt_chk(s191_at(up, rt_s(&S16)), "primary"))) {
      (void)rt_chk(w4_failAtHint(v324_tk, rt_s(&S280), rt_s(&S281)), "primary");
    }
    wv v330_e = rt_bind(rt_chk(s213_expr(up), "primary"), 1);
    if (rt_cond(rt_chk(s191_at(up, rt_s(&S96)), "primary"))) {
      wv v331_args = rt_arr(1, (wv[]){ v330_e }, 1);
      while (rt_cond(rt_ne(rt_chk(s195_accept(up, rt_s(&S96)), "primary"), RT_NULL))) {
        if (rt_cond(rt_chk(s191_at(up, rt_s(&S16)), "primary"))) {
          (void)rt_chk(w4_failAt(rt_chk(s190_peek(up), "primary"), rt_s(&S282)), "primary");
        }
        (void)({ wv t464 = v331_args; wv t465 = rt_chk(s213_expr(up), "primary"); std_push(t464, t465); });
      }
      (void)rt_chk(s196_expect(up, rt_s(&S16), rt_s(&S283), RT_NULL), "primary");
      if (rt_cond(rt_not(rt_chk(s193_atCall(up), "primary")))) {
        (void)rt_chk(w4_failAtHint(v324_tk, rt_s(&S284), rt_s(&S285)), "primary");
      }
      RT_RET(rt_chk(s221_call(up, v331_args), "primary"));
    }
    (void)rt_chk(s196_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "primary");
    rt_set_mem(rt_b(1), &F_paren, v330_e);
    RT_RET(v330_e);
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S98)))) {
    wv v332_items = rt_arr(0, NULL, 1);
    if (rt_cond(rt_chk(s191_at(up, rt_s(&S28)), "primary"))) {
      (void)rt_chk(s194_next(up), "primary");
      (void)rt_chk(s196_expect(up, rt_s(&S99), rt_s(&S286), rt_s(&S287)), "primary");
      RT_RET(({ wv t466 = rt_arr(0, NULL, 0); wv t467 = v325_at0; rt_rec(0, 4, K32, (wv[]){ rt_s(&S187), t466, t467, rt_b(0) }); }));
    }
    if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S99)), "primary")))) {
      (void)({ wv t468 = v332_items; wv t469 = rt_chk(s213_expr(up), "primary"); std_push(t468, t469); });
      if (rt_cond(rt_chk(s191_at(up, rt_s(&S28)), "primary"))) {
        RT_RET(({ wv t470 = v324_tk; wv t471 = rt_idx(rt_i(0), v332_items); rt_chk(s225_mapLiteral(up, t470, t471), "primary"); }));
      }
      while (rt_cond(rt_chk(s191_at(up, rt_s(&S96)), "primary"))) {
        wv v333_comma = rt_bind(rt_chk(s194_next(up), "primary"), 0);
        if (rt_cond(rt_chk(s191_at(up, rt_s(&S99)), "primary"))) {
          (void)rt_chk(w4_failAt(v333_comma, rt_s(&S288)), "primary");
        }
        (void)({ wv t472 = v332_items; wv t473 = rt_chk(s213_expr(up), "primary"); std_push(t472, t473); });
      }
    }
    (void)rt_chk(s196_expect(up, rt_s(&S99), rt_s(&S289), RT_NULL), "primary");
    RT_RET(rt_rec(0, 4, K32, (wv[]){ rt_s(&S186), v332_items, v325_at0, rt_b(0) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v326_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v326_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v326_kind, rt_s(&S37)), "||")), "||") || rt_bool(rt_eq(v326_kind, rt_s(&S38)), "||")))) {
    (void)({ wv t474 = v324_tk; wv t475 = rt_cat(3, (wv[]){ rt_s(&S290), rt_text(v326_kind), rt_s(&S113) }); wv t476 = rt_cat(3, (wv[]){ rt_s(&S291), rt_text(v326_kind), rt_s(&S292) }); rt_chk(w4_failAtHint(t474, t475, t476), "primary"); });
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S55)))) {
    (void)rt_chk(w4_failAtHint(v324_tk, rt_s(&S293), rt_s(&S294)), "primary");
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S111)))) {
    (void)({ wv t477 = v324_tk; wv t478 = rt_cat(3, (wv[]){ rt_s(&S290), rt_text(rt_get(&F_v, v324_tk)), rt_s(&S181) }); wv t479 = rt_cat(3, (wv[]){ rt_s(&S295), rt_text(rt_get(&F_v, v324_tk)), rt_s(&S296) }); rt_chk(w4_failAtHint(t477, t478, t479), "primary"); });
  }
  if (rt_cond(rt_eq(v326_kind, rt_s(&S100)))) {
    RT_RET(rt_chk(s223_record(up, v324_tk), "primary"));
  }
  (void)({ wv t480 = v324_tk; wv t481 = rt_cat(2, (wv[]){ rt_s(&S297), rt_text(rt_chk(w4_describe(v324_tk), "primary")) }); rt_chk(w4_failAt(t480, t481), "primary"); });
  RT_RET(RT_NULL);
}

static wv s223_record(struct fr49 *up, wv v334_open) {
  rt_enter("record");
  v334_open = rt_bind(v334_open, 0);
  wv v335_fields = rt_arr(0, NULL, 1);
  wv v336_seen = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S15)), "record")))) {
    while (rt_cond(rt_b(1))) {
      wv v337_key = rt_bind(rt_chk(s190_peek(up), "record"), 0);
      if (rt_cond(rt_ne(rt_get(&F_t, v337_key), rt_s(&S137)))) {
        wv v339_message = rt_bind((rt_cond(({ wv t482 = w3_keywords; wv t483 = rt_get(&F_t, v337_key); rt_chk(w1_contains(t482, t483), "record"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v337_key)), rt_s(&S277) }) : rt_cat(2, (wv[]){ rt_s(&S298), rt_text(rt_chk(w4_describe(v337_key), "record")) })), 0);
        (void)rt_chk(w4_failAtHint(v337_key, v339_message, rt_s(&S299)), "record");
      }
      (void)rt_chk(s194_next(up), "record");
      if (rt_cond(({ wv t484 = v336_seen; wv t485 = rt_get(&F_v, v337_key); rt_chk(w1_contains(t484, t485), "record"); }))) {
        (void)({ wv t486 = v337_key; wv t487 = rt_cat(3, (wv[]){ rt_s(&S300), rt_text(rt_get(&F_v, v337_key)), rt_s(&S301) }); rt_chk(w4_failAt(t486, t487), "record"); });
      }
      (void)({ wv t488 = v336_seen; wv t489 = rt_get(&F_v, v337_key); std_push(t488, t489); });
      if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S28)), "record")))) {
        wv v340_found = rt_bind(rt_chk(s190_peek(up), "record"), 0);
        wv v341_hint = rt_bind((rt_cond(rt_b(rt_bool(rt_chk(s191_at(up, rt_s(&S96)), "record"), "||") || rt_bool(rt_chk(s191_at(up, rt_s(&S15)), "record"), "||"))) ? ({ wv t490 = rt_text(rt_get(&F_v, v337_key)); wv t491 = rt_text(rt_get(&F_v, v337_key)); rt_cat(4, (wv[]){ rt_s(&S302), t490, rt_s(&S29), t491 }); }) : RT_NULL), 0);
        (void)({ wv t494 = v340_found; wv t495 = ({ wv t492 = rt_text(rt_get(&F_v, v337_key)); wv t493 = rt_text(rt_chk(w4_describe(v340_found), "record")); rt_cat(4, (wv[]){ rt_s(&S303), t492, rt_s(&S196), t493 }); }); wv t496 = v341_hint; rt_chk(w4_failAtHint(t494, t495, t496), "record"); });
      }
      (void)rt_chk(s194_next(up), "record");
      (void)({ wv t500 = v335_fields; wv t501 = ({ wv t497 = rt_get(&F_v, v337_key); wv t498 = rt_chk(s213_expr(up), "record"); wv t499 = rt_get(&F_pos, v337_key); rt_rec(0, 3, K33, (wv[]){ t497, t498, t499 }); }); std_push(t500, t501); });
      if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S96)), "record")))) {
        break;
      }
      wv v338_comma = rt_bind(rt_chk(s194_next(up), "record"), 0);
      if (rt_cond(rt_chk(s191_at(up, rt_s(&S15)), "record"))) {
        (void)rt_chk(w4_failAt(v338_comma, rt_s(&S304)), "record");
      }
    }
  }
  (void)rt_chk(s196_expect(up, rt_s(&S15), rt_s(&S305), RT_NULL), "record");
  RT_RET(({ wv t502 = v335_fields; wv t503 = rt_get(&F_pos, v334_open); rt_rec(0, 4, K34, (wv[]){ rt_s(&S188), t502, t503, rt_b(0) }); }));
}

static wv s224_constantKey(struct fr49 *up, wv v342_e) {
  rt_enter("constantKey");
  v342_e = rt_bind(v342_e, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v342_e), rt_s(&S138)))) {
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S306), rt_text(rt_get(&F_v, v342_e)) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v342_e), rt_s(&S279)))) {
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S140), rt_text(rt_get(&F_v, v342_e)) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v342_e), rt_s(&S171)), "&&") && rt_bool(rt_le(std_length(rt_get(&F_parts, v342_e)), rt_i(1)), "&&")), "&&") && rt_bool(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v342_e)), rt_i(0)), "||") || rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v342_e))), rt_s(&S185)), "||")), "&&")))) {
    RT_RET(rt_add(rt_s(&S307), (rt_cond(rt_eq(std_length(rt_get(&F_parts, v342_e)), rt_i(0))) ? rt_s(&S13) : rt_idx(rt_i(0), rt_get(&F_parts, v342_e)))));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v342_e), rt_s(&S148)), "||") || rt_bool(rt_eq(rt_get(&F_k, v342_e), rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v342_e), rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v342_e), rt_s(&S51)), "||")))) {
    (void)rt_chk(w2_failHint(rt_get(&F_pos, v342_e), rt_s(&S308), RT_NULL), "constantKey");
  }
  RT_RET(RT_NULL);
}

static wv s225_mapLiteral(struct fr49 *up, wv v343_open, wv v344_first) {
  rt_enter("mapLiteral");
  v343_open = rt_bind(v343_open, 0);
  v344_first = rt_bind(v344_first, 0);
  wv v345_items = rt_arr(0, NULL, 1);
  wv v346_seen = rt_arr(0, NULL, 1);
  wv v347_key = rt_bind(v344_first, 1);
  while (rt_cond(rt_b(1))) {
    wv v348_c = rt_bind(rt_chk(s224_constantKey(up, v347_key), "mapLiteral"), 0);
    if (rt_cond(rt_ne(v348_c, RT_NULL))) {
      if (rt_cond(rt_chk(w1_contains(v346_seen, v348_c), "mapLiteral"))) {
        (void)rt_chk(w2_failHint(rt_get(&F_pos, v347_key), rt_s(&S309), RT_NULL), "mapLiteral");
      }
      (void)std_push(v346_seen, v348_c);
    }
    (void)rt_chk(s196_expect(up, rt_s(&S28), rt_s(&S310), rt_s(&S311)), "mapLiteral");
    (void)std_push(v345_items, v347_key);
    (void)({ wv t504 = v345_items; wv t505 = rt_chk(s213_expr(up), "mapLiteral"); std_push(t504, t505); });
    if (rt_cond(rt_not(rt_chk(s191_at(up, rt_s(&S96)), "mapLiteral")))) {
      break;
    }
    wv v349_comma = rt_bind(rt_chk(s194_next(up), "mapLiteral"), 0);
    if (rt_cond(rt_chk(s191_at(up, rt_s(&S99)), "mapLiteral"))) {
      (void)rt_chk(w4_failAt(v349_comma, rt_s(&S312)), "mapLiteral");
    }
    v347_key = rt_bind(rt_chk(s213_expr(up), "mapLiteral"), 1);
  }
  (void)rt_chk(s196_expect(up, rt_s(&S99), rt_s(&S313), RT_NULL), "mapLiteral");
  RT_RET(({ wv t506 = v345_items; wv t507 = rt_get(&F_pos, v343_open); rt_rec(0, 4, K32, (wv[]){ rt_s(&S187), t506, t507, rt_b(0) }); }));
}

static wv s226_parseMatch(struct fr49 *up) {
  rt_enter("parseMatch");
  wv v350_kw = rt_bind(rt_chk(s194_next(up), "parseMatch"), 0);
  wv v351_subject = rt_bind(rt_chk(s210_cond(up, rt_s(&S44)), "parseMatch"), 0);
  (void)rt_chk(s196_expect(up, rt_s(&S100), rt_s(&S314), RT_NULL), "parseMatch");
  wv v352_cases = rt_arr(0, NULL, 1);
  wv v353_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s191_at(up, rt_s(&S45)), "parseMatch"))) {
      wv v354_a = rt_bind(rt_chk(s194_next(up), "parseMatch"), 0);
      if (rt_cond(rt_ne(v353_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v354_a, rt_s(&S315)), "parseMatch");
      }
      if (rt_cond(rt_chk(s191_at(up, rt_s(&S100)), "parseMatch"))) {
        (void)rt_chk(w4_failAtHint(rt_chk(s190_peek(up), "parseMatch"), rt_s(&S316), rt_s(&S317)), "parseMatch");
      }
      wv v355_values = rt_arr(0, NULL, 1);
      (void)({ wv t508 = v355_values; wv t509 = rt_chk(s227_caseValue(up), "parseMatch"); std_push(t508, t509); });
      while (rt_cond(rt_ne(rt_chk(s195_accept(up, rt_s(&S96)), "parseMatch"), RT_NULL))) {
        (void)({ wv t510 = v355_values; wv t511 = rt_chk(s227_caseValue(up), "parseMatch"); std_push(t510, t511); });
      }
      (void)({ wv t515 = v352_cases; wv t516 = ({ wv t512 = v355_values; wv t513 = rt_chk(s211_block(up), "parseMatch"); wv t514 = rt_get(&F_pos, v354_a); rt_rec(0, 3, K35, (wv[]){ t512, t513, t514 }); }); std_push(t515, t516); });
    } else if (rt_cond(rt_chk(s191_at(up, rt_s(&S40)), "parseMatch"))) {
      wv v356_a = rt_bind(rt_chk(s194_next(up), "parseMatch"), 0);
      if (rt_cond(rt_ne(v353_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v356_a, rt_s(&S318)), "parseMatch");
      }
      if (rt_cond(rt_eq(std_length(v352_cases), rt_i(0)))) {
        (void)rt_chk(w4_failAt(v356_a, rt_s(&S319)), "parseMatch");
      }
      v353_els = rt_bind(rt_chk(s211_block(up), "parseMatch"), 1);
    } else {
      break;
    }
  }
  if (rt_cond(rt_eq(std_length(v352_cases), rt_i(0)))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s190_peek(up), "parseMatch"), rt_s(&S320), rt_s(&S321)), "parseMatch");
  }
  (void)rt_chk(s196_expect(up, rt_s(&S15), rt_s(&S322), RT_NULL), "parseMatch");
  RT_RET(({ wv t517 = v351_subject; wv t518 = v352_cases; wv t519 = v353_els; wv t520 = rt_get(&F_pos, v350_kw); rt_rec(0, 6, K36, (wv[]){ rt_s(&S44), t517, t518, t519, t520, rt_b(0) }); }));
}

static wv s227_caseValue(struct fr49 *up) {
  rt_enter("caseValue");
  wv v357_tk = rt_bind(rt_chk(s190_peek(up), "caseValue"), 0);
  wv v358_v = rt_bind(rt_chk(s218_unary(up), "caseValue"), 0);
  wv v359_kind = rt_bind(rt_get(&F_k, v358_v), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v359_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v359_kind, rt_s(&S279)), "||")), "||") || rt_bool(rt_eq(v359_kind, rt_s(&S51)), "||")))) {
    RT_RET(v358_v);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v359_kind, rt_s(&S190)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v358_v)), rt_s(&S212)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v358_v)), rt_s(&S190)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, rt_get(&F_rec, v358_v))), rt_s(&S212)), "&&")), "||")), "&&")))) {
    RT_RET(v358_v);
  }
  if (rt_cond(rt_eq(v359_kind, rt_s(&S171)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v358_v)), rt_i(1)), "&&") && rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v358_v))), rt_s(&S185)), "&&")))) {
      RT_RET(v358_v);
    }
    (void)rt_chk(w4_failAt(v357_tk, rt_s(&S323)), "caseValue");
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v359_kind, rt_s(&S148)), "||") || rt_bool(rt_eq(v359_kind, rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(v359_kind, rt_s(&S53)), "||")))) {
    (void)rt_chk(w4_failAtHint(v357_tk, rt_s(&S324), rt_s(&S325)), "caseValue");
  }
  (void)rt_chk(w4_failAtHint(v357_tk, rt_s(&S326), rt_s(&S327)), "caseValue");
  RT_RET(RT_NULL);
}

static wv s228_chain(struct fr49 *up) {
  rt_enter("chain");
  wv v360_branches = rt_arr(0, NULL, 1);
  wv v361_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    wv v362_kw = rt_bind(rt_chk(s194_next(up), "chain"), 0);
    wv v363_c = rt_bind(rt_chk(s210_cond(up, rt_get(&F_t, v362_kw)), "chain"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v362_kw), rt_s(&S175)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v363_c), rt_s(&S192)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v363_c), rt_s(&S77)), "&&")))) {
      (void)({ wv t521 = w2_warnings; wv t522 = rt_chk(w2_warn(rt_get(&F_pos, v362_kw), rt_s(&S328), rt_s(&S329)), "chain"); std_push(t521, t522); });
    }
    (void)({ wv t527 = v360_branches; wv t528 = ({ wv t523 = rt_eq(rt_get(&F_t, v362_kw), rt_s(&S175)); wv t524 = v363_c; wv t525 = rt_chk(s211_block(up), "chain"); wv t526 = rt_get(&F_pos, v362_kw); rt_rec(0, 4, K37, (wv[]){ t523, t524, t525, t526 }); }); std_push(t527, t528); });
    if (rt_cond(rt_eq(rt_chk(s195_accept(up, rt_s(&S40)), "chain"), RT_NULL))) {
      break;
    }
    if (rt_cond(rt_b(rt_bool(rt_chk(s191_at(up, rt_s(&S39)), "chain"), "||") || rt_bool(rt_chk(s191_at(up, rt_s(&S175)), "chain"), "||")))) {
      continue;
    }
    v361_els = rt_bind(rt_chk(s211_block(up), "chain"), 1);
    break;
  }
  RT_RET(({ wv t529 = v360_branches; wv t530 = v361_els; wv t531 = rt_get(&F_pos, rt_idx(rt_i(0), v360_branches)); rt_rec(0, 5, K38, (wv[]){ rt_s(&S184), t529, t530, t531, rt_b(0) }); }));
}

static wv w5_stdSig(wv v366_name) {
  rt_enter("stdSig");
  v366_name = rt_bind(v366_name, 0);
  wv v367_k = rt_i(0);
  while (rt_cond(({ wv t582 = v367_k; wv t583 = std_length(rt_def(w5_stdlib, "stdlib")); rt_lt(t582, t583); }))) {
    if (rt_cond(({ wv t586 = rt_get(&F_name, ({ wv t584 = v367_k; wv t585 = rt_def(w5_stdlib, "stdlib"); rt_idx(t584, t585); })); wv t587 = v366_name; rt_eq(t586, t587); }))) {
      RT_RET(rt_get(&F_arity, ({ wv t588 = v367_k; wv t589 = rt_def(w5_stdlib, "stdlib"); rt_idx(t588, t589); })));
    }
    v367_k = rt_add(v367_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w5_modeOf(wv v368_x) {
  rt_enter("modeOf");
  v368_x = rt_bind(v368_x, 0);
  RT_RET((rt_cond(rt_get(&F_isVar, v368_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v368_x)) ? rt_s(&S37) : rt_s(&S13))));
}

static wv w5_kindText(wv v369_kind) {
  rt_enter("kindText");
  v369_kind = rt_bind(v369_kind, 0);
  RT_RET((rt_cond(rt_eq(v369_kind, rt_s(&S33))) ? rt_s(&S380) : (rt_cond(rt_eq(v369_kind, rt_s(&S46))) ? rt_s(&S381) : rt_cat(2, (wv[]){ rt_s(&S0), rt_text(v369_kind) }))));
}

static wv w5_plural(wv v370_n) {
  rt_enter("plural");
  v370_n = rt_bind(v370_n, 0);
  RT_RET(({ wv t590 = rt_cat(2, (wv[]){ rt_text(v370_n), rt_s(&S382) }); wv t591 = (rt_cond(rt_eq(v370_n, rt_i(1))) ? rt_s(&S13) : rt_s(&S307)); rt_add(t590, t591); }));
}

static wv w5_enumText(wv v371_x) {
  rt_enter("enumText");
  v371_x = rt_bind(v371_x, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v371_x), rt_s(&S212))) ? rt_get(&F_name, v371_x) : ({ wv t592 = rt_text(rt_get(&F_name, rt_get(&F_rec, v371_x))); wv t593 = rt_text(rt_get(&F_field, v371_x)); rt_cat(3, (wv[]){ t592, rt_s(&S129), t593 }); })));
}

static wv w5_caseKey(wv v372_v) {
  rt_enter("caseKey");
  v372_v = rt_bind(v372_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v372_v), rt_s(&S171)))) {
    RT_RET(rt_add(rt_s(&S383), rt_idx(rt_i(0), rt_get(&F_parts, v372_v))));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v372_v), rt_s(&S190)))) {
    RT_RET(({ wv t594 = rt_text(rt_get(&F_field, v372_v)); wv t595 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v372_v)), "caseKey")); rt_cat(4, (wv[]){ rt_s(&S384), t594, rt_s(&S129), t595 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v372_v), rt_s(&S51)))) {
    RT_RET(rt_s(&S51));
  }
  RT_RET(({ wv t596 = rt_text(rt_get(&F_k, v372_v)); wv t597 = rt_text(rt_get(&F_v, v372_v)); rt_cat(3, (wv[]){ t596, rt_s(&S28), t597 }); }));
}

static wv w5_caseText(wv v373_v) {
  rt_enter("caseText");
  v373_v = rt_bind(v373_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v373_v), rt_s(&S171)))) {
    RT_RET(rt_chk(w1_quote(rt_idx(rt_i(0), rt_get(&F_parts, v373_v))), "caseText"));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v373_v), rt_s(&S190)))) {
    RT_RET(({ wv t598 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v373_v)), "caseText")); wv t599 = rt_text(rt_get(&F_field, v373_v)); rt_cat(3, (wv[]){ t598, rt_s(&S129), t599 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v373_v), rt_s(&S51)))) {
    RT_RET(rt_s(&S51));
  }
  RT_RET(rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v373_v)) }));
}

static wv w5_getsAwayWithValue(wv v374_list) {
  rt_enter("getsAwayWithValue");
  wv v375_i = rt_i(0);
  while (rt_cond(({ wv t600 = v375_i; wv t601 = std_length(v374_list); rt_lt(t600, t601); }))) {
    wv v376_s = rt_bind(rt_idx(v375_i, v374_list), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_handler, v376_s), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v376_s)))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v376_s), rt_s(&S34)), "&&") && rt_bool(rt_ne(rt_get(&F_e, v376_s), RT_NULL), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v376_s), rt_s(&S41)), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v376_s))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v376_s), rt_s(&S176)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v376_s)), rt_s(&S184)), "&&")))) {
      wv v377_b = rt_i(0);
      while (rt_cond(({ wv t602 = v377_b; wv t603 = std_length(rt_get(&F_branches, rt_get(&F_e, v376_s))); rt_lt(t602, t603); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t604 = v377_b; wv t605 = rt_get(&F_branches, rt_get(&F_e, v376_s)); rt_idx(t604, t605); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v377_b = rt_add(v377_b, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v376_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v376_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v376_s), rt_s(&S176)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v376_s)), rt_s(&S44)), "&&")))) {
      wv v378_a = rt_i(0);
      while (rt_cond(({ wv t606 = v378_a; wv t607 = std_length(rt_get(&F_cases, rt_get(&F_e, v376_s))); rt_lt(t606, t607); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t608 = v378_a; wv t609 = rt_get(&F_cases, rt_get(&F_e, v376_s)); rt_idx(t608, t609); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v378_a = rt_add(v378_a, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v376_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v376_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    v375_i = rt_add(v375_i, rt_i(1));
  }
  RT_RET(rt_b(0));
}

static wv w5_lineIn(wv v379_chars, wv v380_pos) {
  rt_enter("lineIn");
  v379_chars = rt_bind(v379_chars, 0);
  v380_pos = rt_bind(v380_pos, 0);
  wv v381_line = rt_i(1);
  wv v382_k = rt_i(0);
  while (rt_cond(rt_lt(v382_k, v380_pos))) {
    if (rt_cond(rt_eq(rt_idx(v382_k, v379_chars), rt_s(&S2)))) {
      v381_line = rt_add(v381_line, rt_i(1));
    }
    v382_k = rt_add(v382_k, rt_i(1));
  }
  RT_RET(v381_line);
}

static wv w5_lineOf(wv v383_pos) {
  rt_enter("lineOf");
  v383_pos = rt_bind(v383_pos, 0);
  RT_RET(rt_chk(w5_lineIn(w2_srcChars, v383_pos), "lineOf"));
}

static wv w5_check(wv p_v384_modules, wv v385_requireExit) {
  rt_enter("check");
  struct fr100 fr = { 0 };
  fr.v384_modules = rt_bind(p_v384_modules, 0);
  v385_requireExit = rt_bind(v385_requireExit, 0);
  fr.v386_errors = rt_arr(0, NULL, 1);
  fr.v387_nextFn = rt_i(1);
  fr.v388_curMod = rt_i(0);
  fr.v389_functions = rt_arr(0, NULL, 1);
  fr.v390_uses = rt_arr(0, NULL, 1);
  fr.v391_enums = rt_arr(0, NULL, 1);
  fr.v424_topFn = rt_rec(0, 2, K44, (wv[]){ rt_i(0), rt_b(1) });
  wv v425_entry = rt_sub(std_length(fr.v384_modules), rt_i(1));
  wv v426_mi = rt_i(0);
  while (rt_cond(({ wv t1135 = v426_mi; wv t1136 = std_length(fr.v384_modules); rt_lt(t1135, t1136); }))) {
    (void)({ wv t1137 = v426_mi; wv t1138 = rt_eq(v426_mi, v425_entry); rt_chk(s423_checkModule(&fr, t1137, t1138), "check"); });
    v426_mi = rt_add(v426_mi, rt_i(1));
  }
  wv v427_si = rt_i(0);
  while (rt_cond(({ wv t1139 = v427_si; wv t1140 = std_length(fr.v389_functions); rt_lt(t1139, t1140); }))) {
    wv v617_sc = rt_bind(rt_idx(v427_si, fr.v389_functions), 0);
    wv v618_use = rt_bind(({ wv t1141 = rt_get(&F_mod, v617_sc); wv t1142 = rt_get(&F_namePos, v617_sc); rt_chk(s395_firstUse(&fr, t1141, t1142), "check"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v618_use, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v617_sc)), RT_NULL), "&&")), "&&") && rt_bool(rt_not(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v617_sc))), "check")), "&&")))) {
      (void)rt_chk(s393_enter(&fr, rt_get(&F_mod, v617_sc)), "check");
      wv v619_um = rt_bind(rt_get(&F_useMod, v618_use), 0);
      wv v620_where = rt_bind((rt_cond(({ wv t1143 = v619_um; wv t1144 = rt_get(&F_mod, v617_sc); rt_eq(t1143, t1144); })) ? rt_cat(2, (wv[]){ rt_s(&S472), rt_text(rt_chk(w5_lineOf(rt_get(&F_pos, v618_use)), "check")) }) : ({ wv t1147 = rt_text(rt_get(&F_file, rt_idx(v619_um, fr.v384_modules))); wv t1148 = rt_text(({ wv t1145 = rt_get(&F_chars, rt_idx(v619_um, fr.v384_modules)); wv t1146 = rt_get(&F_pos, v618_use); rt_chk(w5_lineIn(t1145, t1146), "check"); })); rt_cat(3, (wv[]){ t1147, rt_s(&S473), t1148 }); })), 0);
      (void)({ wv t1151 = rt_get(&F_namePos, v617_sc); wv t1152 = ({ wv t1149 = rt_text(rt_get(&F_name, v617_sc)); wv t1150 = rt_text(v620_where); rt_cat(5, (wv[]){ rt_s(&S113), t1149, rt_s(&S474), t1150, rt_s(&S16) }); }); rt_chk(s392_error(&fr, t1151, t1152, rt_s(&S475)), "check"); });
    }
    v427_si = rt_add(v427_si, rt_i(1));
  }
  (void)rt_chk(s393_enter(&fr, v425_entry), "check");
  wv v428_program = rt_bind(rt_get(&F_program, rt_idx(v425_entry, fr.v384_modules)), 0);
  if (rt_cond(v385_requireExit)) {
    wv v621_last = RT_NULL;
    wv v622_k = rt_i(0);
    while (rt_cond(({ wv t1153 = v622_k; wv t1154 = std_length(rt_get(&F_stmts, v428_program)); rt_lt(t1153, t1154); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1155 = v622_k; wv t1156 = rt_get(&F_stmts, v428_program); rt_idx(t1155, t1156); })), rt_s(&S201)))) {
        v621_last = rt_bind(({ wv t1157 = v622_k; wv t1158 = rt_get(&F_stmts, v428_program); rt_idx(t1157, t1158); }), 1);
      }
      v622_k = rt_add(v622_k, rt_i(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(v621_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v621_last), rt_s(&S54)), "||")))) {
      (void)rt_chk(s392_error(&fr, rt_get(&F_end, v428_program), rt_s(&S476), rt_s(&S477)), "check");
    }
  }
  RT_RET(fr.v386_errors);
}

static wv s392_error(struct fr100 *up, wv v429_pos, wv v430_message, wv v431_hint) {
  rt_enter("error");
  v429_pos = rt_bind(v429_pos, 0);
  v430_message = rt_bind(v430_message, 0);
  v431_hint = rt_bind(v431_hint, 0);
  (void)({ wv t610 = rt_def(up->v386_errors, "errors"); wv t611 = rt_chk(w2_err(v429_pos, v430_message, v431_hint), "error"); std_push(t610, t611); });
  RT_RET(RT_NULL);
}

static wv s393_enter(struct fr100 *up, wv v432_m) {
  rt_enter("enter");
  v432_m = rt_bind(v432_m, 0);
  wv t612 = rt_bind(v432_m, 1);
  (void)rt_def(up->v388_curMod, "curMod");
  up->v388_curMod = t612;
  (void)({ wv t613 = rt_get(&F_file, rt_idx(v432_m, up->v384_modules)); wv t614 = rt_get(&F_chars, rt_idx(v432_m, up->v384_modules)); rt_chk(w2_setSource(t613, t614), "enter"); });
  RT_RET(RT_NULL);
}

static wv s394_enumMembers(struct fr100 *up, wv v433_d) {
  rt_enter("enumMembers");
  v433_d = rt_bind(v433_d, 0);
  wv v434_k = rt_i(0);
  while (rt_cond(({ wv t615 = v434_k; wv t616 = std_length(rt_def(up->v391_enums, "enums")); rt_lt(t615, t616); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t619 = rt_get(&F_mod, ({ wv t617 = v434_k; wv t618 = rt_def(up->v391_enums, "enums"); rt_idx(t617, t618); })); wv t620 = rt_get(&F_mod, v433_d); rt_eq(t619, t620); }), "&&") && rt_bool(({ wv t623 = rt_get(&F_name, ({ wv t621 = v434_k; wv t622 = rt_def(up->v391_enums, "enums"); rt_idx(t621, t622); })); wv t624 = rt_get(&F_name, v433_d); rt_eq(t623, t624); }), "&&")))) {
      RT_RET(rt_get(&F_members, ({ wv t625 = v434_k; wv t626 = rt_def(up->v391_enums, "enums"); rt_idx(t625, t626); })));
    }
    v434_k = rt_add(v434_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s395_firstUse(struct fr100 *up, wv v435_mod, wv v436_declPos) {
  rt_enter("firstUse");
  v435_mod = rt_bind(v435_mod, 0);
  v436_declPos = rt_bind(v436_declPos, 0);
  wv v437_k = rt_i(0);
  while (rt_cond(({ wv t627 = v437_k; wv t628 = std_length(rt_def(up->v390_uses, "uses")); rt_lt(t627, t628); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t631 = rt_get(&F_mod, ({ wv t629 = v437_k; wv t630 = rt_def(up->v390_uses, "uses"); rt_idx(t629, t630); })); wv t632 = v435_mod; rt_eq(t631, t632); }), "&&") && rt_bool(({ wv t635 = rt_get(&F_namePos, ({ wv t633 = v437_k; wv t634 = rt_def(up->v390_uses, "uses"); rt_idx(t633, t634); })); wv t636 = v436_declPos; rt_eq(t635, t636); }), "&&")))) {
      RT_RET(({ wv t637 = v437_k; wv t638 = rt_def(up->v390_uses, "uses"); rt_idx(t637, t638); }));
    }
    v437_k = rt_add(v437_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s396_requireValue(struct fr100 *up, wv v438_b) {
  rt_enter("requireValue");
  v438_b = rt_bind(v438_b, 0);
  if (rt_cond(rt_ne(rt_get(&F_value, v438_b), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v439_last = RT_NULL;
  wv v440_k = rt_i(0);
  while (rt_cond(({ wv t639 = v440_k; wv t640 = std_length(rt_get(&F_stmts, v438_b)); rt_lt(t639, t640); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t641 = v440_k; wv t642 = rt_get(&F_stmts, v438_b); rt_idx(t641, t642); })), rt_s(&S201)))) {
      v439_last = rt_bind(({ wv t643 = v440_k; wv t644 = rt_get(&F_stmts, v438_b); rt_idx(t643, t644); }), 1);
    }
    v440_k = rt_add(v440_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_ne(v439_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t645 = rt_def(w5_neverCompletes, "neverCompletes"); wv t646 = rt_get(&F_k, v439_last); rt_chk(w1_contains(t645, t646), "requireValue"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v439_last), rt_s(&S176)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v439_last)), rt_s(&S183)), "&&")), "||")), "&&")))) {
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s392_error(up, rt_get(&F_close, v438_b), rt_s(&S385), rt_s(&S386)), "requireValue");
  RT_RET(RT_NULL);
}

static wv s397_newScope(struct fr100 *up, wv v441_parent) {
  rt_enter("newScope");
  v441_parent = rt_bind(v441_parent, 0);
  wv v442_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K40, (wv[]){ v441_parent, v442_names }));
}

static wv s398_decl(struct fr100 *up, wv v443_kind, wv v444_name, wv v445_mode, wv v446_namePos, wv v447_visibleAt, wv v448_fnId, wv v449_arity) {
  rt_enter("decl");
  v443_kind = rt_bind(v443_kind, 0);
  v444_name = rt_bind(v444_name, 0);
  v445_mode = rt_bind(v445_mode, 0);
  v446_namePos = rt_bind(v446_namePos, 0);
  v447_visibleAt = rt_bind(v447_visibleAt, 0);
  v448_fnId = rt_bind(v448_fnId, 0);
  v449_arity = rt_bind(v449_arity, 0);
  RT_RET(({ wv t647 = v443_kind; wv t648 = v444_name; wv t649 = rt_eq(v445_mode, rt_s(&S36)); wv t650 = rt_eq(v445_mode, rt_s(&S37)); wv t651 = v446_namePos; wv t652 = v447_visibleAt; wv t653 = v448_fnId; wv t654 = v449_arity; wv t655 = rt_def(up->v388_curMod, "curMod"); rt_rec(0, 10, K41, (wv[]){ t647, t648, t649, t650, t651, t652, t653, t654, t655, RT_NULL }); }));
}

static wv s399_own(struct fr100 *up, wv v450_scope, wv v451_name) {
  rt_enter("own");
  v451_name = rt_bind(v451_name, 0);
  wv v452_k = rt_i(0);
  while (rt_cond(({ wv t656 = v452_k; wv t657 = std_length(rt_get(&F_names, v450_scope)); rt_lt(t656, t657); }))) {
    if (rt_cond(({ wv t660 = rt_get(&F_name, ({ wv t658 = v452_k; wv t659 = rt_get(&F_names, v450_scope); rt_idx(t658, t659); })); wv t661 = v451_name; rt_eq(t660, t661); }))) {
      RT_RET(({ wv t662 = v452_k; wv t663 = rt_get(&F_names, v450_scope); rt_idx(t662, t663); }));
    }
    v452_k = rt_add(v452_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s400_declare(struct fr100 *up, wv v453_scope, wv v454_name, wv v455_d) {
  rt_enter("declare");
  v454_name = rt_bind(v454_name, 0);
  v455_d = rt_bind(v455_d, 0);
  wv v456_dup = rt_bind(rt_chk(s399_own(up, v453_scope, v454_name), "declare"), 0);
  if (rt_cond(rt_ne(v456_dup, RT_NULL))) {
    (void)({ wv t666 = rt_get(&F_namePos, v455_d); wv t667 = ({ wv t664 = rt_text(v454_name); wv t665 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v456_dup)), "declare")); rt_cat(5, (wv[]){ rt_s(&S113), t664, rt_s(&S387), t665, rt_s(&S16) }); }); rt_chk(s392_error(up, t666, t667, RT_NULL), "declare"); });
    RT_RET(RT_NULL);
  }
  wv v457_s = rt_get(&F_parent, v453_scope);
  while (rt_cond(rt_ne(v457_s, RT_NULL))) {
    wv v458_outer = rt_bind(rt_chk(s399_own(up, v457_s, v454_name), "declare"), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v458_outer, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_kind, v458_outer), rt_s(&S177)), "||") || rt_bool(({ wv t668 = rt_get(&F_namePos, v458_outer); wv t669 = rt_get(&F_namePos, v455_d); rt_lt(t668, t669); }), "||")), "&&")))) {
      (void)({ wv t673 = rt_get(&F_namePos, v455_d); wv t674 = ({ wv t670 = rt_text(v454_name); wv t671 = rt_text(v454_name); wv t672 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v458_outer)), "declare")); rt_cat(7, (wv[]){ rt_s(&S113), t670, rt_s(&S388), t671, rt_s(&S389), t672, rt_s(&S16) }); }); rt_chk(s392_error(up, t673, t674, rt_s(&S390)), "declare"); });
      break;
    }
    v457_s = rt_get(&F_parent, v457_s);
  }
  (void)({ wv t675 = rt_get(&F_names, v453_scope); wv t676 = v455_d; std_push(t675, t676); });
  RT_RET(RT_NULL);
}

static wv s401_lookup(struct fr100 *up, wv v459_scope, wv v460_name) {
  rt_enter("lookup");
  v460_name = rt_bind(v460_name, 0);
  wv v461_s = v459_scope;
  while (rt_cond(rt_ne(v461_s, RT_NULL))) {
    wv v462_d = rt_bind(rt_chk(s399_own(up, v461_s, v460_name), "lookup"), 0);
    if (rt_cond(rt_ne(v462_d, RT_NULL))) {
      RT_RET(v462_d);
    }
    v461_s = rt_get(&F_parent, v461_s);
  }
  RT_RET(RT_NULL);
}

static wv s402_resolve(struct fr100 *up, wv v463_name, wv v464_pos, wv v465_scope, wv v466_ctx) {
  rt_enter("resolve");
  v463_name = rt_bind(v463_name, 0);
  v464_pos = rt_bind(v464_pos, 0);
  v465_scope = rt_bind(v465_scope, 0);
  v466_ctx = rt_bind(v466_ctx, 0);
  wv v467_d = rt_bind(rt_chk(s401_lookup(up, v465_scope, v463_name), "resolve"), 0);
  if (rt_cond(rt_eq(v467_d, RT_NULL))) {
    (void)({ wv t677 = v464_pos; wv t678 = rt_cat(3, (wv[]){ rt_s(&S391), rt_text(v463_name), rt_s(&S113) }); rt_chk(s392_error(up, t677, t678, RT_NULL), "resolve"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v467_d), rt_s(&S177)), "&&") && rt_bool(({ wv t679 = rt_get(&F_fnId, v467_d); wv t680 = rt_get(&F_id, rt_get(&F_func, v466_ctx)); rt_eq(t679, t680); }), "&&")), "&&") && rt_bool(({ wv t681 = v464_pos; wv t682 = rt_get(&F_visibleAt, v467_d); rt_lt(t681, t682); }), "&&")))) {
    (void)({ wv t685 = v464_pos; wv t686 = ({ wv t683 = rt_text(v463_name); wv t684 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v467_d)), "resolve")); rt_cat(5, (wv[]){ rt_s(&S113), t683, rt_s(&S392), t684, rt_s(&S16) }); }); rt_chk(s392_error(up, t685, t686, RT_NULL), "resolve"); });
  }
  RT_RET(v467_d);
}

static wv s403_block(struct fr100 *up, wv v468_b, wv v469_scope, wv v470_ctx) {
  rt_enter("block");
  v469_scope = rt_bind(v469_scope, 0);
  v470_ctx = rt_bind(v470_ctx, 0);
  wv v471_k = rt_i(0);
  while (rt_cond(({ wv t687 = v471_k; wv t688 = std_length(rt_get(&F_stmts, v468_b)); rt_lt(t687, t688); }))) {
    wv v472_s = rt_bind(({ wv t689 = v471_k; wv t690 = rt_get(&F_stmts, v468_b); rt_idx(t689, t690); }), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v472_s), rt_s(&S33)))) {
      wv v473_d = rt_bind(({ wv t691 = rt_get(&F_name, v472_s); wv t692 = rt_get(&F_namePos, v472_s); wv t693 = rt_get(&F_id, rt_get(&F_func, v470_ctx)); wv t694 = std_length(rt_get(&F_params, v472_s)); rt_chk(s398_decl(up, rt_s(&S33), t691, rt_s(&S13), t692, RT_NULL, t693, t694), "block"); }), 0);
      (void)({ wv t695 = v469_scope; wv t696 = rt_get(&F_name, v472_s); wv t697 = v473_d; rt_chk(s400_declare(up, t695, t696, t697), "block"); });
      if (rt_cond(({ wv t700 = ({ wv t698 = v469_scope; wv t699 = rt_get(&F_name, v472_s); rt_chk(s399_own(up, t698, t699), "block"); }); wv t701 = v473_d; rt_eq(t700, t701); }))) {
        (void)({ wv t706 = rt_def(up->v389_functions, "functions"); wv t707 = ({ wv t702 = rt_def(up->v388_curMod, "curMod"); wv t703 = rt_get(&F_name, v472_s); wv t704 = rt_get(&F_namePos, v472_s); wv t705 = rt_get(&F_body, v472_s); rt_rec(0, 4, K42, (wv[]){ t702, t703, t704, t705 }); }); std_push(t706, t707); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v472_s), rt_s(&S46)))) {
      wv v474_d = rt_bind(({ wv t708 = rt_get(&F_name, v472_s); wv t709 = rt_get(&F_namePos, v472_s); wv t710 = rt_get(&F_id, rt_get(&F_func, v470_ctx)); rt_chk(s398_decl(up, rt_s(&S46), t708, rt_s(&S13), t709, RT_NULL, t710, RT_NULL), "block"); }), 0);
      (void)({ wv t711 = v469_scope; wv t712 = rt_get(&F_name, v472_s); wv t713 = v474_d; rt_chk(s400_declare(up, t711, t712, t713), "block"); });
      if (rt_cond(({ wv t716 = ({ wv t714 = v469_scope; wv t715 = rt_get(&F_name, v472_s); rt_chk(s399_own(up, t714, t715), "block"); }); wv t717 = v474_d; rt_eq(t716, t717); }))) {
        (void)({ wv t721 = rt_def(up->v391_enums, "enums"); wv t722 = ({ wv t718 = rt_def(up->v388_curMod, "curMod"); wv t719 = rt_get(&F_name, v472_s); wv t720 = rt_get(&F_members, v472_s); rt_rec(0, 3, K43, (wv[]){ t718, t719, t720 }); }); std_push(t721, t722); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v472_s), rt_s(&S177)))) {
      (void)({ wv t728 = v469_scope; wv t729 = rt_get(&F_name, v472_s); wv t730 = ({ wv t723 = rt_get(&F_name, v472_s); wv t724 = rt_chk(w5_modeOf(v472_s), "block"); wv t725 = rt_get(&F_namePos, v472_s); wv t726 = rt_get(&F_end, v472_s); wv t727 = rt_get(&F_id, rt_get(&F_func, v470_ctx)); rt_chk(s398_decl(up, rt_s(&S177), t723, t724, t725, t726, t727, RT_NULL), "block"); }); rt_chk(s400_declare(up, t728, t729, t730), "block"); });
    }
    v471_k = rt_add(v471_k, rt_i(1));
  }
  v471_k = rt_i(0);
  while (rt_cond(({ wv t731 = v471_k; wv t732 = std_length(rt_get(&F_stmts, v468_b)); rt_lt(t731, t732); }))) {
    (void)({ wv t735 = ({ wv t733 = v471_k; wv t734 = rt_get(&F_stmts, v468_b); rt_idx(t733, t734); }); wv t736 = v469_scope; wv t737 = v470_ctx; rt_chk(s404_stmt(up, t735, t736, t737), "block"); });
    v471_k = rt_add(v471_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v468_b), RT_NULL))) {
    (void)({ wv t738 = rt_get(&F_value, v468_b); wv t739 = v469_scope; wv t740 = v470_ctx; rt_chk(s419_expr(up, t738, t739, t740), "block"); });
  }
  RT_RET(RT_NULL);
}

static wv s404_stmt(struct fr100 *up, wv v475_s, wv v476_scope, wv v477_ctx) {
  rt_enter("stmt");
  v476_scope = rt_bind(v476_scope, 0);
  v477_ctx = rt_bind(v477_ctx, 0);
  wv v478_kind = rt_bind(rt_get(&F_k, v475_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v478_kind, rt_s(&S201)), "||") || rt_bool(rt_eq(v478_kind, rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(v478_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v478_kind, rt_s(&S204)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v478_kind, rt_s(&S33)))) {
    wv v479_func = rt_rec(0, 2, K44, (wv[]){ rt_def(up->v387_nextFn, "nextFn"), rt_b(0) });
    up->v387_nextFn = rt_add(rt_def(up->v387_nextFn, "nextFn"), rt_i(1));
    wv v480_ps = rt_bind(rt_chk(s397_newScope(up, v476_scope), "stmt"), 0);
    wv v481_k = rt_i(0);
    while (rt_cond(({ wv t741 = v481_k; wv t742 = std_length(rt_get(&F_params, v475_s)); rt_lt(t741, t742); }))) {
      wv v482_p = rt_bind(({ wv t743 = v481_k; wv t744 = rt_get(&F_params, v475_s); rt_idx(t743, t744); }), 0);
      (void)({ wv t749 = v480_ps; wv t750 = rt_get(&F_name, v482_p); wv t751 = ({ wv t745 = rt_get(&F_name, v482_p); wv t746 = rt_chk(w5_modeOf(v482_p), "stmt"); wv t747 = rt_get(&F_pos, v482_p); wv t748 = rt_get(&F_id, v479_func); rt_chk(s398_decl(up, rt_s(&S393), t745, t746, t747, RT_NULL, t748, RT_NULL), "stmt"); }); rt_chk(s400_declare(up, t749, t750, t751), "stmt"); });
      v481_k = rt_add(v481_k, rt_i(1));
    }
    (void)({ wv t752 = rt_get(&F_body, v475_s); wv t753 = rt_chk(s397_newScope(up, v480_ps), "stmt"); wv t754 = rt_rec(0, 2, K45, (wv[]){ v479_func, rt_i(0) }); rt_chk(s403_block(up, t752, t753, t754), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v478_kind, rt_s(&S34)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v477_ctx)))) {
      (void)rt_chk(s392_error(up, rt_get(&F_pos, v475_s), rt_s(&S394), RT_NULL), "stmt");
    }
    if (rt_cond(rt_ne(rt_get(&F_e, v475_s), RT_NULL))) {
      (void)({ wv t755 = rt_get(&F_e, v475_s); wv t756 = v476_scope; wv t757 = v477_ctx; rt_chk(s419_expr(up, t755, t756, t757), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v478_kind, rt_s(&S57)))) {
    (void)({ wv t758 = rt_get(&F_e, v475_s); wv t759 = v476_scope; wv t760 = v477_ctx; rt_chk(s419_expr(up, t758, t759, t760), "stmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v478_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v478_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_get(&F_loops, v477_ctx), rt_i(0)))) {
      (void)({ wv t761 = rt_get(&F_pos, v475_s); wv t762 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text((rt_cond(rt_eq(v478_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43))), rt_s(&S395) }); rt_chk(s392_error(up, t761, t762, RT_NULL), "stmt"); });
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v478_kind, rt_s(&S41)))) {
    (void)({ wv t763 = rt_get(&F_cond, v475_s); wv t764 = v476_scope; wv t765 = v477_ctx; rt_chk(s419_expr(up, t763, t764, t765), "stmt"); });
    (void)({ wv t768 = rt_get(&F_body, v475_s); wv t769 = rt_chk(s397_newScope(up, v476_scope), "stmt"); wv t770 = ({ wv t766 = rt_get(&F_func, v477_ctx); wv t767 = rt_add(rt_get(&F_loops, v477_ctx), rt_i(1)); rt_rec(0, 2, K45, (wv[]){ t766, t767 }); }); rt_chk(s403_block(up, t768, t769, t770), "stmt"); });
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v478_kind, rt_s(&S55)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v477_ctx)))) {
      (void)rt_chk(s392_error(up, rt_get(&F_pos, v475_s), rt_s(&S396), RT_NULL), "stmt");
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v478_kind, rt_s(&S54)))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v475_s), RT_NULL))) {
      (void)({ wv t771 = rt_get(&F_e, v475_s); wv t772 = v476_scope; wv t773 = v477_ctx; rt_chk(s419_expr(up, t771, t772, t773), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v478_kind, rt_s(&S176)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v475_s)), rt_s(&S183)))) {
      if (rt_cond(rt_get(&F_top, rt_get(&F_func, v477_ctx)))) {
        (void)rt_chk(s392_error(up, rt_get(&F_pos, rt_get(&F_e, v475_s)), rt_s(&S396), RT_NULL), "stmt");
      }
      (void)({ wv t774 = rt_get(&F_arg, rt_get(&F_e, v475_s)); wv t775 = v476_scope; wv t776 = v477_ctx; rt_chk(s419_expr(up, t774, t775, t776), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v475_s)), rt_s(&S184)))) {
      (void)({ wv t777 = rt_get(&F_e, v475_s); wv t778 = v476_scope; wv t779 = v477_ctx; rt_chk(s415_chain(up, t777, t778, t779, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v475_s)), rt_s(&S44)))) {
      (void)({ wv t780 = rt_get(&F_e, v475_s); wv t781 = v476_scope; wv t782 = v477_ctx; rt_chk(s406_checkMatch(up, t780, t781, t782, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v475_s)), rt_s(&S182)))) {
      wv v483_k = rt_i(0);
      while (rt_cond(({ wv t783 = v483_k; wv t784 = std_length(rt_get(&F_args, rt_get(&F_e, v475_s))); rt_lt(t783, t784); }))) {
        (void)({ wv t787 = ({ wv t785 = v483_k; wv t786 = rt_get(&F_args, rt_get(&F_e, v475_s)); rt_idx(t785, t786); }); wv t788 = v476_scope; wv t789 = v477_ctx; rt_chk(s419_expr(up, t787, t788, t789), "stmt"); });
        v483_k = rt_add(v483_k, rt_i(1));
      }
      (void)({ wv t790 = rt_get(&F_e, v475_s); wv t791 = v476_scope; rt_chk(s420_callee(up, t790, t791, rt_b(0)), "stmt"); });
    } else {
      (void)({ wv t792 = rt_get(&F_e, v475_s); wv t793 = v476_scope; wv t794 = v477_ctx; rt_chk(s419_expr(up, t792, t793, t794), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v478_kind, rt_s(&S177)))) {
    (void)({ wv t795 = rt_get(&F_e, v475_s); wv t796 = v476_scope; wv t797 = v477_ctx; rt_chk(s419_expr(up, t795, t796, t797), "stmt"); });
  } else if (rt_cond(rt_eq(v478_kind, rt_s(&S38)))) {
    (void)({ wv t798 = rt_get(&F_e, v475_s); wv t799 = v476_scope; wv t800 = v477_ctx; rt_chk(s419_expr(up, t798, t799, t800), "stmt"); });
    (void)({ wv t801 = rt_get(&F_target, v475_s); wv t802 = v476_scope; wv t803 = v477_ctx; rt_chk(s416_lvalue(up, t801, t802, t803), "stmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v475_s), RT_NULL))) {
    (void)rt_chk(s405_onfailHandler(up, v475_s, v476_scope, v477_ctx), "stmt");
  }
  RT_RET(RT_NULL);
}

static wv s405_onfailHandler(struct fr100 *up, wv v484_s, wv v485_scope, wv v486_ctx) {
  rt_enter("onfailHandler");
  v484_s = rt_bind(v484_s, 0);
  v485_scope = rt_bind(v485_scope, 0);
  v486_ctx = rt_bind(v486_ctx, 0);
  wv v487_h = rt_bind(rt_get(&F_handler, v484_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_e, v484_s), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v484_s)), rt_s(&S182)), "||")))) {
    (void)rt_chk(s392_error(up, rt_get(&F_pos, v487_h), rt_s(&S397), rt_s(&S398)), "onfailHandler");
  }
  wv v488_hs = rt_bind(rt_chk(s397_newScope(up, v485_scope), "onfailHandler"), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v487_h), RT_NULL))) {
    (void)({ wv t807 = v488_hs; wv t808 = rt_get(&F_param, v487_h); wv t809 = ({ wv t804 = rt_get(&F_param, v487_h); wv t805 = rt_get(&F_paramPos, v487_h); wv t806 = rt_get(&F_id, rt_get(&F_func, v486_ctx)); rt_chk(s398_decl(up, rt_s(&S56), t804, rt_s(&S13), t805, RT_NULL, t806, RT_NULL), "onfailHandler"); }); rt_chk(s400_declare(up, t807, t808, t809), "onfailHandler"); });
  }
  (void)({ wv t810 = rt_get(&F_body, v487_h); wv t811 = rt_chk(s397_newScope(up, v488_hs), "onfailHandler"); wv t812 = v486_ctx; rt_chk(s403_block(up, t810, t811, t812), "onfailHandler"); });
  if (rt_cond(({ wv t813 = rt_def(w5_onfailValueUsed, "onfailValueUsed"); wv t814 = rt_get(&F_k, v484_s); rt_chk(w1_contains(t813, t814), "onfailHandler"); }))) {
    (void)rt_chk(s396_requireValue(up, rt_get(&F_body, v487_h)), "onfailHandler");
  }
  RT_RET(RT_NULL);
}

static wv s406_checkMatch(struct fr100 *up, wv v489_e, wv v490_scope, wv v491_ctx, wv v492_stmtLevel) {
  rt_enter("checkMatch");
  v489_e = rt_bind(v489_e, 0);
  v490_scope = rt_bind(v490_scope, 0);
  v491_ctx = rt_bind(v491_ctx, 0);
  v492_stmtLevel = rt_bind(v492_stmtLevel, 0);
  (void)({ wv t815 = rt_get(&F_subject, v489_e); wv t816 = v490_scope; wv t817 = v491_ctx; rt_chk(s419_expr(up, t815, t816, t817), "checkMatch"); });
  wv v493_seen = rt_arr(0, NULL, 1);
  wv v494_sort = RT_NULL;
  wv v495_sortEnum = RT_NULL;
  wv v496_a = rt_i(0);
  while (rt_cond(({ wv t818 = v496_a; wv t819 = std_length(rt_get(&F_cases, v489_e)); rt_lt(t818, t819); }))) {
    wv v497_al = rt_bind(({ wv t820 = v496_a; wv t821 = rt_get(&F_cases, v489_e); rt_idx(t820, t821); }), 0);
    wv v498_j = rt_i(0);
    while (rt_cond(({ wv t822 = v498_j; wv t823 = std_length(rt_get(&F_values, v497_al)); rt_lt(t822, t823); }))) {
      wv v499_v = rt_bind(({ wv t824 = v498_j; wv t825 = rt_get(&F_values, v497_al); rt_idx(t824, t825); }), 0);
      wv v500_vs = rt_s(&S13);
      if (rt_cond(rt_eq(rt_get(&F_k, v499_v), rt_s(&S190)))) {
        wv v504_d = rt_bind(rt_chk(s413_caseEnum(up, v499_v, v490_scope), "checkMatch"), 0);
        v500_vs = rt_bind((rt_cond(rt_ne(v504_d, RT_NULL)) ? rt_chk(w5_enumText(rt_get(&F_rec, v499_v)), "checkMatch") : RT_NULL), 1);
        if (rt_cond(rt_eq(v494_sort, RT_NULL))) {
          v495_sortEnum = rt_bind(v504_d, 1);
        }
      }
      if (rt_cond(rt_eq(v494_sort, RT_NULL))) {
        v494_sort = rt_bind(v500_vs, 1);
      } else if (rt_cond(rt_b(rt_bool(rt_ne(v500_vs, RT_NULL), "&&") && rt_bool(rt_ne(v500_vs, v494_sort), "&&")))) {
        wv v505_message = rt_bind((rt_cond(rt_b(rt_bool(rt_eq(v494_sort, rt_s(&S13)), "||") || rt_bool(rt_eq(v500_vs, rt_s(&S13)), "||"))) ? rt_s(&S399) : ({ wv t826 = rt_text(v494_sort); wv t827 = rt_text(v500_vs); rt_cat(5, (wv[]){ rt_s(&S400), t826, rt_s(&S401), t827, rt_s(&S113) }); })), 0);
        (void)({ wv t828 = rt_get(&F_pos, v499_v); wv t829 = v505_message; rt_chk(s392_error(up, t828, t829, RT_NULL), "checkMatch"); });
      }
      wv v501_key = rt_bind(rt_chk(w5_caseKey(v499_v), "checkMatch"), 0);
      wv v502_first = RT_NULL;
      wv v503_q = rt_i(0);
      while (rt_cond(({ wv t830 = v503_q; wv t831 = std_length(v493_seen); rt_lt(t830, t831); }))) {
        if (rt_cond(({ wv t832 = rt_get(&F_key, rt_idx(v503_q, v493_seen)); wv t833 = v501_key; rt_eq(t832, t833); }))) {
          v502_first = rt_bind(rt_get(&F_pos, rt_idx(v503_q, v493_seen)), 1);
        }
        v503_q = rt_add(v503_q, rt_i(1));
      }
      if (rt_cond(rt_ne(v502_first, RT_NULL))) {
        (void)({ wv t836 = rt_get(&F_pos, v499_v); wv t837 = ({ wv t834 = rt_text(rt_chk(w5_caseText(v499_v), "checkMatch")); wv t835 = rt_text(rt_chk(w5_lineOf(v502_first), "checkMatch")); rt_cat(5, (wv[]){ rt_s(&S402), t834, rt_s(&S403), t835, rt_s(&S16) }); }); rt_chk(s392_error(up, t836, t837, RT_NULL), "checkMatch"); });
      } else {
        (void)({ wv t840 = v493_seen; wv t841 = ({ wv t838 = v501_key; wv t839 = rt_get(&F_pos, v499_v); rt_rec(0, 2, K46, (wv[]){ t838, t839 }); }); std_push(t840, t841); });
      }
      v498_j = rt_add(v498_j, rt_i(1));
    }
    (void)({ wv t842 = rt_get(&F_body, v497_al); wv t843 = rt_chk(s397_newScope(up, v490_scope), "checkMatch"); wv t844 = v491_ctx; rt_chk(s403_block(up, t842, t843, t844), "checkMatch"); });
    if (rt_cond(rt_not(v492_stmtLevel))) {
      (void)rt_chk(s396_requireValue(up, rt_get(&F_body, v497_al)), "checkMatch");
    }
    v496_a = rt_add(v496_a, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v489_e), RT_NULL))) {
    (void)({ wv t845 = rt_get(&F_els, v489_e); wv t846 = rt_chk(s397_newScope(up, v490_scope), "checkMatch"); wv t847 = v491_ctx; rt_chk(s403_block(up, t845, t846, t847), "checkMatch"); });
    if (rt_cond(rt_not(v492_stmtLevel))) {
      (void)rt_chk(s396_requireValue(up, rt_get(&F_els, v489_e)), "checkMatch");
    }
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v494_sort, RT_NULL), "&&") && rt_bool(rt_ne(v494_sort, rt_s(&S13)), "&&")))) {
    wv v506_ms = rt_bind(rt_chk(s394_enumMembers(up, v495_sortEnum), "checkMatch"), 0);
    wv v507_missing = rt_arr(0, NULL, 1);
    wv v508_m = rt_i(0);
    while (rt_cond(({ wv t848 = v508_m; wv t849 = std_length(v506_ms); rt_lt(t848, t849); }))) {
      wv v509_key = ({ wv t850 = rt_text(rt_get(&F_name, rt_idx(v508_m, v506_ms))); wv t851 = rt_text(v494_sort); rt_cat(4, (wv[]){ rt_s(&S384), t850, rt_s(&S129), t851 }); });
      wv v510_handled = rt_b(0);
      wv v511_q = rt_i(0);
      while (rt_cond(({ wv t852 = v511_q; wv t853 = std_length(v493_seen); rt_lt(t852, t853); }))) {
        if (rt_cond(({ wv t854 = rt_get(&F_key, rt_idx(v511_q, v493_seen)); wv t855 = v509_key; rt_eq(t854, t855); }))) {
          v510_handled = rt_b(1);
        }
        v511_q = rt_add(v511_q, rt_i(1));
      }
      if (rt_cond(rt_not(v510_handled))) {
        (void)({ wv t858 = v507_missing; wv t859 = ({ wv t856 = rt_text(v494_sort); wv t857 = rt_text(rt_get(&F_name, rt_idx(v508_m, v506_ms))); rt_cat(5, (wv[]){ rt_s(&S113), t856, rt_s(&S129), t857, rt_s(&S113) }); }); std_push(t858, t859); });
      }
      v508_m = rt_add(v508_m, rt_i(1));
    }
    if (rt_cond(rt_gt(std_length(v507_missing), rt_i(0)))) {
      (void)({ wv t860 = rt_get(&F_pos, v489_e); wv t861 = rt_cat(2, (wv[]){ rt_s(&S404), rt_text(std_join(v507_missing, rt_s(&S405))) }); rt_chk(s392_error(up, t860, t861, rt_s(&S406)), "checkMatch"); });
    }
  } else if (rt_cond(rt_not(v492_stmtLevel))) {
    (void)rt_chk(s392_error(up, rt_get(&F_pos, v489_e), rt_s(&S407), RT_NULL), "checkMatch");
  }
  RT_RET(RT_NULL);
}

static wv s407_moduleNamed(struct fr100 *up, wv v512_x, wv v513_scope) {
  rt_enter("moduleNamed");
  v513_scope = rt_bind(v513_scope, 0);
  if (rt_cond(rt_ne(rt_get(&F_k, v512_x), rt_s(&S212)))) {
    RT_RET(RT_NULL);
  }
  wv v514_d = rt_bind(({ wv t862 = v513_scope; wv t863 = rt_get(&F_name, v512_x); rt_chk(s401_lookup(up, t862, t863), "moduleNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v514_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v514_d), rt_s(&S408)), "&&"))) ? v514_d : RT_NULL));
}

static wv s408_exported(struct fr100 *up, wv v515_lib, wv v516_name) {
  rt_enter("exported");
  v515_lib = rt_bind(v515_lib, 0);
  v516_name = rt_bind(v516_name, 0);
  wv v517_xs = rt_bind(rt_get(&F_exports, rt_idx(v515_lib, up->v384_modules)), 0);
  wv v518_k = rt_i(0);
  while (rt_cond(({ wv t864 = v518_k; wv t865 = std_length(v517_xs); rt_lt(t864, t865); }))) {
    if (rt_cond(({ wv t866 = rt_get(&F_name, rt_idx(v518_k, v517_xs)); wv t867 = v516_name; rt_eq(t866, t867); }))) {
      RT_RET(rt_idx(v518_k, v517_xs));
    }
    v518_k = rt_add(v518_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s409_enumNamed(struct fr100 *up, wv v519_x, wv v520_scope) {
  rt_enter("enumNamed");
  v520_scope = rt_bind(v520_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v519_x), rt_s(&S212)))) {
    wv v523_d = rt_bind(({ wv t868 = v520_scope; wv t869 = rt_get(&F_name, v519_x); rt_chk(s401_lookup(up, t868, t869), "enumNamed"); }), 0);
    RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v523_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v523_d), rt_s(&S46)), "&&"))) ? v523_d : RT_NULL));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v519_x), rt_s(&S190)))) {
    RT_RET(RT_NULL);
  }
  wv v521_m = rt_bind(({ wv t870 = rt_get(&F_rec, v519_x); wv t871 = v520_scope; rt_chk(s407_moduleNamed(up, t870, t871), "enumNamed"); }), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v521_m, RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_lib, v521_m), RT_NULL), "||")))) {
    RT_RET(RT_NULL);
  }
  wv v522_d = rt_bind(({ wv t872 = rt_get(&F_lib, v521_m); wv t873 = rt_get(&F_field, v519_x); rt_chk(s408_exported(up, t872, t873), "enumNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v522_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v522_d), rt_s(&S46)), "&&"))) ? v522_d : RT_NULL));
}

static wv s410_exportOf(struct fr100 *up, wv v524_e, wv v525_m) {
  rt_enter("exportOf");
  v524_e = rt_bind(v524_e, 0);
  v525_m = rt_bind(v525_m, 0);
  if (rt_cond(rt_eq(rt_get(&F_lib, v525_m), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v526_d = rt_bind(({ wv t874 = rt_get(&F_lib, v525_m); wv t875 = rt_get(&F_field, v524_e); rt_chk(s408_exported(up, t874, t875), "exportOf"); }), 0);
  if (rt_cond(rt_eq(v526_d, RT_NULL))) {
    (void)({ wv t880 = rt_get(&F_pos, v524_e); wv t881 = ({ wv t878 = rt_text(rt_get(&F_path, ({ wv t876 = rt_get(&F_lib, v525_m); wv t877 = up->v384_modules; rt_idx(t876, t877); }))); wv t879 = rt_text(rt_get(&F_field, v524_e)); rt_cat(5, (wv[]){ rt_s(&S113), t878, rt_s(&S409), t879, rt_s(&S113) }); }); rt_chk(s392_error(up, t880, t881, RT_NULL), "exportOf"); });
  }
  RT_RET(v526_d);
}

static wv s411_unknownModule(struct fr100 *up, wv v527_x, wv v528_scope) {
  rt_enter("unknownModule");
  v527_x = rt_bind(v527_x, 0);
  v528_scope = rt_bind(v528_scope, 0);
  wv v529_message = rt_bind((rt_cond(rt_ne(({ wv t882 = v528_scope; wv t883 = rt_get(&F_name, v527_x); rt_chk(s401_lookup(up, t882, t883), "unknownModule"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v527_x)), rt_s(&S410) }) : rt_cat(3, (wv[]){ rt_s(&S411), rt_text(rt_get(&F_name, v527_x)), rt_s(&S113) })), 0);
  (void)({ wv t884 = rt_get(&F_pos, v527_x); wv t885 = v529_message; rt_chk(s392_error(up, t884, t885, RT_NULL), "unknownModule"); });
  RT_RET(RT_NULL);
}

static wv s412_qualified(struct fr100 *up, wv v530_e, wv v531_m) {
  rt_enter("qualified");
  v530_e = rt_bind(v530_e, 0);
  v531_m = rt_bind(v531_m, 0);
  wv v532_d = rt_bind(rt_chk(s410_exportOf(up, v530_e, v531_m), "qualified"), 0);
  wv v533_label = ({ wv t886 = rt_text(rt_get(&F_name, rt_get(&F_rec, v530_e))); wv t887 = rt_text(rt_get(&F_field, v530_e)); rt_cat(3, (wv[]){ t886, rt_s(&S129), t887 }); });
  if (rt_cond(rt_b(rt_bool(rt_ne(v532_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v532_d), rt_s(&S33)), "&&")))) {
    (void)({ wv t890 = rt_get(&F_pos, v530_e); wv t891 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v533_label), rt_s(&S412) }); wv t892 = ({ wv t888 = rt_text(v533_label); wv t889 = rt_text(v533_label); rt_cat(5, (wv[]){ rt_s(&S413), t888, rt_s(&S414), t889, rt_s(&S415) }); }); rt_chk(s392_error(up, t890, t891, t892), "qualified"); });
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v532_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v532_d), rt_s(&S46)), "&&")))) {
    (void)({ wv t893 = rt_get(&F_pos, v530_e); wv t894 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v533_label), rt_s(&S416) }); wv t895 = rt_cat(3, (wv[]){ rt_s(&S417), rt_text(v533_label), rt_s(&S418) }); rt_chk(s392_error(up, t893, t894, t895), "qualified"); });
  }
  RT_RET(RT_NULL);
}

static wv s413_caseEnum(struct fr100 *up, wv v534_v, wv v535_scope) {
  rt_enter("caseEnum");
  v534_v = rt_bind(v534_v, 0);
  v535_scope = rt_bind(v535_scope, 0);
  wv v536_x = rt_bind(rt_get(&F_rec, v534_v), 0);
  wv v537_d = rt_bind(rt_chk(s409_enumNamed(up, v536_x, v535_scope), "caseEnum"), 0);
  if (rt_cond(rt_ne(v537_d, RT_NULL))) {
    (void)rt_chk(s414_member(up, v534_v, v537_d), "caseEnum");
    RT_RET(v537_d);
  }
  wv v538_hint = rt_s(&S419);
  if (rt_cond(rt_eq(rt_get(&F_k, v536_x), rt_s(&S212)))) {
    wv v540_message = rt_bind((rt_cond(rt_ne(({ wv t896 = v535_scope; wv t897 = rt_get(&F_name, v536_x); rt_chk(s401_lookup(up, t896, t897), "caseEnum"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v536_x)), rt_s(&S420) }) : rt_cat(3, (wv[]){ rt_s(&S421), rt_text(rt_get(&F_name, v536_x)), rt_s(&S113) })), 0);
    (void)({ wv t898 = rt_get(&F_pos, v536_x); wv t899 = v540_message; wv t900 = v538_hint; rt_chk(s392_error(up, t898, t899, t900), "caseEnum"); });
    RT_RET(RT_NULL);
  }
  wv v539_m = rt_bind(({ wv t901 = rt_get(&F_rec, v536_x); wv t902 = v535_scope; rt_chk(s407_moduleNamed(up, t901, t902), "caseEnum"); }), 0);
  if (rt_cond(rt_eq(v539_m, RT_NULL))) {
    (void)({ wv t903 = rt_get(&F_rec, v536_x); wv t904 = v535_scope; rt_chk(s411_unknownModule(up, t903, t904), "caseEnum"); });
  } else if (rt_cond(rt_ne(rt_chk(s410_exportOf(up, v536_x, v539_m), "caseEnum"), RT_NULL))) {
    (void)({ wv t905 = rt_get(&F_pos, v536_x); wv t906 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_chk(w5_enumText(v536_x), "caseEnum")), rt_s(&S420) }); wv t907 = v538_hint; rt_chk(s392_error(up, t905, t906, t907), "caseEnum"); });
  }
  RT_RET(RT_NULL);
}

static wv s414_member(struct fr100 *up, wv v541_e, wv v542_d) {
  rt_enter("member");
  v541_e = rt_bind(v541_e, 0);
  v542_d = rt_bind(v542_d, 0);
  wv v543_ms = rt_bind(rt_chk(s394_enumMembers(up, v542_d), "member"), 0);
  wv v544_names = rt_arr(0, NULL, 1);
  wv v545_found = rt_b(0);
  wv v546_k = rt_i(0);
  while (rt_cond(({ wv t908 = v546_k; wv t909 = std_length(v543_ms); rt_lt(t908, t909); }))) {
    (void)({ wv t910 = v544_names; wv t911 = rt_get(&F_name, rt_idx(v546_k, v543_ms)); std_push(t910, t911); });
    if (rt_cond(({ wv t912 = rt_get(&F_name, rt_idx(v546_k, v543_ms)); wv t913 = rt_get(&F_field, v541_e); rt_eq(t912, t913); }))) {
      v545_found = rt_b(1);
    }
    v546_k = rt_add(v546_k, rt_i(1));
  }
  if (rt_cond(rt_not(v545_found))) {
    (void)({ wv t916 = rt_get(&F_pos, v541_e); wv t917 = ({ wv t914 = rt_text(rt_get(&F_field, v541_e)); wv t915 = rt_text(rt_get(&F_name, v542_d)); rt_cat(5, (wv[]){ rt_s(&S422), t914, rt_s(&S423), t915, rt_s(&S113) }); }); wv t918 = rt_cat(2, (wv[]){ rt_s(&S424), rt_text(std_join(v544_names, rt_s(&S405))) }); rt_chk(s392_error(up, t916, t917, t918), "member"); });
  }
  RT_RET(RT_NULL);
}

static wv s415_chain(struct fr100 *up, wv v547_e, wv v548_scope, wv v549_ctx, wv v550_stmtLevel) {
  rt_enter("chain");
  v547_e = rt_bind(v547_e, 0);
  v548_scope = rt_bind(v548_scope, 0);
  v549_ctx = rt_bind(v549_ctx, 0);
  v550_stmtLevel = rt_bind(v550_stmtLevel, 0);
  wv v551_k = rt_i(0);
  while (rt_cond(({ wv t919 = v551_k; wv t920 = std_length(rt_get(&F_branches, v547_e)); rt_lt(t919, t920); }))) {
    wv v552_b = rt_bind(({ wv t921 = v551_k; wv t922 = rt_get(&F_branches, v547_e); rt_idx(t921, t922); }), 0);
    (void)({ wv t923 = rt_get(&F_cond, v552_b); wv t924 = v548_scope; wv t925 = v549_ctx; rt_chk(s419_expr(up, t923, t924, t925), "chain"); });
    (void)({ wv t926 = rt_get(&F_body, v552_b); wv t927 = rt_chk(s397_newScope(up, v548_scope), "chain"); wv t928 = v549_ctx; rt_chk(s403_block(up, t926, t927, t928), "chain"); });
    if (rt_cond(rt_not(v550_stmtLevel))) {
      (void)rt_chk(s396_requireValue(up, rt_get(&F_body, v552_b)), "chain");
    }
    v551_k = rt_add(v551_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v547_e), RT_NULL))) {
    (void)({ wv t929 = rt_get(&F_els, v547_e); wv t930 = rt_chk(s397_newScope(up, v548_scope), "chain"); wv t931 = v549_ctx; rt_chk(s403_block(up, t929, t930, t931), "chain"); });
    if (rt_cond(rt_not(v550_stmtLevel))) {
      (void)rt_chk(s396_requireValue(up, rt_get(&F_els, v547_e)), "chain");
    }
  } else if (rt_cond(rt_not(v550_stmtLevel))) {
    (void)rt_chk(s392_error(up, rt_get(&F_pos, v547_e), rt_s(&S425), RT_NULL), "chain");
  }
  RT_RET(RT_NULL);
}

static wv s416_lvalue(struct fr100 *up, wv v553_t, wv v554_scope, wv v555_ctx) {
  rt_enter("lvalue");
  v554_scope = rt_bind(v554_scope, 0);
  v555_ctx = rt_bind(v555_ctx, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v553_t), rt_s(&S212)))) {
    wv v559_d = rt_bind(({ wv t932 = rt_get(&F_name, v553_t); wv t933 = rt_get(&F_pos, v553_t); wv t934 = v554_scope; wv t935 = v555_ctx; rt_chk(s402_resolve(up, t932, t933, t934, t935), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v559_d, RT_NULL))) {
      (void)({ wv t936 = v559_d; wv t937 = rt_get(&F_name, v553_t); wv t938 = rt_get(&F_pos, v553_t); wv t939 = rt_cat(3, (wv[]){ rt_s(&S426), rt_text(rt_get(&F_name, v553_t)), rt_s(&S427) }); rt_chk(s417_mustBeMutable(up, t936, t937, t938, t939, rt_b(1)), "lvalue"); });
      if (rt_cond(rt_get(&F_isRef, v559_d))) {
        (void)({ wv t942 = rt_get(&F_refTargets, ({ wv t940 = rt_def(up->v388_curMod, "curMod"); wv t941 = up->v384_modules; rt_idx(t940, t941); })); wv t943 = rt_get(&F_pos, v553_t); std_push(t942, t943); });
      }
    }
    RT_RET(RT_NULL);
  }
  wv v556_isMem = rt_eq(rt_get(&F_k, v553_t), rt_s(&S190));
  wv v557_box = rt_bind((rt_cond(v556_isMem) ? rt_get(&F_rec, v553_t) : rt_get(&F_arr, v553_t)), 0);
  wv v558_what = rt_bind((rt_cond(v556_isMem) ? rt_s(&S428) : rt_s(&S429)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v553_t), rt_s(&S189)))) {
    (void)({ wv t944 = rt_get(&F_index, v553_t); wv t945 = v554_scope; wv t946 = v555_ctx; rt_chk(s419_expr(up, t944, t945, t946), "lvalue"); });
  }
  if (rt_cond(v556_isMem)) {
    if (rt_cond(rt_ne(rt_chk(s409_enumNamed(up, v557_box, v554_scope), "lvalue"), RT_NULL))) {
      (void)({ wv t949 = rt_get(&F_pos, v553_t); wv t950 = ({ wv t947 = rt_text(rt_chk(w5_enumText(v557_box), "lvalue")); wv t948 = rt_text(rt_get(&F_field, v553_t)); rt_cat(5, (wv[]){ rt_s(&S426), t947, rt_s(&S129), t948, rt_s(&S430) }); }); rt_chk(s392_error(up, t949, t950, RT_NULL), "lvalue"); });
      RT_RET(RT_NULL);
    }
    wv v560_m = rt_bind(rt_chk(s407_moduleNamed(up, v557_box, v554_scope), "lvalue"), 0);
    if (rt_cond(rt_ne(v560_m, RT_NULL))) {
      if (rt_cond(rt_ne(rt_chk(s410_exportOf(up, v553_t, v560_m), "lvalue"), RT_NULL))) {
        (void)({ wv t956 = rt_get(&F_pos, v553_t); wv t957 = ({ wv t953 = rt_text(rt_get(&F_name, v557_box)); wv t954 = rt_text(rt_get(&F_field, v553_t)); wv t955 = rt_text(rt_get(&F_path, ({ wv t951 = rt_get(&F_lib, v560_m); wv t952 = up->v384_modules; rt_idx(t951, t952); }))); rt_cat(7, (wv[]){ rt_s(&S426), t953, rt_s(&S129), t954, rt_s(&S431), t955, rt_s(&S432) }); }); rt_chk(s392_error(up, t956, t957, RT_NULL), "lvalue"); });
      }
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v557_box), rt_s(&S212)))) {
    wv v561_d = rt_bind(({ wv t958 = rt_get(&F_name, v557_box); wv t959 = rt_get(&F_pos, v557_box); wv t960 = v554_scope; wv t961 = v555_ctx; rt_chk(s402_resolve(up, t958, t959, t960, t961), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v561_d, RT_NULL))) {
      (void)({ wv t964 = v561_d; wv t965 = rt_get(&F_name, v557_box); wv t966 = rt_get(&F_pos, v557_box); wv t967 = ({ wv t962 = rt_text(v558_what); wv t963 = rt_text(rt_get(&F_name, v557_box)); rt_cat(5, (wv[]){ rt_s(&S433), t962, rt_s(&S434), t963, rt_s(&S435) }); }); rt_chk(s417_mustBeMutable(up, t964, t965, t966, t967, rt_b(0)), "lvalue"); });
    }
  } else {
    (void)rt_chk(s419_expr(up, v557_box, v554_scope, v555_ctx), "lvalue");
  }
  RT_RET(RT_NULL);
}

static wv s417_mustBeMutable(struct fr100 *up, wv v562_d, wv v563_name, wv v564_pos, wv v565_message, wv v566_rebind) {
  rt_enter("mustBeMutable");
  v562_d = rt_bind(v562_d, 0);
  v563_name = rt_bind(v563_name, 0);
  v564_pos = rt_bind(v564_pos, 0);
  v565_message = rt_bind(v565_message, 0);
  v566_rebind = rt_bind(v566_rebind, 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v562_d), rt_s(&S33)), "||") || rt_bool(rt_eq(rt_get(&F_kind, v562_d), rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_kind, v562_d), rt_s(&S408)), "||")))) {
    (void)({ wv t970 = v564_pos; wv t971 = ({ wv t968 = rt_text(v563_name); wv t969 = rt_text(rt_chk(w5_kindText(rt_get(&F_kind, v562_d)), "mustBeMutable")); rt_cat(5, (wv[]){ rt_s(&S113), t968, rt_s(&S436), t969, rt_s(&S437) }); }); rt_chk(s392_error(up, t970, t971, RT_NULL), "mustBeMutable"); });
  } else if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_isVar, v562_d)), "&&") && rt_bool(rt_not(rt_get(&F_isRef, v562_d)), "&&")))) {
    wv v567_hint = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_kind, v562_d), rt_s(&S393)))) {
      v567_hint = rt_bind((rt_cond(v566_rebind) ? ({ wv t972 = rt_text(v563_name); wv t973 = rt_text(v563_name); rt_cat(5, (wv[]){ rt_s(&S438), t972, rt_s(&S439), t973, rt_s(&S440) }); }) : rt_cat(3, (wv[]){ rt_s(&S441), rt_text(v563_name), rt_s(&S16) })), 1);
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v562_d), rt_s(&S177)))) {
      v567_hint = rt_bind((rt_cond(v566_rebind) ? ({ wv t974 = rt_text(v563_name); wv t975 = rt_text(v563_name); rt_cat(5, (wv[]){ rt_s(&S442), t974, rt_s(&S443), t975, rt_s(&S444) }); }) : rt_cat(3, (wv[]){ rt_s(&S445), rt_text(v563_name), rt_s(&S446) })), 1);
    }
    (void)rt_chk(s392_error(up, v564_pos, v565_message, v567_hint), "mustBeMutable");
  }
  RT_RET(RT_NULL);
}

static wv s418_unknownStd(struct fr100 *up, wv v568_node) {
  rt_enter("unknownStd");
  v568_node = rt_bind(v568_node, 0);
  (void)({ wv t976 = rt_get(&F_pos, v568_node); wv t977 = rt_cat(3, (wv[]){ rt_s(&S447), rt_text(rt_get(&F_name, v568_node)), rt_s(&S113) }); rt_chk(s392_error(up, t976, t977, rt_s(&S136)), "unknownStd"); });
  RT_RET(RT_NULL);
}

static wv s419_expr(struct fr100 *up, wv v569_e, wv v570_scope, wv v571_ctx) {
  rt_enter("expr");
  v570_scope = rt_bind(v570_scope, 0);
  v571_ctx = rt_bind(v571_ctx, 0);
  wv v572_kind = rt_bind(rt_get(&F_k, v569_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v572_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v572_kind, rt_s(&S148)), "||")), "||") || rt_bool(rt_eq(v572_kind, rt_s(&S279)), "||")), "||") || rt_bool(rt_eq(v572_kind, rt_s(&S51)), "||")), "||") || rt_bool(rt_eq(v572_kind, rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(v572_kind, rt_s(&S53)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S171)))) {
    wv v573_k = rt_i(0);
    while (rt_cond(({ wv t978 = v573_k; wv t979 = std_length(rt_get(&F_parts, v569_e)); rt_lt(t978, t979); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t980 = v573_k; wv t981 = rt_get(&F_parts, v569_e); rt_idx(t980, t981); })), rt_s(&S185)))) {
        (void)({ wv t984 = ({ wv t982 = v573_k; wv t983 = rt_get(&F_parts, v569_e); rt_idx(t982, t983); }); wv t985 = v570_scope; wv t986 = v571_ctx; rt_chk(s419_expr(up, t984, t985, t986), "expr"); });
      }
      v573_k = rt_add(v573_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S212)))) {
    wv v574_d = rt_bind(({ wv t987 = rt_get(&F_name, v569_e); wv t988 = rt_get(&F_pos, v569_e); wv t989 = v570_scope; wv t990 = v571_ctx; rt_chk(s402_resolve(up, t987, t988, t989, t990), "expr"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v574_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v574_d), rt_s(&S33)), "&&")))) {
      (void)({ wv t993 = rt_get(&F_pos, v569_e); wv t994 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v569_e)), rt_s(&S412) }); wv t995 = ({ wv t991 = rt_text(rt_get(&F_name, v569_e)); wv t992 = rt_text(rt_get(&F_name, v569_e)); rt_cat(5, (wv[]){ rt_s(&S413), t991, rt_s(&S414), t992, rt_s(&S415) }); }); rt_chk(s392_error(up, t993, t994, t995), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v574_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v574_d), rt_s(&S46)), "&&")))) {
      (void)({ wv t996 = rt_get(&F_pos, v569_e); wv t997 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v569_e)), rt_s(&S416) }); wv t998 = rt_cat(3, (wv[]){ rt_s(&S417), rt_text(rt_get(&F_name, v569_e)), rt_s(&S418) }); rt_chk(s392_error(up, t996, t997, t998), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v574_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v574_d), rt_s(&S408)), "&&")))) {
      (void)({ wv t999 = rt_get(&F_pos, v569_e); wv t1000 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v569_e)), rt_s(&S448) }); wv t1001 = rt_cat(3, (wv[]){ rt_s(&S449), rt_text(rt_get(&F_name, v569_e)), rt_s(&S450) }); rt_chk(s392_error(up, t999, t1000, t1001), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S156)))) {
    wv v575_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v569_e)), "expr"), 0);
    if (rt_cond(rt_eq(v575_sig, RT_NULL))) {
      (void)rt_chk(s418_unknownStd(up, v569_e), "expr");
    } else if (rt_cond(rt_ne(v575_sig, rt_s(&S35)))) {
      (void)({ wv t1004 = rt_get(&F_pos, v569_e); wv t1005 = ({ wv t1002 = rt_text(rt_get(&F_name, v569_e)); wv t1003 = rt_text(rt_get(&F_name, v569_e)); rt_cat(5, (wv[]){ rt_s(&S179), t1002, rt_s(&S451), t1003, rt_s(&S16) }); }); rt_chk(s392_error(up, t1004, t1005, RT_NULL), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v572_kind, rt_s(&S186)), "||") || rt_bool(rt_eq(v572_kind, rt_s(&S187)), "||")))) {
    wv v576_k = rt_i(0);
    while (rt_cond(({ wv t1006 = v576_k; wv t1007 = std_length(rt_get(&F_items, v569_e)); rt_lt(t1006, t1007); }))) {
      (void)({ wv t1010 = ({ wv t1008 = v576_k; wv t1009 = rt_get(&F_items, v569_e); rt_idx(t1008, t1009); }); wv t1011 = v570_scope; wv t1012 = v571_ctx; rt_chk(s419_expr(up, t1010, t1011, t1012), "expr"); });
      v576_k = rt_add(v576_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S188)))) {
    wv v577_k = rt_i(0);
    while (rt_cond(({ wv t1013 = v577_k; wv t1014 = std_length(rt_get(&F_fields, v569_e)); rt_lt(t1013, t1014); }))) {
      (void)({ wv t1017 = rt_get(&F_e, ({ wv t1015 = v577_k; wv t1016 = rt_get(&F_fields, v569_e); rt_idx(t1015, t1016); })); wv t1018 = v570_scope; wv t1019 = v571_ctx; rt_chk(s419_expr(up, t1017, t1018, t1019), "expr"); });
      v577_k = rt_add(v577_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S189)))) {
    (void)({ wv t1020 = rt_get(&F_index, v569_e); wv t1021 = v570_scope; wv t1022 = v571_ctx; rt_chk(s419_expr(up, t1020, t1021, t1022), "expr"); });
    (void)({ wv t1023 = rt_get(&F_arr, v569_e); wv t1024 = v570_scope; wv t1025 = v571_ctx; rt_chk(s419_expr(up, t1023, t1024, t1025), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S190)))) {
    wv v578_lin = rt_bind(({ wv t1026 = rt_get(&F_rec, v569_e); wv t1027 = v570_scope; rt_chk(s409_enumNamed(up, t1026, t1027), "expr"); }), 0);
    if (rt_cond(rt_ne(v578_lin, RT_NULL))) {
      (void)rt_chk(s414_member(up, v569_e, v578_lin), "expr");
      RT_RET(RT_NULL);
    }
    wv v579_m = rt_bind(({ wv t1028 = rt_get(&F_rec, v569_e); wv t1029 = v570_scope; rt_chk(s407_moduleNamed(up, t1028, t1029), "expr"); }), 0);
    if (rt_cond(rt_ne(v579_m, RT_NULL))) {
      (void)rt_chk(s412_qualified(up, v569_e, v579_m), "expr");
    } else {
      (void)({ wv t1030 = rt_get(&F_rec, v569_e); wv t1031 = v570_scope; wv t1032 = v571_ctx; rt_chk(s419_expr(up, t1030, t1031, t1032), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S191)))) {
    (void)({ wv t1033 = rt_get(&F_e, v569_e); wv t1034 = v570_scope; wv t1035 = v571_ctx; rt_chk(s419_expr(up, t1033, t1034, t1035), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S192)))) {
    (void)({ wv t1036 = rt_get(&F_l, v569_e); wv t1037 = v570_scope; wv t1038 = v571_ctx; rt_chk(s419_expr(up, t1036, t1037, t1038), "expr"); });
    (void)({ wv t1039 = rt_get(&F_r, v569_e); wv t1040 = v570_scope; wv t1041 = v571_ctx; rt_chk(s419_expr(up, t1039, t1040, t1041), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S193)))) {
    (void)({ wv t1042 = rt_get(&F_c, v569_e); wv t1043 = v570_scope; wv t1044 = v571_ctx; rt_chk(s419_expr(up, t1042, t1043, t1044), "expr"); });
    (void)({ wv t1045 = rt_get(&F_a, v569_e); wv t1046 = v570_scope; wv t1047 = v571_ctx; rt_chk(s419_expr(up, t1045, t1046, t1047), "expr"); });
    (void)({ wv t1048 = rt_get(&F_b, v569_e); wv t1049 = v570_scope; wv t1050 = v571_ctx; rt_chk(s419_expr(up, t1048, t1049, t1050), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S182)))) {
    wv v580_k = rt_i(0);
    while (rt_cond(({ wv t1051 = v580_k; wv t1052 = std_length(rt_get(&F_args, v569_e)); rt_lt(t1051, t1052); }))) {
      (void)({ wv t1055 = ({ wv t1053 = v580_k; wv t1054 = rt_get(&F_args, v569_e); rt_idx(t1053, t1054); }); wv t1056 = v570_scope; wv t1057 = v571_ctx; rt_chk(s419_expr(up, t1055, t1056, t1057), "expr"); });
      v580_k = rt_add(v580_k, rt_i(1));
    }
    (void)rt_chk(s420_callee(up, v569_e, v570_scope, rt_b(1)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S183)))) {
    (void)rt_chk(s392_error(up, rt_get(&F_pos, v569_e), rt_s(&S452), RT_NULL), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S44)))) {
    (void)rt_chk(s406_checkMatch(up, v569_e, v570_scope, v571_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v572_kind, rt_s(&S184)))) {
    (void)rt_chk(s415_chain(up, v569_e, v570_scope, v571_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S453), rt_text(v572_kind) }));
  RT_RET(RT_NULL);
}

static wv s420_callee(struct fr100 *up, wv v581_e, wv v582_scope, wv v583_used) {
  rt_enter("callee");
  v581_e = rt_bind(v581_e, 0);
  v582_scope = rt_bind(v582_scope, 0);
  v583_used = rt_bind(v583_used, 0);
  wv v584_c = rt_bind(rt_get(&F_callee, v581_e), 0);
  wv v585_n = rt_bind(std_length(rt_get(&F_args, v581_e)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v584_c), rt_s(&S156)))) {
    wv v588_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v584_c)), "callee"), 0);
    if (rt_cond(rt_eq(v588_sig, RT_NULL))) {
      (void)rt_chk(s418_unknownStd(up, v584_c), "callee");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(v588_sig, rt_s(&S35)))) {
      (void)({ wv t1058 = rt_get(&F_pos, v584_c); wv t1059 = rt_cat(3, (wv[]){ rt_s(&S179), rt_text(rt_get(&F_name, v584_c)), rt_s(&S454) }); rt_chk(s392_error(up, t1058, t1059, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_ne(v588_sig, v585_n))) {
      (void)({ wv t1063 = rt_get(&F_pos, v584_c); wv t1064 = ({ wv t1060 = rt_text(rt_get(&F_name, v584_c)); wv t1061 = rt_text(rt_chk(w5_plural(v588_sig), "callee")); wv t1062 = rt_text(v585_n); rt_cat(6, (wv[]){ rt_s(&S179), t1060, rt_s(&S455), t1061, rt_s(&S456), t1062 }); }); rt_chk(s392_error(up, t1063, t1064, RT_NULL), "callee"); });
    }
    RT_RET(RT_NULL);
  }
  wv v586_d = RT_NULL;
  wv v587_label = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v584_c), rt_s(&S190)))) {
    v587_label = ({ wv t1065 = rt_text(rt_get(&F_name, rt_get(&F_rec, v584_c))); wv t1066 = rt_text(rt_get(&F_field, v584_c)); rt_cat(3, (wv[]){ t1065, rt_s(&S129), t1066 }); });
    wv v589_m = rt_bind(({ wv t1067 = rt_get(&F_rec, v584_c); wv t1068 = v582_scope; rt_chk(s407_moduleNamed(up, t1067, t1068), "callee"); }), 0);
    if (rt_cond(rt_eq(v589_m, RT_NULL))) {
      (void)({ wv t1069 = rt_get(&F_rec, v584_c); wv t1070 = v582_scope; rt_chk(s411_unknownModule(up, t1069, t1070), "callee"); });
      RT_RET(RT_NULL);
    }
    v586_d = rt_chk(s410_exportOf(up, v584_c, v589_m), "callee");
    if (rt_cond(rt_eq(v586_d, RT_NULL))) {
      RT_RET(RT_NULL);
    }
  } else {
    v587_label = rt_bind(rt_get(&F_name, v584_c), 1);
    v586_d = ({ wv t1071 = v582_scope; wv t1072 = rt_get(&F_name, v584_c); rt_chk(s401_lookup(up, t1071, t1072), "callee"); });
    if (rt_cond(rt_eq(v586_d, RT_NULL))) {
      (void)({ wv t1073 = rt_get(&F_pos, v584_c); wv t1074 = rt_cat(3, (wv[]){ rt_s(&S457), rt_text(rt_get(&F_name, v584_c)), rt_s(&S113) }); rt_chk(s392_error(up, t1073, t1074, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_ne(rt_get(&F_kind, v586_d), rt_s(&S33)))) {
    wv v590_hint = rt_bind((rt_cond(rt_eq(v585_n, rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S97), rt_text(v587_label), rt_s(&S458) }) : rt_s(&S459)), 0);
    (void)({ wv t1075 = rt_get(&F_pos, v584_c); wv t1076 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v587_label), rt_s(&S460) }); wv t1077 = v590_hint; rt_chk(s392_error(up, t1075, t1076, t1077), "callee"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(({ wv t1078 = rt_get(&F_arity, v586_d); wv t1079 = v585_n; rt_ne(t1078, t1079); }))) {
    (void)({ wv t1083 = rt_get(&F_pos, v584_c); wv t1084 = ({ wv t1080 = rt_text(v587_label); wv t1081 = rt_text(rt_chk(w5_plural(rt_get(&F_arity, v586_d)), "callee")); wv t1082 = rt_text(v585_n); rt_cat(6, (wv[]){ rt_s(&S113), t1080, rt_s(&S455), t1081, rt_s(&S456), t1082 }); }); rt_chk(s392_error(up, t1083, t1084, RT_NULL), "callee"); });
  }
  if (rt_cond(rt_b(rt_bool(v583_used, "&&") && rt_bool(rt_eq(({ wv t1085 = rt_get(&F_mod, v586_d); wv t1086 = rt_get(&F_namePos, v586_d); rt_chk(s395_firstUse(up, t1085, t1086), "callee"); }), RT_NULL), "&&")))) {
    (void)({ wv t1091 = rt_def(up->v390_uses, "uses"); wv t1092 = ({ wv t1087 = rt_get(&F_mod, v586_d); wv t1088 = rt_get(&F_namePos, v586_d); wv t1089 = rt_get(&F_pos, v584_c); wv t1090 = rt_def(up->v388_curMod, "curMod"); rt_rec(0, 4, K47, (wv[]){ t1087, t1088, t1089, t1090 }); }); std_push(t1091, t1092); });
  }
  RT_RET(RT_NULL);
}

static wv s421_importNames(struct fr100 *up, wv v591_m, wv v592_scope) {
  rt_enter("importNames");
  v591_m = rt_bind(v591_m, 0);
  v592_scope = rt_bind(v592_scope, 0);
  wv v593_imports = rt_bind(rt_get(&F_imports, rt_idx(v591_m, up->v384_modules)), 0);
  wv v594_seen = rt_arr(0, NULL, 1);
  wv v595_k = rt_i(0);
  while (rt_cond(({ wv t1093 = v595_k; wv t1094 = std_length(v593_imports); rt_lt(t1093, t1094); }))) {
    wv v596_node = rt_bind(rt_get(&F_node, rt_idx(v595_k, v593_imports)), 0);
    wv v597_lib = rt_bind(rt_get(&F_lib, rt_idx(v595_k, v593_imports)), 0);
    v595_k = rt_add(v595_k, rt_i(1));
    wv v598_path = rt_bind(rt_get(&F_path, rt_idx(v597_lib, up->v384_modules)), 0);
    wv v599_usable = rt_bind(v597_lib, 1);
    if (rt_cond(rt_chk(w1_contains(v594_seen, v597_lib), "importNames"))) {
      (void)({ wv t1095 = rt_get(&F_pathPos, v596_node); wv t1096 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v598_path), rt_s(&S461) }); rt_chk(s392_error(up, t1095, t1096, RT_NULL), "importNames"); });
      v599_usable = RT_NULL;
    } else if (rt_cond(rt_get(&F_notLibrary, rt_idx(v597_lib, up->v384_modules)))) {
      (void)({ wv t1097 = rt_get(&F_pathPos, v596_node); wv t1098 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v598_path), rt_s(&S462) }); rt_chk(s392_error(up, t1097, t1098, RT_NULL), "importNames"); });
      v599_usable = RT_NULL;
    }
    (void)std_push(v594_seen, v597_lib);
    wv v600_d = ({ wv t1099 = rt_get(&F_name, v596_node); wv t1100 = rt_get(&F_namePos, v596_node); wv t1101 = rt_def(up->v388_curMod, "curMod"); wv t1102 = v599_usable; rt_rec(0, 10, K41, (wv[]){ rt_s(&S408), t1099, rt_b(0), rt_b(0), t1100, RT_NULL, RT_NULL, RT_NULL, t1101, t1102 }); });
    (void)({ wv t1103 = v592_scope; wv t1104 = rt_get(&F_name, v596_node); wv t1105 = v600_d; rt_chk(s400_declare(up, t1103, t1104, t1105), "importNames"); });
  }
  RT_RET(RT_NULL);
}

static wv s422_libraryRules(struct fr100 *up, wv v601_m, wv v602_scope, wv v603_isEntry, wv v604_last) {
  rt_enter("libraryRules");
  v601_m = rt_bind(v601_m, 0);
  v602_scope = rt_bind(v602_scope, 0);
  v603_isEntry = rt_bind(v603_isEntry, 0);
  v604_last = rt_bind(v604_last, 0);
  wv v605_program = rt_bind(rt_get(&F_program, rt_idx(v601_m, up->v384_modules)), 0);
  wv v606_k = rt_i(0);
  while (rt_cond(({ wv t1106 = v606_k; wv t1107 = std_length(rt_get(&F_stmts, v605_program)); rt_lt(t1106, t1107); }))) {
    wv v608_s = rt_bind(({ wv t1108 = v606_k; wv t1109 = rt_get(&F_stmts, v605_program); rt_idx(t1108, t1109); }), 0);
    v606_k = rt_add(v606_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v608_s), rt_s(&S204)), "&&") && rt_bool(rt_b(rt_bool(v603_isEntry, "||") || rt_bool(({ wv t1110 = rt_get(&F_pos, v608_s); wv t1111 = rt_get(&F_pos, v604_last); rt_ne(t1110, t1111); }), "||")), "&&")))) {
      (void)rt_chk(s392_error(up, rt_get(&F_pos, v608_s), rt_s(&S463), rt_s(&S464)), "libraryRules");
    } else if (rt_cond(rt_b(rt_bool(rt_not(v603_isEntry), "&&") && rt_bool(rt_not(({ wv t1112 = rt_def(w5_libraryTop, "libraryTop"); wv t1113 = rt_get(&F_k, v608_s); rt_chk(w1_contains(t1112, t1113), "libraryRules"); })), "&&")))) {
      (void)rt_chk(s392_error(up, rt_get(&F_pos, v608_s), rt_s(&S465), rt_s(&S466)), "libraryRules");
    }
  }
  if (rt_cond(v603_isEntry)) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v604_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v604_last), rt_s(&S204)), "||")))) {
    (void)rt_chk(s392_error(up, rt_get(&F_end, v605_program), rt_s(&S467), rt_s(&S468)), "libraryRules");
    RT_RET(RT_NULL);
  }
  wv v607_exports = rt_arr(0, NULL, 1);
  v606_k = rt_i(0);
  while (rt_cond(({ wv t1114 = v606_k; wv t1115 = std_length(rt_get(&F_names, v604_last)); rt_lt(t1114, t1115); }))) {
    wv v609_n = rt_bind(({ wv t1116 = v606_k; wv t1117 = rt_get(&F_names, v604_last); rt_idx(t1116, t1117); }), 0);
    v606_k = rt_add(v606_k, rt_i(1));
    wv v610_d = rt_bind(({ wv t1118 = v602_scope; wv t1119 = rt_get(&F_name, v609_n); rt_chk(s399_own(up, t1118, t1119), "libraryRules"); }), 0);
    if (rt_cond(rt_eq(v610_d, RT_NULL))) {
      (void)({ wv t1120 = rt_get(&F_pos, v609_n); wv t1121 = rt_cat(3, (wv[]){ rt_s(&S469), rt_text(rt_get(&F_name, v609_n)), rt_s(&S470) }); rt_chk(s392_error(up, t1120, t1121, RT_NULL), "libraryRules"); });
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v610_d), rt_s(&S408)))) {
      (void)({ wv t1122 = rt_get(&F_pos, v609_n); wv t1123 = rt_cat(3, (wv[]){ rt_s(&S469), rt_text(rt_get(&F_name, v609_n)), rt_s(&S471) }); rt_chk(s392_error(up, t1122, t1123, RT_NULL), "libraryRules"); });
    } else {
      (void)std_push(v607_exports, v610_d);
    }
  }
  ({ wv t1124 = v607_exports; wv t1125 = rt_idx(v601_m, up->v384_modules); rt_set_mem(t1124, &F_exports, t1125); });
  RT_RET(RT_NULL);
}

static wv s423_checkModule(struct fr100 *up, wv v611_m, wv v612_isEntry) {
  rt_enter("checkModule");
  v611_m = rt_bind(v611_m, 0);
  v612_isEntry = rt_bind(v612_isEntry, 0);
  (void)rt_chk(s393_enter(up, v611_m), "checkModule");
  wv v613_program = rt_bind(rt_get(&F_program, rt_idx(v611_m, up->v384_modules)), 0);
  wv v614_last = RT_NULL;
  wv v615_k = rt_i(0);
  while (rt_cond(({ wv t1126 = v615_k; wv t1127 = std_length(rt_get(&F_stmts, v613_program)); rt_lt(t1126, t1127); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1128 = v615_k; wv t1129 = rt_get(&F_stmts, v613_program); rt_idx(t1128, t1129); })), rt_s(&S201)))) {
      v614_last = rt_bind(({ wv t1130 = v615_k; wv t1131 = rt_get(&F_stmts, v613_program); rt_idx(t1130, t1131); }), 1);
    }
    v615_k = rt_add(v615_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v612_isEntry), "&&") && rt_bool(rt_ne(v614_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v614_last), rt_s(&S54)), "&&")))) {
    rt_set_mem(rt_b(1), &F_notLibrary, rt_idx(v611_m, up->v384_modules));
    RT_RET(RT_NULL);
  }
  wv v616_scope = rt_bind(rt_chk(s397_newScope(up, RT_NULL), "checkModule"), 0);
  (void)rt_chk(s421_importNames(up, v611_m, v616_scope), "checkModule");
  (void)({ wv t1132 = rt_rec(0, 2, K48, (wv[]){ rt_get(&F_stmts, v613_program), RT_NULL }); wv t1133 = v616_scope; wv t1134 = rt_rec(0, 2, K45, (wv[]){ rt_def(up->v424_topFn, "topFn"), rt_i(0) }); rt_chk(s403_block(up, t1132, t1133, t1134), "checkModule"); });
  (void)rt_chk(s422_libraryRules(up, v611_m, v616_scope, v612_isEntry, v614_last), "checkModule");
  RT_RET(RT_NULL);
}

static wv w6_binFn(wv v623_op) {
  rt_enter("binFn");
  v623_op = rt_bind(v623_op, 0);
  wv v624_k = rt_i(0);
  while (rt_cond(({ wv t1161 = ({ wv t1159 = v624_k; wv t1160 = rt_def(w6_binOps, "binOps"); rt_idx(t1159, t1160); }); wv t1162 = v623_op; rt_ne(t1161, t1162); }))) {
    v624_k = rt_add(v624_k, rt_i(1));
  }
  RT_RET(({ wv t1163 = v624_k; wv t1164 = rt_def(w6_binFns, "binFns"); rt_idx(t1163, t1164); }));
}

static wv w6_modeOf(wv v625_x) {
  rt_enter("modeOf");
  v625_x = rt_bind(v625_x, 0);
  RT_RET((rt_cond(rt_get(&F_isVar, v625_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v625_x)) ? rt_s(&S37) : rt_s(&S35))));
}

static wv w6_unFn(wv v626_op) {
  rt_enter("unFn");
  v626_op = rt_bind(v626_op, 0);
  RT_RET((rt_cond(rt_eq(v626_op, rt_s(&S85))) ? rt_s(&S495) : (rt_cond(rt_eq(v626_op, rt_s(&S94))) ? rt_s(&S496) : rt_s(&S497))));
}

static wv w6_repeat(wv v627_s, wv v628_count) {
  rt_enter("repeat");
  v627_s = rt_bind(v627_s, 0);
  v628_count = rt_bind(v628_count, 0);
  wv v629_out = rt_s(&S13);
  wv v630_k = rt_i(0);
  while (rt_cond(rt_lt(v630_k, v628_count))) {
    v629_out = rt_add(v629_out, v627_s);
    v630_k = rt_add(v630_k, rt_i(1));
  }
  RT_RET(v629_out);
}

static wv w6_utf8Length(wv v631_s) {
  rt_enter("utf8Length");
  v631_s = rt_bind(v631_s, 0);
  wv v632_cs = rt_bind(std_split(v631_s, rt_s(&S13)), 0);
  wv v633_n = rt_i(0);
  wv v634_k = rt_i(0);
  while (rt_cond(({ wv t1165 = v634_k; wv t1166 = std_length(v632_cs); rt_lt(t1165, t1166); }))) {
    wv v635_c = rt_bind(std_code(rt_idx(v634_k, v632_cs)), 0);
    wv t1167 = (rt_cond(rt_lt(v635_c, rt_i(128))) ? rt_i(1) : (rt_cond(rt_lt(v635_c, rt_i(2048))) ? rt_i(2) : (rt_cond(rt_lt(v635_c, rt_i(65536))) ? rt_i(3) : rt_i(4))));
    v633_n = rt_add(v633_n, t1167);
    v634_k = rt_add(v634_k, rt_i(1));
  }
  RT_RET(v633_n);
}

static wv w6_cString(wv v636_s) {
  rt_enter("cString");
  v636_s = rt_bind(v636_s, 0);
  wv v637_cs = rt_bind(std_split(v636_s, rt_s(&S13)), 0);
  wv v638_out = rt_s(&S18);
  wv v639_k = rt_i(0);
  while (rt_cond(({ wv t1168 = v639_k; wv t1169 = std_length(v637_cs); rt_lt(t1168, t1169); }))) {
    wv v640_ch = rt_bind(rt_idx(v639_k, v637_cs), 0);
    wv v641_c = rt_bind(std_code(v640_ch), 0);
    if (rt_cond(rt_eq(v640_ch, rt_s(&S18)))) {
      v638_out = rt_add(v638_out, rt_s(&S498));
    } else if (rt_cond(rt_eq(v640_ch, rt_s(&S19)))) {
      v638_out = rt_add(v638_out, rt_s(&S499));
    } else if (rt_cond(rt_eq(v640_ch, rt_s(&S2)))) {
      v638_out = rt_add(v638_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v640_ch, rt_s(&S1)))) {
      v638_out = rt_add(v638_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v640_ch, rt_s(&S3)))) {
      v638_out = rt_add(v638_out, rt_s(&S21));
    } else if (rt_cond(rt_b(rt_bool(rt_lt(v641_c, rt_i(32)), "||") || rt_bool(rt_eq(v641_c, rt_i(127)), "||")))) {
      v638_out = ({ wv t1173 = v638_out; wv t1174 = ({ wv t1170 = rt_text(std_floor(rt_div(v641_c, rt_i(64)))); wv t1171 = rt_text(rt_mod(std_floor(rt_div(v641_c, rt_i(8))), rt_i(8))); wv t1172 = rt_text(rt_mod(v641_c, rt_i(8))); rt_cat(4, (wv[]){ rt_s(&S19), t1170, t1171, t1172 }); }); rt_add(t1173, t1174); });
    } else {
      v638_out = rt_add(v638_out, v640_ch);
    }
    v639_k = rt_add(v639_k, rt_i(1));
  }
  RT_RET(rt_add(v638_out, rt_s(&S18)));
}

static wv w6_wstrInit(wv v642_s) {
  rt_enter("wstrInit");
  v642_s = rt_bind(v642_s, 0);
  wv v643_bytes = rt_bind(rt_chk(w6_utf8Length(v642_s), "wstrInit"), 0);
  wv v644_cps = rt_bind(std_length(v642_s), 0);
  RT_RET(({ wv t1175 = rt_text(v643_bytes); wv t1176 = rt_text(v644_cps); wv t1177 = rt_text((rt_cond(rt_eq(v643_bytes, v644_cps)) ? rt_i(1) : rt_i(0))); wv t1178 = rt_text(rt_chk(w6_cString(v642_s), "wstrInit")); rt_cat(9, (wv[]){ rt_s(&S500), t1175, rt_s(&S405), t1176, rt_s(&S405), t1177, rt_s(&S501), t1178, rt_s(&S502) }); }));
}

static wv w6_values(wv v645_cs) {
  rt_enter("values");
  RT_RET((rt_cond(rt_eq(std_length(v645_cs), rt_i(0))) ? rt_s(&S503) : rt_cat(3, (wv[]){ rt_s(&S504), rt_text(std_join(v645_cs, rt_s(&S405))), rt_s(&S502) })));
}

static wv w6_generateC(wv p_v646_modules, wv p_v647_runtime) {
  rt_enter("generateC");
  struct fr142 fr = { 0 };
  fr.v646_modules = rt_bind(p_v646_modules, 0);
  fr.v647_runtime = rt_bind(p_v647_runtime, 0);
  fr.v648_n = rt_i(0);
  fr.v649_tmps = rt_i(0);
  fr.v650_strings = rt_arr(0, NULL, 1);
  fr.v651_fields = rt_arr(0, NULL, 1);
  fr.v652_shapes = rt_arr(0, NULL, 1);
  fr.v653_enumDefs = rt_arr(0, NULL, 1);
  fr.v654_globals = rt_arr(0, NULL, 1);
  fr.v655_frames = rt_arr(0, NULL, 1);
  fr.v656_functions = rt_arr(0, NULL, 1);
  fr.v657_out = rt_arr(0, NULL, 1);
  fr.v658_indent = rt_i(1);
  fr.v659_loops = rt_i(0);
  fr.v660_iife = rt_b(0);
  fr.v661_decls = rt_arr(0, NULL, 1);
  fr.v662_fns = rt_arr(0, NULL, 1);
  fr.v663_tops = rt_arr(0, NULL, 1);
  fr.v664_slots = rt_arr(0, NULL, 1);
  fr.v665_curMod = rt_i(0);
  fr.v666_mod = rt_s(&S13);
  fr.v667_curSlot = rt_arr(0, NULL, 1);
  fr.v668_stolen = rt_arr(0, NULL, 1);
  fr.v669_ownEnums = rt_arr(0, NULL, 1);
  fr.v670_cur = rt_i(0);
  wv v731_mi = rt_i(0);
  while (rt_cond(({ wv t2125 = v731_mi; wv t2126 = std_length(fr.v646_modules); rt_lt(t2125, t2126); }))) {
    (void)rt_chk(s685_analyze(&fr, v731_mi), "generateC");
    v731_mi = rt_add(v731_mi, rt_i(1));
  }
  v731_mi = rt_i(0);
  while (rt_cond(({ wv t2127 = v731_mi; wv t2128 = std_length(fr.v646_modules); rt_lt(t2127, t2128); }))) {
    wv v1009_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v731_mi, fr.v646_modules))), 0);
    wv v1010_li = rt_i(0);
    while (rt_cond(({ wv t2129 = v1010_li; wv t2130 = std_length(v1009_list); rt_lt(t2129, t2130); }))) {
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1010_li, v1009_list)), rt_s(&S46)))) {
        (void)({ wv t2131 = rt_idx(v1010_li, v1009_list); wv t2132 = rt_chk(s671_modId(&fr, v731_mi), "generateC"); rt_chk(s691_enumDef(&fr, t2131, t2132), "generateC"); });
      }
      v1010_li = rt_add(v1010_li, rt_i(1));
    }
    v731_mi = rt_add(v731_mi, rt_i(1));
  }
  v731_mi = rt_i(0);
  while (rt_cond(({ wv t2133 = v731_mi; wv t2134 = std_length(fr.v646_modules); rt_lt(t2133, t2134); }))) {
    (void)rt_chk(s686_enter(&fr, v731_mi), "generateC");
    (void)rt_chk(s700_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v731_mi, fr.v646_modules)))), "generateC");
    v731_mi = rt_add(v731_mi, rt_i(1));
  }
  RT_RET(rt_chk(s730_assemble(&fr), "generateC"));
}

static wv s671_modId(struct fr142 *up, wv v732_m) {
  rt_enter("modId");
  v732_m = rt_bind(v732_m, 0);
  RT_RET((rt_cond(({ wv t1179 = v732_m; wv t1180 = rt_sub(std_length(up->v646_modules), rt_i(1)); rt_eq(t1179, t1180); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v732_m, rt_i(1))) })));
}

static wv s672_hasFrame(struct fr142 *up, wv v733_f) {
  rt_enter("hasFrame");
  v733_f = rt_bind(v733_f, 0);
  RT_RET(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1181 = v733_f; wv t1182 = rt_def(up->v662_fns, "fns"); rt_idx(t1181, t1182); }))), "&&") && rt_bool(rt_gt(rt_get(&F_children, ({ wv t1183 = v733_f; wv t1184 = rt_def(up->v662_fns, "fns"); rt_idx(t1183, t1184); })), rt_i(0)), "&&")));
}

static wv s673_newScope(struct fr142 *up, wv v734_parent) {
  rt_enter("newScope");
  v734_parent = rt_bind(v734_parent, 0);
  wv v735_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K40, (wv[]){ v734_parent, v735_names }));
}

static wv s674_put(struct fr142 *up, wv v736_scope, wv v737_name, wv v738_kind, wv v739_d) {
  rt_enter("put");
  v736_scope = rt_bind(v736_scope, 0);
  v737_name = rt_bind(v737_name, 0);
  v738_kind = rt_bind(v738_kind, 0);
  v739_d = rt_bind(v739_d, 0);
  (void)({ wv t1185 = rt_get(&F_names, v736_scope); wv t1186 = rt_rec(0, 3, K49, (wv[]){ v737_name, v738_kind, v739_d }); std_push(t1185, t1186); });
  RT_RET(RT_NULL);
}

static wv s675_find(struct fr142 *up, wv v740_scope, wv v741_name) {
  rt_enter("find");
  v741_name = rt_bind(v741_name, 0);
  wv v742_s = v740_scope;
  while (rt_cond(rt_ne(v742_s, RT_NULL))) {
    wv v743_ns = rt_get(&F_names, v742_s);
    wv v744_k = rt_i(0);
    while (rt_cond(({ wv t1187 = v744_k; wv t1188 = std_length(v743_ns); rt_lt(t1187, t1188); }))) {
      if (rt_cond(({ wv t1189 = rt_get(&F_name, rt_idx(v744_k, v743_ns)); wv t1190 = v741_name; rt_eq(t1189, t1190); }))) {
        RT_RET(rt_idx(v744_k, v743_ns));
      }
      v744_k = rt_add(v744_k, rt_i(1));
    }
    v742_s = rt_get(&F_parent, v742_s);
  }
  RT_RET(RT_NULL);
}

static wv s676_kindOf(struct fr142 *up, wv v745_scope, wv v746_name) {
  rt_enter("kindOf");
  v745_scope = rt_bind(v745_scope, 0);
  v746_name = rt_bind(v746_name, 0);
  wv v747_entry = rt_bind(rt_chk(s675_find(up, v745_scope, v746_name), "kindOf"), 0);
  RT_RET((rt_cond(rt_eq(v747_entry, RT_NULL)) ? RT_NULL : rt_get(&F_kind, v747_entry)));
}

static wv s677_newFn(struct fr142 *up, wv v748_name, wv v749_parent) {
  rt_enter("newFn");
  v748_name = rt_bind(v748_name, 0);
  v749_parent = rt_bind(v749_parent, 0);
  wv v750_f = ({ wv t1193 = v748_name; wv t1194 = v749_parent; wv t1195 = (rt_cond(rt_eq(v749_parent, RT_NULL)) ? rt_i(0) : rt_add(rt_get(&F_depth, ({ wv t1191 = v749_parent; wv t1192 = rt_def(up->v662_fns, "fns"); rt_idx(t1191, t1192); })), rt_i(1))); wv t1196 = rt_eq(v749_parent, RT_NULL); rt_rec(1, 6, K50, (wv[]){ t1193, t1194, t1195, t1196, rt_i(0), rt_s(&S13) }); });
  (void)({ wv t1197 = rt_def(up->v662_fns, "fns"); wv t1198 = v750_f; std_push(t1197, t1198); });
  RT_RET(rt_sub(std_length(rt_def(up->v662_fns, "fns")), rt_i(1)));
}

static wv s678_bind(struct fr142 *up, wv v751_kind, wv v752_name, wv v753_f, wv v754_atTop, wv v755_mode) {
  rt_enter("bind");
  v751_kind = rt_bind(v751_kind, 0);
  v752_name = rt_bind(v752_name, 0);
  v753_f = rt_bind(v753_f, 0);
  v754_atTop = rt_bind(v754_atTop, 0);
  v755_mode = rt_bind(v755_mode, 0);
  wv v756_cname = ({ wv t1199 = rt_text(rt_def(up->v666_mod, "mod")); wv t1200 = rt_text(v752_name); rt_cat(4, (wv[]){ rt_s(&S505), t1199, rt_s(&S12), t1200 }); });
  if (rt_cond(rt_not(v754_atTop))) {
    v756_cname = ({ wv t1203 = (rt_cond(rt_eq(v751_kind, rt_s(&S33))) ? rt_s(&S307) : rt_s(&S506)); wv t1204 = ({ wv t1201 = rt_text(rt_def(up->v648_n, "n")); wv t1202 = rt_text(v752_name); rt_cat(3, (wv[]){ t1201, rt_s(&S12), t1202 }); }); rt_add(t1203, t1204); });
    up->v648_n = rt_add(rt_def(up->v648_n, "n"), rt_i(1));
  }
  wv v757_d = ({ wv t1205 = v751_kind; wv t1206 = v752_name; wv t1207 = v753_f; wv t1208 = v756_cname; wv t1209 = rt_eq(v755_mode, rt_s(&S37)); rt_rec(1, 7, K51, (wv[]){ t1205, t1206, t1207, t1208, t1209, rt_b(0), RT_NULL }); });
  (void)({ wv t1210 = rt_def(up->v661_decls, "decls"); wv t1211 = v757_d; std_push(t1210, t1211); });
  if (rt_cond(rt_b(rt_bool(rt_get(&F_top, ({ wv t1212 = v753_f; wv t1213 = rt_def(up->v662_fns, "fns"); rt_idx(t1212, t1213); })), "&&") && rt_bool(rt_ne(v751_kind, rt_s(&S33)), "&&")))) {
    (void)({ wv t1214 = rt_def(up->v654_globals, "globals"); wv t1215 = v756_cname; std_push(t1214, t1215); });
  }
  RT_RET(rt_sub(std_length(rt_def(up->v661_decls, "decls")), rt_i(1)));
}

static wv s679_ablock(struct fr142 *up, wv v758_b, wv v759_scope, wv v760_f, wv v761_topScope) {
  rt_enter("ablock");
  v759_scope = rt_bind(v759_scope, 0);
  v760_f = rt_bind(v760_f, 0);
  v761_topScope = rt_bind(v761_topScope, 0);
  wv v762_atTop = rt_eq(v759_scope, v761_topScope);
  wv v763_list = rt_bind(rt_get(&F_stmts, v758_b), 0);
  wv v764_k = rt_i(0);
  while (rt_cond(({ wv t1216 = v764_k; wv t1217 = std_length(v763_list); rt_lt(t1216, t1217); }))) {
    wv v765_s = rt_bind(rt_idx(v764_k, v763_list), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v765_s), rt_s(&S33)))) {
      wv v766_d = rt_bind(({ wv t1218 = rt_get(&F_name, v765_s); wv t1219 = v760_f; wv t1220 = v762_atTop; rt_chk(s678_bind(up, rt_s(&S33), t1218, t1219, t1220, rt_s(&S13)), "ablock"); }), 0);
      wv v767_child = rt_bind(({ wv t1221 = rt_get(&F_name, v765_s); wv t1222 = v760_f; rt_chk(s677_newFn(up, t1221, t1222), "ablock"); }), 0);
      ({ wv t1227 = rt_get(&F_cname, ({ wv t1223 = v766_d; wv t1224 = rt_def(up->v661_decls, "decls"); rt_idx(t1223, t1224); })); wv t1228 = ({ wv t1225 = v767_child; wv t1226 = rt_def(up->v662_fns, "fns"); rt_idx(t1225, t1226); }); rt_set_mem(t1227, &F_cname, t1228); });
      ({ wv t1231 = v767_child; wv t1232 = ({ wv t1229 = v766_d; wv t1230 = rt_def(up->v661_decls, "decls"); rt_idx(t1229, t1230); }); rt_set_mem(t1231, &F_fnIndex, t1232); });
      rt_upd_mem(rt_i(1), &F_children, ({ wv t1233 = v760_f; wv t1234 = rt_def(up->v662_fns, "fns"); rt_idx(t1233, t1234); }), rt_add);
      ({ wv t1235 = v766_d; wv t1236 = rt_get(&F_namePos, v765_s); wv t1237 = rt_def(up->v667_curSlot, "curSlot"); rt_set_idx(t1235, t1236, t1237); });
      (void)({ wv t1238 = v759_scope; wv t1239 = rt_get(&F_name, v765_s); wv t1240 = v766_d; rt_chk(s674_put(up, t1238, t1239, rt_s(&S33), t1240), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v765_s), rt_s(&S46)))) {
      (void)({ wv t1241 = v759_scope; wv t1242 = rt_get(&F_name, v765_s); rt_chk(s674_put(up, t1241, t1242, rt_s(&S46), RT_NULL), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v765_s), rt_s(&S177)))) {
      wv v768_d = rt_bind(({ wv t1243 = rt_get(&F_name, v765_s); wv t1244 = v760_f; wv t1245 = v762_atTop; wv t1246 = rt_chk(w6_modeOf(v765_s), "ablock"); rt_chk(s678_bind(up, rt_s(&S177), t1243, t1244, t1245, t1246), "ablock"); }), 0);
      ({ wv t1247 = v768_d; wv t1248 = rt_get(&F_namePos, v765_s); wv t1249 = rt_def(up->v667_curSlot, "curSlot"); rt_set_idx(t1247, t1248, t1249); });
      (void)({ wv t1250 = v759_scope; wv t1251 = rt_get(&F_name, v765_s); wv t1252 = v768_d; rt_chk(s674_put(up, t1250, t1251, rt_s(&S177), t1252), "ablock"); });
    }
    v764_k = rt_add(v764_k, rt_i(1));
  }
  v764_k = rt_i(0);
  while (rt_cond(({ wv t1253 = v764_k; wv t1254 = std_length(v763_list); rt_lt(t1253, t1254); }))) {
    (void)({ wv t1255 = rt_idx(v764_k, v763_list); wv t1256 = v759_scope; wv t1257 = v760_f; wv t1258 = v761_topScope; rt_chk(s680_astmt(up, t1255, t1256, t1257, t1258), "ablock"); });
    v764_k = rt_add(v764_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v758_b), RT_NULL))) {
    (void)({ wv t1259 = rt_get(&F_value, v758_b); wv t1260 = v759_scope; wv t1261 = v760_f; rt_chk(s684_aexpr(up, t1259, t1260, t1261), "ablock"); });
  }
  RT_RET(RT_NULL);
}

static wv s680_astmt(struct fr142 *up, wv v769_s, wv v770_scope, wv v771_f, wv v772_topScope) {
  rt_enter("astmt");
  v770_scope = rt_bind(v770_scope, 0);
  v771_f = rt_bind(v771_f, 0);
  v772_topScope = rt_bind(v772_topScope, 0);
  wv v773_kind = rt_bind(rt_get(&F_k, v769_s), 0);
  if (rt_cond(rt_eq(v773_kind, rt_s(&S33)))) {
    wv v774_d = rt_bind(({ wv t1262 = rt_get(&F_namePos, v769_s); wv t1263 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1262, t1263); }), 0);
    wv v775_child = rt_bind(rt_get(&F_fnIndex, ({ wv t1264 = v774_d; wv t1265 = rt_def(up->v661_decls, "decls"); rt_idx(t1264, t1265); })), 0);
    wv v776_ps = rt_bind(rt_chk(s673_newScope(up, v770_scope), "astmt"), 0);
    wv v777_k = rt_i(0);
    while (rt_cond(({ wv t1266 = v777_k; wv t1267 = std_length(rt_get(&F_params, v769_s)); rt_lt(t1266, t1267); }))) {
      wv v778_p = rt_bind(({ wv t1268 = v777_k; wv t1269 = rt_get(&F_params, v769_s); rt_idx(t1268, t1269); }), 0);
      wv v779_pd = rt_bind(({ wv t1270 = rt_get(&F_name, v778_p); wv t1271 = v775_child; wv t1272 = rt_chk(w6_modeOf(v778_p), "astmt"); rt_chk(s678_bind(up, rt_s(&S393), t1270, t1271, rt_b(0), t1272), "astmt"); }), 0);
      ({ wv t1273 = v779_pd; wv t1274 = rt_get(&F_pos, v778_p); wv t1275 = rt_def(up->v667_curSlot, "curSlot"); rt_set_idx(t1273, t1274, t1275); });
      (void)({ wv t1276 = v776_ps; wv t1277 = rt_get(&F_name, v778_p); wv t1278 = v779_pd; rt_chk(s674_put(up, t1276, t1277, rt_s(&S393), t1278), "astmt"); });
      v777_k = rt_add(v777_k, rt_i(1));
    }
    (void)({ wv t1279 = rt_get(&F_body, v769_s); wv t1280 = rt_chk(s673_newScope(up, v776_ps), "astmt"); wv t1281 = v775_child; wv t1282 = v772_topScope; rt_chk(s679_ablock(up, t1279, t1280, t1281, t1282), "astmt"); });
  } else if (rt_cond(rt_eq(v773_kind, rt_s(&S41)))) {
    (void)({ wv t1283 = rt_get(&F_cond, v769_s); wv t1284 = v770_scope; wv t1285 = v771_f; rt_chk(s684_aexpr(up, t1283, t1284, t1285), "astmt"); });
    (void)({ wv t1286 = rt_get(&F_body, v769_s); wv t1287 = rt_chk(s673_newScope(up, v770_scope), "astmt"); wv t1288 = v771_f; wv t1289 = v772_topScope; rt_chk(s679_ablock(up, t1286, t1287, t1288, t1289), "astmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v773_kind, rt_s(&S34)), "||") || rt_bool(rt_eq(v773_kind, rt_s(&S57)), "||")), "||") || rt_bool(rt_eq(v773_kind, rt_s(&S54)), "||")), "||") || rt_bool(rt_eq(v773_kind, rt_s(&S177)), "||")))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v769_s), RT_NULL))) {
      (void)({ wv t1290 = rt_get(&F_e, v769_s); wv t1291 = v770_scope; wv t1292 = v771_f; rt_chk(s684_aexpr(up, t1290, t1291, t1292), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v773_kind, rt_s(&S176)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v769_s)), rt_s(&S183)))) {
      (void)({ wv t1293 = rt_get(&F_arg, rt_get(&F_e, v769_s)); wv t1294 = v770_scope; wv t1295 = v771_f; rt_chk(s684_aexpr(up, t1293, t1294, t1295), "astmt"); });
    } else {
      (void)({ wv t1296 = rt_get(&F_e, v769_s); wv t1297 = v770_scope; wv t1298 = v771_f; rt_chk(s684_aexpr(up, t1296, t1297, t1298), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v773_kind, rt_s(&S38)))) {
    (void)({ wv t1299 = rt_get(&F_e, v769_s); wv t1300 = v770_scope; wv t1301 = v771_f; rt_chk(s684_aexpr(up, t1299, t1300, t1301), "astmt"); });
    (void)({ wv t1302 = rt_get(&F_target, v769_s); wv t1303 = v770_scope; wv t1304 = v771_f; rt_chk(s682_atarget(up, t1302, t1303, t1304), "astmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v769_s), RT_NULL))) {
    wv v780_h = rt_bind(rt_get(&F_handler, v769_s), 0);
    wv v781_hs = rt_bind(rt_chk(s673_newScope(up, v770_scope), "astmt"), 0);
    if (rt_cond(rt_ne(rt_get(&F_param, v780_h), RT_NULL))) {
      wv v782_wd = rt_bind(({ wv t1305 = rt_get(&F_param, v780_h); wv t1306 = v771_f; rt_chk(s678_bind(up, rt_s(&S56), t1305, t1306, rt_b(0), rt_s(&S13)), "astmt"); }), 0);
      ({ wv t1307 = v782_wd; wv t1308 = rt_get(&F_paramPos, v780_h); wv t1309 = rt_def(up->v667_curSlot, "curSlot"); rt_set_idx(t1307, t1308, t1309); });
      (void)({ wv t1310 = v781_hs; wv t1311 = rt_get(&F_param, v780_h); wv t1312 = v782_wd; rt_chk(s674_put(up, t1310, t1311, rt_s(&S56), t1312), "astmt"); });
    }
    (void)({ wv t1313 = rt_get(&F_body, v780_h); wv t1314 = rt_chk(s673_newScope(up, v781_hs), "astmt"); wv t1315 = v771_f; wv t1316 = v772_topScope; rt_chk(s679_ablock(up, t1313, t1314, t1315, t1316), "astmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s681_use(struct fr142 *up, wv v783_x, wv v784_scope, wv v785_f) {
  rt_enter("use");
  v783_x = rt_bind(v783_x, 0);
  v784_scope = rt_bind(v784_scope, 0);
  v785_f = rt_bind(v785_f, 0);
  wv v786_d = rt_bind(rt_get(&F_d, ({ wv t1317 = v784_scope; wv t1318 = rt_get(&F_name, v783_x); rt_chk(s675_find(up, t1317, t1318), "use"); })), 0);
  ({ wv t1319 = v786_d; wv t1320 = rt_get(&F_pos, v783_x); wv t1321 = rt_def(up->v667_curSlot, "curSlot"); rt_set_idx(t1319, t1320, t1321); });
  if (rt_cond(({ wv t1324 = rt_get(&F_func, ({ wv t1322 = v786_d; wv t1323 = rt_def(up->v661_decls, "decls"); rt_idx(t1322, t1323); })); wv t1325 = v785_f; rt_ne(t1324, t1325); }))) {
    rt_set_mem(rt_b(1), &F_captured, ({ wv t1326 = v786_d; wv t1327 = rt_def(up->v661_decls, "decls"); rt_idx(t1326, t1327); }));
  }
  RT_RET(RT_NULL);
}

static wv s682_atarget(struct fr142 *up, wv v787_t, wv v788_scope, wv v789_f) {
  rt_enter("atarget");
  v787_t = rt_bind(v787_t, 0);
  v788_scope = rt_bind(v788_scope, 0);
  v789_f = rt_bind(v789_f, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v787_t), rt_s(&S212)))) {
    (void)rt_chk(s681_use(up, v787_t, v788_scope, v789_f), "atarget");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v787_t), rt_s(&S189)))) {
    (void)({ wv t1328 = rt_get(&F_index, v787_t); wv t1329 = v788_scope; wv t1330 = v789_f; rt_chk(s684_aexpr(up, t1328, t1329, t1330), "atarget"); });
  }
  (void)({ wv t1331 = (rt_cond(rt_eq(rt_get(&F_k, v787_t), rt_s(&S190))) ? rt_get(&F_rec, v787_t) : rt_get(&F_arr, v787_t)); wv t1332 = v788_scope; wv t1333 = v789_f; rt_chk(s684_aexpr(up, t1331, t1332, t1333), "atarget"); });
  RT_RET(RT_NULL);
}

static wv s683_notValue(struct fr142 *up, wv v790_x, wv v791_scope) {
  rt_enter("notValue");
  v791_scope = rt_bind(v791_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v790_x), rt_s(&S190)))) {
    RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v790_x)), rt_s(&S212)), "&&") && rt_bool(rt_eq(({ wv t1334 = v791_scope; wv t1335 = rt_get(&F_name, rt_get(&F_rec, v790_x)); rt_chk(s676_kindOf(up, t1334, t1335), "notValue"); }), rt_s(&S408)), "&&")));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v790_x), rt_s(&S212)))) {
    RT_RET(rt_b(0));
  }
  wv v792_kind = rt_bind(({ wv t1336 = v791_scope; wv t1337 = rt_get(&F_name, v790_x); rt_chk(s676_kindOf(up, t1336, t1337), "notValue"); }), 0);
  RT_RET(rt_b(rt_bool(rt_eq(v792_kind, rt_s(&S46)), "||") || rt_bool(rt_eq(v792_kind, rt_s(&S408)), "||")));
}

static wv s684_aexpr(struct fr142 *up, wv v793_e, wv v794_scope, wv v795_f) {
  rt_enter("aexpr");
  v794_scope = rt_bind(v794_scope, 0);
  v795_f = rt_bind(v795_f, 0);
  wv v796_kind = rt_bind(rt_get(&F_k, v793_e), 0);
  if (rt_cond(rt_eq(v796_kind, rt_s(&S171)))) {
    wv v797_k = rt_i(0);
    while (rt_cond(({ wv t1338 = v797_k; wv t1339 = std_length(rt_get(&F_parts, v793_e)); rt_lt(t1338, t1339); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t1340 = v797_k; wv t1341 = rt_get(&F_parts, v793_e); rt_idx(t1340, t1341); })), rt_s(&S185)))) {
        (void)({ wv t1344 = ({ wv t1342 = v797_k; wv t1343 = rt_get(&F_parts, v793_e); rt_idx(t1342, t1343); }); wv t1345 = v794_scope; wv t1346 = v795_f; rt_chk(s684_aexpr(up, t1344, t1345, t1346), "aexpr"); });
      }
      v797_k = rt_add(v797_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S212)))) {
    (void)rt_chk(s681_use(up, v793_e, v794_scope, v795_f), "aexpr");
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v796_kind, rt_s(&S186)), "||") || rt_bool(rt_eq(v796_kind, rt_s(&S187)), "||")))) {
    wv v798_k = rt_i(0);
    while (rt_cond(({ wv t1347 = v798_k; wv t1348 = std_length(rt_get(&F_items, v793_e)); rt_lt(t1347, t1348); }))) {
      (void)({ wv t1351 = ({ wv t1349 = v798_k; wv t1350 = rt_get(&F_items, v793_e); rt_idx(t1349, t1350); }); wv t1352 = v794_scope; wv t1353 = v795_f; rt_chk(s684_aexpr(up, t1351, t1352, t1353), "aexpr"); });
      v798_k = rt_add(v798_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S188)))) {
    wv v799_k = rt_i(0);
    while (rt_cond(({ wv t1354 = v799_k; wv t1355 = std_length(rt_get(&F_fields, v793_e)); rt_lt(t1354, t1355); }))) {
      (void)({ wv t1358 = rt_get(&F_e, ({ wv t1356 = v799_k; wv t1357 = rt_get(&F_fields, v793_e); rt_idx(t1356, t1357); })); wv t1359 = v794_scope; wv t1360 = v795_f; rt_chk(s684_aexpr(up, t1358, t1359, t1360), "aexpr"); });
      v799_k = rt_add(v799_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S189)))) {
    (void)({ wv t1361 = rt_get(&F_index, v793_e); wv t1362 = v794_scope; wv t1363 = v795_f; rt_chk(s684_aexpr(up, t1361, t1362, t1363), "aexpr"); });
    (void)({ wv t1364 = rt_get(&F_arr, v793_e); wv t1365 = v794_scope; wv t1366 = v795_f; rt_chk(s684_aexpr(up, t1364, t1365, t1366), "aexpr"); });
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S190)))) {
    if (rt_cond(rt_not(({ wv t1367 = rt_get(&F_rec, v793_e); wv t1368 = v794_scope; rt_chk(s683_notValue(up, t1367, t1368), "aexpr"); })))) {
      (void)({ wv t1369 = rt_get(&F_rec, v793_e); wv t1370 = v794_scope; wv t1371 = v795_f; rt_chk(s684_aexpr(up, t1369, t1370, t1371), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S191)))) {
    (void)({ wv t1372 = rt_get(&F_e, v793_e); wv t1373 = v794_scope; wv t1374 = v795_f; rt_chk(s684_aexpr(up, t1372, t1373, t1374), "aexpr"); });
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S192)))) {
    (void)({ wv t1375 = rt_get(&F_l, v793_e); wv t1376 = v794_scope; wv t1377 = v795_f; rt_chk(s684_aexpr(up, t1375, t1376, t1377), "aexpr"); });
    (void)({ wv t1378 = rt_get(&F_r, v793_e); wv t1379 = v794_scope; wv t1380 = v795_f; rt_chk(s684_aexpr(up, t1378, t1379, t1380), "aexpr"); });
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S193)))) {
    (void)({ wv t1381 = rt_get(&F_c, v793_e); wv t1382 = v794_scope; wv t1383 = v795_f; rt_chk(s684_aexpr(up, t1381, t1382, t1383), "aexpr"); });
    (void)({ wv t1384 = rt_get(&F_a, v793_e); wv t1385 = v794_scope; wv t1386 = v795_f; rt_chk(s684_aexpr(up, t1384, t1385, t1386), "aexpr"); });
    (void)({ wv t1387 = rt_get(&F_b, v793_e); wv t1388 = v794_scope; wv t1389 = v795_f; rt_chk(s684_aexpr(up, t1387, t1388, t1389), "aexpr"); });
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S182)))) {
    wv v800_k = rt_i(0);
    while (rt_cond(({ wv t1390 = v800_k; wv t1391 = std_length(rt_get(&F_args, v793_e)); rt_lt(t1390, t1391); }))) {
      (void)({ wv t1394 = ({ wv t1392 = v800_k; wv t1393 = rt_get(&F_args, v793_e); rt_idx(t1392, t1393); }); wv t1395 = v794_scope; wv t1396 = v795_f; rt_chk(s684_aexpr(up, t1394, t1395, t1396), "aexpr"); });
      v800_k = rt_add(v800_k, rt_i(1));
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_callee, v793_e)), rt_s(&S212)))) {
      ({ wv t1399 = rt_get(&F_d, ({ wv t1397 = v794_scope; wv t1398 = rt_get(&F_name, rt_get(&F_callee, v793_e)); rt_chk(s675_find(up, t1397, t1398), "aexpr"); })); wv t1400 = rt_get(&F_pos, rt_get(&F_callee, v793_e)); wv t1401 = rt_def(up->v667_curSlot, "curSlot"); rt_set_idx(t1399, t1400, t1401); });
    }
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S184)))) {
    wv v801_k = rt_i(0);
    while (rt_cond(({ wv t1402 = v801_k; wv t1403 = std_length(rt_get(&F_branches, v793_e)); rt_lt(t1402, t1403); }))) {
      (void)({ wv t1406 = rt_get(&F_cond, ({ wv t1404 = v801_k; wv t1405 = rt_get(&F_branches, v793_e); rt_idx(t1404, t1405); })); wv t1407 = v794_scope; wv t1408 = v795_f; rt_chk(s684_aexpr(up, t1406, t1407, t1408), "aexpr"); });
      (void)({ wv t1411 = rt_get(&F_body, ({ wv t1409 = v801_k; wv t1410 = rt_get(&F_branches, v793_e); rt_idx(t1409, t1410); })); wv t1412 = rt_chk(s673_newScope(up, v794_scope), "aexpr"); wv t1413 = v795_f; rt_chk(s679_ablock(up, t1411, t1412, t1413, RT_NULL), "aexpr"); });
      v801_k = rt_add(v801_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v793_e), RT_NULL))) {
      (void)({ wv t1414 = rt_get(&F_els, v793_e); wv t1415 = rt_chk(s673_newScope(up, v794_scope), "aexpr"); wv t1416 = v795_f; rt_chk(s679_ablock(up, t1414, t1415, t1416, RT_NULL), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v796_kind, rt_s(&S44)))) {
    (void)({ wv t1417 = rt_get(&F_subject, v793_e); wv t1418 = v794_scope; wv t1419 = v795_f; rt_chk(s684_aexpr(up, t1417, t1418, t1419), "aexpr"); });
    wv v802_k = rt_i(0);
    while (rt_cond(({ wv t1420 = v802_k; wv t1421 = std_length(rt_get(&F_cases, v793_e)); rt_lt(t1420, t1421); }))) {
      (void)({ wv t1424 = rt_get(&F_body, ({ wv t1422 = v802_k; wv t1423 = rt_get(&F_cases, v793_e); rt_idx(t1422, t1423); })); wv t1425 = rt_chk(s673_newScope(up, v794_scope), "aexpr"); wv t1426 = v795_f; rt_chk(s679_ablock(up, t1424, t1425, t1426, RT_NULL), "aexpr"); });
      v802_k = rt_add(v802_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v793_e), RT_NULL))) {
      (void)({ wv t1427 = rt_get(&F_els, v793_e); wv t1428 = rt_chk(s673_newScope(up, v794_scope), "aexpr"); wv t1429 = v795_f; rt_chk(s679_ablock(up, t1427, t1428, t1429, RT_NULL), "aexpr"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv s685_analyze(struct fr142 *up, wv v803_m) {
  rt_enter("analyze");
  v803_m = rt_bind(v803_m, 0);
  wv t1430 = rt_bind(v803_m, 1);
  (void)rt_def(up->v665_curMod, "curMod");
  up->v665_curMod = t1430;
  wv t1431 = rt_bind(rt_chk(s671_modId(up, v803_m), "analyze"), 1);
  (void)rt_def(up->v666_mod, "mod");
  up->v666_mod = t1431;
  wv v804_top = rt_bind(rt_chk(s677_newFn(up, RT_NULL, RT_NULL), "analyze"), 0);
  (void)({ wv t1432 = rt_def(up->v663_tops, "tops"); wv t1433 = v804_top; std_push(t1432, t1433); });
  wv v805_slot = rt_arr(0, NULL, 1);
  wv v806_i = rt_i(0);
  while (rt_cond(({ wv t1434 = v806_i; wv t1435 = std_length(rt_get(&F_chars, rt_idx(v803_m, up->v646_modules))); rt_le(t1434, t1435); }))) {
    (void)std_push(v805_slot, RT_NULL);
    v806_i = rt_add(v806_i, rt_i(1));
  }
  (void)({ wv t1436 = rt_def(up->v664_slots, "slots"); wv t1437 = v805_slot; std_push(t1436, t1437); });
  wv t1438 = rt_bind(v805_slot, 1);
  (void)rt_def(up->v667_curSlot, "curSlot");
  up->v667_curSlot = t1438;
  wv v807_scope = rt_bind(rt_chk(s673_newScope(up, RT_NULL), "analyze"), 0);
  wv v808_imports = rt_bind(rt_get(&F_imports, rt_idx(v803_m, up->v646_modules)), 0);
  wv v809_k = rt_i(0);
  while (rt_cond(({ wv t1439 = v809_k; wv t1440 = std_length(v808_imports); rt_lt(t1439, t1440); }))) {
    (void)({ wv t1441 = v807_scope; wv t1442 = rt_get(&F_name, rt_get(&F_node, rt_idx(v809_k, v808_imports))); rt_chk(s674_put(up, t1441, t1442, rt_s(&S408), RT_NULL), "analyze"); });
    v809_k = rt_add(v809_k, rt_i(1));
  }
  (void)({ wv t1443 = rt_rec(0, 2, K48, (wv[]){ rt_get(&F_stmts, rt_get(&F_program, rt_idx(v803_m, up->v646_modules))), RT_NULL }); wv t1444 = v807_scope; wv t1445 = v804_top; wv t1446 = v807_scope; rt_chk(s679_ablock(up, t1443, t1444, t1445, t1446), "analyze"); });
  RT_RET(RT_NULL);
}

static wv s686_enter(struct fr142 *up, wv v810_m) {
  rt_enter("enter");
  v810_m = rt_bind(v810_m, 0);
  (void)({ wv t1447 = rt_get(&F_file, rt_idx(v810_m, up->v646_modules)); wv t1448 = rt_get(&F_chars, rt_idx(v810_m, up->v646_modules)); rt_chk(w2_setSource(t1447, t1448), "enter"); });
  wv t1449 = rt_bind(v810_m, 1);
  (void)rt_def(up->v665_curMod, "curMod");
  up->v665_curMod = t1449;
  wv t1450 = rt_bind(rt_chk(s671_modId(up, v810_m), "enter"), 1);
  (void)rt_def(up->v666_mod, "mod");
  up->v666_mod = t1450;
  wv t1453 = rt_bind(({ wv t1451 = v810_m; wv t1452 = rt_def(up->v664_slots, "slots"); rt_idx(t1451, t1452); }), 1);
  (void)rt_def(up->v667_curSlot, "curSlot");
  up->v667_curSlot = t1453;
  wv t1456 = rt_bind(({ wv t1454 = v810_m; wv t1455 = rt_def(up->v663_tops, "tops"); rt_idx(t1454, t1455); }), 1);
  (void)rt_def(up->v670_cur, "cur");
  up->v670_cur = t1456;
  wv t1457 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v668_stolen, "stolen");
  up->v668_stolen = t1457;
  wv t1458 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v669_ownEnums, "ownEnums");
  up->v669_ownEnums = t1458;
  wv v811_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v810_m, up->v646_modules))), 0);
  wv v812_k = rt_i(0);
  while (rt_cond(({ wv t1459 = v812_k; wv t1460 = std_length(v811_list); rt_lt(t1459, t1460); }))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v812_k, v811_list)), rt_s(&S46)))) {
      (void)({ wv t1461 = rt_def(up->v669_ownEnums, "ownEnums"); wv t1462 = rt_idx(v812_k, v811_list); std_push(t1461, t1462); });
    }
    v812_k = rt_add(v812_k, rt_i(1));
  }
  wv v813_imports = rt_bind(rt_get(&F_imports, rt_idx(v810_m, up->v646_modules)), 0);
  v812_k = rt_i(0);
  while (rt_cond(({ wv t1463 = v812_k; wv t1464 = std_length(v813_imports); rt_lt(t1463, t1464); }))) {
    (void)({ wv t1467 = rt_def(up->v668_stolen, "stolen"); wv t1468 = ({ wv t1465 = rt_get(&F_name, rt_get(&F_node, rt_idx(v812_k, v813_imports))); wv t1466 = rt_get(&F_lib, rt_idx(v812_k, v813_imports)); rt_rec(0, 2, K52, (wv[]){ t1465, t1466 }); }); std_push(t1467, t1468); });
    v812_k = rt_add(v812_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s687_emit(struct fr142 *up, wv v814_line) {
  rt_enter("emit");
  v814_line = rt_bind(v814_line, 0);
  (void)({ wv t1471 = rt_def(up->v657_out, "out"); wv t1472 = ({ wv t1469 = rt_chk(w6_repeat(rt_s(&S507), rt_def(up->v658_indent, "indent")), "emit"); wv t1470 = v814_line; rt_add(t1469, t1470); }); std_push(t1471, t1472); });
  RT_RET(RT_NULL);
}

static wv s688_tmp(struct fr142 *up) {
  rt_enter("tmp");
  wv v815_name = rt_cat(2, (wv[]){ rt_s(&S104), rt_text(rt_def(up->v649_tmps, "tmps")) });
  up->v649_tmps = rt_add(rt_def(up->v649_tmps, "tmps"), rt_i(1));
  RT_RET(v815_name);
}

static wv s689_str(struct fr142 *up, wv v816_s) {
  rt_enter("str");
  v816_s = rt_bind(v816_s, 0);
  wv v817_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1473 = v817_k; wv t1474 = std_length(rt_def(up->v650_strings, "strings")); rt_lt(t1473, t1474); }), "&&") && rt_bool(({ wv t1477 = ({ wv t1475 = v817_k; wv t1476 = rt_def(up->v650_strings, "strings"); rt_idx(t1475, t1476); }); wv t1478 = v816_s; rt_ne(t1477, t1478); }), "&&")))) {
    v817_k = rt_add(v817_k, rt_i(1));
  }
  if (rt_cond(({ wv t1479 = v817_k; wv t1480 = std_length(rt_def(up->v650_strings, "strings")); rt_eq(t1479, t1480); }))) {
    (void)({ wv t1481 = rt_def(up->v650_strings, "strings"); wv t1482 = v816_s; std_push(t1481, t1482); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S508), rt_text(v817_k) }));
}

static wv s690_fieldRef(struct fr142 *up, wv v818_name) {
  rt_enter("fieldRef");
  v818_name = rt_bind(v818_name, 0);
  if (rt_cond(rt_not(({ wv t1483 = rt_def(up->v651_fields, "fields"); wv t1484 = v818_name; rt_chk(w1_contains(t1483, t1484), "fieldRef"); })))) {
    (void)({ wv t1485 = rt_def(up->v651_fields, "fields"); wv t1486 = v818_name; std_push(t1485, t1486); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S509), rt_text(v818_name) }));
}

static wv s691_enumDef(struct fr142 *up, wv v819_s, wv v820_m) {
  rt_enter("enumDef");
  v819_s = rt_bind(v819_s, 0);
  v820_m = rt_bind(v820_m, 0);
  wv v821_nm = rt_bind(rt_chk(s689_str(up, rt_get(&F_name, v819_s)), "enumDef"), 0);
  wv v822_members = rt_arr(0, NULL, 1);
  wv v823_k = rt_i(0);
  while (rt_cond(({ wv t1487 = v823_k; wv t1488 = std_length(rt_get(&F_members, v819_s)); rt_lt(t1487, t1488); }))) {
    (void)({ wv t1493 = v822_members; wv t1494 = ({ wv t1491 = rt_text(v821_nm); wv t1492 = rt_text(rt_chk(s689_str(up, rt_get(&F_name, ({ wv t1489 = v823_k; wv t1490 = rt_get(&F_members, v819_s); rt_idx(t1489, t1490); }))), "enumDef")); rt_cat(5, (wv[]){ rt_s(&S500), t1491, rt_s(&S405), t1492, rt_s(&S502) }); }); std_push(t1493, t1494); });
    v823_k = rt_add(v823_k, rt_i(1));
  }
  (void)({ wv t1498 = rt_def(up->v653_enumDefs, "enumDefs"); wv t1499 = ({ wv t1495 = rt_text(v820_m); wv t1496 = rt_text(rt_get(&F_name, v819_s)); wv t1497 = rt_text(std_join(v822_members, rt_s(&S405))); rt_cat(7, (wv[]){ rt_s(&S510), t1495, rt_s(&S12), t1496, rt_s(&S511), t1497, rt_s(&S512) }); }); std_push(t1498, t1499); });
  RT_RET(RT_NULL);
}

static wv s692_stolenFrom(struct fr142 *up, wv v824_x) {
  rt_enter("stolenFrom");
  if (rt_cond(rt_ne(rt_get(&F_k, v824_x), rt_s(&S212)))) {
    RT_RET(RT_NULL);
  }
  wv v825_k = rt_i(0);
  while (rt_cond(({ wv t1500 = v825_k; wv t1501 = std_length(rt_def(up->v668_stolen, "stolen")); rt_lt(t1500, t1501); }))) {
    if (rt_cond(({ wv t1504 = rt_get(&F_name, ({ wv t1502 = v825_k; wv t1503 = rt_def(up->v668_stolen, "stolen"); rt_idx(t1502, t1503); })); wv t1505 = rt_get(&F_name, v824_x); rt_eq(t1504, t1505); }))) {
      RT_RET(rt_get(&F_lib, ({ wv t1506 = v825_k; wv t1507 = rt_def(up->v668_stolen, "stolen"); rt_idx(t1506, t1507); })));
    }
    v825_k = rt_add(v825_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s693_enumIn(struct fr142 *up, wv v826_list, wv v827_name) {
  rt_enter("enumIn");
  v827_name = rt_bind(v827_name, 0);
  wv v828_k = rt_i(0);
  while (rt_cond(({ wv t1508 = v828_k; wv t1509 = std_length(v826_list); rt_lt(t1508, t1509); }))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_idx(v828_k, v826_list)), rt_s(&S46)), "&&") && rt_bool(({ wv t1510 = rt_get(&F_name, rt_idx(v828_k, v826_list)); wv t1511 = v827_name; rt_eq(t1510, t1511); }), "&&")))) {
      RT_RET(rt_idx(v828_k, v826_list));
    }
    v828_k = rt_add(v828_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s694_enumOf(struct fr142 *up, wv v829_x) {
  rt_enter("enumOf");
  if (rt_cond(rt_eq(rt_get(&F_k, v829_x), rt_s(&S212)))) {
    wv v833_s = rt_bind(({ wv t1512 = rt_def(up->v669_ownEnums, "ownEnums"); wv t1513 = rt_get(&F_name, v829_x); rt_chk(s693_enumIn(up, t1512, t1513), "enumOf"); }), 0);
    RT_RET((rt_cond(rt_eq(v833_s, RT_NULL)) ? RT_NULL : ({ wv t1516 = ({ wv t1514 = rt_text(rt_def(up->v666_mod, "mod")); wv t1515 = rt_text(rt_get(&F_name, v829_x)); rt_cat(4, (wv[]){ rt_s(&S513), t1514, rt_s(&S12), t1515 }); }); wv t1517 = v833_s; rt_rec(0, 2, K53, (wv[]){ t1516, t1517 }); })));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v829_x), rt_s(&S190)))) {
    RT_RET(RT_NULL);
  }
  wv v830_lib = rt_bind(rt_chk(s692_stolenFrom(up, rt_get(&F_rec, v829_x)), "enumOf"), 0);
  if (rt_cond(rt_eq(v830_lib, RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v831_xs = rt_bind(rt_get(&F_exports, rt_idx(v830_lib, up->v646_modules)), 0);
  wv v832_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1518 = v832_k; wv t1519 = std_length(v831_xs); rt_lt(t1518, t1519); }), "&&") && rt_bool(({ wv t1520 = rt_get(&F_name, rt_idx(v832_k, v831_xs)); wv t1521 = rt_get(&F_field, v829_x); rt_ne(t1520, t1521); }), "&&")))) {
    v832_k = rt_add(v832_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1522 = v832_k; wv t1523 = std_length(v831_xs); rt_eq(t1522, t1523); }), "||") || rt_bool(rt_ne(rt_get(&F_kind, rt_idx(v832_k, v831_xs)), rt_s(&S46)), "||")))) {
    RT_RET(RT_NULL);
  }
  RT_RET(({ wv t1528 = ({ wv t1524 = rt_text(rt_chk(s671_modId(up, v830_lib), "enumOf")); wv t1525 = rt_text(rt_get(&F_field, v829_x)); rt_cat(4, (wv[]){ rt_s(&S513), t1524, rt_s(&S12), t1525 }); }); wv t1529 = ({ wv t1526 = rt_get(&F_stmts, rt_get(&F_program, rt_idx(v830_lib, up->v646_modules))); wv t1527 = rt_get(&F_field, v829_x); rt_chk(s693_enumIn(up, t1526, t1527), "enumOf"); }); rt_rec(0, 2, K53, (wv[]){ t1528, t1529 }); }));
}

static wv s695_place(struct fr142 *up, wv v834_d) {
  rt_enter("place");
  v834_d = rt_bind(v834_d, 0);
  wv v835_owner = rt_bind(rt_get(&F_func, ({ wv t1530 = v834_d; wv t1531 = rt_def(up->v661_decls, "decls"); rt_idx(t1530, t1531); })), 0);
  if (rt_cond(rt_get(&F_top, ({ wv t1532 = v835_owner; wv t1533 = rt_def(up->v662_fns, "fns"); rt_idx(t1532, t1533); })))) {
    RT_RET(rt_get(&F_cname, ({ wv t1534 = v834_d; wv t1535 = rt_def(up->v661_decls, "decls"); rt_idx(t1534, t1535); })));
  }
  if (rt_cond(({ wv t1536 = v835_owner; wv t1537 = rt_def(up->v670_cur, "cur"); rt_eq(t1536, t1537); }))) {
    RT_RET((rt_cond(rt_get(&F_captured, ({ wv t1538 = v834_d; wv t1539 = rt_def(up->v661_decls, "decls"); rt_idx(t1538, t1539); }))) ? rt_cat(2, (wv[]){ rt_s(&S514), rt_text(rt_get(&F_cname, ({ wv t1540 = v834_d; wv t1541 = rt_def(up->v661_decls, "decls"); rt_idx(t1540, t1541); }))) }) : rt_get(&F_cname, ({ wv t1542 = v834_d; wv t1543 = rt_def(up->v661_decls, "decls"); rt_idx(t1542, t1543); }))));
  }
  RT_RET(({ wv t1552 = rt_text(rt_chk(w6_repeat(rt_s(&S516), rt_sub(({ wv t1548 = rt_get(&F_depth, ({ wv t1544 = rt_def(up->v670_cur, "cur"); wv t1545 = rt_def(up->v662_fns, "fns"); rt_idx(t1544, t1545); })); wv t1549 = rt_get(&F_depth, ({ wv t1546 = v835_owner; wv t1547 = rt_def(up->v662_fns, "fns"); rt_idx(t1546, t1547); })); rt_sub(t1548, t1549); }), rt_i(1))), "place")); wv t1553 = rt_text(rt_get(&F_cname, ({ wv t1550 = v834_d; wv t1551 = rt_def(up->v661_decls, "decls"); rt_idx(t1550, t1551); }))); rt_cat(4, (wv[]){ rt_s(&S515), t1552, rt_s(&S517), t1553 }); }));
}

static wv s696_tdz(struct fr142 *up, wv v836_d) {
  rt_enter("tdz");
  v836_d = rt_bind(v836_d, 0);
  RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_kind, ({ wv t1554 = v836_d; wv t1555 = rt_def(up->v661_decls, "decls"); rt_idx(t1554, t1555); })), rt_s(&S177)), "&&") && rt_bool(({ wv t1558 = rt_get(&F_func, ({ wv t1556 = v836_d; wv t1557 = rt_def(up->v661_decls, "decls"); rt_idx(t1556, t1557); })); wv t1559 = rt_def(up->v670_cur, "cur"); rt_ne(t1558, t1559); }), "&&")));
}

static wv s697_read(struct fr142 *up, wv v837_d) {
  rt_enter("read");
  v837_d = rt_bind(v837_d, 0);
  RT_RET((rt_cond(rt_chk(s696_tdz(up, v837_d), "read")) ? ({ wv t1564 = ({ wv t1562 = rt_text(rt_chk(s695_place(up, v837_d), "read")); wv t1563 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1560 = v837_d; wv t1561 = rt_def(up->v661_decls, "decls"); rt_idx(t1560, t1561); }))), "read")); rt_cat(5, (wv[]){ rt_s(&S518), t1562, rt_s(&S405), t1563, rt_s(&S16) }); }); wv t1565 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1564, t1565 }); }) : ({ wv t1566 = rt_chk(s695_place(up, v837_d), "read"); wv t1567 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K54, (wv[]){ t1566, t1567 }); })));
}

static wv s698_seq(struct fr142 *up, wv v838_args) {
  rt_enter("seq");
  v838_args = rt_bind(v838_args, 0);
  wv v839_effects = rt_i(0);
  wv v840_anyPure = rt_b(0);
  wv v841_k = rt_i(0);
  while (rt_cond(({ wv t1568 = v841_k; wv t1569 = std_length(v838_args); rt_lt(t1568, t1569); }))) {
    if (rt_cond(({ wv t1570 = rt_get(&F_k, rt_idx(v841_k, v838_args)); wv t1571 = rt_def(w6_effectK, "effectK"); rt_eq(t1570, t1571); }))) {
      v839_effects = rt_add(v839_effects, rt_i(1));
    }
    if (rt_cond(({ wv t1572 = rt_get(&F_k, rt_idx(v841_k, v838_args)); wv t1573 = rt_def(w6_pureK, "pureK"); rt_eq(t1572, t1573); }))) {
      v840_anyPure = rt_b(1);
    }
    v841_k = rt_add(v841_k, rt_i(1));
  }
  wv v842_cs = rt_arr(0, NULL, 1);
  if (rt_cond(rt_b(rt_bool(rt_eq(v839_effects, rt_i(0)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v839_effects, rt_i(1)), "&&") && rt_bool(rt_not(v840_anyPure), "&&")), "||")))) {
    v841_k = rt_i(0);
    while (rt_cond(({ wv t1574 = v841_k; wv t1575 = std_length(v838_args); rt_lt(t1574, t1575); }))) {
      (void)({ wv t1576 = v842_cs; wv t1577 = rt_get(&F_c, rt_idx(v841_k, v838_args)); std_push(t1576, t1577); });
      v841_k = rt_add(v841_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S13), v842_cs }));
  }
  wv v843_pre = rt_arr(0, NULL, 1);
  v841_k = rt_i(0);
  while (rt_cond(({ wv t1578 = v841_k; wv t1579 = std_length(v838_args); rt_lt(t1578, t1579); }))) {
    if (rt_cond(({ wv t1580 = rt_get(&F_k, rt_idx(v841_k, v838_args)); wv t1581 = rt_def(w6_constK, "constK"); rt_eq(t1580, t1581); }))) {
      (void)({ wv t1582 = v842_cs; wv t1583 = rt_get(&F_c, rt_idx(v841_k, v838_args)); std_push(t1582, t1583); });
    } else {
      wv v844_temp = rt_bind(rt_chk(s688_tmp(up), "seq"), 0);
      (void)({ wv t1586 = v843_pre; wv t1587 = ({ wv t1584 = rt_text(v844_temp); wv t1585 = rt_text(rt_get(&F_c, rt_idx(v841_k, v838_args))); rt_cat(5, (wv[]){ rt_s(&S519), t1584, rt_s(&S520), t1585, rt_s(&S14) }); }); std_push(t1586, t1587); });
      (void)std_push(v842_cs, v844_temp);
    }
    v841_k = rt_add(v841_k, rt_i(1));
  }
  RT_RET(({ wv t1588 = std_join(v843_pre, rt_s(&S0)); wv t1589 = v842_cs; rt_rec(0, 2, K55, (wv[]){ t1588, t1589 }); }));
}

static wv s699_wrap(struct fr142 *up, wv v845_sq, wv v846_inner) {
  rt_enter("wrap");
  v845_sq = rt_bind(v845_sq, 0);
  v846_inner = rt_bind(v846_inner, 0);
  RT_RET(({ wv t1592 = (rt_cond(rt_eq(rt_get(&F_pre, v845_sq), rt_s(&S13))) ? v846_inner : ({ wv t1590 = rt_text(rt_get(&F_pre, v845_sq)); wv t1591 = rt_text(v846_inner); rt_cat(5, (wv[]){ rt_s(&S521), t1590, rt_s(&S0), t1591, rt_s(&S522) }); })); wv t1593 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1592, t1593 }); }));
}

static wv s700_stmts(struct fr142 *up, wv v847_list) {
  rt_enter("stmts");
  v847_list = rt_bind(v847_list, 0);
  wv v848_k = rt_i(0);
  while (rt_cond(({ wv t1594 = v848_k; wv t1595 = std_length(v847_list); rt_lt(t1594, t1595); }))) {
    (void)rt_chk(s704_stmt(up, rt_idx(v848_k, v847_list)), "stmts");
    v848_k = rt_add(v848_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s701_resets(struct fr142 *up, wv v849_b) {
  rt_enter("resets");
  wv v850_k = rt_i(0);
  while (rt_cond(({ wv t1596 = v850_k; wv t1597 = std_length(rt_get(&F_stmts, v849_b)); rt_lt(t1596, t1597); }))) {
    wv v851_s = ({ wv t1598 = v850_k; wv t1599 = rt_get(&F_stmts, v849_b); rt_idx(t1598, t1599); });
    if (rt_cond(rt_eq(rt_get(&F_k, v851_s), rt_s(&S177)))) {
      wv v852_d = rt_bind(({ wv t1600 = rt_get(&F_namePos, v851_s); wv t1601 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1600, t1601); }), 0);
      if (rt_cond(rt_get(&F_captured, ({ wv t1602 = v852_d; wv t1603 = rt_def(up->v661_decls, "decls"); rt_idx(t1602, t1603); })))) {
        (void)rt_chk(s687_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s695_place(up, v852_d), "resets")), rt_s(&S523) })), "resets");
      }
    }
    v850_k = rt_add(v850_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s702_blockBody(struct fr142 *up, wv v853_b) {
  rt_enter("blockBody");
  v853_b = rt_bind(v853_b, 0);
  (void)rt_chk(s701_resets(up, v853_b), "blockBody");
  (void)rt_chk(s700_stmts(up, rt_get(&F_stmts, v853_b)), "blockBody");
  if (rt_cond(rt_ne(rt_get(&F_value, v853_b), RT_NULL))) {
    (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S524), rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_value, v853_b)), "blockBody"))), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s703_valueOf(struct fr142 *up, wv v854_b) {
  rt_enter("valueOf");
  v854_b = rt_bind(v854_b, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_value, v854_b), RT_NULL)) ? rt_s(&S525) : rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_value, v854_b)), "valueOf"))));
}

static wv s704_stmt(struct fr142 *up, wv v855_s) {
  rt_enter("stmt");
  if (rt_cond(rt_ne(rt_get(&F_handler, v855_s), RT_NULL))) {
    (void)rt_chk(s714_onfailStmt(up, v855_s), "stmt");
    RT_RET(RT_NULL);
  }
  wv v856_kind = rt_bind(rt_get(&F_k, v855_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v856_kind, rt_s(&S201)), "||") || rt_bool(rt_eq(v856_kind, rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(v856_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v856_kind, rt_s(&S204)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S33)))) {
    (void)rt_chk(s707_fnDef(up, v855_s), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S34)))) {
    if (rt_cond(rt_def(up->v660_iife, "iife"))) {
      (void)rt_chk(w2_abort(rt_get(&F_pos, v855_s), rt_s(&S526)), "stmt");
    }
    (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S527), rt_text((rt_cond(rt_eq(rt_get(&F_e, v855_s), RT_NULL)) ? rt_s(&S525) : rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_e, v855_s)), "stmt")))), rt_s(&S528) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S57)))) {
    (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S529), rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_e, v855_s)), "stmt"))), rt_s(&S528) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v856_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v856_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_def(up->v659_loops, "loops"), rt_i(0)))) {
      (void)({ wv t1604 = rt_get(&F_pos, v855_s); wv t1605 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text((rt_cond(rt_eq(v856_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43))), rt_s(&S530) }); rt_chk(w2_abort(t1604, t1605), "stmt"); });
    }
    (void)rt_chk(s687_emit(up, (rt_cond(rt_eq(v856_kind, rt_s(&S42))) ? rt_s(&S531) : rt_s(&S532))), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S41)))) {
    (void)rt_chk(s687_emit(up, ({ wv t1606 = rt_text((rt_cond(rt_get(&F_neg, v855_s)) ? rt_s(&S94) : rt_s(&S13))); wv t1607 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_cond, v855_s)), "stmt"))); rt_cat(5, (wv[]){ rt_s(&S533), t1606, rt_s(&S534), t1607, rt_s(&S535) }); })), "stmt");
    up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
    up->v659_loops = rt_add(rt_def(up->v659_loops, "loops"), rt_i(1));
    (void)rt_chk(s702_blockBody(up, rt_get(&F_body, v855_s)), "stmt");
    up->v659_loops = rt_sub(rt_def(up->v659_loops, "loops"), rt_i(1));
    up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
    (void)rt_chk(s687_emit(up, rt_s(&S15)), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S55)))) {
    (void)rt_chk(s705_signal(up, rt_cat(3, (wv[]){ rt_s(&S536), rt_text(rt_get(&F_code, v855_s)), rt_s(&S16) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S54)))) {
    (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S537), rt_text((rt_cond(rt_eq(rt_get(&F_e, v855_s), RT_NULL)) ? rt_s(&S538) : rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_e, v855_s)), "stmt")))), rt_s(&S528) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S176)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v855_s)), rt_s(&S184)))) {
      (void)rt_chk(s715_chainStmt(up, rt_get(&F_e, v855_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v855_s)), rt_s(&S44)))) {
      (void)rt_chk(s726_matchStmt(up, rt_get(&F_e, v855_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v855_s)), rt_s(&S183)))) {
      (void)rt_chk(s705_signal(up, rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_arg, rt_get(&F_e, v855_s))), "stmt"))), "stmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S524), rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_e, v855_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S177)))) {
    (void)({ wv t1612 = ({ wv t1608 = rt_get(&F_namePos, v855_s); wv t1609 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1608, t1609); }); wv t1613 = ({ wv t1610 = rt_get(&F_e, v855_s); wv t1611 = rt_chk(w6_modeOf(v855_s), "stmt"); rt_chk(s708_bindRhs(up, t1610, t1611), "stmt"); }); rt_chk(s706_bindDecl(up, t1612, t1613), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v856_kind, rt_s(&S38)))) {
    (void)rt_chk(s709_setStmt(up, v855_s), "stmt");
    RT_RET(RT_NULL);
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S539), rt_text(v856_kind) }));
  RT_RET(RT_NULL);
}

static wv s705_signal(struct fr142 *up, wv v857_code) {
  rt_enter("signal");
  v857_code = rt_bind(v857_code, 0);
  (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S540), rt_text(v857_code), rt_s(&S528) })), "signal");
  (void)rt_chk(s687_emit(up, rt_s(&S541)), "signal");
  RT_RET(RT_NULL);
}

static wv s706_bindDecl(struct fr142 *up, wv v858_d, wv v859_value) {
  rt_enter("bindDecl");
  v858_d = rt_bind(v858_d, 0);
  v859_value = rt_bind(v859_value, 0);
  wv v860_owner = rt_bind(rt_get(&F_func, ({ wv t1614 = v858_d; wv t1615 = rt_def(up->v661_decls, "decls"); rt_idx(t1614, t1615); })), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1616 = v860_owner; wv t1617 = rt_def(up->v662_fns, "fns"); rt_idx(t1616, t1617); }))), "&&") && rt_bool(({ wv t1618 = v860_owner; wv t1619 = rt_def(up->v670_cur, "cur"); rt_eq(t1618, t1619); }), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_captured, ({ wv t1620 = v858_d; wv t1621 = rt_def(up->v661_decls, "decls"); rt_idx(t1620, t1621); }))), "&&")))) {
    (void)rt_chk(s687_emit(up, ({ wv t1624 = rt_text(rt_get(&F_cname, ({ wv t1622 = v858_d; wv t1623 = rt_def(up->v661_decls, "decls"); rt_idx(t1622, t1623); }))); wv t1625 = rt_text(v859_value); rt_cat(5, (wv[]){ rt_s(&S519), t1624, rt_s(&S520), t1625, rt_s(&S14) }); })), "bindDecl");
  } else {
    (void)rt_chk(s687_emit(up, ({ wv t1626 = rt_text(rt_chk(s695_place(up, v858_d), "bindDecl")); wv t1627 = rt_text(v859_value); rt_cat(4, (wv[]){ t1626, rt_s(&S520), t1627, rt_s(&S14) }); })), "bindDecl");
  }
  RT_RET(RT_NULL);
}

static wv s707_fnDef(struct fr142 *up, wv v861_s) {
  rt_enter("fnDef");
  v861_s = rt_bind(v861_s, 0);
  wv v862_d = rt_bind(({ wv t1628 = rt_get(&F_namePos, v861_s); wv t1629 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1628, t1629); }), 0);
  wv v863_f = rt_bind(rt_get(&F_fnIndex, ({ wv t1630 = v862_d; wv t1631 = rt_def(up->v661_decls, "decls"); rt_idx(t1630, t1631); })), 0);
  wv v864_pf = rt_bind(rt_get(&F_parent, ({ wv t1632 = v863_f; wv t1633 = rt_def(up->v662_fns, "fns"); rt_idx(t1632, t1633); })), 0);
  wv v865_hasUp = rt_not(rt_get(&F_top, ({ wv t1634 = v864_pf; wv t1635 = rt_def(up->v662_fns, "fns"); rt_idx(t1634, t1635); })));
  wv v866_params = rt_arr(0, NULL, 1);
  if (rt_cond(v865_hasUp)) {
    (void)({ wv t1636 = v866_params; wv t1637 = rt_cat(3, (wv[]){ rt_s(&S542), rt_text(v864_pf), rt_s(&S543) }); std_push(t1636, t1637); });
  }
  wv v867_k = rt_i(0);
  while (rt_cond(({ wv t1638 = v867_k; wv t1639 = std_length(rt_get(&F_params, v861_s)); rt_lt(t1638, t1639); }))) {
    wv v875_pd = rt_bind(({ wv t1642 = rt_get(&F_pos, ({ wv t1640 = v867_k; wv t1641 = rt_get(&F_params, v861_s); rt_idx(t1640, t1641); })); wv t1643 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1642, t1643); }), 0);
    (void)({ wv t1650 = v866_params; wv t1651 = ({ wv t1648 = rt_text((rt_cond(rt_get(&F_captured, ({ wv t1644 = v875_pd; wv t1645 = rt_def(up->v661_decls, "decls"); rt_idx(t1644, t1645); }))) ? rt_s(&S544) : rt_s(&S13))); wv t1649 = rt_text(rt_get(&F_cname, ({ wv t1646 = v875_pd; wv t1647 = rt_def(up->v661_decls, "decls"); rt_idx(t1646, t1647); }))); rt_cat(3, (wv[]){ rt_s(&S519), t1648, t1649 }); }); std_push(t1650, t1651); });
    v867_k = rt_add(v867_k, rt_i(1));
  }
  wv v868_proto = ({ wv t1654 = rt_text(rt_get(&F_cname, ({ wv t1652 = v863_f; wv t1653 = rt_def(up->v662_fns, "fns"); rt_idx(t1652, t1653); }))); wv t1655 = rt_text((rt_cond(rt_eq(std_length(v866_params), rt_i(0))) ? rt_s(&S546) : std_join(v866_params, rt_s(&S405)))); rt_cat(5, (wv[]){ rt_s(&S545), t1654, rt_s(&S97), t1655, rt_s(&S16) }); });
  wv v869_entry = ({ wv t1656 = v868_proto; wv t1657 = rt_arr(0, NULL, 0); rt_rec(1, 2, K56, (wv[]){ t1656, t1657 }); });
  (void)({ wv t1658 = rt_def(up->v656_functions, "functions"); wv t1659 = v869_entry; std_push(t1658, t1659); });
  if (rt_cond(rt_chk(s672_hasFrame(up, v863_f), "fnDef"))) {
    (void)({ wv t1660 = rt_def(up->v655_frames, "frames"); wv t1661 = v863_f; std_push(t1660, t1661); });
  }
  wv v870_start = rt_bind(std_length(rt_def(up->v657_out, "out")), 0);
  wv v871_savedIndent = rt_bind(rt_def(up->v658_indent, "indent"), 0);
  wv v872_savedLoops = rt_bind(rt_def(up->v659_loops, "loops"), 0);
  wv v873_savedIife = rt_bind(rt_def(up->v660_iife, "iife"), 0);
  wv v874_savedCur = rt_bind(rt_def(up->v670_cur, "cur"), 0);
  wv t1662 = rt_i(1);
  (void)rt_def(up->v658_indent, "indent");
  up->v658_indent = t1662;
  wv t1663 = rt_i(0);
  (void)rt_def(up->v659_loops, "loops");
  up->v659_loops = t1663;
  wv t1664 = rt_b(0);
  (void)rt_def(up->v660_iife, "iife");
  up->v660_iife = t1664;
  wv t1665 = rt_bind(v863_f, 1);
  (void)rt_def(up->v670_cur, "cur");
  up->v670_cur = t1665;
  (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S547), rt_text(rt_chk(w1_quote(rt_get(&F_name, v861_s)), "fnDef")), rt_s(&S528) })), "fnDef");
  if (rt_cond(rt_chk(s672_hasFrame(up, v863_f), "fnDef"))) {
    (void)rt_chk(s687_emit(up, ({ wv t1666 = rt_text(v863_f); wv t1667 = rt_text((rt_cond(v865_hasUp) ? rt_s(&S549) : rt_s(&S4))); rt_cat(5, (wv[]){ rt_s(&S542), t1666, rt_s(&S548), t1667, rt_s(&S512) }); })), "fnDef");
  }
  v867_k = rt_i(0);
  while (rt_cond(({ wv t1668 = v867_k; wv t1669 = std_length(rt_get(&F_params, v861_s)); rt_lt(t1668, t1669); }))) {
    wv v876_p = rt_bind(({ wv t1670 = v867_k; wv t1671 = rt_get(&F_params, v861_s); rt_idx(t1670, t1671); }), 0);
    wv v877_pd = rt_bind(({ wv t1672 = rt_get(&F_pos, v876_p); wv t1673 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1672, t1673); }), 0);
    wv v878_cn = rt_bind(rt_get(&F_cname, ({ wv t1674 = v877_pd; wv t1675 = rt_def(up->v661_decls, "decls"); rt_idx(t1674, t1675); })), 0);
    wv v879_arg = rt_bind((rt_cond(rt_get(&F_captured, ({ wv t1676 = v877_pd; wv t1677 = rt_def(up->v661_decls, "decls"); rt_idx(t1676, t1677); }))) ? rt_cat(2, (wv[]){ rt_s(&S544), rt_text(v878_cn) }) : v878_cn), 0);
    wv v880_v = rt_bind((rt_cond(rt_get(&F_isRef, v876_p)) ? v879_arg : ({ wv t1678 = rt_text(v879_arg); wv t1679 = rt_text((rt_cond(rt_get(&F_isVar, v876_p)) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S550), t1678, rt_s(&S405), t1679, rt_s(&S16) }); })), 0);
    if (rt_cond(rt_get(&F_captured, ({ wv t1680 = v877_pd; wv t1681 = rt_def(up->v661_decls, "decls"); rt_idx(t1680, t1681); })))) {
      (void)rt_chk(s687_emit(up, ({ wv t1682 = rt_text(v878_cn); wv t1683 = rt_text(v880_v); rt_cat(5, (wv[]){ rt_s(&S514), t1682, rt_s(&S520), t1683, rt_s(&S14) }); })), "fnDef");
    } else if (rt_cond(rt_not(rt_get(&F_isRef, v876_p)))) {
      (void)rt_chk(s687_emit(up, ({ wv t1684 = rt_text(v878_cn); wv t1685 = rt_text(v880_v); rt_cat(4, (wv[]){ t1684, rt_s(&S520), t1685, rt_s(&S14) }); })), "fnDef");
    }
    v867_k = rt_add(v867_k, rt_i(1));
  }
  (void)rt_chk(s700_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v861_s))), "fnDef");
  (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S527), rt_text(rt_chk(s703_valueOf(up, rt_get(&F_body, v861_s)), "fnDef")), rt_s(&S528) })), "fnDef");
  ({ wv t1689 = ({ wv t1686 = rt_def(up->v657_out, "out"); wv t1687 = v870_start; wv t1688 = std_length(rt_def(up->v657_out, "out")); std_slice(t1686, t1687, t1688); }); wv t1690 = v869_entry; rt_set_mem(t1689, &F_lines, t1690); });
  while (rt_cond(({ wv t1691 = std_length(rt_def(up->v657_out, "out")); wv t1692 = v870_start; rt_gt(t1691, t1692); }))) {
    (void)std_pop(rt_def(up->v657_out, "out"));
  }
  wv t1693 = rt_bind(v871_savedIndent, 1);
  (void)rt_def(up->v658_indent, "indent");
  up->v658_indent = t1693;
  wv t1694 = rt_bind(v872_savedLoops, 1);
  (void)rt_def(up->v659_loops, "loops");
  up->v659_loops = t1694;
  wv t1695 = rt_bind(v873_savedIife, 1);
  (void)rt_def(up->v660_iife, "iife");
  up->v660_iife = t1695;
  wv t1696 = rt_bind(v874_savedCur, 1);
  (void)rt_def(up->v670_cur, "cur");
  up->v670_cur = t1696;
  RT_RET(RT_NULL);
}

static wv s708_bindRhs(struct fr142 *up, wv v881_e, wv v882_mode) {
  rt_enter("bindRhs");
  v881_e = rt_bind(v881_e, 0);
  v882_mode = rt_bind(v882_mode, 0);
  wv v883_mutable = rt_eq(v882_mode, rt_s(&S36));
  if (rt_cond(rt_eq(rt_get(&F_k, v881_e), rt_s(&S186)))) {
    RT_RET(rt_get(&F_c, rt_chk(s711_array(up, v881_e, v883_mutable), "bindRhs")));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v881_e), rt_s(&S187)))) {
    RT_RET(rt_get(&F_c, rt_chk(s712_mapLit(up, v881_e, v883_mutable), "bindRhs")));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v881_e), rt_s(&S188)))) {
    RT_RET(rt_get(&F_c, rt_chk(s713_record(up, v881_e, v883_mutable), "bindRhs")));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1697 = rt_def(w6_scalarKinds, "scalarKinds"); wv t1698 = rt_get(&F_k, v881_e); rt_chk(w1_contains(t1697, t1698), "bindRhs"); }), "||") || rt_bool(rt_eq(v882_mode, rt_s(&S37)), "||")))) {
    RT_RET(rt_get(&F_c, rt_chk(s729_expr(up, v881_e), "bindRhs")));
  }
  RT_RET(({ wv t1699 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, v881_e), "bindRhs"))); wv t1700 = rt_text((rt_cond(v883_mutable) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S550), t1699, rt_s(&S405), t1700, rt_s(&S16) }); }));
}

static wv s709_setStmt(struct fr142 *up, wv v884_s) {
  rt_enter("setStmt");
  v884_s = rt_bind(v884_s, 0);
  wv v885_t = rt_bind(rt_get(&F_target, v884_s), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v885_t), rt_s(&S212)))) {
    wv v892_d = rt_bind(({ wv t1701 = rt_get(&F_pos, v885_t); wv t1702 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1701, t1702); }), 0);
    wv v893_at = rt_bind(rt_chk(s695_place(up, v892_d), "setStmt"), 0);
    if (rt_cond(rt_eq(rt_get(&F_op, v884_s), RT_NULL))) {
      wv v896_v = rt_bind(({ wv t1705 = rt_get(&F_e, v884_s); wv t1706 = (rt_cond(rt_get(&F_isRef, ({ wv t1703 = v892_d; wv t1704 = rt_def(up->v661_decls, "decls"); rt_idx(t1703, t1704); }))) ? rt_s(&S37) : rt_s(&S36)); rt_chk(s708_bindRhs(up, t1705, t1706), "setStmt"); }), 0);
      if (rt_cond(rt_not(rt_chk(s696_tdz(up, v892_d), "setStmt")))) {
        (void)rt_chk(s687_emit(up, ({ wv t1707 = rt_text(v893_at); wv t1708 = rt_text(v896_v); rt_cat(4, (wv[]){ t1707, rt_s(&S520), t1708, rt_s(&S14) }); })), "setStmt");
        RT_RET(RT_NULL);
      }
      wv v897_temp = rt_bind(rt_chk(s688_tmp(up), "setStmt"), 0);
      (void)rt_chk(s687_emit(up, ({ wv t1709 = rt_text(v897_temp); wv t1710 = rt_text(v896_v); rt_cat(5, (wv[]){ rt_s(&S519), t1709, rt_s(&S520), t1710, rt_s(&S14) }); })), "setStmt");
      (void)rt_chk(s687_emit(up, ({ wv t1713 = rt_text(v893_at); wv t1714 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1711 = v892_d; wv t1712 = rt_def(up->v661_decls, "decls"); rt_idx(t1711, t1712); }))), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S551), t1713, rt_s(&S405), t1714, rt_s(&S528) }); })), "setStmt");
      (void)rt_chk(s687_emit(up, ({ wv t1715 = rt_text(v893_at); wv t1716 = rt_text(v897_temp); rt_cat(4, (wv[]){ t1715, rt_s(&S520), t1716, rt_s(&S14) }); })), "setStmt");
      RT_RET(RT_NULL);
    }
    wv v894_v = rt_bind(rt_chk(s729_expr(up, rt_get(&F_e, v884_s)), "setStmt"), 1);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v884_s)), rt_s(&S138)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v884_s)), rt_s(&S212)), "&&")))) {
      wv v898_temp = rt_bind(rt_chk(s688_tmp(up), "setStmt"), 0);
      (void)rt_chk(s687_emit(up, ({ wv t1717 = rt_text(v898_temp); wv t1718 = rt_text(rt_get(&F_c, v894_v)); rt_cat(5, (wv[]){ rt_s(&S519), t1717, rt_s(&S520), t1718, rt_s(&S14) }); })), "setStmt");
      v894_v = ({ wv t1719 = v898_temp; wv t1720 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K54, (wv[]){ t1719, t1720 }); });
    }
    wv v895_sq = rt_bind(rt_chk(s698_seq(up, ({ wv t1721 = rt_chk(s697_read(up, v892_d), "setStmt"); wv t1722 = v894_v; rt_arr(2, (wv[]){ t1721, t1722 }, 0); })), "setStmt"), 0);
    (void)rt_chk(s687_emit(up, ({ wv t1728 = rt_text(v893_at); wv t1729 = rt_text(rt_get(&F_c, ({ wv t1726 = v895_sq; wv t1727 = ({ wv t1723 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v884_s)), "setStmt")); wv t1724 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v895_sq))); wv t1725 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v895_sq))); rt_cat(7, (wv[]){ rt_s(&S552), t1723, rt_s(&S97), t1724, rt_s(&S405), t1725, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1726, t1727), "setStmt"); }))); rt_cat(4, (wv[]){ t1728, rt_s(&S520), t1729, rt_s(&S14) }); })), "setStmt");
    RT_RET(RT_NULL);
  }
  wv v886_key = RT_NULL;
  if (rt_cond(rt_eq(rt_get(&F_k, v885_t), rt_s(&S190)))) {
    v886_key = ({ wv t1730 = rt_chk(s690_fieldRef(up, rt_get(&F_field, v885_t)), "setStmt"); wv t1731 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K54, (wv[]){ t1730, t1731 }); });
  }
  wv v887_v = rt_bind(rt_chk(s729_expr(up, rt_get(&F_e, v884_s)), "setStmt"), 0);
  wv v888_i = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v885_t), rt_s(&S190))) ? v886_key : rt_chk(s729_expr(up, rt_get(&F_index, v885_t)), "setStmt")), 0);
  wv v889_box = rt_bind(rt_chk(s729_expr(up, (rt_cond(rt_eq(rt_get(&F_k, v885_t), rt_s(&S190))) ? rt_get(&F_rec, v885_t) : rt_get(&F_arr, v885_t))), "setStmt"), 0);
  wv v890_sq = rt_bind(rt_chk(s698_seq(up, rt_arr(3, (wv[]){ v887_v, v888_i, v889_box }, 0)), "setStmt"), 0);
  wv v891_which = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v885_t), rt_s(&S190))) ? rt_s(&S190) : rt_s(&S189)), 0);
  if (rt_cond(rt_eq(rt_get(&F_op, v884_s), RT_NULL))) {
    (void)rt_chk(s687_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t1734 = v890_sq; wv t1735 = ({ wv t1732 = rt_text(v891_which); wv t1733 = rt_text(std_join(rt_get(&F_cs, v890_sq), rt_s(&S405))); rt_cat(5, (wv[]){ rt_s(&S553), t1732, rt_s(&S97), t1733, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1734, t1735), "setStmt"); }))), rt_s(&S14) })), "setStmt");
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s687_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t1739 = v890_sq; wv t1740 = ({ wv t1736 = rt_text(v891_which); wv t1737 = rt_text(std_join(rt_get(&F_cs, v890_sq), rt_s(&S405))); wv t1738 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v884_s)), "setStmt")); rt_cat(7, (wv[]){ rt_s(&S554), t1736, rt_s(&S97), t1737, rt_s(&S555), t1738, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1739, t1740), "setStmt"); }))), rt_s(&S14) })), "setStmt");
  RT_RET(RT_NULL);
}

static wv s710_exprs(struct fr142 *up, wv v899_list) {
  rt_enter("exprs");
  v899_list = rt_bind(v899_list, 0);
  wv v900_xs = rt_arr(0, NULL, 1);
  wv v901_k = rt_i(0);
  while (rt_cond(({ wv t1741 = v901_k; wv t1742 = std_length(v899_list); rt_lt(t1741, t1742); }))) {
    (void)({ wv t1743 = v900_xs; wv t1744 = rt_chk(s729_expr(up, rt_idx(v901_k, v899_list)), "exprs"); std_push(t1743, t1744); });
    v901_k = rt_add(v901_k, rt_i(1));
  }
  RT_RET(v900_xs);
}

static wv s711_array(struct fr142 *up, wv v902_e, wv v903_mutable) {
  rt_enter("array");
  v902_e = rt_bind(v902_e, 0);
  v903_mutable = rt_bind(v903_mutable, 0);
  wv v904_sq = rt_bind(rt_chk(s698_seq(up, rt_chk(s710_exprs(up, rt_get(&F_items, v902_e)), "array")), "array"), 0);
  RT_RET(({ wv t1748 = v904_sq; wv t1749 = ({ wv t1745 = rt_text(std_length(rt_get(&F_cs, v904_sq))); wv t1746 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v904_sq)), "array")); wv t1747 = rt_text((rt_cond(v903_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S556), t1745, rt_s(&S405), t1746, rt_s(&S405), t1747, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1748, t1749), "array"); }));
}

static wv s712_mapLit(struct fr142 *up, wv v905_e, wv v906_mutable) {
  rt_enter("mapLit");
  v905_e = rt_bind(v905_e, 0);
  v906_mutable = rt_bind(v906_mutable, 0);
  wv v907_sq = rt_bind(rt_chk(s698_seq(up, rt_chk(s710_exprs(up, rt_get(&F_items, v905_e)), "mapLit")), "mapLit"), 0);
  RT_RET(({ wv t1753 = v907_sq; wv t1754 = ({ wv t1750 = rt_text(std_floor(rt_div(std_length(rt_get(&F_cs, v907_sq)), rt_i(2)))); wv t1751 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v907_sq)), "mapLit")); wv t1752 = rt_text((rt_cond(v906_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S557), t1750, rt_s(&S405), t1751, rt_s(&S405), t1752, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1753, t1754), "mapLit"); }));
}

static wv s713_record(struct fr142 *up, wv v908_e, wv v909_mutable) {
  rt_enter("record");
  v908_e = rt_bind(v908_e, 0);
  v909_mutable = rt_bind(v909_mutable, 0);
  wv v910_shape = rt_s(&S503);
  if (rt_cond(rt_gt(std_length(rt_get(&F_fields, v908_e)), rt_i(0)))) {
    wv v914_names = rt_arr(0, NULL, 1);
    wv v915_k = rt_i(0);
    while (rt_cond(({ wv t1755 = v915_k; wv t1756 = std_length(rt_get(&F_fields, v908_e)); rt_lt(t1755, t1756); }))) {
      (void)({ wv t1759 = v914_names; wv t1760 = rt_get(&F_name, ({ wv t1757 = v915_k; wv t1758 = rt_get(&F_fields, v908_e); rt_idx(t1757, t1758); })); std_push(t1759, t1760); });
      v915_k = rt_add(v915_k, rt_i(1));
    }
    wv v916_key = rt_bind(std_join(v914_names, rt_s(&S96)), 0);
    v915_k = rt_i(0);
    while (rt_cond(({ wv t1761 = v915_k; wv t1762 = std_length(v914_names); rt_lt(t1761, t1762); }))) {
      (void)rt_chk(s690_fieldRef(up, rt_idx(v915_k, v914_names)), "record");
      v915_k = rt_add(v915_k, rt_i(1));
    }
    v915_k = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t1763 = v915_k; wv t1764 = std_length(rt_def(up->v652_shapes, "shapes")); rt_lt(t1763, t1764); }), "&&") && rt_bool(({ wv t1767 = ({ wv t1765 = v915_k; wv t1766 = rt_def(up->v652_shapes, "shapes"); rt_idx(t1765, t1766); }); wv t1768 = v916_key; rt_ne(t1767, t1768); }), "&&")))) {
      v915_k = rt_add(v915_k, rt_i(1));
    }
    if (rt_cond(({ wv t1769 = v915_k; wv t1770 = std_length(rt_def(up->v652_shapes, "shapes")); rt_eq(t1769, t1770); }))) {
      (void)({ wv t1771 = rt_def(up->v652_shapes, "shapes"); wv t1772 = v916_key; std_push(t1771, t1772); });
    }
    v910_shape = rt_cat(2, (wv[]){ rt_s(&S558), rt_text(v915_k) });
  }
  wv v911_vals = rt_arr(0, NULL, 1);
  wv v912_k = rt_i(0);
  while (rt_cond(({ wv t1773 = v912_k; wv t1774 = std_length(rt_get(&F_fields, v908_e)); rt_lt(t1773, t1774); }))) {
    (void)({ wv t1777 = v911_vals; wv t1778 = rt_chk(s729_expr(up, rt_get(&F_e, ({ wv t1775 = v912_k; wv t1776 = rt_get(&F_fields, v908_e); rt_idx(t1775, t1776); }))), "record"); std_push(t1777, t1778); });
    v912_k = rt_add(v912_k, rt_i(1));
  }
  wv v913_sq = rt_bind(rt_chk(s698_seq(up, v911_vals), "record"), 0);
  RT_RET(({ wv t1783 = v913_sq; wv t1784 = ({ wv t1779 = rt_text((rt_cond(v909_mutable) ? rt_i(1) : rt_i(0))); wv t1780 = rt_text(std_length(rt_get(&F_cs, v913_sq))); wv t1781 = rt_text(v910_shape); wv t1782 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v913_sq)), "record")); rt_cat(9, (wv[]){ rt_s(&S559), t1779, rt_s(&S405), t1780, rt_s(&S405), t1781, rt_s(&S405), t1782, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1783, t1784), "record"); }));
}

static wv s714_onfailStmt(struct fr142 *up, wv v917_s) {
  rt_enter("onfailStmt");
  v917_s = rt_bind(v917_s, 0);
  wv v918_call = rt_bind(rt_get(&F_e, v917_s), 0);
  wv v919_args = rt_arr(0, NULL, 1);
  wv v920_k = rt_i(0);
  while (rt_cond(({ wv t1785 = v920_k; wv t1786 = std_length(rt_get(&F_args, v918_call)); rt_lt(t1785, t1786); }))) {
    wv v924_a = rt_bind(({ wv t1787 = v920_k; wv t1788 = rt_get(&F_args, v918_call); rt_idx(t1787, t1788); }), 0);
    if (rt_cond(({ wv t1789 = rt_def(w6_plainKinds, "plainKinds"); wv t1790 = rt_get(&F_k, v924_a); rt_chk(w1_contains(t1789, t1790), "onfailStmt"); }))) {
      (void)({ wv t1791 = v919_args; wv t1792 = rt_get(&F_c, rt_chk(s729_expr(up, v924_a), "onfailStmt")); std_push(t1791, t1792); });
    } else {
      wv v925_temp = rt_bind(rt_chk(s688_tmp(up), "onfailStmt"), 0);
      (void)rt_chk(s687_emit(up, ({ wv t1793 = rt_text(v925_temp); wv t1794 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, v924_a), "onfailStmt"))); rt_cat(5, (wv[]){ rt_s(&S519), t1793, rt_s(&S520), t1794, rt_s(&S14) }); })), "onfailStmt");
      (void)std_push(v919_args, v925_temp);
    }
    v920_k = rt_add(v920_k, rt_i(1));
  }
  wv v921_r = rt_bind(rt_chk(s688_tmp(up), "onfailStmt"), 0);
  (void)rt_chk(s687_emit(up, ({ wv t1797 = rt_text(v921_r); wv t1798 = rt_text(({ wv t1795 = rt_get(&F_callee, v918_call); wv t1796 = v919_args; rt_chk(s728_callC(up, t1795, t1796, rt_b(0)), "onfailStmt"); })); rt_cat(5, (wv[]){ rt_s(&S519), t1797, rt_s(&S520), t1798, rt_s(&S14) }); })), "onfailStmt");
  (void)rt_chk(s687_emit(up, rt_s(&S560)), "onfailStmt");
  up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
  wv v922_h = rt_bind(rt_get(&F_handler, v917_s), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v922_h), RT_NULL))) {
    (void)rt_chk(s706_bindDecl(up, ({ wv t1799 = rt_get(&F_paramPos, v922_h); wv t1800 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1799, t1800); }), rt_s(&S561)), "onfailStmt");
  }
  (void)rt_chk(s687_emit(up, rt_s(&S562)), "onfailStmt");
  (void)rt_chk(s687_emit(up, rt_s(&S100)), "onfailStmt");
  up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
  (void)rt_chk(s701_resets(up, rt_get(&F_body, v922_h)), "onfailStmt");
  (void)rt_chk(s700_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v922_h))), "onfailStmt");
  (void)rt_chk(s687_emit(up, ({ wv t1801 = rt_text(v921_r); wv t1802 = rt_text(rt_chk(s703_valueOf(up, rt_get(&F_body, v922_h)), "onfailStmt")); rt_cat(4, (wv[]){ t1801, rt_s(&S520), t1802, rt_s(&S14) }); })), "onfailStmt");
  up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
  (void)rt_chk(s687_emit(up, rt_s(&S15)), "onfailStmt");
  up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
  (void)rt_chk(s687_emit(up, rt_s(&S15)), "onfailStmt");
  wv v923_rest = rt_bind(v917_s, 1);
  rt_set_mem(RT_NULL, &F_handler, v923_rest);
  ({ wv t1805 = ({ wv t1803 = v921_r; wv t1804 = rt_get(&F_pos, v917_s); rt_rec(0, 4, K57, (wv[]){ rt_s(&S563), t1803, t1804, rt_b(0) }); }); wv t1806 = v923_rest; rt_set_mem(t1805, &F_e, t1806); });
  (void)rt_chk(s704_stmt(up, v923_rest), "onfailStmt");
  RT_RET(RT_NULL);
}

static wv s715_chainStmt(struct fr142 *up, wv v926_e) {
  rt_enter("chainStmt");
  v926_e = rt_bind(v926_e, 0);
  wv v927_k = rt_i(0);
  while (rt_cond(({ wv t1807 = v927_k; wv t1808 = std_length(rt_get(&F_branches, v926_e)); rt_lt(t1807, t1808); }))) {
    wv v928_b = rt_bind(({ wv t1809 = v927_k; wv t1810 = rt_get(&F_branches, v926_e); rt_idx(t1809, t1810); }), 0);
    (void)rt_chk(s687_emit(up, ({ wv t1811 = rt_text((rt_cond(rt_gt(v927_k, rt_i(0))) ? rt_s(&S564) : rt_s(&S13))); wv t1812 = rt_text(rt_chk(s716_test(up, v928_b), "chainStmt")); rt_cat(4, (wv[]){ t1811, rt_s(&S565), t1812, rt_s(&S566) }); })), "chainStmt");
    up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
    (void)rt_chk(s702_blockBody(up, rt_get(&F_body, v928_b)), "chainStmt");
    up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
    v927_k = rt_add(v927_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v926_e), RT_NULL))) {
    (void)rt_chk(s687_emit(up, rt_s(&S567)), "chainStmt");
    up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
    (void)rt_chk(s702_blockBody(up, rt_get(&F_els, v926_e)), "chainStmt");
    up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s687_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s716_test(struct fr142 *up, wv v929_b) {
  rt_enter("test");
  v929_b = rt_bind(v929_b, 0);
  RT_RET(({ wv t1813 = rt_text((rt_cond(rt_get(&F_neg, v929_b)) ? rt_s(&S94) : rt_s(&S13))); wv t1814 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_cond, v929_b)), "test"))); rt_cat(4, (wv[]){ t1813, rt_s(&S534), t1814, rt_s(&S16) }); }));
}

static wv s717_branchCount(struct fr142 *up, wv v930_e) {
  rt_enter("branchCount");
  v930_e = rt_bind(v930_e, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v930_e), rt_s(&S184))) ? std_length(rt_get(&F_branches, v930_e)) : std_length(rt_get(&F_cases, v930_e))));
}

static wv s718_branchBody(struct fr142 *up, wv v931_e, wv v932_i) {
  rt_enter("branchBody");
  v931_e = rt_bind(v931_e, 0);
  v932_i = rt_bind(v932_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v931_e), rt_s(&S184))) ? rt_get(&F_body, ({ wv t1815 = v932_i; wv t1816 = rt_get(&F_branches, v931_e); rt_idx(t1815, t1816); })) : rt_get(&F_body, ({ wv t1817 = v932_i; wv t1818 = rt_get(&F_cases, v931_e); rt_idx(t1817, t1818); }))));
}

static wv s719_branchTest(struct fr142 *up, wv v933_e, wv v934_t, wv v935_i) {
  rt_enter("branchTest");
  v933_e = rt_bind(v933_e, 0);
  v934_t = rt_bind(v934_t, 0);
  v935_i = rt_bind(v935_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v933_e), rt_s(&S184))) ? rt_chk(s716_test(up, ({ wv t1819 = v935_i; wv t1820 = rt_get(&F_branches, v933_e); rt_idx(t1819, t1820); })), "branchTest") : ({ wv t1823 = v934_t; wv t1824 = ({ wv t1821 = v935_i; wv t1822 = rt_get(&F_cases, v933_e); rt_idx(t1821, t1822); }); rt_chk(s724_caseTest(up, t1823, t1824), "branchTest"); })));
}

static wv s720_isPlain(struct fr142 *up, wv v936_e) {
  rt_enter("isPlain");
  v936_e = rt_bind(v936_e, 0);
  wv v937_k = rt_i(0);
  while (rt_cond(({ wv t1825 = v937_k; wv t1826 = rt_chk(s717_branchCount(up, v936_e), "isPlain"); rt_le(t1825, t1826); }))) {
    wv v938_b = rt_bind((rt_cond(({ wv t1827 = v937_k; wv t1828 = rt_chk(s717_branchCount(up, v936_e), "isPlain"); rt_lt(t1827, t1828); })) ? rt_chk(s718_branchBody(up, v936_e, v937_k), "isPlain") : rt_get(&F_els, v936_e)), 0);
    wv v939_j = rt_i(0);
    while (rt_cond(({ wv t1829 = v939_j; wv t1830 = std_length(rt_get(&F_stmts, v938_b)); rt_lt(t1829, t1830); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1831 = v939_j; wv t1832 = rt_get(&F_stmts, v938_b); rt_idx(t1831, t1832); })), rt_s(&S201)))) {
        RT_RET(rt_b(0));
      }
      v939_j = rt_add(v939_j, rt_i(1));
    }
    v937_k = rt_add(v937_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s721_ternaryChain(struct fr142 *up, wv v940_e, wv v941_t) {
  rt_enter("ternaryChain");
  v940_e = rt_bind(v940_e, 0);
  v941_t = rt_bind(v941_t, 0);
  wv v942_code = rt_bind(rt_chk(s703_valueOf(up, rt_get(&F_els, v940_e)), "ternaryChain"), 1);
  wv v943_k = rt_sub(rt_chk(s717_branchCount(up, v940_e), "ternaryChain"), rt_i(1));
  while (rt_cond(rt_ge(v943_k, rt_i(0)))) {
    v942_code = ({ wv t1833 = rt_text(rt_chk(s719_branchTest(up, v940_e, v941_t, v943_k), "ternaryChain")); wv t1834 = rt_text(rt_chk(s703_valueOf(up, rt_chk(s718_branchBody(up, v940_e, v943_k), "ternaryChain")), "ternaryChain")); wv t1835 = rt_text(v942_code); rt_cat(7, (wv[]){ rt_s(&S97), t1833, rt_s(&S568), t1834, rt_s(&S569), t1835, rt_s(&S16) }); });
    v943_k = rt_sub(v943_k, rt_i(1));
  }
  RT_RET(v942_code);
}

static wv s722_branchValue(struct fr142 *up, wv v944_e, wv v945_t, wv v946_subject) {
  rt_enter("branchValue");
  v944_e = rt_bind(v944_e, 0);
  v945_t = rt_bind(v945_t, 0);
  v946_subject = rt_bind(v946_subject, 0);
  wv v947_start = rt_bind(std_length(rt_def(up->v657_out, "out")), 0);
  wv v948_savedIndent = rt_bind(rt_def(up->v658_indent, "indent"), 0);
  wv v949_savedLoops = rt_bind(rt_def(up->v659_loops, "loops"), 0);
  wv v950_savedIife = rt_bind(rt_def(up->v660_iife, "iife"), 0);
  wv t1836 = rt_add(v948_savedIndent, rt_i(1));
  (void)rt_def(up->v658_indent, "indent");
  up->v658_indent = t1836;
  wv t1837 = rt_i(0);
  (void)rt_def(up->v659_loops, "loops");
  up->v659_loops = t1837;
  wv t1838 = rt_b(1);
  (void)rt_def(up->v660_iife, "iife");
  up->v660_iife = t1838;
  if (rt_cond(rt_ne(v946_subject, RT_NULL))) {
    (void)rt_chk(s687_emit(up, ({ wv t1839 = rt_text(v945_t); wv t1840 = rt_text(v946_subject); rt_cat(5, (wv[]){ rt_s(&S519), t1839, rt_s(&S520), t1840, rt_s(&S14) }); })), "branchValue");
  }
  wv v951_r = rt_bind(rt_chk(s688_tmp(up), "branchValue"), 0);
  (void)rt_chk(s687_emit(up, rt_cat(3, (wv[]){ rt_s(&S519), rt_text(v951_r), rt_s(&S14) })), "branchValue");
  wv v952_k = rt_i(0);
  while (rt_cond(({ wv t1841 = v952_k; wv t1842 = rt_chk(s717_branchCount(up, v944_e), "branchValue"); rt_lt(t1841, t1842); }))) {
    wv v954_b = rt_bind(rt_chk(s718_branchBody(up, v944_e, v952_k), "branchValue"), 0);
    (void)rt_chk(s687_emit(up, ({ wv t1843 = rt_text((rt_cond(rt_gt(v952_k, rt_i(0))) ? rt_s(&S564) : rt_s(&S13))); wv t1844 = rt_text(rt_chk(s719_branchTest(up, v944_e, v945_t, v952_k), "branchValue")); rt_cat(4, (wv[]){ t1843, rt_s(&S565), t1844, rt_s(&S566) }); })), "branchValue");
    up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
    (void)rt_chk(s701_resets(up, v954_b), "branchValue");
    (void)rt_chk(s700_stmts(up, rt_get(&F_stmts, v954_b)), "branchValue");
    (void)rt_chk(s687_emit(up, ({ wv t1845 = rt_text(v951_r); wv t1846 = rt_text(rt_chk(s703_valueOf(up, v954_b), "branchValue")); rt_cat(4, (wv[]){ t1845, rt_s(&S520), t1846, rt_s(&S14) }); })), "branchValue");
    up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
    v952_k = rt_add(v952_k, rt_i(1));
  }
  (void)rt_chk(s687_emit(up, rt_s(&S567)), "branchValue");
  up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
  (void)rt_chk(s701_resets(up, rt_get(&F_els, v944_e)), "branchValue");
  (void)rt_chk(s700_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v944_e))), "branchValue");
  (void)rt_chk(s687_emit(up, ({ wv t1847 = rt_text(v951_r); wv t1848 = rt_text(rt_chk(s703_valueOf(up, rt_get(&F_els, v944_e)), "branchValue")); rt_cat(4, (wv[]){ t1847, rt_s(&S520), t1848, rt_s(&S14) }); })), "branchValue");
  up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
  (void)rt_chk(s687_emit(up, rt_s(&S15)), "branchValue");
  (void)rt_chk(s687_emit(up, rt_cat(2, (wv[]){ rt_text(v951_r), rt_s(&S14) })), "branchValue");
  wv v953_body = rt_bind(std_join(({ wv t1849 = rt_def(up->v657_out, "out"); wv t1850 = v947_start; wv t1851 = std_length(rt_def(up->v657_out, "out")); std_slice(t1849, t1850, t1851); }), rt_s(&S2)), 0);
  while (rt_cond(({ wv t1852 = std_length(rt_def(up->v657_out, "out")); wv t1853 = v947_start; rt_gt(t1852, t1853); }))) {
    (void)std_pop(rt_def(up->v657_out, "out"));
  }
  wv t1854 = rt_bind(v948_savedIndent, 1);
  (void)rt_def(up->v658_indent, "indent");
  up->v658_indent = t1854;
  wv t1855 = rt_bind(v949_savedLoops, 1);
  (void)rt_def(up->v659_loops, "loops");
  up->v659_loops = t1855;
  wv t1856 = rt_bind(v950_savedIife, 1);
  (void)rt_def(up->v660_iife, "iife");
  up->v660_iife = t1856;
  RT_RET(({ wv t1857 = rt_text(v953_body); wv t1858 = rt_text(rt_chk(w6_repeat(rt_s(&S507), rt_def(up->v658_indent, "indent")), "branchValue")); rt_cat(5, (wv[]){ rt_s(&S570), t1857, rt_s(&S2), t1858, rt_s(&S571) }); }));
}

static wv s723_chainExpr(struct fr142 *up, wv v955_e) {
  rt_enter("chainExpr");
  v955_e = rt_bind(v955_e, 0);
  if (rt_cond(rt_chk(s720_isPlain(up, v955_e), "chainExpr"))) {
    RT_RET(rt_chk(s721_ternaryChain(up, v955_e, RT_NULL), "chainExpr"));
  }
  RT_RET(rt_chk(s722_branchValue(up, v955_e, RT_NULL, RT_NULL), "chainExpr"));
}

static wv s724_caseTest(struct fr142 *up, wv v956_t, wv v957_a) {
  rt_enter("caseTest");
  v956_t = rt_bind(v956_t, 0);
  v957_a = rt_bind(v957_a, 0);
  wv v958_tests = rt_arr(0, NULL, 1);
  wv v959_k = rt_i(0);
  while (rt_cond(({ wv t1859 = v959_k; wv t1860 = std_length(rt_get(&F_values, v957_a)); rt_lt(t1859, t1860); }))) {
    (void)({ wv t1865 = v958_tests; wv t1866 = ({ wv t1863 = rt_text(v956_t); wv t1864 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, ({ wv t1861 = v959_k; wv t1862 = rt_get(&F_values, v957_a); rt_idx(t1861, t1862); })), "caseTest"))); rt_cat(5, (wv[]){ rt_s(&S572), t1863, rt_s(&S405), t1864, rt_s(&S16) }); }); std_push(t1865, t1866); });
    v959_k = rt_add(v959_k, rt_i(1));
  }
  RT_RET(std_join(v958_tests, rt_s(&S573)));
}

static wv s725_matchElse(struct fr142 *up, wv v960_e, wv v961_t) {
  rt_enter("matchElse");
  v960_e = rt_bind(v960_e, 0);
  v961_t = rt_bind(v961_t, 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v960_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_cases, v960_e))))), rt_s(&S190)), "||")))) {
    RT_RET(rt_get(&F_els, v960_e));
  }
  RT_RET(({ wv t1867 = rt_arr(0, NULL, 0); wv t1868 = rt_rec(0, 4, K57, (wv[]){ rt_s(&S563), rt_cat(3, (wv[]){ rt_s(&S574), rt_text(v961_t), rt_s(&S575) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K21, (wv[]){ t1867, t1868, RT_NULL }); }));
}

static wv s726_matchStmt(struct fr142 *up, wv v962_e) {
  rt_enter("matchStmt");
  v962_e = rt_bind(v962_e, 0);
  wv v963_t = rt_bind(rt_chk(s688_tmp(up), "matchStmt"), 0);
  (void)rt_chk(s687_emit(up, ({ wv t1869 = rt_text(v963_t); wv t1870 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_subject, v962_e)), "matchStmt"))); rt_cat(5, (wv[]){ rt_s(&S519), t1869, rt_s(&S520), t1870, rt_s(&S14) }); })), "matchStmt");
  wv v964_k = rt_i(0);
  while (rt_cond(({ wv t1871 = v964_k; wv t1872 = std_length(rt_get(&F_cases, v962_e)); rt_lt(t1871, t1872); }))) {
    (void)rt_chk(s687_emit(up, ({ wv t1877 = rt_text((rt_cond(rt_gt(v964_k, rt_i(0))) ? rt_s(&S564) : rt_s(&S13))); wv t1878 = rt_text(({ wv t1875 = v963_t; wv t1876 = ({ wv t1873 = v964_k; wv t1874 = rt_get(&F_cases, v962_e); rt_idx(t1873, t1874); }); rt_chk(s724_caseTest(up, t1875, t1876), "matchStmt"); })); rt_cat(4, (wv[]){ t1877, rt_s(&S565), t1878, rt_s(&S566) }); })), "matchStmt");
    up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
    (void)rt_chk(s702_blockBody(up, rt_get(&F_body, ({ wv t1879 = v964_k; wv t1880 = rt_get(&F_cases, v962_e); rt_idx(t1879, t1880); }))), "matchStmt");
    up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
    v964_k = rt_add(v964_k, rt_i(1));
  }
  wv v965_els = rt_bind(rt_chk(s725_matchElse(up, v962_e, v963_t), "matchStmt"), 0);
  if (rt_cond(rt_ne(v965_els, RT_NULL))) {
    (void)rt_chk(s687_emit(up, rt_s(&S567)), "matchStmt");
    up->v658_indent = rt_add(rt_def(up->v658_indent, "indent"), rt_i(1));
    (void)rt_chk(s702_blockBody(up, v965_els), "matchStmt");
    up->v658_indent = rt_sub(rt_def(up->v658_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s687_emit(up, rt_s(&S15)), "matchStmt");
  RT_RET(RT_NULL);
}

static wv s727_matchExpr(struct fr142 *up, wv v966_e) {
  rt_enter("matchExpr");
  v966_e = rt_bind(v966_e, 0);
  wv v967_t = rt_bind(rt_chk(s688_tmp(up), "matchExpr"), 0);
  wv v968_subject = rt_bind(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_subject, v966_e)), "matchExpr")), 0);
  wv v969_withElse = rt_bind(v966_e, 1);
  ({ wv t1881 = rt_chk(s725_matchElse(up, v966_e, v967_t), "matchExpr"); wv t1882 = v969_withElse; rt_set_mem(t1881, &F_els, t1882); });
  if (rt_cond(rt_chk(s720_isPlain(up, v969_withElse), "matchExpr"))) {
    RT_RET(({ wv t1883 = rt_text(v967_t); wv t1884 = rt_text(v968_subject); wv t1885 = rt_text(rt_chk(s721_ternaryChain(up, v969_withElse, v967_t), "matchExpr")); rt_cat(7, (wv[]){ rt_s(&S576), t1883, rt_s(&S520), t1884, rt_s(&S577), t1885, rt_s(&S522) }); }));
  }
  RT_RET(rt_chk(s722_branchValue(up, v969_withElse, v967_t, v968_subject), "matchExpr"));
}

static wv s728_callC(struct fr142 *up, wv v970_callee, wv v971_args, wv v972_checked) {
  rt_enter("callC");
  v970_callee = rt_bind(v970_callee, 0);
  v972_checked = rt_bind(v972_checked, 0);
  wv v973_code = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v970_callee), rt_s(&S156)))) {
    v973_code = ({ wv t1886 = rt_text(rt_get(&F_name, v970_callee)); wv t1887 = rt_text(std_join(v971_args, rt_s(&S405))); rt_cat(5, (wv[]){ rt_s(&S578), t1886, rt_s(&S97), t1887, rt_s(&S16) }); });
    if (rt_cond(rt_not(({ wv t1888 = rt_def(w6_signalers, "signalers"); wv t1889 = rt_get(&F_name, v970_callee); rt_chk(w1_contains(t1888, t1889), "callC"); })))) {
      RT_RET(v973_code);
    }
  } else {
    wv v974_f = rt_s(&S13);
    wv v975_parent = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_k, v970_callee), rt_s(&S190)))) {
      v974_f = ({ wv t1890 = rt_text(rt_chk(s671_modId(up, rt_chk(s692_stolenFrom(up, rt_get(&F_rec, v970_callee)), "callC")), "callC")); wv t1891 = rt_text(rt_get(&F_field, v970_callee)); rt_cat(4, (wv[]){ rt_s(&S505), t1890, rt_s(&S12), t1891 }); });
    } else {
      wv v978_d = rt_bind(({ wv t1892 = rt_get(&F_pos, v970_callee); wv t1893 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1892, t1893); }), 0);
      wv v979_sf = rt_bind(rt_get(&F_fnIndex, ({ wv t1894 = v978_d; wv t1895 = rt_def(up->v661_decls, "decls"); rt_idx(t1894, t1895); })), 0);
      v974_f = rt_bind(rt_get(&F_cname, ({ wv t1896 = v979_sf; wv t1897 = rt_def(up->v662_fns, "fns"); rt_idx(t1896, t1897); })), 1);
      wv v980_pf = rt_bind(rt_get(&F_parent, ({ wv t1898 = v979_sf; wv t1899 = rt_def(up->v662_fns, "fns"); rt_idx(t1898, t1899); })), 0);
      if (rt_cond(rt_not(rt_get(&F_top, ({ wv t1900 = v980_pf; wv t1901 = rt_def(up->v662_fns, "fns"); rt_idx(t1900, t1901); }))))) {
        v975_parent = rt_bind(v980_pf, 1);
      }
    }
    wv v976_all = rt_arr(0, NULL, 1);
    if (rt_cond(rt_ne(v975_parent, RT_NULL))) {
      (void)({ wv t1910 = v976_all; wv t1911 = (rt_cond(({ wv t1902 = v975_parent; wv t1903 = rt_def(up->v670_cur, "cur"); rt_eq(t1902, t1903); })) ? rt_s(&S579) : rt_cat(2, (wv[]){ rt_s(&S515), rt_text(rt_chk(w6_repeat(rt_s(&S516), rt_sub(({ wv t1908 = rt_get(&F_depth, ({ wv t1904 = rt_def(up->v670_cur, "cur"); wv t1905 = rt_def(up->v662_fns, "fns"); rt_idx(t1904, t1905); })); wv t1909 = rt_get(&F_depth, ({ wv t1906 = v975_parent; wv t1907 = rt_def(up->v662_fns, "fns"); rt_idx(t1906, t1907); })); rt_sub(t1908, t1909); }), rt_i(1))), "callC")) })); std_push(t1910, t1911); });
    }
    wv v977_k = rt_i(0);
    while (rt_cond(({ wv t1912 = v977_k; wv t1913 = std_length(v971_args); rt_lt(t1912, t1913); }))) {
      (void)({ wv t1914 = v976_all; wv t1915 = rt_idx(v977_k, v971_args); std_push(t1914, t1915); });
      v977_k = rt_add(v977_k, rt_i(1));
    }
    v973_code = ({ wv t1916 = rt_text(v974_f); wv t1917 = rt_text(std_join(v976_all, rt_s(&S405))); rt_cat(4, (wv[]){ t1916, rt_s(&S97), t1917, rt_s(&S16) }); });
  }
  RT_RET((rt_cond(v972_checked) ? ({ wv t1922 = rt_text(v973_code); wv t1923 = rt_text((rt_cond(rt_get(&F_top, ({ wv t1918 = rt_def(up->v670_cur, "cur"); wv t1919 = rt_def(up->v662_fns, "fns"); rt_idx(t1918, t1919); }))) ? rt_s(&S503) : rt_chk(w1_quote(rt_get(&F_name, ({ wv t1920 = rt_def(up->v670_cur, "cur"); wv t1921 = rt_def(up->v662_fns, "fns"); rt_idx(t1920, t1921); }))), "callC"))); rt_cat(5, (wv[]){ rt_s(&S580), t1922, rt_s(&S405), t1923, rt_s(&S16) }); }) : v973_code));
}

static wv s729_expr(struct fr142 *up, wv v981_e) {
  rt_enter("expr");
  wv v982_kind = rt_bind(rt_get(&F_k, v981_e), 0);
  if (rt_cond(rt_eq(v982_kind, rt_s(&S138)))) {
    RT_RET(({ wv t1924 = rt_cat(3, (wv[]){ rt_s(&S536), rt_text(rt_get(&F_v, v981_e)), rt_s(&S16) }); wv t1925 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1924, t1925 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S148)))) {
    RT_RET(({ wv t1926 = rt_cat(3, (wv[]){ rt_s(&S581), rt_text(rt_get(&F_v, v981_e)), rt_s(&S16) }); wv t1927 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1926, t1927 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S171)))) {
    wv v983_parts = rt_arr(0, NULL, 1);
    wv v984_k = rt_i(0);
    while (rt_cond(({ wv t1928 = v984_k; wv t1929 = std_length(rt_get(&F_parts, v981_e)); rt_lt(t1928, t1929); }))) {
      wv v986_part = rt_bind(({ wv t1930 = v984_k; wv t1931 = rt_get(&F_parts, v981_e); rt_idx(t1930, t1931); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v986_part), rt_s(&S185)))) {
        if (rt_cond(rt_ne(v986_part, rt_s(&S13)))) {
          (void)({ wv t1934 = v983_parts; wv t1935 = ({ wv t1932 = rt_cat(3, (wv[]){ rt_s(&S582), rt_text(rt_chk(s689_str(up, v986_part), "expr")), rt_s(&S16) }); wv t1933 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1932, t1933 }); }); std_push(t1934, t1935); });
        }
      } else {
        (void)({ wv t1938 = v983_parts; wv t1939 = ({ wv t1936 = rt_cat(3, (wv[]){ rt_s(&S583), rt_text(rt_get(&F_c, rt_chk(s729_expr(up, v986_part), "expr"))), rt_s(&S16) }); wv t1937 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1936, t1937 }); }); std_push(t1938, t1939); });
      }
      v984_k = rt_add(v984_k, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v983_parts), rt_i(0)))) {
      RT_RET(({ wv t1940 = rt_cat(3, (wv[]){ rt_s(&S582), rt_text(rt_chk(s689_str(up, rt_s(&S13)), "expr")), rt_s(&S16) }); wv t1941 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1940, t1941 }); }));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v983_parts), rt_i(1)), "&&") && rt_bool(({ wv t1942 = rt_get(&F_k, rt_idx(rt_i(0), v983_parts)); wv t1943 = rt_def(w6_constK, "constK"); rt_eq(t1942, t1943); }), "&&")))) {
      RT_RET(rt_idx(rt_i(0), v983_parts));
    }
    wv v985_sq = rt_bind(rt_chk(s698_seq(up, v983_parts), "expr"), 0);
    RT_RET(({ wv t1946 = v985_sq; wv t1947 = ({ wv t1944 = rt_text(std_length(rt_get(&F_cs, v985_sq))); wv t1945 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v985_sq)), "expr")); rt_cat(5, (wv[]){ rt_s(&S584), t1944, rt_s(&S405), t1945, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1946, t1947), "expr"); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S279)))) {
    RT_RET(({ wv t1948 = rt_cat(3, (wv[]){ rt_s(&S585), rt_text((rt_cond(rt_get(&F_v, v981_e)) ? rt_i(1) : rt_i(0))), rt_s(&S16) }); wv t1949 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1948, t1949 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S51)))) {
    RT_RET(rt_rec(0, 2, K54, (wv[]){ rt_s(&S525), rt_def(w6_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S52)))) {
    RT_RET(rt_rec(0, 2, K54, (wv[]){ rt_s(&S586), rt_def(w6_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S53)))) {
    RT_RET(rt_rec(0, 2, K54, (wv[]){ rt_s(&S587), rt_def(w6_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S212)))) {
    RT_RET(rt_chk(s697_read(up, ({ wv t1950 = rt_get(&F_pos, v981_e); wv t1951 = rt_def(up->v667_curSlot, "curSlot"); rt_idx(t1950, t1951); })), "expr"));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S156)))) {
    RT_RET(({ wv t1952 = rt_cat(3, (wv[]){ rt_s(&S578), rt_text(rt_get(&F_name, v981_e)), rt_s(&S588) }); wv t1953 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1952, t1953 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S186)))) {
    RT_RET(rt_chk(s711_array(up, v981_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S187)))) {
    RT_RET(rt_chk(s712_mapLit(up, v981_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S188)))) {
    RT_RET(rt_chk(s713_record(up, v981_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S189)))) {
    wv v987_sq = rt_bind(rt_chk(s698_seq(up, ({ wv t1954 = rt_chk(s729_expr(up, rt_get(&F_index, v981_e)), "expr"); wv t1955 = rt_chk(s729_expr(up, rt_get(&F_arr, v981_e)), "expr"); rt_arr(2, (wv[]){ t1954, t1955 }, 0); })), "expr"), 0);
    RT_RET(({ wv t1958 = v987_sq; wv t1959 = ({ wv t1956 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v987_sq))); wv t1957 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v987_sq))); rt_cat(5, (wv[]){ rt_s(&S589), t1956, rt_s(&S405), t1957, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1958, t1959), "expr"); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S190)))) {
    wv v988_lin = rt_bind(rt_chk(s694_enumOf(up, rt_get(&F_rec, v981_e)), "expr"), 0);
    if (rt_cond(rt_ne(v988_lin, RT_NULL))) {
      wv v992_ms = rt_bind(rt_get(&F_members, rt_get(&F_node, v988_lin)), 0);
      wv v993_k = rt_i(0);
      while (rt_cond(({ wv t1960 = rt_get(&F_name, rt_idx(v993_k, v992_ms)); wv t1961 = rt_get(&F_field, v981_e); rt_ne(t1960, t1961); }))) {
        v993_k = rt_add(v993_k, rt_i(1));
      }
      RT_RET(({ wv t1964 = ({ wv t1962 = rt_text(rt_get(&F_c, v988_lin)); wv t1963 = rt_text(v993_k); rt_cat(5, (wv[]){ rt_s(&S590), t1962, rt_s(&S98), t1963, rt_s(&S591) }); }); wv t1965 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1964, t1965 }); }));
    }
    wv v989_lib = rt_bind(rt_chk(s692_stolenFrom(up, rt_get(&F_rec, v981_e)), "expr"), 0);
    if (rt_cond(rt_ne(v989_lib, RT_NULL))) {
      RT_RET(({ wv t1968 = ({ wv t1966 = rt_text(rt_chk(s671_modId(up, v989_lib), "expr")); wv t1967 = rt_text(rt_get(&F_field, v981_e)); rt_cat(4, (wv[]){ rt_s(&S505), t1966, rt_s(&S12), t1967 }); }); wv t1969 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K54, (wv[]){ t1968, t1969 }); }));
    }
    wv v990_key = ({ wv t1970 = rt_chk(s690_fieldRef(up, rt_get(&F_field, v981_e)), "expr"); wv t1971 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1970, t1971 }); });
    wv v991_sq = rt_bind(rt_chk(s698_seq(up, ({ wv t1972 = v990_key; wv t1973 = rt_chk(s729_expr(up, rt_get(&F_rec, v981_e)), "expr"); rt_arr(2, (wv[]){ t1972, t1973 }, 0); })), "expr"), 0);
    RT_RET(({ wv t1976 = v991_sq; wv t1977 = ({ wv t1974 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v991_sq))); wv t1975 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v991_sq))); rt_cat(5, (wv[]){ rt_s(&S592), t1974, rt_s(&S405), t1975, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1976, t1977), "expr"); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S191)))) {
    RT_RET(({ wv t1980 = ({ wv t1978 = rt_text(rt_chk(w6_unFn(rt_get(&F_op, v981_e)), "expr")); wv t1979 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_e, v981_e)), "expr"))); rt_cat(5, (wv[]){ rt_s(&S552), t1978, rt_s(&S97), t1979, rt_s(&S16) }); }); wv t1981 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1980, t1981 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S192)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v981_e), rt_s(&S80)), "||") || rt_bool(rt_eq(rt_get(&F_op, v981_e), rt_s(&S81)), "||")))) {
      RT_RET(({ wv t1987 = ({ wv t1982 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_l, v981_e)), "expr"))); wv t1983 = rt_text(rt_get(&F_op, v981_e)); wv t1984 = rt_text(rt_get(&F_op, v981_e)); wv t1985 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_r, v981_e)), "expr"))); wv t1986 = rt_text(rt_get(&F_op, v981_e)); rt_cat(11, (wv[]){ rt_s(&S593), t1982, rt_s(&S594), t1983, rt_s(&S595), t1984, rt_s(&S596), t1985, rt_s(&S594), t1986, rt_s(&S597) }); }); wv t1988 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1987, t1988 }); }));
    }
    wv v994_sq = rt_bind(rt_chk(s698_seq(up, ({ wv t1989 = rt_chk(s729_expr(up, rt_get(&F_l, v981_e)), "expr"); wv t1990 = rt_chk(s729_expr(up, rt_get(&F_r, v981_e)), "expr"); rt_arr(2, (wv[]){ t1989, t1990 }, 0); })), "expr"), 0);
    RT_RET(({ wv t1994 = v994_sq; wv t1995 = ({ wv t1991 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v981_e)), "expr")); wv t1992 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v994_sq))); wv t1993 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v994_sq))); rt_cat(7, (wv[]){ rt_s(&S552), t1991, rt_s(&S97), t1992, rt_s(&S405), t1993, rt_s(&S16) }); }); rt_chk(s699_wrap(up, t1994, t1995), "expr"); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S193)))) {
    RT_RET(({ wv t1999 = ({ wv t1996 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_c, v981_e)), "expr"))); wv t1997 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_a, v981_e)), "expr"))); wv t1998 = rt_text(rt_get(&F_c, rt_chk(s729_expr(up, rt_get(&F_b, v981_e)), "expr"))); rt_cat(7, (wv[]){ rt_s(&S598), t1996, rt_s(&S599), t1997, rt_s(&S569), t1998, rt_s(&S16) }); }); wv t2000 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1999, t2000 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S182)))) {
    wv v995_sq = rt_bind(rt_chk(s698_seq(up, rt_chk(s710_exprs(up, rt_get(&F_args, v981_e)), "expr")), "expr"), 0);
    RT_RET(({ wv t2003 = v995_sq; wv t2004 = ({ wv t2001 = rt_get(&F_callee, v981_e); wv t2002 = rt_get(&F_cs, v995_sq); rt_chk(s728_callC(up, t2001, t2002, rt_b(1)), "expr"); }); rt_chk(s699_wrap(up, t2003, t2004), "expr"); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S184)))) {
    RT_RET(({ wv t2005 = rt_chk(s723_chainExpr(up, v981_e), "expr"); wv t2006 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t2005, t2006 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S44)))) {
    RT_RET(({ wv t2007 = rt_chk(s727_matchExpr(up, v981_e), "expr"); wv t2008 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t2007, t2008 }); }));
  }
  if (rt_cond(rt_eq(v982_kind, rt_s(&S563)))) {
    RT_RET(({ wv t2009 = rt_get(&F_js, v981_e); wv t2010 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t2009, t2010 }); }));
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S600), rt_text(v982_kind) }));
  RT_RET(RT_NULL);
}

static wv s730_assemble(struct fr142 *up) {
  rt_enter("assemble");
  wv v996_all = rt_arr(3, (wv[]){ std_join(std_split(up->v647_runtime, rt_s(&S601)), rt_s(&S2)), rt_s(&S602), rt_s(&S13) }, 1);
  wv v997_k = rt_i(0);
  while (rt_cond(({ wv t2011 = v997_k; wv t2012 = std_length(rt_def(up->v650_strings, "strings")); rt_lt(t2011, t2012); }))) {
    (void)({ wv t2017 = v996_all; wv t2018 = ({ wv t2015 = rt_text(v997_k); wv t2016 = rt_text(rt_chk(w6_wstrInit(({ wv t2013 = v997_k; wv t2014 = rt_def(up->v650_strings, "strings"); rt_idx(t2013, t2014); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S603), t2015, rt_s(&S520), t2016, rt_s(&S14) }); }); std_push(t2017, t2018); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2019 = v997_k; wv t2020 = std_length(rt_def(up->v651_fields, "fields")); rt_lt(t2019, t2020); }))) {
    (void)({ wv t2027 = v996_all; wv t2028 = ({ wv t2025 = rt_text(({ wv t2021 = v997_k; wv t2022 = rt_def(up->v651_fields, "fields"); rt_idx(t2021, t2022); })); wv t2026 = rt_text(rt_chk(w6_wstrInit(({ wv t2023 = v997_k; wv t2024 = rt_def(up->v651_fields, "fields"); rt_idx(t2023, t2024); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S604), t2025, rt_s(&S520), t2026, rt_s(&S14) }); }); std_push(t2027, t2028); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2029 = v997_k; wv t2030 = std_length(rt_def(up->v652_shapes, "shapes")); rt_lt(t2029, t2030); }))) {
    wv v998_names = rt_bind(std_split(({ wv t2031 = v997_k; wv t2032 = rt_def(up->v652_shapes, "shapes"); rt_idx(t2031, t2032); }), rt_s(&S96)), 0);
    wv v999_refs = rt_arr(0, NULL, 1);
    wv v1000_j = rt_i(0);
    while (rt_cond(({ wv t2033 = v1000_j; wv t2034 = std_length(v998_names); rt_lt(t2033, t2034); }))) {
      (void)({ wv t2035 = v999_refs; wv t2036 = rt_cat(2, (wv[]){ rt_s(&S509), rt_text(rt_idx(v1000_j, v998_names)) }); std_push(t2035, t2036); });
      v1000_j = rt_add(v1000_j, rt_i(1));
    }
    (void)({ wv t2039 = v996_all; wv t2040 = ({ wv t2037 = rt_text(v997_k); wv t2038 = rt_text(std_join(v999_refs, rt_s(&S405))); rt_cat(5, (wv[]){ rt_s(&S605), t2037, rt_s(&S511), t2038, rt_s(&S512) }); }); std_push(t2039, t2040); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2041 = v997_k; wv t2042 = std_length(rt_def(up->v653_enumDefs, "enumDefs")); rt_lt(t2041, t2042); }))) {
    (void)({ wv t2045 = v996_all; wv t2046 = ({ wv t2043 = v997_k; wv t2044 = rt_def(up->v653_enumDefs, "enumDefs"); rt_idx(t2043, t2044); }); std_push(t2045, t2046); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2047 = v997_k; wv t2048 = std_length(rt_def(up->v655_frames, "frames")); rt_lt(t2047, t2048); }))) {
    wv v1001_f = rt_bind(({ wv t2049 = v997_k; wv t2050 = rt_def(up->v655_frames, "frames"); rt_idx(t2049, t2050); }), 0);
    wv v1002_members = rt_arr(0, NULL, 1);
    wv v1003_pf = rt_bind(rt_get(&F_parent, ({ wv t2051 = v1001_f; wv t2052 = rt_def(up->v662_fns, "fns"); rt_idx(t2051, t2052); })), 0);
    if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2053 = v1003_pf; wv t2054 = rt_def(up->v662_fns, "fns"); rt_idx(t2053, t2054); }))))) {
      (void)({ wv t2055 = v1002_members; wv t2056 = rt_cat(3, (wv[]){ rt_s(&S542), rt_text(v1003_pf), rt_s(&S606) }); std_push(t2055, t2056); });
    }
    wv v1004_j = rt_i(0);
    while (rt_cond(({ wv t2057 = v1004_j; wv t2058 = std_length(rt_def(up->v661_decls, "decls")); rt_lt(t2057, t2058); }))) {
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(({ wv t2061 = rt_get(&F_func, ({ wv t2059 = v1004_j; wv t2060 = rt_def(up->v661_decls, "decls"); rt_idx(t2059, t2060); })); wv t2062 = v1001_f; rt_eq(t2061, t2062); }), "&&") && rt_bool(rt_get(&F_captured, ({ wv t2063 = v1004_j; wv t2064 = rt_def(up->v661_decls, "decls"); rt_idx(t2063, t2064); })), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_kind, ({ wv t2065 = v1004_j; wv t2066 = rt_def(up->v661_decls, "decls"); rt_idx(t2065, t2066); })), rt_s(&S33)), "&&")))) {
        (void)({ wv t2069 = v1002_members; wv t2070 = rt_cat(3, (wv[]){ rt_s(&S519), rt_text(rt_get(&F_cname, ({ wv t2067 = v1004_j; wv t2068 = rt_def(up->v661_decls, "decls"); rt_idx(t2067, t2068); }))), rt_s(&S14) }); std_push(t2069, t2070); });
      }
      v1004_j = rt_add(v1004_j, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v1002_members), rt_i(0)))) {
      (void)std_push(v1002_members, rt_s(&S607));
    }
    (void)({ wv t2073 = v996_all; wv t2074 = ({ wv t2071 = rt_text(v1001_f); wv t2072 = rt_text(std_join(v1002_members, rt_s(&S0))); rt_cat(5, (wv[]){ rt_s(&S542), t2071, rt_s(&S608), t2072, rt_s(&S512) }); }); std_push(t2073, t2074); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2075 = v997_k; wv t2076 = std_length(rt_def(up->v654_globals, "globals")); rt_lt(t2075, t2076); }))) {
    (void)({ wv t2079 = v996_all; wv t2080 = rt_cat(3, (wv[]){ rt_s(&S545), rt_text(({ wv t2077 = v997_k; wv t2078 = rt_def(up->v654_globals, "globals"); rt_idx(t2077, t2078); })), rt_s(&S14) }); std_push(t2079, t2080); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2081 = v997_k; wv t2082 = std_length(rt_def(up->v656_functions, "functions")); rt_lt(t2081, t2082); }))) {
    (void)({ wv t2085 = v996_all; wv t2086 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2083 = v997_k; wv t2084 = rt_def(up->v656_functions, "functions"); rt_idx(t2083, t2084); }))), rt_s(&S14) }); std_push(t2085, t2086); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2087 = v997_k; wv t2088 = std_length(rt_def(up->v656_functions, "functions")); rt_lt(t2087, t2088); }))) {
    (void)std_push(v996_all, rt_s(&S13));
    (void)({ wv t2091 = v996_all; wv t2092 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2089 = v997_k; wv t2090 = rt_def(up->v656_functions, "functions"); rt_idx(t2089, t2090); }))), rt_s(&S609) }); std_push(t2091, t2092); });
    wv v1005_lines = rt_bind(rt_get(&F_lines, ({ wv t2093 = v997_k; wv t2094 = rt_def(up->v656_functions, "functions"); rt_idx(t2093, t2094); })), 0);
    wv v1006_j = rt_i(0);
    while (rt_cond(({ wv t2095 = v1006_j; wv t2096 = std_length(v1005_lines); rt_lt(t2095, t2096); }))) {
      (void)({ wv t2097 = v996_all; wv t2098 = rt_idx(v1006_j, v1005_lines); std_push(t2097, t2098); });
      v1006_j = rt_add(v1006_j, rt_i(1));
    }
    (void)std_push(v996_all, rt_s(&S15));
    v997_k = rt_add(v997_k, rt_i(1));
  }
  (void)std_push(v996_all, rt_s(&S13));
  (void)std_push(v996_all, rt_s(&S610));
  if (rt_cond(rt_gt(std_length(rt_def(up->v651_fields, "fields")), rt_i(0)))) {
    wv v1007_refs = rt_arr(0, NULL, 1);
    v997_k = rt_i(0);
    while (rt_cond(({ wv t2099 = v997_k; wv t2100 = std_length(rt_def(up->v651_fields, "fields")); rt_lt(t2099, t2100); }))) {
      (void)({ wv t2103 = v1007_refs; wv t2104 = rt_cat(2, (wv[]){ rt_s(&S509), rt_text(({ wv t2101 = v997_k; wv t2102 = rt_def(up->v651_fields, "fields"); rt_idx(t2101, t2102); })) }); std_push(t2103, t2104); });
      v997_k = rt_add(v997_k, rt_i(1));
    }
    (void)({ wv t2107 = v996_all; wv t2108 = ({ wv t2105 = rt_text(std_length(rt_def(up->v651_fields, "fields"))); wv t2106 = rt_text(std_join(v1007_refs, rt_s(&S405))); rt_cat(5, (wv[]){ rt_s(&S611), t2105, rt_s(&S612), t2106, rt_s(&S613) }); }); std_push(t2107, t2108); });
  }
  if (rt_cond(rt_gt(std_length(rt_def(up->v654_globals, "globals")), rt_i(0)))) {
    wv v1008_refs = rt_arr(0, NULL, 1);
    v997_k = rt_i(0);
    while (rt_cond(({ wv t2109 = v997_k; wv t2110 = std_length(rt_def(up->v654_globals, "globals")); rt_lt(t2109, t2110); }))) {
      (void)({ wv t2113 = v1008_refs; wv t2114 = rt_cat(2, (wv[]){ rt_s(&S91), rt_text(({ wv t2111 = v997_k; wv t2112 = rt_def(up->v654_globals, "globals"); rt_idx(t2111, t2112); })) }); std_push(t2113, t2114); });
      v997_k = rt_add(v997_k, rt_i(1));
    }
    (void)({ wv t2117 = v996_all; wv t2118 = ({ wv t2115 = rt_text(std_length(rt_def(up->v654_globals, "globals"))); wv t2116 = rt_text(std_join(v1008_refs, rt_s(&S405))); rt_cat(5, (wv[]){ rt_s(&S614), t2115, rt_s(&S615), t2116, rt_s(&S613) }); }); std_push(t2117, t2118); });
  }
  v997_k = rt_i(0);
  while (rt_cond(({ wv t2119 = v997_k; wv t2120 = std_length(rt_def(up->v657_out, "out")); rt_lt(t2119, t2120); }))) {
    (void)({ wv t2123 = v996_all; wv t2124 = ({ wv t2121 = v997_k; wv t2122 = rt_def(up->v657_out, "out"); rt_idx(t2121, t2122); }); std_push(t2123, t2124); });
    v997_k = rt_add(v997_k, rt_i(1));
  }
  (void)std_push(v996_all, rt_s(&S15));
  (void)std_push(v996_all, rt_s(&S13));
  RT_RET(std_join(v996_all, rt_s(&S2)));
}

static wv w7_dumpTokens(wv v1011_toks, wv v1012_indent) {
  rt_enter("dumpTokens");
  v1011_toks = rt_bind(v1011_toks, 0);
  v1012_indent = rt_bind(v1012_indent, 0);
  wv v1013_k = rt_i(0);
  while (rt_cond(({ wv t2135 = v1013_k; wv t2136 = std_length(v1011_toks); rt_lt(t2135, t2136); }))) {
    wv v1014_tk = rt_bind(rt_idx(v1013_k, v1011_toks), 0);
    wv v1015_line = ({ wv t2137 = rt_text(v1012_indent); wv t2138 = rt_text(rt_get(&F_t, v1014_tk)); wv t2139 = rt_text(rt_get(&F_pos, v1014_tk)); wv t2140 = rt_text(rt_get(&F_end, v1014_tk)); wv t2141 = rt_text(rt_get(&F_sp, v1014_tk)); rt_cat(8, (wv[]){ t2137, t2138, rt_s(&S0), t2139, rt_s(&S0), t2140, rt_s(&S0), t2141 }); });
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1014_tk), rt_s(&S137)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1014_tk), rt_s(&S156)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1014_tk), rt_s(&S111)), "||")))) {
      v1015_line = ({ wv t2142 = v1015_line; wv t2143 = rt_cat(2, (wv[]){ rt_s(&S0), rt_text(rt_get(&F_v, v1014_tk)) }); rt_add(t2142, t2143); });
    } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1014_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1014_tk), rt_s(&S148)), "||")))) {
      v1015_line = ({ wv t2146 = v1015_line; wv t2147 = ({ wv t2144 = rt_text(rt_get(&F_text, v1014_tk)); wv t2145 = rt_text(rt_get(&F_v, v1014_tk)); rt_cat(4, (wv[]){ rt_s(&S0), t2144, rt_s(&S0), t2145 }); }); rt_add(t2146, t2147); });
    }
    (void)std_print(v1015_line);
    if (rt_cond(rt_eq(rt_get(&F_t, v1014_tk), rt_s(&S171)))) {
      wv v1016_p = rt_i(0);
      while (rt_cond(({ wv t2148 = v1016_p; wv t2149 = std_length(rt_get(&F_parts, v1014_tk)); rt_lt(t2148, t2149); }))) {
        wv v1017_part = rt_bind(({ wv t2150 = v1016_p; wv t2151 = rt_get(&F_parts, v1014_tk); rt_idx(t2150, t2151); }), 0);
        if (rt_cond(rt_eq(std_typeOf(v1017_part), rt_s(&S185)))) {
          (void)std_print(({ wv t2152 = rt_text(v1012_indent); wv t2153 = rt_text(rt_chk(w1_quote(v1017_part), "dumpTokens")); rt_cat(3, (wv[]){ t2152, rt_s(&S616), t2153 }); }));
        } else {
          (void)std_print(({ wv t2154 = rt_text(v1012_indent); wv t2155 = rt_text(rt_get(&F_pos, v1017_part)); rt_cat(3, (wv[]){ t2154, rt_s(&S617), t2155 }); }));
          (void)({ wv t2156 = rt_get(&F_tokens, v1017_part); wv t2157 = rt_add(v1012_indent, rt_s(&S618)); rt_chk(w7_dumpTokens(t2156, t2157), "dumpTokens"); });
          (void)std_print(rt_cat(2, (wv[]){ rt_text(v1012_indent), rt_s(&S619) }));
        }
        v1016_p = rt_add(v1016_p, rt_i(1));
      }
    }
    v1013_k = rt_add(v1013_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w7_sorted(wv v1018_xs) {
  rt_enter("sorted");
  v1018_xs = rt_bind(v1018_xs, 0);
  wv v1019_out = rt_arr(0, NULL, 1);
  wv v1020_k = rt_i(0);
  while (rt_cond(({ wv t2158 = v1020_k; wv t2159 = std_length(v1018_xs); rt_lt(t2158, t2159); }))) {
    (void)({ wv t2160 = v1019_out; wv t2161 = rt_idx(v1020_k, v1018_xs); std_push(t2160, t2161); });
    wv v1021_j = rt_sub(std_length(v1019_out), rt_i(1));
    while (rt_cond(rt_b(rt_bool(rt_gt(v1021_j, rt_i(0)), "&&") && rt_bool(({ wv t2164 = ({ wv t2162 = rt_sub(v1021_j, rt_i(1)); wv t2163 = v1019_out; rt_idx(t2162, t2163); }); wv t2165 = rt_idx(v1021_j, v1019_out); rt_gt(t2164, t2165); }), "&&")))) {
      wv v1022_swap = rt_bind(rt_idx(v1021_j, v1019_out), 0);
      ({ wv t2168 = ({ wv t2166 = rt_sub(v1021_j, rt_i(1)); wv t2167 = v1019_out; rt_idx(t2166, t2167); }); wv t2169 = v1021_j; wv t2170 = v1019_out; rt_set_idx(t2168, t2169, t2170); });
      ({ wv t2171 = v1022_swap; wv t2172 = rt_sub(v1021_j, rt_i(1)); wv t2173 = v1019_out; rt_set_idx(t2171, t2172, t2173); });
      v1021_j = rt_sub(v1021_j, rt_i(1));
    }
    v1020_k = rt_add(v1020_k, rt_i(1));
  }
  RT_RET(v1019_out);
}

static wv w7_dumpValue(wv v1023_x, wv v1024_indent) {
  rt_enter("dumpValue");
  v1023_x = rt_bind(v1023_x, 0);
  v1024_indent = rt_bind(v1024_indent, 0);
  wv v1025_type = rt_bind(std_typeOf(v1023_x), 0);
  if (rt_cond(rt_eq(v1025_type, rt_s(&S185)))) {
    RT_RET(rt_chk(w1_quote(v1023_x), "dumpValue"));
  }
  wv v1026_inner = rt_add(v1024_indent, rt_s(&S507));
  if (rt_cond(rt_eq(v1025_type, rt_s(&S620)))) {
    if (rt_cond(rt_eq(std_length(v1023_x), rt_i(0)))) {
      RT_RET(rt_s(&S621));
    }
    wv v1027_out = rt_s(&S98);
    wv v1028_k = rt_i(0);
    while (rt_cond(({ wv t2174 = v1028_k; wv t2175 = std_length(v1023_x); rt_lt(t2174, t2175); }))) {
      v1027_out = ({ wv t2180 = ({ wv t2176 = rt_add(v1027_out, rt_s(&S2)); wv t2177 = v1026_inner; rt_add(t2176, t2177); }); wv t2181 = ({ wv t2178 = rt_idx(v1028_k, v1023_x); wv t2179 = v1026_inner; rt_chk(w7_dumpValue(t2178, t2179), "dumpValue"); }); rt_add(t2180, t2181); });
      v1028_k = rt_add(v1028_k, rt_i(1));
    }
    RT_RET(rt_add(({ wv t2182 = rt_add(v1027_out, rt_s(&S2)); wv t2183 = v1024_indent; rt_add(t2182, t2183); }), rt_s(&S99)));
  }
  if (rt_cond(rt_eq(v1025_type, rt_s(&S622)))) {
    wv v1029_keys = rt_bind(rt_chk(w7_sorted(std_fields(v1023_x)), "dumpValue"), 0);
    wv v1030_out = rt_s(&S100);
    wv v1031_k = rt_i(0);
    while (rt_cond(({ wv t2184 = v1031_k; wv t2185 = std_length(v1029_keys); rt_lt(t2184, t2185); }))) {
      wv v1032_key = rt_bind(rt_idx(v1031_k, v1029_keys), 0);
      v1030_out = ({ wv t2192 = ({ wv t2188 = v1030_out; wv t2189 = ({ wv t2186 = rt_text(v1026_inner); wv t2187 = rt_text(v1032_key); rt_cat(4, (wv[]){ rt_s(&S2), t2186, t2187, rt_s(&S29) }); }); rt_add(t2188, t2189); }); wv t2193 = ({ wv t2190 = rt_idx(v1032_key, v1023_x); wv t2191 = v1026_inner; rt_chk(w7_dumpValue(t2190, t2191), "dumpValue"); }); rt_add(t2192, t2193); });
      v1031_k = rt_add(v1031_k, rt_i(1));
    }
    RT_RET(({ wv t2194 = v1030_out; wv t2195 = rt_cat(3, (wv[]){ rt_s(&S2), rt_text(v1024_indent), rt_s(&S15) }); rt_add(t2194, t2195); }));
  }
  RT_RET(std_toString(v1023_x));
}

static wv w8_dirOf(wv v1033_path) {
  rt_enter("dirOf");
  v1033_path = rt_bind(v1033_path, 0);
  wv v1034_cs = rt_bind(std_split(v1033_path, rt_s(&S13)), 0);
  wv v1035_k = rt_sub(std_length(v1034_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_ge(v1035_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1035_k, v1034_cs), rt_s(&S87)), "&&")))) {
    v1035_k = rt_sub(v1035_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2196 = v1034_cs; wv t2197 = rt_add(v1035_k, rt_i(1)); std_slice(t2196, rt_i(0), t2197); }), rt_s(&S13)));
}

static wv w8_normalize(wv v1036_path) {
  rt_enter("normalize");
  v1036_path = rt_bind(v1036_path, 0);
  wv v1037_segs = rt_bind(std_split(v1036_path, rt_s(&S87)), 0);
  wv v1038_out = rt_arr(0, NULL, 1);
  wv v1039_k = rt_i(0);
  while (rt_cond(({ wv t2198 = v1039_k; wv t2199 = std_length(v1037_segs); rt_lt(t2198, t2199); }))) {
    wv v1040_seg = rt_bind(rt_idx(v1039_k, v1037_segs), 0);
    v1039_k = rt_add(v1039_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(v1040_seg, rt_s(&S129)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1040_seg, rt_s(&S13)), "&&") && rt_bool(rt_gt(std_length(v1038_out), rt_i(0)), "&&")), "||")))) {
      continue;
    }
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1040_seg, rt_s(&S623)), "&&") && rt_bool(rt_gt(std_length(v1038_out), rt_i(0)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1038_out), rt_s(&S623)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1038_out), rt_s(&S13)), "&&")))) {
      (void)std_pop(v1038_out);
    } else {
      (void)std_push(v1038_out, v1040_seg);
    }
  }
  RT_RET(std_join(v1038_out, rt_s(&S87)));
}

static wv w8_with(wv v1041_xs, wv v1042_x) {
  rt_enter("with");
  v1041_xs = rt_bind(v1041_xs, 0);
  v1042_x = rt_bind(v1042_x, 0);
  wv v1043_out = rt_arr(0, NULL, 1);
  wv v1044_k = rt_i(0);
  while (rt_cond(({ wv t2200 = v1044_k; wv t2201 = std_length(v1041_xs); rt_lt(t2200, t2201); }))) {
    (void)({ wv t2202 = v1043_out; wv t2203 = rt_idx(v1044_k, v1041_xs); std_push(t2202, t2203); });
    v1044_k = rt_add(v1044_k, rt_i(1));
  }
  (void)std_push(v1043_out, v1042_x);
  RT_RET(v1043_out);
}

static wv w8_failIn(wv v1045_file, wv v1046_chars, wv v1047_pos, wv v1048_message) {
  rt_enter("failIn");
  v1045_file = rt_bind(v1045_file, 0);
  v1046_chars = rt_bind(v1046_chars, 0);
  v1047_pos = rt_bind(v1047_pos, 0);
  v1048_message = rt_bind(v1048_message, 0);
  (void)rt_chk(w2_setSource(v1045_file, v1046_chars), "failIn");
  (void)rt_chk(w2_abort(v1047_pos, v1048_message), "failIn");
  RT_RET(RT_NULL);
}

static wv w8_loadProgram(wv v1049_entryFile, wv v1050_entryChars) {
  rt_enter("loadProgram");
  struct fr212 fr = { 0 };
  v1049_entryFile = rt_bind(v1049_entryFile, 0);
  fr.v1051_modules = rt_arr(0, NULL, 1);
  wv v1054_path = rt_bind(rt_chk(w8_normalize(std_join(std_split(v1049_entryFile, rt_s(&S19)), rt_s(&S87))), "loadProgram"), 0);
  (void)({ wv t2254 = v1049_entryFile; wv t2255 = v1050_entryChars; wv t2256 = v1054_path; wv t2257 = rt_arr(1, (wv[]){ v1054_path }, 0); rt_chk(s1053_visit(&fr, t2254, t2255, t2256, t2257), "loadProgram"); });
  RT_RET(fr.v1051_modules);
}

static wv s1052_loaded(struct fr212 *up, wv v1055_path) {
  rt_enter("loaded");
  v1055_path = rt_bind(v1055_path, 0);
  wv v1056_k = rt_i(0);
  while (rt_cond(({ wv t2204 = v1056_k; wv t2205 = std_length(rt_def(up->v1051_modules, "modules")); rt_lt(t2204, t2205); }))) {
    if (rt_cond(({ wv t2208 = rt_get(&F_path, ({ wv t2206 = v1056_k; wv t2207 = rt_def(up->v1051_modules, "modules"); rt_idx(t2206, t2207); })); wv t2209 = v1055_path; rt_eq(t2208, t2209); }))) {
      RT_RET(v1056_k);
    }
    v1056_k = rt_add(v1056_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s1053_visit(struct fr212 *up, wv v1057_file, wv v1058_chars, wv v1059_path, wv v1060_chain) {
  rt_enter("visit");
  v1057_file = rt_bind(v1057_file, 0);
  v1058_chars = rt_bind(v1058_chars, 0);
  v1059_path = rt_bind(v1059_path, 0);
  v1060_chain = rt_bind(v1060_chain, 0);
  (void)rt_chk(w2_setSource(v1057_file, v1058_chars), "visit");
  wv v1061_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(v1058_chars), "visit")), "visit"), 0);
  wv v1062_imports = rt_arr(0, NULL, 1);
  wv v1063_k = rt_i(0);
  while (rt_cond(({ wv t2210 = v1063_k; wv t2211 = std_length(rt_get(&F_stmts, v1061_program)); rt_lt(t2210, t2211); }))) {
    wv v1066_s = rt_bind(({ wv t2212 = v1063_k; wv t2213 = rt_get(&F_stmts, v1061_program); rt_idx(t2212, t2213); }), 0);
    v1063_k = rt_add(v1063_k, rt_i(1));
    if (rt_cond(rt_ne(rt_get(&F_k, v1066_s), rt_s(&S47)))) {
      continue;
    }
    wv v1067_libPath = rt_bind(rt_chk(w8_normalize(({ wv t2214 = rt_chk(w8_dirOf(v1059_path), "visit"); wv t2215 = rt_get(&F_path, v1066_s); rt_add(t2214, t2215); })), "visit"), 0);
    wv v1068_at = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t2216 = v1068_at; wv t2217 = std_length(v1060_chain); rt_lt(t2216, t2217); }), "&&") && rt_bool(({ wv t2218 = rt_idx(v1068_at, v1060_chain); wv t2219 = v1067_libPath; rt_ne(t2218, t2219); }), "&&")))) {
      v1068_at = rt_add(v1068_at, rt_i(1));
    }
    if (rt_cond(({ wv t2220 = v1068_at; wv t2221 = std_length(v1060_chain); rt_lt(t2220, t2221); }))) {
      wv v1070_cycle = rt_bind(({ wv t2225 = ({ wv t2222 = v1060_chain; wv t2223 = v1068_at; wv t2224 = std_length(v1060_chain); std_slice(t2222, t2223, t2224); }); wv t2226 = v1067_libPath; rt_chk(w8_with(t2225, t2226), "visit"); }), 0);
      (void)({ wv t2227 = v1057_file; wv t2228 = v1058_chars; wv t2229 = rt_get(&F_pathPos, v1066_s); wv t2230 = rt_cat(2, (wv[]){ rt_s(&S624), rt_text(std_join(v1070_cycle, rt_s(&S625))) }); rt_chk(w8_failIn(t2227, t2228, t2229, t2230), "visit"); });
    }
    wv v1069_lib = rt_bind(rt_chk(s1052_loaded(up, v1067_libPath), "visit"), 1);
    if (rt_cond(rt_eq(v1069_lib, RT_NULL))) {
      wv t2231 = std_readFile(v1067_libPath);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          (void)({ wv t2232 = v1057_file; wv t2233 = v1058_chars; wv t2234 = rt_get(&F_pathPos, v1066_s); wv t2235 = rt_cat(3, (wv[]){ rt_s(&S626), rt_text(v1067_libPath), rt_s(&S113) }); rt_chk(w8_failIn(t2232, t2233, t2234, t2235), "visit"); });
          t2231 = RT_NULL;
        }
      }
      wv v1071_text = rt_bind(t2231, 0);
      wv v1072_cs = rt_bind(std_split(v1071_text, rt_s(&S13)), 0);
      wv v1073_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1072_cs), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1072_cs), rt_s(&S627)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
      v1069_lib = rt_bind(({ wv t2239 = v1067_libPath; wv t2240 = ({ wv t2236 = v1072_cs; wv t2237 = v1073_bom; wv t2238 = std_length(v1072_cs); std_slice(t2236, t2237, t2238); }); wv t2241 = v1067_libPath; wv t2242 = rt_chk(w8_with(v1060_chain, v1067_libPath), "visit"); rt_chk(s1053_visit(up, t2239, t2240, t2241, t2242), "visit"); }), 1);
    }
    (void)({ wv t2243 = v1062_imports; wv t2244 = rt_rec(0, 2, K58, (wv[]){ v1066_s, v1069_lib }); std_push(t2243, t2244); });
  }
  wv v1064_refTargets = rt_arr(0, NULL, 1);
  wv v1065_mod = ({ wv t2245 = v1057_file; wv t2246 = v1058_chars; wv t2247 = v1059_path; wv t2248 = v1061_program; wv t2249 = v1062_imports; wv t2250 = rt_arr(0, NULL, 0); wv t2251 = v1064_refTargets; rt_rec(1, 8, K59, (wv[]){ t2245, t2246, t2247, t2248, t2249, t2250, rt_b(0), t2251 }); });
  (void)({ wv t2252 = rt_def(up->v1051_modules, "modules"); wv t2253 = v1065_mod; std_push(t2252, t2253); });
  RT_RET(rt_sub(std_length(rt_def(up->v1051_modules, "modules")), rt_i(1)));
}

static wv w_die(void) {
  rt_enter("die");
  (void)std_eprint(rt_def(w_usage, "usage"));
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w_load(wv v1074_file) {
  rt_enter("load");
  v1074_file = rt_bind(v1074_file, 0);
  wv t2258 = std_readFile(v1074_file);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S633), rt_text(v1074_file) }));
      rt_exit(rt_i(2));
      t2258 = RT_NULL;
    }
  }
  wv v1075_text = rt_bind(t2258, 0);
  wv v1076_chars = rt_bind(std_split(v1075_text, rt_s(&S13)), 0);
  wv v1077_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1076_chars), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1076_chars), rt_s(&S627)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
  (void)({ wv t2262 = v1074_file; wv t2263 = ({ wv t2259 = v1076_chars; wv t2260 = v1077_bom; wv t2261 = std_length(v1076_chars); std_slice(t2259, t2260, t2261); }); rt_chk(w2_setSource(t2262, t2263), "load"); });
  RT_RET(RT_NULL);
}

static wv w_onWindows(void) {
  rt_enter("onWindows");
  wv t2268 = rt_s(&S640);
  wv t2269 = std_env(t2268);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2269 = rt_s(&S13);
    }
  }
  wv v1079_os = rt_bind(t2269, 0);
  RT_RET(rt_eq(v1079_os, rt_s(&S641)));
}

static wv w_tryRead(wv v1080_path) {
  rt_enter("tryRead");
  v1080_path = rt_bind(v1080_path, 0);
  wv t2276 = std_readFile(v1080_path);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2276 = RT_NULL;
    }
  }
  wv v1081_text = rt_bind(t2276, 0);
  RT_RET(v1081_text);
}

static wv w_programDir(void) {
  rt_enter("programDir");
  wv v1082_cs = rt_bind(std_split(std_programPath(), rt_s(&S13)), 0);
  wv v1083_k = rt_sub(std_length(v1082_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v1083_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1083_k, v1082_cs), rt_s(&S87)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(v1083_k, v1082_cs), rt_s(&S19)), "&&")))) {
    v1083_k = rt_sub(v1083_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2277 = v1082_cs; wv t2278 = rt_add(v1083_k, rt_i(1)); std_slice(t2277, rt_i(0), t2278); }), rt_s(&S13)));
}

static wv w_readRuntime(void) {
  rt_enter("readRuntime");
  wv v1084_text = rt_bind(rt_chk(w_tryRead(rt_def(w_runtimePath, "runtimePath")), "readRuntime"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1084_text, RT_NULL), "||") || rt_bool(rt_def(w_runtimeGiven, "runtimeGiven"), "||")))) {
    RT_RET(v1084_text);
  }
  wv v1085_dir = rt_bind(rt_chk(w_programDir(), "readRuntime"), 0);
  wv v1086_here = rt_bind(rt_chk(w_tryRead(({ wv t2279 = v1085_dir; wv t2280 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2279, t2280); })), "readRuntime"), 0);
  RT_RET((rt_cond(rt_ne(v1086_here, RT_NULL)) ? v1086_here : rt_chk(w_tryRead(({ wv t2281 = rt_add(v1085_dir, rt_s(&S648)); wv t2282 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2281, t2282); })), "readRuntime")));
}

static void wvt_main(void) {
  rt_syms(88, (wstr *const[]){ &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars, &F_word, &F_t, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts, &F_tokens, &F_k, &F_e, &F_handler, &F_items, &F_fields, &F_index, &F_arr, &F_rec, &F_l, &F_r, &F_c, &F_a, &F_b, &F_stmts, &F_param, &F_paramPos, &F_body, &F_neg, &F_cond, &F_isVar, &F_isRef, &F_name, &F_namePos, &F_op, &F_target, &F_code, &F_arg, &F_paren, &F_path, &F_pathPos, &F_names, &F_params, &F_members, &F_value, &F_close, &F_field, &F_callee, &F_args, &F_values, &F_subject, &F_cases, &F_els, &F_branches, &F_arity, &F_mod, &F_parent, &F_kind, &F_visibleAt, &F_fnId, &F_lib, &F_id, &F_func, &F_top, &F_loops, &F_key, &F_exports, &F_refTargets, &F_useMod, &F_imports, &F_node, &F_notLibrary, &F_program, &F_children, &F_d, &F_depth, &F_cname, &F_captured, &F_fnIndex, &F_pre, &F_cs, &F_proto, &F_lines, &F_js });
  rt_roots(50, (wv *const[]){ &w2_srcFile, &w2_srcChars, &w2_warnings, &w3_keywords, &w3_reserved, &w3_bangWords, &w3_setOps, &w3_ops, &w3_maxInt, &w4_exprStart, &w4_onfailOk, &w4_callees, &w4_exportable, &w5_stdlib, &w5_neverCompletes, &w5_onfailValueUsed, &w5_libraryTop, &w6_binOps, &w6_binFns, &w6_scalarKinds, &w6_plainKinds, &w6_signalers, &w6_constK, &w6_pureK, &w6_effectK, &w_usage, &w_argv, &w_mode, &w_input, &w_output, &w_runtimePath, &w_cc, &w_arg, &w_inspect, &w_dotWvt, &w_runtimeGiven, &w_modules, &w_problems, &w_runtime, &w_code, &w_dotExe, &w_cFile, &w_parts, &w_ccName, &w_flags, &w_status, &v1078_a, &v1087_tokens, &v1088_program, &v1089_p });
  w2_srcFile = rt_s(&S13);
  w2_srcChars = rt_arr(0, NULL, 0);
  w2_warnings = rt_arr(0, NULL, 1);
  w3_keywords = rt_arr(25, (wv[]){ rt_s(&S33), rt_s(&S34), rt_s(&S35), rt_s(&S36), rt_s(&S37), rt_s(&S38), rt_s(&S39), rt_s(&S40), rt_s(&S41), rt_s(&S42), rt_s(&S43), rt_s(&S44), rt_s(&S45), rt_s(&S46), rt_s(&S47), rt_s(&S48), rt_s(&S49), rt_s(&S50), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S54), rt_s(&S55), rt_s(&S56), rt_s(&S57) }, 0);
  w3_reserved = ({ wv t68 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S58), rt_s(&S59) }); wv t69 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S60), rt_s(&S61) }); wv t70 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S62), rt_s(&S63) }); rt_arr(3, (wv[]){ t68, t69, t70 }, 0); });
  w3_bangWords = rt_arr(2, (wv[]){ rt_s(&S39), rt_s(&S41) }, 0);
  w3_setOps = rt_arr(11, (wv[]){ rt_s(&S64), rt_s(&S65), rt_s(&S66), rt_s(&S67), rt_s(&S68), rt_s(&S69), rt_s(&S70), rt_s(&S71), rt_s(&S72), rt_s(&S73), rt_s(&S74) }, 0);
  w3_ops = rt_arr(31, (wv[]){ rt_s(&S75), rt_s(&S76), rt_s(&S77), rt_s(&S78), rt_s(&S79), rt_s(&S80), rt_s(&S81), rt_s(&S82), rt_s(&S83), rt_s(&S84), rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S89), rt_s(&S90), rt_s(&S91), rt_s(&S92), rt_s(&S32), rt_s(&S93), rt_s(&S94), rt_s(&S95), rt_s(&S28), rt_s(&S14), rt_s(&S96), rt_s(&S97), rt_s(&S16), rt_s(&S98), rt_s(&S99), rt_s(&S100), rt_s(&S15) }, 0);
  w3_maxInt = rt_i(9007199254740991);
  w4_exprStart = rt_arr(19, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S171), rt_s(&S49), rt_s(&S50), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S137), rt_s(&S156), rt_s(&S97), rt_s(&S98), rt_s(&S100), rt_s(&S39), rt_s(&S175), rt_s(&S44), rt_s(&S85), rt_s(&S94), rt_s(&S93) }, 0);
  w4_onfailOk = rt_arr(6, (wv[]){ rt_s(&S176), rt_s(&S177), rt_s(&S38), rt_s(&S34), rt_s(&S57), rt_s(&S54) }, 0);
  w4_callees = rt_arr(2, (wv[]){ rt_s(&S137), rt_s(&S156) }, 0);
  w4_exportable = rt_arr(5, (wv[]){ rt_s(&S33), rt_s(&S35), rt_s(&S36), rt_s(&S37), rt_s(&S46) }, 0);
  w5_stdlib = ({ wv t534 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S332), rt_i(1) }); wv t535 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S333), rt_i(1) }); wv t536 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S334), rt_i(1) }); wv t537 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S335), rt_i(0) }); wv t538 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S336), rt_i(0) }); wv t539 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S337), rt_i(1) }); wv t540 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S338), rt_i(2) }); wv t541 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S339), rt_i(2) }); wv t542 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S340), rt_i(1) }); wv t543 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S341), rt_i(1) }); wv t544 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S342), rt_i(1) }); wv t545 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S343), rt_s(&S35) }); wv t546 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S344), rt_i(1) }); wv t547 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S345), rt_s(&S35) }); wv t548 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S346), rt_i(2) }); wv t549 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S347), rt_i(1) }); wv t550 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S348), rt_i(2) }); wv t551 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S349), rt_i(3) }); wv t552 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S350), rt_i(1) }); wv t553 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S351), rt_i(1) }); wv t554 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S352), rt_i(2) }); wv t555 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S353), rt_i(2) }); wv t556 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S354), rt_i(1) }); wv t557 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S355), rt_i(1) }); wv t558 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S356), rt_i(1) }); wv t559 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S357), rt_i(2) }); wv t560 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S358), rt_i(1) }); wv t561 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S359), rt_i(2) }); wv t562 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S360), rt_i(1) }); wv t563 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S361), rt_i(2) }); wv t564 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S362), rt_i(1) }); wv t565 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S363), rt_i(2) }); wv t566 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S364), rt_i(2) }); wv t567 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S365), rt_i(2) }); wv t568 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S366), rt_i(1) }); wv t569 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S367), rt_i(1) }); wv t570 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S368), rt_i(1) }); wv t571 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S369), rt_i(1) }); wv t572 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S370), rt_i(1) }); wv t573 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S371), rt_i(1) }); wv t574 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S372), rt_i(1) }); wv t575 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S373), rt_i(1) }); wv t576 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S374), rt_i(1) }); wv t577 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S375), rt_i(1) }); wv t578 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S376), rt_i(0) }); wv t579 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S377), rt_i(1) }); wv t580 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S378), rt_i(2) }); wv t581 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S379), rt_i(1) }); rt_arr(48, (wv[]){ t534, t535, t536, t537, t538, t539, t540, t541, t542, t543, t544, t545, t546, t547, t548, t549, t550, t551, t552, t553, t554, t555, t556, t557, t558, t559, t560, t561, t562, t563, t564, t565, t566, t567, t568, t569, t570, t571, t572, t573, t574, t575, t576, t577, t578, t579, t580, t581 }, 0); });
  w5_neverCompletes = rt_arr(6, (wv[]){ rt_s(&S34), rt_s(&S57), rt_s(&S54), rt_s(&S55), rt_s(&S42), rt_s(&S43) }, 0);
  w5_onfailValueUsed = rt_arr(5, (wv[]){ rt_s(&S177), rt_s(&S38), rt_s(&S34), rt_s(&S57), rt_s(&S54) }, 0);
  w5_libraryTop = rt_arr(6, (wv[]){ rt_s(&S201), rt_s(&S47), rt_s(&S33), rt_s(&S177), rt_s(&S46), rt_s(&S204) }, 0);
  w6_binOps = rt_arr(17, (wv[]){ rt_s(&S84), rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S75), rt_s(&S76), rt_s(&S77), rt_s(&S89), rt_s(&S90), rt_s(&S78), rt_s(&S79), rt_s(&S91), rt_s(&S92), rt_s(&S32), rt_s(&S82), rt_s(&S83) }, 0);
  w6_binFns = rt_arr(17, (wv[]){ rt_s(&S478), rt_s(&S479), rt_s(&S480), rt_s(&S481), rt_s(&S482), rt_s(&S483), rt_s(&S484), rt_s(&S485), rt_s(&S486), rt_s(&S487), rt_s(&S488), rt_s(&S489), rt_s(&S490), rt_s(&S491), rt_s(&S492), rt_s(&S493), rt_s(&S494) }, 0);
  w6_scalarKinds = rt_arr(9, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S171), rt_s(&S279), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S192), rt_s(&S191) }, 0);
  w6_plainKinds = rt_arr(8, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S279), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S212), rt_s(&S156) }, 0);
  w6_signalers = rt_arr(13, (wv[]){ rt_s(&S335), rt_s(&S336), rt_s(&S363), rt_s(&S365), rt_s(&S337), rt_s(&S338), rt_s(&S339), rt_s(&S341), rt_s(&S342), rt_s(&S344), rt_s(&S346), rt_s(&S355), rt_s(&S356) }, 0);
  w6_constK = rt_i(0);
  w6_pureK = rt_i(1);
  w6_effectK = rt_i(2);
  w_usage = rt_add(rt_add(rt_add(rt_add(rt_s(&S628), rt_s(&S629)), rt_s(&S630)), rt_s(&S631)), rt_s(&S632));
  w_argv = rt_bind(std_args(), 0);
  w_mode = rt_s(&S13);
  w_input = rt_s(&S13);
  w_output = rt_s(&S13);
  w_runtimePath = rt_s(&S13);
  w_cc = rt_s(&S13);
  w_arg = rt_i(0);
  while (rt_cond(({ wv t2264 = w_arg; wv t2265 = std_length(w_argv); rt_lt(t2264, t2265); }))) {
    v1078_a = rt_bind(rt_idx(w_arg, w_argv), 0);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_arg, rt_i(0)), "&&") && rt_bool(rt_eq(v1078_a, rt_s(&S634)), "&&")))) {
      w_mode = rt_bind(v1078_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v1078_a, rt_s(&S635)), "||") || rt_bool(rt_eq(v1078_a, rt_s(&S636)), "||")))) {
      w_mode = rt_bind(v1078_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1078_a, rt_s(&S637)), "||") || rt_bool(rt_eq(v1078_a, rt_s(&S638)), "||")), "||") || rt_bool(rt_eq(v1078_a, rt_s(&S639)), "||")))) {
      w_arg = rt_add(w_arg, rt_i(1));
      if (rt_cond(({ wv t2266 = w_arg; wv t2267 = std_length(w_argv); rt_ge(t2266, t2267); }))) {
        (void)rt_chk(w_die(), NULL);
      }
      if (rt_cond(rt_eq(v1078_a, rt_s(&S637)))) {
        w_output = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1078_a, rt_s(&S638)))) {
        w_runtimePath = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else {
        w_cc = rt_bind(rt_idx(w_arg, w_argv), 1);
      }
    } else if (rt_cond(rt_b(rt_bool(rt_ne(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(rt_gt(std_length(v1078_a), rt_i(0)), "&&") && rt_bool(rt_eq(std_at(v1078_a, rt_i(0)), rt_s(&S85)), "&&")), "||")))) {
      (void)rt_chk(w_die(), NULL);
    } else {
      w_input = rt_bind(v1078_a, 1);
    }
    w_arg = rt_add(w_arg, rt_i(1));
  }
  w_inspect = rt_b(rt_bool(rt_eq(w_mode, rt_s(&S635)), "||") || rt_bool(rt_eq(w_mode, rt_s(&S636)), "||"));
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(w_inspect, "&&") && rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S634)), "&&") && rt_bool(rt_ne(w_cc, rt_s(&S13)), "&&")), "||")))) {
    (void)rt_chk(w_die(), NULL);
  }
  w_dotWvt = rt_b(rt_bool(rt_ge(std_length(w_input), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t2270 = w_input; wv t2271 = std_length(w_input); std_slice(t2270, rt_i(-4), t2271); }), rt_s(&S232)), "&&"));
  if (rt_cond(rt_eq(w_mode, rt_s(&S634)))) {
    if (rt_cond(rt_eq(w_cc, rt_s(&S13)))) {
      wv t2272 = rt_s(&S642);
      wv t2273 = std_env(t2272);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          t2273 = rt_s(&S643);
        }
      }
      w_cc = rt_bind(t2273, 1);
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(w_output, rt_s(&S13)), "&&") && rt_bool(rt_not(w_dotWvt), "&&")))) {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S644), rt_text(w_input), rt_s(&S645) }));
      rt_exit(rt_i(2));
    }
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = ({ wv t2274 = std_slice(w_input, rt_i(0), rt_i(-4)); wv t2275 = (rt_cond(rt_chk(w_onWindows(), NULL)) ? rt_s(&S646) : rt_s(&S13)); rt_add(t2274, t2275); });
    }
  }
  w_runtimeGiven = rt_ne(w_runtimePath, rt_s(&S13));
  if (rt_cond(rt_not(w_runtimeGiven))) {
    w_runtimePath = rt_s(&S647);
  }
  (void)rt_chk(w_load(w_input), NULL);
  if (rt_cond(rt_eq(w_mode, rt_s(&S635)))) {
    v1087_tokens = rt_bind(rt_chk(w3_lex(w2_srcChars), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)rt_chk(w7_dumpTokens(v1087_tokens, rt_s(&S13)), NULL);
    rt_exit(rt_i(0));
  }
  if (rt_cond(rt_eq(w_mode, rt_s(&S636)))) {
    v1088_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(w2_srcChars), NULL)), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)std_print(rt_chk(w7_dumpValue(v1088_program, rt_s(&S13)), NULL));
    rt_exit(rt_i(0));
  }
  w_modules = rt_bind(rt_chk(w8_loadProgram(w2_srcFile, w2_srcChars), NULL), 0);
  w_problems = rt_bind(rt_chk(w5_check(w_modules, rt_b(1)), NULL), 0);
  if (rt_cond(rt_gt(std_length(w_problems), rt_i(0)))) {
    (void)rt_chk(w2_flushWarnings(), NULL);
    v1089_p = rt_i(0);
    while (rt_cond(({ wv t2283 = v1089_p; wv t2284 = std_length(w_problems); rt_lt(t2283, t2284); }))) {
      (void)std_eprint(rt_chk(w2_formatDiag(rt_idx(v1089_p, w_problems)), NULL));
      v1089_p = rt_add(v1089_p, rt_i(1));
    }
    rt_exit(rt_i(2));
  }
  w_runtime = rt_bind(rt_chk(w_readRuntime(), NULL), 0);
  if (rt_cond(rt_eq(w_runtime, RT_NULL))) {
    (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S633), rt_text(w_runtimePath) }));
    rt_exit(rt_i(2));
  }
  w_code = rt_bind(rt_chk(w6_generateC(w_modules, w_runtime), NULL), 0);
  (void)rt_chk(w2_flushWarnings(), NULL);
  if (rt_cond(rt_ne(w_mode, rt_s(&S634)))) {
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = rt_add((rt_cond(w_dotWvt) ? std_slice(w_input, rt_i(0), rt_i(-4)) : w_input), rt_s(&S649));
    }
    wv t2285 = std_writeFile(w_output, w_code);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S650), rt_text(w_output) }));
        rt_exit(rt_i(2));
        t2285 = RT_NULL;
      }
    }
    (void)t2285;
    rt_exit(rt_i(0));
  }
  w_dotExe = rt_b(rt_bool(rt_ge(std_length(w_output), rt_i(4)), "&&") && rt_bool(rt_eq(rt_chk(w1_toLower(({ wv t2286 = w_output; wv t2287 = std_length(w_output); std_slice(t2286, rt_i(-4), t2287); })), NULL), rt_s(&S646)), "&&"));
  w_cFile = rt_add((rt_cond(w_dotExe) ? std_slice(w_output, rt_i(0), rt_i(-4)) : w_output), rt_s(&S649));
  wv t2288 = std_writeFile(w_cFile, w_code);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S650), rt_text(w_cFile) }));
      rt_exit(rt_i(2));
      t2288 = RT_NULL;
    }
  }
  (void)t2288;
  w_parts = rt_bind(std_split(std_join(std_split(w_cc, rt_s(&S19)), rt_s(&S87)), rt_s(&S87)), 0);
  w_ccName = rt_bind(rt_chk(w1_toLower(rt_idx(rt_i(-1), w_parts)), NULL), 0);
  w_flags = rt_arr(4, (wv[]){ rt_s(&S651), rt_s(&S637), w_output, w_cFile }, 0);
  wv t2289 = (rt_cond(rt_b(rt_bool(rt_eq(w_ccName, rt_s(&S643)), "||") || rt_bool(rt_eq(w_ccName, rt_s(&S652)), "||"))) ? rt_arr(5, (wv[]){ rt_s(&S653), rt_s(&S651), rt_s(&S637), w_output, w_cFile }, 0) : w_flags);
  wv t2290 = std_run(w_cc, t2289);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S654), rt_text(w_cc), rt_s(&S655) }));
      rt_exit(rt_i(2));
      t2290 = RT_NULL;
    }
  }
  w_status = rt_bind(t2290, 0);
  if (rt_cond(rt_ne(w_status, rt_i(0)))) {
    (void)std_eprint(({ wv t2291 = rt_text(w_cc); wv t2292 = rt_text(w_status); rt_cat(4, (wv[]){ rt_s(&S644), t2291, rt_s(&S656), t2292 }); }));
    rt_exit(rt_i(2));
  }
  rt_exit(rt_i(0));
}
