// wahc must load, check and compile programs made of several files (spec §13.1)
// exactly as wahc0 does: the same errors, naming the same files, and the same JavaScript.

import { test } from 'node:test';
import { agree } from './helpers.js';

const shapes = `;lineup (circle, square)Shape
;scheme (s)sides { suspect (s) { alibi circle.Shape { wh } alibi square.Shape { waaaah } } }
;sides, Shape bribe`;
const a = ';wah claim shifty n\n;[wah] claim shifty xs\n;[wah] claim fixed\n;{ k: wah } claim shifty r\n'
  + ';scheme (x)f { x }\n;scheme ()g { }\n;lineup (p, q)L\n;n, xs, fixed, r, f, g, L bribe';
const files = {
  'lib/a.wah': ';"common.wah" steal common\n;"init a"($print) claim ready\n;scheme ()a { "a sees {,(c.common)}" }\n;a bribe',
  'lib/b.wah': ';"./common.wah" steal common\n;"../top.wah" steal top\n;"init b"($print) claim ready\n;scheme ()b { "b" }\n;b bribe',
  'lib/common.wah': ';"init common"($print) claim ready\n;scheme ()c { "c" }\n;c bribe',
  'top.wah': ';"init top"($print) claim ready\n;wah claim one\n;one bribe',
  'shapes.wah': shapes,
  'a.wah': a,
  'c.wah': ';"a.wah" steal a\n;scheme ()h { }\n;h bribe',
  'x.wah': ';scheme ()f { "x" }\n;f bribe',
  'y.wah': ';scheme ()f { "y" }\n;f bribe',
  'counter.wah': ';[wh] claim shifty counts\n;scheme ()bump { ;++cheat wh[counts] }\n;scheme ()count { wh[counts] }\n;counts, bump, count bribe',
  'cfg.wah': ';{ name: "wah", size: { w: waah } } claim cfg\n;cfg bribe',
  'p.wah': ';wah claim secret\n;scheme ()f { secret }\n;f bribe',
  'notlib.wah': ';q($print)\n;!invited',
  'cycle1.wah': ';"cycle2.wah" steal c2\n;f bribe',
  'cycle2.wah': ';"cycle1.wah" steal c1\n;g bribe',
  'missing.wah': ';"b/c.wah" steal c\n;scheme ()f { }\n;f bribe',
  'broken.wah': ';wah claim\n;x bribe',
  'noname.wah': ';"one.wah" steal\n;x bribe',
  'warns.wah': ';x+wah claim y\n;y bribe',
  'one.wah': ';wah claim one\n;one bribe',
  'uno.wah': ';"./one.wah" steal o\n;one.o claim uno\n;uno bribe',
  'early.wah': ';scheme ()peek { late }\n;,(peek) claim x\n;wah claim late\n;x bribe',
  'bad1.wah': ';"x"($print)\n;scheme ()f { }\n;f bribe',
  'bad2.wah': ';scheme ()f { }\n;!doubt (real) { }\n;f bribe',
  'bad3.wah': ';scheme ()f { }',
  'bad4.wah': ';scheme ()f { }\n;f bribe\n;wah claim x',
  'bad5.wah': ';scheme ()f { ;wah claim x }\n;x, f, nope bribe',
  'bad6.wah': ';"one.wah" steal o\n;o bribe',
  'fine.wah': ';scheme ()f { ;"x"($print) ;wah !invited }\n;f bribe',
  'novalue.wah': ';scheme ()g { }\n;scheme ()h { ,(g) }\n;h, g bribe',
};

