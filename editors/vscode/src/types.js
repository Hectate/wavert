// What wvc --types reports, read for the editor: its diagnostics, and the
// types of names, as JSON (wvc/checker.wvt, typesJson). Kept apart from the
// VS Code calls so that it can be tested without VS Code.
//
// The types come from the last version of a file that parsed, so the lines
// they give may have moved since. A name is looked up by its text and the
// nearest declaration above it, which survives most edits.

'use strict';

const path = require('path');

// The diagnostics in wvc's output: [{ file, line, col, severity, message, hint }],
// from lines such as "main.wvt:3:5: error: message", with "  hint: ..." after.
function parseDiagnostics(text) {
  const out = [];
  const lines = text.replace(/\r\n/g, '\n').split('\n');
  for (let i = 0; i < lines.length; i++) {
    const m = /^(.+):(\d+):(\d+): (error|warning): (.*)$/.exec(lines[i]);
    if (!m) continue;
    const hint = /^ {2}hint: (.*)$/.exec(lines[i + 1] ?? '');
    out.push({ file: m[1], line: Number(m[2]), col: Number(m[3]), severity: m[4], message: m[5], hint: hint ? hint[1] : null });
  }
  return out;
}

// Whether two paths name the same file. Windows paths ignore case and the
// direction of slashes.
function samePath(a, b, platform = process.platform) {
  const norm = (p) => {
    const n = path.resolve(p).replace(/\\/g, '/');
    return platform === 'win32' ? n.toLowerCase() : n;
  };
  return norm(a) === norm(b);
}

// The entries of info.names declared in file.
function namesIn(info, file) {
  if (!info) return [];
  const index = info.files.findIndex((f) => samePath(f, file));
  return index < 0 ? [] : info.names.filter((n) => n.file === index);
}

// The binding that name refers to on line (1-based) of file: the nearest
// declaration of it at or above the line, else the first below (a function
// may use a top-level binding declared after it). Not a function. null if none.
function bindingAt(info, file, name, line) {
  const found = namesIn(info, file).filter((n) => n.name === name && n.kind !== 'fn');
  const above = found.filter((n) => n.line <= line);
  if (above.length > 0) return above.reduce((a, b) => (b.line >= a.line ? b : a));
  return found.length > 0 ? found[0] : null;
}

// The function name declared in file, or null.
function functionIn(info, file, name) {
  return namesIn(info, file).find((n) => n.name === name && n.kind === 'fn') ?? null;
}

// The type reached from type through the fields of path, such as ['cursor', 'x']
// for state.cursor.x: null if a step is not known.
function fieldType(type, fields) {
  let t = type;
  for (const f of fields) {
    const field = t && t.fields ? t.fields.find((x) => x.name === f) : null;
    if (!field) return null;
    t = field.type;
  }
  return t;
}

module.exports = { parseDiagnostics, samePath, bindingAt, functionIn, fieldType };
