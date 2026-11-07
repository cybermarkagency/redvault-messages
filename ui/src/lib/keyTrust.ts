// Trust in protocol 1 conversation keys (SECURITY_DESIGN_AND_MIGRATION.md, C-2 and H-2).
//
// crypto_box_seal says nothing about who sealed a key, so the server could
// hand out a key of its own. Two rules stop that:
//   - A direct chat's key never changes: this device pins the key it first
//     saw, and a new epoch or a different key is refused.
//   - A group's key comes with a key notice, signed by the member who issued
//     it, naming the conversation, the epoch, the members and a commitment to
//     the key. A new epoch's issuer must be in the member list this device
//     last verified, and epochs and sequence numbers never go backwards.
// First contact is still trust on first use: a device has nothing earlier to
// compare with. Protocol 3 replaces all of this for chats that use it.
import type { ChatKeyNotice } from "./api.js";
import * as crypto from "./e2eCrypto.js";

export interface KeyPin {
  epoch: number;
  seq: number;
  /** base64 commitment to the key at `epoch`. */
  commitment: string;
  members: string[];
  /** Commitments to the epochs before `epoch` that this device verified, so they can't be swapped later. */
  past?: Record<string, string>;
}

const PIN_PREFIX = "redvaultsystem:key-pin:";
// Lowercased, like every id compared here: another spelling must never find no pin and start over.
const pinName = (selfUserId: string, conversationId: string) => `${PIN_PREFIX}${selfUserId.toLowerCase()}:${conversationId.toLowerCase()}`;

export function loadPin(selfUserId: string, conversationId: string): KeyPin | null {
  try {
    const raw = localStorage.getItem(pinName(selfUserId, conversationId));
    return raw ? (JSON.parse(raw) as KeyPin) : null;
  } catch {
    return null;
  }
}

export function savePin(selfUserId: string, conversationId: string, pin: KeyPin): void {
  try {
    localStorage.setItem(pinName(selfUserId, conversationId), JSON.stringify(pin));
  } catch {
    // Best effort: private browsing or disabled storage. There's then nothing to compare with next time.
  }
}

export const DIRECT_KEY_CHANGED = "This chat's key was changed, which should never happen. Nothing can be sent.";

/** What's wrong with a direct chat's key, or null. Pins the key the first time. */
export function checkDirectKey(selfUserId: string, conversationId: string, epoch: number, key: Uint8Array | undefined): string | null {
  if (epoch !== 0) return DIRECT_KEY_CHANGED;
  if (!key) return "This chat's key couldn't be opened.";
  const commitment = crypto.toBase64(crypto.keyCommitment(key));
  const pin = loadPin(selfUserId, conversationId);
  if (pin && pin.commitment !== commitment) return DIRECT_KEY_CHANGED;
  if (!pin) savePin(selfUserId, conversationId, { epoch: 0, seq: 0, commitment, members: [] });
  return null;
}

export interface GroupKeyCheck {
  conversationId: string;
  selfUserId: string;
  epoch: number;
  /** The key the server's copy opens to, for `epoch`. */
  key: Uint8Array | undefined;
  notice: ChatKeyNotice | null;
  /** The pinned signing key of a user, or null when unknown. */
  signingKey: (userId: string) => Uint8Array | null;
}

const sameMembers = (a: string[], b: string[]) => {
  const x = [...new Set(a.map((m) => m.toLowerCase()))].sort();
  const y = [...new Set(b.map((m) => m.toLowerCase()))].sort();
  return x.length === y.length && x.every((m, i) => m === y[i]);
};

/**
 * Whether a member who held `key` signed this notice. The signature covers a commitment to the key that's never
 * sent, so nobody without the key can make one that passes, even with a signing key of their own.
 */
export function noticeSignatureOk(conversationId: string, notice: ChatKeyNotice, key: Uint8Array, signingKey: (userId: string) => Uint8Array | null): boolean {
  const issuerKey = signingKey(notice.issuerId);
  if (!issuerKey) return false;
  try {
    const input = crypto.keyNoticeInput(conversationId, notice, crypto.keyCommitment(key));
    return crypto.verifyMessageSignature(crypto.fromBase64(notice.signature), input, issuerKey);
  } catch {
    return false;
  }
}

