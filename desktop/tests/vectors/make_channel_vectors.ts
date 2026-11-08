// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces channel_vectors.json with the web app's own code, so the desktop
// app's private channel links, and channel posts forwarded into chats, are
// pinned to it byte for byte. Not part of any build: copy it into
// ui/src/test/ as makeChannelVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeChannelVectors.test.ts
//
// and delete the copy. The output lands next to this file (desktop/tests/vectors/).
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import { channelAccessToken, channelSecretFromFragment, channelUrl, openChannelSecret, sealChannelSecret } from "../lib/channels.js";
import * as crypto from "../lib/e2eCrypto.js";
import { parseStructured, serializeChannelForward } from "../lib/richMessage.js";

// From ui/ (where vitest runs) to desktop/tests/vectors/, where this file lives.
const OUT = resolve(process.cwd(), "../desktop/tests/vectors/channel_vectors.json");

const CHANNEL = "11111111-2222-4333-8444-555555555555";
const POST = "aaaaaaaa-0000-4000-8000-000000000001";

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const b64 = crypto.toBase64;
  const b64url = (bytes: Uint8Array) => sodium.to_base64(bytes, sodium.base64_variants.URLSAFE_NO_PADDING);

  // Secrets as the link carries them, and the access token each one shows the server.
  // 0xfb… comes out with '-' and '_' in base64url, '+' and '/' in standard base64.
  const secrets = [
    b64url(Uint8Array.from({ length: 32 }, (_, i) => i + 1)),
    b64url(new Uint8Array(32).fill(0xfb)),
    b64url(new Uint8Array(32)),
    b64url(sodium.randombytes_buf(32)),
  ];
  const tokens = secrets.map((secret) => ({ secret, access: channelAccessToken(secret) }));

  const frag = secrets[0];
  const fragmentCases = [
    frag,
    `#${frag}`,
    frag.slice(1),
    `${frag}=`,
    `${frag}x`,
    secrets[1],
    b64(new Uint8Array(32).fill(0xfb)).replace(/=+$/, ""),
    b64url(new Uint8Array(33).fill(7)),
    b64url(new Uint8Array(31).fill(7)),
    frag.slice(0, 42) + "B",
    `${frag.slice(0, 20)} ${frag.slice(21)}`,
    "#",
    "",
  ].map((fragment) => ({ fragment, secret: channelSecretFromFragment(fragment) }));

  // The owner's copy of the secret, sealed to their public key (crypto_box_seal of the base64url text).
  const owner = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(9));
  const other = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(10));
  const sealed = sealChannelSecret(secrets[0], owner.publicKey);
  expect(openChannelSecret(sealed, owner.publicKey, owner.privateKey)).toBe(secrets[0]);
  expect(openChannelSecret(sealed, other.publicKey, other.privateKey)).toBeNull();

  const publicUrl = channelUrl(CHANNEL);
  const privateUrl = channelUrl(CHANNEL, secrets[0]);
  const webBase = publicUrl.slice(0, publicUrl.indexOf("?"));

  // A channel post forwarded into a chat: what goes inside the encrypted message.
  const forwards = [
    { text: "Hello everyone", ref: { channelId: CHANNEL.toUpperCase(), postId: POST.toUpperCase(), channelName: "RedVault news" } },
    // Hidden characters, runs of whitespace, quotes, an emoji, and more than 80 characters of name.
    {
      text: 'Line one\nline "two" \u{1F512}',
      ref: { channelId: CHANNEL, postId: POST, channelName: `  Café​ \t "crew"\n\u{1F512} ${"x".repeat(90)}  ` },
    },
  ].map(({ text, ref }) => ({ text, ref, body: serializeChannelForward(text, ref) }));

  const raw = (extra: Record<string, unknown>) => "\u0000rv-msg:" + JSON.stringify({ type: "text", version: 1, text: "Hi", ...extra });
  const ok = { channelId: CHANNEL, postId: POST, channelName: "News" };
  const parseCases = [
    forwards[0].body,
    forwards[1].body,
    raw({ forwarded: true, channelPost: ok }),
    raw({ channelPost: { ...ok, channelId: CHANNEL.toUpperCase(), postId: POST.toUpperCase() } }),
    raw({ forwarded: true, channelPost: { ...ok, channelName: `  a​​b\n\nc ${"y".repeat(100)}` } }),
    raw({ forwarded: true, channelPost: { ...ok, channelId: "nope" } }),
    raw({ forwarded: true, channelPost: { ...ok, postId: 7 } }),
    raw({ forwarded: true, channelPost: { channelId: CHANNEL, postId: POST } }),
    raw({ forwarded: true, channelPost: { ...ok, channelName: 7 } }),
    raw({ forwarded: true, channelPost: { ...ok, channelName: " ​⁠ " } }),
    raw({ forwarded: true, channelPost: null }),
    raw({ forwarded: true, channelPost: "x" }),
    raw({ forwarded: true, channelPost: [] }),
    raw({ forwarded: true, channelPost: { ...ok, extra: 1 } }),
  ].map((body) => ({ body, parsed: parseStructured(body) }));

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        channelId: CHANNEL,
        tokens,
        fragmentCases,
        owner: { publicKey: b64(owner.publicKey), privateKey: b64(owner.privateKey) },
        other: { publicKey: b64(other.publicKey), privateKey: b64(other.privateKey) },
        sealed: { secret: secrets[0], sealed },
        webBase,
        publicUrl,
        privateUrl,
        forwards,
        parseCases,
      },
      null,
      2
    ) + "\n"
  );
});
