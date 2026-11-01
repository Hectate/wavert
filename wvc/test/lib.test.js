// The standard library written in Wavert (lib/) and the '$lib/' import prefix
// that reaches it from any program (§13.1, §15).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { diagnostics, exe, noZig, run, runtimeC, tempDir, wvcExe } from './helpers.js';

const skip = noZig;
const lines = (...xs) => xs.map((x) => x + '\n').join('');

// The output of a program using the repository's lib/, which must run cleanly.
function out(source) {
  const r = run(source + '\n;exit\n');
  assert.deepEqual({ err: r.err, code: r.code }, { err: '', code: 0 }, source);
  return r.out;
}
// The message of the panic that a program using lib/ ends with.
function panicOf(source) {
  const r = run(source + '\n;exit\n');
  assert.equal(r.code, 1, source);
  return /uncaught panic: (.*)/.exec(r.err)[1];
}

// A stand-in standard library: a module that imports its neighbour relatively.
const stdlib = {
  'std/util.wvt': ';import h from "helper.wvt"\n;export fn twice(x) { x(h.double) }\n;end\n',
  'std/helper.wvt': ';export fn double(x) { x * 2 }\n;end\n',
};

test('$lib/ imports a standard library module from anywhere', { skip }, () => {
  const r = run(';import u from "$lib/util.wvt"\n;21(u.twice)($print)\n;exit\n', { files: stdlib, lib: 'std' });
  assert.deepEqual({ out: r.out, err: r.err, code: r.code }, { out: '42\n', err: '', code: 0 });
  // The '$' in an import's path gets no warning; one in another string still does.
  const d = diagnostics(';import u from "$lib/util.wvt"\n;"$x"($print)\n;exit\n', stdlib, { lib: 'std' });
  assert.match(d, /^main\.wvt:2:3: warning: did you mean \{x\}\?/);
  assert.equal(d.match(/warning/g).length, 1);
});

test('$lib/ is the only reserved prefix, and stays inside lib/', { skip }, () => {
  const first = (source) => diagnostics(source, stdlib, { lib: 'std' }).split('\n').slice(0, 2).join(' | ');
  assert.equal(first(';import u from "$std/util.wvt"\n;exit\n'),
    "main.wvt:1:16: error: an import path that starts with '$' must start with '$lib/', the standard library |   hint: a file of your own is imported by a relative path, such as \"lib.wvt\"");
  assert.equal(first(';import u from "$lib/../main.wvt"\n;exit\n'), "main.wvt:1:16: error: a '$lib/' path cannot leave lib/ with '..' |     ;import u from \"$lib/../main.wvt\"");
  assert.match(first(';import u from "$lib/nope.wvt"\n;exit\n'), /^main\.wvt:1:16: error: the standard library has no 'nope\.wvt' \(.*std\/nope\.wvt\)/);
  assert.match(diagnostics(';import u from "$lib/util.wvt"\n;exit\n', {}, { lib: 'no-such-folder' }),
    /^main\.wvt:1:16: error: cannot find the standard library: no lib\/ folder in .*no-such-folder/);
});

test('wvc finds lib/ from where it is, not from the current directory', { skip }, () => {
  // Laid out like the repository: wvc one level down, lib/ beside that. The
  // current directory has a lib/ of its own, which must not be used.
  const dir = tempDir();
  mkdirSync(join(dir, 'tool'));
  mkdirSync(join(dir, 'lib'));
  copyFileSync(wvcExe(), join(dir, 'tool', `wvc${exe}`));
  writeFileSync(join(dir, 'lib/which.wvt'), ';export const name is "standard"\n;end\n');
  const cwd = tempDir();
  mkdirSync(join(cwd, 'lib'));
  writeFileSync(join(cwd, 'lib/which.wvt'), ';export const name is "impostor"\n;end\n');
  writeFileSync(join(cwd, 'main.wvt'), ';import w from "$lib/which.wvt"\n;w.name($print)\n;exit\n');
  const c = spawnSync(join(dir, 'tool', `wvc${exe}`), ['main.wvt', '-o', 'main.c', '--runtime', runtimeC], { cwd, encoding: 'utf8' });
  assert.deepEqual({ err: c.stderr, code: c.status }, { err: '', code: 0 });
  // The C that wvc wrote holds the standard library's string, not the impostor's.
  const text = readFileSync(join(cwd, 'main.c'), 'utf8');
  assert.ok(text.includes('"standard"') && !text.includes('"impostor"'));
  // --lib names another folder.
  const r = run(';import w from "$lib/which.wvt"\n;w.name($print)\n;exit\n', { files: { 'l/which.wvt': ';export const name is "given"\n;end\n' }, lib: 'l' });
  assert.equal(r.out, 'given\n');
});

// ---- math.wvt ----

test('math: abs, sign, min, max and clamp', { skip }, () => {
  assert.equal(out(`;import math from "$lib/math.wvt"
;"{(-7)(math.abs)} {2.5(math.abs)} {(-2.5)(math.sign)} {0(math.sign)} {9(math.sign)} {nan(math.sign)}"($print)
;"{(3, 9)(math.min)} {(3, 9)(math.max)} {(1, 1.0)(math.min)} {(7, 0, 5)(math.clamp)} {(-1, 0, 5)(math.clamp)} {(3, 0, 5)(math.clamp)}"($print)
// The bounds may come in either order.
;"{(7, 5, 0)(math.clamp)} {(-1, 5, 0)(math.clamp)} {(3, 5, 0)(math.clamp)}"($print)`),
  lines('7 2.5 -1 0 1 0', '3 9 1 5 0 3', '5 0 3'));
});

