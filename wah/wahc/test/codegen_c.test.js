// wahc --target c must generate the same C as wahc0, byte for byte.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { compile } from '../../js/src/compile.js';
import { formatDiag } from '../../js/src/errors.js';
import { exampleLibraries, examples, harvest, readRepo, runWahc, wahcFiles } from './helpers.js';

const RUNTIME_C = 'runtime/wah.c';
const runtimeC = readRepo(RUNTIME_C);
const libraries = Object.fromEntries([...exampleLibraries(), ...wahcFiles()].map((f) => [f, readRepo(f)]));

// wahc compiling text to C: { out, err, code, c }.
function wahcC(text, files = {}) {
  const written = {};
  const r = runWahc(['test.wah', '--target', 'c', '-o', 'test.c'], { ...files, 'test.wah': text, [RUNTIME_C]: runtimeC }, written);
  return { ...r, c: written['test.c'] ?? null };
}

// wahc0 compiling text to C: { out, err, code, c }.
function expectedC(text, files = {}) {
  const readFile = (p) => {
    if (Object.hasOwn(files, p)) return files[p];
    throw new Error(`no such file ${p}`);
  };
  const r = compile(text, 'test.wah', { readFile, target: 'c' });
  const diags = r.ok ? r.warnings : [...r.warnings, ...r.errors];
  return { out: '', err: diags.map((d) => formatDiag(d, r.src) + '\n').join(''), code: r.ok ? 0 : 2, c: r.ok ? r.c : null };
}

// valid: the program must pass wahc0, so that code generation is what is compared.
function agreeC(text, files = {}, valid = false) {
  const got = wahcC(text, files), want = expectedC(text, files);
  if (valid) assert.equal(want.err, '', text);
  // On a mismatch, show the first differing line rather than two whole programs.
  if (got.c !== null && want.c !== null && got.c !== want.c) {
    const a = got.c.split('\n'), b = want.c.split('\n');
    const i = a.findIndex((line, k) => line !== b[k]);
    assert.fail(`${JSON.stringify(text)}\nline ${i + 1}:\n  wahc:  ${a[i]}\n  wahc0: ${b[i]}`);
  }
  assert.deepEqual(got, want, JSON.stringify(text));
}

for (const file of [...examples(), 'wahc/wahc.wah']) {
  test(`compiles ${file} to C exactly as wahc0 does`, () => {
    const text = readRepo(file);
    const r = compile(text, file, { readFile: readRepo, target: 'c' });
    assert.ok(r.ok);
    const written = {};
    const got = runWahc([file, '--target', 'c', '-o', 'out.c'], { ...libraries, [file]: text, [RUNTIME_C]: runtimeC }, written);
    assert.deepEqual(got, { out: '', err: '', code: 0 });
    assert.equal(written['out.c'], r.c);
  });
}

test('agrees on every snippet in the wahc0 tests', () => {
  for (const s of harvest()) {
    agreeC(s);
    agreeC(`${s}\n;!invited`);
  }
});

test('agrees on frames, captures and the temporal dead zone', () => {
  const programs = [
    // a nested scheme three deep, reading and cheating each level
    `;scheme (n)outer {
        ;wh claim shifty total
        ;scheme (k)mid {
            ;scheme ()inner { ;k +cheat total n + k }
            ,(inner)
        }
        ;wah(mid)($print)
        total
    }
    ;wa[10]h(outer)($print)`,
    // recursion, a scheme's own frame, sibling calls and a call from a grandchild
    `;scheme (n)count {
        ;scheme ()get { n }
        ;scheme ()twice { ;,(get) claim a ;,(get) claim b a + b }
        ;scheme ()deeper { ;scheme ()deepest { (,(twice)) } (,(deepest)) }
        ;!doubt (n == wh) { ;getaway (,(deeper)) }
        ;n - wah(count) claim rest
        ;,(get) claim mine
        rest + mine
    }`,
    // captured parameters, whine codes and claims in blocks, top level and loops
    `;scheme (shifty a, b)f {
        ;scheme ()g { ;wah +cheat a a + b }
        ;"x"($toInt) claim n whine (c) { ;scheme ()h { c } (,(h)) }
        ;!doubt (real) { ;wah claim z ;scheme ()y { z } ;,(y)($print) }
        (,(g))
    }
    ;wh claim shifty i
    ;lurk (i < waah) {
        ;scheme ()q { x }
        ;!doubt (i == wah) { ;,(q)($print) }
        ;i claim x
        ;++cheat i
    }
    ;"y"($toInt) claim m whine (code) { ;scheme ()r { code } (,(r)) }`,
    ';scheme ()f { ;wah cheat x x } ;,(f)($print) ;wh claim shifty x',
    // fickle (§5, §10.4): claims, cheats, and parameters, captured or not
    `;[wah] claim a ;a claim fickle f ;[waah] cheat f ;a cheat f ;{ v: wah } claim fickle r ;{ v: v.r } cheat r ;wah +cheat v.r
    ;scheme (fickle xs, shifty ys, zs)g {
        ;scheme ()h { ;[wah] cheat xs xs }
        ;ys cheat xs
        ;wah cheat wh[xs]
        (,(h))
    }
    ;scheme (fickle n)k { ;n + wah cheat n n }
    ;[wah] claim shifty s ;s, s, s(g) claim fickle b ;wah(k)($print)`,
    // argument order: temporaries, binding reads and constants
    `;scheme (s)say { ;s($print) s }
    ;scheme (a, b)pair { a + b }
    ;wah claim x
    ;("a"(say)) + ("b"(say))($print)
    ;x, ("b"(say))(pair) ;wah + ("c"(say))($print)
    ;[("c"(say)), ("d"(say))] claim shifty arr ;{ a: "g"(say), b: x } claim r
    ;"{x}{("e"(say))}"($print) ;("e"(say)) cheat wh[arr] ;("e"(say)) +cheat (,(f))[arr]
    ;scheme ()f { wh }`,
    // purples that need escapes, and record shapes shared and not
    ';["é😀", "\\u{1}\\u{7F}", "tab\\there", "\\"q\\" \\\\", "?? */", ""] claim a ;{ a: wah, b: waah } claim r ;{ a: wh, b: wh } claim s ;{ b: wh, a: wh } claim t ;{} claim e',
  ];
  for (const p of programs) agreeC(`${p}\n;!invited`, {}, true);
});

