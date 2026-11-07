// Group key epochs (db/migrations/030, docs/group-keys-and-signatures.md).
//
// A group's key is numbered. When anyone leaves or is removed, the server
// refuses new messages until a remaining member's app makes a fresh random
// key for the next epoch and seals it to every remaining member with
// crypto_box_seal — the same way the group's first key was shared. The
// person who left never gets it. Older epochs' keys stay, so history stays
// readable. One-to-one chats only ever have epoch 0.
//
// This module keeps each conversation's unsealed keys for the session (the
// key ring), so every sender and reader picks the right key by epoch
// without each call site having to carry them.
import * as api from "./api.js";
import { ApiError } from "./api.js";
import * as crypto from "./e2eCrypto.js";
import { checkDirectKey, checkGroupKey, DIRECT_KEY_CHANGED, loadPin, matchesVerifiedEpoch, membersToSign, noticeSignatureOk, unconfirmedMembers } from "./keyTrust.js";
import { requireUnchangedKey } from "./verifiedContacts.js";
import { forgetKeylessSenders, resolveSenderKeys, senderSigningKey, canSignAsAccount, signAsAccount } from "./messageSigning.js";

export interface KeyRing {
  conversationId: string;
  isGroup: boolean;
  currentEpoch: number;
  /** epoch -> unsealed key */
  keys: Map<number, Uint8Array>;
  rekeyRequired: boolean;
  /** Set when the key failed a trust check: nothing may be sent, and its key isn't used. */
  keyProblem?: string;
}

interface RingOwner {
  userId: string;
  publicKey: Uint8Array;
  privateKey: Uint8Array;
}

const rings = new Map<string, KeyRing>();
let owner: RingOwner | null = null;
const rotating = new Map<string, Promise<void>>();

/** Called on sign-in: rings are only ever for this account. */
export function setKeyRingOwner(identity: RingOwner | null): void {
  if (owner?.userId !== identity?.userId) rings.clear();
  owner = identity;
}

/**
 * Unseals every key a conversation carries. Throws only if the current one
 * can't be opened; an older one that fails is skipped (its messages will
 * show as undecryptable rather than hiding the whole chat).
 */
export function keyRingFromDto(dto: api.ConversationDto, identity: RingOwner): KeyRing {
  const currentEpoch = dto.keyEpoch ?? 0;
  const keys = new Map<number, Uint8Array>();
  // A direct chat's key never changes (keyTrust.ts): only epoch 0 is ever opened, whatever else the server lists.
  if (!dto.isGroup) {
    const wrapped = currentEpoch === 0 ? dto.wrappedKey : dto.keys?.find((k) => k.epoch === 0)?.wrappedKey;
    let key: Uint8Array | undefined;
    if (currentEpoch === 0) {
      key = crypto.unsealConversationKey(crypto.fromBase64(dto.wrappedKey), identity.publicKey, identity.privateKey);
    } else if (wrapped) {
      try {
        key = crypto.unsealConversationKey(crypto.fromBase64(wrapped), identity.publicKey, identity.privateKey);
      } catch {
        // Left out: the chat just can't be read.
      }
    }
    const pinned = loadPin(identity.userId, dto.id);
    const substituted = Boolean(key && pinned && pinned.commitment !== crypto.toBase64(crypto.keyCommitment(key)));
    // A later epoch is refused without pinning anything: nothing about this answer is trusted.
    const keyProblem = substituted || currentEpoch !== 0 ? DIRECT_KEY_CHANGED : (checkDirectKey(identity.userId, dto.id, 0, key) ?? undefined);
    if (key && !substituted) keys.set(0, key);
    return { conversationId: dto.id, isGroup: false, currentEpoch: 0, keys, rekeyRequired: false, keyProblem };
  }
  keys.set(currentEpoch, crypto.unsealConversationKey(crypto.fromBase64(dto.wrappedKey), identity.publicKey, identity.privateKey));
  for (const k of dto.keys ?? []) {
    // Only older keys are history. Nothing else the server lists is a key of this group.
    if (keys.has(k.epoch) || !Number.isInteger(k.epoch) || k.epoch < 0 || k.epoch >= currentEpoch) continue;
    try {
      const key = crypto.unsealConversationKey(crypto.fromBase64(k.wrappedKey), identity.publicKey, identity.privateKey);
      if (matchesVerifiedEpoch(identity.userId, dto.id, k.epoch, key)) keys.set(k.epoch, key);
    } catch {
      // Left out: only messages from that epoch are affected.
    }
  }
  return { conversationId: dto.id, isGroup: dto.isGroup, currentEpoch, keys, rekeyRequired: dto.rekeyRequired ?? false };
}

/**
 * Checks a group's current key against its signed notice, and drops the key if it doesn't hold up: a key
 * that fails is never used, so a substituted key can neither read what's sent nor forge what's shown.
 */
