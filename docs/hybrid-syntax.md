# Wavert: a hybrid of Γ-- syntax (proposed changes)

> This is the design document that derived Wavert from Γ--. The language itself is specified in [wavert-spec.md](wavert-spec.md).

Γ-- inverts C-style syntax but keeps standard operator semantics (spec: [wah/wah-spec.md](../wah/wah-spec.md), Draft 0.8). Writing programs in it showed that only some of the inversions help. This document lists what Wavert, a "hybrid" syntax derived from Γ--, keeps, changes and drops. It also replaces every themed element (keywords, numerals, signals, type and constant names) with a standard equivalent, so that the result is judged on structure alone.

Goal: keep the inversions that make code easier to write, parse and debug, and remove the ones that make it harder to read and to support in an editor.

## Keep

| Feature | Spec | Why |
|---|---|---|
| Statements start with `;` | §2.1 | Unambiguous statement boundaries and easy error recovery. Expressions can span lines freely. |
| Block value is the trailing expression without a leading `;` | §7.2 | You can see at the left margin whether a line is a statement or the block's result. Keep the explicit-value checks and the "no effect" warning. |
| Postfix calls | §6.2 | Calls read as a pipeline (`x(parse)(validate)(save)`), and a step can be added or removed at the end of an expression. **Precedence changes; see below.** |
| `!` prefix on control keywords | §2.4, §7 | `!if` (unless) and `!while` (until) negate the whole structure without wrapping the condition in parentheses. |
| Required end-of-file marker | §13 | A truncated or half-merged file fails to compile. A program ends with `;exit` (also used for early exit, `;exit code`), and a library with `;end` (change 5). |
| Swallowed-value warnings | §2.8, §7.2 | A line starting with `[` or `,` can still continue the previous statement, so keep the warnings for it. |
| `$` sigil on stdlib names | §2.9 | Not themed, and it means adding a stdlib name can never break user code. |

## Change

### 1. Calls bind tightest
Today calls bind loosest (§11.4 level 16), so `1 + x(f)` means `f(1 + x)`, and any call used as an operand needs parentheses.

- Move postfix calls to level 1, together with indexing and member access.
- `i < xs($length)` and `c(isSpace) || c == ";"` then need no parentheses.
- To apply a call to a whole expression, group it: `(a + b)($print)`.
- **No pipeline operator for now.** A low-precedence `|>` would save only one pair of parentheses, and in the current `wahc` source calls are rarely applied to an ungrouped compound expression. A `|>` only helps with chains that pass extra arguments (`((x, y)(f), z)(g)`) if it has a topic placeholder, which adds a token, a precedence level and a second way to write every call. A local binding does the same job. `|>` is already a syntax error, so it can be added later without breaking code. Revisit after the `wahc` source is converted, by counting real inside-out chains.

### 2. Arguments are grouped explicitly; remove comma capture
Today `(f)` takes every comma-separated expression to its left (§6.2), so `[x, y(f)]` means `[f(x, y)]`, and zero-argument calls are written `,(f)`.

| Arity | Current | Wavert |
|---|---|---|
| 1 | `x(f)` | `x(f)` (unchanged) |
| many | `a, b(f)` | `(a, b)(f)` |
| 0 | `,(f)` | `(f)` |

- Arrays and records then follow the same rule: `[x, y(f)]` is `[x, f(y)]`.
- Remove the nesting rule and the zero-argument capture error.
- **Zero-argument calls:** `(name)` or `(module.name)` at the start of an expression is always a call; any other `( … )` there is grouping. This is purely syntactic, and it works because functions are not values (§6.1), so grouping a lone name is never useful.
  - If `name` is a variable, it is a compile error with the hint "`x` is not a function; remove the parentheses".
  - A swallowed block value fails safely. In `{ ;x($print) (next) }`, the `(next)` continues the statement as `next($print(x))`, and the compile-time arity check rejects it.
  - If functions ever become values, this form becomes ambiguous and has to be revisited.
  - Rejected alternatives: `()(f)` (noisy), `f!` (reads as negation, and `f!==x` lexes as `!=` `=`), `f()` (reverses the call direction only for zero arguments), and `<a, b>(f)` argument brackets (the closing `>` is ambiguous with comparison, and `<<`/`>>` collide in the lexer).

### 3. Member access and indexing in normal order
Today they are inverted: `field.rec` and `i[arr]` (§10.2, §10.8).

