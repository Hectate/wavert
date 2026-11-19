// Reads Wavert source for the editor, without the compiler: the declarations
// of a file with their documentation, its imports, and where they lead.
//
// Documentation is the run of // comment lines directly above a declaration,
// with no blank line between (§2.2). Declarations are found line by line, as
// the grammar finds them, so this is a guide for hovers and completions, not
// a parser: wvc is the authority.

'use strict';

const fs = require('fs');
const path = require('path');

// A declaration: { kind, name, params, signature, exported, doc, line }.
// kind is fn, const, var, ref or enum; params is the text between the
// parentheses of a fn or enum, else null; signature is what a hover shows.
// Functions are found at any depth, other declarations only at the top level.
function declarations(text) {
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  const out = [];
  for (let i = 0; i < lines.length; i++) {
    const m = /^(\s*);(export\s+)?(fn|const|var|ref|enum)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\()?/.exec(lines[i]);
    if (!m) continue;
    const [, indent, exported, kind, name, open] = m;
    if (kind !== 'fn' && indent.length > 0) continue;
    // A fn's parameters or an enum's members, which may run onto later lines.
    let params = null;
    if (open) {
      let rest = lines[i].slice(m[0].length);
      for (let k = i + 1; !rest.includes(')') && k < lines.length && k < i + 20; k++) rest += ' ' + lines[k];
      params = rest.includes(')') ? rest.slice(0, rest.indexOf(')')) : '';
    }
    const doc = [];
    for (let j = i - 1; j >= 0; j--) {
      const c = /^\s*\/\/ ?(.*)$/.exec(lines[j]);
      if (!c) break;
      doc.unshift(c[1]);
    }
    const hasParams = kind === 'fn' || kind === 'enum';
    const p = hasParams ? (params ?? '').replace(/\s+/g, ' ').trim() : null;
    let signature;
    if (kind === 'fn') signature = `fn ${name}(${p})`;
    else if (kind === 'enum') signature = `enum ${name} (${p})`;
    else signature = lines[i].trim().replace(/^;(export\s+)?/, '').replace(/\s*\/\/.*$/, '');
    if (signature.length > 120) signature = signature.slice(0, 117) + '...';
    out.push({ kind, name, params: p, signature, exported: !!exported, doc: doc.join('\n'), line: i });
  }
  return out;
}

// The comment at the top of a file, before its first statement: what a
// module is for.
function header(text) {
  const doc = [];
  for (const line of text.replace(/\r\n/g, '\n').split('\n')) {
    const c = /^\s*\/\/ ?(.*)$/.exec(line);
    if (!c) break;
    doc.push(c[1]);
  }
  return doc.join('\n');
}

// The imports of a file: [{ name, path }].
function imports(text) {
  return [...text.matchAll(/^\s*;import\s+([A-Za-z_][A-Za-z0-9_]*)\s+from\s+"([^"]*)"/gm)]
    .map((m) => ({ name: m[1], path: m[2] }));
}

// The file an import path leads to from the file at fromFile, or null: a
// '$lib/' path is in libDir, if known; any other path is relative (§13.1).
function resolveImport(fromFile, importPath, libDir) {
  if (importPath.startsWith('$')) {
    if (!importPath.startsWith('$lib/') || !libDir) return null;
    return path.join(libDir, importPath.slice(5));
  }
  return path.join(path.dirname(fromFile), importPath);
}

const isDir = (dir) => { try { return fs.statSync(dir).isDirectory(); } catch { return false; } };

// The standard library folder, found as wvc finds it (§15): the libPath
// setting; else lib/ next to the wvc of the wvcPath setting, then one level
// up; else, for a file in the Wavert repository, the repository's lib/
// (a folder above the file that holds both lib/ and wvc/wvc.wvt). null if none.
function findLib({ libPath = '', wvcPath = '', file = '' } = {}) {
  if (libPath) return isDir(libPath) ? libPath : null;
  if (wvcPath) {
    for (const dir of [path.join(path.dirname(wvcPath), 'lib'), path.join(path.dirname(wvcPath), '..', 'lib')]) {
      if (isDir(dir)) return path.normalize(dir);
    }
  }
  for (let dir = path.dirname(file); file; ) {
    if (isDir(path.join(dir, 'lib')) && fs.existsSync(path.join(dir, 'wvc', 'wvc.wvt'))) return path.join(dir, 'lib');
    const up = path.dirname(dir);
    if (up === dir) break;
    dir = up;
  }
  return null;
}

// The dotted name around column col of a line, such as grid.circle or
// $sortBy, and which part of it the column is in: { parts, index }, or null.
function nameAt(line, col) {
  const re = /\$?[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*/g;
  for (const m of line.matchAll(re)) {
    if (col < m.index || col > m.index + m[0].length) continue;
    const parts = m[0].split('.');
    let start = m.index;
    for (let k = 0; k < parts.length; k++) {
      if (col <= start + parts[k].length) return { parts, index: k };
      start += parts[k].length + 1;
    }
  }
  return null;
}

// The names before the last dot of the dotted name that text ends with, as
// text up to the cursor: ['world', 'Terrain'] for 'x(world.Terrain.de', and
// ['grid'] for ';grid.'. null if text does not end in a name and a dot,
// perhaps followed by the start of another name.
function qualifierBefore(text) {
  const m = /(?<![A-Za-z0-9_$])((?:[A-Za-z_][A-Za-z0-9_]*\.)+)[A-Za-z0-9_]*$/.exec(text);
  return m ? m[1].slice(0, -1).split('.') : null;
}

// The members of an enum declaration, in order: ['orc', 'rat'] for
// enum Kind (orc, rat).
function enumMembers(decl) {
  return (decl.params ?? '').split(',').map((s) => s.trim()).filter((s) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(s));
}

// The parameters of a fn declaration, as written: ['a', 'var b'] for 'a, var b'.
function parameters(decl) {
  return (decl.params ?? '').split(',').map((s) => s.trim()).filter((s) => s !== '');
}

// The parameters of a stdlib function, from its call form in builtins.json:
// ['x', 'i'] for ';(x, i)($at)', ['x'] for ';x($ceil)', [] for ';($time)'.
function builtinParameters(call) {
  const args = call.replace(/^;/, '').replace(/\(\$[A-Za-z0-9_]+\)$/, '');
  if (args === '') return [];
  if (args.startsWith('(') && args.endsWith(')')) return args.slice(1, -1).split(',').map((s) => s.trim());
  return [args];
}

const escapeSnippet = (s) => s.replace(/[\\$}]/g, '\\$&');

