// The standard library added in Drafts 0.2 to 0.5: $readKey and $waitKey on
// redirected input and the terminal (§12.2), $time, $elapsed, $sleep, random
// generators (§12.8), maps (§10.11) and sorting (§10.6).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { buildProgram, noZig, readRepo, run } from './helpers.js';

const skip = noZig;

// Runs source, which must compile cleanly, with input (a string or a Buffer):
// { out, err, code }.
function runMain(source, input = '') {
  const r = run(source, { input });
  assert.ok(!r.compileError, r.err);
  return { out: r.out, err: r.err, code: r.code };
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
  const r = runMain(readKeys, Buffer.isBuffer(input) ? input : Buffer.from(input, 'utf8'));
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
  const r = runMain(`
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
  const r = runMain(';($readKey)($print)\n;exit\n');
  assert.equal(r.code, 1);
  assert.match(r.err, /unhandled fail \(code 1\) from '\$readKey'/);
});

test('$time is the time in milliseconds', { skip }, () => {
  const before = Date.now();
  const r = runMain(';const t is ($time)\n;"{t($typeOf)} {t}"($print)\n;exit\n');
  const [type, t] = r.out.trim().split(' ');
  assert.equal(type, 'int');
  assert.ok(Number(t) >= before - 1000 && Number(t) <= Date.now() + 1000, t);
});

// The test vectors in §12.8.
const vectors = [...readRepo('docs/wavert-spec.md').matchAll(/^\| `(-?\d+)` \| ([\d, ]+) \| ([\d, ]+) \| ([\d ]+) \| ([\d.e-]+) \|$/gm)]
  .map((m) => ({ seed: m[1], state: m[2], words: m[3], dice: m[4], float: m[5] }));

test('random generators match the test vectors in the specification', { skip }, () => {
  assert.equal(vectors.length, 4);
  const r = runMain(`
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
  const r = runMain(`
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
  const r = runMain(`
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
    const r = runMain(`${code}\n;exit\n`);
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

// ---- Draft 0.3: maps (§10.11) and sorting (§10.6) ----

// The output of a program that must run without error.
function output(source) {
  const r = runMain(`${source}\n;exit\n`);
  assert.equal(r.err, '', source);
  assert.equal(r.code, 0, source);
  return r.out;
}
// The message of the panic that source ends with.
function panicOf(source) {
  const r = runMain(`${source}\n;exit\n`);
  assert.equal(r.code, 1, source);
  return /uncaught panic: (.*)/.exec(r.err)[1];
}
const lines = (...xs) => xs.map((x) => x + '\n').join('');

test('map literals, reading and writing', { skip }, () => {
  assert.equal(output(`
;enum Kind (rat, bat)
;const rat is "rat"
;var m is [rat: 1, 2: "two", true: null, Kind.bat: 4.5]
;"{m[rat]} {m[2]} {m[true]} {m[Kind.bat]} {m($length)}"($print)
;set m[rat] += 10
;set m[2] to "deux"
;set m[Kind.rat] to [1, 2]
;"{m[rat]} {m[2]} {m[Kind.rat][1]} {m($length)}"($print)
;[:]($length)($print)
;m($typeOf)($print)
// A key given twice keeps its first place and its last value.
;const k is 2
;const twice is [k: "a", 9: "b", 2: "c"]
;(twice($values), " ")($join)($print)`), lines('1 two null 4.5 4', '11 deux 2 5', '0', 'map', 'c b'));
});

test('$get, $has, $remove, $keys and $values', { skip }, () => {
  assert.equal(output(`
;var m is ["a": 1, "b": 2, "c": 3]
;const missing is (m, "z")($get)
    onfail { 0 }
;"{(m, "b")($get)} {missing} {(m, "a")($has)} {(m, "z")($has)}"($print)
;(m, "a")($remove)($print)
;const gone is (m, "a")($remove)
    onfail { -1 }
;gone($print)
// A key added again goes at the end.
;set m["a"] to 10
;(m($keys), " ")($join)($print)
;(m($values), " ")($join)($print)
// $keys and $values are snapshots: changing the map during a loop is safe.
;const keys is m($keys)
;var i is 0
;while (i < keys($length)) {
    ;(m, keys[i])($remove)
    ;set m["{keys[i]}{keys[i]}"] to i
    ;set i += 1
}
;(m($keys), " ")($join)($print)`), lines('2 0 true false', '1', '-1', 'b c a', '2 3 10', 'bb cc aa'));
});

test('$values shares the values themselves', { skip }, () => {
  assert.equal(output(`
;var hero is { hp: 10 }
;var units is [1: hero]
;const all is units($values)
;set all[0].hp -= 3
;units[1].hp($print)`), lines('7'));
});

test('maps follow the copy table', { skip }, () => {
  assert.equal(output(`
;const frozen is [1: "a"]
;var copy is frozen
;set copy[2] to "b"
;"{frozen($length)} {copy($length)} {copy == frozen}"($print)
;ref alias is copy
;set alias[3] to "c"
;copy($length)($print)
;const snapshot is copy
;set copy[4] to "d"
;"{snapshot($length)} {copy($length)}"($print)
;fn add(var m) { ;set m[5] to "e" }
;copy(add)
;copy($length)($print)`), lines('1 2 false', '3', '3 4', '5'));
});

test('map equality', { skip }, () => {
  assert.equal(output(`
;const a is [1: [2], 3: { x: 4 }]
;const b is [3: { x: 4 }, 1: [2]]
;"{a == b} {(a, b)($same)} {a == a}"($print)
;([1: 2], [1: 3])($same)($print)
;([1: 2], [1: 2, 3: 4])($same)($print)
;([1: 2], ["1": 2])($same)($print)`), lines('false true true', 'false', 'false', 'false'));
});

test('a map with many keys added and removed', { skip }, () => {
  assert.equal(output(`
;var m is [:]
;var i is 0
;while (i < 5000) {
    ;set m[i] to i * i
    ;set i += 1
}
;set i to 0
;while (i < 5000) {
    ;if (i % 7 != 0) { ;(m, i)($remove) }
    ;set i += 1
}
;set m[-1] to 0
;const keys is m($keys)
;"{m($length)} {m[4998]} {keys[0]} {keys[1]} {keys[-2]} {keys[-1]}"($print)`), lines('716 24980004 0 7 4998 -1'));
});

test('misusing a map panics', { skip }, () => {
  assert.equal(panicOf(';const m is [1: 2]\n;m[3]($print)'), 'no key 3 in map');
  assert.equal(panicOf(';const m is ["a": 2]\n;m["b"]($print)'), 'no key "b" in map');
  assert.equal(panicOf(';var m is ["a": 2]\n;set m["b"] += 1'), 'no key "b" in map');
  assert.equal(panicOf(';const m is [1: 2]\n;fn f(ref x) { ;set x[1] to 3 }\n;m(f)'), 'cannot modify a frozen map');
  assert.equal(panicOf(';const m is [1: 2]\n;fn f(ref x) { ;(x, 1)($remove) }\n;m(f)'), 'cannot modify a frozen map with $remove');
  assert.equal(panicOf(';var m is [1: 2]\n;set m[1.5] to 3'), 'type error: a map key must be an int, a string, a boolean, an enum member or a record, got float');
  assert.equal(panicOf(';const k is [1]\n;const m is [k: 2]'), 'type error: a map key must be an int, a string, a boolean, an enum member or a record, got array');
  assert.equal(panicOf(';const m is [1: 2]\n;(m, null)($has)'), 'type error: a map key must be an int, a string, a boolean, an enum member or a record, got null');
  assert.equal(panicOf(';[1, 2]($keys)'), 'type error: $keys expects a map, got array');
  assert.equal(panicOf(';[1: 2]($print)'), 'type error: cannot convert map to text; convert it explicitly');
  assert.equal(panicOf(';panic [1: "a", 2: [:]]'), '[1: "a", 2: [:]]');
  assert.equal(panicOf(';([1: 2] < [1: 2])($print)'), "type error: '<' cannot order map values");
  assert.equal(panicOf(';"x"[1]($print)'), 'type error: indexing needs an array, a map or a record, got string (use $at for a string)');
  const r = runMain(';const m is [1: 2]\n;(m, 5)($get)($print)\n;exit\n');
  assert.equal(r.code, 1);
  assert.match(r.err, /unhandled fail \(code 1\) from '\$get'/);
});

test('$sort orders numbers and strings', { skip }, () => {
  assert.equal(output(`
;([3, 1.5, -2, 10, 1, -0.5]($sort), " ")($join)($print)
// Equal keys keep their order: 1 before 1.0 before 1, and 0.0 before -0.0.
;([1.0, 2, 1, 0.0, -0.0, 1]($sort), " ")($join)($print)
// Code point order, so U+FFFD comes before an emoji (U+1F600).
;(["b", "a", "\\u{1F600}", "B", "\\u{FFFD}", "", "ab"]($sort), " ")($join)($print)
;[]($sort)($length)($print)
;(["only"]($sort), " ")($join)($print)`),
  lines('-2 -0.5 1 1.5 3 10', '0.0 -0.0 1.0 1 1 2', ' B a ab b � 😀', '0', 'only'));
});

test('$sort is stable on long arrays', { skip }, () => {
  // 500 records with keys 0-9: within each key, the original order must remain.
  assert.equal(output(`
;var items is []
;var keys is []
;var i is 0
;while (i < 500) {
    ;(items, i)($push)
    ;(keys, (i * 7) % 10)($push)
    ;set i += 1
}
;const sorted is (items, keys)($sortBy)
;var ok is true
;set i to 1
;while (i < 500) {
    ;const a is sorted[i - 1]
    ;const b is sorted[i]
    ;const ka is (a * 7) % 10
    ;const kb is (b * 7) % 10
    ;if (ka > kb || (ka == kb && a > b)) { ;set ok to false }
    ;set i += 1
}
;ok($print)
;"{sorted[0]} {sorted[1]} {sorted[-1]}"($print)`), lines('true', '0 10 497'));
});

test('$sortBy a field or a key array, and $reverse', { skip }, () => {
  assert.equal(output(`
;const ms is [{ n: "a", speed: 3 }, { n: "b", speed: 1 }, { n: "c", speed: 3 }, { n: "d", speed: 0 }]
;fn names(ref xs) {
    ;var out is []
    ;var i is 0
    ;while (i < xs($length)) { ;(out, xs[i].n)($push) ;set i += 1 }
    (out, " ")($join)
}
;(ms, "speed")($sortBy)(names)($print)
;(ms, "n")($sortBy)($reverse)(names)($print)
// Descending and stable: sort by negated keys.
;var neg is []
;var i is 0
;while (i < ms($length)) { ;(neg, -ms[i].speed)($push) ;set i += 1 }
;(ms, neg)($sortBy)(names)($print)
;((["x", "y", "z"], [2.5, 0, 1])($sortBy), " ")($join)($print)
;([1, 2, 3]($reverse), " ")($join)($print)
;const m is ["b": 1, "a": 2, "c": 3]
;(m($keys)($sort), " ")($join)($print)`), lines('d b a c', 'd c b a', 'a c b d', 'y z x', '3 2 1', 'a b c'));
});

test('sorted and reversed arrays are new and frozen', { skip }, () => {
  assert.equal(panicOf(';var xs is [2, 1]\n;const s is xs($sort)\n;fn f(ref a) { ;(a, 3)($push) }\n;s(f)'), 'cannot modify a frozen array with $push');
  assert.equal(output(';var xs is [2, 1]\n;const s is xs($sort)\n;(xs, 0)($push)\n;"{xs($length)} {s($length)}"($print)'), lines('3 2'));
});

test('misusing $sort and $sortBy panics', { skip }, () => {
  assert.equal(panicOf(';[1, "a"]($sort)'), 'type error: $sort cannot compare int with string');
  assert.equal(panicOf(';["a", 2.5]($sort)'), 'type error: $sort cannot compare string with float');
  assert.equal(panicOf(';[1, true]($sort)'), 'type error: $sort can sort only numbers and strings, got boolean');
  assert.equal(panicOf(';[[1], [0]]($sort)'), 'type error: $sort can sort only numbers and strings, got array');
  assert.equal(panicOf(';[1, nan, 0]($sort)'), '$sort cannot sort nan');
  assert.equal(panicOf(';enum E (a, b)\n;[E.b, E.a]($sort)'), 'type error: $sort can sort only numbers and strings, got E');
  assert.equal(panicOf(';([{ a: 1 }, { b: 2 }], "a")($sortBy)'), "no field 'a' in record");
  assert.equal(panicOf(';([{ a: 1 }, 2], "a")($sortBy)'), 'type error: member access needs a record, got int');
  assert.equal(panicOf(';([1, 2], [1])($sortBy)'), '$sortBy: there are 1 keys for 2 items');
  assert.equal(panicOf(';([1, 2], 5)($sortBy)'), 'type error: $sortBy expects a field name or an array of keys, got int');
  assert.equal(panicOf(';"abc"($sort)'), 'type error: $sort expects an array, got string');
  assert.equal(panicOf(';[1: 2]($reverse)'), 'type error: $reverse expects an array, got map');
});

// ---- Draft 0.4: the terminal (§12.2) ----
// The tests run programs with stdin and stdout piped, so neither is a terminal.

test('$terminal reports pipes as not terminals', { skip }, () => {
  assert.equal(output(`
;const t is ($terminal)
;"{t.input} {t.output}"($print)
;(t($fields), " ")($join)($print)
;t($typeOf)($print)
// Each call gives a new record, which a var binding can change.
;var u is ($terminal)
;set u.input to true
;"{u.input} {($terminal).input}"($print)`), lines('false false', 'input output', 'record', 'true false'));
});

test('$terminalSize fails when stdout is not a terminal', { skip }, () => {
  assert.equal(output(`
;const size is ($terminalSize)
    onfail (c) { { width: 80, height: 24, code: c } }
;"{size.width} {size.height} {size.code}"($print)`), lines('80 24 1'));
  const r = runMain(';($terminalSize).width($print)\n;exit\n');
  assert.equal(r.code, 1);
  assert.match(r.err, /unhandled fail \(code 1\) from '\$terminalSize'/);
});

// The code generator checks for a fail only after the stdlib functions in its
// list, so the list must name every function the specification says can fail:
// those whose table row says so, and $toInt and $toFloat (§9.5).
test('the code generator knows which stdlib functions fail', () => {
  const signalers = JSON.parse(/;const signalers is (\[[^\]]*\])/.exec(readRepo('wvc/codegen_c.wvt'))[1]);
  const fails = [...readRepo('docs/wavert-spec.md').matchAll(/^\| `\$(\w+)` \|.*\bFails\b/gm)].map((m) => m[1]);
  assert.deepEqual([...signalers].sort(), [...fails, 'toInt', 'toFloat'].sort());
});

// ---- Draft 0.5: waiting for a key, the clock and sleeping (§12.2, §12.8) ----

// Runs program with stdin a pipe that stays open until the test closes it.
// Each time the output so far ends with a line in steps, that step's action is
// called with the child's stdin. Resolves with the output and the exit code,
// and when the first output and the exit came (ms since the start).
function converse(program, steps = {}) {
  return new Promise((resolve, reject) => {
    const start = Date.now();
    const child = spawn(program, [], { stdio: ['pipe', 'pipe', 'pipe'] });
    let out = '', err = '', first = null;
    const done = new Set();
    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', (chunk) => {
      first ??= { chunk, at: Date.now() - start };
      out += chunk;
      for (const [line, action] of Object.entries(steps)) {
        if (!done.has(line) && out.includes(line + '\n')) { done.add(line); action(child.stdin); }
      }
    });
    child.stderr.on('data', (chunk) => { err += chunk; });
    child.on('error', reject);
    child.on('close', (code) => resolve({ out, err, code, first, end: Date.now() - start }));
  });
}

