// The standard library added in Draft 0.2, natively and on Node: $readKey on
// redirected input (§12.2), $time, and random generators (§12.8). Both
// backends must give the same output.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { exe, noZig, readRepo, runtimeC, runtimeJs, wvc, writeFiles, zigBuild } from './helpers.js';

const skip = noZig;

// Runs source on both backends with input (a string or a Buffer); returns the
// native { out, err, code }, after checking that the two agree unless same is false.
function runBoth(source, input = '', { same = true } = {}) {
  const dir = writeFiles({ 'main.wvt': source });
  const results = [];
  for (const [target, rt] of [['c', runtimeC], ['js', runtimeJs]]) {
    const code = join(dir, `main.${target}`);
    const c = wvc(['main.wvt', '--target', target, '-o', code, '--runtime', rt], { cwd: dir });
    assert.deepEqual({ err: c.err, code: c.code }, { err: '', code: 0 }, `--target ${target}`);
    const [cmd, args] = target === 'c' ? [zigBuild(code, join(dir, `main${exe}`)), []] : [process.execPath, [code]];
    const r = spawnSync(cmd, args, { encoding: 'utf8', input });
    results.push({ out: r.stdout, err: r.stderr, code: r.status });
  }
  if (same) assert.deepEqual(results[1], results[0], 'C and JavaScript differ');
  return results[0];
}

// Every key in the input, one per line, then "end".
const readKeys = `
;var keys is []
;var more is true
;while (more) {
    ;const k is ($readKey)
        onfail { ;set more to false
        "" }
    ;if (more) { ;(keys, k)($push) }
}
;(keys, "\\n")($join)($print)
;"end"($print)
;exit
`;
const keysOf = (input) => {
  const r = runBoth(readKeys, Buffer.isBuffer(input) ? input : Buffer.from(input, 'utf8'));
  assert.equal(r.code, 0, r.err);
  const lines = r.out.split('\n');
  assert.deepEqual(lines.slice(-2), ['end', '']);
  return lines.slice(0, -2).filter((x, i, all) => !(all.length === 1 && x === ''));
};

test('character keys', { skip }, () => {
  assert.deepEqual(keysOf('jJ é?😀'), ['j', 'J', ' ', 'é', '?', '😀']);
  // An invalid byte is a character key for U+FFFD (§12.1).
  assert.deepEqual(keysOf(Buffer.from([0x61, 0xff, 0x62])), ['a', '�', 'b']);
});

test('control characters', { skip }, () => {
  assert.deepEqual(keysOf('\t\r\n\r\x7f\x08\n'), ['tab', 'enter', 'enter', 'backspace', 'backspace', 'enter']);
  assert.deepEqual(keysOf('\x01\x13\x16\x1a'), ['ctrl+a', 'ctrl+s', 'ctrl+v']);
  // Ctrl+C, Ctrl+Z and the other control characters are discarded.
  assert.deepEqual(keysOf('a\x03\x1a\x00\x1c\x1f\u0085b'), ['a', 'b']);
});

test('escape sequences', { skip }, () => {
  const seqs = {
    '\x1b[A': 'up', '\x1bOB': 'down', '\x1b[1;5C': 'right', '\x1b[D': 'left',
    '\x1b[H': 'home', '\x1bOH': 'home', '\x1b[1~': 'home', '\x1b[7~': 'home',
    '\x1b[F': 'end', '\x1bOF': 'end', '\x1b[4~': 'end', '\x1b[8~': 'end',
    '\x1b[2~': 'insert', '\x1b[3~': 'delete', '\x1b[5~': 'pageup', '\x1b[6;2~': 'pagedown',
    '\x1b[E': 'clear', '\x1b[G': 'clear', '\x1bOE': 'clear', '\x1bOM': 'enter', '\x1b[Z': 'tab',
    '\x1bOP': 'f1', '\x1b[11~': 'f1', '\x1b[[A': 'f1', '\x1bOQ': 'f2', '\x1b[[B': 'f2', '\x1bOS': 'f4',
    '\x1b[15~': 'f5', '\x1b[[E': 'f5', '\x1b[17~': 'f6', '\x1b[21~': 'f10', '\x1b[23~': 'f11', '\x1b[24~': 'f12',
  };
  assert.deepEqual(keysOf(Object.keys(seqs).join('')), Object.values(seqs));
  // Unknown sequences are discarded whole.
  assert.deepEqual(keysOf('a\x1b[99~\x1b[2X\x1b[ A\x1b[[Z\x1bOzb'), ['a', 'b']);
});

test('escape and alt', { skip }, () => {
  assert.deepEqual(keysOf('\x1bx\x1bé\x1b?'), ['alt+x', 'alt+é', 'alt+?']);
  // ESC before anything that is not a character key or a sequence is Escape.
  assert.deepEqual(keysOf('\x1b\x01\x1b\x1b[A\x1b\r'), ['escape', 'ctrl+a', 'escape', 'up', 'escape', 'enter']);
  // At the end of input, ESC is Escape and a cut-off sequence is read as keys.
  assert.deepEqual(keysOf('\x1b'), ['escape']);
  assert.deepEqual(keysOf('\x1b[1'), ['escape', '[', '1']);
  assert.deepEqual(keysOf('\x1bO'), ['escape', 'O']);
});

test('$readKey and $readLine share their input', { skip }, () => {
  const r = runBoth(`
;($readKey)($print)
;($readLine)($print)
;($readKey)($print)
;($readLine)($print)
;const k is ($readKey)
    onfail { "none" }
;k($print)
;exit
`, 'ab c\n\x1b[Ade\n');
  assert.deepEqual(r, { out: 'a\nb c\nup\nde\nnone\n', err: '', code: 0 });
});

