// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Group invite links: …/?invite=<id>#<secret>. The 32-byte secret lives only
// in the URL fragment, which browsers never send to any server. From it come
// two keys that can't be worked out from each other:
//   - an encryption key, under which the group's name and (when joins needn't
//     be approved) the group key are stored on the server;
//   - an auth token, shown to the server to prove the link was really opened.
// The server keeps only a hash of the token, so it can't read either.
// Members keep the secret itself encrypted with the group key, so any of
// them can show and share the link again.
import sodium from "libsodium-wrappers-sumo";
import type { GroupInviteLinkDto, GroupInviteLinkInput, GroupInvitePreviewDto } from "./api.js";
import { API_BASE } from "./apiBase.js";
import { fromBase64, toBase64 } from "./e2eCrypto.js";
import { pastedUrl } from "./pastedUrl.js";

const SECRET_BYTES = 32;

function toBase64Url(bytes: Uint8Array): string {
  return sodium.to_base64(bytes, sodium.base64_variants.URLSAFE_NO_PADDING);
}

function encrypt(plaintext: Uint8Array | string, key: Uint8Array): { ciphertext: string; nonce: string } {
  const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
  const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(plaintext, null, null, nonce, key);
  return { ciphertext: toBase64(ciphertext), nonce: toBase64(nonce) };
}

function decrypt(ciphertext: string, nonce: string, key: Uint8Array): Uint8Array {
  return sodium.crypto_aead_xchacha20poly1305_ietf_decrypt(null, fromBase64(ciphertext), null, fromBase64(nonce), key);
}

/** The link's two keys (see the top of this file). */
export function inviteKeys(secret: Uint8Array): { key: Uint8Array; auth: string } {
  return {
    key: sodium.crypto_generichash(32, "redvault-group-invite-v1:key", secret),
    auth: toBase64(sodium.crypto_generichash(32, "redvault-group-invite-v1:auth", secret)),
  };
}

export function inviteSecretFromFragment(fragment: string): Uint8Array | null {
  try {
    const bytes = sodium.from_base64(fragment.replace(/^#/, ""), sodium.base64_variants.URLSAFE_NO_PADDING);
    return bytes.length === SECRET_BYTES ? bytes : null;
  } catch {
    return null;
  }
}

/** The shareable URL: this app's address, the link id, and the secret in the fragment. */
export function groupInviteUrl(id: string, secret: Uint8Array): string {
  const base = new URL(API_BASE.replace(/\/api\/?$/, "/") || "/", window.location.origin);
  base.search = `?invite=${encodeURIComponent(id)}`;
  base.hash = toBase64Url(secret);
  return base.toString();
}

/** A pasted group invite link (…?invite=<id>#<secret>), checked for shape only. */
export function parseGroupInvite(text: string): { linkId: string; secret: string } | null {
  const url = pastedUrl(text);
  if (!url) return null;
  const linkId = url.searchParams.get("invite") ?? "";
  const secret = url.hash.replace(/^#/, "");
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(linkId) || !/^[A-Za-z0-9_-]{43}$/.test(secret)) return null;
  return { linkId, secret };
}

/** The group key under the link's key: lets people join without an admin approving. */
export function wrapGroupKeyForLink(groupKey: Uint8Array, secret: Uint8Array): { wrappedKey: string; wrappedKeyNonce: string } {
  const { ciphertext, nonce } = encrypt(groupKey, inviteKeys(secret).key);
  return { wrappedKey: ciphertext, wrappedKeyNonce: nonce };
}

/** A new link for a group: a fresh secret, and what the server stores for it. */
export function newGroupInviteLink(groupKey: Uint8Array, groupName: string, approval: boolean): { secret: Uint8Array; input: GroupInviteLinkInput } {
  const secret = sodium.randombytes_buf(SECRET_BYTES);
  const { key, auth } = inviteKeys(secret);
  const sealedSecret = encrypt(secret, groupKey);
  const preview = encrypt(JSON.stringify({ v: 1, name: groupName }), key);
  const wrapped = approval ? { wrappedKey: null, wrappedKeyNonce: null } : wrapGroupKeyForLink(groupKey, secret);
  return {
    secret,
    input: {
      auth,
      secretCiphertext: sealedSecret.ciphertext,
      secretNonce: sealedSecret.nonce,
      previewCiphertext: preview.ciphertext,
      previewNonce: preview.nonce,
      ...wrapped,
    },
  };
}

/** A member's view of the group's link: its secret, unlocked with the group key, and the URL to share. */
export function openGroupInviteLink(link: GroupInviteLinkDto, groupKey: Uint8Array): { secret: Uint8Array; url: string } | null {
  try {
    const secret = decrypt(link.secretCiphertext, link.secretNonce, groupKey);
    return { secret, url: groupInviteUrl(link.id, secret) };
  } catch {
    return null;
  }
}

/** What the link shows someone about to join: the group's name and, if no approval is needed, its key. Throws on a wrong secret. */
export function openGroupInvitePreview(preview: GroupInvitePreviewDto, secret: Uint8Array): { name: string; groupKey: Uint8Array | null } {
  const { key } = inviteKeys(secret);
  const parsed = JSON.parse(sodium.to_string(decrypt(preview.previewCiphertext, preview.previewNonce, key))) as { name?: unknown };
  const groupKey = preview.wrappedKey && preview.wrappedKeyNonce ? decrypt(preview.wrappedKey, preview.wrappedKeyNonce, key) : null;
  return { name: typeof parsed.name === "string" ? parsed.name : "Group", groupKey };
}
