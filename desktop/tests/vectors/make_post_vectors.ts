// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces post_vectors.json with the web app's own code, so the desktop
// app's exclusive-post crypto and body format are pinned to it byte for
// byte. Not part of any build: copy it into ui/src/test/ as
// makePostVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makePostVectors.test.ts
//
// and delete the copy. The output lands next to this file (desktop/tests/vectors/).
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { openPostBody, sealPostBody, unwrapPostKey, wrapPostKey, type PostBody } from "../lib/postCrypto.js";

// Run from ui/: the output goes to desktop/tests/vectors/ (where this file lives).
const OUT = resolve(process.cwd(), "../desktop/tests/vectors/post_vectors.json");

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const pair = (seedByte: number) => {
    const kp = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(seedByte));
    return { publicKey: kp.publicKey, privateKey: kp.privateKey };
  };
  const alice = pair(21);
  const bob = pair(22);
  const b64 = crypto.toBase64;

  const postId = "7a3e9c1d-4b2f-4e6a-8d5c-0f1e2d3c4b5a";
  const key = new Uint8Array(32).map((_, i) => 200 - i);

  const wrappedForBob = wrapPostKey(postId, key, bob.publicKey, alice.privateKey);
  const wrappedForAlice = wrapPostKey(postId, key, alice.publicKey, alice.privateKey);
  expect(unwrapPostKey(postId, wrappedForBob, alice.publicKey, bob.privateKey)).toEqual(key);

  const textBody: PostBody = { v: 1, text: "Behind the scenes, part 2 — thanks for the support ✨" };
  const mediaBody: PostBody = {
    v: 1,
    text: "",
    media: {
      key: b64(new Uint8Array(32).fill(7)),
      header: b64(new Uint8Array(24).fill(9)),
      sha256: "ab".repeat(32),
      encryptedSize: 65553,
      chunkSize: 65536,
      mime: "video/mp4",
    },
  };
  const sealedText = sealPostBody(postId, key, textBody);
  const sealedMedia = sealPostBody(postId, key, mediaBody);
  expect(openPostBody(postId, key, sealedText.ciphertext, sealedText.nonce)).toEqual(textBody);

  const vectors = {
    postId,
    alice: { publicKey: b64(alice.publicKey), privateKey: b64(alice.privateKey) },
    bob: { publicKey: b64(bob.publicKey), privateKey: b64(bob.privateKey) },
    key: b64(key),
    wrappedForBob: b64(wrappedForBob),
    wrappedForAlice: b64(wrappedForAlice),
    textBody: { ciphertext: b64(sealedText.ciphertext), nonce: b64(sealedText.nonce), json: JSON.stringify(textBody) },
    mediaBody: { ciphertext: b64(sealedMedia.ciphertext), nonce: b64(sealedMedia.nonce), json: JSON.stringify(mediaBody) },
  };
  writeFileSync(OUT, `${JSON.stringify(vectors, null, 2)}\n`);
});
