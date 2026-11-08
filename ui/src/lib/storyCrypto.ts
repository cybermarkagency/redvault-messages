// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stories, end-to-end encrypted (docs/stories.md).
//
// Each story gets its own random key K. Everything about the story (its
// text, stickers, background, and the key + header of its encrypted photo
// or video) is one JSON blob encrypted with K, bound to the story's id. The
// server stores that blob and the encrypted media, never K.
//
// K reaches each viewer as crypto_box(payload, nonce, viewer's public key,
// author's private key), stored as nonce || box, where
//   payload = "RVST" | version 1 | story id (16 bytes) | K (32 bytes)
// crypto_box is authenticated with the author's key, so a viewer's app
// knows the author made it (the server can't forge a story in someone's
// name), and the id inside stops a key being replayed onto another story.
//
// Interactive stickers (polls, quizzes, questions, sliders, countdowns, links,
// mentions, dates, places) are part of the encrypted details (`stickers`), so
// the server never sees a question. Answers go the other way, sealed to the
// author only ("RVSR" authenticated / "RVSA" without a name), and the author's
// app publishes totals sealed with the story key. See docs/stories-advanced/.
//
// Highlight titles are encrypted with the cover story's key; story settings
// (close friends, hidden from, sharing views, muted) with a key derived
// one-way from the account's private key, like the storage analytics journal.
import sodium from "libsodium-wrappers-sumo";
import { ready } from "./e2eCrypto.js";

const MAGIC = [0x52, 0x56, 0x53, 0x54]; // "RVST"
const VERSION = 1;
const KEY_BYTES = 32;
const ID_BYTES = 16;
const PAYLOAD_BYTES = MAGIC.length + 1 + ID_BYTES + KEY_BYTES;
const META_AD = "redvault-story-meta-v1";
const TITLE_AD = "redvault-story-highlight-v1";
const PREFS_AD = "redvault-story-prefs-v1";

export class StoryCryptoError extends Error {}

export { ready };

