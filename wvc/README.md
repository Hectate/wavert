# wvc: the Wavert compiler, in Wavert

`wvc` compiles Wavert (`.wvt`) to C, or to JavaScript with `--target js`. It is written in Wavert and compiles itself. `wvc.c` is `wvc` compiled to C, the bootstrap seed: a C compiler is all you need to build it.

It began as a fork of `wahc`, the Γ-- compiler in [../wah/wahc](../wah/wahc). The lexer and parser were rewritten for Wavert, and the rest was converted from Γ-- with [../tools/wah2wvt.wah](../tools/wah2wvt.wah). See [../docs/hybrid-syntax.md](../docs/hybrid-syntax.md#bootstrap-status) for how the bootstrap went.

## Usage

```
zig cc -O2 -o wvc/wvc.exe wvc/wvc.c
wvc/wvc.exe build examples/fizzbuzz.wvt     # writes examples/fizzbuzz.c and builds examples/fizzbuzz(.exe)
wvc/wvc.exe examples/fizzbuzz.wvt           # just the C, as examples/fizzbuzz.c
wvc/wvc.exe examples/fizzbuzz.wvt --target js -o fizzbuzz.js && node fizzbuzz.js
wvc/wvc.exe --tokens examples/hello.wvt
wvc/wvc.exe --ast examples/hello.wvt
```

- With a file, `wvc` writes C to `<file>.c`, or to `-o`, inlining the runtime [../runtime/wvt.c](../runtime/wvt.c). With `--target js` it writes JavaScript that runs with plain `node`, inlining [../runtime/wvt.js](../runtime/wvt.js). Pass `--runtime` to use another runtime file.
- A default runtime path is tried from the current directory, then from where `wvc` is: next to it, then one directory up. So a `wvc` in `wvc/` or `.wvt/` finds the repository's `runtime/`.
- `build` compiles to C, writes it beside the program as `<program>.c`, and runs a C compiler. The program defaults to the input without `.wvt`, with `.exe` on Windows. The compiler is `--cc`, else the `ZIG` variable, else `zig` on `PATH`. Zig is called as `zig cc`; other compilers get the flags directly. See [Getting Zig](../wah/README.md#getting-zig).
- A program that imports libraries is compiled whole: libraries are found through their import paths.
- `--tokens` and `--ast` show the given file alone, without the libraries it imports.

Warnings and errors go to stderr, with exit code 2 on an error. The lexer and parser stop at the first error; the checker reports every error it finds.

## Layout

| Module | Phase |
|---|---|
| `wvc.wvt` | Command line |
| `text.wvt` | Characters and text helpers |
| `diag.wvt` | Diagnostics |
| `lexer.wvt` | Lexer |
| `parser.wvt` | Parser. It builds `wahc`'s syntax tree, whose node kinds keep their Γ-- names (`scheme` for `fn`, `claim` for `const`/`var`/`ref`, …); the comment at its top lists them. |
| `loader.wvt` | Loading the modules of the program being compiled |
| `checker.wvt` | Checker |
| `codegen.wvt` | Code generator for `--target js` |
| `codegen_c.wvt` | Code generator for C |
| `dump.wvt` | `--tokens` and `--ast` |

Section numbers (§) in comments and messages refer to the Γ-- spec, [../wah/wah-spec.md](../wah/wah-spec.md). Wavert changes Γ--'s syntax and names, not its semantics.

## Changing wvc

`wvc.c` must stay in step with the sources, so regenerate it after changing `wvc/*.wvt` or `runtime/wvt.c`:

```
zig cc -O2 -o .wvt/wvc.exe wvc/wvc.c
.wvt/wvc.exe wvc/wvc.wvt -o wvc/wvc.c
```

If the change affects the code `wvc` generates, the new `wvc.c` was made by the old compiler: build it and run the second command again, until `wvc.c` no longer changes (the fixed point). The tests check it.

A change that renames or removes a standard library function, or a runtime behaviour that `wvc` itself relies on, takes two steps. First add the new form alongside the old one and regenerate, then switch the sources over, remove the old form and regenerate twice.

`wvc.c` keeps LF line endings on every checkout (`.gitattributes`), so it is the same file everywhere.

## Tests

Run `npm test` here (Node ≥ 22.3). The tests need [Zig](https://ziglang.org), through `ZIG` or on `PATH`; without it they skip. They build `wvc` from `wvc.c`, then check:

| File | What |
|---|---|
| `bootstrap.test.js` | The fixed point: `wvc` built from `wvc.c` compiles `wvc.wvt` back to `wvc.c`. Finding the runtime, and `build`. |
| `examples.test.js` | Every example, natively and on Node, prints what it should. |
| `syntax.test.js` | Wavert's syntax and what it means: precedence, argument grouping, declarations, `set`, `match`, `fail`/`onfail`, modules. |
| `errors.test.js` | The diagnostics for common mistakes. |
| `convert.test.js` | `tools/wah2wvt.wah`, built with `wahc` from `../wah`: the Γ-- examples convert to Wavert that has the same syntax tree and compiles to the same C and JavaScript. |
