// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stories on this device: opening the feed, posting, and keeping keys
// topped up for friends added later. The crypto is in lib/storyCrypto.ts;
// the wire format in lib/storiesApi.ts; the design in docs/stories.md.
import * as crypto from "./e2eCrypto.js";
import { decryptFile, encryptFile } from "./fileCrypto.js";
import * as sapi from "./storiesApi.js";
import {
  DEFAULT_STORY_PREFS,
  newStoryId,
  newStoryKey,
  openHighlightTitle,
  openStoryMeta,
  openStoryPrefs,
  sealHighlightTitle,
  sealStoryMeta,
  sealStoryPrefs,
  unwrapStoryKey,
  wrapStoryKey,
  type StoryMeta,
  type StoryCaption,
  type StoryOverlay,
  type StoryPrefs,
  type StorySticker,
} from "./storyCrypto.js";

export interface Story {
  id: string;
  authorId: string;
  kind: sapi.StoryKind;
  meta: StoryMeta;
  key: Uint8Array;
  createdAt: number;
  expiresAt: number;
  allowReplies: boolean;
  hasMedia: boolean;
  /** Feed only: already seen (shared view, or seen on this device). */
  seen: boolean;
  /** Your own stories: how many friends shared that they saw it. */
  viewCount?: number;
  /** Your archive: when it left your story. */
  archivedAt?: number | null;
}

export interface StoryIdentity {
  userId: string;
  publicKey: Uint8Array;
  privateKey: Uint8Array;
}

/** A friend as stories need them: who they are and the key their stories are sealed with. */
export interface StoryFriend {
  userId: string;
  handle: string;
  avatarUrl: string | null;
  publicKey: Uint8Array;
}

/** Opens one story; null if it can't be (not made by its author for you, or malformed). */
export function decodeStory(dto: sapi.StoryDto, authorPublicKey: Uint8Array, viewerPrivateKey: Uint8Array, extra: Partial<Story> = {}): Story | null {
  try {
    const key = unwrapStoryKey(dto.id, crypto.fromBase64(dto.wrappedKey), authorPublicKey, viewerPrivateKey);
    const meta = openStoryMeta(dto.id, key, dto.kind, crypto.fromBase64(dto.metaCiphertext), crypto.fromBase64(dto.metaNonce));
    return {
      id: dto.id,
      authorId: dto.authorId,
      kind: dto.kind,
      meta,
      key,
      createdAt: new Date(dto.createdAt).getTime(),
      expiresAt: new Date(dto.expiresAt).getTime(),
      allowReplies: dto.allowReplies,
      hasMedia: dto.hasMedia,
      seen: false,
      ...extra,
    };
  } catch {
    return null;
  }
}

// --- Seen on this device ---------------------------------------------------------------
// With "share that I viewed" off nothing reaches the server, so rings are
// also kept grey from this device's own record (ids only, dropped after 2 days).

const SEEN_PREFIX = "redvaultsystem:stories-seen:";
const SEEN_KEEP_MS = 48 * 3600_000;

export function readSeen(userId: string): Map<string, number> {
  try {
    const raw = localStorage.getItem(SEEN_PREFIX + userId);
    const parsed = raw ? (JSON.parse(raw) as Record<string, number>) : {};
    const now = Date.now();
    return new Map(Object.entries(parsed).filter(([, at]) => typeof at === "number" && now - at < SEEN_KEEP_MS));
  } catch {
    return new Map();
  }
}

export function rememberSeen(userId: string, storyId: string): void {
  const seen = readSeen(userId);
  seen.set(storyId, Date.now());
  try {
    localStorage.setItem(SEEN_PREFIX + userId, JSON.stringify(Object.fromEntries(seen)));
  } catch {
    // Best effort: the ring just stays coloured on this device.
  }
}

export interface AuthorStories {
  author: StoryFriend;
  stories: Story[];
  allSeen: boolean;
}

export interface DecodedFeed {
  own: Story[];
  authors: AuthorStories[];
}

