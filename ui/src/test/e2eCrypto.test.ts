// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";

beforeAll(() => crypto.ready());

const randomKey = () => globalThis.crypto.getRandomValues(new Uint8Array(32));

describe("e2eCrypto", () => {
  it("wraps and unwraps a private key with the right password, and rejects the wrong one", () => {
    const { privateKey } = crypto.generateKeyPair();
    const wrapped = crypto.wrapPrivateKey(privateKey, "correct horse battery staple");

    const recovered = crypto.unwrapPrivateKey(wrapped, "correct horse battery staple");
    expect(recovered).toEqual(privateKey);

    expect(() => crypto.unwrapPrivateKey(wrapped, "wrong password")).toThrow();
  });

  it("encrypts and decrypts with a 32-byte key (what a file share link's details are sealed with)", () => {
    const conversationKey = randomKey();
    const { ciphertext, nonce } = crypto.encryptWithConversationKey("hey, made it in", conversationKey);
    expect(crypto.decryptWithConversationKey(ciphertext, nonce, conversationKey)).toBe("hey, made it in");
  });

  it("fails to decrypt with the wrong conversation key (tamper/AEAD check)", () => {
    const keyA = randomKey();
    const keyB = randomKey();
    const { ciphertext, nonce } = crypto.encryptWithConversationKey("secret", keyA);
    expect(() => crypto.decryptWithConversationKey(ciphertext, nonce, keyB)).toThrow();
  });

  it("base64 round-trips arbitrary bytes", () => {
    const bytes = randomKey();
    expect(crypto.fromBase64(crypto.toBase64(bytes))).toEqual(bytes);
  });
});
