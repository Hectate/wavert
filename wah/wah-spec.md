# Γ-- Language Specification (Draft 0.8)

> Draft 0.8 adds `fickle` bindings (§5, §10.4): they can be rebound with `cheat`, and they keep each value as it is, frozen or shifty, without copying it. It also adds the C backend (§15).
> Draft 0.7 gives each stolen module a name. What a module bribes is written `name.module`, so libraries never clash with each other or with the stealing file (§13.1).
> Draft 0.6 prepares for a native backend. It writes down behavior that was previously "whatever Node does" (§9.3, §9.5, §11.3, §12), adds `$programPath` and `$run` (§12.4), and designs modules (§13.1). It also adds explicit-value checks for blocks (§7.2, §2.8), lineups (enumerations, §4.1) and the `suspect` / `alibi` multi-way branch (§7.5). The stage 0 compiler implements all of these (§12.6).
> Draft 0.5 adds records (§10.7–§10.10).
> Draft 0.4 added the decisions made while building the stage 0 compiler `wahc0` (`js/`).

## 1. Overview

| Item | Value |
|---|---|
| Display name | **Γ--** (Waluigi's emblem: Luigi's L inverted; `--` inverts C++'s `++`) |
| ASCII name | `gamma--` / `wah` |
| File extension | `.wah` |
| Compiler | `wahc` |

**Design rule:** *syntax* is inverted, *operators* keep standard semantics. Bizarre but usable.

---

## 2. Lexical Structure

### 2.1 Statements
- Every statement **begins** with `;`. There is no terminator.
  ```
  ;wah claim x
  ;x($print)
  ```
- Newlines are not significant; a statement may span lines.
- Blocks: `{ ;stmt ;stmt }`.
- `;` followed by nothing is an **empty statement** (valid). `;;` is valid but triggers a warning (see §2.8).

### 2.2 Comments
- Line: `// ...`
- Block: `/* ... */`

### 2.3 Numeric literals (wah numerals)
- **Integers:** value = number of `a`s.
  - `wah` = 1, `waah` = 2, `waaaah` = 4
  - `wh` = 0
- All-uppercase negates: `WAH` = -1, `WAAH` = -2.
  - This is part of the literal token, not unary minus. `WAH ** waah` is unambiguous and equals 1.
- Mixed case (`Waah`, `wAH`) is a **lex error**.
- **Bracket form:** `wa[n]h`, `WA[n]H` (negative).
  - `n` must be a **literal**: decimal, hex (`0xFF`), binary (`0b1010`), or float.
  - Expressions inside brackets are not allowed. Use `;x($toInt)` instead.
- **Floats:** bracket form containing a `.` or an exponent.
  - `wa[0.5]h`, `WA[0.5]H` (-0.5), `wa[1e9]h`, `wa[1.0]h`
  - Digits are required on both sides of `.`: `wa[.5]h` and `wa[5.]h` are lex errors.
  - No conflict with hex: `0x1e9` has the prefix.
- **Reserved identifiers:** anything matching `^[wW][aA]*[hH]$`, plus the prefix `wa[` / `WA[`.

### 2.4 Bang tokens
- There are exactly three bang tokens: `!doubt`, `!lurk` and `!invited`. Each is a single token.
- `!` before any other word is the NOT operator: `!real`, `!x`, `!doubtful`.
- `invited` alone is reserved and is not a valid identifier.
- Whitespace between them (`! doubt`) is an error.
- `!` is otherwise the boolean NOT operator.

### 2.5 Signal token (`WAH!`)
- Form: uppercase `W`, one or more `A`, `H`, `!`. Bracket form also valid: `WA[0x10]H!`.
- Recognized **only** when `!` is followed by whitespace, `;`, `}`, `)`, or EOF.
  - Otherwise the numeral ends at `H` and `!` starts the next token:
    - `;WAH!=x` lexes as `WAH`, `!=`, `x`
    - `;WAH!invited` lexes as `WAH`, `!invited`
- Lowercase `wah!` is a lex error.
- Valid only as a full statement: `;WAH!` or `;c(WAH!)` (§8.2).

### 2.6 Strings
- Delimiter: `"..."` only. No char type.
- Escapes: `\n \r \t \\ \" \$ \u{1F608}`. Unknown escape is a lex error.
  - `\$` produces a literal `$` and suppresses the `$` lint warnings (§2.8).
- A raw newline inside a string is a lex error.
- Interpolation: `"score: {n}"`. Any expression may appear between the braces: `"{n / waah}"`, `"{xs($length)}"`.
- Literal braces are written `{{` and `}}`. A lone `}` is a lex error, so `"}"` must be written `"}}"`.
  - `{{` is always a literal brace. An interpolation that starts with a record literal (§10.7) needs a space: `"{ {a: wah, b: waah}(sum) }"`.

### 2.7 Forbidden tokens
- Bare `=` is a lex error with hint: `'=' is not an operator; use 'claim' or 'cheat'`.
- `--` is only valid in `--cheat`. Double negation must be written `- -x` or `-(-x)`.
- `++` is only valid in `++cheat`.
- `>>>` is an error; use `;x($toUint32)`.
- Decimal digits outside `wa[...]h` are an error (`wa[42]h`, not `42`).
- `.` is valid only as member access (§10.8), touching both sides.

### 2.8 Lint warnings
- Missing whitespace around binary operators (`a!=b`).
- `a//b` warns that `//` starts a comment and that integer division is `;a / b($floor)`.
- `doubt (a != b)` suggests `!doubt (a == b)`.
- Zero-argument calls are formatted with no space: `,(f)`, so the leading comma stays visible.
- `whine` is formatted on its own line, indented one level below the call it handles.
  - This rule and the zero-argument spacing rule are for a future formatter. `wahc0` does not lint them.
- `;;` warns: likely a habitual trailing `;` from C-style languages.
- `${` inside a string literal: `Γ-- interpolation is {x}, not ${x}` (JS habit).
- `$identifier` inside a string literal: `did you mean {identifier}?` (Kotlin/Dart/Bash habit).
- A string-literal key in computed field access: `"kind"[node]` suggests `kind.node` (§10.8).
- An expression statement that calls nothing has no effect: `;n`, `;a + b`, `;kind.node`. It warns `this statement has no effect`, with the hint that a block's value is written without a leading `;` (§7.2).
  - A statement that contains any call, or that is a conditional chain or `suspect`, is not affected.
- Inside a block, a `[` at the start of a line that indexes the expression before it (§7.2).

### 2.9 Stdlib sigil (`$`)
- `$` immediately followed by an identifier is a **stdlib name** token: `$print`, `$length`.
- `$` is not a valid character in user identifiers, so user names can never collide with the stdlib.
- An unknown `$name` is a compile error with hint: `'$' is reserved for the standard library; user names have no sigil`.
- `$` anywhere else (bare, trailing `x$`, `$"..."`) is a lex error.

---

## 3. Keywords

| Concept | Keyword |
|---|---|
| function definition | `scheme` |
| return (early) | `getaway` |
| declare (const) | `claim` |
| mutable modifier | `shifty` |
| rebindable modifier | `fickle` (§5) |
| mutate | `cheat` |
| unless | `doubt` |
| if | `!doubt` |
| else | `anyway` |
| switch / case | `suspect` / `alibi` (§7.5) |
| enumeration | `lineup` (§4.1) |
| while | `lurk` |
| until | `!lurk` |
| break | `bail` |
| continue | `loiter` |
| throw | `tantrum` (`;tantrum expr`) |
| catch | `blame` |
| try-else (nothing thrown) | `sulk` |
| finally | `regret` |
| one-frame signal | `WAH!` (see §8) |
| signal handler | `whine` |
| signal-capable marker (post-stage 0) | `wails` |
| import / export | `steal` / `bribe` |
| true / false | `real` / `fake` |
| null | `uninvited` |
| NaN / infinity | `nobody` / `everybody` |
| program end / exit | `!invited` |

Keywords and literals take no `$`. Only stdlib functions and constants do (§12).

These words are reserved but not yet usable: `blame`, `sulk`, `regret`, `wails`, `invited`.

---

## 4. Types

| Type | Notes |
|---|---|
| integer | 53-bit safe range (±(2^53−1)). A result outside it raises `tantrum`. Bitwise ops require int32 range, else error. |
| float | IEEE 754 double. |
| boolean | `real` / `fake`. Distinct from integers: `real == wah` is `fake`. |
| `purple` | String. Immutable sequence of Unicode code points. |
| array | Heterogeneous, reference semantics (§10). |
| record | Fixed set of named fields, reference semantics (§10.7). |
| `uninvited` | Null. Equals only itself. `wh == uninvited` is `fake`. |
| lineup member | A named constant of an enumeration (§4.1). Equals only itself. |

Implicit conversions (the complete list):
- **Int-to-float promotion** in arithmetic and comparison (§11.2).
- **String interpolation** (§9.3).

### 4.1 Lineups (enumerations)
```
;lineup (num, str, call, chain)Kind

;call.Kind claim k
;suspect (k) {
    alibi num.Kind, str.Kind { ;"literal"($print) }
    alibi call.Kind { ;"call"($print) }
    alibi chain.Kind { ;"chain"($print) }
}
```
**Declaring**
- `;lineup (member, member, ...)Name` declares a lineup named `Name` with a fixed, ordered set of members. Its shape mirrors `;scheme (params)name`.
- A lineup is declared only at the top level of a file. It is visible throughout the file, like a scheme, so the no-shadowing rule (§5.1) treats its name like a scheme name.
- It has at least one member. Members are identifiers; keywords, reserved words and wah-numeral shapes are not allowed (as for record keys, §10.7). A duplicate member is a compile error.
- Member names are labels, not bindings, like record field names (§10.8). A binding `num` and a member `num.Kind` can coexist.

**Using**
- `member.Name` is the member's value, using the member-access syntax (§10.8). It is resolved at compile time, and an unknown member is a compile error: `no member 'cal' in lineup 'Kind'`.
- A lineup name is not a value. It may appear only on the right of `member.Name`, so `;Kind($print)`, `;k[Kind]` and `;x cheat call.Kind` are compile errors.
- A member is equal (`==`) only to itself. It never equals a member of another lineup, a purple, or a number.
- Ordering (`< > <= >=`) or arithmetic on members raises `tantrum`.
- Interpolation and `$toPurple` give the member's name (`call`). `$typeOf` gives the lineup's name (`"Kind"`). `$same` on members is `==`.
- Members can be stored in arrays and records, passed to schemes and returned like any value. They are immutable, so the copy rules (§10.4) do not apply to them.
- Libraries export a lineup by bribing its name (§13.1).
- How members are represented is up to each backend (stage 0: frozen objects; the C backend: pointers to static structs).

---

## 5. Bindings

```
;wah claim x              // const
;wah claim shifty y       // mutable
;node claim fickle z      // rebindable; values are kept as they are
;waah cheat y             // mutation; only valid on shifty and fickle bindings
;wah +cheat y             // compound: y += 1
;++cheat y                // increment
;--cheat y                // decrement
```
- `cheat` on a binding that is neither `shifty` nor `fickle` is a compile error.
- `shifty` and `fickle` both allow rebinding with `cheat`. They differ in what happens to an array or record (§10.4):
  - A `shifty` binding always holds a shifty value. Binding it to a frozen value copies the value and makes the copy mutable.
  - A `fickle` binding holds each value as it is, and never copies it. A frozen value stays frozen, and a shifty value is shared.
  - Use `fickle` for a name that is rebound to successive values, such as the result built up by a loop, and `shifty` for a value whose contents change.
- A binding has at most one modifier: `claim shifty fickle x` is an error.
- Compound forms: `+cheat -cheat *cheat /cheat %cheat **cheat &cheat |cheat ^cheat <<cheat >>cheat`.
- `++cheat` / `--cheat` are **statements only** and produce no value. There are no prefix/postfix expression forms.
- Each compound form is a single token. No whitespace is allowed inside it: `+cheat` is valid, `+ cheat` is not.
- The value is evaluated before the target is read.
- The target of any `cheat` is a name, an indexed element or a record field (`;x cheat i[arr]`, `;wah +cheat i[arr]`, `;x cheat kind.node`, §10.9).
- Rebinding an array or record with `cheat` follows the target's column of the copy table (§10.4): `claim shifty` or `claim fickle`.

### 5.1 Scoping
- Bindings are **block-scoped**: a `claim` is visible from its block to the block's end, including nested blocks.
- **No shadowing:** a binding (including a function parameter) may not reuse a name visible from an enclosing scope. Compile error.
  - Sibling blocks may reuse a name, since neither encloses the other.
  - Visibility is positional. An outer `claim` conflicts only if it appears before the inner declaration.
    - Example (§14): a scheme parameter `n` is fine even if a top-level `claim n` comes later in the file.
  - Outer `scheme` names, parameters and `whine` codes are visible throughout their scope, so they always conflict.
  - Redeclaring a name in the same scope is an error.
  - The stdlib cannot be shadowed, because its names carry the `$` sigil (§2.9).
- **Use before declaration (hybrid rule):**
  - Plain `claim` bindings: referencing a name before its `claim` in the same scope is a **compile error**.
  - `scheme` declarations are **hoisted** to the top of their block, so mutual recursion works regardless of order.
  - A hoisted `scheme` that reads an outer binding before that binding's `claim` has executed raises `tantrum` at runtime (temporal dead zone).
  - Nested schemes may read and `cheat` bindings of enclosing scopes.
  - A claimed name becomes visible at the end of its statement, including any `whine` handler. So `;s(parse) claim n whine (c) { n }` is a compile error.
  - Stage 0: compile `scheme` to JS function declarations (hoisted) and bindings to `let`/`const` (runtime TDZ), plus the compile-time check for direct use before `claim`.

---

## 6. Functions

### 6.1 Definition
```
;scheme (a, b)add {
    a + b
}
```
- Return value follows the block-value rule (§7.2): the trailing bare expression, else `uninvited`.
- No parameters: `;scheme ()main { ... }`.
- `getaway` returns early: `;getaway x`. A bare `;getaway` returns `uninvited`.
- Parameters are non-shifty by default.
- `shifty` parameters are mutable local bindings that follow the copy table (§10.4): `;scheme (shifty toks)eat { ... }`.
  - A shifty array argument is aliased.
  - A frozen array argument is copied.
  - Scalars are never passed by reference: `cheat` on a parameter does not affect the caller.
- `fickle` parameters take the argument as it is, never copying it, and can be rebound with `cheat`: `;scheme (fickle node)walk { ... }`. They are the cheapest way to pass a shifty array that the scheme only reads.
- Functions are not values (stage 0). A scheme name may only appear in call position; a call's arity is checked at compile time.

### 6.2 Calls (postfix)
- Arguments precede the function: `;x, y, z(floorInt)`.
- `(f)` takes **every comma-separated expression to its left at the current nesting level**.
  - `;a, (b, c(g))(f)` is `f(a, g(b, c))`
- Calls bind **loosest** of all operators.
  - `;1 + x(f)` is `f(1 + x)`
  - `;c ? x : y($print)` is `$print(c ? x : y)`
- Zero arguments: a leading `,` (an empty argument list), then the call: `;,(getTime)`.
  - A leading `,` is otherwise invalid at expression start, so this is unambiguous. Whitespace between `,` and `(f)` is irrelevant.
  - Chains normally: `;,(getTime)(format)` is `format(getTime())`.
- **Nesting rule:** a call used as an argument inside another argument list must be parenthesized when the scope rule would otherwise make it capture its neighbours. This always applies to zero-argument calls:
  - `;a, (,(getTime))(f)` is `f(a, getTime())`
  - `;a, ,(getTime)(f)` is a **compile error** (without parentheses, `getTime` would capture `a`).
- Chaining reads as a pipeline: `;x(parse)(validate)(save)` is `save(validate(parse(x)))`.
- A call's result continues the expression as a left operand: `;x(f) + wah(g)` is `g(f(x) + 1)`.
- **Call form:** a call is exactly `(name)` or `(name.module)` after an expression, where `name` is a scheme, a `$name` or `WAH!`.
  - `(name.module)` calls a scheme that a stolen module bribes (§13.1): `;"x y"(tokenize.lexer)`.
  - `(…)` at the start of an expression is grouping.
  - Any other `(` after an expression ends that expression. So a block value such as `(a + b) / c` may follow a statement: `{ ;x($print) (a + b) / c }`. A block value of the form `(a.b)` after a statement is read as a call; write it without parentheses.
  - The C-style `;print("hi")` is an error with a hint that calls are postfix.
- Because calls bind loosest, a call used as an operand usually needs parentheses: `(c(isSpace)) || c == ";"`, `i < (xs($length))`.
- Array literals follow the same scope rule: `[a, b(f)]` is `[f(a, b)]`; write `[a, (b(f))]`.

---

## 7. Control Flow

### 7.1 Conditionals
- `doubt (cond)` runs when `cond` is `fake` (unless).
- `!doubt (cond)` runs when `cond` is `real` (if).
- Conditions must be `real`/`fake`. Any other type is an error.
- `anyway` is else. It may be followed by a block or by another `doubt`/`!doubt`:
  ```
  ;!doubt (n < wh) {
      ;"negative"($print)
  } anyway !doubt (n == wh) {        // else if
      ;"zero"($print)
  } anyway doubt (n > wa[100]h) {    // else if not
      ;"small positive"($print)
  } anyway {
      ;"large"($print)
  }
  ```
- The whole chain is one statement (one leading `;`). Branches are always braced, so there is no dangling-else.

### 7.2 Block values (unified rule)
Applies to every block: `scheme` bodies, conditional and `suspect` branches, `whine` handlers.
- A block's value is its **trailing expression without a leading `;`**.
- If the block has none (empty, or its last statement starts with `;`), its value is `uninvited`.
  ```
  { a + b }           // value: a + b
  { ;a + b }          // value: uninvited (expression statement, result discarded; warns, §2.8)
  { ;x($print) ; }    // value: uninvited (ends with an empty statement)
  { }                 // value: uninvited
  ```
- Only blocks in expression positions use their value. Blocks are not standalone expressions: `{ ; } == uninvited` is not valid syntax, though it describes the rule.
- A statement ends where its expression cannot continue (maximal munch). Anything left before the `}` is the block's value, and it must come last.
- A conditional chain **as a statement** (its result is not used) needs no `anyway` and no branch values, and `bail`, `loiter` and `getaway` work as usual.
- A conditional chain **as a value** (its result is claimed, passed to a call, used in an expression, or is a block's value):
  - It must end in a plain `anyway { ... }`; otherwise compile error.
    ```
    ;!doubt (n < wh) { "neg" } anyway { "non-neg" } claim label
    ```
  - Every branch must produce a value (explicit values, below).
  - In stage 0, `getaway`, and a `bail` or `loiter` that would jump out of the conditional, are compile errors. A `bail` or `loiter` for a loop inside a branch is fine.

**Explicit values.** Because a stray `;` turns a block's value into `uninvited` without any other sign, a value that is used must be written out. If `uninvited` is intended, write it at the end: `{ ;x($print) uninvited }`.
- **Schemes.** If any call uses a scheme's result, the scheme must be able to produce a value: its body has a trailing value, or contains a `;getaway` with a value (outside nested schemes). Otherwise it is a compile error: `'f' never produces a value, but its value is used (line N)`.
  - A call's result is used unless the call is the whole of an expression statement (`;x(log)`).
  - A bare `;getaway` does not count as producing a value.
- **Other blocks used as values.** A block whose value is used must end with a trailing value, unless its last statement never completes. The never-completing statements are `getaway`, `tantrum`, `!invited`, `WAH!`, `c(WAH!)`, `bail` and `loiter`. Otherwise it is a compile error.
  - These blocks are the branches of a conditional chain or `suspect` (§7.5) used as a value, and the `whine` handler of a `claim`, `cheat`, `getaway`, `tantrum` or `!invited` statement.
  - The handler of an expression statement (`;x(f) whine { ... }`) is not used as a value.
  ```
  ;!doubt (ok) { "yes" } anyway { ;"no"($print) } claim r           // error: the anyway block has no value
  ;!doubt (ok) { "yes" } anyway { ;"no"($print) uninvited } claim r // ok
  ;path($readFile) claim text
      whine { ;"cannot read {path}"($eprint) ;waah !invited }       // ok: the handler never completes
  ```
- **Diagnostics for a value swallowed by the previous statement.** A statement ends only where its expression cannot continue (maximal munch), so a block's value that begins with `,` or `[` continues the statement before it. The fix is to parenthesize the value: `(,(f))` or `([a, b])`.
  - `,`: this always causes the error `comma-separated values must be consumed by a call`. Inside a block, when the `,` starts a line, the error's hint gives the fix.
  - `[`: this indexes the expression before it, which may compile or fail in several ways. So inside a block, a `[` that starts a line and indexes the expression before it warns: `'[' at the start of a line indexes the expression before it`, with the fix as its hint.

### 7.3 Ternary
- `cond ? a : b`, standard semantics. `cond` must be boolean.

### 7.4 Loops
- `lurk (cond) { ... }`: while.
- `!lurk (cond) { ... }`: until (runs while `cond` is `fake`).
- `bail`: break. `loiter`: continue.

### 7.5 Multi-way branch (`suspect` / `alibi`)
```
;suspect (kind.e) {
    alibi "int", "float" {
        ;"number"($print)
    }
    alibi "str" {
        ;"text"($print)
    }
    anyway {
        ;"other"($print)
    }
}
;suspect (op) { alibi "+" { "add" } alibi "-" { "sub" } anyway { "?" } } claim name
```
- `suspect (subject) { ... }` evaluates the subject once, then runs the first `alibi` with a value equal to it (`==`, §11.1). If none matches, it runs the `anyway` block, if there is one.
- There is **no fallthrough**. Each `alibi` has its own block, and one `alibi` may list several values separated by commas.
- `bail` and `loiter` inside a `suspect` refer to the enclosing loop. There is no "break out of the suspect".
- **Values are constants**, all of one of these two sorts. Mixing the sorts, or members of two lineups, in one `suspect` is a compile error.
  - **Literals**: integer numerals (including negative ones such as `WAH`), string literals without interpolation, `real`, `fake` and `uninvited`.
    - Floats, `nobody` and `everybody` are not allowed, because float equality makes poor match keys. A float subject can still match an integer value, since `==` promotes (`wa[1.0]h` matches `alibi wah`).
  - **Members of one lineup** (§4.1), written `member.Name`.
  - The same value twice in one `suspect` is a compile error, even across different `alibi`s.
- Layout: at least one `alibi`, then an optional `anyway`, which must come last.
- **Exhaustive lineup suspects.** A lineup `suspect` without `anyway` must list every member of the lineup; otherwise it is a compile error that names what is missing: `suspect does not handle 'chain.Kind'`.
  - If such a `suspect` is given a subject that matches nothing (one that is not a member of the lineup, such as a purple), it raises `tantrum`: `suspect: no alibi for "call"`.
  - With `anyway`, any members may be left out, and every unmatched subject goes to `anyway`.
  - A literal `suspect` without `anyway` simply does nothing when nothing matches.
- The whole `suspect` is one statement (one leading `;`), like a conditional chain (§7.1).
- **As a statement** (its result is not used), the branches need no value, and `bail`, `loiter` and `getaway` work as usual:
  ```
  ;lurk (real) {
      ;suspect (cmd) {
          alibi "quit" { ;bail }               // leaves the lurk
          alibi "skip" { ;loiter }
          anyway { ;cmd($print) }
      }
  }
  ```
- **As a value** (its result is claimed, passed to a call, used in an expression, or is a block's value), it follows the conditional chain rules (§7.2):
  - It must end with `anyway { ... }`, unless it is an exhaustive lineup `suspect`.
  - Every branch must produce a value (explicit values, §7.2).
  - In stage 0, `getaway`, and a `bail` or `loiter` that would jump out of the `suspect`, are compile errors. A `bail` or `loiter` for a loop inside a branch is fine.
- Formatting: each `alibi` and the `anyway` go on their own lines, one level in from `suspect`.

---

## 8. Errors and Signals

### 8.1 `tantrum` (exceptions)
- Syntax: `;tantrum expr`. Any value may be thrown.
- Runtime errors raise tantrums too: type errors, out-of-range indexes, overflow, and mutating a frozen array.
- Propagates any depth until caught by `blame`. Stage 0 cannot catch tantrums.
- `sulk`: runs if nothing was thrown.
- `regret`: finally. Runs on normal exit and on `tantrum`.
- Uncaught: prints `Waluigi was not invited to this exception` before the stack trace. The process exits with code 1.
  ```
  Waluigi was not invited to this exception
  tantrum: it broke
      in boom
  ```

### 8.2 `WAH!` (one-frame signals)
- For **expected, recoverable** failures.
- Aborts the current function and delivers a signal to the **immediate caller only**. It never propagates further on its own.
- Signal code = number of `A`s, always positive: `WAAAH!` carries code 3.
- Handled at the call site with `whine`. The handler's block value (§7.2) becomes the call's value:
  ```
  ;"abc"($toInt) claim n
      whine (c) { wh }
  ```
- Formatting convention: `whine` goes on its own line, indented one level, to separate the failure path from the intended result. This is valid because statements are delimited by a leading `;`, not by newlines.
- `whine` follows the whole statement and handles that statement's **outermost** call, i.e. the call that produces its value.
  - In `;s(parse)(validate) claim n whine (c) { ... }` it handles `validate` only. To handle `parse`, split the statement.
  - If the statement's value is not a call, `whine` is a compile error.
  - It may follow an expression statement, `claim`, `cheat`, `getaway`, `tantrum` or `!invited`.
- The code binding is optional: `whine { wh }`.
- **Forwarding:**
  - `;WAH!` sends a **new** signal with code 1.
  - `;c(WAH!)` sends a signal with code `c`, so forwarding the original code must be explicit:
    ```
    ;s(parse) claim n
        whine (c) { ;c(WAH!) }
    ```
  - In call position, only the exact spelling `WAH!` is allowed. `;c(WAAAH!)` is a compile error (the code would be ambiguous).
  - `c` must be an integer ≥ 1 at runtime, otherwise `tantrum`.
- `WAH!` and `c(WAH!)` are only valid inside a scheme (top level has no caller).
- Stage 0: an unhandled signal is a runtime error in the caller: it becomes a `tantrum` such as `unhandled WAH! (code 3) from 'parse' in 'ignore'`.
- Later: functions that can signal are marked `wails`; calling one without `whine` is a compile error.

---

## 9. Strings (`purple`)

### 9.1 Semantics
- Sequence of Unicode code points. `$length`, `$at`, `$slice` count code points.
- Storage encoding is a backend detail (UTF-16 in stage 0; UTF-8 in the C backend).
- Immutable. Rebinding via `cheat` as usual.

### 9.2 Operators
- `+` concatenates `purple + purple` only. `purple + int` is a type error.
- `== !=` compare by **value**.
- `< > <= >=` compare lexicographically by code point.

### 9.3 Interpolation conversions
| Value | Text |
|---|---|
| integer | decimal (`-1`) |
| float | decimal, always with a fractional part (`0.5`, `1.0`, `1.0e+21`); `nobody`, `everybody`, `-everybody` |
| boolean | `real` / `fake` |
| `uninvited` | `uninvited` |
| lineup member | its name (`call`), §4.1 |

Anything else must be converted explicitly.

**Float text, exactly.** Every backend must produce the same text, so the rule is:
1. Take the **shortest** decimal digit string `d1…dk` (no trailing zeros) that converts back to exactly the same double. If several are equally short, take the one closest to the exact value.
2. Let `n` be the decimal exponent, so the value is `0.d1…dk × 10^n`.
3. If `1e-6 <= |v| < 1e21` (that is, `-6 < n <= 21`), write plain decimal notation: `1500`, `0.5`, `0.000001`, `123.456`.
4. Otherwise write exponent notation: `d1`, then `.d2…dk` if `k > 1`, then `e`, then the sign (`+` or `-`), then `n - 1` in decimal: `1e+21`, `1.5e-7`.
5. If the result has no `.` before the exponent (or the end), insert `.0` there: `1500.0`, `1.0e+21`.
6. A negative value gets a leading `-`. Negative zero is `-0.0`.

This is JavaScript's `Number.prototype.toString` (ECMA-262 Number::toString) plus step 5. `$toPurple` and `$join` use the same text.

### 9.4 String stdlib
```
;s($length) claim n
;s, wh($at) claim first      // negative indices count from the end
;s, wah, waaah($slice) claim mid   // end-exclusive: code points 1 and 2
;first($code) claim c        // single code point -> int
;c($fromCode)                // int -> one-character purple
;s, ","($split) claim parts  // "" splits into code points
;parts, ", "($join)          // elements converted as by interpolation
;n($toPurple)                // decimal text
;n($toWah)                   // wah-numeral text, e.g. "WAAH"; bracket form beyond ±10 ("wa[42]h")
;"42"($toInt)                // wails; also parses "-42", "WAAH", "wa[0xFF]h"
                             // float text ("1.5", "1.0") signals WAH!; use $toFloat
```

### 9.5 Numeric text (`$toInt`, `$toFloat`)
The accepted text is exact: no surrounding whitespace, no leading `+`, and only ASCII digits.

`$toInt` accepts:
- Decimal: an optional `-`, then one or more digits (`"42"`, `"-7"`, `"007"`). `"-0"` is `0`.
- A wah numeral (§2.3): `w a* h` or `W A* H`, or the bracket form with an integer literal inside (`wa[0xFF]h`, `WA[0b101]H`, `wa[42]h`).
- Anything else signals `WAH!`, including float text and values outside the 53-bit safe range. An int argument is returned unchanged.

`$toFloat` accepts:
- Decimal: an optional `-`, digits, an optional fraction (`.` and one or more digits), and an optional exponent (`e` or `E`, an optional sign, one or more digits): `"1.5"`, `"-2"`, `"6.02e23"`.
- Any wah numeral, integer or float (`wa[1.5]h`, `WAAH`).
- The result is the double nearest to the exact decimal value, with ties going to the even significand (correct rounding). A value too small to represent becomes `0.0` or `-0.0`.
- If the correctly rounded result would be infinite, `$toFloat` signals `WAH!`, as an out-of-range `wa[...]h` literal is a lex error (§2.3). Anything else that does not match also signals `WAH!`. An int argument is converted, and a float argument is returned unchanged.

---

## 10. Arrays and Records

### 10.1 Basics
- Literal: `[a, b, c]`. Empty: `[]`.
- Zero-indexed (`wh`).
- Heterogeneous (until static types exist).

### 10.2 Indexing (inverted)
- `i[arr]` (legal in C too, since `a[i]` is `*(a + i)`).
- Multi-dimensional reads inside-out: `j[i[grid]]`.
- Negative indices count from the end: `WAH[arr]` is the last element.
- Valid range: `-len` to `len - 1`. Outside raises `tantrum`.
- On a record, indexing is computed field access with a purple key: `k[node]` (§10.8). The rules above apply to arrays only.

### 10.3 Mutability
- Only `shifty` arrays can be mutated. `shifty` and `fickle` bindings can be rebound (§5).
- Element assignment: `;x cheat i[arr]`. The array must be shifty. Through a `fickle` binding this is checked at runtime, because the binding may hold either kind.
- Mutating stdlib calls (`;arr, x($push)`) require a shifty array. Stage 0 enforces via `Object.freeze` on non-shifty arrays.

### 10.4 Copy vs. reference
Copy happens **only when mutability changes**:

| Source | `claim b` | `claim shifty b` | `claim fickle b` |
|---|---|---|---|
| frozen | share reference | copy, made mutable | share reference |
| shifty | shallow copy, frozen (snapshot) | share reference (alias) | share reference (alias) |

- Copies are **shallow**. Nested shifty arrays remain mutable through other aliases.
- `cheat` on a binding follows its column, so rebinding a `fickle` binding never copies, and the binding keeps the value's identity (`==`, §10.5).
- Function parameters follow the same table: non-shifty parameters snapshot shifty arguments (O(n)); `shifty` and `fickle` parameters alias.
  - Passing a shifty array to a non-shifty parameter copies it on **every** call. Hot paths should pass frozen arrays (such as those returned by the stdlib) or use `shifty` or `fickle` parameters.
- Array literals and arrays returned by the stdlib are frozen.

### 10.5 Equality
- `==` is reference equality.
- `;a, b($same)` is deep structural equality; elements compare with `==`. It also recurses into records (§10.10).

### 10.6 Array stdlib
```
;arr($length)                // also works on purple
;arr, i($at)                 // same as i[arr]
;arr, wah, WAH($slice)       // end-exclusive, negative indices from the end; frozen result
;arr, x($push)               // shifty only
;arr($pop)                   // shifty only; tantrum if empty
;a, b($same)
;parts, ", "($join)
```

### 10.7 Records
- A record is a fixed set of named fields. Its literal sets its shape, and the shape never changes: fields cannot be added or removed.
- Literal: `{ kind: "num", value: n }`. Empty: `{}`.
- Keys are identifiers. Keywords, reserved words (`invited`, wah-numeral shapes, §2.3) and `$names` are not valid keys.
  - A duplicate key is a compile error.
  - There is no shorthand: `{ kind }` is an error; write `{ kind: kind }`.
- `{` at the start of an expression always begins a record literal, because blocks are never expressions (§7.2).
  - Where a block is expected, `{` opens the block. A record as a block's value needs its own braces:
    ```
    ;scheme (n)leaf { { kind: "num", value: n } }
    ;!doubt (ok) { { kind: "some" } } anyway { { kind: "none" } } claim r
    ```
- Each field value is a full expression. A ternary in a value is unambiguous, because a key's `:` always follows an identifier directly after `{` or `,`: `{ k: c ? a : b }`.
- **Call scope:** each field value is its own nesting level for the call rule (§6.2), since `,` separates fields.
  - `{ a: x, b: y(f) }` is `{ a: x, b: f(y) }`. Contrast arrays, where `[x, y(f)]` is `[f(x, y)]`.
  - Several arguments need grouping: `{ b: (x, y(f)) }` is `{ b: f(x, y) }`.
- Field order is the literal order. It is observable only through `$fields` (§10.10).
- Record literals are frozen (§10.9).

### 10.8 Member access (inverted)
- `field.rec` reads the field `field` of `rec`. Like `i[arr]`, the container is on the right.
- The left side of `.` is a field **label**, not an expression.
  - Labels are not bindings, so the no-shadowing rule (§5.1) does not apply: a binding `kind` and a field `kind` can coexist, and `kind.node` never reads the binding.
- **Computed access** reuses indexing: `k[node]` reads the field whose name is the value of `k`.
  - `k` must be a purple, otherwise `tantrum`. Negative indexing does not apply to records.
  - A missing field raises `tantrum`. This includes names that can never be fields, such as `""`, `"a b"` or `"claim"`.
  - Computed keys reach only fields that already exist, for reads and writes alike. They never add fields; dynamic key sets need a map (§16).
  - Beware the one-character difference: `kind.node` reads the field named `kind`, while `kind[node]` reads the field named by the binding `kind`.
  - A string-literal key lints (§2.8): `"kind"[node]` suggests `kind.node`.
  - `$fields` (§10.10) and computed access together allow generic code over records: `;f[node]($print)` for each `f` in `node($fields)`.
- The right side is a level-1 operand (§11.4): a name, a literal, grouping, indexing or another member access.
- When the right side is a lineup (§4.1) or a module name (§13.1), `.` is resolved at compile time instead: `call.Kind` is a lineup member, `keywords.lexer` is what the module `lexer` bribes as `keywords`, and `circle.Shape.shapes` is a member of the lineup `Shape` from the module `shapes`.
- `.` and indexing are both right-associative with the container on the right, so chains read inside-out:
  ```
  ;kind.node($print)              // node.kind
  ;name.callee.node($print)       // node.callee.name
  ;wh[args.node]($print)          // node.args[0]
  ;kind.i[items]($print)          // items[i].kind
  ;(kind.i)[items]($print)        // items[i.kind]
  ;kind.(x(parse))($print)        // parse(x).kind
  ;kind.x(parse)                  // parse(x.kind): calls bind loosest
  ```
- `.` must touch both sides. Whitespace around it (`kind . node`) is a lex error.
- Precedence is level 1, with indexing: `-value.node` is `-(node.value)`.
- Reading from a non-record raises `tantrum`. Reading a missing field raises `tantrum`: `no field 'knid' in record`.

### 10.9 Record mutability
- Records use the same frozen/shifty model as arrays. The copy table (§10.4) applies with "record" in place of "array": copies are shallow, and parameters follow the same rules.
- `;x cheat kind.node` assigns an existing field. `node` must be shifty. Through a `fickle` binding this is checked at runtime (§10.3).
  - Compound forms work too: `;wah +cheat count.stats`, `;++cheat count.stats`.
  - Computed targets follow the same rules: `;x cheat k[node]` assigns the existing field named by `k`.
  - Assigning a missing field raises `tantrum`. A shifty record's shape is as fixed as a frozen one's; only its values change.
  - A field's new value may have a different type (until static types exist).
- Mutability is shallow and per container. The record or array that directly holds the target must be shifty; the containers around it need not be.
  - `;x cheat name.callee.node` requires `callee.node` to be shifty. `node` may be frozen.
  - `;x cheat kind.i[items]` requires `i[items]` to be shifty. `items` may be frozen.

### 10.10 Record equality and stdlib
- `==` is reference equality. A record never equals a non-record. Ordering two records raises `tantrum` (§11.1).
- `;a, b($same)` recurses into records: they are the same when they have the same field names (order ignored) and each pair of field values is `$same`. Arrays and records may nest freely.
- Records have no interpolation conversion (§9.3), so `;node($print)` is a type error.
- `;node($typeOf)` is `"record"`.
- `;node($fields)` returns a frozen array of the field names, in literal order.
- Array and string builtins (`$length`, `$at`, `$slice`, ...) raise `tantrum` on a record.

---

## 11. Operators and Numerics

Standard semantics throughout.

### 11.1 Categories
- **Arithmetic:** `+ - * / % **`, unary `-` (no unary `+`)
- **Comparison:** `== != < > <= >=`
  - All six promote int to float when mixed with a float (§11.2).
  - Otherwise strict, no coercion: any other cross-type compare is `fake` (e.g. `real == wah`, `"1" == wah`).
  - Ordering (`< > <= >=`) two booleans, two arrays, two records, two lineup members or two `uninvited` values raises `tantrum`.
- **Logical:** `&& || !`, booleans only, short-circuiting
- **Bitwise:** `& | ^ ~ << >>`, integers only, int32 range
  - Shift amount outside 0–31 is a runtime error.
  - No `>>>`. Use `;x($toUint32)` for an unsigned interpretation.
- **Ternary:** `?:`

### 11.2 Numeric rules
- **Promotion:** in arithmetic (`+ - * / % **`), an int mixed with a float is promoted to float.
- **Division:** `/` always returns a float, including `int / int`.
  - Integer results via rounding functions, which return integers: `$floor`, `$ceil`, `$round`, `$trunc`.
  - Integer division: `;a / b($floor)`. (`//` is the comment token, so there is no integer-division operator.)
- **Modulo:** floored (Python rule). The result takes the **divisor's** sign: `-7 % 3` = 2.
  - Preserves `a == (a / b)($floor) * b + a % b`.
  - Integer modulo by zero raises `tantrum`. Float division and modulo follow IEEE (`wah / wh` is `everybody`).
- **Exponent:** `int ** int` with a non-negative exponent is an int (overflow raises `tantrum`). A negative exponent gives a float: `waah ** WAH` is `0.5`.
  - A float result of `**` may differ between backends in the last bit, as `pow` does between C libraries. Every other float operation gives the same bits on every backend.
- **Comparison across numeric types** (`== != < > <= >=`): the int is promoted to float for the comparison only; the operand's value and type are unchanged.
  - `wah < wa[1.5]h` is `real`.
  - `wah == wa[1.0]h` is `real`, so `wa[4]h / waah == waah` is `real`.
  - Type is still observable elsewhere: interpolation prints `1` vs `1.0`, and bitwise ops reject floats.
- **Constants** (reserved keywords, not literals):
  - `nobody`: NaN. Follows IEEE: `nobody == nobody` is `fake`. Test with `;x($isNobody)`.
  - `everybody`: positive infinity. Negative infinity is `-everybody`.
- **Exponent and unary minus:** unary `-` applied directly to the left operand of `**` is a compile error without parentheses.
  - `-(x ** 2)` and `(-x) ** 2` are valid; `-x ** 2` is not.
  - Negative literals are unaffected: `WAH ** waah` = 1.

### 11.3 Numeric stdlib
```
;n($toFloat)                                      // int -> float
;"1.5"($toFloat)                                  // wails; also parses "wa[1.5]h" (§9.5)
;x($floor)  ;x($ceil)  ;x($round)  ;x($trunc)     // float -> int; identity on int
;x($toUint32)
;x($isNobody)
```
- `$round` rounds half away from zero (`wa[2.5]h` → 3, `WA[2.5]H` → -3).
- Rounding `nobody` or `±everybody` raises `tantrum`.

### 11.4 Precedence (highest to lowest)

| Level | Operators | Assoc |
|---|---|---|
| 1 | grouping `( )`, literals, indexing `i[arr]`, member access `field.rec` | right (indexing, member access) |
| 2 | `**` | right |
| 3 | unary `- ! ~` | right |
| 4 | `* / %` | left |
| 5 | `+ -` | left |
| 6 | `<< >>` | left |
| 7 | `&` | left |
| 8 | `^` | left |
| 9 | `\|` | left |
| 10 | `< > <= >=` | left |
| 11 | `== !=` | left |
| 12 | `&&` | left |
| 13 | `\|\|` | left |
| 14 | `?:` | right |
| 15 | `,` (argument separator) | |
| 16 | `(f)` postfix call | left |

Bitwise binds tighter than comparison (Go/Rust/Python order), fixing C's `a & b == c` defect.

---

## 12. Standard Library

### 12.1 Conventions
- Every stdlib function and constant carries the `$` sigil (§2.9). New stdlib names can never break user code.
- I/O is **synchronous only** (no async model).
- All file reads and writes are **UTF-8**. The text rules are exact, so every backend behaves the same:
  - **Decoding** (`$readFile`, `$readLine`): each invalid UTF-8 sequence becomes U+FFFD, using the WHATWG "maximal subpart" rule (the same rule as JavaScript's `TextDecoder` and Python's `errors="replace"`). A byte-order mark at the start of a file or of stdin is kept as the character U+FEFF. An incomplete sequence at the very end of the input also becomes U+FFFD.
  - **Encoding** (`$write`, `$print`, `$eprint`, `$writeFile`, `$appendFile`): plain UTF-8, with no byte-order mark and no newline translation. `\n` is written as one byte on every platform.
  - **Program arguments and environment variables** (`$args`, `$env`) are UTF-8 on every platform. On Windows they are converted from the system's UTF-16, and an unpaired surrogate becomes U+FFFD.
- **Expected failures signal `WAH!`** (missing file, end of input, unset variable) with code 1. These functions are `wails`.
- Wrong argument types raise `tantrum`. Arity is checked at compile time.
- Names are plain, not themed: keywords carry the theme; stdlib names stay searchable and guessable.

### 12.2 Console
| Builtin | Call | Notes |
|---|---|---|
| `$print` | `;x($print)` | stdout plus newline |
| `$write` | `;x($write)` | stdout, no newline |
| `$eprint` | `;x($eprint)` | stderr plus newline |
| `$readLine` | `;,($readLine)` | One line of stdin, without its line ending. `WAH!` at end of input. |

`$print`, `$write`, `$eprint` accept any value with an interpolation conversion (§9.3); other types are a type error.

`$readLine` lines:
- A line ends at `\n`. A `\r` immediately before the `\n` is removed too. A `\r` anywhere else is part of the line.
- If input ends after some characters with no `\n`, those characters are the last line.
- At end of input with no characters left, `$readLine` signals `WAH!`. So input `"a\n"` gives one line, `"a"` gives one line, and `"a\n\n"` gives `"a"` then `""`.

**Broken pipe.** If a write to stdout or stderr fails because its reader has gone away (a closed pipe, as in `prog | head -1`), the program ends at once and prints nothing more. This follows the Unix convention for `SIGPIPE`, so Γ-- programs behave like other tools in a pipeline:
- Where the host has signals and leaves `SIGPIPE` at its default, the program is killed by it, and shells report status 141 (128 + 13).
- Elsewhere (Windows, or a host that ignores `SIGPIPE`, such as Node) the program exits with code 141.
- It is not a tantrum or a `WAH!`, so nothing can handle it. `$run` reports a child that ended this way as 141 either way (§12.4).

### 12.3 Files
| Builtin | Call | Notes |
|---|---|---|
| `$readFile` | `;path($readFile)` | Whole file as `purple`. `WAH!` if missing or unreadable. |
| `$writeFile` | `;path, text($writeFile)` | Creates or overwrites. `WAH!` on failure. |
| `$appendFile` | `;path, text($appendFile)` | `WAH!` on failure. |
| `$fileExists` | `;path($fileExists)` | `real` / `fake`. |
| `$listDir` | `;path($listDir)` | Frozen array of entry names (not full paths), without `.` and `..`, sorted by code point. `WAH!` if not a directory. |
| `$deleteFile` | `;path($deleteFile)` | `WAH!` on failure. |

Paths are purples. A relative path is relative to the current directory. `/` works as a separator on every platform; on Windows `\` works too.

### 12.4 Process
| Builtin | Form | Notes |
|---|---|---|
| `$args` | constant | Array of `purple`, script name excluded. |
| `$env` | `;"HOME"($env)` | `WAH!` if unset. |
| `$programPath` | constant | Absolute path of the file that was launched (see below). |
| `$run` | `;program, args($run)` | Runs another program and waits for it. Returns its exit code. `WAH!` if it cannot be started. |
| exit | `;!invited` | See §13. |

`$programPath` is the file the host started:
- a native build: the executable (`C:\tools\wahc.exe`)
- the JS backend: the compiled `.js` file
- `wahc0 run`: the `.wah` source file
- `wahc0 -e`: `wahc0`'s own script, since there is no source file

It is useful for finding files installed next to a program. Its separators are the platform's own.

`$run`:
- `program` is a purple. If it contains no path separator, it is looked up on `PATH`. On Windows, `.com` and `.exe` are tried as well.
  - Batch files (`.bat`, `.cmd`) and scripts are not programs: they need an interpreter. To run one, run the interpreter explicitly, for example `;"cmd.exe", ["/c", "build.cmd"]($run)`. That is an explicit request for a shell, with its quoting rules.
- `args` is an array of purples. Each element is passed as exactly one argument. There is **no shell**: no quoting, globbing or variable expansion. On Windows the runtime quotes the arguments so the program receives them unchanged.
- The program shares this program's stdin, stdout and stderr, current directory and environment. The runtime flushes its own output first, so output appears in order.
- The result is the exit code as an int. On POSIX, a program killed by signal `n` gives `128 + n`.
- `WAH!` (code 1) if the program cannot be found or started. A program that starts and then fails is not a signal; its exit code says so.
- Example: `;"clang", ["-O2", "-o", "foo.exe", "foo.c"]($run) claim code`

### 12.5 Reflection
| Builtin | Call | Notes |
|---|---|---|
| `$typeOf` | `;x($typeOf)` | `"int"`, `"float"`, `"boolean"`, `"purple"`, `"array"`, `"record"` or `"uninvited"`; for a lineup member, the lineup's name (§4.1). Useful with arrays as tuples. |
| `$fields` | `;node($fields)` | Frozen array of a record's field names, in literal order (§10.10). |

### 12.6 Stage 0 coverage
`wahc0` implements every builtin in §9.4, §10.6, §10.10, §11.3 and §12.2–§12.5, with the text and I/O rules of §9.3, §9.5 and §12.1.

It also implements modules (§13.1) and lineups (§4.1). The C backend (`--target c`, §15) implements the same, and programs behave the same on both.

### 12.7 Shell usage note
- In shells, `$name` inside double quotes is expanded by the shell before `wahc` sees it. Use single quotes: `wahc -e ';x($print)'`.
- In Makefiles, write `$$print`.

---

## 13. Program Structure

- The entry file's **last statement** must be `;!invited`.
  - Missing: `error: program ended, but Waluigi was never told he wasn't invited`
- `;!invited` may appear earlier for early exit.
- Exit code prefix: `;WAH !invited` exits with -1. Bare `;!invited` exits with 0. The prefix may be any int expression: `;code !invited`.
- Empty statements after the final `;!invited` are ignored by the last-statement rule.
- Library files end with their `bribe` exports and do not require `!invited` (§13.1).

### 13.1 Modules (`steal` / `bribe`)

A program is an **entry file** plus the **libraries** it steals from, directly or indirectly. Modules exist only at compile time: there are no module values, and names are resolved statically.

```
// lexer.wah: a library
;["scheme", "claim"] claim keywords
;scheme (c)isSpace { c == " " || c == "\n" }
;scheme (src)tokenize { ... }
;tokenize, keywords bribe

// main.wah: the entry file
;"lexer.wah" steal lexer
;"x y"(tokenize.lexer)($length)($print)
;keywords.lexer($length)($print)
;!invited
```

**Stealing**
- `;"path" steal name` imports a library and gives it the module name `name` in this file. The path is a plain string literal (no interpolation). It is relative to the directory of the file containing the `steal`, uses `/` as its separator, and includes the `.wah` extension.
- `steal` statements may appear only at the top level, before every other statement. Comments and empty statements may come before them.
- The module name is the only name a `steal` declares. It is visible throughout the file and behaves like a scheme name for the no-shadowing rule (§5.1): any other declaration with the same name, at any depth, is a compile error. The stealing file chooses it, so a clash can always be fixed in that file.
- What the library bribes is reached through the module name, with member access (§10.8):
  - a claim: `keywords.lexer`, which can be indexed or accessed further like any value (`wh[keywords.lexer]`, `name.cfg.lexer`);
  - a scheme: called as `(tokenize.lexer)` (§6.2). Like any scheme, it is not a value;
  - a lineup: its members are `member.Lineup.module`, also as alibi values (§7.5). The lineup itself is not a value.
- Using a name the module does not bribe is a compile error: `'lexer.wah' does not bribe 'tokenise'`. A module name is not a value.
- Bribed names never enter the stealing file. Two libraries may bribe the same name, and the stealing file may declare that name too: `f`, `f.x` and `f.y` are three different things.
- Stealing is not transitive. A file sees only the modules it steals itself.
- Stealing the same library twice in one file, even under two names, is a compile error.
- Stealing a file that is not a library (one that ends with `;!invited`) is a compile error.
- Style: name a module after its file (`;"lexer.wah" steal lexer`), unless that name is taken in the stealing file.

**Bribing**
- A library's last statement (ignoring empty statements) is a single `;name, name, ... bribe`, listing at least one name. As everywhere in Γ--, the values come before the keyword.
- Each bribed name must be a scheme, a claim or a lineup (§4.1) declared at the library's top level. A module name cannot be bribed, so a library cannot pass on what it stole.
- Names that are not bribed are private. Private names in different modules never conflict.

**Library contents**
- At its top level a library may contain only `steal`, `scheme`, `claim`, `lineup` and empty statements, followed by the `bribe`. Other statements, including `;!invited`, are compile errors there. Inside schemes, everything is allowed, including `;!invited`.
- Only the library that declares a binding may `cheat` it: `;wah cheat n.lib` is a compile error. Importers read it as `n.lib` and see its current value. The contents of a stolen shifty array or record can still be mutated, following the usual per-container rule (§10.9): `;wah cheat wh[xs.lib]`.

**Loading and initialization**
- A library is loaded once per program, however many files steal it. Libraries are identified by their resolved path.
- A cycle of `steal`s is a compile error that lists the cycle: `import cycle: a.wah -> b.wah -> a.wah`.
- Libraries are initialized depth first, in `steal` order, each before any file that steals it. Initializing a library evaluates its top-level claims once, in source order. The entry file's body runs last. The order is fully determined, so every backend runs claims, including any side effects in their values, in the same order.

**Diagnostics** name the file they occur in. The compiler is given the entry file and finds libraries through their `steal`s.

---

## 14. Example

```
;scheme (n)fizzbuzz {
    ;wah claim shifty i
    ;lurk (i <= n) {
        ;!doubt (i % wa[15]h == wh) {
            ;"WAHWAH"($print)
        } anyway !doubt (i % waaah == wh) {
            ;"WAH"($print)
        } anyway !doubt (i % waaaaah == wh) {
            ;"WAAAH"($print)
        } anyway {
            ;i($print)
        }
        ;++cheat i
    }
}

;scheme (a, b)average {
    (a + b) / waah
}

;"x"($toInt) claim n
    whine (c) { wh }

;wa[100]h(fizzbuzz)
;wah, waah(average)($print)        // 1.5
;!invited
```

---

## 15. Bootstrap Plan

1. **Stage 0 (`wahc0`, JS):** compiles Γ-- to JS. **Done**; see `js/`. It covers everything specified here except the §16 open questions.
2. **Stage 1:** write `wahc` in Γ--, compile with `wahc0`, which produces `wahc.js`. **Done**; see `wahc/`.
3. **Stage 2:** `wahc.js` compiles `wahc.wah`. Output must match stage 1 byte for byte (fixed point). **Done**; the `wahc` tests check it.
4. **C backend:** both compilers compile Γ-- to C with `--target c`, and their output matches byte for byte. **Done**; see `runtime/`.
5. **Native bootstrap:** `wahc/wahc.c` is `wahc` compiled to C. Built with a C compiler, it gives a native `wahc` that needs no JavaScript, and that compiles itself back to the same `wahc.c` (native fixed point). **Done**; the `wahc` tests check it.
6. Retire `wahc0`.

Codegen notes:
- `WAH!` compiles to throwing a `Wah` object; each call site with `whine` gets its own try/catch.
- Non-shifty arrays: `Object.freeze`.
- Records: null-prototype objects. Frozen records use `Object.freeze`, and shifty records use `Object.seal` (fixed shape, mutable values). Reads check for an own property, so a missing field raises `tantrum` instead of returning `undefined`.
  - Field names are mangled (e.g. `f_kind`), and computed keys are mangled the same way at runtime. With the null prototype, this keeps keys like `"__proto__"`, `"constructor"` or `"toString"` from reaching JS internals.
- Strings: code-point operations via `Array.from` (O(n) indexing, acceptable for stage 0).
- Int vs. float must be tracked at runtime (JS has one number type), e.g. a tagged float wrapper, so `/`, `==`, and interpolation behave per §11.2.
- Stdlib: `$name` compiles to a member of a runtime object (e.g. `$rt.print`), so it can never collide with emitted user identifiers.
- I/O: Node `fs.*Sync`.
- Stage 0 as built:
  - Floats are wrapped and ints are plain JS numbers.
  - User names are emitted as `w_name`.
  - The runtime is inlined, so the output runs with plain `node`.
  - A signal raised by a scheme is marked as it leaves the scheme. Any other signal reaching a scheme boundary becomes a tantrum, which implements the one-frame rule.
  - Shifty records are tracked in a `WeakSet`. `Object.isFrozen` cannot tell them apart, because JS reports an empty sealed object as frozen.

C backend notes:
- The output is one C file with `runtime/wah.c` inlined. It needs a GNU C compiler (clang or gcc; `zig cc` works everywhere) for statement expressions `({ ... })`, which carry conditionals and `suspect` used as values.
- Values are a 16-byte tagged struct. Purples are UTF-8. Lineup members are pointers to static structs.
- Memory comes from an arena that is never freed. `wahc` compiling itself peaks near 120 MB.
- Schemes are C functions. The bindings a nested scheme uses live in a frame struct on the C stack, reached through an `up` pointer, which is enough because a nested scheme never outlives the call that contains it (§16). A module's top-level bindings are globals.
- A binding read from another scheme is checked for the temporal dead zone (§5.1) by a tag, and a captured claim in a block is unclaimed again each time the block runs.
- `WAH!` sets a pending code and returns. After each call, the caller either handles it with `whine` or turns it into a tantrum (the one-frame rule). Tantrums cannot be caught yet, so a tantrum prints the scheme stack and ends the program; nothing unwinds.
- C leaves the order of a call's arguments unspecified, so arguments whose order matters are evaluated into temporaries, left to right (§6.2).
- Float text and `$toFloat` are exact on any C library: the runtime corrects `strtod` with an exact comparison. Float `**` uses fdlibm's `pow` (§11.2).

Stress test: `examples/tokenize.wah` is a Γ-- tokenizer written in Γ--. Its output must match `wahc0`'s lexer token for token, on itself and on the other examples.

Examples: `examples/` must stay in sync with the spec, and each major feature gets an example that demonstrates it (e.g. `examples/records.wah` for §10.7–§10.10). The `wahc0` tests run every example.

---

## 16. Open Questions

**Errors**
- Syntax for `blame` / `sulk` / `regret` (block placement, error binding).

**Not yet designed**
- Functions as values and closures. Deferred until after the native bootstrap: today a nested scheme can never outlive the call that contains it, so a native backend needs no heap-allocated closures.
- Static types and `wails` checking. Must decide the type of a computed field read `k[rec]` (§10.8): literal key types (like TypeScript's `keyof`), or a dynamic result that needs a check.
- Catching tantrums from `blame` (stage 0 has no way to catch them).
- Maps/dictionaries: a separate type from records (§10.7). Needs a literal syntax, key types, `key[map]` indexing, and builtins such as `$keys`, `$has` and `$remove`. Stage 1 can manage with arrays of pairs, at O(n) lookup.

**Runtime**
- Copy-on-write storage for the copies of §10.4: a copy would share the elements until one side changes them. It would change no behavior. Measured in Draft 0.8, `wahc` compiling itself made about 190,000 copies (some 23 MB, a fifth of its peak memory) but spent little time on them, so `fickle` (§5) came first, since it removes the copies a program does not need.
