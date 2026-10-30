# Wavert Language Specification (Draft 0.1)

> Draft 0.1 is the first Wavert specification. Wavert is derived from Γ-- ([wah/wah-spec.md](../wah/wah-spec.md), Draft 0.8). It changes Γ--'s syntax and names and keeps its semantics; [hybrid-syntax.md](hybrid-syntax.md) gives the reasons for each change. Section numbers follow the Γ-- specification, so a reference such as §10.4 means the same topic in both.

## 1. Overview

| Item | Value |
|---|---|
| Name | **Wavert** |
| File extension | `.wvt` |
| Compiler | `wvc` |

**Design rules:**
- Every statement starts with `;`. Calls are postfix: the arguments come first, then the function. Every file ends with a required marker.
- Everything else reads as in mainstream languages: names come first in declarations, indexing and member access read left to right, and the keywords are the usual ones.
- Operators keep their standard meaning, with strict typing and exact numeric rules (§11).

```
;fn average(a, b) {
    (a + b) / 2
}

;"Hello, World"($print)
;(1, 2)(average)($print)        // 1.5
;exit
```

---

## 2. Lexical Structure

Source files are UTF-8. A byte-order mark at the start of a file is ignored. Positions in diagnostics count code points.

### 2.1 Statements
- Every statement **begins** with `;`. There is no terminator.
  ```
  ;const x is 1
  ;x($print)
  ```
- Newlines are not significant: a statement may span lines, and several statements may share one.
- Blocks: `{ ;stmt ;stmt }`.
- `;` followed by `;`, `}` or the end of the file is an **empty statement** (valid). `;;` warns (§2.8).

### 2.2 Comments
- Line: `// ...`
- Block: `/* ... */`. Block comments do not nest. An unterminated block comment is a lex error.

### 2.3 Numeric literals
| Form | Examples | Type |
|---|---|---|
| decimal | `0`, `42`, `007` | integer |
| hexadecimal | `0xFF`, `0x1f` | integer |
| binary | `0b101` | integer |
| decimal with a fraction or an exponent | `1.5`, `1e9`, `2.5e-3`, `6E+2` | float |

- The prefixes `0x` and `0b` are lowercase. Hex digits may be either case.
- A float needs digits on both sides of `.`: `.5` and `5.` are lex errors (`5.` has the hint `write 5.0`).
- An exponent needs digits: `1e` is a lex error.
- A literal followed directly by a letter, digit, `_`, or a `.` and a digit is a lex error: `12ab`, `0x`, `0b2`, `1.5.3`.
- An integer literal must be within the 53-bit safe range (±(2^53−1), §4). A float literal whose value would be infinite is a lex error.
- **Negative numbers.** `-` is not part of the literal token. The parser reads `-` directly before a number literal as a negative literal, so `-1` is a constant that can be a `case` value (§7.5). Parenthesized, `-(1)` stays a negation of `1`; the value is the same.
  - Unary minus binds looser than a call (§11.4), so `-5(abs)` is `-(5(abs))`. Write `(-5)(abs)` to pass -5.
  - `-2 ** 2` is a compile error, like `-x ** 2` (§11.2).

### 2.4 Bang tokens
- There are exactly two bang tokens: `!if` and `!while`. Each is a single token.
- `!` before any other word is the NOT operator: `!true`, `!x`, `!iffy`.
- Whitespace inside them (`! if`) is a lex error.

### 2.5 Keywords
Keywords are reserved and cannot be used as names, parameters, field names or enum members:

| Concept | Keyword |
|---|---|
| function | `fn`, `return` |
| declarations and assignment | `const`, `var`, `ref`, `set` |
| conditionals | `if`, `!if`, `else` |
| multi-way branch | `match`, `case` |
| loops | `while`, `!while`, `break`, `continue` |
| enumeration | `enum` |
| modules | `import`, `export` |
| constants | `true`, `false`, `null`, `nan`, `inf` |
| failures and errors | `fail`, `onfail`, `panic` |
| program end, early exit | `exit` |

**Reserved for later**, and lex errors today: `fails` (marks a function that can fail, §8.2), `recover` and `finally` (catching panics, §8.1).

**Contextual keywords** are ordinary names everywhere except in one position:
- `is`, right after the name in `;const x is ...`, `;var x is ...` and `;ref x is ...`;
- `to`, right after the target in `;set target to ...`;
- `from`, right after the module name in `;import name from "path"`;
- `end`, as a whole statement: `;end` followed by `;`, `}` or the end of the file (§13.1).

So `;const end is 5`, `node.end` and `{ to: 1 }` are all valid.

### 2.6 Strings
- Delimiter: `"..."` only. There is no character type.
- Escapes: `\n \r \t \\ \" \$ \u{1F608}`. `\u{...}` takes 1 to 6 hex digits naming a Unicode scalar value. An unknown or invalid escape is a lex error.
  - `\$` produces a literal `$` and suppresses the `$` lints (§2.8).
- A raw newline inside a string is a lex error.
- **Interpolation:** `"score: {n}"`. Any expression may appear between the braces: `"{n / 2}"`, `"{xs($length)}"`, `"{items[i].name}"`.
- Literal braces are written `{{` and `}}`. A lone `}` is a lex error, so `"}"` must be written `"}}"`.
  - `{{` is always a literal brace. An interpolation that starts with a record literal needs a space: `"{ {a: 1}.a }"`.
- An empty interpolation `"{}"` is a lex error.

### 2.7 Operators and forbidden tokens
- Operators: `** * / % + - << >> & ^ | < > <= >= == != && || ! ~ ? :`, and the compound assignments `+= -= *= /= %= **= &= |= ^= <<= >>=`, which are single tokens valid only in `;set` (§5).
- Punctuation: `; , ( ) [ ] { } .`
- **Bare `=` is a lex error** whose hint names the right forms: `declare with ';const x is value', assign with ';set x to value', and compare with '=='`. Wavert has no `=`, so it cannot be confused with `==`.
- `++` and `--` are lex errors. Increment with `;set x += 1`. Double negation is written `- -x` or `-(-x)`.
- `>>>` is a lex error; use `x($toUint32)`.
- `.` must touch both sides. Whitespace or a comment around it (`node . kind`) is a lex error.
- Any other character outside strings and comments is a lex error.

### 2.8 Lint warnings
- Missing whitespace around a binary operator or the ternary's `?` and `:` (`a+b`).
- `a//b` warns that `//` starts a comment and that integer division is `(a / b)($floor)`.
- `!if (a != b)` is a double negative; it suggests `if (a == b)`.
- `;;` warns: likely a habitual trailing `;` from C-style languages.
- `${` inside a string: `Wavert interpolation is {x}, not ${x}`.
- `$identifier` inside a string: `did you mean {identifier}?`.
- A string-literal key in computed field access: `node["kind"]` suggests `node.kind` (§10.8).
- An expression statement that calls nothing has no effect: `;n`, `;a + b`, `;node.kind`. Inside a block the hint says that a block's value is written without a leading `;` (§7.2).
  - A statement that contains any call, or is a conditional chain or a `match`, is not affected.
