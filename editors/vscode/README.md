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
code --install-extension wavert-language-0.4.0.vsix
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

Typing `lib.` completes the module's exports, each with its signature and comment, and `$` completes the standard library's names. Other names complete to the file's own declarations and imports, and to the words of the file.

## Writing calls

Wavert calls put the function after its arguments, so completing a function writes out the whole call, with a placeholder for each argument. Type the argument over the selected placeholder, and press Tab to move to the next one, then past the call:

| Typed | Completed |
|---|---|
| `;(grid.po` | `;(x, y)(grid.pos)`, with `x` selected |
| `;$pri` | `;x($print)` |
| `;$ti` | `;($time)` |
| `;wa`, for the file's `fn walk(ref g, var at, steps)` | `;(g, at, steps)(walk)` |

Completing a call inside a placeholder nests it: Tab goes through the inner call's placeholders first, then carries on with the outer ones. While the suggestion list is open, Tab accepts the suggestion instead, so close the list with Escape first. Escape with no list open ends the placeholders, keeping the text.

Where the function of a call is being written, after an expression and `(` as in `xs(grid.po`, a completion inserts only the name.

While a placeholder is selected, VS Code does not open the suggestion list as you type, except after `.` and `$`. Press Ctrl+Space for the file's own functions.

Inside the arguments of a call, as in `(1, |)(grid.pos)`, parameter hints show the function's parameters, with the one at the cursor in bold. They open after typing `(` or `,` in a call whose function is already written, and after completing a call with two or more parameters. Ctrl+Shift+Space opens them anywhere.

`$lib/` imports are found in the standard library folder, as `wvc` finds it:

| Setting | Meaning |
|---|---|
| `wavert.libPath` | The standard library folder. |
| `wavert.wvcPath` | The `wvc` compiler; its `lib/` is next to it, or one folder up. |

With neither set, a file inside the Wavert repository uses the repository's `lib/`.

The extension reads files line by line, as the grammar does, rather than parsing them: `wvc` is the authority. Unsaved changes to an open file are included.

## Checking as you edit

The extension checks each open file with `wvc --types` when it opens, as you type (once typing pauses) and when any file is saved. It checks the text in the editor, saved or not.

- **Errors and warnings** from `wvc` are shown in the file, and in the files it imports. A file that does not parse shows its first syntax error.
- **Types** that `wvc` infers (§4.2 of the specification) appear in hovers: a binding's type, what a function returns, and a field's type, as in `state.cursor`. Hovering a local binding, parameter or loop variable shows its type, where it is known.
- **Fields:** typing `p.` after a binding holding a record whose fields are known, such as one from `grid.pos` or a record literal, completes its fields.

The types come from the last version of the file that parsed, so while a line is half written, hovers and completions use what was known before it. A name is matched to the nearest declaration of it above the cursor.

| Setting | Meaning |
|---|---|
| `wavert.check` | Check files as they are edited. On by default. |
| `wavert.wvcPath` | The `wvc` that checks them. Empty: a `wvc` built in the Wavert repository the file is in, in `.wvt/` or `wvc/`. Without one, nothing is checked. |

## Limits

TextMate grammars match regexes line by line and do not parse expressions. Two consequences:
- A `;set` statement whose `to` is on a later line highlights the rest of the statement as ordinary code.
- A record key on a line of its own is recognized by `name:` at the start of the line. A ternary whose `:` branch starts a line would be shown as a key.

Checking runs the whole of `wvc` for each check, on the file and what it imports; a language server would answer faster and per keystroke, and does not exist yet.

## Keeping it in sync

`wvc/test/grammar.test.js` checks the grammar against the compiler: keywords, reserved words, bang tokens, compound assignments, builtins and numbers. When the lexer or the stdlib changes, that test fails until the grammar is updated.

`builtins.json`, the documentation shown for `$names`, is generated from the specification's stdlib tables and the checker's arities:

```
node editors/vscode/scripts/builtins.mjs
```

`wvc/test/editor.test.js` fails until it is regenerated after either changes. It also checks that every export of the standard library has a documentation comment, and tests the scanner in `src/docs.js`.