const programs = [
  // running programs: initialization order, private names, lineups, claims and containers across files
  ';"lib/a.wah" steal a ;"lib/b.wah" steal b ;"main"($print) ;,(a.a)($print) ;,(b.b)($print) ;!invited',
  ';"shapes.wah" steal sh\n;square.Shape.sh(sides.sh)($print)\n;suspect (circle.Shape.sh) { alibi circle.Shape.sh { } alibi square.Shape.sh { } }\n;"{square.Shape.sh}"($print)\n;!invited',
  ';"shapes.wah" steal sh\n;suspect (circle.Shape.sh) { alibi circle.Shape.sh { "c" } alibi square.Shape.sh { "s" } }($print)\n;!invited',
  ';"counter.wah" steal c ;,(bump.c) ;wa[10]h +cheat wh[counts.c] ;,(count.c)($print) ;!invited',
  ';"cfg.wah" steal c ;"{name.cfg.c} {w.size.cfg.c}"($print) ;!invited',
  ';"x.wah" steal x ;"y.wah" steal y ;scheme ()f { "own" } ;,(f.x)($print) ;,(f.y)($print) ;,(f)($print) ;!invited',
  ';"p.wah" steal p\n;wah claim secret\n;,(f.p)($print)\n;!invited',
  ';"x/../one.wah" steal o\n;"uno.wah" steal u\n;[one.o, uno.u]($print)\n;!invited',
  ';"early.wah" steal e ;x.e($print) ;!invited',
  ';"fine.wah" steal f ;,(f.f) ;!invited',
  // parsing
  ';wah claim x ;"a.wah" steal a ;!invited', '// comment\n;\n;"one.wah" steal o\n;!invited', ';scheme ()f { ;"a.wah" steal a }',
  ';x steal a', ';"{x}.wah" steal a', ';("a.wah") steal a', ';"a" steal a', ';"" steal a', ';"lib\\\\a.wah" steal a',
  ';"a.wah" steal', ';"a.wah" steal ;!invited', ';"a.wah" steal "a"', ';bribe', ';wah bribe', ';a, wah bribe',
  ';a, b, a bribe', ';f.m bribe', ';scheme ()f { ;f bribe }', ';"a.wah" steal a whine { }',
  ';x(f.m)', ';,(f.m)', ';!doubt (real) { ;x($print) (kind.node) }', ';x(f.$print)',
  // loading
  ';"nope.wah" steal n\n;!invited', ';"missing.wah" steal m\n;!invited', ';"cycle1.wah" steal c\n;!invited',
  ';"test.wah" steal t\n;!invited', ';"broken.wah" steal b\n;q($print)\n;!invited', ';"noname.wah" steal n\n;!invited',
  ';"warns.wah" steal w\n;y.w + wah($print)\n;!invited',
  // library rules
  ';"bad1.wah" steal b\n;!invited', ';"bad2.wah" steal b\n;!invited', ';"bad3.wah" steal b\n;!invited', ';"bad4.wah" steal b\n;!invited',
  ';"bad5.wah" steal b\n;!invited', ';"bad6.wah" steal b\n;!invited', ';"c.wah" steal c\n;,(h.c)\n;!invited', ';;f bribe\n;!invited',
  ';"notlib.wah" steal n\n;n.n($print)\n;!invited', ';"novalue.wah" steal v\n;,(h.v)($print)\n;,(g.v)($print)\n;!invited',
  // module names
  ...[';[n.a, (wah(f.a)), p.L.a, wh[xs.a], k.r.a] claim t ;wah cheat wh[xs.a] ;waah cheat k.r.a ;,(g.a)',
    ';scheme (n)f { ;wah claim L n } ;lineup (q)M ;wah claim xs',
    ';suspect (p.L.a) { alibi p.L.a { } alibi q.L.a { } }', ';wah claim a', ';scheme (a)k { }', ';a($print)', ';wah cheat a',
    ';wah cheat wh[a]', ';wah(a)', ';nope.a($print)', ';,(nope.a)', ';,(g.b)', ';wah claim b ;,(g.b)', ';,(n.a)', ';f.a($print)',
    ';L.a($print)', ';z.L.a($print)', ';p.M.a($print)', ';wah, waah(f.a)', ';wah cheat n.a', ';++cheat n.a', ';wah cheat p.L.a',
    ';wah cheat nope.a', ';suspect (p.L.a) { alibi p.L.a { } }', ';lineup (p, q)L ;suspect (p.L) { alibi p.L { } alibi q.L.a { } anyway { } }',
    ';suspect (wah) { alibi p.L.a { } alibi p.L.a { } anyway { } }', ';suspect (wah) { alibi p.M.a { } anyway { } }',
    ';suspect (wah) { alibi p.n.a { } anyway { } }', ';suspect (wah) { alibi p.L.b { } anyway { } }', ';,(g.a)($print)',
  ].map((body) => `;"a.wah" steal a\n${body}\n;!invited`),
  ';"a.wah" steal a\n;"c.wah" steal a\n;!invited', ';"a.wah" steal a\n;"./a.wah" steal b\n;!invited',
  ';"c.wah" steal c\n;,(g.a)\n;!invited', ';"c.wah" steal c\n;,(g.c)\n;!invited',
];

test('agrees on programs that steal libraries', () => {
  for (const p of programs) agree(p, files);
});