test('math: div is floored and exact, isqrt, gcd and lcm', { skip }, () => {
  assert.equal(out(`;import math from "$lib/math.wvt"
;"{(7, 2)(math.div)} {(-7, 2)(math.div)} {(7, -2)(math.div)} {(-7, -2)(math.div)} {(6, 3)(math.div)}"($print)
// Near the largest int, where (a / b)($floor) would round.
;"{(9007199254740991, 2)(math.div)} {(9007199254740991, 3)(math.div)}"($print)
;"{0(math.isqrt)} {1(math.isqrt)} {99(math.isqrt)} {100(math.isqrt)} {(94906265 * 94906265)(math.isqrt)} {9007199254740991(math.isqrt)}"($print)
;"{(12, 18)(math.gcd)} {(-12, 18)(math.gcd)} {(0, 5)(math.gcd)} {(0, 0)(math.gcd)} {(4, 6)(math.lcm)} {(-4, 6)(math.lcm)} {(0, 6)(math.lcm)}"($print)`),
  lines('3 -4 -4 3 2', '4503599627370495 3002399751580330', '0 1 9 10 94906265 94906265', '6 6 5 0 12 12 0'));
  assert.equal(panicOf(';import math from "$lib/math.wvt"\n;(1, 0)(math.div)'), 'math.div: division by zero');
  assert.equal(panicOf(';import math from "$lib/math.wvt"\n;(-1)(math.isqrt)'), 'math.isqrt: -1 is negative');
});

test('math: sum, minOf, maxOf and lerp', { skip }, () => {
  assert.equal(out(`;import math from "$lib/math.wvt"
;"{[1, 2, 3.5](math.sum)} {[](math.sum)} {[3, -1, 2, -1](math.minOf)} {[3, -1, 7, 2](math.maxOf)}"($print)
;const none is [](math.maxOf)
    onfail { "none" }
;"{none} {(0, 10, 0.25)(math.lerp)} {(2, 4, 1.5)(math.lerp)} {(10, 0, 0.5)(math.lerp)}"($print)`),
  lines('6.5 0 -1 7', 'none 2.5 5.0 5.0'));
});

// ---- grid.wvt ----

// A Wavert function that shows an area as "x,y x,y ...".
const showArea = `;fn show(ref area) {
    ;var parts is []
    ;var i is 0
    ;while (i < area($length)) {
        ;(parts, "{area[i].x},{area[i].y}")($push)
        ;set i += 1
    }
    (parts, " ")($join)
}`;
const gridProgram = (body) => `;import grid from "$lib/grid.wvt"\n${showArea}\n${body}`;

test('grid: cells, positions and indexes', { skip }, () => {
  assert.equal(out(gridProgram(`
;var map is (4, 3, ".")(grid.new)
;(map, 1, 2, "#")(grid.put)
;(map, (3, 0)(grid.pos), "@")(grid.putAt)
;(map(grid.rows), "|")($join)($print)
;"{(map, 1, 2)(grid.get)} {(map, (3, 0)(grid.pos))(grid.at)} {(map, 3, 2)(grid.inside)} {(map, 4, 0)(grid.inside)} {(map, -1, 0)(grid.inside)}"($print)
;const p is (map, 9)(grid.posOf)
;"{(map, 1, 2)(grid.index)} {p.x},{p.y} {map.width}x{map.height} {(0, 0, 0)(grid.new).cells($length)}"($print)`)),
  lines('...@|....|.#..', '# @ true false false', '9 1,2 4x3 0'));
  assert.equal(panicOf(gridProgram(';(-1, 3, 0)(grid.new)')), 'grid.new: the size -1 x 3 is negative');
});

test('grid: off the grid, one cell fails and the caller decides', { skip }, () => {
  assert.equal(out(gridProgram(`
;var map is (4, 3, ".")(grid.new)
;const edge is (map, 4, 0)(grid.get)
    onfail { "#" }
;const corner is (map, (-1, -1)(grid.pos))(grid.at)
    onfail { "#" }
;const i is (map, 0, 3)(grid.index)
    onfail { -1 }
;const p is (map, 12)(grid.posOf)
    onfail { null }
;"{edge} {corner} {i} {p}"($print)
// Writing off the grid changes nothing.
;(map, 9, 9, "@")(grid.put)
    onfail { null }
;(map, (-1, 0)(grid.pos), "@")(grid.putAt)
    onfail { null }
;(map(grid.rows), "|")($join)($print)`)),
  lines('# # -1 null', '....|....|....'));
  const r = run(gridProgram(';var g is (4, 3, 0)(grid.new)\n;(g, 4, 0)(grid.get)\n;exit\n'));
  assert.equal(r.code, 1);
  assert.match(r.err, /unhandled fail \(code 1\) from 'get'/);
});

test('grid: copies never change each other', { skip }, () => {
  assert.equal(out(gridProgram(`
;const base is (3, 1, ".")(grid.new)
// Two var grids made from one const grid: the first put copies the cells.
;var a is base
;var b is base
;(a, 0, 0, "a")(grid.put)
;(b, 2, 0, "b")(grid.put)
;"{a(grid.rows)[0]} {b(grid.rows)[0]} {base(grid.rows)[0]}"($print)
// copy is a true snapshot, and a second grid to change separately.
;const snap is a(grid.copy)
;var c is a(grid.copy)
;(a, 1, 0, "!")(grid.put)
;(c, 2, 0, "c")(grid.put)
;"{a(grid.rows)[0]} {snap(grid.rows)[0]} {c(grid.rows)[0]}"($print)`)),
  lines('a.. ..b ...', 'a!. a.. a.c'));
  assert.equal(panicOf(gridProgram(';const g is (2, 2, 0)(grid.new)\n;fn change(ref h) { ;(h, 0, 0, 1)(grid.put) }\n;g(change)')),
    'cannot modify a field of a frozen record');
});

