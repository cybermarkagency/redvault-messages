// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import sodium from "libsodium-wrappers-sumo";
import { beforeAll, describe, expect, it } from "vitest";
import { loadCallHistory, recordCall, type CallRecord } from "../lib/callHistory.js";
import { callLinkUrl, newCallLinkSecret, openLinkName, openLinkSignal, openOwnedLink, parseCallLink, sealLinkName, sealLinkSignal, sealSecretForOwner, secretFromFragment } from "../lib/callLinkCrypto.js";
import type { CallSignal } from "../lib/callCrypto.js";
import * as crypto from "../lib/e2eCrypto.js";
import { readPrefs, toggleIn, writePrefs } from "../lib/localPrefs.js";
import { hexToBytes, leafHash, verifyConsistency, verifyInclusion, bytesToHex } from "../lib/merkle.js";
import { SAS_WORD_COUNT, sasWords, sdpFingerprint } from "../lib/sas.js";
import { checkpointMessage, signatureValid } from "../lib/transparencyLog.js";
import vectors from "./merkleVectors.json";

beforeAll(async () => {
  await crypto.ready();
  await sodium.ready;
});

describe("Merkle proof verification (matches the server's tree)", () => {
  const leaves = vectors.leaves.map(hexToBytes);

  it("verifies inclusion proofs made by the server, and rejects wrong ones", async () => {
    const root = hexToBytes(vectors.root13);
    expect(await verifyInclusion(await leafHash(leaves[5]), 5, 13, vectors.proof5in13.map(hexToBytes), root)).toBe(true);
    expect(await verifyInclusion(await leafHash(leaves[12]), 12, 13, vectors.proof12in13.map(hexToBytes), root)).toBe(true);
    expect(await verifyInclusion(await leafHash(leaves[6]), 5, 13, vectors.proof5in13.map(hexToBytes), root)).toBe(false);
    expect(await verifyInclusion(await leafHash(leaves[5]), 5, 13, vectors.proof12in13.map(hexToBytes), root)).toBe(false);
  });

  it("verifies consistency proofs, and catches a forged earlier root", async () => {
    const to = hexToBytes(vectors.root13);
    expect(await verifyConsistency(7, 13, hexToBytes(vectors.root7), to, vectors.consistency7to13.map(hexToBytes))).toBe(true);
    expect(await verifyConsistency(4, 13, hexToBytes(vectors.root4), to, vectors.consistency4to13.map(hexToBytes))).toBe(true);
    expect(await verifyConsistency(7, 13, hexToBytes(vectors.root4), to, vectors.consistency7to13.map(hexToBytes))).toBe(false);
  });

  it("hex helpers round-trip", () => {
    expect(bytesToHex(hexToBytes(vectors.root13))).toBe(vectors.root13);
  });
});

describe("checkpoint signatures", () => {
  it("accepts the log key's signature over exactly the published fields", () => {
    const keys = sodium.crypto_sign_keypair();
    const cp = { treeSize: 42, rootHash: "ab".repeat(32), signedAt: "2026-09-28T12:34:00.000Z", publicKey: crypto.toBase64(keys.publicKey), signature: "" };
    cp.signature = crypto.toBase64(sodium.crypto_sign_detached(checkpointMessage(cp.treeSize, cp.rootHash, cp.signedAt), keys.privateKey));
    expect(signatureValid(cp)).toBe(true);
    expect(signatureValid({ ...cp, treeSize: 43 })).toBe(false);
    expect(signatureValid({ ...cp, publicKey: crypto.toBase64(sodium.crypto_sign_keypair().publicKey) })).toBe(false);
  });
});

describe("call safety words", () => {
  it("has one distinct word per byte", () => {
    expect(SAS_WORD_COUNT).toBe(256);
    const all = new Set<string>();
    for (let i = 0; i < 4000; i++) sasWords(`sha-256 ${i}`, "sha-256 x").forEach((w) => all.add(w));
    expect([...all].every((w) => typeof w === "string" && w.length > 0)).toBe(true);
  });

  it("are the same on both sides and change with either fingerprint", () => {
    const a = "sha-256 AA:BB:CC";
    const b = "sha-256 11:22:33";
    expect(sasWords(a, b)).toEqual(sasWords(b, a));
    expect(sasWords(a, b)).toHaveLength(4);
    expect(sasWords(a, "sha-256 11:22:34")).not.toEqual(sasWords(a, b));
    expect(sdpFingerprint("v=0\r\na=fingerprint:sha-256 AB:CD\r\n")).toBe("sha-256 AB:CD");
  });
});

