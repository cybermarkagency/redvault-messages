import { describe, expect, it } from "vitest";
import { fnv1aHash, generateKeyGlyph, mulberry32 } from "../lib/glyph.js";

describe("fnv1aHash", () => {
  it("is deterministic", () => {
    expect(fnv1aHash("quiet-harbor·4f2a")).toBe(fnv1aHash("quiet-harbor·4f2a"));
  });

  it("differs for different inputs", () => {
    expect(fnv1aHash("a")).not.toBe(fnv1aHash("b"));
  });
});

describe("mulberry32", () => {
  it("produces the same sequence for the same seed", () => {
    const a = mulberry32(42);
    const b = mulberry32(42);
    expect([a(), a(), a()]).toEqual([b(), b(), b()]);
  });

  it("produces values in [0, 1)", () => {
    const rng = mulberry32(1234);
    for (let i = 0; i < 50; i++) {
      const v = rng();
      expect(v).toBeGreaterThanOrEqual(0);
      expect(v).toBeLessThan(1);
    }
  });
});

describe("generateKeyGlyph", () => {
  it("is deterministic for the same seed", () => {
    expect(generateKeyGlyph("quiet-harbor")).toEqual(generateKeyGlyph("quiet-harbor"));
  });

  it("differs for a different seed", () => {
    expect(generateKeyGlyph("quiet-harbor")).not.toEqual(generateKeyGlyph("salt-lantern"));
  });

  it("is a 5x5 grid", () => {
    const grid = generateKeyGlyph("quiet-harbor");
    expect(grid).toHaveLength(5);
    grid.forEach((row) => expect(row).toHaveLength(5));
  });

  it("is mirrored left-right per row (columns 0<->4, 1<->3)", () => {
    const grid = generateKeyGlyph("quiet-harbor");
    for (const row of grid) {
      expect(row[0]).toEqual(row[4]);
      expect(row[1]).toEqual(row[3]);
    }
  });
});
