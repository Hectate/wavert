// Code generation for --target c: checked AST -> a standalone C program.
//
// runtime/wah.c is inlined at the top; the output needs GNU C (clang or gcc,
// e.g. `zig cc`), for statement expressions ({ ... }).
//
// Names:
//   w<k>_<name>   a scheme or claim at a module's top scope (k: library number; '' for the entry file)
//   l<k>_<name>   a lineup: a static array of its members
//   s<n>_<name>   any other scheme (a C function)
//   v<n>_<name>   any other binding: a claim, parameter or whine code
//   fr<f>         the frame of scheme f: the bindings its nested schemes use
//   S<n>, F_<name>, K<n>   purple literals, record keys, record shapes
//   t<n>          temporaries
// n counts every scheme and binding in the order the analysis declares them.
//
// Bindings of a module's top level, at any depth, are C globals. A scheme's
// bindings are C locals, except those a nested scheme uses: they live in the
// scheme's frame, and a nested scheme reaches the frames around it through
// its `up` pointer. A nested scheme never outlives the call that contains it
// (§16), so frames live on the C stack.
//
// C does not fix the order in which a call's arguments are evaluated, so a
// call whose order could matter evaluates its arguments into temporaries
// first, left to right (see call()).

import { readFileSync } from 'node:fs';
import { WahError, err } from './errors.js';
import { mode } from './codegen.js';

// With LF line endings however it is checked out, so the output is the same everywhere.
const RUNTIME = readFileSync(new URL('../../runtime/wah.c', import.meta.url), 'utf8').replaceAll('\r\n', '\n');

const BIN = {
  '+': 'add', '-': 'sub', '*': 'mul', '/': 'div', '%': 'mod', '**': 'pow',
  '==': 'eq', '!=': 'ne', '<': 'lt', '>': 'gt', '<=': 'le', '>=': 'ge',
  '&': 'band', '|': 'bor', '^': 'bxor', '<<': 'shl', '>>': 'shr',
};
const UN = { '-': 'neg', '!': 'not', '~': 'bnot' };
// Node kinds whose value can never be an array or record (so binding needs no copy check).
const SCALAR = new Set(['int', 'float', 'str', 'bool', 'null', 'nan', 'inf', 'bin', 'un']);
// Node kinds that evaluate without calling anything.
const PLAIN = new Set(['int', 'float', 'bool', 'null', 'nan', 'inf', 'name', 'std']);
// Stdlib functions that can signal WAH! (§12.1).
const WAILS = new Set(['readLine', 'readFile', 'writeFile', 'appendFile', 'listDir', 'deleteFile', 'env', 'run', 'toInt', 'toFloat']);

// How an expression's evaluation interacts with others (see call()).
const CONST = 0; // a constant: nothing can change it
const PURE = 1; // reads a binding, so another argument's effects could change it
const EFFECT = 2; // may have effects or raise a tantrum

// Float text (§9.3): also a C double literal, and what wahc writes too.
function fmtFloat(v) {
  if (Object.is(v, -0)) return '-0.0';
  const s = String(v);
  if (/^-?\d+$/.test(s)) return s + '.0';
  const m = /^(-?\d+)(e.*)$/.exec(s);
  return m ? `${m[1]}.0${m[2]}` : s;
}

// A C string literal for s: printable characters as they are (the C file is
// UTF-8), other control characters as three-digit octal escapes.
function cString(s) {
  let out = '"';
  for (const ch of s) {
    const c = ch.codePointAt(0);
    if (c === 0x22) out += '\\"';
    else if (c === 0x5c) out += '\\\\';
    else if (c === 0x0a) out += '\\n';
    else if (c === 0x09) out += '\\t';
    else if (c === 0x0d) out += '\\r';
    else if (c < 0x20 || c === 0x7f) out += '\\' + c.toString(8).padStart(3, '0');
    else out += ch;
  }
  return out + '"';
}
// A static wstr initializer: { bytes, code points, ascii, sym, data }.
function wstrInit(s) {
  const bytes = Buffer.byteLength(s, 'utf8');
  const cps = Array.from(s).length;
  return `{ ${bytes}, ${cps}, ${bytes === cps ? 1 : 0}, 0, ${cString(s)} }`;
}

