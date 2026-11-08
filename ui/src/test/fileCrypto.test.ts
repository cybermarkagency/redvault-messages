// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  decryptFile,
  encryptedSizeFor,
  encryptFile,
  FileDecryptError,
  FILE_ALGORITHM,
  FILE_CHUNK_BYTES,
  type EncryptedFileResult,
} from "../lib/fileCrypto.js";
import { isFileMessage, parseFileDescriptor, sanitizeFilename, serializeFileDescriptor, type FileDescriptor } from "../lib/fileMessage.js";

beforeAll(() => crypto.ready());

/** jsdom's Blob has no arrayBuffer(); FileReader works everywhere. */
function readBlob(blob: Blob): Promise<Uint8Array<ArrayBuffer>> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

function randomPlaintext(size: number): Uint8Array<ArrayBuffer> {
  const out = new Uint8Array(size);
  for (let off = 0; off < size; off += 65536) {
    globalThis.crypto.getRandomValues(out.subarray(off, Math.min(size, off + 65536)));
  }
  return out;
}

function paramsOf(enc: EncryptedFileResult) {
  return {
    key: enc.key,
    header: enc.header,
    chunkSize: enc.chunkSize,
    encryptedSize: enc.encryptedSize,
    ciphertextSha256: enc.ciphertextSha256,
  };
}

/** Feeds ciphertext back in awkward, non-chunk-aligned pieces, like a network stream would. */
function* inPieces(bytes: Uint8Array, pieceSize: number): Iterable<Uint8Array> {
  for (let off = 0; off < bytes.length; off += pieceSize) yield bytes.slice(off, off + pieceSize);
}

describe("fileCrypto (libsodium secretstream, format v1)", () => {
  it.each([0, 1, 1000, FILE_CHUNK_BYTES, FILE_CHUNK_BYTES + 1, 3 * FILE_CHUNK_BYTES + 12345])(
    "(13) encrypt/decrypt round-trips %i bytes to identical output",
    async (size) => {
      const plaintext = randomPlaintext(size);
      const enc = await encryptFile(new Blob([plaintext]));
      expect(enc.plaintextSize).toBe(size);
      expect(enc.encryptedSize).toBe(encryptedSizeFor(size));
      expect(enc.key).toHaveLength(32); // 256-bit key
      expect(enc.header).toHaveLength(24);

      const ciphertext = await readBlob(enc.blob);
      expect(ciphertext).toHaveLength(enc.encryptedSize);
      const out = await readBlob(await decryptFile(inPieces(ciphertext, 7919), paramsOf(enc)));
      expect(out).toEqual(plaintext);
    }
  );

  it("the ciphertext doesn't contain the plaintext", async () => {
    const marker = new TextEncoder().encode("PASSPORT-NUMBER-X1234567".repeat(50));
    const enc = await encryptFile(new Blob([marker]));
    const ciphertext = await readBlob(enc.blob);
    const hay = Array.from(ciphertext).join(",");
    expect(hay.includes(Array.from(marker.slice(0, 24)).join(","))).toBe(false);
  });

  describe("(14) tampering fails authentication", () => {
    async function encrypted() {
      const enc = await encryptFile(new Blob([randomPlaintext(2 * FILE_CHUNK_BYTES + 500)]));
      return { enc, bytes: await readBlob(enc.blob) };
    }

    it("a flipped bit anywhere in the ciphertext", async () => {
      const { enc, bytes } = await encrypted();
      for (const pos of [0, 100, FILE_CHUNK_BYTES + 20, bytes.length - 1]) {
        const tampered = bytes.slice();
        tampered[pos] ^= 0x01;
        // Skip the hash check so this proves the AEAD itself catches it.
        const params = { ...paramsOf(enc), ciphertextSha256: await sha256Hex(tampered) };
        await expect(decryptFile([tampered], params)).rejects.toBeInstanceOf(FileDecryptError);
      }
    });

    it("truncation (dropping the final chunk)", async () => {
      const { enc, bytes } = await encrypted();
      const truncated = bytes.slice(0, 2 * (FILE_CHUNK_BYTES + 17));
      const params = { ...paramsOf(enc), encryptedSize: truncated.length, ciphertextSha256: await sha256Hex(truncated) };
      await expect(decryptFile([truncated], params)).rejects.toBeInstanceOf(FileDecryptError);
    });

    it("reordered chunks", async () => {
      const { enc, bytes } = await encrypted();
      const frame = FILE_CHUNK_BYTES + 17;
      const swapped = new Uint8Array(bytes.length);
      swapped.set(bytes.slice(frame, 2 * frame), 0);
      swapped.set(bytes.slice(0, frame), frame);
      swapped.set(bytes.slice(2 * frame), 2 * frame);
      const params = { ...paramsOf(enc), ciphertextSha256: await sha256Hex(swapped) };
      await expect(decryptFile([swapped], params)).rejects.toBeInstanceOf(FileDecryptError);
    });

    it("trailing data appended after the final chunk", async () => {
      const { enc, bytes } = await encrypted();
      const extended = new Uint8Array(bytes.length + 40);
      extended.set(bytes);
      const params = { ...paramsOf(enc), encryptedSize: extended.length, ciphertextSha256: await sha256Hex(extended) };
      await expect(decryptFile([extended], params)).rejects.toBeInstanceOf(FileDecryptError);
    });

    it("the wrong key, or a ciphertext hash that doesn't match the descriptor", async () => {
      const { enc, bytes } = await encrypted();
      const other = await encryptFile(new Blob([new Uint8Array(10)]));
      await expect(decryptFile([bytes], { ...paramsOf(enc), key: other.key })).rejects.toBeInstanceOf(FileDecryptError);
      await expect(decryptFile([bytes], { ...paramsOf(enc), ciphertextSha256: "0".repeat(64) })).rejects.toBeInstanceOf(
        FileDecryptError
      );
    });
  });

  it("(15) every file gets a fresh random key and header (no key/nonce reuse), even for identical content", async () => {
    const plaintext = randomPlaintext(5000);
    const results = await Promise.all([1, 2, 3, 4, 5].map(() => encryptFile(new Blob([plaintext]))));
    const keys = new Set(results.map((r) => crypto.toBase64(r.key)));
    const headers = new Set(results.map((r) => crypto.toBase64(r.header)));
    const hashes = new Set(results.map((r) => r.ciphertextSha256));
    expect(keys.size).toBe(5);
    expect(headers.size).toBe(5);
    expect(hashes.size).toBe(5);
  });
});