test('$readKey at the end of input is an unhandled fail', { skip }, () => {
  const r = runBoth(';($readKey)($print)\n;exit\n');
  assert.equal(r.code, 1);
  assert.match(r.err, /unhandled fail \(code 1\) from '\$readKey'/);
});

test('$time is the time in milliseconds', { skip }, () => {
  const before = Date.now();
  const r = runBoth(';const t is ($time)\n;"{t($typeOf)} {t}"($print)\n;exit\n', '', { same: false });
  const [type, t] = r.out.trim().split(' ');
  assert.equal(type, 'int');
  assert.ok(Number(t) >= before - 1000 && Number(t) <= Date.now() + 1000, t);
});

// The test vectors in §12.8.
const vectors = [...readRepo('docs/wavert-spec.md').matchAll(/^\| `(-?\d+)` \| ([\d, ]+) \| ([\d, ]+) \| ([\d ]+) \| ([\d.e-]+) \|$/gm)]
  .map((m) => ({ seed: m[1], state: m[2], words: m[3], dice: m[4], float: m[5] }));

test('random generators match the test vectors in the specification', { skip }, () => {
  assert.equal(vectors.length, 4);
  const r = runBoth(`
;fn show(seed) {
    ;var rng is seed($newRandom)
    ;const state is "{rng.s0}, {rng.s1}, {rng.s2}, {rng.s3}"
    ;var words is []
    ;while (words($length) < 3) { ;(words, (rng, 2 ** 32)($random))($push) }
    ;var dice is []
    ;while (dice($length) < 10) { ;(dice, (rng, 6)($random) + 1)($push) }
    ;"{state} | {(words, ", ")($join)} | {(dice, " ")($join)} | {rng($randomFloat)}"($print)
}
${vectors.map((v) => `;(${v.seed})(show)`).join('\n')}
;exit
`);
  assert.equal(r.code, 0, r.err);
  assert.equal(r.out, vectors.map((v) => `${v.state} | ${v.words} | ${v.dice} | ${v.float}\n`).join(''));
});

test('a generator is a record that can be saved, restored and replayed', { skip }, () => {
  const r = runBoth(`
;var rng is 7($newRandom)
;(rng($fields), ",")($join)($print)
;rng($typeOf)($print)
;const saved is rng
;const a is (rng, 100)($random)
;const b is (rng, 100)($random)
;var replay is saved
;((replay, 100)($random) == a && (replay, 100)($random) == b)($print)
;var restored is { s3: saved.s3, s2: saved.s2, s1: saved.s1, s0: saved.s0 }
;((restored, 100)($random) == a)($print)
;fn roll(ref g) { (g, 6)($random) + 1 }
;var before is rng($fields)
;const r1 is rng(roll)
;(r1 >= 1 && r1 <= 6)($print)
;const f is rng($randomFloat)
;(f($typeOf) == "float" && f >= 0 && f < 1)($print)
;(1($newRandom), 1($newRandom))($same)($print)
;(1($newRandom), (-1)($newRandom))($same)($print)
;exit
`);
  assert.deepEqual(r, { out: 's0,s1,s2,s3\nrecord\ntrue\ntrue\ntrue\ntrue\ntrue\nfalse\n', err: '', code: 0 });
});

test('$random draws every value of a small range evenly', { skip }, () => {
  const r = runBoth(`
;var rng is 2026($newRandom)
;var counts is [0, 0, 0]
;var i is 0
;while (i < 3000) {
    ;set counts[(rng, 3)($random)] += 1
    ;set i += 1
}
;(counts, " ")($join)($print)
;exit
`);
  assert.equal(r.code, 0, r.err);
  for (const n of r.out.trim().split(' ').map(Number)) assert.ok(n > 900 && n < 1100, r.out);
});

test('misusing a generator panics', { skip }, () => {
  const panic = (code) => {
    const r = runBoth(`${code}\n;exit\n`);
    assert.equal(r.code, 1, code);
    return /uncaught panic: (.*)/.exec(r.err)[1];
  };
  assert.equal(panic(';const rng is 1($newRandom)\n;(rng, 6)($random)'), 'random generator is frozen; hold it in a var or ref binding');
  assert.equal(panic(';fn roll(g) { (g, 6)($random) }\n;var rng is 1($newRandom)\n;rng(roll)'),
    'random generator is frozen; hold it in a var or ref binding');
  assert.equal(panic(';var rng is { s0: 0, s1: 0, s2: 0, s3: 0 }\n;rng($randomFloat)'), '$randomFloat: the argument is not a random generator');
  assert.equal(panic(';var rng is { s0: 1, s1: 2, s2: 3 }\n;(rng, 6)($random)'), '$random: the argument is not a random generator');
  assert.equal(panic(';var rng is { s0: 1, s1: 2, s2: 3, s3: -4 }\n;(rng, 6)($random)'), '$random: the argument is not a random generator');
  assert.equal(panic(';[1, 2]($randomFloat)'), '$randomFloat: the argument is not a random generator');
  assert.equal(panic(';var rng is 1($newRandom)\n;(rng, 0)($random)'), '$random: n must be from 1 to 4294967296, got 0');
  assert.equal(panic(';var rng is 1($newRandom)\n;(rng, 2 ** 32 + 1)($random)'), '$random: n must be from 1 to 4294967296, got 4294967297');
  assert.equal(panic(';var rng is 1($newRandom)\n;(rng, 1.5)($random)'), 'type error: $random expects an int, got float');
  assert.equal(panic(';"x"($newRandom)'), 'type error: $newRandom expects an int, got string');
});