class Scope {
  constructor(parent) {
    this.parent = parent;
    this.names = new Map();
  }
  lookup(name) {
    for (let s = this; s; s = s.parent) {
      const d = s.names.get(name);
      if (d) return d;
    }
    return null;
  }
}

// A C function: a scheme, or a module's top level (top).
class Fn {
  constructor(id, name, parent) {
    this.id = id;
    this.name = name;
    this.parent = parent;
    this.top = parent === null;
    this.depth = parent ? parent.depth + 1 : 0;
    this.children = [];
    this.decls = []; // its claims, parameters and whine codes
  }
  // A scheme with nested schemes has a frame, even when they use none of its bindings.
  get frame() { return !this.top && this.children.length > 0; }
}

// modules: checked, in initialization order, the entry file last.
export function generateC(modules) {
  const g = new CGen();
  modules.forEach((m, i) => { m.id = i === modules.length - 1 ? '' : String(i + 1); });
  for (const m of modules) g.analyze(m);
  for (const m of modules) for (const s of m.program.stmts) if (s.k === 'lineup') g.lineup(s, m.id);
  for (const m of modules) {
    g.enter(m);
    g.stmts(m.program.stmts);
  }
  return g.assemble();
}

class CGen {
  constructor() {
    this.n = 0; // schemes and bindings, for unique names
    this.fns = 0;
    this.t = 0; // temporaries
    this.strings = new Map(); // text -> S number
    this.fields = []; // record keys, in first use
    this.shapes = new Map(); // keys joined -> K number
    this.lineups = []; // C definitions
    this.globals = []; // cnames
    this.frames = []; // Fns with frames
    this.functions = []; // { fn, proto, lines }
    this.main = [];
    this.out = this.main;
    this.indent = 1;
    this.ctx = { loops: 0, iife: false };
  }

  // ---- analysis: resolve names, find what nested schemes use ----

  analyze(m) {
    this.mod = m.id;
    const top = new Fn(this.fns++, null, null);
    m.top = top;
    const scope = new Scope(null);
    for (const { node, lib } of m.steals) scope.names.set(node.name, { kind: 'module', lib });
    this.ablock(m.program, scope, top, scope);
  }

  // A binding of fn. At a module's top scope, claims and schemes keep their
  // names (other modules use them); everything else is numbered.
  bind(kind, name, fn, atTop, shifty = false) {
    const prefix = kind === 'scheme' ? 's' : 'v';
    const cname = atTop ? `w${this.mod}_${name}` : `${prefix}${this.n++}_${name}`;
    const d = { kind, name, fn, cname, shifty, captured: false };
    if (kind !== 'scheme') fn.decls.push(d);
    if (fn.top && kind !== 'scheme') this.globals.push(cname);
    return d;
  }

  ablock(b, scope, fn, topScope) {
    const atTop = scope === topScope;
    b.claims = [];
    for (const s of b.stmts) {
      if (s.k === 'scheme') {
        const d = this.bind('scheme', s.name, fn, atTop);
        const child = new Fn(this.fns++, s.name, fn);
        child.cname = d.cname;
        d.schemeFn = child;
        fn.children.push(child);
        s.fn = child;
        scope.names.set(s.name, d);
      } else if (s.k === 'lineup') {
        scope.names.set(s.name, { kind: 'lineup' });
      } else if (s.k === 'claim') {
        s.decl = this.bind('claim', s.name, fn, atTop, s.shifty);
        b.claims.push(s.decl);
        scope.names.set(s.name, s.decl);
      }
    }
    for (const s of b.stmts) this.astmt(s, scope, fn, topScope);
    if (b.value) this.aexpr(b.value, scope, fn);
  }

  astmt(s, scope, fn, topScope) {
    switch (s.k) {
      case 'scheme': {
        const f = s.fn;
        const ps = new Scope(scope);
        for (const p of s.params) {
          p.decl = this.bind('param', p.name, f, false, p.shifty);
          ps.names.set(p.name, p.decl);
        }
        this.ablock(s.body, new Scope(ps), f, topScope);
        break;
      }
      case 'lurk':
        this.aexpr(s.cond, scope, fn);
        this.ablock(s.body, new Scope(scope), fn, topScope);
        break;
      case 'getaway': case 'tantrum': case 'exit': case 'claim':
        if (s.e) this.aexpr(s.e, scope, fn);
        break;
      case 'expr':
        if (s.e.k === 'sigcall') this.aexpr(s.e.arg, scope, fn);
        else this.aexpr(s.e, scope, fn, topScope);
        break;
      case 'cheat':
        this.aexpr(s.e, scope, fn);
        this.atarget(s.target, scope, fn);
        break;
    }
    if (s.whine) {
      const hs = new Scope(scope);
      if (s.whine.param) {
        s.whine.decl = this.bind('whine', s.whine.param, fn, false);
        hs.names.set(s.whine.param, s.whine.decl);
      }
      this.ablock(s.whine.body, new Scope(hs), fn, topScope);
    }
  }

