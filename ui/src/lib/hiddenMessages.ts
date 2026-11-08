// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// "Delete for me": message ids this browser hides for the signed-in user.
// Local only — the other participant still has the message, and the server
// is never told (use delete-for-everyone for that).
const PREFIX = "redvaultsystem:hidden:";
const MAX_IDS = 5000;

function key(selfUserId: string): string {
  return `${PREFIX}${selfUserId}`;
}

export function loadHidden(selfUserId: string): Set<string> {
  try {
    const raw = localStorage.getItem(key(selfUserId));
    const ids = raw ? (JSON.parse(raw) as unknown) : [];
    return new Set(Array.isArray(ids) ? ids.filter((id): id is string => typeof id === "string") : []);
  } catch {
    return new Set();
  }
}

export function hideMessage(selfUserId: string, messageId: string): void {
  try {
    const ids = [...loadHidden(selfUserId), messageId].slice(-MAX_IDS);
    localStorage.setItem(key(selfUserId), JSON.stringify(ids));
  } catch {
    // Best-effort — private browsing or storage disabled.
  }
}
