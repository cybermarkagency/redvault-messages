// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import "fake-indexeddb/auto";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import type * as Api from "../lib/api.js";

// A small stand-in for the server's protocol-2 endpoints, acting as whoever
// `as` is set to. It does what the real one checks (see
// server/src/services/deviceService.ts): one payload per other device, each
// one-time prekey handed out once, payloads deleted on ack.
const server = vi.hoisted(() => {
  class FakeApiError extends Error {
    constructor(
      public readonly status: number,
      public readonly code: string,
      message = code
    ) {
      super(message);
    }
  }
  type Device = { id: string; userId: string; preKeys: Api.E2ePreKeysInput; oneTime: Api.E2eOneTimePreKeyInput[] };
  type Stored = Api.MessageDto & { payloads: Map<string, string> };
  const state = {
    as: "",
    enabled: true,
    publicKeys: new Map<string, string>(),
    conversations: new Map<string, string[]>(),
    devices: new Map<string, Device>(),
    messages: [] as Stored[],
    seq: 0,
  };
  const uuid = () => `00000000-0000-4000-8000-${String(++state.seq).padStart(12, "0")}`;
  const own = (deviceId: string) => {
    const d = state.devices.get(deviceId);
    if (!d || d.userId !== state.as) throw new FakeApiError(404, "not_found");
    return d;
  };
  const devicesOf = (userId: string) => [...state.devices.values()].filter((d) => d.userId === userId);

  const api = {
    ApiError: FakeApiError,
    getE2eConfig: async () => ({ enabled: state.enabled }),
    getUser: async (userId: string) => ({ userId, username: userId, publicKey: state.publicKeys.get(userId)!, avatarUrl: null }),
    registerE2eDevice: async (input: Api.E2ePreKeysInput & { oneTimePreKeys: Api.E2eOneTimePreKeyInput[] }) => {
      const device: Device = { id: uuid(), userId: state.as, preKeys: input, oneTime: [...input.oneTimePreKeys] };
      state.devices.set(device.id, device);
      return { id: device.id, createdOn: "2026-09-28", signedPreKeyId: input.signedPreKey.id, pqPreKeyId: input.pqPreKey.id };
    },
    getE2eDevice: async (deviceId: string) => {
      const d = own(deviceId);
      return { id: d.id, createdOn: "2026-09-28", signedPreKeyId: 0, pqPreKeyId: 0, oneTimePreKeyCount: d.oneTime.length };
    },
    addE2eOneTimePreKeys: async (deviceId: string, keys: Api.E2eOneTimePreKeyInput[]) => {
      own(deviceId).oneTime.push(...keys);
    },
    rotateE2ePreKeys: async (deviceId: string, input: Api.E2ePreKeysInput) => {
      own(deviceId).preKeys = input;
    },
    listConversationDevices: async (conversationId: string) =>
      state.conversations.get(conversationId)!.map((userId) => ({ userId, deviceIds: devicesOf(userId).map((d) => d.id) })),
    fetchPreKeyBundles: async (_conversationId: string, targets: { userId: string; deviceId: string }[]) =>
      targets.map((t) => {
        const d = state.devices.get(t.deviceId)!;
        const oneTime = d.oneTime.shift() ?? null;
        return { userId: t.userId, deviceId: d.id, identityKey: state.publicKeys.get(t.userId)!, ...d.preKeys, oneTimePreKey: oneTime };
      }),
    sendMessage: async (conversationId: string, ciphertext: string, nonce: string, _attachments?: string[], fs?: Api.ForwardSecretEnvelope) => {
      const members = state.conversations.get(conversationId)!;
      if (fs) {
        if (members.some((u) => u !== state.as && devicesOf(u).length === 0)) throw new FakeApiError(409, "participant_without_devices");
        const expected = members.flatMap((u) => devicesOf(u).map((d) => d.id)).filter((id) => id !== fs.senderDeviceId).sort();
        const given = fs.payloads.map((p) => p.deviceId).sort();
        if (JSON.stringify(expected) !== JSON.stringify(given)) throw new FakeApiError(409, "device_list_stale");
      }
      const message: Stored = {
        id: uuid(),
        conversationId,
        senderId: state.as,
        ciphertext,
        nonce,
        createdAt: new Date(Date.now() + state.seq).toISOString(),
        editedAt: null,
        deletedAt: null,
        deleteAt: null,
        protocol: fs ? 2 : 1,
        senderDeviceId: fs?.senderDeviceId ?? null,
        payloads: new Map(fs?.payloads.map((p) => [p.deviceId, p.payload]) ?? []),
      };
      state.messages.push(message);
      return message;
    },
    listMessages: async (conversationId: string, options?: { deviceId?: string }) => {
      if (options?.deviceId) own(options.deviceId);
      return state.messages
        .filter((m) => m.conversationId === conversationId)
        .map(({ payloads, ...m }) => (options?.deviceId && m.protocol === 2 ? { ...m, payload: payloads.get(options.deviceId) ?? null } : m));
    },
    acknowledgeE2ePayloads: async (deviceId: string, messageIds: string[]) => {
      own(deviceId);
      for (const m of state.messages) if (messageIds.includes(m.id)) m.payloads.delete(deviceId);
    },
  };
  return { state, api, FakeApiError };
});

