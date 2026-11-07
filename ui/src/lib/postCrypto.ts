// Exclusive posts, end-to-end encrypted (docs/creator-earnings.md).
//
// Each post has its own random key K. Its body (text, and the key + header
// of an encrypted photo or video) is a JSON blob encrypted with K, bound to
// the post id. The server stores that blob and the encrypted media, never K.
//
// K reaches readers as nonce || crypto_box(payload, nonce, reader's public
// key, author's private key), with payload = "RVEP" | version 1 | post id
// (16 bytes) | K (32 bytes): the same construction as story keys
// (lib/storyCrypto.ts), with its own tag so a story key can never be passed
// off as a post key or the other way round. The author's app seals K to
// itself when posting and to each buyer after their payment is confirmed;
// the server never can.
import sodium from "libsodium-wrappers-sumo";
import { ready } from "./e2eCrypto.js";

const MAGIC = [0x52, 0x56, 0x45, 0x50]; // "RVEP"
const VERSION = 1;
const ID_BYTES = 16;
const KEY_BYTES = 32;
const PAYLOAD_BYTES = MAGIC.length + 1 + ID_BYTES + KEY_BYTES;
const BODY_AD = "redvault-post-body-v1";

export class PostCryptoError extends Error {}

export { ready };

function bytes(s: string): Uint8Array {
  const src = sodium.from_string(s);
  const out = new Uint8Array(src.length);
  out.set(src);
  return out;
}

function uuidBytes(id: string): Uint8Array {
  const hex = id.replace(/-/g, "").toLowerCase();
  if (!/^[0-9a-f]{32}$/.test(hex)) throw new PostCryptoError("not a post id");
  const out = new Uint8Array(ID_BYTES);
  for (let i = 0; i < ID_BYTES; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

function boundTo(id: string): Uint8Array {
  const tag = bytes(BODY_AD);
  const out = new Uint8Array(tag.length + ID_BYTES);
  out.set(tag, 0);
  out.set(uuidBytes(id), tag.length);
  return out;
}

export function newPostId(): string {
  const b = sodium.randombytes_buf(16);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const hex = sodium.to_hex(b);
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

export function newPostKey(): Uint8Array {
  return sodium.crypto_aead_xchacha20poly1305_ietf_keygen();
}

export function wrapPostKey(postId: string, key: Uint8Array, recipientPublicKey: Uint8Array, authorPrivateKey: Uint8Array): Uint8Array {
  if (key.length !== KEY_BYTES) throw new PostCryptoError("post keys are 32 bytes");
  const payload = new Uint8Array(PAYLOAD_BYTES);
  payload.set(MAGIC, 0);
  payload[4] = VERSION;
  payload.set(uuidBytes(postId), 5);
  payload.set(key, 5 + ID_BYTES);
  const nonce = sodium.randombytes_buf(sodium.crypto_box_NONCEBYTES);
  const box = sodium.crypto_box_easy(payload, nonce, recipientPublicKey, authorPrivateKey);
  const out = new Uint8Array(nonce.length + box.length);
  out.set(nonce, 0);
  out.set(box, nonce.length);
  return out;
}

export function unwrapPostKey(postId: string, wrapped: Uint8Array, authorPublicKey: Uint8Array, readerPrivateKey: Uint8Array): Uint8Array {
  if (wrapped.length !== sodium.crypto_box_NONCEBYTES + PAYLOAD_BYTES + sodium.crypto_box_MACBYTES) throw new PostCryptoError("malformed post key");
  let payload: Uint8Array;
  try {
    payload = sodium.crypto_box_open_easy(wrapped.subarray(sodium.crypto_box_NONCEBYTES), wrapped.subarray(0, sodium.crypto_box_NONCEBYTES), authorPublicKey, readerPrivateKey);
  } catch {
    throw new PostCryptoError("this post key wasn't made by the creator for you");
  }
  if (!sameBytes(payload.subarray(0, 4), new Uint8Array(MAGIC)) || payload[4] !== VERSION) throw new PostCryptoError("unknown post key format");
  if (!sameBytes(payload.subarray(5, 5 + ID_BYTES), uuidBytes(postId))) throw new PostCryptoError("this key belongs to another post");
  return payload.slice(5 + ID_BYTES);
}

export const MAX_POST_TEXT = 10_000;

export interface PostMediaRef {
  key: string;
  header: string;
  sha256: string;
  encryptedSize: number;
  chunkSize: number;
  mime: string;
}

export interface PostBody {
  v: 1;
  text: string;
  media?: PostMediaRef;
}

export function sealPostBody(postId: string, key: Uint8Array, body: PostBody): { ciphertext: Uint8Array; nonce: Uint8Array } {
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  return { ciphertext: sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(JSON.stringify(body), boundTo(postId), null, nonce, key), nonce };
}

export function openPostBody(postId: string, key: Uint8Array, ciphertext: Uint8Array, nonce: Uint8Array): PostBody {
  let raw: Record<string, unknown>;
  try {
    raw = JSON.parse(sodium.to_string(sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, ciphertext, boundTo(postId), nonce, key))) as Record<string, unknown>;
  } catch {
    throw new PostCryptoError("this post can't be opened");
  }
  if (raw.v !== 1 || typeof raw.text !== "string" || raw.text.length > MAX_POST_TEXT) throw new PostCryptoError("malformed post");
  const body: PostBody = { v: 1, text: raw.text };
  const m = raw.media as Record<string, unknown> | undefined;
  if (m !== undefined) {
    const okB64 = (v: unknown) => typeof v === "string" && v.length > 0 && v.length <= 64;
    const okInt = (v: unknown) => typeof v === "number" && Number.isSafeInteger(v) && v > 0;
    if (!m || !okB64(m.key) || !okB64(m.header) || typeof m.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(m.sha256) || !okInt(m.encryptedSize) || !okInt(m.chunkSize)) {
      throw new PostCryptoError("malformed post");
    }
    if (typeof m.mime !== "string" || !/^(image\/(jpeg|png|webp)|video\/(mp4|webm|quicktime))$/.test(m.mime)) throw new PostCryptoError("malformed post");
    body.media = { key: m.key as string, header: m.header as string, sha256: m.sha256, encryptedSize: m.encryptedSize as number, chunkSize: m.chunkSize as number, mime: m.mime };
  }
  return body;
}
