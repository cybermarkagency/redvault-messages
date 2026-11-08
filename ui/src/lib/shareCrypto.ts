// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Public share links for Files: the protocol's cryptography
// (docs/public-share-links.md). The desktop (desktop/core/src/share_crypto.cpp)
// and Android (crypto/ShareCrypto.kt) implement the same functions and are
// tested against vectors made from this file (desktop/tests/vectors/share_vectors.json).
//
// Only libsodium primitives: crypto_kdf (BLAKE2b) for key separation,
// Argon2id13 for passwords, XChaCha20-Poly1305-IETF with associated data for
// everything sealed under a symmetric key, crypto_box_seal for uploads.
import sodium from "libsodium-wrappers-sumo";
import { ready } from "./e2eCrypto.js";

export const SHARE_KDF_CONTEXT = "rvshare1";
const ENVELOPE_AD = "rvshare-envelope-v1";
const ITEM_AD_PREFIX = "rvshare-item-v1|";
const OWNER_AD = "rvshare-owner-v1";
const UPLOAD_KEY_LABEL = "redvault-share-upload-v1";
const PASSWORD_LABEL = "redvault-share-password-v1";

/** Argon2id13 cost for new links: 3 passes over 64 MiB (fits phones' browsers; stored per link so it can be raised). */
export const DEFAULT_PASSWORD_OPS = 3;
export const DEFAULT_PASSWORD_MEM = 64 * 1024 * 1024;
export const PASSWORD_OPS_RANGE = { min: 2, max: 10 };
export const PASSWORD_MEM_RANGE = { min: 64 * 1024 * 1024, max: 256 * 1024 * 1024 };

/** crypto_kdf subkey ids (docs/public-share-links.md §2). */
const Subkey = { Lookup: 1, Root: 2, PasswordKey: 3, Access: 4, Wrap: 5 } as const;
type Subkey = (typeof Subkey)[keyof typeof Subkey];

export interface SharePasswordParams {
  salt: Uint8Array;
  ops: number;
  mem: number;
}

/** Everything a device derives from a link's secret (and password). Only `lookup` and `access` ever leave it, as tokens. */
export interface ShareKeys {
  lookup: Uint8Array;
  access: Uint8Array;
  wrap: Uint8Array;
}

export interface ShareEnvelope {
  /** The share key: opens every item's metadata. */
  key: Uint8Array;
  /** The owner's upload public key (Contributor links use it; always present for links made by these apps). */
  upload: Uint8Array | null;
  /** The item id of what was shared (the file, or the top folder). */
  root: string;
}

export interface ShareFolderMeta {
  v: 1;
  kind: "folder";
  name: string;
  parent: string;
}

export interface ShareFileMeta {
  v: 1;
  kind: "file";
  name: string;
  parent: string;
  mime: string;
  size: number;
  modified: number;
  /** The file's own content key, secretstream header (base64), chunk size and ciphertext SHA-256 (hex). */
  key: string;
  header: string;
  chunk: number;
  sha: string;
}

export type ShareItemMeta = ShareFolderMeta | ShareFileMeta;

export interface OwnerBox {
  v: 1;
  /** The link's secret, base64url (what goes after # in the link). */
  secret: string;
  /** The share key, base64. */
  key: string;
  /** The item id of what was shared (as in the envelope), so the owner can rebuild the link's items. */
  root: string;
  label: string;
  /** When the link was made (epoch ms), on the owner's device only. */
  created: number;
}

/** What a contributor's browser seals to the owner with each upload. */
export interface ContributionMeta {
  v: 1;
  name: string;
  mime: string;
  size: number;
  modified: number;
  /** When it was sent (epoch ms, the uploader's clock). */
  sent: number;
  key: string;
  header: string;
  chunk: number;
  sha: string;
}

export class ShareCryptoError extends Error {
  constructor(message = "This link is damaged or the password is wrong.") {
    super(message);
    this.name = "ShareCryptoError";
  }
}

// --- Encoding ---

