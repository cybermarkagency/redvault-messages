// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import { countChannelViews } from "../lib/channelViews.js";
import { REACTION_EMOJI, type ReactionEmoji } from "../lib/richMessage.js";
import { type FormatOptions, toPlain } from "../lib/textFormat.js";
import type { Message, VerificationStatus } from "../types.js";
import { EmojiPicker } from "./EmojiPicker.js";
import { FileBubble } from "./FileBubble.js";
import { FormattedText } from "./FormattedText.js";
import { HourglassIcon, TrashIcon } from "./icons.js";
import styles from "./MessageBubble.module.css";

export interface MessageBubbleProps {
  message: Message;
  /** Present only for messages the current user sent — that's the only case a delete button should render. */
  onDelete?: () => void;
  /** Called once, the first time the security panel is expanded for a message that doesn't have `security` yet (e.g. loaded from a poll rather than sent this session) — see state/actions.ts's loadMessageVerification. */
  onRequestVerification?: () => void;
  /** Navigates to the full "View Verification Details" screen. */
  onOpenDetails?: () => void;
  /** Groups: the sender's handle, shown above messages from other members. */
  senderName?: string;
  /** Real DMs: start a reply to this message. */
  onReply?: () => void;
  /** Real DMs: add, or remove if already mine, this emoji reaction. */
  onToggleReaction?: (emoji: ReactionEmoji) => void;
  /** Real DMs: hide this message in this browser only. */
  onDeleteForMe?: () => void;
  /** Mentions to highlight (a group's members). */
  formatOptions?: FormatOptions;
  /** Your own text message from the last 24 hours: start editing it. */
  onEdit?: () => void;
  /** Pin or unpin this message for everyone in the chat. */
  onTogglePin?: () => void;
  pinned?: boolean;
  onForward?: () => void;
  /** Communities: report this message (the reporter chooses whether to share its text). */
  onReport?: () => void;
  /** A post forwarded from a channel: open that channel. */
  onOpenChannel?: (channelId: string) => void;
  /** A queued message (not on the server yet): send again after a refusal, or drop it unsent. */
  onRetryQueued?: () => void;
  onDiscardQueued?: () => void;
  /** A poll: the new set of picked options (empty takes the vote back). */
  onVote?: (options: number[]) => void;
  /** Your poll: end it. */
  onEndPoll?: () => void;
  /** A pin notice: who did it ("You" or a handle). */
  noticeWho?: string;
  /** Outlined for a moment (the pinned bar jumped here). */
  highlighted?: boolean;
}

const VERIFICATION_LABEL: Record<VerificationStatus, string> = {
  pending: "Checking…",
  verified: "Server's copy matches ✓",
  failed: "Server's copy differs ✗",
  unavailable: "Not compared",
};

/** Compact status shown next to the timestamp — real events only, never a decorative checkmark. */
function compactStatus(message: Message): string {
  if (!message.security) return "Sent";
  if (message.security.verification === "failed") return "⚠ Server's copy differs";
  if (message.seen) return "✓✓ Read";
  if (message.delivered) return "✓✓ Delivered";
  if (message.security.verification === "verified") return "✓ Sent, server's copy matches";
  return "✓ Sent";
}

const clock = (epochMs: number) => new Date(epochMs).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });

