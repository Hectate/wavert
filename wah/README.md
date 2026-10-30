# Γ-- (gamma--, "wah")

A deliberately inverted programming language: statements *begin* with `;`, calls are postfix, numbers are spelled `wah`, and `if` is `!doubt`. Only the syntax is inverted; operators keep their usual meaning. Bizarre but usable.

```
;scheme (a, b)average {
    (a + b) / waah
}

;"Hello, World"($print)
;wah, waah(average)($print)        // 1.5
;!invited
```

The full language definition is in [wah-spec.md](wah-spec.md).

## Quick tour

| Γ-- | Meaning |
|---|---|
| `;wah claim x` | `const x = 1` |
| `;wh claim shifty i` / `;++cheat i` | `let i = 0` / `i++` |
| `;node claim fickle head` / `;next.head cheat head` | `let head = node` / `head = head.next`, rebound without copying |
| `wah`, `waah`, `WAH`, `wa[100]h`, `wa[0.5]h` | `1`, `2`, `-1`, `100`, `0.5` |
| `;x, y(f)` / `;,(f)` | `f(x, y)` / `f()` |
| `i[arr]` | `arr[i]` |
| `;{ kind: "num", value: wah } claim node` | a record (fixed set of fields) |
| `kind.node`, `name.callee.node`, `k[node]` | `node.kind`, `node.callee.name`, `node[k]` |
| `;!doubt (c) { } anyway { }` | `if (c) { } else { }` |
| `;doubt (c) { }` | `unless (c) { }` |
| `;suspect (x) { alibi "a", "b" { } anyway { } }` | `switch` without fallthrough |
| `;lineup (red, green)Light` / `red.Light` | `enum Light { red, green }` / `Light.red` |
| `;lurk (c) { }` / `;!lurk (c) { }` | `while` / `until` |
| `;scheme (a, b)add { a + b }` | function; the last bare expression is the result |
| `real`, `fake`, `uninvited` | `true`, `false`, `null` |
| `;WAH!` + `whine (c) { ... }` | a one-frame signal handled by the caller |
| `;"lib.wah" steal lib` / `;a, b bribe` / `x(f.lib)` | `import lib` / `export` / `lib.f(x)` (§13.1) |
| `;!invited` | end of program (required) |

## Using the stage 0 compiler

`wahc0` compiles `.wah` files to JavaScript. It needs Node ≥ 22.3 and has no dependencies.

```
node js/bin/wahc0.js run examples/hello.wah
node js/bin/wahc0.js run examples/fizzbuzz.wah
node js/bin/wahc0.js run examples/records.wah
node js/bin/wahc0.js run examples/suspect.wah
node js/bin/wahc0.js run examples/lineup.wah
node js/bin/wahc0.js run examples/modules.wah
node js/bin/wahc0.js run examples/fickle.wah
node js/bin/wahc0.js run examples/tokenize.wah examples/fizzbuzz.wah
node js/bin/wahc0.js examples/fizzbuzz.wah -o fizzbuzz.js && node fizzbuzz.js
node js/bin/wahc0.js -e ';"inline"($print)'
```

Run the tests with `cd js && npm test`, and the stage 1 tests with `cd wahc && npm test`. See [js/README.md](js/README.md) for the compiler's layout and the stage 0 details.

## Native programs

