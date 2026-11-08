// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The unlocked password vault, in memory only. Locking (Lock now, 5 minutes
// idle, the app lock, signing out) drops every decrypted item. Items are the
// same ones the desktop app shows: sealed with the account's vault key on
// the server, plus read-only copies other people shared with you.
import { useSyncExternalStore } from "react";
import {
  ApiError,
  createVaultItem,
  deleteVaultItem,
  getBreachRange,
  listVaultItems,
  listVaultSharedWithMe,
  listVaultShares,
  shareVaultItem,
  unshareVaultItem,
  updateVaultItem,
} from "./api.js";
import {
  acceptItem,
  breachCount,
  breachHash,
  checkAccountPassword,
  isHiddenItem,
  openFromSender,
  openItem,
  sealForRecipient,
  sealItem,
  secretOf,
  type VaultData,
  type VaultEntry,
  vaultKey,
} from "./passwordVault.js";

export const IDLE_LOCK_MS = 5 * 60 * 1000;

export interface VaultKeys {
  userId: string;
  publicKey: Uint8Array;
  privateKey: Uint8Array;
}

export type BreachResult = { state: "checking" } | { state: "done"; count: number } | { state: "error" };

export interface VaultState {
  status: "locked" | "loading" | "unlocked";
  entries: VaultEntry[];
  error: string | null;
  /** Breach check results by SHA-1 of the password (never the password itself). */
  breaches: Map<string, BreachResult>;
  breachCheckOff: boolean;
  /** Items left out because they aren't what this device last saw, or are in another item's place. */
  refused: number;
  /** When the vault locks if nothing happens (epoch ms). */
  locksAt: number | null;
}

const initial: VaultState = { status: "locked", entries: [], error: null, breaches: new Map(), breachCheckOff: false, refused: 0, locksAt: null };
let state: VaultState = initial;
const listeners = new Set<() => void>();
let idleTimer: ReturnType<typeof setTimeout> | null = null;
let generation = 0;

function set(change: Partial<VaultState>) {
  state = { ...state, ...change };
  listeners.forEach((l) => l());
}

export function getVaultState(): VaultState {
  return state;
}

export function useVault(): VaultState {
  return useSyncExternalStore(
    (l) => {
      listeners.add(l);
      return () => listeners.delete(l);
    },
    () => state
  );
}

// --- Locking ---

const ACTIVITY = ["pointerdown", "keydown", "wheel", "touchstart"] as const;

function onActivity() {
  if (state.status === "unlocked") armIdleTimer();
}

function armIdleTimer() {
  if (idleTimer) clearTimeout(idleTimer);
  idleTimer = setTimeout(lockVault, IDLE_LOCK_MS);
  // Only re-render for the countdown text when it moves by a minute or more.
  const locksAt = Date.now() + IDLE_LOCK_MS;
  if (!state.locksAt || Math.abs(locksAt - state.locksAt) > 60_000) set({ locksAt });
  else state = { ...state, locksAt };
}

export function lockVault(): void {
  generation += 1;
  if (idleTimer) clearTimeout(idleTimer);
  idleTimer = null;
  if (typeof window !== "undefined") ACTIVITY.forEach((e) => window.removeEventListener(e, onActivity));
  if (state.status !== "locked" || state.entries.length) set({ ...initial, breaches: new Map() });
}

/** Unlocks with the account password, checked on this device against the key from sign-in. */
export async function unlockVault(keys: VaultKeys, password: string): Promise<boolean> {
  if (!checkAccountPassword(password, keys.privateKey)) {
    set({ error: "That's not your account password." });
    return false;
  }
  await loadVault(keys);
  return state.status === "unlocked";
}

export async function loadVault(keys: VaultKeys): Promise<void> {
  const mine = ++generation;
  set({ status: "loading", error: null });
  try {
    const [items, shares, incoming] = await Promise.all([listVaultItems(), listVaultShares(), listVaultSharedWithMe()]);
    if (mine !== generation) return;
    const key = vaultKey(keys.privateKey);
    const sharedWith = new Map<string, string[]>();
    for (const s of shares.shares) sharedWith.set(s.itemId, [...(sharedWith.get(s.itemId) ?? []), s.recipientId]);
    const entries: VaultEntry[] = [];
    let refused = 0;
    for (const item of items.items) {
      try {
        const data = openItem(item.ciphertext, item.nonce, key);
        if (!acceptItem(keys.userId, item.id, data)) {
          refused += 1;
          continue;
        }
        if (isHiddenItem(data)) continue;
        entries.push({ id: item.id, revision: item.revision, data, incoming: false, ownerId: null, sharedWith: sharedWith.get(item.id) ?? [] });
      } catch {
        // Not openable with this key (should never happen): leave it out rather than show garbage.
      }
    }
    for (const item of incoming.items) {
      try {
        const data = openFromSender(item.sealed, keys.publicKey, keys.privateKey);
        if (!acceptItem(keys.userId, item.itemId, data, "shared")) {
          refused += 1;
          continue;
        }
        if (isHiddenItem(data)) continue;
        entries.push({ id: item.itemId, revision: 0, data, incoming: true, ownerId: item.ownerId, sharedWith: [] });
      } catch {
        // Skip a copy that doesn't open.
      }
    }
    set({ status: "unlocked", entries, error: null, refused });
    if (typeof window !== "undefined") ACTIVITY.forEach((e) => window.addEventListener(e, onActivity, { passive: true }));
    armIdleTimer();
    void checkBreaches(mine);
  } catch (err) {
    if (mine !== generation) return;
    set({ status: "locked", entries: [], error: err instanceof ApiError ? err.message : "Couldn't open your vault. Check your connection and try again." });
  }
}

