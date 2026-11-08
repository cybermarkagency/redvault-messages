// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Async orchestration for the real backend: crypto + network calls that end
// by dispatching one or more plain, synchronous actions into the reducer.
// Screens call these instead of dispatching network-shaped actions directly.
import type { Dispatch } from "react";
import * as api from "../lib/api.js";
import { ApiError } from "../lib/api.js";
import * as commitment from "../lib/commitment.js";
import * as crypto from "../lib/e2eCrypto.js";
import { currentForwardSecretDevice, resetForwardSecretDevice } from "../lib/e2eV2/device.js";
import { sendEncryptedMessage } from "../lib/e2eV2/messaging.js";
import { forwardSecretSince, pinForwardSecret } from "../lib/e2eV2/pins.js";
import { loadPin, membersToSign } from "../lib/keyTrust.js";
import { adoptGroupNotice, checkedLatestNotice, currentKey, keyRingFor, keyRingFromDto, noticeAdding, publishGroupNotice, rememberKeyRing, rotateGroupKey, verifyGroupRing, setKeyRingOwner } from "../lib/groupKeys.js";
import { checkSender, forgetKeylessSenders, resolveSenderKeys } from "../lib/messageSigning.js";
import { decryptForCommunity } from "../lib/communityCrypto.js";
import { communityRingFor, ensureCommunityRing, forgetCommunityKeys, setCommunityIdentity } from "../lib/communityKeys.js";
import { decryptFile, encryptFile, FileDecryptError, FILE_ALGORITHM } from "../lib/fileCrypto.js";
import { isFileMessage, parseFileDescriptor, sanitizeFilename, serializeFileDescriptor, type FileDescriptor } from "../lib/fileMessage.js";
import { downloadCiphertext, TransferError, uploadCiphertext } from "../lib/fileTransfer.js";
import { hideMessage, loadHidden } from "../lib/hiddenMessages.js";
import { loadLastHandle, saveLastHandle } from "../lib/lastIdentity.js";
import { credentialFor, movePin, newCredential, passwordProof, pinSignInKeys } from "../lib/authKey.js";
import { rememberWrappedKey } from "../lib/passwordVault.js";
import {
  formatRecoveryKey,
  generateRecoveryKey,
  parseRecoveryKey,
  recoveryAuthFor,
  recoveryMaterialFor,
  unwrapWithRecoveryKey,
} from "../lib/recoveryKey.js";
import {
  isStructured,
  parseStructured,
  REACTION_EMOJI,
  serializeEdit,
  serializeEndPoll,
  serializeChannelForward,
  serializeStoryReply,
  serializeForwarded,
  serializePin,
  serializePoll,
  serializeReaction,
  serializeReply,
  serializeUnpin,
  serializeVote,
  type ChannelPostRef,
  type StoryReplyRef,
  type ReactionEmoji,
  type ReplyRef,
} from "../lib/richMessage.js";
import { inviteKeys } from "../lib/groupInviteCrypto.js";
import { hasKeyChanged, isVerified, requireUnchangedKey } from "../lib/verifiedContacts.js";
import {
  DURATION_SECONDS,
  durationKeyFromSeconds,
  type Conversation,
  type DMConversation,
  type DurationKey,
  type FriendRequestView,
  type GroupConversation,
  type GroupMember,
  type Message,
  type MessageReaction,
  type MessageSecurity,
  type SelfIdentity,
  type SubscriptionTier,
} from "../types.js";
import type { Action } from "./reducer.js";
import * as outbox from "../lib/outbox.js";
import { isNetworkFailure, onOnionOrigin, TOR_UNAVAILABLE_MESSAGE, unreachableMessage } from "../lib/torRouting.js";
import { nowLabel } from "./reducer.js";

function describeError(err: unknown, fallback = "Something went wrong."): string {
  // Never the browser's own wording ("NetworkError when attempting to fetch resource.").
  if (isNetworkFailure(err)) return unreachableMessage();
  if (err instanceof ApiError) return err.message || fallback;
  if (err instanceof Error) return err.message || fallback;
  return fallback;
}

function timeLabel(iso: string): string {
  return new Date(iso).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
}

/**
 * Stage 3 ("Transaction verified"): independently recomputes the local
 * commitment over the exact envelope just sent and compares it against the
 * server's own independently-computed commitment (see
 * server/src/services/transparencyService.ts). Never assumes a match —
 * mismatches surface as `security.verification === "failed"`, which the UI
 * must show as a real failure, not silently ignore (see notification.md).
 */
async function buildSecurity(
  messageId: string,
  ciphertext: Uint8Array,
  nonce: Uint8Array,
  verification: api.VerificationDto | null
): Promise<MessageSecurity> {
  if (!verification) {
    return { encryptedLocally: true, relayed: true, verification: "unavailable" };
  }
  await crypto.ready();
  const local = commitment.computeCommitment({ messageId, ciphertext, nonce }, verification.protocolVersion);
  const localCommitment = commitment.commitmentToHex(local);
  const matches = commitment.commitmentsMatch(localCommitment, verification.commitment);
  return {
    encryptedLocally: true,
    relayed: true,
    verification: matches ? "verified" : "failed",
    publicTransactionId: verification.publicTransactionId,
    protocolVersion: verification.protocolVersion,
    localCommitment,
    networkCommitment: verification.commitment,
  };
}

/**
 * On-demand re-verification for a message that doesn't already have
 * `security` computed — e.g. loaded from a poll/reload rather than sent
 * this session. Recomputes the local commitment from the envelope stashed
 * on the message (`ciphertextB64`/`nonceB64` — see fetchAndDecryptMessages)
 * and fetches the server's record fresh, so this also catches a message
 * whose transparency record didn't exist yet the first time it was viewed.
 */
export async function loadMessageVerification(dispatch: Dispatch<Action>, conversationId: string, message: Message): Promise<void> {
  if (!message.id || !message.ciphertextB64 || !message.nonceB64) return;
  const messageId = message.id;
  try {
    const verification = await api.getMessageVerification(messageId);
    const security = await buildSecurity(
      messageId,
      crypto.fromBase64(message.ciphertextB64),
      crypto.fromBase64(message.nonceB64),
      verification
    );
    dispatch({ type: "MESSAGE_SECURITY_UPDATED", conversationId, messageId, security });
  } catch {
    dispatch({
      type: "MESSAGE_SECURITY_UPDATED",
      conversationId,
      messageId,
      security: { encryptedLocally: true, relayed: true, verification: "unavailable" },
    });
  }
}

/** What the person ticked on the sign-up form, sent as is: the server decides. */
export interface SignupConsent {
  termsAccepted: boolean;
  privacyAcknowledged: boolean;
  age18Confirmed: boolean;
}

export async function registerIdentity(
  dispatch: Dispatch<Action>,
  handle: string,
  password: string,
  consent: SignupConsent
): Promise<void> {
  dispatch({ type: "AUTH_STARTED" });
  try {
    await crypto.ready();
    const keyPair = crypto.generateKeyPair();
    const wrapped = crypto.wrapPrivateKey(keyPair.privateKey, password);
    // A recovery key is created with every account and shown once, right after sign-up.
    const recoveryKey = generateRecoveryKey();
    const recovery = await recoveryMaterialFor(recoveryKey, keyPair.privateKey);
    const credential = await newCredential(handle, password, wrapped.kdfSalt);
    const result = await api.register({
      recovery,
      username: handle,
      ...credential,
      publicKey: crypto.toBase64(keyPair.publicKey),
      encryptedPrivateKey: crypto.toBase64(wrapped.encryptedPrivateKey),
      privateKeyNonce: crypto.toBase64(wrapped.nonce),
      privateKeyKdfSalt: crypto.toBase64(wrapped.kdfSalt),
      kdfOps: wrapped.kdf?.ops,
      kdfMem: wrapped.kdf?.mem,
      terms_accepted: consent.termsAccepted,
      privacy_acknowledged: consent.privacyAcknowledged,
      age_18_confirmed: consent.age18Confirmed,
    });
    if (credential.authVersion === 2) pinSignInKeys(handle);
    const identity: SelfIdentity = {
      handle,
      userId: result.userId,
      publicKey: keyPair.publicKey,
      privateKey: keyPair.privateKey,
      avatarUrl: result.avatarUrl,
      role: result.role,
      subscriptionTier: result.subscriptionTier,
      mfaEnabled: false, // freshly registered accounts never start with MFA on
    };
    saveLastHandle(handle);
    rememberWrappedKey(wrapped);
    dispatch({
      type: "AUTH_SUCCEEDED",
      identity,
      acceptMessageRequests: result.acceptMessageRequests,
      transparencyMode: result.transparencyMode,
      showOnlineStatus: result.showOnlineStatus ?? false,
    });
    dispatch({ type: "OPEN_RECOVERY_KEY", issuedKey: formatRecoveryKey(recoveryKey) });
  } catch (err) {
    dispatch({ type: "AUTH_FAILED", message: describeError(err, "Couldn't create that identity.") });
  }
}

/** Builds the in-memory identity from a completed login/MFA-verify result (both share the same LoginResult shape) and unwraps the private key with the just-entered password. `mfaEnabled` is threaded in by the caller since it's determined by which code path reached here, not by anything in the result itself. */
function identityFromLoginResult(
  handle: string,
  password: string,
  result: api.LoginResult,
  mfaEnabled: boolean
): SelfIdentity {
  const wrapped = {
    encryptedPrivateKey: crypto.fromBase64(result.encryptedPrivateKey),
    nonce: crypto.fromBase64(result.privateKeyNonce),
    kdfSalt: crypto.fromBase64(result.privateKeyKdfSalt),
    kdf: result.privateKeyKdfOps && result.privateKeyKdfMem ? { ops: result.privateKeyKdfOps, mem: result.privateKeyKdfMem } : undefined,
  };
  const privateKey = crypto.unwrapPrivateKey(wrapped, password);
  // Other people's keys get sealed to this one, and copies of our own: it has to be ours.
  if (crypto.toBase64(crypto.publicKeyOf(privateKey)) !== crypto.toBase64(crypto.fromBase64(result.publicKey))) {
    throw new Error("This account's keys don't match. Nothing was opened.");
  }
  // Kept in memory so the password manager can be unlocked with this password later.
  rememberWrappedKey(wrapped);
  return {
    handle,
    userId: result.userId,
    publicKey: crypto.fromBase64(result.publicKey),
    privateKey,
    avatarUrl: result.avatarUrl,
    role: result.role,
    subscriptionTier: result.subscriptionTier,
    mfaEnabled,
  };
}

