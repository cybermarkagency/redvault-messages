// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// This browser's protocol-3 device and what it does with the server
// (docs/multi-device-threat-model.md). A preview: off unless the server has
// E2E_V3_ENABLED and this build includes the engine. Protocol 1 is untouched.
import * as api from "../api.js";
import { browserDeviceStore, type DeviceStore } from "./deviceStore.js";
import { runEngine, type EngineResponse } from "./engine.js";

export class EngineError extends Error {
  constructor(
    readonly code: string,
    message: string
  ) {
    super(message);
  }
}

function toBase64(bytes: Uint8Array): string {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary);
}

/** This browser's device for one account. Every engine call saves the new state before returning. */
export class V3Device {
  private constructor(
    readonly userId: string,
    readonly deviceId: string,
    private readonly storageKey: string,
    private state: string,
    private readonly store: DeviceStore
  ) {}

  /**
   * One open instance per account and store: every screen that opens the device shares it, so none
   * works from a stale copy of the state (or saves over another's newer one).
   */
  private static readonly shared = new WeakMap<DeviceStore, Map<string, V3Device>>();

  private static remember(device: V3Device): V3Device {
    const forStore = V3Device.shared.get(device.store) ?? new Map<string, V3Device>();
    forStore.set(device.userId, device);
    V3Device.shared.set(device.store, forStore);
    return device;
  }

  /** The device this browser keeps for `userId`, if any. */
  static async open(userId: string, store: DeviceStore = browserDeviceStore()): Promise<V3Device | null> {
    const stored = await store.load(userId);
    const open = V3Device.shared.get(store)?.get(userId);
    // Still the device this store holds (it may have been wiped or replaced from elsewhere).
    if (open && stored?.deviceId === open.deviceId) return open;
    if (open) V3Device.shared.get(store)?.delete(userId);
    return stored?.state ? V3Device.remember(new V3Device(userId, stored.deviceId, stored.storageKey, stored.state, store)) : null;
  }

  /** Makes the engine state for a new device record (createDevice or createAccount) and keeps it. */
  static async create(
    userId: string,
    op: "createDevice" | "createAccount",
    fields: Record<string, unknown>,
    store: DeviceStore = browserDeviceStore()
  ): Promise<{ device: V3Device; response: EngineResponse }> {
    const stored = await store.create(userId);
    const response = await runEngine({ op, userId, deviceId: stored.deviceId, pickleKey: stored.storageKey, ...fields });
    if (!response.ok || !response.state) {
      await store.wipe(userId);
      throw new EngineError(response.error ?? "engine_error", response.message ?? "the engine refused");
    }
    await store.saveState(userId, stored.deviceId, response.state);
    return { device: V3Device.remember(new V3Device(userId, stored.deviceId, stored.storageKey, response.state, store)), response };
  }

  /** One engine call at a time: each reads the state the last one saved (a send and a receive can overlap in the app). */
  private queue: Promise<unknown> = Promise.resolve();

  op(op: string, fields: Record<string, unknown> = {}): Promise<EngineResponse> {
    const run = async (): Promise<EngineResponse> => {
      const response = await runEngine({ op, state: this.state, pickleKey: this.storageKey, ...fields });
      if (response.ok && response.state) {
        // Saved before anything this call produced can be sent: a crash never reuses a message key.
        await this.store.saveState(this.userId, this.deviceId, response.state);
        this.state = response.state;
      }
      return response;
    };
    const result = this.queue.then(run, run);
    this.queue = result.catch(() => {});
    return result;
  }

  async must(op: string, fields: Record<string, unknown> = {}): Promise<EngineResponse> {
    const response = await this.op(op, fields);
    if (!response.ok) throw new EngineError(response.error ?? "engine_error", response.message ?? op);
    return response;
  }

