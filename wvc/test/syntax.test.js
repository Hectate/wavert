// Wavert syntax and its meaning (docs/hybrid-syntax.md), each program built
// and run natively.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { noZig, run } from './helpers.js';

const skip = noZig;

// The output of a program, which must compile and exit with 0.
function out(source, opts) {
  const r = run(source + '\n;exit\n', opts);
  assert.equal(r.err, '', source);
  assert.equal(r.code, 0, source);
  return r.out;
}
const lines = (...xs) => xs.map((x) => x + '\n').join('');

test('calls bind tightest, so operands need no parentheses', { skip }, () => {
  assert.equal(out(`
;fn double(x) { x * 2 }
;const xs is [1, 2, 3]
;(1 + 2(double))($print)
;(xs($length) > 2)($print)
;(1 + 2)(double)($print)
;3(double)(double)($print)`), lines('5', 'true', '6', '12'));
});

test('a call binds tighter than unary minus', { skip }, () => {
  assert.equal(out(`
;fn abs(x) { x < 0 ? -x : x }
;(-5(abs))($print)
;(-5)(abs)($print)`), lines('-5', '5'));
});

test('arguments are grouped before the function; arrays do not capture', { skip }, () => {
  assert.equal(out(`
;fn add(a, b) { a + b }
;fn neg(x) { -x }
;(1, 2)(add)($print)
;([1, 2(neg)], ", ")($join)($print)
;{ a: (3, 4)(add), b: 5(neg) }.a($print)
;((1, 2)(add), 10)(add)($print)`), lines('3', '1, -2', '7', '13'));
});

test('(f) where an expression starts is a call with no arguments', { skip }, () => {
  assert.equal(out(`
;fn seven() { 7 }
;(seven)($print)
;((seven) + 1)($print)
;([(seven), (seven)], "+")($join)($print)`), lines('7', '8', '7+7'));
});

test('member access and indexing read left to right', { skip }, () => {
  assert.equal(out(`
;const items is [{ kind: "a", tags: ["x", "y"] }, { kind: "b", tags: [] }]
;items[0].kind($print)
;items[0].tags[1]($print)
;items[1].tags($length)($print)
;items[-1].kind($print)`), lines('a', 'y', '0', 'b'));
});

test('const, var and ref; set with to and compound forms', { skip }, () => {
  assert.equal(out(`
;const c is 1
;var v is 10
;set v += c
;set v *= 2
;set v **= 2
;set v -= 4
;set v to v / 4
;v($print)
;var xs is [1, 2]
;set xs[0] to 5
;set xs[1] <<= 3
;(xs, ", ")($join)($print)
;var r is { n: 1 }
;set r.n += 1
;r.n($print)
;ref alias is r
;set alias.n to 9
;r.n($print)`), lines('120.0', '5, 16', '2', '9'));
});

test('is, to, from and end are names elsewhere', { skip }, () => {
  assert.equal(out(`
;const is is 1
;var to is is + 1
;set to to to + 1
;const from is { end: to }
;from.end($print)
;fn end(x) { x }
;3(end)($print)`), lines('3', '3'));
});

test('negative numbers are literals; -x ** y stays an error', { skip }, () => {
  assert.equal(out(`
;(-2)($print)
;((-2) ** 2)($print)
;(-(2 ** 2))($print)
;(3 - -1)($print)
;(- -1)($print)
;([-1.5, 0xFF, 0b101, 1e3, 2.5e-1, 007], ", ")($join)($print)`), lines('-2', '4', '-4', '4', '1', '-1.5, 255, 5, 1000.0, 0.25, 7'));
  assert.match(run(';(-2 ** 2)($print)\n;exit\n').err, /'-x \*\* y' is ambiguous/);
});

test('if, !if, else if; while and !while; break and continue', { skip }, () => {
  assert.equal(out(`
;fn sign(n) { if (n < 0) { "-" } else if (n == 0) { "0" } else { "+" } }
;((-5)(sign) + 0(sign) + 5(sign))($print)
;!if (false) { "unless"($print) }
;var i is 0
;!while (i == 5) {
    ;set i += 1
    ;if (i == 2) { ;continue }
    ;if (i == 4) { ;break }
    ;i($print)
}`), lines('-0+', 'unless', '1', '3'));
});

