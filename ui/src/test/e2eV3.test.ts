// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// @vitest-environment node
// Protocol 3 in the web app (lib/e2eV3): the real engine (crypto-core's Node
// build of the same WebAssembly the browser loads), two or three simulated
// browsers (separate IndexedDBs), and an in-memory server (fakeV3Api.ts).
// Skipped when crypto-core/build-web.sh hasn't been run.
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
const { NewDevicePairing, TrustedPairing } = await import("../lib/e2eV3/pairing.js");

// Tests run from ui/ (vitest's root).
const enginePath = resolve(process.cwd(), "../crypto-core/pkg/node/redvault_e2ee.js");
const available = existsSync(enginePath);
const fast = { intervalMs: 5, timeoutMs: 5000 };
const b64 = (bytes: Uint8Array) => Buffer.from(bytes).toString("base64");

const ALICE = "11111111-1111-4111-8111-111111111111";
const BOB = "22222222-2222-4222-8222-222222222222";
const CONVERSATION = "33333333-3333-4333-8333-333333333333";

describe.skipIf(!available)("protocol 3 in the web app", () => {
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
    server.tamper = null;
  });

  const legacyKey = crypto.getRandomValues(new Uint8Array(32));

  type Device = import("../lib/e2eV3/device.js").V3Device;

  async function pair(trusted: Device, fresh: Device) {
    const newSide = await NewDevicePairing.begin(fresh);
    const [newCodes, trustedSide] = await Promise.all([newSide.codes(fast), TrustedPairing.begin(trusted, newSide.enrollmentId, fast)]);
    return { newSide, newCodes, trustedSide: trustedSide.pairing, trustedCodes: trustedSide.codes };
  }

  it("migrates the first device with the legacy key, and undoes everything if a step fails", async () => {
    server.currentUser = ALICE;
    const store = new DeviceStore(new IDBFactory());
    server.failNextListUpload = true;
    await expect(v3.setUpFirstDevice(ALICE, legacyKey, store)).rejects.toThrow();
    expect(await store.load(ALICE)).toBeNull();
    expect(server.devices.size).toBe(0);

    const phone = await v3.setUpFirstDevice(ALICE, legacyKey, store);
    const list = server.lists.get(ALICE)!;
    expect(list.version).toBe(1);
    expect(list.legacyCertificate).toBeDefined();
    expect((await phone.must("legacyKeys")).legacyAccountKey).toBe(b64(legacyKey));
    // A restart: the device comes back from this browser's storage alone.
    const reopened = await v3.V3Device.open(ALICE, store);
    expect(reopened?.deviceId).toBe(phone.deviceId);
  });

  it("pairs a second browser by comparing codes; it gets the account and legacy keys", async () => {
    server.currentUser = ALICE;
    const phone = await v3.setUpFirstDevice(ALICE, legacyKey, new DeviceStore(new IDBFactory()));
    const laptop = await v3.startNewDevice(ALICE, new DeviceStore(new IDBFactory()));
    const { newSide, newCodes, trustedSide, trustedCodes } = await pair(phone, laptop);
    expect(newCodes).toEqual(trustedCodes);
    const [linked] = await Promise.all([newSide.confirm(fast), trustedSide.confirm(fast)]);
    expect(linked.devices.map((d) => d.deviceId).sort()).toEqual([phone.deviceId, laptop.deviceId].sort());
    expect(server.lists.get(ALICE)!.version).toBe(2);
    expect((await laptop.must("legacyKeys")).legacyAccountKey).toBe(b64(legacyKey));
  });

  it("a server that swaps the pairing keys gets different codes on each screen, and nothing is approved", async () => {
    server.currentUser = ALICE;
    const phone = await v3.setUpFirstDevice(ALICE, legacyKey, new DeviceStore(new IDBFactory()));
    const laptop = await v3.startNewDevice(ALICE, new DeviceStore(new IDBFactory()));
    // The relay answers the new device with its own key instead of the phone's.
    const own = await engine.runEngine({ op: "sasStart", pairingId: "x", role: "trusted" });
    server.tamper = (body) => {
      const message = JSON.parse(body);
      return message.type === "key" ? JSON.stringify({ ...message, publicKey: own.publicKey }) : body;
    };
    const { newSide, newCodes, trustedSide, trustedCodes } = await pair(phone, laptop);
    expect(newCodes).not.toEqual(trustedCodes);
    // The person sees the mismatch and stops on both screens.
    await trustedSide.reject();
    await newSide.cancel();
    expect(server.lists.get(ALICE)!.devices).toHaveLength(1);
  });

  it("sends to every device of the contact and copies to the sender's other devices; a revoked device gets nothing", async () => {
    server.currentUser = ALICE;
    const alicePhone = await v3.setUpFirstDevice(ALICE, legacyKey, new DeviceStore(new IDBFactory()));
    const aliceLaptop = await v3.startNewDevice(ALICE, new DeviceStore(new IDBFactory()));
    const { newSide, trustedSide } = await pair(alicePhone, aliceLaptop);
    await Promise.all([newSide.confirm(fast), trustedSide.confirm(fast)]);
    server.currentUser = BOB;
    const bobPhone = await v3.setUpFirstDevice(BOB, crypto.getRandomValues(new Uint8Array(32)), new DeviceStore(new IDBFactory()));

    server.currentUser = ALICE;
    expect(await v3.sendText(alicePhone, CONVERSATION, BOB, "hello bob 👋")).toBe(2);
    server.currentUser = BOB;
    expect((await v3.receive(bobPhone)).map((r) => r.text)).toEqual(["hello bob 👋"]);
    server.currentUser = ALICE;
    expect((await v3.receive(aliceLaptop)).map((r) => r.text)).toEqual(["hello bob 👋"]);

    server.currentUser = BOB;
    expect(await v3.sendText(bobPhone, CONVERSATION, ALICE, "hi both")).toBe(2);
    server.currentUser = ALICE;
    expect((await v3.receive(alicePhone)).map((r) => r.text)).toEqual(["hi both"]);
    expect((await v3.receive(aliceLaptop)).map((r) => r.text)).toEqual(["hi both"]);

    // Both apps compute the same safety number over the two account keys.
    server.currentUser = BOB;
    const bobSees = await v3.deviceKeySafetyNumber(bobPhone, ALICE, CONVERSATION);
    server.currentUser = ALICE;
    const aliceSees = await v3.deviceKeySafetyNumber(alicePhone, BOB, CONVERSATION);
    expect(bobSees).toHaveLength(12);
    expect(aliceSees).toEqual(bobSees);

    // The phone removes the laptop: bob's app learns it from the next list it checks.
    await v3.revokeDevice(alicePhone, aliceLaptop.deviceId);
    server.currentUser = BOB;
    expect(await v3.sendText(bobPhone, CONVERSATION, ALICE, "only the phone now")).toBe(1);
    server.currentUser = ALICE;
    expect(await v3.receive(aliceLaptop)).toEqual([]);
    expect((await v3.receive(alicePhone)).map((r) => r.text)).toEqual(["only the phone now"]);
  });

  it("restores the account on a new browser from the recovery key, with the legacy key", async () => {
    server.currentUser = ALICE;
    const phone = await v3.setUpFirstDevice(ALICE, legacyKey, new DeviceStore(new IDBFactory()));
    const shown = await v3.createRecoveryBackup(phone);
    expect(shown).toMatch(/^([0-9A-F]{4}-){15}[0-9A-F]{4}$/);
    await expect(v3.restoreWithRecoveryKey(ALICE, "0000-".repeat(15) + "0000", new DeviceStore(new IDBFactory()))).rejects.toThrow();
    const restored = await v3.restoreWithRecoveryKey(ALICE, shown.toLowerCase(), new DeviceStore(new IDBFactory()));
    expect(server.lists.get(ALICE)!.devices.map((d) => d.deviceId)).toContain(restored.deviceId);
    expect((await restored.must("legacyKeys")).legacyAccountKey).toBe(b64(legacyKey));
  });

  it("keeps the storage key wrapped by a non-extractable key, and sends nothing private to the server", async () => {
    server.currentUser = ALICE;
    const factory = new IDBFactory();
    const store = new DeviceStore(factory);
    const phone = await v3.setUpFirstDevice(ALICE, legacyKey, store);
    await v3.createRecoveryBackup(phone);
    const record = await new Promise<Record<string, unknown>>((resolve, reject) => {
      const open = factory.open("redvault-e2e-v3");
      open.onsuccess = () => {
        const get = open.result.transaction("devices").objectStore("devices").get(ALICE);
        get.onsuccess = () => resolve(get.result);
        get.onerror = () => reject(get.error);
      };
    });
    const wrappingKey = record.wrappingKey as CryptoKey;
    expect(wrappingKey.extractable).toBe(false);
    await expect(crypto.subtle.exportKey("raw", wrappingKey)).rejects.toThrow();
    const opened = await store.load(ALICE);
    expect(JSON.stringify(record)).not.toContain(opened!.storageKey);
    // The state on disk is the engine's ciphertext: the account key inside it isn't readable.
    expect(String(record.state)).not.toContain("account_key");
    const sent = server.sent.join("\n");
    for (const secret of [opened!.storageKey, b64(legacyKey), b64(legacyKey).replace(/=+$/, "")]) expect(sent).not.toContain(secret);
  });
});
