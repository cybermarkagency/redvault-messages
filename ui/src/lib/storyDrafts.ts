// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Unfinished stories, kept on this device only.
//
// Nothing readable is stored: the details are sealed with a key derived one-way from the account's
// private key (so a draft opens only while you're signed in as that account), and each slide's photo,
// video or audio is encrypted like any story media with its key inside the sealed details. A draft never
// reaches the server, and signing out leaves only ciphertext behind. Ten per account, 30 days.
//
// A draft holds all the slides of a story and the choices that apply to the story as a whole (who sees it,
// how long, replies). A story that failed to upload is saved here too, so nothing typed or recorded is lost.
import sodium from "libsodium-wrappers-sumo";
import { ready, fromBase64, toBase64 } from "./e2eCrypto.js";
import { decryptFile, encryptFile, FILE_CHUNK_BYTES, FILE_CHUNK_OVERHEAD_BYTES } from "./fileCrypto.js";
import type { PreparedMedia, StoryAudiencePick, StoryIdentity } from "./stories.js";
import { parseCaptions, parseStickers, parseWaveform, STORY_BACKGROUNDS, STORY_FONTS, type StoryBackground, type StoryCaption, type StoryFont, type StoryOverlay, type StorySticker } from "./storyCrypto.js";

export const MAX_DRAFTS = 10;
export const MAX_SLIDES = 10;
export const DRAFT_KEEP_MS = 30 * 24 * 3600_000;
const DB_NAME = "redvault-story-drafts";
const STORE = "drafts";
const KEY_LABEL = "redvault-story-drafts-v1";
const AD_LABEL = "redvault-story-draft-v1";

export class DraftsUnavailableError extends Error {
  constructor() {
    super("Drafts can't be saved in this browser (storage is blocked or full).");
  }
}

/** One slide: a photo, video, voice recording or text, with what's on it. `media` is the plaintext file, in memory only. */
export interface DraftSlide {
  mode: "media" | "text" | "audio";
  text: string;
  background: StoryBackground;
  font: StoryFont;
  overlays: StoryOverlay[];
  stickers: StorySticker[];
  captions: StoryCaption[];
  media: PreparedMedia | null;
}

/** A whole unfinished story. */
export interface DraftContent {
  slides: DraftSlide[];
  audience: StoryAudiencePick;
  customIds: string[];
  allowReplies: boolean;
  ttlSeconds: number;
}

export interface DraftSummary {
  id: string;
  updatedAt: number;
  slideCount: number;
  /** The first slide: the start of its text, or "Photo", "Video", "Voice". */
  preview: string;
  stickerCount: number;
}

type MediaRef = { kind: "image" | "video" | "audio"; key: string; header: string; sha256: string; encryptedSize: number; chunkSize: number; mime: string; width: number; height: number; durationMs?: number; waveform?: number[] };

interface SealedDraft {
  v: 2;
  story: Omit<DraftContent, "slides">;
  slides: { content: Omit<DraftSlide, "media">; media: MediaRef | null }[];
  summary: Omit<DraftSummary, "id" | "updatedAt">;
}

interface StoredDraft {
  id: string;
  userId: string;
  updatedAt: number;
  nonce: Uint8Array;
  sealed: Uint8Array;
  /** The encrypted media of each slide, in order (null where a slide has none). Plain bytes rather than Blobs: older browsers store Blobs unreliably. */
  media: (ArrayBuffer | null)[];
}

// --- IndexedDB -------------------------------------------------------------------------------------

function openDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === "undefined") return reject(new DraftsUnavailableError());
    let req: IDBOpenDBRequest;
    try {
      req = indexedDB.open(DB_NAME, 1);
    } catch {
      return reject(new DraftsUnavailableError());
    }
    req.onupgradeneeded = () => {
      const store = req.result.createObjectStore(STORE, { keyPath: "id" });
      store.createIndex("byUser", "userId");
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(new DraftsUnavailableError());
    req.onblocked = () => reject(new DraftsUnavailableError());
  });
}