test('match with cases, enums and else', { skip }, () => {
  assert.equal(out(`
;enum Color (red, green)
;fn name(c) { match (c) { case Color.red { "R" } case Color.green { "G" } } }
;fn kind(n) {
    match (n) {
        case -1 { "minus one" }
        case 0, 1 { "bit" }
        case "x" { "x" }
        else { "other" }
    }
}
;(Color.red(name) + Color.green(name))($print)
;([(-1)(kind), 1(kind), "x"(kind), 7(kind)], ", ")($join)($print)
;Color.green($typeOf)($print)`), lines('RG', 'minus one, bit, x, other', 'Color'));
});

test('fail, onfail and panic', { skip }, () => {
  assert.equal(out(`
;fn parse(s) {
    ;if (s == "") { ;fail }
    ;if (s == "?") { ;fail 3 }
    ;const n is s($toInt)
        onfail (c) { ;fail c + 10 }
    n
}
;fn code(s) {
    ;const n is s(parse)
        onfail (c) { c }
    n
}
;([""(code), "?"(code), "x"(code), "42"(code)], ", ")($join)($print)`), lines('1, 3, 11, 42'));
  const r = run(';fn f() { ;fail 2 }\n;fn g() { ;(f) }\n;(g)\n;exit\n');
  assert.equal(r.code, 1);
  assert.match(r.err, /uncaught panic: unhandled fail \(code 2\) from 'f' in 'g'/);
  const p = run(';panic "boom"\n;exit\n');
  assert.equal(p.code, 1);
  assert.match(p.err, /^uncaught panic: boom/m);
});

test('exit with a code', { skip }, () => {
  const r = run(';"before"($print)\n;exit 3\n;"after"($print)\n;exit\n');
  assert.deepEqual({ out: r.out, code: r.code }, { out: 'before\n', code: 3 });
});

test('values print with Wavert names', { skip }, () => {
  assert.equal(out(`
;"{true} {false} {null} {nan} {inf} {-inf} {0.0 / 0.0}"($print)
;([1($typeOf), 1.5($typeOf), "s"($typeOf), true($typeOf), null($typeOf), []($typeOf), {}($typeOf)], ", ")($join)($print)
;([12($toString), nan($isNan), 1.5($isNan)], ", ")($join)($print)`),
  lines('true false null nan inf -inf nan', 'int, float, string, boolean, null, array, record', '12, true, false'));
});

test('$toInt and $toFloat read decimal text only', { skip }, () => {
  assert.equal(out(`
;fn int(s) { ;const n is s($toInt) onfail { "no" } n }
;fn float(s) { ;const n is s($toFloat) onfail { "no" } n }
;(["42"(int), "-7"(int), "wah"(int), "wa[3]h"(int), "1.5"(float), "WAAH"(float)], " ")($join)($print)`),
  lines('42 -7 no no 1.5 no'));
});

test('import, export and end', { skip }, () => {
  const files = {
    'lib/shapes.wvt': `;import text from "text.wvt"
;export enum Shape (circle, square)
;export const sides is [0, 4]
;export fn describe(s) { "{s} with {(sides[s == Shape.circle ? 0 : 1], "side")(text.count)}" }
;fn hidden() { 0 }
;end
`,
    'lib/text.wvt': `;export fn count(n, word) { "{n} {word}{n == 1 ? "" : "s"}" }
;end
`,
  };
  assert.equal(out(`;import shapes from "lib/shapes.wvt"
;shapes.Shape.square(shapes.describe)($print)
;shapes.sides[1]($print)
;match (shapes.Shape.circle) { case shapes.Shape.circle { "circle"($print) } case shapes.Shape.square { } }`, { files }),
  lines('square with 4 sides', '4', 'circle'));
});
