# wahc0: the stage 0 Γ-- compiler

Compiles Γ-- (`.wah`) to standalone JavaScript (spec §15, stage 0). It has no dependencies and needs Node ≥ 22.3.

```
node js/bin/wahc0.js <file.wah> [-o out.js]    # compile; the output runs with plain `node out.js`
node js/bin/wahc0.js <file.wah> --target c [-o out.c]
                                               # compile to C; build with zig cc -O2 -o prog out.c
node js/bin/wahc0.js build <file.wah> [-o program] [--cc compiler]
                                               # compile to <program>.c and build <program>
                                               # (default: <file> without .wah, .exe on Windows;
                                               # compiler: $ZIG, else zig)
node js/bin/wahc0.js run <file.wah> [args...]  # compile and run
node js/bin/wahc0.js -e '<source>' [args...]   # run inline source (no ;!invited needed)
cd js && npm test
```

The C backend's tests need [Zig](https://ziglang.org), found through the `ZIG` variable or on `PATH`; without it they skip. See [Getting Zig](../README.md#getting-zig) for installing it and setting `ZIG`. `WAH_TARGET=c npm test` runs the whole suite through the C backend: each test program is compiled to C, built and run natively.

Exit codes:
- 2: compile error.
- 1: uncaught tantrum.
- Otherwise, the program's own `!invited` code.

## Layout

| File | Role |
|---|---|
| `src/compile.js` | The pipeline. Loads the entry file and the libraries it steals (§13.1), in initialization order |
| `src/lexer.js` | Tokens, wah numerals, `WAH!`, bang tokens, `$` names, `.`, strings and interpolation, lexical lints |
| `src/parser.js` | Statements, precedence (§11.4), postfix calls, block values, record literals, member access |
| `src/check.js` | Scoping, shifty rules, arity, placement of `getaway`/`bail`/`WAH!`, the `;!invited` rule, library rules and stolen names |
| `src/codegen.js` | AST → JS. User names become `w_name` (`w<k>_name` in library *k*), stdlib names `$std.name`, record fields `f_name` |
| `src/runtime.js` | Inlined into every output: typed operators, arrays, records, signals, stdlib |
| `src/codegen_c.js` | AST → C, for `--target c` (spec §15). Inlines [`runtime/wah.c`](../runtime/) |

## Stage 0 interpretations

Draft 0.3 was silent or ambiguous on the points below. Each choice made here has been folded into draft 0.4 of [the spec](../wah-spec.md).

**Not supported in stage 0**
- `blame`, `sulk` and `regret`: tantrums cannot be caught.
- Maps and functions as values.

**Syntax**
- `;tantrum expr` throws any value.
- `whine` handles the **outermost** call of its statement. Its block value replaces that call's value.
- A call is exactly `(name)` after an expression. Any other `(` ends the expression, so a grouped block value such as `(a + b) / c` can follow a statement.
- Only `!doubt`, `!lurk` and `!invited` are bang tokens. `!real` is NOT applied to `real`.
- A zero-parameter scheme is written `;scheme ()name { ... }`.
- Compound cheats (`+cheat`, `++cheat` and so on) are single tokens.
- `;expr !invited` exits with any int code.

**Scoping**
- No shadowing is checked positionally. A claim conflicts only with outer claims that come before it. Outer schemes, parameters and whine codes always conflict.
- Nested schemes may read and `cheat` outer bindings.

**Calls and copying**
- `shifty` parameters follow the §10.4 copy table. Scalars are not passed by reference.

**Numerics**
- Int results outside ±(2^53−1) raise a tantrum.
- `int ** negative int` returns a float.
- `int % 0` raises a tantrum. Float division and modulo follow IEEE.
- `$round` rounds half away from zero.
- Ordering two booleans, arrays or `uninvited` values raises a tantrum.

**Text**
- Floats print with a fractional part, e.g. `1.0` and `1.0e+21`.
- NaN and ±∞ print as `nobody`, `everybody` and `-everybody`.

**Standard library**
- `$length`, `$at` and `$slice` work on purples and arrays. `$slice` is end-exclusive.
- Additions beyond §12: `$pop`, `$join`, `$fromCode`, `$typeOf` and `$isNobody`.
- Stdlib signals use code 1.

**Lints**
- Formatter conventions (`,(f)` spacing, `whine` indentation) are not linted yet, because there is no formatter.

## Notes for stage 1

`examples/tokenize.wah` is a Γ-- tokenizer written in Γ--. The tests check that it agrees with this lexer token for token, on itself and on the other examples. Writing it surfaced the following:
- Calls bind loosest, so a call in a condition or an operand usually needs parentheses: `(c(isSpace)) || c == ";"`, `i < (xs($length))`.
- Array literals follow the call rule as well: `[a, b(f)]` is `[f(a, b)]`. Write `[a, (b(f))]`.
- A literal `}` in a string is written `"}}"`. Writing the tokenizer also led to adding the `\r` escape in draft 0.4.
- Passing a shifty array to a non-shifty parameter copies it on every call (§10.4). Pass frozen arrays, such as those from `$split`, or use `shifty` parameters.
- In a record literal, `,` separates fields, so a call with several arguments must be grouped: `{ kind: kind, text: (src, start, i($slice)) }`. A field and a binding may share a name, as `kind` does here.

Every file in `examples/` is run by the tests (`test/cli.test.js`). When the language changes, update the examples. When a major feature is added, add an example for it, like `examples/records.wah`.
