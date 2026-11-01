// Every example runs, natively and on Node, and prints what it should.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { exe, examples, noZig, root, runtimeC, runtimeJs, tempDir, wvc, zigBuild } from './helpers.js';

const skip = noZig;
const lines = (...xs) => xs.map((x) => x + '\n').join('');

// One turn of examples/walk.wvt: clear the screen, then the room (whose middle
// two rows change), the gold and the message.
const frame = (rows, gold, message) =>
  '\x1b[2J\x1b[H' + lines('#########', ...rows, '#.......#', '#########', `gold: ${gold}`, message);

const fizzbuzz = [];
for (let i = 1; i <= 100; i++) fizzbuzz.push(i % 15 === 0 ? 'FizzBuzz' : i % 3 === 0 ? 'Fizz' : i % 5 === 0 ? 'Buzz' : String(i));

// What each example prints, and the arguments and input it is given.
const expected = {
  'examples/echo.wvt': {
    input: 'one\ntwo\nquit\nthree\n',
    out: lines("Simple input/output echo test. Type 'quit' to end.", 'ECHO: one', 'ECHO: two'),
  },
  'examples/enums.wvt': {
    out: lines('rock, paper, scissors are each a Hand', 'rock vs scissors: rock wins', 'scissors vs paper: scissors wins',
      'rock vs paper: paper wins', 'paper vs paper: draw', 'red -> green -> amber -> red -> green', 'false'),
  },
  'examples/fizzbuzz.wvt': { out: lines(...fizzbuzz, '1.5') },
  'examples/hello.wvt': { out: lines('Hello, World') },
  'examples/match.wvt': {
    out: lines('1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz', '7', "unknown instruction 'dance', skipped", '70'),
  },
  'examples/modules.wvt': {
    out: lines('text.wvt: initialized', 'shapes.wvt: initialized', 'modules.wvt: running', 'a circle has 0 corners',
      'a triangle has 3 corners', 'a square has 4 corners', '7 corners in all'),
  },
  'examples/records.wvt': {
    out: lines('((2 + 3) * 7) = 35', '2', 'Ada: 50', 'Grace: 3', 'Grace: 10 coins, 1 win', 'now 15, snapshot 10',
      'name = Grace', 'coins = 15', 'wins = 2', 'false', 'true'),
  },
  'examples/refs.wvt': {
    out: lines('total: 10', '4 -> 3 -> 2 -> 1', 'ref keeps it: true, var copies it: false', 'through the alias: 3'),
  },
  'examples/walk.wvt': {
    input: 'h\x1b[Ahkxq',
    out: [
      frame(['#.$...$.#', '#..$@...#'], 0, ''),
      frame(['#.$...$.#', '#..@....#'], 7, 'found 7 gold'),
      frame(['#.$@..$.#', '#.......#'], 7, ''),
      frame(['#.@...$.#', '#.......#'], 16, 'found 9 gold'),
      frame(['#.@...$.#', '#.......#'], 16, 'bump'),
      frame(['#.@...$.#', '#.......#'], 16, 'x does nothing'),
    ].join('') + lines('Goodbye, with 16 gold.'),
  },
  'examples/tokenize.wvt': {
    args: [join(root, 'examples/hello.wvt')],
    out: lines('operator ;', 'keyword const', 'name s', 'name is', 'string "World"', 'operator ;', 'string "Hello, {s}"',
      'operator (', 'stdlib $print', 'operator )', 'operator ;', 'keyword exit', '12 tokens'),
  },
};

test('every example has an expected output', () => {
  assert.deepEqual(examples(), Object.keys(expected).sort());
});

for (const file of examples()) {
  test(file, { skip }, () => {
    const { input = '', args = [], out } = expected[file] ?? {};
    const dir = tempDir();
    for (const [target, rt] of [['c', runtimeC], ['js', runtimeJs]]) {
      const code = join(dir, `out.${target}`);
      const c = wvc([file, '--target', target, '-o', code, '--runtime', rt]);
      assert.deepEqual({ err: c.err, code: c.code }, { err: '', code: 0 }, `${file} --target ${target}`);
      const [cmd, cmdArgs] = target === 'c'
        ? [zigBuild(code, join(dir, `out${exe}`)), args]
        : [process.execPath, [code, ...args]];
      const r = spawnSync(cmd, cmdArgs, { encoding: 'utf8', input });
      assert.deepEqual({ out: r.stdout, err: r.stderr, code: r.status }, { out, err: '', code: 0 }, `${file} --target ${target}`);
    }
  });
}
