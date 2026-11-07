// Deterministic visual identity: key glyphs and safety numbers are derived
// on demand from a seed string — nothing is stored. In this prototype the
// seed is a display handle; a production build must seed from the actual
// public key material instead (see the component docs in KeyGlyph.tsx /
// SafetyNumber.tsx).

export function fnv1aHash(input: string): number {
  let hash = 0x811c9dc5;
  for (let i = 0; i < input.length; i++) {
    hash ^= input.charCodeAt(i);
    hash = Math.imul(hash, 0x01000193);
  }
  return hash >>> 0;
}

/** mulberry32: small, fast, deterministic PRNG from a 32-bit seed. */
export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return function next() {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

export function rngFromSeed(seed: string): () => number {
  return mulberry32(fnv1aHash(seed));
}

export type GlyphTone = "accent" | "text-3";

export interface GlyphCell {
  filled: boolean;
  tone: GlyphTone;
}

export type GlyphGrid = GlyphCell[][]; // 5x5, indexed [row][col]

const GRID_SIZE = 5;
const CENTER_COL = 2;
const FILL_PROBABILITY = 0.5;
const ACCENT_PROBABILITY = 0.72;

/**
 * A deterministic 5x5 grid seeded from `seed`, mirrored left-right
 * (columns 0<->4, 1<->3, center column 2 unmirrored) so it always reads as
 * a symmetric mark. Filled cells are ~72% --accent, ~28% --text-3.
 */
export function generateKeyGlyph(seed: string): GlyphGrid {
  const rng = rngFromSeed(seed);
  const grid: GlyphCell[][] = Array.from({ length: GRID_SIZE }, () =>
    new Array<GlyphCell>(GRID_SIZE)
  );

  for (let row = 0; row < GRID_SIZE; row++) {
    for (let col = 0; col <= CENTER_COL; col++) {
      const cell: GlyphCell = {
        filled: rng() < FILL_PROBABILITY,
        tone: rng() < ACCENT_PROBABILITY ? "accent" : "text-3",
      };
      grid[row][col] = cell;
      if (col !== CENTER_COL) {
        grid[row][GRID_SIZE - 1 - col] = { ...cell };
      }
    }
  }
  return grid;
}

