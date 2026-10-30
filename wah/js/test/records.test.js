import { test } from 'node:test';
import assert from 'node:assert/strict';
import { errors, out, runWah, shape } from './helpers.js';
import { lex } from '../src/lexer.js';
import { parse } from '../src/parser.js';
import { WahError } from '../src/errors.js';

const lines = (...xs) => xs.map((x) => `${x}\n`).join('');
const compileError = (src) => {
  try {
    parse(lex(src));
  } catch (e) {
    if (e instanceof WahError) return e.diags[0].message + (e.diags[0].hint ? ` | ${e.diags[0].hint}` : '');
    throw e;
  }
  assert.fail(`expected a compile error for ${src}`);
};
const tantrum = (body) => {
  const r = runWah(`${body}\n;!invited`);
  if (r.compileErrors) throw new Error(`compile error: ${r.compileErrors.join('; ')}`);
  assert.equal(r.code, 1, `expected a tantrum, got stdout ${JSON.stringify(r.out)}`);
  return r.err.split('\n')[1].slice('tantrum: '.length);
};
const one = (body, re) => {
  const errs = errors(body);
  assert.ok(errs.some((e) => re.test(e)), `expected ${re} in ${JSON.stringify(errs)}`);
};
const node = ';{ kind: "num", value: wa[42]h } claim node';

test('§10.7 record literals', () => {
  assert.equal(out(`${node} ;kind.node($print) ;value.node($print)`), lines('num', 42));
  assert.equal(out(';{}($typeOf)($print) ;{}($fields)($length)($print)'), lines('record', 0));
  // a ternary inside a field value keeps its own ':'
  assert.equal(out(';{ k: real ? "t" : "f", n: wah } claim r ;k.r($print)'), 't\n');
  // records as block values need their own braces
  assert.equal(out(';scheme (n)leaf { { kind: "num", value: n } } ;kind.(waah(leaf))($print)'), 'num\n');
  assert.equal(out(';!doubt (fake) { { kind: "some" } } anyway { { kind: "none" } } claim r ;kind.r($print)'), 'none\n');
  // ;getaway takes a record
  assert.equal(out(';scheme ()mk { ;getaway { a: wah } } ;a.(,(mk))($print)'), '1\n');
});

test('§10.7 call scope: each field value is its own nesting level', () => {
  assert.equal(out(';{ a: wah, b: "xyz"($length) } claim r ;b.r($print) ;a.r($print)'), lines(3, 1));
  assert.equal(out(';{ b: ("a,b", ","($split))($length) } claim r ;b.r($print)'), '2\n');
  assert.equal(out(';{ b: (("a-b", "-"($split)), "+"($join)) } claim r ;b.r($print)'), 'a+b\n');
  // a top-level ',' in a value ends the field
  assert.match(compileError(';{ b: ("a-b", "-"($split)), "+"($join) } claim r'), /expected a field name/);
  assert.equal(out(';{ t: ,($readLine) } claim r ;t.r($print)', { stdin: ['line'] }), 'line\n');
});

test('§10.7 literal errors', () => {
  assert.match(compileError(';{ a: wah, a: waah } claim r'), /duplicate field 'a'/);
  assert.match(compileError(';{ kind } claim r'), /there is no shorthand; write kind: kind/);
  assert.match(compileError(';{ a: wah, } claim r'), /trailing comma in record literal/);
  assert.match(compileError(';{ claim: wah } claim r'), /'claim' is a keyword and cannot be a field name/);
  assert.match(compileError(';{ wah: wah } claim r'), /expected a field name/);
  assert.match(compileError(';{ "a": wah } claim r'), /expected a field name/);
  assert.match(compileError(';scheme (n)leaf { kind: n }'), /needs its own braces/);
});