test('$waitKey times out with null, and returns keys as they come', { skip, timeout: 30000 }, async () => {
  const program = buildProgram(`
;var t is ($elapsed)
;const a is 50($waitKey)
;"none: {a == null} {($elapsed) - t >= 50}"($print)
;set t to ($elapsed)
;const b is 20000($waitKey)
;"got {b} {($elapsed) - t < 15000}"($print)
;const c is (-5)($waitKey)
;"negative: {c == null}"($print)
;const y is 20000($waitKey)
;const z is 0($waitKey)
;const n is 0($waitKey)
;"{y} {z} {n == null}"($print)
;const d is 20000($waitKey)
    onfail { "end" }
;d($print)
;exit
`);
  // Each line must appear while the program waits, so output is flushed before waiting.
  const r = await converse(program, {
    'none: true true': (stdin) => stdin.write('x'),
    'negative: true': (stdin) => stdin.write('yz'),
    'y z true': (stdin) => stdin.end(),
  });
  assert.deepEqual({ out: r.out, err: r.err, code: r.code },
    { out: 'none: true true\ngot x true\nnegative: true\ny z true\nend\n', err: '', code: 0 });
});

test('$waitKey on a file returns each key at once, then fails', { skip }, () => {
  const r = runMain(`
;var keys is []
;var more is true
;while (more) {
    ;const k is 10000($waitKey)
        onfail { ;set more to false
        "" }
    ;if (more) { ;(keys, k == null ? "null" : k)($push) }
}
;(keys, " ")($join)($print)
;exit
`, 'a\x1b[Bb');
  assert.deepEqual(r, { out: 'a down b\n', err: '', code: 0 });
  assert.match(runMain(';0($waitKey)($print)\n;exit\n').err, /unhandled fail \(code 1\) from '\$waitKey'/);
});

