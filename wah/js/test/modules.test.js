import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compile, normalize } from '../src/compile.js';
import { runWah } from './helpers.js';

const lines = (...xs) => xs.map((x) => `${x}\n`).join('');

// Compiles main.wah with library files; returns each error as "file:line: message".
function errors(main, files = {}) {
  const readFile = (p) => {
    if (Object.hasOwn(files, p)) return files[p];
    throw new Error(`no such file ${p}`);
  };
  const r = compile(main, 'main.wah', { readFile });
  if (r.ok) return [];
  return r.errors.map((d) => `${d.src.file}:${d.src.loc(d.pos).line}: ${d.message}`);
}
const one = (main, files, want) => {
  const errs = errors(main, files);
  assert.ok(errs.includes(want), `expected ${JSON.stringify(want)} in ${JSON.stringify(errs)}`);
};
const out = (main, files) => {
  const r = runWah(main, { files });
  if (r.compileErrors) throw new Error(`compile error: ${r.compileErrors.join('; ')}`);
  return r.out;
};

const shapes = `;lineup (circle, square)Shape
;scheme (s)sides { suspect (s) { alibi circle.Shape { wh } alibi square.Shape { waaaah } } }
;sides, Shape bribe`;

test('§13.1 paths', () => {
  assert.equal(normalize('a/./b/../c.wah'), 'a/c.wah');
  assert.equal(normalize('./x.wah'), 'x.wah');
  assert.equal(normalize('../lib/x.wah'), '../lib/x.wah');
  assert.equal(normalize('a//b.wah'), 'a/b.wah');
  assert.equal(normalize('a/../../b.wah'), '../b.wah');
  assert.equal(normalize('/abs/x.wah'), '/abs/x.wah');
});

test('§13.1 stealing and initialization', () => {
  // libraries run depth first, in steal order, once each; then the entry file
  const files = {
    'lib/a.wah': ';"common.wah" steal common\n;"init a"($print) claim ready\n;scheme ()a { "a sees {,(c.common)}" }\n;a bribe',
    'lib/b.wah': ';"./common.wah" steal common\n;"../top.wah" steal top\n;"init b"($print) claim ready\n;scheme ()b { "b" }\n;b bribe',
    'lib/common.wah': ';"init common"($print) claim ready\n;scheme ()c { "c" }\n;c bribe',
    'top.wah': ';"init top"($print) claim ready\n;wah claim one\n;one bribe',
  };
  assert.equal(out(';"lib/a.wah" steal a ;"lib/b.wah" steal b ;"main"($print) ;,(a.a)($print) ;,(b.b)($print) ;!invited', files),
    lines('init common', 'init a', 'init top', 'init b', 'main', 'a sees c', 'b'));
  // private names never conflict: every module above has its own 'ready'
  // claims, schemes and lineups can be stolen; members are member.Lineup.module
  assert.equal(out(`;"shapes.wah" steal sh
;square.Shape.sh(sides.sh)($print)
;suspect (circle.Shape.sh) { alibi circle.Shape.sh { ;"round"($print) } alibi square.Shape.sh { } }
;"{square.Shape.sh}: {square.Shape.sh($typeOf)}"($print)
;!invited`, { 'shapes.wah': shapes }), lines(4, 'round', 'square: Shape'));
  // a stolen shifty container can be changed through its contents, and its library sees the change
  const counter = ';[wh] claim shifty counts\n;scheme ()bump { ;++cheat wh[counts] }\n;scheme ()count { wh[counts] }\n;counts, bump, count bribe';
  assert.equal(out(';"counter.wah" steal c ;,(bump.c) ;wa[10]h +cheat wh[counts.c] ;,(count.c)($print) ;!invited', { 'counter.wah': counter }), '11\n');
  // a stolen shifty claim changed by its own library is seen as it changes
  const n = ';wh claim shifty n\n;scheme ()inc { ;++cheat n }\n;inc, n bribe';
  assert.equal(out(';"n.wah" steal m ;n.m($print) ;,(inc.m) ;,(inc.m) ;n.m($print) ;!invited', { 'n.wah': n }), lines(0, 2));
  // field access through a stolen record, and stolen names inside interpolation
  const cfg = ';{ name: "wah", size: { w: waah } } claim cfg\n;cfg bribe';
  assert.equal(out(';"cfg.wah" steal c ;"{name.cfg.c} {w.size.cfg.c}"($print) ;!invited', { 'cfg.wah': cfg }), 'wah 2\n');
  // two libraries may bribe the same name, and the importer may use it too
  const twins = { 'x.wah': ';scheme ()f { "x" }\n;f bribe', 'y.wah': ';scheme ()f { "y" }\n;f bribe' };
  assert.equal(out(';"x.wah" steal x ;"y.wah" steal y ;scheme ()f { "own" } ;,(f.x)($print) ;,(f.y)($print) ;,(f)($print) ;!invited', twins),
    lines('x', 'y', 'own'));
  // tantrums name the scheme, wherever it lives
  const r = runWah(';"t.wah" steal t ;,(boom.t) ;!invited', { files: { 't.wah': ';scheme ()boom { ;tantrum "no" }\n;boom bribe' } });
  assert.match(r.err, /tantrum: no\n {4}in boom\n/);
  // a library's claim read before it is initialized, through a scheme
  const early = { 'e.wah': ';scheme ()peek { late }\n;,(peek) claim x\n;wah claim late\n;x bribe' };
  assert.match(runWah(';"e.wah" steal e ;x.e($print) ;!invited', { files: early }).err, /'late' was used before its claim/);
});

