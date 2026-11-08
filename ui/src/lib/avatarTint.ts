// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Initials-on-tint avatars from the design. The tint is picked from the
// handle, so the same person always gets the same color.

const TINTS = ["#C9B6E4", "#9CC9E8", "#F2C38B", "#B9D98C", "#E8A7A0", "#E6D48A"];

export function avatarTint(seed: string): string {
  let hash = 0;
  for (let i = 0; i < seed.length; i++) hash = (hash * 31 + seed.charCodeAt(i)) >>> 0;
  return TINTS[hash % TINTS.length];
}

/** "maya.okafor" -> "MO", "quiet-harbor·3f2a" -> "QH", "Design Team" -> "DT". */
export function initials(name: string): string {
  const words = name
    .replace(/·.*$/, "")
    .split(/[\s._-]+/)
    .filter((w) => /[A-Za-z0-9]/.test(w));
  const letters = words.length >= 2 ? words[0][0] + words[1][0] : (words[0] ?? "?").slice(0, 2);
  return letters.toUpperCase();
}
