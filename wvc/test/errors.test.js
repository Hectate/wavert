// wvc's diagnostics for Wavert mistakes: each snippet gives exactly this
// error or warning (the first line of it, and its hint if there is one).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { diagnostics, noZig } from './helpers.js';

const skip = noZig;

// The first diagnostic for source as "line:col: severity: message", with its
// hint after " | " when it has one.
function first(source, files) {
  const text = diagnostics(source, files);
  const m = /^main\.wvt:(\d+:\d+: \w+: .*)\n(?:  hint: (.*)\n)?/.exec(text);
  assert.ok(m, `no diagnostic for ${JSON.stringify(source)}:\n${text}`);
  return m[2] ? `${m[1]} | ${m[2]}` : m[1];
}

const cases = [
  // lexical
  [';const x = 1\n;exit\n', "1:10: error: '=' is not an operator | declare with ';const x is value', assign with ';set x to value', and compare with '=='"],
  [';var x is 1\n;x++\n;exit\n', "2:3: error: '++' is not an operator | write ';set x += 1' to increment"],
  [';const x is 5.\n;exit\n', "1:13: error: digits are required on both sides of '.' | write 5.0"],
  [';const x is 0x\n;exit\n', "1:13: error: '0x' must be followed by hex digits"],
  [';const x is 12ab\n;exit\n', "1:13: error: '12ab' is not a numeric literal"],
  [';const x is 99999999999999999\n;exit\n', '1:13: error: integer literal exceeds the 53-bit safe range'],
  [';const x is ! if\n;exit\n', "1:13: error: no whitespace is allowed inside '!if' | write '!if'"],
  [';const x is 1 + recover\n;exit\n', "1:17: error: 'recover' is reserved: catching panics is not designed yet"],
  [';const s is "${1}"\n;exit\n', '1:14: warning: Wavert interpolation is {x}, not ${x}'],
  // declarations and assignment
  [';var x is 1\n;x += 1\n;exit\n', "2:2: error: an assignment starts with 'set' | ;set x to value"],
  [';const x 1\n;exit\n', "1:10: error: expected 'is', found '1' | ;const x is value"],
  [';set x 1\n;exit\n', "1:8: error: expected 'to' or a compound assignment such as '+=' after the target, found '1' | ;set x to value"],
  [';const fn is 1\n;exit\n', "1:8: error: expected a name after 'const', found 'fn' | ;const name is value"],
  [';const x is 1\n;set x to 2\n;exit\n', "2:6: error: cannot set 'x': it is not a var or ref | declare it with ';ref x is ...' to rebind it, or ';var x is ...' for a mutable copy"],
  [';fn f(a) { ;set a to 2 }\n;exit\n', "1:17: error: cannot set 'a': it is not a var or ref | mark the parameter (ref a) to rebind it, or (var a) for a mutable copy"],
  [';const x is 1\n;const x is 2\n;exit\n', "2:8: error: 'x' is already declared in this scope (line 1)"],
  // calls
  [';const x is 1\n;(x)\n;exit\n', "2:3: error: 'x' is not a function | (x) is a call with no arguments; to use the value, remove the parentheses"],
  [';const x is 1\n;3(x)\n;exit\n', "2:4: error: 'x' is not a function | calls are postfix: the function comes last, e.g. x(f)"],
  [';(1, 2)\n;exit\n', '1:2: error: a parenthesized list must be followed by the function it is passed to | (a, b)(f) calls f with a and b'],
  [';()\n;exit\n', '1:2: error: empty parentheses | a call with no arguments is written (f)'],
  [';fn seven() { 7 }\n;(seven)(seven)\n;exit\n', "2:10: error: 'seven' takes 0 arguments, got 1"],
  [';fn f(x) { x }\n;const y is f\n;exit\n', "2:13: error: 'f' is a function; functions are not values | to call it, write x(f), or (f) with no arguments"],
  [';const x is $toPurple\n;exit\n', "1:13: error: unknown standard library name '$toPurple' | '$' is reserved for the standard library; user names have no sigil"],
  // statements
  [';"hi"($print)\n', '2:1: error: a program must end with ;exit | end the program with ;exit, or a library with ;end'],
  [';return\n;exit\n', "1:2: error: 'return' outside a function"],
  [';break\n;exit\n', "1:2: error: 'break' outside a loop"],
  [';fail\n;exit\n', "1:2: error: 'fail' outside a function: there is no caller to fail to"],
  [';fn f() { ;fail 0 }\n;exit\n', '1:17: error: a fail code must be an integer >= 1'],
  [';const y is 1 onfail { 2 }\n;exit\n', "1:15: error: 'onfail' handles a call, but this statement's value is not a call | the call must be the outermost expression, e.g. ;const n is s(parse) onfail (c) { ... }"],
  [';else { }\n;exit\n', "1:2: error: 'else' without a preceding if or !if"],
  [';const x is if (true) { 1 }\n;exit\n', "1:13: error: a conditional used as a value must end with a plain 'else { ... }'"],
  [';const x is 1\n;x\n;exit\n', '2:2: warning: this statement has no effect'],
  [';fn f(const a) { a }\n;exit\n', "1:7: error: expected a parameter name, found 'const' | a parameter is a name, var name or ref name"],
  [';fn (a) { a }\n;exit\n', '1:5: error: the name comes before the parameters | ;fn name(a, b) { ... }'],
  // for loops
  [';for i in [1] { }\n;exit\n', "1:6: error: expected '(' after 'for', found 'i' | ;for (x in xs) { ... } or ;for (i in 0, n) { ... }"],
  [';for (x of [1]) { }\n;exit\n', "1:9: error: expected 'in', found 'of' | ;for (x in xs) { ... } or ;for (i in 0, n) { ... }"],
  [';for (fn in [1]) { }\n;exit\n', "1:7: error: 'fn' is a keyword and cannot be a loop variable | ;for (x in xs) { ... } or ;for (i in 0, n) { ... }"],
  [';for (i, j in 0, 3) { }\n;exit\n', '1:10: error: a range gives one value on each pass, so it takes one name | ;for (i in a, b) { ... }'],
  [';for (i in 0, 3, 0) { }\n;exit\n', "1:18: error: a for loop's step must be greater than 0 | to count down, add reversed: ;for (i in 0, n, 2 reversed)"],
  [';for (i in 0, 3, -1) { }\n;exit\n', "1:18: error: a for loop's step must be greater than 0 | to count down, add reversed: ;for (i in 0, n, 2 reversed)"],
  [';for (i in 3, 0 backwards) { }\n;exit\n', "1:17: error: expected ')' to close the for loop, found 'backwards' | to go backwards, write reversed: ;for (i in 0, n reversed)"],
  [';for (i in 0, 3) { ;set i += 1 }\n;exit\n', "1:25: error: cannot set 'i': a for loop's variable cannot be set | to replace an element, set it through its array, such as ;set xs[i] to v; to control a counter yourself, use a while loop"],
  [';var xs is [1]\n;for (x in xs) { ;set xs to [] }\n;exit\n', "2:23: error: cannot set 'xs' inside a for loop over it | build the new value in another variable, and set 'xs' to it after the loop"],
  [';var s is { items: [1] }\n;for (x in s.items) { ;set s to { items: [] } }\n;exit\n', "2:28: error: cannot set 's' inside a for loop over 's.items' | build the new value in another variable, and set 's' to it after the loop"],
  [';var xs is [1]\n;for (x in xs) { ;fn clear() { ;set xs to [] } ;(clear) }\n;exit\n', "2:37: error: cannot set 'xs' inside a for loop over it | build the new value in another variable, and set 'xs' to it after the loop"],
  [';const i is 1\n;for (i in [1]) { }\n;exit\n', "2:7: error: 'i' shadows 'i' from an enclosing scope (line 1) | Wavert forbids shadowing; choose another name"],
  [';fn f() { 1 }\n;for (x in f) { }\n;exit\n', "2:12: error: 'f' is a function; functions are not values | to call it, write x(f), or (f) with no arguments"],
  // enums and match
  [';enum (a)\n;exit\n', '1:7: error: the name comes before the members | ;enum Name (a, b)'],
  [';enum E (a, b)\n;fn f(e) { match (e) { case E.a { 1 } } }\n;exit\n', "2:12: error: match does not handle 'E.b' | add a case for each, or end the match with 'else { ... }'"],
  [';enum E (a)\n;E.b($print)\n;exit\n', "2:4: error: no member 'b' in enum 'E' | its members are a"],
  [';fn f(x) { match (x) { case 1.5 { 1 } else { 2 } } }\n;exit\n', '1:29: error: floats cannot be case values | float equality makes a poor match; compare with if instead'],
  // members and indexing
  // map literals
  [';const m is ["a": 1, "a": 2]\n;exit\n', '1:22: error: duplicate key in map literal'],
  [';const m is [1: 1, -1: 2, 1: 3]\n;exit\n', '1:27: error: duplicate key in map literal'],
  [';const m is [1.5: 1]\n;exit\n', '1:14: error: a map key must be an int, a string, a boolean, an enum member or a record'],
  [';const m is [null: 1]\n;exit\n', '1:14: error: a map key must be an int, a string, a boolean, an enum member or a record'],
  [';const m is [1: 2, 3]\n;exit\n', "1:21: error: expected ':' after a key in the map literal, found ']' | map literals are written [key: value, ...]"],
  [';const m is [1: 2,]\n;exit\n', '1:18: error: trailing comma in map literal'],
  [';const m is [:1]\n;exit\n', "1:15: error: expected ']' after '[:', found '1' | an empty map is written [:]"],
  [';const x is a.if\n;exit\n', "1:15: error: 'if' is a keyword and cannot be a field name | member access is written record.field, e.g. node.kind"],
  // modules
  [';export fn f() { 1 }\n;exit\n', "1:2: error: 'export' is only allowed in a library, which ends with ;end"],
  [';fn f() { 1 }\n;end\n', '2:2: error: a library must export at least one declaration | mark one with \'export\', e.g. ;export fn name(x) { ... }'],
  [';export fn f() { 1 }\n;end\n;"x"($print)\n', "2:2: error: ';end' must be the last statement of a library"],
  [';import lib from "lib.wah"\n;exit\n', '1:18: error: an import path includes the .wvt extension'],
  [';import lib "lib.wvt"\n;exit\n', '1:13: error: expected \'from\', found a string | ;import lib from "lib.wvt"'],
  [';const x is 1\n;import lib from "lib.wvt"\n;exit\n', "2:2: error: 'import' must come before every other statement"],
];

