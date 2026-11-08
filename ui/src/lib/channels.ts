// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Channels on this device: following them (lib/localPrefs.ts followedChannels;
// the server never learns who follows what) and private channels' links.
//
// A private channel's link is …/?channel=<id>#<secret>. The 32-byte secret
// stays in the URL fragment, which browsers never send to a server; readers
// send an access token derived from it (the server keeps only its hash). The
// owner keeps the secret sealed to their own public key on the server, so
// their devices can show the link again.
import sodium from "libsodium-wrappers-sumo";
import type { ChannelDto } from "./api.js";
import { API_BASE } from "./apiBase.js";
import { fromBase64, toBase64 } from "./e2eCrypto.js";
import type { FollowedChannel } from "./localPrefs.js";

const SECRET_BYTES = 32;

/** A channel's shareable link: ?channel=<id>, plus the secret in the fragment for a private one. */
export function channelUrl(id: string, secret?: string | null): string {
  const base = new URL(API_BASE.replace(/\/api\/?$/, "/") || "/", window.location.origin);
  base.search = `?channel=${encodeURIComponent(id)}`;
  base.hash = secret ?? "";
  return base.toString();
}

/** A new private link's secret, base64url (what goes after the #). */
export function newChannelSecret(): string {
  return sodium.to_base64(sodium.randombytes_buf(SECRET_BYTES), sodium.base64_variants.URLSAFE_NO_PADDING);
}

/** The secret from a link's fragment, or null if it isn't one. */
export function channelSecretFromFragment(fragment: string): string | null {
  const secret = fragment.replace(/^#/, "");
  try {
    return sodium.from_base64(secret, sodium.base64_variants.URLSAFE_NO_PADDING).length === SECRET_BYTES ? secret : null;
  } catch {
    return null;
  }
}

/** What a reader shows the server (X-Channel-Access): derived from the secret, which can't be worked back from it. */
export function channelAccessToken(secret: string): string {
  const bytes = sodium.from_base64(secret, sodium.base64_variants.URLSAFE_NO_PADDING);
  return toBase64(sodium.crypto_generichash(32, "redvault-channel-v1:access", bytes));
}

export function sealChannelSecret(secret: string, ownerPublicKey: Uint8Array): string {
  return toBase64(sodium.crypto_box_seal(secret, ownerPublicKey));
}

/** The owner's private link secret, unsealed with their key pair; null when there's none or it won't open. */
export function openChannelSecret(sealed: string | null, publicKey: Uint8Array, privateKey: Uint8Array): string | null {
  if (!sealed) return null;
  try {
    return sodium.to_string(sodium.crypto_box_seal_open(fromBase64(sealed), publicKey, privateKey));
  } catch {
    return null;
  }
}

export function isFollowing(list: FollowedChannel[], id: string): boolean {
  return list.some((f) => f.id === id);
}

/** Following starts with everything already posted counted as seen. A private channel's secret comes along. */
export function follow(list: FollowedChannel[], channel: ChannelDto, secret?: string | null): FollowedChannel[] {
  if (isFollowing(list, channel.id)) return list;
  return [...list, { id: channel.id, name: channel.name, seenAt: channel.lastPostAt, ...(secret ? { secret } : {}) }];
}

export function unfollow(list: FollowedChannel[], id: string): FollowedChannel[] {
  return list.filter((f) => f.id !== id);
}

/** Everything in the channel up to its newest post has been seen; also keeps its name (and a newer secret) current. */
export function markSeen(list: FollowedChannel[], channel: ChannelDto, secret?: string | null): FollowedChannel[] {
  return list.map((f) => (f.id === channel.id ? { ...f, name: channel.name, seenAt: channel.lastPostAt, ...(secret ? { secret } : {}) } : f));
}

export function hasUnread(followed: FollowedChannel, channel: ChannelDto): boolean {
  if (!channel.lastPostAt) return false;
  return !followed.seenAt || Date.parse(channel.lastPostAt) > Date.parse(followed.seenAt);
}