  /** An AES-GCM key for this device's local chat history, derived from the storage key (docs/multi-device-threat-model.md 9.7). */
  async historyKey(): Promise<CryptoKey> {
    const raw = Uint8Array.from(atob(this.storageKey), (c) => c.charCodeAt(0));
    const base = await crypto.subtle.importKey("raw", raw, "HKDF", false, ["deriveKey"]);
    raw.fill(0);
    return crypto.subtle.deriveKey(
      { name: "HKDF", hash: "SHA-256", salt: new Uint8Array(0), info: new TextEncoder().encode("redvault-v3-history") },
      base,
      { name: "AES-GCM", length: 256 },
      false,
      ["encrypt", "decrypt"]
    );
  }

  async identity(): Promise<api.V3DeviceIdentity> {
    return (await this.must("identity")).identity as api.V3DeviceIdentity;
  }

  /** Publishes this device's public keys: identity, signed one-time keys, the signed fallback key. */
  async publishKeys(count = 20): Promise<void> {
    const { upload } = (await this.must("prekeys", { count })) as unknown as {
      upload: { identity: api.V3DeviceIdentity; oneTimeKeys: api.V3SignedKey[]; fallbackKey: api.V3SignedKey | null };
    };
    await api.registerV3Device(upload);
  }

  /** Forgets this device in this browser and in the server's directory. */
  async forget(): Promise<void> {
    await api.removeV3Device(this.deviceId).catch(() => {});
    await this.store.wipe(this.userId);
    V3Device.shared.get(this.store)?.delete(this.userId);
  }
}

/**
 * Moves an existing account to per-device identities from this browser: a new
 * account key, certified by the account's legacy signing key (migration 030)
 * so contacts who pinned that key accept it. The legacy key stays where it is
 * (protocol 1 keeps working). If any step fails, everything this did is undone.
 */
export async function setUpFirstDevice(userId: string, legacyPrivateKey: Uint8Array, store?: DeviceStore): Promise<V3Device> {
  const { device, response } = await V3Device.create(userId, "createAccount", { legacyAccountKey: toBase64(legacyPrivateKey) }, store);
  try {
    await device.publishKeys();
    await api.putV3DeviceList(response.deviceList as api.V3SignedDeviceList);
    return device;
  } catch (err) {
    await device.forget();
    throw err;
  }
}

/** A browser joining an account that already has trusted devices: its own keys, waiting to be linked. */
export async function startNewDevice(userId: string, store?: DeviceStore): Promise<V3Device> {
  const { device } = await V3Device.create(userId, "createDevice", {}, store);
  await device.publishKeys();
  return device;
}

/**
 * Puts a list this device signed. If another trusted device changed the list
 * first, adopts that list (it's signed by the same account key) and redoes the
 * change once on top of it.
 */
export async function publishList(
  device: V3Device,
  change: () => Promise<EngineResponse>,
  rotationProof: string | null = null
): Promise<EngineResponse> {
  let made = await change();
  try {
    await api.putV3DeviceList(made.deviceList as api.V3SignedDeviceList, rotationProof);
  } catch (err) {
    if (!(err instanceof api.ApiError) || err.code !== "stale_device_list") throw err;
    const current = (await api.getV3DeviceList(device.userId)).deviceList;
    await device.must("verifyDeviceList", { deviceList: current });
    made = await change();
    await api.putV3DeviceList(made.deviceList as api.V3SignedDeviceList, rotationProof);
  }
  return made;
}

/** Removes another device of this account from the signed list; contacts are told by the next list they check. */
export async function revokeDevice(device: V3Device, deviceId: string): Promise<void> {
  await publishList(device, () => device.must("revokeDevice", { deviceId }));
  await api.removeV3Device(deviceId).catch(() => {});
}

// --- Recovery ---

/** The recovery key as people see it: 64 hex digits in groups of four. */
export function formatRecoveryKey(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0"))
    .join("")
    .toUpperCase()
    .match(/.{4}/g)!
    .join("-");
}

export function parseRecoveryKey(text: string): Uint8Array | null {
  const digits = text.replace(/[^0-9a-f]/gi, "");
  if (digits.length !== 64) return null;
  return Uint8Array.from(digits.match(/.{2}/g)!, (h) => parseInt(h, 16));
}

