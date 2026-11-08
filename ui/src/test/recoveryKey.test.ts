// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, describe, expect, it } from "vitest";
import { generateKeyPair, ready, toBase64 } from "../lib/e2eCrypto.js";
import {
  deriveRecoveryKeys,
  formatRecoveryKey,
  generateRecoveryKey,
  parseRecoveryKey,
  recoveryMaterialFor,
  unwrapWithRecoveryKey,
} from "../lib/recoveryKey.js";

// Shared with android/.../crypto/RecoveryKeyTest.kt: both must derive exactly these.
const FIXED_KEY = Uint8Array.from({ length: 32 }, (_, i) => (i * 13 + 5) & 0xff);
const FIXED_FORMATTED = "AUJB-6LBZ-IZJW-A3L2-Q6KK-DLV3-ZDK6-F374-BELC-GMB5-JJLW-I4L6-ROMA";
const FIXED_WRAP = "hNkdtJ9WVWt9NeRT9e0PbW25zE/F7VmLxjWUqY12lwM=";
const FIXED_AUTH = "7N3QawoTizJf9BpyLEIA7jfSfleT9oISJi8CM8b1pSI=";

beforeAll(async () => {
  await ready();
});

describe("recovery key", () => {
  it("derives the shared test vector", () => {
    expect(formatRecoveryKey(FIXED_KEY)).toBe(FIXED_FORMATTED);
    const { wrapKey, authToken } = deriveRecoveryKeys(FIXED_KEY);
    expect(toBase64(wrapKey)).toBe(FIXED_WRAP);
    expect(toBase64(authToken)).toBe(FIXED_AUTH);
  });

  it("formats and parses round-trip, forgiving spacing, case and look-alikes", () => {
    const key = generateRecoveryKey();
    const text = formatRecoveryKey(key);
    expect(text).toMatch(/^([A-Z2-7]{4}-){12}[A-Z2-7]{4}$/);
    expect(parseRecoveryKey(text)).toEqual(key);
    expect(parseRecoveryKey(text.toLowerCase().replace(/-/g, " "))).toEqual(key);
    expect(parseRecoveryKey(FIXED_FORMATTED.replace(/O/g, "0"))).toEqual(FIXED_KEY);
    expect(parseRecoveryKey("too-short")).toBeNull();
  });

  it("wraps and unwraps the private key, and rejects a key for another account", async () => {
    const pair = generateKeyPair();
    const key = generateRecoveryKey();
    const m = await recoveryMaterialFor(key, pair.privateKey);
    expect(unwrapWithRecoveryKey(key, m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, toBase64(pair.publicKey))).toEqual(pair.privateKey);

    const other = generateKeyPair();
    expect(() => unwrapWithRecoveryKey(key, m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, toBase64(other.publicKey))).toThrow();
    expect(() => unwrapWithRecoveryKey(generateRecoveryKey(), m.recoveryEncryptedPrivateKey, m.recoveryPrivateKeyNonce, toBase64(pair.publicKey))).toThrow();
  });
});
