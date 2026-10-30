// Source positions and diagnostics shared by every compiler phase.

export class Source {
  constructor(file, text) {
    this.file = file;
    this.text = text;
    this.lineStarts = [0];
    for (let i = 0; i < text.length; i++) if (text[i] === '\n') this.lineStarts.push(i + 1);
  }

  loc(pos) {
    let lo = 0, hi = this.lineStarts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (this.lineStarts[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return { line: lo + 1, col: pos - this.lineStarts[lo] + 1 };
  }

  lineText(line) {
    const start = this.lineStarts[line - 1];
    const next = this.lineStarts[line];
    const end = next === undefined ? this.text.length : next - 1;
    return this.text.slice(start, end).replace(/\r$/, '');
  }
}

// Thrown by any phase; carries one or more diagnostics.
export class WahError extends Error {
  constructor(diags) {
    super(diags.map((d) => d.message).join('\n'));
    this.diags = diags;
  }
}

export const err = (pos, message, hint) => ({ severity: 'error', pos, message, hint });
export const warn = (pos, message, hint) => ({ severity: 'warning', pos, message, hint });

// src is the file the diagnostic is in, unless the diagnostic names its own (d.src).
export function formatDiag(d, src) {
  src = d.src ?? src;
  const { line, col } = src.loc(d.pos);
  let s = `${src.file}:${line}:${col}: ${d.severity}: ${d.message}`;
  if (d.hint) s += `\n  hint: ${d.hint}`;
  const text = src.lineText(line);
  const pad = text.slice(0, col - 1).replace(/[^\t]/g, ' ');
  return `${s}\n    ${text}\n    ${pad}^`;
}