async function sha256Hex(bytes: Uint8Array<ArrayBuffer>): Promise<string> {
  const digest = new Uint8Array(await globalThis.crypto.subtle.digest("SHA-256", bytes));
  return Array.from(digest, (b) => b.toString(16).padStart(2, "0")).join("");
}

describe("file descriptor (travels only inside the E2EE message)", () => {
  const base: FileDescriptor = {
    type: "file",
    version: 1,
    objectId: "0199cc25-8214-4cf2-a88d-32c890aabbcc",
    fileKey: "a2V5a2V5a2V5a2V5a2V5a2V5a2V5a2V5a2V5a2V5a2U=",
    filename: "photo.jpg",
    mimeType: "image/jpeg",
    originalSize: 2819231,
    encryptedSize: 2819300,
    ciphertextSha256: "ab".repeat(32),
    encryption: { algorithm: FILE_ALGORITHM, version: 1, header: "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", chunkSize: FILE_CHUNK_BYTES },
  };

  it("round-trips, and is distinguishable from any typeable text message", () => {
    const text = serializeFileDescriptor(base);
    expect(isFileMessage(text)).toBe(true);
    expect(parseFileDescriptor(text)).toEqual(base);
    expect(isFileMessage(JSON.stringify(base))).toBe(false);
  });

  it("rejects malformed or unexpected descriptors", () => {
    const bad = [
      { ...base, version: 2 },
      { ...base, objectId: "../../etc/passwd" },
      { ...base, originalSize: -1 },
      { ...base, ciphertextSha256: "zz" },
      { ...base, encryption: { ...base.encryption, algorithm: "rot13" } },
      { ...base, encryption: { ...base.encryption, chunkSize: 1 } },
    ];
    for (const d of bad) expect(parseFileDescriptor(serializeFileDescriptor(d as FileDescriptor))).toBeNull();
    expect(parseFileDescriptor(serializeFileDescriptor(base).slice(0, 30))).toBeNull();
  });

  it("sanitizes hostile filenames from the sender", () => {
    expect(sanitizeFilename("../../../etc/passwd")).toBe("passwd");
    for (const name of ["CONIN$", "conout$.txt", "COM¹.txt", "LPT²", "NUL .txt"]) {
      expect(sanitizeFilename(name)).toBe(`_${name}`);
    }
    expect(sanitizeFilename("console.txt")).toBe("console.txt");
    expect(sanitizeFilename("C:\\Windows\\System32\\evil.dll")).toBe("evil.dll");
    expect(sanitizeFilename("invoice\u202Egpj.exe")).toBe("invoicegpj.exe"); // no RTL-override disguise
    expect(sanitizeFilename("<img src=x onerror=alert(1)>.png")).not.toMatch(/[<>]/);
    expect(sanitizeFilename("CON.txt")).toBe("_CON.txt");
    expect(sanitizeFilename("...")).toBe("file");
    expect(sanitizeFilename("a".repeat(500) + ".pdf")).toMatch(/^a+\.pdf$/);
    expect(sanitizeFilename("a".repeat(500) + ".pdf").length).toBeLessThanOrEqual(150);
  });

  it("falls back to a neutral MIME type when the declared one is junk", () => {
    const parsed = parseFileDescriptor(serializeFileDescriptor({ ...base, mimeType: "text/html; <script>" }));
    expect(parsed?.mimeType).toBe("application/octet-stream");
  });
});
