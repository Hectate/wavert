import { readFileSync, readdirSync } from 'node:fs';
import vm from 'node:vm';
import { compile } from '../../js/src/compile.js';
import { formatDiag } from '../../js/src/errors.js';
import assert from 'node:assert/strict';

const root = new URL('../../', import.meta.url);
export const readRepo = (path) => readFileSync(new URL(path, root), 'utf8');

// Every example program, as repo-relative paths.
export const examples = () => readdirSync(new URL('examples/', root)).filter((f) => f.endsWith('.wah')).sort().map((f) => `examples/${f}`);
// The files of wahc itself, as repo-relative paths: the entry file wahc.wah and its modules.
export const wahcFiles = () => readdirSync(new URL('wahc/', root)).filter((f) => f.endsWith('.wah')).sort().map((f) => `wahc/${f}`);
// The libraries the examples steal (§13.1), as repo-relative paths.
export const exampleLibraries = () => readdirSync(new URL('examples/modules/', root)).filter((f) => f.endsWith('.wah')).sort()
  .map((f) => `examples/modules/${f}`);

// wahc.wah compiled once by wahc0 (stage 1).
let wahcJs = null;
export function stage1() {
  if (wahcJs) return wahcJs;
  const r = compile(readRepo('wahc/wahc.wah'), 'wahc/wahc.wah', { readFile: readRepo });
  if (!r.ok) throw new Error(r.errors.map((d) => formatDiag(d, r.src)).join('\n'));
  return (wahcJs = r.js);
}

// Where wahc says it runs from ($programPath), for finding its runtime.
export const PROGRAM_PATH = '/repo/wahc/wahc.js';

// Runs wahc with args. `files` maps paths to contents for $readFile, and
// $writeFile stores into `written`. `js` is the compiler to run (stage 1 by default).
export function runWahc(args, files = {}, written = {}, js = stage1()) {
  let out = '', err = '';
  const fs = {
    readFileSync(p) {
      if (Object.hasOwn(files, p)) return files[p];
      throw new Error(`no such file ${p}`);
    },
    writeFileSync(p, text) {
      if (p.startsWith('readonly/')) throw new Error(`cannot write ${p}`);
      written[p] = text;
    },
  };
  const host = {
    fs,
    args,
    programPath: PROGRAM_PATH,
    env: () => undefined, // no variables are set
    write: (fd, s) => { if (fd === 1) out += s; else err += s; },
    exit: (c) => { throw { __wahExit: c }; },
  };
  let code = 0;
  try {
    vm.runInNewContext(js, { $wahHost: host });
  } catch (e) {
    if (e && e.__wahExit !== undefined) code = e.__wahExit;
    else throw e;
  }
  return { out, err, code };
}

// Γ-- float text (§9.3).
export function fmtFloat(v) {
  if (Object.is(v, -0)) return '-0.0';
  const s = String(v);
  if (/^-?\d+$/.test(s)) return s + '.0';
  const m = /^(-?\d+)(e.*)$/.exec(s);
  return m ? `${m[1]}.0${m[2]}` : s;
}

// Maps wahc0's UTF-16 positions in text to wahc's code point positions.
export function codePoints(text) {
  const cps = [0];
  for (let i = 0, n = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    const low = c >= 0xdc00 && c <= 0xdfff;
    const afterHigh = i > 0 && text.charCodeAt(i - 1) >= 0xd800 && text.charCodeAt(i - 1) <= 0xdbff;
    if (!(low && afterHigh)) n++;
    cps.push(n);
  }
  return (pos) => cps[pos];
}

// Every literal Γ-- snippet in wahc0's own tests: the first argument of shape(),
// out(), errors() and the like, when it is a plain string without ${...}.
export function harvest() {
  const snippets = new Set();
  const call = /\b(shape|out|errors|one|none|lint|err|parseError|compileError|tantrum|runWah|lexError|kinds|warnings|lex|parse\(lex)\(('(?:[^'\\\n]|\\.)*'|`(?:[^`\\$]|\\.)*`)/g;
  for (const f of readdirSync(new URL('../../js/test/', import.meta.url))) {
    if (!f.endsWith('.test.js')) continue;
    for (const m of readRepo(`js/test/${f}`).matchAll(call)) {
      const s = new Function(`return ${m[2]}`)();
      snippets.add(m[1] === 'shape' ? `;${s}` : s);
    }
  }
  return [...snippets];
}

export const RUNTIME = 'js/src/runtime.js';
export const runtime = readRepo(RUNTIME);

// wahc compiling text: { out, err, code, js }. files maps paths to libraries it may steal.
export function wahc(text, args = ['test.wah', '-o', 'test.js'], js = undefined, files = {}) {
  const written = {};
  const r = runWahc(args, { ...files, [args[0]]: text, [RUNTIME]: runtime }, written, js);
  return { ...r, js: written['test.js'] ?? null };
}

// wahc0 compiling text as an entry file: { out, err, code, js }.
export function expected(text, files = {}) {
  const readFile = (p) => {
    if (Object.hasOwn(files, p)) return files[p];
    throw new Error(`no such file ${p}`);
  };
  const r = compile(text, 'test.wah', { readFile });
  const diags = r.ok ? r.warnings : [...r.warnings, ...r.errors];
  return { out: '', err: diags.map((d) => formatDiag(d, r.src) + '\n').join(''), code: r.ok ? 0 : 2, js: r.ok ? r.js : null };
}

// wahc and wahc0 agree on compiling text: the same output, errors and JavaScript.
export function agree(text, files = {}) {
  assert.deepEqual(wahc(text, undefined, undefined, files), expected(text, files), JSON.stringify({ text, files }));
}