export async function loginIdentity(dispatch: Dispatch<Action>, handle: string, password: string): Promise<void> {
  dispatch({ type: "AUTH_STARTED" });
  try {
    await crypto.ready();
    const credential = await credentialFor(handle, password);
    const result = await api.login(handle, credential);
    if (credential.authVersion === 2 || credential.upgradeAuthKey) pinSignInKeys(handle);
    if ("mfaRequired" in result) {
      // Password was correct, but no key material is returned until the
      // 6-digit code is verified — stay on a distinct "mfa-pending" status
      // rather than either signing in or bouncing back to signed-out.
      dispatch({ type: "AUTH_MFA_REQUIRED" });
      return;
    }
    // Reaching here (not "mfaRequired") means this account has no MFA.
    const identity = identityFromLoginResult(handle, password, result, false);
    saveLastHandle(handle);
    dispatch({
      type: "AUTH_SUCCEEDED",
      identity,
      acceptMessageRequests: result.acceptMessageRequests,
      transparencyMode: result.transparencyMode,
      showOnlineStatus: result.showOnlineStatus ?? false,
    });
  } catch (err) {
    // Deliberately the same message whether the handle doesn't exist, the
    // server rejected the password, or the private key failed to decrypt
    // locally (wrong password) — never leak which one, and never leak the
    // raw AEAD error text. The one exception: the server was never reached,
    // which says nothing about the account (and over Tor happens routinely).
    dispatch({ type: "AUTH_FAILED", message: isNetworkFailure(err) ? unreachableMessage() : "Wrong handle or password." });
  }
}

/** Completes a login that returned {mfaRequired: true} — verifies the 6-digit authenticator code and, on success, finishes exactly like a normal login would (full key material, same AUTH_SUCCEEDED). A wrong code dispatches AUTH_MFA_FAILED, which deliberately keeps authStatus at "mfa-pending" instead of bouncing back to signed-out. */
export async function submitMfaCode(
  dispatch: Dispatch<Action>,
  handle: string,
  password: string,
  code: string
): Promise<void> {
  try {
    await crypto.ready();
    const result = await api.verifyMfaCode(code);
    const identity = identityFromLoginResult(handle, password, result, true);
    saveLastHandle(handle);
    dispatch({
      type: "AUTH_SUCCEEDED",
      identity,
      acceptMessageRequests: result.acceptMessageRequests,
      transparencyMode: result.transparencyMode,
      showOnlineStatus: result.showOnlineStatus ?? false,
    });
  } catch (err) {
    dispatch({ type: "AUTH_MFA_FAILED", message: describeError(err, "Wrong code. Try again.") });
  }
}

export async function logout(dispatch: Dispatch<Action>): Promise<void> {
  try {
    await api.logout();
  } catch {
    // Best-effort — the client forgets its keys either way.
  }
  rememberWrappedKey(null);
  forgetCommunityKeys();
  dispatch({ type: "LOGOUT" });
}

/**
 * Fetches + decrypts a DM's messages, and also computes real, server-backed
 * read state from the conversation's receipts: `unread` counts incoming
 * messages the caller hasn't marked delivered/read yet (survives reload —
 * unlike the old "all non-me messages" guess), and each of the caller's own
 * messages gets `seen: true` once the other participant has a 'read'
 * receipt for it.
 */
async function fetchAndDecryptMessages(
  conversationId: string,
  conversationKey: Uint8Array,
  selfUserId: string,
  /** The other participant(s): one for a DM, every other member for a group. */
  otherUserIds: string[]
): Promise<{ messages: Message[]; unread: number }> {
  const device = await currentForwardSecretDevice();
  const listed = async () => {
    if (!device || device.userId !== selfUserId) return { dtos: await api.listMessages(conversationId), device: null };
    try {
      return { dtos: await api.listMessages(conversationId, { deviceId: device.deviceId }), device };
    } catch (err) {
      if (!(err instanceof ApiError && err.status === 404)) throw err;
      resetForwardSecretDevice(); // The server dropped this device; the next use registers a new one.
      return { dtos: await api.listMessages(conversationId), device: null };
    }
  };
  const [{ dtos, device: reader }, receipts] = await Promise.all([listed(), api.listReceipts(conversationId)]);
  // Communities: each message names the epoch whose key encrypted it.
  const communityRing = communityRingFor(conversationId);
  // Forward-secret (protocol 2) messages: decrypted once by this device and
  // kept locally. One sent before this device existed has nothing for it
  // and isn't shown, like on a newly linked phone.
  const forwardSecret = reader ? await reader.read(conversationId, dtos) : new Map<string, string | null>();
  // Every send gets a fresh nonce, so identical bytes under another id are a replay and show once.
  const seenSends = new Set<string>();
  const live = dtos.filter((m) => {
    if (m.deletedAt || (m.protocol === 2 && !forwardSecret.has(m.id))) return false;
    const send = `${m.nonce}:${m.ciphertext}`;
    if (seenSends.has(send)) return false;
    seenSends.add(send);
    return true;
  });
  // A forward-secret message read here pins the chat to protocol 2 on this
  // browser (lib/e2eV2/pins.ts); later conversation-key messages are marked.
  const firstForwardSecret = live.find((m) => m.protocol === 2 && forwardSecret.get(m.id) != null);
  if (firstForwardSecret) pinForwardSecret(selfUserId, conversationId, firstForwardSecret.createdAt);
  const pinnedSince = forwardSecretSince(selfUserId, conversationId);
  const ring = keyRingFor(conversationId);
  const isGroup = ring?.isGroup ?? false;
  forgetKeylessSenders(live.filter((m) => m.protocol !== 2 && m.signature).map((m) => m.senderId));
  await resolveSenderKeys(
    selfUserId,
    live.filter((m) => m.protocol !== 2).map((m) => m.senderId)
  );
  // Deleted for everyone: a pin of one of these drops out of the pinned bar.
  const deletedIds = new Set(dtos.filter((m) => m.deletedAt).map((m) => m.id.toLowerCase()));

  const myReceiptMessageIds = new Set(
    receipts.filter((r) => r.userId === selfUserId).map((r) => r.messageId)
  );
  // A message counts as read/delivered once EVERY other participant has read/received it
  // (for a DM that's just the one other person). The server always upserts
  // 'delivered' alongside 'read', and 'read' implies delivered here too.
  const readBy = new Map<string, Set<string>>();
  const deliveredBy = new Map<string, Set<string>>();
  for (const r of receipts) {
    if (!otherUserIds.includes(r.userId)) continue;
    const target = r.status === "read" ? readBy : deliveredBy;
    if (!target.has(r.messageId)) target.set(r.messageId, new Set());
    target.get(r.messageId)!.add(r.userId);
    if (r.status === "read") {
      if (!deliveredBy.has(r.messageId)) deliveredBy.set(r.messageId, new Set());
      deliveredBy.get(r.messageId)!.add(r.userId);
    }
  }
  const byAll = (map: Map<string, Set<string>>, id: string) => otherUserIds.length > 0 && (map.get(id)?.size ?? 0) >= otherUserIds.length;

  // Reactions arrive as their own (hidden) messages; they're folded into
  // the message they react to below, and never shown on their own. Edits,
  // votes and a poll's end are folded the same way; pins stay as notices.
  const hidden = loadHidden(selfUserId);
  const reactionIds = new Set<string>();
  const reactionsByTarget = new Map<string, Map<ReactionEmoji, ReactionTally>>();
  const edits: { id: string; targetId: string; senderId: string; text: string; at: number }[] = [];
  const pollEvents: { pollId: string; senderId: string; at: number; end: boolean; options: number[] }[] = [];
  const senderOf = new Map<string, string>();

  const messages = live
    .map((m): Message | null => {
      const from = m.senderId === selfUserId ? ("me" as const) : m.senderId;
      // Protocol 1: the key of the epoch it was sent under, and the sender's
      // signature. One message that can't be opened or checked never stops
      // the rest of the chat from loading. A community's messages open with the
      // community's own epoch key and aren't signed.
      const check = m.protocol === 2 || communityRing ? "verified" : checkSender({ ...m, conversationId }, isGroup);
      let plaintext: string;
      if (m.protocol === 2) {
        plaintext = forwardSecret.get(m.id) ?? UNDECRYPTABLE;
      } else if (check === "invalid") {
        plaintext = UNVERIFIABLE;
      } else if (communityRing) {
        plaintext = decryptForCommunity(communityRing, m.keyEpoch ?? 0, crypto.fromBase64(m.ciphertext), crypto.fromBase64(m.nonce)) ?? UNDECRYPTABLE;
      } else {
        const key = ring?.keys.get(m.keyEpoch ?? 0) ?? (ring ? undefined : conversationKey);
        try {
          if (!key) throw new Error("no key for this epoch");
          plaintext = crypto.decryptWithConversationKey(crypto.fromBase64(m.ciphertext), crypto.fromBase64(m.nonce), key);
        } catch {
          plaintext = UNDECRYPTABLE;
        }
      }
      const unverified = check === "unverified";
      const structured = isStructured(plaintext) ? parseStructured(plaintext) : null;
      // Edits, reactions, votes and pins only count when we know who sent them.
      const acts =
        structured?.kind === "reaction" ||
        (structured?.kind === "text" && (structured.edits || structured.vote || structured.endPoll || structured.pin || structured.unpin));
      if (unverified && acts) return null;
      if (structured?.kind === "reaction") {
        reactionIds.add(m.id);
        const reactedAt = new Date(m.createdAt).getTime();
        const byEmoji = reactionsByTarget.get(structured.messageId) ?? new Map<ReactionEmoji, ReactionTally>();
        const entry = byEmoji.get(structured.emoji) ?? { senders: new Set<string>(), firstAt: reactedAt };
        entry.firstAt = Math.min(entry.firstAt, reactedAt);
        entry.senders.add(m.senderId);
        if (from === "me") entry.myReactionId = m.id;
        byEmoji.set(structured.emoji, entry);
        reactionsByTarget.set(structured.messageId, byEmoji);
        return null;
      }
      const at = new Date(m.createdAt).getTime();
      if (structured?.kind === "text" && structured.edits) {
        reactionIds.add(m.id);
        edits.push({ id: m.id, targetId: structured.edits, senderId: m.senderId, text: structured.text, at });
        return null;
      }
      if (structured?.kind === "text" && (structured.vote || structured.endPoll)) {
        reactionIds.add(m.id);
        pollEvents.push({
          pollId: structured.vote?.pollId ?? structured.endPoll!,
          senderId: m.senderId,
          at,
          end: !structured.vote,
          options: structured.vote?.options ?? [],
        });
        return null;
      }
      // Pin notices aren't new messages for the unread count.
      if (structured?.kind === "text" && (structured.pin || structured.unpin)) reactionIds.add(m.id);
      if (hidden.has(m.id)) return null;
      senderOf.set(m.id.toLowerCase(), m.senderId);
      const file = isFileMessage(plaintext) ? fileViewFor(plaintext, m.attachments ?? []) : undefined;
      const text = file
        ? ""
        : structured?.kind === "text"
          ? structured.text
          : structured?.kind === "invalid" || isFileMessage(plaintext)
            ? "Unreadable message."
            : plaintext;
      const replyTo =
        structured?.kind === "text" && structured.replyTo
          ? { ...structured.replyTo, fromMe: structured.replyTo.senderId === selfUserId }
          : undefined;
      const extras: Partial<Message> = {};
      let shownText = text;
      if (structured?.kind === "text") {
        // A poll shows its question; a pin notice the pinned message's preview.
        if (structured.poll) shownText = structured.poll.question;
        else if (structured.pin) shownText = structured.pin.preview;
        else if (structured.unpin) shownText = "";
        if (structured.forwarded) extras.forwarded = true;
        if (structured.channelPost) extras.channelPost = structured.channelPost;
        if (structured.storyReply) extras.storyReply = structured.storyReply;
        if (structured.poll) {
          const { question, options, multiple } = structured.poll;
          extras.poll = { question, options, multiple, closed: false, counts: options.map(() => 0), mine: [], voters: 0 };
        }
        if (structured.pin) {
          const targetId = structured.pin.messageId;
          const gone = deletedIds.has(targetId) || hidden.has(targetId);
          extras.notice = { kind: "pin", targetId, ...(gone ? { targetGone: true } : {}) };
        }
        if (structured.unpin) extras.notice = { kind: "unpin", targetId: structured.unpin };
      }
      if (unverified) extras.unverifiedSender = true;
      if (m.protocol !== 2 && pinnedSince && m.createdAt >= pinnedSince) extras.withoutForwardSecrecy = true;
      return {
        id: m.id,
        from,
        text: shownText,
        file,
        replyTo,
        ...extras,
        time: timeLabel(m.createdAt),
        createdAt: new Date(m.createdAt).getTime(),
        seen: from === "me" ? byAll(readBy, m.id) : undefined,
        delivered: from === "me" ? byAll(deliveredBy, m.id) : undefined,
        deleteAt: m.deleteAt ? new Date(m.deleteAt).getTime() : null,
        // Stashed so a message loaded from a poll/reload (rather than sent
        // this session) can still have its stage-3 verification recomputed
        // on demand — see loadMessageVerification.
        ciphertextB64: from === "me" ? m.ciphertext : undefined,
        nonceB64: from === "me" ? m.nonce : undefined,
      };
    })
    .filter((m): m is Message => m !== null)
    .map((m) => foldEditsAndVotes(m, edits, pollEvents, senderOf, selfUserId))
    .map((m) => {
      const byEmoji = m.id ? reactionsByTarget.get(m.id.toLowerCase()) : undefined;
      if (!byEmoji) return m;
      return { ...m, reactions: orderReactions(byEmoji) };
    })
    .sort((a, b) => (a.createdAt ?? 0) - (b.createdAt ?? 0));

  // Reactions don't count as unread messages.
  const unread = live.filter(
    (m) => m.senderId !== selfUserId && !myReceiptMessageIds.has(m.id) && !reactionIds.has(m.id)
  ).length;

  return { messages, unread };
}

