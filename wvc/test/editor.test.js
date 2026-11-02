// The VS Code extension's documentation (editors/vscode): builtins.json agrees
// with the specification and the checker, every export of the standard
// library is documented (§2.2), and the scanner behind hovers and completions
// reads Wavert as intended. These need neither Zig nor VS Code.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readdirSync } from 'node:fs';
import { join } from 'node:path';
import { readRepo, root } from './helpers.js';
import { generated } from '../../editors/vscode/scripts/builtins.mjs';

const require = createRequire(import.meta.url);
const docs = require('../../editors/vscode/src/docs.js');
const builtins = JSON.parse(readRepo('editors/vscode/builtins.json'));

test('builtins.json is generated from the specification and the checker', () => {
  assert.ok(readRepo('editors/vscode/builtins.json').replaceAll('\r\n', '\n') === generated(),
    'builtins.json is out of date: run node editors/vscode/scripts/builtins.mjs');
});

test('every stdlib name is documented in a table of the specification', () => {
  const names = [...readRepo('wvc/checker.wvt').matchAll(/\{ name: "(\w+)", arity: ("const"|\d+) \}/g)].map((m) => m[1]);
  assert.deepEqual(Object.keys(builtins).sort(), names.sort());
  for (const [name, b] of Object.entries(builtins)) {
    assert.ok(b.notes.length > 10, `$${name} has no notes`);
    assert.ok(b.call === 'constant' || b.call.endsWith(`($${name})`), `$${name}: ${b.call}`);
    assert.ok(!b.notes.includes('(below)'), `$${name}: ${b.notes}`);
  }
  assert.equal(builtins.sortBy.call, ';(arr, by)($sortBy)');
  assert.equal(builtins.args.arity, 'constant');
});

test('every export of the standard library has a documentation comment', () => {
  const files = readdirSync(join(root, 'lib')).filter((f) => f.endsWith('.wvt'));
  assert.ok(files.length >= 3, files.join(' '));
  for (const f of files) {
    const text = readRepo(`lib/${f}`);
    assert.ok(docs.header(text).startsWith(f.replace('.wvt', ':')), `${f} has no header comment naming it`);
    for (const d of docs.declarations(text).filter((x) => x.exported)) {
      assert.ok(d.doc.length > 0, `${f}: ${d.name} has no documentation comment`);
    }
  }
});

test('declarations, their signatures and documentation', () => {
  const text = [
    '// The file.',
    '',
    ';import math from "$lib/math.wvt"',
    ';enum Kind (orc,',
    '    rat)',
    '// Twice x.',
    '// Panics on text.',
    ';export fn twice(x) { x * 2 }',
    '',
    '// Not this: a blank line follows.',
    '',
    ';export const limit is 10    // a trailing comment',
    ';fn outer(ref g, var n) {',
    '    // A nested function.',
    '    ;fn inner() { 1 }',
    '    ;const local is 2',
    '}',
  ].join('\r\n');
  const ds = docs.declarations(text);
  assert.deepEqual(ds.map((d) => [d.kind, d.name, d.exported, d.signature, d.doc]), [
    ['enum', 'Kind', false, 'enum Kind (orc, rat)', ''],
    ['fn', 'twice', true, 'fn twice(x)', 'Twice x.\nPanics on text.'],
    ['const', 'limit', true, 'const limit is 10', ''],
    ['fn', 'outer', false, 'fn outer(ref g, var n)', ''],
    ['fn', 'inner', false, 'fn inner()', 'A nested function.'],
  ]);
  assert.equal(docs.header(text), 'The file.');
  assert.deepEqual(docs.imports(text), [{ name: 'math', path: '$lib/math.wvt' }]);
});

test('finding the name under the cursor', () => {
  const line = ';const blast is ((3, 4)(grid.pos), 2)(grid.circle)';
  const at = (s) => docs.nameAt(line, line.lastIndexOf(s) + 1);
  assert.deepEqual(at('circle'), { parts: ['grid', 'circle'], index: 1 });
  assert.deepEqual(at('grid.circle'), { parts: ['grid', 'circle'], index: 0 });
  assert.deepEqual(at('blast'), { parts: ['blast'], index: 0 });
  assert.deepEqual(docs.nameAt(';x($sortBy)', 5), { parts: ['$sortBy'], index: 0 });
  assert.deepEqual(docs.nameAt(';shapes.Shape.circle', 10), { parts: ['shapes', 'Shape', 'circle'], index: 1 });
  assert.equal(docs.nameAt(';(1, 2)', 3), null);
});

test('completing after a dot: the qualifier, and an enum\'s members', () => {
  assert.deepEqual(docs.qualifierBefore(';grid.'), ['grid']);
  assert.deepEqual(docs.qualifierBefore(';x(grid.ci'), ['grid']);
  assert.deepEqual(docs.qualifierBefore(';set t to world.Terrain.'), ['world', 'Terrain']);
  assert.deepEqual(docs.qualifierBefore('    Kind.o'), ['Kind']);
  assert.equal(docs.qualifierBefore(';const x is grid'), null);
  assert.equal(docs.qualifierBefore(';const x is 1.'), null);
  assert.equal(docs.qualifierBefore(';xs[0].'), null);
  const kind = docs.declarations(';enum Kind (orc,\r\n    rat, bat)').find((d) => d.name === 'Kind');
  assert.deepEqual(docs.enumMembers(kind), ['orc', 'rat', 'bat']);
  assert.deepEqual(docs.enumMembers(docs.declarations(';fn f() { 1 }')[0]), []);
});

test('imports lead to files, and $lib/ to the standard library', () => {
  const lib = join(root, 'lib');
  assert.equal(docs.resolveImport(join(root, 'examples/stdlib.wvt'), '$lib/math.wvt', lib), join(lib, 'math.wvt'));
  assert.equal(docs.resolveImport(join(root, 'examples/modules.wvt'), 'modules/shapes.wvt', null), join(root, 'examples/modules/shapes.wvt'));
  assert.equal(docs.resolveImport(join(root, 'x.wvt'), '$lib/math.wvt', null), null);
  // In the repository, lib/ is found from the file; elsewhere it comes from the settings.
  assert.equal(docs.findLib({ file: join(root, 'examples/stdlib.wvt') }), lib);
  assert.equal(docs.findLib({ libPath: lib }), lib);
  assert.equal(docs.findLib({ wvcPath: join(root, 'wvc/wvc.exe') }), lib);
  assert.equal(docs.findLib({ libPath: join(root, 'no-such-folder') }), null);
});
