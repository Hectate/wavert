// Semantic checks (spec §5.1, §6, §7.2, §8.2, §12, §13).
//
// - names resolve; no shadowing; no use of a claim before it in the same function
// - cheat only on shifty or fickle bindings (or fields/elements of them); schemes are not values; call arity
// - lineup members exist; lineups are not values; suspects over members are exhaustive
// - getaway / bail / loiter / WAH! only where they make sense
// - conditional chains used as values end in a plain `anyway { ... }`
// - the entry file ends with ;!invited; libraries end with their bribe (§13.1)
// - a steal names its module; what the module bribes is used as name.module, and only it cheats its claims

import { WahError, err } from './errors.js';
import { STDLIB } from './stdlib.js';

class Scope {
  constructor(parent, fn) {
    this.parent = parent;
    this.fn = fn;
    this.names = new Map();
  }
}

// modules: in initialization order, the entry file last (compile.js).
export function check(modules, { requireExit = true } = {}) {
  const c = new Checker();
  c.run(modules, requireExit);
  if (c.errors.length) throw new WahError(c.errors);
}

const plural = (n) => `${n} argument${n === 1 ? '' : 's'}`;

// Statements after which a block never reaches its end (§7.2).
const NEVER_COMPLETES = new Set(['getaway', 'tantrum', 'exit', 'signal', 'bail', 'loiter']);
// Statements whose whine handler's value is used (§7.2).
const WHINE_VALUE_USED = new Set(['claim', 'cheat', 'getaway', 'tantrum', 'exit']);
// What a library's top level may contain (§13.1).
const LIBRARY_TOP = new Set(['empty', 'steal', 'scheme', 'claim', 'lineup', 'bribe']);

// Does a scheme body contain a ;getaway with a value, outside nested schemes?
function getsAwayWithValue(stmts) {
  return stmts.some((s) => {
    if (s.whine && getsAwayWithValue(s.whine.body.stmts)) return true;
    switch (s.k) {
      case 'getaway': return s.e !== null;
      case 'lurk': return getsAwayWithValue(s.body.stmts);
      case 'expr': {
        const e = s.e;
        const els = () => e.els !== null && getsAwayWithValue(e.els.stmts);
        if (e.k === 'chain') return e.branches.some((b) => getsAwayWithValue(b.body.stmts)) || els();
        if (e.k === 'suspect') return e.alibis.some((a) => getsAwayWithValue(a.body.stmts)) || els();
        return false;
      }
      default: return false;
    }
  });
}

// A lineup as written: Lineup, or Lineup.module for a stolen one (§4.1, §13.1).
const lineupText = (x) => (x.k === 'name' ? x.name : `${x.field}.${x.rec.name}`);

// An alibi value's identity for duplicate detection, and its text in messages (§7.5).
const alibiKey = (v) => {
  switch (v.k) {
    case 'str': return `str:${v.parts[0]}`;
    case 'mem': return `mem:${v.field}.${lineupText(v.rec)}`;
    default: return `${v.k}:${v.v}`;
  }
};
const alibiText = (v) => {
  switch (v.k) {
    case 'str': return JSON.stringify(v.parts[0]);
    case 'mem': return `${v.field}.${lineupText(v.rec)}`;
    case 'bool': return v.v ? 'real' : 'fake';
    case 'null': return 'uninvited';
    default: return String(v.v);
  }
};

class Checker {
  constructor() {
    this.src = null; // the module being checked
    this.errors = [];
    this.schemes = []; // scheme declarations, in order, for the explicit-value check
  }

  error(pos, msg, hint) { this.errors.push({ ...err(pos, msg, hint), src: this.src }); }
  line(pos) { return this.src.loc(pos).line; }

  run(modules, requireExit) {
    const entry = modules[modules.length - 1];
    for (const m of modules) this.module(m, m === entry);
    // A scheme whose value is used must be able to produce one (§7.2).
    for (const d of this.schemes) {
      const body = d.node.body;
      if (d.usedAt !== null && body.value === null && !getsAwayWithValue(body.stmts)) {
        this.src = d.src;
        const where = d.usedSrc === d.src ? `line ${this.line(d.usedAt)}` : `${d.usedSrc.file} line ${d.usedSrc.loc(d.usedAt).line}`;
        this.error(d.namePos, `'${d.node.name}' never produces a value, but its value is used (${where})`,
          'end the scheme with its value, or with uninvited if that is intended');
      }
    }
    this.src = entry.src;
    const prog = entry.program;
    if (requireExit) {
      const last = prog.stmts.filter((s) => s.k !== 'empty').pop();
      if (!last || last.k !== 'exit') {
        this.error(prog.end, "program ended, but Waluigi was never told he wasn't invited", 'end the entry file with ;!invited');
      }
    }
  }

