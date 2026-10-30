// Parser for Γ-- (spec §5-§8, §11.4).
//
// Statements begin with ';'. Expressions use precedence climbing; the comma
// list and the postfix call `(f)` sit below the ternary, and a call consumes
// every comma-separated expression to its left at the current nesting level.
//
// Expression nodes (k): int float str bool null nan inf name std arr rec idx mem
//   un bin tern call sigcall chain suspect. A parenthesized node gets `paren: true`.
// Statement nodes (k): empty scheme lineup steal bribe getaway tantrum bail loiter
//   lurk signal exit expr claim cheat. Any of expr/claim/cheat/getaway/tantrum/exit may carry `whine`.

import { WahError, err, warn } from './errors.js';
import { KEYWORDS } from './lexer.js';

const PREC = {
  '||': 1, '&&': 2, '==': 3, '!=': 3, '<': 4, '>': 4, '<=': 4, '>=': 4,
  '|': 5, '^': 6, '&': 7, '<<': 8, '>>': 8, '+': 9, '-': 9, '*': 10, '/': 10, '%': 10,
};

const EXPR_START = new Set([
  'int', 'float', 'str', 'real', 'fake', 'uninvited', 'nobody', 'everybody', 'id', 'std',
  '(', '[', '{', 'doubt', '!doubt', 'suspect', '-', '!', '~', ',',
]);
const IDENT = /^[A-Za-z_][A-Za-z0-9_]*$/;

const WHINE_OK = new Set(['expr', 'claim', 'cheat', 'getaway', 'tantrum', 'exit']);
const CALLEE = new Set(['id', 'std', 'sig']);
const POSTFIX_HINT = 'calls are postfix (arguments first, then one function name): ;x, y(f)';

export function describe(t) {
  switch (t.t) {
    case 'eof': return 'end of input';
    case 'id': return `'${t.v}'`;
    case 'std': return `'$${t.v}'`;
    case 'int': case 'float': case 'sig': return `'${t.text}'`;
    case 'str': return 'a string';
    case 'opcheat': return `'${t.v}cheat'`;
    default: return `'${t.t}'`;
  }
}

// Does evaluating e call anything? An expression statement that does not has no effect (§2.8).
// Conditional chains count as having an effect.
function hasEffect(e) {
  switch (e.k) {
    case 'call': case 'sigcall': case 'chain': case 'suspect': return true;
    case 'str': return e.parts.some((p) => typeof p !== 'string' && hasEffect(p));
    case 'arr': return e.items.some(hasEffect);
    case 'rec': return e.fields.some((f) => hasEffect(f.e));
    case 'idx': return hasEffect(e.index) || hasEffect(e.arr);
    case 'mem': return hasEffect(e.rec);
    case 'un': return hasEffect(e.e);
    case 'bin': return hasEffect(e.l) || hasEffect(e.r);
    case 'tern': return hasEffect(e.c) || hasEffect(e.a) || hasEffect(e.b);
    default: return false;
  }
}

export function parse(tokens, warnings = []) {
  return new Parser(tokens, warnings).program();
}

class Parser {
  constructor(toks, warnings) {
    this.toks = toks;
    this.p = 0;
    this.warnings = warnings;
    this.depth = 0; // blocks entered; hints about block values apply only inside one
  }