async function withStore<T>(mode: IDBTransactionMode, run: (store: IDBObjectStore) => IDBRequest<T> | void): Promise<T | undefined> {
  const db = await openDb();
  try {
    return await new Promise<T | undefined>((resolve, reject) => {
      const tx = db.transaction(STORE, mode);
      const request = run(tx.objectStore(STORE));
      tx.oncomplete = () => resolve(request ? request.result : undefined);
      tx.onerror = () => reject(new DraftsUnavailableError());
      tx.onabort = () => reject(new DraftsUnavailableError());
    });
  } finally {
    db.close();
  }
}

const allOf = async (userId: string): Promise<StoredDraft[]> =>
  ((await withStore<StoredDraft[]>("readonly", (s) => s.index("byUser").getAll(userId))) ?? []) as StoredDraft[];

// --- Sealing -----------------------------------------------------------------------------------------

function draftKey(privateKey: Uint8Array): Uint8Array {
  return sodium.crypto_generichash(32, KEY_LABEL, privateKey);
}

/** A copy in this realm's Uint8Array (libsodium's own output fails its AEAD type checks in some builds, see e2eCrypto.ts). */
function ad(userId: string, id: string): Uint8Array {
  const src = sodium.from_string(`${AD_LABEL}:${userId}:${id}`);
  const out = new Uint8Array(src.length);
  out.set(src);
  return out;
}

function readBlob(blob: Blob): Promise<Uint8Array> {
  if (typeof blob.arrayBuffer === "function") return blob.arrayBuffer().then((b) => new Uint8Array(b));
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error ?? new Error("Couldn't read the draft"));
    reader.readAsArrayBuffer(blob);
  });
}

async function* chunksOf(blob: Blob): AsyncGenerator<Uint8Array> {
  const frame = FILE_CHUNK_BYTES + FILE_CHUNK_OVERHEAD_BYTES;
  for (let offset = 0; offset < blob.size; offset += frame) yield await readBlob(blob.slice(offset, offset + frame));
}

function unseal(identity: StoryIdentity, row: StoredDraft): SealedDraft | null {
  try {
    // Copies in this realm's Uint8Array: what IndexedDB hands back can be another realm's, which libsodium's type checks refuse (see e2eCrypto.ts).
    const plain = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, Uint8Array.from(row.sealed), ad(identity.userId, row.id), Uint8Array.from(row.nonce), draftKey(identity.privateKey));
    const parsed = JSON.parse(sodium.to_string(plain)) as SealedDraft;
    return parsed && parsed.v === 2 && Array.isArray(parsed.slides) && parsed.slides.length > 0 && parsed.story ? parsed : null;
  } catch {
    return null;
  }
}

// --- Public API ----------------------------------------------------------------------------------------

function newId(): string {
  return sodium.to_hex(sodium.randombytes_buf(12));
}

const slidePreview = (s: DraftSlide): string => (s.media ? (s.media.kind === "video" ? "Video" : s.media.kind === "audio" ? s.text.trim().slice(0, 60) || "Voice" : "Photo") : s.text.trim().slice(0, 60));

/** Your drafts, newest first. Ones that are too old, or can't be opened any more, are deleted. */
export async function listDrafts(identity: StoryIdentity): Promise<DraftSummary[]> {
  await ready();
  const rows = await allOf(identity.userId);
  const out: DraftSummary[] = [];
  const drop: string[] = [];
  for (const row of rows) {
    const sealed = Date.now() - row.updatedAt > DRAFT_KEEP_MS ? null : unseal(identity, row);
    if (!sealed) drop.push(row.id);
    else out.push({ id: row.id, updatedAt: row.updatedAt, ...sealed.summary });
  }
  for (const id of drop) await deleteDraft(id).catch(() => {});
  return out.sort((a, b) => b.updatedAt - a.updatedAt);
}