  // Checks one file, and records in m.exports the declarations it bribes.
  module(m, isEntry) {
    this.src = m.src;
    m.exports = new Map();
    const last = m.program.stmts.filter((s) => s.k !== 'empty').pop();
    // A stolen file that is not a library is reported where it is stolen.
    m.notLibrary = !isEntry && last?.k === 'exit';
    if (m.notLibrary) return;
    const top = { name: null, top: true };
    const scope = new Scope(null, top);
    this.steal(m, scope);
    this.block({ stmts: m.program.stmts, value: null }, scope, { fn: top, loops: 0 });
    this.library(m, scope, isEntry, last);
  }

  // Each steal declares a module name, visible throughout the file like a
  // scheme name (§13.1). A module whose steal was in error has lib null, so
  // its uses are not reported again.
  steal(m, scope) {
    const seen = new Set();
    for (const { node, lib } of m.steals) {
      let usable = lib;
      if (seen.has(lib)) {
        this.error(node.pathPos, `'${lib.path}' is already stolen`);
        usable = null;
      } else if (lib.notLibrary) {
        this.error(node.pathPos, `'${lib.path}' is not a library: it ends with ;!invited`);
        usable = null;
      }
      seen.add(lib);
      this.declare(scope, node.name, { kind: 'module', lib: usable, namePos: node.namePos, fn: null });
    }
  }

  // The module that name is in scope, or null.
  moduleNamed(x, scope) {
    if (x.k !== 'name') return null;
    const d = this.lookup(scope, x.name);
    return d && d.kind === 'module' ? d : null;
  }

  // The lineup that x names, written Lineup or Lineup.module, or null. Reports nothing.
  lineupNamed(x, scope) {
    if (x.k === 'name') {
      const d = this.lookup(scope, x.name);
      return d && d.kind === 'lineup' ? d : null;
    }
    if (x.k !== 'mem') return null;
    const m = this.moduleNamed(x.rec, scope);
    const d = m?.lib?.exports.get(x.field);
    return d && d.kind === 'lineup' ? d : null;
  }

  // The declaration that name.module refers to, or null after an error (or
  // when the module's steal was already reported).
  bribed(e, m) {
    if (!m.lib) return null;
    const d = m.lib.exports.get(e.field);
    if (!d) this.error(e.pos, `'${m.lib.path}' does not bribe '${e.field}'`);
    return d ?? null;
  }

  // A library's top level and its bribe; bribe anywhere else (§13.1).
  library(m, scope, isEntry, last) {
    for (const s of m.program.stmts) {
      if (s.k === 'bribe' && (isEntry || s !== last)) {
        this.error(s.pos, "'bribe' must be the last statement of a library");
      } else if (!isEntry && !LIBRARY_TOP.has(s.k)) {
        this.error(s.pos, "a library's top level may contain only steal, scheme, claim and lineup statements",
          'move other statements into a scheme');
      }
    }
    if (isEntry) return;
    if (!last || last.k !== 'bribe') {
      this.error(m.program.end, 'a library must end with the names it exports', ';a, b bribe');
      return;
    }
    for (const n of last.names) {
      const d = scope.names.get(n.name);
      if (!d) this.error(n.pos, `cannot bribe '${n.name}': it is not declared at the top level`);
      else if (d.kind === 'module') this.error(n.pos, `cannot bribe '${n.name}': it is a module`);
      else m.exports.set(n.name, d);
    }
  }