/**
 * A new recovery key and the account key (and legacy key) encrypted under it,
 * stored on the server, which never sees the key. Returns the key, to show
 * once; nothing in this browser keeps it.
 */
export async function createRecoveryBackup(device: V3Device): Promise<string> {
  const key = crypto.getRandomValues(new Uint8Array(32));
  const made = await device.must("exportAccountBackup", { recoveryKey: toBase64(key) });
  await api.putV3RecoveryBackup(made.backup as string, made.signature as string);
  const shown = formatRecoveryKey(key);
  key.fill(0);
  return shown;
}

/** With no other device left: a new device in this browser, restored from the recovery backup. */
export async function restoreWithRecoveryKey(userId: string, recoveryKeyText: string, store?: DeviceStore): Promise<V3Device> {
  const key = parseRecoveryKey(recoveryKeyText);
  if (!key) throw new EngineError("bad_recovery_key", "A recovery key has 64 letters and digits.");
  const { backup } = await api.getV3RecoveryBackup();
  const current = (await api.getV3DeviceList(userId)).deviceList;
  const device = await startNewDevice(userId, store);
  try {
    const restored = await device.must("restoreAccountBackup", { backup, recoveryKey: toBase64(key), deviceList: current });
    await api.putV3DeviceList(restored.deviceList as api.V3SignedDeviceList);
    return device;
  } catch (err) {
    await device.forget();
    throw err;
  } finally {
    key.fill(0);
  }
}

// --- One-to-one messages, with copies for this account's other devices ---

/** The devices of `userId` from a list this device verified (contacts: pinned key; own: same account key). */
export async function verifiedDevices(device: V3Device, userId: string, conversationId: string | null, legacySigningKey?: string | null) {
  const view = await api.getV3DeviceList(userId, conversationId);
  if (!view.deviceList) return { devices: [] as api.V3DeviceIdentity[], response: null, pendingReset: view.pendingReset };
  const response = await device.op("verifyDeviceList", { deviceList: view.deviceList, ...(legacySigningKey ? { legacySigningKey } : {}) });
  if (!response.ok) throw new EngineError(response.error ?? "bad_device_list", response.message ?? "the device list doesn't verify");
  return { devices: response.devices as api.V3DeviceIdentity[], response, pendingReset: view.pendingReset };
}

/**
 * The safety number over this account's and the contact's account keys
 * (Signal's numeric fingerprint construction, crypto-core/src/identity.rs), or
 * null while the contact has no device keys. Comparing it proves both apps see
 * the same keys; it says nothing about who the person is.
 */
export async function deviceKeySafetyNumber(device: V3Device, contactUserId: string, conversationId: string | null = null): Promise<string[] | null> {
  const { devices } = await verifiedDevices(device, contactUserId, conversationId);
  if (!devices.length) return null;
  const fingerprint = await device.must("fingerprint", { userId: contactUserId });
  return fingerprint.safetyNumber as string[];
}

/** A contact's account key changed since this device pinned it: the new key's safety number, to compare before trusting it. */
export interface KeyChange {
  accountKey: string;
  safetyNumber: string[];
}

/** Null when the contact's key is unchanged (or unknown); otherwise what the person must compare. */
export async function pendingKeyChange(device: V3Device, contactUserId: string, conversationId: string | null = null): Promise<KeyChange | null> {
  const view = await api.getV3DeviceList(contactUserId, conversationId);
  if (!view.deviceList) return null;
  const response = await device.op("verifyDeviceList", { deviceList: view.deviceList });
  if (response.ok || response.error !== "account_key_changed") return null;
  const accountKey = view.deviceList.accountKey;
  const fingerprint = await device.must("fingerprint", { userId: contactUserId, accountKey });
  return { accountKey, safetyNumber: fingerprint.safetyNumber as string[] };
}

