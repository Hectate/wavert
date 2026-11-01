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
  // enums and match
  [';enum (a)\n;exit\n', '1:7: error: the name comes before the members | ;enum Name (a, b)'],
  [';enum E (a, b)\n;fn f(e) { match (e) { case E.a { 1 } } }\n;exit\n', "2:12: error: match does not handle 'E.b' | add a case for each, or end the match with 'else { ... }'"],
  [';enum E (a)\n;E.b($print)\n;exit\n', "2:4: error: no member 'b' in enum 'E' | its members are a"],
  [';fn f(x) { match (x) { case 1.5 { 1 } else { 2 } } }\n;exit\n', '1:29: error: floats cannot be case values | float equality makes a poor match; compare with if instead'],
  // members and indexing
  // map literals
  [';const m is ["a": 1, "a": 2]\n;exit\n', '1:22: error: duplicate key in map literal'],
  [';const m is [1: 1, -1: 2, 1: 3]\n;exit\n', '1:27: error: duplicate key in map literal'],
  [';const m is [1.5: 1]\n;exit\n', '1:14: error: a map key must be an int, a string, a boolean or an enum member'],
  [';const m is [null: 1]\n;exit\n', '1:14: error: a map key must be an int, a string, a boolean or an enum member'],
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

for (const [source, expected] of cases) {
  test(JSON.stringify(source), { skip }, () => {
    assert.equal(first(source), expected);
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
