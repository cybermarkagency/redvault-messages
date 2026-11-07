// Client-side file encryption for E2EE attachments — format version 1.
//
// Construction: libsodium's crypto_secretstream_xchacha20poly1305, the
// library's own audited API for encrypting a long stream as a sequence of
// authenticated chunks. Nothing here is a custom cipher or mode:
//   - a fresh random 256-bit key per file (crypto_secretstream_…_keygen,
//     backed by the platform CSPRNG),
//   - a random 24-byte stream header per file (init_push) — libsodium
//     derives per-chunk nonces from it internally, so nonce reuse under a
//     key is impossible by construction,
//   - the file is cut into 64 KiB plaintext chunks, each sealed with a
//     16-byte Poly1305 tag (+1 byte chunk tag = 17 bytes overhead),
//   - the last chunk carries TAG_FINAL, so truncation, reordering,
//     dropping or appending chunks all fail authentication.
//
// The key and header never go to the API or to storage: they travel only
// inside the E2EE message body (see lib/fileMessage.ts). The server/B2 only
// ever see the concatenated ciphertext chunks plus its SHA-256, which the
// recipient re-checks before trusting anything.
//
// Memory: the file is read and encrypted one chunk at a time and the
// ciphertext is collected as a Blob of chunk parts (browsers can page large
// Blobs to disk), so the whole plaintext is never held in one JS buffer.
// Plaintext is never written to localStorage/IndexedDB.
import sodium from "libsodium-wrappers-sumo";
import { ready } from "./e2eCrypto.js";

export const FILE_FORMAT_VERSION = 1;
export const FILE_ALGORITHM = "xchacha20poly1305-secretstream";
/** Must match server/src/services/fileService.ts FILE_CHUNK_BYTES. */
export const FILE_CHUNK_BYTES = 64 * 1024;
export const FILE_CHUNK_OVERHEAD_BYTES = 17; // crypto_secretstream_xchacha20poly1305_ABYTES

export class FileDecryptError extends Error {
  constructor(message = "Unable to decrypt this file — it may be corrupted or tampered with.") {
    super(message);
    this.name = "FileDecryptError";
  }
}

export interface EncryptedFileResult {
  /** Ciphertext to upload. Opaque bytes, application/octet-stream. */
  blob: Blob;
  key: Uint8Array;
  header: Uint8Array;
  plaintextSize: number;
  encryptedSize: number;
  /** Lowercase hex SHA-256 of the ciphertext. */
  ciphertextSha256: string;
  chunkSize: number;
}

export interface FileDecryptParams {
  key: Uint8Array;
  header: Uint8Array;
  chunkSize: number;
  encryptedSize: number;
  ciphertextSha256: string;
}

export function encryptedSizeFor(plaintextSize: number, chunkSize = FILE_CHUNK_BYTES): number {
  const chunks = Math.max(1, Math.ceil(plaintextSize / chunkSize));
  return plaintextSize + chunks * FILE_CHUNK_OVERHEAD_BYTES;
}

/** Blob.arrayBuffer isn't available everywhere (e.g. older Safari, jsdom) — FileReader is. */
function readBlob(blob: Blob): Promise<Uint8Array> {
  if (typeof blob.arrayBuffer === "function") {
    return blob.arrayBuffer().then((buf) => new Uint8Array(buf));
  }
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error ?? new Error("Couldn't read the file"));
    reader.readAsArrayBuffer(blob);
  });
}

export async function encryptFile(
  file: Blob,
  onProgress?: (fraction: number) => void
): Promise<EncryptedFileResult> {
  await ready();
  const key = sodium.crypto_secretstream_xchacha20poly1305_keygen();
  const { state, header } = sodium.crypto_secretstream_xchacha20poly1305_init_push(key);
  const hash = sodium.crypto_hash_sha256_init();
  const parts: Uint8Array[] = [];
  let encryptedSize = 0;

  const size = file.size;
  let offset = 0;
  do {
    const end = Math.min(offset + FILE_CHUNK_BYTES, size);
    const plaintext = await readBlob(file.slice(offset, end));
    const isLast = end >= size;
    const chunk = sodium.crypto_secretstream_xchacha20poly1305_push(
      state,
      plaintext,
      null,
      isLast ? sodium.crypto_secretstream_xchacha20poly1305_TAG_FINAL : sodium.crypto_secretstream_xchacha20poly1305_TAG_MESSAGE
    );
    // Best-effort: don't leave this chunk's plaintext lying around in the heap.
    sodium.memzero(plaintext);
    sodium.crypto_hash_sha256_update(hash, chunk);
    parts.push(chunk);
    encryptedSize += chunk.length;
    offset = end;
    onProgress?.(size === 0 ? 1 : offset / size);
  } while (offset < size);

  return {
    blob: new Blob(parts as BlobPart[], { type: "application/octet-stream" }),
    key,
    header,
    plaintextSize: size,
    encryptedSize,
    ciphertextSha256: sodium.to_hex(sodium.crypto_hash_sha256_final(hash)),
    chunkSize: FILE_CHUNK_BYTES,
  };
}

