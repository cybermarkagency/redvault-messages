// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces story_vectors.json with the web app's own code, so the desktop
// app's story crypto and formats are pinned to it byte for byte. Not part of
// any build: copy it into ui/src/test/ as makeStoryVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeStoryVectors.test.ts
//
// and delete the copy. The output lands next to this file (desktop/tests/vectors/).
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { serializeStoryReply } from "../lib/richMessage.js";
import {
  openHighlightTitle,
  openStoryMeta,
  openStoryPrefs,
  sealHighlightTitle,
  sealStoryMeta,
  sealStoryPrefs,
  unwrapStoryKey,
  wrapStoryKey,
  type StoryMeta,
} from "../lib/storyCrypto.js";

// Run from ui/: the output goes to desktop/tests/vectors/ (where this file lives).
const OUT = resolve(process.cwd(), "../desktop/tests/vectors/story_vectors.json");

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const pair = (seedByte: number) => {
    const kp = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(seedByte));
    return { publicKey: kp.publicKey, privateKey: kp.privateKey };
  };
  const alice = pair(11);
  const bob = pair(12);
  const b64 = crypto.toBase64;

  const storyId = "5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6";
  const coverId = "0e9d8c7b-6a5f-4e3d-8c2b-1a0f9e8d7c6b";
  const key = new Uint8Array(32).map((_, i) => i + 1);
  const wrapped = wrapStoryKey(storyId, key, bob.publicKey, alice.privateKey);
  expect(unwrapStoryKey(storyId, wrapped, alice.publicKey, bob.privateKey)).toEqual(key);

  const meta: StoryMeta = {
    v: 1,
    kind: "image",
    overlays: [
      { type: "text", text: "Golden hour 🌅", x: 0.5, y: 0.3, scale: 1.2, rotation: 356, color: "yellow", boxed: true, font: "modern" },
      { type: "emoji", text: "🔥", x: 0.75, y: 0.62, scale: 1.3, rotation: 12, color: "white", boxed: false, font: "classic" },
    ],
    closeFriends: true,
    media: {
      key: b64(new Uint8Array(32).fill(7)),
      header: b64(new Uint8Array(24).fill(9)),
      sha256: "ab".repeat(32),
      encryptedSize: 65553,
      chunkSize: 65536,
      mime: "image/jpeg",
      width: 1080,
      height: 1920,
    },
  };
  const sealedMeta = sealStoryMeta(storyId, key, meta);
  expect(openStoryMeta(storyId, key, "image", sealedMeta.ciphertext, sealedMeta.nonce)).toEqual(meta);

  const coverKey = new Uint8Array(32).fill(3);
  const title = sealHighlightTitle("Summer 2026 ☀️", coverId, coverKey);
  expect(openHighlightTitle(title.ciphertext, title.nonce, coverId, coverKey)).toBe("Summer 2026 ☀️");

  const prefs = { closeFriends: ["00000000-0000-4000-8000-00000000000b"], hideFrom: ["00000000-0000-4000-8000-00000000000c"], muted: [], shareViews: false, allowReplies: true };
  const sealedPrefs = sealStoryPrefs(prefs, alice.privateKey);
  expect(openStoryPrefs(sealedPrefs.ciphertext, sealedPrefs.nonce, alice.privateKey)).toEqual(prefs);

  const replyBody = serializeStoryReply("Love this 😍", {
    authorId: "00000000-0000-4000-8000-00000000000A",
    storyId,
    kind: "image",
    preview: "Photo",
    reaction: false,
  });

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        note: "Made by desktop/tests/vectors/make_story_vectors.ts with the web app's own code.",
        alice: { publicKey: b64(alice.publicKey), privateKey: b64(alice.privateKey) },
        bob: { publicKey: b64(bob.publicKey), privateKey: b64(bob.privateKey) },
        storyId,
        key: b64(key),
        wrappedForBob: b64(wrapped),
        metaJson: JSON.stringify(meta),
        metaCiphertext: b64(sealedMeta.ciphertext),
        metaNonce: b64(sealedMeta.nonce),
        coverId,
        coverKey: b64(coverKey),
        titleCiphertext: b64(title.ciphertext),
        titleNonce: b64(title.nonce),
        title: "Summer 2026 ☀️",
        prefsCiphertext: b64(sealedPrefs.ciphertext),
        prefsNonce: b64(sealedPrefs.nonce),
        storyReplyBody: replyBody.slice(replyBody.indexOf("{")),
      },
      null,
      2
    ) + "\n"
  );
});
