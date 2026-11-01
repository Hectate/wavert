# Wavert for VS Code

Syntax highlighting, bracket matching, comment toggling, snippets, and documentation on hover and in completions for Wavert (`.wvt`).

## Install

Try it without installing, in an Extension Development Host window:

```
code --extensionDevelopmentPath=D:/git/wavert/editors/vscode D:/git/wavert/examples
```

VS Code opens a folder in only one window at a time. If `D:/git/wavert` is already open in another window, the development window cannot open it and VS Code focuses the existing window instead. That is why the command above opens `examples/`. Opening a single file works too.

To install it permanently, package a `.vsix`:

```
cd editors/vscode
npx @vscode/vsce package --skip-license --allow-missing-repository
code --install-extension wavert-language-0.3.1.vsix
```

After changing the extension, reload the window (**Developer: Reload Window**). An installed copy must be repackaged and reinstalled.

## What is highlighted

| Token | Scope |
|---|---|
| `42`, `-7`, `0xFF`, `0b101`, `1.5`, `2.5e-3` | `constant.numeric` |
| `true`, `false`, `null`, `nan`, `inf` | `constant.language` |
| `if`, `!if`, `while`, `!while`, `match`, `fail`, `onfail`, `panic` and the other keywords | `keyword.control.*`, `storage.*` |
| `;const total is …`, `;set x to …`, `;set x += …` | the name, with `is`, `to` and `+=` as `keyword.operator.assignment` |
| `;export fn name(a, var b)` | `export`, then the parameters and the function name |
| `;enum Light (red, green)` | the enum name and its members |
| `;import lib from "lib.wvt"`, `;end` | `lib` as `entity.name.namespace`; `from` and `end` as keywords |
| `x(f)`, `(f)`, `($print)`, `(lib.f)` | `entity.name.function.call`, `support.function.builtin` |
| `node.kind`, `{ kind: ... }` | `kind` as `variable.other.property` |
| `"score: {n}"` | interpolation as embedded code |
| Errors the lexer rejects: `12ab`, `=`, `++`, `>>>`, `$nope`, `node . kind`, unknown escapes, reserved words | `invalid.illegal.*` |

`is`, `to`, `from` and `end` are keywords only in their statements, so they are plain names elsewhere, as they are to the compiler.

## Documentation on hover and in completions

Hovering a name shows what it is and its documentation comment: the `//` lines directly above its declaration (§2.2 of the specification).

| Hover | Shows |
|---|---|
| A function, `const`, `var`, `ref` or enum of the file | its declaration and comment |
| `lib.name`, `lib.Enum`, where `lib` is imported | the export's declaration and comment, and the file it is from |
| `lib` itself | the import, and the comment at the top of the module |
| `$name` | its call form and notes from the specification's tables, and the section |

Typing `lib.` completes the module's exports, each with its signature and comment, and `$` completes the standard library's names. Wavert calls put the function after its arguments, so there is no help while typing arguments: the signature in the completion shows their order first.

`$lib/` imports are found in the standard library folder, as `wvc` finds it:

| Setting | Meaning |
|---|---|
| `wavert.libPath` | The standard library folder. |
| `wavert.wvcPath` | The `wvc` compiler; its `lib/` is next to it, or one folder up. |

With neither set, a file inside the Wavert repository uses the repository's `lib/`.

The extension reads files line by line, as the grammar does, rather than parsing them: `wvc` is the authority. Unsaved changes to an open file are included.

## Limits

TextMate grammars match regexes line by line and do not parse expressions. Two consequences:
- A `;set` statement whose `to` is on a later line highlights the rest of the statement as ordinary code.
- A record key on a line of its own is recognized by `name:` at the start of the line. A ternary whose `:` branch starts a line would be shown as a key.

Unknown names, arity errors and other semantic checks need a language server, which does not exist yet.

## Keeping it in sync

`wvc/test/grammar.test.js` checks the grammar against the compiler: keywords, reserved words, bang tokens, compound assignments, builtins and numbers. When the lexer or the stdlib changes, that test fails until the grammar is updated.

`builtins.json`, the documentation shown for `$names`, is generated from the specification's stdlib tables and the checker's arities:

```
node editors/vscode/scripts/builtins.mjs
```

`wvc/test/editor.test.js` fails until it is regenerated after either changes. It also checks that every export of the standard library has a documentation comment, and tests the scanner in `src/docs.js`.