test('grid: neighbors, distances and lines', { skip }, () => {
  assert.equal(out(gridProgram(`
;const p is (5, 5)(grid.pos)
;p(grid.neighbors4)(show)($print)
;p(grid.neighbors8)(show)($print)
;"{(p, (8, 1)(grid.pos))(grid.manhattan)} {(p, (8, 1)(grid.pos))(grid.chebyshev)}"($print)
;((0, 0)(grid.pos), (5, 2)(grid.pos))(grid.line)(show)($print)
;((2, 3)(grid.pos), (2, 0)(grid.pos))(grid.line)(show)($print)
;((1, 1)(grid.pos), (1, 1)(grid.pos))(grid.line)(show)($print)`)),
  lines('5,4 6,5 5,6 4,5', '5,4 6,4 6,5 6,6 5,6 4,6 4,5 4,4', '7 4', '0,0 1,0 2,1 3,1 4,2 5,2', '2,3 2,2 2,1 2,0', '1,1'));
});

test('grid: area shapes', { skip }, () => {
  assert.equal(out(gridProgram(`
;const c is (0, 0)(grid.pos)
;((2, 1)(grid.pos), (0, 0)(grid.pos))(grid.rect)(show)($print)
;(c, 1)(grid.square)(show)($print)
;(c, 1)(grid.ring)(show)($print)
;(c, 1)(grid.diamond)(show)($print)
;(c, 2)(grid.circle)(show)($print)
;"{(c, 0)(grid.ring)(show)} {(c, 0)(grid.circle)(show)} [{(c, -1)(grid.square)(show)}]"($print)`)),
  lines('0,0 1,0 2,0 0,1 1,1 2,1', '-1,-1 0,-1 1,-1 -1,0 0,0 1,0 -1,1 0,1 1,1', '-1,-1 0,-1 1,-1 -1,0 1,0 -1,1 0,1 1,1',
    '0,-1 -1,0 0,0 1,0 0,1',
    '-1,-2 0,-2 1,-2 -2,-1 -1,-1 0,-1 1,-1 2,-1 -2,0 -1,0 0,0 1,0 2,0 -2,1 -1,1 0,1 1,1 2,1 -1,2 0,2 1,2', '0,0 0,0 []'));
});

test('grid: reading and writing through areas, which use only what is on the grid', { skip }, () => {
  assert.equal(out(gridProgram(`
;var map is (5, 3, ".")(grid.new)
// A blast at the corner reaches off the grid: fill writes the part on it.
;const blast is ((0, 0)(grid.pos), 1)(grid.square)
;(map, blast)(grid.within)(show)($print)
;(map, blast, "*")(grid.fill)
;(map, 4, 2, "@")(grid.put)
;(map(grid.rows), "|")($join)($print)
// valuesAt and cellsAt give the same cells, in the same order.
;const box is ((3, 1)(grid.pos), (5, 2)(grid.pos))(grid.rect)
;((map, box)(grid.valuesAt), "")($join)($print)
;const cells is (map, box)(grid.cellsAt)
;var parts is []
;var i is 0
;while (i < cells($length)) {
    ;(parts, "{cells[i].pos.x},{cells[i].pos.y}={cells[i].value}")($push)
    ;set i += 1
}
;(parts, " ")($join)($print)
;"{(map, [(9, 9)(grid.pos)])(grid.valuesAt)($length)} {(map, [])(grid.cellsAt)($length)}"($print)
;(map, [(9, 9)(grid.pos)], "!")(grid.fill)
;(map(grid.rows), "|")($join)($print)`)),
  lines('0,0 1,0 0,1 1,1', '**...|**...|....@', '...@', '3,1=. 4,1=. 3,2=. 4,2=@', '0 0', '**...|**...|....@'));
});

test('grid: combining areas', { skip }, () => {
  assert.equal(out(gridProgram(`
;const a is ((0, 0)(grid.pos), (2, 0)(grid.pos))(grid.rect)
;const b is [(3, 0)(grid.pos), (1, 0)(grid.pos), (2, 0)(grid.pos), (3, 0)(grid.pos)]
;(a, b)(grid.union)(show)($print)
;(a, b)(grid.intersect)(show)($print)
;(a, b)(grid.minus)(show)($print)
;(a, b)(grid.exclude)(show)($print)
;(b, a)(grid.union)(show)($print)
// Positions compare by contents, and every position appears once.
;const dup is [(1, 1)(grid.pos), { y: 1, x: 1 }, (1, 1)(grid.pos)]
;"{(dup, [])(grid.union)(show)} [{(a, [])(grid.intersect)(show)}] [{(a, a)(grid.exclude)(show)}]"($print)`)),
  lines('0,0 1,0 2,0 3,0', '1,0 2,0', '0,0', '0,0 3,0', '3,0 1,0 2,0 0,0', '1,1 [] []'));
});

// ---- dice.wvt ----

const diceProgram = (body) => `;import dice from "$lib/dice.wvt"\n;var rng is 2026($newRandom)\n${body}`;