export function toB64(bytes: Uint8Array): string {
  return sodium.to_base64(bytes, sodium.base64_variants.ORIGINAL);
}

export function fromB64(text: string): Uint8Array {
  return sodium.from_base64(text, sodium.base64_variants.ORIGINAL);
}

export function toB64Url(bytes: Uint8Array): string {
  return sodium.to_base64(bytes, sodium.base64_variants.URLSAFE_NO_PADDING);
}

export function fromB64Url(text: string): Uint8Array {
  return sodium.from_base64(text, sodium.base64_variants.URLSAFE_NO_PADDING);
}

/** UTF-8, copied into this module's own Uint8Array: the type libsodium accepts (TextEncoder's can come from another realm, e.g. under jsdom). */
function utf8(text: string): Uint8Array {
  const encoded = new TextEncoder().encode(text);
  const out = new Uint8Array(encoded.length);
  out.set(encoded);
  return out;
}

/** JSON padded with spaces to the next multiple of `bucket` bytes (always at least one space). */
export function padJson(value: unknown, bucket: number): Uint8Array {
  const bytes = utf8(JSON.stringify(value));
  const padded = new Uint8Array((Math.floor(bytes.length / bucket) + 1) * bucket).fill(0x20);
  padded.set(bytes);
  return padded;
}

function parsePadded(bytes: Uint8Array): unknown {
  return JSON.parse(sodium.to_string(bytes).replace(/ +$/, ""));
}

function concat(...parts: Uint8Array[]): Uint8Array {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let offset = 0;
  for (const p of parts) {
    out.set(p, offset);
    offset += p.length;
  }
  return out;
}

// --- Keys ---

function kdf(key: Uint8Array, id: Subkey): Uint8Array {
  return sodium.crypto_kdf_derive_from_key(32, id, SHARE_KDF_CONTEXT, key);
}

export function newLinkSecret(): Uint8Array {
  return sodium.randombytes_buf(32);
}

export function newShareKey(): Uint8Array {
  return sodium.randombytes_buf(32);
}

export function newSalt(): Uint8Array {
  return sodium.randombytes_buf(16);
}

export function newPasswordParams(): SharePasswordParams {
  return { salt: newSalt(), ops: DEFAULT_PASSWORD_OPS, mem: DEFAULT_PASSWORD_MEM };
}

/** Proves "I have the link" to look it up (and learn its password salt). */
export function lookupToken(secret: Uint8Array): Uint8Array {
  if (secret.length !== 32) throw new ShareCryptoError("This link is incomplete. Copy the whole link and try again.");
  return kdf(secret, Subkey.Lookup);
}

/**
 * The link's keys. With a password, Argon2id13(password, salt) is mixed in,
 * so neither the access token nor the envelope's key exists without it.
 * Passwords are NFC-normalised UTF-8 on every platform.
 */
export async function deriveShareKeys(secret: Uint8Array, password: string | null, params: SharePasswordParams | null): Promise<ShareKeys> {
  await ready();
  const lookup = lookupToken(secret);
  let root: Uint8Array;
  if (params) {
    if (password === null || password === "") throw new ShareCryptoError("This link needs a password.");
    if (params.salt.length !== 16) throw new ShareCryptoError();
    if (params.ops < PASSWORD_OPS_RANGE.min || params.ops > PASSWORD_OPS_RANGE.max) throw new ShareCryptoError();
    if (params.mem < PASSWORD_MEM_RANGE.min || params.mem > PASSWORD_MEM_RANGE.max) throw new ShareCryptoError();
    const stretched = sodium.crypto_pwhash(32, utf8(password.normalize("NFC")), params.salt, params.ops, params.mem, sodium.crypto_pwhash_ALG_ARGON2ID13);
    const passwordKey = kdf(secret, Subkey.PasswordKey);
    root = sodium.crypto_generichash(32, concat(utf8(PASSWORD_LABEL), stretched), passwordKey);
    sodium.memzero(stretched);
    sodium.memzero(passwordKey);
  } else {
    root = kdf(secret, Subkey.Root);
  }
  const keys = { lookup, access: kdf(root, Subkey.Access), wrap: kdf(root, Subkey.Wrap) };
  sodium.memzero(root);
  return keys;
}

