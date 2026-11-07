// This browser as a forward-secret device (docs/e2e-v2-protocol.md).
//
// Sending: every message body is encrypted once with a fresh random key,
// and that key goes to each other device in the conversation (the other
// participants' and our own) inside its own Double Ratchet message.
//
// Receiving: each payload is decrypted exactly once, the plaintext kept in
// the local store, and the server told to delete the payload. A message
// sent before this device existed has no payload for it and isn't shown.
//
// Everything that touches ratchet state runs under one lock per account,
// shared by all of this browser's tabs, so two tabs can't advance the same
// session at once.
import * as api from "../api.js";
import { ApiError } from "../api.js";
import { fromBase64, toBase64 } from "../e2eCrypto.js";
import { generateOneTimePreKeys, generatePqPreKey, generateSignedPreKey, type PreKeyBundle } from "./pqxdh.js";
import {
  AEAD_NONCE_BYTES,
  aeadDecrypt,
  aeadEncrypt,
  concat,
  equalBytes,
  randomBytes,
  utf8,
  uuidBytes,
  type X25519KeyPair,
} from "./primitives.js";
import { decryptFrom, encryptFor, startSession, type LocalDevice, type SessionRecord } from "./sessions.js";
import { openRecordStore, type RecordStore } from "./store.js";

const ONE_TIME_PREKEY_TARGET = 100;
const ONE_TIME_PREKEY_REFILL_BELOW = 25;
const SIGNED_PREKEY_MAX_AGE_MS = 7 * 24 * 60 * 60 * 1000;
/** How many signed prekeys (and ML-KEM prekeys) to keep after rotating, for sessions started with older ones. */
const PREKEYS_KEPT = 5;
const MAINTENANCE_INTERVAL_MS = 60 * 60 * 1000;

const PAYLOAD_VERSION = 1;
const BODY_AD_LABEL = "RedvaultMessage_v2";

export interface DeviceIdentity {
  userId: string;
  publicKey: Uint8Array;
  privateKey: Uint8Array;
}

/** What a stored message record holds: the plaintext, or that it couldn't be decrypted. */
type MessageRecord = { text: string } | { failed: true };

export interface PreparedMessage {
  ciphertext: Uint8Array;
  nonce: Uint8Array;
  envelope: api.ForwardSecretEnvelope;
}

function bodyAssociatedData(conversationId: string): Uint8Array {
  return concat(utf8(BODY_AD_LABEL), uuidBytes(conversationId));
}

function sessionKey(userId: string, deviceId: string): string {
  return `session:${userId}.${deviceId}`;
}

function isNotFound(err: unknown): boolean {
  return err instanceof ApiError && err.status === 404;
}

function bundleFromDto(dto: api.PreKeyBundleDto): PreKeyBundle {
  return {
    identityKey: fromBase64(dto.identityKey),
    signedPreKey: { id: dto.signedPreKey.id, publicKey: fromBase64(dto.signedPreKey.publicKey), signature: fromBase64(dto.signedPreKey.signature) },
    pqPreKey: { id: dto.pqPreKey.id, publicKey: fromBase64(dto.pqPreKey.publicKey), signature: fromBase64(dto.pqPreKey.signature) },
    oneTimePreKey: dto.oneTimePreKey ? { id: dto.oneTimePreKey.id, publicKey: fromBase64(dto.oneTimePreKey.publicKey) } : null,
  };
}

function preKeysInput(device: LocalDevice): api.E2ePreKeysInput {
  const [signed] = device.signedPreKeys;
  const [pq] = device.pqPreKeys;
  return {
    signedPreKey: { id: signed.id, publicKey: toBase64(signed.keyPair.publicKey), signature: toBase64(signed.signature) },
    pqPreKey: { id: pq.id, publicKey: toBase64(pq.publicKey), signature: toBase64(pq.signature) },
  };
}

// One lock per account: the Web Locks API across tabs when there is one,
// and a promise chain within this tab either way.
const localChains = new Map<string, Promise<unknown>>();

function withLock<T>(name: string, fn: () => Promise<T>): Promise<T> {
  const run = (): Promise<T> => {
    const locks = typeof navigator !== "undefined" ? navigator.locks : undefined;
    return locks ? (locks.request(name, fn) as Promise<T>) : fn();
  };
  const previous = localChains.get(name) ?? Promise.resolve();
  const next = previous.then(run, run);
  localChains.set(name, next.catch(() => undefined));
  return next;
}

