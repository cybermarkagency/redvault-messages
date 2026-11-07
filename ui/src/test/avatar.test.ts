import { describe, expect, it } from "vitest";
import { readImageAsAvatar } from "../lib/avatar.js";

describe("readImageAsAvatar validation", () => {
  it("rejects a non-image file before ever touching the filesystem/canvas", async () => {
    const file = new File(["not an image"], "notes.txt", { type: "text/plain" });
    await expect(readImageAsAvatar(file)).rejects.toThrow(/not an image/i);
  });

  it("rejects a file over the size cap", async () => {
    const bigBytes = new Uint8Array(10 * 1024 * 1024 + 1);
    const file = new File([bigBytes], "huge.png", { type: "image/png" });
    await expect(readImageAsAvatar(file)).rejects.toThrow(/too large/i);
  });
});
