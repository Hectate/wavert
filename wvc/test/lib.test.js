// The standard library written in Wavert (lib/) and the '$lib/' import prefix
// that reaches it from any program (§13.1, §15).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { diagnostics, exe, noZig, run, runtimeC, tempDir, wvcExe } from './helpers.js';

const skip = noZig;

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
