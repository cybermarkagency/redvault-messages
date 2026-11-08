// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, describe, expect, it } from "vitest";
import { type CallSignal, CallSignalError, openCallSignal, sealCallSignal } from "../lib/callCrypto.js";
import * as crypto from "../lib/e2eCrypto.js";
import { checkSafetyQr, safetyQrPayload } from "../lib/safetyQr.js";
import { acceptContactKey, hasKeyChanged } from "../lib/verifiedContacts.js";

const SDP = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\na=fingerprint:sha-256 AB:CD:EF\r\na=candidate:1 1 udp 1 203.0.113.9 3478 typ relay\r\n";

describe("call setup sealing", () => {
  let alice: crypto.KeyPair;
  let bob: crypto.KeyPair;
  let mallory: crypto.KeyPair;
  beforeAll(async () => {
    await crypto.ready();
    alice = crypto.generateKeyPair();
    bob = crypto.generateKeyPair();
    mallory = crypto.generateKeyPair();
  });

  const offer = (overrides: Partial<CallSignal> = {}): CallSignal => ({
    v: 1,
    type: "offer",
    callId: "call-1",
    conversationId: "conv-1",
    from: "alice",
    to: "bob",
    kind: "video",
    sdp: SDP,
    sentAt: Date.now(),
    ...overrides,
  });
  const expected = { type: "offer" as const, callId: "call-1", conversationId: "conv-1", from: "alice", to: "bob", kind: "video" as const };

  it("the contact opens what was sealed to them, and it carries the DTLS fingerprint", () => {
    const sealed = sealCallSignal(offer(), bob.publicKey, alice.privateKey);
    expect(sealed.ciphertext).not.toContain("fingerprint");
    const opened = openCallSignal(sealed, alice.publicKey, bob.privateKey, expected);
    expect(opened.sdp).toBe(SDP);
  });

  it("refuses a signal sealed by someone else's key (a server swapping in its own offer)", () => {
    const forged = sealCallSignal(offer(), bob.publicKey, mallory.privateKey);
    expect(() => openCallSignal(forged, alice.publicKey, bob.privateKey, expected)).toThrow(CallSignalError);
  });

  it("refuses a tampered signal", () => {
    const sealed = sealCallSignal(offer(), bob.publicKey, alice.privateKey);
    const bytes = crypto.fromBase64(sealed.ciphertext);
    bytes[bytes.length - 1] ^= 1;
    expect(() => openCallSignal({ ...sealed, ciphertext: crypto.toBase64(bytes) }, alice.publicKey, bob.privateKey, expected)).toThrow(
      CallSignalError
    );
  });

  it("refuses a genuine signal replayed into another call, turned around, or relabelled", () => {
    const sealed = sealCallSignal(offer(), bob.publicKey, alice.privateKey);
    expect(() => openCallSignal(sealed, alice.publicKey, bob.privateKey, { ...expected, callId: "call-2" })).toThrow(CallSignalError);
    expect(() => openCallSignal(sealed, alice.publicKey, bob.privateKey, { ...expected, type: "answer" })).toThrow(CallSignalError);
    expect(() => openCallSignal(sealed, alice.publicKey, bob.privateKey, { ...expected, kind: "voice" })).toThrow(CallSignalError);
    expect(() => openCallSignal(sealed, alice.publicKey, bob.privateKey, { ...expected, conversationId: "conv-2" })).toThrow(CallSignalError);
  });

  it("refuses stale signals and SDP without an encryption fingerprint", () => {
    const old = sealCallSignal(offer({ sentAt: Date.now() - 10 * 60 * 1000 }), bob.publicKey, alice.privateKey);
    expect(() => openCallSignal(old, alice.publicKey, bob.privateKey, expected)).toThrow(/too old/);
    const bare = sealCallSignal(offer({ sdp: "v=0\r\n" }), bob.publicKey, alice.privateKey);
    expect(() => openCallSignal(bare, alice.publicKey, bob.privateKey, expected)).toThrow(/fingerprint/);
  });
});

describe("safety code QR", () => {
  it("matches only when both halves are the keys each side holds", async () => {
    await crypto.ready();
    const me = crypto.generateKeyPair();
    const them = crypto.generateKeyPair();
    const imposter = crypto.generateKeyPair();

    // Their phone shows: their key, then the key they hold for me.
    expect(checkSafetyQr(safetyQrPayload(them.publicKey, me.publicKey), me.publicKey, them.publicKey)).toBe("match");
    // The server gave me an imposter's key for them, or gave them an imposter's key for me.
    expect(checkSafetyQr(safetyQrPayload(them.publicKey, me.publicKey), me.publicKey, imposter.publicKey)).toBe("mismatch");
    expect(checkSafetyQr(safetyQrPayload(them.publicKey, imposter.publicKey), me.publicKey, them.publicKey)).toBe("mismatch");
    expect(checkSafetyQr("https://example.com", me.publicKey, them.publicKey)).toBe("not-a-safety-code");
  });
});

describe("key-change detection", () => {
  it("remembers a contact's first key, flags a different one, and clears once it's verified", () => {
    localStorage.clear();
    expect(hasKeyChanged("me", "bob", "key-1")).toBe(false);
    expect(hasKeyChanged("me", "bob", "key-1")).toBe(false);
    expect(hasKeyChanged("me", "bob", "key-2")).toBe(true);
    // Still flagged on every load until the new key is verified.
    expect(hasKeyChanged("me", "bob", "key-2")).toBe(true);
    acceptContactKey("me", "bob", "key-2");
    expect(hasKeyChanged("me", "bob", "key-2")).toBe(false);
  });
});
