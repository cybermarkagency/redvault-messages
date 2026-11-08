// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import sodium from "libsodium-wrappers-sumo";
import { beforeAll, describe, expect, it } from "vitest";
import { ready } from "../lib/e2eCrypto.js";
import {
  BundleVerificationError,
  generateOneTimePreKeys,
  generatePqPreKey,
  generateSignedPreKey,
  initiate,
  respond,
  type PreKeyBundle,
} from "../lib/e2eV2/pqxdh.js";
import { generateX25519, utf8, xeddsaSign, xeddsaVerify } from "../lib/e2eV2/primitives.js";
import {
  decodeWireMessage,
  encodeWireMessage,
  initInitiator,
  initResponder,
  ratchetDecrypt,
  ratchetEncrypt,
  type SessionState,
} from "../lib/e2eV2/ratchet.js";

beforeAll(() => ready());

const ALICE_DEVICE = "11111111-1111-4111-8111-111111111111";
const BOB_DEVICE = "22222222-2222-4222-8222-222222222222";
const text = (bytes: Uint8Array) => new TextDecoder().decode(bytes);

describe("XEdDSA", () => {
  it("signs with an X25519 private key and verifies against the X25519 public key", () => {
    const identity = generateX25519();
    const message = utf8("signed prekey bytes");
    const signature = xeddsaSign(identity.privateKey, message);
    expect(xeddsaVerify(identity.publicKey, message, signature)).toBe(true);
    expect(xeddsaVerify(identity.publicKey, utf8("other bytes"), signature)).toBe(false);
    expect(xeddsaVerify(generateX25519().publicKey, message, signature)).toBe(false);
    const tampered = Uint8Array.from(signature);
    tampered[40] ^= 1;
    expect(xeddsaVerify(identity.publicKey, message, tampered)).toBe(false);
  });

  it("verifies for many keys (both signs of the Edwards point)", () => {
    for (let i = 0; i < 40; i++) {
      const identity = generateX25519();
      expect(xeddsaVerify(identity.publicKey, utf8(`m${i}`), xeddsaSign(identity.privateKey, utf8(`m${i}`)))).toBe(true);
    }
  });

  it("matches the Edwards key of a libsodium Ed25519 keypair converted to X25519", () => {
    const ed = sodium.crypto_sign_keypair();
    const xPublic = sodium.crypto_sign_ed25519_pk_to_curve25519(ed.publicKey);
    const xPrivate = sodium.crypto_sign_ed25519_sk_to_curve25519(ed.privateKey);
    const signature = xeddsaSign(xPrivate, utf8("hello"));
    expect(xeddsaVerify(xPublic, utf8("hello"), signature)).toBe(true);
  });
});

function bobKeys(withOneTime = true) {
  const identity = generateX25519();
  const signedPreKey = generateSignedPreKey(identity, 1);
  const pqPreKey = generatePqPreKey(identity, 2);
  const [oneTimePreKey] = generateOneTimePreKeys(3, 1);
  const bundle: PreKeyBundle = {
    identityKey: identity.publicKey,
    signedPreKey: { id: 1, publicKey: signedPreKey.keyPair.publicKey, signature: signedPreKey.signature },
    pqPreKey: { id: 2, publicKey: pqPreKey.publicKey, signature: pqPreKey.signature },
    oneTimePreKey: withOneTime ? { id: 3, publicKey: oneTimePreKey.keyPair.publicKey } : null,
  };
  return { identity, signedPreKey, pqPreKey, oneTimePreKey: withOneTime ? oneTimePreKey : null, bundle };
}

describe("PQXDH", () => {
  it.each([true, false])("both sides derive the same key (one-time prekey: %s)", (withOneTime) => {
    const alice = generateX25519();
    const bob = bobKeys(withOneTime);
    const started = initiate(alice, bob.bundle, ALICE_DEVICE, BOB_DEVICE);
    const answered = respond(bob.identity, bob.signedPreKey, bob.pqPreKey, bob.oneTimePreKey, started.header, ALICE_DEVICE, BOB_DEVICE);
    expect(answered.sharedKey).toEqual(started.sharedKey);
    expect(answered.associatedData).toEqual(started.associatedData);
  });

  it("refuses a bundle whose prekeys aren't signed by the identity key", () => {
    const bob = bobKeys();
    const forged = { ...bob.bundle, signedPreKey: { ...bob.bundle.signedPreKey, publicKey: generateX25519().publicKey } };
    expect(() => initiate(generateX25519(), forged, ALICE_DEVICE, BOB_DEVICE)).toThrow(BundleVerificationError);
    const otherIdentity = { ...bob.bundle, identityKey: generateX25519().publicKey };
    expect(() => initiate(generateX25519(), otherIdentity, ALICE_DEVICE, BOB_DEVICE)).toThrow(BundleVerificationError);
  });
});