vi.mock("../lib/api.js", () => server.api);

import * as crypto from "../lib/e2eCrypto.js";
import { ForwardSecretDevice } from "../lib/e2eV2/device.js";
import { generatePqPreKey, generateSignedPreKey } from "../lib/e2eV2/pqxdh.js";
import { sendEncryptedMessage } from "../lib/e2eV2/messaging.js";
import { currentForwardSecretDevice, beginForwardSecrecy, stopForwardSecrecy } from "../lib/e2eV2/device.js";

beforeAll(() => crypto.ready());

let users: Record<string, { userId: string; publicKey: Uint8Array; privateKey: Uint8Array }>;

beforeEach(() => {
  server.state.enabled = true;
  server.state.conversations.clear();
  server.state.devices.clear();
  server.state.messages.length = 0;
  users = {};
  for (const name of ["alice", "bob", "carol"]) {
    const kp = crypto.generateKeyPair();
    // Fresh user ids per test: each test's devices get a clean local store.
    const userId = `${name}-${Math.random().toString(36).slice(2)}`;
    users[name] = { userId, ...kp };
    server.state.publicKeys.set(userId, crypto.toBase64(kp.publicKey));
  }
});

async function as<T>(name: string, fn: () => Promise<T>): Promise<T> {
  server.state.as = users[name].userId;
  return fn();
}

async function deviceFor(name: string): Promise<ForwardSecretDevice> {
  return as(name, async () => (await ForwardSecretDevice.start(users[name]))!);
}

/** Another device of the same account, as if on a second browser (just its public prekeys, on the server). */
async function registerSecondDevice(name: string): Promise<void> {
  const signed = generateSignedPreKey(users[name], 1);
  const pq = generatePqPreKey(users[name], 2);
  await as(name, () =>
    server.api.registerE2eDevice({
      signedPreKey: { id: 1, publicKey: crypto.toBase64(signed.keyPair.publicKey), signature: crypto.toBase64(signed.signature) },
      pqPreKey: { id: 2, publicKey: crypto.toBase64(pq.publicKey), signature: crypto.toBase64(pq.signature) },
      oneTimePreKeys: [],
    })
  );
}

function conversation(...names: string[]): string {
  const id = `c0000000-0000-4000-8000-${String(++server.state.seq).padStart(12, "0")}`;
  server.state.conversations.set(id, names.map((n) => users[n].userId));
  return id;
}

async function send(name: string, device: ForwardSecretDevice, conversationId: string, text: string): Promise<Api.MessageDto> {
  return as(name, async () => {
    const prepared = (await device.prepare(conversationId, text))!;
    const dto = await server.api.sendMessage(conversationId, crypto.toBase64(prepared.ciphertext), crypto.toBase64(prepared.nonce), undefined, prepared.envelope);
    await device.rememberSent(dto.id, text);
    return dto;
  });
}

async function read(name: string, device: ForwardSecretDevice, conversationId: string): Promise<Map<string, string | null>> {
  return as(name, async () => device.read(conversationId, await server.api.listMessages(conversationId, { deviceId: device.deviceId })));
}

