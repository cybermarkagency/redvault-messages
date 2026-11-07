// Community group keys with epochs (docs/paid-communities.md, "Encrypted
// membership"). Same primitives as every other group here — a random
// XChaCha20-Poly1305 key, sealed to each member with crypto_box_seal — plus
// one rule: whenever anyone leaves or is removed, a remaining member's app
// makes a NEW key (the next epoch) and seals it only to who remains. Nothing
// is sent under an older epoch afterwards (the server refuses it), so a
// removed member's saved keys open nothing new.
//
// Each sealed copy binds the key to its community and epoch:
//   "RVCK" | version 1 | epoch (u32, big-endian) | community id (16 bytes) | key (32 bytes)
// so the server can't hand someone an old epoch's copy as a new epoch, or
// another community's key: the app checks both before using a key, and
// refuses a "new" epoch whose key equals one it already holds.
import * as crypto from "./e2eCrypto.js";

const MAGIC = [0x52, 0x56, 0x43, 0x4b]; // "RVCK"
const VERSION = 1;
const KEY_BYTES = 32;
const PAYLOAD_BYTES = MAGIC.length + 1 + 4 + 16 + KEY_BYTES;

export class EpochKeyError extends Error {}

function uuidBytes(id: string): Uint8Array {
  const hex = id.replace(/-/g, "").toLowerCase();
  if (!/^[0-9a-f]{32}$/.test(hex)) throw new EpochKeyError("not a community id");
  const out = new Uint8Array(16);
  for (let i = 0; i < 16; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

/** The bound payload for one epoch key (exported for tests). */
export function epochKeyPayload(key: Uint8Array, epoch: number, communityId: string): Uint8Array {
  if (key.length !== KEY_BYTES) throw new EpochKeyError("community keys are 32 bytes");
  if (!Number.isInteger(epoch) || epoch < 0 || epoch > 0xffffffff) throw new EpochKeyError("bad epoch");
  const out = new Uint8Array(PAYLOAD_BYTES);
  out.set(MAGIC, 0);
  out[4] = VERSION;
  new DataView(out.buffer).setUint32(5, epoch, false);
  out.set(uuidBytes(communityId), 9);
  out.set(key, 25);
  return out;
}

/** Seals one epoch's key to one member's public key. */
export function sealEpochKey(key: Uint8Array, epoch: number, communityId: string, recipientPublicKey: Uint8Array): Uint8Array {
  return crypto.sealConversationKey(epochKeyPayload(key, epoch, communityId), recipientPublicKey);
}

/** Opens a sealed epoch key, checking it was made for exactly this community and epoch. */
export function openEpochKey(sealed: Uint8Array, epoch: number, communityId: string, identity: { publicKey: Uint8Array; privateKey: Uint8Array }): Uint8Array {
  let payload: Uint8Array;
  try {
    payload = crypto.unsealConversationKey(sealed, identity.publicKey, identity.privateKey);
  } catch {
    throw new EpochKeyError("This key wasn't sealed to you.");
  }
  if (payload.length !== PAYLOAD_BYTES || !MAGIC.every((b, i) => payload[i] === b) || payload[4] !== VERSION) {
    throw new EpochKeyError("Not a community key.");
  }
  if (new DataView(payload.buffer, payload.byteOffset).getUint32(5, false) !== epoch) throw new EpochKeyError("Key sealed for a different epoch.");
  if (!sameBytes(payload.slice(9, 25), uuidBytes(communityId))) throw new EpochKeyError("Key sealed for a different community.");
  return payload.slice(25);
}

/** Every epoch key this member holds for one community. */
export interface KeyRing {
  communityId: string;
  conversationId: string;
  /** The epoch new messages must use. */
  currentEpoch: number;
  rekeyRequired: boolean;
  keys: Map<number, Uint8Array>;
  /** Epochs whose sealed copy failed the checks above (tampering or corruption): their messages show as unreadable. */
  rejected: number[];
}

export interface KeyStateDto {
  conversationId: string;
  keyEpoch: number;
  rekeyRequired: boolean;
  keys: { epoch: number; wrappedKey: string }[];
}

export function buildKeyRing(communityId: string, state: KeyStateDto, identity: { publicKey: Uint8Array; privateKey: Uint8Array }): KeyRing {
  const keys = new Map<number, Uint8Array>();
  const rejected: number[] = [];
  for (const k of [...state.keys].sort((a, b) => a.epoch - b.epoch)) {
    try {
      const key = openEpochKey(crypto.fromBase64(k.wrappedKey), k.epoch, communityId, identity);
      // A "new" epoch with a key we already hold would let whoever held the old one read on.
      if ([...keys.values()].some((existing) => sameBytes(existing, key))) throw new EpochKeyError("Reused key");
      keys.set(k.epoch, key);
    } catch {
      rejected.push(k.epoch);
    }
  }
  return { communityId, conversationId: state.conversationId, currentEpoch: state.keyEpoch, rekeyRequired: state.rekeyRequired, keys, rejected };
}

/** Encrypts a body under the current epoch. Throws when this member doesn't hold that key (yet). */
export function encryptForCommunity(ring: KeyRing, plaintext: string): { ciphertext: Uint8Array; nonce: Uint8Array; keyEpoch: number } {
  const key = ring.keys.get(ring.currentEpoch);
  if (!key) throw new EpochKeyError("You don't have the community's current key yet.");
  const { ciphertext, nonce } = crypto.encryptWithConversationKey(plaintext, key);
  return { ciphertext, nonce, keyEpoch: ring.currentEpoch };
}

/** Null when the epoch's key isn't held (from before joining) or the body doesn't open. */
export function decryptForCommunity(ring: KeyRing, epoch: number, ciphertext: Uint8Array, nonce: Uint8Array): string | null {
  const key = ring.keys.get(epoch);
  if (!key) return null;
  try {
    return crypto.decryptWithConversationKey(ciphertext, nonce, key);
  } catch {
    return null;
  }
}

export interface RosterMember {
  userId: string;
  publicKey: string;
}

/**
 * The next epoch: a fresh random key sealed to every member on the roster
 * (who the server says is still in). Never one of the keys already held.
 */
export function prepareRotation(ring: KeyRing, roster: { keyEpoch: number; members: RosterMember[] }): { fromEpoch: number; key: Uint8Array; keys: { userId: string; wrappedKey: string }[] } {
  let key = crypto.generateConversationKey();
  while ([...ring.keys.values()].some((existing) => sameBytes(existing, key))) key = crypto.generateConversationKey();
  const epoch = roster.keyEpoch + 1;
  return {
    fromEpoch: roster.keyEpoch,
    key,
    keys: roster.members.map((m) => ({ userId: m.userId, wrappedKey: crypto.toBase64(sealEpochKey(key, epoch, ring.communityId, crypto.fromBase64(m.publicKey))) })),
  };
}

/** The current epoch's key, sealed to each member who is waiting to be let in. */
export function prepareEnrollments(ring: KeyRing, pending: RosterMember[]): { userId: string; epoch: number; wrappedKey: string }[] {
  const key = ring.keys.get(ring.currentEpoch);
  if (!key || ring.rekeyRequired) return [];
  return pending.map((p) => ({ userId: p.userId, epoch: ring.currentEpoch, wrappedKey: crypto.toBase64(sealEpochKey(key, ring.currentEpoch, ring.communityId, crypto.fromBase64(p.publicKey))) }));
}

/** Epoch 0, made by the creator's app for a community id it chose. */
export function newCommunityKey(communityId: string, creatorPublicKey: Uint8Array): { key: Uint8Array; wrappedKey: string } {
  const key = crypto.generateConversationKey();
  return { key, wrappedKey: crypto.toBase64(sealEpochKey(key, 0, communityId, creatorPublicKey)) };
}
