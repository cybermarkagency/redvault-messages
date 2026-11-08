// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { type ReactNode, useState } from "react";
import { type FormatNode, type FormatOptions, parseFormatted } from "../lib/textFormat.js";
import styles from "./FormattedText.module.css";

/**
 * A message's text with its formatting (lib/textFormat.ts). Built from React
 * elements only, so nothing the sender typed can become markup. Links ask
 * before leaving the app; a spoiler shows once clicked.
 */
export function FormattedText({ text, options }: { text: string; options?: FormatOptions }) {
  const [revealed, setRevealed] = useState(false);

  const openLink = (event: React.MouseEvent, url: string) => {
    event.preventDefault();
    // Show the whole address first: a link's text can look like one site and go to another.
    if (!/^https?:\/\//i.test(url)) return;
    if (window.confirm(`Open this link in a new tab?\n\n${url}\n\nCheck the address first: a link can look like one site and go to another.`)) {
      window.open(url, "_blank", "noopener,noreferrer");
    }
  };

  const render = (nodes: FormatNode[], keyPrefix: string): ReactNode[] =>
    nodes.map((node, index) => {
      const key = `${keyPrefix}${index}`;
      if (typeof node === "string") return node;
      switch (node.kind) {
        case "b":
          return <strong key={key}>{render(node.children, `${key}-`)}</strong>;
        case "i":
          return <em key={key}>{render(node.children, `${key}-`)}</em>;
        case "s":
          return <s key={key}>{render(node.children, `${key}-`)}</s>;
        case "code":
          return (
            <code key={key} className={styles.code}>
              {node.text}
            </code>
          );
        case "spoiler":
          return revealed ? (
            <span key={key} className={styles.spoilerShown}>
              {render(node.children, `${key}-`)}
            </span>
          ) : (
            <button key={key} type="button" className={styles.spoiler} aria-label="Spoiler, click to show" onClick={() => setRevealed(true)}>
              {render(node.children, `${key}-`)}
            </button>
          );
        case "link":
          return (
            <a key={key} href={node.url} className={styles.link} rel="noopener noreferrer" target="_blank" onClick={(e) => openLink(e, node.url)}>
              {node.url}
            </a>
          );
        case "mention":
          return (
            <span key={key} className={node.self ? styles.mentionSelf : styles.mention}>
              {node.text}
            </span>
          );
      }
    });

  return <>{render(parseFormatted(text, options), "f")}</>;
}
