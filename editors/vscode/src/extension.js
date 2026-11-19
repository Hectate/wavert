// The Wavert extension's code: hovers and completions that show what a name
// is and its documentation, for the file's own declarations, the libraries it
// imports (including the standard library, '$lib/'), and the stdlib's $names.
// Completing a function writes out its call, and parameter hints show its
// parameters inside a call's arguments.
// The grammar, brackets and snippets are declared in package.json.

'use strict';

const fs = require('fs');
const path = require('path');
const vscode = require('vscode');
const docs = require('./docs');
const builtins = require('../builtins.json');

// The documentation of stdlib name, or undefined: only builtins.json's own
// entries, so that $toString is found and $constructor is not.
const builtin = (name) => (Object.hasOwn(builtins, name) ? builtins[name] : undefined);

// The text of a file: from its editor if it is open, so unsaved changes count.
function textOf(file) {
  const open = vscode.workspace.textDocuments.find((d) => d.uri.fsPath === file);
  if (open) return open.getText();
  try { return fs.readFileSync(file, 'utf8'); } catch { return null; }
}

function libDir(file) {
  const config = vscode.workspace.getConfiguration('wavert');
  return docs.findLib({ libPath: config.get('libPath'), wvcPath: config.get('wvcPath'), file });
}

// The module an import name stands for in the file: { file, label, text }, or null.
function moduleOf(file, text, name) {
  const imp = docs.imports(text).find((i) => i.name === name);
  if (!imp) return null;
  const target = docs.resolveImport(file, imp.path, imp.path.startsWith('$') ? libDir(file) : null);
  const targetText = target && textOf(target);
  return targetText == null ? null : { file: target, label: imp.path, text: targetText };
}

// Markdown for a declaration, and where it comes from.
function describe(decl, from) {
  const md = new vscode.MarkdownString();
  md.appendCodeblock(decl.signature, 'wavert');
  if (decl.doc) appendDoc(md, decl.doc);
  if (from) md.appendMarkdown(`*from* \`${from}\``);
  return md;
}

// Markdown for a stdlib name.
function describeBuiltin(name, b) {
  const md = new vscode.MarkdownString();
  md.appendCodeblock(b.call === 'constant' ? `$${name}` : b.call.replace(/^;/, ''), 'wavert');
  md.appendMarkdown(`${b.notes}\n\n*standard library,* §${b.section}`);
  return md;
}