  peek() { return this.toks[this.p]; }
  at(t) { return this.toks[this.p].t === t; }
  // A call is exactly `(name)` or `(name.module)` after an expression. Any other
  // '(' ends the expression, so a block value like `(a + b) / c` may follow a statement.
  atCall() {
    const [open, name, dot, mod, close] = this.toks.slice(this.p, this.p + 5);
    if (open.t !== '(' || name === undefined) return false;
    if (CALLEE.has(name.t) && dot !== undefined && dot.t === ')') return true;
    return name.t === 'id' && dot?.t === '.' && mod?.t === 'id' && close?.t === ')';
  }
  next() {
    const t = this.toks[this.p];
    if (t.t !== 'eof') this.p++;
    return t;
  }
  accept(t) { return this.at(t) ? this.next() : null; }
  expect(t, msg, hint) {
    if (this.at(t)) return this.next();
    this.fail(this.peek(), `${msg ?? `expected '${t}'`}, found ${describe(this.peek())}`, hint);
  }
  fail(tok, msg, hint) { throw new WahError([err(tok.pos, msg, hint)]); }
  lintSpace(op) {
    if (!op.sp || !this.peek().sp) this.warnings.push(warn(op.pos, `missing whitespace around '${op.t}'`));
  }

  // ---- statements ----

  program() {
    const stmts = [];
    let started = false; // has a statement other than steal or empty been seen?
    while (!this.at('eof')) {
      if (!this.at(';')) {
        this.fail(this.peek(), `expected ';' to start a statement, found ${describe(this.peek())}`,
          this.at('(') ? POSTFIX_HINT : 'every Γ-- statement begins with ;');
      }
      const s = this.statement();
      if (s.k === 'steal' && started) this.fail(s, "'steal' must come before every other statement");
      if (s.k !== 'steal' && s.k !== 'empty') started = true;
      stmts.push(s);
    }
    return { stmts, end: this.peek().pos };
  }

  statement() {
    const semi = this.next();
    const t = this.peek();
    let s;
    switch (t.t) {
      case ';': case '}': case 'eof':
        return { k: 'empty', pos: semi.pos, end: t.pos };
      case 'scheme': s = this.scheme(); break;
      case 'lineup': s = this.lineup(); break;
      case 'getaway':
        this.next();
        s = { k: 'getaway', e: EXPR_START.has(this.peek().t) ? this.expr() : null };
        break;
      case 'tantrum': this.next(); s = { k: 'tantrum', e: this.expr() }; break;
      case 'bail': case 'loiter': this.next(); s = { k: t.t }; break;
      case 'lurk': case '!lurk':
        this.next();
        s = { k: 'lurk', neg: t.t === '!lurk', cond: this.cond(t.t), body: this.block() };
        break;
      case 'sig': this.next(); s = { k: 'signal', code: t.v }; break;
      case '!invited': this.next(); s = { k: 'exit', e: null }; break;
      case 'opcheat':
        if (t.v === '++' || t.v === '--') {
          this.next();
          s = { k: 'cheat', op: t.v[0], e: { k: 'int', v: 1, pos: t.pos }, target: this.lvalue() };
          break;
        }
        this.fail(t, `'${t.v}cheat' needs a value before it`, `;value ${t.v}cheat name`);
        break;
      case 'whine': this.fail(t, "'whine' must follow a statement whose value comes from a call");
      // falls through (fail throws)
      case 'anyway': this.fail(t, "'anyway' without a preceding doubt or !doubt");
      // falls through (fail throws)
      case 'bribe': this.fail(t, "'bribe' needs the names it exports before it", ';a, b bribe');
      // falls through (fail throws)
      default:
        if (this.atBribe()) {
          s = this.bribe();
          break;
        }
        s = this.exprStatement();
        if (s.k === 'expr' && !hasEffect(s.e)) {
          this.warnings.push(warn(t.pos, 'this statement has no effect',
            this.depth > 0 ? "to make it the block's value, remove the ';' before it" : undefined));
        }
    }
    s.pos = t.pos;
    if (this.at('whine')) {
      const w = this.next();
      if (!WHINE_OK.has(s.k)) this.fail(w, "'whine' must follow a statement whose value comes from a call");
      let param = null;
      if (this.accept('(')) {
        param = this.expect('id', 'expected a name for the signal code');
        this.expect(')');
      }
      s.whine = { pos: w.pos, param: param && param.v, paramPos: param && param.pos, body: this.block() };
    }
    s.end = this.peek().pos;
    return s;
  }

