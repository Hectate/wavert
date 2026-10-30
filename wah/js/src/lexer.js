// Lexer for Γ-- (spec §2).
//
// Token shape: { t, pos, end, sp, v?, text?, parts? }
//   t     token type: punctuation/operator text, a keyword, '!doubt' | '!lurk' | '!invited',
//         'int' | 'float' | 'sig' | 'str' | 'id' | 'std' | 'opcheat' | 'eof'
//   sp    true when whitespace or a comment precedes the token (used by lints)
//   nl    true when that whitespace or comment contains a newline (used by hints)

import { WahError, err, warn } from './errors.js';

export const KEYWORDS = new Set([
  'scheme', 'getaway', 'claim', 'shifty', 'fickle', 'cheat', 'doubt', 'anyway', 'lurk', 'bail', 'loiter',
  'tantrum', 'whine', 'real', 'fake', 'uninvited', 'nobody', 'everybody', 'suspect', 'alibi',
  'lineup', 'steal', 'bribe',
]);

export const RESERVED = new Map([
  ['blame', "'blame' is reserved: catching tantrums is not designed yet"],
  ['sulk', "'sulk' is reserved: catching tantrums is not designed yet"],
  ['regret', "'regret' is reserved: catching tantrums is not designed yet"],
  ['wails', "'wails' is reserved for after stage 0"],
  ['invited', "'invited' is reserved; did you mean '!invited'?"],
]);

export const BANG = new Set(['doubt', 'lurk', 'invited']);
export const CHEAT_OPS = ['**', '<<', '>>', '++', '--', '+', '-', '*', '/', '%', '&', '|', '^'];
const OPS = [
  '**', '==', '!=', '<=', '>=', '&&', '||', '<<', '>>',
  '+', '-', '*', '/', '%', '<', '>', '&', '|', '^', '~', '!', '?', ':',
  ';', ',', '(', ')', '[', ']', '{', '}',
];

const isIdStart = (c) => c !== undefined && /[A-Za-z_]/.test(c);
const isIdChar = (c) => c !== undefined && /[A-Za-z0-9_]/.test(c);
const isDigit = (c) => c !== undefined && c >= '0' && c <= '9';
const isSpace = (c) => c === ' ' || c === '\t' || c === '\n' || c === '\r';
// A signal token's '!' must be followed by one of these (§2.5).
const isSigTerm = (c) => c === undefined || isSpace(c) || c === ';' || c === '}' || c === ')';

