// wahc must generate the same JavaScript as wahc0, byte for byte, and must
// reproduce itself when compiled by itself (bootstrap stage 2, spec §15).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compile } from '../../js/src/compile.js';
import { lex } from '../../js/src/lexer.js';
import { parse } from '../../js/src/parser.js';
import { check } from '../../js/src/check.js';
import { Source, WahError } from '../../js/src/errors.js';
import { RUNTIME, agree, exampleLibraries, examples, harvest, readRepo, runWahc, runtime, stage1, wahcFiles } from './helpers.js';

// Does text pass wahc0's checker? Programs written to test codegen must. Some
// pass it and are then rejected by code generation, which is compared too.
// Programs that steal libraries are compiled whole instead.
function checks(text, files) {
  if (Object.keys(files).length > 0) {
    const readFile = (p) => {
      if (Object.hasOwn(files, p)) return files[p];
      throw new Error(`no such file ${p}`);
    };
    return compile(text, 'test.wah', { readFile }).ok;
  }
  const src = new Source('test.wah', text);
  try {
    check([{ src, path: 'test.wah', program: parse(lex(src.text)), steals: [] }]);
    return true;
  } catch (e) {
    if (e instanceof WahError) return false;
    throw e;
  }
}

// A program that must pass the checker, so that its code generation is compared.
function generates(text, files = {}) {
  const program = text.trimEnd().endsWith('!invited') ? text : `${text}\n;!invited`;
  assert.ok(checks(program, files), `wahc0 rejects ${program}`);
  agree(program, files);
}

// The examples' libraries and wahc's modules, for the programs that steal them.
const libraries = Object.fromEntries([...exampleLibraries(), ...wahcFiles()].map((f) => [f, readRepo(f)]));

for (const file of [...examples(), 'wahc/wahc.wah']) {
  test(`compiles ${file} exactly as wahc0 does`, () => {
    const text = readRepo(file);
    const r = compile(text, file, { readFile: readRepo });
    assert.ok(r.ok);
    const written = {};
    const got = runWahc([file, '-o', 'out.js'], { ...libraries, [file]: text, [RUNTIME]: runtime }, written);
    assert.deepEqual(got, { out: '', err: '', code: 0 });
    assert.equal(written['out.js'], r.js);
  });
}

test('stage 2: wahc compiled by itself reproduces itself', () => {
  const files = { ...libraries, [RUNTIME]: runtime };
  const stage2 = {};
  runWahc(['wahc/wahc.wah', '-o', 'wahc.js'], files, stage2);
  assert.equal(stage2['wahc.js'], stage1());
  const stage3 = {};
  assert.deepEqual(runWahc(['wahc/wahc.wah', '-o', 'wahc.js'], files, stage3, stage2['wahc.js']), { out: '', err: '', code: 0 });
  assert.equal(stage3['wahc.js'], stage2['wahc.js']);
});

test('agrees on every snippet in the wahc0 tests', () => {
  for (const s of harvest()) {
    agree(s);
    agree(`${s}\n;!invited`);
  }
});

test('agrees on statements', () => {
  const programs = [
    ';wah claim x ;x claim shifty y ;wah +cheat y ;++cheat y ;--cheat y ;x * waah **cheat y ;y($print)',
    ';[wah, waah] claim a ;[wah] claim shifty b ;a claim shifty c ;b claim d ;b cheat b ;wah cheat wh[b] ;wah +cheat wh[b]',
    ';{ n: wah } claim r ;{ n: wah, m: waah } claim shifty s ;waah cheat n.s ;wah +cheat n.s ;"m" claim k ;waah cheat k[s] ;wah *cheat k[s]',
    ';scheme (a, shifty b)f { ;a + b cheat b b } ;wah, waah(f)($print)',
    ';scheme ()f { } ;scheme (x)g { ;getaway x ;getaway } ;,(f) ;wah(g)',
    ';wh claim shifty i ;lurk (i < waah) { ;++cheat i ;!doubt (i == wah) { ;loiter } ;bail } ;!lurk (real) { }',
    ';!doubt (real) { ;"a"($print) } anyway !doubt (fake) { wah } anyway doubt (real) { } anyway { ;wah($print) waah }',
    ';wa[1.0]h claim shifty f ;wa[0.5]h +cheat f ;"s" claim shifty t ;"x{f}" +cheat t ;[wah] claim xs ;wh[xs] +cheat f ;wah claim one ;one -cheat f',
    ';tantrum "x"', ';!invited', ';waah !invited', ';wah + wah !invited',
    ';scheme (x)f { ;WAH! ;WAAAH! ;x(WAH!) }',
    ';scheme (x)f { x } ;"1"($toInt) claim a whine { wh } ;scheme (p, q)g { p } ;wah, (waah(f))(g) whine (c) { ;c($print) }',
    ';scheme (a, b, c)f { a } ;wah, "s", (wah + wah)(f) claim shifty r whine { wh } ;wah, [wah], (,($readLine))(f) cheat r whine { "x" }',
    ';scheme (x)f { x } ;scheme ()g { ;wah(f) !invited whine { waah } ;getaway wah(f) whine (c) { c } ;tantrum "e"(f) whine { "w" } }',
    ';scheme (x)f { x } ;wh claim shifty n ;wah(f) +cheat n whine { wh } ;wah(f)($print) whine { }',
    // fickle (§5, §10.4): claims and cheats keep values as they are; parameters are not copied
    ';[wah] claim a ;a claim fickle f ;[waah] cheat f ;a cheat f ;{ v: wah } claim fickle r ;{ v: v.r } cheat r ;wah +cheat v.r',
    ';scheme (fickle xs, shifty ys, zs)g { ;ys cheat xs ;[wah] cheat xs ;wah cheat wh[xs] xs } ;[wah] claim shifty a ;a, a, a(g) claim fickle b',
  ];
  for (const p of programs) generates(p);
});

