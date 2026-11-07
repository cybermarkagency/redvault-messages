// Produces story_wire_vectors.json with the web app's own code, so the Android app's story
// parsing, key opening and media decryption are pinned to it. Not part of any build: copy it into
// ui/src/test/ as makeAndroidStoryVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeAndroidStoryVectors.test.ts
//
// and delete the copy. The output lands next to this file (android/app/src/test/resources/vectors/).
//
// What it holds: a feed exactly as the server would hand it to a viewer (bob), made of stories
// sealed by their author (alice) with the web's own functions, among them ones an older Android app
// must skip (an `audio` story, a kind nobody knows yet, a story with an overlay type nobody knows)
// and ones every app must refuse (a key made for another story, a changed byte, a kind that does not
// match). `webVisible` is what the web's own decodeFeed makes of that feed: the Android parser must
// agree with it on every story it shows. Everything is fixed (ids, keys, times) except the random
// nonces libsodium picks, and those are stored in the output.
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { encryptFile } from "../lib/fileCrypto.js";
import { decodeFeed, type StoryFriend } from "../lib/stories.js";
import type { FeedDto, StoryDto } from "../lib/storiesApi.js";
import {
  openStoryPrefs,
  parseStickers,
  sealStoryMeta,
  sealStoryPrefs,
  wrapStoryKey,
  type StoryMeta,
} from "../lib/storyCrypto.js";

const OUT = resolve(process.cwd(), "../android/app/src/test/resources/vectors/story_wire_vectors.json");

/** jsdom's Blob has no arrayBuffer(); FileReader works everywhere. */
function readBlob(blob: Blob): Promise<Uint8Array> {
  return new Promise((resolveBlob, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolveBlob(new Uint8Array(reader.result as ArrayBuffer));
    reader.onerror = () => reject(reader.error);
    reader.readAsArrayBuffer(blob);
  });
}

