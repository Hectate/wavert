# Wavert

A programming language derived from [Γ--](wah/README.md) ("wah"). Wavert keeps the parts of Γ--'s inverted syntax that make code easier to write, parse and debug (statements that start with `;`, postfix calls, required end markers), puts the rest back in the usual order, and replaces Γ--'s themed keywords with standard ones.

```
;fn average(a, b) {
    (a + b) / 2
}

;"Hello, World"($print)
;(1, 2)(average)($print)        // 1.5
;exit
```

The language is specified in [docs/wavert-spec.md](docs/wavert-spec.md). The reasoning behind each way it differs from Γ-- is in [docs/hybrid-syntax.md](docs/hybrid-syntax.md).

## Quick tour

| Wavert | Meaning |
|---|---|
| `;const x is 1` / `;var i is 0` / `;ref node is head` | declare a frozen, mutable or alias binding |
| `;set i to i + 1` / `;set i += 1` / `;set xs[k] to v` / `;set rec.f to v` | assign |
| `;x(f)` / `;(a, b)(f)` / `;(f)` | `f(x)` / `f(a, b)` / `f()`: calls are postfix and bind tightest |
| `xs[i]`, `node.kind`, `items[i].tags[0]` | indexing and member access |
| `["rat": 4, "bat": 2]` / `[:]` / `m["rat"]` | a map, an empty map, and a lookup |
| `0`, `42`, `-7`, `0xFF`, `0b101`, `1.5`, `1e9` | numbers |
| `true`, `false`, `null`, `nan`, `inf` | constants |
| `;if (c) { } else if (d) { } else { }` / `;!if (c) { }` | if / unless |
| `;while (c) { }` / `;!while (c) { }` / `;break` / `;continue` | loops |
| `;match (x) { case "a", "b" { } else { } }` | multi-way branch, no fallthrough |
| `;enum Light (red, green)` / `Light.red` | enumeration |
| `;fn add(a, b) { a + b }` / `;return x` | function; the last bare expression is its value |
| `;fail 3` + `onfail (c) { ... }` | a failure the immediate caller handles |
| `;panic "message"` | a fatal error |
| `;import lib from "lib.wvt"` / `;export fn f() { }` / `lib.f`, `(x)(lib.f)` | modules |
| `;exit` / `;end` | end of a program / of a library (required) |

## Layout

| Path | Contents |
|---|---|
| [docs/](docs/) | The Wavert specification, and the design document that derived Wavert from Γ--. |
| [wvc/](wvc/) | `wvc`, the Wavert compiler, written in Wavert, with its tests; `wvc/wvc.c` is the bootstrap seed. |
| [runtime/](runtime/) | `wvt.c`, the runtime that compiled programs inline. |
| [examples/](examples/) | Short programs, each showing one feature: Hello World, echo, FizzBuzz, records, `match`, enums, modules, `ref` bindings, maps, records as map keys, telling kinds of record apart, sorting, files, processes, keys, random numbers, the terminal, pausing, and animating between key presses. |
| [samples/](samples/) | Larger programs that combine features: a Wavert tokenizer written in Wavert, and walking around a room with the keyboard. |
| [src/](src/) | Programs, built by `build.ps1`. |
| [build.ps1](build.ps1) | Builds every program in `src\` into `bin\` with `wvc` and Zig. |
| [editors/vscode/](editors/vscode/) | VS Code syntax highlighting and snippets. |
| [tools/](tools/) | `wah2wvt.wah`, a Γ-- to Wavert converter written in Γ--, and `verify.mjs`, which checks a conversion. |
| [wah/](wah/) | The Γ-- language and toolchain, imported from the wah repo (commit `5d2318b`): spec, compilers, runtime, examples, editor support and project template. It was the seed for bootstrapping Wavert, and Wavert no longer depends on it. This repo does not depend on the wah repo. |

## Building

You need [Zig](https://ziglang.org/download/) (on PATH, in `ZIG`, or passed with `-Zig`); see [Getting Zig](wah/README.md#getting-zig).

`.\build.ps1` builds every program in `src\`; `.\build.ps1 -Program primes -Run` builds and runs one. The first build compiles `wvc` from `wvc\wvc.c` and caches it in `.wvt\`.

To use `wvc` directly:

```
zig cc -O2 -o wvc/wvc.exe wvc/wvc.c
wvc/wvc.exe build examples/fizzbuzz.wvt
```

See [wvc/README.md](wvc/README.md) for its options, and for changing it.

## Tests

Run `npm test` in `wvc\` (Node ≥ 22.3, and Zig). The imported Γ-- toolchain has its own tests: `npm test` in `wah\js`, `wah\wahc` or `wah\runtime`.

## Converting Γ-- code

```
zig cc -O2 -o .wvt/wahc.exe wah/wahc/wahc.c
.wvt/wahc.exe tools/wah2wvt.wah --target c -o .wvt/wah2wvt.c --runtime wah/runtime/wah.c
zig cc -O2 -o .wvt/wah2wvt.exe .wvt/wah2wvt.c
.wvt/wah2wvt.exe program.wah              # writes program.wvt
node tools/verify.mjs .wvt/wahc.exe wvc/wvc.exe <dir with .wah files> <dir with .wvt files>
```

The converter keeps comments, blank lines and line breaks. It refuses names that are Wavert keywords (rename them in the Γ-- source first) and `$toWah`. Comparisons with `$typeOf`'s result `"purple"` must be changed to `"string"` by hand.
