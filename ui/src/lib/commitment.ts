// Client-side half of stage-3 "Transaction verified" — see
// server/src/crypto/commitment.ts, which this MUST match byte-for-byte.
// Computes the same canonical-string SHA-256 commitment over the exact
// encrypted envelope {messageId, ciphertext, nonce} that the server
// independently computes from what it persisted, so the sender's client can
// compare its own local commitment against the server's without trusting
// the server's word for it.
import sodium from "libsodium-wrappers-sumo";
import { toBase64 } from "./e2eCrypto.js";

export const COMMITMENT_PROTOCOL_VERSION = 1;

export interface EncryptedEnvelope {
  messageId: string; // UUID, as assigned by the server on insert
  ciphertext: Uint8Array;
  nonce: Uint8Array;
}

/**
 * Canonical byte representation that gets hashed — MUST stay identical to
 * server/src/crypto/commitment.ts's canonicalEnvelopeString. See that
 * file's comment for the full rationale (why colon-joined base64, not
 * JSON, not raw concatenation).
 *
 * v1 = "v1:" + messageId + ":" + base64(ciphertext) + ":" + base64(nonce)
 */
export function canonicalEnvelopeString(
  envelope: EncryptedEnvelope,
  protocolVersion: number = COMMITMENT_PROTOCOL_VERSION
): string {
  if (protocolVersion !== COMMITMENT_PROTOCOL_VERSION) {
    throw new Error(`Unsupported commitment protocol version: ${protocolVersion}`);
  }
  return `v${protocolVersion}:${envelope.messageId}:${toBase64(envelope.ciphertext)}:${toBase64(envelope.nonce)}`;
}

/**
 * SHA-256 of canonicalEnvelopeString(). Caller must have already awaited
 * e2eCrypto's `ready()`. Hashes the canonical STRING, not raw bytes —
 * libsodium-wrappers-sumo's ESM build has a real bug where
 * crypto_hash_sha256 rejects a Uint8Array built by plain JS concatenation
 * (fails its own internal type check); passing a string always takes the
 * working code path. This is the same workaround e2eCrypto.ts's
 * safetyNumberDigits already uses for the same reason — see that function's
 * comment for the fuller explanation.
 */
export function computeCommitment(
  envelope: EncryptedEnvelope,
  protocolVersion: number = COMMITMENT_PROTOCOL_VERSION
): Uint8Array {
  return sodium.crypto_hash_sha256(canonicalEnvelopeString(envelope, protocolVersion));
}

export function commitmentToHex(commitment: Uint8Array): string {
  return Array.from(commitment)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

/** True when the client's own recomputed commitment matches what the server/transparency record reports. Plain string equality is fine here — a commitment is a public hash, not a secret, so there's no timing-attack concern. */
export function commitmentsMatch(localHex: string, networkHex: string): boolean {
  return localHex.toLowerCase() === networkHex.toLowerCase();
}
