// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeEach, describe, expect, it } from "vitest";
import { isVerified, setVerified } from "../lib/verifiedContacts.js";

describe("verifiedContacts", () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it("defaults to unverified", () => {
    expect(isVerified("self-1", "pubkey-a")).toBe(false);
  });

  it("round-trips through setVerified/isVerified, scoped per self+key", () => {
    setVerified("self-1", "pubkey-a", true);
    expect(isVerified("self-1", "pubkey-a")).toBe(true);
    // Different self (e.g. a different identity in the same browser) doesn't inherit it.
    expect(isVerified("self-2", "pubkey-a")).toBe(false);
    // A different key for the same self isn't verified either.
    expect(isVerified("self-1", "pubkey-b")).toBe(false);
  });

  it("setVerified(false) clears it — e.g. if the contact's key changes and they need re-verifying", () => {
    setVerified("self-1", "pubkey-a", true);
    setVerified("self-1", "pubkey-a", false);
    expect(isVerified("self-1", "pubkey-a")).toBe(false);
  });
});
