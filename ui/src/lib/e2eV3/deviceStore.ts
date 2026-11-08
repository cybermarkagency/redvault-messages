// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// This browser's protocol-3 device (docs/multi-device-threat-model.md §5):
// the engine's encrypted state, and the 32-byte storage key it's encrypted
// under, which is itself wrapped by a NON-EXTRACTABLE WebCrypto AES-GCM key.
// All three live only in this browser's IndexedDB; none of them ever goes to
// the server.
//
// What this protects, honestly: someone who copies the browser profile's
// files gets the wrapped key and the encrypted state, and no way to export the
// WebCrypto key. Code running in this page (XSS, a tampered bundle, a hostile
// extension) can still *use* the key while the page is open. Clearing site
// data loses the device: it must be linked again, which loses its protocol-3
// history (other devices and the recovery key are unaffected).

const DB_NAME = "redvault-e2e-v3";
const STORE_NAME = "devices";
const WRAP_CONTEXT = "redvault-v3-storage-key";

interface StoredDevice {
  userId: string;
  deviceId: string;
  wrappingKey: CryptoKey;
  iv: Uint8Array<ArrayBuffer>;
  wrappedStorageKey: Uint8Array<ArrayBuffer>;
  /** The engine's state, encrypted by the engine under the storage key (base64). Null until the engine made one. */
  state: string | null;
}

export interface OpenDevice {
  deviceId: string;
  /** base64; handed to the engine with each call, never stored unwrapped. */
  storageKey: string;
  state: string | null;
}

function promisify<T>(req: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function done(transaction: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    transaction.oncomplete = () => resolve();
    transaction.onerror = () => reject(transaction.error);
    transaction.onabort = () => reject(transaction.error);
  });
}

function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (const b of bytes) binary += String.fromCharCode(b);
  return btoa(binary);
}

function additionalData(userId: string, deviceId: string): Uint8Array<ArrayBuffer> {
  return new TextEncoder().encode(`${WRAP_CONTEXT}\0${userId}\0${deviceId}`);
}

/** Whether this browser can keep a device at all (some private modes have no IndexedDB or WebCrypto). */
export function deviceStorageAvailable(): boolean {
  return typeof indexedDB !== "undefined" && typeof crypto !== "undefined" && typeof crypto.subtle !== "undefined";
}

/** One browser's devices, one per account. Tests make one per simulated browser. */
export class DeviceStore {
  private db: Promise<IDBDatabase> | null = null;

  constructor(private readonly factory: IDBFactory = indexedDB) {}

  private open(): Promise<IDBDatabase> {
    if (!this.db) {
      this.db = new Promise<IDBDatabase>((resolve, reject) => {
        const req = this.factory.open(DB_NAME, 1);
        req.onupgradeneeded = () => req.result.createObjectStore(STORE_NAME, { keyPath: "userId" });
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      }).catch((err) => {
        this.db = null;
        throw err;
      });
    }
    return this.db;
  }

  private async read(userId: string): Promise<StoredDevice | undefined> {
    const transaction = (await this.open()).transaction(STORE_NAME, "readonly");
    const record = (await promisify(transaction.objectStore(STORE_NAME).get(userId))) as StoredDevice | undefined;
    await done(transaction);
    return record;
  }

  /**
   * A new device for `userId` in this browser: a random device id, a random
   * storage key, and a fresh non-extractable key that wraps it. Replaces any
   * device this browser already had for the account.
   */
  async create(userId: string): Promise<OpenDevice> {
    const deviceId = crypto.randomUUID();
    const wrappingKey = await crypto.subtle.generateKey({ name: "AES-GCM", length: 256 }, false, ["encrypt", "decrypt"]);
    const storageKey = crypto.getRandomValues(new Uint8Array(32));
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const wrapped = new Uint8Array(
      await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: additionalData(userId, deviceId) }, wrappingKey, storageKey)
    );
    const transaction = (await this.open()).transaction(STORE_NAME, "readwrite");
    transaction.objectStore(STORE_NAME).put({ userId, deviceId, wrappingKey, iv, wrappedStorageKey: wrapped, state: null } satisfies StoredDevice);
    await done(transaction);
    const out = { deviceId, storageKey: bytesToBase64(storageKey), state: null };
    storageKey.fill(0);
    return out;
  }

  /** This browser's device for `userId`, or null when it has none (or its key can't be used any more). */
  async load(userId: string): Promise<OpenDevice | null> {
    const record = await this.read(userId);
    if (!record) return null;
    try {
      const raw = new Uint8Array(
        await crypto.subtle.decrypt(
          { name: "AES-GCM", iv: record.iv, additionalData: additionalData(userId, record.deviceId) },
          record.wrappingKey,
          record.wrappedStorageKey
        )
      );
      const storageKey = bytesToBase64(raw);
      raw.fill(0);
      return { deviceId: record.deviceId, storageKey, state: record.state };
    } catch {
      return null;
    }
  }

  /** Saves the engine's new state. Called before anything that call produced is sent. */
  async saveState(userId: string, deviceId: string, state: string): Promise<void> {
    const transaction = (await this.open()).transaction(STORE_NAME, "readwrite");
    const store = transaction.objectStore(STORE_NAME);
    const record = (await promisify(store.get(userId))) as StoredDevice | undefined;
    if (!record || record.deviceId !== deviceId) {
      transaction.abort();
      throw new Error("this browser's device was replaced or removed");
    }
    store.put({ ...record, state });
    await done(transaction);
  }

  /** Forgets this browser's device for `userId`: its keys and its protocol-3 history here. */
  async wipe(userId: string): Promise<void> {
    const transaction = (await this.open()).transaction(STORE_NAME, "readwrite");
    transaction.objectStore(STORE_NAME).delete(userId);
    await done(transaction);
  }
}

let defaultStore: DeviceStore | null = null;

/** This browser's store. */
export function browserDeviceStore(): DeviceStore {
  if (!defaultStore) defaultStore = new DeviceStore();
  return defaultStore;
}