function uuidBytes(id: string): Uint8Array {
  const hex = id.replace(/-/g, "").toLowerCase();
  if (!/^[0-9a-f]{32}$/.test(hex)) throw new StoryCryptoError("not a story id");
  const out = new Uint8Array(ID_BYTES);
  for (let i = 0; i < ID_BYTES; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

/**
 * A copy in this realm's Uint8Array: libsodium's own from_string output
 * fails its AEAD type checks in some builds (see e2eCrypto.ts), so messages
 * are passed as strings and additional data as fresh arrays.
 */
function bytes(s: string): Uint8Array {
  const src = sodium.from_string(s);
  const out = new Uint8Array(src.length);
  out.set(src);
  return out;
}

function ad(label: string, id: string): Uint8Array {
  const tag = bytes(label);
  const out = new Uint8Array(tag.length + ID_BYTES);
  out.set(tag, 0);
  out.set(uuidBytes(id), tag.length);
  return out;
}

/** A fresh random story id (v4 UUID); the app picks it so the key can be bound to it before upload. */
export function newStoryId(): string {
  const b = sodium.randombytes_buf(16);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const hex = sodium.to_hex(b);
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

export function newStoryKey(): Uint8Array {
  return sodium.crypto_aead_xchacha20poly1305_ietf_keygen();
}

/** The bound payload (exported for tests). */
export function storyKeyPayload(storyId: string, key: Uint8Array): Uint8Array {
  if (key.length !== KEY_BYTES) throw new StoryCryptoError("story keys are 32 bytes");
  const out = new Uint8Array(PAYLOAD_BYTES);
  out.set(MAGIC, 0);
  out[4] = VERSION;
  out.set(uuidBytes(storyId), 5);
  out.set(key, 5 + ID_BYTES);
  return out;
}

/** Seals a story's key to one viewer (the author's own copy is sealed to themself). */
export function wrapStoryKey(storyId: string, key: Uint8Array, recipientPublicKey: Uint8Array, authorPrivateKey: Uint8Array): Uint8Array {
  const nonce = sodium.randombytes_buf(sodium.crypto_box_NONCEBYTES);
  const box = sodium.crypto_box_easy(storyKeyPayload(storyId, key), nonce, recipientPublicKey, authorPrivateKey);
  const out = new Uint8Array(nonce.length + box.length);
  out.set(nonce, 0);
  out.set(box, nonce.length);
  return out;
}

/** Opens a viewer's copy, checking the author made it and that it's for exactly this story. */
export function unwrapStoryKey(storyId: string, wrapped: Uint8Array, authorPublicKey: Uint8Array, viewerPrivateKey: Uint8Array): Uint8Array {
  if (wrapped.length !== sodium.crypto_box_NONCEBYTES + PAYLOAD_BYTES + sodium.crypto_box_MACBYTES) throw new StoryCryptoError("malformed story key");
  let payload: Uint8Array;
  try {
    payload = sodium.crypto_box_open_easy(
      wrapped.subarray(sodium.crypto_box_NONCEBYTES),
      wrapped.subarray(0, sodium.crypto_box_NONCEBYTES),
      authorPublicKey,
      viewerPrivateKey
    );
  } catch {
    throw new StoryCryptoError("this story key wasn't made by its author for you");
  }
  if (!sameBytes(payload.subarray(0, 4), new Uint8Array(MAGIC)) || payload[4] !== VERSION) throw new StoryCryptoError("unknown story key format");
  if (!sameBytes(payload.subarray(5, 5 + ID_BYTES), uuidBytes(storyId))) throw new StoryCryptoError("this key belongs to another story");
  return payload.slice(5 + ID_BYTES);
}

// --- What a story shows -----------------------------------------------------------------

/** Named colors only, so every app draws a story the same way (tokens.css --story-*). */
export const STORY_COLORS = ["white", "black", "red", "orange", "yellow", "green", "teal", "blue", "purple", "pink"] as const;
export type StoryColor = (typeof STORY_COLORS)[number];
export const STORY_BACKGROUNDS = ["sunset", "ocean", "forest", "berry", "night", "ember", "mint", "slate"] as const;
export type StoryBackground = (typeof STORY_BACKGROUNDS)[number];
export const STORY_FONTS = ["classic", "modern", "typewriter", "strong"] as const;
export type StoryFont = (typeof STORY_FONTS)[number];

export const MAX_OVERLAYS = 30;
export const MAX_OVERLAY_TEXT = 300;
export const MAX_STORY_TEXT = 700;

/** How a text or emoji overlay moves (nothing when the viewer asks for reduced motion). */
export const STORY_ANIMATIONS = ["none", "pulse", "bounce", "fade"] as const;
export type StoryAnimation = (typeof STORY_ANIMATIONS)[number];

/** Text or an emoji sticker placed on a story; x and y are the centre, as fractions of the frame. */
export interface StoryOverlay {
  type: "text" | "emoji";
  text: string;
  x: number;
  y: number;
  /** 0.5-4, relative to the default size. */
  scale: number;
  /** Degrees. */
  rotation: number;
  color: StoryColor;
  /** Text only: a solid box behind it. */
  boxed: boolean;
  font: StoryFont;
  /** Optional: apps from before animated text show it still. */
  anim?: StoryAnimation;
}

export interface StoryMediaRef {
  /** base64: the file key and secretstream header (lib/fileCrypto.ts). */
  key: string;
  header: string;
  sha256: string;
  encryptedSize: number;
  chunkSize: number;
  mime: string;
  width: number;
  height: number;
  /** Video only. */
  durationMs?: number;
}

// --- Interactive stickers ----------------------------------------------------------------

export const MAX_STICKERS = 12;
export const MAX_STICKER_TEXT = 120;
export const MAX_STICKER_OPTION = 40;
export const MAX_QUESTION_ANSWER = 500;
export const MAX_LINK_LENGTH = 300;
export const STICKER_ID_PATTERN = /^[a-z0-9]{6,16}$/;

export interface StickerPlacement {
  /** 6-16 characters of a-z0-9; the server only ever sees this id. */
  id: string;
  /** Centre, as fractions of the frame. */
  x: number;
  y: number;
  scale: number;
  rotation: number;
}

/** Who sees a sticker's totals: everyone who can see the story, or only its author. */
export type ResultsVisibility = "everyone" | "author";

export interface PollSticker extends StickerPlacement {
  type: "poll";
  question: string;
  options: string[];
  /** Epoch ms; voting stops then (when left out, until the story ends). */
  closesAt?: number;
  results: ResultsVisibility;
}
export interface QuizSticker extends StickerPlacement {
  type: "quiz";
  question: string;
  options: string[];
  /** Index into options. Viewers' apps hold it too (they hold the story key): it isn't a secret from them. */
  correct: number;
  explanation?: string;
  results: ResultsVisibility;
}
export interface QuestionSticker extends StickerPlacement {
  type: "question";
  prompt: string;
  /** Answers arrive without the sender's name (the server still knows who sent them). */
  anonymous: boolean;
}
export interface SliderSticker extends StickerPlacement {
  type: "slider";
  prompt: string;
  emoji: string;
  results: ResultsVisibility;
}
export interface CountdownSticker extends StickerPlacement {
  type: "countdown";
  title: string;
  endsAt: number;
}
export interface LinkSticker extends StickerPlacement {
  type: "link";
  url: string;
  label: string;
}
export interface MentionSticker extends StickerPlacement {
  type: "mention";
  userId: string;
}
export interface DateSticker extends StickerPlacement {
  type: "date";
  at: number;
  style: "date" | "time" | "datetime";
}
/** A place the author typed. Never read from the device. */
export interface LocationSticker extends StickerPlacement {
  type: "location";
  label: string;
}

export type StorySticker = PollSticker | QuizSticker | QuestionSticker | SliderSticker | CountdownSticker | LinkSticker | MentionSticker | DateSticker | LocationSticker;
export type AnswerableSticker = PollSticker | QuizSticker | QuestionSticker | SliderSticker;

export const isAnswerable = (s: StorySticker): s is AnswerableSticker => s.type === "poll" || s.type === "quiz" || s.type === "question" || s.type === "slider";

/** A fresh sticker id (a-z0-9, 12 characters). */
export function newStickerId(): string {
  const alphabet = "abcdefghijklmnopqrstuvwxyz0123456789";
  const b = sodium.randombytes_buf(12);
  let out = "";
  for (let i = 0; i < 12; i++) out += alphabet[b[i] % alphabet.length];
  return out;
}

/** Control characters and bidi overrides out, whitespace tidied: sticker text is shown as plain text only. */
export function cleanStickerText(v: unknown, max: number): string | null {
  if (typeof v !== "string") return null;
  // eslint-disable-next-line no-control-regex
  const t = v.replace(/[\u0000-\u001f\u007f-\u009f\u202a-\u202e\u2066-\u2069]/g, " ").replace(/\s+/g, " ").trim();
  return t && t.length <= max ? t : null;
}

/**
 * A link a story may carry: https only, no credentials, a real host. The host is shown as the
 * browser spells it (punycode for look-alike letters) and a viewer confirms before leaving.
 */
export function safeStoryLink(raw: unknown): string | null {
  if (typeof raw !== "string" || raw.length > MAX_LINK_LENGTH) return null;
  let u: URL;
  try {
    u = new URL(raw.trim());
  } catch {
    return null;
  }
  if (u.protocol !== "https:" || u.username || u.password || !u.hostname || !u.hostname.includes(".")) return null;
  return u.toString();
}

const isOptions = (v: unknown): v is string[] => Array.isArray(v) && v.length >= 2 && v.length <= 4;

function parseSticker(raw: unknown): StorySticker | null {
  if (!raw || typeof raw !== "object") return null;
  const r = raw as Record<string, unknown>;
  if (typeof r.id !== "string" || !STICKER_ID_PATTERN.test(r.id)) return null;
  if (!isFraction(r.x) || !isFraction(r.y)) return null;
  const place: StickerPlacement = {
    id: r.id,
    x: r.x,
    y: r.y,
    scale: typeof r.scale === "number" && Number.isFinite(r.scale) ? Math.min(2.5, Math.max(0.6, r.scale)) : 1,
    rotation: typeof r.rotation === "number" && Number.isFinite(r.rotation) ? ((r.rotation % 360) + 360) % 360 : 0,
  };
  const visibility: ResultsVisibility = r.results === "author" ? "author" : "everyone";
  const when = (v: unknown) => (typeof v === "number" && Number.isFinite(v) && v > 0 && v < 8.64e15 ? v : null);
  switch (r.type) {
    case "poll":
    case "quiz": {
      const question = cleanStickerText(r.question, MAX_STICKER_TEXT);
      if (!question || !isOptions(r.options)) return null;
      const options = r.options.map((o) => cleanStickerText(o, MAX_STICKER_OPTION));
      if (options.some((o) => o === null)) return null;
      const opts = options as string[];
      if (r.type === "poll") {
        const closesAt = when(r.closesAt);
        return { ...place, type: "poll", question, options: opts, results: visibility, ...(closesAt ? { closesAt } : {}) };
      }
      if (typeof r.correct !== "number" || !Number.isInteger(r.correct) || r.correct < 0 || r.correct >= opts.length) return null;
      const explanation = r.explanation === undefined ? null : cleanStickerText(r.explanation, MAX_STICKER_TEXT);
      return { ...place, type: "quiz", question, options: opts, correct: r.correct, results: visibility, ...(explanation ? { explanation } : {}) };
    }
    case "question": {
      const prompt = cleanStickerText(r.prompt, MAX_STICKER_TEXT);
      return prompt ? { ...place, type: "question", prompt, anonymous: r.anonymous === true } : null;
    }
    case "slider": {
      const prompt = cleanStickerText(r.prompt, MAX_STICKER_TEXT);
      const emoji = cleanStickerText(r.emoji, 8);
      return prompt && emoji ? { ...place, type: "slider", prompt, emoji, results: visibility } : null;
    }
    case "countdown": {
      const title = cleanStickerText(r.title, 60);
      const endsAt = when(r.endsAt);
      return title && endsAt ? { ...place, type: "countdown", title, endsAt } : null;
    }
    case "link": {
      const url = safeStoryLink(r.url);
      const label = cleanStickerText(r.label, 40);
      return url && label ? { ...place, type: "link", url, label } : null;
    }
    case "mention":
      return typeof r.userId === "string" && /^[0-9a-f-]{36}$/i.test(r.userId) ? { ...place, type: "mention", userId: r.userId.toLowerCase() } : null;
    case "date": {
      const at = when(r.at);
      return at && (r.style === "date" || r.style === "time" || r.style === "datetime") ? { ...place, type: "date", at, style: r.style } : null;
    }
    case "location": {
      const label = cleanStickerText(r.label, 60);
      return label ? { ...place, type: "location", label } : null;
    }
    default:
      // A sticker type from a newer app: left out, the rest of the story still shows.
      return null;
  }
}

/** Drops anything malformed or unknown (one bad sticker never hides a story); duplicates of an id are dropped too. */
export function parseStickers(raw: unknown): StorySticker[] {
  if (!Array.isArray(raw)) return [];
  const seen = new Set<string>();
  const out: StorySticker[] = [];
  for (const item of raw.slice(0, MAX_STICKERS)) {
    const sticker = parseSticker(item);
    if (sticker && !seen.has(sticker.id)) {
      seen.add(sticker.id);
      out.push(sticker);
    }
  }
  return out;
}

// --- Captions and waveform -------------------------------------------------------------------------

export const MAX_CAPTIONS = 60;
export const MAX_CAPTION_TEXT = 120;
export const MAX_CAPTION_END_MS = 10 * 60_000;

/** One line of subtitle text shown over a video or audio story between two times. */
export interface StoryCaption {
  startMs: number;
  endMs: number;
  text: string;
}

/** Keeps the lines that make sense (a real span, readable text), in time order, and drops the rest. */
export function parseCaptions(raw: unknown): StoryCaption[] {
  if (!Array.isArray(raw)) return [];
  const out: StoryCaption[] = [];
  for (const c of raw.slice(0, MAX_CAPTIONS)) {
    if (!c || typeof c !== "object") continue;
    const r = c as Record<string, unknown>;
    const text = cleanStickerText(r.text, MAX_CAPTION_TEXT);
    if (!text) continue;
    if (typeof r.startMs !== "number" || typeof r.endMs !== "number" || !Number.isFinite(r.startMs) || !Number.isFinite(r.endMs)) continue;
    const startMs = Math.round(r.startMs);
    const endMs = Math.round(r.endMs);
    if (startMs < 0 || endMs <= startMs || endMs > MAX_CAPTION_END_MS) continue;
    out.push({ startMs, endMs, text });
  }
  return out.sort((a, b) => a.startMs - b.startMs);
}

/** The line to show at `timeMs`, if any. */
export function captionAt(captions: readonly StoryCaption[], timeMs: number): string | null {
  for (const c of captions) if (timeMs >= c.startMs && timeMs < c.endMs) return c.text;
  return null;
}

export const WAVEFORM_MIN = 16;
export const WAVEFORM_MAX = 96;

/** A coarse picture of an audio story's loudness, 0-100 per bar, so it can be drawn before it plays. */
export function parseWaveform(raw: unknown): number[] | undefined {
  if (!Array.isArray(raw) || raw.length < WAVEFORM_MIN || raw.length > WAVEFORM_MAX) return undefined;
  if (!raw.every((v) => typeof v === "number" && Number.isInteger(v) && v >= 0 && v <= 100)) return undefined;
  return raw as number[];
}

export interface StoryMeta {
  v: 1;
  kind: "image" | "video" | "text" | "audio";
  /** A text story's words and look. */
  text?: string;
  background?: StoryBackground;
  font?: StoryFont;
  overlays: StoryOverlay[];
  media?: StoryMediaRef;
  /** Interactive and informational stickers. Optional: apps from before stickers ignore it and show the rest. */
  stickers?: StorySticker[];
  /** Subtitles for a video or audio story. */
  captions?: StoryCaption[];
  /** Audio stories: the loudness bars. */
  waveform?: number[];
  /** Shared with close friends only (shown with a green ring). */
  closeFriends: boolean;
}

const isColor = (v: unknown): v is StoryColor => typeof v === "string" && (STORY_COLORS as readonly string[]).includes(v);
const isBackground = (v: unknown): v is StoryBackground => typeof v === "string" && (STORY_BACKGROUNDS as readonly string[]).includes(v);
const isFont = (v: unknown): v is StoryFont => typeof v === "string" && (STORY_FONTS as readonly string[]).includes(v);
const isAnimation = (v: unknown): v is StoryAnimation => typeof v === "string" && (STORY_ANIMATIONS as readonly string[]).includes(v);
const isFraction = (v: unknown): v is number => typeof v === "number" && Number.isFinite(v) && v >= 0 && v <= 1;
const isB64 = (v: unknown, max: number): v is string => typeof v === "string" && v.length > 0 && v.length <= max && /^[A-Za-z0-9+/_=-]+$/.test(v);
const posInt = (v: unknown): v is number => typeof v === "number" && Number.isSafeInteger(v) && v > 0;

function parseOverlay(o: unknown): StoryOverlay | null {
  if (!o || typeof o !== "object") return null;
  const r = o as Record<string, unknown>;
  if (r.type !== "text" && r.type !== "emoji") return null;
  if (typeof r.text !== "string" || !r.text || r.text.length > MAX_OVERLAY_TEXT) return null;
  if (!isFraction(r.x) || !isFraction(r.y)) return null;
  const scale = typeof r.scale === "number" && Number.isFinite(r.scale) ? Math.min(4, Math.max(0.5, r.scale)) : 1;
  const rotation = typeof r.rotation === "number" && Number.isFinite(r.rotation) ? ((r.rotation % 360) + 360) % 360 : 0;
  return {
    type: r.type,
    text: r.text,
    x: r.x,
    y: r.y,
    scale,
    rotation,
    color: isColor(r.color) ? r.color : "white",
    boxed: r.boxed === true,
    font: isFont(r.font) ? r.font : "classic",
    ...(isAnimation(r.anim) && r.anim !== "none" ? { anim: r.anim } : {}),
  };
}

/** Validates decrypted details; anything malformed is refused rather than half-shown. */
export function parseStoryMeta(raw: unknown, kind: StoryMeta["kind"]): StoryMeta {
  if (!raw || typeof raw !== "object") throw new StoryCryptoError("malformed story");
  const r = raw as Record<string, unknown>;
  if (r.v !== 1 || r.kind !== kind) throw new StoryCryptoError("malformed story");
  const overlays = Array.isArray(r.overlays) ? r.overlays.slice(0, MAX_OVERLAYS).map(parseOverlay) : [];
  if (overlays.some((o) => o === null)) throw new StoryCryptoError("malformed story");
  const meta: StoryMeta = { v: 1, kind, overlays: overlays as StoryOverlay[], closeFriends: r.closeFriends === true };
  const stickers = parseStickers(r.stickers);
  if (stickers.length) meta.stickers = stickers;
  if (kind === "text") {
    if (typeof r.text !== "string" || !r.text.trim() || r.text.length > MAX_STORY_TEXT) throw new StoryCryptoError("malformed story");
    meta.text = r.text;
    meta.background = isBackground(r.background) ? r.background : "sunset";
    meta.font = isFont(r.font) ? r.font : "classic";
  } else {
    const m = r.media as Record<string, unknown> | undefined;
    if (!m || typeof m !== "object") throw new StoryCryptoError("malformed story");
    if (!isB64(m.key, 64) || !isB64(m.header, 64) || typeof m.sha256 !== "string" || !/^[0-9a-f]{64}$/.test(m.sha256)) throw new StoryCryptoError("malformed story");
    // An audio story has no picture: its width and height are 1.
    if (!posInt(m.encryptedSize) || !posInt(m.chunkSize) || (kind !== "audio" && (!posInt(m.width) || !posInt(m.height)))) throw new StoryCryptoError("malformed story");
    const mime = typeof m.mime === "string" ? m.mime : "";
    const allowed = kind === "image" ? /^image\/(jpeg|png|webp)$/ : kind === "audio" ? /^audio\/(webm|ogg|mp4|mpeg|wav|aac)$/ : /^video\/(mp4|webm|quicktime)$/;
    if (!allowed.test(mime)) throw new StoryCryptoError("malformed story");
    meta.media = {
      key: m.key,
      header: m.header,
      sha256: m.sha256,
      encryptedSize: m.encryptedSize,
      chunkSize: m.chunkSize,
      mime,
      width: kind === "audio" ? 1 : (m.width as number),
      height: kind === "audio" ? 1 : (m.height as number),
    };
    if (kind === "video" || kind === "audio") {
      if (!posInt(m.durationMs)) throw new StoryCryptoError("malformed story");
      meta.media.durationMs = m.durationMs;
    }
    if (kind === "audio") {
      meta.background = isBackground(r.background) ? r.background : "night";
      meta.font = isFont(r.font) ? r.font : "classic";
      // A title is optional on an audio story.
      if (typeof r.text === "string" && r.text.trim() && r.text.length <= MAX_STORY_TEXT) meta.text = r.text;
      const waveform = parseWaveform(r.waveform);
      if (waveform) meta.waveform = waveform;
    }
    const captions = parseCaptions(r.captions);
    if (captions.length && kind !== "image") meta.captions = captions;
  }
  return meta;
}

export function sealStoryMeta(storyId: string, key: Uint8Array, meta: StoryMeta): { ciphertext: Uint8Array; nonce: Uint8Array } {
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(JSON.stringify(meta), ad(META_AD, storyId), null, nonce, key);
  return { ciphertext, nonce };
}

export function openStoryMeta(storyId: string, key: Uint8Array, kind: StoryMeta["kind"], ciphertext: Uint8Array, nonce: Uint8Array): StoryMeta {
  let plain: Uint8Array;
  try {
    plain = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, ciphertext, ad(META_AD, storyId), nonce, key);
  } catch {
    throw new StoryCryptoError("this story can't be opened");
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(sodium.to_string(plain));
  } catch {
    throw new StoryCryptoError("malformed story");
  }
  return parseStoryMeta(parsed, kind);
}

// --- Highlight titles -------------------------------------------------------------------

export const MAX_HIGHLIGHT_TITLE = 40;

/** Encrypted with the cover story's key: whoever can see the cover can read the title. */
export function sealHighlightTitle(title: string, coverStoryId: string, coverKey: Uint8Array): { ciphertext: Uint8Array; nonce: Uint8Array } {
  const t = title.trim().slice(0, MAX_HIGHLIGHT_TITLE);
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  return { ciphertext: sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(t, ad(TITLE_AD, coverStoryId), null, nonce, coverKey), nonce };
}

/** Null when it can't be read (e.g. this viewer can't see the cover story). */
export function openHighlightTitle(ciphertext: Uint8Array, nonce: Uint8Array, coverStoryId: string, coverKey: Uint8Array): string | null {
  try {
    const t = sodium.to_string(sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, ciphertext, ad(TITLE_AD, coverStoryId), nonce, coverKey));
    return t.slice(0, MAX_HIGHLIGHT_TITLE);
  } catch {
    return null;
  }
}

// --- Story settings (synced between your devices, unreadable by the server) -------------

export interface StoryPrefs {
  /** userIds who see "Close friends" stories. */
  closeFriends: string[];
  /** userIds who never get your stories. */
  hideFrom: string[];
  /** userIds whose stories you've muted (they stay out of your tray). */
  muted: string[];
  /** Let people see that you viewed their story. Off: your app never tells the server. */
  shareViews: boolean;
  /** New stories allow replies by default. */
  allowReplies: boolean;
}

export const DEFAULT_STORY_PREFS: StoryPrefs = { closeFriends: [], hideFrom: [], muted: [], shareViews: true, allowReplies: true };

function prefsKey(privateKey: Uint8Array): Uint8Array {
  // A dedicated, one-way-derived key: the private key itself is never used directly.
  return sodium.crypto_generichash(32, "redvault-story-prefs-v1", privateKey);
}

const uuidList = (v: unknown): string[] =>
  Array.isArray(v) ? [...new Set(v.filter((x): x is string => typeof x === "string" && /^[0-9a-f-]{36}$/i.test(x)).map((x) => x.toLowerCase()))].slice(0, 5000) : [];

export function sealStoryPrefs(prefs: StoryPrefs, privateKey: Uint8Array): { ciphertext: Uint8Array; nonce: Uint8Array } {
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  return { ciphertext: sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(JSON.stringify(prefs), bytes(PREFS_AD), null, nonce, prefsKey(privateKey)), nonce };
}

export function openStoryPrefs(ciphertext: Uint8Array, nonce: Uint8Array, privateKey: Uint8Array): StoryPrefs {
  let raw: Record<string, unknown>;
  try {
    const plain = sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, ciphertext, bytes(PREFS_AD), nonce, prefsKey(privateKey));
    raw = JSON.parse(sodium.to_string(plain)) as Record<string, unknown>;
  } catch {
    throw new StoryCryptoError("your story settings can't be opened");
  }
  return {
    closeFriends: uuidList(raw.closeFriends),
    hideFrom: uuidList(raw.hideFrom),
    muted: uuidList(raw.muted),
    shareViews: raw.shareViews !== false,
    allowReplies: raw.allowReplies !== false,
  };
}

