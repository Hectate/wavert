// Checks converted files against their Γ-- originals (docs/hybrid-syntax.md,
// "Verification"): each .wvt file must parse to the same syntax tree as its
// .wah original, apart from positions and parentheses, and each program must
// compile to the same C. Both compilers are given Wavert's runtime, so the
// only difference allowed is the standard library functions that Wavert
// renamed ($toPurple is $toString, $isNobody is $isNan).
//
// Usage: node tools/verify.mjs <wahc> <wvc> <dir.wah> <dir.wvt> [file.wah ...]
//   Compares every .wah file under dir.wah (or just the files named) with the
//   .wvt file at the same place under dir.wvt.

import { spawnSync } from 'node:child_process';
import { mkdtempSync, readdirSync, readFileSync, statSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, relative } from 'node:path';

const [wahc, wvc, wahDir, wvtDir, ...only] = process.argv.slice(2);
if (!wvtDir) {
  console.error('usage: node tools/verify.mjs <wahc> <wvc> <dir.wah> <dir.wvt> [file.wah ...]');
  process.exit(2);
}
const runtimeC = new URL('../runtime/wvt.c', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');
const tmp = mkdtempSync(join(tmpdir(), 'wvt-verify-'));

const walk = (dir) => readdirSync(dir).flatMap((f) => {
  const p = join(dir, f);
  return statSync(p).isDirectory() ? walk(p) : f.endsWith('.wah') ? [p] : [];
});

function run(exe, args) {
  const r = spawnSync(exe, args, { encoding: 'utf8' });
  return { out: r.stdout, err: r.stderr, code: r.status };
}

// The --ast dump without positions and parentheses, which differ by design.
const positionKeys = /^\s*(pos|end|namePos|pathPos|paramPos|close|paren): /;
function ast(exe, file) {
  const r = run(exe, ['--ast', file]);
  if (r.code !== 0) return { error: r.err };
  return { text: renamed(r.out.split('\n').filter((l) => !positionKeys.test(l)).join('\n').replace(/\.wah"/g, '.wvt"')) };
}

// wvc's syntax tree names the node kinds and fields that wahc's names in Γ--.
const kinds = {
  scheme: 'fn', lineup: 'enum', steal: 'import', bribe: 'end', getaway: 'return', tantrum: 'panic',
  bail: 'break', loiter: 'continue', lurk: 'while', signal: 'fail', sigcall: 'failWith', claim: 'decl',
  cheat: 'set', suspect: 'match',
};
const fields = { isShifty: 'isVar', isFickle: 'isRef', alibis: 'cases' };
// What the code generators call in the runtime, which Wavert renamed.
const runtimeNames = {
  rt_tantrum: 'rt_panic', rt_no_alibi: 'rt_no_case', rt_signal: 'rt_fail_with', rt_sigcode: 'rt_failcode',
  RT_NOBODY: 'RT_NAN', RT_EVERYBODY: 'RT_INF', wah_main: 'wvt_main',
};

// Γ-- output with Wavert's names for the renamed standard library functions,
// and a --ast dump with wvc's node names and values spelled as wvc prints them.
const lookup = (map, w) => (Object.hasOwn(map, w) ? map[w] : w);
const renamed = (text) => text
  .replace(/\b\w+\b/g, (w) => lookup(runtimeNames, w))
  .replace(/^(\s*k: ")(\w+)"$/gm, (m, key, k) => `${key}${lookup(kinds, k)}"`)
  .replace(/^(\s*)(\w+):/gm, (m, sp, f) => `${sp}${lookup(fields, f)}:`)
  .replace(/(\bstd_|\bname: ")toPurple\b/g, '$1toString')
  .replace(/(\bstd_|\bname: ")isNobody\b/g, '$1isNan')
  .replace(/^(\s*\w+: )(uninvited|real|fake)$/gm, (m, key, v) => key + { uninvited: 'null', real: 'true', fake: 'false' }[v]);

const files = only.length ? only : walk(wahDir);
let failed = 0;
for (const wah of files) {
  const rel = relative(wahDir, wah);
  const wvt = join(wvtDir, rel.replace(/\.wah$/, '.wvt'));
  const problems = [];
  const a = ast(wahc, wah), b = ast(wvc, wvt);
  if (a.error || b.error) problems.push(`--ast failed:\n${a.error ?? ''}${b.error ?? ''}`);
  else if (a.text !== b.text) {
    const al = a.text.split('\n'), bl = b.text.split('\n');
    let i = 0;
    while (i < al.length && al[i] === bl[i]) i++;
    problems.push(`syntax trees differ at line ${i + 1} of the dump:\n  wah: ${al.slice(Math.max(0, i - 3), i + 3).join('\n       ')}\n  wvt: ${bl.slice(Math.max(0, i - 3), i + 3).join('\n       ')}`);
  }
  const isProgram = /^\s*;\s*(\w+\s*)?!invited\s*(\/\/.*)?$/m.test(readFileSync(wah, 'utf8').trimEnd().split('\n').reverse().find((l) => l.trim() && !l.trim().startsWith('//')) ?? '');
  if (isProgram) {
    const outA = join(tmp, 'a.c'), outB = join(tmp, 'b.c');
    const ra = run(wahc, [wah, '--target', 'c', '-o', outA, '--runtime', runtimeC]);
    const rb = run(wvc, [wvt, '-o', outB, '--runtime', runtimeC]);
    if (ra.code !== 0 || rb.code !== 0) problems.push(`compiling to C failed:\n${ra.err}${rb.err}`);
    else {
      // wvc registers a program's globals with the collector, which wahc predates.
      const wvtOut = readFileSync(outB, 'utf8').replace(/^ {2}rt_roots\(.*\);\n/m, '');
      if (renamed(readFileSync(outA, 'utf8')) !== wvtOut) problems.push('the C differs');
    }
  }
  if (problems.length) {
    failed++;
    console.log(`FAIL ${rel}\n  ${problems.join('\n  ')}`);
  } else {
    console.log(`ok   ${rel}${isProgram ? ' (C identical)' : ''}`);
  }
}
console.log(failed ? `${failed} of ${files.length} failed` : `all ${files.length} match`);
process.exit(failed ? 1 : 0);