- Use `rec.field` and `arr[i]`. Chains then read left to right, the same direction as the pipeline calls: `items[i].kind`.
- This makes editor completion after `.` possible again.
- Enum members and module members become `Kind.call` and `lexer.tokenize`.
- **No methods or UFCS.** `.` is member access only, and calls stay one-way. `x.f(y)` keeps its plain parse, `y(x.f)`, which is a compile error because functions are not values. Supporting UFCS would make the parse of `.name(` depend on whether `name` is a field or a function, and would reverse the call direction. Postfix calls already cover left-to-right chaining (`x(parse)(validate)`) and receiver-first calls (`(items, 3)(push)`).

### 4. Names come first in declarations; no `=`
Today the value comes first (`;value claim name`) and parameters come before the scheme name (`;scheme (a, b)name`).

`=` stays out of the language entirely, because new programmers often confuse it with `==`. A bare `=` remains a lex error whose hint points to the right form, as it does today (§2.7). Equality stays `==`.

**Bindings and assignment:**

```
;const count is 0         // declare (frozen)
;var total is 0           // declare (mutable)
;ref head is node         // declare an alias (rebindable, never copies)
;set total to total + 1   // assign
;set items[i] to x        // assign an element
;set node.kind to "num"   // assign a field
;set total += 1           // compound forms replace "to"; "+=" is one token, so it is never read as "="
```

- **Three declarators, one per binding kind:** `const`, `var` and `ref` replace `claim`, `claim shifty` and `claim fickle`. `set` replaces `cheat`. The §10.4 copy rules are unchanged.
  - `const` is **deep**: the binding can't be reassigned, and the value is frozen (assigning a mutable array to it takes a snapshot). JS `const` is shallow, but the difference fails safely: mutating a frozen value is an immediate runtime error.
  - `var` is mutable. `ref` shares the reference without copying and can be reseated with `set`; it is the cheapest way to walk or read a mutable structure.
  - Parameters use the same words: `;fn eat(var toks)`, `;fn walk(ref node)`.
  - **Why not `let`:** it means a mutable variable in JS/TS but an immutable one in Rust, Swift and OCaml, so some readers would always get it backwards. `const` reads as "can't change" and `var` as "can change" in every mainstream language that has them. Rejected: `let`/`let mut` (Rust), `let`/`var` (Swift), and a neutral `bind` (familiar to no one).
- Putting the keyword first keeps "this line declares or assigns, and what kind of binding it is" visible at the left margin, the same as every other statement kind.
- **`is` and `to` are contextual keywords.** They are special only right after a declared name or a `set` target, and remain usable as identifiers elsewhere. The current `wahc` source uses neither as an identifier.
  - `is` reads naturally (`;const count is 0`). Because other languages use `is` as a type-test or identity operator, a future type test should use a different word.
- **Why a separator:** a `set` target is an lvalue that can end in indexing, so with no separator a value starting with `[` would merge into it (`;set row [0, 0]` would parse as the target `row[0, 0]` with no value). Values starting with `-` or `(name)` cause the same problem unless the lvalue grammar is restricted. The separator also keeps planned features unambiguous: type annotations (§16) as `;const x: int is 5`, and destructuring as `;const [a, b] is pair`.
- **Rejected alternatives:** the plain form `;const x 0` (has the merge traps above), `be` (reads well after `let` but not after `const`/`var`), `:=` (brings `=` back into view), and infix `;total assign total + 1` (a reader only learns that the line is an assignment after reading a possibly long target).

**Functions, enums and imports:**
- Functions: `;fn name(a, b) { … }`.
- Enumerations: `;enum Name (a, b, c)`.
- Imports: `;import name from "path.wvt"`.
- Trade-off: definitions no longer mirror their call site (`(a, b)(name)`). In exchange, names sit at the left margin where they can be scanned, searched and outlined.

### 5. Exports and end markers
Today exports are a `;a, b bribe` list at the end of the library (§13.1).

- **An `export` modifier on each exported declaration:**
  ```
  ;import text from "text.wvt"
  ;export fn tokenize(src) { … }
  ;export const keywords is [ … ]
  ;fn isSpace(c) { … }            // private
  ;end
  ```
  - Whether a name is public is visible at its definition, and renaming or removing it takes one edit, with no separate list to keep in sync.
  - The first words of the line still say what it is: `;export fn tokenize`. The whole API can be found by searching for `^;export` or in an editor outline.
  - Only top-level `fn`, `const`, `var`, `ref` and `enum` declarations can be exported. Modules cannot be re-exported, as in Γ-- (§13.1).
  - Importers still cannot `set` an exported binding, even a `var` one.
  - A library that exports nothing is a compile error, as in Γ--.
  - Rejected alternative: an export list right after the imports (`;export tokenize, keywords`). It shows the whole API on one line, but every public name appears in two places. Libraries export few names (1–10 in `wahc`), so the list saves little.
