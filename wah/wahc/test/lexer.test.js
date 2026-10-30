// The wahc lexer must agree with wahc0's: the same tokens, warnings and errors.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { lex } from '../../js/src/lexer.js';
import { Source, WahError, formatDiag } from '../../js/src/errors.js';
import { codePoints, exampleLibraries, examples, fmtFloat, readRepo, runWahc, wahcFiles } from './helpers.js';

// wahc's --tokens format, from wahc0's tokens. wahc counts positions in code points.
function dump(toks, cp, indent = '') {
  let s = '';
  for (const t of toks) {
    s += `${indent}${t.t} ${cp(t.pos)} ${cp(t.end)} ${t.sp ? 'real' : 'fake'}`;
    if (t.t === 'id' || t.t === 'std' || t.t === 'opcheat') s += ` ${t.v}`;
    else if (t.t === 'float') s += ` ${t.text} ${fmtFloat(t.v)}`;
    else if (t.t === 'int' || t.t === 'sig') s += ` ${t.text} ${t.v}`;
    s += '\n';
    if (t.t !== 'str') continue;
    for (const p of t.parts) {
      if (typeof p === 'string') {
        s += `${indent}  | ${JSON.stringify(p)}\n`;
      } else {
        s += `${indent}  { ${cp(p.pos)}\n${dump(p.tokens, cp, indent + '    ')}${indent}  }\n`;
      }
    }
  }
  return s;
}

function expected(text, file) {
  const src = new Source(file, text.replace(/^﻿/, ''));
  const cp = codePoints(src.text);
  const warnings = [];
  let toks = null, error = null;
  try {
    toks = lex(src.text, warnings);
  } catch (e) {
    if (!(e instanceof WahError)) throw e;
    error = e.diags[0];
  }
  const diags = error ? [...warnings, error] : warnings;
  return {
    out: toks ? dump(toks, cp) : '',
    err: diags.map((d) => formatDiag(d, src) + '\n').join(''),
    code: error ? 2 : 0,
  };
}

function agree(text, file = 'test.wah') {
  assert.deepEqual(runWahc(['--tokens', file], { [file]: text }), expected(text, file), JSON.stringify(text));
}

for (const file of [...examples(), ...exampleLibraries(), ...wahcFiles()]) {
  test(`agrees with wahc0 on ${file}`, () => agree(readRepo(file), file));
}

test('agrees on numerals', () => {
  for (const s of ['wah waah waaaah wh', 'WAH WAAH WH', 'WAH ** waah', 'wa[100]h WA[0xFF]H wa[0b1010]h',
    'wa[0.5]h WA[0.5]H wa[1e9]h wa[1.0]h wa[1E+3]h wa[2.5e-3]h WA[0.0]H wa[1e21]h', 'wa[0x1e9]h', 'wa[007]h',
    'wa when wahoo', 'wah[arr]', 'wa[ 1]h', 'wa[0x]h', 'wa[0b2]h', 'wa[1e]h', 'wa[1.5e]h', 'wa[1e999]h',
    'Waah', 'wAH', 'Wa[5]h', 'wa[.5]h', 'wa[5.]h', 'wa[.]h', 'wa[.5e3]h', 'wa[x + y]h', 'wa[5]H', 'wa[5',
    'wa[5]hx', 'wa[99999999999999999]h', 'WA[9007199254740991]H', 'wah$', 'wa[5]h$x']) {
    agree(s);
  }
});

test('agrees on bang and signal tokens', () => {
  for (const s of ['!doubt !lurk !invited !x !real', '!doubtful', '! doubt', '!\tlurk', '!\u000binvited',
    '! doubt', '! real', `!${' '.repeat(70)}doubt`, ';WAH!', 'WAAAH! ;', 'c(WAH!)', 'WA[0x10]H!}',
    ';WAH!=x', ';WAH!invited', ';wah!', 'wah!=x', 'WH!', 'WA[0.5]H!', 'WA[0]H! ']) {
    agree(s);
  }
});

test('agrees on strings', () => {
  for (const s of ['"a\\n\\r\\t\\\\\\"\\$\\u{1F608}"', '"score: {n} {{x}}"', '"{a + "{b}"} c"', '"{ {a: wah}(f) }"',
    '"\u{1F608} {x}" y', '"\\q"', '"\\', '"a\nb"', '"a\rb"', '"abc', '"a } b"', '"{}"', '"{x"', '"{x" + "y"}',
    '"\\u{110000}"', '"\\u{D800}"', '"\\u{}"', '"\\u{1234567}"', '"\\u{12"', '"\\u{g}"', '"\\u12"', '"\\u{41}{{"',
    '"{"}"}"']) {
    agree(s);
  }
});

test('agrees on operators and forbidden tokens', () => {
  for (const s of ['x = 1', '--x', '++x', '- -x', '--cheat x ++cheat y wah +cheat z wah <<cheat z',
    'wah **cheat z >>cheat y +cheaters', 'a >>> b', 'x 42', 'x 4.2e1abc', '$print', '$ x', 'x$', '$"a"', '$',
    'blame', 'sulk', 'regret', 'wails', 'steal', 'bribe', 'invited', 'kind.node', 'kind . node', 'kind .node',
    'kind. node', 'a.', 'a./b', 'a.//b', '@', '#x', 'a == b != c <= d >= e && f || g ~h ? i : j % k ^ l & m | n',
    '/* never', '/* a */ x /* b */']) {
    agree(s);
  }
});

test('agrees on warnings', () => {
  for (const s of [';x;;y', ';; ;', '"${x}"', '"$name"', '"\\$name costs \\${x}"', '"$"', 'a//b', 'a //b', '(a)//b',
    ';;;\n x = 1', '"$a $b"\n;;\n@']) {
    agree(s);
  }
});

test('agrees on diagnostic locations', () => {
  for (const s of ['x\n  ;y = 2', '\t;x = 1', '\t\tx\t= 1', ';x\r\n;y = 1\r\n', '﻿;x = 1', ';x\n', '"\u{1F608}"\n@',
    'x\n\n\n   @   ']) {
    agree(s);
  }
});

test('unreadable files', () => {
  assert.deepEqual(runWahc(['--tokens', 'missing.wah']), { out: '', err: 'wahc: cannot read missing.wah\n', code: 2 });
});
