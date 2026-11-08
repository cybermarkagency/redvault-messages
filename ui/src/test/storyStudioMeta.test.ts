// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import * as sapi from "../lib/storiesApi.js";
import { MAX_AUDIO_BYTES, MAX_AUDIO_MS, normalizeMediaMime, prepareAudio, prepareRecordedVideo, publishStory, storyPreview, StoryInputError, type PreparedMedia, type Story } from "../lib/stories.js";
import {
  DEFAULT_STORY_PREFS,
  newStoryId,
  newStoryKey,
  openStoryMeta,
  parseStoryMeta,
  sealStoryMeta,
  unwrapStoryKey,
  type StoryMeta,
} from "../lib/storyCrypto.js";

vi.mock("../lib/storiesApi.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/storiesApi.js")>()),
  createStory: vi.fn(async (s: sapi.NewStoryDto) => ({ id: s.id, published: s.kind === "text", expiresAt: new Date(Date.now() + 86400_000).toISOString() })),
  uploadStoryMedia: vi.fn(async () => {}),
  deleteStory: vi.fn(async () => {}),
}));

type Keys = { publicKey: Uint8Array; privateKey: Uint8Array };
let me: Keys & { userId: string };
let bob: Keys & { userId: string };

beforeAll(async () => {
  await crypto.ready();
  me = { userId: "00000000-0000-4000-8000-00000000000a", ...crypto.generateKeyPair() };
  bob = { userId: "00000000-0000-4000-8000-00000000000b", ...crypto.generateKeyPair() };
});
beforeEach(() => {
  vi.mocked(sapi.createStory).mockClear();
  vi.mocked(sapi.uploadStoryMedia).mockClear();
});

const mediaRef = (over: Record<string, unknown> = {}) => ({
  key: "a2V5a2V5a2V5a2V5",
  header: "aGVhZGVyaGVhZGVy",
  sha256: "a".repeat(64),
  encryptedSize: 5000,
  chunkSize: 65536,
  mime: "audio/webm",
  width: 1,
  height: 1,
  durationMs: 4200,
  ...over,
});
const audioMeta = (over: Record<string, unknown> = {}) => ({ v: 1, kind: "audio", overlays: [], closeFriends: false, media: mediaRef(), ...over });

describe("audio stories", () => {
  it("keep their title, look, picture of the sound and captions through sealing and opening", () => {
    const waveform = Array.from({ length: 48 }, (_, i) => (i * 2) % 101);
    const meta = audioMeta({ text: "Road trip", background: "ocean", font: "modern", waveform, captions: [{ startMs: 0, endMs: 1500, text: "Hello" }] }) as unknown as StoryMeta;
    const id = newStoryId();
    const key = newStoryKey();
    const sealed = sealStoryMeta(id, key, meta);
    const opened = openStoryMeta(id, key, "audio", sealed.ciphertext, sealed.nonce);
    expect(opened).toMatchObject({ kind: "audio", text: "Road trip", background: "ocean", font: "modern", waveform, captions: [{ startMs: 0, endMs: 1500, text: "Hello" }] });
    expect(opened.media).toMatchObject({ mime: "audio/webm", durationMs: 4200, width: 1, height: 1 });
  });

  it("need audio media with a length; a picture or video type in an audio story is refused", () => {
    expect(() => parseStoryMeta(audioMeta({ media: mediaRef({ mime: "video/webm" }) }), "audio")).toThrow();
    expect(() => parseStoryMeta(audioMeta({ media: mediaRef({ mime: "image/jpeg" }) }), "audio")).toThrow();
    expect(() => parseStoryMeta(audioMeta({ media: mediaRef({ mime: "audio/x-evil" }) }), "audio")).toThrow();
    expect(() => parseStoryMeta(audioMeta({ media: mediaRef({ durationMs: undefined }) }), "audio")).toThrow();
    expect(() => parseStoryMeta(audioMeta({ media: mediaRef({ durationMs: 0 }) }), "audio")).toThrow();
    expect(() => parseStoryMeta(audioMeta({ media: undefined }), "audio")).toThrow();
    expect(() => parseStoryMeta(audioMeta({ media: mediaRef({ sha256: "nope" }) }), "audio")).toThrow();
    // And the other way round: an audio type can't pass as a video.
    expect(() => parseStoryMeta({ ...audioMeta({ media: mediaRef({ mime: "audio/webm", width: 100, height: 100 }) }), kind: "video" }, "video")).toThrow();
    // Every type a recording or a file can come as is accepted.
    for (const mime of ["audio/webm", "audio/ogg", "audio/mp4", "audio/mpeg", "audio/wav", "audio/aac"]) expect(() => parseStoryMeta(audioMeta({ media: mediaRef({ mime }) }), "audio"), mime).not.toThrow();
  });

  it("a title is optional, and a story with no look gets the night background", () => {
    const meta = parseStoryMeta(audioMeta(), "audio");
    expect(meta.text).toBeUndefined();
    expect(meta.background).toBe("night");
    expect(meta.font).toBe("classic");
    expect(parseStoryMeta(audioMeta({ text: "x".repeat(701), background: "nope" }), "audio")).toMatchObject({ background: "night" });
    expect(parseStoryMeta(audioMeta({ text: "x".repeat(701) }), "audio").text).toBeUndefined();
  });

  it("a picture of the sound must be a sane list of 0-100 whole numbers, or it is left out", () => {
    const ok = Array.from({ length: 16 }, () => 50);
    expect(parseStoryMeta(audioMeta({ waveform: ok }), "audio").waveform).toEqual(ok);
    for (const bad of [[1, 2, 3], Array.from({ length: 97 }, () => 1), Array.from({ length: 20 }, () => 101), Array.from({ length: 20 }, () => -1), Array.from({ length: 20 }, () => 0.5), "bars", null]) {
      expect(parseStoryMeta(audioMeta({ waveform: bad }), "audio").waveform).toBeUndefined();
    }
  });

  it("show in a reply as their title, or as a voice message", () => {
    const story = (meta: StoryMeta): Story => ({ id: "x", authorId: "y", kind: "audio", meta, key: new Uint8Array(32), createdAt: 0, expiresAt: 1, allowReplies: true, hasMedia: true, seen: false });
    expect(storyPreview(story(parseStoryMeta(audioMeta({ text: "Road trip" }), "audio")))).toBe("Road trip");
    expect(storyPreview(story(parseStoryMeta(audioMeta(), "audio")))).toBe("Voice");
  });
});