test('dice: rolls stay in range and cover it', { skip }, () => {
  // Over many rolls, every value comes up and none falls outside.
  assert.equal(out(diceProgram(`
;fn spread(lo, hi, ref values) {
    ;var seen is [:]
    ;var i is 0
    ;while (i < values($length)) {
        ;if (values[i] < lo || values[i] > hi) { ;return "out of range: {values[i]}" }
        ;set seen[values[i]] to true
        ;set i += 1
    }
    seen($length) == hi - lo + 1 ? "ok" : "missed some: {seen($length)}"
}
;var d6 is []
;var d3d6 is []
;var mid is []
;var swapped is []
;var i is 0
;while (i < 2000) {
    ;(d6, (rng, 6)(dice.roll))($push)
    ;(d3d6, (rng, 3, 6)(dice.dice))($push)
    ;(mid, (rng, -2, 2)(dice.between))($push)
    ;(swapped, (rng, 2, -2)(dice.between))($push)
    ;set i += 1
}
;"{(1, 6, d6)(spread)} {(3, 18, d3d6)(spread)} {(-2, 2, mid)(spread)} {(-2, 2, swapped)(spread)} {(rng, 0, 6)(dice.dice)} {(rng, 4, 4)(dice.between)}"($print)`)),
  lines('ok ok ok ok 0 4'));
  assert.equal(panicOf(diceProgram(';(rng, 0)(dice.roll)')), 'dice.roll: a die needs 1 or more sides, got 0');
  assert.equal(panicOf(diceProgram(';(rng, -1, 6)(dice.dice)')), 'dice.dice: the count of dice is negative, got -1');
});

test('dice: chance, pick, shuffled and weighted', { skip }, () => {
  assert.equal(out(diceProgram(`
;var hits is 0
;var never is 0
;var always is 0
;var w is [0, 0, 0]
;var wf is [0, 0]
;var i is 0
;while (i < 4000) {
    ;if ((rng, 25)(dice.chance)) { ;set hits += 1 }
    ;if ((rng, 0)(dice.chance)) { ;set never += 1 }
    ;if ((rng, 100)(dice.chance)) { ;set always += 1 }
    ;set w[(rng, [1, 2, 0, 1])(dice.weighted) % 3] += 1
    ;set wf[(rng, [0.0, 0.25, 0.75])(dice.weighted) - 1] += 1
    ;set i += 1
}
;"{hits > 900 && hits < 1100} {never} {always}"($print)
// [1, 2, 0, 1]: index 0 and 3 share a count, 1 is twice either, 2 never comes.
;"{w[0] > 1800 && w[0] < 2200} {w[1] > 1800 && w[1] < 2200} {w[2]}"($print)
;"{wf[0] > 850 && wf[0] < 1150}"($print)
;const xs is [1, 2, 3, 4, 5]
;const s is (rng, xs)(dice.shuffled)
;"{(s($sort), " ")($join)} {(xs, " ")($join)} {(rng, ["only"])(dice.pick)}"($print)
;const none is (rng, [])(dice.pick)
    onfail { "none" }
;none($print)`)),
  lines('true 0 4000', 'true true 0', 'true', '1 2 3 4 5 1 2 3 4 5 only', 'none'));
  assert.equal(panicOf(diceProgram(';(rng, [0, 0])(dice.weighted)')), 'dice.weighted: the weights add up to 0');
  assert.equal(panicOf(diceProgram(';(rng, [1, -1])(dice.weighted)')), 'dice.weighted: weight 1 is negative, got -1');
});

test('dice: notation, check and exact odds', { skip }, () => {
  assert.equal(out(diceProgram(`
;fn ok(text) {
    ;const r is (rng, text)(dice.rollText)
        onfail { "bad" }
    r
}
;var inRange is true
;var i is 0
;while (i < 500) {
    ;const r is (rng, "2d6+1")(dice.rollText)
    ;const s is (rng, "d4 - 1")(dice.rollText)
    ;if (r < 3 || r > 13 || s < 0 || s > 3) { ;set inRange to false }
    ;set i += 1
}
;"{inRange} {"4"(ok)} {"0d6+2"(ok)} {"2D1"(ok)}"($print)
;([""(ok), "2d"(ok), "d"(ok), "2x6"(ok), "2d6+"(ok), "2d0"(ok), "-1d6"(ok), "2d6+1+1"(ok), "1.5d6"(ok)], " ")($join)($print)
;const c is (rng, 3, 1, 3)(dice.check)
;const f is (rng, 1, 1, 2)(dice.check)
;"{c.total} {c.success} {f.total} {f.success}"($print)
;const o is (2, 6)(dice.odds)
;"{o($length)} {o[0]} {o[1]} {o[2] * 36} {o[7] * 6} {o[12] * 36} {(0, 6)(dice.odds)[0]}"($print)`)),
  lines('true 4 2 2', 'bad bad bad bad bad bad bad bad bad', '3 true 1 false', '13 0.0 0.0 1.0 1.0 1.0 1.0'));
  assert.equal(out(diceProgram(`
;fn p(count, sides, target) { (count, sides, target)(dice.chanceAtLeast)(dice.percent) }
;"{(2, 6, 8)(dice.chanceAtLeast)} {(2, 6, 8)(p)} {(2, 6, 2)(p)} {(2, 6, 13)(p)} {(2, 6, 12)(p)}"($print)
// Near-certain and near-impossible chances never show as 100% or 0%.
;"{(3, 6, 4)(p)} {(20, 6, 21)(p)} {(3, 6, 18)(p)} {(4, 6, 24)(p)}"($print)
;"{("2d6+1", 9)(dice.chanceText)(dice.percent)} {("d20", 11)(dice.chanceText)(dice.percent)} {("5", 5)(dice.chanceText)}"($print)
;const bad is ("2d", 3)(dice.chanceText)
    onfail { "bad" }
;"{bad} {0.0(dice.percent)} {1.0(dice.percent)} {0.004(dice.percent)} {0.995(dice.percent)} {0.5(dice.percent)}"($print)`)),
  lines('0.4166666666666667 42% 100% 0% 3%', '>99% >99% <1% <1%', '42% 50% 1.0', 'bad 0% 100% <1% >99% 50%'));
  assert.equal(panicOf(diceProgram(';(2, 0)(dice.odds)')), 'dice.odds: cannot roll 2d0');
});