describe("forward-secret devices", () => {
  it("registers a device with signed prekeys and 100 one-time prekeys", async () => {
    const alice = await deviceFor("alice");
    const registered = server.state.devices.get(alice.deviceId)!;
    expect(registered.userId).toBe(users.alice.userId);
    expect(registered.oneTime).toHaveLength(100);
    expect(crypto.fromBase64(registered.preKeys.pqPreKey.publicKey)).toHaveLength(1184);
  });

  it("carries a conversation both ways; the server never sees a message key and drops payloads once read", async () => {
    const alice = await deviceFor("alice");
    const bob = await deviceFor("bob");
    const c = conversation("alice", "bob");

    const m1 = await send("alice", alice, c, "hi bob");
    const stored = server.state.messages[0];
    expect(stored.protocol).toBe(2);
    expect(Buffer.from(stored.ciphertext, "base64").toString()).not.toContain("hi bob");
    expect(stored.payloads.size).toBe(1);

    expect((await read("bob", bob, c)).get(m1.id)).toBe("hi bob");
    expect(stored.payloads.size).toBe(0);

    const m2 = await send("bob", bob, c, "hi alice");
    const aliceView = await read("alice", alice, c);
    expect(aliceView.get(m2.id)).toBe("hi alice");
    // Her own message, from her own record of it.
    expect(aliceView.get(m1.id)).toBe("hi bob");

    // Reading again comes from the local store, not the (already used) payloads.
    expect((await read("bob", bob, c)).get(m1.id)).toBe("hi bob");
  });

  it("only uses each one-time prekey once, and deletes its private half after use", async () => {
    const alice = await deviceFor("alice");
    const bob = await deviceFor("bob");
    const c = conversation("alice", "bob");
    await send("alice", alice, c, "one");
    expect(server.state.devices.get(bob.deviceId)!.oneTime).toHaveLength(99);
    await read("bob", bob, c);
    // Bob's device kept 100 private one-time keys; one is now gone for good.
    const restarted = await deviceFor("bob");
    expect(restarted.deviceId).toBe(bob.deviceId);
    const device = await as("bob", () => (restarted as unknown as { store: { get: (k: string) => Promise<{ oneTimePreKeys: unknown[] }> } }).store.get("device"));
    expect(device.oneTimePreKeys).toHaveLength(99);
  });

  it("decrypts messages read out of order, across conversations sharing one session", async () => {
    const alice = await deviceFor("alice");
    const bob = await deviceFor("bob");
    const dm = conversation("alice", "bob");
    const group = conversation("alice", "bob", "carol");
    await deviceFor("carol");

    const a = await send("alice", alice, dm, "first, in the DM");
    const b = await send("alice", alice, group, "second, in the group");
    const c = await send("alice", alice, dm, "third, in the DM");
    // Bob opens the group before the DM.
    expect((await read("bob", bob, group)).get(b.id)).toBe("second, in the group");
    const dmView = await read("bob", bob, dm);
    expect(dmView.get(a.id)).toBe("first, in the DM");
    expect(dmView.get(c.id)).toBe("third, in the DM");
  });

  it("keeps sessions and history across a restart (state lives in the encrypted local store)", async () => {
    const alice = await deviceFor("alice");
    let bob = await deviceFor("bob");
    const c = conversation("alice", "bob");
    const m1 = await send("alice", alice, c, "before restart");
    await read("bob", bob, c);

    bob = await deviceFor("bob");
    const m2 = await send("alice", alice, c, "after restart");
    const view = await read("bob", bob, c);
    expect(view.get(m1.id)).toBe("before restart");
    expect(view.get(m2.id)).toBe("after restart");
  });

  it("drops this browser's decrypted copy once a message is deleted (by its sender or by expiry)", async () => {
    const alice = await deviceFor("alice");
    const bob = await deviceFor("bob");
    const c = conversation("alice", "bob");
    const m1 = await send("alice", alice, c, "soon gone");
    expect((await read("bob", bob, c)).get(m1.id)).toBe("soon gone");
    const store = (bob as unknown as { store: { get: (k: string) => Promise<unknown> } }).store;
    expect(await as("bob", () => store.get(`msg:${m1.id}`))).toBeTruthy();

    // The server clears it (deleted for everyone, or its disappearing timer ran out).
    const stored = server.state.messages.find((m) => m.id === m1.id)!;
    stored.deletedAt = new Date().toISOString();
    stored.ciphertext = "";
    stored.nonce = "";
    expect((await read("bob", bob, c)).has(m1.id)).toBe(false);
    expect(await as("bob", () => store.get(`msg:${m1.id}`))).toBeUndefined();
  });

  it("sends to every device of every member, including the sender's other devices", async () => {
    const alice = await deviceFor("alice");
    await deviceFor("bob");
    await deviceFor("carol");
    const group = conversation("alice", "bob", "carol");
    await send("alice", alice, group, "to the group");
    expect(server.state.messages[0].payloads.size).toBe(2);
  });

  it("leaves out messages sent before this device existed (new devices start empty)", async () => {
    const alice = await deviceFor("alice");
    await deviceFor("bob");
    const c = conversation("alice", "bob");
    const early = await send("alice", alice, c, "before carol");
    server.state.conversations.set(c, [...server.state.conversations.get(c)!, users.carol.userId]);
    const carol = await deviceFor("carol");
    const later = await send("alice", alice, c, "after carol");
    const view = await read("carol", carol, c);
    expect(view.has(early.id)).toBe(false);
    expect(view.get(later.id)).toBe("after carol");
  });

  it("refuses a device whose server-supplied identity key isn't the account's", async () => {
    const alice = await deviceFor("alice");
    await deviceFor("bob");
    const c = conversation("alice", "bob");
    // The server swaps in a key of its own for Bob.
    const forged = crypto.generateKeyPair();
    const realGetUser = server.api.getUser;
    const bundles = server.api.fetchPreKeyBundles;
    server.api.fetchPreKeyBundles = async (id, targets) =>
      (await bundles(id, targets)).map((b) => ({ ...b, identityKey: crypto.toBase64(forged.publicKey) }));
    try {
      await expect(as("alice", () => alice.prepare(c, "hello"))).rejects.toThrow();
    } finally {
      server.api.fetchPreKeyBundles = bundles;
      server.api.getUser = realGetUser;
    }
  });

  it("marks a tampered message undecryptable without breaking the session", async () => {
    const alice = await deviceFor("alice");
    const bob = await deviceFor("bob");
    const c = conversation("alice", "bob");
    const m1 = await send("alice", alice, c, "one");
    await read("bob", bob, c);
    const m2 = await send("alice", alice, c, "two");
    server.state.messages[1].ciphertext = crypto.toBase64(crypto.generateConversationKey());
    const m3 = await send("alice", alice, c, "three");
    const view = await read("bob", bob, c);
    expect(view.get(m1.id)).toBe("one");
    expect(view.get(m2.id)).toBeNull();
    expect(view.get(m3.id)).toBe("three");
  });
});

