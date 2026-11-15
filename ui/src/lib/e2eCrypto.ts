// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Browser-side E2E crypto for Redvaultsystem — same primitives and flows as
// ../../../client/src/crypto.ts (that module documents the design in full),
// re-implemented here because it can't be imported directly into a Vite
// bundle: it loads libsodium-wrappers-sumo via node:module's createRequire,
// a workaround for that package's ESM build only needed under plain Node
// (e.g. vitest for the standalone `client` package). Vite's own bundler
// resolves the package's CJS build correctly on its own, so a normal
// default import works here.
//
//   - X25519 keypair per user
//   - private key wrapped with an Argon2id-derived key (from the password)
//   - random symmetric conversation key, sealed once per participant with
//     crypto_box_seal (anonymous ECIES over X25519)
//   - message bodies encrypted with the conversation key via
//     XChaCha20-Poly1305 (AEAD)
//
// Nothing here ever sends a private key or plaintext message to the server.
import sodium from "libsodium-wrappers-sumo";

let readyPromise: Promise<typeof sodium> | null = null;

/** Must resolve before calling anything else in this module. */
export function ready(): Promise<typeof sodium> {
  if (!readyPromise) {
    readyPromise = sodium.ready.then(() => sodium);
  }
  return readyPromise;
}

export interface KeyPair {
  publicKey: Uint8Array;
  privateKey: Uint8Array;
}

export interface WrappedPrivateKey {
  encryptedPrivateKey: Uint8Array;
  nonce: Uint8Array;
  kdfSalt: Uint8Array;
  /** Missing on data from before settings were stored: those all used KDF_INTERACTIVE. */
  kdf?: KdfSettings;
}

/** Argon2id's cost: passes, and memory in bytes. */
export interface KdfSettings {
  ops: number;
  mem: number;
}

/** libsodium's INTERACTIVE values, the settings of every account so far and the lowest any app accepts. */
export const KDF_INTERACTIVE: KdfSettings = { ops: 2, mem: 67108864 };
/**
 * Used for every new wrap. Raising it takes a benchmark on real low-end phones first: Argon2 is memory-hard, and a
 * setting a phone can't finish locks that person out of their account (SECURITY_DESIGN_AND_MIGRATION.md).
 */
export const KDF_CURRENT: KdfSettings = KDF_INTERACTIVE;
const KDF_MAX: KdfSettings = { ops: 10, mem: 1073741824 };

/** Settings come from the server. Too low would make a captured sign-in key cheap to attack, too high would hang the app. */
function requireSafeKdf(kdf: KdfSettings): void {
  const inRange = kdf.ops >= KDF_INTERACTIVE.ops && kdf.ops <= KDF_MAX.ops && kdf.mem >= KDF_INTERACTIVE.mem && kdf.mem <= KDF_MAX.mem;
  if (!inRange) throw new Error("This account's sign-in details look wrong. Your password wasn't sent.");
}

export interface AeadCiphertext {
  ciphertext: Uint8Array;
  nonce: Uint8Array;
}

/** Generates a fresh X25519 keypair for E2E messaging. */
export function generateKeyPair(): KeyPair {
  const kp = sodium.crypto_box_keypair();
  return { publicKey: kp.publicKey, privateKey: kp.privateKey };
}

/**
 * Derives a symmetric key from a password + salt via Argon2id. Used both to
 * wrap the private key at registration and to unwrap it at login — the
 * caller must reuse the same salt (private_key_kdf_salt) both times.
 */
export function deriveWrapKey(password: string, kdfSalt: Uint8Array, kdf: KdfSettings = KDF_INTERACTIVE): Uint8Array {
  requireSafeKdf(kdf);
  if (kdfSalt.length !== sodium.crypto_pwhash_SALTBYTES) {
    throw new Error(`kdfSalt must be ${sodium.crypto_pwhash_SALTBYTES} bytes`);
  }
  // INTERACTIVE limits are the libsodium-recommended floor for anything run
  // in a browser/foreground UI.
  return sodium.crypto_pwhash(
    sodium.crypto_aead_xchacha20poly1305_ietf_KEYBYTES,
    password,
    kdfSalt,
    kdf.ops,
    kdf.mem,
    sodium.crypto_pwhash_ALG_ARGON2ID13
  );
}

function aeadEncrypt(plaintext: Uint8Array | string, key: Uint8Array): AeadCiphertext {
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(
    plaintext,
    null,
    null,
    nonce,
    key
  );
  return { ciphertext, nonce };
}

function aeadDecrypt(ciphertext: Uint8Array, nonce: Uint8Array, key: Uint8Array): Uint8Array {
  const plaintext = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(
    null,
    ciphertext,
    null,
    nonce,
    key
  );
  if (!plaintext) {
    throw new Error("AEAD decryption failed: wrong key, wrong nonce, or tampered ciphertext");
  }
  return plaintext;
}

/**
 * Wraps (encrypts) a private key with a password-derived key. Generates a
 * fresh random salt every call — store all three outputs
 * (encrypted_private_key, private_key_nonce, private_key_kdf_salt).
 */
