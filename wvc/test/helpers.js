// Shared helpers for the wvc tests. They need Zig (the ZIG variable, else zig
// on PATH) to build wvc from its seed, wvc/wvc.c, once per test file.

import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const root = fileURLToPath(new URL('../../', import.meta.url));
export const zig = process.env.ZIG || 'zig';
export const noZig = spawnSync(zig, ['version']).status === 0 ? false : 'Zig not found (set ZIG to its path)';
export const exe = process.platform === 'win32' ? '.exe' : '';
export const runtimeC = join(root, 'runtime/wvt.c');
export const readRepo = (path) => readFileSync(join(root, path), 'utf8');

// A fresh temporary directory.
export const tempDir = (prefix = 'wvt-test-') => mkdtempSync(join(tmpdir(), prefix));

// Builds a C file into an executable with zig cc; returns the executable's path.
export function zigBuild(cFile, out) {
  const r = spawnSync(zig, ['cc', '-std=gnu11', '-O2', '-w', '-o', out, cFile], { encoding: 'utf8' });
  if (r.status !== 0) throw new Error(`zig cc failed for ${cFile}:\n${r.stderr}`);
  return out;
}

// wvc built from the seed, once per process.
let wvcPath = null;
export function wvcExe() {
  if (!wvcPath) wvcPath = zigBuild(join(root, 'wvc/wvc.c'), join(tempDir('wvc-'), `wvc${exe}`));
  return wvcPath;
}

// Runs wvc with args: { out, err, code }.
export function wvc(args, opts = {}) {
  const r = spawnSync(wvcExe(), args, { encoding: 'utf8', cwd: opts.cwd ?? root });
  return { out: r.stdout, err: r.stderr, code: r.status };
}

// Writes files ({ path: text }) into a new directory; returns the directory.
export function writeFiles(files) {
  const dir = tempDir();
  for (const [path, text] of Object.entries(files)) {
    mkdirSync(dirname(join(dir, path)), { recursive: true });
    writeFileSync(join(dir, path), text);
  }
  return dir;
}

// Compiles source (the entry file main.wvt, with libraries in files) to C,
// builds it and runs it with input (a string or a Buffer): { out, err, code },
// or the compile error as { err, code: 2, compileError: true }.
export function run(source, { files = {}, input = '', args = [] } = {}) {
  const dir = writeFiles({ ...files, 'main.wvt': source });
  const c = wvc(['main.wvt', '-o', 'main.c', '--runtime', runtimeC], { cwd: dir });
  if (c.code !== 0) return { out: '', err: c.err, code: c.code, compileError: true };
  const program = zigBuild(join(dir, 'main.c'), join(dir, `main${exe}`));
  const r = spawnSync(program, args, { encoding: 'utf8', input });
  return { out: r.stdout, err: c.err + r.stderr, code: r.status };
}

// Builds source, which must compile cleanly; returns the program's path.
export function buildProgram(source) {
  const dir = writeFiles({ 'main.wvt': source });
  const c = wvc(['main.wvt', '-o', 'main.c', '--runtime', runtimeC], { cwd: dir });
  if (c.code !== 0 || c.err !== '') throw new Error(`wvc failed:\n${c.err}`);
  return zigBuild(join(dir, 'main.c'), join(dir, `main${exe}`));
}

// The diagnostics wvc gives for source, or '' if it compiles.
export function diagnostics(source, files = {}) {
  const dir = writeFiles({ ...files, 'main.wvt': source });
  return wvc(['main.wvt', '-o', 'main.c', '--runtime', runtimeC], { cwd: dir }).err;
}

// Every program in examples/ and samples/, as repo-relative paths, sorted;
// libraries are in their modules/ folders.
export const examples = () => ['examples', 'samples']
  .flatMap((d) => readdirSync(join(root, d)).filter((f) => f.endsWith('.wvt')).map((f) => `${d}/${f}`)).sort();
