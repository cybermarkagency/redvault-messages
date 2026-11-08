// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useMemo, useRef, useState } from "react";
import { Avatar } from "../components/Avatar.js";
import {
  ArrowDownLeftIcon,
  ArrowUpRightIcon,
  ChatIcon,
  CheckIcon,
  ChevronLeftIcon,
  CloseIcon,
  CopyIcon,
  LinkIcon,
  LockIcon,
  PhoneIcon,
  PlusIcon,
  ShieldCheckIcon,
  TrashIcon,
  VideoIcon,
} from "../components/icons24.js";
import { useDialogFocus } from "../components/useDialogFocus.js";
import { useCallLinks } from "../hooks/useCallLinks.js";
import { useIsDesktopLayout } from "../hooks/useMediaQuery.js";
import { ApiError } from "../lib/api.js";
import { type CallRecord, formatCallDuration, loadCallHistory, onCallHistoryChange } from "../lib/callHistory.js";
import { type OpenedCallLink, parseCallLink } from "../lib/callLinkCrypto.js";
import { parseGroupCallLink } from "../lib/groupCallLink.js";
import { useAppState } from "../state/AppStateContext.js";
import { useCall } from "../state/CallContext.js";
import { useGroupCall } from "../state/GroupCallContext.js";
import { useLive } from "../state/LiveContext.js";
import { type DMConversation, tierAtLeast } from "../types.js";
import styles from "./CallsScreen.module.css";

function useCallHistory(): CallRecord[] {
  const { state } = useAppState();
  const identity = state.identity;
  const [records, setRecords] = useState<CallRecord[]>([]);
  useEffect(() => {
    if (!identity) return;
    const load = () => setRecords(loadCallHistory(identity.userId, identity.privateKey));
    load();
    return onCallHistoryChange(load);
  }, [identity]);
  return records;
}

function isMissed(r: CallRecord): boolean {
  return r.direction === "incoming" && r.outcome !== "answered";
}

function whenLabel(epochMs: number): string {
  const d = new Date(epochMs);
  const today = new Date();
  const yesterday = new Date();
  yesterday.setDate(today.getDate() - 1);
  if (d.toDateString() === today.toDateString()) return d.toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
  if (d.toDateString() === yesterday.toDateString()) return "Yesterday";
  if (today.getTime() - d.getTime() < 6 * 86_400_000) return d.toLocaleDateString(undefined, { weekday: "short" });
  return d.toLocaleDateString(undefined, { month: "short", day: "numeric" });
}

function summary(r: CallRecord): string {
  const kind = r.kind === "video" ? "Video" : "Voice";
  if (isMissed(r)) return `Missed ${kind.toLowerCase()}`;
  if (r.outcome !== "answered") return `${kind} · ${r.outcome === "declined" ? "declined" : "no answer"}`;
  return `${kind} · ${formatCallDuration(r.durationSec)}`;
}

function Direction({ record }: { record: CallRecord }) {
  return record.direction === "outgoing" ? <ArrowUpRightIcon size={14} /> : <ArrowDownLeftIcon size={14} />;
}

function contactFor(conversations: ReturnType<typeof useAppState>["state"]["conversations"], userId: string): DMConversation | undefined {
  return conversations.find((c): c is DMConversation => c.kind === "dm" && c.userId === userId);
}