/** SHA-256 of a token: what the server keeps (base64). */
export function verifierOf(token: Uint8Array): string {
  return toB64(sodium.crypto_hash_sha256(token));
}

// --- Sealing ---

function seal(key: Uint8Array, ad: string, plaintext: Uint8Array): { ciphertext: Uint8Array; nonce: Uint8Array } {
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  return { ciphertext: sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(plaintext, utf8(ad), null, nonce, key), nonce };
}

function open(key: Uint8Array, ad: string, ciphertext: Uint8Array, nonce: Uint8Array): Uint8Array | null {
  if (nonce.length !== sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES || key.length !== 32) return null;
  try {
    return sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, ciphertext, utf8(ad), nonce, key);
  } catch {
    return null;
  }
}

export function sealEnvelope(wrap: Uint8Array, envelope: ShareEnvelope): { ciphertext: Uint8Array; nonce: Uint8Array } {
  const json = { v: 1, key: toB64(envelope.key), upload: envelope.upload ? toB64(envelope.upload) : null, root: envelope.root };
  return seal(wrap, ENVELOPE_AD, padJson(json, 128));
}

/** Null when the key is wrong (a wrong password) or the envelope was tampered with. */
export function openEnvelope(wrap: Uint8Array, ciphertext: Uint8Array, nonce: Uint8Array): ShareEnvelope | null {
  const plain = open(wrap, ENVELOPE_AD, ciphertext, nonce);
  if (!plain) return null;
  try {
    const json = parsePadded(plain) as { v?: unknown; key?: unknown; upload?: unknown; root?: unknown };
    if (json.v !== 1 || typeof json.key !== "string" || typeof json.root !== "string") return null;
    const key = fromB64(json.key);
    const upload = typeof json.upload === "string" ? fromB64(json.upload) : null;
    if (key.length !== 32 || (upload !== null && upload.length !== sodium.crypto_box_PUBLICKEYBYTES)) return null;
    return { key, upload, root: json.root };
  } catch {
    return null;
  }
}

export function sealItem(shareKey: Uint8Array, itemId: string, meta: ShareItemMeta): { ciphertext: Uint8Array; nonce: Uint8Array } {
  return seal(shareKey, ITEM_AD_PREFIX + itemId, padJson(meta, 128));
}

const HEX64 = /^[0-9a-f]{64}$/;
const isStr = (v: unknown, max = 4096): v is string => typeof v === "string" && v.length <= max;
const isCount = (v: unknown): v is number => typeof v === "number" && Number.isSafeInteger(v) && v >= 0;

/** Null when it doesn't open under this key and this item id, or isn't well-formed. */
export function openItem(shareKey: Uint8Array, itemId: string, ciphertext: Uint8Array, nonce: Uint8Array): ShareItemMeta | null {
  const plain = open(shareKey, ITEM_AD_PREFIX + itemId, ciphertext, nonce);
  if (!plain) return null;
  try {
    const m = parsePadded(plain) as Record<string, unknown>;
    if (m.v !== 1 || !isStr(m.name, 1024) || !isStr(m.parent, 64)) return null;
    if (m.kind === "folder") return { v: 1, kind: "folder", name: m.name, parent: m.parent };
    if (m.kind !== "file") return null;
    if (!isStr(m.mime, 255) || !isCount(m.size) || !isCount(m.modified) || !isStr(m.key, 64) || !isStr(m.header, 64)) return null;
    if (!isCount(m.chunk) || !isStr(m.sha) || !HEX64.test(m.sha)) return null;
    return { v: 1, kind: "file", name: m.name, parent: m.parent, mime: m.mime, size: m.size, modified: m.modified, key: m.key, header: m.header, chunk: m.chunk, sha: m.sha };
  } catch {
    return null;
  }
}