  // A name used in fn: a binding of another function is captured.
  use(x, scope, fn) {
    const d = scope.lookup(x.name);
    x.decl = d;
    if (d.fn !== fn) d.captured = true;
  }

  atarget(t, scope, fn) {
    if (t.k === 'name') return this.use(t, scope, fn);
    if (t.k === 'idx') this.aexpr(t.index, scope, fn);
    this.aexpr(t.k === 'mem' ? t.rec : t.arr, scope, fn);
  }

  // Is x (the right of '.') a lineup or module name, rather than a value?
  notValue(x, scope) {
    if (x.k === 'mem') return x.rec.k === 'name' && scope.lookup(x.rec.name)?.kind === 'module';
    if (x.k !== 'name') return false;
    const kind = scope.lookup(x.name)?.kind;
    return kind === 'lineup' || kind === 'module';
  }

  aexpr(e, scope, fn, topScope) {
    switch (e.k) {
      case 'str':
        for (const p of e.parts) if (typeof p !== 'string') this.aexpr(p, scope, fn);
        return;
      case 'name': return this.use(e, scope, fn);
      case 'arr':
        for (const i of e.items) this.aexpr(i, scope, fn);
        return;
      case 'rec':
        for (const f of e.fields) this.aexpr(f.e, scope, fn);
        return;
      case 'idx':
        this.aexpr(e.index, scope, fn);
        this.aexpr(e.arr, scope, fn);
        return;
      case 'mem':
        if (!this.notValue(e.rec, scope)) this.aexpr(e.rec, scope, fn);
        return;
      case 'un': return this.aexpr(e.e, scope, fn);
      case 'bin':
        this.aexpr(e.l, scope, fn);
        this.aexpr(e.r, scope, fn);
        return;
      case 'tern':
        this.aexpr(e.c, scope, fn);
        this.aexpr(e.a, scope, fn);
        this.aexpr(e.b, scope, fn);
        return;
      case 'call':
        for (const a of e.args) this.aexpr(a, scope, fn);
        if (e.callee.k === 'name') e.callee.decl = scope.lookup(e.callee.name);
        return;
      case 'chain':
        for (const b of e.branches) {
          this.aexpr(b.cond, scope, fn);
          this.ablock(b.body, new Scope(scope), fn, topScope);
        }
        if (e.els) this.ablock(e.els, new Scope(scope), fn, topScope);
        return;
      case 'suspect':
        this.aexpr(e.subject, scope, fn);
        for (const a of e.alibis) this.ablock(a.body, new Scope(scope), fn, topScope);
        if (e.els) this.ablock(e.els, new Scope(scope), fn, topScope);
        return;
    }
  }

  // ---- emission ----

  // Starts module m: name.module means the stolen module's name (§13.1).
  enter(m) {
    this.src = m.src;
    this.fn = m.top;
    this.modules = new Map(m.steals.map(({ node, lib }) => [node.name, lib]));
    this.lineupNames = new Map(m.program.stmts.filter((s) => s.k === 'lineup').map((s) => [s.name, s]));
    this.mod = m.id;
  }

  emit(line) { this.out.push('  '.repeat(this.indent) + line); }
  tmp() { return `t${this.t++}`; }
  fail(pos, msg) { throw new WahError([{ ...err(pos, msg), src: this.src }]); }

  str(s) {
    if (!this.strings.has(s)) this.strings.set(s, this.strings.size);
    return `&S${this.strings.get(s)}`;
  }
  field(name) {
    if (!this.fields.includes(name)) this.fields.push(name);
    return `&F_${name}`;
  }

  // A lineup is a static array of its members (§4.1).
  lineup(s, mod) {
    const name = this.str(s.name);
    const members = s.members.map((m) => `{ ${name}, ${this.str(m.name)} }`).join(', ');
    this.lineups.push(`static const wmem l${mod}_${s.name}[] = { ${members} };`);
  }

