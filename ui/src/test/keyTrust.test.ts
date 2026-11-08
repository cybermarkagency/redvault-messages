// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Protocol 1 key trust: the attacks from audit/tests EXPOSE-2 and finding H-2, now refused, with controls.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { keyRingFromDto, verifyGroupRing } from "../lib/groupKeys.js";
import { checkDirectKey, checkGroupKey, loadPin, membersToSign, noticeSignatureOk, unconfirmedMembers } from "../lib/keyTrust.js";
import { requireUnchangedKey } from "../lib/verifiedContacts.js";

const SELF = "11111111-1111-4111-8111-111111111111";
const BOB = "22222222-2222-4222-8222-222222222222";
const EVE = "33333333-3333-4333-8333-333333333333";
const DM = "44444444-4444-4444-8444-444444444444";
const GROUP = "55555555-5555-4555-8555-555555555555";

const b64 = (u: Uint8Array) => crypto.toBase64(u);
const signers = new Map<string, crypto.SigningKeyPair>();
const signingKey = (id: string) => signers.get(id)?.publicKey ?? null;

function notice(issuer: string, epoch: number, seq: number, members: string[], key: Uint8Array) {
  const body = { epoch, seq, issuerId: issuer, members };
  const signature = crypto.signMessage(crypto.keyNoticeInput(GROUP, body, crypto.keyCommitment(key)), signers.get(issuer)!.secretKey);
  return { ...body, signature: b64(signature) };
}

const check = (key: Uint8Array | undefined, n: ReturnType<typeof notice> | null, epoch = n?.epoch ?? 0) =>
  checkGroupKey({ conversationId: GROUP, selfUserId: SELF, epoch, key, notice: n, signingKey });

beforeAll(async () => {
  await crypto.ready();
  for (const id of [SELF, BOB, EVE]) signers.set(id, crypto.deriveSigningKeyPair(crypto.generateConversationKey()));
});

beforeEach(() => localStorage.clear());

describe("direct chats", () => {
  it("refuses a server-chosen key at a new epoch: EXPOSE-2 no longer works", () => {
    const pair = crypto.generateKeyPair();
    const owner = { userId: SELF, publicKey: pair.publicKey, privateKey: pair.privateKey };
    const realKey = crypto.generateConversationKey();
    const attackerKey = crypto.generateConversationKey();
    const seal = (k: Uint8Array) => b64(crypto.sealConversationKey(k, pair.publicKey));

    const honest = keyRingFromDto({ id: DM, isGroup: false, keyEpoch: 0, wrappedKey: seal(realKey), keys: [] } as never, owner);
    expect(honest.keyProblem).toBeUndefined();

    const forged = keyRingFromDto(
      { id: DM, isGroup: false, keyEpoch: 1, wrappedKey: seal(attackerKey), keys: [{ epoch: 0, wrappedKey: seal(realKey) }] } as never,
      owner
    );
    expect(forged.keyProblem).toMatch(/changed/);
    expect([...forged.keys.values()].some((k) => Buffer.from(k).equals(Buffer.from(attackerKey)))).toBe(false);
  });

  it("never opens extra epochs a server lists for a direct chat", () => {
    const pair = crypto.generateKeyPair();
    const owner = { userId: SELF, publicKey: pair.publicKey, privateKey: pair.privateKey };
    const seal = (k: Uint8Array) => b64(crypto.sealConversationKey(k, pair.publicKey));
    const ring = keyRingFromDto(
      { id: DM, isGroup: false, keyEpoch: 0, wrappedKey: seal(crypto.generateConversationKey()), keys: [{ epoch: 7, wrappedKey: seal(crypto.generateConversationKey()) }] } as never,
      owner
    );
    expect([...ring.keys.keys()]).toEqual([0]);
  });

  it("a later epoch can't bring a substitute epoch 0 with it", () => {
    const pair = crypto.generateKeyPair();
    const owner = { userId: SELF, publicKey: pair.publicKey, privateKey: pair.privateKey };
    const seal = (k: Uint8Array) => b64(crypto.sealConversationKey(k, pair.publicKey));
    const realKey = crypto.generateConversationKey();
    keyRingFromDto({ id: DM, isGroup: false, keyEpoch: 0, wrappedKey: seal(realKey), keys: [] } as never, owner);
    const swapped = crypto.generateConversationKey();
    const forged = keyRingFromDto(
      { id: DM, isGroup: false, keyEpoch: 1, wrappedKey: seal(swapped), keys: [{ epoch: 0, wrappedKey: seal(swapped) }] } as never,
      owner
    );
    expect(forged.keyProblem).toMatch(/changed/);
    expect(forged.keys.size).toBe(0);
  });

  it("refuses a substituted key at epoch 0 and doesn't keep it", () => {
    const pair = crypto.generateKeyPair();
    const owner = { userId: SELF, publicKey: pair.publicKey, privateKey: pair.privateKey };
    const seal = (k: Uint8Array) => b64(crypto.sealConversationKey(k, pair.publicKey));
    keyRingFromDto({ id: DM, isGroup: false, keyEpoch: 0, wrappedKey: seal(crypto.generateConversationKey()), keys: [] } as never, owner);
    const forged = keyRingFromDto({ id: DM, isGroup: false, keyEpoch: 0, wrappedKey: seal(crypto.generateConversationKey()), keys: [] } as never, owner);
    expect(forged.keyProblem).toMatch(/changed/);
    expect(forged.keys.size).toBe(0);
  });

  it("refuses a different key at the same epoch", () => {
    expect(checkDirectKey(SELF, DM, 0, crypto.generateConversationKey())).toBeNull();
    expect(checkDirectKey(SELF, DM, 0, crypto.generateConversationKey())).toMatch(/changed/);
  });

  it("control: the same key keeps working", () => {
    const key = crypto.generateConversationKey();
    expect(checkDirectKey(SELF, DM, 0, key)).toBeNull();
    expect(checkDirectKey(SELF, DM, 0, key)).toBeNull();
  });
});

