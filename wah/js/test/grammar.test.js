// Keeps the VS Code grammar (editors/vscode) in sync with the compiler: its
// word lists must match the lexer's and the stdlib's, and its regexes must
// classify the lexer's token shapes the same way. Zero dependencies, so this
// checks rule by rule rather than running the TextMate engine.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { KEYWORDS, RESERVED, BANG, CHEAT_OPS, lex } from '../src/lexer.js';
import { STDLIB } from '../src/stdlib.js';

const grammar = JSON.parse(readFileSync(new URL('../../editors/vscode/syntaxes/wah.tmLanguage.json', import.meta.url), 'utf8'));
const repo = grammar.repository;
const rules = (name) => repo[name].patterns ?? [repo[name]];

// The scope of the first rule in `name` that matches all of `text`, or null.
function scopeOf(name, text) {
  for (const r of rules(name)) {
    if (r.match && new RegExp(`^(?:${r.match})$`).test(text)) return r.name.replace(/\.wah$/, '');
  }
  return null;
}
// Lowercase words named in a rule group's regexes.
const wordsIn = (...names) => new Set(names.flatMap((n) => rules(n).flatMap((r) => r.match.replace(/\\b/g, ' ').match(/[a-z]{2,}/g) ?? [])));

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
  for (const w of KEYWORDS) {
    assert.ok(scopeOf('keywords', w) ?? scopeOf('constants', w), `keyword '${w}' is not highlighted`);
  }
  for (const w of RESERVED.keys()) {
    assert.ok(scopeOf('keywords', w), `reserved word '${w}' is not highlighted`);
  }
  assert.equal(scopeOf('keywords', 'invited'), 'invalid.illegal.reserved');
  for (const w of wordsIn('keywords', 'constants')) {
    assert.ok(KEYWORDS.has(w) || RESERVED.has(w), `the grammar highlights '${w}', which the lexer does not reserve`);
  }
  for (const w of BANG) assert.ok(scopeOf('bang-keywords', `!${w}`), `'!${w}' is not highlighted`);
  for (const op of CHEAT_OPS) assert.equal(scopeOf('compound-cheat', `${op}cheat`), 'keyword.operator.assignment.compound');
});

test('builtins match the stdlib table', () => {
  for (const [name, sig] of STDLIB) {
    const want = sig === 'const' ? 'support.constant.builtin' : 'support.function.builtin';
    assert.equal(scopeOf('stdlib', `$${name}`), want, `$${name}`);
  }
  const listed = rules('stdlib').filter((r) => r.name.startsWith('support.'))
    .flatMap((r) => r.match.match(/\(\?:([^)]*)\)/)[1].split('|'));
  for (const name of listed) assert.ok(STDLIB.has(name), `the grammar lists $${name}, which the stdlib does not define`);
  assert.equal(scopeOf('stdlib', '$nope'), 'invalid.illegal.unknown-builtin');
});

test('numerals and signals match the lexer', () => {
  for (const src of ['wah', 'wh', 'WAAH', 'WH', 'wa[42]h', 'WA[0xFF]H', 'wa[0b1010]h', 'wa[0.5]h', 'WA[1e9]H']) {
    assert.match(lex(src)[0].t, /^(int|float)$/, src);
    assert.equal(scopeOf('numerals', src), 'constant.numeric', src);
  }
  for (const src of ['Waah', 'wAH', 'Wa[5]h']) {
    assert.throws(() => lex(src), undefined, src);
    assert.equal(scopeOf('numerals', src), 'invalid.illegal.mixed-case-numeral', src);
  }
  for (const src of ['WAH!', 'WAAAH!', 'WA[0x10]H!']) {
    assert.equal(lex(`${src} `)[0].t, 'sig', src);
    assert.equal(scopeOf('signal', src), 'keyword.control.signal', src);
  }
  // '!' followed by anything else is not part of a signal (§2.5)
  assert.equal(lex('WAH!=x')[0].t, 'int');
  assert.ok(!new RegExp(repo.signal.match).test('WAH!=x'));
});

test('member access and record keys', () => {
  const [mem] = rules('member-access');
  assert.ok(new RegExp(mem.match).test('kind.node'));
  assert.ok(!new RegExp(mem.match).test('kind . node'));
  const key = new RegExp(repo['record-key'].match);
  assert.ok(key.test('{ kind: "num" }'));
  assert.ok(key.test('{ a: wah, b: waah }'.slice(9)), 'a key after a comma');
  assert.ok(!key.test('c ? a : b'), 'a ternary branch is not a key');
});
