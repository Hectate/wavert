// The runtime's host functions (§12): arguments, $run, files, environment,
// broken pipes, and how tantrums and unhandled signals end a program.
import { before, test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { cc, exe, noZig, root, tmp } from './helpers.js';

let host, dir;
before(() => {
  if (noZig) return;
  dir = tmp();
  host = cc([join(root, 'runtime/test/host.c')], join(dir, `host${exe}`));
});

const run = (args, opts = {}) => spawnSync(host, args, { encoding: 'utf8', ...opts });
const lines = (...vs) => vs.map((v) => `${typeof v === 'string' ? JSON.stringify(v) : v}\n`).join('');

const TRICKY = ['plain', 'a b', 'c"d', '', 'e\\', 'f\\"g', 'h\\\\', '\\\\server\\share', 'é😀', 'tab\there', '"', 'x y\\',
  '"quoted"', '*', '%PATH%', '$HOME', 'a\\\\"b', ' lead', 'trail '];

test('§12.4 $args arrive unchanged', { skip: noZig }, () => {
  const r = run(['args', ...TRICKY]);
  assert.equal(r.stdout, lines(...TRICKY));
  assert.equal(r.status, 0);
});

test('§12.4 $run passes each argument unchanged and returns the exit code', { skip: noZig }, () => {
  let r = run(['run', host, 'args', ...TRICKY]);
  assert.equal(r.stdout, `before\n${lines(...TRICKY)}0\n`);
  r = run(['run', host, 'exit', '7']);
  assert.equal(r.stdout, 'before\n7\n');
  r = run(['run', join(dir, 'no-such-program'), 'x']);
  assert.equal(r.stdout, 'before\nWAH $run\n');
  // A name without a path is looked up on PATH (and, on Windows, with .exe added).
  r = run(['run', 'node', '-e', 'process.exit(3)']);
  assert.equal(r.stdout, 'before\n3\n');
});

test('§12.4 $run reports a negative exit code as node does', { skip: noZig || process.platform !== 'win32' }, () => {
  const r = run(['run', host, 'exit', '-1']);
  const viaNode = spawnSync(host, ['exit', '-1']).status;
  assert.equal(r.stdout, `before\n${viaNode}\n`);
});

test('§12.3 files', { skip: noZig }, () => {
  const d = join(dir, 'files');
  mkdirSync(d);
  const r = run(['files', d.replace(/\\/g, '/')]);
  assert.equal(r.stdout, [
    'fake', 'WAH $readFile', 'uninvited', 'uninvited', JSON.stringify('one\ntwo €\n'), 'real',
    'uninvited', JSON.stringify('﻿bom'), '["a.txt", "é😀.txt"]', 'WAH $listDir', 'WAH $readFile',
    'uninvited', 'WAH $deleteFile', '["é😀.txt"]', 'real', 'WAH $writeFile', 'WAH $appendFile',
    'WAH $readFile', 'fake', 'WAH $listDir', '',
  ].join('\n'));
});

test('§12.4 $env and $programPath', { skip: noZig }, () => {
  const r = run(['env', 'WAH_TEST_VAR', 'WAH_TEST_UNSET', ''], {
    env: { ...process.env, WAH_TEST_VAR: 'é😀 x' },
  });
  assert.equal(r.stdout, `${JSON.stringify('é😀 x')}\nWAH $env\nWAH $env\n`);
  assert.equal(run(['path']).stdout, `${JSON.stringify(host)}\n`);
});

test('§12.2 $readLine and a prompt', { skip: noZig }, () => {
  assert.equal(run(['echo'], { input: 'a\r\nb\n\nc' }).stdout, 'prompt> a\nb\n\nc\n');
});

// Closes the program's stream after its first output; resolves with how it ended.
function closeEarly(mode, stream) {
  return new Promise((resolve, reject) => {
    const child = spawn(host, [mode]);
    let stderr = '';
    if (stream === 'stdout') child.stderr.on('data', (d) => { stderr += d; });
    child[stream].once('data', () => child[stream].destroy());
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error('the program kept running after its reader went away'));
    }, 30000);
    child.on('exit', (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal, stderr });
    });
  });
}

test('§12.2 a broken pipe ends the program silently with 141', { skip: noZig }, async () => {
  const want = process.platform === 'win32' ? { code: 141, signal: null } : { code: null, signal: 'SIGPIPE' };
  assert.deepEqual(await closeEarly('pipe', 'stdout'), { ...want, stderr: '' });
  const err = await closeEarly('epipe', 'stderr');
  assert.deepEqual({ code: err.code, signal: err.signal }, want);
});

test('§8.1 a tantrum prints the scheme stack and exits with 1', { skip: noZig }, () => {
  let r = run(['tantrum']);
  assert.equal(r.stdout, 'out first\n');
  assert.equal(r.stderr, 'Waluigi was not invited to this exception\ntantrum: it broke\n    in boom\n    in outer\n');
  assert.equal(r.status, 1);
  r = run(['tantrum-value']);
  assert.equal(r.stderr, 'Waluigi was not invited to this exception\ntantrum: [1, "a\\"b"]\n');
  r = run(['overflow']);
  assert.match(r.stderr, /^Waluigi was not invited to this exception\ntantrum: stack overflow\n( {4}in deep\n)+$/);
  assert.equal(r.status, 1);
});

test('§8.2 an unhandled signal becomes a tantrum in the caller', { skip: noZig }, () => {
  let r = run(['unhandled']);
  assert.equal(r.stderr, "Waluigi was not invited to this exception\ntantrum: unhandled WAH! (code 3) from 'parse' in 'ignore'\n    in ignore\n");
  assert.equal(r.status, 1);
  r = run(['unhandled-top'], { input: '' });
  assert.equal(r.stderr, "Waluigi was not invited to this exception\ntantrum: unhandled WAH! (code 1) from '$readLine'\n");
});
