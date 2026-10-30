// Evaluates runtime operations read from stdin, one per line, so the tests can
// compare runtime/wah.c with js/src/runtime.js on the same inputs.
//
// A line is an operation name and its arguments: i:<int>, f:<16 hex digits of
// the double's bits>, s:<hex of the UTF-8 bytes>, b:0, b:1, n (uninvited), or
// [ ... ] for a frozen array. The result is printed the same way, with type
// names (int:3, float:<bits>, purple:<hex>, ...), as "WAH <from>" for a
// signal, or as "! <message>" for a tantrum.
#include <setjmp.h>
static jmp_buf probe_jb;
static void probe_die(const char *msg, size_t n);
#define WAH_TEST_DIE(msg, n) probe_die(msg, n)
#include "../wah.c"

static void probe_die(const char *msg, size_t n) {
  rt_write(1, "! ", 2);
  rt_write(1, msg, n);
  rt_write(1, "\n", 1);
  rt_flush();
  rt_depth = 0;
  longjmp(probe_jb, 1);
}

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }
static sb unhex(const char *p, const char *end) {
  sb b = { 0 };
  sb_need(&b, 0);
  for (; p + 1 < end; p += 2) sb_putc(&b, (char)(hexval(p[0]) * 16 + hexval(p[1])));
  return b;
}

// Parses one value from *p.
static wv parse_value(const char **p, const char *end) {
  while (*p < end && **p == ' ') (*p)++;
  const char *s = *p;
  while (*p < end && **p != ' ') (*p)++;
  const char *e = *p;
  if (s[0] == '[') {
    warr *a = arr_new(0, 1);
    for (;;) {
      while (*p < end && **p == ' ') (*p)++;
      if (*p < end && **p == ']') { (*p)++; break; }
      std_push((wv){ .tag = T_ARR, .a = a }, parse_value(p, end));
    }
    a->shifty = 0;
    return (wv){ .tag = T_ARR, .a = a };
  }
  if (s[0] == 'n') return RT_NULL;
  if (s[0] == 'b') return rt_b(s[2] == '1');
  if (s[0] == 'i') return rt_i(strtoll(s + 2, NULL, 10));
  if (s[0] == 'f') {
    uint64_t bits = strtoull(s + 2, NULL, 16);
    double d;
    memcpy(&d, &bits, 8);
    return rt_f(d);
  }
  sb b = unhex(s + 2, e);
  wv v = rt_str(b.p, b.n);
  free(b.p);
  return v;
}

static void put_value(sb *b, wv v) {
  switch (v.tag) {
    case T_INT: sb_printf(b, "int:%lld", (long long)v.i); return;
    case T_FLOAT: {
      uint64_t bits;
      memcpy(&bits, &v.f, 8);
      if (isnan(v.f)) bits = 0x7ff8000000000000ULL;
      sb_printf(b, "float:%016llx", (unsigned long long)bits);
      return;
    }
    case T_BOOL: sb_puts(b, v.b ? "boolean:real" : "boolean:fake"); return;
    case T_NULL: sb_puts(b, "uninvited"); return;
    case T_STR:
      sb_puts(b, "purple:");
      for (int64_t i = 0; i < v.s->bytes; i++) sb_printf(b, "%02x", (unsigned char)v.s->data[i]);
      return;
    case T_ARR:
      sb_puts(b, "[");
      for (int64_t i = 0; i < v.a->len; i++) {
        sb_putc(b, ' ');
        put_value(b, v.a->items[i]);
      }
      sb_puts(b, " ]");
      return;
  }
  sb_puts(b, "?");
}

static wv op_text(wv v) { return rt_text(v); }
static wv op_idx(wv i, wv a) { return rt_idx(i, a); }
static wv op_cond(wv x) { return rt_b(rt_cond(x)); }
static wv op_decode(wv s) { return rt_decode(s.s->data, s.s->bytes); }
static wv op_show(wv v) { sb b = { 0 }; show_to(&b, v); return sb_str(&b); }
static wv op_readLine(void) { return std_readLine(); }