interface ReactionTally {
  senders: Set<string>;
  myReactionId?: string;
  /** When this emoji was first used on the message. */
  firstAt: number;
}

/** The quick six first, in their order; any other emoji after them, in the order they were first used. */
export function orderReactions(byEmoji: Map<ReactionEmoji, ReactionTally>): MessageReaction[] {
  const quick = (emoji: string) => {
    const index = REACTION_EMOJI.indexOf(emoji);
    return index < 0 ? REACTION_EMOJI.length : index;
  };
  return [...byEmoji.entries()]
    .sort(([a, ta], [b, tb]) => quick(a) - quick(b) || ta.firstAt - tb.firstAt)
    .map(([emoji, entry]) => ({ emoji, count: entry.senders.size, mine: entry.myReactionId !== undefined, myReactionId: entry.myReactionId }));
}

/**
 * A message's later versions (only its own sender may edit it, and only
 * text), and a poll's votes: each person's latest vote counts until the
 * poll's creator ends it.
 */
export function foldEditsAndVotes(
  message: Message,
  edits: { id: string; targetId: string; senderId: string; text: string; at: number }[],
  pollEvents: { pollId: string; senderId: string; at: number; end: boolean; options: number[] }[],
  senderOf: Map<string, string>,
  selfUserId: string
): Message {
  if (!message.id) return message;
  const id = message.id.toLowerCase();
  const sender = senderOf.get(id);
  let out = message;
  const mine = edits.filter((e) => e.targetId === id && e.senderId === sender).sort((a, b) => a.at - b.at);
  if (mine.length > 0 && !message.file && !message.poll && !message.notice) {
    const earlierVersions: { text: string; at: number }[] = [];
    let text = message.text;
    let at = message.createdAt ?? 0;
    for (const edit of mine) {
      earlierVersions.push({ text, at });
      text = edit.text;
      at = edit.at;
    }
    out = { ...out, text, editedAt: at, earlierVersions, editMessageIds: mine.map((e) => e.id) };
  }
  if (message.poll) {
    const poll = message.poll;
    const events = pollEvents.filter((e) => e.pollId === id).sort((a, b) => a.at - b.at);
    const endedAt = events.find((e) => e.end && e.senderId === sender)?.at;
    const latest = new Map<string, number[]>();
    for (const event of events) {
      if (event.end || (endedAt !== undefined && event.at > endedAt)) continue;
      let picked = event.options.filter((o) => o < poll.options.length);
      if (!poll.multiple) picked = picked.slice(0, 1);
      latest.set(event.senderId, picked);
    }
    const counts = poll.options.map(() => 0);
    let voters = 0;
    let mineVotes: number[] = [];
    for (const [voter, picked] of latest) {
      if (picked.length === 0) continue;
      voters++;
      for (const o of picked) counts[o]++;
      if (voter === selfUserId) mineVotes = picked;
    }
    out = { ...out, poll: { ...poll, closed: endedAt !== undefined, counts, mine: mineVotes, voters } };
  }
  return out;
}

/** Pinned messages from the pin notices in a chat, oldest pin first; at most three stay pinned. */
export function pinnedFrom(messages: Message[]): { messageId: string; preview: string; by: string }[] {
  const pinned: { messageId: string; preview: string; by: string }[] = [];
  for (const m of messages) {
    if (!m.notice) continue;
    const index = pinned.findIndex((p) => p.messageId === m.notice!.targetId);
    if (index >= 0) pinned.splice(index, 1);
    if (m.notice.kind === "unpin" || m.notice.targetGone) continue;
    pinned.push({ messageId: m.notice.targetId, preview: m.text, by: m.from });
    if (pinned.length > MAX_PINS) pinned.shift();
  }
  return pinned;
}

export const MAX_PINS = 3;

/** Stands in for a key that failed its trust check: it opens nothing. */
const NO_KEY = new Uint8Array(32);

/** Shown in place of a message this device couldn't decrypt. */
const UNDECRYPTABLE = "Couldn't decrypt this message.";
/** Shown in place of a message whose signature doesn't match its sender (lib/messageSigning.ts). */
const UNVERIFIABLE = "Couldn't verify who sent this message.";

/**
 * A file message only counts if the server also lists that object as this
 * message's attachment — so a descriptor can't point the recipient at some
 * other object, and a pasted/forged descriptor doesn't render as a file.
 */
function fileViewFor(plaintext: string, attachments: api.AttachmentDto[]): Message["file"] | undefined {
  const descriptor = parseFileDescriptor(plaintext);
  if (!descriptor) return undefined;
  const attachment = attachments.find((a) => a.objectId === descriptor.objectId);
  if (!attachment) return undefined;
  return { descriptor, status: attachment.status };
}

/** Turns one non-group conversation DTO into a fully-hydrated DMConversation: unseal its key, resolve the other participant, decrypt its messages. Returns null if it isn't a well-formed 1:1 (e.g. the other participant already left). */
async function hydrateDm(dto: api.ConversationDto, identity: SelfIdentity): Promise<DMConversation | null> {
  const ring = keyRingFromDto(dto, identity);
  rememberKeyRing(ring);
  const conversationKey = ring.keys.get(ring.currentEpoch) ?? NO_KEY;
  const participants = await api.listParticipants(dto.id);
  const other = participants.find((p) => p.userId !== identity.userId);
  if (!other) return null;

  const otherUser = await api.getUser(other.userId);
  const publicKey = crypto.fromBase64(otherUser.publicKey);
  const { messages, unread } = await fetchAndDecryptMessages(dto.id, conversationKey, identity.userId, [other.userId]);

  return {
    kind: "dm",
    id: dto.id,
    handle: otherUser.username,
    verified: isVerified(identity.userId, otherUser.publicKey),
    keyChanged: hasKeyChanged(identity.userId, other.userId, otherUser.publicKey),
    disappearing: durationKeyFromSeconds(dto.disappearingSeconds),
    time: messages.at(-1)?.time ?? nowLabel(),
    unread,
    messages,
    userId: other.userId,
    publicKey,
    conversationKey: ring.keyProblem ? undefined : conversationKey,
    keyProblem: ring.keyProblem,
    safetyNumber: crypto.safetyNumberDigits(identity.publicKey, publicKey),
    avatarUrl: otherUser.avatarUrl,
  };
}

/** Loads every real (1:1) conversation for the signed-in identity, decrypting names/keys/messages along the way. Safe to call repeatedly (e.g. on a poll) — the reducer preserves locally-set state like `verified` across reloads. */
export async function loadConversations(dispatch: Dispatch<Action>, identity: SelfIdentity): Promise<void> {
  await crypto.ready();
  setKeyRingOwner(identity);
  setCommunityIdentity(identity);
  const dtos = await api.listConversations();
  const dms: DMConversation[] = [];

  const groups: GroupConversation[] = [];
  for (const dto of dtos) {
    // A conversation whose key can't be opened (or that fails to load for
    // any other reason) is left out of this refresh; the others still load.
    try {
      // A chat this device pinned isn't a community, whatever the server says now: communities skip those checks.
      if (dto.community && !loadPin(identity.userId, dto.id)) {
        groups.push(await hydrateCommunity(dto, identity));
        continue;
      }
      if (dto.isGroup) {
        groups.push(await hydrateGroup(dto, identity));
        continue;
      }
      const dm = await hydrateDm(dto, identity);
      if (dm) dms.push(dm);
    } catch (err) {
      if (err instanceof ApiError && err.status === 401) throw err;
      console.error("Couldn't load a conversation", err instanceof Error ? err.message : err);
    }
  }

  dispatch({ type: "CONVERSATIONS_LOADED", conversations: [...dms, ...groups] });
}