/** The person compared the numbers and accepts the contact's new key: pins it, then reads their list again. */
export async function trustNewKeys(device: V3Device, contactUserId: string, accountKey: string, conversationId: string | null = null): Promise<void> {
  await device.must("trustAccountKey", { userId: contactUserId, accountKey, verified: false });
  const { devices } = await verifiedDevices(device, contactUserId, conversationId);
  if (!devices.length) throw new EngineError("no_devices", "The contact has no device keys.");
}

/** What to tell the person when a v3 send or read stops because of the contact's keys. Null for other errors. */
export function keyProblemMessage(code: string): string | null {
  switch (code) {
    case "account_key_changed":
      return "Not sent: this contact's device keys changed. Compare the new safety number (Verify) before trusting them.";
    case "rollback":
      return "Not sent: this contact's device list is older than one already seen, which can mean someone is replaying old keys. Check the safety number (Verify).";
    case "equivocation":
      return "Not sent: the server showed two different device lists for this contact. Check the safety number (Verify) and try again later.";
    case "uncertified_account_key":
      return "Not sent: this contact's new device keys aren't vouched for by the key you already knew. Compare the safety number (Verify) before trusting them.";
    case "bad_device_list":
      return "Not sent: this contact's device list doesn't verify. Try again later; if it keeps happening, check the safety number (Verify).";
    default:
      return null;
  }
}

/**
 * Sends `text` to every device of the contact and every other device of this
 * account, claiming one-time keys only where there's no session yet.
 */
export async function sendText(device: V3Device, conversationId: string, contactUserId: string, text: string): Promise<number> {
  const theirs = (await verifiedDevices(device, contactUserId, conversationId)).devices;
  // A contact with no listed device can't read this: send nothing, not even the copies for our own devices.
  if (theirs.length === 0) return 0;
  const mine = (await verifiedDevices(device, device.userId, null)).devices.filter((d) => d.deviceId !== device.deviceId);
  const recipients = [...theirs, ...mine];
  const first = await device.must("encryptMany", { plaintext: text, recipients: recipients.map((identity) => ({ identity })) });
  let results = first.results as { userId: string; deviceId: string; message?: unknown; error?: string }[];
  const needKeys = recipients.filter((_, i) => results[i].error === "no_session");
  if (needKeys.length) {
    const claimed = await api.claimV3Keys(conversationId, needKeys.map((d) => ({ userId: d.userId, deviceId: d.deviceId })));
    const retry = await device.must("encryptMany", {
      plaintext: text,
      recipients: needKeys.map((identity, i) => ({ identity, oneTimeKey: claimed[i].oneTimeKey ?? claimed[i].fallbackKey })),
    });
    let j = 0;
    const retried = retry.results as typeof results;
    results = results.map((r) => (r.error === "no_session" ? retried[j++] : r));
  }
  const delivered = results.filter((r) => !r.error && r.message);
  if (delivered.length) {
    await api.sendV3Messages(
      device.deviceId,
      conversationId,
      delivered.map((r) => ({ userId: r.userId, deviceId: r.deviceId, body: JSON.stringify(r.message) }))
    );
  }
  return delivered.length;
}

export interface Received {
  id: string;
  senderUserId: string;
  senderDeviceId: string;
  conversationId: string | null;
  kind: string;
  text: string | null;
  error: string | null;
  /** Group messages: true. The sender is then the group session's sender as the engine authenticated it, not as the server named it. */
  group?: boolean;
}

/** Errors about the sender's list, not the message: the item waits until the person accepts the change. */
const HOLD = new Set(["account_key_changed", "rollback", "equivocation", "uncertified_account_key", "bad_device_list"]);
/** A group message whose room key hasn't arrived waits this long (the key may come from another device later). */
const ROOM_KEY_WAIT_MS = 2 * 24 * 60 * 60 * 1000;

// --- Groups (Megolm); docs/multi-device-threat-model.md 9.5 ---

export interface GroupMembers {
  /** Every verified, listed device of every active member that has a list, this device included. */
  devices: api.V3DeviceIdentity[];
  /** Members with no device list (they can't receive v3 messages). */
  withoutDevices: string[];
}

