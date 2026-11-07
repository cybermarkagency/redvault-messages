// Account recovery key — mirrored exactly by the Android app
// (android/.../crypto/RecoveryKey.kt; the shared test vectors pin both).
//
// The key is 32 random bytes generated on the device and shown once as
// base32 in 13 dash-separated groups of 4. It never leaves the device.
// Two independent values are derived from it with keyed BLAKE2b:
//   wrapKey   = BLAKE2b-256(key = recoveryKey, "redvault-recovery-wrap-v1")
//               encrypts a second copy of the private key (XChaCha20-Poly1305);
//               the server stores that copy but can't open it.
//   authToken = BLAKE2b-256(key = recoveryKey, "redvault-recovery-auth-v1")
//               proves the key to the server, which stores only an Argon2id
//               hash of it (like a password).
import sodium from "libsodium-wrappers-sumo";
import { fromBase64, ready, toBase64 } from "./e2eCrypto.js";

const ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
const KEY_BYTES = 32;

export interface RecoveryMaterial {
  /** base64 — sent to the server, which stores only an Argon2id hash of it. */
  recoveryAuth: string;
  recoveryEncryptedPrivateKey: string;
  recoveryPrivateKeyNonce: string;
}

export function generateRecoveryKey(): Uint8Array {
  return sodium.randombytes_buf(KEY_BYTES);
}

/** "ABCD-EFGH-…" (52 base32 characters, 13 groups). */
export function formatRecoveryKey(key: Uint8Array): string {
  let bits = 0;
  let value = 0;
  let out = "";
  for (const byte of key) {
    value = (value << 8) | byte;
    bits += 8;
    while (bits >= 5) {
      out += ALPHABET[(value >>> (bits - 5)) & 31];
      bits -= 5;
    }
  }
  if (bits > 0) out += ALPHABET[(value << (5 - bits)) & 31];
  return out.match(/.{1,4}/g)!.join("-");
}

/** Accepts any spacing/dashes/case, and the look-alikes 0/1/8 for O/I/B. Null if it isn't a valid key. */
export function parseRecoveryKey(text: string): Uint8Array | null {
  const clean = text.toUpperCase().replace(/[^A-Z0-9]/g, "").replace(/0/g, "O").replace(/1/g, "I").replace(/8/g, "B");
  if (clean.length !== 52) return null;
  const out = new Uint8Array(KEY_BYTES);
  let bits = 0;
  let value = 0;
  let index = 0;
  for (const ch of clean) {
    const v = ALPHABET.indexOf(ch);
    if (v < 0) return null;
    value = (value << 5) | v;
    bits += 5;
    if (bits >= 8) {
      if (index < KEY_BYTES) out[index] = (value >>> (bits - 8)) & 0xff;
      index += 1;
      bits -= 8;
    }
  }
  // 52 chars = 260 bits: the 4 spare bits must be zero, as formatRecoveryKey writes them.
  if (index !== KEY_BYTES || (value & ((1 << bits) - 1)) !== 0) return null;
  return out;
}

export function deriveRecoveryKeys(key: Uint8Array): { wrapKey: Uint8Array; authToken: Uint8Array } {
  return {
    wrapKey: sodium.crypto_generichash(32, "redvault-recovery-wrap-v1", key),
    authToken: sodium.crypto_generichash(32, "redvault-recovery-auth-v1", key),
  };
}

/** What to send the server so this recovery key can restore the account's private key later. */
export async function recoveryMaterialFor(key: Uint8Array, privateKey: Uint8Array): Promise<RecoveryMaterial> {
  await ready();
  const { wrapKey, authToken } = deriveRecoveryKeys(key);
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  const sealed = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(privateKey, null, null, nonce, wrapKey);
  return { recoveryAuth: toBase64(authToken), recoveryEncryptedPrivateKey: toBase64(sealed), recoveryPrivateKeyNonce: toBase64(nonce) };
}

export function recoveryAuthFor(key: Uint8Array): string {
  return toBase64(deriveRecoveryKeys(key).authToken);
}

/**
 * Unwraps the private key with the recovery key and checks it really is
 * this account's key (its public key must match), so a wrong or tampered
 * copy can never be adopted. Throws on any mismatch.
 */
export function unwrapWithRecoveryKey(
  key: Uint8Array,
  encryptedPrivateKeyB64: string,
  nonceB64: string,
  publicKeyB64: string
): Uint8Array {
  const { wrapKey } = deriveRecoveryKeys(key);
  const privateKey = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, fromBase64(encryptedPrivateKeyB64), null, fromBase64(nonceB64), wrapKey);
  const derivedPublic = sodium.crypto_scalarmult_base(privateKey);
  if (toBase64(derivedPublic) !== publicKeyB64) {
    throw new Error("Recovered key doesn't match this account");
  }
  return privateKey;
}