function startSession(): { alice: SessionState; bob: SessionState } {
  const aliceIdentity = generateX25519();
  const bob = bobKeys();
  const started = initiate(aliceIdentity, bob.bundle, ALICE_DEVICE, BOB_DEVICE);
  const alice = initInitiator(started.sharedKey, bob.bundle.signedPreKey.publicKey, started.associatedData, started.header);
  const answered = respond(bob.identity, bob.signedPreKey, bob.pqPreKey, bob.oneTimePreKey, started.header, ALICE_DEVICE, BOB_DEVICE);
  const bobState = initResponder(answered.sharedKey, bob.signedPreKey.keyPair, answered.associatedData, started.header.ephemeralKey);
  return { alice, bob: bobState };
}

/** Encrypts, round-trips through the wire format, and returns the updated sender state. */
function send(state: SessionState, body: string) {
  const out = ratchetEncrypt(state, utf8(body));
  return { state: out.state, wire: decodeWireMessage(encodeWireMessage(out.message)) };
}

describe("Double Ratchet", () => {
  it("carries a conversation both ways, and drops the prekey header once the other side replies", () => {
    let { alice, bob } = startSession();
    const a1 = send(alice, "hi bob");
    alice = a1.state;
    expect(a1.wire.preKey).not.toBeNull();
    const b1 = ratchetDecrypt(bob, a1.wire);
    bob = b1.state;
    expect(text(b1.plaintext)).toBe("hi bob");

    const r1 = send(bob, "hi alice");
    bob = r1.state;
    expect(r1.wire.preKey).toBeNull();
    const a2 = ratchetDecrypt(alice, r1.wire);
    alice = a2.state;
    expect(text(a2.plaintext)).toBe("hi alice");

    const a3 = send(alice, "after the reply");
    expect(a3.wire.preKey).toBeNull();
    expect(text(ratchetDecrypt(bob, a3.wire).plaintext)).toBe("after the reply");
  });

  it("uses a new key for every message, and changes the ratchet key on each turn", () => {
    let { alice, bob } = startSession();
    const m1 = send(alice, "same text");
    alice = m1.state;
    const m2 = send(alice, "same text");
    alice = m2.state;
    expect(m1.wire.ciphertext).not.toEqual(m2.wire.ciphertext);
    bob = ratchetDecrypt(bob, m1.wire).state;
    bob = ratchetDecrypt(bob, m2.wire).state;
    const reply = send(bob, "ok");
    alice = ratchetDecrypt(alice, reply.wire).state;
    const m3 = send(alice, "next turn");
    expect(m3.wire.header.ratchetKey).not.toEqual(m1.wire.header.ratchetKey);
  });

  it("decrypts messages that arrive late or out of order", () => {
    let { alice, bob } = startSession();
    const sent = [];
    for (let i = 0; i < 5; i++) {
      const out = send(alice, `m${i}`);
      alice = out.state;
      sent.push(out.wire);
    }
    for (const i of [3, 0, 4, 2, 1]) {
      const out = ratchetDecrypt(bob, sent[i]);
      bob = out.state;
      expect(text(out.plaintext)).toBe(`m${i}`);
    }
    // Across a ratchet turn, too: Alice's old-chain message arrives after Bob has moved on.
    const late = send(alice, "late");
    alice = late.state;
    const reply = send(bob, "reply");
    bob = reply.state;
    alice = ratchetDecrypt(alice, reply.wire).state;
    const fresh = send(alice, "fresh");
    bob = ratchetDecrypt(bob, fresh.wire).state;
    expect(text(ratchetDecrypt(bob, late.wire).plaintext)).toBe("late");
  });

  it("can't decrypt the same message twice (each key is deleted once used)", () => {
    const { alice, bob } = startSession();
    const m = send(alice, "once");
    const after = ratchetDecrypt(bob, m.wire).state;
    expect(() => ratchetDecrypt(after, m.wire)).toThrow();
  });

  it("rejects a tampered message and leaves the session usable", () => {
    let { alice, bob } = startSession();
    const m = send(alice, "real");
    alice = m.state;
    const tampered = { ...m.wire, ciphertext: Uint8Array.from(m.wire.ciphertext) };
    tampered.ciphertext[0] ^= 1;
    expect(() => ratchetDecrypt(bob, tampered)).toThrow();
    const forgedHeader = { ...m.wire, header: { ...m.wire.header, n: 7 } };
    expect(() => ratchetDecrypt(bob, forgedHeader)).toThrow();
    expect(text(ratchetDecrypt(bob, m.wire).plaintext)).toBe("real");
  });

  it("forward secrecy: a later session state can't decrypt earlier messages", () => {
    let { alice, bob } = startSession();
    const early = send(alice, "early");
    alice = early.state;
    bob = ratchetDecrypt(bob, early.wire).state;
    for (let i = 0; i < 3; i++) {
      const r = send(bob, `r${i}`);
      bob = r.state;
      alice = ratchetDecrypt(alice, r.wire).state;
      const a = send(alice, `a${i}`);
      alice = a.state;
      bob = ratchetDecrypt(bob, a.wire).state;
    }
    // An attacker who steals Bob's current state still can't read "early".
    expect(() => ratchetDecrypt(bob, early.wire)).toThrow();
  });
});