  // The stolen module that x names, or undefined.
  stolenFrom(x) { return x.k === 'name' ? this.modules.get(x.name) : undefined; }

  // The C name and declaration of the lineup x names (Lineup or Lineup.module), or null.
  lineupOf(x) {
    if (x.k === 'name') {
      const s = this.lineupNames.get(x.name);
      return s ? { c: `l${this.mod}_${x.name}`, node: s } : null;
    }
    const lib = x.k === 'mem' ? this.stolenFrom(x.rec) : undefined;
    const d = lib?.exports.get(x.field);
    return d?.kind === 'lineup' ? { c: `l${lib.id}_${x.field}`, node: d.node } : null;
  }

  // Where binding d lives, seen from the current function.
  place(d) {
    if (d.fn.top) return d.cname;
    if (d.fn === this.fn) return d.captured ? `fr.${d.cname}` : d.cname;
    return `up${'->up'.repeat(this.fn.depth - d.fn.depth - 1)}->${d.cname}`;
  }
  // Does reading d need the temporal dead zone check (§5.1)?
  tdz(d) { return d.kind === 'claim' && d.fn !== this.fn; }
  read(d) {
    return this.tdz(d) ? { c: `rt_def(${this.place(d)}, ${JSON.stringify(d.name)})`, k: EFFECT } : { c: this.place(d), k: PURE };
  }

  // fn(args) with the arguments evaluated left to right. A direct C call is
  // safe when at most one argument has effects and the others are constants;
  // otherwise each non-constant argument goes into a temporary first.
  call(build, args) {
    const effects = args.filter((a) => a.k === EFFECT).length;
    if (effects === 0 || (effects === 1 && args.every((a) => a.k !== PURE))) {
      return { c: build(args.map((a) => a.c)), k: EFFECT };
    }
    const decls = [];
    const cs = args.map((a) => {
      if (a.k === CONST) return a.c;
      const t = this.tmp();
      decls.push(`wv ${t} = ${a.c};`);
      return t;
    });
    return { c: `({ ${decls.join(' ')} ${build(cs)}; })`, k: EFFECT };
  }
  // A C array of values: (wv[]){ ... }, or NULL when empty.
  values(n, cs, i = 0) { return n ? `(wv[]){ ${cs.slice(i, i + n).join(', ')} }` : 'NULL'; }

  stmts(list) { for (const s of list) this.stmt(s); }

  // A block whose value is discarded. Its captured claims start unset each
  // time it runs, as a fresh scope would (temporal dead zone, §5.1).
  blockBody(b) {
    this.resets(b);
    this.stmts(b.stmts);
    if (b.value) this.emit(`(void)${this.expr(b.value).c};`);
  }
  resets(b) {
    for (const d of b.claims) if (d.captured) this.emit(`${this.place(d)} = RT_UNSET;`);
  }

  valueOf(b) { return b.value ? this.expr(b.value).c : 'RT_NULL'; }

  stmt(s) {
    if (s.whine) return this.whine(s);
    switch (s.k) {
      case 'empty': case 'lineup': case 'steal': case 'bribe': return;
      case 'scheme': return this.scheme(s);
      case 'getaway':
        if (this.ctx.iife) this.fail(s.pos, "'getaway' inside a conditional used as a value is not supported in stage 0");
        return this.emit(`RT_RET(${s.e ? this.expr(s.e).c : 'RT_NULL'});`);
      case 'tantrum': return this.emit(`rt_tantrum(${this.expr(s.e).c});`);
      case 'bail': case 'loiter':
        if (!this.ctx.loops) this.fail(s.pos, `'${s.k}' cannot leave a conditional used as a value in stage 0`);
        return this.emit(s.k === 'bail' ? 'break;' : 'continue;');
      case 'lurk':
        this.emit(`while (${s.neg ? '!' : ''}rt_cond(${this.expr(s.cond).c})) {`);
        this.indent++;
        this.ctx.loops++;
        this.blockBody(s.body);
        this.ctx.loops--;
        this.indent--;
        return this.emit('}');
      case 'signal': return this.signal(`rt_i(${s.code})`);
      case 'exit': return this.emit(`rt_exit(${s.e ? this.expr(s.e).c : 'rt_i(0)'});`);
      case 'expr':
        if (s.e.k === 'chain') return this.chainStmt(s.e);
        if (s.e.k === 'suspect') return this.suspectStmt(s.e);
        if (s.e.k === 'sigcall') return this.signal(this.expr(s.e.arg).c);
        return this.emit(`(void)${this.expr(s.e).c};`);
      case 'claim': return this.claim(s.decl, this.bindRhs(s.e, mode(s)));
      case 'cheat': return this.cheat(s);
    }
    throw new Error(`codegen: unhandled statement ${s.k}`);
  }