  // Claims become visible at their position; schemes, parameters and whine
  // codes are visible throughout their scope. A declaration may not reuse a
  // name that is visible from an enclosing scope (§5.1).
  declare(scope, name, decl) {
    const dup = scope.names.get(name);
    if (dup) {
      this.error(decl.namePos, `'${name}' is already declared in this scope (line ${this.line(dup.namePos)})`);
      return;
    }
    for (let s = scope.parent; s; s = s.parent) {
      const d = s.names.get(name);
      if (d && (d.kind !== 'claim' || d.namePos < decl.namePos)) {
        this.error(decl.namePos, `'${name}' shadows '${name}' from an enclosing scope (line ${this.line(d.namePos)})`,
          'Γ-- forbids shadowing; choose another name');
        break;
      }
    }
    scope.names.set(name, decl);
  }

  lookup(scope, name) {
    for (let s = scope; s; s = s.parent) {
      const d = s.names.get(name);
      if (d) return d;
    }
    return null;
  }

  ref(name, pos, scope, ctx) {
    const d = this.lookup(scope, name);
    if (!d) {
      this.error(pos, `unknown name '${name}'`);
      return null;
    }
    // Inside a nested scheme an outer claim may be read before it runs; that is
    // checked at runtime (temporal dead zone).
    if (d.kind === 'claim' && d.fn === ctx.fn && pos < d.visibleAt) {
      this.error(pos, `'${name}' is used before its claim (line ${this.line(d.namePos)})`);
    }
    return d;
  }

  block(b, scope, ctx) {
    for (const s of b.stmts) {
      if (s.k === 'scheme') {
        const d = { kind: 'scheme', node: s, namePos: s.namePos, fn: ctx.fn, src: this.src, usedAt: null, usedSrc: null };
        this.declare(scope, s.name, d);
        if (scope.names.get(s.name) === d) this.schemes.push(d);
      } else if (s.k === 'lineup') {
        this.declare(scope, s.name, { kind: 'lineup', node: s, namePos: s.namePos, fn: ctx.fn });
      } else if (s.k === 'claim') {
        this.declare(scope, s.name, { kind: 'claim', shifty: s.shifty, fickle: s.fickle, namePos: s.namePos, visibleAt: s.end, fn: ctx.fn });
      }
    }
    for (const s of b.stmts) this.stmt(s, scope, ctx);
    if (b.value) this.expr(b.value, scope, ctx);
  }

  stmt(s, scope, ctx) {
    switch (s.k) {
      case 'empty': case 'lineup': case 'steal': case 'bribe': return;
      case 'scheme': {
        const fn = { name: s.name, top: false };
        const ps = new Scope(scope, fn);
        for (const p of s.params) this.declare(ps, p.name, { kind: 'param', shifty: p.shifty, fickle: p.fickle, namePos: p.pos, fn });
        this.block(s.body, new Scope(ps, fn), { fn, loops: 0 });
        return;
      }
      case 'getaway':
        if (ctx.fn.top) this.error(s.pos, "'getaway' outside a scheme");
        if (s.e) this.expr(s.e, scope, ctx);
        break;
      case 'tantrum':
        this.expr(s.e, scope, ctx);
        break;
      case 'bail': case 'loiter':
        if (!ctx.loops) this.error(s.pos, `'${s.k}' outside a loop`);
        return;
      case 'lurk':
        this.expr(s.cond, scope, ctx);
        this.block(s.body, new Scope(scope, ctx.fn), { ...ctx, loops: ctx.loops + 1 });
        return;
      case 'signal':
        if (ctx.fn.top) this.error(s.pos, 'WAH! outside a scheme: there is no caller to signal');
        return;
      case 'exit':
        if (s.e) this.expr(s.e, scope, ctx);
        break;
      case 'expr':
        if (s.e.k === 'sigcall') {
          if (ctx.fn.top) this.error(s.e.pos, 'WAH! outside a scheme: there is no caller to signal');
          this.expr(s.e.arg, scope, ctx);
        } else if (s.e.k === 'chain') {
          this.chain(s.e, scope, ctx, true);
        } else if (s.e.k === 'suspect') {
          this.suspect(s.e, scope, ctx, true);
        } else if (s.e.k === 'call') {
          // The whole statement is a call, so its value is discarded.
          for (const a of s.e.args) this.expr(a, scope, ctx);
          this.callee(s.e, scope, false);
        } else {
          this.expr(s.e, scope, ctx);
        }
        break;
      case 'claim':
        this.expr(s.e, scope, ctx);
        break;
      case 'cheat':
        this.expr(s.e, scope, ctx);
        this.lvalue(s.target, scope, ctx);
        break;
    }
    if (s.whine) this.whine(s, scope, ctx);
  }

