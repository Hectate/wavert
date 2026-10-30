# Building and debugging this project

This project is written in **Γ--** (gamma--, "wah"), a deliberately inverted programming language: statements begin with `;`, calls are postfix, numbers are spelled `wah`, and `if` is `!doubt`. Source files end in `.wah`.

`wahc`, the Γ-- compiler, compiles a program to C, and [Zig](https://ziglang.org) compiles the C into a native executable. `build.ps1` does both for every program in `src\`.

**Who writes what.** The author writes this project's Γ-- code. This guide is for building it and for helping debug it: explaining a compiler diagnostic or a runtime error, finding why a program misbehaves, and pointing at the rule in the spec. Γ-- has no IDE tools beyond syntax highlighting (no language server, no debugger), so debugging works from the compiler's diagnostics, the program's output and the techniques under [Debugging](#debugging). Suggest fixes; do not rewrite the code unless asked.

## What you need

1. **A checkout of the wah repository.** It holds the compiler, its C runtime and the language definition. The scripts find it through the `WAH_HOME` environment variable, which defaults to `D:\git\wah`. The files used are:
   - `wah-spec.md`: the language specification. It is the authority on syntax and behavior; read it before writing Γ--.
   - `examples\`: working programs. Copy their idioms.
   - `wahc\wahc.c`: the compiler, as one C file.
   - `runtime\wah.c`: the runtime that every compiled program includes.
2. **Zig**, any recent release. Zig is one folder with nothing to install: download the archive for your platform from [ziglang.org/download](https://ziglang.org/download/) and extract it, or use `winget install zig.zig`. The script finds it through `-Zig`, else the `ZIG` environment variable (the full path of `zig.exe`, not its folder), else `zig` on `PATH`.
3. **Windows PowerShell 5.1 or PowerShell 7**, to run `build.ps1`.

```powershell
# For this session; use [Environment]::SetEnvironmentVariable(name, value, 'User') to keep them.
$env:WAH_HOME = 'D:\git\wah'
$env:ZIG = 'D:\zig\zig.exe'
```

## Layout

| Path | Contents |
|---|---|
| `src\*.wah` | Programs. A program's last statement is `;!invited`. Each one becomes `bin\<name>.exe`. |
| `src\**\*.wah` | Libraries: files that end with a `bribe` statement. Programs steal them; they are not built on their own. |
| `bin\` | Executables, and the C that `wahc` generated for each (`<name>.c`). Build output: do not commit it. |
| `.wah\` | The compiler, built from the wah checkout on first use. Do not commit it. |
| `build.ps1` | The build script. |

Add `bin/` and `.wah/` to `.gitignore`.

## Building

```powershell
.\build.ps1                                   # every program in src\
.\build.ps1 -Program mansion                  # one program, by name or path
.\build.ps1 -Program mansion -Run             # build it, then run it
.\build.ps1 -Program mansion -Run -RunArgs 'save1', 'extra'
.\build.ps1 -Clean                            # delete bin\ and the cached compiler first
```

If PowerShell refuses to run scripts, use `powershell -ExecutionPolicy Bypass -File .\build.ps1`.

The first run builds the compiler into `.wah\wahc.exe`. The first time a given Zig is used, this takes about half a minute while Zig builds its C library cache. Later builds take a second or two per program. The compiler is rebuilt when the checkout's `wahc\wahc.c` is newer.

The script stops with exit code 1 if any program fails to build, after printing the compiler's diagnostics.

### What the script runs

For each program it runs `wahc build`:

```powershell
.wah\wahc.exe build src\mansion.wah -o bin\mansion.exe --cc D:\zig\zig.exe --runtime D:\git\wah\runtime\wah.c
```

`wahc build` writes `bin\mansion.c`, then runs `zig cc -O2 -o bin\mansion.exe bin\mansion.c`. To do it by hand:

```powershell
& $env:ZIG cc -O2 -o .wah\wahc.exe "$env:WAH_HOME\wahc\wahc.c"     # the compiler; keep the 'cc'
.wah\wahc.exe src\mansion.wah --target c -o bin\mansion.c --runtime "$env:WAH_HOME\runtime\wah.c"
& $env:ZIG cc -O2 -o bin\mansion.exe bin\mansion.c
```

Without `cc`, Zig reads `-O2` as a command and fails with "unknown command".

## Running and testing

Programs read input a line at a time with `$readLine`, so they can be tested by piping text in:

```powershell
"look", "go north", "quit" | .\bin\mansion.exe
Get-Content tests\walkthrough.txt | .\bin\mansion.exe > out.txt
```

**Byte-order mark.** PowerShell may send a UTF-8 byte-order mark before piped text. Γ-- keeps it as the character U+FEFF at the start of the first line (spec §12.1), so the first command compares unequal to `"look"`. Read input through a helper that removes it:

```
// $readLine, without a byte-order mark that a pipe may put before the first line.
// At end of input it signals WAH!, like $readLine, so callers can whine.
;scheme ()readInput {
    ;,($readLine) claim line
        whine { ;WAH! }
    (line($length)) > wh && (line, wh($at)) == "\u{FEFF}" ? (line, wah, (line($length))($slice)) : line
}
```

Exit codes:

| Code | Meaning |
|---|---|
| 0 | The program ended with `;!invited` |
| *n* | The program ended with `;n !invited` |
| 1 | An uncaught tantrum: a runtime error, or `;tantrum value`. The message and the scheme stack go to stderr. |
| 2 | From the compiler or `build.ps1`: a compile error |
| 141 | The program's output was closed before it finished writing (a broken pipe) |

## Writing Γ--: the essentials

Read `wah-spec.md` for the full rules. The points below are the ones newcomers trip over.

**Basics**
- Every statement starts with `;`. Comments are `//`.
- Numbers are words:
  - `wh` = 0, `wah` = 1, `waah` = 2, `waaah` = 3.
  - All uppercase negates: `WAH` = -1.
  - Larger and fractional numbers use brackets: `wa[42]h`, `wa[0x1F]h`, `WA[7]H` = -7, `wa[0.5]h`.
  - Plain `42` is an error, and so is mixed case (`Waah`).
- Values come first: `;wah claim x` binds `x` to 1.
  - `claim` is constant.
  - `claim shifty` makes a mutable value.
  - `claim fickle` can be rebound but keeps each value as it is.
  - `;waah cheat x` assigns; `;wah +cheat x` and `;++cheat x` work too.
- `real`, `fake` and `uninvited` are true, false and null.

**Calls are postfix and bind loosest**
- `;x, y(f)` is `f(x, y)`, and `;,(f)` calls `f()`. `;x(f)(g)` is `g(f(x))`.
- A call takes every comma-separated value to its left, so parenthesize calls used as operands or elements:
  - `i < (xs($length))`
  - `[a, (b(f))]`
  - `(c(isSpace)) || done`
- A block's value that is a zero-argument call must be wrapped: `(,(f))`.

**Schemes (functions)**
- `;scheme (a, b)add { a + b }`. A block's value is its last expression **without** a leading `;`.
  - `{ ;a + b }` has no value. If a value is used, it must be written out: end with `uninvited` if nothing else.
- `;getaway x` returns early.
- Plain parameters cannot be reassigned, and a shifty array or record argument arrives as a frozen snapshot (a copy). `shifty` parameters can be changed and share a shifty argument; `fickle` parameters can be reassigned and take the argument as it is, without copying.
- Nested schemes can read and `cheat` the bindings around them.
- Schemes are not values: no closures, no passing functions.

**Control flow**
- `;!doubt (c) { ... } anyway !doubt (d) { ... } anyway { ... }` is if / else if / else; `doubt` means "unless".
- A conditional whose value is used needs a final plain `anyway { ... }`. Conditions must be `real` or `fake`.
- `;lurk (c) { ... }` is a while loop and `;!lurk (c) { ... }` an until loop. `;bail` is break, `;loiter` is continue.
- `;suspect (x) { alibi "go", "walk" { ... } alibi "look" { ... } anyway { ... } }` is a switch with no fallthrough.
- `c ? a : b` works as usual.

**Data**
- **Arrays.** `[a, b]` is frozen. `i[arr]` indexes: the index comes first, and negative indexes count from the end. `;arr, x($push)`, `$pop` and `;x cheat i[arr]` need a shifty array: `;[] claim shifty arr`.
- **Records.** `{ kind: "door", open: fake }` has a fixed set of fields.
  - Read a field with `open.door`: the field comes first, and chains read inside out, so `name.owner.item` is `item.owner.name`.
  - `k[rec]` reads the field named by the value of `k`.
  - Writing `;real cheat open.door` needs a shifty record.
  - Keywords (`claim`, `shifty`, `fickle`, `doubt`, ...) cannot be field names.
- **Lineups (enumerations).** `;lineup (north, south, east, west)Dir` declares one, and members are written `north.Dir`. A `suspect` over a lineup that lists every member needs no `anyway`.
- **Strings ("purples").**
  - Interpolation: `"HP: {hp}/{maxHp}"`. A literal brace is `{{` or `}}`.
  - `+` joins two purples only. Convert other values with interpolation or `$toPurple`.
- **No maps.** Use arrays of records and search them.

**Errors**
- `;WAH!` inside a scheme sends a signal to its caller, and only to its caller.
- The caller handles it on the statement: `;s(parse) claim n whine (code) { fallback }`.
  - The stdlib signals the same way: a missing file, end of input, a bad number in `$toInt`, or an unset variable in `$env`.
  - An unhandled signal becomes a tantrum.
- `;tantrum "message"` stops the program with exit code 1. Tantrums cannot be caught.

**Modules**
- `;"lib/items.wah" steal items` must come before every other statement. It names the module `items` in this file.
- A library's last statement lists what it exports: `;take, drop, Item bribe`.
  - Use the exports as `count.items` (a claim), `;x(take.items)` (a call) and `sword.Item.items` (a lineup member).
  - A library's top level may contain only `steal`, `scheme`, `claim` and `lineup`.
- A steal path is relative to the stealing file, uses `/`, and includes `.wah`.

**The program**
- The entry file's last statement is `;!invited` (exit code 0), or `;code !invited`.

**Standard library** (spec §9.4, §10.6, §11.3, §12)
- Console: `$print` `$write` `$eprint` `$readLine`.
- Files: `$readFile` `$writeFile` `$appendFile` `$fileExists` `$listDir` `$deleteFile`.
- Process: `$args` `$env` `$programPath` `$run`.
- Purples and arrays: `$length` `$at` `$slice` `$split` `$join` `$code` `$fromCode` `$toPurple` `$toWah` `$push` `$pop` `$same`.
- Numbers: `$toInt` `$toFloat` `$floor` `$ceil` `$round` `$trunc` `$toUint32` `$isNobody`.
- Reflection: `$typeOf` `$fields`.

**What the language does not have**
- **Random numbers or a clock.** Write a small generator, seeded from input or `$args`. Ints are exact up to 2^53, so the Park–Miller generator fits (the seed must be 1 to 2147483646):
  ```
  ;wa[12345]h claim shifty seed
  ;scheme (n)roll {
      ;seed * wa[48271]h % wa[2147483647]h cheat seed
      seed % n + wah                  // 1..n
  }
  ```
  Bitwise operators need int32 values, so generators built on them (such as xorshift) need care.
- **Single-key input.** Input is line by line.
- **Maps, closures, and catching tantrums.**
- **A `$print` for arrays or records.** `$print` takes ints, floats, booleans, purples, `uninvited` and lineup members. Format arrays with `$join`.
- **Integer division.** `/` always gives a float. Write `(a / b)($floor)`.
- **`$` before a name in a string.** `"cost: $gold"` prints `$gold` literally and warns `did you mean {gold}?`. Interpolation is `{gold}`; write `\$` for a literal `$`.

## Debugging

There is no debugger. These are the tools:

- **Compiler diagnostics.** Errors and warnings give `file:line:column`, a message, often a `hint:` line, and the source line with a caret. The compiler stops at the first syntax error. The checker reports every error it finds. Warnings, such as `this statement has no effect`, often point at a real bug: a `;` that discards a block's value.
- **Runtime errors (tantrums).** They print the message and the schemes the error passed through, innermost first, then exit with code 1:
  ```
  Waluigi was not invited to this exception
  tantrum: index 1 is out of range for an array of length 1
      in second
      in run
  ```
  An unhandled `WAH!` reads `unhandled WAH! (code 1) from '$readLine' in 'loop'`: a signal reached a caller that did not `whine`.
- **Printing.** `;"hp={hp} room={name.room}"($eprint)` writes to stderr, so it stays out of the program's output.
  - `;x($typeOf)($eprint)` shows a value's type: `int`, `float`, `purple`, `array`, `record`, `boolean`, `uninvited`, or a lineup's name.
  - Arrays and records cannot be printed directly. Use `;xs, ", "($join)($eprint)`, and `;rec($fields)` for a record's field names.
- **Reproducible runs.** Pipe a file of commands in, and save the output, for example `Get-Content tests\walkthrough.txt | .\bin\mansion.exe > out.txt`. If the program uses random numbers, seed them from `$args` so a run can be repeated exactly.
- **How the compiler reads a file.** These help when a line does not parse the way it looks:
  - `.wah\wahc.exe --tokens src\file.wah` lists the tokens.
  - `.wah\wahc.exe --ast src\file.wah` prints the syntax tree. It is the quickest way to see which arguments a postfix call takes, or where a statement ends. In the tree, a call `x, y(f)` is a `call` node with `args`.
- **The generated C.** `bin\<name>.c` is readable. Each scheme is a C function named after it, such as `w_take` or `s12_helper`. The language runtime is at the top of the file.
- **Small experiments.** To check what a construct does, put it in a scratch program in `src\`, build it alone with `-Program`, and delete it afterwards. With Node and the wah checkout, `node $env:WAH_HOME\js\bin\wahc0.js -e ';wah / waah($print)'` runs a snippet without building.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `Zig not found` | Set `ZIG` to the full path of `zig.exe`, put Zig's folder on `PATH`, or pass `-Zig`. |
| `unknown command: -O2` from Zig | A manual `zig` command is missing `cc`: `zig cc -O2 ...`. |
| `No wah checkout at ...` | Set `WAH_HOME` to the wah repository, or pass `-WahHome`. |
| `file.wah:3:5: error: ...` | A compile error, with file, line and column. The hint line usually gives the fix. |
| `'f' never produces a value, but its value is used` | The scheme's last line starts with `;`. Remove the `;`, or end the scheme with `uninvited`. |
| `comma-separated values must be consumed by a call` | A call is missing, or a block value needs wrapping: `(,(f))`. |
| `Waluigi was not invited to this exception` | A runtime error (a tantrum). The message and the scheme stack follow on stderr. |
| The first command is not recognized when input is piped | The byte-order mark: read input with `readInput` above. |

## Editor support

The wah checkout has a VS Code extension in `editors\vscode` for highlighting, brackets and snippets. If its `.vsix` has not been packaged yet, see `editors\vscode\README.md`. Then install it:

```powershell
code --install-extension "$env:WAH_HOME\editors\vscode\wah-language-0.0.4.vsix"
```