  exprStatement() {
    const e = this.expr();
    const t = this.peek();
    switch (t.t) {
      case 'claim': {
        this.next();
        // At most one modifier: shifty or fickle (§5).
        const shifty = !!this.accept('shifty');
        const fickle = !shifty && !!this.accept('fickle');
        const name = this.expect('id', "expected a name after 'claim'");
        return { k: 'claim', e, shifty, fickle, name: name.v, namePos: name.pos };
      }
      case 'cheat':
        this.next();
        return { k: 'cheat', e, op: null, target: this.lvalue() };
      case 'opcheat':
        if (t.v === '++' || t.v === '--') this.fail(t, `'${t.v}cheat' takes no value`, `write ;${t.v}cheat name`);
        this.next();
        return { k: 'cheat', e, op: t.v, target: this.lvalue() };
      case '!invited':
        this.next();
        return { k: 'exit', e };
      case 'shifty': case 'fickle':
        this.fail(t, `'${t.t}' goes after 'claim'`, `;value claim ${t.t} name`);
      // falls through (fail throws)
      case 'steal':
        this.next();
        return this.steal(t, e);
      case 'bribe':
        this.fail(t, 'only names can be bribed', ';a, b bribe');
      // falls through (fail throws)
      default:
        return { k: 'expr', e };
    }
  }

  // ;"path" steal name (§13.1). The path is a plain string literal; name is the module's name here.
  steal(kw, e) {
    if (this.depth > 0) this.fail(kw, "'steal' is only allowed at the top level of a file");
    if (e.k !== 'str' || e.paren || e.parts.length > 1 || (e.parts.length === 1 && typeof e.parts[0] !== 'string')) {
      this.fail(e, 'a steal path must be a plain string literal', ';"lib.wah" steal');
    }
    const path = e.parts[0] ?? '';
    if (path.includes('\\')) this.fail(e, "steal paths use '/' as the separator");
    if (!path.endsWith('.wah')) this.fail(e, 'a steal path includes the .wah extension');
    const name = this.expect('id', "expected a name for the module after 'steal'", ';"lib.wah" steal lib');
    return { k: 'steal', path, pathPos: e.pos, name: name.v, namePos: name.pos };
  }

  // Is this statement `name, name, ... bribe`?
  atBribe() {
    for (let i = this.p; ; i += 2) {
      if (this.toks[i].t !== 'id') return false;
      if (this.toks[i + 1].t === 'bribe') return true;
      if (this.toks[i + 1].t !== ',') return false;
    }
  }

  // ;name, name, ... bribe (§13.1), when atBribe() holds.
  bribe() {
    const names = [];
    for (;;) {
      const n = this.next();
      if (names.some((x) => x.name === n.v)) this.fail(n, `'${n.v}' is already bribed`);
      names.push({ name: n.v, pos: n.pos });
      if (!this.accept(',')) break;
    }
    const kw = this.next();
    if (this.depth > 0) this.fail(kw, "'bribe' is only allowed at the top level of a library");
    return { k: 'bribe', names };
  }

  scheme() {
    this.next();
    if (this.at('id')) this.fail(this.peek(), 'parameters come before the scheme name', ';scheme (a, b)name { ... }');
    this.expect('(', "expected '(' to open the parameter list");
    const params = [];
    if (!this.at(')')) {
      for (;;) {
        const shifty = !!this.accept('shifty');
        const fickle = !shifty && !!this.accept('fickle');
        const n = this.expect('id', 'expected a parameter name');
        params.push({ name: n.v, shifty, fickle, pos: n.pos });
        if (!this.accept(',')) break;
      }
    }
    this.expect(')', "expected ')' to close the parameter list");
    const name = this.expect('id', 'expected the scheme name after its parameters', ';scheme (a, b)name { ... }');
    return { k: 'scheme', name: name.v, namePos: name.pos, params, body: this.block() };
  }

