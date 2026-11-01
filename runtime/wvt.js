// Wavert JavaScript runtime, forked from Γ--'s js/src/runtime.js. This file is
// inlined verbatim at the top of every program compiled with --target js (it
// is a plain script, not a module).
//
// Values: int -> JS number, float -> F wrapper, boolean -> JS boolean,
// string -> JS string, array -> JS array (frozen unless mutable), null -> null,
// record -> null-prototype object with keys f_<name> (frozen, or sealed if mutable),
// enum member -> frozen Member.
//
// A host object on globalThis.$wvtHost may override I/O (used by tests):
// { fs, args, write(fd, s), readLine(), env(name), exit(code) }.

const $rt = (function () {
  'use strict';
  const host = globalThis.$wvtHost || {};
  let fsMod = host.fs || null;
  const fs = () => fsMod || (fsMod = process.getBuiltinModule('node:fs'));
  const MAX = 9007199254740991;
  Error.stackTraceLimit = 50;

  class F { constructor(v) { this.v = v; } }
  class Fail {
    constructor(code, fresh, from) {
      this.code = code;
      this.fresh = fresh; // true until it leaves the function that raised it
      this.from = from;
    }
  }
  // An enum member (§4.1): immutable, and equal only to itself.
  class Member {
    constructor(enumName, name) {
      this.enumName = enumName;
      this.name = name;
      Object.freeze(this);
    }
  }
  class Panic extends Error {
    constructor(value) {
      super(typeof value === 'string' ? value : show(value));
      this.name = 'panic';
      this.value = value;
    }
  }

  const NAN = new F(NaN);
  const INF = new F(Infinity);
  const f = (v) => new F(v);
  const isNum = (x) => typeof x === 'number' || x instanceof F;
  const isRec = (x) => x !== null && typeof x === 'object' && Object.getPrototypeOf(x) === null;
  const nv = (x) => (typeof x === 'number' ? x : x.v);

  function typeName(x) {
    if (typeof x === 'number') return 'int';
    if (x instanceof F) return 'float';
    if (typeof x === 'boolean') return 'boolean';
    if (typeof x === 'string') return 'string';
    if (x === null) return 'null';
    if (Array.isArray(x)) return 'array';
    if (isRec(x)) return 'record';
    if (x instanceof Member) return x.enumName;
    return 'unknown';
  }

  function fail(msg) { throw new Panic(msg); }

  function int(r, op) {
    if (!(r <= MAX && r >= -MAX)) fail(`integer overflow in '${op}'`);
    return r + 0; // normalizes -0
  }

  function bad(op, a, b) {
    const types = arguments.length > 2 ? `${typeName(a)} and ${typeName(b)}` : typeName(a);
    let msg = `type error: '${op}' cannot be applied to ${types}`;
    if (op === '+' && (typeof a === 'string' || typeof b === 'string')) msg += ' (use interpolation: "{x}")';
    fail(msg);
  }

  // ---- arithmetic (§11.2) ----

  function add(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return int(a + b, '+');
    if (typeof a === 'string' && typeof b === 'string') return a + b;
    if (isNum(a) && isNum(b)) return new F(nv(a) + nv(b));
    return bad('+', a, b);
  }
  function sub(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return int(a - b, '-');
    if (isNum(a) && isNum(b)) return new F(nv(a) - nv(b));
    return bad('-', a, b);
  }
  function mul(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return int(a * b, '*');
    if (isNum(a) && isNum(b)) return new F(nv(a) * nv(b));
    return bad('*', a, b);
  }
  function div(a, b) {
    if (isNum(a) && isNum(b)) return new F(nv(a) / nv(b));
    return bad('/', a, b);
  }
  // Floored modulo: the result takes the divisor's sign.
  function fmod(x, y) {
    const r = x % y;
    return r !== 0 && (r < 0) !== (y < 0) ? r + y : r;
  }
  function mod(a, b) {
    if (typeof a === 'number' && typeof b === 'number') {
      if (b === 0) fail('integer modulo by zero');
      return fmod(a, b) + 0;
    }
    if (isNum(a) && isNum(b)) return new F(fmod(nv(a), nv(b)));
    return bad('%', a, b);
  }
  function ipow(a, e) {
    let r = 1, base = a;
    while (e > 0) {
      if (e % 2 === 1) r = int(r * base, '**');
      e = Math.floor(e / 2);
      if (e > 0) base = int(base * base, '**');
    }
    return r;
  }
  function pow(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return b >= 0 ? ipow(a, b) : new F(a ** b);
    if (isNum(a) && isNum(b)) return new F(nv(a) ** nv(b));
    return bad('**', a, b);
  }
  function neg(a) {
    if (typeof a === 'number') return 0 - a;
    if (a instanceof F) return new F(-a.v);
    return bad('-', a);
  }

  // ---- comparison (§11.1) ----

  function eq(a, b) {
    if (isNum(a)) return isNum(b) && nv(a) === nv(b);
    if (isNum(b)) return false;
    return a === b;
  }
  const ne = (a, b) => !eq(a, b);

  // Code point order (JS compares UTF-16 code units).
  function cmpStr(a, b) {
    if (a === b) return 0;
    const n = Math.min(a.length, b.length);
    for (let i = 0; i < n; i++) {
      let x = a.charCodeAt(i), y = b.charCodeAt(i);
      if (x !== y) {
        if (x >= 0xd800 && y >= 0xd800) {
          x = x >= 0xe000 ? x - 0x800 : x + 0x2000;
          y = y >= 0xe000 ? y - 0x800 : y + 0x2000;
        }
        return x < y ? -1 : 1;
      }
    }
    return a.length - b.length;
  }
  // Slow path for ordering: returns [x, y] to compare, or null when the types differ.
  function rel(a, b, op) {
    if (isNum(a) && isNum(b)) return [nv(a), nv(b)];
    if (typeof a === 'string' && typeof b === 'string') return [cmpStr(a, b), 0];
    if (a instanceof Member && b instanceof Member) fail(`type error: '${op}' cannot order enum members`);
    if (typeName(a) === typeName(b)) fail(`type error: '${op}' cannot order ${typeName(a)} values`);
    return null;
  }
  function lt(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return a < b;
    if (typeof a === 'string' && typeof b === 'string') return cmpStr(a, b) < 0;
    const r = rel(a, b, '<');
    return r !== null && r[0] < r[1];
  }
  function le(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return a <= b;
    if (typeof a === 'string' && typeof b === 'string') return cmpStr(a, b) <= 0;
    const r = rel(a, b, '<=');
    return r !== null && r[0] <= r[1];
  }
  function gt(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return a > b;
    if (typeof a === 'string' && typeof b === 'string') return cmpStr(a, b) > 0;
    const r = rel(a, b, '>');
    return r !== null && r[0] > r[1];
  }
  function ge(a, b) {
    if (typeof a === 'number' && typeof b === 'number') return a >= b;
    if (typeof a === 'string' && typeof b === 'string') return cmpStr(a, b) >= 0;
    const r = rel(a, b, '>=');
    return r !== null && r[0] >= r[1];
  }

  // ---- bitwise: integers in int32 range ----

  function i32(x, op) {
    if (typeof x !== 'number') fail(`type error: '${op}' requires integers, got ${typeName(x)}`);
    if (x < -2147483648 || x > 2147483647) fail(`'${op}' operand ${x} is outside the int32 range`);
    return x;
  }
  const band = (a, b) => i32(a, '&') & i32(b, '&');
  const bor = (a, b) => i32(a, '|') | i32(b, '|');
  const bxor = (a, b) => i32(a, '^') ^ i32(b, '^');
  const bnot = (a) => ~i32(a, '~');
  function shift(a, b, op) {
    i32(a, op);
    if (typeof b !== 'number') fail(`type error: '${op}' requires integers, got ${typeName(b)}`);
    if (b < 0 || b > 31) fail(`shift amount ${b} is outside 0-31`);
    return op === '<<' ? a << b : a >> b;
  }
  const shl = (a, b) => shift(a, b, '<<');
  const shr = (a, b) => shift(a, b, '>>');

  // ---- booleans ----

  function bool(x, op) {
    if (typeof x !== 'boolean') fail(`type error: '${op}' requires true or false, got ${typeName(x)}`);
    return x;
  }
  const not = (x) => !bool(x, '!');
  function cond(x) {
    if (typeof x !== 'boolean') fail(`condition must be true or false, got ${typeName(x)}`);
    return x;
  }

  // ---- text (§9.3) ----

  function fmtFloat(v) {
    if (Number.isNaN(v)) return 'nan';
    if (v === Infinity) return 'inf';
    if (v === -Infinity) return '-inf';
    if (Object.is(v, -0)) return '-0.0';
    const s = String(v);
    if (/^-?\d+$/.test(s)) return s + '.0';
    const m = /^(-?\d+)(e.*)$/.exec(s);
    return m ? `${m[1]}.0${m[2]}` : s;
  }
  function text(v) {
    switch (typeof v) {
      case 'number': return String(v);
      case 'string': return v;
      case 'boolean': return v ? 'true' : 'false';
    }
    if (v === null) return 'null';
    if (v instanceof F) return fmtFloat(v.v);
    if (v instanceof Member) return v.name;
    return fail(`type error: cannot convert ${typeName(v)} to text; convert it explicitly`);
  }
  function show(v) {
    if (typeof v === 'string') return JSON.stringify(v);
    if (Array.isArray(v)) return `[${v.map(show).join(', ')}]`;
    if (isRec(v)) return `{${Object.keys(v).map((k) => `${k.slice(2)}: ${show(v[k])}`).join(', ')}}`;
    if (isNum(v) || v === null || typeof v === 'boolean' || v instanceof Member) return text(v);
    return String(v);
  }

  // ---- enums (§4.1, §7.5) ----

  function makeEnum(name, members) {
    const l = Object.create(null);
    for (const m of members) l['m_' + m] = new Member(name, m);
    return Object.freeze(l);
  }
  // An exhaustive match whose subject is not one of the enum's members.
  function noCase(v) { fail(`match: no case for ${show(v)}`); }

  // ---- arrays (§10) ----

  function slot(i, arr) {
    if (!Array.isArray(arr)) {
      fail(`type error: indexing needs an array or a record, got ${typeName(arr)}${typeof arr === 'string' ? ' (use $at for a string)' : ''}`);
    }
    if (typeof i !== 'number') fail(`type error: an index must be an int, got ${typeName(i)}`);
    const n = arr.length;
    if (i < -n || i >= n) fail(`index ${i} is out of range for an array of length ${n}`);
    return i < 0 ? i + n : i;
  }
  // Indexing a record is computed field access with a string key (§10.8).
  const idx = (i, arr) => (isRec(arr) ? get(recKey(i), arr) : arr[slot(i, arr)]);
  function mutable(arr, what) {
    if (Object.isFrozen(arr)) fail(`cannot modify a frozen array${what ? ` with ${what}` : ''}`);
  }
  function setIdx(v, i, arr) {
    if (isRec(arr)) return setMem(v, recKey(i), arr);
    const k = slot(i, arr);
    mutable(arr);
    arr[k] = v;
  }
  function updIdx(v, i, arr, op) {
    if (isRec(arr)) return updMem(v, recKey(i), arr, op);
    const k = slot(i, arr);
    mutable(arr);
    arr[k] = op(arr[k], v);
  }
  const frozen = (a) => Object.freeze(a);
  // Copy happens only when mutability changes (§10.4, §10.9).
  function bind(v, isMut) {
    if (isRec(v)) {
      const sh = SHIFTY.has(v);
      if (isMut) return sh ? v : rec(Object.assign(Object.create(null), v), true);
      return sh ? Object.freeze(Object.assign(Object.create(null), v)) : v;
    }
    if (!Array.isArray(v)) return v;
    const fr = Object.isFrozen(v);
    if (isMut) return fr ? v.slice() : v;
    return fr ? v : Object.freeze(v.slice());
  }

  // ---- records (§10.7-§10.10) ----
  // Keys are mangled as f_<name>, so no Γ-- key can reach a JS built-in, and
  // reads check own properties. Mutable records are sealed (fixed shape) and
  // tracked in SHIFTY, because an empty sealed object also counts as frozen.

  const SHIFTY = new WeakSet();
  function rec(o, isMut) {
    if (!isMut) return Object.freeze(o);
    SHIFTY.add(Object.seal(o));
    return o;
  }
  function recKey(k) {
    if (typeof k !== 'string') fail(`type error: a record key must be a string, got ${typeName(k)}`);
    return 'f_' + k;
  }
  function field(key, r) {
    if (!isRec(r)) fail(`type error: member access needs a record, got ${typeName(r)}`);
    if (!Object.hasOwn(r, key)) fail(`no field '${key.slice(2)}' in record`);
  }
  function get(key, r) {
    field(key, r);
    return r[key];
  }
  function writable(r) {
    if (!SHIFTY.has(r)) fail('cannot modify a field of a frozen record');
  }
  function setMem(v, key, r) {
    field(key, r);
    writable(r);
    r[key] = v;
  }
  function updMem(v, key, r, op) {
    field(key, r);
    writable(r);
    r[key] = op(r[key], v);
  }

  // ---- fails, panics, exit (§8, §13) ----

  function failWith(code) {
    if (typeof code !== 'number' || code < 1) fail(`a fail code must be an integer >= 1, got ${show(code)}`);
    return new Fail(code, true, null);
  }
  // A fail raised by a stdlib function arrives at its caller already "left".
  const stdFail = (from, code = 1) => new Fail(code, false, from);

  function tdz(e) {
    const m = /\bw\d*_(\w+)/.exec(e.message);
    return new Panic(m ? `'${m[1]}' was used before its declaration` : e.message);
  }
  // Called when an error leaves a function. A fail the function raised itself goes
  // on to its caller; a fail that came from a callee and was not handled with
  // onfail becomes a runtime error here (one-frame rule).
  function leave(e, name) {
    if (e instanceof Fail) {
      if (e.fresh) {
        e.fresh = false;
        e.from = name;
        return e;
      }
      return new Panic(`unhandled fail (code ${e.code}) from '${e.from}' in '${name}'`);
    }
    if (e instanceof ReferenceError) return tdz(e);
    return e;
  }
  const panic = (v) => new Panic(v);

  function exit(code) {
    if (typeof code !== 'number') fail(`an exit code must be an int, got ${typeName(code)}`);
    if (host.exit) host.exit(code);
    else process.exit(code);
  }

  function main(fn) {
    try {
      fn();
    } catch (e0) {
      if (e0 && e0.__wvtExit !== undefined) throw e0;
      let e = e0;
      if (e instanceof Fail) e = new Panic(`unhandled fail (code ${e.code}) from '${e.from}'`);
      else if (e instanceof ReferenceError) e = tdz(e);
      else if (e instanceof RangeError && /call stack/.test(e.message)) e = new Panic('stack overflow');
      let out = '';
      if (e instanceof Panic) {
        out += `uncaught panic: ${e.message}\n`;
        for (const m of String(e.stack).matchAll(/at w\d*_(\w+)/g)) out += `    in ${m[1]}\n`;
      } else {
        out += `internal error: ${(e && e.stack) || e}\n`;
      }
      write(2, out);
      exit(1);
    }
  }

  // ---- I/O ----

  // A write to a reader that has gone away (prog | head) ends the program
  // silently with 141, as if killed by SIGPIPE, which Node ignores (§12.2).
  const BROKEN_PIPE = 141;

  function write(fd, s) {
    if (host.write) return host.write(fd, s);
    const b = Buffer.from(s, 'utf8');
    let off = 0;
    while (off < b.length) {
      try {
        off += fs().writeSync(fd, b, off, b.length - off);
      } catch (e) {
        if (e.code === 'EPIPE') exit(BROKEN_PIPE);
        if (e.code !== 'EAGAIN') throw e;
      }
    }
  }

  let inBuf = '', inEnd = false, decoder = null;
  function readLine() {
    if (host.readLine) return host.readLine();
    for (;;) {
      const nl = inBuf.indexOf('\n');
      if (nl >= 0) {
        const line = inBuf.slice(0, nl);
        inBuf = inBuf.slice(nl + 1);
        return line.endsWith('\r') ? line.slice(0, -1) : line;
      }
      if (inEnd) {
        // An incomplete sequence at the very end becomes U+FFFD (§12.1).
        if (decoder) {
          inBuf += decoder.decode();
          decoder = null;
          continue;
        }
        if (inBuf === '') return null;
        const line = inBuf;
        inBuf = '';
        return line;
      }
      // ignoreBOM keeps a byte-order mark as U+FEFF, as $readFile does (§12.1).
      decoder ??= new TextDecoder('utf-8', { ignoreBOM: true });
      const chunk = Buffer.alloc(65536);
      let n = 0;
      try {
        n = fs().readSync(0, chunk, 0, chunk.length, null);
      } catch (e) {
        if (e.code === 'EAGAIN') continue;
        if (e.code !== 'EOF') throw e;
      }
      if (n === 0) inEnd = true;
      else inBuf += decoder.decode(chunk.subarray(0, n), { stream: true });
    }
  }

  // ---- the terminal and keys (§12.2) ----
  // The first $readKey on a terminal turns on raw mode, which also turns off
  // Ctrl+C's signal: the runtime ends the program itself when it reads U+0003.

  let keyStarted = false, keyTty = false, keyOn = false;
  function keyApply(on) {
    if (!keyOn) return;
    try { process.stdin.setRawMode(on); } catch { /* the terminal has gone */ }
  }
  function keyStart() {
    if (keyStarted) return;
    keyStarted = true;
    if (host.readLine) return;
    keyTty = !!process.stdin.isTTY;
    if (!keyTty) return;
    keyOn = true;
    process.on('exit', () => keyApply(false));
  }

  // A synchronous pause, for a non-blocking terminal with nothing to read yet.
  const pauseCell = new Int32Array(new SharedArrayBuffer(4));
  const pause = (ms) => Atomics.wait(pauseCell, 0, 0, ms);

  // Reads more of stdin into inBuf; false at the end of input. wait is false to
  // use only what has been read already.
  function fillKeys(wait) {
    if (inEnd) {
      if (decoder) {
        inBuf += decoder.decode();
        decoder = null;
        return true;
      }
      return false;
    }
    if (!wait) return false;
    decoder ??= new TextDecoder('utf-8', { ignoreBOM: true });
    const chunk = Buffer.alloc(65536);
    for (;;) {
      let n = 0;
      try {
        n = fs().readSync(0, chunk, 0, chunk.length, null);
      } catch (e) {
        if (e.code === 'EAGAIN') { pause(10); continue; }
        if (e.code !== 'EOF') throw e;
      }
      if (n === 0) inEnd = true;
      else inBuf += decoder.decode(chunk.subarray(0, n), { stream: true });
      return true;
    }
  }
  // The code point at UTF-16 offset i of inBuf, reading more if wait; -1 if none.
  function keyCp(i, wait) {
    while (inBuf.length <= i) if (!fillKeys(wait)) return -1;
    return inBuf.codePointAt(i);
  }
  const cpLen = (c) => (c > 0xffff ? 2 : 1);
  // A code point that types a character: not a control character.
  const isChar = (c) => c >= 0x20 && !(c >= 0x7f && c <= 0x9f);
  const keyFs = ['f1', 'f2', 'f3', 'f4', 'f5', 'f6', 'f7', 'f8', 'f9', 'f10', 'f11', 'f12'];

  // Ctrl with a letter, c from 1 (Ctrl+A) to 26; null if it is discarded.
  function ctrlKey(c) {
    if (c === 3 || c === 26) return null;
    if (c === 8) return 'backspace';
    if (c === 9) return 'tab';
    if (c === 10 || c === 13) return 'enter';
    return 'ctrl+' + String.fromCharCode(0x60 + c);
  }
  // The key named by ESC [ params final, with no intermediate bytes; null if none.
  function csiKey(params, final) {
    const first = params.split(';')[0];
    if (!/^\d*$/.test(first)) return null;
    const num = first === '' ? -1 : Number(first);
    if (final === '~') {
      const names = { 1: 'home', 7: 'home', 2: 'insert', 3: 'delete', 4: 'end', 8: 'end', 5: 'pageup', 6: 'pagedown', 23: 'f11', 24: 'f12' };
      if (names[num]) return names[num];
      if (num >= 11 && num <= 15) return keyFs[num - 11];
      if (num >= 17 && num <= 21) return keyFs[num - 12];
      return null;
    }
    if (num !== -1 && num !== 1) return null;
    return { A: 'up', B: 'down', C: 'right', D: 'left', H: 'home', F: 'end', E: 'clear', G: 'clear', Z: 'tab' }[final] ?? null;
  }
  const ss3Keys = { A: 'up', B: 'down', C: 'right', D: 'left', H: 'home', F: 'end', E: 'clear', M: 'enter', P: 'f1', Q: 'f2', R: 'f3', S: 'f4' };

  // A key that starts with ESC: the key, or null if it is discarded. In a
  // terminal, the rest of an escape sequence is what arrived with the ESC.
  function escKey(brief) {
    const b1 = keyCp(1, brief);
    if (b1 === 0x5b || b1 === 0x4f) { // [ or O
      const b2 = keyCp(2, brief);
      if (b1 === 0x4f || b2 === 0x5b) {
        const at = b1 === 0x4f ? 2 : 3;
        const f = b1 === 0x4f ? b2 : keyCp(3, brief);
        if (f >= 0) {
          inBuf = inBuf.slice(at + 1);
          const ch = String.fromCharCode(f);
          return (b1 === 0x4f ? ss3Keys[ch] : 'ABCDE'.includes(ch) ? keyFs[f - 0x41] : null) ?? null;
        }
      } else {
        let i = 2, b = b2;
        while (b >= 0x30 && b <= 0x3f) b = keyCp(++i, brief);
        const params = i;
        while (b >= 0x20 && b <= 0x2f) b = keyCp(++i, brief);
        if (b >= 0x40 && b <= 0x7e) {
          const name = params === i ? csiKey(inBuf.slice(2, params), String.fromCharCode(b)) : null;
          inBuf = inBuf.slice(i + 1);
          return name;
        }
      }
    } else if (isChar(b1)) {
      inBuf = inBuf.slice(1 + cpLen(b1));
      return 'alt+' + String.fromCodePoint(b1);
    }
    inBuf = inBuf.slice(1);
    return 'escape';
  }

  // The next key: a string, null if it was discarded, or undefined at the end of input.
  function nextKey() {
    const c = keyCp(0, true);
    if (c < 0) return undefined;
    if (c === 0x1b) return escKey(!keyTty);
    inBuf = inBuf.slice(cpLen(c));
    if (c === 3 && keyTty) {
      // Ctrl+C (§12.2): end as the signal would.
      keyApply(false);
      if (process.platform !== 'win32') process.kill(process.pid, 'SIGINT');
      exit(130);
    }
    if (c === 0x0d) {
      // Enter in a terminal sends "\r" alone, so only redirected input waits for a "\n".
      if (keyCp(0, !keyTty) === 0x0a) inBuf = inBuf.slice(1);
      return 'enter';
    }
    if (c === 0x7f) return 'backspace';
    if (c >= 1 && c <= 26) return ctrlKey(c);
    return isChar(c) ? String.fromCodePoint(c) : null;
  }

  // ---- code points (§9.1) ----
  // cpView returns null when every UTF-16 unit is a code point, else an array of
  // code points. Long strings are cached so loops over $at stay O(1) per call.

  let cacheS = null, cacheV = null;
  function hasSurrogate(s) {
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      if (c >= 0xd800 && c <= 0xdfff) return true;
    }
    return false;
  }
  function cpView(s) {
    if (s.length < 64) return hasSurrogate(s) ? Array.from(s) : null;
    if (s !== cacheS) {
      cacheS = s;
      cacheV = hasSurrogate(s) ? Array.from(s) : null;
    }
    return cacheV;
  }
  const cpLength = (s) => { const v = cpView(s); return v ? v.length : s.length; };

  // ---- numbers as text ($toInt, $toFloat) ----

  // ---- standard library (§12) ----

  const std = Object.create(null);
  const aString = (x, fn) => (typeof x === 'string' ? x : fail(`type error: ${fn} expects a string, got ${typeName(x)}`));
  const anInt = (x, fn) => (typeof x === 'number' ? x : fail(`type error: ${fn} expects an int, got ${typeName(x)}`));
  const anArray = (x, fn) => (Array.isArray(x) ? x : fail(`type error: ${fn} expects an array, got ${typeName(x)}`));

  std.print = (v) => { write(1, text(v) + '\n'); return null; };
  std.write = (v) => { write(1, text(v)); return null; };
  std.eprint = (v) => { write(2, text(v) + '\n'); return null; };
  std.readLine = () => {
    keyApply(false);
    const line = readLine();
    keyApply(true);
    if (line === null) throw stdFail('$readLine');
    return line;
  };
  std.readKey = () => {
    keyStart();
    keyApply(true);
    for (;;) {
      const key = nextKey();
      if (key === undefined) throw stdFail('$readKey');
      if (key !== null) return key;
    }
  };

  std.readFile = (p) => {
    aString(p, '$readFile');
    try { return fs().readFileSync(p, 'utf8'); } catch { throw stdFail('$readFile'); }
  };
  std.writeFile = (p, t) => {
    aString(p, '$writeFile');
    aString(t, '$writeFile');
    try { fs().writeFileSync(p, t, 'utf8'); } catch { throw stdFail('$writeFile'); }
    return null;
  };
  std.appendFile = (p, t) => {
    aString(p, '$appendFile');
    aString(t, '$appendFile');
    try { fs().appendFileSync(p, t, 'utf8'); } catch { throw stdFail('$appendFile'); }
    return null;
  };
  std.fileExists = (p) => fs().existsSync(aString(p, '$fileExists'));
  std.listDir = (p) => {
    aString(p, '$listDir');
    let names;
    try { names = fs().readdirSync(p); } catch { throw stdFail('$listDir'); }
    return Object.freeze(names.slice().sort(cmpStr));
  };
  std.deleteFile = (p) => {
    aString(p, '$deleteFile');
    try { fs().unlinkSync(p); } catch { throw stdFail('$deleteFile'); }
    return null;
  };

  let argsCache = null;
  Object.defineProperty(std, 'args', {
    get: () => argsCache || (argsCache = Object.freeze((host.args || process.argv.slice(2)).slice())),
  });
  std.env = (n) => {
    aString(n, '$env');
    const v = host.env ? host.env(n) : process.env[n];
    if (v === undefined) throw stdFail('$env');
    return v;
  };
  // The file the host started (§12.4): the compiled .js under node, or what the host says.
  let programPathCache = null;
  Object.defineProperty(std, 'programPath', {
    get: () => programPathCache
      || (programPathCache = host.programPath ?? process.getBuiltinModule('node:path').resolve(process.argv[1] ?? '')),
  });
  // Runs a program with inherited stdio and no shell; returns its exit code (§12.4).
  std.run = (program, args) => {
    aString(program, '$run');
    anArray(args, '$run');
    for (const a of args) {
      if (typeof a !== 'string') fail(`type error: $run expects an array of strings, got an element of type ${typeName(a)}`);
    }
    if (host.run) return host.run(program, args.slice());
    keyApply(false);
    const r = process.getBuiltinModule('node:child_process')
      .spawnSync(program, args, { stdio: 'inherit', shell: false, windowsHide: true });
    keyApply(true);
    if (r.error) throw stdFail('$run');
    if (r.status !== null) return r.status;
    return 128 + (process.getBuiltinModule('node:os').constants.signals[r.signal] ?? 0);
  };

  std.length = (x) => {
    if (typeof x === 'string') return cpLength(x);
    if (Array.isArray(x)) return x.length;
    return fail(`type error: $length expects a string or an array, got ${typeName(x)}`);
  };
  function cpIndex(i, n, fn) {
    anInt(i, fn);
    if (i < -n || i >= n) fail(`${fn}: index ${i} is out of range for length ${n}`);
    return i < 0 ? i + n : i;
  }
  std.at = (x, i) => {
    if (Array.isArray(x)) return idx(i, x);
    aString(x, '$at');
    const v = cpView(x);
    const k = cpIndex(i, v ? v.length : x.length, '$at');
    return v ? v[k] : x[k];
  };
  std.slice = (x, a, b) => {
    const isArr = Array.isArray(x);
    if (!isArr) aString(x, '$slice');
    anInt(a, '$slice');
    anInt(b, '$slice');
    const v = isArr ? null : cpView(x);
    const n = isArr || !v ? x.length : v.length;
    const s = a < 0 ? a + n : a, e = b < 0 ? b + n : b;
    if (!(s >= 0 && s <= e && e <= n)) fail(`$slice: ${a}..${b} is out of range for length ${n}`);
    if (isArr) return Object.freeze(x.slice(s, e));
    return v ? v.slice(s, e).join('') : x.slice(s, e);
  };
  std.code = (s) => {
    aString(s, '$code');
    if (cpLength(s) !== 1) fail(`$code expects a single character, got ${show(s)}`);
    return s.codePointAt(0);
  };
  std.fromCode = (c) => {
    if (typeof c !== 'number' || c < 0 || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) {
      fail(`$fromCode: ${show(c)} is not a valid code point`);
    }
    return String.fromCodePoint(c);
  };
  std.split = (s, sep) => {
    aString(s, '$split');
    aString(sep, '$split');
    return Object.freeze(sep === '' ? Array.from(s) : s.split(sep));
  };
  std.join = (arr, sep) => {
    anArray(arr, '$join');
    aString(sep, '$join');
    return arr.map(text).join(sep);
  };
  std.toString = (v) => text(v);
  std.toInt = (x) => {
    if (typeof x === 'number') return x;
    if (typeof x !== 'string') {
      fail(`type error: $toInt expects a string, got ${typeName(x)}${x instanceof F ? ' (use $floor, $ceil, $round or $trunc)' : ''}`);
    }
    if (/^-?\d+$/.test(x)) {
      const v = Number(x);
      if (Number.isSafeInteger(v)) return v + 0;
      throw stdFail('$toInt');
    }
    throw stdFail('$toInt');
  };
  std.toFloat = (x) => {
    if (typeof x === 'number') return new F(x);
    if (x instanceof F) return x;
    aString(x, '$toFloat');
    if (/^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(x)) {
      // Number() rounds correctly; a result too large for a double signals (§9.5).
      const v = Number(x);
      if (!Number.isFinite(v)) throw stdFail('$toFloat');
      return new F(v);
    }
    throw stdFail('$toFloat');
  };

  function rounder(name, fn) {
    return (x) => {
      if (typeof x === 'number') return x;
      if (!(x instanceof F)) fail(`type error: $${name} expects a number, got ${typeName(x)}`);
      if (!Number.isFinite(x.v)) fail(`$${name}: cannot convert ${fmtFloat(x.v)} to an int`);
      return int(fn(x.v), '$' + name);
    };
  }
  std.floor = rounder('floor', Math.floor);
  std.ceil = rounder('ceil', Math.ceil);
  std.round = rounder('round', (v) => Math.sign(v) * Math.round(Math.abs(v))); // half away from zero
  std.trunc = rounder('trunc', Math.trunc);
  std.toUint32 = (x) => anInt(x, '$toUint32') >>> 0;
  std.isNan = (x) => x instanceof F && Number.isNaN(x.v);

  std.push = (arr, v) => {
    anArray(arr, '$push');
    mutable(arr, '$push');
    arr.push(v);
    return null;
  };
  std.pop = (arr) => {
    anArray(arr, '$pop');
    mutable(arr, '$pop');
    if (arr.length === 0) fail('$pop: the array is empty');
    return arr.pop();
  };
  function same(a, b) {
    if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((x, i) => same(x, b[i]));
    if (isRec(a) && isRec(b)) {
      const ks = Object.keys(a);
      return ks.length === Object.keys(b).length && ks.every((k) => Object.hasOwn(b, k) && same(a[k], b[k]));
    }
    return eq(a, b);
  }
  std.same = same;
  std.typeOf = typeName;
  std.fields = (r) => {
    if (!isRec(r)) fail(`type error: $fields expects a record, got ${typeName(r)}`);
    return Object.freeze(Object.keys(r).map((k) => k.slice(2)));
  };

  // ---- time and random numbers (§12.8) ----

  std.time = () => Date.now();

  // A generator is a mutable record { s0, s1, s2, s3 } of 32-bit words.
  const RNG_KEYS = ['f_s0', 'f_s1', 'f_s2', 'f_s3'];
  // MurmurHash3's finalizer.
  function rngMix(z) {
    z = (z ^ (z >>> 16)) >>> 0;
    z = Math.imul(z, 0x85ebca6b) >>> 0;
    z = (z ^ (z >>> 13)) >>> 0;
    z = Math.imul(z, 0xc2b2ae35) >>> 0;
    return (z ^ (z >>> 16)) >>> 0;
  }
  const rotl32 = (x, k) => ((x << k) | (x >>> (32 - k))) >>> 0;
  std.newRandom = (seed) => {
    anInt(seed, '$newRandom');
    const a = Math.abs(seed);
    const lo = a % 2 ** 32, hi = Math.floor(a / 2 ** 32) + (seed < 0 ? 2 ** 21 : 0);
    const k = rngMix(hi);
    const r = Object.create(null);
    let c = lo;
    for (const key of RNG_KEYS) {
      c = (c + 0x9e3779b9) >>> 0;
      r[key] = (rngMix(c) ^ k) >>> 0;
    }
    return rec(r, true);
  };
  // Panics unless rng is a mutable generator (§12.8).
  function rngCheck(rng, fn) {
    const ok = isRec(rng) && Object.keys(rng).length === 4
      && RNG_KEYS.every((k) => Object.hasOwn(rng, k) && typeof rng[k] === 'number' && rng[k] >= 0 && rng[k] <= 0xffffffff)
      && RNG_KEYS.some((k) => rng[k] !== 0);
    if (!ok) fail(`${fn}: the argument is not a random generator`);
    if (!SHIFTY.has(rng)) fail('random generator is frozen; hold it in a var or ref binding');
  }
  // The next word, by xoshiro128**.
  function rngNext(r) {
    let s0 = r.f_s0, s1 = r.f_s1, s2 = r.f_s2, s3 = r.f_s3;
    const result = Math.imul(rotl32(Math.imul(s1, 5) >>> 0, 7), 9) >>> 0;
    const t = (s1 << 9) >>> 0;
    s2 = (s2 ^ s0) >>> 0;
    s3 = (s3 ^ s1) >>> 0;
    s1 = (s1 ^ s2) >>> 0;
    s0 = (s0 ^ s3) >>> 0;
    s2 = (s2 ^ t) >>> 0;
    s3 = rotl32(s3, 11);
    r.f_s0 = s0;
    r.f_s1 = s1;
    r.f_s2 = s2;
    r.f_s3 = s3;
    return result;
  }
  std.random = (rng, n) => {
    rngCheck(rng, '$random');
    anInt(n, '$random');
    if (n < 1 || n > 2 ** 32) fail(`$random: n must be from 1 to 4294967296, got ${n}`);
    if (n === 2 ** 32) return rngNext(rng);
    // Rejecting the top partial range keeps every result equally likely.
    const limit = 2 ** 32 - (2 ** 32 % n);
    for (;;) {
      const x = rngNext(rng);
      if (x < limit) return x % n;
    }
  };
  std.randomFloat = (rng) => {
    rngCheck(rng, '$randomFloat');
    const a = rngNext(rng) >>> 5, b = rngNext(rng) >>> 6;
    return new F((a * 2 ** 26 + b) / 2 ** 53);
  };

  return {
    F, Fail, Panic, NAN, INF, f,
    add, sub, mul, div, mod, pow, neg, eq, ne, lt, le, gt, ge,
    band, bor, bxor, bnot, shl, shr, bool, not, cond, text, makeEnum, noCase,
    idx, setIdx, updIdx, frozen, bind, rec, get, setMem, updMem, failWith, leave, panic, exit, main, std,
  };
})();
const $std = $rt.std;