test('§10.8 member access is inverted and right-associative', () => {
  assert.equal(shape('kind.node'), 'node.kind');
  assert.equal(shape('name.callee.node'), 'node.callee.name');
  assert.equal(shape('kind.i[items]'), 'i[items].kind');
  assert.equal(shape('(kind.i)[items]'), 'i.kind[items]');
  assert.equal(shape('kind.(x(parse))'), 'parse(x).kind');
  assert.equal(shape('kind.x(parse)'), 'parse(x.kind)');
  assert.equal(shape('{ a: x, b: y(f) }'), '{a: x, b: f(y)}');
  const prelude = ';{ name: "f" } claim callee ;{ callee: callee, args: [{ kind: "a" }, { kind: "b" }] } claim call ;wah claim i';
  assert.equal(out(`${prelude} ;name.callee.call($print)`), 'f\n');
  assert.equal(out(`${prelude} ;kind.wh[args.call]($print)`), 'a\n');
  assert.equal(out(`${prelude} ;kind.i[args.call]($print)`), 'b\n');
  assert.equal(out(';{ i: wah } claim k ;["x", "y"] claim items ;(i.k)[items]($print)'), 'y\n');
  assert.equal(out(`${node} ;-value.node($print) ;value.node + wah($print)`), lines(-42, 43));
  // calls bind loosest: kind.x(parse) is parse(x.kind)
  assert.equal(out(';{ s: "abc" } claim x ;s.x($length)($print)'), '3\n');
});

test('§10.8 dot syntax errors', () => {
  assert.match(compileError(';kind . node'), /no whitespace is allowed around '\.'/);
  assert.match(compileError(';kind. node'), /no whitespace is allowed around '\.'/);
  assert.match(compileError(';kind .node'), /no whitespace is allowed around '\.'/);
  assert.match(compileError(';wh.node'), /expected a field name before '\.', found 'wh'/);
  assert.match(compileError(';(a).b'), /left side of '\.' must be a field name/);
  assert.match(compileError(';real.x'), /'real' is a keyword/);
});

test('§10.8 field labels are not bindings', () => {
  // a binding and a field may share a name; kind.node never reads the binding
  assert.equal(out(`${node} ;"value" claim kind ;kind.node($print) ;kind[node]($print)`), lines('num', 42));
  // an unknown label is not a compile error; the record is checked at runtime
  assert.deepEqual(errors(`${node} ;knid.node($print)`), []);
});

test('§10.8 runtime errors', () => {
  assert.match(tantrum(`${node} ;knid.node($print)`), /no field 'knid' in record/);
  assert.match(tantrum(';[wah] claim a ;kind.a($print)'), /member access needs a record, got array/);
  assert.match(tantrum(`${node} ;wh[node]($print)`), /a record key must be a purple, got int/);
  assert.match(tantrum(`${node} ;"a b"[node]($print)`), /no field 'a b' in record/);
  assert.match(tantrum(`${node} ;""[node]($print)`), /no field '' in record/);
});

test('§10.8 computed keys cannot reach JS internals', () => {
  for (const k of ['__proto__', 'constructor', 'toString', 'hasOwnProperty', 'f_kind']) {
    assert.match(tantrum(`${node} ;"${k}" claim k ;k[node]($print)`), new RegExp(`no field '${k}' in record`));
  }
  assert.match(tantrum(';{} claim shifty r ;"__proto__" claim k ;wah cheat k[r]'), /no field '__proto__' in record/);
});

test('§10.8 computed access', () => {
  assert.equal(out(`${node} ;node($fields) claim fs ;wh claim shifty i ;lurk (i < (fs($length))) { ;"{i[fs]}={i[fs][node]}"($print) ;++cheat i }`),
    lines('kind=num', 'value=42'));
});

test('§2.8 string-literal key lint', () => {
  const w = runWah(`${node} ;"kind"[node]($print) ;!invited`).warnings;
  assert.ok(w.some((m) => /write kind\.record instead of "kind"\[record\]/.test(m)), JSON.stringify(w));
  assert.deepEqual(runWah(`${node} ;"kind" claim k ;k[node]($print) ;!invited`).warnings, []);
});

