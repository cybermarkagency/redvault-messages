// Produces group_call_vectors.json with the web app's own code, so the desktop
// app's room-key protocol is pinned to it byte for byte. Not part of any build:
// copy it into ui/src/test/ as makeGroupCallVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeGroupCallVectors.test.ts
//
// and delete the copy. The output lands next to this file (desktop/tests/vectors/).
import { writeFileSync } from "node:fs";
import { beforeAll, expect, it, vi } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { newRoomKey, openRoomKey, sealRoomKey } from "../lib/groupCallCrypto.js";
import { groupSasWords } from "../lib/sas.js";

// From ui/src/test/ to desktop/tests/vectors/ (where this file lives).
const OUT = new URL("../../../desktop/tests/vectors/group_call_vectors.json", import.meta.url);
const SENT_AT = 1790000000000;

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const pair = (seedByte: number) => {
    const kp = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(seedByte));
    return { publicKey: kp.publicKey, privateKey: kp.privateKey };
  };
  const alice = pair(1);
  const bob = pair(2);
  const carol = pair(3);
  const mallory = pair(4);
  const b64 = crypto.toBase64;

  vi.spyOn(Date, "now").mockReturnValue(SENT_AT);
  const message = {
    callId: "3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b",
    conversationId: "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d",
    epoch: 19,
    from: "11111111-1111-4111-8111-111111111111",
    to: "22222222-2222-4222-8222-222222222222",
  };
  const key = newRoomKey();
  const sealed = sealRoomKey(message, key, bob.publicKey, alice.privateKey);
  // Sanity: the web's own opener accepts it.
  expect(openRoomKey(sealed, alice.publicKey, bob.privateKey, message, SENT_AT)).toEqual(key);
  const fromMallory = sealRoomKey(message, key, bob.publicKey, mallory.privateKey);

  const people = [
    { userId: message.from, publicKey: alice.publicKey },
    { userId: message.to, publicKey: bob.publicKey },
    { userId: "33333333-3333-4333-8333-333333333333", publicKey: carol.publicKey },
  ];

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        alicePublic: b64(alice.publicKey),
        alicePrivate: b64(alice.privateKey),
        bobPublic: b64(bob.publicKey),
        bobPrivate: b64(bob.privateKey),
        carolPublic: b64(carol.publicKey),
        malloryPublic: b64(mallory.publicKey),
        message,
        roomKey: b64(key),
        sentAt: SENT_AT,
        sealed,
        sealedByMallory: fromMallory,
        sasCallId: message.callId,
        sasParticipants: people.map((p) => ({ userId: p.userId, publicKey: b64(p.publicKey) })),
        sasWords: groupSasWords(message.callId, people),
        sasWordsOtherCall: groupSasWords("00000000-0000-4000-8000-000000000000", people),
        sasWordsSwappedKey: groupSasWords(message.callId, [people[0], { userId: people[1].userId, publicKey: mallory.publicKey }, people[2]]),
      },
      null,
      2
    ) + "\n"
  );
});
