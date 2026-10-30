// Code generation: checked AST -> standalone JavaScript (spec §15 codegen notes).
//
// User names are emitted as w_<name>; stdlib names as $std.<name>; record
// fields as f_<name>; every operator goes through a $rt helper that enforces
// Γ-- typing rules. Library k (in initialization order) uses w<k>_<name>, so
// private names in different modules never collide (§13.1).

import { readFileSync } from 'node:fs';
import { WahError, err } from './errors.js';

const RUNTIME = readFileSync(new URL('./runtime.js', import.meta.url), 'utf8');

const BIN = {
  '+': 'add', '-': 'sub', '*': 'mul', '/': 'div', '%': 'mod', '**': 'pow',
  '==': 'eq', '!=': 'ne', '<': 'lt', '>': 'gt', '<=': 'le', '>=': 'ge',
  '&': 'band', '|': 'bor', '^': 'bxor', '<<': 'shl', '>>': 'shr',
};
const UN = { '-': 'neg', '!': 'not', '~': 'bnot' };
// How a claim or parameter binds its value (§10.4).
export const mode = (d) => (d.shifty ? 'shifty' : d.fickle ? 'fickle' : 'const');
// Node kinds whose value can never be an array (so binding needs no copy check).
const SCALAR = new Set(['int', 'float', 'str', 'bool', 'null', 'nan', 'inf', 'bin', 'un']);
// Node kinds that evaluate without calling anything.
const PLAIN = new Set(['int', 'float', 'bool', 'null', 'nan', 'inf', 'name', 'std']);

// modules: checked, in initialization order, the entry file last. Libraries
// run first, in that order, then the entry file's body (§13.1).
export function generate(modules) {
  const g = new Gen();
  g.indent = 1;
  modules.forEach((m, i) => { m.id = i === modules.length - 1 ? '' : String(i + 1); });
  // Lineups are visible throughout their file, so they are all created first (§4.1).
  for (const m of modules) for (const s of m.program.stmts) if (s.k === 'lineup') g.lineup(s, m.id);
  for (const m of modules) {
    g.enter(m);
    g.stmts(m.program.stmts);
  }
  return `"use strict";\n${RUNTIME}\n$rt.main(function () {\n${g.out.join('\n')}\n});\n`;
}

class Gen {
  constructor() {
    this.out = [];
    this.indent = 0;
    this.n = 0;
    // loops: enclosing loops reachable by bail/loiter; iife: inside an expression-valued conditional
    this.ctx = { loops: 0, iife: false };
  }

  // Starts module m: its own names get its id; name.module, the stolen module's (§13.1).
  enter(m) {
    this.src = m.src;
    this.mod = m.id;
    this.modules = new Map(m.steals.map(({ node, lib }) => [node.name, lib]));
    this.lineups = new Set(m.program.stmts.filter((s) => s.k === 'lineup').map((s) => s.name));
  }

  id(name) { return `w${this.mod}_${name}`; }

  // The module that x names, or undefined. The checker forbids shadowing a module name.
  stolenFrom(x) { return x.k === 'name' ? this.modules.get(x.name) : undefined; }

  // The JS name of the lineup x names (Lineup or Lineup.module), or null.
  lineupJs(x) {
    if (x.k === 'name') return this.lineups.has(x.name) ? `l${this.mod}_${x.name}` : null;
    const lib = x.k === 'mem' ? this.stolenFrom(x.rec) : undefined;
    return lib && lib.exports.get(x.field).kind === 'lineup' ? `l${lib.id}_${x.field}` : null;
  }
  emit(line) { this.out.push('  '.repeat(this.indent) + line); }
  tmp() { return `$t${this.n++}`; }
  fail(pos, msg) { throw new WahError([{ ...err(pos, msg), src: this.src }]); }

  stmts(list) { for (const s of list) this.stmt(s); }

  // A block whose value is discarded.
  blockBody(b) {
    this.stmts(b.stmts);
    if (b.value) this.emit(`${this.expr(b.value)};`);
  }

  valueOf(b) { return b.value ? this.expr(b.value) : 'null'; }