test('§10.9 mutation', () => {
  assert.equal(out(`${node} ;node claim shifty m ;wa[7]h cheat value.m ;wah +cheat value.m ;++cheat value.m ;"str" cheat "kind"[m]
;value.m($print) ;kind.m($print) ;value.node($print)`), lines(9, 'str', 42));
  // compound computed target
  assert.equal(out(';{ n: wah } claim shifty r ;"n" claim k ;waah *cheat k[r] ;n.r($print)'), '2\n');
  // a field's value may change type
  assert.equal(out(';{ n: wah } claim shifty r ;"one" cheat n.r ;n.r($print)'), 'one\n');
});

test('§10.9 mutation errors', () => {
  one(`${node} ;wah cheat value.node`, /cannot modify a field of 'node': it is not shifty/);
  one(`${node} ;"value" claim k ;wah cheat k[node]`, /cannot modify an element of 'node': it is not shifty/);
  assert.match(tantrum(';{ n: wah } claim shifty r ;wah cheat m.r'), /no field 'm' in record/);
  // the container that directly holds the target must be shifty; the outer one need not be
  assert.match(tantrum(';{ inner: { n: wah } } claim shifty r ;waah cheat n.inner.r'), /cannot modify a field of a non-shifty record/);
  assert.equal(out(';{ n: wah } claim shifty inner ;{ inner: inner } claim r ;waah cheat n.inner.r ;n.inner($print)'), '2\n');
  assert.equal(out(';{ n: wah } claim shifty inner ;[inner] claim items ;waah cheat n.wh[items] ;n.inner($print)'), '2\n');
});

test('§10.9 copy table', () => {
  // frozen -> claim shifty: copy, made mutable
  assert.equal(out(`${node} ;node claim shifty m ;wh cheat value.m ;value.node($print) ;m == node($print)`), lines(42, 'fake'));
  // shifty -> claim: frozen snapshot
  assert.equal(out(';{ n: wah } claim shifty m ;m claim snap ;waah cheat n.m ;n.snap($print)'), '1\n');
  // shifty -> claim shifty: alias, including an empty record
  assert.equal(out(';{ n: wah } claim shifty m ;m claim shifty a ;waah cheat n.a ;n.m($print) ;m == a($print)'), lines(2, 'real'));
  assert.equal(out(';{} claim shifty e ;e claim shifty e2 ;e == e2($print)'), 'real\n');
  // frozen -> claim: share
  assert.equal(out(`${node} ;node claim same ;same == node($print)`), 'real\n');
  // parameters follow the same table
  assert.equal(out(`
;scheme (shifty r)bump { ;++cheat n.r }
;scheme (r)peek { n.r }
;{ n: wh } claim shifty c
;c(bump) ;c(bump)
;c(peek)($print)
;{ n: wh } claim frozenOne
;frozenOne(bump)
;n.frozenOne($print)`), lines(2, 0));
});

test('§10.10 equality and stdlib', () => {
  assert.equal(out(`${node} ;{ kind: "num", value: wa[42]h } claim twin
;node == twin($print) ;node == node($print) ;node, twin($same)($print)
;{ value: wa[42]h, kind: "num" }, node($same)($print)
;{ kind: "num" }, node($same)($print)
;{ a: [{ b: wah }] }, { a: [{ b: wah }] }($same)($print)
;node == wah($print)`), lines('fake', 'real', 'real', 'real', 'fake', 'real', 'fake'));
  assert.equal(out(';{ z: wah, a: waah }($fields), ", "($join)($print)'), 'z, a\n');
  assert.match(tantrum(`${node} ;node < node($print)`), /cannot order record values/);
  assert.match(tantrum(`${node} ;node($print)`), /cannot convert record to text/);
  assert.match(tantrum(`${node} ;node($length)($print)`), /got record/);
  assert.match(tantrum(';[wah]($fields)($print)'), /\$fields expects a record, got array/);
  assert.match(tantrum(`${node} ;tantrum node`), /\{kind: "num", value: 42\}/);
});

test('§2.6 an interpolation may start with a record literal', () => {
  assert.equal(out(';"{ {a: wah, b: waah}($fields)($length) } fields"($print)'), '2 fields\n');
});
