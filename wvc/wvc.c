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
static const wstr S157 = { 4, 4, 1, 0, "from" };
static const wstr S158 = { 19, 19, 1, 0, "unterminated string" };
static const wstr S159 = { 27, 27, 1, 0, "raw newline inside a string" };
static const wstr S160 = { 10, 10, 1, 0, "write '\\n'" };
static const wstr S161 = { 1, 1, 1, 0, "u" };
static const wstr S162 = { 22, 22, 1, 0, "invalid \\u{...} escape" };
static const wstr S163 = { 69, 69, 1, 0, "write 1 to 6 hex digits naming a Unicode scalar value, e.g. \\u{1F608}" };
static const wstr S164 = { 17, 17, 1, 0, "unknown escape '\\" };
static const wstr S165 = { 43, 43, 1, 0, "valid escapes are \\n \\r \\t \\\\ \\\" \\$ \\u{...}" };
static const wstr S166 = { 37, 37, 1, 0, "Wavert interpolation is {x}, not ${x}" };
static const wstr S167 = { 23, 23, 1, 0, "unmatched '}' in string" };
static const wstr S168 = { 30, 30, 1, 0, "write '}}' for a literal brace" };
static const wstr S169 = { 14, 14, 1, 0, "did you mean {" };
static const wstr S170 = { 2, 2, 1, 0, "}?" };
static const wstr S171 = { 53, 53, 1, 0, "Wavert interpolation is {x}; write \\$ for a literal $" };
static const wstr S172 = { 3, 3, 1, 0, "str" };
static const wstr S173 = { 36, 36, 1, 0, "unterminated interpolation in string" };
static const wstr S174 = { 19, 19, 1, 0, "empty interpolation" };
static const wstr S175 = { 30, 30, 1, 0, "write '{{' for a literal brace" };
static const wstr S176 = { 3, 3, 1, 0, "!if" };
static const wstr S177 = { 4, 4, 1, 0, "expr" };
static const wstr S178 = { 4, 4, 1, 0, "decl" };
static const wstr S179 = { 12, 12, 1, 0, "end of input" };
static const wstr S180 = { 2, 2, 1, 0, "'$" };
static const wstr S181 = { 8, 8, 1, 0, "a string" };
static const wstr S182 = { 2, 2, 1, 0, "='" };
static const wstr S183 = { 4, 4, 1, 0, "call" };
static const wstr S184 = { 8, 8, 1, 0, "failWith" };
static const wstr S185 = { 5, 5, 1, 0, "chain" };
static const wstr S186 = { 6, 6, 1, 0, "string" };
static const wstr S187 = { 3, 3, 1, 0, "arr" };
static const wstr S188 = { 3, 3, 1, 0, "map" };
static const wstr S189 = { 3, 3, 1, 0, "rec" };
static const wstr S190 = { 3, 3, 1, 0, "idx" };
static const wstr S191 = { 3, 3, 1, 0, "mem" };
static const wstr S192 = { 2, 2, 1, 0, "un" };
static const wstr S193 = { 3, 3, 1, 0, "bin" };
static const wstr S194 = { 4, 4, 1, 0, "tern" };
static const wstr S195 = { 10, 10, 1, 0, "expected '" };
static const wstr S196 = { 8, 8, 1, 0, ", found " };
static const wstr S197 = { 9, 9, 1, 0, "', found " };
static const wstr S198 = { 27, 27, 1, 0, "missing whitespace around '" };
static const wstr S199 = { 79, 79, 1, 0, "calls are postfix: write the arguments first, then the function, e.g. (a, b)(f)" };
static const wstr S200 = { 36, 36, 1, 0, "every Wavert statement begins with ;" };
static const wstr S201 = { 41, 41, 1, 0, "expected ';' to start a statement, found " };
static const wstr S202 = { 5, 5, 1, 0, "empty" };
static const wstr S203 = { 46, 46, 1, 0, "';end' must be the last statement of a library" };
static const wstr S204 = { 47, 47, 1, 0, "'import' must come before every other statement" };
static const wstr S205 = { 3, 3, 1, 0, "end" };
static const wstr S206 = { 59, 59, 1, 0, "'export' is only allowed in a library, which ends with ;end" };
static const wstr S207 = { 62, 62, 1, 0, "'onfail' must follow a statement whose value comes from a call" };
static const wstr S208 = { 33, 33, 1, 0, "expected a name for the fail code" };
static const wstr S209 = { 6, 6, 1, 0, "!while" };
static const wstr S210 = { 36, 36, 1, 0, "'else' without a preceding if or !if" };
static const wstr S211 = { 22, 22, 1, 0, "'case' outside a match" };
static const wstr S212 = { 2, 2, 1, 0, "to" };
static const wstr S213 = { 4, 4, 1, 0, "name" };
static const wstr S214 = { 31, 31, 1, 0, "an assignment starts with 'set'" };
static const wstr S215 = { 15, 15, 1, 0, ";set x to value" };
static const wstr S216 = { 54, 54, 1, 0, "to make it the block's value, remove the ';' before it" };
static const wstr S217 = { 28, 28, 1, 0, "this statement has no effect" };
static const wstr S218 = { 23, 23, 1, 0, "expected a name after '" };
static const wstr S219 = { 14, 14, 1, 0, " name is value" };
static const wstr S220 = { 2, 2, 1, 0, "is" };
static const wstr S221 = { 9, 9, 1, 0, " is value" };
static const wstr S222 = { 76, 76, 1, 0, "expected 'to' or a compound assignment such as '+=' after the target, found " };
static const wstr S223 = { 35, 35, 1, 0, "a fail code must be an integer >= 1" };
static const wstr S224 = { 51, 51, 1, 0, "'import' is only allowed at the top level of a file" };
static const wstr S225 = { 45, 45, 1, 0, "expected a name for the module after 'import'" };
static const wstr S226 = { 26, 26, 1, 0, ";import lib from \"lib.wvt\"" };
static const wstr S227 = { 8, 8, 1, 0, ";import " };
static const wstr S228 = { 7, 7, 1, 0, " from \"" };
static const wstr S229 = { 5, 5, 1, 0, ".wvt\"" };
static const wstr S230 = { 45, 45, 1, 0, "an import path must be a plain string literal" };
static const wstr S231 = { 37, 37, 1, 0, "import paths use '/' as the separator" };
static const wstr S232 = { 4, 4, 1, 0, ".wvt" };
static const wstr S233 = { 42, 42, 1, 0, "an import path includes the .wvt extension" };
static const wstr S234 = { 5, 5, 1, 0, "$lib/" };
static const wstr S235 = { 81, 81, 1, 0, "an import path that starts with '$' must start with '$lib/', the standard library" };
static const wstr S236 = { 68, 68, 1, 0, "a file of your own is imported by a relative path, such as \"lib.wvt\"" };
static const wstr S237 = { 2, 2, 1, 0, ".." };
static const wstr S238 = { 42, 42, 1, 0, "a '$lib/' path cannot leave lib/ with '..'" };
static const wstr S239 = { 43, 43, 1, 0, "only top-level declarations can be exported" };
static const wstr S240 = { 70, 70, 1, 0, "only fn, const, var, ref and enum declarations can be exported, found " };
static const wstr S241 = { 52, 52, 1, 0, "';end' is only allowed at the top level of a library" };
static const wstr S242 = { 46, 46, 1, 0, "a library must export at least one declaration" };
static const wstr S243 = { 55, 55, 1, 0, "mark one with 'export', e.g. ;export fn name(x) { ... }" };
static const wstr S244 = { 36, 36, 1, 0, "the name comes before the parameters" };
static const wstr S245 = { 22, 22, 1, 0, ";fn name(a, b) { ... }" };
static const wstr S246 = { 37, 37, 1, 0, "expected the function name after 'fn'" };
static const wstr S247 = { 39, 39, 1, 0, "expected '(' to open the parameter list" };
static const wstr S248 = { 4, 4, 1, 0, ";fn " };
static const wstr S249 = { 14, 14, 1, 0, "(a, b) { ... }" };
static const wstr S250 = { 25, 25, 1, 0, "expected a parameter name" };
static const wstr S251 = { 43, 43, 1, 0, "a parameter is a name, var name or ref name" };
static const wstr S252 = { 40, 40, 1, 0, "expected ')' to close the parameter list" };
static const wstr S253 = { 45, 45, 1, 0, "an enum can only be declared at the top level" };
static const wstr S254 = { 33, 33, 1, 0, "the name comes before the members" };
static const wstr S255 = { 17, 17, 1, 0, ";enum Name (a, b)" };
static const wstr S256 = { 35, 35, 1, 0, "expected the enum name after 'enum'" };
static const wstr S257 = { 36, 36, 1, 0, "expected '(' to open the member list" };
static const wstr S258 = { 6, 6, 1, 0, ";enum " };
static const wstr S259 = { 7, 7, 1, 0, " (a, b)" };
static const wstr S260 = { 33, 33, 1, 0, "an enum needs at least one member" };
static const wstr S261 = { 42, 42, 1, 0, "' is a keyword and cannot be a member name" };
static const wstr S262 = { 30, 30, 1, 0, "expected a member name, found " };
static const wstr S263 = { 18, 18, 1, 0, "duplicate member '" };
static const wstr S264 = { 9, 9, 1, 0, "' in enum" };
static const wstr S265 = { 38, 38, 1, 0, "expected ',' or ')' in the member list" };
static const wstr S266 = { 20, 20, 1, 0, "expected '(' after '" };
static const wstr S267 = { 30, 30, 1, 0, "conditions are parenthesized: " };
static const wstr S268 = { 12, 12, 1, 0, " (x) { ... }" };
static const wstr S269 = { 35, 35, 1, 0, "expected ')' to close the condition" };
static const wstr S270 = { 12, 12, 1, 0, "expected '{'" };
static const wstr S271 = { 32, 32, 1, 0, "unterminated block: expected '}'" };
static const wstr S272 = { 68, 68, 1, 0, "a record as a block's value needs its own braces: { { key: value } }" };
static const wstr S273 = { 75, 75, 1, 0, "an expression without a leading ';' is the block's value and must come last" };
static const wstr S274 = { 44, 44, 1, 0, "expected '}' after the block's value, found " };
static const wstr S275 = { 53, 53, 1, 0, "expected a name, an indexed element or a field to set" };
static const wstr S276 = { 38, 38, 1, 0, "expected ':' in conditional expression" };
static const wstr S277 = { 22, 22, 1, 0, "'-x ** y' is ambiguous" };
static const wstr S278 = { 28, 28, 1, 0, "write -(x ** y) or (-x) ** y" };
static const wstr S279 = { 59, 59, 1, 0, "'[' at the start of a line indexes the expression before it" };
static const wstr S280 = { 71, 71, 1, 0, "if it starts the block's value, wrap the value in parentheses: ([a, b])" };
static const wstr S281 = { 31, 31, 1, 0, "expected ']' to close the index" };
static const wstr S282 = { 41, 41, 1, 0, "' is a keyword and cannot be a field name" };
static const wstr S283 = { 39, 39, 1, 0, "expected a field name after '.', found " };
static const wstr S284 = { 4, 4, 1, 0, "bool" };
static const wstr S285 = { 17, 17, 1, 0, "empty parentheses" };
static const wstr S286 = { 39, 39, 1, 0, "a call with no arguments is written (f)" };
static const wstr S287 = { 34, 34, 1, 0, "trailing comma in an argument list" };
static const wstr S288 = { 40, 40, 1, 0, "expected ',' or ')' in the argument list" };
static const wstr S289 = { 69, 69, 1, 0, "a parenthesized list must be followed by the function it is passed to" };
static const wstr S290 = { 30, 30, 1, 0, "(a, b)(f) calls f with a and b" };
static const wstr S291 = { 23, 23, 1, 0, "expected ']' after '[:'" };
static const wstr S292 = { 27, 27, 1, 0, "an empty map is written [:]" };
static const wstr S293 = { 31, 31, 1, 0, "trailing comma in array literal" };
static const wstr S294 = { 32, 32, 1, 0, "expected ',' or ']' in the array" };
static const wstr S295 = { 12, 12, 1, 0, "unexpected '" };
static const wstr S296 = { 46, 46, 1, 0, "declarations and assignments are statements: ;" };
static const wstr S297 = { 4, 4, 1, 0, " ..." };
static const wstr S298 = { 21, 21, 1, 0, "'fail' is a statement" };
static const wstr S299 = { 25, 25, 1, 0, ";fail c fails with code c" };
static const wstr S300 = { 37, 37, 1, 0, "an assignment is a statement: ;set x " };
static const wstr S301 = { 7, 7, 1, 0, "= value" };
static const wstr S302 = { 30, 30, 1, 0, "expected an expression, found " };
static const wstr S303 = { 29, 29, 1, 0, "expected a field name, found " };
static const wstr S304 = { 47, 47, 1, 0, "record literals are written { key: value, ... }" };
static const wstr S305 = { 17, 17, 1, 0, "duplicate field '" };
static const wstr S306 = { 19, 19, 1, 0, "' in record literal" };
static const wstr S307 = { 29, 29, 1, 0, "there is no shorthand; write " };
static const wstr S308 = { 31, 31, 1, 0, "expected ':' after field name '" };
static const wstr S309 = { 32, 32, 1, 0, "trailing comma in record literal" };
static const wstr S310 = { 37, 37, 1, 0, "expected ',' or '}' in record literal" };
static const wstr S311 = { 1, 1, 1, 0, "i" };
static const wstr S312 = { 1, 1, 1, 0, "s" };
static const wstr S313 = { 73, 73, 1, 0, "a map key must be an int, a string, a boolean, an enum member or a record" };
static const wstr S314 = { 28, 28, 1, 0, "duplicate key in map literal" };
static const wstr S315 = { 43, 43, 1, 0, "expected ':' after a key in the map literal" };
static const wstr S316 = { 42, 42, 1, 0, "map literals are written [key: value, ...]" };
static const wstr S317 = { 29, 29, 1, 0, "trailing comma in map literal" };
static const wstr S318 = { 38, 38, 1, 0, "expected ',' or ']' in the map literal" };
static const wstr S319 = { 38, 38, 1, 0, "expected '{' after the match's subject" };
static const wstr S320 = { 32, 32, 1, 0, "'else' must come last in a match" };
static const wstr S321 = { 31, 31, 1, 0, "a case needs at least one value" };
static const wstr S322 = { 21, 21, 1, 0, "case \"a\", \"b\" { ... }" };
static const wstr S323 = { 27, 27, 1, 0, "a match has only one 'else'" };
static const wstr S324 = { 45, 45, 1, 0, "a match needs at least one case before 'else'" };
static const wstr S325 = { 31, 31, 1, 0, "a match needs at least one case" };
static const wstr S326 = { 30, 30, 1, 0, "match (x) { case \"a\" { ... } }" };
static const wstr S327 = { 41, 41, 1, 0, "expected 'case', 'else' or '}' in a match" };
static const wstr S328 = { 35, 35, 1, 0, "a case value cannot be interpolated" };
static const wstr S329 = { 28, 28, 1, 0, "floats cannot be case values" };
static const wstr S330 = { 58, 58, 1, 0, "float equality makes a poor match; compare with if instead" };
static const wstr S331 = { 48, 48, 1, 0, "a case value must be a literal or an enum member" };
static const wstr S332 = { 67, 67, 1, 0, "case values are integers, strings, true, false, null or Enum.member" };
static const wstr S333 = { 35, 35, 1, 0, "'!if (a != b)' is a double negative" };
static const wstr S334 = { 19, 19, 1, 0, "write 'if (a == b)'" };
static const wstr S335 = { 11, 11, 1, 0, "unexpected " };
static const wstr S336 = { 17, 17, 1, 0, " in interpolation" };
static const wstr S337 = { 5, 5, 1, 0, "print" };
static const wstr S338 = { 5, 5, 1, 0, "write" };
static const wstr S339 = { 6, 6, 1, 0, "eprint" };
static const wstr S340 = { 8, 8, 1, 0, "readLine" };
static const wstr S341 = { 7, 7, 1, 0, "readKey" };
static const wstr S342 = { 7, 7, 1, 0, "waitKey" };
static const wstr S343 = { 8, 8, 1, 0, "terminal" };
static const wstr S344 = { 12, 12, 1, 0, "terminalSize" };
static const wstr S345 = { 8, 8, 1, 0, "readFile" };
static const wstr S346 = { 9, 9, 1, 0, "writeFile" };
static const wstr S347 = { 10, 10, 1, 0, "appendFile" };
static const wstr S348 = { 10, 10, 1, 0, "fileExists" };
static const wstr S349 = { 7, 7, 1, 0, "listDir" };
static const wstr S350 = { 10, 10, 1, 0, "deleteFile" };
static const wstr S351 = { 4, 4, 1, 0, "args" };
static const wstr S352 = { 3, 3, 1, 0, "env" };
static const wstr S353 = { 11, 11, 1, 0, "programPath" };
static const wstr S354 = { 3, 3, 1, 0, "run" };
static const wstr S355 = { 6, 6, 1, 0, "length" };
static const wstr S356 = { 2, 2, 1, 0, "at" };
static const wstr S357 = { 5, 5, 1, 0, "slice" };
static const wstr S358 = { 4, 4, 1, 0, "code" };
static const wstr S359 = { 8, 8, 1, 0, "fromCode" };
static const wstr S360 = { 5, 5, 1, 0, "split" };
static const wstr S361 = { 4, 4, 1, 0, "join" };
static const wstr S362 = { 8, 8, 1, 0, "toString" };
static const wstr S363 = { 5, 5, 1, 0, "toInt" };
static const wstr S364 = { 7, 7, 1, 0, "toFloat" };
static const wstr S365 = { 4, 4, 1, 0, "push" };
static const wstr S366 = { 3, 3, 1, 0, "pop" };
static const wstr S367 = { 4, 4, 1, 0, "same" };
static const wstr S368 = { 4, 4, 1, 0, "sort" };
static const wstr S369 = { 6, 6, 1, 0, "sortBy" };
static const wstr S370 = { 7, 7, 1, 0, "reverse" };
static const wstr S371 = { 3, 3, 1, 0, "get" };
static const wstr S372 = { 3, 3, 1, 0, "has" };
static const wstr S373 = { 6, 6, 1, 0, "remove" };
static const wstr S374 = { 4, 4, 1, 0, "keys" };
static const wstr S375 = { 6, 6, 1, 0, "values" };
static const wstr S376 = { 6, 6, 1, 0, "typeOf" };
static const wstr S377 = { 6, 6, 1, 0, "fields" };
static const wstr S378 = { 5, 5, 1, 0, "floor" };
static const wstr S379 = { 4, 4, 1, 0, "ceil" };
static const wstr S380 = { 5, 5, 1, 0, "round" };
static const wstr S381 = { 5, 5, 1, 0, "trunc" };
static const wstr S382 = { 8, 8, 1, 0, "toUint32" };
static const wstr S383 = { 5, 5, 1, 0, "isNan" };
static const wstr S384 = { 4, 4, 1, 0, "time" };
static const wstr S385 = { 7, 7, 1, 0, "elapsed" };
static const wstr S386 = { 5, 5, 1, 0, "sleep" };
static const wstr S387 = { 9, 9, 1, 0, "newRandom" };
static const wstr S388 = { 6, 6, 1, 0, "random" };
static const wstr S389 = { 11, 11, 1, 0, "randomFloat" };
static const wstr S390 = { 9, 9, 1, 0, " function" };
static const wstr S391 = { 6, 6, 1, 0, "n enum" };
static const wstr S392 = { 9, 9, 1, 0, " argument" };
static const wstr S393 = { 4, 4, 1, 0, "str:" };
static const wstr S394 = { 4, 4, 1, 0, "mem:" };
static const wstr S395 = { 55, 55, 1, 0, "this block's value is used, but it does not produce one" };
static const wstr S396 = { 62, 62, 1, 0, "end the block with its value, or with null if that is intended" };
static const wstr S397 = { 42, 42, 1, 0, "' is already declared in this scope (line " };
static const wstr S398 = { 11, 11, 1, 0, "' shadows '" };
static const wstr S399 = { 32, 32, 1, 0, "' from an enclosing scope (line " };
static const wstr S400 = { 45, 45, 1, 0, "Wavert forbids shadowing; choose another name" };
static const wstr S401 = { 14, 14, 1, 0, "unknown name '" };
static const wstr S402 = { 39, 39, 1, 0, "' is used before its declaration (line " };
static const wstr S403 = { 5, 5, 1, 0, "param" };
static const wstr S404 = { 27, 27, 1, 0, "'return' outside a function" };
static const wstr S405 = { 16, 16, 1, 0, "' outside a loop" };
static const wstr S406 = { 56, 56, 1, 0, "'fail' outside a function: there is no caller to fail to" };
static const wstr S407 = { 65, 65, 1, 0, "'onfail' handles a call, but this statement's value is not a call" };
static const wstr S408 = { 87, 87, 1, 0, "the call must be the outermost expression, e.g. ;const n is s(parse) onfail (c) { ... }" };
static const wstr S409 = { 44, 44, 1, 0, "a match cannot mix literals and enum members" };
static const wstr S410 = { 31, 31, 1, 0, "a match cannot mix members of '" };
static const wstr S411 = { 7, 7, 1, 0, "' and '" };
static const wstr S412 = { 21, 21, 1, 0, "duplicate case value " };
static const wstr S413 = { 7, 7, 1, 0, " (line " };
static const wstr S414 = { 22, 22, 1, 0, "match does not handle " };
static const wstr S415 = { 2, 2, 1, 0, ", " };
static const wstr S416 = { 57, 57, 1, 0, "add a case for each, or end the match with 'else { ... }'" };
static const wstr S417 = { 52, 52, 1, 0, "a match used as a value must end with 'else { ... }'" };
static const wstr S418 = { 6, 6, 1, 0, "module" };
static const wstr S419 = { 19, 19, 1, 0, "' does not export '" };
static const wstr S420 = { 17, 17, 1, 0, "' is not a module" };
static const wstr S421 = { 16, 16, 1, 0, "unknown module '" };
static const wstr S422 = { 41, 41, 1, 0, "' is a function; functions are not values" };
static const wstr S423 = { 20, 20, 1, 0, "to call it, write x(" };
static const wstr S424 = { 7, 7, 1, 0, "), or (" };
static const wstr S425 = { 19, 19, 1, 0, ") with no arguments" };
static const wstr S426 = { 25, 25, 1, 0, "' is an enum, not a value" };
static const wstr S427 = { 24, 24, 1, 0, "its members are written " };
static const wstr S428 = { 7, 7, 1, 0, ".member" };
static const wstr S429 = { 54, 54, 1, 0, "case values are literals or enum members (Enum.member)" };
static const wstr S430 = { 16, 16, 1, 0, "' is not an enum" };
static const wstr S431 = { 14, 14, 1, 0, "unknown enum '" };
static const wstr S432 = { 11, 11, 1, 0, "no member '" };
static const wstr S433 = { 11, 11, 1, 0, "' in enum '" };
static const wstr S434 = { 16, 16, 1, 0, "its members are " };
static const wstr S435 = { 66, 66, 1, 0, "a conditional used as a value must end with a plain 'else { ... }'" };
static const wstr S436 = { 12, 12, 1, 0, "cannot set '" };
static const wstr S437 = { 25, 25, 1, 0, "': it is not a var or ref" };
static const wstr S438 = { 7, 7, 1, 0, "a field" };
static const wstr S439 = { 10, 10, 1, 0, "an element" };
static const wstr S440 = { 29, 29, 1, 0, "': enum members are constants" };
static const wstr S441 = { 9, 9, 1, 0, "': only '" };
static const wstr S442 = { 12, 12, 1, 0, "' may set it" };
static const wstr S443 = { 14, 14, 1, 0, "cannot modify " };
static const wstr S444 = { 5, 5, 1, 0, " of '" };
static const wstr S445 = { 18, 18, 1, 0, "': it is not a var" };
static const wstr S446 = { 6, 6, 1, 0, "' is a" };
static const wstr S447 = { 15, 15, 1, 0, ", not a binding" };
static const wstr S448 = { 24, 24, 1, 0, "mark the parameter (ref " };
static const wstr S449 = { 24, 24, 1, 0, ") to rebind it, or (var " };
static const wstr S450 = { 20, 20, 1, 0, ") for a mutable copy" };
static const wstr S451 = { 29, 29, 1, 0, "mark the parameter var: (var " };
static const wstr S452 = { 22, 22, 1, 0, "declare it with ';ref " };
static const wstr S453 = { 32, 32, 1, 0, " is ...' to rebind it, or ';var " };
static const wstr S454 = { 27, 27, 1, 0, " is ...' for a mutable copy" };
static const wstr S455 = { 22, 22, 1, 0, "declare it with ';var " };
static const wstr S456 = { 8, 8, 1, 0, " is ...'" };
static const wstr S457 = { 32, 32, 1, 0, "unknown standard library name '$" };
static const wstr S458 = { 26, 26, 1, 0, "' is a module, not a value" };
static const wstr S459 = { 27, 27, 1, 0, "what it exports is written " };
static const wstr S460 = { 5, 5, 1, 0, ".name" };
static const wstr S461 = { 32, 32, 1, 0, "' is a function; call it with ($" };
static const wstr S462 = { 30, 30, 1, 0, "'fail' is a statement: ;fail c" };
static const wstr S463 = { 24, 24, 1, 0, "checker: unhandled node " };
static const wstr S464 = { 31, 31, 1, 0, "' is a constant, not a function" };
static const wstr S465 = { 8, 8, 1, 0, "' takes " };
static const wstr S466 = { 6, 6, 1, 0, ", got " };
static const wstr S467 = { 18, 18, 1, 0, "unknown function '" };
static const wstr S468 = { 71, 71, 1, 0, ") is a call with no arguments; to use the value, remove the parentheses" };
static const wstr S469 = { 53, 53, 1, 0, "calls are postfix: the function comes last, e.g. x(f)" };
static const wstr S470 = { 19, 19, 1, 0, "' is not a function" };
static const wstr S471 = { 21, 21, 1, 0, "' is already imported" };
static const wstr S472 = { 38, 38, 1, 0, "' is not a library: it ends with ;exit" };
static const wstr S473 = { 31, 31, 1, 0, "only a library ends with ';end'" };
static const wstr S474 = { 25, 25, 1, 0, "a program ends with ;exit" };
static const wstr S475 = { 88, 88, 1, 0, "a library's top level may contain only import, fn, const, var, ref and enum declarations" };
static const wstr S476 = { 37, 37, 1, 0, "move other statements into a function" };
static const wstr S477 = { 28, 28, 1, 0, "a library must end with ;end" };
static const wstr S478 = { 56, 56, 1, 0, "mark what it exports with 'export', and end it with ;end" };
static const wstr S479 = { 15, 15, 1, 0, "cannot export '" };
static const wstr S480 = { 38, 38, 1, 0, "': it is not declared at the top level" };
static const wstr S481 = { 17, 17, 1, 0, "': it is a module" };
static const wstr S482 = { 5, 5, 1, 0, "line " };
static const wstr S483 = { 6, 6, 1, 0, " line " };
static const wstr S484 = { 49, 49, 1, 0, "' never produces a value, but its value is used (" };
static const wstr S485 = { 65, 65, 1, 0, "end the function with its value, or with null if that is intended" };
static const wstr S486 = { 29, 29, 1, 0, "a program must end with ;exit" };
static const wstr S487 = { 50, 50, 1, 0, "end the program with ;exit, or a library with ;end" };
static const wstr S488 = { 3, 3, 1, 0, "add" };
static const wstr S489 = { 3, 3, 1, 0, "sub" };
static const wstr S490 = { 3, 3, 1, 0, "mul" };
static const wstr S491 = { 3, 3, 1, 0, "div" };
static const wstr S492 = { 3, 3, 1, 0, "mod" };
static const wstr S493 = { 3, 3, 1, 0, "pow" };
static const wstr S494 = { 2, 2, 1, 0, "eq" };
static const wstr S495 = { 2, 2, 1, 0, "ne" };
static const wstr S496 = { 2, 2, 1, 0, "lt" };
static const wstr S497 = { 2, 2, 1, 0, "gt" };
static const wstr S498 = { 2, 2, 1, 0, "le" };
static const wstr S499 = { 2, 2, 1, 0, "ge" };
static const wstr S500 = { 4, 4, 1, 0, "band" };
static const wstr S501 = { 3, 3, 1, 0, "bor" };
static const wstr S502 = { 4, 4, 1, 0, "bxor" };
static const wstr S503 = { 3, 3, 1, 0, "shl" };
static const wstr S504 = { 3, 3, 1, 0, "shr" };
static const wstr S505 = { 3, 3, 1, 0, "neg" };
static const wstr S506 = { 3, 3, 1, 0, "not" };
static const wstr S507 = { 4, 4, 1, 0, "bnot" };
static const wstr S508 = { 2, 2, 1, 0, "\\\"" };
static const wstr S509 = { 2, 2, 1, 0, "\\\\" };
static const wstr S510 = { 2, 2, 1, 0, "{ " };
static const wstr S511 = { 5, 5, 1, 0, ", 0, " };
static const wstr S512 = { 2, 2, 1, 0, " }" };
static const wstr S513 = { 4, 4, 1, 0, "NULL" };
static const wstr S514 = { 8, 8, 1, 0, "(wv[]){ " };
static const wstr S515 = { 1, 1, 1, 0, "w" };
static const wstr S516 = { 1, 1, 1, 0, "v" };
static const wstr S517 = { 2, 2, 1, 0, "  " };
static const wstr S518 = { 2, 2, 1, 0, "&S" };
static const wstr S519 = { 3, 3, 1, 0, "&F_" };
static const wstr S520 = { 19, 19, 1, 0, "static const wmem l" };
static const wstr S521 = { 7, 7, 1, 0, "[] = { " };
static const wstr S522 = { 3, 3, 1, 0, " };" };
static const wstr S523 = { 1, 1, 1, 0, "l" };
static const wstr S524 = { 3, 3, 1, 0, "fr." };
static const wstr S525 = { 2, 2, 1, 0, "up" };
static const wstr S526 = { 4, 4, 1, 0, "->up" };
static const wstr S527 = { 2, 2, 1, 0, "->" };
static const wstr S528 = { 7, 7, 1, 0, "rt_def(" };
static const wstr S529 = { 3, 3, 1, 0, "wv " };
static const wstr S530 = { 3, 3, 1, 0, " = " };
static const wstr S531 = { 3, 3, 1, 0, "({ " };
static const wstr S532 = { 4, 4, 1, 0, "; })" };
static const wstr S533 = { 12, 12, 1, 0, " = RT_UNSET;" };
static const wstr S534 = { 6, 6, 1, 0, "(void)" };
static const wstr S535 = { 7, 7, 1, 0, "RT_NULL" };
static const wstr S536 = { 66, 66, 1, 0, "'return' inside a conditional used as a value is not supported yet" };
static const wstr S537 = { 7, 7, 1, 0, "RT_RET(" };
static const wstr S538 = { 2, 2, 1, 0, ");" };
static const wstr S539 = { 9, 9, 1, 0, "rt_panic(" };
static const wstr S540 = { 48, 48, 1, 0, "' cannot leave a conditional used as a value yet" };
static const wstr S541 = { 6, 6, 1, 0, "break;" };
static const wstr S542 = { 9, 9, 1, 0, "continue;" };
static const wstr S543 = { 7, 7, 1, 0, "while (" };
static const wstr S544 = { 8, 8, 1, 0, "rt_cond(" };
static const wstr S545 = { 4, 4, 1, 0, ")) {" };
static const wstr S546 = { 5, 5, 1, 0, "rt_i(" };
static const wstr S547 = { 8, 8, 1, 0, "rt_exit(" };
static const wstr S548 = { 7, 7, 1, 0, "rt_i(0)" };
static const wstr S549 = { 29, 29, 1, 0, "codegen: unhandled statement " };
static const wstr S550 = { 13, 13, 1, 0, "rt_fail_with(" };
static const wstr S551 = { 16, 16, 1, 0, "RT_RET(RT_NULL);" };
static const wstr S552 = { 9, 9, 1, 0, "struct fr" };
static const wstr S553 = { 4, 4, 1, 0, " *up" };
static const wstr S554 = { 2, 2, 1, 0, "p_" };
static const wstr S555 = { 10, 10, 1, 0, "static wv " };
static const wstr S556 = { 4, 4, 1, 0, "void" };
static const wstr S557 = { 9, 9, 1, 0, "rt_enter(" };
static const wstr S558 = { 8, 8, 1, 0, " fr = { " };
static const wstr S559 = { 8, 8, 1, 0, ".up = up" };
static const wstr S560 = { 8, 8, 1, 0, "rt_bind(" };
static const wstr S561 = { 13, 13, 1, 0, "(void)rt_def(" };
static const wstr S562 = { 3, 3, 1, 0, "rt_" };
static const wstr S563 = { 7, 7, 1, 0, "rt_set_" };
static const wstr S564 = { 7, 7, 1, 0, "rt_upd_" };
static const wstr S565 = { 5, 5, 1, 0, ", rt_" };
static const wstr S566 = { 7, 7, 1, 0, "rt_arr(" };
static const wstr S567 = { 7, 7, 1, 0, "rt_map(" };
static const wstr S568 = { 1, 1, 1, 0, "K" };
static const wstr S569 = { 7, 7, 1, 0, "rt_rec(" };
static const wstr S570 = { 18, 18, 1, 0, "if (rt_failcode) {" };
static const wstr S571 = { 17, 17, 1, 0, "rt_i(rt_failcode)" };
static const wstr S572 = { 16, 16, 1, 0, "rt_failcode = 0;" };
static const wstr S573 = { 3, 3, 1, 0, "raw" };
static const wstr S574 = { 7, 7, 1, 0, "} else " };
static const wstr S575 = { 4, 4, 1, 0, "if (" };
static const wstr S576 = { 3, 3, 1, 0, ") {" };
static const wstr S577 = { 8, 8, 1, 0, "} else {" };
static const wstr S578 = { 3, 3, 1, 0, " ? " };
static const wstr S579 = { 3, 3, 1, 0, " : " };
static const wstr S580 = { 3, 3, 1, 0, "({\n" };
static const wstr S581 = { 2, 2, 1, 0, "})" };
static const wstr S582 = { 3, 3, 1, 0, "eq(" };
static const wstr S583 = { 4, 4, 1, 0, " || " };
static const wstr S584 = { 12, 12, 1, 0, "(rt_no_case(" };
static const wstr S585 = { 11, 11, 1, 0, "), RT_NULL)" };
static const wstr S586 = { 6, 6, 1, 0, "({ wv " };
static const wstr S587 = { 2, 2, 1, 0, "; " };
static const wstr S588 = { 4, 4, 1, 0, "std_" };
static const wstr S589 = { 3, 3, 1, 0, "&fr" };
static const wstr S590 = { 7, 7, 1, 0, "rt_chk(" };
static const wstr S591 = { 5, 5, 1, 0, "rt_f(" };
static const wstr S592 = { 5, 5, 1, 0, "rt_s(" };
static const wstr S593 = { 8, 8, 1, 0, "rt_text(" };
static const wstr S594 = { 7, 7, 1, 0, "rt_cat(" };
static const wstr S595 = { 5, 5, 1, 0, "rt_b(" };
static const wstr S596 = { 6, 6, 1, 0, "RT_NAN" };
static const wstr S597 = { 6, 6, 1, 0, "RT_INF" };
static const wstr S598 = { 2, 2, 1, 0, "()" };
static const wstr S599 = { 7, 7, 1, 0, "rt_idx(" };
static const wstr S600 = { 6, 6, 1, 0, "rt_m(&" };
static const wstr S601 = { 2, 2, 1, 0, "])" };
static const wstr S602 = { 7, 7, 1, 0, "rt_get(" };
static const wstr S603 = { 13, 13, 1, 0, "rt_b(rt_bool(" };
static const wstr S604 = { 3, 3, 1, 0, ", \"" };
static const wstr S605 = { 3, 3, 1, 0, "\") " };
static const wstr S606 = { 9, 9, 1, 0, " rt_bool(" };
static const wstr S607 = { 3, 3, 1, 0, "\"))" };
static const wstr S608 = { 9, 9, 1, 0, "(rt_cond(" };
static const wstr S609 = { 4, 4, 1, 0, ") ? " };
static const wstr S610 = { 30, 30, 1, 0, "codegen: unhandled expression " };
static const wstr S611 = { 2, 2, 1, 0, "\r\n" };
static const wstr S612 = { 20, 20, 1, 0, "// ---- program ----" };
static const wstr S613 = { 19, 19, 1, 0, "static const wstr S" };
static const wstr S614 = { 14, 14, 1, 0, "static wstr F_" };
static const wstr S615 = { 26, 26, 1, 0, "static const wstr *const K" };
static const wstr S616 = { 5, 5, 1, 0, " *up;" };
static const wstr S617 = { 10, 10, 1, 0, "char none;" };
static const wstr S618 = { 3, 3, 1, 0, " { " };
static const wstr S619 = { 2, 2, 1, 0, " {" };
static const wstr S620 = { 28, 28, 1, 0, "static void wvt_main(void) {" };
static const wstr S621 = { 10, 10, 1, 0, "  rt_syms(" };
static const wstr S622 = { 19, 19, 1, 0, ", (wstr *const[]){ " };
static const wstr S623 = { 4, 4, 1, 0, " });" };
static const wstr S624 = { 11, 11, 1, 0, "  rt_roots(" };
static const wstr S625 = { 17, 17, 1, 0, ", (wv *const[]){ " };
static const wstr S626 = { 4, 4, 1, 0, "  | " };
static const wstr S627 = { 4, 4, 1, 0, "  { " };
static const wstr S628 = { 4, 4, 1, 0, "    " };
static const wstr S629 = { 3, 3, 1, 0, "  }" };
static const wstr S630 = { 5, 5, 1, 0, "array" };
static const wstr S631 = { 2, 2, 1, 0, "[]" };
static const wstr S632 = { 6, 6, 1, 0, "record" };
static const wstr S633 = { 52, 52, 1, 0, "cannot find the standard library: no lib/ folder in " };
static const wstr S634 = { 4, 4, 1, 0, " or " };
static const wstr S635 = { 14, 14, 1, 0, "import cycle: " };
static const wstr S636 = { 4, 4, 1, 0, " -> " };
static const wstr S637 = { 29, 29, 1, 0, "the standard library has no '" };
static const wstr S638 = { 3, 3, 1, 0, "' (" };
static const wstr S639 = { 13, 13, 1, 0, "cannot read '" };
static const wstr S640 = { 3, 1, 0, 0, "﻿" };
static const wstr S641 = { 7, 7, 1, 0, "usage:\n" };
static const wstr S642 = { 64, 64, 1, 0, "  wvc <file.wvt> [-o <out.c>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S643 = { 90, 90, 1, 0, "  wvc build <file.wvt> [-o <program>] [--cc <compiler>] [--runtime <wvt.c>] [--lib <dir>]\n" };
static const wstr S644 = { 26, 26, 1, 0, "  wvc --tokens <file.wvt>\n" };
static const wstr S645 = { 22, 22, 1, 0, "  wvc --ast <file.wvt>" };
static const wstr S646 = { 17, 17, 1, 0, "wvc: cannot read " };
static const wstr S647 = { 5, 5, 1, 0, "build" };
static const wstr S648 = { 8, 8, 1, 0, "--tokens" };
static const wstr S649 = { 5, 5, 1, 0, "--ast" };
static const wstr S650 = { 2, 2, 1, 0, "-o" };
static const wstr S651 = { 9, 9, 1, 0, "--runtime" };
static const wstr S652 = { 4, 4, 1, 0, "--cc" };
static const wstr S653 = { 5, 5, 1, 0, "--lib" };
static const wstr S654 = { 2, 2, 1, 0, "OS" };
static const wstr S655 = { 10, 10, 1, 0, "Windows_NT" };
static const wstr S656 = { 3, 3, 1, 0, "ZIG" };
static const wstr S657 = { 3, 3, 1, 0, "zig" };
static const wstr S658 = { 5, 5, 1, 0, "wvc: " };
static const wstr S659 = { 52, 52, 1, 0, " does not end with .wvt, so name the program with -o" };
static const wstr S660 = { 4, 4, 1, 0, ".exe" };
static const wstr S661 = { 13, 13, 1, 0, "runtime/wvt.c" };
static const wstr S662 = { 3, 3, 1, 0, "../" };
static const wstr S663 = { 3, 3, 1, 0, "lib" };
static const wstr S664 = { 6, 6, 1, 0, "../lib" };
static const wstr S665 = { 2, 2, 1, 0, ".c" };
static const wstr S666 = { 18, 18, 1, 0, "wvc: cannot write " };
static const wstr S667 = { 3, 3, 1, 0, "-O2" };
static const wstr S668 = { 7, 7, 1, 0, "zig.exe" };
static const wstr S669 = { 2, 2, 1, 0, "cc" };
static const wstr S670 = { 16, 16, 1, 0, "wvc: cannot run " };
static const wstr S671 = { 67, 67, 1, 0, ": install Zig (https://ziglang.org), or name a C compiler with --cc" };
static const wstr S672 = { 23, 23, 1, 0, " failed with exit code " };
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
struct fr27 { wv v71_cs; wv v72_n; wv v73_i; wv v74_prevT; wv v75_nlNow; wv v76_toks; };
struct fr49 { wv v185_toks; wv v187_p; wv v188_depth; wv v189_exports; wv v190_firstExport; };
struct fr100 { wv v385_modules; wv v387_errors; wv v388_nextFn; wv v389_curMod; wv v390_functions; wv v391_uses; wv v392_enums; wv v425_topFn; };
struct fr142 { wv v647_modules; wv v648_runtime; wv v649_n; wv v650_tmps; wv v651_strings; wv v652_fields; wv v653_shapes; wv v654_enumDefs; wv v655_globals; wv v656_frames; wv v657_functions; wv v658_out; wv v659_indent; wv v660_loops; wv v661_iife; wv v662_decls; wv v663_fns; wv v664_tops; wv v665_slots; wv v666_curMod; wv v667_mod; wv v668_curSlot; wv v669_stolen; wv v670_ownEnums; wv v671_cur; };
struct fr212 { wv v1052_libDir; wv v1053_searched; wv v1054_modules; };
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
static wv v1087_a;
static wv v1102_tokens;
static wv v1103_program;
static wv v1104_p;
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
static wv s192_at(struct fr49 *up, wv v230_type);
static wv s193_atWord(struct fr49 *up, wv v231_word);
static wv s194_atCall(struct fr49 *up);
static wv s195_next(struct fr49 *up);
static wv s196_accept(struct fr49 *up, wv v233_type);
static wv s197_expect(struct fr49 *up, wv v234_type, wv v235_msg, wv v236_hint);
static wv s198_expectWord(struct fr49 *up, wv v239_word, wv v240_hint);
static wv s199_lintSpace(struct fr49 *up, wv v242_op);
static wv s200_program(struct fr49 *up);
static wv s201_statement(struct fr49 *up);
static wv s202_statementBody(struct fr49 *up, wv v255_tk);
static wv s203_declaration(struct fr49 *up);
static wv s204_setStatement(struct fr49 *up);
static wv s205_failStatement(struct fr49 *up);
static wv s206_importStatement(struct fr49 *up);
static wv s207_exportStatement(struct fr49 *up);
static wv s208_endStatement(struct fr49 *up);
static wv s209_fnStatement(struct fr49 *up);
static wv s210_enumStatement(struct fr49 *up);
static wv s211_cond(struct fr49 *up, wv v290_kw);
static wv s212_block(struct fr49 *up);
static wv s213_lvalue(struct fr49 *up);
static wv s214_expr(struct fr49 *up);
static wv s215_ternary(struct fr49 *up);
static wv s216_ternaryTail(struct fr49 *up, wv v300_c);
static wv s217_binary(struct fr49 *up, wv v305_min);
static wv s218_binaryFrom(struct fr49 *up, wv v306_left, wv v307_min);
static wv s219_unary(struct fr49 *up);
static wv s220_powerTail(struct fr49 *up, wv v314_base);
static wv s221_postfix(struct fr49 *up);
static wv s222_call(struct fr49 *up, wv v321_args);
static wv s223_primary(struct fr49 *up);
static wv s224_record(struct fr49 *up, wv v335_open);
static wv s225_constantKey(struct fr49 *up, wv v343_e);
static wv s226_mapLiteral(struct fr49 *up, wv v344_open, wv v345_first);
static wv s227_parseMatch(struct fr49 *up);
static wv s228_caseValue(struct fr49 *up);
static wv s229_chain(struct fr49 *up);
static wv w5_stdSig(wv v367_name);
static wv w5_modeOf(wv v369_x);
static wv w5_kindText(wv v370_kind);
static wv w5_plural(wv v371_n);
static wv w5_enumText(wv v372_x);
static wv w5_caseKey(wv v373_v);
static wv w5_caseText(wv v374_v);
static wv w5_getsAwayWithValue(wv v375_list);
static wv w5_lineIn(wv v380_chars, wv v381_pos);
static wv w5_lineOf(wv v384_pos);
static wv w5_check(wv p_v385_modules, wv v386_requireExit);
static wv s393_error(struct fr100 *up, wv v430_pos, wv v431_message, wv v432_hint);
static wv s394_enter(struct fr100 *up, wv v433_m);
static wv s395_enumMembers(struct fr100 *up, wv v434_d);
static wv s396_firstUse(struct fr100 *up, wv v436_mod, wv v437_declPos);
static wv s397_requireValue(struct fr100 *up, wv v439_b);
static wv s398_newScope(struct fr100 *up, wv v442_parent);
static wv s399_decl(struct fr100 *up, wv v444_kind, wv v445_name, wv v446_mode, wv v447_namePos, wv v448_visibleAt, wv v449_fnId, wv v450_arity);
static wv s400_own(struct fr100 *up, wv v451_scope, wv v452_name);
static wv s401_declare(struct fr100 *up, wv v454_scope, wv v455_name, wv v456_d);
static wv s402_lookup(struct fr100 *up, wv v460_scope, wv v461_name);
static wv s403_resolve(struct fr100 *up, wv v464_name, wv v465_pos, wv v466_scope, wv v467_ctx);
static wv s404_block(struct fr100 *up, wv v469_b, wv v470_scope, wv v471_ctx);
static wv s405_stmt(struct fr100 *up, wv v476_s, wv v477_scope, wv v478_ctx);
static wv s406_onfailHandler(struct fr100 *up, wv v485_s, wv v486_scope, wv v487_ctx);
static wv s407_checkMatch(struct fr100 *up, wv v490_e, wv v491_scope, wv v492_ctx, wv v493_stmtLevel);
static wv s408_moduleNamed(struct fr100 *up, wv v513_x, wv v514_scope);
static wv s409_exported(struct fr100 *up, wv v516_lib, wv v517_name);
static wv s410_enumNamed(struct fr100 *up, wv v520_x, wv v521_scope);
static wv s411_exportOf(struct fr100 *up, wv v525_e, wv v526_m);
static wv s412_unknownModule(struct fr100 *up, wv v528_x, wv v529_scope);
static wv s413_qualified(struct fr100 *up, wv v531_e, wv v532_m);
static wv s414_caseEnum(struct fr100 *up, wv v535_v, wv v536_scope);
static wv s415_member(struct fr100 *up, wv v542_e, wv v543_d);
static wv s416_chain(struct fr100 *up, wv v548_e, wv v549_scope, wv v550_ctx, wv v551_stmtLevel);
static wv s417_lvalue(struct fr100 *up, wv v554_t, wv v555_scope, wv v556_ctx);
static wv s418_mustBeMutable(struct fr100 *up, wv v563_d, wv v564_name, wv v565_pos, wv v566_message, wv v567_rebind);
static wv s419_unknownStd(struct fr100 *up, wv v569_node);
static wv s420_expr(struct fr100 *up, wv v570_e, wv v571_scope, wv v572_ctx);
static wv s421_callee(struct fr100 *up, wv v582_e, wv v583_scope, wv v584_used);
static wv s422_importNames(struct fr100 *up, wv v592_m, wv v593_scope);
static wv s423_libraryRules(struct fr100 *up, wv v602_m, wv v603_scope, wv v604_isEntry, wv v605_last);
static wv s424_checkModule(struct fr100 *up, wv v612_m, wv v613_isEntry);
static wv w6_binFn(wv v624_op);
static wv w6_modeOf(wv v626_x);
static wv w6_unFn(wv v627_op);
static wv w6_repeat(wv v628_s, wv v629_count);
static wv w6_utf8Length(wv v632_s);
static wv w6_cString(wv v637_s);
static wv w6_wstrInit(wv v643_s);
static wv w6_values(wv v646_cs);
static wv w6_generateC(wv p_v647_modules, wv p_v648_runtime);
static wv s672_modId(struct fr142 *up, wv v733_m);
static wv s673_hasFrame(struct fr142 *up, wv v734_f);
static wv s674_newScope(struct fr142 *up, wv v735_parent);
static wv s675_put(struct fr142 *up, wv v737_scope, wv v738_name, wv v739_kind, wv v740_d);
static wv s676_find(struct fr142 *up, wv v741_scope, wv v742_name);
static wv s677_kindOf(struct fr142 *up, wv v746_scope, wv v747_name);
static wv s678_newFn(struct fr142 *up, wv v749_name, wv v750_parent);
static wv s679_bind(struct fr142 *up, wv v752_kind, wv v753_name, wv v754_f, wv v755_atTop, wv v756_mode);
static wv s680_ablock(struct fr142 *up, wv v759_b, wv v760_scope, wv v761_f, wv v762_topScope);
static wv s681_astmt(struct fr142 *up, wv v770_s, wv v771_scope, wv v772_f, wv v773_topScope);
static wv s682_use(struct fr142 *up, wv v784_x, wv v785_scope, wv v786_f);
static wv s683_atarget(struct fr142 *up, wv v788_t, wv v789_scope, wv v790_f);
static wv s684_notValue(struct fr142 *up, wv v791_x, wv v792_scope);
static wv s685_aexpr(struct fr142 *up, wv v794_e, wv v795_scope, wv v796_f);
static wv s686_analyze(struct fr142 *up, wv v804_m);
static wv s687_enter(struct fr142 *up, wv v811_m);
static wv s688_emit(struct fr142 *up, wv v815_line);
static wv s689_tmp(struct fr142 *up);
static wv s690_str(struct fr142 *up, wv v817_s);
static wv s691_fieldRef(struct fr142 *up, wv v819_name);
static wv s692_enumDef(struct fr142 *up, wv v820_s, wv v821_m);
static wv s693_stolenFrom(struct fr142 *up, wv v825_x);
static wv s694_enumIn(struct fr142 *up, wv v827_list, wv v828_name);
static wv s695_enumOf(struct fr142 *up, wv v830_x);
static wv s696_place(struct fr142 *up, wv v835_d);
static wv s697_tdz(struct fr142 *up, wv v837_d);
static wv s698_read(struct fr142 *up, wv v838_d);
static wv s699_seq(struct fr142 *up, wv v839_args);
static wv s700_wrap(struct fr142 *up, wv v846_sq, wv v847_inner);
static wv s701_stmts(struct fr142 *up, wv v848_list);
static wv s702_resets(struct fr142 *up, wv v850_b);
static wv s703_blockBody(struct fr142 *up, wv v854_b);
static wv s704_valueOf(struct fr142 *up, wv v855_b);
static wv s705_stmt(struct fr142 *up, wv v856_s);
static wv s706_signal(struct fr142 *up, wv v858_code);
static wv s707_bindDecl(struct fr142 *up, wv v859_d, wv v860_value);
static wv s708_fnDef(struct fr142 *up, wv v862_s);
static wv s709_bindRhs(struct fr142 *up, wv v882_e, wv v883_mode);
static wv s710_setStmt(struct fr142 *up, wv v885_s);
static wv s711_exprs(struct fr142 *up, wv v900_list);
static wv s712_array(struct fr142 *up, wv v903_e, wv v904_mutable);
static wv s713_mapLit(struct fr142 *up, wv v906_e, wv v907_mutable);
static wv s714_record(struct fr142 *up, wv v909_e, wv v910_mutable);
static wv s715_onfailStmt(struct fr142 *up, wv v918_s);
static wv s716_chainStmt(struct fr142 *up, wv v927_e);
static wv s717_test(struct fr142 *up, wv v930_b);
static wv s718_branchCount(struct fr142 *up, wv v931_e);
static wv s719_branchBody(struct fr142 *up, wv v932_e, wv v933_i);
static wv s720_branchTest(struct fr142 *up, wv v934_e, wv v935_t, wv v936_i);
static wv s721_isPlain(struct fr142 *up, wv v937_e);
static wv s722_ternaryChain(struct fr142 *up, wv v941_e, wv v942_t);
static wv s723_branchValue(struct fr142 *up, wv v945_e, wv v946_t, wv v947_subject);
static wv s724_chainExpr(struct fr142 *up, wv v956_e);
static wv s725_caseTest(struct fr142 *up, wv v957_t, wv v958_a);
static wv s726_matchElse(struct fr142 *up, wv v961_e, wv v962_t);
static wv s727_matchStmt(struct fr142 *up, wv v963_e);
static wv s728_matchExpr(struct fr142 *up, wv v967_e);
static wv s729_callC(struct fr142 *up, wv v971_callee, wv v972_args, wv v973_checked);
static wv s730_expr(struct fr142 *up, wv v982_e);
static wv s731_assemble(struct fr142 *up);
static wv w7_dumpTokens(wv v1012_toks, wv v1013_indent);
static wv w7_sorted(wv v1019_xs);
static wv w7_dumpValue(wv v1024_x, wv v1025_indent);
static wv w8_dirOf(wv v1034_path);
static wv w8_normalize(wv v1037_path);
static wv w8_with(wv v1042_xs, wv v1043_x);
static wv w8_failIn(wv v1046_file, wv v1047_chars, wv v1048_pos, wv v1049_message);
static wv w8_loadProgram(wv v1050_entryFile, wv v1051_entryChars, wv p_v1052_libDir, wv p_v1053_searched);
static wv s1055_target(struct fr212 *up, wv v1059_s, wv v1060_file, wv v1061_chars, wv v1062_path);
static wv s1056_loaded(struct fr212 *up, wv v1063_path);
static wv s1057_visit(struct fr212 *up, wv v1065_file, wv v1066_chars, wv v1067_path, wv v1068_chain);
static wv w_die(void);
static wv w_load(wv v1083_file);
static wv w_onWindows(void);
static wv w_tryRead(wv v1089_path);
static wv w_programDir(void);
static wv w_readRuntime(void);
static wv w_findLib(void);

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
  fr.v76_toks = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v169_t0 = rt_bind(rt_chk(s84_next(&fr), "lex"), 0);
    (void)std_push(fr.v76_toks, v169_t0);
    if (rt_cond(rt_eq(rt_get(&F_t, v169_t0), rt_s(&S110)))) {
      break;
    }
  }
  RT_RET(fr.v76_toks);
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
  wv v150_importPath = rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ge(std_length(rt_def(up->v76_toks, "toks")), rt_i(3)), "&&") && rt_bool(rt_eq(rt_get(&F_t, rt_idx(rt_i(-1), rt_def(up->v76_toks, "toks"))), rt_s(&S137)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_v, rt_idx(rt_i(-1), rt_def(up->v76_toks, "toks"))), rt_s(&S157)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, rt_idx(rt_i(-3), rt_def(up->v76_toks, "toks"))), rt_s(&S47)), "&&"));
  wv v151_k = rt_add(v145_start, rt_i(1));
  while (rt_cond(rt_b(1))) {
    wv v152_c = rt_bind(rt_chk(s77_ch(up, v151_k), "string"), 0);
    if (rt_cond(rt_eq(v152_c, rt_s(&S13)))) {
      (void)rt_chk(w2_abort(v145_start, rt_s(&S158)), "string");
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(v152_c, rt_s(&S2)), "||") || rt_bool(rt_eq(v152_c, rt_s(&S3)), "||")))) {
      (void)rt_chk(w2_failHint(v151_k, rt_s(&S159), rt_s(&S160)), "string");
    }
    if (rt_cond(rt_eq(v152_c, rt_s(&S18)))) {
      v151_k = rt_add(v151_k, rt_i(1));
      break;
    }
    if (rt_cond(rt_eq(v152_c, rt_s(&S19)))) {
      wv v153_e = rt_bind(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string"), 0);
      wv v154_simple = rt_bind(rt_chk(w3_simpleEscape(v153_e), "string"), 0);
      if (rt_cond(rt_ne(v154_simple, RT_NULL))) {
        v148_buf = rt_add(v148_buf, v154_simple);
        v151_k = rt_add(v151_k, rt_i(2));
        continue;
      }
      if (rt_cond(rt_b(rt_bool(rt_eq(v153_e, rt_s(&S161)), "&&") && rt_bool(rt_eq(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(2))), "string"), rt_s(&S100)), "&&")))) {
        wv v155_close = rt_bind(rt_chk(s80_find(up, rt_s(&S15), rt_add(v151_k, rt_i(3))), "string"), 0);
        wv v156_hex = rt_bind((rt_cond(rt_lt(v155_close, rt_i(0))) ? rt_s(&S13) : ({ wv t199 = rt_add(v151_k, rt_i(3)); wv t200 = v155_close; rt_chk(s78_cut(up, t199, t200), "string"); })), 0);
        wv v157_cp = rt_bind(rt_chk(w1_hexValue(v156_hex), "string"), 0);
        if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_lt(v157_cp, rt_i(0)), "||") || rt_bool(rt_gt(v157_cp, rt_i(1114111)), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v157_cp, rt_i(55296)), "&&") && rt_bool(rt_le(v157_cp, rt_i(57343)), "&&")), "||")))) {
          (void)rt_chk(w2_failHint(v151_k, rt_s(&S162), rt_s(&S163)), "string");
        }
        v148_buf = ({ wv t201 = v148_buf; wv t202 = std_fromCode(v157_cp); rt_add(t201, t202); });
        v151_k = rt_add(v155_close, rt_i(1));
        continue;
      }
      (void)({ wv t203 = v151_k; wv t204 = rt_cat(3, (wv[]){ rt_s(&S164), rt_text(v153_e), rt_s(&S113) }); rt_chk(w2_failHint(t203, t204, rt_s(&S165)), "string"); });
    }
    if (rt_cond(rt_eq(v152_c, rt_s(&S100)))) {
      if (rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string"), rt_s(&S100)))) {
        v148_buf = rt_add(v148_buf, rt_s(&S100));
        v151_k = rt_add(v151_k, rt_i(2));
        continue;
      }
      if (rt_cond(({ wv t205 = v149_rawDollar; wv t206 = rt_sub(v151_k, rt_i(1)); rt_eq(t205, t206); }))) {
        (void)({ wv t207 = w2_warnings; wv t208 = rt_chk(w2_warn(v149_rawDollar, rt_s(&S166), RT_NULL), "string"); std_push(t207, t208); });
      }
      (void)std_push(v147_parts, v148_buf);
      v148_buf = rt_s(&S13);
      wv v158_sub = rt_bind(rt_chk(s89_interpolation(up, v151_k), "string"), 0);
      (void)({ wv t211 = v147_parts; wv t212 = ({ wv t209 = rt_get(&F_tokens, v158_sub); wv t210 = v151_k; rt_rec(0, 2, K3, (wv[]){ t209, t210 }); }); std_push(t211, t212); });
      v151_k = rt_bind(rt_get(&F_end, v158_sub), 1);
      continue;
    }
    if (rt_cond(rt_eq(v152_c, rt_s(&S15)))) {
      if (rt_cond(rt_eq(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string"), rt_s(&S15)))) {
        v148_buf = rt_add(v148_buf, rt_s(&S15));
        v151_k = rt_add(v151_k, rt_i(2));
        continue;
      }
      (void)rt_chk(w2_failHint(v151_k, rt_s(&S167), rt_s(&S168)), "string");
    }
    if (rt_cond(rt_eq(v152_c, rt_s(&S105)))) {
      v149_rawDollar = rt_bind(v151_k, 1);
      if (rt_cond(rt_b(rt_bool(rt_not(v150_importPath), "&&") && rt_bool(rt_chk(w1_isIdStart(rt_chk(s77_ch(up, rt_add(v151_k, rt_i(1))), "string")), "string"), "&&")))) {
        wv v159_j = rt_add(v151_k, rt_i(1));
        while (rt_cond(rt_chk(w1_isIdChar(rt_chk(s77_ch(up, v159_j), "string")), "string"))) {
          v159_j = rt_add(v159_j, rt_i(1));
        }
        wv v160_name = rt_bind(({ wv t213 = rt_add(v151_k, rt_i(1)); wv t214 = v159_j; rt_chk(s78_cut(up, t213, t214), "string"); }), 0);
        (void)({ wv t217 = w2_warnings; wv t218 = ({ wv t215 = v151_k; wv t216 = rt_cat(3, (wv[]){ rt_s(&S169), rt_text(v160_name), rt_s(&S170) }); rt_chk(w2_warn(t215, t216, rt_s(&S171)), "string"); }); std_push(t217, t218); });
      }
    }
    v148_buf = rt_add(v148_buf, v152_c);
    v151_k = rt_add(v151_k, rt_i(1));
  }
  (void)std_push(v147_parts, v148_buf);
  wv t219 = rt_bind(v151_k, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t219;
  RT_RET(rt_chk(s81_tok(up, rt_s(&S172), v145_start, v146_sp, RT_NULL, RT_NULL, v147_parts), "string"));
}

static wv s89_interpolation(struct fr27 *up, wv v161_open) {
  rt_enter("interpolation");
  v161_open = rt_bind(v161_open, 0);
  wv v162_saveI = rt_bind(rt_def(up->v73_i, "i"), 0);
  wv v163_savePrev = rt_bind(rt_def(up->v74_prevT, "prevT"), 0);
  wv v164_saveNl = rt_bind(rt_def(up->v75_nlNow, "nlNow"), 0);
  wv t220 = rt_add(v161_open, rt_i(1));
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t220;
  wv t221 = rt_s(&S13);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t221;
  wv v165_sub = rt_arr(0, NULL, 1);
  wv v166_depth = rt_i(0);
  while (rt_cond(rt_b(1))) {
    wv v168_tk = rt_bind(rt_chk(s84_next(up), "interpolation"), 0);
    if (rt_cond(rt_eq(rt_get(&F_t, v168_tk), rt_s(&S110)))) {
      (void)rt_chk(w2_abort(v161_open, rt_s(&S173)), "interpolation");
    }
    if (rt_cond(rt_eq(rt_get(&F_t, v168_tk), rt_s(&S15)))) {
      if (rt_cond(rt_eq(v166_depth, rt_i(0)))) {
        (void)({ wv t226 = v165_sub; wv t227 = ({ wv t222 = rt_get(&F_pos, v168_tk); wv t223 = rt_get(&F_end, v168_tk); wv t224 = rt_get(&F_sp, v168_tk); wv t225 = rt_get(&F_nl, v168_tk); rt_rec(0, 8, K2, (wv[]){ rt_s(&S110), t222, t223, t224, t225, RT_NULL, RT_NULL, RT_NULL }); }); std_push(t226, t227); });
        break;
      }
      v166_depth = rt_sub(v166_depth, rt_i(1));
    } else if (rt_cond(rt_eq(rt_get(&F_t, v168_tk), rt_s(&S100)))) {
      v166_depth = rt_add(v166_depth, rt_i(1));
    }
    (void)std_push(v165_sub, v168_tk);
  }
  wv v167_endI = rt_bind(rt_def(up->v73_i, "i"), 0);
  wv t228 = rt_bind(v162_saveI, 1);
  (void)rt_def(up->v73_i, "i");
  up->v73_i = t228;
  wv t229 = rt_bind(v163_savePrev, 1);
  (void)rt_def(up->v74_prevT, "prevT");
  up->v74_prevT = t229;
  wv t230 = rt_bind(v164_saveNl, 1);
  (void)rt_def(up->v75_nlNow, "nlNow");
  up->v75_nlNow = t230;
  if (rt_cond(rt_eq(std_length(v165_sub), rt_i(1)))) {
    (void)rt_chk(w2_failHint(v161_open, rt_s(&S174), rt_s(&S175)), "interpolation");
  }
  RT_RET(rt_rec(0, 2, K4, (wv[]){ v165_sub, v167_endI }));
}

static wv w4_precedence(wv v170_t) {
  rt_enter("precedence");
  v170_t = rt_bind(v170_t, 0);
  if (rt_cond(rt_eq(v170_t, rt_s(&S81)))) {
    RT_RET(rt_i(1));
  }
  if (rt_cond(rt_eq(v170_t, rt_s(&S80)))) {
    RT_RET(rt_i(2));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S76)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S77)), "||")))) {
    RT_RET(rt_i(3));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S89)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S90)), "||")), "||") || rt_bool(rt_eq(v170_t, rt_s(&S78)), "||")), "||") || rt_bool(rt_eq(v170_t, rt_s(&S79)), "||")))) {
    RT_RET(rt_i(4));
  }
  if (rt_cond(rt_eq(v170_t, rt_s(&S92)))) {
    RT_RET(rt_i(5));
  }
  if (rt_cond(rt_eq(v170_t, rt_s(&S32)))) {
    RT_RET(rt_i(6));
  }
  if (rt_cond(rt_eq(v170_t, rt_s(&S91)))) {
    RT_RET(rt_i(7));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S82)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S83)), "||")))) {
    RT_RET(rt_i(8));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S84)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S85)), "||")))) {
    RT_RET(rt_i(9));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v170_t, rt_s(&S86)), "||") || rt_bool(rt_eq(v170_t, rt_s(&S87)), "||")), "||") || rt_bool(rt_eq(v170_t, rt_s(&S88)), "||")))) {
    RT_RET(rt_i(10));
  }
  RT_RET(rt_i(0));
}

static wv w4_describe(wv v171_tk) {
  rt_enter("describe");
  v171_tk = rt_bind(v171_tk, 0);
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S110)))) {
    RT_RET(rt_s(&S179));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S137)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_v, v171_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S156)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S180), rt_text(rt_get(&F_v, v171_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S148)), "||")))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_text, v171_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S172)))) {
    RT_RET(rt_s(&S181));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v171_tk), rt_s(&S111)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_v, v171_tk)), rt_s(&S182) }));
  }
  RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v171_tk)), rt_s(&S113) }));
}

static wv w4_failAtHint(wv v172_tk, wv v173_message, wv v174_hint) {
  rt_enter("failAtHint");
  v172_tk = rt_bind(v172_tk, 0);
  v173_message = rt_bind(v173_message, 0);
  v174_hint = rt_bind(v174_hint, 0);
  (void)({ wv t231 = rt_get(&F_pos, v172_tk); wv t232 = v173_message; wv t233 = v174_hint; rt_chk(w2_failHint(t231, t232, t233), "failAtHint"); });
  RT_RET(RT_NULL);
}

static wv w4_failAt(wv v175_tk, wv v176_message) {
  rt_enter("failAt");
  v175_tk = rt_bind(v175_tk, 0);
  v176_message = rt_bind(v176_message, 0);
  (void)({ wv t234 = rt_get(&F_pos, v175_tk); wv t235 = v176_message; rt_chk(w2_failHint(t234, t235, RT_NULL), "failAt"); });
  RT_RET(RT_NULL);
}

static wv w4_simpleStatement(wv v177_k, wv v178_e) {
  rt_enter("simpleStatement");
  v177_k = rt_bind(v177_k, 0);
  v178_e = rt_bind(v178_e, 0);
  RT_RET(rt_rec(0, 5, K5, (wv[]){ v177_k, v178_e, RT_NULL, RT_NULL, RT_NULL }));
}

static wv w4_parse(wv v179_toks) {
  rt_enter("parse");
  RT_RET(rt_chk(w4_parser(v179_toks, rt_b(0)), "parse"));
}

static wv w4_hasEffect(wv v180_e) {
  rt_enter("hasEffect");
  v180_e = rt_bind(v180_e, 0);
  wv v181_kind = rt_bind(rt_get(&F_k, v180_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v181_kind, rt_s(&S183)), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S184)), "||")), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S185)), "||")), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S44)), "||")))) {
    RT_RET(rt_b(1));
  }
  if (rt_cond(rt_eq(v181_kind, rt_s(&S172)))) {
    wv v182_i = rt_i(0);
    while (rt_cond(({ wv t236 = v182_i; wv t237 = std_length(rt_get(&F_parts, v180_e)); rt_lt(t236, t237); }))) {
      if (rt_cond(rt_b(rt_bool(rt_ne(std_typeOf(({ wv t238 = v182_i; wv t239 = rt_get(&F_parts, v180_e); rt_idx(t238, t239); })), rt_s(&S186)), "&&") && rt_bool(rt_chk(w4_hasEffect(({ wv t240 = v182_i; wv t241 = rt_get(&F_parts, v180_e); rt_idx(t240, t241); })), "hasEffect"), "&&")))) {
        RT_RET(rt_b(1));
      }
      v182_i = rt_add(v182_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v181_kind, rt_s(&S187)), "||") || rt_bool(rt_eq(v181_kind, rt_s(&S188)), "||")))) {
    wv v183_i = rt_i(0);
    while (rt_cond(({ wv t242 = v183_i; wv t243 = std_length(rt_get(&F_items, v180_e)); rt_lt(t242, t243); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(({ wv t244 = v183_i; wv t245 = rt_get(&F_items, v180_e); rt_idx(t244, t245); })), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v183_i = rt_add(v183_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v181_kind, rt_s(&S189)))) {
    wv v184_i = rt_i(0);
    while (rt_cond(({ wv t246 = v184_i; wv t247 = std_length(rt_get(&F_fields, v180_e)); rt_lt(t246, t247); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(rt_get(&F_e, ({ wv t248 = v184_i; wv t249 = rt_get(&F_fields, v180_e); rt_idx(t248, t249); }))), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v184_i = rt_add(v184_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v181_kind, rt_s(&S190)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_index, v180_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_arr, v180_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v181_kind, rt_s(&S191)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_rec, v180_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v181_kind, rt_s(&S192)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_e, v180_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v181_kind, rt_s(&S193)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_l, v180_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_r, v180_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v181_kind, rt_s(&S194)))) {
    RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_c, v180_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_a, v180_e)), "hasEffect"), "||")), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_b, v180_e)), "hasEffect"), "||")));
  }
  RT_RET(rt_b(0));
}

static wv w4_parser(wv p_v185_toks, wv v186_inner) {
  rt_enter("parser");
  struct fr49 fr = { 0 };
  fr.v185_toks = p_v185_toks;
  v186_inner = rt_bind(v186_inner, 0);
  fr.v187_p = rt_i(0);
  fr.v188_depth = rt_i(0);
  fr.v189_exports = rt_arr(0, NULL, 1);
  fr.v190_firstExport = RT_NULL;
  if (rt_cond(v186_inner)) {
    wv v365_e = rt_bind(rt_chk(s214_expr(&fr), "parser"), 0);
    if (rt_cond(rt_not(rt_chk(s192_at(&fr, rt_s(&S110)), "parser")))) {
      wv v366_tk = rt_bind(rt_chk(s191_peek(&fr), "parser"), 0);
      (void)({ wv t532 = v366_tk; wv t533 = rt_cat(3, (wv[]){ rt_s(&S335), rt_text(rt_chk(w4_describe(v366_tk), "parser")), rt_s(&S336) }); rt_chk(w4_failAt(t532, t533), "parser"); });
    }
    RT_RET(v365_e);
  }
  RT_RET(rt_chk(s200_program(&fr), "parser"));
}

static wv s191_peek(struct fr49 *up) {
  rt_enter("peek");
  RT_RET(({ wv t250 = rt_def(up->v187_p, "p"); wv t251 = up->v185_toks; rt_idx(t250, t251); }));
}

static wv s192_at(struct fr49 *up, wv v230_type) {
  rt_enter("at");
  v230_type = rt_bind(v230_type, 0);
  RT_RET(({ wv t254 = rt_get(&F_t, ({ wv t252 = rt_def(up->v187_p, "p"); wv t253 = up->v185_toks; rt_idx(t252, t253); })); wv t255 = v230_type; rt_eq(t254, t255); }));
}

static wv s193_atWord(struct fr49 *up, wv v231_word) {
  rt_enter("atWord");
  v231_word = rt_bind(v231_word, 0);
  RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t256 = rt_def(up->v187_p, "p"); wv t257 = up->v185_toks; rt_idx(t256, t257); })), rt_s(&S137)), "&&") && rt_bool(({ wv t260 = rt_get(&F_v, ({ wv t258 = rt_def(up->v187_p, "p"); wv t259 = up->v185_toks; rt_idx(t258, t259); })); wv t261 = v231_word; rt_eq(t260, t261); }), "&&")));
}

static wv s194_atCall(struct fr49 *up) {
  rt_enter("atCall");
  if (rt_cond(rt_b(rt_bool(({ wv t262 = rt_add(rt_def(up->v187_p, "p"), rt_i(2)); wv t263 = std_length(up->v185_toks); rt_ge(t262, t263); }), "||") || rt_bool(rt_ne(rt_get(&F_t, ({ wv t264 = rt_def(up->v187_p, "p"); wv t265 = up->v185_toks; rt_idx(t264, t265); })), rt_s(&S97)), "||")))) {
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t266 = rt_add(rt_def(up->v187_p, "p"), rt_i(2)); wv t267 = up->v185_toks; rt_idx(t266, t267); })), rt_s(&S16)), "&&") && rt_bool(({ wv t270 = rt_def(w4_callees, "callees"); wv t271 = rt_get(&F_t, ({ wv t268 = rt_add(rt_def(up->v187_p, "p"), rt_i(1)); wv t269 = up->v185_toks; rt_idx(t268, t269); })); rt_chk(w1_contains(t270, t271), "atCall"); }), "&&")))) {
    RT_RET(rt_b(1));
  }
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(({ wv t272 = rt_add(rt_def(up->v187_p, "p"), rt_i(4)); wv t273 = std_length(up->v185_toks); rt_lt(t272, t273); }), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t274 = rt_add(rt_def(up->v187_p, "p"), rt_i(1)); wv t275 = up->v185_toks; rt_idx(t274, t275); })), rt_s(&S137)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t276 = rt_add(rt_def(up->v187_p, "p"), rt_i(2)); wv t277 = up->v185_toks; rt_idx(t276, t277); })), rt_s(&S129)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t278 = rt_add(rt_def(up->v187_p, "p"), rt_i(3)); wv t279 = up->v185_toks; rt_idx(t278, t279); })), rt_s(&S137)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t280 = rt_add(rt_def(up->v187_p, "p"), rt_i(4)); wv t281 = up->v185_toks; rt_idx(t280, t281); })), rt_s(&S16)), "&&")));
}

static wv s195_next(struct fr49 *up) {
  rt_enter("next");
  wv v232_tk = rt_bind(({ wv t282 = rt_def(up->v187_p, "p"); wv t283 = up->v185_toks; rt_idx(t282, t283); }), 0);
  if (rt_cond(rt_ne(rt_get(&F_t, v232_tk), rt_s(&S110)))) {
    up->v187_p = rt_add(rt_def(up->v187_p, "p"), rt_i(1));
  }
  RT_RET(v232_tk);
}

static wv s196_accept(struct fr49 *up, wv v233_type) {
  rt_enter("accept");
  v233_type = rt_bind(v233_type, 0);
  RT_RET((rt_cond(rt_chk(s192_at(up, v233_type), "accept")) ? rt_chk(s195_next(up), "accept") : RT_NULL));
}

static wv s197_expect(struct fr49 *up, wv v234_type, wv v235_msg, wv v236_hint) {
  rt_enter("expect");
  v234_type = rt_bind(v234_type, 0);
  v235_msg = rt_bind(v235_msg, 0);
  v236_hint = rt_bind(v236_hint, 0);
  if (rt_cond(rt_chk(s192_at(up, v234_type), "expect"))) {
    RT_RET(rt_chk(s195_next(up), "expect"));
  }
  wv v237_found = rt_bind(rt_chk(s191_peek(up), "expect"), 0);
  wv v238_m = rt_bind((rt_cond(rt_eq(v235_msg, RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S195), rt_text(v234_type), rt_s(&S113) }) : v235_msg), 0);
  (void)({ wv t286 = v237_found; wv t287 = ({ wv t284 = rt_text(v238_m); wv t285 = rt_text(rt_chk(w4_describe(v237_found), "expect")); rt_cat(3, (wv[]){ t284, rt_s(&S196), t285 }); }); wv t288 = v236_hint; rt_chk(w4_failAtHint(t286, t287, t288), "expect"); });
  RT_RET(RT_NULL);
}

static wv s198_expectWord(struct fr49 *up, wv v239_word, wv v240_hint) {
  rt_enter("expectWord");
  v239_word = rt_bind(v239_word, 0);
  v240_hint = rt_bind(v240_hint, 0);
  if (rt_cond(rt_chk(s193_atWord(up, v239_word), "expectWord"))) {
    RT_RET(rt_chk(s195_next(up), "expectWord"));
  }
  wv v241_found = rt_bind(rt_chk(s191_peek(up), "expectWord"), 0);
  (void)({ wv t291 = v241_found; wv t292 = ({ wv t289 = rt_text(v239_word); wv t290 = rt_text(rt_chk(w4_describe(v241_found), "expectWord")); rt_cat(4, (wv[]){ rt_s(&S195), t289, rt_s(&S197), t290 }); }); wv t293 = v240_hint; rt_chk(w4_failAtHint(t291, t292, t293), "expectWord"); });
  RT_RET(RT_NULL);
}

static wv s199_lintSpace(struct fr49 *up, wv v242_op) {
  rt_enter("lintSpace");
  v242_op = rt_bind(v242_op, 0);
  if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_sp, v242_op)), "||") || rt_bool(rt_not(rt_get(&F_sp, rt_chk(s191_peek(up), "lintSpace"))), "||")))) {
    (void)({ wv t296 = w2_warnings; wv t297 = ({ wv t294 = rt_get(&F_pos, v242_op); wv t295 = rt_cat(3, (wv[]){ rt_s(&S198), rt_text(rt_get(&F_t, v242_op)), rt_s(&S113) }); rt_chk(w2_warn(t294, t295, RT_NULL), "lintSpace"); }); std_push(t296, t297); });
  }
  RT_RET(RT_NULL);
}

static wv s200_program(struct fr49 *up) {
  rt_enter("program");
  wv v243_stmts = rt_arr(0, NULL, 1);
  wv v244_started = rt_b(0);
  wv v245_ended = RT_NULL;
  while (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S110)), "program")))) {
    if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S14)), "program")))) {
      wv v247_tk = rt_bind(rt_chk(s191_peek(up), "program"), 0);
      wv v248_hint = rt_bind((rt_cond(rt_chk(s192_at(up, rt_s(&S97)), "program")) ? rt_s(&S199) : rt_s(&S200)), 0);
      (void)({ wv t298 = v247_tk; wv t299 = rt_cat(2, (wv[]){ rt_s(&S201), rt_text(rt_chk(w4_describe(v247_tk), "program")) }); wv t300 = v248_hint; rt_chk(w4_failAtHint(t298, t299, t300), "program"); });
    }
    wv v246_s = rt_bind(rt_chk(s201_statement(up), "program"), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v245_ended, RT_NULL), "&&") && rt_bool(rt_ne(rt_get(&F_k, v246_s), rt_s(&S202)), "&&")))) {
      (void)rt_chk(w4_failAt(v245_ended, rt_s(&S203)), "program");
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v246_s), rt_s(&S47)), "&&") && rt_bool(v244_started, "&&")))) {
      (void)rt_chk(w2_abort(rt_get(&F_pos, v246_s), rt_s(&S204)), "program");
    }
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, v246_s), rt_s(&S47)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v246_s), rt_s(&S202)), "&&")))) {
      v244_started = rt_b(1);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, v246_s), rt_s(&S205)))) {
      v245_ended = rt_rec(1, 1, K6, (wv[]){ rt_get(&F_pos, v246_s) });
    }
    (void)std_push(v243_stmts, v246_s);
  }
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_def(up->v190_firstExport, "firstExport"), RT_NULL), "&&") && rt_bool(rt_eq(v245_ended, RT_NULL), "&&")))) {
    (void)rt_chk(w4_failAt(rt_def(up->v190_firstExport, "firstExport"), rt_s(&S206)), "program");
  }
  RT_RET(({ wv t301 = v243_stmts; wv t302 = rt_get(&F_pos, rt_chk(s191_peek(up), "program")); rt_rec(0, 2, K7, (wv[]){ t301, t302 }); }));
}

static wv s201_statement(struct fr49 *up) {
  rt_enter("statement");
  wv v249_semi = rt_bind(rt_chk(s195_next(up), "statement"), 0);
  wv v250_tk = rt_bind(rt_chk(s191_peek(up), "statement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v250_tk), rt_s(&S14)), "||") || rt_bool(rt_eq(rt_get(&F_t, v250_tk), rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v250_tk), rt_s(&S110)), "||")))) {
    RT_RET(({ wv t303 = rt_get(&F_pos, v249_semi); wv t304 = rt_get(&F_pos, v250_tk); rt_rec(0, 4, K8, (wv[]){ rt_s(&S202), t303, t304, RT_NULL }); }));
  }
  wv v251_s = rt_bind(rt_chk(s202_statementBody(up, v250_tk), "statement"), 1);
  ({ wv t305 = rt_get(&F_pos, v250_tk); wv t306 = v251_s; rt_set_mem(t305, &F_pos, t306); });
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S56)), "statement"))) {
    wv v252_w = rt_bind(rt_chk(s195_next(up), "statement"), 0);
    if (rt_cond(rt_not(({ wv t307 = rt_def(w4_onfailOk, "onfailOk"); wv t308 = rt_get(&F_k, v251_s); rt_chk(w1_contains(t307, t308), "statement"); })))) {
      (void)rt_chk(w4_failAt(v252_w, rt_s(&S207)), "statement");
    }
    wv v253_param = RT_NULL;
    if (rt_cond(rt_ne(rt_chk(s196_accept(up, rt_s(&S97)), "statement"), RT_NULL))) {
      v253_param = rt_bind(rt_chk(s197_expect(up, rt_s(&S137), rt_s(&S208), RT_NULL), "statement"), 1);
      (void)rt_chk(s197_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "statement");
    }
    wv v254_noParam = rt_eq(v253_param, RT_NULL);
    ({ wv t313 = ({ wv t309 = rt_get(&F_pos, v252_w); wv t310 = (rt_cond(v254_noParam) ? RT_NULL : rt_get(&F_v, v253_param)); wv t311 = (rt_cond(v254_noParam) ? RT_NULL : rt_get(&F_pos, v253_param)); wv t312 = rt_chk(s212_block(up), "statement"); rt_rec(0, 4, K9, (wv[]){ t309, t310, t311, t312 }); }); wv t314 = v251_s; rt_set_mem(t313, &F_handler, t314); });
  }
  ({ wv t315 = rt_get(&F_pos, rt_chk(s191_peek(up), "statement")); wv t316 = v251_s; rt_set_mem(t315, &F_end, t316); });
  RT_RET(v251_s);
}

static wv s202_statementBody(struct fr49 *up, wv v255_tk) {
  rt_enter("statementBody");
  v255_tk = rt_bind(v255_tk, 0);
  wv v256_kind = rt_bind(rt_get(&F_t, v255_tk), 0);
  if (rt_cond(rt_eq(v256_kind, rt_s(&S33)))) {
    RT_RET(rt_chk(s209_fnStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S46)))) {
    RT_RET(rt_chk(s210_enumStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S47)))) {
    RT_RET(rt_chk(s206_importStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S48)))) {
    RT_RET(rt_chk(s207_exportStatement(up), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S37)), "||")))) {
    RT_RET(rt_chk(s203_declaration(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S38)))) {
    RT_RET(rt_chk(s204_setStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S34)))) {
    (void)rt_chk(s195_next(up), "statementBody");
    wv v258_e = rt_bind((rt_cond(({ wv t317 = rt_def(w4_exprStart, "exprStart"); wv t318 = rt_get(&F_t, rt_chk(s191_peek(up), "statementBody")); rt_chk(w1_contains(t317, t318), "statementBody"); })) ? rt_chk(s214_expr(up), "statementBody") : RT_NULL), 0);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S34), v258_e), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S57)))) {
    (void)rt_chk(s195_next(up), "statementBody");
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S57), rt_chk(s214_expr(up), "statementBody")), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S43)), "||")))) {
    (void)rt_chk(s195_next(up), "statementBody");
    RT_RET(rt_rec(0, 4, K8, (wv[]){ (rt_cond(rt_eq(v256_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43)), RT_NULL, RT_NULL, RT_NULL }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S41)), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S209)), "||")))) {
    (void)rt_chk(s195_next(up), "statementBody");
    RT_RET(({ wv t319 = rt_eq(v256_kind, rt_s(&S209)); wv t320 = rt_chk(s211_cond(up, v256_kind), "statementBody"); wv t321 = rt_chk(s212_block(up), "statementBody"); rt_rec(0, 7, K10, (wv[]){ rt_s(&S41), t319, t320, t321, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S55)))) {
    RT_RET(rt_chk(s205_failStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S54)))) {
    (void)rt_chk(s195_next(up), "statementBody");
    wv v259_e = rt_bind((rt_cond(({ wv t322 = rt_def(w4_exprStart, "exprStart"); wv t323 = rt_get(&F_t, rt_chk(s191_peek(up), "statementBody")); rt_chk(w1_contains(t322, t323), "statementBody"); })) ? rt_chk(s214_expr(up), "statementBody") : RT_NULL), 0);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S54), v259_e), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S137)), "&&") && rt_bool(rt_eq(rt_get(&F_v, v255_tk), rt_s(&S205)), "&&")))) {
    wv v260_after = rt_bind(rt_get(&F_t, ({ wv t324 = rt_add(rt_def(up->v187_p, "p"), rt_i(1)); wv t325 = up->v185_toks; rt_idx(t324, t325); })), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v260_after, rt_s(&S14)), "||") || rt_bool(rt_eq(v260_after, rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(v260_after, rt_s(&S110)), "||")))) {
      RT_RET(rt_chk(s208_endStatement(up), "statementBody"));
    }
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S56)))) {
    (void)rt_chk(w4_failAt(v255_tk, rt_s(&S207)), "statementBody");
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S40)))) {
    (void)rt_chk(w4_failAt(v255_tk, rt_s(&S210)), "statementBody");
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S45)))) {
    (void)rt_chk(w4_failAt(v255_tk, rt_s(&S211)), "statementBody");
  }
  wv v257_e = rt_bind(rt_chk(s214_expr(up), "statementBody"), 0);
  if (rt_cond(rt_b(rt_bool(rt_chk(s192_at(up, rt_s(&S111)), "statementBody"), "||") || rt_bool(rt_b(rt_bool(rt_chk(s193_atWord(up, rt_s(&S212)), "statementBody"), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v257_e), rt_s(&S213)), "||") || rt_bool(rt_eq(rt_get(&F_k, v257_e), rt_s(&S190)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v257_e), rt_s(&S191)), "||")), "&&")), "||")))) {
    (void)rt_chk(w4_failAtHint(v255_tk, rt_s(&S214), rt_s(&S215)), "statementBody");
  }
  if (rt_cond(rt_not(rt_chk(w4_hasEffect(v257_e), "statementBody")))) {
    wv v261_hint = rt_bind((rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0))) ? rt_s(&S216) : RT_NULL), 0);
    (void)({ wv t328 = w2_warnings; wv t329 = ({ wv t326 = rt_get(&F_pos, v255_tk); wv t327 = v261_hint; rt_chk(w2_warn(t326, rt_s(&S217), t327), "statementBody"); }); std_push(t328, t329); });
  }
  RT_RET(rt_chk(w4_simpleStatement(rt_s(&S177), v257_e), "statementBody"));
}

static wv s203_declaration(struct fr49 *up) {
  rt_enter("declaration");
  wv v262_kw = rt_bind(rt_chk(s195_next(up), "declaration"), 0);
  wv v263_name = rt_bind(({ wv t330 = rt_cat(3, (wv[]){ rt_s(&S218), rt_text(rt_get(&F_t, v262_kw)), rt_s(&S113) }); wv t331 = rt_cat(3, (wv[]){ rt_s(&S14), rt_text(rt_get(&F_t, v262_kw)), rt_s(&S219) }); rt_chk(s197_expect(up, rt_s(&S137), t330, t331), "declaration"); }), 0);
  (void)rt_chk(s198_expectWord(up, rt_s(&S220), ({ wv t332 = rt_text(rt_get(&F_t, v262_kw)); wv t333 = rt_text(rt_get(&F_v, v263_name)); rt_cat(5, (wv[]){ rt_s(&S14), t332, rt_s(&S0), t333, rt_s(&S221) }); })), "declaration");
  wv v264_e = rt_bind(rt_chk(s214_expr(up), "declaration"), 0);
  RT_RET(({ wv t334 = v264_e; wv t335 = rt_eq(rt_get(&F_t, v262_kw), rt_s(&S36)); wv t336 = rt_eq(rt_get(&F_t, v262_kw), rt_s(&S37)); wv t337 = rt_get(&F_v, v263_name); wv t338 = rt_get(&F_pos, v263_name); rt_rec(0, 9, K11, (wv[]){ rt_s(&S178), t334, t335, t336, t337, t338, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s204_setStatement(struct fr49 *up) {
  rt_enter("setStatement");
  (void)rt_chk(s195_next(up), "setStatement");
  wv v265_target = rt_bind(rt_chk(s213_lvalue(up), "setStatement"), 0);
  if (rt_cond(rt_chk(s193_atWord(up, rt_s(&S212)), "setStatement"))) {
    (void)rt_chk(s195_next(up), "setStatement");
    RT_RET(({ wv t339 = rt_chk(s214_expr(up), "setStatement"); wv t340 = v265_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t339, RT_NULL, t340, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S111)), "setStatement"))) {
    wv v267_op = rt_bind(rt_chk(s195_next(up), "setStatement"), 0);
    RT_RET(({ wv t341 = rt_chk(s214_expr(up), "setStatement"); wv t342 = rt_get(&F_v, v267_op); wv t343 = v265_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t341, t342, t343, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  wv v266_found = rt_bind(rt_chk(s191_peek(up), "setStatement"), 0);
  (void)({ wv t344 = v266_found; wv t345 = rt_cat(2, (wv[]){ rt_s(&S222), rt_text(rt_chk(w4_describe(v266_found), "setStatement")) }); rt_chk(w4_failAtHint(t344, t345, rt_s(&S215)), "setStatement"); });
  RT_RET(RT_NULL);
}

static wv s205_failStatement(struct fr49 *up) {
  rt_enter("failStatement");
  wv v268_kw = rt_bind(rt_chk(s195_next(up), "failStatement"), 0);
  if (rt_cond(rt_not(({ wv t346 = rt_def(w4_exprStart, "exprStart"); wv t347 = rt_get(&F_t, rt_chk(s191_peek(up), "failStatement")); rt_chk(w1_contains(t346, t347), "failStatement"); })))) {
    RT_RET(rt_rec(0, 5, K13, (wv[]){ rt_s(&S55), rt_i(1), RT_NULL, RT_NULL, RT_NULL }));
  }
  wv v269_first = rt_bind(rt_chk(s191_peek(up), "failStatement"), 0);
  wv v270_e = rt_bind(rt_chk(s214_expr(up), "failStatement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v270_e), rt_s(&S148)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v270_e), rt_s(&S138)), "&&") && rt_bool(rt_lt(rt_get(&F_v, v270_e), rt_i(1)), "&&")), "||")))) {
    (void)rt_chk(w4_failAt(v269_first, rt_s(&S223)), "failStatement");
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v270_e), rt_s(&S138)))) {
    RT_RET(rt_rec(0, 5, K13, (wv[]){ rt_s(&S55), rt_get(&F_v, v270_e), RT_NULL, RT_NULL, RT_NULL }));
  }
  RT_RET(rt_chk(w4_simpleStatement(rt_s(&S177), ({ wv t348 = v270_e; wv t349 = rt_get(&F_pos, v268_kw); rt_rec(0, 4, K14, (wv[]){ rt_s(&S184), t348, t349, rt_b(0) }); })), "failStatement"));
}

static wv s206_importStatement(struct fr49 *up) {
  rt_enter("importStatement");
  wv v271_kw = rt_bind(rt_chk(s195_next(up), "importStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v271_kw, rt_s(&S224)), "importStatement");
  }
  wv v272_name = rt_bind(rt_chk(s197_expect(up, rt_s(&S137), rt_s(&S225), rt_s(&S226)), "importStatement"), 0);
  (void)rt_chk(s198_expectWord(up, rt_s(&S157), ({ wv t350 = rt_text(rt_get(&F_v, v272_name)); wv t351 = rt_text(rt_get(&F_v, v272_name)); rt_cat(5, (wv[]){ rt_s(&S227), t350, rt_s(&S228), t351, rt_s(&S229) }); })), "importStatement");
  wv v273_e = rt_bind(rt_chk(s191_peek(up), "importStatement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_t, v273_e), rt_s(&S172)), "||") || rt_bool(rt_ne(std_length(rt_get(&F_parts, v273_e)), rt_i(1)), "||")))) {
    (void)rt_chk(w4_failAtHint(v273_e, rt_s(&S230), rt_s(&S226)), "importStatement");
  }
  (void)rt_chk(s195_next(up), "importStatement");
  wv v274_path = rt_bind(rt_idx(rt_i(0), rt_get(&F_parts, v273_e)), 0);
  if (rt_cond(rt_chk(w1_contains(std_split(v274_path, rt_s(&S13)), rt_s(&S19)), "importStatement"))) {
    (void)rt_chk(w4_failAt(v273_e, rt_s(&S231)), "importStatement");
  }
  if (rt_cond(rt_not(rt_b(rt_bool(rt_ge(std_length(v274_path), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t352 = v274_path; wv t353 = std_length(v274_path); std_slice(t352, rt_i(-4), t353); }), rt_s(&S232)), "&&"))))) {
    (void)rt_chk(w4_failAt(v273_e, rt_s(&S233)), "importStatement");
  }
  if (rt_cond(rt_eq(std_at(v274_path, rt_i(0)), rt_s(&S105)))) {
    if (rt_cond(rt_b(rt_bool(rt_lt(std_length(v274_path), rt_i(5)), "||") || rt_bool(rt_ne(std_slice(v274_path, rt_i(0), rt_i(5)), rt_s(&S234)), "||")))) {
      (void)rt_chk(w4_failAtHint(v273_e, rt_s(&S235), rt_s(&S236)), "importStatement");
    }
    if (rt_cond(rt_chk(w1_contains(std_split(v274_path, rt_s(&S87)), rt_s(&S237)), "importStatement"))) {
      (void)rt_chk(w4_failAt(v273_e, rt_s(&S238)), "importStatement");
    }
  }
  RT_RET(({ wv t354 = v274_path; wv t355 = rt_get(&F_pos, v273_e); wv t356 = rt_get(&F_v, v272_name); wv t357 = rt_get(&F_pos, v272_name); rt_rec(0, 8, K15, (wv[]){ rt_s(&S47), t354, t355, t356, t357, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s207_exportStatement(struct fr49 *up) {
  rt_enter("exportStatement");
  wv v275_kw = rt_bind(rt_chk(s195_next(up), "exportStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v275_kw, rt_s(&S239)), "exportStatement");
  }
  wv v276_tk = rt_bind(rt_chk(s191_peek(up), "exportStatement"), 0);
  if (rt_cond(rt_not(({ wv t358 = rt_def(w4_exportable, "exportable"); wv t359 = rt_get(&F_t, v276_tk); rt_chk(w1_contains(t358, t359), "exportStatement"); })))) {
    (void)({ wv t360 = v276_tk; wv t361 = rt_cat(2, (wv[]){ rt_s(&S240), rt_text(rt_chk(w4_describe(v276_tk), "exportStatement")) }); rt_chk(w4_failAt(t360, t361), "exportStatement"); });
  }
  wv v277_s = rt_bind(rt_chk(s202_statementBody(up, v276_tk), "exportStatement"), 0);
  if (rt_cond(rt_eq(rt_def(up->v190_firstExport, "firstExport"), RT_NULL))) {
    wv t362 = rt_bind(v275_kw, 1);
    (void)rt_def(up->v190_firstExport, "firstExport");
    up->v190_firstExport = t362;
  }
  (void)({ wv t365 = rt_def(up->v189_exports, "exports"); wv t366 = ({ wv t363 = rt_get(&F_name, v277_s); wv t364 = rt_get(&F_namePos, v277_s); rt_rec(0, 2, K16, (wv[]){ t363, t364 }); }); std_push(t365, t366); });
  RT_RET(v277_s);
}

static wv s208_endStatement(struct fr49 *up) {
  rt_enter("endStatement");
  wv v278_kw = rt_bind(rt_chk(s195_next(up), "endStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v278_kw, rt_s(&S241)), "endStatement");
  }
  if (rt_cond(rt_eq(std_length(rt_def(up->v189_exports, "exports")), rt_i(0)))) {
    (void)rt_chk(w4_failAtHint(v278_kw, rt_s(&S242), rt_s(&S243)), "endStatement");
  }
  RT_RET(rt_rec(0, 5, K17, (wv[]){ rt_s(&S205), rt_def(up->v189_exports, "exports"), RT_NULL, RT_NULL, RT_NULL }));
}

static wv s209_fnStatement(struct fr49 *up) {
  rt_enter("fnStatement");
  (void)rt_chk(s195_next(up), "fnStatement");
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S97)), "fnStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "fnStatement"), rt_s(&S244), rt_s(&S245)), "fnStatement");
  }
  wv v279_name = rt_bind(rt_chk(s197_expect(up, rt_s(&S137), rt_s(&S246), rt_s(&S245)), "fnStatement"), 0);
  (void)rt_chk(s197_expect(up, rt_s(&S97), rt_s(&S247), rt_cat(3, (wv[]){ rt_s(&S248), rt_text(rt_get(&F_v, v279_name)), rt_s(&S249) })), "fnStatement");
  wv v280_params = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S16)), "fnStatement")))) {
    while (rt_cond(rt_b(1))) {
      wv v281_isVar = rt_ne(rt_chk(s196_accept(up, rt_s(&S36)), "fnStatement"), RT_NULL);
      wv v282_isRef = rt_b(rt_bool(rt_not(v281_isVar), "&&") && rt_bool(rt_ne(rt_chk(s196_accept(up, rt_s(&S37)), "fnStatement"), RT_NULL), "&&"));
      wv v283_pname = rt_bind(rt_chk(s197_expect(up, rt_s(&S137), rt_s(&S250), rt_s(&S251)), "fnStatement"), 0);
      (void)({ wv t371 = v280_params; wv t372 = ({ wv t367 = rt_get(&F_v, v283_pname); wv t368 = v281_isVar; wv t369 = v282_isRef; wv t370 = rt_get(&F_pos, v283_pname); rt_rec(0, 4, K18, (wv[]){ t367, t368, t369, t370 }); }); std_push(t371, t372); });
      if (rt_cond(rt_eq(rt_chk(s196_accept(up, rt_s(&S96)), "fnStatement"), RT_NULL))) {
        break;
      }
    }
  }
  (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S252), RT_NULL), "fnStatement");
  RT_RET(({ wv t373 = rt_get(&F_v, v279_name); wv t374 = rt_get(&F_pos, v279_name); wv t375 = v280_params; wv t376 = rt_chk(s212_block(up), "fnStatement"); rt_rec(0, 8, K19, (wv[]){ rt_s(&S33), t373, t374, t375, t376, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s210_enumStatement(struct fr49 *up) {
  rt_enter("enumStatement");
  wv v284_kw = rt_bind(rt_chk(s195_next(up), "enumStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v284_kw, rt_s(&S253)), "enumStatement");
  }
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S97)), "enumStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "enumStatement"), rt_s(&S254), rt_s(&S255)), "enumStatement");
  }
  wv v285_name = rt_bind(rt_chk(s197_expect(up, rt_s(&S137), rt_s(&S256), rt_s(&S255)), "enumStatement"), 0);
  (void)rt_chk(s197_expect(up, rt_s(&S97), rt_s(&S257), rt_cat(3, (wv[]){ rt_s(&S258), rt_text(rt_get(&F_v, v285_name)), rt_s(&S259) })), "enumStatement");
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S16)), "enumStatement"))) {
    (void)({ wv t377 = rt_chk(s191_peek(up), "enumStatement"); wv t378 = rt_cat(3, (wv[]){ rt_s(&S258), rt_text(rt_get(&F_v, v285_name)), rt_s(&S259) }); rt_chk(w4_failAtHint(t377, rt_s(&S260), t378), "enumStatement"); });
  }
  wv v286_members = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v287_m = rt_bind(rt_chk(s191_peek(up), "enumStatement"), 0);
    if (rt_cond(rt_ne(rt_get(&F_t, v287_m), rt_s(&S137)))) {
      wv v289_message = rt_bind((rt_cond(({ wv t379 = w3_keywords; wv t380 = rt_get(&F_t, v287_m); rt_chk(w1_contains(t379, t380), "enumStatement"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v287_m)), rt_s(&S261) }) : rt_cat(2, (wv[]){ rt_s(&S262), rt_text(rt_chk(w4_describe(v287_m), "enumStatement")) })), 0);
      (void)rt_chk(w4_failAt(v287_m, v289_message), "enumStatement");
    }
    (void)rt_chk(s195_next(up), "enumStatement");
    wv v288_k = rt_i(0);
    while (rt_cond(({ wv t381 = v288_k; wv t382 = std_length(v286_members); rt_lt(t381, t382); }))) {
      if (rt_cond(({ wv t383 = rt_get(&F_name, rt_idx(v288_k, v286_members)); wv t384 = rt_get(&F_v, v287_m); rt_eq(t383, t384); }))) {
        (void)({ wv t385 = v287_m; wv t386 = rt_cat(3, (wv[]){ rt_s(&S263), rt_text(rt_get(&F_v, v287_m)), rt_s(&S264) }); rt_chk(w4_failAt(t385, t386), "enumStatement"); });
      }
      v288_k = rt_add(v288_k, rt_i(1));
    }
    (void)({ wv t389 = v286_members; wv t390 = ({ wv t387 = rt_get(&F_v, v287_m); wv t388 = rt_get(&F_pos, v287_m); rt_rec(0, 2, K16, (wv[]){ t387, t388 }); }); std_push(t389, t390); });
    if (rt_cond(rt_eq(rt_chk(s196_accept(up, rt_s(&S96)), "enumStatement"), RT_NULL))) {
      break;
    }
  }
  (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S265), RT_NULL), "enumStatement");
  RT_RET(({ wv t391 = rt_get(&F_v, v285_name); wv t392 = rt_get(&F_pos, v285_name); wv t393 = v286_members; rt_rec(0, 7, K20, (wv[]){ rt_s(&S46), t391, t392, t393, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s211_cond(struct fr49 *up, wv v290_kw) {
  rt_enter("cond");
  v290_kw = rt_bind(v290_kw, 0);
  (void)({ wv t394 = rt_cat(3, (wv[]){ rt_s(&S266), rt_text(v290_kw), rt_s(&S113) }); wv t395 = rt_cat(3, (wv[]){ rt_s(&S267), rt_text(v290_kw), rt_s(&S268) }); rt_chk(s197_expect(up, rt_s(&S97), t394, t395), "cond"); });
  wv v291_e = rt_bind(rt_chk(s214_expr(up), "cond"), 0);
  (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S269), RT_NULL), "cond");
  RT_RET(v291_e);
}

static wv s212_block(struct fr49 *up) {
  rt_enter("block");
  (void)rt_chk(s197_expect(up, rt_s(&S100), rt_s(&S270), RT_NULL), "block");
  up->v188_depth = rt_add(rt_def(up->v188_depth, "depth"), rt_i(1));
  wv v292_stmts = rt_arr(0, NULL, 1);
  while (rt_cond(rt_chk(s192_at(up, rt_s(&S14)), "block"))) {
    (void)({ wv t396 = v292_stmts; wv t397 = rt_chk(s201_statement(up), "block"); std_push(t396, t397); });
  }
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S15)), "block"))) {
    wv v295_close = rt_bind(rt_chk(s195_next(up), "block"), 0);
    up->v188_depth = rt_sub(rt_def(up->v188_depth, "depth"), rt_i(1));
    RT_RET(({ wv t398 = v292_stmts; wv t399 = rt_get(&F_pos, v295_close); rt_rec(0, 3, K21, (wv[]){ t398, RT_NULL, t399 }); }));
  }
  if (rt_cond(rt_chk(s192_at(up, rt_s(&S110)), "block"))) {
    (void)rt_chk(w4_failAt(rt_chk(s191_peek(up), "block"), rt_s(&S271)), "block");
  }
  wv v293_value = rt_bind(rt_chk(s214_expr(up), "block"), 0);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S15)), "block")))) {
    wv v296_hint = rt_bind((rt_cond(rt_chk(s192_at(up, rt_s(&S28)), "block")) ? rt_s(&S272) : rt_s(&S273)), 0);
    wv v297_found = rt_bind(rt_chk(s191_peek(up), "block"), 0);
    (void)({ wv t400 = v297_found; wv t401 = rt_cat(2, (wv[]){ rt_s(&S274), rt_text(rt_chk(w4_describe(v297_found), "block")) }); wv t402 = v296_hint; rt_chk(w4_failAtHint(t400, t401, t402), "block"); });
  }
  wv v294_close = rt_bind(rt_chk(s195_next(up), "block"), 0);
  up->v188_depth = rt_sub(rt_def(up->v188_depth, "depth"), rt_i(1));
  RT_RET(({ wv t403 = v292_stmts; wv t404 = v293_value; wv t405 = rt_get(&F_pos, v294_close); rt_rec(0, 3, K21, (wv[]){ t403, t404, t405 }); }));
}

static wv s213_lvalue(struct fr49 *up) {
  rt_enter("lvalue");
  wv v298_tk = rt_bind(rt_chk(s191_peek(up), "lvalue"), 0);
  wv v299_e = rt_bind(rt_chk(s221_postfix(up), "lvalue"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_k, v299_e), rt_s(&S213)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v299_e), rt_s(&S190)), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_k, v299_e), rt_s(&S191)), "&&")))) {
    (void)rt_chk(w4_failAt(v298_tk, rt_s(&S275)), "lvalue");
  }
  RT_RET(v299_e);
}

static wv s214_expr(struct fr49 *up) {
  rt_enter("expr");
  RT_RET(rt_chk(s215_ternary(up), "expr"));
}

static wv s215_ternary(struct fr49 *up) {
  rt_enter("ternary");
  RT_RET(rt_chk(s216_ternaryTail(up, rt_chk(s217_binary(up, rt_i(1)), "ternary")), "ternary"));
}

static wv s216_ternaryTail(struct fr49 *up, wv v300_c) {
  rt_enter("ternaryTail");
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S95)), "ternaryTail")))) {
    RT_RET(v300_c);
  }
  wv v301_q = rt_bind(rt_chk(s195_next(up), "ternaryTail"), 0);
  (void)rt_chk(s199_lintSpace(up, v301_q), "ternaryTail");
  wv v302_a = rt_bind(rt_chk(s215_ternary(up), "ternaryTail"), 0);
  wv v303_colon = rt_bind(rt_chk(s197_expect(up, rt_s(&S28), rt_s(&S276), RT_NULL), "ternaryTail"), 0);
  (void)rt_chk(s199_lintSpace(up, v303_colon), "ternaryTail");
  wv v304_b = rt_bind(rt_chk(s215_ternary(up), "ternaryTail"), 0);
  RT_RET(({ wv t406 = v300_c; wv t407 = v302_a; wv t408 = v304_b; wv t409 = rt_get(&F_pos, v301_q); rt_rec(0, 6, K22, (wv[]){ rt_s(&S194), t406, t407, t408, t409, rt_b(0) }); }));
}

static wv s217_binary(struct fr49 *up, wv v305_min) {
  rt_enter("binary");
  v305_min = rt_bind(v305_min, 0);
  RT_RET(({ wv t410 = rt_chk(s219_unary(up), "binary"); wv t411 = v305_min; rt_chk(s218_binaryFrom(up, t410, t411), "binary"); }));
}

static wv s218_binaryFrom(struct fr49 *up, wv v306_left, wv v307_min) {
  rt_enter("binaryFrom");
  v306_left = rt_bind(v306_left, 0);
  v307_min = rt_bind(v307_min, 0);
  wv v308_acc = v306_left;
  while (rt_cond(rt_b(1))) {
    wv v309_op = rt_bind(rt_chk(s191_peek(up), "binaryFrom"), 0);
    wv v310_prec = rt_bind(rt_chk(w4_precedence(rt_get(&F_t, v309_op)), "binaryFrom"), 0);
    if (rt_cond(rt_lt(v310_prec, v307_min))) {
      RT_RET(v308_acc);
    }
    (void)rt_chk(s195_next(up), "binaryFrom");
    (void)rt_chk(s199_lintSpace(up, v309_op), "binaryFrom");
    wv v311_right = rt_chk(s217_binary(up, rt_add(v310_prec, rt_i(1))), "binaryFrom");
    v308_acc = ({ wv t412 = rt_get(&F_t, v309_op); wv t413 = v308_acc; wv t414 = v311_right; wv t415 = rt_get(&F_pos, v309_op); rt_rec(0, 6, K23, (wv[]){ rt_s(&S193), t412, t413, t414, t415, rt_b(0) }); });
  }
  RT_RET(RT_NULL);
}

static wv s219_unary(struct fr49 *up) {
  rt_enter("unary");
  wv v312_tk = rt_bind(rt_chk(s191_peek(up), "unary"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S85)), "||") || rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S94)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S93)), "||")))) {
    (void)rt_chk(s195_next(up), "unary");
    wv v313_e = rt_bind(rt_chk(s219_unary(up), "unary"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S85)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v313_e), rt_s(&S193)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v313_e), rt_s(&S75)), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v313_e)), "&&")))) {
      (void)rt_chk(w4_failAtHint(v312_tk, rt_s(&S277), rt_s(&S278)), "unary");
    }
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S85)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v313_e), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_k, v313_e), rt_s(&S148)), "||")), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v313_e)), "&&")))) {
      RT_RET(({ wv t416 = rt_get(&F_k, v313_e); wv t417 = rt_neg(rt_get(&F_v, v313_e)); wv t418 = rt_get(&F_pos, v312_tk); rt_rec(0, 4, K24, (wv[]){ t416, t417, t418, rt_b(0) }); }));
    }
    RT_RET(({ wv t419 = rt_get(&F_t, v312_tk); wv t420 = v313_e; wv t421 = rt_get(&F_pos, v312_tk); rt_rec(0, 5, K25, (wv[]){ rt_s(&S192), t419, t420, t421, rt_b(0) }); }));
  }
  RT_RET(rt_chk(s220_powerTail(up, rt_chk(s221_postfix(up), "unary")), "unary"));
}

static wv s220_powerTail(struct fr49 *up, wv v314_base) {
  rt_enter("powerTail");
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S75)), "powerTail")))) {
    RT_RET(v314_base);
  }
  wv v315_op = rt_bind(rt_chk(s195_next(up), "powerTail"), 0);
  (void)rt_chk(s199_lintSpace(up, v315_op), "powerTail");
  RT_RET(({ wv t422 = v314_base; wv t423 = rt_chk(s219_unary(up), "powerTail"); wv t424 = rt_get(&F_pos, v315_op); rt_rec(0, 6, K23, (wv[]){ rt_s(&S193), rt_s(&S75), t422, t423, t424, rt_b(0) }); }));
}

static wv s221_postfix(struct fr49 *up) {
  rt_enter("postfix");
  wv v316_e = rt_chk(s223_primary(up), "postfix");
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s194_atCall(up), "postfix"))) {
      v316_e = rt_chk(s222_call(up, rt_arr(1, (wv[]){ v316_e }, 0)), "postfix");
    } else if (rt_cond(rt_chk(s192_at(up, rt_s(&S98)), "postfix"))) {
      wv v317_open = rt_bind(rt_chk(s195_next(up), "postfix"), 0);
      if (rt_cond(rt_b(rt_bool(rt_get(&F_nl, v317_open), "&&") && rt_bool(rt_gt(rt_def(up->v188_depth, "depth"), rt_i(0)), "&&")))) {
        (void)({ wv t425 = w2_warnings; wv t426 = rt_chk(w2_warn(rt_get(&F_pos, v317_open), rt_s(&S279), rt_s(&S280)), "postfix"); std_push(t425, t426); });
      }
      wv v318_index = rt_bind(rt_chk(s214_expr(up), "postfix"), 0);
      (void)rt_chk(s197_expect(up, rt_s(&S99), rt_s(&S281), RT_NULL), "postfix");
      v316_e = ({ wv t427 = v318_index; wv t428 = v316_e; wv t429 = rt_get(&F_pos, v317_open); rt_rec(0, 5, K26, (wv[]){ rt_s(&S190), t427, t428, t429, rt_b(0) }); });
    } else if (rt_cond(rt_chk(s192_at(up, rt_s(&S129)), "postfix"))) {
      (void)rt_chk(s195_next(up), "postfix");
      wv v319_tk = rt_bind(rt_chk(s191_peek(up), "postfix"), 0);
      if (rt_cond(rt_ne(rt_get(&F_t, v319_tk), rt_s(&S137)))) {
        wv v320_message = rt_bind((rt_cond(({ wv t430 = w3_keywords; wv t431 = rt_get(&F_t, v319_tk); rt_chk(w1_contains(t430, t431), "postfix"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v319_tk)), rt_s(&S282) }) : rt_cat(2, (wv[]){ rt_s(&S283), rt_text(rt_chk(w4_describe(v319_tk), "postfix")) })), 0);
        (void)rt_chk(w4_failAtHint(v319_tk, v320_message, rt_s(&S133)), "postfix");
      }
      (void)rt_chk(s195_next(up), "postfix");
      v316_e = ({ wv t432 = rt_get(&F_v, v319_tk); wv t433 = v316_e; wv t434 = rt_get(&F_pos, v319_tk); rt_rec(0, 5, K27, (wv[]){ rt_s(&S191), t432, t433, t434, rt_b(0) }); });
    } else {
      break;
    }
  }
  RT_RET(v316_e);
}

static wv s222_call(struct fr49 *up, wv v321_args) {
  rt_enter("call");
  (void)rt_chk(s195_next(up), "call");
  wv v322_c = rt_bind(rt_chk(s195_next(up), "call"), 0);
  if (rt_cond(rt_ne(rt_chk(s196_accept(up, rt_s(&S129)), "call"), RT_NULL))) {
    wv v323_m = rt_bind(rt_chk(s195_next(up), "call"), 0);
    (void)rt_chk(s195_next(up), "call");
    wv v324_rec = ({ wv t435 = rt_get(&F_v, v322_c); wv t436 = rt_get(&F_pos, v322_c); rt_rec(0, 4, K28, (wv[]){ rt_s(&S213), t435, t436, rt_b(0) }); });
    RT_RET(({ wv t440 = ({ wv t437 = rt_get(&F_v, v323_m); wv t438 = v324_rec; wv t439 = rt_get(&F_pos, v323_m); rt_rec(0, 5, K27, (wv[]){ rt_s(&S191), t437, t438, t439, rt_b(0) }); }); wv t441 = v321_args; wv t442 = rt_get(&F_pos, v323_m); rt_rec(0, 5, K29, (wv[]){ rt_s(&S183), t440, t441, t442, rt_b(0) }); }));
  }
  (void)rt_chk(s195_next(up), "call");
  RT_RET(({ wv t446 = ({ wv t443 = (rt_cond(rt_eq(rt_get(&F_t, v322_c), rt_s(&S137))) ? rt_s(&S213) : rt_s(&S156)); wv t444 = rt_get(&F_v, v322_c); wv t445 = rt_get(&F_pos, v322_c); rt_rec(0, 4, K28, (wv[]){ t443, t444, t445, rt_b(0) }); }); wv t447 = v321_args; wv t448 = rt_get(&F_pos, v322_c); rt_rec(0, 5, K29, (wv[]){ rt_s(&S183), t446, t447, t448, rt_b(0) }); }));
}

static wv s223_primary(struct fr49 *up) {
  rt_enter("primary");
  wv v325_tk = rt_bind(rt_chk(s191_peek(up), "primary"), 0);
  wv v326_at0 = rt_bind(rt_get(&F_pos, v325_tk), 0);
  wv v327_kind = rt_bind(rt_get(&F_t, v325_tk), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v327_kind, rt_s(&S39)), "||") || rt_bool(rt_eq(v327_kind, rt_s(&S176)), "||")))) {
    RT_RET(rt_chk(s229_chain(up), "primary"));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S44)))) {
    RT_RET(rt_chk(s227_parseMatch(up), "primary"));
  }
  if (rt_cond(rt_chk(s194_atCall(up), "primary"))) {
    RT_RET(rt_chk(s222_call(up, rt_arr(0, NULL, 0)), "primary"));
  }
  (void)rt_chk(s195_next(up), "primary");
  if (rt_cond(rt_b(rt_bool(rt_eq(v327_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v327_kind, rt_s(&S148)), "||")))) {
    RT_RET(({ wv t449 = v327_kind; wv t450 = rt_get(&F_v, v325_tk); wv t451 = v326_at0; rt_rec(0, 4, K24, (wv[]){ t449, t450, t451, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S172)))) {
    wv v328_parts = rt_arr(0, NULL, 1);
    wv v329_k = rt_i(0);
    while (rt_cond(({ wv t452 = v329_k; wv t453 = std_length(rt_get(&F_parts, v325_tk)); rt_lt(t452, t453); }))) {
      wv v330_part = rt_bind(({ wv t454 = v329_k; wv t455 = rt_get(&F_parts, v325_tk); rt_idx(t454, t455); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v330_part), rt_s(&S186)))) {
        (void)std_push(v328_parts, v330_part);
      } else {
        (void)({ wv t456 = v328_parts; wv t457 = rt_chk(w4_parser(rt_get(&F_tokens, v330_part), rt_b(1)), "primary"); std_push(t456, t457); });
      }
      v329_k = rt_add(v329_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 4, K30, (wv[]){ rt_s(&S172), v328_parts, v326_at0, rt_b(0) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v327_kind, rt_s(&S49)), "||") || rt_bool(rt_eq(v327_kind, rt_s(&S50)), "||")))) {
    RT_RET(({ wv t458 = rt_eq(v327_kind, rt_s(&S49)); wv t459 = v326_at0; rt_rec(0, 4, K24, (wv[]){ rt_s(&S284), t458, t459, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S51)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S51), v326_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S52)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S52), v326_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S53)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S53), v326_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S137)))) {
    RT_RET(({ wv t460 = rt_get(&F_v, v325_tk); wv t461 = v326_at0; rt_rec(0, 4, K28, (wv[]){ rt_s(&S213), t460, t461, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S156)))) {
    RT_RET(({ wv t462 = rt_get(&F_v, v325_tk); wv t463 = v326_at0; rt_rec(0, 4, K28, (wv[]){ rt_s(&S156), t462, t463, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S97)))) {
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S16)), "primary"))) {
      (void)rt_chk(w4_failAtHint(v325_tk, rt_s(&S285), rt_s(&S286)), "primary");
    }
    wv v331_e = rt_bind(rt_chk(s214_expr(up), "primary"), 1);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S96)), "primary"))) {
      wv v332_args = rt_arr(1, (wv[]){ v331_e }, 1);
      while (rt_cond(rt_ne(rt_chk(s196_accept(up, rt_s(&S96)), "primary"), RT_NULL))) {
        if (rt_cond(rt_chk(s192_at(up, rt_s(&S16)), "primary"))) {
          (void)rt_chk(w4_failAt(rt_chk(s191_peek(up), "primary"), rt_s(&S287)), "primary");
        }
        (void)({ wv t464 = v332_args; wv t465 = rt_chk(s214_expr(up), "primary"); std_push(t464, t465); });
      }
      (void)rt_chk(s197_expect(up, rt_s(&S16), rt_s(&S288), RT_NULL), "primary");
      if (rt_cond(rt_not(rt_chk(s194_atCall(up), "primary")))) {
        (void)rt_chk(w4_failAtHint(v325_tk, rt_s(&S289), rt_s(&S290)), "primary");
      }
      RT_RET(rt_chk(s222_call(up, v332_args), "primary"));
    }
    (void)rt_chk(s197_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "primary");
    rt_set_mem(rt_b(1), &F_paren, v331_e);
    RT_RET(v331_e);
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S98)))) {
    wv v333_items = rt_arr(0, NULL, 1);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S28)), "primary"))) {
      (void)rt_chk(s195_next(up), "primary");
      (void)rt_chk(s197_expect(up, rt_s(&S99), rt_s(&S291), rt_s(&S292)), "primary");
      RT_RET(({ wv t466 = rt_arr(0, NULL, 0); wv t467 = v326_at0; rt_rec(0, 4, K32, (wv[]){ rt_s(&S188), t466, t467, rt_b(0) }); }));
    }
    if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S99)), "primary")))) {
      (void)({ wv t468 = v333_items; wv t469 = rt_chk(s214_expr(up), "primary"); std_push(t468, t469); });
      if (rt_cond(rt_chk(s192_at(up, rt_s(&S28)), "primary"))) {
        RT_RET(({ wv t470 = v325_tk; wv t471 = rt_idx(rt_i(0), v333_items); rt_chk(s226_mapLiteral(up, t470, t471), "primary"); }));
      }
      while (rt_cond(rt_chk(s192_at(up, rt_s(&S96)), "primary"))) {
        wv v334_comma = rt_bind(rt_chk(s195_next(up), "primary"), 0);
        if (rt_cond(rt_chk(s192_at(up, rt_s(&S99)), "primary"))) {
          (void)rt_chk(w4_failAt(v334_comma, rt_s(&S293)), "primary");
        }
        (void)({ wv t472 = v333_items; wv t473 = rt_chk(s214_expr(up), "primary"); std_push(t472, t473); });
      }
    }
    (void)rt_chk(s197_expect(up, rt_s(&S99), rt_s(&S294), RT_NULL), "primary");
    RT_RET(rt_rec(0, 4, K32, (wv[]){ rt_s(&S187), v333_items, v326_at0, rt_b(0) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v327_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v327_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v327_kind, rt_s(&S37)), "||")), "||") || rt_bool(rt_eq(v327_kind, rt_s(&S38)), "||")))) {
    (void)({ wv t474 = v325_tk; wv t475 = rt_cat(3, (wv[]){ rt_s(&S295), rt_text(v327_kind), rt_s(&S113) }); wv t476 = rt_cat(3, (wv[]){ rt_s(&S296), rt_text(v327_kind), rt_s(&S297) }); rt_chk(w4_failAtHint(t474, t475, t476), "primary"); });
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S55)))) {
    (void)rt_chk(w4_failAtHint(v325_tk, rt_s(&S298), rt_s(&S299)), "primary");
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S111)))) {
    (void)({ wv t477 = v325_tk; wv t478 = rt_cat(3, (wv[]){ rt_s(&S295), rt_text(rt_get(&F_v, v325_tk)), rt_s(&S182) }); wv t479 = rt_cat(3, (wv[]){ rt_s(&S300), rt_text(rt_get(&F_v, v325_tk)), rt_s(&S301) }); rt_chk(w4_failAtHint(t477, t478, t479), "primary"); });
  }
  if (rt_cond(rt_eq(v327_kind, rt_s(&S100)))) {
    RT_RET(rt_chk(s224_record(up, v325_tk), "primary"));
  }
  (void)({ wv t480 = v325_tk; wv t481 = rt_cat(2, (wv[]){ rt_s(&S302), rt_text(rt_chk(w4_describe(v325_tk), "primary")) }); rt_chk(w4_failAt(t480, t481), "primary"); });
  RT_RET(RT_NULL);
}

static wv s224_record(struct fr49 *up, wv v335_open) {
  rt_enter("record");
  v335_open = rt_bind(v335_open, 0);
  wv v336_fields = rt_arr(0, NULL, 1);
  wv v337_seen = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S15)), "record")))) {
    while (rt_cond(rt_b(1))) {
      wv v338_key = rt_bind(rt_chk(s191_peek(up), "record"), 0);
      if (rt_cond(rt_ne(rt_get(&F_t, v338_key), rt_s(&S137)))) {
        wv v340_message = rt_bind((rt_cond(({ wv t482 = w3_keywords; wv t483 = rt_get(&F_t, v338_key); rt_chk(w1_contains(t482, t483), "record"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v338_key)), rt_s(&S282) }) : rt_cat(2, (wv[]){ rt_s(&S303), rt_text(rt_chk(w4_describe(v338_key), "record")) })), 0);
        (void)rt_chk(w4_failAtHint(v338_key, v340_message, rt_s(&S304)), "record");
      }
      (void)rt_chk(s195_next(up), "record");
      if (rt_cond(({ wv t484 = v337_seen; wv t485 = rt_get(&F_v, v338_key); rt_chk(w1_contains(t484, t485), "record"); }))) {
        (void)({ wv t486 = v338_key; wv t487 = rt_cat(3, (wv[]){ rt_s(&S305), rt_text(rt_get(&F_v, v338_key)), rt_s(&S306) }); rt_chk(w4_failAt(t486, t487), "record"); });
      }
      (void)({ wv t488 = v337_seen; wv t489 = rt_get(&F_v, v338_key); std_push(t488, t489); });
      if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S28)), "record")))) {
        wv v341_found = rt_bind(rt_chk(s191_peek(up), "record"), 0);
        wv v342_hint = rt_bind((rt_cond(rt_b(rt_bool(rt_chk(s192_at(up, rt_s(&S96)), "record"), "||") || rt_bool(rt_chk(s192_at(up, rt_s(&S15)), "record"), "||"))) ? ({ wv t490 = rt_text(rt_get(&F_v, v338_key)); wv t491 = rt_text(rt_get(&F_v, v338_key)); rt_cat(4, (wv[]){ rt_s(&S307), t490, rt_s(&S29), t491 }); }) : RT_NULL), 0);
        (void)({ wv t494 = v341_found; wv t495 = ({ wv t492 = rt_text(rt_get(&F_v, v338_key)); wv t493 = rt_text(rt_chk(w4_describe(v341_found), "record")); rt_cat(4, (wv[]){ rt_s(&S308), t492, rt_s(&S197), t493 }); }); wv t496 = v342_hint; rt_chk(w4_failAtHint(t494, t495, t496), "record"); });
      }
      (void)rt_chk(s195_next(up), "record");
      (void)({ wv t500 = v336_fields; wv t501 = ({ wv t497 = rt_get(&F_v, v338_key); wv t498 = rt_chk(s214_expr(up), "record"); wv t499 = rt_get(&F_pos, v338_key); rt_rec(0, 3, K33, (wv[]){ t497, t498, t499 }); }); std_push(t500, t501); });
      if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S96)), "record")))) {
        break;
      }
      wv v339_comma = rt_bind(rt_chk(s195_next(up), "record"), 0);
      if (rt_cond(rt_chk(s192_at(up, rt_s(&S15)), "record"))) {
        (void)rt_chk(w4_failAt(v339_comma, rt_s(&S309)), "record");
      }
    }
  }
  (void)rt_chk(s197_expect(up, rt_s(&S15), rt_s(&S310), RT_NULL), "record");
  RT_RET(({ wv t502 = v336_fields; wv t503 = rt_get(&F_pos, v335_open); rt_rec(0, 4, K34, (wv[]){ rt_s(&S189), t502, t503, rt_b(0) }); }));
}

static wv s225_constantKey(struct fr49 *up, wv v343_e) {
  rt_enter("constantKey");
  v343_e = rt_bind(v343_e, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v343_e), rt_s(&S138)))) {
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S311), rt_text(rt_get(&F_v, v343_e)) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v343_e), rt_s(&S284)))) {
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S140), rt_text(rt_get(&F_v, v343_e)) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v343_e), rt_s(&S172)), "&&") && rt_bool(rt_le(std_length(rt_get(&F_parts, v343_e)), rt_i(1)), "&&")), "&&") && rt_bool(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v343_e)), rt_i(0)), "||") || rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v343_e))), rt_s(&S186)), "||")), "&&")))) {
    RT_RET(rt_add(rt_s(&S312), (rt_cond(rt_eq(std_length(rt_get(&F_parts, v343_e)), rt_i(0))) ? rt_s(&S13) : rt_idx(rt_i(0), rt_get(&F_parts, v343_e)))));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v343_e), rt_s(&S148)), "||") || rt_bool(rt_eq(rt_get(&F_k, v343_e), rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v343_e), rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v343_e), rt_s(&S51)), "||")))) {
    (void)rt_chk(w2_failHint(rt_get(&F_pos, v343_e), rt_s(&S313), RT_NULL), "constantKey");
  }
  RT_RET(RT_NULL);
}

static wv s226_mapLiteral(struct fr49 *up, wv v344_open, wv v345_first) {
  rt_enter("mapLiteral");
  v344_open = rt_bind(v344_open, 0);
  v345_first = rt_bind(v345_first, 0);
  wv v346_items = rt_arr(0, NULL, 1);
  wv v347_seen = rt_arr(0, NULL, 1);
  wv v348_key = rt_bind(v345_first, 1);
  while (rt_cond(rt_b(1))) {
    wv v349_c = rt_bind(rt_chk(s225_constantKey(up, v348_key), "mapLiteral"), 0);
    if (rt_cond(rt_ne(v349_c, RT_NULL))) {
      if (rt_cond(rt_chk(w1_contains(v347_seen, v349_c), "mapLiteral"))) {
        (void)rt_chk(w2_failHint(rt_get(&F_pos, v348_key), rt_s(&S314), RT_NULL), "mapLiteral");
      }
      (void)std_push(v347_seen, v349_c);
    }
    (void)rt_chk(s197_expect(up, rt_s(&S28), rt_s(&S315), rt_s(&S316)), "mapLiteral");
    (void)std_push(v346_items, v348_key);
    (void)({ wv t504 = v346_items; wv t505 = rt_chk(s214_expr(up), "mapLiteral"); std_push(t504, t505); });
    if (rt_cond(rt_not(rt_chk(s192_at(up, rt_s(&S96)), "mapLiteral")))) {
      break;
    }
    wv v350_comma = rt_bind(rt_chk(s195_next(up), "mapLiteral"), 0);
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S99)), "mapLiteral"))) {
      (void)rt_chk(w4_failAt(v350_comma, rt_s(&S317)), "mapLiteral");
    }
    v348_key = rt_bind(rt_chk(s214_expr(up), "mapLiteral"), 1);
  }
  (void)rt_chk(s197_expect(up, rt_s(&S99), rt_s(&S318), RT_NULL), "mapLiteral");
  RT_RET(({ wv t506 = v346_items; wv t507 = rt_get(&F_pos, v344_open); rt_rec(0, 4, K32, (wv[]){ rt_s(&S188), t506, t507, rt_b(0) }); }));
}

static wv s227_parseMatch(struct fr49 *up) {
  rt_enter("parseMatch");
  wv v351_kw = rt_bind(rt_chk(s195_next(up), "parseMatch"), 0);
  wv v352_subject = rt_bind(rt_chk(s211_cond(up, rt_s(&S44)), "parseMatch"), 0);
  (void)rt_chk(s197_expect(up, rt_s(&S100), rt_s(&S319), RT_NULL), "parseMatch");
  wv v353_cases = rt_arr(0, NULL, 1);
  wv v354_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s192_at(up, rt_s(&S45)), "parseMatch"))) {
      wv v355_a = rt_bind(rt_chk(s195_next(up), "parseMatch"), 0);
      if (rt_cond(rt_ne(v354_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v355_a, rt_s(&S320)), "parseMatch");
      }
      if (rt_cond(rt_chk(s192_at(up, rt_s(&S100)), "parseMatch"))) {
        (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "parseMatch"), rt_s(&S321), rt_s(&S322)), "parseMatch");
      }
      wv v356_values = rt_arr(0, NULL, 1);
      (void)({ wv t508 = v356_values; wv t509 = rt_chk(s228_caseValue(up), "parseMatch"); std_push(t508, t509); });
      while (rt_cond(rt_ne(rt_chk(s196_accept(up, rt_s(&S96)), "parseMatch"), RT_NULL))) {
        (void)({ wv t510 = v356_values; wv t511 = rt_chk(s228_caseValue(up), "parseMatch"); std_push(t510, t511); });
      }
      (void)({ wv t515 = v353_cases; wv t516 = ({ wv t512 = v356_values; wv t513 = rt_chk(s212_block(up), "parseMatch"); wv t514 = rt_get(&F_pos, v355_a); rt_rec(0, 3, K35, (wv[]){ t512, t513, t514 }); }); std_push(t515, t516); });
    } else if (rt_cond(rt_chk(s192_at(up, rt_s(&S40)), "parseMatch"))) {
      wv v357_a = rt_bind(rt_chk(s195_next(up), "parseMatch"), 0);
      if (rt_cond(rt_ne(v354_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v357_a, rt_s(&S323)), "parseMatch");
      }
      if (rt_cond(rt_eq(std_length(v353_cases), rt_i(0)))) {
        (void)rt_chk(w4_failAt(v357_a, rt_s(&S324)), "parseMatch");
      }
      v354_els = rt_bind(rt_chk(s212_block(up), "parseMatch"), 1);
    } else {
      break;
    }
  }
  if (rt_cond(rt_eq(std_length(v353_cases), rt_i(0)))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s191_peek(up), "parseMatch"), rt_s(&S325), rt_s(&S326)), "parseMatch");
  }
  (void)rt_chk(s197_expect(up, rt_s(&S15), rt_s(&S327), RT_NULL), "parseMatch");
  RT_RET(({ wv t517 = v352_subject; wv t518 = v353_cases; wv t519 = v354_els; wv t520 = rt_get(&F_pos, v351_kw); rt_rec(0, 6, K36, (wv[]){ rt_s(&S44), t517, t518, t519, t520, rt_b(0) }); }));
}

static wv s228_caseValue(struct fr49 *up) {
  rt_enter("caseValue");
  wv v358_tk = rt_bind(rt_chk(s191_peek(up), "caseValue"), 0);
  wv v359_v = rt_bind(rt_chk(s219_unary(up), "caseValue"), 0);
  wv v360_kind = rt_bind(rt_get(&F_k, v359_v), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v360_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v360_kind, rt_s(&S284)), "||")), "||") || rt_bool(rt_eq(v360_kind, rt_s(&S51)), "||")))) {
    RT_RET(v359_v);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v360_kind, rt_s(&S191)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v359_v)), rt_s(&S213)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v359_v)), rt_s(&S191)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, rt_get(&F_rec, v359_v))), rt_s(&S213)), "&&")), "||")), "&&")))) {
    RT_RET(v359_v);
  }
  if (rt_cond(rt_eq(v360_kind, rt_s(&S172)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v359_v)), rt_i(1)), "&&") && rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v359_v))), rt_s(&S186)), "&&")))) {
      RT_RET(v359_v);
    }
    (void)rt_chk(w4_failAt(v358_tk, rt_s(&S328)), "caseValue");
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v360_kind, rt_s(&S148)), "||") || rt_bool(rt_eq(v360_kind, rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(v360_kind, rt_s(&S53)), "||")))) {
    (void)rt_chk(w4_failAtHint(v358_tk, rt_s(&S329), rt_s(&S330)), "caseValue");
  }
  (void)rt_chk(w4_failAtHint(v358_tk, rt_s(&S331), rt_s(&S332)), "caseValue");
  RT_RET(RT_NULL);
}

static wv s229_chain(struct fr49 *up) {
  rt_enter("chain");
  wv v361_branches = rt_arr(0, NULL, 1);
  wv v362_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    wv v363_kw = rt_bind(rt_chk(s195_next(up), "chain"), 0);
    wv v364_c = rt_bind(rt_chk(s211_cond(up, rt_get(&F_t, v363_kw)), "chain"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v363_kw), rt_s(&S176)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v364_c), rt_s(&S193)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v364_c), rt_s(&S77)), "&&")))) {
      (void)({ wv t521 = w2_warnings; wv t522 = rt_chk(w2_warn(rt_get(&F_pos, v363_kw), rt_s(&S333), rt_s(&S334)), "chain"); std_push(t521, t522); });
    }
    (void)({ wv t527 = v361_branches; wv t528 = ({ wv t523 = rt_eq(rt_get(&F_t, v363_kw), rt_s(&S176)); wv t524 = v364_c; wv t525 = rt_chk(s212_block(up), "chain"); wv t526 = rt_get(&F_pos, v363_kw); rt_rec(0, 4, K37, (wv[]){ t523, t524, t525, t526 }); }); std_push(t527, t528); });
    if (rt_cond(rt_eq(rt_chk(s196_accept(up, rt_s(&S40)), "chain"), RT_NULL))) {
      break;
    }
    if (rt_cond(rt_b(rt_bool(rt_chk(s192_at(up, rt_s(&S39)), "chain"), "||") || rt_bool(rt_chk(s192_at(up, rt_s(&S176)), "chain"), "||")))) {
      continue;
    }
    v362_els = rt_bind(rt_chk(s212_block(up), "chain"), 1);
    break;
  }
  RT_RET(({ wv t529 = v361_branches; wv t530 = v362_els; wv t531 = rt_get(&F_pos, rt_idx(rt_i(0), v361_branches)); rt_rec(0, 5, K38, (wv[]){ rt_s(&S185), t529, t530, t531, rt_b(0) }); }));
}

static wv w5_stdSig(wv v367_name) {
  rt_enter("stdSig");
  v367_name = rt_bind(v367_name, 0);
  wv v368_k = rt_i(0);
  while (rt_cond(({ wv t587 = v368_k; wv t588 = std_length(rt_def(w5_stdlib, "stdlib")); rt_lt(t587, t588); }))) {
    if (rt_cond(({ wv t591 = rt_get(&F_name, ({ wv t589 = v368_k; wv t590 = rt_def(w5_stdlib, "stdlib"); rt_idx(t589, t590); })); wv t592 = v367_name; rt_eq(t591, t592); }))) {
      RT_RET(rt_get(&F_arity, ({ wv t593 = v368_k; wv t594 = rt_def(w5_stdlib, "stdlib"); rt_idx(t593, t594); })));
    }
    v368_k = rt_add(v368_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w5_modeOf(wv v369_x) {
  rt_enter("modeOf");
  v369_x = rt_bind(v369_x, 0);
  RT_RET((rt_cond(rt_get(&F_isVar, v369_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v369_x)) ? rt_s(&S37) : rt_s(&S13))));
}

static wv w5_kindText(wv v370_kind) {
  rt_enter("kindText");
  v370_kind = rt_bind(v370_kind, 0);
  RT_RET((rt_cond(rt_eq(v370_kind, rt_s(&S33))) ? rt_s(&S390) : (rt_cond(rt_eq(v370_kind, rt_s(&S46))) ? rt_s(&S391) : rt_cat(2, (wv[]){ rt_s(&S0), rt_text(v370_kind) }))));
}

static wv w5_plural(wv v371_n) {
  rt_enter("plural");
  v371_n = rt_bind(v371_n, 0);
  RT_RET(({ wv t595 = rt_cat(2, (wv[]){ rt_text(v371_n), rt_s(&S392) }); wv t596 = (rt_cond(rt_eq(v371_n, rt_i(1))) ? rt_s(&S13) : rt_s(&S312)); rt_add(t595, t596); }));
}

static wv w5_enumText(wv v372_x) {
  rt_enter("enumText");
  v372_x = rt_bind(v372_x, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v372_x), rt_s(&S213))) ? rt_get(&F_name, v372_x) : ({ wv t597 = rt_text(rt_get(&F_name, rt_get(&F_rec, v372_x))); wv t598 = rt_text(rt_get(&F_field, v372_x)); rt_cat(3, (wv[]){ t597, rt_s(&S129), t598 }); })));
}

static wv w5_caseKey(wv v373_v) {
  rt_enter("caseKey");
  v373_v = rt_bind(v373_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v373_v), rt_s(&S172)))) {
    RT_RET(rt_add(rt_s(&S393), rt_idx(rt_i(0), rt_get(&F_parts, v373_v))));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v373_v), rt_s(&S191)))) {
    RT_RET(({ wv t599 = rt_text(rt_get(&F_field, v373_v)); wv t600 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v373_v)), "caseKey")); rt_cat(4, (wv[]){ rt_s(&S394), t599, rt_s(&S129), t600 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v373_v), rt_s(&S51)))) {
    RT_RET(rt_s(&S51));
  }
  RT_RET(({ wv t601 = rt_text(rt_get(&F_k, v373_v)); wv t602 = rt_text(rt_get(&F_v, v373_v)); rt_cat(3, (wv[]){ t601, rt_s(&S28), t602 }); }));
}

static wv w5_caseText(wv v374_v) {
  rt_enter("caseText");
  v374_v = rt_bind(v374_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v374_v), rt_s(&S172)))) {
    RT_RET(rt_chk(w1_quote(rt_idx(rt_i(0), rt_get(&F_parts, v374_v))), "caseText"));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v374_v), rt_s(&S191)))) {
    RT_RET(({ wv t603 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v374_v)), "caseText")); wv t604 = rt_text(rt_get(&F_field, v374_v)); rt_cat(3, (wv[]){ t603, rt_s(&S129), t604 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v374_v), rt_s(&S51)))) {
    RT_RET(rt_s(&S51));
  }
  RT_RET(rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v374_v)) }));
}

static wv w5_getsAwayWithValue(wv v375_list) {
  rt_enter("getsAwayWithValue");
  wv v376_i = rt_i(0);
  while (rt_cond(({ wv t605 = v376_i; wv t606 = std_length(v375_list); rt_lt(t605, t606); }))) {
    wv v377_s = rt_bind(rt_idx(v376_i, v375_list), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_handler, v377_s), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v377_s)))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v377_s), rt_s(&S34)), "&&") && rt_bool(rt_ne(rt_get(&F_e, v377_s), RT_NULL), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v377_s), rt_s(&S41)), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v377_s))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v377_s), rt_s(&S177)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v377_s)), rt_s(&S185)), "&&")))) {
      wv v378_b = rt_i(0);
      while (rt_cond(({ wv t607 = v378_b; wv t608 = std_length(rt_get(&F_branches, rt_get(&F_e, v377_s))); rt_lt(t607, t608); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t609 = v378_b; wv t610 = rt_get(&F_branches, rt_get(&F_e, v377_s)); rt_idx(t609, t610); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v378_b = rt_add(v378_b, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v377_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v377_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v377_s), rt_s(&S177)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v377_s)), rt_s(&S44)), "&&")))) {
      wv v379_a = rt_i(0);
      while (rt_cond(({ wv t611 = v379_a; wv t612 = std_length(rt_get(&F_cases, rt_get(&F_e, v377_s))); rt_lt(t611, t612); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t613 = v379_a; wv t614 = rt_get(&F_cases, rt_get(&F_e, v377_s)); rt_idx(t613, t614); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v379_a = rt_add(v379_a, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v377_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v377_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    v376_i = rt_add(v376_i, rt_i(1));
  }
  RT_RET(rt_b(0));
}

static wv w5_lineIn(wv v380_chars, wv v381_pos) {
  rt_enter("lineIn");
  v380_chars = rt_bind(v380_chars, 0);
  v381_pos = rt_bind(v381_pos, 0);
  wv v382_line = rt_i(1);
  wv v383_k = rt_i(0);
  while (rt_cond(rt_lt(v383_k, v381_pos))) {
    if (rt_cond(rt_eq(rt_idx(v383_k, v380_chars), rt_s(&S2)))) {
      v382_line = rt_add(v382_line, rt_i(1));
    }
    v383_k = rt_add(v383_k, rt_i(1));
  }
  RT_RET(v382_line);
}

static wv w5_lineOf(wv v384_pos) {
  rt_enter("lineOf");
  v384_pos = rt_bind(v384_pos, 0);
  RT_RET(rt_chk(w5_lineIn(w2_srcChars, v384_pos), "lineOf"));
}

static wv w5_check(wv p_v385_modules, wv v386_requireExit) {
  rt_enter("check");
  struct fr100 fr = { 0 };
  fr.v385_modules = rt_bind(p_v385_modules, 0);
  v386_requireExit = rt_bind(v386_requireExit, 0);
  fr.v387_errors = rt_arr(0, NULL, 1);
  fr.v388_nextFn = rt_i(1);
  fr.v389_curMod = rt_i(0);
  fr.v390_functions = rt_arr(0, NULL, 1);
  fr.v391_uses = rt_arr(0, NULL, 1);
  fr.v392_enums = rt_arr(0, NULL, 1);
  fr.v425_topFn = rt_rec(0, 2, K44, (wv[]){ rt_i(0), rt_b(1) });
  wv v426_entry = rt_sub(std_length(fr.v385_modules), rt_i(1));
  wv v427_mi = rt_i(0);
  while (rt_cond(({ wv t1140 = v427_mi; wv t1141 = std_length(fr.v385_modules); rt_lt(t1140, t1141); }))) {
    (void)({ wv t1142 = v427_mi; wv t1143 = rt_eq(v427_mi, v426_entry); rt_chk(s424_checkModule(&fr, t1142, t1143), "check"); });
    v427_mi = rt_add(v427_mi, rt_i(1));
  }
  wv v428_si = rt_i(0);
  while (rt_cond(({ wv t1144 = v428_si; wv t1145 = std_length(fr.v390_functions); rt_lt(t1144, t1145); }))) {
    wv v618_sc = rt_bind(rt_idx(v428_si, fr.v390_functions), 0);
    wv v619_use = rt_bind(({ wv t1146 = rt_get(&F_mod, v618_sc); wv t1147 = rt_get(&F_namePos, v618_sc); rt_chk(s396_firstUse(&fr, t1146, t1147), "check"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v619_use, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v618_sc)), RT_NULL), "&&")), "&&") && rt_bool(rt_not(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v618_sc))), "check")), "&&")))) {
      (void)rt_chk(s394_enter(&fr, rt_get(&F_mod, v618_sc)), "check");
      wv v620_um = rt_bind(rt_get(&F_useMod, v619_use), 0);
      wv v621_where = rt_bind((rt_cond(({ wv t1148 = v620_um; wv t1149 = rt_get(&F_mod, v618_sc); rt_eq(t1148, t1149); })) ? rt_cat(2, (wv[]){ rt_s(&S482), rt_text(rt_chk(w5_lineOf(rt_get(&F_pos, v619_use)), "check")) }) : ({ wv t1152 = rt_text(rt_get(&F_file, rt_idx(v620_um, fr.v385_modules))); wv t1153 = rt_text(({ wv t1150 = rt_get(&F_chars, rt_idx(v620_um, fr.v385_modules)); wv t1151 = rt_get(&F_pos, v619_use); rt_chk(w5_lineIn(t1150, t1151), "check"); })); rt_cat(3, (wv[]){ t1152, rt_s(&S483), t1153 }); })), 0);
      (void)({ wv t1156 = rt_get(&F_namePos, v618_sc); wv t1157 = ({ wv t1154 = rt_text(rt_get(&F_name, v618_sc)); wv t1155 = rt_text(v621_where); rt_cat(5, (wv[]){ rt_s(&S113), t1154, rt_s(&S484), t1155, rt_s(&S16) }); }); rt_chk(s393_error(&fr, t1156, t1157, rt_s(&S485)), "check"); });
    }
    v428_si = rt_add(v428_si, rt_i(1));
  }
  (void)rt_chk(s394_enter(&fr, v426_entry), "check");
  wv v429_program = rt_bind(rt_get(&F_program, rt_idx(v426_entry, fr.v385_modules)), 0);
  if (rt_cond(v386_requireExit)) {
    wv v622_last = RT_NULL;
    wv v623_k = rt_i(0);
    while (rt_cond(({ wv t1158 = v623_k; wv t1159 = std_length(rt_get(&F_stmts, v429_program)); rt_lt(t1158, t1159); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1160 = v623_k; wv t1161 = rt_get(&F_stmts, v429_program); rt_idx(t1160, t1161); })), rt_s(&S202)))) {
        v622_last = rt_bind(({ wv t1162 = v623_k; wv t1163 = rt_get(&F_stmts, v429_program); rt_idx(t1162, t1163); }), 1);
      }
      v623_k = rt_add(v623_k, rt_i(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(v622_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v622_last), rt_s(&S54)), "||")))) {
      (void)rt_chk(s393_error(&fr, rt_get(&F_end, v429_program), rt_s(&S486), rt_s(&S487)), "check");
    }
  }
  RT_RET(fr.v387_errors);
}

static wv s393_error(struct fr100 *up, wv v430_pos, wv v431_message, wv v432_hint) {
  rt_enter("error");
  v430_pos = rt_bind(v430_pos, 0);
  v431_message = rt_bind(v431_message, 0);
  v432_hint = rt_bind(v432_hint, 0);
  (void)({ wv t615 = rt_def(up->v387_errors, "errors"); wv t616 = rt_chk(w2_err(v430_pos, v431_message, v432_hint), "error"); std_push(t615, t616); });
  RT_RET(RT_NULL);
}

static wv s394_enter(struct fr100 *up, wv v433_m) {
  rt_enter("enter");
  v433_m = rt_bind(v433_m, 0);
  wv t617 = rt_bind(v433_m, 1);
  (void)rt_def(up->v389_curMod, "curMod");
  up->v389_curMod = t617;
  (void)({ wv t618 = rt_get(&F_file, rt_idx(v433_m, up->v385_modules)); wv t619 = rt_get(&F_chars, rt_idx(v433_m, up->v385_modules)); rt_chk(w2_setSource(t618, t619), "enter"); });
  RT_RET(RT_NULL);
}

static wv s395_enumMembers(struct fr100 *up, wv v434_d) {
  rt_enter("enumMembers");
  v434_d = rt_bind(v434_d, 0);
  wv v435_k = rt_i(0);
  while (rt_cond(({ wv t620 = v435_k; wv t621 = std_length(rt_def(up->v392_enums, "enums")); rt_lt(t620, t621); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t624 = rt_get(&F_mod, ({ wv t622 = v435_k; wv t623 = rt_def(up->v392_enums, "enums"); rt_idx(t622, t623); })); wv t625 = rt_get(&F_mod, v434_d); rt_eq(t624, t625); }), "&&") && rt_bool(({ wv t628 = rt_get(&F_name, ({ wv t626 = v435_k; wv t627 = rt_def(up->v392_enums, "enums"); rt_idx(t626, t627); })); wv t629 = rt_get(&F_name, v434_d); rt_eq(t628, t629); }), "&&")))) {
      RT_RET(rt_get(&F_members, ({ wv t630 = v435_k; wv t631 = rt_def(up->v392_enums, "enums"); rt_idx(t630, t631); })));
    }
    v435_k = rt_add(v435_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s396_firstUse(struct fr100 *up, wv v436_mod, wv v437_declPos) {
  rt_enter("firstUse");
  v436_mod = rt_bind(v436_mod, 0);
  v437_declPos = rt_bind(v437_declPos, 0);
  wv v438_k = rt_i(0);
  while (rt_cond(({ wv t632 = v438_k; wv t633 = std_length(rt_def(up->v391_uses, "uses")); rt_lt(t632, t633); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t636 = rt_get(&F_mod, ({ wv t634 = v438_k; wv t635 = rt_def(up->v391_uses, "uses"); rt_idx(t634, t635); })); wv t637 = v436_mod; rt_eq(t636, t637); }), "&&") && rt_bool(({ wv t640 = rt_get(&F_namePos, ({ wv t638 = v438_k; wv t639 = rt_def(up->v391_uses, "uses"); rt_idx(t638, t639); })); wv t641 = v437_declPos; rt_eq(t640, t641); }), "&&")))) {
      RT_RET(({ wv t642 = v438_k; wv t643 = rt_def(up->v391_uses, "uses"); rt_idx(t642, t643); }));
    }
    v438_k = rt_add(v438_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s397_requireValue(struct fr100 *up, wv v439_b) {
  rt_enter("requireValue");
  v439_b = rt_bind(v439_b, 0);
  if (rt_cond(rt_ne(rt_get(&F_value, v439_b), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v440_last = RT_NULL;
  wv v441_k = rt_i(0);
  while (rt_cond(({ wv t644 = v441_k; wv t645 = std_length(rt_get(&F_stmts, v439_b)); rt_lt(t644, t645); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t646 = v441_k; wv t647 = rt_get(&F_stmts, v439_b); rt_idx(t646, t647); })), rt_s(&S202)))) {
      v440_last = rt_bind(({ wv t648 = v441_k; wv t649 = rt_get(&F_stmts, v439_b); rt_idx(t648, t649); }), 1);
    }
    v441_k = rt_add(v441_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_ne(v440_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t650 = rt_def(w5_neverCompletes, "neverCompletes"); wv t651 = rt_get(&F_k, v440_last); rt_chk(w1_contains(t650, t651), "requireValue"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v440_last), rt_s(&S177)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v440_last)), rt_s(&S184)), "&&")), "||")), "&&")))) {
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s393_error(up, rt_get(&F_close, v439_b), rt_s(&S395), rt_s(&S396)), "requireValue");
  RT_RET(RT_NULL);
}

static wv s398_newScope(struct fr100 *up, wv v442_parent) {
  rt_enter("newScope");
  v442_parent = rt_bind(v442_parent, 0);
  wv v443_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K40, (wv[]){ v442_parent, v443_names }));
}

static wv s399_decl(struct fr100 *up, wv v444_kind, wv v445_name, wv v446_mode, wv v447_namePos, wv v448_visibleAt, wv v449_fnId, wv v450_arity) {
  rt_enter("decl");
  v444_kind = rt_bind(v444_kind, 0);
  v445_name = rt_bind(v445_name, 0);
  v446_mode = rt_bind(v446_mode, 0);
  v447_namePos = rt_bind(v447_namePos, 0);
  v448_visibleAt = rt_bind(v448_visibleAt, 0);
  v449_fnId = rt_bind(v449_fnId, 0);
  v450_arity = rt_bind(v450_arity, 0);
  RT_RET(({ wv t652 = v444_kind; wv t653 = v445_name; wv t654 = rt_eq(v446_mode, rt_s(&S36)); wv t655 = rt_eq(v446_mode, rt_s(&S37)); wv t656 = v447_namePos; wv t657 = v448_visibleAt; wv t658 = v449_fnId; wv t659 = v450_arity; wv t660 = rt_def(up->v389_curMod, "curMod"); rt_rec(0, 10, K41, (wv[]){ t652, t653, t654, t655, t656, t657, t658, t659, t660, RT_NULL }); }));
}

static wv s400_own(struct fr100 *up, wv v451_scope, wv v452_name) {
  rt_enter("own");
  v452_name = rt_bind(v452_name, 0);
  wv v453_k = rt_i(0);
  while (rt_cond(({ wv t661 = v453_k; wv t662 = std_length(rt_get(&F_names, v451_scope)); rt_lt(t661, t662); }))) {
    if (rt_cond(({ wv t665 = rt_get(&F_name, ({ wv t663 = v453_k; wv t664 = rt_get(&F_names, v451_scope); rt_idx(t663, t664); })); wv t666 = v452_name; rt_eq(t665, t666); }))) {
      RT_RET(({ wv t667 = v453_k; wv t668 = rt_get(&F_names, v451_scope); rt_idx(t667, t668); }));
    }
    v453_k = rt_add(v453_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s401_declare(struct fr100 *up, wv v454_scope, wv v455_name, wv v456_d) {
  rt_enter("declare");
  v455_name = rt_bind(v455_name, 0);
  v456_d = rt_bind(v456_d, 0);
  wv v457_dup = rt_bind(rt_chk(s400_own(up, v454_scope, v455_name), "declare"), 0);
  if (rt_cond(rt_ne(v457_dup, RT_NULL))) {
    (void)({ wv t671 = rt_get(&F_namePos, v456_d); wv t672 = ({ wv t669 = rt_text(v455_name); wv t670 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v457_dup)), "declare")); rt_cat(5, (wv[]){ rt_s(&S113), t669, rt_s(&S397), t670, rt_s(&S16) }); }); rt_chk(s393_error(up, t671, t672, RT_NULL), "declare"); });
    RT_RET(RT_NULL);
  }
  wv v458_s = rt_get(&F_parent, v454_scope);
  while (rt_cond(rt_ne(v458_s, RT_NULL))) {
    wv v459_outer = rt_bind(rt_chk(s400_own(up, v458_s, v455_name), "declare"), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v459_outer, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_kind, v459_outer), rt_s(&S178)), "||") || rt_bool(({ wv t673 = rt_get(&F_namePos, v459_outer); wv t674 = rt_get(&F_namePos, v456_d); rt_lt(t673, t674); }), "||")), "&&")))) {
      (void)({ wv t678 = rt_get(&F_namePos, v456_d); wv t679 = ({ wv t675 = rt_text(v455_name); wv t676 = rt_text(v455_name); wv t677 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v459_outer)), "declare")); rt_cat(7, (wv[]){ rt_s(&S113), t675, rt_s(&S398), t676, rt_s(&S399), t677, rt_s(&S16) }); }); rt_chk(s393_error(up, t678, t679, rt_s(&S400)), "declare"); });
      break;
    }
    v458_s = rt_get(&F_parent, v458_s);
  }
  (void)({ wv t680 = rt_get(&F_names, v454_scope); wv t681 = v456_d; std_push(t680, t681); });
  RT_RET(RT_NULL);
}

static wv s402_lookup(struct fr100 *up, wv v460_scope, wv v461_name) {
  rt_enter("lookup");
  v461_name = rt_bind(v461_name, 0);
  wv v462_s = v460_scope;
  while (rt_cond(rt_ne(v462_s, RT_NULL))) {
    wv v463_d = rt_bind(rt_chk(s400_own(up, v462_s, v461_name), "lookup"), 0);
    if (rt_cond(rt_ne(v463_d, RT_NULL))) {
      RT_RET(v463_d);
    }
    v462_s = rt_get(&F_parent, v462_s);
  }
  RT_RET(RT_NULL);
}

static wv s403_resolve(struct fr100 *up, wv v464_name, wv v465_pos, wv v466_scope, wv v467_ctx) {
  rt_enter("resolve");
  v464_name = rt_bind(v464_name, 0);
  v465_pos = rt_bind(v465_pos, 0);
  v466_scope = rt_bind(v466_scope, 0);
  v467_ctx = rt_bind(v467_ctx, 0);
  wv v468_d = rt_bind(rt_chk(s402_lookup(up, v466_scope, v464_name), "resolve"), 0);
  if (rt_cond(rt_eq(v468_d, RT_NULL))) {
    (void)({ wv t682 = v465_pos; wv t683 = rt_cat(3, (wv[]){ rt_s(&S401), rt_text(v464_name), rt_s(&S113) }); rt_chk(s393_error(up, t682, t683, RT_NULL), "resolve"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v468_d), rt_s(&S178)), "&&") && rt_bool(({ wv t684 = rt_get(&F_fnId, v468_d); wv t685 = rt_get(&F_id, rt_get(&F_func, v467_ctx)); rt_eq(t684, t685); }), "&&")), "&&") && rt_bool(({ wv t686 = v465_pos; wv t687 = rt_get(&F_visibleAt, v468_d); rt_lt(t686, t687); }), "&&")))) {
    (void)({ wv t690 = v465_pos; wv t691 = ({ wv t688 = rt_text(v464_name); wv t689 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v468_d)), "resolve")); rt_cat(5, (wv[]){ rt_s(&S113), t688, rt_s(&S402), t689, rt_s(&S16) }); }); rt_chk(s393_error(up, t690, t691, RT_NULL), "resolve"); });
  }
  RT_RET(v468_d);
}

static wv s404_block(struct fr100 *up, wv v469_b, wv v470_scope, wv v471_ctx) {
  rt_enter("block");
  v470_scope = rt_bind(v470_scope, 0);
  v471_ctx = rt_bind(v471_ctx, 0);
  wv v472_k = rt_i(0);
  while (rt_cond(({ wv t692 = v472_k; wv t693 = std_length(rt_get(&F_stmts, v469_b)); rt_lt(t692, t693); }))) {
    wv v473_s = rt_bind(({ wv t694 = v472_k; wv t695 = rt_get(&F_stmts, v469_b); rt_idx(t694, t695); }), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v473_s), rt_s(&S33)))) {
      wv v474_d = rt_bind(({ wv t696 = rt_get(&F_name, v473_s); wv t697 = rt_get(&F_namePos, v473_s); wv t698 = rt_get(&F_id, rt_get(&F_func, v471_ctx)); wv t699 = std_length(rt_get(&F_params, v473_s)); rt_chk(s399_decl(up, rt_s(&S33), t696, rt_s(&S13), t697, RT_NULL, t698, t699), "block"); }), 0);
      (void)({ wv t700 = v470_scope; wv t701 = rt_get(&F_name, v473_s); wv t702 = v474_d; rt_chk(s401_declare(up, t700, t701, t702), "block"); });
      if (rt_cond(({ wv t705 = ({ wv t703 = v470_scope; wv t704 = rt_get(&F_name, v473_s); rt_chk(s400_own(up, t703, t704), "block"); }); wv t706 = v474_d; rt_eq(t705, t706); }))) {
        (void)({ wv t711 = rt_def(up->v390_functions, "functions"); wv t712 = ({ wv t707 = rt_def(up->v389_curMod, "curMod"); wv t708 = rt_get(&F_name, v473_s); wv t709 = rt_get(&F_namePos, v473_s); wv t710 = rt_get(&F_body, v473_s); rt_rec(0, 4, K42, (wv[]){ t707, t708, t709, t710 }); }); std_push(t711, t712); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v473_s), rt_s(&S46)))) {
      wv v475_d = rt_bind(({ wv t713 = rt_get(&F_name, v473_s); wv t714 = rt_get(&F_namePos, v473_s); wv t715 = rt_get(&F_id, rt_get(&F_func, v471_ctx)); rt_chk(s399_decl(up, rt_s(&S46), t713, rt_s(&S13), t714, RT_NULL, t715, RT_NULL), "block"); }), 0);
      (void)({ wv t716 = v470_scope; wv t717 = rt_get(&F_name, v473_s); wv t718 = v475_d; rt_chk(s401_declare(up, t716, t717, t718), "block"); });
      if (rt_cond(({ wv t721 = ({ wv t719 = v470_scope; wv t720 = rt_get(&F_name, v473_s); rt_chk(s400_own(up, t719, t720), "block"); }); wv t722 = v475_d; rt_eq(t721, t722); }))) {
        (void)({ wv t726 = rt_def(up->v392_enums, "enums"); wv t727 = ({ wv t723 = rt_def(up->v389_curMod, "curMod"); wv t724 = rt_get(&F_name, v473_s); wv t725 = rt_get(&F_members, v473_s); rt_rec(0, 3, K43, (wv[]){ t723, t724, t725 }); }); std_push(t726, t727); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v473_s), rt_s(&S178)))) {
      (void)({ wv t733 = v470_scope; wv t734 = rt_get(&F_name, v473_s); wv t735 = ({ wv t728 = rt_get(&F_name, v473_s); wv t729 = rt_chk(w5_modeOf(v473_s), "block"); wv t730 = rt_get(&F_namePos, v473_s); wv t731 = rt_get(&F_end, v473_s); wv t732 = rt_get(&F_id, rt_get(&F_func, v471_ctx)); rt_chk(s399_decl(up, rt_s(&S178), t728, t729, t730, t731, t732, RT_NULL), "block"); }); rt_chk(s401_declare(up, t733, t734, t735), "block"); });
    }
    v472_k = rt_add(v472_k, rt_i(1));
  }
  v472_k = rt_i(0);
  while (rt_cond(({ wv t736 = v472_k; wv t737 = std_length(rt_get(&F_stmts, v469_b)); rt_lt(t736, t737); }))) {
    (void)({ wv t740 = ({ wv t738 = v472_k; wv t739 = rt_get(&F_stmts, v469_b); rt_idx(t738, t739); }); wv t741 = v470_scope; wv t742 = v471_ctx; rt_chk(s405_stmt(up, t740, t741, t742), "block"); });
    v472_k = rt_add(v472_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v469_b), RT_NULL))) {
    (void)({ wv t743 = rt_get(&F_value, v469_b); wv t744 = v470_scope; wv t745 = v471_ctx; rt_chk(s420_expr(up, t743, t744, t745), "block"); });
  }
  RT_RET(RT_NULL);
}

static wv s405_stmt(struct fr100 *up, wv v476_s, wv v477_scope, wv v478_ctx) {
  rt_enter("stmt");
  v477_scope = rt_bind(v477_scope, 0);
  v478_ctx = rt_bind(v478_ctx, 0);
  wv v479_kind = rt_bind(rt_get(&F_k, v476_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v479_kind, rt_s(&S202)), "||") || rt_bool(rt_eq(v479_kind, rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(v479_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v479_kind, rt_s(&S205)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v479_kind, rt_s(&S33)))) {
    wv v480_func = rt_rec(0, 2, K44, (wv[]){ rt_def(up->v388_nextFn, "nextFn"), rt_b(0) });
    up->v388_nextFn = rt_add(rt_def(up->v388_nextFn, "nextFn"), rt_i(1));
    wv v481_ps = rt_bind(rt_chk(s398_newScope(up, v477_scope), "stmt"), 0);
    wv v482_k = rt_i(0);
    while (rt_cond(({ wv t746 = v482_k; wv t747 = std_length(rt_get(&F_params, v476_s)); rt_lt(t746, t747); }))) {
      wv v483_p = rt_bind(({ wv t748 = v482_k; wv t749 = rt_get(&F_params, v476_s); rt_idx(t748, t749); }), 0);
      (void)({ wv t754 = v481_ps; wv t755 = rt_get(&F_name, v483_p); wv t756 = ({ wv t750 = rt_get(&F_name, v483_p); wv t751 = rt_chk(w5_modeOf(v483_p), "stmt"); wv t752 = rt_get(&F_pos, v483_p); wv t753 = rt_get(&F_id, v480_func); rt_chk(s399_decl(up, rt_s(&S403), t750, t751, t752, RT_NULL, t753, RT_NULL), "stmt"); }); rt_chk(s401_declare(up, t754, t755, t756), "stmt"); });
      v482_k = rt_add(v482_k, rt_i(1));
    }
    (void)({ wv t757 = rt_get(&F_body, v476_s); wv t758 = rt_chk(s398_newScope(up, v481_ps), "stmt"); wv t759 = rt_rec(0, 2, K45, (wv[]){ v480_func, rt_i(0) }); rt_chk(s404_block(up, t757, t758, t759), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v479_kind, rt_s(&S34)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v478_ctx)))) {
      (void)rt_chk(s393_error(up, rt_get(&F_pos, v476_s), rt_s(&S404), RT_NULL), "stmt");
    }
    if (rt_cond(rt_ne(rt_get(&F_e, v476_s), RT_NULL))) {
      (void)({ wv t760 = rt_get(&F_e, v476_s); wv t761 = v477_scope; wv t762 = v478_ctx; rt_chk(s420_expr(up, t760, t761, t762), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v479_kind, rt_s(&S57)))) {
    (void)({ wv t763 = rt_get(&F_e, v476_s); wv t764 = v477_scope; wv t765 = v478_ctx; rt_chk(s420_expr(up, t763, t764, t765), "stmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v479_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v479_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_get(&F_loops, v478_ctx), rt_i(0)))) {
      (void)({ wv t766 = rt_get(&F_pos, v476_s); wv t767 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text((rt_cond(rt_eq(v479_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43))), rt_s(&S405) }); rt_chk(s393_error(up, t766, t767, RT_NULL), "stmt"); });
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v479_kind, rt_s(&S41)))) {
    (void)({ wv t768 = rt_get(&F_cond, v476_s); wv t769 = v477_scope; wv t770 = v478_ctx; rt_chk(s420_expr(up, t768, t769, t770), "stmt"); });
    (void)({ wv t773 = rt_get(&F_body, v476_s); wv t774 = rt_chk(s398_newScope(up, v477_scope), "stmt"); wv t775 = ({ wv t771 = rt_get(&F_func, v478_ctx); wv t772 = rt_add(rt_get(&F_loops, v478_ctx), rt_i(1)); rt_rec(0, 2, K45, (wv[]){ t771, t772 }); }); rt_chk(s404_block(up, t773, t774, t775), "stmt"); });
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v479_kind, rt_s(&S55)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v478_ctx)))) {
      (void)rt_chk(s393_error(up, rt_get(&F_pos, v476_s), rt_s(&S406), RT_NULL), "stmt");
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v479_kind, rt_s(&S54)))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v476_s), RT_NULL))) {
      (void)({ wv t776 = rt_get(&F_e, v476_s); wv t777 = v477_scope; wv t778 = v478_ctx; rt_chk(s420_expr(up, t776, t777, t778), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v479_kind, rt_s(&S177)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v476_s)), rt_s(&S184)))) {
      if (rt_cond(rt_get(&F_top, rt_get(&F_func, v478_ctx)))) {
        (void)rt_chk(s393_error(up, rt_get(&F_pos, rt_get(&F_e, v476_s)), rt_s(&S406), RT_NULL), "stmt");
      }
      (void)({ wv t779 = rt_get(&F_arg, rt_get(&F_e, v476_s)); wv t780 = v477_scope; wv t781 = v478_ctx; rt_chk(s420_expr(up, t779, t780, t781), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v476_s)), rt_s(&S185)))) {
      (void)({ wv t782 = rt_get(&F_e, v476_s); wv t783 = v477_scope; wv t784 = v478_ctx; rt_chk(s416_chain(up, t782, t783, t784, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v476_s)), rt_s(&S44)))) {
      (void)({ wv t785 = rt_get(&F_e, v476_s); wv t786 = v477_scope; wv t787 = v478_ctx; rt_chk(s407_checkMatch(up, t785, t786, t787, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v476_s)), rt_s(&S183)))) {
      wv v484_k = rt_i(0);
      while (rt_cond(({ wv t788 = v484_k; wv t789 = std_length(rt_get(&F_args, rt_get(&F_e, v476_s))); rt_lt(t788, t789); }))) {
        (void)({ wv t792 = ({ wv t790 = v484_k; wv t791 = rt_get(&F_args, rt_get(&F_e, v476_s)); rt_idx(t790, t791); }); wv t793 = v477_scope; wv t794 = v478_ctx; rt_chk(s420_expr(up, t792, t793, t794), "stmt"); });
        v484_k = rt_add(v484_k, rt_i(1));
      }
      (void)({ wv t795 = rt_get(&F_e, v476_s); wv t796 = v477_scope; rt_chk(s421_callee(up, t795, t796, rt_b(0)), "stmt"); });
    } else {
      (void)({ wv t797 = rt_get(&F_e, v476_s); wv t798 = v477_scope; wv t799 = v478_ctx; rt_chk(s420_expr(up, t797, t798, t799), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v479_kind, rt_s(&S178)))) {
    (void)({ wv t800 = rt_get(&F_e, v476_s); wv t801 = v477_scope; wv t802 = v478_ctx; rt_chk(s420_expr(up, t800, t801, t802), "stmt"); });
  } else if (rt_cond(rt_eq(v479_kind, rt_s(&S38)))) {
    (void)({ wv t803 = rt_get(&F_e, v476_s); wv t804 = v477_scope; wv t805 = v478_ctx; rt_chk(s420_expr(up, t803, t804, t805), "stmt"); });
    (void)({ wv t806 = rt_get(&F_target, v476_s); wv t807 = v477_scope; wv t808 = v478_ctx; rt_chk(s417_lvalue(up, t806, t807, t808), "stmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v476_s), RT_NULL))) {
    (void)rt_chk(s406_onfailHandler(up, v476_s, v477_scope, v478_ctx), "stmt");
  }
  RT_RET(RT_NULL);
}

static wv s406_onfailHandler(struct fr100 *up, wv v485_s, wv v486_scope, wv v487_ctx) {
  rt_enter("onfailHandler");
  v485_s = rt_bind(v485_s, 0);
  v486_scope = rt_bind(v486_scope, 0);
  v487_ctx = rt_bind(v487_ctx, 0);
  wv v488_h = rt_bind(rt_get(&F_handler, v485_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_e, v485_s), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v485_s)), rt_s(&S183)), "||")))) {
    (void)rt_chk(s393_error(up, rt_get(&F_pos, v488_h), rt_s(&S407), rt_s(&S408)), "onfailHandler");
  }
  wv v489_hs = rt_bind(rt_chk(s398_newScope(up, v486_scope), "onfailHandler"), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v488_h), RT_NULL))) {
    (void)({ wv t812 = v489_hs; wv t813 = rt_get(&F_param, v488_h); wv t814 = ({ wv t809 = rt_get(&F_param, v488_h); wv t810 = rt_get(&F_paramPos, v488_h); wv t811 = rt_get(&F_id, rt_get(&F_func, v487_ctx)); rt_chk(s399_decl(up, rt_s(&S56), t809, rt_s(&S13), t810, RT_NULL, t811, RT_NULL), "onfailHandler"); }); rt_chk(s401_declare(up, t812, t813, t814), "onfailHandler"); });
  }
  (void)({ wv t815 = rt_get(&F_body, v488_h); wv t816 = rt_chk(s398_newScope(up, v489_hs), "onfailHandler"); wv t817 = v487_ctx; rt_chk(s404_block(up, t815, t816, t817), "onfailHandler"); });
  if (rt_cond(({ wv t818 = rt_def(w5_onfailValueUsed, "onfailValueUsed"); wv t819 = rt_get(&F_k, v485_s); rt_chk(w1_contains(t818, t819), "onfailHandler"); }))) {
    (void)rt_chk(s397_requireValue(up, rt_get(&F_body, v488_h)), "onfailHandler");
  }
  RT_RET(RT_NULL);
}

static wv s407_checkMatch(struct fr100 *up, wv v490_e, wv v491_scope, wv v492_ctx, wv v493_stmtLevel) {
  rt_enter("checkMatch");
  v490_e = rt_bind(v490_e, 0);
  v491_scope = rt_bind(v491_scope, 0);
  v492_ctx = rt_bind(v492_ctx, 0);
  v493_stmtLevel = rt_bind(v493_stmtLevel, 0);
  (void)({ wv t820 = rt_get(&F_subject, v490_e); wv t821 = v491_scope; wv t822 = v492_ctx; rt_chk(s420_expr(up, t820, t821, t822), "checkMatch"); });
  wv v494_seen = rt_arr(0, NULL, 1);
  wv v495_sort = RT_NULL;
  wv v496_sortEnum = RT_NULL;
  wv v497_a = rt_i(0);
  while (rt_cond(({ wv t823 = v497_a; wv t824 = std_length(rt_get(&F_cases, v490_e)); rt_lt(t823, t824); }))) {
    wv v498_al = rt_bind(({ wv t825 = v497_a; wv t826 = rt_get(&F_cases, v490_e); rt_idx(t825, t826); }), 0);
    wv v499_j = rt_i(0);
    while (rt_cond(({ wv t827 = v499_j; wv t828 = std_length(rt_get(&F_values, v498_al)); rt_lt(t827, t828); }))) {
      wv v500_v = rt_bind(({ wv t829 = v499_j; wv t830 = rt_get(&F_values, v498_al); rt_idx(t829, t830); }), 0);
      wv v501_vs = rt_s(&S13);
      if (rt_cond(rt_eq(rt_get(&F_k, v500_v), rt_s(&S191)))) {
        wv v505_d = rt_bind(rt_chk(s414_caseEnum(up, v500_v, v491_scope), "checkMatch"), 0);
        v501_vs = rt_bind((rt_cond(rt_ne(v505_d, RT_NULL)) ? rt_chk(w5_enumText(rt_get(&F_rec, v500_v)), "checkMatch") : RT_NULL), 1);
        if (rt_cond(rt_eq(v495_sort, RT_NULL))) {
          v496_sortEnum = rt_bind(v505_d, 1);
        }
      }
      if (rt_cond(rt_eq(v495_sort, RT_NULL))) {
        v495_sort = rt_bind(v501_vs, 1);
      } else if (rt_cond(rt_b(rt_bool(rt_ne(v501_vs, RT_NULL), "&&") && rt_bool(rt_ne(v501_vs, v495_sort), "&&")))) {
        wv v506_message = rt_bind((rt_cond(rt_b(rt_bool(rt_eq(v495_sort, rt_s(&S13)), "||") || rt_bool(rt_eq(v501_vs, rt_s(&S13)), "||"))) ? rt_s(&S409) : ({ wv t831 = rt_text(v495_sort); wv t832 = rt_text(v501_vs); rt_cat(5, (wv[]){ rt_s(&S410), t831, rt_s(&S411), t832, rt_s(&S113) }); })), 0);
        (void)({ wv t833 = rt_get(&F_pos, v500_v); wv t834 = v506_message; rt_chk(s393_error(up, t833, t834, RT_NULL), "checkMatch"); });
      }
      wv v502_key = rt_bind(rt_chk(w5_caseKey(v500_v), "checkMatch"), 0);
      wv v503_first = RT_NULL;
      wv v504_q = rt_i(0);
      while (rt_cond(({ wv t835 = v504_q; wv t836 = std_length(v494_seen); rt_lt(t835, t836); }))) {
        if (rt_cond(({ wv t837 = rt_get(&F_key, rt_idx(v504_q, v494_seen)); wv t838 = v502_key; rt_eq(t837, t838); }))) {
          v503_first = rt_bind(rt_get(&F_pos, rt_idx(v504_q, v494_seen)), 1);
        }
        v504_q = rt_add(v504_q, rt_i(1));
      }
      if (rt_cond(rt_ne(v503_first, RT_NULL))) {
        (void)({ wv t841 = rt_get(&F_pos, v500_v); wv t842 = ({ wv t839 = rt_text(rt_chk(w5_caseText(v500_v), "checkMatch")); wv t840 = rt_text(rt_chk(w5_lineOf(v503_first), "checkMatch")); rt_cat(5, (wv[]){ rt_s(&S412), t839, rt_s(&S413), t840, rt_s(&S16) }); }); rt_chk(s393_error(up, t841, t842, RT_NULL), "checkMatch"); });
      } else {
        (void)({ wv t845 = v494_seen; wv t846 = ({ wv t843 = v502_key; wv t844 = rt_get(&F_pos, v500_v); rt_rec(0, 2, K46, (wv[]){ t843, t844 }); }); std_push(t845, t846); });
      }
      v499_j = rt_add(v499_j, rt_i(1));
    }
    (void)({ wv t847 = rt_get(&F_body, v498_al); wv t848 = rt_chk(s398_newScope(up, v491_scope), "checkMatch"); wv t849 = v492_ctx; rt_chk(s404_block(up, t847, t848, t849), "checkMatch"); });
    if (rt_cond(rt_not(v493_stmtLevel))) {
      (void)rt_chk(s397_requireValue(up, rt_get(&F_body, v498_al)), "checkMatch");
    }
    v497_a = rt_add(v497_a, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v490_e), RT_NULL))) {
    (void)({ wv t850 = rt_get(&F_els, v490_e); wv t851 = rt_chk(s398_newScope(up, v491_scope), "checkMatch"); wv t852 = v492_ctx; rt_chk(s404_block(up, t850, t851, t852), "checkMatch"); });
    if (rt_cond(rt_not(v493_stmtLevel))) {
      (void)rt_chk(s397_requireValue(up, rt_get(&F_els, v490_e)), "checkMatch");
    }
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v495_sort, RT_NULL), "&&") && rt_bool(rt_ne(v495_sort, rt_s(&S13)), "&&")))) {
    wv v507_ms = rt_bind(rt_chk(s395_enumMembers(up, v496_sortEnum), "checkMatch"), 0);
    wv v508_missing = rt_arr(0, NULL, 1);
    wv v509_m = rt_i(0);
    while (rt_cond(({ wv t853 = v509_m; wv t854 = std_length(v507_ms); rt_lt(t853, t854); }))) {
      wv v510_key = ({ wv t855 = rt_text(rt_get(&F_name, rt_idx(v509_m, v507_ms))); wv t856 = rt_text(v495_sort); rt_cat(4, (wv[]){ rt_s(&S394), t855, rt_s(&S129), t856 }); });
      wv v511_handled = rt_b(0);
      wv v512_q = rt_i(0);
      while (rt_cond(({ wv t857 = v512_q; wv t858 = std_length(v494_seen); rt_lt(t857, t858); }))) {
        if (rt_cond(({ wv t859 = rt_get(&F_key, rt_idx(v512_q, v494_seen)); wv t860 = v510_key; rt_eq(t859, t860); }))) {
          v511_handled = rt_b(1);
        }
        v512_q = rt_add(v512_q, rt_i(1));
      }
      if (rt_cond(rt_not(v511_handled))) {
        (void)({ wv t863 = v508_missing; wv t864 = ({ wv t861 = rt_text(v495_sort); wv t862 = rt_text(rt_get(&F_name, rt_idx(v509_m, v507_ms))); rt_cat(5, (wv[]){ rt_s(&S113), t861, rt_s(&S129), t862, rt_s(&S113) }); }); std_push(t863, t864); });
      }
      v509_m = rt_add(v509_m, rt_i(1));
    }
    if (rt_cond(rt_gt(std_length(v508_missing), rt_i(0)))) {
      (void)({ wv t865 = rt_get(&F_pos, v490_e); wv t866 = rt_cat(2, (wv[]){ rt_s(&S414), rt_text(std_join(v508_missing, rt_s(&S415))) }); rt_chk(s393_error(up, t865, t866, rt_s(&S416)), "checkMatch"); });
    }
  } else if (rt_cond(rt_not(v493_stmtLevel))) {
    (void)rt_chk(s393_error(up, rt_get(&F_pos, v490_e), rt_s(&S417), RT_NULL), "checkMatch");
  }
  RT_RET(RT_NULL);
}

static wv s408_moduleNamed(struct fr100 *up, wv v513_x, wv v514_scope) {
  rt_enter("moduleNamed");
  v514_scope = rt_bind(v514_scope, 0);
  if (rt_cond(rt_ne(rt_get(&F_k, v513_x), rt_s(&S213)))) {
    RT_RET(RT_NULL);
  }
  wv v515_d = rt_bind(({ wv t867 = v514_scope; wv t868 = rt_get(&F_name, v513_x); rt_chk(s402_lookup(up, t867, t868), "moduleNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v515_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v515_d), rt_s(&S418)), "&&"))) ? v515_d : RT_NULL));
}

static wv s409_exported(struct fr100 *up, wv v516_lib, wv v517_name) {
  rt_enter("exported");
  v516_lib = rt_bind(v516_lib, 0);
  v517_name = rt_bind(v517_name, 0);
  wv v518_xs = rt_bind(rt_get(&F_exports, rt_idx(v516_lib, up->v385_modules)), 0);
  wv v519_k = rt_i(0);
  while (rt_cond(({ wv t869 = v519_k; wv t870 = std_length(v518_xs); rt_lt(t869, t870); }))) {
    if (rt_cond(({ wv t871 = rt_get(&F_name, rt_idx(v519_k, v518_xs)); wv t872 = v517_name; rt_eq(t871, t872); }))) {
      RT_RET(rt_idx(v519_k, v518_xs));
    }
    v519_k = rt_add(v519_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s410_enumNamed(struct fr100 *up, wv v520_x, wv v521_scope) {
  rt_enter("enumNamed");
  v521_scope = rt_bind(v521_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v520_x), rt_s(&S213)))) {
    wv v524_d = rt_bind(({ wv t873 = v521_scope; wv t874 = rt_get(&F_name, v520_x); rt_chk(s402_lookup(up, t873, t874), "enumNamed"); }), 0);
    RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v524_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v524_d), rt_s(&S46)), "&&"))) ? v524_d : RT_NULL));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v520_x), rt_s(&S191)))) {
    RT_RET(RT_NULL);
  }
  wv v522_m = rt_bind(({ wv t875 = rt_get(&F_rec, v520_x); wv t876 = v521_scope; rt_chk(s408_moduleNamed(up, t875, t876), "enumNamed"); }), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v522_m, RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_lib, v522_m), RT_NULL), "||")))) {
    RT_RET(RT_NULL);
  }
  wv v523_d = rt_bind(({ wv t877 = rt_get(&F_lib, v522_m); wv t878 = rt_get(&F_field, v520_x); rt_chk(s409_exported(up, t877, t878), "enumNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v523_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v523_d), rt_s(&S46)), "&&"))) ? v523_d : RT_NULL));
}

static wv s411_exportOf(struct fr100 *up, wv v525_e, wv v526_m) {
  rt_enter("exportOf");
  v525_e = rt_bind(v525_e, 0);
  v526_m = rt_bind(v526_m, 0);
  if (rt_cond(rt_eq(rt_get(&F_lib, v526_m), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v527_d = rt_bind(({ wv t879 = rt_get(&F_lib, v526_m); wv t880 = rt_get(&F_field, v525_e); rt_chk(s409_exported(up, t879, t880), "exportOf"); }), 0);
  if (rt_cond(rt_eq(v527_d, RT_NULL))) {
    (void)({ wv t885 = rt_get(&F_pos, v525_e); wv t886 = ({ wv t883 = rt_text(rt_get(&F_path, ({ wv t881 = rt_get(&F_lib, v526_m); wv t882 = up->v385_modules; rt_idx(t881, t882); }))); wv t884 = rt_text(rt_get(&F_field, v525_e)); rt_cat(5, (wv[]){ rt_s(&S113), t883, rt_s(&S419), t884, rt_s(&S113) }); }); rt_chk(s393_error(up, t885, t886, RT_NULL), "exportOf"); });
  }
  RT_RET(v527_d);
}

static wv s412_unknownModule(struct fr100 *up, wv v528_x, wv v529_scope) {
  rt_enter("unknownModule");
  v528_x = rt_bind(v528_x, 0);
  v529_scope = rt_bind(v529_scope, 0);
  wv v530_message = rt_bind((rt_cond(rt_ne(({ wv t887 = v529_scope; wv t888 = rt_get(&F_name, v528_x); rt_chk(s402_lookup(up, t887, t888), "unknownModule"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v528_x)), rt_s(&S420) }) : rt_cat(3, (wv[]){ rt_s(&S421), rt_text(rt_get(&F_name, v528_x)), rt_s(&S113) })), 0);
  (void)({ wv t889 = rt_get(&F_pos, v528_x); wv t890 = v530_message; rt_chk(s393_error(up, t889, t890, RT_NULL), "unknownModule"); });
  RT_RET(RT_NULL);
}

static wv s413_qualified(struct fr100 *up, wv v531_e, wv v532_m) {
  rt_enter("qualified");
  v531_e = rt_bind(v531_e, 0);
  v532_m = rt_bind(v532_m, 0);
  wv v533_d = rt_bind(rt_chk(s411_exportOf(up, v531_e, v532_m), "qualified"), 0);
  wv v534_label = ({ wv t891 = rt_text(rt_get(&F_name, rt_get(&F_rec, v531_e))); wv t892 = rt_text(rt_get(&F_field, v531_e)); rt_cat(3, (wv[]){ t891, rt_s(&S129), t892 }); });
  if (rt_cond(rt_b(rt_bool(rt_ne(v533_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v533_d), rt_s(&S33)), "&&")))) {
    (void)({ wv t895 = rt_get(&F_pos, v531_e); wv t896 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v534_label), rt_s(&S422) }); wv t897 = ({ wv t893 = rt_text(v534_label); wv t894 = rt_text(v534_label); rt_cat(5, (wv[]){ rt_s(&S423), t893, rt_s(&S424), t894, rt_s(&S425) }); }); rt_chk(s393_error(up, t895, t896, t897), "qualified"); });
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v533_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v533_d), rt_s(&S46)), "&&")))) {
    (void)({ wv t898 = rt_get(&F_pos, v531_e); wv t899 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v534_label), rt_s(&S426) }); wv t900 = rt_cat(3, (wv[]){ rt_s(&S427), rt_text(v534_label), rt_s(&S428) }); rt_chk(s393_error(up, t898, t899, t900), "qualified"); });
  }
  RT_RET(RT_NULL);
}

static wv s414_caseEnum(struct fr100 *up, wv v535_v, wv v536_scope) {
  rt_enter("caseEnum");
  v535_v = rt_bind(v535_v, 0);
  v536_scope = rt_bind(v536_scope, 0);
  wv v537_x = rt_bind(rt_get(&F_rec, v535_v), 0);
  wv v538_d = rt_bind(rt_chk(s410_enumNamed(up, v537_x, v536_scope), "caseEnum"), 0);
  if (rt_cond(rt_ne(v538_d, RT_NULL))) {
    (void)rt_chk(s415_member(up, v535_v, v538_d), "caseEnum");
    RT_RET(v538_d);
  }
  wv v539_hint = rt_s(&S429);
  if (rt_cond(rt_eq(rt_get(&F_k, v537_x), rt_s(&S213)))) {
    wv v541_message = rt_bind((rt_cond(rt_ne(({ wv t901 = v536_scope; wv t902 = rt_get(&F_name, v537_x); rt_chk(s402_lookup(up, t901, t902), "caseEnum"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v537_x)), rt_s(&S430) }) : rt_cat(3, (wv[]){ rt_s(&S431), rt_text(rt_get(&F_name, v537_x)), rt_s(&S113) })), 0);
    (void)({ wv t903 = rt_get(&F_pos, v537_x); wv t904 = v541_message; wv t905 = v539_hint; rt_chk(s393_error(up, t903, t904, t905), "caseEnum"); });
    RT_RET(RT_NULL);
  }
  wv v540_m = rt_bind(({ wv t906 = rt_get(&F_rec, v537_x); wv t907 = v536_scope; rt_chk(s408_moduleNamed(up, t906, t907), "caseEnum"); }), 0);
  if (rt_cond(rt_eq(v540_m, RT_NULL))) {
    (void)({ wv t908 = rt_get(&F_rec, v537_x); wv t909 = v536_scope; rt_chk(s412_unknownModule(up, t908, t909), "caseEnum"); });
  } else if (rt_cond(rt_ne(rt_chk(s411_exportOf(up, v537_x, v540_m), "caseEnum"), RT_NULL))) {
    (void)({ wv t910 = rt_get(&F_pos, v537_x); wv t911 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_chk(w5_enumText(v537_x), "caseEnum")), rt_s(&S430) }); wv t912 = v539_hint; rt_chk(s393_error(up, t910, t911, t912), "caseEnum"); });
  }
  RT_RET(RT_NULL);
}

static wv s415_member(struct fr100 *up, wv v542_e, wv v543_d) {
  rt_enter("member");
  v542_e = rt_bind(v542_e, 0);
  v543_d = rt_bind(v543_d, 0);
  wv v544_ms = rt_bind(rt_chk(s395_enumMembers(up, v543_d), "member"), 0);
  wv v545_names = rt_arr(0, NULL, 1);
  wv v546_found = rt_b(0);
  wv v547_k = rt_i(0);
  while (rt_cond(({ wv t913 = v547_k; wv t914 = std_length(v544_ms); rt_lt(t913, t914); }))) {
    (void)({ wv t915 = v545_names; wv t916 = rt_get(&F_name, rt_idx(v547_k, v544_ms)); std_push(t915, t916); });
    if (rt_cond(({ wv t917 = rt_get(&F_name, rt_idx(v547_k, v544_ms)); wv t918 = rt_get(&F_field, v542_e); rt_eq(t917, t918); }))) {
      v546_found = rt_b(1);
    }
    v547_k = rt_add(v547_k, rt_i(1));
  }
  if (rt_cond(rt_not(v546_found))) {
    (void)({ wv t921 = rt_get(&F_pos, v542_e); wv t922 = ({ wv t919 = rt_text(rt_get(&F_field, v542_e)); wv t920 = rt_text(rt_get(&F_name, v543_d)); rt_cat(5, (wv[]){ rt_s(&S432), t919, rt_s(&S433), t920, rt_s(&S113) }); }); wv t923 = rt_cat(2, (wv[]){ rt_s(&S434), rt_text(std_join(v545_names, rt_s(&S415))) }); rt_chk(s393_error(up, t921, t922, t923), "member"); });
  }
  RT_RET(RT_NULL);
}

static wv s416_chain(struct fr100 *up, wv v548_e, wv v549_scope, wv v550_ctx, wv v551_stmtLevel) {
  rt_enter("chain");
  v548_e = rt_bind(v548_e, 0);
  v549_scope = rt_bind(v549_scope, 0);
  v550_ctx = rt_bind(v550_ctx, 0);
  v551_stmtLevel = rt_bind(v551_stmtLevel, 0);
  wv v552_k = rt_i(0);
  while (rt_cond(({ wv t924 = v552_k; wv t925 = std_length(rt_get(&F_branches, v548_e)); rt_lt(t924, t925); }))) {
    wv v553_b = rt_bind(({ wv t926 = v552_k; wv t927 = rt_get(&F_branches, v548_e); rt_idx(t926, t927); }), 0);
    (void)({ wv t928 = rt_get(&F_cond, v553_b); wv t929 = v549_scope; wv t930 = v550_ctx; rt_chk(s420_expr(up, t928, t929, t930), "chain"); });
    (void)({ wv t931 = rt_get(&F_body, v553_b); wv t932 = rt_chk(s398_newScope(up, v549_scope), "chain"); wv t933 = v550_ctx; rt_chk(s404_block(up, t931, t932, t933), "chain"); });
    if (rt_cond(rt_not(v551_stmtLevel))) {
      (void)rt_chk(s397_requireValue(up, rt_get(&F_body, v553_b)), "chain");
    }
    v552_k = rt_add(v552_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v548_e), RT_NULL))) {
    (void)({ wv t934 = rt_get(&F_els, v548_e); wv t935 = rt_chk(s398_newScope(up, v549_scope), "chain"); wv t936 = v550_ctx; rt_chk(s404_block(up, t934, t935, t936), "chain"); });
    if (rt_cond(rt_not(v551_stmtLevel))) {
      (void)rt_chk(s397_requireValue(up, rt_get(&F_els, v548_e)), "chain");
    }
  } else if (rt_cond(rt_not(v551_stmtLevel))) {
    (void)rt_chk(s393_error(up, rt_get(&F_pos, v548_e), rt_s(&S435), RT_NULL), "chain");
  }
  RT_RET(RT_NULL);
}

static wv s417_lvalue(struct fr100 *up, wv v554_t, wv v555_scope, wv v556_ctx) {
  rt_enter("lvalue");
  v555_scope = rt_bind(v555_scope, 0);
  v556_ctx = rt_bind(v556_ctx, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v554_t), rt_s(&S213)))) {
    wv v560_d = rt_bind(({ wv t937 = rt_get(&F_name, v554_t); wv t938 = rt_get(&F_pos, v554_t); wv t939 = v555_scope; wv t940 = v556_ctx; rt_chk(s403_resolve(up, t937, t938, t939, t940), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v560_d, RT_NULL))) {
      (void)({ wv t941 = v560_d; wv t942 = rt_get(&F_name, v554_t); wv t943 = rt_get(&F_pos, v554_t); wv t944 = rt_cat(3, (wv[]){ rt_s(&S436), rt_text(rt_get(&F_name, v554_t)), rt_s(&S437) }); rt_chk(s418_mustBeMutable(up, t941, t942, t943, t944, rt_b(1)), "lvalue"); });
      if (rt_cond(rt_get(&F_isRef, v560_d))) {
        (void)({ wv t947 = rt_get(&F_refTargets, ({ wv t945 = rt_def(up->v389_curMod, "curMod"); wv t946 = up->v385_modules; rt_idx(t945, t946); })); wv t948 = rt_get(&F_pos, v554_t); std_push(t947, t948); });
      }
    }
    RT_RET(RT_NULL);
  }
  wv v557_isMem = rt_eq(rt_get(&F_k, v554_t), rt_s(&S191));
  wv v558_box = rt_bind((rt_cond(v557_isMem) ? rt_get(&F_rec, v554_t) : rt_get(&F_arr, v554_t)), 0);
  wv v559_what = rt_bind((rt_cond(v557_isMem) ? rt_s(&S438) : rt_s(&S439)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v554_t), rt_s(&S190)))) {
    (void)({ wv t949 = rt_get(&F_index, v554_t); wv t950 = v555_scope; wv t951 = v556_ctx; rt_chk(s420_expr(up, t949, t950, t951), "lvalue"); });
  }
  if (rt_cond(v557_isMem)) {
    if (rt_cond(rt_ne(rt_chk(s410_enumNamed(up, v558_box, v555_scope), "lvalue"), RT_NULL))) {
      (void)({ wv t954 = rt_get(&F_pos, v554_t); wv t955 = ({ wv t952 = rt_text(rt_chk(w5_enumText(v558_box), "lvalue")); wv t953 = rt_text(rt_get(&F_field, v554_t)); rt_cat(5, (wv[]){ rt_s(&S436), t952, rt_s(&S129), t953, rt_s(&S440) }); }); rt_chk(s393_error(up, t954, t955, RT_NULL), "lvalue"); });
      RT_RET(RT_NULL);
    }
    wv v561_m = rt_bind(rt_chk(s408_moduleNamed(up, v558_box, v555_scope), "lvalue"), 0);
    if (rt_cond(rt_ne(v561_m, RT_NULL))) {
      if (rt_cond(rt_ne(rt_chk(s411_exportOf(up, v554_t, v561_m), "lvalue"), RT_NULL))) {
        (void)({ wv t961 = rt_get(&F_pos, v554_t); wv t962 = ({ wv t958 = rt_text(rt_get(&F_name, v558_box)); wv t959 = rt_text(rt_get(&F_field, v554_t)); wv t960 = rt_text(rt_get(&F_path, ({ wv t956 = rt_get(&F_lib, v561_m); wv t957 = up->v385_modules; rt_idx(t956, t957); }))); rt_cat(7, (wv[]){ rt_s(&S436), t958, rt_s(&S129), t959, rt_s(&S441), t960, rt_s(&S442) }); }); rt_chk(s393_error(up, t961, t962, RT_NULL), "lvalue"); });
      }
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v558_box), rt_s(&S213)))) {
    wv v562_d = rt_bind(({ wv t963 = rt_get(&F_name, v558_box); wv t964 = rt_get(&F_pos, v558_box); wv t965 = v555_scope; wv t966 = v556_ctx; rt_chk(s403_resolve(up, t963, t964, t965, t966), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v562_d, RT_NULL))) {
      (void)({ wv t969 = v562_d; wv t970 = rt_get(&F_name, v558_box); wv t971 = rt_get(&F_pos, v558_box); wv t972 = ({ wv t967 = rt_text(v559_what); wv t968 = rt_text(rt_get(&F_name, v558_box)); rt_cat(5, (wv[]){ rt_s(&S443), t967, rt_s(&S444), t968, rt_s(&S445) }); }); rt_chk(s418_mustBeMutable(up, t969, t970, t971, t972, rt_b(0)), "lvalue"); });
    }
  } else {
    (void)rt_chk(s420_expr(up, v558_box, v555_scope, v556_ctx), "lvalue");
  }
  RT_RET(RT_NULL);
}

static wv s418_mustBeMutable(struct fr100 *up, wv v563_d, wv v564_name, wv v565_pos, wv v566_message, wv v567_rebind) {
  rt_enter("mustBeMutable");
  v563_d = rt_bind(v563_d, 0);
  v564_name = rt_bind(v564_name, 0);
  v565_pos = rt_bind(v565_pos, 0);
  v566_message = rt_bind(v566_message, 0);
  v567_rebind = rt_bind(v567_rebind, 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v563_d), rt_s(&S33)), "||") || rt_bool(rt_eq(rt_get(&F_kind, v563_d), rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_kind, v563_d), rt_s(&S418)), "||")))) {
    (void)({ wv t975 = v565_pos; wv t976 = ({ wv t973 = rt_text(v564_name); wv t974 = rt_text(rt_chk(w5_kindText(rt_get(&F_kind, v563_d)), "mustBeMutable")); rt_cat(5, (wv[]){ rt_s(&S113), t973, rt_s(&S446), t974, rt_s(&S447) }); }); rt_chk(s393_error(up, t975, t976, RT_NULL), "mustBeMutable"); });
  } else if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_isVar, v563_d)), "&&") && rt_bool(rt_not(rt_get(&F_isRef, v563_d)), "&&")))) {
    wv v568_hint = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_kind, v563_d), rt_s(&S403)))) {
      v568_hint = rt_bind((rt_cond(v567_rebind) ? ({ wv t977 = rt_text(v564_name); wv t978 = rt_text(v564_name); rt_cat(5, (wv[]){ rt_s(&S448), t977, rt_s(&S449), t978, rt_s(&S450) }); }) : rt_cat(3, (wv[]){ rt_s(&S451), rt_text(v564_name), rt_s(&S16) })), 1);
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v563_d), rt_s(&S178)))) {
      v568_hint = rt_bind((rt_cond(v567_rebind) ? ({ wv t979 = rt_text(v564_name); wv t980 = rt_text(v564_name); rt_cat(5, (wv[]){ rt_s(&S452), t979, rt_s(&S453), t980, rt_s(&S454) }); }) : rt_cat(3, (wv[]){ rt_s(&S455), rt_text(v564_name), rt_s(&S456) })), 1);
    }
    (void)rt_chk(s393_error(up, v565_pos, v566_message, v568_hint), "mustBeMutable");
  }
  RT_RET(RT_NULL);
}

static wv s419_unknownStd(struct fr100 *up, wv v569_node) {
  rt_enter("unknownStd");
  v569_node = rt_bind(v569_node, 0);
  (void)({ wv t981 = rt_get(&F_pos, v569_node); wv t982 = rt_cat(3, (wv[]){ rt_s(&S457), rt_text(rt_get(&F_name, v569_node)), rt_s(&S113) }); rt_chk(s393_error(up, t981, t982, rt_s(&S136)), "unknownStd"); });
  RT_RET(RT_NULL);
}

static wv s420_expr(struct fr100 *up, wv v570_e, wv v571_scope, wv v572_ctx) {
  rt_enter("expr");
  v571_scope = rt_bind(v571_scope, 0);
  v572_ctx = rt_bind(v572_ctx, 0);
  wv v573_kind = rt_bind(rt_get(&F_k, v570_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v573_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v573_kind, rt_s(&S148)), "||")), "||") || rt_bool(rt_eq(v573_kind, rt_s(&S284)), "||")), "||") || rt_bool(rt_eq(v573_kind, rt_s(&S51)), "||")), "||") || rt_bool(rt_eq(v573_kind, rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(v573_kind, rt_s(&S53)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S172)))) {
    wv v574_k = rt_i(0);
    while (rt_cond(({ wv t983 = v574_k; wv t984 = std_length(rt_get(&F_parts, v570_e)); rt_lt(t983, t984); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t985 = v574_k; wv t986 = rt_get(&F_parts, v570_e); rt_idx(t985, t986); })), rt_s(&S186)))) {
        (void)({ wv t989 = ({ wv t987 = v574_k; wv t988 = rt_get(&F_parts, v570_e); rt_idx(t987, t988); }); wv t990 = v571_scope; wv t991 = v572_ctx; rt_chk(s420_expr(up, t989, t990, t991), "expr"); });
      }
      v574_k = rt_add(v574_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S213)))) {
    wv v575_d = rt_bind(({ wv t992 = rt_get(&F_name, v570_e); wv t993 = rt_get(&F_pos, v570_e); wv t994 = v571_scope; wv t995 = v572_ctx; rt_chk(s403_resolve(up, t992, t993, t994, t995), "expr"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v575_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v575_d), rt_s(&S33)), "&&")))) {
      (void)({ wv t998 = rt_get(&F_pos, v570_e); wv t999 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v570_e)), rt_s(&S422) }); wv t1000 = ({ wv t996 = rt_text(rt_get(&F_name, v570_e)); wv t997 = rt_text(rt_get(&F_name, v570_e)); rt_cat(5, (wv[]){ rt_s(&S423), t996, rt_s(&S424), t997, rt_s(&S425) }); }); rt_chk(s393_error(up, t998, t999, t1000), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v575_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v575_d), rt_s(&S46)), "&&")))) {
      (void)({ wv t1001 = rt_get(&F_pos, v570_e); wv t1002 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v570_e)), rt_s(&S426) }); wv t1003 = rt_cat(3, (wv[]){ rt_s(&S427), rt_text(rt_get(&F_name, v570_e)), rt_s(&S428) }); rt_chk(s393_error(up, t1001, t1002, t1003), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v575_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v575_d), rt_s(&S418)), "&&")))) {
      (void)({ wv t1004 = rt_get(&F_pos, v570_e); wv t1005 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v570_e)), rt_s(&S458) }); wv t1006 = rt_cat(3, (wv[]){ rt_s(&S459), rt_text(rt_get(&F_name, v570_e)), rt_s(&S460) }); rt_chk(s393_error(up, t1004, t1005, t1006), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S156)))) {
    wv v576_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v570_e)), "expr"), 0);
    if (rt_cond(rt_eq(v576_sig, RT_NULL))) {
      (void)rt_chk(s419_unknownStd(up, v570_e), "expr");
    } else if (rt_cond(rt_ne(v576_sig, rt_s(&S35)))) {
      (void)({ wv t1009 = rt_get(&F_pos, v570_e); wv t1010 = ({ wv t1007 = rt_text(rt_get(&F_name, v570_e)); wv t1008 = rt_text(rt_get(&F_name, v570_e)); rt_cat(5, (wv[]){ rt_s(&S180), t1007, rt_s(&S461), t1008, rt_s(&S16) }); }); rt_chk(s393_error(up, t1009, t1010, RT_NULL), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v573_kind, rt_s(&S187)), "||") || rt_bool(rt_eq(v573_kind, rt_s(&S188)), "||")))) {
    wv v577_k = rt_i(0);
    while (rt_cond(({ wv t1011 = v577_k; wv t1012 = std_length(rt_get(&F_items, v570_e)); rt_lt(t1011, t1012); }))) {
      (void)({ wv t1015 = ({ wv t1013 = v577_k; wv t1014 = rt_get(&F_items, v570_e); rt_idx(t1013, t1014); }); wv t1016 = v571_scope; wv t1017 = v572_ctx; rt_chk(s420_expr(up, t1015, t1016, t1017), "expr"); });
      v577_k = rt_add(v577_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S189)))) {
    wv v578_k = rt_i(0);
    while (rt_cond(({ wv t1018 = v578_k; wv t1019 = std_length(rt_get(&F_fields, v570_e)); rt_lt(t1018, t1019); }))) {
      (void)({ wv t1022 = rt_get(&F_e, ({ wv t1020 = v578_k; wv t1021 = rt_get(&F_fields, v570_e); rt_idx(t1020, t1021); })); wv t1023 = v571_scope; wv t1024 = v572_ctx; rt_chk(s420_expr(up, t1022, t1023, t1024), "expr"); });
      v578_k = rt_add(v578_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S190)))) {
    (void)({ wv t1025 = rt_get(&F_index, v570_e); wv t1026 = v571_scope; wv t1027 = v572_ctx; rt_chk(s420_expr(up, t1025, t1026, t1027), "expr"); });
    (void)({ wv t1028 = rt_get(&F_arr, v570_e); wv t1029 = v571_scope; wv t1030 = v572_ctx; rt_chk(s420_expr(up, t1028, t1029, t1030), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S191)))) {
    wv v579_lin = rt_bind(({ wv t1031 = rt_get(&F_rec, v570_e); wv t1032 = v571_scope; rt_chk(s410_enumNamed(up, t1031, t1032), "expr"); }), 0);
    if (rt_cond(rt_ne(v579_lin, RT_NULL))) {
      (void)rt_chk(s415_member(up, v570_e, v579_lin), "expr");
      RT_RET(RT_NULL);
    }
    wv v580_m = rt_bind(({ wv t1033 = rt_get(&F_rec, v570_e); wv t1034 = v571_scope; rt_chk(s408_moduleNamed(up, t1033, t1034), "expr"); }), 0);
    if (rt_cond(rt_ne(v580_m, RT_NULL))) {
      (void)rt_chk(s413_qualified(up, v570_e, v580_m), "expr");
    } else {
      (void)({ wv t1035 = rt_get(&F_rec, v570_e); wv t1036 = v571_scope; wv t1037 = v572_ctx; rt_chk(s420_expr(up, t1035, t1036, t1037), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S192)))) {
    (void)({ wv t1038 = rt_get(&F_e, v570_e); wv t1039 = v571_scope; wv t1040 = v572_ctx; rt_chk(s420_expr(up, t1038, t1039, t1040), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S193)))) {
    (void)({ wv t1041 = rt_get(&F_l, v570_e); wv t1042 = v571_scope; wv t1043 = v572_ctx; rt_chk(s420_expr(up, t1041, t1042, t1043), "expr"); });
    (void)({ wv t1044 = rt_get(&F_r, v570_e); wv t1045 = v571_scope; wv t1046 = v572_ctx; rt_chk(s420_expr(up, t1044, t1045, t1046), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S194)))) {
    (void)({ wv t1047 = rt_get(&F_c, v570_e); wv t1048 = v571_scope; wv t1049 = v572_ctx; rt_chk(s420_expr(up, t1047, t1048, t1049), "expr"); });
    (void)({ wv t1050 = rt_get(&F_a, v570_e); wv t1051 = v571_scope; wv t1052 = v572_ctx; rt_chk(s420_expr(up, t1050, t1051, t1052), "expr"); });
    (void)({ wv t1053 = rt_get(&F_b, v570_e); wv t1054 = v571_scope; wv t1055 = v572_ctx; rt_chk(s420_expr(up, t1053, t1054, t1055), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S183)))) {
    wv v581_k = rt_i(0);
    while (rt_cond(({ wv t1056 = v581_k; wv t1057 = std_length(rt_get(&F_args, v570_e)); rt_lt(t1056, t1057); }))) {
      (void)({ wv t1060 = ({ wv t1058 = v581_k; wv t1059 = rt_get(&F_args, v570_e); rt_idx(t1058, t1059); }); wv t1061 = v571_scope; wv t1062 = v572_ctx; rt_chk(s420_expr(up, t1060, t1061, t1062), "expr"); });
      v581_k = rt_add(v581_k, rt_i(1));
    }
    (void)rt_chk(s421_callee(up, v570_e, v571_scope, rt_b(1)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S184)))) {
    (void)rt_chk(s393_error(up, rt_get(&F_pos, v570_e), rt_s(&S462), RT_NULL), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S44)))) {
    (void)rt_chk(s407_checkMatch(up, v570_e, v571_scope, v572_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v573_kind, rt_s(&S185)))) {
    (void)rt_chk(s416_chain(up, v570_e, v571_scope, v572_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S463), rt_text(v573_kind) }));
  RT_RET(RT_NULL);
}

static wv s421_callee(struct fr100 *up, wv v582_e, wv v583_scope, wv v584_used) {
  rt_enter("callee");
  v582_e = rt_bind(v582_e, 0);
  v583_scope = rt_bind(v583_scope, 0);
  v584_used = rt_bind(v584_used, 0);
  wv v585_c = rt_bind(rt_get(&F_callee, v582_e), 0);
  wv v586_n = rt_bind(std_length(rt_get(&F_args, v582_e)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v585_c), rt_s(&S156)))) {
    wv v589_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v585_c)), "callee"), 0);
    if (rt_cond(rt_eq(v589_sig, RT_NULL))) {
      (void)rt_chk(s419_unknownStd(up, v585_c), "callee");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(v589_sig, rt_s(&S35)))) {
      (void)({ wv t1063 = rt_get(&F_pos, v585_c); wv t1064 = rt_cat(3, (wv[]){ rt_s(&S180), rt_text(rt_get(&F_name, v585_c)), rt_s(&S464) }); rt_chk(s393_error(up, t1063, t1064, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_ne(v589_sig, v586_n))) {
      (void)({ wv t1068 = rt_get(&F_pos, v585_c); wv t1069 = ({ wv t1065 = rt_text(rt_get(&F_name, v585_c)); wv t1066 = rt_text(rt_chk(w5_plural(v589_sig), "callee")); wv t1067 = rt_text(v586_n); rt_cat(6, (wv[]){ rt_s(&S180), t1065, rt_s(&S465), t1066, rt_s(&S466), t1067 }); }); rt_chk(s393_error(up, t1068, t1069, RT_NULL), "callee"); });
    }
    RT_RET(RT_NULL);
  }
  wv v587_d = RT_NULL;
  wv v588_label = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v585_c), rt_s(&S191)))) {
    v588_label = ({ wv t1070 = rt_text(rt_get(&F_name, rt_get(&F_rec, v585_c))); wv t1071 = rt_text(rt_get(&F_field, v585_c)); rt_cat(3, (wv[]){ t1070, rt_s(&S129), t1071 }); });
    wv v590_m = rt_bind(({ wv t1072 = rt_get(&F_rec, v585_c); wv t1073 = v583_scope; rt_chk(s408_moduleNamed(up, t1072, t1073), "callee"); }), 0);
    if (rt_cond(rt_eq(v590_m, RT_NULL))) {
      (void)({ wv t1074 = rt_get(&F_rec, v585_c); wv t1075 = v583_scope; rt_chk(s412_unknownModule(up, t1074, t1075), "callee"); });
      RT_RET(RT_NULL);
    }
    v587_d = rt_chk(s411_exportOf(up, v585_c, v590_m), "callee");
    if (rt_cond(rt_eq(v587_d, RT_NULL))) {
      RT_RET(RT_NULL);
    }
  } else {
    v588_label = rt_bind(rt_get(&F_name, v585_c), 1);
    v587_d = ({ wv t1076 = v583_scope; wv t1077 = rt_get(&F_name, v585_c); rt_chk(s402_lookup(up, t1076, t1077), "callee"); });
    if (rt_cond(rt_eq(v587_d, RT_NULL))) {
      (void)({ wv t1078 = rt_get(&F_pos, v585_c); wv t1079 = rt_cat(3, (wv[]){ rt_s(&S467), rt_text(rt_get(&F_name, v585_c)), rt_s(&S113) }); rt_chk(s393_error(up, t1078, t1079, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_ne(rt_get(&F_kind, v587_d), rt_s(&S33)))) {
    wv v591_hint = rt_bind((rt_cond(rt_eq(v586_n, rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S97), rt_text(v588_label), rt_s(&S468) }) : rt_s(&S469)), 0);
    (void)({ wv t1080 = rt_get(&F_pos, v585_c); wv t1081 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v588_label), rt_s(&S470) }); wv t1082 = v591_hint; rt_chk(s393_error(up, t1080, t1081, t1082), "callee"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(({ wv t1083 = rt_get(&F_arity, v587_d); wv t1084 = v586_n; rt_ne(t1083, t1084); }))) {
    (void)({ wv t1088 = rt_get(&F_pos, v585_c); wv t1089 = ({ wv t1085 = rt_text(v588_label); wv t1086 = rt_text(rt_chk(w5_plural(rt_get(&F_arity, v587_d)), "callee")); wv t1087 = rt_text(v586_n); rt_cat(6, (wv[]){ rt_s(&S113), t1085, rt_s(&S465), t1086, rt_s(&S466), t1087 }); }); rt_chk(s393_error(up, t1088, t1089, RT_NULL), "callee"); });
  }
  if (rt_cond(rt_b(rt_bool(v584_used, "&&") && rt_bool(rt_eq(({ wv t1090 = rt_get(&F_mod, v587_d); wv t1091 = rt_get(&F_namePos, v587_d); rt_chk(s396_firstUse(up, t1090, t1091), "callee"); }), RT_NULL), "&&")))) {
    (void)({ wv t1096 = rt_def(up->v391_uses, "uses"); wv t1097 = ({ wv t1092 = rt_get(&F_mod, v587_d); wv t1093 = rt_get(&F_namePos, v587_d); wv t1094 = rt_get(&F_pos, v585_c); wv t1095 = rt_def(up->v389_curMod, "curMod"); rt_rec(0, 4, K47, (wv[]){ t1092, t1093, t1094, t1095 }); }); std_push(t1096, t1097); });
  }
  RT_RET(RT_NULL);
}

static wv s422_importNames(struct fr100 *up, wv v592_m, wv v593_scope) {
  rt_enter("importNames");
  v592_m = rt_bind(v592_m, 0);
  v593_scope = rt_bind(v593_scope, 0);
  wv v594_imports = rt_bind(rt_get(&F_imports, rt_idx(v592_m, up->v385_modules)), 0);
  wv v595_seen = rt_arr(0, NULL, 1);
  wv v596_k = rt_i(0);
  while (rt_cond(({ wv t1098 = v596_k; wv t1099 = std_length(v594_imports); rt_lt(t1098, t1099); }))) {
    wv v597_node = rt_bind(rt_get(&F_node, rt_idx(v596_k, v594_imports)), 0);
    wv v598_lib = rt_bind(rt_get(&F_lib, rt_idx(v596_k, v594_imports)), 0);
    v596_k = rt_add(v596_k, rt_i(1));
    wv v599_path = rt_bind(rt_get(&F_path, rt_idx(v598_lib, up->v385_modules)), 0);
    wv v600_usable = rt_bind(v598_lib, 1);
    if (rt_cond(rt_chk(w1_contains(v595_seen, v598_lib), "importNames"))) {
      (void)({ wv t1100 = rt_get(&F_pathPos, v597_node); wv t1101 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v599_path), rt_s(&S471) }); rt_chk(s393_error(up, t1100, t1101, RT_NULL), "importNames"); });
      v600_usable = RT_NULL;
    } else if (rt_cond(rt_get(&F_notLibrary, rt_idx(v598_lib, up->v385_modules)))) {
      (void)({ wv t1102 = rt_get(&F_pathPos, v597_node); wv t1103 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v599_path), rt_s(&S472) }); rt_chk(s393_error(up, t1102, t1103, RT_NULL), "importNames"); });
      v600_usable = RT_NULL;
    }
    (void)std_push(v595_seen, v598_lib);
    wv v601_d = ({ wv t1104 = rt_get(&F_name, v597_node); wv t1105 = rt_get(&F_namePos, v597_node); wv t1106 = rt_def(up->v389_curMod, "curMod"); wv t1107 = v600_usable; rt_rec(0, 10, K41, (wv[]){ rt_s(&S418), t1104, rt_b(0), rt_b(0), t1105, RT_NULL, RT_NULL, RT_NULL, t1106, t1107 }); });
    (void)({ wv t1108 = v593_scope; wv t1109 = rt_get(&F_name, v597_node); wv t1110 = v601_d; rt_chk(s401_declare(up, t1108, t1109, t1110), "importNames"); });
  }
  RT_RET(RT_NULL);
}

static wv s423_libraryRules(struct fr100 *up, wv v602_m, wv v603_scope, wv v604_isEntry, wv v605_last) {
  rt_enter("libraryRules");
  v602_m = rt_bind(v602_m, 0);
  v603_scope = rt_bind(v603_scope, 0);
  v604_isEntry = rt_bind(v604_isEntry, 0);
  v605_last = rt_bind(v605_last, 0);
  wv v606_program = rt_bind(rt_get(&F_program, rt_idx(v602_m, up->v385_modules)), 0);
  wv v607_k = rt_i(0);
  while (rt_cond(({ wv t1111 = v607_k; wv t1112 = std_length(rt_get(&F_stmts, v606_program)); rt_lt(t1111, t1112); }))) {
    wv v609_s = rt_bind(({ wv t1113 = v607_k; wv t1114 = rt_get(&F_stmts, v606_program); rt_idx(t1113, t1114); }), 0);
    v607_k = rt_add(v607_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v609_s), rt_s(&S205)), "&&") && rt_bool(rt_b(rt_bool(v604_isEntry, "||") || rt_bool(({ wv t1115 = rt_get(&F_pos, v609_s); wv t1116 = rt_get(&F_pos, v605_last); rt_ne(t1115, t1116); }), "||")), "&&")))) {
      (void)rt_chk(s393_error(up, rt_get(&F_pos, v609_s), rt_s(&S473), rt_s(&S474)), "libraryRules");
    } else if (rt_cond(rt_b(rt_bool(rt_not(v604_isEntry), "&&") && rt_bool(rt_not(({ wv t1117 = rt_def(w5_libraryTop, "libraryTop"); wv t1118 = rt_get(&F_k, v609_s); rt_chk(w1_contains(t1117, t1118), "libraryRules"); })), "&&")))) {
      (void)rt_chk(s393_error(up, rt_get(&F_pos, v609_s), rt_s(&S475), rt_s(&S476)), "libraryRules");
    }
  }
  if (rt_cond(v604_isEntry)) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v605_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v605_last), rt_s(&S205)), "||")))) {
    (void)rt_chk(s393_error(up, rt_get(&F_end, v606_program), rt_s(&S477), rt_s(&S478)), "libraryRules");
    RT_RET(RT_NULL);
  }
  wv v608_exports = rt_arr(0, NULL, 1);
  v607_k = rt_i(0);
  while (rt_cond(({ wv t1119 = v607_k; wv t1120 = std_length(rt_get(&F_names, v605_last)); rt_lt(t1119, t1120); }))) {
    wv v610_n = rt_bind(({ wv t1121 = v607_k; wv t1122 = rt_get(&F_names, v605_last); rt_idx(t1121, t1122); }), 0);
    v607_k = rt_add(v607_k, rt_i(1));
    wv v611_d = rt_bind(({ wv t1123 = v603_scope; wv t1124 = rt_get(&F_name, v610_n); rt_chk(s400_own(up, t1123, t1124), "libraryRules"); }), 0);
    if (rt_cond(rt_eq(v611_d, RT_NULL))) {
      (void)({ wv t1125 = rt_get(&F_pos, v610_n); wv t1126 = rt_cat(3, (wv[]){ rt_s(&S479), rt_text(rt_get(&F_name, v610_n)), rt_s(&S480) }); rt_chk(s393_error(up, t1125, t1126, RT_NULL), "libraryRules"); });
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v611_d), rt_s(&S418)))) {
      (void)({ wv t1127 = rt_get(&F_pos, v610_n); wv t1128 = rt_cat(3, (wv[]){ rt_s(&S479), rt_text(rt_get(&F_name, v610_n)), rt_s(&S481) }); rt_chk(s393_error(up, t1127, t1128, RT_NULL), "libraryRules"); });
    } else {
      (void)std_push(v608_exports, v611_d);
    }
  }
  ({ wv t1129 = v608_exports; wv t1130 = rt_idx(v602_m, up->v385_modules); rt_set_mem(t1129, &F_exports, t1130); });
  RT_RET(RT_NULL);
}

static wv s424_checkModule(struct fr100 *up, wv v612_m, wv v613_isEntry) {
  rt_enter("checkModule");
  v612_m = rt_bind(v612_m, 0);
  v613_isEntry = rt_bind(v613_isEntry, 0);
  (void)rt_chk(s394_enter(up, v612_m), "checkModule");
  wv v614_program = rt_bind(rt_get(&F_program, rt_idx(v612_m, up->v385_modules)), 0);
  wv v615_last = RT_NULL;
  wv v616_k = rt_i(0);
  while (rt_cond(({ wv t1131 = v616_k; wv t1132 = std_length(rt_get(&F_stmts, v614_program)); rt_lt(t1131, t1132); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1133 = v616_k; wv t1134 = rt_get(&F_stmts, v614_program); rt_idx(t1133, t1134); })), rt_s(&S202)))) {
      v615_last = rt_bind(({ wv t1135 = v616_k; wv t1136 = rt_get(&F_stmts, v614_program); rt_idx(t1135, t1136); }), 1);
    }
    v616_k = rt_add(v616_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v613_isEntry), "&&") && rt_bool(rt_ne(v615_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v615_last), rt_s(&S54)), "&&")))) {
    rt_set_mem(rt_b(1), &F_notLibrary, rt_idx(v612_m, up->v385_modules));
    RT_RET(RT_NULL);
  }
  wv v617_scope = rt_bind(rt_chk(s398_newScope(up, RT_NULL), "checkModule"), 0);
  (void)rt_chk(s422_importNames(up, v612_m, v617_scope), "checkModule");
  (void)({ wv t1137 = rt_rec(0, 2, K48, (wv[]){ rt_get(&F_stmts, v614_program), RT_NULL }); wv t1138 = v617_scope; wv t1139 = rt_rec(0, 2, K45, (wv[]){ rt_def(up->v425_topFn, "topFn"), rt_i(0) }); rt_chk(s404_block(up, t1137, t1138, t1139), "checkModule"); });
  (void)rt_chk(s423_libraryRules(up, v612_m, v617_scope, v613_isEntry, v615_last), "checkModule");
  RT_RET(RT_NULL);
}

static wv w6_binFn(wv v624_op) {
  rt_enter("binFn");
  v624_op = rt_bind(v624_op, 0);
  wv v625_k = rt_i(0);
  while (rt_cond(({ wv t1166 = ({ wv t1164 = v625_k; wv t1165 = rt_def(w6_binOps, "binOps"); rt_idx(t1164, t1165); }); wv t1167 = v624_op; rt_ne(t1166, t1167); }))) {
    v625_k = rt_add(v625_k, rt_i(1));
  }
  RT_RET(({ wv t1168 = v625_k; wv t1169 = rt_def(w6_binFns, "binFns"); rt_idx(t1168, t1169); }));
}

static wv w6_modeOf(wv v626_x) {
  rt_enter("modeOf");
  v626_x = rt_bind(v626_x, 0);
  RT_RET((rt_cond(rt_get(&F_isVar, v626_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v626_x)) ? rt_s(&S37) : rt_s(&S35))));
}

static wv w6_unFn(wv v627_op) {
  rt_enter("unFn");
  v627_op = rt_bind(v627_op, 0);
  RT_RET((rt_cond(rt_eq(v627_op, rt_s(&S85))) ? rt_s(&S505) : (rt_cond(rt_eq(v627_op, rt_s(&S94))) ? rt_s(&S506) : rt_s(&S507))));
}

static wv w6_repeat(wv v628_s, wv v629_count) {
  rt_enter("repeat");
  v628_s = rt_bind(v628_s, 0);
  v629_count = rt_bind(v629_count, 0);
  wv v630_out = rt_s(&S13);
  wv v631_k = rt_i(0);
  while (rt_cond(rt_lt(v631_k, v629_count))) {
    v630_out = rt_add(v630_out, v628_s);
    v631_k = rt_add(v631_k, rt_i(1));
  }
  RT_RET(v630_out);
}

static wv w6_utf8Length(wv v632_s) {
  rt_enter("utf8Length");
  v632_s = rt_bind(v632_s, 0);
  wv v633_cs = rt_bind(std_split(v632_s, rt_s(&S13)), 0);
  wv v634_n = rt_i(0);
  wv v635_k = rt_i(0);
  while (rt_cond(({ wv t1170 = v635_k; wv t1171 = std_length(v633_cs); rt_lt(t1170, t1171); }))) {
    wv v636_c = rt_bind(std_code(rt_idx(v635_k, v633_cs)), 0);
    wv t1172 = (rt_cond(rt_lt(v636_c, rt_i(128))) ? rt_i(1) : (rt_cond(rt_lt(v636_c, rt_i(2048))) ? rt_i(2) : (rt_cond(rt_lt(v636_c, rt_i(65536))) ? rt_i(3) : rt_i(4))));
    v634_n = rt_add(v634_n, t1172);
    v635_k = rt_add(v635_k, rt_i(1));
  }
  RT_RET(v634_n);
}

static wv w6_cString(wv v637_s) {
  rt_enter("cString");
  v637_s = rt_bind(v637_s, 0);
  wv v638_cs = rt_bind(std_split(v637_s, rt_s(&S13)), 0);
  wv v639_out = rt_s(&S18);
  wv v640_k = rt_i(0);
  while (rt_cond(({ wv t1173 = v640_k; wv t1174 = std_length(v638_cs); rt_lt(t1173, t1174); }))) {
    wv v641_ch = rt_bind(rt_idx(v640_k, v638_cs), 0);
    wv v642_c = rt_bind(std_code(v641_ch), 0);
    if (rt_cond(rt_eq(v641_ch, rt_s(&S18)))) {
      v639_out = rt_add(v639_out, rt_s(&S508));
    } else if (rt_cond(rt_eq(v641_ch, rt_s(&S19)))) {
      v639_out = rt_add(v639_out, rt_s(&S509));
    } else if (rt_cond(rt_eq(v641_ch, rt_s(&S2)))) {
      v639_out = rt_add(v639_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v641_ch, rt_s(&S1)))) {
      v639_out = rt_add(v639_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v641_ch, rt_s(&S3)))) {
      v639_out = rt_add(v639_out, rt_s(&S21));
    } else if (rt_cond(rt_b(rt_bool(rt_lt(v642_c, rt_i(32)), "||") || rt_bool(rt_eq(v642_c, rt_i(127)), "||")))) {
      v639_out = ({ wv t1178 = v639_out; wv t1179 = ({ wv t1175 = rt_text(std_floor(rt_div(v642_c, rt_i(64)))); wv t1176 = rt_text(rt_mod(std_floor(rt_div(v642_c, rt_i(8))), rt_i(8))); wv t1177 = rt_text(rt_mod(v642_c, rt_i(8))); rt_cat(4, (wv[]){ rt_s(&S19), t1175, t1176, t1177 }); }); rt_add(t1178, t1179); });
    } else {
      v639_out = rt_add(v639_out, v641_ch);
    }
    v640_k = rt_add(v640_k, rt_i(1));
  }
  RT_RET(rt_add(v639_out, rt_s(&S18)));
}

static wv w6_wstrInit(wv v643_s) {
  rt_enter("wstrInit");
  v643_s = rt_bind(v643_s, 0);
  wv v644_bytes = rt_bind(rt_chk(w6_utf8Length(v643_s), "wstrInit"), 0);
  wv v645_cps = rt_bind(std_length(v643_s), 0);
  RT_RET(({ wv t1180 = rt_text(v644_bytes); wv t1181 = rt_text(v645_cps); wv t1182 = rt_text((rt_cond(rt_eq(v644_bytes, v645_cps)) ? rt_i(1) : rt_i(0))); wv t1183 = rt_text(rt_chk(w6_cString(v643_s), "wstrInit")); rt_cat(9, (wv[]){ rt_s(&S510), t1180, rt_s(&S415), t1181, rt_s(&S415), t1182, rt_s(&S511), t1183, rt_s(&S512) }); }));
}

static wv w6_values(wv v646_cs) {
  rt_enter("values");
  RT_RET((rt_cond(rt_eq(std_length(v646_cs), rt_i(0))) ? rt_s(&S513) : rt_cat(3, (wv[]){ rt_s(&S514), rt_text(std_join(v646_cs, rt_s(&S415))), rt_s(&S512) })));
}

static wv w6_generateC(wv p_v647_modules, wv p_v648_runtime) {
  rt_enter("generateC");
  struct fr142 fr = { 0 };
  fr.v647_modules = rt_bind(p_v647_modules, 0);
  fr.v648_runtime = rt_bind(p_v648_runtime, 0);
  fr.v649_n = rt_i(0);
  fr.v650_tmps = rt_i(0);
  fr.v651_strings = rt_arr(0, NULL, 1);
  fr.v652_fields = rt_arr(0, NULL, 1);
  fr.v653_shapes = rt_arr(0, NULL, 1);
  fr.v654_enumDefs = rt_arr(0, NULL, 1);
  fr.v655_globals = rt_arr(0, NULL, 1);
  fr.v656_frames = rt_arr(0, NULL, 1);
  fr.v657_functions = rt_arr(0, NULL, 1);
  fr.v658_out = rt_arr(0, NULL, 1);
  fr.v659_indent = rt_i(1);
  fr.v660_loops = rt_i(0);
  fr.v661_iife = rt_b(0);
  fr.v662_decls = rt_arr(0, NULL, 1);
  fr.v663_fns = rt_arr(0, NULL, 1);
  fr.v664_tops = rt_arr(0, NULL, 1);
  fr.v665_slots = rt_arr(0, NULL, 1);
  fr.v666_curMod = rt_i(0);
  fr.v667_mod = rt_s(&S13);
  fr.v668_curSlot = rt_arr(0, NULL, 1);
  fr.v669_stolen = rt_arr(0, NULL, 1);
  fr.v670_ownEnums = rt_arr(0, NULL, 1);
  fr.v671_cur = rt_i(0);
  wv v732_mi = rt_i(0);
  while (rt_cond(({ wv t2130 = v732_mi; wv t2131 = std_length(fr.v647_modules); rt_lt(t2130, t2131); }))) {
    (void)rt_chk(s686_analyze(&fr, v732_mi), "generateC");
    v732_mi = rt_add(v732_mi, rt_i(1));
  }
  v732_mi = rt_i(0);
  while (rt_cond(({ wv t2132 = v732_mi; wv t2133 = std_length(fr.v647_modules); rt_lt(t2132, t2133); }))) {
    wv v1010_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v732_mi, fr.v647_modules))), 0);
    wv v1011_li = rt_i(0);
    while (rt_cond(({ wv t2134 = v1011_li; wv t2135 = std_length(v1010_list); rt_lt(t2134, t2135); }))) {
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1011_li, v1010_list)), rt_s(&S46)))) {
        (void)({ wv t2136 = rt_idx(v1011_li, v1010_list); wv t2137 = rt_chk(s672_modId(&fr, v732_mi), "generateC"); rt_chk(s692_enumDef(&fr, t2136, t2137), "generateC"); });
      }
      v1011_li = rt_add(v1011_li, rt_i(1));
    }
    v732_mi = rt_add(v732_mi, rt_i(1));
  }
  v732_mi = rt_i(0);
  while (rt_cond(({ wv t2138 = v732_mi; wv t2139 = std_length(fr.v647_modules); rt_lt(t2138, t2139); }))) {
    (void)rt_chk(s687_enter(&fr, v732_mi), "generateC");
    (void)rt_chk(s701_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v732_mi, fr.v647_modules)))), "generateC");
    v732_mi = rt_add(v732_mi, rt_i(1));
  }
  RT_RET(rt_chk(s731_assemble(&fr), "generateC"));
}

static wv s672_modId(struct fr142 *up, wv v733_m) {
  rt_enter("modId");
  v733_m = rt_bind(v733_m, 0);
  RT_RET((rt_cond(({ wv t1184 = v733_m; wv t1185 = rt_sub(std_length(up->v647_modules), rt_i(1)); rt_eq(t1184, t1185); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v733_m, rt_i(1))) })));
}

static wv s673_hasFrame(struct fr142 *up, wv v734_f) {
  rt_enter("hasFrame");
  v734_f = rt_bind(v734_f, 0);
  RT_RET(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1186 = v734_f; wv t1187 = rt_def(up->v663_fns, "fns"); rt_idx(t1186, t1187); }))), "&&") && rt_bool(rt_gt(rt_get(&F_children, ({ wv t1188 = v734_f; wv t1189 = rt_def(up->v663_fns, "fns"); rt_idx(t1188, t1189); })), rt_i(0)), "&&")));
}

static wv s674_newScope(struct fr142 *up, wv v735_parent) {
  rt_enter("newScope");
  v735_parent = rt_bind(v735_parent, 0);
  wv v736_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K40, (wv[]){ v735_parent, v736_names }));
}

static wv s675_put(struct fr142 *up, wv v737_scope, wv v738_name, wv v739_kind, wv v740_d) {
  rt_enter("put");
  v737_scope = rt_bind(v737_scope, 0);
  v738_name = rt_bind(v738_name, 0);
  v739_kind = rt_bind(v739_kind, 0);
  v740_d = rt_bind(v740_d, 0);
  (void)({ wv t1190 = rt_get(&F_names, v737_scope); wv t1191 = rt_rec(0, 3, K49, (wv[]){ v738_name, v739_kind, v740_d }); std_push(t1190, t1191); });
  RT_RET(RT_NULL);
}

static wv s676_find(struct fr142 *up, wv v741_scope, wv v742_name) {
  rt_enter("find");
  v742_name = rt_bind(v742_name, 0);
  wv v743_s = v741_scope;
  while (rt_cond(rt_ne(v743_s, RT_NULL))) {
    wv v744_ns = rt_get(&F_names, v743_s);
    wv v745_k = rt_i(0);
    while (rt_cond(({ wv t1192 = v745_k; wv t1193 = std_length(v744_ns); rt_lt(t1192, t1193); }))) {
      if (rt_cond(({ wv t1194 = rt_get(&F_name, rt_idx(v745_k, v744_ns)); wv t1195 = v742_name; rt_eq(t1194, t1195); }))) {
        RT_RET(rt_idx(v745_k, v744_ns));
      }
      v745_k = rt_add(v745_k, rt_i(1));
    }
    v743_s = rt_get(&F_parent, v743_s);
  }
  RT_RET(RT_NULL);
}

static wv s677_kindOf(struct fr142 *up, wv v746_scope, wv v747_name) {
  rt_enter("kindOf");
  v746_scope = rt_bind(v746_scope, 0);
  v747_name = rt_bind(v747_name, 0);
  wv v748_entry = rt_bind(rt_chk(s676_find(up, v746_scope, v747_name), "kindOf"), 0);
  RT_RET((rt_cond(rt_eq(v748_entry, RT_NULL)) ? RT_NULL : rt_get(&F_kind, v748_entry)));
}

static wv s678_newFn(struct fr142 *up, wv v749_name, wv v750_parent) {
  rt_enter("newFn");
  v749_name = rt_bind(v749_name, 0);
  v750_parent = rt_bind(v750_parent, 0);
  wv v751_f = ({ wv t1198 = v749_name; wv t1199 = v750_parent; wv t1200 = (rt_cond(rt_eq(v750_parent, RT_NULL)) ? rt_i(0) : rt_add(rt_get(&F_depth, ({ wv t1196 = v750_parent; wv t1197 = rt_def(up->v663_fns, "fns"); rt_idx(t1196, t1197); })), rt_i(1))); wv t1201 = rt_eq(v750_parent, RT_NULL); rt_rec(1, 6, K50, (wv[]){ t1198, t1199, t1200, t1201, rt_i(0), rt_s(&S13) }); });
  (void)({ wv t1202 = rt_def(up->v663_fns, "fns"); wv t1203 = v751_f; std_push(t1202, t1203); });
  RT_RET(rt_sub(std_length(rt_def(up->v663_fns, "fns")), rt_i(1)));
}

static wv s679_bind(struct fr142 *up, wv v752_kind, wv v753_name, wv v754_f, wv v755_atTop, wv v756_mode) {
  rt_enter("bind");
  v752_kind = rt_bind(v752_kind, 0);
  v753_name = rt_bind(v753_name, 0);
  v754_f = rt_bind(v754_f, 0);
  v755_atTop = rt_bind(v755_atTop, 0);
  v756_mode = rt_bind(v756_mode, 0);
  wv v757_cname = ({ wv t1204 = rt_text(rt_def(up->v667_mod, "mod")); wv t1205 = rt_text(v753_name); rt_cat(4, (wv[]){ rt_s(&S515), t1204, rt_s(&S12), t1205 }); });
  if (rt_cond(rt_not(v755_atTop))) {
    v757_cname = ({ wv t1208 = (rt_cond(rt_eq(v752_kind, rt_s(&S33))) ? rt_s(&S312) : rt_s(&S516)); wv t1209 = ({ wv t1206 = rt_text(rt_def(up->v649_n, "n")); wv t1207 = rt_text(v753_name); rt_cat(3, (wv[]){ t1206, rt_s(&S12), t1207 }); }); rt_add(t1208, t1209); });
    up->v649_n = rt_add(rt_def(up->v649_n, "n"), rt_i(1));
  }
  wv v758_d = ({ wv t1210 = v752_kind; wv t1211 = v753_name; wv t1212 = v754_f; wv t1213 = v757_cname; wv t1214 = rt_eq(v756_mode, rt_s(&S37)); rt_rec(1, 7, K51, (wv[]){ t1210, t1211, t1212, t1213, t1214, rt_b(0), RT_NULL }); });
  (void)({ wv t1215 = rt_def(up->v662_decls, "decls"); wv t1216 = v758_d; std_push(t1215, t1216); });
  if (rt_cond(rt_b(rt_bool(rt_get(&F_top, ({ wv t1217 = v754_f; wv t1218 = rt_def(up->v663_fns, "fns"); rt_idx(t1217, t1218); })), "&&") && rt_bool(rt_ne(v752_kind, rt_s(&S33)), "&&")))) {
    (void)({ wv t1219 = rt_def(up->v655_globals, "globals"); wv t1220 = v757_cname; std_push(t1219, t1220); });
  }
  RT_RET(rt_sub(std_length(rt_def(up->v662_decls, "decls")), rt_i(1)));
}

static wv s680_ablock(struct fr142 *up, wv v759_b, wv v760_scope, wv v761_f, wv v762_topScope) {
  rt_enter("ablock");
  v760_scope = rt_bind(v760_scope, 0);
  v761_f = rt_bind(v761_f, 0);
  v762_topScope = rt_bind(v762_topScope, 0);
  wv v763_atTop = rt_eq(v760_scope, v762_topScope);
  wv v764_list = rt_bind(rt_get(&F_stmts, v759_b), 0);
  wv v765_k = rt_i(0);
  while (rt_cond(({ wv t1221 = v765_k; wv t1222 = std_length(v764_list); rt_lt(t1221, t1222); }))) {
    wv v766_s = rt_bind(rt_idx(v765_k, v764_list), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v766_s), rt_s(&S33)))) {
      wv v767_d = rt_bind(({ wv t1223 = rt_get(&F_name, v766_s); wv t1224 = v761_f; wv t1225 = v763_atTop; rt_chk(s679_bind(up, rt_s(&S33), t1223, t1224, t1225, rt_s(&S13)), "ablock"); }), 0);
      wv v768_child = rt_bind(({ wv t1226 = rt_get(&F_name, v766_s); wv t1227 = v761_f; rt_chk(s678_newFn(up, t1226, t1227), "ablock"); }), 0);
      ({ wv t1232 = rt_get(&F_cname, ({ wv t1228 = v767_d; wv t1229 = rt_def(up->v662_decls, "decls"); rt_idx(t1228, t1229); })); wv t1233 = ({ wv t1230 = v768_child; wv t1231 = rt_def(up->v663_fns, "fns"); rt_idx(t1230, t1231); }); rt_set_mem(t1232, &F_cname, t1233); });
      ({ wv t1236 = v768_child; wv t1237 = ({ wv t1234 = v767_d; wv t1235 = rt_def(up->v662_decls, "decls"); rt_idx(t1234, t1235); }); rt_set_mem(t1236, &F_fnIndex, t1237); });
      rt_upd_mem(rt_i(1), &F_children, ({ wv t1238 = v761_f; wv t1239 = rt_def(up->v663_fns, "fns"); rt_idx(t1238, t1239); }), rt_add);
      ({ wv t1240 = v767_d; wv t1241 = rt_get(&F_namePos, v766_s); wv t1242 = rt_def(up->v668_curSlot, "curSlot"); rt_set_idx(t1240, t1241, t1242); });
      (void)({ wv t1243 = v760_scope; wv t1244 = rt_get(&F_name, v766_s); wv t1245 = v767_d; rt_chk(s675_put(up, t1243, t1244, rt_s(&S33), t1245), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v766_s), rt_s(&S46)))) {
      (void)({ wv t1246 = v760_scope; wv t1247 = rt_get(&F_name, v766_s); rt_chk(s675_put(up, t1246, t1247, rt_s(&S46), RT_NULL), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v766_s), rt_s(&S178)))) {
      wv v769_d = rt_bind(({ wv t1248 = rt_get(&F_name, v766_s); wv t1249 = v761_f; wv t1250 = v763_atTop; wv t1251 = rt_chk(w6_modeOf(v766_s), "ablock"); rt_chk(s679_bind(up, rt_s(&S178), t1248, t1249, t1250, t1251), "ablock"); }), 0);
      ({ wv t1252 = v769_d; wv t1253 = rt_get(&F_namePos, v766_s); wv t1254 = rt_def(up->v668_curSlot, "curSlot"); rt_set_idx(t1252, t1253, t1254); });
      (void)({ wv t1255 = v760_scope; wv t1256 = rt_get(&F_name, v766_s); wv t1257 = v769_d; rt_chk(s675_put(up, t1255, t1256, rt_s(&S178), t1257), "ablock"); });
    }
    v765_k = rt_add(v765_k, rt_i(1));
  }
  v765_k = rt_i(0);
  while (rt_cond(({ wv t1258 = v765_k; wv t1259 = std_length(v764_list); rt_lt(t1258, t1259); }))) {
    (void)({ wv t1260 = rt_idx(v765_k, v764_list); wv t1261 = v760_scope; wv t1262 = v761_f; wv t1263 = v762_topScope; rt_chk(s681_astmt(up, t1260, t1261, t1262, t1263), "ablock"); });
    v765_k = rt_add(v765_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v759_b), RT_NULL))) {
    (void)({ wv t1264 = rt_get(&F_value, v759_b); wv t1265 = v760_scope; wv t1266 = v761_f; rt_chk(s685_aexpr(up, t1264, t1265, t1266), "ablock"); });
  }
  RT_RET(RT_NULL);
}

static wv s681_astmt(struct fr142 *up, wv v770_s, wv v771_scope, wv v772_f, wv v773_topScope) {
  rt_enter("astmt");
  v771_scope = rt_bind(v771_scope, 0);
  v772_f = rt_bind(v772_f, 0);
  v773_topScope = rt_bind(v773_topScope, 0);
  wv v774_kind = rt_bind(rt_get(&F_k, v770_s), 0);
  if (rt_cond(rt_eq(v774_kind, rt_s(&S33)))) {
    wv v775_d = rt_bind(({ wv t1267 = rt_get(&F_namePos, v770_s); wv t1268 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1267, t1268); }), 0);
    wv v776_child = rt_bind(rt_get(&F_fnIndex, ({ wv t1269 = v775_d; wv t1270 = rt_def(up->v662_decls, "decls"); rt_idx(t1269, t1270); })), 0);
    wv v777_ps = rt_bind(rt_chk(s674_newScope(up, v771_scope), "astmt"), 0);
    wv v778_k = rt_i(0);
    while (rt_cond(({ wv t1271 = v778_k; wv t1272 = std_length(rt_get(&F_params, v770_s)); rt_lt(t1271, t1272); }))) {
      wv v779_p = rt_bind(({ wv t1273 = v778_k; wv t1274 = rt_get(&F_params, v770_s); rt_idx(t1273, t1274); }), 0);
      wv v780_pd = rt_bind(({ wv t1275 = rt_get(&F_name, v779_p); wv t1276 = v776_child; wv t1277 = rt_chk(w6_modeOf(v779_p), "astmt"); rt_chk(s679_bind(up, rt_s(&S403), t1275, t1276, rt_b(0), t1277), "astmt"); }), 0);
      ({ wv t1278 = v780_pd; wv t1279 = rt_get(&F_pos, v779_p); wv t1280 = rt_def(up->v668_curSlot, "curSlot"); rt_set_idx(t1278, t1279, t1280); });
      (void)({ wv t1281 = v777_ps; wv t1282 = rt_get(&F_name, v779_p); wv t1283 = v780_pd; rt_chk(s675_put(up, t1281, t1282, rt_s(&S403), t1283), "astmt"); });
      v778_k = rt_add(v778_k, rt_i(1));
    }
    (void)({ wv t1284 = rt_get(&F_body, v770_s); wv t1285 = rt_chk(s674_newScope(up, v777_ps), "astmt"); wv t1286 = v776_child; wv t1287 = v773_topScope; rt_chk(s680_ablock(up, t1284, t1285, t1286, t1287), "astmt"); });
  } else if (rt_cond(rt_eq(v774_kind, rt_s(&S41)))) {
    (void)({ wv t1288 = rt_get(&F_cond, v770_s); wv t1289 = v771_scope; wv t1290 = v772_f; rt_chk(s685_aexpr(up, t1288, t1289, t1290), "astmt"); });
    (void)({ wv t1291 = rt_get(&F_body, v770_s); wv t1292 = rt_chk(s674_newScope(up, v771_scope), "astmt"); wv t1293 = v772_f; wv t1294 = v773_topScope; rt_chk(s680_ablock(up, t1291, t1292, t1293, t1294), "astmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v774_kind, rt_s(&S34)), "||") || rt_bool(rt_eq(v774_kind, rt_s(&S57)), "||")), "||") || rt_bool(rt_eq(v774_kind, rt_s(&S54)), "||")), "||") || rt_bool(rt_eq(v774_kind, rt_s(&S178)), "||")))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v770_s), RT_NULL))) {
      (void)({ wv t1295 = rt_get(&F_e, v770_s); wv t1296 = v771_scope; wv t1297 = v772_f; rt_chk(s685_aexpr(up, t1295, t1296, t1297), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v774_kind, rt_s(&S177)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v770_s)), rt_s(&S184)))) {
      (void)({ wv t1298 = rt_get(&F_arg, rt_get(&F_e, v770_s)); wv t1299 = v771_scope; wv t1300 = v772_f; rt_chk(s685_aexpr(up, t1298, t1299, t1300), "astmt"); });
    } else {
      (void)({ wv t1301 = rt_get(&F_e, v770_s); wv t1302 = v771_scope; wv t1303 = v772_f; rt_chk(s685_aexpr(up, t1301, t1302, t1303), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v774_kind, rt_s(&S38)))) {
    (void)({ wv t1304 = rt_get(&F_e, v770_s); wv t1305 = v771_scope; wv t1306 = v772_f; rt_chk(s685_aexpr(up, t1304, t1305, t1306), "astmt"); });
    (void)({ wv t1307 = rt_get(&F_target, v770_s); wv t1308 = v771_scope; wv t1309 = v772_f; rt_chk(s683_atarget(up, t1307, t1308, t1309), "astmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v770_s), RT_NULL))) {
    wv v781_h = rt_bind(rt_get(&F_handler, v770_s), 0);
    wv v782_hs = rt_bind(rt_chk(s674_newScope(up, v771_scope), "astmt"), 0);
    if (rt_cond(rt_ne(rt_get(&F_param, v781_h), RT_NULL))) {
      wv v783_wd = rt_bind(({ wv t1310 = rt_get(&F_param, v781_h); wv t1311 = v772_f; rt_chk(s679_bind(up, rt_s(&S56), t1310, t1311, rt_b(0), rt_s(&S13)), "astmt"); }), 0);
      ({ wv t1312 = v783_wd; wv t1313 = rt_get(&F_paramPos, v781_h); wv t1314 = rt_def(up->v668_curSlot, "curSlot"); rt_set_idx(t1312, t1313, t1314); });
      (void)({ wv t1315 = v782_hs; wv t1316 = rt_get(&F_param, v781_h); wv t1317 = v783_wd; rt_chk(s675_put(up, t1315, t1316, rt_s(&S56), t1317), "astmt"); });
    }
    (void)({ wv t1318 = rt_get(&F_body, v781_h); wv t1319 = rt_chk(s674_newScope(up, v782_hs), "astmt"); wv t1320 = v772_f; wv t1321 = v773_topScope; rt_chk(s680_ablock(up, t1318, t1319, t1320, t1321), "astmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s682_use(struct fr142 *up, wv v784_x, wv v785_scope, wv v786_f) {
  rt_enter("use");
  v784_x = rt_bind(v784_x, 0);
  v785_scope = rt_bind(v785_scope, 0);
  v786_f = rt_bind(v786_f, 0);
  wv v787_d = rt_bind(rt_get(&F_d, ({ wv t1322 = v785_scope; wv t1323 = rt_get(&F_name, v784_x); rt_chk(s676_find(up, t1322, t1323), "use"); })), 0);
  ({ wv t1324 = v787_d; wv t1325 = rt_get(&F_pos, v784_x); wv t1326 = rt_def(up->v668_curSlot, "curSlot"); rt_set_idx(t1324, t1325, t1326); });
  if (rt_cond(({ wv t1329 = rt_get(&F_func, ({ wv t1327 = v787_d; wv t1328 = rt_def(up->v662_decls, "decls"); rt_idx(t1327, t1328); })); wv t1330 = v786_f; rt_ne(t1329, t1330); }))) {
    rt_set_mem(rt_b(1), &F_captured, ({ wv t1331 = v787_d; wv t1332 = rt_def(up->v662_decls, "decls"); rt_idx(t1331, t1332); }));
  }
  RT_RET(RT_NULL);
}

static wv s683_atarget(struct fr142 *up, wv v788_t, wv v789_scope, wv v790_f) {
  rt_enter("atarget");
  v788_t = rt_bind(v788_t, 0);
  v789_scope = rt_bind(v789_scope, 0);
  v790_f = rt_bind(v790_f, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v788_t), rt_s(&S213)))) {
    (void)rt_chk(s682_use(up, v788_t, v789_scope, v790_f), "atarget");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v788_t), rt_s(&S190)))) {
    (void)({ wv t1333 = rt_get(&F_index, v788_t); wv t1334 = v789_scope; wv t1335 = v790_f; rt_chk(s685_aexpr(up, t1333, t1334, t1335), "atarget"); });
  }
  (void)({ wv t1336 = (rt_cond(rt_eq(rt_get(&F_k, v788_t), rt_s(&S191))) ? rt_get(&F_rec, v788_t) : rt_get(&F_arr, v788_t)); wv t1337 = v789_scope; wv t1338 = v790_f; rt_chk(s685_aexpr(up, t1336, t1337, t1338), "atarget"); });
  RT_RET(RT_NULL);
}

static wv s684_notValue(struct fr142 *up, wv v791_x, wv v792_scope) {
  rt_enter("notValue");
  v792_scope = rt_bind(v792_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v791_x), rt_s(&S191)))) {
    RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v791_x)), rt_s(&S213)), "&&") && rt_bool(rt_eq(({ wv t1339 = v792_scope; wv t1340 = rt_get(&F_name, rt_get(&F_rec, v791_x)); rt_chk(s677_kindOf(up, t1339, t1340), "notValue"); }), rt_s(&S418)), "&&")));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v791_x), rt_s(&S213)))) {
    RT_RET(rt_b(0));
  }
  wv v793_kind = rt_bind(({ wv t1341 = v792_scope; wv t1342 = rt_get(&F_name, v791_x); rt_chk(s677_kindOf(up, t1341, t1342), "notValue"); }), 0);
  RT_RET(rt_b(rt_bool(rt_eq(v793_kind, rt_s(&S46)), "||") || rt_bool(rt_eq(v793_kind, rt_s(&S418)), "||")));
}

static wv s685_aexpr(struct fr142 *up, wv v794_e, wv v795_scope, wv v796_f) {
  rt_enter("aexpr");
  v795_scope = rt_bind(v795_scope, 0);
  v796_f = rt_bind(v796_f, 0);
  wv v797_kind = rt_bind(rt_get(&F_k, v794_e), 0);
  if (rt_cond(rt_eq(v797_kind, rt_s(&S172)))) {
    wv v798_k = rt_i(0);
    while (rt_cond(({ wv t1343 = v798_k; wv t1344 = std_length(rt_get(&F_parts, v794_e)); rt_lt(t1343, t1344); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t1345 = v798_k; wv t1346 = rt_get(&F_parts, v794_e); rt_idx(t1345, t1346); })), rt_s(&S186)))) {
        (void)({ wv t1349 = ({ wv t1347 = v798_k; wv t1348 = rt_get(&F_parts, v794_e); rt_idx(t1347, t1348); }); wv t1350 = v795_scope; wv t1351 = v796_f; rt_chk(s685_aexpr(up, t1349, t1350, t1351), "aexpr"); });
      }
      v798_k = rt_add(v798_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S213)))) {
    (void)rt_chk(s682_use(up, v794_e, v795_scope, v796_f), "aexpr");
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v797_kind, rt_s(&S187)), "||") || rt_bool(rt_eq(v797_kind, rt_s(&S188)), "||")))) {
    wv v799_k = rt_i(0);
    while (rt_cond(({ wv t1352 = v799_k; wv t1353 = std_length(rt_get(&F_items, v794_e)); rt_lt(t1352, t1353); }))) {
      (void)({ wv t1356 = ({ wv t1354 = v799_k; wv t1355 = rt_get(&F_items, v794_e); rt_idx(t1354, t1355); }); wv t1357 = v795_scope; wv t1358 = v796_f; rt_chk(s685_aexpr(up, t1356, t1357, t1358), "aexpr"); });
      v799_k = rt_add(v799_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S189)))) {
    wv v800_k = rt_i(0);
    while (rt_cond(({ wv t1359 = v800_k; wv t1360 = std_length(rt_get(&F_fields, v794_e)); rt_lt(t1359, t1360); }))) {
      (void)({ wv t1363 = rt_get(&F_e, ({ wv t1361 = v800_k; wv t1362 = rt_get(&F_fields, v794_e); rt_idx(t1361, t1362); })); wv t1364 = v795_scope; wv t1365 = v796_f; rt_chk(s685_aexpr(up, t1363, t1364, t1365), "aexpr"); });
      v800_k = rt_add(v800_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S190)))) {
    (void)({ wv t1366 = rt_get(&F_index, v794_e); wv t1367 = v795_scope; wv t1368 = v796_f; rt_chk(s685_aexpr(up, t1366, t1367, t1368), "aexpr"); });
    (void)({ wv t1369 = rt_get(&F_arr, v794_e); wv t1370 = v795_scope; wv t1371 = v796_f; rt_chk(s685_aexpr(up, t1369, t1370, t1371), "aexpr"); });
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S191)))) {
    if (rt_cond(rt_not(({ wv t1372 = rt_get(&F_rec, v794_e); wv t1373 = v795_scope; rt_chk(s684_notValue(up, t1372, t1373), "aexpr"); })))) {
      (void)({ wv t1374 = rt_get(&F_rec, v794_e); wv t1375 = v795_scope; wv t1376 = v796_f; rt_chk(s685_aexpr(up, t1374, t1375, t1376), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S192)))) {
    (void)({ wv t1377 = rt_get(&F_e, v794_e); wv t1378 = v795_scope; wv t1379 = v796_f; rt_chk(s685_aexpr(up, t1377, t1378, t1379), "aexpr"); });
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S193)))) {
    (void)({ wv t1380 = rt_get(&F_l, v794_e); wv t1381 = v795_scope; wv t1382 = v796_f; rt_chk(s685_aexpr(up, t1380, t1381, t1382), "aexpr"); });
    (void)({ wv t1383 = rt_get(&F_r, v794_e); wv t1384 = v795_scope; wv t1385 = v796_f; rt_chk(s685_aexpr(up, t1383, t1384, t1385), "aexpr"); });
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S194)))) {
    (void)({ wv t1386 = rt_get(&F_c, v794_e); wv t1387 = v795_scope; wv t1388 = v796_f; rt_chk(s685_aexpr(up, t1386, t1387, t1388), "aexpr"); });
    (void)({ wv t1389 = rt_get(&F_a, v794_e); wv t1390 = v795_scope; wv t1391 = v796_f; rt_chk(s685_aexpr(up, t1389, t1390, t1391), "aexpr"); });
    (void)({ wv t1392 = rt_get(&F_b, v794_e); wv t1393 = v795_scope; wv t1394 = v796_f; rt_chk(s685_aexpr(up, t1392, t1393, t1394), "aexpr"); });
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S183)))) {
    wv v801_k = rt_i(0);
    while (rt_cond(({ wv t1395 = v801_k; wv t1396 = std_length(rt_get(&F_args, v794_e)); rt_lt(t1395, t1396); }))) {
      (void)({ wv t1399 = ({ wv t1397 = v801_k; wv t1398 = rt_get(&F_args, v794_e); rt_idx(t1397, t1398); }); wv t1400 = v795_scope; wv t1401 = v796_f; rt_chk(s685_aexpr(up, t1399, t1400, t1401), "aexpr"); });
      v801_k = rt_add(v801_k, rt_i(1));
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_callee, v794_e)), rt_s(&S213)))) {
      ({ wv t1404 = rt_get(&F_d, ({ wv t1402 = v795_scope; wv t1403 = rt_get(&F_name, rt_get(&F_callee, v794_e)); rt_chk(s676_find(up, t1402, t1403), "aexpr"); })); wv t1405 = rt_get(&F_pos, rt_get(&F_callee, v794_e)); wv t1406 = rt_def(up->v668_curSlot, "curSlot"); rt_set_idx(t1404, t1405, t1406); });
    }
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S185)))) {
    wv v802_k = rt_i(0);
    while (rt_cond(({ wv t1407 = v802_k; wv t1408 = std_length(rt_get(&F_branches, v794_e)); rt_lt(t1407, t1408); }))) {
      (void)({ wv t1411 = rt_get(&F_cond, ({ wv t1409 = v802_k; wv t1410 = rt_get(&F_branches, v794_e); rt_idx(t1409, t1410); })); wv t1412 = v795_scope; wv t1413 = v796_f; rt_chk(s685_aexpr(up, t1411, t1412, t1413), "aexpr"); });
      (void)({ wv t1416 = rt_get(&F_body, ({ wv t1414 = v802_k; wv t1415 = rt_get(&F_branches, v794_e); rt_idx(t1414, t1415); })); wv t1417 = rt_chk(s674_newScope(up, v795_scope), "aexpr"); wv t1418 = v796_f; rt_chk(s680_ablock(up, t1416, t1417, t1418, RT_NULL), "aexpr"); });
      v802_k = rt_add(v802_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v794_e), RT_NULL))) {
      (void)({ wv t1419 = rt_get(&F_els, v794_e); wv t1420 = rt_chk(s674_newScope(up, v795_scope), "aexpr"); wv t1421 = v796_f; rt_chk(s680_ablock(up, t1419, t1420, t1421, RT_NULL), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v797_kind, rt_s(&S44)))) {
    (void)({ wv t1422 = rt_get(&F_subject, v794_e); wv t1423 = v795_scope; wv t1424 = v796_f; rt_chk(s685_aexpr(up, t1422, t1423, t1424), "aexpr"); });
    wv v803_k = rt_i(0);
    while (rt_cond(({ wv t1425 = v803_k; wv t1426 = std_length(rt_get(&F_cases, v794_e)); rt_lt(t1425, t1426); }))) {
      (void)({ wv t1429 = rt_get(&F_body, ({ wv t1427 = v803_k; wv t1428 = rt_get(&F_cases, v794_e); rt_idx(t1427, t1428); })); wv t1430 = rt_chk(s674_newScope(up, v795_scope), "aexpr"); wv t1431 = v796_f; rt_chk(s680_ablock(up, t1429, t1430, t1431, RT_NULL), "aexpr"); });
      v803_k = rt_add(v803_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v794_e), RT_NULL))) {
      (void)({ wv t1432 = rt_get(&F_els, v794_e); wv t1433 = rt_chk(s674_newScope(up, v795_scope), "aexpr"); wv t1434 = v796_f; rt_chk(s680_ablock(up, t1432, t1433, t1434, RT_NULL), "aexpr"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv s686_analyze(struct fr142 *up, wv v804_m) {
  rt_enter("analyze");
  v804_m = rt_bind(v804_m, 0);
  wv t1435 = rt_bind(v804_m, 1);
  (void)rt_def(up->v666_curMod, "curMod");
  up->v666_curMod = t1435;
  wv t1436 = rt_bind(rt_chk(s672_modId(up, v804_m), "analyze"), 1);
  (void)rt_def(up->v667_mod, "mod");
  up->v667_mod = t1436;
  wv v805_top = rt_bind(rt_chk(s678_newFn(up, RT_NULL, RT_NULL), "analyze"), 0);
  (void)({ wv t1437 = rt_def(up->v664_tops, "tops"); wv t1438 = v805_top; std_push(t1437, t1438); });
  wv v806_slot = rt_arr(0, NULL, 1);
  wv v807_i = rt_i(0);
  while (rt_cond(({ wv t1439 = v807_i; wv t1440 = std_length(rt_get(&F_chars, rt_idx(v804_m, up->v647_modules))); rt_le(t1439, t1440); }))) {
    (void)std_push(v806_slot, RT_NULL);
    v807_i = rt_add(v807_i, rt_i(1));
  }
  (void)({ wv t1441 = rt_def(up->v665_slots, "slots"); wv t1442 = v806_slot; std_push(t1441, t1442); });
  wv t1443 = rt_bind(v806_slot, 1);
  (void)rt_def(up->v668_curSlot, "curSlot");
  up->v668_curSlot = t1443;
  wv v808_scope = rt_bind(rt_chk(s674_newScope(up, RT_NULL), "analyze"), 0);
  wv v809_imports = rt_bind(rt_get(&F_imports, rt_idx(v804_m, up->v647_modules)), 0);
  wv v810_k = rt_i(0);
  while (rt_cond(({ wv t1444 = v810_k; wv t1445 = std_length(v809_imports); rt_lt(t1444, t1445); }))) {
    (void)({ wv t1446 = v808_scope; wv t1447 = rt_get(&F_name, rt_get(&F_node, rt_idx(v810_k, v809_imports))); rt_chk(s675_put(up, t1446, t1447, rt_s(&S418), RT_NULL), "analyze"); });
    v810_k = rt_add(v810_k, rt_i(1));
  }
  (void)({ wv t1448 = rt_rec(0, 2, K48, (wv[]){ rt_get(&F_stmts, rt_get(&F_program, rt_idx(v804_m, up->v647_modules))), RT_NULL }); wv t1449 = v808_scope; wv t1450 = v805_top; wv t1451 = v808_scope; rt_chk(s680_ablock(up, t1448, t1449, t1450, t1451), "analyze"); });
  RT_RET(RT_NULL);
}

static wv s687_enter(struct fr142 *up, wv v811_m) {
  rt_enter("enter");
  v811_m = rt_bind(v811_m, 0);
  (void)({ wv t1452 = rt_get(&F_file, rt_idx(v811_m, up->v647_modules)); wv t1453 = rt_get(&F_chars, rt_idx(v811_m, up->v647_modules)); rt_chk(w2_setSource(t1452, t1453), "enter"); });
  wv t1454 = rt_bind(v811_m, 1);
  (void)rt_def(up->v666_curMod, "curMod");
  up->v666_curMod = t1454;
  wv t1455 = rt_bind(rt_chk(s672_modId(up, v811_m), "enter"), 1);
  (void)rt_def(up->v667_mod, "mod");
  up->v667_mod = t1455;
  wv t1458 = rt_bind(({ wv t1456 = v811_m; wv t1457 = rt_def(up->v665_slots, "slots"); rt_idx(t1456, t1457); }), 1);
  (void)rt_def(up->v668_curSlot, "curSlot");
  up->v668_curSlot = t1458;
  wv t1461 = rt_bind(({ wv t1459 = v811_m; wv t1460 = rt_def(up->v664_tops, "tops"); rt_idx(t1459, t1460); }), 1);
  (void)rt_def(up->v671_cur, "cur");
  up->v671_cur = t1461;
  wv t1462 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v669_stolen, "stolen");
  up->v669_stolen = t1462;
  wv t1463 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v670_ownEnums, "ownEnums");
  up->v670_ownEnums = t1463;
  wv v812_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v811_m, up->v647_modules))), 0);
  wv v813_k = rt_i(0);
  while (rt_cond(({ wv t1464 = v813_k; wv t1465 = std_length(v812_list); rt_lt(t1464, t1465); }))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v813_k, v812_list)), rt_s(&S46)))) {
      (void)({ wv t1466 = rt_def(up->v670_ownEnums, "ownEnums"); wv t1467 = rt_idx(v813_k, v812_list); std_push(t1466, t1467); });
    }
    v813_k = rt_add(v813_k, rt_i(1));
  }
  wv v814_imports = rt_bind(rt_get(&F_imports, rt_idx(v811_m, up->v647_modules)), 0);
  v813_k = rt_i(0);
  while (rt_cond(({ wv t1468 = v813_k; wv t1469 = std_length(v814_imports); rt_lt(t1468, t1469); }))) {
    (void)({ wv t1472 = rt_def(up->v669_stolen, "stolen"); wv t1473 = ({ wv t1470 = rt_get(&F_name, rt_get(&F_node, rt_idx(v813_k, v814_imports))); wv t1471 = rt_get(&F_lib, rt_idx(v813_k, v814_imports)); rt_rec(0, 2, K52, (wv[]){ t1470, t1471 }); }); std_push(t1472, t1473); });
    v813_k = rt_add(v813_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s688_emit(struct fr142 *up, wv v815_line) {
  rt_enter("emit");
  v815_line = rt_bind(v815_line, 0);
  (void)({ wv t1476 = rt_def(up->v658_out, "out"); wv t1477 = ({ wv t1474 = rt_chk(w6_repeat(rt_s(&S517), rt_def(up->v659_indent, "indent")), "emit"); wv t1475 = v815_line; rt_add(t1474, t1475); }); std_push(t1476, t1477); });
  RT_RET(RT_NULL);
}

static wv s689_tmp(struct fr142 *up) {
  rt_enter("tmp");
  wv v816_name = rt_cat(2, (wv[]){ rt_s(&S104), rt_text(rt_def(up->v650_tmps, "tmps")) });
  up->v650_tmps = rt_add(rt_def(up->v650_tmps, "tmps"), rt_i(1));
  RT_RET(v816_name);
}

static wv s690_str(struct fr142 *up, wv v817_s) {
  rt_enter("str");
  v817_s = rt_bind(v817_s, 0);
  wv v818_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1478 = v818_k; wv t1479 = std_length(rt_def(up->v651_strings, "strings")); rt_lt(t1478, t1479); }), "&&") && rt_bool(({ wv t1482 = ({ wv t1480 = v818_k; wv t1481 = rt_def(up->v651_strings, "strings"); rt_idx(t1480, t1481); }); wv t1483 = v817_s; rt_ne(t1482, t1483); }), "&&")))) {
    v818_k = rt_add(v818_k, rt_i(1));
  }
  if (rt_cond(({ wv t1484 = v818_k; wv t1485 = std_length(rt_def(up->v651_strings, "strings")); rt_eq(t1484, t1485); }))) {
    (void)({ wv t1486 = rt_def(up->v651_strings, "strings"); wv t1487 = v817_s; std_push(t1486, t1487); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S518), rt_text(v818_k) }));
}

static wv s691_fieldRef(struct fr142 *up, wv v819_name) {
  rt_enter("fieldRef");
  v819_name = rt_bind(v819_name, 0);
  if (rt_cond(rt_not(({ wv t1488 = rt_def(up->v652_fields, "fields"); wv t1489 = v819_name; rt_chk(w1_contains(t1488, t1489), "fieldRef"); })))) {
    (void)({ wv t1490 = rt_def(up->v652_fields, "fields"); wv t1491 = v819_name; std_push(t1490, t1491); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S519), rt_text(v819_name) }));
}

static wv s692_enumDef(struct fr142 *up, wv v820_s, wv v821_m) {
  rt_enter("enumDef");
  v820_s = rt_bind(v820_s, 0);
  v821_m = rt_bind(v821_m, 0);
  wv v822_nm = rt_bind(rt_chk(s690_str(up, rt_get(&F_name, v820_s)), "enumDef"), 0);
  wv v823_members = rt_arr(0, NULL, 1);
  wv v824_k = rt_i(0);
  while (rt_cond(({ wv t1492 = v824_k; wv t1493 = std_length(rt_get(&F_members, v820_s)); rt_lt(t1492, t1493); }))) {
    (void)({ wv t1498 = v823_members; wv t1499 = ({ wv t1496 = rt_text(v822_nm); wv t1497 = rt_text(rt_chk(s690_str(up, rt_get(&F_name, ({ wv t1494 = v824_k; wv t1495 = rt_get(&F_members, v820_s); rt_idx(t1494, t1495); }))), "enumDef")); rt_cat(5, (wv[]){ rt_s(&S510), t1496, rt_s(&S415), t1497, rt_s(&S512) }); }); std_push(t1498, t1499); });
    v824_k = rt_add(v824_k, rt_i(1));
  }
  (void)({ wv t1503 = rt_def(up->v654_enumDefs, "enumDefs"); wv t1504 = ({ wv t1500 = rt_text(v821_m); wv t1501 = rt_text(rt_get(&F_name, v820_s)); wv t1502 = rt_text(std_join(v823_members, rt_s(&S415))); rt_cat(7, (wv[]){ rt_s(&S520), t1500, rt_s(&S12), t1501, rt_s(&S521), t1502, rt_s(&S522) }); }); std_push(t1503, t1504); });
  RT_RET(RT_NULL);
}

static wv s693_stolenFrom(struct fr142 *up, wv v825_x) {
  rt_enter("stolenFrom");
  if (rt_cond(rt_ne(rt_get(&F_k, v825_x), rt_s(&S213)))) {
    RT_RET(RT_NULL);
  }
  wv v826_k = rt_i(0);
  while (rt_cond(({ wv t1505 = v826_k; wv t1506 = std_length(rt_def(up->v669_stolen, "stolen")); rt_lt(t1505, t1506); }))) {
    if (rt_cond(({ wv t1509 = rt_get(&F_name, ({ wv t1507 = v826_k; wv t1508 = rt_def(up->v669_stolen, "stolen"); rt_idx(t1507, t1508); })); wv t1510 = rt_get(&F_name, v825_x); rt_eq(t1509, t1510); }))) {
      RT_RET(rt_get(&F_lib, ({ wv t1511 = v826_k; wv t1512 = rt_def(up->v669_stolen, "stolen"); rt_idx(t1511, t1512); })));
    }
    v826_k = rt_add(v826_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s694_enumIn(struct fr142 *up, wv v827_list, wv v828_name) {
  rt_enter("enumIn");
  v828_name = rt_bind(v828_name, 0);
  wv v829_k = rt_i(0);
  while (rt_cond(({ wv t1513 = v829_k; wv t1514 = std_length(v827_list); rt_lt(t1513, t1514); }))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_idx(v829_k, v827_list)), rt_s(&S46)), "&&") && rt_bool(({ wv t1515 = rt_get(&F_name, rt_idx(v829_k, v827_list)); wv t1516 = v828_name; rt_eq(t1515, t1516); }), "&&")))) {
      RT_RET(rt_idx(v829_k, v827_list));
    }
    v829_k = rt_add(v829_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s695_enumOf(struct fr142 *up, wv v830_x) {
  rt_enter("enumOf");
  if (rt_cond(rt_eq(rt_get(&F_k, v830_x), rt_s(&S213)))) {
    wv v834_s = rt_bind(({ wv t1517 = rt_def(up->v670_ownEnums, "ownEnums"); wv t1518 = rt_get(&F_name, v830_x); rt_chk(s694_enumIn(up, t1517, t1518), "enumOf"); }), 0);
    RT_RET((rt_cond(rt_eq(v834_s, RT_NULL)) ? RT_NULL : ({ wv t1521 = ({ wv t1519 = rt_text(rt_def(up->v667_mod, "mod")); wv t1520 = rt_text(rt_get(&F_name, v830_x)); rt_cat(4, (wv[]){ rt_s(&S523), t1519, rt_s(&S12), t1520 }); }); wv t1522 = v834_s; rt_rec(0, 2, K53, (wv[]){ t1521, t1522 }); })));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v830_x), rt_s(&S191)))) {
    RT_RET(RT_NULL);
  }
  wv v831_lib = rt_bind(rt_chk(s693_stolenFrom(up, rt_get(&F_rec, v830_x)), "enumOf"), 0);
  if (rt_cond(rt_eq(v831_lib, RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v832_xs = rt_bind(rt_get(&F_exports, rt_idx(v831_lib, up->v647_modules)), 0);
  wv v833_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1523 = v833_k; wv t1524 = std_length(v832_xs); rt_lt(t1523, t1524); }), "&&") && rt_bool(({ wv t1525 = rt_get(&F_name, rt_idx(v833_k, v832_xs)); wv t1526 = rt_get(&F_field, v830_x); rt_ne(t1525, t1526); }), "&&")))) {
    v833_k = rt_add(v833_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1527 = v833_k; wv t1528 = std_length(v832_xs); rt_eq(t1527, t1528); }), "||") || rt_bool(rt_ne(rt_get(&F_kind, rt_idx(v833_k, v832_xs)), rt_s(&S46)), "||")))) {
    RT_RET(RT_NULL);
  }
  RT_RET(({ wv t1533 = ({ wv t1529 = rt_text(rt_chk(s672_modId(up, v831_lib), "enumOf")); wv t1530 = rt_text(rt_get(&F_field, v830_x)); rt_cat(4, (wv[]){ rt_s(&S523), t1529, rt_s(&S12), t1530 }); }); wv t1534 = ({ wv t1531 = rt_get(&F_stmts, rt_get(&F_program, rt_idx(v831_lib, up->v647_modules))); wv t1532 = rt_get(&F_field, v830_x); rt_chk(s694_enumIn(up, t1531, t1532), "enumOf"); }); rt_rec(0, 2, K53, (wv[]){ t1533, t1534 }); }));
}

static wv s696_place(struct fr142 *up, wv v835_d) {
  rt_enter("place");
  v835_d = rt_bind(v835_d, 0);
  wv v836_owner = rt_bind(rt_get(&F_func, ({ wv t1535 = v835_d; wv t1536 = rt_def(up->v662_decls, "decls"); rt_idx(t1535, t1536); })), 0);
  if (rt_cond(rt_get(&F_top, ({ wv t1537 = v836_owner; wv t1538 = rt_def(up->v663_fns, "fns"); rt_idx(t1537, t1538); })))) {
    RT_RET(rt_get(&F_cname, ({ wv t1539 = v835_d; wv t1540 = rt_def(up->v662_decls, "decls"); rt_idx(t1539, t1540); })));
  }
  if (rt_cond(({ wv t1541 = v836_owner; wv t1542 = rt_def(up->v671_cur, "cur"); rt_eq(t1541, t1542); }))) {
    RT_RET((rt_cond(rt_get(&F_captured, ({ wv t1543 = v835_d; wv t1544 = rt_def(up->v662_decls, "decls"); rt_idx(t1543, t1544); }))) ? rt_cat(2, (wv[]){ rt_s(&S524), rt_text(rt_get(&F_cname, ({ wv t1545 = v835_d; wv t1546 = rt_def(up->v662_decls, "decls"); rt_idx(t1545, t1546); }))) }) : rt_get(&F_cname, ({ wv t1547 = v835_d; wv t1548 = rt_def(up->v662_decls, "decls"); rt_idx(t1547, t1548); }))));
  }
  RT_RET(({ wv t1557 = rt_text(rt_chk(w6_repeat(rt_s(&S526), rt_sub(({ wv t1553 = rt_get(&F_depth, ({ wv t1549 = rt_def(up->v671_cur, "cur"); wv t1550 = rt_def(up->v663_fns, "fns"); rt_idx(t1549, t1550); })); wv t1554 = rt_get(&F_depth, ({ wv t1551 = v836_owner; wv t1552 = rt_def(up->v663_fns, "fns"); rt_idx(t1551, t1552); })); rt_sub(t1553, t1554); }), rt_i(1))), "place")); wv t1558 = rt_text(rt_get(&F_cname, ({ wv t1555 = v835_d; wv t1556 = rt_def(up->v662_decls, "decls"); rt_idx(t1555, t1556); }))); rt_cat(4, (wv[]){ rt_s(&S525), t1557, rt_s(&S527), t1558 }); }));
}

static wv s697_tdz(struct fr142 *up, wv v837_d) {
  rt_enter("tdz");
  v837_d = rt_bind(v837_d, 0);
  RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_kind, ({ wv t1559 = v837_d; wv t1560 = rt_def(up->v662_decls, "decls"); rt_idx(t1559, t1560); })), rt_s(&S178)), "&&") && rt_bool(({ wv t1563 = rt_get(&F_func, ({ wv t1561 = v837_d; wv t1562 = rt_def(up->v662_decls, "decls"); rt_idx(t1561, t1562); })); wv t1564 = rt_def(up->v671_cur, "cur"); rt_ne(t1563, t1564); }), "&&")));
}

static wv s698_read(struct fr142 *up, wv v838_d) {
  rt_enter("read");
  v838_d = rt_bind(v838_d, 0);
  RT_RET((rt_cond(rt_chk(s697_tdz(up, v838_d), "read")) ? ({ wv t1569 = ({ wv t1567 = rt_text(rt_chk(s696_place(up, v838_d), "read")); wv t1568 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1565 = v838_d; wv t1566 = rt_def(up->v662_decls, "decls"); rt_idx(t1565, t1566); }))), "read")); rt_cat(5, (wv[]){ rt_s(&S528), t1567, rt_s(&S415), t1568, rt_s(&S16) }); }); wv t1570 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1569, t1570 }); }) : ({ wv t1571 = rt_chk(s696_place(up, v838_d), "read"); wv t1572 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K54, (wv[]){ t1571, t1572 }); })));
}

static wv s699_seq(struct fr142 *up, wv v839_args) {
  rt_enter("seq");
  v839_args = rt_bind(v839_args, 0);
  wv v840_effects = rt_i(0);
  wv v841_anyPure = rt_b(0);
  wv v842_k = rt_i(0);
  while (rt_cond(({ wv t1573 = v842_k; wv t1574 = std_length(v839_args); rt_lt(t1573, t1574); }))) {
    if (rt_cond(({ wv t1575 = rt_get(&F_k, rt_idx(v842_k, v839_args)); wv t1576 = rt_def(w6_effectK, "effectK"); rt_eq(t1575, t1576); }))) {
      v840_effects = rt_add(v840_effects, rt_i(1));
    }
    if (rt_cond(({ wv t1577 = rt_get(&F_k, rt_idx(v842_k, v839_args)); wv t1578 = rt_def(w6_pureK, "pureK"); rt_eq(t1577, t1578); }))) {
      v841_anyPure = rt_b(1);
    }
    v842_k = rt_add(v842_k, rt_i(1));
  }
  wv v843_cs = rt_arr(0, NULL, 1);
  if (rt_cond(rt_b(rt_bool(rt_eq(v840_effects, rt_i(0)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v840_effects, rt_i(1)), "&&") && rt_bool(rt_not(v841_anyPure), "&&")), "||")))) {
    v842_k = rt_i(0);
    while (rt_cond(({ wv t1579 = v842_k; wv t1580 = std_length(v839_args); rt_lt(t1579, t1580); }))) {
      (void)({ wv t1581 = v843_cs; wv t1582 = rt_get(&F_c, rt_idx(v842_k, v839_args)); std_push(t1581, t1582); });
      v842_k = rt_add(v842_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S13), v843_cs }));
  }
  wv v844_pre = rt_arr(0, NULL, 1);
  v842_k = rt_i(0);
  while (rt_cond(({ wv t1583 = v842_k; wv t1584 = std_length(v839_args); rt_lt(t1583, t1584); }))) {
    if (rt_cond(({ wv t1585 = rt_get(&F_k, rt_idx(v842_k, v839_args)); wv t1586 = rt_def(w6_constK, "constK"); rt_eq(t1585, t1586); }))) {
      (void)({ wv t1587 = v843_cs; wv t1588 = rt_get(&F_c, rt_idx(v842_k, v839_args)); std_push(t1587, t1588); });
    } else {
      wv v845_temp = rt_bind(rt_chk(s689_tmp(up), "seq"), 0);
      (void)({ wv t1591 = v844_pre; wv t1592 = ({ wv t1589 = rt_text(v845_temp); wv t1590 = rt_text(rt_get(&F_c, rt_idx(v842_k, v839_args))); rt_cat(5, (wv[]){ rt_s(&S529), t1589, rt_s(&S530), t1590, rt_s(&S14) }); }); std_push(t1591, t1592); });
      (void)std_push(v843_cs, v845_temp);
    }
    v842_k = rt_add(v842_k, rt_i(1));
  }
  RT_RET(({ wv t1593 = std_join(v844_pre, rt_s(&S0)); wv t1594 = v843_cs; rt_rec(0, 2, K55, (wv[]){ t1593, t1594 }); }));
}

static wv s700_wrap(struct fr142 *up, wv v846_sq, wv v847_inner) {
  rt_enter("wrap");
  v846_sq = rt_bind(v846_sq, 0);
  v847_inner = rt_bind(v847_inner, 0);
  RT_RET(({ wv t1597 = (rt_cond(rt_eq(rt_get(&F_pre, v846_sq), rt_s(&S13))) ? v847_inner : ({ wv t1595 = rt_text(rt_get(&F_pre, v846_sq)); wv t1596 = rt_text(v847_inner); rt_cat(5, (wv[]){ rt_s(&S531), t1595, rt_s(&S0), t1596, rt_s(&S532) }); })); wv t1598 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1597, t1598 }); }));
}

static wv s701_stmts(struct fr142 *up, wv v848_list) {
  rt_enter("stmts");
  v848_list = rt_bind(v848_list, 0);
  wv v849_k = rt_i(0);
  while (rt_cond(({ wv t1599 = v849_k; wv t1600 = std_length(v848_list); rt_lt(t1599, t1600); }))) {
    (void)rt_chk(s705_stmt(up, rt_idx(v849_k, v848_list)), "stmts");
    v849_k = rt_add(v849_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s702_resets(struct fr142 *up, wv v850_b) {
  rt_enter("resets");
  wv v851_k = rt_i(0);
  while (rt_cond(({ wv t1601 = v851_k; wv t1602 = std_length(rt_get(&F_stmts, v850_b)); rt_lt(t1601, t1602); }))) {
    wv v852_s = ({ wv t1603 = v851_k; wv t1604 = rt_get(&F_stmts, v850_b); rt_idx(t1603, t1604); });
    if (rt_cond(rt_eq(rt_get(&F_k, v852_s), rt_s(&S178)))) {
      wv v853_d = rt_bind(({ wv t1605 = rt_get(&F_namePos, v852_s); wv t1606 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1605, t1606); }), 0);
      if (rt_cond(rt_get(&F_captured, ({ wv t1607 = v853_d; wv t1608 = rt_def(up->v662_decls, "decls"); rt_idx(t1607, t1608); })))) {
        (void)rt_chk(s688_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s696_place(up, v853_d), "resets")), rt_s(&S533) })), "resets");
      }
    }
    v851_k = rt_add(v851_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s703_blockBody(struct fr142 *up, wv v854_b) {
  rt_enter("blockBody");
  v854_b = rt_bind(v854_b, 0);
  (void)rt_chk(s702_resets(up, v854_b), "blockBody");
  (void)rt_chk(s701_stmts(up, rt_get(&F_stmts, v854_b)), "blockBody");
  if (rt_cond(rt_ne(rt_get(&F_value, v854_b), RT_NULL))) {
    (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S534), rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_value, v854_b)), "blockBody"))), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s704_valueOf(struct fr142 *up, wv v855_b) {
  rt_enter("valueOf");
  v855_b = rt_bind(v855_b, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_value, v855_b), RT_NULL)) ? rt_s(&S535) : rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_value, v855_b)), "valueOf"))));
}

static wv s705_stmt(struct fr142 *up, wv v856_s) {
  rt_enter("stmt");
  if (rt_cond(rt_ne(rt_get(&F_handler, v856_s), RT_NULL))) {
    (void)rt_chk(s715_onfailStmt(up, v856_s), "stmt");
    RT_RET(RT_NULL);
  }
  wv v857_kind = rt_bind(rt_get(&F_k, v856_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v857_kind, rt_s(&S202)), "||") || rt_bool(rt_eq(v857_kind, rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(v857_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v857_kind, rt_s(&S205)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S33)))) {
    (void)rt_chk(s708_fnDef(up, v856_s), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S34)))) {
    if (rt_cond(rt_def(up->v661_iife, "iife"))) {
      (void)rt_chk(w2_abort(rt_get(&F_pos, v856_s), rt_s(&S536)), "stmt");
    }
    (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S537), rt_text((rt_cond(rt_eq(rt_get(&F_e, v856_s), RT_NULL)) ? rt_s(&S535) : rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_e, v856_s)), "stmt")))), rt_s(&S538) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S57)))) {
    (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S539), rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_e, v856_s)), "stmt"))), rt_s(&S538) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v857_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v857_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_def(up->v660_loops, "loops"), rt_i(0)))) {
      (void)({ wv t1609 = rt_get(&F_pos, v856_s); wv t1610 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text((rt_cond(rt_eq(v857_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43))), rt_s(&S540) }); rt_chk(w2_abort(t1609, t1610), "stmt"); });
    }
    (void)rt_chk(s688_emit(up, (rt_cond(rt_eq(v857_kind, rt_s(&S42))) ? rt_s(&S541) : rt_s(&S542))), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S41)))) {
    (void)rt_chk(s688_emit(up, ({ wv t1611 = rt_text((rt_cond(rt_get(&F_neg, v856_s)) ? rt_s(&S94) : rt_s(&S13))); wv t1612 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_cond, v856_s)), "stmt"))); rt_cat(5, (wv[]){ rt_s(&S543), t1611, rt_s(&S544), t1612, rt_s(&S545) }); })), "stmt");
    up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
    up->v660_loops = rt_add(rt_def(up->v660_loops, "loops"), rt_i(1));
    (void)rt_chk(s703_blockBody(up, rt_get(&F_body, v856_s)), "stmt");
    up->v660_loops = rt_sub(rt_def(up->v660_loops, "loops"), rt_i(1));
    up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
    (void)rt_chk(s688_emit(up, rt_s(&S15)), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S55)))) {
    (void)rt_chk(s706_signal(up, rt_cat(3, (wv[]){ rt_s(&S546), rt_text(rt_get(&F_code, v856_s)), rt_s(&S16) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S54)))) {
    (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S547), rt_text((rt_cond(rt_eq(rt_get(&F_e, v856_s), RT_NULL)) ? rt_s(&S548) : rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_e, v856_s)), "stmt")))), rt_s(&S538) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S177)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v856_s)), rt_s(&S185)))) {
      (void)rt_chk(s716_chainStmt(up, rt_get(&F_e, v856_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v856_s)), rt_s(&S44)))) {
      (void)rt_chk(s727_matchStmt(up, rt_get(&F_e, v856_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v856_s)), rt_s(&S184)))) {
      (void)rt_chk(s706_signal(up, rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_arg, rt_get(&F_e, v856_s))), "stmt"))), "stmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S534), rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_e, v856_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S178)))) {
    (void)({ wv t1617 = ({ wv t1613 = rt_get(&F_namePos, v856_s); wv t1614 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1613, t1614); }); wv t1618 = ({ wv t1615 = rt_get(&F_e, v856_s); wv t1616 = rt_chk(w6_modeOf(v856_s), "stmt"); rt_chk(s709_bindRhs(up, t1615, t1616), "stmt"); }); rt_chk(s707_bindDecl(up, t1617, t1618), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v857_kind, rt_s(&S38)))) {
    (void)rt_chk(s710_setStmt(up, v856_s), "stmt");
    RT_RET(RT_NULL);
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S549), rt_text(v857_kind) }));
  RT_RET(RT_NULL);
}

static wv s706_signal(struct fr142 *up, wv v858_code) {
  rt_enter("signal");
  v858_code = rt_bind(v858_code, 0);
  (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S550), rt_text(v858_code), rt_s(&S538) })), "signal");
  (void)rt_chk(s688_emit(up, rt_s(&S551)), "signal");
  RT_RET(RT_NULL);
}

static wv s707_bindDecl(struct fr142 *up, wv v859_d, wv v860_value) {
  rt_enter("bindDecl");
  v859_d = rt_bind(v859_d, 0);
  v860_value = rt_bind(v860_value, 0);
  wv v861_owner = rt_bind(rt_get(&F_func, ({ wv t1619 = v859_d; wv t1620 = rt_def(up->v662_decls, "decls"); rt_idx(t1619, t1620); })), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1621 = v861_owner; wv t1622 = rt_def(up->v663_fns, "fns"); rt_idx(t1621, t1622); }))), "&&") && rt_bool(({ wv t1623 = v861_owner; wv t1624 = rt_def(up->v671_cur, "cur"); rt_eq(t1623, t1624); }), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_captured, ({ wv t1625 = v859_d; wv t1626 = rt_def(up->v662_decls, "decls"); rt_idx(t1625, t1626); }))), "&&")))) {
    (void)rt_chk(s688_emit(up, ({ wv t1629 = rt_text(rt_get(&F_cname, ({ wv t1627 = v859_d; wv t1628 = rt_def(up->v662_decls, "decls"); rt_idx(t1627, t1628); }))); wv t1630 = rt_text(v860_value); rt_cat(5, (wv[]){ rt_s(&S529), t1629, rt_s(&S530), t1630, rt_s(&S14) }); })), "bindDecl");
  } else {
    (void)rt_chk(s688_emit(up, ({ wv t1631 = rt_text(rt_chk(s696_place(up, v859_d), "bindDecl")); wv t1632 = rt_text(v860_value); rt_cat(4, (wv[]){ t1631, rt_s(&S530), t1632, rt_s(&S14) }); })), "bindDecl");
  }
  RT_RET(RT_NULL);
}

static wv s708_fnDef(struct fr142 *up, wv v862_s) {
  rt_enter("fnDef");
  v862_s = rt_bind(v862_s, 0);
  wv v863_d = rt_bind(({ wv t1633 = rt_get(&F_namePos, v862_s); wv t1634 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1633, t1634); }), 0);
  wv v864_f = rt_bind(rt_get(&F_fnIndex, ({ wv t1635 = v863_d; wv t1636 = rt_def(up->v662_decls, "decls"); rt_idx(t1635, t1636); })), 0);
  wv v865_pf = rt_bind(rt_get(&F_parent, ({ wv t1637 = v864_f; wv t1638 = rt_def(up->v663_fns, "fns"); rt_idx(t1637, t1638); })), 0);
  wv v866_hasUp = rt_not(rt_get(&F_top, ({ wv t1639 = v865_pf; wv t1640 = rt_def(up->v663_fns, "fns"); rt_idx(t1639, t1640); })));
  wv v867_params = rt_arr(0, NULL, 1);
  if (rt_cond(v866_hasUp)) {
    (void)({ wv t1641 = v867_params; wv t1642 = rt_cat(3, (wv[]){ rt_s(&S552), rt_text(v865_pf), rt_s(&S553) }); std_push(t1641, t1642); });
  }
  wv v868_k = rt_i(0);
  while (rt_cond(({ wv t1643 = v868_k; wv t1644 = std_length(rt_get(&F_params, v862_s)); rt_lt(t1643, t1644); }))) {
    wv v876_pd = rt_bind(({ wv t1647 = rt_get(&F_pos, ({ wv t1645 = v868_k; wv t1646 = rt_get(&F_params, v862_s); rt_idx(t1645, t1646); })); wv t1648 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1647, t1648); }), 0);
    (void)({ wv t1655 = v867_params; wv t1656 = ({ wv t1653 = rt_text((rt_cond(rt_get(&F_captured, ({ wv t1649 = v876_pd; wv t1650 = rt_def(up->v662_decls, "decls"); rt_idx(t1649, t1650); }))) ? rt_s(&S554) : rt_s(&S13))); wv t1654 = rt_text(rt_get(&F_cname, ({ wv t1651 = v876_pd; wv t1652 = rt_def(up->v662_decls, "decls"); rt_idx(t1651, t1652); }))); rt_cat(3, (wv[]){ rt_s(&S529), t1653, t1654 }); }); std_push(t1655, t1656); });
    v868_k = rt_add(v868_k, rt_i(1));
  }
  wv v869_proto = ({ wv t1659 = rt_text(rt_get(&F_cname, ({ wv t1657 = v864_f; wv t1658 = rt_def(up->v663_fns, "fns"); rt_idx(t1657, t1658); }))); wv t1660 = rt_text((rt_cond(rt_eq(std_length(v867_params), rt_i(0))) ? rt_s(&S556) : std_join(v867_params, rt_s(&S415)))); rt_cat(5, (wv[]){ rt_s(&S555), t1659, rt_s(&S97), t1660, rt_s(&S16) }); });
  wv v870_entry = ({ wv t1661 = v869_proto; wv t1662 = rt_arr(0, NULL, 0); rt_rec(1, 2, K56, (wv[]){ t1661, t1662 }); });
  (void)({ wv t1663 = rt_def(up->v657_functions, "functions"); wv t1664 = v870_entry; std_push(t1663, t1664); });
  if (rt_cond(rt_chk(s673_hasFrame(up, v864_f), "fnDef"))) {
    (void)({ wv t1665 = rt_def(up->v656_frames, "frames"); wv t1666 = v864_f; std_push(t1665, t1666); });
  }
  wv v871_start = rt_bind(std_length(rt_def(up->v658_out, "out")), 0);
  wv v872_savedIndent = rt_bind(rt_def(up->v659_indent, "indent"), 0);
  wv v873_savedLoops = rt_bind(rt_def(up->v660_loops, "loops"), 0);
  wv v874_savedIife = rt_bind(rt_def(up->v661_iife, "iife"), 0);
  wv v875_savedCur = rt_bind(rt_def(up->v671_cur, "cur"), 0);
  wv t1667 = rt_i(1);
  (void)rt_def(up->v659_indent, "indent");
  up->v659_indent = t1667;
  wv t1668 = rt_i(0);
  (void)rt_def(up->v660_loops, "loops");
  up->v660_loops = t1668;
  wv t1669 = rt_b(0);
  (void)rt_def(up->v661_iife, "iife");
  up->v661_iife = t1669;
  wv t1670 = rt_bind(v864_f, 1);
  (void)rt_def(up->v671_cur, "cur");
  up->v671_cur = t1670;
  (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S557), rt_text(rt_chk(w1_quote(rt_get(&F_name, v862_s)), "fnDef")), rt_s(&S538) })), "fnDef");
  if (rt_cond(rt_chk(s673_hasFrame(up, v864_f), "fnDef"))) {
    (void)rt_chk(s688_emit(up, ({ wv t1671 = rt_text(v864_f); wv t1672 = rt_text((rt_cond(v866_hasUp) ? rt_s(&S559) : rt_s(&S4))); rt_cat(5, (wv[]){ rt_s(&S552), t1671, rt_s(&S558), t1672, rt_s(&S522) }); })), "fnDef");
  }
  v868_k = rt_i(0);
  while (rt_cond(({ wv t1673 = v868_k; wv t1674 = std_length(rt_get(&F_params, v862_s)); rt_lt(t1673, t1674); }))) {
    wv v877_p = rt_bind(({ wv t1675 = v868_k; wv t1676 = rt_get(&F_params, v862_s); rt_idx(t1675, t1676); }), 0);
    wv v878_pd = rt_bind(({ wv t1677 = rt_get(&F_pos, v877_p); wv t1678 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1677, t1678); }), 0);
    wv v879_cn = rt_bind(rt_get(&F_cname, ({ wv t1679 = v878_pd; wv t1680 = rt_def(up->v662_decls, "decls"); rt_idx(t1679, t1680); })), 0);
    wv v880_arg = rt_bind((rt_cond(rt_get(&F_captured, ({ wv t1681 = v878_pd; wv t1682 = rt_def(up->v662_decls, "decls"); rt_idx(t1681, t1682); }))) ? rt_cat(2, (wv[]){ rt_s(&S554), rt_text(v879_cn) }) : v879_cn), 0);
    wv v881_v = rt_bind((rt_cond(rt_get(&F_isRef, v877_p)) ? v880_arg : ({ wv t1683 = rt_text(v880_arg); wv t1684 = rt_text((rt_cond(rt_get(&F_isVar, v877_p)) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S560), t1683, rt_s(&S415), t1684, rt_s(&S16) }); })), 0);
    if (rt_cond(rt_get(&F_captured, ({ wv t1685 = v878_pd; wv t1686 = rt_def(up->v662_decls, "decls"); rt_idx(t1685, t1686); })))) {
      (void)rt_chk(s688_emit(up, ({ wv t1687 = rt_text(v879_cn); wv t1688 = rt_text(v881_v); rt_cat(5, (wv[]){ rt_s(&S524), t1687, rt_s(&S530), t1688, rt_s(&S14) }); })), "fnDef");
    } else if (rt_cond(rt_not(rt_get(&F_isRef, v877_p)))) {
      (void)rt_chk(s688_emit(up, ({ wv t1689 = rt_text(v879_cn); wv t1690 = rt_text(v881_v); rt_cat(4, (wv[]){ t1689, rt_s(&S530), t1690, rt_s(&S14) }); })), "fnDef");
    }
    v868_k = rt_add(v868_k, rt_i(1));
  }
  (void)rt_chk(s701_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v862_s))), "fnDef");
  (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S537), rt_text(rt_chk(s704_valueOf(up, rt_get(&F_body, v862_s)), "fnDef")), rt_s(&S538) })), "fnDef");
  ({ wv t1694 = ({ wv t1691 = rt_def(up->v658_out, "out"); wv t1692 = v871_start; wv t1693 = std_length(rt_def(up->v658_out, "out")); std_slice(t1691, t1692, t1693); }); wv t1695 = v870_entry; rt_set_mem(t1694, &F_lines, t1695); });
  while (rt_cond(({ wv t1696 = std_length(rt_def(up->v658_out, "out")); wv t1697 = v871_start; rt_gt(t1696, t1697); }))) {
    (void)std_pop(rt_def(up->v658_out, "out"));
  }
  wv t1698 = rt_bind(v872_savedIndent, 1);
  (void)rt_def(up->v659_indent, "indent");
  up->v659_indent = t1698;
  wv t1699 = rt_bind(v873_savedLoops, 1);
  (void)rt_def(up->v660_loops, "loops");
  up->v660_loops = t1699;
  wv t1700 = rt_bind(v874_savedIife, 1);
  (void)rt_def(up->v661_iife, "iife");
  up->v661_iife = t1700;
  wv t1701 = rt_bind(v875_savedCur, 1);
  (void)rt_def(up->v671_cur, "cur");
  up->v671_cur = t1701;
  RT_RET(RT_NULL);
}

static wv s709_bindRhs(struct fr142 *up, wv v882_e, wv v883_mode) {
  rt_enter("bindRhs");
  v882_e = rt_bind(v882_e, 0);
  v883_mode = rt_bind(v883_mode, 0);
  wv v884_mutable = rt_eq(v883_mode, rt_s(&S36));
  if (rt_cond(rt_eq(rt_get(&F_k, v882_e), rt_s(&S187)))) {
    RT_RET(rt_get(&F_c, rt_chk(s712_array(up, v882_e, v884_mutable), "bindRhs")));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v882_e), rt_s(&S188)))) {
    RT_RET(rt_get(&F_c, rt_chk(s713_mapLit(up, v882_e, v884_mutable), "bindRhs")));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v882_e), rt_s(&S189)))) {
    RT_RET(rt_get(&F_c, rt_chk(s714_record(up, v882_e, v884_mutable), "bindRhs")));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1702 = rt_def(w6_scalarKinds, "scalarKinds"); wv t1703 = rt_get(&F_k, v882_e); rt_chk(w1_contains(t1702, t1703), "bindRhs"); }), "||") || rt_bool(rt_eq(v883_mode, rt_s(&S37)), "||")))) {
    RT_RET(rt_get(&F_c, rt_chk(s730_expr(up, v882_e), "bindRhs")));
  }
  RT_RET(({ wv t1704 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, v882_e), "bindRhs"))); wv t1705 = rt_text((rt_cond(v884_mutable) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S560), t1704, rt_s(&S415), t1705, rt_s(&S16) }); }));
}

static wv s710_setStmt(struct fr142 *up, wv v885_s) {
  rt_enter("setStmt");
  v885_s = rt_bind(v885_s, 0);
  wv v886_t = rt_bind(rt_get(&F_target, v885_s), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v886_t), rt_s(&S213)))) {
    wv v893_d = rt_bind(({ wv t1706 = rt_get(&F_pos, v886_t); wv t1707 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1706, t1707); }), 0);
    wv v894_at = rt_bind(rt_chk(s696_place(up, v893_d), "setStmt"), 0);
    if (rt_cond(rt_eq(rt_get(&F_op, v885_s), RT_NULL))) {
      wv v897_v = rt_bind(({ wv t1710 = rt_get(&F_e, v885_s); wv t1711 = (rt_cond(rt_get(&F_isRef, ({ wv t1708 = v893_d; wv t1709 = rt_def(up->v662_decls, "decls"); rt_idx(t1708, t1709); }))) ? rt_s(&S37) : rt_s(&S36)); rt_chk(s709_bindRhs(up, t1710, t1711), "setStmt"); }), 0);
      if (rt_cond(rt_not(rt_chk(s697_tdz(up, v893_d), "setStmt")))) {
        (void)rt_chk(s688_emit(up, ({ wv t1712 = rt_text(v894_at); wv t1713 = rt_text(v897_v); rt_cat(4, (wv[]){ t1712, rt_s(&S530), t1713, rt_s(&S14) }); })), "setStmt");
        RT_RET(RT_NULL);
      }
      wv v898_temp = rt_bind(rt_chk(s689_tmp(up), "setStmt"), 0);
      (void)rt_chk(s688_emit(up, ({ wv t1714 = rt_text(v898_temp); wv t1715 = rt_text(v897_v); rt_cat(5, (wv[]){ rt_s(&S529), t1714, rt_s(&S530), t1715, rt_s(&S14) }); })), "setStmt");
      (void)rt_chk(s688_emit(up, ({ wv t1718 = rt_text(v894_at); wv t1719 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1716 = v893_d; wv t1717 = rt_def(up->v662_decls, "decls"); rt_idx(t1716, t1717); }))), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S561), t1718, rt_s(&S415), t1719, rt_s(&S538) }); })), "setStmt");
      (void)rt_chk(s688_emit(up, ({ wv t1720 = rt_text(v894_at); wv t1721 = rt_text(v898_temp); rt_cat(4, (wv[]){ t1720, rt_s(&S530), t1721, rt_s(&S14) }); })), "setStmt");
      RT_RET(RT_NULL);
    }
    wv v895_v = rt_bind(rt_chk(s730_expr(up, rt_get(&F_e, v885_s)), "setStmt"), 1);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v885_s)), rt_s(&S138)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v885_s)), rt_s(&S213)), "&&")))) {
      wv v899_temp = rt_bind(rt_chk(s689_tmp(up), "setStmt"), 0);
      (void)rt_chk(s688_emit(up, ({ wv t1722 = rt_text(v899_temp); wv t1723 = rt_text(rt_get(&F_c, v895_v)); rt_cat(5, (wv[]){ rt_s(&S529), t1722, rt_s(&S530), t1723, rt_s(&S14) }); })), "setStmt");
      v895_v = ({ wv t1724 = v899_temp; wv t1725 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K54, (wv[]){ t1724, t1725 }); });
    }
    wv v896_sq = rt_bind(rt_chk(s699_seq(up, ({ wv t1726 = rt_chk(s698_read(up, v893_d), "setStmt"); wv t1727 = v895_v; rt_arr(2, (wv[]){ t1726, t1727 }, 0); })), "setStmt"), 0);
    (void)rt_chk(s688_emit(up, ({ wv t1733 = rt_text(v894_at); wv t1734 = rt_text(rt_get(&F_c, ({ wv t1731 = v896_sq; wv t1732 = ({ wv t1728 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v885_s)), "setStmt")); wv t1729 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v896_sq))); wv t1730 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v896_sq))); rt_cat(7, (wv[]){ rt_s(&S562), t1728, rt_s(&S97), t1729, rt_s(&S415), t1730, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1731, t1732), "setStmt"); }))); rt_cat(4, (wv[]){ t1733, rt_s(&S530), t1734, rt_s(&S14) }); })), "setStmt");
    RT_RET(RT_NULL);
  }
  wv v887_key = RT_NULL;
  if (rt_cond(rt_eq(rt_get(&F_k, v886_t), rt_s(&S191)))) {
    v887_key = ({ wv t1735 = rt_chk(s691_fieldRef(up, rt_get(&F_field, v886_t)), "setStmt"); wv t1736 = rt_def(w6_constK, "constK"); rt_rec(1, 2, K54, (wv[]){ t1735, t1736 }); });
  }
  wv v888_v = rt_bind(rt_chk(s730_expr(up, rt_get(&F_e, v885_s)), "setStmt"), 0);
  wv v889_i = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v886_t), rt_s(&S191))) ? v887_key : rt_chk(s730_expr(up, rt_get(&F_index, v886_t)), "setStmt")), 0);
  wv v890_box = rt_bind(rt_chk(s730_expr(up, (rt_cond(rt_eq(rt_get(&F_k, v886_t), rt_s(&S191))) ? rt_get(&F_rec, v886_t) : rt_get(&F_arr, v886_t))), "setStmt"), 0);
  wv v891_sq = rt_bind(rt_chk(s699_seq(up, rt_arr(3, (wv[]){ v888_v, v889_i, v890_box }, 0)), "setStmt"), 0);
  wv v892_which = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v886_t), rt_s(&S191))) ? rt_s(&S191) : rt_s(&S190)), 0);
  if (rt_cond(rt_eq(rt_get(&F_op, v885_s), RT_NULL))) {
    (void)rt_chk(s688_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t1739 = v891_sq; wv t1740 = ({ wv t1737 = rt_text(v892_which); wv t1738 = rt_text(std_join(rt_get(&F_cs, v891_sq), rt_s(&S415))); rt_cat(5, (wv[]){ rt_s(&S563), t1737, rt_s(&S97), t1738, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1739, t1740), "setStmt"); }))), rt_s(&S14) })), "setStmt");
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s688_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t1744 = v891_sq; wv t1745 = ({ wv t1741 = rt_text(v892_which); wv t1742 = rt_text(std_join(rt_get(&F_cs, v891_sq), rt_s(&S415))); wv t1743 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v885_s)), "setStmt")); rt_cat(7, (wv[]){ rt_s(&S564), t1741, rt_s(&S97), t1742, rt_s(&S565), t1743, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1744, t1745), "setStmt"); }))), rt_s(&S14) })), "setStmt");
  RT_RET(RT_NULL);
}

static wv s711_exprs(struct fr142 *up, wv v900_list) {
  rt_enter("exprs");
  v900_list = rt_bind(v900_list, 0);
  wv v901_xs = rt_arr(0, NULL, 1);
  wv v902_k = rt_i(0);
  while (rt_cond(({ wv t1746 = v902_k; wv t1747 = std_length(v900_list); rt_lt(t1746, t1747); }))) {
    (void)({ wv t1748 = v901_xs; wv t1749 = rt_chk(s730_expr(up, rt_idx(v902_k, v900_list)), "exprs"); std_push(t1748, t1749); });
    v902_k = rt_add(v902_k, rt_i(1));
  }
  RT_RET(v901_xs);
}

static wv s712_array(struct fr142 *up, wv v903_e, wv v904_mutable) {
  rt_enter("array");
  v903_e = rt_bind(v903_e, 0);
  v904_mutable = rt_bind(v904_mutable, 0);
  wv v905_sq = rt_bind(rt_chk(s699_seq(up, rt_chk(s711_exprs(up, rt_get(&F_items, v903_e)), "array")), "array"), 0);
  RT_RET(({ wv t1753 = v905_sq; wv t1754 = ({ wv t1750 = rt_text(std_length(rt_get(&F_cs, v905_sq))); wv t1751 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v905_sq)), "array")); wv t1752 = rt_text((rt_cond(v904_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S566), t1750, rt_s(&S415), t1751, rt_s(&S415), t1752, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1753, t1754), "array"); }));
}

static wv s713_mapLit(struct fr142 *up, wv v906_e, wv v907_mutable) {
  rt_enter("mapLit");
  v906_e = rt_bind(v906_e, 0);
  v907_mutable = rt_bind(v907_mutable, 0);
  wv v908_sq = rt_bind(rt_chk(s699_seq(up, rt_chk(s711_exprs(up, rt_get(&F_items, v906_e)), "mapLit")), "mapLit"), 0);
  RT_RET(({ wv t1758 = v908_sq; wv t1759 = ({ wv t1755 = rt_text(std_floor(rt_div(std_length(rt_get(&F_cs, v908_sq)), rt_i(2)))); wv t1756 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v908_sq)), "mapLit")); wv t1757 = rt_text((rt_cond(v907_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S567), t1755, rt_s(&S415), t1756, rt_s(&S415), t1757, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1758, t1759), "mapLit"); }));
}

static wv s714_record(struct fr142 *up, wv v909_e, wv v910_mutable) {
  rt_enter("record");
  v909_e = rt_bind(v909_e, 0);
  v910_mutable = rt_bind(v910_mutable, 0);
  wv v911_shape = rt_s(&S513);
  if (rt_cond(rt_gt(std_length(rt_get(&F_fields, v909_e)), rt_i(0)))) {
    wv v915_names = rt_arr(0, NULL, 1);
    wv v916_k = rt_i(0);
    while (rt_cond(({ wv t1760 = v916_k; wv t1761 = std_length(rt_get(&F_fields, v909_e)); rt_lt(t1760, t1761); }))) {
      (void)({ wv t1764 = v915_names; wv t1765 = rt_get(&F_name, ({ wv t1762 = v916_k; wv t1763 = rt_get(&F_fields, v909_e); rt_idx(t1762, t1763); })); std_push(t1764, t1765); });
      v916_k = rt_add(v916_k, rt_i(1));
    }
    wv v917_key = rt_bind(std_join(v915_names, rt_s(&S96)), 0);
    v916_k = rt_i(0);
    while (rt_cond(({ wv t1766 = v916_k; wv t1767 = std_length(v915_names); rt_lt(t1766, t1767); }))) {
      (void)rt_chk(s691_fieldRef(up, rt_idx(v916_k, v915_names)), "record");
      v916_k = rt_add(v916_k, rt_i(1));
    }
    v916_k = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t1768 = v916_k; wv t1769 = std_length(rt_def(up->v653_shapes, "shapes")); rt_lt(t1768, t1769); }), "&&") && rt_bool(({ wv t1772 = ({ wv t1770 = v916_k; wv t1771 = rt_def(up->v653_shapes, "shapes"); rt_idx(t1770, t1771); }); wv t1773 = v917_key; rt_ne(t1772, t1773); }), "&&")))) {
      v916_k = rt_add(v916_k, rt_i(1));
    }
    if (rt_cond(({ wv t1774 = v916_k; wv t1775 = std_length(rt_def(up->v653_shapes, "shapes")); rt_eq(t1774, t1775); }))) {
      (void)({ wv t1776 = rt_def(up->v653_shapes, "shapes"); wv t1777 = v917_key; std_push(t1776, t1777); });
    }
    v911_shape = rt_cat(2, (wv[]){ rt_s(&S568), rt_text(v916_k) });
  }
  wv v912_vals = rt_arr(0, NULL, 1);
  wv v913_k = rt_i(0);
  while (rt_cond(({ wv t1778 = v913_k; wv t1779 = std_length(rt_get(&F_fields, v909_e)); rt_lt(t1778, t1779); }))) {
    (void)({ wv t1782 = v912_vals; wv t1783 = rt_chk(s730_expr(up, rt_get(&F_e, ({ wv t1780 = v913_k; wv t1781 = rt_get(&F_fields, v909_e); rt_idx(t1780, t1781); }))), "record"); std_push(t1782, t1783); });
    v913_k = rt_add(v913_k, rt_i(1));
  }
  wv v914_sq = rt_bind(rt_chk(s699_seq(up, v912_vals), "record"), 0);
  RT_RET(({ wv t1788 = v914_sq; wv t1789 = ({ wv t1784 = rt_text((rt_cond(v910_mutable) ? rt_i(1) : rt_i(0))); wv t1785 = rt_text(std_length(rt_get(&F_cs, v914_sq))); wv t1786 = rt_text(v911_shape); wv t1787 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v914_sq)), "record")); rt_cat(9, (wv[]){ rt_s(&S569), t1784, rt_s(&S415), t1785, rt_s(&S415), t1786, rt_s(&S415), t1787, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1788, t1789), "record"); }));
}

static wv s715_onfailStmt(struct fr142 *up, wv v918_s) {
  rt_enter("onfailStmt");
  v918_s = rt_bind(v918_s, 0);
  wv v919_call = rt_bind(rt_get(&F_e, v918_s), 0);
  wv v920_args = rt_arr(0, NULL, 1);
  wv v921_k = rt_i(0);
  while (rt_cond(({ wv t1790 = v921_k; wv t1791 = std_length(rt_get(&F_args, v919_call)); rt_lt(t1790, t1791); }))) {
    wv v925_a = rt_bind(({ wv t1792 = v921_k; wv t1793 = rt_get(&F_args, v919_call); rt_idx(t1792, t1793); }), 0);
    if (rt_cond(({ wv t1794 = rt_def(w6_plainKinds, "plainKinds"); wv t1795 = rt_get(&F_k, v925_a); rt_chk(w1_contains(t1794, t1795), "onfailStmt"); }))) {
      (void)({ wv t1796 = v920_args; wv t1797 = rt_get(&F_c, rt_chk(s730_expr(up, v925_a), "onfailStmt")); std_push(t1796, t1797); });
    } else {
      wv v926_temp = rt_bind(rt_chk(s689_tmp(up), "onfailStmt"), 0);
      (void)rt_chk(s688_emit(up, ({ wv t1798 = rt_text(v926_temp); wv t1799 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, v925_a), "onfailStmt"))); rt_cat(5, (wv[]){ rt_s(&S529), t1798, rt_s(&S530), t1799, rt_s(&S14) }); })), "onfailStmt");
      (void)std_push(v920_args, v926_temp);
    }
    v921_k = rt_add(v921_k, rt_i(1));
  }
  wv v922_r = rt_bind(rt_chk(s689_tmp(up), "onfailStmt"), 0);
  (void)rt_chk(s688_emit(up, ({ wv t1802 = rt_text(v922_r); wv t1803 = rt_text(({ wv t1800 = rt_get(&F_callee, v919_call); wv t1801 = v920_args; rt_chk(s729_callC(up, t1800, t1801, rt_b(0)), "onfailStmt"); })); rt_cat(5, (wv[]){ rt_s(&S529), t1802, rt_s(&S530), t1803, rt_s(&S14) }); })), "onfailStmt");
  (void)rt_chk(s688_emit(up, rt_s(&S570)), "onfailStmt");
  up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
  wv v923_h = rt_bind(rt_get(&F_handler, v918_s), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v923_h), RT_NULL))) {
    (void)rt_chk(s707_bindDecl(up, ({ wv t1804 = rt_get(&F_paramPos, v923_h); wv t1805 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1804, t1805); }), rt_s(&S571)), "onfailStmt");
  }
  (void)rt_chk(s688_emit(up, rt_s(&S572)), "onfailStmt");
  (void)rt_chk(s688_emit(up, rt_s(&S100)), "onfailStmt");
  up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
  (void)rt_chk(s702_resets(up, rt_get(&F_body, v923_h)), "onfailStmt");
  (void)rt_chk(s701_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v923_h))), "onfailStmt");
  (void)rt_chk(s688_emit(up, ({ wv t1806 = rt_text(v922_r); wv t1807 = rt_text(rt_chk(s704_valueOf(up, rt_get(&F_body, v923_h)), "onfailStmt")); rt_cat(4, (wv[]){ t1806, rt_s(&S530), t1807, rt_s(&S14) }); })), "onfailStmt");
  up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
  (void)rt_chk(s688_emit(up, rt_s(&S15)), "onfailStmt");
  up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
  (void)rt_chk(s688_emit(up, rt_s(&S15)), "onfailStmt");
  wv v924_rest = rt_bind(v918_s, 1);
  rt_set_mem(RT_NULL, &F_handler, v924_rest);
  ({ wv t1810 = ({ wv t1808 = v922_r; wv t1809 = rt_get(&F_pos, v918_s); rt_rec(0, 4, K57, (wv[]){ rt_s(&S573), t1808, t1809, rt_b(0) }); }); wv t1811 = v924_rest; rt_set_mem(t1810, &F_e, t1811); });
  (void)rt_chk(s705_stmt(up, v924_rest), "onfailStmt");
  RT_RET(RT_NULL);
}

static wv s716_chainStmt(struct fr142 *up, wv v927_e) {
  rt_enter("chainStmt");
  v927_e = rt_bind(v927_e, 0);
  wv v928_k = rt_i(0);
  while (rt_cond(({ wv t1812 = v928_k; wv t1813 = std_length(rt_get(&F_branches, v927_e)); rt_lt(t1812, t1813); }))) {
    wv v929_b = rt_bind(({ wv t1814 = v928_k; wv t1815 = rt_get(&F_branches, v927_e); rt_idx(t1814, t1815); }), 0);
    (void)rt_chk(s688_emit(up, ({ wv t1816 = rt_text((rt_cond(rt_gt(v928_k, rt_i(0))) ? rt_s(&S574) : rt_s(&S13))); wv t1817 = rt_text(rt_chk(s717_test(up, v929_b), "chainStmt")); rt_cat(4, (wv[]){ t1816, rt_s(&S575), t1817, rt_s(&S576) }); })), "chainStmt");
    up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
    (void)rt_chk(s703_blockBody(up, rt_get(&F_body, v929_b)), "chainStmt");
    up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
    v928_k = rt_add(v928_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v927_e), RT_NULL))) {
    (void)rt_chk(s688_emit(up, rt_s(&S577)), "chainStmt");
    up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
    (void)rt_chk(s703_blockBody(up, rt_get(&F_els, v927_e)), "chainStmt");
    up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s688_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s717_test(struct fr142 *up, wv v930_b) {
  rt_enter("test");
  v930_b = rt_bind(v930_b, 0);
  RT_RET(({ wv t1818 = rt_text((rt_cond(rt_get(&F_neg, v930_b)) ? rt_s(&S94) : rt_s(&S13))); wv t1819 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_cond, v930_b)), "test"))); rt_cat(4, (wv[]){ t1818, rt_s(&S544), t1819, rt_s(&S16) }); }));
}

static wv s718_branchCount(struct fr142 *up, wv v931_e) {
  rt_enter("branchCount");
  v931_e = rt_bind(v931_e, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v931_e), rt_s(&S185))) ? std_length(rt_get(&F_branches, v931_e)) : std_length(rt_get(&F_cases, v931_e))));
}

static wv s719_branchBody(struct fr142 *up, wv v932_e, wv v933_i) {
  rt_enter("branchBody");
  v932_e = rt_bind(v932_e, 0);
  v933_i = rt_bind(v933_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v932_e), rt_s(&S185))) ? rt_get(&F_body, ({ wv t1820 = v933_i; wv t1821 = rt_get(&F_branches, v932_e); rt_idx(t1820, t1821); })) : rt_get(&F_body, ({ wv t1822 = v933_i; wv t1823 = rt_get(&F_cases, v932_e); rt_idx(t1822, t1823); }))));
}

static wv s720_branchTest(struct fr142 *up, wv v934_e, wv v935_t, wv v936_i) {
  rt_enter("branchTest");
  v934_e = rt_bind(v934_e, 0);
  v935_t = rt_bind(v935_t, 0);
  v936_i = rt_bind(v936_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v934_e), rt_s(&S185))) ? rt_chk(s717_test(up, ({ wv t1824 = v936_i; wv t1825 = rt_get(&F_branches, v934_e); rt_idx(t1824, t1825); })), "branchTest") : ({ wv t1828 = v935_t; wv t1829 = ({ wv t1826 = v936_i; wv t1827 = rt_get(&F_cases, v934_e); rt_idx(t1826, t1827); }); rt_chk(s725_caseTest(up, t1828, t1829), "branchTest"); })));
}

static wv s721_isPlain(struct fr142 *up, wv v937_e) {
  rt_enter("isPlain");
  v937_e = rt_bind(v937_e, 0);
  wv v938_k = rt_i(0);
  while (rt_cond(({ wv t1830 = v938_k; wv t1831 = rt_chk(s718_branchCount(up, v937_e), "isPlain"); rt_le(t1830, t1831); }))) {
    wv v939_b = rt_bind((rt_cond(({ wv t1832 = v938_k; wv t1833 = rt_chk(s718_branchCount(up, v937_e), "isPlain"); rt_lt(t1832, t1833); })) ? rt_chk(s719_branchBody(up, v937_e, v938_k), "isPlain") : rt_get(&F_els, v937_e)), 0);
    wv v940_j = rt_i(0);
    while (rt_cond(({ wv t1834 = v940_j; wv t1835 = std_length(rt_get(&F_stmts, v939_b)); rt_lt(t1834, t1835); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1836 = v940_j; wv t1837 = rt_get(&F_stmts, v939_b); rt_idx(t1836, t1837); })), rt_s(&S202)))) {
        RT_RET(rt_b(0));
      }
      v940_j = rt_add(v940_j, rt_i(1));
    }
    v938_k = rt_add(v938_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s722_ternaryChain(struct fr142 *up, wv v941_e, wv v942_t) {
  rt_enter("ternaryChain");
  v941_e = rt_bind(v941_e, 0);
  v942_t = rt_bind(v942_t, 0);
  wv v943_code = rt_bind(rt_chk(s704_valueOf(up, rt_get(&F_els, v941_e)), "ternaryChain"), 1);
  wv v944_k = rt_sub(rt_chk(s718_branchCount(up, v941_e), "ternaryChain"), rt_i(1));
  while (rt_cond(rt_ge(v944_k, rt_i(0)))) {
    v943_code = ({ wv t1838 = rt_text(rt_chk(s720_branchTest(up, v941_e, v942_t, v944_k), "ternaryChain")); wv t1839 = rt_text(rt_chk(s704_valueOf(up, rt_chk(s719_branchBody(up, v941_e, v944_k), "ternaryChain")), "ternaryChain")); wv t1840 = rt_text(v943_code); rt_cat(7, (wv[]){ rt_s(&S97), t1838, rt_s(&S578), t1839, rt_s(&S579), t1840, rt_s(&S16) }); });
    v944_k = rt_sub(v944_k, rt_i(1));
  }
  RT_RET(v943_code);
}

static wv s723_branchValue(struct fr142 *up, wv v945_e, wv v946_t, wv v947_subject) {
  rt_enter("branchValue");
  v945_e = rt_bind(v945_e, 0);
  v946_t = rt_bind(v946_t, 0);
  v947_subject = rt_bind(v947_subject, 0);
  wv v948_start = rt_bind(std_length(rt_def(up->v658_out, "out")), 0);
  wv v949_savedIndent = rt_bind(rt_def(up->v659_indent, "indent"), 0);
  wv v950_savedLoops = rt_bind(rt_def(up->v660_loops, "loops"), 0);
  wv v951_savedIife = rt_bind(rt_def(up->v661_iife, "iife"), 0);
  wv t1841 = rt_add(v949_savedIndent, rt_i(1));
  (void)rt_def(up->v659_indent, "indent");
  up->v659_indent = t1841;
  wv t1842 = rt_i(0);
  (void)rt_def(up->v660_loops, "loops");
  up->v660_loops = t1842;
  wv t1843 = rt_b(1);
  (void)rt_def(up->v661_iife, "iife");
  up->v661_iife = t1843;
  if (rt_cond(rt_ne(v947_subject, RT_NULL))) {
    (void)rt_chk(s688_emit(up, ({ wv t1844 = rt_text(v946_t); wv t1845 = rt_text(v947_subject); rt_cat(5, (wv[]){ rt_s(&S529), t1844, rt_s(&S530), t1845, rt_s(&S14) }); })), "branchValue");
  }
  wv v952_r = rt_bind(rt_chk(s689_tmp(up), "branchValue"), 0);
  (void)rt_chk(s688_emit(up, rt_cat(3, (wv[]){ rt_s(&S529), rt_text(v952_r), rt_s(&S14) })), "branchValue");
  wv v953_k = rt_i(0);
  while (rt_cond(({ wv t1846 = v953_k; wv t1847 = rt_chk(s718_branchCount(up, v945_e), "branchValue"); rt_lt(t1846, t1847); }))) {
    wv v955_b = rt_bind(rt_chk(s719_branchBody(up, v945_e, v953_k), "branchValue"), 0);
    (void)rt_chk(s688_emit(up, ({ wv t1848 = rt_text((rt_cond(rt_gt(v953_k, rt_i(0))) ? rt_s(&S574) : rt_s(&S13))); wv t1849 = rt_text(rt_chk(s720_branchTest(up, v945_e, v946_t, v953_k), "branchValue")); rt_cat(4, (wv[]){ t1848, rt_s(&S575), t1849, rt_s(&S576) }); })), "branchValue");
    up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
    (void)rt_chk(s702_resets(up, v955_b), "branchValue");
    (void)rt_chk(s701_stmts(up, rt_get(&F_stmts, v955_b)), "branchValue");
    (void)rt_chk(s688_emit(up, ({ wv t1850 = rt_text(v952_r); wv t1851 = rt_text(rt_chk(s704_valueOf(up, v955_b), "branchValue")); rt_cat(4, (wv[]){ t1850, rt_s(&S530), t1851, rt_s(&S14) }); })), "branchValue");
    up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
    v953_k = rt_add(v953_k, rt_i(1));
  }
  (void)rt_chk(s688_emit(up, rt_s(&S577)), "branchValue");
  up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
  (void)rt_chk(s702_resets(up, rt_get(&F_els, v945_e)), "branchValue");
  (void)rt_chk(s701_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v945_e))), "branchValue");
  (void)rt_chk(s688_emit(up, ({ wv t1852 = rt_text(v952_r); wv t1853 = rt_text(rt_chk(s704_valueOf(up, rt_get(&F_els, v945_e)), "branchValue")); rt_cat(4, (wv[]){ t1852, rt_s(&S530), t1853, rt_s(&S14) }); })), "branchValue");
  up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
  (void)rt_chk(s688_emit(up, rt_s(&S15)), "branchValue");
  (void)rt_chk(s688_emit(up, rt_cat(2, (wv[]){ rt_text(v952_r), rt_s(&S14) })), "branchValue");
  wv v954_body = rt_bind(std_join(({ wv t1854 = rt_def(up->v658_out, "out"); wv t1855 = v948_start; wv t1856 = std_length(rt_def(up->v658_out, "out")); std_slice(t1854, t1855, t1856); }), rt_s(&S2)), 0);
  while (rt_cond(({ wv t1857 = std_length(rt_def(up->v658_out, "out")); wv t1858 = v948_start; rt_gt(t1857, t1858); }))) {
    (void)std_pop(rt_def(up->v658_out, "out"));
  }
  wv t1859 = rt_bind(v949_savedIndent, 1);
  (void)rt_def(up->v659_indent, "indent");
  up->v659_indent = t1859;
  wv t1860 = rt_bind(v950_savedLoops, 1);
  (void)rt_def(up->v660_loops, "loops");
  up->v660_loops = t1860;
  wv t1861 = rt_bind(v951_savedIife, 1);
  (void)rt_def(up->v661_iife, "iife");
  up->v661_iife = t1861;
  RT_RET(({ wv t1862 = rt_text(v954_body); wv t1863 = rt_text(rt_chk(w6_repeat(rt_s(&S517), rt_def(up->v659_indent, "indent")), "branchValue")); rt_cat(5, (wv[]){ rt_s(&S580), t1862, rt_s(&S2), t1863, rt_s(&S581) }); }));
}

static wv s724_chainExpr(struct fr142 *up, wv v956_e) {
  rt_enter("chainExpr");
  v956_e = rt_bind(v956_e, 0);
  if (rt_cond(rt_chk(s721_isPlain(up, v956_e), "chainExpr"))) {
    RT_RET(rt_chk(s722_ternaryChain(up, v956_e, RT_NULL), "chainExpr"));
  }
  RT_RET(rt_chk(s723_branchValue(up, v956_e, RT_NULL, RT_NULL), "chainExpr"));
}

static wv s725_caseTest(struct fr142 *up, wv v957_t, wv v958_a) {
  rt_enter("caseTest");
  v957_t = rt_bind(v957_t, 0);
  v958_a = rt_bind(v958_a, 0);
  wv v959_tests = rt_arr(0, NULL, 1);
  wv v960_k = rt_i(0);
  while (rt_cond(({ wv t1864 = v960_k; wv t1865 = std_length(rt_get(&F_values, v958_a)); rt_lt(t1864, t1865); }))) {
    (void)({ wv t1870 = v959_tests; wv t1871 = ({ wv t1868 = rt_text(v957_t); wv t1869 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, ({ wv t1866 = v960_k; wv t1867 = rt_get(&F_values, v958_a); rt_idx(t1866, t1867); })), "caseTest"))); rt_cat(5, (wv[]){ rt_s(&S582), t1868, rt_s(&S415), t1869, rt_s(&S16) }); }); std_push(t1870, t1871); });
    v960_k = rt_add(v960_k, rt_i(1));
  }
  RT_RET(std_join(v959_tests, rt_s(&S583)));
}

static wv s726_matchElse(struct fr142 *up, wv v961_e, wv v962_t) {
  rt_enter("matchElse");
  v961_e = rt_bind(v961_e, 0);
  v962_t = rt_bind(v962_t, 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v961_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_cases, v961_e))))), rt_s(&S191)), "||")))) {
    RT_RET(rt_get(&F_els, v961_e));
  }
  RT_RET(({ wv t1872 = rt_arr(0, NULL, 0); wv t1873 = rt_rec(0, 4, K57, (wv[]){ rt_s(&S573), rt_cat(3, (wv[]){ rt_s(&S584), rt_text(v962_t), rt_s(&S585) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K21, (wv[]){ t1872, t1873, RT_NULL }); }));
}

static wv s727_matchStmt(struct fr142 *up, wv v963_e) {
  rt_enter("matchStmt");
  v963_e = rt_bind(v963_e, 0);
  wv v964_t = rt_bind(rt_chk(s689_tmp(up), "matchStmt"), 0);
  (void)rt_chk(s688_emit(up, ({ wv t1874 = rt_text(v964_t); wv t1875 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_subject, v963_e)), "matchStmt"))); rt_cat(5, (wv[]){ rt_s(&S529), t1874, rt_s(&S530), t1875, rt_s(&S14) }); })), "matchStmt");
  wv v965_k = rt_i(0);
  while (rt_cond(({ wv t1876 = v965_k; wv t1877 = std_length(rt_get(&F_cases, v963_e)); rt_lt(t1876, t1877); }))) {
    (void)rt_chk(s688_emit(up, ({ wv t1882 = rt_text((rt_cond(rt_gt(v965_k, rt_i(0))) ? rt_s(&S574) : rt_s(&S13))); wv t1883 = rt_text(({ wv t1880 = v964_t; wv t1881 = ({ wv t1878 = v965_k; wv t1879 = rt_get(&F_cases, v963_e); rt_idx(t1878, t1879); }); rt_chk(s725_caseTest(up, t1880, t1881), "matchStmt"); })); rt_cat(4, (wv[]){ t1882, rt_s(&S575), t1883, rt_s(&S576) }); })), "matchStmt");
    up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
    (void)rt_chk(s703_blockBody(up, rt_get(&F_body, ({ wv t1884 = v965_k; wv t1885 = rt_get(&F_cases, v963_e); rt_idx(t1884, t1885); }))), "matchStmt");
    up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
    v965_k = rt_add(v965_k, rt_i(1));
  }
  wv v966_els = rt_bind(rt_chk(s726_matchElse(up, v963_e, v964_t), "matchStmt"), 0);
  if (rt_cond(rt_ne(v966_els, RT_NULL))) {
    (void)rt_chk(s688_emit(up, rt_s(&S577)), "matchStmt");
    up->v659_indent = rt_add(rt_def(up->v659_indent, "indent"), rt_i(1));
    (void)rt_chk(s703_blockBody(up, v966_els), "matchStmt");
    up->v659_indent = rt_sub(rt_def(up->v659_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s688_emit(up, rt_s(&S15)), "matchStmt");
  RT_RET(RT_NULL);
}

static wv s728_matchExpr(struct fr142 *up, wv v967_e) {
  rt_enter("matchExpr");
  v967_e = rt_bind(v967_e, 0);
  wv v968_t = rt_bind(rt_chk(s689_tmp(up), "matchExpr"), 0);
  wv v969_subject = rt_bind(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_subject, v967_e)), "matchExpr")), 0);
  wv v970_withElse = rt_bind(v967_e, 1);
  ({ wv t1886 = rt_chk(s726_matchElse(up, v967_e, v968_t), "matchExpr"); wv t1887 = v970_withElse; rt_set_mem(t1886, &F_els, t1887); });
  if (rt_cond(rt_chk(s721_isPlain(up, v970_withElse), "matchExpr"))) {
    RT_RET(({ wv t1888 = rt_text(v968_t); wv t1889 = rt_text(v969_subject); wv t1890 = rt_text(rt_chk(s722_ternaryChain(up, v970_withElse, v968_t), "matchExpr")); rt_cat(7, (wv[]){ rt_s(&S586), t1888, rt_s(&S530), t1889, rt_s(&S587), t1890, rt_s(&S532) }); }));
  }
  RT_RET(rt_chk(s723_branchValue(up, v970_withElse, v968_t, v969_subject), "matchExpr"));
}

static wv s729_callC(struct fr142 *up, wv v971_callee, wv v972_args, wv v973_checked) {
  rt_enter("callC");
  v971_callee = rt_bind(v971_callee, 0);
  v973_checked = rt_bind(v973_checked, 0);
  wv v974_code = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v971_callee), rt_s(&S156)))) {
    v974_code = ({ wv t1891 = rt_text(rt_get(&F_name, v971_callee)); wv t1892 = rt_text(std_join(v972_args, rt_s(&S415))); rt_cat(5, (wv[]){ rt_s(&S588), t1891, rt_s(&S97), t1892, rt_s(&S16) }); });
    if (rt_cond(rt_not(({ wv t1893 = rt_def(w6_signalers, "signalers"); wv t1894 = rt_get(&F_name, v971_callee); rt_chk(w1_contains(t1893, t1894), "callC"); })))) {
      RT_RET(v974_code);
    }
  } else {
    wv v975_f = rt_s(&S13);
    wv v976_parent = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_k, v971_callee), rt_s(&S191)))) {
      v975_f = ({ wv t1895 = rt_text(rt_chk(s672_modId(up, rt_chk(s693_stolenFrom(up, rt_get(&F_rec, v971_callee)), "callC")), "callC")); wv t1896 = rt_text(rt_get(&F_field, v971_callee)); rt_cat(4, (wv[]){ rt_s(&S515), t1895, rt_s(&S12), t1896 }); });
    } else {
      wv v979_d = rt_bind(({ wv t1897 = rt_get(&F_pos, v971_callee); wv t1898 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1897, t1898); }), 0);
      wv v980_sf = rt_bind(rt_get(&F_fnIndex, ({ wv t1899 = v979_d; wv t1900 = rt_def(up->v662_decls, "decls"); rt_idx(t1899, t1900); })), 0);
      v975_f = rt_bind(rt_get(&F_cname, ({ wv t1901 = v980_sf; wv t1902 = rt_def(up->v663_fns, "fns"); rt_idx(t1901, t1902); })), 1);
      wv v981_pf = rt_bind(rt_get(&F_parent, ({ wv t1903 = v980_sf; wv t1904 = rt_def(up->v663_fns, "fns"); rt_idx(t1903, t1904); })), 0);
      if (rt_cond(rt_not(rt_get(&F_top, ({ wv t1905 = v981_pf; wv t1906 = rt_def(up->v663_fns, "fns"); rt_idx(t1905, t1906); }))))) {
        v976_parent = rt_bind(v981_pf, 1);
      }
    }
    wv v977_all = rt_arr(0, NULL, 1);
    if (rt_cond(rt_ne(v976_parent, RT_NULL))) {
      (void)({ wv t1915 = v977_all; wv t1916 = (rt_cond(({ wv t1907 = v976_parent; wv t1908 = rt_def(up->v671_cur, "cur"); rt_eq(t1907, t1908); })) ? rt_s(&S589) : rt_cat(2, (wv[]){ rt_s(&S525), rt_text(rt_chk(w6_repeat(rt_s(&S526), rt_sub(({ wv t1913 = rt_get(&F_depth, ({ wv t1909 = rt_def(up->v671_cur, "cur"); wv t1910 = rt_def(up->v663_fns, "fns"); rt_idx(t1909, t1910); })); wv t1914 = rt_get(&F_depth, ({ wv t1911 = v976_parent; wv t1912 = rt_def(up->v663_fns, "fns"); rt_idx(t1911, t1912); })); rt_sub(t1913, t1914); }), rt_i(1))), "callC")) })); std_push(t1915, t1916); });
    }
    wv v978_k = rt_i(0);
    while (rt_cond(({ wv t1917 = v978_k; wv t1918 = std_length(v972_args); rt_lt(t1917, t1918); }))) {
      (void)({ wv t1919 = v977_all; wv t1920 = rt_idx(v978_k, v972_args); std_push(t1919, t1920); });
      v978_k = rt_add(v978_k, rt_i(1));
    }
    v974_code = ({ wv t1921 = rt_text(v975_f); wv t1922 = rt_text(std_join(v977_all, rt_s(&S415))); rt_cat(4, (wv[]){ t1921, rt_s(&S97), t1922, rt_s(&S16) }); });
  }
  RT_RET((rt_cond(v973_checked) ? ({ wv t1927 = rt_text(v974_code); wv t1928 = rt_text((rt_cond(rt_get(&F_top, ({ wv t1923 = rt_def(up->v671_cur, "cur"); wv t1924 = rt_def(up->v663_fns, "fns"); rt_idx(t1923, t1924); }))) ? rt_s(&S513) : rt_chk(w1_quote(rt_get(&F_name, ({ wv t1925 = rt_def(up->v671_cur, "cur"); wv t1926 = rt_def(up->v663_fns, "fns"); rt_idx(t1925, t1926); }))), "callC"))); rt_cat(5, (wv[]){ rt_s(&S590), t1927, rt_s(&S415), t1928, rt_s(&S16) }); }) : v974_code));
}

static wv s730_expr(struct fr142 *up, wv v982_e) {
  rt_enter("expr");
  wv v983_kind = rt_bind(rt_get(&F_k, v982_e), 0);
  if (rt_cond(rt_eq(v983_kind, rt_s(&S138)))) {
    RT_RET(({ wv t1929 = rt_cat(3, (wv[]){ rt_s(&S546), rt_text(rt_get(&F_v, v982_e)), rt_s(&S16) }); wv t1930 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1929, t1930 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S148)))) {
    RT_RET(({ wv t1931 = rt_cat(3, (wv[]){ rt_s(&S591), rt_text(rt_get(&F_v, v982_e)), rt_s(&S16) }); wv t1932 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1931, t1932 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S172)))) {
    wv v984_parts = rt_arr(0, NULL, 1);
    wv v985_k = rt_i(0);
    while (rt_cond(({ wv t1933 = v985_k; wv t1934 = std_length(rt_get(&F_parts, v982_e)); rt_lt(t1933, t1934); }))) {
      wv v987_part = rt_bind(({ wv t1935 = v985_k; wv t1936 = rt_get(&F_parts, v982_e); rt_idx(t1935, t1936); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v987_part), rt_s(&S186)))) {
        if (rt_cond(rt_ne(v987_part, rt_s(&S13)))) {
          (void)({ wv t1939 = v984_parts; wv t1940 = ({ wv t1937 = rt_cat(3, (wv[]){ rt_s(&S592), rt_text(rt_chk(s690_str(up, v987_part), "expr")), rt_s(&S16) }); wv t1938 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1937, t1938 }); }); std_push(t1939, t1940); });
        }
      } else {
        (void)({ wv t1943 = v984_parts; wv t1944 = ({ wv t1941 = rt_cat(3, (wv[]){ rt_s(&S593), rt_text(rt_get(&F_c, rt_chk(s730_expr(up, v987_part), "expr"))), rt_s(&S16) }); wv t1942 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1941, t1942 }); }); std_push(t1943, t1944); });
      }
      v985_k = rt_add(v985_k, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v984_parts), rt_i(0)))) {
      RT_RET(({ wv t1945 = rt_cat(3, (wv[]){ rt_s(&S592), rt_text(rt_chk(s690_str(up, rt_s(&S13)), "expr")), rt_s(&S16) }); wv t1946 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1945, t1946 }); }));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v984_parts), rt_i(1)), "&&") && rt_bool(({ wv t1947 = rt_get(&F_k, rt_idx(rt_i(0), v984_parts)); wv t1948 = rt_def(w6_constK, "constK"); rt_eq(t1947, t1948); }), "&&")))) {
      RT_RET(rt_idx(rt_i(0), v984_parts));
    }
    wv v986_sq = rt_bind(rt_chk(s699_seq(up, v984_parts), "expr"), 0);
    RT_RET(({ wv t1951 = v986_sq; wv t1952 = ({ wv t1949 = rt_text(std_length(rt_get(&F_cs, v986_sq))); wv t1950 = rt_text(rt_chk(w6_values(rt_get(&F_cs, v986_sq)), "expr")); rt_cat(5, (wv[]){ rt_s(&S594), t1949, rt_s(&S415), t1950, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1951, t1952), "expr"); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S284)))) {
    RT_RET(({ wv t1953 = rt_cat(3, (wv[]){ rt_s(&S595), rt_text((rt_cond(rt_get(&F_v, v982_e)) ? rt_i(1) : rt_i(0))), rt_s(&S16) }); wv t1954 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1953, t1954 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S51)))) {
    RT_RET(rt_rec(0, 2, K54, (wv[]){ rt_s(&S535), rt_def(w6_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S52)))) {
    RT_RET(rt_rec(0, 2, K54, (wv[]){ rt_s(&S596), rt_def(w6_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S53)))) {
    RT_RET(rt_rec(0, 2, K54, (wv[]){ rt_s(&S597), rt_def(w6_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S213)))) {
    RT_RET(rt_chk(s698_read(up, ({ wv t1955 = rt_get(&F_pos, v982_e); wv t1956 = rt_def(up->v668_curSlot, "curSlot"); rt_idx(t1955, t1956); })), "expr"));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S156)))) {
    RT_RET(({ wv t1957 = rt_cat(3, (wv[]){ rt_s(&S588), rt_text(rt_get(&F_name, v982_e)), rt_s(&S598) }); wv t1958 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1957, t1958 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S187)))) {
    RT_RET(rt_chk(s712_array(up, v982_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S188)))) {
    RT_RET(rt_chk(s713_mapLit(up, v982_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S189)))) {
    RT_RET(rt_chk(s714_record(up, v982_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S190)))) {
    wv v988_sq = rt_bind(rt_chk(s699_seq(up, ({ wv t1959 = rt_chk(s730_expr(up, rt_get(&F_index, v982_e)), "expr"); wv t1960 = rt_chk(s730_expr(up, rt_get(&F_arr, v982_e)), "expr"); rt_arr(2, (wv[]){ t1959, t1960 }, 0); })), "expr"), 0);
    RT_RET(({ wv t1963 = v988_sq; wv t1964 = ({ wv t1961 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v988_sq))); wv t1962 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v988_sq))); rt_cat(5, (wv[]){ rt_s(&S599), t1961, rt_s(&S415), t1962, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1963, t1964), "expr"); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S191)))) {
    wv v989_lin = rt_bind(rt_chk(s695_enumOf(up, rt_get(&F_rec, v982_e)), "expr"), 0);
    if (rt_cond(rt_ne(v989_lin, RT_NULL))) {
      wv v993_ms = rt_bind(rt_get(&F_members, rt_get(&F_node, v989_lin)), 0);
      wv v994_k = rt_i(0);
      while (rt_cond(({ wv t1965 = rt_get(&F_name, rt_idx(v994_k, v993_ms)); wv t1966 = rt_get(&F_field, v982_e); rt_ne(t1965, t1966); }))) {
        v994_k = rt_add(v994_k, rt_i(1));
      }
      RT_RET(({ wv t1969 = ({ wv t1967 = rt_text(rt_get(&F_c, v989_lin)); wv t1968 = rt_text(v994_k); rt_cat(5, (wv[]){ rt_s(&S600), t1967, rt_s(&S98), t1968, rt_s(&S601) }); }); wv t1970 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1969, t1970 }); }));
    }
    wv v990_lib = rt_bind(rt_chk(s693_stolenFrom(up, rt_get(&F_rec, v982_e)), "expr"), 0);
    if (rt_cond(rt_ne(v990_lib, RT_NULL))) {
      RT_RET(({ wv t1973 = ({ wv t1971 = rt_text(rt_chk(s672_modId(up, v990_lib), "expr")); wv t1972 = rt_text(rt_get(&F_field, v982_e)); rt_cat(4, (wv[]){ rt_s(&S515), t1971, rt_s(&S12), t1972 }); }); wv t1974 = rt_def(w6_pureK, "pureK"); rt_rec(0, 2, K54, (wv[]){ t1973, t1974 }); }));
    }
    wv v991_key = ({ wv t1975 = rt_chk(s691_fieldRef(up, rt_get(&F_field, v982_e)), "expr"); wv t1976 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t1975, t1976 }); });
    wv v992_sq = rt_bind(rt_chk(s699_seq(up, ({ wv t1977 = v991_key; wv t1978 = rt_chk(s730_expr(up, rt_get(&F_rec, v982_e)), "expr"); rt_arr(2, (wv[]){ t1977, t1978 }, 0); })), "expr"), 0);
    RT_RET(({ wv t1981 = v992_sq; wv t1982 = ({ wv t1979 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v992_sq))); wv t1980 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v992_sq))); rt_cat(5, (wv[]){ rt_s(&S602), t1979, rt_s(&S415), t1980, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1981, t1982), "expr"); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S192)))) {
    RT_RET(({ wv t1985 = ({ wv t1983 = rt_text(rt_chk(w6_unFn(rt_get(&F_op, v982_e)), "expr")); wv t1984 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_e, v982_e)), "expr"))); rt_cat(5, (wv[]){ rt_s(&S562), t1983, rt_s(&S97), t1984, rt_s(&S16) }); }); wv t1986 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1985, t1986 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S193)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v982_e), rt_s(&S80)), "||") || rt_bool(rt_eq(rt_get(&F_op, v982_e), rt_s(&S81)), "||")))) {
      RT_RET(({ wv t1992 = ({ wv t1987 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_l, v982_e)), "expr"))); wv t1988 = rt_text(rt_get(&F_op, v982_e)); wv t1989 = rt_text(rt_get(&F_op, v982_e)); wv t1990 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_r, v982_e)), "expr"))); wv t1991 = rt_text(rt_get(&F_op, v982_e)); rt_cat(11, (wv[]){ rt_s(&S603), t1987, rt_s(&S604), t1988, rt_s(&S605), t1989, rt_s(&S606), t1990, rt_s(&S604), t1991, rt_s(&S607) }); }); wv t1993 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t1992, t1993 }); }));
    }
    wv v995_sq = rt_bind(rt_chk(s699_seq(up, ({ wv t1994 = rt_chk(s730_expr(up, rt_get(&F_l, v982_e)), "expr"); wv t1995 = rt_chk(s730_expr(up, rt_get(&F_r, v982_e)), "expr"); rt_arr(2, (wv[]){ t1994, t1995 }, 0); })), "expr"), 0);
    RT_RET(({ wv t1999 = v995_sq; wv t2000 = ({ wv t1996 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v982_e)), "expr")); wv t1997 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v995_sq))); wv t1998 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v995_sq))); rt_cat(7, (wv[]){ rt_s(&S562), t1996, rt_s(&S97), t1997, rt_s(&S415), t1998, rt_s(&S16) }); }); rt_chk(s700_wrap(up, t1999, t2000), "expr"); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S194)))) {
    RT_RET(({ wv t2004 = ({ wv t2001 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_c, v982_e)), "expr"))); wv t2002 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_a, v982_e)), "expr"))); wv t2003 = rt_text(rt_get(&F_c, rt_chk(s730_expr(up, rt_get(&F_b, v982_e)), "expr"))); rt_cat(7, (wv[]){ rt_s(&S608), t2001, rt_s(&S609), t2002, rt_s(&S579), t2003, rt_s(&S16) }); }); wv t2005 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t2004, t2005 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S183)))) {
    wv v996_sq = rt_bind(rt_chk(s699_seq(up, rt_chk(s711_exprs(up, rt_get(&F_args, v982_e)), "expr")), "expr"), 0);
    RT_RET(({ wv t2008 = v996_sq; wv t2009 = ({ wv t2006 = rt_get(&F_callee, v982_e); wv t2007 = rt_get(&F_cs, v996_sq); rt_chk(s729_callC(up, t2006, t2007, rt_b(1)), "expr"); }); rt_chk(s700_wrap(up, t2008, t2009), "expr"); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S185)))) {
    RT_RET(({ wv t2010 = rt_chk(s724_chainExpr(up, v982_e), "expr"); wv t2011 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t2010, t2011 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S44)))) {
    RT_RET(({ wv t2012 = rt_chk(s728_matchExpr(up, v982_e), "expr"); wv t2013 = rt_def(w6_effectK, "effectK"); rt_rec(0, 2, K54, (wv[]){ t2012, t2013 }); }));
  }
  if (rt_cond(rt_eq(v983_kind, rt_s(&S573)))) {
    RT_RET(({ wv t2014 = rt_get(&F_js, v982_e); wv t2015 = rt_def(w6_constK, "constK"); rt_rec(0, 2, K54, (wv[]){ t2014, t2015 }); }));
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S610), rt_text(v983_kind) }));
  RT_RET(RT_NULL);
}

static wv s731_assemble(struct fr142 *up) {
  rt_enter("assemble");
  wv v997_all = rt_arr(3, (wv[]){ std_join(std_split(up->v648_runtime, rt_s(&S611)), rt_s(&S2)), rt_s(&S612), rt_s(&S13) }, 1);
  wv v998_k = rt_i(0);
  while (rt_cond(({ wv t2016 = v998_k; wv t2017 = std_length(rt_def(up->v651_strings, "strings")); rt_lt(t2016, t2017); }))) {
    (void)({ wv t2022 = v997_all; wv t2023 = ({ wv t2020 = rt_text(v998_k); wv t2021 = rt_text(rt_chk(w6_wstrInit(({ wv t2018 = v998_k; wv t2019 = rt_def(up->v651_strings, "strings"); rt_idx(t2018, t2019); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S613), t2020, rt_s(&S530), t2021, rt_s(&S14) }); }); std_push(t2022, t2023); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2024 = v998_k; wv t2025 = std_length(rt_def(up->v652_fields, "fields")); rt_lt(t2024, t2025); }))) {
    (void)({ wv t2032 = v997_all; wv t2033 = ({ wv t2030 = rt_text(({ wv t2026 = v998_k; wv t2027 = rt_def(up->v652_fields, "fields"); rt_idx(t2026, t2027); })); wv t2031 = rt_text(rt_chk(w6_wstrInit(({ wv t2028 = v998_k; wv t2029 = rt_def(up->v652_fields, "fields"); rt_idx(t2028, t2029); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S614), t2030, rt_s(&S530), t2031, rt_s(&S14) }); }); std_push(t2032, t2033); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2034 = v998_k; wv t2035 = std_length(rt_def(up->v653_shapes, "shapes")); rt_lt(t2034, t2035); }))) {
    wv v999_names = rt_bind(std_split(({ wv t2036 = v998_k; wv t2037 = rt_def(up->v653_shapes, "shapes"); rt_idx(t2036, t2037); }), rt_s(&S96)), 0);
    wv v1000_refs = rt_arr(0, NULL, 1);
    wv v1001_j = rt_i(0);
    while (rt_cond(({ wv t2038 = v1001_j; wv t2039 = std_length(v999_names); rt_lt(t2038, t2039); }))) {
      (void)({ wv t2040 = v1000_refs; wv t2041 = rt_cat(2, (wv[]){ rt_s(&S519), rt_text(rt_idx(v1001_j, v999_names)) }); std_push(t2040, t2041); });
      v1001_j = rt_add(v1001_j, rt_i(1));
    }
    (void)({ wv t2044 = v997_all; wv t2045 = ({ wv t2042 = rt_text(v998_k); wv t2043 = rt_text(std_join(v1000_refs, rt_s(&S415))); rt_cat(5, (wv[]){ rt_s(&S615), t2042, rt_s(&S521), t2043, rt_s(&S522) }); }); std_push(t2044, t2045); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2046 = v998_k; wv t2047 = std_length(rt_def(up->v654_enumDefs, "enumDefs")); rt_lt(t2046, t2047); }))) {
    (void)({ wv t2050 = v997_all; wv t2051 = ({ wv t2048 = v998_k; wv t2049 = rt_def(up->v654_enumDefs, "enumDefs"); rt_idx(t2048, t2049); }); std_push(t2050, t2051); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2052 = v998_k; wv t2053 = std_length(rt_def(up->v656_frames, "frames")); rt_lt(t2052, t2053); }))) {
    wv v1002_f = rt_bind(({ wv t2054 = v998_k; wv t2055 = rt_def(up->v656_frames, "frames"); rt_idx(t2054, t2055); }), 0);
    wv v1003_members = rt_arr(0, NULL, 1);
    wv v1004_pf = rt_bind(rt_get(&F_parent, ({ wv t2056 = v1002_f; wv t2057 = rt_def(up->v663_fns, "fns"); rt_idx(t2056, t2057); })), 0);
    if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2058 = v1004_pf; wv t2059 = rt_def(up->v663_fns, "fns"); rt_idx(t2058, t2059); }))))) {
      (void)({ wv t2060 = v1003_members; wv t2061 = rt_cat(3, (wv[]){ rt_s(&S552), rt_text(v1004_pf), rt_s(&S616) }); std_push(t2060, t2061); });
    }
    wv v1005_j = rt_i(0);
    while (rt_cond(({ wv t2062 = v1005_j; wv t2063 = std_length(rt_def(up->v662_decls, "decls")); rt_lt(t2062, t2063); }))) {
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(({ wv t2066 = rt_get(&F_func, ({ wv t2064 = v1005_j; wv t2065 = rt_def(up->v662_decls, "decls"); rt_idx(t2064, t2065); })); wv t2067 = v1002_f; rt_eq(t2066, t2067); }), "&&") && rt_bool(rt_get(&F_captured, ({ wv t2068 = v1005_j; wv t2069 = rt_def(up->v662_decls, "decls"); rt_idx(t2068, t2069); })), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_kind, ({ wv t2070 = v1005_j; wv t2071 = rt_def(up->v662_decls, "decls"); rt_idx(t2070, t2071); })), rt_s(&S33)), "&&")))) {
        (void)({ wv t2074 = v1003_members; wv t2075 = rt_cat(3, (wv[]){ rt_s(&S529), rt_text(rt_get(&F_cname, ({ wv t2072 = v1005_j; wv t2073 = rt_def(up->v662_decls, "decls"); rt_idx(t2072, t2073); }))), rt_s(&S14) }); std_push(t2074, t2075); });
      }
      v1005_j = rt_add(v1005_j, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v1003_members), rt_i(0)))) {
      (void)std_push(v1003_members, rt_s(&S617));
    }
    (void)({ wv t2078 = v997_all; wv t2079 = ({ wv t2076 = rt_text(v1002_f); wv t2077 = rt_text(std_join(v1003_members, rt_s(&S0))); rt_cat(5, (wv[]){ rt_s(&S552), t2076, rt_s(&S618), t2077, rt_s(&S522) }); }); std_push(t2078, t2079); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2080 = v998_k; wv t2081 = std_length(rt_def(up->v655_globals, "globals")); rt_lt(t2080, t2081); }))) {
    (void)({ wv t2084 = v997_all; wv t2085 = rt_cat(3, (wv[]){ rt_s(&S555), rt_text(({ wv t2082 = v998_k; wv t2083 = rt_def(up->v655_globals, "globals"); rt_idx(t2082, t2083); })), rt_s(&S14) }); std_push(t2084, t2085); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2086 = v998_k; wv t2087 = std_length(rt_def(up->v657_functions, "functions")); rt_lt(t2086, t2087); }))) {
    (void)({ wv t2090 = v997_all; wv t2091 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2088 = v998_k; wv t2089 = rt_def(up->v657_functions, "functions"); rt_idx(t2088, t2089); }))), rt_s(&S14) }); std_push(t2090, t2091); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2092 = v998_k; wv t2093 = std_length(rt_def(up->v657_functions, "functions")); rt_lt(t2092, t2093); }))) {
    (void)std_push(v997_all, rt_s(&S13));
    (void)({ wv t2096 = v997_all; wv t2097 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2094 = v998_k; wv t2095 = rt_def(up->v657_functions, "functions"); rt_idx(t2094, t2095); }))), rt_s(&S619) }); std_push(t2096, t2097); });
    wv v1006_lines = rt_bind(rt_get(&F_lines, ({ wv t2098 = v998_k; wv t2099 = rt_def(up->v657_functions, "functions"); rt_idx(t2098, t2099); })), 0);
    wv v1007_j = rt_i(0);
    while (rt_cond(({ wv t2100 = v1007_j; wv t2101 = std_length(v1006_lines); rt_lt(t2100, t2101); }))) {
      (void)({ wv t2102 = v997_all; wv t2103 = rt_idx(v1007_j, v1006_lines); std_push(t2102, t2103); });
      v1007_j = rt_add(v1007_j, rt_i(1));
    }
    (void)std_push(v997_all, rt_s(&S15));
    v998_k = rt_add(v998_k, rt_i(1));
  }
  (void)std_push(v997_all, rt_s(&S13));
  (void)std_push(v997_all, rt_s(&S620));
  if (rt_cond(rt_gt(std_length(rt_def(up->v652_fields, "fields")), rt_i(0)))) {
    wv v1008_refs = rt_arr(0, NULL, 1);
    v998_k = rt_i(0);
    while (rt_cond(({ wv t2104 = v998_k; wv t2105 = std_length(rt_def(up->v652_fields, "fields")); rt_lt(t2104, t2105); }))) {
      (void)({ wv t2108 = v1008_refs; wv t2109 = rt_cat(2, (wv[]){ rt_s(&S519), rt_text(({ wv t2106 = v998_k; wv t2107 = rt_def(up->v652_fields, "fields"); rt_idx(t2106, t2107); })) }); std_push(t2108, t2109); });
      v998_k = rt_add(v998_k, rt_i(1));
    }
    (void)({ wv t2112 = v997_all; wv t2113 = ({ wv t2110 = rt_text(std_length(rt_def(up->v652_fields, "fields"))); wv t2111 = rt_text(std_join(v1008_refs, rt_s(&S415))); rt_cat(5, (wv[]){ rt_s(&S621), t2110, rt_s(&S622), t2111, rt_s(&S623) }); }); std_push(t2112, t2113); });
  }
  if (rt_cond(rt_gt(std_length(rt_def(up->v655_globals, "globals")), rt_i(0)))) {
    wv v1009_refs = rt_arr(0, NULL, 1);
    v998_k = rt_i(0);
    while (rt_cond(({ wv t2114 = v998_k; wv t2115 = std_length(rt_def(up->v655_globals, "globals")); rt_lt(t2114, t2115); }))) {
      (void)({ wv t2118 = v1009_refs; wv t2119 = rt_cat(2, (wv[]){ rt_s(&S91), rt_text(({ wv t2116 = v998_k; wv t2117 = rt_def(up->v655_globals, "globals"); rt_idx(t2116, t2117); })) }); std_push(t2118, t2119); });
      v998_k = rt_add(v998_k, rt_i(1));
    }
    (void)({ wv t2122 = v997_all; wv t2123 = ({ wv t2120 = rt_text(std_length(rt_def(up->v655_globals, "globals"))); wv t2121 = rt_text(std_join(v1009_refs, rt_s(&S415))); rt_cat(5, (wv[]){ rt_s(&S624), t2120, rt_s(&S625), t2121, rt_s(&S623) }); }); std_push(t2122, t2123); });
  }
  v998_k = rt_i(0);
  while (rt_cond(({ wv t2124 = v998_k; wv t2125 = std_length(rt_def(up->v658_out, "out")); rt_lt(t2124, t2125); }))) {
    (void)({ wv t2128 = v997_all; wv t2129 = ({ wv t2126 = v998_k; wv t2127 = rt_def(up->v658_out, "out"); rt_idx(t2126, t2127); }); std_push(t2128, t2129); });
    v998_k = rt_add(v998_k, rt_i(1));
  }
  (void)std_push(v997_all, rt_s(&S15));
  (void)std_push(v997_all, rt_s(&S13));
  RT_RET(std_join(v997_all, rt_s(&S2)));
}

static wv w7_dumpTokens(wv v1012_toks, wv v1013_indent) {
  rt_enter("dumpTokens");
  v1012_toks = rt_bind(v1012_toks, 0);
  v1013_indent = rt_bind(v1013_indent, 0);
  wv v1014_k = rt_i(0);
  while (rt_cond(({ wv t2140 = v1014_k; wv t2141 = std_length(v1012_toks); rt_lt(t2140, t2141); }))) {
    wv v1015_tk = rt_bind(rt_idx(v1014_k, v1012_toks), 0);
    wv v1016_line = ({ wv t2142 = rt_text(v1013_indent); wv t2143 = rt_text(rt_get(&F_t, v1015_tk)); wv t2144 = rt_text(rt_get(&F_pos, v1015_tk)); wv t2145 = rt_text(rt_get(&F_end, v1015_tk)); wv t2146 = rt_text(rt_get(&F_sp, v1015_tk)); rt_cat(8, (wv[]){ t2142, t2143, rt_s(&S0), t2144, rt_s(&S0), t2145, rt_s(&S0), t2146 }); });
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1015_tk), rt_s(&S137)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1015_tk), rt_s(&S156)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1015_tk), rt_s(&S111)), "||")))) {
      v1016_line = ({ wv t2147 = v1016_line; wv t2148 = rt_cat(2, (wv[]){ rt_s(&S0), rt_text(rt_get(&F_v, v1015_tk)) }); rt_add(t2147, t2148); });
    } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1015_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1015_tk), rt_s(&S148)), "||")))) {
      v1016_line = ({ wv t2151 = v1016_line; wv t2152 = ({ wv t2149 = rt_text(rt_get(&F_text, v1015_tk)); wv t2150 = rt_text(rt_get(&F_v, v1015_tk)); rt_cat(4, (wv[]){ rt_s(&S0), t2149, rt_s(&S0), t2150 }); }); rt_add(t2151, t2152); });
    }
    (void)std_print(v1016_line);
    if (rt_cond(rt_eq(rt_get(&F_t, v1015_tk), rt_s(&S172)))) {
      wv v1017_p = rt_i(0);
      while (rt_cond(({ wv t2153 = v1017_p; wv t2154 = std_length(rt_get(&F_parts, v1015_tk)); rt_lt(t2153, t2154); }))) {
        wv v1018_part = rt_bind(({ wv t2155 = v1017_p; wv t2156 = rt_get(&F_parts, v1015_tk); rt_idx(t2155, t2156); }), 0);
        if (rt_cond(rt_eq(std_typeOf(v1018_part), rt_s(&S186)))) {
          (void)std_print(({ wv t2157 = rt_text(v1013_indent); wv t2158 = rt_text(rt_chk(w1_quote(v1018_part), "dumpTokens")); rt_cat(3, (wv[]){ t2157, rt_s(&S626), t2158 }); }));
        } else {
          (void)std_print(({ wv t2159 = rt_text(v1013_indent); wv t2160 = rt_text(rt_get(&F_pos, v1018_part)); rt_cat(3, (wv[]){ t2159, rt_s(&S627), t2160 }); }));
          (void)({ wv t2161 = rt_get(&F_tokens, v1018_part); wv t2162 = rt_add(v1013_indent, rt_s(&S628)); rt_chk(w7_dumpTokens(t2161, t2162), "dumpTokens"); });
          (void)std_print(rt_cat(2, (wv[]){ rt_text(v1013_indent), rt_s(&S629) }));
        }
        v1017_p = rt_add(v1017_p, rt_i(1));
      }
    }
    v1014_k = rt_add(v1014_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w7_sorted(wv v1019_xs) {
  rt_enter("sorted");
  v1019_xs = rt_bind(v1019_xs, 0);
  wv v1020_out = rt_arr(0, NULL, 1);
  wv v1021_k = rt_i(0);
  while (rt_cond(({ wv t2163 = v1021_k; wv t2164 = std_length(v1019_xs); rt_lt(t2163, t2164); }))) {
    (void)({ wv t2165 = v1020_out; wv t2166 = rt_idx(v1021_k, v1019_xs); std_push(t2165, t2166); });
    wv v1022_j = rt_sub(std_length(v1020_out), rt_i(1));
    while (rt_cond(rt_b(rt_bool(rt_gt(v1022_j, rt_i(0)), "&&") && rt_bool(({ wv t2169 = ({ wv t2167 = rt_sub(v1022_j, rt_i(1)); wv t2168 = v1020_out; rt_idx(t2167, t2168); }); wv t2170 = rt_idx(v1022_j, v1020_out); rt_gt(t2169, t2170); }), "&&")))) {
      wv v1023_swap = rt_bind(rt_idx(v1022_j, v1020_out), 0);
      ({ wv t2173 = ({ wv t2171 = rt_sub(v1022_j, rt_i(1)); wv t2172 = v1020_out; rt_idx(t2171, t2172); }); wv t2174 = v1022_j; wv t2175 = v1020_out; rt_set_idx(t2173, t2174, t2175); });
      ({ wv t2176 = v1023_swap; wv t2177 = rt_sub(v1022_j, rt_i(1)); wv t2178 = v1020_out; rt_set_idx(t2176, t2177, t2178); });
      v1022_j = rt_sub(v1022_j, rt_i(1));
    }
    v1021_k = rt_add(v1021_k, rt_i(1));
  }
  RT_RET(v1020_out);
}

static wv w7_dumpValue(wv v1024_x, wv v1025_indent) {
  rt_enter("dumpValue");
  v1024_x = rt_bind(v1024_x, 0);
  v1025_indent = rt_bind(v1025_indent, 0);
  wv v1026_type = rt_bind(std_typeOf(v1024_x), 0);
  if (rt_cond(rt_eq(v1026_type, rt_s(&S186)))) {
    RT_RET(rt_chk(w1_quote(v1024_x), "dumpValue"));
  }
  wv v1027_inner = rt_add(v1025_indent, rt_s(&S517));
  if (rt_cond(rt_eq(v1026_type, rt_s(&S630)))) {
    if (rt_cond(rt_eq(std_length(v1024_x), rt_i(0)))) {
      RT_RET(rt_s(&S631));
    }
    wv v1028_out = rt_s(&S98);
    wv v1029_k = rt_i(0);
    while (rt_cond(({ wv t2179 = v1029_k; wv t2180 = std_length(v1024_x); rt_lt(t2179, t2180); }))) {
      v1028_out = ({ wv t2185 = ({ wv t2181 = rt_add(v1028_out, rt_s(&S2)); wv t2182 = v1027_inner; rt_add(t2181, t2182); }); wv t2186 = ({ wv t2183 = rt_idx(v1029_k, v1024_x); wv t2184 = v1027_inner; rt_chk(w7_dumpValue(t2183, t2184), "dumpValue"); }); rt_add(t2185, t2186); });
      v1029_k = rt_add(v1029_k, rt_i(1));
    }
    RT_RET(rt_add(({ wv t2187 = rt_add(v1028_out, rt_s(&S2)); wv t2188 = v1025_indent; rt_add(t2187, t2188); }), rt_s(&S99)));
  }
  if (rt_cond(rt_eq(v1026_type, rt_s(&S632)))) {
    wv v1030_keys = rt_bind(rt_chk(w7_sorted(std_fields(v1024_x)), "dumpValue"), 0);
    wv v1031_out = rt_s(&S100);
    wv v1032_k = rt_i(0);
    while (rt_cond(({ wv t2189 = v1032_k; wv t2190 = std_length(v1030_keys); rt_lt(t2189, t2190); }))) {
      wv v1033_key = rt_bind(rt_idx(v1032_k, v1030_keys), 0);
      v1031_out = ({ wv t2197 = ({ wv t2193 = v1031_out; wv t2194 = ({ wv t2191 = rt_text(v1027_inner); wv t2192 = rt_text(v1033_key); rt_cat(4, (wv[]){ rt_s(&S2), t2191, t2192, rt_s(&S29) }); }); rt_add(t2193, t2194); }); wv t2198 = ({ wv t2195 = rt_idx(v1033_key, v1024_x); wv t2196 = v1027_inner; rt_chk(w7_dumpValue(t2195, t2196), "dumpValue"); }); rt_add(t2197, t2198); });
      v1032_k = rt_add(v1032_k, rt_i(1));
    }
    RT_RET(({ wv t2199 = v1031_out; wv t2200 = rt_cat(3, (wv[]){ rt_s(&S2), rt_text(v1025_indent), rt_s(&S15) }); rt_add(t2199, t2200); }));
  }
  RT_RET(std_toString(v1024_x));
}

static wv w8_dirOf(wv v1034_path) {
  rt_enter("dirOf");
  v1034_path = rt_bind(v1034_path, 0);
  wv v1035_cs = rt_bind(std_split(v1034_path, rt_s(&S13)), 0);
  wv v1036_k = rt_sub(std_length(v1035_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_ge(v1036_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1036_k, v1035_cs), rt_s(&S87)), "&&")))) {
    v1036_k = rt_sub(v1036_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2201 = v1035_cs; wv t2202 = rt_add(v1036_k, rt_i(1)); std_slice(t2201, rt_i(0), t2202); }), rt_s(&S13)));
}

static wv w8_normalize(wv v1037_path) {
  rt_enter("normalize");
  v1037_path = rt_bind(v1037_path, 0);
  wv v1038_segs = rt_bind(std_split(v1037_path, rt_s(&S87)), 0);
  wv v1039_out = rt_arr(0, NULL, 1);
  wv v1040_k = rt_i(0);
  while (rt_cond(({ wv t2203 = v1040_k; wv t2204 = std_length(v1038_segs); rt_lt(t2203, t2204); }))) {
    wv v1041_seg = rt_bind(rt_idx(v1040_k, v1038_segs), 0);
    v1040_k = rt_add(v1040_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(v1041_seg, rt_s(&S129)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1041_seg, rt_s(&S13)), "&&") && rt_bool(rt_gt(std_length(v1039_out), rt_i(0)), "&&")), "||")))) {
      continue;
    }
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1041_seg, rt_s(&S237)), "&&") && rt_bool(rt_gt(std_length(v1039_out), rt_i(0)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1039_out), rt_s(&S237)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1039_out), rt_s(&S13)), "&&")))) {
      (void)std_pop(v1039_out);
    } else {
      (void)std_push(v1039_out, v1041_seg);
    }
  }
  RT_RET(std_join(v1039_out, rt_s(&S87)));
}

static wv w8_with(wv v1042_xs, wv v1043_x) {
  rt_enter("with");
  v1042_xs = rt_bind(v1042_xs, 0);
  v1043_x = rt_bind(v1043_x, 0);
  wv v1044_out = rt_arr(0, NULL, 1);
  wv v1045_k = rt_i(0);
  while (rt_cond(({ wv t2205 = v1045_k; wv t2206 = std_length(v1042_xs); rt_lt(t2205, t2206); }))) {
    (void)({ wv t2207 = v1044_out; wv t2208 = rt_idx(v1045_k, v1042_xs); std_push(t2207, t2208); });
    v1045_k = rt_add(v1045_k, rt_i(1));
  }
  (void)std_push(v1044_out, v1043_x);
  RT_RET(v1044_out);
}

static wv w8_failIn(wv v1046_file, wv v1047_chars, wv v1048_pos, wv v1049_message) {
  rt_enter("failIn");
  v1046_file = rt_bind(v1046_file, 0);
  v1047_chars = rt_bind(v1047_chars, 0);
  v1048_pos = rt_bind(v1048_pos, 0);
  v1049_message = rt_bind(v1049_message, 0);
  (void)rt_chk(w2_setSource(v1046_file, v1047_chars), "failIn");
  (void)rt_chk(w2_abort(v1048_pos, v1049_message), "failIn");
  RT_RET(RT_NULL);
}

static wv w8_loadProgram(wv v1050_entryFile, wv v1051_entryChars, wv p_v1052_libDir, wv p_v1053_searched) {
  rt_enter("loadProgram");
  struct fr212 fr = { 0 };
  v1050_entryFile = rt_bind(v1050_entryFile, 0);
  fr.v1052_libDir = rt_bind(p_v1052_libDir, 0);
  fr.v1053_searched = rt_bind(p_v1053_searched, 0);
  fr.v1054_modules = rt_arr(0, NULL, 1);
  wv v1058_path = rt_bind(rt_chk(w8_normalize(std_join(std_split(v1050_entryFile, rt_s(&S19)), rt_s(&S87))), "loadProgram"), 0);
  (void)({ wv t2271 = v1050_entryFile; wv t2272 = v1051_entryChars; wv t2273 = v1058_path; wv t2274 = rt_arr(1, (wv[]){ v1058_path }, 0); rt_chk(s1057_visit(&fr, t2271, t2272, t2273, t2274), "loadProgram"); });
  RT_RET(fr.v1054_modules);
}

static wv s1055_target(struct fr212 *up, wv v1059_s, wv v1060_file, wv v1061_chars, wv v1062_path) {
  rt_enter("target");
  v1059_s = rt_bind(v1059_s, 0);
  v1060_file = rt_bind(v1060_file, 0);
  v1061_chars = rt_bind(v1061_chars, 0);
  v1062_path = rt_bind(v1062_path, 0);
  if (rt_cond(rt_ne(std_at(rt_get(&F_path, v1059_s), rt_i(0)), rt_s(&S105)))) {
    RT_RET(rt_chk(w8_normalize(({ wv t2209 = rt_chk(w8_dirOf(v1062_path), "target"); wv t2210 = rt_get(&F_path, v1059_s); rt_add(t2209, t2210); })), "target"));
  }
  if (rt_cond(rt_eq(up->v1052_libDir, RT_NULL))) {
    (void)({ wv t2211 = v1060_file; wv t2212 = v1061_chars; wv t2213 = rt_get(&F_pathPos, v1059_s); wv t2214 = rt_cat(2, (wv[]){ rt_s(&S633), rt_text(std_join(up->v1053_searched, rt_s(&S634))) }); rt_chk(w8_failIn(t2211, t2212, t2213, t2214), "target"); });
  }
  RT_RET(rt_chk(w8_normalize(({ wv t2217 = up->v1052_libDir; wv t2218 = ({ wv t2215 = rt_get(&F_path, v1059_s); wv t2216 = std_length(rt_get(&F_path, v1059_s)); std_slice(t2215, rt_i(5), t2216); }); rt_add(t2217, t2218); })), "target"));
}

static wv s1056_loaded(struct fr212 *up, wv v1063_path) {
  rt_enter("loaded");
  v1063_path = rt_bind(v1063_path, 0);
  wv v1064_k = rt_i(0);
  while (rt_cond(({ wv t2219 = v1064_k; wv t2220 = std_length(rt_def(up->v1054_modules, "modules")); rt_lt(t2219, t2220); }))) {
    if (rt_cond(({ wv t2223 = rt_get(&F_path, ({ wv t2221 = v1064_k; wv t2222 = rt_def(up->v1054_modules, "modules"); rt_idx(t2221, t2222); })); wv t2224 = v1063_path; rt_eq(t2223, t2224); }))) {
      RT_RET(v1064_k);
    }
    v1064_k = rt_add(v1064_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s1057_visit(struct fr212 *up, wv v1065_file, wv v1066_chars, wv v1067_path, wv v1068_chain) {
  rt_enter("visit");
  v1065_file = rt_bind(v1065_file, 0);
  v1066_chars = rt_bind(v1066_chars, 0);
  v1067_path = rt_bind(v1067_path, 0);
  v1068_chain = rt_bind(v1068_chain, 0);
  (void)rt_chk(w2_setSource(v1065_file, v1066_chars), "visit");
  wv v1069_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(v1066_chars), "visit")), "visit"), 0);
  wv v1070_imports = rt_arr(0, NULL, 1);
  wv v1071_k = rt_i(0);
  while (rt_cond(({ wv t2225 = v1071_k; wv t2226 = std_length(rt_get(&F_stmts, v1069_program)); rt_lt(t2225, t2226); }))) {
    wv v1074_s = rt_bind(({ wv t2227 = v1071_k; wv t2228 = rt_get(&F_stmts, v1069_program); rt_idx(t2227, t2228); }), 0);
    v1071_k = rt_add(v1071_k, rt_i(1));
    if (rt_cond(rt_ne(rt_get(&F_k, v1074_s), rt_s(&S47)))) {
      continue;
    }
    wv v1075_libPath = rt_bind(rt_chk(s1055_target(up, v1074_s, v1065_file, v1066_chars, v1067_path), "visit"), 0);
    wv v1076_at = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t2229 = v1076_at; wv t2230 = std_length(v1068_chain); rt_lt(t2229, t2230); }), "&&") && rt_bool(({ wv t2231 = rt_idx(v1076_at, v1068_chain); wv t2232 = v1075_libPath; rt_ne(t2231, t2232); }), "&&")))) {
      v1076_at = rt_add(v1076_at, rt_i(1));
    }
    if (rt_cond(({ wv t2233 = v1076_at; wv t2234 = std_length(v1068_chain); rt_lt(t2233, t2234); }))) {
      wv v1078_cycle = rt_bind(({ wv t2238 = ({ wv t2235 = v1068_chain; wv t2236 = v1076_at; wv t2237 = std_length(v1068_chain); std_slice(t2235, t2236, t2237); }); wv t2239 = v1075_libPath; rt_chk(w8_with(t2238, t2239), "visit"); }), 0);
      (void)({ wv t2240 = v1065_file; wv t2241 = v1066_chars; wv t2242 = rt_get(&F_pathPos, v1074_s); wv t2243 = rt_cat(2, (wv[]){ rt_s(&S635), rt_text(std_join(v1078_cycle, rt_s(&S636))) }); rt_chk(w8_failIn(t2240, t2241, t2242, t2243), "visit"); });
    }
    wv v1077_lib = rt_bind(rt_chk(s1056_loaded(up, v1075_libPath), "visit"), 1);
    if (rt_cond(rt_eq(v1077_lib, RT_NULL))) {
      wv v1079_missing = rt_bind((rt_cond(rt_eq(std_at(rt_get(&F_path, v1074_s), rt_i(0)), rt_s(&S105))) ? ({ wv t2246 = rt_text(({ wv t2244 = rt_get(&F_path, v1074_s); wv t2245 = std_length(rt_get(&F_path, v1074_s)); std_slice(t2244, rt_i(5), t2245); })); wv t2247 = rt_text(v1075_libPath); rt_cat(5, (wv[]){ rt_s(&S637), t2246, rt_s(&S638), t2247, rt_s(&S16) }); }) : rt_cat(3, (wv[]){ rt_s(&S639), rt_text(v1075_libPath), rt_s(&S113) })), 0);
      wv t2248 = std_readFile(v1075_libPath);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          (void)({ wv t2249 = v1065_file; wv t2250 = v1066_chars; wv t2251 = rt_get(&F_pathPos, v1074_s); wv t2252 = v1079_missing; rt_chk(w8_failIn(t2249, t2250, t2251, t2252), "visit"); });
          t2248 = RT_NULL;
        }
      }
      wv v1080_text = rt_bind(t2248, 0);
      wv v1081_cs = rt_bind(std_split(v1080_text, rt_s(&S13)), 0);
      wv v1082_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1081_cs), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1081_cs), rt_s(&S640)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
      v1077_lib = rt_bind(({ wv t2256 = v1075_libPath; wv t2257 = ({ wv t2253 = v1081_cs; wv t2254 = v1082_bom; wv t2255 = std_length(v1081_cs); std_slice(t2253, t2254, t2255); }); wv t2258 = v1075_libPath; wv t2259 = rt_chk(w8_with(v1068_chain, v1075_libPath), "visit"); rt_chk(s1057_visit(up, t2256, t2257, t2258, t2259), "visit"); }), 1);
    }
    (void)({ wv t2260 = v1070_imports; wv t2261 = rt_rec(0, 2, K58, (wv[]){ v1074_s, v1077_lib }); std_push(t2260, t2261); });
  }
  wv v1072_refTargets = rt_arr(0, NULL, 1);
  wv v1073_mod = ({ wv t2262 = v1065_file; wv t2263 = v1066_chars; wv t2264 = v1067_path; wv t2265 = v1069_program; wv t2266 = v1070_imports; wv t2267 = rt_arr(0, NULL, 0); wv t2268 = v1072_refTargets; rt_rec(1, 8, K59, (wv[]){ t2262, t2263, t2264, t2265, t2266, t2267, rt_b(0), t2268 }); });
  (void)({ wv t2269 = rt_def(up->v1054_modules, "modules"); wv t2270 = v1073_mod; std_push(t2269, t2270); });
  RT_RET(rt_sub(std_length(rt_def(up->v1054_modules, "modules")), rt_i(1)));
}

static wv w_die(void) {
  rt_enter("die");
  (void)std_eprint(rt_def(w_usage, "usage"));
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w_load(wv v1083_file) {
  rt_enter("load");
  v1083_file = rt_bind(v1083_file, 0);
  wv t2275 = std_readFile(v1083_file);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S646), rt_text(v1083_file) }));
      rt_exit(rt_i(2));
      t2275 = RT_NULL;
    }
  }
  wv v1084_text = rt_bind(t2275, 0);
  wv v1085_chars = rt_bind(std_split(v1084_text, rt_s(&S13)), 0);
  wv v1086_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1085_chars), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1085_chars), rt_s(&S640)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
  (void)({ wv t2279 = v1083_file; wv t2280 = ({ wv t2276 = v1085_chars; wv t2277 = v1086_bom; wv t2278 = std_length(v1085_chars); std_slice(t2276, t2277, t2278); }); rt_chk(w2_setSource(t2279, t2280), "load"); });
  RT_RET(RT_NULL);
}

static wv w_onWindows(void) {
  rt_enter("onWindows");
  wv t2285 = rt_s(&S654);
  wv t2286 = std_env(t2285);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2286 = rt_s(&S13);
    }
  }
  wv v1088_os = rt_bind(t2286, 0);
  RT_RET(rt_eq(v1088_os, rt_s(&S655)));
}

static wv w_tryRead(wv v1089_path) {
  rt_enter("tryRead");
  v1089_path = rt_bind(v1089_path, 0);
  wv t2293 = std_readFile(v1089_path);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2293 = RT_NULL;
    }
  }
  wv v1090_text = rt_bind(t2293, 0);
  RT_RET(v1090_text);
}

static wv w_programDir(void) {
  rt_enter("programDir");
  wv v1091_cs = rt_bind(std_split(std_programPath(), rt_s(&S13)), 0);
  wv v1092_k = rt_sub(std_length(v1091_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v1092_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1092_k, v1091_cs), rt_s(&S87)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(v1092_k, v1091_cs), rt_s(&S19)), "&&")))) {
    v1092_k = rt_sub(v1092_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2294 = v1091_cs; wv t2295 = rt_add(v1092_k, rt_i(1)); std_slice(t2294, rt_i(0), t2295); }), rt_s(&S13)));
}

static wv w_readRuntime(void) {
  rt_enter("readRuntime");
  wv v1093_text = rt_bind(rt_chk(w_tryRead(rt_def(w_runtimePath, "runtimePath")), "readRuntime"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1093_text, RT_NULL), "||") || rt_bool(rt_def(w_runtimeGiven, "runtimeGiven"), "||")))) {
    RT_RET(v1093_text);
  }
  wv v1094_dir = rt_bind(rt_chk(w_programDir(), "readRuntime"), 0);
  wv v1095_here = rt_bind(rt_chk(w_tryRead(({ wv t2296 = v1094_dir; wv t2297 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2296, t2297); })), "readRuntime"), 0);
  RT_RET((rt_cond(rt_ne(v1095_here, RT_NULL)) ? v1095_here : rt_chk(w_tryRead(({ wv t2298 = rt_add(v1094_dir, rt_s(&S662)); wv t2299 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2298, t2299); })), "readRuntime")));
}

static wv w_findLib(void) {
  rt_enter("findLib");
  wv v1096_dir = rt_bind(std_split(rt_chk(w_programDir(), "findLib"), rt_s(&S19)), 0);
  wv v1097_slashed = rt_bind(std_join(v1096_dir, rt_s(&S87)), 0);
  wv v1098_candidates = rt_bind((rt_cond(rt_ne(rt_def(w_libGiven, "libGiven"), rt_s(&S13))) ? rt_arr(1, (wv[]){ std_join(std_split(rt_def(w_libGiven, "libGiven"), rt_s(&S19)), rt_s(&S87)) }, 0) : ({ wv t2300 = rt_add(v1097_slashed, rt_s(&S663)); wv t2301 = rt_add(v1097_slashed, rt_s(&S664)); rt_arr(2, (wv[]){ t2300, t2301 }, 0); })), 0);
  wv v1099_k = rt_i(0);
  while (rt_cond(({ wv t2302 = v1099_k; wv t2303 = std_length(v1098_candidates); rt_lt(t2302, t2303); }))) {
    wv v1100_c = rt_bind(rt_idx(v1099_k, v1098_candidates), 0);
    (void)({ wv t2304 = rt_def(w_searched, "searched"); wv t2305 = v1100_c; std_push(t2304, t2305); });
    wv t2306 = std_listDir(v1100_c);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        t2306 = RT_NULL;
      }
    }
    wv v1101_names = rt_bind(t2306, 0);
    if (rt_cond(rt_ne(v1101_names, RT_NULL))) {
      RT_RET((rt_cond(rt_eq(({ wv t2307 = v1100_c; wv t2308 = std_length(v1100_c); std_slice(t2307, rt_i(-1), t2308); }), rt_s(&S87))) ? v1100_c : rt_add(v1100_c, rt_s(&S87))));
    }
    v1099_k = rt_add(v1099_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static void wvt_main(void) {
  rt_syms(88, (wstr *const[]){ &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars, &F_word, &F_t, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts, &F_tokens, &F_k, &F_e, &F_handler, &F_items, &F_fields, &F_index, &F_arr, &F_rec, &F_l, &F_r, &F_c, &F_a, &F_b, &F_stmts, &F_param, &F_paramPos, &F_body, &F_neg, &F_cond, &F_isVar, &F_isRef, &F_name, &F_namePos, &F_op, &F_target, &F_code, &F_arg, &F_paren, &F_path, &F_pathPos, &F_names, &F_params, &F_members, &F_value, &F_close, &F_field, &F_callee, &F_args, &F_values, &F_subject, &F_cases, &F_els, &F_branches, &F_arity, &F_mod, &F_parent, &F_kind, &F_visibleAt, &F_fnId, &F_lib, &F_id, &F_func, &F_top, &F_loops, &F_key, &F_exports, &F_refTargets, &F_useMod, &F_imports, &F_node, &F_notLibrary, &F_program, &F_children, &F_d, &F_depth, &F_cname, &F_captured, &F_fnIndex, &F_pre, &F_cs, &F_proto, &F_lines, &F_js });
  rt_roots(53, (wv *const[]){ &w2_srcFile, &w2_srcChars, &w2_warnings, &w3_keywords, &w3_reserved, &w3_bangWords, &w3_setOps, &w3_ops, &w3_maxInt, &w4_exprStart, &w4_onfailOk, &w4_callees, &w4_exportable, &w5_stdlib, &w5_neverCompletes, &w5_onfailValueUsed, &w5_libraryTop, &w6_binOps, &w6_binFns, &w6_scalarKinds, &w6_plainKinds, &w6_signalers, &w6_constK, &w6_pureK, &w6_effectK, &w_usage, &w_argv, &w_mode, &w_input, &w_output, &w_runtimePath, &w_cc, &w_libGiven, &w_arg, &w_inspect, &w_dotWvt, &w_runtimeGiven, &w_searched, &w_libDir, &w_modules, &w_problems, &w_runtime, &w_code, &w_dotExe, &w_cFile, &w_parts, &w_ccName, &w_flags, &w_status, &v1087_a, &v1102_tokens, &v1103_program, &v1104_p });
  w2_srcFile = rt_s(&S13);
  w2_srcChars = rt_arr(0, NULL, 0);
  w2_warnings = rt_arr(0, NULL, 1);
  w3_keywords = rt_arr(25, (wv[]){ rt_s(&S33), rt_s(&S34), rt_s(&S35), rt_s(&S36), rt_s(&S37), rt_s(&S38), rt_s(&S39), rt_s(&S40), rt_s(&S41), rt_s(&S42), rt_s(&S43), rt_s(&S44), rt_s(&S45), rt_s(&S46), rt_s(&S47), rt_s(&S48), rt_s(&S49), rt_s(&S50), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S54), rt_s(&S55), rt_s(&S56), rt_s(&S57) }, 0);
  w3_reserved = ({ wv t68 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S58), rt_s(&S59) }); wv t69 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S60), rt_s(&S61) }); wv t70 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S62), rt_s(&S63) }); rt_arr(3, (wv[]){ t68, t69, t70 }, 0); });
  w3_bangWords = rt_arr(2, (wv[]){ rt_s(&S39), rt_s(&S41) }, 0);
  w3_setOps = rt_arr(11, (wv[]){ rt_s(&S64), rt_s(&S65), rt_s(&S66), rt_s(&S67), rt_s(&S68), rt_s(&S69), rt_s(&S70), rt_s(&S71), rt_s(&S72), rt_s(&S73), rt_s(&S74) }, 0);
  w3_ops = rt_arr(31, (wv[]){ rt_s(&S75), rt_s(&S76), rt_s(&S77), rt_s(&S78), rt_s(&S79), rt_s(&S80), rt_s(&S81), rt_s(&S82), rt_s(&S83), rt_s(&S84), rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S89), rt_s(&S90), rt_s(&S91), rt_s(&S92), rt_s(&S32), rt_s(&S93), rt_s(&S94), rt_s(&S95), rt_s(&S28), rt_s(&S14), rt_s(&S96), rt_s(&S97), rt_s(&S16), rt_s(&S98), rt_s(&S99), rt_s(&S100), rt_s(&S15) }, 0);
  w3_maxInt = rt_i(9007199254740991);
  w4_exprStart = rt_arr(19, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S172), rt_s(&S49), rt_s(&S50), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S137), rt_s(&S156), rt_s(&S97), rt_s(&S98), rt_s(&S100), rt_s(&S39), rt_s(&S176), rt_s(&S44), rt_s(&S85), rt_s(&S94), rt_s(&S93) }, 0);
  w4_onfailOk = rt_arr(6, (wv[]){ rt_s(&S177), rt_s(&S178), rt_s(&S38), rt_s(&S34), rt_s(&S57), rt_s(&S54) }, 0);
  w4_callees = rt_arr(2, (wv[]){ rt_s(&S137), rt_s(&S156) }, 0);
  w4_exportable = rt_arr(5, (wv[]){ rt_s(&S33), rt_s(&S35), rt_s(&S36), rt_s(&S37), rt_s(&S46) }, 0);
  w5_stdlib = ({ wv t534 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S337), rt_i(1) }); wv t535 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S338), rt_i(1) }); wv t536 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S339), rt_i(1) }); wv t537 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S340), rt_i(0) }); wv t538 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S341), rt_i(0) }); wv t539 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S342), rt_i(1) }); wv t540 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S343), rt_i(0) }); wv t541 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S344), rt_i(0) }); wv t542 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S345), rt_i(1) }); wv t543 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S346), rt_i(2) }); wv t544 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S347), rt_i(2) }); wv t545 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S348), rt_i(1) }); wv t546 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S349), rt_i(1) }); wv t547 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S350), rt_i(1) }); wv t548 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S351), rt_s(&S35) }); wv t549 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S352), rt_i(1) }); wv t550 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S353), rt_s(&S35) }); wv t551 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S354), rt_i(2) }); wv t552 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S355), rt_i(1) }); wv t553 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S356), rt_i(2) }); wv t554 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S357), rt_i(3) }); wv t555 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S358), rt_i(1) }); wv t556 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S359), rt_i(1) }); wv t557 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S360), rt_i(2) }); wv t558 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S361), rt_i(2) }); wv t559 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S362), rt_i(1) }); wv t560 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S363), rt_i(1) }); wv t561 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S364), rt_i(1) }); wv t562 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S365), rt_i(2) }); wv t563 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S366), rt_i(1) }); wv t564 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S367), rt_i(2) }); wv t565 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S368), rt_i(1) }); wv t566 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S369), rt_i(2) }); wv t567 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S370), rt_i(1) }); wv t568 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S371), rt_i(2) }); wv t569 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S372), rt_i(2) }); wv t570 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S373), rt_i(2) }); wv t571 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S374), rt_i(1) }); wv t572 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S375), rt_i(1) }); wv t573 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S376), rt_i(1) }); wv t574 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S377), rt_i(1) }); wv t575 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S378), rt_i(1) }); wv t576 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S379), rt_i(1) }); wv t577 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S380), rt_i(1) }); wv t578 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S381), rt_i(1) }); wv t579 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S382), rt_i(1) }); wv t580 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S383), rt_i(1) }); wv t581 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S384), rt_i(0) }); wv t582 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S385), rt_i(0) }); wv t583 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S386), rt_i(1) }); wv t584 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S387), rt_i(1) }); wv t585 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S388), rt_i(2) }); wv t586 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S389), rt_i(1) }); rt_arr(53, (wv[]){ t534, t535, t536, t537, t538, t539, t540, t541, t542, t543, t544, t545, t546, t547, t548, t549, t550, t551, t552, t553, t554, t555, t556, t557, t558, t559, t560, t561, t562, t563, t564, t565, t566, t567, t568, t569, t570, t571, t572, t573, t574, t575, t576, t577, t578, t579, t580, t581, t582, t583, t584, t585, t586 }, 0); });
  w5_neverCompletes = rt_arr(6, (wv[]){ rt_s(&S34), rt_s(&S57), rt_s(&S54), rt_s(&S55), rt_s(&S42), rt_s(&S43) }, 0);
  w5_onfailValueUsed = rt_arr(5, (wv[]){ rt_s(&S178), rt_s(&S38), rt_s(&S34), rt_s(&S57), rt_s(&S54) }, 0);
  w5_libraryTop = rt_arr(6, (wv[]){ rt_s(&S202), rt_s(&S47), rt_s(&S33), rt_s(&S178), rt_s(&S46), rt_s(&S205) }, 0);
  w6_binOps = rt_arr(17, (wv[]){ rt_s(&S84), rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S75), rt_s(&S76), rt_s(&S77), rt_s(&S89), rt_s(&S90), rt_s(&S78), rt_s(&S79), rt_s(&S91), rt_s(&S92), rt_s(&S32), rt_s(&S82), rt_s(&S83) }, 0);
  w6_binFns = rt_arr(17, (wv[]){ rt_s(&S488), rt_s(&S489), rt_s(&S490), rt_s(&S491), rt_s(&S492), rt_s(&S493), rt_s(&S494), rt_s(&S495), rt_s(&S496), rt_s(&S497), rt_s(&S498), rt_s(&S499), rt_s(&S500), rt_s(&S501), rt_s(&S502), rt_s(&S503), rt_s(&S504) }, 0);
  w6_scalarKinds = rt_arr(9, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S172), rt_s(&S284), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S193), rt_s(&S192) }, 0);
  w6_plainKinds = rt_arr(8, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S284), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S213), rt_s(&S156) }, 0);
  w6_signalers = rt_arr(15, (wv[]){ rt_s(&S340), rt_s(&S341), rt_s(&S342), rt_s(&S344), rt_s(&S371), rt_s(&S373), rt_s(&S345), rt_s(&S346), rt_s(&S347), rt_s(&S349), rt_s(&S350), rt_s(&S352), rt_s(&S354), rt_s(&S363), rt_s(&S364) }, 0);
  w6_constK = rt_i(0);
  w6_pureK = rt_i(1);
  w6_effectK = rt_i(2);
  w_usage = rt_add(rt_add(rt_add(rt_add(rt_s(&S641), rt_s(&S642)), rt_s(&S643)), rt_s(&S644)), rt_s(&S645));
  w_argv = rt_bind(std_args(), 0);
  w_mode = rt_s(&S13);
  w_input = rt_s(&S13);
  w_output = rt_s(&S13);
  w_runtimePath = rt_s(&S13);
  w_cc = rt_s(&S13);
  w_libGiven = rt_s(&S13);
  w_arg = rt_i(0);
  while (rt_cond(({ wv t2281 = w_arg; wv t2282 = std_length(w_argv); rt_lt(t2281, t2282); }))) {
    v1087_a = rt_bind(rt_idx(w_arg, w_argv), 0);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_arg, rt_i(0)), "&&") && rt_bool(rt_eq(v1087_a, rt_s(&S647)), "&&")))) {
      w_mode = rt_bind(v1087_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v1087_a, rt_s(&S648)), "||") || rt_bool(rt_eq(v1087_a, rt_s(&S649)), "||")))) {
      w_mode = rt_bind(v1087_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1087_a, rt_s(&S650)), "||") || rt_bool(rt_eq(v1087_a, rt_s(&S651)), "||")), "||") || rt_bool(rt_eq(v1087_a, rt_s(&S652)), "||")), "||") || rt_bool(rt_eq(v1087_a, rt_s(&S653)), "||")))) {
      w_arg = rt_add(w_arg, rt_i(1));
      if (rt_cond(({ wv t2283 = w_arg; wv t2284 = std_length(w_argv); rt_ge(t2283, t2284); }))) {
        (void)rt_chk(w_die(), NULL);
      }
      if (rt_cond(rt_eq(v1087_a, rt_s(&S650)))) {
        w_output = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1087_a, rt_s(&S651)))) {
        w_runtimePath = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1087_a, rt_s(&S653)))) {
        w_libGiven = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else {
        w_cc = rt_bind(rt_idx(w_arg, w_argv), 1);
      }
    } else if (rt_cond(rt_b(rt_bool(rt_ne(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(rt_gt(std_length(v1087_a), rt_i(0)), "&&") && rt_bool(rt_eq(std_at(v1087_a, rt_i(0)), rt_s(&S85)), "&&")), "||")))) {
      (void)rt_chk(w_die(), NULL);
    } else {
      w_input = rt_bind(v1087_a, 1);
    }
    w_arg = rt_add(w_arg, rt_i(1));
  }
  w_inspect = rt_b(rt_bool(rt_eq(w_mode, rt_s(&S648)), "||") || rt_bool(rt_eq(w_mode, rt_s(&S649)), "||"));
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(w_inspect, "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "||") || rt_bool(rt_ne(w_libGiven, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S647)), "&&") && rt_bool(rt_ne(w_cc, rt_s(&S13)), "&&")), "||")))) {
    (void)rt_chk(w_die(), NULL);
  }
  w_dotWvt = rt_b(rt_bool(rt_ge(std_length(w_input), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t2287 = w_input; wv t2288 = std_length(w_input); std_slice(t2287, rt_i(-4), t2288); }), rt_s(&S232)), "&&"));
  if (rt_cond(rt_eq(w_mode, rt_s(&S647)))) {
    if (rt_cond(rt_eq(w_cc, rt_s(&S13)))) {
      wv t2289 = rt_s(&S656);
      wv t2290 = std_env(t2289);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          t2290 = rt_s(&S657);
        }
      }
      w_cc = rt_bind(t2290, 1);
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(w_output, rt_s(&S13)), "&&") && rt_bool(rt_not(w_dotWvt), "&&")))) {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S658), rt_text(w_input), rt_s(&S659) }));
      rt_exit(rt_i(2));
    }
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = ({ wv t2291 = std_slice(w_input, rt_i(0), rt_i(-4)); wv t2292 = (rt_cond(rt_chk(w_onWindows(), NULL)) ? rt_s(&S660) : rt_s(&S13)); rt_add(t2291, t2292); });
    }
  }
  w_runtimeGiven = rt_ne(w_runtimePath, rt_s(&S13));
  if (rt_cond(rt_not(w_runtimeGiven))) {
    w_runtimePath = rt_s(&S661);
  }
  w_searched = rt_arr(0, NULL, 1);
  (void)rt_chk(w_load(w_input), NULL);
  if (rt_cond(rt_eq(w_mode, rt_s(&S648)))) {
    v1102_tokens = rt_bind(rt_chk(w3_lex(w2_srcChars), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)rt_chk(w7_dumpTokens(v1102_tokens, rt_s(&S13)), NULL);
    rt_exit(rt_i(0));
  }
  if (rt_cond(rt_eq(w_mode, rt_s(&S649)))) {
    v1103_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(w2_srcChars), NULL)), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)std_print(rt_chk(w7_dumpValue(v1103_program, rt_s(&S13)), NULL));
    rt_exit(rt_i(0));
  }
  w_libDir = rt_bind(rt_chk(w_findLib(), NULL), 0);
  w_modules = rt_bind(rt_chk(w8_loadProgram(w2_srcFile, w2_srcChars, w_libDir, w_searched), NULL), 0);
  w_problems = rt_bind(rt_chk(w5_check(w_modules, rt_b(1)), NULL), 0);
  if (rt_cond(rt_gt(std_length(w_problems), rt_i(0)))) {
    (void)rt_chk(w2_flushWarnings(), NULL);
    v1104_p = rt_i(0);
    while (rt_cond(({ wv t2309 = v1104_p; wv t2310 = std_length(w_problems); rt_lt(t2309, t2310); }))) {
      (void)std_eprint(rt_chk(w2_formatDiag(rt_idx(v1104_p, w_problems)), NULL));
      v1104_p = rt_add(v1104_p, rt_i(1));
    }
    rt_exit(rt_i(2));
  }
  w_runtime = rt_bind(rt_chk(w_readRuntime(), NULL), 0);
  if (rt_cond(rt_eq(w_runtime, RT_NULL))) {
    (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S646), rt_text(w_runtimePath) }));
    rt_exit(rt_i(2));
  }
  w_code = rt_bind(rt_chk(w6_generateC(w_modules, w_runtime), NULL), 0);
  (void)rt_chk(w2_flushWarnings(), NULL);
  if (rt_cond(rt_ne(w_mode, rt_s(&S647)))) {
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = rt_add((rt_cond(w_dotWvt) ? std_slice(w_input, rt_i(0), rt_i(-4)) : w_input), rt_s(&S665));
    }
    wv t2311 = std_writeFile(w_output, w_code);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S666), rt_text(w_output) }));
        rt_exit(rt_i(2));
        t2311 = RT_NULL;
      }
    }
    (void)t2311;
    rt_exit(rt_i(0));
  }
  w_dotExe = rt_b(rt_bool(rt_ge(std_length(w_output), rt_i(4)), "&&") && rt_bool(rt_eq(rt_chk(w1_toLower(({ wv t2312 = w_output; wv t2313 = std_length(w_output); std_slice(t2312, rt_i(-4), t2313); })), NULL), rt_s(&S660)), "&&"));
  w_cFile = rt_add((rt_cond(w_dotExe) ? std_slice(w_output, rt_i(0), rt_i(-4)) : w_output), rt_s(&S665));
  wv t2314 = std_writeFile(w_cFile, w_code);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S666), rt_text(w_cFile) }));
      rt_exit(rt_i(2));
      t2314 = RT_NULL;
    }
  }
  (void)t2314;
  w_parts = rt_bind(std_split(std_join(std_split(w_cc, rt_s(&S19)), rt_s(&S87)), rt_s(&S87)), 0);
  w_ccName = rt_bind(rt_chk(w1_toLower(rt_idx(rt_i(-1), w_parts)), NULL), 0);
  w_flags = rt_arr(4, (wv[]){ rt_s(&S667), rt_s(&S650), w_output, w_cFile }, 0);
  wv t2315 = (rt_cond(rt_b(rt_bool(rt_eq(w_ccName, rt_s(&S657)), "||") || rt_bool(rt_eq(w_ccName, rt_s(&S668)), "||"))) ? rt_arr(5, (wv[]){ rt_s(&S669), rt_s(&S667), rt_s(&S650), w_output, w_cFile }, 0) : w_flags);
  wv t2316 = std_run(w_cc, t2315);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S670), rt_text(w_cc), rt_s(&S671) }));
      rt_exit(rt_i(2));
      t2316 = RT_NULL;
    }
  }
  w_status = rt_bind(t2316, 0);
  if (rt_cond(rt_ne(w_status, rt_i(0)))) {
    (void)std_eprint(({ wv t2317 = rt_text(w_cc); wv t2318 = rt_text(w_status); rt_cat(4, (wv[]){ rt_s(&S658), t2317, rt_s(&S672), t2318 }); }));
    rt_exit(rt_i(2));
  }
  rt_exit(rt_i(0));
}