test('$elapsed starts near 0 and $sleep pauses', { skip }, () => {
  assert.equal(output(`
;const start is ($elapsed)
;"{start($typeOf)} {start >= 0 && start < 1000}"($print)
;100($sleep)
;(($elapsed) - start >= 100)($print)
;const t is ($elapsed)
;(-50)($sleep)
;0($sleep)
;(($elapsed) - t < 50)($print)
;var last is ($elapsed)
;var ok is true
;var i is 0
;while (i < 10000) {
    ;const now is ($elapsed)
    ;if (now < last) { ;set ok to false }
    ;set last to now
    ;set i += 1
}
;ok($print)`), lines('int true', 'true', 'true', 'true'));
});

test('$sleep shows what was written before pausing', { skip, timeout: 30000 }, async () => {
  const r = await converse(buildProgram(';"a"($write)\n;500($sleep)\n;"b"($print)\n;exit\n'));
  assert.equal(r.out, 'ab\n');
  assert.equal(r.first.chunk, 'a');
  assert.ok(r.end - r.first.at >= 300, `"a" came ${r.first.at} ms in, the exit ${r.end} ms in`);
});

test('$waitKey and $sleep need an int', { skip }, () => {
  assert.equal(panicOf(';"x"($sleep)'), 'type error: $sleep expects an int, got string');
  assert.equal(panicOf(';1.5($waitKey)'), 'type error: $waitKey expects an int, got float');
});