  // `whine` handles the outermost call of its statement (§8.2).
  whine(s, scope, ctx) {
    if (!s.e || s.e.k !== 'call') {
      this.error(s.whine.pos, "'whine' handles a call, but this statement's value is not a call",
        'the call must be the outermost expression, e.g. ;s(parse) claim n whine (c) { ... }');
    }
    const hs = new Scope(scope, ctx.fn);
    if (s.whine.param) this.declare(hs, s.whine.param, { kind: 'whine', namePos: s.whine.paramPos, fn: ctx.fn });
    this.block(s.whine.body, new Scope(hs, ctx.fn), ctx);
    if (WHINE_VALUE_USED.has(s.k)) this.requireValue(s.whine.body);
  }

  // A block whose value is used must end with one, unless it never completes (§7.2).
  requireValue(b) {
    if (b.value) return;
    const last = b.stmts.filter((s) => s.k !== 'empty').pop();
    if (last && (NEVER_COMPLETES.has(last.k) || (last.k === 'expr' && last.e.k === 'sigcall'))) return;
    this.error(b.close, "this block's value is used, but it does not produce one",
      'end the block with its value, or with uninvited if that is intended');
  }

  // A multi-way branch (§7.5). The parser has checked that each value is a
  // literal or has the shape member.Lineup or member.Lineup.module; members are
  // resolved here.
  suspect(e, scope, ctx, stmtLevel) {
    this.expr(e.subject, scope, ctx);
    const seen = new Map();
    // The sort of the values: '' for literals, else the lineup as written
    // (Kind, or Kind.module); null until one is known.
    let sort = null;
    let lineup = null; // the sort's declaration, for a lineup
    for (const a of e.alibis) {
      for (const v of a.values) {
        let vs = '';
        if (v.k === 'mem') {
          const d = this.alibiLineup(v, scope);
          vs = d ? lineupText(v.rec) : null;
          if (sort === null) lineup = d;
        }
        if (sort === null) {
          sort = vs;
        } else if (vs !== null && vs !== sort) {
          this.error(v.pos, sort === '' || vs === '' ? 'a suspect cannot mix literals and lineup members'
            : `a suspect cannot mix members of '${sort}' and '${vs}'`);
        }
        const key = alibiKey(v);
        if (seen.has(key)) this.error(v.pos, `duplicate alibi value ${alibiText(v)} (line ${this.line(seen.get(key))})`);
        else seen.set(key, v.pos);
      }
      this.block(a.body, new Scope(scope, ctx.fn), ctx);
      if (!stmtLevel) this.requireValue(a.body);
    }
    if (e.els) {
      this.block(e.els, new Scope(scope, ctx.fn), ctx);
      if (!stmtLevel) this.requireValue(e.els);
    } else if (sort !== null && sort !== '') {
      // Without anyway, a lineup suspect must handle every member.
      const missing = lineup.node.members.filter((m) => !seen.has(`mem:${m.name}.${sort}`));
      if (missing.length) {
        this.error(e.pos, `suspect does not handle ${missing.map((m) => `'${m.name}.${sort}'`).join(', ')}`,
          "add an alibi for each, or end the suspect with 'anyway { ... }'");
      }
    } else if (!stmtLevel) {
      this.error(e.pos, "a suspect used as a value must end with 'anyway { ... }'");
    }
  }

  // The lineup that an alibi value member.Lineup or member.Lineup.module
  // belongs to, or null after an error.
  alibiLineup(v, scope) {
    const d = this.lineupNamed(v.rec, scope);
    if (d) {
      this.member(v, d);
      return d;
    }
    const hint = 'alibi values are literals or lineup members (member.Lineup)';
    const x = v.rec;
    if (x.k === 'name') {
      this.error(x.pos, this.lookup(scope, x.name) ? `'${x.name}' is not a lineup` : `unknown lineup '${x.name}'`, hint);
      return null;
    }
    const m = this.moduleNamed(x.rec, scope);
    if (!m) this.unknownModule(x.rec, scope);
    else if (this.bribed(x, m)) this.error(x.pos, `'${lineupText(x)}' is not a lineup`, hint);
    return null;
  }