/** Opens the whole feed; stories that can't be opened (or from people who aren't friends any more) are left out. */
export function decodeFeed(feed: sapi.FeedDto, identity: StoryIdentity, friends: Map<string, StoryFriend>): DecodedFeed {
  const seenHere = readSeen(identity.userId);
  const own = feed.own
    .map((dto) => decodeStory(dto, identity.publicKey, identity.privateKey, { viewCount: dto.viewCount, seen: true }))
    .filter((s): s is Story => s !== null)
    .sort((a, b) => a.createdAt - b.createdAt);
  const authors: AuthorStories[] = [];
  for (const group of feed.authors) {
    const author = friends.get(group.authorId);
    if (!author) continue;
    const stories = group.stories
      .map((dto) => decodeStory(dto, author.publicKey, identity.privateKey, { seen: dto.seen || seenHere.has(dto.id) }))
      .filter((s): s is Story => s !== null && s.authorId === group.authorId)
      .sort((a, b) => a.createdAt - b.createdAt);
    if (stories.length) authors.push({ author, stories, allSeen: stories.every((s) => s.seen) });
  }
  // People with something new first (the server's order otherwise: most recent first).
  authors.sort((a, b) => Number(a.allSeen) - Number(b.allSeen));
  return { own, authors };
}

// --- Media ------------------------------------------------------------------------------

const mediaCache = new Map<string, Promise<string>>();
const MEDIA_CACHE_MAX = 40;

/** The decrypted photo or video as a blob: URL (kept in memory for this tab only). */
export function storyMediaUrl(story: Story): Promise<string> {
  const cached = mediaCache.get(story.id);
  if (cached) return cached;
  const media = story.meta.media;
  if (!media) return Promise.reject(new Error("This story has no media"));
  const p = (async () => {
    const stream = await sapi.downloadStoryMedia(story.id);
    const blob = await decryptFile(
      stream,
      {
        key: crypto.fromBase64(media.key),
        header: crypto.fromBase64(media.header),
        chunkSize: media.chunkSize,
        encryptedSize: media.encryptedSize,
        ciphertextSha256: media.sha256,
      },
      media.mime
    );
    return URL.createObjectURL(blob);
  })();
  p.catch(() => mediaCache.delete(story.id));
  mediaCache.set(story.id, p);
  if (mediaCache.size > MEDIA_CACHE_MAX) {
    const [oldestId, oldest] = mediaCache.entries().next().value as [string, Promise<string>];
    mediaCache.delete(oldestId);
    oldest.then((url) => URL.revokeObjectURL(url)).catch(() => {});
  }
  return p;
}

/** Signing out or locking: decrypted media doesn't outlive the session. */
export function clearStoryMedia(): void {
  for (const p of mediaCache.values()) p.then((url) => URL.revokeObjectURL(url)).catch(() => {});
  mediaCache.clear();
}

// --- Preparing what you post ------------------------------------------------------------

export const MAX_IMAGE_SIDE = 1920;
export const MAX_VIDEO_MS = 60_000;
export const MAX_VIDEO_BYTES = 60 * 1024 * 1024;
const VIDEO_TYPES = /^video\/(mp4|webm|quicktime)$/;

export class StoryInputError extends Error {}

export interface PreparedMedia {
  kind: "image" | "video" | "audio";
  blob: Blob;
  mime: string;
  width: number;
  height: number;
  durationMs?: number;
  /** Audio only: loudness bars (0-100) to draw before it plays. */
  waveform?: number[];
}

/** "video/webm;codecs=vp9" is "video/webm"; the other spellings of the same audio types are folded together. */
export function normalizeMediaMime(type: string): string {
  const base = type.split(";")[0].trim().toLowerCase();
  const alias: Record<string, string> = {
    "audio/x-wav": "audio/wav",
    "audio/wave": "audio/wav",
    "audio/vnd.wave": "audio/wav",
    "audio/x-m4a": "audio/mp4",
    "audio/m4a": "audio/mp4",
    "audio/mp3": "audio/mpeg",
    "audio/x-aac": "audio/aac",
  };
  return alias[base] ?? base;
}