  // ;lineup (member, ...)Name (§4.1), only at the top level.
  lineup() {
    const kw = this.next();
    if (this.depth > 0) this.fail(kw, 'a lineup can only be declared at the top level');
    if (this.at('id')) this.fail(this.peek(), 'members come before the lineup name', ';lineup (a, b)Name');
    this.expect('(', "expected '(' to open the member list");
    if (this.at(')')) this.fail(this.peek(), 'a lineup needs at least one member', ';lineup (a, b)Name');
    const members = [];
    for (;;) {
      const m = this.peek();
      if (m.t !== 'id') {
        this.fail(m, KEYWORDS.has(m.t) ? `'${m.t}' is a keyword and cannot be a member name`
          : `expected a member name, found ${describe(m)}`);
      }
      this.next();
      if (members.some((x) => x.name === m.v)) this.fail(m, `duplicate member '${m.v}' in lineup`);
      members.push({ name: m.v, pos: m.pos });
      if (!this.accept(',')) break;
    }
    this.expect(')', "expected ',' or ')' in the member list");
    const name = this.expect('id', 'expected the lineup name after its members', ';lineup (a, b)Name');
    return { k: 'lineup', name: name.v, namePos: name.pos, members };
  }

  cond(kw) {
    this.expect('(', `expected '(' after '${kw}'`, `conditions are parenthesized: ${kw} (x) { ... }`);
    const e = this.expr();
    this.expect(')', "expected ')' to close the condition");
    return e;
  }

  block() {
    this.expect('{', "expected '{'");
    this.depth++;
    const stmts = [];
    let value = null;
    for (;;) {
      if (this.at(';')) { stmts.push(this.statement()); continue; }
      if (this.at('}')) break;
      if (this.at('eof')) this.fail(this.peek(), "unterminated block: expected '}'");
      value = this.expr();
      if (!this.at('}')) {
        const hint = this.at('(') ? POSTFIX_HINT
          : this.at(':') ? "a record as a block's value needs its own braces: { { key: value } }"
            : "an expression without a leading ';' is the block's value and must come last";
        this.fail(this.peek(), `expected '}' after the block's value, found ${describe(this.peek())}`, hint);
      }
      break;
    }
    const close = this.next();
    this.depth--;
    return { stmts, value, close: close.pos };
  }

  lvalue() {
    const t = this.peek();
    const e = this.postfix();
    if (e.k !== 'name' && e.k !== 'idx' && e.k !== 'mem') this.fail(t, 'expected a name, an indexed element or a field to cheat on');
    return e;
  }

  // ---- expressions ----

  expr() { return this.seq(false); }

  // A comma-separated sequence at one nesting level. Each `(f)` consumes every
  // pending expression to its left. In list mode (array literals) the leftovers
  // are the items; otherwise exactly one must remain.
  seq(list) {
    const pending = [];
    let firstComma = null;
    let cur = this.at(',') ? this.zeroCall() : this.ternary();
    for (;;) {
      if (this.atCall()) {
        pending.push(cur);
        cur = this.continueOps(this.call(pending.splice(0)));
        firstComma = null;
        continue;
      }
      if (this.at(',')) {
        const comma = this.next();
        firstComma ??= comma;
        pending.push(cur);
        if (this.at(',')) {
          this.fail(this.peek(), 'a zero-argument call inside an argument list must be parenthesized',
            'write (,(f)) so the call does not capture its neighbours');
        }
        if (list && this.at(']')) this.fail(comma, 'trailing comma in array literal');
        cur = this.ternary();
        continue;
      }
      break;
    }
    pending.push(cur);
    if (list) return pending;
    if (pending.length > 1) {
      if (this.at('bribe')) this.fail(this.peek(), 'only names can be bribed', ';a, b bribe');
      // A block's value on a new line that starts with ',' continues the statement before it (§7.2).
      const hint = firstComma.nl && this.depth > 0
        ? "a block's value that starts with ',' continues the statement before it; wrap the value in parentheses: (,(f))"
        : ';a, b(f) calls f with a and b';
      this.fail(firstComma, 'comma-separated values must be consumed by a call', hint);
    }
    return cur;
  }

