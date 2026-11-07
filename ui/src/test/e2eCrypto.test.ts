import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";

beforeAll(() => crypto.ready());

describe("e2eCrypto", () => {
  it("wraps and unwraps a private key with the right password, and rejects the wrong one", () => {
    const { privateKey } = crypto.generateKeyPair();
    const wrapped = crypto.wrapPrivateKey(privateKey, "correct horse battery staple");

    const recovered = crypto.unwrapPrivateKey(wrapped, "correct horse battery staple");
    expect(recovered).toEqual(privateKey);

    expect(() => crypto.unwrapPrivateKey(wrapped, "wrong password")).toThrow();
  });

  it("seals and unseals a conversation key to a recipient's real keypair", () => {
    const recipient = crypto.generateKeyPair();
    const conversationKey = crypto.generateConversationKey();

    const sealed = crypto.sealConversationKey(conversationKey, recipient.publicKey);
    const opened = crypto.unsealConversationKey(sealed, recipient.publicKey, recipient.privateKey);
    expect(opened).toEqual(conversationKey);
  });

  it("encrypts and decrypts a message body with the conversation key", () => {
    const conversationKey = crypto.generateConversationKey();
    const { ciphertext, nonce } = crypto.encryptWithConversationKey("hey, made it in", conversationKey);
    expect(crypto.decryptWithConversationKey(ciphertext, nonce, conversationKey)).toBe("hey, made it in");
  });

  it("fails to decrypt with the wrong conversation key (tamper/AEAD check)", () => {
    const keyA = crypto.generateConversationKey();
    const keyB = crypto.generateConversationKey();
    const { ciphertext, nonce } = crypto.encryptWithConversationKey("secret", keyA);
    expect(() => crypto.decryptWithConversationKey(ciphertext, nonce, keyB)).toThrow();
  });

  it("safetyNumberDigits is order-independent and differs for a different key pair", () => {
    const a = crypto.generateKeyPair();
    const b = crypto.generateKeyPair();
    const c = crypto.generateKeyPair();

    const forward = crypto.safetyNumberDigits(a.publicKey, b.publicKey);
    const backward = crypto.safetyNumberDigits(b.publicKey, a.publicKey);
    expect(forward).toEqual(backward);
    expect(forward).toHaveLength(12);
    forward.forEach((group) => expect(group).toMatch(/^\d{5}$/));

    const different = crypto.safetyNumberDigits(a.publicKey, c.publicKey);
    expect(different).not.toEqual(forward);
  });

  it("base64 round-trips arbitrary bytes", () => {
    const bytes = crypto.generateConversationKey();
    expect(crypto.fromBase64(crypto.toBase64(bytes))).toEqual(bytes);
  });
});