/**
 * Re-fetches and merges a single DM's messages — used for lightweight
 * polling while a thread is open. Also tells the server we've now seen
 * whatever's unread, so the conversation-list unread count (computed from
 * these same receipts — see fetchAndDecryptMessages) stays correctly at
 * zero instead of resurrecting itself on the next reload. `readReceipts`
 * picks 'read' (visible to the sender, drives their "Seen" indicator) vs
 * 'delivered' (private — still clears our own unread count).
 */
export async function refreshDmMessages(
  dispatch: Dispatch<Action>,
  conversationId: string,
  conversationKey: Uint8Array,
  selfUserId: string,
  otherUserIds: string[],
  readReceipts: boolean
): Promise<void> {
  await crypto.ready();
  const { messages, unread } = await fetchAndDecryptMessages(conversationId, conversationKey, selfUserId, otherUserIds);
  dispatch({ type: "MESSAGES_LOADED", conversationId, messages });
  if (unread > 0) {
    await api.markConversationRead(conversationId, readReceipts ? "read" : "delivered");
  }
}

export async function sendDmMessage(
  dispatch: Dispatch<Action>,
  conversationId: string,
  conversationKey: Uint8Array,
  text: string,
  replyTo?: ReplyRef & { fromMe: boolean },
  /** To someone who charges for messages: the paid message used, or the free safety contact. */
  paid?: api.PaidSend
): Promise<void> {
  // Leading NULs are reserved for the file-descriptor marker (lib/fileMessage.ts).
  const trimmed = text.replace(/^\u0000+/, "").trim();
  if (!trimmed) return;
  // Stage 1 — encrypted locally: this must complete, and the plaintext must
  // never reach the network, before anything below runs. If it throws, the
  // caller's catch block (Thread.tsx) stops here — sendMessage() is never
  // called, matching notification.md's "Encrypted locally ✗ ... the message
  // must NOT be transmitted."
  await crypto.ready();
  // A reply carries its quote inside the encrypted body; plain text stays plain.
  const body = replyTo ? serializeReply(trimmed, replyTo) : trimmed;
  // Forward-secret when the whole conversation supports it (lib/e2eV2/messaging.ts).
  //
  // Stage 2 — relayed as ciphertext: only true once the server responds
  // with a real, persisted message id (201, not just "the request was
  // sent"). `dto.verification` is the server's own independent commitment
  // over what it actually stored — see MessageService.send.
  //
  // The clientMessageId makes a repeat of this send (after a lost answer) store it once.
  const clientMessageId = outbox.newClientMessageId();
  let sent: Awaited<ReturnType<typeof sendEncryptedMessage>>;
  try {
    sent = await sendEncryptedMessage(conversationId, conversationKey, body, { clientMessageId, ...(paid ? { paid } : {}) });
  } catch (err) {
    if (!transientSendFailure(err) || !outboxKey || paid) throw err;
    // No connection (in Tor Browser: Tor or the onion service unreachable), or the server
    // briefly unavailable. It waits sealed in the outbox, shown as queued, and goes out once
    // it can: never another way. A repeat with the same clientMessageId is stored once.
    const item = await outbox.addText(outboxKey, clientMessageId, conversationId, trimmed, replyTo);
    if (isNetworkFailure(err)) dispatch({ type: "NETWORK_STATE", offline: true });
    else await outbox.recordAttempt(outboxKey, item, "The server isn't answering. It's sent when it is.", false);
    await publishOutbox(dispatch);
    return;
  }
  const { dto, ciphertext, nonce } = sent;
  const ciphertextB64 = crypto.toBase64(ciphertext);
  const nonceB64 = crypto.toBase64(nonce);

  // Stage 3 — transaction verified: recompute the same commitment locally
  // and compare.
  const security = await buildSecurity(dto.id, ciphertext, nonce, dto.verification ?? null);

  dispatch({
    type: "MESSAGE_SENT",
    conversationId,
    message: {
      id: dto.id,
      from: "me",
      text: trimmed,
      replyTo,
      time: timeLabel(dto.createdAt),
      createdAt: new Date(dto.createdAt).getTime(),
      deleteAt: dto.deleteAt ? new Date(dto.deleteAt).getTime() : null,
      security,
      ciphertextB64,
      nonceB64,
    },
  });
}

// --- The outbox (lib/outbox.ts) ---

/** Worth sending again as it is: no connection, or the server briefly unavailable (not a refusal). */
function transientSendFailure(err: unknown): boolean {
  return isNetworkFailure(err) || (err instanceof ApiError && (err.status >= 500 || err.status === 429));
}

let outboxKey: Uint8Array | null = null;
let draining = false;

/** After sign-in: opens this account's outbox and shows what's waiting in it. */
export async function beginOutbox(dispatch: Dispatch<Action>, identity: SelfIdentity): Promise<void> {
  outboxKey = await outbox.outboxKey(identity.privateKey);
  await publishOutbox(dispatch);
}

/** Sign-out: the key goes (the sealed items stay for the next sign-in, in this browser session). */
export function endOutbox(): void {
  outboxKey?.fill(0);
  outboxKey = null;
}

function queuedView(item: outbox.OutboxItem): Message {
  return {
    from: "me",
    text: item.text,
    time: timeLabel(new Date(item.createdAt).toISOString()),
    createdAt: item.createdAt,
    replyTo: item.replyTo,
    queueId: item.id,
    queueError: item.lastError,
    queueHeld: item.held,
  };
}

async function publishOutbox(dispatch: Dispatch<Action>): Promise<outbox.OutboxItem[]> {
  if (!outboxKey) return [];
  const items = await outbox.items(outboxKey).catch(() => [] as outbox.OutboxItem[]);
  dispatch({
    type: "OUTBOX_LOADED",
    queued: items.map((i) => ({ conversationId: i.conversationId, message: queuedView(i) })),
    held: items.filter((i) => i.held).length,
  });
  return items;
}

/**
 * Sends what's waiting, in order, one at a time. No connection stops the round
 * (everything waits); a refusal holds that one item for Retry or Discard. An item
 * leaves the outbox only once the server stored it.
 */
export async function drainOutbox(dispatch: Dispatch<Action>, conversations: Conversation[]): Promise<void> {
  const key = outboxKey;
  if (!key || draining) return;
  draining = true;
  try {
    const items = (await outbox.items(key)).filter((i) => !i.held);
    for (const item of items) {
      const conversation = conversations.find((c) => c.id === item.conversationId);
      if (!conversation?.conversationKey) continue; // not loaded yet: the next round
      const body = item.replyTo ? serializeReply(item.text, item.replyTo) : item.text;
      try {
        const { dto, ciphertext, nonce } = await sendEncryptedMessage(item.conversationId, conversation.conversationKey, body, {
          clientMessageId: item.id,
        });
        await outbox.remove(item.id);
        const security = await buildSecurity(dto.id, ciphertext, nonce, dto.verification ?? null);
        dispatch({
          type: "MESSAGE_SENT",
          conversationId: item.conversationId,
          replacesQueueId: item.id,
          message: {
            id: dto.id,
            from: "me",
            text: item.text,
            replyTo: item.replyTo,
            time: timeLabel(dto.createdAt),
            createdAt: new Date(dto.createdAt).getTime(),
            deleteAt: dto.deleteAt ? new Date(dto.deleteAt).getTime() : null,
            security,
            ciphertextB64: crypto.toBase64(ciphertext),
            nonceB64: crypto.toBase64(nonce),
          },
        });
        dispatch({ type: "NETWORK_STATE", offline: false });
      } catch (err) {
        if (err instanceof ApiError && err.status === 401) return;
        const network = isNetworkFailure(err);
        const transient = transientSendFailure(err);
        const shown = network
          ? onOnionOrigin()
            ? TOR_UNAVAILABLE_MESSAGE
            : "Can't reach the server. It's sent when the connection is back."
          : transient
            ? "The server isn't answering. It's sent when it is."
            : err instanceof Error && err.message
              ? err.message
              : "Couldn't send it.";
        await outbox.recordAttempt(key, item, shown, !transient);
        if (network) dispatch({ type: "NETWORK_STATE", offline: true });
        if (transient) break; // the rest wait too, in order
      }
    }
  } catch {
    /* IndexedDB unavailable: nothing queued to send */
  } finally {
    draining = false;
    await publishOutbox(dispatch);
  }
}

/** Retry: a message the server refused goes out again now. */
export async function retryQueued(dispatch: Dispatch<Action>, conversations: Conversation[], queueId: string): Promise<void> {
  if (!outboxKey) return;
  const item = (await outbox.items(outboxKey)).find((i) => i.id === queueId);
  if (item) await outbox.release(outboxKey, item);
  await drainOutbox(dispatch, conversations);
}

/** Discard: a queued message is deleted from this browser without being sent. */
export async function discardQueued(dispatch: Dispatch<Action>, queueId: string): Promise<void> {
  await outbox.remove(queueId);
  await publishOutbox(dispatch);
}

/**
 * Sets a real, server-enforced disappearing-messages timer for a DM —
 * whoever changes it, both sides see it within the next poll, and the
 * server itself auto-expires (soft-deletes) messages older than it, so
 * this doesn't depend on either client staying open. Updates optimistically
 * and reverts if the server call fails.
 */
export async function updateDisappearing(
  dispatch: Dispatch<Action>,
  conversationId: string,
  duration: DurationKey,
  previousDuration: DurationKey
): Promise<void> {
  dispatch({ type: "SET_DISAPPEARING", conversationId, duration });
  try {
    await api.setDisappearing(conversationId, DURATION_SECONDS[duration]);
  } catch (err) {
    dispatch({ type: "SET_DISAPPEARING", conversationId, duration: previousDuration });
    throw err;
  }
}

/** Reacts to a message: sends a small encrypted reaction message (see lib/richMessage.ts). The next refresh folds it in. */
export async function sendReaction(conversationId: string, conversationKey: Uint8Array, messageId: string, emoji: ReactionEmoji): Promise<void> {
  await sendEncryptedMessage(conversationId, conversationKey, serializeReaction(messageId, emoji));
}

/** A new version of one of your own messages; the next refresh folds it into the original. */
export async function sendEdit(conversationId: string, conversationKey: Uint8Array, messageId: string, text: string): Promise<void> {
  const trimmed = text.replace(/^\u0000+/, "").trim();
  if (!trimmed) return;
  await sendEncryptedMessage(conversationId, conversationKey, serializeEdit(trimmed, messageId.toLowerCase()));
}