test('§13.1 steal and bribe: parsing', () => {
  one(';wah claim x ;"a.wah" steal a ;!invited', {}, "main.wah:1: 'steal' must come before every other statement");
  assert.deepEqual(errors('// comment\n;\n;"a.wah" steal a\n;!invited', { 'a.wah': ';wah claim x ;x bribe' }), []);
  one(';scheme ()f { ;"a.wah" steal a }', {}, "main.wah:1: 'steal' is only allowed at the top level of a file");
  one(';x steal a', {}, 'main.wah:1: a steal path must be a plain string literal');
  one(';"{x}.wah" steal a', {}, 'main.wah:1: a steal path must be a plain string literal');
  one(';("a.wah") steal a', {}, 'main.wah:1: a steal path must be a plain string literal');
  one(';"a" steal a', {}, 'main.wah:1: a steal path includes the .wah extension');
  one(';"" steal a', {}, 'main.wah:1: a steal path includes the .wah extension');
  one(';"lib\\\\a.wah" steal a', {}, "main.wah:1: steal paths use '/' as the separator");
  one(';"a.wah" steal', {}, "main.wah:1: expected a name for the module after 'steal', found end of input");
  one(';"a.wah" steal ;!invited', {}, "main.wah:1: expected a name for the module after 'steal', found ';'");
  one(';"a.wah" steal "a"', {}, "main.wah:1: expected a name for the module after 'steal', found a string");
  one(';bribe', {}, "main.wah:1: 'bribe' needs the names it exports before it");
  one(';wah bribe', {}, 'main.wah:1: only names can be bribed');
  one(';a, wah bribe', {}, 'main.wah:1: only names can be bribed');
  one(';a, b, a bribe', {}, "main.wah:1: 'a' is already bribed");
  one(';f.m bribe', {}, 'main.wah:1: only names can be bribed');
  one(';scheme ()f { ;f bribe }', {}, "main.wah:1: 'bribe' is only allowed at the top level of a library");
  one(';"a.wah" steal a whine { }', {}, "main.wah:1: 'whine' must follow a statement whose value comes from a call");
});

