// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Local-only convenience: remembers the handle (never the password or any
// key material) of whoever last signed in on this browser, so a reload
// doesn't look like "the account vanished" — it lands on the Log in tab
// with the handle already filled in, instead of defaulting back to Create
// a new identity, where someone can easily register a second account by
// mistake without realizing their original one is still there.
const STORAGE_KEY = "redvaultsystem:lastHandle";

export function loadLastHandle(): string | null {
  try {
    return localStorage.getItem(STORAGE_KEY);
  } catch {
    return null;
  }
}

export function saveLastHandle(handle: string): void {
  try {
    localStorage.setItem(STORAGE_KEY, handle);
  } catch {
    // Best-effort — private browsing, disabled storage, or quota exceeded.
  }
}
