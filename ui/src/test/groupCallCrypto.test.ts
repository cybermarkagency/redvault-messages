// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { GroupKeyError, newRoomKey, openRoomKey, ROOM_KEY_BYTES, sealRoomKey } from "../lib/groupCallCrypto.js";
import { groupSasWords } from "../lib/sas.js";

describe("group call room keys", () => {
  let alice: crypto.KeyPair;
  let bob: crypto.KeyPair;
  let mallory: crypto.KeyPair;
  beforeAll(async () => {
    await crypto.ready();
    alice = crypto.generateKeyPair();
    bob = crypto.generateKeyPair();
    mallory = crypto.generateKeyPair();
  });

  const message = { callId: "call-1", conversationId: "group-1", epoch: 3, from: "alice", to: "bob" };

  it("the participant opens the key the keyholder sealed to them", () => {
    const key = newRoomKey();
    expect(key.length).toBe(ROOM_KEY_BYTES);
    const sealed = sealRoomKey(message, key, bob.publicKey, alice.privateKey);
    expect(openRoomKey(sealed, alice.publicKey, bob.privateKey, message)).toEqual(key);
  });

  it("refuses a key from anyone but the keyholder's identity key", () => {
    const sealed = sealRoomKey({ ...message }, newRoomKey(), bob.publicKey, mallory.privateKey);
    expect(() => openRoomKey(sealed, alice.publicKey, bob.privateKey, message)).toThrow(GroupKeyError);
  });

  it("refuses a key replayed into another epoch, call or group, or meant for someone else", () => {
    const sealed = sealRoomKey(message, newRoomKey(), bob.publicKey, alice.privateKey);
    for (const other of [{ epoch: 4 }, { callId: "call-2" }, { conversationId: "group-2" }, { to: "carol" }, { from: "mallory" }]) {
      expect(() => openRoomKey(sealed, alice.publicKey, bob.privateKey, { ...message, ...other })).toThrow("didn't match");
    }
  });

  it("refuses a stale key", () => {
    const sealed = sealRoomKey(message, newRoomKey(), bob.publicKey, alice.privateKey);
    expect(() => openRoomKey(sealed, alice.publicKey, bob.privateKey, message, Date.now() + 10 * 60 * 1000)).toThrow("too old");
  });

  it("everyone gets the same safety words, and a swapped identity key changes them", () => {
    const people = [
      { userId: "alice", publicKey: alice.publicKey },
      { userId: "bob", publicKey: bob.publicKey },
    ];
    const words = groupSasWords("call-1", people);
    expect(words).toHaveLength(4);
    // Order doesn't matter: every app lists the roster its own way.
    expect(groupSasWords("call-1", [...people].reverse())).toEqual(words);
    // Alice's app was fed Mallory's key for Bob.
    expect(groupSasWords("call-1", [people[0], { userId: "bob", publicKey: mallory.publicKey }])).not.toEqual(words);
    expect(groupSasWords("call-2", people)).not.toEqual(words);
  });
});