  // A signal leaves the scheme at once; the caller sees the code (§8.2).
  signal(code) {
    this.emit(`rt_signal(${code});`);
    this.emit('RT_RET(RT_NULL);');
  }

  // Sets binding d for the first time: a new C local, or its frame slot or global.
  claim(d, value) {
    if (!d.fn.top && d.fn === this.fn && !d.captured) this.emit(`wv ${d.cname} = ${value};`);
    else this.emit(`${this.place(d)} = ${value};`);
  }

  scheme(s) {
    const f = s.fn;
    const parent = f.parent.top ? null : f.parent;
    const params = s.params.map((p) => `wv ${p.decl.captured ? `p_${p.decl.cname}` : p.decl.cname}`);
    if (parent) params.unshift(`struct fr${parent.id} *up`);
    const proto = `static wv ${f.cname}(${params.length ? params.join(', ') : 'void'})`;
    const entry = { proto, lines: [] };
    this.functions.push(entry);
    if (f.frame) this.frames.push(f);
    const saved = { out: this.out, indent: this.indent, ctx: this.ctx, fn: this.fn };
    this.out = entry.lines;
    this.indent = 1;
    this.ctx = { loops: 0, iife: false };
    this.fn = f;
    this.emit(`rt_enter(${JSON.stringify(s.name)});`);
    if (f.frame) this.emit(`struct fr${f.id} fr = { ${parent ? '.up = up' : '0'} };`);
    // A fickle parameter takes the argument as it is (§6.1).
    for (const p of s.params) {
      const d = p.decl;
      const arg = d.captured ? `p_${d.cname}` : d.cname;
      const v = p.fickle ? arg : `rt_bind(${arg}, ${p.shifty ? 1 : 0})`;
      if (d.captured) this.emit(`fr.${d.cname} = ${v};`);
      else if (!p.fickle) this.emit(`${d.cname} = ${v};`);
    }
    this.stmts(s.body.stmts);
    this.emit(`RT_RET(${this.valueOf(s.body)});`);
    Object.assign(this, saved);
  }

  // The value bound by claim/cheat, applying the column of the copy table for
  // mode ('const', 'shifty' or 'fickle', §10.4, §10.9). A literal is frozen
  // unless bound shifty; fickle keeps any other value as it is.
  bindRhs(e, mode) {
    const shifty = mode === 'shifty';
    if (e.k === 'arr') return this.array(e, shifty).c;
    if (e.k === 'rec') return this.record(e, shifty).c;
    if (SCALAR.has(e.k) || mode === 'fickle') return this.expr(e).c;
    return `rt_bind(${this.expr(e).c}, ${shifty ? 1 : 0})`;
  }

  cheat(s) {
    const t = s.target;
    if (t.k === 'name') {
      const d = t.decl;
      const at = this.place(d);
      if (!s.op) {
        const v = this.bindRhs(s.e, t.fickle ? 'fickle' : 'shifty');
        if (!this.tdz(d)) return this.emit(`${at} = ${v};`);
        // The value is evaluated first; then the binding must exist (§5.1).
        const tmp = this.tmp();
        this.emit(`wv ${tmp} = ${v};`);
        this.emit(`(void)rt_def(${at}, ${JSON.stringify(d.name)});`);
        return this.emit(`${at} = ${tmp};`);
      }
      // The value is evaluated before the target is read (source order).
      let v = this.expr(s.e);
      if (s.e.k !== 'int' && s.e.k !== 'name') {
        const tmp = this.tmp();
        this.emit(`wv ${tmp} = ${v.c};`);
        v = { c: tmp, k: CONST };
      }
      return this.emit(`${at} = ${this.call(([a, b]) => `rt_${BIN[s.op]}(${a}, ${b})`, [this.read(d), v]).c};`);
    }
    const key = t.k === 'mem' ? { c: this.field(t.field), k: CONST } : null;
    const args = [this.expr(s.e), key ?? this.expr(t.index), this.expr(t.k === 'mem' ? t.rec : t.arr)];
    const fn = `rt_${t.k === 'mem' ? 'set_mem' : 'set_idx'}`;
    if (!s.op) return this.emit(`${this.call((cs) => `${fn}(${cs.join(', ')})`, args).c};`);
    const upd = `rt_${t.k === 'mem' ? 'upd_mem' : 'upd_idx'}`;
    return this.emit(`${this.call((cs) => `${upd}(${cs.join(', ')}, rt_${BIN[s.op]})`, args).c};`);
  }

