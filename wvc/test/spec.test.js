// Every complete program in the specification (docs/wavert-spec.md) compiles
// without a diagnostic: a code block that ends with ;exit and has no "..."
// placeholder.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { diagnostics, noZig, readRepo, run } from './helpers.js';

const spec = readRepo('docs/wavert-spec.md');
const programs = [...spec.matchAll(/```\n([\s\S]*?)```/g)].map((m) => m[1])
  .filter((code) => /^;exit\s*$/m.test(code.trimEnd().split('\n').at(-1)) && !code.includes('...'));

test('the specification has complete programs', () => {
  assert.ok(programs.length >= 2, `found ${programs.length}`);
});

programs.forEach((code, i) => {
  test(`program ${i + 1}: ${code.split('\n')[0]}`, { skip: noZig }, () => {
    assert.equal(diagnostics(code), '');
  });
});

test('the example in §14 runs', { skip: noZig }, () => {
  const example = /## 14\. Example\n\n```\n([\s\S]*?)```/.exec(spec)[1];
  const r = run(example);
  assert.equal(r.code, 0, r.err);
  assert.ok(r.out.startsWith('1\n2\nFizz\n4\nBuzz\n'));
  assert.ok(r.out.endsWith('Buzz\n1.5\n'));
});