test('agrees on expressions', () => {
  const programs = [
    ';[wah, WAH, wh, wa[100]h, WA[0xFF]H] claim a',
    ';[wa[0.5]h, WA[0.5]H, wa[1.0]h, WA[0.0]H, wa[1e21]h, wa[1e-7]h, wa[1.5e-7]h, wa[123.456]h, wa[1e300]h] claim a',
    ';["", "a", "{wah}", "a{wah}", "{wah}b", "a{wah}b{waah}c", "\\"q\\" \\\\ \\n \\t \\r \\u{1}\\u{1F608}", "{{}}"] claim a',
    ';[real, fake, uninvited, nobody, everybody, -everybody, $args] claim a',
    ';wah claim x ;[-x, !real, ~x, x + x - x * x / x % x ** x, x == x != real, x < x, x > x, x <= x, x >= x] claim a',
    ';wah claim x ;[x & x | x ^ x << x >> x, real && fake || real, real ? x : wh] claim a',
    ';[wah, waah] claim a ;{ k: { n: a } } claim r ;[wh[a], n.k.r, wh[n.k.r], "k"[r], k.r] claim b',
    ';{} claim e ;{ a: wah, b: [wah], c: { d: wah } } claim r',
    ';scheme (x)f { x } ;!doubt (real) { wah } anyway { waah } claim a ;(!doubt (real) { wah } anyway { waah })(f)',
    ';scheme (x)f { x } ;!doubt (real) { wah } anyway !doubt (fake) { waah } anyway doubt (fake) { waaah } anyway { wh } claim a',
    ';scheme (x)f { x } ;!doubt (real) { ;wah($print) wah } anyway { waah } claim a',
    ';scheme (x)f { x } ;wh claim shifty i ;lurk (i < waah) { ;!doubt (real) { ;lurk (fake) { ;bail } wah } anyway { ;wah(f) waah }($print) ;++cheat i }',
    ';scheme (x)f { x } ;!doubt (real) { !doubt (fake) { ;wah($print) wah } anyway { waah } } anyway { ;"s"($print) uninvited } claim a',
    ';scheme ()f { !doubt (real) { ;"x"($print) wah } anyway { waah } } ;,(f)($print)',
    ';scheme (x)f { x } ;scheme (a, b, c, d)h { a } ;wah, (waah(f)), (,($readLine)), [wah(f)](h)',
    ';scheme (x)f { x } ;scheme ()g { !doubt (real) { ;getaway wah } anyway { waah } }',
    ';wh claim shifty i ;lurk (real) { ;!doubt (real) { ;bail wah } anyway { waah } claim x }',
    ';scheme (n)f { !doubt (n < waah) { n } anyway { (n - wah(f)) + (n - waah(f)) } } ;wa[10]h(f)($print)',
    // suspect (§7.5): statement, plain value, and value with statements in its blocks
    ';wah claim x ;suspect (x) { alibi wah, waah { ;"a"($print) } alibi "s" { } anyway { ;"z"($print) } } ;suspect (x) { alibi real { } }',
    ';wah claim x ;suspect (x) { alibi wah { "one" } alibi WAH, wh { "small" } anyway { "?" } }($print)',
    ';scheme (x)f { x } ;wah claim x ;suspect (x(f)) { alibi wah { ;"side"($print) wah(f) } anyway { suspect (x) { alibi wh { "z" } anyway { "n" } } } } claim r',
    ';scheme ()g { suspect (wah) { alibi wah { ;getaway wah } anyway { waah } } }',
    ';wh claim shifty i ;lurk (real) { ;suspect (i) { alibi wah { ;bail } anyway { ;++cheat i ;loiter } } }',
    ';"{suspect (wah) { alibi uninvited { "n" } anyway { "{wah}" } }}"($print)',
    // lineups (§4.1): created first, members as values, and exhaustive suspects
    ';scheme ()f { b.L } ;lineup (a, b)L ;[a.L, (,(f)), { m: b.L }] claim xs ;lineup (only)One ;only.One($print)',
    ';lineup (a, b)L ;a.L claim x ;suspect (x) { alibi a.L { ;"a"($print) } alibi b.L { } }',
    ';lineup (a, b)L ;a.L claim x ;suspect (x) { alibi a.L { ;"a"($print) } anyway { } }',
    ';lineup (a, b)L ;a.L claim x ;suspect (x) { alibi a.L { "a" } alibi b.L { "b" } }($print)',
    ';lineup (a, b)L ;a.L claim x ;suspect (x) { alibi a.L { ;"s"($print) "a" } alibi b.L { "b" } } claim y',
  ];
  for (const p of programs) generates(p);
});

