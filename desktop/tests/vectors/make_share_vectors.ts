// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Produces share_vectors.json with the web app's own code, so the desktop
// and Android apps' public share links (docs/public-share-links.md) are
// pinned to it byte for byte. Not part of any build: copy it into
// ui/src/test/ as makeShareVectors.test.ts, run
//
//   cd ui && npx vitest run src/test/makeShareVectors.test.ts
//
// and delete the copy. The output lands next to this file (desktop/tests/vectors/).
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import sodium from "libsodium-wrappers-sumo";
import { expect, it } from "vitest";
import {
  deriveShareKeys,
  openContribution,
  openEnvelope,
  openItem,
  openOwnerBox,
  parseShareLocation,
  ready,
  sealContribution,
  sealEnvelope,
  sealItem,
  sealOwnerBox,
  type ShareItemMeta,
  shareUrl,
  toB64,
  toB64Url,
  uploadKeyPair,
  verifierOf,
} from "../lib/shareCrypto.js";

// From ui/ (where vitest runs) to desktop/tests/vectors/, where this file lives.
const OUT = resolve(process.cwd(), "../desktop/tests/vectors/share_vectors.json");
const MIB = 1024 * 1024;

it("writes the vectors", async () => {
  await ready();
  const fill = (n: number, byte: number) => new Uint8Array(n).fill(byte);
  const counting = Uint8Array.from({ length: 32 }, (_, i) => i + 1);

  // Keys: no password, passwords (with a decomposed one that must match its NFC form), other costs.
  const keyCases: { secret: Uint8Array; password: string | null; params: { salt: Uint8Array; ops: number; mem: number } | null }[] = [
    { secret: counting, password: null, params: null },
    { secret: fill(32, 0xfb), password: null, params: null },
    { secret: counting, password: "correct horse battery staple", params: { salt: fill(16, 0x11), ops: 2, mem: 64 * MIB } },
    { secret: counting, password: "café ☃ 🔑", params: { salt: fill(16, 0x22), ops: 3, mem: 64 * MIB } },
    { secret: fill(32, 0x7f), password: "p", params: { salt: fill(16, 0x33), ops: 2, mem: 128 * MIB } },
  ];
  const keys = [];
  for (const c of keyCases) {
    const k = await deriveShareKeys(c.secret, c.password, c.params);
    keys.push({
      secret: toB64Url(c.secret),
      password: c.password,
      params: c.params ? { salt: toB64(c.params.salt), ops: c.params.ops, mem: c.params.mem } : null,
      lookup: toB64(k.lookup),
      access: toB64(k.access),
      wrap: toB64(k.wrap),
      lookupVerifier: verifierOf(k.lookup),
      accessVerifier: verifierOf(k.access),
    });
  }
  // The decomposed password derives what its composed form does.
  const composed = await deriveShareKeys(counting, "café ☃ 🔑", keyCases[3].params);
  expect(toB64(composed.access)).toBe(keys[3].access);

  // Envelopes under the first two cases' wrap keys (one with an upload key, one without).
  const driveKey = Uint8Array.from({ length: 32 }, (_, i) => 200 - i);
  const uploadSalt = fill(16, 0x44);
  const pair = uploadKeyPair(driveKey, uploadSalt);
  const shareKey = fill(32, 0x55);
  const wrap0 = sodium.from_base64(keys[0].wrap, sodium.base64_variants.ORIGINAL);
  const wrap2 = sodium.from_base64(keys[2].wrap, sodium.base64_variants.ORIGINAL);
  const envelopes = [
    { wrap: wrap0, envelope: { key: shareKey, upload: pair.publicKey, root: "aaaaaaaa-0000-4000-8000-000000000001" } },
    { wrap: wrap2, envelope: { key: fill(32, 0x66), upload: null, root: "aaaaaaaa-0000-4000-8000-000000000002" } },
  ].map(({ wrap, envelope }) => {
    const sealed = sealEnvelope(wrap, envelope);
    expect(openEnvelope(wrap, sealed.ciphertext, sealed.nonce)).toEqual(envelope);
    return {
      wrap: toB64(wrap),
      ciphertext: toB64(sealed.ciphertext),
      nonce: toB64(sealed.nonce),
      key: toB64(envelope.key),
      upload: envelope.upload ? toB64(envelope.upload) : null,
      root: envelope.root,
    };
  });

  // Items: a folder and files, including names that need escaping and non-ASCII.
  const metas: { id: string; meta: ShareItemMeta }[] = [
    { id: "aaaaaaaa-0000-4000-8000-000000000001", meta: { v: 1, kind: "folder", name: "Project Files", parent: "" } },
    {
      id: "aaaaaaaa-0000-4000-8000-000000000003",
      meta: {
        v: 1,
        kind: "file",
        name: 'Q4 "draft" — résumé.pdf',
        parent: "aaaaaaaa-0000-4000-8000-000000000001",
        mime: "application/pdf",
        size: 123456,
        modified: 1_760_000_000_123,
        key: toB64(fill(32, 0x77)),
        header: toB64(fill(24, 0x78)),
        chunk: 65536,
        sha: "0123456789abcdef".repeat(4),
      },
    },
    { id: "aaaaaaaa-0000-4000-8000-000000000004", meta: { v: 1, kind: "folder", name: "写真 📷", parent: "aaaaaaaa-0000-4000-8000-000000000001" } },
  ];
  const items = metas.map(({ id, meta }) => {
    const sealed = sealItem(shareKey, id, meta);
    expect(openItem(shareKey, id, sealed.ciphertext, sealed.nonce)).toEqual(meta);
    return { shareKey: toB64(shareKey), itemId: id, ciphertext: toB64(sealed.ciphertext), nonce: toB64(sealed.nonce), meta };
  });

  // The owner's copy.
  const box = {
    v: 1 as const,
    secret: keys[0].secret,
    key: toB64(shareKey),
    root: "aaaaaaaa-0000-4000-8000-000000000001",
    label: "Clients — Q4 ✓",
    created: 1_760_000_000_000,
  };
  const sealedBox = sealOwnerBox(driveKey, box);
  expect(openOwnerBox(driveKey, sealedBox.ciphertext, sealedBox.nonce)).toEqual(box);

  // Upload key pairs, and a contribution sealed to the first.
  const uploadKeys = [uploadSalt, fill(16, 0x45), counting.slice(0, 16)].map((salt) => {
    const p = uploadKeyPair(driveKey, salt);
    return { driveKey: toB64(driveKey), salt: toB64(salt), publicKey: toB64(p.publicKey), privateKey: toB64(p.privateKey) };
  });
  const contributionMeta = {
    v: 1 as const,
    name: "Reports/2026/Ünïcode report.docx",
    mime: "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    size: 42,
    modified: 1_760_000_000_000,
    sent: 1_760_000_100_000,
    key: toB64(fill(32, 0x88)),
    header: toB64(fill(24, 0x89)),
    chunk: 65536,
    sha: "fedcba9876543210".repeat(4),
  };
  const sealedContribution = sealContribution(pair.publicKey, contributionMeta);
  expect(openContribution(pair, sealedContribution)).toEqual(contributionMeta);

  const secret = sodium.from_base64(keys[1].secret, sodium.base64_variants.URLSAFE_NO_PADDING);
  const url = shareUrl("https://www.redvaultsystems.com/redvaultmessages/", "Zm9vYmFyYmF6cXV4LV9fXw", secret);
  const parsed = new URL(url);
  expect(parseShareLocation(parsed.search, parsed.hash)?.publicId).toBe("Zm9vYmFyYmF6cXV4LV9fXw");

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        _comment: "Made by make_share_vectors.ts from ui/src/lib/shareCrypto.ts. See docs/public-share-links.md.",
        keys,
        envelopes,
        items,
        ownerBox: { driveKey: toB64(driveKey), ciphertext: toB64(sealedBox.ciphertext), nonce: toB64(sealedBox.nonce), box },
        uploadKeys,
        contribution: { publicKey: uploadKeys[0].publicKey, privateKey: uploadKeys[0].privateKey, sealed: toB64(sealedContribution), meta: contributionMeta },
        url: { webBase: "https://www.redvaultsystems.com/redvaultmessages/", publicId: "Zm9vYmFyYmF6cXV4LV9fXw", secret: keys[1].secret, url },
      },
      null,
      2
    ) + "\n"
  );
}, 120_000);