// --- Answers to stickers (viewer -> author) ----------------------------------------------------
//
// An answer is sealed so that only the story's author can read it:
//   authenticated ("RVSR"): nonce || crypto_box(payload, authorPublic, responderPrivate)
//     the author knows exactly who sent it (their key made it) and so does the sender, who can open
//     their own answer again on another device;
//   without a name ("RVSA"): crypto_box_seal(payload, authorPublic), no sender key inside at all.
// payload = magic (4) | version 1 | story id (16) | sticker id length (1) | sticker id | answer JSON.
// The story and sticker ids inside stop a server moving an answer onto another story or sticker.

const ANSWER_MAGIC = [0x52, 0x56, 0x53, 0x52]; // "RVSR"
const ANON_MAGIC = [0x52, 0x56, 0x53, 0x41]; // "RVSA"
const ANSWER_VERSION = 1;

export type StoryAnswer =
  | { k: "poll"; choice: number }
  | { k: "quiz"; choice: number }
  | { k: "slider"; value: number }
  | { k: "question"; text: string };

function answerPayload(magic: number[], storyId: string, stickerId: string, answer: StoryAnswer): Uint8Array {
  if (!STICKER_ID_PATTERN.test(stickerId)) throw new StoryCryptoError("not a sticker id");
  const id = bytes(stickerId);
  const body = bytes(JSON.stringify(answer));
  const out = new Uint8Array(4 + 1 + ID_BYTES + 1 + id.length + body.length);
  out.set(magic, 0);
  out[4] = ANSWER_VERSION;
  out.set(uuidBytes(storyId), 5);
  out[5 + ID_BYTES] = id.length;
  out.set(id, 6 + ID_BYTES);
  out.set(body, 6 + ID_BYTES + id.length);
  return out;
}

