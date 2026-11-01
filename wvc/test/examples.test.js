// Every example and sample runs and prints what it should.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { exe, examples, libDir, noZig, root, runtimeC, tempDir, wvc, zigBuild } from './helpers.js';

const skip = noZig;
const lines = (...xs) => xs.map((x) => x + '\n').join('');

// One turn of samples/walk.wvt: clear the screen, then the room (whose middle
// two rows change), the gold and the message.
const frame = (rows, gold, message) =>
  '\x1b[2J\x1b[H' + lines('#########', ...rows, '#.......#', '#########', `gold: ${gold}`, message);

const fizzbuzz = [];
for (let i = 1; i <= 100; i++) fizzbuzz.push(i % 15 === 0 ? 'FizzBuzz' : i % 3 === 0 ? 'Fizz' : i % 5 === 0 ? 'Buzz' : String(i));

// What each example prints (a string, or a pattern when it varies from run to
// run), and the arguments and input it is given. Each runs in a new directory.
const expected = {
  'examples/animate.wvt': {
    // From a file, keys come at once, so there are no ticks between them.
    input: 'ab\x1b[Aq',
    out: ['none', 'a', 'b', 'up'].map((k) => `\r| last key: ${k}      `).join('') + '\n',
  },
  'examples/echo.wvt': {
    input: 'one\ntwo\nquit\nthree\n',
    out: lines("Simple input/output echo test. Type 'quit' to end.", 'ECHO: one', 'ECHO: two'),
  },
  'examples/enums.wvt': {
    out: lines('rock, paper, scissors are each a Hand', 'rock vs scissors: rock wins', 'scissors vs paper: scissors wins',
      'rock vs paper: paper wins', 'paper vs paper: draw', 'red -> green -> amber -> red -> green', 'false'),
  },
  'examples/files.wvt': { out: lines('first line', 'second line', 'listed: true', 'exists: false', 'cannot read it now') },
  'examples/fizzbuzz.wvt': { out: lines(...fizzbuzz, '1.5') },
  'examples/hello.wvt': { out: lines('Hello, World') },
  'examples/kinds.wvt': { out: lines('an orc, with 5 hp', 'a ring, worth 30 gold') },
  'examples/keys.wvt': {
    input: 'a?\x1b[A\x13q',
    out: lines('press keys; q quits', 'a: a character', '?: a character', 'up: a named key', 'ctrl+s: a named key'),
  },
  'examples/maps.wvt': { out: lines('3', 'plums: 0', 'true', 'apple: 4', 'plum: 5', '2 kinds') },
  'examples/match.wvt': {
    out: lines('1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz', '7', "unknown instruction 'dance', skipped", '70'),
  },
  'examples/modules.wvt': {
    out: lines('text.wvt: initialized', 'shapes.wvt: initialized', 'modules.wvt: running', 'a circle has 0 corners',
      'a triangle has 3 corners', 'a square has 4 corners', '7 corners in all'),
  },
  'examples/positions.wvt': { out: lines('orc', 'false', 'orc at 3,4', 'rat at -2,7') },
  'examples/process.wvt': {
    out: lines('hello, stranger', 'child: given 2 arguments: child, two words', 'the child exited with 3'),
  },
  'examples/random.wvt': { out: lines('dice: 2 3 3 5 4', 'a float from 0.0 up to 1.0: 0.3107552296191898', 'next: 10, replayed: 10') },
  'examples/records.wvt': {
    out: lines('((2 + 3) * 7) = 35', '2', 'Ada: 50', 'Grace: 3', 'Grace: 10 coins, 1 win', 'now 15, snapshot 10',
      'name = Grace', 'coins = 15', 'wins = 2', 'false', 'true'),
  },
  'examples/refs.wvt': {
    out: lines('total: 10', '4 -> 3 -> 2 -> 1', 'ref keeps it: true, var copies it: false', 'through the alias: 3'),
  },
  'examples/sleep.wvt': { out: /^3\.\.\.2\.\.\.1\.\.\.go!\ntook (9\d\d|1\d\d\d) ms\n$/ },
  'examples/sort.wvt': {
    out: lines('1 2 3', 'apple fig pear', 'Ada 7, Cy 7, Bo 9', 'Bo 9, Ada 7, Cy 7', 'Bo 9, Cy 7, Ada 7'),
  },
  'examples/terminal.wvt': { out: lines('input: false, output: false', 'no size: stdout is not a terminal') },
  'samples/walk.wvt': {
    input: 'a\x1b[Aawxq',
    out: [
      frame(['#.$...$.#', '#..$@...#'], 0, ''),
      frame(['#.$...$.#', '#..@....#'], 7, 'found 7 gold'),
      frame(['#.$@..$.#', '#.......#'], 7, ''),
      frame(['#.@...$.#', '#.......#'], 16, 'found 9 gold'),
      frame(['#.@...$.#', '#.......#'], 16, 'bump'),
      frame(['#.@...$.#', '#.......#'], 16, 'x does nothing'),
    ].join('') + lines('Goodbye, with 16 gold.'),
  },
  'samples/tokenize.wvt': {
    args: [join(root, 'examples/hello.wvt')],
    out: lines('operator ;', 'keyword const', 'name s', 'name is', 'string "World"', 'operator ;', 'string "Hello, {s}"',
      'operator (', 'stdlib $print', 'operator )', 'operator ;', 'keyword exit', '12 tokens'),
  },
};

test('every example and sample has an expected output', () => {
  assert.deepEqual(examples(), Object.keys(expected).sort());
});

for (const file of examples()) {
  test(file, { skip }, () => {
    const { input = '', args = [], out } = expected[file] ?? {};
    const dir = tempDir();
    const code = join(dir, 'out.c');
    const c = wvc([file, '-o', code, '--runtime', runtimeC, '--lib', libDir]);
    assert.deepEqual({ err: c.err, code: c.code }, { err: '', code: 0 });
    const env = { ...process.env };
    delete env.WAVERT_NAME; // examples/process.wvt reads it
    const r = spawnSync(zigBuild(code, join(dir, `out${exe}`)), args, { encoding: 'utf8', input, cwd: dir, env });
    assert.deepEqual({ err: r.stderr, code: r.status }, { err: '', code: 0 });
    if (out instanceof RegExp) assert.match(r.stdout, out);
    else assert.equal(r.stdout, out);
  });
}
