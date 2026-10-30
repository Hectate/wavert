// Compares runtime/wah.c with js/src/runtime.js: the probe (probe.c) runs each
// operation natively, and the same operation runs here through the JS runtime.
import { before, test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { cc, exe, noZig, root, tmp } from './helpers.js';

const $rt = new Function(`${readFileSync(join(root, 'js/src/runtime.js'), 'utf8')}\nreturn $rt;`)();
const { F } = $rt;

// A seeded generator, so a failure can be reproduced.
let seed = Number(process.env.SEED ?? 20261015);
function rand() {
  seed = (seed + 0x6d2b79f5) | 0;
  let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
  t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
  return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
}
const int = (n) => Math.floor(rand() * n);
const pick = (xs) => xs[int(xs.length)];

const bitsOf = (d) => {
  const v = new DataView(new ArrayBuffer(8));
  v.setFloat64(0, Number.isNaN(d) ? NaN : d);
  return v.getBigUint64(0).toString(16).padStart(16, '0');
};
const doubleOf = (bits) => {
  const v = new DataView(new ArrayBuffer(8));
  v.setBigUint64(0, bits);
  return v.getFloat64(0);
};
const hex = (s) => Buffer.from(s, 'utf8').toString('hex');

// A value as a probe argument.
function arg(v) {
  if (Buffer.isBuffer(v)) return `s:${v.toString('hex')}`;
  if (typeof v === 'number') return `i:${v}`;
  if (v instanceof F) return `f:${bitsOf(v.v)}`;
  if (typeof v === 'string') return `s:${hex(v)}`;
  if (typeof v === 'boolean') return `b:${v ? 1 : 0}`;
  if (v === null) return 'n';
  if (Array.isArray(v)) return `[ ${v.map(arg).join(' ')} ]`.replace('[  ]', '[ ]');
  throw new Error(`cannot encode ${v}`);
}
// A result as the probe prints it.
function result(v) {
  if (typeof v === 'number') return `int:${v}`;
  if (v instanceof F) return `float:${bitsOf(v.v)}`;
  if (typeof v === 'string') return `purple:${hex(v)}`;
  if (typeof v === 'boolean') return `boolean:${v ? 'real' : 'fake'}`;
  if (v === null) return 'uninvited';
  if (Array.isArray(v)) return `[${v.map((x) => ` ${result(x)}`).join('')} ]`;
  return '?';
}

const show = (v) => (typeof v === 'string' ? JSON.stringify(v) : $rt.tantrum(v).message);
const OPS = {
  ...Object.fromEntries('add sub mul div mod pow neg eq ne lt le gt ge band bor bxor bnot shl shr not text idx cond'
    .split(' ').map((k) => [k, $rt[k]])),
  ...Object.fromEntries('length at slice code fromCode split join toPurple toWah toInt toFloat floor ceil round trunc toUint32 isNobody same typeOf'
    .split(' ').map((k) => [k, $rt.std[k]])),
  show: (v) => show(v),
  decode: (b) => new TextDecoder('utf-8', { ignoreBOM: true }).decode(b),
};

// Ints are never -0 in a program, though the generators can make one.
const norm = (v) => (typeof v === 'number' ? v + 0 : Array.isArray(v) ? Object.freeze(v.map(norm)) : v);

// What the JS runtime gives for an operation, in the probe's notation.
function expected(op, args) {
  try {
    return result(OPS[op](...args.map(norm)));
  } catch (e) {
    if (e instanceof $rt.Wah) return `WAH ${e.from}`;
    if (e instanceof $rt.Tantrum) return `! ${e.message}`;
    throw e;
  }
}

let probe, dir;
before(() => {
  if (noZig) return;
  dir = tmp();
  probe = cc([join(root, 'runtime/test/probe.c')], join(dir, `probe${exe}`));
});

// How many doubles apart two probe results are (both floats of the same sign), else Infinity.
function ulps(a, b) {
  if (!a.startsWith('float:') || !b.startsWith('float:')) return Infinity;
  const x = BigInt('0x' + a.slice(6)), y = BigInt('0x' + b.slice(6));
  if ((x >> 63n) !== (y >> 63n)) return Infinity;
  return Number(x > y ? x - y : y - x);
}

// Runs cases ([op, ...args]) through the probe and compares each result.
// ** on floats may differ from V8 in the last bit: V8 does not use one pow
// algorithm, and the runtime uses fdlibm's, so it is the same on every platform.
let checks = 0;
function check(cases, input = '') {
  const file = join(dir, `cases${checks++}.txt`); // kept, to rerun a failure by hand
  writeFileSync(file, cases.map(([op, ...args]) => [op, ...args.map(arg)].join(' ')).join('\n'));
  const r = spawnSync(probe, [file], { input, maxBuffer: 1 << 30, timeout: 300000 });
  assert.equal(r.stderr.toString(), '');
  assert.equal(r.status, 0);
  const got = r.stdout.toString().split('\n');
  let bad = 0;
  const report = [];
  cases.forEach(([op, ...args], i) => {
    const want = expected(op, args);
    if (got[i] !== want && !(op === 'pow' && ulps(got[i], want) <= 1)) {
      bad++;
      if (report.length < 20) report.push(`${op} ${args.map(arg).join(' ')}\n    C:  ${got[i]}\n    JS: ${want}`);
    }
  });
  assert.equal(bad, 0, `${bad} of ${cases.length} differ:\n${report.join('\n')}`);
  return got.slice(cases.length);
}

// ---- generators ----

function randomDouble() {
  switch (int(8)) {
    case 0: return doubleOf(BigInt.asUintN(64, BigInt(int(2 ** 32)) << 32n | BigInt(int(2 ** 32))));
    case 1: return 2 ** (int(2098) - 1074) * pick([1, -1]); // powers of two, lopsided intervals
    case 2: return Number(`${int(1e9)}e${int(60) - 30}`);
    case 3: return (int(2 ** 31) - 2 ** 30) / pick([1, 2, 3, 7, 10, 100, 1000]);
    case 4: return Number(`1e${int(50) - 25}`) * pick([1, 0.999999, 1.000001, 9.9999999]);
    case 5: return int(2 ** 53) * pick([1, -1, 0.5, 1024]);
    case 6: return pick([0, -0, NaN, Infinity, -Infinity, 5e-324, 1.7976931348623157e308, 2.2250738585072014e-308, 0.1, 0.2, 0.3, 1e21, 1e-7, 1e-6, 123e-20]);
    default: return rand() * 10 ** (int(40) - 20);
  }
}

// The exact decimal value of the midpoint between d (> 0, finite) and the next double up.
function midpoint(d) {
  const bits = BigInt('0x' + bitsOf(d));
  const expBits = Number((bits >> 52n) & 0x7ffn);
  let m = bits & ((1n << 52n) - 1n), e;
  if (expBits === 0) e = -1074;
  else { m |= 1n << 52n; e = expBits - 1075; }
  const num = 2n * m + 1n; // midpoint = num * 2^(e-1)
  const k = e - 1;
  if (k >= 0) return (num << BigInt(k)).toString();
  const digits = (num * 5n ** BigInt(-k)).toString().padStart(-k + 1, '0');
  return `${digits.slice(0, k)}.${digits.slice(k)}`;
}

function randomFloatText() {
  switch (int(6)) {
    case 0: return midpoint(Math.abs(randomDouble()) || 1);
    case 1: {
      const d = Math.abs(randomDouble());
      return Number.isFinite(d) && d > 0 ? midpoint(d) : '1.5';
    }
    case 2: return `${pick(['', '-'])}${int(1e6)}${rand() < 0.5 ? `.${int(1e9)}` : ''}${rand() < 0.5 ? `${pick(['e', 'E'])}${pick(['', '+', '-'])}${int(400)}` : ''}`;
    case 3: return `${'9'.repeat(int(30) + 1)}.${'9'.repeat(int(30) + 1)}e${int(330)}`;
    case 4: return pick([
      '1.7976931348623157e308', '1.7976931348623158e308', '1.7976931348623159e308', '2e308',
      '4.9e-324', '2.4703282292062328e-324', '2.4703282292062327e-324', '2.4703282292062329e-324',
      '1e-400', '-1e-400', '0', '-0', '0.0', '00.5', '1.', '.5', '+1', ' 1', '1e', '1e+', 'e5', '1_0', '١',
      '9007199254740993', '0.1', '-2', '6.02e23', 'nobody', 'everybody',
    ]);
    default: return String(randomDouble());
  }
}

const WAHS = ['wh', 'wah', 'waaaaaaaaaah', 'WH', 'WAH', 'WAAAH', 'wa[42]h', 'WA[42]H', 'wa[0xFF]h', 'wa[0xff]h', 'wa[0XFF]h',
  'WA[0b101]H', 'wa[0b]h', 'wa[0x]h', 'wa[]h', 'wa[1.5]h', 'WA[1.5]H', 'WA[0.0]H', 'wa[1e3]h', 'wa[1e999]h', 'wa[9007199254740991]h',
  'wa[9007199254740992]h', 'wa[0x1FFFFFFFFFFFFF]h', 'wa[0x20000000000000]h', 'waah ', 'wAh', 'Wah', 'waH', 'w', 'h', 'wa[[1]]h',
  'wa[1]]h', 'waa[1]h', 'wa[1]', 'wa[-1]h', 'wa[007]h'];

function randomIntText() {
  switch (int(4)) {
    case 0: return pick(WAHS);
    case 1: return `${pick(['', '-', '+', '--'])}${int(10 ** int(17))}`;
    case 2: return pick(['9007199254740991', '-9007199254740991', '9007199254740992', '-9007199254740992', '00000000000000000000042', '-0', '', '-', '1.0', '1e3', '0x10', ' 1']);
    default: return randomFloatText();
  }
}

const CHARS = ['a', 'b', ' ', ',', 'é', 'ß', '€', '中', '😀', '𝄞', '\n', '"', '\\', '\u0001', '\u007f', ' ', '﻿'];
const randomText = (n = int(8)) => Array.from({ length: n }, () => pick(CHARS)).join('');

function randomBytes() {
  const pieces = [[0x61], [0xc3, 0xa9], [0xe2, 0x82, 0xac], [0xf0, 0x9f, 0x98, 0x80], [0x80], [0xbf], [0xc0], [0xc1, 0x80],
    [0xc2], [0xe0, 0x80], [0xe0, 0xa0], [0xed, 0xa0, 0x80], [0xed, 0x9f, 0xbf], [0xf0, 0x8f], [0xf0, 0x90, 0x80], [0xf4, 0x90],
    [0xf4, 0x8f, 0xbf, 0xbf], [0xf5], [0xff], [0xfe], [0xef, 0xbb, 0xbf], [0x0a], [0x0d], [0x00]];
  const out = [];
  const n = int(10);
  for (let i = 0; i < n; i++) out.push(...(rand() < 0.2 ? [int(256)] : pick(pieces)));
  return Buffer.from(out);
}

const INTS = [0, 1, -1, 2, 3, -7, 31, 32, 255, 2 ** 31 - 1, -(2 ** 31), 2 ** 31, 2 ** 32, 94906265, 94906266,
  2 ** 53 - 1, -(2 ** 53 - 1), 2 ** 26, 3037000499, 10, 100];
const SCALARS = () => [
  pick(INTS), int(2001) - 1000, new F(randomDouble()), new F(pick([0, -0, NaN, Infinity, -Infinity, 0.5, -2.5, 3])),
  randomText(3), pick([true, false, null]), [], [1, 'a'], pick(INTS) * pick([1, -1]),
];
const value = () => pick(SCALARS());

// ---- tests ----

test('float text (§9.3) matches JavaScript', { skip: noZig }, () => {
  const cases = [];
  for (let i = 0; i < 60000; i++) cases.push(['text', new F(randomDouble())]);
  for (let k = -1074; k <= 1023; k++) cases.push(['text', new F(2 ** k)]);
  for (let i = 0; i < 5000; i++) cases.push(['toWah', new F(randomDouble())]);
  check(cases);
});

test('$toFloat (§9.5) rounds correctly', { skip: noZig }, () => {
  const cases = [];
  for (let i = 0; i < 40000; i++) cases.push(['toFloat', randomFloatText()]);
  for (const w of WAHS) cases.push(['toFloat', w]);
  check(cases);
});

test('$toInt (§9.5)', { skip: noZig }, () => {
  const cases = [];
  for (let i = 0; i < 10000; i++) cases.push(['toInt', randomIntText()]);
  for (const w of WAHS) cases.push(['toInt', w]);
  cases.push(['toInt', 5], ['toInt', new F(1.5)], ['toInt', null], ['toFloat', true], ['toFloat', 3], ['toFloat', new F(-0)]);
  check(cases);
});

test('$toWah', { skip: noZig }, () => {
  const cases = [];
  for (let n = -12; n <= 12; n++) cases.push(['toWah', n]);
  for (let i = 0; i < 2000; i++) cases.push(['toWah', pick(INTS) * pick([1, -1])], ['toWah', value()]);
  check(cases);
});

test('UTF-8 decoding (§12.1) matches TextDecoder', { skip: noZig }, () => {
  const cases = [];
  for (let i = 0; i < 30000; i++) cases.push(['decode', randomBytes()]);
  check(cases);
});

test('operators (§11) match on mixed values', { skip: noZig }, () => {
  const bin = 'add sub mul div mod pow eq ne lt le gt ge band bor bxor shl shr'.split(' ');
  const un = 'neg bnot not text show cond floor ceil round trunc toUint32 isNobody typeOf toPurple length'.split(' ');
  const cases = [];
  for (let i = 0; i < 40000; i++) cases.push([pick(bin), value(), value()]);
  for (let i = 0; i < 10000; i++) cases.push([pick(un), value()]);
  // Integer edge cases, where overflow checks matter.
  for (let i = 0; i < 20000; i++) {
    const a = pick(INTS) * pick([1, -1]), b = pick(INTS) * pick([1, -1]);
    cases.push([pick(['add', 'sub', 'mul', 'mod', 'shl', 'shr', 'band', 'bor', 'bxor']), a, b]);
    cases.push(['pow', pick([0, 1, -1, 2, -2, 3, 10, 94906265, 2 ** 26]), pick([0, 1, 2, 3, 4, 10, 52, 53, 60, -1, -2])]);
    cases.push(['shl', a, int(40) - 4], ['mod', a, pick(INTS)]);
  }
  // pow on floats, where JavaScript and C differ in corner cases.
  const pf = [NaN, Infinity, -Infinity, 1, -1, 0, -0, 0.5, -0.5, 2, -2, 3.5];
  for (const a of pf) for (const b of pf) cases.push(['pow', new F(a), new F(b)]);
  for (const a of pf) for (const b of [0, 1, -1, 2]) cases.push(['pow', new F(a), b], ['pow', b, new F(a)]);
  check(cases);
});

test('purple and array builtins (§9.4, §10.6)', { skip: noZig }, () => {
  const cases = [];
  const ix = () => int(15) - 7;
  for (let i = 0; i < 20000; i++) {
    const s = randomText(int(12)), arr = Array.from({ length: int(5) }, value);
    switch (int(9)) {
      case 0: cases.push(['at', s, ix()], ['at', arr, ix()], ['idx', ix(), arr]); break;
      case 1: cases.push(['slice', s, ix(), ix()], ['slice', arr, ix(), ix()]); break;
      case 2: cases.push(['length', s], ['length', arr]); break;
      case 3: cases.push(['code', randomText(int(3))]); break;
      case 4: cases.push(['fromCode', pick([0, 65, 0xd7ff, 0xd800, 0xdfff, 0xe000, 0x10ffff, 0x110000, -1, int(0x110000)])]); break;
      case 5: cases.push(['split', s, pick(['', ',', 'a', 'é', 'ab', Array.from(s).slice(0, 2).join('')])]); break;
      case 6: cases.push(['join', arr, pick(['', ', ', '€'])]); break;
      case 7: cases.push(['same', arr, pick([arr, arr.slice(), [...arr, 1], value()])]); break;
      default: cases.push([pick(['lt', 'gt', 'le', 'ge', 'eq']), s, randomText(int(12))]); break;
    }
  }
  // A long non-ASCII purple, which uses the offset cache.
  const long = randomText(300);
  for (let i = 0; i < 2000; i++) cases.push(['at', long, int(600) - 300], ['slice', long, int(300), 300]);
  cases.push(['at', 'abc', new F(1)], ['at', null, 0], ['slice', 'abc', 1, null], ['length', null], ['join', 'x', ','],
    ['join', [[1]], ','], ['split', 1, ','], ['idx', 'a', 'abc'], ['idx', 0, null], ['code', 5]);
  check(cases);
});

test('$readLine (§12.2) splits and decodes stdin', { skip: noZig }, () => {
  const inputs = ['', 'a', 'a\n', 'a\n\n', 'a\r\nb\rc\n\r\n', '﻿bom\n', 'x'.repeat(200000) + '\ny'];
  for (let i = 0; i < 50; i++) inputs.push(Buffer.concat(Array.from({ length: int(30) }, randomBytes)));
  for (const input of inputs) {
    const text = new TextDecoder('utf-8', { ignoreBOM: true }).decode(Buffer.from(input));
    const lines = text.split('\n');
    if (lines[lines.length - 1] === '') lines.pop();
    const want = lines.map((l, i) => result(i < lines.length - 1 || text.endsWith('\n') ? l.replace(/\r$/, '') : l));
    assert.deepEqual(check([], input).filter((l) => l !== ''), want);
  }
});
