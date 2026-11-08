// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Storage analytics journal: what this app has seen of the account's Files
// on earlier visits, so it can show history the metadata alone can't
// (daily totals, files deleted for good, renames and moves, restores,
// share links appearing and going away).
//
// Encrypted here with a key derived from the private key and stored on the
// server as one opaque blob per account (/api/analytics/journal, Pro and
// Premium), so it follows the account between browsers and devices without
// the server being able to read it. Only what this app observed is in it:
// it records no IP addresses, devices or locations, and never file contents.
import sodium from "libsodium-wrappers-sumo";
import { ApiError, getAnalyticsJournal, saveAnalyticsJournal } from "./api.js";
import type { BackupLinkDto } from "./api.js";
import type { DriveEntry } from "./driveMeta.js";
import { decryptWithConversationKey, encryptWithConversationKey, fromBase64, toBase64 } from "./e2eCrypto.js";

export const DAY_MS = 86_400_000;
const PAD_BUCKET = 1024;
const MAX_EVENTS = 4000;
/** Renames or restores noticed in one visit that get an event each; the rest become one "and N more". */
const MAX_NAMED_EVENTS = 40;

/** One day's totals, as seen on the last visit that day. */
export interface JournalSnapshot {
  /** Local midnight (epoch ms) of the day. */
  d: number;
  /** Storage used (bytes, as the server counts it). */
  u: number;
  /** Files (current versions, not in the Trash). */
  f: number;
  /** Bytes in the PC backup folder. */
  b: number;
  /** Bytes in the Trash. */
  t: number;
  /** Storage score that day (0–100). */
  s?: number;
}

export type AlertRule = "mass-deletion" | "mass-trashing" | "upload-spike" | "open-links";

/** Choices made on the analytics page, kept with the journal so they follow the account. */
export interface AnalyticsSettings {
  /** Alert rules switched off (all are on by default). */
  alertsOff?: AlertRule[];
  /** Alerts marked as expected. */
  dismissed?: string[];
  /** PC backup: remove previous versions older than this many days (null: keep forever). */
  keepBackupDays?: number | null;
}

export type JournalEventKind = "deleted" | "renamed" | "restored" | "link-created" | "link-removed" | "link-expired" | "cleanup";

export interface JournalEvent {
  /** Epoch ms it was noticed (or done, for cleanup). */
  t: number;
  k: JournalEventKind;
  /** A name, when there is one to show. */
  n?: string;
  /** How many items. */
  c?: number;
  /** How many bytes. */
  b?: number;
  /** Deleted: how many stored items there were before. */
  o?: number;
}

export interface AnalyticsJournal {
  v: 1;
  /** When this journal was started (epoch ms): history before this is estimated from upload dates. */
  started: number;
  snapshots: JournalSnapshot[];
  /** Short item id -> [stored bytes, name/folder fingerprint, in Trash]: enough to notice deletions, renames, moves and restores. */
  state: Record<string, [number, number, 0 | 1]>;
  /** Share link id -> when first seen (0: before the journal started), its expiry, and whether its expiry was already noted. */
  links: Record<string, { s: number; e: number | null; x?: 1 }>;
  events: JournalEvent[];
  settings?: AnalyticsSettings;
}

export function startOfDay(ms: number): number {
  const d = new Date(ms);
  d.setHours(0, 0, 0, 0);
  return d.getTime();
}

/** Local midnight `days` calendar days from the day of `ms` (not days × 24 h, which drifts across daylight-saving changes). */
export function addDays(ms: number, days: number): number {
  const d = new Date(startOfDay(ms));
  d.setDate(d.getDate() + days);
  return d.getTime();
}

/** 64 bits of the id, from both ends: random in a v4 UUID, and still distinct for ids that share a prefix. */
function shortId(id: string): string {
  const hex = id.replace(/-/g, "");
  return hex.length <= 16 ? hex : hex.slice(0, 8) + hex.slice(-8);
}

