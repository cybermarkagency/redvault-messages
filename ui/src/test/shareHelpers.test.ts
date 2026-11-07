import { describe, expect, it } from "vitest";
import { cleanUploadName } from "../lib/shareTransfer.js";
import { isRisky, likelyPreview, looksLikeText, previewOf, safeFileName, sniff } from "../lib/safePreview.js";
import { archivePath, buildZip, crc32 } from "../lib/zipStore.js";

const enc = (s: string) => new TextEncoder().encode(s);
const blob = (bytes: Uint8Array | string) => new Blob([typeof bytes === "string" ? enc(bytes) : (bytes as BlobPart)]);
const readBlob = (b: Blob): Promise<Uint8Array> =>
  new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(b);
  });

describe("previews never trust the claimed type", () => {
  it("recognises inert formats by their bytes", () => {
    expect(sniff(Uint8Array.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0, 0]))).toEqual({ kind: "image", mime: "image/png" });
    expect(sniff(Uint8Array.from([0xff, 0xd8, 0xff, 0xe0]))).toEqual({ kind: "image", mime: "image/jpeg" });
    expect(sniff(enc("%PDF-1.7\n"))).toEqual({ kind: "pdf", mime: "application/pdf" });
    expect(sniff(Uint8Array.from([0, 0, 0, 0x18, ...enc("ftypisom")]))).toEqual({ kind: "video", mime: "video/mp4" });
    expect(sniff(enc("OggS\0\0"))).toEqual({ kind: "audio", mime: "audio/ogg" });
    expect(sniff(enc("RIFF\0\0\0\0WAVEfmt "))).toEqual({ kind: "audio", mime: "audio/wav" });
  });

  it("never previews active content, whatever it's called", async () => {
    expect(sniff(enc("<!doctype html><script>alert(1)</script>"))).toBeNull();
    expect(sniff(enc('<svg xmlns="http://www.w3.org/2000/svg"><script>alert(1)</script></svg>'))).toBeNull();
    expect(await previewOf("photo.png", blob("<html><script>alert(1)</script></html>"))).toBeNull();
    expect(await previewOf("page.html", blob("<html></html>"))).toBeNull();
    expect(await previewOf("drawing.svg", blob("<svg></svg>"))).toBeNull();
    expect(likelyPreview("page.html", 10)).toBeNull();
    expect(likelyPreview("drawing.svg", 10)).toBeNull();
  });

  it("text is shown only as text, and only if it's really UTF-8 text", async () => {
    const text = await previewOf("notes.txt", blob("<script>alert(1)</script> is just text here"));
    expect(text?.kind).toBe("text");
    expect(text?.blob.type).toBe("text/plain;charset=utf-8");
    expect(await previewOf("notes.txt", blob(Uint8Array.from([0x41, 0, 0x42])))).toBeNull();
    expect(looksLikeText(Uint8Array.from([0xc3, 0x28]))).toBe(false);
  });

  it("a renamed image is re-typed from its bytes", async () => {
    const png = Uint8Array.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3]);
    const preview = await previewOf("not-really.txt", blob(png));
    expect(preview?.kind).toBe("image");
    expect(preview?.blob.type).toBe("image/png");
  });
});

describe("file names", () => {
  it("saves under names that can't escape a folder or disguise themselves", () => {
    expect(safeFileName("../../etc/passwd")).toBe(".._.._etc_passwd".replace(/^[.\s]+/, ""));
    expect(safeFileName("C:\\Windows\\evil.exe")).toBe("C__Windows_evil.exe");
    expect(safeFileName("invoice\u202Etxt.exe")).toBe("invoicetxt.exe");
    expect(safeFileName("CON")).toBe("_CON");
    expect(safeFileName("  ")).toBe("file");
    expect(safeFileName(`${"a".repeat(300)}.pdf`)).toMatch(/^a{196}\.pdf$/);
    expect(cleanUploadName("folder/sub/report.pdf")).toBe("report.pdf");
    expect(cleanUploadName("..\\..\\x\u0000y.txt")).toBe("xy.txt");
  });

  it("flags files that can run code", () => {
    expect(isRisky("setup.EXE")).toBe(true);
    expect(isRisky("page.html")).toBe(true);
    expect(isRisky("macros.docm")).toBe(true);
    expect(isRisky("photo.jpg")).toBe(false);
  });
});

describe("Download all archives", () => {
  it("computes standard CRC-32", () => {
    expect(crc32(enc("hello"))).toBe(0x3610a686);
    expect(crc32(new Uint8Array())).toBe(0);
  });

  it("keeps paths inside the archive", () => {
    expect(archivePath("../../a/./b/../c.txt")).toBe("a/b/c.txt");
    expect(archivePath("/abs/x")).toBe("abs/x");
  });

  it("writes a valid stored ZIP with UTF-8 names and no clashing paths", async () => {
    const zip = await buildZip([
      { path: "Drafts/notes.txt", data: blob("hello"), modified: Date.UTC(2026, 9, 20, 12, 0, 0) },
      { path: "résumé.txt", data: blob("world!"), modified: 0 },
      { path: "Drafts/notes.txt", data: blob("again"), modified: 0 },
    ]);
    const bytes = await readBlob(zip);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    expect(view.getUint32(0, true)).toBe(0x04034b50);
    expect(view.getUint16(6, true) & 0x0800).toBe(0x0800);
    expect(view.getUint32(14, true)).toBe(0x3610a686);
    const end = bytes.length - 22;
    expect(view.getUint32(end, true)).toBe(0x06054b50);
    expect(view.getUint16(end + 10, true)).toBe(3);
    // Walk the central directory and read every name back.
    let offset = view.getUint32(end + 16, true);
    const names: string[] = [];
    for (let i = 0; i < 3; i += 1) {
      expect(view.getUint32(offset, true)).toBe(0x02014b50);
      const nameLength = view.getUint16(offset + 28, true);
      names.push(new TextDecoder().decode(bytes.subarray(offset + 46, offset + 46 + nameLength)));
      offset += 46 + nameLength;
    }
    expect(names).toEqual(["Drafts/notes.txt", "résumé.txt", "Drafts/notes (2).txt"]);
  });
});