/** Reads and verifies every member's list, then tells the engine the group's member devices (it rotates the sending session if one left). */
export async function syncGroupMembers(device: V3Device, conversationId: string): Promise<GroupMembers> {
  const { members } = await api.getV3ConversationLists(conversationId);
  const devices: api.V3DeviceIdentity[] = [];
  const withoutDevices: string[] = [];
  for (const member of members) {
    if (!member.deviceList) {
      withoutDevices.push(member.userId);
      continue;
    }
    const response = await device.op("verifyDeviceList", { deviceList: member.deviceList });
    if (!response.ok) throw new EngineError(response.error ?? "bad_device_list", response.message ?? "a member's device list doesn't verify");
    devices.push(...(response.devices as api.V3DeviceIdentity[]));
  }
  await device.must("groupSetMembers", { groupId: conversationId, members: devices });
  return { devices, withoutDevices };
}

/**
 * Sends `plaintext` to a group: shares this device's sending-session key with
 * members that don't have it yet (Olm, one message each), then posts the
 * Megolm message once for all member devices.
 */
export async function sendGroup(
  device: V3Device,
  conversationId: string,
  plaintext: string
): Promise<{ delivered: number; withoutDevices: string[] }> {
  const { devices, withoutDevices } = await syncGroupMembers(device, conversationId);
  const session = (await device.must("groupSession", { groupId: conversationId })) as unknown as { unshared: api.V3DeviceIdentity[] };
  const targets = session.unshared.filter((d) => d.deviceId !== device.deviceId);
  if (targets.length) {
    const keyMessages: { userId: string; deviceId: string; body: string }[] = [];
    const needKeys: api.V3DeviceIdentity[] = [];
    for (const recipient of targets) {
      const response = await device.op("shareRoomKey", { groupId: conversationId, recipient });
      if (response.ok) keyMessages.push({ userId: recipient.userId, deviceId: recipient.deviceId, body: JSON.stringify(response.message) });
      else if (response.error === "no_session") needKeys.push(recipient);
      else throw new EngineError(response.error ?? "engine_error", response.message ?? "shareRoomKey");
    }
    if (needKeys.length) {
      const claimed = await api.claimV3Keys(conversationId, needKeys.map((d) => ({ userId: d.userId, deviceId: d.deviceId })));
      for (let i = 0; i < needKeys.length; i++) {
        const response = await device.must("shareRoomKey", {
          groupId: conversationId,
          recipient: needKeys[i],
          oneTimeKey: claimed[i].oneTimeKey ?? claimed[i].fallbackKey,
        });
        keyMessages.push({ userId: needKeys[i].userId, deviceId: needKeys[i].deviceId, body: JSON.stringify(response.message) });
      }
    }
    await api.sendV3Messages(device.deviceId, conversationId, keyMessages);
  }
  const sealed = (await device.must("groupEncrypt", { groupId: conversationId, plaintext })) as unknown as { sessionId: string; ciphertext: string };
  const recipients = devices.filter((d) => d.deviceId !== device.deviceId).map((d) => ({ userId: d.userId, deviceId: d.deviceId }));
  if (recipients.length) {
    const body = JSON.stringify({ v: 1, kind: "group", groupId: conversationId, sessionId: sealed.sessionId, ciphertext: sealed.ciphertext });
    await api.sendV3GroupMessage(device.deviceId, conversationId, body, recipients);
  }
  return { delivered: recipients.length, withoutDevices };
}

export interface ReceiveOptions {
  /**
   * Called with everything this run produced BEFORE any item is acknowledged.
   * If it throws, nothing is acknowledged: the server still has the items.
   */
  persist?: (items: Received[]) => Promise<void>;
}

/**
 * Reads this device's mailbox. Each sender's list is verified first. Items
 * are acknowledged once handled (and persisted by the caller), even one that
 * can't be decrypted (a duplicate, or not for this device), so one bad item
 * never blocks the rest; only items whose sender's identity changed, or group
 * messages still waiting for their key, are kept for later.
 */
