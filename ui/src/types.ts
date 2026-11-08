// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { ReactionEmoji, ReplyRef, StoryReplyRef } from "./lib/richMessage.js";
import type { FileDescriptor } from "./lib/fileMessage.js";

export type DurationKey = "off" | "30s" | "1m" | "1h" | "instant";

export const DURATION_LABELS: Record<DurationKey, string> = {
  off: "Off",
  "30s": "30 seconds",
  "1m": "1 minute",
  "1h": "1 hour",
  instant: "Instantly",
};

export const DURATION_KEYS: DurationKey[] = ["off", "30s", "1m", "1h", "instant"];

/** Real TTL in seconds, sent to and enforced by the server — null means off (never expires). */
export const DURATION_SECONDS: Record<DurationKey, number | null> = {
  off: null,
  instant: 1,
  "30s": 30,
  "1m": 60,
  "1h": 3600,
};

/** Inverse of DURATION_SECONDS — used to display whatever TTL the server reports back. */
export function durationKeyFromSeconds(seconds: number | null): DurationKey {
  if (seconds === null) return "off";
  const match = DURATION_KEYS.find((key) => DURATION_SECONDS[key] === seconds);
  return match ?? "off";
}

/**
 * An end-to-end encrypted file attached to a message. `descriptor` (incl.
 * the file key) came out of the decrypted message body and lives only in
 * memory for this session, same as the conversation key; `status` is the
 * server's lifecycle view of the ciphertext object.
 */
export interface FileAttachmentView {
  descriptor: FileDescriptor;
  status: "pending" | "uploaded" | "available" | "deleted" | "expired" | "failed";
}

/**
 * Stage-3 "Transaction verified" outcome for one outgoing message — see
 * ui/src/lib/commitment.ts and server/src/services/transparencyService.ts.
 * 'pending' only appears transiently while a lazy re-check is in flight
 * (e.g. after opening "Message Security" for a message loaded from a poll,
 * not freshly sent this session).
 */
export type VerificationStatus = "pending" | "verified" | "failed" | "unavailable";

/**
 * Own ("me") messages only. `encryptedLocally` and `relayed` are always
 * true once this exists at all — a message object is never created unless
 * the client actually encrypted it and the server actually acknowledged
 * persisting it (see state/actions.ts's sendDmMessage/sendDmFile). The only
 * things that genuinely vary are the transaction-verification outcome and,
 * separately, delivered/read (tracked on Message itself, not here, since
 * they're driven by live receipt polling rather than the one-time send).
 */
export interface MessageSecurity {
  encryptedLocally: true;
  relayed: true;
  verification: VerificationStatus;
  publicTransactionId?: string;
  protocolVersion?: number;
  localCommitment?: string; // hex
  networkCommitment?: string; // hex
}

export interface MessageReaction {
  emoji: ReactionEmoji;
  count: number;
  /** The signed-in user reacted with this emoji; `myReactionId` is that reaction message's id (deleting it removes the reaction). */
  mine: boolean;
  myReactionId?: string;
}

/** A poll and its tally: each person's latest vote counts until the poll's creator ends it. */
export interface PollView {
  question: string;
  options: string[];
  multiple: boolean;
  closed: boolean;
  /** Votes per option, and the options you picked. */
  counts: number[];
  mine: number[];
  voters: number;
}

