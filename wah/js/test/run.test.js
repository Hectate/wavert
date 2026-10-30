import { test } from 'node:test';
import assert from 'node:assert/strict';
import { out, runWah } from './helpers.js';

const lines = (...xs) => xs.map((x) => `${x}\n`).join('');
const tantrum = (body) => {
  const r = runWah(`${body}\n;!invited`);
  if (r.compileErrors) throw new Error(`compile error: ${r.compileErrors.join('; ')}`);
  assert.equal(r.code, 1, `expected a tantrum, got stdout ${JSON.stringify(r.out)}`);
  assert.match(r.err, /^Waluigi was not invited to this exception\ntantrum: /);
  return r.err.split('\n')[1].slice('tantrum: '.length);
};

test('§14 example', () => {
  const r = runWah(`
;scheme (n)fizzbuzz {
    ;wah claim shifty i
    ;lurk (i <= n) {
        ;!doubt (i % wa[15]h == wh) { ;"WAHWAH"($print) }
        anyway !doubt (i % waaah == wh) { ;"WAH"($print) }
        anyway !doubt (i % waaaaah == wh) { ;"WAAAH"($print) }
        anyway { ;i($print) }
        ;++cheat i
    }
}
;scheme (a, b)average { (a + b) / waah }
;"x"($toInt) claim n
    whine (c) { wh }
;wa[15]h(fizzbuzz)
;wah, waah(average)($print)
;!invited`);
  assert.equal(r.out, lines(1, 2, 'WAH', 4, 'WAAAH', 'WAH', 7, 8, 'WAH', 'WAAAH', 11, 'WAH', 13, 14, 'WAHWAH', 1.5));
  assert.equal(r.code, 0);
});

test('§13 exit codes', () => {
  assert.equal(runWah(';!invited').code, 0);
  assert.equal(runWah(';WAH !invited').code, -1);
  assert.equal(runWah(';waah !invited ;"no"($print) ;!invited').code, 2);
});

test('§11.2 numerics', () => {
  assert.equal(out(';wah / waah($print)'), '0.5\n');
  assert.equal(out(';wa[4]h / waah($print)'), '2.0\n');
  assert.equal(out(';wa[4]h / waah == waah($print)'), 'real\n');
  assert.equal(out(';wah == wa[1.0]h($print)'), 'real\n');
  assert.equal(out(';wah < wa[1.5]h($print)'), 'real\n');
  assert.equal(out(';WAAAAAAAH % waaah($print)'), '2\n');
  assert.equal(out(';wa[7]h % WAAAH($print)'), '-2\n');
  assert.equal(out(';WA[7.0]H % wa[3.0]h($print)'), '2.0\n');
  assert.equal(out(';WAH ** waah($print)'), '1\n');
  assert.equal(out(';waah ** WAH($print)'), '0.5\n');
  assert.equal(out(';waah ** wa[10]h($print)'), '1024\n');
  assert.equal(out(';wah + wa[0.5]h($print)'), '1.5\n');
  assert.equal(out(';wa[2.5]h($round)($print) ;WA[2.5]H($round)($print) ;WA[2.5]H($floor)($print)'), lines(3, -3, -3));
  assert.equal(out(';wa[7]h / waah($floor)($print)'), '3\n');
  assert.equal(out(';nobody == nobody($print) ;nobody($isNobody)($print) ;-everybody($print)'), lines('fake', 'real', '-everybody'));
  assert.equal(out(';wa[1e21]h($print) ;wa[0.1]h + wa[0.2]h($print)'), lines('1.0e+21', '0.30000000000000004'));
  assert.match(tantrum(';waah ** wa[60]h($print)'), /integer overflow/);
  assert.match(tantrum(';wah % wh($print)'), /modulo by zero/);
  assert.match(tantrum(';nobody($floor)($print)'), /cannot convert nobody/);
});