describe("captions and animated text on any story", () => {
  const video = (over: Record<string, unknown> = {}) => ({
    v: 1,
    kind: "video",
    overlays: [],
    closeFriends: false,
    media: mediaRef({ mime: "video/webm", width: 720, height: 1280 }),
    ...over,
  });

  it("captions come with a video or audio story only; a photo ignores them", () => {
    const captions = [{ startMs: 0, endMs: 1000, text: "Hi" }];
    expect(parseStoryMeta(video({ captions }), "video").captions).toEqual(captions);
    const photo = { v: 1, kind: "image", overlays: [], closeFriends: false, media: mediaRef({ mime: "image/jpeg", width: 10, height: 10, durationMs: undefined }), captions };
    expect(parseStoryMeta(photo, "image").captions).toBeUndefined();
    expect(parseStoryMeta({ v: 1, kind: "text", text: "hi", overlays: [], closeFriends: false, captions }, "text").captions).toBeUndefined();
    expect(parseStoryMeta(video({ captions: [] }), "video").captions).toBeUndefined();
  });

  it("an animation on text or an emoji is kept when it is one of the known ones, and dropped when not", () => {
    const overlay = (anim: unknown) => ({ type: "text", text: "Hi", x: 0.5, y: 0.5, scale: 1, rotation: 0, color: "white", boxed: false, font: "classic", anim });
    for (const anim of ["pulse", "bounce", "fade"]) expect(parseStoryMeta(video({ overlays: [overlay(anim)] }), "video").overlays[0].anim, anim).toBe(anim);
    for (const anim of ["spin", "none", 3, null, undefined]) expect(parseStoryMeta(video({ overlays: [overlay(anim)] }), "video").overlays[0].anim, String(anim)).toBeUndefined();
  });
});

