// Messages waiting for the connection (Tor Browser lost Tor, or the onion
// service is unreachable): the web side of the apps' sealed outbox
// (docs/tor-routing-plan.md, desktop data/outbox, Android data/Outbox.kt).
//
// Each item is sealed with XChaCha20-Poly1305 under a key derived from the
// account's private key (crypto_kdf "rvoutbx1", 1: the same derivation as
// the other apps), bound to its id by the associated data, and kept in
// IndexedDB until the server has stored the message. The item's id is the
// message's clientMessageId (db/migrations/033), so a send repeated after a
// lost answer is stored once.
//
// Tor Browser always browses privately: its IndexedDB lives in memory and is
// gone when the browser closes, so there the queue lasts as long as the
// browser session (reloads included), not across a browser restart. That's
// Tor Browser keeping nothing on disk, on purpose.

import * as crypto from "./e2eCrypto.js";

export interface OutboxItem {
  id: string;
  conversationId: string;
  text: string;
  replyTo?: { messageId: string; senderId: string; preview: string; fromMe: boolean };
  createdAt: number;
  /** Send order: strictly increasing. */
  order: number;
  attempts: number;
  /** The last failure, safe to show. */
  lastError?: string;
  /** The server refused it: it waits for Retry or Discard. */
  held: boolean;
}

const DB_NAME = "redvault-outbox";
const STORE = "items";
const KDF_CONTEXT = "rvoutbx1";

let lastOrder = 0;

function open(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, 1);
    req.onupgradeneeded = () => req.result.createObjectStore(STORE, { keyPath: "id" });
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

async function withStore<T>(mode: IDBTransactionMode, fn: (store: IDBObjectStore) => IDBRequest<T>): Promise<T> {
  const db = await open();
  try {
    return await new Promise<T>((resolve, reject) => {
      const tx = db.transaction(STORE, mode);
      const req = fn(tx.objectStore(STORE));
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  } finally {
    db.close();
  }
}

/** crypto_kdf_derive_from_key(32, 1, "rvoutbx1", account private key). */
export async function outboxKey(accountPrivateKey: Uint8Array): Promise<Uint8Array> {
  const sodium = await crypto.ready();
  return sodium.crypto_kdf_derive_from_key(32, 1, KDF_CONTEXT, accountPrivateKey);
}

/** What IndexedDB holds per item: its id, and nonce + ciphertext as base64. */
interface SealedRecord {
  id: string;
  sealed: string;
}

async function seal(item: OutboxItem, key: Uint8Array): Promise<SealedRecord> {
  const sodium = await crypto.ready();
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  const body = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(
    // A string: libsodium converts it itself (a Uint8Array from another realm, e.g. a test's, would be refused).
    JSON.stringify(item),
    `outbox-item:${item.id}`,
    null,
    nonce,
    key
  );
  const sealed = new Uint8Array(nonce.length + body.length);
  sealed.set(nonce);
  sealed.set(body, nonce.length);
  return { id: item.id, sealed: crypto.toBase64(sealed) };
}

async function unseal(record: SealedRecord, key: Uint8Array): Promise<OutboxItem | null> {
  const sodium = await crypto.ready();
  const n = sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
  try {
    const sealed = crypto.fromBase64(record.sealed);
    const plain = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(
      null,
      sealed.slice(n),
      `outbox-item:${record.id}`,
      sealed.slice(0, n),
      key,
      "text"
    );
    const item = JSON.parse(plain) as OutboxItem;
    return item.id === record.id ? item : null;
  } catch {
    // Another account's, or damaged: not ours to show.
    return null;
  }
}

async function put(item: OutboxItem, key: Uint8Array): Promise<OutboxItem> {
  const record = await seal(item, key);
  await withStore("readwrite", (store) => store.put(record));
  return item;
}

/** This account's queued items, oldest first. */
export async function items(key: Uint8Array): Promise<OutboxItem[]> {
  const records = await withStore<SealedRecord[]>("readonly", (store) => store.getAll());
  const opened = (await Promise.all(records.map((r) => unseal(r, key)))).filter((i): i is OutboxItem => i !== null);
  opened.sort((a, b) => a.order - b.order);
  for (const i of opened) lastOrder = Math.max(lastOrder, i.order);
  return opened;
}

export async function addText(
  key: Uint8Array,
  id: string,
  conversationId: string,
  text: string,
  replyTo?: OutboxItem["replyTo"]
): Promise<OutboxItem> {
  lastOrder = Math.max(Date.now() * 1000, lastOrder + 1);
  return put({ id, conversationId, text, replyTo, createdAt: Date.now(), order: lastOrder, attempts: 0, held: false }, key);
}

export async function recordAttempt(key: Uint8Array, item: OutboxItem, error: string, hold: boolean): Promise<OutboxItem> {
  return put({ ...item, attempts: item.attempts + 1, lastError: error, held: item.held || hold }, key);
}

export async function release(key: Uint8Array, item: OutboxItem): Promise<OutboxItem> {
  return put({ ...item, held: false, lastError: undefined }, key);
}

export async function remove(id: string): Promise<void> {
  await withStore("readwrite", (store) => store.delete(id));
}

/** A clientMessageId: ^[A-Za-z0-9_-]{16,64}$ (server/src/routes/conversationRoutes.ts). */
export function newClientMessageId(): string {
  const bytes = new Uint8Array(16);
  globalThis.crypto.getRandomValues(bytes);
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}
