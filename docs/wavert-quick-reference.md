# Wavert quick reference

Everything needed to write Wavert, in brief. The [specification](wavert-spec.md) is the authority; § numbers point into it. When in doubt, compile a small test file: `wvc` reports mistakes with a hint at the fix, which is faster than reasoning from the rules.

## Shape of a file

```
// hello.wvt: the comment at the top documents the file.
;import grid from "$lib/grid.wvt"     // imports come first

;const name is "World"
;"Hello, {name}"($print)
;exit                                  // a program ends with ;exit (or ;exit 2 for a status)
```

- **Every statement starts with `;`.** There is no terminator, newlines don't matter, and a statement may span lines.
- A **program** ends with `;exit`. A **library** ends with `;end`, exports with `;export`, and its top level holds only `import`, `fn`, `const`, `var`, `ref` and `enum` (§13).
- Comments: `//` and `/* */`. The `//` lines directly above a declaration are its documentation (§2.2).

## Bindings (§5)

```
;const x is 1              // frozen: cannot be set, and its value is frozen
;var xs is [1, 2]          // mutable: holds a mutable value (copies a frozen one)
;ref node is head          // alias: rebindable, never copies
;set xs[0] to 5            // every assignment starts with set
;set n += 1                // compound forms; there is no ++
```

There is no `=`: declare with `is`, assign with `set ... to`, compare with `==`.

## Functions and calls (§6)

```
;fn add(a, b) { a + b }        // the value is the trailing expression, with no ;
;fn log(var xs, ref g) { ... } // parameters: plain (frozen copy), var, ref
;(1, 2)(add)                   // calls are postfix: arguments, then the function
;x(f)                          // f(x)
;(now)                         // no arguments: (f) where an expression starts
;x(parse)(check)($print)       // chains left to right
```

- Calls bind tightest: `1 + x(f)` is `1 + f(x)`, and `i < xs($length)` needs no parentheses.
- Unary minus binds looser than a call: `-5(abs)` is `-(abs(5))`; write `(-5)(abs)`.
- Functions are not values: no callbacks, no storing a function.
- `;return x` returns early. A function ending in a `;` statement returns `null`.

## Control flow (§7)

```
;if (n < 0) { ... } else if (n == 0) { ... } else { ... }
;!if (done) { ... }                  // unless
;while (i < n) { ... }               // also !while (until), break, continue
;const sign is if (n < 0) { "-" } else { "+" }   // as a value it needs a final else
;match (key) {
    case "q", "escape" { ;break }
    case "up" { ... }
    else { ... }
}
;const c is cond ? a : b
```

- Conditions must be booleans.
- `match` cases are literal constants or members of one enum. An enum `match` without `else` must list every member.
- A block's value is its trailing expression without `;`. If a value is used, write it out, even if it's `null`: `{ ;x($print) null }`.

## Failures (§8)

```
;fn parse(s) {
    ;if (s == "") { ;fail }          // fail code 1; ;fail 3 for code 3
    s($toInt)
}
;const n is s(parse)
    onfail (c) { 0 }                 // the handler's value replaces the call's
;"x"($toInt)
    onfail { ;"not a number"($eprint) ;exit 2 }
;panic "unreachable"                 // fatal; cannot be caught
```

- A fail reaches only the immediate caller. Unhandled, it panics: `unhandled fail (code 1) from 'parse'`.
- A fail carries only a number. To report why, set a variable before failing.

## Values

| Kind | Notes |
|---|---|
| int | 53-bit, overflow panics. `0xFF`, `0b101`, `-1` |
| float | needs digits both sides: `1.5`, not `5.`. `nan`, `inf` |
| string | `"..."`, code points. Interpolation `"{expr}"`; escapes `\n \t \" \\ \$ \u{1F600}`; literal braces `{{ }}` |
| boolean, null | `true`, `false`, `null` |
| array | `[1, 2]`; `xs[i]`, `xs[-1]` from the end |
| record | `{ kind: "num", value: 1 }`; fixed fields, `r.value`, `r["value"]` |
| map | `["a": 1, 2: "b"]`, empty `[:]`; insertion order; `m[k]` panics if missing, `(m, k)($get)` fails |
| enum member | `;enum Color (red, green)` then `Color.red`; prints its name |