export function sealOwnerBox(driveKey: Uint8Array, box: OwnerBox): { ciphertext: Uint8Array; nonce: Uint8Array } {
  return seal(driveKey, OWNER_AD, padJson(box, 128));
}

export function openOwnerBox(driveKey: Uint8Array, ciphertext: Uint8Array, nonce: Uint8Array): OwnerBox | null {
  const plain = open(driveKey, OWNER_AD, ciphertext, nonce);
  if (!plain) return null;
  try {
    const b = parsePadded(plain) as Record<string, unknown>;
    if (b.v !== 1 || !isStr(b.secret, 64) || !isStr(b.key, 64) || !isStr(b.root, 64) || !isStr(b.label, 1024) || !isCount(b.created)) return null;
    return { v: 1, secret: b.secret, key: b.key, root: b.root, label: b.label, created: b.created };
  } catch {
    return null;
  }
}

// --- Uploads ---

/** The owner's upload key pair for one link: derived from their Files key, never stored or sent. */
export function uploadKeyPair(driveKey: Uint8Array, salt: Uint8Array): { publicKey: Uint8Array; privateKey: Uint8Array } {
  if (salt.length !== 16) throw new ShareCryptoError();
  const seed = sodium.crypto_generichash(32, concat(utf8(UPLOAD_KEY_LABEL), salt), driveKey);
  const pair = sodium.crypto_box_seed_keypair(seed);
  sodium.memzero(seed);
  return { publicKey: pair.publicKey, privateKey: pair.privateKey };
}

/** A contributor's browser: the file's key and name, sealed so only the owner can open them. */
export function sealContribution(uploadPublicKey: Uint8Array, meta: ContributionMeta): Uint8Array {
  return sodium.crypto_box_seal(padJson(meta, 256), uploadPublicKey);
}

export function openContribution(pair: { publicKey: Uint8Array; privateKey: Uint8Array }, sealed: Uint8Array): ContributionMeta | null {
  let plain: Uint8Array;
  try {
    plain = sodium.crypto_box_seal_open(sealed, pair.publicKey, pair.privateKey);
  } catch {
    return null;
  }
  try {
    const m = parsePadded(plain) as Record<string, unknown>;
    if (m.v !== 1 || !isStr(m.name, 1024) || !isStr(m.mime, 255) || !isCount(m.size) || !isCount(m.modified) || !isCount(m.sent)) return null;
    if (!isStr(m.key, 64) || !isStr(m.header, 64) || !isCount(m.chunk) || !isStr(m.sha) || !HEX64.test(m.sha)) return null;
    return { v: 1, name: m.name, mime: m.mime, size: m.size, modified: m.modified, sent: m.sent, key: m.key, header: m.header, chunk: m.chunk, sha: m.sha };
  } catch {
    return null;
  }
}

// --- Links ---

/** https://host/redvaultmessages/share/?id=<publicId>#<secret>. `webBase` ends with "/". */
export function shareUrl(webBase: string, publicId: string, secret: Uint8Array): string {
  return `${webBase}share/?id=${encodeURIComponent(publicId)}#${toB64Url(secret)}`;
}

/** The public id and secret from a link (or from the share page's own location). Null if either is missing or malformed. */
export function parseShareLocation(search: string, hash: string): { publicId: string; secret: Uint8Array } | null {
  const publicId = new URLSearchParams(search).get("id") ?? "";
  if (!/^[A-Za-z0-9_-]{22}$/.test(publicId)) return null;
  const fragment = hash.replace(/^#/, "");
  if (!/^[A-Za-z0-9_-]{43}$/.test(fragment)) return null;
  // Plain atob, not libsodium: this runs before libsodium has loaded.
  try {
    const binary = atob(`${fragment.replace(/-/g, "+").replace(/_/g, "/")}=`);
    const secret = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i += 1) secret[i] = binary.charCodeAt(i);
    return secret.length === 32 ? { publicId, secret } : null;
  } catch {
    return null;
  }
}

export { ready };