test('§13.1 loading', () => {
  one(';"nope.wah" steal n\n;!invited', {}, "main.wah:1: cannot read 'nope.wah'");
  one(';"a.wah" steal a\n;!invited', { 'a.wah': ';"b/c.wah" steal c\n;scheme ()f { }\n;f bribe' }, "a.wah:1: cannot read 'b/c.wah'");
  one(';"a.wah" steal a\n;!invited', { 'a.wah': ';"b.wah" steal b\n;f bribe', 'b.wah': ';"a.wah" steal a\n;g bribe' },
    'b.wah:1: import cycle: a.wah -> b.wah -> a.wah');
  one(';"main.wah" steal m\n;!invited', {}, 'main.wah:1: import cycle: main.wah -> main.wah');
  // errors in a library name the library, and stop compilation like any lexer or parser error
  assert.deepEqual(errors(';"a.wah" steal a\n;q($print)\n;!invited', { 'a.wah': ';wah claim\n;x bribe' }),
    ["a.wah:2: expected a name after 'claim', found ';'"]);
  // a library is loaded once, whatever path spells it
  assert.deepEqual(errors(';"x/../a.wah" steal a\n;"b.wah" steal b\n;!invited',
    { 'a.wah': ';wah claim one\n;one bribe', 'b.wah': ';"./a.wah" steal a\n;one.a claim uno\n;uno bribe' }), []);
});

test('§13.1 libraries: checker', () => {
  const lib = (body) => ({ 'a.wah': body });
  const main = ';"a.wah" steal a\n;!invited';
  one(main, lib(';"x"($print)\n;scheme ()f { }\n;f bribe'),
    "a.wah:1: a library's top level may contain only steal, scheme, claim and lineup statements");
  one(main, lib(';scheme ()f { }\n;!doubt (real) { }\n;f bribe'), "a.wah:2: a library's top level may contain only steal, scheme, claim and lineup statements");
  one(main, lib(';scheme ()f { }'), 'a.wah:1: a library must end with the names it exports');
  one(main, lib(';scheme ()f { }\n;f bribe\n;wah claim x'), "a.wah:2: 'bribe' must be the last statement of a library");
  one(main, lib(';scheme ()f { }\n;f bribe\n;f bribe'), "a.wah:2: 'bribe' must be the last statement of a library");
  one(main, lib(';scheme ()f { ;wah claim x }\n;x bribe'), "a.wah:2: cannot bribe 'x': it is not declared at the top level");
  one(';"a.wah" steal a\n;!invited', { 'a.wah': ';"b.wah" steal b\n;b bribe', 'b.wah': ';scheme ()g { }\n;g bribe' },
    "a.wah:2: cannot bribe 'b': it is a module");
  one(';;f bribe\n;!invited', {}, "main.wah:1: 'bribe' must be the last statement of a library");
  // inside schemes, everything is allowed
  assert.deepEqual(errors(main, lib(';scheme ()f { ;"x"($print) ;wah !invited }\n;f bribe')), []);
  // a stolen file that is not a library is reported where it is stolen, and not checked itself
  assert.deepEqual(errors(';"a.wah" steal a\n;a.a($print)\n;!invited', lib(';q($print)\n;!invited')),
    ["main.wah:1: 'a.wah' is not a library: it ends with ;!invited"]);
});

