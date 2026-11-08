// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Protocol-3 chat messages on this device (docs/multi-device-threat-model.md
// section 9): the envelope inside the Olm/Megolm plaintext, the per-device
// history that never leaves this browser, and the one-to-one and group
// send/receive built on device.ts. Off unless the server has E2E_V3_ENABLED
// and this browser has a device; protocol 1 is untouched.
import * as api from "../api.js";
import { parseFileDescriptor, serializeFileDescriptor, type FileDescriptor } from "../fileMessage.js";
import { EngineError, keyProblemMessage, receive, sendGroup, sendText, verifiedDevices, type Received, type V3Device } from "./device.js";

const DB_NAME = "redvault-e2e-v3-history";
const MESSAGES = "messages";
const PINS = "pins";
const MAX_TEXT_BYTES = 16 * 1024;
/** Envelopes fill 256-byte buckets, so their size shows less about what they hold. */
const PAD_BUCKET = 256;
const MAX_EMOJI_BYTES = 32;
const MAX_PAST_MS = 30 * 24 * 60 * 60 * 1000;
const MAX_FUTURE_MS = 5 * 60 * 1000;
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

export interface ChatMessage {
  conversationId: string;
  /** The envelope's id (dedupes retries and the copies sent to this account's other devices). */
  id: string;
  fromUserId: string;
  fromDeviceId: string;
  /** The sender's clock, clamped (9.3). */
  ts: number;
  arrivedAt: number;
  text: string;
  direction: "in" | "out";
  status: "sent" | "failed" | "received";
  /** A file message: `text` is then empty. */
  file?: FileDescriptor;
  /** Set on a reaction, edit or delete. These stay out of the list a chat shows (foldChat). */
  op?: ChatOp;
  /** Worked out by foldChat from the ops that apply to this message. */
  edited?: boolean;
  deleted?: boolean;
  reactions?: { userId: string; emoji: string }[];
}

/** What a reaction, edit or delete does to the message `target`. An empty emoji removes the person's reaction. */
export type ChatOp = { t: "reaction"; target: string; emoji: string } | { t: "edit"; target: string; body: string } | { t: "delete"; target: string };

// --- The envelope (9.3) ---

export interface Envelope {
  v: 1;
  t: "text";
  id: string;
  c: string;
  ts: number;
  body: string;
}

/** Spaces after the JSON, which every parser skips. */
function padded(json: string): string {
  const bytes = new TextEncoder().encode(json).length;
  return json + " ".repeat((Math.floor(bytes / PAD_BUCKET) + 1) * PAD_BUCKET - bytes);
}

export function encodeEnvelope(conversationId: string, text: string, id = crypto.randomUUID(), ts = Date.now()): string {
  if (new TextEncoder().encode(text).length > MAX_TEXT_BYTES) throw new Error("That message is too long.");
  const envelope: Envelope = { v: 1, t: "text", id, c: conversationId, ts, body: text };
  return padded(JSON.stringify(envelope));
}

/** The envelope for a reaction, edit, delete or file. */
export function encodeContent(conversationId: string, content: ChatOp | { t: "file"; file: FileDescriptor }, id = crypto.randomUUID(), ts = Date.now()): string {
  if (content.t === "edit" && new TextEncoder().encode(content.body).length > MAX_TEXT_BYTES) throw new Error("That message is too long.");
  return padded(JSON.stringify({ v: 1, id, c: conversationId, ts, ...content }));
}

export type EnvelopeResult =
  | { kind: "message"; id: string; ts: number; text: string; file?: FileDescriptor }
  | { kind: "op"; id: string; ts: number; op: ChatOp }
  /** A newer envelope version: keep the item and ask for an update. */
  | { kind: "newer" }
  /** An unknown type, or a message bound to another conversation: nothing to show; the item is acknowledged. */
  | { kind: "ignore"; reason: string };