test('agrees on every module of wahc', () => {
  for (const f of wahcFiles().filter((f) => f !== 'wahc/wahc.wah')) {
    // A library compiles as a program that steals it.
    agreeC(`;"${f.slice('wahc/'.length)}" steal lib\n;!invited`, Object.fromEntries(wahcFiles().map((g) => [g.slice('wahc/'.length), readRepo(g)])));
  }
});

test('command line: --target', () => {
  const src = ';"hi"($print)\n;!invited';
  const want = compile(src, 'a.wah', { target: 'c' }).c;
  const written = {};
  assert.deepEqual(runWahc(['a.wah', '--target', 'c'], { 'a.wah': src, [RUNTIME_C]: runtimeC }, written), { out: '', err: '', code: 0 });
  assert.deepEqual(written, { 'a.c': want });
  // --target js is the default; an unknown target and a target with --ast are errors.
  assert.equal(runWahc(['a.wah', '--target', 'wasm'], { 'a.wah': src }).code, 2);
  assert.equal(runWahc(['--ast', 'a.wah', '--target', 'c'], { 'a.wah': src }).code, 2);
  assert.deepEqual(runWahc(['a.wah', '--target', 'c'], { 'a.wah': src }), { out: '', err: 'wahc: cannot read runtime/wah.c\n', code: 2 });
});

test('command line: build', () => {
  const src = ';"hi"($print)\n;!invited';
  // build always targets C; --cc only goes with build; build without .wah needs -o.
  for (const args of [['build', 'a.wah', '--target', 'c'], ['a.wah', '--cc', 'zig'], ['--ast', 'a.wah', '--cc', 'zig'],
    ['a.wah', 'build'], ['build'], ['build', 'a.wah', '--cc']]) {
    assert.equal(runWahc(args, { 'a.wah': src }).code, 2, JSON.stringify(args));
  }
  assert.deepEqual(runWahc(['build', 'prog'], { prog: src }),
    { out: '', err: 'wahc: prog does not end with .wah, so name the program with -o\n', code: 2 });
  // Compile errors come before anything is written.
  const written = {};
  assert.equal(runWahc(['build', 'a.wah'], { 'a.wah': ';x($print)\n;!invited', [RUNTIME_C]: runtimeC }, written).code, 2);
  assert.deepEqual(written, {});
});

test('command line: the runtime is found from where wahc is, too', () => {
  const src = ';"hi"($print)\n;!invited';
  const want = compile(src, 'a.wah', { target: 'c' }).c;
  // wahc runs as /repo/wahc/wahc.js: it tries /repo/wahc/<path>, then /repo/wahc/../<path>.
  for (const at of ['/repo/wahc/runtime/wah.c', '/repo/wahc/../runtime/wah.c']) {
    const written = {};
    assert.deepEqual(runWahc(['a.wah', '--target', 'c'], { 'a.wah': src, [at]: runtimeC }, written), { out: '', err: '', code: 0 }, at);
    assert.deepEqual(written, { 'a.c': want });
  }
  // A runtime given with --runtime is only looked for where it says.
  assert.equal(runWahc(['a.wah', '--target', 'c', '--runtime', 'wah.c'], { 'a.wah': src, '/repo/wahc/wah.c': runtimeC }).code, 2);
});
