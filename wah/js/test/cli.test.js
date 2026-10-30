import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { lex, KEYWORDS } from '../src/lexer.js';

const root = fileURLToPath(new URL('../../', import.meta.url));
const wahc0 = (...args) => spawnSync(process.execPath, ['js/bin/wahc0.js', ...args], { cwd: root, encoding: 'utf8' });
const wahc0In = (input, ...args) => spawnSync(process.execPath, ['js/bin/wahc0.js', ...args], { cwd: root, encoding: 'utf8', input });

// Runs inline source that writes forever to stream ('stdout' or 'stderr'), closes
// that stream after the first output, and resolves with { code, signal, stderr }.
function closeEarly(source, stream) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, ['js/bin/wahc0.js', '-e', source], { cwd: root });
    let stderr = '';
    if (stream === 'stdout') child.stderr.on('data', (d) => { stderr += d; });
    child[stream].once('data', () => child[stream].destroy());
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error('the program kept running after its reader went away'));
    }, 30000);
    child.on('exit', (code, signal) => {
      clearTimeout(timer);
      resolve({ code, signal, stderr });
    });
  });
}

test('§12.2 a broken pipe ends the program silently with 141', async () => {
  const out = await closeEarly(';lurk (real) { ;"wah"($print) }', 'stdout');
  assert.deepEqual(out, { code: 141, signal: null, stderr: '' });
  const err = await closeEarly(';lurk (real) { ;"wah"($eprint) }', 'stderr');
  assert.equal(err.code, 141);
});

test('examples/echo.wah', () => {
  const banner = "Simple input/output echo test. Type 'quit' to end.\n";
  const r = wahc0In('one\n\nthree', 'run', 'examples/echo.wah');
  assert.equal(r.stdout, `${banner}ECHO: one\nECHO: \nECHO: three\n`);
  assert.equal(r.status, 0);
  const q = wahc0In('hi\nquit\nafter\n', 'run', 'examples/echo.wah');
  assert.equal(q.stdout, `${banner}ECHO: hi\n`);
  assert.equal(q.status, 0);
});

test('examples/hello.wah', () => {
  const r = wahc0('run', 'examples/hello.wah');
  assert.equal(r.stdout, 'Hello, World\n');
  assert.equal(r.status, 0);
});

test('examples/fizzbuzz.wah', () => {
  const r = wahc0('run', 'examples/fizzbuzz.wah');
  const lines = r.stdout.trimEnd().split('\n');
  assert.equal(lines.length, 101);
  assert.deepEqual(lines.slice(0, 5), ['1', '2', 'WAH', '4', 'WAAAH']);
  assert.equal(lines[14], 'WAHWAH');
  assert.equal(lines[100], '1.5');
  assert.equal(r.status, 0);
});

test('examples/records.wah', () => {
  const r = wahc0('run', 'examples/records.wah');
  assert.equal(r.stderr, '');
  assert.deepEqual(r.stdout.trimEnd().split('\n'), [
    '((2 + 3) * 7) = 35', '2', 'Wario: 50', 'Waluigi: 3', 'Waluigi: 10 coins, 1 win', 'now 15, snapshot 10',
    'name = Waluigi', 'coins = 15', 'wins = 2', 'fake', 'real',
  ]);
  assert.equal(r.status, 0);
});

test('examples/suspect.wah', () => {
  const r = wahc0('run', 'examples/suspect.wah');
  assert.equal(r.stderr, '');
  assert.deepEqual(r.stdout.trimEnd().split('\n'), [
    '1 2 WAH 4 WAAAH WAH 7 8 WAH WAAAH 11 WAH 13 14 WAHWAH', '7', "unknown instruction 'dance', skipped", '70',
  ]);
  assert.equal(r.status, 0);
});

test('examples/lineup.wah', () => {
  const r = wahc0('run', 'examples/lineup.wah');
  assert.equal(r.stderr, '');
  assert.deepEqual(r.stdout.trimEnd().split('\n'), [
    'rock, paper, scissors are each a Hand', 'rock vs scissors: rock wins', 'scissors vs paper: scissors wins',
    'rock vs paper: paper wins', 'paper vs paper: draw', 'red -> green -> amber -> red -> green', 'fake',
  ]);
  assert.equal(r.status, 0);
});

test('examples/modules.wah', () => {
  const r = wahc0('run', 'examples/modules.wah');
  assert.equal(r.stderr, '');
  assert.deepEqual(r.stdout.trimEnd().split('\n'), [
    'text.wah: initialized', 'shapes.wah: initialized', 'modules.wah: running',
    'a circle has 0 corners', 'a triangle has 3 corners', 'a square has 4 corners', '7 corners in all',
  ]);
  assert.equal(r.status, 0);
});

// The stage 0 stress test: a Γ-- tokenizer written in Γ-- must agree with wahc0's own lexer.
const KIND = { int: 'numeral', float: 'numeral', sig: 'signal', str: 'string', std: 'stdlib', id: 'name' };
function expectedTokens(file) {
  const text = readFileSync(new URL(`../../${file}`, import.meta.url), 'utf8');
  const toks = lex(text).slice(0, -1).map((t) => {
    const kind = KIND[t.t] ?? (KEYWORDS.has(t.t) ? 'keyword' : t.t.startsWith('!') && t.t.length > 1 && t.t !== '!=' ? 'bang' : 'operator');
    return `${kind} ${text.slice(t.pos, t.end)}`;
  });
  return [...toks, `${toks.length} tokens`];
}

