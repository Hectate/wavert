// The wahc parser must agree with wahc0's: the same AST, warnings and errors.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { lex } from '../../js/src/lexer.js';
import { parse } from '../../js/src/parser.js';
import { Source, WahError, formatDiag } from '../../js/src/errors.js';
import { codePoints, exampleLibraries, examples, fmtFloat, harvest, readRepo, runWahc, wahcFiles } from './helpers.js';

const EXPR = new Set(['int', 'float', 'str', 'bool', 'null', 'nan', 'inf', 'name', 'std', 'arr', 'rec', 'idx', 'mem',
  'un', 'bin', 'tern', 'call', 'sigcall', 'chain', 'suspect']);
const STMT = new Set(['empty', 'scheme', 'lineup', 'steal', 'bribe', 'getaway', 'tantrum', 'bail', 'loiter', 'lurk', 'signal', 'exit', 'expr',
  'claim', 'cheat']);
const POS = new Set(['pos', 'end', 'namePos', 'paramPos']);
// Keywords cannot be field names in Γ-- (§10.7), so wahc renames these.
const RENAME = { shifty: 'isShifty', fickle: 'isFickle', whine: 'handler' };

// wahc's --ast format, from wahc0's AST: wahc always has paren on expressions and
// handler on statements, where wahc0 adds them only when they apply.
function dump(x, cp, indent = '') {
  const inner = indent + '  ';
  if (typeof x === 'string') return JSON.stringify(x);
  if (x === null) return 'uninvited';
  if (typeof x === 'boolean') return x ? 'real' : 'fake';
  if (typeof x === 'number') return String(x);
  if (Array.isArray(x)) {
    if (x.length === 0) return '[]';
    return `[${x.map((v) => `\n${inner}${dump(v, cp, inner)}`).join('')}\n${indent}]`;
  }
  const o = {};
  for (const [k, v] of Object.entries(x)) {
    o[RENAME[k] ?? k] = POS.has(k) && typeof v === 'number' ? cp(v) : k === 'v' && x.k === 'float' ? { float: v } : v;
  }
  if (EXPR.has(x.k)) o.paren ??= false;
  if (STMT.has(x.k)) o.handler ??= null;
  return `{${Object.keys(o).sort().map((k) => {
    const v = o[k];
    return `\n${inner}${k}: ${v && v.float !== undefined ? fmtFloat(v.float) : dump(v, cp, inner)}`;
  }).join('')}\n${indent}}`;
}

function expected(text, file) {
  const src = new Source(file, text.replace(/^﻿/, ''));
  const cp = codePoints(src.text);
  const warnings = [];
  let ast = null, error = null;
  try {
    ast = parse(lex(src.text, warnings), warnings);
  } catch (e) {
    if (!(e instanceof WahError)) throw e;
    error = e.diags[0];
  }
  const diags = error ? [...warnings, error] : warnings;
  return {
    out: ast ? dump(ast, cp) + '\n' : '',
    err: diags.map((d) => formatDiag(d, src) + '\n').join(''),
    code: error ? 2 : 0,
  };
}

function agree(text, file = 'test.wah') {
  assert.deepEqual(runWahc(['--ast', file], { [file]: text }), expected(text, file), JSON.stringify(text));
}

for (const file of [...examples(), ...exampleLibraries(), ...wahcFiles()]) {
  test(`agrees with wahc0 on ${file}`, () => agree(readRepo(file), file));
}

test('agrees on every snippet in the wahc0 tests', () => {
  const snippets = harvest();
  assert.ok(snippets.length > 200, `only ${snippets.length} snippets found`);
  for (const s of snippets) agree(s);
});

test('agrees on statements', () => {
  for (const s of [';(x)', 'x', ';print("hi")', ';', ';;', ';x;;', ';scheme add(a, b) { a }', ';scheme x', ';scheme (a b)f { }',
    ';scheme (a,)f { }', ';scheme (a)', ';scheme (a) {', ';scheme ()f', ';scheme (shifty a, b)f { a }', ';scheme ()f {',
    ';scheme ()f { a ;b }', ';scheme ()f { a b }', ';scheme ()f { a: b }', ';scheme ()f { a (b + c) }', ';scheme ()f { ;x }',
    ';lurk x { }', ';lurk (x { }', ';lurk (x) ;', ';!lurk (x) { ;bail ;loiter }', ';whine', ';anyway', ';x whine', ';x whine (',
    ';x whine (c', ';x whine (wah) { }', ';x(f) whine (c) { wh }', ';x(f) claim y whine { wh }', ';scheme ()f { } whine { }',
    ';bail whine { }', ';+cheat x', ';++cheat x', ';--cheat i[a]', ';wah ++cheat x', ';wah cheat wah', ';wah cheat (x)',
    ';wah cheat x(f)', ';x shifty', ';wah claim', ';wah claim shifty', ';wah claim shifty x', ';wah +cheat count.stats',
    ';x cheat k[node]', ';getaway', ';getaway x', ';getaway ;', ';getaway }', ';tantrum', ';tantrum x', ';WAH!', ';WAAAH!',
    ';!invited', ';x !invited', ';x(f) !invited whine { wh }']) {
    agree(s);
  }
});

test('agrees on expressions', () => {
  for (const s of [';a, b claim x', ';a, ,(f)(g)', ';[a,]', ';[a, b]', ';[]', ';[,(f)]', ';,x', ';, (f)', ';,(f)(g) + wah',
    ';c(WAAAH!)', ';a, b(WAH!)', ';c(WAH!)', ';x + WAH!', ';claim x', ';()', ';(a', ';(a, b)', ';x[y', ';-x ** waah',
    ';-(x ** waah)', ';(-x) ** waah', ';- -x', ';!~x', ';a ? b', ';a ? b : c ? d : e', ';a || b && c | d ^ e & f == g < h << i + j * k',
    ';"kind"[node]', ';("kind")[node]', ';"a b"[n]', ';"claim"[n]', ';"{x}"[n]', ';kind.node', ';wah.node', ';claim.node',
    ';"s".node', ';x[y].z', ';a.b.c', ';kind.i[items]', ';(kind.i)[items]', ';kind.(x(parse))', ';wh[args.node]',
    ';x(f) + wah(g)', ';(a + b) / c', ';a, b(f), c(g)', ';x($print)', ';$args', ';nobody everybody uninvited real fake',
    ';wa[0.5]h + WA[1e3]H', ';{}', ';{ a: wah, }', ';{ a }', ';{ a, b }', ';{ a wah }', ';{ claim: wah }', ';{ wah: wah }',
    ';{ a: wah, a: waah }', ';{ a: wah b: waah }', ';{ a: x(f), b: (y, z(g)) }', ';{ a: ,(f), b: c ? d : e }',
    ';{ a: x(f)(g) + wah }', ';"{x y}"', ';"{x}" + "{(y}"', ';"a{b}c{d(f)}e"', ';"{"{x}"}"', ';doubt (a != b) { }',
    ';doubt (a == b) { }', ';!doubt (a) { } anyway', ';!doubt (a) { } anyway x', ';!doubt (a) { b } anyway { c } claim r',
    ';!doubt (a) { } anyway doubt (b) { } anyway !doubt (c) { } anyway { }', ';a!=b', ';a ?b : c', ';a ? b :c', ';a**b',
    ';a ** -b', ';x[y][z]', ';x[y]($f)']) {
    agree(s);
  }
});