- Inside a block, a `[` at the start of a line that indexes the expression before it (§7.2).

### 2.9 Stdlib sigil (`$`)
- `$` immediately followed by an identifier is a **stdlib name**: `$print`, `$length`.
- `$` is not valid in user identifiers, so user names can never collide with the standard library, and adding a stdlib name never breaks a program.
- An unknown `$name` is a compile error with the hint `'$' is reserved for the standard library; user names have no sigil`.
- `$` anywhere else (bare, trailing `x$`, `$"..."`) is a lex error.

### 2.10 Identifiers
- An identifier is `[A-Za-z_][A-Za-z0-9_]*`, other than a keyword or reserved word (§2.5).
- Names are case-sensitive.

---

## 3. Grammar Summary

The grammar in EBNF. `{ x }` is zero or more, `[ x ]` is optional, and quoted text is literal. The words `IS`, `TO`, `FROM` and `END` are the contextual keywords of §2.5, which are identifiers elsewhere.

```
file        = { statement } ;
statement   = ";" [ body [ onfail ] ] ;
onfail      = "onfail" [ "(" ident ")" ] block ;
body        = [ "export" ] ( fn | enum | decl ) | import | set
            | "return" [ expr ] | "panic" expr | "break" | "continue"
            | ( "while" | "!while" ) "(" expr ")" block
            | "fail" [ expr ] | "exit" [ expr ] | END | expr ;
fn          = "fn" ident "(" [ param { "," param } ] ")" block ;
param       = [ "var" | "ref" ] ident ;
enum        = "enum" ident "(" ident { "," ident } ")" ;
import      = "import" ident FROM string ;
decl        = ( "const" | "var" | "ref" ) ident IS expr ;
set         = "set" postfix ( TO expr | compound expr ) ;
compound    = "+=" | "-=" | "*=" | "/=" | "%=" | "**=" | "&=" | "|=" | "^=" | "<<=" | ">>=" ;
block       = "{" { statement } [ expr ] "}" ;

expr        = ternary ;
ternary     = binary [ "?" ternary ":" ternary ] ;
binary      = unary { binop unary } ;              (* by precedence, §11.4 *)
unary       = ( "-" | "!" | "~" ) unary | power ;
power       = postfix [ "**" unary ] ;
postfix     = primary { call | "[" expr "]" | "." ident } ;
call        = "(" callee ")" ;
callee      = ident | stdname | ident "." ident ;
primary     = number | string | "true" | "false" | "null" | "nan" | "inf"
            | ident | stdname | call                       (* a call with no arguments *)
            | "(" expr ")"
            | "(" expr "," expr { "," expr } ")" call      (* a call with several arguments *)
            | "[" [ expr { "," expr } ] "]"
            | "{" [ ident ":" expr { "," ident ":" expr } ] "}"
            | chain | match ;
chain       = ( "if" | "!if" ) "(" expr ")" block
              { "else" ( "if" | "!if" ) "(" expr ")" block } [ "else" block ] ;
match       = "match" "(" expr ")" "{" case { case } [ "else" block ] "}" ;
case        = "case" caseval { "," caseval } block ;
caseval     = [ "-" ] integer | string | "true" | "false" | "null"
            | ident "." ident | ident "." ident "." ident ;
```

Notes on the grammar:
- `primary` tries `call` before `"(" expr ")"`: `(name)`, `($name)` and `(module.name)` where an expression starts are always calls (§6.2).
- After an expression, `(` continues it only as a `call`. Any other `(` ends the expression, so a statement can be followed by a block value that starts with `(` (§7.2).
- `"export"` is allowed only at the top level, and `END` only as the last statement of a file (§13.1).
- Trailing commas are not allowed in any list.

---

## 4. Types

| Type | `$typeOf` | Notes |
|---|---|---|
| integer | `"int"` | 53-bit safe range (±(2^53−1)). A result outside it panics. Bitwise operators require the int32 range (§11.1). |
| float | `"float"` | IEEE 754 double. |
| boolean | `"boolean"` | `true` / `false`. Distinct from integers: `true == 1` is `false`. |
| string | `"string"` | Immutable sequence of Unicode code points (§9). |
| array | `"array"` | Heterogeneous, reference semantics (§10). |
| record | `"record"` | Fixed set of named fields, reference semantics (§10.7). |
| null | `"null"` | `null` equals only itself. `0 == null` is `false`. |
| enum member | the enum's name | A named constant of an enumeration (§4.1). Equals only itself. |

Implicit conversions (the complete list):
- **Int-to-float promotion** in arithmetic and comparison (§11.2).
- **String interpolation** (§9.3).

### 4.1 Enums
```
;enum Kind (num, str, call, chain)

;const k is Kind.call
;match (k) {
    case Kind.num, Kind.str { ;"literal"($print) }
    case Kind.call { ;"call"($print) }
    case Kind.chain { ;"chain"($print) }
}
```
**Declaring**
- `;enum Name (member, member, ...)` declares an enum named `Name` with a fixed, ordered set of members.
- An enum is declared only at the top level of a file. It is visible throughout the file, like a function, so the no-shadowing rule (§5.1) treats its name like a function name.
- It has at least one member. Members are identifiers, not keywords. A duplicate member is a compile error.
- Member names are labels, not bindings, like record field names (§10.8). A binding `num` and a member `Kind.num` can coexist.

**Using**
- `Name.member` is the member's value. It is resolved at compile time, and an unknown member is a compile error: `no member 'cal' in enum 'Kind'`.
- An enum name is not a value. It may appear only on the left of `Name.member`, so `;Kind($print)` is a compile error.
- A member is equal (`==`) only to itself. It never equals a member of another enum, a string or a number.
- Ordering (`< > <= >=`) or arithmetic on members panics.
- Interpolation and `$toString` give the member's name (`call`). `$typeOf` gives the enum's name (`"Kind"`). `$same` on members is `==`.
- Members can be stored in arrays and records, passed to functions and returned like any value. They are immutable, so the copy rules (§10.4) do not apply to them.
- A library exports an enum by marking it `export` (§13.1). Its members are then written `module.Name.member`.

---

## 5. Bindings

```
;const x is 1             // frozen
;var y is [1, 2]          // mutable
;ref z is node            // alias: rebindable, never copies
;set y to [3]             // assign
;set y[0] to 4            // assign an element
;set node.kind to "num"   // assign a field
;set n += 1               // compound assignment
```
- **Three declarators**, one per kind of binding:
  - `const` cannot be reassigned, and its value is frozen: declaring a mutable array or record with `const` takes a frozen snapshot (§10.4). Mutating a frozen value panics.
  - `var` can be reassigned, and always holds a mutable value. Declaring it with a frozen array or record copies the value and makes the copy mutable.
  - `ref` can be reassigned, and holds each value as it is, never copying it: a frozen value stays frozen, and a mutable value is shared. Use `ref` for a name that walks or reads a structure, or that is rebound to successive values, and `var` for a value whose contents change.
