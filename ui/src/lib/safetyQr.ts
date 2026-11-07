// The safety number as a QR code, for verifying in person: each phone
// shows "my key, and the key I have for you"; scanning the other phone's
// code checks both halves at once. A match proves neither side's key was
// swapped in transit (by the server or anyone else).
import { toBase64 } from "./e2eCrypto.js";

const PREFIX = "redvault-verify:1:";

/** What this device shows: its own public key, then the key it holds for the contact. */
export function safetyQrPayload(myPublicKey: Uint8Array, theirPublicKey: Uint8Array): string {
  return `${PREFIX}${toBase64(myPublicKey)}:${toBase64(theirPublicKey)}`;
}

export type SafetyQrResult = "match" | "mismatch" | "not-a-safety-code";

/** Checks the code scanned from the contact's screen against the keys this device holds. */
export function checkSafetyQr(scanned: string, myPublicKey: Uint8Array, theirPublicKey: Uint8Array): SafetyQrResult {
  if (!scanned.startsWith(PREFIX)) return "not-a-safety-code";
  const [theirOwnKey, theirCopyOfMine, ...rest] = scanned.slice(PREFIX.length).split(":");
  if (!theirOwnKey || !theirCopyOfMine || rest.length > 0) return "not-a-safety-code";
  return theirOwnKey === toBase64(theirPublicKey) && theirCopyOfMine === toBase64(myPublicKey) ? "match" : "mismatch";
}
