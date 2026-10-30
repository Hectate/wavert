# wahc: the Γ-- compiler, in Γ--

`wahc.wah` is the stage 1 compiler (spec §15). It is a port of `wahc0` ([../js/src](../js/src)) to Γ--, compiled by `wahc0`. It keeps the same diagnostics so the two compilers can be checked against each other. Each phase is a module (§13.1) that `wahc.wah`, the command line, steals.

## Status

| Module | Phase | `wahc0` source |
|---|---|---|
| `text.wah` | Characters and text helpers | |
| `diag.wah` | Diagnostics | `errors.js` |
| `lexer.wah` | Lexer | `lexer.js` |
| `parser.wah` | Parser | `parser.js` |
| `loader.wah` | Loading the modules of the program being compiled | `compile.js` |
| `checker.wah` | Checker | `check.js` |
| `codegen.wah` | Code generator | `codegen.js` |
| `codegen_c.wah` | Code generator for `--target c` | `codegen_c.js` |
| `dump.wah` | `--tokens` and `--ast` | |
| `wahc.wah` | Command line | `../bin/wahc0.js` |

Every phase is ported.

The output inlines `js/src/runtime.js`, which `wahc` reads with `$readFile`, so both compilers share one runtime.

**Bootstrap stage 2 holds:** `wahc` compiled by `wahc0` and `wahc` compiled by itself produce the same JavaScript, byte for byte. A third generation is identical too.

**The native fixed point holds:** `wahc.c` is `wahc` compiled to C. A native `wahc` built from it compiles itself back to the same `wahc.c`, and compiles every program exactly as `wahc0` does.

## Usage

```
node js/bin/wahc0.js run wahc/wahc.wah <file.wah> [-o <out.js>] [--runtime <runtime.js>]
node js/bin/wahc0.js run wahc/wahc.wah <file.wah> --target c [-o <out.c>] [--runtime <wah.c>]
node js/bin/wahc0.js run wahc/wahc.wah build <file.wah> [-o <program>] [--cc <compiler>] [--runtime <wah.c>]
node js/bin/wahc0.js run wahc/wahc.wah --tokens <file.wah>
node js/bin/wahc0.js run wahc/wahc.wah --ast <file.wah>
```

