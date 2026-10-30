import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import vm from 'node:vm';
import { compile } from '../src/compile.js';
import { lex } from '../src/lexer.js';
import { parse } from '../src/parser.js';

// With WAH_TARGET=c, runWah compiles to C instead, builds the program with Zig
// (the ZIG variable, else zig on PATH) and runs it, so the whole suite tests
// the C backend: WAH_TARGET=c ZIG=/path/to/zig npm test
export const target = process.env.WAH_TARGET === 'c' ? 'c' : 'js';
let cDir = null, cCount = 0;

// Builds and runs a compiled C program like the JS host below: only the given
// environment, stdin as lines, and the exit code as a signed int.
function runC(c, { args, stdin, env }) {
  cDir ??= fs.mkdtempSync(path.join(os.tmpdir(), 'wah-c-'));
  const base = path.join(cDir, `t${cCount++}`);
  fs.writeFileSync(`${base}.c`, c);
  const exe = `${base}${process.platform === 'win32' ? '.exe' : ''}`;
  const cc = spawnSync(process.env.ZIG || 'zig', ['cc', '-std=gnu11', '-O1', '-o', exe, `${base}.c`], { encoding: 'utf8' });
  if (cc.status !== 0) throw new Error(`zig cc failed for ${base}.c:\n${cc.stderr}`);
  const keep = process.platform === 'win32' ? { SYSTEMROOT: process.env.SYSTEMROOT } : {};
  const r = spawnSync(exe, args, {
    input: stdin.length ? stdin.join('\n') + '\n' : '',
    env: { ...keep, ...env },
    maxBuffer: 1 << 28,
    timeout: 60000,
  });
  if (r.error) throw r.error;
  return { out: r.stdout.toString(), err: r.stderr.toString(), code: r.status > 0x7fffffff ? r.status - 0x100000000 : r.status };
}

// Compiles and runs Γ-- source in a fresh VM context with captured I/O.
// files maps paths to the text of libraries the source may steal.
// Returns { out, err, code, warnings } or { compileErrors, warnings }.
export function runWah(source, { args = [], stdin = [], env = {}, requireExit = true, files = {} } = {}) {
  const readFile = (p) => {
    if (Object.hasOwn(files, p)) return files[p];
    throw new Error(`no such file ${p}`);
  };
  const r = compile(source, 'test.wah', { requireExit, readFile, target });
  const warnings = r.warnings.map((w) => w.message);
  if (!r.ok) return { compileErrors: r.errors.map((e) => e.message), hints: r.errors.map((e) => e.hint), warnings };
  if (target === 'c') return { ...runC(r.c, { args, stdin, env }), warnings };
  let out = '', err = '';
  const lines = [...stdin];
  const host = {
    fs,
    args,
    write: (fd, s) => { if (fd === 1) out += s; else err += s; },
    readLine: () => (lines.length ? lines.shift() : null),
    env: (n) => env[n],
    exit: (c) => { throw { __wahExit: c }; },
  };
  let code = 0;
  try {
    vm.runInNewContext(r.js, { $wahHost: host });
  } catch (e) {
    if (e && e.__wahExit !== undefined) code = e.__wahExit;
    else throw e;
  }
  return { out, err, code, warnings };
}

// Runs statements as a program body (appends ;!invited) and returns stdout.
export function out(body, opts) {
  const r = runWah(`${body}\n;!invited`, opts);
  if (r.compileErrors) throw new Error(`compile error: ${r.compileErrors.join('; ')}`);
  return r.out;
}

// Compile errors for a program body (with ;!invited appended).
export function errors(body) {
  const r = runWah(`${body}\n;!invited`);
  return r.compileErrors ?? [];
}

// Parses one statement and renders its expression as nested calls.
export function shape(src) {
  const prog = parse(lex(`;${src}`));
  return show(prog.stmts[0].e);
}

export function show(e) {
  switch (e.k) {
    case 'int': case 'float': return String(e.v);
    case 'str': return JSON.stringify(e.parts.map((p) => (typeof p === 'string' ? p : show(p))).join(''));
    case 'bool': return e.v ? 'real' : 'fake';
    case 'null': return 'uninvited';
    case 'name': return e.name;
    case 'std': return `$${e.name}`;
    case 'arr': return `[${e.items.map(show).join(', ')}]`;
    case 'rec': return `{${e.fields.map((f) => `${f.name}: ${show(f.e)}`).join(', ')}}`;
    case 'idx': return `${show(e.index)}[${show(e.arr)}]`;
    // conventional order, so tests can state what an inverted chain means
    case 'mem': return `${show(e.rec)}.${e.field}`;
    case 'un': return `(${e.op}${show(e.e)})`;
    case 'bin': return `(${show(e.l)} ${e.op} ${show(e.r)})`;
    case 'tern': return `(${show(e.c)} ? ${show(e.a)} : ${show(e.b)})`;
    case 'call': {
      const c = e.callee;
      const name = c.k === 'std' ? `$${c.name}` : c.k === 'mem' ? `${c.rec.name}.${c.field}` : c.name;
      return `${name}(${e.args.map(show).join(', ')})`;
    }
    case 'sigcall': return `WAH!(${show(e.arg)})`;
    case 'chain': return 'chain';
    default: return `<${e.k}>`;
  }
}