/** Checks an answer against the sticker it is for; anything that doesn't fit is refused, never half-counted. */
export function parseAnswer(raw: unknown, sticker: AnswerableSticker): StoryAnswer {
  if (!raw || typeof raw !== "object") throw new StoryCryptoError("malformed answer");
  const r = raw as Record<string, unknown>;
  if (r.k !== sticker.type) throw new StoryCryptoError("malformed answer");
  switch (sticker.type) {
    case "poll":
    case "quiz":
      if (typeof r.choice !== "number" || !Number.isInteger(r.choice) || r.choice < 0 || r.choice >= sticker.options.length) throw new StoryCryptoError("malformed answer");
      return { k: sticker.type, choice: r.choice };
    case "slider":
      if (typeof r.value !== "number" || !Number.isInteger(r.value) || r.value < 0 || r.value > 100) throw new StoryCryptoError("malformed answer");
      return { k: "slider", value: r.value };
    case "question": {
      const text = typeof r.text === "string" ? r.text.trim() : "";
      if (!text || text.length > MAX_QUESTION_ANSWER) throw new StoryCryptoError("malformed answer");
      return { k: "question", text };
    }
  }
}

function openPayload(payload: Uint8Array, magic: number[], storyId: string, sticker: AnswerableSticker): StoryAnswer {
  if (payload.length < 6 + ID_BYTES) throw new StoryCryptoError("malformed answer");
  if (!sameBytes(payload.subarray(0, 4), new Uint8Array(magic)) || payload[4] !== ANSWER_VERSION) throw new StoryCryptoError("unknown answer format");
  if (!sameBytes(payload.subarray(5, 5 + ID_BYTES), uuidBytes(storyId))) throw new StoryCryptoError("this answer belongs to another story");
  const idLen = payload[5 + ID_BYTES];
  const idEnd = 6 + ID_BYTES + idLen;
  if (idEnd > payload.length || sodium.to_string(payload.subarray(6 + ID_BYTES, idEnd)) !== sticker.id) throw new StoryCryptoError("this answer belongs to another sticker");
  let parsed: unknown;
  try {
    parsed = JSON.parse(sodium.to_string(payload.subarray(idEnd)));
  } catch {
    throw new StoryCryptoError("malformed answer");
  }
  return parseAnswer(parsed, sticker);
}