/** 32-bit FNV-1a: a fingerprint, not a secret (the whole journal is encrypted). */
function fingerprint(text: string): number {
  let hash = 0x811c9dc5;
  for (let i = 0; i < text.length; i++) {
    hash ^= text.charCodeAt(i);
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash;
}

export interface JournalInput {
  entries: DriveEntry[];
  /** Ids in the Trash, including everything inside a trashed folder. */
  inTrash: Set<string>;
  links: BackupLinkDto[];
  snapshot: Omit<JournalSnapshot, "d">;
  now: number;
  /** Pro 30 days, Premium 365. */
  retentionDays: number;
}

export function emptyJournal(now: number): AnalyticsJournal {
  return { v: 1, started: now, snapshots: [], state: {}, links: {}, events: [] };
}

/** The journal after this visit: new events from what changed since the last one, today's totals, and anything past retention dropped. */
export function updateJournal(prev: AnalyticsJournal | null, input: JournalInput): AnalyticsJournal {
  const { entries, inTrash, links, now } = input;
  const j: AnalyticsJournal = prev
    ? { ...prev, snapshots: [...prev.snapshots], links: { ...prev.links }, events: [...prev.events] }
    : emptyJournal(now);
  const byId = new Map(entries.map((e) => [e.id, e]));

  const state: AnalyticsJournal["state"] = {};
  const names = new Map<string, DriveEntry>();
  for (const e of entries) {
    const key = shortId(e.id);
    state[key] = [e.storedBytes, fingerprint(`${e.name}\u0000${e.parent}`), inTrash.has(e.id) ? 1 : 0];
    names.set(key, e);
  }

  if (prev) {
    let deleted = 0;
    let deletedBytes = 0;
    const renamed: DriveEntry[] = [];
    const restored: DriveEntry[] = [];
    for (const [key, [bytes, print, trashed]] of Object.entries(prev.state)) {
      const now_ = state[key];
      if (!now_) {
        deleted += 1;
        deletedBytes += bytes;
        continue;
      }
      const entry = names.get(key)!;
      if (trashed === 1 && now_[2] === 0) restored.push(entry);
      // Restoring can move it back to the top level: that's the restore, not a move.
      else if (now_[1] !== print && !entry.unreadable) renamed.push(entry);
    }
    if (deleted > 0) j.events.push({ t: now, k: "deleted", c: deleted, b: deletedBytes, o: Object.keys(prev.state).length });
    // A new version is a new item, not a rename; only the current name is known, so that's what's shown.
    for (const [kind, list] of [["renamed", renamed], ["restored", restored]] as const) {
      for (const e of list.slice(0, MAX_NAMED_EVENTS)) j.events.push({ t: now, k: kind, n: e.name });
      if (list.length > MAX_NAMED_EVENTS) j.events.push({ t: now, k: kind, c: list.length - MAX_NAMED_EVENTS });
    }
  }

  const current = new Set<string>();
  for (const link of links) {
    current.add(link.id);
    const expiresAt = link.expiresAt ? Date.parse(link.expiresAt) : null;
    const name = byId.get(link.backupId)?.name;
    let known = j.links[link.id];
    if (!known) {
      known = { s: prev ? now : 0, e: expiresAt };
      if (prev) j.events.push({ t: now, k: "link-created", n: name });
    }
    known = { ...known, e: expiresAt };
    if (expiresAt !== null && expiresAt <= now && !known.x) {
      known.x = 1;
      // Noted at the time it expired, not when this visit noticed it.
      if (prev) j.events.push({ t: expiresAt, k: "link-expired", n: name });
    }
    j.links[link.id] = known;
  }
  for (const [id, known] of Object.entries(j.links)) {
    if (current.has(id)) continue;
    delete j.links[id];
    const expired = known.e !== null && known.e <= now;
    if (expired && !known.x) j.events.push({ t: known.e!, k: "link-expired" });
    else if (!expired) j.events.push({ t: now, k: "link-removed" });
  }

  const day = startOfDay(now);
  j.snapshots = [...j.snapshots.filter((s) => s.d !== day), { d: day, ...input.snapshot }].sort((a, b) => a.d - b.d);
  j.state = state;
  return pruneJournal(j, now, input.retentionDays);
}

export function pruneJournal(j: AnalyticsJournal, now: number, retentionDays: number): AnalyticsJournal {
  const cutoff = addDays(now, -retentionDays);
  const events = j.events.filter((e) => e.t >= cutoff).sort((a, b) => a.t - b.t);
  return {
    ...j,
    snapshots: j.snapshots.filter((s) => s.d >= cutoff),
    events: events.slice(Math.max(0, events.length - MAX_EVENTS)),
  };
}

export function withSettings(j: AnalyticsJournal, change: (s: AnalyticsSettings) => AnalyticsSettings): AnalyticsJournal {
  const settings = change({ ...(j.settings ?? {}) });
  // Dismissed alerts are kept for a year at most.
  if (settings.dismissed && settings.dismissed.length > 500) settings.dismissed = settings.dismissed.slice(-500);
  return { ...j, settings };
}

export function addJournalEvent(j: AnalyticsJournal, event: JournalEvent): AnalyticsJournal {
  return { ...j, events: [...j.events, event] };
}

/**
 * A cleanup done from the analytics page: recorded as that, and forgotten
 * from the state so the next visit doesn't report the same items as
 * "deleted" (or raise a mass-deletion alert about the user's own cleanup).
 */
export function recordCleanup(
  j: AnalyticsJournal,
  cleanup: { label: string; itemIds: string[]; linkIds: string[]; bytes: number; count: number; now: number }
): AnalyticsJournal {
  const state = { ...j.state };
  for (const id of cleanup.itemIds) delete state[shortId(id)];
  const links = { ...j.links };
  for (const id of cleanup.linkIds) delete links[id];
  return addJournalEvent({ ...j, state, links }, { t: cleanup.now, k: "cleanup", n: cleanup.label, c: cleanup.count, b: cleanup.bytes });
}

// --- Encryption ---

function journalKey(privateKey: Uint8Array): Uint8Array {
  // A dedicated, one-way-derived key: the private key itself is never used directly.
  return sodium.crypto_generichash(32, "redvault-storage-analytics-v1", privateKey);
}

export function sealJournal(j: AnalyticsJournal, privateKey: Uint8Array): { ciphertext: string; nonce: string } {
  const json = JSON.stringify(j);
  const bytes = new TextEncoder().encode(json).length;
  // Padded so the stored size only moves in 1 KB steps.
  const padded = json + " ".repeat((Math.floor(bytes / PAD_BUCKET) + 1) * PAD_BUCKET - bytes);
  const { ciphertext, nonce } = encryptWithConversationKey(padded, journalKey(privateKey));
  return { ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) };
}

