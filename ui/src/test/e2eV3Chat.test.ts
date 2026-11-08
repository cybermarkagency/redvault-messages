// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// @vitest-environment node
// Protocol-3 chat (lib/e2eV3/chat.ts, docs/multi-device-threat-model.md section 9):
// the real engine, simulated browsers (separate IndexedDBs) and an in-memory
// server (fakeV3Api.ts). Skipped when crypto-core/build-web.sh hasn't been run.
import { existsSync } from "node:fs";
import { createRequire } from "node:module";
import { resolve } from "node:path";
import { IDBFactory } from "fake-indexeddb";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type { FakeV3Server } from "./fakeV3Api.js";

const holder = vi.hoisted(() => ({ server: null as FakeV3Server | null }));

vi.mock("../lib/api.js", async (importActual) => {
  const actual = await importActual<typeof import("../lib/api.js")>();
  const { FakeV3Server } = await import("./fakeV3Api.js");
  holder.server = new FakeV3Server(actual.ApiError);
  return { ...actual, ...holder.server.module() };
});

await import("../lib/api.js");
const server = holder.server!;

const { DeviceStore } = await import("../lib/e2eV3/deviceStore.js");
const engine = await import("../lib/e2eV3/engine.js");
const v3 = await import("../lib/e2eV3/device.js");
const chat = await import("../lib/e2eV3/chat.js");
const { NewDevicePairing, TrustedPairing } = await import("../lib/e2eV3/pairing.js");

const enginePath = resolve(process.cwd(), "../crypto-core/pkg/node/redvault_e2ee.js");
const available = existsSync(enginePath);
const fast = { intervalMs: 5, timeoutMs: 5000 };

const ALICE = "11111111-1111-4111-8111-111111111111";
const BOB = "22222222-2222-4222-8222-222222222222";
const CAROL = "44444444-4444-4444-8444-444444444444";
const DM = "33333333-3333-4333-8333-333333333333";
const GROUP = "55555555-5555-4555-8555-555555555555";

type Device = import("../lib/e2eV3/device.js").V3Device;

interface Browser {
  device: Device;
  history: InstanceType<typeof chat.ChatHistory>;
  userId: string;
  store?: InstanceType<typeof DeviceStore>;
}