export class ForwardSecretDevice {
  private readonly identities = new Map<string, Promise<Uint8Array>>();
  private lastMaintenance = 0;

  private constructor(
    private readonly identity: DeviceIdentity,
    private readonly store: RecordStore,
    readonly deviceId: string
  ) {}

  get userId(): string {
    return this.identity.userId;
  }

  private get keyPair(): X25519KeyPair {
    return { publicKey: this.identity.publicKey, privateKey: this.identity.privateKey };
  }

  private lock<T>(fn: () => Promise<T>): Promise<T> {
    return withLock(`redvault-e2e-v2:${this.identity.userId}`, fn);
  }

  /**
   * Loads this browser's device for the account, registering a new one when
   * there's none (or the server no longer has it). Null when the server has
   * forward-secret messaging off, or this browser can't keep device state.
   */
  static async start(identity: DeviceIdentity): Promise<ForwardSecretDevice | null> {
    const { enabled } = await api.getE2eConfig();
    if (!enabled) return null;
    const store = await openRecordStore(identity.userId, identity.privateKey);
    if (!store) return null;

    const deviceId = await withLock(`redvault-e2e-v2:${identity.userId}`, async () => {
      let device: LocalDevice | undefined;
      try {
        device = await store.get<LocalDevice>("device");
      } catch {
        // Written under a different account key (e.g. the account was
        // recreated): nothing in it is readable, so start over.
        device = undefined;
      }
      if (device) {
        try {
          await api.getE2eDevice(device.deviceId);
        } catch (err) {
          if (!isNotFound(err)) throw err;
          device = undefined; // Removed on the server: this browser needs a new device.
        }
      }
      if (!device) {
        await store.clear();
        device = await ForwardSecretDevice.register(identity);
        await store.putMany([["device", device]]);
      }
      return device.deviceId;
    });

    const active = new ForwardSecretDevice(identity, store, deviceId);
    await active.maintain();
    return active;
  }

  private static async register(identity: DeviceIdentity): Promise<LocalDevice> {
    const keyPair = { publicKey: identity.publicKey, privateKey: identity.privateKey };
    const signed = generateSignedPreKey(keyPair, 1);
    const pq = generatePqPreKey(keyPair, 2);
    const oneTime = generateOneTimePreKeys(3, ONE_TIME_PREKEY_TARGET);
    const draft: LocalDevice = { deviceId: "", signedPreKeys: [signed], pqPreKeys: [pq], oneTimePreKeys: oneTime, nextKeyId: 3 + oneTime.length };
    const created = await api.registerE2eDevice({
      ...preKeysInput(draft),
      oneTimePreKeys: oneTime.map((k) => ({ id: k.id, publicKey: toBase64(k.keyPair.publicKey) })),
    });
    return { ...draft, deviceId: created.id };
  }

  /** Tops up one-time prekeys and rotates the signed prekeys once a week. Best-effort. */
  private async maintain(): Promise<void> {
    if (Date.now() - this.lastMaintenance < MAINTENANCE_INTERVAL_MS) return;
    this.lastMaintenance = Date.now();
    try {
      await this.lock(async () => {
        let device = (await this.store.get<LocalDevice>("device"))!;
        const status = await api.getE2eDevice(device.deviceId);
        const count = status.oneTimePreKeyCount ?? 0;
        if (count < ONE_TIME_PREKEY_REFILL_BELOW) {
          const fresh = generateOneTimePreKeys(device.nextKeyId, ONE_TIME_PREKEY_TARGET - count);
          device = { ...device, oneTimePreKeys: [...device.oneTimePreKeys, ...fresh], nextKeyId: device.nextKeyId + fresh.length };
          // Saved before uploading: a key the server hands out must always have its private half here.
          await this.store.putMany([["device", device]]);
          await api.addE2eOneTimePreKeys(
            device.deviceId,
            fresh.map((k) => ({ id: k.id, publicKey: toBase64(k.keyPair.publicKey) }))
          );
        }
        if (Date.now() - device.signedPreKeys[0].createdAt > SIGNED_PREKEY_MAX_AGE_MS) {
          const signed = generateSignedPreKey(this.keyPair, device.nextKeyId);
          const pq = generatePqPreKey(this.keyPair, device.nextKeyId + 1);
          device = {
            ...device,
            signedPreKeys: [signed, ...device.signedPreKeys].slice(0, PREKEYS_KEPT),
            pqPreKeys: [pq, ...device.pqPreKeys].slice(0, PREKEYS_KEPT),
            nextKeyId: device.nextKeyId + 2,
          };
          await this.store.putMany([["device", device]]);
          await api.rotateE2ePreKeys(device.deviceId, preKeysInput(device));
        }
      });
    } catch {
      this.lastMaintenance = 0; // Try again next time.
    }
  }