export function openJournal(sealed: { ciphertext: string; nonce: string }, privateKey: Uint8Array): AnalyticsJournal | null {
  try {
    const j = JSON.parse(decryptWithConversationKey(fromBase64(sealed.ciphertext), fromBase64(sealed.nonce), journalKey(privateKey)));
    return j && j.v === 1 && Array.isArray(j.snapshots) && Array.isArray(j.events) ? (j as AnalyticsJournal) : null;
  } catch {
    return null;
  }
}

// --- Sync with the server ---

/**
 * Reads the journal, applies `next` to it and saves the result. If another
 * device saved in between, starts over from its version (once).
 */
export async function syncJournal(
  privateKey: Uint8Array,
  next: (prev: AnalyticsJournal | null) => AnalyticsJournal
): Promise<AnalyticsJournal> {
  for (let attempt = 0; ; attempt++) {
    const { journal } = await getAnalyticsJournal();
    const prev = journal ? openJournal(journal, privateKey) : null;
    const updated = next(prev);
    try {
      await saveAnalyticsJournal({ ...sealJournal(updated, privateKey), revision: journal?.revision ?? 0 });
      return updated;
    } catch (err) {
      if (attempt === 0 && err instanceof ApiError && err.status === 409) continue;
      throw err;
    }
  }
}