  stmt(s) {
    if (s.whine) return this.whine(s);
    switch (s.k) {
      case 'empty': case 'lineup': case 'steal': case 'bribe': return;
      case 'scheme': return this.scheme(s);
      case 'getaway':
        if (this.ctx.iife) this.fail(s.pos, "'getaway' inside a conditional used as a value is not supported in stage 0");
        return this.emit(`return ${s.e ? this.expr(s.e) : 'null'};`);
      case 'tantrum': return this.emit(`throw $rt.tantrum(${this.expr(s.e)});`);
      case 'bail': case 'loiter':
        if (!this.ctx.loops) this.fail(s.pos, `'${s.k}' cannot leave a conditional used as a value in stage 0`);
        return this.emit(s.k === 'bail' ? 'break;' : 'continue;');
      case 'lurk':
        this.emit(`while (${s.neg ? '!' : ''}$rt.cond(${this.expr(s.cond)})) {`);
        this.indent++;
        this.ctx.loops++;
        this.blockBody(s.body);
        this.ctx.loops--;
        this.indent--;
        return this.emit('}');
      case 'signal': return this.emit(`throw $rt.signal(${s.code});`);
      case 'exit': return this.emit(`$rt.exit(${s.e ? this.expr(s.e) : '0'});`);
      case 'expr':
        if (s.e.k === 'chain') return this.chainStmt(s.e);
        if (s.e.k === 'suspect') return this.suspectStmt(s.e);
        if (s.e.k === 'sigcall') return this.emit(`throw $rt.signal(${this.expr(s.e.arg)});`);
        return this.emit(`${this.expr(s.e)};`);
      case 'claim':
        return this.emit(`${s.shifty || s.fickle ? 'let' : 'const'} ${this.id(s.name)} = ${this.bindRhs(s.e, mode(s))};`);
      case 'cheat': return this.cheat(s);
    }
    throw new Error(`codegen: unhandled statement ${s.k}`);
  }

  scheme(s) {
    this.emit(`function ${this.id(s.name)}(${s.params.map((p) => this.id(p.name)).join(', ')}) {`);
    this.indent++;
    // A fickle parameter takes the argument as it is (§6.1).
    for (const p of s.params) if (!p.fickle) this.emit(`${this.id(p.name)} = $rt.bind(${this.id(p.name)}, ${p.shifty});`);
    this.emit('try {');
    this.indent++;
    const saved = this.ctx;
    this.ctx = { loops: 0, iife: false };
    this.stmts(s.body.stmts);
    this.emit(`return ${this.valueOf(s.body)};`);
    this.ctx = saved;
    this.indent--;
    this.emit('} catch ($e) {');
    this.emit(`  throw $rt.leave($e, ${JSON.stringify(s.name)});`);
    this.emit('}');
    this.indent--;
    this.emit('}');
  }

  // A lineup is l_<name>, a frozen object whose m_<member> properties are its members.
  lineup(s, mod) {
    this.emit(`const l${mod}_${s.name} = $rt.lineup(${JSON.stringify(s.name)}, [${s.members.map((m) => JSON.stringify(m.name)).join(', ')}]);`);
  }

  // The value bound by claim/cheat, applying the column of the copy table for
  // mode ('const', 'shifty' or 'fickle', §10.4, §10.9). A literal is frozen
  // unless bound shifty; fickle keeps any other value as it is.
  bindRhs(e, mode) {
    const shifty = mode === 'shifty';
    if (e.k === 'arr') {
      const items = e.items.map((i) => this.expr(i)).join(', ');
      return shifty ? `[${items}]` : `$rt.frozen([${items}])`;
    }
    if (e.k === 'rec') return this.record(e, shifty);
    if (SCALAR.has(e.k) || mode === 'fickle') return this.expr(e);
    return `$rt.bind(${this.expr(e)}, ${shifty})`;
  }

  cheat(s) {
    const t = s.target;
    if (t.k === 'name') {
      if (!s.op) return this.emit(`${this.id(t.name)} = ${this.bindRhs(s.e, t.fickle ? 'fickle' : 'shifty')};`);
      // The value is evaluated before the target is read (source order).
      let v = this.expr(s.e);
      if (s.e.k !== 'int' && s.e.k !== 'name') {
        const tmp = this.tmp();
        this.emit(`const ${tmp} = ${v};`);
        v = tmp;
      }
      return this.emit(`${this.id(t.name)} = $rt.${BIN[s.op]}(${this.id(t.name)}, ${v});`);
    }
    if (t.k === 'mem') {
      const args = `${this.expr(s.e)}, "f_${t.field}", ${this.expr(t.rec)}`;
      if (!s.op) return this.emit(`$rt.setMem(${args});`);
      return this.emit(`$rt.updMem(${args}, $rt.${BIN[s.op]});`);
    }
    const args = `${this.expr(s.e)}, ${this.expr(t.index)}, ${this.expr(t.arr)}`;
    if (!s.op) return this.emit(`$rt.setIdx(${args});`);
    return this.emit(`$rt.updIdx(${args}, $rt.${BIN[s.op]});`);
  }