/** The plaintext of a made-up photo or video: byte i is (i * 7 + 3) mod 256, so the test can rebuild it. */
const filler = (length: number) => Uint8Array.from({ length }, (_, i) => (i * 7 + 3) & 0xff);

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const b64 = crypto.toBase64;
  const pair = (seedByte: number) => {
    const kp = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(seedByte));
    return { publicKey: kp.publicKey, privateKey: kp.privateKey };
  };
  const alice = pair(11); // the author
  const bob = pair(12); // the viewer
  const carol = pair(13); // another friend of bob's
  const mallory = pair(14); // not bob's friend
  const ids = {
    alice: "00000000-0000-4000-8000-0000000000a1",
    bob: "00000000-0000-4000-8000-0000000000b2",
    carol: "00000000-0000-4000-8000-0000000000c3",
    mallory: "00000000-0000-4000-8000-0000000000d4",
  };
  const story = (n: number) => `5a000000-0000-4000-8000-${String(n).padStart(12, "0")}`;
  const keyFor = (n: number) => new Uint8Array(32).map((_, i) => (n * 37 + i * 5 + 1) & 0xff);
  const T0 = Date.UTC(2026, 9, 20, 12, 0, 0);
  const iso = (ms: number) => new Date(ms).toISOString();

  // --- media: encrypted with the web's encryptFile, stored as base64 -------------------------------
  const media: Record<string, { ciphertext: string; plaintextLength: number; plaintextSha256: string }> = {};
  const mediaRef = async (storyId: string, length: number, mime: string, extra: Record<string, unknown>) => {
    const plain = filler(length);
    const enc = await encryptFile(new Blob([plain.slice().buffer]));
    media[storyId] = {
      ciphertext: b64(await readBlob(enc.blob)),
      plaintextLength: length,
      plaintextSha256: sodium.to_hex(sodium.crypto_hash_sha256(plain)),
    };
    return {
      key: b64(enc.key),
      header: b64(enc.header),
      sha256: enc.ciphertextSha256,
      encryptedSize: enc.encryptedSize,
      chunkSize: enc.chunkSize,
      mime,
      ...extra,
    };
  };

  // A story as the server hands it to `viewer` (who holds `wrapFor`'s copy of the key).
  const dto = (
    n: number,
    opts: { author?: keyof typeof ids; kind: string; meta: unknown; mediaSize?: number | null; wrapFor?: { publicKey: Uint8Array }; wrapId?: string; seen?: boolean; senderKey?: Uint8Array; flip?: boolean; allowReplies?: boolean }
  ): StoryDto & { seen: boolean } => {
    const id = story(n);
    const key = keyFor(n);
    const authorKeys = opts.senderKey ? { privateKey: opts.senderKey } : alice;
    const sealed = sealStoryMeta(id, key, opts.meta as StoryMeta);
    const ciphertext = sealed.ciphertext.slice();
    if (opts.flip) ciphertext[ciphertext.length - 1] ^= 1;
    return {
      id,
      authorId: ids[opts.author ?? "alice"],
      kind: opts.kind as StoryDto["kind"],
      metaCiphertext: b64(ciphertext),
      metaNonce: b64(sealed.nonce),
      mediaSize: opts.mediaSize ?? null,
      hasMedia: opts.mediaSize != null,
      allowReplies: opts.allowReplies ?? true,
      createdAt: iso(T0 - (100 - n) * 60_000),
      expiresAt: iso(T0 + 20 * 3600_000),
      wrappedKey: b64(wrapStoryKey(opts.wrapId ?? id, key, (opts.wrapFor ?? bob).publicKey, authorKeys.privateKey)),
      seen: opts.seen ?? false,
    };
  };

  const textOverlay = { type: "text", text: "Golden hour 🌅", x: 0.5, y: 0.3, scale: 1.2, rotation: 356, color: "yellow", boxed: true, font: "modern", anim: "pulse" };
  const emojiOverlay = { type: "emoji", text: "🔥", x: 0.75, y: 0.62, scale: 1.3, rotation: 12, color: "white", boxed: false, font: "classic", anim: "bounce" };
  const plainOverlay = { type: "text", text: "plain", x: 0.1, y: 0.9, scale: 1, rotation: 0, color: "red", boxed: false, font: "strong" };
  const stickers = [
    { id: "locabc1234", type: "location", x: 0.4, y: 0.8, scale: 1, rotation: 0, label: "Lisbon" },
    { id: "gifabc1234", type: "gif", x: 0.5, y: 0.5, scale: 1, rotation: 0, url: "https://example.com/a.gif" },
    { id: "pollabc123", type: "poll", x: 0.5, y: 0.5, scale: 1, rotation: 0, question: "Tacos?", options: ["Yes", "No"], results: "everyone" },
  ];
  const captions = [
    { startMs: 1500, endMs: 3000, text: "second line" },
    { startMs: 0, endMs: 1400, text: "first line" },
    { startMs: 5, endMs: 5, text: "empty span, dropped" },
    { startMs: 100, endMs: 200, text: "" },
  ];
  const waveform = Array.from({ length: 32 }, (_, i) => (i * 3) % 101);

  // Stories 1..: what bob's tray holds from alice.
  const imageId = story(2);
  const videoId = story(3);
  const audioId = story(4);
  const imageMedia = await mediaRef(imageId, 70_000, "image/jpeg", { width: 1080, height: 1920 });
  const videoMedia = await mediaRef(videoId, 1_000, "video/mp4", { width: 720, height: 1280, durationMs: 12_000 });
  const audioMedia = await mediaRef(audioId, 3_000, "audio/webm", { width: 1, height: 1, durationMs: 9_000 });

  const aliceStories: (StoryDto & { seen: boolean })[] = [
    dto(1, { kind: "text", meta: { v: 1, kind: "text", text: "Good morning ☀️", background: "ocean", font: "typewriter", overlays: [textOverlay, emojiOverlay], closeFriends: true }, mediaSize: null }),
    dto(2, { kind: "image", meta: { v: 1, kind: "image", overlays: [plainOverlay], stickers, captions, closeFriends: false, media: imageMedia }, mediaSize: imageMedia.encryptedSize, seen: true }),
    dto(3, { kind: "video", meta: { v: 1, kind: "video", overlays: [], captions, closeFriends: false, media: videoMedia }, mediaSize: videoMedia.encryptedSize }),
    // A voice story: shown by the web, skipped by an app that does not know the kind.
    dto(4, { kind: "audio", meta: { v: 1, kind: "audio", text: "Voice note", background: "night", font: "classic", waveform, captions, overlays: [], closeFriends: false, media: audioMedia }, mediaSize: audioMedia.encryptedSize }),
    // A kind nobody has built yet, from a newer app.
    dto(5, { kind: "reel", meta: { v: 1, kind: "reel", overlays: [], closeFriends: false, effects: ["sparkle"] }, mediaSize: null }),
    // Fields no app reads yet, and look names this app has not heard of: the story still shows.
    dto(6, {
      kind: "text",
      meta: {
        v: 1,
        kind: "text",
        text: "Future proof",
        background: "aurora",
        font: "handwriting",
        futureField: { anything: [1, 2, 3] },
        layout: "grid",
        overlays: [
          { type: "text", text: "odd look", x: 0.2, y: 0.2, scale: 9, rotation: -30, color: "chartreuse", boxed: "yes", font: "gothic", anim: "spin", sticker3d: true },
          { type: "emoji", text: "🙂", x: 0.8, y: 0.8, scale: 0.1, rotation: 725, anim: "none" },
          { type: "text", text: "fading", x: 0.5, y: 0.5, scale: "big", rotation: "spin", color: "teal", font: "strong", anim: "fade" },
        ],
        closeFriends: "maybe",
      },
      mediaSize: null,
    }),
    // An overlay type nobody knows: the web refuses the whole story (and so does this app).
    dto(7, { kind: "text", meta: { v: 1, kind: "text", text: "Banner story", background: "sunset", font: "classic", overlays: [{ type: "banner", text: "NEW", x: 0.5, y: 0.5 }], closeFriends: false }, mediaSize: null }),
    // A key made for another story, replayed onto this one.
    dto(8, { kind: "text", meta: { v: 1, kind: "text", text: "Replayed key", background: "sunset", font: "classic", overlays: [], closeFriends: false }, mediaSize: null, wrapId: story(1) }),
    // A byte changed on the way.
    dto(9, { kind: "text", meta: { v: 1, kind: "text", text: "Tampered", background: "sunset", font: "classic", overlays: [], closeFriends: false }, mediaSize: null, flip: true }),
    // The server says text, the sealed details say image.
    dto(10, { kind: "text", meta: { v: 1, kind: "image", overlays: [], closeFriends: false, media: imageMedia }, mediaSize: null }),
    // A key sealed by someone who is not the author.
    dto(11, { kind: "text", meta: { v: 1, kind: "text", text: "Forged", background: "sunset", font: "classic", overlays: [], closeFriends: false }, mediaSize: null, senderKey: mallory.privateKey }),
    // A story that claims to be someone else's inside alice's group.
    { ...dto(12, { kind: "text", meta: { v: 1, kind: "text", text: "Wrong author", background: "sunset", font: "classic", overlays: [], closeFriends: false }, mediaSize: null }), authorId: ids.carol },
    // Details that are not an object, and a text story with no text.
    dto(13, { kind: "text", meta: "just a string", mediaSize: null }),
    dto(14, { kind: "text", meta: { v: 1, kind: "text", text: "   ", background: "sunset", font: "classic", overlays: [], closeFriends: false }, mediaSize: null }),
    // A photo with a mime type a photo cannot have.
    dto(15, { kind: "image", meta: { v: 1, kind: "image", overlays: [], closeFriends: false, media: { ...imageMedia, mime: "image/svg+xml" } }, mediaSize: imageMedia.encryptedSize }),
  ];
  const carolStories = [dto(20, { author: "carol", kind: "text", meta: { v: 1, kind: "text", text: "Hi from Carol", background: "forest", font: "classic", overlays: [], closeFriends: false }, mediaSize: null, senderKey: carol.privateKey, allowReplies: false })];
  const malloryStories = [dto(30, { author: "mallory", kind: "text", meta: { v: 1, kind: "text", text: "Not a friend", background: "forest", font: "classic", overlays: [], closeFriends: false }, mediaSize: null, senderKey: mallory.privateKey })];

  const feed: FeedDto = {
    own: [],
    authors: [
      { authorId: ids.alice, stories: aliceStories },
      { authorId: ids.carol, stories: carolStories },
      { authorId: ids.mallory, stories: malloryStories },
    ],
  };
  // Extra, unknown fields a newer server might add, at every level.
  const wireFeed = JSON.parse(JSON.stringify(feed));
  wireFeed.nextPageToken = "abc";
  wireFeed.authors[0].verified = true;
  wireFeed.authors[0].stories[0].reactions = [];

  const friends = new Map<string, StoryFriend>([
    [ids.alice, { userId: ids.alice, handle: "alice", avatarUrl: null, publicKey: alice.publicKey }],
    [ids.carol, { userId: ids.carol, handle: "carol", avatarUrl: null, publicKey: carol.publicKey }],
  ]);
  const decoded = decodeFeed(feed, { userId: ids.bob, publicKey: bob.publicKey, privateKey: bob.privateKey }, friends);
  const webVisible = decoded.authors.flatMap((a) => a.stories.map((s) => ({ id: s.id, authorId: s.authorId, kind: s.kind, allowReplies: s.allowReplies, seen: s.seen, createdAt: s.createdAt, expiresAt: s.expiresAt, key: b64(s.key), meta: s.meta })));
  // The web shows these; the others must stay hidden by every app.
  expect(webVisible.map((s) => s.id)).toEqual([story(1), story(2), story(3), story(4), story(6), story(20)]);

  // --- alice's own stories: sealed to herself --------------------------------------------------------
  const ownDto = { ...dto(40, { kind: "text", meta: { v: 1, kind: "text", text: "My own story", background: "mint", font: "strong", overlays: [plainOverlay], closeFriends: false }, mediaSize: null, wrapFor: alice }), viewCount: 3 };
  delete (ownDto as { seen?: boolean }).seen;
  const ownDecoded = decodeFeed({ own: [ownDto], authors: [] }, { userId: ids.alice, publicKey: alice.publicKey, privateKey: alice.privateKey }, new Map());
  expect(ownDecoded.own).toHaveLength(1);

  // --- prefs: clean and sloppy ----------------------------------------------------------------------------
  const cleanPrefs = { closeFriends: [ids.carol], hideFrom: [ids.mallory], muted: [], shareViews: false, allowReplies: true };
  const sealedClean = sealStoryPrefs(cleanPrefs, bob.privateKey);
  const upper = "00000000-0000-4000-8000-0000000000C3".toUpperCase();
  const sloppyPrefs = { closeFriends: [upper, ids.carol, "not-a-uuid", 5, null], hideFrom: [ids.mallory, ids.mallory], muted: "nobody", shareViews: "no", allowReplies: false, somethingNew: { a: 1 } };
  const sealedSloppy = sealStoryPrefs(sloppyPrefs as never, bob.privateKey);
  const openedSloppy = openStoryPrefs(sealedSloppy.ciphertext, sealedSloppy.nonce, bob.privateKey);

  // --- reference outputs the Kotlin code must reproduce -------------------------------------------------
  const stickerCase = parseStickers(stickers);
  expect(stickerCase.map((s) => s.type)).toEqual(["location", "poll"]);

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        note: "Made by android/app/src/test/resources/vectors/make_android_story_vectors.ts with the web app's own code.",
        ids,
        keys: {
          alice: { publicKey: b64(alice.publicKey), privateKey: b64(alice.privateKey) },
          bob: { publicKey: b64(bob.publicKey), privateKey: b64(bob.privateKey) },
          carol: { publicKey: b64(carol.publicKey), privateKey: b64(carol.privateKey) },
          mallory: { publicKey: b64(mallory.publicKey), privateKey: b64(mallory.privateKey) },
        },
        friendsOfBob: [
          { userId: ids.alice, username: "alice", publicKey: b64(alice.publicKey), avatarUrl: null },
          { userId: ids.carol, username: "carol", publicKey: b64(carol.publicKey), avatarUrl: null },
        ],
        feedForBob: wireFeed,
        webVisible,
        skippedByAndroid: [{ id: story(4), why: "audio kind" }],
        refusedEverywhere: [story(5), story(7), story(8), story(9), story(10), story(11), story(12), story(13), story(14), story(15), story(30)],
        media,
        ownFeed: {
          dto: { own: [ownDto], authors: [] },
          webVisible: ownDecoded.own.map((s) => ({ id: s.id, kind: s.kind, key: b64(s.key), viewCount: s.viewCount, meta: s.meta })),
        },
        prefs: {
          clean: { ciphertext: b64(sealedClean.ciphertext), nonce: b64(sealedClean.nonce), expected: cleanPrefs },
          sloppy: { ciphertext: b64(sealedSloppy.ciphertext), nonce: b64(sealedSloppy.nonce), expected: openedSloppy },
        },
        stickersRaw: stickers,
        stickersExpected: stickerCase,
      },
      null,
      2
    ) + "\n"
  );
  // Keep the reader honest that the file exists and parses.
  expect(JSON.parse(readFileSync(OUT, "utf8")).webVisible).toHaveLength(6);
});
