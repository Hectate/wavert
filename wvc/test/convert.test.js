// tools/wah2wvt.wah, the Γ-- to Wavert converter: it is built with wahc from
// the imported Γ-- toolchain (wah/), and its output must parse to the same
// syntax tree and compile to the same C as the original (tools/verify.mjs).

import { before, test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { exe, noZig, root, tempDir, wvcExe, zigBuild } from './helpers.js';

const skip = noZig;
let dir, wahc, wah2wvt;

before(() => {
  if (noZig) return;
  dir = tempDir('wah2wvt-');
  wahc = zigBuild(join(root, 'wah/wahc/wahc.c'), join(dir, `wahc${exe}`));
  const c = spawnSync(wahc, [join(root, 'tools/wah2wvt.wah'), '--target', 'c', '-o', join(dir, 'wah2wvt.c'),
    '--runtime', join(root, 'wah/runtime/wah.c')], { encoding: 'utf8' });
  assert.equal(c.status, 0, c.stderr);
  wah2wvt = zigBuild(join(dir, 'wah2wvt.c'), join(dir, `wah2wvt${exe}`));
});

function convert(input, output) {
  const r = spawnSync(wah2wvt, [input, '-o', output], { encoding: 'utf8' });
  return { err: r.stderr, code: r.status };
}

test('the Γ-- examples convert to Wavert that compiles to the same C', { skip }, () => {
  const out = join(dir, 'examples');
  mkdirSync(join(out, 'modules'), { recursive: true });
  const from = join(root, 'wah/examples');
  const files = [
    ...readdirSync(from).filter((f) => f.endsWith('.wah')),
    ...readdirSync(join(from, 'modules')).filter((f) => f.endsWith('.wah')).map((f) => `modules/${f}`),
  ];
  for (const f of files) {
    assert.deepEqual(convert(join(from, f), join(out, f.replace(/\.wah$/, '.wvt'))), { err: '', code: 0 }, f);
  }
  const v = spawnSync(process.execPath, [join(root, 'tools/verify.mjs'), wahc, wvcExe(), from, out], { encoding: 'utf8' });
  assert.equal(v.status, 0, v.stdout + v.stderr);
  assert.match(v.stdout, /all 11 match/);
});

test('layout and comments are kept', { skip }, () => {
  const src = join(dir, 'layout.wah');
  writeFileSync(src, `// A header comment.

;scheme (a, b)add {
    a + b // the sum
}

;[
    wah, waah,
    WAAAH
] claim xs
;!doubt (wah(add) > waah &&
    fake) { ;xs($print) } anyway { ;waah !invited }
;"x"($toInt) claim n
    whine (c) { c }
;!invited
`);
  assert.deepEqual(convert(src, join(dir, 'layout.wvt')), { err: '', code: 0 });
  assert.equal(readFileSync(join(dir, 'layout.wvt'), 'utf8'), `// A header comment.

;fn add(a, b) {
    a + b // the sum
}

;const xs is [
    1, 2,
    -3
]
;if (1(add) > 2 &&
    false) { ;xs($print) } else { ;exit 2 }
;const n is "x"($toInt)
    onfail (c) { c }
;exit
`);
});

test('precedence: calls, grouping, negative numbers, member and index order', { skip }, () => {
  const src = join(dir, 'prec.wah');
  writeFileSync(src, `;scheme (x)f { x }
;scheme ()g { wah }
;(wah + waah(f)) + (,(g)) claim a
;WAH(f) claim b
;-wah claim c
;WAH ** waah claim d
;[wah, waah(f)] claim e
;{ k: [wah] } claim r
;wh[k.r] claim i
;,(g)(f) + wah claim j
;!invited
`);
  assert.deepEqual(convert(src, join(dir, 'prec.wvt')), { err: '', code: 0 });
  assert.equal(readFileSync(join(dir, 'prec.wvt'), 'utf8'), `;fn f(x) { x }
;fn g() { 1 }
;const a is (1 + 2)(f) + (g)
;const b is (-1)(f)
;const c is -(1)
;const d is (-1) ** 2
;const e is [(1, 2)(f)]
;const r is { k: [1] }
;const i is r.k[0]
;const j is (g)(f) + 1
;exit
`);
});

test('names that are Wavert keywords are reported', { skip }, () => {
  const src = join(dir, 'keywords.wah');
  writeFileSync(src, ';wah claim fn\n;$toWah claim x\n;!invited\n');
  const r = convert(src, join(dir, 'keywords.wvt'));
  assert.equal(r.code, 2);
  assert.match(r.err, /'fn' is a keyword in Wavert; rename it before converting/);
  assert.match(r.err, /Wavert has no \$toWah/);
});