/** An answer the author knows came from this account. The sender can open it again too (openAnswer with the author's public key). */
export function sealAnswer(storyId: string, sticker: AnswerableSticker, answer: StoryAnswer, authorPublicKey: Uint8Array, responderPrivateKey: Uint8Array): Uint8Array {
  const checked = parseAnswer(answer, sticker);
  const nonce = sodium.randombytes_buf(sodium.crypto_box_NONCEBYTES);
  const box = sodium.crypto_box_easy(answerPayload(ANSWER_MAGIC, storyId, sticker.id, checked), nonce, authorPublicKey, responderPrivateKey);
  const out = new Uint8Array(nonce.length + box.length);
  out.set(nonce, 0);
  out.set(box, nonce.length);
  return out;
}

/**
 * Opens an authenticated answer. The author passes the responder's public key and their own private
 * key; the responder passes the author's public key and their own private key.
 */
export function openAnswer(storyId: string, sticker: AnswerableSticker, envelope: Uint8Array, otherPartyPublicKey: Uint8Array, ownPrivateKey: Uint8Array): StoryAnswer {
  if (envelope.length <= sodium.crypto_box_NONCEBYTES + sodium.crypto_box_MACBYTES) throw new StoryCryptoError("malformed answer");
  let payload: Uint8Array;
  try {
    payload = sodium.crypto_box_open_easy(envelope.subarray(sodium.crypto_box_NONCEBYTES), envelope.subarray(0, sodium.crypto_box_NONCEBYTES), otherPartyPublicKey, ownPrivateKey);
  } catch {
    throw new StoryCryptoError("this answer wasn't made by that person for this author");
  }
  return openPayload(payload, ANSWER_MAGIC, storyId, sticker);
}