export async function verifyGroupRing(ring: KeyRing, dto: api.ConversationDto, selfUserId: string): Promise<void> {
  const notice = dto.keyNotice ?? null;
  if (notice) {
    // They may have registered a signing key since this session last asked.
    forgetKeylessSenders([notice.issuerId]);
    await resolveSenderKeys(selfUserId, [notice.issuerId]);
  }
  const problem = checkGroupKey({
    conversationId: dto.id,
    selfUserId,
    epoch: ring.currentEpoch,
    key: ring.keys.get(ring.currentEpoch),
    notice,
    signingKey: senderSigningKey,
  });
  if (problem) {
    ring.keyProblem = problem;
    ring.keys.delete(ring.currentEpoch);
  }
}

/**
 * Publishes a signed notice for the group's current key and these members. Called after anything that changes
 * them, and once for a group that has none yet. Best effort: a client that loses a race has nothing to redo.
 */
export async function publishGroupNotice(
  conversationId: string,
  selfUserId: string,
  epoch: number,
  key: Uint8Array,
  members: string[],
  latest?: api.ChatKeyNotice | null
): Promise<void> {
  const signed = signGroupNotice(conversationId, selfUserId, epoch, key, members, latest);
  if (!signed) return;
  try {
    await api.publishKeyNotice(conversationId, signed);
  } catch (err) {
    // Someone else's is newer: this device picks that one up on its next load, and pins nothing now.
    if (err instanceof ApiError && err.code === "stale_key_notice") return;
    throw err;
  }
  await adoptGroupNotice(conversationId, selfUserId, epoch, key, signed);
}

/**
 * The group's latest notice, for someone who has just joined and has no pin of their own, or null if no member
 * who held this key signed it. The joiner names its members, so an unchecked list would vouch for the server's.
 */
export async function checkedLatestNotice(conversationId: string, selfUserId: string, key: Uint8Array, notice: api.ChatKeyNotice): Promise<api.ChatKeyNotice | null> {
  forgetKeylessSenders([notice.issuerId]);
  await resolveSenderKeys(selfUserId, [notice.issuerId]);
  return noticeSignatureOk(conversationId, notice, key, senderSigningKey) ? notice : null;
}

/**
 * The notice for adding someone to a group. Reads the latest notice first, because naming an out-of-date list
 * would leave out whoever joined since. Undefined until this account's signing key is registered.
 */
export async function noticeAdding(conversationId: string, epoch: number, key: Uint8Array, userId: string): Promise<api.ChatKeyNotice | undefined> {
  if (!owner) throw new Error("Not signed in");
  const dto = (await api.listConversations()).find((c) => c.id === conversationId);
  if (!dto || !dto.isGroup) throw new Error("Not a group you're in");
  const ring = keyRingFromDto(dto, owner);
  await verifyGroupRing(ring, dto, owner.userId);
  rings.set(conversationId, ring);
  if (ring.keyProblem) throw new Error(ring.keyProblem);
  const listed = (await api.listParticipants(conversationId)).map((p) => p.userId);
  const members = membersToSign(owner.userId, conversationId, listed, [userId, owner.userId], dto.keyNotice);
  return signGroupNotice(conversationId, owner.userId, epoch, key, members, dto.keyNotice) ?? undefined;
}

/** A signed notice for this key and these members, or null until this account's signing key is registered. */
export function signGroupNotice(
  conversationId: string,
  selfUserId: string,
  epoch: number,
  key: Uint8Array,
  members: string[],
  latest?: api.ChatKeyNotice | null
): api.ChatKeyNotice | null {
  if (!canSignAsAccount()) return null;
  const pin = loadPin(selfUserId, conversationId);
  const seq = Math.max(pin?.epoch === epoch ? pin.seq : 0, latest?.epoch === epoch ? latest.seq : 0) + 1;
  const notice = { epoch, seq, issuerId: selfUserId, members: [...new Set(members.map((m) => m.toLowerCase()))].sort() };
  const signature = signAsAccount(crypto.keyNoticeInput(conversationId, notice, crypto.keyCommitment(key)));
  return signature ? { ...notice, signature: crypto.toBase64(signature) } : null;
}

/** Pins a notice this device published. Only after the server has it: pinning first would lock the group if the send failed. */
export async function adoptGroupNotice(conversationId: string, selfUserId: string, epoch: number, key: Uint8Array, notice: api.ChatKeyNotice): Promise<void> {
  await resolveSenderKeys(selfUserId, [selfUserId]);
  checkGroupKey({ conversationId, selfUserId, epoch, key, notice, signingKey: senderSigningKey });
}

/** The signed-in account these rings belong to, or null. */
export function keyRingOwnerId(): string | null {
  return owner?.userId ?? null;
}

export function rememberKeyRing(ring: KeyRing): void {
  rings.set(ring.conversationId, ring);
}

