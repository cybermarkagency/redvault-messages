// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useMemo, useRef, useState } from "react";
import { useV3Chat } from "../state/V3Chat.js";
import { PaidMessageBar, usePaidMessaging } from "../components/earnings/PaidMessageBar.js";
import { Avatar } from "../components/Avatar.js";
import { ChatDetails } from "../components/ChatDetails.js";
import { CommunityReportDialog } from "./CommunityReportDialog.js";
import { DurationSelect } from "../components/DurationSelect.js";
import { GroupGlyph } from "../components/GroupGlyph.js";
import {
  ArrowDownLeftIcon,
  ArrowUpRightIcon,
  ChevronLeftIcon,
  LockIcon,
  PanelRightIcon,
  PaperclipIcon,
  PhoneIcon,
  SearchIcon,
  SendIcon,
  ShieldCheckIcon,
  TimerIcon,
  VideoIcon,
} from "../components/icons24.js";
import { MessageBubble } from "../components/MessageBubble.js";
import { dayLabel } from "../lib/dayLabel.js";
import { useIsDesktopLayout } from "../hooks/useMediaQuery.js";
import { ApiError } from "../lib/api.js";
import { type CallRecord, formatCallDuration, loadCallHistory, onCallHistoryChange } from "../lib/callHistory.js";
import { formatBytes } from "../lib/fileMessage.js";
import { useLocalPrefs } from "../lib/localPrefs.js";
import type { ReactionEmoji } from "../lib/richMessage.js";
import { type FormatOptions, toPlain } from "../lib/textFormat.js";
import {
  createPoll,
  deleteDmMessage,
  deletedWith,
  downloadDmFile,
  endPoll,
  pinMessage,
  pinnedFrom,
  sendEdit,
  uploadFileForDeviceKeys,
  sendChannelForward,
  sendForwarded,
  unpinMessage,
  votePoll,
  FileActionError,
  type FileSendStage,
  hideMessageForMe,
  loadMessageVerification,
  refreshDmMessages,
  removeReaction,
  sendDmFile,
  sendDmMessage,
  sendReaction,
  updateDisappearing,
  retryQueued,
  discardQueued,
} from "../state/actions.js";
import { onOnionOrigin, TOR_UNAVAILABLE_MESSAGE } from "../lib/torRouting.js";
import { useAppState } from "../state/AppStateContext.js";
import { useCall } from "../state/CallContext.js";
import { useGroupCall } from "../state/GroupCallContext.js";
import { useLive } from "../state/LiveContext.js";
import { type Conversation, DURATION_LABELS, type DurationKey, type Message, tierAtLeast, verifiedCount } from "../types.js";
import styles from "./Thread.module.css";

const DM_POLL_MS = 3000;

const FILE_STAGE_LABEL: Record<FileSendStage | "sent", string> = {
  encrypting: "Encrypting…",
  uploading: "Uploading…",
  sending: "Sending…",
  sent: "Sent",
};
/** 1 week — matches the server's RETENTION_WARNING_MS (server/src/constants.ts); duplicated here since the client can't import server code, but both derive from the same 6-month retention policy. */
const RETENTION_WARNING_MS = 7 * 24 * 60 * 60 * 1000;
/** Same wording as the server's pro_required error (FileService.requestUpload), which stays the real gate. */
const PRO_REQUIRED_MESSAGE = "Sending files is a Pro feature — upgrade to Pro to send files.";

/** Signal's window for editing a sent message. */
const EDIT_WINDOW_MS = 24 * 60 * 60 * 1000;

const canEdit = (m: Message) =>
  m.from === "me" && Boolean(m.id) && !m.deleted && !m.file && !m.poll && !m.notice && Date.now() - (m.createdAt ?? 0) < EDIT_WINDOW_MS;

/** Everyone in the conversation except you: the one contact of a DM, or every other group member. */
function othersIn(conversation: Conversation, selfUserId: string | undefined): string[] {
  if (conversation.kind === "dm") return conversation.userId ? [conversation.userId] : [];
  return conversation.members.filter((m) => !m.isSelf && m.userId !== selfUserId).map((m) => m.userId);
}

type TimelineItem = { kind: "message"; message: Message; index: number; at: number } | { kind: "call"; call: CallRecord; at: number };

