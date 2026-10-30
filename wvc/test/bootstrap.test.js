// The bootstrap seed wvc/wvc.c: wvc compiled to C by itself. A native wvc
// built from it must compile wvc/wvc.wvt back to the same file (the fixed
// point), so the seed is up to date with the sources.
//
// To update the seed after changing wvc/*.wvt or runtime/wvt.c:
//   zig cc -O2 -o wvc.exe wvc/wvc.c
//   ./wvc.exe wvc/wvc.wvt -o wvc/wvc.c       (twice, if the change affects code generation)

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { exe, noZig, readRepo, root, runtimeC, tempDir, wvc, wvcExe, zig } from './helpers.js';

const skip = noZig;

test('native fixed point: wvc built from wvc.c compiles itself to wvc.c', { skip }, () => {
  const out = join(tempDir(), 'self.c');
  const r = wvc(['wvc/wvc.wvt', '-o', out]);
  assert.deepEqual({ err: r.err, code: r.code }, { err: '', code: 0 });
  assert.ok(readFileSync(out, 'utf8') === readRepo('wvc/wvc.c'),
    'wvc/wvc.c is out of date: build wvc from it, then run wvc wvc/wvc.wvt -o wvc/wvc.c until it no longer changes');
});

test('wvc finds its runtime from where it is', { skip }, () => {
  // Laid out like the repository: wvc one level down, the runtime beside that.
  const dir = tempDir();
  const sub = join(dir, 'tool');
  mkdirSync(sub);
  mkdirSync(join(dir, 'runtime'));
  copyFileSync(wvcExe(), join(sub, `wvc${exe}`));
  copyFileSync(runtimeC, join(dir, 'runtime/wvt.c'));
  const r = spawnSync(join(sub, `wvc${exe}`), [join(root, 'examples/hello.wvt'), '-o', join(dir, 'hello.c')], { cwd: tempDir(), encoding: 'utf8' });
  assert.deepEqual({ err: r.stderr, code: r.status }, { err: '', code: 0 });
});

test('wvc build makes a native program', { skip }, () => {
  const dir = tempDir();
  copyFileSync(join(root, 'examples/hello.wvt'), join(dir, 'hello.wvt'));
  const r = wvc(['build', join(dir, 'hello.wvt'), '--cc', zig, '--runtime', runtimeC]);
  assert.deepEqual({ err: r.err, code: r.code }, { err: '', code: 0 });
  const run = spawnSync(join(dir, `hello${exe}`), [], { encoding: 'utf8' });
  assert.deepEqual({ out: run.stdout, code: run.status }, { out: 'Hello, World\n', code: 0 });
  const missing = wvc(['build', join(dir, 'hello.wvt'), '--cc', join(dir, 'no-such-cc'), '--runtime', runtimeC]);
  assert.equal(missing.code, 2);
  assert.match(missing.err, /cannot run/);
});