/** An answer with no sender inside (a question marked "without my name"). */
export function sealAnonymousAnswer(storyId: string, sticker: AnswerableSticker, answer: StoryAnswer, authorPublicKey: Uint8Array): Uint8Array {
  return sodium.crypto_box_seal(answerPayload(ANON_MAGIC, storyId, sticker.id, parseAnswer(answer, sticker)), authorPublicKey);
}

export function openAnonymousAnswer(storyId: string, sticker: AnswerableSticker, envelope: Uint8Array, authorPublicKey: Uint8Array, authorPrivateKey: Uint8Array): StoryAnswer {
  let payload: Uint8Array;
  try {
    payload = sodium.crypto_box_seal_open(envelope, authorPublicKey, authorPrivateKey);
  } catch {
    throw new StoryCryptoError("this answer isn't for this author");
  }
  return openPayload(payload, ANON_MAGIC, storyId, sticker);
}

// --- Totals (author -> everyone who can see the story) -----------------------------------------
//
// Aggregates only, never who answered what. Sealed with the story key, bound to the story, the
// sticker and the revision, so a server can't forge totals or serve them for another sticker. It
// can still serve an older revision (docs/stories-advanced/threat-model.md).

const RESULT_AD = "redvault-story-result-v1";

export interface StickerTotals {
  /** How many answers were counted. */
  total: number;
  /** Poll and quiz: answers per option. */
  counts?: number[];
  /** Quiz: how many chose the right option. */
  correct?: number;
  /** Slider: the mean position, 0-100. */
  average?: number;
}