function CallEntry({ call }: { call: CallRecord }) {
  const missed = call.outcome !== "answered";
  const what = `${call.kind === "video" ? "Video" : "Voice"} call`;
  const label =
    call.outcome === "answered"
      ? `${what} · ${formatCallDuration(call.durationSec)}`
      : call.direction === "incoming"
        ? `Missed ${what.toLowerCase()}`
        : `${what} · ${call.outcome === "declined" ? "declined" : "no answer"}`;
  return (
    <div className={styles.callEntry}>
      <span className={`${styles.callPill} ${missed && call.direction === "incoming" ? styles.callPillMissed : ""}`}>
        {call.kind === "video" ? <VideoIcon size={15} /> : <PhoneIcon size={15} />}
        {call.direction === "outgoing" ? <ArrowUpRightIcon size={13} /> : <ArrowDownLeftIcon size={13} />}
        {label}
        <span className={styles.callTime}>{new Date(call.startedAt).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" })}</span>
      </span>
    </div>
  );
}

export function Thread({ conversationId }: { conversationId: string }) {
  const { state, dispatch, contactsById } = useAppState();
  const isDesktop = useIsDesktopLayout();
  const live = useLive();
  const calls = useCall();
  const groupCalls = useGroupCall();
  const [prefs, setPrefs] = useLocalPrefs(state.identity?.userId);
  const [draft, setDraft] = useState("");
  const [replyingTo, setReplyingTo] = useState<Message | null>(null);
  // Editing one of your messages: the draft holds its new text.
  const [editing, setEditing] = useState<Message | null>(null);
  const [forwarding, setForwarding] = useState<Message | null>(null);
  /** Communities: the message being reported (the dialog asks whether to share its text). */
  const [reporting, setReporting] = useState<Message | null>(null);
  const [forwardError, setForwardError] = useState<string | null>(null);
  const [pollOpen, setPollOpen] = useState(false);
  const [pollQuestion, setPollQuestion] = useState("");
  const [pollOptions, setPollOptions] = useState<string[]>(["", ""]);
  const [pollMultiple, setPollMultiple] = useState(false);
  const [pollError, setPollError] = useState<string | null>(null);
  // Which pinned message the bar shows (it cycles through them on click), and the one just jumped to.
  const [pinIndex, setPinIndex] = useState(0);
  const [flashId, setFlashId] = useState<string | null>(null);
  const [mentionIndex, setMentionIndex] = useState(0);
  const draftRef = useRef<HTMLTextAreaElement>(null);
  const [sending, setSending] = useState(false);
  // Attachment flow: pick -> preview (name/size, local only) -> Encrypting -> Uploading -> Sending -> Sent.
  const fileInputRef = useRef<HTMLInputElement>(null);
  const [pendingFile, setPendingFile] = useState<File | null>(null);
  const [fileStage, setFileStage] = useState<FileSendStage | "sent" | null>(null);
  const [fileError, setFileError] = useState<string | null>(null);
  const [searchOpen, setSearchOpen] = useState(false);
  const [search, setSearch] = useState("");
  const [callLog, setCallLog] = useState<CallRecord[]>([]);
  const messagesEnd = useRef<HTMLDivElement>(null);

  const conversation = state.conversations.find((c) => c.id === conversationId);
  // Per-device keys (protocol 3, preview): off unless the server and this browser have them (state/V3Chat.tsx).
  const v3 = useV3Chat(conversation);

  // Poll the open chat for new messages — "receiving" happens by re-fetching
  // and re-decrypting. Each tick also tells the server we've now seen
  // whatever's unread (see refreshDmMessages), which keeps the unread badge
  // honest across reloads and drives the other side's "Read" when read
  // receipts are on.
  const otherUserIds = conversation ? othersIn(conversation, state.identity?.userId) : [];
  useEffect(() => {
    if (!conversation?.conversationKey || otherUserIds.length === 0 || !state.identity) return;
    const { conversationKey } = conversation;
    const selfUserId = state.identity.userId;
    const readReceipts = state.settings.readReceipts;
    const tick = () =>
      refreshDmMessages(dispatch, conversationId, conversationKey, selfUserId, otherUserIds, readReceipts).catch(console.error);
    tick();
    const interval = setInterval(tick, DM_POLL_MS);
    return () => clearInterval(interval);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [conversationId, conversation?.kind, state.identity?.userId, state.settings.readReceipts, otherUserIds.join(",")]);

  // Calls with this person, from this device's own (encrypted) history.
  const identity = state.identity;
  useEffect(() => {
    if (!identity) return;
    const load = () => setCallLog(loadCallHistory(identity.userId, identity.privateKey).filter((c) => c.conversationId === conversationId));
    load();
    return onCallHistoryChange(load);
  }, [identity, conversationId]);

  const messageCount = conversation?.messages.length ?? 0;
  const typingUsers = live.typingIn(conversationId);
  useEffect(() => {
    messagesEnd.current?.scrollIntoView?.({ block: "end" });
  }, [messageCount, typingUsers.length, callLog.length]);

  const timeline = useMemo(() => {
    const q = search.trim().toLowerCase();
    const items: TimelineItem[] = (conversation?.messages ?? [])
      .map((message, index) => ({ kind: "message" as const, message, index, at: message.createdAt ?? 0 }))
      .filter((item) => !q || item.message.text.toLowerCase().includes(q) || item.message.file?.descriptor.filename.toLowerCase().includes(q));
    items.push(
      ...v3.messages
        .map((message, i) => ({ kind: "message" as const, message, index: -(i + 1), at: message.createdAt ?? 0 }))
        .filter((item) => !q || item.message.text.toLowerCase().includes(q))
    );
    if (!q) items.push(...callLog.map((call) => ({ kind: "call" as const, call, at: call.startedAt })));
    return items.sort((a, b) => a.at - b.at);
  }, [conversation?.messages, v3.messages, callLog, search]);

  // Paid messages: what sending to this person costs (null: free).
  const paidMessaging = usePaidMessaging(conversation?.kind === "dm" ? conversation.userId : null, conversationId);

  if (!conversation) return null;

  const isGroup = conversation.kind === "group";
  const canSendFiles = state.identity ? tierAtLeast(state.identity.subscriptionTier, "pro") : false;
  const name = isGroup ? conversation.label : conversation.handle;
  const keyChanged = conversation.kind === "dm" && !!conversation.keyChanged;
  const online = conversation.kind === "dm" && !!conversation.userId && live.online.has(conversation.userId);
  // 1:1 only; hidden when this server has no call relay.
  const showCallButtons = conversation.kind === "dm" && calls.callsAvailable && !!conversation.userId;
  const inGroupCall = groupCalls.call !== null && groupCalls.call.phase !== "ended";
  const callBusy = (calls.call !== null && calls.call.phase !== "ended") || inGroupCall;
  // Groups: a call through the group-call server, hosted on Pro (voice) or Premium (video).
  const community = conversation.kind === "group" ? conversation.community : undefined;
  // A channel's subscribers read; only members who can post write or start calls.
  const readOnly = !!community && !community.canPost;
  const showGroupCallButtons = conversation.kind === "group" && groupCalls.available && !readOnly;
  const ongoingGroupCall = conversation.kind === "group" ? groupCalls.active.get(conversation.id) : undefined;
  const inThisGroupCall = inGroupCall && groupCalls.call?.conversationId === conversationId;
  const detailsOpen = isDesktop && prefs.chatDetailsOpen;

  function placeCall(kind: "voice" | "video") {
    if (!conversation || !state.identity) return;
    if (conversation.kind === "group") groupCalls.start(conversation, kind);
    else calls.startCall(conversation, kind);
  }

  // Opening a group checks for a call going on there (for the Join banner).
  const refreshGroupCalls = groupCalls.refreshActive;
  const openedGroup = conversation?.kind === "group" ? conversation.id : null;
  useEffect(() => {
    if (openedGroup) refreshGroupCalls();
  }, [openedGroup, refreshGroupCalls]);

  // Only a Premium member can start a group call; anyone can join one that's going on.
  const canStartGroupCall = state.identity ? tierAtLeast(state.identity.subscriptionTier, "premium") : false;
  const groupCallBlocked = isGroup && !canStartGroupCall && !ongoingGroupCall;

  function groupCallTitle(kind: "voice" | "video"): string {
    if (ongoingGroupCall) return `Join the ${ongoingGroupCall.kind} call`;
    return canStartGroupCall ? `Group ${kind} call` : `Only Premium members can start group ${kind} calls`;
  }

  // Symmetric retention warning: derived purely from the earliest upcoming
  // deleteAt among this thread's currently-live messages, re-evaluated on
  // every poll — no dismiss-and-remember state, shown to both participants.
  const earliestDeleteAt = conversation.messages.reduce<number | null>((min, m) => {
    if (m.deleteAt == null) return min;
    return min == null || m.deleteAt < min ? m.deleteAt : min;
  }, null);
  const handleOf = (userId: string) =>
    conversation.kind === "group" ? (conversation.members.find((m) => m.userId === userId)?.handle ?? "Former member") : conversation.handle;
  const showRetentionBanner = earliestDeleteAt != null && earliestDeleteAt - Date.now() <= RETENTION_WARNING_MS;

  const refreshNow = () =>
    conversation?.conversationKey && state.identity
      ? refreshDmMessages(dispatch, conversationId, conversation.conversationKey, state.identity.userId, otherUserIds, state.settings.readReceipts)
      : Promise.resolve();

  /** Sends a small control message (pin, vote...) and refreshes so it shows. */
  async function control(work: (key: Uint8Array) => Promise<void>) {
    if (!conversation?.conversationKey) return;
    try {
      await work(conversation.conversationKey);
      await refreshNow();
    } catch (err) {
      console.error("Couldn't update the chat", err);
    }
  }

  function startEdit(message: Message) {
    if (!canEdit(message)) return;
    setReplyingTo(null);
    setEditing(message);
    setDraft(message.text);
    draftRef.current?.focus();
  }

  function cancelEdit() {
    setEditing(null);
    setDraft("");
  }

  async function send() {
    if (!draft.trim() || sending || !conversation?.conversationKey) return;
    // Account keys never change in normal use, so a new one holds sending until it's verified.
    if (keyChanged) return;
    // A chat on per-device keys sends text that way (replies and edits are protocol 1 only in this preview).
    if (editing?.v3) {
      setSending(true);
      try {
        if (draft.trim() !== editing.text) await v3.edit(editing.id!, draft.trim());
        cancelEdit();
      } catch (err) {
        console.error("Failed to edit with device keys", err);
      } finally {
        setSending(false);
      }
      return;
    }
    if (v3.active && v3.mode !== "v1" && !editing && !replyingTo) {
      setSending(true);
      try {
        await v3.send(draft);
        setDraft("");
      } catch (err) {
        console.error("Failed to send with device keys", err);
      } finally {
        setSending(false);
      }
      return;
    }
    if (editing?.id) {
      setSending(true);
      try {
        if (draft.trim() !== editing.text) await sendEdit(conversationId, conversation.conversationKey, editing.id, draft);
        cancelEdit();
        await refreshNow();
      } catch (err) {
        console.error("Failed to edit message", err);
      } finally {
        setSending(false);
      }
      return;
    }
    setSending(true);
    try {
      const replyTo =
        replyingTo?.id && state.identity
          ? {
              messageId: replyingTo.id,
              senderId:
                replyingTo.from === "me"
                  ? state.identity.userId
                  : conversation.kind === "dm"
                    ? (conversation.userId ?? replyingTo.from)
                    : replyingTo.from,
              preview: replyingTo.file ? `📎 ${replyingTo.file.descriptor.filename}` : replyingTo.text,
              fromMe: replyingTo.from === "me",
            }
          : undefined;
      // To someone who charges for messages: pay first (the bar above the composer), then send.
      const paid = paidMessaging.sendOptions();
      if (paid === null) return;
      await sendDmMessage(dispatch, conversationId, conversation.conversationKey, draft, replyTo, paid);
      if (paidMessaging.quote) paidMessaging.afterSend();
      setDraft("");
      setReplyingTo(null);
    } catch (err) {
      console.error("Failed to send message", err);
      if (err instanceof ApiError && err.code === "payment_required") paidMessaging.refresh();
      if (err instanceof ApiError && err.code === "blocked") {
        setFileStage(null);
        setFileError("You can't send messages here: one of you has blocked the other.");
      }
    } finally {
      setSending(false);
    }
  }

  function openFilePicker() {
    if (!canSendFiles) {
      setFileStage(null);
      setFileError(PRO_REQUIRED_MESSAGE);
      return;
    }
    fileInputRef.current?.click();
  }

  function pickFile(e: React.ChangeEvent<HTMLInputElement>) {
    const picked = e.target.files?.[0] ?? null;
    // Reset so choosing the same file again still fires onChange.
    e.target.value = "";
    setFileStage(null);
    if (!canSendFiles) {
      setPendingFile(null);
      setFileError(PRO_REQUIRED_MESSAGE);
      return;
    }
    setFileError(null);
    setPendingFile(picked);
  }

  async function sendFile() {
    if (!pendingFile || !conversation?.conversationKey) return;
    if (fileStage && fileStage !== "sent") return;
    if (keyChanged) {
      setFileError("Sending is paused because this contact's safety number changed. Verify it first.");
      return;
    }
    setFileError(null);
    try {
      if (v3.active && v3.mode !== "v1") {
        await v3.sendFile(await uploadFileForDeviceKeys(conversationId, pendingFile, setFileStage));
        setFileStage("sent");
        setPendingFile(null);
        setTimeout(() => setFileStage((s) => (s === "sent" ? null : s)), 2000);
        return;
      }
      await sendDmFile(dispatch, conversationId, conversation.conversationKey, pendingFile, setFileStage);
      // Only reached once upload + message both succeeded.
      setFileStage("sent");
      setPendingFile(null);
      setTimeout(() => setFileStage((s) => (s === "sent" ? null : s)), 2000);
    } catch (err) {
      setFileStage(null);
      setFileError(err instanceof FileActionError ? err.message : "Upload failed — try again.");
    }
  }

  const fileBusy = fileStage !== null && fileStage !== "sent";

  /** Adds the emoji, or removes it if it's already your reaction, then refreshes so everyone's reactions show. */
  async function toggleReaction(message: Message, emoji: ReactionEmoji) {
    if (message.v3) {
      await v3.react(message, emoji).catch((err) => console.error("Failed to react with device keys", err));
      return;
    }
    if (!conversation?.conversationKey || !state.identity || !message.id) return;
    const mine = message.reactions?.find((r) => r.emoji === emoji && r.mine);
    try {
      if (mine?.myReactionId) await removeReaction(mine.myReactionId);
      else await sendReaction(conversationId, conversation.conversationKey, message.id, emoji);
      await refreshDmMessages(dispatch, conversationId, conversation.conversationKey, state.identity.userId, otherUserIds, state.settings.readReceipts);
    } catch (err) {
      console.error("Failed to update reaction", err);
    }
  }

  function deleteForMe(message: Message) {
    if (!state.identity || !message.id) return;
    if (!window.confirm("Delete this message for you? The other person still has it.")) return;
    hideMessageForMe(dispatch, state.identity.userId, conversationId, message.id);
  }

  function deleteMessageAt(index: number) {
    if (!conversation || !window.confirm("Delete this message? This can't be undone.")) return;
    const message = conversation.messages[index];
    if (!message?.id) return;
    deleteDmMessage(dispatch, conversationId, message.id, deletedWith(message, conversation.messages)).catch((err) => {
      console.error("Failed to delete message", err);
    });
  }

  async function forwardTo(target: Conversation) {
    const message = forwarding;
    if (!message || !target.conversationKey) return;
    setForwardError(null);
    try {
      if (message.file) {
        // A file belongs to the chat it was uploaded to: it's decrypted here and sent again (a Pro feature, like any file).
        if (!canSendFiles) throw new FileActionError(PRO_REQUIRED_MESSAGE);
        const blob = await downloadDmFile(message.file.descriptor, () => {});
        const file = new File([blob], message.file.descriptor.filename, { type: message.file.descriptor.mimeType });
        await sendDmFile(dispatch, target.id, target.conversationKey, file, () => {});
      } else {
        // A post forwarded from a channel stays one, so every copy keeps counting toward its views.
        if (message.channelPost) await sendChannelForward(target.id, target.conversationKey, message.text, message.channelPost);
        else await sendForwarded(target.id, target.conversationKey, message.text);
      }
      setForwarding(null);
    } catch (err) {
      setForwardError(err instanceof FileActionError ? err.message : "Couldn't forward it. Try again.");
    }
  }

  async function submitPoll() {
    if (!conversation?.conversationKey) return;
    setPollError(null);
    try {
      await createPoll(conversationId, conversation.conversationKey, pollQuestion, pollOptions, pollMultiple);
      setPollOpen(false);
      setPollQuestion("");
      setPollOptions(["", ""]);
      setPollMultiple(false);
      await refreshNow();
    } catch {
      setPollError("Write a question and 2 to 10 different options.");
    }
  }

  function showPinned(messageId: string) {
    const element = document.getElementById(`msg-${messageId}`);
    element?.scrollIntoView?.({ block: "center", behavior: "smooth" });
    setFlashId(messageId);
    setTimeout(() => setFlashId((id) => (id === messageId ? null : id)), 1600);
  }

  function changeDisappearing(duration: DurationKey) {
    if (!conversation) return;
    updateDisappearing(dispatch, conversationId, duration, conversation.disappearing).catch((err) => {
      console.error("Failed to update disappearing messages", err);
    });
  }

  const openVerify = () => conversation.kind === "dm" && dispatch({ type: "OPEN_VERIFY_FROM_THREAD", contactId: conversation.id });

  const status = community ? (
    <span className={styles.statusMuted}>
      {community.kind === "channel" ? "Channel" : "Community"} · end-to-end encrypted{readOnly ? " · read only" : ""}
    </span>
  ) : conversation.kind === "group" ? (
      <span className={styles.statusMuted}>
        {verifiedCount(conversation, contactsById)} of {conversation.members.filter((m) => !m.isSelf).length} verified
      </span>
    ) : keyChanged ? (
      <span className={styles.statusWarn}>Safety number changed</span>
    ) : conversation.verified ? (
      <span className={styles.statusOk}>
        <ShieldCheckIcon size={14} /> Verified{online ? " · online" : ""}
      </span>
    ) : (
      <span className={styles.statusWarn}>Not verified{online ? " · online" : ""}</span>
    );

  // Mentions: in a group, "@handle" of a member is highlighted (yours stands out).
  const formatOptions: FormatOptions | undefined =
    conversation.kind === "group"
      ? { mentions: conversation.members.map((m) => m.handle), selfHandle: state.identity?.handle }
      : undefined;
  // Typing "@" in a group suggests members.
  const mentionQuery = (() => {
    if (conversation.kind !== "group") return null;
    const caret = draftRef.current?.selectionStart ?? draft.length;
    const match = /(?:^|[\s([{"'])@([^\s@]{0,64})$/.exec(draft.slice(0, caret));
    return match ? match[1].toLowerCase() : null;
  })();
  const mentionMatches =
    mentionQuery === null || conversation.kind !== "group"
      ? []
      : conversation.members.filter((m) => !m.isSelf && m.handle.toLowerCase().startsWith(mentionQuery)).slice(0, 6);
  function pickMention(handle: string) {
    const caret = draftRef.current?.selectionStart ?? draft.length;
    const before = draft.slice(0, caret);
    const at = before.lastIndexOf("@");
    if (at < 0) return;
    const next = `${draft.slice(0, at)}@${handle} ${draft.slice(caret)}`;
    setDraft(next);
    setMentionIndex(0);
    requestAnimationFrame(() => {
      const position = at + handle.length + 2;
      draftRef.current?.focus();
      draftRef.current?.setSelectionRange(position, position);
    });
  }

  const pins = pinnedFrom(conversation.messages).reverse();
  const shownPin = pins.length > 0 ? pins[Math.min(pinIndex, pins.length - 1)] : null;
  const pinnedIds = new Set(pins.map((p) => p.messageId));
  const forwardTargets = state.conversations.filter((c) => c.id !== conversationId && c.conversationKey);

  let lastDay = "";

  return (
    <div className={styles.layout}>
      <div className={styles.screen}>
        <header className={styles.header}>
          {!isDesktop && (
            <button type="button" className={styles.iconButton} aria-label="Back" onClick={() => dispatch({ type: "GO_CHATS" })}>
              <ChevronLeftIcon />
            </button>
          )}
          <button
            type="button"
            className={styles.headerButton}
            onClick={
              community
                ? () => dispatch({ type: "OPEN_COMMUNITIES", page: "community", communityId: community.id })
                : isGroup
                ? () => dispatch({ type: "OPEN_GROUP_INFO", conversationId })
                : conversation.userId
                  ? () => dispatch({ type: "OPEN_PROFILE", userId: conversation.userId!, origin: { screen: "thread", conversationId } })
                  : undefined
            }
          >
            {isGroup ? (
              <GroupGlyph label={conversation.label} memberSeeds={conversation.members.filter((m) => !m.isSelf).map((m) => m.handle)} size={42} />
            ) : (
              <Avatar avatarUrl={conversation.avatarUrl} seed={conversation.handle} size={42} online={online} />
            )}
            <span className={styles.headerText}>
              <span className={styles.headerName}>{name}</span>
              <span className={styles.headerStatus}>{status}</span>
            </span>
          </button>
          <div className={styles.headerActions}>
            {(showCallButtons || showGroupCallButtons) && (
              <>
                <button
                  type="button"
                  className={styles.iconButton}
                  onClick={() => placeCall("voice")}
                  disabled={callBusy || groupCallBlocked}
                  aria-label={`Voice call ${name}`}
                  title={isGroup ? groupCallTitle("voice") : "Voice call"}
                >
                  <PhoneIcon />
                </button>
                <button
                  type="button"
                  className={styles.iconButton}
                  onClick={() => placeCall("video")}
                  disabled={callBusy || groupCallBlocked}
                  aria-label={`Video call ${name}`}
                  title={isGroup ? groupCallTitle("video") : "Video call"}
                >
                  <VideoIcon />
                </button>
              </>
            )}
            <button
              type="button"
              className={`${styles.iconButton} ${searchOpen ? styles.iconButtonOn : ""}`}
              aria-label="Search in chat"
              aria-pressed={searchOpen}
              onClick={() => {
                setSearchOpen((v) => !v);
                setSearch("");
              }}
            >
              <SearchIcon />
            </button>
            {isDesktop && (
              <button
                type="button"
                className={`${styles.iconButton} ${detailsOpen ? styles.iconButtonOn : ""}`}
                aria-label="Chat details"
                aria-pressed={detailsOpen}
                onClick={() => setPrefs({ chatDetailsOpen: !prefs.chatDetailsOpen })}
              >
                <PanelRightIcon />
              </button>
            )}
          </div>
        </header>

        {shownPin && (
          <div className={styles.pinnedBar}>
            <button
              type="button"
              className={styles.pinnedMain}
              onClick={() => {
                showPinned(shownPin.messageId);
                setPinIndex((i) => (i + 1) % pins.length);
              }}
            >
              <span className={styles.pinnedTitle}>
                📌 {pins.length > 1 ? `Pinned message ${Math.min(pinIndex, pins.length - 1) + 1} of ${pins.length}` : "Pinned message"}
              </span>
              <span className={styles.pinnedPreview}>{toPlain(shownPin.preview)}</span>
            </button>
            <button
              type="button"
              className={styles.attachmentCancel}
              aria-label="Unpin for everyone"
              title="Unpin for everyone"
              onClick={() => control((key) => unpinMessage(conversationId, key, shownPin.messageId))}
            >
              ✕
            </button>
          </div>
        )}

        {searchOpen && (
          <div className={styles.searchBar}>
            <SearchIcon size={18} />
            <input autoFocus type="search" placeholder="Search this chat" value={search} onChange={(e) => setSearch(e.target.value)} aria-label="Search this chat" />
            {search && <span className={styles.searchCount}>{timeline.length} found</span>}
          </div>
        )}

        {ongoingGroupCall && !inThisGroupCall && (
          <button type="button" className={styles.banner} onClick={() => placeCall(ongoingGroupCall.kind)} disabled={callBusy}>
            <span>
              A group {ongoingGroupCall.kind} call is going on
              {ongoingGroupCall.participants > 0 ? ` · ${ongoingGroupCall.participants} in the call` : ""}
            </span>
            <span className={styles.bannerAction}>Join</span>
          </button>
        )}

        {state.outbox.offline && (
          // Nothing goes another way: what's written waits, sealed, until the connection is back.
          <div className={styles.banner} role="status">
            <span>
              {onOnionOrigin()
                ? TOR_UNAVAILABLE_MESSAGE
                : "Can't reach the server. Your messages are kept and sent once the connection is back."}
              {state.outbox.queued > 0 ? ` (${state.outbox.queued} waiting)` : ""}
            </span>
          </div>
        )}

        {conversation.keyProblem && (
          <div className={`${styles.banner} ${styles.dangerBanner}`}>
            <span>{conversation.keyProblem}</span>
          </div>
        )}

        {keyChanged && (
          <button type="button" className={`${styles.banner} ${styles.dangerBanner}`} onClick={openVerify}>
            <span>
              {conversation.kind === "dm" && conversation.handle}'s safety number changed. Someone may be intercepting. Sending is paused until
              you verify it.
            </span>
            <span className={styles.bannerAction}>Verify</span>
          </button>
        )}

        {showRetentionBanner && earliestDeleteAt != null && (
          <div className={styles.banner}>
            <span>
              A message here will be permanently deleted on{" "}
              {new Date(earliestDeleteAt).toLocaleDateString(undefined, { month: "long", day: "numeric", year: "numeric" })}.
            </span>
          </div>
        )}

        {!detailsOpen && (
          <div className={styles.durationRow}>
            <span className={styles.durationLabel}>
              <TimerIcon size={15} /> Disappearing
            </span>
            <DurationSelect value={conversation.disappearing} onChange={changeDisappearing} />
          </div>
        )}

        <div className={styles.messages}>
          <div className={styles.notice}>
            <LockIcon size={18} />
            <span>
              Messages and calls are end-to-end encrypted. Nobody outside this chat — not even us — can read or listen to them.
              {conversation.kind === "dm" && (
                <>
                  {" "}
                  <button type="button" className={styles.noticeLink} onClick={openVerify}>
                    {conversation.verified && !keyChanged ? "View safety number" : "Verify keys"}
                  </button>
                </>
              )}
            </span>
          </div>
          {v3.active && v3.mode !== "v1" && (
            <div className={styles.notice} role="status">
              <LockIcon size={18} />
              <span>
                Per-device keys (preview): new text messages in this chat are encrypted for each device and kept only on the devices that have them.
                {v3.error && <strong> {v3.error}</strong>}
                {v3.mode === "blocked" && (
                  <>
                    {" "}
                    <button type="button" className={styles.noticeLink} onClick={() => void v3.allowOlderProtocol()}>
                      Use the older protocol for this chat
                    </button>
                  </>
                )}
              </span>
            </div>
          )}
          {conversation.disappearing !== "off" && (
            <div className={`${styles.caption} type-mono`}>
              <TimerIcon size={13} /> Disappearing messages · {DURATION_LABELS[conversation.disappearing].toLowerCase()}
            </div>
          )}

          {timeline.map((item) => {
            const day = item.at ? dayLabel(item.at) : "";
            const separator = day && day !== lastDay ? (
              <div className={styles.daySeparator} key={`day-${day}-${item.at}`}>
                {day}
              </div>
            ) : null;
            if (day) lastDay = day;
            if (item.kind === "call") {
              return (
                <div key={`call-${item.call.id}`}>
                  {separator}
                  <CallEntry call={item.call} />
                </div>
              );
            }
            const m = item.message;
            return (
              <div key={m.id ?? `i-${item.index}`}>
                {separator}
                <MessageBubble
                  message={m}
                  onDelete={
                    m.from === "me" && !m.queueId && !m.deleted
                      ? m.v3
                        ? () => window.confirm("Delete this message? This can't be undone.") && void v3.remove(m).catch(() => {})
                        : () => deleteMessageAt(item.index)
                      : undefined
                  }
                  onRetryQueued={m.queueId ? () => void (m.v3 ? v3.retry(m.queueId!) : retryQueued(dispatch, state.conversations, m.queueId!)) : undefined}
                  onDiscardQueued={m.queueId ? () => void (m.v3 ? v3.discard(m.queueId!) : discardQueued(dispatch, m.queueId!)) : undefined}
                  onRequestVerification={() => loadMessageVerification(dispatch, conversationId, m)}
                  onOpenDetails={() => m.id && !m.v3 && dispatch({ type: "OPEN_MESSAGE_SECURITY", conversationId, messageId: m.id })}
                  senderName={isGroup && m.from !== "me" ? handleOf(m.from) : undefined}
                  onReply={m.id && !m.v3 ? () => setReplyingTo(m) : undefined}
                  onToggleReaction={m.id && !m.deleted ? (emoji) => toggleReaction(m, emoji) : undefined}
                  onDeleteForMe={m.id && !m.v3 ? () => deleteForMe(m) : undefined}
                  formatOptions={formatOptions}
                  onEdit={canEdit(m) ? () => startEdit(m) : undefined}
                  onForward={m.id && !m.v3 ? () => setForwarding(m) : undefined}
                  onReport={community && m.id && !m.v3 && m.from !== "me" && m.from !== "system" ? () => setReporting(m) : undefined}
                  onOpenChannel={(channelId) => dispatch({ type: "OPEN_CHANNELS", channelId })}
                  pinned={m.id ? pinnedIds.has(m.id.toLowerCase()) : false}
                  onTogglePin={
                    m.id && !m.v3
                      ? () =>
                          control((key) =>
                            pinnedIds.has(m.id!.toLowerCase())
                              ? unpinMessage(conversationId, key, m.id!)
                              : pinMessage(conversationId, key, m.id!, m.file ? `📎 ${m.file.descriptor.filename}` : m.poll ? `📊 ${m.text}` : m.text)
                          )
                      : undefined
                  }
                  onVote={m.id && m.poll ? (options) => control((key) => votePoll(conversationId, key, m.id!, options, m.poll!.options)) : undefined}
                  onEndPoll={m.id && m.poll && m.from === "me" ? () => control((key) => endPoll(conversationId, key, m.id!)) : undefined}
                  noticeWho={m.notice ? (m.from === "me" ? "You" : handleOf(m.from)) : undefined}
                  highlighted={Boolean(m.id && flashId === m.id)}
                />
              </div>
            );
          })}

          {typingUsers.length > 0 && !search && (
            <div className={styles.typingRow} aria-live="polite">
              <span className={styles.typingBubble} aria-label={`${typingUsers.map(handleOf).join(", ")} typing`}>
                <span />
                <span />
                <span />
              </span>
            </div>
          )}
          <div ref={messagesEnd} />
        </div>

        {(pendingFile || fileStage || fileError) && (
          <div className={styles.attachmentBar} aria-live="polite">
            {pendingFile && (
              <span className={styles.attachmentName} title={pendingFile.name}>
                {pendingFile.name} · {formatBytes(pendingFile.size)}
              </span>
            )}
            {fileStage && <span className={`${styles.attachmentStage} type-mono`}>{FILE_STAGE_LABEL[fileStage]}</span>}
            {fileError && (
              <span className={styles.attachmentError} role="alert">
                {fileError}
              </span>
            )}
            {pendingFile && !fileBusy && (
              <>
                <button type="button" className={styles.smallPrimary} onClick={sendFile}>
                  {fileError ? "Retry" : "Send file"}
                </button>
                <button
                  type="button"
                  className={styles.attachmentCancel}
                  onClick={() => {
                    setPendingFile(null);
                    setFileError(null);
                  }}
                >
                  Cancel
                </button>
              </>
            )}
          </div>
        )}

        {editing && (
          <div className={styles.replyBar}>
            <div className={styles.replyBarText}>
              <span className={styles.replyBarWho}>Editing message</span>
              <span className={styles.replyBarPreview}>Enter saves · Esc cancels</span>
            </div>
            <button type="button" className={styles.attachmentCancel} aria-label="Cancel editing" onClick={cancelEdit}>
              ✕
            </button>
          </div>
        )}

        {pollOpen && (
          <div className={styles.pollPanel} role="group" aria-label="New poll">
            <input
              className={styles.pollInput}
              placeholder="Ask a question"
              aria-label="Poll question"
              maxLength={300}
              value={pollQuestion}
              onChange={(e) => setPollQuestion(e.target.value)}
            />
            {pollOptions.map((option, index) => (
              <div key={index} className={styles.pollOptionRow}>
                <input
                  className={styles.pollInput}
                  placeholder={`Option ${index + 1}`}
                  aria-label={`Option ${index + 1}`}
                  maxLength={100}
                  value={option}
                  onChange={(e) => setPollOptions((all) => all.map((o, i) => (i === index ? e.target.value : o)))}
                />
                {pollOptions.length > 2 && (
                  <button
                    type="button"
                    className={styles.attachmentCancel}
                    aria-label={`Remove option ${index + 1}`}
                    onClick={() => setPollOptions((all) => all.filter((_, i) => i !== index))}
                  >
                    ✕
                  </button>
                )}
              </div>
            ))}
            <div className={styles.pollActions}>
              {pollOptions.length < 10 && (
                <button type="button" className={styles.attachmentCancel} onClick={() => setPollOptions((all) => [...all, ""])}>
                  + Add an option
                </button>
              )}
              <label className={styles.pollCheck}>
                <input type="checkbox" checked={pollMultiple} onChange={(e) => setPollMultiple(e.target.checked)} /> More than one answer
              </label>
              {pollError && (
                <span className={styles.attachmentError} role="alert">
                  {pollError}
                </span>
              )}
              <button type="button" className={styles.attachmentCancel} onClick={() => setPollOpen(false)}>
                Cancel
              </button>
              <button type="button" className={styles.smallPrimary} onClick={submitPoll} disabled={!pollQuestion.trim()}>
                Send poll
              </button>
            </div>
          </div>
        )}

        {mentionMatches.length > 0 && (
          <div className={styles.mentionList} role="listbox" aria-label="Mention someone">
            {mentionMatches.map((member, index) => (
              <button
                key={member.userId}
                type="button"
                role="option"
                aria-selected={index === mentionIndex}
                className={`${styles.mentionOption} ${index === mentionIndex ? styles.mentionOptionOn : ""}`}
                onMouseDown={(e) => {
                  e.preventDefault();
                  pickMention(member.handle);
                }}
              >
                @{member.handle}
              </button>
            ))}
          </div>
        )}

        {replyingTo && (
          <div className={styles.replyBar}>
            <div className={styles.replyBarText}>
              <span className={styles.replyBarWho}>Replying to {replyingTo.from === "me" ? "yourself" : handleOf(replyingTo.from)}</span>
              <span className={styles.replyBarPreview}>{replyingTo.file ? `📎 ${replyingTo.file.descriptor.filename}` : replyingTo.text}</span>
            </div>
            <button type="button" className={styles.attachmentCancel} aria-label="Cancel reply" onClick={() => setReplyingTo(null)}>
              ✕
            </button>
          </div>
        )}

        {readOnly ? (
          <div className={styles.composerWrap}>
            <div className={styles.hints}>
              <span className={styles.hintSecure}>
                <LockIcon size={12} /> Only the creator and staff post in this channel.
              </span>
            </div>
          </div>
        ) : (
        <div className={styles.composerWrap}>
          {conversation.kind === "dm" && conversation.userId && <PaidMessageBar name={conversation.handle} creatorUserId={conversation.userId} paid={paidMessaging} />}
          <div className={styles.composer}>
            <button
              type="button"
              className={styles.composerIcon}
              aria-label="Attach file"
              title={canSendFiles ? "Attach file" : "Attach file (Pro)"}
              onClick={openFilePicker}
              disabled={fileBusy}
            >
              <PaperclipIcon />
            </button>
            <input ref={fileInputRef} type="file" hidden onChange={pickFile} data-testid="file-input" />
            <button
              type="button"
              className={styles.composerIcon}
              aria-label="Create a poll"
              title="Create a poll"
              onClick={() => {
                setPollError(null);
                setPollOpen((v) => !v);
              }}
            >
              📊
            </button>
            <textarea
              ref={draftRef}
              className={styles.input}
              placeholder="Message"
              aria-label="Write an encrypted message"
              rows={1}
              value={draft}
              onChange={(e) => {
                setDraft(e.target.value);
                if (e.target.value) live.notifyTyping(conversationId);
              }}
              onKeyDown={(e) => {
                if (mentionMatches.length > 0) {
                  if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                    e.preventDefault();
                    const n = mentionMatches.length;
                    setMentionIndex((i) => (i + (e.key === "ArrowDown" ? 1 : n - 1)) % n);
                    return;
                  }
                  if (e.key === "Enter" || e.key === "Tab") {
                    e.preventDefault();
                    pickMention(mentionMatches[Math.min(mentionIndex, mentionMatches.length - 1)].handle);
                    return;
                  }
                }
                if (e.key === "Escape" && editing) {
                  e.preventDefault();
                  cancelEdit();
                  return;
                }
                // Up in an empty composer edits your last message, like Signal.
                if (e.key === "ArrowUp" && !draft && !editing) {
                  const last = [...conversation.messages].reverse().find((m) => m.from === "me" && !m.notice);
                  if (last && canEdit(last)) {
                    e.preventDefault();
                    startEdit(last);
                    return;
                  }
                }
                if (e.key === "Enter" && !e.shiftKey) {
                  e.preventDefault();
                  void send();
                }
              }}
            />
            <button type="button" className={styles.sendButton} onClick={send} disabled={!draft.trim() || sending} aria-label="Send">
              <SendIcon />
            </button>
          </div>
          <div className={styles.hints}>
            <span className={styles.hintSecure}>
              <LockIcon size={12} /> Encrypted on this device before sending
            </span>
            {isDesktop && <span>Enter to send · Shift + Enter for a new line</span>}
          </div>
        </div>
        )}
      </div>
      {detailsOpen && <ChatDetails conversation={conversation} />}
      {reporting && community && (
        <CommunityReportDialog
          communityId={community.id}
          target={{ kind: "content", messageId: reporting.id!, senderId: reporting.from === "me" || reporting.from === "system" ? null : reporting.from, text: reporting.text }}
          onClose={() => setReporting(null)}
          onDone={() => setFileError("Report sent. Thank you.")}
        />
      )}

      {forwarding && (
        <div className={styles.overlay} role="dialog" aria-modal="true" aria-label="Forward message">
          <div className={styles.dialog}>
            <div className={styles.dialogTitle}>Forward to…</div>
            {forwardTargets.length === 0 && <p className={styles.dialogHint}>There's no other chat to forward to yet.</p>}
            <div className={styles.dialogList}>
              {forwardTargets.map((target) => (
                <button key={target.id} type="button" className={styles.dialogRow} onClick={() => forwardTo(target)}>
                  {target.kind === "group" ? target.label : target.handle}
                </button>
              ))}
            </div>
            {forwarding.file && <p className={styles.dialogHint}>The file is encrypted again on this device and uploaded for that chat.</p>}
            {forwardError && (
              <p className={styles.attachmentError} role="alert">
                {forwardError}
              </p>
            )}
            <button
              type="button"
              className={styles.attachmentCancel}
              onClick={() => {
                setForwarding(null);
                setForwardError(null);
              }}
            >
              Cancel
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