  /** The account identity key of `userId`, as this app knows it. */
  private identityOf(userId: string): Promise<Uint8Array> {
    if (userId === this.identity.userId) return Promise.resolve(this.identity.publicKey);
    let known = this.identities.get(userId);
    if (!known) {
      known = api.getUser(userId).then((u) => fromBase64(u.publicKey));
      known.catch(() => this.identities.delete(userId));
      this.identities.set(userId, known);
    }
    return known;
  }

  /**
   * Encrypts `plaintext` for every other device in the conversation. Null
   * when someone in it has no device yet (their apps don't support this):
   * the caller then sends with the conversation key instead.
   */
  async prepare(conversationId: string, plaintext: string): Promise<PreparedMessage | null> {
    const byUser = await api.listConversationDevices(conversationId);
    if (byUser.some((u) => u.userId !== this.identity.userId && u.deviceIds.length === 0)) return null;
    const targets = byUser.flatMap((u) => u.deviceIds.map((deviceId) => ({ userId: u.userId, deviceId }))).filter((t) => t.deviceId !== this.deviceId);

    return this.lock(async () => {
      const records = new Map<string, SessionRecord>();
      const missing: typeof targets = [];
      for (const t of targets) {
        const record = await this.store.get<SessionRecord>(sessionKey(t.userId, t.deviceId));
        if (record?.current?.sendingChainKey && equalBytes(record.remoteIdentity, await this.identityOf(t.userId))) {
          records.set(sessionKey(t.userId, t.deviceId), record);
        } else {
          missing.push(t);
        }
      }
      if (missing.length > 0) {
        for (const dto of await api.fetchPreKeyBundles(conversationId, missing)) {
          const bundle = bundleFromDto(dto);
          // The server hands out identity keys too; only trust one that matches the account key this app already knows.
          if (!equalBytes(bundle.identityKey, await this.identityOf(dto.userId))) {
            throw new Error("A device's identity key doesn't match its account");
          }
          records.set(sessionKey(dto.userId, dto.deviceId), startSession(this.keyPair, bundle, this.deviceId, dto.deviceId));
        }
      }

      const messageKey = randomBytes(32);
      const nonce = randomBytes(AEAD_NONCE_BYTES);
      const ciphertext = aeadEncrypt(messageKey, nonce, utf8(plaintext), bodyAssociatedData(conversationId));
      const inner = concat(Uint8Array.of(PAYLOAD_VERSION), uuidBytes(conversationId), messageKey);

      const payloads: api.ForwardSecretEnvelope["payloads"] = [];
      const writes: [string, unknown][] = [];
      for (const t of targets) {
        const key = sessionKey(t.userId, t.deviceId);
        const record = records.get(key);
        if (!record) throw new Error("No session for a device in this conversation");
        const out = encryptFor(record, inner);
        payloads.push({ deviceId: t.deviceId, payload: toBase64(out.payload) });
        writes.push([key, out.record]);
      }
      await this.store.putMany(writes);
      return { ciphertext, nonce, envelope: { senderDeviceId: this.deviceId, payloads } };
    });
  }

  /** Keeps the plaintext of a message this device just sent, since it has no payload of its own. */
  async rememberSent(messageId: string, plaintext: string): Promise<void> {
    await this.store.putMany([[`msg:${messageId}`, { text: plaintext } satisfies MessageRecord]]);
  }

