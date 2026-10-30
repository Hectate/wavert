// Wavert native runtime, forked from Γ--'s runtime/wah.c (wah/runtime). This file is inlined
// verbatim at the top of every program compiled with --target c, the
// counterpart of runtime/wvt.js.
// It needs a GNU C compiler (clang or gcc; `zig cc` is clang).
//
// Values: a 16-byte tagged wv. int -> int64_t (checked to ±(2^53-1)),
// float -> double, string -> immutable UTF-8 wstr, array -> warr, record ->
// wrec (keys shared by every record of one literal), enum member -> pointer
// to a static wmem, null -> T_NULL. T_UNSET marks a binding before its
// declaration (temporal dead zone).
//
// Memory is allocated from an arena and never freed.
//
// A fail is a pending code in rt_failcode: a function or stdlib function sets
// it and returns, and the caller either handles it (onfail) or turns it into
// a panic with rt_chk (the one-frame rule, §8.2). A panic ends the program:
// panics cannot be caught yet, so nothing unwinds.

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

// A string. data holds bytes valid UTF-8 and is followed by a NUL.
typedef struct wstr {
  int64_t bytes, cps;
  int32_t ascii, sym; // sym: interned as a record key
  const char *data;
} wstr;
typedef struct warr { int64_t len, cap; int is_mut; struct wv *items; } warr;
typedef struct wrec { int64_t n; int is_mut; const wstr *const *keys; struct wv *vals; } wrec;
typedef struct wmem { const wstr *enum_name, *name; } wmem;
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
#define RT_NAN rt_f(NAN)
#define RT_INF rt_f(INFINITY)
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
// Copy happens only when mutability changes (§10.4, §10.9).
static wv rt_bind(wv v, int is_mut) {
  if (v.tag == T_ARR) return v.a->is_mut == is_mut ? v : arr_copy(v.a, 0, v.a->len, is_mut);
  if (v.tag == T_REC) return v.r->is_mut == is_mut ? v : rec_copy(v.r, is_mut);
  return v;
}

static int64_t slot(wv i, wv arr) {
  if (arr.tag != T_ARR) {
    rt_fail("type error: indexing needs an array or a record, got %s%s", type_name(arr),
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

// Indexing a record is computed field access with a string key (§10.8).
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
static wv std_readLine(void) {
  wv line;
  return read_line(&line) ? line : rt_std_fail("$readLine");
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
static wv std_run(wv program, wv args) {
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

static wv std_length(wv x) {
  if (x.tag == T_STR) return rt_i(x.s->cps);
  if (x.tag == T_ARR) return rt_i(x.a->len);
  rt_fail("type error: $length expects a string or an array, got %s", type_name(x));
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
static wv std_typeOf(wv x) { return x.tag == T_MEM ? rt_s(x.m->enum_name) : rt_cstr(type_name(x)); }
static wv std_fields(wv r) {
  if (r.tag != T_REC) rt_fail("type error: $fields expects a record, got %s", type_name(r));
  warr *a = arr_new(r.r->n, 0);
  for (int64_t i = 0; i < r.r->n; i++) a->items[i] = rt_s(r.r->keys[i]);
  return (wv){ .tag = T_ARR, .a = a };
}

// ---- startup ----

// The compiled program: library initialization, then the entry file's body.
static void wvt_main(void);

#ifdef _WIN32
static DWORD WINAPI rt_thread(LPVOID arg) {
  (void)arg;
  wvt_main();
  rt_exit_code(0);
  return 0;
}
#else
static void *rt_thread(void *arg) {
  (void)arg;
  wvt_main();
  rt_exit_code(0);
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
static const wstr S187 = { 3, 3, 1, 0, "rec" };
static const wstr S188 = { 3, 3, 1, 0, "idx" };
static const wstr S189 = { 3, 3, 1, 0, "mem" };
static const wstr S190 = { 2, 2, 1, 0, "un" };
static const wstr S191 = { 3, 3, 1, 0, "bin" };
static const wstr S192 = { 4, 4, 1, 0, "tern" };
static const wstr S193 = { 10, 10, 1, 0, "expected '" };
static const wstr S194 = { 8, 8, 1, 0, ", found " };
static const wstr S195 = { 9, 9, 1, 0, "', found " };
static const wstr S196 = { 27, 27, 1, 0, "missing whitespace around '" };
static const wstr S197 = { 79, 79, 1, 0, "calls are postfix: write the arguments first, then the function, e.g. (a, b)(f)" };
static const wstr S198 = { 36, 36, 1, 0, "every Wavert statement begins with ;" };
static const wstr S199 = { 41, 41, 1, 0, "expected ';' to start a statement, found " };
static const wstr S200 = { 5, 5, 1, 0, "empty" };
static const wstr S201 = { 46, 46, 1, 0, "';end' must be the last statement of a library" };
static const wstr S202 = { 47, 47, 1, 0, "'import' must come before every other statement" };
static const wstr S203 = { 3, 3, 1, 0, "end" };
static const wstr S204 = { 59, 59, 1, 0, "'export' is only allowed in a library, which ends with ;end" };
static const wstr S205 = { 62, 62, 1, 0, "'onfail' must follow a statement whose value comes from a call" };
static const wstr S206 = { 33, 33, 1, 0, "expected a name for the fail code" };
static const wstr S207 = { 6, 6, 1, 0, "!while" };
static const wstr S208 = { 36, 36, 1, 0, "'else' without a preceding if or !if" };
static const wstr S209 = { 22, 22, 1, 0, "'case' outside a match" };
static const wstr S210 = { 2, 2, 1, 0, "to" };
static const wstr S211 = { 4, 4, 1, 0, "name" };
static const wstr S212 = { 31, 31, 1, 0, "an assignment starts with 'set'" };
static const wstr S213 = { 15, 15, 1, 0, ";set x to value" };
static const wstr S214 = { 54, 54, 1, 0, "to make it the block's value, remove the ';' before it" };
static const wstr S215 = { 28, 28, 1, 0, "this statement has no effect" };
static const wstr S216 = { 23, 23, 1, 0, "expected a name after '" };
static const wstr S217 = { 14, 14, 1, 0, " name is value" };
static const wstr S218 = { 2, 2, 1, 0, "is" };
static const wstr S219 = { 9, 9, 1, 0, " is value" };
static const wstr S220 = { 76, 76, 1, 0, "expected 'to' or a compound assignment such as '+=' after the target, found " };
static const wstr S221 = { 35, 35, 1, 0, "a fail code must be an integer >= 1" };
static const wstr S222 = { 51, 51, 1, 0, "'import' is only allowed at the top level of a file" };
static const wstr S223 = { 45, 45, 1, 0, "expected a name for the module after 'import'" };
static const wstr S224 = { 26, 26, 1, 0, ";import lib from \"lib.wvt\"" };
static const wstr S225 = { 4, 4, 1, 0, "from" };
static const wstr S226 = { 8, 8, 1, 0, ";import " };
static const wstr S227 = { 7, 7, 1, 0, " from \"" };
static const wstr S228 = { 5, 5, 1, 0, ".wvt\"" };
static const wstr S229 = { 45, 45, 1, 0, "an import path must be a plain string literal" };
static const wstr S230 = { 37, 37, 1, 0, "import paths use '/' as the separator" };
static const wstr S231 = { 4, 4, 1, 0, ".wvt" };
static const wstr S232 = { 42, 42, 1, 0, "an import path includes the .wvt extension" };
static const wstr S233 = { 43, 43, 1, 0, "only top-level declarations can be exported" };
static const wstr S234 = { 70, 70, 1, 0, "only fn, const, var, ref and enum declarations can be exported, found " };
static const wstr S235 = { 52, 52, 1, 0, "';end' is only allowed at the top level of a library" };
static const wstr S236 = { 46, 46, 1, 0, "a library must export at least one declaration" };
static const wstr S237 = { 55, 55, 1, 0, "mark one with 'export', e.g. ;export fn name(x) { ... }" };
static const wstr S238 = { 36, 36, 1, 0, "the name comes before the parameters" };
static const wstr S239 = { 22, 22, 1, 0, ";fn name(a, b) { ... }" };
static const wstr S240 = { 37, 37, 1, 0, "expected the function name after 'fn'" };
static const wstr S241 = { 39, 39, 1, 0, "expected '(' to open the parameter list" };
static const wstr S242 = { 4, 4, 1, 0, ";fn " };
static const wstr S243 = { 14, 14, 1, 0, "(a, b) { ... }" };
static const wstr S244 = { 25, 25, 1, 0, "expected a parameter name" };
static const wstr S245 = { 43, 43, 1, 0, "a parameter is a name, var name or ref name" };
static const wstr S246 = { 40, 40, 1, 0, "expected ')' to close the parameter list" };
static const wstr S247 = { 45, 45, 1, 0, "an enum can only be declared at the top level" };
static const wstr S248 = { 33, 33, 1, 0, "the name comes before the members" };
static const wstr S249 = { 17, 17, 1, 0, ";enum Name (a, b)" };
static const wstr S250 = { 35, 35, 1, 0, "expected the enum name after 'enum'" };
static const wstr S251 = { 36, 36, 1, 0, "expected '(' to open the member list" };
static const wstr S252 = { 6, 6, 1, 0, ";enum " };
static const wstr S253 = { 7, 7, 1, 0, " (a, b)" };
static const wstr S254 = { 33, 33, 1, 0, "an enum needs at least one member" };
static const wstr S255 = { 42, 42, 1, 0, "' is a keyword and cannot be a member name" };
static const wstr S256 = { 30, 30, 1, 0, "expected a member name, found " };
static const wstr S257 = { 18, 18, 1, 0, "duplicate member '" };
static const wstr S258 = { 9, 9, 1, 0, "' in enum" };
static const wstr S259 = { 38, 38, 1, 0, "expected ',' or ')' in the member list" };
static const wstr S260 = { 20, 20, 1, 0, "expected '(' after '" };
static const wstr S261 = { 30, 30, 1, 0, "conditions are parenthesized: " };
static const wstr S262 = { 12, 12, 1, 0, " (x) { ... }" };
static const wstr S263 = { 35, 35, 1, 0, "expected ')' to close the condition" };
static const wstr S264 = { 12, 12, 1, 0, "expected '{'" };
static const wstr S265 = { 32, 32, 1, 0, "unterminated block: expected '}'" };
static const wstr S266 = { 68, 68, 1, 0, "a record as a block's value needs its own braces: { { key: value } }" };
static const wstr S267 = { 75, 75, 1, 0, "an expression without a leading ';' is the block's value and must come last" };
static const wstr S268 = { 44, 44, 1, 0, "expected '}' after the block's value, found " };
static const wstr S269 = { 53, 53, 1, 0, "expected a name, an indexed element or a field to set" };
static const wstr S270 = { 38, 38, 1, 0, "expected ':' in conditional expression" };
static const wstr S271 = { 22, 22, 1, 0, "'-x ** y' is ambiguous" };
static const wstr S272 = { 28, 28, 1, 0, "write -(x ** y) or (-x) ** y" };
static const wstr S273 = { 59, 59, 1, 0, "'[' at the start of a line indexes the expression before it" };
static const wstr S274 = { 71, 71, 1, 0, "if it starts the block's value, wrap the value in parentheses: ([a, b])" };
static const wstr S275 = { 31, 31, 1, 0, "expected ']' to close the index" };
static const wstr S276 = { 39, 39, 1, 0, "string-literal field key: write record." };
static const wstr S277 = { 20, 20, 1, 0, " instead of record[\"" };
static const wstr S278 = { 2, 2, 1, 0, "\"]" };
static const wstr S279 = { 41, 41, 1, 0, "' is a keyword and cannot be a field name" };
static const wstr S280 = { 39, 39, 1, 0, "expected a field name after '.', found " };
static const wstr S281 = { 4, 4, 1, 0, "bool" };
static const wstr S282 = { 17, 17, 1, 0, "empty parentheses" };
static const wstr S283 = { 39, 39, 1, 0, "a call with no arguments is written (f)" };
static const wstr S284 = { 34, 34, 1, 0, "trailing comma in an argument list" };
static const wstr S285 = { 40, 40, 1, 0, "expected ',' or ')' in the argument list" };
static const wstr S286 = { 69, 69, 1, 0, "a parenthesized list must be followed by the function it is passed to" };
static const wstr S287 = { 30, 30, 1, 0, "(a, b)(f) calls f with a and b" };
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
static const wstr S306 = { 38, 38, 1, 0, "expected '{' after the match's subject" };
static const wstr S307 = { 32, 32, 1, 0, "'else' must come last in a match" };
static const wstr S308 = { 31, 31, 1, 0, "a case needs at least one value" };
static const wstr S309 = { 21, 21, 1, 0, "case \"a\", \"b\" { ... }" };
static const wstr S310 = { 27, 27, 1, 0, "a match has only one 'else'" };
static const wstr S311 = { 45, 45, 1, 0, "a match needs at least one case before 'else'" };
static const wstr S312 = { 31, 31, 1, 0, "a match needs at least one case" };
static const wstr S313 = { 30, 30, 1, 0, "match (x) { case \"a\" { ... } }" };
static const wstr S314 = { 41, 41, 1, 0, "expected 'case', 'else' or '}' in a match" };
static const wstr S315 = { 35, 35, 1, 0, "a case value cannot be interpolated" };
static const wstr S316 = { 28, 28, 1, 0, "floats cannot be case values" };
static const wstr S317 = { 58, 58, 1, 0, "float equality makes a poor match; compare with if instead" };
static const wstr S318 = { 48, 48, 1, 0, "a case value must be a literal or an enum member" };
static const wstr S319 = { 67, 67, 1, 0, "case values are integers, strings, true, false, null or Enum.member" };
static const wstr S320 = { 35, 35, 1, 0, "'!if (a != b)' is a double negative" };
static const wstr S321 = { 19, 19, 1, 0, "write 'if (a == b)'" };
static const wstr S322 = { 11, 11, 1, 0, "unexpected " };
static const wstr S323 = { 17, 17, 1, 0, " in interpolation" };
static const wstr S324 = { 5, 5, 1, 0, "print" };
static const wstr S325 = { 5, 5, 1, 0, "write" };
static const wstr S326 = { 6, 6, 1, 0, "eprint" };
static const wstr S327 = { 8, 8, 1, 0, "readLine" };
static const wstr S328 = { 8, 8, 1, 0, "readFile" };
static const wstr S329 = { 9, 9, 1, 0, "writeFile" };
static const wstr S330 = { 10, 10, 1, 0, "appendFile" };
static const wstr S331 = { 10, 10, 1, 0, "fileExists" };
static const wstr S332 = { 7, 7, 1, 0, "listDir" };
static const wstr S333 = { 10, 10, 1, 0, "deleteFile" };
static const wstr S334 = { 4, 4, 1, 0, "args" };
static const wstr S335 = { 3, 3, 1, 0, "env" };
static const wstr S336 = { 11, 11, 1, 0, "programPath" };
static const wstr S337 = { 3, 3, 1, 0, "run" };
static const wstr S338 = { 6, 6, 1, 0, "length" };
static const wstr S339 = { 2, 2, 1, 0, "at" };
static const wstr S340 = { 5, 5, 1, 0, "slice" };
static const wstr S341 = { 4, 4, 1, 0, "code" };
static const wstr S342 = { 8, 8, 1, 0, "fromCode" };
static const wstr S343 = { 5, 5, 1, 0, "split" };
static const wstr S344 = { 4, 4, 1, 0, "join" };
static const wstr S345 = { 8, 8, 1, 0, "toString" };
static const wstr S346 = { 5, 5, 1, 0, "toInt" };
static const wstr S347 = { 7, 7, 1, 0, "toFloat" };
static const wstr S348 = { 4, 4, 1, 0, "push" };
static const wstr S349 = { 3, 3, 1, 0, "pop" };
static const wstr S350 = { 4, 4, 1, 0, "same" };
static const wstr S351 = { 6, 6, 1, 0, "typeOf" };
static const wstr S352 = { 6, 6, 1, 0, "fields" };
static const wstr S353 = { 5, 5, 1, 0, "floor" };
static const wstr S354 = { 4, 4, 1, 0, "ceil" };
static const wstr S355 = { 5, 5, 1, 0, "round" };
static const wstr S356 = { 5, 5, 1, 0, "trunc" };
static const wstr S357 = { 8, 8, 1, 0, "toUint32" };
static const wstr S358 = { 5, 5, 1, 0, "isNan" };
static const wstr S359 = { 9, 9, 1, 0, " function" };
static const wstr S360 = { 6, 6, 1, 0, "n enum" };
static const wstr S361 = { 9, 9, 1, 0, " argument" };
static const wstr S362 = { 1, 1, 1, 0, "s" };
static const wstr S363 = { 4, 4, 1, 0, "str:" };
static const wstr S364 = { 4, 4, 1, 0, "mem:" };
static const wstr S365 = { 55, 55, 1, 0, "this block's value is used, but it does not produce one" };
static const wstr S366 = { 62, 62, 1, 0, "end the block with its value, or with null if that is intended" };
static const wstr S367 = { 42, 42, 1, 0, "' is already declared in this scope (line " };
static const wstr S368 = { 11, 11, 1, 0, "' shadows '" };
static const wstr S369 = { 32, 32, 1, 0, "' from an enclosing scope (line " };
static const wstr S370 = { 45, 45, 1, 0, "Wavert forbids shadowing; choose another name" };
static const wstr S371 = { 14, 14, 1, 0, "unknown name '" };
static const wstr S372 = { 39, 39, 1, 0, "' is used before its declaration (line " };
static const wstr S373 = { 5, 5, 1, 0, "param" };
static const wstr S374 = { 27, 27, 1, 0, "'return' outside a function" };
static const wstr S375 = { 16, 16, 1, 0, "' outside a loop" };
static const wstr S376 = { 56, 56, 1, 0, "'fail' outside a function: there is no caller to fail to" };
static const wstr S377 = { 65, 65, 1, 0, "'onfail' handles a call, but this statement's value is not a call" };
static const wstr S378 = { 87, 87, 1, 0, "the call must be the outermost expression, e.g. ;const n is s(parse) onfail (c) { ... }" };
static const wstr S379 = { 44, 44, 1, 0, "a match cannot mix literals and enum members" };
static const wstr S380 = { 31, 31, 1, 0, "a match cannot mix members of '" };
static const wstr S381 = { 7, 7, 1, 0, "' and '" };
static const wstr S382 = { 21, 21, 1, 0, "duplicate case value " };
static const wstr S383 = { 7, 7, 1, 0, " (line " };
static const wstr S384 = { 22, 22, 1, 0, "match does not handle " };
static const wstr S385 = { 2, 2, 1, 0, ", " };
static const wstr S386 = { 57, 57, 1, 0, "add a case for each, or end the match with 'else { ... }'" };
static const wstr S387 = { 52, 52, 1, 0, "a match used as a value must end with 'else { ... }'" };
static const wstr S388 = { 6, 6, 1, 0, "module" };
static const wstr S389 = { 19, 19, 1, 0, "' does not export '" };
static const wstr S390 = { 17, 17, 1, 0, "' is not a module" };
static const wstr S391 = { 16, 16, 1, 0, "unknown module '" };
static const wstr S392 = { 41, 41, 1, 0, "' is a function; functions are not values" };
static const wstr S393 = { 20, 20, 1, 0, "to call it, write x(" };
static const wstr S394 = { 7, 7, 1, 0, "), or (" };
static const wstr S395 = { 19, 19, 1, 0, ") with no arguments" };
static const wstr S396 = { 25, 25, 1, 0, "' is an enum, not a value" };
static const wstr S397 = { 24, 24, 1, 0, "its members are written " };
static const wstr S398 = { 7, 7, 1, 0, ".member" };
static const wstr S399 = { 54, 54, 1, 0, "case values are literals or enum members (Enum.member)" };
static const wstr S400 = { 16, 16, 1, 0, "' is not an enum" };
static const wstr S401 = { 14, 14, 1, 0, "unknown enum '" };
static const wstr S402 = { 11, 11, 1, 0, "no member '" };
static const wstr S403 = { 11, 11, 1, 0, "' in enum '" };
static const wstr S404 = { 16, 16, 1, 0, "its members are " };
static const wstr S405 = { 66, 66, 1, 0, "a conditional used as a value must end with a plain 'else { ... }'" };
static const wstr S406 = { 12, 12, 1, 0, "cannot set '" };
static const wstr S407 = { 25, 25, 1, 0, "': it is not a var or ref" };
static const wstr S408 = { 7, 7, 1, 0, "a field" };
static const wstr S409 = { 10, 10, 1, 0, "an element" };
static const wstr S410 = { 29, 29, 1, 0, "': enum members are constants" };
static const wstr S411 = { 9, 9, 1, 0, "': only '" };
static const wstr S412 = { 12, 12, 1, 0, "' may set it" };
static const wstr S413 = { 14, 14, 1, 0, "cannot modify " };
static const wstr S414 = { 5, 5, 1, 0, " of '" };
static const wstr S415 = { 18, 18, 1, 0, "': it is not a var" };
static const wstr S416 = { 6, 6, 1, 0, "' is a" };
static const wstr S417 = { 15, 15, 1, 0, ", not a binding" };
static const wstr S418 = { 24, 24, 1, 0, "mark the parameter (ref " };
static const wstr S419 = { 24, 24, 1, 0, ") to rebind it, or (var " };
static const wstr S420 = { 20, 20, 1, 0, ") for a mutable copy" };
static const wstr S421 = { 29, 29, 1, 0, "mark the parameter var: (var " };
static const wstr S422 = { 22, 22, 1, 0, "declare it with ';ref " };
static const wstr S423 = { 32, 32, 1, 0, " is ...' to rebind it, or ';var " };
static const wstr S424 = { 27, 27, 1, 0, " is ...' for a mutable copy" };
static const wstr S425 = { 22, 22, 1, 0, "declare it with ';var " };
static const wstr S426 = { 8, 8, 1, 0, " is ...'" };
static const wstr S427 = { 32, 32, 1, 0, "unknown standard library name '$" };
static const wstr S428 = { 26, 26, 1, 0, "' is a module, not a value" };
static const wstr S429 = { 27, 27, 1, 0, "what it exports is written " };
static const wstr S430 = { 5, 5, 1, 0, ".name" };
static const wstr S431 = { 32, 32, 1, 0, "' is a function; call it with ($" };
static const wstr S432 = { 30, 30, 1, 0, "'fail' is a statement: ;fail c" };
static const wstr S433 = { 24, 24, 1, 0, "checker: unhandled node " };
static const wstr S434 = { 31, 31, 1, 0, "' is a constant, not a function" };
static const wstr S435 = { 8, 8, 1, 0, "' takes " };
static const wstr S436 = { 6, 6, 1, 0, ", got " };
static const wstr S437 = { 18, 18, 1, 0, "unknown function '" };
static const wstr S438 = { 71, 71, 1, 0, ") is a call with no arguments; to use the value, remove the parentheses" };
static const wstr S439 = { 53, 53, 1, 0, "calls are postfix: the function comes last, e.g. x(f)" };
static const wstr S440 = { 19, 19, 1, 0, "' is not a function" };
static const wstr S441 = { 21, 21, 1, 0, "' is already imported" };
static const wstr S442 = { 38, 38, 1, 0, "' is not a library: it ends with ;exit" };
static const wstr S443 = { 31, 31, 1, 0, "only a library ends with ';end'" };
static const wstr S444 = { 25, 25, 1, 0, "a program ends with ;exit" };
static const wstr S445 = { 88, 88, 1, 0, "a library's top level may contain only import, fn, const, var, ref and enum declarations" };
static const wstr S446 = { 37, 37, 1, 0, "move other statements into a function" };
static const wstr S447 = { 28, 28, 1, 0, "a library must end with ;end" };
static const wstr S448 = { 56, 56, 1, 0, "mark what it exports with 'export', and end it with ;end" };
static const wstr S449 = { 15, 15, 1, 0, "cannot export '" };
static const wstr S450 = { 38, 38, 1, 0, "': it is not declared at the top level" };
static const wstr S451 = { 17, 17, 1, 0, "': it is a module" };
static const wstr S452 = { 5, 5, 1, 0, "line " };
static const wstr S453 = { 6, 6, 1, 0, " line " };
static const wstr S454 = { 49, 49, 1, 0, "' never produces a value, but its value is used (" };
static const wstr S455 = { 65, 65, 1, 0, "end the function with its value, or with null if that is intended" };
static const wstr S456 = { 29, 29, 1, 0, "a program must end with ;exit" };
static const wstr S457 = { 50, 50, 1, 0, "end the program with ;exit, or a library with ;end" };
static const wstr S458 = { 3, 3, 1, 0, "add" };
static const wstr S459 = { 3, 3, 1, 0, "sub" };
static const wstr S460 = { 3, 3, 1, 0, "mul" };
static const wstr S461 = { 3, 3, 1, 0, "div" };
static const wstr S462 = { 3, 3, 1, 0, "mod" };
static const wstr S463 = { 3, 3, 1, 0, "pow" };
static const wstr S464 = { 2, 2, 1, 0, "eq" };
static const wstr S465 = { 2, 2, 1, 0, "ne" };
static const wstr S466 = { 2, 2, 1, 0, "lt" };
static const wstr S467 = { 2, 2, 1, 0, "gt" };
static const wstr S468 = { 2, 2, 1, 0, "le" };
static const wstr S469 = { 2, 2, 1, 0, "ge" };
static const wstr S470 = { 4, 4, 1, 0, "band" };
static const wstr S471 = { 3, 3, 1, 0, "bor" };
static const wstr S472 = { 4, 4, 1, 0, "bxor" };
static const wstr S473 = { 3, 3, 1, 0, "shl" };
static const wstr S474 = { 3, 3, 1, 0, "shr" };
static const wstr S475 = { 3, 3, 1, 0, "neg" };
static const wstr S476 = { 3, 3, 1, 0, "not" };
static const wstr S477 = { 4, 4, 1, 0, "bnot" };
static const wstr S478 = { 1, 1, 1, 0, "w" };
static const wstr S479 = { 1, 1, 1, 0, "l" };
static const wstr S480 = { 2, 2, 1, 0, "  " };
static const wstr S481 = { 2, 2, 1, 0, "$t" };
static const wstr S482 = { 66, 66, 1, 0, "'return' inside a conditional used as a value is not supported yet" };
static const wstr S483 = { 7, 7, 1, 0, "return " };
static const wstr S484 = { 16, 16, 1, 0, "throw $rt.panic(" };
static const wstr S485 = { 2, 2, 1, 0, ");" };
static const wstr S486 = { 48, 48, 1, 0, "' cannot leave a conditional used as a value yet" };
static const wstr S487 = { 6, 6, 1, 0, "break;" };
static const wstr S488 = { 9, 9, 1, 0, "continue;" };
static const wstr S489 = { 7, 7, 1, 0, "while (" };
static const wstr S490 = { 9, 9, 1, 0, "$rt.cond(" };
static const wstr S491 = { 4, 4, 1, 0, ")) {" };
static const wstr S492 = { 19, 19, 1, 0, "throw $rt.failWith(" };
static const wstr S493 = { 9, 9, 1, 0, "$rt.exit(" };
static const wstr S494 = { 3, 3, 1, 0, "let" };
static const wstr S495 = { 3, 3, 1, 0, " = " };
static const wstr S496 = { 29, 29, 1, 0, "codegen: unhandled statement " };
static const wstr S497 = { 9, 9, 1, 0, "function " };
static const wstr S498 = { 3, 3, 1, 0, ") {" };
static const wstr S499 = { 12, 12, 1, 0, " = $rt.bind(" };
static const wstr S500 = { 5, 5, 1, 0, "try {" };
static const wstr S501 = { 14, 14, 1, 0, "} catch ($e) {" };
static const wstr S502 = { 22, 22, 1, 0, "  throw $rt.leave($e, " };
static const wstr S503 = { 7, 7, 1, 0, "const l" };
static const wstr S504 = { 16, 16, 1, 0, " = $rt.makeEnum(" };
static const wstr S505 = { 3, 3, 1, 0, ", [" };
static const wstr S506 = { 3, 3, 1, 0, "]);" };
static const wstr S507 = { 12, 12, 1, 0, "$rt.frozen([" };
static const wstr S508 = { 2, 2, 1, 0, "])" };
static const wstr S509 = { 9, 9, 1, 0, "$rt.bind(" };
static const wstr S510 = { 6, 6, 1, 0, "const " };
static const wstr S511 = { 7, 7, 1, 0, " = $rt." };
static const wstr S512 = { 5, 5, 1, 0, ", \"f_" };
static const wstr S513 = { 3, 3, 1, 0, "\", " };
static const wstr S514 = { 11, 11, 1, 0, "$rt.setMem(" };
static const wstr S515 = { 11, 11, 1, 0, "$rt.updMem(" };
static const wstr S516 = { 6, 6, 1, 0, ", $rt." };
static const wstr S517 = { 11, 11, 1, 0, "$rt.setIdx(" };
static const wstr S518 = { 11, 11, 1, 0, "$rt.updIdx(" };
static const wstr S519 = { 4, 4, 1, 0, ", f_" };
static const wstr S520 = { 25, 25, 1, 0, "$rt.rec({ __proto__: null" };
static const wstr S521 = { 4, 4, 1, 0, " }, " };
static const wstr S522 = { 2, 2, 1, 0, "$e" };
static const wstr S523 = { 4, 4, 1, 0, "let " };
static const wstr S524 = { 9, 9, 1, 0, "} catch (" };
static const wstr S525 = { 6, 6, 1, 0, "if (!(" };
static const wstr S526 = { 29, 29, 1, 0, " instanceof $rt.Fail)) throw " };
static const wstr S527 = { 6, 6, 1, 0, ".code;" };
static const wstr S528 = { 3, 3, 1, 0, "raw" };
static const wstr S529 = { 7, 7, 1, 0, "} else " };
static const wstr S530 = { 4, 4, 1, 0, "if (" };
static const wstr S531 = { 8, 8, 1, 0, "} else {" };
static const wstr S532 = { 3, 3, 1, 0, " ? " };
static const wstr S533 = { 3, 3, 1, 0, " : " };
static const wstr S534 = { 9, 9, 1, 0, "(() => {\n" };
static const wstr S535 = { 4, 4, 1, 0, "})()" };
static const wstr S536 = { 7, 7, 1, 0, "$rt.eq(" };
static const wstr S537 = { 4, 4, 1, 0, " || " };
static const wstr S538 = { 11, 11, 1, 0, "$rt.noCase(" };
static const wstr S539 = { 2, 2, 1, 0, "((" };
static const wstr S540 = { 5, 5, 1, 0, ") => " };
static const wstr S541 = { 2, 2, 1, 0, ")(" };
static const wstr S542 = { 7, 7, 1, 0, ") => {\n" };
static const wstr S543 = { 3, 3, 1, 0, "})(" };
static const wstr S544 = { 5, 5, 1, 0, "$std." };
static const wstr S545 = { 6, 6, 1, 0, "$rt.f(" };
static const wstr S546 = { 9, 9, 1, 0, "$rt.text(" };
static const wstr S547 = { 2, 2, 1, 0, "\"\"" };
static const wstr S548 = { 3, 3, 1, 0, " + " };
static const wstr S549 = { 7, 7, 1, 0, "$rt.NAN" };
static const wstr S550 = { 7, 7, 1, 0, "$rt.INF" };
static const wstr S551 = { 8, 8, 1, 0, "$rt.idx(" };
static const wstr S552 = { 3, 3, 1, 0, ".m_" };
static const wstr S553 = { 11, 11, 1, 0, "$rt.get(\"f_" };
static const wstr S554 = { 4, 4, 1, 0, "$rt." };
static const wstr S555 = { 10, 10, 1, 0, "($rt.bool(" };
static const wstr S556 = { 3, 3, 1, 0, ", \"" };
static const wstr S557 = { 3, 3, 1, 0, "\") " };
static const wstr S558 = { 10, 10, 1, 0, " $rt.bool(" };
static const wstr S559 = { 3, 3, 1, 0, "\"))" };
static const wstr S560 = { 10, 10, 1, 0, "($rt.cond(" };
static const wstr S561 = { 4, 4, 1, 0, ") ? " };
static const wstr S562 = { 30, 30, 1, 0, "codegen: unhandled expression " };
static const wstr S563 = { 14, 14, 1, 0, "\"use strict\";\n" };
static const wstr S564 = { 24, 24, 1, 0, "\n$rt.main(function () {\n" };
static const wstr S565 = { 5, 5, 1, 0, "\n});\n" };
static const wstr S566 = { 2, 2, 1, 0, "\\\"" };
static const wstr S567 = { 2, 2, 1, 0, "\\\\" };
static const wstr S568 = { 2, 2, 1, 0, "{ " };
static const wstr S569 = { 5, 5, 1, 0, ", 0, " };
static const wstr S570 = { 2, 2, 1, 0, " }" };
static const wstr S571 = { 4, 4, 1, 0, "NULL" };
static const wstr S572 = { 8, 8, 1, 0, "(wv[]){ " };
static const wstr S573 = { 1, 1, 1, 0, "v" };
static const wstr S574 = { 2, 2, 1, 0, "&S" };
static const wstr S575 = { 3, 3, 1, 0, "&F_" };
static const wstr S576 = { 19, 19, 1, 0, "static const wmem l" };
static const wstr S577 = { 7, 7, 1, 0, "[] = { " };
static const wstr S578 = { 3, 3, 1, 0, " };" };
static const wstr S579 = { 3, 3, 1, 0, "fr." };
static const wstr S580 = { 2, 2, 1, 0, "up" };
static const wstr S581 = { 4, 4, 1, 0, "->up" };
static const wstr S582 = { 2, 2, 1, 0, "->" };
static const wstr S583 = { 7, 7, 1, 0, "rt_def(" };
static const wstr S584 = { 3, 3, 1, 0, "wv " };
static const wstr S585 = { 3, 3, 1, 0, "({ " };
static const wstr S586 = { 4, 4, 1, 0, "; })" };
static const wstr S587 = { 12, 12, 1, 0, " = RT_UNSET;" };
static const wstr S588 = { 6, 6, 1, 0, "(void)" };
static const wstr S589 = { 7, 7, 1, 0, "RT_NULL" };
static const wstr S590 = { 7, 7, 1, 0, "RT_RET(" };
static const wstr S591 = { 9, 9, 1, 0, "rt_panic(" };
static const wstr S592 = { 8, 8, 1, 0, "rt_cond(" };
static const wstr S593 = { 5, 5, 1, 0, "rt_i(" };
static const wstr S594 = { 8, 8, 1, 0, "rt_exit(" };
static const wstr S595 = { 7, 7, 1, 0, "rt_i(0)" };
static const wstr S596 = { 13, 13, 1, 0, "rt_fail_with(" };
static const wstr S597 = { 16, 16, 1, 0, "RT_RET(RT_NULL);" };
static const wstr S598 = { 9, 9, 1, 0, "struct fr" };
static const wstr S599 = { 4, 4, 1, 0, " *up" };
static const wstr S600 = { 2, 2, 1, 0, "p_" };
static const wstr S601 = { 10, 10, 1, 0, "static wv " };
static const wstr S602 = { 4, 4, 1, 0, "void" };
static const wstr S603 = { 9, 9, 1, 0, "rt_enter(" };
static const wstr S604 = { 8, 8, 1, 0, " fr = { " };
static const wstr S605 = { 8, 8, 1, 0, ".up = up" };
static const wstr S606 = { 8, 8, 1, 0, "rt_bind(" };
static const wstr S607 = { 13, 13, 1, 0, "(void)rt_def(" };
static const wstr S608 = { 3, 3, 1, 0, "rt_" };
static const wstr S609 = { 7, 7, 1, 0, "rt_set_" };
static const wstr S610 = { 7, 7, 1, 0, "rt_upd_" };
static const wstr S611 = { 5, 5, 1, 0, ", rt_" };
static const wstr S612 = { 7, 7, 1, 0, "rt_arr(" };
static const wstr S613 = { 1, 1, 1, 0, "K" };
static const wstr S614 = { 7, 7, 1, 0, "rt_rec(" };
static const wstr S615 = { 18, 18, 1, 0, "if (rt_failcode) {" };
static const wstr S616 = { 17, 17, 1, 0, "rt_i(rt_failcode)" };
static const wstr S617 = { 16, 16, 1, 0, "rt_failcode = 0;" };
static const wstr S618 = { 3, 3, 1, 0, "({\n" };
static const wstr S619 = { 2, 2, 1, 0, "})" };
static const wstr S620 = { 3, 3, 1, 0, "eq(" };
static const wstr S621 = { 12, 12, 1, 0, "(rt_no_case(" };
static const wstr S622 = { 11, 11, 1, 0, "), RT_NULL)" };
static const wstr S623 = { 6, 6, 1, 0, "({ wv " };
static const wstr S624 = { 2, 2, 1, 0, "; " };
static const wstr S625 = { 4, 4, 1, 0, "std_" };
static const wstr S626 = { 3, 3, 1, 0, "&fr" };
static const wstr S627 = { 7, 7, 1, 0, "rt_chk(" };
static const wstr S628 = { 5, 5, 1, 0, "rt_f(" };
static const wstr S629 = { 5, 5, 1, 0, "rt_s(" };
static const wstr S630 = { 8, 8, 1, 0, "rt_text(" };
static const wstr S631 = { 7, 7, 1, 0, "rt_cat(" };
static const wstr S632 = { 5, 5, 1, 0, "rt_b(" };
static const wstr S633 = { 6, 6, 1, 0, "RT_NAN" };
static const wstr S634 = { 6, 6, 1, 0, "RT_INF" };
static const wstr S635 = { 2, 2, 1, 0, "()" };
static const wstr S636 = { 7, 7, 1, 0, "rt_idx(" };
static const wstr S637 = { 6, 6, 1, 0, "rt_m(&" };
static const wstr S638 = { 7, 7, 1, 0, "rt_get(" };
static const wstr S639 = { 13, 13, 1, 0, "rt_b(rt_bool(" };
static const wstr S640 = { 9, 9, 1, 0, " rt_bool(" };
static const wstr S641 = { 9, 9, 1, 0, "(rt_cond(" };
static const wstr S642 = { 2, 2, 1, 0, "\r\n" };
static const wstr S643 = { 20, 20, 1, 0, "// ---- program ----" };
static const wstr S644 = { 19, 19, 1, 0, "static const wstr S" };
static const wstr S645 = { 14, 14, 1, 0, "static wstr F_" };
static const wstr S646 = { 26, 26, 1, 0, "static const wstr *const K" };
static const wstr S647 = { 5, 5, 1, 0, " *up;" };
static const wstr S648 = { 10, 10, 1, 0, "char none;" };
static const wstr S649 = { 3, 3, 1, 0, " { " };
static const wstr S650 = { 2, 2, 1, 0, " {" };
static const wstr S651 = { 28, 28, 1, 0, "static void wvt_main(void) {" };
static const wstr S652 = { 10, 10, 1, 0, "  rt_syms(" };
static const wstr S653 = { 19, 19, 1, 0, ", (wstr *const[]){ " };
static const wstr S654 = { 4, 4, 1, 0, " });" };
static const wstr S655 = { 4, 4, 1, 0, "  | " };
static const wstr S656 = { 4, 4, 1, 0, "  { " };
static const wstr S657 = { 4, 4, 1, 0, "    " };
static const wstr S658 = { 3, 3, 1, 0, "  }" };
static const wstr S659 = { 5, 5, 1, 0, "array" };
static const wstr S660 = { 2, 2, 1, 0, "[]" };
static const wstr S661 = { 6, 6, 1, 0, "record" };
static const wstr S662 = { 2, 2, 1, 0, ".." };
static const wstr S663 = { 14, 14, 1, 0, "import cycle: " };
static const wstr S664 = { 4, 4, 1, 0, " -> " };
static const wstr S665 = { 13, 13, 1, 0, "cannot read '" };
static const wstr S666 = { 3, 1, 0, 0, "﻿" };
static const wstr S667 = { 7, 7, 1, 0, "usage:\n" };
static const wstr S668 = { 50, 50, 1, 0, "  wvc <file.wvt> [-o <out.c>] [--runtime <wvt.c>]\n" };
static const wstr S669 = { 64, 64, 1, 0, "  wvc <file.wvt> --target js [-o <out.js>] [--runtime <wvt.js>]\n" };
static const wstr S670 = { 76, 76, 1, 0, "  wvc build <file.wvt> [-o <program>] [--cc <compiler>] [--runtime <wvt.c>]\n" };
static const wstr S671 = { 26, 26, 1, 0, "  wvc --tokens <file.wvt>\n" };
static const wstr S672 = { 22, 22, 1, 0, "  wvc --ast <file.wvt>" };
static const wstr S673 = { 17, 17, 1, 0, "wvc: cannot read " };
static const wstr S674 = { 5, 5, 1, 0, "build" };
static const wstr S675 = { 8, 8, 1, 0, "--tokens" };
static const wstr S676 = { 5, 5, 1, 0, "--ast" };
static const wstr S677 = { 2, 2, 1, 0, "-o" };
static const wstr S678 = { 9, 9, 1, 0, "--runtime" };
static const wstr S679 = { 8, 8, 1, 0, "--target" };
static const wstr S680 = { 4, 4, 1, 0, "--cc" };
static const wstr S681 = { 2, 2, 1, 0, "js" };
static const wstr S682 = { 1, 1, 1, 0, "c" };
static const wstr S683 = { 2, 2, 1, 0, "OS" };
static const wstr S684 = { 10, 10, 1, 0, "Windows_NT" };
static const wstr S685 = { 3, 3, 1, 0, "ZIG" };
static const wstr S686 = { 3, 3, 1, 0, "zig" };
static const wstr S687 = { 5, 5, 1, 0, "wvc: " };
static const wstr S688 = { 52, 52, 1, 0, " does not end with .wvt, so name the program with -o" };
static const wstr S689 = { 4, 4, 1, 0, ".exe" };
static const wstr S690 = { 13, 13, 1, 0, "runtime/wvt.c" };
static const wstr S691 = { 14, 14, 1, 0, "runtime/wvt.js" };
static const wstr S692 = { 3, 3, 1, 0, "../" };
static const wstr S693 = { 18, 18, 1, 0, "wvc: cannot write " };
static const wstr S694 = { 2, 2, 1, 0, ".c" };
static const wstr S695 = { 3, 3, 1, 0, "-O2" };
static const wstr S696 = { 7, 7, 1, 0, "zig.exe" };
static const wstr S697 = { 2, 2, 1, 0, "cc" };
static const wstr S698 = { 16, 16, 1, 0, "wvc: cannot run " };
static const wstr S699 = { 67, 67, 1, 0, ": install Zig (https://ziglang.org), or name a C compiler with --cc" };
static const wstr S700 = { 23, 23, 1, 0, " failed with exit code " };
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
static wstr F_js = { 2, 2, 1, 0, "js" };
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
static const wstr *const K49[] = { &F_name, &F_lib };
static const wstr *const K50[] = { &F_k, &F_js, &F_pos, &F_paren };
static const wstr *const K51[] = { &F_name, &F_kind, &F_d };
static const wstr *const K52[] = { &F_name, &F_parent, &F_depth, &F_top, &F_children, &F_cname };
static const wstr *const K53[] = { &F_kind, &F_name, &F_func, &F_cname, &F_isRef, &F_captured, &F_fnIndex };
static const wstr *const K54[] = { &F_c, &F_node };
static const wstr *const K55[] = { &F_c, &F_k };
static const wstr *const K56[] = { &F_pre, &F_cs };
static const wstr *const K57[] = { &F_proto, &F_lines };
static const wstr *const K58[] = { &F_node, &F_lib };
static const wstr *const K59[] = { &F_file, &F_chars, &F_path, &F_program, &F_imports, &F_exports, &F_notLibrary, &F_refTargets };
struct fr27 { wv v71_cs; wv v72_n; wv v73_i; wv v74_prevT; wv v75_nlNow; };
struct fr50 { wv v187_toks; wv v189_p; wv v190_depth; wv v191_exports; wv v192_firstExport; };
struct fr99 { wv v378_modules; wv v380_errors; wv v381_nextFn; wv v382_curMod; wv v383_functions; wv v384_uses; wv v385_enums; wv v418_topFn; };
struct fr139 { wv v631_modules; wv v633_out; wv v634_indent; wv v635_n; wv v636_loops; wv v637_iife; wv v638_mod; wv v639_stolen; wv v640_enums; wv v641_refPositions; };
struct fr183 { wv v818_modules; wv v819_runtime; wv v820_n; wv v821_tmps; wv v822_strings; wv v823_fields; wv v824_shapes; wv v825_enumDefs; wv v826_globals; wv v827_frames; wv v828_functions; wv v829_out; wv v830_indent; wv v831_loops; wv v832_iife; wv v833_decls; wv v834_fns; wv v835_tops; wv v836_slots; wv v837_curMod; wv v838_mod; wv v839_curSlot; wv v840_stolen; wv v841_ownEnums; wv v842_cur; };
struct fr252 { wv v1218_modules; };
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
static wv v1245_a;
static wv v1254_tokens;
static wv v1255_program;
static wv v1256_p;
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
static wv w4_isIdent(wv v170_s);
static wv w4_describe(wv v173_tk);
static wv w4_failAtHint(wv v174_tk, wv v175_message, wv v176_hint);
static wv w4_failAt(wv v177_tk, wv v178_message);
static wv w4_simpleStatement(wv v179_k, wv v180_e);
static wv w4_parse(wv v181_toks);
static wv w4_hasEffect(wv v182_e);
static wv w4_parser(wv p_v187_toks, wv v188_inner);
static wv s193_peek(struct fr50 *up);
static wv s194_at(struct fr50 *up, wv v230_type);
static wv s195_atWord(struct fr50 *up, wv v231_word);
static wv s196_atCall(struct fr50 *up);
static wv s197_next(struct fr50 *up);
static wv s198_accept(struct fr50 *up, wv v233_type);
static wv s199_expect(struct fr50 *up, wv v234_type, wv v235_msg, wv v236_hint);
static wv s200_expectWord(struct fr50 *up, wv v239_word, wv v240_hint);
static wv s201_lintSpace(struct fr50 *up, wv v242_op);
static wv s202_program(struct fr50 *up);
static wv s203_statement(struct fr50 *up);
static wv s204_statementBody(struct fr50 *up, wv v255_tk);
static wv s205_declaration(struct fr50 *up);
static wv s206_setStatement(struct fr50 *up);
static wv s207_failStatement(struct fr50 *up);
static wv s208_importStatement(struct fr50 *up);
static wv s209_exportStatement(struct fr50 *up);
static wv s210_endStatement(struct fr50 *up);
static wv s211_fnStatement(struct fr50 *up);
static wv s212_enumStatement(struct fr50 *up);
static wv s213_cond(struct fr50 *up, wv v290_kw);
static wv s214_block(struct fr50 *up);
static wv s215_lvalue(struct fr50 *up);
static wv s216_expr(struct fr50 *up);
static wv s217_ternary(struct fr50 *up);
static wv s218_ternaryTail(struct fr50 *up, wv v300_c);
static wv s219_binary(struct fr50 *up, wv v305_min);
static wv s220_binaryFrom(struct fr50 *up, wv v306_left, wv v307_min);
static wv s221_unary(struct fr50 *up);
static wv s222_powerTail(struct fr50 *up, wv v314_base);
static wv s223_postfix(struct fr50 *up);
static wv s224_call(struct fr50 *up, wv v322_args);
static wv s225_primary(struct fr50 *up);
static wv s226_record(struct fr50 *up, wv v336_open);
static wv s227_parseMatch(struct fr50 *up);
static wv s228_caseValue(struct fr50 *up);
static wv s229_chain(struct fr50 *up);
static wv w5_stdSig(wv v360_name);
static wv w5_modeOf(wv v362_x);
static wv w5_kindText(wv v363_kind);
static wv w5_plural(wv v364_n);
static wv w5_enumText(wv v365_x);
static wv w5_caseKey(wv v366_v);
static wv w5_caseText(wv v367_v);
static wv w5_getsAwayWithValue(wv v368_list);
static wv w5_lineIn(wv v373_chars, wv v374_pos);
static wv w5_lineOf(wv v377_pos);
static wv w5_check(wv p_v378_modules, wv v379_requireExit);
static wv s386_error(struct fr99 *up, wv v423_pos, wv v424_message, wv v425_hint);
static wv s387_enter(struct fr99 *up, wv v426_m);
static wv s388_enumMembers(struct fr99 *up, wv v427_d);
static wv s389_firstUse(struct fr99 *up, wv v429_mod, wv v430_declPos);
static wv s390_requireValue(struct fr99 *up, wv v432_b);
static wv s391_newScope(struct fr99 *up, wv v435_parent);
static wv s392_decl(struct fr99 *up, wv v437_kind, wv v438_name, wv v439_mode, wv v440_namePos, wv v441_visibleAt, wv v442_fnId, wv v443_arity);
static wv s393_own(struct fr99 *up, wv v444_scope, wv v445_name);
static wv s394_declare(struct fr99 *up, wv v447_scope, wv v448_name, wv v449_d);
static wv s395_lookup(struct fr99 *up, wv v453_scope, wv v454_name);
static wv s396_resolve(struct fr99 *up, wv v457_name, wv v458_pos, wv v459_scope, wv v460_ctx);
static wv s397_block(struct fr99 *up, wv v462_b, wv v463_scope, wv v464_ctx);
static wv s398_stmt(struct fr99 *up, wv v469_s, wv v470_scope, wv v471_ctx);
static wv s399_onfailHandler(struct fr99 *up, wv v478_s, wv v479_scope, wv v480_ctx);
static wv s400_checkMatch(struct fr99 *up, wv v483_e, wv v484_scope, wv v485_ctx, wv v486_stmtLevel);
static wv s401_moduleNamed(struct fr99 *up, wv v506_x, wv v507_scope);
static wv s402_exported(struct fr99 *up, wv v509_lib, wv v510_name);
static wv s403_enumNamed(struct fr99 *up, wv v513_x, wv v514_scope);
static wv s404_exportOf(struct fr99 *up, wv v518_e, wv v519_m);
static wv s405_unknownModule(struct fr99 *up, wv v521_x, wv v522_scope);
static wv s406_qualified(struct fr99 *up, wv v524_e, wv v525_m);
static wv s407_caseEnum(struct fr99 *up, wv v528_v, wv v529_scope);
static wv s408_member(struct fr99 *up, wv v535_e, wv v536_d);
static wv s409_chain(struct fr99 *up, wv v541_e, wv v542_scope, wv v543_ctx, wv v544_stmtLevel);
static wv s410_lvalue(struct fr99 *up, wv v547_t, wv v548_scope, wv v549_ctx);
static wv s411_mustBeMutable(struct fr99 *up, wv v556_d, wv v557_name, wv v558_pos, wv v559_message, wv v560_rebind);
static wv s412_unknownStd(struct fr99 *up, wv v562_node);
static wv s413_expr(struct fr99 *up, wv v563_e, wv v564_scope, wv v565_ctx);
static wv s414_callee(struct fr99 *up, wv v575_e, wv v576_scope, wv v577_used);
static wv s415_importNames(struct fr99 *up, wv v585_m, wv v586_scope);
static wv s416_libraryRules(struct fr99 *up, wv v595_m, wv v596_scope, wv v597_isEntry, wv v598_last);
static wv s417_checkModule(struct fr99 *up, wv v605_m, wv v606_isEntry);
static wv w6_binFn(wv v617_op);
static wv w6_modeOf(wv v619_x);
static wv w6_unFn(wv v620_op);
static wv w6_jsBool(wv v621_b);
static wv w6_repeat(wv v622_s, wv v623_count);
static wv w6_jsFloat(wv v626_x);
static wv w6_generate(wv p_v631_modules, wv v632_runtime);
static wv s642_modId(struct fr139 *up, wv v677_m);
static wv s643_enter(struct fr139 *up, wv v678_m);
static wv s644_id(struct fr139 *up, wv v682_name);
static wv s645_stolenFrom(struct fr139 *up, wv v683_x);
static wv s646_exportKind(struct fr139 *up, wv v685_lib, wv v686_name);
static wv s647_enumJs(struct fr139 *up, wv v689_x);
static wv s648_emit(struct fr139 *up, wv v691_line);
static wv s649_tmp(struct fr139 *up);
static wv s650_stmts(struct fr139 *up, wv v693_list);
static wv s651_blockBody(struct fr139 *up, wv v695_b);
static wv s652_valueOf(struct fr139 *up, wv v696_b);
static wv s653_stmt(struct fr139 *up, wv v697_s);
static wv s654_fnDef(struct fr139 *up, wv v699_s);
static wv s655_enumDef(struct fr139 *up, wv v705_s, wv v706_m);
static wv s656_bindRhs(struct fr139 *up, wv v709_e, wv v710_mode);
static wv s657_setStmt(struct fr139 *up, wv v713_s);
static wv s658_record(struct fr139 *up, wv v720_e, wv v721_mutable);
static wv s659_onfailStmt(struct fr139 *up, wv v725_s);
static wv s660_chainStmt(struct fr139 *up, wv v734_e);
static wv s661_test(struct fr139 *up, wv v737_b);
static wv s662_branchCount(struct fr139 *up, wv v738_e);
static wv s663_branchBody(struct fr139 *up, wv v739_e, wv v740_i);
static wv s664_branchTest(struct fr139 *up, wv v741_e, wv v742_t, wv v743_i);
static wv s665_isPlain(struct fr139 *up, wv v744_e);
static wv s666_ternaryChain(struct fr139 *up, wv v748_e, wv v749_t);
static wv s667_branchFunction(struct fr139 *up, wv v752_e, wv v753_t);
static wv s668_chainExpr(struct fr139 *up, wv v761_e);
static wv s669_caseTest(struct fr139 *up, wv v763_t, wv v764_a);
static wv s670_matchElse(struct fr139 *up, wv v767_e, wv v768_t);
static wv s671_matchStmt(struct fr139 *up, wv v769_e);
static wv s672_matchExpr(struct fr139 *up, wv v773_e);
static wv s673_callJs(struct fr139 *up, wv v778_callee, wv v779_args);
static wv s674_exprList(struct fr139 *up, wv v781_list);
static wv s675_expr(struct fr139 *up, wv v784_e);
static wv w7_binFn(wv v795_op);
static wv w7_modeOf(wv v797_x);
static wv w7_unFn(wv v798_op);
static wv w7_repeat(wv v799_s, wv v800_count);
static wv w7_utf8Length(wv v803_s);
static wv w7_cString(wv v808_s);
static wv w7_wstrInit(wv v814_s);
static wv w7_values(wv v817_cs);
static wv w7_generateC(wv p_v818_modules, wv p_v819_runtime);
static wv s843_modId(struct fr183 *up, wv v903_m);
static wv s844_hasFrame(struct fr183 *up, wv v904_f);
static wv s845_newScope(struct fr183 *up, wv v905_parent);
static wv s846_put(struct fr183 *up, wv v907_scope, wv v908_name, wv v909_kind, wv v910_d);
static wv s847_find(struct fr183 *up, wv v911_scope, wv v912_name);
static wv s848_kindOf(struct fr183 *up, wv v916_scope, wv v917_name);
static wv s849_newFn(struct fr183 *up, wv v919_name, wv v920_parent);
static wv s850_bind(struct fr183 *up, wv v922_kind, wv v923_name, wv v924_f, wv v925_atTop, wv v926_mode);
static wv s851_ablock(struct fr183 *up, wv v929_b, wv v930_scope, wv v931_f, wv v932_topScope);
static wv s852_astmt(struct fr183 *up, wv v940_s, wv v941_scope, wv v942_f, wv v943_topScope);
static wv s853_use(struct fr183 *up, wv v954_x, wv v955_scope, wv v956_f);
static wv s854_atarget(struct fr183 *up, wv v958_t, wv v959_scope, wv v960_f);
static wv s855_notValue(struct fr183 *up, wv v961_x, wv v962_scope);
static wv s856_aexpr(struct fr183 *up, wv v964_e, wv v965_scope, wv v966_f);
static wv s857_analyze(struct fr183 *up, wv v974_m);
static wv s858_enter(struct fr183 *up, wv v981_m);
static wv s859_emit(struct fr183 *up, wv v985_line);
static wv s860_tmp(struct fr183 *up);
static wv s861_str(struct fr183 *up, wv v987_s);
static wv s862_fieldRef(struct fr183 *up, wv v989_name);
static wv s863_enumDef(struct fr183 *up, wv v990_s, wv v991_m);
static wv s864_stolenFrom(struct fr183 *up, wv v995_x);
static wv s865_enumIn(struct fr183 *up, wv v997_list, wv v998_name);
static wv s866_enumOf(struct fr183 *up, wv v1000_x);
static wv s867_place(struct fr183 *up, wv v1005_d);
static wv s868_tdz(struct fr183 *up, wv v1007_d);
static wv s869_read(struct fr183 *up, wv v1008_d);
static wv s870_seq(struct fr183 *up, wv v1009_args);
static wv s871_wrap(struct fr183 *up, wv v1016_sq, wv v1017_inner);
static wv s872_stmts(struct fr183 *up, wv v1018_list);
static wv s873_resets(struct fr183 *up, wv v1020_b);
static wv s874_blockBody(struct fr183 *up, wv v1024_b);
static wv s875_valueOf(struct fr183 *up, wv v1025_b);
static wv s876_stmt(struct fr183 *up, wv v1026_s);
static wv s877_signal(struct fr183 *up, wv v1028_code);
static wv s878_bindDecl(struct fr183 *up, wv v1029_d, wv v1030_value);
static wv s879_fnDef(struct fr183 *up, wv v1032_s);
static wv s880_bindRhs(struct fr183 *up, wv v1052_e, wv v1053_mode);
static wv s881_setStmt(struct fr183 *up, wv v1055_s);
static wv s882_exprs(struct fr183 *up, wv v1070_list);
static wv s883_array(struct fr183 *up, wv v1073_e, wv v1074_mutable);
static wv s884_record(struct fr183 *up, wv v1076_e, wv v1077_mutable);
static wv s885_onfailStmt(struct fr183 *up, wv v1085_s);
static wv s886_chainStmt(struct fr183 *up, wv v1094_e);
static wv s887_test(struct fr183 *up, wv v1097_b);
static wv s888_branchCount(struct fr183 *up, wv v1098_e);
static wv s889_branchBody(struct fr183 *up, wv v1099_e, wv v1100_i);
static wv s890_branchTest(struct fr183 *up, wv v1101_e, wv v1102_t, wv v1103_i);
static wv s891_isPlain(struct fr183 *up, wv v1104_e);
static wv s892_ternaryChain(struct fr183 *up, wv v1108_e, wv v1109_t);
static wv s893_branchValue(struct fr183 *up, wv v1112_e, wv v1113_t, wv v1114_subject);
static wv s894_chainExpr(struct fr183 *up, wv v1123_e);
static wv s895_caseTest(struct fr183 *up, wv v1124_t, wv v1125_a);
static wv s896_matchElse(struct fr183 *up, wv v1128_e, wv v1129_t);
static wv s897_matchStmt(struct fr183 *up, wv v1130_e);
static wv s898_matchExpr(struct fr183 *up, wv v1134_e);
static wv s899_callC(struct fr183 *up, wv v1138_callee, wv v1139_args, wv v1140_checked);
static wv s900_expr(struct fr183 *up, wv v1149_e);
static wv s901_assemble(struct fr183 *up);
static wv w8_dumpTokens(wv v1178_toks, wv v1179_indent);
static wv w8_sorted(wv v1185_xs);
static wv w8_dumpValue(wv v1190_x, wv v1191_indent);
static wv w9_dirOf(wv v1200_path);
static wv w9_normalize(wv v1203_path);
static wv w9_with(wv v1208_xs, wv v1209_x);
static wv w9_failIn(wv v1212_file, wv v1213_chars, wv v1214_pos, wv v1215_message);
static wv w9_loadProgram(wv v1216_entryFile, wv v1217_entryChars);
static wv s1219_loaded(struct fr252 *up, wv v1222_path);
static wv s1220_visit(struct fr252 *up, wv v1224_file, wv v1225_chars, wv v1226_path, wv v1227_chain);
static wv w_die(void);
static wv w_load(wv v1241_file);
static wv w_onWindows(void);
static wv w_tryRead(wv v1247_path);
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

static wv w4_isIdent(wv v170_s) {
  rt_enter("isIdent");
  v170_s = rt_bind(v170_s, 0);
  wv v171_cs = rt_bind(std_split(v170_s, rt_s(&S13)), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v171_cs), rt_i(0)), "||") || rt_bool(rt_not(rt_chk(w1_isIdStart(rt_idx(rt_i(0), v171_cs)), "isIdent")), "||")))) {
    RT_RET(rt_b(0));
  }
  wv v172_k = rt_i(1);
  while (rt_cond(({ wv t231 = v172_k; wv t232 = std_length(v171_cs); rt_lt(t231, t232); }))) {
    if (rt_cond(rt_not(rt_chk(w1_isIdChar(rt_idx(v172_k, v171_cs)), "isIdent")))) {
      RT_RET(rt_b(0));
    }
    v172_k = rt_add(v172_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv w4_describe(wv v173_tk) {
  rt_enter("describe");
  v173_tk = rt_bind(v173_tk, 0);
  if (rt_cond(rt_eq(rt_get(&F_t, v173_tk), rt_s(&S110)))) {
    RT_RET(rt_s(&S178));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v173_tk), rt_s(&S137)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_v, v173_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v173_tk), rt_s(&S156)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S179), rt_text(rt_get(&F_v, v173_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v173_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v173_tk), rt_s(&S148)), "||")))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_text, v173_tk)), rt_s(&S113) }));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v173_tk), rt_s(&S171)))) {
    RT_RET(rt_s(&S180));
  }
  if (rt_cond(rt_eq(rt_get(&F_t, v173_tk), rt_s(&S111)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_v, v173_tk)), rt_s(&S181) }));
  }
  RT_RET(rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v173_tk)), rt_s(&S113) }));
}

static wv w4_failAtHint(wv v174_tk, wv v175_message, wv v176_hint) {
  rt_enter("failAtHint");
  v174_tk = rt_bind(v174_tk, 0);
  v175_message = rt_bind(v175_message, 0);
  v176_hint = rt_bind(v176_hint, 0);
  (void)({ wv t233 = rt_get(&F_pos, v174_tk); wv t234 = v175_message; wv t235 = v176_hint; rt_chk(w2_failHint(t233, t234, t235), "failAtHint"); });
  RT_RET(RT_NULL);
}

static wv w4_failAt(wv v177_tk, wv v178_message) {
  rt_enter("failAt");
  v177_tk = rt_bind(v177_tk, 0);
  v178_message = rt_bind(v178_message, 0);
  (void)({ wv t236 = rt_get(&F_pos, v177_tk); wv t237 = v178_message; rt_chk(w2_failHint(t236, t237, RT_NULL), "failAt"); });
  RT_RET(RT_NULL);
}

static wv w4_simpleStatement(wv v179_k, wv v180_e) {
  rt_enter("simpleStatement");
  v179_k = rt_bind(v179_k, 0);
  v180_e = rt_bind(v180_e, 0);
  RT_RET(rt_rec(0, 5, K5, (wv[]){ v179_k, v180_e, RT_NULL, RT_NULL, RT_NULL }));
}

static wv w4_parse(wv v181_toks) {
  rt_enter("parse");
  RT_RET(rt_chk(w4_parser(v181_toks, rt_b(0)), "parse"));
}

static wv w4_hasEffect(wv v182_e) {
  rt_enter("hasEffect");
  v182_e = rt_bind(v182_e, 0);
  wv v183_kind = rt_bind(rt_get(&F_k, v182_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v183_kind, rt_s(&S182)), "||") || rt_bool(rt_eq(v183_kind, rt_s(&S183)), "||")), "||") || rt_bool(rt_eq(v183_kind, rt_s(&S184)), "||")), "||") || rt_bool(rt_eq(v183_kind, rt_s(&S44)), "||")))) {
    RT_RET(rt_b(1));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S171)))) {
    wv v184_i = rt_i(0);
    while (rt_cond(({ wv t238 = v184_i; wv t239 = std_length(rt_get(&F_parts, v182_e)); rt_lt(t238, t239); }))) {
      if (rt_cond(rt_b(rt_bool(rt_ne(std_typeOf(({ wv t240 = v184_i; wv t241 = rt_get(&F_parts, v182_e); rt_idx(t240, t241); })), rt_s(&S185)), "&&") && rt_bool(rt_chk(w4_hasEffect(({ wv t242 = v184_i; wv t243 = rt_get(&F_parts, v182_e); rt_idx(t242, t243); })), "hasEffect"), "&&")))) {
        RT_RET(rt_b(1));
      }
      v184_i = rt_add(v184_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S186)))) {
    wv v185_i = rt_i(0);
    while (rt_cond(({ wv t244 = v185_i; wv t245 = std_length(rt_get(&F_items, v182_e)); rt_lt(t244, t245); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(({ wv t246 = v185_i; wv t247 = rt_get(&F_items, v182_e); rt_idx(t246, t247); })), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v185_i = rt_add(v185_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S187)))) {
    wv v186_i = rt_i(0);
    while (rt_cond(({ wv t248 = v186_i; wv t249 = std_length(rt_get(&F_fields, v182_e)); rt_lt(t248, t249); }))) {
      if (rt_cond(rt_chk(w4_hasEffect(rt_get(&F_e, ({ wv t250 = v186_i; wv t251 = rt_get(&F_fields, v182_e); rt_idx(t250, t251); }))), "hasEffect"))) {
        RT_RET(rt_b(1));
      }
      v186_i = rt_add(v186_i, rt_i(1));
    }
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S188)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_index, v182_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_arr, v182_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S189)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_rec, v182_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S190)))) {
    RT_RET(rt_chk(w4_hasEffect(rt_get(&F_e, v182_e)), "hasEffect"));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S191)))) {
    RT_RET(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_l, v182_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_r, v182_e)), "hasEffect"), "||")));
  }
  if (rt_cond(rt_eq(v183_kind, rt_s(&S192)))) {
    RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_chk(w4_hasEffect(rt_get(&F_c, v182_e)), "hasEffect"), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_a, v182_e)), "hasEffect"), "||")), "||") || rt_bool(rt_chk(w4_hasEffect(rt_get(&F_b, v182_e)), "hasEffect"), "||")));
  }
  RT_RET(rt_b(0));
}

static wv w4_parser(wv p_v187_toks, wv v188_inner) {
  rt_enter("parser");
  struct fr50 fr = { 0 };
  fr.v187_toks = p_v187_toks;
  v188_inner = rt_bind(v188_inner, 0);
  fr.v189_p = rt_i(0);
  fr.v190_depth = rt_i(0);
  fr.v191_exports = rt_arr(0, NULL, 1);
  fr.v192_firstExport = RT_NULL;
  if (rt_cond(v188_inner)) {
    wv v358_e = rt_bind(rt_chk(s216_expr(&fr), "parser"), 0);
    if (rt_cond(rt_not(rt_chk(s194_at(&fr, rt_s(&S110)), "parser")))) {
      wv v359_tk = rt_bind(rt_chk(s193_peek(&fr), "parser"), 0);
      (void)({ wv t532 = v359_tk; wv t533 = rt_cat(3, (wv[]){ rt_s(&S322), rt_text(rt_chk(w4_describe(v359_tk), "parser")), rt_s(&S323) }); rt_chk(w4_failAt(t532, t533), "parser"); });
    }
    RT_RET(v358_e);
  }
  RT_RET(rt_chk(s202_program(&fr), "parser"));
}

static wv s193_peek(struct fr50 *up) {
  rt_enter("peek");
  RT_RET(({ wv t252 = rt_def(up->v189_p, "p"); wv t253 = up->v187_toks; rt_idx(t252, t253); }));
}

static wv s194_at(struct fr50 *up, wv v230_type) {
  rt_enter("at");
  v230_type = rt_bind(v230_type, 0);
  RT_RET(({ wv t256 = rt_get(&F_t, ({ wv t254 = rt_def(up->v189_p, "p"); wv t255 = up->v187_toks; rt_idx(t254, t255); })); wv t257 = v230_type; rt_eq(t256, t257); }));
}

static wv s195_atWord(struct fr50 *up, wv v231_word) {
  rt_enter("atWord");
  v231_word = rt_bind(v231_word, 0);
  RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t258 = rt_def(up->v189_p, "p"); wv t259 = up->v187_toks; rt_idx(t258, t259); })), rt_s(&S137)), "&&") && rt_bool(({ wv t262 = rt_get(&F_v, ({ wv t260 = rt_def(up->v189_p, "p"); wv t261 = up->v187_toks; rt_idx(t260, t261); })); wv t263 = v231_word; rt_eq(t262, t263); }), "&&")));
}

static wv s196_atCall(struct fr50 *up) {
  rt_enter("atCall");
  if (rt_cond(rt_b(rt_bool(({ wv t264 = rt_add(rt_def(up->v189_p, "p"), rt_i(2)); wv t265 = std_length(up->v187_toks); rt_ge(t264, t265); }), "||") || rt_bool(rt_ne(rt_get(&F_t, ({ wv t266 = rt_def(up->v189_p, "p"); wv t267 = up->v187_toks; rt_idx(t266, t267); })), rt_s(&S97)), "||")))) {
    RT_RET(rt_b(0));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, ({ wv t268 = rt_add(rt_def(up->v189_p, "p"), rt_i(2)); wv t269 = up->v187_toks; rt_idx(t268, t269); })), rt_s(&S16)), "&&") && rt_bool(({ wv t272 = rt_def(w4_callees, "callees"); wv t273 = rt_get(&F_t, ({ wv t270 = rt_add(rt_def(up->v189_p, "p"), rt_i(1)); wv t271 = up->v187_toks; rt_idx(t270, t271); })); rt_chk(w1_contains(t272, t273), "atCall"); }), "&&")))) {
    RT_RET(rt_b(1));
  }
  RT_RET(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(({ wv t274 = rt_add(rt_def(up->v189_p, "p"), rt_i(4)); wv t275 = std_length(up->v187_toks); rt_lt(t274, t275); }), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t276 = rt_add(rt_def(up->v189_p, "p"), rt_i(1)); wv t277 = up->v187_toks; rt_idx(t276, t277); })), rt_s(&S137)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t278 = rt_add(rt_def(up->v189_p, "p"), rt_i(2)); wv t279 = up->v187_toks; rt_idx(t278, t279); })), rt_s(&S129)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t280 = rt_add(rt_def(up->v189_p, "p"), rt_i(3)); wv t281 = up->v187_toks; rt_idx(t280, t281); })), rt_s(&S137)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_t, ({ wv t282 = rt_add(rt_def(up->v189_p, "p"), rt_i(4)); wv t283 = up->v187_toks; rt_idx(t282, t283); })), rt_s(&S16)), "&&")));
}

static wv s197_next(struct fr50 *up) {
  rt_enter("next");
  wv v232_tk = rt_bind(({ wv t284 = rt_def(up->v189_p, "p"); wv t285 = up->v187_toks; rt_idx(t284, t285); }), 0);
  if (rt_cond(rt_ne(rt_get(&F_t, v232_tk), rt_s(&S110)))) {
    up->v189_p = rt_add(rt_def(up->v189_p, "p"), rt_i(1));
  }
  RT_RET(v232_tk);
}

static wv s198_accept(struct fr50 *up, wv v233_type) {
  rt_enter("accept");
  v233_type = rt_bind(v233_type, 0);
  RT_RET((rt_cond(rt_chk(s194_at(up, v233_type), "accept")) ? rt_chk(s197_next(up), "accept") : RT_NULL));
}

static wv s199_expect(struct fr50 *up, wv v234_type, wv v235_msg, wv v236_hint) {
  rt_enter("expect");
  v234_type = rt_bind(v234_type, 0);
  v235_msg = rt_bind(v235_msg, 0);
  v236_hint = rt_bind(v236_hint, 0);
  if (rt_cond(rt_chk(s194_at(up, v234_type), "expect"))) {
    RT_RET(rt_chk(s197_next(up), "expect"));
  }
  wv v237_found = rt_bind(rt_chk(s193_peek(up), "expect"), 0);
  wv v238_m = rt_bind((rt_cond(rt_eq(v235_msg, RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S193), rt_text(v234_type), rt_s(&S113) }) : v235_msg), 0);
  (void)({ wv t288 = v237_found; wv t289 = ({ wv t286 = rt_text(v238_m); wv t287 = rt_text(rt_chk(w4_describe(v237_found), "expect")); rt_cat(3, (wv[]){ t286, rt_s(&S194), t287 }); }); wv t290 = v236_hint; rt_chk(w4_failAtHint(t288, t289, t290), "expect"); });
  RT_RET(RT_NULL);
}

static wv s200_expectWord(struct fr50 *up, wv v239_word, wv v240_hint) {
  rt_enter("expectWord");
  v239_word = rt_bind(v239_word, 0);
  v240_hint = rt_bind(v240_hint, 0);
  if (rt_cond(rt_chk(s195_atWord(up, v239_word), "expectWord"))) {
    RT_RET(rt_chk(s197_next(up), "expectWord"));
  }
  wv v241_found = rt_bind(rt_chk(s193_peek(up), "expectWord"), 0);
  (void)({ wv t293 = v241_found; wv t294 = ({ wv t291 = rt_text(v239_word); wv t292 = rt_text(rt_chk(w4_describe(v241_found), "expectWord")); rt_cat(4, (wv[]){ rt_s(&S193), t291, rt_s(&S195), t292 }); }); wv t295 = v240_hint; rt_chk(w4_failAtHint(t293, t294, t295), "expectWord"); });
  RT_RET(RT_NULL);
}

static wv s201_lintSpace(struct fr50 *up, wv v242_op) {
  rt_enter("lintSpace");
  v242_op = rt_bind(v242_op, 0);
  if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_sp, v242_op)), "||") || rt_bool(rt_not(rt_get(&F_sp, rt_chk(s193_peek(up), "lintSpace"))), "||")))) {
    (void)({ wv t298 = w2_warnings; wv t299 = ({ wv t296 = rt_get(&F_pos, v242_op); wv t297 = rt_cat(3, (wv[]){ rt_s(&S196), rt_text(rt_get(&F_t, v242_op)), rt_s(&S113) }); rt_chk(w2_warn(t296, t297, RT_NULL), "lintSpace"); }); std_push(t298, t299); });
  }
  RT_RET(RT_NULL);
}

static wv s202_program(struct fr50 *up) {
  rt_enter("program");
  wv v243_stmts = rt_arr(0, NULL, 1);
  wv v244_started = rt_b(0);
  wv v245_ended = RT_NULL;
  while (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S110)), "program")))) {
    if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S14)), "program")))) {
      wv v247_tk = rt_bind(rt_chk(s193_peek(up), "program"), 0);
      wv v248_hint = rt_bind((rt_cond(rt_chk(s194_at(up, rt_s(&S97)), "program")) ? rt_s(&S197) : rt_s(&S198)), 0);
      (void)({ wv t300 = v247_tk; wv t301 = rt_cat(2, (wv[]){ rt_s(&S199), rt_text(rt_chk(w4_describe(v247_tk), "program")) }); wv t302 = v248_hint; rt_chk(w4_failAtHint(t300, t301, t302), "program"); });
    }
    wv v246_s = rt_bind(rt_chk(s203_statement(up), "program"), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v245_ended, RT_NULL), "&&") && rt_bool(rt_ne(rt_get(&F_k, v246_s), rt_s(&S200)), "&&")))) {
      (void)rt_chk(w4_failAt(v245_ended, rt_s(&S201)), "program");
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v246_s), rt_s(&S47)), "&&") && rt_bool(v244_started, "&&")))) {
      (void)rt_chk(w2_abort(rt_get(&F_pos, v246_s), rt_s(&S202)), "program");
    }
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, v246_s), rt_s(&S47)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v246_s), rt_s(&S200)), "&&")))) {
      v244_started = rt_b(1);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, v246_s), rt_s(&S203)))) {
      v245_ended = rt_rec(1, 1, K6, (wv[]){ rt_get(&F_pos, v246_s) });
    }
    (void)std_push(v243_stmts, v246_s);
  }
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_def(up->v192_firstExport, "firstExport"), RT_NULL), "&&") && rt_bool(rt_eq(v245_ended, RT_NULL), "&&")))) {
    (void)rt_chk(w4_failAt(rt_def(up->v192_firstExport, "firstExport"), rt_s(&S204)), "program");
  }
  RT_RET(({ wv t303 = v243_stmts; wv t304 = rt_get(&F_pos, rt_chk(s193_peek(up), "program")); rt_rec(0, 2, K7, (wv[]){ t303, t304 }); }));
}

static wv s203_statement(struct fr50 *up) {
  rt_enter("statement");
  wv v249_semi = rt_bind(rt_chk(s197_next(up), "statement"), 0);
  wv v250_tk = rt_bind(rt_chk(s193_peek(up), "statement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v250_tk), rt_s(&S14)), "||") || rt_bool(rt_eq(rt_get(&F_t, v250_tk), rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v250_tk), rt_s(&S110)), "||")))) {
    RT_RET(({ wv t305 = rt_get(&F_pos, v249_semi); wv t306 = rt_get(&F_pos, v250_tk); rt_rec(0, 4, K8, (wv[]){ rt_s(&S200), t305, t306, RT_NULL }); }));
  }
  wv v251_s = rt_bind(rt_chk(s204_statementBody(up, v250_tk), "statement"), 1);
  ({ wv t307 = rt_get(&F_pos, v250_tk); wv t308 = v251_s; rt_set_mem(t307, &F_pos, t308); });
  if (rt_cond(rt_chk(s194_at(up, rt_s(&S56)), "statement"))) {
    wv v252_w = rt_bind(rt_chk(s197_next(up), "statement"), 0);
    if (rt_cond(rt_not(({ wv t309 = rt_def(w4_onfailOk, "onfailOk"); wv t310 = rt_get(&F_k, v251_s); rt_chk(w1_contains(t309, t310), "statement"); })))) {
      (void)rt_chk(w4_failAt(v252_w, rt_s(&S205)), "statement");
    }
    wv v253_param = RT_NULL;
    if (rt_cond(rt_ne(rt_chk(s198_accept(up, rt_s(&S97)), "statement"), RT_NULL))) {
      v253_param = rt_bind(rt_chk(s199_expect(up, rt_s(&S137), rt_s(&S206), RT_NULL), "statement"), 1);
      (void)rt_chk(s199_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "statement");
    }
    wv v254_noParam = rt_eq(v253_param, RT_NULL);
    ({ wv t315 = ({ wv t311 = rt_get(&F_pos, v252_w); wv t312 = (rt_cond(v254_noParam) ? RT_NULL : rt_get(&F_v, v253_param)); wv t313 = (rt_cond(v254_noParam) ? RT_NULL : rt_get(&F_pos, v253_param)); wv t314 = rt_chk(s214_block(up), "statement"); rt_rec(0, 4, K9, (wv[]){ t311, t312, t313, t314 }); }); wv t316 = v251_s; rt_set_mem(t315, &F_handler, t316); });
  }
  ({ wv t317 = rt_get(&F_pos, rt_chk(s193_peek(up), "statement")); wv t318 = v251_s; rt_set_mem(t317, &F_end, t318); });
  RT_RET(v251_s);
}

static wv s204_statementBody(struct fr50 *up, wv v255_tk) {
  rt_enter("statementBody");
  v255_tk = rt_bind(v255_tk, 0);
  wv v256_kind = rt_bind(rt_get(&F_t, v255_tk), 0);
  if (rt_cond(rt_eq(v256_kind, rt_s(&S33)))) {
    RT_RET(rt_chk(s211_fnStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S46)))) {
    RT_RET(rt_chk(s212_enumStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S47)))) {
    RT_RET(rt_chk(s208_importStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S48)))) {
    RT_RET(rt_chk(s209_exportStatement(up), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S37)), "||")))) {
    RT_RET(rt_chk(s205_declaration(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S38)))) {
    RT_RET(rt_chk(s206_setStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S34)))) {
    (void)rt_chk(s197_next(up), "statementBody");
    wv v258_e = rt_bind((rt_cond(({ wv t319 = rt_def(w4_exprStart, "exprStart"); wv t320 = rt_get(&F_t, rt_chk(s193_peek(up), "statementBody")); rt_chk(w1_contains(t319, t320), "statementBody"); })) ? rt_chk(s216_expr(up), "statementBody") : RT_NULL), 0);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S34), v258_e), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S57)))) {
    (void)rt_chk(s197_next(up), "statementBody");
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S57), rt_chk(s216_expr(up), "statementBody")), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S43)), "||")))) {
    (void)rt_chk(s197_next(up), "statementBody");
    RT_RET(rt_rec(0, 4, K8, (wv[]){ (rt_cond(rt_eq(v256_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43)), RT_NULL, RT_NULL, RT_NULL }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S41)), "||") || rt_bool(rt_eq(v256_kind, rt_s(&S207)), "||")))) {
    (void)rt_chk(s197_next(up), "statementBody");
    RT_RET(({ wv t321 = rt_eq(v256_kind, rt_s(&S207)); wv t322 = rt_chk(s213_cond(up, v256_kind), "statementBody"); wv t323 = rt_chk(s214_block(up), "statementBody"); rt_rec(0, 7, K10, (wv[]){ rt_s(&S41), t321, t322, t323, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S55)))) {
    RT_RET(rt_chk(s207_failStatement(up), "statementBody"));
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S54)))) {
    (void)rt_chk(s197_next(up), "statementBody");
    wv v259_e = rt_bind((rt_cond(({ wv t324 = rt_def(w4_exprStart, "exprStart"); wv t325 = rt_get(&F_t, rt_chk(s193_peek(up), "statementBody")); rt_chk(w1_contains(t324, t325), "statementBody"); })) ? rt_chk(s216_expr(up), "statementBody") : RT_NULL), 0);
    RT_RET(rt_chk(w4_simpleStatement(rt_s(&S54), v259_e), "statementBody"));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v256_kind, rt_s(&S137)), "&&") && rt_bool(rt_eq(rt_get(&F_v, v255_tk), rt_s(&S203)), "&&")))) {
    wv v260_after = rt_bind(rt_get(&F_t, ({ wv t326 = rt_add(rt_def(up->v189_p, "p"), rt_i(1)); wv t327 = up->v187_toks; rt_idx(t326, t327); })), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v260_after, rt_s(&S14)), "||") || rt_bool(rt_eq(v260_after, rt_s(&S15)), "||")), "||") || rt_bool(rt_eq(v260_after, rt_s(&S110)), "||")))) {
      RT_RET(rt_chk(s210_endStatement(up), "statementBody"));
    }
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S56)))) {
    (void)rt_chk(w4_failAt(v255_tk, rt_s(&S205)), "statementBody");
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S40)))) {
    (void)rt_chk(w4_failAt(v255_tk, rt_s(&S208)), "statementBody");
  }
  if (rt_cond(rt_eq(v256_kind, rt_s(&S45)))) {
    (void)rt_chk(w4_failAt(v255_tk, rt_s(&S209)), "statementBody");
  }
  wv v257_e = rt_bind(rt_chk(s216_expr(up), "statementBody"), 0);
  if (rt_cond(rt_b(rt_bool(rt_chk(s194_at(up, rt_s(&S111)), "statementBody"), "||") || rt_bool(rt_b(rt_bool(rt_chk(s195_atWord(up, rt_s(&S210)), "statementBody"), "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v257_e), rt_s(&S211)), "||") || rt_bool(rt_eq(rt_get(&F_k, v257_e), rt_s(&S188)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_k, v257_e), rt_s(&S189)), "||")), "&&")), "||")))) {
    (void)rt_chk(w4_failAtHint(v255_tk, rt_s(&S212), rt_s(&S213)), "statementBody");
  }
  if (rt_cond(rt_not(rt_chk(w4_hasEffect(v257_e), "statementBody")))) {
    wv v261_hint = rt_bind((rt_cond(rt_gt(rt_def(up->v190_depth, "depth"), rt_i(0))) ? rt_s(&S214) : RT_NULL), 0);
    (void)({ wv t330 = w2_warnings; wv t331 = ({ wv t328 = rt_get(&F_pos, v255_tk); wv t329 = v261_hint; rt_chk(w2_warn(t328, rt_s(&S215), t329), "statementBody"); }); std_push(t330, t331); });
  }
  RT_RET(rt_chk(w4_simpleStatement(rt_s(&S176), v257_e), "statementBody"));
}

static wv s205_declaration(struct fr50 *up) {
  rt_enter("declaration");
  wv v262_kw = rt_bind(rt_chk(s197_next(up), "declaration"), 0);
  wv v263_name = rt_bind(({ wv t332 = rt_cat(3, (wv[]){ rt_s(&S216), rt_text(rt_get(&F_t, v262_kw)), rt_s(&S113) }); wv t333 = rt_cat(3, (wv[]){ rt_s(&S14), rt_text(rt_get(&F_t, v262_kw)), rt_s(&S217) }); rt_chk(s199_expect(up, rt_s(&S137), t332, t333), "declaration"); }), 0);
  (void)rt_chk(s200_expectWord(up, rt_s(&S218), ({ wv t334 = rt_text(rt_get(&F_t, v262_kw)); wv t335 = rt_text(rt_get(&F_v, v263_name)); rt_cat(5, (wv[]){ rt_s(&S14), t334, rt_s(&S0), t335, rt_s(&S219) }); })), "declaration");
  wv v264_e = rt_bind(rt_chk(s216_expr(up), "declaration"), 0);
  RT_RET(({ wv t336 = v264_e; wv t337 = rt_eq(rt_get(&F_t, v262_kw), rt_s(&S36)); wv t338 = rt_eq(rt_get(&F_t, v262_kw), rt_s(&S37)); wv t339 = rt_get(&F_v, v263_name); wv t340 = rt_get(&F_pos, v263_name); rt_rec(0, 9, K11, (wv[]){ rt_s(&S177), t336, t337, t338, t339, t340, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s206_setStatement(struct fr50 *up) {
  rt_enter("setStatement");
  (void)rt_chk(s197_next(up), "setStatement");
  wv v265_target = rt_bind(rt_chk(s215_lvalue(up), "setStatement"), 0);
  if (rt_cond(rt_chk(s195_atWord(up, rt_s(&S210)), "setStatement"))) {
    (void)rt_chk(s197_next(up), "setStatement");
    RT_RET(({ wv t341 = rt_chk(s216_expr(up), "setStatement"); wv t342 = v265_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t341, RT_NULL, t342, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  if (rt_cond(rt_chk(s194_at(up, rt_s(&S111)), "setStatement"))) {
    wv v267_op = rt_bind(rt_chk(s197_next(up), "setStatement"), 0);
    RT_RET(({ wv t343 = rt_chk(s216_expr(up), "setStatement"); wv t344 = rt_get(&F_v, v267_op); wv t345 = v265_target; rt_rec(0, 7, K12, (wv[]){ rt_s(&S38), t343, t344, t345, RT_NULL, RT_NULL, RT_NULL }); }));
  }
  wv v266_found = rt_bind(rt_chk(s193_peek(up), "setStatement"), 0);
  (void)({ wv t346 = v266_found; wv t347 = rt_cat(2, (wv[]){ rt_s(&S220), rt_text(rt_chk(w4_describe(v266_found), "setStatement")) }); rt_chk(w4_failAtHint(t346, t347, rt_s(&S213)), "setStatement"); });
  RT_RET(RT_NULL);
}

static wv s207_failStatement(struct fr50 *up) {
  rt_enter("failStatement");
  wv v268_kw = rt_bind(rt_chk(s197_next(up), "failStatement"), 0);
  if (rt_cond(rt_not(({ wv t348 = rt_def(w4_exprStart, "exprStart"); wv t349 = rt_get(&F_t, rt_chk(s193_peek(up), "failStatement")); rt_chk(w1_contains(t348, t349), "failStatement"); })))) {
    RT_RET(rt_rec(0, 5, K13, (wv[]){ rt_s(&S55), rt_i(1), RT_NULL, RT_NULL, RT_NULL }));
  }
  wv v269_first = rt_bind(rt_chk(s193_peek(up), "failStatement"), 0);
  wv v270_e = rt_bind(rt_chk(s216_expr(up), "failStatement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v270_e), rt_s(&S148)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v270_e), rt_s(&S138)), "&&") && rt_bool(rt_lt(rt_get(&F_v, v270_e), rt_i(1)), "&&")), "||")))) {
    (void)rt_chk(w4_failAt(v269_first, rt_s(&S221)), "failStatement");
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v270_e), rt_s(&S138)))) {
    RT_RET(rt_rec(0, 5, K13, (wv[]){ rt_s(&S55), rt_get(&F_v, v270_e), RT_NULL, RT_NULL, RT_NULL }));
  }
  RT_RET(rt_chk(w4_simpleStatement(rt_s(&S176), ({ wv t350 = v270_e; wv t351 = rt_get(&F_pos, v268_kw); rt_rec(0, 4, K14, (wv[]){ rt_s(&S183), t350, t351, rt_b(0) }); })), "failStatement"));
}

static wv s208_importStatement(struct fr50 *up) {
  rt_enter("importStatement");
  wv v271_kw = rt_bind(rt_chk(s197_next(up), "importStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v190_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v271_kw, rt_s(&S222)), "importStatement");
  }
  wv v272_name = rt_bind(rt_chk(s199_expect(up, rt_s(&S137), rt_s(&S223), rt_s(&S224)), "importStatement"), 0);
  (void)rt_chk(s200_expectWord(up, rt_s(&S225), ({ wv t352 = rt_text(rt_get(&F_v, v272_name)); wv t353 = rt_text(rt_get(&F_v, v272_name)); rt_cat(5, (wv[]){ rt_s(&S226), t352, rt_s(&S227), t353, rt_s(&S228) }); })), "importStatement");
  wv v273_e = rt_bind(rt_chk(s193_peek(up), "importStatement"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_t, v273_e), rt_s(&S171)), "||") || rt_bool(rt_ne(std_length(rt_get(&F_parts, v273_e)), rt_i(1)), "||")))) {
    (void)rt_chk(w4_failAtHint(v273_e, rt_s(&S229), rt_s(&S224)), "importStatement");
  }
  (void)rt_chk(s197_next(up), "importStatement");
  wv v274_path = rt_bind(rt_idx(rt_i(0), rt_get(&F_parts, v273_e)), 0);
  if (rt_cond(rt_chk(w1_contains(std_split(v274_path, rt_s(&S13)), rt_s(&S19)), "importStatement"))) {
    (void)rt_chk(w4_failAt(v273_e, rt_s(&S230)), "importStatement");
  }
  if (rt_cond(rt_not(rt_b(rt_bool(rt_ge(std_length(v274_path), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t354 = v274_path; wv t355 = std_length(v274_path); std_slice(t354, rt_i(-4), t355); }), rt_s(&S231)), "&&"))))) {
    (void)rt_chk(w4_failAt(v273_e, rt_s(&S232)), "importStatement");
  }
  RT_RET(({ wv t356 = v274_path; wv t357 = rt_get(&F_pos, v273_e); wv t358 = rt_get(&F_v, v272_name); wv t359 = rt_get(&F_pos, v272_name); rt_rec(0, 8, K15, (wv[]){ rt_s(&S47), t356, t357, t358, t359, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s209_exportStatement(struct fr50 *up) {
  rt_enter("exportStatement");
  wv v275_kw = rt_bind(rt_chk(s197_next(up), "exportStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v190_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v275_kw, rt_s(&S233)), "exportStatement");
  }
  wv v276_tk = rt_bind(rt_chk(s193_peek(up), "exportStatement"), 0);
  if (rt_cond(rt_not(({ wv t360 = rt_def(w4_exportable, "exportable"); wv t361 = rt_get(&F_t, v276_tk); rt_chk(w1_contains(t360, t361), "exportStatement"); })))) {
    (void)({ wv t362 = v276_tk; wv t363 = rt_cat(2, (wv[]){ rt_s(&S234), rt_text(rt_chk(w4_describe(v276_tk), "exportStatement")) }); rt_chk(w4_failAt(t362, t363), "exportStatement"); });
  }
  wv v277_s = rt_bind(rt_chk(s204_statementBody(up, v276_tk), "exportStatement"), 0);
  if (rt_cond(rt_eq(rt_def(up->v192_firstExport, "firstExport"), RT_NULL))) {
    wv t364 = rt_bind(v275_kw, 1);
    (void)rt_def(up->v192_firstExport, "firstExport");
    up->v192_firstExport = t364;
  }
  (void)({ wv t367 = rt_def(up->v191_exports, "exports"); wv t368 = ({ wv t365 = rt_get(&F_name, v277_s); wv t366 = rt_get(&F_namePos, v277_s); rt_rec(0, 2, K16, (wv[]){ t365, t366 }); }); std_push(t367, t368); });
  RT_RET(v277_s);
}

static wv s210_endStatement(struct fr50 *up) {
  rt_enter("endStatement");
  wv v278_kw = rt_bind(rt_chk(s197_next(up), "endStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v190_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v278_kw, rt_s(&S235)), "endStatement");
  }
  if (rt_cond(rt_eq(std_length(rt_def(up->v191_exports, "exports")), rt_i(0)))) {
    (void)rt_chk(w4_failAtHint(v278_kw, rt_s(&S236), rt_s(&S237)), "endStatement");
  }
  RT_RET(rt_rec(0, 5, K17, (wv[]){ rt_s(&S203), rt_def(up->v191_exports, "exports"), RT_NULL, RT_NULL, RT_NULL }));
}

static wv s211_fnStatement(struct fr50 *up) {
  rt_enter("fnStatement");
  (void)rt_chk(s197_next(up), "fnStatement");
  if (rt_cond(rt_chk(s194_at(up, rt_s(&S97)), "fnStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s193_peek(up), "fnStatement"), rt_s(&S238), rt_s(&S239)), "fnStatement");
  }
  wv v279_name = rt_bind(rt_chk(s199_expect(up, rt_s(&S137), rt_s(&S240), rt_s(&S239)), "fnStatement"), 0);
  (void)rt_chk(s199_expect(up, rt_s(&S97), rt_s(&S241), rt_cat(3, (wv[]){ rt_s(&S242), rt_text(rt_get(&F_v, v279_name)), rt_s(&S243) })), "fnStatement");
  wv v280_params = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S16)), "fnStatement")))) {
    while (rt_cond(rt_b(1))) {
      wv v281_isVar = rt_ne(rt_chk(s198_accept(up, rt_s(&S36)), "fnStatement"), RT_NULL);
      wv v282_isRef = rt_b(rt_bool(rt_not(v281_isVar), "&&") && rt_bool(rt_ne(rt_chk(s198_accept(up, rt_s(&S37)), "fnStatement"), RT_NULL), "&&"));
      wv v283_pname = rt_bind(rt_chk(s199_expect(up, rt_s(&S137), rt_s(&S244), rt_s(&S245)), "fnStatement"), 0);
      (void)({ wv t373 = v280_params; wv t374 = ({ wv t369 = rt_get(&F_v, v283_pname); wv t370 = v281_isVar; wv t371 = v282_isRef; wv t372 = rt_get(&F_pos, v283_pname); rt_rec(0, 4, K18, (wv[]){ t369, t370, t371, t372 }); }); std_push(t373, t374); });
      if (rt_cond(rt_eq(rt_chk(s198_accept(up, rt_s(&S96)), "fnStatement"), RT_NULL))) {
        break;
      }
    }
  }
  (void)rt_chk(s199_expect(up, rt_s(&S16), rt_s(&S246), RT_NULL), "fnStatement");
  RT_RET(({ wv t375 = rt_get(&F_v, v279_name); wv t376 = rt_get(&F_pos, v279_name); wv t377 = v280_params; wv t378 = rt_chk(s214_block(up), "fnStatement"); rt_rec(0, 8, K19, (wv[]){ rt_s(&S33), t375, t376, t377, t378, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s212_enumStatement(struct fr50 *up) {
  rt_enter("enumStatement");
  wv v284_kw = rt_bind(rt_chk(s197_next(up), "enumStatement"), 0);
  if (rt_cond(rt_gt(rt_def(up->v190_depth, "depth"), rt_i(0)))) {
    (void)rt_chk(w4_failAt(v284_kw, rt_s(&S247)), "enumStatement");
  }
  if (rt_cond(rt_chk(s194_at(up, rt_s(&S97)), "enumStatement"))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s193_peek(up), "enumStatement"), rt_s(&S248), rt_s(&S249)), "enumStatement");
  }
  wv v285_name = rt_bind(rt_chk(s199_expect(up, rt_s(&S137), rt_s(&S250), rt_s(&S249)), "enumStatement"), 0);
  (void)rt_chk(s199_expect(up, rt_s(&S97), rt_s(&S251), rt_cat(3, (wv[]){ rt_s(&S252), rt_text(rt_get(&F_v, v285_name)), rt_s(&S253) })), "enumStatement");
  if (rt_cond(rt_chk(s194_at(up, rt_s(&S16)), "enumStatement"))) {
    (void)({ wv t379 = rt_chk(s193_peek(up), "enumStatement"); wv t380 = rt_cat(3, (wv[]){ rt_s(&S252), rt_text(rt_get(&F_v, v285_name)), rt_s(&S253) }); rt_chk(w4_failAtHint(t379, rt_s(&S254), t380), "enumStatement"); });
  }
  wv v286_members = rt_arr(0, NULL, 1);
  while (rt_cond(rt_b(1))) {
    wv v287_m = rt_bind(rt_chk(s193_peek(up), "enumStatement"), 0);
    if (rt_cond(rt_ne(rt_get(&F_t, v287_m), rt_s(&S137)))) {
      wv v289_message = rt_bind((rt_cond(({ wv t381 = w3_keywords; wv t382 = rt_get(&F_t, v287_m); rt_chk(w1_contains(t381, t382), "enumStatement"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v287_m)), rt_s(&S255) }) : rt_cat(2, (wv[]){ rt_s(&S256), rt_text(rt_chk(w4_describe(v287_m), "enumStatement")) })), 0);
      (void)rt_chk(w4_failAt(v287_m, v289_message), "enumStatement");
    }
    (void)rt_chk(s197_next(up), "enumStatement");
    wv v288_k = rt_i(0);
    while (rt_cond(({ wv t383 = v288_k; wv t384 = std_length(v286_members); rt_lt(t383, t384); }))) {
      if (rt_cond(({ wv t385 = rt_get(&F_name, rt_idx(v288_k, v286_members)); wv t386 = rt_get(&F_v, v287_m); rt_eq(t385, t386); }))) {
        (void)({ wv t387 = v287_m; wv t388 = rt_cat(3, (wv[]){ rt_s(&S257), rt_text(rt_get(&F_v, v287_m)), rt_s(&S258) }); rt_chk(w4_failAt(t387, t388), "enumStatement"); });
      }
      v288_k = rt_add(v288_k, rt_i(1));
    }
    (void)({ wv t391 = v286_members; wv t392 = ({ wv t389 = rt_get(&F_v, v287_m); wv t390 = rt_get(&F_pos, v287_m); rt_rec(0, 2, K16, (wv[]){ t389, t390 }); }); std_push(t391, t392); });
    if (rt_cond(rt_eq(rt_chk(s198_accept(up, rt_s(&S96)), "enumStatement"), RT_NULL))) {
      break;
    }
  }
  (void)rt_chk(s199_expect(up, rt_s(&S16), rt_s(&S259), RT_NULL), "enumStatement");
  RT_RET(({ wv t393 = rt_get(&F_v, v285_name); wv t394 = rt_get(&F_pos, v285_name); wv t395 = v286_members; rt_rec(0, 7, K20, (wv[]){ rt_s(&S46), t393, t394, t395, RT_NULL, RT_NULL, RT_NULL }); }));
}

static wv s213_cond(struct fr50 *up, wv v290_kw) {
  rt_enter("cond");
  v290_kw = rt_bind(v290_kw, 0);
  (void)({ wv t396 = rt_cat(3, (wv[]){ rt_s(&S260), rt_text(v290_kw), rt_s(&S113) }); wv t397 = rt_cat(3, (wv[]){ rt_s(&S261), rt_text(v290_kw), rt_s(&S262) }); rt_chk(s199_expect(up, rt_s(&S97), t396, t397), "cond"); });
  wv v291_e = rt_bind(rt_chk(s216_expr(up), "cond"), 0);
  (void)rt_chk(s199_expect(up, rt_s(&S16), rt_s(&S263), RT_NULL), "cond");
  RT_RET(v291_e);
}

static wv s214_block(struct fr50 *up) {
  rt_enter("block");
  (void)rt_chk(s199_expect(up, rt_s(&S100), rt_s(&S264), RT_NULL), "block");
  up->v190_depth = rt_add(rt_def(up->v190_depth, "depth"), rt_i(1));
  wv v292_stmts = rt_arr(0, NULL, 1);
  while (rt_cond(rt_chk(s194_at(up, rt_s(&S14)), "block"))) {
    (void)({ wv t398 = v292_stmts; wv t399 = rt_chk(s203_statement(up), "block"); std_push(t398, t399); });
  }
  if (rt_cond(rt_chk(s194_at(up, rt_s(&S15)), "block"))) {
    wv v295_close = rt_bind(rt_chk(s197_next(up), "block"), 0);
    up->v190_depth = rt_sub(rt_def(up->v190_depth, "depth"), rt_i(1));
    RT_RET(({ wv t400 = v292_stmts; wv t401 = rt_get(&F_pos, v295_close); rt_rec(0, 3, K21, (wv[]){ t400, RT_NULL, t401 }); }));
  }
  if (rt_cond(rt_chk(s194_at(up, rt_s(&S110)), "block"))) {
    (void)rt_chk(w4_failAt(rt_chk(s193_peek(up), "block"), rt_s(&S265)), "block");
  }
  wv v293_value = rt_bind(rt_chk(s216_expr(up), "block"), 0);
  if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S15)), "block")))) {
    wv v296_hint = rt_bind((rt_cond(rt_chk(s194_at(up, rt_s(&S28)), "block")) ? rt_s(&S266) : rt_s(&S267)), 0);
    wv v297_found = rt_bind(rt_chk(s193_peek(up), "block"), 0);
    (void)({ wv t402 = v297_found; wv t403 = rt_cat(2, (wv[]){ rt_s(&S268), rt_text(rt_chk(w4_describe(v297_found), "block")) }); wv t404 = v296_hint; rt_chk(w4_failAtHint(t402, t403, t404), "block"); });
  }
  wv v294_close = rt_bind(rt_chk(s197_next(up), "block"), 0);
  up->v190_depth = rt_sub(rt_def(up->v190_depth, "depth"), rt_i(1));
  RT_RET(({ wv t405 = v292_stmts; wv t406 = v293_value; wv t407 = rt_get(&F_pos, v294_close); rt_rec(0, 3, K21, (wv[]){ t405, t406, t407 }); }));
}

static wv s215_lvalue(struct fr50 *up) {
  rt_enter("lvalue");
  wv v298_tk = rt_bind(rt_chk(s193_peek(up), "lvalue"), 0);
  wv v299_e = rt_bind(rt_chk(s223_postfix(up), "lvalue"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_k, v299_e), rt_s(&S211)), "&&") && rt_bool(rt_ne(rt_get(&F_k, v299_e), rt_s(&S188)), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_k, v299_e), rt_s(&S189)), "&&")))) {
    (void)rt_chk(w4_failAt(v298_tk, rt_s(&S269)), "lvalue");
  }
  RT_RET(v299_e);
}

static wv s216_expr(struct fr50 *up) {
  rt_enter("expr");
  RT_RET(rt_chk(s217_ternary(up), "expr"));
}

static wv s217_ternary(struct fr50 *up) {
  rt_enter("ternary");
  RT_RET(rt_chk(s218_ternaryTail(up, rt_chk(s219_binary(up, rt_i(1)), "ternary")), "ternary"));
}

static wv s218_ternaryTail(struct fr50 *up, wv v300_c) {
  rt_enter("ternaryTail");
  if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S95)), "ternaryTail")))) {
    RT_RET(v300_c);
  }
  wv v301_q = rt_bind(rt_chk(s197_next(up), "ternaryTail"), 0);
  (void)rt_chk(s201_lintSpace(up, v301_q), "ternaryTail");
  wv v302_a = rt_bind(rt_chk(s217_ternary(up), "ternaryTail"), 0);
  wv v303_colon = rt_bind(rt_chk(s199_expect(up, rt_s(&S28), rt_s(&S270), RT_NULL), "ternaryTail"), 0);
  (void)rt_chk(s201_lintSpace(up, v303_colon), "ternaryTail");
  wv v304_b = rt_bind(rt_chk(s217_ternary(up), "ternaryTail"), 0);
  RT_RET(({ wv t408 = v300_c; wv t409 = v302_a; wv t410 = v304_b; wv t411 = rt_get(&F_pos, v301_q); rt_rec(0, 6, K22, (wv[]){ rt_s(&S192), t408, t409, t410, t411, rt_b(0) }); }));
}

static wv s219_binary(struct fr50 *up, wv v305_min) {
  rt_enter("binary");
  v305_min = rt_bind(v305_min, 0);
  RT_RET(({ wv t412 = rt_chk(s221_unary(up), "binary"); wv t413 = v305_min; rt_chk(s220_binaryFrom(up, t412, t413), "binary"); }));
}

static wv s220_binaryFrom(struct fr50 *up, wv v306_left, wv v307_min) {
  rt_enter("binaryFrom");
  v306_left = rt_bind(v306_left, 0);
  v307_min = rt_bind(v307_min, 0);
  wv v308_acc = v306_left;
  while (rt_cond(rt_b(1))) {
    wv v309_op = rt_bind(rt_chk(s193_peek(up), "binaryFrom"), 0);
    wv v310_prec = rt_bind(rt_chk(w4_precedence(rt_get(&F_t, v309_op)), "binaryFrom"), 0);
    if (rt_cond(rt_lt(v310_prec, v307_min))) {
      RT_RET(v308_acc);
    }
    (void)rt_chk(s197_next(up), "binaryFrom");
    (void)rt_chk(s201_lintSpace(up, v309_op), "binaryFrom");
    wv v311_right = rt_chk(s219_binary(up, rt_add(v310_prec, rt_i(1))), "binaryFrom");
    v308_acc = ({ wv t414 = rt_get(&F_t, v309_op); wv t415 = v308_acc; wv t416 = v311_right; wv t417 = rt_get(&F_pos, v309_op); rt_rec(0, 6, K23, (wv[]){ rt_s(&S191), t414, t415, t416, t417, rt_b(0) }); });
  }
  RT_RET(RT_NULL);
}

static wv s221_unary(struct fr50 *up) {
  rt_enter("unary");
  wv v312_tk = rt_bind(rt_chk(s193_peek(up), "unary"), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S85)), "||") || rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S94)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S93)), "||")))) {
    (void)rt_chk(s197_next(up), "unary");
    wv v313_e = rt_bind(rt_chk(s221_unary(up), "unary"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S85)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v313_e), rt_s(&S191)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v313_e), rt_s(&S75)), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v313_e)), "&&")))) {
      (void)rt_chk(w4_failAtHint(v312_tk, rt_s(&S271), rt_s(&S272)), "unary");
    }
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v312_tk), rt_s(&S85)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v313_e), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_k, v313_e), rt_s(&S148)), "||")), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_paren, v313_e)), "&&")))) {
      RT_RET(({ wv t418 = rt_get(&F_k, v313_e); wv t419 = rt_neg(rt_get(&F_v, v313_e)); wv t420 = rt_get(&F_pos, v312_tk); rt_rec(0, 4, K24, (wv[]){ t418, t419, t420, rt_b(0) }); }));
    }
    RT_RET(({ wv t421 = rt_get(&F_t, v312_tk); wv t422 = v313_e; wv t423 = rt_get(&F_pos, v312_tk); rt_rec(0, 5, K25, (wv[]){ rt_s(&S190), t421, t422, t423, rt_b(0) }); }));
  }
  RT_RET(rt_chk(s222_powerTail(up, rt_chk(s223_postfix(up), "unary")), "unary"));
}

static wv s222_powerTail(struct fr50 *up, wv v314_base) {
  rt_enter("powerTail");
  if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S75)), "powerTail")))) {
    RT_RET(v314_base);
  }
  wv v315_op = rt_bind(rt_chk(s197_next(up), "powerTail"), 0);
  (void)rt_chk(s201_lintSpace(up, v315_op), "powerTail");
  RT_RET(({ wv t424 = v314_base; wv t425 = rt_chk(s221_unary(up), "powerTail"); wv t426 = rt_get(&F_pos, v315_op); rt_rec(0, 6, K23, (wv[]){ rt_s(&S191), rt_s(&S75), t424, t425, t426, rt_b(0) }); }));
}

static wv s223_postfix(struct fr50 *up) {
  rt_enter("postfix");
  wv v316_e = rt_chk(s225_primary(up), "postfix");
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s196_atCall(up), "postfix"))) {
      v316_e = rt_chk(s224_call(up, rt_arr(1, (wv[]){ v316_e }, 0)), "postfix");
    } else if (rt_cond(rt_chk(s194_at(up, rt_s(&S98)), "postfix"))) {
      wv v317_open = rt_bind(rt_chk(s197_next(up), "postfix"), 0);
      if (rt_cond(rt_b(rt_bool(rt_get(&F_nl, v317_open), "&&") && rt_bool(rt_gt(rt_def(up->v190_depth, "depth"), rt_i(0)), "&&")))) {
        (void)({ wv t427 = w2_warnings; wv t428 = rt_chk(w2_warn(rt_get(&F_pos, v317_open), rt_s(&S273), rt_s(&S274)), "postfix"); std_push(t427, t428); });
      }
      wv v318_index = rt_bind(rt_chk(s216_expr(up), "postfix"), 0);
      (void)rt_chk(s199_expect(up, rt_s(&S99), rt_s(&S275), RT_NULL), "postfix");
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v318_index), rt_s(&S171)), "&&") && rt_bool(rt_not(rt_get(&F_paren, v318_index)), "&&")), "&&") && rt_bool(rt_eq(std_length(rt_get(&F_parts, v318_index)), rt_i(1)), "&&")), "&&") && rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v318_index))), rt_s(&S185)), "&&")))) {
        wv v319_key = rt_bind(rt_idx(rt_i(0), rt_get(&F_parts, v318_index)), 0);
        if (rt_cond(rt_b(rt_bool(rt_chk(w4_isIdent(v319_key), "postfix"), "&&") && rt_bool(rt_not(rt_chk(w1_contains(w3_keywords, v319_key), "postfix")), "&&")))) {
          (void)({ wv t433 = w2_warnings; wv t434 = ({ wv t431 = rt_get(&F_pos, v318_index); wv t432 = ({ wv t429 = rt_text(v319_key); wv t430 = rt_text(v319_key); rt_cat(5, (wv[]){ rt_s(&S276), t429, rt_s(&S277), t430, rt_s(&S278) }); }); rt_chk(w2_warn(t431, t432, RT_NULL), "postfix"); }); std_push(t433, t434); });
        }
      }
      v316_e = ({ wv t435 = v318_index; wv t436 = v316_e; wv t437 = rt_get(&F_pos, v317_open); rt_rec(0, 5, K26, (wv[]){ rt_s(&S188), t435, t436, t437, rt_b(0) }); });
    } else if (rt_cond(rt_chk(s194_at(up, rt_s(&S129)), "postfix"))) {
      (void)rt_chk(s197_next(up), "postfix");
      wv v320_tk = rt_bind(rt_chk(s193_peek(up), "postfix"), 0);
      if (rt_cond(rt_ne(rt_get(&F_t, v320_tk), rt_s(&S137)))) {
        wv v321_message = rt_bind((rt_cond(({ wv t438 = w3_keywords; wv t439 = rt_get(&F_t, v320_tk); rt_chk(w1_contains(t438, t439), "postfix"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v320_tk)), rt_s(&S279) }) : rt_cat(2, (wv[]){ rt_s(&S280), rt_text(rt_chk(w4_describe(v320_tk), "postfix")) })), 0);
        (void)rt_chk(w4_failAtHint(v320_tk, v321_message, rt_s(&S133)), "postfix");
      }
      (void)rt_chk(s197_next(up), "postfix");
      v316_e = ({ wv t440 = rt_get(&F_v, v320_tk); wv t441 = v316_e; wv t442 = rt_get(&F_pos, v320_tk); rt_rec(0, 5, K27, (wv[]){ rt_s(&S189), t440, t441, t442, rt_b(0) }); });
    } else {
      break;
    }
  }
  RT_RET(v316_e);
}

static wv s224_call(struct fr50 *up, wv v322_args) {
  rt_enter("call");
  (void)rt_chk(s197_next(up), "call");
  wv v323_c = rt_bind(rt_chk(s197_next(up), "call"), 0);
  if (rt_cond(rt_ne(rt_chk(s198_accept(up, rt_s(&S129)), "call"), RT_NULL))) {
    wv v324_m = rt_bind(rt_chk(s197_next(up), "call"), 0);
    (void)rt_chk(s197_next(up), "call");
    wv v325_rec = ({ wv t443 = rt_get(&F_v, v323_c); wv t444 = rt_get(&F_pos, v323_c); rt_rec(0, 4, K28, (wv[]){ rt_s(&S211), t443, t444, rt_b(0) }); });
    RT_RET(({ wv t448 = ({ wv t445 = rt_get(&F_v, v324_m); wv t446 = v325_rec; wv t447 = rt_get(&F_pos, v324_m); rt_rec(0, 5, K27, (wv[]){ rt_s(&S189), t445, t446, t447, rt_b(0) }); }); wv t449 = v322_args; wv t450 = rt_get(&F_pos, v324_m); rt_rec(0, 5, K29, (wv[]){ rt_s(&S182), t448, t449, t450, rt_b(0) }); }));
  }
  (void)rt_chk(s197_next(up), "call");
  RT_RET(({ wv t454 = ({ wv t451 = (rt_cond(rt_eq(rt_get(&F_t, v323_c), rt_s(&S137))) ? rt_s(&S211) : rt_s(&S156)); wv t452 = rt_get(&F_v, v323_c); wv t453 = rt_get(&F_pos, v323_c); rt_rec(0, 4, K28, (wv[]){ t451, t452, t453, rt_b(0) }); }); wv t455 = v322_args; wv t456 = rt_get(&F_pos, v323_c); rt_rec(0, 5, K29, (wv[]){ rt_s(&S182), t454, t455, t456, rt_b(0) }); }));
}

static wv s225_primary(struct fr50 *up) {
  rt_enter("primary");
  wv v326_tk = rt_bind(rt_chk(s193_peek(up), "primary"), 0);
  wv v327_at0 = rt_bind(rt_get(&F_pos, v326_tk), 0);
  wv v328_kind = rt_bind(rt_get(&F_t, v326_tk), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v328_kind, rt_s(&S39)), "||") || rt_bool(rt_eq(v328_kind, rt_s(&S175)), "||")))) {
    RT_RET(rt_chk(s229_chain(up), "primary"));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S44)))) {
    RT_RET(rt_chk(s227_parseMatch(up), "primary"));
  }
  if (rt_cond(rt_chk(s196_atCall(up), "primary"))) {
    RT_RET(rt_chk(s224_call(up, rt_arr(0, NULL, 0)), "primary"));
  }
  (void)rt_chk(s197_next(up), "primary");
  if (rt_cond(rt_b(rt_bool(rt_eq(v328_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v328_kind, rt_s(&S148)), "||")))) {
    RT_RET(({ wv t457 = v328_kind; wv t458 = rt_get(&F_v, v326_tk); wv t459 = v327_at0; rt_rec(0, 4, K24, (wv[]){ t457, t458, t459, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S171)))) {
    wv v329_parts = rt_arr(0, NULL, 1);
    wv v330_k = rt_i(0);
    while (rt_cond(({ wv t460 = v330_k; wv t461 = std_length(rt_get(&F_parts, v326_tk)); rt_lt(t460, t461); }))) {
      wv v331_part = rt_bind(({ wv t462 = v330_k; wv t463 = rt_get(&F_parts, v326_tk); rt_idx(t462, t463); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v331_part), rt_s(&S185)))) {
        (void)std_push(v329_parts, v331_part);
      } else {
        (void)({ wv t464 = v329_parts; wv t465 = rt_chk(w4_parser(rt_get(&F_tokens, v331_part), rt_b(1)), "primary"); std_push(t464, t465); });
      }
      v330_k = rt_add(v330_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 4, K30, (wv[]){ rt_s(&S171), v329_parts, v327_at0, rt_b(0) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v328_kind, rt_s(&S49)), "||") || rt_bool(rt_eq(v328_kind, rt_s(&S50)), "||")))) {
    RT_RET(({ wv t466 = rt_eq(v328_kind, rt_s(&S49)); wv t467 = v327_at0; rt_rec(0, 4, K24, (wv[]){ rt_s(&S281), t466, t467, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S51)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S51), v327_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S52)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S52), v327_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S53)))) {
    RT_RET(rt_rec(0, 3, K31, (wv[]){ rt_s(&S53), v327_at0, rt_b(0) }));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S137)))) {
    RT_RET(({ wv t468 = rt_get(&F_v, v326_tk); wv t469 = v327_at0; rt_rec(0, 4, K28, (wv[]){ rt_s(&S211), t468, t469, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S156)))) {
    RT_RET(({ wv t470 = rt_get(&F_v, v326_tk); wv t471 = v327_at0; rt_rec(0, 4, K28, (wv[]){ rt_s(&S156), t470, t471, rt_b(0) }); }));
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S97)))) {
    if (rt_cond(rt_chk(s194_at(up, rt_s(&S16)), "primary"))) {
      (void)rt_chk(w4_failAtHint(v326_tk, rt_s(&S282), rt_s(&S283)), "primary");
    }
    wv v332_e = rt_bind(rt_chk(s216_expr(up), "primary"), 1);
    if (rt_cond(rt_chk(s194_at(up, rt_s(&S96)), "primary"))) {
      wv v333_args = rt_arr(1, (wv[]){ v332_e }, 1);
      while (rt_cond(rt_ne(rt_chk(s198_accept(up, rt_s(&S96)), "primary"), RT_NULL))) {
        if (rt_cond(rt_chk(s194_at(up, rt_s(&S16)), "primary"))) {
          (void)rt_chk(w4_failAt(rt_chk(s193_peek(up), "primary"), rt_s(&S284)), "primary");
        }
        (void)({ wv t472 = v333_args; wv t473 = rt_chk(s216_expr(up), "primary"); std_push(t472, t473); });
      }
      (void)rt_chk(s199_expect(up, rt_s(&S16), rt_s(&S285), RT_NULL), "primary");
      if (rt_cond(rt_not(rt_chk(s196_atCall(up), "primary")))) {
        (void)rt_chk(w4_failAtHint(v326_tk, rt_s(&S286), rt_s(&S287)), "primary");
      }
      RT_RET(rt_chk(s224_call(up, v333_args), "primary"));
    }
    (void)rt_chk(s199_expect(up, rt_s(&S16), RT_NULL, RT_NULL), "primary");
    rt_set_mem(rt_b(1), &F_paren, v332_e);
    RT_RET(v332_e);
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S98)))) {
    wv v334_items = rt_arr(0, NULL, 1);
    if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S99)), "primary")))) {
      (void)({ wv t474 = v334_items; wv t475 = rt_chk(s216_expr(up), "primary"); std_push(t474, t475); });
      while (rt_cond(rt_chk(s194_at(up, rt_s(&S96)), "primary"))) {
        wv v335_comma = rt_bind(rt_chk(s197_next(up), "primary"), 0);
        if (rt_cond(rt_chk(s194_at(up, rt_s(&S99)), "primary"))) {
          (void)rt_chk(w4_failAt(v335_comma, rt_s(&S288)), "primary");
        }
        (void)({ wv t476 = v334_items; wv t477 = rt_chk(s216_expr(up), "primary"); std_push(t476, t477); });
      }
    }
    (void)rt_chk(s199_expect(up, rt_s(&S99), rt_s(&S289), RT_NULL), "primary");
    RT_RET(rt_rec(0, 4, K32, (wv[]){ rt_s(&S186), v334_items, v327_at0, rt_b(0) }));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v328_kind, rt_s(&S35)), "||") || rt_bool(rt_eq(v328_kind, rt_s(&S36)), "||")), "||") || rt_bool(rt_eq(v328_kind, rt_s(&S37)), "||")), "||") || rt_bool(rt_eq(v328_kind, rt_s(&S38)), "||")))) {
    (void)({ wv t478 = v326_tk; wv t479 = rt_cat(3, (wv[]){ rt_s(&S290), rt_text(v328_kind), rt_s(&S113) }); wv t480 = rt_cat(3, (wv[]){ rt_s(&S291), rt_text(v328_kind), rt_s(&S292) }); rt_chk(w4_failAtHint(t478, t479, t480), "primary"); });
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S55)))) {
    (void)rt_chk(w4_failAtHint(v326_tk, rt_s(&S293), rt_s(&S294)), "primary");
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S111)))) {
    (void)({ wv t481 = v326_tk; wv t482 = rt_cat(3, (wv[]){ rt_s(&S290), rt_text(rt_get(&F_v, v326_tk)), rt_s(&S181) }); wv t483 = rt_cat(3, (wv[]){ rt_s(&S295), rt_text(rt_get(&F_v, v326_tk)), rt_s(&S296) }); rt_chk(w4_failAtHint(t481, t482, t483), "primary"); });
  }
  if (rt_cond(rt_eq(v328_kind, rt_s(&S100)))) {
    RT_RET(rt_chk(s226_record(up, v326_tk), "primary"));
  }
  (void)({ wv t484 = v326_tk; wv t485 = rt_cat(2, (wv[]){ rt_s(&S297), rt_text(rt_chk(w4_describe(v326_tk), "primary")) }); rt_chk(w4_failAt(t484, t485), "primary"); });
  RT_RET(RT_NULL);
}

static wv s226_record(struct fr50 *up, wv v336_open) {
  rt_enter("record");
  v336_open = rt_bind(v336_open, 0);
  wv v337_fields = rt_arr(0, NULL, 1);
  wv v338_seen = rt_arr(0, NULL, 1);
  if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S15)), "record")))) {
    while (rt_cond(rt_b(1))) {
      wv v339_key = rt_bind(rt_chk(s193_peek(up), "record"), 0);
      if (rt_cond(rt_ne(rt_get(&F_t, v339_key), rt_s(&S137)))) {
        wv v341_message = rt_bind((rt_cond(({ wv t486 = w3_keywords; wv t487 = rt_get(&F_t, v339_key); rt_chk(w1_contains(t486, t487), "record"); })) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_t, v339_key)), rt_s(&S279) }) : rt_cat(2, (wv[]){ rt_s(&S298), rt_text(rt_chk(w4_describe(v339_key), "record")) })), 0);
        (void)rt_chk(w4_failAtHint(v339_key, v341_message, rt_s(&S299)), "record");
      }
      (void)rt_chk(s197_next(up), "record");
      if (rt_cond(({ wv t488 = v338_seen; wv t489 = rt_get(&F_v, v339_key); rt_chk(w1_contains(t488, t489), "record"); }))) {
        (void)({ wv t490 = v339_key; wv t491 = rt_cat(3, (wv[]){ rt_s(&S300), rt_text(rt_get(&F_v, v339_key)), rt_s(&S301) }); rt_chk(w4_failAt(t490, t491), "record"); });
      }
      (void)({ wv t492 = v338_seen; wv t493 = rt_get(&F_v, v339_key); std_push(t492, t493); });
      if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S28)), "record")))) {
        wv v342_found = rt_bind(rt_chk(s193_peek(up), "record"), 0);
        wv v343_hint = rt_bind((rt_cond(rt_b(rt_bool(rt_chk(s194_at(up, rt_s(&S96)), "record"), "||") || rt_bool(rt_chk(s194_at(up, rt_s(&S15)), "record"), "||"))) ? ({ wv t494 = rt_text(rt_get(&F_v, v339_key)); wv t495 = rt_text(rt_get(&F_v, v339_key)); rt_cat(4, (wv[]){ rt_s(&S302), t494, rt_s(&S29), t495 }); }) : RT_NULL), 0);
        (void)({ wv t498 = v342_found; wv t499 = ({ wv t496 = rt_text(rt_get(&F_v, v339_key)); wv t497 = rt_text(rt_chk(w4_describe(v342_found), "record")); rt_cat(4, (wv[]){ rt_s(&S303), t496, rt_s(&S195), t497 }); }); wv t500 = v343_hint; rt_chk(w4_failAtHint(t498, t499, t500), "record"); });
      }
      (void)rt_chk(s197_next(up), "record");
      (void)({ wv t504 = v337_fields; wv t505 = ({ wv t501 = rt_get(&F_v, v339_key); wv t502 = rt_chk(s216_expr(up), "record"); wv t503 = rt_get(&F_pos, v339_key); rt_rec(0, 3, K33, (wv[]){ t501, t502, t503 }); }); std_push(t504, t505); });
      if (rt_cond(rt_not(rt_chk(s194_at(up, rt_s(&S96)), "record")))) {
        break;
      }
      wv v340_comma = rt_bind(rt_chk(s197_next(up), "record"), 0);
      if (rt_cond(rt_chk(s194_at(up, rt_s(&S15)), "record"))) {
        (void)rt_chk(w4_failAt(v340_comma, rt_s(&S304)), "record");
      }
    }
  }
  (void)rt_chk(s199_expect(up, rt_s(&S15), rt_s(&S305), RT_NULL), "record");
  RT_RET(({ wv t506 = v337_fields; wv t507 = rt_get(&F_pos, v336_open); rt_rec(0, 4, K34, (wv[]){ rt_s(&S187), t506, t507, rt_b(0) }); }));
}

static wv s227_parseMatch(struct fr50 *up) {
  rt_enter("parseMatch");
  wv v344_kw = rt_bind(rt_chk(s197_next(up), "parseMatch"), 0);
  wv v345_subject = rt_bind(rt_chk(s213_cond(up, rt_s(&S44)), "parseMatch"), 0);
  (void)rt_chk(s199_expect(up, rt_s(&S100), rt_s(&S306), RT_NULL), "parseMatch");
  wv v346_cases = rt_arr(0, NULL, 1);
  wv v347_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    if (rt_cond(rt_chk(s194_at(up, rt_s(&S45)), "parseMatch"))) {
      wv v348_a = rt_bind(rt_chk(s197_next(up), "parseMatch"), 0);
      if (rt_cond(rt_ne(v347_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v348_a, rt_s(&S307)), "parseMatch");
      }
      if (rt_cond(rt_chk(s194_at(up, rt_s(&S100)), "parseMatch"))) {
        (void)rt_chk(w4_failAtHint(rt_chk(s193_peek(up), "parseMatch"), rt_s(&S308), rt_s(&S309)), "parseMatch");
      }
      wv v349_values = rt_arr(0, NULL, 1);
      (void)({ wv t508 = v349_values; wv t509 = rt_chk(s228_caseValue(up), "parseMatch"); std_push(t508, t509); });
      while (rt_cond(rt_ne(rt_chk(s198_accept(up, rt_s(&S96)), "parseMatch"), RT_NULL))) {
        (void)({ wv t510 = v349_values; wv t511 = rt_chk(s228_caseValue(up), "parseMatch"); std_push(t510, t511); });
      }
      (void)({ wv t515 = v346_cases; wv t516 = ({ wv t512 = v349_values; wv t513 = rt_chk(s214_block(up), "parseMatch"); wv t514 = rt_get(&F_pos, v348_a); rt_rec(0, 3, K35, (wv[]){ t512, t513, t514 }); }); std_push(t515, t516); });
    } else if (rt_cond(rt_chk(s194_at(up, rt_s(&S40)), "parseMatch"))) {
      wv v350_a = rt_bind(rt_chk(s197_next(up), "parseMatch"), 0);
      if (rt_cond(rt_ne(v347_els, RT_NULL))) {
        (void)rt_chk(w4_failAt(v350_a, rt_s(&S310)), "parseMatch");
      }
      if (rt_cond(rt_eq(std_length(v346_cases), rt_i(0)))) {
        (void)rt_chk(w4_failAt(v350_a, rt_s(&S311)), "parseMatch");
      }
      v347_els = rt_bind(rt_chk(s214_block(up), "parseMatch"), 1);
    } else {
      break;
    }
  }
  if (rt_cond(rt_eq(std_length(v346_cases), rt_i(0)))) {
    (void)rt_chk(w4_failAtHint(rt_chk(s193_peek(up), "parseMatch"), rt_s(&S312), rt_s(&S313)), "parseMatch");
  }
  (void)rt_chk(s199_expect(up, rt_s(&S15), rt_s(&S314), RT_NULL), "parseMatch");
  RT_RET(({ wv t517 = v345_subject; wv t518 = v346_cases; wv t519 = v347_els; wv t520 = rt_get(&F_pos, v344_kw); rt_rec(0, 6, K36, (wv[]){ rt_s(&S44), t517, t518, t519, t520, rt_b(0) }); }));
}

static wv s228_caseValue(struct fr50 *up) {
  rt_enter("caseValue");
  wv v351_tk = rt_bind(rt_chk(s193_peek(up), "caseValue"), 0);
  wv v352_v = rt_bind(rt_chk(s221_unary(up), "caseValue"), 0);
  wv v353_kind = rt_bind(rt_get(&F_k, v352_v), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v353_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v353_kind, rt_s(&S281)), "||")), "||") || rt_bool(rt_eq(v353_kind, rt_s(&S51)), "||")))) {
    RT_RET(v352_v);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v353_kind, rt_s(&S189)), "&&") && rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v352_v)), rt_s(&S211)), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v352_v)), rt_s(&S189)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, rt_get(&F_rec, v352_v))), rt_s(&S211)), "&&")), "||")), "&&")))) {
    RT_RET(v352_v);
  }
  if (rt_cond(rt_eq(v353_kind, rt_s(&S171)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(rt_get(&F_parts, v352_v)), rt_i(1)), "&&") && rt_bool(rt_eq(std_typeOf(rt_idx(rt_i(0), rt_get(&F_parts, v352_v))), rt_s(&S185)), "&&")))) {
      RT_RET(v352_v);
    }
    (void)rt_chk(w4_failAt(v351_tk, rt_s(&S315)), "caseValue");
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v353_kind, rt_s(&S148)), "||") || rt_bool(rt_eq(v353_kind, rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(v353_kind, rt_s(&S53)), "||")))) {
    (void)rt_chk(w4_failAtHint(v351_tk, rt_s(&S316), rt_s(&S317)), "caseValue");
  }
  (void)rt_chk(w4_failAtHint(v351_tk, rt_s(&S318), rt_s(&S319)), "caseValue");
  RT_RET(RT_NULL);
}

static wv s229_chain(struct fr50 *up) {
  rt_enter("chain");
  wv v354_branches = rt_arr(0, NULL, 1);
  wv v355_els = RT_NULL;
  while (rt_cond(rt_b(1))) {
    wv v356_kw = rt_bind(rt_chk(s197_next(up), "chain"), 0);
    wv v357_c = rt_bind(rt_chk(s213_cond(up, rt_get(&F_t, v356_kw)), "chain"), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v356_kw), rt_s(&S175)), "&&") && rt_bool(rt_eq(rt_get(&F_k, v357_c), rt_s(&S191)), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_op, v357_c), rt_s(&S77)), "&&")))) {
      (void)({ wv t521 = w2_warnings; wv t522 = rt_chk(w2_warn(rt_get(&F_pos, v356_kw), rt_s(&S320), rt_s(&S321)), "chain"); std_push(t521, t522); });
    }
    (void)({ wv t527 = v354_branches; wv t528 = ({ wv t523 = rt_eq(rt_get(&F_t, v356_kw), rt_s(&S175)); wv t524 = v357_c; wv t525 = rt_chk(s214_block(up), "chain"); wv t526 = rt_get(&F_pos, v356_kw); rt_rec(0, 4, K37, (wv[]){ t523, t524, t525, t526 }); }); std_push(t527, t528); });
    if (rt_cond(rt_eq(rt_chk(s198_accept(up, rt_s(&S40)), "chain"), RT_NULL))) {
      break;
    }
    if (rt_cond(rt_b(rt_bool(rt_chk(s194_at(up, rt_s(&S39)), "chain"), "||") || rt_bool(rt_chk(s194_at(up, rt_s(&S175)), "chain"), "||")))) {
      continue;
    }
    v355_els = rt_bind(rt_chk(s214_block(up), "chain"), 1);
    break;
  }
  RT_RET(({ wv t529 = v354_branches; wv t530 = v355_els; wv t531 = rt_get(&F_pos, rt_idx(rt_i(0), v354_branches)); rt_rec(0, 5, K38, (wv[]){ rt_s(&S184), t529, t530, t531, rt_b(0) }); }));
}

static wv w5_stdSig(wv v360_name) {
  rt_enter("stdSig");
  v360_name = rt_bind(v360_name, 0);
  wv v361_k = rt_i(0);
  while (rt_cond(({ wv t569 = v361_k; wv t570 = std_length(rt_def(w5_stdlib, "stdlib")); rt_lt(t569, t570); }))) {
    if (rt_cond(({ wv t573 = rt_get(&F_name, ({ wv t571 = v361_k; wv t572 = rt_def(w5_stdlib, "stdlib"); rt_idx(t571, t572); })); wv t574 = v360_name; rt_eq(t573, t574); }))) {
      RT_RET(rt_get(&F_arity, ({ wv t575 = v361_k; wv t576 = rt_def(w5_stdlib, "stdlib"); rt_idx(t575, t576); })));
    }
    v361_k = rt_add(v361_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w5_modeOf(wv v362_x) {
  rt_enter("modeOf");
  v362_x = rt_bind(v362_x, 0);
  RT_RET((rt_cond(rt_get(&F_isVar, v362_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v362_x)) ? rt_s(&S37) : rt_s(&S13))));
}

static wv w5_kindText(wv v363_kind) {
  rt_enter("kindText");
  v363_kind = rt_bind(v363_kind, 0);
  RT_RET((rt_cond(rt_eq(v363_kind, rt_s(&S33))) ? rt_s(&S359) : (rt_cond(rt_eq(v363_kind, rt_s(&S46))) ? rt_s(&S360) : rt_cat(2, (wv[]){ rt_s(&S0), rt_text(v363_kind) }))));
}

static wv w5_plural(wv v364_n) {
  rt_enter("plural");
  v364_n = rt_bind(v364_n, 0);
  RT_RET(({ wv t577 = rt_cat(2, (wv[]){ rt_text(v364_n), rt_s(&S361) }); wv t578 = (rt_cond(rt_eq(v364_n, rt_i(1))) ? rt_s(&S13) : rt_s(&S362)); rt_add(t577, t578); }));
}

static wv w5_enumText(wv v365_x) {
  rt_enter("enumText");
  v365_x = rt_bind(v365_x, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v365_x), rt_s(&S211))) ? rt_get(&F_name, v365_x) : ({ wv t579 = rt_text(rt_get(&F_name, rt_get(&F_rec, v365_x))); wv t580 = rt_text(rt_get(&F_field, v365_x)); rt_cat(3, (wv[]){ t579, rt_s(&S129), t580 }); })));
}

static wv w5_caseKey(wv v366_v) {
  rt_enter("caseKey");
  v366_v = rt_bind(v366_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v366_v), rt_s(&S171)))) {
    RT_RET(rt_add(rt_s(&S363), rt_idx(rt_i(0), rt_get(&F_parts, v366_v))));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v366_v), rt_s(&S189)))) {
    RT_RET(({ wv t581 = rt_text(rt_get(&F_field, v366_v)); wv t582 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v366_v)), "caseKey")); rt_cat(4, (wv[]){ rt_s(&S364), t581, rt_s(&S129), t582 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v366_v), rt_s(&S51)))) {
    RT_RET(rt_s(&S51));
  }
  RT_RET(({ wv t583 = rt_text(rt_get(&F_k, v366_v)); wv t584 = rt_text(rt_get(&F_v, v366_v)); rt_cat(3, (wv[]){ t583, rt_s(&S28), t584 }); }));
}

static wv w5_caseText(wv v367_v) {
  rt_enter("caseText");
  v367_v = rt_bind(v367_v, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v367_v), rt_s(&S171)))) {
    RT_RET(rt_chk(w1_quote(rt_idx(rt_i(0), rt_get(&F_parts, v367_v))), "caseText"));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v367_v), rt_s(&S189)))) {
    RT_RET(({ wv t585 = rt_text(rt_chk(w5_enumText(rt_get(&F_rec, v367_v)), "caseText")); wv t586 = rt_text(rt_get(&F_field, v367_v)); rt_cat(3, (wv[]){ t585, rt_s(&S129), t586 }); }));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v367_v), rt_s(&S51)))) {
    RT_RET(rt_s(&S51));
  }
  RT_RET(rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v367_v)) }));
}

static wv w5_getsAwayWithValue(wv v368_list) {
  rt_enter("getsAwayWithValue");
  wv v369_i = rt_i(0);
  while (rt_cond(({ wv t587 = v369_i; wv t588 = std_length(v368_list); rt_lt(t587, t588); }))) {
    wv v370_s = rt_bind(rt_idx(v369_i, v368_list), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_handler, v370_s), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v370_s)))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v370_s), rt_s(&S34)), "&&") && rt_bool(rt_ne(rt_get(&F_e, v370_s), RT_NULL), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v370_s), rt_s(&S41)), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v370_s))), "getsAwayWithValue"), "&&")))) {
      RT_RET(rt_b(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v370_s), rt_s(&S176)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v370_s)), rt_s(&S184)), "&&")))) {
      wv v371_b = rt_i(0);
      while (rt_cond(({ wv t589 = v371_b; wv t590 = std_length(rt_get(&F_branches, rt_get(&F_e, v370_s))); rt_lt(t589, t590); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t591 = v371_b; wv t592 = rt_get(&F_branches, rt_get(&F_e, v370_s)); rt_idx(t591, t592); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v371_b = rt_add(v371_b, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v370_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v370_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v370_s), rt_s(&S176)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v370_s)), rt_s(&S44)), "&&")))) {
      wv v372_a = rt_i(0);
      while (rt_cond(({ wv t593 = v372_a; wv t594 = std_length(rt_get(&F_cases, rt_get(&F_e, v370_s))); rt_lt(t593, t594); }))) {
        if (rt_cond(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, ({ wv t595 = v372_a; wv t596 = rt_get(&F_cases, rt_get(&F_e, v370_s)); rt_idx(t595, t596); })))), "getsAwayWithValue"))) {
          RT_RET(rt_b(1));
        }
        v372_a = rt_add(v372_a, rt_i(1));
      }
      if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, rt_get(&F_e, v370_s)), RT_NULL), "&&") && rt_bool(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_els, rt_get(&F_e, v370_s)))), "getsAwayWithValue"), "&&")))) {
        RT_RET(rt_b(1));
      }
    }
    v369_i = rt_add(v369_i, rt_i(1));
  }
  RT_RET(rt_b(0));
}

static wv w5_lineIn(wv v373_chars, wv v374_pos) {
  rt_enter("lineIn");
  v373_chars = rt_bind(v373_chars, 0);
  v374_pos = rt_bind(v374_pos, 0);
  wv v375_line = rt_i(1);
  wv v376_k = rt_i(0);
  while (rt_cond(rt_lt(v376_k, v374_pos))) {
    if (rt_cond(rt_eq(rt_idx(v376_k, v373_chars), rt_s(&S2)))) {
      v375_line = rt_add(v375_line, rt_i(1));
    }
    v376_k = rt_add(v376_k, rt_i(1));
  }
  RT_RET(v375_line);
}

static wv w5_lineOf(wv v377_pos) {
  rt_enter("lineOf");
  v377_pos = rt_bind(v377_pos, 0);
  RT_RET(rt_chk(w5_lineIn(w2_srcChars, v377_pos), "lineOf"));
}

static wv w5_check(wv p_v378_modules, wv v379_requireExit) {
  rt_enter("check");
  struct fr99 fr = { 0 };
  fr.v378_modules = rt_bind(p_v378_modules, 0);
  v379_requireExit = rt_bind(v379_requireExit, 0);
  fr.v380_errors = rt_arr(0, NULL, 1);
  fr.v381_nextFn = rt_i(1);
  fr.v382_curMod = rt_i(0);
  fr.v383_functions = rt_arr(0, NULL, 1);
  fr.v384_uses = rt_arr(0, NULL, 1);
  fr.v385_enums = rt_arr(0, NULL, 1);
  fr.v418_topFn = rt_rec(0, 2, K44, (wv[]){ rt_i(0), rt_b(1) });
  wv v419_entry = rt_sub(std_length(fr.v378_modules), rt_i(1));
  wv v420_mi = rt_i(0);
  while (rt_cond(({ wv t1122 = v420_mi; wv t1123 = std_length(fr.v378_modules); rt_lt(t1122, t1123); }))) {
    (void)({ wv t1124 = v420_mi; wv t1125 = rt_eq(v420_mi, v419_entry); rt_chk(s417_checkModule(&fr, t1124, t1125), "check"); });
    v420_mi = rt_add(v420_mi, rt_i(1));
  }
  wv v421_si = rt_i(0);
  while (rt_cond(({ wv t1126 = v421_si; wv t1127 = std_length(fr.v383_functions); rt_lt(t1126, t1127); }))) {
    wv v611_sc = rt_bind(rt_idx(v421_si, fr.v383_functions), 0);
    wv v612_use = rt_bind(({ wv t1128 = rt_get(&F_mod, v611_sc); wv t1129 = rt_get(&F_namePos, v611_sc); rt_chk(s389_firstUse(&fr, t1128, t1129), "check"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ne(v612_use, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_value, rt_get(&F_body, v611_sc)), RT_NULL), "&&")), "&&") && rt_bool(rt_not(rt_chk(w5_getsAwayWithValue(rt_get(&F_stmts, rt_get(&F_body, v611_sc))), "check")), "&&")))) {
      (void)rt_chk(s387_enter(&fr, rt_get(&F_mod, v611_sc)), "check");
      wv v613_um = rt_bind(rt_get(&F_useMod, v612_use), 0);
      wv v614_where = rt_bind((rt_cond(({ wv t1130 = v613_um; wv t1131 = rt_get(&F_mod, v611_sc); rt_eq(t1130, t1131); })) ? rt_cat(2, (wv[]){ rt_s(&S452), rt_text(rt_chk(w5_lineOf(rt_get(&F_pos, v612_use)), "check")) }) : ({ wv t1134 = rt_text(rt_get(&F_file, rt_idx(v613_um, fr.v378_modules))); wv t1135 = rt_text(({ wv t1132 = rt_get(&F_chars, rt_idx(v613_um, fr.v378_modules)); wv t1133 = rt_get(&F_pos, v612_use); rt_chk(w5_lineIn(t1132, t1133), "check"); })); rt_cat(3, (wv[]){ t1134, rt_s(&S453), t1135 }); })), 0);
      (void)({ wv t1138 = rt_get(&F_namePos, v611_sc); wv t1139 = ({ wv t1136 = rt_text(rt_get(&F_name, v611_sc)); wv t1137 = rt_text(v614_where); rt_cat(5, (wv[]){ rt_s(&S113), t1136, rt_s(&S454), t1137, rt_s(&S16) }); }); rt_chk(s386_error(&fr, t1138, t1139, rt_s(&S455)), "check"); });
    }
    v421_si = rt_add(v421_si, rt_i(1));
  }
  (void)rt_chk(s387_enter(&fr, v419_entry), "check");
  wv v422_program = rt_bind(rt_get(&F_program, rt_idx(v419_entry, fr.v378_modules)), 0);
  if (rt_cond(v379_requireExit)) {
    wv v615_last = RT_NULL;
    wv v616_k = rt_i(0);
    while (rt_cond(({ wv t1140 = v616_k; wv t1141 = std_length(rt_get(&F_stmts, v422_program)); rt_lt(t1140, t1141); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1142 = v616_k; wv t1143 = rt_get(&F_stmts, v422_program); rt_idx(t1142, t1143); })), rt_s(&S200)))) {
        v615_last = rt_bind(({ wv t1144 = v616_k; wv t1145 = rt_get(&F_stmts, v422_program); rt_idx(t1144, t1145); }), 1);
      }
      v616_k = rt_add(v616_k, rt_i(1));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(v615_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v615_last), rt_s(&S54)), "||")))) {
      (void)rt_chk(s386_error(&fr, rt_get(&F_end, v422_program), rt_s(&S456), rt_s(&S457)), "check");
    }
  }
  RT_RET(fr.v380_errors);
}

static wv s386_error(struct fr99 *up, wv v423_pos, wv v424_message, wv v425_hint) {
  rt_enter("error");
  v423_pos = rt_bind(v423_pos, 0);
  v424_message = rt_bind(v424_message, 0);
  v425_hint = rt_bind(v425_hint, 0);
  (void)({ wv t597 = rt_def(up->v380_errors, "errors"); wv t598 = rt_chk(w2_err(v423_pos, v424_message, v425_hint), "error"); std_push(t597, t598); });
  RT_RET(RT_NULL);
}

static wv s387_enter(struct fr99 *up, wv v426_m) {
  rt_enter("enter");
  v426_m = rt_bind(v426_m, 0);
  wv t599 = rt_bind(v426_m, 1);
  (void)rt_def(up->v382_curMod, "curMod");
  up->v382_curMod = t599;
  (void)({ wv t600 = rt_get(&F_file, rt_idx(v426_m, up->v378_modules)); wv t601 = rt_get(&F_chars, rt_idx(v426_m, up->v378_modules)); rt_chk(w2_setSource(t600, t601), "enter"); });
  RT_RET(RT_NULL);
}

static wv s388_enumMembers(struct fr99 *up, wv v427_d) {
  rt_enter("enumMembers");
  v427_d = rt_bind(v427_d, 0);
  wv v428_k = rt_i(0);
  while (rt_cond(({ wv t602 = v428_k; wv t603 = std_length(rt_def(up->v385_enums, "enums")); rt_lt(t602, t603); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t606 = rt_get(&F_mod, ({ wv t604 = v428_k; wv t605 = rt_def(up->v385_enums, "enums"); rt_idx(t604, t605); })); wv t607 = rt_get(&F_mod, v427_d); rt_eq(t606, t607); }), "&&") && rt_bool(({ wv t610 = rt_get(&F_name, ({ wv t608 = v428_k; wv t609 = rt_def(up->v385_enums, "enums"); rt_idx(t608, t609); })); wv t611 = rt_get(&F_name, v427_d); rt_eq(t610, t611); }), "&&")))) {
      RT_RET(rt_get(&F_members, ({ wv t612 = v428_k; wv t613 = rt_def(up->v385_enums, "enums"); rt_idx(t612, t613); })));
    }
    v428_k = rt_add(v428_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s389_firstUse(struct fr99 *up, wv v429_mod, wv v430_declPos) {
  rt_enter("firstUse");
  v429_mod = rt_bind(v429_mod, 0);
  v430_declPos = rt_bind(v430_declPos, 0);
  wv v431_k = rt_i(0);
  while (rt_cond(({ wv t614 = v431_k; wv t615 = std_length(rt_def(up->v384_uses, "uses")); rt_lt(t614, t615); }))) {
    if (rt_cond(rt_b(rt_bool(({ wv t618 = rt_get(&F_mod, ({ wv t616 = v431_k; wv t617 = rt_def(up->v384_uses, "uses"); rt_idx(t616, t617); })); wv t619 = v429_mod; rt_eq(t618, t619); }), "&&") && rt_bool(({ wv t622 = rt_get(&F_namePos, ({ wv t620 = v431_k; wv t621 = rt_def(up->v384_uses, "uses"); rt_idx(t620, t621); })); wv t623 = v430_declPos; rt_eq(t622, t623); }), "&&")))) {
      RT_RET(({ wv t624 = v431_k; wv t625 = rt_def(up->v384_uses, "uses"); rt_idx(t624, t625); }));
    }
    v431_k = rt_add(v431_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s390_requireValue(struct fr99 *up, wv v432_b) {
  rt_enter("requireValue");
  v432_b = rt_bind(v432_b, 0);
  if (rt_cond(rt_ne(rt_get(&F_value, v432_b), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v433_last = RT_NULL;
  wv v434_k = rt_i(0);
  while (rt_cond(({ wv t626 = v434_k; wv t627 = std_length(rt_get(&F_stmts, v432_b)); rt_lt(t626, t627); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t628 = v434_k; wv t629 = rt_get(&F_stmts, v432_b); rt_idx(t628, t629); })), rt_s(&S200)))) {
      v433_last = rt_bind(({ wv t630 = v434_k; wv t631 = rt_get(&F_stmts, v432_b); rt_idx(t630, t631); }), 1);
    }
    v434_k = rt_add(v434_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_ne(v433_last, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(({ wv t632 = rt_def(w5_neverCompletes, "neverCompletes"); wv t633 = rt_get(&F_k, v433_last); rt_chk(w1_contains(t632, t633), "requireValue"); }), "||") || rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_k, v433_last), rt_s(&S176)), "&&") && rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_e, v433_last)), rt_s(&S183)), "&&")), "||")), "&&")))) {
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s386_error(up, rt_get(&F_close, v432_b), rt_s(&S365), rt_s(&S366)), "requireValue");
  RT_RET(RT_NULL);
}

static wv s391_newScope(struct fr99 *up, wv v435_parent) {
  rt_enter("newScope");
  v435_parent = rt_bind(v435_parent, 0);
  wv v436_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K40, (wv[]){ v435_parent, v436_names }));
}

static wv s392_decl(struct fr99 *up, wv v437_kind, wv v438_name, wv v439_mode, wv v440_namePos, wv v441_visibleAt, wv v442_fnId, wv v443_arity) {
  rt_enter("decl");
  v437_kind = rt_bind(v437_kind, 0);
  v438_name = rt_bind(v438_name, 0);
  v439_mode = rt_bind(v439_mode, 0);
  v440_namePos = rt_bind(v440_namePos, 0);
  v441_visibleAt = rt_bind(v441_visibleAt, 0);
  v442_fnId = rt_bind(v442_fnId, 0);
  v443_arity = rt_bind(v443_arity, 0);
  RT_RET(({ wv t634 = v437_kind; wv t635 = v438_name; wv t636 = rt_eq(v439_mode, rt_s(&S36)); wv t637 = rt_eq(v439_mode, rt_s(&S37)); wv t638 = v440_namePos; wv t639 = v441_visibleAt; wv t640 = v442_fnId; wv t641 = v443_arity; wv t642 = rt_def(up->v382_curMod, "curMod"); rt_rec(0, 10, K41, (wv[]){ t634, t635, t636, t637, t638, t639, t640, t641, t642, RT_NULL }); }));
}

static wv s393_own(struct fr99 *up, wv v444_scope, wv v445_name) {
  rt_enter("own");
  v445_name = rt_bind(v445_name, 0);
  wv v446_k = rt_i(0);
  while (rt_cond(({ wv t643 = v446_k; wv t644 = std_length(rt_get(&F_names, v444_scope)); rt_lt(t643, t644); }))) {
    if (rt_cond(({ wv t647 = rt_get(&F_name, ({ wv t645 = v446_k; wv t646 = rt_get(&F_names, v444_scope); rt_idx(t645, t646); })); wv t648 = v445_name; rt_eq(t647, t648); }))) {
      RT_RET(({ wv t649 = v446_k; wv t650 = rt_get(&F_names, v444_scope); rt_idx(t649, t650); }));
    }
    v446_k = rt_add(v446_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s394_declare(struct fr99 *up, wv v447_scope, wv v448_name, wv v449_d) {
  rt_enter("declare");
  v448_name = rt_bind(v448_name, 0);
  v449_d = rt_bind(v449_d, 0);
  wv v450_dup = rt_bind(rt_chk(s393_own(up, v447_scope, v448_name), "declare"), 0);
  if (rt_cond(rt_ne(v450_dup, RT_NULL))) {
    (void)({ wv t653 = rt_get(&F_namePos, v449_d); wv t654 = ({ wv t651 = rt_text(v448_name); wv t652 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v450_dup)), "declare")); rt_cat(5, (wv[]){ rt_s(&S113), t651, rt_s(&S367), t652, rt_s(&S16) }); }); rt_chk(s386_error(up, t653, t654, RT_NULL), "declare"); });
    RT_RET(RT_NULL);
  }
  wv v451_s = rt_get(&F_parent, v447_scope);
  while (rt_cond(rt_ne(v451_s, RT_NULL))) {
    wv v452_outer = rt_bind(rt_chk(s393_own(up, v451_s, v448_name), "declare"), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v452_outer, RT_NULL), "&&") && rt_bool(rt_b(rt_bool(rt_ne(rt_get(&F_kind, v452_outer), rt_s(&S177)), "||") || rt_bool(({ wv t655 = rt_get(&F_namePos, v452_outer); wv t656 = rt_get(&F_namePos, v449_d); rt_lt(t655, t656); }), "||")), "&&")))) {
      (void)({ wv t660 = rt_get(&F_namePos, v449_d); wv t661 = ({ wv t657 = rt_text(v448_name); wv t658 = rt_text(v448_name); wv t659 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v452_outer)), "declare")); rt_cat(7, (wv[]){ rt_s(&S113), t657, rt_s(&S368), t658, rt_s(&S369), t659, rt_s(&S16) }); }); rt_chk(s386_error(up, t660, t661, rt_s(&S370)), "declare"); });
      break;
    }
    v451_s = rt_get(&F_parent, v451_s);
  }
  (void)({ wv t662 = rt_get(&F_names, v447_scope); wv t663 = v449_d; std_push(t662, t663); });
  RT_RET(RT_NULL);
}

static wv s395_lookup(struct fr99 *up, wv v453_scope, wv v454_name) {
  rt_enter("lookup");
  v454_name = rt_bind(v454_name, 0);
  wv v455_s = v453_scope;
  while (rt_cond(rt_ne(v455_s, RT_NULL))) {
    wv v456_d = rt_bind(rt_chk(s393_own(up, v455_s, v454_name), "lookup"), 0);
    if (rt_cond(rt_ne(v456_d, RT_NULL))) {
      RT_RET(v456_d);
    }
    v455_s = rt_get(&F_parent, v455_s);
  }
  RT_RET(RT_NULL);
}

static wv s396_resolve(struct fr99 *up, wv v457_name, wv v458_pos, wv v459_scope, wv v460_ctx) {
  rt_enter("resolve");
  v457_name = rt_bind(v457_name, 0);
  v458_pos = rt_bind(v458_pos, 0);
  v459_scope = rt_bind(v459_scope, 0);
  v460_ctx = rt_bind(v460_ctx, 0);
  wv v461_d = rt_bind(rt_chk(s395_lookup(up, v459_scope, v457_name), "resolve"), 0);
  if (rt_cond(rt_eq(v461_d, RT_NULL))) {
    (void)({ wv t664 = v458_pos; wv t665 = rt_cat(3, (wv[]){ rt_s(&S371), rt_text(v457_name), rt_s(&S113) }); rt_chk(s386_error(up, t664, t665, RT_NULL), "resolve"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v461_d), rt_s(&S177)), "&&") && rt_bool(({ wv t666 = rt_get(&F_fnId, v461_d); wv t667 = rt_get(&F_id, rt_get(&F_func, v460_ctx)); rt_eq(t666, t667); }), "&&")), "&&") && rt_bool(({ wv t668 = v458_pos; wv t669 = rt_get(&F_visibleAt, v461_d); rt_lt(t668, t669); }), "&&")))) {
    (void)({ wv t672 = v458_pos; wv t673 = ({ wv t670 = rt_text(v457_name); wv t671 = rt_text(rt_chk(w5_lineOf(rt_get(&F_namePos, v461_d)), "resolve")); rt_cat(5, (wv[]){ rt_s(&S113), t670, rt_s(&S372), t671, rt_s(&S16) }); }); rt_chk(s386_error(up, t672, t673, RT_NULL), "resolve"); });
  }
  RT_RET(v461_d);
}

static wv s397_block(struct fr99 *up, wv v462_b, wv v463_scope, wv v464_ctx) {
  rt_enter("block");
  v463_scope = rt_bind(v463_scope, 0);
  v464_ctx = rt_bind(v464_ctx, 0);
  wv v465_k = rt_i(0);
  while (rt_cond(({ wv t674 = v465_k; wv t675 = std_length(rt_get(&F_stmts, v462_b)); rt_lt(t674, t675); }))) {
    wv v466_s = rt_bind(({ wv t676 = v465_k; wv t677 = rt_get(&F_stmts, v462_b); rt_idx(t676, t677); }), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v466_s), rt_s(&S33)))) {
      wv v467_d = rt_bind(({ wv t678 = rt_get(&F_name, v466_s); wv t679 = rt_get(&F_namePos, v466_s); wv t680 = rt_get(&F_id, rt_get(&F_func, v464_ctx)); wv t681 = std_length(rt_get(&F_params, v466_s)); rt_chk(s392_decl(up, rt_s(&S33), t678, rt_s(&S13), t679, RT_NULL, t680, t681), "block"); }), 0);
      (void)({ wv t682 = v463_scope; wv t683 = rt_get(&F_name, v466_s); wv t684 = v467_d; rt_chk(s394_declare(up, t682, t683, t684), "block"); });
      if (rt_cond(({ wv t687 = ({ wv t685 = v463_scope; wv t686 = rt_get(&F_name, v466_s); rt_chk(s393_own(up, t685, t686), "block"); }); wv t688 = v467_d; rt_eq(t687, t688); }))) {
        (void)({ wv t693 = rt_def(up->v383_functions, "functions"); wv t694 = ({ wv t689 = rt_def(up->v382_curMod, "curMod"); wv t690 = rt_get(&F_name, v466_s); wv t691 = rt_get(&F_namePos, v466_s); wv t692 = rt_get(&F_body, v466_s); rt_rec(0, 4, K42, (wv[]){ t689, t690, t691, t692 }); }); std_push(t693, t694); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v466_s), rt_s(&S46)))) {
      wv v468_d = rt_bind(({ wv t695 = rt_get(&F_name, v466_s); wv t696 = rt_get(&F_namePos, v466_s); wv t697 = rt_get(&F_id, rt_get(&F_func, v464_ctx)); rt_chk(s392_decl(up, rt_s(&S46), t695, rt_s(&S13), t696, RT_NULL, t697, RT_NULL), "block"); }), 0);
      (void)({ wv t698 = v463_scope; wv t699 = rt_get(&F_name, v466_s); wv t700 = v468_d; rt_chk(s394_declare(up, t698, t699, t700), "block"); });
      if (rt_cond(({ wv t703 = ({ wv t701 = v463_scope; wv t702 = rt_get(&F_name, v466_s); rt_chk(s393_own(up, t701, t702), "block"); }); wv t704 = v468_d; rt_eq(t703, t704); }))) {
        (void)({ wv t708 = rt_def(up->v385_enums, "enums"); wv t709 = ({ wv t705 = rt_def(up->v382_curMod, "curMod"); wv t706 = rt_get(&F_name, v466_s); wv t707 = rt_get(&F_members, v466_s); rt_rec(0, 3, K43, (wv[]){ t705, t706, t707 }); }); std_push(t708, t709); });
      }
    } else if (rt_cond(rt_eq(rt_get(&F_k, v466_s), rt_s(&S177)))) {
      (void)({ wv t715 = v463_scope; wv t716 = rt_get(&F_name, v466_s); wv t717 = ({ wv t710 = rt_get(&F_name, v466_s); wv t711 = rt_chk(w5_modeOf(v466_s), "block"); wv t712 = rt_get(&F_namePos, v466_s); wv t713 = rt_get(&F_end, v466_s); wv t714 = rt_get(&F_id, rt_get(&F_func, v464_ctx)); rt_chk(s392_decl(up, rt_s(&S177), t710, t711, t712, t713, t714, RT_NULL), "block"); }); rt_chk(s394_declare(up, t715, t716, t717), "block"); });
    }
    v465_k = rt_add(v465_k, rt_i(1));
  }
  v465_k = rt_i(0);
  while (rt_cond(({ wv t718 = v465_k; wv t719 = std_length(rt_get(&F_stmts, v462_b)); rt_lt(t718, t719); }))) {
    (void)({ wv t722 = ({ wv t720 = v465_k; wv t721 = rt_get(&F_stmts, v462_b); rt_idx(t720, t721); }); wv t723 = v463_scope; wv t724 = v464_ctx; rt_chk(s398_stmt(up, t722, t723, t724), "block"); });
    v465_k = rt_add(v465_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v462_b), RT_NULL))) {
    (void)({ wv t725 = rt_get(&F_value, v462_b); wv t726 = v463_scope; wv t727 = v464_ctx; rt_chk(s413_expr(up, t725, t726, t727), "block"); });
  }
  RT_RET(RT_NULL);
}

static wv s398_stmt(struct fr99 *up, wv v469_s, wv v470_scope, wv v471_ctx) {
  rt_enter("stmt");
  v470_scope = rt_bind(v470_scope, 0);
  v471_ctx = rt_bind(v471_ctx, 0);
  wv v472_kind = rt_bind(rt_get(&F_k, v469_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v472_kind, rt_s(&S200)), "||") || rt_bool(rt_eq(v472_kind, rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(v472_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v472_kind, rt_s(&S203)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v472_kind, rt_s(&S33)))) {
    wv v473_func = rt_rec(0, 2, K44, (wv[]){ rt_def(up->v381_nextFn, "nextFn"), rt_b(0) });
    up->v381_nextFn = rt_add(rt_def(up->v381_nextFn, "nextFn"), rt_i(1));
    wv v474_ps = rt_bind(rt_chk(s391_newScope(up, v470_scope), "stmt"), 0);
    wv v475_k = rt_i(0);
    while (rt_cond(({ wv t728 = v475_k; wv t729 = std_length(rt_get(&F_params, v469_s)); rt_lt(t728, t729); }))) {
      wv v476_p = rt_bind(({ wv t730 = v475_k; wv t731 = rt_get(&F_params, v469_s); rt_idx(t730, t731); }), 0);
      (void)({ wv t736 = v474_ps; wv t737 = rt_get(&F_name, v476_p); wv t738 = ({ wv t732 = rt_get(&F_name, v476_p); wv t733 = rt_chk(w5_modeOf(v476_p), "stmt"); wv t734 = rt_get(&F_pos, v476_p); wv t735 = rt_get(&F_id, v473_func); rt_chk(s392_decl(up, rt_s(&S373), t732, t733, t734, RT_NULL, t735, RT_NULL), "stmt"); }); rt_chk(s394_declare(up, t736, t737, t738), "stmt"); });
      v475_k = rt_add(v475_k, rt_i(1));
    }
    (void)({ wv t739 = rt_get(&F_body, v469_s); wv t740 = rt_chk(s391_newScope(up, v474_ps), "stmt"); wv t741 = rt_rec(0, 2, K45, (wv[]){ v473_func, rt_i(0) }); rt_chk(s397_block(up, t739, t740, t741), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v472_kind, rt_s(&S34)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v471_ctx)))) {
      (void)rt_chk(s386_error(up, rt_get(&F_pos, v469_s), rt_s(&S374), RT_NULL), "stmt");
    }
    if (rt_cond(rt_ne(rt_get(&F_e, v469_s), RT_NULL))) {
      (void)({ wv t742 = rt_get(&F_e, v469_s); wv t743 = v470_scope; wv t744 = v471_ctx; rt_chk(s413_expr(up, t742, t743, t744), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v472_kind, rt_s(&S57)))) {
    (void)({ wv t745 = rt_get(&F_e, v469_s); wv t746 = v470_scope; wv t747 = v471_ctx; rt_chk(s413_expr(up, t745, t746, t747), "stmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_eq(v472_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v472_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_get(&F_loops, v471_ctx), rt_i(0)))) {
      (void)({ wv t748 = rt_get(&F_pos, v469_s); wv t749 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text((rt_cond(rt_eq(v472_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43))), rt_s(&S375) }); rt_chk(s386_error(up, t748, t749, RT_NULL), "stmt"); });
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v472_kind, rt_s(&S41)))) {
    (void)({ wv t750 = rt_get(&F_cond, v469_s); wv t751 = v470_scope; wv t752 = v471_ctx; rt_chk(s413_expr(up, t750, t751, t752), "stmt"); });
    (void)({ wv t755 = rt_get(&F_body, v469_s); wv t756 = rt_chk(s391_newScope(up, v470_scope), "stmt"); wv t757 = ({ wv t753 = rt_get(&F_func, v471_ctx); wv t754 = rt_add(rt_get(&F_loops, v471_ctx), rt_i(1)); rt_rec(0, 2, K45, (wv[]){ t753, t754 }); }); rt_chk(s397_block(up, t755, t756, t757), "stmt"); });
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v472_kind, rt_s(&S55)))) {
    if (rt_cond(rt_get(&F_top, rt_get(&F_func, v471_ctx)))) {
      (void)rt_chk(s386_error(up, rt_get(&F_pos, v469_s), rt_s(&S376), RT_NULL), "stmt");
    }
    RT_RET(RT_NULL);
  } else if (rt_cond(rt_eq(v472_kind, rt_s(&S54)))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v469_s), RT_NULL))) {
      (void)({ wv t758 = rt_get(&F_e, v469_s); wv t759 = v470_scope; wv t760 = v471_ctx; rt_chk(s413_expr(up, t758, t759, t760), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v472_kind, rt_s(&S176)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v469_s)), rt_s(&S183)))) {
      if (rt_cond(rt_get(&F_top, rt_get(&F_func, v471_ctx)))) {
        (void)rt_chk(s386_error(up, rt_get(&F_pos, rt_get(&F_e, v469_s)), rt_s(&S376), RT_NULL), "stmt");
      }
      (void)({ wv t761 = rt_get(&F_arg, rt_get(&F_e, v469_s)); wv t762 = v470_scope; wv t763 = v471_ctx; rt_chk(s413_expr(up, t761, t762, t763), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v469_s)), rt_s(&S184)))) {
      (void)({ wv t764 = rt_get(&F_e, v469_s); wv t765 = v470_scope; wv t766 = v471_ctx; rt_chk(s409_chain(up, t764, t765, t766, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v469_s)), rt_s(&S44)))) {
      (void)({ wv t767 = rt_get(&F_e, v469_s); wv t768 = v470_scope; wv t769 = v471_ctx; rt_chk(s400_checkMatch(up, t767, t768, t769, rt_b(1)), "stmt"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v469_s)), rt_s(&S182)))) {
      wv v477_k = rt_i(0);
      while (rt_cond(({ wv t770 = v477_k; wv t771 = std_length(rt_get(&F_args, rt_get(&F_e, v469_s))); rt_lt(t770, t771); }))) {
        (void)({ wv t774 = ({ wv t772 = v477_k; wv t773 = rt_get(&F_args, rt_get(&F_e, v469_s)); rt_idx(t772, t773); }); wv t775 = v470_scope; wv t776 = v471_ctx; rt_chk(s413_expr(up, t774, t775, t776), "stmt"); });
        v477_k = rt_add(v477_k, rt_i(1));
      }
      (void)({ wv t777 = rt_get(&F_e, v469_s); wv t778 = v470_scope; rt_chk(s414_callee(up, t777, t778, rt_b(0)), "stmt"); });
    } else {
      (void)({ wv t779 = rt_get(&F_e, v469_s); wv t780 = v470_scope; wv t781 = v471_ctx; rt_chk(s413_expr(up, t779, t780, t781), "stmt"); });
    }
  } else if (rt_cond(rt_eq(v472_kind, rt_s(&S177)))) {
    (void)({ wv t782 = rt_get(&F_e, v469_s); wv t783 = v470_scope; wv t784 = v471_ctx; rt_chk(s413_expr(up, t782, t783, t784), "stmt"); });
  } else if (rt_cond(rt_eq(v472_kind, rt_s(&S38)))) {
    (void)({ wv t785 = rt_get(&F_e, v469_s); wv t786 = v470_scope; wv t787 = v471_ctx; rt_chk(s413_expr(up, t785, t786, t787), "stmt"); });
    (void)({ wv t788 = rt_get(&F_target, v469_s); wv t789 = v470_scope; wv t790 = v471_ctx; rt_chk(s410_lvalue(up, t788, t789, t790), "stmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v469_s), RT_NULL))) {
    (void)rt_chk(s399_onfailHandler(up, v469_s, v470_scope, v471_ctx), "stmt");
  }
  RT_RET(RT_NULL);
}

static wv s399_onfailHandler(struct fr99 *up, wv v478_s, wv v479_scope, wv v480_ctx) {
  rt_enter("onfailHandler");
  v478_s = rt_bind(v478_s, 0);
  v479_scope = rt_bind(v479_scope, 0);
  v480_ctx = rt_bind(v480_ctx, 0);
  wv v481_h = rt_bind(rt_get(&F_handler, v478_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_e, v478_s), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v478_s)), rt_s(&S182)), "||")))) {
    (void)rt_chk(s386_error(up, rt_get(&F_pos, v481_h), rt_s(&S377), rt_s(&S378)), "onfailHandler");
  }
  wv v482_hs = rt_bind(rt_chk(s391_newScope(up, v479_scope), "onfailHandler"), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v481_h), RT_NULL))) {
    (void)({ wv t794 = v482_hs; wv t795 = rt_get(&F_param, v481_h); wv t796 = ({ wv t791 = rt_get(&F_param, v481_h); wv t792 = rt_get(&F_paramPos, v481_h); wv t793 = rt_get(&F_id, rt_get(&F_func, v480_ctx)); rt_chk(s392_decl(up, rt_s(&S56), t791, rt_s(&S13), t792, RT_NULL, t793, RT_NULL), "onfailHandler"); }); rt_chk(s394_declare(up, t794, t795, t796), "onfailHandler"); });
  }
  (void)({ wv t797 = rt_get(&F_body, v481_h); wv t798 = rt_chk(s391_newScope(up, v482_hs), "onfailHandler"); wv t799 = v480_ctx; rt_chk(s397_block(up, t797, t798, t799), "onfailHandler"); });
  if (rt_cond(({ wv t800 = rt_def(w5_onfailValueUsed, "onfailValueUsed"); wv t801 = rt_get(&F_k, v478_s); rt_chk(w1_contains(t800, t801), "onfailHandler"); }))) {
    (void)rt_chk(s390_requireValue(up, rt_get(&F_body, v481_h)), "onfailHandler");
  }
  RT_RET(RT_NULL);
}

static wv s400_checkMatch(struct fr99 *up, wv v483_e, wv v484_scope, wv v485_ctx, wv v486_stmtLevel) {
  rt_enter("checkMatch");
  v483_e = rt_bind(v483_e, 0);
  v484_scope = rt_bind(v484_scope, 0);
  v485_ctx = rt_bind(v485_ctx, 0);
  v486_stmtLevel = rt_bind(v486_stmtLevel, 0);
  (void)({ wv t802 = rt_get(&F_subject, v483_e); wv t803 = v484_scope; wv t804 = v485_ctx; rt_chk(s413_expr(up, t802, t803, t804), "checkMatch"); });
  wv v487_seen = rt_arr(0, NULL, 1);
  wv v488_sort = RT_NULL;
  wv v489_sortEnum = RT_NULL;
  wv v490_a = rt_i(0);
  while (rt_cond(({ wv t805 = v490_a; wv t806 = std_length(rt_get(&F_cases, v483_e)); rt_lt(t805, t806); }))) {
    wv v491_al = rt_bind(({ wv t807 = v490_a; wv t808 = rt_get(&F_cases, v483_e); rt_idx(t807, t808); }), 0);
    wv v492_j = rt_i(0);
    while (rt_cond(({ wv t809 = v492_j; wv t810 = std_length(rt_get(&F_values, v491_al)); rt_lt(t809, t810); }))) {
      wv v493_v = rt_bind(({ wv t811 = v492_j; wv t812 = rt_get(&F_values, v491_al); rt_idx(t811, t812); }), 0);
      wv v494_vs = rt_s(&S13);
      if (rt_cond(rt_eq(rt_get(&F_k, v493_v), rt_s(&S189)))) {
        wv v498_d = rt_bind(rt_chk(s407_caseEnum(up, v493_v, v484_scope), "checkMatch"), 0);
        v494_vs = rt_bind((rt_cond(rt_ne(v498_d, RT_NULL)) ? rt_chk(w5_enumText(rt_get(&F_rec, v493_v)), "checkMatch") : RT_NULL), 1);
        if (rt_cond(rt_eq(v488_sort, RT_NULL))) {
          v489_sortEnum = rt_bind(v498_d, 1);
        }
      }
      if (rt_cond(rt_eq(v488_sort, RT_NULL))) {
        v488_sort = rt_bind(v494_vs, 1);
      } else if (rt_cond(rt_b(rt_bool(rt_ne(v494_vs, RT_NULL), "&&") && rt_bool(rt_ne(v494_vs, v488_sort), "&&")))) {
        wv v499_message = rt_bind((rt_cond(rt_b(rt_bool(rt_eq(v488_sort, rt_s(&S13)), "||") || rt_bool(rt_eq(v494_vs, rt_s(&S13)), "||"))) ? rt_s(&S379) : ({ wv t813 = rt_text(v488_sort); wv t814 = rt_text(v494_vs); rt_cat(5, (wv[]){ rt_s(&S380), t813, rt_s(&S381), t814, rt_s(&S113) }); })), 0);
        (void)({ wv t815 = rt_get(&F_pos, v493_v); wv t816 = v499_message; rt_chk(s386_error(up, t815, t816, RT_NULL), "checkMatch"); });
      }
      wv v495_key = rt_bind(rt_chk(w5_caseKey(v493_v), "checkMatch"), 0);
      wv v496_first = RT_NULL;
      wv v497_q = rt_i(0);
      while (rt_cond(({ wv t817 = v497_q; wv t818 = std_length(v487_seen); rt_lt(t817, t818); }))) {
        if (rt_cond(({ wv t819 = rt_get(&F_key, rt_idx(v497_q, v487_seen)); wv t820 = v495_key; rt_eq(t819, t820); }))) {
          v496_first = rt_bind(rt_get(&F_pos, rt_idx(v497_q, v487_seen)), 1);
        }
        v497_q = rt_add(v497_q, rt_i(1));
      }
      if (rt_cond(rt_ne(v496_first, RT_NULL))) {
        (void)({ wv t823 = rt_get(&F_pos, v493_v); wv t824 = ({ wv t821 = rt_text(rt_chk(w5_caseText(v493_v), "checkMatch")); wv t822 = rt_text(rt_chk(w5_lineOf(v496_first), "checkMatch")); rt_cat(5, (wv[]){ rt_s(&S382), t821, rt_s(&S383), t822, rt_s(&S16) }); }); rt_chk(s386_error(up, t823, t824, RT_NULL), "checkMatch"); });
      } else {
        (void)({ wv t827 = v487_seen; wv t828 = ({ wv t825 = v495_key; wv t826 = rt_get(&F_pos, v493_v); rt_rec(0, 2, K46, (wv[]){ t825, t826 }); }); std_push(t827, t828); });
      }
      v492_j = rt_add(v492_j, rt_i(1));
    }
    (void)({ wv t829 = rt_get(&F_body, v491_al); wv t830 = rt_chk(s391_newScope(up, v484_scope), "checkMatch"); wv t831 = v485_ctx; rt_chk(s397_block(up, t829, t830, t831), "checkMatch"); });
    if (rt_cond(rt_not(v486_stmtLevel))) {
      (void)rt_chk(s390_requireValue(up, rt_get(&F_body, v491_al)), "checkMatch");
    }
    v490_a = rt_add(v490_a, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v483_e), RT_NULL))) {
    (void)({ wv t832 = rt_get(&F_els, v483_e); wv t833 = rt_chk(s391_newScope(up, v484_scope), "checkMatch"); wv t834 = v485_ctx; rt_chk(s397_block(up, t832, t833, t834), "checkMatch"); });
    if (rt_cond(rt_not(v486_stmtLevel))) {
      (void)rt_chk(s390_requireValue(up, rt_get(&F_els, v483_e)), "checkMatch");
    }
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v488_sort, RT_NULL), "&&") && rt_bool(rt_ne(v488_sort, rt_s(&S13)), "&&")))) {
    wv v500_ms = rt_bind(rt_chk(s388_enumMembers(up, v489_sortEnum), "checkMatch"), 0);
    wv v501_missing = rt_arr(0, NULL, 1);
    wv v502_m = rt_i(0);
    while (rt_cond(({ wv t835 = v502_m; wv t836 = std_length(v500_ms); rt_lt(t835, t836); }))) {
      wv v503_key = ({ wv t837 = rt_text(rt_get(&F_name, rt_idx(v502_m, v500_ms))); wv t838 = rt_text(v488_sort); rt_cat(4, (wv[]){ rt_s(&S364), t837, rt_s(&S129), t838 }); });
      wv v504_handled = rt_b(0);
      wv v505_q = rt_i(0);
      while (rt_cond(({ wv t839 = v505_q; wv t840 = std_length(v487_seen); rt_lt(t839, t840); }))) {
        if (rt_cond(({ wv t841 = rt_get(&F_key, rt_idx(v505_q, v487_seen)); wv t842 = v503_key; rt_eq(t841, t842); }))) {
          v504_handled = rt_b(1);
        }
        v505_q = rt_add(v505_q, rt_i(1));
      }
      if (rt_cond(rt_not(v504_handled))) {
        (void)({ wv t845 = v501_missing; wv t846 = ({ wv t843 = rt_text(v488_sort); wv t844 = rt_text(rt_get(&F_name, rt_idx(v502_m, v500_ms))); rt_cat(5, (wv[]){ rt_s(&S113), t843, rt_s(&S129), t844, rt_s(&S113) }); }); std_push(t845, t846); });
      }
      v502_m = rt_add(v502_m, rt_i(1));
    }
    if (rt_cond(rt_gt(std_length(v501_missing), rt_i(0)))) {
      (void)({ wv t847 = rt_get(&F_pos, v483_e); wv t848 = rt_cat(2, (wv[]){ rt_s(&S384), rt_text(std_join(v501_missing, rt_s(&S385))) }); rt_chk(s386_error(up, t847, t848, rt_s(&S386)), "checkMatch"); });
    }
  } else if (rt_cond(rt_not(v486_stmtLevel))) {
    (void)rt_chk(s386_error(up, rt_get(&F_pos, v483_e), rt_s(&S387), RT_NULL), "checkMatch");
  }
  RT_RET(RT_NULL);
}

static wv s401_moduleNamed(struct fr99 *up, wv v506_x, wv v507_scope) {
  rt_enter("moduleNamed");
  v507_scope = rt_bind(v507_scope, 0);
  if (rt_cond(rt_ne(rt_get(&F_k, v506_x), rt_s(&S211)))) {
    RT_RET(RT_NULL);
  }
  wv v508_d = rt_bind(({ wv t849 = v507_scope; wv t850 = rt_get(&F_name, v506_x); rt_chk(s395_lookup(up, t849, t850), "moduleNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v508_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v508_d), rt_s(&S388)), "&&"))) ? v508_d : RT_NULL));
}

static wv s402_exported(struct fr99 *up, wv v509_lib, wv v510_name) {
  rt_enter("exported");
  v509_lib = rt_bind(v509_lib, 0);
  v510_name = rt_bind(v510_name, 0);
  wv v511_xs = rt_bind(rt_get(&F_exports, rt_idx(v509_lib, up->v378_modules)), 0);
  wv v512_k = rt_i(0);
  while (rt_cond(({ wv t851 = v512_k; wv t852 = std_length(v511_xs); rt_lt(t851, t852); }))) {
    if (rt_cond(({ wv t853 = rt_get(&F_name, rt_idx(v512_k, v511_xs)); wv t854 = v510_name; rt_eq(t853, t854); }))) {
      RT_RET(rt_idx(v512_k, v511_xs));
    }
    v512_k = rt_add(v512_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s403_enumNamed(struct fr99 *up, wv v513_x, wv v514_scope) {
  rt_enter("enumNamed");
  v514_scope = rt_bind(v514_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v513_x), rt_s(&S211)))) {
    wv v517_d = rt_bind(({ wv t855 = v514_scope; wv t856 = rt_get(&F_name, v513_x); rt_chk(s395_lookup(up, t855, t856), "enumNamed"); }), 0);
    RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v517_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v517_d), rt_s(&S46)), "&&"))) ? v517_d : RT_NULL));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v513_x), rt_s(&S189)))) {
    RT_RET(RT_NULL);
  }
  wv v515_m = rt_bind(({ wv t857 = rt_get(&F_rec, v513_x); wv t858 = v514_scope; rt_chk(s401_moduleNamed(up, t857, t858), "enumNamed"); }), 0);
  if (rt_cond(rt_b(rt_bool(rt_eq(v515_m, RT_NULL), "||") || rt_bool(rt_eq(rt_get(&F_lib, v515_m), RT_NULL), "||")))) {
    RT_RET(RT_NULL);
  }
  wv v516_d = rt_bind(({ wv t859 = rt_get(&F_lib, v515_m); wv t860 = rt_get(&F_field, v513_x); rt_chk(s402_exported(up, t859, t860), "enumNamed"); }), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v516_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v516_d), rt_s(&S46)), "&&"))) ? v516_d : RT_NULL));
}

static wv s404_exportOf(struct fr99 *up, wv v518_e, wv v519_m) {
  rt_enter("exportOf");
  v518_e = rt_bind(v518_e, 0);
  v519_m = rt_bind(v519_m, 0);
  if (rt_cond(rt_eq(rt_get(&F_lib, v519_m), RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v520_d = rt_bind(({ wv t861 = rt_get(&F_lib, v519_m); wv t862 = rt_get(&F_field, v518_e); rt_chk(s402_exported(up, t861, t862), "exportOf"); }), 0);
  if (rt_cond(rt_eq(v520_d, RT_NULL))) {
    (void)({ wv t867 = rt_get(&F_pos, v518_e); wv t868 = ({ wv t865 = rt_text(rt_get(&F_path, ({ wv t863 = rt_get(&F_lib, v519_m); wv t864 = up->v378_modules; rt_idx(t863, t864); }))); wv t866 = rt_text(rt_get(&F_field, v518_e)); rt_cat(5, (wv[]){ rt_s(&S113), t865, rt_s(&S389), t866, rt_s(&S113) }); }); rt_chk(s386_error(up, t867, t868, RT_NULL), "exportOf"); });
  }
  RT_RET(v520_d);
}

static wv s405_unknownModule(struct fr99 *up, wv v521_x, wv v522_scope) {
  rt_enter("unknownModule");
  v521_x = rt_bind(v521_x, 0);
  v522_scope = rt_bind(v522_scope, 0);
  wv v523_message = rt_bind((rt_cond(rt_ne(({ wv t869 = v522_scope; wv t870 = rt_get(&F_name, v521_x); rt_chk(s395_lookup(up, t869, t870), "unknownModule"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v521_x)), rt_s(&S390) }) : rt_cat(3, (wv[]){ rt_s(&S391), rt_text(rt_get(&F_name, v521_x)), rt_s(&S113) })), 0);
  (void)({ wv t871 = rt_get(&F_pos, v521_x); wv t872 = v523_message; rt_chk(s386_error(up, t871, t872, RT_NULL), "unknownModule"); });
  RT_RET(RT_NULL);
}

static wv s406_qualified(struct fr99 *up, wv v524_e, wv v525_m) {
  rt_enter("qualified");
  v524_e = rt_bind(v524_e, 0);
  v525_m = rt_bind(v525_m, 0);
  wv v526_d = rt_bind(rt_chk(s404_exportOf(up, v524_e, v525_m), "qualified"), 0);
  wv v527_label = ({ wv t873 = rt_text(rt_get(&F_name, rt_get(&F_rec, v524_e))); wv t874 = rt_text(rt_get(&F_field, v524_e)); rt_cat(3, (wv[]){ t873, rt_s(&S129), t874 }); });
  if (rt_cond(rt_b(rt_bool(rt_ne(v526_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v526_d), rt_s(&S33)), "&&")))) {
    (void)({ wv t877 = rt_get(&F_pos, v524_e); wv t878 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v527_label), rt_s(&S392) }); wv t879 = ({ wv t875 = rt_text(v527_label); wv t876 = rt_text(v527_label); rt_cat(5, (wv[]){ rt_s(&S393), t875, rt_s(&S394), t876, rt_s(&S395) }); }); rt_chk(s386_error(up, t877, t878, t879), "qualified"); });
  } else if (rt_cond(rt_b(rt_bool(rt_ne(v526_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v526_d), rt_s(&S46)), "&&")))) {
    (void)({ wv t880 = rt_get(&F_pos, v524_e); wv t881 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v527_label), rt_s(&S396) }); wv t882 = rt_cat(3, (wv[]){ rt_s(&S397), rt_text(v527_label), rt_s(&S398) }); rt_chk(s386_error(up, t880, t881, t882), "qualified"); });
  }
  RT_RET(RT_NULL);
}

static wv s407_caseEnum(struct fr99 *up, wv v528_v, wv v529_scope) {
  rt_enter("caseEnum");
  v528_v = rt_bind(v528_v, 0);
  v529_scope = rt_bind(v529_scope, 0);
  wv v530_x = rt_bind(rt_get(&F_rec, v528_v), 0);
  wv v531_d = rt_bind(rt_chk(s403_enumNamed(up, v530_x, v529_scope), "caseEnum"), 0);
  if (rt_cond(rt_ne(v531_d, RT_NULL))) {
    (void)rt_chk(s408_member(up, v528_v, v531_d), "caseEnum");
    RT_RET(v531_d);
  }
  wv v532_hint = rt_s(&S399);
  if (rt_cond(rt_eq(rt_get(&F_k, v530_x), rt_s(&S211)))) {
    wv v534_message = rt_bind((rt_cond(rt_ne(({ wv t883 = v529_scope; wv t884 = rt_get(&F_name, v530_x); rt_chk(s395_lookup(up, t883, t884), "caseEnum"); }), RT_NULL)) ? rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v530_x)), rt_s(&S400) }) : rt_cat(3, (wv[]){ rt_s(&S401), rt_text(rt_get(&F_name, v530_x)), rt_s(&S113) })), 0);
    (void)({ wv t885 = rt_get(&F_pos, v530_x); wv t886 = v534_message; wv t887 = v532_hint; rt_chk(s386_error(up, t885, t886, t887), "caseEnum"); });
    RT_RET(RT_NULL);
  }
  wv v533_m = rt_bind(({ wv t888 = rt_get(&F_rec, v530_x); wv t889 = v529_scope; rt_chk(s401_moduleNamed(up, t888, t889), "caseEnum"); }), 0);
  if (rt_cond(rt_eq(v533_m, RT_NULL))) {
    (void)({ wv t890 = rt_get(&F_rec, v530_x); wv t891 = v529_scope; rt_chk(s405_unknownModule(up, t890, t891), "caseEnum"); });
  } else if (rt_cond(rt_ne(rt_chk(s404_exportOf(up, v530_x, v533_m), "caseEnum"), RT_NULL))) {
    (void)({ wv t892 = rt_get(&F_pos, v530_x); wv t893 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_chk(w5_enumText(v530_x), "caseEnum")), rt_s(&S400) }); wv t894 = v532_hint; rt_chk(s386_error(up, t892, t893, t894), "caseEnum"); });
  }
  RT_RET(RT_NULL);
}

static wv s408_member(struct fr99 *up, wv v535_e, wv v536_d) {
  rt_enter("member");
  v535_e = rt_bind(v535_e, 0);
  v536_d = rt_bind(v536_d, 0);
  wv v537_ms = rt_bind(rt_chk(s388_enumMembers(up, v536_d), "member"), 0);
  wv v538_names = rt_arr(0, NULL, 1);
  wv v539_found = rt_b(0);
  wv v540_k = rt_i(0);
  while (rt_cond(({ wv t895 = v540_k; wv t896 = std_length(v537_ms); rt_lt(t895, t896); }))) {
    (void)({ wv t897 = v538_names; wv t898 = rt_get(&F_name, rt_idx(v540_k, v537_ms)); std_push(t897, t898); });
    if (rt_cond(({ wv t899 = rt_get(&F_name, rt_idx(v540_k, v537_ms)); wv t900 = rt_get(&F_field, v535_e); rt_eq(t899, t900); }))) {
      v539_found = rt_b(1);
    }
    v540_k = rt_add(v540_k, rt_i(1));
  }
  if (rt_cond(rt_not(v539_found))) {
    (void)({ wv t903 = rt_get(&F_pos, v535_e); wv t904 = ({ wv t901 = rt_text(rt_get(&F_field, v535_e)); wv t902 = rt_text(rt_get(&F_name, v536_d)); rt_cat(5, (wv[]){ rt_s(&S402), t901, rt_s(&S403), t902, rt_s(&S113) }); }); wv t905 = rt_cat(2, (wv[]){ rt_s(&S404), rt_text(std_join(v538_names, rt_s(&S385))) }); rt_chk(s386_error(up, t903, t904, t905), "member"); });
  }
  RT_RET(RT_NULL);
}

static wv s409_chain(struct fr99 *up, wv v541_e, wv v542_scope, wv v543_ctx, wv v544_stmtLevel) {
  rt_enter("chain");
  v541_e = rt_bind(v541_e, 0);
  v542_scope = rt_bind(v542_scope, 0);
  v543_ctx = rt_bind(v543_ctx, 0);
  v544_stmtLevel = rt_bind(v544_stmtLevel, 0);
  wv v545_k = rt_i(0);
  while (rt_cond(({ wv t906 = v545_k; wv t907 = std_length(rt_get(&F_branches, v541_e)); rt_lt(t906, t907); }))) {
    wv v546_b = rt_bind(({ wv t908 = v545_k; wv t909 = rt_get(&F_branches, v541_e); rt_idx(t908, t909); }), 0);
    (void)({ wv t910 = rt_get(&F_cond, v546_b); wv t911 = v542_scope; wv t912 = v543_ctx; rt_chk(s413_expr(up, t910, t911, t912), "chain"); });
    (void)({ wv t913 = rt_get(&F_body, v546_b); wv t914 = rt_chk(s391_newScope(up, v542_scope), "chain"); wv t915 = v543_ctx; rt_chk(s397_block(up, t913, t914, t915), "chain"); });
    if (rt_cond(rt_not(v544_stmtLevel))) {
      (void)rt_chk(s390_requireValue(up, rt_get(&F_body, v546_b)), "chain");
    }
    v545_k = rt_add(v545_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v541_e), RT_NULL))) {
    (void)({ wv t916 = rt_get(&F_els, v541_e); wv t917 = rt_chk(s391_newScope(up, v542_scope), "chain"); wv t918 = v543_ctx; rt_chk(s397_block(up, t916, t917, t918), "chain"); });
    if (rt_cond(rt_not(v544_stmtLevel))) {
      (void)rt_chk(s390_requireValue(up, rt_get(&F_els, v541_e)), "chain");
    }
  } else if (rt_cond(rt_not(v544_stmtLevel))) {
    (void)rt_chk(s386_error(up, rt_get(&F_pos, v541_e), rt_s(&S405), RT_NULL), "chain");
  }
  RT_RET(RT_NULL);
}

static wv s410_lvalue(struct fr99 *up, wv v547_t, wv v548_scope, wv v549_ctx) {
  rt_enter("lvalue");
  v548_scope = rt_bind(v548_scope, 0);
  v549_ctx = rt_bind(v549_ctx, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v547_t), rt_s(&S211)))) {
    wv v553_d = rt_bind(({ wv t919 = rt_get(&F_name, v547_t); wv t920 = rt_get(&F_pos, v547_t); wv t921 = v548_scope; wv t922 = v549_ctx; rt_chk(s396_resolve(up, t919, t920, t921, t922), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v553_d, RT_NULL))) {
      (void)({ wv t923 = v553_d; wv t924 = rt_get(&F_name, v547_t); wv t925 = rt_get(&F_pos, v547_t); wv t926 = rt_cat(3, (wv[]){ rt_s(&S406), rt_text(rt_get(&F_name, v547_t)), rt_s(&S407) }); rt_chk(s411_mustBeMutable(up, t923, t924, t925, t926, rt_b(1)), "lvalue"); });
      if (rt_cond(rt_get(&F_isRef, v553_d))) {
        (void)({ wv t929 = rt_get(&F_refTargets, ({ wv t927 = rt_def(up->v382_curMod, "curMod"); wv t928 = up->v378_modules; rt_idx(t927, t928); })); wv t930 = rt_get(&F_pos, v547_t); std_push(t929, t930); });
      }
    }
    RT_RET(RT_NULL);
  }
  wv v550_isMem = rt_eq(rt_get(&F_k, v547_t), rt_s(&S189));
  wv v551_box = rt_bind((rt_cond(v550_isMem) ? rt_get(&F_rec, v547_t) : rt_get(&F_arr, v547_t)), 0);
  wv v552_what = rt_bind((rt_cond(v550_isMem) ? rt_s(&S408) : rt_s(&S409)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v547_t), rt_s(&S188)))) {
    (void)({ wv t931 = rt_get(&F_index, v547_t); wv t932 = v548_scope; wv t933 = v549_ctx; rt_chk(s413_expr(up, t931, t932, t933), "lvalue"); });
  }
  if (rt_cond(v550_isMem)) {
    if (rt_cond(rt_ne(rt_chk(s403_enumNamed(up, v551_box, v548_scope), "lvalue"), RT_NULL))) {
      (void)({ wv t936 = rt_get(&F_pos, v547_t); wv t937 = ({ wv t934 = rt_text(rt_chk(w5_enumText(v551_box), "lvalue")); wv t935 = rt_text(rt_get(&F_field, v547_t)); rt_cat(5, (wv[]){ rt_s(&S406), t934, rt_s(&S129), t935, rt_s(&S410) }); }); rt_chk(s386_error(up, t936, t937, RT_NULL), "lvalue"); });
      RT_RET(RT_NULL);
    }
    wv v554_m = rt_bind(rt_chk(s401_moduleNamed(up, v551_box, v548_scope), "lvalue"), 0);
    if (rt_cond(rt_ne(v554_m, RT_NULL))) {
      if (rt_cond(rt_ne(rt_chk(s404_exportOf(up, v547_t, v554_m), "lvalue"), RT_NULL))) {
        (void)({ wv t943 = rt_get(&F_pos, v547_t); wv t944 = ({ wv t940 = rt_text(rt_get(&F_name, v551_box)); wv t941 = rt_text(rt_get(&F_field, v547_t)); wv t942 = rt_text(rt_get(&F_path, ({ wv t938 = rt_get(&F_lib, v554_m); wv t939 = up->v378_modules; rt_idx(t938, t939); }))); rt_cat(7, (wv[]){ rt_s(&S406), t940, rt_s(&S129), t941, rt_s(&S411), t942, rt_s(&S412) }); }); rt_chk(s386_error(up, t943, t944, RT_NULL), "lvalue"); });
      }
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v551_box), rt_s(&S211)))) {
    wv v555_d = rt_bind(({ wv t945 = rt_get(&F_name, v551_box); wv t946 = rt_get(&F_pos, v551_box); wv t947 = v548_scope; wv t948 = v549_ctx; rt_chk(s396_resolve(up, t945, t946, t947, t948), "lvalue"); }), 0);
    if (rt_cond(rt_ne(v555_d, RT_NULL))) {
      (void)({ wv t951 = v555_d; wv t952 = rt_get(&F_name, v551_box); wv t953 = rt_get(&F_pos, v551_box); wv t954 = ({ wv t949 = rt_text(v552_what); wv t950 = rt_text(rt_get(&F_name, v551_box)); rt_cat(5, (wv[]){ rt_s(&S413), t949, rt_s(&S414), t950, rt_s(&S415) }); }); rt_chk(s411_mustBeMutable(up, t951, t952, t953, t954, rt_b(0)), "lvalue"); });
    }
  } else {
    (void)rt_chk(s413_expr(up, v551_box, v548_scope, v549_ctx), "lvalue");
  }
  RT_RET(RT_NULL);
}

static wv s411_mustBeMutable(struct fr99 *up, wv v556_d, wv v557_name, wv v558_pos, wv v559_message, wv v560_rebind) {
  rt_enter("mustBeMutable");
  v556_d = rt_bind(v556_d, 0);
  v557_name = rt_bind(v557_name, 0);
  v558_pos = rt_bind(v558_pos, 0);
  v559_message = rt_bind(v559_message, 0);
  v560_rebind = rt_bind(v560_rebind, 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_kind, v556_d), rt_s(&S33)), "||") || rt_bool(rt_eq(rt_get(&F_kind, v556_d), rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_kind, v556_d), rt_s(&S388)), "||")))) {
    (void)({ wv t957 = v558_pos; wv t958 = ({ wv t955 = rt_text(v557_name); wv t956 = rt_text(rt_chk(w5_kindText(rt_get(&F_kind, v556_d)), "mustBeMutable")); rt_cat(5, (wv[]){ rt_s(&S113), t955, rt_s(&S416), t956, rt_s(&S417) }); }); rt_chk(s386_error(up, t957, t958, RT_NULL), "mustBeMutable"); });
  } else if (rt_cond(rt_b(rt_bool(rt_not(rt_get(&F_isVar, v556_d)), "&&") && rt_bool(rt_not(rt_get(&F_isRef, v556_d)), "&&")))) {
    wv v561_hint = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_kind, v556_d), rt_s(&S373)))) {
      v561_hint = rt_bind((rt_cond(v560_rebind) ? ({ wv t959 = rt_text(v557_name); wv t960 = rt_text(v557_name); rt_cat(5, (wv[]){ rt_s(&S418), t959, rt_s(&S419), t960, rt_s(&S420) }); }) : rt_cat(3, (wv[]){ rt_s(&S421), rt_text(v557_name), rt_s(&S16) })), 1);
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v556_d), rt_s(&S177)))) {
      v561_hint = rt_bind((rt_cond(v560_rebind) ? ({ wv t961 = rt_text(v557_name); wv t962 = rt_text(v557_name); rt_cat(5, (wv[]){ rt_s(&S422), t961, rt_s(&S423), t962, rt_s(&S424) }); }) : rt_cat(3, (wv[]){ rt_s(&S425), rt_text(v557_name), rt_s(&S426) })), 1);
    }
    (void)rt_chk(s386_error(up, v558_pos, v559_message, v561_hint), "mustBeMutable");
  }
  RT_RET(RT_NULL);
}

static wv s412_unknownStd(struct fr99 *up, wv v562_node) {
  rt_enter("unknownStd");
  v562_node = rt_bind(v562_node, 0);
  (void)({ wv t963 = rt_get(&F_pos, v562_node); wv t964 = rt_cat(3, (wv[]){ rt_s(&S427), rt_text(rt_get(&F_name, v562_node)), rt_s(&S113) }); rt_chk(s386_error(up, t963, t964, rt_s(&S136)), "unknownStd"); });
  RT_RET(RT_NULL);
}

static wv s413_expr(struct fr99 *up, wv v563_e, wv v564_scope, wv v565_ctx) {
  rt_enter("expr");
  v564_scope = rt_bind(v564_scope, 0);
  v565_ctx = rt_bind(v565_ctx, 0);
  wv v566_kind = rt_bind(rt_get(&F_k, v563_e), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v566_kind, rt_s(&S138)), "||") || rt_bool(rt_eq(v566_kind, rt_s(&S148)), "||")), "||") || rt_bool(rt_eq(v566_kind, rt_s(&S281)), "||")), "||") || rt_bool(rt_eq(v566_kind, rt_s(&S51)), "||")), "||") || rt_bool(rt_eq(v566_kind, rt_s(&S52)), "||")), "||") || rt_bool(rt_eq(v566_kind, rt_s(&S53)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S171)))) {
    wv v567_k = rt_i(0);
    while (rt_cond(({ wv t965 = v567_k; wv t966 = std_length(rt_get(&F_parts, v563_e)); rt_lt(t965, t966); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t967 = v567_k; wv t968 = rt_get(&F_parts, v563_e); rt_idx(t967, t968); })), rt_s(&S185)))) {
        (void)({ wv t971 = ({ wv t969 = v567_k; wv t970 = rt_get(&F_parts, v563_e); rt_idx(t969, t970); }); wv t972 = v564_scope; wv t973 = v565_ctx; rt_chk(s413_expr(up, t971, t972, t973), "expr"); });
      }
      v567_k = rt_add(v567_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S211)))) {
    wv v568_d = rt_bind(({ wv t974 = rt_get(&F_name, v563_e); wv t975 = rt_get(&F_pos, v563_e); wv t976 = v564_scope; wv t977 = v565_ctx; rt_chk(s396_resolve(up, t974, t975, t976, t977), "expr"); }), 0);
    if (rt_cond(rt_b(rt_bool(rt_ne(v568_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v568_d), rt_s(&S33)), "&&")))) {
      (void)({ wv t980 = rt_get(&F_pos, v563_e); wv t981 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v563_e)), rt_s(&S392) }); wv t982 = ({ wv t978 = rt_text(rt_get(&F_name, v563_e)); wv t979 = rt_text(rt_get(&F_name, v563_e)); rt_cat(5, (wv[]){ rt_s(&S393), t978, rt_s(&S394), t979, rt_s(&S395) }); }); rt_chk(s386_error(up, t980, t981, t982), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v568_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v568_d), rt_s(&S46)), "&&")))) {
      (void)({ wv t983 = rt_get(&F_pos, v563_e); wv t984 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v563_e)), rt_s(&S396) }); wv t985 = rt_cat(3, (wv[]){ rt_s(&S397), rt_text(rt_get(&F_name, v563_e)), rt_s(&S398) }); rt_chk(s386_error(up, t983, t984, t985), "expr"); });
    } else if (rt_cond(rt_b(rt_bool(rt_ne(v568_d, RT_NULL), "&&") && rt_bool(rt_eq(rt_get(&F_kind, v568_d), rt_s(&S388)), "&&")))) {
      (void)({ wv t986 = rt_get(&F_pos, v563_e); wv t987 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(rt_get(&F_name, v563_e)), rt_s(&S428) }); wv t988 = rt_cat(3, (wv[]){ rt_s(&S429), rt_text(rt_get(&F_name, v563_e)), rt_s(&S430) }); rt_chk(s386_error(up, t986, t987, t988), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S156)))) {
    wv v569_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v563_e)), "expr"), 0);
    if (rt_cond(rt_eq(v569_sig, RT_NULL))) {
      (void)rt_chk(s412_unknownStd(up, v563_e), "expr");
    } else if (rt_cond(rt_ne(v569_sig, rt_s(&S35)))) {
      (void)({ wv t991 = rt_get(&F_pos, v563_e); wv t992 = ({ wv t989 = rt_text(rt_get(&F_name, v563_e)); wv t990 = rt_text(rt_get(&F_name, v563_e)); rt_cat(5, (wv[]){ rt_s(&S179), t989, rt_s(&S431), t990, rt_s(&S16) }); }); rt_chk(s386_error(up, t991, t992, RT_NULL), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S186)))) {
    wv v570_k = rt_i(0);
    while (rt_cond(({ wv t993 = v570_k; wv t994 = std_length(rt_get(&F_items, v563_e)); rt_lt(t993, t994); }))) {
      (void)({ wv t997 = ({ wv t995 = v570_k; wv t996 = rt_get(&F_items, v563_e); rt_idx(t995, t996); }); wv t998 = v564_scope; wv t999 = v565_ctx; rt_chk(s413_expr(up, t997, t998, t999), "expr"); });
      v570_k = rt_add(v570_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S187)))) {
    wv v571_k = rt_i(0);
    while (rt_cond(({ wv t1000 = v571_k; wv t1001 = std_length(rt_get(&F_fields, v563_e)); rt_lt(t1000, t1001); }))) {
      (void)({ wv t1004 = rt_get(&F_e, ({ wv t1002 = v571_k; wv t1003 = rt_get(&F_fields, v563_e); rt_idx(t1002, t1003); })); wv t1005 = v564_scope; wv t1006 = v565_ctx; rt_chk(s413_expr(up, t1004, t1005, t1006), "expr"); });
      v571_k = rt_add(v571_k, rt_i(1));
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S188)))) {
    (void)({ wv t1007 = rt_get(&F_index, v563_e); wv t1008 = v564_scope; wv t1009 = v565_ctx; rt_chk(s413_expr(up, t1007, t1008, t1009), "expr"); });
    (void)({ wv t1010 = rt_get(&F_arr, v563_e); wv t1011 = v564_scope; wv t1012 = v565_ctx; rt_chk(s413_expr(up, t1010, t1011, t1012), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S189)))) {
    wv v572_lin = rt_bind(({ wv t1013 = rt_get(&F_rec, v563_e); wv t1014 = v564_scope; rt_chk(s403_enumNamed(up, t1013, t1014), "expr"); }), 0);
    if (rt_cond(rt_ne(v572_lin, RT_NULL))) {
      (void)rt_chk(s408_member(up, v563_e, v572_lin), "expr");
      RT_RET(RT_NULL);
    }
    wv v573_m = rt_bind(({ wv t1015 = rt_get(&F_rec, v563_e); wv t1016 = v564_scope; rt_chk(s401_moduleNamed(up, t1015, t1016), "expr"); }), 0);
    if (rt_cond(rt_ne(v573_m, RT_NULL))) {
      (void)rt_chk(s406_qualified(up, v563_e, v573_m), "expr");
    } else {
      (void)({ wv t1017 = rt_get(&F_rec, v563_e); wv t1018 = v564_scope; wv t1019 = v565_ctx; rt_chk(s413_expr(up, t1017, t1018, t1019), "expr"); });
    }
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S190)))) {
    (void)({ wv t1020 = rt_get(&F_e, v563_e); wv t1021 = v564_scope; wv t1022 = v565_ctx; rt_chk(s413_expr(up, t1020, t1021, t1022), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S191)))) {
    (void)({ wv t1023 = rt_get(&F_l, v563_e); wv t1024 = v564_scope; wv t1025 = v565_ctx; rt_chk(s413_expr(up, t1023, t1024, t1025), "expr"); });
    (void)({ wv t1026 = rt_get(&F_r, v563_e); wv t1027 = v564_scope; wv t1028 = v565_ctx; rt_chk(s413_expr(up, t1026, t1027, t1028), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S192)))) {
    (void)({ wv t1029 = rt_get(&F_c, v563_e); wv t1030 = v564_scope; wv t1031 = v565_ctx; rt_chk(s413_expr(up, t1029, t1030, t1031), "expr"); });
    (void)({ wv t1032 = rt_get(&F_a, v563_e); wv t1033 = v564_scope; wv t1034 = v565_ctx; rt_chk(s413_expr(up, t1032, t1033, t1034), "expr"); });
    (void)({ wv t1035 = rt_get(&F_b, v563_e); wv t1036 = v564_scope; wv t1037 = v565_ctx; rt_chk(s413_expr(up, t1035, t1036, t1037), "expr"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S182)))) {
    wv v574_k = rt_i(0);
    while (rt_cond(({ wv t1038 = v574_k; wv t1039 = std_length(rt_get(&F_args, v563_e)); rt_lt(t1038, t1039); }))) {
      (void)({ wv t1042 = ({ wv t1040 = v574_k; wv t1041 = rt_get(&F_args, v563_e); rt_idx(t1040, t1041); }); wv t1043 = v564_scope; wv t1044 = v565_ctx; rt_chk(s413_expr(up, t1042, t1043, t1044), "expr"); });
      v574_k = rt_add(v574_k, rt_i(1));
    }
    (void)rt_chk(s414_callee(up, v563_e, v564_scope, rt_b(1)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S183)))) {
    (void)rt_chk(s386_error(up, rt_get(&F_pos, v563_e), rt_s(&S432), RT_NULL), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S44)))) {
    (void)rt_chk(s400_checkMatch(up, v563_e, v564_scope, v565_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v566_kind, rt_s(&S184)))) {
    (void)rt_chk(s409_chain(up, v563_e, v564_scope, v565_ctx, rt_b(0)), "expr");
    RT_RET(RT_NULL);
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S433), rt_text(v566_kind) }));
  RT_RET(RT_NULL);
}

static wv s414_callee(struct fr99 *up, wv v575_e, wv v576_scope, wv v577_used) {
  rt_enter("callee");
  v575_e = rt_bind(v575_e, 0);
  v576_scope = rt_bind(v576_scope, 0);
  v577_used = rt_bind(v577_used, 0);
  wv v578_c = rt_bind(rt_get(&F_callee, v575_e), 0);
  wv v579_n = rt_bind(std_length(rt_get(&F_args, v575_e)), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v578_c), rt_s(&S156)))) {
    wv v582_sig = rt_bind(rt_chk(w5_stdSig(rt_get(&F_name, v578_c)), "callee"), 0);
    if (rt_cond(rt_eq(v582_sig, RT_NULL))) {
      (void)rt_chk(s412_unknownStd(up, v578_c), "callee");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(v582_sig, rt_s(&S35)))) {
      (void)({ wv t1045 = rt_get(&F_pos, v578_c); wv t1046 = rt_cat(3, (wv[]){ rt_s(&S179), rt_text(rt_get(&F_name, v578_c)), rt_s(&S434) }); rt_chk(s386_error(up, t1045, t1046, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_ne(v582_sig, v579_n))) {
      (void)({ wv t1050 = rt_get(&F_pos, v578_c); wv t1051 = ({ wv t1047 = rt_text(rt_get(&F_name, v578_c)); wv t1048 = rt_text(rt_chk(w5_plural(v582_sig), "callee")); wv t1049 = rt_text(v579_n); rt_cat(6, (wv[]){ rt_s(&S179), t1047, rt_s(&S435), t1048, rt_s(&S436), t1049 }); }); rt_chk(s386_error(up, t1050, t1051, RT_NULL), "callee"); });
    }
    RT_RET(RT_NULL);
  }
  wv v580_d = RT_NULL;
  wv v581_label = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v578_c), rt_s(&S189)))) {
    v581_label = ({ wv t1052 = rt_text(rt_get(&F_name, rt_get(&F_rec, v578_c))); wv t1053 = rt_text(rt_get(&F_field, v578_c)); rt_cat(3, (wv[]){ t1052, rt_s(&S129), t1053 }); });
    wv v583_m = rt_bind(({ wv t1054 = rt_get(&F_rec, v578_c); wv t1055 = v576_scope; rt_chk(s401_moduleNamed(up, t1054, t1055), "callee"); }), 0);
    if (rt_cond(rt_eq(v583_m, RT_NULL))) {
      (void)({ wv t1056 = rt_get(&F_rec, v578_c); wv t1057 = v576_scope; rt_chk(s405_unknownModule(up, t1056, t1057), "callee"); });
      RT_RET(RT_NULL);
    }
    v580_d = rt_chk(s404_exportOf(up, v578_c, v583_m), "callee");
    if (rt_cond(rt_eq(v580_d, RT_NULL))) {
      RT_RET(RT_NULL);
    }
  } else {
    v581_label = rt_bind(rt_get(&F_name, v578_c), 1);
    v580_d = ({ wv t1058 = v576_scope; wv t1059 = rt_get(&F_name, v578_c); rt_chk(s395_lookup(up, t1058, t1059), "callee"); });
    if (rt_cond(rt_eq(v580_d, RT_NULL))) {
      (void)({ wv t1060 = rt_get(&F_pos, v578_c); wv t1061 = rt_cat(3, (wv[]){ rt_s(&S437), rt_text(rt_get(&F_name, v578_c)), rt_s(&S113) }); rt_chk(s386_error(up, t1060, t1061, RT_NULL), "callee"); });
      RT_RET(RT_NULL);
    }
  }
  if (rt_cond(rt_ne(rt_get(&F_kind, v580_d), rt_s(&S33)))) {
    wv v584_hint = rt_bind((rt_cond(rt_eq(v579_n, rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S97), rt_text(v581_label), rt_s(&S438) }) : rt_s(&S439)), 0);
    (void)({ wv t1062 = rt_get(&F_pos, v578_c); wv t1063 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v581_label), rt_s(&S440) }); wv t1064 = v584_hint; rt_chk(s386_error(up, t1062, t1063, t1064), "callee"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(({ wv t1065 = rt_get(&F_arity, v580_d); wv t1066 = v579_n; rt_ne(t1065, t1066); }))) {
    (void)({ wv t1070 = rt_get(&F_pos, v578_c); wv t1071 = ({ wv t1067 = rt_text(v581_label); wv t1068 = rt_text(rt_chk(w5_plural(rt_get(&F_arity, v580_d)), "callee")); wv t1069 = rt_text(v579_n); rt_cat(6, (wv[]){ rt_s(&S113), t1067, rt_s(&S435), t1068, rt_s(&S436), t1069 }); }); rt_chk(s386_error(up, t1070, t1071, RT_NULL), "callee"); });
  }
  if (rt_cond(rt_b(rt_bool(v577_used, "&&") && rt_bool(rt_eq(({ wv t1072 = rt_get(&F_mod, v580_d); wv t1073 = rt_get(&F_namePos, v580_d); rt_chk(s389_firstUse(up, t1072, t1073), "callee"); }), RT_NULL), "&&")))) {
    (void)({ wv t1078 = rt_def(up->v384_uses, "uses"); wv t1079 = ({ wv t1074 = rt_get(&F_mod, v580_d); wv t1075 = rt_get(&F_namePos, v580_d); wv t1076 = rt_get(&F_pos, v578_c); wv t1077 = rt_def(up->v382_curMod, "curMod"); rt_rec(0, 4, K47, (wv[]){ t1074, t1075, t1076, t1077 }); }); std_push(t1078, t1079); });
  }
  RT_RET(RT_NULL);
}

static wv s415_importNames(struct fr99 *up, wv v585_m, wv v586_scope) {
  rt_enter("importNames");
  v585_m = rt_bind(v585_m, 0);
  v586_scope = rt_bind(v586_scope, 0);
  wv v587_imports = rt_bind(rt_get(&F_imports, rt_idx(v585_m, up->v378_modules)), 0);
  wv v588_seen = rt_arr(0, NULL, 1);
  wv v589_k = rt_i(0);
  while (rt_cond(({ wv t1080 = v589_k; wv t1081 = std_length(v587_imports); rt_lt(t1080, t1081); }))) {
    wv v590_node = rt_bind(rt_get(&F_node, rt_idx(v589_k, v587_imports)), 0);
    wv v591_lib = rt_bind(rt_get(&F_lib, rt_idx(v589_k, v587_imports)), 0);
    v589_k = rt_add(v589_k, rt_i(1));
    wv v592_path = rt_bind(rt_get(&F_path, rt_idx(v591_lib, up->v378_modules)), 0);
    wv v593_usable = rt_bind(v591_lib, 1);
    if (rt_cond(rt_chk(w1_contains(v588_seen, v591_lib), "importNames"))) {
      (void)({ wv t1082 = rt_get(&F_pathPos, v590_node); wv t1083 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v592_path), rt_s(&S441) }); rt_chk(s386_error(up, t1082, t1083, RT_NULL), "importNames"); });
      v593_usable = RT_NULL;
    } else if (rt_cond(rt_get(&F_notLibrary, rt_idx(v591_lib, up->v378_modules)))) {
      (void)({ wv t1084 = rt_get(&F_pathPos, v590_node); wv t1085 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text(v592_path), rt_s(&S442) }); rt_chk(s386_error(up, t1084, t1085, RT_NULL), "importNames"); });
      v593_usable = RT_NULL;
    }
    (void)std_push(v588_seen, v591_lib);
    wv v594_d = ({ wv t1086 = rt_get(&F_name, v590_node); wv t1087 = rt_get(&F_namePos, v590_node); wv t1088 = rt_def(up->v382_curMod, "curMod"); wv t1089 = v593_usable; rt_rec(0, 10, K41, (wv[]){ rt_s(&S388), t1086, rt_b(0), rt_b(0), t1087, RT_NULL, RT_NULL, RT_NULL, t1088, t1089 }); });
    (void)({ wv t1090 = v586_scope; wv t1091 = rt_get(&F_name, v590_node); wv t1092 = v594_d; rt_chk(s394_declare(up, t1090, t1091, t1092), "importNames"); });
  }
  RT_RET(RT_NULL);
}

static wv s416_libraryRules(struct fr99 *up, wv v595_m, wv v596_scope, wv v597_isEntry, wv v598_last) {
  rt_enter("libraryRules");
  v595_m = rt_bind(v595_m, 0);
  v596_scope = rt_bind(v596_scope, 0);
  v597_isEntry = rt_bind(v597_isEntry, 0);
  v598_last = rt_bind(v598_last, 0);
  wv v599_program = rt_bind(rt_get(&F_program, rt_idx(v595_m, up->v378_modules)), 0);
  wv v600_k = rt_i(0);
  while (rt_cond(({ wv t1093 = v600_k; wv t1094 = std_length(rt_get(&F_stmts, v599_program)); rt_lt(t1093, t1094); }))) {
    wv v602_s = rt_bind(({ wv t1095 = v600_k; wv t1096 = rt_get(&F_stmts, v599_program); rt_idx(t1095, t1096); }), 0);
    v600_k = rt_add(v600_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, v602_s), rt_s(&S203)), "&&") && rt_bool(rt_b(rt_bool(v597_isEntry, "||") || rt_bool(({ wv t1097 = rt_get(&F_pos, v602_s); wv t1098 = rt_get(&F_pos, v598_last); rt_ne(t1097, t1098); }), "||")), "&&")))) {
      (void)rt_chk(s386_error(up, rt_get(&F_pos, v602_s), rt_s(&S443), rt_s(&S444)), "libraryRules");
    } else if (rt_cond(rt_b(rt_bool(rt_not(v597_isEntry), "&&") && rt_bool(rt_not(({ wv t1099 = rt_def(w5_libraryTop, "libraryTop"); wv t1100 = rt_get(&F_k, v602_s); rt_chk(w1_contains(t1099, t1100), "libraryRules"); })), "&&")))) {
      (void)rt_chk(s386_error(up, rt_get(&F_pos, v602_s), rt_s(&S445), rt_s(&S446)), "libraryRules");
    }
  }
  if (rt_cond(v597_isEntry)) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v598_last, RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, v598_last), rt_s(&S203)), "||")))) {
    (void)rt_chk(s386_error(up, rt_get(&F_end, v599_program), rt_s(&S447), rt_s(&S448)), "libraryRules");
    RT_RET(RT_NULL);
  }
  wv v601_exports = rt_arr(0, NULL, 1);
  v600_k = rt_i(0);
  while (rt_cond(({ wv t1101 = v600_k; wv t1102 = std_length(rt_get(&F_names, v598_last)); rt_lt(t1101, t1102); }))) {
    wv v603_n = rt_bind(({ wv t1103 = v600_k; wv t1104 = rt_get(&F_names, v598_last); rt_idx(t1103, t1104); }), 0);
    v600_k = rt_add(v600_k, rt_i(1));
    wv v604_d = rt_bind(({ wv t1105 = v596_scope; wv t1106 = rt_get(&F_name, v603_n); rt_chk(s393_own(up, t1105, t1106), "libraryRules"); }), 0);
    if (rt_cond(rt_eq(v604_d, RT_NULL))) {
      (void)({ wv t1107 = rt_get(&F_pos, v603_n); wv t1108 = rt_cat(3, (wv[]){ rt_s(&S449), rt_text(rt_get(&F_name, v603_n)), rt_s(&S450) }); rt_chk(s386_error(up, t1107, t1108, RT_NULL), "libraryRules"); });
    } else if (rt_cond(rt_eq(rt_get(&F_kind, v604_d), rt_s(&S388)))) {
      (void)({ wv t1109 = rt_get(&F_pos, v603_n); wv t1110 = rt_cat(3, (wv[]){ rt_s(&S449), rt_text(rt_get(&F_name, v603_n)), rt_s(&S451) }); rt_chk(s386_error(up, t1109, t1110, RT_NULL), "libraryRules"); });
    } else {
      (void)std_push(v601_exports, v604_d);
    }
  }
  ({ wv t1111 = v601_exports; wv t1112 = rt_idx(v595_m, up->v378_modules); rt_set_mem(t1111, &F_exports, t1112); });
  RT_RET(RT_NULL);
}

static wv s417_checkModule(struct fr99 *up, wv v605_m, wv v606_isEntry) {
  rt_enter("checkModule");
  v605_m = rt_bind(v605_m, 0);
  v606_isEntry = rt_bind(v606_isEntry, 0);
  (void)rt_chk(s387_enter(up, v605_m), "checkModule");
  wv v607_program = rt_bind(rt_get(&F_program, rt_idx(v605_m, up->v378_modules)), 0);
  wv v608_last = RT_NULL;
  wv v609_k = rt_i(0);
  while (rt_cond(({ wv t1113 = v609_k; wv t1114 = std_length(rt_get(&F_stmts, v607_program)); rt_lt(t1113, t1114); }))) {
    if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1115 = v609_k; wv t1116 = rt_get(&F_stmts, v607_program); rt_idx(t1115, t1116); })), rt_s(&S200)))) {
      v608_last = rt_bind(({ wv t1117 = v609_k; wv t1118 = rt_get(&F_stmts, v607_program); rt_idx(t1117, t1118); }), 1);
    }
    v609_k = rt_add(v609_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(v606_isEntry), "&&") && rt_bool(rt_ne(v608_last, RT_NULL), "&&")), "&&") && rt_bool(rt_eq(rt_get(&F_k, v608_last), rt_s(&S54)), "&&")))) {
    rt_set_mem(rt_b(1), &F_notLibrary, rt_idx(v605_m, up->v378_modules));
    RT_RET(RT_NULL);
  }
  wv v610_scope = rt_bind(rt_chk(s391_newScope(up, RT_NULL), "checkModule"), 0);
  (void)rt_chk(s415_importNames(up, v605_m, v610_scope), "checkModule");
  (void)({ wv t1119 = rt_rec(0, 2, K48, (wv[]){ rt_get(&F_stmts, v607_program), RT_NULL }); wv t1120 = v610_scope; wv t1121 = rt_rec(0, 2, K45, (wv[]){ rt_def(up->v418_topFn, "topFn"), rt_i(0) }); rt_chk(s397_block(up, t1119, t1120, t1121), "checkModule"); });
  (void)rt_chk(s416_libraryRules(up, v605_m, v610_scope, v606_isEntry, v608_last), "checkModule");
  RT_RET(RT_NULL);
}

static wv w6_binFn(wv v617_op) {
  rt_enter("binFn");
  v617_op = rt_bind(v617_op, 0);
  wv v618_k = rt_i(0);
  while (rt_cond(({ wv t1148 = ({ wv t1146 = v618_k; wv t1147 = rt_def(w6_binOps, "binOps"); rt_idx(t1146, t1147); }); wv t1149 = v617_op; rt_ne(t1148, t1149); }))) {
    v618_k = rt_add(v618_k, rt_i(1));
  }
  RT_RET(({ wv t1150 = v618_k; wv t1151 = rt_def(w6_binFns, "binFns"); rt_idx(t1150, t1151); }));
}

static wv w6_modeOf(wv v619_x) {
  rt_enter("modeOf");
  v619_x = rt_bind(v619_x, 0);
  RT_RET((rt_cond(rt_get(&F_isVar, v619_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v619_x)) ? rt_s(&S37) : rt_s(&S35))));
}

static wv w6_unFn(wv v620_op) {
  rt_enter("unFn");
  v620_op = rt_bind(v620_op, 0);
  RT_RET((rt_cond(rt_eq(v620_op, rt_s(&S85))) ? rt_s(&S475) : (rt_cond(rt_eq(v620_op, rt_s(&S94))) ? rt_s(&S476) : rt_s(&S477))));
}

static wv w6_jsBool(wv v621_b) {
  rt_enter("jsBool");
  v621_b = rt_bind(v621_b, 0);
  RT_RET((rt_cond(v621_b) ? rt_s(&S49) : rt_s(&S50)));
}

static wv w6_repeat(wv v622_s, wv v623_count) {
  rt_enter("repeat");
  v622_s = rt_bind(v622_s, 0);
  v623_count = rt_bind(v623_count, 0);
  wv v624_out = rt_s(&S13);
  wv v625_k = rt_i(0);
  while (rt_cond(rt_lt(v625_k, v623_count))) {
    v624_out = rt_add(v624_out, v622_s);
    v625_k = rt_add(v625_k, rt_i(1));
  }
  RT_RET(v624_out);
}

static wv w6_jsFloat(wv v626_x) {
  rt_enter("jsFloat");
  v626_x = rt_bind(v626_x, 0);
  wv v627_cs = rt_bind(std_split(std_toString(v626_x), rt_s(&S13)), 0);
  wv v628_len = rt_bind(std_length(v627_cs), 0);
  wv v629_out = rt_s(&S13);
  wv v630_k = rt_i(0);
  while (rt_cond(rt_lt(v630_k, v628_len))) {
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_idx(v630_k, v627_cs), rt_s(&S129)), "&&") && rt_bool(rt_eq(({ wv t1152 = rt_add(v630_k, rt_i(1)); wv t1153 = v627_cs; rt_idx(t1152, t1153); }), rt_s(&S4)), "&&")), "&&") && rt_bool(rt_b(rt_bool(({ wv t1154 = rt_add(v630_k, rt_i(2)); wv t1155 = v628_len; rt_eq(t1154, t1155); }), "||") || rt_bool(rt_eq(({ wv t1156 = rt_add(v630_k, rt_i(2)); wv t1157 = v627_cs; rt_idx(t1156, t1157); }), rt_s(&S149)), "||")), "&&")))) {
      v630_k = rt_add(v630_k, rt_i(2));
      continue;
    }
    v629_out = ({ wv t1158 = v629_out; wv t1159 = rt_idx(v630_k, v627_cs); rt_add(t1158, t1159); });
    v630_k = rt_add(v630_k, rt_i(1));
  }
  RT_RET(v629_out);
}

static wv w6_generate(wv p_v631_modules, wv v632_runtime) {
  rt_enter("generate");
  struct fr139 fr = { 0 };
  fr.v631_modules = rt_bind(p_v631_modules, 0);
  v632_runtime = rt_bind(v632_runtime, 0);
  fr.v633_out = rt_arr(0, NULL, 1);
  fr.v634_indent = rt_i(1);
  fr.v635_n = rt_i(0);
  fr.v636_loops = rt_i(0);
  fr.v637_iife = rt_b(0);
  fr.v638_mod = rt_s(&S13);
  fr.v639_stolen = rt_arr(0, NULL, 1);
  fr.v640_enums = rt_arr(0, NULL, 1);
  fr.v641_refPositions = rt_arr(0, NULL, 1);
  wv v676_mi = rt_i(0);
  while (rt_cond(({ wv t1424 = v676_mi; wv t1425 = std_length(fr.v631_modules); rt_lt(t1424, t1425); }))) {
    wv v793_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v676_mi, fr.v631_modules))), 0);
    wv v794_li = rt_i(0);
    while (rt_cond(({ wv t1426 = v794_li; wv t1427 = std_length(v793_list); rt_lt(t1426, t1427); }))) {
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v794_li, v793_list)), rt_s(&S46)))) {
        (void)({ wv t1428 = rt_idx(v794_li, v793_list); wv t1429 = rt_chk(s642_modId(&fr, v676_mi), "generate"); rt_chk(s655_enumDef(&fr, t1428, t1429), "generate"); });
      }
      v794_li = rt_add(v794_li, rt_i(1));
    }
    v676_mi = rt_add(v676_mi, rt_i(1));
  }
  v676_mi = rt_i(0);
  while (rt_cond(({ wv t1430 = v676_mi; wv t1431 = std_length(fr.v631_modules); rt_lt(t1430, t1431); }))) {
    (void)rt_chk(s643_enter(&fr, v676_mi), "generate");
    (void)rt_chk(s650_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v676_mi, fr.v631_modules)))), "generate");
    v676_mi = rt_add(v676_mi, rt_i(1));
  }
  RT_RET(({ wv t1432 = rt_text(v632_runtime); wv t1433 = rt_text(std_join(fr.v633_out, rt_s(&S2))); rt_cat(5, (wv[]){ rt_s(&S563), t1432, rt_s(&S564), t1433, rt_s(&S565) }); }));
}

static wv s642_modId(struct fr139 *up, wv v677_m) {
  rt_enter("modId");
  v677_m = rt_bind(v677_m, 0);
  RT_RET((rt_cond(({ wv t1160 = v677_m; wv t1161 = rt_sub(std_length(up->v631_modules), rt_i(1)); rt_eq(t1160, t1161); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v677_m, rt_i(1))) })));
}

static wv s643_enter(struct fr139 *up, wv v678_m) {
  rt_enter("enter");
  v678_m = rt_bind(v678_m, 0);
  (void)({ wv t1162 = rt_get(&F_file, rt_idx(v678_m, up->v631_modules)); wv t1163 = rt_get(&F_chars, rt_idx(v678_m, up->v631_modules)); rt_chk(w2_setSource(t1162, t1163), "enter"); });
  wv t1164 = rt_bind(rt_chk(s642_modId(up, v678_m), "enter"), 1);
  (void)rt_def(up->v638_mod, "mod");
  up->v638_mod = t1164;
  wv t1165 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v639_stolen, "stolen");
  up->v639_stolen = t1165;
  wv t1166 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v640_enums, "enums");
  up->v640_enums = t1166;
  wv t1167 = rt_bind(rt_get(&F_refTargets, rt_idx(v678_m, up->v631_modules)), 1);
  (void)rt_def(up->v641_refPositions, "refPositions");
  up->v641_refPositions = t1167;
  wv v679_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v678_m, up->v631_modules))), 0);
  wv v680_k = rt_i(0);
  while (rt_cond(({ wv t1168 = v680_k; wv t1169 = std_length(v679_list); rt_lt(t1168, t1169); }))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v680_k, v679_list)), rt_s(&S46)))) {
      (void)({ wv t1170 = rt_def(up->v640_enums, "enums"); wv t1171 = rt_get(&F_name, rt_idx(v680_k, v679_list)); std_push(t1170, t1171); });
    }
    v680_k = rt_add(v680_k, rt_i(1));
  }
  wv v681_imports = rt_bind(rt_get(&F_imports, rt_idx(v678_m, up->v631_modules)), 0);
  v680_k = rt_i(0);
  while (rt_cond(({ wv t1172 = v680_k; wv t1173 = std_length(v681_imports); rt_lt(t1172, t1173); }))) {
    (void)({ wv t1176 = rt_def(up->v639_stolen, "stolen"); wv t1177 = ({ wv t1174 = rt_get(&F_name, rt_get(&F_node, rt_idx(v680_k, v681_imports))); wv t1175 = rt_get(&F_lib, rt_idx(v680_k, v681_imports)); rt_rec(0, 2, K49, (wv[]){ t1174, t1175 }); }); std_push(t1176, t1177); });
    v680_k = rt_add(v680_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s644_id(struct fr139 *up, wv v682_name) {
  rt_enter("id");
  v682_name = rt_bind(v682_name, 0);
  RT_RET(({ wv t1178 = rt_text(rt_def(up->v638_mod, "mod")); wv t1179 = rt_text(v682_name); rt_cat(4, (wv[]){ rt_s(&S478), t1178, rt_s(&S12), t1179 }); }));
}

static wv s645_stolenFrom(struct fr139 *up, wv v683_x) {
  rt_enter("stolenFrom");
  v683_x = rt_bind(v683_x, 0);
  if (rt_cond(rt_ne(rt_get(&F_k, v683_x), rt_s(&S211)))) {
    RT_RET(RT_NULL);
  }
  wv v684_k = rt_i(0);
  while (rt_cond(({ wv t1180 = v684_k; wv t1181 = std_length(rt_def(up->v639_stolen, "stolen")); rt_lt(t1180, t1181); }))) {
    if (rt_cond(({ wv t1184 = rt_get(&F_name, ({ wv t1182 = v684_k; wv t1183 = rt_def(up->v639_stolen, "stolen"); rt_idx(t1182, t1183); })); wv t1185 = rt_get(&F_name, v683_x); rt_eq(t1184, t1185); }))) {
      RT_RET(rt_get(&F_lib, ({ wv t1186 = v684_k; wv t1187 = rt_def(up->v639_stolen, "stolen"); rt_idx(t1186, t1187); })));
    }
    v684_k = rt_add(v684_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s646_exportKind(struct fr139 *up, wv v685_lib, wv v686_name) {
  rt_enter("exportKind");
  v685_lib = rt_bind(v685_lib, 0);
  v686_name = rt_bind(v686_name, 0);
  wv v687_xs = rt_bind(rt_get(&F_exports, rt_idx(v685_lib, up->v631_modules)), 0);
  wv v688_k = rt_i(0);
  while (rt_cond(({ wv t1188 = rt_get(&F_name, rt_idx(v688_k, v687_xs)); wv t1189 = v686_name; rt_ne(t1188, t1189); }))) {
    v688_k = rt_add(v688_k, rt_i(1));
  }
  RT_RET(rt_get(&F_kind, rt_idx(v688_k, v687_xs)));
}

static wv s647_enumJs(struct fr139 *up, wv v689_x) {
  rt_enter("enumJs");
  v689_x = rt_bind(v689_x, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v689_x), rt_s(&S211)))) {
    RT_RET((rt_cond(({ wv t1190 = rt_def(up->v640_enums, "enums"); wv t1191 = rt_get(&F_name, v689_x); rt_chk(w1_contains(t1190, t1191), "enumJs"); })) ? ({ wv t1192 = rt_text(rt_def(up->v638_mod, "mod")); wv t1193 = rt_text(rt_get(&F_name, v689_x)); rt_cat(4, (wv[]){ rt_s(&S479), t1192, rt_s(&S12), t1193 }); }) : RT_NULL));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v689_x), rt_s(&S189)))) {
    RT_RET(RT_NULL);
  }
  wv v690_lib = rt_bind(rt_chk(s645_stolenFrom(up, rt_get(&F_rec, v689_x)), "enumJs"), 0);
  RT_RET((rt_cond(rt_b(rt_bool(rt_ne(v690_lib, RT_NULL), "&&") && rt_bool(rt_eq(({ wv t1194 = v690_lib; wv t1195 = rt_get(&F_field, v689_x); rt_chk(s646_exportKind(up, t1194, t1195), "enumJs"); }), rt_s(&S46)), "&&"))) ? ({ wv t1196 = rt_text(rt_chk(s642_modId(up, v690_lib), "enumJs")); wv t1197 = rt_text(rt_get(&F_field, v689_x)); rt_cat(4, (wv[]){ rt_s(&S479), t1196, rt_s(&S12), t1197 }); }) : RT_NULL));
}

static wv s648_emit(struct fr139 *up, wv v691_line) {
  rt_enter("emit");
  v691_line = rt_bind(v691_line, 0);
  (void)({ wv t1200 = rt_def(up->v633_out, "out"); wv t1201 = ({ wv t1198 = rt_chk(w6_repeat(rt_s(&S480), rt_def(up->v634_indent, "indent")), "emit"); wv t1199 = v691_line; rt_add(t1198, t1199); }); std_push(t1200, t1201); });
  RT_RET(RT_NULL);
}

static wv s649_tmp(struct fr139 *up) {
  rt_enter("tmp");
  wv v692_name = rt_cat(2, (wv[]){ rt_s(&S481), rt_text(rt_def(up->v635_n, "n")) });
  up->v635_n = rt_add(rt_def(up->v635_n, "n"), rt_i(1));
  RT_RET(v692_name);
}

static wv s650_stmts(struct fr139 *up, wv v693_list) {
  rt_enter("stmts");
  v693_list = rt_bind(v693_list, 0);
  wv v694_k = rt_i(0);
  while (rt_cond(({ wv t1202 = v694_k; wv t1203 = std_length(v693_list); rt_lt(t1202, t1203); }))) {
    (void)rt_chk(s653_stmt(up, rt_idx(v694_k, v693_list)), "stmts");
    v694_k = rt_add(v694_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s651_blockBody(struct fr139 *up, wv v695_b) {
  rt_enter("blockBody");
  v695_b = rt_bind(v695_b, 0);
  (void)rt_chk(s650_stmts(up, rt_get(&F_stmts, v695_b)), "blockBody");
  if (rt_cond(rt_ne(rt_get(&F_value, v695_b), RT_NULL))) {
    (void)rt_chk(s648_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s675_expr(up, rt_get(&F_value, v695_b)), "blockBody")), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s652_valueOf(struct fr139 *up, wv v696_b) {
  rt_enter("valueOf");
  v696_b = rt_bind(v696_b, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_value, v696_b), RT_NULL)) ? rt_s(&S51) : rt_chk(s675_expr(up, rt_get(&F_value, v696_b)), "valueOf")));
}

static wv s653_stmt(struct fr139 *up, wv v697_s) {
  rt_enter("stmt");
  v697_s = rt_bind(v697_s, 0);
  if (rt_cond(rt_ne(rt_get(&F_handler, v697_s), RT_NULL))) {
    (void)rt_chk(s659_onfailStmt(up, v697_s), "stmt");
    RT_RET(RT_NULL);
  }
  wv v698_kind = rt_bind(rt_get(&F_k, v697_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v698_kind, rt_s(&S200)), "||") || rt_bool(rt_eq(v698_kind, rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(v698_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v698_kind, rt_s(&S203)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S33)))) {
    (void)rt_chk(s654_fnDef(up, v697_s), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S34)))) {
    if (rt_cond(rt_def(up->v637_iife, "iife"))) {
      (void)rt_chk(w2_abort(rt_get(&F_pos, v697_s), rt_s(&S482)), "stmt");
    }
    (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S483), rt_text((rt_cond(rt_eq(rt_get(&F_e, v697_s), RT_NULL)) ? rt_s(&S51) : rt_chk(s675_expr(up, rt_get(&F_e, v697_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S57)))) {
    (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S484), rt_text(rt_chk(s675_expr(up, rt_get(&F_e, v697_s)), "stmt")), rt_s(&S485) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v698_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v698_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_def(up->v636_loops, "loops"), rt_i(0)))) {
      (void)({ wv t1204 = rt_get(&F_pos, v697_s); wv t1205 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text((rt_cond(rt_eq(v698_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43))), rt_s(&S486) }); rt_chk(w2_abort(t1204, t1205), "stmt"); });
    }
    (void)rt_chk(s648_emit(up, (rt_cond(rt_eq(v698_kind, rt_s(&S42))) ? rt_s(&S487) : rt_s(&S488))), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S41)))) {
    (void)rt_chk(s648_emit(up, ({ wv t1206 = rt_text((rt_cond(rt_get(&F_neg, v697_s)) ? rt_s(&S94) : rt_s(&S13))); wv t1207 = rt_text(rt_chk(s675_expr(up, rt_get(&F_cond, v697_s)), "stmt")); rt_cat(5, (wv[]){ rt_s(&S489), t1206, rt_s(&S490), t1207, rt_s(&S491) }); })), "stmt");
    up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
    up->v636_loops = rt_add(rt_def(up->v636_loops, "loops"), rt_i(1));
    (void)rt_chk(s651_blockBody(up, rt_get(&F_body, v697_s)), "stmt");
    up->v636_loops = rt_sub(rt_def(up->v636_loops, "loops"), rt_i(1));
    up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
    (void)rt_chk(s648_emit(up, rt_s(&S15)), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S55)))) {
    (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S492), rt_text(rt_get(&F_code, v697_s)), rt_s(&S485) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S54)))) {
    (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S493), rt_text((rt_cond(rt_eq(rt_get(&F_e, v697_s), RT_NULL)) ? rt_s(&S4) : rt_chk(s675_expr(up, rt_get(&F_e, v697_s)), "stmt"))), rt_s(&S485) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S176)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v697_s)), rt_s(&S184)))) {
      (void)rt_chk(s660_chainStmt(up, rt_get(&F_e, v697_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v697_s)), rt_s(&S44)))) {
      (void)rt_chk(s671_matchStmt(up, rt_get(&F_e, v697_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v697_s)), rt_s(&S183)))) {
      (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S492), rt_text(rt_chk(s675_expr(up, rt_get(&F_arg, rt_get(&F_e, v697_s))), "stmt")), rt_s(&S485) })), "stmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s648_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s675_expr(up, rt_get(&F_e, v697_s)), "stmt")), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S177)))) {
    (void)rt_chk(s648_emit(up, ({ wv t1210 = rt_text((rt_cond(rt_b(rt_bool(rt_get(&F_isVar, v697_s), "||") || rt_bool(rt_get(&F_isRef, v697_s), "||"))) ? rt_s(&S494) : rt_s(&S35))); wv t1211 = rt_text(rt_chk(s644_id(up, rt_get(&F_name, v697_s)), "stmt")); wv t1212 = rt_text(({ wv t1208 = rt_get(&F_e, v697_s); wv t1209 = rt_chk(w6_modeOf(v697_s), "stmt"); rt_chk(s656_bindRhs(up, t1208, t1209), "stmt"); })); rt_cat(6, (wv[]){ t1210, rt_s(&S0), t1211, rt_s(&S495), t1212, rt_s(&S14) }); })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v698_kind, rt_s(&S38)))) {
    (void)rt_chk(s657_setStmt(up, v697_s), "stmt");
    RT_RET(RT_NULL);
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S496), rt_text(v698_kind) }));
  RT_RET(RT_NULL);
}

static wv s654_fnDef(struct fr139 *up, wv v699_s) {
  rt_enter("fnDef");
  v699_s = rt_bind(v699_s, 0);
  wv v700_names = rt_arr(0, NULL, 1);
  wv v701_k = rt_i(0);
  while (rt_cond(({ wv t1213 = v701_k; wv t1214 = std_length(rt_get(&F_params, v699_s)); rt_lt(t1213, t1214); }))) {
    (void)({ wv t1217 = v700_names; wv t1218 = rt_chk(s644_id(up, rt_get(&F_name, ({ wv t1215 = v701_k; wv t1216 = rt_get(&F_params, v699_s); rt_idx(t1215, t1216); }))), "fnDef"); std_push(t1217, t1218); });
    v701_k = rt_add(v701_k, rt_i(1));
  }
  (void)rt_chk(s648_emit(up, ({ wv t1219 = rt_text(rt_chk(s644_id(up, rt_get(&F_name, v699_s)), "fnDef")); wv t1220 = rt_text(std_join(v700_names, rt_s(&S385))); rt_cat(5, (wv[]){ rt_s(&S497), t1219, rt_s(&S97), t1220, rt_s(&S498) }); })), "fnDef");
  up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
  v701_k = rt_i(0);
  while (rt_cond(({ wv t1221 = v701_k; wv t1222 = std_length(rt_get(&F_params, v699_s)); rt_lt(t1221, t1222); }))) {
    wv v704_param = rt_bind(({ wv t1223 = v701_k; wv t1224 = rt_get(&F_params, v699_s); rt_idx(t1223, t1224); }), 0);
    if (rt_cond(rt_not(rt_get(&F_isRef, v704_param)))) {
      (void)rt_chk(s648_emit(up, ({ wv t1225 = rt_text(rt_chk(s644_id(up, rt_get(&F_name, v704_param)), "fnDef")); wv t1226 = rt_text(rt_chk(s644_id(up, rt_get(&F_name, v704_param)), "fnDef")); wv t1227 = rt_text(rt_chk(w6_jsBool(rt_get(&F_isVar, v704_param)), "fnDef")); rt_cat(6, (wv[]){ t1225, rt_s(&S499), t1226, rt_s(&S385), t1227, rt_s(&S485) }); })), "fnDef");
    }
    v701_k = rt_add(v701_k, rt_i(1));
  }
  (void)rt_chk(s648_emit(up, rt_s(&S500)), "fnDef");
  up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
  wv v702_savedLoops = rt_bind(rt_def(up->v636_loops, "loops"), 0);
  wv v703_savedIife = rt_bind(rt_def(up->v637_iife, "iife"), 0);
  wv t1228 = rt_i(0);
  (void)rt_def(up->v636_loops, "loops");
  up->v636_loops = t1228;
  wv t1229 = rt_b(0);
  (void)rt_def(up->v637_iife, "iife");
  up->v637_iife = t1229;
  (void)rt_chk(s650_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v699_s))), "fnDef");
  (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S483), rt_text(rt_chk(s652_valueOf(up, rt_get(&F_body, v699_s)), "fnDef")), rt_s(&S14) })), "fnDef");
  wv t1230 = rt_bind(v702_savedLoops, 1);
  (void)rt_def(up->v636_loops, "loops");
  up->v636_loops = t1230;
  wv t1231 = rt_bind(v703_savedIife, 1);
  (void)rt_def(up->v637_iife, "iife");
  up->v637_iife = t1231;
  up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s648_emit(up, rt_s(&S501)), "fnDef");
  (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S502), rt_text(rt_chk(w1_quote(rt_get(&F_name, v699_s)), "fnDef")), rt_s(&S485) })), "fnDef");
  (void)rt_chk(s648_emit(up, rt_s(&S15)), "fnDef");
  up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s648_emit(up, rt_s(&S15)), "fnDef");
  RT_RET(RT_NULL);
}

static wv s655_enumDef(struct fr139 *up, wv v705_s, wv v706_m) {
  rt_enter("enumDef");
  v705_s = rt_bind(v705_s, 0);
  v706_m = rt_bind(v706_m, 0);
  wv v707_names = rt_arr(0, NULL, 1);
  wv v708_k = rt_i(0);
  while (rt_cond(({ wv t1232 = v708_k; wv t1233 = std_length(rt_get(&F_members, v705_s)); rt_lt(t1232, t1233); }))) {
    (void)({ wv t1236 = v707_names; wv t1237 = rt_chk(w1_quote(rt_get(&F_name, ({ wv t1234 = v708_k; wv t1235 = rt_get(&F_members, v705_s); rt_idx(t1234, t1235); }))), "enumDef"); std_push(t1236, t1237); });
    v708_k = rt_add(v708_k, rt_i(1));
  }
  (void)rt_chk(s648_emit(up, ({ wv t1238 = rt_text(v706_m); wv t1239 = rt_text(rt_get(&F_name, v705_s)); wv t1240 = rt_text(rt_chk(w1_quote(rt_get(&F_name, v705_s)), "enumDef")); wv t1241 = rt_text(std_join(v707_names, rt_s(&S385))); rt_cat(9, (wv[]){ rt_s(&S503), t1238, rt_s(&S12), t1239, rt_s(&S504), t1240, rt_s(&S505), t1241, rt_s(&S506) }); })), "enumDef");
  RT_RET(RT_NULL);
}

static wv s656_bindRhs(struct fr139 *up, wv v709_e, wv v710_mode) {
  rt_enter("bindRhs");
  v709_e = rt_bind(v709_e, 0);
  v710_mode = rt_bind(v710_mode, 0);
  wv v711_mutable = rt_eq(v710_mode, rt_s(&S36));
  if (rt_cond(rt_eq(rt_get(&F_k, v709_e), rt_s(&S186)))) {
    wv v712_items = rt_bind(rt_chk(s674_exprList(up, rt_get(&F_items, v709_e)), "bindRhs"), 0);
    RT_RET((rt_cond(v711_mutable) ? rt_cat(3, (wv[]){ rt_s(&S98), rt_text(v712_items), rt_s(&S99) }) : rt_cat(3, (wv[]){ rt_s(&S507), rt_text(v712_items), rt_s(&S508) })));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v709_e), rt_s(&S187)))) {
    RT_RET(rt_chk(s658_record(up, v709_e, v711_mutable), "bindRhs"));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1242 = rt_def(w6_scalarKinds, "scalarKinds"); wv t1243 = rt_get(&F_k, v709_e); rt_chk(w1_contains(t1242, t1243), "bindRhs"); }), "||") || rt_bool(rt_eq(v710_mode, rt_s(&S37)), "||")))) {
    RT_RET(rt_chk(s675_expr(up, v709_e), "bindRhs"));
  }
  RT_RET(({ wv t1244 = rt_text(rt_chk(s675_expr(up, v709_e), "bindRhs")); wv t1245 = rt_text(rt_chk(w6_jsBool(v711_mutable), "bindRhs")); rt_cat(5, (wv[]){ rt_s(&S509), t1244, rt_s(&S385), t1245, rt_s(&S16) }); }));
}

static wv s657_setStmt(struct fr139 *up, wv v713_s) {
  rt_enter("setStmt");
  v713_s = rt_bind(v713_s, 0);
  wv v714_t = rt_bind(rt_get(&F_target, v713_s), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v714_t), rt_s(&S211)))) {
    wv v716_mode = rt_bind((rt_cond(({ wv t1246 = rt_def(up->v641_refPositions, "refPositions"); wv t1247 = rt_get(&F_pos, v714_t); rt_chk(w1_contains(t1246, t1247), "setStmt"); })) ? rt_s(&S37) : rt_s(&S36)), 0);
    if (rt_cond(rt_eq(rt_get(&F_op, v713_s), RT_NULL))) {
      (void)rt_chk(s648_emit(up, ({ wv t1250 = rt_text(rt_chk(s644_id(up, rt_get(&F_name, v714_t)), "setStmt")); wv t1251 = rt_text(({ wv t1248 = rt_get(&F_e, v713_s); wv t1249 = v716_mode; rt_chk(s656_bindRhs(up, t1248, t1249), "setStmt"); })); rt_cat(4, (wv[]){ t1250, rt_s(&S495), t1251, rt_s(&S14) }); })), "setStmt");
      RT_RET(RT_NULL);
    }
    wv v717_v = rt_bind(rt_chk(s675_expr(up, rt_get(&F_e, v713_s)), "setStmt"), 1);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v713_s)), rt_s(&S138)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v713_s)), rt_s(&S211)), "&&")))) {
      wv v718_temp = rt_bind(rt_chk(s649_tmp(up), "setStmt"), 0);
      (void)rt_chk(s648_emit(up, ({ wv t1252 = rt_text(v718_temp); wv t1253 = rt_text(v717_v); rt_cat(5, (wv[]){ rt_s(&S510), t1252, rt_s(&S495), t1253, rt_s(&S14) }); })), "setStmt");
      v717_v = rt_bind(v718_temp, 1);
    }
    (void)rt_chk(s648_emit(up, ({ wv t1254 = rt_text(rt_chk(s644_id(up, rt_get(&F_name, v714_t)), "setStmt")); wv t1255 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v713_s)), "setStmt")); wv t1256 = rt_text(rt_chk(s644_id(up, rt_get(&F_name, v714_t)), "setStmt")); wv t1257 = rt_text(v717_v); rt_cat(8, (wv[]){ t1254, rt_s(&S511), t1255, rt_s(&S97), t1256, rt_s(&S385), t1257, rt_s(&S485) }); })), "setStmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v714_t), rt_s(&S189)))) {
    wv v719_args = ({ wv t1258 = rt_text(rt_chk(s675_expr(up, rt_get(&F_e, v713_s)), "setStmt")); wv t1259 = rt_text(rt_get(&F_field, v714_t)); wv t1260 = rt_text(rt_chk(s675_expr(up, rt_get(&F_rec, v714_t)), "setStmt")); rt_cat(5, (wv[]){ t1258, rt_s(&S512), t1259, rt_s(&S513), t1260 }); });
    if (rt_cond(rt_eq(rt_get(&F_op, v713_s), RT_NULL))) {
      (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S514), rt_text(v719_args), rt_s(&S485) })), "setStmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s648_emit(up, ({ wv t1261 = rt_text(v719_args); wv t1262 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v713_s)), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S515), t1261, rt_s(&S516), t1262, rt_s(&S485) }); })), "setStmt");
    RT_RET(RT_NULL);
  }
  wv v715_args = ({ wv t1263 = rt_text(rt_chk(s675_expr(up, rt_get(&F_e, v713_s)), "setStmt")); wv t1264 = rt_text(rt_chk(s675_expr(up, rt_get(&F_index, v714_t)), "setStmt")); wv t1265 = rt_text(rt_chk(s675_expr(up, rt_get(&F_arr, v714_t)), "setStmt")); rt_cat(5, (wv[]){ t1263, rt_s(&S385), t1264, rt_s(&S385), t1265 }); });
  if (rt_cond(rt_eq(rt_get(&F_op, v713_s), RT_NULL))) {
    (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S517), rt_text(v715_args), rt_s(&S485) })), "setStmt");
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s648_emit(up, ({ wv t1266 = rt_text(v715_args); wv t1267 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v713_s)), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S518), t1266, rt_s(&S516), t1267, rt_s(&S485) }); })), "setStmt");
  RT_RET(RT_NULL);
}

static wv s658_record(struct fr139 *up, wv v720_e, wv v721_mutable) {
  rt_enter("record");
  v720_e = rt_bind(v720_e, 0);
  v721_mutable = rt_bind(v721_mutable, 0);
  wv v722_fields = rt_s(&S13);
  wv v723_k = rt_i(0);
  while (rt_cond(({ wv t1268 = v723_k; wv t1269 = std_length(rt_get(&F_fields, v720_e)); rt_lt(t1268, t1269); }))) {
    wv v724_field = rt_bind(({ wv t1270 = v723_k; wv t1271 = rt_get(&F_fields, v720_e); rt_idx(t1270, t1271); }), 0);
    v722_fields = ({ wv t1274 = v722_fields; wv t1275 = ({ wv t1272 = rt_text(rt_get(&F_name, v724_field)); wv t1273 = rt_text(rt_chk(s675_expr(up, rt_get(&F_e, v724_field)), "record")); rt_cat(4, (wv[]){ rt_s(&S519), t1272, rt_s(&S29), t1273 }); }); rt_add(t1274, t1275); });
    v723_k = rt_add(v723_k, rt_i(1));
  }
  RT_RET(({ wv t1276 = rt_text(v722_fields); wv t1277 = rt_text(rt_chk(w6_jsBool(v721_mutable), "record")); rt_cat(5, (wv[]){ rt_s(&S520), t1276, rt_s(&S521), t1277, rt_s(&S16) }); }));
}

static wv s659_onfailStmt(struct fr139 *up, wv v725_s) {
  rt_enter("onfailStmt");
  v725_s = rt_bind(v725_s, 0);
  wv v726_call = rt_bind(rt_get(&F_e, v725_s), 0);
  wv v727_args = rt_arr(0, NULL, 1);
  wv v728_k = rt_i(0);
  while (rt_cond(({ wv t1278 = v728_k; wv t1279 = std_length(rt_get(&F_args, v726_call)); rt_lt(t1278, t1279); }))) {
    wv v732_a = rt_bind(({ wv t1280 = v728_k; wv t1281 = rt_get(&F_args, v726_call); rt_idx(t1280, t1281); }), 0);
    if (rt_cond(({ wv t1282 = rt_def(w6_plainKinds, "plainKinds"); wv t1283 = rt_get(&F_k, v732_a); rt_chk(w1_contains(t1282, t1283), "onfailStmt"); }))) {
      (void)({ wv t1284 = v727_args; wv t1285 = rt_chk(s675_expr(up, v732_a), "onfailStmt"); std_push(t1284, t1285); });
    } else {
      wv v733_temp = rt_bind(rt_chk(s649_tmp(up), "onfailStmt"), 0);
      (void)rt_chk(s648_emit(up, ({ wv t1286 = rt_text(v733_temp); wv t1287 = rt_text(rt_chk(s675_expr(up, v732_a), "onfailStmt")); rt_cat(5, (wv[]){ rt_s(&S510), t1286, rt_s(&S495), t1287, rt_s(&S14) }); })), "onfailStmt");
      (void)std_push(v727_args, v733_temp);
    }
    v728_k = rt_add(v728_k, rt_i(1));
  }
  wv v729_r = rt_bind(rt_chk(s649_tmp(up), "onfailStmt"), 0);
  wv v730_ev = rt_cat(2, (wv[]){ rt_s(&S522), rt_text(rt_def(up->v635_n, "n")) });
  up->v635_n = rt_add(rt_def(up->v635_n, "n"), rt_i(1));
  (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S523), rt_text(v729_r), rt_s(&S14) })), "onfailStmt");
  (void)rt_chk(s648_emit(up, rt_s(&S500)), "onfailStmt");
  (void)rt_chk(s648_emit(up, ({ wv t1290 = rt_text(v729_r); wv t1291 = rt_text(({ wv t1288 = rt_get(&F_callee, v726_call); wv t1289 = v727_args; rt_chk(s673_callJs(up, t1288, t1289), "onfailStmt"); })); rt_cat(5, (wv[]){ rt_s(&S480), t1290, rt_s(&S495), t1291, rt_s(&S14) }); })), "onfailStmt");
  (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S524), rt_text(v730_ev), rt_s(&S498) })), "onfailStmt");
  up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s648_emit(up, ({ wv t1292 = rt_text(v730_ev); wv t1293 = rt_text(v730_ev); rt_cat(5, (wv[]){ rt_s(&S525), t1292, rt_s(&S526), t1293, rt_s(&S14) }); })), "onfailStmt");
  if (rt_cond(rt_ne(rt_get(&F_param, rt_get(&F_handler, v725_s)), RT_NULL))) {
    (void)rt_chk(s648_emit(up, ({ wv t1294 = rt_text(rt_chk(s644_id(up, rt_get(&F_param, rt_get(&F_handler, v725_s))), "onfailStmt")); wv t1295 = rt_text(v730_ev); rt_cat(5, (wv[]){ rt_s(&S510), t1294, rt_s(&S495), t1295, rt_s(&S527) }); })), "onfailStmt");
  }
  (void)rt_chk(s648_emit(up, rt_s(&S100)), "onfailStmt");
  up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s650_stmts(up, rt_get(&F_stmts, rt_get(&F_body, rt_get(&F_handler, v725_s)))), "onfailStmt");
  (void)rt_chk(s648_emit(up, ({ wv t1296 = rt_text(v729_r); wv t1297 = rt_text(rt_chk(s652_valueOf(up, rt_get(&F_body, rt_get(&F_handler, v725_s))), "onfailStmt")); rt_cat(4, (wv[]){ t1296, rt_s(&S495), t1297, rt_s(&S14) }); })), "onfailStmt");
  up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s648_emit(up, rt_s(&S15)), "onfailStmt");
  up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s648_emit(up, rt_s(&S15)), "onfailStmt");
  wv v731_rest = rt_bind(v725_s, 1);
  rt_set_mem(RT_NULL, &F_handler, v731_rest);
  ({ wv t1300 = ({ wv t1298 = v729_r; wv t1299 = rt_get(&F_pos, v725_s); rt_rec(0, 4, K50, (wv[]){ rt_s(&S528), t1298, t1299, rt_b(0) }); }); wv t1301 = v731_rest; rt_set_mem(t1300, &F_e, t1301); });
  (void)rt_chk(s653_stmt(up, v731_rest), "onfailStmt");
  RT_RET(RT_NULL);
}

static wv s660_chainStmt(struct fr139 *up, wv v734_e) {
  rt_enter("chainStmt");
  v734_e = rt_bind(v734_e, 0);
  wv v735_k = rt_i(0);
  while (rt_cond(({ wv t1302 = v735_k; wv t1303 = std_length(rt_get(&F_branches, v734_e)); rt_lt(t1302, t1303); }))) {
    wv v736_b = rt_bind(({ wv t1304 = v735_k; wv t1305 = rt_get(&F_branches, v734_e); rt_idx(t1304, t1305); }), 0);
    (void)rt_chk(s648_emit(up, ({ wv t1306 = rt_text((rt_cond(rt_gt(v735_k, rt_i(0))) ? rt_s(&S529) : rt_s(&S13))); wv t1307 = rt_text(rt_chk(s661_test(up, v736_b), "chainStmt")); rt_cat(4, (wv[]){ t1306, rt_s(&S530), t1307, rt_s(&S498) }); })), "chainStmt");
    up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
    (void)rt_chk(s651_blockBody(up, rt_get(&F_body, v736_b)), "chainStmt");
    up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
    v735_k = rt_add(v735_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v734_e), RT_NULL))) {
    (void)rt_chk(s648_emit(up, rt_s(&S531)), "chainStmt");
    up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
    (void)rt_chk(s651_blockBody(up, rt_get(&F_els, v734_e)), "chainStmt");
    up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s648_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s661_test(struct fr139 *up, wv v737_b) {
  rt_enter("test");
  v737_b = rt_bind(v737_b, 0);
  RT_RET(({ wv t1308 = rt_text((rt_cond(rt_get(&F_neg, v737_b)) ? rt_s(&S94) : rt_s(&S13))); wv t1309 = rt_text(rt_chk(s675_expr(up, rt_get(&F_cond, v737_b)), "test")); rt_cat(4, (wv[]){ t1308, rt_s(&S490), t1309, rt_s(&S16) }); }));
}

static wv s662_branchCount(struct fr139 *up, wv v738_e) {
  rt_enter("branchCount");
  v738_e = rt_bind(v738_e, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v738_e), rt_s(&S184))) ? std_length(rt_get(&F_branches, v738_e)) : std_length(rt_get(&F_cases, v738_e))));
}

static wv s663_branchBody(struct fr139 *up, wv v739_e, wv v740_i) {
  rt_enter("branchBody");
  v739_e = rt_bind(v739_e, 0);
  v740_i = rt_bind(v740_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v739_e), rt_s(&S184))) ? rt_get(&F_body, ({ wv t1310 = v740_i; wv t1311 = rt_get(&F_branches, v739_e); rt_idx(t1310, t1311); })) : rt_get(&F_body, ({ wv t1312 = v740_i; wv t1313 = rt_get(&F_cases, v739_e); rt_idx(t1312, t1313); }))));
}

static wv s664_branchTest(struct fr139 *up, wv v741_e, wv v742_t, wv v743_i) {
  rt_enter("branchTest");
  v741_e = rt_bind(v741_e, 0);
  v742_t = rt_bind(v742_t, 0);
  v743_i = rt_bind(v743_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v741_e), rt_s(&S184))) ? rt_chk(s661_test(up, ({ wv t1314 = v743_i; wv t1315 = rt_get(&F_branches, v741_e); rt_idx(t1314, t1315); })), "branchTest") : ({ wv t1318 = v742_t; wv t1319 = ({ wv t1316 = v743_i; wv t1317 = rt_get(&F_cases, v741_e); rt_idx(t1316, t1317); }); rt_chk(s669_caseTest(up, t1318, t1319), "branchTest"); })));
}

static wv s665_isPlain(struct fr139 *up, wv v744_e) {
  rt_enter("isPlain");
  v744_e = rt_bind(v744_e, 0);
  wv v745_k = rt_i(0);
  while (rt_cond(({ wv t1320 = v745_k; wv t1321 = rt_chk(s662_branchCount(up, v744_e), "isPlain"); rt_le(t1320, t1321); }))) {
    wv v746_b = rt_bind((rt_cond(({ wv t1322 = v745_k; wv t1323 = rt_chk(s662_branchCount(up, v744_e), "isPlain"); rt_lt(t1322, t1323); })) ? rt_chk(s663_branchBody(up, v744_e, v745_k), "isPlain") : rt_get(&F_els, v744_e)), 0);
    wv v747_j = rt_i(0);
    while (rt_cond(({ wv t1324 = v747_j; wv t1325 = std_length(rt_get(&F_stmts, v746_b)); rt_lt(t1324, t1325); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t1326 = v747_j; wv t1327 = rt_get(&F_stmts, v746_b); rt_idx(t1326, t1327); })), rt_s(&S200)))) {
        RT_RET(rt_b(0));
      }
      v747_j = rt_add(v747_j, rt_i(1));
    }
    v745_k = rt_add(v745_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s666_ternaryChain(struct fr139 *up, wv v748_e, wv v749_t) {
  rt_enter("ternaryChain");
  v748_e = rt_bind(v748_e, 0);
  v749_t = rt_bind(v749_t, 0);
  wv v750_js = rt_bind(rt_chk(s652_valueOf(up, rt_get(&F_els, v748_e)), "ternaryChain"), 1);
  wv v751_k = rt_sub(rt_chk(s662_branchCount(up, v748_e), "ternaryChain"), rt_i(1));
  while (rt_cond(rt_ge(v751_k, rt_i(0)))) {
    v750_js = ({ wv t1328 = rt_text(rt_chk(s664_branchTest(up, v748_e, v749_t, v751_k), "ternaryChain")); wv t1329 = rt_text(rt_chk(s652_valueOf(up, rt_chk(s663_branchBody(up, v748_e, v751_k), "ternaryChain")), "ternaryChain")); wv t1330 = rt_text(v750_js); rt_cat(7, (wv[]){ rt_s(&S97), t1328, rt_s(&S532), t1329, rt_s(&S533), t1330, rt_s(&S16) }); });
    v751_k = rt_sub(v751_k, rt_i(1));
  }
  RT_RET(v750_js);
}

static wv s667_branchFunction(struct fr139 *up, wv v752_e, wv v753_t) {
  rt_enter("branchFunction");
  v752_e = rt_bind(v752_e, 0);
  v753_t = rt_bind(v753_t, 0);
  wv v754_start = rt_bind(std_length(rt_def(up->v633_out, "out")), 0);
  wv v755_savedIndent = rt_bind(rt_def(up->v634_indent, "indent"), 0);
  wv v756_savedLoops = rt_bind(rt_def(up->v636_loops, "loops"), 0);
  wv v757_savedIife = rt_bind(rt_def(up->v637_iife, "iife"), 0);
  wv t1331 = rt_add(v755_savedIndent, rt_i(1));
  (void)rt_def(up->v634_indent, "indent");
  up->v634_indent = t1331;
  wv t1332 = rt_i(0);
  (void)rt_def(up->v636_loops, "loops");
  up->v636_loops = t1332;
  wv t1333 = rt_b(1);
  (void)rt_def(up->v637_iife, "iife");
  up->v637_iife = t1333;
  wv v758_k = rt_i(0);
  while (rt_cond(({ wv t1334 = v758_k; wv t1335 = rt_chk(s662_branchCount(up, v752_e), "branchFunction"); rt_lt(t1334, t1335); }))) {
    wv v760_b = rt_bind(rt_chk(s663_branchBody(up, v752_e, v758_k), "branchFunction"), 0);
    (void)rt_chk(s648_emit(up, ({ wv t1336 = rt_text((rt_cond(rt_gt(v758_k, rt_i(0))) ? rt_s(&S529) : rt_s(&S13))); wv t1337 = rt_text(rt_chk(s664_branchTest(up, v752_e, v753_t, v758_k), "branchFunction")); rt_cat(4, (wv[]){ t1336, rt_s(&S530), t1337, rt_s(&S498) }); })), "branchFunction");
    up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
    (void)rt_chk(s650_stmts(up, rt_get(&F_stmts, v760_b)), "branchFunction");
    (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S483), rt_text(rt_chk(s652_valueOf(up, v760_b), "branchFunction")), rt_s(&S14) })), "branchFunction");
    up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
    v758_k = rt_add(v758_k, rt_i(1));
  }
  (void)rt_chk(s648_emit(up, rt_s(&S531)), "branchFunction");
  up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s650_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v752_e))), "branchFunction");
  (void)rt_chk(s648_emit(up, rt_cat(3, (wv[]){ rt_s(&S483), rt_text(rt_chk(s652_valueOf(up, rt_get(&F_els, v752_e)), "branchFunction")), rt_s(&S14) })), "branchFunction");
  up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
  (void)rt_chk(s648_emit(up, rt_s(&S15)), "branchFunction");
  wv v759_body = rt_bind(std_join(({ wv t1338 = rt_def(up->v633_out, "out"); wv t1339 = v754_start; wv t1340 = std_length(rt_def(up->v633_out, "out")); std_slice(t1338, t1339, t1340); }), rt_s(&S2)), 0);
  while (rt_cond(({ wv t1341 = std_length(rt_def(up->v633_out, "out")); wv t1342 = v754_start; rt_gt(t1341, t1342); }))) {
    (void)std_pop(rt_def(up->v633_out, "out"));
  }
  wv t1343 = rt_bind(v755_savedIndent, 1);
  (void)rt_def(up->v634_indent, "indent");
  up->v634_indent = t1343;
  wv t1344 = rt_bind(v756_savedLoops, 1);
  (void)rt_def(up->v636_loops, "loops");
  up->v636_loops = t1344;
  wv t1345 = rt_bind(v757_savedIife, 1);
  (void)rt_def(up->v637_iife, "iife");
  up->v637_iife = t1345;
  RT_RET(v759_body);
}

static wv s668_chainExpr(struct fr139 *up, wv v761_e) {
  rt_enter("chainExpr");
  v761_e = rt_bind(v761_e, 0);
  if (rt_cond(rt_chk(s665_isPlain(up, v761_e), "chainExpr"))) {
    RT_RET(rt_chk(s666_ternaryChain(up, v761_e, RT_NULL), "chainExpr"));
  }
  wv v762_body = rt_bind(rt_chk(s667_branchFunction(up, v761_e, RT_NULL), "chainExpr"), 0);
  RT_RET(({ wv t1346 = rt_text(v762_body); wv t1347 = rt_text(rt_chk(w6_repeat(rt_s(&S480), rt_def(up->v634_indent, "indent")), "chainExpr")); rt_cat(5, (wv[]){ rt_s(&S534), t1346, rt_s(&S2), t1347, rt_s(&S535) }); }));
}

static wv s669_caseTest(struct fr139 *up, wv v763_t, wv v764_a) {
  rt_enter("caseTest");
  v763_t = rt_bind(v763_t, 0);
  v764_a = rt_bind(v764_a, 0);
  wv v765_tests = rt_arr(0, NULL, 1);
  wv v766_k = rt_i(0);
  while (rt_cond(({ wv t1348 = v766_k; wv t1349 = std_length(rt_get(&F_values, v764_a)); rt_lt(t1348, t1349); }))) {
    (void)({ wv t1354 = v765_tests; wv t1355 = ({ wv t1352 = rt_text(v763_t); wv t1353 = rt_text(rt_chk(s675_expr(up, ({ wv t1350 = v766_k; wv t1351 = rt_get(&F_values, v764_a); rt_idx(t1350, t1351); })), "caseTest")); rt_cat(5, (wv[]){ rt_s(&S536), t1352, rt_s(&S385), t1353, rt_s(&S16) }); }); std_push(t1354, t1355); });
    v766_k = rt_add(v766_k, rt_i(1));
  }
  RT_RET(std_join(v765_tests, rt_s(&S537)));
}

static wv s670_matchElse(struct fr139 *up, wv v767_e, wv v768_t) {
  rt_enter("matchElse");
  v767_e = rt_bind(v767_e, 0);
  v768_t = rt_bind(v768_t, 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v767_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_cases, v767_e))))), rt_s(&S189)), "||")))) {
    RT_RET(rt_get(&F_els, v767_e));
  }
  RT_RET(({ wv t1356 = rt_arr(0, NULL, 0); wv t1357 = rt_rec(0, 4, K50, (wv[]){ rt_s(&S528), rt_cat(3, (wv[]){ rt_s(&S538), rt_text(v768_t), rt_s(&S16) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K21, (wv[]){ t1356, t1357, RT_NULL }); }));
}

static wv s671_matchStmt(struct fr139 *up, wv v769_e) {
  rt_enter("matchStmt");
  v769_e = rt_bind(v769_e, 0);
  wv v770_t = rt_bind(rt_chk(s649_tmp(up), "matchStmt"), 0);
  (void)rt_chk(s648_emit(up, ({ wv t1358 = rt_text(v770_t); wv t1359 = rt_text(rt_chk(s675_expr(up, rt_get(&F_subject, v769_e)), "matchStmt")); rt_cat(5, (wv[]){ rt_s(&S510), t1358, rt_s(&S495), t1359, rt_s(&S14) }); })), "matchStmt");
  wv v771_k = rt_i(0);
  while (rt_cond(({ wv t1360 = v771_k; wv t1361 = std_length(rt_get(&F_cases, v769_e)); rt_lt(t1360, t1361); }))) {
    (void)rt_chk(s648_emit(up, ({ wv t1366 = rt_text((rt_cond(rt_gt(v771_k, rt_i(0))) ? rt_s(&S529) : rt_s(&S13))); wv t1367 = rt_text(({ wv t1364 = v770_t; wv t1365 = ({ wv t1362 = v771_k; wv t1363 = rt_get(&F_cases, v769_e); rt_idx(t1362, t1363); }); rt_chk(s669_caseTest(up, t1364, t1365), "matchStmt"); })); rt_cat(4, (wv[]){ t1366, rt_s(&S530), t1367, rt_s(&S498) }); })), "matchStmt");
    up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
    (void)rt_chk(s651_blockBody(up, rt_get(&F_body, ({ wv t1368 = v771_k; wv t1369 = rt_get(&F_cases, v769_e); rt_idx(t1368, t1369); }))), "matchStmt");
    up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
    v771_k = rt_add(v771_k, rt_i(1));
  }
  wv v772_els = rt_bind(rt_chk(s670_matchElse(up, v769_e, v770_t), "matchStmt"), 0);
  if (rt_cond(rt_ne(v772_els, RT_NULL))) {
    (void)rt_chk(s648_emit(up, rt_s(&S531)), "matchStmt");
    up->v634_indent = rt_add(rt_def(up->v634_indent, "indent"), rt_i(1));
    (void)rt_chk(s651_blockBody(up, v772_els), "matchStmt");
    up->v634_indent = rt_sub(rt_def(up->v634_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s648_emit(up, rt_s(&S15)), "matchStmt");
  RT_RET(RT_NULL);
}

static wv s672_matchExpr(struct fr139 *up, wv v773_e) {
  rt_enter("matchExpr");
  v773_e = rt_bind(v773_e, 0);
  wv v774_t = rt_bind(rt_chk(s649_tmp(up), "matchExpr"), 0);
  wv v775_subject = rt_bind(rt_chk(s675_expr(up, rt_get(&F_subject, v773_e)), "matchExpr"), 0);
  wv v776_withElse = rt_bind(v773_e, 1);
  ({ wv t1370 = rt_chk(s670_matchElse(up, v773_e, v774_t), "matchExpr"); wv t1371 = v776_withElse; rt_set_mem(t1370, &F_els, t1371); });
  if (rt_cond(rt_chk(s665_isPlain(up, v776_withElse), "matchExpr"))) {
    RT_RET(({ wv t1372 = rt_text(v774_t); wv t1373 = rt_text(rt_chk(s666_ternaryChain(up, v776_withElse, v774_t), "matchExpr")); wv t1374 = rt_text(v775_subject); rt_cat(7, (wv[]){ rt_s(&S539), t1372, rt_s(&S540), t1373, rt_s(&S541), t1374, rt_s(&S16) }); }));
  }
  wv v777_body = rt_bind(rt_chk(s667_branchFunction(up, v776_withElse, v774_t), "matchExpr"), 0);
  RT_RET(({ wv t1375 = rt_text(v774_t); wv t1376 = rt_text(v777_body); wv t1377 = rt_text(rt_chk(w6_repeat(rt_s(&S480), rt_def(up->v634_indent, "indent")), "matchExpr")); wv t1378 = rt_text(v775_subject); rt_cat(9, (wv[]){ rt_s(&S539), t1375, rt_s(&S542), t1376, rt_s(&S2), t1377, rt_s(&S543), t1378, rt_s(&S16) }); }));
}

static wv s673_callJs(struct fr139 *up, wv v778_callee, wv v779_args) {
  rt_enter("callJs");
  v778_callee = rt_bind(v778_callee, 0);
  v779_args = rt_bind(v779_args, 0);
  wv v780_func = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v778_callee), rt_s(&S156))) ? rt_cat(2, (wv[]){ rt_s(&S544), rt_text(rt_get(&F_name, v778_callee)) }) : (rt_cond(rt_eq(rt_get(&F_k, v778_callee), rt_s(&S189))) ? ({ wv t1379 = rt_text(rt_chk(s642_modId(up, rt_chk(s645_stolenFrom(up, rt_get(&F_rec, v778_callee)), "callJs")), "callJs")); wv t1380 = rt_text(rt_get(&F_field, v778_callee)); rt_cat(4, (wv[]){ rt_s(&S478), t1379, rt_s(&S12), t1380 }); }) : rt_chk(s644_id(up, rt_get(&F_name, v778_callee)), "callJs"))), 0);
  RT_RET(({ wv t1381 = rt_text(v780_func); wv t1382 = rt_text(std_join(v779_args, rt_s(&S385))); rt_cat(4, (wv[]){ t1381, rt_s(&S97), t1382, rt_s(&S16) }); }));
}

static wv s674_exprList(struct fr139 *up, wv v781_list) {
  rt_enter("exprList");
  v781_list = rt_bind(v781_list, 0);
  wv v782_js = rt_arr(0, NULL, 1);
  wv v783_k = rt_i(0);
  while (rt_cond(({ wv t1383 = v783_k; wv t1384 = std_length(v781_list); rt_lt(t1383, t1384); }))) {
    (void)({ wv t1385 = v782_js; wv t1386 = rt_chk(s675_expr(up, rt_idx(v783_k, v781_list)), "exprList"); std_push(t1385, t1386); });
    v783_k = rt_add(v783_k, rt_i(1));
  }
  RT_RET(std_join(v782_js, rt_s(&S385)));
}

static wv s675_expr(struct fr139 *up, wv v784_e) {
  rt_enter("expr");
  v784_e = rt_bind(v784_e, 0);
  wv v785_kind = rt_bind(rt_get(&F_k, v784_e), 0);
  if (rt_cond(rt_eq(v785_kind, rt_s(&S138)))) {
    RT_RET((rt_cond(rt_lt(rt_get(&F_v, v784_e), rt_i(0))) ? rt_cat(3, (wv[]){ rt_s(&S97), rt_text(rt_get(&F_v, v784_e)), rt_s(&S16) }) : rt_cat(1, (wv[]){ rt_text(rt_get(&F_v, v784_e)) })));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S148)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S545), rt_text(rt_chk(w6_jsFloat(rt_get(&F_v, v784_e)), "expr")), rt_s(&S16) }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S171)))) {
    wv v786_parts = rt_arr(0, NULL, 1);
    wv v787_k = rt_i(0);
    while (rt_cond(({ wv t1387 = v787_k; wv t1388 = std_length(rt_get(&F_parts, v784_e)); rt_lt(t1387, t1388); }))) {
      wv v788_part = rt_bind(({ wv t1389 = v787_k; wv t1390 = rt_get(&F_parts, v784_e); rt_idx(t1389, t1390); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v788_part), rt_s(&S185)))) {
        if (rt_cond(rt_ne(v788_part, rt_s(&S13)))) {
          (void)({ wv t1391 = v786_parts; wv t1392 = rt_chk(w1_quote(v788_part), "expr"); std_push(t1391, t1392); });
        }
      } else {
        (void)({ wv t1393 = v786_parts; wv t1394 = rt_cat(3, (wv[]){ rt_s(&S546), rt_text(rt_chk(s675_expr(up, v788_part), "expr")), rt_s(&S16) }); std_push(t1393, t1394); });
      }
      v787_k = rt_add(v787_k, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v786_parts), rt_i(0)))) {
      RT_RET(rt_s(&S547));
    }
    if (rt_cond(rt_eq(std_length(v786_parts), rt_i(1)))) {
      RT_RET(rt_idx(rt_i(0), v786_parts));
    }
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S97), rt_text(std_join(v786_parts, rt_s(&S548))), rt_s(&S16) }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S281)))) {
    RT_RET(rt_chk(w6_jsBool(rt_get(&F_v, v784_e)), "expr"));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S51)))) {
    RT_RET(rt_s(&S51));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S52)))) {
    RT_RET(rt_s(&S549));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S53)))) {
    RT_RET(rt_s(&S550));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S211)))) {
    RT_RET(rt_chk(s644_id(up, rt_get(&F_name, v784_e)), "expr"));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S156)))) {
    RT_RET(rt_cat(2, (wv[]){ rt_s(&S544), rt_text(rt_get(&F_name, v784_e)) }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S186)))) {
    RT_RET(rt_cat(3, (wv[]){ rt_s(&S507), rt_text(rt_chk(s674_exprList(up, rt_get(&F_items, v784_e)), "expr")), rt_s(&S508) }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S187)))) {
    RT_RET(rt_chk(s658_record(up, v784_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S188)))) {
    RT_RET(({ wv t1395 = rt_text(rt_chk(s675_expr(up, rt_get(&F_index, v784_e)), "expr")); wv t1396 = rt_text(rt_chk(s675_expr(up, rt_get(&F_arr, v784_e)), "expr")); rt_cat(5, (wv[]){ rt_s(&S551), t1395, rt_s(&S385), t1396, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S189)))) {
    wv v789_lin = rt_bind(rt_chk(s647_enumJs(up, rt_get(&F_rec, v784_e)), "expr"), 0);
    if (rt_cond(rt_ne(v789_lin, RT_NULL))) {
      RT_RET(({ wv t1397 = rt_text(v789_lin); wv t1398 = rt_text(rt_get(&F_field, v784_e)); rt_cat(3, (wv[]){ t1397, rt_s(&S552), t1398 }); }));
    }
    wv v790_lib = rt_bind(rt_chk(s645_stolenFrom(up, rt_get(&F_rec, v784_e)), "expr"), 0);
    if (rt_cond(rt_ne(v790_lib, RT_NULL))) {
      RT_RET(({ wv t1399 = rt_text(rt_chk(s642_modId(up, v790_lib), "expr")); wv t1400 = rt_text(rt_get(&F_field, v784_e)); rt_cat(4, (wv[]){ rt_s(&S478), t1399, rt_s(&S12), t1400 }); }));
    }
    RT_RET(({ wv t1401 = rt_text(rt_get(&F_field, v784_e)); wv t1402 = rt_text(rt_chk(s675_expr(up, rt_get(&F_rec, v784_e)), "expr")); rt_cat(5, (wv[]){ rt_s(&S553), t1401, rt_s(&S513), t1402, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S190)))) {
    RT_RET(({ wv t1403 = rt_text(rt_chk(w6_unFn(rt_get(&F_op, v784_e)), "expr")); wv t1404 = rt_text(rt_chk(s675_expr(up, rt_get(&F_e, v784_e)), "expr")); rt_cat(5, (wv[]){ rt_s(&S554), t1403, rt_s(&S97), t1404, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S191)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v784_e), rt_s(&S80)), "||") || rt_bool(rt_eq(rt_get(&F_op, v784_e), rt_s(&S81)), "||")))) {
      RT_RET(({ wv t1405 = rt_text(rt_chk(s675_expr(up, rt_get(&F_l, v784_e)), "expr")); wv t1406 = rt_text(rt_get(&F_op, v784_e)); wv t1407 = rt_text(rt_get(&F_op, v784_e)); wv t1408 = rt_text(rt_chk(s675_expr(up, rt_get(&F_r, v784_e)), "expr")); wv t1409 = rt_text(rt_get(&F_op, v784_e)); rt_cat(11, (wv[]){ rt_s(&S555), t1405, rt_s(&S556), t1406, rt_s(&S557), t1407, rt_s(&S558), t1408, rt_s(&S556), t1409, rt_s(&S559) }); }));
    }
    RT_RET(({ wv t1410 = rt_text(rt_chk(w6_binFn(rt_get(&F_op, v784_e)), "expr")); wv t1411 = rt_text(rt_chk(s675_expr(up, rt_get(&F_l, v784_e)), "expr")); wv t1412 = rt_text(rt_chk(s675_expr(up, rt_get(&F_r, v784_e)), "expr")); rt_cat(7, (wv[]){ rt_s(&S554), t1410, rt_s(&S97), t1411, rt_s(&S385), t1412, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S192)))) {
    RT_RET(({ wv t1413 = rt_text(rt_chk(s675_expr(up, rt_get(&F_c, v784_e)), "expr")); wv t1414 = rt_text(rt_chk(s675_expr(up, rt_get(&F_a, v784_e)), "expr")); wv t1415 = rt_text(rt_chk(s675_expr(up, rt_get(&F_b, v784_e)), "expr")); rt_cat(7, (wv[]){ rt_s(&S560), t1413, rt_s(&S561), t1414, rt_s(&S533), t1415, rt_s(&S16) }); }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S182)))) {
    wv v791_args = rt_arr(0, NULL, 1);
    wv v792_k = rt_i(0);
    while (rt_cond(({ wv t1416 = v792_k; wv t1417 = std_length(rt_get(&F_args, v784_e)); rt_lt(t1416, t1417); }))) {
      (void)({ wv t1420 = v791_args; wv t1421 = rt_chk(s675_expr(up, ({ wv t1418 = v792_k; wv t1419 = rt_get(&F_args, v784_e); rt_idx(t1418, t1419); })), "expr"); std_push(t1420, t1421); });
      v792_k = rt_add(v792_k, rt_i(1));
    }
    RT_RET(({ wv t1422 = rt_get(&F_callee, v784_e); wv t1423 = v791_args; rt_chk(s673_callJs(up, t1422, t1423), "expr"); }));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S184)))) {
    RT_RET(rt_chk(s668_chainExpr(up, v784_e), "expr"));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S44)))) {
    RT_RET(rt_chk(s672_matchExpr(up, v784_e), "expr"));
  }
  if (rt_cond(rt_eq(v785_kind, rt_s(&S528)))) {
    RT_RET(rt_get(&F_js, v784_e));
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S562), rt_text(v785_kind) }));
  RT_RET(RT_NULL);
}

static wv w7_binFn(wv v795_op) {
  rt_enter("binFn");
  v795_op = rt_bind(v795_op, 0);
  wv v796_k = rt_i(0);
  while (rt_cond(({ wv t1436 = ({ wv t1434 = v796_k; wv t1435 = rt_def(w7_binOps, "binOps"); rt_idx(t1434, t1435); }); wv t1437 = v795_op; rt_ne(t1436, t1437); }))) {
    v796_k = rt_add(v796_k, rt_i(1));
  }
  RT_RET(({ wv t1438 = v796_k; wv t1439 = rt_def(w7_binFns, "binFns"); rt_idx(t1438, t1439); }));
}

static wv w7_modeOf(wv v797_x) {
  rt_enter("modeOf");
  v797_x = rt_bind(v797_x, 0);
  RT_RET((rt_cond(rt_get(&F_isVar, v797_x)) ? rt_s(&S36) : (rt_cond(rt_get(&F_isRef, v797_x)) ? rt_s(&S37) : rt_s(&S35))));
}

static wv w7_unFn(wv v798_op) {
  rt_enter("unFn");
  v798_op = rt_bind(v798_op, 0);
  RT_RET((rt_cond(rt_eq(v798_op, rt_s(&S85))) ? rt_s(&S475) : (rt_cond(rt_eq(v798_op, rt_s(&S94))) ? rt_s(&S476) : rt_s(&S477))));
}

static wv w7_repeat(wv v799_s, wv v800_count) {
  rt_enter("repeat");
  v799_s = rt_bind(v799_s, 0);
  v800_count = rt_bind(v800_count, 0);
  wv v801_out = rt_s(&S13);
  wv v802_k = rt_i(0);
  while (rt_cond(rt_lt(v802_k, v800_count))) {
    v801_out = rt_add(v801_out, v799_s);
    v802_k = rt_add(v802_k, rt_i(1));
  }
  RT_RET(v801_out);
}

static wv w7_utf8Length(wv v803_s) {
  rt_enter("utf8Length");
  v803_s = rt_bind(v803_s, 0);
  wv v804_cs = rt_bind(std_split(v803_s, rt_s(&S13)), 0);
  wv v805_n = rt_i(0);
  wv v806_k = rt_i(0);
  while (rt_cond(({ wv t1440 = v806_k; wv t1441 = std_length(v804_cs); rt_lt(t1440, t1441); }))) {
    wv v807_c = rt_bind(std_code(rt_idx(v806_k, v804_cs)), 0);
    wv t1442 = (rt_cond(rt_lt(v807_c, rt_i(128))) ? rt_i(1) : (rt_cond(rt_lt(v807_c, rt_i(2048))) ? rt_i(2) : (rt_cond(rt_lt(v807_c, rt_i(65536))) ? rt_i(3) : rt_i(4))));
    v805_n = rt_add(v805_n, t1442);
    v806_k = rt_add(v806_k, rt_i(1));
  }
  RT_RET(v805_n);
}

static wv w7_cString(wv v808_s) {
  rt_enter("cString");
  v808_s = rt_bind(v808_s, 0);
  wv v809_cs = rt_bind(std_split(v808_s, rt_s(&S13)), 0);
  wv v810_out = rt_s(&S18);
  wv v811_k = rt_i(0);
  while (rt_cond(({ wv t1443 = v811_k; wv t1444 = std_length(v809_cs); rt_lt(t1443, t1444); }))) {
    wv v812_ch = rt_bind(rt_idx(v811_k, v809_cs), 0);
    wv v813_c = rt_bind(std_code(v812_ch), 0);
    if (rt_cond(rt_eq(v812_ch, rt_s(&S18)))) {
      v810_out = rt_add(v810_out, rt_s(&S566));
    } else if (rt_cond(rt_eq(v812_ch, rt_s(&S19)))) {
      v810_out = rt_add(v810_out, rt_s(&S567));
    } else if (rt_cond(rt_eq(v812_ch, rt_s(&S2)))) {
      v810_out = rt_add(v810_out, rt_s(&S20));
    } else if (rt_cond(rt_eq(v812_ch, rt_s(&S1)))) {
      v810_out = rt_add(v810_out, rt_s(&S22));
    } else if (rt_cond(rt_eq(v812_ch, rt_s(&S3)))) {
      v810_out = rt_add(v810_out, rt_s(&S21));
    } else if (rt_cond(rt_b(rt_bool(rt_lt(v813_c, rt_i(32)), "||") || rt_bool(rt_eq(v813_c, rt_i(127)), "||")))) {
      v810_out = ({ wv t1448 = v810_out; wv t1449 = ({ wv t1445 = rt_text(std_floor(rt_div(v813_c, rt_i(64)))); wv t1446 = rt_text(rt_mod(std_floor(rt_div(v813_c, rt_i(8))), rt_i(8))); wv t1447 = rt_text(rt_mod(v813_c, rt_i(8))); rt_cat(4, (wv[]){ rt_s(&S19), t1445, t1446, t1447 }); }); rt_add(t1448, t1449); });
    } else {
      v810_out = rt_add(v810_out, v812_ch);
    }
    v811_k = rt_add(v811_k, rt_i(1));
  }
  RT_RET(rt_add(v810_out, rt_s(&S18)));
}

static wv w7_wstrInit(wv v814_s) {
  rt_enter("wstrInit");
  v814_s = rt_bind(v814_s, 0);
  wv v815_bytes = rt_bind(rt_chk(w7_utf8Length(v814_s), "wstrInit"), 0);
  wv v816_cps = rt_bind(std_length(v814_s), 0);
  RT_RET(({ wv t1450 = rt_text(v815_bytes); wv t1451 = rt_text(v816_cps); wv t1452 = rt_text((rt_cond(rt_eq(v815_bytes, v816_cps)) ? rt_i(1) : rt_i(0))); wv t1453 = rt_text(rt_chk(w7_cString(v814_s), "wstrInit")); rt_cat(9, (wv[]){ rt_s(&S568), t1450, rt_s(&S385), t1451, rt_s(&S385), t1452, rt_s(&S569), t1453, rt_s(&S570) }); }));
}

static wv w7_values(wv v817_cs) {
  rt_enter("values");
  RT_RET((rt_cond(rt_eq(std_length(v817_cs), rt_i(0))) ? rt_s(&S571) : rt_cat(3, (wv[]){ rt_s(&S572), rt_text(std_join(v817_cs, rt_s(&S385))), rt_s(&S570) })));
}

static wv w7_generateC(wv p_v818_modules, wv p_v819_runtime) {
  rt_enter("generateC");
  struct fr183 fr = { 0 };
  fr.v818_modules = rt_bind(p_v818_modules, 0);
  fr.v819_runtime = rt_bind(p_v819_runtime, 0);
  fr.v820_n = rt_i(0);
  fr.v821_tmps = rt_i(0);
  fr.v822_strings = rt_arr(0, NULL, 1);
  fr.v823_fields = rt_arr(0, NULL, 1);
  fr.v824_shapes = rt_arr(0, NULL, 1);
  fr.v825_enumDefs = rt_arr(0, NULL, 1);
  fr.v826_globals = rt_arr(0, NULL, 1);
  fr.v827_frames = rt_arr(0, NULL, 1);
  fr.v828_functions = rt_arr(0, NULL, 1);
  fr.v829_out = rt_arr(0, NULL, 1);
  fr.v830_indent = rt_i(1);
  fr.v831_loops = rt_i(0);
  fr.v832_iife = rt_b(0);
  fr.v833_decls = rt_arr(0, NULL, 1);
  fr.v834_fns = rt_arr(0, NULL, 1);
  fr.v835_tops = rt_arr(0, NULL, 1);
  fr.v836_slots = rt_arr(0, NULL, 1);
  fr.v837_curMod = rt_i(0);
  fr.v838_mod = rt_s(&S13);
  fr.v839_curSlot = rt_arr(0, NULL, 1);
  fr.v840_stolen = rt_arr(0, NULL, 1);
  fr.v841_ownEnums = rt_arr(0, NULL, 1);
  fr.v842_cur = rt_i(0);
  wv v902_mi = rt_i(0);
  while (rt_cond(({ wv t2385 = v902_mi; wv t2386 = std_length(fr.v818_modules); rt_lt(t2385, t2386); }))) {
    (void)rt_chk(s857_analyze(&fr, v902_mi), "generateC");
    v902_mi = rt_add(v902_mi, rt_i(1));
  }
  v902_mi = rt_i(0);
  while (rt_cond(({ wv t2387 = v902_mi; wv t2388 = std_length(fr.v818_modules); rt_lt(t2387, t2388); }))) {
    wv v1176_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v902_mi, fr.v818_modules))), 0);
    wv v1177_li = rt_i(0);
    while (rt_cond(({ wv t2389 = v1177_li; wv t2390 = std_length(v1176_list); rt_lt(t2389, t2390); }))) {
      if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v1177_li, v1176_list)), rt_s(&S46)))) {
        (void)({ wv t2391 = rt_idx(v1177_li, v1176_list); wv t2392 = rt_chk(s843_modId(&fr, v902_mi), "generateC"); rt_chk(s863_enumDef(&fr, t2391, t2392), "generateC"); });
      }
      v1177_li = rt_add(v1177_li, rt_i(1));
    }
    v902_mi = rt_add(v902_mi, rt_i(1));
  }
  v902_mi = rt_i(0);
  while (rt_cond(({ wv t2393 = v902_mi; wv t2394 = std_length(fr.v818_modules); rt_lt(t2393, t2394); }))) {
    (void)rt_chk(s858_enter(&fr, v902_mi), "generateC");
    (void)rt_chk(s872_stmts(&fr, rt_get(&F_stmts, rt_get(&F_program, rt_idx(v902_mi, fr.v818_modules)))), "generateC");
    v902_mi = rt_add(v902_mi, rt_i(1));
  }
  RT_RET(rt_chk(s901_assemble(&fr), "generateC"));
}

static wv s843_modId(struct fr183 *up, wv v903_m) {
  rt_enter("modId");
  v903_m = rt_bind(v903_m, 0);
  RT_RET((rt_cond(({ wv t1454 = v903_m; wv t1455 = rt_sub(std_length(up->v818_modules), rt_i(1)); rt_eq(t1454, t1455); })) ? rt_s(&S13) : rt_cat(1, (wv[]){ rt_text(rt_add(v903_m, rt_i(1))) })));
}

static wv s844_hasFrame(struct fr183 *up, wv v904_f) {
  rt_enter("hasFrame");
  v904_f = rt_bind(v904_f, 0);
  RT_RET(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1456 = v904_f; wv t1457 = rt_def(up->v834_fns, "fns"); rt_idx(t1456, t1457); }))), "&&") && rt_bool(rt_gt(rt_get(&F_children, ({ wv t1458 = v904_f; wv t1459 = rt_def(up->v834_fns, "fns"); rt_idx(t1458, t1459); })), rt_i(0)), "&&")));
}

static wv s845_newScope(struct fr183 *up, wv v905_parent) {
  rt_enter("newScope");
  v905_parent = rt_bind(v905_parent, 0);
  wv v906_names = rt_arr(0, NULL, 1);
  RT_RET(rt_rec(0, 2, K40, (wv[]){ v905_parent, v906_names }));
}

static wv s846_put(struct fr183 *up, wv v907_scope, wv v908_name, wv v909_kind, wv v910_d) {
  rt_enter("put");
  v907_scope = rt_bind(v907_scope, 0);
  v908_name = rt_bind(v908_name, 0);
  v909_kind = rt_bind(v909_kind, 0);
  v910_d = rt_bind(v910_d, 0);
  (void)({ wv t1460 = rt_get(&F_names, v907_scope); wv t1461 = rt_rec(0, 3, K51, (wv[]){ v908_name, v909_kind, v910_d }); std_push(t1460, t1461); });
  RT_RET(RT_NULL);
}

static wv s847_find(struct fr183 *up, wv v911_scope, wv v912_name) {
  rt_enter("find");
  v912_name = rt_bind(v912_name, 0);
  wv v913_s = v911_scope;
  while (rt_cond(rt_ne(v913_s, RT_NULL))) {
    wv v914_ns = rt_get(&F_names, v913_s);
    wv v915_k = rt_i(0);
    while (rt_cond(({ wv t1462 = v915_k; wv t1463 = std_length(v914_ns); rt_lt(t1462, t1463); }))) {
      if (rt_cond(({ wv t1464 = rt_get(&F_name, rt_idx(v915_k, v914_ns)); wv t1465 = v912_name; rt_eq(t1464, t1465); }))) {
        RT_RET(rt_idx(v915_k, v914_ns));
      }
      v915_k = rt_add(v915_k, rt_i(1));
    }
    v913_s = rt_get(&F_parent, v913_s);
  }
  RT_RET(RT_NULL);
}

static wv s848_kindOf(struct fr183 *up, wv v916_scope, wv v917_name) {
  rt_enter("kindOf");
  v916_scope = rt_bind(v916_scope, 0);
  v917_name = rt_bind(v917_name, 0);
  wv v918_entry = rt_bind(rt_chk(s847_find(up, v916_scope, v917_name), "kindOf"), 0);
  RT_RET((rt_cond(rt_eq(v918_entry, RT_NULL)) ? RT_NULL : rt_get(&F_kind, v918_entry)));
}

static wv s849_newFn(struct fr183 *up, wv v919_name, wv v920_parent) {
  rt_enter("newFn");
  v919_name = rt_bind(v919_name, 0);
  v920_parent = rt_bind(v920_parent, 0);
  wv v921_f = ({ wv t1468 = v919_name; wv t1469 = v920_parent; wv t1470 = (rt_cond(rt_eq(v920_parent, RT_NULL)) ? rt_i(0) : rt_add(rt_get(&F_depth, ({ wv t1466 = v920_parent; wv t1467 = rt_def(up->v834_fns, "fns"); rt_idx(t1466, t1467); })), rt_i(1))); wv t1471 = rt_eq(v920_parent, RT_NULL); rt_rec(1, 6, K52, (wv[]){ t1468, t1469, t1470, t1471, rt_i(0), rt_s(&S13) }); });
  (void)({ wv t1472 = rt_def(up->v834_fns, "fns"); wv t1473 = v921_f; std_push(t1472, t1473); });
  RT_RET(rt_sub(std_length(rt_def(up->v834_fns, "fns")), rt_i(1)));
}

static wv s850_bind(struct fr183 *up, wv v922_kind, wv v923_name, wv v924_f, wv v925_atTop, wv v926_mode) {
  rt_enter("bind");
  v922_kind = rt_bind(v922_kind, 0);
  v923_name = rt_bind(v923_name, 0);
  v924_f = rt_bind(v924_f, 0);
  v925_atTop = rt_bind(v925_atTop, 0);
  v926_mode = rt_bind(v926_mode, 0);
  wv v927_cname = ({ wv t1474 = rt_text(rt_def(up->v838_mod, "mod")); wv t1475 = rt_text(v923_name); rt_cat(4, (wv[]){ rt_s(&S478), t1474, rt_s(&S12), t1475 }); });
  if (rt_cond(rt_not(v925_atTop))) {
    v927_cname = ({ wv t1478 = (rt_cond(rt_eq(v922_kind, rt_s(&S33))) ? rt_s(&S362) : rt_s(&S573)); wv t1479 = ({ wv t1476 = rt_text(rt_def(up->v820_n, "n")); wv t1477 = rt_text(v923_name); rt_cat(3, (wv[]){ t1476, rt_s(&S12), t1477 }); }); rt_add(t1478, t1479); });
    up->v820_n = rt_add(rt_def(up->v820_n, "n"), rt_i(1));
  }
  wv v928_d = ({ wv t1480 = v922_kind; wv t1481 = v923_name; wv t1482 = v924_f; wv t1483 = v927_cname; wv t1484 = rt_eq(v926_mode, rt_s(&S37)); rt_rec(1, 7, K53, (wv[]){ t1480, t1481, t1482, t1483, t1484, rt_b(0), RT_NULL }); });
  (void)({ wv t1485 = rt_def(up->v833_decls, "decls"); wv t1486 = v928_d; std_push(t1485, t1486); });
  if (rt_cond(rt_b(rt_bool(rt_get(&F_top, ({ wv t1487 = v924_f; wv t1488 = rt_def(up->v834_fns, "fns"); rt_idx(t1487, t1488); })), "&&") && rt_bool(rt_ne(v922_kind, rt_s(&S33)), "&&")))) {
    (void)({ wv t1489 = rt_def(up->v826_globals, "globals"); wv t1490 = v927_cname; std_push(t1489, t1490); });
  }
  RT_RET(rt_sub(std_length(rt_def(up->v833_decls, "decls")), rt_i(1)));
}

static wv s851_ablock(struct fr183 *up, wv v929_b, wv v930_scope, wv v931_f, wv v932_topScope) {
  rt_enter("ablock");
  v930_scope = rt_bind(v930_scope, 0);
  v931_f = rt_bind(v931_f, 0);
  v932_topScope = rt_bind(v932_topScope, 0);
  wv v933_atTop = rt_eq(v930_scope, v932_topScope);
  wv v934_list = rt_bind(rt_get(&F_stmts, v929_b), 0);
  wv v935_k = rt_i(0);
  while (rt_cond(({ wv t1491 = v935_k; wv t1492 = std_length(v934_list); rt_lt(t1491, t1492); }))) {
    wv v936_s = rt_bind(rt_idx(v935_k, v934_list), 0);
    if (rt_cond(rt_eq(rt_get(&F_k, v936_s), rt_s(&S33)))) {
      wv v937_d = rt_bind(({ wv t1493 = rt_get(&F_name, v936_s); wv t1494 = v931_f; wv t1495 = v933_atTop; rt_chk(s850_bind(up, rt_s(&S33), t1493, t1494, t1495, rt_s(&S13)), "ablock"); }), 0);
      wv v938_child = rt_bind(({ wv t1496 = rt_get(&F_name, v936_s); wv t1497 = v931_f; rt_chk(s849_newFn(up, t1496, t1497), "ablock"); }), 0);
      ({ wv t1502 = rt_get(&F_cname, ({ wv t1498 = v937_d; wv t1499 = rt_def(up->v833_decls, "decls"); rt_idx(t1498, t1499); })); wv t1503 = ({ wv t1500 = v938_child; wv t1501 = rt_def(up->v834_fns, "fns"); rt_idx(t1500, t1501); }); rt_set_mem(t1502, &F_cname, t1503); });
      ({ wv t1506 = v938_child; wv t1507 = ({ wv t1504 = v937_d; wv t1505 = rt_def(up->v833_decls, "decls"); rt_idx(t1504, t1505); }); rt_set_mem(t1506, &F_fnIndex, t1507); });
      rt_upd_mem(rt_i(1), &F_children, ({ wv t1508 = v931_f; wv t1509 = rt_def(up->v834_fns, "fns"); rt_idx(t1508, t1509); }), rt_add);
      ({ wv t1510 = v937_d; wv t1511 = rt_get(&F_namePos, v936_s); wv t1512 = rt_def(up->v839_curSlot, "curSlot"); rt_set_idx(t1510, t1511, t1512); });
      (void)({ wv t1513 = v930_scope; wv t1514 = rt_get(&F_name, v936_s); wv t1515 = v937_d; rt_chk(s846_put(up, t1513, t1514, rt_s(&S33), t1515), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v936_s), rt_s(&S46)))) {
      (void)({ wv t1516 = v930_scope; wv t1517 = rt_get(&F_name, v936_s); rt_chk(s846_put(up, t1516, t1517, rt_s(&S46), RT_NULL), "ablock"); });
    } else if (rt_cond(rt_eq(rt_get(&F_k, v936_s), rt_s(&S177)))) {
      wv v939_d = rt_bind(({ wv t1518 = rt_get(&F_name, v936_s); wv t1519 = v931_f; wv t1520 = v933_atTop; wv t1521 = rt_chk(w7_modeOf(v936_s), "ablock"); rt_chk(s850_bind(up, rt_s(&S177), t1518, t1519, t1520, t1521), "ablock"); }), 0);
      ({ wv t1522 = v939_d; wv t1523 = rt_get(&F_namePos, v936_s); wv t1524 = rt_def(up->v839_curSlot, "curSlot"); rt_set_idx(t1522, t1523, t1524); });
      (void)({ wv t1525 = v930_scope; wv t1526 = rt_get(&F_name, v936_s); wv t1527 = v939_d; rt_chk(s846_put(up, t1525, t1526, rt_s(&S177), t1527), "ablock"); });
    }
    v935_k = rt_add(v935_k, rt_i(1));
  }
  v935_k = rt_i(0);
  while (rt_cond(({ wv t1528 = v935_k; wv t1529 = std_length(v934_list); rt_lt(t1528, t1529); }))) {
    (void)({ wv t1530 = rt_idx(v935_k, v934_list); wv t1531 = v930_scope; wv t1532 = v931_f; wv t1533 = v932_topScope; rt_chk(s852_astmt(up, t1530, t1531, t1532, t1533), "ablock"); });
    v935_k = rt_add(v935_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_value, v929_b), RT_NULL))) {
    (void)({ wv t1534 = rt_get(&F_value, v929_b); wv t1535 = v930_scope; wv t1536 = v931_f; rt_chk(s856_aexpr(up, t1534, t1535, t1536), "ablock"); });
  }
  RT_RET(RT_NULL);
}

static wv s852_astmt(struct fr183 *up, wv v940_s, wv v941_scope, wv v942_f, wv v943_topScope) {
  rt_enter("astmt");
  v941_scope = rt_bind(v941_scope, 0);
  v942_f = rt_bind(v942_f, 0);
  v943_topScope = rt_bind(v943_topScope, 0);
  wv v944_kind = rt_bind(rt_get(&F_k, v940_s), 0);
  if (rt_cond(rt_eq(v944_kind, rt_s(&S33)))) {
    wv v945_d = rt_bind(({ wv t1537 = rt_get(&F_namePos, v940_s); wv t1538 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t1537, t1538); }), 0);
    wv v946_child = rt_bind(rt_get(&F_fnIndex, ({ wv t1539 = v945_d; wv t1540 = rt_def(up->v833_decls, "decls"); rt_idx(t1539, t1540); })), 0);
    wv v947_ps = rt_bind(rt_chk(s845_newScope(up, v941_scope), "astmt"), 0);
    wv v948_k = rt_i(0);
    while (rt_cond(({ wv t1541 = v948_k; wv t1542 = std_length(rt_get(&F_params, v940_s)); rt_lt(t1541, t1542); }))) {
      wv v949_p = rt_bind(({ wv t1543 = v948_k; wv t1544 = rt_get(&F_params, v940_s); rt_idx(t1543, t1544); }), 0);
      wv v950_pd = rt_bind(({ wv t1545 = rt_get(&F_name, v949_p); wv t1546 = v946_child; wv t1547 = rt_chk(w7_modeOf(v949_p), "astmt"); rt_chk(s850_bind(up, rt_s(&S373), t1545, t1546, rt_b(0), t1547), "astmt"); }), 0);
      ({ wv t1548 = v950_pd; wv t1549 = rt_get(&F_pos, v949_p); wv t1550 = rt_def(up->v839_curSlot, "curSlot"); rt_set_idx(t1548, t1549, t1550); });
      (void)({ wv t1551 = v947_ps; wv t1552 = rt_get(&F_name, v949_p); wv t1553 = v950_pd; rt_chk(s846_put(up, t1551, t1552, rt_s(&S373), t1553), "astmt"); });
      v948_k = rt_add(v948_k, rt_i(1));
    }
    (void)({ wv t1554 = rt_get(&F_body, v940_s); wv t1555 = rt_chk(s845_newScope(up, v947_ps), "astmt"); wv t1556 = v946_child; wv t1557 = v943_topScope; rt_chk(s851_ablock(up, t1554, t1555, t1556, t1557), "astmt"); });
  } else if (rt_cond(rt_eq(v944_kind, rt_s(&S41)))) {
    (void)({ wv t1558 = rt_get(&F_cond, v940_s); wv t1559 = v941_scope; wv t1560 = v942_f; rt_chk(s856_aexpr(up, t1558, t1559, t1560), "astmt"); });
    (void)({ wv t1561 = rt_get(&F_body, v940_s); wv t1562 = rt_chk(s845_newScope(up, v941_scope), "astmt"); wv t1563 = v942_f; wv t1564 = v943_topScope; rt_chk(s851_ablock(up, t1561, t1562, t1563, t1564), "astmt"); });
  } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v944_kind, rt_s(&S34)), "||") || rt_bool(rt_eq(v944_kind, rt_s(&S57)), "||")), "||") || rt_bool(rt_eq(v944_kind, rt_s(&S54)), "||")), "||") || rt_bool(rt_eq(v944_kind, rt_s(&S177)), "||")))) {
    if (rt_cond(rt_ne(rt_get(&F_e, v940_s), RT_NULL))) {
      (void)({ wv t1565 = rt_get(&F_e, v940_s); wv t1566 = v941_scope; wv t1567 = v942_f; rt_chk(s856_aexpr(up, t1565, t1566, t1567), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v944_kind, rt_s(&S176)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v940_s)), rt_s(&S183)))) {
      (void)({ wv t1568 = rt_get(&F_arg, rt_get(&F_e, v940_s)); wv t1569 = v941_scope; wv t1570 = v942_f; rt_chk(s856_aexpr(up, t1568, t1569, t1570), "astmt"); });
    } else {
      (void)({ wv t1571 = rt_get(&F_e, v940_s); wv t1572 = v941_scope; wv t1573 = v942_f; rt_chk(s856_aexpr(up, t1571, t1572, t1573), "astmt"); });
    }
  } else if (rt_cond(rt_eq(v944_kind, rt_s(&S38)))) {
    (void)({ wv t1574 = rt_get(&F_e, v940_s); wv t1575 = v941_scope; wv t1576 = v942_f; rt_chk(s856_aexpr(up, t1574, t1575, t1576), "astmt"); });
    (void)({ wv t1577 = rt_get(&F_target, v940_s); wv t1578 = v941_scope; wv t1579 = v942_f; rt_chk(s854_atarget(up, t1577, t1578, t1579), "astmt"); });
  }
  if (rt_cond(rt_ne(rt_get(&F_handler, v940_s), RT_NULL))) {
    wv v951_h = rt_bind(rt_get(&F_handler, v940_s), 0);
    wv v952_hs = rt_bind(rt_chk(s845_newScope(up, v941_scope), "astmt"), 0);
    if (rt_cond(rt_ne(rt_get(&F_param, v951_h), RT_NULL))) {
      wv v953_wd = rt_bind(({ wv t1580 = rt_get(&F_param, v951_h); wv t1581 = v942_f; rt_chk(s850_bind(up, rt_s(&S56), t1580, t1581, rt_b(0), rt_s(&S13)), "astmt"); }), 0);
      ({ wv t1582 = v953_wd; wv t1583 = rt_get(&F_paramPos, v951_h); wv t1584 = rt_def(up->v839_curSlot, "curSlot"); rt_set_idx(t1582, t1583, t1584); });
      (void)({ wv t1585 = v952_hs; wv t1586 = rt_get(&F_param, v951_h); wv t1587 = v953_wd; rt_chk(s846_put(up, t1585, t1586, rt_s(&S56), t1587), "astmt"); });
    }
    (void)({ wv t1588 = rt_get(&F_body, v951_h); wv t1589 = rt_chk(s845_newScope(up, v952_hs), "astmt"); wv t1590 = v942_f; wv t1591 = v943_topScope; rt_chk(s851_ablock(up, t1588, t1589, t1590, t1591), "astmt"); });
  }
  RT_RET(RT_NULL);
}

static wv s853_use(struct fr183 *up, wv v954_x, wv v955_scope, wv v956_f) {
  rt_enter("use");
  v954_x = rt_bind(v954_x, 0);
  v955_scope = rt_bind(v955_scope, 0);
  v956_f = rt_bind(v956_f, 0);
  wv v957_d = rt_bind(rt_get(&F_d, ({ wv t1592 = v955_scope; wv t1593 = rt_get(&F_name, v954_x); rt_chk(s847_find(up, t1592, t1593), "use"); })), 0);
  ({ wv t1594 = v957_d; wv t1595 = rt_get(&F_pos, v954_x); wv t1596 = rt_def(up->v839_curSlot, "curSlot"); rt_set_idx(t1594, t1595, t1596); });
  if (rt_cond(({ wv t1599 = rt_get(&F_func, ({ wv t1597 = v957_d; wv t1598 = rt_def(up->v833_decls, "decls"); rt_idx(t1597, t1598); })); wv t1600 = v956_f; rt_ne(t1599, t1600); }))) {
    rt_set_mem(rt_b(1), &F_captured, ({ wv t1601 = v957_d; wv t1602 = rt_def(up->v833_decls, "decls"); rt_idx(t1601, t1602); }));
  }
  RT_RET(RT_NULL);
}

static wv s854_atarget(struct fr183 *up, wv v958_t, wv v959_scope, wv v960_f) {
  rt_enter("atarget");
  v958_t = rt_bind(v958_t, 0);
  v959_scope = rt_bind(v959_scope, 0);
  v960_f = rt_bind(v960_f, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v958_t), rt_s(&S211)))) {
    (void)rt_chk(s853_use(up, v958_t, v959_scope, v960_f), "atarget");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v958_t), rt_s(&S188)))) {
    (void)({ wv t1603 = rt_get(&F_index, v958_t); wv t1604 = v959_scope; wv t1605 = v960_f; rt_chk(s856_aexpr(up, t1603, t1604, t1605), "atarget"); });
  }
  (void)({ wv t1606 = (rt_cond(rt_eq(rt_get(&F_k, v958_t), rt_s(&S189))) ? rt_get(&F_rec, v958_t) : rt_get(&F_arr, v958_t)); wv t1607 = v959_scope; wv t1608 = v960_f; rt_chk(s856_aexpr(up, t1606, t1607, t1608), "atarget"); });
  RT_RET(RT_NULL);
}

static wv s855_notValue(struct fr183 *up, wv v961_x, wv v962_scope) {
  rt_enter("notValue");
  v962_scope = rt_bind(v962_scope, 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v961_x), rt_s(&S189)))) {
    RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_get(&F_rec, v961_x)), rt_s(&S211)), "&&") && rt_bool(rt_eq(({ wv t1609 = v962_scope; wv t1610 = rt_get(&F_name, rt_get(&F_rec, v961_x)); rt_chk(s848_kindOf(up, t1609, t1610), "notValue"); }), rt_s(&S388)), "&&")));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v961_x), rt_s(&S211)))) {
    RT_RET(rt_b(0));
  }
  wv v963_kind = rt_bind(({ wv t1611 = v962_scope; wv t1612 = rt_get(&F_name, v961_x); rt_chk(s848_kindOf(up, t1611, t1612), "notValue"); }), 0);
  RT_RET(rt_b(rt_bool(rt_eq(v963_kind, rt_s(&S46)), "||") || rt_bool(rt_eq(v963_kind, rt_s(&S388)), "||")));
}

static wv s856_aexpr(struct fr183 *up, wv v964_e, wv v965_scope, wv v966_f) {
  rt_enter("aexpr");
  v965_scope = rt_bind(v965_scope, 0);
  v966_f = rt_bind(v966_f, 0);
  wv v967_kind = rt_bind(rt_get(&F_k, v964_e), 0);
  if (rt_cond(rt_eq(v967_kind, rt_s(&S171)))) {
    wv v968_k = rt_i(0);
    while (rt_cond(({ wv t1613 = v968_k; wv t1614 = std_length(rt_get(&F_parts, v964_e)); rt_lt(t1613, t1614); }))) {
      if (rt_cond(rt_ne(std_typeOf(({ wv t1615 = v968_k; wv t1616 = rt_get(&F_parts, v964_e); rt_idx(t1615, t1616); })), rt_s(&S185)))) {
        (void)({ wv t1619 = ({ wv t1617 = v968_k; wv t1618 = rt_get(&F_parts, v964_e); rt_idx(t1617, t1618); }); wv t1620 = v965_scope; wv t1621 = v966_f; rt_chk(s856_aexpr(up, t1619, t1620, t1621), "aexpr"); });
      }
      v968_k = rt_add(v968_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S211)))) {
    (void)rt_chk(s853_use(up, v964_e, v965_scope, v966_f), "aexpr");
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S186)))) {
    wv v969_k = rt_i(0);
    while (rt_cond(({ wv t1622 = v969_k; wv t1623 = std_length(rt_get(&F_items, v964_e)); rt_lt(t1622, t1623); }))) {
      (void)({ wv t1626 = ({ wv t1624 = v969_k; wv t1625 = rt_get(&F_items, v964_e); rt_idx(t1624, t1625); }); wv t1627 = v965_scope; wv t1628 = v966_f; rt_chk(s856_aexpr(up, t1626, t1627, t1628), "aexpr"); });
      v969_k = rt_add(v969_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S187)))) {
    wv v970_k = rt_i(0);
    while (rt_cond(({ wv t1629 = v970_k; wv t1630 = std_length(rt_get(&F_fields, v964_e)); rt_lt(t1629, t1630); }))) {
      (void)({ wv t1633 = rt_get(&F_e, ({ wv t1631 = v970_k; wv t1632 = rt_get(&F_fields, v964_e); rt_idx(t1631, t1632); })); wv t1634 = v965_scope; wv t1635 = v966_f; rt_chk(s856_aexpr(up, t1633, t1634, t1635), "aexpr"); });
      v970_k = rt_add(v970_k, rt_i(1));
    }
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S188)))) {
    (void)({ wv t1636 = rt_get(&F_index, v964_e); wv t1637 = v965_scope; wv t1638 = v966_f; rt_chk(s856_aexpr(up, t1636, t1637, t1638), "aexpr"); });
    (void)({ wv t1639 = rt_get(&F_arr, v964_e); wv t1640 = v965_scope; wv t1641 = v966_f; rt_chk(s856_aexpr(up, t1639, t1640, t1641), "aexpr"); });
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S189)))) {
    if (rt_cond(rt_not(({ wv t1642 = rt_get(&F_rec, v964_e); wv t1643 = v965_scope; rt_chk(s855_notValue(up, t1642, t1643), "aexpr"); })))) {
      (void)({ wv t1644 = rt_get(&F_rec, v964_e); wv t1645 = v965_scope; wv t1646 = v966_f; rt_chk(s856_aexpr(up, t1644, t1645, t1646), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S190)))) {
    (void)({ wv t1647 = rt_get(&F_e, v964_e); wv t1648 = v965_scope; wv t1649 = v966_f; rt_chk(s856_aexpr(up, t1647, t1648, t1649), "aexpr"); });
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S191)))) {
    (void)({ wv t1650 = rt_get(&F_l, v964_e); wv t1651 = v965_scope; wv t1652 = v966_f; rt_chk(s856_aexpr(up, t1650, t1651, t1652), "aexpr"); });
    (void)({ wv t1653 = rt_get(&F_r, v964_e); wv t1654 = v965_scope; wv t1655 = v966_f; rt_chk(s856_aexpr(up, t1653, t1654, t1655), "aexpr"); });
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S192)))) {
    (void)({ wv t1656 = rt_get(&F_c, v964_e); wv t1657 = v965_scope; wv t1658 = v966_f; rt_chk(s856_aexpr(up, t1656, t1657, t1658), "aexpr"); });
    (void)({ wv t1659 = rt_get(&F_a, v964_e); wv t1660 = v965_scope; wv t1661 = v966_f; rt_chk(s856_aexpr(up, t1659, t1660, t1661), "aexpr"); });
    (void)({ wv t1662 = rt_get(&F_b, v964_e); wv t1663 = v965_scope; wv t1664 = v966_f; rt_chk(s856_aexpr(up, t1662, t1663, t1664), "aexpr"); });
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S182)))) {
    wv v971_k = rt_i(0);
    while (rt_cond(({ wv t1665 = v971_k; wv t1666 = std_length(rt_get(&F_args, v964_e)); rt_lt(t1665, t1666); }))) {
      (void)({ wv t1669 = ({ wv t1667 = v971_k; wv t1668 = rt_get(&F_args, v964_e); rt_idx(t1667, t1668); }); wv t1670 = v965_scope; wv t1671 = v966_f; rt_chk(s856_aexpr(up, t1669, t1670, t1671), "aexpr"); });
      v971_k = rt_add(v971_k, rt_i(1));
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_callee, v964_e)), rt_s(&S211)))) {
      ({ wv t1674 = rt_get(&F_d, ({ wv t1672 = v965_scope; wv t1673 = rt_get(&F_name, rt_get(&F_callee, v964_e)); rt_chk(s847_find(up, t1672, t1673), "aexpr"); })); wv t1675 = rt_get(&F_pos, rt_get(&F_callee, v964_e)); wv t1676 = rt_def(up->v839_curSlot, "curSlot"); rt_set_idx(t1674, t1675, t1676); });
    }
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S184)))) {
    wv v972_k = rt_i(0);
    while (rt_cond(({ wv t1677 = v972_k; wv t1678 = std_length(rt_get(&F_branches, v964_e)); rt_lt(t1677, t1678); }))) {
      (void)({ wv t1681 = rt_get(&F_cond, ({ wv t1679 = v972_k; wv t1680 = rt_get(&F_branches, v964_e); rt_idx(t1679, t1680); })); wv t1682 = v965_scope; wv t1683 = v966_f; rt_chk(s856_aexpr(up, t1681, t1682, t1683), "aexpr"); });
      (void)({ wv t1686 = rt_get(&F_body, ({ wv t1684 = v972_k; wv t1685 = rt_get(&F_branches, v964_e); rt_idx(t1684, t1685); })); wv t1687 = rt_chk(s845_newScope(up, v965_scope), "aexpr"); wv t1688 = v966_f; rt_chk(s851_ablock(up, t1686, t1687, t1688, RT_NULL), "aexpr"); });
      v972_k = rt_add(v972_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v964_e), RT_NULL))) {
      (void)({ wv t1689 = rt_get(&F_els, v964_e); wv t1690 = rt_chk(s845_newScope(up, v965_scope), "aexpr"); wv t1691 = v966_f; rt_chk(s851_ablock(up, t1689, t1690, t1691, RT_NULL), "aexpr"); });
    }
  } else if (rt_cond(rt_eq(v967_kind, rt_s(&S44)))) {
    (void)({ wv t1692 = rt_get(&F_subject, v964_e); wv t1693 = v965_scope; wv t1694 = v966_f; rt_chk(s856_aexpr(up, t1692, t1693, t1694), "aexpr"); });
    wv v973_k = rt_i(0);
    while (rt_cond(({ wv t1695 = v973_k; wv t1696 = std_length(rt_get(&F_cases, v964_e)); rt_lt(t1695, t1696); }))) {
      (void)({ wv t1699 = rt_get(&F_body, ({ wv t1697 = v973_k; wv t1698 = rt_get(&F_cases, v964_e); rt_idx(t1697, t1698); })); wv t1700 = rt_chk(s845_newScope(up, v965_scope), "aexpr"); wv t1701 = v966_f; rt_chk(s851_ablock(up, t1699, t1700, t1701, RT_NULL), "aexpr"); });
      v973_k = rt_add(v973_k, rt_i(1));
    }
    if (rt_cond(rt_ne(rt_get(&F_els, v964_e), RT_NULL))) {
      (void)({ wv t1702 = rt_get(&F_els, v964_e); wv t1703 = rt_chk(s845_newScope(up, v965_scope), "aexpr"); wv t1704 = v966_f; rt_chk(s851_ablock(up, t1702, t1703, t1704, RT_NULL), "aexpr"); });
    }
  }
  RT_RET(RT_NULL);
}

static wv s857_analyze(struct fr183 *up, wv v974_m) {
  rt_enter("analyze");
  v974_m = rt_bind(v974_m, 0);
  wv t1705 = rt_bind(v974_m, 1);
  (void)rt_def(up->v837_curMod, "curMod");
  up->v837_curMod = t1705;
  wv t1706 = rt_bind(rt_chk(s843_modId(up, v974_m), "analyze"), 1);
  (void)rt_def(up->v838_mod, "mod");
  up->v838_mod = t1706;
  wv v975_top = rt_bind(rt_chk(s849_newFn(up, RT_NULL, RT_NULL), "analyze"), 0);
  (void)({ wv t1707 = rt_def(up->v835_tops, "tops"); wv t1708 = v975_top; std_push(t1707, t1708); });
  wv v976_slot = rt_arr(0, NULL, 1);
  wv v977_i = rt_i(0);
  while (rt_cond(({ wv t1709 = v977_i; wv t1710 = std_length(rt_get(&F_chars, rt_idx(v974_m, up->v818_modules))); rt_le(t1709, t1710); }))) {
    (void)std_push(v976_slot, RT_NULL);
    v977_i = rt_add(v977_i, rt_i(1));
  }
  (void)({ wv t1711 = rt_def(up->v836_slots, "slots"); wv t1712 = v976_slot; std_push(t1711, t1712); });
  wv t1713 = rt_bind(v976_slot, 1);
  (void)rt_def(up->v839_curSlot, "curSlot");
  up->v839_curSlot = t1713;
  wv v978_scope = rt_bind(rt_chk(s845_newScope(up, RT_NULL), "analyze"), 0);
  wv v979_imports = rt_bind(rt_get(&F_imports, rt_idx(v974_m, up->v818_modules)), 0);
  wv v980_k = rt_i(0);
  while (rt_cond(({ wv t1714 = v980_k; wv t1715 = std_length(v979_imports); rt_lt(t1714, t1715); }))) {
    (void)({ wv t1716 = v978_scope; wv t1717 = rt_get(&F_name, rt_get(&F_node, rt_idx(v980_k, v979_imports))); rt_chk(s846_put(up, t1716, t1717, rt_s(&S388), RT_NULL), "analyze"); });
    v980_k = rt_add(v980_k, rt_i(1));
  }
  (void)({ wv t1718 = rt_rec(0, 2, K48, (wv[]){ rt_get(&F_stmts, rt_get(&F_program, rt_idx(v974_m, up->v818_modules))), RT_NULL }); wv t1719 = v978_scope; wv t1720 = v975_top; wv t1721 = v978_scope; rt_chk(s851_ablock(up, t1718, t1719, t1720, t1721), "analyze"); });
  RT_RET(RT_NULL);
}

static wv s858_enter(struct fr183 *up, wv v981_m) {
  rt_enter("enter");
  v981_m = rt_bind(v981_m, 0);
  (void)({ wv t1722 = rt_get(&F_file, rt_idx(v981_m, up->v818_modules)); wv t1723 = rt_get(&F_chars, rt_idx(v981_m, up->v818_modules)); rt_chk(w2_setSource(t1722, t1723), "enter"); });
  wv t1724 = rt_bind(v981_m, 1);
  (void)rt_def(up->v837_curMod, "curMod");
  up->v837_curMod = t1724;
  wv t1725 = rt_bind(rt_chk(s843_modId(up, v981_m), "enter"), 1);
  (void)rt_def(up->v838_mod, "mod");
  up->v838_mod = t1725;
  wv t1728 = rt_bind(({ wv t1726 = v981_m; wv t1727 = rt_def(up->v836_slots, "slots"); rt_idx(t1726, t1727); }), 1);
  (void)rt_def(up->v839_curSlot, "curSlot");
  up->v839_curSlot = t1728;
  wv t1731 = rt_bind(({ wv t1729 = v981_m; wv t1730 = rt_def(up->v835_tops, "tops"); rt_idx(t1729, t1730); }), 1);
  (void)rt_def(up->v842_cur, "cur");
  up->v842_cur = t1731;
  wv t1732 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v840_stolen, "stolen");
  up->v840_stolen = t1732;
  wv t1733 = rt_arr(0, NULL, 1);
  (void)rt_def(up->v841_ownEnums, "ownEnums");
  up->v841_ownEnums = t1733;
  wv v982_list = rt_bind(rt_get(&F_stmts, rt_get(&F_program, rt_idx(v981_m, up->v818_modules))), 0);
  wv v983_k = rt_i(0);
  while (rt_cond(({ wv t1734 = v983_k; wv t1735 = std_length(v982_list); rt_lt(t1734, t1735); }))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_idx(v983_k, v982_list)), rt_s(&S46)))) {
      (void)({ wv t1736 = rt_def(up->v841_ownEnums, "ownEnums"); wv t1737 = rt_idx(v983_k, v982_list); std_push(t1736, t1737); });
    }
    v983_k = rt_add(v983_k, rt_i(1));
  }
  wv v984_imports = rt_bind(rt_get(&F_imports, rt_idx(v981_m, up->v818_modules)), 0);
  v983_k = rt_i(0);
  while (rt_cond(({ wv t1738 = v983_k; wv t1739 = std_length(v984_imports); rt_lt(t1738, t1739); }))) {
    (void)({ wv t1742 = rt_def(up->v840_stolen, "stolen"); wv t1743 = ({ wv t1740 = rt_get(&F_name, rt_get(&F_node, rt_idx(v983_k, v984_imports))); wv t1741 = rt_get(&F_lib, rt_idx(v983_k, v984_imports)); rt_rec(0, 2, K49, (wv[]){ t1740, t1741 }); }); std_push(t1742, t1743); });
    v983_k = rt_add(v983_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s859_emit(struct fr183 *up, wv v985_line) {
  rt_enter("emit");
  v985_line = rt_bind(v985_line, 0);
  (void)({ wv t1746 = rt_def(up->v829_out, "out"); wv t1747 = ({ wv t1744 = rt_chk(w7_repeat(rt_s(&S480), rt_def(up->v830_indent, "indent")), "emit"); wv t1745 = v985_line; rt_add(t1744, t1745); }); std_push(t1746, t1747); });
  RT_RET(RT_NULL);
}

static wv s860_tmp(struct fr183 *up) {
  rt_enter("tmp");
  wv v986_name = rt_cat(2, (wv[]){ rt_s(&S104), rt_text(rt_def(up->v821_tmps, "tmps")) });
  up->v821_tmps = rt_add(rt_def(up->v821_tmps, "tmps"), rt_i(1));
  RT_RET(v986_name);
}

static wv s861_str(struct fr183 *up, wv v987_s) {
  rt_enter("str");
  v987_s = rt_bind(v987_s, 0);
  wv v988_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1748 = v988_k; wv t1749 = std_length(rt_def(up->v822_strings, "strings")); rt_lt(t1748, t1749); }), "&&") && rt_bool(({ wv t1752 = ({ wv t1750 = v988_k; wv t1751 = rt_def(up->v822_strings, "strings"); rt_idx(t1750, t1751); }); wv t1753 = v987_s; rt_ne(t1752, t1753); }), "&&")))) {
    v988_k = rt_add(v988_k, rt_i(1));
  }
  if (rt_cond(({ wv t1754 = v988_k; wv t1755 = std_length(rt_def(up->v822_strings, "strings")); rt_eq(t1754, t1755); }))) {
    (void)({ wv t1756 = rt_def(up->v822_strings, "strings"); wv t1757 = v987_s; std_push(t1756, t1757); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S574), rt_text(v988_k) }));
}

static wv s862_fieldRef(struct fr183 *up, wv v989_name) {
  rt_enter("fieldRef");
  v989_name = rt_bind(v989_name, 0);
  if (rt_cond(rt_not(({ wv t1758 = rt_def(up->v823_fields, "fields"); wv t1759 = v989_name; rt_chk(w1_contains(t1758, t1759), "fieldRef"); })))) {
    (void)({ wv t1760 = rt_def(up->v823_fields, "fields"); wv t1761 = v989_name; std_push(t1760, t1761); });
  }
  RT_RET(rt_cat(2, (wv[]){ rt_s(&S575), rt_text(v989_name) }));
}

static wv s863_enumDef(struct fr183 *up, wv v990_s, wv v991_m) {
  rt_enter("enumDef");
  v990_s = rt_bind(v990_s, 0);
  v991_m = rt_bind(v991_m, 0);
  wv v992_nm = rt_bind(rt_chk(s861_str(up, rt_get(&F_name, v990_s)), "enumDef"), 0);
  wv v993_members = rt_arr(0, NULL, 1);
  wv v994_k = rt_i(0);
  while (rt_cond(({ wv t1762 = v994_k; wv t1763 = std_length(rt_get(&F_members, v990_s)); rt_lt(t1762, t1763); }))) {
    (void)({ wv t1768 = v993_members; wv t1769 = ({ wv t1766 = rt_text(v992_nm); wv t1767 = rt_text(rt_chk(s861_str(up, rt_get(&F_name, ({ wv t1764 = v994_k; wv t1765 = rt_get(&F_members, v990_s); rt_idx(t1764, t1765); }))), "enumDef")); rt_cat(5, (wv[]){ rt_s(&S568), t1766, rt_s(&S385), t1767, rt_s(&S570) }); }); std_push(t1768, t1769); });
    v994_k = rt_add(v994_k, rt_i(1));
  }
  (void)({ wv t1773 = rt_def(up->v825_enumDefs, "enumDefs"); wv t1774 = ({ wv t1770 = rt_text(v991_m); wv t1771 = rt_text(rt_get(&F_name, v990_s)); wv t1772 = rt_text(std_join(v993_members, rt_s(&S385))); rt_cat(7, (wv[]){ rt_s(&S576), t1770, rt_s(&S12), t1771, rt_s(&S577), t1772, rt_s(&S578) }); }); std_push(t1773, t1774); });
  RT_RET(RT_NULL);
}

static wv s864_stolenFrom(struct fr183 *up, wv v995_x) {
  rt_enter("stolenFrom");
  if (rt_cond(rt_ne(rt_get(&F_k, v995_x), rt_s(&S211)))) {
    RT_RET(RT_NULL);
  }
  wv v996_k = rt_i(0);
  while (rt_cond(({ wv t1775 = v996_k; wv t1776 = std_length(rt_def(up->v840_stolen, "stolen")); rt_lt(t1775, t1776); }))) {
    if (rt_cond(({ wv t1779 = rt_get(&F_name, ({ wv t1777 = v996_k; wv t1778 = rt_def(up->v840_stolen, "stolen"); rt_idx(t1777, t1778); })); wv t1780 = rt_get(&F_name, v995_x); rt_eq(t1779, t1780); }))) {
      RT_RET(rt_get(&F_lib, ({ wv t1781 = v996_k; wv t1782 = rt_def(up->v840_stolen, "stolen"); rt_idx(t1781, t1782); })));
    }
    v996_k = rt_add(v996_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s865_enumIn(struct fr183 *up, wv v997_list, wv v998_name) {
  rt_enter("enumIn");
  v998_name = rt_bind(v998_name, 0);
  wv v999_k = rt_i(0);
  while (rt_cond(({ wv t1783 = v999_k; wv t1784 = std_length(v997_list); rt_lt(t1783, t1784); }))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_k, rt_idx(v999_k, v997_list)), rt_s(&S46)), "&&") && rt_bool(({ wv t1785 = rt_get(&F_name, rt_idx(v999_k, v997_list)); wv t1786 = v998_name; rt_eq(t1785, t1786); }), "&&")))) {
      RT_RET(rt_idx(v999_k, v997_list));
    }
    v999_k = rt_add(v999_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s866_enumOf(struct fr183 *up, wv v1000_x) {
  rt_enter("enumOf");
  if (rt_cond(rt_eq(rt_get(&F_k, v1000_x), rt_s(&S211)))) {
    wv v1004_s = rt_bind(({ wv t1787 = rt_def(up->v841_ownEnums, "ownEnums"); wv t1788 = rt_get(&F_name, v1000_x); rt_chk(s865_enumIn(up, t1787, t1788), "enumOf"); }), 0);
    RT_RET((rt_cond(rt_eq(v1004_s, RT_NULL)) ? RT_NULL : ({ wv t1791 = ({ wv t1789 = rt_text(rt_def(up->v838_mod, "mod")); wv t1790 = rt_text(rt_get(&F_name, v1000_x)); rt_cat(4, (wv[]){ rt_s(&S479), t1789, rt_s(&S12), t1790 }); }); wv t1792 = v1004_s; rt_rec(0, 2, K54, (wv[]){ t1791, t1792 }); })));
  }
  if (rt_cond(rt_ne(rt_get(&F_k, v1000_x), rt_s(&S189)))) {
    RT_RET(RT_NULL);
  }
  wv v1001_lib = rt_bind(rt_chk(s864_stolenFrom(up, rt_get(&F_rec, v1000_x)), "enumOf"), 0);
  if (rt_cond(rt_eq(v1001_lib, RT_NULL))) {
    RT_RET(RT_NULL);
  }
  wv v1002_xs = rt_bind(rt_get(&F_exports, rt_idx(v1001_lib, up->v818_modules)), 0);
  wv v1003_k = rt_i(0);
  while (rt_cond(rt_b(rt_bool(({ wv t1793 = v1003_k; wv t1794 = std_length(v1002_xs); rt_lt(t1793, t1794); }), "&&") && rt_bool(({ wv t1795 = rt_get(&F_name, rt_idx(v1003_k, v1002_xs)); wv t1796 = rt_get(&F_field, v1000_x); rt_ne(t1795, t1796); }), "&&")))) {
    v1003_k = rt_add(v1003_k, rt_i(1));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1797 = v1003_k; wv t1798 = std_length(v1002_xs); rt_eq(t1797, t1798); }), "||") || rt_bool(rt_ne(rt_get(&F_kind, rt_idx(v1003_k, v1002_xs)), rt_s(&S46)), "||")))) {
    RT_RET(RT_NULL);
  }
  RT_RET(({ wv t1803 = ({ wv t1799 = rt_text(rt_chk(s843_modId(up, v1001_lib), "enumOf")); wv t1800 = rt_text(rt_get(&F_field, v1000_x)); rt_cat(4, (wv[]){ rt_s(&S479), t1799, rt_s(&S12), t1800 }); }); wv t1804 = ({ wv t1801 = rt_get(&F_stmts, rt_get(&F_program, rt_idx(v1001_lib, up->v818_modules))); wv t1802 = rt_get(&F_field, v1000_x); rt_chk(s865_enumIn(up, t1801, t1802), "enumOf"); }); rt_rec(0, 2, K54, (wv[]){ t1803, t1804 }); }));
}

static wv s867_place(struct fr183 *up, wv v1005_d) {
  rt_enter("place");
  v1005_d = rt_bind(v1005_d, 0);
  wv v1006_owner = rt_bind(rt_get(&F_func, ({ wv t1805 = v1005_d; wv t1806 = rt_def(up->v833_decls, "decls"); rt_idx(t1805, t1806); })), 0);
  if (rt_cond(rt_get(&F_top, ({ wv t1807 = v1006_owner; wv t1808 = rt_def(up->v834_fns, "fns"); rt_idx(t1807, t1808); })))) {
    RT_RET(rt_get(&F_cname, ({ wv t1809 = v1005_d; wv t1810 = rt_def(up->v833_decls, "decls"); rt_idx(t1809, t1810); })));
  }
  if (rt_cond(({ wv t1811 = v1006_owner; wv t1812 = rt_def(up->v842_cur, "cur"); rt_eq(t1811, t1812); }))) {
    RT_RET((rt_cond(rt_get(&F_captured, ({ wv t1813 = v1005_d; wv t1814 = rt_def(up->v833_decls, "decls"); rt_idx(t1813, t1814); }))) ? rt_cat(2, (wv[]){ rt_s(&S579), rt_text(rt_get(&F_cname, ({ wv t1815 = v1005_d; wv t1816 = rt_def(up->v833_decls, "decls"); rt_idx(t1815, t1816); }))) }) : rt_get(&F_cname, ({ wv t1817 = v1005_d; wv t1818 = rt_def(up->v833_decls, "decls"); rt_idx(t1817, t1818); }))));
  }
  RT_RET(({ wv t1827 = rt_text(rt_chk(w7_repeat(rt_s(&S581), rt_sub(({ wv t1823 = rt_get(&F_depth, ({ wv t1819 = rt_def(up->v842_cur, "cur"); wv t1820 = rt_def(up->v834_fns, "fns"); rt_idx(t1819, t1820); })); wv t1824 = rt_get(&F_depth, ({ wv t1821 = v1006_owner; wv t1822 = rt_def(up->v834_fns, "fns"); rt_idx(t1821, t1822); })); rt_sub(t1823, t1824); }), rt_i(1))), "place")); wv t1828 = rt_text(rt_get(&F_cname, ({ wv t1825 = v1005_d; wv t1826 = rt_def(up->v833_decls, "decls"); rt_idx(t1825, t1826); }))); rt_cat(4, (wv[]){ rt_s(&S580), t1827, rt_s(&S582), t1828 }); }));
}

static wv s868_tdz(struct fr183 *up, wv v1007_d) {
  rt_enter("tdz");
  v1007_d = rt_bind(v1007_d, 0);
  RT_RET(rt_b(rt_bool(rt_eq(rt_get(&F_kind, ({ wv t1829 = v1007_d; wv t1830 = rt_def(up->v833_decls, "decls"); rt_idx(t1829, t1830); })), rt_s(&S177)), "&&") && rt_bool(({ wv t1833 = rt_get(&F_func, ({ wv t1831 = v1007_d; wv t1832 = rt_def(up->v833_decls, "decls"); rt_idx(t1831, t1832); })); wv t1834 = rt_def(up->v842_cur, "cur"); rt_ne(t1833, t1834); }), "&&")));
}

static wv s869_read(struct fr183 *up, wv v1008_d) {
  rt_enter("read");
  v1008_d = rt_bind(v1008_d, 0);
  RT_RET((rt_cond(rt_chk(s868_tdz(up, v1008_d), "read")) ? ({ wv t1839 = ({ wv t1837 = rt_text(rt_chk(s867_place(up, v1008_d), "read")); wv t1838 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1835 = v1008_d; wv t1836 = rt_def(up->v833_decls, "decls"); rt_idx(t1835, t1836); }))), "read")); rt_cat(5, (wv[]){ rt_s(&S583), t1837, rt_s(&S385), t1838, rt_s(&S16) }); }); wv t1840 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t1839, t1840 }); }) : ({ wv t1841 = rt_chk(s867_place(up, v1008_d), "read"); wv t1842 = rt_def(w7_pureK, "pureK"); rt_rec(0, 2, K55, (wv[]){ t1841, t1842 }); })));
}

static wv s870_seq(struct fr183 *up, wv v1009_args) {
  rt_enter("seq");
  v1009_args = rt_bind(v1009_args, 0);
  wv v1010_effects = rt_i(0);
  wv v1011_anyPure = rt_b(0);
  wv v1012_k = rt_i(0);
  while (rt_cond(({ wv t1843 = v1012_k; wv t1844 = std_length(v1009_args); rt_lt(t1843, t1844); }))) {
    if (rt_cond(({ wv t1845 = rt_get(&F_k, rt_idx(v1012_k, v1009_args)); wv t1846 = rt_def(w7_effectK, "effectK"); rt_eq(t1845, t1846); }))) {
      v1010_effects = rt_add(v1010_effects, rt_i(1));
    }
    if (rt_cond(({ wv t1847 = rt_get(&F_k, rt_idx(v1012_k, v1009_args)); wv t1848 = rt_def(w7_pureK, "pureK"); rt_eq(t1847, t1848); }))) {
      v1011_anyPure = rt_b(1);
    }
    v1012_k = rt_add(v1012_k, rt_i(1));
  }
  wv v1013_cs = rt_arr(0, NULL, 1);
  if (rt_cond(rt_b(rt_bool(rt_eq(v1010_effects, rt_i(0)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1010_effects, rt_i(1)), "&&") && rt_bool(rt_not(v1011_anyPure), "&&")), "||")))) {
    v1012_k = rt_i(0);
    while (rt_cond(({ wv t1849 = v1012_k; wv t1850 = std_length(v1009_args); rt_lt(t1849, t1850); }))) {
      (void)({ wv t1851 = v1013_cs; wv t1852 = rt_get(&F_c, rt_idx(v1012_k, v1009_args)); std_push(t1851, t1852); });
      v1012_k = rt_add(v1012_k, rt_i(1));
    }
    RT_RET(rt_rec(0, 2, K56, (wv[]){ rt_s(&S13), v1013_cs }));
  }
  wv v1014_pre = rt_arr(0, NULL, 1);
  v1012_k = rt_i(0);
  while (rt_cond(({ wv t1853 = v1012_k; wv t1854 = std_length(v1009_args); rt_lt(t1853, t1854); }))) {
    if (rt_cond(({ wv t1855 = rt_get(&F_k, rt_idx(v1012_k, v1009_args)); wv t1856 = rt_def(w7_constK, "constK"); rt_eq(t1855, t1856); }))) {
      (void)({ wv t1857 = v1013_cs; wv t1858 = rt_get(&F_c, rt_idx(v1012_k, v1009_args)); std_push(t1857, t1858); });
    } else {
      wv v1015_temp = rt_bind(rt_chk(s860_tmp(up), "seq"), 0);
      (void)({ wv t1861 = v1014_pre; wv t1862 = ({ wv t1859 = rt_text(v1015_temp); wv t1860 = rt_text(rt_get(&F_c, rt_idx(v1012_k, v1009_args))); rt_cat(5, (wv[]){ rt_s(&S584), t1859, rt_s(&S495), t1860, rt_s(&S14) }); }); std_push(t1861, t1862); });
      (void)std_push(v1013_cs, v1015_temp);
    }
    v1012_k = rt_add(v1012_k, rt_i(1));
  }
  RT_RET(({ wv t1863 = std_join(v1014_pre, rt_s(&S0)); wv t1864 = v1013_cs; rt_rec(0, 2, K56, (wv[]){ t1863, t1864 }); }));
}

static wv s871_wrap(struct fr183 *up, wv v1016_sq, wv v1017_inner) {
  rt_enter("wrap");
  v1016_sq = rt_bind(v1016_sq, 0);
  v1017_inner = rt_bind(v1017_inner, 0);
  RT_RET(({ wv t1867 = (rt_cond(rt_eq(rt_get(&F_pre, v1016_sq), rt_s(&S13))) ? v1017_inner : ({ wv t1865 = rt_text(rt_get(&F_pre, v1016_sq)); wv t1866 = rt_text(v1017_inner); rt_cat(5, (wv[]){ rt_s(&S585), t1865, rt_s(&S0), t1866, rt_s(&S586) }); })); wv t1868 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t1867, t1868 }); }));
}

static wv s872_stmts(struct fr183 *up, wv v1018_list) {
  rt_enter("stmts");
  v1018_list = rt_bind(v1018_list, 0);
  wv v1019_k = rt_i(0);
  while (rt_cond(({ wv t1869 = v1019_k; wv t1870 = std_length(v1018_list); rt_lt(t1869, t1870); }))) {
    (void)rt_chk(s876_stmt(up, rt_idx(v1019_k, v1018_list)), "stmts");
    v1019_k = rt_add(v1019_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s873_resets(struct fr183 *up, wv v1020_b) {
  rt_enter("resets");
  wv v1021_k = rt_i(0);
  while (rt_cond(({ wv t1871 = v1021_k; wv t1872 = std_length(rt_get(&F_stmts, v1020_b)); rt_lt(t1871, t1872); }))) {
    wv v1022_s = ({ wv t1873 = v1021_k; wv t1874 = rt_get(&F_stmts, v1020_b); rt_idx(t1873, t1874); });
    if (rt_cond(rt_eq(rt_get(&F_k, v1022_s), rt_s(&S177)))) {
      wv v1023_d = rt_bind(({ wv t1875 = rt_get(&F_namePos, v1022_s); wv t1876 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t1875, t1876); }), 0);
      if (rt_cond(rt_get(&F_captured, ({ wv t1877 = v1023_d; wv t1878 = rt_def(up->v833_decls, "decls"); rt_idx(t1877, t1878); })))) {
        (void)rt_chk(s859_emit(up, rt_cat(2, (wv[]){ rt_text(rt_chk(s867_place(up, v1023_d), "resets")), rt_s(&S587) })), "resets");
      }
    }
    v1021_k = rt_add(v1021_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s874_blockBody(struct fr183 *up, wv v1024_b) {
  rt_enter("blockBody");
  v1024_b = rt_bind(v1024_b, 0);
  (void)rt_chk(s873_resets(up, v1024_b), "blockBody");
  (void)rt_chk(s872_stmts(up, rt_get(&F_stmts, v1024_b)), "blockBody");
  if (rt_cond(rt_ne(rt_get(&F_value, v1024_b), RT_NULL))) {
    (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S588), rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_value, v1024_b)), "blockBody"))), rt_s(&S14) })), "blockBody");
  }
  RT_RET(RT_NULL);
}

static wv s875_valueOf(struct fr183 *up, wv v1025_b) {
  rt_enter("valueOf");
  v1025_b = rt_bind(v1025_b, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_value, v1025_b), RT_NULL)) ? rt_s(&S589) : rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_value, v1025_b)), "valueOf"))));
}

static wv s876_stmt(struct fr183 *up, wv v1026_s) {
  rt_enter("stmt");
  if (rt_cond(rt_ne(rt_get(&F_handler, v1026_s), RT_NULL))) {
    (void)rt_chk(s885_onfailStmt(up, v1026_s), "stmt");
    RT_RET(RT_NULL);
  }
  wv v1027_kind = rt_bind(rt_get(&F_k, v1026_s), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1027_kind, rt_s(&S200)), "||") || rt_bool(rt_eq(v1027_kind, rt_s(&S46)), "||")), "||") || rt_bool(rt_eq(v1027_kind, rt_s(&S47)), "||")), "||") || rt_bool(rt_eq(v1027_kind, rt_s(&S203)), "||")))) {
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S33)))) {
    (void)rt_chk(s879_fnDef(up, v1026_s), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S34)))) {
    if (rt_cond(rt_def(up->v832_iife, "iife"))) {
      (void)rt_chk(w2_abort(rt_get(&F_pos, v1026_s), rt_s(&S482)), "stmt");
    }
    (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S590), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1026_s), RT_NULL)) ? rt_s(&S589) : rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_e, v1026_s)), "stmt")))), rt_s(&S485) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S57)))) {
    (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S591), rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_e, v1026_s)), "stmt"))), rt_s(&S485) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_b(rt_bool(rt_eq(v1027_kind, rt_s(&S42)), "||") || rt_bool(rt_eq(v1027_kind, rt_s(&S43)), "||")))) {
    if (rt_cond(rt_eq(rt_def(up->v831_loops, "loops"), rt_i(0)))) {
      (void)({ wv t1879 = rt_get(&F_pos, v1026_s); wv t1880 = rt_cat(3, (wv[]){ rt_s(&S113), rt_text((rt_cond(rt_eq(v1027_kind, rt_s(&S42))) ? rt_s(&S42) : rt_s(&S43))), rt_s(&S486) }); rt_chk(w2_abort(t1879, t1880), "stmt"); });
    }
    (void)rt_chk(s859_emit(up, (rt_cond(rt_eq(v1027_kind, rt_s(&S42))) ? rt_s(&S487) : rt_s(&S488))), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S41)))) {
    (void)rt_chk(s859_emit(up, ({ wv t1881 = rt_text((rt_cond(rt_get(&F_neg, v1026_s)) ? rt_s(&S94) : rt_s(&S13))); wv t1882 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_cond, v1026_s)), "stmt"))); rt_cat(5, (wv[]){ rt_s(&S489), t1881, rt_s(&S592), t1882, rt_s(&S491) }); })), "stmt");
    up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
    up->v831_loops = rt_add(rt_def(up->v831_loops, "loops"), rt_i(1));
    (void)rt_chk(s874_blockBody(up, rt_get(&F_body, v1026_s)), "stmt");
    up->v831_loops = rt_sub(rt_def(up->v831_loops, "loops"), rt_i(1));
    up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
    (void)rt_chk(s859_emit(up, rt_s(&S15)), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S55)))) {
    (void)rt_chk(s877_signal(up, rt_cat(3, (wv[]){ rt_s(&S593), rt_text(rt_get(&F_code, v1026_s)), rt_s(&S16) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S54)))) {
    (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S594), rt_text((rt_cond(rt_eq(rt_get(&F_e, v1026_s), RT_NULL)) ? rt_s(&S595) : rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_e, v1026_s)), "stmt")))), rt_s(&S485) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S176)))) {
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1026_s)), rt_s(&S184)))) {
      (void)rt_chk(s886_chainStmt(up, rt_get(&F_e, v1026_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1026_s)), rt_s(&S44)))) {
      (void)rt_chk(s897_matchStmt(up, rt_get(&F_e, v1026_s)), "stmt");
      RT_RET(RT_NULL);
    }
    if (rt_cond(rt_eq(rt_get(&F_k, rt_get(&F_e, v1026_s)), rt_s(&S183)))) {
      (void)rt_chk(s877_signal(up, rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_arg, rt_get(&F_e, v1026_s))), "stmt"))), "stmt");
      RT_RET(RT_NULL);
    }
    (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S588), rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_e, v1026_s)), "stmt"))), rt_s(&S14) })), "stmt");
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S177)))) {
    (void)({ wv t1887 = ({ wv t1883 = rt_get(&F_namePos, v1026_s); wv t1884 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t1883, t1884); }); wv t1888 = ({ wv t1885 = rt_get(&F_e, v1026_s); wv t1886 = rt_chk(w7_modeOf(v1026_s), "stmt"); rt_chk(s880_bindRhs(up, t1885, t1886), "stmt"); }); rt_chk(s878_bindDecl(up, t1887, t1888), "stmt"); });
    RT_RET(RT_NULL);
  }
  if (rt_cond(rt_eq(v1027_kind, rt_s(&S38)))) {
    (void)rt_chk(s881_setStmt(up, v1026_s), "stmt");
    RT_RET(RT_NULL);
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S496), rt_text(v1027_kind) }));
  RT_RET(RT_NULL);
}

static wv s877_signal(struct fr183 *up, wv v1028_code) {
  rt_enter("signal");
  v1028_code = rt_bind(v1028_code, 0);
  (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S596), rt_text(v1028_code), rt_s(&S485) })), "signal");
  (void)rt_chk(s859_emit(up, rt_s(&S597)), "signal");
  RT_RET(RT_NULL);
}

static wv s878_bindDecl(struct fr183 *up, wv v1029_d, wv v1030_value) {
  rt_enter("bindDecl");
  v1029_d = rt_bind(v1029_d, 0);
  v1030_value = rt_bind(v1030_value, 0);
  wv v1031_owner = rt_bind(rt_get(&F_func, ({ wv t1889 = v1029_d; wv t1890 = rt_def(up->v833_decls, "decls"); rt_idx(t1889, t1890); })), 0);
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_not(rt_get(&F_top, ({ wv t1891 = v1031_owner; wv t1892 = rt_def(up->v834_fns, "fns"); rt_idx(t1891, t1892); }))), "&&") && rt_bool(({ wv t1893 = v1031_owner; wv t1894 = rt_def(up->v842_cur, "cur"); rt_eq(t1893, t1894); }), "&&")), "&&") && rt_bool(rt_not(rt_get(&F_captured, ({ wv t1895 = v1029_d; wv t1896 = rt_def(up->v833_decls, "decls"); rt_idx(t1895, t1896); }))), "&&")))) {
    (void)rt_chk(s859_emit(up, ({ wv t1899 = rt_text(rt_get(&F_cname, ({ wv t1897 = v1029_d; wv t1898 = rt_def(up->v833_decls, "decls"); rt_idx(t1897, t1898); }))); wv t1900 = rt_text(v1030_value); rt_cat(5, (wv[]){ rt_s(&S584), t1899, rt_s(&S495), t1900, rt_s(&S14) }); })), "bindDecl");
  } else {
    (void)rt_chk(s859_emit(up, ({ wv t1901 = rt_text(rt_chk(s867_place(up, v1029_d), "bindDecl")); wv t1902 = rt_text(v1030_value); rt_cat(4, (wv[]){ t1901, rt_s(&S495), t1902, rt_s(&S14) }); })), "bindDecl");
  }
  RT_RET(RT_NULL);
}

static wv s879_fnDef(struct fr183 *up, wv v1032_s) {
  rt_enter("fnDef");
  v1032_s = rt_bind(v1032_s, 0);
  wv v1033_d = rt_bind(({ wv t1903 = rt_get(&F_namePos, v1032_s); wv t1904 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t1903, t1904); }), 0);
  wv v1034_f = rt_bind(rt_get(&F_fnIndex, ({ wv t1905 = v1033_d; wv t1906 = rt_def(up->v833_decls, "decls"); rt_idx(t1905, t1906); })), 0);
  wv v1035_pf = rt_bind(rt_get(&F_parent, ({ wv t1907 = v1034_f; wv t1908 = rt_def(up->v834_fns, "fns"); rt_idx(t1907, t1908); })), 0);
  wv v1036_hasUp = rt_not(rt_get(&F_top, ({ wv t1909 = v1035_pf; wv t1910 = rt_def(up->v834_fns, "fns"); rt_idx(t1909, t1910); })));
  wv v1037_params = rt_arr(0, NULL, 1);
  if (rt_cond(v1036_hasUp)) {
    (void)({ wv t1911 = v1037_params; wv t1912 = rt_cat(3, (wv[]){ rt_s(&S598), rt_text(v1035_pf), rt_s(&S599) }); std_push(t1911, t1912); });
  }
  wv v1038_k = rt_i(0);
  while (rt_cond(({ wv t1913 = v1038_k; wv t1914 = std_length(rt_get(&F_params, v1032_s)); rt_lt(t1913, t1914); }))) {
    wv v1046_pd = rt_bind(({ wv t1917 = rt_get(&F_pos, ({ wv t1915 = v1038_k; wv t1916 = rt_get(&F_params, v1032_s); rt_idx(t1915, t1916); })); wv t1918 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t1917, t1918); }), 0);
    (void)({ wv t1925 = v1037_params; wv t1926 = ({ wv t1923 = rt_text((rt_cond(rt_get(&F_captured, ({ wv t1919 = v1046_pd; wv t1920 = rt_def(up->v833_decls, "decls"); rt_idx(t1919, t1920); }))) ? rt_s(&S600) : rt_s(&S13))); wv t1924 = rt_text(rt_get(&F_cname, ({ wv t1921 = v1046_pd; wv t1922 = rt_def(up->v833_decls, "decls"); rt_idx(t1921, t1922); }))); rt_cat(3, (wv[]){ rt_s(&S584), t1923, t1924 }); }); std_push(t1925, t1926); });
    v1038_k = rt_add(v1038_k, rt_i(1));
  }
  wv v1039_proto = ({ wv t1929 = rt_text(rt_get(&F_cname, ({ wv t1927 = v1034_f; wv t1928 = rt_def(up->v834_fns, "fns"); rt_idx(t1927, t1928); }))); wv t1930 = rt_text((rt_cond(rt_eq(std_length(v1037_params), rt_i(0))) ? rt_s(&S602) : std_join(v1037_params, rt_s(&S385)))); rt_cat(5, (wv[]){ rt_s(&S601), t1929, rt_s(&S97), t1930, rt_s(&S16) }); });
  wv v1040_entry = ({ wv t1931 = v1039_proto; wv t1932 = rt_arr(0, NULL, 0); rt_rec(1, 2, K57, (wv[]){ t1931, t1932 }); });
  (void)({ wv t1933 = rt_def(up->v828_functions, "functions"); wv t1934 = v1040_entry; std_push(t1933, t1934); });
  if (rt_cond(rt_chk(s844_hasFrame(up, v1034_f), "fnDef"))) {
    (void)({ wv t1935 = rt_def(up->v827_frames, "frames"); wv t1936 = v1034_f; std_push(t1935, t1936); });
  }
  wv v1041_start = rt_bind(std_length(rt_def(up->v829_out, "out")), 0);
  wv v1042_savedIndent = rt_bind(rt_def(up->v830_indent, "indent"), 0);
  wv v1043_savedLoops = rt_bind(rt_def(up->v831_loops, "loops"), 0);
  wv v1044_savedIife = rt_bind(rt_def(up->v832_iife, "iife"), 0);
  wv v1045_savedCur = rt_bind(rt_def(up->v842_cur, "cur"), 0);
  wv t1937 = rt_i(1);
  (void)rt_def(up->v830_indent, "indent");
  up->v830_indent = t1937;
  wv t1938 = rt_i(0);
  (void)rt_def(up->v831_loops, "loops");
  up->v831_loops = t1938;
  wv t1939 = rt_b(0);
  (void)rt_def(up->v832_iife, "iife");
  up->v832_iife = t1939;
  wv t1940 = rt_bind(v1034_f, 1);
  (void)rt_def(up->v842_cur, "cur");
  up->v842_cur = t1940;
  (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S603), rt_text(rt_chk(w1_quote(rt_get(&F_name, v1032_s)), "fnDef")), rt_s(&S485) })), "fnDef");
  if (rt_cond(rt_chk(s844_hasFrame(up, v1034_f), "fnDef"))) {
    (void)rt_chk(s859_emit(up, ({ wv t1941 = rt_text(v1034_f); wv t1942 = rt_text((rt_cond(v1036_hasUp) ? rt_s(&S605) : rt_s(&S4))); rt_cat(5, (wv[]){ rt_s(&S598), t1941, rt_s(&S604), t1942, rt_s(&S578) }); })), "fnDef");
  }
  v1038_k = rt_i(0);
  while (rt_cond(({ wv t1943 = v1038_k; wv t1944 = std_length(rt_get(&F_params, v1032_s)); rt_lt(t1943, t1944); }))) {
    wv v1047_p = rt_bind(({ wv t1945 = v1038_k; wv t1946 = rt_get(&F_params, v1032_s); rt_idx(t1945, t1946); }), 0);
    wv v1048_pd = rt_bind(({ wv t1947 = rt_get(&F_pos, v1047_p); wv t1948 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t1947, t1948); }), 0);
    wv v1049_cn = rt_bind(rt_get(&F_cname, ({ wv t1949 = v1048_pd; wv t1950 = rt_def(up->v833_decls, "decls"); rt_idx(t1949, t1950); })), 0);
    wv v1050_arg = rt_bind((rt_cond(rt_get(&F_captured, ({ wv t1951 = v1048_pd; wv t1952 = rt_def(up->v833_decls, "decls"); rt_idx(t1951, t1952); }))) ? rt_cat(2, (wv[]){ rt_s(&S600), rt_text(v1049_cn) }) : v1049_cn), 0);
    wv v1051_v = rt_bind((rt_cond(rt_get(&F_isRef, v1047_p)) ? v1050_arg : ({ wv t1953 = rt_text(v1050_arg); wv t1954 = rt_text((rt_cond(rt_get(&F_isVar, v1047_p)) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S606), t1953, rt_s(&S385), t1954, rt_s(&S16) }); })), 0);
    if (rt_cond(rt_get(&F_captured, ({ wv t1955 = v1048_pd; wv t1956 = rt_def(up->v833_decls, "decls"); rt_idx(t1955, t1956); })))) {
      (void)rt_chk(s859_emit(up, ({ wv t1957 = rt_text(v1049_cn); wv t1958 = rt_text(v1051_v); rt_cat(5, (wv[]){ rt_s(&S579), t1957, rt_s(&S495), t1958, rt_s(&S14) }); })), "fnDef");
    } else if (rt_cond(rt_not(rt_get(&F_isRef, v1047_p)))) {
      (void)rt_chk(s859_emit(up, ({ wv t1959 = rt_text(v1049_cn); wv t1960 = rt_text(v1051_v); rt_cat(4, (wv[]){ t1959, rt_s(&S495), t1960, rt_s(&S14) }); })), "fnDef");
    }
    v1038_k = rt_add(v1038_k, rt_i(1));
  }
  (void)rt_chk(s872_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1032_s))), "fnDef");
  (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S590), rt_text(rt_chk(s875_valueOf(up, rt_get(&F_body, v1032_s)), "fnDef")), rt_s(&S485) })), "fnDef");
  ({ wv t1964 = ({ wv t1961 = rt_def(up->v829_out, "out"); wv t1962 = v1041_start; wv t1963 = std_length(rt_def(up->v829_out, "out")); std_slice(t1961, t1962, t1963); }); wv t1965 = v1040_entry; rt_set_mem(t1964, &F_lines, t1965); });
  while (rt_cond(({ wv t1966 = std_length(rt_def(up->v829_out, "out")); wv t1967 = v1041_start; rt_gt(t1966, t1967); }))) {
    (void)std_pop(rt_def(up->v829_out, "out"));
  }
  wv t1968 = rt_bind(v1042_savedIndent, 1);
  (void)rt_def(up->v830_indent, "indent");
  up->v830_indent = t1968;
  wv t1969 = rt_bind(v1043_savedLoops, 1);
  (void)rt_def(up->v831_loops, "loops");
  up->v831_loops = t1969;
  wv t1970 = rt_bind(v1044_savedIife, 1);
  (void)rt_def(up->v832_iife, "iife");
  up->v832_iife = t1970;
  wv t1971 = rt_bind(v1045_savedCur, 1);
  (void)rt_def(up->v842_cur, "cur");
  up->v842_cur = t1971;
  RT_RET(RT_NULL);
}

static wv s880_bindRhs(struct fr183 *up, wv v1052_e, wv v1053_mode) {
  rt_enter("bindRhs");
  v1052_e = rt_bind(v1052_e, 0);
  v1053_mode = rt_bind(v1053_mode, 0);
  wv v1054_mutable = rt_eq(v1053_mode, rt_s(&S36));
  if (rt_cond(rt_eq(rt_get(&F_k, v1052_e), rt_s(&S186)))) {
    RT_RET(rt_get(&F_c, rt_chk(s883_array(up, v1052_e, v1054_mutable), "bindRhs")));
  }
  if (rt_cond(rt_eq(rt_get(&F_k, v1052_e), rt_s(&S187)))) {
    RT_RET(rt_get(&F_c, rt_chk(s884_record(up, v1052_e, v1054_mutable), "bindRhs")));
  }
  if (rt_cond(rt_b(rt_bool(({ wv t1972 = rt_def(w7_scalarKinds, "scalarKinds"); wv t1973 = rt_get(&F_k, v1052_e); rt_chk(w1_contains(t1972, t1973), "bindRhs"); }), "||") || rt_bool(rt_eq(v1053_mode, rt_s(&S37)), "||")))) {
    RT_RET(rt_get(&F_c, rt_chk(s900_expr(up, v1052_e), "bindRhs")));
  }
  RT_RET(({ wv t1974 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, v1052_e), "bindRhs"))); wv t1975 = rt_text((rt_cond(v1054_mutable) ? rt_i(1) : rt_i(0))); rt_cat(5, (wv[]){ rt_s(&S606), t1974, rt_s(&S385), t1975, rt_s(&S16) }); }));
}

static wv s881_setStmt(struct fr183 *up, wv v1055_s) {
  rt_enter("setStmt");
  v1055_s = rt_bind(v1055_s, 0);
  wv v1056_t = rt_bind(rt_get(&F_target, v1055_s), 0);
  if (rt_cond(rt_eq(rt_get(&F_k, v1056_t), rt_s(&S211)))) {
    wv v1063_d = rt_bind(({ wv t1976 = rt_get(&F_pos, v1056_t); wv t1977 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t1976, t1977); }), 0);
    wv v1064_at = rt_bind(rt_chk(s867_place(up, v1063_d), "setStmt"), 0);
    if (rt_cond(rt_eq(rt_get(&F_op, v1055_s), RT_NULL))) {
      wv v1067_v = rt_bind(({ wv t1980 = rt_get(&F_e, v1055_s); wv t1981 = (rt_cond(rt_get(&F_isRef, ({ wv t1978 = v1063_d; wv t1979 = rt_def(up->v833_decls, "decls"); rt_idx(t1978, t1979); }))) ? rt_s(&S37) : rt_s(&S36)); rt_chk(s880_bindRhs(up, t1980, t1981), "setStmt"); }), 0);
      if (rt_cond(rt_not(rt_chk(s868_tdz(up, v1063_d), "setStmt")))) {
        (void)rt_chk(s859_emit(up, ({ wv t1982 = rt_text(v1064_at); wv t1983 = rt_text(v1067_v); rt_cat(4, (wv[]){ t1982, rt_s(&S495), t1983, rt_s(&S14) }); })), "setStmt");
        RT_RET(RT_NULL);
      }
      wv v1068_temp = rt_bind(rt_chk(s860_tmp(up), "setStmt"), 0);
      (void)rt_chk(s859_emit(up, ({ wv t1984 = rt_text(v1068_temp); wv t1985 = rt_text(v1067_v); rt_cat(5, (wv[]){ rt_s(&S584), t1984, rt_s(&S495), t1985, rt_s(&S14) }); })), "setStmt");
      (void)rt_chk(s859_emit(up, ({ wv t1988 = rt_text(v1064_at); wv t1989 = rt_text(rt_chk(w1_quote(rt_get(&F_name, ({ wv t1986 = v1063_d; wv t1987 = rt_def(up->v833_decls, "decls"); rt_idx(t1986, t1987); }))), "setStmt")); rt_cat(5, (wv[]){ rt_s(&S607), t1988, rt_s(&S385), t1989, rt_s(&S485) }); })), "setStmt");
      (void)rt_chk(s859_emit(up, ({ wv t1990 = rt_text(v1064_at); wv t1991 = rt_text(v1068_temp); rt_cat(4, (wv[]){ t1990, rt_s(&S495), t1991, rt_s(&S14) }); })), "setStmt");
      RT_RET(RT_NULL);
    }
    wv v1065_v = rt_bind(rt_chk(s900_expr(up, rt_get(&F_e, v1055_s)), "setStmt"), 1);
    if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1055_s)), rt_s(&S138)), "&&") && rt_bool(rt_ne(rt_get(&F_k, rt_get(&F_e, v1055_s)), rt_s(&S211)), "&&")))) {
      wv v1069_temp = rt_bind(rt_chk(s860_tmp(up), "setStmt"), 0);
      (void)rt_chk(s859_emit(up, ({ wv t1992 = rt_text(v1069_temp); wv t1993 = rt_text(rt_get(&F_c, v1065_v)); rt_cat(5, (wv[]){ rt_s(&S584), t1992, rt_s(&S495), t1993, rt_s(&S14) }); })), "setStmt");
      v1065_v = ({ wv t1994 = v1069_temp; wv t1995 = rt_def(w7_constK, "constK"); rt_rec(1, 2, K55, (wv[]){ t1994, t1995 }); });
    }
    wv v1066_sq = rt_bind(rt_chk(s870_seq(up, ({ wv t1996 = rt_chk(s869_read(up, v1063_d), "setStmt"); wv t1997 = v1065_v; rt_arr(2, (wv[]){ t1996, t1997 }, 0); })), "setStmt"), 0);
    (void)rt_chk(s859_emit(up, ({ wv t2003 = rt_text(v1064_at); wv t2004 = rt_text(rt_get(&F_c, ({ wv t2001 = v1066_sq; wv t2002 = ({ wv t1998 = rt_text(rt_chk(w7_binFn(rt_get(&F_op, v1055_s)), "setStmt")); wv t1999 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1066_sq))); wv t2000 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1066_sq))); rt_cat(7, (wv[]){ rt_s(&S608), t1998, rt_s(&S97), t1999, rt_s(&S385), t2000, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2001, t2002), "setStmt"); }))); rt_cat(4, (wv[]){ t2003, rt_s(&S495), t2004, rt_s(&S14) }); })), "setStmt");
    RT_RET(RT_NULL);
  }
  wv v1057_key = RT_NULL;
  if (rt_cond(rt_eq(rt_get(&F_k, v1056_t), rt_s(&S189)))) {
    v1057_key = ({ wv t2005 = rt_chk(s862_fieldRef(up, rt_get(&F_field, v1056_t)), "setStmt"); wv t2006 = rt_def(w7_constK, "constK"); rt_rec(1, 2, K55, (wv[]){ t2005, t2006 }); });
  }
  wv v1058_v = rt_bind(rt_chk(s900_expr(up, rt_get(&F_e, v1055_s)), "setStmt"), 0);
  wv v1059_i = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1056_t), rt_s(&S189))) ? v1057_key : rt_chk(s900_expr(up, rt_get(&F_index, v1056_t)), "setStmt")), 0);
  wv v1060_box = rt_bind(rt_chk(s900_expr(up, (rt_cond(rt_eq(rt_get(&F_k, v1056_t), rt_s(&S189))) ? rt_get(&F_rec, v1056_t) : rt_get(&F_arr, v1056_t))), "setStmt"), 0);
  wv v1061_sq = rt_bind(rt_chk(s870_seq(up, rt_arr(3, (wv[]){ v1058_v, v1059_i, v1060_box }, 0)), "setStmt"), 0);
  wv v1062_which = rt_bind((rt_cond(rt_eq(rt_get(&F_k, v1056_t), rt_s(&S189))) ? rt_s(&S189) : rt_s(&S188)), 0);
  if (rt_cond(rt_eq(rt_get(&F_op, v1055_s), RT_NULL))) {
    (void)rt_chk(s859_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2009 = v1061_sq; wv t2010 = ({ wv t2007 = rt_text(v1062_which); wv t2008 = rt_text(std_join(rt_get(&F_cs, v1061_sq), rt_s(&S385))); rt_cat(5, (wv[]){ rt_s(&S609), t2007, rt_s(&S97), t2008, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2009, t2010), "setStmt"); }))), rt_s(&S14) })), "setStmt");
    RT_RET(RT_NULL);
  }
  (void)rt_chk(s859_emit(up, rt_cat(2, (wv[]){ rt_text(rt_get(&F_c, ({ wv t2014 = v1061_sq; wv t2015 = ({ wv t2011 = rt_text(v1062_which); wv t2012 = rt_text(std_join(rt_get(&F_cs, v1061_sq), rt_s(&S385))); wv t2013 = rt_text(rt_chk(w7_binFn(rt_get(&F_op, v1055_s)), "setStmt")); rt_cat(7, (wv[]){ rt_s(&S610), t2011, rt_s(&S97), t2012, rt_s(&S611), t2013, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2014, t2015), "setStmt"); }))), rt_s(&S14) })), "setStmt");
  RT_RET(RT_NULL);
}

static wv s882_exprs(struct fr183 *up, wv v1070_list) {
  rt_enter("exprs");
  v1070_list = rt_bind(v1070_list, 0);
  wv v1071_xs = rt_arr(0, NULL, 1);
  wv v1072_k = rt_i(0);
  while (rt_cond(({ wv t2016 = v1072_k; wv t2017 = std_length(v1070_list); rt_lt(t2016, t2017); }))) {
    (void)({ wv t2018 = v1071_xs; wv t2019 = rt_chk(s900_expr(up, rt_idx(v1072_k, v1070_list)), "exprs"); std_push(t2018, t2019); });
    v1072_k = rt_add(v1072_k, rt_i(1));
  }
  RT_RET(v1071_xs);
}

static wv s883_array(struct fr183 *up, wv v1073_e, wv v1074_mutable) {
  rt_enter("array");
  v1073_e = rt_bind(v1073_e, 0);
  v1074_mutable = rt_bind(v1074_mutable, 0);
  wv v1075_sq = rt_bind(rt_chk(s870_seq(up, rt_chk(s882_exprs(up, rt_get(&F_items, v1073_e)), "array")), "array"), 0);
  RT_RET(({ wv t2023 = v1075_sq; wv t2024 = ({ wv t2020 = rt_text(std_length(rt_get(&F_cs, v1075_sq))); wv t2021 = rt_text(rt_chk(w7_values(rt_get(&F_cs, v1075_sq)), "array")); wv t2022 = rt_text((rt_cond(v1074_mutable) ? rt_i(1) : rt_i(0))); rt_cat(7, (wv[]){ rt_s(&S612), t2020, rt_s(&S385), t2021, rt_s(&S385), t2022, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2023, t2024), "array"); }));
}

static wv s884_record(struct fr183 *up, wv v1076_e, wv v1077_mutable) {
  rt_enter("record");
  v1076_e = rt_bind(v1076_e, 0);
  v1077_mutable = rt_bind(v1077_mutable, 0);
  wv v1078_shape = rt_s(&S571);
  if (rt_cond(rt_gt(std_length(rt_get(&F_fields, v1076_e)), rt_i(0)))) {
    wv v1082_names = rt_arr(0, NULL, 1);
    wv v1083_k = rt_i(0);
    while (rt_cond(({ wv t2025 = v1083_k; wv t2026 = std_length(rt_get(&F_fields, v1076_e)); rt_lt(t2025, t2026); }))) {
      (void)({ wv t2029 = v1082_names; wv t2030 = rt_get(&F_name, ({ wv t2027 = v1083_k; wv t2028 = rt_get(&F_fields, v1076_e); rt_idx(t2027, t2028); })); std_push(t2029, t2030); });
      v1083_k = rt_add(v1083_k, rt_i(1));
    }
    wv v1084_key = rt_bind(std_join(v1082_names, rt_s(&S96)), 0);
    v1083_k = rt_i(0);
    while (rt_cond(({ wv t2031 = v1083_k; wv t2032 = std_length(v1082_names); rt_lt(t2031, t2032); }))) {
      (void)rt_chk(s862_fieldRef(up, rt_idx(v1083_k, v1082_names)), "record");
      v1083_k = rt_add(v1083_k, rt_i(1));
    }
    v1083_k = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t2033 = v1083_k; wv t2034 = std_length(rt_def(up->v824_shapes, "shapes")); rt_lt(t2033, t2034); }), "&&") && rt_bool(({ wv t2037 = ({ wv t2035 = v1083_k; wv t2036 = rt_def(up->v824_shapes, "shapes"); rt_idx(t2035, t2036); }); wv t2038 = v1084_key; rt_ne(t2037, t2038); }), "&&")))) {
      v1083_k = rt_add(v1083_k, rt_i(1));
    }
    if (rt_cond(({ wv t2039 = v1083_k; wv t2040 = std_length(rt_def(up->v824_shapes, "shapes")); rt_eq(t2039, t2040); }))) {
      (void)({ wv t2041 = rt_def(up->v824_shapes, "shapes"); wv t2042 = v1084_key; std_push(t2041, t2042); });
    }
    v1078_shape = rt_cat(2, (wv[]){ rt_s(&S613), rt_text(v1083_k) });
  }
  wv v1079_vals = rt_arr(0, NULL, 1);
  wv v1080_k = rt_i(0);
  while (rt_cond(({ wv t2043 = v1080_k; wv t2044 = std_length(rt_get(&F_fields, v1076_e)); rt_lt(t2043, t2044); }))) {
    (void)({ wv t2047 = v1079_vals; wv t2048 = rt_chk(s900_expr(up, rt_get(&F_e, ({ wv t2045 = v1080_k; wv t2046 = rt_get(&F_fields, v1076_e); rt_idx(t2045, t2046); }))), "record"); std_push(t2047, t2048); });
    v1080_k = rt_add(v1080_k, rt_i(1));
  }
  wv v1081_sq = rt_bind(rt_chk(s870_seq(up, v1079_vals), "record"), 0);
  RT_RET(({ wv t2053 = v1081_sq; wv t2054 = ({ wv t2049 = rt_text((rt_cond(v1077_mutable) ? rt_i(1) : rt_i(0))); wv t2050 = rt_text(std_length(rt_get(&F_cs, v1081_sq))); wv t2051 = rt_text(v1078_shape); wv t2052 = rt_text(rt_chk(w7_values(rt_get(&F_cs, v1081_sq)), "record")); rt_cat(9, (wv[]){ rt_s(&S614), t2049, rt_s(&S385), t2050, rt_s(&S385), t2051, rt_s(&S385), t2052, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2053, t2054), "record"); }));
}

static wv s885_onfailStmt(struct fr183 *up, wv v1085_s) {
  rt_enter("onfailStmt");
  v1085_s = rt_bind(v1085_s, 0);
  wv v1086_call = rt_bind(rt_get(&F_e, v1085_s), 0);
  wv v1087_args = rt_arr(0, NULL, 1);
  wv v1088_k = rt_i(0);
  while (rt_cond(({ wv t2055 = v1088_k; wv t2056 = std_length(rt_get(&F_args, v1086_call)); rt_lt(t2055, t2056); }))) {
    wv v1092_a = rt_bind(({ wv t2057 = v1088_k; wv t2058 = rt_get(&F_args, v1086_call); rt_idx(t2057, t2058); }), 0);
    if (rt_cond(({ wv t2059 = rt_def(w7_plainKinds, "plainKinds"); wv t2060 = rt_get(&F_k, v1092_a); rt_chk(w1_contains(t2059, t2060), "onfailStmt"); }))) {
      (void)({ wv t2061 = v1087_args; wv t2062 = rt_get(&F_c, rt_chk(s900_expr(up, v1092_a), "onfailStmt")); std_push(t2061, t2062); });
    } else {
      wv v1093_temp = rt_bind(rt_chk(s860_tmp(up), "onfailStmt"), 0);
      (void)rt_chk(s859_emit(up, ({ wv t2063 = rt_text(v1093_temp); wv t2064 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, v1092_a), "onfailStmt"))); rt_cat(5, (wv[]){ rt_s(&S584), t2063, rt_s(&S495), t2064, rt_s(&S14) }); })), "onfailStmt");
      (void)std_push(v1087_args, v1093_temp);
    }
    v1088_k = rt_add(v1088_k, rt_i(1));
  }
  wv v1089_r = rt_bind(rt_chk(s860_tmp(up), "onfailStmt"), 0);
  (void)rt_chk(s859_emit(up, ({ wv t2067 = rt_text(v1089_r); wv t2068 = rt_text(({ wv t2065 = rt_get(&F_callee, v1086_call); wv t2066 = v1087_args; rt_chk(s899_callC(up, t2065, t2066, rt_b(0)), "onfailStmt"); })); rt_cat(5, (wv[]){ rt_s(&S584), t2067, rt_s(&S495), t2068, rt_s(&S14) }); })), "onfailStmt");
  (void)rt_chk(s859_emit(up, rt_s(&S615)), "onfailStmt");
  up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
  wv v1090_h = rt_bind(rt_get(&F_handler, v1085_s), 0);
  if (rt_cond(rt_ne(rt_get(&F_param, v1090_h), RT_NULL))) {
    (void)rt_chk(s878_bindDecl(up, ({ wv t2069 = rt_get(&F_paramPos, v1090_h); wv t2070 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t2069, t2070); }), rt_s(&S616)), "onfailStmt");
  }
  (void)rt_chk(s859_emit(up, rt_s(&S617)), "onfailStmt");
  (void)rt_chk(s859_emit(up, rt_s(&S100)), "onfailStmt");
  up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
  (void)rt_chk(s873_resets(up, rt_get(&F_body, v1090_h)), "onfailStmt");
  (void)rt_chk(s872_stmts(up, rt_get(&F_stmts, rt_get(&F_body, v1090_h))), "onfailStmt");
  (void)rt_chk(s859_emit(up, ({ wv t2071 = rt_text(v1089_r); wv t2072 = rt_text(rt_chk(s875_valueOf(up, rt_get(&F_body, v1090_h)), "onfailStmt")); rt_cat(4, (wv[]){ t2071, rt_s(&S495), t2072, rt_s(&S14) }); })), "onfailStmt");
  up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
  (void)rt_chk(s859_emit(up, rt_s(&S15)), "onfailStmt");
  up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
  (void)rt_chk(s859_emit(up, rt_s(&S15)), "onfailStmt");
  wv v1091_rest = rt_bind(v1085_s, 1);
  rt_set_mem(RT_NULL, &F_handler, v1091_rest);
  ({ wv t2075 = ({ wv t2073 = v1089_r; wv t2074 = rt_get(&F_pos, v1085_s); rt_rec(0, 4, K50, (wv[]){ rt_s(&S528), t2073, t2074, rt_b(0) }); }); wv t2076 = v1091_rest; rt_set_mem(t2075, &F_e, t2076); });
  (void)rt_chk(s876_stmt(up, v1091_rest), "onfailStmt");
  RT_RET(RT_NULL);
}

static wv s886_chainStmt(struct fr183 *up, wv v1094_e) {
  rt_enter("chainStmt");
  v1094_e = rt_bind(v1094_e, 0);
  wv v1095_k = rt_i(0);
  while (rt_cond(({ wv t2077 = v1095_k; wv t2078 = std_length(rt_get(&F_branches, v1094_e)); rt_lt(t2077, t2078); }))) {
    wv v1096_b = rt_bind(({ wv t2079 = v1095_k; wv t2080 = rt_get(&F_branches, v1094_e); rt_idx(t2079, t2080); }), 0);
    (void)rt_chk(s859_emit(up, ({ wv t2081 = rt_text((rt_cond(rt_gt(v1095_k, rt_i(0))) ? rt_s(&S529) : rt_s(&S13))); wv t2082 = rt_text(rt_chk(s887_test(up, v1096_b), "chainStmt")); rt_cat(4, (wv[]){ t2081, rt_s(&S530), t2082, rt_s(&S498) }); })), "chainStmt");
    up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
    (void)rt_chk(s874_blockBody(up, rt_get(&F_body, v1096_b)), "chainStmt");
    up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
    v1095_k = rt_add(v1095_k, rt_i(1));
  }
  if (rt_cond(rt_ne(rt_get(&F_els, v1094_e), RT_NULL))) {
    (void)rt_chk(s859_emit(up, rt_s(&S531)), "chainStmt");
    up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
    (void)rt_chk(s874_blockBody(up, rt_get(&F_els, v1094_e)), "chainStmt");
    up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s859_emit(up, rt_s(&S15)), "chainStmt");
  RT_RET(RT_NULL);
}

static wv s887_test(struct fr183 *up, wv v1097_b) {
  rt_enter("test");
  v1097_b = rt_bind(v1097_b, 0);
  RT_RET(({ wv t2083 = rt_text((rt_cond(rt_get(&F_neg, v1097_b)) ? rt_s(&S94) : rt_s(&S13))); wv t2084 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_cond, v1097_b)), "test"))); rt_cat(4, (wv[]){ t2083, rt_s(&S592), t2084, rt_s(&S16) }); }));
}

static wv s888_branchCount(struct fr183 *up, wv v1098_e) {
  rt_enter("branchCount");
  v1098_e = rt_bind(v1098_e, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v1098_e), rt_s(&S184))) ? std_length(rt_get(&F_branches, v1098_e)) : std_length(rt_get(&F_cases, v1098_e))));
}

static wv s889_branchBody(struct fr183 *up, wv v1099_e, wv v1100_i) {
  rt_enter("branchBody");
  v1099_e = rt_bind(v1099_e, 0);
  v1100_i = rt_bind(v1100_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v1099_e), rt_s(&S184))) ? rt_get(&F_body, ({ wv t2085 = v1100_i; wv t2086 = rt_get(&F_branches, v1099_e); rt_idx(t2085, t2086); })) : rt_get(&F_body, ({ wv t2087 = v1100_i; wv t2088 = rt_get(&F_cases, v1099_e); rt_idx(t2087, t2088); }))));
}

static wv s890_branchTest(struct fr183 *up, wv v1101_e, wv v1102_t, wv v1103_i) {
  rt_enter("branchTest");
  v1101_e = rt_bind(v1101_e, 0);
  v1102_t = rt_bind(v1102_t, 0);
  v1103_i = rt_bind(v1103_i, 0);
  RT_RET((rt_cond(rt_eq(rt_get(&F_k, v1101_e), rt_s(&S184))) ? rt_chk(s887_test(up, ({ wv t2089 = v1103_i; wv t2090 = rt_get(&F_branches, v1101_e); rt_idx(t2089, t2090); })), "branchTest") : ({ wv t2093 = v1102_t; wv t2094 = ({ wv t2091 = v1103_i; wv t2092 = rt_get(&F_cases, v1101_e); rt_idx(t2091, t2092); }); rt_chk(s895_caseTest(up, t2093, t2094), "branchTest"); })));
}

static wv s891_isPlain(struct fr183 *up, wv v1104_e) {
  rt_enter("isPlain");
  v1104_e = rt_bind(v1104_e, 0);
  wv v1105_k = rt_i(0);
  while (rt_cond(({ wv t2095 = v1105_k; wv t2096 = rt_chk(s888_branchCount(up, v1104_e), "isPlain"); rt_le(t2095, t2096); }))) {
    wv v1106_b = rt_bind((rt_cond(({ wv t2097 = v1105_k; wv t2098 = rt_chk(s888_branchCount(up, v1104_e), "isPlain"); rt_lt(t2097, t2098); })) ? rt_chk(s889_branchBody(up, v1104_e, v1105_k), "isPlain") : rt_get(&F_els, v1104_e)), 0);
    wv v1107_j = rt_i(0);
    while (rt_cond(({ wv t2099 = v1107_j; wv t2100 = std_length(rt_get(&F_stmts, v1106_b)); rt_lt(t2099, t2100); }))) {
      if (rt_cond(rt_ne(rt_get(&F_k, ({ wv t2101 = v1107_j; wv t2102 = rt_get(&F_stmts, v1106_b); rt_idx(t2101, t2102); })), rt_s(&S200)))) {
        RT_RET(rt_b(0));
      }
      v1107_j = rt_add(v1107_j, rt_i(1));
    }
    v1105_k = rt_add(v1105_k, rt_i(1));
  }
  RT_RET(rt_b(1));
}

static wv s892_ternaryChain(struct fr183 *up, wv v1108_e, wv v1109_t) {
  rt_enter("ternaryChain");
  v1108_e = rt_bind(v1108_e, 0);
  v1109_t = rt_bind(v1109_t, 0);
  wv v1110_code = rt_bind(rt_chk(s875_valueOf(up, rt_get(&F_els, v1108_e)), "ternaryChain"), 1);
  wv v1111_k = rt_sub(rt_chk(s888_branchCount(up, v1108_e), "ternaryChain"), rt_i(1));
  while (rt_cond(rt_ge(v1111_k, rt_i(0)))) {
    v1110_code = ({ wv t2103 = rt_text(rt_chk(s890_branchTest(up, v1108_e, v1109_t, v1111_k), "ternaryChain")); wv t2104 = rt_text(rt_chk(s875_valueOf(up, rt_chk(s889_branchBody(up, v1108_e, v1111_k), "ternaryChain")), "ternaryChain")); wv t2105 = rt_text(v1110_code); rt_cat(7, (wv[]){ rt_s(&S97), t2103, rt_s(&S532), t2104, rt_s(&S533), t2105, rt_s(&S16) }); });
    v1111_k = rt_sub(v1111_k, rt_i(1));
  }
  RT_RET(v1110_code);
}

static wv s893_branchValue(struct fr183 *up, wv v1112_e, wv v1113_t, wv v1114_subject) {
  rt_enter("branchValue");
  v1112_e = rt_bind(v1112_e, 0);
  v1113_t = rt_bind(v1113_t, 0);
  v1114_subject = rt_bind(v1114_subject, 0);
  wv v1115_start = rt_bind(std_length(rt_def(up->v829_out, "out")), 0);
  wv v1116_savedIndent = rt_bind(rt_def(up->v830_indent, "indent"), 0);
  wv v1117_savedLoops = rt_bind(rt_def(up->v831_loops, "loops"), 0);
  wv v1118_savedIife = rt_bind(rt_def(up->v832_iife, "iife"), 0);
  wv t2106 = rt_add(v1116_savedIndent, rt_i(1));
  (void)rt_def(up->v830_indent, "indent");
  up->v830_indent = t2106;
  wv t2107 = rt_i(0);
  (void)rt_def(up->v831_loops, "loops");
  up->v831_loops = t2107;
  wv t2108 = rt_b(1);
  (void)rt_def(up->v832_iife, "iife");
  up->v832_iife = t2108;
  if (rt_cond(rt_ne(v1114_subject, RT_NULL))) {
    (void)rt_chk(s859_emit(up, ({ wv t2109 = rt_text(v1113_t); wv t2110 = rt_text(v1114_subject); rt_cat(5, (wv[]){ rt_s(&S584), t2109, rt_s(&S495), t2110, rt_s(&S14) }); })), "branchValue");
  }
  wv v1119_r = rt_bind(rt_chk(s860_tmp(up), "branchValue"), 0);
  (void)rt_chk(s859_emit(up, rt_cat(3, (wv[]){ rt_s(&S584), rt_text(v1119_r), rt_s(&S14) })), "branchValue");
  wv v1120_k = rt_i(0);
  while (rt_cond(({ wv t2111 = v1120_k; wv t2112 = rt_chk(s888_branchCount(up, v1112_e), "branchValue"); rt_lt(t2111, t2112); }))) {
    wv v1122_b = rt_bind(rt_chk(s889_branchBody(up, v1112_e, v1120_k), "branchValue"), 0);
    (void)rt_chk(s859_emit(up, ({ wv t2113 = rt_text((rt_cond(rt_gt(v1120_k, rt_i(0))) ? rt_s(&S529) : rt_s(&S13))); wv t2114 = rt_text(rt_chk(s890_branchTest(up, v1112_e, v1113_t, v1120_k), "branchValue")); rt_cat(4, (wv[]){ t2113, rt_s(&S530), t2114, rt_s(&S498) }); })), "branchValue");
    up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
    (void)rt_chk(s873_resets(up, v1122_b), "branchValue");
    (void)rt_chk(s872_stmts(up, rt_get(&F_stmts, v1122_b)), "branchValue");
    (void)rt_chk(s859_emit(up, ({ wv t2115 = rt_text(v1119_r); wv t2116 = rt_text(rt_chk(s875_valueOf(up, v1122_b), "branchValue")); rt_cat(4, (wv[]){ t2115, rt_s(&S495), t2116, rt_s(&S14) }); })), "branchValue");
    up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
    v1120_k = rt_add(v1120_k, rt_i(1));
  }
  (void)rt_chk(s859_emit(up, rt_s(&S531)), "branchValue");
  up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
  (void)rt_chk(s873_resets(up, rt_get(&F_els, v1112_e)), "branchValue");
  (void)rt_chk(s872_stmts(up, rt_get(&F_stmts, rt_get(&F_els, v1112_e))), "branchValue");
  (void)rt_chk(s859_emit(up, ({ wv t2117 = rt_text(v1119_r); wv t2118 = rt_text(rt_chk(s875_valueOf(up, rt_get(&F_els, v1112_e)), "branchValue")); rt_cat(4, (wv[]){ t2117, rt_s(&S495), t2118, rt_s(&S14) }); })), "branchValue");
  up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
  (void)rt_chk(s859_emit(up, rt_s(&S15)), "branchValue");
  (void)rt_chk(s859_emit(up, rt_cat(2, (wv[]){ rt_text(v1119_r), rt_s(&S14) })), "branchValue");
  wv v1121_body = rt_bind(std_join(({ wv t2119 = rt_def(up->v829_out, "out"); wv t2120 = v1115_start; wv t2121 = std_length(rt_def(up->v829_out, "out")); std_slice(t2119, t2120, t2121); }), rt_s(&S2)), 0);
  while (rt_cond(({ wv t2122 = std_length(rt_def(up->v829_out, "out")); wv t2123 = v1115_start; rt_gt(t2122, t2123); }))) {
    (void)std_pop(rt_def(up->v829_out, "out"));
  }
  wv t2124 = rt_bind(v1116_savedIndent, 1);
  (void)rt_def(up->v830_indent, "indent");
  up->v830_indent = t2124;
  wv t2125 = rt_bind(v1117_savedLoops, 1);
  (void)rt_def(up->v831_loops, "loops");
  up->v831_loops = t2125;
  wv t2126 = rt_bind(v1118_savedIife, 1);
  (void)rt_def(up->v832_iife, "iife");
  up->v832_iife = t2126;
  RT_RET(({ wv t2127 = rt_text(v1121_body); wv t2128 = rt_text(rt_chk(w7_repeat(rt_s(&S480), rt_def(up->v830_indent, "indent")), "branchValue")); rt_cat(5, (wv[]){ rt_s(&S618), t2127, rt_s(&S2), t2128, rt_s(&S619) }); }));
}

static wv s894_chainExpr(struct fr183 *up, wv v1123_e) {
  rt_enter("chainExpr");
  v1123_e = rt_bind(v1123_e, 0);
  if (rt_cond(rt_chk(s891_isPlain(up, v1123_e), "chainExpr"))) {
    RT_RET(rt_chk(s892_ternaryChain(up, v1123_e, RT_NULL), "chainExpr"));
  }
  RT_RET(rt_chk(s893_branchValue(up, v1123_e, RT_NULL, RT_NULL), "chainExpr"));
}

static wv s895_caseTest(struct fr183 *up, wv v1124_t, wv v1125_a) {
  rt_enter("caseTest");
  v1124_t = rt_bind(v1124_t, 0);
  v1125_a = rt_bind(v1125_a, 0);
  wv v1126_tests = rt_arr(0, NULL, 1);
  wv v1127_k = rt_i(0);
  while (rt_cond(({ wv t2129 = v1127_k; wv t2130 = std_length(rt_get(&F_values, v1125_a)); rt_lt(t2129, t2130); }))) {
    (void)({ wv t2135 = v1126_tests; wv t2136 = ({ wv t2133 = rt_text(v1124_t); wv t2134 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, ({ wv t2131 = v1127_k; wv t2132 = rt_get(&F_values, v1125_a); rt_idx(t2131, t2132); })), "caseTest"))); rt_cat(5, (wv[]){ rt_s(&S620), t2133, rt_s(&S385), t2134, rt_s(&S16) }); }); std_push(t2135, t2136); });
    v1127_k = rt_add(v1127_k, rt_i(1));
  }
  RT_RET(std_join(v1126_tests, rt_s(&S537)));
}

static wv s896_matchElse(struct fr183 *up, wv v1128_e, wv v1129_t) {
  rt_enter("matchElse");
  v1128_e = rt_bind(v1128_e, 0);
  v1129_t = rt_bind(v1129_t, 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(rt_get(&F_els, v1128_e), RT_NULL), "||") || rt_bool(rt_ne(rt_get(&F_k, rt_idx(rt_i(0), rt_get(&F_values, rt_idx(rt_i(0), rt_get(&F_cases, v1128_e))))), rt_s(&S189)), "||")))) {
    RT_RET(rt_get(&F_els, v1128_e));
  }
  RT_RET(({ wv t2137 = rt_arr(0, NULL, 0); wv t2138 = rt_rec(0, 4, K50, (wv[]){ rt_s(&S528), rt_cat(3, (wv[]){ rt_s(&S621), rt_text(v1129_t), rt_s(&S622) }), RT_NULL, rt_b(0) }); rt_rec(0, 3, K21, (wv[]){ t2137, t2138, RT_NULL }); }));
}

static wv s897_matchStmt(struct fr183 *up, wv v1130_e) {
  rt_enter("matchStmt");
  v1130_e = rt_bind(v1130_e, 0);
  wv v1131_t = rt_bind(rt_chk(s860_tmp(up), "matchStmt"), 0);
  (void)rt_chk(s859_emit(up, ({ wv t2139 = rt_text(v1131_t); wv t2140 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_subject, v1130_e)), "matchStmt"))); rt_cat(5, (wv[]){ rt_s(&S584), t2139, rt_s(&S495), t2140, rt_s(&S14) }); })), "matchStmt");
  wv v1132_k = rt_i(0);
  while (rt_cond(({ wv t2141 = v1132_k; wv t2142 = std_length(rt_get(&F_cases, v1130_e)); rt_lt(t2141, t2142); }))) {
    (void)rt_chk(s859_emit(up, ({ wv t2147 = rt_text((rt_cond(rt_gt(v1132_k, rt_i(0))) ? rt_s(&S529) : rt_s(&S13))); wv t2148 = rt_text(({ wv t2145 = v1131_t; wv t2146 = ({ wv t2143 = v1132_k; wv t2144 = rt_get(&F_cases, v1130_e); rt_idx(t2143, t2144); }); rt_chk(s895_caseTest(up, t2145, t2146), "matchStmt"); })); rt_cat(4, (wv[]){ t2147, rt_s(&S530), t2148, rt_s(&S498) }); })), "matchStmt");
    up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
    (void)rt_chk(s874_blockBody(up, rt_get(&F_body, ({ wv t2149 = v1132_k; wv t2150 = rt_get(&F_cases, v1130_e); rt_idx(t2149, t2150); }))), "matchStmt");
    up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
    v1132_k = rt_add(v1132_k, rt_i(1));
  }
  wv v1133_els = rt_bind(rt_chk(s896_matchElse(up, v1130_e, v1131_t), "matchStmt"), 0);
  if (rt_cond(rt_ne(v1133_els, RT_NULL))) {
    (void)rt_chk(s859_emit(up, rt_s(&S531)), "matchStmt");
    up->v830_indent = rt_add(rt_def(up->v830_indent, "indent"), rt_i(1));
    (void)rt_chk(s874_blockBody(up, v1133_els), "matchStmt");
    up->v830_indent = rt_sub(rt_def(up->v830_indent, "indent"), rt_i(1));
  }
  (void)rt_chk(s859_emit(up, rt_s(&S15)), "matchStmt");
  RT_RET(RT_NULL);
}

static wv s898_matchExpr(struct fr183 *up, wv v1134_e) {
  rt_enter("matchExpr");
  v1134_e = rt_bind(v1134_e, 0);
  wv v1135_t = rt_bind(rt_chk(s860_tmp(up), "matchExpr"), 0);
  wv v1136_subject = rt_bind(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_subject, v1134_e)), "matchExpr")), 0);
  wv v1137_withElse = rt_bind(v1134_e, 1);
  ({ wv t2151 = rt_chk(s896_matchElse(up, v1134_e, v1135_t), "matchExpr"); wv t2152 = v1137_withElse; rt_set_mem(t2151, &F_els, t2152); });
  if (rt_cond(rt_chk(s891_isPlain(up, v1137_withElse), "matchExpr"))) {
    RT_RET(({ wv t2153 = rt_text(v1135_t); wv t2154 = rt_text(v1136_subject); wv t2155 = rt_text(rt_chk(s892_ternaryChain(up, v1137_withElse, v1135_t), "matchExpr")); rt_cat(7, (wv[]){ rt_s(&S623), t2153, rt_s(&S495), t2154, rt_s(&S624), t2155, rt_s(&S586) }); }));
  }
  RT_RET(rt_chk(s893_branchValue(up, v1137_withElse, v1135_t, v1136_subject), "matchExpr"));
}

static wv s899_callC(struct fr183 *up, wv v1138_callee, wv v1139_args, wv v1140_checked) {
  rt_enter("callC");
  v1138_callee = rt_bind(v1138_callee, 0);
  v1140_checked = rt_bind(v1140_checked, 0);
  wv v1141_code = rt_s(&S13);
  if (rt_cond(rt_eq(rt_get(&F_k, v1138_callee), rt_s(&S156)))) {
    v1141_code = ({ wv t2156 = rt_text(rt_get(&F_name, v1138_callee)); wv t2157 = rt_text(std_join(v1139_args, rt_s(&S385))); rt_cat(5, (wv[]){ rt_s(&S625), t2156, rt_s(&S97), t2157, rt_s(&S16) }); });
    if (rt_cond(rt_not(({ wv t2158 = rt_def(w7_signalers, "signalers"); wv t2159 = rt_get(&F_name, v1138_callee); rt_chk(w1_contains(t2158, t2159), "callC"); })))) {
      RT_RET(v1141_code);
    }
  } else {
    wv v1142_f = rt_s(&S13);
    wv v1143_parent = RT_NULL;
    if (rt_cond(rt_eq(rt_get(&F_k, v1138_callee), rt_s(&S189)))) {
      v1142_f = ({ wv t2160 = rt_text(rt_chk(s843_modId(up, rt_chk(s864_stolenFrom(up, rt_get(&F_rec, v1138_callee)), "callC")), "callC")); wv t2161 = rt_text(rt_get(&F_field, v1138_callee)); rt_cat(4, (wv[]){ rt_s(&S478), t2160, rt_s(&S12), t2161 }); });
    } else {
      wv v1146_d = rt_bind(({ wv t2162 = rt_get(&F_pos, v1138_callee); wv t2163 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t2162, t2163); }), 0);
      wv v1147_sf = rt_bind(rt_get(&F_fnIndex, ({ wv t2164 = v1146_d; wv t2165 = rt_def(up->v833_decls, "decls"); rt_idx(t2164, t2165); })), 0);
      v1142_f = rt_bind(rt_get(&F_cname, ({ wv t2166 = v1147_sf; wv t2167 = rt_def(up->v834_fns, "fns"); rt_idx(t2166, t2167); })), 1);
      wv v1148_pf = rt_bind(rt_get(&F_parent, ({ wv t2168 = v1147_sf; wv t2169 = rt_def(up->v834_fns, "fns"); rt_idx(t2168, t2169); })), 0);
      if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2170 = v1148_pf; wv t2171 = rt_def(up->v834_fns, "fns"); rt_idx(t2170, t2171); }))))) {
        v1143_parent = rt_bind(v1148_pf, 1);
      }
    }
    wv v1144_all = rt_arr(0, NULL, 1);
    if (rt_cond(rt_ne(v1143_parent, RT_NULL))) {
      (void)({ wv t2180 = v1144_all; wv t2181 = (rt_cond(({ wv t2172 = v1143_parent; wv t2173 = rt_def(up->v842_cur, "cur"); rt_eq(t2172, t2173); })) ? rt_s(&S626) : rt_cat(2, (wv[]){ rt_s(&S580), rt_text(rt_chk(w7_repeat(rt_s(&S581), rt_sub(({ wv t2178 = rt_get(&F_depth, ({ wv t2174 = rt_def(up->v842_cur, "cur"); wv t2175 = rt_def(up->v834_fns, "fns"); rt_idx(t2174, t2175); })); wv t2179 = rt_get(&F_depth, ({ wv t2176 = v1143_parent; wv t2177 = rt_def(up->v834_fns, "fns"); rt_idx(t2176, t2177); })); rt_sub(t2178, t2179); }), rt_i(1))), "callC")) })); std_push(t2180, t2181); });
    }
    wv v1145_k = rt_i(0);
    while (rt_cond(({ wv t2182 = v1145_k; wv t2183 = std_length(v1139_args); rt_lt(t2182, t2183); }))) {
      (void)({ wv t2184 = v1144_all; wv t2185 = rt_idx(v1145_k, v1139_args); std_push(t2184, t2185); });
      v1145_k = rt_add(v1145_k, rt_i(1));
    }
    v1141_code = ({ wv t2186 = rt_text(v1142_f); wv t2187 = rt_text(std_join(v1144_all, rt_s(&S385))); rt_cat(4, (wv[]){ t2186, rt_s(&S97), t2187, rt_s(&S16) }); });
  }
  RT_RET((rt_cond(v1140_checked) ? ({ wv t2192 = rt_text(v1141_code); wv t2193 = rt_text((rt_cond(rt_get(&F_top, ({ wv t2188 = rt_def(up->v842_cur, "cur"); wv t2189 = rt_def(up->v834_fns, "fns"); rt_idx(t2188, t2189); }))) ? rt_s(&S571) : rt_chk(w1_quote(rt_get(&F_name, ({ wv t2190 = rt_def(up->v842_cur, "cur"); wv t2191 = rt_def(up->v834_fns, "fns"); rt_idx(t2190, t2191); }))), "callC"))); rt_cat(5, (wv[]){ rt_s(&S627), t2192, rt_s(&S385), t2193, rt_s(&S16) }); }) : v1141_code));
}

static wv s900_expr(struct fr183 *up, wv v1149_e) {
  rt_enter("expr");
  wv v1150_kind = rt_bind(rt_get(&F_k, v1149_e), 0);
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S138)))) {
    RT_RET(({ wv t2194 = rt_cat(3, (wv[]){ rt_s(&S593), rt_text(rt_get(&F_v, v1149_e)), rt_s(&S16) }); wv t2195 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2194, t2195 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S148)))) {
    RT_RET(({ wv t2196 = rt_cat(3, (wv[]){ rt_s(&S628), rt_text(rt_get(&F_v, v1149_e)), rt_s(&S16) }); wv t2197 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2196, t2197 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S171)))) {
    wv v1151_parts = rt_arr(0, NULL, 1);
    wv v1152_k = rt_i(0);
    while (rt_cond(({ wv t2198 = v1152_k; wv t2199 = std_length(rt_get(&F_parts, v1149_e)); rt_lt(t2198, t2199); }))) {
      wv v1154_part = rt_bind(({ wv t2200 = v1152_k; wv t2201 = rt_get(&F_parts, v1149_e); rt_idx(t2200, t2201); }), 0);
      if (rt_cond(rt_eq(std_typeOf(v1154_part), rt_s(&S185)))) {
        if (rt_cond(rt_ne(v1154_part, rt_s(&S13)))) {
          (void)({ wv t2204 = v1151_parts; wv t2205 = ({ wv t2202 = rt_cat(3, (wv[]){ rt_s(&S629), rt_text(rt_chk(s861_str(up, v1154_part), "expr")), rt_s(&S16) }); wv t2203 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2202, t2203 }); }); std_push(t2204, t2205); });
        }
      } else {
        (void)({ wv t2208 = v1151_parts; wv t2209 = ({ wv t2206 = rt_cat(3, (wv[]){ rt_s(&S630), rt_text(rt_get(&F_c, rt_chk(s900_expr(up, v1154_part), "expr"))), rt_s(&S16) }); wv t2207 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2206, t2207 }); }); std_push(t2208, t2209); });
      }
      v1152_k = rt_add(v1152_k, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v1151_parts), rt_i(0)))) {
      RT_RET(({ wv t2210 = rt_cat(3, (wv[]){ rt_s(&S629), rt_text(rt_chk(s861_str(up, rt_s(&S13)), "expr")), rt_s(&S16) }); wv t2211 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2210, t2211 }); }));
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(std_length(v1151_parts), rt_i(1)), "&&") && rt_bool(({ wv t2212 = rt_get(&F_k, rt_idx(rt_i(0), v1151_parts)); wv t2213 = rt_def(w7_constK, "constK"); rt_eq(t2212, t2213); }), "&&")))) {
      RT_RET(rt_idx(rt_i(0), v1151_parts));
    }
    wv v1153_sq = rt_bind(rt_chk(s870_seq(up, v1151_parts), "expr"), 0);
    RT_RET(({ wv t2216 = v1153_sq; wv t2217 = ({ wv t2214 = rt_text(std_length(rt_get(&F_cs, v1153_sq))); wv t2215 = rt_text(rt_chk(w7_values(rt_get(&F_cs, v1153_sq)), "expr")); rt_cat(5, (wv[]){ rt_s(&S631), t2214, rt_s(&S385), t2215, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2216, t2217), "expr"); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S281)))) {
    RT_RET(({ wv t2218 = rt_cat(3, (wv[]){ rt_s(&S632), rt_text((rt_cond(rt_get(&F_v, v1149_e)) ? rt_i(1) : rt_i(0))), rt_s(&S16) }); wv t2219 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2218, t2219 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S51)))) {
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S589), rt_def(w7_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S52)))) {
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S633), rt_def(w7_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S53)))) {
    RT_RET(rt_rec(0, 2, K55, (wv[]){ rt_s(&S634), rt_def(w7_constK, "constK") }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S211)))) {
    RT_RET(rt_chk(s869_read(up, ({ wv t2220 = rt_get(&F_pos, v1149_e); wv t2221 = rt_def(up->v839_curSlot, "curSlot"); rt_idx(t2220, t2221); })), "expr"));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S156)))) {
    RT_RET(({ wv t2222 = rt_cat(3, (wv[]){ rt_s(&S625), rt_text(rt_get(&F_name, v1149_e)), rt_s(&S635) }); wv t2223 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2222, t2223 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S186)))) {
    RT_RET(rt_chk(s883_array(up, v1149_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S187)))) {
    RT_RET(rt_chk(s884_record(up, v1149_e, rt_b(0)), "expr"));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S188)))) {
    wv v1155_sq = rt_bind(rt_chk(s870_seq(up, ({ wv t2224 = rt_chk(s900_expr(up, rt_get(&F_index, v1149_e)), "expr"); wv t2225 = rt_chk(s900_expr(up, rt_get(&F_arr, v1149_e)), "expr"); rt_arr(2, (wv[]){ t2224, t2225 }, 0); })), "expr"), 0);
    RT_RET(({ wv t2228 = v1155_sq; wv t2229 = ({ wv t2226 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1155_sq))); wv t2227 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1155_sq))); rt_cat(5, (wv[]){ rt_s(&S636), t2226, rt_s(&S385), t2227, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2228, t2229), "expr"); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S189)))) {
    wv v1156_lin = rt_bind(rt_chk(s866_enumOf(up, rt_get(&F_rec, v1149_e)), "expr"), 0);
    if (rt_cond(rt_ne(v1156_lin, RT_NULL))) {
      wv v1160_ms = rt_bind(rt_get(&F_members, rt_get(&F_node, v1156_lin)), 0);
      wv v1161_k = rt_i(0);
      while (rt_cond(({ wv t2230 = rt_get(&F_name, rt_idx(v1161_k, v1160_ms)); wv t2231 = rt_get(&F_field, v1149_e); rt_ne(t2230, t2231); }))) {
        v1161_k = rt_add(v1161_k, rt_i(1));
      }
      RT_RET(({ wv t2234 = ({ wv t2232 = rt_text(rt_get(&F_c, v1156_lin)); wv t2233 = rt_text(v1161_k); rt_cat(5, (wv[]){ rt_s(&S637), t2232, rt_s(&S98), t2233, rt_s(&S508) }); }); wv t2235 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2234, t2235 }); }));
    }
    wv v1157_lib = rt_bind(rt_chk(s864_stolenFrom(up, rt_get(&F_rec, v1149_e)), "expr"), 0);
    if (rt_cond(rt_ne(v1157_lib, RT_NULL))) {
      RT_RET(({ wv t2238 = ({ wv t2236 = rt_text(rt_chk(s843_modId(up, v1157_lib), "expr")); wv t2237 = rt_text(rt_get(&F_field, v1149_e)); rt_cat(4, (wv[]){ rt_s(&S478), t2236, rt_s(&S12), t2237 }); }); wv t2239 = rt_def(w7_pureK, "pureK"); rt_rec(0, 2, K55, (wv[]){ t2238, t2239 }); }));
    }
    wv v1158_key = ({ wv t2240 = rt_chk(s862_fieldRef(up, rt_get(&F_field, v1149_e)), "expr"); wv t2241 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2240, t2241 }); });
    wv v1159_sq = rt_bind(rt_chk(s870_seq(up, ({ wv t2242 = v1158_key; wv t2243 = rt_chk(s900_expr(up, rt_get(&F_rec, v1149_e)), "expr"); rt_arr(2, (wv[]){ t2242, t2243 }, 0); })), "expr"), 0);
    RT_RET(({ wv t2246 = v1159_sq; wv t2247 = ({ wv t2244 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1159_sq))); wv t2245 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1159_sq))); rt_cat(5, (wv[]){ rt_s(&S638), t2244, rt_s(&S385), t2245, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2246, t2247), "expr"); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S190)))) {
    RT_RET(({ wv t2250 = ({ wv t2248 = rt_text(rt_chk(w7_unFn(rt_get(&F_op, v1149_e)), "expr")); wv t2249 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_e, v1149_e)), "expr"))); rt_cat(5, (wv[]){ rt_s(&S608), t2248, rt_s(&S97), t2249, rt_s(&S16) }); }); wv t2251 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2250, t2251 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S191)))) {
    if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_op, v1149_e), rt_s(&S80)), "||") || rt_bool(rt_eq(rt_get(&F_op, v1149_e), rt_s(&S81)), "||")))) {
      RT_RET(({ wv t2257 = ({ wv t2252 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_l, v1149_e)), "expr"))); wv t2253 = rt_text(rt_get(&F_op, v1149_e)); wv t2254 = rt_text(rt_get(&F_op, v1149_e)); wv t2255 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_r, v1149_e)), "expr"))); wv t2256 = rt_text(rt_get(&F_op, v1149_e)); rt_cat(11, (wv[]){ rt_s(&S639), t2252, rt_s(&S556), t2253, rt_s(&S557), t2254, rt_s(&S640), t2255, rt_s(&S556), t2256, rt_s(&S559) }); }); wv t2258 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2257, t2258 }); }));
    }
    wv v1162_sq = rt_bind(rt_chk(s870_seq(up, ({ wv t2259 = rt_chk(s900_expr(up, rt_get(&F_l, v1149_e)), "expr"); wv t2260 = rt_chk(s900_expr(up, rt_get(&F_r, v1149_e)), "expr"); rt_arr(2, (wv[]){ t2259, t2260 }, 0); })), "expr"), 0);
    RT_RET(({ wv t2264 = v1162_sq; wv t2265 = ({ wv t2261 = rt_text(rt_chk(w7_binFn(rt_get(&F_op, v1149_e)), "expr")); wv t2262 = rt_text(rt_idx(rt_i(0), rt_get(&F_cs, v1162_sq))); wv t2263 = rt_text(rt_idx(rt_i(1), rt_get(&F_cs, v1162_sq))); rt_cat(7, (wv[]){ rt_s(&S608), t2261, rt_s(&S97), t2262, rt_s(&S385), t2263, rt_s(&S16) }); }); rt_chk(s871_wrap(up, t2264, t2265), "expr"); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S192)))) {
    RT_RET(({ wv t2269 = ({ wv t2266 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_c, v1149_e)), "expr"))); wv t2267 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_a, v1149_e)), "expr"))); wv t2268 = rt_text(rt_get(&F_c, rt_chk(s900_expr(up, rt_get(&F_b, v1149_e)), "expr"))); rt_cat(7, (wv[]){ rt_s(&S641), t2266, rt_s(&S561), t2267, rt_s(&S533), t2268, rt_s(&S16) }); }); wv t2270 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2269, t2270 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S182)))) {
    wv v1163_sq = rt_bind(rt_chk(s870_seq(up, rt_chk(s882_exprs(up, rt_get(&F_args, v1149_e)), "expr")), "expr"), 0);
    RT_RET(({ wv t2273 = v1163_sq; wv t2274 = ({ wv t2271 = rt_get(&F_callee, v1149_e); wv t2272 = rt_get(&F_cs, v1163_sq); rt_chk(s899_callC(up, t2271, t2272, rt_b(1)), "expr"); }); rt_chk(s871_wrap(up, t2273, t2274), "expr"); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S184)))) {
    RT_RET(({ wv t2275 = rt_chk(s894_chainExpr(up, v1149_e), "expr"); wv t2276 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2275, t2276 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S44)))) {
    RT_RET(({ wv t2277 = rt_chk(s898_matchExpr(up, v1149_e), "expr"); wv t2278 = rt_def(w7_effectK, "effectK"); rt_rec(0, 2, K55, (wv[]){ t2277, t2278 }); }));
  }
  if (rt_cond(rt_eq(v1150_kind, rt_s(&S528)))) {
    RT_RET(({ wv t2279 = rt_get(&F_js, v1149_e); wv t2280 = rt_def(w7_constK, "constK"); rt_rec(0, 2, K55, (wv[]){ t2279, t2280 }); }));
  }
  rt_panic(rt_cat(2, (wv[]){ rt_s(&S562), rt_text(v1150_kind) }));
  RT_RET(RT_NULL);
}

static wv s901_assemble(struct fr183 *up) {
  rt_enter("assemble");
  wv v1164_all = rt_arr(3, (wv[]){ std_join(std_split(up->v819_runtime, rt_s(&S642)), rt_s(&S2)), rt_s(&S643), rt_s(&S13) }, 1);
  wv v1165_k = rt_i(0);
  while (rt_cond(({ wv t2281 = v1165_k; wv t2282 = std_length(rt_def(up->v822_strings, "strings")); rt_lt(t2281, t2282); }))) {
    (void)({ wv t2287 = v1164_all; wv t2288 = ({ wv t2285 = rt_text(v1165_k); wv t2286 = rt_text(rt_chk(w7_wstrInit(({ wv t2283 = v1165_k; wv t2284 = rt_def(up->v822_strings, "strings"); rt_idx(t2283, t2284); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S644), t2285, rt_s(&S495), t2286, rt_s(&S14) }); }); std_push(t2287, t2288); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2289 = v1165_k; wv t2290 = std_length(rt_def(up->v823_fields, "fields")); rt_lt(t2289, t2290); }))) {
    (void)({ wv t2297 = v1164_all; wv t2298 = ({ wv t2295 = rt_text(({ wv t2291 = v1165_k; wv t2292 = rt_def(up->v823_fields, "fields"); rt_idx(t2291, t2292); })); wv t2296 = rt_text(rt_chk(w7_wstrInit(({ wv t2293 = v1165_k; wv t2294 = rt_def(up->v823_fields, "fields"); rt_idx(t2293, t2294); })), "assemble")); rt_cat(5, (wv[]){ rt_s(&S645), t2295, rt_s(&S495), t2296, rt_s(&S14) }); }); std_push(t2297, t2298); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2299 = v1165_k; wv t2300 = std_length(rt_def(up->v824_shapes, "shapes")); rt_lt(t2299, t2300); }))) {
    wv v1166_names = rt_bind(std_split(({ wv t2301 = v1165_k; wv t2302 = rt_def(up->v824_shapes, "shapes"); rt_idx(t2301, t2302); }), rt_s(&S96)), 0);
    wv v1167_refs = rt_arr(0, NULL, 1);
    wv v1168_j = rt_i(0);
    while (rt_cond(({ wv t2303 = v1168_j; wv t2304 = std_length(v1166_names); rt_lt(t2303, t2304); }))) {
      (void)({ wv t2305 = v1167_refs; wv t2306 = rt_cat(2, (wv[]){ rt_s(&S575), rt_text(rt_idx(v1168_j, v1166_names)) }); std_push(t2305, t2306); });
      v1168_j = rt_add(v1168_j, rt_i(1));
    }
    (void)({ wv t2309 = v1164_all; wv t2310 = ({ wv t2307 = rt_text(v1165_k); wv t2308 = rt_text(std_join(v1167_refs, rt_s(&S385))); rt_cat(5, (wv[]){ rt_s(&S646), t2307, rt_s(&S577), t2308, rt_s(&S578) }); }); std_push(t2309, t2310); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2311 = v1165_k; wv t2312 = std_length(rt_def(up->v825_enumDefs, "enumDefs")); rt_lt(t2311, t2312); }))) {
    (void)({ wv t2315 = v1164_all; wv t2316 = ({ wv t2313 = v1165_k; wv t2314 = rt_def(up->v825_enumDefs, "enumDefs"); rt_idx(t2313, t2314); }); std_push(t2315, t2316); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2317 = v1165_k; wv t2318 = std_length(rt_def(up->v827_frames, "frames")); rt_lt(t2317, t2318); }))) {
    wv v1169_f = rt_bind(({ wv t2319 = v1165_k; wv t2320 = rt_def(up->v827_frames, "frames"); rt_idx(t2319, t2320); }), 0);
    wv v1170_members = rt_arr(0, NULL, 1);
    wv v1171_pf = rt_bind(rt_get(&F_parent, ({ wv t2321 = v1169_f; wv t2322 = rt_def(up->v834_fns, "fns"); rt_idx(t2321, t2322); })), 0);
    if (rt_cond(rt_not(rt_get(&F_top, ({ wv t2323 = v1171_pf; wv t2324 = rt_def(up->v834_fns, "fns"); rt_idx(t2323, t2324); }))))) {
      (void)({ wv t2325 = v1170_members; wv t2326 = rt_cat(3, (wv[]){ rt_s(&S598), rt_text(v1171_pf), rt_s(&S647) }); std_push(t2325, t2326); });
    }
    wv v1172_j = rt_i(0);
    while (rt_cond(({ wv t2327 = v1172_j; wv t2328 = std_length(rt_def(up->v833_decls, "decls")); rt_lt(t2327, t2328); }))) {
      if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(({ wv t2331 = rt_get(&F_func, ({ wv t2329 = v1172_j; wv t2330 = rt_def(up->v833_decls, "decls"); rt_idx(t2329, t2330); })); wv t2332 = v1169_f; rt_eq(t2331, t2332); }), "&&") && rt_bool(rt_get(&F_captured, ({ wv t2333 = v1172_j; wv t2334 = rt_def(up->v833_decls, "decls"); rt_idx(t2333, t2334); })), "&&")), "&&") && rt_bool(rt_ne(rt_get(&F_kind, ({ wv t2335 = v1172_j; wv t2336 = rt_def(up->v833_decls, "decls"); rt_idx(t2335, t2336); })), rt_s(&S33)), "&&")))) {
        (void)({ wv t2339 = v1170_members; wv t2340 = rt_cat(3, (wv[]){ rt_s(&S584), rt_text(rt_get(&F_cname, ({ wv t2337 = v1172_j; wv t2338 = rt_def(up->v833_decls, "decls"); rt_idx(t2337, t2338); }))), rt_s(&S14) }); std_push(t2339, t2340); });
      }
      v1172_j = rt_add(v1172_j, rt_i(1));
    }
    if (rt_cond(rt_eq(std_length(v1170_members), rt_i(0)))) {
      (void)std_push(v1170_members, rt_s(&S648));
    }
    (void)({ wv t2343 = v1164_all; wv t2344 = ({ wv t2341 = rt_text(v1169_f); wv t2342 = rt_text(std_join(v1170_members, rt_s(&S0))); rt_cat(5, (wv[]){ rt_s(&S598), t2341, rt_s(&S649), t2342, rt_s(&S578) }); }); std_push(t2343, t2344); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2345 = v1165_k; wv t2346 = std_length(rt_def(up->v826_globals, "globals")); rt_lt(t2345, t2346); }))) {
    (void)({ wv t2349 = v1164_all; wv t2350 = rt_cat(3, (wv[]){ rt_s(&S601), rt_text(({ wv t2347 = v1165_k; wv t2348 = rt_def(up->v826_globals, "globals"); rt_idx(t2347, t2348); })), rt_s(&S14) }); std_push(t2349, t2350); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2351 = v1165_k; wv t2352 = std_length(rt_def(up->v828_functions, "functions")); rt_lt(t2351, t2352); }))) {
    (void)({ wv t2355 = v1164_all; wv t2356 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2353 = v1165_k; wv t2354 = rt_def(up->v828_functions, "functions"); rt_idx(t2353, t2354); }))), rt_s(&S14) }); std_push(t2355, t2356); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2357 = v1165_k; wv t2358 = std_length(rt_def(up->v828_functions, "functions")); rt_lt(t2357, t2358); }))) {
    (void)std_push(v1164_all, rt_s(&S13));
    (void)({ wv t2361 = v1164_all; wv t2362 = rt_cat(2, (wv[]){ rt_text(rt_get(&F_proto, ({ wv t2359 = v1165_k; wv t2360 = rt_def(up->v828_functions, "functions"); rt_idx(t2359, t2360); }))), rt_s(&S650) }); std_push(t2361, t2362); });
    wv v1173_lines = rt_bind(rt_get(&F_lines, ({ wv t2363 = v1165_k; wv t2364 = rt_def(up->v828_functions, "functions"); rt_idx(t2363, t2364); })), 0);
    wv v1174_j = rt_i(0);
    while (rt_cond(({ wv t2365 = v1174_j; wv t2366 = std_length(v1173_lines); rt_lt(t2365, t2366); }))) {
      (void)({ wv t2367 = v1164_all; wv t2368 = rt_idx(v1174_j, v1173_lines); std_push(t2367, t2368); });
      v1174_j = rt_add(v1174_j, rt_i(1));
    }
    (void)std_push(v1164_all, rt_s(&S15));
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  (void)std_push(v1164_all, rt_s(&S13));
  (void)std_push(v1164_all, rt_s(&S651));
  if (rt_cond(rt_gt(std_length(rt_def(up->v823_fields, "fields")), rt_i(0)))) {
    wv v1175_refs = rt_arr(0, NULL, 1);
    v1165_k = rt_i(0);
    while (rt_cond(({ wv t2369 = v1165_k; wv t2370 = std_length(rt_def(up->v823_fields, "fields")); rt_lt(t2369, t2370); }))) {
      (void)({ wv t2373 = v1175_refs; wv t2374 = rt_cat(2, (wv[]){ rt_s(&S575), rt_text(({ wv t2371 = v1165_k; wv t2372 = rt_def(up->v823_fields, "fields"); rt_idx(t2371, t2372); })) }); std_push(t2373, t2374); });
      v1165_k = rt_add(v1165_k, rt_i(1));
    }
    (void)({ wv t2377 = v1164_all; wv t2378 = ({ wv t2375 = rt_text(std_length(rt_def(up->v823_fields, "fields"))); wv t2376 = rt_text(std_join(v1175_refs, rt_s(&S385))); rt_cat(5, (wv[]){ rt_s(&S652), t2375, rt_s(&S653), t2376, rt_s(&S654) }); }); std_push(t2377, t2378); });
  }
  v1165_k = rt_i(0);
  while (rt_cond(({ wv t2379 = v1165_k; wv t2380 = std_length(rt_def(up->v829_out, "out")); rt_lt(t2379, t2380); }))) {
    (void)({ wv t2383 = v1164_all; wv t2384 = ({ wv t2381 = v1165_k; wv t2382 = rt_def(up->v829_out, "out"); rt_idx(t2381, t2382); }); std_push(t2383, t2384); });
    v1165_k = rt_add(v1165_k, rt_i(1));
  }
  (void)std_push(v1164_all, rt_s(&S15));
  (void)std_push(v1164_all, rt_s(&S13));
  RT_RET(std_join(v1164_all, rt_s(&S2)));
}

static wv w8_dumpTokens(wv v1178_toks, wv v1179_indent) {
  rt_enter("dumpTokens");
  v1178_toks = rt_bind(v1178_toks, 0);
  v1179_indent = rt_bind(v1179_indent, 0);
  wv v1180_k = rt_i(0);
  while (rt_cond(({ wv t2395 = v1180_k; wv t2396 = std_length(v1178_toks); rt_lt(t2395, t2396); }))) {
    wv v1181_tk = rt_bind(rt_idx(v1180_k, v1178_toks), 0);
    wv v1182_line = ({ wv t2397 = rt_text(v1179_indent); wv t2398 = rt_text(rt_get(&F_t, v1181_tk)); wv t2399 = rt_text(rt_get(&F_pos, v1181_tk)); wv t2400 = rt_text(rt_get(&F_end, v1181_tk)); wv t2401 = rt_text(rt_get(&F_sp, v1181_tk)); rt_cat(8, (wv[]){ t2397, t2398, rt_s(&S0), t2399, rt_s(&S0), t2400, rt_s(&S0), t2401 }); });
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1181_tk), rt_s(&S137)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1181_tk), rt_s(&S156)), "||")), "||") || rt_bool(rt_eq(rt_get(&F_t, v1181_tk), rt_s(&S111)), "||")))) {
      v1182_line = ({ wv t2402 = v1182_line; wv t2403 = rt_cat(2, (wv[]){ rt_s(&S0), rt_text(rt_get(&F_v, v1181_tk)) }); rt_add(t2402, t2403); });
    } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_get(&F_t, v1181_tk), rt_s(&S138)), "||") || rt_bool(rt_eq(rt_get(&F_t, v1181_tk), rt_s(&S148)), "||")))) {
      v1182_line = ({ wv t2406 = v1182_line; wv t2407 = ({ wv t2404 = rt_text(rt_get(&F_text, v1181_tk)); wv t2405 = rt_text(rt_get(&F_v, v1181_tk)); rt_cat(4, (wv[]){ rt_s(&S0), t2404, rt_s(&S0), t2405 }); }); rt_add(t2406, t2407); });
    }
    (void)std_print(v1182_line);
    if (rt_cond(rt_eq(rt_get(&F_t, v1181_tk), rt_s(&S171)))) {
      wv v1183_p = rt_i(0);
      while (rt_cond(({ wv t2408 = v1183_p; wv t2409 = std_length(rt_get(&F_parts, v1181_tk)); rt_lt(t2408, t2409); }))) {
        wv v1184_part = rt_bind(({ wv t2410 = v1183_p; wv t2411 = rt_get(&F_parts, v1181_tk); rt_idx(t2410, t2411); }), 0);
        if (rt_cond(rt_eq(std_typeOf(v1184_part), rt_s(&S185)))) {
          (void)std_print(({ wv t2412 = rt_text(v1179_indent); wv t2413 = rt_text(rt_chk(w1_quote(v1184_part), "dumpTokens")); rt_cat(3, (wv[]){ t2412, rt_s(&S655), t2413 }); }));
        } else {
          (void)std_print(({ wv t2414 = rt_text(v1179_indent); wv t2415 = rt_text(rt_get(&F_pos, v1184_part)); rt_cat(3, (wv[]){ t2414, rt_s(&S656), t2415 }); }));
          (void)({ wv t2416 = rt_get(&F_tokens, v1184_part); wv t2417 = rt_add(v1179_indent, rt_s(&S657)); rt_chk(w8_dumpTokens(t2416, t2417), "dumpTokens"); });
          (void)std_print(rt_cat(2, (wv[]){ rt_text(v1179_indent), rt_s(&S658) }));
        }
        v1183_p = rt_add(v1183_p, rt_i(1));
      }
    }
    v1180_k = rt_add(v1180_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv w8_sorted(wv v1185_xs) {
  rt_enter("sorted");
  v1185_xs = rt_bind(v1185_xs, 0);
  wv v1186_out = rt_arr(0, NULL, 1);
  wv v1187_k = rt_i(0);
  while (rt_cond(({ wv t2418 = v1187_k; wv t2419 = std_length(v1185_xs); rt_lt(t2418, t2419); }))) {
    (void)({ wv t2420 = v1186_out; wv t2421 = rt_idx(v1187_k, v1185_xs); std_push(t2420, t2421); });
    wv v1188_j = rt_sub(std_length(v1186_out), rt_i(1));
    while (rt_cond(rt_b(rt_bool(rt_gt(v1188_j, rt_i(0)), "&&") && rt_bool(({ wv t2424 = ({ wv t2422 = rt_sub(v1188_j, rt_i(1)); wv t2423 = v1186_out; rt_idx(t2422, t2423); }); wv t2425 = rt_idx(v1188_j, v1186_out); rt_gt(t2424, t2425); }), "&&")))) {
      wv v1189_swap = rt_bind(rt_idx(v1188_j, v1186_out), 0);
      ({ wv t2428 = ({ wv t2426 = rt_sub(v1188_j, rt_i(1)); wv t2427 = v1186_out; rt_idx(t2426, t2427); }); wv t2429 = v1188_j; wv t2430 = v1186_out; rt_set_idx(t2428, t2429, t2430); });
      ({ wv t2431 = v1189_swap; wv t2432 = rt_sub(v1188_j, rt_i(1)); wv t2433 = v1186_out; rt_set_idx(t2431, t2432, t2433); });
      v1188_j = rt_sub(v1188_j, rt_i(1));
    }
    v1187_k = rt_add(v1187_k, rt_i(1));
  }
  RT_RET(v1186_out);
}

static wv w8_dumpValue(wv v1190_x, wv v1191_indent) {
  rt_enter("dumpValue");
  v1190_x = rt_bind(v1190_x, 0);
  v1191_indent = rt_bind(v1191_indent, 0);
  wv v1192_type = rt_bind(std_typeOf(v1190_x), 0);
  if (rt_cond(rt_eq(v1192_type, rt_s(&S185)))) {
    RT_RET(rt_chk(w1_quote(v1190_x), "dumpValue"));
  }
  wv v1193_inner = rt_add(v1191_indent, rt_s(&S480));
  if (rt_cond(rt_eq(v1192_type, rt_s(&S659)))) {
    if (rt_cond(rt_eq(std_length(v1190_x), rt_i(0)))) {
      RT_RET(rt_s(&S660));
    }
    wv v1194_out = rt_s(&S98);
    wv v1195_k = rt_i(0);
    while (rt_cond(({ wv t2434 = v1195_k; wv t2435 = std_length(v1190_x); rt_lt(t2434, t2435); }))) {
      v1194_out = ({ wv t2440 = ({ wv t2436 = rt_add(v1194_out, rt_s(&S2)); wv t2437 = v1193_inner; rt_add(t2436, t2437); }); wv t2441 = ({ wv t2438 = rt_idx(v1195_k, v1190_x); wv t2439 = v1193_inner; rt_chk(w8_dumpValue(t2438, t2439), "dumpValue"); }); rt_add(t2440, t2441); });
      v1195_k = rt_add(v1195_k, rt_i(1));
    }
    RT_RET(rt_add(({ wv t2442 = rt_add(v1194_out, rt_s(&S2)); wv t2443 = v1191_indent; rt_add(t2442, t2443); }), rt_s(&S99)));
  }
  if (rt_cond(rt_eq(v1192_type, rt_s(&S661)))) {
    wv v1196_keys = rt_bind(rt_chk(w8_sorted(std_fields(v1190_x)), "dumpValue"), 0);
    wv v1197_out = rt_s(&S100);
    wv v1198_k = rt_i(0);
    while (rt_cond(({ wv t2444 = v1198_k; wv t2445 = std_length(v1196_keys); rt_lt(t2444, t2445); }))) {
      wv v1199_key = rt_bind(rt_idx(v1198_k, v1196_keys), 0);
      v1197_out = ({ wv t2452 = ({ wv t2448 = v1197_out; wv t2449 = ({ wv t2446 = rt_text(v1193_inner); wv t2447 = rt_text(v1199_key); rt_cat(4, (wv[]){ rt_s(&S2), t2446, t2447, rt_s(&S29) }); }); rt_add(t2448, t2449); }); wv t2453 = ({ wv t2450 = rt_idx(v1199_key, v1190_x); wv t2451 = v1193_inner; rt_chk(w8_dumpValue(t2450, t2451), "dumpValue"); }); rt_add(t2452, t2453); });
      v1198_k = rt_add(v1198_k, rt_i(1));
    }
    RT_RET(({ wv t2454 = v1197_out; wv t2455 = rt_cat(3, (wv[]){ rt_s(&S2), rt_text(v1191_indent), rt_s(&S15) }); rt_add(t2454, t2455); }));
  }
  RT_RET(std_toString(v1190_x));
}

static wv w9_dirOf(wv v1200_path) {
  rt_enter("dirOf");
  v1200_path = rt_bind(v1200_path, 0);
  wv v1201_cs = rt_bind(std_split(v1200_path, rt_s(&S13)), 0);
  wv v1202_k = rt_sub(std_length(v1201_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_ge(v1202_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1202_k, v1201_cs), rt_s(&S87)), "&&")))) {
    v1202_k = rt_sub(v1202_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2456 = v1201_cs; wv t2457 = rt_add(v1202_k, rt_i(1)); std_slice(t2456, rt_i(0), t2457); }), rt_s(&S13)));
}

static wv w9_normalize(wv v1203_path) {
  rt_enter("normalize");
  v1203_path = rt_bind(v1203_path, 0);
  wv v1204_segs = rt_bind(std_split(v1203_path, rt_s(&S87)), 0);
  wv v1205_out = rt_arr(0, NULL, 1);
  wv v1206_k = rt_i(0);
  while (rt_cond(({ wv t2458 = v1206_k; wv t2459 = std_length(v1204_segs); rt_lt(t2458, t2459); }))) {
    wv v1207_seg = rt_bind(rt_idx(v1206_k, v1204_segs), 0);
    v1206_k = rt_add(v1206_k, rt_i(1));
    if (rt_cond(rt_b(rt_bool(rt_eq(v1207_seg, rt_s(&S129)), "||") || rt_bool(rt_b(rt_bool(rt_eq(v1207_seg, rt_s(&S13)), "&&") && rt_bool(rt_gt(std_length(v1205_out), rt_i(0)), "&&")), "||")))) {
      continue;
    }
    if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1207_seg, rt_s(&S662)), "&&") && rt_bool(rt_gt(std_length(v1205_out), rt_i(0)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1205_out), rt_s(&S662)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(rt_i(-1), v1205_out), rt_s(&S13)), "&&")))) {
      (void)std_pop(v1205_out);
    } else {
      (void)std_push(v1205_out, v1207_seg);
    }
  }
  RT_RET(std_join(v1205_out, rt_s(&S87)));
}

static wv w9_with(wv v1208_xs, wv v1209_x) {
  rt_enter("with");
  v1208_xs = rt_bind(v1208_xs, 0);
  v1209_x = rt_bind(v1209_x, 0);
  wv v1210_out = rt_arr(0, NULL, 1);
  wv v1211_k = rt_i(0);
  while (rt_cond(({ wv t2460 = v1211_k; wv t2461 = std_length(v1208_xs); rt_lt(t2460, t2461); }))) {
    (void)({ wv t2462 = v1210_out; wv t2463 = rt_idx(v1211_k, v1208_xs); std_push(t2462, t2463); });
    v1211_k = rt_add(v1211_k, rt_i(1));
  }
  (void)std_push(v1210_out, v1209_x);
  RT_RET(v1210_out);
}

static wv w9_failIn(wv v1212_file, wv v1213_chars, wv v1214_pos, wv v1215_message) {
  rt_enter("failIn");
  v1212_file = rt_bind(v1212_file, 0);
  v1213_chars = rt_bind(v1213_chars, 0);
  v1214_pos = rt_bind(v1214_pos, 0);
  v1215_message = rt_bind(v1215_message, 0);
  (void)rt_chk(w2_setSource(v1212_file, v1213_chars), "failIn");
  (void)rt_chk(w2_abort(v1214_pos, v1215_message), "failIn");
  RT_RET(RT_NULL);
}

static wv w9_loadProgram(wv v1216_entryFile, wv v1217_entryChars) {
  rt_enter("loadProgram");
  struct fr252 fr = { 0 };
  v1216_entryFile = rt_bind(v1216_entryFile, 0);
  fr.v1218_modules = rt_arr(0, NULL, 1);
  wv v1221_path = rt_bind(rt_chk(w9_normalize(std_join(std_split(v1216_entryFile, rt_s(&S19)), rt_s(&S87))), "loadProgram"), 0);
  (void)({ wv t2514 = v1216_entryFile; wv t2515 = v1217_entryChars; wv t2516 = v1221_path; wv t2517 = rt_arr(1, (wv[]){ v1221_path }, 0); rt_chk(s1220_visit(&fr, t2514, t2515, t2516, t2517), "loadProgram"); });
  RT_RET(fr.v1218_modules);
}

static wv s1219_loaded(struct fr252 *up, wv v1222_path) {
  rt_enter("loaded");
  v1222_path = rt_bind(v1222_path, 0);
  wv v1223_k = rt_i(0);
  while (rt_cond(({ wv t2464 = v1223_k; wv t2465 = std_length(rt_def(up->v1218_modules, "modules")); rt_lt(t2464, t2465); }))) {
    if (rt_cond(({ wv t2468 = rt_get(&F_path, ({ wv t2466 = v1223_k; wv t2467 = rt_def(up->v1218_modules, "modules"); rt_idx(t2466, t2467); })); wv t2469 = v1222_path; rt_eq(t2468, t2469); }))) {
      RT_RET(v1223_k);
    }
    v1223_k = rt_add(v1223_k, rt_i(1));
  }
  RT_RET(RT_NULL);
}

static wv s1220_visit(struct fr252 *up, wv v1224_file, wv v1225_chars, wv v1226_path, wv v1227_chain) {
  rt_enter("visit");
  v1224_file = rt_bind(v1224_file, 0);
  v1225_chars = rt_bind(v1225_chars, 0);
  v1226_path = rt_bind(v1226_path, 0);
  v1227_chain = rt_bind(v1227_chain, 0);
  (void)rt_chk(w2_setSource(v1224_file, v1225_chars), "visit");
  wv v1228_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(v1225_chars), "visit")), "visit"), 0);
  wv v1229_imports = rt_arr(0, NULL, 1);
  wv v1230_k = rt_i(0);
  while (rt_cond(({ wv t2470 = v1230_k; wv t2471 = std_length(rt_get(&F_stmts, v1228_program)); rt_lt(t2470, t2471); }))) {
    wv v1233_s = rt_bind(({ wv t2472 = v1230_k; wv t2473 = rt_get(&F_stmts, v1228_program); rt_idx(t2472, t2473); }), 0);
    v1230_k = rt_add(v1230_k, rt_i(1));
    if (rt_cond(rt_ne(rt_get(&F_k, v1233_s), rt_s(&S47)))) {
      continue;
    }
    wv v1234_libPath = rt_bind(rt_chk(w9_normalize(({ wv t2474 = rt_chk(w9_dirOf(v1226_path), "visit"); wv t2475 = rt_get(&F_path, v1233_s); rt_add(t2474, t2475); })), "visit"), 0);
    wv v1235_at = rt_i(0);
    while (rt_cond(rt_b(rt_bool(({ wv t2476 = v1235_at; wv t2477 = std_length(v1227_chain); rt_lt(t2476, t2477); }), "&&") && rt_bool(({ wv t2478 = rt_idx(v1235_at, v1227_chain); wv t2479 = v1234_libPath; rt_ne(t2478, t2479); }), "&&")))) {
      v1235_at = rt_add(v1235_at, rt_i(1));
    }
    if (rt_cond(({ wv t2480 = v1235_at; wv t2481 = std_length(v1227_chain); rt_lt(t2480, t2481); }))) {
      wv v1237_cycle = rt_bind(({ wv t2485 = ({ wv t2482 = v1227_chain; wv t2483 = v1235_at; wv t2484 = std_length(v1227_chain); std_slice(t2482, t2483, t2484); }); wv t2486 = v1234_libPath; rt_chk(w9_with(t2485, t2486), "visit"); }), 0);
      (void)({ wv t2487 = v1224_file; wv t2488 = v1225_chars; wv t2489 = rt_get(&F_pathPos, v1233_s); wv t2490 = rt_cat(2, (wv[]){ rt_s(&S663), rt_text(std_join(v1237_cycle, rt_s(&S664))) }); rt_chk(w9_failIn(t2487, t2488, t2489, t2490), "visit"); });
    }
    wv v1236_lib = rt_bind(rt_chk(s1219_loaded(up, v1234_libPath), "visit"), 1);
    if (rt_cond(rt_eq(v1236_lib, RT_NULL))) {
      wv t2491 = std_readFile(v1234_libPath);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          (void)({ wv t2492 = v1224_file; wv t2493 = v1225_chars; wv t2494 = rt_get(&F_pathPos, v1233_s); wv t2495 = rt_cat(3, (wv[]){ rt_s(&S665), rt_text(v1234_libPath), rt_s(&S113) }); rt_chk(w9_failIn(t2492, t2493, t2494, t2495), "visit"); });
          t2491 = RT_NULL;
        }
      }
      wv v1238_text = rt_bind(t2491, 0);
      wv v1239_cs = rt_bind(std_split(v1238_text, rt_s(&S13)), 0);
      wv v1240_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1239_cs), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1239_cs), rt_s(&S666)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
      v1236_lib = rt_bind(({ wv t2499 = v1234_libPath; wv t2500 = ({ wv t2496 = v1239_cs; wv t2497 = v1240_bom; wv t2498 = std_length(v1239_cs); std_slice(t2496, t2497, t2498); }); wv t2501 = v1234_libPath; wv t2502 = rt_chk(w9_with(v1227_chain, v1234_libPath), "visit"); rt_chk(s1220_visit(up, t2499, t2500, t2501, t2502), "visit"); }), 1);
    }
    (void)({ wv t2503 = v1229_imports; wv t2504 = rt_rec(0, 2, K58, (wv[]){ v1233_s, v1236_lib }); std_push(t2503, t2504); });
  }
  wv v1231_refTargets = rt_arr(0, NULL, 1);
  wv v1232_mod = ({ wv t2505 = v1224_file; wv t2506 = v1225_chars; wv t2507 = v1226_path; wv t2508 = v1228_program; wv t2509 = v1229_imports; wv t2510 = rt_arr(0, NULL, 0); wv t2511 = v1231_refTargets; rt_rec(1, 8, K59, (wv[]){ t2505, t2506, t2507, t2508, t2509, t2510, rt_b(0), t2511 }); });
  (void)({ wv t2512 = rt_def(up->v1218_modules, "modules"); wv t2513 = v1232_mod; std_push(t2512, t2513); });
  RT_RET(rt_sub(std_length(rt_def(up->v1218_modules, "modules")), rt_i(1)));
}

static wv w_die(void) {
  rt_enter("die");
  (void)std_eprint(rt_def(w_usage, "usage"));
  rt_exit(rt_i(2));
  RT_RET(RT_NULL);
}

static wv w_load(wv v1241_file) {
  rt_enter("load");
  v1241_file = rt_bind(v1241_file, 0);
  wv t2518 = std_readFile(v1241_file);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S673), rt_text(v1241_file) }));
      rt_exit(rt_i(2));
      t2518 = RT_NULL;
    }
  }
  wv v1242_text = rt_bind(t2518, 0);
  wv v1243_chars = rt_bind(std_split(v1242_text, rt_s(&S13)), 0);
  wv v1244_bom = rt_bind((rt_cond(rt_b(rt_bool(rt_gt(std_length(v1243_chars), rt_i(0)), "&&") && rt_bool(rt_eq(rt_idx(rt_i(0), v1243_chars), rt_s(&S666)), "&&"))) ? rt_i(1) : rt_i(0)), 0);
  (void)({ wv t2522 = v1241_file; wv t2523 = ({ wv t2519 = v1243_chars; wv t2520 = v1244_bom; wv t2521 = std_length(v1243_chars); std_slice(t2519, t2520, t2521); }); rt_chk(w2_setSource(t2522, t2523), "load"); });
  RT_RET(RT_NULL);
}

static wv w_onWindows(void) {
  rt_enter("onWindows");
  wv t2528 = rt_s(&S683);
  wv t2529 = std_env(t2528);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2529 = rt_s(&S13);
    }
  }
  wv v1246_os = rt_bind(t2529, 0);
  RT_RET(rt_eq(v1246_os, rt_s(&S684)));
}

static wv w_tryRead(wv v1247_path) {
  rt_enter("tryRead");
  v1247_path = rt_bind(v1247_path, 0);
  wv t2536 = std_readFile(v1247_path);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      t2536 = RT_NULL;
    }
  }
  wv v1248_text = rt_bind(t2536, 0);
  RT_RET(v1248_text);
}

static wv w_programDir(void) {
  rt_enter("programDir");
  wv v1249_cs = rt_bind(std_split(std_programPath(), rt_s(&S13)), 0);
  wv v1250_k = rt_sub(std_length(v1249_cs), rt_i(1));
  while (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_ge(v1250_k, rt_i(0)), "&&") && rt_bool(rt_ne(rt_idx(v1250_k, v1249_cs), rt_s(&S87)), "&&")), "&&") && rt_bool(rt_ne(rt_idx(v1250_k, v1249_cs), rt_s(&S19)), "&&")))) {
    v1250_k = rt_sub(v1250_k, rt_i(1));
  }
  RT_RET(std_join(({ wv t2537 = v1249_cs; wv t2538 = rt_add(v1250_k, rt_i(1)); std_slice(t2537, rt_i(0), t2538); }), rt_s(&S13)));
}

static wv w_readRuntime(void) {
  rt_enter("readRuntime");
  wv v1251_text = rt_bind(rt_chk(w_tryRead(rt_def(w_runtimePath, "runtimePath")), "readRuntime"), 0);
  if (rt_cond(rt_b(rt_bool(rt_ne(v1251_text, RT_NULL), "||") || rt_bool(rt_def(w_runtimeGiven, "runtimeGiven"), "||")))) {
    RT_RET(v1251_text);
  }
  wv v1252_dir = rt_bind(rt_chk(w_programDir(), "readRuntime"), 0);
  wv v1253_here = rt_bind(rt_chk(w_tryRead(({ wv t2539 = v1252_dir; wv t2540 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2539, t2540); })), "readRuntime"), 0);
  RT_RET((rt_cond(rt_ne(v1253_here, RT_NULL)) ? v1253_here : rt_chk(w_tryRead(({ wv t2541 = rt_add(v1252_dir, rt_s(&S692)); wv t2542 = rt_def(w_runtimePath, "runtimePath"); rt_add(t2541, t2542); })), "readRuntime")));
}

static void wvt_main(void) {
  rt_syms(88, (wstr *const[]){ &F_severity, &F_pos, &F_message, &F_hint, &F_file, &F_chars, &F_word, &F_t, &F_end, &F_sp, &F_nl, &F_v, &F_text, &F_parts, &F_tokens, &F_k, &F_e, &F_handler, &F_items, &F_fields, &F_index, &F_arr, &F_rec, &F_l, &F_r, &F_c, &F_a, &F_b, &F_stmts, &F_param, &F_paramPos, &F_body, &F_neg, &F_cond, &F_isVar, &F_isRef, &F_name, &F_namePos, &F_op, &F_target, &F_code, &F_arg, &F_paren, &F_path, &F_pathPos, &F_names, &F_params, &F_members, &F_value, &F_close, &F_field, &F_callee, &F_args, &F_values, &F_subject, &F_cases, &F_els, &F_branches, &F_arity, &F_mod, &F_parent, &F_kind, &F_visibleAt, &F_fnId, &F_lib, &F_id, &F_func, &F_top, &F_loops, &F_key, &F_exports, &F_refTargets, &F_useMod, &F_imports, &F_node, &F_notLibrary, &F_program, &F_js, &F_children, &F_d, &F_depth, &F_cname, &F_captured, &F_fnIndex, &F_pre, &F_cs, &F_proto, &F_lines });
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
  w5_stdlib = ({ wv t534 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S324), rt_i(1) }); wv t535 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S325), rt_i(1) }); wv t536 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S326), rt_i(1) }); wv t537 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S327), rt_i(0) }); wv t538 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S328), rt_i(1) }); wv t539 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S329), rt_i(2) }); wv t540 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S330), rt_i(2) }); wv t541 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S331), rt_i(1) }); wv t542 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S332), rt_i(1) }); wv t543 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S333), rt_i(1) }); wv t544 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S334), rt_s(&S35) }); wv t545 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S335), rt_i(1) }); wv t546 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S336), rt_s(&S35) }); wv t547 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S337), rt_i(2) }); wv t548 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S338), rt_i(1) }); wv t549 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S339), rt_i(2) }); wv t550 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S340), rt_i(3) }); wv t551 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S341), rt_i(1) }); wv t552 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S342), rt_i(1) }); wv t553 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S343), rt_i(2) }); wv t554 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S344), rt_i(2) }); wv t555 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S345), rt_i(1) }); wv t556 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S346), rt_i(1) }); wv t557 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S347), rt_i(1) }); wv t558 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S348), rt_i(2) }); wv t559 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S349), rt_i(1) }); wv t560 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S350), rt_i(2) }); wv t561 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S351), rt_i(1) }); wv t562 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S352), rt_i(1) }); wv t563 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S353), rt_i(1) }); wv t564 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S354), rt_i(1) }); wv t565 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S355), rt_i(1) }); wv t566 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S356), rt_i(1) }); wv t567 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S357), rt_i(1) }); wv t568 = rt_rec(0, 2, K39, (wv[]){ rt_s(&S358), rt_i(1) }); rt_arr(35, (wv[]){ t534, t535, t536, t537, t538, t539, t540, t541, t542, t543, t544, t545, t546, t547, t548, t549, t550, t551, t552, t553, t554, t555, t556, t557, t558, t559, t560, t561, t562, t563, t564, t565, t566, t567, t568 }, 0); });
  w5_neverCompletes = rt_arr(6, (wv[]){ rt_s(&S34), rt_s(&S57), rt_s(&S54), rt_s(&S55), rt_s(&S42), rt_s(&S43) }, 0);
  w5_onfailValueUsed = rt_arr(5, (wv[]){ rt_s(&S177), rt_s(&S38), rt_s(&S34), rt_s(&S57), rt_s(&S54) }, 0);
  w5_libraryTop = rt_arr(6, (wv[]){ rt_s(&S200), rt_s(&S47), rt_s(&S33), rt_s(&S177), rt_s(&S46), rt_s(&S203) }, 0);
  w6_binOps = rt_arr(17, (wv[]){ rt_s(&S84), rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S75), rt_s(&S76), rt_s(&S77), rt_s(&S89), rt_s(&S90), rt_s(&S78), rt_s(&S79), rt_s(&S91), rt_s(&S92), rt_s(&S32), rt_s(&S82), rt_s(&S83) }, 0);
  w6_binFns = rt_arr(17, (wv[]){ rt_s(&S458), rt_s(&S459), rt_s(&S460), rt_s(&S461), rt_s(&S462), rt_s(&S463), rt_s(&S464), rt_s(&S465), rt_s(&S466), rt_s(&S467), rt_s(&S468), rt_s(&S469), rt_s(&S470), rt_s(&S471), rt_s(&S472), rt_s(&S473), rt_s(&S474) }, 0);
  w6_scalarKinds = rt_arr(9, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S171), rt_s(&S281), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S191), rt_s(&S190) }, 0);
  w6_plainKinds = rt_arr(8, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S281), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S211), rt_s(&S156) }, 0);
  w7_binOps = rt_arr(17, (wv[]){ rt_s(&S84), rt_s(&S85), rt_s(&S86), rt_s(&S87), rt_s(&S88), rt_s(&S75), rt_s(&S76), rt_s(&S77), rt_s(&S89), rt_s(&S90), rt_s(&S78), rt_s(&S79), rt_s(&S91), rt_s(&S92), rt_s(&S32), rt_s(&S82), rt_s(&S83) }, 0);
  w7_binFns = rt_arr(17, (wv[]){ rt_s(&S458), rt_s(&S459), rt_s(&S460), rt_s(&S461), rt_s(&S462), rt_s(&S463), rt_s(&S464), rt_s(&S465), rt_s(&S466), rt_s(&S467), rt_s(&S468), rt_s(&S469), rt_s(&S470), rt_s(&S471), rt_s(&S472), rt_s(&S473), rt_s(&S474) }, 0);
  w7_scalarKinds = rt_arr(9, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S171), rt_s(&S281), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S191), rt_s(&S190) }, 0);
  w7_plainKinds = rt_arr(8, (wv[]){ rt_s(&S138), rt_s(&S148), rt_s(&S281), rt_s(&S51), rt_s(&S52), rt_s(&S53), rt_s(&S211), rt_s(&S156) }, 0);
  w7_signalers = rt_arr(10, (wv[]){ rt_s(&S327), rt_s(&S328), rt_s(&S329), rt_s(&S330), rt_s(&S332), rt_s(&S333), rt_s(&S335), rt_s(&S337), rt_s(&S346), rt_s(&S347) }, 0);
  w7_constK = rt_i(0);
  w7_pureK = rt_i(1);
  w7_effectK = rt_i(2);
  w_usage = rt_add(rt_add(rt_add(rt_add(rt_add(rt_s(&S667), rt_s(&S668)), rt_s(&S669)), rt_s(&S670)), rt_s(&S671)), rt_s(&S672));
  w_argv = rt_bind(std_args(), 0);
  w_mode = rt_s(&S13);
  w_input = rt_s(&S13);
  w_output = rt_s(&S13);
  w_target = rt_s(&S13);
  w_runtimePath = rt_s(&S13);
  w_cc = rt_s(&S13);
  w_arg = rt_i(0);
  while (rt_cond(({ wv t2524 = w_arg; wv t2525 = std_length(w_argv); rt_lt(t2524, t2525); }))) {
    v1245_a = rt_bind(rt_idx(w_arg, w_argv), 0);
    if (rt_cond(rt_b(rt_bool(rt_eq(w_arg, rt_i(0)), "&&") && rt_bool(rt_eq(v1245_a, rt_s(&S674)), "&&")))) {
      w_mode = rt_bind(v1245_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_eq(v1245_a, rt_s(&S675)), "||") || rt_bool(rt_eq(v1245_a, rt_s(&S676)), "||")))) {
      w_mode = rt_bind(v1245_a, 1);
    } else if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(v1245_a, rt_s(&S677)), "||") || rt_bool(rt_eq(v1245_a, rt_s(&S678)), "||")), "||") || rt_bool(rt_eq(v1245_a, rt_s(&S679)), "||")), "||") || rt_bool(rt_eq(v1245_a, rt_s(&S680)), "||")))) {
      w_arg = rt_add(w_arg, rt_i(1));
      if (rt_cond(({ wv t2526 = w_arg; wv t2527 = std_length(w_argv); rt_ge(t2526, t2527); }))) {
        (void)rt_chk(w_die(), NULL);
      }
      if (rt_cond(rt_eq(v1245_a, rt_s(&S677)))) {
        w_output = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1245_a, rt_s(&S678)))) {
        w_runtimePath = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_eq(v1245_a, rt_s(&S680)))) {
        w_cc = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else if (rt_cond(rt_b(rt_bool(rt_eq(rt_idx(w_arg, w_argv), rt_s(&S681)), "||") || rt_bool(rt_eq(rt_idx(w_arg, w_argv), rt_s(&S682)), "||")))) {
        w_target = rt_bind(rt_idx(w_arg, w_argv), 1);
      } else {
        (void)rt_chk(w_die(), NULL);
      }
    } else if (rt_cond(rt_b(rt_bool(rt_ne(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(rt_gt(std_length(v1245_a), rt_i(0)), "&&") && rt_bool(rt_eq(std_at(v1245_a, rt_i(0)), rt_s(&S85)), "&&")), "||")))) {
      (void)rt_chk(w_die(), NULL);
    } else {
      w_input = rt_bind(v1245_a, 1);
    }
    w_arg = rt_add(w_arg, rt_i(1));
  }
  w_inspect = rt_b(rt_bool(rt_eq(w_mode, rt_s(&S675)), "||") || rt_bool(rt_eq(w_mode, rt_s(&S676)), "||"));
  if (rt_cond(rt_b(rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_eq(w_input, rt_s(&S13)), "||") || rt_bool(rt_b(rt_bool(w_inspect, "&&") && rt_bool(rt_b(rt_bool(rt_b(rt_bool(rt_ne(w_output, rt_s(&S13)), "||") || rt_bool(rt_ne(w_runtimePath, rt_s(&S13)), "||")), "||") || rt_bool(rt_ne(w_target, rt_s(&S13)), "||")), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_eq(w_mode, rt_s(&S674)), "&&") && rt_bool(rt_ne(w_target, rt_s(&S13)), "&&")), "||")), "||") || rt_bool(rt_b(rt_bool(rt_ne(w_mode, rt_s(&S674)), "&&") && rt_bool(rt_ne(w_cc, rt_s(&S13)), "&&")), "||")))) {
    (void)rt_chk(w_die(), NULL);
  }
  w_dotWvt = rt_b(rt_bool(rt_ge(std_length(w_input), rt_i(4)), "&&") && rt_bool(rt_eq(({ wv t2530 = w_input; wv t2531 = std_length(w_input); std_slice(t2530, rt_i(-4), t2531); }), rt_s(&S231)), "&&"));
  if (rt_cond(rt_eq(w_mode, rt_s(&S674)))) {
    w_target = rt_s(&S682);
    if (rt_cond(rt_eq(w_cc, rt_s(&S13)))) {
      wv t2532 = rt_s(&S685);
      wv t2533 = std_env(t2532);
      if (rt_failcode) {
        rt_failcode = 0;
        {
          t2533 = rt_s(&S686);
        }
      }
      w_cc = rt_bind(t2533, 1);
    }
    if (rt_cond(rt_b(rt_bool(rt_eq(w_output, rt_s(&S13)), "&&") && rt_bool(rt_not(w_dotWvt), "&&")))) {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S687), rt_text(w_input), rt_s(&S688) }));
      rt_exit(rt_i(2));
    }
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = ({ wv t2534 = std_slice(w_input, rt_i(0), rt_i(-4)); wv t2535 = (rt_cond(rt_chk(w_onWindows(), NULL)) ? rt_s(&S689) : rt_s(&S13)); rt_add(t2534, t2535); });
    }
  }
  if (rt_cond(rt_eq(w_target, rt_s(&S13)))) {
    w_target = rt_s(&S682);
  }
  w_runtimeGiven = rt_ne(w_runtimePath, rt_s(&S13));
  if (rt_cond(rt_not(w_runtimeGiven))) {
    w_runtimePath = rt_bind((rt_cond(rt_eq(w_target, rt_s(&S682))) ? rt_s(&S690) : rt_s(&S691)), 1);
  }
  (void)rt_chk(w_load(w_input), NULL);
  if (rt_cond(rt_eq(w_mode, rt_s(&S675)))) {
    v1254_tokens = rt_bind(rt_chk(w3_lex(w2_srcChars), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)rt_chk(w8_dumpTokens(v1254_tokens, rt_s(&S13)), NULL);
    rt_exit(rt_i(0));
  }
  if (rt_cond(rt_eq(w_mode, rt_s(&S676)))) {
    v1255_program = rt_bind(rt_chk(w4_parse(rt_chk(w3_lex(w2_srcChars), NULL)), NULL), 0);
    (void)rt_chk(w2_flushWarnings(), NULL);
    (void)std_print(rt_chk(w8_dumpValue(v1255_program, rt_s(&S13)), NULL));
    rt_exit(rt_i(0));
  }
  w_modules = rt_bind(rt_chk(w9_loadProgram(w2_srcFile, w2_srcChars), NULL), 0);
  w_problems = rt_bind(rt_chk(w5_check(w_modules, rt_b(1)), NULL), 0);
  if (rt_cond(rt_gt(std_length(w_problems), rt_i(0)))) {
    (void)rt_chk(w2_flushWarnings(), NULL);
    v1256_p = rt_i(0);
    while (rt_cond(({ wv t2543 = v1256_p; wv t2544 = std_length(w_problems); rt_lt(t2543, t2544); }))) {
      (void)std_eprint(rt_chk(w2_formatDiag(rt_idx(v1256_p, w_problems)), NULL));
      v1256_p = rt_add(v1256_p, rt_i(1));
    }
    rt_exit(rt_i(2));
  }
  w_runtime = rt_bind(rt_chk(w_readRuntime(), NULL), 0);
  if (rt_cond(rt_eq(w_runtime, RT_NULL))) {
    (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S673), rt_text(w_runtimePath) }));
    rt_exit(rt_i(2));
  }
  w_code = rt_bind((rt_cond(rt_eq(w_target, rt_s(&S682))) ? rt_chk(w7_generateC(w_modules, w_runtime), NULL) : rt_chk(w6_generate(w_modules, w_runtime), NULL)), 0);
  (void)rt_chk(w2_flushWarnings(), NULL);
  if (rt_cond(rt_ne(w_mode, rt_s(&S674)))) {
    if (rt_cond(rt_eq(w_output, rt_s(&S13)))) {
      w_output = ({ wv t2545 = (rt_cond(w_dotWvt) ? std_slice(w_input, rt_i(0), rt_i(-4)) : w_input); wv t2546 = rt_cat(2, (wv[]){ rt_s(&S129), rt_text(w_target) }); rt_add(t2545, t2546); });
    }
    wv t2547 = std_writeFile(w_output, w_code);
    if (rt_failcode) {
      rt_failcode = 0;
      {
        (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S693), rt_text(w_output) }));
        rt_exit(rt_i(2));
        t2547 = RT_NULL;
      }
    }
    (void)t2547;
    rt_exit(rt_i(0));
  }
  w_dotExe = rt_b(rt_bool(rt_ge(std_length(w_output), rt_i(4)), "&&") && rt_bool(rt_eq(rt_chk(w1_toLower(({ wv t2548 = w_output; wv t2549 = std_length(w_output); std_slice(t2548, rt_i(-4), t2549); })), NULL), rt_s(&S689)), "&&"));
  w_cFile = rt_add((rt_cond(w_dotExe) ? std_slice(w_output, rt_i(0), rt_i(-4)) : w_output), rt_s(&S694));
  wv t2550 = std_writeFile(w_cFile, w_code);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(2, (wv[]){ rt_s(&S693), rt_text(w_cFile) }));
      rt_exit(rt_i(2));
      t2550 = RT_NULL;
    }
  }
  (void)t2550;
  w_parts = rt_bind(std_split(std_join(std_split(w_cc, rt_s(&S19)), rt_s(&S87)), rt_s(&S87)), 0);
  w_ccName = rt_bind(rt_chk(w1_toLower(rt_idx(rt_i(-1), w_parts)), NULL), 0);
  w_flags = rt_arr(4, (wv[]){ rt_s(&S695), rt_s(&S677), w_output, w_cFile }, 0);
  wv t2551 = (rt_cond(rt_b(rt_bool(rt_eq(w_ccName, rt_s(&S686)), "||") || rt_bool(rt_eq(w_ccName, rt_s(&S696)), "||"))) ? rt_arr(5, (wv[]){ rt_s(&S697), rt_s(&S695), rt_s(&S677), w_output, w_cFile }, 0) : w_flags);
  wv t2552 = std_run(w_cc, t2551);
  if (rt_failcode) {
    rt_failcode = 0;
    {
      (void)std_eprint(rt_cat(3, (wv[]){ rt_s(&S698), rt_text(w_cc), rt_s(&S699) }));
      rt_exit(rt_i(2));
      t2552 = RT_NULL;
    }
  }
  w_status = rt_bind(t2552, 0);
  if (rt_cond(rt_ne(w_status, rt_i(0)))) {
    (void)std_eprint(({ wv t2553 = rt_text(w_cc); wv t2554 = rt_text(w_status); rt_cat(4, (wv[]){ rt_s(&S687), t2553, rt_s(&S700), t2554 }); }));
    rt_exit(rt_i(2));
  }
  rt_exit(rt_i(0));
}