/** Validates an envelope against the mailbox item that carried it (9.3). */
export function decodeEnvelope(plaintext: string, conversationId: string | null, now = Date.now()): EnvelopeResult {
  let parsed: unknown;
  try {
    parsed = JSON.parse(plaintext);
  } catch {
    return { kind: "ignore", reason: "not_json" };
  }
  if (typeof parsed !== "object" || parsed === null) return { kind: "ignore", reason: "not_an_envelope" };
  const e = parsed as Record<string, unknown>;
  if (e.v !== 1) return typeof e.v === "number" && e.v > 1 ? { kind: "newer" } : { kind: "ignore", reason: "bad_version" };
  if (e.t !== "text" && e.t !== "file" && e.t !== "reaction" && e.t !== "edit" && e.t !== "delete") return { kind: "ignore", reason: "unknown_type" };
  if (typeof e.id !== "string" || !UUID.test(e.id)) return { kind: "ignore", reason: "bad_id" };
  if (typeof e.c !== "string" || !conversationId || e.c !== conversationId) return { kind: "ignore", reason: "wrong_conversation" };
  const claimed = typeof e.ts === "number" && Number.isFinite(e.ts) ? e.ts : now;
  const ts = Math.min(now + MAX_FUTURE_MS, Math.max(now - MAX_PAST_MS, claimed));
  const fits = (text: unknown, max: number): text is string => typeof text === "string" && new TextEncoder().encode(text).length <= max;
  if (e.t === "file") {
    const file = parseFileDescriptor(serializeFileDescriptor(e.file as FileDescriptor));
    return file ? { kind: "message", id: e.id, ts, text: "", file } : { kind: "ignore", reason: "bad_file" };
  }
  if (e.t === "text") {
    if (!fits(e.body, MAX_TEXT_BYTES)) return { kind: "ignore", reason: "bad_body" };
    return { kind: "message", id: e.id, ts, text: e.body };
  }
  if (typeof e.target !== "string" || !UUID.test(e.target)) return { kind: "ignore", reason: "bad_target" };
  if (e.t === "delete") return { kind: "op", id: e.id, ts, op: { t: "delete", target: e.target } };
  if (e.t === "edit") {
    if (!fits(e.body, MAX_TEXT_BYTES) || e.body.length === 0) return { kind: "ignore", reason: "bad_body" };
    return { kind: "op", id: e.id, ts, op: { t: "edit", target: e.target, body: e.body } };
  }
  if (!fits(e.emoji, MAX_EMOJI_BYTES)) return { kind: "ignore", reason: "bad_emoji" };
  return { kind: "op", id: e.id, ts, op: { t: "reaction", target: e.target, emoji: e.emoji } };
}

/** Later wins: by the sender's clock, then by id, so every device settles on the same answer whatever order things arrived in. */
function newer(a: { ts: number; id: string }, b: { ts: number; id: string }): boolean {
  return a.ts > b.ts || (a.ts === b.ts && a.id > b.id);
}

/**
 * Turns stored records into what a chat shows. Only the person who sent a message can edit or delete it. An op whose
 * message hasn't arrived yet waits in the history, so arrival order never changes the result.
 */
export function foldChat(records: ChatMessage[]): ChatMessage[] {
  const messages = new Map(records.filter((r) => !r.op).map((r) => [r.id, { ...r }]));
  const ops = records.filter((r) => r.op);
  const deleted = new Set(
    ops.filter((o) => o.op!.t === "delete" && messages.get(o.op!.target)?.fromUserId === o.fromUserId).map((o) => o.op!.target)
  );
  const edits = new Map<string, ChatMessage>();
  const reactions = new Map<string, ChatMessage>();
  for (const record of ops) {
    const op = record.op!;
    const message = messages.get(op.target);
    if (!message || deleted.has(op.target)) continue;
    if (op.t === "edit" && record.fromUserId === message.fromUserId && !message.file) {
      const held = edits.get(op.target);
      if (!held || newer(record, held)) edits.set(op.target, record);
    } else if (op.t === "reaction") {
      const key = `${op.target}\0${record.fromUserId}`;
      const held = reactions.get(key);
      if (!held || newer(record, held)) reactions.set(key, record);
    }
  }
  for (const [target, record] of edits) {
    const message = messages.get(target)!;
    message.text = (record.op as { body: string }).body;
    message.edited = true;
  }
  for (const record of reactions.values()) {
    const op = record.op as { target: string; emoji: string };
    if (!op.emoji) continue;
    const message = messages.get(op.target)!;
    message.reactions = [...(message.reactions ?? []), { userId: record.fromUserId, emoji: op.emoji }];
  }
  for (const id of deleted) {
    const message = messages.get(id)!;
    message.text = "";
    delete message.file;
    delete message.reactions;
    message.deleted = true;
  }
  return [...messages.values()].sort((a, b) => a.ts - b.ts || a.arrivedAt - b.arrivedAt);
}

// --- Local history (9.7): encrypted under a key derived from the device's storage key ---

