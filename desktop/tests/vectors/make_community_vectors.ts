// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces community_vectors.json with the web app's own code, so the desktop
// app's community epoch keys are pinned to it byte for byte. Not part of any
// build: copy it into ui/src/test/ as makeCommunityVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeCommunityVectors.test.ts
//
// and delete the copy. The output lands next to this file (desktop/tests/vectors/).
//
// crypto_box_seal is randomized, so sealed copies can't be reproduced; the
// desktop opens the web's copies instead (and must refuse the bad ones), and
// the bound payload itself is compared byte for byte.
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  buildKeyRing,
  encryptForCommunity,
  epochKeyPayload,
  newCommunityKey,
  prepareEnrollments,
  prepareRotation,
  sealEpochKey,
  type KeyStateDto,
} from "../lib/communityCrypto.js";

// From ui/ (where vitest runs) to desktop/tests/vectors/, where this file lives.
const OUT = resolve(process.cwd(), "../desktop/tests/vectors/community_vectors.json");

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const b64 = crypto.toBase64;
  const hex = (bytes: Uint8Array) => sodium.to_hex(bytes);
  const keyOf = (first: number) => Uint8Array.from({ length: 32 }, (_, i) => (first + i) & 0xff);

  const communityId = "6f9619ff-8b86-4011-b42d-00c04fc964ff";
  const otherCommunityId = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";
  const conversationId = "11111111-2222-4333-8444-555555555555";
  const me = sodium.crypto_box_seed_keypair(Uint8Array.from({ length: 32 }, (_, i) => 0x40 + i));
  const someoneElse = sodium.crypto_box_seed_keypair(Uint8Array.from({ length: 32 }, (_, i) => 0x90 + i));
  const identity = { publicKey: me.publicKey, privateKey: me.privateKey };

  // The payload: magic, version, epoch (big-endian), id (dashes and case don't matter), key.
  const payloads = [
    { key: keyOf(1), epoch: 0, communityId },
    { key: keyOf(100), epoch: 1, communityId },
    { key: keyOf(200), epoch: 0x01020304, communityId: communityId.toUpperCase() },
    { key: keyOf(7), epoch: 0xffffffff, communityId: communityId.replace(/-/g, "") },
  ].map((p) => ({ key: b64(p.key), epoch: p.epoch, communityId: p.communityId, payloadHex: hex(epochKeyPayload(p.key, p.epoch, p.communityId)) }));

  // What the app must refuse to build a payload from.
  const badPayloads: { name: string; key: string; epoch: number; communityId: string }[] = [
    { name: "short key", key: b64(keyOf(1).slice(0, 31)), epoch: 0, communityId },
    { name: "negative epoch", key: b64(keyOf(1)), epoch: -1, communityId },
    { name: "epoch over 32 bits", key: b64(keyOf(1)), epoch: 0x100000000, communityId },
    { name: "id too short", key: b64(keyOf(1)), epoch: 0, communityId: communityId.slice(0, 35) },
    { name: "id not hex", key: b64(keyOf(1)), epoch: 0, communityId: communityId.replace("6f", "zz") },
  ];
  for (const bad of badPayloads) expect(() => epochKeyPayload(crypto.fromBase64(bad.key), bad.epoch, bad.communityId)).toThrow();

  // Copies sealed to "me" that must open, and the key each gives.
  const opens = [
    { epoch: 0, key: keyOf(1) },
    { epoch: 7, key: keyOf(50) },
    { epoch: 0xffffffff, key: keyOf(9) },
  ].map((o) => ({ epoch: o.epoch, communityId, key: b64(o.key), wrappedKey: b64(sealEpochKey(o.key, o.epoch, communityId, me.publicKey)) }));

  // Copies that must be refused, opened as `epoch` of `communityId`.
  const sealRaw = (payload: Uint8Array) => b64(crypto.sealConversationKey(payload, me.publicKey));
  const versionTwo = epochKeyPayload(keyOf(3), 4, communityId);
  versionTwo[4] = 2;
  const badMagic = epochKeyPayload(keyOf(3), 4, communityId);
  badMagic[0] = 0x51;
  const refused = [
    { name: "sealed for another epoch", epoch: 5, communityId, wrappedKey: b64(sealEpochKey(keyOf(3), 4, communityId, me.publicKey)) },
    { name: "sealed for another community", epoch: 4, communityId, wrappedKey: b64(sealEpochKey(keyOf(3), 4, otherCommunityId, me.publicKey)) },
    { name: "sealed to someone else", epoch: 4, communityId, wrappedKey: b64(sealEpochKey(keyOf(3), 4, communityId, someoneElse.publicKey)) },
    { name: "a plain conversation key", epoch: 4, communityId, wrappedKey: sealRaw(keyOf(3)) },
    { name: "version 2", epoch: 4, communityId, wrappedKey: sealRaw(versionTwo) },
    { name: "wrong magic", epoch: 4, communityId, wrappedKey: sealRaw(badMagic) },
    { name: "one byte short", epoch: 4, communityId, wrappedKey: sealRaw(epochKeyPayload(keyOf(3), 4, communityId).slice(0, 56)) },
    { name: "one byte long", epoch: 4, communityId, wrappedKey: sealRaw(Uint8Array.from([...epochKeyPayload(keyOf(3), 4, communityId), 0])) },
  ];

  // A ring: listed out of order, with a reused key, another community's copy,
  // a copy claiming the wrong epoch and one that isn't base64 at all.
  const epochKeys = new Map<number, Uint8Array>([
    [0, keyOf(10)],
    [1, keyOf(20)],
    [3, keyOf(30)],
  ]);
  const state: KeyStateDto = {
    conversationId,
    keyEpoch: 3,
    rekeyRequired: false,
    keys: [
      { epoch: 3, wrappedKey: b64(sealEpochKey(epochKeys.get(3)!, 3, communityId, me.publicKey)) },
      { epoch: 0, wrappedKey: b64(sealEpochKey(epochKeys.get(0)!, 0, communityId, me.publicKey)) },
      { epoch: 2, wrappedKey: b64(sealEpochKey(epochKeys.get(1)!, 2, communityId, me.publicKey)) },
      { epoch: 1, wrappedKey: b64(sealEpochKey(epochKeys.get(1)!, 1, communityId, me.publicKey)) },
      { epoch: 4, wrappedKey: b64(sealEpochKey(keyOf(40), 4, otherCommunityId, me.publicKey)) },
      { epoch: 5, wrappedKey: b64(sealEpochKey(keyOf(50), 6, communityId, me.publicKey)) },
      { epoch: 6, wrappedKey: "not base64!" },
    ],
  };
  const ring = buildKeyRing(communityId, state, identity);
  expect([...ring.keys.keys()].sort()).toEqual([0, 1, 3]);
  const expectedRing = {
    currentEpoch: ring.currentEpoch,
    rekeyRequired: ring.rekeyRequired,
    keys: Object.fromEntries([...ring.keys.entries()].map(([epoch, key]) => [String(epoch), b64(key)])),
    rejected: ring.rejected,
  };

  // Posts: under the current epoch (encryptForCommunity) and an older one.
  const current = encryptForCommunity(ring, "Hello members \u{1F512} — épocas");
  const older = crypto.encryptWithConversationKey("from epoch 0", epochKeys.get(0)!);
  const messages = [
    { epoch: current.keyEpoch, ciphertext: b64(current.ciphertext), nonce: b64(current.nonce), plaintext: "Hello members \u{1F512} — épocas" },
    { epoch: 0, ciphertext: b64(older.ciphertext), nonce: b64(older.nonce), plaintext: "from epoch 0" },
    // Labelled with an epoch whose key doesn't open it: unreadable, not a crash.
    { epoch: 1, ciphertext: b64(older.ciphertext), nonce: b64(older.nonce), plaintext: null },
    // An epoch this member was never given (from before they joined).
    { epoch: 2, ciphertext: b64(older.ciphertext), nonce: b64(older.nonce), plaintext: null },
  ];

  // Upkeep: the web seals the next epoch to the roster, and the current one to someone waiting.
  const roster = { keyEpoch: 3, members: [{ userId: "me", publicKey: b64(me.publicKey) }, { userId: "them", publicKey: b64(someoneElse.publicKey) }] };
  const rotation = prepareRotation(ring, roster);
  const enrollments = prepareEnrollments(ring, [{ userId: "me", publicKey: b64(me.publicKey) }]);
  expect(enrollments).toHaveLength(1);
  expect(prepareEnrollments({ ...ring, rekeyRequired: true }, roster.members)).toEqual([]);
  const created = newCommunityKey(communityId, me.publicKey);

  const out = {
    communityId,
    otherCommunityId,
    me: { publicKey: b64(me.publicKey), privateKey: b64(me.privateKey) },
    someoneElse: { publicKey: b64(someoneElse.publicKey) },
    payloads,
    badPayloads,
    opens,
    refused,
    ring: { state, expected: expectedRing },
    messages,
    rotation: { fromEpoch: rotation.fromEpoch, key: b64(rotation.key), mine: rotation.keys.find((k) => k.userId === "me")!.wrappedKey },
    enrollment: { epoch: enrollments[0].epoch, wrappedKey: enrollments[0].wrappedKey },
    newCommunityKey: { key: b64(created.key), wrappedKey: created.wrappedKey },
  };
  writeFileSync(OUT, JSON.stringify(out, null, 2) + "\n");
});