test('§11.1 comparison and logic', () => {
  assert.equal(out(';real == wah($print) ;"1" == wah($print) ;wh == uninvited($print) ;uninvited == uninvited($print)'),
    lines('fake', 'fake', 'fake', 'real'));
  assert.equal(out(';"a" < wah($print) ;"abc" < "abd"($print) ;"\\u{FFFF}" < "\\u{1F608}"($print)'), lines('fake', 'real', 'real'));
  assert.equal(out(';real && fake || real($print) ;!fake($print)'), lines('real', 'real'));
  assert.match(tantrum(';real < fake($print)'), /cannot order boolean/);
  assert.match(tantrum(';wah && real($print)'), /'&&' requires real or fake, got int/);
  assert.match(tantrum(';!doubt (wah) { }'), /condition must be real or fake, got int/);
  // short-circuit: the right side would be a type error
  assert.equal(out(';fake && wah($print)'), 'fake\n');
});

test('§11.1 bitwise', () => {
  assert.equal(out(';wa[0b1100]h & wa[0b1010]h($print) ;wa[12]h | waaah($print) ;wah << wa[4]h($print) ;WA[16]H >> waah($print) ;~wh($print)'),
    lines(8, 15, 16, -4, -1));
  assert.equal(out(';WAH($toUint32)($print)'), '4294967295\n');
  assert.match(tantrum(';wah << wa[32]h($print)'), /outside 0-31/);
  assert.match(tantrum(';wa[0x100000000]h & wah($print)'), /int32/);
  assert.match(tantrum(';wa[1.0]h & wah($print)'), /requires integers/);
});