test('examples/fickle.wah', () => {
  const r = wahc0('run', 'examples/fickle.wah');
  assert.equal(r.stderr, '');
  assert.deepEqual(r.stdout.trimEnd().split('\n'), [
    'total: 10', '4 -> 3 -> 2 -> 1', 'fickle keeps it: real, shifty copies it: fake', 'through the alias: 3',
  ]);
  assert.equal(r.status, 0);
});

for (const file of ['examples/fizzbuzz.wah', 'examples/tokenize.wah', 'examples/records.wah', 'examples/suspect.wah',
  'examples/lineup.wah', 'examples/modules.wah', 'examples/modules/shapes.wah', 'examples/fickle.wah']) {
  test(`examples/tokenize.wah agrees with the wahc0 lexer on ${file}`, () => {
    const r = wahc0('run', 'examples/tokenize.wah', file);
    assert.equal(r.stderr, '');
    assert.deepEqual(r.stdout.trimEnd().split('\n'), expectedTokens(file));
  });
}

test('-e runs inline source with arguments', () => {
  const r = wahc0('-e', ';$args, ", "($join)($print)', 'x', 'y');
  assert.equal(r.stdout, 'x, y\n');
});

test('compile errors are reported with location and exit 2', () => {
  const r = wahc0('-e', ';wah claim x\n;x = waah');
  assert.equal(r.status, 2);
  assert.match(r.stderr, /<-e>:2:4: error: '=' is not an operator\n {2}hint: use 'claim' or 'cheat'\n {4};x = waah\n {7}\^/);
});

test('an uncaught tantrum exits 1', () => {
  const r = wahc0('-e', ';tantrum "nope"');
  assert.equal(r.status, 1);
  assert.match(r.stderr, /^Waluigi was not invited to this exception\ntantrum: nope/);
});

// ---- Draft 0.6 host behavior (§12) ----

const tmp = () => mkdtempSync(join(tmpdir(), 'wah-'));

test('§12.4 $programPath is the file the host started', () => {
  const dir = tmp();
  const src = join(dir, 'p.wah');
  writeFileSync(src, ';$programPath($print)\n;!invited');
  assert.equal(wahc0('run', src).stdout, `${src}\n`);
  assert.equal(wahc0(src, '-o', join(dir, 'p.js')).status, 0);
  const node = spawnSync(process.execPath, [join(dir, 'p.js')], { encoding: 'utf8' });
  assert.equal(node.stdout, `${join(dir, 'p.js')}\n`);
  assert.equal(wahc0('-e', ';$programPath($print)').stdout, `${join(root, 'js', 'bin', 'wahc0.js')}\n`);
  rmSync(dir, { recursive: true });
});

test('§12.4 $run passes arguments unchanged and returns the exit code', () => {
  const echo = 'process.stdout.write(JSON.stringify(process.argv.slice(1)) + "\\n")';
  const args = ['a b', 'q"x', '', 'c:\\dir\\', '$HOME', '*', 'é \u{1F608}'];
  const lit = (s) => JSON.stringify(s).replace(/\$/g, '\\$');
  const r = wahc0('-e', `;"node", [${['-e', echo, ...args].map(lit).join(', ')}]($run)($print)`);
  assert.equal(r.stdout, `${JSON.stringify(args)}\n0\n`);
  assert.equal(wahc0('-e', ';"node", ["-e", "process.exit(7)"]($run)($print)').stdout, '7\n');
  // output written before $run appears before the child's
  assert.equal(wahc0('-e', ';"first"($print) ;"node", ["-e", "console.log(2)"]($run) claim c').stdout, 'first\n2\n');
  // a program that cannot be started signals; one that starts and fails does not
  assert.equal(wahc0('-e', ';"no-such-program-wah", []($run) claim c whine (k) { -k } ;c($print)').stdout, '-1\n');
  assert.match(wahc0('-e', ';"node", ["x", wah]($run)').stderr, /tantrum: type error: \$run expects an array of purples/);
});

test('§12.1/§12.2 $readLine decoding and line endings', () => {
  const lines = ';lurk (real) { ;,($readLine) claim l whine { ;!invited } ;"[{l}]"($print) }';
  const read = (bytes) => spawnSync(process.execPath, ['js/bin/wahc0.js', '-e', lines], { cwd: root, input: Buffer.from(bytes) }).stdout.toString('utf8');
  assert.equal(read('a\n\nb'), '[a]\n[]\n[b]\n');
  assert.equal(read('a\r\nb\rc\n'), '[a]\n[b\rc]\n');
  assert.equal(read(''), '');
  assert.equal(read('\n'), '[]\n');
  // a byte-order mark is kept; invalid and incomplete sequences become U+FFFD
  assert.equal(read([0xef, 0xbb, 0xbf, 0x61, 0x0a]), '[\uFEFFa]\n');
  assert.equal(read([0x61, 0xc3, 0x28, 0x0a, 0x62, 0xe2, 0x82]), '[a\uFFFD(]\n[b\uFFFD]\n');
});

test('§12.3 $listDir: names sorted by code point', () => {
  const dir = tmp();
  // UTF-16 order would put U+10000 before U+E000
  for (const n of ['b', 'C', 'a', '\u{E000}', '\u{10000}', 'é']) writeFileSync(join(dir, n), '');
  mkdirSync(join(dir, 'sub'));
  const r = wahc0('-e', `;${JSON.stringify(dir).replace(/\$/g, '\\$')}($listDir), "|"($join)($print)`);
  assert.equal(r.stdout, `${['C', 'a', 'b', 'sub', 'é', '\u{E000}', '\u{10000}'].join('|')}\n`);
  rmSync(dir, { recursive: true });
});
