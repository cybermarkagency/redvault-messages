// Call history, kept only on this device and encrypted there. The server
// never records calls (server/src/services/callService.ts), so this is the
// only record of them anywhere: who, when, how long. It's encrypted with a
// key derived from the account's private key, so it's unreadable without
// signing in, like the rest of the account's data.
import sodium from "libsodium-wrappers-sumo";
import { decryptWithConversationKey, encryptWithConversationKey, fromBase64, toBase64 } from "./e2eCrypto.js";

export type CallOutcome = "answered" | "missed" | "declined" | "unanswered" | "failed";

export interface CallRecord {
  id: string;
  /** The contact's userId (or a link guest's). */
  peerUserId: string;
  peerHandle: string;
  conversationId: string | null;
  /** Set for calls through one of your call links (or a link you joined). */
  linkName: string | null;
  kind: "voice" | "video";
  direction: "outgoing" | "incoming";
  outcome: CallOutcome;
  /** Epoch ms the call started ringing. */
  startedAt: number;
  /** Seconds connected; 0 when it never connected. */
  durationSec: number;
}

const PREFIX = "redvaultsystem:call-history:";
const MAX_RECORDS = 500;
const CHANGE_EVENT = "redvaultsystem:call-history-changed";

function storageKey(userId: string): string {
  return `${PREFIX}${userId}`;
}

function historyKey(privateKey: Uint8Array): Uint8Array {
  // A dedicated, one-way-derived key: the private key itself is never used directly.
  return sodium.crypto_generichash(32, "redvault-call-history-v1", privateKey);
}

export function loadCallHistory(userId: string, privateKey: Uint8Array): CallRecord[] {
  try {
    const raw = localStorage.getItem(storageKey(userId));
    if (!raw) return [];
    const { c, n } = JSON.parse(raw) as { c: string; n: string };
    const records = JSON.parse(decryptWithConversationKey(fromBase64(c), fromBase64(n), historyKey(privateKey))) as CallRecord[];
    return Array.isArray(records) ? records : [];
  } catch {
    return [];
  }
}

function save(userId: string, privateKey: Uint8Array, records: CallRecord[]): void {
  try {
    const { ciphertext, nonce } = encryptWithConversationKey(JSON.stringify(records.slice(0, MAX_RECORDS)), historyKey(privateKey));
    localStorage.setItem(storageKey(userId), JSON.stringify({ c: toBase64(ciphertext), n: toBase64(nonce) }));
  } catch {
    // Storage full or unavailable: the call just isn't remembered.
  }
  window.dispatchEvent(new CustomEvent(CHANGE_EVENT, { detail: userId }));
}

/** Newest first. */
export function recordCall(userId: string, privateKey: Uint8Array, record: CallRecord): void {
  const records = loadCallHistory(userId, privateKey).filter((r) => r.id !== record.id);
  save(userId, privateKey, [record, ...records].sort((a, b) => b.startedAt - a.startedAt));
}

export function clearCallHistory(userId: string, privateKey: Uint8Array): void {
  save(userId, privateKey, []);
}

export function onCallHistoryChange(listener: (userId: string) => void): () => void {
  const handler = (e: Event) => listener((e as CustomEvent<string>).detail);
  window.addEventListener(CHANGE_EVENT, handler);
  return () => window.removeEventListener(CHANGE_EVENT, handler);
}

export function formatCallDuration(seconds: number): string {
  if (seconds < 60) return `${seconds} s`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes} min`;
  return `${Math.floor(minutes / 60)} h ${minutes % 60} min`;
}
