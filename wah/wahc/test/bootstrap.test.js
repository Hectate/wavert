// The bootstrap seed wahc/wahc.c: wahc compiled to C. It must be up to date,
// and a native wahc built from it must reproduce it (the native fixed point).
// The native tests need Zig (the ZIG variable, else zig on PATH) and skip
// without it.
//
// To update the seed: node js/bin/wahc0.js wahc/wahc.wah --target c -o wahc/wahc.c
// To build wahc:      zig cc -O2 -o wahc/wahc.exe wahc/wahc.c

import { before, test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compile } from '../../js/src/compile.js';
import { examples, readRepo, stage1 } from './helpers.js';

const root = fileURLToPath(new URL('../../', import.meta.url));
const zig = process.env.ZIG || 'zig';
const noZig = spawnSync(zig, ['version']).status === 0 ? false : 'Zig not found (set ZIG to its path)';
const exe = process.platform === 'win32' ? '.exe' : '';
const seed = readRepo('wahc/wahc.c');

test('wahc/wahc.c is wahc compiled to C (regenerate it if wahc changed)', () => {
  const r = compile(readRepo('wahc/wahc.wah'), 'wahc/wahc.wah', { readFile: readRepo, target: 'c' });
  assert.ok(r.ok);
  assert.ok(r.c === seed, 'wahc/wahc.c is out of date: node js/bin/wahc0.js wahc/wahc.wah --target c -o wahc/wahc.c');
});

let dir, wahc;
before(() => {
  if (noZig) return;
  dir = mkdtempSync(join(tmpdir(), 'wah-boot-'));
  // Laid out like the repository, so wahc finds its runtimes from where it is.
  mkdirSync(join(dir, 'wahc'));
  mkdirSync(join(dir, 'runtime'));
  mkdirSync(join(dir, 'js/src'), { recursive: true });
  copyFileSync(join(root, 'runtime/wah.c'), join(dir, 'runtime/wah.c'));
  copyFileSync(join(root, 'js/src/runtime.js'), join(dir, 'js/src/runtime.js'));
  wahc = join(dir, `wahc/wahc${exe}`);
  const r = spawnSync(zig, ['cc', '-std=gnu11', '-O2', '-o', wahc, join(root, 'wahc/wahc.c')], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
});

// Runs the native wahc in the repository root; returns what it wrote to out.
function native(args, out) {
  const r = spawnSync(wahc, [...args, '-o', out], { cwd: root, encoding: 'utf8' });
  assert.deepEqual({ err: r.stderr, code: r.status }, { err: '', code: 0 }, args.join(' '));
  return readFileSync(out, 'utf8');
}

test('native fixed point: wahc built from wahc.c compiles itself to wahc.c', { skip: noZig }, () => {
  assert.ok(native(['wahc/wahc.wah', '--target', 'c'], join(dir, 'self.c')) === seed);
});

test('native wahc compiles wahc to the same JavaScript as stage 1', { skip: noZig }, () => {
  assert.ok(native(['wahc/wahc.wah'], join(dir, 'self.js')) === stage1());
});

test('native wahc compiles every example as wahc0 does', { skip: noZig }, () => {
  for (const file of examples()) {
    for (const target of ['js', 'c']) {
      const r = compile(readRepo(file), file, { readFile: readRepo, target });
      assert.ok(native([file, '--target', target], join(dir, `out.${target}`)) === r[target], `${file} --target ${target}`);
    }
  }
});

test('native wahc builds a program with build', { skip: noZig }, () => {
  const at = mkdtempSync(join(tmpdir(), 'wah-build-'));
  copyFileSync(join(root, 'examples/fizzbuzz.wah'), join(at, 'fizzbuzz.wah'));
  const r = spawnSync(wahc, ['build', join(at, 'fizzbuzz.wah'), '--cc', zig], { cwd: at, encoding: 'utf8' });
  assert.deepEqual({ err: r.stderr, code: r.status }, { err: '', code: 0 });
  assert.equal(readFileSync(join(at, 'fizzbuzz.c'), 'utf8'), compile(readRepo('examples/fizzbuzz.wah'), 'fizzbuzz.wah', { target: 'c' }).c);
  const run = spawnSync(join(at, `fizzbuzz${exe}`), [], { encoding: 'utf8' });
  const js = spawnSync(process.execPath, ['js/bin/wahc0.js', 'run', 'examples/fizzbuzz.wah'], { cwd: root, encoding: 'utf8' });
  assert.deepEqual({ out: run.stdout, code: run.status }, { out: js.stdout, code: js.status });
  // A compiler that cannot be started, or that fails, is reported.
  const missing = spawnSync(wahc, ['build', join(at, 'fizzbuzz.wah'), '--cc', join(at, 'no-such-cc')], { encoding: 'utf8' });
  assert.equal(missing.status, 2);
  assert.match(missing.stderr, /^wahc: cannot run .*no-such-cc: install Zig/);
  const failing = spawnSync(wahc, ['build', join(at, 'fizzbuzz.wah'), '--cc', process.execPath], { encoding: 'utf8' });
  assert.equal(failing.status, 2);
  assert.match(failing.stderr, /wahc: .* failed with exit code \d+\n$/);
});

test('native wahc finds its runtimes from where it is, run from anywhere', { skip: noZig }, () => {
  const elsewhere = mkdtempSync(join(tmpdir(), 'wah-cwd-'));
  for (const target of ['c', 'js']) {
    const out = join(elsewhere, `hello.${target}`);
    const r = spawnSync(wahc, [join(root, 'examples/hello.wah'), '--target', target, '-o', out], { cwd: elsewhere, encoding: 'utf8' });
    assert.deepEqual({ err: r.stderr, code: r.status }, { err: '', code: 0 }, target);
  }
  // The C it wrote builds and runs.
  const prog = join(elsewhere, `hello${exe}`);
  assert.equal(spawnSync(zig, ['cc', '-std=gnu11', '-O2', '-o', prog, join(elsewhere, 'hello.c')]).status, 0);
  const run = spawnSync(prog, [], { encoding: 'utf8' });
  const js = spawnSync(process.execPath, [join(elsewhere, 'hello.js')], { encoding: 'utf8' });
  assert.deepEqual({ out: run.stdout, code: run.status }, { out: js.stdout, code: js.status });
});
