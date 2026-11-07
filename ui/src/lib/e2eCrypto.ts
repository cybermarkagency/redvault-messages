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
export const KEY_NOTICE_CONTEXT = "redvault-group-key-v1";

/** A public fingerprint of a conversation key. One-way: it shows two people hold the same key without revealing it. */
export function keyCommitment(conversationKey: Uint8Array): Uint8Array {
  return sodium.crypto_kdf_derive_from_key(32, KEY_COMMITMENT_SUBKEY_ID, KEY_COMMITMENT_CONTEXT, conversationKey);
}

/**
 * The bytes a group key notice signs (lib/keyTrust.ts):
 * context | 0x00 | conversation id | issuer id (36 lowercase ASCII chars each) | epoch (u32 BE) | seq (u32 BE) |
 * member count (u32 BE) | each member id, sorted, 36 chars | commitment (32 bytes).
 */
const UUID_FORMAT = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

export function keyNoticeInput(
  conversationId: string,
  notice: { epoch: number; seq: number; issuerId: string; members: string[] },
  commitment: Uint8Array
): Uint8Array {
  const members = notice.members.map((m) => m.toLowerCase()).sort();
  // Anything else wouldn't survive the encoding below: 2^32 + 1 would sign and verify the same as 1.
  if (![conversationId, notice.issuerId, ...members].every((id) => UUID_FORMAT.test(id.toLowerCase()))) throw new Error("bad id in a key notice");
  if (![notice.epoch, notice.seq].every((n) => Number.isInteger(n) && n >= 0 && n <= 0xffffffff)) throw new Error("bad epoch or sequence number");
  const text = new TextEncoder();
  const head = text.encode(`${KEY_NOTICE_CONTEXT}\u0000${conversationId.toLowerCase()}${notice.issuerId.toLowerCase()}`);
  const ids = text.encode(members.join(""));
  const out = new Uint8Array(head.length + 12 + ids.length + commitment.length);
  out.set(head, 0);
  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  view.setUint32(head.length, notice.epoch);
  view.setUint32(head.length + 4, notice.seq);
  view.setUint32(head.length + 8, members.length);
  out.set(ids, head.length + 12);
  out.set(commitment, head.length + 12 + ids.length);
  return out;
}

/** The public key that goes with a private key. */
export function publicKeyOf(privateKey: Uint8Array): Uint8Array {
  return sodium.crypto_scalarmult_base(privateKey);
}

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

/** Generates a fresh random symmetric key for a new conversation. */
export function generateConversationKey(): Uint8Array {
  return sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
}

/**
 * Seals a conversation key to one participant's public key
 * (crypto_box_seal: anonymous, one-way ECIES — only that participant's
 * matching private key can open it). Call once per participant.
 */
export function sealConversationKey(
  conversationKey: Uint8Array,
  recipientPublicKey: Uint8Array
): Uint8Array {
  return sodium.crypto_box_seal(conversationKey, recipientPublicKey);
}

/** Unseals a conversation key using the recipient's own keypair. */
export function unsealConversationKey(
  sealed: Uint8Array,
  recipientPublicKey: Uint8Array,
  recipientPrivateKey: Uint8Array
): Uint8Array {
  const opened = sodium.crypto_box_seal_open(sealed, recipientPublicKey, recipientPrivateKey);
  if (!opened) {
    throw new Error("Failed to unseal conversation key: wrong keypair or corrupted data");
  }
  return opened;
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
export const MESSAGE_SIGNATURE_CONTEXT = "redvault-message-signature-v1";

export interface SigningKeyPair {
  publicKey: Uint8Array;
  /** libsodium's 64-byte Ed25519 secret key (seed ‖ public key). */
  secretKey: Uint8Array;
}

export function deriveSigningKeyPair(accountPrivateKey: Uint8Array): SigningKeyPair {
  const seed = sodium.crypto_kdf_derive_from_key(sodium.crypto_sign_SEEDBYTES, SIGNING_KDF_SUBKEY_ID, SIGNING_KDF_CONTEXT, accountPrivateKey);
  const pair = sodium.crypto_sign_seed_keypair(seed);
  sodium.memzero(seed);
  return { publicKey: pair.publicKey, secretKey: pair.privateKey };
}

const UUID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

/**
 * The exact bytes a message signature covers — identical on every app and on
 * the server (server/src/crypto/messageSignature.ts):
 * context ‖ 0x00 ‖ conversation id ‖ sender id (36 lowercase ASCII chars
 * each) ‖ key epoch (u32 BE) ‖ nonce length (u32 BE) ‖ nonce ‖ ciphertext.
 */
export function messageSignatureInput(
  conversationId: string,
  senderId: string,
  keyEpoch: number,
  nonce: Uint8Array,
  ciphertext: Uint8Array
): Uint8Array {
  const conversation = conversationId.toLowerCase();
  const sender = senderId.toLowerCase();
  if (!UUID_PATTERN.test(conversation) || !UUID_PATTERN.test(sender)) throw new Error("ids must be UUIDs");
  if (!Number.isInteger(keyEpoch) || keyEpoch < 0 || keyEpoch > 0xffffffff) throw new Error("bad key epoch");
  const head = new TextEncoder().encode(`${MESSAGE_SIGNATURE_CONTEXT}\u0000${conversation}${sender}`);
  const out = new Uint8Array(head.length + 8 + nonce.length + ciphertext.length);
  out.set(head, 0);
  const view = new DataView(out.buffer, out.byteOffset, out.byteLength);
  view.setUint32(head.length, keyEpoch);
  view.setUint32(head.length + 4, nonce.length);
  out.set(nonce, head.length + 8);
  out.set(ciphertext, head.length + 8 + nonce.length);
  return out;
}

export function signMessage(input: Uint8Array, secretKey: Uint8Array): Uint8Array {
  return sodium.crypto_sign_detached(input, secretKey);
}

export function verifyMessageSignature(signature: Uint8Array, input: Uint8Array, publicKey: Uint8Array): boolean {
  if (signature.length !== sodium.crypto_sign_BYTES || publicKey.length !== sodium.crypto_sign_PUBLICKEYBYTES) return false;
  try {
    return sodium.crypto_sign_verify_detached(signature, input, publicKey);
  } catch {
    return false;
  }
}

/**
 * Order-independent 60-digit fingerprint of both participants' real public
 * keys — the safety number two people compare out-of-band to make sure
 * neither is talking to a swapped/MITM'd key. (Deliberately not the UI's
 * generateSafetyNumber, which is display-string-seeded and cosmetic only.)
 */
export function safetyNumberDigits(publicKeyA: Uint8Array, publicKeyB: Uint8Array): string[] {
  const [first, second] = [publicKeyA, publicKeyB].sort(compareBytes);
  // Hash a joined base64 *string* rather than a concatenated byte buffer —
  // crypto_hash_sha256's message-type check rejects Uint8Arrays built by
  // plain JS code (same underlying bug worked around in aeadEncrypt above),
  // but a string always takes the working code path.
  const message = `${toBase64(first)}:${toBase64(second)}`;
  const hash = sodium.crypto_hash_sha256(message);
  const digits = bytesToDecimalString(hash.subarray(0, 30));
  const groups: string[] = [];
  for (let i = 0; i < 12; i++) {
    groups.push(digits.slice(i * 5, i * 5 + 5));
  }
  return groups;
}

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
