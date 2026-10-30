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

Wavert is still being designed. The proposed syntax, and the reasoning behind each decision, is in [docs/hybrid-syntax.md](docs/hybrid-syntax.md).

## Layout

| Path | Contents |
|---|---|
| [docs/](docs/) | Wavert design documents. |
| [wah/](wah/) | The Γ-- language and toolchain, imported from the wah repo (commit `5d2318b`): spec, compilers, runtime, examples, editor support and project template. It is the seed for bootstrapping Wavert, and its contents will be converted to Wavert over time. This repo does not depend on the wah repo. |
| [src/](src/) | Programs, built by `build.ps1`. Γ-- (`.wah`) for now. |
| [build.ps1](build.ps1) | Builds every program in `src\` into `bin\` with the Γ-- compiler in `wah\` and Zig. |

## Building

`.\build.ps1` builds every program; `.\build.ps1 -Program hello -Run` builds and runs one. It needs [Zig](https://ziglang.org/download/) (on PATH, in `ZIG`, or passed with `-Zig`). The first build compiles the compiler from `wah\wahc\wahc.c` and caches it in `.wah\`.

The imported toolchain's tests need Node ≥ 22.3: run `npm test` in `wah\js`, `wah\wahc` or `wah\runtime`.
