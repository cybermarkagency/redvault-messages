// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Local-only trust store: "have I confirmed this contact's safety number?"
// Verification is a judgment about a PUBLIC KEY, not a session or a
// conversation, so it's never sent to the server (which shouldn't know or
// care) and it's kept separate from `conversations` state, which is
// rebuilt from scratch every login/reload. Keyed by the key itself, not
// just the contact's userId, so a superseded/changed key never silently
// inherits old trust — the whole point of re-verifying.
const STORAGE_PREFIX = "redvaultsystem:verified:";

function storageKey(selfUserId: string, contactPublicKeyB64: string): string {
  return `${STORAGE_PREFIX}${selfUserId}:${contactPublicKeyB64}`;
}

export function isVerified(selfUserId: string, contactPublicKeyB64: string): boolean {
  try {
    return localStorage.getItem(storageKey(selfUserId, contactPublicKeyB64)) === "1";
  } catch {
    return false;
  }
}

export function setVerified(selfUserId: string, contactPublicKeyB64: string, verified: boolean): void {
  try {
    if (verified) {
      localStorage.setItem(storageKey(selfUserId, contactPublicKeyB64), "1");
    } else {
      localStorage.removeItem(storageKey(selfUserId, contactPublicKeyB64));
    }
  } catch {
    // Best-effort — private browsing, disabled storage, or quota exceeded.
  }
}

// Key-change detection. The first key seen for a contact is remembered
// (trust on first use); if a later load shows a different one, the chat
// and any call with them warn until the new safety number is verified.
// A changed key usually means they reinstalled or recovered their account,
// but it's also exactly what a server swapping in its own key would look
// like, so it's never accepted silently.
const PINNED_PREFIX = "redvaultsystem:pinned-key:";

function pinnedKey(selfUserId: string, contactUserId: string): string {
  return `${PINNED_PREFIX}${selfUserId.toLowerCase()}:${contactUserId.toLowerCase()}`;
}

/** Remembers the first key seen for this contact; true when the key now differs from the remembered one. */
export function hasKeyChanged(selfUserId: string, contactUserId: string, contactPublicKeyB64: string): boolean {
  try {
    const known = localStorage.getItem(pinnedKey(selfUserId, contactUserId));
    if (known === null) {
      localStorage.setItem(pinnedKey(selfUserId, contactUserId), contactPublicKeyB64);
      return false;
    }
    return known !== contactPublicKeyB64;
  } catch {
    return false;
  }
}

/**
 * Call before sealing anything to someone. The server hands out public keys, so one that differs from what this
 * device has seen would seal the group key, or a shared item, to whoever holds the other half.
 */
export function requireUnchangedKey(selfUserId: string, contactUserId: string, contactPublicKeyB64: string): void {
  if (hasKeyChanged(selfUserId, contactUserId, contactPublicKeyB64)) {
    throw new Error("Someone's safety number changed, so nothing was sent to them. Check it in their chat first.");
  }
}

/** The new key was verified: remember it, so the warning clears. */
export function acceptContactKey(selfUserId: string, contactUserId: string, contactPublicKeyB64: string): void {
  try {
    localStorage.setItem(pinnedKey(selfUserId, contactUserId), contactPublicKeyB64);
  } catch {
    // Best-effort, as above.
  }
}