export async function receive(device: V3Device, options: ReceiveOptions = {}): Promise<Received[]> {
  const items = await api.getV3Inbox(device.deviceId);
  const out: Received[] = [];
  const handled: string[] = [];
  const lists = new Map<string, { devices: api.V3DeviceIdentity[]; error: string | null }>();
  const syncedGroups = new Set<string>();
  const now = Date.now();
  for (const item of items) {
    let body: Record<string, unknown> | null = null;
    try {
      body = JSON.parse(item.body) as Record<string, unknown>;
    } catch {
      body = null;
    }
    let response: EngineResponse | null = null;
    let known: { devices: api.V3DeviceIdentity[]; error: string | null } | undefined;
    let held = false;
    const isGroup = body?.kind === "group";
    if (body && isGroup) {
      if (typeof body.sessionId === "string" && typeof body.ciphertext === "string" && body.groupId === item.conversationId) {
        try {
          response = await device.op("groupDecrypt", { groupId: body.groupId, sessionId: body.sessionId, ciphertext: body.ciphertext });
        } catch {
          response = { ok: false, error: "decrypt_failed" };
        }
        // The room key may still be on its way from another device.
        if (!response.ok && response.error === "unknown_session" && now - Date.parse(`${item.createdOn}T00:00:00Z`) < ROOM_KEY_WAIT_MS) held = true;
      } else {
        response = { ok: false, error: "malformed_message" };
      }
    } else if (body) {
      const key = `${item.senderUserId}/${item.conversationId ?? ""}`;
      known = lists.get(key);
      if (!known) {
        try {
          known = { devices: (await verifiedDevices(device, item.senderUserId, item.conversationId)).devices, error: null };
        } catch (err) {
          const code = err instanceof EngineError ? err.code : "unknown_sender";
          // A device waiting for its link has no list of its own yet: the link message carries and checks one.
          if (code === "not_linked" && item.senderUserId === device.userId) {
            known = { devices: (await api.getV3DeviceList(device.userId)).deviceList?.devices ?? [], error: null };
          } else {
            known = { devices: [], error: code };
          }
        }
        lists.set(key, known);
      }
      const sender = known.devices.find((d) => d.deviceId === item.senderDeviceId);
      if (sender) {
        const attempt = async (): Promise<EngineResponse> => {
          try {
            return await device.op("decrypt", { sender, message: body });
          } catch {
            return { ok: false, error: "decrypt_failed" };
          }
        };
        response = await attempt();
        // A room key from a member this device hasn't learned about yet: read the group's lists once, then retry.
        const unknownGroup = response.error === "not_a_member" || response.error === "unknown_group";
        if (!response.ok && unknownGroup && item.conversationId && !syncedGroups.has(item.conversationId)) {
          syncedGroups.add(item.conversationId);
          try {
            await syncGroupMembers(device, item.conversationId);
            response = await attempt();
          } catch {
            // keep the first answer
          }
        }
      }
    } else {
      response = { ok: false, error: "malformed_message" };
    }
    const error = response ? (response.ok ? null : (response.error ?? "decrypt_failed")) : (known?.error ?? "unknown_sender");
    if (!held && (!error || !HOLD.has(error))) handled.push(item.id);
    const authenticated = response?.ok && response.sender ? (response.sender as { userId: string; deviceId: string }) : null;
    out.push({
      id: item.id,
      senderUserId: authenticated?.userId ?? item.senderUserId,
      senderDeviceId: authenticated?.deviceId ?? item.senderDeviceId,
      conversationId: item.conversationId,
      kind: (response?.kind as string) ?? "message",
      text: response?.ok ? ((response.plaintext as string | undefined) ?? null) : null,
      error,
      group: isGroup || undefined,
    });
  }
  if (options.persist) await options.persist(out);
  if (handled.length) await api.ackV3Messages(device.deviceId, handled);
  return out;
}