export function MessageBubble({
  message,
  onDelete,
  onRequestVerification,
  onOpenDetails,
  senderName,
  onReply,
  onToggleReaction,
  onDeleteForMe,
  formatOptions,
  onEdit,
  onTogglePin,
  pinned,
  onForward,
  onReport,
  onOpenChannel,
  onVote,
  onEndPoll,
  noticeWho,
  highlighted,
  onRetryQueued,
  onDiscardQueued,
}: MessageBubbleProps) {
  // A forwarded channel post counts as a view of the original (once per device: lib/channelViews.ts).
  const channelPostId = message.channelPost?.postId;
  useEffect(() => {
    if (channelPostId) countChannelViews([channelPostId]);
  }, [channelPostId]);

  const sent = message.from === "me";
  const [expanded, setExpanded] = useState(false);
  const [picking, setPicking] = useState(false);
  // The full emoji picker, opened from "+" after the quick reactions.
  const [pickingAny, setPickingAny] = useState(false);
  const [showVersions, setShowVersions] = useState(false);
  const canInspect = sent && Boolean(message.id);

  // A pin or unpin: a small line in the timeline, not a bubble.
  if (message.notice) {
    return (
      <div className={styles.notice} id={message.id ? `msg-${message.id}` : undefined}>
        📌 {noticeWho ?? "Someone"} {message.notice.kind === "pin" ? <>pinned “{toPlain(message.text)}”</> : "unpinned a message"} · {message.time}
      </div>
    );
  }

  function toggleExpanded() {
    if (!canInspect) return;
    const next = !expanded;
    setExpanded(next);
    if (next && !message.security) onRequestVerification?.();
  }

  const poll = message.poll;
  function pickOption(index: number) {
    if (!poll || poll.closed || !onVote) return;
    const already = poll.mine.includes(index);
    onVote(already ? poll.mine.filter((i) => i !== index) : poll.multiple ? [...poll.mine, index] : [index]);
  }

  return (
    <div className={`${styles.row} ${sent ? styles.rowSent : styles.rowReceived}`} id={message.id ? `msg-${message.id}` : undefined}>
      <div className={`${styles.bubble} ${sent ? styles.sent : styles.received} ${highlighted ? styles.highlighted : ""}`}>
        {senderName && <div className={styles.sender}>{senderName}</div>}
        {message.unverifiedSender && (
          <div className={styles.securityNote} title="Their apps sign every message, but this one has no signature: it came from an older app of theirs, or someone else made it.">
            ⚠ Sender not verified
          </div>
        )}
        {message.withoutForwardSecrecy && (
          <div className={styles.securityNote} title="This chat uses forward-secret encryption, but this message was sent with the older protocol.">
            ⚠ Sent without forward secrecy
          </div>
        )}
        {message.channelPost ? (
          <button type="button" className={`${styles.forwarded} ${styles.forwardedLink}`} onClick={() => onOpenChannel?.(message.channelPost!.channelId)}>
            ↪ Forwarded from {message.channelPost.channelName}
          </button>
        ) : (
          message.forwarded && <div className={styles.forwarded}>↪ Forwarded</div>
        )}
        {message.storyReply && (
          <div className={styles.quote}>
            <div className={styles.quoteWho}>
              {sent
                ? message.storyReply.reaction
                  ? "You reacted to their story"
                  : "You replied to their story"
                : message.storyReply.reaction
                  ? "Reacted to your story"
                  : "Replied to your story"}
            </div>
            <div className={styles.quoteText}>
              {message.storyReply.kind === "image" ? "📷 " : message.storyReply.kind === "video" ? "🎬 " : "Aa "}
              {message.storyReply.preview}
            </div>
          </div>
        )}
        {message.replyTo && (
          <div className={styles.quote}>
            <div className={styles.quoteWho}>{message.replyTo.fromMe ? "You" : "Them"}</div>
            <div className={styles.quoteText}>{message.replyTo.preview || "Message"}</div>
          </div>
        )}
        {message.file ? (
          <FileBubble file={message.file} sent={sent} />
        ) : poll ? (
          <div className={styles.poll}>
            <div className={styles.pollKind}>{poll.closed ? "Poll · ended" : poll.multiple ? "Poll · pick any" : "Poll · pick one"}</div>
            <div className={styles.pollQuestion}>{poll.question}</div>
            {poll.options.map((label, index) => {
              const share = poll.voters > 0 ? (poll.counts[index] / poll.voters) * 100 : 0;
              const mine = poll.mine.includes(index);
              return (
                <button
                  key={index}
                  type="button"
                  className={`${styles.pollOption} ${mine ? styles.pollOptionMine : ""}`}
                  disabled={poll.closed || !onVote}
                  aria-pressed={mine}
                  onClick={() => pickOption(index)}
                >
                  <span className={styles.pollBar} style={{ width: `${share}%` }} />
                  <span className={`${styles.pollMark} ${poll.multiple ? styles.pollMarkSquare : ""} ${mine ? styles.pollMarkOn : ""}`} />
                  <span className={styles.pollLabel}>{label}</span>
                  <span className={styles.pollCount}>{poll.counts[index]}</span>
                </button>
              );
            })}
            <div className={styles.pollFooter}>
              <span>{poll.voters === 1 ? "1 person voted" : `${poll.voters} people voted`}</span>
              {sent && !poll.closed && onEndPoll && (
                <button type="button" className={styles.pollEnd} onClick={onEndPoll}>
                  End poll
                </button>
              )}
            </div>
          </div>
        ) : (
          <div className={`${styles.text} type-row-primary`} style={{ fontWeight: 400 }}>
            <FormattedText text={message.text} options={formatOptions} />
          </div>
        )}
        {showVersions && message.earlierVersions && (
          <div className={styles.versions}>
            {message.earlierVersions.map((v, i) => (
              <div key={i} className={styles.version}>
                <span className={styles.versionTime}>{clock(v.at)}</span>
                <span>{v.text}</span>
              </div>
            ))}
          </div>
        )}
        <div className={`${styles.meta} ${sent ? styles.metaSent : ""}`}>
          {message.editedAt && (
            <button
              type="button"
              className={styles.editedButton}
              title="Show earlier versions"
              aria-expanded={showVersions}
              onClick={() => setShowVersions((v) => !v)}
            >
              edited
            </button>
          )}
          <span>{message.time}</span>
          {message.disappearing && <HourglassIcon />}
          {canInspect && (
            <button type="button" className={styles.statusButton} onClick={toggleExpanded} aria-expanded={expanded}>
              {compactStatus(message)}
            </button>
          )}
          {message.queueId && (
            // Waiting in the outbox: not sent, so never "Sent" or "Delivered".
            <span className={styles.statusButton} title={message.queueError ?? "Waiting to send"}>
              {message.queueHeld ? "⚠ Not sent" : "◷ Queued"}
            </span>
          )}
          {message.queueId && message.queueHeld && onRetryQueued && (
            <button type="button" className={styles.actionButton} onClick={onRetryQueued} title="Send it again">
              Retry
            </button>
          )}
          {message.queueId && onDiscardQueued && (
            <button type="button" className={styles.actionButton} onClick={onDiscardQueued} title="Delete it without sending">
              Discard
            </button>
          )}
          {onReply && (
            <button type="button" className={styles.actionButton} aria-label="Reply" title="Reply" onClick={onReply}>
              ↩
            </button>
          )}
          {onToggleReaction && (
            <button type="button" className={styles.actionButton} aria-label="React" title="React" onClick={() => setPicking((p) => !p)}>
              ☺
            </button>
          )}
          {onEdit && (
            <button type="button" className={styles.actionButton} aria-label="Edit" title="Edit" onClick={onEdit}>
              ✎
            </button>
          )}
          {onForward && (
            <button type="button" className={styles.actionButton} aria-label="Forward" title="Forward" onClick={onForward}>
              ↪
            </button>
          )}
          {onTogglePin && (
            <button
              type="button"
              className={styles.actionButton}
              aria-label={pinned ? "Unpin" : "Pin"}
              title={pinned ? "Unpin for everyone" : "Pin for everyone"}
              onClick={onTogglePin}
            >
              {pinned ? "⊗" : "📌"}
            </button>
          )}
          {onReport && (
            <button type="button" className={styles.actionButton} aria-label="Report" title="Report" onClick={onReport}>
              ⚑
            </button>
          )}
          {onDeleteForMe && (
            <button type="button" className={styles.actionButton} aria-label="Delete for me" title="Delete for me" onClick={onDeleteForMe}>
              ⊘
            </button>
          )}
          {sent && onDelete && (
            <button type="button" className={styles.deleteButton} aria-label="Delete for everyone" title="Delete for everyone" onClick={onDelete}>
              <TrashIcon />
            </button>
          )}
        </div>
        {picking && onToggleReaction && (
          <div className={styles.picker} role="group" aria-label="Choose a reaction">
            {REACTION_EMOJI.map((emoji) => (
              <button
                key={emoji}
                type="button"
                className={styles.pickerEmoji}
                onClick={() => {
                  setPicking(false);
                  onToggleReaction(emoji);
                }}
              >
                {emoji}
              </button>
            ))}
            <button
              type="button"
              className={styles.pickerEmoji}
              aria-label="More reactions"
              title="More reactions"
              aria-expanded={pickingAny}
              onClick={() => setPickingAny((v) => !v)}
            >
              +
            </button>
            {pickingAny && (
              <EmojiPicker
                alignEnd={sent}
                onClose={() => setPickingAny(false)}
                onPick={(emoji) => {
                  setPickingAny(false);
                  setPicking(false);
                  onToggleReaction(emoji);
                }}
              />
            )}
          </div>
        )}
        {message.reactions && message.reactions.length > 0 && (
          <div className={styles.reactions}>
            {message.reactions.map((r) => (
              <button
                key={r.emoji}
                type="button"
                className={`${styles.reactionChip} ${r.mine ? styles.reactionMine : ""}`}
                aria-label={`${r.emoji} ${r.count}${r.mine ? ", including you" : ""}`}
                onClick={() => onToggleReaction?.(r.emoji)}
              >
                {r.emoji} {r.count > 1 ? r.count : ""}
              </button>
            ))}
          </div>
        )}
        {expanded && canInspect && (
          <div className={styles.securityPanel}>
            <div className={`${styles.securityTitle} type-caption`}>Message Security</div>
            <SecurityRow label="Encrypted locally" ok />
            <SecurityRow label="Relayed as ciphertext" ok />
            <SecurityRow
              label="Server's copy matches"
              ok={message.security?.verification === "verified"}
              failed={message.security?.verification === "failed"}
              pending={!message.security || message.security.verification === "pending"}
            />
            <SecurityRow label="Delivered" ok={Boolean(message.delivered)} pending={!message.delivered} />
            {message.seen !== undefined && <SecurityRow label="Read" ok={Boolean(message.seen)} pending={!message.seen} />}
            {message.security && (
              <button type="button" className={styles.detailsLink} onClick={onOpenDetails}>
                View Verification Details
              </button>
            )}
          </div>
        )}
      </div>
    </div>
  );
}

function SecurityRow({ label, ok, failed, pending }: { label: string; ok?: boolean; failed?: boolean; pending?: boolean }) {
  const mark = failed ? "✗" : ok ? "✓" : pending ? "…" : "–";
  const className = failed ? styles.securityRowFailed : ok ? styles.securityRowOk : styles.securityRowPending;
  return (
    <div className={`${styles.securityRow} ${className} type-caption`}>
      <span>{label}</span>
      <span>{mark}</span>
    </div>
  );
}
