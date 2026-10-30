// The wahc checker must agree with wahc0's: the same errors, in the same order.
// (codegen.test.js also compares every snippet in wahc0's tests, checker included.)

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { STDLIB } from '../../js/src/stdlib.js';
import { agree, readRepo } from './helpers.js';

const exit = (body) => `${body}\n;!invited`;

test('the stdlib table matches js/src/stdlib.js', () => {
  // wahc keeps its own copy; probe every name with too few and too many arguments.
  for (const [name, sig] of STDLIB) {
    if (sig === 'const') {
      agree(exit(`;$${name}($print) ;$${name}`));
      agree(exit(`;wah($${name})`));
      continue;
    }
    const args = (n) => (n === 0 ? ',' : Array.from({ length: n }, () => 'wah').join(', '));
    agree(exit(`;${args(sig)}($${name})`));
    agree(exit(`;${args(sig + 1)}($${name})`));
    if (sig > 0) agree(exit(`;${args(sig - 1)}($${name})`));
    agree(exit(`;$${name}`));
  }
  const listed = readRepo('wahc/checker.wah').match(/\{ name: "\w+", arity: [^}]+\}/g).length;
  assert.equal(listed, STDLIB.size);
  agree(exit(';$nope ;wah($nope) ;$Print'));
});

test('names, scopes and shadowing', () => {
  for (const p of [
    ';x($print)', ';wah claim x ;wah claim x', ';scheme ()f { } ;scheme ()f { }', ';wah claim f ;scheme ()f { }',
    ';wah claim x ;scheme (x)f { }', ';scheme (x)f { } ;wah claim x', ';scheme (a, a)f { }',
    ';scheme (n)f { ;wah claim n }', ';scheme ()f { ;scheme ()f { } }', ';scheme ()f { ;scheme ()g { } } ;scheme ()g { }',
    ';!doubt (real) { ;wah claim x } anyway { ;wah claim x } ;wah claim x', ';!doubt (real) { ;wah claim x ;!doubt (real) { ;wah claim x } }',
    ';lurk (fake) { ;wah claim x } ;x($print)', ';x($print) ;wah claim x', ';x claim x', ';scheme ()f { x } ;wah claim x ;,(f)',
    ';scheme ()f { ;x($print) ;wah claim x }', ';"1"($toInt) claim n whine { n }', ';"1"($toInt) claim n whine (c) { c } ;c($print)',
    ';"1"($toInt) claim c whine (c) { c }', ';wah claim c ;"1"($toInt) claim n whine (c) { c }',
    ';scheme (c)f { ;"1"($toInt) claim n whine (c) { c } }', ';"1"($toInt) claim n whine (c) { ;wah claim c }',
    ';scheme ()f { ;wah claim x ;scheme ()g { x } }', ';scheme ()f { ;scheme ()g { x } ;wah claim x }',
    ';{ x: wah } claim r ;x.r($print) ;y.r($print)', ';scheme ()f { } ;f($print)', ';scheme ()f { } ;[f, wah]',
    ';scheme ()f { } ;f + wah', ';scheme ()f { } ;wah[f]', ';scheme ()x { } ;scheme ()f { ;wah claim x ;scheme ()g { ;wah claim x } }', ';wah\n;wah claim x\n\n;wah claim x',
  ]) {
    agree(exit(p));
  }
});

test('cheat targets', () => {
  for (const p of [
    ';wah claim x ;waah cheat x', ';wah claim shifty x ;waah cheat x', ';scheme ()f { } ;wah cheat f', ';wah cheat y',
    ';scheme (x)f { ;wah cheat x }', ';scheme (shifty x)f { ;wah cheat x }', ';"1"($toInt) claim n whine (c) { ;wah cheat c }',
    ';[wah] claim a ;wah cheat wh[a]', ';[wah] claim shifty a ;wah cheat wh[a]', ';{ n: wah } claim r ;waah cheat n.r',
    ';{ n: wah } claim shifty r ;waah +cheat n.r', ';scheme ()f { } ;wah cheat wh[f]', ';wah cheat wh[q]',
    ';[[wah]] claim a ;wah cheat wh[wh[a]]', ';{ n: { m: wah } } claim r ;wah cheat m.n.r', ';wah cheat wh[$args]',
    ';wah cheat y[x]', ';wah claim x ;x +cheat x', ';++cheat z', ';scheme (x)f { ;++cheat x }',
  ]) {
    agree(exit(p));
  }
});