export interface Message {
  /** Sent or received with per-device keys (lib/e2eV3/chat.ts): kept on this device only, text messages only. */
  v3?: boolean;
  /** A device-key message its sender deleted. */
  deleted?: boolean;
  from: "me" | string; // DM: "me" or the literal "them" · Group: "me" or the sender's DM id
  /**
   * Own messages waiting in the outbox (lib/outbox.ts): not on the server yet, so no `id`, and never
   * shown as sent or delivered. The outbox item's id, which is also the message's clientMessageId.
   */
  queueId?: string;
  /** Why the last attempt failed, safe to show. */
  queueError?: string;
  /** The server refused it: it waits for Retry or Discard. */
  queueHeld?: boolean;
  text: string;
  time: string;
  disappearing?: boolean;
  /** Real DMs only: the server message id + timestamp, used to dedupe while polling. */
  id?: string;
  createdAt?: number;
  /** Real DMs only, own messages ("me") only: true once the other participant has a 'read' receipt for it. Undefined for received messages. */
  seen?: boolean;
  /** Real DMs only, own messages ("me") only: true once the other participant has at least a 'delivered' receipt (set regardless of whether read receipts are on — see server/src/services/messageService.ts). Distinct from `seen`/'read': a message can be delivered without being read. Undefined for received messages. */
  delivered?: boolean;
  /** Real DMs only: epoch ms this message is scheduled to be auto-deleted at retention's 6-month cutoff (null if it's already been manually deleted server-side). Drives Thread.tsx's retention warning banner. */
  deleteAt?: number | null;
  /** Present for file messages — `text` is then empty. */
  file?: FileAttachmentView;
  /** Real DMs only: the message this one replies to (quote carried encrypted with the reply). */
  replyTo?: ReplyRef & { fromMe: boolean };
  /** Real DMs only: reactions to this message, one entry per emoji. */
  reactions?: MessageReaction[];
  /** Own ("me") messages only: stage-3 verification outcome. Undefined until either just-sent (computed immediately) or lazily fetched (see state/actions.ts's loadMessageVerification) — the reducer preserves it across poll refreshes once known. */
  security?: MessageSecurity;
  /** Own ("me") messages only, base64: the exact encrypted envelope sent, kept around so verification can be recomputed later without re-fetching it. */
  ciphertextB64?: string;
  nonceB64?: string;
  /** Set once its sender edited it: when the latest version was sent, and the earlier ones (oldest first). */
  editedAt?: number;
  earlierVersions?: { text: string; at: number }[];
  /** The edit messages carrying its later versions (deleted along with it). */
  editMessageIds?: string[];
  /** Forwarded from another chat. */
  forwarded?: boolean;
  /** Forwarded from a channel: which post, shown as "Forwarded from <channel>" (lib/richMessage.ts). */
  channelPost?: { channelId: string; postId: string; channelName: string };
  /** A reply or reaction to a story (lib/richMessage.ts). */
  storyReply?: StoryReplyRef;
  /** A poll (then `text` is its question). */
  poll?: PollView;
  /** Something that happened, shown as a small line instead of a bubble: a pin (`text` is the pinned message's preview) or an unpin. */
  /** A pin or unpin of `targetId`; `targetGone` when that message was deleted (or hidden here), so it's no longer pinned. */
  notice?: { kind: "pin" | "unpin"; targetId: string; targetGone?: boolean };
  /** A group message without the signature its sender's apps now add (lib/messageSigning.ts): an older app of theirs, or a forgery. */
  unverifiedSender?: boolean;
  /** Sent with the older conversation-key protocol in a chat that already uses forward secrecy here (lib/e2eV2/pins.ts). */
  withoutForwardSecrecy?: boolean;
}

export interface DMConversation {
  kind: "dm";
  id: string; // real conversation id from the server
  handle: string; // the other participant's username, as registered
  verified: boolean;
  /** Their key differs from the one this browser first saw for them (lib/verifiedContacts.ts's hasKeyChanged): warn until re-verified. */
  keyChanged?: boolean;
  disappearing: DurationKey;
  time: string; // list-row timestamp label
  unread: number;
  messages: Message[];
  /** Real E2E fields — absent for nothing in this app anymore, but kept optional so tests can build bare fixtures. */
  userId?: string; // the other participant's userId
  publicKey?: Uint8Array; // the other participant's public key
  conversationKey?: Uint8Array; // this conversation's unwrapped symmetric key
  /** Why this chat\'s key failed a trust check (lib/keyTrust.ts). Nothing can be sent. */
  keyProblem?: string;
  /** 12 groups of 5 digits — real fingerprint of both participants' public keys, precomputed when the contact is added/loaded. */
  safetyNumber?: string[];
  /** The other participant's profile picture — real, server-hosted, visible to every user. Null if they haven't set one. */
  avatarUrl?: string | null;
}

export type GroupRole = "owner" | "admin" | "member";

/** One member of a server-backed group, the signed-in user included (`isSelf`). */
export interface GroupMember {
  userId: string;
  handle: string;
  avatarUrl: string | null;
  role: GroupRole;
  /** base64, needed to seal the group key for them and to check verification. */
  publicKey: string;
  isSelf: boolean;
}