  // A null-prototype object with mangled keys; values evaluate in source order.
  record(e, shifty) {
    const fields = e.fields.map((f) => `, f_${f.name}: ${this.expr(f.e)}`).join('');
    return `$rt.rec({ __proto__: null${fields} }, ${shifty})`;
  }

  // `whine` catches a signal from the statement's outermost call only, so the
  // call's arguments are evaluated before the try.
  whine(s) {
    const call = s.e;
    const args = call.args.map((a) => {
      if (PLAIN.has(a.k)) return this.expr(a);
      const t = this.tmp();
      this.emit(`const ${t} = ${this.expr(a)};`);
      return t;
    });
    const r = this.tmp();
    const ev = `$e${this.n++}`;
    this.emit(`let ${r};`);
    this.emit('try {');
    this.emit(`  ${r} = ${this.callJs(call.callee, args)};`);
    this.emit(`} catch (${ev}) {`);
    this.indent++;
    this.emit(`if (!(${ev} instanceof $rt.Wah)) throw ${ev};`);
    if (s.whine.param) this.emit(`const ${this.id(s.whine.param)} = ${ev}.code;`);
    this.emit('{');
    this.indent++;
    this.stmts(s.whine.body.stmts);
    this.emit(`${r} = ${this.valueOf(s.whine.body)};`);
    this.indent--;
    this.emit('}');
    this.indent--;
    this.emit('}');
    this.stmt({ ...s, whine: null, e: { k: 'raw', js: r } });
  }