export function keyRingFor(conversationId: string): KeyRing | undefined {
  return rings.get(conversationId);
}

/** The key to encrypt with now, and its epoch. Falls back to the key the caller holds for a conversation not loaded yet. */
export function currentKey(conversationId: string, fallback: Uint8Array): { key: Uint8Array; epoch: number } {
  const ring = rings.get(conversationId);
  if (ring?.keyProblem) throw new Error(ring.keyProblem);
  const key = ring?.keys.get(ring.currentEpoch);
  return ring && key ? { key, epoch: ring.currentEpoch } : { key: fallback, epoch: 0 };
}

/** Re-reads one conversation's keys from the server (after someone else rotated). */
export async function refreshKeyRing(conversationId: string): Promise<KeyRing | null> {
  if (!owner) return null;
  const dto = (await api.listConversations()).find((c) => c.id === conversationId);
  if (!dto) {
    rings.delete(conversationId);
    return null;
  }
  const ring = keyRingFromDto(dto, owner);
  if (dto.isGroup) await verifyGroupRing(ring, dto, owner.userId);
  rings.set(conversationId, ring);
  return ring;
}

/**
 * Moves a group to its next key. Any current member may do it; when two do
 * at once, the server keeps one and the other just picks up the winner's key.
 * Concurrent calls for the same group share one attempt.
 */
export function rotateGroupKey(conversationId: string): Promise<void> {
  const inFlight = rotating.get(conversationId);
  if (inFlight) return inFlight;
  const attempt = rotateOnce(conversationId).finally(() => rotating.delete(conversationId));
  rotating.set(conversationId, attempt);
  return attempt;
}

const MAX_ROTATION_ATTEMPTS = 3;

async function rotateOnce(conversationId: string): Promise<void> {
  for (let attempt = 0; attempt < MAX_ROTATION_ATTEMPTS; attempt++) {
    if (!owner) throw new Error("Not signed in");
    const dto = (await api.listConversations()).find((c) => c.id === conversationId);
    if (!dto || !dto.isGroup) throw new Error("Not a group you're in");
    const ring = keyRingFromDto(dto, owner);
    await verifyGroupRing(ring, dto, owner.userId);
    rings.set(conversationId, ring);
    if (!ring.rekeyRequired) return; // Someone else already rotated.
    if (ring.keyProblem) throw new Error(ring.keyProblem);

    const oldKey = ring.keys.get(ring.currentEpoch)!;
    const newKey = crypto.generateConversationKey();
    const participants = await api.listParticipants(conversationId);
    // The server's list isn't taken on trust: the new key is only sealed to people a signed notice named.
    const unconfirmed = unconfirmedMembers(owner.userId, conversationId, participants.map((p) => p.userId));
    if (unconfirmed.length > 0) throw new Error("This group lists someone no member confirmed, so its key wasn't changed.");
    const keys: { userId: string; wrappedKey: string }[] = [];
    for (const p of participants) {
      const user = await api.getUser(p.userId);
      if (user.deleted) continue; // A deleted account's key is gone: nothing to seal to.
      requireUnchangedKey(owner.userId, p.userId, user.publicKey);
      keys.push({ userId: p.userId, wrappedKey: crypto.toBase64(crypto.sealConversationKey(newKey, crypto.fromBase64(user.publicKey))) });
    }
    let name: { nameCiphertext: string | null; nameNonce: string | null } = { nameCiphertext: null, nameNonce: null };
    if (dto.nameCiphertext && dto.nameNonce) {
      const plain = crypto.decryptWithConversationKey(crypto.fromBase64(dto.nameCiphertext), crypto.fromBase64(dto.nameNonce), oldKey);
      const encrypted = crypto.encryptWithConversationKey(plain, newKey);
      name = { nameCiphertext: crypto.toBase64(encrypted.ciphertext), nameNonce: crypto.toBase64(encrypted.nonce) };
    }
    try {
      const keyNotice = signGroupNotice(conversationId, owner.userId, ring.currentEpoch + 1, newKey, keys.map((k) => k.userId)) ?? undefined;
      const { keyEpoch } = await api.rotateGroupKey(conversationId, { fromEpoch: ring.currentEpoch, keys, ...name, keyNotice });
      ring.keys.set(keyEpoch, newKey);
      ring.currentEpoch = keyEpoch;
      ring.rekeyRequired = false;
      if (keyNotice) await adoptGroupNotice(conversationId, owner.userId, keyEpoch, newKey, keyNotice);
      return;
    } catch (err) {
      // Lost a race (someone else rotated, or the members changed meanwhile): look again.
      if (err instanceof ApiError && (err.code === "stale_key_epoch" || err.code === "membership_changed")) continue;
      throw err;
    }
  }
  throw new Error("Couldn't give this group a new key. Try again.");
}

/** Test helper. */
export function resetKeyRings(): void {
  rings.clear();
  rotating.clear();
  owner = null;
}
