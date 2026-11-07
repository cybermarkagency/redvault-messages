// End-to-end encryption for group calls.
//
// Group calls go through a LiveKit SFU, which forwards each person's media
// to the others. With DTLS alone the SFU would see and hear everything, so
// every audio/video frame is also encrypted in this browser (LiveKit's E2EE,
// AES-GCM in a worker) under a room key the server never holds:
//
//   - The first person in the call (the "keyholder") makes a random 32-byte
//     key for each epoch and seals it with crypto_box to every other
//     participant's identity key, the same key their safety number covers.
//     The server relays the sealed copies but can't open or forge them.
//   - The server bumps the epoch whenever someone joins or leaves, and the
//     keyholder sends a fresh key. Someone who left can't decrypt what comes
//     after; someone who joined can't decrypt what came before.
//   - The sealed payload names the call, group, epoch, sender and recipient,
//     and is refused if any don't match or it's stale, so it can't be
//     replayed into another call or epoch.
import sodium from "libsodium-wrappers-sumo";
import { BaseKeyProvider } from "livekit-client";
import type { SealedSignalDto } from "./api.js";
import { fromBase64, toBase64 } from "./e2eCrypto.js";

export const ROOM_KEY_BYTES = 32;
/** LiveKit's default keyring size: epochs map onto key slots modulo this. */
const KEYRING_SIZE = 16;
const MAX_KEY_AGE_MS = 5 * 60 * 1000;

interface RoomKeyMessage {
  v: 1;
  type: "group-call-key";
  callId: string;
  conversationId: string;
  epoch: number;
  from: string;
  to: string;
  key: string;
  sentAt: number;
}

export class GroupKeyError extends Error {}

export function newRoomKey(): Uint8Array {
  return sodium.randombytes_buf(ROOM_KEY_BYTES);
}

export function sealRoomKey(
  message: { callId: string; conversationId: string; epoch: number; from: string; to: string },
  key: Uint8Array,
  recipientPublicKey: Uint8Array,
  senderPrivateKey: Uint8Array
): SealedSignalDto {
  const payload: RoomKeyMessage = { v: 1, type: "group-call-key", ...message, key: toBase64(key), sentAt: Date.now() };
  const nonce = sodium.randombytes_buf(sodium.crypto_box_NONCEBYTES);
  // A string, not pre-encoded bytes: see e2eCrypto.encryptWithConversationKey.
  const ciphertext = sodium.crypto_box_easy(JSON.stringify(payload), nonce, recipientPublicKey, senderPrivateKey);
  return { ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) };
}

/** Opens a room key from `senderPublicKey` and checks it's exactly the one expected. Throws GroupKeyError otherwise. */
export function openRoomKey(
  sealed: SealedSignalDto,
  senderPublicKey: Uint8Array,
  recipientPrivateKey: Uint8Array,
  expected: { callId: string; conversationId: string; epoch: number; from: string; to: string },
  now = Date.now()
): Uint8Array {
  let message: RoomKeyMessage;
  try {
    const opened = sodium.crypto_box_open_easy(fromBase64(sealed.ciphertext), fromBase64(sealed.nonce), senderPublicKey, recipientPrivateKey);
    message = JSON.parse(sodium.to_string(opened)) as RoomKeyMessage;
  } catch {
    throw new GroupKeyError("The call key couldn't be verified as coming from the keyholder's identity key.");
  }
  const matches =
    message.v === 1 &&
    message.type === "group-call-key" &&
    message.callId === expected.callId &&
    message.conversationId === expected.conversationId &&
    message.epoch === expected.epoch &&
    message.from === expected.from &&
    message.to === expected.to &&
    typeof message.key === "string";
  if (!matches) throw new GroupKeyError("The call key didn't match this call.");
  if (typeof message.sentAt !== "number" || Math.abs(now - message.sentAt) > MAX_KEY_AGE_MS) {
    throw new GroupKeyError("The call key is too old.");
  }
  const key = fromBase64(message.key);
  if (key.length !== ROOM_KEY_BYTES) throw new GroupKeyError("The call key has the wrong length.");
  return key;
}

/**
 * LiveKit key provider for one shared room key per epoch. Each epoch goes in
 * its own key slot, so frames already in flight under the previous key still
 * decrypt while everyone switches over. Never ratchets or guesses keys.
 */
export class RoomKeyProvider extends BaseKeyProvider {
  constructor() {
    super({ sharedKey: true, ratchetWindowSize: 0, failureTolerance: -1, keyringSize: KEYRING_SIZE });
  }

  async setRoomKey(key: Uint8Array, epoch: number): Promise<void> {
    const material = await crypto.subtle.importKey("raw", new Uint8Array(key), "HKDF", false, ["deriveBits", "deriveKey"]);
    this.onSetEncryptionKey(material, undefined, epoch % KEYRING_SIZE);
  }
}
