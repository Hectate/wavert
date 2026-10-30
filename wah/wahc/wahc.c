// Γ-- native runtime. This file is inlined verbatim at the top of every
// program compiled with --target c, the counterpart of js/src/runtime.js.
// It needs a GNU C compiler (clang or gcc; `zig cc` is clang).
//
// Values: a 16-byte tagged wv. int -> int64_t (checked to ±(2^53-1)),
// float -> double, purple -> immutable UTF-8 wstr, array -> warr, record ->
// wrec (keys shared by every record of one literal), lineup member -> pointer
// to a static wmem, uninvited -> T_NULL. T_UNSET marks a binding before its
// claim (temporal dead zone).
//
// Memory is allocated from an arena and never freed.
//
// Signals (WAH!) are a pending code in rt_sigcode: a scheme or stdlib function
// sets it and returns, and the caller either handles it (whine) or turns it
// into a tantrum with rt_chk (the one-frame rule, §8.2). A tantrum ends the
// program: tantrums cannot be caught yet, so nothing unwinds.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW: zig cc links shell32 by default
#include <wchar.h>
#else
#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

// ---- values ----

enum { T_UNSET, T_NULL, T_INT, T_FLOAT, T_BOOL, T_STR, T_ARR, T_REC, T_MEM };

// A purple. data holds bytes valid UTF-8 and is followed by a NUL.
typedef struct wstr {
  int64_t bytes, cps;
  int32_t ascii, sym; // sym: interned as a record key
  const char *data;
} wstr;
typedef struct warr { int64_t len, cap; int shifty; struct wv *items; } warr;
typedef struct wrec { int64_t n; int shifty; const wstr *const *keys; struct wv *vals; } wrec;
typedef struct wmem { const wstr *lineup, *name; } wmem;
typedef struct wv {
  uint32_t tag;
  union { int64_t i; double f; int b; const wstr *s; warr *a; wrec *r; const wmem *m; };
} wv;

#define RT_MAX 9007199254740991LL
static const wv RT_NULL = { .tag = T_NULL };
static const wv RT_UNSET = { .tag = T_UNSET };
static inline wv rt_i(int64_t x) { return (wv){ .tag = T_INT, .i = x }; }
static inline wv rt_f(double x) { return (wv){ .tag = T_FLOAT, .f = x }; }
static inline wv rt_b(int x) { return (wv){ .tag = T_BOOL, .b = !!x }; }
static inline wv rt_s(const wstr *s) { return (wv){ .tag = T_STR, .s = s }; }
static inline wv rt_m(const wmem *m) { return (wv){ .tag = T_MEM, .m = m }; }
#define RT_NOBODY rt_f(NAN)
#define RT_EVERYBODY rt_f(INFINITY)
static inline int isnum(wv x) { return x.tag == T_INT || x.tag == T_FLOAT; }
static inline double nv(wv x) { return x.tag == T_INT ? (double)x.i : x.f; }

// ---- memory ----

static char *ar_p, *ar_end;
static void rt_flush(void);
static void *xmalloc(size_t n) {
  void *p = malloc(n ? n : 1);
  if (!p) { rt_flush(); fputs("out of memory\n", stderr); exit(1); } // output written before the failure still appears
  return p;
}
static void *rt_alloc(size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (n > (size_t)(ar_end - ar_p)) {
    if (n >= (1 << 20)) return xmalloc(n);
    size_t chunk = 16 << 20;
    ar_p = xmalloc(chunk);
    ar_end = ar_p + chunk;
  }
  void *p = ar_p;
  ar_p += n;
  return p;
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

// ---- purples ----

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

// A new purple from n bytes of valid UTF-8.
static const wstr *str_new(const char *p, int64_t n) {
  wstr *s = rt_alloc(sizeof(wstr) + n + 1);
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

// Byte offsets of code points, for $at and $slice on non-ASCII purples. Long
// purples are cached so loops over $at stay O(1) per call.
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
    cache_off = rt_alloc(sizeof(int64_t) * s->cps);
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
      if (e == ERROR_NO_DATA || e == ERROR_BROKEN_PIPE) ExitProcess(BROKEN_PIPE);
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
      if (errno == EPIPE) _exit(BROKEN_PIPE);
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

// ---- scheme stack, tantrums, signals (§8) ----

#define RT_DEPTH 100000
static const char *rt_stack[RT_DEPTH];
static int rt_depth;
static int64_t rt_sigcode;     // a pending WAH! signal, or 0
static const char *rt_sigfrom; // the scheme or stdlib function that raised it

static _Noreturn void rt_die(const char *msg, size_t n) {
#ifdef WAH_TEST_DIE
  WAH_TEST_DIE(msg, n); // the runtime tests report a tantrum and go on
#endif
  sb b = { 0 };
  sb_puts(&b, "Waluigi was not invited to this exception\ntantrum: ");
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

// Called on entry to every scheme; RT_RET pops.
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
    case T_STR: return "purple";
    case T_NULL: return "uninvited";
    case T_ARR: return "array";
    case T_REC: return "record";
    case T_MEM: return x.m->lineup->data;
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
  if (isnan(v)) { sb_puts(b, "nobody"); return; }
  if (isinf(v)) { sb_puts(b, v > 0 ? "everybody" : "-everybody"); return; }
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
    case T_BOOL: sb_puts(b, v.b ? "real" : "fake"); return 1;
    case T_NULL: sb_puts(b, "uninvited"); return 1;
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
// Joins n purples (interpolation).
static wv rt_cat(int n, const wv *parts) {
  int64_t len = 0;
  for (int i = 0; i < n; i++) len += parts[i].s->bytes;
  wstr *s = rt_alloc(sizeof(wstr) + len + 1);
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
// so every backend gets the same bits. It also gives JavaScript's NaN results
// for 1 ** nobody and 1 ** everybody, where C's pow returns 1.
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
  if (a.tag == T_MEM && b.tag == T_MEM) rt_fail("type error: '%s' cannot order lineup members", op);
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
  if (x.tag != T_BOOL) rt_fail("type error: '%s' requires real or fake, got %s", op, type_name(x));
  return x.b;
}
static wv rt_not(wv x) { return rt_b(!rt_bool(x, "!")); }
static int rt_cond(wv x) {
  if (x.tag != T_BOOL) rt_fail("condition must be real or fake, got %s", type_name(x));
  return x.b;
}

// ---- lineups (§4.1, §7.5) ----

static _Noreturn void rt_no_alibi(wv v) {
  sb b = { 0 };
  sb_puts(&b, "suspect: no alibi for ");
  show_to(&b, v);
  rt_fail_sb(&b);
}

// ---- arrays (§10) ----

static warr *arr_new(int64_t n, int shifty) {
  warr *a = rt_alloc(sizeof(warr));
  *a = (warr){ n, n, shifty, n ? rt_alloc(sizeof(wv) * n) : NULL };
  return a;
}
// An array literal: n values, frozen unless shifty.
static wv rt_arr(int64_t n, const wv *items, int shifty) {
  warr *a = arr_new(n, shifty);
  if (n) memcpy(a->items, items, sizeof(wv) * n);
  return (wv){ .tag = T_ARR, .a = a };
}
static wv arr_copy(const warr *src, int64_t from, int64_t to, int shifty) {
  return rt_arr(to - from, src->items + from, shifty);
}

static wv rec_copy(const wrec *src, int shifty);
// Copy happens only when mutability changes (§10.4, §10.9).
static wv rt_bind(wv v, int shifty) {
  if (v.tag == T_ARR) return v.a->shifty == shifty ? v : arr_copy(v.a, 0, v.a->len, shifty);
  if (v.tag == T_REC) return v.r->shifty == shifty ? v : rec_copy(v.r, shifty);
  return v;
}

static int64_t slot(wv i, wv arr) {
  if (arr.tag != T_ARR) {
    rt_fail("type error: indexing needs an array or a record, got %s%s", type_name(arr),
      arr.tag == T_STR ? " (use $at for purple)" : "");
  }
  if (i.tag != T_INT) rt_fail("type error: an index must be an int, got %s", type_name(i));
  int64_t n = arr.a->len;
  if (i.i < -n || i.i >= n) rt_fail("index %lld is out of range for an array of length %lld", (long long)i.i, (long long)n);
  return i.i < 0 ? i.i + n : i.i;
}
static void mutable(const warr *a, const char *what) {
  if (!a->shifty) rt_fail("cannot modify a non-shifty array%s%s", what ? " with " : "", what ? what : "");
}

// ---- records (§10.7-§10.10) ----

// A record literal: keys is shared by every record of the literal.
static wv rt_rec(int shifty, int64_t n, const wstr *const *keys, const wv *vals) {
  wrec *r = rt_alloc(sizeof(wrec) + sizeof(wv) * n);
  *r = (wrec){ n, shifty, keys, (wv *)(r + 1) };
  if (n) memcpy(r->vals, vals, sizeof(wv) * n);
  return (wv){ .tag = T_REC, .r = r };
}
static wv rec_copy(const wrec *src, int shifty) { return rt_rec(shifty, src->n, src->keys, src->vals); }
static const wstr *rec_key(wv k) {
  if (k.tag != T_STR) rt_fail("type error: a record key must be a purple, got %s", type_name(k));
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
  if (!r.r->shifty) rt_fail("cannot modify a field of a non-shifty record");
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

// Indexing a record is computed field access with a purple key (§10.8).
static wv rt_idx(wv i, wv arr) {
  if (arr.tag == T_REC) return *computed(i, arr);
  int64_t k = slot(i, arr); // checks arr before it is read
  return arr.a->items[k];
}
static void rt_set_idx(wv v, wv i, wv arr) {
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

// ---- bindings, signals, exit ----

// A captured binding read before its claim has executed (§5.1).
static inline wv rt_def(wv v, const char *name) {
  if (v.tag == T_UNSET) rt_fail("'%s' was used before its claim", name);
  return v;
}

// ;c(WAH!) inside a scheme: the caller sees code c from this scheme.
static void rt_signal(wv code) {
  if (code.tag != T_INT || code.i < 1) {
    sb b = { 0 };
    sb_puts(&b, "a WAH! code must be an integer >= 1, got ");
    show_to(&b, code);
    rt_fail_sb(&b);
  }
  rt_sigcode = code.i;
  rt_sigfrom = rt_stack[rt_depth - 1];
}
// A stdlib function signals with code 1.
static wv rt_wah(const char *from) {
  rt_sigcode = 1;
  rt_sigfrom = from;
  return RT_NULL;
}
// After a call that is not handled with whine: a pending signal becomes a
// tantrum in the caller (one-frame rule). caller is NULL at the top level.
static inline wv rt_chk(wv v, const char *caller) {
  if (__builtin_expect(rt_sigcode != 0, 0)) {
    if (caller) rt_fail("unhandled WAH! (code %lld) from '%s' in '%s'", (long long)rt_sigcode, rt_sigfrom, caller);
    rt_fail("unhandled WAH! (code %lld) from '%s'", (long long)rt_sigcode, rt_sigfrom);
  }
  return v;
}

static _Noreturn void rt_tantrum(wv v) {
  if (v.tag == T_STR) rt_die(v.s->data, v.s->bytes);
  sb b = { 0 };
  show_to(&b, v);
  rt_fail_sb(&b);
}

static void rt_exit_code(int64_t code) {
  rt_flush();
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
// UTF-16 to a purple; an unpaired surrogate becomes U+FFFD (§12.1).
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

static const wstr *purple(wv x, const char *fn) {
  if (x.tag != T_STR) rt_fail("type error: %s expects a purple, got %s", fn, type_name(x));
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
static wv std_readLine(void) {
  wv line;
  return read_line(&line) ? line : rt_wah("$readLine");
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
  const wstr *path = purple(p, "$readFile");
  char *data;
  int64_t n;
  if (!read_file(path, &data, &n)) return rt_wah("$readFile");
  wv s = rt_decode(data ? data : "", n);
  free(data);
  return s;
}
static wv put_file(wv p, wv t, const char *fn, int append) {
  const wstr *path = purple(p, fn), *text = purple(t, fn);
  pchar *hp = host_path(path);
  if (!hp) return rt_wah(fn);
#ifdef _WIN32
  FILE *f = _wfopen(hp, append ? L"ab" : L"wb");
#else
  FILE *f = fopen(hp, append ? "ab" : "wb");
#endif
  free(hp);
  if (!f) return rt_wah(fn);
  int ok = fwrite(text->data, 1, text->bytes, f) == (size_t)text->bytes;
  if (fclose(f) != 0) ok = 0;
  return ok ? RT_NULL : rt_wah(fn);
}
static wv std_writeFile(wv p, wv t) { return put_file(p, t, "$writeFile", 0); }
static wv std_appendFile(wv p, wv t) { return put_file(p, t, "$appendFile", 1); }
static wv std_fileExists(wv p) {
  pchar *hp = host_path(purple(p, "$fileExists"));
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
  const wstr *path = purple(p, "$listDir");
  if (path->bytes == 0) return rt_wah("$listDir");
  pchar *hp = host_path(path);
  if (!hp) return rt_wah("$listDir");
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
  if (h == INVALID_HANDLE_VALUE) return rt_wah("$listDir");
  do {
    const wchar_t *n = fd.cFileName;
    if (!wcscmp(n, L".") || !wcscmp(n, L"..")) continue;
    wv name = from_wide(n, wcslen(n));
#else
  DIR *d = opendir(hp);
  free(hp);
  if (!d) return rt_wah("$listDir");
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
  pchar *hp = host_path(purple(p, "$deleteFile"));
  if (!hp) return rt_wah("$deleteFile");
#ifdef _WIN32
  int ok = DeleteFileW(hp);
#else
  int ok = unlink(hp) == 0;
#endif
  free(hp);
  return ok ? RT_NULL : rt_wah("$deleteFile");
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
  const wstr *n = purple(name, "$env");
  if (n->bytes == 0 || memchr(n->data, 0, n->bytes)) return rt_wah("$env");
#ifdef _WIN32
  wchar_t *w = to_wide(n->data, n->bytes);
  const wchar_t *v = _wgetenv(w);
  free(w);
  return v ? from_wide(v, wcslen(v)) : rt_wah("$env");
#else
  const char *v = getenv(n->data);
  return v ? rt_decode(v, strlen(v)) : rt_wah("$env");
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
static wv std_run(wv program, wv args) {
  const wstr *prog = purple(program, "$run");
  warr *a = an_array(args, "$run");
  for (int64_t i = 0; i < a->len; i++) {
    if (a->items[i].tag != T_STR) {
      rt_fail("type error: $run expects an array of purples, got an element of type %s", type_name(a->items[i]));
    }
  }
  rt_flush();
  if (prog->bytes == 0 || memchr(prog->data, 0, prog->bytes)) return rt_wah("$run");
#ifdef _WIN32
  wchar_t *wname = to_wide(prog->data, prog->bytes);
  wchar_t *app = find_program(wname);
  free(wname);
  if (!app) return rt_wah("$run");
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
  if (!ok) return rt_wah("$run");
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
  if (err) return rt_wah("$run");
  int status;
  while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return rt_wah("$run");
  if (WIFSIGNALED(status)) return rt_i(128 + WTERMSIG(status));
  return rt_i(WEXITSTATUS(status));
#endif
}

static wv std_length(wv x) {
  if (x.tag == T_STR) return rt_i(x.s->cps);
  if (x.tag == T_ARR) return rt_i(x.a->len);
  rt_fail("type error: $length expects a purple or an array, got %s", type_name(x));
}
static int64_t cp_index(wv i, int64_t n, const char *fn) {
  int64_t k = an_int(i, fn);
  if (k < -n || k >= n) rt_fail("%s: index %lld is out of range for length %lld", fn, (long long)k, (long long)n);
  return k < 0 ? k + n : k;
}
static wv std_at(wv x, wv i) {
  if (x.tag == T_ARR) return rt_idx(i, x);
  const wstr *s = purple(x, "$at");
  int64_t k = cp_index(i, s->cps, "$at");
  int64_t off = cp_offset(s, k), end = cp_offset(s, k + 1);
  return rt_str(s->data + off, end - off);
}
static wv std_slice(wv x, wv a, wv b) {
  int is_arr = x.tag == T_ARR;
  if (!is_arr) purple(x, "$slice");
  int64_t ai = an_int(a, "$slice"), bi = an_int(b, "$slice");
  int64_t n = is_arr ? x.a->len : x.s->cps;
  int64_t s = ai < 0 ? ai + n : ai, e = bi < 0 ? bi + n : bi;
  if (!(s >= 0 && s <= e && e <= n)) rt_fail("$slice: %lld..%lld is out of range for length %lld", (long long)ai, (long long)bi, (long long)n);
  if (is_arr) return arr_copy(x.a, s, e, 0);
  int64_t so = cp_offset(x.s, s), eo = cp_offset(x.s, e);
  return rt_str(x.s->data + so, eo - so);
}
static wv std_code(wv x) {
  const wstr *s = purple(x, "$code");
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
  const wstr *s = purple(x, "$split"), *sep = purple(y, "$split");
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
  const wstr *s = purple(sep, "$join");
  sb b = { 0 };
  for (int64_t i = 0; i < a->len; i++) {
    if (i) sb_putn(&b, s->data, s->bytes);
    if (!text_to(&b, a->items[i])) no_text(a->items[i]);
  }
  return sb_str(&b);
}
static wv std_toPurple(wv v) { return rt_text(v); }

// ---- numerals as text ($toInt, $toFloat, $toWah) ----

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
// The value of a bracket literal (§2.3); 0 if it is not one.
static int bracket_lit(const char *p, int64_t n, wv *out) {
  int64_t v;
  if (n > 2 && p[0] == '0' && p[1] == 'x' && all_digits(p + 2, n - 2, 16)) {
    if (!parse_uint(p + 2, n - 2, 16, &v)) return 0;
    *out = rt_i(v);
    return 1;
  }
  if (n > 2 && p[0] == '0' && p[1] == 'b' && all_digits(p + 2, n - 2, 2)) {
    if (!parse_uint(p + 2, n - 2, 2, &v)) return 0;
    *out = rt_i(v);
    return 1;
  }
  if (all_digits(p, n, 10)) {
    if (!parse_uint(p, n, 10, &v)) return 0;
    *out = rt_i(v);
    return 1;
  }
  if (float_text(p, n, 0)) {
    double d = to_double(p, n);
    if (!isfinite(d)) return 0;
    *out = rt_f(d);
    return 1;
  }
  return 0;
}
// Parses a wah numeral; returns 0 if s is not one.
static int parse_wah(const wstr *s, wv *out) {
  const char *p = s->data;
  int64_t n = s->bytes;
  if (n < 2) return 0;
  int lower = p[0] == 'w';
  if (!lower && p[0] != 'W') return 0;
  char a = lower ? 'a' : 'A', h = lower ? 'h' : 'H';
  if (p[n - 1] != h) return 0;
  int64_t i = 1;
  while (i < n - 1 && p[i] == a) i++;
  if (i == n - 1) {
    *out = rt_i(lower ? n - 2 : -(n - 2));
    return 1;
  }
  // wa[...]h or WA[...]H, with no ']' inside the brackets.
  if (n < 5 || p[1] != a || p[2] != '[' || p[n - 2] != ']') return 0;
  if (memchr(p + 3, ']', n - 5)) return 0;
  wv v;
  if (!bracket_lit(p + 3, n - 5, &v)) return 0;
  if (!lower) v = v.tag == T_INT ? rt_i(-v.i) : rt_f(-v.f);
  *out = v;
  return 1;
}
static wv std_toWah(wv x) {
  sb b = { 0 };
  if (x.tag == T_INT) {
    int64_t n = x.i < 0 ? -x.i : x.i;
    if (n <= 10) {
      sb_putc(&b, x.i < 0 ? 'W' : 'w');
      for (int64_t i = 0; i < n; i++) sb_putc(&b, x.i < 0 ? 'A' : 'a');
      sb_putc(&b, x.i < 0 ? 'H' : 'h');
    } else sb_printf(&b, x.i < 0 ? "WA[%lld]H" : "wa[%lld]h", (long long)n);
    return sb_str(&b);
  }
  if (x.tag == T_FLOAT) {
    if (!isfinite(x.f)) { fmt_float(&b, x.f); return sb_str(&b); }
    int neg = signbit(x.f);
    sb_puts(&b, neg ? "WA[" : "wa[");
    fmt_float(&b, fabs(x.f));
    sb_puts(&b, neg ? "]H" : "]h");
    return sb_str(&b);
  }
  rt_fail("type error: $toWah expects a number, got %s", type_name(x));
}
static wv std_toInt(wv x) {
  if (x.tag == T_INT) return x;
  if (x.tag != T_STR) {
    rt_fail("type error: $toInt expects a purple, got %s%s", type_name(x),
      x.tag == T_FLOAT ? " (use $floor, $ceil, $round or $trunc)" : "");
  }
  const char *p = x.s->data;
  int64_t n = x.s->bytes, neg = n > 0 && p[0] == '-';
  if (all_digits(p + neg, n - neg, 10)) {
    int64_t v;
    if (!parse_uint(p + neg, n - neg, 10, &v)) return rt_wah("$toInt");
    return rt_i(neg ? -v : v);
  }
  wv r;
  if (parse_wah(x.s, &r) && r.tag == T_INT) return r;
  return rt_wah("$toInt");
}
static wv std_toFloat(wv x) {
  if (x.tag == T_INT) return rt_f((double)x.i);
  if (x.tag == T_FLOAT) return x;
  const wstr *s = purple(x, "$toFloat");
  if (float_text(s->data, s->bytes, 1)) {
    // A result too large for a double signals (§9.5).
    double v = to_double(s->data, s->bytes);
    return isfinite(v) ? rt_f(v) : rt_wah("$toFloat");
  }
  wv r;
  if (!parse_wah(s, &r)) return rt_wah("$toFloat");
  return r.tag == T_INT ? rt_f((double)r.i) : r;
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
static wv std_isNobody(wv x) { return rt_b(x.tag == T_FLOAT && isnan(x.f)); }

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
  return a->items[--a->len];
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
  return eq(a, b);
}
static wv std_same(wv a, wv b) { return rt_b(same(a, b)); }
static wv std_typeOf(wv x) { return x.tag == T_MEM ? rt_s(x.m->lineup) : rt_cstr(type_name(x)); }
static wv std_fields(wv r) {
  if (r.tag != T_REC) rt_fail("type error: $fields expects a record, got %s", type_name(r));
  warr *a = arr_new(r.r->n, 0);
  for (int64_t i = 0; i < r.r->n; i++) a->items[i] = rt_s(r.r->keys[i]);
  return (wv){ .tag = T_ARR, .a = a };
}

// ---- startup ----

// The compiled program: library initialization, then the entry file's body.
static void wah_main(void);

#ifdef _WIN32
static DWORD WINAPI rt_thread(LPVOID arg) {
  (void)arg;
  wah_main();
  rt_exit_code(0);
  return 0;
}
#else
static void *rt_thread(void *arg) {
  (void)arg;
  wah_main();
  rt_exit_code(0);
  return NULL;
}
#endif

// The program runs on a thread with a large stack, since schemes recurse deeply.
int main(int argc, char **argv) {
  rt_argc = argc;
  rt_argv = argv;
  size_t stack = (size_t)1 << 30;
#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
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
static const wstr S33 = { 6, 6, 1, 0, "scheme" };
static const wstr S34 = { 7, 7, 1, 0, "getaway" };
static const wstr S35 = { 5, 5, 1, 0, "claim" };
static const wstr S36 = { 6, 6, 1, 0, "shifty" };
static const wstr S37 = { 6, 6, 1, 0, "fickle" };
static const wstr S38 = { 5, 5, 1, 0, "cheat" };
static const wstr S39 = { 5, 5, 1, 0, "doubt" };
static const wstr S40 = { 6, 6, 1, 0, "anyway" };
static const wstr S41 = { 4, 4, 1, 0, "lurk" };
static const wstr S42 = { 4, 4, 1, 0, "bail" };
static const wstr S43 = { 6, 6, 1, 0, "loiter" };
static const wstr S44 = { 7, 7, 1, 0, "tantrum" };
static const wstr S45 = { 5, 5, 1, 0, "whine" };
static const wstr S46 = { 4, 4, 1, 0, "real" };
static const wstr S47 = { 4, 4, 1, 0, "fake" };
static const wstr S48 = { 9, 9, 1, 0, "uninvited" };
static const wstr S49 = { 6, 6, 1, 0, "nobody" };
static const wstr S50 = { 9, 9, 1, 0, "everybody" };
static const wstr S51 = { 7, 7, 1, 0, "suspect" };
static const wstr S52 = { 5, 5, 1, 0, "alibi" };
static const wstr S53 = { 6, 6, 1, 0, "lineup" };
static const wstr S54 = { 5, 5, 1, 0, "steal" };
static const wstr S55 = { 5, 5, 1, 0, "bribe" };
static const wstr S56 = { 5, 5, 1, 0, "blame" };
static const wstr S57 = { 58, 58, 1, 0, "'blame' is reserved: catching tantrums is not designed yet" };
static const wstr S58 = { 4, 4, 1, 0, "sulk" };
static const wstr S59 = { 57, 57, 1, 0, "'sulk' is reserved: catching tantrums is not designed yet" };
static const wstr S60 = { 6, 6, 1, 0, "regret" };
static const wstr S61 = { 59, 59, 1, 0, "'regret' is reserved: catching tantrums is not designed yet" };
static const wstr S62 = { 5, 5, 1, 0, "wails" };
static const wstr S63 = { 37, 37, 1, 0, "'wails' is reserved for after stage 0" };
static const wstr S64 = { 7, 7, 1, 0, "invited" };
static const wstr S65 = { 47, 47, 1, 0, "'invited' is reserved; did you mean '!invited'?" };
static const wstr S66 = { 2, 2, 1, 0, "**" };
static const wstr S67 = { 2, 2, 1, 0, "<<" };
static const wstr S68 = { 2, 2, 1, 0, ">>" };
static const wstr S69 = { 2, 2, 1, 0, "++" };
static const wstr S70 = { 2, 2, 1, 0, "--" };
static const wstr S71 = { 1, 1, 1, 0, "+" };
static const wstr S72 = { 1, 1, 1, 0, "-" };
static const wstr S73 = { 1, 1, 1, 0, "*" };
static const wstr S74 = { 1, 1, 1, 0, "/" };
static const wstr S75 = { 1, 1, 1, 0, "%" };
static const wstr S76 = { 1, 1, 1, 0, "&" };
static const wstr S77 = { 1, 1, 1, 0, "|" };
static const wstr S78 = { 2, 2, 1, 0, "==" };
static const wstr S79 = { 2, 2, 1, 0, "!=" };
static const wstr S80 = { 2, 2, 1, 0, "<=" };
static const wstr S81 = { 2, 2, 1, 0, ">=" };
static const wstr S82 = { 2, 2, 1, 0, "&&" };
static const wstr S83 = { 2, 2, 1, 0, "||" };
static const wstr S84 = { 1, 1, 1, 0, "<" };
static const wstr S85 = { 1, 1, 1, 0, ">" };
static const wstr S86 = { 1, 1, 1, 0, "~" };
static const wstr S87 = { 1, 1, 1, 0, "!" };
static const wstr S88 = { 1, 1, 1, 0, "?" };
static const wstr S89 = { 1, 1, 1, 0, "," };
static const wstr S90 = { 1, 1, 1, 0, "(" };
static const wstr S91 = { 1, 1, 1, 0, "[" };
static const wstr S92 = { 1, 1, 1, 0, "]" };
static const wstr S93 = { 1, 1, 1, 0, "{" };
static const wstr S94 = { 1, 1, 1, 0, "w" };
static const wstr S95 = { 1, 1, 1, 0, "W" };
static const wstr S96 = { 1, 1, 1, 0, "h" };
static const wstr S97 = { 1, 1, 1, 0, "H" };
static const wstr S98 = { 1, 1, 1, 0, "e" };
static const wstr S99 = { 1, 1, 1, 0, "E" };
static const wstr S100 = { 1, 1, 1, 0, "x" };
static const wstr S101 = { 1, 1, 1, 0, "b" };
static const wstr S102 = { 1, 1, 1, 0, "1" };
static const wstr S103 = { 3, 3, 1, 0, "wa[" };
static const wstr S104 = { 2, 2, 1, 0, "]h" };
static const wstr S105 = { 45, 45, 1, 0, "integer literal exceeds the 53-bit safe range" };
static const wstr S106 = { 3, 3, 1, 0, "int" };
static const wstr S107 = { 1, 1, 1, 0, "." };
static const wstr S108 = { 29, 29, 1, 0, "float literal is out of range" };
static const wstr S109 = { 5, 5, 1, 0, "float" };
static const wstr S110 = { 40, 40, 1, 0, "digits are required on both sides of '.'" };
static const wstr S111 = { 1, 1, 1, 0, "'" };
static const wstr S112 = { 26, 26, 1, 0, "' is not a numeric literal" };
static const wstr S113 = { 1, 1, 1, 0, "n" };
static const wstr S114 = { 1, 1, 1, 0, "r" };
static const wstr S115 = { 1, 1, 1, 0, "t" };
static const wstr S116 = { 1, 1, 1, 0, "$" };
static const wstr S117 = { 60, 59, 0, 0, "'//' starts a comment; Γ-- has no integer-division operator" };
static const wstr S118 = { 41, 41, 1, 0, "for integer division write ;a / b($floor)" };
static const wstr S119 = { 2, 2, 1, 0, "*/" };
static const wstr S120 = { 26, 26, 1, 0, "unterminated block comment" };
static const wstr S121 = { 3, 3, 1, 0, "eof" };
static const wstr S122 = { 39, 38, 0, 0, "': decimal digits are not Γ-- numerals" };
static const wstr S123 = { 9, 9, 1, 0, "write wa[" };
static const wstr S124 = { 7, 7, 1, 0, "opcheat" };
static const wstr S125 = { 34, 34, 1, 0, "no whitespace is allowed inside '!" };
static const wstr S126 = { 8, 8, 1, 0, "write '!" };
static const wstr S127 = { 3, 3, 1, 0, ">>>" };
static const wstr S128 = { 26, 26, 1, 0, "there is no '>>>' operator" };
static const wstr S129 = { 48, 48, 1, 0, "for an unsigned interpretation use ;x($toUint32)" };
static const wstr S130 = { 23, 23, 1, 0, "'--' is not an operator" };
static const wstr S131 = { 73, 73, 1, 0, "write '- -x' or '-(-x)' for double negation, or ';--cheat x' to decrement" };
static const wstr S132 = { 23, 23, 1, 0, "'++' is not an operator" };
static const wstr S133 = { 31, 31, 1, 0, "write ';++cheat x' to increment" };
static const wstr S134 = { 59, 59, 1, 0, "';;': likely a habitual trailing ';' from C-style languages" };
static const wstr S135 = { 59, 58, 0, 0, "Γ-- statements begin with ;, they are not terminated by it" };
static const wstr S136 = { 1, 1, 1, 0, "=" };
static const wstr S137 = { 22, 22, 1, 0, "'=' is not an operator" };
static const wstr S138 = { 22, 22, 1, 0, "use 'claim' or 'cheat'" };
static const wstr S139 = { 2, 2, 1, 0, "//" };
static const wstr S140 = { 2, 2, 1, 0, "/*" };
static const wstr S141 = { 35, 35, 1, 0, "no whitespace is allowed around '.'" };
static const wstr S142 = { 53, 53, 1, 0, "member access is written field.record, e.g. kind.node" };
static const wstr S143 = { 22, 22, 1, 0, "unexpected character '" };
static const wstr S144 = { 2, 2, 1, 0, "WA" };
static const wstr S145 = { 24, 24, 1, 0, "mixed-case wah numeral '" };
static const wstr S146 = { 52, 52, 1, 0, "all lowercase is positive, all uppercase is negative" };
static const wstr S147 = { 32, 32, 1, 0, "'$' cannot appear in identifiers" };
static const wstr S148 = { 66, 66, 1, 0, "'$' is reserved for the standard library; user names have no sigil" };
static const wstr S149 = { 2, 2, 1, 0, "id" };
static const wstr S150 = { 2, 2, 1, 0, "wa" };
static const wstr S151 = { 5, 5, 1, 0, "[...'" };
static const wstr S152 = { 37, 37, 1, 0, "unterminated wah numeral: missing ']'" };
static const wstr S153 = { 10, 10, 1, 0, "expected '" };
static const wstr S154 = { 26, 26, 1, 0, "' to close the wah numeral" };
static const wstr S155 = { 19, 19, 1, 0, "invalid wah numeral" };
static const wstr S156 = { 77, 77, 1, 0, "only literals are allowed inside wa[...]h; for computed values use ;x($toInt)" };
static const wstr S157 = { 11, 11, 1, 0, "lowercase '" };
static const wstr S158 = { 18, 18, 1, 0, "!' is not a signal" };
static const wstr S159 = { 7, 7, 1, 0, "write '" };
static const wstr S160 = { 2, 2, 1, 0, "!'" };
static const wstr S161 = { 42, 42, 1, 0, "a WAH! signal code must be an integer >= 1" };
static const wstr S162 = { 3, 3, 1, 0, "sig" };
static const wstr S163 = { 59, 59, 1, 0, "'$' must be immediately followed by a standard library name" };
static const wstr S164 = { 3, 3, 1, 0, "std" };
static const wstr S165 = { 19, 19, 1, 0, "unterminated string" };
static const wstr S166 = { 27, 27, 1, 0, "raw newline inside a string" };
static const wstr S167 = { 10, 10, 1, 0, "write '\\n'" };
static const wstr S168 = { 1, 1, 1, 0, "u" };
static const wstr S169 = { 22, 22, 1, 0, "invalid \\u{...} escape" };
static const wstr S170 = { 69, 69, 1, 0, "write 1 to 6 hex digits naming a Unicode scalar value, e.g. \\u{1F608}" };
static const wstr S171 = { 17, 17, 1, 0, "unknown escape '\\" };
static const wstr S172 = { 43, 43, 1, 0, "valid escapes are \\n \\r \\t \\\\ \\\" \\$ \\u{...}" };
static const wstr S173 = { 35, 34, 0, 0, "Γ-- interpolation is {x}, not ${x}" };
static const wstr S174 = { 23, 23, 1, 0, "unmatched '}' in string" };
static const wstr S175 = { 30, 30, 1, 0, "write '}}' for a literal brace" };
static const wstr S176 = { 14, 14, 1, 0, "did you mean {" };
static const wstr S177 = { 2, 2, 1, 0, "}?" };
static const wstr S178 = { 51, 50, 0, 0, "Γ-- interpolation is {x}; write \\$ for a literal $" };
static const wstr S179 = { 3, 3, 1, 0, "str" };
static const wstr S180 = { 36, 36, 1, 0, "unterminated interpolation in string" };
static const wstr S181 = { 19, 19, 1, 0, "empty interpolation" };
static const wstr S182 = { 30, 30, 1, 0, "write '{{' for a literal brace" };
static const wstr S183 = { 6, 6, 1, 0, "!doubt" };
static const wstr S184 = { 4, 4, 1, 0, "expr" };
static const wstr S185 = { 4, 4, 1, 0, "exit" };
static const wstr S186 = { 69, 69, 1, 0, "calls are postfix (arguments first, then one function name): ;x, y(f)" };
static const wstr S187 = { 12, 12, 1, 0, "end of input" };
static const wstr S188 = { 2, 2, 1, 0, "'$" };
static const wstr S189 = { 8, 8, 1, 0, "a string" };
static const wstr S190 = { 6, 6, 1, 0, "cheat'" };
static const wstr S191 = { 4, 4, 1, 0, "call" };
static const wstr S192 = { 7, 7, 1, 0, "sigcall" };
static const wstr S193 = { 5, 5, 1, 0, "chain" };
static const wstr S194 = { 6, 6, 1, 0, "purple" };
static const wstr S195 = { 3, 3, 1, 0, "arr" };
static const wstr S196 = { 3, 3, 1, 0, "rec" };
static const wstr S197 = { 3, 3, 1, 0, "idx" };
static const wstr S198 = { 3, 3, 1, 0, "mem" };
static const wstr S199 = { 2, 2, 1, 0, "un" };
static const wstr S200 = { 3, 3, 1, 0, "bin" };
static const wstr S201 = { 4, 4, 1, 0, "tern" };
static const wstr S202 = { 8, 8, 1, 0, ", found " };
static const wstr S203 = { 27, 27, 1, 0, "missing whitespace around '" };
static const wstr S204 = { 34, 33, 0, 0, "every Γ-- statement begins with ;" };
static const wstr S205 = { 41, 41, 1, 0, "expected ';' to start a statement, found " };
static const wstr S206 = { 46, 46, 1, 0, "'steal' must come before every other statement" };
static const wstr S207 = { 5, 5, 1, 0, "empty" };
static const wstr S208 = { 61, 61, 1, 0, "'whine' must follow a statement whose value comes from a call" };
static const wstr S209 = { 35, 35, 1, 0, "expected a name for the signal code" };
static const wstr S210 = { 5, 5, 1, 0, "!lurk" };
static const wstr S211 = { 6, 6, 1, 0, "signal" };
static const wstr S212 = { 8, 8, 1, 0, "!invited" };
static const wstr S213 = { 30, 30, 1, 0, "cheat' needs a value before it" };
static const wstr S214 = { 7, 7, 1, 0, ";value " };
static const wstr S215 = { 10, 10, 1, 0, "cheat name" };
static const wstr S216 = { 44, 44, 1, 0, "'anyway' without a preceding doubt or !doubt" };
static const wstr S217 = { 44, 44, 1, 0, "'bribe' needs the names it exports before it" };
static const wstr S218 = { 11, 11, 1, 0, ";a, b bribe" };
static const wstr S219 = { 54, 54, 1, 0, "to make it the block's value, remove the ';' before it" };
static const wstr S220 = { 28, 28, 1, 0, "this statement has no effect" };
static const wstr S221 = { 29, 29, 1, 0, "expected a name after 'claim'" };
static const wstr S222 = { 21, 21, 1, 0, "cheat' takes no value" };
static const wstr S223 = { 7, 7, 1, 0, "write ;" };
static const wstr S224 = { 20, 20, 1, 0, "' goes after 'claim'" };
static const wstr S225 = { 13, 13, 1, 0, ";value claim " };
static const wstr S226 = { 5, 5, 1, 0, " name" };
static const wstr S227 = { 24, 24, 1, 0, "only names can be bribed" };
static const wstr S228 = { 50, 50, 1, 0, "'steal' is only allowed at the top level of a file" };
static const wstr S229 = { 43, 43, 1, 0, "a steal path must be a plain string literal" };
static const wstr S230 = { 16, 16, 1, 0, ";\"lib.wah\" steal" };
static const wstr S231 = { 36, 36, 1, 0, "steal paths use '/' as the separator" };
static const wstr S232 = { 4, 4, 1, 0, ".wah" };
static const wstr S233 = { 40, 40, 1, 0, "a steal path includes the .wah extension" };
static const wstr S234 = { 44, 44, 1, 0, "expected a name for the module after 'steal'" };
static const wstr S235 = { 20, 20, 1, 0, ";\"lib.wah\" steal lib" };
static const wstr S236 = { 19, 19, 1, 0, "' is already bribed" };
static const wstr S237 = { 53, 53, 1, 0, "'bribe' is only allowed at the top level of a library" };
static const wstr S238 = { 38, 38, 1, 0, "parameters come before the scheme name" };
static const wstr S239 = { 26, 26, 1, 0, ";scheme (a, b)name { ... }" };
static const wstr S240 = { 39, 39, 1, 0, "expected '(' to open the parameter list" };
static const wstr S241 = { 25, 25, 1, 0, "expected a parameter name" };
static const wstr S242 = { 40, 40, 1, 0, "expected ')' to close the parameter list" };
static const wstr S243 = { 45, 45, 1, 0, "expected the scheme name after its parameters" };
static const wstr S244 = { 46, 46, 1, 0, "a lineup can only be declared at the top level" };
static const wstr S245 = { 35, 35, 1, 0, "members come before the lineup name" };
static const wstr S246 = { 18, 18, 1, 0, ";lineup (a, b)Name" };
static const wstr S247 = { 36, 36, 1, 0, "expected '(' to open the member list" };
static const wstr S248 = { 34, 34, 1, 0, "a lineup needs at least one member" };
static const wstr S249 = { 42, 42, 1, 0, "' is a keyword and cannot be a member name" };
static const wstr S250 = { 30, 30, 1, 0, "expected a member name, found " };
static const wstr S251 = { 18, 18, 1, 0, "duplicate member '" };
static const wstr S252 = { 11, 11, 1, 0, "' in lineup" };
static const wstr S253 = { 38, 38, 1, 0, "expected ',' or ')' in the member list" };
static const wstr S254 = { 42, 42, 1, 0, "expected the lineup name after its members" };
static const wstr S255 = { 20, 20, 1, 0, "expected '(' after '" };
static const wstr S256 = { 30, 30, 1, 0, "conditions are parenthesized: " };
static const wstr S257 = { 12, 12, 1, 0, " (x) { ... }" };
static const wstr S258 = { 35, 35, 1, 0, "expected ')' to close the condition" };
static const wstr S259 = { 12, 12, 1, 0, "expected '{'" };
static const wstr S260 = { 32, 32, 1, 0, "unterminated block: expected '}'" };
static const wstr S261 = { 68, 68, 1, 0, "a record as a block's value needs its own braces: { { key: value } }" };
static const wstr S262 = { 75, 75, 1, 0, "an expression without a leading ';' is the block's value and must come last" };
static const wstr S263 = { 44, 44, 1, 0, "expected '}' after the block's value, found " };
static const wstr S264 = { 4, 4, 1, 0, "name" };
static const wstr S265 = { 58, 58, 1, 0, "expected a name, an indexed element or a field to cheat on" };
static const wstr S266 = { 66, 66, 1, 0, "a zero-argument call inside an argument list must be parenthesized" };
static const wstr S267 = { 56, 56, 1, 0, "write (,(f)) so the call does not capture its neighbours" };
static const wstr S268 = { 31, 31, 1, 0, "trailing comma in array literal" };
static const wstr S269 = { 109, 109, 1, 0, "a block's value that starts with ',' continues the statement before it; wrap the value in parentheses: (,(f))" };
static const wstr S270 = { 29, 29, 1, 0, ";a, b(f) calls f with a and b" };
static const wstr S271 = { 49, 49, 1, 0, "comma-separated values must be consumed by a call" };
static const wstr S272 = { 40, 40, 1, 0, "a leading ',' must be followed by a call" };
static const wstr S273 = { 36, 36, 1, 0, "zero-argument calls are written ,(f)" };
static const wstr S274 = { 4, 4, 1, 0, "WAH!" };
static const wstr S275 = { 62, 62, 1, 0, "only the exact spelling WAH! is allowed in call position, not " };
static const wstr S276 = { 71, 71, 1, 0, ";c(WAH!) sends code c; any other spelling would make the code ambiguous" };
static const wstr S277 = { 58, 58, 1, 0, "WAH! in call position takes exactly one argument: the code" };
static const wstr S278 = { 38, 38, 1, 0, "expected ':' in conditional expression" };
static const wstr S279 = { 22, 22, 1, 0, "'-x ** y' is ambiguous" };
static const wstr S280 = { 28, 28, 1, 0, "write -(x ** y) or (-x) ** y" };
static const wstr S281 = { 41, 41, 1, 0, "' is a keyword and cannot be a field name" };
static const wstr S282 = { 40, 40, 1, 0, "expected a field name before '.', found " };
static const wstr S283 = { 59, 59, 1, 0, "'[' at the start of a line indexes the expression before it" };
static const wstr S284 = { 71, 71, 1, 0, "if it starts the block's value, wrap the value in parentheses: ([a, b])" };
static const wstr S285 = { 31, 31, 1, 0, "expected ']' to close the index" };
static const wstr S286 = { 32, 32, 1, 0, "string-literal field key: write " };
static const wstr S287 = { 20, 20, 1, 0, ".record instead of \"" };
static const wstr S288 = { 9, 9, 1, 0, "\"[record]" };
static const wstr S289 = { 41, 41, 1, 0, "the left side of '.' must be a field name" };
static const wstr S290 = { 75, 75, 1, 0, "member access is written field.record; for a computed key write key[record]" };
static const wstr S291 = { 4, 4, 1, 0, "bool" };
static const wstr S292 = { 4, 4, 1, 0, "null" };
static const wstr S293 = { 3, 3, 1, 0, "nan" };
static const wstr S294 = { 3, 3, 1, 0, "inf" };
static const wstr S295 = { 17, 17, 1, 0, "empty parentheses" };
static const wstr S296 = { 31, 31, 1, 0, "expected ']' to close the array" };
static const wstr S297 = { 15, 15, 1, 0, " is a statement" };
static const wstr S298 = { 52, 52, 1, 0, "write ;WAH! to signal, or ;c(WAH!) to forward a code" };
static const wstr S299 = { 12, 12, 1, 0, "unexpected '" };
static const wstr S300 = { 44, 43, 0, 0, "Γ-- puts the value first: ;value claim name" };
static const wstr S301 = { 30, 30, 1, 0, "expected an expression, found " };
static const wstr S302 = { 29, 29, 1, 0, "expected a field name, found " };
static const wstr S303 = { 47, 47, 1, 0, "record literals are written { key: value, ... }" };
static const wstr S304 = { 17, 17, 1, 0, "duplicate field '" };
static const wstr S305 = { 19, 19, 1, 0, "' in record literal" };
static const wstr S306 = { 29, 29, 1, 0, "there is no shorthand; write " };
static const wstr S307 = { 31, 31, 1, 0, "expected ':' after field name '" };
static const wstr S308 = { 9, 9, 1, 0, "', found " };
static const wstr S309 = { 32, 32, 1, 0, "trailing comma in record literal" };
static const wstr S310 = { 37, 37, 1, 0, "expected ',' or '}' in record literal" };
static const wstr S311 = { 40, 40, 1, 0, "expected '{' after the suspect's subject" };
static const wstr S312 = { 36, 36, 1, 0, "'anyway' must come last in a suspect" };
static const wstr S313 = { 33, 33, 1, 0, "an alibi needs at least one value" };
static const wstr S314 = { 22, 22, 1, 0, "alibi \"a\", \"b\" { ... }" };
static const wstr S315 = { 31, 31, 1, 0, "a suspect has only one 'anyway'" };
static const wstr S316 = { 50, 50, 1, 0, "a suspect needs at least one alibi before 'anyway'" };
static const wstr S317 = { 34, 34, 1, 0, "a suspect needs at least one alibi" };
static const wstr S318 = { 33, 33, 1, 0, "suspect (x) { alibi \"a\" { ... } }" };
static const wstr S319 = { 46, 46, 1, 0, "expected 'alibi', 'anyway' or '}' in a suspect" };
static const wstr S320 = { 37, 37, 1, 0, "an alibi value cannot be interpolated" };
static const wstr S321 = { 29, 29, 1, 0, "floats cannot be alibi values" };
static const wstr S322 = { 62, 62, 1, 0, "float equality makes a poor match; compare with !doubt instead" };
static const wstr S323 = { 51, 51, 1, 0, "an alibi value must be a literal or a lineup member" };
static const wstr S324 = { 74, 74, 1, 0, "alibi values are integers, purples, real, fake, uninvited or member.Lineup" };
static const wstr S325 = { 37, 37, 1, 0, "'doubt (a != b)' is a double negative" };
static const wstr S326 = { 23, 23, 1, 0, "write '!doubt (a == b)'" };
static const wstr S327 = { 11, 11, 1, 0, "unexpected " };
static const wstr S328 = { 17, 17, 1, 0, " in interpolation" };
static const wstr S329 = { 5, 5, 1, 0, "print" };
static const wstr S330 = { 5, 5, 1, 0, "write" };
static const wstr S331 = { 6, 6, 1, 0, "eprint" };
static const wstr S332 = { 8, 8, 1, 0, "readLine" };
static const wstr S333 = { 8, 8, 1, 0, "readFile" };
static const wstr S334 = { 9, 9, 1, 0, "writeFile" };
static const wstr S335 = { 10, 10, 1, 0, "appendFile" };
static const wstr S336 = { 10, 10, 1, 0, "fileExists" };
static const wstr S337 = { 7, 7, 1, 0, "listDir" };
static const wstr S338 = { 10, 10, 1, 0, "deleteFile" };
static const wstr S339 = { 4, 4, 1, 0, "args" };
static const wstr S340 = { 5, 5, 1, 0, "const" };
static const wstr S341 = { 3, 3, 1, 0, "env" };
static const wstr S342 = { 11, 11, 1, 0, "programPath" };
static const wstr S343 = { 3, 3, 1, 0, "run" };
static const wstr S344 = { 6, 6, 1, 0, "length" };
static const wstr S345 = { 2, 2, 1, 0, "at" };
static const wstr S346 = { 5, 5, 1, 0, "slice" };
static const wstr S347 = { 4, 4, 1, 0, "code" };
static const wstr S348 = { 8, 8, 1, 0, "fromCode" };
static const wstr S349 = { 5, 5, 1, 0, "split" };
static const wstr S350 = { 4, 4, 1, 0, "join" };
static const wstr S351 = { 8, 8, 1, 0, "toPurple" };
static const wstr S352 = { 5, 5, 1, 0, "toWah" };
static const wstr S353 = { 5, 5, 1, 0, "toInt" };
static const wstr S354 = { 7, 7, 1, 0, "toFloat" };
static const wstr S355 = { 4, 4, 1, 0, "push" };
static const wstr S356 = { 3, 3, 1, 0, "pop" };
static const wstr S357 = { 4, 4, 1, 0, "same" };
static const wstr S358 = { 6, 6, 1, 0, "typeOf" };
static const wstr S359 = { 6, 6, 1, 0, "fields" };
static const wstr S360 = { 5, 5, 1, 0, "floor" };
static const wstr S361 = { 4, 4, 1, 0, "ceil" };
static const wstr S362 = { 5, 5, 1, 0, "round" };
static const wstr S363 = { 5, 5, 1, 0, "trunc" };
static const wstr S364 = { 8, 8, 1, 0, "toUint32" };
static const wstr S365 = { 8, 8, 1, 0, "isNobody" };
static const wstr S366 = { 9, 9, 1, 0, " argument" };
static const wstr S367 = { 1, 1, 1, 0, "s" };
static const wstr S368 = { 4, 4, 1, 0, "str:" };
static const wstr S369 = { 4, 4, 1, 0, "mem:" };
static const wstr S370 = { 55, 55, 1, 0, "this block's value is used, but it does not produce one" };
static const wstr S371 = { 67, 67, 1, 0, "end the block with its value, or with uninvited if that is intended" };
static const wstr S372 = { 42, 42, 1, 0, "' is already declared in this scope (line " };
static const wstr S373 = { 11, 11, 1, 0, "' shadows '" };
static const wstr S374 = { 32, 32, 1, 0, "' from an enclosing scope (line " };
static const wstr S375 = { 43, 42, 0, 0, "Γ-- forbids shadowing; choose another name" };
static const wstr S376 = { 14, 14, 1, 0, "unknown name '" };
static const wstr S377 = { 33, 33, 1, 0, "' is used before its claim (line " };
static const wstr S378 = { 5, 5, 1, 0, "param" };
static const wstr S379 = { 26, 26, 1, 0, "'getaway' outside a scheme" };
static const wstr S380 = { 16, 16, 1, 0, "' outside a loop" };
static const wstr S381 = { 51, 51, 1, 0, "WAH! outside a scheme: there is no caller to signal" };
static const wstr S382 = { 64, 64, 1, 0, "'whine' handles a call, but this statement's value is not a call" };
static const wstr S383 = { 83, 83, 1, 0, "the call must be the outermost expression, e.g. ;s(parse) claim n whine (c) { ... }" };
static const wstr S384 = { 48, 48, 1, 0, "a suspect cannot mix literals and lineup members" };
static const wstr S385 = { 33, 33, 1, 0, "a suspect cannot mix members of '" };
static const wstr S386 = { 7, 7, 1, 0, "' and '" };
static const wstr S387 = { 22, 22, 1, 0, "duplicate alibi value " };
static const wstr S388 = { 7, 7, 1, 0, " (line " };
static const wstr S389 = { 24, 24, 1, 0, "suspect does not handle " };
static const wstr S390 = { 2, 2, 1, 0, ", " };
static const wstr S391 = { 63, 63, 1, 0, "add an alibi for each, or end the suspect with 'anyway { ... }'" };
static const wstr S392 = { 56, 56, 1, 0, "a suspect used as a value must end with 'anyway { ... }'" };
static const wstr S393 = { 6, 6, 1, 0, "module" };
static const wstr S394 = { 18, 18, 1, 0, "' does not bribe '" };
static const wstr S395 = { 17, 17, 1, 0, "' is not a module" };
static const wstr S396 = { 16, 16, 1, 0, "unknown module '" };
static const wstr S397 = { 50, 50, 1, 0, "' is a scheme; functions are not values in stage 0" };
static const wstr S398 = { 24, 24, 1, 0, "to call it, write ;args(" };
static const wstr S399 = { 26, 26, 1, 0, "' is a lineup, not a value" };
static const wstr S400 = { 31, 31, 1, 0, "its members are written member." };
static const wstr S401 = { 59, 59, 1, 0, "alibi values are literals or lineup members (member.Lineup)" };
static const wstr S402 = { 17, 17, 1, 0, "' is not a lineup" };
static const wstr S403 = { 16, 16, 1, 0, "unknown lineup '" };
static const wstr S404 = { 11, 11, 1, 0, "no member '" };
static const wstr S405 = { 13, 13, 1, 0, "' in lineup '" };
static const wstr S406 = { 16, 16, 1, 0, "its members are " };
static const wstr S407 = { 68, 68, 1, 0, "a conditional used as a value must end with a plain 'anyway { ... }'" };
static const wstr S408 = { 17, 17, 1, 0, "cannot cheat on '" };
static const wstr S409 = { 29, 29, 1, 0, "': it is not shifty or fickle" };
static const wstr S410 = { 7, 7, 1, 0, "a field" };
static const wstr S411 = { 10, 10, 1, 0, "an element" };
static const wstr S412 = { 31, 31, 1, 0, "': lineup members are constants" };
static const wstr S413 = { 9, 9, 1, 0, "': only '" };
static const wstr S414 = { 14, 14, 1, 0, "' may cheat it" };
static const wstr S415 = { 14, 14, 1, 0, "cannot modify " };
static const wstr S416 = { 5, 5, 1, 0, " of '" };
static const wstr S417 = { 19, 19, 1, 0, "': it is not shifty" };
static const wstr S418 = { 7, 7, 1, 0, "' is a " };
static const wstr S419 = { 15, 15, 1, 0, ", not a binding" };
static const wstr S420 = { 27, 27, 1, 0, "mark the parameter (fickle " };
static const wstr S421 = { 27, 27, 1, 0, ") to rebind it, or (shifty " };
static const wstr S422 = { 20, 20, 1, 0, ") for a mutable copy" };
static const wstr S423 = { 35, 35, 1, 0, "mark the parameter shifty: (shifty " };
static const wstr S424 = { 35, 35, 1, 0, "declare it with ';... claim fickle " };
static const wstr S425 = { 38, 38, 1, 0, "' to rebind it, or ';... claim shifty " };
static const wstr S426 = { 20, 20, 1, 0, "' for a mutable copy" };
static const wstr S427 = { 35, 35, 1, 0, "declare it with ';... claim shifty " };
static const wstr S428 = { 32, 32, 1, 0, "unknown standard library name '$" };
static const wstr S429 = { 26, 26, 1, 0, "' is a module, not a value" };
static const wstr S430 = { 31, 31, 1, 0, "what it bribes is written name." };
static const wstr S431 = { 32, 32, 1, 0, "' is a function; call it with ($" };
static const wstr S432 = { 52, 52, 1, 0, "c(WAH!) is only valid as a whole statement: ;c(WAH!)" };
static const wstr S433 = { 24, 24, 1, 0, "checker: unhandled node " };
static const wstr S434 = { 31, 31, 1, 0, "' is a constant, not a function" };
static const wstr S435 = { 8, 8, 1, 0, "' takes " };
static const wstr S436 = { 6, 6, 1, 0, ", got " };
static const wstr S437 = { 16, 16, 1, 0, "unknown scheme '" };
static const wstr S438 = { 17, 17, 1, 0, "' is not a scheme" };
static const wstr S439 = { 51, 51, 1, 0, "calls are postfix: arguments come first, e.g. ;x(f)" };
static const wstr S440 = { 19, 19, 1, 0, "' is already stolen" };
static const wstr S441 = { 42, 42, 1, 0, "' is not a library: it ends with ;!invited" };
static const wstr S442 = { 47, 47, 1, 0, "'bribe' must be the last statement of a library" };
static const wstr S443 = { 81, 81, 1, 0, "a library's top level may contain only steal, scheme, claim and lineup statements" };
static const wstr S444 = { 35, 35, 1, 0, "move other statements into a scheme" };
static const wstr S445 = { 44, 44, 1, 0, "a library must end with the names it exports" };
static const wstr S446 = { 14, 14, 1, 0, "cannot bribe '" };
static const wstr S447 = { 38, 38, 1, 0, "': it is not declared at the top level" };
static const wstr S448 = { 17, 17, 1, 0, "': it is a module" };
static const wstr S449 = { 5, 5, 1, 0, "line " };
static const wstr S450 = { 6, 6, 1, 0, " line " };
static const wstr S451 = { 49, 49, 1, 0, "' never produces a value, but its value is used (" };
static const wstr S452 = { 68, 68, 1, 0, "end the scheme with its value, or with uninvited if that is intended" };
static const wstr S453 = { 59, 59, 1, 0, "program ended, but Waluigi was never told he wasn't invited" };
static const wstr S454 = { 33, 33, 1, 0, "end the entry file with ;!invited" };
static const wstr S455 = { 3, 3, 1, 0, "add" };
static const wstr S456 = { 3, 3, 1, 0, "sub" };
static const wstr S457 = { 3, 3, 1, 0, "mul" };
static const wstr S458 = { 3, 3, 1, 0, "div" };
static const wstr S459 = { 3, 3, 1, 0, "mod" };
static const wstr S460 = { 3, 3, 1, 0, "pow" };
static const wstr S461 = { 2, 2, 1, 0, "eq" };
static const wstr S462 = { 2, 2, 1, 0, "ne" };
static const wstr S463 = { 2, 2, 1, 0, "lt" };
static const wstr S464 = { 2, 2, 1, 0, "gt" };
static const wstr S465 = { 2, 2, 1, 0, "le" };
static const wstr S466 = { 2, 2, 1, 0, "ge" };
static const wstr S467 = { 4, 4, 1, 0, "band" };
static const wstr S468 = { 3, 3, 1, 0, "bor" };
static const wstr S469 = { 4, 4, 1, 0, "bxor" };
static const wstr S470 = { 3, 3, 1, 0, "shl" };
static const wstr S471 = { 3, 3, 1, 0, "shr" };
static const wstr S472 = { 3, 3, 1, 0, "neg" };
static const wstr S473 = { 3, 3, 1, 0, "not" };
static const wstr S474 = { 4, 4, 1, 0, "bnot" };
static const wstr S475 = { 4, 4, 1, 0, "true" };
static const wstr S476 = { 5, 5, 1, 0, "false" };
static const wstr S477 = { 1, 1, 1, 0, "l" };
static const wstr S478 = { 2, 2, 1, 0, "  " };
static const wstr S479 = { 2, 2, 1, 0, "$t" };
static const wstr S480 = { 74, 74, 1, 0, "'getaway' inside a conditional used as a value is not supported in stage 0" };
static const wstr S481 = { 7, 7, 1, 0, "return " };
static const wstr S482 = { 18, 18, 1, 0, "throw $rt.tantrum(" };
static const wstr S483 = { 2, 2, 1, 0, ");" };
static const wstr S484 = { 55, 55, 1, 0, "' cannot leave a conditional used as a value in stage 0" };
static const wstr S485 = { 6, 6, 1, 0, "break;" };
static const wstr S486 = { 9, 9, 1, 0, "continue;" };
static const wstr S487 = { 7, 7, 1, 0, "while (" };
static const wstr S488 = { 9, 9, 1, 0, "$rt.cond(" };
static const wstr S489 = { 4, 4, 1, 0, ")) {" };
static const wstr S490 = { 17, 17, 1, 0, "throw $rt.signal(" };
static const wstr S491 = { 9, 9, 1, 0, "$rt.exit(" };
static const wstr S492 = { 3, 3, 1, 0, "let" };
static const wstr S493 = { 3, 3, 1, 0, " = " };
static const wstr S494 = { 29, 29, 1, 0, "codegen: unhandled statement " };
static const wstr S495 = { 9, 9, 1, 0, "function " };
static const wstr S496 = { 3, 3, 1, 0, ") {" };
static const wstr S497 = { 12, 12, 1, 0, " = $rt.bind(" };
static const wstr S498 = { 5, 5, 1, 0, "try {" };
static const wstr S499 = { 14, 14, 1, 0, "} catch ($e) {" };
static const wstr S500 = { 22, 22, 1, 0, "  throw $rt.leave($e, " };
static const wstr S501 = { 7, 7, 1, 0, "const l" };
static const wstr S502 = { 14, 14, 1, 0, " = $rt.lineup(" };
static const wstr S503 = { 3, 3, 1, 0, ", [" };
static const wstr S504 = { 3, 3, 1, 0, "]);" };
static const wstr S505 = { 12, 12, 1, 0, "$rt.frozen([" };
static const wstr S506 = { 2, 2, 1, 0, "])" };
static const wstr S507 = { 9, 9, 1, 0, "$rt.bind(" };
static const wstr S508 = { 6, 6, 1, 0, "const " };
static const wstr S509 = { 7, 7, 1, 0, " = $rt." };
static const wstr S510 = { 5, 5, 1, 0, ", \"f_" };
static const wstr S511 = { 3, 3, 1, 0, "\", " };
static const wstr S512 = { 11, 11, 1, 0, "$rt.setMem(" };
static const wstr S513 = { 11, 11, 1, 0, "$rt.updMem(" };
static const wstr S514 = { 6, 6, 1, 0, ", $rt." };
static const wstr S515 = { 11, 11, 1, 0, "$rt.setIdx(" };
static const wstr S516 = { 11, 11, 1, 0, "$rt.updIdx(" };
static const wstr S517 = { 4, 4, 1, 0, ", f_" };
static const wstr S518 = { 25, 25, 1, 0, "$rt.rec({ __proto__: null" };
static const wstr S519 = { 4, 4, 1, 0, " }, " };
static const wstr S520 = { 2, 2, 1, 0, "$e" };
static const wstr S521 = { 4, 4, 1, 0, "let " };
static const wstr S522 = { 9, 9, 1, 0, "} catch (" };
static const wstr S523 = { 6, 6, 1, 0, "if (!(" };
static const wstr S524 = { 28, 28, 1, 0, " instanceof $rt.Wah)) throw " };
static const wstr S525 = { 6, 6, 1, 0, ".code;" };
static const wstr S526 = { 3, 3, 1, 0, "raw" };
static const wstr S527 = { 7, 7, 1, 0, "} else " };
static const wstr S528 = { 4, 4, 1, 0, "if (" };
static const wstr S529 = { 8, 8, 1, 0, "} else {" };
static const wstr S530 = { 3, 3, 1, 0, " ? " };
static const wstr S531 = { 3, 3, 1, 0, " : " };
static const wstr S532 = { 9, 9, 1, 0, "(() => {\n" };
static const wstr S533 = { 4, 4, 1, 0, "})()" };
static const wstr S534 = { 7, 7, 1, 0, "$rt.eq(" };
static const wstr S535 = { 4, 4, 1, 0, " || " };
static const wstr S536 = { 12, 12, 1, 0, "$rt.noAlibi(" };
static const wstr S537 = { 2, 2, 1, 0, "((" };
static const wstr S538 = { 5, 5, 1, 0, ") => " };
static const wstr S539 = { 2, 2, 1, 0, ")(" };
static const wstr S540 = { 7, 7, 1, 0, ") => {\n" };
static const wstr S541 = { 3, 3, 1, 0, "})(" };
static const wstr S542 = { 5, 5, 1, 0, "$std." };
static const wstr S543 = { 6, 6, 1, 0, "$rt.f(" };
static const wstr S544 = { 9, 9, 1, 0, "$rt.text(" };
static const wstr S545 = { 2, 2, 1, 0, "\"\"" };
static const wstr S546 = { 3, 3, 1, 0, " + " };
static const wstr S547 = { 10, 10, 1, 0, "$rt.NOBODY" };
static const wstr S548 = { 13, 13, 1, 0, "$rt.EVERYBODY" };
static const wstr S549 = { 8, 8, 1, 0, "$rt.idx(" };
static const wstr S550 = { 3, 3, 1, 0, ".m_" };
static const wstr S551 = { 11, 11, 1, 0, "$rt.get(\"f_" };
static const wstr S552 = { 4, 4, 1, 0, "$rt." };
static const wstr S553 = { 10, 10, 1, 0, "($rt.bool(" };
static const wstr S554 = { 3, 3, 1, 0, ", \"" };
static const wstr S555 = { 3, 3, 1, 0, "\") " };
static const wstr S556 = { 10, 10, 1, 0, " $rt.bool(" };
static const wstr S557 = { 3, 3, 1, 0, "\"))" };
static const wstr S558 = { 10, 10, 1, 0, "($rt.cond(" };
static const wstr S559 = { 4, 4, 1, 0, ") ? " };
static const wstr S560 = { 30, 30, 1, 0, "codegen: unhandled expression " };
static const wstr S561 = { 14, 14, 1, 0, "\"use strict\";\n" };
static const wstr S562 = { 24, 24, 1, 0, "\n$rt.main(function () {\n" };
static const wstr S563 = { 5, 5, 1, 0, "\n});\n" };
static const wstr S564 = { 2, 2, 1, 0, "\\\"" };
static const wstr S565 = { 2, 2, 1, 0, "\\\\" };
static const wstr S566 = { 2, 2, 1, 0, "{ " };
static const wstr S567 = { 5, 5, 1, 0, ", 0, " };
static const wstr S568 = { 2, 2, 1, 0, " }" };
static const wstr S569 = { 4, 4, 1, 0, "NULL" };
static const wstr S570 = { 8, 8, 1, 0, "(wv[]){ " };
static const wstr S571 = { 1, 1, 1, 0, "v" };
static const wstr S572 = { 2, 2, 1, 0, "&S" };
static const wstr S573 = { 3, 3, 1, 0, "&F_" };
static const wstr S574 = { 19, 19, 1, 0, "static const wmem l" };
static const wstr S575 = { 7, 7, 1, 0, "[] = { " };
static const wstr S576 = { 3, 3, 1, 0, " };" };
static const wstr S577 = { 3, 3, 1, 0, "fr." };
static const wstr S578 = { 2, 2, 1, 0, "up" };
static const wstr S579 = { 4, 4, 1, 0, "->up" };
static const wstr S580 = { 2, 2, 1, 0, "->" };
static const wstr S581 = { 7, 7, 1, 0, "rt_def(" };
static const wstr S582 = { 3, 3, 1, 0, "wv " };
static const wstr S583 = { 3, 3, 1, 0, "({ " };
static const wstr S584 = { 4, 4, 1, 0, "; })" };
static const wstr S585 = { 12, 12, 1, 0, " = RT_UNSET;" };
static const wstr S586 = { 6, 6, 1, 0, "(void)" };
static const wstr S587 = { 7, 7, 1, 0, "RT_NULL" };
static const wstr S588 = { 7, 7, 1, 0, "RT_RET(" };
static const wstr S589 = { 11, 11, 1, 0, "rt_tantrum(" };
static const wstr S590 = { 8, 8, 1, 0, "rt_cond(" };
static const wstr S591 = { 5, 5, 1, 0, "rt_i(" };
static const wstr S592 = { 8, 8, 1, 0, "rt_exit(" };
static const wstr S593 = { 7, 7, 1, 0, "rt_i(0)" };
static const wstr S594 = { 10, 10, 1, 0, "rt_signal(" };
static const wstr S595 = { 16, 16, 1, 0, "RT_RET(RT_NULL);" };
static const wstr S596 = { 9, 9, 1, 0, "struct fr" };
static const wstr S597 = { 4, 4, 1, 0, " *up" };
static const wstr S598 = { 2, 2, 1, 0, "p_" };
static const wstr S599 = { 10, 10, 1, 0, "static wv " };
static const wstr S600 = { 4, 4, 1, 0, "void" };
static const wstr S601 = { 9, 9, 1, 0, "rt_enter(" };
static const wstr S602 = { 8, 8, 1, 0, " fr = { " };
static const wstr S603 = { 8, 8, 1, 0, ".up = up" };
static const wstr S604 = { 8, 8, 1, 0, "rt_bind(" };
static const wstr S605 = { 13, 13, 1, 0, "(void)rt_def(" };
static const wstr S606 = { 3, 3, 1, 0, "rt_" };
static const wstr S607 = { 7, 7, 1, 0, "rt_set_" };
static const wstr S608 = { 7, 7, 1, 0, "rt_upd_" };
static const wstr S609 = { 5, 5, 1, 0, ", rt_" };
static const wstr S610 = { 7, 7, 1, 0, "rt_arr(" };
static const wstr S611 = { 1, 1, 1, 0, "K" };
static const wstr S612 = { 7, 7, 1, 0, "rt_rec(" };
static const wstr S613 = { 17, 17, 1, 0, "if (rt_sigcode) {" };
static const wstr S614 = { 16, 16, 1, 0, "rt_i(rt_sigcode)" };
static const wstr S615 = { 15, 15, 1, 0, "rt_sigcode = 0;" };
static const wstr S616 = { 3, 3, 1, 0, "({\n" };
static const wstr S617 = { 2, 2, 1, 0, "})" };
static const wstr S618 = { 3, 3, 1, 0, "eq(" };
static const wstr S619 = { 13, 13, 1, 0, "(rt_no_alibi(" };
static const wstr S620 = { 11, 11, 1, 0, "), RT_NULL)" };
static const wstr S621 = { 6, 6, 1, 0, "({ wv " };
static const wstr S622 = { 2, 2, 1, 0, "; " };
static const wstr S623 = { 4, 4, 1, 0, "std_" };
static const wstr S624 = { 3, 3, 1, 0, "&fr" };
static const wstr S625 = { 7, 7, 1, 0, "rt_chk(" };
static const wstr S626 = { 5, 5, 1, 0, "rt_f(" };
static const wstr S627 = { 5, 5, 1, 0, "rt_s(" };
static const wstr S628 = { 8, 8, 1, 0, "rt_text(" };
static const wstr S629 = { 7, 7, 1, 0, "rt_cat(" };
static const wstr S630 = { 5, 5, 1, 0, "rt_b(" };
static const wstr S631 = { 9, 9, 1, 0, "RT_NOBODY" };
static const wstr S632 = { 12, 12, 1, 0, "RT_EVERYBODY" };
static const wstr S633 = { 2, 2, 1, 0, "()" };
static const wstr S634 = { 7, 7, 1, 0, "rt_idx(" };
static const wstr S635 = { 6, 6, 1, 0, "rt_m(&" };
static const wstr S636 = { 7, 7, 1, 0, "rt_get(" };
static const wstr S637 = { 13, 13, 1, 0, "rt_b(rt_bool(" };
static const wstr S638 = { 9, 9, 1, 0, " rt_bool(" };
static const wstr S639 = { 9, 9, 1, 0, "(rt_cond(" };
static const wstr S640 = { 2, 2, 1, 0, "\r\n" };
static const wstr S641 = { 20, 20, 1, 0, "// ---- program ----" };
static const wstr S642 = { 19, 19, 1, 0, "static const wstr S" };
static const wstr S643 = { 14, 14, 1, 0, "static wstr F_" };
static const wstr S644 = { 26, 26, 1, 0, "static const wstr *const K" };
static const wstr S645 = { 5, 5, 1, 0, " *up;" };
static const wstr S646 = { 10, 10, 1, 0, "char none;" };
static const wstr S647 = { 3, 3, 1, 0, " { " };
static const wstr S648 = { 2, 2, 1, 0, " {" };
static const wstr S649 = { 28, 28, 1, 0, "static void wah_main(void) {" };
static const wstr S650 = { 10, 10, 1, 0, "  rt_syms(" };
static const wstr S651 = { 19, 19, 1, 0, ", (wstr *const[]){ " };
static const wstr S652 = { 4, 4, 1, 0, " });" };
static const wstr S653 = { 4, 4, 1, 0, "  | " };
static const wstr S654 = { 4, 4, 1, 0, "  { " };
static const wstr S655 = { 4, 4, 1, 0, "    " };
static const wstr S656 = { 3, 3, 1, 0, "  }" };
static const wstr S657 = { 5, 5, 1, 0, "array" };
static const wstr S658 = { 2, 2, 1, 0, "[]" };
static const wstr S659 = { 6, 6, 1, 0, "record" };
static const wstr S660 = { 2, 2, 1, 0, ".." };
static const wstr S661 = { 14, 14, 1, 0, "import cycle: " };
static const wstr S662 = { 4, 4, 1, 0, " -> " };
static const wstr S663 = { 13, 13, 1, 0, "cannot read '" };
static const wstr S664 = { 3, 1, 0, 0, "﻿" };
static const wstr S665 = { 7, 7, 1, 0, "usage:\n" };
static const wstr S666 = { 76, 76, 1, 0, "  wahc0 run wahc/wahc.wah <file.wah> [-o <out.js>] [--runtime <runtime.js>]\n" };
static const wstr S667 = { 81, 81, 1, 0, "  wahc0 run wahc/wahc.wah <file.wah> --target c [-o <out.c>] [--runtime <wah.c>]\n" };
static const wstr S668 = { 96, 96, 1, 0, "  wahc0 run wahc/wahc.wah build <file.wah> [-o <program>] [--cc <compiler>] [--runtime <wah.c>]\n" };
static const wstr S669 = { 46, 46, 1, 0, "  wahc0 run wahc/wahc.wah --tokens <file.wah>\n" };
static const wstr S670 = { 42, 42, 1, 0, "  wahc0 run wahc/wahc.wah --ast <file.wah>" };
static const wstr S671 = { 18, 18, 1, 0, "wahc: cannot read " };
static const wstr S672 = { 5, 5, 1, 0, "build" };
static const wstr S673 = { 8, 8, 1, 0, "--tokens" };
static const wstr S674 = { 5, 5, 1, 0, "--ast" };
static const wstr S675 = { 2, 2, 1, 0, "-o" };
static const wstr S676 = { 9, 9, 1, 0, "--runtime" };
static const wstr S677 = { 8, 8, 1, 0, "--target" };
static const wstr S678 = { 4, 4, 1, 0, "--cc" };
static const wstr S679 = { 2, 2, 1, 0, "js" };
static const wstr S680 = { 1, 1, 1, 0, "c" };
static const wstr S681 = { 2, 2, 1, 0, "OS" };
static const wstr S682 = { 10, 10, 1, 0, "Windows_NT" };
static const wstr S683 = { 3, 3, 1, 0, "ZIG" };
static const wstr S684 = { 3, 3, 1, 0, "zig" };
static const wstr S685 = { 6, 6, 1, 0, "wahc: " };
static const wstr S686 = { 52, 52, 1, 0, " does not end with .wah, so name the program with -o" };
static const wstr S687 = { 4, 4, 1, 0, ".exe" };
static const wstr S688 = { 13, 13, 1, 0, "runtime/wah.c" };
static const wstr S689 = { 17, 17, 1, 0, "js/src/runtime.js" };
static const wstr S690 = { 3, 3, 1, 0, "../" };
static const wstr S691 = { 19, 19, 1, 0, "wahc: cannot write " };
static const wstr S692 = { 2, 2, 1, 0, ".c" };
static const wstr S693 = { 3, 3, 1, 0, "-O2" };
static const wstr S694 = { 7, 7, 1, 0, "zig.exe" };
static const wstr S695 = { 2, 2, 1, 0, "cc" };
static const wstr S696 = { 17, 17, 1, 0, "wahc: cannot run " };
static const wstr S697 = { 67, 67, 1, 0, ": install Zig (https://ziglang.org), or name a C compiler with --cc" };
static const wstr S698 = { 23, 23, 1, 0, " failed with exit code " };
static wstr F_severity = { 8, 8, 1, 0, "severity" };
static wstr F_pos = { 3, 3, 1, 0, "pos" };
static wstr F_message = { 7, 7, 1, 0, "message" };
static wstr F_hint = { 4, 4, 1, 0, "hint" };
static wstr F_file = { 4, 4, 1, 0, "file" };
static wstr F_chars = { 5, 5, 1, 0, "chars" };
static wstr F_word = { 4, 4, 1, 0, "word" };
static wstr F_kind = { 4, 4, 1, 0, "kind" };
static wstr F_error = { 5, 5, 1, 0, "error" };
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
static wstr F_code = { 4, 4, 1, 0, "code" };
static wstr F_op = { 2, 2, 1, 0, "op" };
static wstr F_target = { 6, 6, 1, 0, "target" };
static wstr F_paren = { 5, 5, 1, 0, "paren" };
static wstr F_isShifty = { 8, 8, 1, 0, "isShifty" };
static wstr F_isFickle = { 8, 8, 1, 0, "isFickle" };
static wstr F_name = { 4, 4, 1, 0, "name" };
static wstr F_namePos = { 7, 7, 1, 0, "namePos" };
static wstr F_path = { 4, 4, 1, 0, "path" };
static wstr F_pathPos = { 7, 7, 1, 0, "pathPos" };
static wstr F_names = { 5, 5, 1, 0, "names" };
static wstr F_params = { 6, 6, 1, 0, "params" };
static wstr F_members = { 7, 7, 1, 0, "members" };
static wstr F_value = { 5, 5, 1, 0, "value" };
static wstr F_close = { 5, 5, 1, 0, "close" };
static wstr F_callee = { 6, 6, 1, 0, "callee" };
static wstr F_args = { 4, 4, 1, 0, "args" };
static wstr F_field = { 5, 5, 1, 0, "field" };
static wstr F_arg = { 3, 3, 1, 0, "arg" };
static wstr F_values = { 6, 6, 1, 0, "values" };
static wstr F_subject = { 7, 7, 1, 0, "subject" };
static wstr F_alibis = { 6, 6, 1, 0, "alibis" };
static wstr F_els = { 3, 3, 1, 0, "els" };
static wstr F_branches = { 8, 8, 1, 0, "branches" };
static wstr F_arity = { 5, 5, 1, 0, "arity" };
static wstr F_mod = { 3, 3, 1, 0, "mod" };
static wstr F_parent = { 6, 6, 1, 0, "parent" };
static wstr F_visibleAt = { 9, 9, 1, 0, "visibleAt" };
static wstr F_fnId = { 4, 4, 1, 0, "fnId" };
static wstr F_lib = { 3, 3, 1, 0, "lib" };
static wstr F_id = { 2, 2, 1, 0, "id" };
static wstr F_fn = { 2, 2, 1, 0, "fn" };
static wstr F_top = { 3, 3, 1, 0, "top" };
static wstr F_loops = { 5, 5, 1, 0, "loops" };
static wstr F_key = { 3, 3, 1, 0, "key" };
static wstr F_exports = { 7, 7, 1, 0, "exports" };
static wstr F_fickleTargets = { 13, 13, 1, 0, "fickleTargets" };
static wstr F_useMod = { 6, 6, 1, 0, "useMod" };
static wstr F_steals = { 6, 6, 1, 0, "steals" };
static wstr F_node = { 4, 4, 1, 0, "node" };
static wstr F_notLibrary = { 10, 10, 1, 0, "notLibrary" };
static wstr F_program = { 7, 7, 1, 0, "program" };
static wstr F_js = { 2, 2, 1, 0, "js" };
static wstr F_children = { 8, 8, 1, 0, "children" };
static wstr F_d = { 1, 1, 1, 0, "d" };
static wstr F_depth = { 5, 5, 1, 0, "depth" };
static wstr F_cname = { 5, 5, 1, 0, "cname" };
static wstr F_captured = { 8, 8, 1, 0, "captured" };
static wstr F_schemeFn = { 8, 8, 1, 0, "schemeFn" };
static wstr F_pre = { 3, 3, 1, 0, "pre" };
static wstr F_cs = { 2, 2, 1, 0, "cs" };
static wstr F_proto = { 5, 5, 1, 0, "proto" };
static wstr F_lines = { 5, 5, 1, 0, "lines" };
static const wstr *const K0[] = { &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars };
static const wstr *const K1[] = { &F_word, &F_message };
static const wstr *const K2[] = { &F_kind, &F_error };
static const wstr *const K3[] = { &F_t, &F_pos, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts };
static const wstr *const K4[] = { &F_tokens, &F_pos };
static const wstr *const K5[] = { &F_tokens, &F_end };
static const wstr *const K6[] = { &F_k, &F_e, &F_pos, &F_end, &F_handler };
static const wstr *const K7[] = { &F_stmts, &F_end };
static const wstr *const K8[] = { &F_k, &F_pos, &F_end, &F_handler };
static const wstr *const K9[] = { &F_pos, &F_param, &F_paramPos, &F_body };
static const wstr *const K10[] = { &F_k, &F_neg, &F_cond, &F_body, &F_pos, &F_end, &F_handler };
static const wstr *const K11[] = { &F_k, &F_code, &F_pos, &F_end, &F_handler };
static const wstr *const K12[] = { &F_k, &F_op, &F_e, &F_target, &F_pos, &F_end, &F_handler };
static const wstr *const K13[] = { &F_k, &F_v, &F_pos, &F_paren };
static const wstr *const K14[] = { &F_k, &F_e, &F_isShifty, &F_isFickle, &F_name, &F_namePos, &F_pos, &F_end, &F_handler };
static const wstr *const K15[] = { &F_k, &F_e, &F_op, &F_target, &F_pos, &F_end, &F_handler };
static const wstr *const K16[] = { &F_k, &F_path, &F_pathPos, &F_name, &F_namePos, &F_pos, &F_end, &F_handler };
static const wstr *const K17[] = { &F_name, &F_pos };
static const wstr *const K18[] = { &F_k, &F_names, &F_pos, &F_end, &F_handler };
static const wstr *const K19[] = { &F_name, &F_isShifty, &F_isFickle, &F_pos };
static const wstr *const K20[] = { &F_k, &F_name, &F_namePos, &F_params, &F_body, &F_pos, &F_end, &F_handler };
static const wstr *const K21[] = { &F_k, &F_name, &F_namePos, &F_members, &F_pos, &F_end, &F_handler };
static const wstr *const K22[] = { &F_stmts, &F_value, &F_close };
static const wstr *const K23[] = { &F_k, &F_name, &F_pos, &F_paren };
static const wstr *const K24[] = { &F_k, &F_callee, &F_args, &F_pos, &F_paren };
static const wstr *const K25[] = { &F_k, &F_field, &F_rec, &F_pos, &F_paren };
static const wstr *const K26[] = { &F_k, &F_arg, &F_pos, &F_paren };
static const wstr *const K27[] = { &F_k, &F_c, &F_a, &F_b, &F_pos, &F_paren };
static const wstr *const K28[] = { &F_k, &F_op, &F_l, &F_r, &F_pos, &F_paren };
static const wstr *const K29[] = { &F_k, &F_op, &F_e, &F_pos, &F_paren };
static const wstr *const K30[] = { &F_k, &F_index, &F_arr, &F_pos, &F_paren };
static const wstr *const K31[] = { &F_k, &F_parts, &F_pos, &F_paren };
static const wstr *const K32[] = { &F_k, &F_pos, &F_paren };
static const wstr *const K33[] = { &F_k, &F_items, &F_pos, &F_paren };
static const wstr *const K34[] = { &F_name, &F_e, &F_pos };
static const wstr *const K35[] = { &F_k, &F_fields, &F_pos, &F_paren };
static const wstr *const K36[] = { &F_values, &F_body, &F_pos };
static const wstr *const K37[] = { &F_k, &F_subject, &F_alibis, &F_els, &F_pos, &F_paren };
static const wstr *const K38[] = { &F_neg, &F_cond, &F_body, &F_pos };
static const wstr *const K39[] = { &F_k, &F_branches, &F_els, &F_pos, &F_paren };
static const wstr *const K40[] = { &F_name, &F_arity };
static const wstr *const K41[] = { &F_parent, &F_names };
static const wstr *const K42[] = { &F_kind, &F_name, &F_isShifty, &F_isFickle, &F_namePos, &F_visibleAt, &F_fnId, &F_arity, &F_mod, &F_lib };
static const wstr *const K43[] = { &F_mod, &F_name, &F_namePos, &F_body };
static const wstr *const K44[] = { &F_mod, &F_name, &F_members };
static const wstr *const K45[] = { &F_id, &F_top };
static const wstr *const K46[] = { &F_fn, &F_loops };
static const wstr *const K47[] = { &F_key, &F_pos };
static const wstr *const K48[] = { &F_mod, &F_namePos, &F_pos, &F_useMod };
static const wstr *const K49[] = { &F_stmts, &F_value };
static const wstr *const K50[] = { &F_name, &F_lib };
static const wstr *const K51[] = { &F_k, &F_js, &F_pos, &F_paren };
static const wstr *const K52[] = { &F_name, &F_kind, &F_d };
static const wstr *const K53[] = { &F_name, &F_parent, &F_depth, &F_top, &F_children, &F_cname };
static const wstr *const K54[] = { &F_kind, &F_name, &F_fn, &F_cname, &F_isFickle, &F_captured, &F_schemeFn };
static const wstr *const K55[] = { &F_c, &F_node };
static const wstr *const K56[] = { &F_c, &F_k };
static const wstr *const K57[] = { &F_pre, &F_cs };
static const wstr *const K58[] = { &F_proto, &F_lines };
static const wstr *const K59[] = { &F_node, &F_lib };
static const wstr *const K60[] = { &F_file, &F_chars, &F_path, &F_program, &F_steals, &F_exports, &F_notLibrary, &F_fickleTargets };
struct fr29 { wv v80_cs; wv v81_n; wv v82_i; wv v83_prevT; wv v84_nlNow; };
struct fr53 { wv v202_toks; wv v204_p; wv v205_depth; };
struct fr101 { wv v388_modules; wv v390_errors; wv v391_nextFn; wv v392_curMod; wv v393_schemes; wv v394_uses; wv v395_lineups; wv v428_topFn; };
struct fr141 { wv v640_modules; wv v642_out; wv v643_indent; wv v644_n; wv v645_loops; wv v646_iife; wv v647_mod; wv v648_stolen; wv v649_lineups; wv v650_fickles; };
struct fr185 { wv v827_modules; wv v828_runtime; wv v829_n; wv v830_tmps; wv v831_strings; wv v832_fields; wv v833_shapes; wv v834_lineupDefs; wv v835_globals; wv v836_frames; wv v837_functions; wv v838_out; wv v839_indent; wv v840_loops; wv v841_iife; wv v842_decls; wv v843_fns; wv v844_tops; wv v845_slots; wv v846_curMod; wv v847_mod; wv v848_curSlot; wv v849_stolen; wv v850_ownLineups; wv v851_cur; };
struct fr254 { wv v1227_modules; };
static wv w2_srcFile;
static wv w2_srcChars;
static wv w2_warnings;
static wv w3_keywords;
static wv w3_reserved;
static wv w3_bangWords;
static wv w3_cheatOps;
static wv w3_ops;
static wv w4_exprStart;
static wv w4_whineOk;
static wv w4_callees;
static wv w4_postfixHint;
static wv w5_stdlib;
static wv w5_neverCompletes;
static wv w5_whineValueUsed;
static wv w5_libraryTop;
static wv w6_binOps;
static wv w6_binFns;
static wv w6_scalarKinds;
static wv w6_plainKinds;
static wv w7_binOps;
static wv w7_binFns;
static wv w7_scalarKinds;
static wv w7_plainKinds;
static wv w7_signalers;
static wv w7_constK;
static wv w7_pureK;
static wv w7_effectK;
static wv w_usage;
static wv w_argv;
static wv w_mode;
static wv w_input;
static wv w_output;
static wv w_target;
static wv w_runtimePath;
static wv w_cc;
static wv w_arg;
static wv w_inspect;
static wv w_dotWah;
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
static wv v1254_a;
static wv v1263_tokens;
static wv v1264_program;
static wv v1265_p;
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
static wv w2_fail(wv v55_pos, wv v56_message);
static wv w3_isNumeralShape(wv v57_w);
static wv w3_isUpper(wv v61_w);
static wv w3_digitRun(wv v62_cs, wv v63_k);
static wv w3_exponentEnd(wv v65_cs, wv v66_k);
static wv w3_bracketLiteral(wv v69_b);
static wv w3_simpleEscape(wv v79_e);
static wv w3_lex(wv p_v80_cs);
static wv s86_ch(struct fr29 *up, wv v100_k);
static wv s87_cut(struct fr29 *up, wv v101_a, wv v102_b);
static wv s88_startsWith(struct fr29 *up, wv v103_k, wv v104_s);
static wv s89_find(struct fr29 *up, wv v107_s, wv v108_from);
static wv s90_tok(struct fr29 *up, wv v110_t, wv v111_pos, wv v112_sp, wv v113_v, wv v114_spelling, wv v115_parts);
static wv s91_plain(struct fr29 *up, wv v116_t, wv v117_pos, wv v118_sp);
static wv s92_skip(struct fr29 *up);
static wv s93_next(struct fr29 *up);
static wv s94_word(struct fr29 *up, wv v138_start, wv v139_sp);
static wv s95_bracketNumeral(struct fr29 *up, wv v143_start, wv v144_open, wv v145_sp);
static wv s96_numeral(struct fr29 *up, wv v151_start, wv v152_sp, wv v153_upper, wv v154_kind, wv v155_spelling);
static wv s97_sigil(struct fr29 *up, wv v157_start, wv v158_sp);
static wv s98_string(struct fr29 *up, wv v160_start, wv v161_sp);
static wv s99_interpolation(struct fr29 *up, wv v175_open);
static wv w4_precedence(wv v184_t);
static wv w4_isIdent(wv v185_s);
static wv w4_describe(wv v188_tk);
static wv w4_failAtHint(wv v189_tk, wv v190_message, wv v191_hint);
static wv w4_failAt(wv v192_tk, wv v193_message);
static wv w4_simpleStatement(wv v194_k, wv v195_e);
static wv w4_parse(wv v196_toks);
static wv w4_hasEffect(wv v197_e);
static wv w4_parser(wv p_v202_toks, wv v203_inner);
static wv s206_peek(struct fr53 *up);
static wv s207_at(struct fr53 *up, wv v243_type);
static wv s208_atCall(struct fr53 *up);
static wv s209_next(struct fr53 *up);
static wv s210_accept(struct fr53 *up, wv v245_type);
static wv s211_expect(struct fr53 *up, wv v246_type, wv v247_msg, wv v248_hint);
static wv s212_lintSpace(struct fr53 *up, wv v251_op);
static wv s213_program(struct fr53 *up);
static wv s214_statement(struct fr53 *up);
static wv s215_statementBody(struct fr53 *up, wv v263_tk);
static wv s216_exprStatement(struct fr53 *up);
static wv s217_stealStatement(struct fr53 *up, wv v273_kw, wv v274_e);
static wv s218_atBribe(struct fr53 *up);
static wv s219_bribeStatement(struct fr53 *up);
static wv s220_schemeStatement(struct fr53 *up);
static wv s221_lineupStatement(struct fr53 *up);
static wv s222_cond(struct fr53 *up, wv v293_kw);
static wv s223_block(struct fr53 *up);
static wv s224_lvalue(struct fr53 *up);
static wv s225_expr(struct fr53 *up);
static wv s226_seq(struct fr53 *up, wv v303_list);
static wv s227_zeroCall(struct fr53 *up);
static wv s228_call(struct fr53 *up, wv v310_args);
static wv s229_continueOps(struct fr53 *up, wv v314_left);
static wv s230_ternary(struct fr53 *up);
static wv s231_ternaryTail(struct fr53 *up, wv v316_c);
static wv s232_binary(struct fr53 *up, wv v321_min);
static wv s233_binaryFrom(struct fr53 *up, wv v322_left, wv v323_min);
static wv s234_unary(struct fr53 *up);
static wv s235_powerTail(struct fr53 *up, wv v330_base);
static wv s236_postfix(struct fr53 *up);
static wv s237_primary(struct fr53 *up);
static wv s238_record(struct fr53 *up, wv v346_open);
static wv s239_parseSuspect(struct fr53 *up);
static wv s240_alibiValue(struct fr53 *up);
static wv s241_fieldValue(struct fr53 *up);
static wv s242_chain(struct fr53 *up);
static wv w5_stdSig(wv v371_name);
static wv w5_modeOf(wv v373_x);
static wv w5_plural(wv v374_n);
static wv w5_lineupText(wv v375_x);
static wv w5_alibiKey(wv v376_v);
static wv w5_alibiText(wv v377_v);
static wv w5_getsAwayWithValue(wv v378_list);
static wv w5_lineIn(wv v383_chars, wv v384_pos);
static wv w5_lineOf(wv v387_pos);
static wv w5_check(wv p_v388_modules, wv v389_requireExit);
static wv s396_error(struct fr101 *up, wv v433_pos, wv v434_message, wv v435_hint);
static wv s397_enter(struct fr101 *up, wv v436_m);
static wv s398_lineupMembers(struct fr101 *up, wv v437_d);
static wv s399_firstUse(struct fr101 *up, wv v439_mod, wv v440_declPos);
static wv s400_requireValue(struct fr101 *up, wv v442_b);
static wv s401_newScope(struct fr101 *up, wv v445_parent);
static wv s402_decl(struct fr101 *up, wv v447_kind, wv v448_name, wv v449_mode, wv v450_namePos, wv v451_visibleAt, wv v452_fnId, wv v453_arity);
static wv s403_own(struct fr101 *up, wv v454_scope, wv v455_name);
static wv s404_declare(struct fr101 *up, wv v457_scope, wv v458_name, wv v459_d);
static wv s405_lookup(struct fr101 *up, wv v463_scope, wv v464_name);
static wv s406_ref(struct fr101 *up, wv v467_name, wv v468_pos, wv v469_scope, wv v470_ctx);
static wv s407_block(struct fr101 *up, wv v472_b, wv v473_scope, wv v474_ctx);
static wv s408_stmt(struct fr101 *up, wv v479_s, wv v480_scope, wv v481_ctx);
static wv s409_whineHandler(struct fr101 *up, wv v488_s, wv v489_scope, wv v490_ctx);
static wv s410_checkSuspect(struct fr101 *up, wv v493_e, wv v494_scope, wv v495_ctx, wv v496_stmtLevel);
static wv s411_moduleNamed(struct fr101 *up, wv v516_x, wv v517_scope);
static wv s412_exported(struct fr101 *up, wv v519_lib, wv v520_name);
static wv s413_lineupNamed(struct fr101 *up, wv v523_x, wv v524_scope);
static wv s414_bribed(struct fr101 *up, wv v528_e, wv v529_m);
static wv s415_unknownModule(struct fr101 *up, wv v531_x, wv v532_scope);
static wv s416_qualified(struct fr101 *up, wv v534_e, wv v535_m);
static wv s417_alibiLineup(struct fr101 *up, wv v538_v, wv v539_scope);
static wv s418_member(struct fr101 *up, wv v545_e, wv v546_d);
static wv s419_chain(struct fr101 *up, wv v551_e, wv v552_scope, wv v553_ctx, wv v554_stmtLevel);
static wv s420_lvalue(struct fr101 *up, wv v557_t, wv v558_scope, wv v559_ctx);
static wv s421_mustBeShifty(struct fr101 *up, wv v566_d, wv v567_name, wv v568_pos, wv v569_message, wv v570_rebind);
static wv s422_unknownStd(struct fr101 *up, wv v572_node);
static wv s423_expr(struct fr101 *up, wv v573_e, wv v574_scope, wv v575_ctx);
static wv s424_callee(struct fr101 *up, wv v585_e, wv v586_scope, wv v587_used);
static wv s425_stealNames(struct fr101 *up, wv v594_m, wv v595_scope);
static wv s426_libraryRules(struct fr101 *up, wv v604_m, wv v605_scope, wv v606_isEntry, wv v607_last);
static wv s427_checkModule(struct fr101 *up, wv v614_m, wv v615_isEntry);
static wv w6_binFn(wv v626_op);
static wv w6_modeOf(wv v628_x);
static wv w6_unFn(wv v629_op);
static wv w6_jsBool(wv v630_b);
static wv w6_repeat(wv v631_s, wv v632_count);
static wv w6_jsFloat(wv v635_x);
static wv w6_generate(wv p_v640_modules, wv v641_runtime);
static wv s651_modId(struct fr141 *up, wv v686_m);
static wv s652_enter(struct fr141 *up, wv v687_m);
static wv s653_id(struct fr141 *up, wv v691_name);
static wv s654_stolenFrom(struct fr141 *up, wv v692_x);
static wv s655_exportKind(struct fr141 *up, wv v694_lib, wv v695_name);
static wv s656_lineupJs(struct fr141 *up, wv v698_x);
static wv s657_emit(struct fr141 *up, wv v700_line);
static wv s658_tmp(struct fr141 *up);
static wv s659_stmts(struct fr141 *up, wv v702_list);
static wv s660_blockBody(struct fr141 *up, wv v704_b);
static wv s661_valueOf(struct fr141 *up, wv v705_b);
static wv s662_stmt(struct fr141 *up, wv v706_s);
static wv s663_schemeDef(struct fr141 *up, wv v708_s);
static wv s664_lineupDef(struct fr141 *up, wv v714_s, wv v715_m);
static wv s665_bindRhs(struct fr141 *up, wv v718_e, wv v719_mode);
static wv s666_cheatStmt(struct fr141 *up, wv v722_s);
static wv s667_record(struct fr141 *up, wv v729_e, wv v730_mutable);
static wv s668_whineStmt(struct fr141 *up, wv v734_s);
static wv s669_chainStmt(struct fr141 *up, wv v743_e);
static wv s670_test(struct fr141 *up, wv v746_b);
static wv s671_branchCount(struct fr141 *up, wv v747_e);
static wv s672_branchBody(struct fr141 *up, wv v748_e, wv v749_i);
static wv s673_branchTest(struct fr141 *up, wv v750_e, wv v751_t, wv v752_i);
static wv s674_isPlain(struct fr141 *up, wv v753_e);
static wv s675_ternaryChain(struct fr141 *up, wv v757_e, wv v758_t);
static wv s676_branchFunction(struct fr141 *up, wv v761_e, wv v762_t);
static wv s677_chainExpr(struct fr141 *up, wv v770_e);
static wv s678_alibiTest(struct fr141 *up, wv v772_t, wv v773_a);
static wv s679_suspectElse(struct fr141 *up, wv v776_e, wv v777_t);
static wv s680_suspectStmt(struct fr141 *up, wv v778_e);
static wv s681_suspectExpr(struct fr141 *up, wv v782_e);
static wv s682_callJs(struct fr141 *up, wv v787_callee, wv v788_args);
static wv s683_exprList(struct fr141 *up, wv v790_list);
static wv s684_expr(struct fr141 *up, wv v793_e);
static wv w7_binFn(wv v804_op);
static wv w7_modeOf(wv v806_x);
static wv w7_unFn(wv v807_op);
static wv w7_repeat(wv v808_s, wv v809_count);
static wv w7_utf8Length(wv v812_s);
static wv w7_cString(wv v817_s);
static wv w7_wstrInit(wv v823_s);
static wv w7_values(wv v826_cs);
static wv w7_generateC(wv p_v827_modules, wv p_v828_runtime);
static wv s852_modId(struct fr185 *up, wv v912_m);
static wv s853_hasFrame(struct fr185 *up, wv v913_f);
static wv s854_newScope(struct fr185 *up, wv v914_parent);
static wv s855_put(struct fr185 *up, wv v916_scope, wv v917_name, wv v918_kind, wv v919_d);
static wv s856_find(struct fr185 *up, wv v920_scope, wv v921_name);
static wv s857_kindOf(struct fr185 *up, wv v925_scope, wv v926_name);
static wv s858_newFn(struct fr185 *up, wv v928_name, wv v929_parent);
static wv s859_bind(struct fr185 *up, wv v931_kind, wv v932_name, wv v933_f, wv v934_atTop, wv v935_mode);
static wv s860_ablock(struct fr185 *up, wv v938_b, wv v939_scope, wv v940_f, wv v941_topScope);
static wv s861_astmt(struct fr185 *up, wv v949_s, wv v950_scope, wv v951_f, wv v952_topScope);
static wv s862_use(struct fr185 *up, wv v963_x, wv v964_scope, wv v965_f);
static wv s863_atarget(struct fr185 *up, wv v967_t, wv v968_scope, wv v969_f);
static wv s864_notValue(struct fr185 *up, wv v970_x, wv v971_scope);
static wv s865_aexpr(struct fr185 *up, wv v973_e, wv v974_scope, wv v975_f);
static wv s866_analyze(struct fr185 *up, wv v983_m);
static wv s867_enter(struct fr185 *up, wv v990_m);
static wv s868_emit(struct fr185 *up, wv v994_line);
static wv s869_tmp(struct fr185 *up);
static wv s870_str(struct fr185 *up, wv v996_s);
static wv s871_fieldRef(struct fr185 *up, wv v998_name);
static wv s872_lineupDef(struct fr185 *up, wv v999_s, wv v1000_m);
static wv s873_stolenFrom(struct fr185 *up, wv v1004_x);
static wv s874_lineupIn(struct fr185 *up, wv v1006_list, wv v1007_name);
static wv s875_lineupOf(struct fr185 *up, wv v1009_x);
static wv s876_place(struct fr185 *up, wv v1014_d);
static wv s877_tdz(struct fr185 *up, wv v1016_d);
static wv s878_read(struct fr185 *up, wv v1017_d);
static wv s879_seq(struct fr185 *up, wv v1018_args);
static wv s880_wrap(struct fr185 *up, wv v1025_sq, wv v1026_inner);
static wv s881_stmts(struct fr185 *up, wv v1027_list);
static wv s882_resets(struct fr185 *up, wv v1029_b);
static wv s883_blockBody(struct fr185 *up, wv v1033_b);
static wv s884_valueOf(struct fr185 *up, wv v1034_b);
static wv s885_stmt(struct fr185 *up, wv v1035_s);
static wv s886_signal(struct fr185 *up, wv v1037_code);
static wv s887_claimDecl(struct fr185 *up, wv v1038_d, wv v1039_value);
static wv s888_schemeDef(struct fr185 *up, wv v1041_s);
static wv s889_bindRhs(struct fr185 *up, wv v1061_e, wv v1062_mode);
static wv s890_cheatStmt(struct fr185 *up, wv v1064_s);
static wv s891_exprs(struct fr185 *up, wv v1079_list);
static wv s892_array(struct fr185 *up, wv v1082_e, wv v1083_mutable);
static wv s893_record(struct fr185 *up, wv v1085_e, wv v1086_mutable);
static wv s894_whineStmt(struct fr185 *up, wv v1094_s);
static wv s895_chainStmt(struct fr185 *up, wv v1103_e);
static wv s896_test(struct fr185 *up, wv v1106_b);
static wv s897_branchCount(struct fr185 *up, wv v1107_e);
static wv s898_branchBody(struct fr185 *up, wv v1108_e, wv v1109_i);
static wv s899_branchTest(struct fr185 *up, wv v1110_e, wv v1111_t, wv v1112_i);
static wv s900_isPlain(struct fr185 *up, wv v1113_e);
static wv s901_ternaryChain(struct fr185 *up, wv v1117_e, wv v1118_t);
static wv s902_branchValue(struct fr185 *up, wv v1121_e, wv v1122_t, wv v1123_subject);
static wv s903_chainExpr(struct fr185 *up, wv v1132_e);
static wv s904_alibiTest(struct fr185 *up, wv v1133_t, wv v1134_a);
static wv s905_suspectElse(struct fr185 *up, wv v1137_e, wv v1138_t);
static wv s906_suspectStmt(struct fr185 *up, wv v1139_e);
static wv s907_suspectExpr(struct fr185 *up, wv v1143_e);
static wv s908_callC(struct fr185 *up, wv v1147_callee, wv v1148_args, wv v1149_checked);
static wv s909_expr(struct fr185 *up, wv v1158_e);
static wv s910_assemble(struct fr185 *up);
static wv w8_dumpTokens(wv v1187_toks, wv v1188_indent);
static wv w8_sorted(wv v1194_xs);
static wv w8_dumpValue(wv v1199_x, wv v1200_indent);
static wv w9_dirOf(wv v1209_path);
static wv w9_normalize(wv v1212_path);
static wv w9_with(wv v1217_xs, wv v1218_x);
static wv w9_failIn(wv v1221_file, wv v1222_chars, wv v1223_pos, wv v1224_message);
static wv w9_loadProgram(wv v1225_entryFile, wv v1226_entryChars);
static wv s1228_loaded(struct fr254 *up, wv v1231_path);
static wv s1229_visit(struct fr254 *up, wv v1233_file, wv v1234_chars, wv v1235_path, wv v1236_chain);
static wv w_die(void);
static wv w_load(wv v1250_file);
static wv w_onWindows(void);
static wv w_tryRead(wv v1256_path);
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

static wv w2_fail(wv v55_pos, wv v56_message) {
  rt_enter("fail");
  v55_pos = rt_bind(v55_pos, 0);
  v56_message = rt_bind(v56_message, 0);
  (void)rt_chk(w2_failHint(v55_pos, v56_message, RT_NULL), "fail");
  RT_RET(RT_NULL);
}

static wv w3_isNumeralShape(wv v57_w) {
  rt_enter("isNumeralShape");
  v57_w = rt_bind(v57_w, 0);
  wv v58_cs = rt_bind(std_split(v57_w, rt_s(&S13)), 0);
  wv v59_len = rt_bind(std_length(v58_cs), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_lt(v59_len, rt_i(2)), "||") || rt_bool(rt_b(rt_bool(rt_ne(rt_idx(rt_i(0), v58_cs), rt_s(&S94)), "&&") && rt_bool(rt_ne(rt_idx(rt_i(0), v58_cs), rt_s(&S95)), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(rt_idx(rt_i(-1), v58_cs), rt_s(&S96)), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v58_cs), rt_s(&S97)), "&&")), "||")))) {
    RT_RET(rt_b(0));
  }
  wv v60_k = rt_i(1);
  while (rt_cond(({ wv t73 = v60_k; wv t74 = rt_sub(v59_len, rt_i(1)); rt_lt(t73, t74); }))) {
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_idx(v60_k, v58_cs), rt_s(&S6)), "&&") && rt_bool(rt_ne(rt_idx(v60_k, v58_cs), rt_s(&S8)), "&&")))) {
      RT_RET(rt_b(0));
    }
    v60_k = rt_add(v60_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv w3_isUpper(wv v61_w) {
  rt_enter("isUpper");
  v61_w = rt_bind(v61_w, 0);
  RT_RET(({ wv t75 = v61_w; wv t76 = rt_chk(w1_toUpper(v61_w), "isUpper"); rt_eq(t75, t76); }));
}

static wv w3_digitRun(wv v62_cs, wv v63_k) {
  rt_enter("digitRun");
  v62_cs = rt_bind(v62_cs, 0);
  v63_k = rt_bind(v63_k, 0);
  wv v64_j = rt_bind(v63_k, 1);
  while (rt_cond(rt_b(rt_bool(({ wv t77 = v64_j; wv t78 = std_length(v62_cs); rt_lt(t77, t78); }), "&&") && rt_bool(rt_chk(w1_isDigit(rt_idx(v64_j, v62_cs)), "digitRun"), "&&")))) {
    v64_j = rt_add(v64_j, rt_i(1));
  }
  RT_RET(v64_j);
}

static wv w3_exponentEnd(wv v65_cs, wv v66_k) {
  rt_enter("exponentEnd");
  v65_cs = rt_bind(v65_cs, 0);
  v66_k = rt_bind(v66_k, 0);
  if (rt_cond(rt_b(rt_bool(({ wv t79 = v66_k; wv t80 = std_length(v65_cs); rt_ge(t79, t80); }), "||") || rt_bool(rt_b(rt_bool(rt_ne(rt_idx(v66_k, v65_cs), rt_s(&S98)), "&&") && rt_bool(rt_ne(rt_idx(v66_k, v65_cs), rt_s(&S99)), "&&")), "||")))) {
    RT_RET(v66_k);
  }
  wv v67_j = rt_add(v66_k, rt_i(1));
  if (rt_cond(rt_b(rt_bool(({ wv t81 = v67_j; wv t82 = std_length(v65_cs); rt_lt(t81, t82); }), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_idx(v67_j, v65_cs), rt_s(&S71)), "||") || rt_bool(rt_eq(rt_idx(v67_j, v65_cs), rt_s(&S72)), "||")), "&&")))) {
    v67_j = rt_add(v67_j, rt_i(1));
  }
  wv v68_end = rt_bind(rt_chk(w3_digitRun(v65_cs, v67_j), "exponentEnd"), 0);
  RT_RET((rt_cond(rt_gt(v68_end, v67_j)) ? v68_end : rt_i(-1)));
}

static wv w3_bracketLiteral(wv v69_b) {
  rt_enter("bracketLiteral");
  v69_b = rt_bind(v69_b, 0);
  wv v70_cs = rt_bind(std_split(v69_b, rt_s(&S13)), 0);
  wv v71_len = rt_bind(std_length(v70_cs), 0);
  wv v72_isInt = rt_b(0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_gt(v71_len, rt_i(2)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v70_cs), rt_s(&S4)), "&&")), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_idx(rt_i(1), v70_cs), rt_s(&S100)), "||") || rt_bool(rt_eq(rt_idx(rt_i(1), v70_cs), rt_s(&S101)), "||")), "&&")))) {
    wv v75_k = rt_i(2);
    while (rt_cond(rt_b(rt_bool(rt_lt(v75_k, v71_len), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_idx(rt_i(1), v70_cs), rt_s(&S100)), "&&") && rt_bool(rt_chk(w1_isHexDigit(rt_idx(v75_k, v70_cs)), "bracketLiteral"), "&&")), "||") || rt_bool(rt_eq(rt_idx(v75_k, v70_cs), rt_s(&S4)), "||")), "||") || rt_bool(rt_eq(rt_idx(v75_k, v70_cs), rt_s(&S102)), "||")), "&&")))) {
      v75_k = rt_add(v75_k, rt_i(1));
    }
    v72_isInt = rt_eq(v75_k, v71_len);
  }
  if (rt_cond(rt_b(rt_bool(rt_gt(v71_len, rt_i(0)), "&&") && rt_bool(({ wv t83 = rt_chk(w3_digitRun(v70_cs, rt_i(0)), "bracketLiteral"); wv t84 = v71_len; rt_eq(t83, t84); }), "&&")))) {
    v72_isInt = rt_b(1);
  }
  if (rt_cond(v72_isInt)) {
    wv t85 = rt_cat(3, (wv[]){ rt_s(&S103), rt_text(v69_b), rt_s(&S104) });
    wv t86 = std_toInt(t85);
    if (rt_sigcode) {
      rt_sigcode = 0;
      {
        t86 = RT_NULL;
      }
    }
    wv v76_v = rt_bind(t86, 0);
    if (rt_cond(rt_eq(v76_v, RT_NULL))) {
      RT_RET(rt_rec(0, 2, K2, (wv[]){ rt_s(&S13), rt_s(&S105) }));
    }
    RT_RET(rt_rec(0, 2, K2, (wv[]){ rt_s(&S106), RT_NULL }));
  }
  wv v73_whole = rt_bind(rt_chk(w3_digitRun(v70_cs, rt_i(0)), "bracketLiteral"), 0);
  wv v74_p = rt_bind(v73_whole, 1);
  if (rt_cond(rt_b(rt_bool(rt_lt(v74_p, v71_len), "&&") && rt_bool(rt_eq(rt_idx(v74_p, v70_cs), rt_s(&S107)), "&&")))) {
    wv v77_frac = rt_bind(({ wv t87 = v70_cs; wv t88 = rt_add(v74_p, rt_i(1)); rt_chk(w3_digitRun(t87, t88), "bracketLiteral"); }), 0);
    if (rt_cond(({ wv t89 = v77_frac; wv t90 = rt_add(v74_p, rt_i(1)); rt_gt(t89, t90); }))) {
      v74_p = rt_bind(v77_frac, 1);
    } else {
      v74_p = rt_i(-1);
    }
  }
  if (rt_cond(rt_b(rt_bool(rt_gt(v73_whole, rt_i(0)), "&&") && rt_bool(rt_ge(v74_p, rt_i(0)), "&&")))) {
    if (rt_cond(({ wv t91 = rt_chk(w3_exponentEnd(v70_cs, v74_p), "bracketLiteral"); wv t92 = v71_len; rt_eq(t91, t92); }))) {
      wv t93 = rt_cat(3, (wv[]){ rt_s(&S103), rt_text(v69_b), rt_s(&S104) });
      wv t94 = std_toFloat(t93);
      if (rt_sigcode) {
        rt_sigcode = 0;
        {
          t94 = RT_NULL;
        }
      }
      wv v78_f = rt_bind(t94, 0);
      if (rt_cond(rt_eq(v78_f, RT_NULL))) {
        RT_RET(rt_rec(0, 2, K2, (wv[]){ rt_s(&S13), rt_s(&S108) }));
      }
      RT_RET(rt_rec(0, 2, K2, (wv[]){ rt_s(&S109), RT_NULL }));
    }
  }
  if (rt_cond(rt_b(rt_bool(rt_lt(v73_whole, v71_len), "&&") && rt_bool(rt_eq(rt_idx(v73_whole, v70_cs), rt_s(&S107)), "&&")))) {
    if (rt_cond(({ wv t99 = ({ wv t97 = v70_cs; wv t98 = ({ wv t95 = v70_cs; wv t96 = rt_add(v73_whole, rt_i(1)); rt_chk(w3_digitRun(t95, t96), "bracketLiteral"); }); rt_chk(w3_exponentEnd(t97, t98), "bracketLiteral"); }); wv t100 = v71_len; rt_eq(t99, t100); }))) {
      RT_RET(rt_rec(0, 2, K2, (wv[]){ rt_s(&S13), rt_s(&S110) }));
    }
  }
  RT_RET(rt_rec(0, 2, K2, (wv[]){ rt_s(&S13), rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v69_b), rt_s(&S112) }) }));
}

static wv w3_simpleEscape(wv v79_e) {
  rt_enter("simpleEscape");
  v79_e = rt_bind(v79_e, 0);
  if (rt_cond(rt_eq(v79_e, rt_s(&S113)))) {
    RT_RET(rt_s(&S2));
  }
  if (rt_cond(rt_eq(v79_e, rt_s(&S114)))) {
    RT_RET(rt_s(&S3));
  }
  if (rt_cond(rt_eq(v79_e, rt_s(&S115)))) {
    RT_RET(rt_s(&S1));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v79_e, rt_s(&S19)), "||") || rt_bool(rt_eq(v79_e, rt_s(&S18)), "||")), "||") || rt_bool(rt_eq(v79_e, rt_s(&S116)), "||")))) {
    RT_RET(v79_e);
  }
  RT_RET(RT_NULL);
}

static wv w3_lex(wv p_v80_cs) {
  rt_enter("lex");
  struct fr29 fr = { 0 };
  fr.v80_cs = rt_bind(p_v80_cs, 0);
  fr.v81_n = rt_bind(std_length(fr.v80_cs), 0);
  fr.v82_i = rt_i(0);
  fr.v83_prevT = rt_s(&S13);
  fr.v84_nlNow = rt_b(0);
  wv v85_toks = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v183_t0 = rt_bind(rt_chk(s93_next(&fr), "lex"), 0);
    (void)std_push(v85_toks, v183_t0);
    if (rt_cond(rt_eq(rt_get(&F_t, v183_t0), rt_s(&S121)))) {
      break;
    }
  }
  RT_RET(v85_toks);
}

static wv s86_ch(struct fr29 *up, wv v100_k) {
  rt_enter("ch");
  v100_k = rt_bind(v100_k, 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ge(v100_k, rt_i(0)), "&&") && rt_bool(({ wv t101 = v100_k; wv t102 = rt_def(up->v81_n, "n"); rt_lt(t101, t102); }), "&&"))) ? rt_idx(v100_k, up->v80_cs) : rt_s(&S13)));
}

static wv s87_cut(struct fr29 *up, wv v101_a, wv v102_b) {
  rt_enter("cut");
  v101_a = rt_bind(v101_a, 0);
  v102_b = rt_bind(v102_b, 0);
  RT_RET(std_join(std_slice(up->v80_cs, v101_a, v102_b), rt_s(&S13)));
}

static wv s88_startsWith(struct fr29 *up, wv v103_k, wv v104_s) {
  rt_enter("startsWith");
  v103_k = rt_bind(v103_k, 0);
  v104_s = rt_bind(v104_s, 0);
  wv v105_sc = rt_bind(std_split(v104_s, rt_s(&S13)), 0);
  if (rt_cond(({ wv t105 = ({ wv t103 = v103_k; wv t104 = std_length(v105_sc); rt_add(t103, t104); }); wv t106 = rt_def(up->v81_n, "n"); rt_gt(t105, t106); }))) {
    RT_RET(rt_b(0));
  }
  wv v106_q = rt_i(0);
  while (rt_cond(({ wv t107 = v106_q; wv t108 = std_length(v105_sc); rt_lt(t107, t108); }))) {
    if (rt_cond(({ wv t111 = ({ wv t109 = rt_add(v103_k, v106_q); wv t110 = up->v80_cs; rt_idx(t109, t110); }); wv t112 = rt_idx(v106_q, v105_sc); rt_ne(t111, t112); }))) {
      RT_RET(rt_b(0));
    }
    v106_q = rt_add(v106_q, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s89_find(struct fr29 *up, wv v107_s, wv v108_from) {
  rt_enter("find");
  v107_s = rt_bind(v107_s, 0);
  v108_from = rt_bind(v108_from, 0);
  wv v109_q = rt_bind(v108_from, 1);
  while (rt_cond(({ wv t113 = v109_q; wv t114 = rt_def(up->v81_n, "n"); rt_lt(t113, t114); }))) {
    if (rt_cond(rt_chk(s88_startsWith(up, v109_q, v107_s), "find"))) {
      RT_RET(v109_q);
    }
    v109_q = rt_add(v109_q, rt_i(1));
  }
  RT_RET(rt_i(-1));
}

static wv s90_tok(struct fr29 *up, wv v110_t, wv v111_pos, wv v112_sp, wv v113_v, wv v114_spelling, wv v115_parts) {
  rt_enter("tok");
  v110_t = rt_bind(v110_t, 0);
  v111_pos = rt_bind(v111_pos, 0);
  v112_sp = rt_bind(v112_sp, 0);
  v113_v = rt_bind(v113_v, 0);
  v114_spelling = rt_bind(v114_spelling, 0);
  v115_parts = rt_bind(v115_parts, 0);
  wv t115 = rt_bind(v110_t, 1);
  (void)rt_def(up->v83_prevT, "prevT");
  up->v83_prevT = t115;
  RT_RET(({ wv t116 = v110_t; wv t117 = v111_pos; wv t118 = rt_def(up->v82_i, "i"); wv t119 = v112_sp; wv t120 = rt_def(up->v84_nlNow, "nlNow"); wv t121 = v113_v; wv t122 = v114_spelling; wv t123 = v115_parts; rt_rec(0, 8, K3, (wv[]){ t116, t117, t118, t119, t120, t121, t122, t123 }); }));
}

static wv s91_plain(struct fr29 *up, wv v116_t, wv v117_pos, wv v118_sp) {
  rt_enter("plain");
  v116_t = rt_bind(v116_t, 0);
  v117_pos = rt_bind(v117_pos, 0);
  v118_sp = rt_bind(v118_sp, 0);
  RT_RET(rt_chk(s90_tok(up, v116_t, v117_pos, v118_sp, RT_NULL, RT_NULL, RT_NULL), "plain"));
}

static wv s92_skip(struct fr29 *up) {
  rt_enter("skip");
  wv v119_skipped = rt_b(0);
  while (rt_cond(rt_b(1))) {
    wv v120_c = rt_bind(rt_chk(s86_ch(up, rt_def(up->v82_i, "i")), "skip"), 0);
    if (rt_cond(rt_chk(w1_isSpace(v120_c), "skip"))) {
      up->v82_i = rt_add(rt_def(up->v82_i, "i"), rt_i(1));
      v119_skipped = rt_b(1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v120_c, rt_s(&S74)), "&&") && rt_bool(rt_eq(rt_chk(s86_ch(up, rt_add(rt_def(up->v82_i, "i"), rt_i(1))), "skip"), rt_s(&S74)), "&&")))) {
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v119_skipped), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, rt_sub(rt_def(up->v82_i, "i"), rt_i(1))), "skip")), "skip"), "&&")), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, rt_add(rt_def(up->v82_i, "i"), rt_i(2))), "skip")), "skip"), "&&")))) {
        (void)({ wv t124 = w2_warnings; wv t125 = rt_chk(w2_warn(rt_def(up->v82_i, "i"), rt_s(&S117), rt_s(&S118)), "skip"); std_push(t124, t125); });
      }
      while (rt_cond(rt_b(rt_bool(({ wv t126 = rt_def(up->v82_i, "i"); wv t127 = rt_def(up->v81_n, "n"); rt_lt(t126, t127); }), "&&") && rt_bool(rt_ne(({ wv t128 = rt_def(up->v82_i, "i"); wv t129 = up->v80_cs; rt_idx(t128, t129); }), rt_s(&S2)), "&&")))) {
        up->v82_i = rt_add(rt_def(up->v82_i, "i"), rt_i(1));
      }
      v119_skipped = rt_b(1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v120_c, rt_s(&S74)), "&&") && rt_bool(rt_eq(rt_chk(s86_ch(up, rt_add(rt_def(up->v82_i, "i"), rt_i(1))), "skip"), rt_s(&S73)), "&&")))) {
      wv v121_close = rt_bind(rt_chk(s89_find(up, rt_s(&S119), rt_add(rt_def(up->v82_i, "i"), rt_i(2))), "skip"), 0);
      if (rt_cond(rt_lt(v121_close, rt_i(0)))) {
        (void)rt_chk(w2_fail(rt_def(up->v82_i, "i"), rt_s(&S120)), "skip");
      }
      wv t130 = rt_add(v121_close, rt_i(2));
      (void)rt_def(up->v82_i, "i");
      up->v82_i = t130;
      v119_skipped = rt_b(1);
    } else {
      RT_RET(v119_skipped);
    }
  }
  RT_RET(RT_NULL);
}

static wv s93_next(struct fr29 *up) {
  rt_enter("next");
  wv v122_from = rt_bind(rt_def(up->v82_i, "i"), 0);
  wv v123_sp = rt_b(rt_bool(rt_chk(s92_skip(up), "next"), "||") || rt_bool(rt_eq(rt_def(up->v82_i, "i"), rt_i(0)), "||"));
  wv t131 = rt_b(0);
  (void)rt_def(up->v84_nlNow, "nlNow");
  up->v84_nlNow = t131;
  wv v124_q = rt_bind(v122_from, 1);
  while (rt_cond(({ wv t132 = v124_q; wv t133 = rt_def(up->v82_i, "i"); rt_lt(t132, t133); }))) {
    if (rt_cond(rt_eq(rt_idx(v124_q, up->v80_cs), rt_s(&S2)))) {
      wv t134 = rt_b(1);
      (void)rt_def(up->v84_nlNow, "nlNow");
      up->v84_nlNow = t134;
    }
    v124_q = rt_add(v124_q, rt_i(1));
  }
  wv v125_start = rt_bind(rt_def(up->v82_i, "i"), 0);
  wv v126_c = rt_bind(rt_chk(s86_ch(up, v125_start), "next"), 0);
  if (rt_cond(rt_eq(v126_c, rt_s(&S13)))) {
    RT_RET(rt_chk(s91_plain(up, rt_s(&S121), v125_start, v123_sp), "next"));
  }
  if (rt_cond(rt_chk(w1_isIdStart(v126_c), "next"))) {
    RT_RET(rt_chk(s94_word(up, v125_start, v123_sp), "next"));
  }
  if (rt_cond(rt_eq(v126_c, rt_s(&S18)))) {
    RT_RET(rt_chk(s98_string(up, v125_start, v123_sp), "next"));
  }
  if (rt_cond(rt_eq(v126_c, rt_s(&S116)))) {
    RT_RET(rt_chk(s97_sigil(up, v125_start, v123_sp), "next"));
  }
  if (rt_cond(rt_chk(w1_isDigit(v126_c), "next"))) {
    wv v128_j = rt_bind(v125_start, 1);
    while (rt_cond(rt_b(rt_bool(({ wv t135 = v128_j; wv t136 = rt_add(v125_start, rt_i(40)); rt_lt(t135, t136); }), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_chk(s86_ch(up, v128_j), "next"), rt_s(&S107)), "||") || rt_bool(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, v128_j), "next")), "next"), "||")), "&&")))) {
      v128_j = rt_add(v128_j, rt_i(1));
    }
    wv v129_lit = rt_bind(rt_chk(s87_cut(up, v125_start, v128_j), "next"), 0);
    (void)({ wv t137 = v125_start; wv t138 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v129_lit), rt_s(&S122) }); wv t139 = rt_cat(3, (wv[]){ rt_s(&S123), rt_text(v129_lit), rt_s(&S104) }); rt_chk(w2_failHint(t137, t138, t139), "next"); });
  }
  wv v127_k = rt_i(0);
  while (rt_cond(({ wv t140 = v127_k; wv t141 = std_length(rt_def(w3_cheatOps, "cheatOps")); rt_lt(t140, t141); }))) {
    wv v130_op = rt_bind(({ wv t142 = v127_k; wv t143 = rt_def(w3_cheatOps, "cheatOps"); rt_idx(t142, t143); }), 0);
    wv v131_after = ({ wv t144 = v125_start; wv t145 = std_length(v130_op); rt_add(t144, t145); });
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_chk(s88_startsWith(up, v125_start, v130_op), "next"), "&&") && rt_bool(rt_chk(s88_startsWith(up, v131_after, rt_s(&S38)), "next"), "&&")), "&&") && rt_bool(rt_not(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, rt_add(v131_after, rt_i(5))), "next")), "next")), "&&")))) {
      wv t146 = rt_add(v131_after, rt_i(5));
      (void)rt_def(up->v82_i, "i");
      up->v82_i = t146;
      RT_RET(rt_chk(s90_tok(up, rt_s(&S124), v125_start, v123_sp, v130_op, RT_NULL, RT_NULL), "next"));
    }
    v127_k = rt_add(v127_k, rt_i(1));
  }
  if (rt_cond(rt_eq(v126_c, rt_s(&S87)))) {
    wv v132_limit = rt_add(v125_start, rt_i(64));
    wv v133_j = rt_add(v125_start, rt_i(1));
    while (rt_cond(rt_b(rt_bool(rt_lt(v133_j, v132_limit), "&&") && rt_bool(rt_chk(w1_isJsSpace(rt_chk(s86_ch(up, v133_j), "next")), "next"), "&&")))) {
      v133_j = rt_add(v133_j, rt_i(1));
    }
    wv v134_wordStart = rt_bind(v133_j, 0);
    if (rt_cond(rt_b(rt_bool(rt_lt(v133_j, v132_limit), "&&") && rt_bool(rt_chk(w1_isIdStart(rt_chk(s86_ch(up, v133_j), "next")), "next"), "&&")))) {
      while (rt_cond(rt_b(rt_bool(rt_lt(v133_j, v132_limit), "&&") && rt_bool(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, v133_j), "next")), "next"), "&&")))) {
        v133_j = rt_add(v133_j, rt_i(1));
      }
      wv v135_w = rt_bind(rt_chk(s87_cut(up, v134_wordStart, v133_j), "next"), 0);
      if (rt_cond(({ wv t147 = rt_def(w3_bangWords, "bangWords"); wv t148 = v135_w; rt_chk(w1_contains(t147, t148), "next"); }))) {
        if (rt_cond(({ wv t149 = v134_wordStart; wv t150 = rt_add(v125_start, rt_i(1)); rt_gt(t149, t150); }))) {
          (void)({ wv t151 = v125_start; wv t152 = rt_cat(3, (wv[]){ rt_s(&S125), rt_text(v135_w), rt_s(&S111) }); wv t153 = rt_cat(3, (wv[]){ rt_s(&S126), rt_text(v135_w), rt_s(&S111) }); rt_chk(w2_failHint(t151, t152, t153), "next"); });
        }
        wv t154 = rt_bind(v133_j, 1);
        (void)rt_def(up->v82_i, "i");
        up->v82_i = t154;
        RT_RET(({ wv t155 = rt_add(rt_s(&S87), v135_w); wv t156 = v125_start; wv t157 = v123_sp; rt_chk(s91_plain(up, t155, t156, t157), "next"); }));
      }
    }
  }
  if (rt_cond(rt_chk(s88_startsWith(up, v125_start, rt_s(&S127)), "next"))) {
    (void)rt_chk(w2_failHint(v125_start, rt_s(&S128), rt_s(&S129)), "next");
  }
  if (rt_cond(rt_chk(s88_startsWith(up, v125_start, rt_s(&S70)), "next"))) {
    (void)rt_chk(w2_failHint(v125_start, rt_s(&S130), rt_s(&S131)), "next");
  }
  if (rt_cond(rt_chk(s88_startsWith(up, v125_start, rt_s(&S69)), "next"))) {
    (void)rt_chk(w2_failHint(v125_start, rt_s(&S132), rt_s(&S133)), "next");
  }
  v127_k = rt_i(0);
  while (rt_cond(({ wv t158 = v127_k; wv t159 = std_length(rt_def(w3_ops, "ops")); rt_lt(t158, t159); }))) {
    wv v136_op = rt_bind(({ wv t160 = v127_k; wv t161 = rt_def(w3_ops, "ops"); rt_idx(t160, t161); }), 0);
    if (rt_cond(rt_chk(s88_startsWith(up, v125_start, v136_op), "next"))) {
      wv t164 = ({ wv t162 = v125_start; wv t163 = std_length(v136_op); rt_add(t162, t163); });
      (void)rt_def(up->v82_i, "i");
      up->v82_i = t164;
      if (rt_cond(rt_b(rt_bool(rt_eq(v136_op, rt_s(&S14)), "&&") && rt_bool(rt_eq(rt_def(up->v83_prevT, "prevT"), rt_s(&S14)), "&&")))) {
        (void)({ wv t165 = w2_warnings; wv t166 = rt_chk(w2_warn(v125_start, rt_s(&S134), rt_s(&S135)), "next"); std_push(t165, t166); });
      }
      RT_RET(rt_chk(s91_plain(up, v136_op, v125_start, v123_sp), "next"));
    }
    v127_k = rt_add(v127_k, rt_i(1));
  }
  if (rt_cond(rt_eq(v126_c, rt_s(&S136)))) {
    (void)rt_chk(w2_failHint(v125_start, rt_s(&S137), rt_s(&S138)), "next");
  }
  if (rt_cond(rt_eq(v126_c, rt_s(&S107)))) {
    wv v137_after = rt_bind(rt_chk(s86_ch(up, rt_add(v125_start, rt_i(1))), "next"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(v123_sp, "||") || rt_bool(rt_eq(v137_after, rt_s(&S13)), "||")), "||") || rt_bool(rt_chk(w1_isSpace(v137_after), "next"), "||")), "||") || rt_bool(rt_chk(s88_startsWith(up, rt_add(v125_start, rt_i(1)), rt_s(&S139)), "next"), "||")), "||") || rt_bool(rt_chk(s88_startsWith(up, rt_add(v125_start, rt_i(1)), rt_s(&S140)), "next"), "||")))) {
      (void)rt_chk(w2_failHint(v125_start, rt_s(&S141), rt_s(&S142)), "next");
    }
    wv t167 = rt_add(v125_start, rt_i(1));
    (void)rt_def(up->v82_i, "i");
    up->v82_i = t167;
    RT_RET(rt_chk(s91_plain(up, rt_s(&S107), v125_start, v123_sp), "next"));
  }
  (void)({ wv t168 = v125_start; wv t169 = rt_cat(3, (wv[]){ rt_s(&S143), rt_text(v126_c), rt_s(&S111) }); rt_chk(w2_fail(t168, t169), "next"); });
  RT_RET(RT_NULL);
}

static wv s94_word(struct fr29 *up, wv v138_start, wv v139_sp) {
  rt_enter("word");
  v138_start = rt_bind(v138_start, 0);
  v139_sp = rt_bind(v139_sp, 0);
  wv v140_j = rt_bind(v138_start, 1);
  while (rt_cond(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, v140_j), "word")), "word"))) {
    v140_j = rt_add(v140_j, rt_i(1));
  }
  wv v141_w = rt_bind(rt_chk(s87_cut(up, v138_start, v140_j), "word"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_chk(s86_ch(up, v140_j), "word"), rt_s(&S91)), "&&") && rt_bool(rt_eq(std_length(v141_w), rt_i(2)), "&&")), "&&") && rt_bool(rt_eq(rt_chk(w1_toUpper(v141_w), "word"), rt_s(&S144)), "&&")))) {
    RT_RET(rt_chk(s95_bracketNumeral(up, v138_start, v140_j, v139_sp), "word"));
  }
  if (rt_cond(rt_chk(w3_isNumeralShape(v141_w), "word"))) {
    if (rt_cond(rt_b(rt_bool(rt_not(rt_chk(w3_isUpper(v141_w), "word")), "&&") && rt_bool(({ wv t170 = v141_w; wv t171 = rt_chk(w1_toLower(v141_w), "word"); rt_ne(t170, t171); }), "&&")))) {
      (void)({ wv t172 = v138_start; wv t173 = rt_cat(3, (wv[]){ rt_s(&S145), rt_text(v141_w), rt_s(&S111) }); rt_chk(w2_failHint(t172, t173, rt_s(&S146)), "word"); });
    }
    wv t174 = rt_bind(v140_j, 1);
    (void)rt_def(up->v82_i, "i");
    up->v82_i = t174;
    RT_RET(({ wv t175 = v138_start; wv t176 = v139_sp; wv t177 = rt_chk(w3_isUpper(v141_w), "word"); wv t178 = v141_w; rt_chk(s96_numeral(up, t175, t176, t177, rt_s(&S106), t178), "word"); }));
  }
  wv t179 = rt_bind(v140_j, 1);
  (void)rt_def(up->v82_i, "i");
  up->v82_i = t179;
  if (rt_cond(rt_eq(rt_chk(s86_ch(up, v140_j), "word"), rt_s(&S116)))) {
    (void)rt_chk(w2_failHint(v140_j, rt_s(&S147), rt_s(&S148)), "word");
  }
  if (rt_cond(({ wv t180 = rt_def(w3_keywords, "keywords"); wv t181 = v141_w; rt_chk(w1_contains(t180, t181), "word"); }))) {
    RT_RET(rt_chk(s91_plain(up, v141_w, v138_start, v139_sp), "word"));
  }
  wv v142_k = rt_i(0);
  while (rt_cond(({ wv t182 = v142_k; wv t183 = std_length(rt_def(w3_reserved, "reserved")); rt_lt(t182, t183); }))) {
    if (rt_cond(({ wv t186 = rt_get(&F_word, ({ wv t184 = v142_k; wv t185 = rt_def(w3_reserved, "reserved"); rt_idx(t184, t185); })); wv t187 = v141_w; rt_eq(t186, t187); }))) {
      (void)({ wv t190 = v138_start; wv t191 = rt_get(&F_message, ({ wv t188 = v142_k; wv t189 = rt_def(w3_reserved, "reserved"); rt_idx(t188, t189); })); rt_chk(w2_fail(t190, t191), "word"); });
    }
    v142_k = rt_add(v142_k, rt_i(1));
  }
  RT_RET(rt_chk(s90_tok(up, rt_s(&S149), v138_start, v139_sp, v141_w, RT_NULL, RT_NULL), "word"));
}

static wv s95_bracketNumeral(struct fr29 *up, wv v143_start, wv v144_open, wv v145_sp) {
  rt_enter("bracketNumeral");
  v143_start = rt_bind(v143_start, 0);
  v144_open = rt_bind(v144_open, 0);
  v145_sp = rt_bind(v145_sp, 0);
  wv v146_pre = rt_bind(rt_chk(s87_cut(up, v143_start, v144_open), "bracketNumeral"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(v146_pre, rt_s(&S150)), "&&") && rt_bool(rt_ne(v146_pre, rt_s(&S144)), "&&")))) {
    (void)({ wv t192 = v143_start; wv t193 = rt_cat(3, (wv[]){ rt_s(&S145), rt_text(v146_pre), rt_s(&S151) }); rt_chk(w2_failHint(t192, t193, rt_s(&S146)), "bracketNumeral"); });
  }
  wv v147_upper = rt_eq(v146_pre, rt_s(&S144));
  wv v148_close = rt_bind(rt_chk(s89_find(up, rt_s(&S92), v144_open), "bracketNumeral"), 0);
  if (rt_cond(rt_lt(v148_close, rt_i(0)))) {
    (void)rt_chk(w2_fail(v143_start, rt_s(&S152)), "bracketNumeral");
  }
  wv v149_h = rt_bind((rt_cond(v147_upper) ? rt_s(&S97) : rt_s(&S96)), 0);
  if (rt_cond(({ wv t194 = rt_chk(s86_ch(up, rt_add(v148_close, rt_i(1))), "bracketNumeral"); wv t195 = v149_h; rt_ne(t194, t195); }))) {
    (void)({ wv t196 = rt_add(v148_close, rt_i(1)); wv t197 = rt_cat(3, (wv[]){ rt_s(&S153), rt_text(v149_h), rt_s(&S154) }); rt_chk(w2_fail(t196, t197), "bracketNumeral"); });
  }
  wv t198 = rt_add(v148_close, rt_i(2));
  (void)rt_def(up->v82_i, "i");
  up->v82_i = t198;
  if (rt_cond(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, rt_def(up->v82_i, "i")), "bracketNumeral")), "bracketNumeral"))) {
    (void)rt_chk(w2_fail(v143_start, rt_s(&S155)), "bracketNumeral");
  }
  wv v150_lit = rt_bind(rt_chk(w3_bracketLiteral(({ wv t199 = rt_add(v144_open, rt_i(1)); wv t200 = v148_close; rt_chk(s87_cut(up, t199, t200), "bracketNumeral"); })), "bracketNumeral"), 0);
  if (rt_cond(rt_ne(rt_get(&F_error, v150_lit), RT_NULL))) {
    (void)({ wv t201 = rt_add(v144_open, rt_i(1)); wv t202 = rt_get(&F_error, v150_lit); rt_chk(w2_failHint(t201, t202, rt_s(&S156)), "bracketNumeral"); });
  }
  RT_RET(({ wv t205 = v143_start; wv t206 = v145_sp; wv t207 = v147_upper; wv t208 = rt_get(&F_kind, v150_lit); wv t209 = ({ wv t203 = v143_start; wv t204 = rt_def(up->v82_i, "i"); rt_chk(s87_cut(up, t203, t204), "bracketNumeral"); }); rt_chk(s96_numeral(up, t205, t206, t207, t208, t209), "bracketNumeral"); }));
}

static wv s96_numeral(struct fr29 *up, wv v151_start, wv v152_sp, wv v153_upper, wv v154_kind, wv v155_spelling) {
  rt_enter("numeral");
  v151_start = rt_bind(v151_start, 0);
  v152_sp = rt_bind(v152_sp, 0);
  v153_upper = rt_bind(v153_upper, 0);
  v154_kind = rt_bind(v154_kind, 0);
  v155_spelling = rt_bind(v155_spelling, 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_chk(s86_ch(up, rt_def(up->v82_i, "i")), "numeral"), rt_s(&S87)), "&&") && rt_bool(rt_chk(w1_isSigTerm(rt_chk(s86_ch(up, rt_add(rt_def(up->v82_i, "i"), rt_i(1))), "numeral")), "numeral"), "&&")))) {
    if (rt_cond(rt_not(v153_upper))) {
      (void)({ wv t210 = v151_start; wv t211 = rt_cat(3, (wv[]){ rt_s(&S157), rt_text(v155_spelling), rt_s(&S158) }); wv t212 = rt_cat(3, (wv[]){ rt_s(&S159), rt_text(rt_chk(w1_toUpper(v155_spelling), "numeral")), rt_s(&S160) }); rt_chk(w2_failHint(t210, t211, t212), "numeral"); });
    }
    if (rt_cond(rt_b(rt_bool(rt_ne(v154_kind, rt_s(&S106)), "||") || rt_bool(rt_gt(rt_chk(std_toInt(v155_spelling), "numeral"), rt_i(-1)), "||")))) {
      (void)rt_chk(w2_fail(v151_start, rt_s(&S161)), "numeral");
    }
    up->v82_i = rt_add(rt_def(up->v82_i, "i"), rt_i(1));
    RT_RET(({ wv t213 = v151_start; wv t214 = v152_sp; wv t215 = rt_sub(rt_i(0), rt_chk(std_toInt(v155_spelling), "numeral")); wv t216 = rt_add(v155_spelling, rt_s(&S87)); rt_chk(s90_tok(up, rt_s(&S162), t213, t214, t215, t216, RT_NULL), "numeral"); }));
  }
  if (rt_cond(rt_eq(rt_chk(s86_ch(up, rt_def(up->v82_i, "i")), "numeral"), rt_s(&S116)))) {
    (void)rt_chk(w2_fail(rt_def(up->v82_i, "i"), rt_s(&S163)), "numeral");
  }
  wv v156_v = rt_bind((rt_cond(rt_eq(v154_kind, rt_s(&S106))) ? rt_chk(std_toInt(v155_spelling), "numeral") : rt_chk(std_toFloat(v155_spelling), "numeral")), 0);
  RT_RET(rt_chk(s90_tok(up, v154_kind, v151_start, v152_sp, v156_v, v155_spelling, RT_NULL), "numeral"));
}

static wv s97_sigil(struct fr29 *up, wv v157_start, wv v158_sp) {
  rt_enter("sigil");
  v157_start = rt_bind(v157_start, 0);
  v158_sp = rt_bind(v158_sp, 0);
  if (rt_cond(rt_not(rt_chk(w1_isIdStart(rt_chk(s86_ch(up, rt_add(v157_start, rt_i(1))), "sigil")), "sigil")))) {
    (void)rt_chk(w2_failHint(v157_start, rt_s(&S163), rt_s(&S148)), "sigil");
  }
  wv v159_j = rt_add(v157_start, rt_i(1));
  while (rt_cond(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, v159_j), "sigil")), "sigil"))) {
    v159_j = rt_add(v159_j, rt_i(1));
  }
  wv t217 = rt_bind(v159_j, 1);
  (void)rt_def(up->v82_i, "i");
  up->v82_i = t217;
  RT_RET(({ wv t220 = v157_start; wv t221 = v158_sp; wv t222 = ({ wv t218 = rt_add(v157_start, rt_i(1)); wv t219 = v159_j; rt_chk(s87_cut(up, t218, t219), "sigil"); }); rt_chk(s90_tok(up, rt_s(&S164), t220, t221, t222, RT_NULL, RT_NULL), "sigil"); }));
}

static wv s98_string(struct fr29 *up, wv v160_start, wv v161_sp) {
  rt_enter("string");
  v160_start = rt_bind(v160_start, 0);
  v161_sp = rt_bind(v161_sp, 0);
  wv v162_parts = rt_arr(0, NULL, 1);
  wv v163_buf = rt_s(&S13);
  wv v164_rawDollar = rt_i(-1);
  wv v165_k = rt_add(v160_start, rt_i(1));
  while (rt_cond(rt_b(1))) {
    wv v166_c = rt_bind(rt_chk(s86_ch(up, v165_k), "string"), 0);
    if (rt_cond(rt_eq(v166_c, rt_s(&S13)))) {
      (void)rt_chk(w2_fail(v160_start, rt_s(&S165)), "string");
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(v166_c, rt_s(&S2)), "||") || rt_bool(rt_eq(v166_c, rt_s(&S3)), "||")))) {
      (void)rt_chk(w2_failHint(v165_k, rt_s(&S166), rt_s(&S167)), "string");
    }
    if (rt_cond(rt_eq(v166_c, rt_s(&S18)))) {
      v165_k = rt_add(v165_k, rt_i(1));
      break;
    }
    if (rt_cond(rt_eq(v166_c, rt_s(&S19)))) {
      wv v167_e = rt_bind(rt_chk(s86_ch(up, rt_add(v165_k, rt_i(1))), "string"), 0);
      wv v168_simple = rt_bind(rt_chk(w3_simpleEscape(v167_e), "string"), 0);
      if (rt_cond(rt_ne(v168_simple, RT_NULL))) {
        v163_buf = rt_add(v163_buf, v168_simple);
        v165_k = rt_add(v165_k, rt_i(2));
        continue;
      }
      if (rt_cond(rt_b(rt_bool(rt_eq(v167_e, rt_s(&S168)), "&&") && rt_bool(rt_eq(rt_chk(s86_ch(up, rt_add(v165_k, rt_i(2))), "string"), rt_s(&S93)), "&&")))) {
        wv v169_close = rt_bind(rt_chk(s89_find(up, rt_s(&S15), rt_add(v165_k, rt_i(3))), "string"), 0);
        wv v170_hex = rt_bind((rt_cond(rt_lt(v169_close, rt_i(0))) ? rt_s(&S13) : ({ wv t223 = rt_add(v165_k, rt_i(3)); wv t224 = v169_close; rt_chk(s87_cut(up, t223, t224), "string"); })), 0);
        wv v171_cp = rt_bind(rt_chk(w1_hexValue(v170_hex), "string"), 0);
        if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_lt(v171_cp, rt_i(0)), "||") || rt_bool(rt_gt(v171_cp, rt_i(1114111)), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ge(v171_cp, rt_i(55296)), "&&") && rt_bool(rt_le(v171_cp, rt_i(57343)), "&&")), "||")))) {
          (void)rt_chk(w2_failHint(v165_k, rt_s(&S169), rt_s(&S170)), "string");
        }
        v163_buf = ({ wv t225 = v163_buf; wv t226 = std_fromCode(v171_cp); rt_add(t225, t226); });
        v165_k = rt_add(v169_close, rt_i(1));
        continue;
      }
      (void)({ wv t227 = v165_k; wv t228 = rt_cat(3, (wv[]){ rt_s(&S171), rt_text(v167_e), rt_s(&S111) }); rt_chk(w2_failHint(t227, t228, rt_s(&S172)), "string"); });
    }
    if (rt_cond(rt_eq(v166_c, rt_s(&S93)))) {
      if (rt_cond(rt_eq(rt_chk(s86_ch(up, rt_add(v165_k, rt_i(1))), "string"), rt_s(&S93)))) {
        v163_buf = rt_add(v163_buf, rt_s(&S93));
        v165_k = rt_add(v165_k, rt_i(2));
        continue;
      }
      if (rt_cond(({ wv t229 = v164_rawDollar; wv t230 = rt_sub(v165_k, rt_i(1)); rt_eq(t229, t230); }))) {
        (void)({ wv t231 = w2_warnings; wv t232 = rt_chk(w2_warn(v164_rawDollar, rt_s(&S173), RT_NULL), "string"); std_push(t231, t232); });
      }
      (void)std_push(v162_parts, v163_buf);
      v163_buf = rt_s(&S13);
      wv v172_sub = rt_bind(rt_chk(s99_interpolation(up, v165_k), "string"), 0);
      (void)({ wv t235 = v162_parts; wv t236 = ({ wv t233 = rt_get(&F_tokens, v172_sub); wv t234 = v165_k; rt_rec(0, 2, K4, (wv[]){ t233, t234 }); }); std_push(t235, t236); });
      v165_k = rt_bind(rt_get(&F_end, v172_sub), 1);
      continue;
    }
    if (rt_cond(rt_eq(v166_c, rt_s(&S15)))) {
      if (rt_cond(rt_eq(rt_chk(s86_ch(up, rt_add(v165_k, rt_i(1))), "string"), rt_s(&S15)))) {
        v163_buf = rt_add(v163_buf, rt_s(&S15));
        v165_k = rt_add(v165_k, rt_i(2));
        continue;
      }
      (void)rt_chk(w2_failHint(v165_k, rt_s(&S174), rt_s(&S175)), "string");
    }
    if (rt_cond(rt_eq(v166_c, rt_s(&S116)))) {
      v164_rawDollar = rt_bind(v165_k, 1);
      if (rt_cond(rt_chk(w1_isIdStart(rt_chk(s86_ch(up, rt_add(v165_k, rt_i(1))), "string")), "string"))) {
        wv v173_j = rt_add(v165_k, rt_i(1));
        while (rt_cond(rt_chk(w1_isIdChar(rt_chk(s86_ch(up, v173_j), "string")), "string"))) {
          v173_j = rt_add(v173_j, rt_i(1));
        }
        wv v174_name = rt_bind(({ wv t237 = rt_add(v165_k, rt_i(1)); wv t238 = v173_j; rt_chk(s87_cut(up, t237, t238), "string"); }), 0);
        (void)({ wv t241 = w2_warnings; wv t242 = ({ wv t239 = v165_k; wv t240 = rt_cat(3, (wv[]){ rt_s(&S176), rt_text(v174_name), rt_s(&S177) }); rt_chk(w2_warn(t239, t240, rt_s(&S178)), "string"); }); std_push(t241, t242); });
      }
    }
    v163_buf = rt_add(v163_buf, v166_c);
    v165_k = rt_add(v165_k, rt_i(1));
  }
  (void)std_push(v162_parts, v163_buf);
  wv t243 = rt_bind(v165_k, 1);
  (void)rt_def(up->v82_i, "i");
  up->v82_i = t243;
  RT_RET(rt_chk(s90_tok(up, rt_s(&S179), v160_start, v161_sp, RT_NULL, RT_NULL, v162_parts), "string"));
}

static wv s99_interpolation(struct fr29 *up, wv v175_open) {
  rt_enter("interpolation");
  v175_open = rt_bind(v175_open, 0);
  wv v176_saveI = rt_bind(rt_def(up->v82_i, "i"), 0);
  wv v177_savePrev = rt_bind(rt_def(up->v83_prevT, "prevT"), 0);
  wv v178_saveNl = rt_bind(rt_def(up->v84_nlNow, "nlNow"), 0);
  wv t244 = rt_add(v175_open, rt_i(1));
  (void)rt_def(up->v82_i, "i");
  up->v82_i = t244;
  wv t245 = rt_s(&S13);
  (void)rt_def(up->v83_prevT, "prevT");
  up->v83_prevT = t245;
  wv v179_sub = rt_arr(0, NULL, 1);
  wv v180_depth = rt_i(0);
  while (rt_cond(rt_b(1))) {
    wv v182_tk = rt_bind(rt_chk(s93_next(up), "interpolation"), 0);
    if (rt_cond(rt_eq(rt_get(&F_t, v182_tk), rt_s(&S121)))) {
      (void)rt_chk(w2_fail(v175_open, rt_s(&S180)), "interpolation");
    }
    if (rt_cond(rt_eq(rt_get(&F_t, v182_tk), rt_s(&S15)))) {
      if (rt_cond(rt_eq(v180_depth, rt_i(0)))) {
        (void)({ wv t250 = v179_sub; wv t251 = ({ wv t246 = rt_get(&F_pos, v182_tk); wv t247 = rt_get(&F_end, v182_tk); wv t248 = rt_get(&F_sp, v182_tk); wv t249 = rt_get(&F_nl, v182_tk); rt_rec(0, 8, K3, (wv[]){ rt_s(&S121), t246, t247, t248, t249, RT_NULL, RT_NULL, RT_NULL }); }); std_push(t250, t251); });
        break;
      }
      v180_depth = rt_sub(v180_depth, rt_i(1));
    } else if (rt_cond(rt_eq(rt_get(&F_t, v182_tk), rt_s(&S93)))) {
      v180_depth = rt_add(v180_depth, rt_i(1));
    }
    (void)std_push(v179_sub, v182_tk);
  }
  wv v181_endI = rt_bind(rt_def(up->v82_i, "i"), 0);
  wv t252 = rt_bind(v176_saveI, 1);
  (void)rt_def(up->v82_i, "i");
  up->v82_i = t252;
  wv t253 = rt_bind(v177_savePrev, 1);
  (void)rt_def(up->v83_prevT, "prevT");
  up->v83_prevT = t253;
  wv t254 = rt_bind(v178_saveNl, 1);
  (void)rt_def(up->v84_nlNow, "nlNow");
  up->v84_nlNow = t254;
  if (rt_cond(rt_eq(std_length(v179_sub), rt_i(1)))) {
    (void)rt_chk(w2_failHint(v175_open, rt_s(&S181), rt_s(&S182)), "interpolation");
  }
  RT_RET(rt_rec(0, 2, K5, (wv[]){ v179_sub, v181_endI }));
}

static wv w4_precedence(wv v184_t) {
  rt_enter("precedence");
  v184_t = rt_bind(v184_t, 0);
  if (rt_cond(rt_eq(v184_t, rt_s(&S83)))) {
    RT_RET(rt_i(1));
  }
  if (rt_cond(rt_eq(v184_t, rt_s(&S82)))) {
    RT_RET(rt_i(2));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v184_t, rt_s(&S78)), "||") || rt_bool(rt_eq(v184_t, rt_s(&S79)), "||")))) {
    RT_RET(rt_i(3));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v184_t, rt_s(&S84)), "||") || rt_bool(rt_eq(v184_t, rt_s(&S85)), "||")), "||") || rt_bool(rt_eq(v184_t, rt_s(&S80)), "||")), "||") || rt_bool(rt_eq(v184_t, rt_s(&S81)), "||")))) {
    RT_RET(rt_i(4));
  }
  if (rt_cond(rt_eq(v184_t, rt_s(&S77)))) {
    RT_RET(rt_i(5));
  }
  if (rt_cond(rt_eq(v184_t, rt_s(&S32)))) {
    RT_RET(rt_i(6));
  }
  if (rt_cond(rt_eq(v184_t, rt_s(&S76)))) {
    RT_RET(rt_i(7));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v184_t, rt_s(&S67)), "||") || rt_bool(rt_eq(v184_t, rt_s(&S68)), "||")))) {
    RT_RET(rt_i(8));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v184_t, rt_s(&S71)), "||") || rt_bool(rt_eq(v184_t, rt_s(&S72)), "||")))) {
    RT_RET(rt_i(9));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v184_t, rt_s(&S73)), "||") || rt_bool(rt_eq(v184_t, rt_s(&S74)), "||")), "||") || rt_bool(rt_eq(v184_t, rt_s(&S75)), "||")))) {
    RT_RET(rt_i(10));
  }
  RT_RET(rt_i(0));
}

static wv w4_isIdent(wv v185_s) {
  rt_enter("isIdent");
  v185_s = rt_bind(v185_s, 0);
  wv v186_cs = rt_bind(std_split(v185_s, rt_s(&S13)), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v186_cs), rt_i(0)), "||") || rt_bool(rt_not(rt_chk(w1_isIdStart(rt_idx(rt_i(0), v186_cs)), "isIdent")), "||")))) {
    RT_RET(rt_b(0));
  }
  wv v187_k = rt_i(1);
  while (rt_cond(({ wv t255 = v187_k; wv t256 = std_length(v186_cs); rt_lt(t255, t256); }))) {
    if (rt_cond(rt_not(rt_chk(w1_isIdChar(rt_idx(v187_k, v186_cs)), "isIdent")))) {
      RT_RET(rt_b(0));
    }
    v187_k = rt_add(v187_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv w4_describe(wv v188_tk) {
  rt_enter("describe");
  v188_tk = rt_bind(v188_tk, 0);
  if (rt_cond(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S121)))) {
    RT_RET(rt_s(&S187));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S149)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_v, v188_tk)), rt_s(&S111) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S164)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S188), rt_text(rt_get(&F_v, v188_tk)), rt_s(&S111) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S106)), "||") || rt_bool(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S109)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S162)), "||")))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_text, v188_tk)), rt_s(&S111) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S179)))) {
    RT_RET(rt_s(&S189));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v188_tk), rt_s(&S124)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_v, v188_tk)), rt_s(&S190) }));
  }
  RT_RET(rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_t, v188_tk)), rt_s(&S111) }));
}

static wv w4_failAtHint(wv v189_tk, wv v190_message, wv v191_hint) {
  rt_enter("failAtHint");
  v189_tk = rt_bind(v189_tk, 0);
  v190_message = rt_bind(v190_message, 0);
  v191_hint = rt_bind(v191_hint, 0);
  (void)({ wv t257 = rt_get(&F_pos, v189_tk); wv t258 = v190_message; wv t259 = v191_hint; rt_chk(w2_failHint(t257, t258, t259), "failAtHint"); });
  RT_RET(RT_NULL);
}

static wv w4_failAt(wv v192_tk, wv v193_message) {
  rt_enter("failAt");
  v192_tk = rt_bind(v192_tk, 0);
  v193_message = rt_bind(v193_message, 0);
  (void)({ wv t260 = rt_get(&F_pos, v192_tk); wv t261 = v193_message; rt_chk(w2_failHint(t260, t261, RT_NULL), "failAt"); });
  RT_RET(RT_NULL);
}

static wv w4_simpleStatement(wv v194_k, wv v195_e) {
  rt_enter("simpleStatement");
  v194_k = rt_bind(v194_k, 0);
  v195_e = rt_bind(v195_e, 0);
  RT_RET(rt_rec(0, 5, K6, (wv[]){ v194_k, v195_e, RT_NULL, RT_NULL, RT_NULL }));
}

static wv w4_parse(wv v196_toks) {
  rt_enter("parse");
  RT_RET(rt_chk(w4_parser(v196_toks, rt_b(0)), "parse"));
}

static wv w4_hasEffect(wv v197_e) {
  rt_enter("hasEffect");
  v197_e = rt_bind(v197_e, 0);
  wv v198_kind = rt_bind(rt_get(&F_k, v197_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v198_kind, rt_s(&S191)), "||") || rt_bool(rt_eq(v198_kind, rt_s(&S192)), "||")), "||") || rt_bool(rt_eq(v198_kind, rt_s(&S193)), "||")), "||") || rt_bool(rt_eq(v198_kind, rt_s(&S51)), "||")))) {
    RT_RET(rt_b(1));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S179)))) {
    wv v199_i = rt_i(0);
    while (rt_cond(({ wv t262 = v199_i; wv t263 = std_length(rt_get(&F_parts, v197_e)); rt_lt(t262, t263); }))) {
      if (rt_cond(rt_b(rt_bool(rt_ne(std_typeOf(({ wv t264 = v199_i; wv t265 = rt_get(&F_parts, v197_e); rt_idx(t264, t265); })), rt_s(&S194)), "&&") && rt_bool(rt_chk(w4_hasEffect(({ wv t266 = v199_i; wv t267 = rt_get(&F_parts, v197_e); rt_idx(t266, t267); })), "hasEffect"), "&&")))) {
        RT_RET(rt_b(1));
      }
      v199_i = rt_add(v199_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S195)))) {
    wv v200_i = rt_i(0);
    while (rt_cond(({ wv t268 = v200_i; wv t269 = std_length(rt_get(&F_items, v197_e)); rt_lt(t268, t269); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(({ wv t270 = v200_i; wv t271 = rt_get(&F_items, v197_e); rt_idx(t270, t271); })), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v200_i = rt_add(v200_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S196)))) {
    wv v201_i = rt_i(0);
    while (rt_cond(({ wv t272 = v201_i; wv t273 = std_length(rt_get(&F_fields, v197_e)); rt_lt(t272, t273); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(rt_get(&F_e, ({ wv t274 = v201_i; wv t275 = rt_get(&F_fields, v197_e); rt_idx(t274, t275); }))), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v201_i = rt_add(v201_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S197)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_index, v197_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_arr, v197_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S198)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_rec, v197_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S199)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_e, v197_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S200)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_l, v197_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_r, v197_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v198_kind, rt_s(&S201)))) {
    RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_c, v197_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_a, v197_e)), "hasEffect"), "||")), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_b, v197_e)), "hasEffect"), "||")));
  }
  RT_RET(rt_b(0));
}

static wv w4_parser(wv p_v202_toks, wv v203_inner) {
  rt_enter("parser");
  struct fr53 fr = { 0 };
  fr.v202_toks = p_v202_toks;
  v203_inner = rt_bind(v203_inner, 0);
  fr.v204_p = rt_i(0);
  fr.v205_depth = rt_i(0);
  if (rt_cond(v203_inner)) {
    wv v369_e = rt_bind(rt_chk(s225_expr(&fr), "parser"), 0);
    if (rt_cond(rt_not(rt_chk(s207_at(&fr, rt_s(&S121)), "parser")))) {
      wv v370_tk = rt_bind(rt_chk(s206_peek(&fr), "parser"), 0);
      (void)({ wv t541 = v370_tk; wv t542 = rt_cat(3, (wv[]){ rt_s(&S327), rt_text(rt_chk(w4_describe(v370_tk), "parser")), rt_s(&S328) }); rt_chk(w4_failAt(t541, t542), "parser"); });
    }
    RT_RET(v369_e);
  }
  RT_RET(rt_chk(s213_program(&fr), "parser"));
}

static wv s206_peek(struct fr53 *up) {
  rt_enter("peek");
  RT_RET(({ wv t276 = rt_def(up->v204_p, "p"); wv t277 = up->v202_toks; rt_idx(t276, t277); }));
}

static wv s207_at(struct fr53 *up, wv v243_type) {
  rt_enter("at");
  v243_type = rt_bind(v243_type, 0);
  RT_RET(({ wv t280 = rt_get(&F_t, ({ wv t278 = rt_def(up->v204_p, "p"); wv t279 = up->v202_toks; rt_idx(t278, t279); })); wv t281 = v243_type; rt_eq(t280, t281); }));
}

static wv s208_atCall(struct fr53 *up) {
  rt_enter("atCall");
  if (rt_cond(rt_b(rt_bool(({ wv t282 = rt_add(rt_def(up->v204_p, "p"), rt_i(2)); wv t283 = std_length(up->v202_toks); rt_ge(t282, t283); }), "||") || rt_bool(rt_ne(rt_get(&F_t, ({ wv t284 = rt_def(up->v204_p, "p"); wv t285 = up->v202_toks; rt_idx(t284, t285); })), rt_s(&S90)), "||")))) {
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t286 = rt_add(rt_def(up->v204_p, "p"), rt_i(2)); wv t287 = up->v202_toks; rt_idx(t286, t287); })), rt_s(&S16)), "&&") && rt_bool(({ wv t290 = rt_def(w4_callees, "callees"); wv t291 = rt_get(&F_t, ({ wv t288 = rt_add(rt_def(up->v204_p, "p"), rt_i(1)); wv t289 = up->v202_toks; rt_idx(t288, t289); })); rt_chk(w1_contains(t290, t291), "atCall"); }), "&&")))) {
    RT_RET(rt_b(1));
  }
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(({ wv t292 = rt_add(rt_def(up->v204_p, "p"), rt_i(4)); wv t293 = std_length(up->v202_toks); rt_lt(t292, t293); }), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t294 = rt_add(rt_def(up->v204_p, "p"), rt_i(1)); wv t295 = up->v202_toks; rt_idx(t294, t295); })), rt_s(&S149)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t296 = rt_add(rt_def(up->v204_p, "p"), rt_i(2)); wv t297 = up->v202_toks; rt_idx(t296, t297); })), rt_s(&S107)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t298 = rt_add(rt_def(up->v204_p, "p"), rt_i(3)); wv t299 = up->v202_toks; rt_idx(t298, t299); })), rt_s(&S149)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t300 = rt_add(rt_def(up->v204_p, "p"), rt_i(4)); wv t301 = up->v202_toks; rt_idx(t300, t301); })), rt_s(&S16)), "&&")));
}

static wv s209_next(struct fr53 *up) {
  rt_enter("next");
  wv v244_tk = rt_bind(({ wv t302 = rt_def(up->v204_p, "p"); wv t303 = up->v202_toks; rt_idx(t302, t303); }), 0);
  if (rt_cond(rt_ne(rt_get(&F_t, v244_tk), rt_s(&S121)))) {
    up->v204_p = rt_add(rt_def(up->v204_p, "p"), rt_i(1));
  }
  RT_RET(v244_tk);
}

static wv s210_accept(struct fr53 *up, wv v245_type) {
  rt_enter("accept");
  v245_type = rt_bind(v245_type, 0);
  RT_RET((rt_cond(rt_chk(s207_at(up, v245_type), "accept")) ? rt_chk(s209_next(up), "accept") : RT_NULL));
}

static wv s211_expect(struct fr53 *up, wv v246_type, wv v247_msg, wv v248_hint) {
  rt_enter("expect");
  v246_type = rt_bind(v246_type, 0);
  v247_msg = rt_bind(v247_msg, 0);
  v248_hint = rt_bind(v248_hint, 0);
  if (rt_cond(rt_chk(s207_at(up, v246_type), "expect"))) {
    RT_RET(rt_chk(s209_next(up), "expect"));
  }
  wv v249_found = rt_bind(rt_chk(s206_peek(up), "expect"), 0);
  wv v250_m = rt_bind((rt_cond(rt_eq(v247_msg, RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S153), rt_text(v246_type), rt_s(&S111) }) : v247_msg), 0);
  (void)({ wv t306 = v249_found; wv t307 = ({ wv t304 = rt_text(v250_m); wv t305 = rt_text(rt_chk(w4_describe(v249_found), "expect")); rt_cat(3, (wv[]){ t304, rt_s(&S202), t305 }); }); wv t308 = v248_hint; rt_chk(w4_failAtHint(t306, t307, t308), "expect"); });
  RT_RET(RT_NULL);
}

static wv s212_lintSpace(struct fr53 *up, wv v251_op) {
  rt_enter("lintSpace");
  v251_op = rt_bind(v251_op, 0);
  if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_sp, v251_op)), "||") || rt_bool(rt_not(rt_get(&F_sp, rt_chk(s206_peek(up), "lintSpace"))), "||")))) {
    (void)({ wv t311 = w2_warnings; wv t312 = ({ wv t309 = rt_get(&F_pos, v251_op); wv t310 = rt_cat(3, (wv[]){ rt_s(&S203), rt_text(rt_get(&F_t, v251_op)), rt_s(&S111) }); rt_chk(w2_warn(t309, t310, RT_NULL), "lintSpace"); }); std_push(t311, t312); });
  }
  RT_RET(RT_NULL);
}

static wv s213_program(struct fr53 *up) {
  rt_enter("program");
  wv v252_stmts = rt_arr(0, NULL, 1);
  wv v253_started = rt_b(0);
  while (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S121)), "program")))) {
    if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S14)), "program")))) {
      wv v255_tk = rt_bind(rt_chk(s206_peek(up), "program"), 0);
      wv v256_hint = rt_bind((rt_cond(rt_chk(s207_at(up, rt_s(&S90)), "program")) ? rt_def(w4_postfixHint, "postfixHint") : rt_s(&S204)), 0);
      (void)({ wv t313 = v255_tk; wv t314 = rt_cat(2, (wv[]){ rt_s(&S205), rt_text(rt_chk(w4_describe(v255_tk), "program")) }); wv t315 = v256_hint; rt_chk(w4_failAtHint(t313, t314, t315), "program"); });
    }
    wv v254_s = rt_bind(rt_chk(s214_statement(up), "program"), 0);
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v254_s), rt_s(&S54)), "&&") && rt_bool(v253_started, "&&")))) {
      (void)rt_chk(w2_fail(rt_get(&F_pos, v254_s), rt_s(&S206)), "program");
    }
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, v254_s), rt_s(&S54)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v254_s), rt_s(&S207)), "&&")))) {
      v253_started = rt_b(1);
    }
    (void)std_push(v252_stmts, v254_s);
  }
  RT_RET(({ wv t316 = v252_stmts; wv t317 = rt_get(&F_pos, rt_chk(s206_peek(up), "program")); rt_rec(0, 2, K7, (wv[]){ t316, t317 }); }));
}

static wv s214_statement(struct fr53 *up) {
  rt_enter("statement");
  wv v257_semi = rt_bind(rt_chk(s209_next(up), "statement"), 0);
  wv v258_tk = rt_bind(rt_chk(s206_peek(up), "statement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v258_tk), rt_s(&S14)), "||") || rt_bool(rt_eq(rt_get(&F_t, v258_tk), rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v258_tk), rt_s(&S121)), "||")))) {
    RT_RET(({ wv t318 = rt_get(&F_pos, v257_semi); wv t319 = rt_get(&F_pos, v258_tk); rt_rec(0, 4, K8, (wv[]){ rt_s(&S207), t318, t319, RT_NULL }); }));
  }
  wv v259_s = rt_bind(rt_chk(s215_statementBody(up, v258_tk), "statement"), 1);
  ({ wv t320 = rt_get(&F_pos, v258_tk); wv t321 = v259_s; rt_set_mem(t320, &F_pos, t321); });
  if (rt_cond(rt_chk(s207_at(up, rt_s(&S45)), "statement"))) {
    wv v260_w = rt_bind(rt_chk(s209_next(up), "statement"), 0);
    if (rt_cond(rt_not(({ wv t322 = rt_def(w4_whineOk, "whineOk"); wv t323 = rt_get(&F_k, v259_s); rt_chk(w1_contains(t322, t323), "statement"); })))) {
      (void)rt_chk(w4_failAt(v260_w, rt_s(&S208)), "statement");
    }
    wv v261_param = RT_NULL;
    if (rt_cond(rt_ne(rt_chk(s210_accept(up, rt_s(&S90)), "statement"), RT_NULL))) {
      v261_param = rt_bind(rt_chk(s211_expect(up, rt_s(&S149), rt_s(&S209), RT_NULL), "statement"), 1);
      (void)rt_chk(s211_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "statement");
    }
    wv v262_noParam = rt_eq(v261_param, RT_NULL);
    ({ wv t328 = ({ wv t324 = rt_get(&F_pos, v260_w); wv t325 = (rt_cond(v262_noParam) ? RT_NULL : rt_get(&F_v, v261_param)); wv t326 = (rt_cond(v262_noParam) ? RT_NULL : rt_get(&F_pos, v261_param)); wv t327 = rt_chk(s223_block(up), "statement"); rt_rec(0, 4, K9, (wv[]){ t324, t325, t326, t327 }); }); wv t329 = v259_s; rt_set_mem(t328, &F_handler, t329); });
  }
  ({ wv t330 = rt_get(&F_pos, rt_chk(s206_peek(up), "statement")); wv t331 = v259_s; rt_set_mem(t330, &F_end, t331); });
  RT_RET(v259_s);
}

static wv s215_statementBody(struct fr53 *up, wv v263_tk) {
  rt_enter("statementBody");
  v263_tk = rt_bind(v263_tk, 0);
  wv v264_kind = rt_bind(rt_get(&F_t, v263_tk), 0);
  if (rt_cond(rt_eq(v264_kind, rt_s(&S33)))) {
    RT_RET(rt_chk(s220_schemeStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S53)))) {
    RT_RET(rt_chk(s221_lineupStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S34)))) {
    (void)rt_chk(s209_next(up), "statementBody");
    wv v266_e = rt_bind((rt_cond(({ wv t332 = rt_def(w4_exprStart, "exprStart"); wv t333 = rt_get(&F_t, rt_chk(s206_peek(up), "statementBody")); rt_chk(w1_contains(t332, t333), "statementBody"); })) ? rt_chk(s225_expr(up), "statementBody") : RT_NULL), 0);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S34), v266_e), "statementBody"));
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S44)))) {
    (void)rt_chk(s209_next(up), "statementBody");
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S44), rt_chk(s225_expr(up), "statementBody")), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v264_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v264_kind, rt_s(&S43)), "||")))) {
    (void)rt_chk(s209_next(up), "statementBody");
    RT_RET(rt_rec(0, 4, K8, (wv[]){ v264_kind, RT_NULL, RT_NULL, RT_NULL }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v264_kind, rt_s(&S41)), "||") || rt_bool(rt_eq(v264_kind, rt_s(&S210)), "||")))) {
    (void)rt_chk(s209_next(up), "statementBody");
    RT_RET(({ wv t334 = rt_eq(v264_kind, rt_s(&S210)); wv t335 = rt_chk(s222_cond(up, v264_kind), "statementBody"); wv t336 = rt_chk(s223_block(up), "statementBody"); rt_rec(0, 7, K10, (wv[]){ rt_s(&S41), t334, t335, t336, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S162)))) {
    (void)rt_chk(s209_next(up), "statementBody");
    RT_RET(rt_rec(0, 5, K11, (wv[]){ rt_s(&S211), rt_get(&F_v, v263_tk), RT_NULL, RT_NULL, RT_NULL }));
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S212)))) {
    (void)rt_chk(s209_next(up), "statementBody");
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S185), RT_NULL), "statementBody"));
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S124)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_v, v263_tk), rt_s(&S69)), "||") || rt_bool(rt_eq(rt_get(&F_v, v263_tk), rt_s(&S70)), "||")))) {
      (void)rt_chk(s209_next(up), "statementBody");
      RT_RET(({ wv t337 = std_at(rt_get(&F_v, v263_tk), rt_i(0)); wv t338 = rt_rec(0, 4, K13, (wv[]){ rt_s(&S106), rt_i(1), rt_get(&F_pos, v263_tk), rt_b(0) }); wv t339 = rt_chk(s224_lvalue(up), "statementBody"); rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t337, t338, t339, RT_NULL, RT_NULL, RT_NULL }); }));
    }
    (void)({ wv t340 = v263_tk; wv t341 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_v, v263_tk)), rt_s(&S213) }); wv t342 = rt_cat(3, (wv[]){ rt_s(&S214), rt_text(rt_get(&F_v, v263_tk)), rt_s(&S215) }); rt_chk(w4_failAtHint(t340, t341, t342), "statementBody"); });
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S45)))) {
    (void)rt_chk(w4_failAt(v263_tk, rt_s(&S208)), "statementBody");
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S40)))) {
    (void)rt_chk(w4_failAt(v263_tk, rt_s(&S216)), "statementBody");
  }
  if (rt_cond(rt_eq(v264_kind, rt_s(&S55)))) {
    (void)rt_chk(w4_failAtHint(v263_tk, rt_s(&S217), rt_s(&S218)), "statementBody");
  }
  if (rt_cond(rt_chk(s218_atBribe(up), "statementBody"))) {
    RT_RET(rt_chk(s219_bribeStatement(up), "statementBody"));
  }
  wv v265_s = rt_bind(rt_chk(s216_exprStatement(up), "statementBody"), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v265_s), rt_s(&S184)), "&&") && rt_bool(rt_not(rt_chk(w4_hasEffect(rt_get(&F_e, v265_s)), "statementBody")), "&&")))) {
    wv v267_hint = rt_bind((rt_cond(rt_gt(rt_def(up->v205_depth, "depth"), rt_i(0))) ? rt_s(&S219) : RT_NULL), 0);
    (void)({ wv t345 = w2_warnings; wv t346 = ({ wv t343 = rt_get(&F_pos, v263_tk); wv t344 = v267_hint; rt_chk(w2_warn(t343, rt_s(&S220), t344), "statementBody"); }); std_push(t345, t346); });
  }
  RT_RET(v265_s);
}

static wv s216_exprStatement(struct fr53 *up) {
  rt_enter("exprStatement");
  wv v268_e = rt_bind(rt_chk(s225_expr(up), "exprStatement"), 0);
  wv v269_tk = rt_bind(rt_chk(s206_peek(up), "exprStatement"), 0);
  if (rt_cond(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S35)))) {
    (void)rt_chk(s209_next(up), "exprStatement");
    wv v270_isShifty = rt_ne(rt_chk(s210_accept(up, rt_s(&S36)), "exprStatement"), RT_NULL);
    wv v271_isFickle = rt_b(rt_bool(rt_not(v270_isShifty), "&&") && rt_bool(rt_ne(rt_chk(s210_accept(up, rt_s(&S37)), "exprStatement"), RT_NULL), "&&"));
    wv v272_name = rt_bind(rt_chk(s211_expect(up, rt_s(&S149), rt_s(&S221), RT_NULL), "exprStatement"), 0);
    RT_RET(({ wv t347 = v268_e; wv t348 = v270_isShifty; wv t349 = v271_isFickle; wv t350 = rt_get(&F_v, v272_name); wv t351 = rt_get(&F_pos, v272_name); rt_rec(0, 9, K14, (wv[]){ rt_s(&S35), t347, t348, t349, t350, t351, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S38)))) {
    (void)rt_chk(s209_next(up), "exprStatement");
    RT_RET(({ wv t352 = v268_e; wv t353 = rt_chk(s224_lvalue(up), "exprStatement"); rt_rec(0, 7, K15, (wv[]){ rt_s(&S38), t352, RT_NULL, t353, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S124)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_v, v269_tk), rt_s(&S69)), "||") || rt_bool(rt_eq(rt_get(&F_v, v269_tk), rt_s(&S70)), "||")))) {
      (void)({ wv t354 = v269_tk; wv t355 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_v, v269_tk)), rt_s(&S222) }); wv t356 = rt_cat(3, (wv[]){ rt_s(&S223), rt_text(rt_get(&F_v, v269_tk)), rt_s(&S215) }); rt_chk(w4_failAtHint(t354, t355, t356), "exprStatement"); });
    }
    (void)rt_chk(s209_next(up), "exprStatement");
    RT_RET(({ wv t357 = v268_e; wv t358 = rt_get(&F_v, v269_tk); wv t359 = rt_chk(s224_lvalue(up), "exprStatement"); rt_rec(0, 7, K15, (wv[]){ rt_s(&S38), t357, t358, t359, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S212)))) {
    (void)rt_chk(s209_next(up), "exprStatement");
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S185), v268_e), "exprStatement"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S36)), "||") || rt_bool(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S37)), "||")))) {
    (void)({ wv t360 = v269_tk; wv t361 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_t, v269_tk)), rt_s(&S224) }); wv t362 = rt_cat(3, (wv[]){ rt_s(&S225), rt_text(rt_get(&F_t, v269_tk)), rt_s(&S226) }); rt_chk(w4_failAtHint(t360, t361, t362), "exprStatement"); });
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S54)))) {
    (void)rt_chk(s209_next(up), "exprStatement");
    RT_RET(rt_chk(s217_stealStatement(up, v269_tk, v268_e), "exprStatement"));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v269_tk), rt_s(&S55)))) {
    (void)rt_chk(w4_failAtHint(v269_tk, rt_s(&S227), rt_s(&S218)), "exprStatement");
  }
  RT_RET(rt_chk(w4_simpleStatement(rt_s(&S184), v268_e), "exprStatement"));
}

static wv s217_stealStatement(struct fr53 *up, wv v273_kw, wv v274_e) {
  rt_enter("stealStatement");
  v273_kw = rt_bind(v273_kw, 0);
  v274_e = rt_bind(v274_e, 0);
  if (rt_cond(rt_gt(rt_def(up->v205_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v273_kw, rt_s(&S228)), "stealStatement");
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_k, v274_e), rt_s(&S179)), "||") || rt_bool(rt_get(&F_paren, v274_e), "||")), "||") || rt_bool(rt_gt(std_length(rt_get(&F_parts, v274_e)), rt_i(1)), "||")), "||") || rt_bool(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v274_e)), rt_i(1)), "&&") && rt_bool(rt_ne(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v274_e))), rt_s(&S194)), "&&")), "||")))) {
    (void)rt_chk(w4_failAtHint(v274_e, rt_s(&S229), rt_s(&S230)), "stealStatement");
  }
  wv v275_path = rt_bind((rt_cond(rt_eq(std_length(rt_get(&F_parts, v274_e)), rt_i(0))) ? rt_s(&S13) : rt_idx(rt_i(0), rt_get(&F_parts, v274_e))), 0);
  if (rt_cond(rt_chk(w1_contains(std_split(v275_path, rt_s(&S13)), rt_s(&S19)), "stealStatement"))) {
    (void)rt_chk(w4_failAt(v274_e, rt_s(&S231)), "stealStatement");
  }
  if (rt_cond(rt_not(rt_b(rt_bool(rt_ge(std_length(v275_path), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t363 = v275_path; wv t364 = std_length(v275_path); std_slice(t363, rt_i(-4), t364); }), rt_s(&S232)), "&&"))))) {
    (void)rt_chk(w4_failAt(v274_e, rt_s(&S233)), "stealStatement");
  }
  wv v276_name = rt_bind(rt_chk(s211_expect(up, rt_s(&S149), rt_s(&S234), rt_s(&S235)), "stealStatement"), 0);
  RT_RET(({ wv t365 = v275_path; wv t366 = rt_get(&F_pos, v274_e); wv t367 = rt_get(&F_v, v276_name); wv t368 = rt_get(&F_pos, v276_name); rt_rec(0, 8, K16, (wv[]){ rt_s(&S54), t365, t366, t367, t368, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s218_atBribe(struct fr53 *up) {
  rt_enter("atBribe");
  wv v277_i = rt_bind(rt_def(up->v204_p, "p"), 1);
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_ne(rt_get(&F_t, rt_idx(v277_i, up->v202_toks)), rt_s(&S149)))) {
      RT_RET(rt_b(0));
    }
    if (rt_cond(rt_eq(rt_get(&F_t, ({ wv t369 = rt_add(v277_i, rt_i(1)); wv t370 = up->v202_toks; rt_idx(t369, t370); })), rt_s(&S55)))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_t, ({ wv t371 = rt_add(v277_i, rt_i(1)); wv t372 = up->v202_toks; rt_idx(t371, t372); })), rt_s(&S89)))) {
      RT_RET(rt_b(0));
    }
    v277_i = rt_add(v277_i, rt_i(2));
  }
  RT_RET(rt_b(0));
}

static wv s219_bribeStatement(struct fr53 *up) {
  rt_enter("bribeStatement");
  wv v278_names = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v280_n = rt_bind(rt_chk(s209_next(up), "bribeStatement"), 0);
    wv v281_k = rt_i(0);
    while (rt_cond(({ wv t373 = v281_k; wv t374 = std_length(v278_names); rt_lt(t373, t374); }))) {
      if (rt_cond(({ wv t375 = rt_get(&F_name, rt_idx(v281_k, v278_names)); wv t376 = rt_get(&F_v, v280_n); rt_eq(t375, t376); }))) {
        (void)({ wv t377 = v280_n; wv t378 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_v, v280_n)), rt_s(&S236) }); rt_chk(w4_failAt(t377, t378), "bribeStatement"); });
      }
      v281_k = rt_add(v281_k, rt_i(1));
    }
    (void)({ wv t381 = v278_names; wv t382 = ({ wv t379 = rt_get(&F_v, v280_n); wv t380 = rt_get(&F_pos, v280_n); rt_rec(0, 2, K17, (wv[]){ t379, t380 }); }); std_push(t381, t382); });
    if (rt_cond(rt_eq(rt_chk(s210_accept(up, rt_s(&S89)), "bribeStatement"), RT_NULL))) {
      break;
    }
  }
  wv v279_kw = rt_bind(rt_chk(s209_next(up), "bribeStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v205_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v279_kw, rt_s(&S237)), "bribeStatement");
  }
  RT_RET(rt_rec(0, 5, K18, (wv[]){ rt_s(&S55), v278_names, RT_NULL, RT_NULL, RT_NULL }));
}

static wv s220_schemeStatement(struct fr53 *up) {
  rt_enter("schemeStatement");
  (void)rt_chk(s209_next(up), "schemeStatement");
  if (rt_cond(rt_chk(s207_at(up, rt_s(&S149)), "schemeStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "schemeStatement"), rt_s(&S238), rt_s(&S239)), "schemeStatement");
  }
  (void)rt_chk(s211_expect(up, rt_s(&S90), rt_s(&S240), RT_NULL), "schemeStatement");
  wv v282_params = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S16)), "schemeStatement")))) {
    while (rt_cond(rt_b(1))) {
      wv v284_isShifty = rt_ne(rt_chk(s210_accept(up, rt_s(&S36)), "schemeStatement"), RT_NULL);
      wv v285_isFickle = rt_b(rt_bool(rt_not(v284_isShifty), "&&") && rt_bool(rt_ne(rt_chk(s210_accept(up, rt_s(&S37)), "schemeStatement"), RT_NULL), "&&"));
      wv v286_name = rt_bind(rt_chk(s211_expect(up, rt_s(&S149), rt_s(&S241), RT_NULL), "schemeStatement"), 0);
      (void)({ wv t387 = v282_params; wv t388 = ({ wv t383 = rt_get(&F_v, v286_name); wv t384 = v284_isShifty; wv t385 = v285_isFickle; wv t386 = rt_get(&F_pos, v286_name); rt_rec(0, 4, K19, (wv[]){ t383, t384, t385, t386 }); }); std_push(t387, t388); });
      if (rt_cond(rt_eq(rt_chk(s210_accept(up, rt_s(&S89)), "schemeStatement"), RT_NULL))) {
        break;
      }
    }
  }
  (void)rt_chk(s211_expect(up, rt_s(&S16), rt_s(&S242), RT_NULL), "schemeStatement");
  wv v283_name = rt_bind(rt_chk(s211_expect(up, rt_s(&S149), rt_s(&S243), rt_s(&S239)), "schemeStatement"), 0);
  RT_RET(({ wv t389 = rt_get(&F_v, v283_name); wv t390 = rt_get(&F_pos, v283_name); wv t391 = v282_params; wv t392 = rt_chk(s223_block(up), "schemeStatement"); rt_rec(0, 8, K20, (wv[]){ rt_s(&S33), t389, t390, t391, t392, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s221_lineupStatement(struct fr53 *up) {
  rt_enter("lineupStatement");
  wv v287_kw = rt_bind(rt_chk(s209_next(up), "lineupStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v205_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v287_kw, rt_s(&S244)), "lineupStatement");
  }
  if (rt_cond(rt_chk(s207_at(up, rt_s(&S149)), "lineupStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "lineupStatement"), rt_s(&S245), rt_s(&S246)), "lineupStatement");
  }
  (void)rt_chk(s211_expect(up, rt_s(&S90), rt_s(&S247), RT_NULL), "lineupStatement");
  if (rt_cond(rt_chk(s207_at(up, rt_s(&S16)), "lineupStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "lineupStatement"), rt_s(&S248), rt_s(&S246)), "lineupStatement");
  }
  wv v288_members = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v290_m = rt_bind(rt_chk(s206_peek(up), "lineupStatement"), 0);
    if (rt_cond(rt_ne(rt_get(&F_t, v290_m), rt_s(&S149)))) {
      wv v292_message = rt_bind((rt_cond(({ wv t393 = w3_keywords; wv t394 = rt_get(&F_t, v290_m); rt_chk(w1_contains(t393, t394), "lineupStatement"); })) ? rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_t, v290_m)), rt_s(&S249) }) : rt_cat(2, (wv[]){ rt_s(&S250), rt_text(rt_chk(w4_describe(v290_m), "lineupStatement")) })), 0);
      (void)rt_chk(w4_failAt(v290_m, v292_message), "lineupStatement");
    }
    (void)rt_chk(s209_next(up), "lineupStatement");
    wv v291_k = rt_i(0);
    while (rt_cond(({ wv t395 = v291_k; wv t396 = std_length(v288_members); rt_lt(t395, t396); }))) {
      if (rt_cond(({ wv t397 = rt_get(&F_name, rt_idx(v291_k, v288_members)); wv t398 = rt_get(&F_v, v290_m); rt_eq(t397, t398); }))) {
        (void)({ wv t399 = v290_m; wv t400 = rt_cat(3, (wv[]){ rt_s(&S251), rt_text(rt_get(&F_v, v290_m)), rt_s(&S252) }); rt_chk(w4_failAt(t399, t400), "lineupStatement"); });
      }
      v291_k = rt_add(v291_k, rt_i(1));
    }
    (void)({ wv t403 = v288_members; wv t404 = ({ wv t401 = rt_get(&F_v, v290_m); wv t402 = rt_get(&F_pos, v290_m); rt_rec(0, 2, K17, (wv[]){ t401, t402 }); }); std_push(t403, t404); });
    if (rt_cond(rt_eq(rt_chk(s210_accept(up, rt_s(&S89)), "lineupStatement"), RT_NULL))) {
      break;
    }
  }
  (void)rt_chk(s211_expect(up, rt_s(&S16), rt_s(&S253), RT_NULL), "lineupStatement");
  wv v289_name = rt_bind(rt_chk(s211_expect(up, rt_s(&S149), rt_s(&S254), rt_s(&S246)), "lineupStatement"), 0);
  RT_RET(({ wv t405 = rt_get(&F_v, v289_name); wv t406 = rt_get(&F_pos, v289_name); wv t407 = v288_members; rt_rec(0, 7, K21, (wv[]){ rt_s(&S53), t405, t406, t407, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s222_cond(struct fr53 *up, wv v293_kw) {
  rt_enter("cond");
  v293_kw = rt_bind(v293_kw, 0);
  (void)({ wv t408 = rt_cat(3, (wv[]){ rt_s(&S255), rt_text(v293_kw), rt_s(&S111) }); wv t409 = rt_cat(3, (wv[]){ rt_s(&S256), rt_text(v293_kw), rt_s(&S257) }); rt_chk(s211_expect(up, rt_s(&S90), t408, t409), "cond"); });
  wv v294_e = rt_bind(rt_chk(s225_expr(up), "cond"), 0);
  (void)rt_chk(s211_expect(up, rt_s(&S16), rt_s(&S258), RT_NULL), "cond");
  RT_RET(v294_e);
}

static wv s223_block(struct fr53 *up) {
  rt_enter("block");
  (void)rt_chk(s211_expect(up, rt_s(&S93), rt_s(&S259), RT_NULL), "block");
  up->v205_depth = rt_add(rt_def(up->v205_depth, "depth"), rt_i(1));
  wv v295_stmts = rt_arr(0, NULL, 1);
  while (rt_cond(rt_chk(s207_at(up, rt_s(&S14)), "block"))) {
    (void)({ wv t410 = v295_stmts; wv t411 = rt_chk(s214_statement(up), "block"); std_push(t410, t411); });
  }
  if (rt_cond(rt_chk(s207_at(up, rt_s(&S15)), "block"))) {
    wv v298_close = rt_bind(rt_chk(s209_next(up), "block"), 0);
    up->v205_depth = rt_sub(rt_def(up->v205_depth, "depth"), rt_i(1));
    RT_RET(({ wv t412 = v295_stmts; wv t413 = rt_get(&F_pos, v298_close); rt_rec(0, 3, K22, (wv[]){ t412, RT_NULL, t413 }); }));
  }
  if (rt_cond(rt_chk(s207_at(up, rt_s(&S121)), "block"))) {
    (void)rt_chk(w4_failAt(rt_chk(s206_peek(up), "block"), rt_s(&S260)), "block");
  }
  wv v296_value = rt_bind(rt_chk(s225_expr(up), "block"), 0);
  if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S15)), "block")))) {
    wv v299_hint = rt_bind((rt_cond(rt_chk(s207_at(up, rt_s(&S90)), "block")) ? rt_def(w4_postfixHint, "postfixHint") : (rt_cond(rt_chk(s207_at(up, rt_s(&S28)), "block")) ? rt_s(&S261) : rt_s(&S262))), 0);
    wv v300_found = rt_bind(rt_chk(s206_peek(up), "block"), 0);
    (void)({ wv t414 = v300_found; wv t415 = rt_cat(2, (wv[]){ rt_s(&S263), rt_text(rt_chk(w4_describe(v300_found), "block")) }); wv t416 = v299_hint; rt_chk(w4_failAtHint(t414, t415, t416), "block"); });
  }
  wv v297_close = rt_bind(rt_chk(s209_next(up), "block"), 0);
  up->v205_depth = rt_sub(rt_def(up->v205_depth, "depth"), rt_i(1));
  RT_RET(({ wv t417 = v295_stmts; wv t418 = v296_value; wv t419 = rt_get(&F_pos, v297_close); rt_rec(0, 3, K22, (wv[]){ t417, t418, t419 }); }));
}

static wv s224_lvalue(struct fr53 *up) {
  rt_enter("lvalue");
  wv v301_tk = rt_bind(rt_chk(s206_peek(up), "lvalue"), 0);
  wv v302_e = rt_bind(rt_chk(s236_postfix(up), "lvalue"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_k, v302_e), rt_s(&S264)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v302_e), rt_s(&S197)), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_k, v302_e), rt_s(&S198)), "&&")))) {
    (void)rt_chk(w4_failAt(v301_tk, rt_s(&S265)), "lvalue");
  }
  RT_RET(v302_e);
}

static wv s225_expr(struct fr53 *up) {
  rt_enter("expr");
  RT_RET(rt_chk(s226_seq(up, rt_b(0)), "expr"));
}

static wv s226_seq(struct fr53 *up, wv v303_list) {
  rt_enter("seq");
  v303_list = rt_bind(v303_list, 0);
  wv v304_pending = rt_arr(0, NULL, 1);
  wv v305_firstComma = RT_NULL;
  wv v306_cur = (rt_cond(rt_chk(s207_at(up, rt_s(&S89)), "seq")) ? rt_chk(s227_zeroCall(up), "seq") : rt_chk(s230_ternary(up), "seq"));
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s208_atCall(up), "seq"))) {
      (void)std_push(v304_pending, v306_cur);
      v306_cur = rt_chk(s229_continueOps(up, rt_chk(s228_call(up, v304_pending), "seq")), "seq");
      v304_pending = rt_arr(0, NULL, 1);
      v305_firstComma = RT_NULL;
      continue;
    }
    if (rt_cond(rt_chk(s207_at(up, rt_s(&S89)), "seq"))) {
      wv v307_comma = rt_bind(rt_chk(s209_next(up), "seq"), 0);
      if (rt_cond(rt_eq(v305_firstComma, RT_NULL))) {
        v305_firstComma = v307_comma;
      }
      (void)std_push(v304_pending, v306_cur);
      if (rt_cond(rt_chk(s207_at(up, rt_s(&S89)), "seq"))) {
        (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "seq"), rt_s(&S266), rt_s(&S267)), "seq");
      }
      if (rt_cond(rt_b(rt_bool(v303_list, "&&") && rt_bool(rt_chk(s207_at(up, rt_s(&S92)), "seq"), "&&")))) {
        (void)rt_chk(w4_failAt(v307_comma, rt_s(&S268)), "seq");
      }
      v306_cur = rt_chk(s230_ternary(up), "seq");
      continue;
    }
    break;
  }
  (void)std_push(v304_pending, v306_cur);
  if (rt_cond(v303_list)) {
    RT_RET(v304_pending);
  }
  if (rt_cond(rt_gt(std_length(v304_pending), rt_i(1)))) {
    if (rt_cond(rt_chk(s207_at(up, rt_s(&S55)), "seq"))) {
      (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "seq"), rt_s(&S227), rt_s(&S218)), "seq");
    }
    wv v308_hint = rt_bind((rt_cond(rt_b(rt_bool(rt_get(&F_nl, v305_firstComma), "&&") && rt_bool(rt_gt(rt_def(up->v205_depth, "depth"), rt_i(0)), "&&"))) ? rt_s(&S269) : rt_s(&S270)), 0);
    (void)rt_chk(w4_failAtHint(v305_firstComma, rt_s(&S271), v308_hint), "seq");
  }
  RT_RET(v306_cur);
}

static wv s227_zeroCall(struct fr53 *up) {
  rt_enter("zeroCall");
  wv v309_comma = rt_bind(rt_chk(s209_next(up), "zeroCall"), 0);
  if (rt_cond(rt_not(rt_chk(s208_atCall(up), "zeroCall")))) {
    (void)rt_chk(w4_failAtHint(v309_comma, rt_s(&S272), rt_s(&S273)), "zeroCall");
  }
  RT_RET(rt_chk(s229_continueOps(up, rt_chk(s228_call(up, rt_arr(0, NULL, 0)), "zeroCall")), "zeroCall"));
}

static wv s228_call(struct fr53 *up, wv v310_args) {
  rt_enter("call");
  (void)rt_chk(s209_next(up), "call");
  wv v311_c = rt_bind(rt_chk(s209_next(up), "call"), 0);
  if (rt_cond(rt_ne(rt_chk(s210_accept(up, rt_s(&S107)), "call"), RT_NULL))) {
    wv v312_m = rt_bind(rt_chk(s209_next(up), "call"), 0);
    (void)rt_chk(s209_next(up), "call");
    wv v313_rec = ({ wv t420 = rt_get(&F_v, v312_m); wv t421 = rt_get(&F_pos, v312_m); rt_rec(0, 4, K23, (wv[]){ rt_s(&S264), t420, t421, rt_b(0) }); });
    RT_RET(({ wv t425 = ({ wv t422 = rt_get(&F_v, v311_c); wv t423 = v313_rec; wv t424 = rt_get(&F_pos, v311_c); rt_rec(0, 5, K25, (wv[]){ rt_s(&S198), t422, t423, t424, rt_b(0) }); }); wv t426 = v310_args; wv t427 = rt_get(&F_pos, v311_c); rt_rec(0, 5, K24, (wv[]){ rt_s(&S191), t425, t426, t427, rt_b(0) }); }));
  }
  (void)rt_chk(s209_next(up), "call");
  if (rt_cond(rt_ne(rt_get(&F_t, v311_c), rt_s(&S162)))) {
    RT_RET(({ wv t431 = ({ wv t428 = (rt_cond(rt_eq(rt_get(&F_t, v311_c), rt_s(&S149))) ? rt_s(&S264) : rt_s(&S164)); wv t429 = rt_get(&F_v, v311_c); wv t430 = rt_get(&F_pos, v311_c); rt_rec(0, 4, K23, (wv[]){ t428, t429, t430, rt_b(0) }); }); wv t432 = v310_args; wv t433 = rt_get(&F_pos, v311_c); rt_rec(0, 5, K24, (wv[]){ rt_s(&S191), t431, t432, t433, rt_b(0) }); }));
  }
  if (rt_cond(rt_ne(rt_get(&F_text, v311_c), rt_s(&S274)))) {
    (void)({ wv t434 = v311_c; wv t435 = rt_cat(2, (wv[]){ rt_s(&S275), rt_text(rt_get(&F_text, v311_c)) }); rt_chk(w4_failAtHint(t434, t435, rt_s(&S276)), "call"); });
  }
  if (rt_cond(rt_ne(std_length(v310_args), rt_i(1)))) {
    (void)rt_chk(w4_failAt(v311_c, rt_s(&S277)), "call");
  }
  RT_RET(({ wv t436 = rt_idx(rt_i(0), v310_args); wv t437 = rt_get(&F_pos, v311_c); rt_rec(0, 4, K26, (wv[]){ rt_s(&S192), t436, t437, rt_b(0) }); }));
}

static wv s229_continueOps(struct fr53 *up, wv v314_left) {
  rt_enter("continueOps");
  v314_left = rt_bind(v314_left, 0);
  wv v315_op = rt_bind(rt_get(&F_t, rt_chk(s206_peek(up), "continueOps")), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v315_op, rt_s(&S66)), "||") || rt_bool(rt_eq(v315_op, rt_s(&S88)), "||")), "||") || rt_bool(rt_gt(rt_chk(w4_precedence(v315_op), "continueOps"), rt_i(0)), "||")))) {
    RT_RET(rt_chk(s231_ternaryTail(up, rt_chk(s233_binaryFrom(up, rt_chk(s235_powerTail(up, v314_left), "continueOps"), rt_i(1)), "continueOps")), "continueOps"));
  }
  RT_RET(v314_left);
}

static wv s230_ternary(struct fr53 *up) {
  rt_enter("ternary");
  RT_RET(rt_chk(s231_ternaryTail(up, rt_chk(s232_binary(up, rt_i(1)), "ternary")), "ternary"));
}

static wv s231_ternaryTail(struct fr53 *up, wv v316_c) {
  rt_enter("ternaryTail");
  if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S88)), "ternaryTail")))) {
    RT_RET(v316_c);
  }
  wv v317_q = rt_bind(rt_chk(s209_next(up), "ternaryTail"), 0);
  (void)rt_chk(s212_lintSpace(up, v317_q), "ternaryTail");
  wv v318_a = rt_bind(rt_chk(s230_ternary(up), "ternaryTail"), 0);
  wv v319_colon = rt_bind(rt_chk(s211_expect(up, rt_s(&S28), rt_s(&S278), RT_NULL), "ternaryTail"), 0);
  (void)rt_chk(s212_lintSpace(up, v319_colon), "ternaryTail");
  wv v320_b = rt_bind(rt_chk(s230_ternary(up), "ternaryTail"), 0);
  RT_RET(({ wv t438 = v316_c; wv t439 = v318_a; wv t440 = v320_b; wv t441 = rt_get(&F_pos, v317_q); rt_rec(0, 6, K27, (wv[]){ rt_s(&S201), t438, t439, t440, t441, rt_b(0) }); }));
}

static wv s232_binary(struct fr53 *up, wv v321_min) {
  rt_enter("binary");
  v321_min = rt_bind(v321_min, 0);
  RT_RET(({ wv t442 = rt_chk(s234_unary(up), "binary"); wv t443 = v321_min; rt_chk(s233_binaryFrom(up, t442, t443), "binary"); }));
}

static wv s233_binaryFrom(struct fr53 *up, wv v322_left, wv v323_min) {
  rt_enter("binaryFrom");
  v322_left = rt_bind(v322_left, 0);
  v323_min = rt_bind(v323_min, 0);
  wv v324_acc = v322_left;
  while (rt_cond(rt_b(1))) {
    wv v325_op = rt_bind(rt_chk(s206_peek(up), "binaryFrom"), 0);
    wv v326_prec = rt_bind(rt_chk(w4_precedence(rt_get(&F_t, v325_op)), "binaryFrom"), 0);
    if (rt_cond(rt_lt(v326_prec, v323_min))) {
      RT_RET(v324_acc);
    }
    (void)rt_chk(s209_next(up), "binaryFrom");
    (void)rt_chk(s212_lintSpace(up, v325_op), "binaryFrom");
    wv v327_right = rt_chk(s232_binary(up, rt_add(v326_prec, rt_i(1))), "binaryFrom");
    v324_acc = ({ wv t444 = rt_get(&F_t, v325_op); wv t445 = v324_acc; wv t446 = v327_right; wv t447 = rt_get(&F_pos, v325_op); rt_rec(0, 6, K28, (wv[]){ rt_s(&S200), t444, t445, t446, t447, rt_b(0) }); });
  }
  RT_RET(RT_NULL);
}

static wv s234_unary(struct fr53 *up) {
  rt_enter("unary");
  wv v328_tk = rt_bind(rt_chk(s206_peek(up), "unary"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S72)), "||") || rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S87)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S86)), "||")))) {
    (void)rt_chk(s209_next(up), "unary");
    wv v329_e = rt_bind(rt_chk(s234_unary(up), "unary"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v328_tk), rt_s(&S72)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v329_e), rt_s(&S200)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v329_e), rt_s(&S66)), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v329_e)), "&&")))) {
      (void)rt_chk(w4_failAtHint(v328_tk, rt_s(&S279), rt_s(&S280)), "unary");
    }
    RT_RET(({ wv t448 = rt_get(&F_t, v328_tk); wv t449 = v329_e; wv t450 = rt_get(&F_pos, v328_tk); rt_rec(0, 5, K29, (wv[]){ rt_s(&S199), t448, t449, t450, rt_b(0) }); }));
  }
  RT_RET(rt_chk(s235_powerTail(up, rt_chk(s236_postfix(up), "unary")), "unary"));
}

static wv s235_powerTail(struct fr53 *up, wv v330_base) {
  rt_enter("powerTail");
  if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S66)), "powerTail")))) {
    RT_RET(v330_base);
  }
  wv v331_op = rt_bind(rt_chk(s209_next(up), "powerTail"), 0);
  (void)rt_chk(s212_lintSpace(up, v331_op), "powerTail");
  RT_RET(({ wv t451 = v330_base; wv t452 = rt_chk(s234_unary(up), "powerTail"); wv t453 = rt_get(&F_pos, v331_op); rt_rec(0, 6, K28, (wv[]){ rt_s(&S200), rt_s(&S66), t451, t452, t453, rt_b(0) }); }));
}

static wv s236_postfix(struct fr53 *up) {
  rt_enter("postfix");
  wv v332_tk = rt_bind(rt_chk(s206_peek(up), "postfix"), 0);
  if (rt_cond(rt_b(rt_bool(({ wv t454 = rt_add(rt_def(up->v204_p, "p"), rt_i(1)); wv t455 = std_length(up->v202_toks); rt_lt(t454, t455); }), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t456 = rt_add(rt_def(up->v204_p, "p"), rt_i(1)); wv t457 = up->v202_toks; rt_idx(t456, t457); })), rt_s(&S107)), "&&")))) {
    if (rt_cond(rt_ne(rt_get(&F_t, v332_tk), rt_s(&S149)))) {
      wv v334_message = rt_bind((rt_cond(({ wv t458 = w3_keywords; wv t459 = rt_get(&F_t, v332_tk); rt_chk(w1_contains(t458, t459), "postfix"); })) ? rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_t, v332_tk)), rt_s(&S281) }) : rt_cat(2, (wv[]){ rt_s(&S282), rt_text(rt_chk(w4_describe(v332_tk), "postfix")) })), 0);
      (void)rt_chk(w4_failAtHint(v332_tk, v334_message, rt_s(&S142)), "postfix");
    }
    (void)rt_chk(s209_next(up), "postfix");
    (void)rt_chk(s209_next(up), "postfix");
    RT_RET(({ wv t460 = rt_get(&F_v, v332_tk); wv t461 = rt_chk(s236_postfix(up), "postfix"); wv t462 = rt_get(&F_pos, v332_tk); rt_rec(0, 5, K25, (wv[]){ rt_s(&S198), t460, t461, t462, rt_b(0) }); }));
  }
  wv v333_e = rt_chk(s237_primary(up), "postfix");
  while (rt_cond(rt_chk(s207_at(up, rt_s(&S91)), "postfix"))) {
    wv v335_open = rt_bind(rt_chk(s209_next(up), "postfix"), 0);
    if (rt_cond(rt_b(rt_bool(rt_get(&F_nl, v335_open), "&&") && rt_bool(rt_gt(rt_def(up->v205_depth, "depth"), rt_i(0)), "&&")))) {
      (void)({ wv t463 = w2_warnings; wv t464 = rt_chk(w2_warn(rt_get(&F_pos, v335_open), rt_s(&S283), rt_s(&S284)), "postfix"); std_push(t463, t464); });
    }
    wv v336_arr = rt_bind(rt_chk(s225_expr(up), "postfix"), 0);
    (void)rt_chk(s211_expect(up, rt_s(&S92), rt_s(&S285), RT_NULL), "postfix");
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v333_e), rt_s(&S179)), "&&") && rt_bool(rt_not(rt_get(&F_paren, v333_e)), "&&")), "&&") && rt_bool(rt_eq(std_length(rt_get(&F_parts, v333_e)), rt_i(1)), "&&")), "&&") && rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v333_e))), rt_s(&S194)), "&&")))) {
      wv v337_key = rt_bind(rt_idx(rt_i(0), rt_get(&F_parts, v333_e)), 0);
      if (rt_cond(rt_b(rt_bool(rt_chk(w4_isIdent(v337_key), "postfix"), "&&") && rt_bool(rt_not(rt_chk(w1_contains(w3_keywords, v337_key), "postfix")), "&&")))) {
        (void)({ wv t469 = w2_warnings; wv t470 = ({ wv t467 = rt_get(&F_pos, v333_e); wv t468 = ({ wv t465 = rt_text(v337_key); wv t466 = rt_text(v337_key); rt_cat(5, (wv[]){ rt_s(&S286), t465, rt_s(&S287), t466, rt_s(&S288) }); }); rt_chk(w2_warn(t467, t468, RT_NULL), "postfix"); }); std_push(t469, t470); });
      }
    }
    v333_e = ({ wv t471 = v333_e; wv t472 = v336_arr; wv t473 = rt_get(&F_pos, v335_open); rt_rec(0, 5, K30, (wv[]){ rt_s(&S197), t471, t472, t473, rt_b(0) }); });
  }
  if (rt_cond(rt_chk(s207_at(up, rt_s(&S107)), "postfix"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "postfix"), rt_s(&S289), rt_s(&S290)), "postfix");
  }
  RT_RET(v333_e);
}

static wv s237_primary(struct fr53 *up) {
  rt_enter("primary");
  wv v338_tk = rt_bind(rt_chk(s206_peek(up), "primary"), 0);
  wv v339_at0 = rt_bind(rt_get(&F_pos, v338_tk), 0);
  wv v340_kind = rt_bind(rt_get(&F_t, v338_tk), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v340_kind, rt_s(&S39)), "||") || rt_bool(rt_eq(v340_kind, rt_s(&S183)), "||")))) {
    RT_RET(rt_chk(s242_chain(up), "primary"));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S51)))) {
    RT_RET(rt_chk(s239_parseSuspect(up), "primary"));
  }
  (void)rt_chk(s209_next(up), "primary");
  if (rt_cond(rt_b(rt_bool(rt_eq(v340_kind, rt_s(&S106)), "||") || rt_bool(rt_eq(v340_kind, rt_s(&S109)), "||")))) {
    RT_RET(({ wv t474 = v340_kind; wv t475 = rt_get(&F_v, v338_tk); wv t476 = v339_at0; rt_rec(0, 4, K13, (wv[]){ t474, t475, t476, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S179)))) {
    wv v341_parts = rt_arr(0, NULL, 1);
    wv v342_k = rt_i(0);
    while (rt_cond(({ wv t477 = v342_k; wv t478 = std_length(rt_get(&F_parts, v338_tk)); rt_lt(t477, t478); }))) {
      wv v343_part = rt_bind(({ wv t479 = v342_k; wv t480 = rt_get(&F_parts, v338_tk); rt_idx(t479, t480); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v343_part), rt_s(&S194)))) {
        (void)std_push(v341_parts, v343_part);
      } else {
        (void)({ wv t481 = v341_parts; wv t482 = rt_chk(w4_parser(rt_get(&F_tokens, v343_part), rt_b(1)), "primary"); std_push(t481, t482); });
      }
      v342_k = rt_add(v342_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 4, K31, (wv[]){ rt_s(&S179), v341_parts, v339_at0, rt_b(0) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v340_kind, rt_s(&S46)), "||") || rt_bool(rt_eq(v340_kind, rt_s(&S47)), "||")))) {
    RT_RET(({ wv t483 = rt_eq(v340_kind, rt_s(&S46)); wv t484 = v339_at0; rt_rec(0, 4, K13, (wv[]){ rt_s(&S291), t483, t484, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S48)))) {
    RT_RET(rt_rec(0, 3, K32, (wv[]){ rt_s(&S292), v339_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S49)))) {
    RT_RET(rt_rec(0, 3, K32, (wv[]){ rt_s(&S293), v339_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S50)))) {
    RT_RET(rt_rec(0, 3, K32, (wv[]){ rt_s(&S294), v339_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S149)))) {
    RT_RET(({ wv t485 = rt_get(&F_v, v338_tk); wv t486 = v339_at0; rt_rec(0, 4, K23, (wv[]){ rt_s(&S264), t485, t486, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S164)))) {
    RT_RET(({ wv t487 = rt_get(&F_v, v338_tk); wv t488 = v339_at0; rt_rec(0, 4, K23, (wv[]){ rt_s(&S164), t487, t488, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S90)))) {
    if (rt_cond(rt_chk(s207_at(up, rt_s(&S16)), "primary"))) {
      (void)rt_chk(w4_failAtHint(v338_tk, rt_s(&S295), rt_s(&S273)), "primary");
    }
    wv v344_e = rt_bind(rt_chk(s225_expr(up), "primary"), 1);
    (void)rt_chk(s211_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "primary");
    rt_set_mem(rt_b(1), &F_paren, v344_e);
    RT_RET(v344_e);
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S91)))) {
    wv v345_items = rt_bind((rt_cond(rt_chk(s207_at(up, rt_s(&S92)), "primary")) ? rt_arr(0, NULL, 0) : rt_chk(s226_seq(up, rt_b(1)), "primary")), 0);
    (void)rt_chk(s211_expect(up, rt_s(&S92), rt_s(&S296), RT_NULL), "primary");
    RT_RET(rt_rec(0, 4, K33, (wv[]){ rt_s(&S195), v345_items, v339_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S162)))) {
    (void)({ wv t489 = v338_tk; wv t490 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_text, v338_tk)), rt_s(&S297) }); rt_chk(w4_failAtHint(t489, t490, rt_s(&S298)), "primary"); });
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v340_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v340_kind, rt_s(&S38)), "||")), "||") || rt_bool(rt_eq(v340_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v340_kind, rt_s(&S37)), "||")))) {
    (void)({ wv t491 = v338_tk; wv t492 = rt_cat(3, (wv[]){ rt_s(&S299), rt_text(v340_kind), rt_s(&S111) }); rt_chk(w4_failAtHint(t491, t492, rt_s(&S300)), "primary"); });
  }
  if (rt_cond(rt_eq(v340_kind, rt_s(&S93)))) {
    RT_RET(rt_chk(s238_record(up, v338_tk), "primary"));
  }
  (void)({ wv t493 = v338_tk; wv t494 = rt_cat(2, (wv[]){ rt_s(&S301), rt_text(rt_chk(w4_describe(v338_tk), "primary")) }); rt_chk(w4_failAt(t493, t494), "primary"); });
  RT_RET(RT_NULL);
}

static wv s238_record(struct fr53 *up, wv v346_open) {
  rt_enter("record");
  v346_open = rt_bind(v346_open, 0);
  wv v347_fields = rt_arr(0, NULL, 1);
  wv v348_seen = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S15)), "record")))) {
    while (rt_cond(rt_b(1))) {
      wv v349_key = rt_bind(rt_chk(s206_peek(up), "record"), 0);
      if (rt_cond(rt_ne(rt_get(&F_t, v349_key), rt_s(&S149)))) {
        wv v351_message = rt_bind((rt_cond(({ wv t495 = w3_keywords; wv t496 = rt_get(&F_t, v349_key); rt_chk(w1_contains(t495, t496), "record"); })) ? rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_t, v349_key)), rt_s(&S281) }) : rt_cat(2, (wv[]){ rt_s(&S302), rt_text(rt_chk(w4_describe(v349_key), "record")) })), 0);
        (void)rt_chk(w4_failAtHint(v349_key, v351_message, rt_s(&S303)), "record");
      }
      (void)rt_chk(s209_next(up), "record");
      if (rt_cond(({ wv t497 = v348_seen; wv t498 = rt_get(&F_v, v349_key); rt_chk(w1_contains(t497, t498), "record"); }))) {
        (void)({ wv t499 = v349_key; wv t500 = rt_cat(3, (wv[]){ rt_s(&S304), rt_text(rt_get(&F_v, v349_key)), rt_s(&S305) }); rt_chk(w4_failAt(t499, t500), "record"); });
      }
      (void)({ wv t501 = v348_seen; wv t502 = rt_get(&F_v, v349_key); std_push(t501, t502); });
      if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S28)), "record")))) {
        wv v352_found = rt_bind(rt_chk(s206_peek(up), "record"), 0);
        wv v353_hint = rt_bind((rt_cond(rt_b(rt_bool(rt_chk(s207_at(up, rt_s(&S89)), "record"), "||") || rt_bool(rt_chk(s207_at(up, rt_s(&S15)), "record"), "||"))) ? ({ wv t503 = rt_text(rt_get(&F_v, v349_key)); wv t504 = rt_text(rt_get(&F_v, v349_key)); rt_cat(4, (wv[]){ rt_s(&S306), t503, rt_s(&S29), t504 }); }) : RT_NULL), 0);
        (void)({ wv t507 = v352_found; wv t508 = ({ wv t505 = rt_text(rt_get(&F_v, v349_key)); wv t506 = rt_text(rt_chk(w4_describe(v352_found), "record")); rt_cat(4, (wv[]){ rt_s(&S307), t505, rt_s(&S308), t506 }); }); wv t509 = v353_hint; rt_chk(w4_failAtHint(t507, t508, t509), "record"); });
      }
      (void)rt_chk(s209_next(up), "record");
      (void)({ wv t513 = v347_fields; wv t514 = ({ wv t510 = rt_get(&F_v, v349_key); wv t511 = rt_chk(s241_fieldValue(up), "record"); wv t512 = rt_get(&F_pos, v349_key); rt_rec(0, 3, K34, (wv[]){ t510, t511, t512 }); }); std_push(t513, t514); });
      if (rt_cond(rt_not(rt_chk(s207_at(up, rt_s(&S89)), "record")))) {
        break;
      }
      wv v350_comma = rt_bind(rt_chk(s209_next(up), "record"), 0);
      if (rt_cond(rt_chk(s207_at(up, rt_s(&S15)), "record"))) {
        (void)rt_chk(w4_failAt(v350_comma, rt_s(&S309)), "record");
      }
    }
  }
  (void)rt_chk(s211_expect(up, rt_s(&S15), rt_s(&S310), RT_NULL), "record");
  RT_RET(({ wv t515 = v347_fields; wv t516 = rt_get(&F_pos, v346_open); rt_rec(0, 4, K35, (wv[]){ rt_s(&S196), t515, t516, rt_b(0) }); }));
}

static wv s239_parseSuspect(struct fr53 *up) {
  rt_enter("parseSuspect");
  wv v354_kw = rt_bind(rt_chk(s209_next(up), "parseSuspect"), 0);
  wv v355_subject = rt_bind(rt_chk(s222_cond(up, rt_s(&S51)), "parseSuspect"), 0);
  (void)rt_chk(s211_expect(up, rt_s(&S93), rt_s(&S311), RT_NULL), "parseSuspect");
  wv v356_alibis = rt_arr(0, NULL, 1);
  wv v357_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s207_at(up, rt_s(&S52)), "parseSuspect"))) {
      wv v358_a = rt_bind(rt_chk(s209_next(up), "parseSuspect"), 0);
      if (rt_cond(rt_ne(v357_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v358_a, rt_s(&S312)), "parseSuspect");
      }
      if (rt_cond(rt_chk(s207_at(up, rt_s(&S93)), "parseSuspect"))) {
        (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "parseSuspect"), rt_s(&S313), rt_s(&S314)), "parseSuspect");
      }
      wv v359_values = rt_arr(0, NULL, 1);
      (void)({ wv t517 = v359_values; wv t518 = rt_chk(s240_alibiValue(up), "parseSuspect"); std_push(t517, t518); });
      while (rt_cond(rt_ne(rt_chk(s210_accept(up, rt_s(&S89)), "parseSuspect"), RT_NULL))) {
        (void)({ wv t519 = v359_values; wv t520 = rt_chk(s240_alibiValue(up), "parseSuspect"); std_push(t519, t520); });
      }
      (void)({ wv t524 = v356_alibis; wv t525 = ({ wv t521 = v359_values; wv t522 = rt_chk(s223_block(up), "parseSuspect"); wv t523 = rt_get(&F_pos, v358_a); rt_rec(0, 3, K36, (wv[]){ t521, t522, t523 }); }); std_push(t524, t525); });
    } else if (rt_cond(rt_chk(s207_at(up, rt_s(&S40)), "parseSuspect"))) {
      wv v360_a = rt_bind(rt_chk(s209_next(up), "parseSuspect"), 0);
      if (rt_cond(rt_ne(v357_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v360_a, rt_s(&S315)), "parseSuspect");
      }
      if (rt_cond(rt_eq(std_length(v356_alibis), rt_i(0)))) {
        (void)rt_chk(w4_failAt(v360_a, rt_s(&S316)), "parseSuspect");
      }
      v357_els = rt_bind(rt_chk(s223_block(up), "parseSuspect"), 1);
    } else {
      break;
    }
  }
  if (rt_cond(rt_eq(std_length(v356_alibis), rt_i(0)))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s206_peek(up), "parseSuspect"), rt_s(&S317), rt_s(&S318)), "parseSuspect");
  }
  (void)rt_chk(s211_expect(up, rt_s(&S15), rt_s(&S319), RT_NULL), "parseSuspect");
  RT_RET(({ wv t526 = v355_subject; wv t527 = v356_alibis; wv t528 = v357_els; wv t529 = rt_get(&F_pos, v354_kw); rt_rec(0, 6, K37, (wv[]){ rt_s(&S51), t526, t527, t528, t529, rt_b(0) }); }));
}

static wv s240_alibiValue(struct fr53 *up) {
  rt_enter("alibiValue");
  wv v361_tk = rt_bind(rt_chk(s206_peek(up), "alibiValue"), 0);
  wv v362_v = rt_bind(rt_chk(s236_postfix(up), "alibiValue"), 0);
  wv v363_kind = rt_bind(rt_get(&F_k, v362_v), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v363_kind, rt_s(&S106)), "||") || rt_bool(rt_eq(v363_kind, rt_s(&S291)), "||")), "||") || rt_bool(rt_eq(v363_kind, rt_s(&S292)), "||")))) {
    RT_RET(v362_v);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v363_kind, rt_s(&S198)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v362_v)), rt_s(&S264)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v362_v)), rt_s(&S198)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, rt_get(&F_rec, v362_v))), rt_s(&S264)), "&&")), "||")), "&&")))) {
    RT_RET(v362_v);
  }
  if (rt_cond(rt_eq(v363_kind, rt_s(&S179)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v362_v)), rt_i(1)), "&&") && rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v362_v))), rt_s(&S194)), "&&")))) {
      RT_RET(v362_v);
    }
    (void)rt_chk(w4_failAt(v361_tk, rt_s(&S320)), "alibiValue");
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v363_kind, rt_s(&S109)), "||") || rt_bool(rt_eq(v363_kind, rt_s(&S293)), "||")), "||") || rt_bool(rt_eq(v363_kind, rt_s(&S294)), "||")))) {
    (void)rt_chk(w4_failAtHint(v361_tk, rt_s(&S321), rt_s(&S322)), "alibiValue");
  }
  (void)rt_chk(w4_failAtHint(v361_tk, rt_s(&S323), rt_s(&S324)), "alibiValue");
  RT_RET(RT_NULL);
}

static wv s241_fieldValue(struct fr53 *up) {
  rt_enter("fieldValue");
  wv v364_cur = (rt_cond(rt_chk(s207_at(up, rt_s(&S89)), "fieldValue")) ? rt_chk(s227_zeroCall(up), "fieldValue") : rt_chk(s230_ternary(up), "fieldValue"));
  while (rt_cond(rt_chk(s208_atCall(up), "fieldValue"))) {
    v364_cur = rt_chk(s229_continueOps(up, rt_chk(s228_call(up, rt_arr(1, (wv[]){ v364_cur }, 0)), "fieldValue")), "fieldValue");
  }
  RT_RET(v364_cur);
}

static wv s242_chain(struct fr53 *up) {
  rt_enter("chain");
  wv v365_branches = rt_arr(0, NULL, 1);
  wv v366_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    wv v367_kw = rt_bind(rt_chk(s209_next(up), "chain"), 0);
    wv v368_c = rt_bind(rt_chk(s222_cond(up, rt_get(&F_t, v367_kw)), "chain"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v367_kw), rt_s(&S39)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v368_c), rt_s(&S200)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v368_c), rt_s(&S79)), "&&")))) {
      (void)({ wv t530 = w2_warnings; wv t531 = rt_chk(w2_warn(rt_get(&F_pos, v367_kw), rt_s(&S325), rt_s(&S326)), "chain"); std_push(t530, t531); });
    }
    (void)({ wv t536 = v365_branches; wv t537 = ({ wv t532 = rt_eq(rt_get(&F_t, v367_kw), rt_s(&S39)); wv t533 = v368_c; wv t534 = rt_chk(s223_block(up), "chain"); wv t535 = rt_get(&F_pos, v367_kw); rt_rec(0, 4, K38, (wv[]){ t532, t533, t534, t535 }); }); std_push(t536, t537); });
    if (rt_cond(rt_eq(rt_chk(s210_accept(up, rt_s(&S40)), "chain"), RT_NULL))) {
      break;
    }
    if (rt_cond(rt_b(rt_bool(rt_chk(s207_at(up, rt_s(&S39)), "chain"), "||") || rt_bool(rt_chk(s207_at(up, rt_s(&S183)), "chain"), "||")))) {
      continue;
    }
    v366_els = rt_bind(rt_chk(s223_block(up), "chain"), 1);
    break;
  }
  RT_RET(({ wv t538 = v365_branches; wv t539 = v366_els; wv t540 = rt_get(&F_pos, rt_idx(rt_i(0), v365_branches)); rt_rec(0, 5, K39, (wv[]){ rt_s(&S193), t538, t539, t540, rt_b(0) }); }));
}

static wv w5_stdSig(wv v371_name) {
  rt_enter("stdSig");
  v371_name = rt_bind(v371_name, 0);
  wv v372_k = rt_i(0);
  while (rt_cond(({ wv t579 = v372_k; wv t580 = std_length(rt_def(w5_stdlib, "stdlib")); rt_lt(t579, t580); }))) {
    if (rt_cond(({ wv t583 = rt_get(&F_name, ({ wv t581 = v372_k; wv t582 = rt_def(w5_stdlib, "stdlib"); rt_idx(t581, t582); })); wv t584 = v371_name; rt_eq(t583, t584); }))) {
      RT_RET(rt_get(&F_arity, ({ wv t585 = v372_k; wv t586 = rt_def(w5_stdlib, "stdlib"); rt_idx(t585, t586); })));
    }
    v372_k = rt_add(v372_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w5_modeOf(wv v373_x) {
  rt_enter("modeOf");
  v373_x = rt_bind(v373_x, 0);
  RT_RET((rt_cond(rt_get(&F_isShifty, v373_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isFickle, v373_x)) ? rt_s(&S37) : rt_s(&S13))));
}

static wv w5_plural(wv v374_n) {
  rt_enter("plural");
  v374_n = rt_bind(v374_n, 0);
  RT_RET(({ wv t587 = rt_cat(2, (wv[]){ rt_text(v374_n), rt_s(&S366) }); wv t588 = (rt_cond(rt_eq(v374_n, rt_i(1))) ? rt_s(&S13) : rt_s(&S367)); rt_add(t587, t588); }));
}

static wv w5_lineupText(wv v375_x) {
  rt_enter("lineupText");
  v375_x = rt_bind(v375_x, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v375_x), rt_s(&S264))) ? rt_get(&F_name, v375_x) : ({ wv t589 = rt_text(rt_get(&F_field, v375_x)); wv t590 = rt_text(rt_get(&F_name, rt_get(&F_rec, v375_x))); rt_cat(3, (wv[]){ t589, rt_s(&S107), t590 }); })));
}

static wv w5_alibiKey(wv v376_v) {
  rt_enter("alibiKey");
  v376_v = rt_bind(v376_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v376_v), rt_s(&S179)))) {
    RT_RET(rt_add(rt_s(&S368), rt_idx(rt_i(0), rt_get(&F_parts, v376_v))));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v376_v), rt_s(&S198)))) {
    RT_RET(({ wv t591 = rt_text(rt_get(&F_field, v376_v)); wv t592 = rt_text(rt_chk(w5_lineupText(rt_get(&F_rec, v376_v)), "alibiKey")); rt_cat(4, (wv[]){ rt_s(&S369), t591, rt_s(&S107), t592 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v376_v), rt_s(&S292)))) {
    RT_RET(rt_s(&S292));
  }
  RT_RET(({ wv t593 = rt_text(rt_get(&F_k, v376_v)); wv t594 = rt_text(rt_get(&F_v, v376_v)); rt_cat(3, (wv[]){ t593, rt_s(&S28), t594 }); }));
}

static wv w5_alibiText(wv v377_v) {
  rt_enter("alibiText");
  v377_v = rt_bind(v377_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v377_v), rt_s(&S179)))) {
    RT_RET(rt_chk(w1_quote(rt_idx(rt_i(0), rt_get(&F_parts, v377_v))), "alibiText"));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v377_v), rt_s(&S198)))) {
    RT_RET(({ wv t595 = rt_text(rt_get(&F_field, v377_v)); wv t596 = rt_text(rt_chk(w5_lineupText(rt_get(&F_rec, v377_v)), "alibiText")); rt_cat(3, (wv[]){ t595, rt_s(&S107), t596 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v377_v), rt_s(&S292)))) {
    RT_RET(rt_s(&S48));
  }
  RT_RET(rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v377_v)) }));
}

static wv w5_getsAwayWithValue(wv v378_list) {
  rt_enter("getsAwayWithValue");
  wv v379_i = rt_i(0);
  while (rt_cond(({ wv t597 = v379_i; wv t598 = std_length(v378_list); rt_lt(t597, t598); }))) {
    wv v380_s = rt_bind(rt_idx(v379_i, v378_list), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_handler, v380_s), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v380_s)))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v380_s), rt_s(&S34)), "&&") && rt_bool(rt_ne(rt_get(&F_e, v380_s), RT_NULL), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v380_s), rt_s(&S41)), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v380_s))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v380_s), rt_s(&S184)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v380_s)), rt_s(&S193)), "&&")))) {
      wv v381_b = rt_i(0);
      while (rt_cond(({ wv t599 = v381_b; wv t600 = std_length(rt_get(&F_branches, rt_get(&F_e, v380_s))); rt_lt(t599, t600); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t601 = v381_b; wv t602 = rt_get(&F_branches, rt_get(&F_e, v380_s)); rt_idx(t601, t602); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v381_b = rt_add(v381_b, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v380_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v380_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v380_s), rt_s(&S184)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v380_s)), rt_s(&S51)), "&&")))) {
      wv v382_a = rt_i(0);
      while (rt_cond(({ wv t603 = v382_a; wv t604 = std_length(rt_get(&F_alibis, rt_get(&F_e, v380_s))); rt_lt(t603, t604); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t605 = v382_a; wv t606 = rt_get(&F_alibis, rt_get(&F_e, v380_s)); rt_idx(t605, t606); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v382_a = rt_add(v382_a, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v380_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v380_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    v379_i = rt_add(v379_i, rt_i(1));
  }
  RT_RET(rt_b(0));
}

static wv w5_lineIn(wv v383_chars, wv v384_pos) {
  rt_enter("lineIn");
  v383_chars = rt_bind(v383_chars, 0);
  v384_pos = rt_bind(v384_pos, 0);
  wv v385_line = rt_i(1);
  wv v386_k = rt_i(0);
  while (rt_cond(rt_lt(v386_k, v384_pos))) {
    if (rt_cond(rt_eq(rt_idx(v386_k, v383_chars), rt_s(&S2)))) {
      v385_line = rt_add(v385_line, rt_i(1));
    }
    v386_k = rt_add(v386_k, rt_i(1));
  }
  RT_RET(v385_line);
}

static wv w5_lineOf(wv v387_pos) {
  rt_enter("lineOf");
  v387_pos = rt_bind(v387_pos, 0);
  RT_RET(rt_chk(w5_lineIn(w2_srcChars, v387_pos), "lineOf"));
}

static wv w5_check(wv p_v388_modules, wv v389_requireExit) {
  rt_enter("check");
  struct fr101 fr = { 0 };
  fr.v388_modules = rt_bind(p_v388_modules, 0);
  v389_requireExit = rt_bind(v389_requireExit, 0);
  fr.v390_errors = rt_arr(0, NULL, 1);
  fr.v391_nextFn = rt_i(1);
  fr.v392_curMod = rt_i(0);
  fr.v393_schemes = rt_arr(0, NULL, 1);
  fr.v394_uses = rt_arr(0, NULL, 1);
  fr.v395_lineups = rt_arr(0, NULL, 1);
  fr.v428_topFn = rt_rec(0, 2, K45, (wv[]){ rt_i(0), rt_b(1) });
  wv v429_entry = rt_sub(std_length(fr.v388_modules), rt_i(1));
  wv v430_mi = rt_i(0);
  while (rt_cond(({ wv t1127 = v430_mi; wv t1128 = std_length(fr.v388_modules); rt_lt(t1127, t1128); }))) {
    (void)({ wv t1129 = v430_mi; wv t1130 = rt_eq(v430_mi, v429_entry); rt_chk(s427_checkModule(&fr, t1129, t1130), "check"); });
    v430_mi = rt_add(v430_mi, rt_i(1));
  }
  wv v431_si = rt_i(0);
  while (rt_cond(({ wv t1131 = v431_si; wv t1132 = std_length(fr.v393_schemes); rt_lt(t1131, t1132); }))) {
    wv v620_sc = rt_bind(rt_idx(v431_si, fr.v393_schemes), 0);
    wv v621_use = rt_bind(({ wv t1133 = rt_get(&F_mod, v620_sc); wv t1134 = rt_get(&F_namePos, v620_sc); rt_chk(s399_firstUse(&fr, t1133, t1134), "check"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v621_use, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v620_sc)), RT_NULL), "&&")), "&&") && rt_bool(rt_not(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v620_sc))), "check")), "&&")))) {
      (void)rt_chk(s397_enter(&fr, rt_get(&F_mod, v620_sc)), "check");
      wv v622_um = rt_bind(rt_get(&F_useMod, v621_use), 0);
      wv v623_where = rt_bind((rt_cond(({ wv t1135 = v622_um; wv t1136 = rt_get(&F_mod, v620_sc); rt_eq(t1135, t1136); })) ? rt_cat(2, (wv[]){ rt_s(&S449), rt_text(rt_chk(w5_lineOf(rt_get(&F_pos, v621_use)), "check")) }) : ({ wv t1139 = rt_text(rt_get(&F_file, rt_idx(v622_um, fr.v388_modules))); wv t1140 = rt_text(({ wv t1137 = rt_get(&F_chars, rt_idx(v622_um, fr.v388_modules)); wv t1138 = rt_get(&F_pos, v621_use); rt_chk(w5_lineIn(t1137, t1138), "check"); })); rt_cat(3, (wv[]){ t1139, rt_s(&S450), t1140 }); })), 0);
      (void)({ wv t1143 = rt_get(&F_namePos, v620_sc); wv t1144 = ({ wv t1141 = rt_text(rt_get(&F_name, v620_sc)); wv t1142 = rt_text(v623_where); rt_cat(5, (wv[]){ rt_s(&S111), t1141, rt_s(&S451), t1142, rt_s(&S16) }); }); rt_chk(s396_error(&fr, t1143, t1144, rt_s(&S452)), "check"); });
    }
    v431_si = rt_add(v431_si, rt_i(1));
  }
  (void)rt_chk(s397_enter(&fr, v429_entry), "check");
  wv v432_program = rt_bind(rt_get(&F_program, rt_idx(v429_entry, fr.v388_modules)), 0);
  if (rt_cond(v389_requireExit)) {
    wv v624_last = RT_NULL;
    wv v625_k = rt_i(0);
    while (rt_cond(({ wv t1145 = v625_k; wv t1146 = std_length(rt_get(&F_stmts, v432_program)); rt_lt(t1145, t1146); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1147 = v625_k; wv t1148 = rt_get(&F_stmts, v432_program); rt_idx(t1147, t1148); })), rt_s(&S207)))) {
        v624_last = rt_bind(({ wv t1149 = v625_k; wv t1150 = rt_get(&F_stmts, v432_program); rt_idx(t1149, t1150); }), 1);
      }
      v625_k = rt_add(v625_k, rt_i(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(v624_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v624_last), rt_s(&S185)), "||")))) {
      (void)rt_chk(s396_error(&fr, rt_get(&F_end, v432_program), rt_s(&S453), rt_s(&S454)), "check");
    }
  }
  RT_RET(fr.v390_errors);
}

static wv s396_error(struct fr101 *up, wv v433_pos, wv v434_message, wv v435_hint) {
  rt_enter("error");
  v433_pos = rt_bind(v433_pos, 0);
  v434_message = rt_bind(v434_message, 0);
  v435_hint = rt_bind(v435_hint, 0);
  (void)({ wv t607 = rt_def(up->v390_errors, "errors"); wv t608 = rt_chk(w2_err(v433_pos, v434_message, v435_hint), "error"); std_push(t607, t608); });
  RT_RET(RT_NULL);
}

static wv s397_enter(struct fr101 *up, wv v436_m) {
  rt_enter("enter");
  v436_m = rt_bind(v436_m, 0);
  wv t609 = rt_bind(v436_m, 1);
  (void)rt_def(up->v392_curMod, "curMod");
  up->v392_curMod = t609;
  (void)({ wv t610 = rt_get(&F_file, rt_idx(v436_m, up->v388_modules)); wv t611 = rt_get(&F_chars, rt_idx(v436_m, up->v388_modules)); rt_chk(w2_setSource(t610, t611), "enter"); });
  RT_RET(RT_NULL);
}

static wv s398_lineupMembers(struct fr101 *up, wv v437_d) {
  rt_enter("lineupMembers");
  v437_d = rt_bind(v437_d, 0);
  wv v438_k = rt_i(0);
  while (rt_cond(({ wv t612 = v438_k; wv t613 = std_length(rt_def(up->v395_lineups, "lineups")); rt_lt(t612, t613); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t616 = rt_get(&F_mod, ({ wv t614 = v438_k; wv t615 = rt_def(up->v395_lineups, "lineups"); rt_idx(t614, t615); })); wv t617 = rt_get(&F_mod, v437_d); rt_eq(t616, t617); }), "&&") && rt_bool(({ wv t620 = rt_get(&F_name, ({ wv t618 = v438_k; wv t619 = rt_def(up->v395_lineups, "lineups"); rt_idx(t618, t619); })); wv t621 = rt_get(&F_name, v437_d); rt_eq(t620, t621); }), "&&")))) {
      RT_RET(rt_get(&F_members, ({ wv t622 = v438_k; wv t623 = rt_def(up->v395_lineups, "lineups"); rt_idx(t622, t623); })));
    }
    v438_k = rt_add(v438_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s399_firstUse(struct fr101 *up, wv v439_mod, wv v440_declPos) {
  rt_enter("firstUse");
  v439_mod = rt_bind(v439_mod, 0);
  v440_declPos = rt_bind(v440_declPos, 0);
  wv v441_k = rt_i(0);
  while (rt_cond(({ wv t624 = v441_k; wv t625 = std_length(rt_def(up->v394_uses, "uses")); rt_lt(t624, t625); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t628 = rt_get(&F_mod, ({ wv t626 = v441_k; wv t627 = rt_def(up->v394_uses, "uses"); rt_idx(t626, t627); })); wv t629 = v439_mod; rt_eq(t628, t629); }), "&&") && rt_bool(({ wv t632 = rt_get(&F_namePos, ({ wv t630 = v441_k; wv t631 = rt_def(up->v394_uses, "uses"); rt_idx(t630, t631); })); wv t633 = v440_declPos; rt_eq(t632, t633); }), "&&")))) {
      RT_RET(({ wv t634 = v441_k; wv t635 = rt_def(up->v394_uses, "uses"); rt_idx(t634, t635); }));
    }
    v441_k = rt_add(v441_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s400_requireValue(struct fr101 *up, wv v442_b) {
  rt_enter("requireValue");
  v442_b = rt_bind(v442_b, 0);
  if (rt_cond(rt_ne(rt_get(&F_value, v442_b), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v443_last = RT_NULL;
  wv v444_k = rt_i(0);
  while (rt_cond(({ wv t636 = v444_k; wv t637 = std_length(rt_get(&F_stmts, v442_b)); rt_lt(t636, t637); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t638 = v444_k; wv t639 = rt_get(&F_stmts, v442_b); rt_idx(t638, t639); })), rt_s(&S207)))) {
      v443_last = rt_bind(({ wv t640 = v444_k; wv t641 = rt_get(&F_stmts, v442_b); rt_idx(t640, t641); }), 1);
    }
    v444_k = rt_add(v444_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_ne(v443_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t642 = rt_def(w5_neverCompletes, "neverCompletes"); wv t643 = rt_get(&F_k, v443_last); rt_chk(w1_contains(t642, t643), "requireValue"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v443_last), rt_s(&S184)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v443_last)), rt_s(&S192)), "&&")), "||")), "&&")))) {
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s396_error(up, rt_get(&F_close, v442_b), rt_s(&S370), rt_s(&S371)), "requireValue");
  RT_RET(RT_NULL);
}

static wv s401_newScope(struct fr101 *up, wv v445_parent) {
  rt_enter("newScope");
  v445_parent = rt_bind(v445_parent, 0);
  wv v446_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K41, (wv[]){ v445_parent, v446_names }));
}

static wv s402_decl(struct fr101 *up, wv v447_kind, wv v448_name, wv v449_mode, wv v450_namePos, wv v451_visibleAt, wv v452_fnId, wv v453_arity) {
  rt_enter("decl");
  v447_kind = rt_bind(v447_kind, 0);
  v448_name = rt_bind(v448_name, 0);
  v449_mode = rt_bind(v449_mode, 0);
  v450_namePos = rt_bind(v450_namePos, 0);
  v451_visibleAt = rt_bind(v451_visibleAt, 0);
  v452_fnId = rt_bind(v452_fnId, 0);
  v453_arity = rt_bind(v453_arity, 0);
  RT_RET(({ wv t644 = v447_kind; wv t645 = v448_name; wv t646 = rt_eq(v449_mode, rt_s(&S36)); wv t647 = rt_eq(v449_mode, rt_s(&S37)); wv t648 = v450_namePos; wv t649 = v451_visibleAt; wv t650 = v452_fnId; wv t651 = v453_arity; wv t652 = rt_def(up->v392_curMod, "curMod"); rt_rec(0, 10, K42, (wv[]){ t644, t645, t646, t647, t648, t649, t650, t651, t652, RT_NULL }); }));
}

static wv s403_own(struct fr101 *up, wv v454_scope, wv v455_name) {
  rt_enter("own");
  v455_name = rt_bind(v455_name, 0);
  wv v456_k = rt_i(0);
  while (rt_cond(({ wv t653 = v456_k; wv t654 = std_length(rt_get(&F_names, v454_scope)); rt_lt(t653, t654); }))) {
    if (rt_cond(({ wv t657 = rt_get(&F_name, ({ wv t655 = v456_k; wv t656 = rt_get(&F_names, v454_scope); rt_idx(t655, t656); })); wv t658 = v455_name; rt_eq(t657, t658); }))) {
      RT_RET(({ wv t659 = v456_k; wv t660 = rt_get(&F_names, v454_scope); rt_idx(t659, t660); }));
    }
    v456_k = rt_add(v456_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s404_declare(struct fr101 *up, wv v457_scope, wv v458_name, wv v459_d) {
  rt_enter("declare");
  v458_name = rt_bind(v458_name, 0);
  v459_d = rt_bind(v459_d, 0);
  wv v460_dup = rt_bind(rt_chk(s403_own(up, v457_scope, v458_name), "declare"), 0);
  if (rt_cond(rt_ne(v460_dup, RT_NULL))) {
    (void)({ wv t663 = rt_get(&F_namePos, v459_d); wv t664 = ({ wv t661 = rt_text(v458_name); wv t662 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v460_dup)), "declare")); rt_cat(5, (wv[]){ rt_s(&S111), t661, rt_s(&S372), t662, rt_s(&S16) }); }); rt_chk(s396_error(up, t663, t664, RT_NULL), "declare"); });
    RT_RET(RT_NULL);
  }
  wv v461_s = rt_get(&F_parent, v457_scope);
  while (rt_cond(rt_ne(v461_s, RT_NULL))) {
    wv v462_outer = rt_bind(rt_chk(s403_own(up, v461_s, v458_name), "declare"), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v462_outer, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_kind, v462_outer), rt_s(&S35)), "||") || rt_bool(({ wv t665 = rt_get(&F_namePos, v462_outer); wv t666 = rt_get(&F_namePos, v459_d); rt_lt(t665, t666); }), "||")), "&&")))) {
      (void)({ wv t670 = rt_get(&F_namePos, v459_d); wv t671 = ({ wv t667 = rt_text(v458_name); wv t668 = rt_text(v458_name); wv t669 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v462_outer)), "declare")); rt_cat(7, (wv[]){ rt_s(&S111), t667, rt_s(&S373), t668, rt_s(&S374), t669, rt_s(&S16) }); }); rt_chk(s396_error(up, t670, t671, rt_s(&S375)), "declare"); });
      break;
    }
    v461_s = rt_get(&F_parent, v461_s);
  }
  (void)({ wv t672 = rt_get(&F_names, v457_scope); wv t673 = v459_d; std_push(t672, t673); });
  RT_RET(RT_NULL);
}

static wv s405_lookup(struct fr101 *up, wv v463_scope, wv v464_name) {
  rt_enter("lookup");
  v464_name = rt_bind(v464_name, 0);
  wv v465_s = v463_scope;
  while (rt_cond(rt_ne(v465_s, RT_NULL))) {
    wv v466_d = rt_bind(rt_chk(s403_own(up, v465_s, v464_name), "lookup"), 0);
    if (rt_cond(rt_ne(v466_d, RT_NULL))) {
      RT_RET(v466_d);
    }
    v465_s = rt_get(&F_parent, v465_s);
  }
  RT_RET(RT_NULL);
}

static wv s406_ref(struct fr101 *up, wv v467_name, wv v468_pos, wv v469_scope, wv v470_ctx) {
  rt_enter("ref");
  v467_name = rt_bind(v467_name, 0);
  v468_pos = rt_bind(v468_pos, 0);
  v469_scope = rt_bind(v469_scope, 0);
  v470_ctx = rt_bind(v470_ctx, 0);
  wv v471_d = rt_bind(rt_chk(s405_lookup(up, v469_scope, v467_name), "ref"), 0);
  if (rt_cond(rt_eq(v471_d, RT_NULL))) {
    (void)({ wv t674 = v468_pos; wv t675 = rt_cat(3, (wv[]){ rt_s(&S376), rt_text(v467_name), rt_s(&S111) }); rt_chk(s396_error(up, t674, t675, RT_NULL), "ref"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v471_d), rt_s(&S35)), "&&") && rt_bool(({ wv t676 = rt_get(&F_fnId, v471_d); wv t677 = rt_get(&F_id, rt_get(&F_fn, v470_ctx)); rt_eq(t676, t677); }), "&&")), "&&") && rt_bool(({ wv t678 = v468_pos; wv t679 = rt_get(&F_visibleAt, v471_d); rt_lt(t678, t679); }), "&&")))) {
    (void)({ wv t682 = v468_pos; wv t683 = ({ wv t680 = rt_text(v467_name); wv t681 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v471_d)), "ref")); rt_cat(5, (wv[]){ rt_s(&S111), t680, rt_s(&S377), t681, rt_s(&S16) }); }); rt_chk(s396_error(up, t682, t683, RT_NULL), "ref"); });
  }
  RT_RET(v471_d);
}

static wv s407_block(struct fr101 *up, wv v472_b, wv v473_scope, wv v474_ctx) {
  rt_enter("block");
  v473_scope = rt_bind(v473_scope, 0);
  v474_ctx = rt_bind(v474_ctx, 0);
  wv v475_k = rt_i(0);
  while (rt_cond(({ wv t684 = v475_k; wv t685 = std_length(rt_get(&F_stmts, v472_b)); rt_lt(t684, t685); }))) {
    wv v476_s = rt_bind(({ wv t686 = v475_k; wv t687 = rt_get(&F_stmts, v472_b); rt_idx(t686, t687); }), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v476_s), rt_s(&S33)))) {
      wv v477_d = rt_bind(({ wv t688 = rt_get(&F_name, v476_s); wv t689 = rt_get(&F_namePos, v476_s); wv t690 = rt_get(&F_id, rt_get(&F_fn, v474_ctx)); wv t691 = std_length(rt_get(&F_params, v476_s)); rt_chk(s402_decl(up, rt_s(&S33), t688, rt_s(&S13), t689, RT_NULL, t690, t691), "block"); }), 0);
      (void)({ wv t692 = v473_scope; wv t693 = rt_get(&F_name, v476_s); wv t694 = v477_d; rt_chk(s404_declare(up, t692, t693, t694), "block"); });
      if (rt_cond(({ wv t697 = ({ wv t695 = v473_scope; wv t696 = rt_get(&F_name, v476_s); rt_chk(s403_own(up, t695, t696), "block"); }); wv t698 = v477_d; rt_eq(t697, t698); }))) {
        (void)({ wv t703 = rt_def(up->v393_schemes, "schemes"); wv t704 = ({ wv t699 = rt_def(up->v392_curMod, "curMod"); wv t700 = rt_get(&F_name, v476_s); wv t701 = rt_get(&F_namePos, v476_s); wv t702 = rt_get(&F_body, v476_s); rt_rec(0, 4, K43, (wv[]){ t699, t700, t701, t702 }); }); std_push(t703, t704); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v476_s), rt_s(&S53)))) {
      wv v478_d = rt_bind(({ wv t705 = rt_get(&F_name, v476_s); wv t706 = rt_get(&F_namePos, v476_s); wv t707 = rt_get(&F_id, rt_get(&F_fn, v474_ctx)); rt_chk(s402_decl(up, rt_s(&S53), t705, rt_s(&S13), t706, RT_NULL, t707, RT_NULL), "block"); }), 0);
      (void)({ wv t708 = v473_scope; wv t709 = rt_get(&F_name, v476_s); wv t710 = v478_d; rt_chk(s404_declare(up, t708, t709, t710), "block"); });
      if (rt_cond(({ wv t713 = ({ wv t711 = v473_scope; wv t712 = rt_get(&F_name, v476_s); rt_chk(s403_own(up, t711, t712), "block"); }); wv t714 = v478_d; rt_eq(t713, t714); }))) {
        (void)({ wv t718 = rt_def(up->v395_lineups, "lineups"); wv t719 = ({ wv t715 = rt_def(up->v392_curMod, "curMod"); wv t716 = rt_get(&F_name, v476_s); wv t717 = rt_get(&F_members, v476_s); rt_rec(0, 3, K44, (wv[]){ t715, t716, t717 }); }); std_push(t718, t719); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v476_s), rt_s(&S35)))) {
      (void)({ wv t725 = v473_scope; wv t726 = rt_get(&F_name, v476_s); wv t727 = ({ wv t720 = rt_get(&F_name, v476_s); wv t721 = rt_chk(w5_modeOf(v476_s), "block"); wv t722 = rt_get(&F_namePos, v476_s); wv t723 = rt_get(&F_end, v476_s); wv t724 = rt_get(&F_id, rt_get(&F_fn, v474_ctx)); rt_chk(s402_decl(up, rt_s(&S35), t720, t721, t722, t723, t724, RT_NULL), "block"); }); rt_chk(s404_declare(up, t725, t726, t727), "block"); });
    }
    v475_k = rt_add(v475_k, rt_i(1));
  }
  v475_k = rt_i(0);
  while (rt_cond(({ wv t728 = v475_k; wv t729 = std_length(rt_get(&F_stmts, v472_b)); rt_lt(t728, t729); }))) {
    (void)({ wv t732 = ({ wv t730 = v475_k; wv t731 = rt_get(&F_stmts, v472_b); rt_idx(t730, t731); }); wv t733 = v473_scope; wv t734 = v474_ctx; rt_chk(s408_stmt(up, t732, t733, t734), "block"); });
    v475_k = rt_add(v475_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v472_b), RT_NULL))) {
    (void)({ wv t735 = rt_get(&F_value, v472_b); wv t736 = v473_scope; wv t737 = v474_ctx; rt_chk(s423_expr(up, t735, t736, t737), "block"); });
  }
  RT_RET(RT_NULL);
}

static wv s408_stmt(struct fr101 *up, wv v479_s, wv v480_scope, wv v481_ctx) {
  rt_enter("stmt");
  v480_scope = rt_bind(v480_scope, 0);
  v481_ctx = rt_bind(v481_ctx, 0);
  wv v482_kind = rt_bind(rt_get(&F_k, v479_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v482_kind, rt_s(&S207)), "||") || rt_bool(rt_eq(v482_kind, rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(v482_kind, rt_s(&S54)), "||")), "||") || rt_bool(rt_eq(v482_kind, rt_s(&S55)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v482_kind, rt_s(&S33)))) {
    wv v483_fn = rt_rec(0, 2, K45, (wv[]){ rt_def(up->v391_nextFn, "nextFn"), rt_b(0) });
    up->v391_nextFn = rt_add(rt_def(up->v391_nextFn, "nextFn"), rt_i(1));
    wv v484_ps = rt_bind(rt_chk(s401_newScope(up, v480_scope), "stmt"), 0);
    wv v485_k = rt_i(0);
    while (rt_cond(({ wv t738 = v485_k; wv t739 = std_length(rt_get(&F_params, v479_s)); rt_lt(t738, t739); }))) {
      wv v486_p = rt_bind(({ wv t740 = v485_k; wv t741 = rt_get(&F_params, v479_s); rt_idx(t740, t741); }), 0);
      (void)({ wv t746 = v484_ps; wv t747 = rt_get(&F_name, v486_p); wv t748 = ({ wv t742 = rt_get(&F_name, v486_p); wv t743 = rt_chk(w5_modeOf(v486_p), "stmt"); wv t744 = rt_get(&F_pos, v486_p); wv t745 = rt_get(&F_id, v483_fn); rt_chk(s402_decl(up, rt_s(&S378), t742, t743, t744, RT_NULL, t745, RT_NULL), "stmt"); }); rt_chk(s404_declare(up, t746, t747, t748), "stmt"); });
      v485_k = rt_add(v485_k, rt_i(1));
    }
    (void)({ wv t749 = rt_get(&F_body, v479_s); wv t750 = rt_chk(s401_newScope(up, v484_ps), "stmt"); wv t751 = rt_rec(0, 2, K46, (wv[]){ v483_fn, rt_i(0) }); rt_chk(s407_block(up, t749, t750, t751), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v482_kind, rt_s(&S34)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_fn, v481_ctx)))) {
      (void)rt_chk(s396_error(up, rt_get(&F_pos, v479_s), rt_s(&S379), RT_NULL), "stmt");
    }
    if (rt_cond(rt_ne(rt_get(&F_e, v479_s), RT_NULL))) {
      (void)({ wv t752 = rt_get(&F_e, v479_s); wv t753 = v480_scope; wv t754 = v481_ctx; rt_chk(s423_expr(up, t752, t753, t754), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v482_kind, rt_s(&S44)))) {
    (void)({ wv t755 = rt_get(&F_e, v479_s); wv t756 = v480_scope; wv t757 = v481_ctx; rt_chk(s423_expr(up, t755, t756, t757), "stmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v482_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v482_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_get(&F_loops, v481_ctx), rt_i(0)))) {
      (void)({ wv t758 = rt_get(&F_pos, v479_s); wv t759 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v482_kind), rt_s(&S380) }); rt_chk(s396_error(up, t758, t759, RT_NULL), "stmt"); });
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v482_kind, rt_s(&S41)))) {
    (void)({ wv t760 = rt_get(&F_cond, v479_s); wv t761 = v480_scope; wv t762 = v481_ctx; rt_chk(s423_expr(up, t760, t761, t762), "stmt"); });
    (void)({ wv t765 = rt_get(&F_body, v479_s); wv t766 = rt_chk(s401_newScope(up, v480_scope), "stmt"); wv t767 = ({ wv t763 = rt_get(&F_fn, v481_ctx); wv t764 = rt_add(rt_get(&F_loops, v481_ctx), rt_i(1)); rt_rec(0, 2, K46, (wv[]){ t763, t764 }); }); rt_chk(s407_block(up, t765, t766, t767), "stmt"); });
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v482_kind, rt_s(&S211)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_fn, v481_ctx)))) {
      (void)rt_chk(s396_error(up, rt_get(&F_pos, v479_s), rt_s(&S381), RT_NULL), "stmt");
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v482_kind, rt_s(&S185)))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v479_s), RT_NULL))) {
      (void)({ wv t768 = rt_get(&F_e, v479_s); wv t769 = v480_scope; wv t770 = v481_ctx; rt_chk(s423_expr(up, t768, t769, t770), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v482_kind, rt_s(&S184)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v479_s)), rt_s(&S192)))) {
      if (rt_cond(rt_get(&F_top, rt_get(&F_fn, v481_ctx)))) {
        (void)rt_chk(s396_error(up, rt_get(&F_pos, rt_get(&F_e, v479_s)), rt_s(&S381), RT_NULL), "stmt");
      }
      (void)({ wv t771 = rt_get(&F_arg, rt_get(&F_e, v479_s)); wv t772 = v480_scope; wv t773 = v481_ctx; rt_chk(s423_expr(up, t771, t772, t773), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v479_s)), rt_s(&S193)))) {
      (void)({ wv t774 = rt_get(&F_e, v479_s); wv t775 = v480_scope; wv t776 = v481_ctx; rt_chk(s419_chain(up, t774, t775, t776, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v479_s)), rt_s(&S51)))) {
      (void)({ wv t777 = rt_get(&F_e, v479_s); wv t778 = v480_scope; wv t779 = v481_ctx; rt_chk(s410_checkSuspect(up, t777, t778, t779, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v479_s)), rt_s(&S191)))) {
      wv v487_k = rt_i(0);
      while (rt_cond(({ wv t780 = v487_k; wv t781 = std_length(rt_get(&F_args, rt_get(&F_e, v479_s))); rt_lt(t780, t781); }))) {
        (void)({ wv t784 = ({ wv t782 = v487_k; wv t783 = rt_get(&F_args, rt_get(&F_e, v479_s)); rt_idx(t782, t783); }); wv t785 = v480_scope; wv t786 = v481_ctx; rt_chk(s423_expr(up, t784, t785, t786), "stmt"); });
        v487_k = rt_add(v487_k, rt_i(1));
      }
      (void)({ wv t787 = rt_get(&F_e, v479_s); wv t788 = v480_scope; rt_chk(s424_callee(up, t787, t788, rt_b(0)), "stmt"); });
    } else {
      (void)({ wv t789 = rt_get(&F_e, v479_s); wv t790 = v480_scope; wv t791 = v481_ctx; rt_chk(s423_expr(up, t789, t790, t791), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v482_kind, rt_s(&S35)))) {
    (void)({ wv t792 = rt_get(&F_e, v479_s); wv t793 = v480_scope; wv t794 = v481_ctx; rt_chk(s423_expr(up, t792, t793, t794), "stmt"); });
  } else if (rt_cond(rt_eq(v482_kind, rt_s(&S38)))) {
    (void)({ wv t795 = rt_get(&F_e, v479_s); wv t796 = v480_scope; wv t797 = v481_ctx; rt_chk(s423_expr(up, t795, t796, t797), "stmt"); });
    (void)({ wv t798 = rt_get(&F_target, v479_s); wv t799 = v480_scope; wv t800 = v481_ctx; rt_chk(s420_lvalue(up, t798, t799, t800), "stmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v479_s), RT_NULL))) {
    (void)rt_chk(s409_whineHandler(up, v479_s, v480_scope, v481_ctx), "stmt");
  }
  RT_RET(RT_NULL);
}

static wv s409_whineHandler(struct fr101 *up, wv v488_s, wv v489_scope, wv v490_ctx) {
  rt_enter("whineHandler");
  v488_s = rt_bind(v488_s, 0);
  v489_scope = rt_bind(v489_scope, 0);
  v490_ctx = rt_bind(v490_ctx, 0);
  wv v491_h = rt_bind(rt_get(&F_handler, v488_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_e, v488_s), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v488_s)), rt_s(&S191)), "||")))) {
    (void)rt_chk(s396_error(up, rt_get(&F_pos, v491_h), rt_s(&S382), rt_s(&S383)), "whineHandler");
  }
  wv v492_hs = rt_bind(rt_chk(s401_newScope(up, v489_scope), "whineHandler"), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v491_h), RT_NULL))) {
    (void)({ wv t804 = v492_hs; wv t805 = rt_get(&F_param, v491_h); wv t806 = ({ wv t801 = rt_get(&F_param, v491_h); wv t802 = rt_get(&F_paramPos, v491_h); wv t803 = rt_get(&F_id, rt_get(&F_fn, v490_ctx)); rt_chk(s402_decl(up, rt_s(&S45), t801, rt_s(&S13), t802, RT_NULL, t803, RT_NULL), "whineHandler"); }); rt_chk(s404_declare(up, t804, t805, t806), "whineHandler"); });
  }
  (void)({ wv t807 = rt_get(&F_body, v491_h); wv t808 = rt_chk(s401_newScope(up, v492_hs), "whineHandler"); wv t809 = v490_ctx; rt_chk(s407_block(up, t807, t808, t809), "whineHandler"); });
  if (rt_cond(({ wv t810 = rt_def(w5_whineValueUsed, "whineValueUsed"); wv t811 = rt_get(&F_k, v488_s); rt_chk(w1_contains(t810, t811), "whineHandler"); }))) {
    (void)rt_chk(s400_requireValue(up, rt_get(&F_body, v491_h)), "whineHandler");
  }
  RT_RET(RT_NULL);
}

static wv s410_checkSuspect(struct fr101 *up, wv v493_e, wv v494_scope, wv v495_ctx, wv v496_stmtLevel) {
  rt_enter("checkSuspect");
  v493_e = rt_bind(v493_e, 0);
  v494_scope = rt_bind(v494_scope, 0);
  v495_ctx = rt_bind(v495_ctx, 0);
  v496_stmtLevel = rt_bind(v496_stmtLevel, 0);
  (void)({ wv t812 = rt_get(&F_subject, v493_e); wv t813 = v494_scope; wv t814 = v495_ctx; rt_chk(s423_expr(up, t812, t813, t814), "checkSuspect"); });
  wv v497_seen = rt_arr(0, NULL, 1);
  wv v498_sort = RT_NULL;
  wv v499_sortLineup = RT_NULL;
  wv v500_a = rt_i(0);
  while (rt_cond(({ wv t815 = v500_a; wv t816 = std_length(rt_get(&F_alibis, v493_e)); rt_lt(t815, t816); }))) {
    wv v501_al = rt_bind(({ wv t817 = v500_a; wv t818 = rt_get(&F_alibis, v493_e); rt_idx(t817, t818); }), 0);
    wv v502_j = rt_i(0);
    while (rt_cond(({ wv t819 = v502_j; wv t820 = std_length(rt_get(&F_values, v501_al)); rt_lt(t819, t820); }))) {
      wv v503_v = rt_bind(({ wv t821 = v502_j; wv t822 = rt_get(&F_values, v501_al); rt_idx(t821, t822); }), 0);
      wv v504_vs = rt_s(&S13);
      if (rt_cond(rt_eq(rt_get(&F_k, v503_v), rt_s(&S198)))) {
        wv v508_d = rt_bind(rt_chk(s417_alibiLineup(up, v503_v, v494_scope), "checkSuspect"), 0);
        v504_vs = rt_bind((rt_cond(rt_ne(v508_d, RT_NULL)) ? rt_chk(w5_lineupText(rt_get(&F_rec, v503_v)), "checkSuspect") : RT_NULL), 1);
        if (rt_cond(rt_eq(v498_sort, RT_NULL))) {
          v499_sortLineup = rt_bind(v508_d, 1);
        }
      }
      if (rt_cond(rt_eq(v498_sort, RT_NULL))) {
        v498_sort = rt_bind(v504_vs, 1);
      } else if (rt_cond(rt_b(rt_bool(rt_ne(v504_vs, RT_NULL), "&&") && rt_bool(rt_ne(v504_vs, v498_sort), "&&")))) {
        wv v509_message = rt_bind((rt_cond(rt_b(rt_bool(rt_eq(v498_sort, rt_s(&S13)), "||") || rt_bool(rt_eq(v504_vs, rt_s(&S13)), "||"))) ? rt_s(&S384) : ({ wv t823 = rt_text(v498_sort); wv t824 = rt_text(v504_vs); rt_cat(5, (wv[]){ rt_s(&S385), t823, rt_s(&S386), t824, rt_s(&S111) }); })), 0);
        (void)({ wv t825 = rt_get(&F_pos, v503_v); wv t826 = v509_message; rt_chk(s396_error(up, t825, t826, RT_NULL), "checkSuspect"); });
      }
      wv v505_key = rt_bind(rt_chk(w5_alibiKey(v503_v), "checkSuspect"), 0);
      wv v506_first = RT_NULL;
      wv v507_q = rt_i(0);
      while (rt_cond(({ wv t827 = v507_q; wv t828 = std_length(v497_seen); rt_lt(t827, t828); }))) {
        if (rt_cond(({ wv t829 = rt_get(&F_key, rt_idx(v507_q, v497_seen)); wv t830 = v505_key; rt_eq(t829, t830); }))) {
          v506_first = rt_bind(rt_get(&F_pos, rt_idx(v507_q, v497_seen)), 1);
        }
        v507_q = rt_add(v507_q, rt_i(1));
      }
      if (rt_cond(rt_ne(v506_first, RT_NULL))) {
        (void)({ wv t833 = rt_get(&F_pos, v503_v); wv t834 = ({ wv t831 = rt_text(rt_chk(w5_alibiText(v503_v), "checkSuspect")); wv t832 = rt_text(rt_chk(w5_lineOf(v506_first), "checkSuspect")); rt_cat(5, (wv[]){ rt_s(&S387), t831, rt_s(&S388), t832, rt_s(&S16) }); }); rt_chk(s396_error(up, t833, t834, RT_NULL), "checkSuspect"); });
      } else {
        (void)({ wv t837 = v497_seen; wv t838 = ({ wv t835 = v505_key; wv t836 = rt_get(&F_pos, v503_v); rt_rec(0, 2, K47, (wv[]){ t835, t836 }); }); std_push(t837, t838); });
      }
      v502_j = rt_add(v502_j, rt_i(1));
    }
    (void)({ wv t839 = rt_get(&F_body, v501_al); wv t840 = rt_chk(s401_newScope(up, v494_scope), "checkSuspect"); wv t841 = v495_ctx; rt_chk(s407_block(up, t839, t840, t841), "checkSuspect"); });
    if (rt_cond(rt_not(v496_stmtLevel))) {
      (void)rt_chk(s400_requireValue(up, rt_get(&F_body, v501_al)), "checkSuspect");
    }
    v500_a = rt_add(v500_a, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v493_e), RT_NULL))) {
    (void)({ wv t842 = rt_get(&F_els, v493_e); wv t843 = rt_chk(s401_newScope(up, v494_scope), "checkSuspect"); wv t844 = v495_ctx; rt_chk(s407_block(up, t842, t843, t844), "checkSuspect"); });
    if (rt_cond(rt_not(v496_stmtLevel))) {
      (void)rt_chk(s400_requireValue(up, rt_get(&F_els, v493_e)), "checkSuspect");
    }
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v498_sort, RT_NULL), "&&") && rt_bool(rt_ne(v498_sort, rt_s(&S13)), "&&")))) {
    wv v510_ms = rt_bind(rt_chk(s398_lineupMembers(up, v499_sortLineup), "checkSuspect"), 0);
    wv v511_missing = rt_arr(0, NULL, 1);
    wv v512_m = rt_i(0);
    while (rt_cond(({ wv t845 = v512_m; wv t846 = std_length(v510_ms); rt_lt(t845, t846); }))) {
      wv v513_key = ({ wv t847 = rt_text(rt_get(&F_name, rt_idx(v512_m, v510_ms))); wv t848 = rt_text(v498_sort); rt_cat(4, (wv[]){ rt_s(&S369), t847, rt_s(&S107), t848 }); });
      wv v514_handled = rt_b(0);
      wv v515_q = rt_i(0);
      while (rt_cond(({ wv t849 = v515_q; wv t850 = std_length(v497_seen); rt_lt(t849, t850); }))) {
        if (rt_cond(({ wv t851 = rt_get(&F_key, rt_idx(v515_q, v497_seen)); wv t852 = v513_key; rt_eq(t851, t852); }))) {
          v514_handled = rt_b(1);
        }
        v515_q = rt_add(v515_q, rt_i(1));
      }
      if (rt_cond(rt_not(v514_handled))) {
        (void)({ wv t855 = v511_missing; wv t856 = ({ wv t853 = rt_text(rt_get(&F_name, rt_idx(v512_m, v510_ms))); wv t854 = rt_text(v498_sort); rt_cat(5, (wv[]){ rt_s(&S111), t853, rt_s(&S107), t854, rt_s(&S111) }); }); std_push(t855, t856); });
      }
      v512_m = rt_add(v512_m, rt_i(1));
    }
    if (rt_cond(rt_gt(std_length(v511_missing), rt_i(0)))) {
      (void)({ wv t857 = rt_get(&F_pos, v493_e); wv t858 = rt_cat(2, (wv[]){ rt_s(&S389), rt_text(std_join(v511_missing, rt_s(&S390))) }); rt_chk(s396_error(up, t857, t858, rt_s(&S391)), "checkSuspect"); });
    }
  } else if (rt_cond(rt_not(v496_stmtLevel))) {
    (void)rt_chk(s396_error(up, rt_get(&F_pos, v493_e), rt_s(&S392), RT_NULL), "checkSuspect");
  }
  RT_RET(RT_NULL);
}

static wv s411_moduleNamed(struct fr101 *up, wv v516_x, wv v517_scope) {
  rt_enter("moduleNamed");
  v517_scope = rt_bind(v517_scope, 0);
  if (rt_cond(rt_ne(rt_get(&F_k, v516_x), rt_s(&S264)))) {
    RT_RET(RT_NULL);
  }
  wv v518_d = rt_bind(({ wv t859 = v517_scope; wv t860 = rt_get(&F_name, v516_x); rt_chk(s405_lookup(up, t859, t860), "moduleNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v518_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v518_d), rt_s(&S393)), "&&"))) ? v518_d : RT_NULL));
}

static wv s412_exported(struct fr101 *up, wv v519_lib, wv v520_name) {
  rt_enter("exported");
  v519_lib = rt_bind(v519_lib, 0);
  v520_name = rt_bind(v520_name, 0);
  wv v521_xs = rt_bind(rt_get(&F_exports, rt_idx(v519_lib, up->v388_modules)), 0);
  wv v522_k = rt_i(0);
  while (rt_cond(({ wv t861 = v522_k; wv t862 = std_length(v521_xs); rt_lt(t861, t862); }))) {
    if (rt_cond(({ wv t863 = rt_get(&F_name, rt_idx(v522_k, v521_xs)); wv t864 = v520_name; rt_eq(t863, t864); }))) {
      RT_RET(rt_idx(v522_k, v521_xs));
    }
    v522_k = rt_add(v522_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s413_lineupNamed(struct fr101 *up, wv v523_x, wv v524_scope) {
  rt_enter("lineupNamed");
  v524_scope = rt_bind(v524_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v523_x), rt_s(&S264)))) {
    wv v527_d = rt_bind(({ wv t865 = v524_scope; wv t866 = rt_get(&F_name, v523_x); rt_chk(s405_lookup(up, t865, t866), "lineupNamed"); }), 0);
    RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v527_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v527_d), rt_s(&S53)), "&&"))) ? v527_d : RT_NULL));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v523_x), rt_s(&S198)))) {
    RT_RET(RT_NULL);
  }
  wv v525_m = rt_bind(({ wv t867 = rt_get(&F_rec, v523_x); wv t868 = v524_scope; rt_chk(s411_moduleNamed(up, t867, t868), "lineupNamed"); }), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v525_m, RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_lib, v525_m), RT_NULL), "||")))) {
    RT_RET(RT_NULL);
  }
  wv v526_d = rt_bind(({ wv t869 = rt_get(&F_lib, v525_m); wv t870 = rt_get(&F_field, v523_x); rt_chk(s412_exported(up, t869, t870), "lineupNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v526_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v526_d), rt_s(&S53)), "&&"))) ? v526_d : RT_NULL));
}

static wv s414_bribed(struct fr101 *up, wv v528_e, wv v529_m) {
  rt_enter("bribed");
  v528_e = rt_bind(v528_e, 0);
  v529_m = rt_bind(v529_m, 0);
  if (rt_cond(rt_eq(rt_get(&F_lib, v529_m), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v530_d = rt_bind(({ wv t871 = rt_get(&F_lib, v529_m); wv t872 = rt_get(&F_field, v528_e); rt_chk(s412_exported(up, t871, t872), "bribed"); }), 0);
  if (rt_cond(rt_eq(v530_d, RT_NULL))) {
    (void)({ wv t877 = rt_get(&F_pos, v528_e); wv t878 = ({ wv t875 = rt_text(rt_get(&F_path, ({ wv t873 = rt_get(&F_lib, v529_m); wv t874 = up->v388_modules; rt_idx(t873, t874); }))); wv t876 = rt_text(rt_get(&F_field, v528_e)); rt_cat(5, (wv[]){ rt_s(&S111), t875, rt_s(&S394), t876, rt_s(&S111) }); }); rt_chk(s396_error(up, t877, t878, RT_NULL), "bribed"); });
  }
  RT_RET(v530_d);
}

static wv s415_unknownModule(struct fr101 *up, wv v531_x, wv v532_scope) {
  rt_enter("unknownModule");
  v531_x = rt_bind(v531_x, 0);
  v532_scope = rt_bind(v532_scope, 0);
  wv v533_message = rt_bind((rt_cond(rt_ne(({ wv t879 = v532_scope; wv t880 = rt_get(&F_name, v531_x); rt_chk(s405_lookup(up, t879, t880), "unknownModule"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_name, v531_x)), rt_s(&S395) }) : rt_cat(3, (wv[]){ rt_s(&S396), rt_text(rt_get(&F_name, v531_x)), rt_s(&S111) })), 0);
  (void)({ wv t881 = rt_get(&F_pos, v531_x); wv t882 = v533_message; rt_chk(s396_error(up, t881, t882, RT_NULL), "unknownModule"); });
  RT_RET(RT_NULL);
}

static wv s416_qualified(struct fr101 *up, wv v534_e, wv v535_m) {
  rt_enter("qualified");
  v534_e = rt_bind(v534_e, 0);
  v535_m = rt_bind(v535_m, 0);
  wv v536_d = rt_bind(rt_chk(s414_bribed(up, v534_e, v535_m), "qualified"), 0);
  wv v537_label = ({ wv t883 = rt_text(rt_get(&F_field, v534_e)); wv t884 = rt_text(rt_get(&F_name, rt_get(&F_rec, v534_e))); rt_cat(3, (wv[]){ t883, rt_s(&S107), t884 }); });
  if (rt_cond(rt_b(rt_bool(rt_ne(v536_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v536_d), rt_s(&S33)), "&&")))) {
    (void)({ wv t885 = rt_get(&F_pos, v534_e); wv t886 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v537_label), rt_s(&S397) }); wv t887 = rt_cat(3, (wv[]){ rt_s(&S398), rt_text(v537_label), rt_s(&S16) }); rt_chk(s396_error(up, t885, t886, t887), "qualified"); });
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v536_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v536_d), rt_s(&S53)), "&&")))) {
    (void)({ wv t888 = rt_get(&F_pos, v534_e); wv t889 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v537_label), rt_s(&S399) }); wv t890 = rt_cat(2, (wv[]){ rt_s(&S400), rt_text(v537_label) }); rt_chk(s396_error(up, t888, t889, t890), "qualified"); });
  }
  RT_RET(RT_NULL);
}

static wv s417_alibiLineup(struct fr101 *up, wv v538_v, wv v539_scope) {
  rt_enter("alibiLineup");
  v538_v = rt_bind(v538_v, 0);
  v539_scope = rt_bind(v539_scope, 0);
  wv v540_x = rt_bind(rt_get(&F_rec, v538_v), 0);
  wv v541_d = rt_bind(rt_chk(s413_lineupNamed(up, v540_x, v539_scope), "alibiLineup"), 0);
  if (rt_cond(rt_ne(v541_d, RT_NULL))) {
    (void)rt_chk(s418_member(up, v538_v, v541_d), "alibiLineup");
    RT_RET(v541_d);
  }
  wv v542_hint = rt_s(&S401);
  if (rt_cond(rt_eq(rt_get(&F_k, v540_x), rt_s(&S264)))) {
    wv v544_message = rt_bind((rt_cond(rt_ne(({ wv t891 = v539_scope; wv t892 = rt_get(&F_name, v540_x); rt_chk(s405_lookup(up, t891, t892), "alibiLineup"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_name, v540_x)), rt_s(&S402) }) : rt_cat(3, (wv[]){ rt_s(&S403), rt_text(rt_get(&F_name, v540_x)), rt_s(&S111) })), 0);
    (void)({ wv t893 = rt_get(&F_pos, v540_x); wv t894 = v544_message; wv t895 = v542_hint; rt_chk(s396_error(up, t893, t894, t895), "alibiLineup"); });
    RT_RET(RT_NULL);
  }
  wv v543_m = rt_bind(({ wv t896 = rt_get(&F_rec, v540_x); wv t897 = v539_scope; rt_chk(s411_moduleNamed(up, t896, t897), "alibiLineup"); }), 0);
  if (rt_cond(rt_eq(v543_m, RT_NULL))) {
    (void)({ wv t898 = rt_get(&F_rec, v540_x); wv t899 = v539_scope; rt_chk(s415_unknownModule(up, t898, t899), "alibiLineup"); });
  } else if (rt_cond(rt_ne(rt_chk(s414_bribed(up, v540_x, v543_m), "alibiLineup"), RT_NULL))) {
    (void)({ wv t900 = rt_get(&F_pos, v540_x); wv t901 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_chk(w5_lineupText(v540_x), "alibiLineup")), rt_s(&S402) }); wv t902 = v542_hint; rt_chk(s396_error(up, t900, t901, t902), "alibiLineup"); });
  }
  RT_RET(RT_NULL);
}

static wv s418_member(struct fr101 *up, wv v545_e, wv v546_d) {
  rt_enter("member");
  v545_e = rt_bind(v545_e, 0);
  v546_d = rt_bind(v546_d, 0);
  wv v547_ms = rt_bind(rt_chk(s398_lineupMembers(up, v546_d), "member"), 0);
  wv v548_names = rt_arr(0, NULL, 1);
  wv v549_found = rt_b(0);
  wv v550_k = rt_i(0);
  while (rt_cond(({ wv t903 = v550_k; wv t904 = std_length(v547_ms); rt_lt(t903, t904); }))) {
    (void)({ wv t905 = v548_names; wv t906 = rt_get(&F_name, rt_idx(v550_k, v547_ms)); std_push(t905, t906); });
    if (rt_cond(({ wv t907 = rt_get(&F_name, rt_idx(v550_k, v547_ms)); wv t908 = rt_get(&F_field, v545_e); rt_eq(t907, t908); }))) {
      v549_found = rt_b(1);
    }
    v550_k = rt_add(v550_k, rt_i(1));
  }
  if (rt_cond(rt_not(v549_found))) {
    (void)({ wv t911 = rt_get(&F_pos, v545_e); wv t912 = ({ wv t909 = rt_text(rt_get(&F_field, v545_e)); wv t910 = rt_text(rt_get(&F_name, v546_d)); rt_cat(5, (wv[]){ rt_s(&S404), t909, rt_s(&S405), t910, rt_s(&S111) }); }); wv t913 = rt_cat(2, (wv[]){ rt_s(&S406), rt_text(std_join(v548_names, rt_s(&S390))) }); rt_chk(s396_error(up, t911, t912, t913), "member"); });
  }
  RT_RET(RT_NULL);
}

static wv s419_chain(struct fr101 *up, wv v551_e, wv v552_scope, wv v553_ctx, wv v554_stmtLevel) {
  rt_enter("chain");
  v551_e = rt_bind(v551_e, 0);
  v552_scope = rt_bind(v552_scope, 0);
  v553_ctx = rt_bind(v553_ctx, 0);
  v554_stmtLevel = rt_bind(v554_stmtLevel, 0);
  wv v555_k = rt_i(0);
  while (rt_cond(({ wv t914 = v555_k; wv t915 = std_length(rt_get(&F_branches, v551_e)); rt_lt(t914, t915); }))) {
    wv v556_b = rt_bind(({ wv t916 = v555_k; wv t917 = rt_get(&F_branches, v551_e); rt_idx(t916, t917); }), 0);
    (void)({ wv t918 = rt_get(&F_cond, v556_b); wv t919 = v552_scope; wv t920 = v553_ctx; rt_chk(s423_expr(up, t918, t919, t920), "chain"); });
    (void)({ wv t921 = rt_get(&F_body, v556_b); wv t922 = rt_chk(s401_newScope(up, v552_scope), "chain"); wv t923 = v553_ctx; rt_chk(s407_block(up, t921, t922, t923), "chain"); });
    if (rt_cond(rt_not(v554_stmtLevel))) {
      (void)rt_chk(s400_requireValue(up, rt_get(&F_body, v556_b)), "chain");
    }
    v555_k = rt_add(v555_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v551_e), RT_NULL))) {
    (void)({ wv t924 = rt_get(&F_els, v551_e); wv t925 = rt_chk(s401_newScope(up, v552_scope), "chain"); wv t926 = v553_ctx; rt_chk(s407_block(up, t924, t925, t926), "chain"); });
    if (rt_cond(rt_not(v554_stmtLevel))) {
      (void)rt_chk(s400_requireValue(up, rt_get(&F_els, v551_e)), "chain");
    }
  } else if (rt_cond(rt_not(v554_stmtLevel))) {
    (void)rt_chk(s396_error(up, rt_get(&F_pos, v551_e), rt_s(&S407), RT_NULL), "chain");
  }
  RT_RET(RT_NULL);
}

static wv s420_lvalue(struct fr101 *up, wv v557_t, wv v558_scope, wv v559_ctx) {
  rt_enter("lvalue");
  v558_scope = rt_bind(v558_scope, 0);
  v559_ctx = rt_bind(v559_ctx, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v557_t), rt_s(&S264)))) {
    wv v563_d = rt_bind(({ wv t927 = rt_get(&F_name, v557_t); wv t928 = rt_get(&F_pos, v557_t); wv t929 = v558_scope; wv t930 = v559_ctx; rt_chk(s406_ref(up, t927, t928, t929, t930), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v563_d, RT_NULL))) {
      (void)({ wv t931 = v563_d; wv t932 = rt_get(&F_name, v557_t); wv t933 = rt_get(&F_pos, v557_t); wv t934 = rt_cat(3, (wv[]){ rt_s(&S408), rt_text(rt_get(&F_name, v557_t)), rt_s(&S409) }); rt_chk(s421_mustBeShifty(up, t931, t932, t933, t934, rt_b(1)), "lvalue"); });
      if (rt_cond(rt_get(&F_isFickle, v563_d))) {
        (void)({ wv t937 = rt_get(&F_fickleTargets, ({ wv t935 = rt_def(up->v392_curMod, "curMod"); wv t936 = up->v388_modules; rt_idx(t935, t936); })); wv t938 = rt_get(&F_pos, v557_t); std_push(t937, t938); });
      }
    }
    RT_RET(RT_NULL);
  }
  wv v560_isMem = rt_eq(rt_get(&F_k, v557_t), rt_s(&S198));
  wv v561_box = rt_bind((rt_cond(v560_isMem) ? rt_get(&F_rec, v557_t) : rt_get(&F_arr, v557_t)), 0);
  wv v562_what = rt_bind((rt_cond(v560_isMem) ? rt_s(&S410) : rt_s(&S411)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v557_t), rt_s(&S197)))) {
    (void)({ wv t939 = rt_get(&F_index, v557_t); wv t940 = v558_scope; wv t941 = v559_ctx; rt_chk(s423_expr(up, t939, t940, t941), "lvalue"); });
  }
  if (rt_cond(v560_isMem)) {
    if (rt_cond(rt_ne(rt_chk(s413_lineupNamed(up, v561_box, v558_scope), "lvalue"), RT_NULL))) {
      (void)({ wv t944 = rt_get(&F_pos, v557_t); wv t945 = ({ wv t942 = rt_text(rt_get(&F_field, v557_t)); wv t943 = rt_text(rt_chk(w5_lineupText(v561_box), "lvalue")); rt_cat(5, (wv[]){ rt_s(&S408), t942, rt_s(&S107), t943, rt_s(&S412) }); }); rt_chk(s396_error(up, t944, t945, RT_NULL), "lvalue"); });
      RT_RET(RT_NULL);
    }
    wv v564_m = rt_bind(rt_chk(s411_moduleNamed(up, v561_box, v558_scope), "lvalue"), 0);
    if (rt_cond(rt_ne(v564_m, RT_NULL))) {
      if (rt_cond(rt_ne(rt_chk(s414_bribed(up, v557_t, v564_m), "lvalue"), RT_NULL))) {
        (void)({ wv t951 = rt_get(&F_pos, v557_t); wv t952 = ({ wv t948 = rt_text(rt_get(&F_field, v557_t)); wv t949 = rt_text(rt_get(&F_name, v561_box)); wv t950 = rt_text(rt_get(&F_path, ({ wv t946 = rt_get(&F_lib, v564_m); wv t947 = up->v388_modules; rt_idx(t946, t947); }))); rt_cat(7, (wv[]){ rt_s(&S408), t948, rt_s(&S107), t949, rt_s(&S413), t950, rt_s(&S414) }); }); rt_chk(s396_error(up, t951, t952, RT_NULL), "lvalue"); });
      }
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v561_box), rt_s(&S264)))) {
    wv v565_d = rt_bind(({ wv t953 = rt_get(&F_name, v561_box); wv t954 = rt_get(&F_pos, v561_box); wv t955 = v558_scope; wv t956 = v559_ctx; rt_chk(s406_ref(up, t953, t954, t955, t956), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v565_d, RT_NULL))) {
      (void)({ wv t959 = v565_d; wv t960 = rt_get(&F_name, v561_box); wv t961 = rt_get(&F_pos, v561_box); wv t962 = ({ wv t957 = rt_text(v562_what); wv t958 = rt_text(rt_get(&F_name, v561_box)); rt_cat(5, (wv[]){ rt_s(&S415), t957, rt_s(&S416), t958, rt_s(&S417) }); }); rt_chk(s421_mustBeShifty(up, t959, t960, t961, t962, rt_b(0)), "lvalue"); });
    }
  } else {
    (void)rt_chk(s423_expr(up, v561_box, v558_scope, v559_ctx), "lvalue");
  }
  RT_RET(RT_NULL);
}

static wv s421_mustBeShifty(struct fr101 *up, wv v566_d, wv v567_name, wv v568_pos, wv v569_message, wv v570_rebind) {
  rt_enter("mustBeShifty");
  v566_d = rt_bind(v566_d, 0);
  v567_name = rt_bind(v567_name, 0);
  v568_pos = rt_bind(v568_pos, 0);
  v569_message = rt_bind(v569_message, 0);
  v570_rebind = rt_bind(v570_rebind, 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v566_d), rt_s(&S33)), "||") || rt_bool(rt_eq(rt_get(&F_kind, v566_d), rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_kind, v566_d), rt_s(&S393)), "||")))) {
    (void)({ wv t965 = v568_pos; wv t966 = ({ wv t963 = rt_text(v567_name); wv t964 = rt_text(rt_get(&F_kind, v566_d)); rt_cat(5, (wv[]){ rt_s(&S111), t963, rt_s(&S418), t964, rt_s(&S419) }); }); rt_chk(s396_error(up, t965, t966, RT_NULL), "mustBeShifty"); });
  } else if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_isShifty, v566_d)), "&&") && rt_bool(rt_not(rt_get(&F_isFickle, v566_d)), "&&")))) {
    wv v571_hint = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_kind, v566_d), rt_s(&S378)))) {
      v571_hint = rt_bind((rt_cond(v570_rebind) ? ({ wv t967 = rt_text(v567_name); wv t968 = rt_text(v567_name); rt_cat(5, (wv[]){ rt_s(&S420), t967, rt_s(&S421), t968, rt_s(&S422) }); }) : rt_cat(3, (wv[]){ rt_s(&S423), rt_text(v567_name), rt_s(&S16) })), 1);
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v566_d), rt_s(&S35)))) {
      v571_hint = rt_bind((rt_cond(v570_rebind) ? ({ wv t969 = rt_text(v567_name); wv t970 = rt_text(v567_name); rt_cat(5, (wv[]){ rt_s(&S424), t969, rt_s(&S425), t970, rt_s(&S426) }); }) : rt_cat(3, (wv[]){ rt_s(&S427), rt_text(v567_name), rt_s(&S111) })), 1);
    }
    (void)rt_chk(s396_error(up, v568_pos, v569_message, v571_hint), "mustBeShifty");
  }
  RT_RET(RT_NULL);
}

static wv s422_unknownStd(struct fr101 *up, wv v572_node) {
  rt_enter("unknownStd");
  v572_node = rt_bind(v572_node, 0);
  (void)({ wv t971 = rt_get(&F_pos, v572_node); wv t972 = rt_cat(3, (wv[]){ rt_s(&S428), rt_text(rt_get(&F_name, v572_node)), rt_s(&S111) }); rt_chk(s396_error(up, t971, t972, rt_s(&S148)), "unknownStd"); });
  RT_RET(RT_NULL);
}

static wv s423_expr(struct fr101 *up, wv v573_e, wv v574_scope, wv v575_ctx) {
  rt_enter("expr");
  v574_scope = rt_bind(v574_scope, 0);
  v575_ctx = rt_bind(v575_ctx, 0);
  wv v576_kind = rt_bind(rt_get(&F_k, v573_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v576_kind, rt_s(&S106)), "||") || rt_bool(rt_eq(v576_kind, rt_s(&S109)), "||")), "||") || rt_bool(rt_eq(v576_kind, rt_s(&S291)), "||")), "||") || rt_bool(rt_eq(v576_kind, rt_s(&S292)), "||")), "||") || rt_bool(rt_eq(v576_kind, rt_s(&S293)), "||")), "||") || rt_bool(rt_eq(v576_kind, rt_s(&S294)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S179)))) {
    wv v577_k = rt_i(0);
    while (rt_cond(({ wv t973 = v577_k; wv t974 = std_length(rt_get(&F_parts, v573_e)); rt_lt(t973, t974); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t975 = v577_k; wv t976 = rt_get(&F_parts, v573_e); rt_idx(t975, t976); })), rt_s(&S194)))) {
        (void)({ wv t979 = ({ wv t977 = v577_k; wv t978 = rt_get(&F_parts, v573_e); rt_idx(t977, t978); }); wv t980 = v574_scope; wv t981 = v575_ctx; rt_chk(s423_expr(up, t979, t980, t981), "expr"); });
      }
      v577_k = rt_add(v577_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S264)))) {
    wv v578_d = rt_bind(({ wv t982 = rt_get(&F_name, v573_e); wv t983 = rt_get(&F_pos, v573_e); wv t984 = v574_scope; wv t985 = v575_ctx; rt_chk(s406_ref(up, t982, t983, t984, t985), "expr"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v578_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v578_d), rt_s(&S33)), "&&")))) {
      (void)({ wv t986 = rt_get(&F_pos, v573_e); wv t987 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_name, v573_e)), rt_s(&S397) }); wv t988 = rt_cat(3, (wv[]){ rt_s(&S398), rt_text(rt_get(&F_name, v573_e)), rt_s(&S16) }); rt_chk(s396_error(up, t986, t987, t988), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v578_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v578_d), rt_s(&S53)), "&&")))) {
      (void)({ wv t989 = rt_get(&F_pos, v573_e); wv t990 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_name, v573_e)), rt_s(&S399) }); wv t991 = rt_cat(2, (wv[]){ rt_s(&S400), rt_text(rt_get(&F_name, v573_e)) }); rt_chk(s396_error(up, t989, t990, t991), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v578_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v578_d), rt_s(&S393)), "&&")))) {
      (void)({ wv t992 = rt_get(&F_pos, v573_e); wv t993 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(rt_get(&F_name, v573_e)), rt_s(&S429) }); wv t994 = rt_cat(2, (wv[]){ rt_s(&S430), rt_text(rt_get(&F_name, v573_e)) }); rt_chk(s396_error(up, t992, t993, t994), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S164)))) {
    wv v579_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v573_e)), "expr"), 0);
    if (rt_cond(rt_eq(v579_sig, RT_NULL))) {
      (void)rt_chk(s422_unknownStd(up, v573_e), "expr");
    } else if (rt_cond(rt_ne(v579_sig, rt_s(&S340)))) {
      (void)({ wv t997 = rt_get(&F_pos, v573_e); wv t998 = ({ wv t995 = rt_text(rt_get(&F_name, v573_e)); wv t996 = rt_text(rt_get(&F_name, v573_e)); rt_cat(5, (wv[]){ rt_s(&S188), t995, rt_s(&S431), t996, rt_s(&S16) }); }); rt_chk(s396_error(up, t997, t998, RT_NULL), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S195)))) {
    wv v580_k = rt_i(0);
    while (rt_cond(({ wv t999 = v580_k; wv t1000 = std_length(rt_get(&F_items, v573_e)); rt_lt(t999, t1000); }))) {
      (void)({ wv t1003 = ({ wv t1001 = v580_k; wv t1002 = rt_get(&F_items, v573_e); rt_idx(t1001, t1002); }); wv t1004 = v574_scope; wv t1005 = v575_ctx; rt_chk(s423_expr(up, t1003, t1004, t1005), "expr"); });
      v580_k = rt_add(v580_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S196)))) {
    wv v581_k = rt_i(0);
    while (rt_cond(({ wv t1006 = v581_k; wv t1007 = std_length(rt_get(&F_fields, v573_e)); rt_lt(t1006, t1007); }))) {
      (void)({ wv t1010 = rt_get(&F_e, ({ wv t1008 = v581_k; wv t1009 = rt_get(&F_fields, v573_e); rt_idx(t1008, t1009); })); wv t1011 = v574_scope; wv t1012 = v575_ctx; rt_chk(s423_expr(up, t1010, t1011, t1012), "expr"); });
      v581_k = rt_add(v581_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S197)))) {
    (void)({ wv t1013 = rt_get(&F_index, v573_e); wv t1014 = v574_scope; wv t1015 = v575_ctx; rt_chk(s423_expr(up, t1013, t1014, t1015), "expr"); });
    (void)({ wv t1016 = rt_get(&F_arr, v573_e); wv t1017 = v574_scope; wv t1018 = v575_ctx; rt_chk(s423_expr(up, t1016, t1017, t1018), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S198)))) {
    wv v582_lin = rt_bind(({ wv t1019 = rt_get(&F_rec, v573_e); wv t1020 = v574_scope; rt_chk(s413_lineupNamed(up, t1019, t1020), "expr"); }), 0);
    if (rt_cond(rt_ne(v582_lin, RT_NULL))) {
      (void)rt_chk(s418_member(up, v573_e, v582_lin), "expr");
      RT_RET(RT_NULL);
    }
    wv v583_m = rt_bind(({ wv t1021 = rt_get(&F_rec, v573_e); wv t1022 = v574_scope; rt_chk(s411_moduleNamed(up, t1021, t1022), "expr"); }), 0);
    if (rt_cond(rt_ne(v583_m, RT_NULL))) {
      (void)rt_chk(s416_qualified(up, v573_e, v583_m), "expr");
    } else {
      (void)({ wv t1023 = rt_get(&F_rec, v573_e); wv t1024 = v574_scope; wv t1025 = v575_ctx; rt_chk(s423_expr(up, t1023, t1024, t1025), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S199)))) {
    (void)({ wv t1026 = rt_get(&F_e, v573_e); wv t1027 = v574_scope; wv t1028 = v575_ctx; rt_chk(s423_expr(up, t1026, t1027, t1028), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S200)))) {
    (void)({ wv t1029 = rt_get(&F_l, v573_e); wv t1030 = v574_scope; wv t1031 = v575_ctx; rt_chk(s423_expr(up, t1029, t1030, t1031), "expr"); });
    (void)({ wv t1032 = rt_get(&F_r, v573_e); wv t1033 = v574_scope; wv t1034 = v575_ctx; rt_chk(s423_expr(up, t1032, t1033, t1034), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S201)))) {
    (void)({ wv t1035 = rt_get(&F_c, v573_e); wv t1036 = v574_scope; wv t1037 = v575_ctx; rt_chk(s423_expr(up, t1035, t1036, t1037), "expr"); });
    (void)({ wv t1038 = rt_get(&F_a, v573_e); wv t1039 = v574_scope; wv t1040 = v575_ctx; rt_chk(s423_expr(up, t1038, t1039, t1040), "expr"); });
    (void)({ wv t1041 = rt_get(&F_b, v573_e); wv t1042 = v574_scope; wv t1043 = v575_ctx; rt_chk(s423_expr(up, t1041, t1042, t1043), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S191)))) {
    wv v584_k = rt_i(0);
    while (rt_cond(({ wv t1044 = v584_k; wv t1045 = std_length(rt_get(&F_args, v573_e)); rt_lt(t1044, t1045); }))) {
      (void)({ wv t1048 = ({ wv t1046 = v584_k; wv t1047 = rt_get(&F_args, v573_e); rt_idx(t1046, t1047); }); wv t1049 = v574_scope; wv t1050 = v575_ctx; rt_chk(s423_expr(up, t1048, t1049, t1050), "expr"); });
      v584_k = rt_add(v584_k, rt_i(1));
    }
    (void)rt_chk(s424_callee(up, v573_e, v574_scope, rt_b(1)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S192)))) {
    (void)rt_chk(s396_error(up, rt_get(&F_pos, v573_e), rt_s(&S432), RT_NULL), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S51)))) {
    (void)rt_chk(s410_checkSuspect(up, v573_e, v574_scope, v575_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v576_kind, rt_s(&S193)))) {
    (void)rt_chk(s419_chain(up, v573_e, v574_scope, v575_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  rt_tantrum(rt_cat(2, (wv[]){ rt_s(&S433), rt_text(v576_kind) }));
  RT_RET(RT_NULL);
}

static wv s424_callee(struct fr101 *up, wv v585_e, wv v586_scope, wv v587_used) {
  rt_enter("callee");
  v585_e = rt_bind(v585_e, 0);
  v586_scope = rt_bind(v586_scope, 0);
  v587_used = rt_bind(v587_used, 0);
  wv v588_c = rt_bind(rt_get(&F_callee, v585_e), 0);
  wv v589_n = rt_bind(std_length(rt_get(&F_args, v585_e)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v588_c), rt_s(&S164)))) {
    wv v592_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v588_c)), "callee"), 0);
    if (rt_cond(rt_eq(v592_sig, RT_NULL))) {
      (void)rt_chk(s422_unknownStd(up, v588_c), "callee");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(v592_sig, rt_s(&S340)))) {
      (void)({ wv t1051 = rt_get(&F_pos, v588_c); wv t1052 = rt_cat(3, (wv[]){ rt_s(&S188), rt_text(rt_get(&F_name, v588_c)), rt_s(&S434) }); rt_chk(s396_error(up, t1051, t1052, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_ne(v592_sig, v589_n))) {
      (void)({ wv t1056 = rt_get(&F_pos, v588_c); wv t1057 = ({ wv t1053 = rt_text(rt_get(&F_name, v588_c)); wv t1054 = rt_text(rt_chk(w5_plural(v592_sig), "callee")); wv t1055 = rt_text(v589_n); rt_cat(6, (wv[]){ rt_s(&S188), t1053, rt_s(&S435), t1054, rt_s(&S436), t1055 }); }); rt_chk(s396_error(up, t1056, t1057, RT_NULL), "callee"); });
    }
    RT_RET(RT_NULL);
  }
  wv v590_d = RT_NULL;
  wv v591_label = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v588_c), rt_s(&S198)))) {
    v591_label = ({ wv t1058 = rt_text(rt_get(&F_field, v588_c)); wv t1059 = rt_text(rt_get(&F_name, rt_get(&F_rec, v588_c))); rt_cat(3, (wv[]){ t1058, rt_s(&S107), t1059 }); });
    wv v593_m = rt_bind(({ wv t1060 = rt_get(&F_rec, v588_c); wv t1061 = v586_scope; rt_chk(s411_moduleNamed(up, t1060, t1061), "callee"); }), 0);
    if (rt_cond(rt_eq(v593_m, RT_NULL))) {
      (void)({ wv t1062 = rt_get(&F_rec, v588_c); wv t1063 = v586_scope; rt_chk(s415_unknownModule(up, t1062, t1063), "callee"); });
      RT_RET(RT_NULL);
    }
    v590_d = rt_chk(s414_bribed(up, v588_c, v593_m), "callee");
    if (rt_cond(rt_eq(v590_d, RT_NULL))) {
      RT_RET(RT_NULL);
    }
  } else {
    v591_label = rt_bind(rt_get(&F_name, v588_c), 1);
    v590_d = ({ wv t1064 = v586_scope; wv t1065 = rt_get(&F_name, v588_c); rt_chk(s405_lookup(up, t1064, t1065), "callee"); });
    if (rt_cond(rt_eq(v590_d, RT_NULL))) {
      (void)({ wv t1066 = rt_get(&F_pos, v588_c); wv t1067 = rt_cat(3, (wv[]){ rt_s(&S437), rt_text(rt_get(&F_name, v588_c)), rt_s(&S111) }); rt_chk(s396_error(up, t1066, t1067, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_ne(rt_get(&F_kind, v590_d), rt_s(&S33)))) {
    (void)({ wv t1068 = rt_get(&F_pos, v588_c); wv t1069 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v591_label), rt_s(&S438) }); rt_chk(s396_error(up, t1068, t1069, rt_s(&S439)), "callee"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(({ wv t1070 = rt_get(&F_arity, v590_d); wv t1071 = v589_n; rt_ne(t1070, t1071); }))) {
    (void)({ wv t1075 = rt_get(&F_pos, v588_c); wv t1076 = ({ wv t1072 = rt_text(v591_label); wv t1073 = rt_text(rt_chk(w5_plural(rt_get(&F_arity, v590_d)), "callee")); wv t1074 = rt_text(v589_n); rt_cat(6, (wv[]){ rt_s(&S111), t1072, rt_s(&S435), t1073, rt_s(&S436), t1074 }); }); rt_chk(s396_error(up, t1075, t1076, RT_NULL), "callee"); });
  }
  if (rt_cond(rt_b(rt_bool(v587_used, "&&") && rt_bool(rt_eq(({ wv t1077 = rt_get(&F_mod, v590_d); wv t1078 = rt_get(&F_namePos, v590_d); rt_chk(s399_firstUse(up, t1077, t1078), "callee"); }), RT_NULL), "&&")))) {
    (void)({ wv t1083 = rt_def(up->v394_uses, "uses"); wv t1084 = ({ wv t1079 = rt_get(&F_mod, v590_d); wv t1080 = rt_get(&F_namePos, v590_d); wv t1081 = rt_get(&F_pos, v588_c); wv t1082 = rt_def(up->v392_curMod, "curMod"); rt_rec(0, 4, K48, (wv[]){ t1079, t1080, t1081, t1082 }); }); std_push(t1083, t1084); });
  }
  RT_RET(RT_NULL);
}

static wv s425_stealNames(struct fr101 *up, wv v594_m, wv v595_scope) {
  rt_enter("stealNames");
  v594_m = rt_bind(v594_m, 0);
  v595_scope = rt_bind(v595_scope, 0);
  wv v596_steals = rt_bind(rt_get(&F_steals, rt_idx(v594_m, up->v388_modules)), 0);
  wv v597_seen = rt_arr(0, NULL, 1);
  wv v598_k = rt_i(0);
  while (rt_cond(({ wv t1085 = v598_k; wv t1086 = std_length(v596_steals); rt_lt(t1085, t1086); }))) {
    wv v599_node = rt_bind(rt_get(&F_node, rt_idx(v598_k, v596_steals)), 0);
    wv v600_lib = rt_bind(rt_get(&F_lib, rt_idx(v598_k, v596_steals)), 0);
    v598_k = rt_add(v598_k, rt_i(1));
    wv v601_path = rt_bind(rt_get(&F_path, rt_idx(v600_lib, up->v388_modules)), 0);
    wv v602_usable = rt_bind(v600_lib, 1);
    if (rt_cond(rt_chk(w1_contains(v597_seen, v600_lib), "stealNames"))) {
      (void)({ wv t1087 = rt_get(&F_pathPos, v599_node); wv t1088 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v601_path), rt_s(&S440) }); rt_chk(s396_error(up, t1087, t1088, RT_NULL), "stealNames"); });
      v602_usable = RT_NULL;
    } else if (rt_cond(rt_get(&F_notLibrary, rt_idx(v600_lib, up->v388_modules)))) {
      (void)({ wv t1089 = rt_get(&F_pathPos, v599_node); wv t1090 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v601_path), rt_s(&S441) }); rt_chk(s396_error(up, t1089, t1090, RT_NULL), "stealNames"); });
      v602_usable = RT_NULL;
    }
    (void)std_push(v597_seen, v600_lib);
    wv v603_d = ({ wv t1091 = rt_get(&F_name, v599_node); wv t1092 = rt_get(&F_namePos, v599_node); wv t1093 = rt_def(up->v392_curMod, "curMod"); wv t1094 = v602_usable; rt_rec(0, 10, K42, (wv[]){ rt_s(&S393), t1091, rt_b(0), rt_b(0), t1092, RT_NULL, RT_NULL, RT_NULL, t1093, t1094 }); });
    (void)({ wv t1095 = v595_scope; wv t1096 = rt_get(&F_name, v599_node); wv t1097 = v603_d; rt_chk(s404_declare(up, t1095, t1096, t1097), "stealNames"); });
  }
  RT_RET(RT_NULL);
}

static wv s426_libraryRules(struct fr101 *up, wv v604_m, wv v605_scope, wv v606_isEntry, wv v607_last) {
  rt_enter("libraryRules");
  v604_m = rt_bind(v604_m, 0);
  v605_scope = rt_bind(v605_scope, 0);
  v606_isEntry = rt_bind(v606_isEntry, 0);
  v607_last = rt_bind(v607_last, 0);
  wv v608_program = rt_bind(rt_get(&F_program, rt_idx(v604_m, up->v388_modules)), 0);
  wv v609_k = rt_i(0);
  while (rt_cond(({ wv t1098 = v609_k; wv t1099 = std_length(rt_get(&F_stmts, v608_program)); rt_lt(t1098, t1099); }))) {
    wv v611_s = rt_bind(({ wv t1100 = v609_k; wv t1101 = rt_get(&F_stmts, v608_program); rt_idx(t1100, t1101); }), 0);
    v609_k = rt_add(v609_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v611_s), rt_s(&S55)), "&&") && rt_bool(rt_b(rt_bool(v606_isEntry, "||") || rt_bool(({ wv t1102 = rt_get(&F_pos, v611_s); wv t1103 = rt_get(&F_pos, v607_last); rt_ne(t1102, t1103); }), "||")), "&&")))) {
      (void)rt_chk(s396_error(up, rt_get(&F_pos, v611_s), rt_s(&S442), RT_NULL), "libraryRules");
    } else if (rt_cond(rt_b(rt_bool(rt_not(v606_isEntry), "&&") && rt_bool(rt_not(({ wv t1104 = rt_def(w5_libraryTop, "libraryTop"); wv t1105 = rt_get(&F_k, v611_s); rt_chk(w1_contains(t1104, t1105), "libraryRules"); })), "&&")))) {
      (void)rt_chk(s396_error(up, rt_get(&F_pos, v611_s), rt_s(&S443), rt_s(&S444)), "libraryRules");
    }
  }
  if (rt_cond(v606_isEntry)) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v607_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v607_last), rt_s(&S55)), "||")))) {
    (void)rt_chk(s396_error(up, rt_get(&F_end, v608_program), rt_s(&S445), rt_s(&S218)), "libraryRules");
    RT_RET(RT_NULL);
  }
  wv v610_exports = rt_arr(0, NULL, 1);
  v609_k = rt_i(0);
  while (rt_cond(({ wv t1106 = v609_k; wv t1107 = std_length(rt_get(&F_names, v607_last)); rt_lt(t1106, t1107); }))) {
    wv v612_n = rt_bind(({ wv t1108 = v609_k; wv t1109 = rt_get(&F_names, v607_last); rt_idx(t1108, t1109); }), 0);
    v609_k = rt_add(v609_k, rt_i(1));
    wv v613_d = rt_bind(({ wv t1110 = v605_scope; wv t1111 = rt_get(&F_name, v612_n); rt_chk(s403_own(up, t1110, t1111), "libraryRules"); }), 0);
    if (rt_cond(rt_eq(v613_d, RT_NULL))) {
      (void)({ wv t1112 = rt_get(&F_pos, v612_n); wv t1113 = rt_cat(3, (wv[]){ rt_s(&S446), rt_text(rt_get(&F_name, v612_n)), rt_s(&S447) }); rt_chk(s396_error(up, t1112, t1113, RT_NULL), "libraryRules"); });
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v613_d), rt_s(&S393)))) {
      (void)({ wv t1114 = rt_get(&F_pos, v612_n); wv t1115 = rt_cat(3, (wv[]){ rt_s(&S446), rt_text(rt_get(&F_name, v612_n)), rt_s(&S448) }); rt_chk(s396_error(up, t1114, t1115, RT_NULL), "libraryRules"); });
    } else {
      (void)std_push(v610_exports, v613_d);
    }
  }
  ({ wv t1116 = v610_exports; wv t1117 = rt_idx(v604_m, up->v388_modules); rt_set_mem(t1116, &F_exports, t1117); });
  RT_RET(RT_NULL);
}

static wv s427_checkModule(struct fr101 *up, wv v614_m, wv v615_isEntry) {
  rt_enter("checkModule");
  v614_m = rt_bind(v614_m, 0);
  v615_isEntry = rt_bind(v615_isEntry, 0);
  (void)rt_chk(s397_enter(up, v614_m), "checkModule");
  wv v616_program = rt_bind(rt_get(&F_program, rt_idx(v614_m, up->v388_modules)), 0);
  wv v617_last = RT_NULL;
  wv v618_k = rt_i(0);
  while (rt_cond(({ wv t1118 = v618_k; wv t1119 = std_length(rt_get(&F_stmts, v616_program)); rt_lt(t1118, t1119); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1120 = v618_k; wv t1121 = rt_get(&F_stmts, v616_program); rt_idx(t1120, t1121); })), rt_s(&S207)))) {
      v617_last = rt_bind(({ wv t1122 = v618_k; wv t1123 = rt_get(&F_stmts, v616_program); rt_idx(t1122, t1123); }), 1);
    }
    v618_k = rt_add(v618_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v615_isEntry), "&&") && rt_bool(rt_ne(v617_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v617_last), rt_s(&S185)), "&&")))) {
    rt_set_mem(rt_b(1), &F_notLibrary, rt_idx(v614_m, up->v388_modules));
    RT_RET(RT_NULL);
  }
  wv v619_scope = rt_bind(rt_chk(s401_newScope(up, RT_NULL), "checkModule"), 0);
  (void)rt_chk(s425_stealNames(up, v614_m, v619_scope), "checkModule");
  (void)({ wv t1124 = rt_rec(0, 2, K49, (wv[]){ rt_get(&F_stmts, v616_program), RT_NULL }); wv t1125 = v619_scope; wv t1126 = rt_rec(0, 2, K46, (wv[]){ rt_def(up->v428_topFn, "topFn"), rt_i(0) }); rt_chk(s407_block(up, t1124, t1125, t1126), "checkModule"); });
  (void)rt_chk(s426_libraryRules(up, v614_m, v619_scope, v615_isEntry, v617_last), "checkModule");
  RT_RET(RT_NULL);
}

static wv w6_binFn(wv v626_op) {
  rt_enter("binFn");
  v626_op = rt_bind(v626_op, 0);
  wv v627_k = rt_i(0);
  while (rt_cond(({ wv t1153 = ({ wv t1151 = v627_k; wv t1152 = rt_def(w6_binOps, "binOps"); rt_idx(t1151, t1152); }); wv t1154 = v626_op; rt_ne(t1153, t1154); }))) {
    v627_k = rt_add(v627_k, rt_i(1));
  }
  RT_RET(({ wv t1155 = v627_k; wv t1156 = rt_def(w6_binFns, "binFns"); rt_idx(t1155, t1156); }));
}

static wv w6_modeOf(wv v628_x) {
  rt_enter("modeOf");
  v628_x = rt_bind(v628_x, 0);
  RT_RET((rt_cond(rt_get(&F_isShifty, v628_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isFickle, v628_x)) ? rt_s(&S37) : rt_s(&S340))));
}

static wv w6_unFn(wv v629_op) {
  rt_enter("unFn");
  v629_op = rt_bind(v629_op, 0);
  RT_RET((rt_cond(rt_eq(v629_op, rt_s(&S72))) ? rt_s(&S472) : (rt_cond(rt_eq(v629_op, rt_s(&S87))) ? rt_s(&S473) : rt_s(&S474))));
}

static wv w6_jsBool(wv v630_b) {
  rt_enter("jsBool");
  v630_b = rt_bind(v630_b, 0);
  RT_RET((rt_cond(v630_b) ? rt_s(&S475) : rt_s(&S476)));
}

static wv w6_repeat(wv v631_s, wv v632_count) {
  rt_enter("repeat");
  v631_s = rt_bind(v631_s, 0);
  v632_count = rt_bind(v632_count, 0);
  wv v633_out = rt_s(&S13);
  wv v634_k = rt_i(0);
  while (rt_cond(rt_lt(v634_k, v632_count))) {
    v633_out = rt_add(v633_out, v631_s);
    v634_k = rt_add(v634_k, rt_i(1));
  }
  RT_RET(v633_out);
}

static wv w6_jsFloat(wv v635_x) {
  rt_enter("jsFloat");
  v635_x = rt_bind(v635_x, 0);
  wv v636_cs = rt_bind(std_split(std_toPurple(v635_x), rt_s(&S13)), 0);
  wv v637_len = rt_bind(std_length(v636_cs), 0);
  wv v638_out = rt_s(&S13);
  wv v639_k = rt_i(0);
  while (rt_cond(rt_lt(v639_k, v637_len))) {
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_idx(v639_k, v636_cs), rt_s(&S107)), "&&") && rt_bool(rt_eq(({ wv t1157 = rt_add(v639_k, rt_i(1)); wv t1158 = v636_cs; rt_idx(t1157, t1158); }), rt_s(&S4)), "&&")), "&&") && rt_bool(rt_b(rt_bool(({ wv t1159 = rt_add(v639_k, rt_i(2)); wv t1160 = v637_len; rt_eq(t1159, t1160); }), "||") || rt_bool(rt_eq(({ wv t1161 = rt_add(v639_k, rt_i(2)); wv t1162 = v636_cs; rt_idx(t1161, t1162); }), rt_s(&S98)), "||")), "&&")))) {
      v639_k = rt_add(v639_k, rt_i(2));
      continue;
    }
    v638_out = ({ wv t1163 = v638_out; wv t1164 = rt_idx(v639_k, v636_cs); rt_add(t1163, t1164); });
    v639_k = rt_add(v639_k, rt_i(1));
  }
  RT_RET(v638_out);
}

static wv w6_generate(wv p_v640_modules, wv v641_runtime) {
  rt_enter("generate");
  struct fr141 fr = { 0 };
  fr.v640_modules = rt_bind(p_v640_modules, 0);
  v641_runtime = rt_bind(v641_runtime, 0);
  fr.v642_out = rt_arr(0, NULL, 1);
  fr.v643_indent = rt_i(1);
  fr.v644_n = rt_i(0);
  fr.v645_loops = rt_i(0);
  fr.v646_iife = rt_b(0);
  fr.v647_mod = rt_s(&S13);
  fr.v648_stolen = rt_arr(0, NULL, 1);
  fr.v649_lineups = rt_arr(0, NULL, 1);
  fr.v650_fickles = rt_arr(0, NULL, 1);
  wv v685_mi = rt_i(0);
  while (rt_cond(({ wv t1429 = v685_mi; wv t1430 = std_length(fr.v640_modules); rt_lt(t1429, t1430); }))) {
    wv v802_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v685_mi, fr.v640_modules))), 0);
    wv v803_li = rt_i(0);
    while (rt_cond(({ wv t1431 = v803_li; wv t1432 = std_length(v802_list); rt_lt(t1431, t1432); }))) {
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v803_li, v802_list)), rt_s(&S53)))) {
        (void)({ wv t1433 = rt_idx(v803_li, v802_list); wv t1434 = rt_chk(s651_modId(&fr, v685_mi), "generate"); rt_chk(s664_lineupDef(&fr, t1433, t1434), "generate"); });
      }
      v803_li = rt_add(v803_li, rt_i(1));
    }
    v685_mi = rt_add(v685_mi, rt_i(1));
  }
  v685_mi = rt_i(0);
  while (rt_cond(({ wv t1435 = v685_mi; wv t1436 = std_length(fr.v640_modules); rt_lt(t1435, t1436); }))) {
    (void)rt_chk(s652_enter(&fr, v685_mi), "generate");
    (void)rt_chk(s659_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v685_mi, fr.v640_modules)))), "generate");
    v685_mi = rt_add(v685_mi, rt_i(1));
  }
  RT_RET(({ wv t1437 = rt_text(v641_runtime); wv t1438 = rt_text(std_join(fr.v642_out, rt_s(&S2))); rt_cat(5, (wv[]){ rt_s(&S561), t1437, rt_s(&S562), t1438, rt_s(&S563) }); }));
}

static wv s651_modId(struct fr141 *up, wv v686_m) {
  rt_enter("modId");
  v686_m = rt_bind(v686_m, 0);
  RT_RET((rt_cond(({ wv t1165 = v686_m; wv t1166 = rt_sub(std_length(up->v640_modules), rt_i(1)); rt_eq(t1165, t1166); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v686_m, rt_i(1))) })));
}

static wv s652_enter(struct fr141 *up, wv v687_m) {
  rt_enter("enter");
  v687_m = rt_bind(v687_m, 0);
  (void)({ wv t1167 = rt_get(&F_file, rt_idx(v687_m, up->v640_modules)); wv t1168 = rt_get(&F_chars, rt_idx(v687_m, up->v640_modules)); rt_chk(w2_setSource(t1167, t1168), "enter"); });
  wv t1169 = rt_bind(rt_chk(s651_modId(up, v687_m), "enter"), 1);
  (void)rt_def(up->v647_mod, "mod");
  up->v647_mod = t1169;
  wv t1170 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v648_stolen, "stolen");
  up->v648_stolen = t1170;
  wv t1171 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v649_lineups, "lineups");
  up->v649_lineups = t1171;
  wv t1172 = rt_bind(rt_get(&F_fickleTargets, rt_idx(v687_m, up->v640_modules)), 1);
  (void)rt_def(up->v650_fickles, "fickles");
  up->v650_fickles = t1172;
  wv v688_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v687_m, up->v640_modules))), 0);
  wv v689_k = rt_i(0);
  while (rt_cond(({ wv t1173 = v689_k; wv t1174 = std_length(v688_list); rt_lt(t1173, t1174); }))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v689_k, v688_list)), rt_s(&S53)))) {
      (void)({ wv t1175 = rt_def(up->v649_lineups, "lineups"); wv t1176 = rt_get(&F_name, rt_idx(v689_k, v688_list)); std_push(t1175, t1176); });
    }
    v689_k = rt_add(v689_k, rt_i(1));
  }
  wv v690_steals = rt_bind(rt_get(&F_steals, rt_idx(v687_m, up->v640_modules)), 0);
  v689_k = rt_i(0);
  while (rt_cond(({ wv t1177 = v689_k; wv t1178 = std_length(v690_steals); rt_lt(t1177, t1178); }))) {
    (void)({ wv t1181 = rt_def(up->v648_stolen, "stolen"); wv t1182 = ({ wv t1179 = rt_get(&F_name, rt_get(&F_node, rt_idx(v689_k, v690_steals))); wv t1180 = rt_get(&F_lib, rt_idx(v689_k, v690_steals)); rt_rec(0, 2, K50, (wv[]){ t1179, t1180 }); }); std_push(t1181, t1182); });
    v689_k = rt_add(v689_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s653_id(struct fr141 *up, wv v691_name) {
  rt_enter("id");
  v691_name = rt_bind(v691_name, 0);
  RT_RET(({ wv t1183 = rt_text(rt_def(up->v647_mod, "mod")); wv t1184 = rt_text(v691_name); rt_cat(4, (wv[]){ rt_s(&S94), t1183, rt_s(&S12), t1184 }); }));
}

static wv s654_stolenFrom(struct fr141 *up, wv v692_x) {
  rt_enter("stolenFrom");
  v692_x = rt_bind(v692_x, 0);
  if (rt_cond(rt_ne(rt_get(&F_k, v692_x), rt_s(&S264)))) {
    RT_RET(RT_NULL);
  }
  wv v693_k = rt_i(0);
  while (rt_cond(({ wv t1185 = v693_k; wv t1186 = std_length(rt_def(up->v648_stolen, "stolen")); rt_lt(t1185, t1186); }))) {
    if (rt_cond(({ wv t1189 = rt_get(&F_name, ({ wv t1187 = v693_k; wv t1188 = rt_def(up->v648_stolen, "stolen"); rt_idx(t1187, t1188); })); wv t1190 = rt_get(&F_name, v692_x); rt_eq(t1189, t1190); }))) {
      RT_RET(rt_get(&F_lib, ({ wv t1191 = v693_k; wv t1192 = rt_def(up->v648_stolen, "stolen"); rt_idx(t1191, t1192); })));
    }
    v693_k = rt_add(v693_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s655_exportKind(struct fr141 *up, wv v694_lib, wv v695_name) {
  rt_enter("exportKind");
  v694_lib = rt_bind(v694_lib, 0);
  v695_name = rt_bind(v695_name, 0);
  wv v696_xs = rt_bind(rt_get(&F_exports, rt_idx(v694_lib, up->v640_modules)), 0);
  wv v697_k = rt_i(0);
  while (rt_cond(({ wv t1193 = rt_get(&F_name, rt_idx(v697_k, v696_xs)); wv t1194 = v695_name; rt_ne(t1193, t1194); }))) {
    v697_k = rt_add(v697_k, rt_i(1));
  }
  RT_RET(rt_get(&F_kind, rt_idx(v697_k, v696_xs)));
}

static wv s656_lineupJs(struct fr141 *up, wv v698_x) {
  rt_enter("lineupJs");
  v698_x = rt_bind(v698_x, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v698_x), rt_s(&S264)))) {
    RT_RET((rt_cond(({ wv t1195 = rt_def(up->v649_lineups, "lineups"); wv t1196 = rt_get(&F_name, v698_x); rt_chk(w1_contains(t1195, t1196), "lineupJs"); })) ? ({ wv t1197 = rt_text(rt_def(up->v647_mod, "mod")); wv t1198 = rt_text(rt_get(&F_name, v698_x)); rt_cat(4, (wv[]){ rt_s(&S477), t1197, rt_s(&S12), t1198 }); }) : RT_NULL));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v698_x), rt_s(&S198)))) {
    RT_RET(RT_NULL);
  }
  wv v699_lib = rt_bind(rt_chk(s654_stolenFrom(up, rt_get(&F_rec, v698_x)), "lineupJs"), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v699_lib, RT_NULL), "&&") && rt_bool(rt_eq(({ wv t1199 = v699_lib; wv t1200 = rt_get(&F_field, v698_x); rt_chk(s655_exportKind(up, t1199, t1200), "lineupJs"); }), rt_s(&S53)), "&&"))) ? ({ wv t1201 = rt_text(rt_chk(s651_modId(up, v699_lib), "lineupJs")); wv t1202 = rt_text(rt_get(&F_field, v698_x)); rt_cat(4, (wv[]){ rt_s(&S477), t1201, rt_s(&S12), t1202 }); }) : RT_NULL));
}

static wv s657_emit(struct fr141 *up, wv v700_line) {
  rt_enter("emit");
  v700_line = rt_bind(v700_line, 0);
  (void)({ wv t1205 = rt_def(up->v642_out, "out"); wv t1206 = ({ wv t1203 = rt_chk(w6_repeat(rt_s(&S478), rt_def(up->v643_indent, "indent")), "emit"); wv t1204 = v700_line; rt_add(t1203, t1204); }); std_push(t1205, t1206); });
  RT_RET(RT_NULL);
}

static wv s658_tmp(struct fr141 *up) {
  rt_enter("tmp");
  wv v701_name = rt_cat(2, (wv[]){ rt_s(&S479), rt_text(rt_def(up->v644_n, "n")) });
  up->v644_n = rt_add(rt_def(up->v644_n, "n"), rt_i(1));
  RT_RET(v701_name);
}

static wv s659_stmts(struct fr141 *up, wv v702_list) {
  rt_enter("stmts");
  v702_list = rt_bind(v702_list, 0);
  wv v703_k = rt_i(0);
  while (rt_cond(({ wv t1207 = v703_k; wv t1208 = std_length(v702_list); rt_lt(t1207, t1208); }))) {
    (void)rt_chk(s662_stmt(up, rt_idx(v703_k, v702_list)), "stmts");
    v703_k = rt_add(v703_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s660_blockBody(struct fr141 *up, wv v704_b) {
  rt_enter("blockBody");
  v704_b = rt_bind(v704_b, 0);
  (void)rt_chk(s659_stmts(up, rt_get(&F_stmts, v704_b)), "blockBody");
  if (rt_cond(rt_ne(rt_get(&F_value, v704_b), RT_NULL))) {
    (void)rt_chk(s657_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s684_expr(up, rt_get(&F_value, v704_b)), "blockBody")), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s661_valueOf(struct fr141 *up, wv v705_b) {
  rt_enter("valueOf");
  v705_b = rt_bind(v705_b, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_value, v705_b), RT_NULL)) ? rt_s(&S292) : rt_chk(s684_expr(up, rt_get(&F_value, v705_b)), "valueOf")));
}

static wv s662_stmt(struct fr141 *up, wv v706_s) {
  rt_enter("stmt");
  v706_s = rt_bind(v706_s, 0);
  if (rt_cond(rt_ne(rt_get(&F_handler, v706_s), RT_NULL))) {
    (void)rt_chk(s668_whineStmt(up, v706_s), "stmt");
    RT_RET(RT_NULL);
  }
  wv v707_kind = rt_bind(rt_get(&F_k, v706_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v707_kind, rt_s(&S207)), "||") || rt_bool(rt_eq(v707_kind, rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(v707_kind, rt_s(&S54)), "||")), "||") || rt_bool(rt_eq(v707_kind, rt_s(&S55)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S33)))) {
    (void)rt_chk(s663_schemeDef(up, v706_s), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S34)))) {
    if (rt_cond(rt_def(up->v646_iife, "iife"))) {
      (void)rt_chk(w2_fail(rt_get(&F_pos, v706_s), rt_s(&S480)), "stmt");
    }
    (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S481), rt_text((rt_cond(rt_eq(rt_get(&F_e, v706_s), RT_NULL)) ? rt_s(&S292) : rt_chk(s684_expr(up, rt_get(&F_e, v706_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S44)))) {
    (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S482), rt_text(rt_chk(s684_expr(up, rt_get(&F_e, v706_s)), "stmt")), rt_s(&S483) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v707_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v707_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_def(up->v645_loops, "loops"), rt_i(0)))) {
      (void)({ wv t1209 = rt_get(&F_pos, v706_s); wv t1210 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v707_kind), rt_s(&S484) }); rt_chk(w2_fail(t1209, t1210), "stmt"); });
    }
    (void)rt_chk(s657_emit(up, (rt_cond(rt_eq(v707_kind, rt_s(&S42))) ? rt_s(&S485) : rt_s(&S486))), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S41)))) {
    (void)rt_chk(s657_emit(up, ({ wv t1211 = rt_text((rt_cond(rt_get(&F_neg, v706_s)) ? rt_s(&S87) : rt_s(&S13))); wv t1212 = rt_text(rt_chk(s684_expr(up, rt_get(&F_cond, v706_s)), "stmt")); rt_cat(5, (wv[]){ rt_s(&S487), t1211, rt_s(&S488), t1212, rt_s(&S489) }); })), "stmt");
    up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
    up->v645_loops = rt_add(rt_def(up->v645_loops, "loops"), rt_i(1));
    (void)rt_chk(s660_blockBody(up, rt_get(&F_body, v706_s)), "stmt");
    up->v645_loops = rt_sub(rt_def(up->v645_loops, "loops"), rt_i(1));
    up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
    (void)rt_chk(s657_emit(up, rt_s(&S15)), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S211)))) {
    (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S490), rt_text(rt_get(&F_code, v706_s)), rt_s(&S483) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S185)))) {
    (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S491), rt_text((rt_cond(rt_eq(rt_get(&F_e, v706_s), RT_NULL)) ? rt_s(&S4) : rt_chk(s684_expr(up, rt_get(&F_e, v706_s)), "stmt"))), rt_s(&S483) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S184)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v706_s)), rt_s(&S193)))) {
      (void)rt_chk(s669_chainStmt(up, rt_get(&F_e, v706_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v706_s)), rt_s(&S51)))) {
      (void)rt_chk(s680_suspectStmt(up, rt_get(&F_e, v706_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v706_s)), rt_s(&S192)))) {
      (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S490), rt_text(rt_chk(s684_expr(up, rt_get(&F_arg, rt_get(&F_e, v706_s))), "stmt")), rt_s(&S483) })), "stmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s657_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s684_expr(up, rt_get(&F_e, v706_s)), "stmt")), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S35)))) {
    (void)rt_chk(s657_emit(up, ({ wv t1215 = rt_text((rt_cond(rt_b(rt_bool(rt_get(&F_isShifty, v706_s), "||") || rt_bool(rt_get(&F_isFickle, v706_s), "||"))) ? rt_s(&S492) : rt_s(&S340))); wv t1216 = rt_text(rt_chk(s653_id(up, rt_get(&F_name, v706_s)), "stmt")); wv t1217 = rt_text(({ wv t1213 = rt_get(&F_e, v706_s); wv t1214 = rt_chk(w6_modeOf(v706_s), "stmt"); rt_chk(s665_bindRhs(up, t1213, t1214), "stmt"); })); rt_cat(6, (wv[]){ t1215, rt_s(&S0), t1216, rt_s(&S493), t1217, rt_s(&S14) }); })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v707_kind, rt_s(&S38)))) {
    (void)rt_chk(s666_cheatStmt(up, v706_s), "stmt");
    RT_RET(RT_NULL);
  }
  rt_tantrum(rt_cat(2, (wv[]){ rt_s(&S494), rt_text(v707_kind) }));
  RT_RET(RT_NULL);
}

static wv s663_schemeDef(struct fr141 *up, wv v708_s) {
  rt_enter("schemeDef");
  v708_s = rt_bind(v708_s, 0);
  wv v709_names = rt_arr(0, NULL, 1);
  wv v710_k = rt_i(0);
  while (rt_cond(({ wv t1218 = v710_k; wv t1219 = std_length(rt_get(&F_params, v708_s)); rt_lt(t1218, t1219); }))) {
    (void)({ wv t1222 = v709_names; wv t1223 = rt_chk(s653_id(up, rt_get(&F_name, ({ wv t1220 = v710_k; wv t1221 = rt_get(&F_params, v708_s); rt_idx(t1220, t1221); }))), "schemeDef"); std_push(t1222, t1223); });
    v710_k = rt_add(v710_k, rt_i(1));
  }
  (void)rt_chk(s657_emit(up, ({ wv t1224 = rt_text(rt_chk(s653_id(up, rt_get(&F_name, v708_s)), "schemeDef")); wv t1225 = rt_text(std_join(v709_names, rt_s(&S390))); rt_cat(5, (wv[]){ rt_s(&S495), t1224, rt_s(&S90), t1225, rt_s(&S496) }); })), "schemeDef");
  up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
  v710_k = rt_i(0);
  while (rt_cond(({ wv t1226 = v710_k; wv t1227 = std_length(rt_get(&F_params, v708_s)); rt_lt(t1226, t1227); }))) {
    wv v713_param = rt_bind(({ wv t1228 = v710_k; wv t1229 = rt_get(&F_params, v708_s); rt_idx(t1228, t1229); }), 0);
    if (rt_cond(rt_not(rt_get(&F_isFickle, v713_param)))) {
      (void)rt_chk(s657_emit(up, ({ wv t1230 = rt_text(rt_chk(s653_id(up, rt_get(&F_name, v713_param)), "schemeDef")); wv t1231 = rt_text(rt_chk(s653_id(up, rt_get(&F_name, v713_param)), "schemeDef")); wv t1232 = rt_text(rt_chk(w6_jsBool(rt_get(&F_isShifty, v713_param)), "schemeDef")); rt_cat(6, (wv[]){ t1230, rt_s(&S497), t1231, rt_s(&S390), t1232, rt_s(&S483) }); })), "schemeDef");
    }
    v710_k = rt_add(v710_k, rt_i(1));
  }
  (void)rt_chk(s657_emit(up, rt_s(&S498)), "schemeDef");
  up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
  wv v711_savedLoops = rt_bind(rt_def(up->v645_loops, "loops"), 0);
  wv v712_savedIife = rt_bind(rt_def(up->v646_iife, "iife"), 0);
  wv t1233 = rt_i(0);
  (void)rt_def(up->v645_loops, "loops");
  up->v645_loops = t1233;
  wv t1234 = rt_b(0);
  (void)rt_def(up->v646_iife, "iife");
  up->v646_iife = t1234;
  (void)rt_chk(s659_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v708_s))), "schemeDef");
  (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S481), rt_text(rt_chk(s661_valueOf(up, rt_get(&F_body, v708_s)), "schemeDef")), rt_s(&S14) })), "schemeDef");
  wv t1235 = rt_bind(v711_savedLoops, 1);
  (void)rt_def(up->v645_loops, "loops");
  up->v645_loops = t1235;
  wv t1236 = rt_bind(v712_savedIife, 1);
  (void)rt_def(up->v646_iife, "iife");
  up->v646_iife = t1236;
  up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s657_emit(up, rt_s(&S499)), "schemeDef");
  (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S500), rt_text(rt_chk(w1_quote(rt_get(&F_name, v708_s)), "schemeDef")), rt_s(&S483) })), "schemeDef");
  (void)rt_chk(s657_emit(up, rt_s(&S15)), "schemeDef");
  up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s657_emit(up, rt_s(&S15)), "schemeDef");
  RT_RET(RT_NULL);
}

static wv s664_lineupDef(struct fr141 *up, wv v714_s, wv v715_m) {
  rt_enter("lineupDef");
  v714_s = rt_bind(v714_s, 0);
  v715_m = rt_bind(v715_m, 0);
  wv v716_names = rt_arr(0, NULL, 1);
  wv v717_k = rt_i(0);
  while (rt_cond(({ wv t1237 = v717_k; wv t1238 = std_length(rt_get(&F_members, v714_s)); rt_lt(t1237, t1238); }))) {
    (void)({ wv t1241 = v716_names; wv t1242 = rt_chk(w1_quote(rt_get(&F_name, ({ wv t1239 = v717_k; wv t1240 = rt_get(&F_members, v714_s); rt_idx(t1239, t1240); }))), "lineupDef"); std_push(t1241, t1242); });
    v717_k = rt_add(v717_k, rt_i(1));
  }
  (void)rt_chk(s657_emit(up, ({ wv t1243 = rt_text(v715_m); wv t1244 = rt_text(rt_get(&F_name, v714_s)); wv t1245 = rt_text(rt_chk(w1_quote(rt_get(&F_name, v714_s)), "lineupDef")); wv t1246 = rt_text(std_join(v716_names, rt_s(&S390))); rt_cat(9, (wv[]){ rt_s(&S501), t1243, rt_s(&S12), t1244, rt_s(&S502), t1245, rt_s(&S503), t1246, rt_s(&S504) }); })), "lineupDef");
  RT_RET(RT_NULL);
}

static wv s665_bindRhs(struct fr141 *up, wv v718_e, wv v719_mode) {
  rt_enter("bindRhs");
  v718_e = rt_bind(v718_e, 0);
  v719_mode = rt_bind(v719_mode, 0);
  wv v720_mutable = rt_eq(v719_mode, rt_s(&S36));
  if (rt_cond(rt_eq(rt_get(&F_k, v718_e), rt_s(&S195)))) {
    wv v721_items = rt_bind(rt_chk(s683_exprList(up, rt_get(&F_items, v718_e)), "bindRhs"), 0);
    RT_RET((rt_cond(v720_mutable) ? rt_cat(3, (wv[]){ rt_s(&S91), rt_text(v721_items), rt_s(&S92) }) : rt_cat(3, (wv[]){ rt_s(&S505), rt_text(v721_items), rt_s(&S506) })));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v718_e), rt_s(&S196)))) {
    RT_RET(rt_chk(s667_record(up, v718_e, v720_mutable), "bindRhs"));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1247 = rt_def(w6_scalarKinds, "scalarKinds"); wv t1248 = rt_get(&F_k, v718_e); rt_chk(w1_contains(t1247, t1248), "bindRhs"); }), "||") || rt_bool(rt_eq(v719_mode, rt_s(&S37)), "||")))) {
    RT_RET(rt_chk(s684_expr(up, v718_e), "bindRhs"));
  }
  RT_RET(({ wv t1249 = rt_text(rt_chk(s684_expr(up, v718_e), "bindRhs")); wv t1250 = rt_text(rt_chk(w6_jsBool(v720_mutable), "bindRhs")); rt_cat(5, (wv[]){ rt_s(&S507), t1249, rt_s(&S390), t1250, rt_s(&S16) }); }));
}

static wv s666_cheatStmt(struct fr141 *up, wv v722_s) {
  rt_enter("cheatStmt");
  v722_s = rt_bind(v722_s, 0);
  wv v723_t = rt_bind(rt_get(&F_target, v722_s), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v723_t), rt_s(&S264)))) {
    wv v725_mode = rt_bind((rt_cond(({ wv t1251 = rt_def(up->v650_fickles, "fickles"); wv t1252 = rt_get(&F_pos, v723_t); rt_chk(w1_contains(t1251, t1252), "cheatStmt"); })) ? rt_s(&S37) : rt_s(&S36)), 0);
    if (rt_cond(rt_eq(rt_get(&F_op, v722_s), RT_NULL))) {
      (void)rt_chk(s657_emit(up, ({ wv t1255 = rt_text(rt_chk(s653_id(up, rt_get(&F_name, v723_t)), "cheatStmt")); wv t1256 = rt_text(({ wv t1253 = rt_get(&F_e, v722_s); wv t1254 = v725_mode; rt_chk(s665_bindRhs(up, t1253, t1254), "cheatStmt"); })); rt_cat(4, (wv[]){ t1255, rt_s(&S493), t1256, rt_s(&S14) }); })), "cheatStmt");
      RT_RET(RT_NULL);
    }
    wv v726_v = rt_bind(rt_chk(s684_expr(up, rt_get(&F_e, v722_s)), "cheatStmt"), 1);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v722_s)), rt_s(&S106)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v722_s)), rt_s(&S264)), "&&")))) {
      wv v727_temp = rt_bind(rt_chk(s658_tmp(up), "cheatStmt"), 0);
      (void)rt_chk(s657_emit(up, ({ wv t1257 = rt_text(v727_temp); wv t1258 = rt_text(v726_v); rt_cat(5, (wv[]){ rt_s(&S508), t1257, rt_s(&S493), t1258, rt_s(&S14) }); })), "cheatStmt");
      v726_v = rt_bind(v727_temp, 1);
    }
    (void)rt_chk(s657_emit(up, ({ wv t1259 = rt_text(rt_chk(s653_id(up, rt_get(&F_name, v723_t)), "cheatStmt")); wv t1260 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v722_s)), "cheatStmt")); wv t1261 = rt_text(rt_chk(s653_id(up, rt_get(&F_name, v723_t)), "cheatStmt")); wv t1262 = rt_text(v726_v); rt_cat(8, (wv[]){ t1259, rt_s(&S509), t1260, rt_s(&S90), t1261, rt_s(&S390), t1262, rt_s(&S483) }); })), "cheatStmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v723_t), rt_s(&S198)))) {
    wv v728_args = ({ wv t1263 = rt_text(rt_chk(s684_expr(up, rt_get(&F_e, v722_s)), "cheatStmt")); wv t1264 = rt_text(rt_get(&F_field, v723_t)); wv t1265 = rt_text(rt_chk(s684_expr(up, rt_get(&F_rec, v723_t)), "cheatStmt")); rt_cat(5, (wv[]){ t1263, rt_s(&S510), t1264, rt_s(&S511), t1265 }); });
    if (rt_cond(rt_eq(rt_get(&F_op, v722_s), RT_NULL))) {
      (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S512), rt_text(v728_args), rt_s(&S483) })), "cheatStmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s657_emit(up, ({ wv t1266 = rt_text(v728_args); wv t1267 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v722_s)), "cheatStmt")); rt_cat(5, (wv[]){ rt_s(&S513), t1266, rt_s(&S514), t1267, rt_s(&S483) }); })), "cheatStmt");
    RT_RET(RT_NULL);
  }
  wv v724_args = ({ wv t1268 = rt_text(rt_chk(s684_expr(up, rt_get(&F_e, v722_s)), "cheatStmt")); wv t1269 = rt_text(rt_chk(s684_expr(up, rt_get(&F_index, v723_t)), "cheatStmt")); wv t1270 = rt_text(rt_chk(s684_expr(up, rt_get(&F_arr, v723_t)), "cheatStmt")); rt_cat(5, (wv[]){ t1268, rt_s(&S390), t1269, rt_s(&S390), t1270 }); });
  if (rt_cond(rt_eq(rt_get(&F_op, v722_s), RT_NULL))) {
    (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S515), rt_text(v724_args), rt_s(&S483) })), "cheatStmt");
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s657_emit(up, ({ wv t1271 = rt_text(v724_args); wv t1272 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v722_s)), "cheatStmt")); rt_cat(5, (wv[]){ rt_s(&S516), t1271, rt_s(&S514), t1272, rt_s(&S483) }); })), "cheatStmt");
  RT_RET(RT_NULL);
}

static wv s667_record(struct fr141 *up, wv v729_e, wv v730_mutable) {
  rt_enter("record");
  v729_e = rt_bind(v729_e, 0);
  v730_mutable = rt_bind(v730_mutable, 0);
  wv v731_fields = rt_s(&S13);
  wv v732_k = rt_i(0);
  while (rt_cond(({ wv t1273 = v732_k; wv t1274 = std_length(rt_get(&F_fields, v729_e)); rt_lt(t1273, t1274); }))) {
    wv v733_field = rt_bind(({ wv t1275 = v732_k; wv t1276 = rt_get(&F_fields, v729_e); rt_idx(t1275, t1276); }), 0);
    v731_fields = ({ wv t1279 = v731_fields; wv t1280 = ({ wv t1277 = rt_text(rt_get(&F_name, v733_field)); wv t1278 = rt_text(rt_chk(s684_expr(up, rt_get(&F_e, v733_field)), "record")); rt_cat(4, (wv[]){ rt_s(&S517), t1277, rt_s(&S29), t1278 }); }); rt_add(t1279, t1280); });
    v732_k = rt_add(v732_k, rt_i(1));
  }
  RT_RET(({ wv t1281 = rt_text(v731_fields); wv t1282 = rt_text(rt_chk(w6_jsBool(v730_mutable), "record")); rt_cat(5, (wv[]){ rt_s(&S518), t1281, rt_s(&S519), t1282, rt_s(&S16) }); }));
}

static wv s668_whineStmt(struct fr141 *up, wv v734_s) {
  rt_enter("whineStmt");
  v734_s = rt_bind(v734_s, 0);
  wv v735_call = rt_bind(rt_get(&F_e, v734_s), 0);
  wv v736_args = rt_arr(0, NULL, 1);
  wv v737_k = rt_i(0);
  while (rt_cond(({ wv t1283 = v737_k; wv t1284 = std_length(rt_get(&F_args, v735_call)); rt_lt(t1283, t1284); }))) {
    wv v741_a = rt_bind(({ wv t1285 = v737_k; wv t1286 = rt_get(&F_args, v735_call); rt_idx(t1285, t1286); }), 0);
    if (rt_cond(({ wv t1287 = rt_def(w6_plainKinds, "plainKinds"); wv t1288 = rt_get(&F_k, v741_a); rt_chk(w1_contains(t1287, t1288), "whineStmt"); }))) {
      (void)({ wv t1289 = v736_args; wv t1290 = rt_chk(s684_expr(up, v741_a), "whineStmt"); std_push(t1289, t1290); });
    } else {
      wv v742_temp = rt_bind(rt_chk(s658_tmp(up), "whineStmt"), 0);
      (void)rt_chk(s657_emit(up, ({ wv t1291 = rt_text(v742_temp); wv t1292 = rt_text(rt_chk(s684_expr(up, v741_a), "whineStmt")); rt_cat(5, (wv[]){ rt_s(&S508), t1291, rt_s(&S493), t1292, rt_s(&S14) }); })), "whineStmt");
      (void)std_push(v736_args, v742_temp);
    }
    v737_k = rt_add(v737_k, rt_i(1));
  }
  wv v738_r = rt_bind(rt_chk(s658_tmp(up), "whineStmt"), 0);
  wv v739_ev = rt_cat(2, (wv[]){ rt_s(&S520), rt_text(rt_def(up->v644_n, "n")) });
  up->v644_n = rt_add(rt_def(up->v644_n, "n"), rt_i(1));
  (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S521), rt_text(v738_r), rt_s(&S14) })), "whineStmt");
  (void)rt_chk(s657_emit(up, rt_s(&S498)), "whineStmt");
  (void)rt_chk(s657_emit(up, ({ wv t1295 = rt_text(v738_r); wv t1296 = rt_text(({ wv t1293 = rt_get(&F_callee, v735_call); wv t1294 = v736_args; rt_chk(s682_callJs(up, t1293, t1294), "whineStmt"); })); rt_cat(5, (wv[]){ rt_s(&S478), t1295, rt_s(&S493), t1296, rt_s(&S14) }); })), "whineStmt");
  (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S522), rt_text(v739_ev), rt_s(&S496) })), "whineStmt");
  up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s657_emit(up, ({ wv t1297 = rt_text(v739_ev); wv t1298 = rt_text(v739_ev); rt_cat(5, (wv[]){ rt_s(&S523), t1297, rt_s(&S524), t1298, rt_s(&S14) }); })), "whineStmt");
  if (rt_cond(rt_ne(rt_get(&F_param, rt_get(&F_handler, v734_s)), RT_NULL))) {
    (void)rt_chk(s657_emit(up, ({ wv t1299 = rt_text(rt_chk(s653_id(up, rt_get(&F_param, rt_get(&F_handler, v734_s))), "whineStmt")); wv t1300 = rt_text(v739_ev); rt_cat(5, (wv[]){ rt_s(&S508), t1299, rt_s(&S493), t1300, rt_s(&S525) }); })), "whineStmt");
  }
  (void)rt_chk(s657_emit(up, rt_s(&S93)), "whineStmt");
  up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s659_stmts(up, rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v734_s)))), "whineStmt");
  (void)rt_chk(s657_emit(up, ({ wv t1301 = rt_text(v738_r); wv t1302 = rt_text(rt_chk(s661_valueOf(up, rt_get(&F_body, rt_get(&F_handler, v734_s))), "whineStmt")); rt_cat(4, (wv[]){ t1301, rt_s(&S493), t1302, rt_s(&S14) }); })), "whineStmt");
  up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s657_emit(up, rt_s(&S15)), "whineStmt");
  up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s657_emit(up, rt_s(&S15)), "whineStmt");
  wv v740_rest = rt_bind(v734_s, 1);
  rt_set_mem(RT_NULL, &F_handler, v740_rest);
  ({ wv t1305 = ({ wv t1303 = v738_r; wv t1304 = rt_get(&F_pos, v734_s); rt_rec(0, 4, K51, (wv[]){ rt_s(&S526), t1303, t1304, rt_b(0) }); }); wv t1306 = v740_rest; rt_set_mem(t1305, &F_e, t1306); });
  (void)rt_chk(s662_stmt(up, v740_rest), "whineStmt");
  RT_RET(RT_NULL);
}

static wv s669_chainStmt(struct fr141 *up, wv v743_e) {
  rt_enter("chainStmt");
  v743_e = rt_bind(v743_e, 0);
  wv v744_k = rt_i(0);
  while (rt_cond(({ wv t1307 = v744_k; wv t1308 = std_length(rt_get(&F_branches, v743_e)); rt_lt(t1307, t1308); }))) {
    wv v745_b = rt_bind(({ wv t1309 = v744_k; wv t1310 = rt_get(&F_branches, v743_e); rt_idx(t1309, t1310); }), 0);
    (void)rt_chk(s657_emit(up, ({ wv t1311 = rt_text((rt_cond(rt_gt(v744_k, rt_i(0))) ? rt_s(&S527) : rt_s(&S13))); wv t1312 = rt_text(rt_chk(s670_test(up, v745_b), "chainStmt")); rt_cat(4, (wv[]){ t1311, rt_s(&S528), t1312, rt_s(&S496) }); })), "chainStmt");
    up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
    (void)rt_chk(s660_blockBody(up, rt_get(&F_body, v745_b)), "chainStmt");
    up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
    v744_k = rt_add(v744_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v743_e), RT_NULL))) {
    (void)rt_chk(s657_emit(up, rt_s(&S529)), "chainStmt");
    up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
    (void)rt_chk(s660_blockBody(up, rt_get(&F_els, v743_e)), "chainStmt");
    up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s657_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s670_test(struct fr141 *up, wv v746_b) {
  rt_enter("test");
  v746_b = rt_bind(v746_b, 0);
  RT_RET(({ wv t1313 = rt_text((rt_cond(rt_get(&F_neg, v746_b)) ? rt_s(&S87) : rt_s(&S13))); wv t1314 = rt_text(rt_chk(s684_expr(up, rt_get(&F_cond, v746_b)), "test")); rt_cat(4, (wv[]){ t1313, rt_s(&S488), t1314, rt_s(&S16) }); }));
}

static wv s671_branchCount(struct fr141 *up, wv v747_e) {
  rt_enter("branchCount");
  v747_e = rt_bind(v747_e, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v747_e), rt_s(&S193))) ? std_length(rt_get(&F_branches, v747_e)) : std_length(rt_get(&F_alibis, v747_e))));
}

static wv s672_branchBody(struct fr141 *up, wv v748_e, wv v749_i) {
  rt_enter("branchBody");
  v748_e = rt_bind(v748_e, 0);
  v749_i = rt_bind(v749_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v748_e), rt_s(&S193))) ? rt_get(&F_body, ({ wv t1315 = v749_i; wv t1316 = rt_get(&F_branches, v748_e); rt_idx(t1315, t1316); })) : rt_get(&F_body, ({ wv t1317 = v749_i; wv t1318 = rt_get(&F_alibis, v748_e); rt_idx(t1317, t1318); }))));
}

static wv s673_branchTest(struct fr141 *up, wv v750_e, wv v751_t, wv v752_i) {
  rt_enter("branchTest");
  v750_e = rt_bind(v750_e, 0);
  v751_t = rt_bind(v751_t, 0);
  v752_i = rt_bind(v752_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v750_e), rt_s(&S193))) ? rt_chk(s670_test(up, ({ wv t1319 = v752_i; wv t1320 = rt_get(&F_branches, v750_e); rt_idx(t1319, t1320); })), "branchTest") : ({ wv t1323 = v751_t; wv t1324 = ({ wv t1321 = v752_i; wv t1322 = rt_get(&F_alibis, v750_e); rt_idx(t1321, t1322); }); rt_chk(s678_alibiTest(up, t1323, t1324), "branchTest"); })));
}

static wv s674_isPlain(struct fr141 *up, wv v753_e) {
  rt_enter("isPlain");
  v753_e = rt_bind(v753_e, 0);
  wv v754_k = rt_i(0);
  while (rt_cond(({ wv t1325 = v754_k; wv t1326 = rt_chk(s671_branchCount(up, v753_e), "isPlain"); rt_le(t1325, t1326); }))) {
    wv v755_b = rt_bind((rt_cond(({ wv t1327 = v754_k; wv t1328 = rt_chk(s671_branchCount(up, v753_e), "isPlain"); rt_lt(t1327, t1328); })) ? rt_chk(s672_branchBody(up, v753_e, v754_k), "isPlain") : rt_get(&F_els, v753_e)), 0);
    wv v756_j = rt_i(0);
    while (rt_cond(({ wv t1329 = v756_j; wv t1330 = std_length(rt_get(&F_stmts, v755_b)); rt_lt(t1329, t1330); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1331 = v756_j; wv t1332 = rt_get(&F_stmts, v755_b); rt_idx(t1331, t1332); })), rt_s(&S207)))) {
        RT_RET(rt_b(0));
      }
      v756_j = rt_add(v756_j, rt_i(1));
    }
    v754_k = rt_add(v754_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s675_ternaryChain(struct fr141 *up, wv v757_e, wv v758_t) {
  rt_enter("ternaryChain");
  v757_e = rt_bind(v757_e, 0);
  v758_t = rt_bind(v758_t, 0);
  wv v759_js = rt_bind(rt_chk(s661_valueOf(up, rt_get(&F_els, v757_e)), "ternaryChain"), 1);
  wv v760_k = rt_sub(rt_chk(s671_branchCount(up, v757_e), "ternaryChain"), rt_i(1));
  while (rt_cond(rt_ge(v760_k, rt_i(0)))) {
    v759_js = ({ wv t1333 = rt_text(rt_chk(s673_branchTest(up, v757_e, v758_t, v760_k), "ternaryChain")); wv t1334 = rt_text(rt_chk(s661_valueOf(up, rt_chk(s672_branchBody(up, v757_e, v760_k), "ternaryChain")), "ternaryChain")); wv t1335 = rt_text(v759_js); rt_cat(7, (wv[]){ rt_s(&S90), t1333, rt_s(&S530), t1334, rt_s(&S531), t1335, rt_s(&S16) }); });
    v760_k = rt_sub(v760_k, rt_i(1));
  }
  RT_RET(v759_js);
}

static wv s676_branchFunction(struct fr141 *up, wv v761_e, wv v762_t) {
  rt_enter("branchFunction");
  v761_e = rt_bind(v761_e, 0);
  v762_t = rt_bind(v762_t, 0);
  wv v763_start = rt_bind(std_length(rt_def(up->v642_out, "out")), 0);
  wv v764_savedIndent = rt_bind(rt_def(up->v643_indent, "indent"), 0);
  wv v765_savedLoops = rt_bind(rt_def(up->v645_loops, "loops"), 0);
  wv v766_savedIife = rt_bind(rt_def(up->v646_iife, "iife"), 0);
  wv t1336 = rt_add(v764_savedIndent, rt_i(1));
  (void)rt_def(up->v643_indent, "indent");
  up->v643_indent = t1336;
  wv t1337 = rt_i(0);
  (void)rt_def(up->v645_loops, "loops");
  up->v645_loops = t1337;
  wv t1338 = rt_b(1);
  (void)rt_def(up->v646_iife, "iife");
  up->v646_iife = t1338;
  wv v767_k = rt_i(0);
  while (rt_cond(({ wv t1339 = v767_k; wv t1340 = rt_chk(s671_branchCount(up, v761_e), "branchFunction"); rt_lt(t1339, t1340); }))) {
    wv v769_b = rt_bind(rt_chk(s672_branchBody(up, v761_e, v767_k), "branchFunction"), 0);
    (void)rt_chk(s657_emit(up, ({ wv t1341 = rt_text((rt_cond(rt_gt(v767_k, rt_i(0))) ? rt_s(&S527) : rt_s(&S13))); wv t1342 = rt_text(rt_chk(s673_branchTest(up, v761_e, v762_t, v767_k), "branchFunction")); rt_cat(4, (wv[]){ t1341, rt_s(&S528), t1342, rt_s(&S496) }); })), "branchFunction");
    up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
    (void)rt_chk(s659_stmts(up, rt_get(&F_stmts, v769_b)), "branchFunction");
    (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S481), rt_text(rt_chk(s661_valueOf(up, v769_b), "branchFunction")), rt_s(&S14) })), "branchFunction");
    up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
    v767_k = rt_add(v767_k, rt_i(1));
  }
  (void)rt_chk(s657_emit(up, rt_s(&S529)), "branchFunction");
  up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s659_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v761_e))), "branchFunction");
  (void)rt_chk(s657_emit(up, rt_cat(3, (wv[]){ rt_s(&S481), rt_text(rt_chk(s661_valueOf(up, rt_get(&F_els, v761_e)), "branchFunction")), rt_s(&S14) })), "branchFunction");
  up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
  (void)rt_chk(s657_emit(up, rt_s(&S15)), "branchFunction");
  wv v768_body = rt_bind(std_join(({ wv t1343 = rt_def(up->v642_out, "out"); wv t1344 = v763_start; wv t1345 = std_length(rt_def(up->v642_out, "out")); std_slice(t1343, t1344, t1345); }), rt_s(&S2)), 0);
  while (rt_cond(({ wv t1346 = std_length(rt_def(up->v642_out, "out")); wv t1347 = v763_start; rt_gt(t1346, t1347); }))) {
    (void)std_pop(rt_def(up->v642_out, "out"));
  }
  wv t1348 = rt_bind(v764_savedIndent, 1);
  (void)rt_def(up->v643_indent, "indent");
  up->v643_indent = t1348;
  wv t1349 = rt_bind(v765_savedLoops, 1);
  (void)rt_def(up->v645_loops, "loops");
  up->v645_loops = t1349;
  wv t1350 = rt_bind(v766_savedIife, 1);
  (void)rt_def(up->v646_iife, "iife");
  up->v646_iife = t1350;
  RT_RET(v768_body);
}

static wv s677_chainExpr(struct fr141 *up, wv v770_e) {
  rt_enter("chainExpr");
  v770_e = rt_bind(v770_e, 0);
  if (rt_cond(rt_chk(s674_isPlain(up, v770_e), "chainExpr"))) {
    RT_RET(rt_chk(s675_ternaryChain(up, v770_e, RT_NULL), "chainExpr"));
  }
  wv v771_body = rt_bind(rt_chk(s676_branchFunction(up, v770_e, RT_NULL), "chainExpr"), 0);
  RT_RET(({ wv t1351 = rt_text(v771_body); wv t1352 = rt_text(rt_chk(w6_repeat(rt_s(&S478), rt_def(up->v643_indent, "indent")), "chainExpr")); rt_cat(5, (wv[]){ rt_s(&S532), t1351, rt_s(&S2), t1352, rt_s(&S533) }); }));
}

static wv s678_alibiTest(struct fr141 *up, wv v772_t, wv v773_a) {
  rt_enter("alibiTest");
  v772_t = rt_bind(v772_t, 0);
  v773_a = rt_bind(v773_a, 0);
  wv v774_tests = rt_arr(0, NULL, 1);
  wv v775_k = rt_i(0);
  while (rt_cond(({ wv t1353 = v775_k; wv t1354 = std_length(rt_get(&F_values, v773_a)); rt_lt(t1353, t1354); }))) {
    (void)({ wv t1359 = v774_tests; wv t1360 = ({ wv t1357 = rt_text(v772_t); wv t1358 = rt_text(rt_chk(s684_expr(up, ({ wv t1355 = v775_k; wv t1356 = rt_get(&F_values, v773_a); rt_idx(t1355, t1356); })), "alibiTest")); rt_cat(5, (wv[]){ rt_s(&S534), t1357, rt_s(&S390), t1358, rt_s(&S16) }); }); std_push(t1359, t1360); });
    v775_k = rt_add(v775_k, rt_i(1));
  }
  RT_RET(std_join(v774_tests, rt_s(&S535)));
}

static wv s679_suspectElse(struct fr141 *up, wv v776_e, wv v777_t) {
  rt_enter("suspectElse");
  v776_e = rt_bind(v776_e, 0);
  v777_t = rt_bind(v777_t, 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v776_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_alibis, v776_e))))), rt_s(&S198)), "||")))) {
    RT_RET(rt_get(&F_els, v776_e));
  }
  RT_RET(({ wv t1361 = rt_arr(0, NULL, 0); wv t1362 = rt_rec(0, 4, K51, (wv[]){ rt_s(&S526), rt_cat(3, (wv[]){ rt_s(&S536), rt_text(v777_t), rt_s(&S16) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K22, (wv[]){ t1361, t1362, RT_NULL }); }));
}

static wv s680_suspectStmt(struct fr141 *up, wv v778_e) {
  rt_enter("suspectStmt");
  v778_e = rt_bind(v778_e, 0);
  wv v779_t = rt_bind(rt_chk(s658_tmp(up), "suspectStmt"), 0);
  (void)rt_chk(s657_emit(up, ({ wv t1363 = rt_text(v779_t); wv t1364 = rt_text(rt_chk(s684_expr(up, rt_get(&F_subject, v778_e)), "suspectStmt")); rt_cat(5, (wv[]){ rt_s(&S508), t1363, rt_s(&S493), t1364, rt_s(&S14) }); })), "suspectStmt");
  wv v780_k = rt_i(0);
  while (rt_cond(({ wv t1365 = v780_k; wv t1366 = std_length(rt_get(&F_alibis, v778_e)); rt_lt(t1365, t1366); }))) {
    (void)rt_chk(s657_emit(up, ({ wv t1371 = rt_text((rt_cond(rt_gt(v780_k, rt_i(0))) ? rt_s(&S527) : rt_s(&S13))); wv t1372 = rt_text(({ wv t1369 = v779_t; wv t1370 = ({ wv t1367 = v780_k; wv t1368 = rt_get(&F_alibis, v778_e); rt_idx(t1367, t1368); }); rt_chk(s678_alibiTest(up, t1369, t1370), "suspectStmt"); })); rt_cat(4, (wv[]){ t1371, rt_s(&S528), t1372, rt_s(&S496) }); })), "suspectStmt");
    up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
    (void)rt_chk(s660_blockBody(up, rt_get(&F_body, ({ wv t1373 = v780_k; wv t1374 = rt_get(&F_alibis, v778_e); rt_idx(t1373, t1374); }))), "suspectStmt");
    up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
    v780_k = rt_add(v780_k, rt_i(1));
  }
  wv v781_els = rt_bind(rt_chk(s679_suspectElse(up, v778_e, v779_t), "suspectStmt"), 0);
  if (rt_cond(rt_ne(v781_els, RT_NULL))) {
    (void)rt_chk(s657_emit(up, rt_s(&S529)), "suspectStmt");
    up->v643_indent = rt_add(rt_def(up->v643_indent, "indent"), rt_i(1));
    (void)rt_chk(s660_blockBody(up, v781_els), "suspectStmt");
    up->v643_indent = rt_sub(rt_def(up->v643_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s657_emit(up, rt_s(&S15)), "suspectStmt");
  RT_RET(RT_NULL);
}

static wv s681_suspectExpr(struct fr141 *up, wv v782_e) {
  rt_enter("suspectExpr");
  v782_e = rt_bind(v782_e, 0);
  wv v783_t = rt_bind(rt_chk(s658_tmp(up), "suspectExpr"), 0);
  wv v784_subject = rt_bind(rt_chk(s684_expr(up, rt_get(&F_subject, v782_e)), "suspectExpr"), 0);
  wv v785_withElse = rt_bind(v782_e, 1);
  ({ wv t1375 = rt_chk(s679_suspectElse(up, v782_e, v783_t), "suspectExpr"); wv t1376 = v785_withElse; rt_set_mem(t1375, &F_els, t1376); });
  if (rt_cond(rt_chk(s674_isPlain(up, v785_withElse), "suspectExpr"))) {
    RT_RET(({ wv t1377 = rt_text(v783_t); wv t1378 = rt_text(rt_chk(s675_ternaryChain(up, v785_withElse, v783_t), "suspectExpr")); wv t1379 = rt_text(v784_subject); rt_cat(7, (wv[]){ rt_s(&S537), t1377, rt_s(&S538), t1378, rt_s(&S539), t1379, rt_s(&S16) }); }));
  }
  wv v786_body = rt_bind(rt_chk(s676_branchFunction(up, v785_withElse, v783_t), "suspectExpr"), 0);
  RT_RET(({ wv t1380 = rt_text(v783_t); wv t1381 = rt_text(v786_body); wv t1382 = rt_text(rt_chk(w6_repeat(rt_s(&S478), rt_def(up->v643_indent, "indent")), "suspectExpr")); wv t1383 = rt_text(v784_subject); rt_cat(9, (wv[]){ rt_s(&S537), t1380, rt_s(&S540), t1381, rt_s(&S2), t1382, rt_s(&S541), t1383, rt_s(&S16) }); }));
}

static wv s682_callJs(struct fr141 *up, wv v787_callee, wv v788_args) {
  rt_enter("callJs");
  v787_callee = rt_bind(v787_callee, 0);
  v788_args = rt_bind(v788_args, 0);
  wv v789_fn = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v787_callee), rt_s(&S164))) ? rt_cat(2, (wv[]){ rt_s(&S542), rt_text(rt_get(&F_name, v787_callee)) }) : (rt_cond(rt_eq(rt_get(&F_k, v787_callee), rt_s(&S198))) ? ({ wv t1384 = rt_text(rt_chk(s651_modId(up, rt_chk(s654_stolenFrom(up, rt_get(&F_rec, v787_callee)), "callJs")), "callJs")); wv t1385 = rt_text(rt_get(&F_field, v787_callee)); rt_cat(4, (wv[]){ rt_s(&S94), t1384, rt_s(&S12), t1385 }); }) : rt_chk(s653_id(up, rt_get(&F_name, v787_callee)), "callJs"))), 0);
  RT_RET(({ wv t1386 = rt_text(v789_fn); wv t1387 = rt_text(std_join(v788_args, rt_s(&S390))); rt_cat(4, (wv[]){ t1386, rt_s(&S90), t1387, rt_s(&S16) }); }));
}

static wv s683_exprList(struct fr141 *up, wv v790_list) {
  rt_enter("exprList");
  v790_list = rt_bind(v790_list, 0);
  wv v791_js = rt_arr(0, NULL, 1);
  wv v792_k = rt_i(0);
  while (rt_cond(({ wv t1388 = v792_k; wv t1389 = std_length(v790_list); rt_lt(t1388, t1389); }))) {
    (void)({ wv t1390 = v791_js; wv t1391 = rt_chk(s684_expr(up, rt_idx(v792_k, v790_list)), "exprList"); std_push(t1390, t1391); });
    v792_k = rt_add(v792_k, rt_i(1));
  }
  RT_RET(std_join(v791_js, rt_s(&S390)));
}

static wv s684_expr(struct fr141 *up, wv v793_e) {
  rt_enter("expr");
  v793_e = rt_bind(v793_e, 0);
  wv v794_kind = rt_bind(rt_get(&F_k, v793_e), 0);
  if (rt_cond(rt_eq(v794_kind, rt_s(&S106)))) {
    RT_RET((rt_cond(rt_lt(rt_get(&F_v, v793_e), rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S90), rt_text(rt_get(&F_v, v793_e)), rt_s(&S16) }) : rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v793_e)) })));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S109)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S543), rt_text(rt_chk(w6_jsFloat(rt_get(&F_v, v793_e)), "expr")), rt_s(&S16) }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S179)))) {
    wv v795_parts = rt_arr(0, NULL, 1);
    wv v796_k = rt_i(0);
    while (rt_cond(({ wv t1392 = v796_k; wv t1393 = std_length(rt_get(&F_parts, v793_e)); rt_lt(t1392, t1393); }))) {
      wv v797_part = rt_bind(({ wv t1394 = v796_k; wv t1395 = rt_get(&F_parts, v793_e); rt_idx(t1394, t1395); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v797_part), rt_s(&S194)))) {
        if (rt_cond(rt_ne(v797_part, rt_s(&S13)))) {
          (void)({ wv t1396 = v795_parts; wv t1397 = rt_chk(w1_quote(v797_part), "expr"); std_push(t1396, t1397); });
        }
      } else {
        (void)({ wv t1398 = v795_parts; wv t1399 = rt_cat(3, (wv[]){ rt_s(&S544), rt_text(rt_chk(s684_expr(up, v797_part), "expr")), rt_s(&S16) }); std_push(t1398, t1399); });
      }
      v796_k = rt_add(v796_k, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v795_parts), rt_i(0)))) {
      RT_RET(rt_s(&S545));
    }
    if (rt_cond(rt_eq(std_length(v795_parts), rt_i(1)))) {
      RT_RET(rt_idx(rt_i(0), v795_parts));
    }
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S90), rt_text(std_join(v795_parts, rt_s(&S546))), rt_s(&S16) }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S291)))) {
    RT_RET(rt_chk(w6_jsBool(rt_get(&F_v, v793_e)), "expr"));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S292)))) {
    RT_RET(rt_s(&S292));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S293)))) {
    RT_RET(rt_s(&S547));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S294)))) {
    RT_RET(rt_s(&S548));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S264)))) {
    RT_RET(rt_chk(s653_id(up, rt_get(&F_name, v793_e)), "expr"));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S164)))) {
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S542), rt_text(rt_get(&F_name, v793_e)) }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S195)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S505), rt_text(rt_chk(s683_exprList(up, rt_get(&F_items, v793_e)), "expr")), rt_s(&S506) }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S196)))) {
    RT_RET(rt_chk(s667_record(up, v793_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S197)))) {
    RT_RET(({ wv t1400 = rt_text(rt_chk(s684_expr(up, rt_get(&F_index, v793_e)), "expr")); wv t1401 = rt_text(rt_chk(s684_expr(up, rt_get(&F_arr, v793_e)), "expr")); rt_cat(5, (wv[]){ rt_s(&S549), t1400, rt_s(&S390), t1401, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S198)))) {
    wv v798_lin = rt_bind(rt_chk(s656_lineupJs(up, rt_get(&F_rec, v793_e)), "expr"), 0);
    if (rt_cond(rt_ne(v798_lin, RT_NULL))) {
      RT_RET(({ wv t1402 = rt_text(v798_lin); wv t1403 = rt_text(rt_get(&F_field, v793_e)); rt_cat(3, (wv[]){ t1402, rt_s(&S550), t1403 }); }));
    }
    wv v799_lib = rt_bind(rt_chk(s654_stolenFrom(up, rt_get(&F_rec, v793_e)), "expr"), 0);
    if (rt_cond(rt_ne(v799_lib, RT_NULL))) {
      RT_RET(({ wv t1404 = rt_text(rt_chk(s651_modId(up, v799_lib), "expr")); wv t1405 = rt_text(rt_get(&F_field, v793_e)); rt_cat(4, (wv[]){ rt_s(&S94), t1404, rt_s(&S12), t1405 }); }));
    }
    RT_RET(({ wv t1406 = rt_text(rt_get(&F_field, v793_e)); wv t1407 = rt_text(rt_chk(s684_expr(up, rt_get(&F_rec, v793_e)), "expr")); rt_cat(5, (wv[]){ rt_s(&S551), t1406, rt_s(&S511), t1407, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S199)))) {
    RT_RET(({ wv t1408 = rt_text(rt_chk(w6_unFn(rt_get(&F_op, v793_e)), "expr")); wv t1409 = rt_text(rt_chk(s684_expr(up, rt_get(&F_e, v793_e)), "expr")); rt_cat(5, (wv[]){ rt_s(&S552), t1408, rt_s(&S90), t1409, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S200)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v793_e), rt_s(&S82)), "||") || rt_bool(rt_eq(rt_get(&F_op, v793_e), rt_s(&S83)), "||")))) {
      RT_RET(({ wv t1410 = rt_text(rt_chk(s684_expr(up, rt_get(&F_l, v793_e)), "expr")); wv t1411 = rt_text(rt_get(&F_op, v793_e)); wv t1412 = rt_text(rt_get(&F_op, v793_e)); wv t1413 = rt_text(rt_chk(s684_expr(up, rt_get(&F_r, v793_e)), "expr")); wv t1414 = rt_text(rt_get(&F_op, v793_e)); rt_cat(11, (wv[]){ rt_s(&S553), t1410, rt_s(&S554), t1411, rt_s(&S555), t1412, rt_s(&S556), t1413, rt_s(&S554), t1414, rt_s(&S557) }); }));
    }
    RT_RET(({ wv t1415 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v793_e)), "expr")); wv t1416 = rt_text(rt_chk(s684_expr(up, rt_get(&F_l, v793_e)), "expr")); wv t1417 = rt_text(rt_chk(s684_expr(up, rt_get(&F_r, v793_e)), "expr")); rt_cat(7, (wv[]){ rt_s(&S552), t1415, rt_s(&S90), t1416, rt_s(&S390), t1417, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S201)))) {
    RT_RET(({ wv t1418 = rt_text(rt_chk(s684_expr(up, rt_get(&F_c, v793_e)), "expr")); wv t1419 = rt_text(rt_chk(s684_expr(up, rt_get(&F_a, v793_e)), "expr")); wv t1420 = rt_text(rt_chk(s684_expr(up, rt_get(&F_b, v793_e)), "expr")); rt_cat(7, (wv[]){ rt_s(&S558), t1418, rt_s(&S559), t1419, rt_s(&S531), t1420, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S191)))) {
    wv v800_args = rt_arr(0, NULL, 1);
    wv v801_k = rt_i(0);
    while (rt_cond(({ wv t1421 = v801_k; wv t1422 = std_length(rt_get(&F_args, v793_e)); rt_lt(t1421, t1422); }))) {
      (void)({ wv t1425 = v800_args; wv t1426 = rt_chk(s684_expr(up, ({ wv t1423 = v801_k; wv t1424 = rt_get(&F_args, v793_e); rt_idx(t1423, t1424); })), "expr"); std_push(t1425, t1426); });
      v801_k = rt_add(v801_k, rt_i(1));
    }
    RT_RET(({ wv t1427 = rt_get(&F_callee, v793_e); wv t1428 = v800_args; rt_chk(s682_callJs(up, t1427, t1428), "expr"); }));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S193)))) {
    RT_RET(rt_chk(s677_chainExpr(up, v793_e), "expr"));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S51)))) {
    RT_RET(rt_chk(s681_suspectExpr(up, v793_e), "expr"));
  }
  if (rt_cond(rt_eq(v794_kind, rt_s(&S526)))) {
    RT_RET(rt_get(&F_js, v793_e));
  }
  rt_tantrum(rt_cat(2, (wv[]){ rt_s(&S560), rt_text(v794_kind) }));
  RT_RET(RT_NULL);
}

static wv w7_binFn(wv v804_op) {
  rt_enter("binFn");
  v804_op = rt_bind(v804_op, 0);
  wv v805_k = rt_i(0);
  while (rt_cond(({ wv t1441 = ({ wv t1439 = v805_k; wv t1440 = rt_def(w7_binOps, "binOps"); rt_idx(t1439, t1440); }); wv t1442 = v804_op; rt_ne(t1441, t1442); }))) {
    v805_k = rt_add(v805_k, rt_i(1));
  }
  RT_RET(({ wv t1443 = v805_k; wv t1444 = rt_def(w7_binFns, "binFns"); rt_idx(t1443, t1444); }));
}

static wv w7_modeOf(wv v806_x) {
  rt_enter("modeOf");
  v806_x = rt_bind(v806_x, 0);
  RT_RET((rt_cond(rt_get(&F_isShifty, v806_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isFickle, v806_x)) ? rt_s(&S37) : rt_s(&S340))));
}

static wv w7_unFn(wv v807_op) {
  rt_enter("unFn");
  v807_op = rt_bind(v807_op, 0);
  RT_RET((rt_cond(rt_eq(v807_op, rt_s(&S72))) ? rt_s(&S472) : (rt_cond(rt_eq(v807_op, rt_s(&S87))) ? rt_s(&S473) : rt_s(&S474))));
}

static wv w7_repeat(wv v808_s, wv v809_count) {
  rt_enter("repeat");
  v808_s = rt_bind(v808_s, 0);
  v809_count = rt_bind(v809_count, 0);
  wv v810_out = rt_s(&S13);
  wv v811_k = rt_i(0);
  while (rt_cond(rt_lt(v811_k, v809_count))) {
    v810_out = rt_add(v810_out, v808_s);
    v811_k = rt_add(v811_k, rt_i(1));
  }
  RT_RET(v810_out);
}

static wv w7_utf8Length(wv v812_s) {
  rt_enter("utf8Length");
  v812_s = rt_bind(v812_s, 0);
  wv v813_cs = rt_bind(std_split(v812_s, rt_s(&S13)), 0);
  wv v814_n = rt_i(0);
  wv v815_k = rt_i(0);
  while (rt_cond(({ wv t1445 = v815_k; wv t1446 = std_length(v813_cs); rt_lt(t1445, t1446); }))) {
    wv v816_c = rt_bind(std_code(rt_idx(v815_k, v813_cs)), 0);
    wv t1447 = (rt_cond(rt_lt(v816_c, rt_i(128))) ? rt_i(1) : (rt_cond(rt_lt(v816_c, rt_i(2048))) ? rt_i(2) : (rt_cond(rt_lt(v816_c, rt_i(65536))) ? rt_i(3) : rt_i(4))));
    v814_n = rt_add(v814_n, t1447);
    v815_k = rt_add(v815_k, rt_i(1));
  }
  RT_RET(v814_n);
}

static wv w7_cString(wv v817_s) {
  rt_enter("cString");
  v817_s = rt_bind(v817_s, 0);
  wv v818_cs = rt_bind(std_split(v817_s, rt_s(&S13)), 0);
  wv v819_out = rt_s(&S18);
  wv v820_k = rt_i(0);
  while (rt_cond(({ wv t1448 = v820_k; wv t1449 = std_length(v818_cs); rt_lt(t1448, t1449); }))) {
    wv v821_ch = rt_bind(rt_idx(v820_k, v818_cs), 0);
    wv v822_c = rt_bind(std_code(v821_ch), 0);
    if (rt_cond(rt_eq(v821_ch, rt_s(&S18)))) {
      v819_out = rt_add(v819_out, rt_s(&S564));
    } else if (rt_cond(rt_eq(v821_ch, rt_s(&S19)))) {
      v819_out = rt_add(v819_out, rt_s(&S565));
    } else if (rt_cond(rt_eq(v821_ch, rt_s(&S2)))) {
      v819_out = rt_add(v819_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v821_ch, rt_s(&S1)))) {
      v819_out = rt_add(v819_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v821_ch, rt_s(&S3)))) {
      v819_out = rt_add(v819_out, rt_s(&S21));
    } else if (rt_cond(rt_b(rt_bool(rt_lt(v822_c, rt_i(32)), "||") || rt_bool(rt_eq(v822_c, rt_i(127)), "||")))) {
      v819_out = ({ wv t1453 = v819_out; wv t1454 = ({ wv t1450 = rt_text(std_floor(rt_div(v822_c, rt_i(64)))); wv t1451 = rt_text(rt_mod(std_floor(rt_div(v822_c, rt_i(8))), rt_i(8))); wv t1452 = rt_text(rt_mod(v822_c, rt_i(8))); rt_cat(4, (wv[]){ rt_s(&S19), t1450, t1451, t1452 }); }); rt_add(t1453, t1454); });
    } else {
      v819_out = rt_add(v819_out, v821_ch);
    }
    v820_k = rt_add(v820_k, rt_i(1));
  }
  RT_RET(rt_add(v819_out, rt_s(&S18)));
}

static wv w7_wstrInit(wv v823_s) {
  rt_enter("wstrInit");
  v823_s = rt_bind(v823_s, 0);
  wv v824_bytes = rt_bind(rt_chk(w7_utf8Length(v823_s), "wstrInit"), 0);
  wv v825_cps = rt_bind(std_length(v823_s), 0);
  RT_RET(({ wv t1455 = rt_text(v824_bytes); wv t1456 = rt_text(v825_cps); wv t1457 = rt_text((rt_cond(rt_eq(v824_bytes, v825_cps)) ? rt_i(1) : rt_i(0))); wv t1458 = rt_text(rt_chk(w7_cString(v823_s), "wstrInit")); rt_cat(9, (wv[]){ rt_s(&S566), t1455, rt_s(&S390), t1456, rt_s(&S390), t1457, rt_s(&S567), t1458, rt_s(&S568) }); }));
}

static wv w7_values(wv v826_cs) {
  rt_enter("values");
  RT_RET((rt_cond(rt_eq(std_length(v826_cs), rt_i(0))) ? rt_s(&S569) : rt_cat(3, (wv[]){ rt_s(&S570), rt_text(std_join(v826_cs, rt_s(&S390))), rt_s(&S568) })));
}

static wv w7_generateC(wv p_v827_modules, wv p_v828_runtime) {
  rt_enter("generateC");
  struct fr185 fr = { 0 };
  fr.v827_modules = rt_bind(p_v827_modules, 0);
  fr.v828_runtime = rt_bind(p_v828_runtime, 0);
  fr.v829_n = rt_i(0);
  fr.v830_tmps = rt_i(0);
  fr.v831_strings = rt_arr(0, NULL, 1);
  fr.v832_fields = rt_arr(0, NULL, 1);
  fr.v833_shapes = rt_arr(0, NULL, 1);
  fr.v834_lineupDefs = rt_arr(0, NULL, 1);
  fr.v835_globals = rt_arr(0, NULL, 1);
  fr.v836_frames = rt_arr(0, NULL, 1);
  fr.v837_functions = rt_arr(0, NULL, 1);
  fr.v838_out = rt_arr(0, NULL, 1);
  fr.v839_indent = rt_i(1);
  fr.v840_loops = rt_i(0);
  fr.v841_iife = rt_b(0);
  fr.v842_decls = rt_arr(0, NULL, 1);
  fr.v843_fns = rt_arr(0, NULL, 1);
  fr.v844_tops = rt_arr(0, NULL, 1);
  fr.v845_slots = rt_arr(0, NULL, 1);
  fr.v846_curMod = rt_i(0);
  fr.v847_mod = rt_s(&S13);
  fr.v848_curSlot = rt_arr(0, NULL, 1);
  fr.v849_stolen = rt_arr(0, NULL, 1);
  fr.v850_ownLineups = rt_arr(0, NULL, 1);
  fr.v851_cur = rt_i(0);
  wv v911_mi = rt_i(0);
  while (rt_cond(({ wv t2390 = v911_mi; wv t2391 = std_length(fr.v827_modules); rt_lt(t2390, t2391); }))) {
    (void)rt_chk(s866_analyze(&fr, v911_mi), "generateC");
    v911_mi = rt_add(v911_mi, rt_i(1));
  }
  v911_mi = rt_i(0);
  while (rt_cond(({ wv t2392 = v911_mi; wv t2393 = std_length(fr.v827_modules); rt_lt(t2392, t2393); }))) {
    wv v1185_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v911_mi, fr.v827_modules))), 0);
    wv v1186_li = rt_i(0);
    while (rt_cond(({ wv t2394 = v1186_li; wv t2395 = std_length(v1185_list); rt_lt(t2394, t2395); }))) {
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1186_li, v1185_list)), rt_s(&S53)))) {
        (void)({ wv t2396 = rt_idx(v1186_li, v1185_list); wv t2397 = rt_chk(s852_modId(&fr, v911_mi), "generateC"); rt_chk(s872_lineupDef(&fr, t2396, t2397), "generateC"); });
      }
      v1186_li = rt_add(v1186_li, rt_i(1));
    }
    v911_mi = rt_add(v911_mi, rt_i(1));
  }
  v911_mi = rt_i(0);
  while (rt_cond(({ wv t2398 = v911_mi; wv t2399 = std_length(fr.v827_modules); rt_lt(t2398, t2399); }))) {
    (void)rt_chk(s867_enter(&fr, v911_mi), "generateC");
    (void)rt_chk(s881_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v911_mi, fr.v827_modules)))), "generateC");
    v911_mi = rt_add(v911_mi, rt_i(1));
  }
  RT_RET(rt_chk(s910_assemble(&fr), "generateC"));
}

static wv s852_modId(struct fr185 *up, wv v912_m) {
  rt_enter("modId");
  v912_m = rt_bind(v912_m, 0);
  RT_RET((rt_cond(({ wv t1459 = v912_m; wv t1460 = rt_sub(std_length(up->v827_modules), rt_i(1)); rt_eq(t1459, t1460); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v912_m, rt_i(1))) })));
}

static wv s853_hasFrame(struct fr185 *up, wv v913_f) {
  rt_enter("hasFrame");
  v913_f = rt_bind(v913_f, 0);
  RT_RET(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1461 = v913_f; wv t1462 = rt_def(up->v843_fns, "fns"); rt_idx(t1461, t1462); }))), "&&") && rt_bool(rt_gt(rt_get(&F_children, ({ wv t1463 = v913_f; wv t1464 = rt_def(up->v843_fns, "fns"); rt_idx(t1463, t1464); })), rt_i(0)), "&&")));
}

static wv s854_newScope(struct fr185 *up, wv v914_parent) {
  rt_enter("newScope");
  v914_parent = rt_bind(v914_parent, 0);
  wv v915_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K41, (wv[]){ v914_parent, v915_names }));
}

static wv s855_put(struct fr185 *up, wv v916_scope, wv v917_name, wv v918_kind, wv v919_d) {
  rt_enter("put");
  v916_scope = rt_bind(v916_scope, 0);
  v917_name = rt_bind(v917_name, 0);
  v918_kind = rt_bind(v918_kind, 0);
  v919_d = rt_bind(v919_d, 0);
  (void)({ wv t1465 = rt_get(&F_names, v916_scope); wv t1466 = rt_rec(0, 3, K52, (wv[]){ v917_name, v918_kind, v919_d }); std_push(t1465, t1466); });
  RT_RET(RT_NULL);
}

static wv s856_find(struct fr185 *up, wv v920_scope, wv v921_name) {
  rt_enter("find");
  v921_name = rt_bind(v921_name, 0);
  wv v922_s = v920_scope;
  while (rt_cond(rt_ne(v922_s, RT_NULL))) {
    wv v923_ns = rt_get(&F_names, v922_s);
    wv v924_k = rt_i(0);
    while (rt_cond(({ wv t1467 = v924_k; wv t1468 = std_length(v923_ns); rt_lt(t1467, t1468); }))) {
      if (rt_cond(({ wv t1469 = rt_get(&F_name, rt_idx(v924_k, v923_ns)); wv t1470 = v921_name; rt_eq(t1469, t1470); }))) {
        RT_RET(rt_idx(v924_k, v923_ns));
      }
      v924_k = rt_add(v924_k, rt_i(1));
    }
    v922_s = rt_get(&F_parent, v922_s);
  }
  RT_RET(RT_NULL);
}

static wv s857_kindOf(struct fr185 *up, wv v925_scope, wv v926_name) {
  rt_enter("kindOf");
  v925_scope = rt_bind(v925_scope, 0);
  v926_name = rt_bind(v926_name, 0);
  wv v927_entry = rt_bind(rt_chk(s856_find(up, v925_scope, v926_name), "kindOf"), 0);
  RT_RET((rt_cond(rt_eq(v927_entry, RT_NULL)) ? RT_NULL : rt_get(&F_kind, v927_entry)));
}

static wv s858_newFn(struct fr185 *up, wv v928_name, wv v929_parent) {
  rt_enter("newFn");
  v928_name = rt_bind(v928_name, 0);
  v929_parent = rt_bind(v929_parent, 0);
  wv v930_f = ({ wv t1473 = v928_name; wv t1474 = v929_parent; wv t1475 = (rt_cond(rt_eq(v929_parent, RT_NULL)) ? rt_i(0) : rt_add(rt_get(&F_depth, ({ wv t1471 = v929_parent; wv t1472 = rt_def(up->v843_fns, "fns"); rt_idx(t1471, t1472); })), rt_i(1))); wv t1476 = rt_eq(v929_parent, RT_NULL); rt_rec(1, 6, K53, (wv[]){ t1473, t1474, t1475, t1476, rt_i(0), rt_s(&S13) }); });
  (void)({ wv t1477 = rt_def(up->v843_fns, "fns"); wv t1478 = v930_f; std_push(t1477, t1478); });
  RT_RET(rt_sub(std_length(rt_def(up->v843_fns, "fns")), rt_i(1)));
}

static wv s859_bind(struct fr185 *up, wv v931_kind, wv v932_name, wv v933_f, wv v934_atTop, wv v935_mode) {
  rt_enter("bind");
  v931_kind = rt_bind(v931_kind, 0);
  v932_name = rt_bind(v932_name, 0);
  v933_f = rt_bind(v933_f, 0);
  v934_atTop = rt_bind(v934_atTop, 0);
  v935_mode = rt_bind(v935_mode, 0);
  wv v936_cname = ({ wv t1479 = rt_text(rt_def(up->v847_mod, "mod")); wv t1480 = rt_text(v932_name); rt_cat(4, (wv[]){ rt_s(&S94), t1479, rt_s(&S12), t1480 }); });
  if (rt_cond(rt_not(v934_atTop))) {
    v936_cname = ({ wv t1483 = (rt_cond(rt_eq(v931_kind, rt_s(&S33))) ? rt_s(&S367) : rt_s(&S571)); wv t1484 = ({ wv t1481 = rt_text(rt_def(up->v829_n, "n")); wv t1482 = rt_text(v932_name); rt_cat(3, (wv[]){ t1481, rt_s(&S12), t1482 }); }); rt_add(t1483, t1484); });
    up->v829_n = rt_add(rt_def(up->v829_n, "n"), rt_i(1));
  }
  wv v937_d = ({ wv t1485 = v931_kind; wv t1486 = v932_name; wv t1487 = v933_f; wv t1488 = v936_cname; wv t1489 = rt_eq(v935_mode, rt_s(&S37)); rt_rec(1, 7, K54, (wv[]){ t1485, t1486, t1487, t1488, t1489, rt_b(0), RT_NULL }); });
  (void)({ wv t1490 = rt_def(up->v842_decls, "decls"); wv t1491 = v937_d; std_push(t1490, t1491); });
  if (rt_cond(rt_b(rt_bool(rt_get(&F_top, ({ wv t1492 = v933_f; wv t1493 = rt_def(up->v843_fns, "fns"); rt_idx(t1492, t1493); })), "&&") && rt_bool(rt_ne(v931_kind, rt_s(&S33)), "&&")))) {
    (void)({ wv t1494 = rt_def(up->v835_globals, "globals"); wv t1495 = v936_cname; std_push(t1494, t1495); });
  }
  RT_RET(rt_sub(std_length(rt_def(up->v842_decls, "decls")), rt_i(1)));
}

static wv s860_ablock(struct fr185 *up, wv v938_b, wv v939_scope, wv v940_f, wv v941_topScope) {
  rt_enter("ablock");
  v939_scope = rt_bind(v939_scope, 0);
  v940_f = rt_bind(v940_f, 0);
  v941_topScope = rt_bind(v941_topScope, 0);
  wv v942_atTop = rt_eq(v939_scope, v941_topScope);
  wv v943_list = rt_bind(rt_get(&F_stmts, v938_b), 0);
  wv v944_k = rt_i(0);
  while (rt_cond(({ wv t1496 = v944_k; wv t1497 = std_length(v943_list); rt_lt(t1496, t1497); }))) {
    wv v945_s = rt_bind(rt_idx(v944_k, v943_list), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v945_s), rt_s(&S33)))) {
      wv v946_d = rt_bind(({ wv t1498 = rt_get(&F_name, v945_s); wv t1499 = v940_f; wv t1500 = v942_atTop; rt_chk(s859_bind(up, rt_s(&S33), t1498, t1499, t1500, rt_s(&S13)), "ablock"); }), 0);
      wv v947_child = rt_bind(({ wv t1501 = rt_get(&F_name, v945_s); wv t1502 = v940_f; rt_chk(s858_newFn(up, t1501, t1502), "ablock"); }), 0);
      ({ wv t1507 = rt_get(&F_cname, ({ wv t1503 = v946_d; wv t1504 = rt_def(up->v842_decls, "decls"); rt_idx(t1503, t1504); })); wv t1508 = ({ wv t1505 = v947_child; wv t1506 = rt_def(up->v843_fns, "fns"); rt_idx(t1505, t1506); }); rt_set_mem(t1507, &F_cname, t1508); });
      ({ wv t1511 = v947_child; wv t1512 = ({ wv t1509 = v946_d; wv t1510 = rt_def(up->v842_decls, "decls"); rt_idx(t1509, t1510); }); rt_set_mem(t1511, &F_schemeFn, t1512); });
      rt_upd_mem(rt_i(1), &F_children, ({ wv t1513 = v940_f; wv t1514 = rt_def(up->v843_fns, "fns"); rt_idx(t1513, t1514); }), rt_add);
      ({ wv t1515 = v946_d; wv t1516 = rt_get(&F_namePos, v945_s); wv t1517 = rt_def(up->v848_curSlot, "curSlot"); rt_set_idx(t1515, t1516, t1517); });
      (void)({ wv t1518 = v939_scope; wv t1519 = rt_get(&F_name, v945_s); wv t1520 = v946_d; rt_chk(s855_put(up, t1518, t1519, rt_s(&S33), t1520), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v945_s), rt_s(&S53)))) {
      (void)({ wv t1521 = v939_scope; wv t1522 = rt_get(&F_name, v945_s); rt_chk(s855_put(up, t1521, t1522, rt_s(&S53), RT_NULL), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v945_s), rt_s(&S35)))) {
      wv v948_d = rt_bind(({ wv t1523 = rt_get(&F_name, v945_s); wv t1524 = v940_f; wv t1525 = v942_atTop; wv t1526 = rt_chk(w7_modeOf(v945_s), "ablock"); rt_chk(s859_bind(up, rt_s(&S35), t1523, t1524, t1525, t1526), "ablock"); }), 0);
      ({ wv t1527 = v948_d; wv t1528 = rt_get(&F_namePos, v945_s); wv t1529 = rt_def(up->v848_curSlot, "curSlot"); rt_set_idx(t1527, t1528, t1529); });
      (void)({ wv t1530 = v939_scope; wv t1531 = rt_get(&F_name, v945_s); wv t1532 = v948_d; rt_chk(s855_put(up, t1530, t1531, rt_s(&S35), t1532), "ablock"); });
    }
    v944_k = rt_add(v944_k, rt_i(1));
  }
  v944_k = rt_i(0);
  while (rt_cond(({ wv t1533 = v944_k; wv t1534 = std_length(v943_list); rt_lt(t1533, t1534); }))) {
    (void)({ wv t1535 = rt_idx(v944_k, v943_list); wv t1536 = v939_scope; wv t1537 = v940_f; wv t1538 = v941_topScope; rt_chk(s861_astmt(up, t1535, t1536, t1537, t1538), "ablock"); });
    v944_k = rt_add(v944_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v938_b), RT_NULL))) {
    (void)({ wv t1539 = rt_get(&F_value, v938_b); wv t1540 = v939_scope; wv t1541 = v940_f; rt_chk(s865_aexpr(up, t1539, t1540, t1541), "ablock"); });
  }
  RT_RET(RT_NULL);
}

static wv s861_astmt(struct fr185 *up, wv v949_s, wv v950_scope, wv v951_f, wv v952_topScope) {
  rt_enter("astmt");
  v950_scope = rt_bind(v950_scope, 0);
  v951_f = rt_bind(v951_f, 0);
  v952_topScope = rt_bind(v952_topScope, 0);
  wv v953_kind = rt_bind(rt_get(&F_k, v949_s), 0);
  if (rt_cond(rt_eq(v953_kind, rt_s(&S33)))) {
    wv v954_d = rt_bind(({ wv t1542 = rt_get(&F_namePos, v949_s); wv t1543 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t1542, t1543); }), 0);
    wv v955_child = rt_bind(rt_get(&F_schemeFn, ({ wv t1544 = v954_d; wv t1545 = rt_def(up->v842_decls, "decls"); rt_idx(t1544, t1545); })), 0);
    wv v956_ps = rt_bind(rt_chk(s854_newScope(up, v950_scope), "astmt"), 0);
    wv v957_k = rt_i(0);
    while (rt_cond(({ wv t1546 = v957_k; wv t1547 = std_length(rt_get(&F_params, v949_s)); rt_lt(t1546, t1547); }))) {
      wv v958_p = rt_bind(({ wv t1548 = v957_k; wv t1549 = rt_get(&F_params, v949_s); rt_idx(t1548, t1549); }), 0);
      wv v959_pd = rt_bind(({ wv t1550 = rt_get(&F_name, v958_p); wv t1551 = v955_child; wv t1552 = rt_chk(w7_modeOf(v958_p), "astmt"); rt_chk(s859_bind(up, rt_s(&S378), t1550, t1551, rt_b(0), t1552), "astmt"); }), 0);
      ({ wv t1553 = v959_pd; wv t1554 = rt_get(&F_pos, v958_p); wv t1555 = rt_def(up->v848_curSlot, "curSlot"); rt_set_idx(t1553, t1554, t1555); });
      (void)({ wv t1556 = v956_ps; wv t1557 = rt_get(&F_name, v958_p); wv t1558 = v959_pd; rt_chk(s855_put(up, t1556, t1557, rt_s(&S378), t1558), "astmt"); });
      v957_k = rt_add(v957_k, rt_i(1));
    }
    (void)({ wv t1559 = rt_get(&F_body, v949_s); wv t1560 = rt_chk(s854_newScope(up, v956_ps), "astmt"); wv t1561 = v955_child; wv t1562 = v952_topScope; rt_chk(s860_ablock(up, t1559, t1560, t1561, t1562), "astmt"); });
  } else if (rt_cond(rt_eq(v953_kind, rt_s(&S41)))) {
    (void)({ wv t1563 = rt_get(&F_cond, v949_s); wv t1564 = v950_scope; wv t1565 = v951_f; rt_chk(s865_aexpr(up, t1563, t1564, t1565), "astmt"); });
    (void)({ wv t1566 = rt_get(&F_body, v949_s); wv t1567 = rt_chk(s854_newScope(up, v950_scope), "astmt"); wv t1568 = v951_f; wv t1569 = v952_topScope; rt_chk(s860_ablock(up, t1566, t1567, t1568, t1569), "astmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v953_kind, rt_s(&S34)), "||") || rt_bool(rt_eq(v953_kind, rt_s(&S44)), "||")), "||") || rt_bool(rt_eq(v953_kind, rt_s(&S185)), "||")), "||") || rt_bool(rt_eq(v953_kind, rt_s(&S35)), "||")))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v949_s), RT_NULL))) {
      (void)({ wv t1570 = rt_get(&F_e, v949_s); wv t1571 = v950_scope; wv t1572 = v951_f; rt_chk(s865_aexpr(up, t1570, t1571, t1572), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v953_kind, rt_s(&S184)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v949_s)), rt_s(&S192)))) {
      (void)({ wv t1573 = rt_get(&F_arg, rt_get(&F_e, v949_s)); wv t1574 = v950_scope; wv t1575 = v951_f; rt_chk(s865_aexpr(up, t1573, t1574, t1575), "astmt"); });
    } else {
      (void)({ wv t1576 = rt_get(&F_e, v949_s); wv t1577 = v950_scope; wv t1578 = v951_f; rt_chk(s865_aexpr(up, t1576, t1577, t1578), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v953_kind, rt_s(&S38)))) {
    (void)({ wv t1579 = rt_get(&F_e, v949_s); wv t1580 = v950_scope; wv t1581 = v951_f; rt_chk(s865_aexpr(up, t1579, t1580, t1581), "astmt"); });
    (void)({ wv t1582 = rt_get(&F_target, v949_s); wv t1583 = v950_scope; wv t1584 = v951_f; rt_chk(s863_atarget(up, t1582, t1583, t1584), "astmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v949_s), RT_NULL))) {
    wv v960_h = rt_bind(rt_get(&F_handler, v949_s), 0);
    wv v961_hs = rt_bind(rt_chk(s854_newScope(up, v950_scope), "astmt"), 0);
    if (rt_cond(rt_ne(rt_get(&F_param, v960_h), RT_NULL))) {
      wv v962_wd = rt_bind(({ wv t1585 = rt_get(&F_param, v960_h); wv t1586 = v951_f; rt_chk(s859_bind(up, rt_s(&S45), t1585, t1586, rt_b(0), rt_s(&S13)), "astmt"); }), 0);
      ({ wv t1587 = v962_wd; wv t1588 = rt_get(&F_paramPos, v960_h); wv t1589 = rt_def(up->v848_curSlot, "curSlot"); rt_set_idx(t1587, t1588, t1589); });
      (void)({ wv t1590 = v961_hs; wv t1591 = rt_get(&F_param, v960_h); wv t1592 = v962_wd; rt_chk(s855_put(up, t1590, t1591, rt_s(&S45), t1592), "astmt"); });
    }
    (void)({ wv t1593 = rt_get(&F_body, v960_h); wv t1594 = rt_chk(s854_newScope(up, v961_hs), "astmt"); wv t1595 = v951_f; wv t1596 = v952_topScope; rt_chk(s860_ablock(up, t1593, t1594, t1595, t1596), "astmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s862_use(struct fr185 *up, wv v963_x, wv v964_scope, wv v965_f) {
  rt_enter("use");
  v963_x = rt_bind(v963_x, 0);
  v964_scope = rt_bind(v964_scope, 0);
  v965_f = rt_bind(v965_f, 0);
  wv v966_d = rt_bind(rt_get(&F_d, ({ wv t1597 = v964_scope; wv t1598 = rt_get(&F_name, v963_x); rt_chk(s856_find(up, t1597, t1598), "use"); })), 0);
  ({ wv t1599 = v966_d; wv t1600 = rt_get(&F_pos, v963_x); wv t1601 = rt_def(up->v848_curSlot, "curSlot"); rt_set_idx(t1599, t1600, t1601); });
  if (rt_cond(({ wv t1604 = rt_get(&F_fn, ({ wv t1602 = v966_d; wv t1603 = rt_def(up->v842_decls, "decls"); rt_idx(t1602, t1603); })); wv t1605 = v965_f; rt_ne(t1604, t1605); }))) {
    rt_set_mem(rt_b(1), &F_captured, ({ wv t1606 = v966_d; wv t1607 = rt_def(up->v842_decls, "decls"); rt_idx(t1606, t1607); }));
  }
  RT_RET(RT_NULL);
}

static wv s863_atarget(struct fr185 *up, wv v967_t, wv v968_scope, wv v969_f) {
  rt_enter("atarget");
  v967_t = rt_bind(v967_t, 0);
  v968_scope = rt_bind(v968_scope, 0);
  v969_f = rt_bind(v969_f, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v967_t), rt_s(&S264)))) {
    (void)rt_chk(s862_use(up, v967_t, v968_scope, v969_f), "atarget");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v967_t), rt_s(&S197)))) {
    (void)({ wv t1608 = rt_get(&F_index, v967_t); wv t1609 = v968_scope; wv t1610 = v969_f; rt_chk(s865_aexpr(up, t1608, t1609, t1610), "atarget"); });
  }
  (void)({ wv t1611 = (rt_cond(rt_eq(rt_get(&F_k, v967_t), rt_s(&S198))) ? rt_get(&F_rec, v967_t) : rt_get(&F_arr, v967_t)); wv t1612 = v968_scope; wv t1613 = v969_f; rt_chk(s865_aexpr(up, t1611, t1612, t1613), "atarget"); });
  RT_RET(RT_NULL);
}

static wv s864_notValue(struct fr185 *up, wv v970_x, wv v971_scope) {
  rt_enter("notValue");
  v971_scope = rt_bind(v971_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v970_x), rt_s(&S198)))) {
    RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v970_x)), rt_s(&S264)), "&&") && rt_bool(rt_eq(({ wv t1614 = v971_scope; wv t1615 = rt_get(&F_name, rt_get(&F_rec, v970_x)); rt_chk(s857_kindOf(up, t1614, t1615), "notValue"); }), rt_s(&S393)), "&&")));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v970_x), rt_s(&S264)))) {
    RT_RET(rt_b(0));
  }
  wv v972_kind = rt_bind(({ wv t1616 = v971_scope; wv t1617 = rt_get(&F_name, v970_x); rt_chk(s857_kindOf(up, t1616, t1617), "notValue"); }), 0);
  RT_RET(rt_b(rt_bool(rt_eq(v972_kind, rt_s(&S53)), "||") || rt_bool(rt_eq(v972_kind, rt_s(&S393)), "||")));
}

static wv s865_aexpr(struct fr185 *up, wv v973_e, wv v974_scope, wv v975_f) {
  rt_enter("aexpr");
  v974_scope = rt_bind(v974_scope, 0);
  v975_f = rt_bind(v975_f, 0);
  wv v976_kind = rt_bind(rt_get(&F_k, v973_e), 0);
  if (rt_cond(rt_eq(v976_kind, rt_s(&S179)))) {
    wv v977_k = rt_i(0);
    while (rt_cond(({ wv t1618 = v977_k; wv t1619 = std_length(rt_get(&F_parts, v973_e)); rt_lt(t1618, t1619); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t1620 = v977_k; wv t1621 = rt_get(&F_parts, v973_e); rt_idx(t1620, t1621); })), rt_s(&S194)))) {
        (void)({ wv t1624 = ({ wv t1622 = v977_k; wv t1623 = rt_get(&F_parts, v973_e); rt_idx(t1622, t1623); }); wv t1625 = v974_scope; wv t1626 = v975_f; rt_chk(s865_aexpr(up, t1624, t1625, t1626), "aexpr"); });
      }
      v977_k = rt_add(v977_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S264)))) {
    (void)rt_chk(s862_use(up, v973_e, v974_scope, v975_f), "aexpr");
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S195)))) {
    wv v978_k = rt_i(0);
    while (rt_cond(({ wv t1627 = v978_k; wv t1628 = std_length(rt_get(&F_items, v973_e)); rt_lt(t1627, t1628); }))) {
      (void)({ wv t1631 = ({ wv t1629 = v978_k; wv t1630 = rt_get(&F_items, v973_e); rt_idx(t1629, t1630); }); wv t1632 = v974_scope; wv t1633 = v975_f; rt_chk(s865_aexpr(up, t1631, t1632, t1633), "aexpr"); });
      v978_k = rt_add(v978_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S196)))) {
    wv v979_k = rt_i(0);
    while (rt_cond(({ wv t1634 = v979_k; wv t1635 = std_length(rt_get(&F_fields, v973_e)); rt_lt(t1634, t1635); }))) {
      (void)({ wv t1638 = rt_get(&F_e, ({ wv t1636 = v979_k; wv t1637 = rt_get(&F_fields, v973_e); rt_idx(t1636, t1637); })); wv t1639 = v974_scope; wv t1640 = v975_f; rt_chk(s865_aexpr(up, t1638, t1639, t1640), "aexpr"); });
      v979_k = rt_add(v979_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S197)))) {
    (void)({ wv t1641 = rt_get(&F_index, v973_e); wv t1642 = v974_scope; wv t1643 = v975_f; rt_chk(s865_aexpr(up, t1641, t1642, t1643), "aexpr"); });
    (void)({ wv t1644 = rt_get(&F_arr, v973_e); wv t1645 = v974_scope; wv t1646 = v975_f; rt_chk(s865_aexpr(up, t1644, t1645, t1646), "aexpr"); });
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S198)))) {
    if (rt_cond(rt_not(({ wv t1647 = rt_get(&F_rec, v973_e); wv t1648 = v974_scope; rt_chk(s864_notValue(up, t1647, t1648), "aexpr"); })))) {
      (void)({ wv t1649 = rt_get(&F_rec, v973_e); wv t1650 = v974_scope; wv t1651 = v975_f; rt_chk(s865_aexpr(up, t1649, t1650, t1651), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S199)))) {
    (void)({ wv t1652 = rt_get(&F_e, v973_e); wv t1653 = v974_scope; wv t1654 = v975_f; rt_chk(s865_aexpr(up, t1652, t1653, t1654), "aexpr"); });
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S200)))) {
    (void)({ wv t1655 = rt_get(&F_l, v973_e); wv t1656 = v974_scope; wv t1657 = v975_f; rt_chk(s865_aexpr(up, t1655, t1656, t1657), "aexpr"); });
    (void)({ wv t1658 = rt_get(&F_r, v973_e); wv t1659 = v974_scope; wv t1660 = v975_f; rt_chk(s865_aexpr(up, t1658, t1659, t1660), "aexpr"); });
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S201)))) {
    (void)({ wv t1661 = rt_get(&F_c, v973_e); wv t1662 = v974_scope; wv t1663 = v975_f; rt_chk(s865_aexpr(up, t1661, t1662, t1663), "aexpr"); });
    (void)({ wv t1664 = rt_get(&F_a, v973_e); wv t1665 = v974_scope; wv t1666 = v975_f; rt_chk(s865_aexpr(up, t1664, t1665, t1666), "aexpr"); });
    (void)({ wv t1667 = rt_get(&F_b, v973_e); wv t1668 = v974_scope; wv t1669 = v975_f; rt_chk(s865_aexpr(up, t1667, t1668, t1669), "aexpr"); });
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S191)))) {
    wv v980_k = rt_i(0);
    while (rt_cond(({ wv t1670 = v980_k; wv t1671 = std_length(rt_get(&F_args, v973_e)); rt_lt(t1670, t1671); }))) {
      (void)({ wv t1674 = ({ wv t1672 = v980_k; wv t1673 = rt_get(&F_args, v973_e); rt_idx(t1672, t1673); }); wv t1675 = v974_scope; wv t1676 = v975_f; rt_chk(s865_aexpr(up, t1674, t1675, t1676), "aexpr"); });
      v980_k = rt_add(v980_k, rt_i(1));
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_callee, v973_e)), rt_s(&S264)))) {
      ({ wv t1679 = rt_get(&F_d, ({ wv t1677 = v974_scope; wv t1678 = rt_get(&F_name, rt_get(&F_callee, v973_e)); rt_chk(s856_find(up, t1677, t1678), "aexpr"); })); wv t1680 = rt_get(&F_pos, rt_get(&F_callee, v973_e)); wv t1681 = rt_def(up->v848_curSlot, "curSlot"); rt_set_idx(t1679, t1680, t1681); });
    }
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S193)))) {
    wv v981_k = rt_i(0);
    while (rt_cond(({ wv t1682 = v981_k; wv t1683 = std_length(rt_get(&F_branches, v973_e)); rt_lt(t1682, t1683); }))) {
      (void)({ wv t1686 = rt_get(&F_cond, ({ wv t1684 = v981_k; wv t1685 = rt_get(&F_branches, v973_e); rt_idx(t1684, t1685); })); wv t1687 = v974_scope; wv t1688 = v975_f; rt_chk(s865_aexpr(up, t1686, t1687, t1688), "aexpr"); });
      (void)({ wv t1691 = rt_get(&F_body, ({ wv t1689 = v981_k; wv t1690 = rt_get(&F_branches, v973_e); rt_idx(t1689, t1690); })); wv t1692 = rt_chk(s854_newScope(up, v974_scope), "aexpr"); wv t1693 = v975_f; rt_chk(s860_ablock(up, t1691, t1692, t1693, RT_NULL), "aexpr"); });
      v981_k = rt_add(v981_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v973_e), RT_NULL))) {
      (void)({ wv t1694 = rt_get(&F_els, v973_e); wv t1695 = rt_chk(s854_newScope(up, v974_scope), "aexpr"); wv t1696 = v975_f; rt_chk(s860_ablock(up, t1694, t1695, t1696, RT_NULL), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v976_kind, rt_s(&S51)))) {
    (void)({ wv t1697 = rt_get(&F_subject, v973_e); wv t1698 = v974_scope; wv t1699 = v975_f; rt_chk(s865_aexpr(up, t1697, t1698, t1699), "aexpr"); });
    wv v982_k = rt_i(0);
    while (rt_cond(({ wv t1700 = v982_k; wv t1701 = std_length(rt_get(&F_alibis, v973_e)); rt_lt(t1700, t1701); }))) {
      (void)({ wv t1704 = rt_get(&F_body, ({ wv t1702 = v982_k; wv t1703 = rt_get(&F_alibis, v973_e); rt_idx(t1702, t1703); })); wv t1705 = rt_chk(s854_newScope(up, v974_scope), "aexpr"); wv t1706 = v975_f; rt_chk(s860_ablock(up, t1704, t1705, t1706, RT_NULL), "aexpr"); });
      v982_k = rt_add(v982_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v973_e), RT_NULL))) {
      (void)({ wv t1707 = rt_get(&F_els, v973_e); wv t1708 = rt_chk(s854_newScope(up, v974_scope), "aexpr"); wv t1709 = v975_f; rt_chk(s860_ablock(up, t1707, t1708, t1709, RT_NULL), "aexpr"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv s866_analyze(struct fr185 *up, wv v983_m) {
  rt_enter("analyze");
  v983_m = rt_bind(v983_m, 0);
  wv t1710 = rt_bind(v983_m, 1);
  (void)rt_def(up->v846_curMod, "curMod");
  up->v846_curMod = t1710;
  wv t1711 = rt_bind(rt_chk(s852_modId(up, v983_m), "analyze"), 1);
  (void)rt_def(up->v847_mod, "mod");
  up->v847_mod = t1711;
  wv v984_top = rt_bind(rt_chk(s858_newFn(up, RT_NULL, RT_NULL), "analyze"), 0);
  (void)({ wv t1712 = rt_def(up->v844_tops, "tops"); wv t1713 = v984_top; std_push(t1712, t1713); });
  wv v985_slot = rt_arr(0, NULL, 1);
  wv v986_i = rt_i(0);
  while (rt_cond(({ wv t1714 = v986_i; wv t1715 = std_length(rt_get(&F_chars, rt_idx(v983_m, up->v827_modules))); rt_le(t1714, t1715); }))) {
    (void)std_push(v985_slot, RT_NULL);
    v986_i = rt_add(v986_i, rt_i(1));
  }
  (void)({ wv t1716 = rt_def(up->v845_slots, "slots"); wv t1717 = v985_slot; std_push(t1716, t1717); });
  wv t1718 = rt_bind(v985_slot, 1);
  (void)rt_def(up->v848_curSlot, "curSlot");
  up->v848_curSlot = t1718;
  wv v987_scope = rt_bind(rt_chk(s854_newScope(up, RT_NULL), "analyze"), 0);
  wv v988_steals = rt_bind(rt_get(&F_steals, rt_idx(v983_m, up->v827_modules)), 0);
  wv v989_k = rt_i(0);
  while (rt_cond(({ wv t1719 = v989_k; wv t1720 = std_length(v988_steals); rt_lt(t1719, t1720); }))) {
    (void)({ wv t1721 = v987_scope; wv t1722 = rt_get(&F_name, rt_get(&F_node, rt_idx(v989_k, v988_steals))); rt_chk(s855_put(up, t1721, t1722, rt_s(&S393), RT_NULL), "analyze"); });
    v989_k = rt_add(v989_k, rt_i(1));
  }
  (void)({ wv t1723 = rt_rec(0, 2, K49, (wv[]){ rt_get(&F_stmts, rt_get(&F_program, rt_idx(v983_m, up->v827_modules))), RT_NULL }); wv t1724 = v987_scope; wv t1725 = v984_top; wv t1726 = v987_scope; rt_chk(s860_ablock(up, t1723, t1724, t1725, t1726), "analyze"); });
  RT_RET(RT_NULL);
}

static wv s867_enter(struct fr185 *up, wv v990_m) {
  rt_enter("enter");
  v990_m = rt_bind(v990_m, 0);
  (void)({ wv t1727 = rt_get(&F_file, rt_idx(v990_m, up->v827_modules)); wv t1728 = rt_get(&F_chars, rt_idx(v990_m, up->v827_modules)); rt_chk(w2_setSource(t1727, t1728), "enter"); });
  wv t1729 = rt_bind(v990_m, 1);
  (void)rt_def(up->v846_curMod, "curMod");
  up->v846_curMod = t1729;
  wv t1730 = rt_bind(rt_chk(s852_modId(up, v990_m), "enter"), 1);
  (void)rt_def(up->v847_mod, "mod");
  up->v847_mod = t1730;
  wv t1733 = rt_bind(({ wv t1731 = v990_m; wv t1732 = rt_def(up->v845_slots, "slots"); rt_idx(t1731, t1732); }), 1);
  (void)rt_def(up->v848_curSlot, "curSlot");
  up->v848_curSlot = t1733;
  wv t1736 = rt_bind(({ wv t1734 = v990_m; wv t1735 = rt_def(up->v844_tops, "tops"); rt_idx(t1734, t1735); }), 1);
  (void)rt_def(up->v851_cur, "cur");
  up->v851_cur = t1736;
  wv t1737 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v849_stolen, "stolen");
  up->v849_stolen = t1737;
  wv t1738 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v850_ownLineups, "ownLineups");
  up->v850_ownLineups = t1738;
  wv v991_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v990_m, up->v827_modules))), 0);
  wv v992_k = rt_i(0);
  while (rt_cond(({ wv t1739 = v992_k; wv t1740 = std_length(v991_list); rt_lt(t1739, t1740); }))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v992_k, v991_list)), rt_s(&S53)))) {
      (void)({ wv t1741 = rt_def(up->v850_ownLineups, "ownLineups"); wv t1742 = rt_idx(v992_k, v991_list); std_push(t1741, t1742); });
    }
    v992_k = rt_add(v992_k, rt_i(1));
  }
  wv v993_steals = rt_bind(rt_get(&F_steals, rt_idx(v990_m, up->v827_modules)), 0);
  v992_k = rt_i(0);
  while (rt_cond(({ wv t1743 = v992_k; wv t1744 = std_length(v993_steals); rt_lt(t1743, t1744); }))) {
    (void)({ wv t1747 = rt_def(up->v849_stolen, "stolen"); wv t1748 = ({ wv t1745 = rt_get(&F_name, rt_get(&F_node, rt_idx(v992_k, v993_steals))); wv t1746 = rt_get(&F_lib, rt_idx(v992_k, v993_steals)); rt_rec(0, 2, K50, (wv[]){ t1745, t1746 }); }); std_push(t1747, t1748); });
    v992_k = rt_add(v992_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s868_emit(struct fr185 *up, wv v994_line) {
  rt_enter("emit");
  v994_line = rt_bind(v994_line, 0);
  (void)({ wv t1751 = rt_def(up->v838_out, "out"); wv t1752 = ({ wv t1749 = rt_chk(w7_repeat(rt_s(&S478), rt_def(up->v839_indent, "indent")), "emit"); wv t1750 = v994_line; rt_add(t1749, t1750); }); std_push(t1751, t1752); });
  RT_RET(RT_NULL);
}

static wv s869_tmp(struct fr185 *up) {
  rt_enter("tmp");
  wv v995_name = rt_cat(2, (wv[]){ rt_s(&S115), rt_text(rt_def(up->v830_tmps, "tmps")) });
  up->v830_tmps = rt_add(rt_def(up->v830_tmps, "tmps"), rt_i(1));
  RT_RET(v995_name);
}

static wv s870_str(struct fr185 *up, wv v996_s) {
  rt_enter("str");
  v996_s = rt_bind(v996_s, 0);
  wv v997_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1753 = v997_k; wv t1754 = std_length(rt_def(up->v831_strings, "strings")); rt_lt(t1753, t1754); }), "&&") && rt_bool(({ wv t1757 = ({ wv t1755 = v997_k; wv t1756 = rt_def(up->v831_strings, "strings"); rt_idx(t1755, t1756); }); wv t1758 = v996_s; rt_ne(t1757, t1758); }), "&&")))) {
    v997_k = rt_add(v997_k, rt_i(1));
  }
  if (rt_cond(({ wv t1759 = v997_k; wv t1760 = std_length(rt_def(up->v831_strings, "strings")); rt_eq(t1759, t1760); }))) {
    (void)({ wv t1761 = rt_def(up->v831_strings, "strings"); wv t1762 = v996_s; std_push(t1761, t1762); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S572), rt_text(v997_k) }));
}

static wv s871_fieldRef(struct fr185 *up, wv v998_name) {
  rt_enter("fieldRef");
  v998_name = rt_bind(v998_name, 0);
  if (rt_cond(rt_not(({ wv t1763 = rt_def(up->v832_fields, "fields"); wv t1764 = v998_name; rt_chk(w1_contains(t1763, t1764), "fieldRef"); })))) {
    (void)({ wv t1765 = rt_def(up->v832_fields, "fields"); wv t1766 = v998_name; std_push(t1765, t1766); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S573), rt_text(v998_name) }));
}

static wv s872_lineupDef(struct fr185 *up, wv v999_s, wv v1000_m) {
  rt_enter("lineupDef");
  v999_s = rt_bind(v999_s, 0);
  v1000_m = rt_bind(v1000_m, 0);
  wv v1001_nm = rt_bind(rt_chk(s870_str(up, rt_get(&F_name, v999_s)), "lineupDef"), 0);
  wv v1002_members = rt_arr(0, NULL, 1);
  wv v1003_k = rt_i(0);
  while (rt_cond(({ wv t1767 = v1003_k; wv t1768 = std_length(rt_get(&F_members, v999_s)); rt_lt(t1767, t1768); }))) {
    (void)({ wv t1773 = v1002_members; wv t1774 = ({ wv t1771 = rt_text(v1001_nm); wv t1772 = rt_text(rt_chk(s870_str(up, rt_get(&F_name, ({ wv t1769 = v1003_k; wv t1770 = rt_get(&F_members, v999_s); rt_idx(t1769, t1770); }))), "lineupDef")); rt_cat(5, (wv[]){ rt_s(&S566), t1771, rt_s(&S390), t1772, rt_s(&S568) }); }); std_push(t1773, t1774); });
    v1003_k = rt_add(v1003_k, rt_i(1));
  }
  (void)({ wv t1778 = rt_def(up->v834_lineupDefs, "lineupDefs"); wv t1779 = ({ wv t1775 = rt_text(v1000_m); wv t1776 = rt_text(rt_get(&F_name, v999_s)); wv t1777 = rt_text(std_join(v1002_members, rt_s(&S390))); rt_cat(7, (wv[]){ rt_s(&S574), t1775, rt_s(&S12), t1776, rt_s(&S575), t1777, rt_s(&S576) }); }); std_push(t1778, t1779); });
  RT_RET(RT_NULL);
}

static wv s873_stolenFrom(struct fr185 *up, wv v1004_x) {
  rt_enter("stolenFrom");
  if (rt_cond(rt_ne(rt_get(&F_k, v1004_x), rt_s(&S264)))) {
    RT_RET(RT_NULL);
  }
  wv v1005_k = rt_i(0);
  while (rt_cond(({ wv t1780 = v1005_k; wv t1781 = std_length(rt_def(up->v849_stolen, "stolen")); rt_lt(t1780, t1781); }))) {
    if (rt_cond(({ wv t1784 = rt_get(&F_name, ({ wv t1782 = v1005_k; wv t1783 = rt_def(up->v849_stolen, "stolen"); rt_idx(t1782, t1783); })); wv t1785 = rt_get(&F_name, v1004_x); rt_eq(t1784, t1785); }))) {
      RT_RET(rt_get(&F_lib, ({ wv t1786 = v1005_k; wv t1787 = rt_def(up->v849_stolen, "stolen"); rt_idx(t1786, t1787); })));
    }
    v1005_k = rt_add(v1005_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s874_lineupIn(struct fr185 *up, wv v1006_list, wv v1007_name) {
  rt_enter("lineupIn");
  v1007_name = rt_bind(v1007_name, 0);
  wv v1008_k = rt_i(0);
  while (rt_cond(({ wv t1788 = v1008_k; wv t1789 = std_length(v1006_list); rt_lt(t1788, t1789); }))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_idx(v1008_k, v1006_list)), rt_s(&S53)), "&&") && rt_bool(({ wv t1790 = rt_get(&F_name, rt_idx(v1008_k, v1006_list)); wv t1791 = v1007_name; rt_eq(t1790, t1791); }), "&&")))) {
      RT_RET(rt_idx(v1008_k, v1006_list));
    }
    v1008_k = rt_add(v1008_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s875_lineupOf(struct fr185 *up, wv v1009_x) {
  rt_enter("lineupOf");
  if (rt_cond(rt_eq(rt_get(&F_k, v1009_x), rt_s(&S264)))) {
    wv v1013_s = rt_bind(({ wv t1792 = rt_def(up->v850_ownLineups, "ownLineups"); wv t1793 = rt_get(&F_name, v1009_x); rt_chk(s874_lineupIn(up, t1792, t1793), "lineupOf"); }), 0);
    RT_RET((rt_cond(rt_eq(v1013_s, RT_NULL)) ? RT_NULL : ({ wv t1796 = ({ wv t1794 = rt_text(rt_def(up->v847_mod, "mod")); wv t1795 = rt_text(rt_get(&F_name, v1009_x)); rt_cat(4, (wv[]){ rt_s(&S477), t1794, rt_s(&S12), t1795 }); }); wv t1797 = v1013_s; rt_rec(0, 2, K55, (wv[]){ t1796, t1797 }); })));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v1009_x), rt_s(&S198)))) {
    RT_RET(RT_NULL);
  }
  wv v1010_lib = rt_bind(rt_chk(s873_stolenFrom(up, rt_get(&F_rec, v1009_x)), "lineupOf"), 0);
  if (rt_cond(rt_eq(v1010_lib, RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v1011_xs = rt_bind(rt_get(&F_exports, rt_idx(v1010_lib, up->v827_modules)), 0);
  wv v1012_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1798 = v1012_k; wv t1799 = std_length(v1011_xs); rt_lt(t1798, t1799); }), "&&") && rt_bool(({ wv t1800 = rt_get(&F_name, rt_idx(v1012_k, v1011_xs)); wv t1801 = rt_get(&F_field, v1009_x); rt_ne(t1800, t1801); }), "&&")))) {
    v1012_k = rt_add(v1012_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1802 = v1012_k; wv t1803 = std_length(v1011_xs); rt_eq(t1802, t1803); }), "||") || rt_bool(rt_ne(rt_get(&F_kind, rt_idx(v1012_k, v1011_xs)), rt_s(&S53)), "||")))) {
    RT_RET(RT_NULL);
  }
  RT_RET(({ wv t1808 = ({ wv t1804 = rt_text(rt_chk(s852_modId(up, v1010_lib), "lineupOf")); wv t1805 = rt_text(rt_get(&F_field, v1009_x)); rt_cat(4, (wv[]){ rt_s(&S477), t1804, rt_s(&S12), t1805 }); }); wv t1809 = ({ wv t1806 = rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1010_lib, up->v827_modules))); wv t1807 = rt_get(&F_field, v1009_x); rt_chk(s874_lineupIn(up, t1806, t1807), "lineupOf"); }); rt_rec(0, 2, K55, (wv[]){ t1808, t1809 }); }));
}

static wv s876_place(struct fr185 *up, wv v1014_d) {
  rt_enter("place");
  v1014_d = rt_bind(v1014_d, 0);
  wv v1015_owner = rt_bind(rt_get(&F_fn, ({ wv t1810 = v1014_d; wv t1811 = rt_def(up->v842_decls, "decls"); rt_idx(t1810, t1811); })), 0);
  if (rt_cond(rt_get(&F_top, ({ wv t1812 = v1015_owner; wv t1813 = rt_def(up->v843_fns, "fns"); rt_idx(t1812, t1813); })))) {
    RT_RET(rt_get(&F_cname, ({ wv t1814 = v1014_d; wv t1815 = rt_def(up->v842_decls, "decls"); rt_idx(t1814, t1815); })));
  }
  if (rt_cond(({ wv t1816 = v1015_owner; wv t1817 = rt_def(up->v851_cur, "cur"); rt_eq(t1816, t1817); }))) {
    RT_RET((rt_cond(rt_get(&F_captured, ({ wv t1818 = v1014_d; wv t1819 = rt_def(up->v842_decls, "decls"); rt_idx(t1818, t1819); }))) ? rt_cat(2, (wv[]){ rt_s(&S577), rt_text(rt_get(&F_cname, ({ wv t1820 = v1014_d; wv t1821 = rt_def(up->v842_decls, "decls"); rt_idx(t1820, t1821); }))) }) : rt_get(&F_cname, ({ wv t1822 = v1014_d; wv t1823 = rt_def(up->v842_decls, "decls"); rt_idx(t1822, t1823); }))));
  }
  RT_RET(({ wv t1832 = rt_text(rt_chk(w7_repeat(rt_s(&S579), rt_sub(({ wv t1828 = rt_get(&F_depth, ({ wv t1824 = rt_def(up->v851_cur, "cur"); wv t1825 = rt_def(up->v843_fns, "fns"); rt_idx(t1824, t1825); })); wv t1829 = rt_get(&F_depth, ({ wv t1826 = v1015_owner; wv t1827 = rt_def(up->v843_fns, "fns"); rt_idx(t1826, t1827); })); rt_sub(t1828, t1829); }), rt_i(1))), "place")); wv t1833 = rt_text(rt_get(&F_cname, ({ wv t1830 = v1014_d; wv t1831 = rt_def(up->v842_decls, "decls"); rt_idx(t1830, t1831); }))); rt_cat(4, (wv[]){ rt_s(&S578), t1832, rt_s(&S580), t1833 }); }));
}

static wv s877_tdz(struct fr185 *up, wv v1016_d) {
  rt_enter("tdz");
  v1016_d = rt_bind(v1016_d, 0);
  RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_kind, ({ wv t1834 = v1016_d; wv t1835 = rt_def(up->v842_decls, "decls"); rt_idx(t1834, t1835); })), rt_s(&S35)), "&&") && rt_bool(({ wv t1838 = rt_get(&F_fn, ({ wv t1836 = v1016_d; wv t1837 = rt_def(up->v842_decls, "decls"); rt_idx(t1836, t1837); })); wv t1839 = rt_def(up->v851_cur, "cur"); rt_ne(t1838, t1839); }), "&&")));
}

static wv s878_read(struct fr185 *up, wv v1017_d) {
  rt_enter("read");
  v1017_d = rt_bind(v1017_d, 0);
  RT_RET((rt_cond(rt_chk(s877_tdz(up, v1017_d), "read")) ? ({ wv t1844 = ({ wv t1842 = rt_text(rt_chk(s876_place(up, v1017_d), "read")); wv t1843 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1840 = v1017_d; wv t1841 = rt_def(up->v842_decls, "decls"); rt_idx(t1840, t1841); }))), "read")); rt_cat(5, (wv[]){ rt_s(&S581), t1842, rt_s(&S390), t1843, rt_s(&S16) }); }); wv t1845 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t1844, t1845 }); }) : ({ wv t1846 = rt_chk(s876_place(up, v1017_d), "read"); wv t1847 = rt_def(w7_pureK, "pureK"); rt_rec(0, 2, K56, (wv[]){ t1846, t1847 }); })));
}

static wv s879_seq(struct fr185 *up, wv v1018_args) {
  rt_enter("seq");
  v1018_args = rt_bind(v1018_args, 0);
  wv v1019_effects = rt_i(0);
  wv v1020_anyPure = rt_b(0);
  wv v1021_k = rt_i(0);
  while (rt_cond(({ wv t1848 = v1021_k; wv t1849 = std_length(v1018_args); rt_lt(t1848, t1849); }))) {
    if (rt_cond(({ wv t1850 = rt_get(&F_k, rt_idx(v1021_k, v1018_args)); wv t1851 = rt_def(w7_effectK, "effectK"); rt_eq(t1850, t1851); }))) {
      v1019_effects = rt_add(v1019_effects, rt_i(1));
    }
    if (rt_cond(({ wv t1852 = rt_get(&F_k, rt_idx(v1021_k, v1018_args)); wv t1853 = rt_def(w7_pureK, "pureK"); rt_eq(t1852, t1853); }))) {
      v1020_anyPure = rt_b(1);
    }
    v1021_k = rt_add(v1021_k, rt_i(1));
  }
  wv v1022_cs = rt_arr(0, NULL, 1);
  if (rt_cond(rt_b(rt_bool(rt_eq(v1019_effects, rt_i(0)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1019_effects, rt_i(1)), "&&") && rt_bool(rt_not(v1020_anyPure), "&&")), "||")))) {
    v1021_k = rt_i(0);
    while (rt_cond(({ wv t1854 = v1021_k; wv t1855 = std_length(v1018_args); rt_lt(t1854, t1855); }))) {
      (void)({ wv t1856 = v1022_cs; wv t1857 = rt_get(&F_c, rt_idx(v1021_k, v1018_args)); std_push(t1856, t1857); });
      v1021_k = rt_add(v1021_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 2, K57, (wv[]){ rt_s(&S13), v1022_cs }));
  }
  wv v1023_pre = rt_arr(0, NULL, 1);
  v1021_k = rt_i(0);
  while (rt_cond(({ wv t1858 = v1021_k; wv t1859 = std_length(v1018_args); rt_lt(t1858, t1859); }))) {
    if (rt_cond(({ wv t1860 = rt_get(&F_k, rt_idx(v1021_k, v1018_args)); wv t1861 = rt_def(w7_constK, "constK"); rt_eq(t1860, t1861); }))) {
      (void)({ wv t1862 = v1022_cs; wv t1863 = rt_get(&F_c, rt_idx(v1021_k, v1018_args)); std_push(t1862, t1863); });
    } else {
      wv v1024_temp = rt_bind(rt_chk(s869_tmp(up), "seq"), 0);
      (void)({ wv t1866 = v1023_pre; wv t1867 = ({ wv t1864 = rt_text(v1024_temp); wv t1865 = rt_text(rt_get(&F_c, rt_idx(v1021_k, v1018_args))); rt_cat(5, (wv[]){ rt_s(&S582), t1864, rt_s(&S493), t1865, rt_s(&S14) }); }); std_push(t1866, t1867); });
      (void)std_push(v1022_cs, v1024_temp);
    }
    v1021_k = rt_add(v1021_k, rt_i(1));
  }
  RT_RET(({ wv t1868 = std_join(v1023_pre, rt_s(&S0)); wv t1869 = v1022_cs; rt_rec(0, 2, K57, (wv[]){ t1868, t1869 }); }));
}

static wv s880_wrap(struct fr185 *up, wv v1025_sq, wv v1026_inner) {
  rt_enter("wrap");
  v1025_sq = rt_bind(v1025_sq, 0);
  v1026_inner = rt_bind(v1026_inner, 0);
  RT_RET(({ wv t1872 = (rt_cond(rt_eq(rt_get(&F_pre, v1025_sq), rt_s(&S13))) ? v1026_inner : ({ wv t1870 = rt_text(rt_get(&F_pre, v1025_sq)); wv t1871 = rt_text(v1026_inner); rt_cat(5, (wv[]){ rt_s(&S583), t1870, rt_s(&S0), t1871, rt_s(&S584) }); })); wv t1873 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t1872, t1873 }); }));
}

static wv s881_stmts(struct fr185 *up, wv v1027_list) {
  rt_enter("stmts");
  v1027_list = rt_bind(v1027_list, 0);
  wv v1028_k = rt_i(0);
  while (rt_cond(({ wv t1874 = v1028_k; wv t1875 = std_length(v1027_list); rt_lt(t1874, t1875); }))) {
    (void)rt_chk(s885_stmt(up, rt_idx(v1028_k, v1027_list)), "stmts");
    v1028_k = rt_add(v1028_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s882_resets(struct fr185 *up, wv v1029_b) {
  rt_enter("resets");
  wv v1030_k = rt_i(0);
  while (rt_cond(({ wv t1876 = v1030_k; wv t1877 = std_length(rt_get(&F_stmts, v1029_b)); rt_lt(t1876, t1877); }))) {
    wv v1031_s = ({ wv t1878 = v1030_k; wv t1879 = rt_get(&F_stmts, v1029_b); rt_idx(t1878, t1879); });
    if (rt_cond(rt_eq(rt_get(&F_k, v1031_s), rt_s(&S35)))) {
      wv v1032_d = rt_bind(({ wv t1880 = rt_get(&F_namePos, v1031_s); wv t1881 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t1880, t1881); }), 0);
      if (rt_cond(rt_get(&F_captured, ({ wv t1882 = v1032_d; wv t1883 = rt_def(up->v842_decls, "decls"); rt_idx(t1882, t1883); })))) {
        (void)rt_chk(s868_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s876_place(up, v1032_d), "resets")), rt_s(&S585) })), "resets");
      }
    }
    v1030_k = rt_add(v1030_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s883_blockBody(struct fr185 *up, wv v1033_b) {
  rt_enter("blockBody");
  v1033_b = rt_bind(v1033_b, 0);
  (void)rt_chk(s882_resets(up, v1033_b), "blockBody");
  (void)rt_chk(s881_stmts(up, rt_get(&F_stmts, v1033_b)), "blockBody");
  if (rt_cond(rt_ne(rt_get(&F_value, v1033_b), RT_NULL))) {
    (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S586), rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_value, v1033_b)), "blockBody"))), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s884_valueOf(struct fr185 *up, wv v1034_b) {
  rt_enter("valueOf");
  v1034_b = rt_bind(v1034_b, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_value, v1034_b), RT_NULL)) ? rt_s(&S587) : rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_value, v1034_b)), "valueOf"))));
}

static wv s885_stmt(struct fr185 *up, wv v1035_s) {
  rt_enter("stmt");
  if (rt_cond(rt_ne(rt_get(&F_handler, v1035_s), RT_NULL))) {
    (void)rt_chk(s894_whineStmt(up, v1035_s), "stmt");
    RT_RET(RT_NULL);
  }
  wv v1036_kind = rt_bind(rt_get(&F_k, v1035_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1036_kind, rt_s(&S207)), "||") || rt_bool(rt_eq(v1036_kind, rt_s(&S53)), "||")), "||") || rt_bool(rt_eq(v1036_kind, rt_s(&S54)), "||")), "||") || rt_bool(rt_eq(v1036_kind, rt_s(&S55)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S33)))) {
    (void)rt_chk(s888_schemeDef(up, v1035_s), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S34)))) {
    if (rt_cond(rt_def(up->v841_iife, "iife"))) {
      (void)rt_chk(w2_fail(rt_get(&F_pos, v1035_s), rt_s(&S480)), "stmt");
    }
    (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S588), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1035_s), RT_NULL)) ? rt_s(&S587) : rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_e, v1035_s)), "stmt")))), rt_s(&S483) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S44)))) {
    (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S589), rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_e, v1035_s)), "stmt"))), rt_s(&S483) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v1036_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v1036_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_def(up->v840_loops, "loops"), rt_i(0)))) {
      (void)({ wv t1884 = rt_get(&F_pos, v1035_s); wv t1885 = rt_cat(3, (wv[]){ rt_s(&S111), rt_text(v1036_kind), rt_s(&S484) }); rt_chk(w2_fail(t1884, t1885), "stmt"); });
    }
    (void)rt_chk(s868_emit(up, (rt_cond(rt_eq(v1036_kind, rt_s(&S42))) ? rt_s(&S485) : rt_s(&S486))), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S41)))) {
    (void)rt_chk(s868_emit(up, ({ wv t1886 = rt_text((rt_cond(rt_get(&F_neg, v1035_s)) ? rt_s(&S87) : rt_s(&S13))); wv t1887 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_cond, v1035_s)), "stmt"))); rt_cat(5, (wv[]){ rt_s(&S487), t1886, rt_s(&S590), t1887, rt_s(&S489) }); })), "stmt");
    up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
    up->v840_loops = rt_add(rt_def(up->v840_loops, "loops"), rt_i(1));
    (void)rt_chk(s883_blockBody(up, rt_get(&F_body, v1035_s)), "stmt");
    up->v840_loops = rt_sub(rt_def(up->v840_loops, "loops"), rt_i(1));
    up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
    (void)rt_chk(s868_emit(up, rt_s(&S15)), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S211)))) {
    (void)rt_chk(s886_signal(up, rt_cat(3, (wv[]){ rt_s(&S591), rt_text(rt_get(&F_code, v1035_s)), rt_s(&S16) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S185)))) {
    (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S592), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1035_s), RT_NULL)) ? rt_s(&S593) : rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_e, v1035_s)), "stmt")))), rt_s(&S483) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S184)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1035_s)), rt_s(&S193)))) {
      (void)rt_chk(s895_chainStmt(up, rt_get(&F_e, v1035_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1035_s)), rt_s(&S51)))) {
      (void)rt_chk(s906_suspectStmt(up, rt_get(&F_e, v1035_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1035_s)), rt_s(&S192)))) {
      (void)rt_chk(s886_signal(up, rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_arg, rt_get(&F_e, v1035_s))), "stmt"))), "stmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S586), rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_e, v1035_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S35)))) {
    (void)({ wv t1892 = ({ wv t1888 = rt_get(&F_namePos, v1035_s); wv t1889 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t1888, t1889); }); wv t1893 = ({ wv t1890 = rt_get(&F_e, v1035_s); wv t1891 = rt_chk(w7_modeOf(v1035_s), "stmt"); rt_chk(s889_bindRhs(up, t1890, t1891), "stmt"); }); rt_chk(s887_claimDecl(up, t1892, t1893), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1036_kind, rt_s(&S38)))) {
    (void)rt_chk(s890_cheatStmt(up, v1035_s), "stmt");
    RT_RET(RT_NULL);
  }
  rt_tantrum(rt_cat(2, (wv[]){ rt_s(&S494), rt_text(v1036_kind) }));
  RT_RET(RT_NULL);
}

static wv s886_signal(struct fr185 *up, wv v1037_code) {
  rt_enter("signal");
  v1037_code = rt_bind(v1037_code, 0);
  (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S594), rt_text(v1037_code), rt_s(&S483) })), "signal");
  (void)rt_chk(s868_emit(up, rt_s(&S595)), "signal");
  RT_RET(RT_NULL);
}

static wv s887_claimDecl(struct fr185 *up, wv v1038_d, wv v1039_value) {
  rt_enter("claimDecl");
  v1038_d = rt_bind(v1038_d, 0);
  v1039_value = rt_bind(v1039_value, 0);
  wv v1040_owner = rt_bind(rt_get(&F_fn, ({ wv t1894 = v1038_d; wv t1895 = rt_def(up->v842_decls, "decls"); rt_idx(t1894, t1895); })), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1896 = v1040_owner; wv t1897 = rt_def(up->v843_fns, "fns"); rt_idx(t1896, t1897); }))), "&&") && rt_bool(({ wv t1898 = v1040_owner; wv t1899 = rt_def(up->v851_cur, "cur"); rt_eq(t1898, t1899); }), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_captured, ({ wv t1900 = v1038_d; wv t1901 = rt_def(up->v842_decls, "decls"); rt_idx(t1900, t1901); }))), "&&")))) {
    (void)rt_chk(s868_emit(up, ({ wv t1904 = rt_text(rt_get(&F_cname, ({ wv t1902 = v1038_d; wv t1903 = rt_def(up->v842_decls, "decls"); rt_idx(t1902, t1903); }))); wv t1905 = rt_text(v1039_value); rt_cat(5, (wv[]){ rt_s(&S582), t1904, rt_s(&S493), t1905, rt_s(&S14) }); })), "claimDecl");
  } else {
    (void)rt_chk(s868_emit(up, ({ wv t1906 = rt_text(rt_chk(s876_place(up, v1038_d), "claimDecl")); wv t1907 = rt_text(v1039_value); rt_cat(4, (wv[]){ t1906, rt_s(&S493), t1907, rt_s(&S14) }); })), "claimDecl");
  }
  RT_RET(RT_NULL);
}

static wv s888_schemeDef(struct fr185 *up, wv v1041_s) {
  rt_enter("schemeDef");
  v1041_s = rt_bind(v1041_s, 0);
  wv v1042_d = rt_bind(({ wv t1908 = rt_get(&F_namePos, v1041_s); wv t1909 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t1908, t1909); }), 0);
  wv v1043_f = rt_bind(rt_get(&F_schemeFn, ({ wv t1910 = v1042_d; wv t1911 = rt_def(up->v842_decls, "decls"); rt_idx(t1910, t1911); })), 0);
  wv v1044_pf = rt_bind(rt_get(&F_parent, ({ wv t1912 = v1043_f; wv t1913 = rt_def(up->v843_fns, "fns"); rt_idx(t1912, t1913); })), 0);
  wv v1045_hasUp = rt_not(rt_get(&F_top, ({ wv t1914 = v1044_pf; wv t1915 = rt_def(up->v843_fns, "fns"); rt_idx(t1914, t1915); })));
  wv v1046_params = rt_arr(0, NULL, 1);
  if (rt_cond(v1045_hasUp)) {
    (void)({ wv t1916 = v1046_params; wv t1917 = rt_cat(3, (wv[]){ rt_s(&S596), rt_text(v1044_pf), rt_s(&S597) }); std_push(t1916, t1917); });
  }
  wv v1047_k = rt_i(0);
  while (rt_cond(({ wv t1918 = v1047_k; wv t1919 = std_length(rt_get(&F_params, v1041_s)); rt_lt(t1918, t1919); }))) {
    wv v1055_pd = rt_bind(({ wv t1922 = rt_get(&F_pos, ({ wv t1920 = v1047_k; wv t1921 = rt_get(&F_params, v1041_s); rt_idx(t1920, t1921); })); wv t1923 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t1922, t1923); }), 0);
    (void)({ wv t1930 = v1046_params; wv t1931 = ({ wv t1928 = rt_text((rt_cond(rt_get(&F_captured, ({ wv t1924 = v1055_pd; wv t1925 = rt_def(up->v842_decls, "decls"); rt_idx(t1924, t1925); }))) ? rt_s(&S598) : rt_s(&S13))); wv t1929 = rt_text(rt_get(&F_cname, ({ wv t1926 = v1055_pd; wv t1927 = rt_def(up->v842_decls, "decls"); rt_idx(t1926, t1927); }))); rt_cat(3, (wv[]){ rt_s(&S582), t1928, t1929 }); }); std_push(t1930, t1931); });
    v1047_k = rt_add(v1047_k, rt_i(1));
  }
  wv v1048_proto = ({ wv t1934 = rt_text(rt_get(&F_cname, ({ wv t1932 = v1043_f; wv t1933 = rt_def(up->v843_fns, "fns"); rt_idx(t1932, t1933); }))); wv t1935 = rt_text((rt_cond(rt_eq(std_length(v1046_params), rt_i(0))) ? rt_s(&S600) : std_join(v1046_params, rt_s(&S390)))); rt_cat(5, (wv[]){ rt_s(&S599), t1934, rt_s(&S90), t1935, rt_s(&S16) }); });
  wv v1049_entry = ({ wv t1936 = v1048_proto; wv t1937 = rt_arr(0, NULL, 0); rt_rec(1, 2, K58, (wv[]){ t1936, t1937 }); });
  (void)({ wv t1938 = rt_def(up->v837_functions, "functions"); wv t1939 = v1049_entry; std_push(t1938, t1939); });
  if (rt_cond(rt_chk(s853_hasFrame(up, v1043_f), "schemeDef"))) {
    (void)({ wv t1940 = rt_def(up->v836_frames, "frames"); wv t1941 = v1043_f; std_push(t1940, t1941); });
  }
  wv v1050_start = rt_bind(std_length(rt_def(up->v838_out, "out")), 0);
  wv v1051_savedIndent = rt_bind(rt_def(up->v839_indent, "indent"), 0);
  wv v1052_savedLoops = rt_bind(rt_def(up->v840_loops, "loops"), 0);
  wv v1053_savedIife = rt_bind(rt_def(up->v841_iife, "iife"), 0);
  wv v1054_savedCur = rt_bind(rt_def(up->v851_cur, "cur"), 0);
  wv t1942 = rt_i(1);
  (void)rt_def(up->v839_indent, "indent");
  up->v839_indent = t1942;
  wv t1943 = rt_i(0);
  (void)rt_def(up->v840_loops, "loops");
  up->v840_loops = t1943;
  wv t1944 = rt_b(0);
  (void)rt_def(up->v841_iife, "iife");
  up->v841_iife = t1944;
  wv t1945 = rt_bind(v1043_f, 1);
  (void)rt_def(up->v851_cur, "cur");
  up->v851_cur = t1945;
  (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S601), rt_text(rt_chk(w1_quote(rt_get(&F_name, v1041_s)), "schemeDef")), rt_s(&S483) })), "schemeDef");
  if (rt_cond(rt_chk(s853_hasFrame(up, v1043_f), "schemeDef"))) {
    (void)rt_chk(s868_emit(up, ({ wv t1946 = rt_text(v1043_f); wv t1947 = rt_text((rt_cond(v1045_hasUp) ? rt_s(&S603) : rt_s(&S4))); rt_cat(5, (wv[]){ rt_s(&S596), t1946, rt_s(&S602), t1947, rt_s(&S576) }); })), "schemeDef");
  }
  v1047_k = rt_i(0);
  while (rt_cond(({ wv t1948 = v1047_k; wv t1949 = std_length(rt_get(&F_params, v1041_s)); rt_lt(t1948, t1949); }))) {
    wv v1056_p = rt_bind(({ wv t1950 = v1047_k; wv t1951 = rt_get(&F_params, v1041_s); rt_idx(t1950, t1951); }), 0);
    wv v1057_pd = rt_bind(({ wv t1952 = rt_get(&F_pos, v1056_p); wv t1953 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t1952, t1953); }), 0);
    wv v1058_cn = rt_bind(rt_get(&F_cname, ({ wv t1954 = v1057_pd; wv t1955 = rt_def(up->v842_decls, "decls"); rt_idx(t1954, t1955); })), 0);
    wv v1059_arg = rt_bind((rt_cond(rt_get(&F_captured, ({ wv t1956 = v1057_pd; wv t1957 = rt_def(up->v842_decls, "decls"); rt_idx(t1956, t1957); }))) ? rt_cat(2, (wv[]){ rt_s(&S598), rt_text(v1058_cn) }) : v1058_cn), 0);
    wv v1060_v = rt_bind((rt_cond(rt_get(&F_isFickle, v1056_p)) ? v1059_arg : ({ wv t1958 = rt_text(v1059_arg); wv t1959 = rt_text((rt_cond(rt_get(&F_isShifty, v1056_p)) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S604), t1958, rt_s(&S390), t1959, rt_s(&S16) }); })), 0);
    if (rt_cond(rt_get(&F_captured, ({ wv t1960 = v1057_pd; wv t1961 = rt_def(up->v842_decls, "decls"); rt_idx(t1960, t1961); })))) {
      (void)rt_chk(s868_emit(up, ({ wv t1962 = rt_text(v1058_cn); wv t1963 = rt_text(v1060_v); rt_cat(5, (wv[]){ rt_s(&S577), t1962, rt_s(&S493), t1963, rt_s(&S14) }); })), "schemeDef");
    } else if (rt_cond(rt_not(rt_get(&F_isFickle, v1056_p)))) {
      (void)rt_chk(s868_emit(up, ({ wv t1964 = rt_text(v1058_cn); wv t1965 = rt_text(v1060_v); rt_cat(4, (wv[]){ t1964, rt_s(&S493), t1965, rt_s(&S14) }); })), "schemeDef");
    }
    v1047_k = rt_add(v1047_k, rt_i(1));
  }
  (void)rt_chk(s881_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1041_s))), "schemeDef");
  (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S588), rt_text(rt_chk(s884_valueOf(up, rt_get(&F_body, v1041_s)), "schemeDef")), rt_s(&S483) })), "schemeDef");
  ({ wv t1969 = ({ wv t1966 = rt_def(up->v838_out, "out"); wv t1967 = v1050_start; wv t1968 = std_length(rt_def(up->v838_out, "out")); std_slice(t1966, t1967, t1968); }); wv t1970 = v1049_entry; rt_set_mem(t1969, &F_lines, t1970); });
  while (rt_cond(({ wv t1971 = std_length(rt_def(up->v838_out, "out")); wv t1972 = v1050_start; rt_gt(t1971, t1972); }))) {
    (void)std_pop(rt_def(up->v838_out, "out"));
  }
  wv t1973 = rt_bind(v1051_savedIndent, 1);
  (void)rt_def(up->v839_indent, "indent");
  up->v839_indent = t1973;
  wv t1974 = rt_bind(v1052_savedLoops, 1);
  (void)rt_def(up->v840_loops, "loops");
  up->v840_loops = t1974;
  wv t1975 = rt_bind(v1053_savedIife, 1);
  (void)rt_def(up->v841_iife, "iife");
  up->v841_iife = t1975;
  wv t1976 = rt_bind(v1054_savedCur, 1);
  (void)rt_def(up->v851_cur, "cur");
  up->v851_cur = t1976;
  RT_RET(RT_NULL);
}

static wv s889_bindRhs(struct fr185 *up, wv v1061_e, wv v1062_mode) {
  rt_enter("bindRhs");
  v1061_e = rt_bind(v1061_e, 0);
  v1062_mode = rt_bind(v1062_mode, 0);
  wv v1063_mutable = rt_eq(v1062_mode, rt_s(&S36));
  if (rt_cond(rt_eq(rt_get(&F_k, v1061_e), rt_s(&S195)))) {
    RT_RET(rt_get(&F_c, rt_chk(s892_array(up, v1061_e, v1063_mutable), "bindRhs")));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v1061_e), rt_s(&S196)))) {
    RT_RET(rt_get(&F_c, rt_chk(s893_record(up, v1061_e, v1063_mutable), "bindRhs")));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1977 = rt_def(w7_scalarKinds, "scalarKinds"); wv t1978 = rt_get(&F_k, v1061_e); rt_chk(w1_contains(t1977, t1978), "bindRhs"); }), "||") || rt_bool(rt_eq(v1062_mode, rt_s(&S37)), "||")))) {
    RT_RET(rt_get(&F_c, rt_chk(s909_expr(up, v1061_e), "bindRhs")));
  }
  RT_RET(({ wv t1979 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, v1061_e), "bindRhs"))); wv t1980 = rt_text((rt_cond(v1063_mutable) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S604), t1979, rt_s(&S390), t1980, rt_s(&S16) }); }));
}

static wv s890_cheatStmt(struct fr185 *up, wv v1064_s) {
  rt_enter("cheatStmt");
  v1064_s = rt_bind(v1064_s, 0);
  wv v1065_t = rt_bind(rt_get(&F_target, v1064_s), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v1065_t), rt_s(&S264)))) {
    wv v1072_d = rt_bind(({ wv t1981 = rt_get(&F_pos, v1065_t); wv t1982 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t1981, t1982); }), 0);
    wv v1073_at = rt_bind(rt_chk(s876_place(up, v1072_d), "cheatStmt"), 0);
    if (rt_cond(rt_eq(rt_get(&F_op, v1064_s), RT_NULL))) {
      wv v1076_v = rt_bind(({ wv t1985 = rt_get(&F_e, v1064_s); wv t1986 = (rt_cond(rt_get(&F_isFickle, ({ wv t1983 = v1072_d; wv t1984 = rt_def(up->v842_decls, "decls"); rt_idx(t1983, t1984); }))) ? rt_s(&S37) : rt_s(&S36)); rt_chk(s889_bindRhs(up, t1985, t1986), "cheatStmt"); }), 0);
      if (rt_cond(rt_not(rt_chk(s877_tdz(up, v1072_d), "cheatStmt")))) {
        (void)rt_chk(s868_emit(up, ({ wv t1987 = rt_text(v1073_at); wv t1988 = rt_text(v1076_v); rt_cat(4, (wv[]){ t1987, rt_s(&S493), t1988, rt_s(&S14) }); })), "cheatStmt");
        RT_RET(RT_NULL);
      }
      wv v1077_temp = rt_bind(rt_chk(s869_tmp(up), "cheatStmt"), 0);
      (void)rt_chk(s868_emit(up, ({ wv t1989 = rt_text(v1077_temp); wv t1990 = rt_text(v1076_v); rt_cat(5, (wv[]){ rt_s(&S582), t1989, rt_s(&S493), t1990, rt_s(&S14) }); })), "cheatStmt");
      (void)rt_chk(s868_emit(up, ({ wv t1993 = rt_text(v1073_at); wv t1994 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1991 = v1072_d; wv t1992 = rt_def(up->v842_decls, "decls"); rt_idx(t1991, t1992); }))), "cheatStmt")); rt_cat(5, (wv[]){ rt_s(&S605), t1993, rt_s(&S390), t1994, rt_s(&S483) }); })), "cheatStmt");
      (void)rt_chk(s868_emit(up, ({ wv t1995 = rt_text(v1073_at); wv t1996 = rt_text(v1077_temp); rt_cat(4, (wv[]){ t1995, rt_s(&S493), t1996, rt_s(&S14) }); })), "cheatStmt");
      RT_RET(RT_NULL);
    }
    wv v1074_v = rt_bind(rt_chk(s909_expr(up, rt_get(&F_e, v1064_s)), "cheatStmt"), 1);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1064_s)), rt_s(&S106)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1064_s)), rt_s(&S264)), "&&")))) {
      wv v1078_temp = rt_bind(rt_chk(s869_tmp(up), "cheatStmt"), 0);
      (void)rt_chk(s868_emit(up, ({ wv t1997 = rt_text(v1078_temp); wv t1998 = rt_text(rt_get(&F_c, v1074_v)); rt_cat(5, (wv[]){ rt_s(&S582), t1997, rt_s(&S493), t1998, rt_s(&S14) }); })), "cheatStmt");
      v1074_v = ({ wv t1999 = v1078_temp; wv t2000 = rt_def(w7_constK, "constK"); rt_rec(1, 2, K56, (wv[]){ t1999, t2000 }); });
    }
    wv v1075_sq = rt_bind(rt_chk(s879_seq(up, ({ wv t2001 = rt_chk(s878_read(up, v1072_d), "cheatStmt"); wv t2002 = v1074_v; rt_arr(2, (wv[]){ t2001, t2002 }, 0); })), "cheatStmt"), 0);
    (void)rt_chk(s868_emit(up, ({ wv t2008 = rt_text(v1073_at); wv t2009 = rt_text(rt_get(&F_c, ({ wv t2006 = v1075_sq; wv t2007 = ({ wv t2003 = rt_text(rt_chk(w7_binFn(rt_get(&F_op, v1064_s)), "cheatStmt")); wv t2004 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1075_sq))); wv t2005 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1075_sq))); rt_cat(7, (wv[]){ rt_s(&S606), t2003, rt_s(&S90), t2004, rt_s(&S390), t2005, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2006, t2007), "cheatStmt"); }))); rt_cat(4, (wv[]){ t2008, rt_s(&S493), t2009, rt_s(&S14) }); })), "cheatStmt");
    RT_RET(RT_NULL);
  }
  wv v1066_key = RT_NULL;
  if (rt_cond(rt_eq(rt_get(&F_k, v1065_t), rt_s(&S198)))) {
    v1066_key = ({ wv t2010 = rt_chk(s871_fieldRef(up, rt_get(&F_field, v1065_t)), "cheatStmt"); wv t2011 = rt_def(w7_constK, "constK"); rt_rec(1, 2, K56, (wv[]){ t2010, t2011 }); });
  }
  wv v1067_v = rt_bind(rt_chk(s909_expr(up, rt_get(&F_e, v1064_s)), "cheatStmt"), 0);
  wv v1068_i = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1065_t), rt_s(&S198))) ? v1066_key : rt_chk(s909_expr(up, rt_get(&F_index, v1065_t)), "cheatStmt")), 0);
  wv v1069_box = rt_bind(rt_chk(s909_expr(up, (rt_cond(rt_eq(rt_get(&F_k, v1065_t), rt_s(&S198))) ? rt_get(&F_rec, v1065_t) : rt_get(&F_arr, v1065_t))), "cheatStmt"), 0);
  wv v1070_sq = rt_bind(rt_chk(s879_seq(up, rt_arr(3, (wv[]){ v1067_v, v1068_i, v1069_box }, 0)), "cheatStmt"), 0);
  wv v1071_which = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1065_t), rt_s(&S198))) ? rt_s(&S198) : rt_s(&S197)), 0);
  if (rt_cond(rt_eq(rt_get(&F_op, v1064_s), RT_NULL))) {
    (void)rt_chk(s868_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2014 = v1070_sq; wv t2015 = ({ wv t2012 = rt_text(v1071_which); wv t2013 = rt_text(std_join(rt_get(&F_cs, v1070_sq), rt_s(&S390))); rt_cat(5, (wv[]){ rt_s(&S607), t2012, rt_s(&S90), t2013, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2014, t2015), "cheatStmt"); }))), rt_s(&S14) })), "cheatStmt");
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s868_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2019 = v1070_sq; wv t2020 = ({ wv t2016 = rt_text(v1071_which); wv t2017 = rt_text(std_join(rt_get(&F_cs, v1070_sq), rt_s(&S390))); wv t2018 = rt_text(rt_chk(w7_binFn(rt_get(&F_op, v1064_s)), "cheatStmt")); rt_cat(7, (wv[]){ rt_s(&S608), t2016, rt_s(&S90), t2017, rt_s(&S609), t2018, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2019, t2020), "cheatStmt"); }))), rt_s(&S14) })), "cheatStmt");
  RT_RET(RT_NULL);
}

static wv s891_exprs(struct fr185 *up, wv v1079_list) {
  rt_enter("exprs");
  v1079_list = rt_bind(v1079_list, 0);
  wv v1080_xs = rt_arr(0, NULL, 1);
  wv v1081_k = rt_i(0);
  while (rt_cond(({ wv t2021 = v1081_k; wv t2022 = std_length(v1079_list); rt_lt(t2021, t2022); }))) {
    (void)({ wv t2023 = v1080_xs; wv t2024 = rt_chk(s909_expr(up, rt_idx(v1081_k, v1079_list)), "exprs"); std_push(t2023, t2024); });
    v1081_k = rt_add(v1081_k, rt_i(1));
  }
  RT_RET(v1080_xs);
}

static wv s892_array(struct fr185 *up, wv v1082_e, wv v1083_mutable) {
  rt_enter("array");
  v1082_e = rt_bind(v1082_e, 0);
  v1083_mutable = rt_bind(v1083_mutable, 0);
  wv v1084_sq = rt_bind(rt_chk(s879_seq(up, rt_chk(s891_exprs(up, rt_get(&F_items, v1082_e)), "array")), "array"), 0);
  RT_RET(({ wv t2028 = v1084_sq; wv t2029 = ({ wv t2025 = rt_text(std_length(rt_get(&F_cs, v1084_sq))); wv t2026 = rt_text(rt_chk(w7_values(rt_get(&F_cs, v1084_sq)), "array")); wv t2027 = rt_text((rt_cond(v1083_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S610), t2025, rt_s(&S390), t2026, rt_s(&S390), t2027, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2028, t2029), "array"); }));
}

static wv s893_record(struct fr185 *up, wv v1085_e, wv v1086_mutable) {
  rt_enter("record");
  v1085_e = rt_bind(v1085_e, 0);
  v1086_mutable = rt_bind(v1086_mutable, 0);
  wv v1087_shape = rt_s(&S569);
  if (rt_cond(rt_gt(std_length(rt_get(&F_fields, v1085_e)), rt_i(0)))) {
    wv v1091_names = rt_arr(0, NULL, 1);
    wv v1092_k = rt_i(0);
    while (rt_cond(({ wv t2030 = v1092_k; wv t2031 = std_length(rt_get(&F_fields, v1085_e)); rt_lt(t2030, t2031); }))) {
      (void)({ wv t2034 = v1091_names; wv t2035 = rt_get(&F_name, ({ wv t2032 = v1092_k; wv t2033 = rt_get(&F_fields, v1085_e); rt_idx(t2032, t2033); })); std_push(t2034, t2035); });
      v1092_k = rt_add(v1092_k, rt_i(1));
    }
    wv v1093_key = rt_bind(std_join(v1091_names, rt_s(&S89)), 0);
    v1092_k = rt_i(0);
    while (rt_cond(({ wv t2036 = v1092_k; wv t2037 = std_length(v1091_names); rt_lt(t2036, t2037); }))) {
      (void)rt_chk(s871_fieldRef(up, rt_idx(v1092_k, v1091_names)), "record");
      v1092_k = rt_add(v1092_k, rt_i(1));
    }
    v1092_k = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t2038 = v1092_k; wv t2039 = std_length(rt_def(up->v833_shapes, "shapes")); rt_lt(t2038, t2039); }), "&&") && rt_bool(({ wv t2042 = ({ wv t2040 = v1092_k; wv t2041 = rt_def(up->v833_shapes, "shapes"); rt_idx(t2040, t2041); }); wv t2043 = v1093_key; rt_ne(t2042, t2043); }), "&&")))) {
      v1092_k = rt_add(v1092_k, rt_i(1));
    }
    if (rt_cond(({ wv t2044 = v1092_k; wv t2045 = std_length(rt_def(up->v833_shapes, "shapes")); rt_eq(t2044, t2045); }))) {
      (void)({ wv t2046 = rt_def(up->v833_shapes, "shapes"); wv t2047 = v1093_key; std_push(t2046, t2047); });
    }
    v1087_shape = rt_cat(2, (wv[]){ rt_s(&S611), rt_text(v1092_k) });
  }
  wv v1088_vals = rt_arr(0, NULL, 1);
  wv v1089_k = rt_i(0);
  while (rt_cond(({ wv t2048 = v1089_k; wv t2049 = std_length(rt_get(&F_fields, v1085_e)); rt_lt(t2048, t2049); }))) {
    (void)({ wv t2052 = v1088_vals; wv t2053 = rt_chk(s909_expr(up, rt_get(&F_e, ({ wv t2050 = v1089_k; wv t2051 = rt_get(&F_fields, v1085_e); rt_idx(t2050, t2051); }))), "record"); std_push(t2052, t2053); });
    v1089_k = rt_add(v1089_k, rt_i(1));
  }
  wv v1090_sq = rt_bind(rt_chk(s879_seq(up, v1088_vals), "record"), 0);
  RT_RET(({ wv t2058 = v1090_sq; wv t2059 = ({ wv t2054 = rt_text((rt_cond(v1086_mutable) ? rt_i(1) : rt_i(0))); wv t2055 = rt_text(std_length(rt_get(&F_cs, v1090_sq))); wv t2056 = rt_text(v1087_shape); wv t2057 = rt_text(rt_chk(w7_values(rt_get(&F_cs, v1090_sq)), "record")); rt_cat(9, (wv[]){ rt_s(&S612), t2054, rt_s(&S390), t2055, rt_s(&S390), t2056, rt_s(&S390), t2057, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2058, t2059), "record"); }));
}

static wv s894_whineStmt(struct fr185 *up, wv v1094_s) {
  rt_enter("whineStmt");
  v1094_s = rt_bind(v1094_s, 0);
  wv v1095_call = rt_bind(rt_get(&F_e, v1094_s), 0);
  wv v1096_args = rt_arr(0, NULL, 1);
  wv v1097_k = rt_i(0);
  while (rt_cond(({ wv t2060 = v1097_k; wv t2061 = std_length(rt_get(&F_args, v1095_call)); rt_lt(t2060, t2061); }))) {
    wv v1101_a = rt_bind(({ wv t2062 = v1097_k; wv t2063 = rt_get(&F_args, v1095_call); rt_idx(t2062, t2063); }), 0);
    if (rt_cond(({ wv t2064 = rt_def(w7_plainKinds, "plainKinds"); wv t2065 = rt_get(&F_k, v1101_a); rt_chk(w1_contains(t2064, t2065), "whineStmt"); }))) {
      (void)({ wv t2066 = v1096_args; wv t2067 = rt_get(&F_c, rt_chk(s909_expr(up, v1101_a), "whineStmt")); std_push(t2066, t2067); });
    } else {
      wv v1102_temp = rt_bind(rt_chk(s869_tmp(up), "whineStmt"), 0);
      (void)rt_chk(s868_emit(up, ({ wv t2068 = rt_text(v1102_temp); wv t2069 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, v1101_a), "whineStmt"))); rt_cat(5, (wv[]){ rt_s(&S582), t2068, rt_s(&S493), t2069, rt_s(&S14) }); })), "whineStmt");
      (void)std_push(v1096_args, v1102_temp);
    }
    v1097_k = rt_add(v1097_k, rt_i(1));
  }
  wv v1098_r = rt_bind(rt_chk(s869_tmp(up), "whineStmt"), 0);
  (void)rt_chk(s868_emit(up, ({ wv t2072 = rt_text(v1098_r); wv t2073 = rt_text(({ wv t2070 = rt_get(&F_callee, v1095_call); wv t2071 = v1096_args; rt_chk(s908_callC(up, t2070, t2071, rt_b(0)), "whineStmt"); })); rt_cat(5, (wv[]){ rt_s(&S582), t2072, rt_s(&S493), t2073, rt_s(&S14) }); })), "whineStmt");
  (void)rt_chk(s868_emit(up, rt_s(&S613)), "whineStmt");
  up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
  wv v1099_h = rt_bind(rt_get(&F_handler, v1094_s), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v1099_h), RT_NULL))) {
    (void)rt_chk(s887_claimDecl(up, ({ wv t2074 = rt_get(&F_paramPos, v1099_h); wv t2075 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t2074, t2075); }), rt_s(&S614)), "whineStmt");
  }
  (void)rt_chk(s868_emit(up, rt_s(&S615)), "whineStmt");
  (void)rt_chk(s868_emit(up, rt_s(&S93)), "whineStmt");
  up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
  (void)rt_chk(s882_resets(up, rt_get(&F_body, v1099_h)), "whineStmt");
  (void)rt_chk(s881_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1099_h))), "whineStmt");
  (void)rt_chk(s868_emit(up, ({ wv t2076 = rt_text(v1098_r); wv t2077 = rt_text(rt_chk(s884_valueOf(up, rt_get(&F_body, v1099_h)), "whineStmt")); rt_cat(4, (wv[]){ t2076, rt_s(&S493), t2077, rt_s(&S14) }); })), "whineStmt");
  up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
  (void)rt_chk(s868_emit(up, rt_s(&S15)), "whineStmt");
  up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
  (void)rt_chk(s868_emit(up, rt_s(&S15)), "whineStmt");
  wv v1100_rest = rt_bind(v1094_s, 1);
  rt_set_mem(RT_NULL, &F_handler, v1100_rest);
  ({ wv t2080 = ({ wv t2078 = v1098_r; wv t2079 = rt_get(&F_pos, v1094_s); rt_rec(0, 4, K51, (wv[]){ rt_s(&S526), t2078, t2079, rt_b(0) }); }); wv t2081 = v1100_rest; rt_set_mem(t2080, &F_e, t2081); });
  (void)rt_chk(s885_stmt(up, v1100_rest), "whineStmt");
  RT_RET(RT_NULL);
}

static wv s895_chainStmt(struct fr185 *up, wv v1103_e) {
  rt_enter("chainStmt");
  v1103_e = rt_bind(v1103_e, 0);
  wv v1104_k = rt_i(0);
  while (rt_cond(({ wv t2082 = v1104_k; wv t2083 = std_length(rt_get(&F_branches, v1103_e)); rt_lt(t2082, t2083); }))) {
    wv v1105_b = rt_bind(({ wv t2084 = v1104_k; wv t2085 = rt_get(&F_branches, v1103_e); rt_idx(t2084, t2085); }), 0);
    (void)rt_chk(s868_emit(up, ({ wv t2086 = rt_text((rt_cond(rt_gt(v1104_k, rt_i(0))) ? rt_s(&S527) : rt_s(&S13))); wv t2087 = rt_text(rt_chk(s896_test(up, v1105_b), "chainStmt")); rt_cat(4, (wv[]){ t2086, rt_s(&S528), t2087, rt_s(&S496) }); })), "chainStmt");
    up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
    (void)rt_chk(s883_blockBody(up, rt_get(&F_body, v1105_b)), "chainStmt");
    up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
    v1104_k = rt_add(v1104_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v1103_e), RT_NULL))) {
    (void)rt_chk(s868_emit(up, rt_s(&S529)), "chainStmt");
    up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
    (void)rt_chk(s883_blockBody(up, rt_get(&F_els, v1103_e)), "chainStmt");
    up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s868_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s896_test(struct fr185 *up, wv v1106_b) {
  rt_enter("test");
  v1106_b = rt_bind(v1106_b, 0);
  RT_RET(({ wv t2088 = rt_text((rt_cond(rt_get(&F_neg, v1106_b)) ? rt_s(&S87) : rt_s(&S13))); wv t2089 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_cond, v1106_b)), "test"))); rt_cat(4, (wv[]){ t2088, rt_s(&S590), t2089, rt_s(&S16) }); }));
}

static wv s897_branchCount(struct fr185 *up, wv v1107_e) {
  rt_enter("branchCount");
  v1107_e = rt_bind(v1107_e, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v1107_e), rt_s(&S193))) ? std_length(rt_get(&F_branches, v1107_e)) : std_length(rt_get(&F_alibis, v1107_e))));
}

static wv s898_branchBody(struct fr185 *up, wv v1108_e, wv v1109_i) {
  rt_enter("branchBody");
  v1108_e = rt_bind(v1108_e, 0);
  v1109_i = rt_bind(v1109_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v1108_e), rt_s(&S193))) ? rt_get(&F_body, ({ wv t2090 = v1109_i; wv t2091 = rt_get(&F_branches, v1108_e); rt_idx(t2090, t2091); })) : rt_get(&F_body, ({ wv t2092 = v1109_i; wv t2093 = rt_get(&F_alibis, v1108_e); rt_idx(t2092, t2093); }))));
}

static wv s899_branchTest(struct fr185 *up, wv v1110_e, wv v1111_t, wv v1112_i) {
  rt_enter("branchTest");
  v1110_e = rt_bind(v1110_e, 0);
  v1111_t = rt_bind(v1111_t, 0);
  v1112_i = rt_bind(v1112_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v1110_e), rt_s(&S193))) ? rt_chk(s896_test(up, ({ wv t2094 = v1112_i; wv t2095 = rt_get(&F_branches, v1110_e); rt_idx(t2094, t2095); })), "branchTest") : ({ wv t2098 = v1111_t; wv t2099 = ({ wv t2096 = v1112_i; wv t2097 = rt_get(&F_alibis, v1110_e); rt_idx(t2096, t2097); }); rt_chk(s904_alibiTest(up, t2098, t2099), "branchTest"); })));
}

static wv s900_isPlain(struct fr185 *up, wv v1113_e) {
  rt_enter("isPlain");
  v1113_e = rt_bind(v1113_e, 0);
  wv v1114_k = rt_i(0);
  while (rt_cond(({ wv t2100 = v1114_k; wv t2101 = rt_chk(s897_branchCount(up, v1113_e), "isPlain"); rt_le(t2100, t2101); }))) {
    wv v1115_b = rt_bind((rt_cond(({ wv t2102 = v1114_k; wv t2103 = rt_chk(s897_branchCount(up, v1113_e), "isPlain"); rt_lt(t2102, t2103); })) ? rt_chk(s898_branchBody(up, v1113_e, v1114_k), "isPlain") : rt_get(&F_els, v1113_e)), 0);
    wv v1116_j = rt_i(0);
    while (rt_cond(({ wv t2104 = v1116_j; wv t2105 = std_length(rt_get(&F_stmts, v1115_b)); rt_lt(t2104, t2105); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t2106 = v1116_j; wv t2107 = rt_get(&F_stmts, v1115_b); rt_idx(t2106, t2107); })), rt_s(&S207)))) {
        RT_RET(rt_b(0));
      }
      v1116_j = rt_add(v1116_j, rt_i(1));
    }
    v1114_k = rt_add(v1114_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s901_ternaryChain(struct fr185 *up, wv v1117_e, wv v1118_t) {
  rt_enter("ternaryChain");
  v1117_e = rt_bind(v1117_e, 0);
  v1118_t = rt_bind(v1118_t, 0);
  wv v1119_code = rt_bind(rt_chk(s884_valueOf(up, rt_get(&F_els, v1117_e)), "ternaryChain"), 1);
  wv v1120_k = rt_sub(rt_chk(s897_branchCount(up, v1117_e), "ternaryChain"), rt_i(1));
  while (rt_cond(rt_ge(v1120_k, rt_i(0)))) {
    v1119_code = ({ wv t2108 = rt_text(rt_chk(s899_branchTest(up, v1117_e, v1118_t, v1120_k), "ternaryChain")); wv t2109 = rt_text(rt_chk(s884_valueOf(up, rt_chk(s898_branchBody(up, v1117_e, v1120_k), "ternaryChain")), "ternaryChain")); wv t2110 = rt_text(v1119_code); rt_cat(7, (wv[]){ rt_s(&S90), t2108, rt_s(&S530), t2109, rt_s(&S531), t2110, rt_s(&S16) }); });
    v1120_k = rt_sub(v1120_k, rt_i(1));
  }
  RT_RET(v1119_code);
}

static wv s902_branchValue(struct fr185 *up, wv v1121_e, wv v1122_t, wv v1123_subject) {
  rt_enter("branchValue");
  v1121_e = rt_bind(v1121_e, 0);
  v1122_t = rt_bind(v1122_t, 0);
  v1123_subject = rt_bind(v1123_subject, 0);
  wv v1124_start = rt_bind(std_length(rt_def(up->v838_out, "out")), 0);
  wv v1125_savedIndent = rt_bind(rt_def(up->v839_indent, "indent"), 0);
  wv v1126_savedLoops = rt_bind(rt_def(up->v840_loops, "loops"), 0);
  wv v1127_savedIife = rt_bind(rt_def(up->v841_iife, "iife"), 0);
  wv t2111 = rt_add(v1125_savedIndent, rt_i(1));
  (void)rt_def(up->v839_indent, "indent");
  up->v839_indent = t2111;
  wv t2112 = rt_i(0);
  (void)rt_def(up->v840_loops, "loops");
  up->v840_loops = t2112;
  wv t2113 = rt_b(1);
  (void)rt_def(up->v841_iife, "iife");
  up->v841_iife = t2113;
  if (rt_cond(rt_ne(v1123_subject, RT_NULL))) {
    (void)rt_chk(s868_emit(up, ({ wv t2114 = rt_text(v1122_t); wv t2115 = rt_text(v1123_subject); rt_cat(5, (wv[]){ rt_s(&S582), t2114, rt_s(&S493), t2115, rt_s(&S14) }); })), "branchValue");
  }
  wv v1128_r = rt_bind(rt_chk(s869_tmp(up), "branchValue"), 0);
  (void)rt_chk(s868_emit(up, rt_cat(3, (wv[]){ rt_s(&S582), rt_text(v1128_r), rt_s(&S14) })), "branchValue");
  wv v1129_k = rt_i(0);
  while (rt_cond(({ wv t2116 = v1129_k; wv t2117 = rt_chk(s897_branchCount(up, v1121_e), "branchValue"); rt_lt(t2116, t2117); }))) {
    wv v1131_b = rt_bind(rt_chk(s898_branchBody(up, v1121_e, v1129_k), "branchValue"), 0);
    (void)rt_chk(s868_emit(up, ({ wv t2118 = rt_text((rt_cond(rt_gt(v1129_k, rt_i(0))) ? rt_s(&S527) : rt_s(&S13))); wv t2119 = rt_text(rt_chk(s899_branchTest(up, v1121_e, v1122_t, v1129_k), "branchValue")); rt_cat(4, (wv[]){ t2118, rt_s(&S528), t2119, rt_s(&S496) }); })), "branchValue");
    up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
    (void)rt_chk(s882_resets(up, v1131_b), "branchValue");
    (void)rt_chk(s881_stmts(up, rt_get(&F_stmts, v1131_b)), "branchValue");
    (void)rt_chk(s868_emit(up, ({ wv t2120 = rt_text(v1128_r); wv t2121 = rt_text(rt_chk(s884_valueOf(up, v1131_b), "branchValue")); rt_cat(4, (wv[]){ t2120, rt_s(&S493), t2121, rt_s(&S14) }); })), "branchValue");
    up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
    v1129_k = rt_add(v1129_k, rt_i(1));
  }
  (void)rt_chk(s868_emit(up, rt_s(&S529)), "branchValue");
  up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
  (void)rt_chk(s882_resets(up, rt_get(&F_els, v1121_e)), "branchValue");
  (void)rt_chk(s881_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v1121_e))), "branchValue");
  (void)rt_chk(s868_emit(up, ({ wv t2122 = rt_text(v1128_r); wv t2123 = rt_text(rt_chk(s884_valueOf(up, rt_get(&F_els, v1121_e)), "branchValue")); rt_cat(4, (wv[]){ t2122, rt_s(&S493), t2123, rt_s(&S14) }); })), "branchValue");
  up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
  (void)rt_chk(s868_emit(up, rt_s(&S15)), "branchValue");
  (void)rt_chk(s868_emit(up, rt_cat(2, (wv[]){ rt_text(v1128_r), rt_s(&S14) })), "branchValue");
  wv v1130_body = rt_bind(std_join(({ wv t2124 = rt_def(up->v838_out, "out"); wv t2125 = v1124_start; wv t2126 = std_length(rt_def(up->v838_out, "out")); std_slice(t2124, t2125, t2126); }), rt_s(&S2)), 0);
  while (rt_cond(({ wv t2127 = std_length(rt_def(up->v838_out, "out")); wv t2128 = v1124_start; rt_gt(t2127, t2128); }))) {
    (void)std_pop(rt_def(up->v838_out, "out"));
  }
  wv t2129 = rt_bind(v1125_savedIndent, 1);
  (void)rt_def(up->v839_indent, "indent");
  up->v839_indent = t2129;
  wv t2130 = rt_bind(v1126_savedLoops, 1);
  (void)rt_def(up->v840_loops, "loops");
  up->v840_loops = t2130;
  wv t2131 = rt_bind(v1127_savedIife, 1);
  (void)rt_def(up->v841_iife, "iife");
  up->v841_iife = t2131;
  RT_RET(({ wv t2132 = rt_text(v1130_body); wv t2133 = rt_text(rt_chk(w7_repeat(rt_s(&S478), rt_def(up->v839_indent, "indent")), "branchValue")); rt_cat(5, (wv[]){ rt_s(&S616), t2132, rt_s(&S2), t2133, rt_s(&S617) }); }));
}

static wv s903_chainExpr(struct fr185 *up, wv v1132_e) {
  rt_enter("chainExpr");
  v1132_e = rt_bind(v1132_e, 0);
  if (rt_cond(rt_chk(s900_isPlain(up, v1132_e), "chainExpr"))) {
    RT_RET(rt_chk(s901_ternaryChain(up, v1132_e, RT_NULL), "chainExpr"));
  }
  RT_RET(rt_chk(s902_branchValue(up, v1132_e, RT_NULL, RT_NULL), "chainExpr"));
}

static wv s904_alibiTest(struct fr185 *up, wv v1133_t, wv v1134_a) {
  rt_enter("alibiTest");
  v1133_t = rt_bind(v1133_t, 0);
  v1134_a = rt_bind(v1134_a, 0);
  wv v1135_tests = rt_arr(0, NULL, 1);
  wv v1136_k = rt_i(0);
  while (rt_cond(({ wv t2134 = v1136_k; wv t2135 = std_length(rt_get(&F_values, v1134_a)); rt_lt(t2134, t2135); }))) {
    (void)({ wv t2140 = v1135_tests; wv t2141 = ({ wv t2138 = rt_text(v1133_t); wv t2139 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, ({ wv t2136 = v1136_k; wv t2137 = rt_get(&F_values, v1134_a); rt_idx(t2136, t2137); })), "alibiTest"))); rt_cat(5, (wv[]){ rt_s(&S618), t2138, rt_s(&S390), t2139, rt_s(&S16) }); }); std_push(t2140, t2141); });
    v1136_k = rt_add(v1136_k, rt_i(1));
  }
  RT_RET(std_join(v1135_tests, rt_s(&S535)));
}

static wv s905_suspectElse(struct fr185 *up, wv v1137_e, wv v1138_t) {
  rt_enter("suspectElse");
  v1137_e = rt_bind(v1137_e, 0);
  v1138_t = rt_bind(v1138_t, 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v1137_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_alibis, v1137_e))))), rt_s(&S198)), "||")))) {
    RT_RET(rt_get(&F_els, v1137_e));
  }
  RT_RET(({ wv t2142 = rt_arr(0, NULL, 0); wv t2143 = rt_rec(0, 4, K51, (wv[]){ rt_s(&S526), rt_cat(3, (wv[]){ rt_s(&S619), rt_text(v1138_t), rt_s(&S620) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K22, (wv[]){ t2142, t2143, RT_NULL }); }));
}

static wv s906_suspectStmt(struct fr185 *up, wv v1139_e) {
  rt_enter("suspectStmt");
  v1139_e = rt_bind(v1139_e, 0);
  wv v1140_t = rt_bind(rt_chk(s869_tmp(up), "suspectStmt"), 0);
  (void)rt_chk(s868_emit(up, ({ wv t2144 = rt_text(v1140_t); wv t2145 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_subject, v1139_e)), "suspectStmt"))); rt_cat(5, (wv[]){ rt_s(&S582), t2144, rt_s(&S493), t2145, rt_s(&S14) }); })), "suspectStmt");
  wv v1141_k = rt_i(0);
  while (rt_cond(({ wv t2146 = v1141_k; wv t2147 = std_length(rt_get(&F_alibis, v1139_e)); rt_lt(t2146, t2147); }))) {
    (void)rt_chk(s868_emit(up, ({ wv t2152 = rt_text((rt_cond(rt_gt(v1141_k, rt_i(0))) ? rt_s(&S527) : rt_s(&S13))); wv t2153 = rt_text(({ wv t2150 = v1140_t; wv t2151 = ({ wv t2148 = v1141_k; wv t2149 = rt_get(&F_alibis, v1139_e); rt_idx(t2148, t2149); }); rt_chk(s904_alibiTest(up, t2150, t2151), "suspectStmt"); })); rt_cat(4, (wv[]){ t2152, rt_s(&S528), t2153, rt_s(&S496) }); })), "suspectStmt");
    up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
    (void)rt_chk(s883_blockBody(up, rt_get(&F_body, ({ wv t2154 = v1141_k; wv t2155 = rt_get(&F_alibis, v1139_e); rt_idx(t2154, t2155); }))), "suspectStmt");
    up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
    v1141_k = rt_add(v1141_k, rt_i(1));
  }
  wv v1142_els = rt_bind(rt_chk(s905_suspectElse(up, v1139_e, v1140_t), "suspectStmt"), 0);
  if (rt_cond(rt_ne(v1142_els, RT_NULL))) {
    (void)rt_chk(s868_emit(up, rt_s(&S529)), "suspectStmt");
    up->v839_indent = rt_add(rt_def(up->v839_indent, "indent"), rt_i(1));
    (void)rt_chk(s883_blockBody(up, v1142_els), "suspectStmt");
    up->v839_indent = rt_sub(rt_def(up->v839_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s868_emit(up, rt_s(&S15)), "suspectStmt");
  RT_RET(RT_NULL);
}

static wv s907_suspectExpr(struct fr185 *up, wv v1143_e) {
  rt_enter("suspectExpr");
  v1143_e = rt_bind(v1143_e, 0);
  wv v1144_t = rt_bind(rt_chk(s869_tmp(up), "suspectExpr"), 0);
  wv v1145_subject = rt_bind(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_subject, v1143_e)), "suspectExpr")), 0);
  wv v1146_withElse = rt_bind(v1143_e, 1);
  ({ wv t2156 = rt_chk(s905_suspectElse(up, v1143_e, v1144_t), "suspectExpr"); wv t2157 = v1146_withElse; rt_set_mem(t2156, &F_els, t2157); });
  if (rt_cond(rt_chk(s900_isPlain(up, v1146_withElse), "suspectExpr"))) {
    RT_RET(({ wv t2158 = rt_text(v1144_t); wv t2159 = rt_text(v1145_subject); wv t2160 = rt_text(rt_chk(s901_ternaryChain(up, v1146_withElse, v1144_t), "suspectExpr")); rt_cat(7, (wv[]){ rt_s(&S621), t2158, rt_s(&S493), t2159, rt_s(&S622), t2160, rt_s(&S584) }); }));
  }
  RT_RET(rt_chk(s902_branchValue(up, v1146_withElse, v1144_t, v1145_subject), "suspectExpr"));
}

static wv s908_callC(struct fr185 *up, wv v1147_callee, wv v1148_args, wv v1149_checked) {
  rt_enter("callC");
  v1147_callee = rt_bind(v1147_callee, 0);
  v1149_checked = rt_bind(v1149_checked, 0);
  wv v1150_code = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v1147_callee), rt_s(&S164)))) {
    v1150_code = ({ wv t2161 = rt_text(rt_get(&F_name, v1147_callee)); wv t2162 = rt_text(std_join(v1148_args, rt_s(&S390))); rt_cat(5, (wv[]){ rt_s(&S623), t2161, rt_s(&S90), t2162, rt_s(&S16) }); });
    if (rt_cond(rt_not(({ wv t2163 = rt_def(w7_signalers, "signalers"); wv t2164 = rt_get(&F_name, v1147_callee); rt_chk(w1_contains(t2163, t2164), "callC"); })))) {
      RT_RET(v1150_code);
    }
  } else {
    wv v1151_f = rt_s(&S13);
    wv v1152_parent = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_k, v1147_callee), rt_s(&S198)))) {
      v1151_f = ({ wv t2165 = rt_text(rt_chk(s852_modId(up, rt_chk(s873_stolenFrom(up, rt_get(&F_rec, v1147_callee)), "callC")), "callC")); wv t2166 = rt_text(rt_get(&F_field, v1147_callee)); rt_cat(4, (wv[]){ rt_s(&S94), t2165, rt_s(&S12), t2166 }); });
    } else {
      wv v1155_d = rt_bind(({ wv t2167 = rt_get(&F_pos, v1147_callee); wv t2168 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t2167, t2168); }), 0);
      wv v1156_sf = rt_bind(rt_get(&F_schemeFn, ({ wv t2169 = v1155_d; wv t2170 = rt_def(up->v842_decls, "decls"); rt_idx(t2169, t2170); })), 0);
      v1151_f = rt_bind(rt_get(&F_cname, ({ wv t2171 = v1156_sf; wv t2172 = rt_def(up->v843_fns, "fns"); rt_idx(t2171, t2172); })), 1);
      wv v1157_pf = rt_bind(rt_get(&F_parent, ({ wv t2173 = v1156_sf; wv t2174 = rt_def(up->v843_fns, "fns"); rt_idx(t2173, t2174); })), 0);
      if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2175 = v1157_pf; wv t2176 = rt_def(up->v843_fns, "fns"); rt_idx(t2175, t2176); }))))) {
        v1152_parent = rt_bind(v1157_pf, 1);
      }
    }
    wv v1153_all = rt_arr(0, NULL, 1);
    if (rt_cond(rt_ne(v1152_parent, RT_NULL))) {
      (void)({ wv t2185 = v1153_all; wv t2186 = (rt_cond(({ wv t2177 = v1152_parent; wv t2178 = rt_def(up->v851_cur, "cur"); rt_eq(t2177, t2178); })) ? rt_s(&S624) : rt_cat(2, (wv[]){ rt_s(&S578), rt_text(rt_chk(w7_repeat(rt_s(&S579), rt_sub(({ wv t2183 = rt_get(&F_depth, ({ wv t2179 = rt_def(up->v851_cur, "cur"); wv t2180 = rt_def(up->v843_fns, "fns"); rt_idx(t2179, t2180); })); wv t2184 = rt_get(&F_depth, ({ wv t2181 = v1152_parent; wv t2182 = rt_def(up->v843_fns, "fns"); rt_idx(t2181, t2182); })); rt_sub(t2183, t2184); }), rt_i(1))), "callC")) })); std_push(t2185, t2186); });
    }
    wv v1154_k = rt_i(0);
    while (rt_cond(({ wv t2187 = v1154_k; wv t2188 = std_length(v1148_args); rt_lt(t2187, t2188); }))) {
      (void)({ wv t2189 = v1153_all; wv t2190 = rt_idx(v1154_k, v1148_args); std_push(t2189, t2190); });
      v1154_k = rt_add(v1154_k, rt_i(1));
    }
    v1150_code = ({ wv t2191 = rt_text(v1151_f); wv t2192 = rt_text(std_join(v1153_all, rt_s(&S390))); rt_cat(4, (wv[]){ t2191, rt_s(&S90), t2192, rt_s(&S16) }); });
  }
  RT_RET((rt_cond(v1149_checked) ? ({ wv t2197 = rt_text(v1150_code); wv t2198 = rt_text((rt_cond(rt_get(&F_top, ({ wv t2193 = rt_def(up->v851_cur, "cur"); wv t2194 = rt_def(up->v843_fns, "fns"); rt_idx(t2193, t2194); }))) ? rt_s(&S569) : rt_chk(w1_quote(rt_get(&F_name, ({ wv t2195 = rt_def(up->v851_cur, "cur"); wv t2196 = rt_def(up->v843_fns, "fns"); rt_idx(t2195, t2196); }))), "callC"))); rt_cat(5, (wv[]){ rt_s(&S625), t2197, rt_s(&S390), t2198, rt_s(&S16) }); }) : v1150_code));
}

static wv s909_expr(struct fr185 *up, wv v1158_e) {
  rt_enter("expr");
  wv v1159_kind = rt_bind(rt_get(&F_k, v1158_e), 0);
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S106)))) {
    RT_RET(({ wv t2199 = rt_cat(3, (wv[]){ rt_s(&S591), rt_text(rt_get(&F_v, v1158_e)), rt_s(&S16) }); wv t2200 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2199, t2200 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S109)))) {
    RT_RET(({ wv t2201 = rt_cat(3, (wv[]){ rt_s(&S626), rt_text(rt_get(&F_v, v1158_e)), rt_s(&S16) }); wv t2202 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2201, t2202 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S179)))) {
    wv v1160_parts = rt_arr(0, NULL, 1);
    wv v1161_k = rt_i(0);
    while (rt_cond(({ wv t2203 = v1161_k; wv t2204 = std_length(rt_get(&F_parts, v1158_e)); rt_lt(t2203, t2204); }))) {
      wv v1163_part = rt_bind(({ wv t2205 = v1161_k; wv t2206 = rt_get(&F_parts, v1158_e); rt_idx(t2205, t2206); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v1163_part), rt_s(&S194)))) {
        if (rt_cond(rt_ne(v1163_part, rt_s(&S13)))) {
          (void)({ wv t2209 = v1160_parts; wv t2210 = ({ wv t2207 = rt_cat(3, (wv[]){ rt_s(&S627), rt_text(rt_chk(s870_str(up, v1163_part), "expr")), rt_s(&S16) }); wv t2208 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2207, t2208 }); }); std_push(t2209, t2210); });
        }
      } else {
        (void)({ wv t2213 = v1160_parts; wv t2214 = ({ wv t2211 = rt_cat(3, (wv[]){ rt_s(&S628), rt_text(rt_get(&F_c, rt_chk(s909_expr(up, v1163_part), "expr"))), rt_s(&S16) }); wv t2212 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t2211, t2212 }); }); std_push(t2213, t2214); });
      }
      v1161_k = rt_add(v1161_k, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v1160_parts), rt_i(0)))) {
      RT_RET(({ wv t2215 = rt_cat(3, (wv[]){ rt_s(&S627), rt_text(rt_chk(s870_str(up, rt_s(&S13)), "expr")), rt_s(&S16) }); wv t2216 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2215, t2216 }); }));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v1160_parts), rt_i(1)), "&&") && rt_bool(({ wv t2217 = rt_get(&F_k, rt_idx(rt_i(0), v1160_parts)); wv t2218 = rt_def(w7_constK, "constK"); rt_eq(t2217, t2218); }), "&&")))) {
      RT_RET(rt_idx(rt_i(0), v1160_parts));
    }
    wv v1162_sq = rt_bind(rt_chk(s879_seq(up, v1160_parts), "expr"), 0);
    RT_RET(({ wv t2221 = v1162_sq; wv t2222 = ({ wv t2219 = rt_text(std_length(rt_get(&F_cs, v1162_sq))); wv t2220 = rt_text(rt_chk(w7_values(rt_get(&F_cs, v1162_sq)), "expr")); rt_cat(5, (wv[]){ rt_s(&S629), t2219, rt_s(&S390), t2220, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2221, t2222), "expr"); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S291)))) {
    RT_RET(({ wv t2223 = rt_cat(3, (wv[]){ rt_s(&S630), rt_text((rt_cond(rt_get(&F_v, v1158_e)) ? rt_i(1) : rt_i(0))), rt_s(&S16) }); wv t2224 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2223, t2224 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S292)))) {
    RT_RET(rt_rec(0, 2, K56, (wv[]){ rt_s(&S587), rt_def(w7_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S293)))) {
    RT_RET(rt_rec(0, 2, K56, (wv[]){ rt_s(&S631), rt_def(w7_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S294)))) {
    RT_RET(rt_rec(0, 2, K56, (wv[]){ rt_s(&S632), rt_def(w7_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S264)))) {
    RT_RET(rt_chk(s878_read(up, ({ wv t2225 = rt_get(&F_pos, v1158_e); wv t2226 = rt_def(up->v848_curSlot, "curSlot"); rt_idx(t2225, t2226); })), "expr"));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S164)))) {
    RT_RET(({ wv t2227 = rt_cat(3, (wv[]){ rt_s(&S623), rt_text(rt_get(&F_name, v1158_e)), rt_s(&S633) }); wv t2228 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2227, t2228 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S195)))) {
    RT_RET(rt_chk(s892_array(up, v1158_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S196)))) {
    RT_RET(rt_chk(s893_record(up, v1158_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S197)))) {
    wv v1164_sq = rt_bind(rt_chk(s879_seq(up, ({ wv t2229 = rt_chk(s909_expr(up, rt_get(&F_index, v1158_e)), "expr"); wv t2230 = rt_chk(s909_expr(up, rt_get(&F_arr, v1158_e)), "expr"); rt_arr(2, (wv[]){ t2229, t2230 }, 0); })), "expr"), 0);
    RT_RET(({ wv t2233 = v1164_sq; wv t2234 = ({ wv t2231 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1164_sq))); wv t2232 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1164_sq))); rt_cat(5, (wv[]){ rt_s(&S634), t2231, rt_s(&S390), t2232, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2233, t2234), "expr"); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S198)))) {
    wv v1165_lin = rt_bind(rt_chk(s875_lineupOf(up, rt_get(&F_rec, v1158_e)), "expr"), 0);
    if (rt_cond(rt_ne(v1165_lin, RT_NULL))) {
      wv v1169_ms = rt_bind(rt_get(&F_members, rt_get(&F_node, v1165_lin)), 0);
      wv v1170_k = rt_i(0);
      while (rt_cond(({ wv t2235 = rt_get(&F_name, rt_idx(v1170_k, v1169_ms)); wv t2236 = rt_get(&F_field, v1158_e); rt_ne(t2235, t2236); }))) {
        v1170_k = rt_add(v1170_k, rt_i(1));
      }
      RT_RET(({ wv t2239 = ({ wv t2237 = rt_text(rt_get(&F_c, v1165_lin)); wv t2238 = rt_text(v1170_k); rt_cat(5, (wv[]){ rt_s(&S635), t2237, rt_s(&S91), t2238, rt_s(&S506) }); }); wv t2240 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2239, t2240 }); }));
    }
    wv v1166_lib = rt_bind(rt_chk(s873_stolenFrom(up, rt_get(&F_rec, v1158_e)), "expr"), 0);
    if (rt_cond(rt_ne(v1166_lib, RT_NULL))) {
      RT_RET(({ wv t2243 = ({ wv t2241 = rt_text(rt_chk(s852_modId(up, v1166_lib), "expr")); wv t2242 = rt_text(rt_get(&F_field, v1158_e)); rt_cat(4, (wv[]){ rt_s(&S94), t2241, rt_s(&S12), t2242 }); }); wv t2244 = rt_def(w7_pureK, "pureK"); rt_rec(0, 2, K56, (wv[]){ t2243, t2244 }); }));
    }
    wv v1167_key = ({ wv t2245 = rt_chk(s871_fieldRef(up, rt_get(&F_field, v1158_e)), "expr"); wv t2246 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2245, t2246 }); });
    wv v1168_sq = rt_bind(rt_chk(s879_seq(up, ({ wv t2247 = v1167_key; wv t2248 = rt_chk(s909_expr(up, rt_get(&F_rec, v1158_e)), "expr"); rt_arr(2, (wv[]){ t2247, t2248 }, 0); })), "expr"), 0);
    RT_RET(({ wv t2251 = v1168_sq; wv t2252 = ({ wv t2249 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1168_sq))); wv t2250 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1168_sq))); rt_cat(5, (wv[]){ rt_s(&S636), t2249, rt_s(&S390), t2250, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2251, t2252), "expr"); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S199)))) {
    RT_RET(({ wv t2255 = ({ wv t2253 = rt_text(rt_chk(w7_unFn(rt_get(&F_op, v1158_e)), "expr")); wv t2254 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_e, v1158_e)), "expr"))); rt_cat(5, (wv[]){ rt_s(&S606), t2253, rt_s(&S90), t2254, rt_s(&S16) }); }); wv t2256 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t2255, t2256 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S200)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v1158_e), rt_s(&S82)), "||") || rt_bool(rt_eq(rt_get(&F_op, v1158_e), rt_s(&S83)), "||")))) {
      RT_RET(({ wv t2262 = ({ wv t2257 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_l, v1158_e)), "expr"))); wv t2258 = rt_text(rt_get(&F_op, v1158_e)); wv t2259 = rt_text(rt_get(&F_op, v1158_e)); wv t2260 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_r, v1158_e)), "expr"))); wv t2261 = rt_text(rt_get(&F_op, v1158_e)); rt_cat(11, (wv[]){ rt_s(&S637), t2257, rt_s(&S554), t2258, rt_s(&S555), t2259, rt_s(&S638), t2260, rt_s(&S554), t2261, rt_s(&S557) }); }); wv t2263 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t2262, t2263 }); }));
    }
    wv v1171_sq = rt_bind(rt_chk(s879_seq(up, ({ wv t2264 = rt_chk(s909_expr(up, rt_get(&F_l, v1158_e)), "expr"); wv t2265 = rt_chk(s909_expr(up, rt_get(&F_r, v1158_e)), "expr"); rt_arr(2, (wv[]){ t2264, t2265 }, 0); })), "expr"), 0);
    RT_RET(({ wv t2269 = v1171_sq; wv t2270 = ({ wv t2266 = rt_text(rt_chk(w7_binFn(rt_get(&F_op, v1158_e)), "expr")); wv t2267 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1171_sq))); wv t2268 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1171_sq))); rt_cat(7, (wv[]){ rt_s(&S606), t2266, rt_s(&S90), t2267, rt_s(&S390), t2268, rt_s(&S16) }); }); rt_chk(s880_wrap(up, t2269, t2270), "expr"); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S201)))) {
    RT_RET(({ wv t2274 = ({ wv t2271 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_c, v1158_e)), "expr"))); wv t2272 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_a, v1158_e)), "expr"))); wv t2273 = rt_text(rt_get(&F_c, rt_chk(s909_expr(up, rt_get(&F_b, v1158_e)), "expr"))); rt_cat(7, (wv[]){ rt_s(&S639), t2271, rt_s(&S559), t2272, rt_s(&S531), t2273, rt_s(&S16) }); }); wv t2275 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t2274, t2275 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S191)))) {
    wv v1172_sq = rt_bind(rt_chk(s879_seq(up, rt_chk(s891_exprs(up, rt_get(&F_args, v1158_e)), "expr")), "expr"), 0);
    RT_RET(({ wv t2278 = v1172_sq; wv t2279 = ({ wv t2276 = rt_get(&F_callee, v1158_e); wv t2277 = rt_get(&F_cs, v1172_sq); rt_chk(s908_callC(up, t2276, t2277, rt_b(1)), "expr"); }); rt_chk(s880_wrap(up, t2278, t2279), "expr"); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S193)))) {
    RT_RET(({ wv t2280 = rt_chk(s903_chainExpr(up, v1158_e), "expr"); wv t2281 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t2280, t2281 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S51)))) {
    RT_RET(({ wv t2282 = rt_chk(s907_suspectExpr(up, v1158_e), "expr"); wv t2283 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K56, (wv[]){ t2282, t2283 }); }));
  }
  if (rt_cond(rt_eq(v1159_kind, rt_s(&S526)))) {
    RT_RET(({ wv t2284 = rt_get(&F_js, v1158_e); wv t2285 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K56, (wv[]){ t2284, t2285 }); }));
  }
  rt_tantrum(rt_cat(2, (wv[]){ rt_s(&S560), rt_text(v1159_kind) }));
  RT_RET(RT_NULL);
}

static wv s910_assemble(struct fr185 *up) {
  rt_enter("assemble");
  wv v1173_all = rt_arr(3, (wv[]){ std_join(std_split(up->v828_runtime, rt_s(&S640)), rt_s(&S2)), rt_s(&S641), rt_s(&S13) }, 1);
  wv v1174_k = rt_i(0);
  while (rt_cond(({ wv t2286 = v1174_k; wv t2287 = std_length(rt_def(up->v831_strings, "strings")); rt_lt(t2286, t2287); }))) {
    (void)({ wv t2292 = v1173_all; wv t2293 = ({ wv t2290 = rt_text(v1174_k); wv t2291 = rt_text(rt_chk(w7_wstrInit(({ wv t2288 = v1174_k; wv t2289 = rt_def(up->v831_strings, "strings"); rt_idx(t2288, t2289); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S642), t2290, rt_s(&S493), t2291, rt_s(&S14) }); }); std_push(t2292, t2293); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2294 = v1174_k; wv t2295 = std_length(rt_def(up->v832_fields, "fields")); rt_lt(t2294, t2295); }))) {
    (void)({ wv t2302 = v1173_all; wv t2303 = ({ wv t2300 = rt_text(({ wv t2296 = v1174_k; wv t2297 = rt_def(up->v832_fields, "fields"); rt_idx(t2296, t2297); })); wv t2301 = rt_text(rt_chk(w7_wstrInit(({ wv t2298 = v1174_k; wv t2299 = rt_def(up->v832_fields, "fields"); rt_idx(t2298, t2299); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S643), t2300, rt_s(&S493), t2301, rt_s(&S14) }); }); std_push(t2302, t2303); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2304 = v1174_k; wv t2305 = std_length(rt_def(up->v833_shapes, "shapes")); rt_lt(t2304, t2305); }))) {
    wv v1175_names = rt_bind(std_split(({ wv t2306 = v1174_k; wv t2307 = rt_def(up->v833_shapes, "shapes"); rt_idx(t2306, t2307); }), rt_s(&S89)), 0);
    wv v1176_refs = rt_arr(0, NULL, 1);
    wv v1177_j = rt_i(0);
    while (rt_cond(({ wv t2308 = v1177_j; wv t2309 = std_length(v1175_names); rt_lt(t2308, t2309); }))) {
      (void)({ wv t2310 = v1176_refs; wv t2311 = rt_cat(2, (wv[]){ rt_s(&S573), rt_text(rt_idx(v1177_j, v1175_names)) }); std_push(t2310, t2311); });
      v1177_j = rt_add(v1177_j, rt_i(1));
    }
    (void)({ wv t2314 = v1173_all; wv t2315 = ({ wv t2312 = rt_text(v1174_k); wv t2313 = rt_text(std_join(v1176_refs, rt_s(&S390))); rt_cat(5, (wv[]){ rt_s(&S644), t2312, rt_s(&S575), t2313, rt_s(&S576) }); }); std_push(t2314, t2315); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2316 = v1174_k; wv t2317 = std_length(rt_def(up->v834_lineupDefs, "lineupDefs")); rt_lt(t2316, t2317); }))) {
    (void)({ wv t2320 = v1173_all; wv t2321 = ({ wv t2318 = v1174_k; wv t2319 = rt_def(up->v834_lineupDefs, "lineupDefs"); rt_idx(t2318, t2319); }); std_push(t2320, t2321); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2322 = v1174_k; wv t2323 = std_length(rt_def(up->v836_frames, "frames")); rt_lt(t2322, t2323); }))) {
    wv v1178_f = rt_bind(({ wv t2324 = v1174_k; wv t2325 = rt_def(up->v836_frames, "frames"); rt_idx(t2324, t2325); }), 0);
    wv v1179_members = rt_arr(0, NULL, 1);
    wv v1180_pf = rt_bind(rt_get(&F_parent, ({ wv t2326 = v1178_f; wv t2327 = rt_def(up->v843_fns, "fns"); rt_idx(t2326, t2327); })), 0);
    if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2328 = v1180_pf; wv t2329 = rt_def(up->v843_fns, "fns"); rt_idx(t2328, t2329); }))))) {
      (void)({ wv t2330 = v1179_members; wv t2331 = rt_cat(3, (wv[]){ rt_s(&S596), rt_text(v1180_pf), rt_s(&S645) }); std_push(t2330, t2331); });
    }
    wv v1181_j = rt_i(0);
    while (rt_cond(({ wv t2332 = v1181_j; wv t2333 = std_length(rt_def(up->v842_decls, "decls")); rt_lt(t2332, t2333); }))) {
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(({ wv t2336 = rt_get(&F_fn, ({ wv t2334 = v1181_j; wv t2335 = rt_def(up->v842_decls, "decls"); rt_idx(t2334, t2335); })); wv t2337 = v1178_f; rt_eq(t2336, t2337); }), "&&") && rt_bool(rt_get(&F_captured, ({ wv t2338 = v1181_j; wv t2339 = rt_def(up->v842_decls, "decls"); rt_idx(t2338, t2339); })), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_kind, ({ wv t2340 = v1181_j; wv t2341 = rt_def(up->v842_decls, "decls"); rt_idx(t2340, t2341); })), rt_s(&S33)), "&&")))) {
        (void)({ wv t2344 = v1179_members; wv t2345 = rt_cat(3, (wv[]){ rt_s(&S582), rt_text(rt_get(&F_cname, ({ wv t2342 = v1181_j; wv t2343 = rt_def(up->v842_decls, "decls"); rt_idx(t2342, t2343); }))), rt_s(&S14) }); std_push(t2344, t2345); });
      }
      v1181_j = rt_add(v1181_j, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v1179_members), rt_i(0)))) {
      (void)std_push(v1179_members, rt_s(&S646));
    }
    (void)({ wv t2348 = v1173_all; wv t2349 = ({ wv t2346 = rt_text(v1178_f); wv t2347 = rt_text(std_join(v1179_members, rt_s(&S0))); rt_cat(5, (wv[]){ rt_s(&S596), t2346, rt_s(&S647), t2347, rt_s(&S576) }); }); std_push(t2348, t2349); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2350 = v1174_k; wv t2351 = std_length(rt_def(up->v835_globals, "globals")); rt_lt(t2350, t2351); }))) {
    (void)({ wv t2354 = v1173_all; wv t2355 = rt_cat(3, (wv[]){ rt_s(&S599), rt_text(({ wv t2352 = v1174_k; wv t2353 = rt_def(up->v835_globals, "globals"); rt_idx(t2352, t2353); })), rt_s(&S14) }); std_push(t2354, t2355); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2356 = v1174_k; wv t2357 = std_length(rt_def(up->v837_functions, "functions")); rt_lt(t2356, t2357); }))) {
    (void)({ wv t2360 = v1173_all; wv t2361 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2358 = v1174_k; wv t2359 = rt_def(up->v837_functions, "functions"); rt_idx(t2358, t2359); }))), rt_s(&S14) }); std_push(t2360, t2361); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2362 = v1174_k; wv t2363 = std_length(rt_def(up->v837_functions, "functions")); rt_lt(t2362, t2363); }))) {
    (void)std_push(v1173_all, rt_s(&S13));
    (void)({ wv t2366 = v1173_all; wv t2367 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2364 = v1174_k; wv t2365 = rt_def(up->v837_functions, "functions"); rt_idx(t2364, t2365); }))), rt_s(&S648) }); std_push(t2366, t2367); });
    wv v1182_lines = rt_bind(rt_get(&F_lines, ({ wv t2368 = v1174_k; wv t2369 = rt_def(up->v837_functions, "functions"); rt_idx(t2368, t2369); })), 0);
    wv v1183_j = rt_i(0);
    while (rt_cond(({ wv t2370 = v1183_j; wv t2371 = std_length(v1182_lines); rt_lt(t2370, t2371); }))) {
      (void)({ wv t2372 = v1173_all; wv t2373 = rt_idx(v1183_j, v1182_lines); std_push(t2372, t2373); });
      v1183_j = rt_add(v1183_j, rt_i(1));
    }
    (void)std_push(v1173_all, rt_s(&S15));
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  (void)std_push(v1173_all, rt_s(&S13));
  (void)std_push(v1173_all, rt_s(&S649));
  if (rt_cond(rt_gt(std_length(rt_def(up->v832_fields, "fields")), rt_i(0)))) {
    wv v1184_refs = rt_arr(0, NULL, 1);
    v1174_k = rt_i(0);
    while (rt_cond(({ wv t2374 = v1174_k; wv t2375 = std_length(rt_def(up->v832_fields, "fields")); rt_lt(t2374, t2375); }))) {
      (void)({ wv t2378 = v1184_refs; wv t2379 = rt_cat(2, (wv[]){ rt_s(&S573), rt_text(({ wv t2376 = v1174_k; wv t2377 = rt_def(up->v832_fields, "fields"); rt_idx(t2376, t2377); })) }); std_push(t2378, t2379); });
      v1174_k = rt_add(v1174_k, rt_i(1));
    }
    (void)({ wv t2382 = v1173_all; wv t2383 = ({ wv t2380 = rt_text(std_length(rt_def(up->v832_fields, "fields"))); wv t2381 = rt_text(std_join(v1184_refs, rt_s(&S390))); rt_cat(5, (wv[]){ rt_s(&S650), t2380, rt_s(&S651), t2381, rt_s(&S652) }); }); std_push(t2382, t2383); });
  }
  v1174_k = rt_i(0);
  while (rt_cond(({ wv t2384 = v1174_k; wv t2385 = std_length(rt_def(up->v838_out, "out")); rt_lt(t2384, t2385); }))) {
    (void)({ wv t2388 = v1173_all; wv t2389 = ({ wv t2386 = v1174_k; wv t2387 = rt_def(up->v838_out, "out"); rt_idx(t2386, t2387); }); std_push(t2388, t2389); });
    v1174_k = rt_add(v1174_k, rt_i(1));
  }
  (void)std_push(v1173_all, rt_s(&S15));
  (void)std_push(v1173_all, rt_s(&S13));
  RT_RET(std_join(v1173_all, rt_s(&S2)));
}

static wv w8_dumpTokens(wv v1187_toks, wv v1188_indent) {
  rt_enter("dumpTokens");
  v1187_toks = rt_bind(v1187_toks, 0);
  v1188_indent = rt_bind(v1188_indent, 0);
  wv v1189_k = rt_i(0);
  while (rt_cond(({ wv t2400 = v1189_k; wv t2401 = std_length(v1187_toks); rt_lt(t2400, t2401); }))) {
    wv v1190_tk = rt_bind(rt_idx(v1189_k, v1187_toks), 0);
    wv v1191_line = ({ wv t2402 = rt_text(v1188_indent); wv t2403 = rt_text(rt_get(&F_t, v1190_tk)); wv t2404 = rt_text(rt_get(&F_pos, v1190_tk)); wv t2405 = rt_text(rt_get(&F_end, v1190_tk)); wv t2406 = rt_text(rt_get(&F_sp, v1190_tk)); rt_cat(8, (wv[]){ t2402, t2403, rt_s(&S0), t2404, rt_s(&S0), t2405, rt_s(&S0), t2406 }); });
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1190_tk), rt_s(&S149)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1190_tk), rt_s(&S164)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1190_tk), rt_s(&S124)), "||")))) {
      v1191_line = ({ wv t2407 = v1191_line; wv t2408 = rt_cat(2, (wv[]){ rt_s(&S0), rt_text(rt_get(&F_v, v1190_tk)) }); rt_add(t2407, t2408); });
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1190_tk), rt_s(&S106)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1190_tk), rt_s(&S109)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1190_tk), rt_s(&S162)), "||")))) {
      v1191_line = ({ wv t2411 = v1191_line; wv t2412 = ({ wv t2409 = rt_text(rt_get(&F_text, v1190_tk)); wv t2410 = rt_text(rt_get(&F_v, v1190_tk)); rt_cat(4, (wv[]){ rt_s(&S0), t2409, rt_s(&S0), t2410 }); }); rt_add(t2411, t2412); });
    }
    (void)std_print(v1191_line);
    if (rt_cond(rt_eq(rt_get(&F_t, v1190_tk), rt_s(&S179)))) {
      wv v1192_p = rt_i(0);
      while (rt_cond(({ wv t2413 = v1192_p; wv t2414 = std_length(rt_get(&F_parts, v1190_tk)); rt_lt(t2413, t2414); }))) {
        wv v1193_part = rt_bind(({ wv t2415 = v1192_p; wv t2416 = rt_get(&F_parts, v1190_tk); rt_idx(t2415, t2416); }), 0);
        if (rt_cond(rt_eq(std_typeOf(v1193_part), rt_s(&S194)))) {
          (void)std_print(({ wv t2417 = rt_text(v1188_indent); wv t2418 = rt_text(rt_chk(w1_quote(v1193_part), "dumpTokens")); rt_cat(3, (wv[]){ t2417, rt_s(&S653), t2418 }); }));
        } else {
          (void)std_print(({ wv t2419 = rt_text(v1188_indent); wv t2420 = rt_text(rt_get(&F_pos, v1193_part)); rt_cat(3, (wv[]){ t2419, rt_s(&S654), t2420 }); }));
          (void)({ wv t2421 = rt_get(&F_tokens, v1193_part); wv t2422 = rt_add(v1188_indent, rt_s(&S655)); rt_chk(w8_dumpTokens(t2421, t2422), "dumpTokens"); });
          (void)std_print(rt_cat(2, (wv[]){ rt_text(v1188_indent), rt_s(&S656) }));
        }
        v1192_p = rt_add(v1192_p, rt_i(1));
      }
    }
    v1189_k = rt_add(v1189_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w8_sorted(wv v1194_xs) {
  rt_enter("sorted");
  v1194_xs = rt_bind(v1194_xs, 0);
  wv v1195_out = rt_arr(0, NULL, 1);
  wv v1196_k = rt_i(0);
  while (rt_cond(({ wv t2423 = v1196_k; wv t2424 = std_length(v1194_xs); rt_lt(t2423, t2424); }))) {
    (void)({ wv t2425 = v1195_out; wv t2426 = rt_idx(v1196_k, v1194_xs); std_push(t2425, t2426); });
    wv v1197_j = rt_sub(std_length(v1195_out), rt_i(1));
    while (rt_cond(rt_b(rt_bool(rt_gt(v1197_j, rt_i(0)), "&&") && rt_bool(({ wv t2429 = ({ wv t2427 = rt_sub(v1197_j, rt_i(1)); wv t2428 = v1195_out; rt_idx(t2427, t2428); }); wv t2430 = rt_idx(v1197_j, v1195_out); rt_gt(t2429, t2430); }), "&&")))) {
      wv v1198_swap = rt_bind(rt_idx(v1197_j, v1195_out), 0);
      ({ wv t2433 = ({ wv t2431 = rt_sub(v1197_j, rt_i(1)); wv t2432 = v1195_out; rt_idx(t2431, t2432); }); wv t2434 = v1197_j; wv t2435 = v1195_out; rt_set_idx(t2433, t2434, t2435); });
      ({ wv t2436 = v1198_swap; wv t2437 = rt_sub(v1197_j, rt_i(1)); wv t2438 = v1195_out; rt_set_idx(t2436, t2437, t2438); });
      v1197_j = rt_sub(v1197_j, rt_i(1));
    }
    v1196_k = rt_add(v1196_k, rt_i(1));
  }
  RT_RET(v1195_out);
}

static wv w8_dumpValue(wv v1199_x, wv v1200_indent) {
  rt_enter("dumpValue");
  v1199_x = rt_bind(v1199_x, 0);
  v1200_indent = rt_bind(v1200_indent, 0);
  wv v1201_type = rt_bind(std_typeOf(v1199_x), 0);
  if (rt_cond(rt_eq(v1201_type, rt_s(&S194)))) {
    RT_RET(rt_chk(w1_quote(v1199_x), "dumpValue"));
  }
  wv v1202_inner = rt_add(v1200_indent, rt_s(&S478));
  if (rt_cond(rt_eq(v1201_type, rt_s(&S657)))) {
    if (rt_cond(rt_eq(std_length(v1199_x), rt_i(0)))) {
      RT_RET(rt_s(&S658));
    }
    wv v1203_out = rt_s(&S91);
    wv v1204_k = rt_i(0);
    while (rt_cond(({ wv t2439 = v1204_k; wv t2440 = std_length(v1199_x); rt_lt(t2439, t2440); }))) {
      v1203_out = ({ wv t2445 = ({ wv t2441 = rt_add(v1203_out, rt_s(&S2)); wv t2442 = v1202_inner; rt_add(t2441, t2442); }); wv t2446 = ({ wv t2443 = rt_idx(v1204_k, v1199_x); wv t2444 = v1202_inner; rt_chk(w8_dumpValue(t2443, t2444), "dumpValue"); }); rt_add(t2445, t2446); });
      v1204_k = rt_add(v1204_k, rt_i(1));
    }
    RT_RET(rt_add(({ wv t2447 = rt_add(v1203_out, rt_s(&S2)); wv t2448 = v1200_indent; rt_add(t2447, t2448); }), rt_s(&S92)));
  }
  if (rt_cond(rt_eq(v1201_type, rt_s(&S659)))) {
    wv v1205_keys = rt_bind(rt_chk(w8_sorted(std_fields(v1199_x)), "dumpValue"), 0);
    wv v1206_out = rt_s(&S93);
    wv v1207_k = rt_i(0);
    while (rt_cond(({ wv t2449 = v1207_k; wv t2450 = std_length(v1205_keys); rt_lt(t2449, t2450); }))) {
      wv v1208_key = rt_bind(rt_idx(v1207_k, v1205_keys), 0);
      v1206_out = ({ wv t2457 = ({ wv t2453 = v1206_out; wv t2454 = ({ wv t2451 = rt_text(v1202_inner); wv t2452 = rt_text(v1208_key); rt_cat(4, (wv[]){ rt_s(&S2), t2451, t2452, rt_s(&S29) }); }); rt_add(t2453, t2454); }); wv t2458 = ({ wv t2455 = rt_idx(v1208_key, v1199_x); wv t2456 = v1202_inner; rt_chk(w8_dumpValue(t2455, t2456), "dumpValue"); }); rt_add(t2457, t2458); });
      v1207_k = rt_add(v1207_k, rt_i(1));
    }
    RT_RET(({ wv t2459 = v1206_out; wv t2460 = rt_cat(3, (wv[]){ rt_s(&S2), rt_text(v1200_indent), rt_s(&S15) }); rt_add(t2459, t2460); }));
  }
  RT_RET(std_toPurple(v1199_x));
}

static wv w9_dirOf(wv v1209_path) {
  rt_enter("dirOf");
  v1209_path = rt_bind(v1209_path, 0);
  wv v1210_cs = rt_bind(std_split(v1209_path, rt_s(&S13)), 0);
  wv v1211_k = rt_sub(std_length(v1210_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_ge(v1211_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1211_k, v1210_cs), rt_s(&S74)), "&&")))) {
    v1211_k = rt_sub(v1211_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2461 = v1210_cs; wv t2462 = rt_add(v1211_k, rt_i(1)); std_slice(t2461, rt_i(0), t2462); }), rt_s(&S13)));
}

static wv w9_normalize(wv v1212_path) {
  rt_enter("normalize");
  v1212_path = rt_bind(v1212_path, 0);
  wv v1213_segs = rt_bind(std_split(v1212_path, rt_s(&S74)), 0);
  wv v1214_out = rt_arr(0, NULL, 1);
  wv v1215_k = rt_i(0);
  while (rt_cond(({ wv t2463 = v1215_k; wv t2464 = std_length(v1213_segs); rt_lt(t2463, t2464); }))) {
    wv v1216_seg = rt_bind(rt_idx(v1215_k, v1213_segs), 0);
    v1215_k = rt_add(v1215_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(v1216_seg, rt_s(&S107)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1216_seg, rt_s(&S13)), "&&") && rt_bool(rt_gt(std_length(v1214_out), rt_i(0)), "&&")), "||")))) {
      continue;
    }
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1216_seg, rt_s(&S660)), "&&") && rt_bool(rt_gt(std_length(v1214_out), rt_i(0)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1214_out), rt_s(&S660)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1214_out), rt_s(&S13)), "&&")))) {
      (void)std_pop(v1214_out);
    } else {
      (void)std_push(v1214_out, v1216_seg);
    }
  }
  RT_RET(std_join(v1214_out, rt_s(&S74)));
}

static wv w9_with(wv v1217_xs, wv v1218_x) {
  rt_enter("with");
  v1217_xs = rt_bind(v1217_xs, 0);
  v1218_x = rt_bind(v1218_x, 0);
  wv v1219_out = rt_arr(0, NULL, 1);
  wv v1220_k = rt_i(0);
  while (rt_cond(({ wv t2465 = v1220_k; wv t2466 = std_length(v1217_xs); rt_lt(t2465, t2466); }))) {
    (void)({ wv t2467 = v1219_out; wv t2468 = rt_idx(v1220_k, v1217_xs); std_push(t2467, t2468); });
    v1220_k = rt_add(v1220_k, rt_i(1));
  }
  (void)std_push(v1219_out, v1218_x);
  RT_RET(v1219_out);
}

static wv w9_failIn(wv v1221_file, wv v1222_chars, wv v1223_pos, wv v1224_message) {
  rt_enter("failIn");
  v1221_file = rt_bind(v1221_file, 0);
  v1222_chars = rt_bind(v1222_chars, 0);
  v1223_pos = rt_bind(v1223_pos, 0);
  v1224_message = rt_bind(v1224_message, 0);
  (void)rt_chk(w2_setSource(v1221_file, v1222_chars), "failIn");
  (void)rt_chk(w2_fail(v1223_pos, v1224_message), "failIn");
  RT_RET(RT_NULL);
}

static wv w9_loadProgram(wv v1225_entryFile, wv v1226_entryChars) {
  rt_enter("loadProgram");
  struct fr254 fr = { 0 };
  v1225_entryFile = rt_bind(v1225_entryFile, 0);
  fr.v1227_modules = rt_arr(0, NULL, 1);
  wv v1230_path = rt_bind(rt_chk(w9_normalize(std_join(std_split(v1225_entryFile, rt_s(&S19)), rt_s(&S74))), "loadProgram"), 0);
  (void)({ wv t2519 = v1225_entryFile; wv t2520 = v1226_entryChars; wv t2521 = v1230_path; wv t2522 = rt_arr(1, (wv[]){ v1230_path }, 0); rt_chk(s1229_visit(&fr, t2519, t2520, t2521, t2522), "loadProgram"); });
  RT_RET(fr.v1227_modules);
}

static wv s1228_loaded(struct fr254 *up, wv v1231_path) {
  rt_enter("loaded");
  v1231_path = rt_bind(v1231_path, 0);
  wv v1232_k = rt_i(0);
  while (rt_cond(({ wv t2469 = v1232_k; wv t2470 = std_length(rt_def(up->v1227_modules, "modules")); rt_lt(t2469, t2470); }))) {
    if (rt_cond(({ wv t2473 = rt_get(&F_path, ({ wv t2471 = v1232_k; wv t2472 = rt_def(up->v1227_modules, "modules"); rt_idx(t2471, t2472); })); wv t2474 = v1231_path; rt_eq(t2473, t2474); }))) {
      RT_RET(v1232_k);
    }
    v1232_k = rt_add(v1232_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s1229_visit(struct fr254 *up, wv v1233_file, wv v1234_chars, wv v1235_path, wv v1236_chain) {
  rt_enter("visit");
  v1233_file = rt_bind(v1233_file, 0);
  v1234_chars = rt_bind(v1234_chars, 0);
  v1235_path = rt_bind(v1235_path, 0);
  v1236_chain = rt_bind(v1236_chain, 0);
  (void)rt_chk(w2_setSource(v1233_file, v1234_chars), "visit");
  wv v1237_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(v1234_chars), "visit")), "visit"), 0);
  wv v1238_steals = rt_arr(0, NULL, 1);
  wv v1239_k = rt_i(0);
  while (rt_cond(({ wv t2475 = v1239_k; wv t2476 = std_length(rt_get(&F_stmts, v1237_program)); rt_lt(t2475, t2476); }))) {
    wv v1242_s = rt_bind(({ wv t2477 = v1239_k; wv t2478 = rt_get(&F_stmts, v1237_program); rt_idx(t2477, t2478); }), 0);
    v1239_k = rt_add(v1239_k, rt_i(1));
    if (rt_cond(rt_ne(rt_get(&F_k, v1242_s), rt_s(&S54)))) {
      continue;
    }
    wv v1243_libPath = rt_bind(rt_chk(w9_normalize(({ wv t2479 = rt_chk(w9_dirOf(v1235_path), "visit"); wv t2480 = rt_get(&F_path, v1242_s); rt_add(t2479, t2480); })), "visit"), 0);
    wv v1244_at = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t2481 = v1244_at; wv t2482 = std_length(v1236_chain); rt_lt(t2481, t2482); }), "&&") && rt_bool(({ wv t2483 = rt_idx(v1244_at, v1236_chain); wv t2484 = v1243_libPath; rt_ne(t2483, t2484); }), "&&")))) {
      v1244_at = rt_add(v1244_at, rt_i(1));
    }
    if (rt_cond(({ wv t2485 = v1244_at; wv t2486 = std_length(v1236_chain); rt_lt(t2485, t2486); }))) {
      wv v1246_cycle = rt_bind(({ wv t2490 = ({ wv t2487 = v1236_chain; wv t2488 = v1244_at; wv t2489 = std_length(v1236_chain); std_slice(t2487, t2488, t2489); }); wv t2491 = v1243_libPath; rt_chk(w9_with(t2490, t2491), "visit"); }), 0);
      (void)({ wv t2492 = v1233_file; wv t2493 = v1234_chars; wv t2494 = rt_get(&F_pathPos, v1242_s); wv t2495 = rt_cat(2, (wv[]){ rt_s(&S661), rt_text(std_join(v1246_cycle, rt_s(&S662))) }); rt_chk(w9_failIn(t2492, t2493, t2494, t2495), "visit"); });
    }
    wv v1245_lib = rt_bind(rt_chk(s1228_loaded(up, v1243_libPath), "visit"), 1);
    if (rt_cond(rt_eq(v1245_lib, RT_NULL))) {
      wv t2496 = std_readFile(v1243_libPath);
      if (rt_sigcode) {
        rt_sigcode = 0;
        {
          (void)({ wv t2497 = v1233_file; wv t2498 = v1234_chars; wv t2499 = rt_get(&F_pathPos, v1242_s); wv t2500 = rt_cat(3, (wv[]){ rt_s(&S663), rt_text(v1243_libPath), rt_s(&S111) }); rt_chk(w9_failIn(t2497, t2498, t2499, t2500), "visit"); });
          t2496 = RT_NULL;
        }
      }
      wv v1247_text = rt_bind(t2496, 0);
      wv v1248_cs = rt_bind(std_split(v1247_text, rt_s(&S13)), 0);
      wv v1249_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1248_cs), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1248_cs), rt_s(&S664)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
      v1245_lib = rt_bind(({ wv t2504 = v1243_libPath; wv t2505 = ({ wv t2501 = v1248_cs; wv t2502 = v1249_bom; wv t2503 = std_length(v1248_cs); std_slice(t2501, t2502, t2503); }); wv t2506 = v1243_libPath; wv t2507 = rt_chk(w9_with(v1236_chain, v1243_libPath), "visit"); rt_chk(s1229_visit(up, t2504, t2505, t2506, t2507), "visit"); }), 1);
    }
    (void)({ wv t2508 = v1238_steals; wv t2509 = rt_rec(0, 2, K59, (wv[]){ v1242_s, v1245_lib }); std_push(t2508, t2509); });
  }
  wv v1240_fickleTargets = rt_arr(0, NULL, 1);
  wv v1241_mod = ({ wv t2510 = v1233_file; wv t2511 = v1234_chars; wv t2512 = v1235_path; wv t2513 = v1237_program; wv t2514 = v1238_steals; wv t2515 = rt_arr(0, NULL, 0); wv t2516 = v1240_fickleTargets; rt_rec(1, 8, K60, (wv[]){ t2510, t2511, t2512, t2513, t2514, t2515, rt_b(0), t2516 }); });
  (void)({ wv t2517 = rt_def(up->v1227_modules, "modules"); wv t2518 = v1241_mod; std_push(t2517, t2518); });
  RT_RET(rt_sub(std_length(rt_def(up->v1227_modules, "modules")), rt_i(1)));
}

static wv w_die(void) {
  rt_enter("die");
  (void)std_eprint(rt_def(w_usage, "usage"));
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w_load(wv v1250_file) {
  rt_enter("load");
  v1250_file = rt_bind(v1250_file, 0);
  wv t2523 = std_readFile(v1250_file);
  if (rt_sigcode) {
    rt_sigcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S671), rt_text(v1250_file) }));
      rt_exit(rt_i(2));
      t2523 = RT_NULL;
    }
  }
  wv v1251_text = rt_bind(t2523, 0);
  wv v1252_chars = rt_bind(std_split(v1251_text, rt_s(&S13)), 0);
  wv v1253_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1252_chars), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1252_chars), rt_s(&S664)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
  (void)({ wv t2527 = v1250_file; wv t2528 = ({ wv t2524 = v1252_chars; wv t2525 = v1253_bom; wv t2526 = std_length(v1252_chars); std_slice(t2524, t2525, t2526); }); rt_chk(w2_setSource(t2527, t2528), "load"); });
  RT_RET(RT_NULL);
}

static wv w_onWindows(void) {
  rt_enter("onWindows");
  wv t2533 = rt_s(&S681);
  wv t2534 = std_env(t2533);
  if (rt_sigcode) {
    rt_sigcode = 0;
    {
      t2534 = rt_s(&S13);
    }
  }
  wv v1255_os = rt_bind(t2534, 0);
  RT_RET(rt_eq(v1255_os, rt_s(&S682)));
}

static wv w_tryRead(wv v1256_path) {
  rt_enter("tryRead");
  v1256_path = rt_bind(v1256_path, 0);
  wv t2541 = std_readFile(v1256_path);
  if (rt_sigcode) {
    rt_sigcode = 0;
    {
      t2541 = RT_NULL;
    }
  }
  wv v1257_text = rt_bind(t2541, 0);
  RT_RET(v1257_text);
}

static wv w_programDir(void) {
  rt_enter("programDir");
  wv v1258_cs = rt_bind(std_split(std_programPath(), rt_s(&S13)), 0);
  wv v1259_k = rt_sub(std_length(v1258_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v1259_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1259_k, v1258_cs), rt_s(&S74)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(v1259_k, v1258_cs), rt_s(&S19)), "&&")))) {
    v1259_k = rt_sub(v1259_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2542 = v1258_cs; wv t2543 = rt_add(v1259_k, rt_i(1)); std_slice(t2542, rt_i(0), t2543); }), rt_s(&S13)));
}

static wv w_readRuntime(void) {
  rt_enter("readRuntime");
  wv v1260_text = rt_bind(rt_chk(w_tryRead(rt_def(w_runtimePath, "runtimePath")), "readRuntime"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1260_text, RT_NULL), "||") || rt_bool(rt_def(w_runtimeGiven, "runtimeGiven"), "||")))) {
    RT_RET(v1260_text);
  }
  wv v1261_dir = rt_bind(rt_chk(w_programDir(), "readRuntime"), 0);
  wv v1262_here = rt_bind(rt_chk(w_tryRead(({ wv t2544 = v1261_dir; wv t2545 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2544, t2545); })), "readRuntime"), 0);
  RT_RET((rt_cond(rt_ne(v1262_here, RT_NULL)) ? v1262_here : rt_chk(w_tryRead(({ wv t2546 = rt_add(v1261_dir, rt_s(&S690)); wv t2547 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2546, t2547); })), "readRuntime")));
}

static void wah_main(void) {
  rt_syms(89, (wstr *const[]){ &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars, &F_word, &F_kind, &F_error, &F_t, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts, &F_tokens, &F_k, &F_e, &F_handler, &F_items, &F_fields, &F_index, &F_arr, &F_rec, &F_l, &F_r, &F_c, &F_a, &F_b, &F_stmts, &F_param, &F_paramPos, &F_body, &F_neg, &F_cond, &F_code, &F_op, &F_target, &F_paren, &F_isShifty, &F_isFickle, &F_name, &F_namePos, &F_path, &F_pathPos, &F_names, &F_params, &F_members, &F_value, &F_close, &F_callee, &F_args, &F_field, &F_arg, &F_values, &F_subject, &F_alibis, &F_els, &F_branches, &F_arity, &F_mod, &F_parent, &F_visibleAt, &F_fnId, &F_lib, &F_id, &F_fn, &F_top, &F_loops, &F_key, &F_exports, &F_fickleTargets, &F_useMod, &F_steals, &F_node, &F_notLibrary, &F_program, &F_js, &F_children, &F_d, &F_depth, &F_cname, &F_captured, &F_schemeFn, &F_pre, &F_cs, &F_proto, &F_lines });
  w2_srcFile = rt_s(&S13);
  w2_srcChars = rt_arr(0, NULL, 0);
  w2_warnings = rt_arr(0, NULL, 1);
  w3_keywords = rt_arr(23, (wv[]){ rt_s(&S33), rt_s(&S34), rt_s(&S35), rt_s(&S36), rt_s(&S37), rt_s(&S38), rt_s(&S39), rt_s(&S40), rt_s(&S41), rt_s(&S42), rt_s(&S43), rt_s(&S44), rt_s(&S45), rt_s(&S46), rt_s(&S47), rt_s(&S48), rt_s(&S49), rt_s(&S50), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S54), rt_s(&S55) }, 0);
  w3_reserved = ({ wv t68 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S56), rt_s(&S57) }); wv t69 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S58), rt_s(&S59) }); wv t70 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S60), rt_s(&S61) }); wv t71 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S62), rt_s(&S63) }); wv t72 = rt_rec(0, 2, K1, (wv[]){ rt_s(&S64), rt_s(&S65) }); rt_arr(5, (wv[]){ t68, t69, t70, t71, t72 }, 0); });
  w3_bangWords = rt_arr(3, (wv[]){ rt_s(&S39), rt_s(&S41), rt_s(&S64) }, 0);
  w3_cheatOps = rt_arr(13, (wv[]){ rt_s(&S66), rt_s(&S67), rt_s(&S68), rt_s(&S69), rt_s(&S70), rt_s(&S71), rt_s(&S72), rt_s(&S73), rt_s(&S74), rt_s(&S75), rt_s(&S76), rt_s(&S77), rt_s(&S32) }, 0);
  w3_ops = rt_arr(31, (wv[]){ rt_s(&S66), rt_s(&S78), rt_s(&S79), rt_s(&S80), rt_s(&S81), rt_s(&S82), rt_s(&S83), rt_s(&S67), rt_s(&S68), rt_s(&S71), rt_s(&S72), rt_s(&S73), rt_s(&S74), rt_s(&S75), rt_s(&S84), rt_s(&S85), rt_s(&S76), rt_s(&S77), rt_s(&S32), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S28), rt_s(&S14), rt_s(&S89), rt_s(&S90), rt_s(&S16), rt_s(&S91), rt_s(&S92), rt_s(&S93), rt_s(&S15) }, 0);
  w4_exprStart = rt_arr(20, (wv[]){ rt_s(&S106), rt_s(&S109), rt_s(&S179), rt_s(&S46), rt_s(&S47), rt_s(&S48), rt_s(&S49), rt_s(&S50), rt_s(&S149), rt_s(&S164), rt_s(&S90), rt_s(&S91), rt_s(&S93), rt_s(&S39), rt_s(&S183), rt_s(&S51), rt_s(&S72), rt_s(&S87), rt_s(&S86), rt_s(&S89) }, 0);
  w4_whineOk = rt_arr(6, (wv[]){ rt_s(&S184), rt_s(&S35), rt_s(&S38), rt_s(&S34), rt_s(&S44), rt_s(&S185) }, 0);
  w4_callees = rt_arr(3, (wv[]){ rt_s(&S149), rt_s(&S164), rt_s(&S162) }, 0);
  w4_postfixHint = rt_s(&S186);
  w5_stdlib = ({ wv t543 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S329), rt_i(1) }); wv t544 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S330), rt_i(1) }); wv t545 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S331), rt_i(1) }); wv t546 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S332), rt_i(0) }); wv t547 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S333), rt_i(1) }); wv t548 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S334), rt_i(2) }); wv t549 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S335), rt_i(2) }); wv t550 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S336), rt_i(1) }); wv t551 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S337), rt_i(1) }); wv t552 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S338), rt_i(1) }); wv t553 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S339), rt_s(&S340) }); wv t554 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S341), rt_i(1) }); wv t555 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S342), rt_s(&S340) }); wv t556 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S343), rt_i(2) }); wv t557 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S344), rt_i(1) }); wv t558 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S345), rt_i(2) }); wv t559 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S346), rt_i(3) }); wv t560 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S347), rt_i(1) }); wv t561 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S348), rt_i(1) }); wv t562 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S349), rt_i(2) }); wv t563 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S350), rt_i(2) }); wv t564 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S351), rt_i(1) }); wv t565 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S352), rt_i(1) }); wv t566 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S353), rt_i(1) }); wv t567 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S354), rt_i(1) }); wv t568 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S355), rt_i(2) }); wv t569 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S356), rt_i(1) }); wv t570 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S357), rt_i(2) }); wv t571 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S358), rt_i(1) }); wv t572 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S359), rt_i(1) }); wv t573 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S360), rt_i(1) }); wv t574 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S361), rt_i(1) }); wv t575 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S362), rt_i(1) }); wv t576 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S363), rt_i(1) }); wv t577 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S364), rt_i(1) }); wv t578 = rt_rec(0, 2, K40, (wv[]){ rt_s(&S365), rt_i(1) }); rt_arr(36, (wv[]){ t543, t544, t545, t546, t547, t548, t549, t550, t551, t552, t553, t554, t555, t556, t557, t558, t559, t560, t561, t562, t563, t564, t565, t566, t567, t568, t569, t570, t571, t572, t573, t574, t575, t576, t577, t578 }, 0); });
  w5_neverCompletes = rt_arr(6, (wv[]){ rt_s(&S34), rt_s(&S44), rt_s(&S185), rt_s(&S211), rt_s(&S42), rt_s(&S43) }, 0);
  w5_whineValueUsed = rt_arr(5, (wv[]){ rt_s(&S35), rt_s(&S38), rt_s(&S34), rt_s(&S44), rt_s(&S185) }, 0);
  w5_libraryTop = rt_arr(6, (wv[]){ rt_s(&S207), rt_s(&S54), rt_s(&S33), rt_s(&S35), rt_s(&S53), rt_s(&S55) }, 0);
  w6_binOps = rt_arr(17, (wv[]){ rt_s(&S71), rt_s(&S72), rt_s(&S73), rt_s(&S74), rt_s(&S75), rt_s(&S66), rt_s(&S78), rt_s(&S79), rt_s(&S84), rt_s(&S85), rt_s(&S80), rt_s(&S81), rt_s(&S76), rt_s(&S77), rt_s(&S32), rt_s(&S67), rt_s(&S68) }, 0);
  w6_binFns = rt_arr(17, (wv[]){ rt_s(&S455), rt_s(&S456), rt_s(&S457), rt_s(&S458), rt_s(&S459), rt_s(&S460), rt_s(&S461), rt_s(&S462), rt_s(&S463), rt_s(&S464), rt_s(&S465), rt_s(&S466), rt_s(&S467), rt_s(&S468), rt_s(&S469), rt_s(&S470), rt_s(&S471) }, 0);
  w6_scalarKinds = rt_arr(9, (wv[]){ rt_s(&S106), rt_s(&S109), rt_s(&S179), rt_s(&S291), rt_s(&S292), rt_s(&S293), rt_s(&S294), rt_s(&S200), rt_s(&S199) }, 0);
  w6_plainKinds = rt_arr(8, (wv[]){ rt_s(&S106), rt_s(&S109), rt_s(&S291), rt_s(&S292), rt_s(&S293), rt_s(&S294), rt_s(&S264), rt_s(&S164) }, 0);
  w7_binOps = rt_arr(17, (wv[]){ rt_s(&S71), rt_s(&S72), rt_s(&S73), rt_s(&S74), rt_s(&S75), rt_s(&S66), rt_s(&S78), rt_s(&S79), rt_s(&S84), rt_s(&S85), rt_s(&S80), rt_s(&S81), rt_s(&S76), rt_s(&S77), rt_s(&S32), rt_s(&S67), rt_s(&S68) }, 0);
  w7_binFns = rt_arr(17, (wv[]){ rt_s(&S455), rt_s(&S456), rt_s(&S457), rt_s(&S458), rt_s(&S459), rt_s(&S460), rt_s(&S461), rt_s(&S462), rt_s(&S463), rt_s(&S464), rt_s(&S465), rt_s(&S466), rt_s(&S467), rt_s(&S468), rt_s(&S469), rt_s(&S470), rt_s(&S471) }, 0);
  w7_scalarKinds = rt_arr(9, (wv[]){ rt_s(&S106), rt_s(&S109), rt_s(&S179), rt_s(&S291), rt_s(&S292), rt_s(&S293), rt_s(&S294), rt_s(&S200), rt_s(&S199) }, 0);
  w7_plainKinds = rt_arr(8, (wv[]){ rt_s(&S106), rt_s(&S109), rt_s(&S291), rt_s(&S292), rt_s(&S293), rt_s(&S294), rt_s(&S264), rt_s(&S164) }, 0);
  w7_signalers = rt_arr(10, (wv[]){ rt_s(&S332), rt_s(&S333), rt_s(&S334), rt_s(&S335), rt_s(&S337), rt_s(&S338), rt_s(&S341), rt_s(&S343), rt_s(&S353), rt_s(&S354) }, 0);
  w7_constK = rt_i(0);
  w7_pureK = rt_i(1);
  w7_effectK = rt_i(2);
  w_usage = rt_add(rt_add(rt_add(rt_add(rt_add(rt_s(&S665), rt_s(&S666)), rt_s(&S667)), rt_s(&S668)), rt_s(&S669)), rt_s(&S670));
  w_argv = rt_bind(std_args(), 0);
  w_mode = rt_s(&S13);
  w_input = rt_s(&S13);
  w_output = rt_s(&S13);
  w_target = rt_s(&S13);
  w_runtimePath = rt_s(&S13);
  w_cc = rt_s(&S13);
  w_arg = rt_i(0);
  while (rt_cond(({ wv t2529 = w_arg; wv t2530 = std_length(w_argv); rt_lt(t2529, t2530); }))) {
    v1254_a = rt_bind(rt_idx(w_arg, w_argv), 0);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_arg, rt_i(0)), "&&") && rt_bool(rt_eq(v1254_a, rt_s(&S672)), "&&")))) {
      w_mode = rt_bind(v1254_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v1254_a, rt_s(&S673)), "||") || rt_bool(rt_eq(v1254_a, rt_s(&S674)), "||")))) {
      w_mode = rt_bind(v1254_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1254_a, rt_s(&S675)), "||") || rt_bool(rt_eq(v1254_a, rt_s(&S676)), "||")), "||") || rt_bool(rt_eq(v1254_a, rt_s(&S677)), "||")), "||") || rt_bool(rt_eq(v1254_a, rt_s(&S678)), "||")))) {
      w_arg = rt_add(w_arg, rt_i(1));
      if (rt_cond(({ wv t2531 = w_arg; wv t2532 = std_length(w_argv); rt_ge(t2531, t2532); }))) {
        (void)rt_chk(w_die(), NULL);
      }
      if (rt_cond(rt_eq(v1254_a, rt_s(&S675)))) {
        w_output = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1254_a, rt_s(&S676)))) {
        w_runtimePath = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1254_a, rt_s(&S678)))) {
        w_cc = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_idx(w_arg, w_argv), rt_s(&S679)), "||") || rt_bool(rt_eq(rt_idx(w_arg, w_argv), rt_s(&S680)), "||")))) {
        w_target = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else {
        (void)rt_chk(w_die(), NULL);
      }
    } else if (rt_cond(rt_b(rt_bool(rt_ne(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(rt_gt(std_length(v1254_a), rt_i(0)), "&&") && rt_bool(rt_eq(std_at(v1254_a, rt_i(0)), rt_s(&S72)), "&&")), "||")))) {
      (void)rt_chk(w_die(), NULL);
    } else {
      w_input = rt_bind(v1254_a, 1);
    }
    w_arg = rt_add(w_arg, rt_i(1));
  }
  w_inspect = rt_b(rt_bool(rt_eq(w_mode, rt_s(&S673)), "||") || rt_bool(rt_eq(w_mode, rt_s(&S674)), "||"));
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(w_inspect, "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "||") || rt_bool(rt_ne(w_target, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_eq(w_mode, rt_s(&S672)), "&&") && rt_bool(rt_ne(w_target, rt_s(&S13)), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S672)), "&&") && rt_bool(rt_ne(w_cc, rt_s(&S13)), "&&")), "||")))) {
    (void)rt_chk(w_die(), NULL);
  }
  w_dotWah = rt_b(rt_bool(rt_ge(std_length(w_input), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t2535 = w_input; wv t2536 = std_length(w_input); std_slice(t2535, rt_i(-4), t2536); }), rt_s(&S232)), "&&"));
  if (rt_cond(rt_eq(w_mode, rt_s(&S672)))) {
    w_target = rt_s(&S680);
    if (rt_cond(rt_eq(w_cc, rt_s(&S13)))) {
      wv t2537 = rt_s(&S683);
      wv t2538 = std_env(t2537);
      if (rt_sigcode) {
        rt_sigcode = 0;
        {
          t2538 = rt_s(&S684);
        }
      }
      w_cc = rt_bind(t2538, 1);
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(w_output, rt_s(&S13)), "&&") && rt_bool(rt_not(w_dotWah), "&&")))) {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S685), rt_text(w_input), rt_s(&S686) }));
      rt_exit(rt_i(2));
    }
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = ({ wv t2539 = std_slice(w_input, rt_i(0), rt_i(-4)); wv t2540 = (rt_cond(rt_chk(w_onWindows(), NULL)) ? rt_s(&S687) : rt_s(&S13)); rt_add(t2539, t2540); });
    }
  }
  if (rt_cond(rt_eq(w_target, rt_s(&S13)))) {
    w_target = rt_s(&S679);
  }
  w_runtimeGiven = rt_ne(w_runtimePath, rt_s(&S13));
  if (rt_cond(rt_not(w_runtimeGiven))) {
    w_runtimePath = rt_bind((rt_cond(rt_eq(w_target, rt_s(&S680))) ? rt_s(&S688) : rt_s(&S689)), 1);
  }
  (void)rt_chk(w_load(w_input), NULL);
  if (rt_cond(rt_eq(w_mode, rt_s(&S673)))) {
    v1263_tokens = rt_bind(rt_chk(w3_lex(w2_srcChars), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)rt_chk(w8_dumpTokens(v1263_tokens, rt_s(&S13)), NULL);
    rt_exit(rt_i(0));
  }
  if (rt_cond(rt_eq(w_mode, rt_s(&S674)))) {
    v1264_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(w2_srcChars), NULL)), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)std_print(rt_chk(w8_dumpValue(v1264_program, rt_s(&S13)), NULL));
    rt_exit(rt_i(0));
  }
  w_modules = rt_bind(rt_chk(w9_loadProgram(w2_srcFile, w2_srcChars), NULL), 0);
  w_problems = rt_bind(rt_chk(w5_check(w_modules, rt_b(1)), NULL), 0);
  if (rt_cond(rt_gt(std_length(w_problems), rt_i(0)))) {
    (void)rt_chk(w2_flushWarnings(), NULL);
    v1265_p = rt_i(0);
    while (rt_cond(({ wv t2548 = v1265_p; wv t2549 = std_length(w_problems); rt_lt(t2548, t2549); }))) {
      (void)std_eprint(rt_chk(w2_formatDiag(rt_idx(v1265_p, w_problems)), NULL));
      v1265_p = rt_add(v1265_p, rt_i(1));
    }
    rt_exit(rt_i(2));
  }
  w_runtime = rt_bind(rt_chk(w_readRuntime(), NULL), 0);
  if (rt_cond(rt_eq(w_runtime, RT_NULL))) {
    (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S671), rt_text(w_runtimePath) }));
    rt_exit(rt_i(2));
  }
  w_code = rt_bind((rt_cond(rt_eq(w_target, rt_s(&S680))) ? rt_chk(w7_generateC(w_modules, w_runtime), NULL) : rt_chk(w6_generate(w_modules, w_runtime), NULL)), 0);
  (void)rt_chk(w2_flushWarnings(), NULL);
  if (rt_cond(rt_ne(w_mode, rt_s(&S672)))) {
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = ({ wv t2550 = (rt_cond(w_dotWah) ? std_slice(w_input, rt_i(0), rt_i(-4)) : w_input); wv t2551 = rt_cat(2, (wv[]){ rt_s(&S107), rt_text(w_target) }); rt_add(t2550, t2551); });
    }
    wv t2552 = std_writeFile(w_output, w_code);
    if (rt_sigcode) {
      rt_sigcode = 0;
      {
        (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S691), rt_text(w_output) }));
        rt_exit(rt_i(2));
        t2552 = RT_NULL;
      }
    }
    (void)t2552;
    rt_exit(rt_i(0));
  }
  w_dotExe = rt_b(rt_bool(rt_ge(std_length(w_output), rt_i(4)), "&&") && rt_bool(rt_eq(rt_chk(w1_toLower(({ wv t2553 = w_output; wv t2554 = std_length(w_output); std_slice(t2553, rt_i(-4), t2554); })), NULL), rt_s(&S687)), "&&"));
  w_cFile = rt_add((rt_cond(w_dotExe) ? std_slice(w_output, rt_i(0), rt_i(-4)) : w_output), rt_s(&S692));
  wv t2555 = std_writeFile(w_cFile, w_code);
  if (rt_sigcode) {
    rt_sigcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S691), rt_text(w_cFile) }));
      rt_exit(rt_i(2));
      t2555 = RT_NULL;
    }
  }
  (void)t2555;
  w_parts = rt_bind(std_split(std_join(std_split(w_cc, rt_s(&S19)), rt_s(&S74)), rt_s(&S74)), 0);
  w_ccName = rt_bind(rt_chk(w1_toLower(rt_idx(rt_i(-1), w_parts)), NULL), 0);
  w_flags = rt_arr(4, (wv[]){ rt_s(&S693), rt_s(&S675), w_output, w_cFile }, 0);
  wv t2556 = (rt_cond(rt_b(rt_bool(rt_eq(w_ccName, rt_s(&S684)), "||") || rt_bool(rt_eq(w_ccName, rt_s(&S694)), "||"))) ? rt_arr(5, (wv[]){ rt_s(&S695), rt_s(&S693), rt_s(&S675), w_output, w_cFile }, 0) : w_flags);
  wv t2557 = std_run(w_cc, t2556);
  if (rt_sigcode) {
    rt_sigcode = 0;
    {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S696), rt_text(w_cc), rt_s(&S697) }));
      rt_exit(rt_i(2));
      t2557 = RT_NULL;
    }
  }
  w_status = rt_bind(t2557, 0);
  if (rt_cond(rt_ne(w_status, rt_i(0)))) {
    (void)std_eprint(({ wv t2558 = rt_text(w_cc); wv t2559 = rt_text(w_status); rt_cat(4, (wv[]){ rt_s(&S685), t2558, rt_s(&S698), t2559 }); }));
    rt_exit(rt_i(2));
  }
  rt_exit(rt_i(0));
}
