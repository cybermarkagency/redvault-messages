// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import QRCode from "qrcode";
import { useEffect, useMemo, useRef, useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { groupsInCommon } from "../components/ChatDetails.js";
import { DurationSelect } from "../components/DurationSelect.js";
import {
  BanIcon,
  BellIcon,
  BellOffIcon,
  ChatIcon,
  CheckIcon,
  ChevronLeftIcon,
  CloseIcon,
  CopyIcon,
  FlagIcon,
  ImageIcon,
  PhoneIcon,
  QrIcon,
  SearchIcon,
  ShareIcon,
  ShieldCheckIcon,
  ShieldIcon,
  StarIcon,
  TimerIcon,
  UserPlusIcon,
  UsersIcon,
  VideoIcon,
} from "../components/icons24.js";
import { SharedMedia, sharedFiles } from "../components/SharedMedia.js";
import { useDialogFocus } from "../components/useDialogFocus.js";
import { useIsDesktopLayout } from "../hooks/useMediaQuery.js";
import * as api from "../lib/api.js";
import { toggleIn, useLocalPrefs } from "../lib/localPrefs.js";
import { submitReport, updateDisappearing } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import { useCall } from "../state/CallContext.js";
import { useLive } from "../state/LiveContext.js";
import { type DMConversation, DURATION_LABELS } from "../types.js";
import styles from "./ContactsScreen.module.css";

function useContacts(): DMConversation[] {
  const { state } = useAppState();
  return useMemo(
    () =>
      state.conversations
        .filter((c): c is DMConversation => c.kind === "dm" && !!c.userId)
        .sort((a, b) => a.handle.localeCompare(b.handle, undefined, { sensitivity: "base" })),
    [state.conversations]
  );
}

function statusLine(contact: DMConversation, online: boolean): { text: string; tone: "ok" | "warn" | "mono" } {
  if (contact.keyChanged) return { text: "Safety number changed", tone: "warn" };
  if (contact.verified) return { text: online ? "Verified · online" : "Verified", tone: "ok" };
  return { text: `@${contact.handle}${online ? " · online" : ""}`, tone: "mono" };
}

/** The contacts column: search, favorites, A–Z, and your contact code. */
export function ContactsList() {
  const { state, dispatch } = useAppState();
  const live = useLive();
  const contacts = useContacts();
  const [prefs] = useLocalPrefs(state.identity?.userId);
  const [query, setQuery] = useState("");
  const [sharing, setSharing] = useState(false);
  const selected = state.screen.key === "contacts" ? state.screen.userId : undefined;
  const q = query.trim().toLowerCase();
  const shown = contacts.filter((c) => !q || c.handle.toLowerCase().includes(q));
  const favorites = prefs.favorites.map((id) => contacts.find((c) => c.userId === id)).filter((c): c is DMConversation => !!c);

  const sections = new Map<string, DMConversation[]>();
  for (const c of shown) {
    const letter = /[a-z]/i.test(c.handle[0] ?? "") ? c.handle[0].toUpperCase() : "#";
    sections.set(letter, [...(sections.get(letter) ?? []), c]);
  }

  return (
    <div className={styles.list}>
      <div className={styles.header}>
        <h1 className={`${styles.title} type-display`}>Contacts</h1>
        <button type="button" className={styles.round} aria-label="Share your contact code" onClick={() => setSharing(true)}>
          <QrIcon />
        </button>
        <button type="button" className={`${styles.round} ${styles.roundPrimary}`} aria-label="Add a contact" onClick={() => dispatch({ type: "OPEN_NEW_CONTACT" })}>
          <UserPlusIcon />
        </button>
      </div>
      <label className={styles.search}>
        <SearchIcon size={18} />
        <input type="search" placeholder="Search name or username" value={query} onChange={(e) => setQuery(e.target.value)} aria-label="Search contacts" />
      </label>

      <div className={styles.scroll}>
        {favorites.length > 0 && !q && (
          <div className={styles.favorites} aria-label="Favorites">
            {favorites.map((c) => (
              <button key={c.id} type="button" className={styles.favorite} onClick={() => dispatch({ type: "OPEN_CONTACTS", userId: c.userId })}>
                <Avatar avatarUrl={c.avatarUrl} seed={c.handle} size={56} ring={selected === c.userId} online={live.online.has(c.userId!)} />
                <span>{c.handle.split(/[\s.·-]/)[0]}</span>
              </button>
            ))}
          </div>
        )}
        {contacts.length === 0 ? (
          <p className={styles.empty}>No contacts yet. Add someone by their username, or share your contact code.</p>
        ) : shown.length === 0 ? (
          <p className={styles.empty}>No contacts match.</p>
        ) : (
          [...sections].map(([letter, list]) => (
            <div key={letter}>
              <div className={styles.letter}>{letter}</div>
              {list.map((c) => {
                const online = live.online.has(c.userId!);
                const status = statusLine(c, online);
                return (
                  <button
                    key={c.id}
                    type="button"
                    className={`${styles.row} ${selected === c.userId ? styles.rowOn : ""}`}
                    onClick={() => dispatch({ type: "OPEN_CONTACTS", userId: c.userId })}
                  >
                    <Avatar avatarUrl={c.avatarUrl} seed={c.handle} size={42} online={online} />
                    <span className={styles.rowText}>
                      <span className={styles.rowName}>{c.handle}</span>
                      <span className={status.tone === "ok" ? styles.ok : status.tone === "warn" ? styles.warn : styles.mono}>{status.text}</span>
                    </span>
                  </button>
                );
              })}
            </div>
          ))
        )}
      </div>

      <button type="button" className={styles.shareCard} onClick={() => setSharing(true)}>
        <span className={styles.shareIcon}>
          <ShareIcon />
        </span>
        <span>
          <span className={styles.shareTitle}>Share your contact code</span>
          <span className={styles.shareDetail}>No phone number needed to connect</span>
        </span>
      </button>
      {sharing && <ContactCodeDialog onClose={() => setSharing(false)} />}
    </div>
  );
}

function ContactCodeDialog({ onClose }: { onClose: () => void }) {
  const { state } = useAppState();
  const handle = state.identity?.handle ?? "";
  const [qr, setQr] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  useDialogFocus(ref, onClose);
  useEffect(() => {
    QRCode.toDataURL(handle, { margin: 1, width: 240, color: { dark: "#141413", light: "#ECEAE3" } })
      .then(setQr)
      .catch(() => {});
  }, [handle]);
  return (
    <div className={styles.backdrop} onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
      <div ref={ref} className={styles.dialog} role="dialog" aria-modal="true" aria-labelledby="contact-code-title">
        <div className={styles.dialogTop}>
          <h2 id="contact-code-title" className="type-display">
            Your contact code
          </h2>
          <button type="button" className={styles.square} aria-label="Close" onClick={onClose}>
            <CloseIcon />
          </button>
        </div>
        <p className={styles.lead}>People add you by your username; no phone number or email is ever involved. Show this code, or send your username.</p>
        {qr && <img src={qr} alt={`QR code for your username, ${handle}`} className={styles.qr} />}
        <div className={`${styles.handleBox} type-mono`}>{handle}</div>
        <button
          type="button"
          className={styles.primary}
          onClick={async () => {
            try {
              await navigator.clipboard.writeText(handle);
              setCopied(true);
            } catch {
              setCopied(false);
            }
          }}
        >
          {copied ? <CheckIcon size={18} /> : <CopyIcon size={18} />} {copied ? "Copied" : "Copy username"}
        </button>
      </div>
    </div>
  );
}

/** One contact's page: actions, safety number, settings, shared media, block and report. */
export function ContactDetail() {
  const { state, dispatch } = useAppState();
  const isDesktop = useIsDesktopLayout();
  const live = useLive();
  const calls = useCall();
  const contacts = useContacts();
  const [prefs, setPrefs] = useLocalPrefs(state.identity?.userId);
  const userId = state.screen.key === "contacts" ? state.screen.userId : undefined;
  const contact = contacts.find((c) => c.userId === userId);
  const [editingTimer, setEditingTimer] = useState(false);
  const [blocked, setBlocked] = useState<boolean | null>(null);
  const [reporting, setReporting] = useState(false);
  const [reason, setReason] = useState("");
  const [notice, setNotice] = useState<string | null>(null);

  useEffect(() => {
    if (!userId) return;
    let cancelled = false;
    setBlocked(null);
    Promise.resolve()
      .then(() => api.listBlocked())
      .then((list) => !cancelled && setBlocked((list ?? []).some((b) => b.userId === userId)))
      .catch(() => !cancelled && setBlocked(false));
    return () => {
      cancelled = true;
    };
  }, [userId]);

  if (!contact || !userId) {
    return (
      <div className={styles.emptyDetail}>
        <p className="type-secondary">{contacts.length ? "Select a contact" : "Your contacts will show up here."}</p>
      </div>
    );
  }

  const online = live.online.has(userId);
  const muted = prefs.muted.includes(userId);
  const favorite = prefs.favorites.includes(userId);
  const files = sharedFiles(contact.messages);
  const common = groupsInCommon(state.conversations, userId);
  const firstName = contact.handle.split(/[\s.·-]/)[0];

  async function toggleBlock() {
    if (blocked === null) return;
    if (!blocked && !window.confirm(`Block ${contact!.handle}? Neither of you will be able to message or call the other until you unblock them.`)) return;
    try {
      if (blocked) await api.unblockUser(userId!);
      else await api.blockUser(userId!);
      setBlocked(!blocked);
    } catch {
      setNotice("Couldn't change that. Try again.");
    }
  }

  async function sendReport() {
    if (!reason.trim()) return;
    try {
      await submitReport(userId!, reason.trim());
      setReporting(false);
      setReason("");
      setNotice("Report sent. Thank you.");
    } catch {
      setNotice("Couldn't send the report. Try again.");
    }
  }

  return (
    <div className={styles.detail}>
      {!isDesktop && (
        <button type="button" className={styles.square} aria-label="Back" onClick={() => dispatch({ type: "OPEN_CONTACTS" })}>
          <ChevronLeftIcon />
        </button>
      )}
      <div className={styles.person}>
        <Avatar avatarUrl={contact.avatarUrl} seed={contact.handle} size={112} online={online} />
        <div className={styles.personText}>
          <h2 className={`${styles.personName} type-display`}>{contact.handle}</h2>
          <span className={`${styles.mono} ${styles.personHandle}`}>
            @{contact.handle}
            {online ? " · online" : ""}
          </span>
        </div>
        <button
          type="button"
          className={styles.secondary}
          aria-pressed={favorite}
          onClick={() => setPrefs({ favorites: toggleIn(prefs.favorites, userId) })}
        >
          <StarIcon size={18} fill={favorite ? "currentColor" : "none"} /> {favorite ? "Favorite" : "Add to favorites"}
        </button>
      </div>

      <div className={styles.actions}>
        <button type="button" className={styles.primary} onClick={() => dispatch({ type: "OPEN_THREAD", conversationId: contact.id })}>
          <ChatIcon size={18} /> Message
        </button>
        {calls.callsAvailable && (
          <>
            <button
              type="button"
              className={styles.secondary}
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
        <button type="button" className={styles.secondary} aria-pressed={muted} onClick={() => setPrefs({ muted: toggleIn(prefs.muted, userId) })}>
          {muted ? <BellOffIcon size={18} /> : <BellIcon size={18} />} {muted ? "Unmute" : "Mute"}
        </button>
      </div>

      <div className={styles.grid}>
        <section className={`${styles.safety} ${contact.keyChanged ? styles.safetyWarn : contact.verified ? styles.safetyOk : styles.safetyNeutral}`}>
          <div className={styles.safetyHead}>
            <span className={styles.safetyIcon}>{contact.verified && !contact.keyChanged ? <ShieldCheckIcon /> : <ShieldIcon />}</span>
            <span>
              <span className={styles.safetyTitle}>
                {contact.keyChanged ? "Safety number changed" : contact.verified ? "Safety number verified" : "Not verified yet"}
              </span>
              <span className={styles.safetyDetail}>
                {contact.keyChanged ? "Verify again before trusting chats or calls" : contact.verified ? "Checked on this device" : "Compare in person or over a call you trust"}
              </span>
            </span>
          </div>
          {contact.safetyNumber && (
            <div className={styles.digits} role="img" aria-label={`Safety number ${contact.safetyNumber.join(" ")}`}>
              {contact.safetyNumber.map((g, i) => (
                <span key={i}>{g}</span>
              ))}
            </div>
          )}
          <button type="button" className={styles.textLink} onClick={() => dispatch({ type: "OPEN_VERIFY_FROM_THREAD", contactId: contact.id })}>
            Compare safety number
          </button>
        </section>

        <section className={styles.rows}>
          <button type="button" className={styles.settingRow} onClick={() => setEditingTimer((v) => !v)} aria-expanded={editingTimer}>
            <TimerIcon /> <span>Disappearing messages</span>
            <span className={styles.value}>{DURATION_LABELS[contact.disappearing]}</span>
          </button>
          {editingTimer && (
            <div className={styles.timer}>
              <DurationSelect
                aria-label={`Disappearing messages with ${contact.handle}`}
                value={contact.disappearing}
                onChange={(d) => updateDisappearing(dispatch, contact.id, d, contact.disappearing).catch(() => {})}
              />
            </div>
          )}
          <button type="button" className={styles.settingRow} onClick={() => setPrefs({ muted: toggleIn(prefs.muted, userId) })}>
            {muted ? <BellOffIcon /> : <BellIcon />} <span>Notifications</span>
            <span className={styles.value}>{muted ? "Muted" : "On"}</span>
          </button>
          <div className={styles.settingRow}>
            <ImageIcon /> <span>Shared media and files</span>
            <span className={styles.value}>{files.length}</span>
          </div>
          <div className={styles.settingRow}>
            <UsersIcon /> <span>Groups in common</span>
            <span className={styles.value}>{common.length}</span>
          </div>
        </section>
      </div>

      <section className={styles.mediaSection}>
        <div className="type-caption">SHARED MEDIA</div>
        <SharedMedia files={files} columns={isDesktop ? 6 : 3} limit={12} />
      </section>

      <div className={styles.dangerRow}>
        <button type="button" className={styles.dangerButton} onClick={toggleBlock} disabled={blocked === null}>
          <BanIcon size={18} /> {blocked ? `Unblock ${firstName}` : `Block ${firstName}`}
        </button>
        <button type="button" className={styles.dangerButton} onClick={() => setReporting((v) => !v)}>
          <FlagIcon size={18} /> Report
        </button>
      </div>
      {reporting && (
        <div className={styles.report}>
          <textarea value={reason} onChange={(e) => setReason(e.target.value)} placeholder="What happened? (Only the moderators see this.)" maxLength={1000} />
          <button type="button" className={styles.primary} onClick={sendReport} disabled={!reason.trim()}>
            Send report
          </button>
        </div>
      )}
      {notice && <p className={styles.notice}>{notice}</p>}
    </div>
  );
}