// Comment text is shown as Markdown: its lines run together into paragraphs,
// an empty comment line starts a new one, "- " starts a list item, and
// `backticks` mark code. Other Markdown characters are shown as they are.
const escape = (text) => text.replace(/([\\*_[\]#<>])/g, '\\$1');

// Adds documentation to md: lines that are Wavert statements (starting with
// ';'), with the indented lines that continue them, become code blocks, and
// the rest prose.
function appendDoc(md, text) {
  const lines = text.split('\n');
  for (let i = 0; i < lines.length; ) {
    const code = lines[i].startsWith(';');
    let j = i + 1;
    while (j < lines.length && (code ? /^(;|\s+\S)/.test(lines[j]) : !lines[j].startsWith(';'))) j++;
    const chunk = lines.slice(i, j).join('\n');
    if (code) md.appendCodeblock(chunk, 'wavert');
    else md.appendMarkdown(escape(chunk) + '\n\n');
    i = j;
  }
}

function hover(document, position) {
  const line = document.lineAt(position.line).text;
  const at = docs.nameAt(line, position.character);
  if (!at) return null;
  const { parts, index } = at;
  const file = document.uri.fsPath;
  const text = document.getText();
  if (parts[0].startsWith('$')) {
    const b = builtin(parts[0].slice(1));
    return index === 0 && b ? new vscode.Hover(describeBuiltin(parts[0].slice(1), b)) : null;
  }
  const mod = moduleOf(file, text, parts[0]);
  if (mod) {
    if (index === 0) {
      const md = new vscode.MarkdownString();
      md.appendCodeblock(`import ${parts[0]} from "${mod.label}"`, 'wavert');
      const head = docs.header(mod.text);
      if (head) appendDoc(md, head);
      return new vscode.Hover(md);
    }
    // module.name, or module.Enum.member, which shows the enum.
    const decl = docs.declarations(mod.text).find((d) => d.exported && d.name === parts[1]);
    return decl ? new vscode.Hover(describe(decl, mod.label)) : null;
  }
  // A name of this file, or Enum.member of one of its enums.
  if (index > 0 && !(index === 1 && parts.length === 2)) return null;
  const decl = docs.declarations(text).find((d) => d.name === parts[0] && (index === 0 || d.kind === 'enum'));
  return decl ? new vscode.Hover(describe(decl, null)) : null;
}

const kinds = {
  fn: vscode.CompletionItemKind.Function,
  enum: vscode.CompletionItemKind.Enum,
  const: vscode.CompletionItemKind.Constant,
  var: vscode.CompletionItemKind.Variable,
  ref: vscode.CompletionItemKind.Variable,
};

// Whether a name written at start would be the function of a call already
// written, as in 'xs(grid.di', rather than start a new expression.
const isCallee = (document, start) => docs.calleeBefore(document.getText(new vscode.Range(new vscode.Position(0, 0), start)));

// Makes item, a function written in range up to the cursor, insert a whole
// call with a placeholder for each argument: 'grid.di' becomes
// '(a, b)(grid.dist)', with a selected and Tab moving on to b. Not if callee.
function insertCall(item, range, callee, name, params) {
  if (callee) return item;
  item.range = range;
  item.filterText = name;
  item.insertText = new vscode.SnippetString(docs.callSnippet(name, params));
  if (params.length > 1) item.command = { command: 'editor.action.triggerParameterHints', title: 'Parameter hints' };
  return item;
}

function completions(document, position) {
  const before = document.lineAt(position.line).text.slice(0, position.character);
  const at = (back) => position.translate(0, -back);
  // $names: replace from the '$', since it is not part of a word.
  const sigil = /\$([A-Za-z0-9_]*)$/.exec(before);
  if (sigil) {
    const range = new vscode.Range(at(sigil[0].length), position);
    const callee = isCallee(document, range.start);
    return Object.entries(builtins).map(([name, b]) => {
      const constant = b.arity === 'constant';
      const item = new vscode.CompletionItem(`$${name}`, constant ? vscode.CompletionItemKind.Constant : vscode.CompletionItemKind.Function);
      item.range = range;
      item.detail = constant ? 'constant' : b.call.replace(/^;/, '');
      item.documentation = describeBuiltin(name, b);
      return constant ? item : insertCall(item, range, callee, `$${name}`, docs.builtinParameters(b.call));
    });
  }
  const names = docs.qualifierBefore(before);
  if (!names) return /(?<![A-Za-z0-9_$.])[A-Za-z0-9_]*$/.test(before) ? bareNames(document, position) : null;
  if (names.length > 2) return null;
  const file = document.uri.fsPath;
  const text = document.getText();
  const mod = moduleOf(file, text, names[0]);
  // module.: what the module exports, functions written out as calls.
  if (names.length === 1 && mod) {
    const range = new vscode.Range(at(/[A-Za-z0-9_.]*$/.exec(before)[0].length), position);
    const callee = isCallee(document, range.start);
    return docs.declarations(mod.text).filter((d) => d.exported).map((d) => {
      const item = new vscode.CompletionItem(d.name, kinds[d.kind] ?? vscode.CompletionItemKind.Variable);
      item.detail = d.signature;
      item.documentation = describe(d, mod.label);
      return d.kind === 'fn' ? insertCall(item, range, callee, `${names[0]}.${d.name}`, docs.parameters(d)) : item;
    });
  }
  // Enum. for an enum of this file, or module.Enum. for one the module
  // exports: the enum's members.
  const decl = names.length === 1
    ? docs.declarations(text).find((d) => d.kind === 'enum' && d.name === names[0])
    : mod && docs.declarations(mod.text).find((d) => d.exported && d.kind === 'enum' && d.name === names[1]);
  if (!decl) return null;
  return docs.enumMembers(decl).map((m) => {
    const item = new vscode.CompletionItem(m, vscode.CompletionItemKind.EnumMember);
    item.detail = decl.signature;
    item.documentation = describe(decl, names.length === 2 ? mod.label : null);
    return item;
  });
}

// Completions for a name with no qualifier: the file's functions, written out
// as calls, its other declarations, its imports, and the words of the file.
// VS Code's own word completions stop once an extension offers any, so the
// words are offered here too.
function bareNames(document, position) {
  const text = document.getText();
  const offset = document.offsetAt(position);
  const typed = /[A-Za-z0-9_]*$/.exec(text.slice(0, offset))[0];
  const start = offset - typed.length;
  // Nothing in comments and strings: whether a letter at start would be code.
  if (!docs.codeMask(text.slice(0, start) + 'a')[start]) return null;
  const range = new vscode.Range(document.positionAt(start), position);
  const callee = docs.calleeBefore(text.slice(0, start));
  const declaring = /;(?:export\s+)?(?:fn|const|var|ref|enum)\s+$/.test(text.slice(Math.max(0, start - 40), start));
  const items = [];
  const seen = new Set();
  for (const d of docs.declarations(text)) {
    if (seen.has(d.name)) continue;
    seen.add(d.name);
    const item = new vscode.CompletionItem(d.name, kinds[d.kind] ?? vscode.CompletionItemKind.Variable);
    item.detail = d.signature;
    item.documentation = describe(d, null);
    items.push(d.kind === 'fn' ? insertCall(item, range, callee || declaring, d.name, docs.parameters(d)) : item);
  }
  for (const imp of docs.imports(text)) {
    if (seen.has(imp.name)) continue;
    seen.add(imp.name);
    const item = new vscode.CompletionItem(imp.name, vscode.CompletionItemKind.Module);
    item.detail = `import ${imp.name} from "${imp.path}"`;
    items.push(item);
  }
  // Words, other than the one being typed, unless it is also written elsewhere.
  const counts = new Map();
  for (const m of text.matchAll(/(?<![A-Za-z0-9_$.])[A-Za-z_][A-Za-z0-9_]*/g)) counts.set(m[0], (counts.get(m[0]) ?? 0) + 1);
  for (const [word, n] of counts) {
    if (seen.has(word) || (word === typed && n === 1)) continue;
    items.push(new vscode.CompletionItem(word, vscode.CompletionItemKind.Text));
  }
  return items;
}

// Describes the function of a call: { label, ranges, doc }, or null.
function functionOf(document, name) {
  if (name.startsWith('$')) {
    const b = builtin(name.slice(1));
    if (!b || b.arity === 'constant') return null;
    return { ...docs.callSignature(name, docs.builtinParameters(b.call)), doc: describeBuiltin(name.slice(1), b) };
  }
  const text = document.getText();
  const [first, second] = name.split('.');
  let decl;
  let from = null;
  if (second) {
    const mod = moduleOf(document.uri.fsPath, text, first);
    decl = mod && docs.declarations(mod.text).find((d) => d.exported && d.kind === 'fn' && d.name === second);
    from = mod?.label;
  } else {
    decl = docs.declarations(text).find((d) => d.kind === 'fn' && d.name === first);
  }
  if (!decl) return null;
  const doc = new vscode.MarkdownString();
  if (decl.doc) appendDoc(doc, decl.doc);
  if (from) doc.appendMarkdown(`*from* \`${from}\``);
  return { ...docs.callSignature(name, docs.parameters(decl)), doc };
}

// Parameter hints: inside the arguments of a call that has its function
// written after them, as in '(1, |)(grid.pos)', the function's parameters,
// with the one at the cursor marked.
function signatureHelp(document, position) {
  const call = docs.callAround(document.getText(), document.offsetAt(position));
  const fn = call && functionOf(document, call.name);
  if (!fn) return null;
  const sig = new vscode.SignatureInformation(fn.label, fn.doc);
  sig.parameters = fn.ranges.map((r) => new vscode.ParameterInformation(r));
  const help = new vscode.SignatureHelp();
  help.signatures = [sig];
  help.activeSignature = 0;
  help.activeParameter = call.active;
  return help;
}

function activate(context) {
  const selector = { language: 'wavert' };
  context.subscriptions.push(
    vscode.languages.registerHoverProvider(selector, { provideHover: hover }),
    vscode.languages.registerCompletionItemProvider(selector, { provideCompletionItems: completions }, '.', '$'),
    vscode.languages.registerSignatureHelpProvider(selector, { provideSignatureHelp: signatureHelp }, {
      triggerCharacters: ['(', ','],
      retriggerCharacters: [','],
    }),
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