/** The calls list column: all / missed, the call-link card, recent calls (one row per person). */
export function CallsList() {
  const { state, dispatch } = useAppState();
  const records = useCallHistory();
  const [tab, setTab] = useState<"all" | "missed">("all");
  const [creating, setCreating] = useState(false);
  const [joining, setJoining] = useState(false);
  const selected = state.screen.key === "calls" ? state.screen.userId : undefined;

  const latestByPeer = useMemo(() => {
    const seen = new Set<string>();
    return records.filter((r) => (tab === "missed" ? isMissed(r) : true)).filter((r) => (seen.has(r.peerUserId) ? false : (seen.add(r.peerUserId), true)));
  }, [records, tab]);
  const missedCount = records.filter(isMissed).length;

  return (
    <div className={styles.list}>
      <div className={styles.listHeader}>
        <h1 className={`${styles.title} type-display`}>Calls</h1>
        <button type="button" className={styles.primaryRound} aria-label="Create a call link" onClick={() => setCreating(true)}>
          <PlusIcon />
        </button>
      </div>
      <div className={styles.segmented} role="tablist" aria-label="Which calls">
        <button type="button" role="tab" aria-selected={tab === "all"} className={tab === "all" ? styles.segOn : ""} onClick={() => setTab("all")}>
          All
        </button>
        <button type="button" role="tab" aria-selected={tab === "missed"} className={tab === "missed" ? styles.segOn : ""} onClick={() => setTab("missed")}>
          {missedCount > 0 ? `Missed · ${missedCount}` : "Missed"}
        </button>
      </div>
      <button type="button" className={styles.linkCard} onClick={() => setCreating(true)}>
        <span className={styles.linkCardIcon}>
          <LinkIcon />
        </span>
        <span>
          <span className={styles.linkCardTitle}>Create a call link</span>
          <span className={styles.linkCardDetail}>Anyone with the link can ask to join</span>
        </span>
      </button>
      <button type="button" className={`${styles.linkCard} ${styles.linkCardQuiet}`} onClick={() => setJoining(true)}>
        <span className={`${styles.linkCardIcon} ${styles.linkCardIconQuiet}`}>
          <LinkIcon />
        </span>
        <span>
          <span className={styles.linkCardTitle}>Join with a link</span>
          <span className={styles.linkCardDetail}>Paste a call link; the host lets you in</span>
        </span>
      </button>
      <div className={`${styles.sectionLabel} type-caption`}>RECENT</div>
      <div className={styles.rows}>
        {latestByPeer.length === 0 ? (
          <p className={styles.empty}>
            {tab === "missed" ? "No missed calls." : "No calls yet. Your call history stays on this device, encrypted, and is never sent to the server."}
          </p>
        ) : (
          latestByPeer.map((r) => {
            const contact = contactFor(state.conversations, r.peerUserId);
            return (
              <button
                key={r.id}
                type="button"
                className={`${styles.row} ${selected === r.peerUserId ? styles.rowOn : ""}`}
                onClick={() => dispatch({ type: "OPEN_CALLS", userId: r.peerUserId })}
              >
                <Avatar avatarUrl={contact?.avatarUrl} seed={contact?.handle ?? r.peerHandle} size={42} />
                <span className={styles.rowText}>
                  <span className={`${styles.rowName} ${isMissed(r) ? styles.missed : ""}`}>{contact?.handle ?? r.peerHandle}</span>
                  <span className={`${styles.rowDetail} ${isMissed(r) ? styles.missed : ""}`}>
                    <Direction record={r} /> {summary(r)}
                    {r.linkName ? ` · ${r.linkName}` : ""}
                  </span>
                </span>
                <span className={styles.rowTime}>{whenLabel(r.startedAt)}</span>
              </button>
            );
          })
        )}
      </div>
      {creating && <CreateLinkDialog onClose={() => setCreating(false)} />}
      {joining && <JoinWithLinkDialog onClose={() => setJoining(false)} />}
    </div>
  );
}

/**
 * Paste a call link to join. A link someone sent you opens the same screen as
 * opening it from outside the app (the host lets you in); the link a group
 * call posts in its chat takes you into that call, if you're in the group.
 */
function JoinWithLinkDialog({ onClose }: { onClose: () => void }) {
  const { dispatch } = useAppState();
  const groupCalls = useGroupCall();
  const [text, setText] = useState("");
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  useDialogFocus(ref, onClose);

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    if (busy) return;
    const link = parseCallLink(text);
    if (link) {
      dispatch({ type: "OPEN_JOIN_CALL", linkId: link.linkId, secret: link.secret });
      onClose();
      return;
    }
    const groupCallId = parseGroupCallLink(text);
    if (groupCallId) {
      if (!groupCalls.available) {
        setError("Group calls aren't available on this server or in this browser.");
        return;
      }
      setBusy(true);
      try {
        await groupCalls.joinByLink(groupCallId);
        onClose();
      } catch (err) {
        setError(err instanceof Error ? err.message : "Couldn't join the call. Try again.");
        setBusy(false);
      }
      return;
    }
    setError("That doesn't look like a call link. Paste the whole link, including the part after #.");
  }

  return (
    <div className={styles.backdrop} onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div ref={ref} className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="join-link-title">
        <div className={styles.dialogTop}>
          <h2 id="join-link-title" className="type-display">
            Join with a link
          </h2>
          <button type="button" className={styles.iconSquare} aria-label="Close" onClick={onClose}>
            <CloseIcon />
          </button>
        </div>
        <p className={styles.lead}>
          Paste a call link. If someone sent it to you, they'll be asked to let you in. If it was posted in one of your group chats, you join
          that group's call straight away. Either way it works on any plan.
        </p>
        <form onSubmit={submit} className={styles.form}>
          <label className={styles.field}>
            <span>Call link</span>
            <input
              value={text}
              onChange={(e) => {
                setText(e.target.value);
                setError(null);
              }}
              placeholder="https://…/?join=…#…"
              autoComplete="off"
              autoCapitalize="off"
              spellCheck={false}
              aria-invalid={error !== null}
              required
            />
          </label>
          <p className={styles.note}>
            <LockIcon size={13} /> The part after # is the link's secret. It stays on your device and is never sent to the server.
          </p>
          {error && (
            <p className={styles.error} role="alert">
              {error}
            </p>
          )}
          <button type="submit" className={styles.primary} disabled={!text.trim() || busy}>
            {busy ? "Joining…" : "Continue"}
          </button>
        </form>
      </div>
    </div>
  );
}