- `;set` assigns. Its target is a name, an indexed element or a record field.
  - Setting a name requires a `var` or `ref` binding; otherwise it is a compile error, with a hint naming the declaration to use.
  - Setting an element or field requires the container that directly holds it to be mutable (§10.9). Through a `const` binding this is a compile error, and through a `ref` binding it is checked at runtime.
  - Rebinding an array or record follows the target's column of the copy table (§10.4).
- Compound forms: `+= -= *= /= %= **= &= |= ^= <<= >>=`. `;set x op= v` means `;set x to x op v`. There is no increment operator: write `;set i += 1`.
- The value is evaluated before the target is read.
- An assignment that does not start with `set` is a compile error with a hint: `;x += 1` and `;x to 2` say `an assignment starts with 'set'`.

### 5.1 Scoping
- Bindings are **block-scoped**: a declaration is visible from its statement to its block's end, including nested blocks.
- **No shadowing:** a binding (including a function parameter) may not reuse a name visible from an enclosing scope. Compile error.
  - Sibling blocks may reuse a name, since neither encloses the other.
  - Visibility is positional. An outer `const`, `var` or `ref` conflicts only if it appears before the inner declaration.
  - Outer function names, enum names, module names, parameters and `onfail` codes are visible throughout their scope, so they always conflict.
  - Redeclaring a name in the same scope is an error.
  - The standard library cannot be shadowed, because its names carry the `$` sigil (§2.9).
- **Use before declaration:**
  - Using a name before its declaration in the same function is a **compile error**.
  - Functions are **hoisted** to the top of their block, so mutual recursion works regardless of order.
  - A hoisted function that reads an outer binding before that binding's declaration has run panics at runtime: `'x' was used before its declaration`.
  - Nested functions may read and `set` bindings of enclosing scopes.
  - A declared name becomes visible at the end of its statement, including any `onfail` handler. So `;const n is s(parse) onfail (c) { n }` is a compile error.

---

## 6. Functions

### 6.1 Definition
```
;fn add(a, b) {
    a + b
}
;fn main() { ... }
```
- The value of a call is the body's block value (§7.2): its trailing expression without a leading `;`, else `null`.
- `;return x` returns early. A bare `;return` returns `null`.
- Parameters are frozen by default, like `const`: an argument that is a mutable array or record is copied, frozen (§10.4).
- `var` parameters are mutable local bindings that follow the copy table: `;fn eat(var toks) { ... }`.
  - A mutable array argument is shared; a frozen one is copied.
  - Scalars are never passed by reference: `set` on a parameter does not affect the caller.
- `ref` parameters take the argument as it is, never copying it, and can be rebound with `set`: `;fn walk(ref node) { ... }`. They are the cheapest way to pass a mutable array that the function only reads.
- **Functions are not values.** A function name may appear only as a callee (§6.2): `;const g is f` is a compile error with the hint `to call it, write x(f), or (f) with no arguments`. A call's arity is checked at compile time.

### 6.2 Calls (postfix)
- The argument comes before the function: `;x(f)` is `f(x)`.
- **Several arguments are grouped** in parentheses: `;(a, b)(f)` is `f(a, b)`.
- **No arguments:** `(f)` where an expression starts is a call with no arguments: `;(now)`, `;(now)(format)` is `format(now())`.
  - This is why a parenthesized name is never grouping: `(x)` calls `x`. If `x` is not a function, it is a compile error with the hint `(x) is a call with no arguments; to use the value, remove the parentheses`.
  - `()` is a compile error.
- **Call form:** a call is exactly `(name)`, `($name)` or `(module.name)`, where `name` is a function, `$name` a stdlib function, and `module.name` a function an imported module exports (§13.1).
  - After an expression, any other `(` ends that expression. So a block value such as `(a + b) / c` may follow a statement: `{ ;x($print) (a + b) / c }`.
  - A parenthesized list that is not followed by a call is a compile error: `;(1, 2)` says `a parenthesized list must be followed by the function it is passed to`.
- **Calls bind tightest**, together with indexing and member access (§11.4), and chain left to right:
  - `;x(parse)(validate)(save)` is `save(validate(parse(x)))`.
  - `;1 + x(f)` is `1 + f(x)`; `i < xs($length)` and `c(isSpace) || c == ";"` need no parentheses.
  - `;items[i](describe)`, `;x(parse).kind`, `;x(f)[0]` mix freely.
  - To call a function with a whole expression, group it: `;(a + b)($print)`.
- Commas never capture: `[x, y(f)]` is `[x, f(y)]`, and `{ a: x, b: y(f) }` is `{ a: x, b: f(y) }`.
- C-style `;print("hi")` is a parse error: after `print`, `("hi")` is not a call form, so the statement ends there, and the next statement must start with `;`. The error's hint says that calls are postfix.

---

## 7. Control Flow

### 7.1 Conditionals
- `if (cond) { ... }` runs its block when `cond` is `true`.
- `!if (cond) { ... }` (unless) runs its block when `cond` is `false`.
- Conditions must be booleans. Any other type panics.
- `else` may be followed by a block or by another `if` or `!if`:
  ```
  ;if (n < 0) {
      ;"negative"($print)
  } else if (n == 0) {
      ;"zero"($print)
  } else !if (n > 100) {
      ;"small positive"($print)
  } else {
      ;"large"($print)
  }
  ```
- The whole chain is one statement (one leading `;`). Blocks are always braced, so there is no dangling `else`.

### 7.2 Block values (unified rule)
Applies to every block: function bodies, conditional and `match` branches, and `onfail` handlers.
- A block's value is its **trailing expression without a leading `;`**.
- If the block has none (it is empty, or its last statement starts with `;`), its value is `null`.
  ```
  { a + b }           // value: a + b
  { ;a + b }          // value: null (an expression statement; warns, §2.8)
  { ;x($print) ; }    // value: null (ends with an empty statement)
  { }                 // value: null
  ```