describe.skipIf(!available)("protocol 3 chat in the web app", () => {
  beforeAll(() => {
    const { call } = createRequire(enginePath)(enginePath);
    engine.useEngineForTests(call);
  });

  beforeEach(() => {
    server.lists.clear();
    server.devices.clear();
    server.mailbox.length = 0;
    server.enrollments.clear();
    server.sent.length = 0;
    server.groups.clear();
    server.tamper = null;
  });

  async function firstBrowser(userId: string): Promise<Browser> {
    server.currentUser = userId;
    const factory = new IDBFactory();
    const store = new DeviceStore(factory);
    const device = await v3.setUpFirstDevice(userId, crypto.getRandomValues(new Uint8Array(32)), store);
    return { device, history: new chat.ChatHistory(device, factory), userId, store };
  }

  async function secondBrowser(trusted: Browser): Promise<Browser> {
    server.currentUser = trusted.userId;
    const factory = new IDBFactory();
    const device = await v3.startNewDevice(trusted.userId, new DeviceStore(factory));
    const newSide = await NewDevicePairing.begin(device);
    const [, trustedSide] = await Promise.all([newSide.codes(fast), TrustedPairing.begin(trusted.device, newSide.enrollmentId, fast)]);
    await Promise.all([newSide.confirm(fast), trustedSide.pairing.confirm(fast)]);
    return { device, history: new chat.ChatHistory(device, factory), userId: trusted.userId };
  }

  const dm = (contact: string) => ({ conversationId: DM, contactUserId: contact });
  const group = { conversationId: GROUP, contactUserId: null };
  const as = (b: Browser) => {
    server.currentUser = b.userId;
  };
  const texts = async (b: Browser, conversationId: string) => (await b.history.list(conversationId)).map((m) => `${m.direction}:${m.text}`);

  it("one-to-one: the contact and the sender's other devices get the message, once, with history kept per device", async () => {
    const alicePhone = await firstBrowser(ALICE);
    const aliceLaptop = await secondBrowser(alicePhone);
    const bob = await firstBrowser(BOB);

    as(alicePhone);
    expect(await chat.chatMode(alicePhone.device, alicePhone.history, dm(BOB))).toBe("v3");
    await chat.sendChatText(alicePhone.device, alicePhone.history, dm(BOB), "hello bob 👋");
    as(bob);
    expect((await chat.receiveChat(bob.device, bob.history)).map((m) => m.text)).toEqual(["hello bob 👋"]);
    as(aliceLaptop);
    const copy = await chat.receiveChat(aliceLaptop.device, aliceLaptop.history);
    expect(copy.map((m) => [m.direction, m.text])).toEqual([["out", "hello bob 👋"]]);

    as(bob);
    await chat.sendChatText(bob.device, bob.history, dm(ALICE), "hi alice");
    as(alicePhone);
    await chat.receiveChat(alicePhone.device, alicePhone.history);
    as(aliceLaptop);
    await chat.receiveChat(aliceLaptop.device, aliceLaptop.history);

    const expected = ["out:hello bob 👋", "in:hi alice"];
    // The chat list previews the newest message of each conversation.
    const latest = await alicePhone.history.latestPerConversation();
    expect(latest.map((m) => [m.conversationId, m.text])).toEqual([[DM, "hi alice"]]);
    expect(await texts(alicePhone, DM)).toEqual(expected);
    expect(await texts(aliceLaptop, DM)).toEqual(expected);
    expect(await texts(bob, DM)).toEqual(["in:hello bob 👋", "out:hi alice"]);
    // Everything was acknowledged; nothing is left on the server.
    expect(server.mailbox).toHaveLength(0);
  });

  it("keeps history encrypted at rest and bound to its conversation and id", async () => {
    const alice = await firstBrowser(ALICE);
    await firstBrowser(BOB);
    as(alice);
    await alice.history.add({
      conversationId: DM,
      id: crypto.randomUUID(),
      fromUserId: ALICE,
      fromDeviceId: alice.device.deviceId,
      ts: 1,
      arrivedAt: 1,
      text: "a secret sentence",
      direction: "out",
      status: "sent",
    });
    const raw = await new Promise<unknown[]>((resolveAll, reject) => {
      const factory = (alice.history as unknown as { factory: IDBFactory }).factory;
      const open = factory.open("redvault-e2e-v3-history");
      open.onsuccess = () => {
        const all = open.result.transaction("messages").objectStore("messages").getAll();
        all.onsuccess = () => resolveAll(all.result);
        all.onerror = () => reject(all.error);
      };
    });
    expect(raw).toHaveLength(1);
    expect(JSON.stringify(Array.from(new Uint8Array((raw[0] as { sealed: Uint8Array }).sealed)))).not.toContain("secret");
    expect(Buffer.from((raw[0] as { sealed: Uint8Array }).sealed).toString("utf8")).not.toContain("a secret sentence");
  });

  it("drops a message whose envelope names another conversation, and acknowledges it", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    as(alice);
    // alice sends an envelope bound to a different conversation through DM's mailbox lane.
    const wrong = chat.encodeEnvelope("66666666-6666-4666-8666-666666666666", "moved");
    await v3.sendText(alice.device, DM, BOB, wrong);
    as(bob);
    expect(await chat.receiveChat(bob.device, bob.history)).toEqual([]);
    expect(await texts(bob, DM)).toEqual([]);
    expect(server.mailbox).toHaveLength(0);
  });

  it("does not acknowledge anything if the history can't be written", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    as(alice);
    await chat.sendChatText(alice.device, alice.history, dm(BOB), "keep me on the server");
    as(bob);
    const broken = { ...bob.history, add: async () => { throw new Error("disk full"); }, pin: async () => {} } as unknown as typeof bob.history;
    await expect(chat.receiveChat(bob.device, broken)).rejects.toThrow("disk full");
    expect(server.mailbox).toHaveLength(1);
    // The engine already advanced, so the redelivery is a duplicate for the engine: the message
    // is lost on this device (9.6 crash window), but nothing blocks the mailbox afterwards.
    await chat.receiveChat(bob.device, bob.history);
    expect(server.mailbox).toHaveLength(0);
  });

  it("a pinned chat refuses to fall back when the contact has no device keys; an unpinned one uses protocol 1", async () => {
    const alice = await firstBrowser(ALICE);
    as(alice);
    server.lists.delete(BOB);
    expect(await chat.chatMode(alice.device, alice.history, dm(BOB))).toBe("v1");
    await alice.history.pin(DM);
    expect(await chat.chatMode(alice.device, alice.history, dm(BOB))).toBe("blocked");
    await expect(chat.sendChatText(alice.device, alice.history, dm(BOB), "x")).rejects.toBeInstanceOf(chat.PinnedChatError);
    expect(server.mailbox).toHaveLength(0);
    await alice.history.unpin(DM);
    expect(await chat.chatMode(alice.device, alice.history, dm(BOB))).toBe("v1");
  });

  it("groups: three members, key shared once per device, messages read by all, a removed member gets nothing new", async () => {
    const aliceA = await firstBrowser(ALICE);
    const aliceB = await secondBrowser(aliceA);
    const bob = await firstBrowser(BOB);
    const carol = await firstBrowser(CAROL);
    server.groups.set(GROUP, [ALICE, BOB, CAROL]);

    as(aliceA);
    await chat.sendChatText(aliceA.device, aliceA.history, group, "welcome all");
    const keysSent = server.mailbox.filter((m) => !m.body.includes('"kind":"group"')).length;
    expect(keysSent).toBe(3); // aliceB, bob, carol: one Olm room key each
    for (const reader of [aliceB, bob, carol]) {
      as(reader);
      await chat.receiveChat(reader.device, reader.history);
      expect(await texts(reader, GROUP)).toEqual([reader === aliceB ? "out:welcome all" : "in:welcome all"]);
    }

    // The same sending session again: no new room keys.
    as(aliceA);
    await chat.sendChatText(aliceA.device, aliceA.history, group, "second");
    expect(server.mailbox.filter((m) => !m.body.includes('"kind":"group"'))).toHaveLength(0);
    // bob answers, using his own session: a room key goes to every other member device.
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    await chat.sendChatText(bob.device, bob.history, group, "thanks!");
    for (const reader of [aliceA, aliceB, carol]) {
      as(reader);
      await chat.receiveChat(reader.device, reader.history);
    }
    expect(await texts(carol, GROUP)).toEqual(["in:welcome all", "in:second", "in:thanks!"]);
    expect(await texts(aliceA, GROUP)).toEqual(["out:welcome all", "out:second", "in:thanks!"]);
    // The sender shown is the authenticated one.
    expect((await carol.history.list(GROUP)).map((m) => m.fromUserId)).toEqual([ALICE, ALICE, BOB]);

    // carol leaves: alice's next message uses a new session that carol never gets.
    server.groups.set(GROUP, [ALICE, BOB]);
    as(aliceA);
    await chat.sendChatText(aliceA.device, aliceA.history, group, "carol is gone");
    as(carol);
    await chat.receiveChat(carol.device, carol.history);
    expect(await texts(carol, GROUP)).toEqual(["in:welcome all", "in:second", "in:thanks!"]);
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    expect((await texts(bob, GROUP)).slice(-1)).toEqual(["in:carol is gone"]);
  });

  it("a group message that arrives before its room key waits and is read once the key comes", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    server.groups.set(GROUP, [ALICE, BOB]);
    as(alice);
    await chat.sendChatText(alice.device, alice.history, group, "early");
    // The server delivers the group message first: move the room key behind it.
    const keyIndex = server.mailbox.findIndex((m) => !m.body.includes('"kind":"group"'));
    const [key] = server.mailbox.splice(keyIndex, 1);
    as(bob);
    expect(await chat.receiveChat(bob.device, bob.history)).toEqual([]);
    expect(server.mailbox).toHaveLength(1); // held, not acknowledged
    server.mailbox.unshift(key);
    expect((await chat.receiveChat(bob.device, bob.history)).map((m) => m.text)).toEqual(["early"]);
    expect(server.mailbox).toHaveLength(0);
  });

  it("a contact's changed account key blocks sending with a clear reason until the person trusts the new keys", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    as(alice);
    await chat.sendChatText(alice.device, alice.history, dm(BOB), "before the reset");
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    as(alice);
    const oldNumber = await v3.deviceKeySafetyNumber(alice.device, BOB, DM);

    // bob sets up again from scratch: a new account key.
    server.lists.delete(BOB);
    for (const key of [...server.devices.keys()]) if (key.startsWith(`${BOB}/`)) server.devices.delete(key);
    const bobAgain = await firstBrowser(BOB);

    as(alice);
    expect(await chat.chatMode(alice.device, alice.history, dm(BOB))).toBe("blocked");
    const failure = await chat.sendChatText(alice.device, alice.history, dm(BOB), "after").catch((err) => err);
    expect(failure).toBeInstanceOf(v3.EngineError);
    expect(failure.code).toBe("account_key_changed");
    expect(v3.keyProblemMessage(failure.code)).toContain("Compare the new safety number");

    // The Verify dialog and the chat open the device separately: they must share one instance.
    const dialogDevice = (await v3.V3Device.open(ALICE, alice.store))!;
    expect(dialogDevice).toBe(alice.device);
    // A refused send is kept as "failed", not as a sent message.
    expect((await alice.history.list(DM)).filter((m) => m.status === "failed").map((m) => m.text)).toEqual(["after"]);

    const change = await v3.pendingKeyChange(dialogDevice, BOB, DM);
    expect(change?.safetyNumber).toHaveLength(12);
    expect(change?.safetyNumber).not.toEqual(oldNumber);
    await v3.trustNewKeys(dialogDevice, BOB, change!.accountKey, DM);
    expect(await v3.pendingKeyChange(alice.device, BOB, DM)).toBeNull();
    expect(await v3.deviceKeySafetyNumber(alice.device, BOB, DM)).toEqual(change!.safetyNumber);

    await chat.sendChatText(alice.device, alice.history, dm(BOB), "after trusting");
    as(bobAgain);
    expect((await chat.receiveChat(bobAgain.device, bobAgain.history)).map((m) => m.text)).toEqual(["after trusting"]);
  });

  it("reactions and edits reach the other person and the sender's other devices", async () => {
    const alicePhone = await firstBrowser(ALICE);
    const aliceLaptop = await secondBrowser(alicePhone);
    const bob = await firstBrowser(BOB);
    as(alicePhone);
    const sent = await chat.sendChatText(alicePhone.device, alicePhone.history, dm(BOB), "see you at 8");
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    await chat.sendChatOp(bob.device, bob.history, dm(ALICE), { t: "reaction", target: sent.id, emoji: "👍" });
    as(alicePhone);
    await chat.receiveChat(alicePhone.device, alicePhone.history);
    await chat.sendChatOp(alicePhone.device, alicePhone.history, dm(BOB), { t: "edit", target: sent.id, body: "see you at 9" });
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    as(aliceLaptop);
    await chat.receiveChat(aliceLaptop.device, aliceLaptop.history);

    for (const browser of [alicePhone, bob, aliceLaptop]) {
      const [message, ...rest] = await browser.history.list(DM);
      expect(rest).toEqual([]);
      expect(message.text).toBe("see you at 9");
      expect(message.edited).toBe(true);
    }
    expect((await alicePhone.history.list(DM))[0].reactions).toEqual([{ userId: BOB, emoji: "👍" }]);
    expect((await bob.history.list(DM))[0].reactions).toEqual([{ userId: BOB, emoji: "👍" }]);
  });

  it("delete for everyone erases the words from the recipient's stored history, not just the screen", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    as(alice);
    const sent = await chat.sendChatText(alice.device, alice.history, dm(BOB), "delete this secret");
    await chat.sendChatOp(alice.device, alice.history, dm(BOB), { t: "edit", target: sent.id, body: "delete this edit too" });
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    await chat.sendChatOp(bob.device, bob.history, dm(ALICE), { t: "reaction", target: sent.id, emoji: "👍" });
    as(alice);
    await chat.receiveChat(alice.device, alice.history);
    await chat.sendChatOp(alice.device, alice.history, dm(BOB), { t: "delete", target: sent.id });
    as(bob);
    await chat.receiveChat(bob.device, bob.history);

    for (const browser of [alice, bob]) {
      const [message] = await browser.history.list(DM);
      expect(message).toMatchObject({ text: "", deleted: true });
      expect(message.reactions).toBeUndefined();
      const raw = await (browser.history as unknown as { records(id: string): Promise<unknown[]> }).records(DM);
      expect(JSON.stringify(raw)).not.toContain("delete this");
      expect(JSON.stringify(raw)).not.toContain("👍");
    }
  });

  it("nobody can edit or delete someone else's message, even with a valid session to them", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    as(alice);
    const sent = await chat.sendChatText(alice.device, alice.history, dm(BOB), "my words");
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    await chat.sendChatOp(bob.device, bob.history, dm(ALICE), { t: "edit", target: sent.id, body: "words I put in your mouth" });
    await chat.sendChatOp(bob.device, bob.history, dm(ALICE), { t: "delete", target: sent.id });
    as(alice);
    await chat.receiveChat(alice.device, alice.history);
    expect(await alice.history.list(DM)).toMatchObject([{ text: "my words" }]);
    expect((await alice.history.list(DM))[0].deleted).toBeUndefined();
    expect((await alice.history.list(DM))[0].edited).toBeUndefined();
  });

  it("a replayed edit changes nothing", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    as(alice);
    const sent = await chat.sendChatText(alice.device, alice.history, dm(BOB), "one");
    await chat.sendChatOp(alice.device, alice.history, dm(BOB), { t: "edit", target: sent.id, body: "two" });
    const copy = [...server.mailbox];
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    server.mailbox.push(...copy);
    await chat.receiveChat(bob.device, bob.history);
    expect((await bob.history.list(DM)).map((m) => m.text)).toEqual(["two"]);
  });

  it("a file's key reaches the recipient and never reaches the server", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    const file = {
      type: "file",
      version: 1,
      objectId: "66666666-6666-4666-8666-666666666666",
      fileKey: "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=",
      filename: "plans.pdf",
      mimeType: "application/pdf",
      originalSize: 5,
      encryptedSize: 46,
      ciphertextSha256: "a".repeat(64),
      encryption: { algorithm: "xchacha20poly1305-secretstream", version: 1, header: "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", chunkSize: 65536 },
    } as never;
    as(alice);
    await chat.sendChatFile(alice.device, alice.history, dm(BOB), file);
    const onTheWire = JSON.stringify([server.sent, server.mailbox]);
    expect(onTheWire).not.toContain("plans.pdf");
    expect(onTheWire).not.toContain("c2VjcmV0LWZpbGUta2V5");
    as(bob);
    const [received] = await chat.receiveChat(bob.device, bob.history);
    expect(received.file?.filename).toBe("plans.pdf");
    expect(received.file?.fileKey).toBe("c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=");
  });

  it("group: reactions and edits reach every member", async () => {
    const alice = await firstBrowser(ALICE);
    const bob = await firstBrowser(BOB);
    server.groups.set(GROUP, [ALICE, BOB]);
    as(alice);
    const sent = await chat.sendChatText(alice.device, alice.history, group, "hello group");
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    await chat.sendChatOp(bob.device, bob.history, group, { t: "reaction", target: sent.id, emoji: "🔥" });
    as(alice);
    await chat.receiveChat(alice.device, alice.history);
    await chat.sendChatOp(alice.device, alice.history, group, { t: "edit", target: sent.id, body: "hello everyone" });
    as(bob);
    await chat.receiveChat(bob.device, bob.history);
    expect(await alice.history.list(GROUP)).toMatchObject([{ text: "hello everyone", reactions: [{ userId: BOB, emoji: "🔥" }] }]);
    expect(await bob.history.list(GROUP)).toMatchObject([{ text: "hello everyone", edited: true }]);
  });

  it("sends no plaintext and no private key to the server", async () => {
    const alice = await firstBrowser(ALICE);
    await firstBrowser(BOB);
    server.groups.set(GROUP, [ALICE, BOB]);
    as(alice);
    await chat.sendChatText(alice.device, alice.history, dm(BOB), "PLAINTEXT-MARKER-ONE");
    await chat.sendChatText(alice.device, alice.history, group, "PLAINTEXT-MARKER-TWO");
    const everything = server.sent.join("\n") + JSON.stringify(server.mailbox);
    expect(everything).not.toContain("PLAINTEXT-MARKER");
  });
});
