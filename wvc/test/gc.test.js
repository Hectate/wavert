// The native runtime's garbage collector (§15): memory stays flat in a long
// loop, and collecting on every allocation (WVT_GC_STRESS=1) changes nothing a
// program does. WVT_GC_STATS makes a program print its collector's statistics.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { exe, noZig, runtimeC, wvc, writeFiles, zigBuild } from './helpers.js';

const skip = noZig;

// Builds source natively; returns a function that runs it: (args, env) => { out, err, code }.
function build(source) {
  const dir = writeFiles({ 'main.wvt': source });
  const c = wvc(['main.wvt', '-o', 'main.c', '--runtime', runtimeC], { cwd: dir });
  assert.deepEqual({ err: c.err, code: c.code }, { err: '', code: 0 });
  const program = zigBuild(join(dir, 'main.c'), join(dir, `main${exe}`));
  return (args = [], env = {}) => {
    const r = spawnSync(program, args, { encoding: 'utf8', env: { ...process.env, ...env } });
    return { out: r.stdout, err: r.stderr, code: r.status };
  };
}
const stats = (err) => {
  const m = /gc: (\d+) collections, live (\d+) bytes, peak heap (\d+) bytes/.exec(err);
  assert.ok(m, err);
  return { runs: Number(m[1]), live: Number(m[2]), peak: Number(m[3]) };
};

test('memory stays flat in a long loop', { skip }, () => {
  // Each turn draws an 80x22 screen into strings, as a game does, and moves
  // monsters kept in a map.
  const run = build(`
;const turns is $args[0]($toInt)
;var cells is []
;var i is 0
;while (i < 80 * 22) {
    ;(cells, i % 7 == 0 ? "#" : ".")($push)
    ;set i += 1
}
;var monsters is [:]
;set i to 0
;while (i < 20) {
    ;set monsters[i] to { x: i, hp: 5 }
    ;set i += 1
}
;var screen is ""
;var t is 0
;while (t < turns) {
    ;var rows is []
    ;var y is 0
    ;while (y < 22) {
        ;(rows, ((cells, y * 80, y * 80 + 80)($slice), "")($join))($push)
        ;set y += 1
    }
    ;set screen to (rows, "\\n")($join)
    ;const ids is monsters($keys)
    ;var k is 0
    ;while (k < ids($length)) {
        ;var m is monsters[ids[k]]
        ;set m.x to (m.x + 1) % 80
        ;set monsters[ids[k]] to m
        ;set k += 1
    }
    ;set t += 1
}
;screen($length)($print)
;exit
`);
  const short = run(['300'], { WVT_GC_STATS: '1' });
  const long = run(['3000'], { WVT_GC_STATS: '1' });
  assert.equal(short.out, '1781\n');
  assert.equal(long.out, '1781\n');
  const a = stats(short.err), b = stats(long.err);
  assert.ok(b.runs > a.runs, `${a.runs} then ${b.runs} collections`);
  assert.ok(b.peak <= a.peak * 1.5, `peak heap ${a.peak} after 300 turns, ${b.peak} after 3000`);
  assert.ok(b.live < 1 << 20, `${b.live} bytes live at the end`);
});

test('collecting on every allocation changes nothing', { skip }, () => {
  // Strings, arrays, records, maps, sorting, nested functions with frames, and
  // deep recursion, with references held only in locals between allocations.
  const run = build(`
;fn build(n) {
    ;if (n == 0) { ;return null }
    { value: "node {n}", next: (n - 1)(build) }
}
;fn walk(ref node) {
    ;var parts is []
    ;while (node != null) {
        ;(parts, node.value)($push)
        ;set node to node.next
    }
    (parts, ",")($join)
}
;fn counter() {
    ;var count is []
    ;fn add(x) { ;(count, "{x}")($push) }
    ;1(add)
    ;"two"(add)
    (count, "+")($join)
}
;const list is 2000(build)
;list(walk)($length)($print)
;var m is [:]
;var i is 0
;while (i < 3000) {
    ;set m["k{i}"] to [i, "v{i}", { n: i }]
    ;if (i % 3 == 0) { ;(m, "k{(i / 3)($floor)}")($remove) onfail { null } }
    ;set i += 1
}
;m($length)($print)
;const vs is m($values)
;var keys is []
;set i to 0
;while (i < vs($length)) {
    ;(keys, -vs[i][2].n)($push)
    ;set i += 1
}
;const sorted is (vs, keys)($sortBy)
;"{sorted[0][1]} {sorted[-1][1]}"($print)
;(counter)($print)
;const words is ("the quick brown fox jumps over the lazy dog", " ")($split)
;(words($sort), " ")($join)($print)
;exit
`);
  const normal = run();
  assert.equal(normal.code, 0, normal.err);
  assert.equal(normal.out, '18892\n2000\nv2999 v1000\n1+two\nbrown dog fox jumps lazy over quick the the\n');
  assert.deepEqual(run([], { WVT_GC_STRESS: '1' }), normal);
});