- `/` always gives a float. For integer division use `(a / b)($floor)`, or `math.div`, which is exact. `%` is floored, so `-7 % 3` is `2`.
- `+` joins two strings only: `"n=" + n` panics, so use `"n={n}"` or `n($toString)`.
- `==` compares arrays, records and maps **by reference**; `$same` compares their contents.
- Literals are frozen. A `var` binding holds a mutable copy (§10.4).
- **Copies are shallow:** a `const` snapshot of a record still shares the arrays inside it.
- Map keys are ints, strings, booleans, enum members, or flat records compared by contents (`{ x: 3, y: 4 }`; §10.11).
- Records have no types. Tell kinds apart with a `kind` field holding an enum member, and `match` on it (§10.10).

## Modules (§13.1)

```
;import shapes from "modules/shapes.wvt"   // relative to this file, with .wvt
;import grid from "$lib/grid.wvt"           // the standard library, lib/
;shapes.describe(...)                       // wrong: calls are postfix
;x(shapes.describe)                         // right
;shapes.Shape.circle                        // an imported enum's member
```

## Standard library: built-in `$names` (§12)

Hover in VS Code for details; the extension shows each one's notes.

| Area | Names |
|---|---|
| Console | `$print $write $eprint $readLine $readKey $waitKey $terminal $terminalSize` |
| Files | `$readFile $writeFile $appendFile $fileExists $listDir $deleteFile` |
| Process | `$args $env $programPath $run` |
| Strings | `$length $at $slice $code $fromCode $split $join $toString $toInt $toFloat` |
| Arrays | `$push $pop $same $sort $sortBy $reverse` |
| Maps | `$get $has $remove $keys $values` |
| Numbers | `$floor $ceil $round $trunc $toUint32 $isNan` |
| Reflection | `$typeOf $fields` |
| Time, random | `$time $elapsed $sleep $newRandom $random $randomFloat` |

Functions that can fail: `$readLine $readKey $waitKey $terminalSize $get $remove $readFile $writeFile $appendFile $listDir $deleteFile $env $run $toInt $toFloat`.

## Standard library: `lib/` modules, written in Wavert

Import with `;import name from "$lib/name.wvt"`; see [lib/README.md](../lib/README.md).

| Module | For |
|---|---|
| `math` | `abs min max clamp div isqrt gcd lcm sum minOf maxOf lerp` |
| `text` | `repeat padLeft padRight center trim startsWith endsWith indexOf contains replace toUpper toLower wrap` |
| `arrays` | `indexOf contains count range filled concat removeAt insertAt` |
| `maps` | `getOr add pushTo` |
| `grid` | Grids of cells, positions (`pos`), areas (`rect circle ...`) and combining them. One cell off the grid fails; areas use the part on the grid |
| `dice` | Rolling, dice notation (`"2d6+1"`), exact odds and `percent` |
| `view` | Drawing layers to rows of text or colour, a window that follows the player, the player's memory of the map, animation |

## Pitfalls

Real mistakes, each caught by the compiler:

- **`(name)` at the start of an expression is a call with no arguments.** `(before)(spaces)` calls `before`; write `before(spaces)`.
- **`onfail` handles only the statement's outermost call.** Not a call inside a ternary, an operator or an interpolation. Split it into its own statement:
  ```
  ;const v is (m, k)($get)
      onfail { 0 }
  ;"{v}"($print)
  ```
- **Nothing can be shadowed,** including a parameter with the name of a function in the same module, and a name declared earlier in an enclosing block. Choose another name.
- **A function used for its value must produce one.** A function ending in `;statement` has no value. The checker reports `'f' never produces a value, but its value is used`.
- **Enums are not values:** `;const C is view.Color` is an error. Write members out: `view.Color.red`.
- **A `{` in a string starts interpolation;** write `{{` for a literal one. `$name` in a string warns; write `\$name`.
- **Assigning through a parameter** needs `var` or `ref`. A plain parameter is a frozen copy, and a `const` grid or record cannot be changed through any binding.
- **`[`, `(f)` and `-` at the start of a block's last line continue the statement before it.** Parenthesize the value: `([1, 2])`, `(-1)`.

## Building and checking

```
zig cc -O2 -o .wvt/wvc wvc/wvc.c          # build the compiler (.exe on Windows)
.wvt/wvc build program.wvt                 # C beside it, then the program
.wvt/wvc program.wvt -o program.c          # just the C, to check a program compiles
.wvt/wvc --ast program.wvt                 # the syntax tree, when precedence is in doubt
```
