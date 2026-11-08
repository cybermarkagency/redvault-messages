// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces group_invite_vectors.json with the web app's own code, so the
// desktop app's group invite links are pinned to it byte for byte. Not part of
// any build: copy it into ui/src/test/ as makeGroupInviteVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeGroupInviteVectors.test.ts
//
// and delete the copy. The output lands next to this file (desktop/tests/vectors/).
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  inviteKeys,
  inviteSecretFromFragment,
  newGroupInviteLink,
  openGroupInviteLink,
  openGroupInvitePreview,
  parseGroupInvite,
  wrapGroupKeyForLink,
} from "../lib/groupInviteCrypto.js";

// From ui/ (where vitest runs) to desktop/tests/vectors/, where this file lives.
const OUT = resolve(process.cwd(), "../desktop/tests/vectors/group_invite_vectors.json");

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const b64 = crypto.toBase64;
  const b64url = (bytes: Uint8Array) => sodium.to_base64(bytes, sodium.base64_variants.URLSAFE_NO_PADDING);
  const groupKey = Uint8Array.from({ length: 32 }, (_, i) => i + 1);
  // Quotes, a control character, an emoji and non-ASCII letters: the preview's JSON must come out the same.
  const name = 'Café "crew" \u{1F512}\n\t\u0001 </>';
  const linkId = "Vx3_kQ-9aZ0bC1dE2fG3hA";

  const open = newGroupInviteLink(groupKey, name, false);
  const approval = newGroupInviteLink(groupKey, "Book club", true);
  const keys = inviteKeys(open.secret);

  const opened = openGroupInviteLink(
    { id: linkId, secretCiphertext: open.input.secretCiphertext, secretNonce: open.input.secretNonce, approval: false, createdAt: "2026-10-18T09:00:00.000Z" },
    groupKey
  )!;
  expect(opened.secret).toEqual(open.secret);
  const webBase = opened.url.slice(0, opened.url.indexOf("?"));
  const frag = b64url(open.secret);

  const previewOf = (input: typeof open.input) => ({
    conversationId: "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d",
    previewCiphertext: input.previewCiphertext,
    previewNonce: input.previewNonce,
    memberCount: 3,
    approval: input.wrappedKey === null,
    wrappedKey: input.wrappedKey,
    wrappedKeyNonce: input.wrappedKeyNonce,
    member: false,
    requested: false,
  });
  const openPreview = openGroupInvitePreview(previewOf(open.input), open.secret);
  expect(openPreview.groupKey).toEqual(groupKey);
  const approvalPreview = openGroupInvitePreview(previewOf(approval.input), approval.secret);
  expect(approvalPreview.groupKey).toBeNull();
  expect(() => openGroupInvitePreview(previewOf(open.input), approval.secret)).toThrow();

  // Previews another app might have written: no name, or a name that isn't text.
  const oddPreview = (json: string) => {
    const nonce = sodium.randombytes_buf(24);
    const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(json, null, null, nonce, keys.key);
    const preview = { ...previewOf(open.input), previewCiphertext: b64(ciphertext), previewNonce: b64(nonce), wrappedKey: null, wrappedKeyNonce: null };
    return { json, previewCiphertext: preview.previewCiphertext, previewNonce: preview.previewNonce, name: openGroupInvitePreview(preview, open.secret).name };
  };

  const wrapped = wrapGroupKeyForLink(groupKey, approval.secret);

  const joiner = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(5));
  const sealedForJoiner = crypto.sealConversationKey(groupKey, joiner.publicKey);

  const parseCases = [
    opened.url,
    `Join us: ${opened.url} see you there`,
    `  ${opened.url}\n`,
    opened.url.split("#")[0],
    `?invite=${linkId}#${frag}`,
    `https://x.example/app/?a=1&invite=${linkId}&b=2#${frag}`,
    `https://x.example/?inv%69te=${linkId}#${frag}`,
    `https://x.example/?invite=${linkId}%21#${frag}`,
    `https://x.example/?invite=&invite=${linkId}#${frag}`,
    `https://x.example/?invite=${"a".repeat(64)}#${frag}`,
    `https://x.example/?invite=${"a".repeat(65)}#${frag}`,
    `https://x.example/?invite=${linkId}#${frag}x`,
    `https://x.example/?invite=${linkId}#${frag.slice(1)}`,
    `https://x.example/?join=${linkId}#${frag}`,
    `https://x.example/#${frag}?invite=${linkId}`,
    `${opened.url}.`,
    "hello world",
    "",
  ].map((text) => ({ text, result: parseGroupInvite(text) }));

  // 0xfb… comes out with '+' and '/' in standard base64, '-' and '_' in base64url.
  const awkward = new Uint8Array(32).fill(0xfb);
  const fragmentCases = [
    frag,
    `#${frag}`,
    frag.slice(1),
    `${frag}=`,
    b64url(awkward),
    b64(awkward).replace(/=+$/, ""),
    b64url(new Uint8Array(33).fill(7)),
    frag.slice(0, 42) + "B",
    "",
  ].map((fragment) => {
    const secret = inviteSecretFromFragment(fragment);
    return { fragment, secret: secret ? b64(secret) : null };
  });

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        groupKey: b64(groupKey),
        name,
        linkId,
        webBase,
        open: {
          secret: b64(open.secret),
          fragment: frag,
          key: b64(keys.key),
          auth: keys.auth,
          input: open.input,
          url: opened.url,
          previewName: openPreview.name,
        },
        approval: {
          secret: b64(approval.secret),
          auth: inviteKeys(approval.secret).auth,
          input: approval.input,
          previewName: approvalPreview.name,
        },
        oddPreviews: [oddPreview('{"v":1}'), oddPreview('{"v":1,"name":7}'), oddPreview('{"name":"Ünïcode \\u00e9"}')],
        wrapForApprovalOff: { secret: b64(approval.secret), ...wrapped },
        joiner: { publicKey: b64(joiner.publicKey), privateKey: b64(joiner.privateKey), sealedGroupKey: b64(sealedForJoiner) },
        parseCases,
        fragmentCases,
      },
      null,
      2
    ) + "\n"
  );
});
