# Wavert

Wavert is a programming language with its own compiler, `wvc`, written in Wavert and compiling to C. It is being made ready for writing roguelikes; a roguelike will live in its own repository.

**Before writing or reading Wavert code, read [docs/wavert-quick-reference.md](docs/wavert-quick-reference.md).** The syntax is unusual (statements start with `;`, calls are postfix), and the pitfalls it lists are real. The [specification](docs/wavert-spec.md) is the authority. Prefer compiling a small test file over reasoning from the rules: `wvc` errors come with hints.

## Layout

| Path | What |
|---|---|
| `docs/wavert-spec.md` | The specification. Its header lists each draft's additions |
| `docs/wavert-quick-reference.md` | Writing Wavert, in brief |
| `wvc/` | The compiler, in Wavert. `wvc/wvc.c` is the bootstrap seed. Tests in `wvc/test/` |
| `runtime/wvt.c` | The C runtime inlined into every program |
| `lib/` | The standard library written in Wavert, imported as `$lib/name.wvt` |
| `examples/` | One short program per feature |
| `samples/` | Larger programs that combine features (walk, tokenize, crates) |
| `editors/vscode/` | The VS Code extension: grammar, snippets, hovers and completions |
| `wah/`, `tools/wah2wvt.wah` | Γ--, the language Wavert was bootstrapped from; rarely touched |

## Commands

Zig is the C compiler. If it is not on `PATH`, set `ZIG` to the executable; the tests skip without it.

```
zig cc -O2 -o .wvt/wvc wvc/wvc.c            # build wvc from the seed (.exe on Windows)
cd wvc && npm test                           # all tests, Node 22.3 or later (about 190, 1.5 min)
cd wvc && node --test test/lib.test.js       # one file
WVT_GC_STRESS=50 node --test test/lib.test.js   # with the collector running every 50 allocations
node editors/vscode/scripts/builtins.mjs     # regenerate builtins.json after changing a stdlib table in the spec
```

**After changing `wvc/*.wvt` or `runtime/wvt.c`, regenerate the seed.** Build `wvc` from the current `wvc.c`, compile `wvc/wvc.wvt` to `wvc/wvc.c` with it, rebuild, and repeat until `wvc.c` no longer changes (the fixed point). `bootstrap.test.js` checks it. Never edit `wvc.c` by hand. See [wvc/README.md](wvc/README.md).

## Conventions

- **A language change updates the spec, with a new draft note** in its header, together with the compiler, the tests, and the VS Code grammar (`grammar.test.js` fails until it matches). A new stdlib function also goes in the checker's table, in the code generator's `signalers` list if it can fail, and in a spec table row; `editor.test.js` and `stdlib.test.js` check that these agree.
- **Examples show one feature each, minimally.** A new feature gets a new small example, with its expected output in `wvc/test/examples.test.js`. Programs that combine features go in `samples/`.
- **`lib/` functions don't make callers check first.** Inputs that only differ in order, such as bounds or corners, are normalized; a function that can't give an answer for an input fails, so the caller handles it with `onfail`. Panics are only for inputs with no meaning, such as a die with 0 sides. Area functions use the part of an area that is on the grid.
- **Every export in `lib/` has a documentation comment**, the `//` lines directly above it, and every module a header comment starting `name:`; `editor.test.js` checks both. The extension shows them on hover.
- **Records are told apart by a `kind` field holding an enum member**, not by their fields. Map keys get their identity from their shape; build position keys with `grid.pos`.
- **Game maps are layers:** a terrain grid plus maps from position to actors or items, combined by `lib/view.wvt`. The player's memory of the map is separate from the terrain, and may disagree with it.
- Tests check exact output, and are run with and without the collector stress setting when they touch allocation.

## Editing files with scripts

The Write and Edit tools are safest for Wavert text, which is full of `$`, `{` and `\`. Shell heredocs and `sed` can mangle backslashes and dollar signs. In a Node script, never pass a plain string as `String.replace`'s replacement, since `$'` and `` $` `` in it insert text from the document; use `s.replace(a, () => b)`. Files checked out on Windows may have CRLF line endings.
