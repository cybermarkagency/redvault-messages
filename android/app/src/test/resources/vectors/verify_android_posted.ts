// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The other half of the Android <-> web check: opens stories the ANDROID code made with the WEB app's own code.
// The Kotlin test StoryBuilderTest (writeAStoryOfEachKindForTheWebToOpen) writes
// android/app/build/story-android-out/android_posted.json. Not part of any build: copy this file into ui/src/test/
// as verifyAndroidPosted.test.ts, run
//
//   cd ui && npx vitest run src/test/verifyAndroidPosted.test.ts
//
// and delete the copy.
import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { decryptFile } from "../lib/fileCrypto.js";
import { decodeStory } from "../lib/stories.js";
import type { StoryDto } from "../lib/storiesApi.js";

const FILE = resolve(process.cwd(), "../android/app/build/story-android-out/android_posted.json");

/** jsdom's Blob has no arrayBuffer(); FileReader works everywhere. */
function readBlob(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolveBlob, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolveBlob(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

it("the web app opens every story the Android code made", async () => {
  await crypto.ready();
  const data = JSON.parse(readFileSync(FILE, "utf8"));
  const alice = { publicKey: crypto.fromBase64(data.keys.alice.publicKey) };
  const bob = { privateKey: crypto.fromBase64(data.keys.bob.privateKey) };
  const carol = { privateKey: crypto.fromBase64(data.keys.carol.privateKey) };
  const mallory = { privateKey: crypto.fromBase64(data.keys.mallory.privateKey) };
  const by = (name: string) => data.stories.find((s: { name: string }) => s.name === name);
  const dtoFor = (s: Record<string, unknown>, wrappedKey: string): StoryDto => ({
    id: s.id as string,
    authorId: s.authorId as string,
    kind: s.kind as StoryDto["kind"],
    metaCiphertext: s.metaCiphertext as string,
    metaNonce: s.metaNonce as string,
    mediaSize: s.mediaSize as number | null,
    hasMedia: s.hasMedia as boolean,
    allowReplies: s.allowReplies as boolean,
    createdAt: "2026-10-20T12:00:00.000Z",
    expiresAt: "2026-10-21T12:00:00.000Z",
    wrappedKey,
  });

  // Text: bob and carol can open it; the hidden friend (mallory) was given no key at all.
  const text = by("text");
  const bobText = decodeStory(dtoFor(text, text.wrappedKeyForBob), alice.publicKey, bob.privateKey);
  expect(bobText?.meta).toMatchObject({ v: 1, kind: "text", text: "Hello from Android ☀️", background: "ocean", font: "typewriter", overlays: [], closeFriends: false });
  expect(decodeStory(dtoFor(text, text.wrappedKeyForCarol), alice.publicKey, carol.privateKey)).not.toBeNull();
  expect(text.recipients).toEqual([data.ids.alice, data.ids.bob, data.ids.carol]); // mallory is on the hide-from list
  // Someone else's key does not open it.
  expect(decodeStory(dtoFor(text, text.wrappedKeyForBob), alice.publicKey, mallory.privateKey)).toBeNull();

  // Close friends: only carol (and the author); the meta says so, so the ring is green.
  const close = by("close");
  expect(close.wrappedKeyForBob).toBeNull();
  expect(decodeStory(dtoFor(close, close.wrappedKeyForCarol), alice.publicKey, carol.privateKey)?.meta.closeFriends).toBe(true);

  // Photo and video: the key and header in the sealed details open the encrypted file the Android code made.
  for (const [name, length, mime] of [["photo", data.photoLength, "image/jpeg"], ["video", data.videoLength, "video/mp4"]] as const) {
    const s = by(name);
    const story = decodeStory(dtoFor(s, s.wrappedKeyForBob), alice.publicKey, bob.privateKey);
    expect(story, name).not.toBeNull();
    const media = story!.meta.media!;
    expect(media.mime).toBe(mime);
    expect(media.chunkSize).toBe(65536);
    expect(media.encryptedSize).toBe(s.mediaSize);
    const cipher = crypto.fromBase64(s.media);
    const plain = await readBlob(
      await decryptFile([cipher], { key: crypto.fromBase64(media.key), header: crypto.fromBase64(media.header), chunkSize: media.chunkSize, encryptedSize: media.encryptedSize, ciphertextSha256: media.sha256 }, media.mime)
    );
    expect(plain.length).toBe(length);
    const rule = name === "photo" ? (i: number) => (i * 7 + 3) & 0xff : (i: number) => (i * 11 + 1) & 0xff;
    expect(plain.every((b, i) => b === rule(i))).toBe(true);
  }
  expect(decodeStory(dtoFor(by("video"), by("video").wrappedKeyForBob), alice.publicKey, bob.privateKey)?.meta.media?.durationMs).toBe(9000);

  // The server's lifetime choice is carried as a number, outside the sealed details.
  expect(by("ttl").ttlSeconds).toBe(21600);

  // Words, emoji and stickers placed on a story are read back as they were placed.
  const decorated = by("decorated");
  const dec = decodeStory(dtoFor(decorated, decorated.wrappedKeyForBob), alice.publicKey, bob.privateKey)!;
  expect(dec).not.toBeNull();
  expect(dec.meta.overlays).toEqual([
    { type: "text", text: "Shh", x: 0.3, y: 0.2, scale: 1.4, rotation: 10, color: "yellow", boxed: true, font: "strong", anim: "pulse" },
    { type: "emoji", text: "🤫", x: 0.7, y: 0.25, scale: 1.1, rotation: 0, color: "white", boxed: false, font: "classic" },
  ]);
  expect(dec.meta.stickers).toEqual([
    { id: "pollsticker1", type: "poll", x: 0.4, y: 0.6, scale: 1.2, rotation: 5, question: "Secret plan? 🎉", options: ["Yes", "No"], results: "everyone", closesAt: 1_900_000_000_000 },
    { id: "linksticker1", type: "link", x: 0.5, y: 0.85, scale: 1, rotation: 0, url: "https://example.com/party?x=1", label: "Details" },
  ]);

  // Picked people: audience "custom", keys only for the people picked (bob, not carol), and not marked close friends.
  const picked = by("picked");
  expect(picked.audience).toBe("custom");
  expect(picked.recipients).toEqual([data.ids.alice, data.ids.bob]);
  expect(picked.wrappedKeyForCarol).toBeNull();
  expect(decodeStory(dtoFor(picked, picked.wrappedKeyForBob), alice.publicKey, bob.privateKey)?.meta.closeFriends).toBe(false);

  // A video keeps the subtitles it was posted with.
  const captioned = by("captioned");
  const cap = decodeStory(dtoFor(captioned, captioned.wrappedKeyForBob), alice.publicKey, bob.privateKey)!;
  expect(cap.meta.captions).toEqual([
    { startMs: 0, endMs: 1500, text: "first" },
    { startMs: 2000, endMs: 4000, text: "second" },
  ]);
});