/** A reply or quick reaction to a story: an ordinary encrypted message to its author, marked with which story. */
export async function sendStoryReply(
  dispatch: Dispatch<Action>,
  conversationId: string,
  conversationKey: Uint8Array,
  text: string,
  ref: StoryReplyRef
): Promise<void> {
  const trimmed = text.replace(/^\u0000+/, "").trim();
  if (!trimmed) return;
  await crypto.ready();
  const { dto, ciphertext, nonce } = await sendEncryptedMessage(conversationId, conversationKey, serializeStoryReply(trimmed, ref));
  const security = await buildSecurity(dto.id, ciphertext, nonce, dto.verification ?? null);
  dispatch({
    type: "MESSAGE_SENT",
    conversationId,
    message: {
      id: dto.id,
      from: "me",
      text: trimmed,
      storyReply: ref,
      time: timeLabel(dto.createdAt),
      createdAt: new Date(dto.createdAt).getTime(),
      deleteAt: dto.deleteAt ? new Date(dto.deleteAt).getTime() : null,
      security,
      ciphertextB64: crypto.toBase64(ciphertext),
      nonceB64: crypto.toBase64(nonce),
    },
  });
}

/** Text forwarded into another chat, marked as forwarded. */
export async function sendForwarded(conversationId: string, conversationKey: Uint8Array, text: string): Promise<void> {
  const trimmed = text.replace(/^\u0000+/, "").trim();
  if (!trimmed) return;
  await sendEncryptedMessage(conversationId, conversationKey, serializeForwarded(trimmed));
}

/** A channel post forwarded into a chat, end-to-end encrypted like any message; it keeps which post it was. */
export async function sendChannelForward(conversationId: string, conversationKey: Uint8Array, text: string, ref: ChannelPostRef): Promise<void> {
  const trimmed = text.replace(/^\u0000+/, "").trim();
  if (!trimmed) return;
  await sendEncryptedMessage(conversationId, conversationKey, serializeChannelForward(trimmed, ref));
}

/** Pins a message for everyone in the chat (at most three stay pinned). */
export async function pinMessage(conversationId: string, conversationKey: Uint8Array, messageId: string, preview: string): Promise<void> {
  await sendEncryptedMessage(conversationId, conversationKey, serializePin(messageId.toLowerCase(), preview));
}

export async function unpinMessage(conversationId: string, conversationKey: Uint8Array, messageId: string): Promise<void> {
  await sendEncryptedMessage(conversationId, conversationKey, serializeUnpin(messageId.toLowerCase()));
}

/** Throws (before sending anything) unless there's a question and 2-10 different options. */
export async function createPoll(conversationId: string, conversationKey: Uint8Array, question: string, options: string[], multiple: boolean): Promise<void> {
  const body = serializePoll(question, options, multiple);
  await sendEncryptedMessage(conversationId, conversationKey, body);
}

/** Your vote replaces an earlier one; an empty `options` takes it back. */
export async function votePoll(conversationId: string, conversationKey: Uint8Array, pollId: string, options: number[], labels: string[]): Promise<void> {
  await sendEncryptedMessage(conversationId, conversationKey, serializeVote(pollId.toLowerCase(), options, labels));
}

/** Only the poll's creator can end it; votes after that don't count. */
export async function endPoll(conversationId: string, conversationKey: Uint8Array, pollId: string): Promise<void> {
  await sendEncryptedMessage(conversationId, conversationKey, serializeEndPoll(pollId.toLowerCase()));
}

/** Removes your reaction by deleting the reaction message itself. */
export async function removeReaction(myReactionId: string): Promise<void> {
  await api.deleteMessage(myReactionId);
}

/** "Delete for me": hides the message in this browser only; the other person keeps it. */
export function hideMessageForMe(dispatch: Dispatch<Action>, selfUserId: string, conversationId: string, messageId: string): void {
  hideMessage(selfUserId, messageId);
  dispatch({ type: "MESSAGE_DELETED", conversationId, messageId });
}

/**
 * Deletes your own message for real — the server clears its ciphertext/nonce, it's gone from the database, not just hidden.
 * `related` (see deletedWith) goes too.
 */
export async function deleteDmMessage(
  dispatch: Dispatch<Action>,
  conversationId: string,
  messageId: string,
  related: string[] = []
): Promise<void> {
  await api.deleteMessage(messageId);
  for (const id of related) await api.deleteMessage(id);
  for (const id of [messageId, ...related]) dispatch({ type: "MESSAGE_DELETED", conversationId, messageId: id });
}

/**
 * What else goes when `message` is deleted for everyone: its edits and your own pin notices for it,
 * since both repeat its text and apps that don't fold them would keep showing it.
 */
export function deletedWith(message: Message, all: Message[]): string[] {
  const target = message.id?.toLowerCase();
  const notices = all.filter((m) => m.from === "me" && m.id && m.notice?.targetId === target).map((m) => m.id!);
  return [...(message.editMessageIds ?? []), ...notices];
}

// --- End-to-end encrypted files ---

export type FileSendStage = "encrypting" | "uploading" | "sending";

/** A user-facing failure — `message` is safe to show as-is. */
export class FileActionError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "FileActionError";
  }
}

let fileConfigCache: Promise<api.FileConfigDto> | null = null;
function fileConfig(): Promise<api.FileConfigDto> {
  if (!fileConfigCache) {
    fileConfigCache = api.getFileConfig().catch((err) => {
      fileConfigCache = null;
      throw err;
    });
  }
  return fileConfigCache;
}

function fileErrorMessage(err: unknown, fallback: string): string {
  if (err instanceof FileActionError) return err.message;
  if (err instanceof FileDecryptError) return "Unable to decrypt this file.";
  if (err instanceof TransferError) {
    if (err.kind === "network") return "Network interrupted — try again.";
    if (err.kind === "expired") return "The transfer link expired — try again.";
    if (err.code === "file_deleted") return "File deleted.";
    if (err.code === "file_expired") return "File expired.";
    if (err.kind === "gone") return "This file is no longer available.";
    return fallback;
  }
  if (err instanceof ApiError) {
    if (err.code === "file_deleted") return "File deleted.";
    if (err.code === "file_expired") return "File expired.";
    if (err.code === "upload_expired") return "Upload took too long — try again.";
    if (
      err.code === "upload_quota_exceeded" ||
      err.code === "too_many_pending_uploads" ||
      err.code === "rate_limited" ||
      err.code === "storage_limit_exceeded" ||
      err.code === "pro_required"
    ) {
      return err.message;
    }
    if (err.code === "file_sharing_disabled") return "File sharing isn't available on this server.";
    if (err.status === 400 && /too large/i.test(err.message)) return "File too large.";
    if (err.status === 404) return "File not found.";
    return fallback;
  }
  // fetch() rejects with a TypeError when the network drops.
  if (err instanceof TypeError) return "Network interrupted — try again.";
  return fallback;
}

async function retryTransient<T>(fn: () => Promise<T>, attempts = 4): Promise<T> {
  for (let i = 0; ; i++) {
    try {
      return await fn();
    } catch (err) {
      const transient =
        err instanceof TypeError || (err instanceof ApiError && (err.status >= 500 || err.status === 409 && err.code === "upload_incomplete"));
      if (!transient || i >= attempts - 1) throw err;
      await new Promise((r) => setTimeout(r, 400 * 2 ** i));
    }
  }
}

/**
 * Sends a file end-to-end encrypted:
 *   1. encrypt locally with a fresh random key (lib/fileCrypto.ts),
 *   2. get upload authorization for an opaque object (only size + ciphertext hash go to the API),
 *   3. upload the ciphertext, 4. confirm it,
 *   5. send an ordinary E2EE message whose encrypted body carries the key,
 *      real filename and type — the only place those ever exist outside this device.
 * Resolves only once the message itself is stored, so the UI never shows
 * "Sent" early. Every network step retries transient failures; the final
 * send is idempotent server-side, so a retry can't duplicate the message.
 */
/**
 * The upload half of a file message, for a chat on device keys: the descriptor (with the file's key) then goes in
 * the encrypted envelope, and the server only learns that a file became available in this conversation.
 */
export async function uploadFileForDeviceKeys(conversationId: string, file: File, onStage: (stage: FileSendStage) => void): Promise<FileDescriptor> {
  try {
    const config = await fileConfig();
    if (file.size > config.maxFileSize) {
      throw new FileActionError(`File too large — the limit is ${Math.floor(config.maxFileSize / (1024 * 1024))} MB.`);
    }
    onStage("encrypting");
    await crypto.ready();
    const encrypted = await encryptFile(file);
    onStage("uploading");
    const auth = await retryTransient(() =>
      api.requestFileUpload({ conversationId, encryptedSize: encrypted.encryptedSize, ciphertextSha256: encrypted.ciphertextSha256 })
    );
    await uploadCiphertext(auth.upload, encrypted.blob);
    await retryTransient(() => api.completeFileUpload(auth.objectId));
    await retryTransient(() => api.releaseFile(auth.objectId));
    onStage("sending");
    return {
      type: "file",
      version: 1,
      objectId: auth.objectId,
      fileKey: crypto.toBase64(encrypted.key),
      filename: sanitizeFilename(file.name),
      mimeType: file.type || "application/octet-stream",
      originalSize: encrypted.plaintextSize,
      encryptedSize: encrypted.encryptedSize,
      ciphertextSha256: encrypted.ciphertextSha256,
      encryption: { algorithm: FILE_ALGORITHM, version: 1, header: crypto.toBase64(encrypted.header), chunkSize: encrypted.chunkSize },
    };
  } catch (err) {
    throw new FileActionError(fileErrorMessage(err, "Upload failed — try again."));
  }
}

