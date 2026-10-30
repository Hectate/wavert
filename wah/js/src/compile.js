// The compiler pipeline: load (lex + parse every module) -> check -> generate.

import { readFileSync } from 'node:fs';
import { Source, WahError, err } from './errors.js';
import { lex } from './lexer.js';
import { parse } from './parser.js';
import { check } from './check.js';
import { generate } from './codegen.js';
import { generateC } from './codegen_c.js';

const stripBom = (text) => text.replace(/^\uFEFF/, '');

// Returns { ok: true, js, src, warnings } or { ok: false, errors, src, warnings };
// with target 'c', the program is in c instead of js. Each diagnostic names
// its file in d.src. requireExit: the entry-file rule that the last statement
// is ;!invited (§13). readFile loads stolen libraries.
export function compile(text, file, { requireExit = true, readFile = (p) => readFileSync(p, 'utf8'), target = 'js' } = {}) {
  const src = new Source(file, stripBom(text));
  const warnings = [];
  try {
    const modules = load(src, readFile, warnings);
    check(modules, { requireExit });
    if (target === 'c') return { ok: true, c: generateC(modules), src, warnings };
    return { ok: true, js: generate(modules), src, warnings };
  } catch (e) {
    if (e instanceof WahError) return { ok: false, errors: e.diags, src, warnings };
    throw e;
  }
}

// ---- Modules (§13.1) ----
// A module is { src, path, program, steals: [{ node, lib }] }. The checker adds
// what it bribes, and the code generator numbers it.

// The directory part of a path, with its trailing '/'.
function dirOf(path) {
  return path.slice(0, path.lastIndexOf('/') + 1);
}

// Removes '.' segments, empty segments and 'dir/..' pairs.
export function normalize(path) {
  const out = [];
  for (const seg of path.split('/')) {
    if (seg === '.' || (seg === '' && out.length > 0)) continue;
    if (seg === '..' && out.length > 0 && out[out.length - 1] !== '..' && out[out.length - 1] !== '') out.pop();
    else out.push(seg);
  }
  return out.join('/');
}

function parseModule(src, warnings) {
  const first = warnings.length;
  try {
    return parse(lex(src.text, warnings), warnings);
  } catch (e) {
    if (e instanceof WahError) for (const d of e.diags) d.src ??= src;
    throw e;
  } finally {
    for (const w of warnings.slice(first)) w.src ??= src;
  }
}

// Loads the entry file and every library it steals, directly or indirectly.
// Returns the modules in initialization order: libraries depth first, in steal
// order, each before any file that steals it; the entry file last.
function load(entry, readFile, warnings) {
  const modules = [];
  const loaded = new Map();
  const fail = (src, pos, message) => {
    throw new WahError([{ ...err(pos, message), src }]);
  };
  // chain: the paths from the entry file to this module, for finding cycles.
  const visit = (src, path, chain) => {
    const mod = { src, path, program: parseModule(src, warnings), steals: [] };
    loaded.set(path, mod);
    for (const s of mod.program.stmts) {
      if (s.k !== 'steal') continue;
      const libPath = normalize(dirOf(path) + s.path);
      const cycle = chain.indexOf(libPath);
      if (cycle >= 0) fail(src, s.pathPos, `import cycle: ${[...chain.slice(cycle), libPath].join(' -> ')}`);
      let lib = loaded.get(libPath);
      if (!lib) {
        let text;
        try {
          text = readFile(libPath);
        } catch {
          fail(src, s.pathPos, `cannot read '${libPath}'`);
        }
        lib = visit(new Source(libPath, stripBom(text)), libPath, [...chain, libPath]);
      }
      mod.steals.push({ node: s, lib });
    }
    modules.push(mod);
    return mod;
  };
  const path = normalize(entry.file.replaceAll('\\', '/'));
  visit(entry, path, [path]);
  return modules;
}