// ---- text.wvt ----

const textProgram = (body) => `;import text from "$lib/text.wvt"\n${body}`;

test('text: repeat and padding', { skip }, () => {
  assert.equal(out(textProgram(`
;"[{("ab", 3)(text.repeat)}] [{("ab", 0)(text.repeat)}] [{("ab", -2)(text.repeat)}] [{(7, 2)(text.repeat)}]"($print)
;"[{("HP", 6)(text.padRight)}{(42, 4)(text.padLeft)}] [{("ab", 7)(text.center)}] [{("ab", 6)(text.center)}]"($print)
// Already as wide as asked, or wider: unchanged.
;"[{("toolong", 3)(text.padLeft)}] [{("toolong", 3)(text.padRight)}] [{("toolong", 3)(text.center)}] [{("é😀", 4)(text.padLeft)}]"($print)`)),
  lines('[ababab] [] [] [77]', '[HP      42] [  ab   ] [  ab  ]', '[toolong] [toolong] [toolong] [  é😀]'));
});

test('text: trim, searching and replacing', { skip }, () => {
  assert.equal(out(textProgram(`
;"[{"  hi there \\t\\r\\n"(text.trim)}] [{" \\n "(text.trim)}] [{"x"(text.trim)}]"($print)
;"{("north gate", "north")(text.startsWith)} {("north gate", "gate")(text.startsWith)} {("north gate", "gate")(text.endsWith)} {("x", "")(text.endsWith)} {("", "x")(text.startsWith)}"($print)
;const missing is ("abc", "z")(text.indexOf)
    onfail { "none" }
;"{("north gate", "gate")(text.indexOf)} {("aaa", "a")(text.indexOf)} {("abc", "")(text.indexOf)} {missing} {("a😀b", "b")(text.indexOf)}"($print)
;"{("north gate", "th g")(text.contains)} {("north gate", "x")(text.contains)}"($print)
;"{("a-b--c", "-", "+")(text.replace)} {("abc", "", "+")(text.replace)} {("aaa", "aa", "b")(text.replace)} {(1010, 0, 2)(text.replace)}"($print)`)),
  lines('[hi there] [] [x]', 'true false true true false', '6 0 0 none 2', 'true false', 'a+b++c abc ba 1212'));
});

test('text: ASCII case', { skip }, () => {
  assert.equal(out(textProgram(`
;"{"Hello, Wörld! az AZ @[\`{{"(text.toUpper)}"($print)
;"{"Hello, WÖRLD! az AZ @[\`{{"(text.toLower)}"($print)`)),
  lines('HELLO, WöRLD! AZ AZ @[`{', 'hello, wÖrld! az az @[`{'));
});

test('text: wrap', { skip }, () => {
  const wrap = (s, w) => out(textProgram(`;((${JSON.stringify(s)}, ${w})(text.wrap), "|")($join)($print)`)).trimEnd();
  assert.equal(wrap('the quick brown fox jumps over the lazy dog', 10), 'the quick|brown fox|jumps over|the lazy|dog');
  assert.equal(wrap('abcdefghijkl and more', 5), 'abcde|fghij|kl|and|more');
  assert.equal(wrap('first\n\nthird   with  spaces', 20), 'first||third with spaces');
  assert.equal(wrap('one\r\ntwo', 9), 'one|two');
  assert.equal(wrap('', 5), '');
  assert.equal(wrap('abc', 0), 'a|b|c');
  assert.equal(wrap('exactly ten', 11), 'exactly ten');
});

// ---- arrays.wvt ----

const arraysProgram = (body) => `;import arrays from "$lib/arrays.wvt"\n${body}`;

test('arrays: finding and counting, by contents', { skip }, () => {
  assert.equal(out(arraysProgram(`
;const recs is [{ x: 1 }, { x: 2 }, { x: 1 }, [1, 2]]
;const missing is (recs, { x: 9 })(arrays.indexOf)
    onfail { "none" }
;"{(recs, { x: 2 })(arrays.indexOf)} {(recs, [1, 2])(arrays.indexOf)} {missing} {(recs, { x: 1 })(arrays.count)} {([], 1)(arrays.count)}"($print)
;"{(recs, { x: 1 })(arrays.contains)} {(recs, { x: 9 })(arrays.contains)} {([1, 1.0], 1)(arrays.count)}"($print)`)),
  lines('1 3 none 2 0', 'true false 2'));
});

test('arrays: range, filled and concat', { skip }, () => {
  assert.equal(out(arraysProgram(`
;fn show(ref xs) { "[{(xs, ",")($join)}]" }
;"{(0, 5)(arrays.range)(show)} {(3, 0)(arrays.range)(show)} {(2, 2)(arrays.range)(show)} {(-2, 1)(arrays.range)(show)}"($print)
;"{(3, "x")(arrays.filled)(show)} {(0, "x")(arrays.filled)(show)} {(-1, "x")(arrays.filled)(show)}"($print)
;const a is [1, 2]
;const both is (a, [3])(arrays.concat)
;"{both(show)} {a(show)} {([], [])(arrays.concat)(show)}"($print)`)),
  lines('[0,1,2,3,4] [3,2,1] [] [-2,-1,0]', '[x,x,x] [] []', '[1,2,3] [1,2] []'));
});

