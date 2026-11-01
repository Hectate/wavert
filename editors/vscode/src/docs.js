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

module.exports = { declarations, header, imports, resolveImport, findLib, nameAt };