// A call of name, as a VS Code snippet with a placeholder for each
// parameter: '(${1:a}, ${2:b})(f)$0', '${1:x}(f)$0' or '(f)$0'.
function callSnippet(name, params) {
  const holes = params.map((p, i) => `\${${i + 1}:${escapeSnippet(p.replace(/^(var|ref)\s+/, ''))}}`);
  const f = `(${escapeSnippet(name)})`;
  if (holes.length === 0) return `${f}$0`;
  if (holes.length === 1) return `${holes[0]}${f}$0`;
  return `(${holes.join(', ')})${f}$0`;
}

// A call of name written out, for parameter hints: { label, ranges }, where
// ranges holds the [start, end] of each parameter in label.
function callSignature(name, params) {
  if (params.length === 0) return { label: `(${name})`, ranges: [] };
  if (params.length === 1) return { label: `${params[0]}(${name})`, ranges: [[0, params[0].length]] };
  const ranges = [];
  let label = '(';
  params.forEach((p, i) => {
    if (i > 0) label += ', ';
    ranges.push([label.length, label.length + p.length]);
    label += p;
  });
  return { label: `${label})(${name})`, ranges };
}

// Words that may come directly before a '(' that is not a call: '(' after
// one of them starts an expression, as after an operator.
const keywords = new Set(['if', 'else', 'while', 'for', 'match', 'case', 'return', 'exit', 'fail', 'panic', 'is', 'to', 'in', 'reversed', 'export']);

// Whether text, the source up to some point, ends with an expression and an
// opening parenthesis: whether a name written next is the function of a call,
// as in 'xs(', '5(' or ')(', rather than the start of a new expression, as in
// ';(', ', (' or 'if ('.
function calleeBefore(text) {
  const m = /([A-Za-z0-9_]+|[\])"}])\($/.exec(text);
  return m !== null && !keywords.has(m[1]);
}

// Which characters of text are code, rather than comments or the text of
// strings (an interpolation's expression is code): an array of 0 and 1.
function codeMask(text) {
  const mask = new Uint8Array(text.length);
  const holes = []; // per open interpolation, the depth of { inside it
  let inString = false;
  for (let i = 0; i < text.length; ) {
    const c = text[i];
    if (inString) {
      if (c === '\\') i += 2;
      else if (c === '"') { inString = false; i++; }
      else if (c === '{' && text[i + 1] === '{') i += 2;
      else if (c === '{') { holes.push(0); inString = false; i++; }
      else i++;
      continue;
    }
    if (c === '/' && text[i + 1] === '/') {
      while (i < text.length && text[i] !== '\n') i++;
      continue;
    }
    if (c === '/' && text[i + 1] === '*') {
      const end = text.indexOf('*/', i + 2);
      i = end < 0 ? text.length : end + 2;
      continue;
    }
    if (c === '"') { inString = true; i++; continue; }
    if (holes.length > 0 && c === '}') {
      if (holes[holes.length - 1] === 0) { holes.pop(); inString = true; i++; continue; }
      holes[holes.length - 1]--;
    } else if (holes.length > 0 && c === '{') holes[holes.length - 1]++;
    mask[i++] = 1;
  }
  return mask;
}

const closer = { '(': ')', '[': ']', '{': '}' };

// The call whose arguments the offset is among: { name, active }, where name
// is the function, such as 'f', 'grid.pos' or '$print', and active the index
// of the argument the offset is in. null if it is in none, or the call is not
// finished: in '(1, |)(f)' it is f's argument 1, and in 'x(f|)' it is in none.
function callAround(text, offset) {
  const mask = codeMask(text);
  const open = []; // the brackets open at offset, innermost last
  for (let i = 0; i < offset; i++) {
    if (!mask[i]) continue;
    const c = text[i];
    if (closer[c]) open.push(i);
    else if (open.length > 0 && c === closer[text[open[open.length - 1]]]) open.pop();
  }
  for (let k = open.length - 1; k >= 0; k--) {
    const p = open[k];
    if (text[p] !== '(' || calleeBefore(text.slice(0, p + 1))) continue;
    // The matching ')', and the call form directly after it.
    let depth = 0;
    let close = -1;
    let active = 0;
    for (let i = p; i < text.length; i++) {
      if (!mask[i]) continue;
      const c = text[i];
      if (closer[c]) depth++;
      else if (c === ')' || c === ']' || c === '}') {
        if (--depth === 0) { close = i; break; }
      } else if (c === ',' && depth === 1 && i < offset) active++;
    }
    if (close < 0) continue;
    const call = /^\((\$?[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?)\)/.exec(text.slice(close + 1, close + 200));
    if (call) return { name: call[1], active };
  }
  return null;
}

module.exports = {
  declarations, header, imports, resolveImport, findLib, nameAt, qualifierBefore, enumMembers,
  parameters, builtinParameters, callSnippet, callSignature, calleeBefore, codeMask, callAround,
};