// ---- Draft 0.6: records as map keys (§10.11) ----

test('record keys compare by contents, in any field order', { skip }, () => {
  assert.equal(output(`
;enum Kind (orc, rat)
;var m is [{ x: 3, y: 4 }: "orc"]
;m[{ y: 4, x: 3 }]($print)
;set m[{ x: 3, y: 4 }] to "troll"
;set m[{ x: -1, y: 0 }] to "rat"
;set m[{ kind: Kind.orc, id: 7, name: "Grub", boss: true }] to "named"
;"{m($length)} {m[{ x: 3, y: 4 }]} {m[{ name: "Grub", id: 7, boss: true, kind: Kind.orc }]}"($print)
// Names are part of the key, and so is every field.
;([(m, { col: 3, row: 4 })($has), (m, { x: 3, y: 4, z: 0 })($has), (m, { x: 3 })($has), (m, 3)($has)], " ")($join)($print)
// Two records built alike are equal keys, but == stays reference equality.
;const a is { x: 3, y: 4 }
;const b is { x: 3, y: 4 }
;"{a == b} {(m, a)($has)} {(m, b)($has)}"($print)
;const keys is m($keys)
;"{keys[0].x},{keys[0].y} {keys[1].x},{keys[1].y} {keys[0]($typeOf)}"($print)
;(m, { y: 0, x: -1 })($remove)($print)
;m($length)($print)`), lines('orc', '3 troll named', 'false false false false', 'false true true', '3,4 -1,0 record', 'rat', '2'));
});

