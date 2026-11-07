import sodium from "libsodium-wrappers-sumo";
import { beforeAll, describe, expect, it } from "vitest";
import {
  deriveShareKeys,
  fromB64Url,
  lookupToken,
  newLinkSecret,
  newShareKey,
  openContribution,
  openEnvelope,
  openItem,
  openOwnerBox,
  padJson,
  parseShareLocation,
  ready,
  sealContribution,
  sealEnvelope,
  sealItem,
  sealOwnerBox,
  type ShareFileMeta,
  shareUrl,
  toB64,
  toB64Url,
  uploadKeyPair,
  verifierOf,
} from "../lib/shareCrypto.js";

// The public share link protocol (docs/public-share-links.md), on the web.
// The desktop and Android apps are pinned to the same bytes by
// desktop/tests/vectors/share_vectors.json (make_share_vectors.ts).

const FAST = { ops: 2, mem: 64 * 1024 * 1024 };
const fileMeta = (over: Partial<ShareFileMeta> = {}): ShareFileMeta => ({
  v: 1,
  kind: "file",
  name: "Report.pdf",
  parent: "11111111-1111-4111-8111-111111111111",
  mime: "application/pdf",
  size: 1234,
  modified: 1_700_000_000_000,
  key: toB64(new Uint8Array(32).fill(1)),
  header: toB64(new Uint8Array(24).fill(2)),
  chunk: 65536,
  sha: "ab".repeat(32),
  ...over,
});

beforeAll(async () => {
  await ready();
});

describe("share link keys", () => {
  it("derives separate lookup, access and wrap keys, and only the tokens are ever sent", async () => {
    const secret = newLinkSecret();
    const keys = await deriveShareKeys(secret, null, null);
    expect(keys.lookup).toEqual(lookupToken(secret));
    const all = [keys.lookup, keys.access, keys.wrap].map(toB64);
    expect(new Set(all).size).toBe(3);
    expect(all.every((k) => !k.includes(toB64(secret)))).toBe(true);
    // Deterministic: every device holding the link derives the same.
    expect(await deriveShareKeys(secret, null, null)).toEqual(keys);
    // The verifier is SHA-256 of the token, nothing else.
    expect(verifierOf(keys.access)).toBe(toB64(sodium.crypto_hash_sha256(keys.access)));
  });

  it("a password changes the access token and the wrap key, but not the lookup token", async () => {
    const secret = newLinkSecret();
    const params = { salt: new Uint8Array(16).fill(5), ...FAST };
    const plain = await deriveShareKeys(secret, null, null);
    const right = await deriveShareKeys(secret, "correct horse", params);
    const wrong = await deriveShareKeys(secret, "correct hors", params);
    expect(right.lookup).toEqual(plain.lookup);
    expect(right.access).not.toEqual(plain.access);
    expect(right.wrap).not.toEqual(plain.wrap);
    expect(wrong.access).not.toEqual(right.access);
    expect(wrong.wrap).not.toEqual(right.wrap);
    // Another salt, another key.
    const salted = await deriveShareKeys(secret, "correct horse", { ...params, salt: new Uint8Array(16).fill(6) });
    expect(salted.wrap).not.toEqual(right.wrap);
    // A password link can't be used without one, and silly costs are refused.
    await expect(deriveShareKeys(secret, null, params)).rejects.toThrow();
    await expect(deriveShareKeys(secret, "x", { ...params, ops: 1 })).rejects.toThrow();
  });

  it("passwords are compared as NFC: the same word typed on any keyboard opens the link", async () => {
    const secret = newLinkSecret();
    const params = { salt: new Uint8Array(16).fill(7), ...FAST };
    const composed = await deriveShareKeys(secret, "café", params);
    const decomposed = await deriveShareKeys(secret, "café", params);
    expect(decomposed).toEqual(composed);
  });
});

