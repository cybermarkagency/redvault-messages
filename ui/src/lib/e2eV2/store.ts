// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// This browser's forward-secret device state: its prekeys, its Double
// Ratchet sessions and the messages it has already decrypted (a message
// key is deleted once used, so the plaintext kept here is the only copy
// this device will ever have).
//
// Kept in IndexedDB, every record encrypted with a key derived from the
// account's private key, so it's unreadable until someone signs in, and
// bound to its own record name so records can't be swapped around.
import { aeadDecrypt, aeadEncrypt, AEAD_NONCE_BYTES, hkdfSha256, randomBytes, utf8 } from "./primitives.js";

const DB_NAME = "redvault-e2e-v2";
const STORE_NAME = "records";

interface EncryptedRecord {
  nonce: Uint8Array;
  ciphertext: Uint8Array;
}

export interface RecordStore {
  get<T>(key: string): Promise<T | undefined>;
  /** Writes (or, for `undefined`, deletes) several records in one transaction: all of them happen or none do. */
  putMany(entries: [key: string, value: unknown][]): Promise<void>;
  /** Deletes every record of this account on this browser. */
  clear(): Promise<void>;
}

// --- JSON with byte arrays ---

function bytesToBase64(bytes: Uint8Array): string {
  let binary = "";
  for (let i = 0; i < bytes.length; i += 0x8000) binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(binary);
}

function base64ToBytes(b64: string): Uint8Array {
  const binary = atob(b64);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
  return out;
}

export function encodeRecord(value: unknown): string {
  return JSON.stringify(value, (_key, v) =>
    ArrayBuffer.isView(v) && !(v instanceof DataView) ? { $b: bytesToBase64(new Uint8Array(v.buffer, v.byteOffset, v.byteLength)) } : v
  );
}

export function decodeRecord<T>(json: string): T {
  return JSON.parse(json, (_key, v) => (v && typeof v === "object" && typeof v.$b === "string" && Object.keys(v).length === 1 ? base64ToBytes(v.$b) : v)) as T;
}

// --- IndexedDB ---

function promisify<T>(req: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

let dbPromise: Promise<IDBDatabase> | null = null;

function openDb(): Promise<IDBDatabase> {
  if (!dbPromise) {
    dbPromise = new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => req.result.createObjectStore(STORE_NAME);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    }).catch((err) => {
      dbPromise = null;
      throw err;
    }) as Promise<IDBDatabase>;
  }
  return dbPromise;
}

/** Null when this browser has no working IndexedDB (some private-browsing modes): forward-secret messaging then stays off here. */
export async function openRecordStore(userId: string, accountPrivateKey: Uint8Array): Promise<RecordStore | null> {
  if (typeof indexedDB === "undefined") return null;
  let db: IDBDatabase;
  try {
    db = await openDb();
  } catch {
    return null;
  }
  const storeKey = hkdfSha256(accountPrivateKey, new Uint8Array(32), "RedvaultDeviceStore_v1", 32);
  const prefix = `${userId}/`;

  const seal = (key: string, value: unknown): EncryptedRecord => {
    const nonce = randomBytes(AEAD_NONCE_BYTES);
    return { nonce, ciphertext: aeadEncrypt(storeKey, nonce, utf8(encodeRecord(value)), utf8(prefix + key)) };
  };
  const open = <T>(key: string, record: EncryptedRecord): T =>
    decodeRecord<T>(new TextDecoder().decode(aeadDecrypt(storeKey, record.nonce, record.ciphertext, utf8(prefix + key))));

  return {
    async get<T>(key: string): Promise<T | undefined> {
      const tx = db.transaction(STORE_NAME, "readonly");
      const record = (await promisify(tx.objectStore(STORE_NAME).get(prefix + key))) as EncryptedRecord | undefined;
      return record ? open<T>(key, record) : undefined;
    },

    putMany(entries: [string, unknown][]): Promise<void> {
      // Encrypt everything first: an IndexedDB transaction commits as soon as it's left idle.
      const sealed = entries.map(([key, value]) => [prefix + key, value === undefined ? undefined : seal(key, value)] as const);
      return new Promise((resolve, reject) => {
        const tx = db.transaction(STORE_NAME, "readwrite");
        const store = tx.objectStore(STORE_NAME);
        for (const [key, record] of sealed) {
          if (record === undefined) store.delete(key);
          else store.put(record, key);
        }
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
        tx.onabort = () => reject(tx.error);
      });
    },

    clear(): Promise<void> {
      return new Promise((resolve, reject) => {
        const tx = db.transaction(STORE_NAME, "readwrite");
        tx.objectStore(STORE_NAME).delete(IDBKeyRange.bound(prefix, `${prefix}￿`));
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
      });
    },
  };
}
