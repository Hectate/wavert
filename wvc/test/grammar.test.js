// Keeps the VS Code grammar (editors/vscode) in sync with the compiler: its
// word lists must match the lexer's keywords and the checker's stdlib table,
// and its regexes must classify the lexer's token shapes the same way. Zero
// dependencies, so this checks rule by rule rather than running the TextMate
// engine.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { noZig, readRepo, wvc, writeFiles } from './helpers.js';

const grammar = JSON.parse(readRepo('editors/vscode/syntaxes/wavert.tmLanguage.json'));
const repo = grammar.repository;
const rules = (name) => repo[name].patterns ?? [repo[name]];

// The compiler's own lists, read from its source.
const lexer = readRepo('wvc/lexer.wvt');
const list = (name) => JSON.parse(new RegExp(`;(?:export )?const ${name} is (\\[[^\\]]*\\])`).exec(lexer)[1]);
const KEYWORDS = list('keywords');
const BANG = list('bangWords');
const SET_OPS = list('setOps');
const RESERVED = [...lexer.matchAll(/\{ word: "(\w+)"/g)].map((m) => m[1]);
const STDLIB = new Map([...readRepo('wvc/checker.wvt').matchAll(/\{ name: "(\w+)", arity: ("const"|\d+) \}/g)]
  .map((m) => [m[1], m[2] === '"const"' ? 'const' : Number(m[2])]));

// The scope of the first rule in `name` that matches all of `text`, or null.
function scopeOf(name, text) {
  for (const r of rules(name)) {
    if (r.match && new RegExp(`^(?:${r.match})$`).test(text)) return r.name.replace(/\.wavert$/, '');
  }
  return null;
}
// Lowercase words named in a rule group's regexes.
const wordsIn = (...names) => new Set(names.flatMap((n) => rules(n).flatMap((r) => r.match.replace(/\\b/g, ' ').match(/[a-z]{2,}/g) ?? [])));

test('the compiler lists were found', () => {
  assert.ok(KEYWORDS.includes('fn') && BANG.includes('if') && SET_OPS.includes('+=') && RESERVED.includes('recover'));
  assert.equal(STDLIB.get('print'), 1);
  assert.equal(STDLIB.get('args'), 'const');
});

test('every grammar regex compiles', () => {
  const walk = (node) => {
    if (Array.isArray(node)) return node.forEach(walk);
    if (!node || typeof node !== 'object') return;
    for (const k of ['match', 'begin', 'end']) if (typeof node[k] === 'string') assert.doesNotThrow(() => new RegExp(node[k]), node[k]);
    Object.values(node).forEach(walk);
  };
  walk(grammar);
});

test('keywords and reserved words match the lexer', () => {
  for (const w of KEYWORDS) assert.ok(scopeOf('keywords', w) ?? scopeOf('constants', w), `keyword '${w}' is not highlighted`);
  for (const w of RESERVED) assert.equal(scopeOf('keywords', w), 'invalid.illegal.reserved', w);
  for (const w of wordsIn('keywords', 'constants')) {
    assert.ok(KEYWORDS.includes(w) || RESERVED.includes(w), `the grammar highlights '${w}', which the lexer does not reserve`);
  }
  for (const w of BANG) assert.ok(scopeOf('bang-keywords', `!${w}`), `'!${w}' is not highlighted`);
  for (const op of SET_OPS) assert.equal(scopeOf('operators', op), 'keyword.operator.assignment.compound', op);
  // The contextual words are keywords only in their statements.
  for (const w of ['is', 'to', 'from', 'end']) assert.equal(scopeOf('keywords', w), null, w);
  assert.ok(new RegExp(repo.declaration.match).test('const x is 1'));
  assert.ok(new RegExp(repo.import.match).test('import lib from "lib.wvt"'));
  assert.ok(new RegExp(repo['end-marker'].match).test(';end'));
  assert.ok(!new RegExp(repo['end-marker'].match).test(';end.x'));
});

test('builtins match the stdlib table', () => {
  for (const [name, sig] of STDLIB) {
    const want = sig === 'const' ? 'support.constant.builtin' : 'support.function.builtin';
    assert.equal(scopeOf('stdlib', `$${name}`), want, `$${name}`);
  }
  const listed = rules('stdlib').filter((r) => r.name.startsWith('support.'))
    .flatMap((r) => r.match.match(/\(\?:([^)]*)\)/)[1].split('|'));
  for (const name of listed) assert.ok(STDLIB.has(name), `the grammar lists $${name}, which the stdlib does not define`);
  assert.equal(scopeOf('stdlib', '$toPurple'), 'invalid.illegal.unknown-builtin');
});

test('numbers match the lexer', { skip: noZig }, () => {
  const good = ['0', '42', '007', '0xFF', '0b101', '1.5', '1e9', '2.5e-3', '6E+2'];
  const bad = ['12ab', '0x', '1.', '1.5.3', '0b2'];
  const dir = writeFiles(Object.fromEntries([...good, ...bad].map((n, i) => [`n${i}.wvt`, `;${n}\n`])));
  [...good, ...bad].forEach((n, i) => {
    const r = wvc(['--tokens', `n${i}.wvt`], { cwd: dir });
    const lexes = r.code === 0 && /^(int|float) 1 /m.test(r.out);
    const scope = rules('numbers').map((x) => x.match && new RegExp(`^(?:${x.match})$`).test(n) ? x.name : null).find(Boolean);
    assert.equal(lexes, good.includes(n), `${n} lexes`);
    assert.equal(scope?.startsWith('constant.numeric'), lexes, `${n} is highlighted as ${scope}`);
  });
});

test('calls, member access and record keys', () => {
  const call = new RegExp(repo.call.match);
  for (const s of ['x(f)', '(f)', '($print)', '(a, b)(lib.f)', 'x[0](g)']) assert.ok(call.test(s), s);
  for (const s of ['if (x)', '!if (x)', 'while (done)', 'match (light)', 'onfail (c)']) assert.ok(!call.test(s), s);
  const [mem] = rules('member-access');
  assert.ok(new RegExp(mem.match).test('node.kind'));
  assert.ok(new RegExp(mem.match).test('xs[0].kind'));
  assert.ok(!new RegExp(mem.match).test('node . kind'));
  const key = new RegExp(repo['record-key'].match);
  assert.ok(key.test('{ kind: "num" }'));
  assert.ok(!key.test('c ? a : b'), 'a ternary branch is not a key');
});