- Only blocks in value positions use their value. Blocks are not standalone expressions.
- A statement ends where its expression cannot continue. Anything left before the `}` is the block's value, and it must come last.
- A conditional chain **as a statement** (its result is not used) needs no `else` and no branch values, and `break`, `continue` and `return` work as usual.
- A conditional chain **as a value** (its result is declared, passed to a call, used in an expression, or is a block's value):
  - It must end in a plain `else { ... }`; otherwise it is a compile error.
    ```
    ;const label is if (n < 0) { "neg" } else { "non-neg" }
    ```
  - Every branch must produce a value (explicit values, below).
  - `return`, and a `break` or `continue` that would leave the conditional, are compile errors there. A `break` or `continue` for a loop inside a branch is fine.

**Explicit values.** A stray `;` turns a block's value into `null` without any other sign, so a value that is used must be written out. If `null` is intended, write it at the end: `{ ;x($print) null }`.
- **Functions.** If any call uses a function's result, the function must be able to produce a value: its body has a trailing value, or contains a `;return` with a value (outside nested functions). Otherwise it is a compile error: `'f' never produces a value, but its value is used (line N)`.
  - A call's result is used unless the call is the whole of an expression statement (`;x(log)`).
  - A bare `;return` does not produce a value.
- **Other blocks used as values** must end with a trailing value, unless their last statement never completes. The statements that never complete are `return`, `panic`, `exit`, `fail`, `break` and `continue`.
  - These blocks are the branches of a conditional chain or `match` used as a value, and the `onfail` handler of a `const`, `var`, `ref`, `set`, `return`, `panic` or `exit` statement.
  - The handler of an expression statement (`;x(f) onfail { ... }`) is not used as a value.
  ```
  ;const r is if (ok) { "yes" } else { ;"no"($print) }        // error: the else block has no value
  ;const r is if (ok) { "yes" } else { ;"no"($print) null }   // ok
  ;const text is path($readFile)
      onfail { ;"cannot read {path}"($eprint) ;exit 2 }        // ok: the handler never completes
  ```

**A value swallowed by the statement before it.** A statement ends only where its expression cannot continue, so a block's value that could continue the statement before it does:
- `[` indexes the previous expression: `{ ;x(f) [1, 2] }` is `{ ;x(f)[1, 2] }`. Inside a block, a `[` that starts a line and indexes the expression before it warns: `'[' at the start of a line indexes the expression before it`.
- `(f)` calls `f` with the previous expression: `{ ;x($print) (next) }` is `{ ;next(x($print)) }`, which the arity check usually rejects.
- `-` subtracts: `{ ;x(f) -1 }` is `{ ;x(f) - 1 }`.

The fix is to parenthesize the value: `([1, 2])`, `((next))`, `(-1)`.

### 7.3 Ternary
- `cond ? a : b`, standard semantics. `cond` must be a boolean. The ternary is right-associative: `a ? b : c ? d : e`.

### 7.4 Loops
- `while (cond) { ... }` runs while `cond` is `true`.
- `!while (cond) { ... }` (until) runs while `cond` is `false`.
- `break` leaves the innermost loop; `continue` starts its next iteration. Outside a loop they are compile errors.

### 7.5 Multi-way branch (`match` / `case`)
```
;match (e.kind) {
    case "int", "float" {
        ;"number"($print)
    }
    case "str" {
        ;"text"($print)
    }
    else {
        ;"other"($print)
    }
}
;const name is match (op) { case "+" { "add" } case "-" { "sub" } else { "?" } }
```
- `match (subject) { ... }` evaluates the subject once, then runs the first `case` with a value equal to it (`==`, §11.1). If none matches, it runs the `else` block, if there is one.
- There is **no fallthrough**. Each `case` has its own block, and one `case` may list several values separated by commas.
- `break` and `continue` inside a `match` refer to the enclosing loop.
- **Values are constants**, all of one of these two sorts. Mixing the sorts, or members of two enums, in one `match` is a compile error.
  - **Literals**: integers (including negative ones such as `-1`), string literals without interpolation, `true`, `false` and `null`.
    - Floats, `nan` and `inf` are not allowed, because float equality makes poor match keys. A float subject can still match an integer value, since `==` promotes (`1.0` matches `case 1`).
  - **Members of one enum** (§4.1), written `Name.member`, or `module.Name.member` for an imported enum.
  - The same value twice in one `match` is a compile error, even across different `case`s.
- Layout: at least one `case`, then an optional `else`, which must come last.
- **Exhaustive enum matches.** An enum `match` without `else` must list every member; otherwise it is a compile error that names what is missing: `match does not handle 'Kind.chain'`.
  - If such a `match` is given a subject that matches nothing (one that is not a member of the enum), it panics: `match: no case for "call"`.
  - With `else`, any members may be left out.
  - A literal `match` without `else` does nothing when nothing matches.
- The whole `match` is one statement (one leading `;`), like a conditional chain.
- **As a statement**, the branches need no value, and `break`, `continue` and `return` work as usual.
- **As a value**, it follows the conditional chain rules (§7.2): it must end with `else { ... }` unless it is an exhaustive enum `match`, and every branch must produce a value.
- Formatting: each `case` and the `else` go on their own lines, one level in from `match`.

---

## 8. Failures and Panics

Wavert has two error mechanisms: a **fail** is an expected failure that only the immediate caller sees, and a **panic** is a fatal error.

### 8.1 `panic`
- Syntax: `;panic expr`. Any value may be given: a string is printed as it is, and any other value as a literal (`[1, 2]`, `"a"` inside an array).
- Runtime errors panic too: type errors, out-of-range indexes, integer overflow, mutating a frozen value, and an unhandled fail (§8.2).
- A panic ends the program. It prints `uncaught panic: ` and the message, then the function stack, innermost first, to stderr, and exits with code 1:
  ```
  uncaught panic: it broke
      in boom
      in main
  ```
- Catching panics is not designed yet. `recover` and `finally` are reserved for it (§16).

### 8.2 `fail` and `onfail`
- For **expected, recoverable** failures.
- `;fail` leaves the current function and delivers a failure to the **immediate caller only**. It never propagates further on its own.
- Every fail carries a code, an integer ≥ 1:
  - `;fail` has code 1.
  - `;fail 3` has code 3. A literal code below 1 is a compile error.
  - `;fail c` has the code computed by `c`. A computed code that is not an integer ≥ 1 panics.
- A fail is handled at the call site with `onfail`. The handler's block value (§7.2) becomes the call's value:
  ```
  ;const n is "abc"($toInt)
      onfail (c) { 0 }
  ```
- Formatting: `onfail` goes on its own line, indented one level, which separates the failure path from the intended result.
- `onfail` follows a whole statement and handles that statement's **outermost** call, the call that produces its value.
  - In `;const n is s(parse)(validate) onfail (c) { ... }` it handles `validate` only. To handle `parse`, split the statement.
  - If the statement's value is not a call, `onfail` is a compile error.
  - It may follow an expression statement, `const`, `var`, `ref`, `set`, `return`, `panic` or `exit`.
- The code binding is optional: `onfail { 0 }`.
- **Forwarding is always explicit:**
  ```
  ;const n is s(parse)
      onfail (c) { ;fail c }
  ```
- `fail` is valid only inside a function (the top level has no caller). It is a statement, not an expression.
- An unhandled fail panics in the caller: `unhandled fail (code 3) from 'parse' in 'ignore'`, or, at the top level, `unhandled fail (code 1) from '$toInt'`.
- Later: functions that can fail will be marked `fails`, as in `;fn parse(s) fails { ... }`, and calling one without `onfail` will be a compile error.

---

## 9. Strings

### 9.1 Semantics
- A string is a sequence of Unicode code points. `$length`, `$at` and `$slice` count code points.
- The storage encoding is a backend detail (UTF-8 in the C backend, UTF-16 in the JavaScript backend).
- Strings are immutable. A binding holding one can be rebound with `set` as usual.

### 9.2 Operators
- `+` concatenates two strings only. `string + int` is a type error.
- `== !=` compare by **value**.
- `< > <= >=` compare lexicographically by code point.

### 9.3 Interpolation conversions
| Value | Text |
|---|---|
| integer | decimal (`-1`) |
| float | decimal, always with a fractional part (`0.5`, `1.0`, `1.0e+21`); `nan`, `inf`, `-inf` |
| boolean | `true` / `false` |
| null | `null` |
| enum member | its name (`call`), §4.1 |

Anything else (arrays, records) must be converted explicitly; interpolating one panics.

**Float text, exactly.** Every backend produces the same text:
1. Take the **shortest** decimal digit string `d1…dk` (no trailing zeros) that converts back to exactly the same double. If several are equally short, take the one closest to the exact value.
2. Let `n` be the decimal exponent, so the value is `0.d1…dk × 10^n`.
3. If `1e-6 <= |v| < 1e21` (that is, `-6 < n <= 21`), write plain decimal notation: `1500`, `0.5`, `0.000001`, `123.456`.
4. Otherwise write exponent notation: `d1`, then `.d2…dk` if `k > 1`, then `e`, then the sign (`+` or `-`), then `n - 1` in decimal: `1e+21`, `1.5e-7`.
5. If the result has no `.` before the exponent (or the end), insert `.0` there: `1500.0`, `1.0e+21`.
6. A negative value gets a leading `-`. Negative zero is `-0.0`.

This is JavaScript's `Number.prototype.toString` plus step 5. `$toString` and `$join` use the same text.

### 9.4 String stdlib
```
;const n is s($length)
;const first is (s, 0)($at)          // negative indices count from the end
;const mid is (s, 1, 3)($slice)      // end-exclusive: code points 1 and 2
;const c is first($code)             // one code point -> int
;c($fromCode)                        // int -> a one-character string
;const parts is (s, ",")($split)     // "" splits into code points
;(parts, ", ")($join)                // elements converted as by interpolation
;n($toString)                        // the interpolation text of any convertible value
;"42"($toInt)                        // fails on anything but integer text (§9.5)
;"1.5"($toFloat)
```

### 9.5 Numeric text (`$toInt`, `$toFloat`)
The accepted text is exact: no surrounding whitespace, no leading `+`, and only ASCII digits.

`$toInt` accepts an optional `-`, then one or more decimal digits (`"42"`, `"-7"`, `"007"`; `"-0"` is `0`). Anything else fails with code 1, including float text, hex, and values outside the 53-bit safe range. An int argument is returned unchanged; any other non-string argument panics.

`$toFloat` accepts an optional `-`, digits, an optional fraction (`.` and one or more digits), and an optional exponent (`e` or `E`, an optional sign, one or more digits): `"1.5"`, `"-2"`, `"6.02e23"`.
- The result is the double nearest to the exact decimal value, with ties going to the even significand (correct rounding). A value too small to represent becomes `0.0` or `-0.0`.
- If the correctly rounded result would be infinite, or the text does not match, `$toFloat` fails with code 1. An int argument is converted, and a float argument is returned unchanged.

---

## 10. Arrays and Records

### 10.1 Basics
- Literal: `[a, b, c]`. Empty: `[]`.
- Zero-indexed.
- Heterogeneous.

### 10.2 Indexing
- `arr[i]` reads element `i`. Chains read left to right: `grid[i][j]`.
- Negative indices count from the end: `arr[-1]` is the last element.
- The valid range is `-len` to `len - 1`. Anything outside panics. An index must be an integer.
- On a record, indexing is computed field access with a string key: `node[k]` (§10.8). The rules above apply to arrays only.
- Indexing a string is a type error; use `$at`.

### 10.3 Mutability
- Arrays and records are **frozen** or **mutable**. Literals and the arrays the standard library returns are frozen; a `var` binding holds a mutable one (§10.4).
- `var` and `ref` bindings can be rebound (§5).
- Element assignment: `;set arr[i] to x`. The array must be mutable. Through a `ref` binding this is checked at runtime, because a `ref` binding may hold either kind.
- Mutating stdlib calls (`;(arr, x)($push)`) require a mutable array; a frozen one panics.

### 10.4 Copy vs. reference
A copy happens **only when mutability changes**:

| Source | `const b` | `var b` | `ref b` |
|---|---|---|---|
| frozen | share reference | copy, made mutable | share reference |
| mutable | shallow copy, frozen (snapshot) | share reference (alias) | share reference (alias) |

- Copies are **shallow**. Nested mutable arrays remain mutable through other aliases.
- `set` on a binding follows its column, so rebinding a `ref` binding never copies, and the binding keeps the value's identity (`==`, §10.5).
- Function parameters follow the same table: plain parameters snapshot mutable arguments (O(n)); `var` and `ref` parameters alias.
  - Passing a mutable array to a plain parameter copies it on **every** call. Hot paths should pass frozen arrays or use `var` or `ref` parameters.

### 10.5 Equality
- `==` is reference equality for arrays and records.
- `;(a, b)($same)` is deep structural equality; elements compare with `==`. It also recurses into records (§10.10).

### 10.6 Array stdlib
```
;arr($length)                // also works on strings
;(arr, i)($at)               // same as arr[i]
;(arr, 1, -1)($slice)        // end-exclusive, negative indices from the end; frozen result
;(arr, x)($push)             // mutable arrays only
;arr($pop)                   // mutable arrays only; panics if empty
;(a, b)($same)
;(parts, ", ")($join)
```

### 10.7 Records
- A record is a fixed set of named fields. Its literal sets its shape, and the shape never changes: fields cannot be added or removed.
- Literal: `{ kind: "num", value: n }`. Empty: `{}`.
- Keys are identifiers; keywords and `$names` are not valid keys.
  - A duplicate key is a compile error.
  - There is no shorthand: `{ kind }` is an error; write `{ kind: kind }`.
- `{` at the start of an expression always begins a record literal, because blocks are never expressions (§7.2).
  - Where a block is expected, `{` opens the block. A record as a block's value needs its own braces:
    ```
    ;fn leaf(n) { { kind: "num", value: n } }
    ;const r is if (ok) { { kind: "some" } } else { { kind: "none" } }
    ```
- Each field value is a full expression. A ternary in a value is unambiguous, because a key's `:` always follows an identifier directly after `{` or `,`: `{ k: c ? a : b }`.
- Field order is the literal order. It is observable only through `$fields` (§10.10).
- Record literals are frozen (§10.9).

### 10.8 Member access
- `rec.field` reads the field `field` of `rec`.
- The right side of `.` is a field **label**, not an expression. Labels are not bindings, so the no-shadowing rule does not apply: a binding `kind` and a field `kind` can coexist.
- Chains read left to right, mixing with indexing and calls (all level 1, §11.4):
  ```
  ;node.kind($print)
  ;node.callee.name($print)
  ;node.args[0]($print)
  ;items[i].kind($print)
  ;x(parse).kind($print)
  ```
- **Computed access** reuses indexing: `node[k]` reads the field whose name is the value of `k`.
  - `k` must be a string, otherwise it panics. Negative indexing does not apply to records.
  - A missing field panics. Computed keys reach only fields that already exist, for reads and writes alike; dynamic key sets need a map (§16).
  - A string-literal key lints (§2.8): `node["kind"]` suggests `node.kind`.
  - `$fields` and computed access together allow generic code over records: `;node[f]($print)` for each `f` in `node($fields)`.
- When the left side is an enum (§4.1) or a module name (§13.1), `.` is resolved at compile time instead: `Kind.call` is an enum member, `lexer.keywords` is what the module `lexer` exports as `keywords`, and `shapes.Shape.circle` is a member of the enum `Shape` from the module `shapes`.
- There are no methods: `.` is only member access, and calls stay postfix. `x.f(y)` parses as `y(x.f)`, a call of `y` with the field `x.f`, which is a compile error unless `y` is a function.
- `.` must touch both sides (§2.7).
- Reading a field of a non-record panics. Reading a missing field panics: `no field 'knid' in record`.

### 10.9 Record mutability
- Records use the same frozen/mutable model as arrays. The copy table (§10.4) applies with "record" in place of "array".
- `;set node.kind to x` assigns an existing field. `node` must be mutable. Through a `ref` binding this is checked at runtime.
  - Compound forms work too: `;set stats.count += 1`.
  - Computed targets follow the same rules: `;set node[k] to x` assigns the existing field named by `k`.
  - Assigning a missing field panics. A mutable record's shape is as fixed as a frozen one's; only its values change.
  - A field's new value may have a different type.
- Mutability is shallow and per container. The record or array that directly holds the target must be mutable; the containers around it need not be.
  - `;set node.callee.name to x` requires `node.callee` to be mutable. `node` may be frozen.
  - `;set items[i].kind to x` requires `items[i]` to be mutable. `items` may be frozen.

### 10.10 Record equality and stdlib
- `==` is reference equality. A record never equals a non-record. Ordering two records panics.
- `;(a, b)($same)` recurses into records: they are the same when they have the same field names (order ignored) and each pair of field values is `$same`. Arrays and records may nest freely.
- Records have no interpolation conversion (§9.3), so `;node($print)` panics.
- `;node($typeOf)` is `"record"`.
- `;node($fields)` returns a frozen array of the field names, in literal order.
- Array and string builtins (`$length`, `$at`, `$slice`, ...) panic on a record.

---

## 11. Operators and Numerics

Standard semantics throughout.

### 11.1 Categories
- **Arithmetic:** `+ - * / % **`, unary `-` (there is no unary `+`).
- **Comparison:** `== != < > <= >=`
  - All six promote an int to float when it is compared with a float (§11.2).
  - Otherwise strict, with no coercion: any other comparison across types is `false` (`true == 1`, `"1" == 1`).
  - Ordering (`< > <= >=`) two booleans, two arrays, two records, two enum members or two `null` values panics.
- **Logical:** `&& || !`, booleans only, short-circuiting.
- **Bitwise:** `& | ^ ~ << >>`, integers only, in the int32 range.
  - A shift amount outside 0–31 panics.
  - There is no `>>>`. Use `x($toUint32)` for an unsigned interpretation.
- **Ternary:** `?:`.

### 11.2 Numeric rules
- **Promotion:** in arithmetic (`+ - * / % **`), an int mixed with a float is promoted to float.
- **Division:** `/` always returns a float, including `int / int`.
  - Integer results come from the rounding functions, which return integers: `$floor`, `$ceil`, `$round`, `$trunc`.
  - Integer division: `(a / b)($floor)`. `//` starts a comment, so there is no integer-division operator.
- **Modulo:** floored (the Python rule). The result takes the **divisor's** sign: `-7 % 3` is `2`.
  - It preserves `a == (a / b)($floor) * b + a % b`.
  - Integer modulo by zero panics. Float division and modulo follow IEEE (`1 / 0` is `inf`).
- **Exponent:** `int ** int` with a non-negative exponent is an int (overflow panics). A negative exponent gives a float: `2 ** -1` is `0.5`.
  - A float result of `**` may differ between backends in the last bit, as `pow` does between C libraries. Every other float operation gives the same bits on every backend.
- **Overflow:** an integer result outside ±(2^53−1) panics.
- **Comparison across numeric types:** the int is promoted to float for the comparison only.
  - `1 < 1.5` is `true`; `1 == 1.0` is `true`, so `4 / 2 == 2` is `true`.
  - The type is still observable elsewhere: interpolation prints `1` and `1.0`, and bitwise operators reject floats.
- **Constants** (keywords, not literals):
  - `nan`: NaN. It follows IEEE: `nan == nan` is `false`. Test with `x($isNan)`.
  - `inf`: positive infinity. Negative infinity is `-inf`.
- **Exponent and unary minus:** unary `-` applied directly to the left operand of `**` is a compile error without parentheses, including for number literals.
  - `-(x ** 2)` and `(-x) ** 2` are valid; `-x ** 2` and `-2 ** 2` are not.

### 11.3 Numeric stdlib
```
;n($toFloat)                                      // int -> float
;"1.5"($toFloat)                                  // fails unless the text is a number (§9.5)
;x($floor)  ;x($ceil)  ;x($round)  ;x($trunc)     // float -> int; identity on int
;x($toUint32)
;x($isNan)
```
- `$round` rounds half away from zero (`2.5` → 3, `-2.5` → -3).
- Rounding `nan` or `±inf` panics.

### 11.4 Precedence (highest to lowest)

| Level | Operators | Assoc |
|---|---|---|
| 1 | literals, names, grouping `( )`, `(f)` with no arguments, `(a, b)(f)`, array and record literals, `if` and `match` expressions | |
| 2 | postfix call `x(f)`, indexing `x[i]`, member access `x.field` | left |
| 3 | `**` | right |
| 4 | unary `- ! ~` | right |
| 5 | `* / %` | left |
| 6 | `+ -` | left |
| 7 | `<< >>` | left |
| 8 | `&` | left |
| 9 | `^` | left |
| 10 | `\|` | left |
| 11 | `< > <= >=` | left |
| 12 | `== !=` | left |
| 13 | `&&` | left |
| 14 | `\|\|` | left |
| 15 | `?:` | right |

- The left operand of `**` is a level-2 expression, and its right operand a level-4 one: `2 ** -1` is valid, and `-x ** y` is an error (§11.2).
- Unary operators apply to a whole level-2 expression: `-x.value` is `-(x.value)`, `!c(isSpace)` is `!(isSpace(c))`, `-5(abs)` is `-(abs(5))`.
- Bitwise binds tighter than comparison (the Go, Rust and Python order), so `a & b == c` is `(a & b) == c`.
- There is no pipeline operator and no comma operator.

---

## 12. Standard Library

### 12.1 Conventions
- Every stdlib function and constant carries the `$` sigil (§2.9).
- I/O is **synchronous only**.
- All file reads and writes are **UTF-8**. The text rules are exact, so every backend behaves the same:
  - **Decoding** (`$readFile`, `$readLine`): each invalid UTF-8 sequence becomes U+FFFD, using the WHATWG "maximal subpart" rule. A byte-order mark at the start of a file or of stdin is kept as the character U+FEFF. An incomplete sequence at the very end of the input also becomes U+FFFD.
  - **Encoding** (`$write`, `$print`, `$eprint`, `$writeFile`, `$appendFile`): plain UTF-8, with no byte-order mark and no newline translation. `\n` is written as one byte on every platform.
  - **Program arguments and environment variables** (`$args`, `$env`) are UTF-8 on every platform. On Windows they are converted from UTF-16, and an unpaired surrogate becomes U+FFFD.
- **Expected failures fail** (§8.2) with code 1: a missing file, the end of input, an unset variable.
- Wrong argument types panic. Arity is checked at compile time.

### 12.2 Console
| Builtin | Call | Notes |
|---|---|---|
| `$print` | `;x($print)` | stdout, then a newline |
| `$write` | `;x($write)` | stdout, no newline |
| `$eprint` | `;x($eprint)` | stderr, then a newline |
| `$readLine` | `;($readLine)` | One line of stdin, without its line ending. Fails at the end of input. |

`$print`, `$write` and `$eprint` accept any value with an interpolation conversion (§9.3); other values panic.

`$readLine` lines:
- A line ends at `\n`. A `\r` immediately before the `\n` is removed too. A `\r` anywhere else is part of the line.
- If input ends after some characters with no `\n`, those characters are the last line.
- At the end of input with no characters left, `$readLine` fails. So input `"a\n"` gives one line, `"a"` gives one line, and `"a\n\n"` gives `"a"` then `""`.

**Broken pipe.** If a write to stdout or stderr fails because its reader has gone away (as in `prog | head -1`), the program ends at once and prints nothing more, following the Unix convention for `SIGPIPE`:
- Where the host has signals and leaves `SIGPIPE` at its default, the program is killed by it, and shells report status 141 (128 + 13).
- Elsewhere (Windows, or a host that ignores `SIGPIPE`, such as Node) the program exits with code 141.
- It is not a panic or a fail, so nothing can handle it.

### 12.3 Files
| Builtin | Call | Notes |
|---|---|---|
| `$readFile` | `;path($readFile)` | The whole file as a string. Fails if missing or unreadable. |
| `$writeFile` | `;(path, text)($writeFile)` | Creates or overwrites. Fails on error. |
| `$appendFile` | `;(path, text)($appendFile)` | Fails on error. |
| `$fileExists` | `;path($fileExists)` | `true` / `false`. |
| `$listDir` | `;path($listDir)` | Frozen array of entry names (not full paths), without `.` and `..`, sorted by code point. Fails if not a directory. |
| `$deleteFile` | `;path($deleteFile)` | Fails on error. |

Paths are strings. A relative path is relative to the current directory. `/` works as a separator on every platform; on Windows `\` works too.

### 12.4 Process
| Builtin | Form | Notes |
|---|---|---|
| `$args` | constant | Array of strings, the program's name excluded. |
| `$env` | `;"HOME"($env)` | Fails if unset. |
| `$programPath` | constant | Absolute path of the file that was launched (see below). |
| `$run` | `;(program, args)($run)` | Runs another program and waits for it. Returns its exit code. Fails if it cannot be started. |
| exit | `;exit`, `;exit code` | See §13. |

`$programPath` is the file the host started: for a native build, the executable; for the JavaScript backend, the compiled `.js` file. Its separators are the platform's own. It is useful for finding files installed next to a program.

`$run`:
- `program` is a string. If it contains no path separator, it is looked up on `PATH`. On Windows, `.com` and `.exe` are tried as well.
  - Batch files and scripts are not programs: run their interpreter explicitly, for example `;("cmd.exe", ["/c", "build.cmd"])($run)`.
- `args` is an array of strings. Each element is passed as exactly one argument. There is **no shell**: no quoting, globbing or variable expansion. On Windows the runtime quotes the arguments so the program receives them unchanged.
- The program shares this program's stdin, stdout, stderr, current directory and environment. The runtime flushes its own output first, so output appears in order.
- The result is the exit code as an int. On POSIX, a program killed by signal `n` gives `128 + n`.
- It fails (code 1) if the program cannot be found or started. A program that starts and then fails does not fail the call; its exit code says so.

### 12.5 Reflection
| Builtin | Call | Notes |
|---|---|---|
| `$typeOf` | `;x($typeOf)` | `"int"`, `"float"`, `"boolean"`, `"string"`, `"array"`, `"record"` or `"null"`; for an enum member, the enum's name (§4.1). |
| `$fields` | `;node($fields)` | Frozen array of a record's field names, in literal order (§10.10). |

### 12.6 Summary
| Name | Arity | | Name | Arity |
|---|---|---|---|---|
| `$print`, `$write`, `$eprint` | 1 | | `$push`, `$same`, `$at`, `$split`, `$join` | 2 |
| `$readLine` | 0 | | `$slice` | 3 |
| `$readFile`, `$fileExists`, `$listDir`, `$deleteFile` | 1 | | `$length`, `$code`, `$fromCode`, `$pop` | 1 |
| `$writeFile`, `$appendFile` | 2 | | `$toString`, `$toInt`, `$toFloat` | 1 |
| `$env` | 1 | | `$floor`, `$ceil`, `$round`, `$trunc`, `$toUint32`, `$isNan` | 1 |
| `$run` | 2 | | `$typeOf`, `$fields` | 1 |
| `$args`, `$programPath` | constant | | | |

### 12.7 Shell usage note
- In shells, `$name` inside double quotes is expanded by the shell. Quote Wavert source with single quotes.
- In Makefiles, write `$$print`.

---

## 13. Program Structure

A file is either a **program** or a **library**, decided by its last statement (ignoring empty statements):
- A program's last statement is `;exit`.
- A library's last statement is `;end` (§13.1).

Both markers are required, so a truncated file fails to compile. An entry file that ends with neither is a compile error (`a program must end with ;exit`), and so is an imported file (`a library must end with ;end`).

**Exit**
- `;exit` ends the program with code 0. `;exit code` ends it with `code`, which must be an int; anything else panics.
- `;exit` may appear anywhere in a program, for an early exit, and inside any function, including one in a library. At a library's top level it is a compile error.
- The program's output is flushed before it exits.

### 13.1 Modules (`import` / `export`)

A program is an **entry file** plus the **libraries** it imports, directly or indirectly. Modules exist only at compile time: there are no module values, and names are resolved statically.

```
// lexer.wvt: a library
;export const keywords is ["fn", "const"]
;fn isSpace(c) { c == " " || c == "\n" }       // private
;export fn tokenize(src) { ... }
;end

// main.wvt: the entry file
;import lexer from "lexer.wvt"
;"x y"(lexer.tokenize)($length)($print)
;lexer.keywords($length)($print)
;exit
```

**Importing**
- `;import name from "path"` imports a library and gives it the module name `name` in this file. The path is a plain string literal (no interpolation). It is relative to the directory of the file containing the `import`, uses `/` as its separator, and includes the `.wvt` extension.
- `import` statements may appear only at the top level, before every other statement. Comments and empty statements may come before them.
- The module name is the only name an `import` declares. It is visible throughout the file and behaves like a function name for the no-shadowing rule (§5.1).
- What the library exports is reached through the module name, with member access (§10.8):
  - a `const`, `var` or `ref`: `lexer.keywords`, which can be indexed or accessed further like any value (`lexer.keywords[0]`, `lexer.config.name`);
  - a function: called as `(lexer.tokenize)` (§6.2). Like any function, it is not a value;
  - an enum: its members are `module.Enum.member`, also as `case` values (§7.5). The enum itself is not a value.
- Using a name the module does not export is a compile error: `'lexer.wvt' does not export 'tokenise'`. A module name is not a value.
- Exported names never enter the importing file. Two libraries may export the same name, and the importing file may declare that name too: `f`, `x.f` and `y.f` are three different things.
- Importing is not transitive. A file sees only the modules it imports itself.
- Importing the same library twice in one file, even under two names, is a compile error.
- Importing a file that is not a library (one that ends with `;exit`) is a compile error.
- Style: name a module after its file (`;import lexer from "lexer.wvt"`), unless that name is taken.

**Exporting**
- `export` before a top-level `fn`, `const`, `var`, `ref` or `enum` declaration makes it visible to importers. Everything else is private, and private names in different modules never conflict.
- `export` is allowed only in a library: in a file that ends with `;exit` it is a compile error.
- Modules cannot be re-exported: a library cannot pass on what it imported.
- A library must export at least one declaration; `;end` without any `export` is a compile error.
- `;end` is the library's last statement, at its top level. Only empty statements may follow it.

**Library contents**
- At its top level a library may contain only `import`, `fn`, `const`, `var`, `ref` and `enum` declarations (exported or not), followed by `;end`. Other statements, including `;exit`, are compile errors there. Inside functions, everything is allowed.
- Only the library that declares a binding may `set` it: `;set lib.n to 1` is a compile error. Importers read it as `lib.n` and see its current value. The contents of an exported mutable array or record can still be changed, following the usual per-container rule (§10.9): `;set lib.xs[0] to 1`.

**Loading and initialization**
- A library is loaded once per program, however many files import it. Libraries are identified by their resolved path.
- A cycle of imports is a compile error that lists the cycle: `import cycle: a.wvt -> b.wvt -> a.wvt`.
- Libraries are initialized depth first, in import order, each before any file that imports it. Initializing a library evaluates its top-level declarations once, in source order. The entry file's body runs last. The order is fully determined, so every backend runs declarations, including any side effects in their values, in the same order.

**Diagnostics** name the file they occur in. The compiler is given the entry file and finds libraries through their imports.

---

## 14. Example

```
;fn fizzbuzz(n) {
    ;var i is 1
    ;while (i <= n) {
        ;if (i % 15 == 0) {
            ;"FizzBuzz"($print)
        } else if (i % 3 == 0) {
            ;"Fizz"($print)
        } else if (i % 5 == 0) {
            ;"Buzz"($print)
        } else {
            ;i($print)
        }
        ;set i += 1
    }
}

;fn average(a, b) {
    (a + b) / 2
}

;const n is "x"($toInt)
    onfail (c) { 0 }

;100(fizzbuzz)
;(1, 2)(average)($print)        // 1.5
;exit
```

More examples are in [examples/](../examples/).

---

## 15. Implementation

- **Compiler.** `wvc` ([wvc/](../wvc/)) is written in Wavert and compiles itself. [wvc/wvc.c](../wvc/wvc.c) is `wvc` compiled to C, the bootstrap seed: a C compiler is enough to build it, and the built `wvc` compiles its own sources back to the same file (the fixed point). It was bootstrapped from Γ--; see [hybrid-syntax.md](hybrid-syntax.md#bootstrap-status).
- **Backends.** `wvc` compiles to C (the default) and to JavaScript (`--target js`). Programs behave the same on both: the same output, the same exit codes and the same error messages.
  - Each output is a single file with its runtime inlined: [runtime/wvt.c](../runtime/wvt.c) or [runtime/wvt.js](../runtime/wvt.js). The JavaScript output runs with plain `node`.
  - The C output needs a GNU C compiler (clang or gcc; `zig cc` works on every platform) for statement expressions, which carry conditionals and `match` used as values.
- **C backend notes.**
  - Values are a 16-byte tagged struct. Strings are UTF-8. Enum members are pointers to static structs.
  - Memory comes from an arena that is never freed.
  - Functions are C functions. The bindings a nested function uses live in a frame struct on the C stack, reached through an `up` pointer; that is enough because a nested function never outlives the call that contains it (§16).
  - A fail sets a pending code and returns. After each call, the caller either handles it with `onfail` or turns it into a panic (the one-frame rule). A panic prints the function stack and ends the program; nothing unwinds.
  - C leaves the order of a call's arguments unspecified, so arguments whose order matters are evaluated into temporaries, left to right.
  - Float text and `$toFloat` are exact on any C library: the runtime corrects `strtod` with an exact comparison. Float `**` uses fdlibm's `pow` (§11.2).
- **JavaScript backend notes.** Ints are JavaScript numbers and floats are wrapped, so `/`, `==` and interpolation follow §11.2. Frozen arrays and records use `Object.freeze`, mutable records `Object.seal`. Records are null-prototype objects with mangled field names. A fail is thrown and caught at each call that has `onfail`.
- **Editor support.** [editors/vscode/](../editors/vscode/) has syntax highlighting and snippets for VS Code.
- **Tests.** `wvc/test` runs every example on both backends, checks this specification's syntax and diagnostics, and checks the bootstrap fixed point.

---

## 16. Open Questions

**Errors**
- Catching panics: `recover`, an `else` for "nothing panicked", and `finally`. The block placement and the binding of the panic value are not designed.
- Marking functions that can fail with `fails`, and requiring `onfail` when calling one. This is tied to static types.

**Not yet designed**
- Functions as values and closures. Today a nested function never outlives the call that contains it, so the native backend needs no heap-allocated closures. If functions become values, `(name)` at the start of an expression would become ambiguous (§6.2) and must be revisited.
- Static types, written `;const x: int is 5`. They must decide the type of a computed field read `rec[k]` (§10.8).
- Destructuring, written `;const [a, b] is pair`.
- Maps: a separate type from records, with a literal syntax, key types, `map[key]` indexing, and builtins such as `$keys`, `$has` and `$remove`.
- A pipeline operator `|>`. It is a syntax error today, so it can be added later without breaking code. Postfix calls already read as a pipeline; revisit after counting real inside-out chains.
- A type test operator. It should not be spelled `is`, which is a contextual keyword in declarations (§2.5).

**Rejected**
- Methods or uniform call syntax: `.` stays member access only, and calls stay postfix (§10.8).
- `=` in any form, including `:=` (§2.7).

**Runtime**
- Copy-on-write storage for the copies of §10.4. It would change no behavior, only memory use.