  /**
   * Plaintexts of the conversation's protocol-2 messages, by message id:
   * a string, or null when it couldn't be decrypted. Messages with nothing
   * for this device (sent before it existed) are left out.
   */
  async read(conversationId: string, messages: api.MessageDto[]): Promise<Map<string, string | null>> {
    const out = new Map<string, string | null>();
    // Retention: once a message is deleted (by its sender, or when its
    // disappearing timer or the server's retention ran out), this browser's
    // decrypted copy goes too. Message keys are gone after first use, so this
    // copy was the only readable one left.
    const gone = messages.filter((m) => m.protocol === 2 && m.deletedAt).map((m) => [`msg:${m.id}`, undefined] as [string, undefined]);
    if (gone.length > 0) await this.lock(() => this.store.putMany(gone));
    const pending = messages
      .filter((m) => m.protocol === 2 && !m.deletedAt)
      .sort((a, b) => new Date(a.createdAt).getTime() - new Date(b.createdAt).getTime());
    if (pending.length === 0) return out;

    const acknowledged = await this.lock(async () => {
      const acked: string[] = [];
      for (const m of pending) {
        const stored = await this.store.get<MessageRecord>(`msg:${m.id}`);
        if (stored) {
          out.set(m.id, "text" in stored ? stored.text : null);
          continue;
        }
        if (!m.payload || !m.senderDeviceId) continue;
        let senderIdentity: Uint8Array;
        try {
          senderIdentity = await this.identityOf(m.senderId);
        } catch {
          continue; // Offline for now: leave it for the next refresh.
        }
        out.set(m.id, await this.decryptOne(conversationId, m, senderIdentity));
        acked.push(m.id);
      }
      return acked;
    });

    if (acknowledged.length > 0) {
      api.acknowledgeE2ePayloads(this.deviceId, acknowledged).catch(() => {
        // The payloads stay on the server until the next ack; they're already used up here.
      });
    }
    void this.maintain();
    return out;
  }

  /** Decrypts one message and stores the result (and the advanced session) in one write. Must run under the lock. */
  private async decryptOne(conversationId: string, m: api.MessageDto, senderIdentity: Uint8Array): Promise<string | null> {
    const key = sessionKey(m.senderId, m.senderDeviceId!);
    try {
      const device = (await this.store.get<LocalDevice>("device"))!;
      const existing = await this.store.get<SessionRecord>(key);
      const decrypted = decryptFrom(
        this.keyPair,
        device,
        existing,
        fromBase64(m.payload!),
        m.senderDeviceId!,
        senderIdentity
      );
      const inner = decrypted.plaintext;
      if (inner.length !== 1 + 16 + 32 || inner[0] !== PAYLOAD_VERSION || !equalBytes(inner.subarray(1, 17), uuidBytes(conversationId))) {
        throw new Error("Payload isn't for this conversation");
      }
      const text = new TextDecoder().decode(
        aeadDecrypt(inner.subarray(17), fromBase64(m.nonce), fromBase64(m.ciphertext), bodyAssociatedData(conversationId))
      );
      const writes: [string, unknown][] = [
        [key, decrypted.record],
        [`msg:${m.id}`, { text } satisfies MessageRecord],
      ];
      if (decrypted.usedOneTimePreKeyId !== null) {
        writes.push(["device", { ...device, oneTimePreKeys: device.oneTimePreKeys.filter((k) => k.id !== decrypted.usedOneTimePreKeyId) }]);
      }
      await this.store.putMany(writes);
      return text;
    } catch {
      // Never retried: the payload can't become decryptable later, and a
      // second attempt would only risk the session.
      await this.store.putMany([[`msg:${m.id}`, { failed: true } satisfies MessageRecord]]);
      return null;
    }
  }
}

// --- The signed-in account's device ---

/** How long a failed or switched-off start is remembered before trying again. */
const RETRY_START_AFTER_MS = 5 * 60 * 1000;

let active: { userId: string; device: Promise<ForwardSecretDevice | null>; startedAt: number } | null = null;

/**
 * This browser's device for the signed-in account, started on first use.
 * Never throws: when the server has it off, or anything fails, it's null
 * (and the app sends with the conversation key), and it tries again a few
 * minutes later.
 */
export async function startForwardSecrecy(identity: DeviceIdentity): Promise<ForwardSecretDevice | null> {
  if (active?.userId === identity.userId) {
    const device = await active.device;
    if (device || Date.now() - active.startedAt < RETRY_START_AFTER_MS) return device;
  }
  const device = ForwardSecretDevice.start(identity).catch(() => null);
  active = { userId: identity.userId, device, startedAt: Date.now() };
  return device;
}

let signedIn: DeviceIdentity | null = null;

/** On sign-in: starts this browser's device in the background. */
export function beginForwardSecrecy(identity: DeviceIdentity): void {
  signedIn = identity;
  void startForwardSecrecy(identity);
}

/** The signed-in account's device, or null (signed out, off, or not working right now). */
export function currentForwardSecretDevice(): Promise<ForwardSecretDevice | null> {
  return signedIn ? startForwardSecrecy(signedIn) : Promise.resolve(null);
}

/** The server no longer knows this device: the next use registers a new one. */
export function resetForwardSecretDevice(): void {
  active = null;
}

/** On sign-out: forgets the in-memory device. Its encrypted state stays in this browser for the next sign-in. */
export function stopForwardSecrecy(): void {
  signedIn = null;
  active = null;
}
