// Generates builtins.json, the documentation the extension shows for the
// standard library's $names, from the specification's tables (docs/wavert-spec.md)
// and the arities in the compiler's checker (wvc/checker.wvt).
//
// Usage: node editors/vscode/scripts/builtins.mjs
// wvc/test/grammar.test.js fails until builtins.json is regenerated after a change.

import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const repo = new URL('../../../', import.meta.url);
const read = (path) => readFileSync(new URL(path, repo), 'utf8').replaceAll('\r\n', '\n');

// { name: { call, notes, arity, section } } for every stdlib name, from a
// table row "| `$name` | call | notes |" whose call is a call (";...") or
// "constant"; the summary table (§12.6), whose second cell is an arity, is skipped.
export function builtins(spec, checker) {
  const arity = new Map([...checker.matchAll(/\{ name: "(\w+)", arity: ("const"|\d+) \}/g)]
    .map((m) => [m[1], m[2] === '"const"' ? 'constant' : Number(m[2])]));
  // No prototype, so that a name such as toString is not already there.
  const out = Object.create(null);
  let section = '';
  for (const line of spec.split('\n')) {
    const h = /^### (\d+\.\d+) /.exec(line);
    if (h) section = h[1];
    const m = /^\| `\$(\w+)` \| (`;[^|]*`|constant) \| (.*) \|$/.exec(line);
    if (m && arity.has(m[1]) && !out[m[1]]) {
      // "(below)" means the text after the table, which a hover does not show.
      const notes = m[3].replaceAll('(below)', `(§${section})`);
      out[m[1]] = { call: m[2].replace(/^`|`$/g, ''), notes, arity: arity.get(m[1]), section };
    }
  }
  return Object.fromEntries(Object.entries(out).sort(([a], [b]) => (a < b ? -1 : 1)));
}

export const generated = () => JSON.stringify(builtins(read('docs/wavert-spec.md'), read('wvc/checker.wvt')), null, 2) + '\n';

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  writeFileSync(new URL('editors/vscode/builtins.json', repo), generated());
}
