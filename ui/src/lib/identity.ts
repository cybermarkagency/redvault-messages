// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

const ADJECTIVES = [
  "quiet",
  "still",
  "hollow",
  "amber",
  "cinder",
  "pale",
  "drift",
  "ember",
  "frost",
  "dusky",
  "salt",
  "brave",
  "lone",
  "hidden",
  "faint",
  "slow",
  "deep",
  "clear",
  "soft",
  "far",
];

const NOUNS = [
  "harbor",
  "grove",
  "ridge",
  "hollow",
  "meadow",
  "summit",
  "brook",
  "thicket",
  "ferry",
  "lantern",
  "orchard",
  "quarry",
  "hearth",
  "tundra",
  "glacier",
  "valley",
  "bramble",
  "willow",
  "canyon",
  "marsh",
];

const HEX = "0123456789abcdef";

function pick<T>(items: T[], rng: () => number): T {
  return items[Math.floor(rng() * items.length)];
}

/**
 * Generates a fresh candidate handle locally — nothing is sent anywhere.
 * In production the glyph is seeded from the actual public key; the handle
 * text itself is just a human-readable label alongside it.
 */
export function generateHandle(rng: () => number = Math.random): string {
  const adjective = pick(ADJECTIVES, rng);
  const noun = pick(NOUNS, rng);
  const suffix = Array.from({ length: 4 }, () => pick(HEX.split(""), rng)).join("");
  return `${adjective}-${noun}·${suffix}`;
}
