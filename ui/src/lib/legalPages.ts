// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The public Privacy Policy and Terms of Service pages
// (www.redvaultsystems.com/redvaultmessages/privacy.html and terms.html),
// rendered at build time from legal/*.md, the same text every app shows at
// sign-up (vite.config.ts writes them into the build). Plain static pages:
// no scripts and nothing loaded from anywhere else.
import { parseLegalDocument } from "./legalParse.js";

export type LegalPage = "privacy" | "terms";

const escape = (text: string) =>
  text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");

/** Escaped text with email addresses made into mailto: links. */
function inline(text: string): string {
  return escape(text).replace(/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/g, (email) => `<a href="mailto:${email}">${email}</a>`);
}

/** "15. Account Deletion" → "section-15", so each section can be linked to. */
function headingId(text: string): string | null {
  const number = /^(\d+)\./.exec(text);
  return number ? `section-${number[1]}` : null;
}

/**
 * Not part of the policy: the steps for each app. Google Play's account
 * deletion requirement links here (privacy.html#delete-account).
 */
const DELETE_ACCOUNT = `
  <section class="howto" id="delete-account">
    <h2>How to delete your account</h2>
    <p>You can delete your RedVault Messages account at any time:</p>
    <ul>
      <li><strong>Android app:</strong> Settings → Delete account.</li>
      <li><strong>Desktop app</strong> (Windows and Linux): Settings → Account and plan → Delete my account.</li>
      <li><strong>Web:</strong> sign in at <a href="./">redvaultsystems.com/redvaultmessages</a>, then Settings → Delete account.</li>
    </ul>
    <p>Enter your password to confirm. Your account is deleted straight away and you're signed out everywhere: your private key,
    recovery key, two-factor setup, profile photo and notification tokens are removed. A deleted marker is kept so nobody can take
    over your username. Messages you already sent stay in the recipients' chats, still end-to-end encrypted, and they'll see your
    account as deleted.</p>
    <p>Deleting your account doesn't cancel a card subscription: switch to Free in Settings first (or use the link in your Stripe
    receipt email). If you can't sign in, email <a href="mailto:cybermarkagency@gmail.com">cybermarkagency@gmail.com</a> and we'll help.</p>
  </section>`;

const STYLE = `
  :root { --bg: #f4f0eb; --text: #1f1a17; --muted: #5d544e; --line: #ddd4ca; --accent: #9b3a2e; --box: #ebe4dc; }
  @media (prefers-color-scheme: dark) { :root { --bg: #161311; --text: #eee7e0; --muted: #b3a9a0; --line: #332c27; --accent: #e07a68; --box: #201c19; } }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--text); font: 16px/1.6 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
  main { max-width: 760px; margin: 0 auto; padding: 32px 16px 64px; }
  nav { display: flex; gap: 16px; flex-wrap: wrap; font-size: 0.95rem; margin-bottom: 24px; }
  nav a[aria-current="page"] { color: var(--text); font-weight: 600; text-decoration: none; }
  h1 { font-size: 1.8rem; line-height: 1.2; margin: 0 0 16px; }
  h2 { font-size: 1.2rem; margin: 36px 0 8px; padding-top: 16px; border-top: 1px solid var(--line); }
  h3 { font-size: 1.05rem; margin: 22px 0 6px; }
  p { margin: 0 0 10px; }
  ul, ol { margin: 0 0 12px; padding-left: 24px; }
  a { color: var(--accent); overflow-wrap: anywhere; }
  .howto { margin-top: 40px; padding: 4px 20px 12px; border-radius: 12px; background: var(--box); }
  .howto h2 { border-top: none; padding-top: 12px; margin-top: 8px; }
  footer { margin-top: 40px; padding-top: 16px; border-top: 1px solid var(--line); color: var(--muted); font-size: 0.9rem; }`;

/** A complete, standalone HTML page for the document. */
export function renderLegalPage(page: LegalPage, markdown: string): string {
  const blocks = parseLegalDocument(markdown);
  const titleBlock = blocks.find((b) => b.kind === "title");
  const title = titleBlock && titleBlock.kind === "title" ? titleBlock.text : page === "privacy" ? "Privacy Policy" : "Terms of Service";
  const body = blocks
    .map((block) => {
      switch (block.kind) {
        case "title":
          return `  <h1>${escape(block.text)}</h1>`;
        case "heading": {
          const id = headingId(block.text);
          return `  <h2${id ? ` id="${id}"` : ""}>${escape(block.text)}</h2>`;
        }
        case "subheading":
          return `  <h3>${escape(block.text)}</h3>`;
        case "bullets":
          return `  <ul>\n${block.items.map((item) => `    <li>${inline(item)}</li>`).join("\n")}\n  </ul>`;
        case "numbered":
          return `  <ol>\n${block.items.map((item) => `    <li>${inline(item)}</li>`).join("\n")}\n  </ol>`;
        default:
          return `  <p>${inline(block.text)}</p>`;
      }
    })
    .join("\n");
  const nav = (where: string) => `  <nav aria-label="${where}">
    <a href="privacy.html"${page === "privacy" ? ' aria-current="page"' : ""}>Privacy Policy</a>
    <a href="terms.html"${page === "terms" ? ' aria-current="page"' : ""}>Terms of Service</a>
    <a href="./">Open RedVault Messages</a>
  </nav>`;
  const description =
    page === "privacy"
      ? "What RedVault Messages processes, what it can't read, and how to delete your account."
      : "The terms for using the RedVault Messages encrypted messaging service.";
  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${escape(title)}</title>
<meta name="description" content="${escape(description)}">
<style>${STYLE}
</style>
</head>
<body>
<main>
${nav("Legal documents")}
${body}
${page === "privacy" ? DELETE_ACCOUNT : ""}
  <footer>
${nav("Legal documents (bottom)")}
  </footer>
</main>
</body>
</html>
`;
}
