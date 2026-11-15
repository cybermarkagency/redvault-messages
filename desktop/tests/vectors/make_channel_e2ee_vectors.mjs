// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces channel_e2ee_vectors.json (docs/signal-integration/10-part-c1-private-channels.md) with libsodium, from
// fixed inputs, so the Android and desktop apps encrypt private channel posts byte for byte the same way.
// Not part of any build. Run from the repository root (ui/ must have its node_modules):
//
//   node desktop/tests/vectors/make_channel_e2ee_vectors.mjs
import { createRequire } from "node:module";
import { writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const require = createRequire(resolve(here, "../../../ui/package.json"));
const sodium = require("libsodium-wrappers-sumo");
await sodium.ready;

const enc = (s) => sodium.from_string(s);
const b64 = (u) => sodium.to_base64(u, sodium.base64_variants.ORIGINAL);
const fill = (n, f) => Uint8Array.from({ length: n }, (_, i) => f(i));

const channelId = "cccccccc-1111-4111-8111-111111111111";
const secret = fill(32, (i) => i + 1);
const secretText = sodium.to_base64(secret, sodium.base64_variants.URLSAFE_NO_PADDING);
const wrapKey = sodium.crypto_generichash(32, enc("redvault-channel-v1:wrap"), secret);
const accessToken = sodium.crypto_generichash(32, enc("redvault-channel-v1:access"), secret);

const epoch = 2;
const contentKey = fill(32, (i) => 0xa0 + i);
const wrapNonce = fill(24, (i) => 0x10 + i);
const keyAd = `RVCHK|v1|${channelId}|${epoch}`;
const wrapped = new Uint8Array([
  ...wrapNonce,
  ...sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(contentKey, enc(keyAd), null, wrapNonce, wrapKey),
]);

const plaintext = JSON.stringify({ v: 1, body: "Hello, **followers** 👋", content: { poll: { question: "Next topic?", options: ["A", "B"] } } });
const postNonce = fill(24, (i) => 0x40 + i);
const postAd = `RVCHP|v1|${channelId}|${epoch}`;
const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(enc(plaintext), enc(postAd), null, postNonce, contentKey);

const flip = (u, at) => { const c = Uint8Array.from(u); c[at] ^= 1; return c; };

const out = {
  comment: "Private channel E2EE (docs/signal-integration/10-part-c1-private-channels.md). All base64 is standard with padding except secretText (base64url, no padding, as in the link). Every client must reproduce wrapKey, accessToken, unwrap the key, decrypt the post, and refuse every case under failures.",
  channelId,
  secretText,
  wrapKey: b64(wrapKey),
  accessToken: b64(accessToken),
  epoch,
  contentKey: b64(contentKey),
  keyAd,
  wrapped: b64(wrapped),
  postAd,
  post: { keyEpoch: epoch, nonce: b64(postNonce), ciphertext: b64(ciphertext) },
  plaintext,
  failures: [
    { name: "the key for another channel", kind: "unwrap", channelId: "cccccccc-2222-4222-8222-222222222222", epoch, wrapped: b64(wrapped) },
    { name: "the key under another epoch", kind: "unwrap", channelId, epoch: 3, wrapped: b64(wrapped) },
    { name: "a wrapped key with a changed byte", kind: "unwrap", channelId, epoch, wrapped: b64(flip(wrapped, 30)) },
    { name: "a post read as another channel's", kind: "post", channelId: "cccccccc-2222-4222-8222-222222222222", keyEpoch: epoch, nonce: b64(postNonce), ciphertext: b64(ciphertext) },
    { name: "a post read under another epoch", kind: "post", channelId, keyEpoch: 1, nonce: b64(postNonce), ciphertext: b64(ciphertext) },
    { name: "a post with a changed byte", kind: "post", channelId, keyEpoch: epoch, nonce: b64(postNonce), ciphertext: b64(flip(ciphertext, 5)) },
  ],
};
writeFileSync(resolve(here, "channel_e2ee_vectors.json"), JSON.stringify(out, null, 1) + "\n");
console.log("wrote channel_e2ee_vectors.json");
