import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  buildKeyRing,
  decryptForCommunity,
  encryptForCommunity,
  EpochKeyError,
  newCommunityKey,
  openEpochKey,
  prepareEnrollments,
  prepareRotation,
  sealEpochKey,
} from "../lib/communityCrypto.js";

beforeAll(() => crypto.ready());

const COMMUNITY = "3f2a9c1e-7b4d-4e8a-9c2f-1a2b3c4d5e6f";
const OTHER = "9e8d7c6b-5a49-4382-a1b0-c9d8e7f6a5b4";

function member() {
  const kp = crypto.generateKeyPair();
  return { ...kp, publicKeyB64: crypto.toBase64(kp.publicKey) };
}

describe("community epoch keys", () => {
  it("seals a key bound to its community and epoch, and refuses any other binding", () => {
    const alice = member();
    const key = crypto.generateConversationKey();
    const sealed = sealEpochKey(key, 3, COMMUNITY, alice.publicKey);
    expect(sealed.length).toBe(105);
    expect(openEpochKey(sealed, 3, COMMUNITY, alice)).toEqual(key);
    expect(() => openEpochKey(sealed, 4, COMMUNITY, alice)).toThrow(EpochKeyError);
    expect(() => openEpochKey(sealed, 3, OTHER, alice)).toThrow(EpochKeyError);
    expect(() => openEpochKey(sealed, 3, COMMUNITY, member())).toThrow(EpochKeyError);
    // A plain group key (no binding) isn't accepted as a community key.
    expect(() => openEpochKey(crypto.sealConversationKey(key, alice.publicKey), 3, COMMUNITY, alice)).toThrow(EpochKeyError);
  });

  it("a member who joins gets the current key; removing someone rotates to a key they never see", () => {
    const creator = member();
    const fan = member();
    const removed = member();

    // Epoch 0, made by the creator; then sealed to both members by enrollment.
    const zero = newCommunityKey(COMMUNITY, creator.publicKey);
    const creatorRing0 = buildKeyRing(COMMUNITY, { conversationId: "c", keyEpoch: 0, rekeyRequired: false, keys: [{ epoch: 0, wrappedKey: zero.wrappedKey }] }, creator);
    const enrollments = prepareEnrollments(creatorRing0, [
      { userId: "fan", publicKey: fan.publicKeyB64 },
      { userId: "removed", publicKey: removed.publicKeyB64 },
    ]);
    const sealedFor = (userId: string) => enrollments.find((e) => e.userId === userId)!.wrappedKey;
    const removedRing = buildKeyRing(COMMUNITY, { conversationId: "c", keyEpoch: 0, rekeyRequired: false, keys: [{ epoch: 0, wrappedKey: sealedFor("removed") }] }, removed);
    const before = encryptForCommunity(creatorRing0, "before the removal");
    expect(decryptForCommunity(removedRing, before.keyEpoch, before.ciphertext, before.nonce)).toBe("before the removal");

    // "removed" leaves; the roster is creator + fan; the creator's app rotates.
    const rotation = prepareRotation(creatorRing0, { keyEpoch: 0, members: [{ userId: "creator", publicKey: creator.publicKeyB64 }, { userId: "fan", publicKey: fan.publicKeyB64 }] });
    expect(rotation.keys.map((k) => k.userId)).toEqual(["creator", "fan"]);
    const creatorRing1 = buildKeyRing(
      COMMUNITY,
      { conversationId: "c", keyEpoch: 1, rekeyRequired: false, keys: [{ epoch: 0, wrappedKey: zero.wrappedKey }, { epoch: 1, wrappedKey: rotation.keys[0].wrappedKey }] },
      creator
    );
    const fanRing1 = buildKeyRing(
      COMMUNITY,
      { conversationId: "c", keyEpoch: 1, rekeyRequired: false, keys: [{ epoch: 0, wrappedKey: sealedFor("fan") }, { epoch: 1, wrappedKey: rotation.keys[1].wrappedKey }] },
      fan
    );
    const after = encryptForCommunity(creatorRing1, "after the removal");
    expect(after.keyEpoch).toBe(1);
    // Remaining members read both; the removed member's saved key opens nothing new.
    expect(decryptForCommunity(fanRing1, after.keyEpoch, after.ciphertext, after.nonce)).toBe("after the removal");
    expect(decryptForCommunity(fanRing1, before.keyEpoch, before.ciphertext, before.nonce)).toBe("before the removal");
    expect(decryptForCommunity(removedRing, after.keyEpoch, after.ciphertext, after.nonce)).toBeNull();
    // Even with epoch 0's key, the removed member can't open epoch 1 ciphertext.
    const zeroKey = removedRing.keys.get(0)!;
    expect(() => crypto.decryptWithConversationKey(after.ciphertext, after.nonce, zeroKey)).toThrow();
  });

  it("rejects a server replaying an old sealed copy as a new epoch, or a rotation that reuses a key", () => {
    const fan = member();
    const k0 = crypto.generateConversationKey();
    const sealed0 = crypto.toBase64(sealEpochKey(k0, 0, COMMUNITY, fan.publicKey));
    // Old copy offered as epoch 1: the binding says 0.
    const replayed = buildKeyRing(COMMUNITY, { conversationId: "c", keyEpoch: 1, rekeyRequired: false, keys: [{ epoch: 0, wrappedKey: sealed0 }, { epoch: 1, wrappedKey: sealed0 }] }, fan);
    expect(replayed.keys.has(1)).toBe(false);
    expect(replayed.rejected).toEqual([1]);
    expect(() => encryptForCommunity(replayed, "x")).toThrow(EpochKeyError);
    // Same key re-sealed as epoch 1 by a malicious rotator: refused too.
    const resealed = crypto.toBase64(sealEpochKey(k0, 1, COMMUNITY, fan.publicKey));
    const reused = buildKeyRing(COMMUNITY, { conversationId: "c", keyEpoch: 1, rekeyRequired: false, keys: [{ epoch: 0, wrappedKey: sealed0 }, { epoch: 1, wrappedKey: resealed }] }, fan);
    expect(reused.rejected).toEqual([1]);
    // Another community's key isn't accepted here.
    const foreign = crypto.toBase64(sealEpochKey(crypto.generateConversationKey(), 0, OTHER, fan.publicKey));
    expect(buildKeyRing(COMMUNITY, { conversationId: "c", keyEpoch: 0, rekeyRequired: false, keys: [{ epoch: 0, wrappedKey: foreign }] }, fan).keys.size).toBe(0);
  });

  it("doesn't enroll anyone while a rotation is pending", () => {
    const creator = member();
    const zero = newCommunityKey(COMMUNITY, creator.publicKey);
    const ring = buildKeyRing(COMMUNITY, { conversationId: "c", keyEpoch: 0, rekeyRequired: true, keys: [{ epoch: 0, wrappedKey: zero.wrappedKey }] }, creator);
    expect(prepareEnrollments(ring, [{ userId: "x", publicKey: member().publicKeyB64 }])).toEqual([]);
  });
});
