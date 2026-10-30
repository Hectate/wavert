// Builds C programs with Zig. Zig is found through the ZIG environment
// variable, else on PATH; when it is missing, the tests that need it skip.
import { spawnSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

export const root = fileURLToPath(new URL('../../', import.meta.url));
export const zig = process.env.ZIG || 'zig';
export const exe = process.platform === 'win32' ? '.exe' : '';

const probe = spawnSync(zig, ['version'], { encoding: 'utf8' });
export const noZig = probe.status === 0 ? false : `Zig not found (set ZIG to zig${exe}'s path)`;

export const tmp = () => mkdtempSync(join(tmpdir(), 'wah-rt-'));

// Compiles C sources to an executable; throws with the compiler's output on failure.
export function cc(sources, out, flags = []) {
  const r = spawnSync(zig, ['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', ...flags, '-o', out, ...sources], {
    encoding: 'utf8',
  });
  if (r.status !== 0 || r.stderr.trim() !== '') throw new Error(`zig cc failed:\n${r.stderr}${r.stdout}`);
  return out;
}