  zeroCall() {
    const comma = this.next();
    if (!this.atCall()) this.fail(comma, "a leading ',' must be followed by a call", 'zero-argument calls are written ,(f)');
    return this.continueOps(this.call([]));
  }

  // Called only when atCall() holds: '(' name ')' or '(' name '.' module ')'.
  // A scheme stolen from a module is called as (name.module) (§13.1).
  call(args) {
    this.next();
    const c = this.next();
    if (this.accept('.')) {
      const m = this.next();
      this.next();
      return { k: 'call', callee: { k: 'mem', field: c.v, rec: { k: 'name', name: m.v, pos: m.pos }, pos: c.pos }, args, pos: c.pos };
    }
    this.next();
    if (c.t !== 'sig') {
      return { k: 'call', callee: { k: c.t === 'id' ? 'name' : 'std', name: c.v, pos: c.pos }, args, pos: c.pos };
    }
    if (c.text !== 'WAH!') {
      this.fail(c, `only the exact spelling WAH! is allowed in call position, not ${c.text}`,
        ';c(WAH!) sends code c; any other spelling would make the code ambiguous');
    }
    if (args.length !== 1) this.fail(c, 'WAH! in call position takes exactly one argument: the code');
    return { k: 'sigcall', arg: args[0], pos: c.pos };
  }

  // After a call, operators may continue with the call's result as left operand.
  continueOps(left) {
    const t = this.peek().t;
    if (t === '**' || t === '?' || PREC[t] !== undefined) {
      return this.ternaryTail(this.binaryFrom(this.powerTail(left), 1));
    }
    return left;
  }

  ternary() { return this.ternaryTail(this.binary(1)); }

  ternaryTail(c) {
    if (!this.at('?')) return c;
    const q = this.next();
    this.lintSpace(q);
    const a = this.ternary();
    const colon = this.expect(':', "expected ':' in conditional expression");
    this.lintSpace(colon);
    const b = this.ternary();
    return { k: 'tern', c, a, b, pos: q.pos };
  }

  binary(min) { return this.binaryFrom(this.unary(), min); }

  binaryFrom(left, min) {
    for (;;) {
      const op = this.peek();
      const p = PREC[op.t];
      if (p === undefined || p < min) return left;
      this.next();
      this.lintSpace(op);
      const right = this.binary(p + 1);
      left = { k: 'bin', op: op.t, l: left, r: right, pos: op.pos };
    }
  }

  unary() {
    const t = this.peek();
    if (t.t === '-' || t.t === '!' || t.t === '~') {
      this.next();
      const e = this.unary();
      if (t.t === '-' && e.k === 'bin' && e.op === '**' && !e.paren) {
        this.fail(t, "'-x ** y' is ambiguous", 'write -(x ** y) or (-x) ** y');
      }
      return { k: 'un', op: t.t, e, pos: t.pos };
    }
    return this.powerTail(this.postfix());
  }

  powerTail(base) {
    if (!this.at('**')) return base;
    const op = this.next();
    this.lintSpace(op);
    return { k: 'bin', op: '**', l: base, r: this.unary(), pos: op.pos };
  }