describe("envelope, items and the owner's copy", () => {
  it("the envelope opens only with the right wrap key and untouched bytes", async () => {
    const keys = await deriveShareKeys(newLinkSecret(), null, null);
    const other = await deriveShareKeys(newLinkSecret(), null, null);
    const shareKey = newShareKey();
    const upload = uploadKeyPair(new Uint8Array(32).fill(3), new Uint8Array(16).fill(4)).publicKey;
    const sealed = sealEnvelope(keys.wrap, { key: shareKey, upload, root: "root-item" });
    expect(sealed.ciphertext.length % 128).toBe(16);
    expect(openEnvelope(keys.wrap, sealed.ciphertext, sealed.nonce)).toEqual({ key: shareKey, upload, root: "root-item" });
    expect(openEnvelope(other.wrap, sealed.ciphertext, sealed.nonce)).toBeNull();
    const tampered = Uint8Array.from(sealed.ciphertext);
    tampered[5] ^= 1;
    expect(openEnvelope(keys.wrap, tampered, sealed.nonce)).toBeNull();
    // An item sealed under the same key isn't an envelope (different associated data).
    const item = sealItem(keys.wrap, "x", fileMeta());
    expect(openEnvelope(keys.wrap, item.ciphertext, item.nonce)).toBeNull();
  });

  it("an item's metadata is bound to its id: the server can't swap names or keys between items", () => {
    const key = newShareKey();
    const a = sealItem(key, "item-a", fileMeta({ name: "a.txt" }));
    expect(openItem(key, "item-a", a.ciphertext, a.nonce)).toEqual(fileMeta({ name: "a.txt" }));
    expect(openItem(key, "item-b", a.ciphertext, a.nonce)).toBeNull();
    expect(openItem(newShareKey(), "item-a", a.ciphertext, a.nonce)).toBeNull();
    const folder = sealItem(key, "f", { v: 1, kind: "folder", name: "Photos", parent: "" });
    expect(openItem(key, "f", folder.ciphertext, folder.nonce)).toEqual({ v: 1, kind: "folder", name: "Photos", parent: "" });
  });

  it("refuses malformed metadata even when it decrypts", () => {
    const key = newShareKey();
    const bad = (meta: unknown) => {
      const nonce = sodium.randombytes_buf(24);
      const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(padJson(meta, 128), new Uint8Array([...new TextEncoder().encode("rvshare-item-v1|i")]), null, nonce, key);
      return openItem(key, "i", ciphertext, nonce);
    };
    expect(bad({ ...fileMeta(), v: 2 })).toBeNull();
    expect(bad({ ...fileMeta(), sha: "not-hex" })).toBeNull();
    expect(bad({ ...fileMeta(), size: -1 })).toBeNull();
    expect(bad({ ...fileMeta(), kind: "script" })).toBeNull();
    expect(bad({ ...fileMeta(), name: "x".repeat(2000) })).toBeNull();
    expect(bad({ ...fileMeta(), extra: true })).toEqual(fileMeta());
  });

  it("the owner's copy opens only with their Files key", () => {
    const driveKey = new Uint8Array(32).fill(8);
    const box = {
      v: 1 as const,
      secret: toB64Url(newLinkSecret()),
      key: toB64(newShareKey()),
      root: "aaaaaaaa-0000-4000-8000-000000000001",
      label: "Clients — Q4",
      created: 1_760_000_000_000,
    };
    const sealed = sealOwnerBox(driveKey, box);
    expect(openOwnerBox(driveKey, sealed.ciphertext, sealed.nonce)).toEqual(box);
    expect(openOwnerBox(new Uint8Array(32).fill(9), sealed.ciphertext, sealed.nonce)).toBeNull();
  });
});

describe("uploads", () => {
  it("the upload key pair is the owner's alone, different for every link", () => {
    const driveKey = new Uint8Array(32).fill(1);
    const pair = uploadKeyPair(driveKey, new Uint8Array(16).fill(2));
    expect(uploadKeyPair(driveKey, new Uint8Array(16).fill(2))).toEqual(pair);
    expect(uploadKeyPair(driveKey, new Uint8Array(16).fill(3)).publicKey).not.toEqual(pair.publicKey);
    expect(uploadKeyPair(new Uint8Array(32).fill(9), new Uint8Array(16).fill(2)).publicKey).not.toEqual(pair.publicKey);
    // Not the account's own key pair derived the usual way.
    expect(pair.privateKey).not.toEqual(driveKey);
  });

  it("a contribution opens with the link's key pair only", () => {
    const pair = uploadKeyPair(new Uint8Array(32).fill(1), new Uint8Array(16).fill(2));
    const other = uploadKeyPair(new Uint8Array(32).fill(1), new Uint8Array(16).fill(3));
    const meta = {
      v: 1 as const,
      name: "holiday/IMG_0001.jpg",
      mime: "image/jpeg",
      size: 99,
      modified: 1,
      sent: 2,
      key: toB64(new Uint8Array(32).fill(4)),
      header: toB64(new Uint8Array(24).fill(5)),
      chunk: 65536,
      sha: "cd".repeat(32),
    };
    const sealed = sealContribution(pair.publicKey, meta);
    // Padded to a multiple of 256 bytes before sealing (48 bytes of sealed-box overhead).
    expect(sealed.length % 256).toBe(48);
    expect(openContribution(pair, sealed)).toEqual(meta);
    expect(openContribution(other, sealed)).toBeNull();
  });
});

describe("links", () => {
  it("puts the secret only in the fragment and reads it back strictly", () => {
    const secret = new Uint8Array(32).fill(0xfb);
    const url = shareUrl("https://www.redvaultsystems.com/redvaultmessages/", "AAAAAAAAAAAAAAAAAAAAAA", secret);
    expect(url).toBe(`https://www.redvaultsystems.com/redvaultmessages/share/?id=AAAAAAAAAAAAAAAAAAAAAA#${toB64Url(secret)}`);
    const parsed = new URL(url);
    expect(parsed.search).not.toContain(toB64Url(secret));
    expect(parseShareLocation(parsed.search, parsed.hash)).toEqual({ publicId: "AAAAAAAAAAAAAAAAAAAAAA", secret });
    expect(parseShareLocation(parsed.search, "")).toBeNull();
    expect(parseShareLocation(parsed.search, parsed.hash.slice(0, -1))).toBeNull();
    expect(parseShareLocation("?id=short", parsed.hash)).toBeNull();
    expect(fromB64Url(toB64Url(secret))).toEqual(secret);
  });
});
