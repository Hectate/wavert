// The C backend (--target c): every example behaves the same compiled to C
// as compiled to JavaScript. These tests need Zig (the ZIG variable, else zig
// on PATH) and skip without it.
import { before, test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, existsSync, mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { compile } from '../src/compile.js';

const root = fileURLToPath(new URL('../../', import.meta.url));
const zig = process.env.ZIG || 'zig';
const noZig = spawnSync(zig, ['version']).status === 0 ? false : 'Zig not found (set ZIG to its path)';
const exe = process.platform === 'win32' ? '.exe' : '';
const wahc0 = (...args) => spawnSync(process.execPath, ['js/bin/wahc0.js', ...args], { cwd: root, encoding: 'utf8' });

let dir;
before(() => { dir = mkdtempSync(join(tmpdir(), 'wah-c-')); });

// Compiles an example with both backends; returns a runner for each.
function both(file) {
  const base = join(dir, file.replace(/[/\\]/g, '_').replace(/\.wah$/, ''));
  let r = wahc0(file, '-o', `${base}.js`);
  assert.equal(r.status, 0, r.stderr);
  r = wahc0(file, '--target', 'c', '-o', `${base}.c`);
  assert.equal(r.status, 0, r.stderr);
  r = spawnSync(zig, ['cc', '-std=gnu11', '-O2', '-o', `${base}${exe}`, `${base}.c`], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  const opts = (input) => ({ cwd: root, encoding: 'utf8', input });
  return {
    js: (args = [], input = '') => spawnSync(process.execPath, [`${base}.js`, ...args], opts(input)),
    c: (args = [], input = '') => spawnSync(`${base}${exe}`, args, opts(input)),
  };
}

function same(file, runs) {
  const p = both(file);
  for (const [args, input] of runs) {
    const want = p.js(args, input), got = p.c(args, input);
    assert.deepEqual({ out: got.stdout, err: got.stderr, code: got.status }, { out: want.stdout, err: want.stderr, code: want.status },
      `${file} ${args.join(' ')}`);
  }
}

for (const file of ['hello.wah', 'fizzbuzz.wah', 'records.wah', 'suspect.wah', 'lineup.wah', 'modules.wah', 'fickle.wah']) {
  test(`examples/${file} compiled to C matches JavaScript`, { skip: noZig }, () => same(`examples/${file}`, [[[], '']]));
}

test('examples/echo.wah compiled to C matches JavaScript', { skip: noZig }, () => {
  same('examples/echo.wah', [[[], 'one\n\nthree'], [[], 'hi\nquit\nafter\n'], [[], '']]);
});

test('examples/tokenize.wah compiled to C matches JavaScript', { skip: noZig }, () => {
  const files = ['examples/fizzbuzz.wah', 'examples/tokenize.wah', 'examples/records.wah', 'examples/suspect.wah',
    'examples/lineup.wah', 'examples/modules.wah', 'examples/modules/shapes.wah', 'examples/fickle.wah'];
  same('examples/tokenize.wah', files.map((f) => [[f], '']));
});

// C leaves the order of a call's arguments unspecified. Clang happens to go left
// to right, so no behavior test catches a missing temporary; check the code.
test('arguments with effects are evaluated into temporaries, in order', () => {
  const c = (body) => compile(`;scheme (s)say { ;s($print) s }\n${body}\n;!invited`, 't.wah', { target: 'c' }).c;
  assert.match(c(';("a"(say)) + ("b"(say))($print)'),
    /\(\{ wv (t\d+) = rt_chk\(w_say\(rt_s\(&S\d+\)\), NULL\); wv (t\d+) = rt_chk\(w_say\(rt_s\(&S\d+\)\), NULL\); rt_add\(\1, \2\); \}\)/);
  // A binding read next to a call could see the call's effects, so it is ordered too.
  assert.match(c(';wah claim x ;x, ("b"(say))(pair) ;scheme (a, b)pair { a }'), /wv (t\d+) = w_x; wv (t\d+) = rt_chk\(w_say/);
  // Constants need no temporaries.
  assert.match(c(';wah + ("b"(say))($print)'), /rt_add\(rt_i\(1\), rt_chk\(w_say/);
});

test('build compiles to C and builds a program', { skip: noZig }, () => {
  const at = mkdtempSync(join(tmpdir(), 'wah-build-'));
  copyFileSync(join(root, 'examples/hello.wah'), join(at, 'hello.wah'));
  let r = wahc0('build', join(at, 'hello.wah'), '--cc', zig);
  assert.deepEqual({ err: r.stderr, code: r.status }, { err: '', code: 0 });
  assert.equal(spawnSync(join(at, `hello${exe}`), [], { encoding: 'utf8' }).stdout, 'Hello, World\n');
  // -o names the program; the C goes beside it.
  r = wahc0('build', join(at, 'hello.wah'), '-o', join(at, `hi${exe}`), '--cc', zig);
  assert.equal(r.status, 0, r.stderr);
  assert.ok(existsSync(join(at, 'hi.c')));
  r = wahc0('build', join(at, 'hello.wah'), '--cc', join(at, 'no-such-cc'));
  assert.equal(r.status, 2);
  assert.match(r.stderr, /^wahc0: cannot run .*no-such-cc: install Zig/);
  r = wahc0('build', join(at, 'hello.wah'), '--cc', process.execPath);
  assert.equal(r.status, 2);
  assert.match(r.stderr, /wahc0: .* failed with exit code \d+\n$/);
  r = wahc0('build', join(at, 'hello'));
  assert.deepEqual({ err: r.stderr, code: r.status }, { err: `wahc0: ${join(at, 'hello')} does not end with .wah, so name the program with -o\n`, code: 2 });
});

test('--target c writes a C program; other targets are rejected', { skip: noZig }, () => {
  const r = wahc0('examples/hello.wah', '--target', 'c', '-o', join(dir, 'hello-cli.c'));
  assert.equal(r.status, 0);
  assert.match(readFileSync(join(dir, 'hello-cli.c'), 'utf8'), /static void wah_main\(void\)/);
  assert.equal(wahc0('examples/hello.wah', '--target', 'wasm').status, 2);
});