  chainStmt(e) {
    e.branches.forEach((b, i) => {
      this.emit(`${i ? '} else ' : ''}if (${b.neg ? '!' : ''}$rt.cond(${this.expr(b.cond)})) {`);
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
  // value, otherwise an immediately-invoked arrow function.
  chainExpr(e) {
    const blocks = [...e.branches.map((b) => b.body), e.els];
    const test = (b) => `${b.neg ? '!' : ''}$rt.cond(${this.expr(b.cond)})`;
    if (blocks.every((b) => b.stmts.every((s) => s.k === 'empty'))) {
      let js = this.valueOf(e.els);
      for (let i = e.branches.length - 1; i >= 0; i--) {
        js = `(${test(e.branches[i])} ? ${this.valueOf(e.branches[i].body)} : ${js})`;
      }
      return js;
    }
    const body = this.branchFunction(e.branches.map((b) => () => test(b)), e.branches.map((b) => b.body), e.els);
    return `(() => {\n${body}\n${'  '.repeat(this.indent)}})()`;
  }

  // The body of an arrow function that runs the first block whose test holds
  // and returns its value. Each test is called while its line is emitted, so
  // temporaries are numbered in source order.
  branchFunction(tests, blocks, els) {
    const saved = { out: this.out, indent: this.indent, ctx: this.ctx };
    this.out = [];
    this.indent = saved.indent + 1;
    this.ctx = { loops: 0, iife: true };
    blocks.forEach((b, i) => {
      this.emit(`${i ? '} else ' : ''}if (${tests[i]()}) {`);
      this.indent++;
      this.stmts(b.stmts);
      this.emit(`return ${this.valueOf(b)};`);
      this.indent--;
    });
    this.emit('} else {');
    this.indent++;
    this.stmts(els.stmts);
    this.emit(`return ${this.valueOf(els)};`);
    this.indent--;
    this.emit('}');
    const body = this.out.join('\n');
    this.out = saved.out;
    this.indent = saved.indent;
    this.ctx = saved.ctx;
    return body;
  }

  // suspect (§7.5): the subject is evaluated once into a temporary, then
  // compared with each alibi value by ==.
  alibiTest(t, a) {
    return a.values.map((v) => `$rt.eq(${t}, ${this.expr(v)})`).join(' || ');
  }

  // The anyway block, or for an exhaustive lineup suspect (which has none), a
  // block that raises a tantrum for a subject that is not one of the members.
  suspectElse(e, t) {
    if (e.els || e.alibis[0].values[0].k !== 'mem') return e.els;
    return { stmts: [], value: { k: 'raw', js: `$rt.noAlibi(${t})` } };
  }

  suspectStmt(e) {
    const t = this.tmp();
    this.emit(`const ${t} = ${this.expr(e.subject)};`);
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

  // A suspect used as a value: an arrow function taking the subject, whose body
  // is a ternary chain when every block is just a value.
  suspectExpr(e) {
    const t = this.tmp();
    const subject = this.expr(e.subject);
    const els = this.suspectElse(e, t);
    const blocks = [...e.alibis.map((a) => a.body), els];
    if (blocks.every((b) => b.stmts.every((s) => s.k === 'empty'))) {
      let js = this.valueOf(els);
      for (let i = e.alibis.length - 1; i >= 0; i--) {
        js = `(${this.alibiTest(t, e.alibis[i])} ? ${this.valueOf(e.alibis[i].body)} : ${js})`;
      }
      return `((${t}) => ${js})(${subject})`;
    }
    const body = this.branchFunction(e.alibis.map((a) => () => this.alibiTest(t, a)), e.alibis.map((a) => a.body), els);
    return `((${t}) => {\n${body}\n${'  '.repeat(this.indent)}})(${subject})`;
  }

  callJs(callee, args) {
    const fn = callee.k === 'std' ? `$std.${callee.name}`
      : callee.k === 'mem' ? `w${this.stolenFrom(callee.rec).id}_${callee.field}` : this.id(callee.name);
    return `${fn}(${args.join(', ')})`;
  }

  expr(e) {
    switch (e.k) {
      case 'int': return e.v < 0 ? `(${e.v})` : String(e.v);
      case 'float': return `$rt.f(${Object.is(e.v, -0) ? '-0' : String(e.v)})`;
      case 'str': {
        const parts = e.parts
          .filter((p) => p !== '')
          .map((p) => (typeof p === 'string' ? JSON.stringify(p) : `$rt.text(${this.expr(p)})`));
        if (parts.length === 0) return '""';
        return parts.length === 1 ? parts[0] : `(${parts.join(' + ')})`;
      }
      case 'bool': return String(e.v);
      case 'null': return 'null';
      case 'nan': return '$rt.NOBODY';
      case 'inf': return '$rt.EVERYBODY';
      case 'name': return this.id(e.name);
      case 'std': return `$std.${e.name}`;
      case 'arr': return `$rt.frozen([${e.items.map((i) => this.expr(i)).join(', ')}])`;
      case 'rec': return this.record(e, false);
      case 'idx': return `$rt.idx(${this.expr(e.index)}, ${this.expr(e.arr)})`;
      case 'mem': {
        // A lineup member, a claim stolen from a module, or a record field. The
        // checker forbids shadowing lineup and module names, so these are exact.
        const lineup = this.lineupJs(e.rec);
        if (lineup) return `${lineup}.m_${e.field}`;
        const lib = this.stolenFrom(e.rec);
        if (lib) return `w${lib.id}_${e.field}`;
        return `$rt.get("f_${e.field}", ${this.expr(e.rec)})`;
      }
      case 'un': return `$rt.${UN[e.op]}(${this.expr(e.e)})`;
      case 'bin':
        if (e.op === '&&' || e.op === '||') {
          return `($rt.bool(${this.expr(e.l)}, "${e.op}") ${e.op} $rt.bool(${this.expr(e.r)}, "${e.op}"))`;
        }
        return `$rt.${BIN[e.op]}(${this.expr(e.l)}, ${this.expr(e.r)})`;
      case 'tern': return `($rt.cond(${this.expr(e.c)}) ? ${this.expr(e.a)} : ${this.expr(e.b)})`;
      case 'call': return this.callJs(e.callee, e.args.map((a) => this.expr(a)));
      case 'chain': return this.chainExpr(e);
      case 'suspect': return this.suspectExpr(e);
      case 'raw': return e.js;
    }
    throw new Error(`codegen: unhandled expression ${e.k}`);
  }
}
