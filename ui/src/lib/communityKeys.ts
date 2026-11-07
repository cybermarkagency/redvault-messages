// The epoch keys this browser holds for each community conversation, kept in
// memory only (like every other conversation key here), plus the upkeep any
// member's app does: delivering the current key to members who are waiting
// (they paid or were let in) and, when someone left, rotating to a new one.
// The server only says WHO is entitled; it never holds a key it can open.
import { ApiError } from "./api.js";
import * as communityApi from "./communityApi.js";
import { buildKeyRing, type KeyRing, prepareEnrollments, prepareRotation } from "./communityCrypto.js";
import * as crypto from "./e2eCrypto.js";

interface Identity {
  userId: string;
  publicKey: Uint8Array;
  privateKey: Uint8Array;
}

const rings = new Map<string, KeyRing>(); // by conversation id
let identity: Identity | null = null;

export function setCommunityIdentity(next: Identity | null): void {
  if (identity?.userId !== next?.userId) rings.clear();
  identity = next;
}

export function forgetCommunityKeys(): void {
  rings.clear();
  identity = null;
}

/** The ring for a community conversation this browser has loaded, if any. */
export function communityRingFor(conversationId: string): KeyRing | undefined {
  return rings.get(conversationId);
}

/** Fetches (or re-fetches) this member's sealed keys and opens them. */
export async function loadCommunityRing(communityId: string, conversationId: string): Promise<KeyRing> {
  if (!identity) throw new Error("Not signed in");
  await crypto.ready();
  const state = await communityApi.getCommunityKeys(communityId);
  const ring = buildKeyRing(communityId, { ...state, conversationId: state.conversationId || conversationId }, identity);
  rings.set(ring.conversationId, ring);
  return ring;
}

/** The ring, refreshed if the conversation list says the community has moved on to a newer epoch. */
export async function ensureCommunityRing(communityId: string, conversationId: string, keyEpoch: number, rekeyRequired: boolean): Promise<KeyRing> {
  const known = rings.get(conversationId);
  if (known && known.currentEpoch === keyEpoch && known.rekeyRequired === rekeyRequired && known.keys.has(keyEpoch)) return known;
  return loadCommunityRing(communityId, conversationId);
}

/** Seals a new epoch key to everyone still in and installs it. Throws ApiError on a race (stale epoch, member set changed). */
export async function rotateCommunityKey(communityId: string, conversationId: string): Promise<KeyRing> {
  const ring = await loadCommunityRing(communityId, conversationId);
  const roster = await communityApi.getCommunityRoster(communityId);
  if (!roster.rekeyRequired && roster.keyEpoch !== ring.currentEpoch) return loadCommunityRing(communityId, conversationId);
  const prepared = prepareRotation(ring, roster);
  await communityApi.rotateCommunityKey(communityId, { fromEpoch: prepared.fromEpoch, keys: prepared.keys });
  return loadCommunityRing(communityId, conversationId);
}

async function enrollWaiting(communityId: string, conversationId: string): Promise<number> {
  const ring = await loadCommunityRing(communityId, conversationId);
  if (ring.rekeyRequired || !ring.keys.has(ring.currentEpoch)) return 0;
  const pending = await communityApi.listCommunityEnrollments(communityId);
  let enrolled = 0;
  for (const e of prepareEnrollments(ring, pending)) {
    try {
      await communityApi.enrollCommunityMember(communityId, e);
      enrolled++;
    } catch (err) {
      // Someone else's app got there first, or the epoch just moved: the next round sorts it out.
      if (!(err instanceof ApiError && err.status === 409)) throw err;
    }
  }
  return enrolled;
}

let running = false;

/**
 * One round of upkeep for every community this member is in. Safe to run
 * from several members' apps at once: the server takes the first rotation or
 * enrollment and refuses the rest with a 409.
 */
export async function runCommunityKeyMaintenance(): Promise<{ rotated: number; enrolled: number }> {
  if (running || !identity) return { rotated: 0, enrolled: 0 };
  running = true;
  let rotated = 0;
  let enrolled = 0;
  try {
    const tasks = await communityApi.communityKeyTasks();
    for (const task of tasks) {
      try {
        if (task.rekeyRequired) {
          if (!task.canPost) continue;
          await rotateCommunityKey(task.communityId, task.conversationId);
          rotated++;
        }
        if (task.pendingEnrollments > 0) enrolled += await enrollWaiting(task.communityId, task.conversationId);
      } catch (err) {
        if (!(err instanceof ApiError && err.status === 409)) console.warn("Community key upkeep failed", err);
      }
    }
  } finally {
    running = false;
  }
  return { rotated, enrolled };
}