test('calls and statements in the wrong place', () => {
  for (const p of [
    ';wah(f)', ';wah claim f ;wah(f)', ';scheme (a)f { } ;,(f)', ';scheme (a)f { } ;wah, waah(f)', ';scheme ()f { } ;wah(f)',
    ';"x"($print)($print)', ';,($print)', ';,($args)', ';getaway', ';getaway wah', ';scheme ()f { ;getaway }', ';bail', ';loiter',
    ';lurk (real) { ;bail ;loiter }', ';lurk (real) { ;scheme ()f { ;bail } }', ';scheme ()f { ;lurk (real) { ;bail } }',
    ';WAH!', ';scheme ()f { ;WAH! }', ';wah(WAH!)', ';scheme ()f { ;wah(WAH!) }', ';"x"($print) whine { }',
    ';wah claim x whine { }', ';wah(x) claim y whine { }', ';scheme (a)f { a } ;wah(f) + wah claim y whine { }',
    ';!doubt (real) { wah } claim x', ';!doubt (real) { wah } anyway !doubt (fake) { waah } claim x',
    ';!doubt (real) { } anyway doubt (fake) { }', ';[!doubt (real) { wah }]', ';!doubt (q) { ;r($print) } anyway { s }',
    ';!lurk (q) { ;tantrum r }', ';"{q} {wah(f)}"($print)', ';{ a: q, b: wah(f) } claim r', ';-q + ~r ** s',
    ';q ? r : s', ';q[r]', ';"k"[q]',
  ]) {
    agree(exit(p));
  }
});

test('lineups and suspects over their members', () => {
  const kind = ';lineup (num, str, call, chain)Kind ;lineup (call)Op ;call.Kind claim k ';
  for (const p of [
    ';chain.Kind($print) ;lineup (call, chain)Kind', ';cal.Kind($print)', ';Kind($print)', ';wah[Kind]($print)', ';wah(Kind)',
    ';wah cheat Kind', ';wah cheat wh[Kind]', ';num.Kind cheat call.Kind', ';num.Kind cheat cal.Kind', ';wah claim Kind',
    ';lineup (a)Kind', ';scheme ()f { ;wah claim Kind }', ';scheme (Kind)f { }', ';wah claim num ;num.Kind claim n',
    ';{ num: wah } claim r ;num.r($print) ;num.Op($print) ;num.Nope($print)',
    ';suspect (k) { alibi num.Kind, str.Kind { } alibi call.Kind { } alibi chain.Kind { } }',
    ';suspect (k) { alibi call.Kind { } }', ';suspect (k) { alibi call.Kind { } anyway { } }',
    ';suspect (k) { alibi num.Kind, str.Kind { "lit" } alibi call.Kind, chain.Kind { "other" } } claim x',
    ';suspect (k) { alibi num.Kind { "lit" } } claim x',
    ';suspect (k) { alibi num.Kind, str.Kind, call.Kind { "a" } alibi chain.Kind { } } claim x',
    ';suspect (k) { alibi call.Kind { } alibi "num" { } }', ';suspect (k) { alibi wah { } alibi call.Kind, real { } }',
    ';suspect (k) { alibi call.Kind { } alibi call.Op, chain.Kind { } }', ';suspect (k) { alibi call.Op { } }',
    ';suspect (k) { alibi call.Kind { } alibi chain.Kind, call.Kind { } anyway { } }',
    ';suspect (k) { alibi cal.Kind { } alibi call.Knid { } alibi call.k { } }',
    ';suspect (k) { alibi cal.Kind, call.Knid, chain.Kind, str.Op { } }',
  ]) {
    agree(exit(kind + p));
  }
});

test('the entry file must end with ;!invited', () => {
  for (const p of ['', ';', ';wah($print)', ';!invited ;wah($print)', ';!invited ;', ';!invited ;;', ';waah !invited',
    ';scheme ()f { ;!invited }', ';!invited // done', '// nothing', ';scheme ()f { }']) {
    agree(p);
  }
});

test('many errors are all reported, in order', () => {
  agree([
    ';wah claim x',
    ';scheme (x)f { ;y cheat x ;wah(g) ;getaway ;bail }',
    ';x, x($print) ;$nope ;$args($print) ;$print',
    ';wah claim x ;q ;WAH! ;wah(WAH!) ;!doubt (real) { wah } claim z',
  ].join('\n'));
});
