// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useState } from "react";
import { toggleIn, useLocalPrefs } from "../lib/localPrefs.js";
import { useAppState } from "../state/AppStateContext.js";
import { useLive } from "../state/LiveContext.js";
import { updateDisappearing } from "../state/actions.js";
import { type Conversation, DURATION_LABELS, type DMConversation, type GroupConversation, verifiedCount } from "../types.js";
import { Avatar } from "./Avatar.js";
import styles from "./ChatDetails.module.css";
import { DurationSelect } from "./DurationSelect.js";
import { GroupGlyph } from "./GroupGlyph.js";
import { BellIcon, BellOffIcon, ChevronDownIcon, ShieldCheckIcon, ShieldIcon, StarIcon, TimerIcon, UsersIcon } from "./icons24.js";
import { SharedMedia, sharedFiles } from "./SharedMedia.js";

/** Groups both of you are in, for "Groups in common". */
export function groupsInCommon(conversations: Conversation[], userId: string | undefined): GroupConversation[] {
  if (!userId) return [];
  return conversations.filter((c): c is GroupConversation => c.kind === "group" && c.members.some((m) => m.userId === userId));
}

/** The safety-number card: verified (with how), changed, or not yet. */
export function SafetyCard({ contact, onVerify, compact }: { contact: DMConversation; onVerify: () => void; compact?: boolean }) {
  const tone = contact.keyChanged ? styles.cardWarn : contact.verified ? styles.cardOk : styles.cardNeutral;
  const title = contact.keyChanged ? "Safety number changed" : contact.verified ? "Safety number verified" : "Not verified yet";
  const detail = contact.keyChanged
    ? "Verify again before trusting this chat"
    : contact.verified
      ? "Checked on this device"
      : "Compare safety numbers to be sure it's them";
  return (
    <button type="button" className={`${styles.card} ${tone}`} onClick={onVerify}>
      <span className={styles.cardIcon}>{contact.verified && !contact.keyChanged ? <ShieldCheckIcon /> : <ShieldIcon />}</span>
      <span className={styles.cardText}>
        <span className={styles.cardTitle}>{title}</span>
        {!compact && <span className={styles.cardDetail}>{detail}</span>}
      </span>
    </button>
  );
}

function Row({ icon, label, value, onClick, expanded }: { icon: React.ReactNode; label: string; value?: React.ReactNode; onClick?: () => void; expanded?: boolean }) {
  const content = (
    <>
      <span className={styles.rowIcon}>{icon}</span>
      <span className={styles.rowLabel}>{label}</span>
      {value !== undefined && <span className={styles.rowValue}>{value}</span>}
      {expanded !== undefined && <ChevronDownIcon size={16} className={expanded ? styles.chevronOpen : styles.chevron} />}
    </>
  );
  return onClick ? (
    <button type="button" className={styles.row} onClick={onClick} aria-expanded={expanded}>
      {content}
    </button>
  ) : (
    <div className={styles.row}>{content}</div>
  );
}

/** The chat details panel (desktop, 320px) for a 1:1 chat or a group. */
export function ChatDetails({ conversation }: { conversation: Conversation }) {
  const { state, dispatch, contactsById } = useAppState();
  const live = useLive();
  const [prefs, setPrefs] = useLocalPrefs(state.identity?.userId);
  const [editingTimer, setEditingTimer] = useState(false);
  const [showAllMedia, setShowAllMedia] = useState(false);
  const files = sharedFiles(conversation.messages);

  const timer = (
    <>
      <Row
        icon={<TimerIcon />}
        label="Disappearing messages"
        value={DURATION_LABELS[conversation.disappearing]}
        onClick={() => setEditingTimer((v) => !v)}
        expanded={editingTimer}
      />
      {editingTimer && (
        <div className={styles.timerPicker}>
          <DurationSelect
            aria-label="Disappearing messages in this chat"
            value={conversation.disappearing}
            onChange={(d) => updateDisappearing(dispatch, conversation.id, d, conversation.disappearing).catch(() => {})}
          />
        </div>
      )}
    </>
  );

  const media = (
    <section className={styles.mediaSection}>
      <div className={styles.sectionHead}>
        <span className="type-caption">SHARED MEDIA</span>
        {files.length > 6 && (
          <button type="button" className={styles.link} onClick={() => setShowAllMedia((v) => !v)}>
            {showAllMedia ? "Show less" : `See all ${files.length}`}
          </button>
        )}
      </div>
      <SharedMedia files={files} limit={showAllMedia ? undefined : 6} />
    </section>
  );

  if (conversation.kind === "group") {
    const others = conversation.members.filter((m) => !m.isSelf);
    return (
      <aside className={styles.panel} aria-label="Group details">
        <div className={styles.identity}>
          <GroupGlyph label={conversation.label} memberSeeds={others.map((m) => m.handle)} size={84} />
          <div className={`${styles.name} type-display`}>{conversation.label}</div>
          <div className={`${styles.handle} type-mono`}>
            {others.length + 1} members · {verifiedCount(conversation, contactsById)} of {others.length} verified
          </div>
        </div>
        <div className={styles.group}>
          {timer}
          <Row icon={<UsersIcon />} label="Members and settings" onClick={() => dispatch({ type: "OPEN_GROUP_INFO", conversationId: conversation.id })} value="" />
        </div>
        {media}
      </aside>
    );
  }

  const userId = conversation.userId;
  const muted = !!userId && prefs.muted.includes(userId);
  const favorite = !!userId && prefs.favorites.includes(userId);
  const common = groupsInCommon(state.conversations, userId);
  const online = !!userId && live.online.has(userId);

  return (
    <aside className={styles.panel} aria-label="Chat details">
      <div className={styles.identity}>
        <Avatar avatarUrl={conversation.avatarUrl} seed={conversation.handle} size={84} online={online} />
        <div className={`${styles.name} type-display`}>{conversation.handle}</div>
        <button
          type="button"
          className={`${styles.handle} ${styles.handleButton} type-mono`}
          onClick={() => userId && dispatch({ type: "OPEN_PROFILE", userId, origin: { screen: "thread", conversationId: conversation.id } })}
        >
          @{conversation.handle}
          {online ? " · online" : ""}
        </button>
      </div>
      <SafetyCard contact={conversation} onVerify={() => dispatch({ type: "OPEN_VERIFY_FROM_THREAD", contactId: conversation.id })} />
      <div className={styles.group}>
        {timer}
        <Row
          icon={muted ? <BellOffIcon /> : <BellIcon />}
          label="Notifications"
          value={muted ? "Muted" : "On"}
          onClick={() => userId && setPrefs({ muted: toggleIn(prefs.muted, userId) })}
        />
        <Row
          icon={<StarIcon fill={favorite ? "currentColor" : "none"} />}
          label="Favorite"
          value={favorite ? "Yes" : "No"}
          onClick={() => userId && setPrefs({ favorites: toggleIn(prefs.favorites, userId) })}
        />
        <Row icon={<UsersIcon />} label="Groups in common" value={String(common.length)} />
      </div>
      {media}
    </aside>
  );
}