  array(e, shifty) {
    const items = e.items.map((i) => this.expr(i));
    return this.call((cs) => `rt_arr(${cs.length}, ${this.values(cs.length, cs)}, ${shifty ? 1 : 0})`, items);
  }

  // Records of one literal share their keys: a static shape K<n>.
  record(e, shifty) {
    const names = e.fields.map((f) => f.name);
    let shape = 'NULL';
    if (names.length) {
      const key = names.join(',');
      for (const n of names) this.field(n);
      if (!this.shapes.has(key)) this.shapes.set(key, this.shapes.size);
      shape = `K${this.shapes.get(key)}`;
    }
    const vals = e.fields.map((f) => this.expr(f.e));
    return this.call((cs) => `rt_rec(${shifty ? 1 : 0}, ${cs.length}, ${shape}, ${this.values(cs.length, cs)})`, vals);
  }

  // `whine` catches a signal from the statement's outermost call only, so the
  // call's arguments are evaluated before it.
  whine(s) {
    const call = s.e;
    const args = call.args.map((a) => {
      if (PLAIN.has(a.k)) return this.expr(a).c;
      const t = this.tmp();
      this.emit(`wv ${t} = ${this.expr(a).c};`);
      return t;
    });
    const r = this.tmp();
    this.emit(`wv ${r} = ${this.callC(call.callee, args, false)};`);
    this.emit('if (rt_sigcode) {');
    this.indent++;
    if (s.whine.decl) this.claim(s.whine.decl, 'rt_i(rt_sigcode)');
    this.emit('rt_sigcode = 0;');
    this.emit('{');
    this.indent++;
    this.resets(s.whine.body);
    this.stmts(s.whine.body.stmts);
    this.emit(`${r} = ${this.valueOf(s.whine.body)};`);
    this.indent--;
    this.emit('}');
    this.indent--;
    this.emit('}');
    this.stmt({ ...s, whine: null, e: { k: 'raw', c: r } });
  }

  chainStmt(e) {
    e.branches.forEach((b, i) => {
      this.emit(`${i ? '} else ' : ''}if (${b.neg ? '!' : ''}rt_cond(${this.expr(b.cond).c})) {`);
      this.indent++;
      this.blockBody(b.body);
      this.indent--;
    });
    if (e.els) {
      this.emit('} else {');
      this.indent++;
      this.blockBody(e.els);
      this.indent--;
    }
    this.emit('}');
  }

  // A conditional used as a value: a ternary chain when every branch is just a
  // value, otherwise a statement expression.
  chainExpr(e) {
    const blocks = [...e.branches.map((b) => b.body), e.els];
    const test = (b) => `${b.neg ? '!' : ''}rt_cond(${this.expr(b.cond).c})`;
    if (blocks.every((b) => b.stmts.every((s) => s.k === 'empty'))) {
      let c = this.valueOf(e.els);
      for (let i = e.branches.length - 1; i >= 0; i--) {
        c = `(${test(e.branches[i])} ? ${this.valueOf(e.branches[i].body)} : ${c})`;
      }
      return c;
    }
    return this.branchValue(null, e.branches.map((b) => () => test(b)), e.branches.map((b) => b.body), e.els);
  }