/**
 * A server-backed group. The name is encrypted with the group key (the
 * server never sees it); every member has their own sealed copy of the key.
 * Messages use the same encryption as DMs; `Message.from` is the sender's
 * userId (or "me").
 */
export interface GroupConversation {
  kind: "group";
  id: string;
  label: string;
  members: GroupMember[];
  myRole: GroupRole;
  disappearing: DurationKey;
  time: string;
  unread: number;
  messages: Message[];
  conversationKey?: Uint8Array;
  /** Why this chat\'s key failed a trust check (lib/keyTrust.ts). Nothing can be sent. */
  keyProblem?: string;
  /** Set when this group is a community (paid or free): membership and keys are managed on its page. */
  community?: CommunityLink;
}

export interface CommunityLink {
  id: string;
  kind: "channel" | "group";
  role: "owner" | "admin" | "moderator" | "member";
  /** A channel's subscribers read; the creator and staff post. */
  canPost: boolean;
  status: string;
}

export type Conversation = DMConversation | GroupConversation;

export interface AppSettings {
  readReceipts: boolean; // default false
  typingIndicators: boolean; // default false
  defaultDisappearing: DurationKey; // default "off" — applies to new conversations only
  appLock: boolean; // default true
  switcherBlur: boolean; // default true
  /** Real, server-persisted (unlike the settings above) — false rejects a stranger's attempt to message you. Default true. */
  acceptMessageRequests: boolean;
  /** Real, server-persisted, opt-in and off by default: while true, this user's own outgoing messages' ciphertext (never plaintext) appears in the public live transparency feed. See server/src/services/transparencyService.ts. */
  transparencyMode: boolean;
  /** Real, server-persisted, off by default: contacts who share theirs see when you're online, and you see theirs. */
  showOnlineStatus: boolean;
}

export type UserRole = "user" | "super_admin";
export type SubscriptionTier = "free" | "pro" | "premium";

/** Plans in order: each includes everything the one before it has. */
const TIER_RANK: Record<SubscriptionTier, number> = { free: 0, pro: 1, premium: 2 };

/** Whether `tier` includes everything `required` does: Pro sends files and makes voice call links; Premium adds video call links and screen sharing. One-to-one calls are free. */
export function tierAtLeast(tier: SubscriptionTier, required: SubscriptionTier): boolean {
  return (TIER_RANK[tier] ?? 0) >= TIER_RANK[required];
}

export const TIER_LABELS: Record<SubscriptionTier, string> = { free: "Free", pro: "Pro", premium: "Premium" };

export interface SelfIdentity {
  handle: string; // == username, chosen (generated) at registration, immutable after
  userId: string;
  publicKey: Uint8Array;
  /** Unwrapped private key, in memory only for this session — never persisted, never sent anywhere. */
  privateKey: Uint8Array;
  /** Real, server-hosted profile picture, visible to every user — a deliberate exception to "no profile, ever." Null if not set. */
  avatarUrl: string | null;
  /** Server-assigned. "super_admin" is granted only via a one-off manual DB update — never through any in-app path. */
  role: UserRole;
  /** The plan: gates sending files (Pro), call links (Pro voice, Premium video) and screen sharing (Premium). One-to-one calls are free. The server enforces each. */
  subscriptionTier: SubscriptionTier;
  /** Whether TOTP MFA is currently enabled on this account. */
  mfaEnabled: boolean;
}

/** A hydrated friend request (incoming or outgoing) — deliberately not a DMConversation: no conversation/key exists until it's accepted. */
export interface FriendRequestView {
  id: string; // friend_requests.id
  userId: string; // the other side: requester (incoming) or recipient (outgoing)
  handle: string;
  avatarUrl: string | null;
  createdAt: string;
}

export type ProfileOrigin =
  | { screen: "new-contact" }
  | { screen: "friend-requests" }
  | { screen: "thread"; conversationId: string }
  | { screen: "group-info"; conversationId: string };

/** A group's trust state is never stored — computed live each render: other members whose 1:1 safety number you've verified. */
export function verifiedCount(group: GroupConversation, contactsById: Map<string, DMConversation>): number {
  const verifiedUserIds = new Set([...contactsById.values()].filter((c) => c.verified && c.userId).map((c) => c.userId));
  return group.members.filter((m) => !m.isSelf && verifiedUserIds.has(m.userId)).length;
}