  unknownModule(x, scope) {
    this.error(x.pos, this.lookup(scope, x.name) ? `'${x.name}' is not a module` : `unknown module '${x.name}'`);
  }

  // name.module used as a value: a bribed claim (§13.1).
  qualified(e, m) {
    const d = this.bribed(e, m);
    const text = `${e.field}.${e.rec.name}`;
    if (d?.kind === 'scheme') {
      this.error(e.pos, `'${text}' is a scheme; functions are not values in stage 0`, `to call it, write ;args(${text})`);
    } else if (d?.kind === 'lineup') {
      this.error(e.pos, `'${text}' is a lineup, not a value`, `its members are written member.${text}`);
    }
  }

  // member.Name, where Name is a lineup (§4.1).
  member(e, d) {
    const ms = d.node.members;
    if (!ms.some((m) => m.name === e.field)) {
      this.error(e.pos, `no member '${e.field}' in lineup '${d.node.name}'`, `its members are ${ms.map((m) => m.name).join(', ')}`);
    }
  }

  chain(e, scope, ctx, stmtLevel) {
    for (const b of e.branches) {
      this.expr(b.cond, scope, ctx);
      this.block(b.body, new Scope(scope, ctx.fn), ctx);
      if (!stmtLevel) this.requireValue(b.body);
    }
    if (e.els) {
      this.block(e.els, new Scope(scope, ctx.fn), ctx);
      if (!stmtLevel) this.requireValue(e.els);
    } else if (!stmtLevel) {
      this.error(e.pos, "a conditional used as a value must end with a plain 'anyway { ... }'");
    }
  }

  lvalue(t, scope, ctx) {
    if (t.k === 'name') {
      const d = this.ref(t.name, t.pos, scope, ctx);
      if (d) {
        this.mustBeShifty(d, t.name, t.pos, `cannot cheat on '${t.name}': it is not shifty or fickle`, true);
        // The code generator binds the new value by the target's column of the copy table (§10.4).
        t.fickle = !!d.fickle;
      }
      return;
    }
    // The container that directly holds the target must be shifty (§10.9);
    // nested containers are checked at runtime.
    const [box, what] = t.k === 'mem' ? [t.rec, 'a field'] : [t.arr, 'an element'];
    if (t.k === 'idx') this.expr(t.index, scope, ctx);
    if (t.k === 'mem') {
      if (this.lineupNamed(box, scope)) {
        this.error(t.pos, `cannot cheat on '${t.field}.${lineupText(box)}': lineup members are constants`);
        return;
      }
      // Only the module that declares a binding may cheat it (§13.1).
      const m = this.moduleNamed(box, scope);
      if (m) {
        if (this.bribed(t, m)) this.error(t.pos, `cannot cheat on '${t.field}.${box.name}': only '${m.lib.path}' may cheat it`);
        return;
      }
    }
    if (box.k === 'name') {
      const d = this.ref(box.name, box.pos, scope, ctx);
      if (d) this.mustBeShifty(d, box.name, box.pos, `cannot modify ${what} of '${box.name}': it is not shifty`);
    } else {
      this.expr(box, scope, ctx);
    }
  }

  // A cheat's binding must be shifty or fickle. Through a fickle binding, the
  // value's own mutability is checked at runtime (§10.3). rebind: the cheat
  // rebinds the name itself, rather than changing what it holds.
  mustBeShifty(d, name, pos, msg, rebind = false) {
    if (d.kind === 'scheme' || d.kind === 'lineup' || d.kind === 'module') this.error(pos, `'${name}' is a ${d.kind}, not a binding`);
    else if (!d.shifty && !d.fickle) {
      let hint;
      if (d.kind === 'param') {
        hint = rebind ? `mark the parameter (fickle ${name}) to rebind it, or (shifty ${name}) for a mutable copy`
          : `mark the parameter shifty: (shifty ${name})`;
      } else if (d.kind === 'claim') {
        hint = rebind ? `declare it with ';... claim fickle ${name}' to rebind it, or ';... claim shifty ${name}' for a mutable copy`
          : `declare it with ';... claim shifty ${name}'`;
      }
      this.error(pos, msg, hint);
    }
  }

  unknownStd(node) {
    this.error(node.pos, `unknown standard library name '$${node.name}'`,
      "'$' is reserved for the standard library; user names have no sigil");
  }