test('§9 strings', () => {
  assert.equal(out(';wa[42]h claim n ;"score: {n}, {n / waah}, {real}, {uninvited}, {{n}}"($print)'),
    'score: 42, 21.0, real, uninvited, {n}\n');
  assert.equal(out(';"a" + "b"($print)'), 'ab\n');
  assert.match(tantrum(';"a" + wah($print)'), /'\+' cannot be applied to purple and int \(use interpolation/);
  assert.equal(out(';"h\\u{1F608}llo" claim s ;s($length)($print) ;s, wah($at)($print) ;s, wah, waaah($slice)($print) ;s, WAH($at)($print)'),
    lines(5, '\u{1F608}', '\u{1F608}l', 'o'));
  assert.equal(out(';"A"($code)($print) ;wa[66]h($fromCode)($print) ;"a,b,c", ","($split)($length)($print)'), lines(65, 'B', 3));
  assert.equal(out(';WAAH($toWah)($print) ;wa[42]h($toWah)($print) ;wh($toWah)($print) ;WA[0.5]H($toWah)($print)'),
    lines('WAAH', 'wa[42]h', 'wh', 'WA[0.5]H'));
  assert.equal(out(';"42"($toInt)($print) ;"WAAH"($toInt)($print) ;"wa[0xFF]h"($toInt)($print) ;"-7"($toInt)($print)'),
    lines(42, -2, 255, -7));
  assert.equal(out(';"1.5"($toFloat)($print) ;"wa[1.5]h"($toFloat)($print) ;wah($toFloat)($print) ;wa[3]h($toPurple) + "!"($print)'),
    lines(1.5, 1.5, '1.0', '3!'));
  assert.equal(out(';"1.5"($toInt) claim n whine (c) { WA[99]H } ;n($print)'), '-99\n');
  assert.match(tantrum(';[wah]($print)'), /cannot convert array to text/);
});

test('§10 arrays: indexing and mutation', () => {
  assert.equal(out(';[wa[10]h, wa[20]h, wa[30]h] claim a ;wh[a]($print) ;WAH[a]($print) ;a($length)($print)'), lines(10, 30, 3));
  assert.equal(out(';[[wah, waah], [waaah, waaaah]] claim g ;wah[wh[g]]($print)'), '2\n');
  assert.equal(out(';[wah] claim shifty a ;wa[5]h cheat wh[a] ;wah +cheat wh[a] ;a, waah($push) ;a, ", "($join)($print) ;a($pop)($print) ;a($length)($print)'),
    lines('6, 2', 2, 1));
  assert.match(tantrum(';[wah] claim a ;waaah[a]($print)'), /index 3 is out of range for an array of length 1/);
  assert.match(tantrum(';[wah] claim a ;a, waah($push)'), /cannot modify a non-shifty array with \$push/);
  assert.match(tantrum(';"abc" claim s ;wh[s]($print)'), /use \$at for purple/);
});

test('§10.4 copy vs reference', () => {
  // shifty -> claim: frozen snapshot
  assert.equal(out(';[wah] claim shifty a ;a claim snap ;a, waah($push) ;snap($length)($print) ;a($length)($print)'), lines(1, 2));
  // shifty -> claim shifty: alias
  assert.equal(out(';[wah] claim shifty a ;a claim shifty b ;b, waah($push) ;a($length)($print)'), '2\n');
  // frozen -> claim shifty: copy made mutable
  assert.equal(out(';[wah] claim a ;a claim shifty b ;b, waah($push) ;a($length)($print) ;b($length)($print)'), lines(1, 2));
  // parameters follow the same table
  assert.equal(out(`
;scheme (shifty xs)grow { ;xs, wah($push) }
;scheme (xs)peek { xs($length) }
;[] claim shifty a
;a(grow) ;a(grow)
;a(peek)($print)
;a($length)($print)`), lines(2, 2));
  // equality: reference vs structural
  assert.equal(out(';[wah] claim a ;[wah] claim b ;a == b($print) ;a == a($print) ;a, b($same)($print) ;[[wah]], [[wah]]($same)($print)'),
    lines('fake', 'real', 'real', 'real'));
});

test('§5, §10.4 fickle bindings keep values as they are', () => {
  // A fickle binding shares the value: frozen stays frozen, shifty is aliased; shifty copies a frozen value.
  assert.equal(out(';[wah] claim a ;a claim fickle f ;a claim shifty s ;f == a($print) ;s == a($print)'), lines('real', 'fake'));
  assert.equal(out(';[wah] claim shifty s ;s claim fickle f ;waah cheat wh[f] ;wh[s]($print) ;f, waaah($push) ;s($length)($print)'),
    lines(2, 2));
  // cheat rebinds without copying, so identity is kept.
  assert.equal(out(`
;{ v: wah } claim first
;first claim fickle acc
;wh claim shifty i
;lurk (i < waaah) {
    ;{ v: v.acc + wah } cheat acc
    ;++cheat i
}
;v.acc($print)
;first claim fickle again
;first cheat again
;again == first($print)
;wa[0.5]h claim fickle n
;wah +cheat n
;n($print)`), lines(4, 'real', 1.5));
  // Through a fickle binding, a frozen value cannot be changed (§10.3, §10.9).
  assert.match(tantrum(';[wah] claim fickle f ;waah cheat wh[f]'), /cannot modify a non-shifty array/);
  assert.match(tantrum(';[wah] claim fickle f ;f, waah($push)'), /cannot modify a non-shifty array with \$push/);
  assert.match(tantrum(';{ v: wah } claim fickle r ;waah cheat v.r'), /cannot modify a field of a non-shifty record/);
  assert.equal(out(';{ v: wah } claim shifty s ;s claim fickle r ;waah cheat v.r ;v.s($print)'), '2\n');
});

test('§6.1 fickle parameters take the argument as it is', () => {
  assert.equal(out(`
;scheme (fickle xs)itself { xs }
;scheme (xs)snapshot { xs }
;[wah] claim shifty a
;(a(itself)) == a($print)
;(a(snapshot)) == a($print)
;scheme (fickle xs, fickle n)grow {
    ;xs, n($push)
    ;n + wah cheat n
    ;scheme ()peek { n }
    ;[(,(peek))] cheat xs
    xs
}
;a, waah(grow) claim r
;wh[r]($print)
;a, ","($join)($print)`), lines('real', 'fake', 3, '1,2'));
});

test('§6 functions, hoisting and recursion', () => {
  assert.equal(out(`
;scheme (n)even { n == wh ? real : (n - wah(odd)) }
;scheme (n)odd { n == wh ? fake : (n - wah(even)) }
;wa[10]h(even)($print)
;scheme (n)fact {
    ;!doubt (n <= wah) { ;getaway wah }
    n * (n - wah(fact))
}
;wa[10]h(fact)($print)
;scheme ()nothing { ;wah claim x uninvited }
;,(nothing)($print)`), lines('real', 3628800, 'uninvited'));
});

test('§5.1 runtime temporal dead zone', () => {
  assert.match(tantrum(';,(f)($print) ;wah claim x ;scheme ()f { x }'), /'x' was used before its claim/);
  // A claim in a loop body is unclaimed again at the start of each iteration.
  assert.match(tantrum(`
;wh claim shifty i
;lurk (i < waah) {
    ;scheme ()f { x }
    ;!doubt (i == wah) { ;,(f)($print) }
    ;i claim x
    ;++cheat i
}`), /'x' was used before its claim/);
  assert.match(tantrum(';scheme ()f { ;wah cheat x x } ;,(f)($print) ;wh claim shifty x'), /'x' was used before its claim/);
});

test('§6 nested schemes read and cheat the bindings around them', () => {
  assert.equal(out(`
;scheme (n)outer {
    ;wh claim shifty total
    ;scheme (k)mid {
        ;scheme ()inner { ;k +cheat total n + k }
        ,(inner)
    }
    ;wah(mid)($print)
    ;waah(mid)($print)
    total
}
;wa[10]h(outer)($print)
;scheme (n)count {
    ;scheme ()get { n }
    ;!doubt (n == wh) { ;getaway wh }
    ;n - wah(count) claim rest
    ;,(get) claim mine
    rest + mine
}
;waaah(count)($print)`), lines(11, 12, 3, 6));
});

test('§6.2 operands, arguments and elements are evaluated left to right', () => {
  assert.equal(out(`
;scheme (s)say { ;s($print) s }
;scheme (a, b)pair { a + b }
;("a"(say)) + ("b"(say))($print)
;[("c"(say)), ("d"(say))]($length)($print)
;("e"(say)), ("f"(say))(pair)($print)
;{ a: "g"(say), b: "h"(say) } claim r
;("i"(say)) == ("j"(say))($print)`), lines('a', 'b', 'ab', 'c', 'd', 2, 'e', 'f', 'ef', 'g', 'h', 'i', 'j', 'fake'));
  // The first element fails before the second runs.
  const r = runWah(';scheme ()boom { ;"boom ran"($print) wh } ;[wah + "x", (,(boom))]($print) ;!invited');
  assert.equal(r.out, '');
  assert.match(r.err, /'\+' cannot be applied to int and purple/);
});

test('§7.5 suspect', () => {
  const kind = ';scheme (k)kind { suspect (k) { alibi "int", "float" { "number" } alibi "str" { "text" } anyway { "other" } } }';
  assert.equal(out(`${kind} ;"int"(kind)($print) ;"float"(kind)($print) ;"str"(kind)($print) ;"x"(kind)($print) ;wah(kind)($print)`),
    lines('number', 'number', 'text', 'other', 'other'));
  // == semantics: an int alibi matches an equal float; types never mix otherwise
  const size = ';scheme (n)size { suspect (n) { alibi wh { "none" } alibi wah { "one" } anyway { "many" } } }';
  assert.equal(out(`${size} ;wh(size)($print) ;wa[1.0]h(size)($print) ;wa[1.5]h(size)($print) ;"1"(size)($print) ;real(size)($print)`),
    lines('none', 'one', 'many', 'many', 'many'));
  assert.equal(out(';suspect (uninvited) { alibi real, fake { ;"bool"($print) } alibi uninvited { ;"null"($print) } }'), 'null\n');
  // no match and no anyway: nothing happens; the subject is evaluated once
  assert.equal(out(';scheme ()s { ;"eval"($print) "z" } ;suspect (,(s)) { alibi "a" { ;"a"($print) } alibi "b" { } }'), 'eval\n');
  // blocks with statements, used as a value
  assert.equal(out(';scheme (n)f { suspect (n) { alibi wah { ;"side"($print) "one" } anyway { ;"other"($print) "?" } } } ;wah(f)($print) ;waah(f)($print)'),
    lines('side', 'one', 'other', '?'));
  // bail and loiter act on the enclosing loop
  assert.equal(out(`;wh claim shifty i
;lurk (i < wa[10]h) {
    ;++cheat i
    ;suspect (i % waaah) { alibi wh { ;loiter } alibi wah { } }
    ;suspect (i) { alibi waaaaah, waaaaaah { ;"skip {i}"($print) } alibi waaaaaaah { ;bail } anyway { ;i($print) } }
}`), lines(1, 2, 4, 'skip 5'));
  // nested, and inside interpolation
  assert.equal(out(';wah claim a ;waah claim b ;suspect (a) { alibi wah { ;suspect (b) { alibi waah { ;"1,2"($print) } } } }'), '1,2\n');
  assert.equal(out(';real claim t ;"{suspect (t) { alibi real { "yes" } anyway { "no" } }}"($print)'), 'yes\n');
});

test('§4.1 lineup members', () => {
  const kind = ';lineup (num, str, call, chain)Kind ;lineup (call)Op';
  // text, $typeOf and == by identity
  assert.equal(out(`${kind} ;call.Kind claim k ;k($print) ;"<{k}>"($print) ;k($toPurple)($print) ;k($typeOf)($print)`),
    lines('call', '<call>', 'call', 'Kind'));
  assert.equal(out(`${kind} ;call.Kind claim k
;(k == call.Kind)($print) ;(k != chain.Kind)($print) ;(k == call.Op)($print) ;(k == "call")($print) ;(k == wh)($print)
;(k, call.Kind($same))($print) ;(k < wah)($print)`), lines('real', 'real', 'fake', 'fake', 'fake', 'real', 'fake'));
  // stored, passed and returned like any value; visible before the declaration
  assert.equal(out(`;scheme (k)next { suspect (k) { alibi a.Light { b.Light } alibi b.Light { c.Light } alibi c.Light { a.Light } } }
;[a.Light, b.Light] claim shifty ls ;ls, (wah[ls](next))($push) ;{ light: c.Light } claim r
;ls, " "($join)($print) ;light.r(next)($print)
;lineup (a, b, c)Light`), lines('a b c', 'a'));
  // ordering and arithmetic raise tantrums
  assert.match(tantrum(`${kind} ;(num.Kind < str.Kind)($print)`), /'<' cannot order lineup members/);
  assert.match(tantrum(`${kind} ;(num.Kind >= call.Op)($print)`), /'>=' cannot order lineup members/);
  assert.match(tantrum(`${kind} ;(num.Kind + wah)($print)`), /'\+' cannot be applied to Kind and int/);
  assert.match(tantrum(`${kind} ;(-num.Kind)($print)`), /'-' cannot be applied to Kind/);
  assert.match(tantrum(`${kind} ;tantrum chain.Kind`), /^chain$/);
});

test('§7.5 suspect over lineup members', () => {
  const kind = `;lineup (num, str, call, chain)Kind
;scheme (k)what { suspect (k) { alibi num.Kind, str.Kind { "literal" } alibi call.Kind { "call" } alibi chain.Kind { "chain" } } }`;
  assert.equal(out(`${kind} ;num.Kind(what)($print) ;str.Kind(what)($print) ;call.Kind(what)($print) ;chain.Kind(what)($print)`),
    lines('literal', 'literal', 'call', 'chain'));
  // an exhaustive suspect raises a tantrum for a subject that is not a member
  assert.match(tantrum(`${kind} ;"call"(what)($print)`), /^suspect: no alibi for "call"$/);
  assert.match(tantrum(`${kind} ;wah(what)($print)`), /^suspect: no alibi for 1$/);
  assert.match(tantrum(`${kind} ;suspect ("x") { alibi num.Kind, str.Kind, call.Kind { } alibi chain.Kind { ;"no"($print) } }`),
    /^suspect: no alibi for "x"$/);
  // ...also when its blocks have statements
  assert.match(tantrum(`${kind} ;suspect (uninvited) { alibi num.Kind, str.Kind, call.Kind { ;wah claim x x } alibi chain.Kind { wh } }($print)`),
    /^suspect: no alibi for uninvited$/);
  // with anyway, unmatched subjects go there
  assert.equal(out(`${kind} ;suspect ("x") { alibi call.Kind { ;"call"($print) } anyway { ;"other"($print) } }`), 'other\n');
  assert.equal(out(`${kind} ;suspect (chain.Kind) { alibi call.Kind { ;"call"($print) } anyway { ;"other"($print) } }`), 'other\n');
});

test('§7 control flow', () => {
  assert.equal(out(`
;wh claim shifty i
;!lurk (i == wa[5]h) {
    ;++cheat i
    ;!doubt (i == waah) { ;loiter }
    ;!doubt (i == waaaah) { ;bail }
    ;i($print)
}
;doubt (i == wa[4]h) { ;"no"($print) } anyway { ;"four"($print) }
;!doubt (i < wh) { "neg" } anyway !doubt (i == wh) { "zero" } anyway { "pos" } claim label
;label($print)
;real ? "t" : "f"($print)`), lines(1, 3, 'four', 'pos', 't'));
  // conditional value with statements inside branches
  assert.equal(out(';!doubt (real) { ;"side"($print) wa[7]h } anyway { wh } claim v ;v($print)'), lines('side', 7));
});

test('§8.2 signals', () => {
  const prelude = `
;scheme (s)parse {
    ;!doubt (s == "") { ;WAAAH! }
    s($length)
}
;scheme (s)forward {
    ;s(parse) claim n
        whine (c) { ;c(WAH!) }
    n
}
;scheme (s)renew {
    ;s(parse) claim n
        whine (c) { ;WAH! }
    n
}
;scheme (s)ignore { s(parse) }
`;
  assert.equal(out(`${prelude}
;""(parse) claim a whine (c) { c } ;a($print)
;"abc"(parse) claim b whine (c) { c } ;b($print)
;""(forward) claim d whine (c) { c } ;d($print)
;""(renew) claim e whine (c) { c } ;e($print)
;""(parse) whine { ;"handled"($print) }`), lines(3, 3, 3, 1, 'handled'));
  // an unhandled signal is a runtime error in the caller (§8.2 stage 0)
  assert.match(tantrum(`${prelude} ;""(ignore) claim x whine (c) { c }`), /unhandled WAH! \(code 3\) from 'parse' in 'ignore'/);
  assert.match(tantrum(`${prelude} ;""(parse)($print)`), /unhandled WAH! \(code 3\) from 'parse'/);
  // whine only covers the outermost call
  assert.match(tantrum(`${prelude} ;""(parse)(forward) claim x whine (c) { c }`), /unhandled WAH!/);
  // forwarding with a bad code is a tantrum
  assert.match(tantrum(';scheme ()f { ;wh(WAH!) } ;,(f) whine { wh }'), /WAH! code must be an integer >= 1/);
});

test('§8.1 tantrum', () => {
  const r = runWah(';scheme ()boom { ;tantrum "it broke" } ;,(boom) ;!invited');
  assert.equal(r.code, 1);
  assert.equal(r.err, 'Waluigi was not invited to this exception\ntantrum: it broke\n    in boom\n');
  assert.match(tantrum(';tantrum [wah, "x"]'), /\[1, "x"\]/);
});

test('§12 console, files, process', () => {
  const r = runWah(';,($readLine)($print) ;,($readLine) claim l whine { "EOF" } ;l($print) ;$args($length)($print) ;"HOME"($env)($print) ;"NOPE"($env) claim e whine { "unset" } ;e($print) ;"err"($eprint) ;"no newline"($write) ;!invited',
    { stdin: ['first'], args: ['a', 'b'], env: { HOME: '/home/wah' } });
  assert.equal(r.out, lines('first', 'EOF', 2, '/home/wah', 'unset') + 'no newline');
  assert.equal(r.err, 'err\n');
});

test('§12.3 files', async (t) => {
  const { mkdtempSync, rmSync } = await import('node:fs');
  const { tmpdir } = await import('node:os');
  const { join } = await import('node:path');
  const dir = mkdtempSync(join(tmpdir(), 'wah-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const p = join(dir, 'f.txt').replaceAll('\\', '/');
  const d = dir.replaceAll('\\', '/');
  assert.equal(out(`
;"${p}" claim p
;p($fileExists)($print)
;p, "héllo"($writeFile)
;p, " \\u{1F608}"($appendFile)
;p($readFile)($print)
;"${d}"($listDir)($length)($print)
;p($deleteFile)
;p($fileExists)($print)
;p($readFile) claim gone whine { "missing" }
;gone($print)`), lines('fake', 'héllo \u{1F608}', 1, 'fake', 'missing'));
});

test('§9.3 float text, exactly', () => {
  const cases = [
    ['1500', '1500.0'], ['0.5', '0.5'], ['0.000001', '0.000001'], ['0.0000001', '1.0e-7'], ['1.5e-7', '1.5e-7'],
    ['123.456', '123.456'], ['1e20', '100000000000000000000.0'], ['1e21', '1.0e+21'], ['1.25e22', '1.25e+22'],
    ['0.30000000000000004', '0.30000000000000004'], ['5e-324', '5.0e-324'], ['1e-400', '0.0'], ['-1e-400', '-0.0'],
    ['1.7976931348623157e308', '1.7976931348623157e+308'], ['-2.5', '-2.5'], ['0.1', '0.1'],
  ];
  const body = cases.map(([t]) => `;"${t}"($toFloat)($print)`).join(' ');
  assert.equal(out(body), lines(...cases.map(([, want]) => want)));
  assert.equal(out(';wa[0.1]h + wa[0.2]h($print) ;wah / waah($print) ;-wa[0.0]h($print)'), lines('0.30000000000000004', '0.5', '-0.0'));
});

test('§9.5 numeric text, exactly', () => {
  const toInt = (s) => out(`;"${s}"($toInt) claim n whine { "WAH!" } ;n($print)`).trimEnd();
  const toFloat = (s) => out(`;"${s}"($toFloat) claim n whine { "WAH!" } ;n($print)`).trimEnd();
  assert.deepEqual(['42', '-7', '007', '-0', 'wh', 'WAAH', 'wa[0xFF]h', 'WA[0b101]H', 'wa[42]h'].map(toInt),
    ['42', '-7', '7', '0', '0', '-2', '255', '-5', '42']);
  for (const bad of ['', ' 1', '1 ', '+1', '1.5', '1e3', '0x10', 'wa[1.5]h', '9007199254740992', 'Wah', '٣']) {
    assert.equal(toInt(bad), 'WAH!', JSON.stringify(bad));
  }
  assert.deepEqual(['1.5', '-2', '6.02e23', '1E3', '1e+3', 'wa[1.5]h', 'WAAH', '9007199254740993'].map(toFloat),
    ['1.5', '-2.0', '6.02e+23', '1000.0', '1000.0', '1.5', '-2.0', '9007199254740992.0']);
  for (const bad of ['', '.5', '5.', '1e', '+1', ' 1', 'nobody', 'Infinity', '1e999', '-1e400', 'wa[1e999]h']) {
    assert.equal(toFloat(bad), 'WAH!', JSON.stringify(bad));
  }
});