export async function sendDmFile(
  dispatch: Dispatch<Action>,
  conversationId: string,
  conversationKey: Uint8Array,
  file: File,
  onStage: (stage: FileSendStage) => void
): Promise<void> {
  try {
    const config = await fileConfig();
    if (file.size > config.maxFileSize) {
      throw new FileActionError(`File too large — the limit is ${Math.floor(config.maxFileSize / (1024 * 1024))} MB.`);
    }

    onStage("encrypting");
    await crypto.ready();
    const encrypted = await encryptFile(file);

    onStage("uploading");
    const auth = await retryTransient(() =>
      api.requestFileUpload({
        conversationId,
        encryptedSize: encrypted.encryptedSize,
        ciphertextSha256: encrypted.ciphertextSha256,
      })
    );
    await uploadCiphertext(auth.upload, encrypted.blob);
    await retryTransient(() => api.completeFileUpload(auth.objectId));

    onStage("sending");
    const descriptor: FileDescriptor = {
      type: "file",
      version: 1,
      objectId: auth.objectId,
      fileKey: crypto.toBase64(encrypted.key),
      filename: sanitizeFilename(file.name),
      // Informational only — never a security decision; downloads are always saved as opaque bytes.
      mimeType: file.type || "application/octet-stream",
      originalSize: encrypted.plaintextSize,
      encryptedSize: encrypted.encryptedSize,
      ciphertextSha256: encrypted.ciphertextSha256,
      encryption: { algorithm: FILE_ALGORITHM, version: 1, header: crypto.toBase64(encrypted.header), chunkSize: encrypted.chunkSize },
    };
    const { dto, ciphertext, nonce } = await sendEncryptedMessage(conversationId, conversationKey, serializeFileDescriptor(descriptor), {
      attachments: [auth.objectId],
      retry: retryTransient,
    });
    const ciphertextB64 = crypto.toBase64(ciphertext);
    const nonceB64 = crypto.toBase64(nonce);
    const security = await buildSecurity(dto.id, ciphertext, nonce, dto.verification ?? null);
    dispatch({
      type: "MESSAGE_SENT",
      conversationId,
      message: {
        id: dto.id,
        from: "me",
        text: "",
        file: { descriptor, status: "available" },
        time: timeLabel(dto.createdAt),
        createdAt: new Date(dto.createdAt).getTime(),
        deleteAt: dto.deleteAt ? new Date(dto.deleteAt).getTime() : null,
        security,
        ciphertextB64,
        nonceB64,
      },
    });
  } catch (err) {
    throw new FileActionError(fileErrorMessage(err, "Upload failed — try again."));
  }
}

export type FileDownloadStage = "downloading" | "decrypting";

/** Fetches a received file's ciphertext, verifies + decrypts it locally, and returns the plaintext. Nothing decrypted is ever sent anywhere. */
export async function downloadDmFile(
  descriptor: FileDescriptor,
  onStage: (stage: FileDownloadStage) => void
): Promise<Blob> {
  try {
    onStage("downloading");
    const { download } = await retryTransient(() => api.getFileDownload(descriptor.objectId));
    const chunks = await downloadCiphertext(download);
    onStage("decrypting");
    await crypto.ready();
    return await decryptFile(
      chunks,
      {
        key: crypto.fromBase64(descriptor.fileKey),
        header: crypto.fromBase64(descriptor.encryption.header),
        chunkSize: descriptor.encryption.chunkSize,
        encryptedSize: descriptor.encryptedSize,
        ciphertextSha256: descriptor.ciphertextSha256,
      },
      "application/octet-stream"
    );
  } catch (err) {
    throw new FileActionError(fileErrorMessage(err, "Download failed — try again."));
  }
}

// --- Friends (replaces the old per-conversation message-request gate) ---

/** A read-only preview of a looked-up user, before any friend request exists — drives the "Add friend" screen's lookup-first UX. */
export interface ContactPreview {
  userId: string;
  handle: string;
  publicKey: string; // base64
  avatarUrl: string | null;
  isSelf: boolean;
  deleted: boolean;
}

/** Looks up a handle and returns a read-only preview — doesn't create anything server-side. Dispatches CONTACT_LOOKUP_FAILED (surfaced as state.contactError) on failure, and clears it on success. */
export async function lookupContact(
  dispatch: Dispatch<Action>,
  identity: SelfIdentity,
  handle: string
): Promise<ContactPreview | null> {
  const trimmed = handle.trim();
  if (!trimmed) return null;

  try {
    await crypto.ready();
    const found = await api.lookupUserByUsername(trimmed);
    const user = await api.getUser(found.userId);
    dispatch({ type: "CONTACT_LOOKUP_FAILED", message: null });
    return {
      userId: user.userId,
      handle: user.username,
      publicKey: user.publicKey,
      avatarUrl: user.avatarUrl,
      isSelf: user.userId === identity.userId,
      deleted: user.deleted,
    };
  } catch (err) {
    dispatch({ type: "CONTACT_LOOKUP_FAILED", message: describeError(err, "No one with that handle.") });
    return null;
  }
}

/** Sends a friend request to a previously-looked-up user. Errors surface via state.contactError, same as lookupContact. */
export async function sendFriendRequest(
  dispatch: Dispatch<Action>,
  userId: string,
  handle: string,
  avatarUrl: string | null
): Promise<void> {
  try {
    const dto = await api.sendFriendRequest(handle);
    dispatch({
      type: "FRIEND_REQUEST_SENT",
      request: { id: dto.id, userId, handle, avatarUrl, createdAt: dto.createdAt },
    });
  } catch (err) {
    dispatch({ type: "CONTACT_LOOKUP_FAILED", message: describeError(err, "Couldn't send that friend request.") });
    throw err;
  }
}

/** Loads both directions of pending friend requests. Safe to call repeatedly (polling). */
export async function loadFriendRequests(dispatch: Dispatch<Action>, identity: SelfIdentity): Promise<void> {
  const [incomingDtos, outgoingDtos] = await Promise.all([
    api.listIncomingFriendRequests(),
    api.listOutgoingFriendRequests(),
  ]);

  const hydrate = async (dtos: api.FriendRequestDto[], otherUserId: (dto: api.FriendRequestDto) => string) => {
    const views: FriendRequestView[] = [];
    for (const dto of dtos) {
      const userId = otherUserId(dto);
      try {
        const user = await api.getUser(userId);
        views.push({ id: dto.id, userId, handle: user.username, avatarUrl: user.avatarUrl, createdAt: dto.createdAt });
      } catch {
        // Skip requests whose other side can no longer be resolved.
      }
    }
    return views;
  };

  const [incoming, outgoing] = await Promise.all([
    hydrate(incomingDtos, (dto) => dto.requesterId),
    hydrate(outgoingDtos, (dto) => dto.recipientId),
  ]);
  dispatch({ type: "FRIEND_REQUESTS_LOADED", incoming, outgoing });
}

/**
 * Accepts a friend request: generates a fresh conversation key and seals it
 * for both sides (the same crypto calls the old addContactByHandle used),
 * atomically creating the friendship + conversation server-side, then
 * hydrates it exactly like CONTACT_READY does — landing on Verify so the
 * newly-materialized safety number gets confirmed.
 */
export async function acceptFriendRequest(
  dispatch: Dispatch<Action>,
  identity: SelfIdentity,
  requestId: string,
  requesterId: string,
  defaultDisappearing: DurationKey
): Promise<void> {
  await crypto.ready();
  const requesterUser = await api.getUser(requesterId);
  requireUnchangedKey(identity.userId, requesterId, requesterUser.publicKey);
  const theirPublicKey = crypto.fromBase64(requesterUser.publicKey);
  const conversationKey = crypto.generateConversationKey();

  const created = await api.acceptFriendRequest(requestId, {
    participants: [
      {
        userId: identity.userId,
        wrappedKey: crypto.toBase64(crypto.sealConversationKey(conversationKey, identity.publicKey)),
      },
      {
        userId: requesterId,
        wrappedKey: crypto.toBase64(crypto.sealConversationKey(conversationKey, theirPublicKey)),
      },
    ],
  });

  dispatch({ type: "FRIEND_REQUEST_ACCEPTED", requestId });

  const contact: DMConversation = {
    kind: "dm",
    id: created.id,
    handle: requesterUser.username,
    verified: isVerified(identity.userId, requesterUser.publicKey),
    keyChanged: hasKeyChanged(identity.userId, requesterId, requesterUser.publicKey),
    disappearing: defaultDisappearing,
    time: nowLabel(),
    unread: 0,
    messages: [],
    userId: requesterId,
    publicKey: theirPublicKey,
    conversationKey,
    safetyNumber: crypto.safetyNumberDigits(identity.publicKey, theirPublicKey),
    avatarUrl: requesterUser.avatarUrl,
  };
  dispatch({ type: "CONTACT_READY", contact });
}

/**
 * Opens a 1:1 chat with a creator who charges for messages, without being
 * friends: a message request on their side (the server refuses it when they
 * don't accept message requests, or when either has blocked the other on
 * send). The same key sealing as accepting a friend request. Whether the
 * first message (or each one) needs paying is the server's call: the paid
 * message bar in the thread shows it.
 */
export async function startMessageRequest(
  dispatch: Dispatch<Action>,
  identity: SelfIdentity,
  userId: string,
  defaultDisappearing: DurationKey
): Promise<string> {
  await crypto.ready();
  const other = await api.getUser(userId);
  requireUnchangedKey(identity.userId, userId, other.publicKey);
  const theirPublicKey = crypto.fromBase64(other.publicKey);
  const conversationKey = crypto.generateConversationKey();
  const created = await api.createConversation({
    isGroup: false,
    participants: [
      { userId: identity.userId, wrappedKey: crypto.toBase64(crypto.sealConversationKey(conversationKey, identity.publicKey)) },
      { userId, wrappedKey: crypto.toBase64(crypto.sealConversationKey(conversationKey, theirPublicKey)) },
    ],
  });
  const contact: DMConversation = {
    kind: "dm",
    id: created.id,
    handle: other.username,
    verified: isVerified(identity.userId, other.publicKey),
    keyChanged: hasKeyChanged(identity.userId, userId, other.publicKey),
    disappearing: defaultDisappearing,
    time: nowLabel(),
    unread: 0,
    messages: [],
    userId,
    publicKey: theirPublicKey,
    conversationKey,
    safetyNumber: crypto.safetyNumberDigits(identity.publicKey, theirPublicKey),
    avatarUrl: other.avatarUrl,
  };
  dispatch({ type: "CONTACT_READY", contact });
  dispatch({ type: "OPEN_THREAD", conversationId: created.id });
  return created.id;
}

/** Denies an incoming friend request — the sender isn't told either way. */
export async function denyFriendRequest(dispatch: Dispatch<Action>, requestId: string): Promise<void> {
  await api.denyFriendRequest(requestId);
  dispatch({ type: "FRIEND_REQUEST_DENIED", requestId });
}

/** Cancels a friend request you sent, before the other side has responded. */
export async function cancelFriendRequest(dispatch: Dispatch<Action>, requestId: string): Promise<void> {
  await api.cancelFriendRequest(requestId);
  dispatch({ type: "FRIEND_REQUEST_CANCELED", requestId });
}

