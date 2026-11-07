import { useMemo } from "react";
import { generateKeyGlyph } from "../lib/glyph.js";
import styles from "./KeyGlyph.module.css";

export interface KeyGlyphProps {
  /**
   * Prototype: a display handle. Production: this must be the person's
   * actual public key material, not a display string — the glyph is meant
   * to be a visual fingerprint of the key, not of the name.
   */
  seed: string;
  size?: number;
}

export function KeyGlyph({ seed, size = 40 }: KeyGlyphProps) {
  const grid = useMemo(() => generateKeyGlyph(seed), [seed]);

  return (
    <div
      className={styles.glyph}
      style={{ width: size, height: size }}
      role="img"
      aria-label={`Key glyph for ${seed}`}
    >
      {grid.map((row, r) =>
        row.map((cell, c) => (
          <div
            key={`${r}-${c}`}
            className={`${styles.cell} ${cell.filled ? (cell.tone === "accent" ? styles.cellAccent : styles.cellTone) : ""}`}
          />
        ))
      )}
    </div>
  );
}