function resultAd(storyId: string, stickerId: string, revision: number): Uint8Array {
  const tag = bytes(RESULT_AD);
  const id = bytes(stickerId);
  const out = new Uint8Array(tag.length + ID_BYTES + 4 + id.length);
  out.set(tag, 0);
  out.set(uuidBytes(storyId), tag.length);
  new DataView(out.buffer).setUint32(tag.length + ID_BYTES, revision, false);
  out.set(id, tag.length + ID_BYTES + 4);
  return out;
}

export function sealTotals(storyId: string, stickerId: string, revision: number, totals: StickerTotals, storyKey: Uint8Array): { ciphertext: Uint8Array; nonce: Uint8Array } {
  if (!Number.isInteger(revision) || revision < 1) throw new StoryCryptoError("bad revision");
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  return { ciphertext: sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(JSON.stringify(totals), resultAd(storyId, stickerId, revision), null, nonce, storyKey), nonce };
}

const count = (v: unknown): v is number => typeof v === "number" && Number.isSafeInteger(v) && v >= 0 && v <= 1_000_000;

export function openTotals(storyId: string, sticker: AnswerableSticker, revision: number, ciphertext: Uint8Array, nonce: Uint8Array, storyKey: Uint8Array): StickerTotals {
  let raw: Record<string, unknown>;
  try {
    raw = JSON.parse(sodium.to_string(sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, ciphertext, resultAd(storyId, sticker.id, revision), nonce, storyKey))) as Record<string, unknown>;
  } catch {
    throw new StoryCryptoError("these results can't be opened");
  }
  if (!raw || typeof raw !== "object" || !count(raw.total)) throw new StoryCryptoError("malformed results");
  const totals: StickerTotals = { total: raw.total };
  if (sticker.type === "poll" || sticker.type === "quiz") {
    if (!Array.isArray(raw.counts) || raw.counts.length !== sticker.options.length || !raw.counts.every(count)) throw new StoryCryptoError("malformed results");
    totals.counts = raw.counts as number[];
    if (sticker.type === "quiz") {
      if (!count(raw.correct)) throw new StoryCryptoError("malformed results");
      totals.correct = raw.correct;
    }
  } else if (sticker.type === "slider") {
    if (typeof raw.average !== "number" || !Number.isFinite(raw.average) || raw.average < 0 || raw.average > 100) throw new StoryCryptoError("malformed results");
    totals.average = raw.average;
  }
  return totals;
}