describe("group key notices", () => {
  const key = () => crypto.generateConversationKey();

  it("control: a signed notice is accepted, then a new epoch from a member", () => {
    const k1 = key();
    expect(check(k1, notice(SELF, 0, 1, [SELF, BOB], k1))).toBeNull();
    const k2 = key();
    expect(check(k2, notice(BOB, 1, 1, [SELF, BOB], k2))).toBeNull();
    expect(loadPin(SELF, GROUP)?.epoch).toBe(1);
  });

  it("refuses a server-chosen key: no real member signed it", () => {
    const k1 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    const forged = notice(BOB, 1, 1, [SELF, BOB], k1); // Signed for another key.
    expect(check(key(), forged)).toMatch(/couldn't be verified/);
  });

  it("refuses a new epoch issued by someone outside the group, even with a valid signature", () => {
    const k1 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    const k2 = key();
    expect(check(k2, notice(EVE, 1, 1, [SELF, BOB, EVE], k2))).toMatch(/couldn't be verified/);
  });

  it("refuses a tampered member list: a hidden member can't be added to a signed notice", () => {
    const k1 = key();
    const tampered = { ...notice(SELF, 0, 1, [SELF, BOB], k1), members: [SELF, BOB, EVE] };
    expect(check(k1, tampered)).toMatch(/couldn't be verified/);
  });

  it("refuses rollback to an older epoch or sequence, and a removed notice", () => {
    const k1 = key();
    check(k1, notice(SELF, 0, 2, [SELF, BOB], k1));
    expect(check(k1, notice(SELF, 0, 1, [SELF, BOB], k1))).toMatch(/couldn't be verified/);
    const k2 = key();
    check(k2, notice(BOB, 1, 1, [SELF, BOB], k2));
    expect(check(k1, notice(SELF, 0, 3, [SELF, BOB], k1))).toMatch(/couldn't be verified/);
    expect(check(k2, null, 1)).toMatch(/couldn't be verified/);
  });

  it("refuses a different key under an epoch already seen", () => {
    const k1 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    const other = key();
    expect(check(other, notice(BOB, 0, 2, [SELF, BOB], other))).toMatch(/couldn't be verified/);
  });

  it("refuses a notice that leaves this person out", () => {
    const k1 = key();
    expect(check(k1, notice(BOB, 0, 1, [BOB, EVE], k1))).toMatch(/couldn't be verified/);
  });

  it("control: a group with no notice yet still works until one has been seen", () => {
    expect(check(key(), null)).toBeNull();
  });

  it("flags members no notice named, so a rotation won't seal the key to them", () => {
    const k1 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    expect(unconfirmedMembers(SELF, GROUP, [SELF, BOB, EVE])).toEqual([EVE]);
    expect(unconfirmedMembers(SELF, GROUP, [SELF, BOB])).toEqual([]);
  });

  it("a notice names the verified members, so a routine change can't confirm someone the server added", () => {
    const k1 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    const named = membersToSign(SELF, GROUP, [SELF, BOB, EVE], [DM]);
    expect(named).toContain(BOB);
    expect(named).toContain(DM);
    expect(named).not.toContain(EVE);
    expect(unconfirmedMembers(SELF, GROUP, [SELF, BOB, EVE])).toEqual([EVE]);
  });

  it("a notice drops someone the server no longer lists", () => {
    const k1 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    expect(membersToSign(SELF, GROUP, [SELF], [])).toEqual([SELF]);
  });

  it("someone joining through a link names the group's last notice, not the server's list", () => {
    const k1 = key();
    const latest = notice(BOB, 0, 1, [BOB, GROUP], k1);
    const named = membersToSign(SELF, GROUP, [SELF, BOB, GROUP, EVE], [SELF], latest);
    expect(named.sort()).toEqual([SELF, BOB, GROUP].sort());
  });

  it("control: with nothing verified yet, the server's list is all there is", () => {
    expect(membersToSign(SELF, GROUP, [SELF, BOB], []).sort()).toEqual([SELF, BOB]);
  });

  it("an older epoch this device verified can't be swapped later", () => {
    const pair = crypto.generateKeyPair();
    const seal = (k: Uint8Array) => b64(crypto.sealConversationKey(k, pair.publicKey));
    const k1 = key();
    const k2 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    check(k2, notice(BOB, 1, 1, [SELF, BOB], k2));
    const dto = (older: Uint8Array) => ({ id: GROUP, isGroup: true, keyEpoch: 1, wrappedKey: seal(k2), keys: [{ epoch: 0, wrappedKey: seal(older) }] }) as never;
    const owner = { userId: SELF, ...pair };
    expect(keyRingFromDto(dto(key()), owner).keys.has(0)).toBe(false);
    expect(keyRingFromDto(dto(k1), owner).keys.has(0)).toBe(true);
  });

  it("a failed check drops the key, so it can't read or forge anything", async () => {
    const pair = crypto.generateKeyPair();
    const k1 = key();
    check(k1, notice(SELF, 0, 1, [SELF, BOB], k1));
    const k2 = key();
    const dto = { id: GROUP, isGroup: true, keyEpoch: 1, wrappedKey: b64(crypto.sealConversationKey(k2, pair.publicKey)), keys: [], keyNotice: null };
    const ring = keyRingFromDto(dto as never, { userId: SELF, ...pair });
    await verifyGroupRing(ring, dto as never, SELF);
    expect(ring.keyProblem).toMatch(/couldn't be verified/);
    expect(ring.keys.has(1)).toBe(false);
  });
});

describe("forged answers", () => {
  const key = () => crypto.generateConversationKey();
  const pair = () => crypto.generateKeyPair();

  it("the same pin holds for another spelling of the conversation id", () => {
    const p = pair();
    const owner = { userId: SELF, ...p };
    const seal = (k: Uint8Array) => b64(crypto.sealConversationKey(k, p.publicKey));
    const id = "abababab-abab-4aba-8aba-abababababab";
    keyRingFromDto({ id, isGroup: false, keyEpoch: 0, wrappedKey: seal(key()), keys: [] } as never, owner);
    const forged = keyRingFromDto({ id: id.toUpperCase(), isGroup: false, keyEpoch: 0, wrappedKey: seal(key()), keys: [] } as never, owner);
    expect(forged.keyProblem).toMatch(/changed/);
    expect(forged.keys.size).toBe(0);
    const upper = { userId: SELF.toUpperCase(), ...p };
    expect(keyRingFromDto({ id, isGroup: false, keyEpoch: 0, wrappedKey: seal(key()), keys: [] } as never, upper).keyProblem).toMatch(/changed/);
  });

  it("an epoch or sequence past 32 bits can't sign or verify as the real one", () => {
    const k = key();
    const real = notice(SELF, 1, 1, [SELF, BOB], k);
    check(k, real);
    expect(() => crypto.keyNoticeInput(GROUP, { ...real, epoch: 1 + 2 ** 32 }, crypto.keyCommitment(k))).toThrow();
    expect(() => crypto.keyNoticeInput(GROUP, { ...real, seq: 1 + 2 ** 32 }, crypto.keyCommitment(k))).toThrow();
    expect(() => crypto.keyNoticeInput(GROUP, { ...real, epoch: -1 }, crypto.keyCommitment(k))).toThrow();
    expect(() => crypto.keyNoticeInput(GROUP, { ...real, epoch: 1.5 }, crypto.keyCommitment(k))).toThrow();
    expect(() => crypto.keyNoticeInput("not-a-uuid", real, crypto.keyCommitment(k))).toThrow();
    const wrapped = { ...real, epoch: 1 + 2 ** 32 };
    expect(check(k, wrapped)).toMatch(/couldn't be verified/);
    expect(loadPin(SELF, GROUP)?.epoch).toBe(1);
  });

  it("the same sequence number with other members is refused", () => {
    const k = key();
    check(k, notice(SELF, 0, 1, [SELF, BOB], k));
    expect(check(k, notice(BOB, 0, 1, [SELF, BOB, EVE], k))).toMatch(/couldn't be verified/);
    expect(check(k, notice(SELF, 0, 1, [SELF, BOB], k))).toBeNull();
  });

  it("only older epochs are opened from the server's list", () => {
    const p = pair();
    const seal = (k: Uint8Array) => b64(crypto.sealConversationKey(k, p.publicKey));
    const ring = keyRingFromDto(
      {
        id: GROUP,
        isGroup: true,
        keyEpoch: 2,
        wrappedKey: seal(key()),
        keys: [{ epoch: 1, wrappedKey: seal(key()) }, { epoch: 999, wrappedKey: seal(key()) }, { epoch: -1, wrappedKey: seal(key()) }],
      } as never,
      { userId: SELF, ...p }
    );
    expect([...ring.keys.keys()].sort()).toEqual([1, 2]);
  });

  it("a notice can't be made without the key, even with a signing key of one's own", () => {
    const k = key();
    const forged = notice(EVE, 0, 9, [BOB, EVE], key()); // Signed for a key the forger made up.
    expect(noticeSignatureOk(GROUP, forged, k, signingKey)).toBe(false);
    expect(noticeSignatureOk(GROUP, notice(BOB, 0, 9, [BOB], k), k, signingKey)).toBe(true);
    expect(noticeSignatureOk(GROUP, notice(BOB, 0, 9, [BOB], k), k, () => null)).toBe(false);
  });

  it("a changed public key stops anything being sealed to it", () => {
    const first = b64(pair().publicKey);
    expect(() => requireUnchangedKey(SELF, BOB, first)).not.toThrow();
    expect(() => requireUnchangedKey(SELF, BOB, first)).not.toThrow();
    expect(() => requireUnchangedKey(SELF, BOB, b64(pair().publicKey))).toThrow(/safety number/);
    expect(() => requireUnchangedKey(SELF, BOB.toUpperCase(), b64(pair().publicKey))).toThrow(/safety number/);
  });

  it("a server-chosen public key at sign-in doesn't match the private key", () => {
    const p = pair();
    expect(b64(crypto.publicKeyOf(p.privateKey))).toBe(b64(p.publicKey));
    expect(b64(crypto.publicKeyOf(p.privateKey))).not.toBe(b64(pair().publicKey));
  });

  it("a rotation refuses to seal the new key to a swapped member key", async () => {
    vi.resetModules();
    const me = pair();
    const bob = pair();
    const swapped = pair();
    const k = key();
    const seal = (x: Uint8Array) => b64(crypto.sealConversationKey(x, me.publicKey));
    const dto = { id: GROUP, isGroup: true, keyEpoch: 0, wrappedKey: seal(k), keys: [], keyNotice: null, rekeyRequired: true };
    const rotated = vi.fn();
    localStorage.setItem(`redvaultsystem:pinned-key:${SELF}:${BOB}`, b64(bob.publicKey));
    vi.doMock("../lib/api.js", async (original) => ({
      ...(await original<typeof import("../lib/api.js")>()),
      listConversations: async () => [dto],
      listParticipants: async () => [{ userId: SELF }, { userId: BOB }],
      getUser: async (id: string) => ({ publicKey: b64(id === SELF ? me.publicKey : swapped.publicKey), signingPublicKey: null }),
      rotateGroupKey: rotated,
    }));
    const groupKeys = await import("../lib/groupKeys.js");
    groupKeys.setKeyRingOwner({ userId: SELF, ...me });
    await expect(groupKeys.rotateGroupKey(GROUP)).rejects.toThrow(/safety number/);
    expect(rotated).not.toHaveBeenCalled();
    groupKeys.setKeyRingOwner(null);
    vi.doUnmock("../lib/api.js");
  });

  it("a server can't turn a pinned signer back into a legacy sender, by saying nothing or by failing", async () => {
    vi.resetModules();
    const pinned = crypto.deriveSigningKeyPair(key());
    localStorage.setItem(`redvaultsystem:signing-key:${SELF}:${BOB}`, b64(pinned.publicKey));
    let answer: () => Promise<unknown> = async () => ({ signingPublicKey: null, signingKeySince: null });
    vi.doMock("../lib/api.js", async (original) => ({
      ...(await original<typeof import("../lib/api.js")>()),
      getUser: () => answer(),
    }));
    const signing = await import("../lib/messageSigning.js");
    const unsigned = { conversationId: DM, senderId: BOB, nonce: "AAAA", ciphertext: "AAAA", signature: null, createdAt: "2026-10-01T00:00:00.000Z" };
    await signing.resolveSenderKeys(SELF, [BOB]);
    expect(signing.checkSender(unsigned, false)).toBe("unverified");
    signing.resetMessageSigning();
    answer = async () => {
      throw new Error("down");
    };
    await signing.resolveSenderKeys(SELF, [BOB]);
    expect(signing.checkSender(unsigned, false)).toBe("unverified");
    vi.doUnmock("../lib/api.js");
  });
});

describe("id spelling", () => {
  it("a member's id in capitals can't earn a second signing key pin", async () => {
    vi.resetModules();
    const genuine = crypto.deriveSigningKeyPair(crypto.generateConversationKey());
    const attacker = crypto.deriveSigningKeyPair(crypto.generateConversationKey());
    const MEMBER = "abababab-abab-4aba-8aba-abababababab"; // Letters, so capitals really are a different spelling.
    const answers: Record<string, Uint8Array> = { [MEMBER]: genuine.publicKey, [MEMBER.toUpperCase()]: attacker.publicKey };
    vi.doMock("../lib/api.js", async (original) => ({
      ...(await original<typeof import("../lib/api.js")>()),
      getUser: async (id: string) => ({ signingPublicKey: b64(answers[id]), signingKeySince: "2026-10-01" }),
    }));
    const signing = await import("../lib/messageSigning.js");
    await signing.resolveSenderKeys(SELF, [MEMBER]);
    await signing.resolveSenderKeys(SELF, [MEMBER.toUpperCase()]);
    expect(b64(signing.senderSigningKey(MEMBER.toUpperCase())!)).toBe(b64(genuine.publicKey));
    expect(b64(signing.senderSigningKey(MEMBER)!)).toBe(b64(genuine.publicKey));
    vi.doUnmock("../lib/api.js");
  });
});

describe("shared vectors", () => {
  const vectors = JSON.parse(readFileSync(resolve(__dirname, "../../../desktop/tests/vectors/key_notice_vectors.json"), "utf8"));

  it("commitment and signed bytes match what every client must produce", () => {
    for (const v of vectors.cases) {
      const key = crypto.fromBase64(v.key);
      const commitment = crypto.keyCommitment(key);
      expect(b64(commitment)).toBe(v.commitment);
      expect(b64(crypto.keyNoticeInput(v.conversationId, v.notice, commitment))).toBe(v.input);
    }
  });
});
