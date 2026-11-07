import { blobBytes } from "./blobBytes.js";

// Which shared or uploaded files may be previewed in a browser, and how.
// The type a file claims (its MIME type or name) is never trusted on its
// own: a preview needs the file's first bytes to match a known inert format
// (images, video, audio, PDF) or to be plain UTF-8 text. Active content
// (HTML, SVG, scripts, Office documents with macros) is never rendered:
// those can only be downloaded. Previews always get a fixed, safe MIME type.

export type PreviewKind = "image" | "video" | "audio" | "pdf" | "text";

/** Bytes needed from the start of a file to recognise it. */
export const SNIFF_BYTES = 64;
/** Largest file previewed as text. */
export const PREVIEW_TEXT_MAX = 2 * 1024 * 1024;
/** Largest file decrypted just to preview it (images, video, audio, PDF). */
export const PREVIEW_MAX = 300 * 1024 * 1024;

const TEXT_EXTENSIONS = new Set(["txt", "md", "markdown", "csv", "tsv", "log", "json", "yaml", "yml", "ini", "conf", "cfg", "toml", "srt", "vtt"]);

function startsWith(bytes: Uint8Array, signature: number[], offset = 0): boolean {
  if (bytes.length < offset + signature.length) return false;
  return signature.every((b, i) => bytes[offset + i] === b);
}

const ascii = (bytes: Uint8Array, start: number, end: number) => String.fromCharCode(...bytes.subarray(start, end));

/** The inert format these bytes really are, with the MIME type a preview gets. Null when it's anything else. */
export function sniff(head: Uint8Array): { kind: Exclude<PreviewKind, "text">; mime: string } | null {
  if (startsWith(head, [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a])) return { kind: "image", mime: "image/png" };
  if (startsWith(head, [0xff, 0xd8, 0xff])) return { kind: "image", mime: "image/jpeg" };
  if (ascii(head, 0, 6) === "GIF87a" || ascii(head, 0, 6) === "GIF89a") return { kind: "image", mime: "image/gif" };
  if (ascii(head, 0, 4) === "RIFF" && ascii(head, 8, 12) === "WEBP") return { kind: "image", mime: "image/webp" };
  if (ascii(head, 0, 2) === "BM" && head.length >= 14) return { kind: "image", mime: "image/bmp" };
  if (ascii(head, 4, 8) === "ftyp") {
    const brand = ascii(head, 8, 12);
    if (brand === "avif" || brand === "avis") return { kind: "image", mime: "image/avif" };
    if (brand.startsWith("M4A") || brand === "M4B ") return { kind: "audio", mime: "audio/mp4" };
    if (brand === "qt  ") return { kind: "video", mime: "video/quicktime" };
    if (/^(isom|iso[2-9]|mp4[12]|avc1|dash|MSNV|M4V |3gp[4-9]|3g2a)$/.test(brand)) return { kind: "video", mime: "video/mp4" };
    return null;
  }
  if (startsWith(head, [0x1a, 0x45, 0xdf, 0xa3])) return { kind: "video", mime: "video/webm" };
  if (ascii(head, 0, 4) === "OggS") return { kind: "audio", mime: "audio/ogg" };
  if (ascii(head, 0, 4) === "fLaC") return { kind: "audio", mime: "audio/flac" };
  if (ascii(head, 0, 4) === "RIFF" && ascii(head, 8, 12) === "WAVE") return { kind: "audio", mime: "audio/wav" };
  if (ascii(head, 0, 3) === "ID3" || (head.length >= 2 && head[0] === 0xff && (head[1] & 0xe0) === 0xe0 && (head[1] & 0x06) !== 0)) {
    return { kind: "audio", mime: "audio/mpeg" };
  }
  if (ascii(head, 0, 5) === "%PDF-") return { kind: "pdf", mime: "application/pdf" };
  return null;
}

/** What a preview of this file would be, from its name and size alone (before downloading). Null: download only. */
export function likelyPreview(name: string, size: number): PreviewKind | null {
  const ext = name.toLowerCase().split(".").pop() ?? "";
  if (TEXT_EXTENSIONS.has(ext)) return size <= PREVIEW_TEXT_MAX ? "text" : null;
  if (size > PREVIEW_MAX) return null;
  if (["png", "jpg", "jpeg", "gif", "webp", "bmp", "avif"].includes(ext)) return "image";
  if (["mp4", "m4v", "mov", "webm"].includes(ext)) return "video";
  if (["mp3", "m4a", "aac", "ogg", "oga", "opus", "flac", "wav"].includes(ext)) return "audio";
  if (ext === "pdf") return "pdf";
  return null;
}

/** Plain text: valid UTF-8 with no NUL bytes. */
export function looksLikeText(bytes: Uint8Array): boolean {
  if (bytes.includes(0)) return false;
  try {
    new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    return true;
  } catch {
    return false;
  }
}

/** The preview for a decrypted file: re-typed as what its bytes are, or null when it mustn't be shown. */
export async function previewOf(name: string, plaintext: Blob): Promise<{ kind: PreviewKind; blob: Blob } | null> {
  const head = await blobBytes(plaintext.slice(0, SNIFF_BYTES));
  const sniffed = sniff(head);
  if (sniffed) return { kind: sniffed.kind, blob: new Blob([plaintext], { type: sniffed.mime }) };
  const ext = name.toLowerCase().split(".").pop() ?? "";
  if (TEXT_EXTENSIONS.has(ext) && plaintext.size <= PREVIEW_TEXT_MAX) {
    const bytes = await blobBytes(plaintext);
    if (looksLikeText(bytes)) return { kind: "text", blob: new Blob([bytes as BlobPart], { type: "text/plain;charset=utf-8" }) };
  }
  return null;
}

const RESERVED = /^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\..*)?$/i;

/**
 * A name that's safe to save to someone's disk: no folders, no control or
 * direction-override characters (which can make "exe.txt" look like
 * "txt.exe"), no reserved Windows names, at most 200 characters.
 */
export function safeFileName(name: string): string {
  // eslint-disable-next-line no-control-regex
  let clean = name.replace(/[\u0000-\u001f\u007f‎‏‪-‮⁦-⁩]/g, "").replace(/[\\/:*?"<>|]/g, "_");
  clean = clean.replace(/^[.\s]+|[.\s]+$/g, "");
  if (RESERVED.test(clean)) clean = `_${clean}`;
  if (clean.length > 200) {
    const dot = clean.lastIndexOf(".");
    const ext = dot > 0 && clean.length - dot <= 16 ? clean.slice(dot) : "";
    clean = clean.slice(0, 200 - ext.length) + ext;
  }
  return clean || "file";
}

/** Names that run code when opened: the owner is warned before opening one that someone uploaded. */
export function isRisky(name: string): boolean {
  const ext = name.toLowerCase().split(".").pop() ?? "";
  return [
    "exe", "msi", "bat", "cmd", "com", "scr", "pif", "ps1", "vbs", "vbe", "js", "jse", "wsf", "wsh", "hta", "jar", "lnk", "reg",
    "dll", "sys", "cpl", "apk", "app", "dmg", "pkg", "sh", "html", "htm", "xhtml", "svg", "docm", "xlsm", "pptm", "dotm", "xlam", "iso",
  ].includes(ext);
}