Both compilers also compile to C with `--target c`, and `build` turns a `.wah` file into a native program in one step. It needs a GNU C compiler; [Zig](https://ziglang.org) works on every platform (see [Getting Zig](#getting-zig)):

```
node js/bin/wahc0.js build examples/fizzbuzz.wah        # writes examples/fizzbuzz.c and builds examples/fizzbuzz(.exe)
node js/bin/wahc0.js examples/fizzbuzz.wah --target c   # just the C
```

`wahc/wahc.c` is `wahc` itself compiled to C, so a native compiler needs nothing but a C compiler:

```
zig cc -O2 -o wahc/wahc.exe wahc/wahc.c
wahc/wahc.exe build examples/fizzbuzz.wah
```

These commands assume `zig` is on `PATH`. If it is not, write its full path, and keep the `cc`: `D:\zig\zig.exe cc -O2 -o wahc/wahc.exe wahc/wahc.c`. Without `cc`, Zig reads `-O2` as a command and fails with "unknown command". For `build`, set `ZIG` to the same path first, or add `--cc D:\zig\zig.exe` (see [Getting Zig](#getting-zig)).

See [runtime/README.md](runtime/README.md) for the C runtime.

### Getting Zig

Zig is one folder with nothing to install. `wahc` uses only `zig cc`, its C compiler, so any recent release works; the tests were run with a 0.18 development build.

1. Download the archive for your operating system and CPU from [ziglang.org/download](https://ziglang.org/download/): for example `zig-x86_64-windows-….zip` for 64-bit Windows, `zig-x86_64-linux-….tar.xz` for Linux on Intel or AMD, `zig-aarch64-macos-….tar.xz` for Apple silicon. Extract it anywhere, such as `C:\tools\zig` or `~/zig`. A package manager works too: `winget install zig.zig` or `scoop install zig` on Windows, `brew install zig` on macOS.
2. Check it: `C:\tools\zig\zig.exe version`, or `~/zig/zig version`.
3. Tell the compilers where it is, in one of three ways. They are tried in this order:
   - `--cc <path>` on the command line: `wahc0 build prog.wah --cc C:\tools\zig\zig.exe`.
   - The `ZIG` environment variable, set to the full path of the `zig` executable (the file, not its folder).
   - `zig` on `PATH`: add the extracted folder to `PATH`.

Setting `ZIG`:

| Shell | For this session | For good |
|---|---|---|
| PowerShell | `$env:ZIG = 'C:\tools\zig\zig.exe'` | `[Environment]::SetEnvironmentVariable('ZIG', 'C:\tools\zig\zig.exe', 'User')`, then open a new terminal |
| cmd | `set ZIG=C:\tools\zig\zig.exe` | `setx ZIG C:\tools\zig\zig.exe`, then open a new terminal |
| Git Bash on Windows | `export ZIG=/c/tools/zig/zig.exe` | the same line in `~/.bashrc` |
| bash or zsh | `export ZIG=~/zig/zig` | the same line in `~/.bashrc` or `~/.zshrc` |

Git Bash turns `/c/tools/...` into `C:/tools/...` for Windows programs, so either form works there.

The first `zig cc` run takes half a minute or so while Zig builds its C library cache; later builds take about a second.

The test suites use the same variable. Without Zig they skip their native tests.

| Variable | Used by | Meaning |
|---|---|---|
| `ZIG` | `build`, and the tests in `js/`, `wahc/` and `runtime/` | The `zig` executable |
| `WAH_TARGET=c` | `cd js && npm test` | Run the whole `wahc0` suite through the C backend (several minutes) |
| `SEED` | `cd runtime && npm test` | Seed for the runtime's random comparison cases |

## Repository

| Path | Contents |
|---|---|
| [wah-spec.md](wah-spec.md) | Language specification |
| [js/](js/) | `wahc0`, the stage 0 compiler in JavaScript, with tests |
| [wahc/](wahc/) | `wahc`, the stage 1 compiler in Γ-- (complete; compiles itself), and `wahc.c`, the native bootstrap seed |
| [runtime/](runtime/) | The C runtime that programs compiled with `--target c` include |
| [templates/wah-project/](templates/wah-project/) | A starting point for a project written in Γ--: `build.ps1` builds its programs with `wahc` and Zig, and `BUILDING.md` explains building and debugging |
| [examples/](examples/) | Hello World, echo, FizzBuzz, records, `suspect`, lineups, modules, `fickle` bindings, and a Γ-- tokenizer written in Γ-- |
| [editors/vscode/](editors/vscode/) | VS Code syntax highlighting and snippets |

## Bootstrap plan

1. **Stage 0:** `wahc0` (JS) compiles Γ-- to JS. Done.
2. **Stage 1:** write `wahc` in Γ-- and compile it with `wahc0`. Done; see [wahc/](wahc/).
3. **Stage 2:** `wahc` compiles itself, and the output must match stage 1 byte for byte. Done: the tests check this fixed point.
4. **C backend:** `--target c` in both compilers. Done.
5. **Native bootstrap:** `wahc/wahc.c` builds a native `wahc` that compiles itself back to the same `wahc.c`. Done.
6. Retire `wahc0`.

## License

MIT. See [LICENSE](LICENSE).