// Type errors the checker can prove (§4.2): each would panic whenever it runs.
const typeCases = [
  [';const n is 3\n;const s is "n=" + n\n;exit\n', "2:18: error: '+' cannot be applied to string and int | use interpolation, as in \"n={n}\""],
  [';const r is { kind: 1, hp: 5 }\n;r.knid($print)\n;exit\n', "2:4: error: no field 'knid' in record { kind, hp }"],
  [';var r is { kind: 1, hp: 5 }\n;set r.hpp to 2\n;exit\n', "2:8: error: no field 'hpp' in record { kind, hp }"],
  [';const r is { a: { b: 1 } }\n;r.a.c($print)\n;exit\n', "2:6: error: no field 'c' in record { b }"],
  [';fn pos(x, y) { { x: x, y: y } }\n;(1, 2)(pos).z($print)\n;exit\n', "2:14: error: no field 'z' in record { x, y }"],
  [';fn f(n) { ;if (n > 0) { ;return { a: 1 } } { a: 2 } }\n;5(f).b($print)\n;exit\n', "2:7: error: no field 'b' in record { a }"],
  [';const t is ($terminalSize) onfail { ;exit }\n;t.w($print)\n;exit\n', "2:4: error: no field 'w' in record { width, height }"],
  [';const n is 3\n;n.x($print)\n;exit\n', "2:4: error: reading field 'x' needs a record, got int"],
  [';import grid from "$lib/grid.wvt"\n;const g is (3, 3, 0)(grid.new)\n;g[(1, 1)(grid.pos)]($print)\n;exit\n',
    "3:16: error: a record's field name must be a string, got record { x, y }, indexing record { width, height, cells } | a record is indexed by the name of a field; for other keys, such as positions, use a map"],
  [';const s is "abc"\n;s[0]($print)\n;exit\n', "2:3: error: indexing needs an array, a map or a record, got string | use $at for a string: (s, i)($at)"],
  [';const xs is [1]\n;xs["0"]($print)\n;exit\n', "2:5: error: an index must be an int, got string"],
  [';const xs is [1]\n;const n is xs * 2\n;exit\n', "2:16: error: '*' cannot be applied to array and int"],
  [';const x is -"a"\n;exit\n', "1:13: error: '-' cannot be applied to string"],
  [';const x is 1.5 & 1\n;exit\n', "1:13: error: '&' requires integers, got float"],
  [';const x is !"yes"\n;exit\n', "1:14: error: '!' requires true or false, got string"],
  [';enum K (a, b)\n;const x is K.a < K.b\n;exit\n', "2:17: error: '<' cannot order enum members"],
  [';const xs is [1, 2]\n;if (xs($length)) { }\n;exit\n', '2:9: error: a condition must be true or false, got int'],
  [';const r is { a: 1 }\n;"{r}"($print)\n;exit\n', "2:4: error: cannot convert record { a } to text; convert it explicitly | use records.toString from $lib/records.wvt, or interpolate its fields"],
  [';const xs is [1]\n;xs($print)\n;exit\n', "2:2: error: cannot convert array to text; convert it explicitly | join its elements with (xs, \", \")($join), or use arrays.toString from $lib/arrays.wvt"],
  [';const n is 5\n;n($keys)($print)\n;exit\n', '2:2: error: $keys expects a map, got int'],
  [';const xs is [1, 2]\n;for (i in 0, xs) { }\n;exit\n', "2:15: error: a for loop's range needs ints, got array | to go through an array, write ;for (x in xs), or ;for (i, x in xs) for each index too"],
  [';for (i in 0, 10, 0.5) { }\n;exit\n', "1:19: error: a for loop's step must be an int, got float"],
  [';const s is "abc"\n;for (c in s) { }\n;exit\n', '2:12: error: a for loop goes through an array, a map, an enum or a range of ints, got string | for its characters, go through (s, "")($split)'],
];