export const MAX_AUDIO_MS = 5 * 60_000;
/** Below the server's 25 MiB for a recording, which also covers the encryption overhead. */
export const MAX_AUDIO_BYTES = 24 * 1024 * 1024;
const AUDIO_TYPES = /^audio\/(webm|ogg|mp4|mpeg|wav|aac)$/;

/**
 * Photos are redrawn onto a canvas and saved fresh as JPEG, which leaves
 * every bit of their metadata (location, camera, time) behind.
 */
export async function prepareImage(file: Blob): Promise<PreparedMedia> {
  if (!/^image\//.test(file.type)) throw new StoryInputError("That isn't a photo.");
  let bitmap: ImageBitmap;
  try {
    bitmap = await createImageBitmap(file, { imageOrientation: "from-image" });
  } catch {
    throw new StoryInputError("That photo couldn't be opened.");
  }
  const scale = Math.min(1, MAX_IMAGE_SIDE / Math.max(bitmap.width, bitmap.height));
  const width = Math.max(1, Math.round(bitmap.width * scale));
  const height = Math.max(1, Math.round(bitmap.height * scale));
  const canvas = document.createElement("canvas");
  canvas.width = width;
  canvas.height = height;
  const ctx = canvas.getContext("2d");
  if (!ctx) throw new StoryInputError("That photo couldn't be prepared.");
  ctx.drawImage(bitmap, 0, 0, width, height);
  bitmap.close();
  const blob = await new Promise<Blob | null>((resolve) => canvas.toBlob(resolve, "image/jpeg", 0.9));
  if (!blob) throw new StoryInputError("That photo couldn't be prepared.");
  return { kind: "image", blob, mime: "image/jpeg", width, height };
}

/**
 * Videos are posted as they are (up to 60 seconds and 60 MB). Unlike
 * photos, a video file's own metadata isn't removed: docs/stories.md says so.
 */
export async function prepareVideo(file: Blob): Promise<PreparedMedia> {
  if (!VIDEO_TYPES.test(normalizeMediaMime(file.type))) throw new StoryInputError("Use an MP4, WebM or MOV video.");
  if (file.size > MAX_VIDEO_BYTES) throw new StoryInputError("That video is too large for a story (at most 60 MB).");
  const url = URL.createObjectURL(file);
  try {
    const info = await new Promise<{ w: number; h: number; ms: number }>((resolve, reject) => {
      const v = document.createElement("video");
      v.preload = "metadata";
      v.muted = true;
      v.onloadedmetadata = () => resolve({ w: v.videoWidth, h: v.videoHeight, ms: Math.round(v.duration * 1000) });
      v.onerror = () => reject(new StoryInputError("That video couldn't be opened."));
      v.src = url;
    });
    if (!Number.isFinite(info.ms) || info.ms <= 0) throw new StoryInputError("That video couldn't be opened.");
    if (info.ms > MAX_VIDEO_MS + 500) throw new StoryInputError("Stories can be up to 60 seconds long. Trim the video first.");
    return { kind: "video", blob: file, mime: normalizeMediaMime(file.type), width: info.w || 1080, height: info.h || 1920, durationMs: Math.min(info.ms, MAX_VIDEO_MS) };
  } finally {
    URL.revokeObjectURL(url);
  }
}

/**
 * A video the camera just recorded. Its length is known from the recording itself: a recorded WebM has
 * no duration in its header, so the file can't be asked.
 */
export function prepareRecordedVideo(blob: Blob, durationMs: number, width: number, height: number): PreparedMedia {
  const mime = normalizeMediaMime(blob.type);
  if (!VIDEO_TYPES.test(mime)) throw new StoryInputError("This browser recorded a video type stories can't use. Choose a video file instead.");
  if (blob.size > MAX_VIDEO_BYTES) throw new StoryInputError("That video is too large for a story (at most 60 MB).");
  if (!Number.isFinite(durationMs) || durationMs <= 0) throw new StoryInputError("That video couldn't be used.");
  return { kind: "video", blob, mime, width: width || 1080, height: height || 1920, durationMs: Math.min(Math.round(durationMs), MAX_VIDEO_MS) };
}

/**
 * Audio for a story: a voice recording or a file. The length is passed in when it is known (a recorded
 * WebM has none in its header); otherwise the file is asked.
 */
export async function prepareAudio(file: Blob, knownDurationMs?: number, waveform?: number[]): Promise<PreparedMedia> {
  const mime = normalizeMediaMime(file.type);
  if (!AUDIO_TYPES.test(mime)) throw new StoryInputError("Use an MP3, M4A, WAV, OGG or WebM audio file.");
  if (file.size > MAX_AUDIO_BYTES) throw new StoryInputError("That audio is too large for a story (at most 24 MB).");
  let ms = knownDurationMs;
  if (ms === undefined) {
    const url = URL.createObjectURL(file);
    try {
      ms = await new Promise<number>((resolve, reject) => {
        const a = document.createElement("audio");
        a.preload = "metadata";
        a.onloadedmetadata = () => resolve(Math.round(a.duration * 1000));
        a.onerror = () => reject(new StoryInputError("That audio couldn't be opened."));
        a.src = url;
      });
    } finally {
      URL.revokeObjectURL(url);
    }
  }
  if (!Number.isFinite(ms) || ms <= 0) throw new StoryInputError("That audio couldn't be opened.");
  if (ms > MAX_AUDIO_MS + 500) throw new StoryInputError("Audio stories can be up to 5 minutes long. Trim it first.");
  return { kind: "audio", blob: file, mime, width: 1, height: 1, durationMs: Math.min(Math.round(ms), MAX_AUDIO_MS), ...(waveform ? { waveform } : {}) };
}

// --- Posting -----------------------------------------------------------------------------

export type StoryAudiencePick = "friends" | "close_friends" | "custom";

/** What is being posted. (Saved, unfinished ones are in lib/storyDrafts.ts.) */
export interface StoryDraft {
  media?: PreparedMedia;
  text?: string;
  background?: StoryMeta["background"];
  font?: StoryMeta["font"];
  overlays: StoryOverlay[];
  stickers?: StorySticker[];
  /** Subtitles for a video or audio story. */
  captions?: StoryCaption[];
  audience: StoryAudiencePick;
  /** For "custom": the friends picked one by one. */
  customIds?: string[];
  allowReplies: boolean;
  /** How long it stays up, in seconds (the server's default is 24 hours). */
  ttlSeconds?: number;
}

/**
 * Who gets a story: your friends minus anyone you hide stories from; only close friends for a
 * close-friends story; only the friends picked one by one for a custom one. Nobody outside your
 * friends ever gets a key, whatever the ids say.
 */
export function storyRecipients(friends: StoryFriend[], prefs: StoryPrefs, audience: StoryDraft["audience"], customIds: readonly string[] = []): StoryFriend[] {
  const hidden = new Set(prefs.hideFrom);
  const close = new Set(prefs.closeFriends);
  const picked = new Set(customIds);
  return friends.filter((f) => !hidden.has(f.userId) && (audience === "friends" || (audience === "close_friends" ? close.has(f.userId) : picked.has(f.userId))));
}

export type PostStage = "encrypting" | "uploading" | "done";

export async function publishStory(
  identity: StoryIdentity,
  draft: StoryDraft,
  friends: StoryFriend[],
  prefs: StoryPrefs,
  onStage?: (stage: PostStage) => void
): Promise<string> {
  await crypto.ready();
  const kind: sapi.StoryKind = draft.media ? draft.media.kind : "text";
  if (kind === "audio" && !draft.media?.durationMs) throw new StoryInputError("That audio has no length.");
  if (kind === "text" && !draft.text?.trim()) throw new StoryInputError("Write something first.");
  const id = newStoryId();
  const key = newStoryKey();
  onStage?.("encrypting");
  const meta: StoryMeta = { v: 1, kind, overlays: draft.overlays, closeFriends: draft.audience === "close_friends" };
  if (draft.stickers?.length) meta.stickers = draft.stickers;
  if (draft.captions?.length && (kind === "video" || kind === "audio")) meta.captions = draft.captions;
  let encrypted: Awaited<ReturnType<typeof encryptFile>> | null = null;
  if (draft.media) {
    encrypted = await encryptFile(draft.media.blob);
    meta.media = {
      key: crypto.toBase64(encrypted.key),
      header: crypto.toBase64(encrypted.header),
      sha256: encrypted.ciphertextSha256,
      encryptedSize: encrypted.encryptedSize,
      chunkSize: encrypted.chunkSize,
      mime: draft.media.mime,
      width: draft.media.width,
      height: draft.media.height,
      ...(draft.media.durationMs ? { durationMs: draft.media.durationMs } : {}),
    };
    if (kind === "audio") {
      meta.background = draft.background ?? "night";
      meta.font = draft.font ?? "classic";
      if (draft.text?.trim()) meta.text = draft.text.trim();
      if (draft.media.waveform) meta.waveform = draft.media.waveform;
    }
  } else {
    meta.text = draft.text!.trim();
    meta.background = draft.background ?? "sunset";
    meta.font = draft.font ?? "classic";
  }
  const sealed = sealStoryMeta(id, key, meta);
  const recipients = storyRecipients(friends, prefs, draft.audience, draft.customIds);
  if (recipients.length === 0 && draft.audience !== "friends") throw new StoryInputError(draft.audience === "custom" ? "Choose at least one friend." : "You have no close friends yet.");
  const keys = [{ userId: identity.userId, publicKey: identity.publicKey }, ...recipients].map((r) => ({
    userId: r.userId,
    wrappedKey: crypto.toBase64(wrapStoryKey(id, key, r.publicKey, identity.privateKey)),
  }));
  onStage?.("uploading");
  await sapi.createStory({
    id,
    kind,
    metaCiphertext: crypto.toBase64(sealed.ciphertext),
    metaNonce: crypto.toBase64(sealed.nonce),
    mediaSize: encrypted ? encrypted.encryptedSize : null,
    // "friends" stories reach friends added later too (fillKeyGaps); close-friends ones don't.
    audience: draft.audience,
    allowReplies: draft.allowReplies,
    ...(draft.ttlSeconds ? { ttlSeconds: draft.ttlSeconds } : {}),
    keys,
  });
  if (encrypted) {
    try {
      await sapi.uploadStoryMedia(id, encrypted.blob);
    } catch (err) {
      await sapi.deleteStory(id).catch(() => {});
      throw err;
    }
  }
  onStage?.("done");
  return id;
}

/**
 * Your "friends" stories (live, or kept in a highlight) and friends who have
 * no key for them yet (added since): seal them one, except anyone you hide
 * stories from. The key used is the one this app knows for that friend; if
 * the server offers a different one, that friend is skipped.
 */
export async function fillKeyGaps(identity: StoryIdentity, ownKeys: Map<string, Uint8Array>, friends: Map<string, StoryFriend>, prefs: StoryPrefs): Promise<number> {
  const gaps = await sapi.getStoryKeyGaps();
  if (gaps.length === 0) return 0;
  let keys = ownKeys;
  if (gaps.some((g) => !keys.has(g.storyId))) {
    // Highlighted stories past their 24 hours are in the archive.
    keys = new Map(keys);
    for (const dto of await sapi.getStoryArchive().catch(() => [])) {
      const s = decodeStory(dto, identity.publicKey, identity.privateKey);
      if (s) keys.set(s.id, s.key);
    }
  }
  const hidden = new Set(prefs.hideFrom);
  let added = 0;
  for (const gap of gaps) {
    const key = keys.get(gap.storyId);
    if (!key) continue;
    const wrapped = gap.users
      .filter((u) => !hidden.has(u.userId))
      .filter((u) => {
        const known = friends.get(u.userId);
        return known !== undefined && crypto.toBase64(known.publicKey) === u.publicKey;
      })
      .map((u) => ({ userId: u.userId, wrappedKey: crypto.toBase64(wrapStoryKey(gap.storyId, key, friends.get(u.userId)!.publicKey, identity.privateKey)) }));
    if (wrapped.length === 0) continue;
    added += (await sapi.addStoryKeys(gap.storyId, wrapped)).added;
  }
  return added;
}

// --- Settings -----------------------------------------------------------------------------

export interface LoadedStoryPrefs {
  prefs: StoryPrefs;
  archiveEnabled: boolean;
  revision: number;
}

export async function loadStoryPrefs(identity: StoryIdentity): Promise<LoadedStoryPrefs> {
  const dto = await sapi.getStoryPrefs();
  let prefs = DEFAULT_STORY_PREFS;
  if (dto.prefsCiphertext && dto.prefsNonce) {
    try {
      prefs = openStoryPrefs(crypto.fromBase64(dto.prefsCiphertext), crypto.fromBase64(dto.prefsNonce), identity.privateKey);
    } catch {
      prefs = DEFAULT_STORY_PREFS;
    }
  }
  return { prefs, archiveEnabled: dto.archiveEnabled, revision: dto.revision };
}

/** Saves settings; on a conflict (another device saved first) the caller reloads and tries again. */
export async function saveStoryPrefs(identity: StoryIdentity, prefs: StoryPrefs, archiveEnabled: boolean, revision: number): Promise<LoadedStoryPrefs> {
  const sealed = sealStoryPrefs(prefs, identity.privateKey);
  const res = await sapi.putStoryPrefs({
    prefsCiphertext: crypto.toBase64(sealed.ciphertext),
    prefsNonce: crypto.toBase64(sealed.nonce),
    archiveEnabled,
    revision,
  });
  return { prefs, archiveEnabled: res.archiveEnabled, revision: res.revision };
}

// --- Highlights ----------------------------------------------------------------------------

export interface HighlightView {
  id: string;
  ownerId: string;
  title: string | null;
  coverStoryId: string;
  stories: Story[];
}

/** Opens someone's highlights: only stories this viewer holds a key for; the title reads only if they can see the cover. */
export function decodeHighlights(dtos: sapi.HighlightDto[], ownerPublicKey: Uint8Array, viewerPrivateKey: Uint8Array): HighlightView[] {
  const out: HighlightView[] = [];
  for (const h of dtos) {
    const stories = h.stories
      .map((d) => decodeStory(d, ownerPublicKey, viewerPrivateKey, { seen: true }))
      .filter((s): s is Story => s !== null && s.authorId === h.ownerId);
    if (stories.length === 0) continue;
    const cover = stories.find((s) => s.id === h.coverStoryId);
    const title = cover ? openHighlightTitle(crypto.fromBase64(h.titleCiphertext), crypto.fromBase64(h.titleNonce), cover.id, cover.key) : null;
    out.push({ id: h.id, ownerId: h.ownerId, title, coverStoryId: cover?.id ?? stories[0].id, stories });
  }
  return out;
}

/** Creates a highlight, or replaces one's title, cover and stories. The title is sealed with the cover's key. */
export async function saveHighlight(highlightId: string | null, title: string, stories: Story[], cover: Story): Promise<string> {
  await crypto.ready();
  const sealed = sealHighlightTitle(title, cover.id, cover.key);
  const body = {
    titleCiphertext: crypto.toBase64(sealed.ciphertext),
    titleNonce: crypto.toBase64(sealed.nonce),
    coverStoryId: cover.id,
    storyIds: stories.map((s) => s.id),
  };
  if (highlightId) return (await sapi.updateHighlight(highlightId, body)).id;
  return (await sapi.createHighlight(body)).id;
}

/** "3h", "12m", "now": how long ago a story went up. */
export function storyAge(createdAt: number, now = Date.now()): string {
  const mins = Math.max(0, Math.floor((now - createdAt) / 60_000));
  if (mins < 1) return "now";
  if (mins < 60) return `${mins}m`;
  const hours = Math.floor(mins / 60);
  if (hours < 24) return `${hours}h`;
  const days = Math.floor(hours / 24);
  return days < 7 ? `${days}d` : `${Math.floor(days / 7)}w`;
}

/** What a reply quotes: "Photo", "Video", or the start of a text story. */
export function storyPreview(story: Story): string {
  if (story.kind === "image") return "Photo";
  if (story.kind === "video") return "Video";
  if (story.kind === "audio") return (story.meta.text ?? "").slice(0, 60) || "Voice";
  return (story.meta.text ?? "").slice(0, 60);
}