describe("sessions (Sesame bookkeeping)", async () => {
  const { decryptFrom, encryptFor, startSession } = await import("../lib/e2eV2/sessions.js");
  const { generateOneTimePreKeys } = await import("../lib/e2eV2/pqxdh.js");

  function localDevice(identity: ReturnType<typeof generateX25519>, deviceId: string) {
    const signed = generateSignedPreKey(identity, 1);
    const pq = generatePqPreKey(identity, 2);
    const oneTime = generateOneTimePreKeys(3, 2);
    const device = { deviceId, signedPreKeys: [signed], pqPreKeys: [pq], oneTimePreKeys: oneTime, nextKeyId: 5 };
    const bundle = (withOneTime: boolean): PreKeyBundle => ({
      identityKey: identity.publicKey,
      signedPreKey: { id: 1, publicKey: signed.keyPair.publicKey, signature: signed.signature },
      pqPreKey: { id: 2, publicKey: pq.publicKey, signature: pq.signature },
      oneTimePreKey: withOneTime ? { id: oneTime[0].id, publicKey: oneTime[0].keyPair.publicKey } : null,
    });
    return { device, bundle };
  }

  it("settles when both devices start a session with each other at once", () => {
    const aliceId = generateX25519();
    const bobId = generateX25519();
    const alice = localDevice(aliceId, ALICE_DEVICE);
    const bob = localDevice(bobId, BOB_DEVICE);

    let aliceRecord = startSession(aliceId, bob.bundle(false), ALICE_DEVICE, BOB_DEVICE);
    let bobRecord = startSession(bobId, alice.bundle(false), BOB_DEVICE, ALICE_DEVICE);
    const fromAlice = encryptFor(aliceRecord, utf8("a1"));
    aliceRecord = fromAlice.record;
    const fromBob = encryptFor(bobRecord, utf8("b1"));
    bobRecord = fromBob.record;

    // Each receives the other's first message, crossing in flight.
    const atBob = decryptFrom(bobId, bob.device, bobRecord, fromAlice.payload, ALICE_DEVICE, aliceId.publicKey);
    bobRecord = atBob.record;
    const atAlice = decryptFrom(aliceId, alice.device, aliceRecord, fromBob.payload, BOB_DEVICE, bobId.publicKey);
    aliceRecord = atAlice.record;
    expect(text(atBob.plaintext)).toBe("a1");
    expect(text(atAlice.plaintext)).toBe("b1");

    // And the conversation carries on both ways.
    for (let i = 0; i < 3; i++) {
      const a = encryptFor(aliceRecord, utf8(`a${i + 2}`));
      aliceRecord = a.record;
      const got = decryptFrom(bobId, bob.device, bobRecord, a.payload, ALICE_DEVICE, aliceId.publicKey);
      bobRecord = got.record;
      expect(text(got.plaintext)).toBe(`a${i + 2}`);
      const b = encryptFor(bobRecord, utf8(`b${i + 2}`));
      bobRecord = b.record;
      const back = decryptFrom(aliceId, alice.device, aliceRecord, b.payload, BOB_DEVICE, bobId.publicKey);
      aliceRecord = back.record;
      expect(text(back.plaintext)).toBe(`b${i + 2}`);
    }
  });

  it("refuses a prekey message claiming a different identity than the sender's account", () => {
    const aliceId = generateX25519();
    const bobId = generateX25519();
    const bob = localDevice(bobId, BOB_DEVICE);
    const record = startSession(aliceId, bob.bundle(true), ALICE_DEVICE, BOB_DEVICE);
    const { payload } = encryptFor(record, utf8("hi"));
    expect(() => decryptFrom(bobId, bob.device, undefined, payload, ALICE_DEVICE, generateX25519().publicKey)).toThrow();
    const ok = decryptFrom(bobId, bob.device, undefined, payload, ALICE_DEVICE, aliceId.publicKey);
    expect(ok.usedOneTimePreKeyId).toBe(3);
  });
});