/** "Accept messages from anyone" — real, server-persisted, unlike the rest of AppSettings. Updates optimistically and reverts if the server call fails. Still gates whether a stranger can send *you* a friend request in the first place. */
export async function updateAcceptMessageRequests(dispatch: Dispatch<Action>, value: boolean): Promise<void> {
  dispatch({ type: "UPDATE_SETTINGS", settings: { acceptMessageRequests: value } });
  try {
    await api.updateSettings({ acceptMessageRequests: value });
  } catch (err) {
    dispatch({ type: "UPDATE_SETTINGS", settings: { acceptMessageRequests: !value } });
    throw err;
  }
}

/** "Transparency Mode" — real, server-persisted, off by default. While on, this user's own outgoing messages' ciphertext appears in the public live transparency feed (see lib/api.ts's getTransparencyFeed). Updates optimistically and reverts if the server call fails. */
export async function updateTransparencyMode(dispatch: Dispatch<Action>, value: boolean): Promise<void> {
  dispatch({ type: "UPDATE_SETTINGS", settings: { transparencyMode: value } });
  try {
    await api.updateSettings({ transparencyMode: value });
  } catch (err) {
    dispatch({ type: "UPDATE_SETTINGS", settings: { transparencyMode: !value } });
    throw err;
  }
}

/** "Show when I'm online": server-persisted, off by default, reciprocal (see state/LiveContext.tsx). */
export async function updateShowOnlineStatus(dispatch: Dispatch<Action>, value: boolean): Promise<void> {
  dispatch({ type: "UPDATE_SETTINGS", settings: { showOnlineStatus: value } });
  try {
    await api.updateSettings({ showOnlineStatus: value });
  } catch (err) {
    dispatch({ type: "UPDATE_SETTINGS", settings: { showOnlineStatus: !value } });
    throw err;
  }
}

/** Uploads a real profile picture — stored in B2, visible to every user, not just you. `imageBase64` should already be a small resized JPEG (see lib/avatar.ts). */
export async function uploadAvatar(dispatch: Dispatch<Action>, imageBase64: string): Promise<void> {
  const result = await api.uploadAvatar(imageBase64);
  dispatch({ type: "AVATAR_UPDATED", avatarUrl: result.avatarUrl });
}

export async function removeAvatar(dispatch: Dispatch<Action>): Promise<void> {
  await api.removeAvatar();
  dispatch({ type: "AVATAR_UPDATED", avatarUrl: null });
}

// --- Account self-service: change password / change username ---

/**
 * Re-derives and re-wraps the private key with the new password client-side
 * and sends both the password change and the re-wrapped key together in one
 * request — changing the password server-side alone would strand the
 * previously-wrapped key. Revokes other sessions server-side; this one
 * stays signed in.
 */
export async function changePassword(
  dispatch: Dispatch<Action>,
  identity: SelfIdentity,
  currentPassword: string,
  newPassword: string
): Promise<void> {
  await crypto.ready();
  const wrapped = crypto.wrapPrivateKey(identity.privateKey, newPassword);
  const next = await newCredential(identity.handle, newPassword, wrapped.kdfSalt);
  await api.changePassword({
    currentPassword: await passwordProof(identity.handle, currentPassword),
    newPassword: next.password,
    authVersion: next.authVersion,
    kdfOps: wrapped.kdf?.ops,
    kdfMem: wrapped.kdf?.mem,
    encryptedPrivateKey: crypto.toBase64(wrapped.encryptedPrivateKey),
    privateKeyNonce: crypto.toBase64(wrapped.nonce),
    privateKeyKdfSalt: crypto.toBase64(wrapped.kdfSalt),
  });
  rememberWrappedKey(wrapped);
  // Nothing in `identity` changes locally — the private key bytes are the
  // same, only how it's wrapped at rest server-side. `dispatch` is kept in
  // the signature for consistency with every other screens-call-actions.ts
  // function, and in case a future revision needs to reflect the change.
}

export async function changeUsername(dispatch: Dispatch<Action>, newUsername: string): Promise<void> {
  const trimmed = newUsername.trim();
  await api.changeUsername(trimmed);
  const previous = loadLastHandle();
  if (previous) movePin(previous, trimmed);
  saveLastHandle(trimmed);
  dispatch({ type: "USERNAME_CHANGED", handle: trimmed });
}

// --- Groups ---

const DEFAULT_GROUP_NAME = "Group";

/** Hydrates a server-backed group: unseals its key, decrypts its name, resolves members, decrypts messages. */
async function hydrateGroup(dto: api.ConversationDto, identity: SelfIdentity): Promise<GroupConversation> {
  const ring = keyRingFromDto(dto, identity);
  await verifyGroupRing(ring, dto, identity.userId);
  rememberKeyRing(ring);
  const conversationKey = ring.keys.get(ring.currentEpoch) ?? NO_KEY;
  // Someone left or was removed: give the group a new key now rather than at
  // the next send (any member may; the server keeps the first).
  if (ring.rekeyRequired) rotateGroupKey(dto.id).catch((err) => console.warn("Group key rotation failed", err instanceof Error ? err.message : err));
  let label = DEFAULT_GROUP_NAME;
  if (dto.nameCiphertext && dto.nameNonce) {
    try {
      label = crypto.decryptWithConversationKey(crypto.fromBase64(dto.nameCiphertext), crypto.fromBase64(dto.nameNonce), conversationKey);
    } catch {
      label = DEFAULT_GROUP_NAME;
    }
  }
  const participants = await api.listParticipants(dto.id);
  const members: GroupMember[] = [];
  for (const p of participants) {
    try {
      const user = await api.getUser(p.userId);
      members.push({
        userId: p.userId,
        handle: user.username,
        avatarUrl: user.avatarUrl,
        role: p.role,
        publicKey: user.publicKey,
        isSelf: p.userId === identity.userId,
      });
    } catch {
      // Skip a member whose profile can't be loaded right now.
    }
  }
  const others = members.filter((m) => !m.isSelf).map((m) => m.userId);
  // A group from before key notices gets its first one here. Only at epoch 0: a later key with no notice is
  // something this device can't vouch for, and members with pins would believe it. The next rekey gives it a notice.
  if (!dto.keyNotice && !ring.keyProblem && ring.currentEpoch === 0) {
    await publishGroupNotice(dto.id, identity.userId, ring.currentEpoch, conversationKey, participants.map((p) => p.userId)).catch((err) =>
      console.warn("Group key notice failed", err instanceof Error ? err.message : err)
    );
  }
  const { messages, unread } = await fetchAndDecryptMessages(dto.id, conversationKey, identity.userId, others);
  return {
    kind: "group",
    id: dto.id,
    keyProblem: ring.keyProblem,
    label,
    members,
    myRole: dto.role,
    disappearing: durationKeyFromSeconds(dto.disappearingSeconds),
    time: messages.at(-1)?.time ?? nowLabel(),
    unread,
    messages,
    conversationKey,
  };
}

/** Profiles of people seen in community messages, looked up once (a community can have up to a thousand members). */
const communityProfiles = new Map<string, Promise<api.UserResult | null>>();

function communityProfile(userId: string): Promise<api.UserResult | null> {
  let profile = communityProfiles.get(userId);
  if (!profile) {
    profile = api.getUser(userId).catch(() => null);
    communityProfiles.set(userId, profile);
  }
  return profile;
}

/**
 * A community conversation: its keys are the member's epoch keys (not one
 * group key), its name is the community's public name, and its member list
 * is only who appears in the loaded messages: the full list is on the
 * community page, for staff.
 */
async function hydrateCommunity(dto: api.ConversationDto, identity: SelfIdentity): Promise<GroupConversation> {
  const info = dto.community!;
  const ring = await ensureCommunityRing(info.id, dto.id, info.keyEpoch, info.rekeyRequired);
  const currentKey = ring.keys.get(ring.currentEpoch) ?? [...ring.keys.values()].at(-1);
  const { messages, unread } = await fetchAndDecryptMessages(dto.id, currentKey ?? new Uint8Array(32), identity.userId, []);
  const senderIds = [...new Set(messages.map((m) => m.from).filter((from): from is string => from !== "me" && from !== "system"))];
  const members: GroupMember[] = [
    { userId: identity.userId, handle: identity.handle, avatarUrl: identity.avatarUrl, role: dto.role, publicKey: crypto.toBase64(identity.publicKey), isSelf: true },
  ];
  for (const userId of senderIds) {
    const user = await communityProfile(userId);
    if (user) members.push({ userId, handle: user.username, avatarUrl: user.avatarUrl, role: "member", publicKey: user.publicKey, isSelf: false });
  }
  return {
    kind: "group",
    id: dto.id,
    label: info.name,
    members,
    myRole: dto.role,
    disappearing: durationKeyFromSeconds(dto.disappearingSeconds),
    time: messages.at(-1)?.time ?? nowLabel(),
    unread,
    messages,
    conversationKey: currentKey,
    community: { id: info.id, kind: info.kind, role: info.role, canPost: info.canPost, status: info.status },
  };
}

function encryptName(name: string, conversationKey: Uint8Array): { nameCiphertext: string; nameNonce: string } {
  const { ciphertext, nonce } = crypto.encryptWithConversationKey(name, conversationKey);
  return { nameCiphertext: crypto.toBase64(ciphertext), nameNonce: crypto.toBase64(nonce) };
}

async function sealFor(identity: SelfIdentity, userId: string, conversationKey: Uint8Array): Promise<string> {
  const user = await api.getUser(userId);
  requireUnchangedKey(identity.userId, userId, user.publicKey);
  return crypto.toBase64(crypto.sealConversationKey(conversationKey, crypto.fromBase64(user.publicKey)));
}

/**
 * Creates a group with some of your friends: a fresh group key sealed once
 * per member (you included), and the name encrypted with that key so the
 * server never learns it. Opens the new group.
 */
export async function createGroup(dispatch: Dispatch<Action>, identity: SelfIdentity, name: string, memberUserIds: string[]): Promise<void> {
  await crypto.ready();
  const conversationKey = crypto.generateConversationKey();
  const participants = [
    { userId: identity.userId, wrappedKey: crypto.toBase64(crypto.sealConversationKey(conversationKey, identity.publicKey)) },
  ];
  for (const userId of memberUserIds) participants.push({ userId, wrappedKey: await sealFor(identity, userId, conversationKey) });
  const created = await api.createConversation({ isGroup: true, ...encryptName(name.trim(), conversationKey), participants });
  const dto: api.ConversationDto = {
    id: created.id,
    isGroup: true,
    role: "owner",
    ...encryptName(name.trim(), conversationKey),
    wrappedKey: participants[0].wrappedKey,
    disappearingSeconds: null,
    createdBy: identity.userId,
    createdAt: created.createdAt,
  };
  dispatch({ type: "GROUP_CREATED", group: await hydrateGroup(dto, identity) });
}

