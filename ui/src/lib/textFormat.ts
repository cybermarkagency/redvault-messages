// Message formatting: WhatsApp-style markers inside the ordinary text, so an
// app that doesn't render them still shows something readable. The same
// rules as the desktop app (desktop/core/.../model/text_format.hpp):
//
//   *bold*   _italic_   ~strikethrough~   `monospace`   ||spoiler||
//
// plus http(s) links, and "@handle" mentions of a group's members. A marker
// only opens after the start of a line, a space or an opening bracket/quote
// and only closes before the end, a space or punctuation, so snake_case,
// 2*3*4 and file_names stay as typed. Nothing inside `monospace` is
// formatted, and markers never span lines.
//
// The result is a tree of nodes, never HTML: the text is untrusted, and the
// React renderer (components/FormattedText.tsx) escapes everything.

export type FormatNode =
  | string
  | { kind: "b" | "i" | "s" | "spoiler"; children: FormatNode[] }
  | { kind: "code"; text: string }
  | { kind: "link"; url: string }
  | { kind: "mention"; text: string; self: boolean };

export interface FormatOptions {
  /** Handles "@handle" highlights (a group's members), matched ignoring ASCII case, longest first. */
  mentions?: string[];
  selfHandle?: string;
}

// Messages are untrusted: a formatted span is at most this long, and styles
// nest at most this deep (deeper markers are shown as typed).
const MAX_SPAN = 4000;
const MAX_DEPTH = 6;

type Style = "b" | "i" | "s" | "spoiler";
// "||" before the one-character markers, so a spoiler isn't read as two empty ones.
const MARKERS: [string, Style][] = [
  ["||", "spoiler"],
  ["*", "b"],
  ["_", "i"],
  ["~", "s"],
];

const isSpace = (c: string) => c === " " || c === "\t" || c === "\n" || c === "\r";
/** Letters, digits and anything non-ASCII (part of a word in another script). */
const isWordChar = (c: string) => c.charCodeAt(0) >= 0x80 || /[A-Za-z0-9]/.test(c);
const canOpenAfter = (s: string, i: number) => i === 0 || isSpace(s[i - 1]) || `([{"'`.includes(s[i - 1]);
const canCloseBefore = (s: string, j: number) => j >= s.length || isSpace(s[j]) || !isWordChar(s[j]);

/** End of an http(s) link starting at `i`, or `i` if there's none. */
function linkEnd(s: string, i: number): number {
  if (i > 0 && isWordChar(s[i - 1])) return i;
  const lower = s.slice(i, i + 8).toLowerCase();
  const scheme = lower.startsWith("https://") ? 8 : lower.startsWith("http://") ? 7 : 0;
  if (!scheme) return i;
  let end = i + scheme;
  while (end < s.length && !isSpace(s[end]) && !`<>"\``.includes(s[end])) end++;
  // Sentence punctuation and formatting markers right after a link aren't part of it.
  let open = 0;
  for (let k = i; k < end; k++) open += s[k] === "(" ? 1 : s[k] === ")" ? -1 : 0;
  while (end > i + scheme) {
    const last = s[end - 1];
    if (last === ")" && open < 0) {
      open++;
      end--;
    } else if (".,;:!?'*_~|".includes(last)) {
      end--;
    } else {
      break;
    }
  }
  return end > i + scheme ? end : i;
}

function findClose(s: string, i: number, token: string): number {
  const limit = Math.min(s.length, i + token.length + MAX_SPAN);
  for (let j = i + token.length + 1; j + token.length <= limit; j++) {
    if (s[j] === "\n") return -1;
    if (s.startsWith(token, j) && !isSpace(s[j - 1]) && canCloseBefore(s, j + token.length)) return j;
  }
  return -1;
}

/** Length of the handle the "@" at `i` mentions (the longest that fits), and which, or null. */
function mentionAt(s: string, i: number, handles: string[]): string | null {
  if (s[i] !== "@" || (i > 0 && isWordChar(s[i - 1]))) return null;
  let best: string | null = null;
  for (const handle of handles) {
    if (!handle || (best && handle.length <= best.length)) continue;
    const candidate = s.slice(i + 1, i + 1 + handle.length);
    if (candidate.length !== handle.length || candidate.toLowerCase() !== handle.toLowerCase()) continue;
    const after = i + 1 + handle.length;
    if (after < s.length && isWordChar(s[after])) continue;
    best = candidate;
  }
  return best;
}

export function parseFormatted(text: string, options: FormatOptions = {}): FormatNode[] {
  const handles = options.mentions ?? [];
  const self = options.selfHandle?.toLowerCase();
  const render = (s: string, depth: number): FormatNode[] => {
    const out: FormatNode[] = [];
    const push = (t: string) => {
      if (!t) return;
      if (typeof out[out.length - 1] === "string") out[out.length - 1] = (out[out.length - 1] as string) + t;
      else out.push(t);
    };
    let pos = 0;
    let i = 0;
    while (i < s.length) {
      if (s[i] === "`") {
        const close = s.indexOf("`", i + 1);
        const newline = s.indexOf("\n", i + 1);
        if (close > i + 1 && (newline < 0 || close < newline)) {
          push(s.slice(pos, i));
          out.push({ kind: "code", text: s.slice(i + 1, close) });
          i = pos = close + 1;
          continue;
        }
      }
      const end = linkEnd(s, i);
      if (end > i) {
        push(s.slice(pos, i));
        out.push({ kind: "link", url: s.slice(i, end) });
        i = pos = end;
        continue;
      }
      if (s[i] === "@" && handles.length > 0) {
        const handle = mentionAt(s, i, handles);
        if (handle) {
          push(s.slice(pos, i));
          out.push({ kind: "mention", text: `@${handle}`, self: self !== undefined && handle.toLowerCase() === self });
          i = pos = i + 1 + handle.length;
          continue;
        }
      }
      let matched = false;
      if (depth < MAX_DEPTH) {
        for (const [token, style] of MARKERS) {
          if (!s.startsWith(token, i) || !canOpenAfter(s, i)) continue;
          const inner = i + token.length;
          if (inner >= s.length || isSpace(s[inner]) || s[inner] === token[0]) continue;
          const close = findClose(s, i, token);
          if (close < 0) continue;
          push(s.slice(pos, i));
          const body = s.slice(inner, close);
          // A spoiler's content is kept plain: nothing nested shows through while it's hidden.
          out.push({ kind: style, children: style === "spoiler" ? [body] : render(body, depth + 1) });
          i = pos = close + token.length;
          matched = true;
          break;
        }
      }
      if (!matched) i++;
    }
    push(s.slice(pos));
    return out;
  };
  return render(text, 0);
}

/** Whether the text has anything to render beyond plain characters. */
export function hasFormatting(text: string, options: FormatOptions = {}): boolean {
  return parseFormatted(text, options).some((n) => typeof n !== "string");
}

/** The text a reader sees, markers removed and spoilers hidden (for previews and notifications). */
export function toPlain(text: string): string {
  const flatten = (nodes: FormatNode[]): string =>
    nodes
      .map((n) => {
        if (typeof n === "string") return n;
        if (n.kind === "code" || n.kind === "mention") return n.text;
        if (n.kind === "link") return n.url;
        if (n.kind === "spoiler") return "•••";
        return flatten(n.children);
      })
      .join("");
  return flatten(parseFormatted(text));
}

/** Whether `text` mentions `handle` ("@handle", by the same rule as above). */
export function mentions(text: string, handle: string): boolean {
  return parseFormatted(text, { mentions: [handle] }).some((n) => typeof n !== "string" && n.kind === "mention");
}
