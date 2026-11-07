// The file descriptor that rides inside an ordinary E2EE message.
//
// It's serialized into the message plaintext and then encrypted with the
// conversation key exactly like a text message, so the file key, real
// filename, MIME type and exact size are only ever readable by the
// conversation's participants — never the API, the database or B2. The
// server only learns the opaque objectId (as the message's `attachments`
// list), which it needs to authorize downloads.
//
// Everything parsed from a received descriptor is untrusted input from the
// other participant: it's validated strictly, and the filename is
// sanitized before it's displayed or used as a download name.
import { FILE_ALGORITHM, FILE_CHUNK_BYTES, FILE_FORMAT_VERSION } from "./fileCrypto.js";

/** Prefix that marks a message plaintext as a file descriptor rather than text. A NUL can't be typed into the composer, so a text message can't impersonate one. */
const FILE_MESSAGE_PREFIX = "\u0000rv-file:";

export interface FileDescriptor {
  type: "file";
  version: 1;
  objectId: string;
  /** base64 — the 256-bit file key. */
  fileKey: string;
  filename: string;
  mimeType: string;
  originalSize: number;
  encryptedSize: number;
  /** hex SHA-256 of the ciphertext blob; the recipient verifies it before decrypting. */
  ciphertextSha256: string;
  encryption: {
    algorithm: typeof FILE_ALGORITHM;
    version: 1;
    /** base64 — the secretstream header (per-file random nonce material). */
    header: string;
    chunkSize: number;
  };
}

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const B64_RE = /^[A-Za-z0-9+/]+={0,2}$/;
const HEX64_RE = /^[0-9a-f]{64}$/;
const MIME_RE = /^[a-z0-9][a-z0-9!#$&^_.+-]{0,63}\/[a-z0-9][a-z0-9!#$&^_.+-]{0,127}$/i;

export function isFileMessage(plaintext: string): boolean {
  return plaintext.startsWith(FILE_MESSAGE_PREFIX);
}

export function serializeFileDescriptor(descriptor: FileDescriptor): string {
  return FILE_MESSAGE_PREFIX + JSON.stringify(descriptor);
}

function isSafeSize(n: unknown): n is number {
  return typeof n === "number" && Number.isSafeInteger(n) && n >= 0;
}

/** Parses + strictly validates a received descriptor. Returns null for anything malformed. */
export function parseFileDescriptor(plaintext: string): FileDescriptor | null {
  if (!isFileMessage(plaintext)) return null;
  let raw: unknown;
  try {
    raw = JSON.parse(plaintext.slice(FILE_MESSAGE_PREFIX.length));
  } catch {
    return null;
  }
  if (!raw || typeof raw !== "object") return null;
  const d = raw as Record<string, unknown>;
  const enc = d.encryption as Record<string, unknown> | undefined;
  if (
    d.type !== "file" ||
    d.version !== FILE_FORMAT_VERSION ||
    typeof d.objectId !== "string" ||
    !UUID_RE.test(d.objectId) ||
    typeof d.fileKey !== "string" ||
    !B64_RE.test(d.fileKey) ||
    typeof d.filename !== "string" ||
    d.filename.length > 1024 ||
    typeof d.mimeType !== "string" ||
    !isSafeSize(d.originalSize) ||
    !isSafeSize(d.encryptedSize) ||
    typeof d.ciphertextSha256 !== "string" ||
    !HEX64_RE.test(d.ciphertextSha256) ||
    !enc ||
    typeof enc !== "object" ||
    enc.algorithm !== FILE_ALGORITHM ||
    enc.version !== FILE_FORMAT_VERSION ||
    typeof enc.header !== "string" ||
    !B64_RE.test(enc.header) ||
    enc.chunkSize !== FILE_CHUNK_BYTES
  ) {
    return null;
  }
  return {
    type: "file",
    version: 1,
    objectId: d.objectId.toLowerCase(),
    fileKey: d.fileKey,
    filename: sanitizeFilename(d.filename),
    mimeType: MIME_RE.test(d.mimeType) ? d.mimeType.toLowerCase() : "application/octet-stream",
    originalSize: d.originalSize,
    encryptedSize: d.encryptedSize,
    ciphertextSha256: d.ciphertextSha256,
    encryption: { algorithm: FILE_ALGORITHM, version: 1, header: enc.header, chunkSize: FILE_CHUNK_BYTES },
  };
}

const WINDOWS_RESERVED = /^(con|prn|aux|nul|conin\$|conout\$|com[0-9¹²³]|lpt[0-9¹²³]) *(\..*)?$/i;

/**
 * Makes a sender-supplied filename safe to show and to use as a download
 * name: strips path components (no traversal), control characters and
 * bidi/zero-width tricks (e.g. "invoice‮gpj.exe" rendering as
 * "invoiceexe.jpg"), leading dots, and Windows-reserved names, and caps the
 * length while keeping the extension. React escapes it on render, so it's
 * never HTML — this is about honest display and safe saving.
 */
export function sanitizeFilename(name: string): string {
  let clean = name
    .normalize("NFC")
    // Path separators -> keep only the final path component.
    .split(/[/\\]/)
    .pop()!
    // Control chars, bidi overrides/isolates, zero-width chars, BOM.
    .replace(/[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁠-⁩﻿]/g, "")
    // Characters invalid in filenames on common OSes.
    .replace(/[<>:"|?*]/g, "_")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/^\.+/, "")
    .replace(/[. ]+$/, "");

  if (WINDOWS_RESERVED.test(clean)) clean = `_${clean}`;
  const MAX = 150;
  if (clean.length > MAX) {
    const dot = clean.lastIndexOf(".");
    const ext = dot > 0 && clean.length - dot <= 16 ? clean.slice(dot) : "";
    clean = clean.slice(0, MAX - ext.length) + ext;
  }
  return clean || "file";
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB"];
  let value = bytes / 1024;
  let i = 0;
  while (value >= 1024 && i < units.length - 1) {
    value /= 1024;
    i += 1;
  }
  return `${value < 10 ? value.toFixed(1) : Math.round(value)} ${units[i]}`;
}