test('arrays: removeAt and insertAt', { skip }, () => {
  assert.equal(out(arraysProgram(`
;var pack is ["sword", "rope", "lamp"]
;const a is (pack, 1)(arrays.removeAt)
;const b is (pack, -1)(arrays.removeAt)
;const c is (pack, 5)(arrays.removeAt)
    onfail { "none" }
;const d is (pack, -2)(arrays.removeAt)
    onfail { "none" }
;"{a} {b} {c} {d} {(pack, ",")($join)}"($print)
;(pack, 0, "map")(arrays.insertAt)
;(pack, 2, "end")(arrays.insertAt)
;(pack, -1, "mid")(arrays.insertAt)
;(pack, 9, "no")(arrays.insertAt)
    onfail { null }
;(pack, -9, "no")(arrays.insertAt)
    onfail { null }
;(pack, ",")($join)($print)
;var empty is []
;(empty, 0, "only")(arrays.insertAt)
;"{(empty, 0)(arrays.removeAt)} {empty($length)}"($print)`)),
  lines('rope lamp none none sword', 'map,sword,mid,end', 'only 0'));
  assert.equal(panicOf(arraysProgram(';const xs is [1, 2]\n;fn f(ref a) { ;(a, 0)(arrays.removeAt) }\n;xs(f)')), 'cannot modify a frozen array');
});

// ---- maps.wvt ----

test('maps: getOr, add and pushTo', { skip }, () => {
  assert.equal(out(`;import maps from "$lib/maps.wvt"
;import grid from "$lib/grid.wvt"
;var kills is [:]
;(kills, "orc", 1)(maps.add)
;const n is (kills, "orc", 2)(maps.add)
;(kills, "rat", -1)(maps.add)
;"{n} {(kills, "rat", 0)(maps.getOr)} {(kills, "bat", 0)(maps.getOr)} {(kills($keys), ",")($join)}"($print)
;var items is [:]
;(items, (3, 4)(grid.pos), "coin")(maps.pushTo)
;(items, (3, 4)(grid.pos), "gem")(maps.pushTo)
;(items, (0, 0)(grid.pos), "rock")(maps.pushTo)
;"{(items[(3, 4)(grid.pos)], ",")($join)} {(items[(0, 0)(grid.pos)], ",")($join)} {items($length)}"($print)
// A frozen array already at the key is copied before the push.
;var piles is ["a": ["x"]]
;const before is piles["a"]
;(piles, "a", "y")(maps.pushTo)
;"{(piles["a"], ",")($join)} {(before, ",")($join)}"($print)`),
  lines('3 -1 0 orc,rat', 'coin,gem rock 2', 'x,y x'));
});

// ---- view.wvt ----

// A 6 x 3 room: walls along the top, a door at 3,2, items and actors on it.
const viewProgram = (body) => `;import grid from "$lib/grid.wvt"
;import view from "$lib/view.wvt"
;import maps from "$lib/maps.wvt"
;enum Terrain (floor, wall, door)
;const tiles is [Terrain.floor: ".", Terrain.wall: "#", Terrain.door: "+"]
;var terrain is (6, 3, Terrain.floor)(grid.new)
;(terrain, ((0, 0)(grid.pos), (5, 0)(grid.pos))(grid.rect), Terrain.wall)(grid.fill)
;(terrain, 3, 2, Terrain.door)(grid.put)
;var items is [:]
;var actors is [:]
;const whole is { x: 0, y: 0, width: 6, height: 3 }
;const all is ((0, 0)(grid.pos), (5, 2)(grid.pos))(grid.rect)
;const left is ((0, 0)(grid.pos), (2, 2)(grid.pos))(grid.rect)
;fn layers() { [(terrain, tiles)(view.gridLayer), (items, [:])(view.mapLayer), (actors, [:])(view.mapLayer)] }
;fn draw(window, ref visible, ref memory) { (((layers), window, visible, memory, tiles, 0)(view.rows), "|")($join)($print) }
${body}`;

test('view: layers, topmost first, with glyphs for each kind of value', { skip }, () => {
  assert.equal(out(viewProgram(`
;(whole, null, null)(draw)
// A pile shows its last item; an actor record its glyph field; a string itself.
;(items, (1, 1)(grid.pos), "$")(maps.pushTo)
;(items, (1, 1)(grid.pos), "!")(maps.pushTo)
;(items, (3, 2)(grid.pos), "%")(maps.pushTo)
;set actors[(3, 2)(grid.pos)] to { glyph: "o", hp: 5, bag: [] }
;set actors[(4, 1)(grid.pos)] to "@"
;set actors[(5, 1)(grid.pos)] to { hp: 1 }
;set actors[(5, 2)(grid.pos)] to 1.5
;(whole, null, null)(draw)
// Empty piles and null cells are empty: the layers below show through.
;set items[(1, 1)(grid.pos)] to []
;set actors[(5, 2)(grid.pos)] to null
;(whole, null, null)(draw)`)),
  lines('######|......|...+..', '######|.!..@?|...o.?', '######|....@?|...o..'));
});

test('view: what is not visible is drawn from memory, which the terrain does not change', { skip }, () => {
  assert.equal(out(viewProgram(`
;var memory is (6, 3)(view.newMemory)
;set actors[(4, 1)(grid.pos)] to "@"
;(memory, terrain, left)(view.remember)
;(whole, left, memory)(draw)
// Seen once, then out of sight: the door becomes a wall unseen, and is still
// remembered as a door; actors are not remembered.
;(memory, terrain, all)(view.remember)
;(terrain, 3, 2, Terrain.wall)(grid.put)
;(whole, left, memory)(draw)
// Seen again, the memory is brought up to date.
;(memory, terrain, all)(view.remember)
;(whole, left, memory)(draw)
;"{(memory, (3, 2)(grid.pos))(view.recalled)} {(memory, (0, 0)(grid.pos))(view.ageOf)}"($print)
// Visible everything: memory is not used.
;(whole, null, memory)(draw)`)),
  lines('###   |...   |...   ', '######|......|...+..', '######|......|...#..', 'wall 0', '######|....@.|...#..'));
});