- **Different end markers for programs and libraries.** A program ends with `;exit` (the same word as early exit; the final `;exit` means `;exit 0`). A library ends with `;end`. Using one word for both would suggest that a library can be run on its own.
  - The last statement decides the file's kind, as in Γ-- (§13).
  - Importing a file that ends with `;exit` is a compile error ("not a library").
  - `;exit` at a library's top level is a compile error. Inside a function it is allowed, as in Γ--.
  - Both markers are still required, so a truncated program or library fails to compile.

### 6. Standard names for every themed element
**Numerals.** Ordinary literals replace wah numerals (§2.3): `0`, `42`, `0xFF`, `0b101`, `1.5`, `1e9`.
- Negative numbers use unary minus. Since `-1` is no longer a single token, the rule against writing `-x ** y` (§11.2) now also applies to `-2 ** 2`; keep that error.
- Identifiers shaped like `wah` or `WAAH` are no longer reserved.
- Remove `$toWah`. `$toInt` and `$toFloat` stop accepting wah-numeral text.

**Keywords and constants.**

| Γ-- | Wavert | Meaning |
|---|---|---|
| `scheme` | `fn` | function definition |
| `getaway` | `return` | early return |
| `claim` / `claim shifty` / `claim fickle` | `const` / `var` / `ref` | declare a frozen, mutable or alias binding (change 4; §10.4 copy rules unchanged) |
| `cheat` | `set` | assign (change 4) |
| `!doubt` / `doubt` | `if` / `!if` | if / unless |
| `anyway` | `else` | else, including `else if` |
| `lurk` / `!lurk` | `while` / `!while` | while / until |
| `bail` / `loiter` | `break` / `continue` | loop control |
| `suspect` / `alibi` | `match` / `case` | multi-way branch; `else` is the default case |
| `lineup` | `enum` | enumeration |
| `steal` / `bribe` | `import` / `export` | modules |
| `real` / `fake` | `true` / `false` | booleans |
| `uninvited` | `null` | null |
| `nobody` / `everybody` | `nan` / `inf` | NaN / infinity |
| `!invited` | `exit` | last statement of a program / early exit with a code |
| (`bribe` list as last statement) | `end` | last statement of a library |
| `purple` | `string` | type name in messages and `$typeOf`; `$toPurple` becomes `$toString` |

**Errors.** Γ-- has two error mechanisms. Both stay; only their names change.

| Γ-- | Wavert | Meaning |
|---|---|---|
| `WAH!` / `WAAAH!` / `c(WAH!)` | `fail` / `fail 3` / `fail c` | recoverable failure, delivered to the immediate caller only (one-frame rule, §8.2). The code is an integer ≥ 1 and defaults to 1. |
| `whine (c) { … }` | `onfail (c) { … }` | handler after a statement; still handles that statement's outermost call |
| `wails` | `fails` | marks a function that can fail (planned) |
| `tantrum expr` | `panic expr` | fatal error that propagates to the top |
| `blame` / `sulk` / `regret` | `recover` / `else` / `finally` | planned panic handling (not designed yet, §16) |

```
;fn parse(s) fails { … ;fail 3 … }

;const n is s(parse)
    onfail (c) { 0 }            // the handler's block value becomes the call's value

;const m is s(parse)
    onfail (c) { ;fail c }      // forwarding is always explicit
```