  // Indexing and member access are inverted, with the container on the right:
  // i[arr], field.rec. Member access takes a whole postfix operand on its right,
  // so `kind.i[items]` is items[i].kind (§10.8).
  postfix() {
    const t = this.peek();
    if (this.toks[this.p + 1]?.t === '.') {
      if (t.t !== 'id') {
        this.fail(t, KEYWORDS.has(t.t) ? `'${t.t}' is a keyword and cannot be a field name`
          : `expected a field name before '.', found ${describe(t)}`, 'member access is written field.record, e.g. kind.node');
      }
      this.next();
      this.next();
      return { k: 'mem', field: t.v, rec: this.postfix(), pos: t.pos };
    }
    let e = this.primary();
    while (this.at('[')) {
      const open = this.next();
      if (open.nl && this.depth > 0) {
        this.warnings.push(warn(open.pos, "'[' at the start of a line indexes the expression before it",
          "if it starts the block's value, wrap the value in parentheses: ([a, b])"));
      }
      const arr = this.expr();
      this.expect(']', "expected ']' to close the index");
      if (e.k === 'str' && !e.paren && e.parts.length === 1 && IDENT.test(e.parts[0]) && !KEYWORDS.has(e.parts[0])) {
        this.warnings.push(warn(e.pos, `string-literal field key: write ${e.parts[0]}.record instead of "${e.parts[0]}"[record]`));
      }
      e = { k: 'idx', index: e, arr, pos: open.pos };
    }
    if (this.at('.')) {
      this.fail(this.peek(), "the left side of '.' must be a field name", 'member access is written field.record; for a computed key write key[record]');
    }
    return e;
  }

  primary() {
    const t = this.peek();
    const pos = t.pos;
    switch (t.t) {
      case 'doubt': case '!doubt': return this.chain();
      case 'suspect': return this.suspect();
    }
    this.next();
    switch (t.t) {
      case 'int': return { k: 'int', v: t.v, pos };
      case 'float': return { k: 'float', v: t.v, pos };
      case 'str': return { k: 'str', parts: t.parts.map((p) => (typeof p === 'string' ? p : this.interp(p))), pos };
      case 'real': return { k: 'bool', v: true, pos };
      case 'fake': return { k: 'bool', v: false, pos };
      case 'uninvited': return { k: 'null', pos };
      case 'nobody': return { k: 'nan', pos };
      case 'everybody': return { k: 'inf', pos };
      case 'id': return { k: 'name', name: t.v, pos };
      case 'std': return { k: 'std', name: t.v, pos };
      case '(': {
        if (this.at(')')) this.fail(t, 'empty parentheses', 'zero-argument calls are written ,(f)');
        const e = this.expr();
        this.expect(')', "expected ')'");
        e.paren = true;
        return e;
      }
      case '[': {
        const items = this.at(']') ? [] : this.seq(true);
        this.expect(']', "expected ']' to close the array");
        return { k: 'arr', items, pos };
      }
      case 'sig':
        this.fail(t, `${t.text} is a statement`, 'write ;WAH! to signal, or ;c(WAH!) to forward a code');
      // falls through (fail throws)
      case 'claim': case 'cheat': case 'shifty': case 'fickle':
        this.fail(t, `unexpected '${t.t}'`, 'Γ-- puts the value first: ;value claim name');
      // falls through (fail throws)
      case '{': return this.record(t);
      default:
        this.fail(t, `expected an expression, found ${describe(t)}`);
    }
  }

  // Record literal (§10.7); the '{' is already consumed.
  record(open) {
    const fields = [];
    const seen = new Set();
    if (!this.at('}')) {
      for (;;) {
        const k = this.peek();
        if (k.t !== 'id') {
          this.fail(k, KEYWORDS.has(k.t) ? `'${k.t}' is a keyword and cannot be a field name`
            : `expected a field name, found ${describe(k)}`, 'record literals are written { key: value, ... }');
        }
        this.next();
        if (seen.has(k.v)) this.fail(k, `duplicate field '${k.v}' in record literal`);
        seen.add(k.v);
        if (!this.at(':')) {
          this.fail(this.peek(), `expected ':' after field name '${k.v}', found ${describe(this.peek())}`,
            this.at(',') || this.at('}') ? `there is no shorthand; write ${k.v}: ${k.v}` : undefined);
        }
        this.next();
        fields.push({ name: k.v, e: this.fieldValue(), pos: k.pos });
        if (!this.at(',')) break;
        const comma = this.next();
        if (this.at('}')) this.fail(comma, 'trailing comma in record literal');
      }
    }
    this.expect('}', "expected ',' or '}' in record literal");
    return { k: 'rec', fields, pos: open.pos };
  }