/** Owner/admin: add a friend, sealing them their own copy of the group key. They only see messages from now on. */
export async function addGroupMember(dispatch: Dispatch<Action>, identity: SelfIdentity, group: GroupConversation, userId: string): Promise<void> {
  if (!group.conversationKey) return;
  await crypto.ready();
  const { key, epoch } = currentKey(group.id, group.conversationKey);
  const keyNotice = await noticeAdding(group.id, epoch, key, userId);
  await api.addParticipant(group.id, userId, await sealFor(identity, userId, key), epoch, keyNotice);
  if (keyNotice) await adoptGroupNotice(group.id, identity.userId, epoch, key, keyNotice);
  await loadConversations(dispatch, identity);
}

/** Owner/admin: remove a member. They lose access to the group's messages. */
export async function removeGroupMember(dispatch: Dispatch<Action>, identity: SelfIdentity, group: GroupConversation, userId: string): Promise<void> {
  await api.removeParticipant(group.id, userId);
  // They may still hold the group key: replace it before anything else is sent.
  await rotateGroupKey(group.id).catch((err) => console.warn("Group key rotation failed", err instanceof Error ? err.message : err));
  await loadConversations(dispatch, identity);
}

/** Owner/admin: let in someone who asked through the invite link, sealing them their own copy of the group key. */
export async function approveJoinRequest(dispatch: Dispatch<Action>, identity: SelfIdentity, group: GroupConversation, userId: string): Promise<void> {
  if (!group.conversationKey) return;
  await crypto.ready();
  const { key, epoch } = currentKey(group.id, group.conversationKey);
  const keyNotice = await noticeAdding(group.id, epoch, key, userId);
  await api.approveGroupJoinRequest(group.id, userId, await sealFor(identity, userId, key), epoch, keyNotice);
  if (keyNotice) await adoptGroupNotice(group.id, identity.userId, epoch, key, keyNotice);
  await loadConversations(dispatch, identity);
}

/**
 * Joins a group from its invite link with a copy of the group key sealed to
 * ourselves (when the link carries the key), or asks to join (when an admin
 * approves). Opens the group once in.
 */
export async function joinGroupFromInvite(
  dispatch: Dispatch<Action>,
  identity: SelfIdentity,
  linkId: string,
  secret: Uint8Array,
  groupKey: Uint8Array | null
): Promise<"joined" | "member" | "requested"> {
  await crypto.ready();
  const wrappedKey = groupKey ? crypto.toBase64(crypto.sealConversationKey(groupKey, identity.publicKey)) : null;
  const result = await api.joinGroupInvite(linkId, inviteKeys(secret).auth, wrappedKey);
  if (result.status !== "requested") {
    // Joining through a link adds this person with no other member present, so they confirm themselves.
    // The key commitment in the notice shows they really hold the group's key.
    if (groupKey && result.status === "joined") {
      await confirmSelfInGroup(identity.userId, result.conversationId, groupKey).catch((err) =>
        console.warn("Group key notice failed", err instanceof Error ? err.message : err)
      );
    }
    await loadConversations(dispatch, identity);
    dispatch({ type: "OPEN_THREAD", conversationId: result.conversationId });
  }
  return result.status;
}

async function confirmSelfInGroup(selfUserId: string, conversationId: string, groupKey: Uint8Array): Promise<void> {
  const dto = (await api.listConversations()).find((c) => c.id === conversationId);
  if (!dto) return;
  const latest = dto.keyNotice ? await checkedLatestNotice(dto.id, selfUserId, groupKey, dto.keyNotice) : null;
  // Not one a member signed, so nothing here can be vouched for. The next rekey says this person is unconfirmed.
  if (dto.keyNotice && !latest) return;
  const participants = (await api.listParticipants(conversationId)).map((p) => p.userId);
  const members = membersToSign(selfUserId, conversationId, participants, [selfUserId], latest);
  await publishGroupNotice(dto.id, selfUserId, dto.keyEpoch ?? 0, groupKey, members, latest);
}

export async function leaveGroup(dispatch: Dispatch<Action>, identity: SelfIdentity, group: GroupConversation): Promise<void> {
  await api.removeParticipant(group.id, identity.userId);
  dispatch({ type: "LEAVE_GROUP", conversationId: group.id });
}

export async function renameGroup(dispatch: Dispatch<Action>, identity: SelfIdentity, group: GroupConversation, name: string): Promise<void> {
  if (!group.conversationKey) return;
  await crypto.ready();
  const { nameCiphertext, nameNonce } = encryptName(name.trim(), currentKey(group.id, group.conversationKey).key);
  await api.renameConversation(group.id, nameCiphertext, nameNonce);
  await loadConversations(dispatch, identity);
}

/** Owner only. */
export async function setGroupRole(
  dispatch: Dispatch<Action>,
  identity: SelfIdentity,
  group: GroupConversation,
  userId: string,
  role: "admin" | "member"
): Promise<void> {
  await api.setParticipantRole(group.id, userId, role);
  await loadConversations(dispatch, identity);
}

// --- Recovery key ---

/** Creates a new recovery key (replacing any old one) and returns it formatted, to show once. Needs the password. */
export async function createRecoveryKey(identity: SelfIdentity, currentPassword: string): Promise<string> {
  await crypto.ready();
  const key = generateRecoveryKey();
  await api.setRecoveryKey(await passwordProof(identity.handle, currentPassword), await recoveryMaterialFor(key, identity.privateKey));
  return formatRecoveryKey(key);
}

export type RecoverOutcome = "signed-in" | "code-required";

/**
 * Recovers an account with its recovery key: unwraps the private key on
 * this device (checking it matches the account's public key), re-wraps it
 * under the new password, and signs in. Other sessions are signed out.
 * Returns "code-required" (nothing changed yet) when two-factor is on and
 * no code was given.
 */
export async function recoverAccount(
  dispatch: Dispatch<Action>,
  handle: string,
  recoveryKeyText: string,
  newPassword: string,
  code?: string
): Promise<RecoverOutcome> {
  const key = parseRecoveryKey(recoveryKeyText);
  if (!key) throw new Error("That doesn't look like a recovery key. Check it and try again.");
  // The server only sees the sign-in key now, so it can't check this for us.
  if (newPassword.length < 8) throw new Error("Choose a password of at least 8 characters.");
  await crypto.ready();
  const recoveryAuth = recoveryAuthFor(key);
  let started: api.RecoveryStartResult;
  try {
    started = await api.recoveryStart(handle, recoveryAuth);
  } catch (err) {
    if (err instanceof ApiError && err.code === "account_locked") throw new Error("Too many tries. Wait a while and try again.");
    throw new Error("That handle and recovery key don't match.");
  }
  if (started.mfaRequired && !code) return "code-required";
  const privateKey = unwrapWithRecoveryKey(key, started.recoveryEncryptedPrivateKey, started.recoveryPrivateKeyNonce, started.publicKey);
  const wrapped = crypto.wrapPrivateKey(privateKey, newPassword);
  const next = await newCredential(handle, newPassword, wrapped.kdfSalt);
  let result: api.LoginResult;
  try {
    result = await api.recoveryComplete({
      username: handle,
      recoveryAuth,
      code,
      newPassword: next.password,
      authVersion: next.authVersion,
      kdfOps: wrapped.kdf?.ops,
      kdfMem: wrapped.kdf?.mem,
      encryptedPrivateKey: crypto.toBase64(wrapped.encryptedPrivateKey),
      privateKeyNonce: crypto.toBase64(wrapped.nonce),
      privateKeyKdfSalt: crypto.toBase64(wrapped.kdfSalt),
    });
  } catch (err) {
    if (err instanceof ApiError && err.code === "invalid_code") throw new Error("Wrong code. Check your authenticator app and try again.");
    throw new Error(describeError(err, "Couldn't recover the account. Try again."));
  }
  if (next.authVersion === 2) pinSignInKeys(handle);
  saveLastHandle(handle);
  rememberWrappedKey(wrapped);
  dispatch({
    type: "AUTH_SUCCEEDED",
    identity: {
      handle,
      userId: result.userId,
      publicKey: crypto.fromBase64(result.publicKey),
      privateKey,
      avatarUrl: result.avatarUrl,
      role: result.role,
      subscriptionTier: result.subscriptionTier,
      mfaEnabled: started.mfaRequired,
    },
    acceptMessageRequests: result.acceptMessageRequests,
    transparencyMode: result.transparencyMode,
    showOnlineStatus: result.showOnlineStatus ?? false,
  });
  return "signed-in";
}

// --- MFA ---

export async function startMfaSetup(): Promise<api.MfaSetupResult> {
  return api.setupMfa();
}

export async function confirmMfaSetup(dispatch: Dispatch<Action>, code: string): Promise<void> {
  await api.confirmMfa(code);
  dispatch({ type: "MFA_ENABLED" });
}

export async function disableMfa(dispatch: Dispatch<Action>, handle: string, password: string): Promise<void> {
  await crypto.ready();
  await api.disableMfa(await passwordProof(handle, password));
  dispatch({ type: "MFA_DISABLED" });
}

// --- Report a user ---

export async function submitReport(reportedUserId: string, reason: string): Promise<void> {
  await api.submitReport(reportedUserId, reason.trim());
}

// --- Admin panel ---

export async function adminCreateAccount(
  handle: string,
  password: string,
  /** Ticked by the admin for the person the account is for; the server refuses it otherwise. */
  consent: SignupConsent
): Promise<{ userId: string }> {
  // Same client-side keypair-generation/wrapping registerIdentity uses —
  // but this never touches state.identity: it's not the admin's own
  // session, just an account being created on someone else's behalf.
  await crypto.ready();
  const keyPair = crypto.generateKeyPair();
  const wrapped = crypto.wrapPrivateKey(keyPair.privateKey, password);
  return api.adminCreateUser({
    username: handle,
    password,
    publicKey: crypto.toBase64(keyPair.publicKey),
    encryptedPrivateKey: crypto.toBase64(wrapped.encryptedPrivateKey),
    privateKeyNonce: crypto.toBase64(wrapped.nonce),
    privateKeyKdfSalt: crypto.toBase64(wrapped.kdfSalt),
    terms_accepted: consent.termsAccepted,
    privacy_acknowledged: consent.privacyAcknowledged,
    age_18_confirmed: consent.age18Confirmed,
  });
}

export async function adminDeleteAccount(userId: string): Promise<void> {
  await api.adminDeleteUser(userId);
}

export async function adminSetSubscriptionTier(userId: string, tier: SubscriptionTier): Promise<void> {
  await api.adminSetTier(userId, tier);
}

export async function adminResolveReport(reportId: string, status: "reviewed" | "dismissed"): Promise<void> {
  await api.adminResolveReport(reportId, status);
}
