// The Wavert extension's code: hovers and completions that show what a name
// is and its documentation, for the file's own declarations, the libraries it
// imports (including the standard library, '$lib/'), and the stdlib's $names.
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

function completions(document, position) {
  const before = document.lineAt(position.line).text.slice(0, position.character);
  // $names: replace from the '$', since it is not part of a word.
  const sigil = /\$([A-Za-z0-9_]*)$/.exec(before);
  if (sigil) {
    const range = new vscode.Range(position.line, position.character - sigil[0].length, position.line, position.character);
    return Object.entries(builtins).map(([name, b]) => {
      const item = new vscode.CompletionItem(`$${name}`, b.arity === 'constant' ? vscode.CompletionItemKind.Constant : vscode.CompletionItemKind.Function);
      item.range = range;
      item.detail = b.call === 'constant' ? 'constant' : b.call.replace(/^;/, '');
      item.documentation = describeBuiltin(name, b);
      return item;
    });
  }
  const names = docs.qualifierBefore(before);
  if (!names || names.length > 2) return null;
  const file = document.uri.fsPath;
  const text = document.getText();
  const mod = moduleOf(file, text, names[0]);
  // module.: what the module exports.
  if (names.length === 1 && mod) {
    const kinds = { fn: vscode.CompletionItemKind.Function, enum: vscode.CompletionItemKind.Enum };
    return docs.declarations(mod.text).filter((d) => d.exported).map((d) => {
      const item = new vscode.CompletionItem(d.name, kinds[d.kind] ?? vscode.CompletionItemKind.Variable);
      item.detail = d.signature;
      item.documentation = describe(d, mod.label);
      return item;
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

function activate(context) {
  const selector = { language: 'wavert' };
  context.subscriptions.push(
    vscode.languages.registerHoverProvider(selector, { provideHover: hover }),
    vscode.languages.registerCompletionItemProvider(selector, { provideCompletionItems: completions }, '.', '$'),
  );
}

function deactivate() {}

module.exports = { activate, deactivate };