describe("call links", () => {
  const signal = (overrides: Partial<CallSignal> = {}): CallSignal => ({
    v: 1,
    type: "offer",
    callId: "call-1",
    conversationId: "link:abcdefghijkl",
    from: "guest",
    to: "owner",
    kind: "video",
    sdp: "v=0\r\na=fingerprint:sha-256 AB\r\n",
    sentAt: Date.now(),
    ...overrides,
  });
  const expected = { type: "offer" as const, callId: "call-1", linkId: "abcdefghijkl", from: "guest", to: "owner" };

  it("keeps the secret in the URL fragment, which browsers never send to a server", () => {
    const secret = newCallLinkSecret();
    const url = new URL(callLinkUrl("abcdefghijkl", secret));
    expect(url.searchParams.get("join")).toBe("abcdefghijkl");
    expect(Array.from(secretFromFragment(url.hash)!)).toEqual(Array.from(secret));
    expect(url.search).not.toContain(url.hash.slice(1));
  });

  it("reads the id and secret out of a pasted link, and rejects anything else", () => {
    const secret = newCallLinkSecret();
    const url = callLinkUrl("abcdefghijkl", secret);
    const fragment = new URL(url).hash.slice(1);
    expect(parseCallLink(url)).toEqual({ linkId: "abcdefghijkl", secret: fragment });
    expect(parseCallLink(`  ${url}\n`)).toEqual({ linkId: "abcdefghijkl", secret: fragment });
    expect(parseCallLink(`https://other.example/?join=abcdefghijkl#${fragment}`)?.linkId).toBe("abcdefghijkl");
    expect(Array.from(secretFromFragment(parseCallLink(url)!.secret)!)).toEqual(Array.from(secret));

    expect(parseCallLink("")).toBeNull();
    expect(parseCallLink("hello there")).toBeNull();
    expect(parseCallLink("https://example.com/?join=abcdefghijkl")).toBeNull();
    expect(parseCallLink(`https://example.com/#${fragment}`)).toBeNull();
    expect(parseCallLink(`https://example.com/?join=abcdefghijkl#${fragment.slice(1)}`)).toBeNull();
  });

  it("the owner gets the secret and name back with their own keys", () => {
    const owner = crypto.generateKeyPair();
    const secret = newCallLinkSecret();
    const name = sealLinkName("Weekly design sync", secret);
    const opened = openOwnedLink(
      { id: "abcdefghijkl", kind: "voice", secretSealed: sealSecretForOwner(secret, owner.publicKey), createdOn: "2026-09-28", ...name },
      owner.publicKey,
      owner.privateKey
    );
    expect(opened?.name).toBe("Weekly design sync");
    expect(openLinkName(name.nameCiphertext, name.nameNonce, secret)).toBe("Weekly design sync");
  });

  it("only signals sealed with the link's secret, for this call, are accepted", () => {
    const secret = newCallLinkSecret();
    expect(openLinkSignal(sealLinkSignal(signal(), secret), secret, expected).kind).toBe("video");
    expect(() => openLinkSignal(sealLinkSignal(signal(), newCallLinkSecret()), secret, expected)).toThrow();
    expect(() => openLinkSignal(sealLinkSignal(signal({ callId: "other" }), secret), secret, expected)).toThrow();
    expect(() => openLinkSignal(sealLinkSignal(signal({ conversationId: "link:zzzzzzzzzzzz" }), secret), secret, expected)).toThrow();
    expect(() => openLinkSignal(sealLinkSignal(signal({ sentAt: Date.now() - 600_000 }), secret), secret, expected)).toThrow();
  });
});

describe("on-device call history", () => {
  it("is encrypted at rest and readable only with the account's key", () => {
    localStorage.clear();
    const me = crypto.generateKeyPair();
    const record: CallRecord = {
      id: "c1",
      peerUserId: "u2",
      peerHandle: "maya.okafor",
      conversationId: "conv",
      linkName: null,
      kind: "video",
      direction: "outgoing",
      outcome: "answered",
      startedAt: 1,
      durationSec: 480,
    };
    recordCall("me", me.privateKey, record);
    expect(loadCallHistory("me", me.privateKey)).toEqual([record]);
    const stored = JSON.stringify(Object.fromEntries(Object.entries(localStorage)));
    expect(stored).not.toContain("maya.okafor");
    expect(loadCallHistory("me", crypto.generateKeyPair().privateKey)).toEqual([]);
  });
});

describe("local prefs", () => {
  it("are per account and survive a reload", () => {
    localStorage.clear();
    writePrefs("a", { favorites: toggleIn([], "u1"), muted: ["u2"] });
    expect(readPrefs("a")).toMatchObject({ favorites: ["u1"], muted: ["u2"], ringtone: true });
    expect(readPrefs("b").favorites).toEqual([]);
    expect(toggleIn(["u1"], "u1")).toEqual([]);
  });
});
