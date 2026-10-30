// Standard library signatures: name (without '$') -> arity, or 'const' for constants.
// The implementations live in runtime.js; the checker uses this table for
// unknown-name and arity errors.

export const STDLIB = new Map(Object.entries({
  // console (§12.2)
  print: 1, write: 1, eprint: 1, readLine: 0,
  // files (§12.3)
  readFile: 1, writeFile: 2, appendFile: 2, fileExists: 1, listDir: 1, deleteFile: 1,
  // process (§12.4)
  args: 'const', env: 1, programPath: 'const', run: 2,
  // strings and arrays (§9.4, §10)
  length: 1, at: 2, slice: 3, code: 1, fromCode: 1, split: 2, join: 2,
  toPurple: 1, toWah: 1, toInt: 1, toFloat: 1,
  push: 2, pop: 1, same: 2, typeOf: 1,
  // records (§10.10)
  fields: 1,
  // numerics (§11.3)
  floor: 1, ceil: 1, round: 1, trunc: 1, toUint32: 1, isNobody: 1,
}));