- **Why not `throw`/`catch`:** those words suggest exceptions that propagate until someone catches them. A `fail` never travels past the immediate caller, which matches Zig's error returns or Swift's `throws`, not Java or JS exceptions. Naming nothing after exceptions keeps the docs from having to explain that the familiar words mean something else. `panic` keeps its familiar name because it does propagate (as in Go's `panic`/`recover`).
- `fail`, `onfail` and `fails` are reserved words. `onfail` must be, because it appears where a block value could also start.
- Messages: `unhandled fail (code 3) from 'parse' in 'ignore'`. The uncaught-panic banner ("Waluigi was not invited to this exception") becomes plain text, e.g. `uncaught panic: <message>`.
- Other names considered: `refuse`/`refused`/`refuses` (most distinctive, least standard), `fallback` as the handler (describes its role, but is sometimes used as a variable name), `throw`/`catch`, `rescue`, `orelse`, `otherwise`, `handle`, `signal`.

## Unchanged
- Operator semantics, numeric rules (53-bit ints, `/` always returns a float, floored `%`, int32 bitwise operators), strings, arrays, records, the copy model, and the one-frame signal semantics. None of these depend on syntax or naming.

## Relationship to Γ-- and bootstrapping
Wavert is a **new language inspired by Γ--**, not a replacement for it. Γ-- and its toolchain (`wahc`, `wahc0`) stay as they are, and Γ-- serves as the seed for bootstrapping the new language.

- **Name:** "Wavert" was generated by a character-level Markov chain trained on common English words. Real words, near-misspellings and anything containing an offensive word were filtered out, so the name feels like English but means nothing.
  - It is typed entirely with the left hand on QWERTY, in a smooth order. The one same-finger pair, `R → T`, is a sideways slide of the index finger along the top row.
  - In October 2026 it was unused on npm, crates.io and PyPI, and nothing significant used the name. The closest match is Wavertree, a district of Liverpool.
  - Rejected candidates: Verack (a Bitcoin protocol message), Mendar (a Galician verb), Tack (an existing 2026 language), and Graft, Stew and Baton (descriptive names that were dropped in favour of a generated one).
- **Files and compiler:** source files use `.wvt`, and the compiler is `wvc`, following Γ--'s `.wah` / `wahc`. Both are typed with the left hand.
  - `.wvt`'s only other known use is an obsolete weather-software export format. `.wv` was rejected because WavPack audio files use it.

- **Shared AST:** every change is syntax or naming only, so the new language's front end (lexer and parser) can produce the existing `wahc` AST and reuse its checker and codegens.
- **Converter:** written in Γ--, reusing `lexer.wah`/`parser.wah`, with a pretty-printer from the AST to the new syntax. Printing from the AST handles call precedence, declaration reordering (`;value claim name` → `;const name is value`), member order and argument grouping, which a text rewrite would get wrong.
  - Comments are recovered from the source text between tokens, so `wahc`'s lexer did not have to change. The printer emits each comment before the statement it precedes, or at the end of the line it ended. Comments in the middle of an expression move to the next line break and are listed for hand-fixing, never dropped silently.
- **Verification:** each converted file must produce the same AST, and therefore byte-identical generated C, as its Γ-- original. Any difference is a bug.
- **Bootstrap order:**
  1. Fork `wahc` as `wvc` and build the new front end in it, still written in Γ--.
  2. Convert the examples and tests, and verify them.
  3. Convert the compiler, build it with the Γ-- `wahc`, and check that the generated C is identical.
  4. Let it compile itself. Its generated C becomes the new language's seed, independent of Γ--.

### Bootstrap status

All four steps are done (October 2026). `wvc` is written in Wavert ([wvc/](../wvc/)), and its seed `wvc/wvc.c` reproduces itself.

1. `wahc` was forked, and its lexer and parser were rewritten for Wavert, still in Γ--. Names in `wahc` that Wavert reserves were renamed first: `fail` → `abort`, `fn` → `func` and `ref` → `resolve`.
2. [tools/wah2wvt.wah](../tools/wah2wvt.wah) converted the Γ-- examples and `src/` programs. [tools/verify.mjs](../tools/verify.mjs) confirmed the same syntax trees and byte-identical C and JavaScript.
3. The converted compiler, compiled by `wvc`, produced the same C as the Γ-- original compiled by `wahc` (8,705 lines).
4. That C became the seed. A native `wvc` built from it compiles `wvc/wvc.wvt` back to the same file.

The runtime's themed text was replaced after step 4, with two more self-compiles: first teaching `wvc` the new standard library names alongside the old ones, then switching to them. Programs now print `true`, `false`, `null`, `nan` and `inf`, `$typeOf` gives `"string"`, and errors read `uncaught panic: …` and `unhandled fail (code 3) from 'parse' in 'ignore'`.

Decisions made during the implementation, beyond this document:
- `from` (in `;import name from "…"`) and `end` (as the statement `;end`) are contextual, like `is` and `to`, because `wahc` uses both as names (`end` is an AST field).
- `$isNobody` is `$isNan`.
- A call binds tighter than unary minus, as in C-family languages: `-5(abs)` is `-(5(abs))`, and `(-5)(abs)` calls `abs` with -5. The converter writes the parentheses.
- A negative number is a literal: the parser folds `-` applied to a number literal, so `case -1` works and the syntax tree matches Γ--'s `WAH`. `-(5)` stays a negation.
- Runtime messages call non-`var` arrays and records *frozen*: `cannot modify a frozen array`.
- `wvc`'s default target is C. The JavaScript backend (`--target js`) was kept for a while, then removed so that new features are written once.
- The syntax tree kept `wahc`'s node kinds (`scheme`, `claim`, `lineup`, `suspect`, …) through the bootstrap. They were renamed afterwards (`fn`, `decl`, `enum`, `match`, …), as were the runtime's internal names (`rt_tantrum` → `rt_panic`), with the generated code checked to be unchanged. `tools/verify.mjs` maps `wahc`'s names when it compares.