static const struct { const char *name; int arity; void *fn; } OPS[] = {
  { "add", 2, rt_add }, { "sub", 2, rt_sub }, { "mul", 2, rt_mul }, { "div", 2, rt_div },
  { "mod", 2, rt_mod }, { "pow", 2, rt_pow }, { "neg", 1, rt_neg },
  { "eq", 2, rt_eq }, { "ne", 2, rt_ne }, { "lt", 2, rt_lt }, { "le", 2, rt_le },
  { "gt", 2, rt_gt }, { "ge", 2, rt_ge },
  { "band", 2, rt_band }, { "bor", 2, rt_bor }, { "bxor", 2, rt_bxor }, { "bnot", 1, rt_bnot },
  { "shl", 2, rt_shl }, { "shr", 2, rt_shr }, { "not", 1, rt_not }, { "cond", 1, op_cond },
  { "text", 1, op_text }, { "show", 1, op_show }, { "idx", 2, op_idx }, { "decode", 1, op_decode },
  { "length", 1, std_length }, { "at", 2, std_at }, { "slice", 3, std_slice },
  { "code", 1, std_code }, { "fromCode", 1, std_fromCode }, { "split", 2, std_split },
  { "join", 2, std_join }, { "toPurple", 1, std_toPurple }, { "toWah", 1, std_toWah },
  { "toInt", 1, std_toInt }, { "toFloat", 1, std_toFloat }, { "floor", 1, std_floor },
  { "ceil", 1, std_ceil }, { "round", 1, std_round }, { "trunc", 1, std_trunc },
  { "toUint32", 1, std_toUint32 }, { "isNobody", 1, std_isNobody }, { "same", 2, std_same },
  { "typeOf", 1, std_typeOf },
};

static void wah_main(void) {
  // The cases come from a file, so stdin stays free for $readLine tests.
  char *data;
  int64_t n;
  const wstr *path = rt_s(str_new(rt_argv[1], strlen(rt_argv[1]))).s;
  if (!read_file(path, &data, &n)) { fputs("probe: cannot read cases\n", stderr); exit(2); }
  const char *p = data, *end = data + n;
  while (p < end) {
    const char *eol = memchr(p, '\n', end - p);
    if (!eol) eol = end;
    const char *q = p;
    while (q < eol && *q != ' ') q++;
    size_t len = q - p;
    const char *name = p;
    p = eol + 1;
    if (setjmp(probe_jb)) continue;
    int k = 0;
    while (k < (int)(sizeof OPS / sizeof OPS[0]) && !(strlen(OPS[k].name) == len && !memcmp(OPS[k].name, name, len))) k++;
    if (k == (int)(sizeof OPS / sizeof OPS[0])) { fprintf(stderr, "probe: unknown op %.*s\n", (int)len, name); exit(2); }
    wv a[3];
    for (int i = 0; i < OPS[k].arity; i++) a[i] = parse_value(&q, eol);
    wv r;
    switch (OPS[k].arity) {
      case 0: r = ((wv (*)(void))OPS[k].fn)(); break;
      case 1: r = ((wv (*)(wv))OPS[k].fn)(a[0]); break;
      case 2: r = ((wv (*)(wv, wv))OPS[k].fn)(a[0], a[1]); break;
      default: r = ((wv (*)(wv, wv, wv))OPS[k].fn)(a[0], a[1], a[2]); break;
    }
    sb b = { 0 };
    if (rt_sigcode) {
      sb_printf(&b, "WAH %s", rt_sigfrom);
      rt_sigcode = 0;
    } else put_value(&b, r);
    sb_putc(&b, '\n');
    rt_write(1, b.p, b.n);
    rt_flush(); // so a crash shows which case caused it
    free(b.p);
  }
  // Then every line of stdin.
  wv line;
  while (read_line(&line)) {
    sb b = { 0 };
    put_value(&b, line);
    sb_putc(&b, '\n');
    rt_write(1, b.p, b.n);
    free(b.p);
  }
}
