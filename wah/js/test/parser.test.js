import { test } from 'node:test';
import assert from 'node:assert/strict';
import { shape, show } from './helpers.js';
import { lex } from '../src/lexer.js';
import { parse } from '../src/parser.js';
import { WahError } from '../src/errors.js';

const parseError = (src) => {
  try {
    parse(lex(src));
  } catch (e) {
    if (e instanceof WahError) return e.diags[0].message;
    throw e;
  }
  assert.fail(`expected a parse error for ${src}`);
};

test('§6.2 postfix calls take every comma-separated expression to their left', () => {
  assert.equal(shape('x, y, z(floorInt)'), 'floorInt(x, y, z)');
  assert.equal(shape('a, (b, c(g))(f)'), 'f(a, g(b, c))');
  assert.equal(shape('x(parse)(validate)(save)'), 'save(validate(parse(x)))');
});

test('§6.2 calls bind loosest', () => {
  assert.equal(shape('wah[a] + x(f)'), 'f((1[a] + x))');
  assert.equal(shape('wah + x(f)'), 'f((1 + x))');
  assert.equal(shape('c ? x : y($print)'), '$print((c ? x : y))');
});

test('§6.2 a call result continues the expression', () => {
  assert.equal(shape('x(f) + wah(g)'), 'g((f(x) + 1))');
  assert.equal(shape('a, b(f), c(g)'), 'g(f(a, b), c)');
});

test('§6.2 zero-argument calls', () => {
  assert.equal(shape(',(getTime)'), 'getTime()');
  assert.equal(shape(', (getTime)'), 'getTime()');
  assert.equal(shape(',(getTime)(format)'), 'format(getTime())');
  assert.equal(shape('a, (,(getTime))(f)'), 'f(a, getTime())');
  assert.match(parseError(';a, ,(getTime)(f)'), /must be parenthesized/);
});

test('§6.2 commas must be consumed by a call', () => {
  assert.match(parseError(';a, b claim x'), /consumed by a call/);
  assert.match(parseError(';print("hi")'), /expected ';' to start a statement, found '\('/);
});

test('§11.4 precedence', () => {
  assert.equal(shape('a + b * c'), '(a + (b * c))');
  assert.equal(shape('a & b == c'), '((a & b) == c)');
  assert.equal(shape('a << wah + b'), '(a << (1 + b))');
  assert.equal(shape('a || b && c'), '(a || (b && c))');
  assert.equal(shape('a ** b ** c'), '(a ** (b ** c))');
  assert.equal(shape('WAH ** waah'), '(-1 ** 2)');
  assert.equal(shape('-(x ** waah)'), '(-(x ** 2))');
  assert.equal(shape('(-x) ** waah'), '((-x) ** 2)');
  assert.equal(shape('a ? b : c ? d : e'), '(a ? b : (c ? d : e))');
  assert.equal(shape('a < b == c > d'), '((a < b) == (c > d))');
  assert.match(parseError(';-x ** waah'), /ambiguous/);
});

test('§10.2 inverted indexing', () => {
  assert.equal(shape('i[arr]'), 'i[arr]');
  assert.equal(shape('j[i[grid]]'), 'j[i[grid]]');
  assert.equal(shape('WAH[arr]'), '-1[arr]');
  assert.equal(shape('[a, b, c]'), '[a, b, c]');
  assert.equal(shape('[a, b(f)]'), '[f(a, b)]');
  assert.equal(shape('[a, (b(f))]'), '[a, f(b)]');
});

test('statements', () => {
  const p = parse(lex(';wah claim x ;wah claim shifty y ;waah cheat y ;wah +cheat y ;++cheat y ;--cheat y ;x cheat i[arr]'));
  assert.deepEqual(p.stmts.map((s) => [s.k, s.shifty ?? s.op ?? null]), [
    ['claim', false], ['claim', true], ['cheat', null], ['cheat', '+'], ['cheat', '+'], ['cheat', '-'], ['cheat', null],
  ]);
  assert.match(parseError(';scheme add(a, b) { a }'), /parameters come before the scheme name/);
  assert.match(parseError('wah claim x'), /expected ';'/);
});