  // A field value is its own nesting level for the call rule: ',' separates
  // fields, so a call captures only its own field's value.
  fieldValue() {
    let cur = this.at(',') ? this.zeroCall() : this.ternary();
    while (this.atCall()) cur = this.continueOps(this.call([cur]));
    return cur;
  }

  // suspect (subject) { alibi v, v { ... } ... anyway { ... } } (§7.5).
  suspect() {
    const kw = this.next();
    const subject = this.cond('suspect');
    this.expect('{', "expected '{' after the suspect's subject");
    const alibis = [];
    let els = null;
    for (;;) {
      if (this.at('alibi')) {
        const a = this.next();
        if (els) this.fail(a, "'anyway' must come last in a suspect");
        if (this.at('{')) this.fail(this.peek(), 'an alibi needs at least one value', 'alibi "a", "b" { ... }');
        const values = [this.alibiValue()];
        while (this.accept(',')) values.push(this.alibiValue());
        alibis.push({ values, body: this.block(), pos: a.pos });
      } else if (this.at('anyway')) {
        const a = this.next();
        if (els) this.fail(a, "a suspect has only one 'anyway'");
        if (alibis.length === 0) this.fail(a, "a suspect needs at least one alibi before 'anyway'");
        els = this.block();
      } else {
        break;
      }
    }
    if (alibis.length === 0) this.fail(this.peek(), 'a suspect needs at least one alibi', 'suspect (x) { alibi "a" { ... } }');
    this.expect('}', "expected 'alibi', 'anyway' or '}' in a suspect");
    return { k: 'suspect', subject, alibis, els, pos: kw.pos };
  }

  // An alibi value: a literal, or member.Name, which the checker resolves (§7.5).
  alibiValue() {
    const t = this.peek();
    const v = this.postfix();
    switch (v.k) {
      case 'int': case 'bool': case 'null':
        return v;
      // member.Lineup, or member.Lineup.module for a stolen lineup
      case 'mem':
        if (v.rec.k === 'name' || (v.rec.k === 'mem' && v.rec.rec.k === 'name')) return v;
        break;
      case 'str':
        if (v.parts.length === 1 && typeof v.parts[0] === 'string') return v;
        return this.fail(t, 'an alibi value cannot be interpolated');
      case 'float': case 'nan': case 'inf':
        return this.fail(t, 'floats cannot be alibi values', 'float equality makes a poor match; compare with !doubt instead');
    }
    return this.fail(t, 'an alibi value must be a literal or a lineup member',
      'alibi values are integers, purples, real, fake, uninvited or member.Lineup');
  }

  interp(part) {
    const sub = new Parser(part.tokens, this.warnings);
    const e = sub.expr();
    if (!sub.at('eof')) sub.fail(sub.peek(), `unexpected ${describe(sub.peek())} in interpolation`);
    return e;
  }

  chain() {
    const branches = [];
    let els = null;
    for (;;) {
      const kw = this.next();
      const cond = this.cond(kw.t);
      if (kw.t === 'doubt' && cond.k === 'bin' && cond.op === '!=') {
        this.warnings.push(warn(kw.pos, "'doubt (a != b)' is a double negative", "write '!doubt (a == b)'"));
      }
      branches.push({ neg: kw.t === 'doubt', cond, body: this.block(), pos: kw.pos });
      if (!this.accept('anyway')) break;
      if (this.at('doubt') || this.at('!doubt')) continue;
      els = this.block();
      break;
    }
    return { k: 'chain', branches, els, pos: branches[0].pos };
  }
}
