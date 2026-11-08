// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import * as api from "../lib/api.js";
import { TIER_LABELS } from "../types.js";
import styles from "./AdminMessages.module.css";

const MAX_OPTIONS = 10;
const when = (iso: string) => new Date(iso).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });

/** Every account, oldest first (the list endpoint pages at 200). */
async function loadAllUsers(): Promise<api.AdminUserDto[]> {
  const all: api.AdminUserDto[] = [];
  for (let offset = 0; ; offset += 200) {
    const page = await api.adminListUsers({ limit: 200, offset });
    all.push(...page);
    if (page.length < 200) return all;
  }
}

/** Send a message or a poll to the accounts you tick (or all of them), choose whether they can reply, and see how it's going. */
export function MessagesTab() {
  const [users, setUsers] = useState<api.AdminUserDto[]>([]);
  const [sent, setSent] = useState<api.AdminBroadcastDto[]>([]);
  const [loading, setLoading] = useState(true);
  const [loadError, setLoadError] = useState<string | null>(null);

  const [kind, setKind] = useState<"message" | "poll">("message");
  const [body, setBody] = useState("");
  const [options, setOptions] = useState(["", ""]);
  const [allowReplies, setAllowReplies] = useState(false);
  const [picked, setPicked] = useState<Set<string>>(new Set());
  const [sending, setSending] = useState(false);
  const [sendError, setSendError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  const recipients = users.filter((u) => !u.deletedAt);
  const allPicked = recipients.length > 0 && recipients.every((u) => picked.has(u.userId));
  const filledOptions = options.map((o) => o.trim()).filter(Boolean);
  const pollOk = filledOptions.length >= 2 && new Set(filledOptions.map((o) => o.toLowerCase())).size === filledOptions.length;
  const canSend = !sending && body.trim().length > 0 && picked.size > 0 && (kind === "message" || pollOk);

  async function load() {
    setLoadError(null);
    try {
      const [everyone, list] = await Promise.all([loadAllUsers(), api.adminListBroadcasts()]);
      setUsers(everyone);
      setSent(list);
    } catch {
      setLoadError("Couldn't load users and sent messages.");
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    load();
  }, []);

  function toggle(userId: string) {
    setPicked((current) => {
      const next = new Set(current);
      if (!next.delete(userId)) next.add(userId);
      return next;
    });
  }

  async function send() {
    if (!canSend) return;
    setSending(true);
    setSendError(null);
    setNotice(null);
    try {
      await api.adminSendBroadcast({
        kind,
        body: body.trim(),
        options: kind === "poll" ? filledOptions : [],
        allowReplies,
        recipientIds: [...picked],
      });
      setNotice(`Sent to ${picked.size} ${picked.size === 1 ? "person" : "people"}.`);
      setBody("");
      setOptions(["", ""]);
      setPicked(new Set());
      setSent(await api.adminListBroadcasts());
    } catch (err) {
      setSendError(err instanceof api.ApiError && err.message ? err.message : "Couldn't send that. Try again.");
    } finally {
      setSending(false);
    }
  }

  const repliesHint = allowReplies
    ? "On: they can write back to you."
    : kind === "poll"
      ? "Off: they can only read it and vote."
      : "Off: they can only read it.";

  return (
    <>
      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>NEW MESSAGE</span>
        <div className={styles.form}>
          <p className={`${styles.hint} type-secondary`}>
            Goes to each person's Inbox. It isn't end-to-end encrypted: the server can read it (it has to, to count poll votes).
          </p>
          {notice && <p className={styles.success}>{notice}</p>}
          {sendError && <p className={styles.error}>{sendError}</p>}
          <div className={styles.filterRow}>
            {(["message", "poll"] as const).map((k) => (
              <button
                key={k}
                type="button"
                className={`${styles.filterButton} ${kind === k ? styles.filterButtonActive : ""}`}
                onClick={() => setKind(k)}
              >
                {k === "message" ? "Message" : "Poll"}
              </button>
            ))}
          </div>
          <textarea
            className={styles.input}
            rows={4}
            maxLength={4000}
            placeholder={kind === "poll" ? "Your question" : "Your message"}
            aria-label={kind === "poll" ? "Poll question" : "Message"}
            value={body}
            onChange={(e) => setBody(e.target.value)}
          />
          {kind === "poll" && (
            <>
              {options.map((value, i) => (
                <div key={i} className={styles.optionRow}>
                  <input
                    className={styles.input}
                    placeholder={`Choice ${i + 1}`}
                    aria-label={`Choice ${i + 1}`}
                    maxLength={200}
                    value={value}
                    onChange={(e) => setOptions((current) => current.map((o, j) => (j === i ? e.target.value : o)))}
                  />
                  {options.length > 2 && (
                    <button
                      type="button"
                      className={styles.secondaryButton}
                      aria-label={`Remove choice ${i + 1}`}
                      onClick={() => setOptions((current) => current.filter((_, j) => j !== i))}
                    >
                      Remove
                    </button>
                  )}
                </div>
              ))}
              {options.length < MAX_OPTIONS && (
                <button type="button" className={styles.secondaryButton} onClick={() => setOptions((current) => [...current, ""])}>
                  Add a choice
                </button>
              )}
            </>
          )}

          <label className={styles.check}>
            <input type="checkbox" checked={allowReplies} onChange={(e) => setAllowReplies(e.target.checked)} />
            <span>
              <span className="type-row-primary">Let people reply </span>
              <span className={`${styles.hint} type-secondary`}>{repliesHint}</span>
            </span>
          </label>

          <span className={`${styles.sectionLabel} type-caption`}>SEND TO ({picked.size} PICKED)</span>
          {loadError && <p className={styles.error}>{loadError}</p>}
          {loading ? (
            <p className={`${styles.hint} type-secondary`}>Loading…</p>
          ) : (
            <div className={styles.pickList}>
              <label className={`${styles.check} ${styles.pickAll}`}>
                <input
                  type="checkbox"
                  checked={allPicked}
                  onChange={(e) => setPicked(e.target.checked ? new Set(recipients.map((u) => u.userId)) : new Set())}
                />
                <span className="type-row-primary">Everyone ({recipients.length})</span>
              </label>
              {recipients.map((u) => (
                <label key={u.userId} className={styles.check}>
                  <input type="checkbox" checked={picked.has(u.userId)} onChange={() => toggle(u.userId)} />
                  <span>
                    <span className="type-row-primary">{u.username}</span>
                    <span className={`${styles.hint} type-secondary`}>
                      {" · "}
                      {u.role === "super_admin" ? "Super admin · " : ""}
                      {TIER_LABELS[u.subscriptionTier] ?? "Free"}
                    </span>
                  </span>
                </label>
              ))}
            </div>
          )}
          <button type="button" className={styles.primaryButton} disabled={!canSend} onClick={send}>
            {sending ? "Sending…" : `Send ${kind}${picked.size ? ` to ${picked.size}` : ""}`}
          </button>
        </div>
      </div>

      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>SENT</span>
        {!loading && sent.length === 0 && <p className={`${styles.hint} type-secondary`}>Nothing sent yet.</p>}
        {sent.map((b) => (
          <SentCard key={b.id} item={b} />
        ))}
      </div>
    </>
  );
}

function SentCard({ item }: { item: api.AdminBroadcastDto }) {
  const [replies, setReplies] = useState<api.AnnouncementReplyDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const totalVotes = item.options.reduce((n, o) => n + o.votes, 0);

  async function showReplies() {
    setError(null);
    try {
      setReplies((await api.adminGetBroadcast(item.id)).replies);
    } catch {
      setError("Couldn't load the replies.");
    }
  }

  return (
    <div className={styles.card}>
      <div className={`${styles.hint} type-secondary`}>
        {item.kind === "poll" ? "Poll" : "Message"} · {when(item.createdAt)} · {item.allowReplies ? "Replies on" : "Replies off"}
      </div>
      <div className={`${styles.text} type-row-primary`}>{item.body}</div>
      <div className={`${styles.hint} type-secondary`}>
        Sent to {item.recipients} · read by {item.read}
        {item.kind === "poll" ? ` · ${totalVotes} ${totalVotes === 1 ? "vote" : "votes"}` : ""}
        {item.allowReplies ? ` · ${item.replyCount} ${item.replyCount === 1 ? "reply" : "replies"}` : ""}
      </div>
      {item.options.map((o) => (
        <div key={o.id} className={styles.tally}>
          <div className={styles.tallyBar} style={{ width: totalVotes ? `${(o.votes / totalVotes) * 100}%` : 0 }} />
          <span className={styles.tallyLabel}>{o.label}</span>
          <span className={styles.tallyLabel}>{o.votes}</span>
        </div>
      ))}
      {error && <p className={styles.error}>{error}</p>}
      {item.allowReplies && item.replyCount > 0 && replies === null && (
        <button type="button" className={styles.secondaryButton} onClick={showReplies}>
          Show replies
        </button>
      )}
      {replies?.map((r) => (
        <div key={r.id} className={styles.replyItem}>
          <span className="type-row-primary">{r.username ?? r.userId}</span>
          <span className={`${styles.hint} type-secondary`}> · {when(r.createdAt)}</span>
          <div>{r.body}</div>
        </div>
      ))}
    </div>
  );
}