describe("sendEncryptedMessage", () => {
  it("uses protocol 2 when everyone has a device, and the conversation key when someone doesn't", async () => {
    await deviceFor("bob");
    const c = conversation("alice", "bob", "carol");
    const conversationKey = crypto.generateConversationKey();
    beginForwardSecrecy(users.alice);
    try {
      server.state.as = users.alice.userId;
      await currentForwardSecretDevice();

      // Carol has no device yet (say she only uses the Android app).
      const v1 = await sendEncryptedMessage(c, conversationKey, "readable everywhere");
      expect(v1.dto.protocol).toBe(1);
      expect(crypto.decryptWithConversationKey(v1.ciphertext, v1.nonce, conversationKey)).toBe("readable everywhere");

      await deviceFor("carol");
      server.state.as = users.alice.userId;
      const v2 = await sendEncryptedMessage(c, conversationKey, "forward secret");
      expect(v2.dto.protocol).toBe(2);
      expect(() => crypto.decryptWithConversationKey(v2.ciphertext, v2.nonce, conversationKey)).toThrow();
    } finally {
      stopForwardSecrecy();
    }
  });

  it("refetches the device list when a device was added meanwhile", async () => {
    await deviceFor("bob");
    const c = conversation("alice", "bob");
    beginForwardSecrecy(users.alice);
    try {
      server.state.as = users.alice.userId;
      const device = (await currentForwardSecretDevice())!;
      // Bob adds a second device between Alice listing devices and sending.
      const prepare = device.prepare.bind(device);
      let first = true;
      device.prepare = async (...args) => {
        const out = await prepare(...args);
        if (first) {
          first = false;
          await registerSecondDevice("bob");
          server.state.as = users.alice.userId;
        }
        return out;
      };
      const sent = await sendEncryptedMessage(c, crypto.generateConversationKey(), "hello");
      expect(sent.dto.protocol).toBe(2);
      expect(server.state.messages.at(-1)!.payloads.size).toBe(2);
    } finally {
      stopForwardSecrecy();
    }
  });

  it("uses the conversation key while the server has protocol 2 off", async () => {
    server.state.enabled = false;
    const c = conversation("alice", "bob");
    beginForwardSecrecy(users.alice);
    try {
      server.state.as = users.alice.userId;
      expect(await currentForwardSecretDevice()).toBeNull();
      expect((await sendEncryptedMessage(c, crypto.generateConversationKey(), "hi")).dto.protocol).toBe(1);
    } finally {
      stopForwardSecrecy();
    }
  });
});
