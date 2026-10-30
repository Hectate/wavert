import { test } from 'node:test';
import assert from 'node:assert/strict';
import { errors, runWah } from './helpers.js';

const one = (body, re) => {
  const errs = errors(body);
  assert.ok(errs.some((e) => re.test(e)), `expected ${re} in ${JSON.stringify(errs)}`);
};
const none = (body) => assert.deepEqual(errors(body), []);

test('§13 entry file must end with !invited', () => {
  const r = runWah(';"hi"($print)');
  assert.deepEqual(r.compileErrors, ["program ended, but Waluigi was never told he wasn't invited"]);
  assert.equal(runWah(';"hi"($print) ;WAH !invited ;').compileErrors, undefined);
});

test('§5 cheat requires shifty', () => {
  one(';wah claim x ;waah cheat x', /cannot cheat on 'x': it is not shifty/);
  one(';wah claim x ;++cheat x', /not shifty/);
  one(';scheme (a)f { ;wah +cheat a } ;wah(f)', /not shifty/);
  none(';scheme (shifty a)f { ;wah +cheat a } ;wah(f)');
  one(';[wah] claim arr ;waah cheat wh[arr]', /cannot modify an element of 'arr'/);
  none(';[wah] claim shifty arr ;waah cheat wh[arr]');
});

test('§5 fickle bindings and parameters', () => {
  const r = runWah(';wah claim x ;waah cheat x ;!invited');
  assert.deepEqual(r.compileErrors, ["cannot cheat on 'x': it is not shifty or fickle"]);
  assert.deepEqual(r.hints, ["declare it with ';... claim fickle x' to rebind it, or ';... claim shifty x' for a mutable copy"]);
  assert.deepEqual(runWah(';scheme (a)f { ;wah cheat a a } ;wah(f) ;!invited').hints,
    ['mark the parameter (fickle a) to rebind it, or (shifty a) for a mutable copy']);
  none(';wah claim fickle x ;waah cheat x ;++cheat x');
  none(';scheme (fickle a, b)f { ;wah +cheat a a } ;wah, wah(f)');
  // The value's own mutability is checked at runtime.
  none(';[wah] claim fickle arr ;waah cheat wh[arr] ;arr, wah($push)');
  none(';{ v: wah } claim fickle r ;waah cheat v.r');
  // At most one modifier, after claim; fickle is a keyword.
  one(';wah claim shifty fickle x', /expected a name after 'claim'/);
  one(';wah claim fickle shifty x', /expected a name after 'claim'/);
  one(';wah fickle x', /'fickle' goes after 'claim'/);
  one(';scheme (shifty fickle a)f { a } ;wah(f)', /expected a parameter name/);
  one(';wah claim fickle', /expected a name after 'claim'/);
  one(';{ fickle: wah } claim r', /field name/);
});

test('§5.1 no shadowing', () => {
  one(';wah claim x ;!doubt (real) { ;waah claim x }', /shadows 'x'/);
  one(';wah claim x ;wah claim x', /already declared/);
  one(';wah claim x ;scheme (x)f { x } ;wah(f)', /shadows 'x'/);
  one(';scheme (a)f { ;wah claim a } ;wah(f)', /shadows 'a'/);
  none(';!doubt (real) { ;wah claim x } ;!doubt (real) { ;waah claim x }');
  // spec §14: a scheme parameter may reuse a name claimed later at top level
  none(';scheme (n)f { n } ;wah(f) ;wah claim n');
});

test('§5.1 use before claim', () => {
  one(';x($print) ;wah claim x', /'x' is used before its claim/);
  one(';x + wah claim x', /used before its claim/);
  one(';"a"($toInt) claim n whine (c) { n }', /used before its claim/);
  none(';scheme ()f { x } ;wah claim x ;,(f)($print)');
  none(';,(f)($print) ;scheme ()f { wah }');
});

