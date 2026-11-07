import { describe, expect, it } from "vitest";
import { generateHandle } from "../lib/identity.js";

describe("generateHandle", () => {
  it("matches the adjective-noun·hex4 shape", () => {
    const handle = generateHandle();
    expect(handle).toMatch(/^[a-z]+-[a-z]+·[0-9a-f]{4}$/);
  });
});