test('command line', () => {
  const usage = 'usage:\n'
    + '  wahc0 run wahc/wahc.wah <file.wah> [-o <out.js>] [--runtime <runtime.js>]\n'
    + '  wahc0 run wahc/wahc.wah <file.wah> --target c [-o <out.c>] [--runtime <wah.c>]\n'
    + '  wahc0 run wahc/wahc.wah build <file.wah> [-o <program>] [--cc <compiler>] [--runtime <wah.c>]\n'
    + '  wahc0 run wahc/wahc.wah --tokens <file.wah>\n'
    + '  wahc0 run wahc/wahc.wah --ast <file.wah>\n';
  const src = ';"hi"($print)\n;!invited';
  for (const args of [[], ['a.wah', 'b.wah'], ['a.wah', '-o'], ['--runtime'], ['-x', 'a.wah'], ['--ast'], [''],
    ['--ast', 'a.wah', '-o', 'b.js'], ['--tokens', 'a.wah', '--runtime', 'r.js']]) {
    assert.deepEqual(runWahc(args, { 'a.wah': src }), { out: '', err: usage, code: 2 }, JSON.stringify(args));
  }
  const want = compile(src, 'a.wah').js;
  for (const [file, out] of [['dir/prog.wah', 'dir/prog.js'], ['prog', 'prog.js'], ['a.wahx', 'a.wahx.js'], ['.wah', '.js']]) {
    const written = {};
    assert.deepEqual(runWahc([file], { [file]: src, [RUNTIME]: runtime }, written), { out: '', err: '', code: 0 });
    assert.deepEqual(written, { [out]: want }, file);
  }
  const written = {};
  assert.equal(runWahc(['--runtime', 'rt.js', 'a.wah', '-o', 'o.js'], { 'a.wah': src, 'rt.js': '// rt' }, written).code, 0);
  assert.ok(written['o.js'].startsWith('"use strict";\n// rt\n$rt.main(function () {\n'));
  assert.deepEqual(runWahc(['a.wah', '--runtime', 'rt.js'], { 'a.wah': src }),
    { out: '', err: 'wahc: cannot read rt.js\n', code: 2 });
  assert.deepEqual(runWahc(['a.wah', '-o', 'readonly/a.js'], { 'a.wah': src, [RUNTIME]: runtime }),
    { out: '', err: 'wahc: cannot write readonly/a.js\n', code: 2 });
  assert.deepEqual(runWahc(['missing.wah']), { out: '', err: 'wahc: cannot read missing.wah\n', code: 2 });
  // warnings are reported, and the program is still written
  const w = {};
  const r = runWahc(['a.wah'], { 'a.wah': ';;"hi"($print)\n;!invited', [RUNTIME]: runtime }, w);
  assert.equal(r.code, 0);
  assert.match(r.err, /^a\.wah:1:2: warning: ';;'/);
  assert.ok(w['a.js']);
});