test('names and stdlib', () => {
  one(';y($print)', /unknown name 'y'/);
  one(';x($nope)', /unknown standard library name '\$nope'/);
  one(';$print($print)', /'\$print' is a function/);
  one(';wah($args)', /'\$args' is a constant/);
  one(';wah, waah($print)', /'\$print' takes 1 argument, got 2/);
  one(';scheme (a, b)f { a } ;wah(f)', /'f' takes 2 arguments, got 1/);
  one(';scheme ()f { wah } ;f($print)', /functions are not values/);
  one(';wah claim x ;wah(x)', /'x' is not a scheme/);
});

test('control flow placement', () => {
  one(';getaway wah', /'getaway' outside a scheme/);
  one(';bail', /'bail' outside a loop/);
  one(';WAH!', /WAH! outside a scheme/);
  one(';scheme ()f { ;wah, waah(WAH!) } ;,(f)', /exactly one argument/);
  none(';scheme ()f { ;lurk (real) { ;bail } }');
});

test('§7.2 conditionals as values need a final anyway', () => {
  one(';!doubt (real) { wah } claim x', /must end with a plain 'anyway/);
  one(';!doubt (real) { wah } anyway !doubt (fake) { waah } claim x', /must end with a plain 'anyway/);
  none(';!doubt (real) { wah } anyway { waah } claim x');
});

test('§7.2 explicit values: schemes', () => {
  const never = /'f' never produces a value, but its value is used \(line \d+\)/;
  one(';scheme ()f { ;"x"($print) } ;,(f)($print)', never);
  one(';scheme ()f { ;"x"($print) } ;,(f) claim y', never);
  one(';scheme ()f { } ;scheme ()g { ;getaway ,(f) }', never);
  one(';scheme ()f { ;getaway } ;,(f) claim y', never);
  one(';scheme (n)f { ;!doubt (n) { ;n } } ;real(f) claim y', never);
  // the error is at the scheme, and names the line of the first use
  assert.deepEqual(errors(';scheme ()f { }\n;,(f)\n;,(f) claim a\n;,(f) claim b'),
    ["'f' never produces a value, but its value is used (line 3)"]);
  none(';scheme ()f { ;"x"($print) } ;,(f)');
  none(';scheme ()f { ;"x"($print) } ;,(f) whine { }');
  none(';scheme ()f { ;"x"($print) uninvited } ;,(f)($print)');
  none(';scheme (n)f { ;!doubt (n) { ;getaway wah } } ;real(f)($print)');
  none(';scheme (n)f { ;lurk (real) { ;getaway n } } ;wah(f)($print)');
  none(';scheme (n)f { ;"1"($toInt) claim x whine { ;getaway n } } ;wah(f)($print)');
  // a getaway inside a nested scheme does not count
  one(';scheme ()f { ;scheme ()g { ;getaway wah } ;,(g) } ;,(f) claim y', never);
});

test('§7.2 explicit values: blocks used as values', () => {
  const noValue = /this block's value is used, but it does not produce one/;
  one(';!doubt (real) { wah } anyway { ;"no"($print) } claim r', noValue);
  one(';!doubt (real) { ;wah } anyway { wah } claim r', noValue);
  one(';"1"($toInt) claim n whine { ;"bad"($print) }', noValue);
  one(';scheme ()f { ;"1"($toInt) claim n whine { } ;getaway n }', noValue);
  none(';!doubt (real) { wah } anyway { ;"no"($print) uninvited } claim r');
  none(';"1"($toInt) claim n whine { ;"bad"($print) ;waah !invited }');
  none(';"1"($toInt) claim n whine { ;tantrum "bad" }');
  none(';scheme ()f { ;"1"($toInt) claim n whine { ;getaway wh } ;getaway n }');
  none(';scheme ()f { ;"1"($toInt) claim n whine { ;WAH! } ;getaway n }');
  none(';scheme ()f { ;"1"($toInt) claim n whine (c) { ;c(WAH!) } ;getaway n }');
  none(';lurk (fake) { ;"1"($toInt) claim n whine { ;bail } }');
  none(';lurk (fake) { ;"1"($toInt) claim n whine { ;loiter ; } }');
  // statement-level chains and expression-statement handlers are not values
  none(';!doubt (real) { ;"a"($print) } anyway { }');
  none(';"1"($toInt)($print) whine { }');
  // the error points at the block's closing brace
  assert.match(errors(';!doubt (real) { wah } anyway {\n  ;"no"($print)\n} claim r')[0], noValue);
});

test('§8.2 whine must follow a call', () => {
  one(';wah claim x whine (c) { wh }', /this statement's value is not a call/);
  one(';"a"($toInt) + wah claim x whine (c) { wh }', /not a call/);
});

test('§7.5 suspect: checker', () => {
  one(';suspect (wah) { alibi wah { } alibi waah, wah { } }', /duplicate alibi value 1 \(line 1\)/);
  one(';suspect ("a") { alibi "a", "b", "a" { } }', /duplicate alibi value "a"/);
  one(';suspect (real) { alibi real { } alibi real { } }', /duplicate alibi value real/);
  one(';suspect (uninvited) { alibi uninvited, uninvited { } }', /duplicate alibi value uninvited/);
  none(';suspect (wah) { alibi wah { } alibi "1", real, uninvited { } }'); // different types never collide
  none(';suspect (wah) { alibi wah { } alibi WAH { } alibi wh { } }');
  // as a value it needs anyway, and every branch needs a value
  one(';suspect (wah) { alibi wah { "a" } } claim x', /a suspect used as a value must end with 'anyway \{ ... \}'/);
  one(';suspect (wah) { alibi wah { ;"a"($print) } anyway { "b" } } claim x', /this block's value is used/);
  one(';suspect (wah) { alibi wah { "a" } anyway { } } claim x', /this block's value is used/);
  none(';suspect (wah) { alibi wah { "a" } anyway { "b" } } claim x');
  none(';suspect (wah) { alibi wah { ;tantrum "no" } anyway { "b" } } claim x');
  none(';suspect (wah) { alibi wah { ;"a"($print) } }'); // a statement needs neither
  // names are resolved in the subject and the blocks, each block in its own scope
  one(';suspect (q) { alibi wah { } }', /unknown name 'q'/);
  one(';suspect (wah) { alibi wah { ;r($print) } anyway { ;s($print) } }', /unknown name 'r'/);
  none(';suspect (wah) { alibi wah { ;wah claim x } anyway { ;wah claim x } } ;wah claim x');
  // bail and loiter refer to the enclosing loop
  one(';suspect (wah) { alibi wah { ;bail } }', /'bail' outside a loop/);
  none(';lurk (fake) { ;suspect (wah) { alibi wah { ;bail } anyway { ;loiter } } }');
  // a getaway inside a statement-level suspect produces the scheme's value
  none(';scheme (n)f { ;suspect (n) { alibi wah { ;getaway "one" } } uninvited } ;wah(f)($print)');
  none(';scheme (n)f { ;suspect (n) { alibi wah { ;getaway "one" } anyway { ;getaway "?" } } } ;wah(f)($print)');
});

test('§4.1 lineups: checker', () => {
  const kind = ';lineup (num, str, call, chain)Kind';
  none(`${kind} ;call.Kind claim k ;[num.Kind, k] claim ks ;{ kind: str.Kind }($print)`);
  none(';chain.Kind($print) ;lineup (call, chain)Kind'); // visible throughout the file
  none(';scheme ()f { num.Kind } ;lineup (num)Kind ;,(f)($print)');
  // a member label is not a binding
  none(`${kind} ;wah claim num ;num.Kind claim k`);
  one(`${kind} ;cal.Kind($print)`, /no member 'cal' in lineup 'Kind'/);
  // a lineup name is not a value
  one(`${kind} ;Kind($print)`, /'Kind' is a lineup, not a value/);
  one(`${kind} ;wah[Kind]($print)`, /'Kind' is a lineup, not a value/);
  one(`${kind} ;wah(Kind)`, /'Kind' is not a scheme/);
  one(`${kind} ;wah cheat Kind`, /'Kind' is a lineup, not a binding/);
  one(`${kind} ;wah cheat wh[Kind]`, /'Kind' is a lineup, not a binding/);
  one(`${kind} ;num.Kind cheat call.Kind`, /cannot cheat on 'call.Kind': lineup members are constants/);
  // the no-shadowing rule treats a lineup like a scheme
  one(`${kind} ;wah claim Kind`, /'Kind' is already declared in this scope/);
  one(`${kind} ;lineup (a)Kind`, /'Kind' is already declared in this scope/);
  one(`${kind} ;scheme ()f { ;wah claim Kind }`, /'Kind' shadows 'Kind'/);
  one(`${kind} ;scheme (Kind)f { }`, /'Kind' shadows 'Kind'/);
  // member access on anything else is still a field
  none(';{ num: wah } claim Kind2 ;num.Kind2($print)');
});

test('§7.5 suspect over lineup members', () => {
  const kind = ';lineup (num, str, call, chain)Kind ;call.Kind claim k';
  none(`${kind} ;suspect (k) { alibi num.Kind, str.Kind { } alibi call.Kind { } alibi chain.Kind { } }`);
  none(`${kind} ;suspect (k) { alibi call.Kind { } anyway { } }`);
  // exhaustive without anyway
  one(`${kind} ;suspect (k) { alibi num.Kind, str.Kind { } alibi call.Kind { } }`, /suspect does not handle 'chain.Kind'$/);
  one(`${kind} ;suspect (k) { alibi call.Kind { } }`, /suspect does not handle 'num.Kind', 'str.Kind', 'chain.Kind'/);
  // as a value, an exhaustive suspect needs no anyway
  none(`${kind} ;suspect (k) { alibi num.Kind, str.Kind { "lit" } alibi call.Kind, chain.Kind { "other" } } claim x`);
  one(`${kind} ;suspect (k) { alibi num.Kind { "lit" } } claim x`, /suspect does not handle 'str.Kind', 'call.Kind', 'chain.Kind'/);
  one(`${kind} ;suspect (k) { alibi num.Kind, str.Kind, call.Kind { "a" } alibi chain.Kind { } } claim x`, /this block's value is used/);
  // one sort per suspect
  one(`${kind} ;suspect (k) { alibi call.Kind { } alibi "num" { } anyway { } }`, /a suspect cannot mix literals and lineup members/);
  one(`${kind} ;suspect (k) { alibi wah { } alibi call.Kind { } anyway { } }`, /cannot mix literals and lineup members/);
  one(`${kind} ;lineup (call)Op ;suspect (k) { alibi call.Kind { } alibi call.Op { } anyway { } }`, /a suspect cannot mix members of 'Kind' and 'Op'/);
  // duplicates, unknown members and things that are not lineups
  one(`${kind} ;suspect (k) { alibi call.Kind { } alibi chain.Kind, call.Kind { } anyway { } }`, /duplicate alibi value call.Kind \(line 1\)/);
  one(`${kind} ;suspect (k) { alibi cal.Kind { } anyway { } }`, /no member 'cal' in lineup 'Kind'/);
  one(`${kind} ;suspect (k) { alibi call.Knid { } anyway { } }`, /unknown lineup 'Knid'/);
  one(`${kind} ;{ call: wah } claim r ;suspect (k) { alibi call.r { } anyway { } }`, /'r' is not a lineup/);
  // a literal suspect without anyway is still fine as a statement
  none(`${kind} ;suspect ("x") { alibi "y" { } }`);
});
