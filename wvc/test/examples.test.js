// Every example and sample runs and prints what it should.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
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
  'examples/stdlib.wvt': { out: lines('7 10 -4') },
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
  // Every puzzle solved in turn, with a space after each to go on: the
  // shortest solutions, which keep the puzzles solvable as they are changed.
  'samples/crates.wvt': {
    args: [join(root, 'samples/crates-puzzles')],
    input: ['d', 'dsasd', 'sdwasaawdwdsddwa', 'wdaaawwddswdddsssaawa', 'dddwwwwasswaaawass'].join(' ') + ' ',
    out: new RegExp('^crates: puzzle 1 of 5: First push\\n\\n#####\\n#@\\$\\.#\\n#####\\n\\nmoves 0 [^\\n]*\\n\\n'
      + ['1 move', '5 moves', '16 moves', '21 moves', '18 moves'].map((m) => `[\\s\\S]*Solved in ${m}!`).join('')
      + '[\\s\\S]*\\nAll 5 puzzles solved\\.\\n$'),
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

test('samples/crates.wvt: undo, restart, moving between puzzles, and bad puzzle files', { skip }, () => {
  const dir = tempDir();
  const c = wvc(['samples/crates.wvt', '-o', join(dir, 'crates.c'), '--runtime', runtimeC, '--lib', libDir]);
  assert.deepEqual({ err: c.err, code: c.code }, { err: '', code: 0 });
  const program = zigBuild(join(dir, 'crates.c'), join(dir, `crates${exe}`));
  const play = (folder, input) => {
    const r = spawnSync(program, [folder], { encoding: 'utf8', input });
    return { out: r.stdout, err: r.stderr, code: r.status };
  };
  // The status and message lines of each frame.
  const status = (out) => out.split('\n').filter((l) => /^(crates:|moves|\w.* (here|undo|puzzle)$|Goodbye)/.test(l));
  const r = play(join(root, 'samples/crates-puzzles'), 'nduuxrppq');
  assert.deepEqual(status(r.out), [
    'crates: puzzle 1 of 5: First push', 'moves 0    u undo  r restart  n/p puzzle  q quit',
    'crates: puzzle 2 of 5: Around the pillar', 'moves 0    u undo  r restart  n/p puzzle  q quit',
    'crates: puzzle 2 of 5: Around the pillar', 'moves 1    u undo  r restart  n/p puzzle  q quit',
    'crates: puzzle 2 of 5: Around the pillar', 'moves 0    u undo  r restart  n/p puzzle  q quit',
    'crates: puzzle 2 of 5: Around the pillar', 'moves 0    u undo  r restart  n/p puzzle  q quit', 'nothing to undo',
    'crates: puzzle 2 of 5: Around the pillar', 'moves 0    u undo  r restart  n/p puzzle  q quit', 'x does nothing here',
    'crates: puzzle 2 of 5: Around the pillar', 'moves 0    u undo  r restart  n/p puzzle  q quit',
    'crates: puzzle 1 of 5: First push', 'moves 0    u undo  r restart  n/p puzzle  q quit',
    'crates: puzzle 1 of 5: First push', 'moves 0    u undo  r restart  n/p puzzle  q quit', 'this is the first puzzle',
    'Goodbye.',
  ]);
  // Files with mistakes are reported and skipped; other files are ignored.
  const folder = join(dir, 'puzzles');
  mkdirSync(folder);
  const files = {
    'a.txt': '; title: Good\n#####\n#@$.#\n#####\n',
    'b.txt': '; nothing\n\n',
    'c.txt': '#####\n#@$x#\n#####\n',
    'd.txt': '#####\n# $.#\n#####\n',
    'e.txt': '######\n#@$$.#\n######\n',
    'f.txt': '#####\n#@@.#\n#####\n',
    'notes.md': 'not a puzzle',
  };
  for (const [name, text] of Object.entries(files)) writeFileSync(join(folder, name), text);
  const bad = play(folder, 'q');
  assert.equal(bad.err, lines(
    'crates: b.txt is skipped: there is no puzzle in it',
    "crates: c.txt is skipped: row 2 has 'x', which is not part of a puzzle",
    'crates: d.txt is skipped: there is no player (@ or +)',
    'crates: e.txt is skipped: it has 2 crates and 1 target; they must match, and there must be some',
    'crates: f.txt is skipped: there is more than one player (@ or +)'));
  assert.match(bad.out, /^crates: puzzle 1 of 1: Good\n/);
  const none = play(join(dir, 'no-such-folder'), '');
  assert.equal(none.code, 2);
  assert.match(none.err, /^crates: cannot read the puzzle folder .*no-such-folder\n$/);
});

test('samples/crates.wvt: a message stays on screen while the game ticks', { skip, timeout: 30000 }, async () => {
  const dir = tempDir();
  const c = wvc(['samples/crates.wvt', '-o', join(dir, 'crates.c'), '--runtime', runtimeC, '--lib', libDir]);
  assert.deepEqual({ err: c.err, code: c.code }, { err: '', code: 0 });
  const program = zigBuild(join(dir, 'crates.c'), join(dir, `crates${exe}`));
  // stdin stays open with nothing to read, so $waitKey times out and the game
  // draws a frame each tick.
  const child = spawn(program, [join(root, 'samples/crates-puzzles')], { stdio: ['pipe', 'pipe', 'pipe'] });
  let out = '';
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (chunk) => { out += chunk; });
  child.stdin.write('d');
  await new Promise((resolve) => setTimeout(resolve, 1200));
  child.stdin.end();
  await new Promise((resolve) => child.on('close', resolve));
  // Every frame from the solving move on shows the message, ticks included.
  const frames = out.split('crates: puzzle').slice(2);
  assert.ok(frames.length >= 3, `only ${frames.length} frames after the move`);
  for (const f of frames) assert.match(f, /Solved in 1 move!/);
});

test('samples/crates.wvt: a key held down does not stop the ticks', { skip, timeout: 30000 }, async () => {
  const dir = tempDir();
  const c = wvc(['samples/crates.wvt', '-o', join(dir, 'crates.c'), '--runtime', runtimeC, '--lib', libDir]);
  assert.deepEqual({ err: c.err, code: c.code }, { err: '', code: 0 });
  const program = zigBuild(join(dir, 'crates.c'), join(dir, `crates${exe}`));
  const child = spawn(program, [join(root, 'samples/crates-puzzles')], { stdio: ['pipe', 'pipe', 'pipe'] });
  let out = '';
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', (chunk) => { out += chunk; });
  // A key every 50 ms for 1.5 s, as key repeat sends them: one that does
  // nothing, so that each draws a frame.
  let keys = 0;
  const timer = setInterval(() => { child.stdin.write('x'); keys++; }, 50);
  await new Promise((resolve) => setTimeout(resolve, 1500));
  clearInterval(timer);
  child.stdin.end();
  await new Promise((resolve) => child.on('close', resolve));
  // A frame for the start, one per key, and one per tick: about 6 ticks in
  // 1.5 s, and at least 3 however slow the machine.
  const frames = out.split('crates: puzzle').length - 1;
  assert.ok(frames >= 1 + keys + 3, `${frames} frames for ${keys} keys: the ticks stopped while keys came`);
});