// The literal inside wa[...]h. Returns { kind, v } or an error message.
export function parseBracketLiteral(b) {
  if (/^0x[0-9a-fA-F]+$/.test(b) || /^0b[01]+$/.test(b) || /^[0-9]+$/.test(b)) {
    const v = Number(b);
    if (!Number.isSafeInteger(v)) return 'integer literal exceeds the 53-bit safe range';
    return { kind: 'int', v };
  }
  if (/^[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?$/.test(b)) {
    const v = Number(b);
    if (!Number.isFinite(v)) return 'float literal is out of range';
    return { kind: 'float', v };
  }
  if (/^[0-9]*\.[0-9]*([eE][+-]?[0-9]+)?$/.test(b)) return "digits are required on both sides of '.'";
  return `'${b}' is not a numeric literal`;
}

export function lex(text, warnings = []) {
  const lx = new Lexer(text, warnings);
  const toks = [];
  for (;;) {
    const t = lx.next();
    toks.push(t);
    if (t.t === 'eof') return toks;
  }
}

class Lexer {
  constructor(text, warnings) {
    this.s = text;
    this.i = 0;
    this.warnings = warnings;
    this.prev = null;
  }

  fail(pos, msg, hint) {
    throw new WahError([err(pos, msg, hint)]);
  }

  tok(t, pos, sp, extra) {
    const tk = { t, pos, end: this.i, sp, ...extra };
    this.prev = tk;
    return tk;
  }

  // Skips whitespace and comments; returns true if anything was skipped.
  skip() {
    const s = this.s;
    let skipped = false;
    for (;;) {
      const c = s[this.i];
      if (isSpace(c)) { this.i++; skipped = true; continue; }
      if (c === '/' && s[this.i + 1] === '/') {
        if (!skipped && isIdChar(s[this.i - 1]) && isIdChar(s[this.i + 2])) {
          this.warnings.push(warn(this.i, "'//' starts a comment; Γ-- has no integer-division operator",
            'for integer division write ;a / b($floor)'));
        }
        while (this.i < s.length && s[this.i] !== '\n') this.i++;
        skipped = true;
        continue;
      }
      if (c === '/' && s[this.i + 1] === '*') {
        const close = s.indexOf('*/', this.i + 2);
        if (close < 0) this.fail(this.i, 'unterminated block comment');
        this.i = close + 2;
        skipped = true;
        continue;
      }
      return skipped;
    }
  }

  next() {
    const from = this.i;
    const sp = this.skip() || this.i === 0;
    const nl = this.s.slice(from, this.i).includes('\n');
    const t = this.token(sp);
    t.nl = nl;
    return t;
  }

  token(sp) {
    const s = this.s, start = this.i, c = s[start];
    if (c === undefined) return this.tok('eof', start, sp);
    if (isIdStart(c)) return this.word(start, sp);
    if (c === '"') return this.string(start, sp);
    if (c === '$') return this.sigil(start, sp);
    if (isDigit(c)) {
      const m = /^[0-9][0-9A-Za-z_.]*/.exec(s.slice(start, start + 40));
      this.fail(start, `'${m[0]}': decimal digits are not Γ-- numerals`, `write wa[${m[0]}]h`);
    }
    for (const op of CHEAT_OPS) {
      if (s.startsWith(op, start) && s.startsWith('cheat', start + op.length) && !isIdChar(s[start + op.length + 5])) {
        this.i = start + op.length + 5;
        return this.tok('opcheat', start, sp, { v: op });
      }
    }
    if (c === '!') {
      const m = /^!(\s*)([A-Za-z_][A-Za-z0-9_]*)/.exec(s.slice(start, start + 64));
      if (m && BANG.has(m[2])) {
        if (m[1]) this.fail(start, `no whitespace is allowed inside '!${m[2]}'`, `write '!${m[2]}'`);
        this.i = start + m[0].length;
        return this.tok('!' + m[2], start, sp);
      }
    }
    if (s.startsWith('>>>', start)) this.fail(start, "there is no '>>>' operator", 'for an unsigned interpretation use ;x($toUint32)');
    if (s.startsWith('--', start)) this.fail(start, "'--' is not an operator", "write '- -x' or '-(-x)' for double negation, or ';--cheat x' to decrement");
    if (s.startsWith('++', start)) this.fail(start, "'++' is not an operator", "write ';++cheat x' to increment");
    for (const op of OPS) {
      if (s.startsWith(op, start)) {
        this.i = start + op.length;
        if (op === ';' && this.prev && this.prev.t === ';') {
          this.warnings.push(warn(start, "';;': likely a habitual trailing ';' from C-style languages",
            'Γ-- statements begin with ;, they are not terminated by it'));
        }
        return this.tok(op, start, sp);
      }
    }
    if (c === '=') this.fail(start, "'=' is not an operator", "use 'claim' or 'cheat'");
    // Member access (§10.8): '.' must touch both sides.
    if (c === '.') {
      const after = s[start + 1];
      if (sp || after === undefined || isSpace(after) || s.startsWith('//', start + 1) || s.startsWith('/*', start + 1)) {
        this.fail(start, "no whitespace is allowed around '.'", 'member access is written field.record, e.g. kind.node');
      }
      this.i = start + 1;
      return this.tok('.', start, sp);
    }
    this.fail(start, `unexpected character '${c}'`);
  }

  word(start, sp) {
    const s = this.s;
    let j = start;
    while (isIdChar(s[j])) j++;
    const w = s.slice(start, j);
    if (s[j] === '[' && /^[wW][aA]$/.test(w)) return this.bracketNumeral(start, j, sp);
    if (/^[wW][aA]*[hH]$/.test(w)) {
      const upper = w === w.toUpperCase();
      if (!upper && w !== w.toLowerCase()) {
        this.fail(start, `mixed-case wah numeral '${w}'`, 'all lowercase is positive, all uppercase is negative');
      }
      this.i = j;
      return this.numeral(start, sp, upper, w.length - 2, 'int', w);
    }
    this.i = j;
    if (s[j] === '$') this.fail(j, "'$' cannot appear in identifiers", "'$' is reserved for the standard library; user names have no sigil");
    if (KEYWORDS.has(w)) return this.tok(w, start, sp);
    if (RESERVED.has(w)) this.fail(start, RESERVED.get(w));
    return this.tok('id', start, sp, { v: w });
  }

  bracketNumeral(start, open, sp) {
    const s = this.s;
    const pre = s.slice(start, open);
    if (pre !== 'wa' && pre !== 'WA') {
      this.fail(start, `mixed-case wah numeral '${pre}[...'`, 'all lowercase is positive, all uppercase is negative');
    }
    const upper = pre === 'WA';
    const close = s.indexOf(']', open);
    if (close < 0) this.fail(start, "unterminated wah numeral: missing ']'");
    const h = upper ? 'H' : 'h';
    if (s[close + 1] !== h) this.fail(close + 1, `expected '${h}' to close the wah numeral`);
    this.i = close + 2;
    if (isIdChar(s[this.i])) this.fail(start, 'invalid wah numeral');
    const lit = parseBracketLiteral(s.slice(open + 1, close));
    if (typeof lit === 'string') {
      this.fail(open + 1, lit, 'only literals are allowed inside wa[...]h; for computed values use ;x($toInt)');
    }
    return this.numeral(start, sp, upper, lit.v, lit.kind, s.slice(start, this.i));
  }

  numeral(start, sp, upper, n, kind, text) {
    const s = this.s;
    if (s[this.i] === '!' && isSigTerm(s[this.i + 1])) {
      if (!upper) this.fail(start, `lowercase '${text}!' is not a signal`, `write '${text.toUpperCase()}!'`);
      if (kind !== 'int' || n < 1) this.fail(start, 'a WAH! signal code must be an integer >= 1');
      this.i++;
      return this.tok('sig', start, sp, { v: n, text: text + '!' });
    }
    if (s[this.i] === '$') this.fail(this.i, "'$' must be immediately followed by a standard library name");
    const v = upper ? -n : n;
    return this.tok(kind, start, sp, { v: kind === 'int' ? v + 0 : v, text });
  }

  sigil(start, sp) {
    const s = this.s;
    if (!isIdStart(s[start + 1])) {
      this.fail(start, "'$' must be immediately followed by a standard library name",
        "'$' is reserved for the standard library; user names have no sigil");
    }
    let j = start + 1;
    while (isIdChar(s[j])) j++;
    this.i = j;
    return this.tok('std', start, sp, { v: s.slice(start + 1, j) });
  }

  string(start, sp) {
    const s = this.s;
    const parts = [];
    let i = start + 1, buf = '', rawDollar = -1;
    for (;;) {
      const c = s[i];
      if (c === undefined) this.fail(start, 'unterminated string');
      if (c === '\n' || c === '\r') this.fail(i, 'raw newline inside a string', "write '\\n'");
      if (c === '"') { i++; break; }
      if (c === '\\') {
        const e = s[i + 1];
        const simple = { n: '\n', r: '\r', t: '\t', '\\': '\\', '"': '"', $: '$' }[e];
        if (simple !== undefined) { buf += simple; i += 2; continue; }
        if (e === 'u' && s[i + 2] === '{') {
          const close = s.indexOf('}', i + 3);
          const hex = close < 0 ? '' : s.slice(i + 3, close);
          const cp = parseInt(hex, 16);
          if (!/^[0-9a-fA-F]{1,6}$/.test(hex) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            this.fail(i, 'invalid \\u{...} escape', 'write 1 to 6 hex digits naming a Unicode scalar value, e.g. \\u{1F608}');
          }
          buf += String.fromCodePoint(cp);
          i = close + 1;
          continue;
        }
        this.fail(i, `unknown escape '\\${e ?? ''}'`, 'valid escapes are \\n \\r \\t \\\\ \\" \\$ \\u{...}');
      }
      if (c === '{') {
        if (s[i + 1] === '{') { buf += '{'; i += 2; continue; }
        if (rawDollar === i - 1) this.warnings.push(warn(rawDollar, 'Γ-- interpolation is {x}, not ${x}'));
        parts.push(buf);
        buf = '';
        const sub = this.interpolation(i);
        parts.push({ tokens: sub.tokens, pos: i });
        i = sub.end;
        continue;
      }
      if (c === '}') {
        if (s[i + 1] === '}') { buf += '}'; i += 2; continue; }
        this.fail(i, "unmatched '}' in string", "write '}}' for a literal brace");
      }
      if (c === '$') {
        rawDollar = i;
        if (isIdStart(s[i + 1])) {
          let j = i + 1;
          while (isIdChar(s[j])) j++;
          this.warnings.push(warn(i, `did you mean {${s.slice(i + 1, j)}}?`, 'Γ-- interpolation is {x}; write \\$ for a literal $'));
        }
      }
      buf += c;
      i++;
    }
    parts.push(buf);
    this.i = i;
    return this.tok('str', start, sp, { parts });
  }

  // Lexes the expression inside "{...}" of a string; `open` is the '{'.
  interpolation(open) {
    const saveI = this.i, savePrev = this.prev;
    this.i = open + 1;
    this.prev = null;
    const toks = [];
    let depth = 0;
    for (;;) {
      const t = this.next();
      if (t.t === 'eof') this.fail(open, 'unterminated interpolation in string');
      if (t.t === '}') {
        if (depth === 0) {
          toks.push({ t: 'eof', pos: t.pos, end: t.end, sp: t.sp, nl: t.nl });
          break;
        }
        depth--;
      } else if (t.t === '{') {
        depth++;
      }
      toks.push(t);
    }
    const end = this.i;
    this.i = saveI;
    this.prev = savePrev;
    if (toks.length === 1) this.fail(open, 'empty interpolation', "write '{{' for a literal brace");
    return { tokens: toks, end };
  }
}
