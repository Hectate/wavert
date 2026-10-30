# Γ-- for VS Code

Syntax highlighting, bracket matching, comment toggling and snippets for Γ-- (`.wah`).

## Install

Try it without installing, in an Extension Development Host window:

```
code --extensionDevelopmentPath=D:/git/wah/editors/vscode D:/git/wah/examples
```

VS Code opens a folder in only one window at a time. If `D:/git/wah` is already open in another window, the development window cannot open it and VS Code focuses the existing window instead. That is why the command above opens `examples/`. Opening a single file works too.

Install it permanently by packaging a `.vsix`:

```
cd editors/vscode
npx @vscode/vsce package --skip-license --allow-missing-repository
code --install-extension wah-language-0.0.4.vsix
```

After changing the grammar, reload the window (**Developer: Reload Window**). An installed copy must be repackaged and reinstalled.

## What is highlighted

| Token | Scope |
|---|---|
| `wah`, `WAAH`, `wa[42]h`, `wa[0.5]h` | `constant.numeric` |
| `WAH!`, `WAAAH!` (signals) | `keyword.control.signal` |
| `!doubt`, `!lurk`, `!invited` and the other keywords | `keyword.control.*`, `storage.*` |
| `$print` and the other builtins | `support.function.builtin` |
| `;scheme (a, b)name` | parameters and the function name |
| `x(f)`, `,(f)` | `entity.name.function.call` |
| `x(f.lib)`, `;"lib.wah" steal lib` | `f` as a call, `lib` as `entity.name.namespace` |
| `kind.node`, `{ kind: ... }` | `kind` as `variable.other.property` |
| `"score: {n}"` | interpolation as embedded code |
| Errors the lexer rejects: `Waah`, `42`, `=`, `>>>`, `$nope`, `kind . node`, unknown escapes | `invalid.illegal.*` |

## Limits

TextMate grammars match regexes line by line and do not parse expressions. Two consequences:
- `(name)` is a call after an expression and grouping at the start of one. The grammar guesses from the character before the `(`. It knows that `!doubt (x)`, `lurk (x)` and `getaway (x)` are not calls, but some rarer cases may be guessed wrong.
- A record key on a line of its own is recognized by `name:` at the start of the line. A ternary whose `:` branch starts a line would be shown as a key.

Unknown names, arity errors and other semantic checks need a language server, which does not exist yet.

## Keeping it in sync

`js/test/grammar.test.js` checks the grammar against the compiler: keywords, reserved words, bang tokens, compound cheats, builtins, numerals and signals. When the lexer or the stdlib changes, that test fails until the grammar is updated.