  // A statement expression ({ ... }) that runs the first block whose test
  // holds and has its value. subject, if given, is a temporary set first.
  // Each test is called while its line is emitted, so temporaries are
  // numbered in source order.
  branchValue(subject, tests, blocks, els) {
    const saved = { out: this.out, indent: this.indent, ctx: this.ctx };
    this.out = [];
    this.indent = saved.indent + 1;
    this.ctx = { loops: 0, iife: true };
    if (subject) this.emit(`wv ${subject.t} = ${subject.c};`);
    const r = this.tmp();
    this.emit(`wv ${r};`);
    blocks.forEach((b, i) => {
      this.emit(`${i ? '} else ' : ''}if (${tests[i]()}) {`);
      this.indent++;
      this.resets(b);
      this.stmts(b.stmts);
      this.emit(`${r} = ${this.valueOf(b)};`);
      this.indent--;
    });
    this.emit('} else {');
    this.indent++;
    this.resets(els);
    this.stmts(els.stmts);
    this.emit(`${r} = ${this.valueOf(els)};`);
    this.indent--;
    this.emit('}');
    this.emit(`${r};`);
    const body = this.out.join('\n');
    this.out = saved.out;
    this.indent = saved.indent;
    this.ctx = saved.ctx;
    return `({\n${body}\n${'  '.repeat(this.indent)}})`;
  }

  // suspect (§7.5): the subject is evaluated once into a temporary, then
  // compared with each alibi value by ==.
  alibiTest(t, a) {
    return a.values.map((v) => `eq(${t}, ${this.expr(v).c})`).join(' || ');
  }

  // The anyway block, or for an exhaustive lineup suspect (which has none), a
  // block that raises a tantrum for a subject that is not one of the members.
  suspectElse(e, t) {
    if (e.els || e.alibis[0].values[0].k !== 'mem') return e.els;
    return { stmts: [], claims: [], value: { k: 'raw', c: `(rt_no_alibi(${t}), RT_NULL)` } };
  }

  suspectStmt(e) {
    const t = this.tmp();
    this.emit(`wv ${t} = ${this.expr(e.subject).c};`);
    e.alibis.forEach((a, i) => {
      this.emit(`${i ? '} else ' : ''}if (${this.alibiTest(t, a)}) {`);
      this.indent++;
      this.blockBody(a.body);
      this.indent--;
    });
    const els = this.suspectElse(e, t);
    if (els) {
      this.emit('} else {');
      this.indent++;
      this.blockBody(els);
      this.indent--;
    }
    this.emit('}');
  }

  // A suspect used as a value: a statement expression that sets the subject,
  // then a ternary chain when every block is just a value.
  suspectExpr(e) {
    const t = this.tmp();
    const subject = this.expr(e.subject).c;
    const els = this.suspectElse(e, t);
    const blocks = [...e.alibis.map((a) => a.body), els];
    if (blocks.every((b) => b.stmts.every((s) => s.k === 'empty'))) {
      let c = this.valueOf(els);
      for (let i = e.alibis.length - 1; i >= 0; i--) {
        c = `(${this.alibiTest(t, e.alibis[i])} ? ${this.valueOf(e.alibis[i].body)} : ${c})`;
      }
      return `({ wv ${t} = ${subject}; ${c}; })`;
    }
    return this.branchValue({ t, c: subject }, e.alibis.map((a) => () => this.alibiTest(t, a)), e.alibis.map((a) => a.body), els);
  }

  // A call; checked: a signal it leaves pending becomes a tantrum here (§8.2).
  callC(callee, args, checked) {
    let c;
    if (callee.k === 'std') {
      c = `std_${callee.name}(${args.join(', ')})`;
      if (!WAILS.has(callee.name)) return c;
    } else {
      let f, parent = null;
      if (callee.k === 'mem') f = `w${this.stolenFrom(callee.rec).id}_${callee.field}`;
      else {
        const sf = callee.decl.schemeFn;
        f = sf.cname;
        if (!sf.parent.top) parent = sf.parent;
      }
      // A nested scheme gets its parent's frame: ours, or one up the chain.
      const up = !parent ? [] : [parent === this.fn ? '&fr' : `up${'->up'.repeat(this.fn.depth - parent.depth - 1)}`];
      c = `${f}(${[...up, ...args].join(', ')})`;
    }
    return checked ? `rt_chk(${c}, ${this.fn.top ? 'NULL' : JSON.stringify(this.fn.name)})` : c;
  }