test('§13.1 module names: checker', () => {
  const files = {
    'a.wah': ';wah claim shifty n\n;[wah] claim shifty xs\n;[wah] claim fixed\n;{ k: wah } claim shifty r\n'
      + ';scheme (x)f { x }\n;scheme ()g { }\n;lineup (p, q)L\n;n, xs, fixed, r, f, g, L bribe',
    'c.wah': ';"a.wah" steal a\n;scheme ()h { }\n;h bribe',
  };
  const ok = (body) => assert.deepEqual(errors(`;"a.wah" steal a\n${body}\n;!invited`, files), []);
  const bad = (body, want) => one(`;"a.wah" steal a\n${body}\n;!invited`, files, want);
  ok(';[n.a, (wah(f.a)), p.L.a, wh[xs.a], k.r.a] claim t ;wah cheat wh[xs.a] ;waah cheat k.r.a ;,(g.a)');
  // what a module bribes does not enter the file, so any name may be reused
  ok(';scheme (n)f { ;wah claim L n } ;lineup (q)M ;wah claim xs');
  ok(';suspect (p.L.a) { alibi p.L.a { } alibi q.L.a { } } ;suspect (wah) { alibi wah { "x" } anyway { "y" } } claim y');
  // the module name follows the scheme-name rules
  bad(';wah claim a', "main.wah:2: 'a' is already declared in this scope (line 1)");
  bad(';scheme (a)k { }', "main.wah:2: 'a' shadows 'a' from an enclosing scope (line 1)");
  one(';"a.wah" steal a\n;"c.wah" steal a\n;!invited', files, "main.wah:2: 'a' is already declared in this scope (line 1)");
  one(';"a.wah" steal a\n;"./a.wah" steal b\n;!invited', files, "main.wah:2: 'a.wah' is already stolen");
  bad(';a($print)', "main.wah:2: 'a' is a module, not a value");
  bad(';wah cheat a', "main.wah:2: 'a' is a module, not a binding");
  bad(';wah cheat wh[a]', "main.wah:2: 'a' is a module, not a binding");
  bad(';wah(a)', "main.wah:2: 'a' is not a scheme");
  // name.module must be bribed, and used as what it is
  bad(';nope.a($print)', "main.wah:2: 'a.wah' does not bribe 'nope'");
  bad(';,(nope.a)', "main.wah:2: 'a.wah' does not bribe 'nope'");
  bad(';,(g.b)', "main.wah:2: unknown module 'b'");
  bad(';wah claim b ;,(g.b)', "main.wah:2: 'b' is not a module");
  bad(';,(n.a)', "main.wah:2: 'n.a' is not a scheme");
  bad(';f.a($print)', "main.wah:2: 'f.a' is a scheme; functions are not values in stage 0");
  bad(';L.a($print)', "main.wah:2: 'L.a' is a lineup, not a value");
  bad(';z.L.a($print)', "main.wah:2: no member 'z' in lineup 'L'");
  bad(';p.M.a($print)', "main.wah:2: 'a.wah' does not bribe 'M'");
  bad(';wah, waah(f.a)', "main.wah:2: 'f.a' takes 1 argument, got 2");
  // only the module that declares a claim may cheat it; contents follow the usual rules
  bad(';wah cheat n.a', "main.wah:2: cannot cheat on 'n.a': only 'a.wah' may cheat it");
  bad(';++cheat n.a', "main.wah:2: cannot cheat on 'n.a': only 'a.wah' may cheat it");
  bad(';wah cheat p.L.a', "main.wah:2: cannot cheat on 'p.L.a': lineup members are constants");
  bad(';wah cheat nope.a', "main.wah:2: 'a.wah' does not bribe 'nope'");
  // suspects over a stolen lineup
  bad(';suspect (p.L.a) { alibi p.L.a { } }', "main.wah:2: suspect does not handle 'q.L.a'");
  bad(';lineup (p, q)L ;suspect (p.L) { alibi p.L { } alibi q.L.a { } anyway { } }', "main.wah:2: a suspect cannot mix members of 'L' and 'L.a'");
  bad(';suspect (wah) { alibi p.L.a { } alibi p.L.a { } anyway { } }', "main.wah:2: duplicate alibi value p.L.a (line 2)");
  bad(';suspect (wah) { alibi p.M.a { } anyway { } }', "main.wah:2: 'a.wah' does not bribe 'M'");
  bad(';suspect (wah) { alibi p.n.a { } anyway { } }', "main.wah:2: 'n.a' is not a lineup");
  bad(';suspect (wah) { alibi p.L.b { } anyway { } }', "main.wah:2: unknown module 'b'");
  // a stolen scheme whose value is used must produce one; the error is in its library
  bad(';,(g.a)($print)', "a.wah:6: 'g' never produces a value, but its value is used (main.wah line 2)");
  // stealing is not transitive
  one(';"c.wah" steal c\n;,(g.a)\n;!invited', files, "main.wah:2: unknown module 'a'");
  one(';"c.wah" steal c\n;,(g.c)\n;!invited', files, "main.wah:2: 'c.wah' does not bribe 'g'");
});
