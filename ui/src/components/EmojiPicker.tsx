// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useMemo, useRef, useState } from "react";
import type { EmojiCategory } from "../lib/emojiData.js";
import styles from "./EmojiPicker.module.css";

export interface EmojiPickerProps {
  onPick: (emoji: string) => void;
  onClose: () => void;
  /** Line up with the right edge of its container (for your own messages, on the right). */
  alignEnd?: boolean;
}

// Recently picked emoji, for this tab only: never stored anywhere.
let recent: string[] = [];

/** Every emoji, searchable by name and grouped by category; the data loads the first time a picker opens. */
export function EmojiPicker({ onPick, onClose, alignEnd }: EmojiPickerProps) {
  const [categories, setCategories] = useState<EmojiCategory[] | null>(null);
  const [category, setCategory] = useState(recent.length > 0 ? -1 : 0);
  const [query, setQuery] = useState("");
  const root = useRef<HTMLDivElement>(null);
  const search = useRef<HTMLInputElement>(null);

  useEffect(() => {
    let live = true;
    import("../lib/emojiData.js").then((m) => live && setCategories(m.EMOJI_CATEGORIES));
    search.current?.focus({ preventScroll: true });
    return () => {
      live = false;
    };
  }, []);

  // Bring the whole picker into view once it has its full height.
  useEffect(() => {
    root.current?.scrollIntoView?.({ block: "nearest" });
  }, [categories]);

  // Escape or a click elsewhere closes it.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    const onDown = (e: MouseEvent) => root.current && !root.current.contains(e.target as Node) && onClose();
    document.addEventListener("keydown", onKey);
    document.addEventListener("mousedown", onDown);
    return () => {
      document.removeEventListener("keydown", onKey);
      document.removeEventListener("mousedown", onDown);
    };
  }, [onClose]);

  const shown = useMemo((): [string, string][] => {
    if (!categories) return [];
    const q = query.trim().toLowerCase();
    if (q) return categories.flatMap((c) => c.emoji).filter(([, name]) => name.includes(q)).slice(0, 200);
    if (category < 0) return recent.map((e) => [e, ""]);
    return categories[category]?.emoji ?? [];
  }, [categories, category, query]);

  function pick(emoji: string) {
    recent = [emoji, ...recent.filter((e) => e !== emoji)].slice(0, 32);
    onPick(emoji);
  }

  return (
    <div ref={root} className={`${styles.picker} ${alignEnd ? styles.alignEnd : ""}`} role="dialog" aria-label="Choose an emoji">
      <input
        ref={search}
        className={styles.search}
        type="search"
        placeholder="Search emoji"
        aria-label="Search emoji"
        value={query}
        onChange={(e) => setQuery(e.target.value)}
      />
      {!query && categories && (
        <div className={styles.tabs} role="tablist">
          {recent.length > 0 && (
            <button type="button" role="tab" aria-selected={category === -1} title="Recent" className={styles.tab} onClick={() => setCategory(-1)}>
              🕘
            </button>
          )}
          {categories.map((c, i) => (
            <button key={c.name} type="button" role="tab" aria-selected={category === i} title={c.name} className={styles.tab} onClick={() => setCategory(i)}>
              {c.emoji[0]?.[0]}
            </button>
          ))}
        </div>
      )}
      <div className={styles.grid}>
        {!categories && <span className={styles.empty}>Loading…</span>}
        {categories && shown.length === 0 && <span className={styles.empty}>No emoji found.</span>}
        {shown.map(([emoji, name]) => (
          <button key={emoji} type="button" className={styles.emoji} title={name || undefined} aria-label={name || emoji} onClick={() => pick(emoji)}>
            {emoji}
          </button>
        ))}
      </div>
    </div>
  );
}