export function wrapPrivateKey(privateKey: Uint8Array, password: string): WrappedPrivateKey {
  const kdfSalt = sodium.randombytes_buf(sodium.crypto_pwhash_SALTBYTES);
  const wrapKey = deriveWrapKey(password, kdfSalt, KDF_CURRENT);
  const { ciphertext, nonce } = aeadEncrypt(privateKey, wrapKey);
  return { encryptedPrivateKey: ciphertext, nonce, kdfSalt, kdf: KDF_CURRENT };
}

/** crypto_kdf context and subkey id for a key commitment. Never change: every app must derive the same value. */
const KEY_COMMITMENT_CONTEXT = "rvkeycm1";
const KEY_COMMITMENT_SUBKEY_ID = 1;

/**
 * The bytes a group key notice signs (lib/keyTrust.ts):
 * context | 0x00 | conversation id | issuer id (36 lowercase ASCII chars each) | epoch (u32 BE) | seq (u32 BE) |
 * member count (u32 BE) | each member id, sorted, 36 chars | commitment (32 bytes).
 */
const UUID_FORMAT = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

/** crypto_kdf context and subkey id for the sign-in key. Never change: every app must derive the same key. */
const SIGN_IN_KDF_CONTEXT = "rvauthk1";
const SIGN_IN_KDF_SUBKEY_ID = 1;

/**
 * The value sent to the server instead of the password (docs/auth-key-split.md).
 * One-way from the unlock key, so the server can't get from it to the private key.
 */
export function deriveSignInKey(password: string, kdfSalt: Uint8Array, kdf: KdfSettings = KDF_INTERACTIVE): string {
  const wrapKey = deriveWrapKey(password, kdfSalt, kdf);
  const signInKey = sodium.crypto_kdf_derive_from_key(32, SIGN_IN_KDF_SUBKEY_ID, SIGN_IN_KDF_CONTEXT, wrapKey);
  sodium.memzero(wrapKey);
  return toBase64(signInKey);
}

/**
 * Unwraps a private key given the password and the stored salt/nonce.
 * Throws if the password is wrong or the data was tampered with.
 */
export function unwrapPrivateKey(wrapped: WrappedPrivateKey, password: string): Uint8Array {
  const wrapKey = deriveWrapKey(password, wrapped.kdfSalt, wrapped.kdf);
  return aeadDecrypt(wrapped.encryptedPrivateKey, wrapped.nonce, wrapKey);
}

/**
 * Encrypts a message (or conversation name) body with the conversation key.
 * Passes the plaintext straight through as a string rather than pre-encoding
 * via `sodium.from_string` — this ESM build's `from_string` output fails
 * the AEAD encrypt call's own internal type check (a real bug in the
 * package, reproduced in isolation before landing this workaround); passing
 * a string directly takes a code path in the same call that works correctly.
 */
export function encryptWithConversationKey(
  plaintext: string,
  conversationKey: Uint8Array
): AeadCiphertext {
  return aeadEncrypt(plaintext, conversationKey);
}

/** Decrypts a message (or conversation name) body with the conversation key. */
export function decryptWithConversationKey(
  ciphertext: Uint8Array,
  nonce: Uint8Array,
  conversationKey: Uint8Array
): string {
  return sodium.to_string(aeadDecrypt(ciphertext, nonce, conversationKey));
}

// --- Message signatures (db/migrations/030, docs/group-keys-and-signatures.md) ---
//
// The conversation key is shared by everyone in a chat, so on its own it
// can't say *which* member wrote a message. Each account therefore also has
// an Ed25519 signing key pair, derived from its X25519 private key with
// libsodium's KDF (so every one of the account's apps gets the same pair
// without storing anything new), and every message carries a detached
// signature over messageSignatureInput below.

/** crypto_kdf context and subkey id for the signing seed. Never change: every app must derive the same key. */
const SIGNING_KDF_CONTEXT = "rvsignk1";
const SIGNING_KDF_SUBKEY_ID = 1;

const UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function compareBytes(a: Uint8Array, b: Uint8Array): number {
  for (let i = 0; i < Math.min(a.length, b.length); i++) {
    if (a[i] !== b[i]) return a[i] - b[i];
  }
  return a.length - b.length;
}

/** Every byte -> a 0-99 pair rendered as 2 decimal digits, concatenated. */
function bytesToDecimalString(bytes: Uint8Array): string {
  let out = "";
  for (const byte of bytes) {
    out += (byte % 100).toString().padStart(2, "0");
  }
  return out;
}

// --- Base64 helpers for the network/DB boundary (Postgres BYTEA <-> JSON) ---

export function toBase64(bytes: Uint8Array): string {
  return sodium.to_base64(bytes, sodium.base64_variants.ORIGINAL);
}

export function fromBase64(b64: string): Uint8Array {
  return sodium.from_base64(b64, sodium.base64_variants.ORIGINAL);
}