/** Saves a draft (a new one, or over `id`). The oldest goes when there are already ten. */
export async function saveDraft(identity: StoryIdentity, content: DraftContent, id?: string): Promise<string> {
  await ready();
  const draftId = id ?? newId();
  const slides = content.slides.slice(0, MAX_SLIDES);
  if (slides.length === 0) throw new Error("There is nothing to save.");
  const mediaBytes: (ArrayBuffer | null)[] = [];
  const sealedSlides: SealedDraft["slides"] = [];
  for (const slide of slides) {
    const { media, ...rest } = slide;
    let ref: MediaRef | null = null;
    if (media) {
      const enc = await encryptFile(media.blob);
      mediaBytes.push((await readBlob(enc.blob)).buffer as ArrayBuffer);
      ref = {
        kind: media.kind,
        key: toBase64(enc.key),
        header: toBase64(enc.header),
        sha256: enc.ciphertextSha256,
        encryptedSize: enc.encryptedSize,
        chunkSize: enc.chunkSize,
        mime: media.mime,
        width: media.width,
        height: media.height,
        ...(media.durationMs ? { durationMs: media.durationMs } : {}),
        ...(media.waveform ? { waveform: media.waveform } : {}),
      };
    } else {
      mediaBytes.push(null);
    }
    sealedSlides.push({ content: rest, media: ref });
  }
  const { slides: _slides, ...story } = content;
  void _slides;
  const sealedJson: SealedDraft = {
    v: 2,
    story,
    slides: sealedSlides,
    summary: { slideCount: slides.length, preview: slidePreview(slides[0]), stickerCount: slides.reduce((n, s) => n + s.stickers.length, 0) },
  };
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  const sealed = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(JSON.stringify(sealedJson), ad(identity.userId, draftId), null, nonce, draftKey(identity.privateKey));
  const existing = await allOf(identity.userId);
  if (!existing.some((r) => r.id === draftId) && existing.length >= MAX_DRAFTS) {
    const oldest = existing.sort((a, b) => a.updatedAt - b.updatedAt)[0];
    await deleteDraft(oldest.id);
  }
  const row: StoredDraft = { id: draftId, userId: identity.userId, updatedAt: Date.now(), nonce, sealed, media: mediaBytes };
  await withStore("readwrite", (s) => s.put(row));
  return draftId;
}

/** Opens a draft for editing; null if it is gone or can't be opened. */
export async function loadDraft(identity: StoryIdentity, id: string): Promise<DraftContent | null> {
  await ready();
  const row = (await withStore<StoredDraft | undefined>("readonly", (s) => s.get(id))) as StoredDraft | undefined;
  if (!row || row.userId !== identity.userId) return null;
  const sealed = unseal(identity, row);
  if (!sealed) return null;
  const slides: DraftSlide[] = [];
  for (let i = 0; i < sealed.slides.length && i < MAX_SLIDES; i++) {
    const { content: c, media: ref } = sealed.slides[i];
    let media: PreparedMedia | null = null;
    if (ref) {
      const bytes = row.media?.[i];
      if (!bytes) return null;
      try {
        const blob = await decryptFile(
          chunksOf(new Blob([bytes])),
          { key: fromBase64(ref.key), header: fromBase64(ref.header), chunkSize: ref.chunkSize, encryptedSize: ref.encryptedSize, ciphertextSha256: ref.sha256 },
          ref.mime
        );
        media = { kind: ref.kind, blob, mime: ref.mime, width: ref.width, height: ref.height, durationMs: ref.durationMs, ...(parseWaveform(ref.waveform) ? { waveform: ref.waveform } : {}) };
      } catch {
        return null;
      }
    }
    slides.push({
      mode: c.mode === "media" && media ? "media" : c.mode === "audio" && media ? "audio" : "text",
      text: typeof c.text === "string" ? c.text : "",
      background: STORY_BACKGROUNDS.includes(c.background) ? c.background : "sunset",
      font: STORY_FONTS.includes(c.font) ? c.font : "classic",
      overlays: Array.isArray(c.overlays) ? c.overlays : [],
      stickers: parseStickers(c.stickers),
      captions: parseCaptions(c.captions),
      media,
    });
  }
  const s = sealed.story;
  return {
    slides,
    audience: s.audience === "close_friends" || s.audience === "custom" ? s.audience : "friends",
    customIds: Array.isArray(s.customIds) ? s.customIds.filter((x): x is string => typeof x === "string") : [],
    allowReplies: s.allowReplies !== false,
    ttlSeconds: typeof s.ttlSeconds === "number" ? s.ttlSeconds : 86400,
  };
}

export async function deleteDraft(id: string): Promise<void> {
  await withStore("readwrite", (s) => s.delete(id));
}
