// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Channel view counts: each device counts a post once, whether it saw it in
// the channel or as a forwarded copy in a chat. The report carries post ids
// only (no account, no cookie), so the server learns how many devices saw a
// post, never which. The list of what this device already counted lives
// only in this browser.
import { recordChannelViews } from "./api.js";

const KEY = "redvaultsystem:channel-views";
/** Remember at most this many posts; the oldest are forgotten first. */
const MAX_REMEMBERED = 5000;
const BATCH = 100;

function remembered(): string[] {
  try {
    const raw = localStorage.getItem(KEY);
    const list = raw ? (JSON.parse(raw) as unknown) : [];
    return Array.isArray(list) ? list.filter((x): x is string => typeof x === "string") : [];
  } catch {
    return [];
  }
}

/** Reports the posts this device hasn't counted yet. Best effort: a failed report is simply not counted. */
export function countChannelViews(postIds: string[]): void {
  const seen = remembered();
  const known = new Set(seen);
  const fresh = [...new Set(postIds.map((id) => id.toLowerCase()))].filter((id) => !known.has(id));
  if (fresh.length === 0) return;
  try {
    localStorage.setItem(KEY, JSON.stringify([...seen, ...fresh].slice(-MAX_REMEMBERED)));
  } catch {
    // Without storage this device may count a post again later; that's all.
  }
  for (let i = 0; i < fresh.length; i += BATCH) {
    recordChannelViews(fresh.slice(i, i + BATCH)).catch(() => {});
  }
}
