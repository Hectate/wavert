#!/usr/bin/env node
// wahc0: the stage 0 Γ-- compiler (Γ-- -> JavaScript, or C with --target c).

import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync, writeSync } from 'node:fs';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runInThisContext } from 'node:vm';
import { compile } from '../src/compile.js';
import { formatDiag } from '../src/errors.js';

const USAGE = `usage:
  wahc0 <file.wah> [-o <out.js>]    compile to JavaScript (default: <file>.js)
  wahc0 <file.wah> --target c [-o <out.c>]
                                    compile to C (default: <file>.c); build it with
                                    a GNU C compiler, e.g. zig cc -O2 -o prog prog.c
  wahc0 build <file.wah> [-o <program>] [--cc <compiler>]
                                    compile to <program>.c and build <program> with
                                    a C compiler (default: $ZIG, else zig)
  wahc0 run <file.wah> [args...]    compile and run
  wahc0 -e '<source>' [args...]     compile and run inline source`;

// Writes to stderr. A reader that has gone away ends wahc0 with 141, as for programs (§12.2).
function say(text) {
  try {
    writeSync(2, text);
  } catch (e) {
    if (e.code === 'EPIPE') process.exit(141);
    throw e;
  }
}

function die(msg, code = 2) {
  say(msg + '\n');
  process.exit(code);
}

function read(file) {
  try {
    return readFileSync(file, 'utf8');
  } catch (e) {
    return die(`wahc0: cannot read ${file}: ${e.message}`);
  }
}

function build(text, file, opts) {
  const r = compile(text, file, opts);
  for (const w of r.warnings) say(formatDiag(w, r.src) + '\n');
  if (!r.ok) die(r.errors.map((d) => formatDiag(d, r.src)).join('\n'));
  return opts?.target === 'c' ? r.c : r.js;
}

// programPath (§12.4): the .wah file under `run`; wahc0 itself under `-e`.
function run(js, filename, args, programPath) {
  globalThis.$wahHost = { args, programPath };
  runInThisContext(js, { filename });
}

// build: compiles to C, writes it beside the program, and runs a C compiler.
// Zig is called as `zig cc`; any other compiler gets the flags directly.
function buildProgram(args) {
  const file = args[0] ?? die(USAGE);
  let out = null, cc = process.env.ZIG || 'zig';
  for (let i = 1; i < args.length; i += 2) {
    const v = args[i + 1] ?? die(USAGE);
    if (args[i] === '-o') out = v;
    else if (args[i] === '--cc') cc = v;
    else die(USAGE);
  }
  if (out === null) {
    if (!file.endsWith('.wah')) die(`wahc0: ${file} does not end with .wah, so name the program with -o`);
    out = file.slice(0, -'.wah'.length) + (process.platform === 'win32' ? '.exe' : '');
  }
  const cFile = `${out.replace(/\.exe$/i, '')}.c`;
  writeFileSync(cFile, build(read(file), file, { target: 'c' }));
  const isZig = /^zig(\.exe)?$/i.test(cc.split(/[/\\]/).pop());
  const r = spawnSync(cc, [...(isZig ? ['cc'] : []), '-O2', '-o', out, cFile], { stdio: 'inherit' });
  if (r.error) die(`wahc0: cannot run ${cc}: install Zig (https://ziglang.org), or name a C compiler with --cc`);
  if (r.status !== 0) die(`wahc0: ${cc} failed with exit code ${r.status ?? r.signal}`);
}

const argv = process.argv.slice(2);
if (argv.length === 0 || argv[0] === '-h' || argv[0] === '--help') die(USAGE, argv.length ? 0 : 2);

if (argv[0] === 'run') {
  const file = argv[1] ?? die(USAGE);
  run(build(read(file), file), `${file}.js`, argv.slice(2), resolve(file));
} else if (argv[0] === 'build') {
  buildProgram(argv.slice(1));
} else if (argv[0] === '-e') {
  const source = argv[1] ?? die(USAGE);
  run(build(source, '<-e>', { requireExit: false }), '<-e>.js', argv.slice(2), fileURLToPath(import.meta.url));
} else {
  const file = argv[0];
  let target = 'js', out = null;
  for (let i = 1; i < argv.length; i += 2) {
    const v = argv[i + 1] ?? die(USAGE);
    if (argv[i] === '-o') out = v;
    else if (argv[i] === '--target' && (v === 'js' || v === 'c')) target = v;
    else die(USAGE);
  }
  out ??= `${file.replace(/\.wah$/, '')}.${target}`;
  writeFileSync(out, build(read(file), file, { target }));
}