/**
 * Decrypts a ciphertext stream (any chunking — it's re-framed here) into a
 * Blob of plaintext. Throws FileDecryptError on any authentication failure,
 * truncation, trailing data, size mismatch or ciphertext-hash mismatch, and
 * never returns partially-decrypted output.
 */
export async function decryptFile(
  ciphertext: AsyncIterable<Uint8Array> | Iterable<Uint8Array>,
  params: FileDecryptParams,
  mimeType = "application/octet-stream",
  onProgress?: (fraction: number) => void
): Promise<Blob> {
  await ready();
  if (params.key.length !== sodium.crypto_secretstream_xchacha20poly1305_KEYBYTES) throw new FileDecryptError();
  if (params.header.length !== sodium.crypto_secretstream_xchacha20poly1305_HEADERBYTES) throw new FileDecryptError();
  if (params.chunkSize !== FILE_CHUNK_BYTES) throw new FileDecryptError("Unsupported file format.");

  let state: ReturnType<typeof sodium.crypto_secretstream_xchacha20poly1305_init_pull>;
  try {
    state = sodium.crypto_secretstream_xchacha20poly1305_init_pull(params.header, params.key);
  } catch {
    throw new FileDecryptError();
  }
  const hash = sodium.crypto_hash_sha256_init();
  const frame = params.chunkSize + FILE_CHUNK_OVERHEAD_BYTES;
  const parts: Uint8Array[] = [];
  let pending = new Uint8Array(0);
  let received = 0;
  let finished = false;

  const pull = (chunk: Uint8Array) => {
    if (finished) throw new FileDecryptError(); // data after TAG_FINAL
    const result = sodium.crypto_secretstream_xchacha20poly1305_pull(state, chunk, null);
    if (!result) throw new FileDecryptError();
    if (result.tag === sodium.crypto_secretstream_xchacha20poly1305_TAG_FINAL) finished = true;
    else if (result.tag !== sodium.crypto_secretstream_xchacha20poly1305_TAG_MESSAGE) throw new FileDecryptError();
    parts.push(result.message);
  };

  try {
    for await (const piece of ciphertext) {
      received += piece.length;
      if (received > params.encryptedSize) throw new FileDecryptError();
      // Copy into a buffer libsodium owns the type of (see the realm note in e2eCrypto.ts).
      const buf = new Uint8Array(pending.length + piece.length);
      buf.set(pending);
      buf.set(piece, pending.length);
      let off = 0;
      // Keep the last frame buffered until the stream ends, so the final
      // (possibly full-size) chunk is always handled by the tail below.
      while (buf.length - off > frame) {
        const chunk = buf.slice(off, off + frame);
        sodium.crypto_hash_sha256_update(hash, chunk);
        pull(chunk);
        off += frame;
      }
      pending = buf.slice(off);
      onProgress?.(received / params.encryptedSize);
    }
    if (pending.length > 0) {
      sodium.crypto_hash_sha256_update(hash, pending);
      pull(pending);
    }
  } catch (err) {
    for (const p of parts) sodium.memzero(p);
    if (err instanceof FileDecryptError) throw err;
    throw new FileDecryptError();
  }

  const digest = sodium.to_hex(sodium.crypto_hash_sha256_final(hash));
  if (!finished || received !== params.encryptedSize || digest !== params.ciphertextSha256) {
    for (const p of parts) sodium.memzero(p);
    throw new FileDecryptError();
  }
  return new Blob(parts as BlobPart[], { type: mimeType });
}