for (const [source, expected] of [...cases, ...typeCases]) {
  test(JSON.stringify(source), { skip }, () => {
    assert.equal(first(source), expected);
  });
}

// What the checker cannot be sure of, it does not report.
const typeOk = [
  ';var n is 0\n;set n to "s"\n;const s is "n=" + n\n;exit\n',      // rebound: its type is not known
  ';fn f(r) { r.anything }\n;exit\n',                                 // a parameter's type is not known
  ';var r is { a: 1 }\n;set r.a to "x"\n;const s is r.a + 1\n;exit\n', // a mutable record's field may change type
  ';fn f(x) { ;if (x) { ;return 1 } "s" }\n;const s is true(f) + 1\n;exit\n', // returns an int or a string
  ';const r is { a: 1 }\n;r["a"]($print)\n;exit\n',
  ';const n is 1 + 2.5\n;"{n * 2}"($print)\n;exit\n',
  ';var xs is []\n;(xs, { a: 1 })($push)\n;xs[0].a($print)\n;exit\n',
  ';for (i, x in [1, 2]) { ;(i + 1)($print) }\n;exit\n',
  ';const n is "5"($toInt) onfail { 0 }\n;(n + 1)($print)\n;exit\n',
  ';const n is 1 < "a"\n;exit\n',                                      // ordering across types is false (§11.1)
];

for (const source of typeOk) {
  test(`no error: ${JSON.stringify(source)}`, { skip }, () => {
    assert.equal(diagnostics(source), '');
  });
}

test('importing a program is an error', { skip }, () => {
  const files = { 'prog.wvt': ';"x"($print)\n;exit\n' };
  assert.equal(first(';import prog from "prog.wvt"\n;exit\n', files), "1:19: error: 'prog.wvt' is not a library: it ends with ;exit");
});

test('an importer cannot set an exported var', { skip }, () => {
  const files = { 'lib.wvt': ';export var n is 1\n;end\n' };
  assert.equal(first(';import lib from "lib.wvt"\n;set lib.n to 2\n;exit\n', files), "2:10: error: cannot set 'lib.n': only 'lib.wvt' may set it");
});

test('a library cannot run statements at its top level', { skip }, () => {
  const files = { 'lib.wvt': ';"x"($print)\n;export const n is 1\n;end\n' };
  const text = diagnostics(';import lib from "lib.wvt"\n;exit\n', files);
  assert.match(text, /lib\.wvt:1:2: error: a library's top level may contain only import, fn, const, var, ref and enum declarations\n  hint: move other statements into a function/);
});
