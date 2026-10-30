// Exercises the runtime's host functions (§12). The first argument picks what
// to do; the test (host.test.js) checks what the program prints and its exit code.
#include "../wah.c"

static wv arg(int64_t i) { return std_args().a->items[i]; }
static int64_t nargs(void) { return std_args().a->len; }
static int is(const char *mode) { return nargs() > 0 && !strcmp(arg(0).s->data, mode); }
static void show_line(wv v) {
  sb b = { 0 };
  show_to(&b, v);
  sb_putc(&b, '\n');
  rt_write(1, b.p, b.n);
  free(b.p);
}
// The result of a stdlib call: the value, or WAH and the function that signaled.
static void result(wv v) {
  if (rt_sigcode) {
    sb b = { 0 };
    sb_printf(&b, "WAH %s\n", rt_sigfrom);
    rt_write(1, b.p, b.n);
    free(b.p);
    rt_sigcode = 0;
    return;
  }
  show_line(v);
}

static void wah_main(void) {
  if (is("args")) {
    // Each argument after the mode, then the arguments as one array.
    for (int64_t i = 1; i < nargs(); i++) show_line(arg(i));
    return;
  }
  if (is("run")) {
    // run <program> <arg>...: runs it and prints its exit code.
    warr *rest = arr_new(nargs() - 2, 0);
    for (int64_t i = 2; i < nargs(); i++) rest->items[i - 2] = arg(i);
    std_write(rt_cstr("before\n")); // flushed before the child writes
    result(std_run(arg(1), (wv){ .tag = T_ARR, .a = rest }));
    return;
  }
  if (is("exit")) rt_exit(std_toInt(arg(1)));
  if (is("files")) {
    // files <dir>: file operations inside an empty directory.
    wv dir = arg(1);
    wv f = rt_add(dir, rt_cstr("/a.txt")), g = rt_add(dir, rt_cstr("/é😀.txt"));
    result(std_fileExists(f));
    result(std_readFile(f));
    result(std_writeFile(f, rt_cstr("one\n")));
    result(std_appendFile(f, rt_cstr("two €\n")));
    result(std_readFile(f));
    result(std_fileExists(f));
    result(std_writeFile(g, rt_cstr("\xef\xbb\xbf" "bom")));
    result(std_readFile(g));
    result(std_listDir(dir));
    result(std_listDir(f));
    result(std_readFile(dir));
    result(std_deleteFile(f));
    result(std_deleteFile(f));
    result(std_listDir(dir));
    result(std_fileExists(dir));
    result(std_writeFile(rt_add(dir, rt_cstr("/no/such/dir/x")), rt_cstr("x")));
    result(std_appendFile(rt_add(dir, rt_cstr("/no/such/dir/x")), rt_cstr("x")));
    result(std_readFile(rt_str("a\0b", 3)));
    result(std_fileExists(rt_cstr("")));
    result(std_listDir(rt_cstr("")));
    return;
  }
  if (is("env")) {
    for (int64_t i = 1; i < nargs(); i++) result(std_env(arg(i)));
    return;
  }
  if (is("path")) { result(std_programPath()); return; }
  if (is("pipe")) for (;;) std_print(rt_cstr("wah"));
  if (is("epipe")) for (;;) std_eprint(rt_cstr("wah"));
  if (is("tantrum")) {
    std_print(rt_cstr("out first"));
    rt_enter("outer");
    rt_enter("boom");
    rt_tantrum(rt_cstr("it broke"));
  }
  if (is("tantrum-value")) rt_tantrum(rt_arr(2, (wv[]){ rt_i(1), rt_cstr("a\"b") }, 0));
  if (is("unhandled")) {
    // A scheme signals code 3, and its caller does not handle it.
    rt_enter("ignore");
    rt_enter("parse");
    rt_signal(rt_i(3));
    rt_depth--;
    rt_chk(RT_NULL, "ignore");
  }
  if (is("unhandled-top")) rt_chk(std_readLine(), NULL);
  if (is("overflow")) {
    for (;;) rt_enter("deep");
  }
  if (is("echo")) {
    // Copies stdin to stdout, line by line, after a prompt.
    std_write(rt_cstr("prompt> "));
    for (;;) {
      wv line = std_readLine();
      if (rt_sigcode) break;
      std_print(line);
    }
    return;
  }
  std_eprint(rt_cstr("host: unknown mode"));
  rt_exit(rt_i(2));
}