test('view: noting, forgetting and fading memory', { skip }, () => {
  assert.equal(out(viewProgram(`
;var memory is (6, 3)(view.newMemory)
;(memory, terrain, all)(view.remember)
// A sound heard out of sight is noted, whatever is visible.
;(memory, (5, 1)(grid.pos), "!")(view.note)
;(memory, (4, 1)(grid.pos))(view.forget)
;(whole, left, memory)(draw)
;(memory, 3, 5)(view.fade)
;(memory, terrain, left)(view.remember)
;"{(memory, (5, 1)(grid.pos))(view.ageOf)} {(memory, (0, 0)(grid.pos))(view.ageOf)}"($print)
// Ages above the limit are forgotten; what is seen again starts at 0.
;(memory, 3, 5)(view.fade)
;(whole, left, memory)(draw)
;const age is (memory, (5, 1)(grid.pos))(view.ageOf)
    onfail { "forgotten" }
;const off is (memory, (9, 9)(grid.pos))(view.recalled)
    onfail { "off the map" }
;(memory, (9, 9)(grid.pos), "!")(view.note)
    onfail { null }
;"{age} {off}"($print)`)),
  lines('######|.... !|...+..', '3 0', '###   |...   |...   ', 'forgotten off the map'));
});

test('view: setting ages, and fading only an area', { skip }, () => {
  assert.equal(out(viewProgram(`
;var memory is (6, 3)(view.newMemory)
;(memory, terrain, all)(view.remember)
;(memory, (0, 0)(grid.pos), 3)(view.setAge)
;(memory, (1, 0)(grid.pos), -100)(view.setAge)
;"{(memory, (0, 0)(grid.pos))(view.ageOf)} {(memory, (1, 0)(grid.pos))(view.ageOf)}"($print)
// An age equal to the limit is kept, and one above it forgotten; a vivid
// memory outlasts the limit.
;(memory, 2, 5)(view.fade)
;"{(memory, (0, 0)(grid.pos))(view.ageOf)} {(memory, (1, 0)(grid.pos))(view.ageOf)} {(memory, (2, 0)(grid.pos))(view.ageOf)}"($print)
;(memory, 2, 5)(view.fade)
;const gone is (memory, (0, 0)(grid.pos))(view.ageOf)
    onfail { "forgotten" }
;"{gone} {(memory, (1, 0)(grid.pos))(view.ageOf)}"($print)
// Only the right half fades here, like a place where memory fails faster;
// positions off the grid are skipped.
;var fresh is (6, 3)(view.newMemory)
;(fresh, terrain, all)(view.remember)
;const right is ((3, 0)(grid.pos), (7, 2)(grid.pos))(grid.rect)
;(fresh, right, 10, 5)(view.fadeArea)
;(whole, [], fresh)(draw)
;(fresh, ((0, 0)(grid.pos), 1)(grid.square), 3, 5)(view.fadeArea)
;"{(fresh, (0, 0)(grid.pos))(view.ageOf)} {(fresh, (1, 1)(grid.pos))(view.ageOf)} {(fresh, (2, 2)(grid.pos))(view.ageOf)}"($print)
// A negative amount makes memories younger; setAgeArea sets ages outright,
// skipping cells off the grid and cells with nothing remembered.
;(fresh, [(0, 0)(grid.pos)], -5, 5)(view.fadeArea)
;(fresh, [(1, 1)(grid.pos), (5, 0)(grid.pos), (9, 9)(grid.pos)], 7)(view.setAgeArea)
;const forgotten is (fresh, (5, 1)(grid.pos))(view.ageOf)
    onfail { "-" }
;"{(fresh, (0, 0)(grid.pos))(view.ageOf)} {(fresh, (1, 1)(grid.pos))(view.ageOf)} {forgotten}"($print)
// setAge needs something remembered, on the grid.
;var none is "set"
;(fresh, (5, 0)(grid.pos), 1)(view.setAge)
    onfail { ;set none to "nothing there" }
;var off is "set"
;(fresh, (9, 9)(grid.pos), 1)(view.setAge)
    onfail { ;set off to "off the map" }
;"{none} {off}"($print)`)),
  lines('3 -100', '5 -98 2', 'forgotten -96', '###   |...   |...   ', '3 3 0', '-2 7 -', 'nothing there off the map'));
});

// colorRows with the escape character shown as ^, so that the codes read.
const colorProgram = (body) => `;import grid from "$lib/grid.wvt"
;import view from "$lib/view.wvt"
;import text from "$lib/text.wvt"
;enum Terrain (floor, lava, snow)
;const tiles is [Terrain.floor: ".", Terrain.lava: { glyph: "~", fg: view.Color.yellow, bg: view.Color.red },
    Terrain.snow: { glyph: "*", bg: view.Color.white }]
;var terrain is (5, 1, Terrain.floor)(grid.new)
;var actors is [:]
;var effects is [:]
;const w is { x: 0, y: 0, width: 5, height: 1 }
;fn layers() { [(terrain, tiles)(view.gridLayer), (actors, [:])(view.mapLayer), (effects, [:])(view.mapLayer)] }
;fn show(ref rows) { ((rows, "|")($join), "\\u{1B}", "^")(text.replace) }
${body}`;