interface StoredRecord {
  userId: string;
  conversationId: string;
  id: string;
  /** Plain so a conversation can be listed in order; the rest is encrypted. */
  ts: number;
  iv: Uint8Array<ArrayBuffer>;
  sealed: Uint8Array<ArrayBuffer>;
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

function aad(userId: string, conversationId: string, id: string): Uint8Array<ArrayBuffer> {
  return new TextEncoder().encode(`redvault-v3-history\0${userId}\0${conversationId}\0${id}`);
}

/** One account's chat history in one browser. Tests make one per simulated browser. */
export class ChatHistory {
  private db: Promise<IDBDatabase> | null = null;
  private key: Promise<CryptoKey> | null = null;

  constructor(
    private readonly device: V3Device,
    private readonly factory: IDBFactory = indexedDB
  ) {}

  private open(): Promise<IDBDatabase> {
    if (!this.db) {
      this.db = new Promise<IDBDatabase>((resolve, reject) => {
        const req = this.factory.open(DB_NAME, 1);
        req.onupgradeneeded = () => {
          const messages = req.result.createObjectStore(MESSAGES, { keyPath: ["userId", "conversationId", "id"] });
          messages.createIndex("byConversation", ["userId", "conversationId", "ts"]);
          req.result.createObjectStore(PINS, { keyPath: ["userId", "conversationId"] });
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
      }).catch((err) => {
        this.db = null;
        throw err;
      });
    }
    return this.db;
  }

  private historyKey(): Promise<CryptoKey> {
    if (!this.key) this.key = this.device.historyKey();
    return this.key;
  }

  private async seal(message: ChatMessage): Promise<StoredRecord> {
    const userId = this.device.userId;
    const iv = crypto.getRandomValues(new Uint8Array(12));
    const { fromUserId, fromDeviceId, arrivedAt, text, direction, status, file, op } = message;
    const plain = new TextEncoder().encode(JSON.stringify({ fromUserId, fromDeviceId, arrivedAt, text, direction, status, file, op }));
    const sealed = new Uint8Array(
      await crypto.subtle.encrypt({ name: "AES-GCM", iv, additionalData: aad(userId, message.conversationId, message.id) }, await this.historyKey(), plain)
    );
    return { userId, conversationId: message.conversationId, id: message.id, ts: message.ts, iv, sealed };
  }

  private async unseal(record: StoredRecord): Promise<ChatMessage> {
    const plain = await crypto.subtle.decrypt(
      { name: "AES-GCM", iv: record.iv, additionalData: aad(record.userId, record.conversationId, record.id) },
      await this.historyKey(),
      record.sealed
    );
    const fields = JSON.parse(new TextDecoder().decode(plain)) as Omit<ChatMessage, "conversationId" | "id" | "ts">;
    return { conversationId: record.conversationId, id: record.id, ts: record.ts, ...fields };
  }

  /** Adds a message unless this conversation already has one with that id. Returns whether it was new. */
  async add(message: ChatMessage): Promise<boolean> {
    const record = await this.seal(message);
    const transaction = (await this.open()).transaction(MESSAGES, "readwrite");
    const store = transaction.objectStore(MESSAGES);
    const existing = await promisify(store.getKey([record.userId, record.conversationId, record.id]));
    if (existing !== undefined) {
      await done(transaction);
      return false;
    }
    store.put(record);
    await done(transaction);
    return true;
  }

  /** Changes a message's status (a failed send that was retried, say). */
  async setStatus(conversationId: string, id: string, status: ChatMessage["status"]): Promise<void> {
    const userId = this.device.userId;
    const transaction = (await this.open()).transaction(MESSAGES, "readwrite");
    const store = transaction.objectStore(MESSAGES);
    const record = (await promisify(store.get([userId, conversationId, id]))) as StoredRecord | undefined;
    if (!record) {
      await done(transaction);
      return;
    }
    const message = await this.unseal(record);
    store.put(await this.seal({ ...message, status }));
    await done(transaction);
  }

  /** The newest message of every conversation (for the chat list). Only those are decrypted. */
  async latestPerConversation(): Promise<ChatMessage[]> {
    const userId = this.device.userId;
    const transaction = (await this.open()).transaction(MESSAGES, "readonly");
    const all = ((await promisify(transaction.objectStore(MESSAGES).getAll())) as StoredRecord[]).filter((r) => r.userId === userId);
    await done(transaction);
    const newest = new Map<string, StoredRecord>();
    for (const record of all) {
      const known = newest.get(record.conversationId);
      if (!known || record.ts >= known.ts) newest.set(record.conversationId, record);
    }
    return Promise.all([...newest.values()].map((r) => this.unseal(r)));
  }

  /** Drops one message from this device's history (a failed send the person discarded or retried). */
  async remove(conversationId: string, id: string): Promise<void> {
    const transaction = (await this.open()).transaction(MESSAGES, "readwrite");
    transaction.objectStore(MESSAGES).delete([this.device.userId, conversationId, id]);
    await done(transaction);
  }

  /** What the chat shows. */
  async list(conversationId: string): Promise<ChatMessage[]> {
    return foldChat(await this.records(conversationId));
  }

  /**
   * Wipes the text and file key of messages their sender deleted, along with their edits and reactions,
   * so "delete for everyone" doesn't leave the words in this device's history.
   */
  async eraseDeleted(conversationId: string): Promise<void> {
    const records = await this.records(conversationId);
    for (const record of records) {
      if (record.op?.t !== "delete") continue;
      const { target } = record.op;
      const message = records.find((r) => r.id === target && !r.op);
      if (!message || message.fromUserId !== record.fromUserId) continue;
      for (const stale of records) {
        if (stale.op && stale.op.t !== "delete" && stale.op.target === target) await this.remove(conversationId, stale.id);
      }
      if (message.text || message.file) await this.replace({ ...message, text: "", file: undefined });
    }
  }

  private async replace(message: ChatMessage): Promise<void> {
    const sealed = await this.seal(message);
    const transaction = (await this.open()).transaction(MESSAGES, "readwrite");
    transaction.objectStore(MESSAGES).put(sealed);
    await done(transaction);
  }

  private async records(conversationId: string): Promise<ChatMessage[]> {
    const userId = this.device.userId;
    const transaction = (await this.open()).transaction(MESSAGES, "readonly");
    const store = transaction.objectStore(MESSAGES);
    // Every browser has IDBKeyRange; a bare test environment may not, and then the whole store is filtered instead.
    const records =
      typeof IDBKeyRange !== "undefined"
        ? ((await promisify(store.index("byConversation").getAll(IDBKeyRange.bound([userId, conversationId, -Infinity], [userId, conversationId, Infinity])))) as StoredRecord[])
        : ((await promisify(store.getAll())) as StoredRecord[]).filter((r) => r.userId === userId && r.conversationId === conversationId);
    await done(transaction);
    const out = await Promise.all(records.map((r) => this.unseal(r)));
    return out.sort((a, b) => a.ts - b.ts || a.arrivedAt - b.arrivedAt);
  }

  /** Marks the conversation as one that has used protocol 3 here: it never silently falls back (9.1 item 5). */
  async pin(conversationId: string): Promise<void> {
    const transaction = (await this.open()).transaction(PINS, "readwrite");
    transaction.objectStore(PINS).put({ userId: this.device.userId, conversationId });
    await done(transaction);
  }

  async isPinned(conversationId: string): Promise<boolean> {
    const transaction = (await this.open()).transaction(PINS, "readonly");
    const found = await promisify(transaction.objectStore(PINS).getKey([this.device.userId, conversationId]));
    await done(transaction);
    return found !== undefined;
  }

  /** The person confirmed "use the older protocol" for this chat. */
  async unpin(conversationId: string): Promise<void> {
    const transaction = (await this.open()).transaction(PINS, "readwrite");
    transaction.objectStore(PINS).delete([this.device.userId, conversationId]);
    await done(transaction);
  }
}

// --- Sending and receiving ---

export class PinnedChatError extends Error {
  constructor() {
    super("This chat uses per-device keys, and the other side has no device keys now. Nothing was sent. Ask them to set up device keys, or choose to use the older protocol for this chat.");
  }
}

export interface ChatTarget {
  conversationId: string;
  /** A direct chat's other participant; null for a group. */
  contactUserId: string | null;
}

/**
 * Sends `text`. Throws PinnedChatError (and sends nothing) for a pinned chat whose
 * contact has no listed device. The message is in the history as "sent" when this
 * returns, or as "failed" (and the error is rethrown) when the server refused it.
 */
export async function sendChatText(device: V3Device, history: ChatHistory, target: ChatTarget, text: string): Promise<ChatMessage> {
  const id = crypto.randomUUID();
  const ts = Date.now();
  return deliver(device, history, target, encodeEnvelope(target.conversationId, text, id, ts), outgoing(device, target, id, ts, { text }));
}

/** Sends a file already uploaded and released. The key inside `file` only ever travels in the envelope. */
export function sendChatFile(device: V3Device, history: ChatHistory, target: ChatTarget, file: FileDescriptor): Promise<ChatMessage> {
  const id = crypto.randomUUID();
  const ts = Date.now();
  return deliver(device, history, target, encodeContent(target.conversationId, { t: "file", file }, id, ts), outgoing(device, target, id, ts, { text: "", file }));
}

/** Sends a reaction, edit or delete. A failed one isn't kept: the person sees it didn't work and can try again. */
export async function sendChatOp(device: V3Device, history: ChatHistory, target: ChatTarget, op: ChatOp): Promise<void> {
  const id = crypto.randomUUID();
  const ts = Date.now();
  const message = outgoing(device, target, id, ts, { text: "", op });
  await deliver(device, history, target, encodeContent(target.conversationId, op, id, ts), message, false);
  if (op.t === "delete") await history.eraseDeleted(target.conversationId);
}

function outgoing(device: V3Device, target: ChatTarget, id: string, ts: number, content: Pick<ChatMessage, "text" | "file" | "op">): ChatMessage {
  return {
    conversationId: target.conversationId,
    id,
    fromUserId: device.userId,
    fromDeviceId: device.deviceId,
    ts,
    arrivedAt: ts,
    direction: "out",
    status: "sent",
    ...content,
  };
}

async function deliver(device: V3Device, history: ChatHistory, target: ChatTarget, envelope: string, message: ChatMessage, keepFailed = true): Promise<ChatMessage> {
  try {
    if (target.contactUserId) {
      const delivered = await sendText(device, target.conversationId, target.contactUserId, envelope);
      if (delivered === 0) throw new PinnedChatError();
    } else {
      await sendGroup(device, target.conversationId, envelope);
    }
  } catch (err) {
    if (err instanceof PinnedChatError || !keepFailed) throw err;
    await history.add({ ...message, status: "failed" });
    throw err;
  }
  await history.add(message);
  await history.pin(target.conversationId);
  return message;
}

/** Checks the mailbox once: decrypts, keeps new messages in the history, and acknowledges. Returns what was new. */
export async function receiveChat(device: V3Device, history: ChatHistory): Promise<ChatMessage[]> {
  const fresh: ChatMessage[] = [];
  await receive(device, {
    persist: async (items: Received[]) => {
      const now = Date.now();
      for (const item of items) {
        if (item.error || item.kind !== "message" || item.text === null || !item.conversationId) continue;
        const decoded = decodeEnvelope(item.text, item.conversationId, now);
        if (decoded.kind !== "message" && decoded.kind !== "op") continue;
        const mine = item.senderUserId === device.userId;
        const message: ChatMessage = {
          conversationId: item.conversationId,
          id: decoded.id,
          fromUserId: item.senderUserId,
          fromDeviceId: item.senderDeviceId,
          ts: decoded.ts,
          arrivedAt: now,
          text: decoded.kind === "message" ? decoded.text : "",
          direction: mine ? "out" : "in",
          status: mine ? "sent" : "received",
          ...(decoded.kind === "message" ? { file: decoded.file } : { op: decoded.op }),
        };
        if (await history.add(message)) {
          fresh.push(message);
          await history.pin(item.conversationId);
          // Covers a delete that arrived before the message it deletes, too.
          await history.eraseDeleted(item.conversationId);
        }
      }
    },
  });
  return fresh;
}

/** Whether this browser can use protocol 3 in `conversationId` right now: a device exists and the server has the flag on. */
export async function chatAvailable(): Promise<boolean> {
  try {
    return (await api.getV3Config()).enabled;
  } catch {
    return false;
  }
}

/**
 * Which protocol a send in this chat uses: "v3" when the contact (or a group)
 * has device keys; "v1" when this chat never used v3 and the contact has none;
 * "blocked" when it is pinned to v3 and the contact has none now (9.1 item 5).
 */
export async function chatMode(device: V3Device, history: ChatHistory, target: ChatTarget): Promise<"v3" | "v1" | "blocked"> {
  const pinned = await history.isPinned(target.conversationId);
  if (!target.contactUserId) return "v3";
  let hasDevices = false;
  try {
    hasDevices = (await verifiedDevices(device, target.contactUserId, target.conversationId)).devices.length > 0;
  } catch (err) {
    // A list that doesn't verify is not a reason to fall back to the older protocol.
    if (err instanceof EngineError && keyProblemMessage(err.code)) return "blocked";
    return pinned ? "blocked" : "v1";
  }
  if (hasDevices) return "v3";
  return pinned ? "blocked" : "v1";
}