test('a mutable record key is snapshotted when it is added', { skip }, () => {
  assert.equal(output(`
;var pos is { x: 1, y: 1 }
;var m is [:]
;set m[pos] to "here"
;set pos.x to 2
;"{(m, { x: 1, y: 1 })($has)} {(m, pos)($has)} {m($keys)[0].x}"($print)
// The stored key is frozen; setting an existing key keeps it.
;set m[{ x: 1, y: 1 }] to "still"
;var stored is m($keys)[0]
;set stored.x to 9
;"{m[{ x: 1, y: 1 }]} {m($length)}"($print)
// A map literal and $same agree on record keys.
;const p is [{ x: 1 }: "a", { x: 1 }: "b"]
;"{p($length)} {p[{ x: 1 }]} {([{ x: 1, y: 2 }: 0], [{ y: 2, x: 1 }: 0])($same)}"($print)`),
  lines('true false 1', 'still 1', '1 b true'));
});

test('many record keys added and removed', { skip }, () => {
  assert.equal(output(`
;var m is [:]
;var y is 0
;while (y < 60) {
    ;var x is 0
    ;while (x < 60) {
        ;set m[{ x: x, y: y }] to x * y
        ;set x += 1
    }
    ;set y += 1
}
;set y to 0
;while (y < 60) {
    ;if (y % 2 == 1) {
        ;var x is 0
        ;while (x < 60) {
            ;(m, { y: y, x: x })($remove)
            ;set x += 1
        }
    }
    ;set y += 1
}
;"{m($length)} {m[{ x: 59, y: 58 }]} {(m, { x: 0, y: 1 })($has)}"($print)`), lines('1800 3422 false'));
});

test('record keys must be flat', { skip }, () => {
  assert.equal(panicOf(';var m is [:]\n;set m[{ x: 1.5 }] to 0'), "type error: field 'x' of a map key must be an int, a string, a boolean or an enum member, got float");
  assert.equal(panicOf(';var m is [:]\n;set m[{ at: { x: 1 } }] to 0'), "type error: field 'at' of a map key must be an int, a string, a boolean or an enum member, got record");
  assert.equal(panicOf(';const m is [{ a: [1] }: 0]'), "type error: field 'a' of a map key must be an int, a string, a boolean or an enum member, got array");
  assert.equal(panicOf(';const m is [1: 2]\n;(m, { x: null })($get)'), "type error: field 'x' of a map key must be an int, a string, a boolean or an enum member, got null");
  assert.equal(panicOf(';const m is [{ x: 1 }: 2]\n;m[{ x: 2 }]($print)'), 'no key {x: 2} in map');
});