- With a file, `wahc` writes JavaScript to `<file>.js`, or to `-o`. The output runs with plain `node`, like `wahc0`'s. The runtime defaults to `js/src/runtime.js`.
- With `--target c`, it writes C to `<file>.c`, with the runtime `runtime/wah.c`. Build it with a GNU C compiler, such as `zig cc -O2 -o prog prog.c`.
- `build` compiles to C, writes it beside the program as `<program>.c`, then runs a C compiler with `$run`. The program defaults to the input without `.wah`, with `.exe` on Windows; an input without `.wah` needs `-o`. The compiler is `--cc`, else the `ZIG` variable, else `zig` on `PATH` (see [Getting Zig](../README.md#getting-zig)); Zig is called as `zig cc`, other compilers get the flags directly. If the compiler cannot be started or fails, `wahc` exits with 2.
- A default runtime path is tried from the current directory, then from where `wahc` is (`$programPath`): next to it, then one directory up. A path given with `--runtime` is used as it is.
- A program that steals libraries is compiled whole, as with `wahc0`: libraries are found through their `steal` paths.
- `--tokens` prints one token per line: type, start, end, whether whitespace precedes it, then the name or the numeral's text and value. A string's parts follow on their own lines.
- `--ast` prints the syntax tree as indented records, with fields sorted by name. `--tokens` and `--ast` show the given file alone, without the libraries it steals.

Warnings and errors go to stderr in `wahc0`'s format, with exit code 2 on an error. Like `wahc0`, the lexer and parser stop at the first error, because stage 0 cannot catch tantrums. The checker reports every error it finds.

## Native wahc

```
zig cc -O2 -o wahc/wahc.exe wahc/wahc.c
wahc/wahc.exe build examples/fizzbuzz.wah
```

The native `wahc` takes the same arguments. It compiles itself about six times faster than `wahc` on Node.

`wahc.c` is generated, and committed so that a native `wahc` can be built without Node. When `wahc` or the runtime changes, regenerate it:

```
node js/bin/wahc0.js wahc/wahc.wah --target c -o wahc/wahc.c
```

It keeps LF line endings on every checkout (`.gitattributes`), so it is the same file everywhere.

## Tests

Run `cd wahc && npm test`. The tests compile `wahc.wah` with `wahc0` and check that both compilers produce the same tokens, syntax trees, JavaScript, C, warnings and errors, checker errors included. A stage 2 test compiles `wahc.wah` with itself and checks the fixed point. `bootstrap.test.js` checks that `wahc.c` is up to date and, with [Zig](https://ziglang.org) (the `ZIG` variable, or on `PATH`), builds a native `wahc` and checks the native fixed point. The comparisons cover:
- every example and its libraries, and every module of `wahc` itself
- programs made of several files: initialization order, the library rules, stolen names, and errors in libraries
- every Γ-- snippet in `wahc0`'s own tests
- every lexer and parser error path, the checker's error kinds, and the code generator's statement and expression forms
- the stdlib table, which `wahc` keeps its own copy of

## Differences from `wahc0`

- Positions count code points, while `wahc0` counts UTF-16 units. They differ only after a character outside the BMP, and that also shifts the column in a diagnostic.
- Record shapes are fixed (§10.7), so every expression node has `paren` and every statement node has `handler`. `wahc0` adds these fields only when they apply.
- Keywords cannot be field names, so `wahc0`'s `shifty` field is `isShifty` here, and its `whine` field is `handler`.
- The stdlib table is a copy of `js/src/stdlib.js` in `checker.wah`, since a Γ-- module cannot steal a JavaScript file. A test keeps the two in sync.

## Language findings

Things that tripped up the port, recorded as input for the spec's open questions (§16):

- **A trailing value can be swallowed by the statement before it.** A block's value that starts with `,` (a zero-argument call) or `[` continues the previous statement's expression instead. `;!doubt (c) { } ,(f)` reads `,(f)` as part of the conditional's expression. Write `(,(f))` or `([])(f)`. *Resolved in Draft 0.6 (§7.2): the `,` error now has a hint, and a line-start `[` warns.*
- **A stray `;` silently drops a block's value.** If a scheme's last line starts with `;`, the line is a statement and the scheme returns `uninvited` (§7.2). Nothing reports this until a later tantrum. *Resolved in Draft 0.6 (§7.2): a used value must be written out, so this is now a compile error, and `;n` warns that it has no effect.* The new rule found seven schemes in `wahc.wah` that return nothing but whose value was used, in 23 places, mostly `;getaway x(emit)`. It also found four test programs that relied on an implicit `uninvited`.
- **`shifty` conflates rebinding with mutation.** A variable that is rebound to successive frozen records, like the left operand in a precedence-climbing loop, must be `shifty`. Then each `cheat` copies the record (§10.4). There is no way to rebind a reference to frozen data without copying it. *Resolved in Draft 0.8 (§5): a `fickle` binding can be rebound and never copies its value.* `wahc` now uses `fickle` for its rebound walkers (the scope chains, the precedence loop) and for parameters it only reads. A native self-compile went from 193,492 copies of 1.47 million values to 20,769 copies of 0.10 million, peak memory from 197 MB to 164 MB, and time from 165 ms to 151 ms. The remaining copies are mostly deliberate: `whineStmt` and `suspectExpr` copy a node before changing it, and the parser's statements and parenthesized expressions are shifty because the parser fills in their fields.
- **`claim shifty` silently copies a frozen record, so identity is lost.** The modules port compared a statement with `!=` to find a library's last one, but the last statement had been held in a `shifty` binding, which is a copy. The check never matched and every `bribe` was reported as misplaced. It now compares positions. This is the same copying as the finding above, seen from the other side. *Resolved in Draft 0.8: a `fickle` binding keeps the value's identity.*
- **Only a binding's own module may `cheat` it (§13.1),** so shared mutable state needs a setter. Four phases set the file being worked on, which `diag.wah` declares, so `diag.wah` exports `setSource`. The rule worked as intended: the setter is now the one place that state changes.
- **Common words are keywords, so they cannot be field or parameter names.** `shifty` and `whine` are natural field names for an AST, and `shifty` is a natural parameter name in a code generator.
- **A call captures every argument to its left.** `;args, a(f)($push)` calls `f(args, a)`, not `$push(args, f(a))`. The correct form is `;args, (a(f))($push)`. Spec §6.2 states the rule, but writing the port hit it six times. The arity checker caught each one at compile time, which keeps it from being a bug, but the natural way to write the code is the wrong one.
- **Without maps, results about syntax nodes need a key.** The C code generator must remember, for each name, which binding it means, and for each binding, whether a nested scheme uses it. Records have fixed shapes (§10.7), so it cannot add fields to the nodes, and finding a node in a list by `==` would take a linear search for every name. It keeps the results in an array indexed by source position, since each name, claim and parameter is its own token. A map type (§16) would make this direct.
