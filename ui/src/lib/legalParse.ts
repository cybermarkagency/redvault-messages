// Parses the Terms of Service and Privacy Policy (legal/*.md at the repository
// root). No imports, so the build (vite.config.ts) can use it as well as the app.

/** One block of a document; the files use only these Markdown basics. */
export type LegalBlock =
  | { kind: "title" | "heading" | "subheading" | "paragraph"; text: string }
  | { kind: "bullets" | "numbered"; items: string[] };

/** "# " title, "## " and "### " headings, "- " and "1. " list items; any other line is a paragraph. */
export function parseLegalDocument(markdown: string): LegalBlock[] {
  const blocks: LegalBlock[] = [];
  const addItem = (kind: "bullets" | "numbered", item: string) => {
    const last = blocks[blocks.length - 1];
    if (last?.kind === kind) last.items.push(item);
    else blocks.push({ kind, items: [item] });
  };
  for (const raw of markdown.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    const numbered = /^\d+\.\s+(.*)$/.exec(line);
    if (line.startsWith("### ")) blocks.push({ kind: "subheading", text: line.slice(4) });
    else if (line.startsWith("## ")) blocks.push({ kind: "heading", text: line.slice(3) });
    else if (line.startsWith("# ")) blocks.push({ kind: "title", text: line.slice(2) });
    else if (line.startsWith("- ")) addItem("bullets", line.slice(2));
    else if (numbered) addItem("numbered", numbered[1]);
    else blocks.push({ kind: "paragraph", text: line });
  }
  return blocks;
}
