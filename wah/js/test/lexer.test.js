import { test } from 'node:test';
import assert from 'node:assert/strict';
import { lex } from '../src/lexer.js';
import { WahError } from '../src/errors.js';

const kinds = (src) => lex(src).slice(0, -1).map((t) => (t.v !== undefined ? `${t.t}:${t.v}` : t.t));
const lexError = (src) => {
  try {
    lex(src);
  } catch (e) {
    if (e instanceof WahError) return e.diags[0].message + (e.diags[0].hint ? ` | ${e.diags[0].hint}` : '');
    throw e;
  }
  assert.fail(`expected a lex error for ${src}`);
};
const warnings = (src) => {
  const w = [];
  lex(src, w);
  return w.map((d) => d.message);
};

test('§2.3 wah numerals', () => {
  assert.deepEqual(kinds('wah waah waaaah wh'), ['int:1', 'int:2', 'int:4', 'int:0']);
  assert.deepEqual(kinds('WAH WAAH WH'), ['int:-1', 'int:-2', 'int:0']);
  assert.deepEqual(kinds('WAH ** waah'), ['int:-1', '**', 'int:2']);
});

test('§2.3 mixed case is a lex error', () => {
  assert.match(lexError('Waah'), /mixed-case/);
  assert.match(lexError('wAH'), /mixed-case/);
  assert.match(lexError('Wa[5]h'), /mixed-case/);
});

test('§2.3 bracket form', () => {
  assert.deepEqual(kinds('wa[100]h WA[0xFF]H wa[0b1010]h'), ['int:100', 'int:-255', 'int:10']);
  assert.deepEqual(kinds('wa[0.5]h WA[0.5]H wa[1e9]h wa[1.0]h'), ['float:0.5', 'float:-0.5', 'float:1000000000', 'float:1']);
  assert.deepEqual(kinds('wa[0x1e9]h'), ['int:489']);
  assert.match(lexError('wa[.5]h'), /both sides of '\.'/);
  assert.match(lexError('wa[5.]h'), /both sides of '\.'/);
  assert.match(lexError('wa[x + y]h'), /not a numeric literal.*\$toInt/);
  assert.match(lexError('wa[5]H'), /expected 'h'/);
  assert.match(lexError('wa[99999999999999999]h'), /53-bit/);
});

test('§2.3 identifiers next to numerals', () => {
  assert.deepEqual(kinds('wa when wahoo'), ['id:wa', 'id:when', 'id:wahoo']);
  assert.deepEqual(kinds('wah[arr]'), ['int:1', '[', 'id:arr', ']']);
});

test('§2.4 bang tokens', () => {
  assert.deepEqual(kinds('!doubt !lurk !invited !x !real'), ['!doubt', '!lurk', '!invited', '!', 'id:x', '!', 'real']);
  assert.match(lexError('! doubt'), /no whitespace/);
  assert.deepEqual(kinds('!doubtful'), ['!', 'id:doubtful']);
});

test('§2.5 signal token', () => {
  assert.deepEqual(kinds(';WAH!'), [';', 'sig:1']);
  assert.deepEqual(kinds('WAAAH! ;'), ['sig:3', ';']);
  assert.deepEqual(kinds('c(WAH!)'), ['id:c', '(', 'sig:1', ')']);
  assert.deepEqual(kinds('WA[0x10]H!}'), ['sig:16', '}']);
  assert.deepEqual(kinds(';WAH!=x'), [';', 'int:-1', '!=', 'id:x']);
  assert.deepEqual(kinds(';WAH!invited'), [';', 'int:-1', '!invited']);
  assert.match(lexError(';wah!'), /lowercase/);
  assert.deepEqual(kinds('wah!=x'), ['int:1', '!=', 'id:x']);
});

test('§2.6 strings and escapes', () => {
  const [s] = lex('"a\\n\\r\\t\\\\\\"\\$\\u{1F608}"');
  assert.deepEqual(s.parts, ['a\n\r\t\\"$\u{1F608}']);
  assert.match(lexError('"\\q"'), /unknown escape/);
  assert.match(lexError('"a\nb"'), /raw newline/);
  assert.match(lexError('"abc'), /unterminated string/);
  const [t] = lex('"score: {n} {{x}}"');
  assert.equal(t.parts[0], 'score: ');
  assert.equal(t.parts[2], ' {x}');
  assert.match(lexError('"a } b"'), /unmatched '}'/);
});

test('§2.7 forbidden tokens', () => {
  assert.match(lexError('x = 1'), /'=' is not an operator \| use 'claim' or 'cheat'/);
  assert.match(lexError('--x'), /'--' is not an operator/);
  assert.deepEqual(kinds('- -x'), ['-', '-', 'id:x']);
  assert.deepEqual(kinds('--cheat x ++cheat y wah +cheat z wah <<cheat z'),
    ['opcheat:--', 'id:x', 'opcheat:++', 'id:y', 'int:1', 'opcheat:+', 'id:z', 'int:1', 'opcheat:<<', 'id:z']);
  assert.match(lexError('a >>> b'), /'>>>'/);
  assert.match(lexError('x 42'), /decimal digits/);
});

test('§2.8 lint warnings', () => {
  assert.deepEqual(warnings(';x;;y'), ["';;': likely a habitual trailing ';' from C-style languages"]);
  assert.deepEqual(warnings('"${x}"'), ['Γ-- interpolation is {x}, not ${x}']);
  assert.deepEqual(warnings('"$name"'), ['did you mean {name}?']);
  assert.deepEqual(warnings('"\\$name costs \\${x}"'), []);
  assert.match(warnings('a//b')[0], /starts a comment/);
});

test('§2.9 stdlib sigil', () => {
  assert.deepEqual(kinds('$print'), ['std:print']);
  assert.match(lexError('$ x'), /reserved for the standard library/);
  assert.match(lexError('x$'), /cannot appear in identifiers/);
  assert.match(lexError('$"a"'), /'\$' must be immediately followed/);
});

test('reserved words', () => {
  assert.match(lexError('blame'), /reserved/);
  assert.match(lexError('invited'), /did you mean '!invited'/);
});