// --- Breach check (k-anonymity) ---

const MAX_BREACH_LOOKUPS = 300;

async function checkBreaches(mine: number) {
  const byHash = new Map<string, string>();
  for (const e of state.entries) {
    const secret = secretOf(e.data);
    if (secret) byHash.set(breachHash(secret), secret);
  }
  const breaches = new Map(state.breaches);
  const hashes = [...byHash.keys()].filter((h) => !breaches.has(h)).slice(0, MAX_BREACH_LOOKUPS);
  for (const h of hashes) breaches.set(h, { state: "checking" });
  set({ breaches });
  // One prefix answers every password that shares it.
  const byPrefix = new Map<string, string[]>();
  for (const h of hashes) byPrefix.set(h.slice(0, 5), [...(byPrefix.get(h.slice(0, 5)) ?? []), h]);
  const queue = [...byPrefix.entries()];
  const worker = async () => {
    for (let next = queue.shift(); next; next = queue.shift()) {
      const [prefix, list] = next;
      if (mine !== generation) return;
      try {
        const range = await getBreachRange(prefix);
        if (mine !== generation) return;
        const updated = new Map(state.breaches);
        for (const h of list) updated.set(h, { state: "done", count: breachCount(h, range) });
        set({ breaches: updated });
      } catch (err) {
        if (mine !== generation) return;
        const off = err instanceof ApiError && err.code === "breach_check_disabled";
        const updated = new Map(state.breaches);
        for (const h of list) updated.set(h, { state: "error" });
        set({ breaches: updated, breachCheckOff: state.breachCheckOff || off });
        if (off) return;
      }
    }
  };
  await Promise.all([worker(), worker(), worker()]);
}

export function breachOf(data: VaultData): BreachResult | null {
  const secret = secretOf(data);
  return secret ? (state.breaches.get(breachHash(secret)) ?? null) : null;
}

// --- Changes ---

export class VaultConflictError extends Error {}

/** Recipients' public keys, for re-sealing their read-only copies. */
export type KeyLookup = (userId: string) => Promise<Uint8Array | null>;

/**
 * Creates or updates one of your items (sealed here), then re-seals the
 * copy of everyone it's shared with, as the desktop does.
 */
export async function saveVaultItem(keys: VaultKeys, existing: VaultEntry | null, data: VaultData, lookup: KeyLookup): Promise<VaultEntry> {
  const now = Date.now();
  const key = vaultKey(keys.privateKey);
  const unbound: VaultData = { ...data, v: 1, created: existing?.data.created ?? data.created ?? now, updated: now };
  let full = unbound;
  let stored;
  try {
    if (existing) {
      full = { ...unbound, boundId: existing.id, rev: (existing.data.rev ?? 0) + 1 };
      stored = await updateVaultItem(existing.id, existing.revision, sealItem(full, key));
    } else {
      // The server picks the id, so the item is bound in a second write. If that one fails, its next save does it.
      stored = await createVaultItem(sealItem(unbound, key));
      const bound = { ...unbound, boundId: stored.id, rev: 1 };
      const created = stored;
      stored = await updateVaultItem(created.id, created.revision, sealItem(bound, key)).catch(() => null);
      if (stored) full = bound;
      else stored = created;
    }
    acceptItem(keys.userId, stored.id, full);
  } catch (err) {
    if (err instanceof ApiError && err.code === "vault_conflict") {
      await loadVault(keys);
      throw new VaultConflictError("This item was changed on another device. It's been reloaded: check it and make your change again.");
    }
    throw err;
  }
  const entry: VaultEntry = { id: stored.id, revision: stored.revision, data: full, incoming: false, ownerId: null, sharedWith: existing?.sharedWith ?? [] };
  for (const recipientId of entry.sharedWith) {
    const publicKey = await lookup(recipientId);
    if (publicKey) await shareVaultItem(entry.id, recipientId, sealForRecipient(full, publicKey));
  }
  const entries = existing ? state.entries.map((e) => (e.id === entry.id && !e.incoming ? entry : e)) : [...state.entries, entry];
  set({ entries });
  void checkBreaches(generation);
  return entry;
}

export async function deleteVaultEntry(id: string): Promise<void> {
  await deleteVaultItem(id);
  set({ entries: state.entries.filter((e) => e.incoming || e.id !== id) });
}

export async function shareVaultEntry(entry: VaultEntry, recipientId: string, recipientPublicKey: Uint8Array): Promise<void> {
  await shareVaultItem(entry.id, recipientId, sealForRecipient(entry.data, recipientPublicKey));
  set({
    entries: state.entries.map((e) => (e.id === entry.id && !e.incoming ? { ...e, sharedWith: [...new Set([...e.sharedWith, recipientId])] } : e)),
  });
}

export async function unshareVaultEntry(entry: VaultEntry, recipientId: string): Promise<void> {
  await unshareVaultItem(entry.id, recipientId);
  set({ entries: state.entries.map((e) => (e.id === entry.id && !e.incoming ? { ...e, sharedWith: e.sharedWith.filter((r) => r !== recipientId) } : e)) });
}