/** What's wrong with a group's current key, or null. Updates the pin when the notice checks out. */
export function checkGroupKey(check: GroupKeyCheck): string | null {
  const { conversationId, selfUserId, epoch, key, notice, signingKey } = check;
  const pin = loadPin(selfUserId, conversationId);
  if (!notice) {
    // Groups from before key notices have none, until a member's app publishes the first. Once this device has
    // seen one, its absence means someone removed it.
    return pin ? "This group's key couldn't be verified. Nothing can be sent." : null;
  }
  if (!key) return "This group's key couldn't be opened.";
  const problem = "This group's key couldn't be verified. Nothing can be sent.";
  if (notice.epoch !== epoch) return problem;
  if (pin && (notice.epoch < pin.epoch || (notice.epoch === pin.epoch && notice.seq < pin.seq))) return problem;
  const commitment = crypto.keyCommitment(key);
  // Two notices never share a sequence number, so this one has to be the notice already seen.
  if (pin && notice.epoch === pin.epoch && notice.seq === pin.seq && !sameMembers(notice.members, pin.members)) return problem;
  // The same epoch has to mean the same key.
  if (pin && notice.epoch === pin.epoch && pin.commitment !== crypto.toBase64(commitment)) return problem;
  // Someone outside the group can't start a new epoch. An unchanged key proves the issuer already holds it.
  if (pin && notice.epoch > pin.epoch && !pin.members.includes(notice.issuerId.toLowerCase())) return problem;
  if (!notice.members.map((m) => m.toLowerCase()).includes(notice.issuerId.toLowerCase())) return problem;
  if (!notice.members.map((m) => m.toLowerCase()).includes(selfUserId.toLowerCase())) return problem;
  if (!noticeSignatureOk(conversationId, notice, key, signingKey)) return problem;
  savePin(selfUserId, conversationId, {
    epoch: notice.epoch,
    seq: notice.seq,
    commitment: crypto.toBase64(commitment),
    members: notice.members.map((m) => m.toLowerCase()),
    past: pin && notice.epoch > pin.epoch ? { ...pin.past, [pin.epoch]: pin.commitment } : pin?.past,
  });
  return null;
}

/** False for a key that isn't the one this device verified at `epoch`. An epoch it never verified can't be checked. */
export function matchesVerifiedEpoch(selfUserId: string, conversationId: string, epoch: number, key: Uint8Array): boolean {
  const pin = loadPin(selfUserId, conversationId);
  const verified = pin?.epoch === epoch ? pin.commitment : pin?.past?.[epoch];
  return !verified || verified === crypto.toBase64(crypto.keyCommitment(key));
}

/**
 * Who a new notice names. The server's list isn't taken on trust: anyone it added that no notice named would
 * otherwise be confirmed by someone else's routine change. So it's the last verified list, or `latest` for someone
 * joining who has none of their own, less whoever the server no longer lists, plus `added`.
 */
export function membersToSign(selfUserId: string, conversationId: string, listed: string[], added: string[], latest?: ChatKeyNotice | null): string[] {
  const current = listed.map((m) => m.toLowerCase());
  const pin = loadPin(selfUserId, conversationId);
  const verified = pin?.members.length ? pin.members : latest?.members.map((m) => m.toLowerCase());
  const kept = verified ? verified.filter((m) => current.includes(m)) : current;
  return [...new Set([...kept, ...added.map((m) => m.toLowerCase())])];
}

/** People the server lists who aren't in the last verified notice. A rotation mustn't seal the key to them. */
export function unconfirmedMembers(selfUserId: string, conversationId: string, participants: string[]): string[] {
  const pin = loadPin(selfUserId, conversationId);
  if (!pin || pin.members.length === 0) return [];
  return participants.filter((p) => !pin.members.includes(p.toLowerCase()));
}