function CreateLinkDialog({ onClose }: { onClose: () => void }) {
  const { state, dispatch } = useAppState();
  const { create } = useCallLinks();
  const tier = state.identity?.subscriptionTier ?? "free";
  const [name, setName] = useState("");
  const [kind, setKind] = useState<"voice" | "video">(tierAtLeast(tier, "premium") ? "video" : "voice");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [created, setCreated] = useState<OpenedCallLink | null>(null);
  const [copied, setCopied] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  useDialogFocus(ref, onClose);

  async function submit(e: React.FormEvent) {
    e.preventDefault();
    if (!name.trim() || busy) return;
    setBusy(true);
    setError(null);
    try {
      setCreated(await create(name.trim().slice(0, 80), kind));
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Couldn't create the link. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.backdrop} onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div ref={ref} className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="new-link-title">
        <div className={styles.dialogTop}>
          <h2 id="new-link-title" className="type-display">
            {created ? "Your call link" : "Create a call link"}
          </h2>
          <button type="button" className={styles.iconSquare} aria-label="Close" onClick={onClose}>
            <CloseIcon />
          </button>
        </div>
        {!tierAtLeast(tier, "pro") ? (
          <>
            <p className={styles.lead}>Call links are included in Pro (voice) and Premium (voice and video), because you host the calls.</p>
            <button type="button" className={styles.primary} onClick={() => dispatch({ type: "OPEN_SETTINGS", section: "account" })}>
              See plans
            </button>
          </>
        ) : created ? (
          <>
            <p className={styles.lead}>
              Share it with the person you want to call. When they open it, you'll be asked to let them in. The part after # is the link's
              secret: it never reaches our server.
            </p>
            <div className={`${styles.urlBox} type-mono`}>{created.url}</div>
            <button
              type="button"
              className={styles.primary}
              onClick={async () => {
                try {
                  await navigator.clipboard.writeText(created.url);
                  setCopied(true);
                } catch {
                  setCopied(false);
                }
              }}
            >
              {copied ? <CheckIcon size={18} /> : <CopyIcon size={18} />} {copied ? "Copied" : "Copy link"}
            </button>
          </>
        ) : (
          <form onSubmit={submit} className={styles.form}>
            <label className={styles.field}>
              <span>Name</span>
              <input value={name} onChange={(e) => setName(e.target.value)} placeholder="e.g. Weekly design sync" maxLength={80} required />
            </label>
            <div className={styles.segmented} role="radiogroup" aria-label="Kind of call">
              <button type="button" role="radio" aria-checked={kind === "voice"} className={kind === "voice" ? styles.segOn : ""} onClick={() => setKind("voice")}>
                <PhoneIcon size={16} /> Voice
              </button>
              <button
                type="button"
                role="radio"
                aria-checked={kind === "video"}
                className={kind === "video" ? styles.segOn : ""}
                onClick={() => setKind("video")}
                disabled={!tierAtLeast(tier, "premium")}
                title={tierAtLeast(tier, "premium") ? undefined : "Video links need Premium"}
              >
                <VideoIcon size={16} /> Video
              </button>
            </div>
            <p className={styles.note}>
              <LockIcon size={13} /> The name is encrypted with the link's secret: only people with the link can read it.
            </p>
            {error && <p className={styles.error}>{error}</p>}
            <button type="submit" className={styles.primary} disabled={!name.trim() || busy}>
              {busy ? "Creating…" : "Create link"}
            </button>
          </form>
        )}
      </div>
    </div>
  );
}

function CallLinksPanel() {
  const { links, loaded, error, remove } = useCallLinks();
  const [creating, setCreating] = useState(false);
  const [copiedId, setCopiedId] = useState<string | null>(null);

  async function copy(link: OpenedCallLink) {
    try {
      await navigator.clipboard.writeText(link.url);
      setCopiedId(link.id);
      setTimeout(() => setCopiedId((id) => (id === link.id ? null : id)), 2000);
    } catch {
      // Clipboard blocked; the URL is on screen.
    }
  }

  return (
    <section className={styles.card}>
      <div className={styles.cardHead}>
        <span className="type-caption">YOUR CALL LINKS</span>
        <button type="button" className={styles.textLink} onClick={() => setCreating(true)}>
          New link
        </button>
      </div>
      {error && <p className={styles.error}>{error}</p>}
      {loaded && links.length === 0 && !error && <p className={styles.empty}>No call links yet.</p>}
      {links.map((link) => (
        <div key={link.id} className={styles.linkRow}>
          <span className={styles.linkText}>
            <span className={styles.linkName}>
              {link.kind === "video" ? <VideoIcon size={15} /> : <PhoneIcon size={15} />} {link.name}
            </span>
            <span className={`${styles.linkUrl} type-mono`} title={link.url}>
              {link.url.replace(/^https?:\/\//, "").replace(/#.*/, "#…")}
            </span>
          </span>
          <button type="button" className={styles.iconSquare} aria-label={`Copy ${link.name} link`} onClick={() => copy(link)}>
            {copiedId === link.id ? <CheckIcon size={18} /> : <CopyIcon size={18} />}
          </button>
          <button
            type="button"
            className={styles.iconSquare}
            aria-label={`Delete ${link.name}`}
            onClick={() => window.confirm(`Delete "${link.name}"? The link stops working for everyone.`) && remove(link.id).catch(() => {})}
          >
            <TrashIcon size={18} />
          </button>
        </div>
      ))}
      <p className={styles.note}>
        <LockIcon size={13} /> People who open a link wait until you let them in. Link calls are end-to-end encrypted too, and you need
        this app open to answer.
      </p>
      {creating && <CreateLinkDialog onClose={() => setCreating(false)} />}
    </section>
  );
}

/** The calls main area: history with one person (and a way to call them), plus your call links. */
export function CallsDetail() {
  const { state, dispatch } = useAppState();
  const isDesktop = useIsDesktopLayout();
  const calls = useCall();
  const live = useLive();
  const records = useCallHistory();
  const userId = state.screen.key === "calls" ? state.screen.userId : undefined;
  const history = userId ? records.filter((r) => r.peerUserId === userId) : [];
  const contact = userId ? contactFor(state.conversations, userId) : undefined;
  const handle = contact?.handle ?? history[0]?.peerHandle ?? "";
  const online = !!userId && live.online.has(userId);

  return (
    <div className={styles.detail}>
      {!isDesktop && (
        <div className={styles.mobileTop}>
          <button type="button" className={styles.iconSquare} aria-label="Back" onClick={() => dispatch({ type: "OPEN_CALLS" })}>
            <ChevronLeftIcon />
          </button>
        </div>
      )}
      {userId && handle && (
        <div className={styles.person}>
          <Avatar avatarUrl={contact?.avatarUrl} seed={handle} size={104} online={online} />
          <div className={styles.personText}>
            <h2 className={`${styles.personName} type-display`}>{handle}</h2>
            {contact && (
              <span className={contact.verified && !contact.keyChanged ? styles.ok : styles.warn}>
                {contact.verified && !contact.keyChanged ? <ShieldCheckIcon size={16} /> : null}
                {contact.keyChanged ? "Safety number changed" : contact.verified ? "Safety number verified" : "Not verified"}
                {online ? " · online" : ""}
              </span>
            )}
          </div>
          {contact && (
            <div className={styles.personActions}>
              {calls.callsAvailable && (
                <>
                  <button
                    type="button"
                    className={styles.primary}
                    onClick={() => calls.startCall(contact, "voice")}
                  >
                    <PhoneIcon size={18} /> Voice call
                  </button>
                  <button
                    type="button"
                    className={styles.secondary}
                    onClick={() => calls.startCall(contact, "video")}
                  >
                    <VideoIcon size={18} /> Video call
                  </button>
                </>
              )}
              <button type="button" className={styles.iconSquare} aria-label={`Message ${handle}`} onClick={() => dispatch({ type: "OPEN_THREAD", conversationId: contact.id })}>
                <ChatIcon />
              </button>
            </div>
          )}
        </div>
      )}
      <div className={styles.cards}>
        {userId && (
          <section className={styles.card}>
            <div className={styles.cardHead}>
              <span className="type-caption">CALL HISTORY WITH {handle.toUpperCase()}</span>
            </div>
            {history.length === 0 && <p className={styles.empty}>No calls with them on this device.</p>}
            {history.map((r) => (
              <div key={r.id} className={styles.historyRow}>
                <span className={`${styles.historyWhat} ${isMissed(r) ? styles.missed : ""}`}>
                  <Direction record={r} />
                  {isMissed(r) ? `Missed ${r.kind} call` : `${r.direction === "outgoing" ? "Outgoing" : "Incoming"} ${r.kind} call`}
                </span>
                <span className={styles.historyDuration}>{r.outcome === "answered" ? formatCallDuration(r.durationSec) : "—"}</span>
                <span className={styles.historyWhen}>
                  {new Date(r.startedAt).toLocaleString(undefined, { month: "short", day: "numeric", hour: "numeric", minute: "2-digit" })}
                </span>
              </div>
            ))}
          </section>
        )}
        <CallLinksPanel />
      </div>
    </div>
  );
}