test('§7.2 block values', () => {
  const body = (src) => parse(lex(`;scheme ()f ${src}`)).stmts[0].body;
  assert.equal(body('{ a + b }').value.k, 'bin');
  assert.equal(body('{ ;a + b }').value, null);
  assert.equal(body('{ ;x($print) ; }').value, null);
  assert.equal(body('{ }').value, null);
  assert.equal(body('{ ;x($print) a }').value.k, 'name');
  // '(' that is not `(name)` ends the statement, so a grouped value may follow
  assert.equal(body('{ ;x($print) (a + b) / c }').value.op, '/');
  assert.equal(body('{ ;!doubt (a) { } (a, b(f)) }').value.k, 'call');
  assert.match(parseError(';scheme ()f { a ;b }'), /block's value/);
});

test('§7.1 conditional chains and whine', () => {
  const [s] = parse(lex(';!doubt (a) { } anyway !doubt (b) { } anyway doubt (c) { } anyway { }')).stmts;
  assert.equal(s.e.k, 'chain');
  assert.deepEqual(s.e.branches.map((b) => b.neg), [false, false, true]);
  assert.ok(s.e.els);
  const [w] = parse(lex(';"abc"($toInt) claim n\n    whine (c) { wh }')).stmts;
  assert.equal(w.k, 'claim');
  assert.equal(w.whine.param, 'c');
});

test('§7.5 suspect: parsing', () => {
  const [s] = parse(lex(';suspect (kind.e) { alibi "int", "float" { ;a($print) } alibi wah { } anyway { b } }')).stmts;
  assert.equal(s.e.k, 'suspect');
  assert.equal(shape('suspect (x) { alibi wah { } }'), '<suspect>');
  assert.deepEqual(s.e.alibis.map((a) => a.values.map((v) => v.k)), [['str', 'str'], ['int']]);
  assert.equal(s.e.els.value.name, 'b');
  // a value suspect continues the statement like a conditional chain
  assert.equal(parse(lex(';suspect (x) { alibi wah { "a" } anyway { "b" } } claim r')).stmts[0].k, 'claim');
  assert.match(parseError(';suspect x { alibi wah { } }'), /expected '\(' after 'suspect'/);
  assert.match(parseError(';suspect (x) alibi wah { }'), /expected '\{' after the suspect's subject/);
  assert.match(parseError(';suspect (x) { }'), /a suspect needs at least one alibi/);
  assert.match(parseError(';suspect (x) { anyway { } }'), /needs at least one alibi before 'anyway'/);
  assert.match(parseError(';suspect (x) { alibi wah { } anyway { } alibi waah { } }'), /'anyway' must come last/);
  assert.match(parseError(';suspect (x) { alibi wah { } anyway { } anyway { } }'), /only one 'anyway'/);
  assert.match(parseError(';suspect (x) { alibi { } }'), /an alibi needs at least one value/);
  assert.match(parseError(';suspect (x) { alibi wah, { } }'), /an alibi value must be a literal/);
  assert.match(parseError(';suspect (x) { alibi wa[1.5]h { } }'), /floats cannot be alibi values/);
  assert.match(parseError(';suspect (x) { alibi nobody { } }'), /floats cannot be alibi values/);
  assert.match(parseError(';suspect (x) { alibi "{y}" { } }'), /cannot be interpolated/);
  assert.match(parseError(';suspect (x) { alibi y { } }'), /an alibi value must be a literal/);
  assert.match(parseError(';suspect (x) { alibi [wah] { } }'), /must be a literal/);
  assert.match(parseError(';suspect (x) { alibi wah { } ;y }'), /expected 'alibi', 'anyway' or '\}' in a suspect, found ';'/);
  assert.match(parseError(';alibi wah { }'), /expected an expression, found 'alibi'/);
});

test('§4.1 lineup: parsing', () => {
  const [s] = parse(lex(';lineup (num, str, call)Kind')).stmts;
  assert.equal(s.k, 'lineup');
  assert.equal(s.name, 'Kind');
  assert.deepEqual(s.members.map((m) => m.name), ['num', 'str', 'call']);
  assert.equal(parse(lex(';lineup (only)One')).stmts[0].members.length, 1);
  // members are written member.Name, an ordinary member access
  assert.equal(shape('call.Kind'), 'Kind.call');
  const [v] = parse(lex(';suspect (k) { alibi num.Kind, str.Kind { } alibi wah { } }')).stmts;
  assert.deepEqual(v.e.alibis[0].values.map((x) => x.k), ['mem', 'mem']);
  assert.match(parseError(';lineup Kind (a, b)'), /members come before the lineup name/);
  assert.match(parseError(';lineup ()Kind'), /a lineup needs at least one member/);
  assert.match(parseError(';lineup (a, b)'), /expected the lineup name after its members/);
  assert.match(parseError(';lineup (a b)Kind'), /expected ',' or '\)' in the member list/);
  assert.match(parseError(';lineup (a, a)Kind'), /duplicate member 'a' in lineup/);
  assert.match(parseError(';lineup (a, claim)Kind'), /'claim' is a keyword and cannot be a member name/);
  assert.match(parseError(';lineup (a, wah)Kind'), /expected a member name, found 'wah'/);
  assert.match(parseError(';lineup (a, "b")Kind'), /expected a member name, found a string/);
  assert.match(parseError(';lineup (a,)Kind'), /expected a member name, found '\)'/);
  assert.match(parseError(';scheme ()f { ;lineup (a)K }'), /a lineup can only be declared at the top level/);
  assert.match(parseError(';!doubt (real) { ;lineup (a)K }'), /only be declared at the top level/);
  assert.match(parseError(';lineup (a)K whine { }'), /'whine' must follow/);
  assert.match(parseError(';lineup claim x'), /expected '\(' to open the member list/);
  // only member.Name or member.Name.module can be an alibi value; the checker decides whether Name is a lineup
  assert.match(parseError(';suspect (k) { alibi a.b.c.d { } }'), /an alibi value must be a literal or a lineup member/);
  assert.match(parseError(';suspect (k) { alibi a.(x(f)) { } }'), /must be a literal or a lineup member/);
});

test('§13.1 calls to a stolen scheme: (name.module)', () => {
  assert.equal(shape('x, y(f.m)'), 'm.f(x, y)');
  assert.equal(shape(',(f.m)'), 'm.f()');
  assert.equal(shape('x(f.m)(g)($print)'), '$print(g(m.f(x)))');
  assert.equal(shape('x(f.m) + wah'), '(m.f(x) + 1)');
  // after an expression, (name.module) is a call, like (name); other groupings end the expression
  const [s] = parse(lex(';!doubt (real) { ;x($print) (kind.node) }')).stmts;
  assert.equal(s.e.branches[0].body.value, null);
  assert.equal(show(s.e.branches[0].body.stmts[0].e), 'node.kind($print(x))');
  assert.equal(parse(lex(';!doubt (real) { ;x($print) (kind.node.r) }')).stmts[0].e.branches[0].body.value.k, 'mem');
  assert.match(parseError(';x(f.$print)'), /expected/);
});

test('§8.2 WAH! in call position', () => {
  assert.equal(shape('c(WAH!)'), 'WAH!(c)');
  assert.match(parseError(';c(WAAAH!)'), /exact spelling WAH!/);
  assert.match(parseError(';x + WAH!'), /is a statement/);
});

test('§2.8 parser lints', () => {
  const w = [];
  parse(lex(';a!=b ;doubt (a != b) { }', w), w);
  assert.deepEqual(w.map((d) => d.message),
    ["missing whitespace around '!='", 'this statement has no effect', "'doubt (a != b)' is a double negative"]);
});

test('§2.8 statements with no effect', () => {
  const lint = (src) => {
    const w = [];
    parse(lex(src, w), w);
    return w.map((d) => `${d.message}${d.hint ? ` | ${d.hint}` : ''}`);
  };
  for (const src of [';n', ';a + b', ';kind.node', ';wah[a]', ';-x', ';a ? b : c', ';[a, b]', ';{ k: v }', ';"s"', ';$args', ';real']) {
    assert.deepEqual(lint(src), ['this statement has no effect'], src);
  }
  for (const src of [';x($print)', ';,(f)', ';c(WAH!)', ';[a(f)]', ';"{x(f)}"', ';{ k: x(f) }', ';!doubt (a) { }', ';a ? (b(f)) : c',
    ';wah claim x', ';x cheat y', ';getaway x', ';tantrum x', ';x !invited', ';', ';bail']) {
    assert.deepEqual(lint(src), [], src);
  }
  assert.deepEqual(lint(';scheme ()f { ;n }'), ["this statement has no effect | to make it the block's value, remove the ';' before it"]);
});

test('§7.2 hints for a block value swallowed by the statement before it', () => {
  const err = (src) => {
    try {
      parse(lex(src));
    } catch (e) {
      if (e instanceof WahError) return `${e.diags[0].message} | ${e.diags[0].hint}`;
      throw e;
    }
    return null;
  };
  assert.match(err(';scheme ()f { ;!doubt (c) { }\n    ,(g) }'), /consumed by a call \| a block's value that starts with ',' continues .*\(,\(f\)\)/);
  assert.match(err(';scheme ()f { ;x($print)\n    ,(g) }'), /a block's value that starts with ','/);
  // the same comma without a line break, or at the top level, keeps the usual hint
  assert.match(err(';scheme ()f { ;x($print) ,(g) }'), /\| ;a, b\(f\) calls f with a and b/);
  assert.match(err(';x($print)\n, (g)'), /\| ;a, b\(f\) calls f with a and b/);
  const lint = (src) => {
    const w = [];
    parse(lex(src, w), w);
    return w.map((d) => d.message);
  };
  assert.deepEqual(lint(';scheme ()f { ;wah claim shifty x ;wah cheat x\n    [x] }'), ["'[' at the start of a line indexes the expression before it"]);
  assert.deepEqual(lint(';scheme ()f { ;wah claim shifty x ;wah cheat x [x] }'), []);
  assert.deepEqual(lint(';wah cheat x\n[a]'), []); // top level: no block value to swallow
});
