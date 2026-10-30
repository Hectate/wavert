# The C runtime

`wah.c` is the runtime of the C backend (`--target c`, spec §15), the counterpart of [`js/src/runtime.js`](../js/src/runtime.js). Both compilers inline it at the top of every C program they write, so the output is a single file:

```
node js/bin/wahc0.js prog.wah --target c -o prog.c
zig cc -O2 -o prog.exe prog.c
```

It needs a GNU C compiler (clang or gcc) for the statement expressions in generated code. [Zig](https://ziglang.org)'s `zig cc` works on Windows, Linux and macOS (see [Getting Zig](../README.md#getting-zig)). It needs no flags and no libraries.

## What is in it

| Part | Notes |
|---|---|
| Values | A 16-byte tagged `wv`. Ints are `int64_t`, checked to ±(2^53−1); purples are immutable UTF-8 with a code point count; arrays and records carry their shifty flag (§10.4, §10.9) |
| Memory | An arena that is never freed |
| Operators | Every check and message of `runtime.js` (§11). Float `**` is fdlibm's `pow`, the same on every platform; V8 can differ in the last bit (§11.2) |
| Text | Shortest round-trip float text (§9.3), and correctly rounded `$toFloat` on any C library (§9.5): `strtod`'s result is checked against the neighboring doubles exactly |
| Signals and tantrums | A pending `WAH!` code that the caller handles or turns into a tantrum (§8.2); a tantrum prints the scheme stack and exits with 1 (§8.1) |
| Host | Files, `$args`, `$env`, `$run` and `$programPath` on Windows and POSIX, UTF-8 throughout (§12.1); a broken pipe exits with 141 (§12.2) |
| Startup | `main` runs the program on a thread with a 1 GB stack, since schemes recurse deeply |

## Tests

```
cd runtime && npm test    # with ZIG set to the zig executable, or zig on PATH
```

Without Zig the tests skip.
- `test/probe.c` runs operations read from a file. The test runs the same seeded random operations through `runtime.js` and compares every result: float text, `$toFloat`, `$toInt`, UTF-8 decoding, operators and the purple and array builtins. Set `SEED` to try other cases.
- `test/host.c` checks arguments, `$run` and its quoting, files, the environment, `$readLine`, broken pipes, and the text of tantrums and unhandled signals.

The C backend as a whole is tested in `js/` (`WAH_TARGET=c npm test`) and `wahc/`.