describe("preparing media", () => {
  it("folds a type's spellings together and drops the codec details", () => {
    expect(normalizeMediaMime("video/webm;codecs=vp9,opus")).toBe("video/webm");
    expect(normalizeMediaMime("Audio/WEBM; codecs=opus")).toBe("audio/webm");
    expect(normalizeMediaMime("audio/x-wav")).toBe("audio/wav");
    expect(normalizeMediaMime("audio/x-m4a")).toBe("audio/mp4");
    expect(normalizeMediaMime("audio/mp3")).toBe("audio/mpeg");
    expect(normalizeMediaMime("image/jpeg")).toBe("image/jpeg");
  });

  it("a recording's length comes from the recording, and its type is cleaned up", async () => {
    const audio = await prepareAudio(new Blob([new Uint8Array(1000)], { type: "audio/webm;codecs=opus" }), 4200, [1, 2, 3]);
    expect(audio).toMatchObject({ kind: "audio", mime: "audio/webm", durationMs: 4200, width: 1, height: 1, waveform: [1, 2, 3] });
    const video = prepareRecordedVideo(new Blob([new Uint8Array(1000)], { type: "video/webm;codecs=vp8,opus" }), 7300.4, 720, 1280);
    expect(video).toMatchObject({ kind: "video", mime: "video/webm", durationMs: 7300, width: 720, height: 1280 });
  });

  it("refuses what a story can't carry, in plain words", async () => {
    await expect(prepareAudio(new Blob(["x"], { type: "audio/flac" }), 1000)).rejects.toThrow(StoryInputError);
    await expect(prepareAudio(new Blob(["x"], { type: "video/mp4" }), 1000)).rejects.toThrow(/MP3, M4A/);
    await expect(prepareAudio(new Blob(["x"], { type: "audio/webm" }), 0)).rejects.toThrow(/couldn't be opened/);
    await expect(prepareAudio(new Blob(["x"], { type: "audio/webm" }), MAX_AUDIO_MS + 5000)).rejects.toThrow(/5 minutes/);
    const big = { type: "audio/mpeg", size: MAX_AUDIO_BYTES + 1 } as Blob;
    await expect(prepareAudio(big, 1000)).rejects.toThrow(/too large/);
    expect(() => prepareRecordedVideo(new Blob(["x"], { type: "video/x-matroska" }), 1000, 1, 1)).toThrow(/can't use/);
    expect(() => prepareRecordedVideo(new Blob(["x"], { type: "video/webm" }), 0, 1, 1)).toThrow(StoryInputError);
    expect(() => prepareRecordedVideo({ type: "video/webm", size: 61 * 1024 * 1024 } as Blob, 1000, 1, 1)).toThrow(/too large/);
  });

  it("a recording of exactly the longest length is kept, one past it (plus the grace) is not", async () => {
    expect((await prepareAudio(new Blob(["x"], { type: "audio/webm" }), MAX_AUDIO_MS)).durationMs).toBe(MAX_AUDIO_MS);
    expect((await prepareAudio(new Blob(["x"], { type: "audio/webm" }), MAX_AUDIO_MS + 400)).durationMs).toBe(MAX_AUDIO_MS);
  });
});

describe("posting an audio story", () => {
  // Made inside the tests: `bob` doesn't exist until beforeAll has run.
  const bobAsFriend = () => ({ userId: bob.userId, handle: "bob", avatarUrl: null, publicKey: bob.publicKey });
  const audio = (): PreparedMedia => ({ kind: "audio", blob: new Blob([new Uint8Array(3000).fill(9)], { type: "audio/wav" }), mime: "audio/wav", width: 1, height: 1, durationMs: 3000, waveform: Array.from({ length: 48 }, () => 40) });

  it("encrypts the recording, seals the details for friends, and announces the kind and size", async () => {
    await publishStory(me, { media: audio(), text: "My day", overlays: [], audience: "friends", allowReplies: true }, [bobAsFriend()], DEFAULT_STORY_PREFS);
    const sent = vi.mocked(sapi.createStory).mock.calls[0][0];
    expect(sent.kind).toBe("audio");
    const uploaded = vi.mocked(sapi.uploadStoryMedia).mock.calls[0][1];
    expect(uploaded.size).toBe(sent.mediaSize);
    expect(sent.mediaSize).toBeGreaterThan(3000);
    const key = unwrapStoryKey(sent.id, crypto.fromBase64(sent.keys.find((k) => k.userId === bob.userId)!.wrappedKey), me.publicKey, bob.privateKey);
    const meta = openStoryMeta(sent.id, key, "audio", crypto.fromBase64(sent.metaCiphertext), crypto.fromBase64(sent.metaNonce));
    expect(meta).toMatchObject({ kind: "audio", text: "My day", background: "night", media: { mime: "audio/wav", durationMs: 3000 } });
    expect(meta.waveform).toHaveLength(48);
    // The server was given neither the title nor the picture of the sound.
    expect(JSON.stringify(sent)).not.toContain("My day");
  });

  it("an audio story needs a length", async () => {
    const noLength = { ...audio(), durationMs: undefined };
    await expect(publishStory(me, { media: noLength, overlays: [], audience: "friends", allowReplies: true }, [], DEFAULT_STORY_PREFS)).rejects.toThrow(StoryInputError);
    expect(sapi.createStory).not.toHaveBeenCalled();
  });

  it("captions travel with a video, inside the encrypted details, and not with a photo", async () => {
    const captions = [{ startMs: 0, endMs: 1000, text: "Hi there" }];
    const clip: PreparedMedia = { kind: "video", blob: new Blob([new Uint8Array(500)], { type: "video/webm" }), mime: "video/webm", width: 720, height: 1280, durationMs: 4000 };
    await publishStory(me, { media: clip, overlays: [], captions, audience: "friends", allowReplies: true }, [], DEFAULT_STORY_PREFS);
    const photo: PreparedMedia = { kind: "image", blob: new Blob([new Uint8Array(500)], { type: "image/jpeg" }), mime: "image/jpeg", width: 10, height: 10 };
    await publishStory(me, { media: photo, overlays: [], captions, audience: "friends", allowReplies: true }, [], DEFAULT_STORY_PREFS);
    const open = (call: number) => {
      const sent = vi.mocked(sapi.createStory).mock.calls[call][0];
      const key = unwrapStoryKey(sent.id, crypto.fromBase64(sent.keys[0].wrappedKey), me.publicKey, me.privateKey);
      return openStoryMeta(sent.id, key, sent.kind, crypto.fromBase64(sent.metaCiphertext), crypto.fromBase64(sent.metaNonce));
    };
    expect(open(0).captions).toEqual(captions);
    expect(open(1).captions).toBeUndefined();
    expect(JSON.stringify(vi.mocked(sapi.createStory).mock.calls[0][0])).not.toContain("Hi there");
  });
});
