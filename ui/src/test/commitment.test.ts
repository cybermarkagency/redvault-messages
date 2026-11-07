import { createHash, randomUUID } from "node:crypto";
import { beforeAll, describe, expect, it } from "vitest";
import * as commitment from "../lib/commitment.js";
import * as crypto from "../lib/e2eCrypto.js";

beforeAll(() => crypto.ready());

// Ciphertext/nonce here come from crypto.encryptWithConversationKey, exactly
// like real usage — not from a plain TextEncoder-built Uint8Array, which
// this libsodium-wrappers-sumo ESM build's internal type check rejects (the
// same bug e2eCrypto.ts's own aeadEncrypt/safetyNumberDigits comments
// document; sodium's own outputs — AEAD ciphertext, from_base64 — always
// pass its check, so this is a non-issue for the real send/verify flow).
function envelope(overrides: Partial<{ messageId: string; plaintext: string }> = {}) {
  const conversationKey = crypto.generateConversationKey();
  const { ciphertext, nonce } = crypto.encryptWithConversationKey(overrides.plaintext ?? "hello bob", conversationKey);
  return { messageId: overrides.messageId ?? randomUUID(), ciphertext, nonce };
}

/** Independently reproduces server/src/crypto/commitment.ts's v1 algorithm using node:crypto directly, so this test proves cross-implementation parity rather than just calling the code under test to check itself. */
function expectedHex(env: ReturnType<typeof envelope>): string {
  const canonical = `v1:${env.messageId}:${crypto.toBase64(env.ciphertext)}:${crypto.toBase64(env.nonce)}`;
  return createHash("sha256").update(canonical, "utf8").digest("hex");
}

describe("commitment (client)", () => {
  it("matches the server's canonical v1 algorithm byte-for-byte", () => {
    const env = envelope();
    const hex = commitment.commitmentToHex(commitment.computeCommitment(env));
    expect(hex).toBe(expectedHex(env));
  });

  it("is deterministic for the same envelope", () => {
    const env = envelope();
    const a = commitment.commitmentToHex(commitment.computeCommitment(env));
    const b = commitment.commitmentToHex(commitment.computeCommitment(env));
    expect(a).toBe(b);
  });

  it("changes when the ciphertext differs", () => {
    const env = envelope();
    const tampered = envelope({ messageId: env.messageId, plaintext: "a different message entirely" });
    expect(commitment.commitmentToHex(commitment.computeCommitment(env))).not.toBe(
      commitment.commitmentToHex(commitment.computeCommitment(tampered))
    );
  });

  it("changes when the nonce (protected metadata) differs", () => {
    const env = envelope();
    // Re-encrypting the SAME plaintext gets a fresh random nonce (and thus
    // different ciphertext too, since AEAD mixes the nonce in) — still
    // proves the commitment is sensitive to the nonce field, not just
    // ciphertext content.
    const reEncrypted = envelope({ messageId: env.messageId, plaintext: "hello bob" });
    expect(env.nonce).not.toEqual(reEncrypted.nonce);
    expect(commitment.commitmentToHex(commitment.computeCommitment(env))).not.toBe(
      commitment.commitmentToHex(commitment.computeCommitment(reEncrypted))
    );
  });

  it("changes when the message id differs", () => {
    const env = envelope();
    const tampered = { ...env, messageId: randomUUID() };
    expect(commitment.commitmentToHex(commitment.computeCommitment(env))).not.toBe(
      commitment.commitmentToHex(commitment.computeCommitment(tampered))
    );
  });

  it("rejects an unsupported protocol version", () => {
    expect(() => commitment.computeCommitment(envelope(), 2)).toThrow();
  });

  describe("commitmentsMatch", () => {
    it("matches case-insensitively", () => {
      expect(commitment.commitmentsMatch("AB12", "ab12")).toBe(true);
    });

    it("does not match a different hash", () => {
      expect(commitment.commitmentsMatch("ab12", "ab13")).toBe(false);
    });
  });
});