  expr(e, scope, ctx) {
    switch (e.k) {
      case 'int': case 'float': case 'bool': case 'null': case 'nan': case 'inf':
        return;
      case 'str':
        for (const p of e.parts) if (typeof p !== 'string') this.expr(p, scope, ctx);
        return;
      case 'name': {
        const d = this.ref(e.name, e.pos, scope, ctx);
        if (d && d.kind === 'scheme') {
          this.error(e.pos, `'${e.name}' is a scheme; functions are not values in stage 0`, `to call it, write ;args(${e.name})`);
        } else if (d && d.kind === 'lineup') {
          this.error(e.pos, `'${e.name}' is a lineup, not a value`, `its members are written member.${e.name}`);
        } else if (d && d.kind === 'module') {
          this.error(e.pos, `'${e.name}' is a module, not a value`, `what it bribes is written name.${e.name}`);
        }
        return;
      }
      case 'std': {
        const sig = STDLIB.get(e.name);
        if (sig === undefined) this.unknownStd(e);
        else if (sig !== 'const') this.error(e.pos, `'$${e.name}' is a function; call it with ($${e.name})`);
        return;
      }
      case 'arr':
        for (const i of e.items) this.expr(i, scope, ctx);
        return;
      case 'rec':
        for (const f of e.fields) this.expr(f.e, scope, ctx);
        return;
      case 'idx':
        this.expr(e.index, scope, ctx);
        this.expr(e.arr, scope, ctx);
        return;
      // A field label is not a binding, so only the record side is resolved
      // (§10.8), unless that side names a lineup (§4.1) or a module (§13.1).
      case 'mem': {
        const lineup = this.lineupNamed(e.rec, scope);
        const m = lineup ? null : this.moduleNamed(e.rec, scope);
        if (lineup) this.member(e, lineup);
        else if (m) this.qualified(e, m);
        else this.expr(e.rec, scope, ctx);
        return;
      }
      case 'un':
        this.expr(e.e, scope, ctx);
        return;
      case 'bin':
        this.expr(e.l, scope, ctx);
        this.expr(e.r, scope, ctx);
        return;
      case 'tern':
        this.expr(e.c, scope, ctx);
        this.expr(e.a, scope, ctx);
        this.expr(e.b, scope, ctx);
        return;
      case 'call':
        for (const a of e.args) this.expr(a, scope, ctx);
        this.callee(e, scope);
        return;
      case 'sigcall':
        this.error(e.pos, 'c(WAH!) is only valid as a whole statement: ;c(WAH!)');
        return;
      case 'suspect':
        this.suspect(e, scope, ctx, false);
        return;
      case 'chain':
        this.chain(e, scope, ctx, false);
        return;
    }
    throw new Error(`checker: unhandled node ${e.k}`);
  }

  // used: whether the call's value is used (it is unless the call is a whole statement).
  callee(e, scope, used = true) {
    const c = e.callee;
    const n = e.args.length;
    if (c.k === 'std') {
      const sig = STDLIB.get(c.name);
      if (sig === undefined) return this.unknownStd(c);
      if (sig === 'const') return this.error(c.pos, `'$${c.name}' is a constant, not a function`);
      if (sig !== n) this.error(c.pos, `'$${c.name}' takes ${plural(sig)}, got ${n}`);
      return;
    }
    let d, label;
    if (c.k === 'mem') {
      // (name.module): a scheme a stolen module bribes (§13.1)
      label = `${c.field}.${c.rec.name}`;
      const m = this.moduleNamed(c.rec, scope);
      if (!m) return this.unknownModule(c.rec, scope);
      d = this.bribed(c, m);
      if (!d) return;
    } else {
      label = c.name;
      d = this.lookup(scope, c.name);
      if (!d) return this.error(c.pos, `unknown scheme '${c.name}'`);
    }
    if (d.kind !== 'scheme') {
      return this.error(c.pos, `'${label}' is not a scheme`, 'calls are postfix: arguments come first, e.g. ;x(f)');
    }
    const want = d.node.params.length;
    if (want !== n) this.error(c.pos, `'${label}' takes ${plural(want)}, got ${n}`);
    if (used && d.usedAt === null) {
      d.usedAt = c.pos;
      d.usedSrc = this.src;
    }
  }
}