test('view: colours combine by layer, and a glyph stays readable', { skip }, () => {
  assert.equal(out(colorProgram(`
;(terrain, ((1, 0)(grid.pos), (3, 0)(grid.pos))(grid.rect), Terrain.lava)(grid.fill)
// An orc in lava under a shield, a red rat in red lava, and the lava itself.
;set actors[(1, 0)(grid.pos)] to { glyph: "o", fg: view.Color.black }
;set actors[(2, 0)(grid.pos)] to { glyph: "r", fg: view.Color.red }
;set effects[(1, 0)(grid.pos)] to { bg: view.Color.blue }
// An effect with the glyph " " hides what is below.
;set effects[(4, 0)(grid.pos)] to { glyph: "" }
;((layers), w, null, null, tiles, null, 0)(view.colorRows)(show)($print)
;((layers), w, null, null, tiles, 0)(view.rows)(show)($print)`)),
  lines('.^[30;44mo^[97;41mr^[33;41m~^[39;49m ', '.or~ '));
  assert.equal(out(colorProgram(`
;(terrain, 0, 0, Terrain.snow)(grid.put)
;(terrain, 1, 0, Terrain.lava)(grid.put)
// White on white snow turns black; a tint above a glyph sets its colour.
;set actors[(0, 0)(grid.pos)] to { glyph: "@", fg: view.Color.white }
;set actors[(1, 0)(grid.pos)] to "k"
;set effects[(1, 0)(grid.pos)] to { fg: view.Color.green }
// Default on default is left alone, and a string draws in default colours.
;set actors[(2, 0)(grid.pos)] to { glyph: "d", fg: view.Color.default, bg: view.Color.default }
;set actors[(3, 0)(grid.pos)] to "s"
;((layers), w, null, null, tiles, null, 0)(view.colorRows)(show)($print)`)),
  lines('^[30;47m@^[32;41mk^[39;49mds.'));
});

test('view: remembered cells in colour', { skip }, () => {
  assert.equal(out(colorProgram(`
;(terrain, 1, 0, Terrain.lava)(grid.put)
;var memory is (5, 1)(view.newMemory)
;(memory, terrain, ((0, 0)(grid.pos), (4, 0)(grid.pos))(grid.rect))(view.remember)
;const near is [(0, 0)(grid.pos)]
// In a memory style of gray on default, or in their own colours.
;((layers), w, near, memory, tiles, { fg: view.Color.gray, bg: view.Color.default }, 0)(view.colorRows)(show)($print)
;((layers), w, near, memory, tiles, null, 0)(view.colorRows)(show)($print)
// A memory style whose colours clash still shows the glyph.
;((layers), w, near, memory, tiles, { fg: view.Color.gray, bg: view.Color.gray }, 0)(view.colorRows)(show)($print)`)),
  lines('.^[90;49m~...^[0m', '.^[33;41m~^[39;49m...', '.^[97;100m~...^[0m'));
});

test('view: animation frames, with phases', { skip }, () => {
  assert.equal(out(colorProgram(`
// Three torches with the same frames: in step, one a frame later, and one with
// a phase far beyond the frames, which counts round; a negative phase too.
;const frames is ["a", "b", "c"]
;set actors[(0, 0)(grid.pos)] to { glyph: frames }
;set actors[(1, 0)(grid.pos)] to { glyph: frames, phase: 1 }
;set actors[(2, 0)(grid.pos)] to { glyph: frames, phase: 7 }
;set actors[(3, 0)(grid.pos)] to { glyph: frames, phase: -1 }
;var tick is 0
;while (tick < 4) {
    ;((layers), w, null, null, tiles, tick)(view.rows)(show)($print)
    ;set tick += 1
}
// Colours have frames of their own length.
;set actors[(4, 0)(grid.pos)] to { glyph: "*", fg: [view.Color.red, view.Color.brightRed] }
;((layers), w, null, null, tiles, null, 0)(view.colorRows)(show)($print)
;((layers), w, null, null, tiles, null, 1)(view.colorRows)(show)($print)`)),
  lines('abbc.', 'bcca.', 'caab.', 'abbc.', 'abbc^[31;49m*^[0m', 'bcca^[91;49m*^[0m'));
  // Glyph tables animate too, and remembered cells are a still picture.
  assert.equal(out(colorProgram(`
;const flowing is [Terrain.floor: { glyph: [".", ","], phase: 1 }]
;fn floor() { [(terrain, flowing)(view.gridLayer)] }
;var memory is (5, 1)(view.newMemory)
;(memory, terrain, ((0, 0)(grid.pos), (4, 0)(grid.pos))(grid.rect))(view.remember)
;const near is [(0, 0)(grid.pos)]
;((floor), w, near, memory, flowing, 0)(view.rows)(show)($print)
;((floor), w, near, memory, flowing, 1)(view.rows)(show)($print)`)),
  lines(',,,,,', '.,,,,'));
});

test('view: the window follows a position and stops at the edges', { skip }, () => {
  assert.equal(out(viewProgram(`
;fn show(w) { "{w.x},{w.y} {w.width}x{w.height}" }
;var big is (20, 10, Terrain.floor)(grid.new)
;"{((10, 5)(grid.pos), 6, 4, big)(view.follow)(show)} {((0, 0)(grid.pos), 6, 4, big)(view.follow)(show)} {((19, 9)(grid.pos), 6, 4, big)(view.follow)(show)}"($print)
// A map smaller than the window is centred in it.
;const w is ((1, 1)(grid.pos), 10, 5, terrain)(view.follow)
;w(show)($print)
;(w, null, null)(draw)
;(((0, 2)(grid.pos), 4, 2, terrain)(view.follow), null, null)(draw)`)),
  lines('7,3 6x4 0,0 6x4 14,6 6x4', '-2,-1 10x5', '          |  ######  |  ......  |  ...+..  |          ', '....|...+'));
});