  expr(e) {
    switch (e.k) {
      case 'int': return { c: `rt_i(${e.v})`, k: CONST };
      case 'float': return { c: `rt_f(${fmtFloat(e.v)})`, k: CONST };
      case 'str': {
        const parts = e.parts.filter((p) => p !== '')
          .map((p) => (typeof p === 'string' ? { c: `rt_s(${this.str(p)})`, k: CONST } : { c: `rt_text(${this.expr(p).c})`, k: EFFECT }));
        if (parts.length === 0) return { c: `rt_s(${this.str('')})`, k: CONST };
        if (parts.length === 1 && parts[0].k === CONST) return parts[0];
        return this.call((cs) => `rt_cat(${cs.length}, ${this.values(cs.length, cs)})`, parts);
      }
      case 'bool': return { c: `rt_b(${e.v ? 1 : 0})`, k: CONST };
      case 'null': return { c: 'RT_NULL', k: CONST };
      case 'nan': return { c: 'RT_NOBODY', k: CONST };
      case 'inf': return { c: 'RT_EVERYBODY', k: CONST };
      case 'name': return this.read(e.decl);
      case 'std': return { c: `std_${e.name}()`, k: CONST };
      case 'arr': return this.array(e, false);
      case 'rec': return this.record(e, false);
      case 'idx': return this.call(([i, a]) => `rt_idx(${i}, ${a})`, [this.expr(e.index), this.expr(e.arr)]);
      case 'mem': {
        // A lineup member, a claim stolen from a module, or a record field. The
        // checker forbids shadowing lineup and module names, so these are exact.
        const lineup = this.lineupOf(e.rec);
        if (lineup) return { c: `rt_m(&${lineup.c}[${lineup.node.members.findIndex((m) => m.name === e.field)}])`, k: CONST };
        const lib = this.stolenFrom(e.rec);
        if (lib) return { c: `w${lib.id}_${e.field}`, k: PURE };
        return this.call(([k, r]) => `rt_get(${k}, ${r})`, [{ c: this.field(e.field), k: CONST }, this.expr(e.rec)]);
      }
      case 'un': return { c: `rt_${UN[e.op]}(${this.expr(e.e).c})`, k: EFFECT };
      case 'bin':
        if (e.op === '&&' || e.op === '||') {
          return { c: `rt_b(rt_bool(${this.expr(e.l).c}, "${e.op}") ${e.op} rt_bool(${this.expr(e.r).c}, "${e.op}"))`, k: EFFECT };
        }
        return this.call(([l, r]) => `rt_${BIN[e.op]}(${l}, ${r})`, [this.expr(e.l), this.expr(e.r)]);
      case 'tern': return { c: `(rt_cond(${this.expr(e.c).c}) ? ${this.expr(e.a).c} : ${this.expr(e.b).c})`, k: EFFECT };
      case 'call': {
        const args = e.args.map((a) => this.expr(a));
        return this.call((cs) => this.callC(e.callee, cs, true), args);
      }
      case 'chain': return { c: this.chainExpr(e), k: EFFECT };
      case 'suspect': return { c: this.suspectExpr(e), k: EFFECT };
      case 'raw': return { c: e.c, k: CONST };
    }
    throw new Error(`codegen: unhandled expression ${e.k}`);
  }

  // The whole program: the runtime, then static data, then the functions.
  assemble() {
    const out = [RUNTIME, '// ---- program ----', ''];
    for (const [s, i] of this.strings) out.push(`static const wstr S${i} = ${wstrInit(s)};`);
    for (const f of this.fields) out.push(`static wstr F_${f} = ${wstrInit(f)};`);
    for (const [key, i] of this.shapes) {
      out.push(`static const wstr *const K${i}[] = { ${key.split(',').map((f) => `&F_${f}`).join(', ')} };`);
    }
    out.push(...this.lineups);
    for (const f of this.frames) {
      const fields = f.decls.filter((d) => d.captured).map((d) => `wv ${d.cname};`);
      if (!f.parent.top) fields.unshift(`struct fr${f.parent.id} *up;`);
      if (!fields.length) fields.push('char none;');
      out.push(`struct fr${f.id} { ${fields.join(' ')} };`);
    }
    for (const g of this.globals) out.push(`static wv ${g};`);
    for (const f of this.functions) out.push(`${f.proto};`);
    for (const f of this.functions) out.push('', `${f.proto} {`, ...f.lines, '}');
    out.push('', 'static void wah_main(void) {');
    if (this.fields.length) out.push(`  rt_syms(${this.fields.length}, (wstr *const[]){ ${this.fields.map((f) => `&F_${f}`).join(', ')} });`);
    out.push(...this.main, '}', '');
    return out.join('\n');
  }
}
