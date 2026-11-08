// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useMemo, useRef, useState } from "react";
import { useLocalPrefs } from "../lib/localPrefs.js";
import { useAppState } from "../state/AppStateContext.js";
import { useLive } from "../state/LiveContext.js";
import { toMessage, useV3Latest } from "../state/V3Chat.js";
import { ConversationRow } from "./ConversationRow.js";
import { StoryTray } from "./stories/StoryTray.js";
import { LockIcon, PlusIcon, SearchIcon, VideoIcon } from "./icons24.js";
import styles from "./Sidebar.module.css";

type Filter = "all" | "unread" | "groups" | "verified";

/** The chat list column: search, filters, friend requests, conversations. */
export function Sidebar({ showLock }: { showLock?: boolean }) {
  const { state, dispatch, contactsById } = useAppState();
  const live = useLive();
  const v3Latest = useV3Latest();
  const [prefs] = useLocalPrefs(state.identity?.userId);
  const activeConversationId = state.screen.key === "thread" ? state.screen.conversationId : null;
  const [query, setQuery] = useState("");
  const [filter, setFilter] = useState<Filter>("all");
  const searchRef = useRef<HTMLInputElement>(null);

  // Ctrl/⌘ K jumps to search, like the design's hint says.
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        searchRef.current?.focus();
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  const unreadCount = state.conversations.filter((c) => c.unread > 0).length;
  const shown = useMemo(() => {
    const q = query.trim().toLowerCase();
    return state.conversations.filter((c) => {
      const name = c.kind === "group" ? c.label : c.handle;
      if (q && !name.toLowerCase().includes(q)) return false;
      if (filter === "unread") return c.unread > 0;
      if (filter === "groups") return c.kind === "group";
      if (filter === "verified") return c.kind === "dm" && c.verified;
      return true;
    });
  }, [state.conversations, query, filter]);

  const chip = (value: Filter, label: string) => (
    <button
      type="button"
      className={`${styles.chip} ${filter === value ? styles.chipActive : ""}`}
      aria-pressed={filter === value}
      onClick={() => setFilter(value)}
    >
      {label}
    </button>
  );

  return (
    <div className={styles.sidebar}>
      <div className={styles.header}>
        <h1 className={`${styles.headerTitle} type-display`}>Chats</h1>
        {showLock && (
          <button type="button" className={styles.iconButton} aria-label="Lock Redvaultsystem" onClick={() => dispatch({ type: "LOCK" })}>
            <LockIcon />
          </button>
        )}
        <button type="button" className={styles.iconButton} aria-label="Calls and call links" onClick={() => dispatch({ type: "OPEN_CALLS" })}>
          <VideoIcon />
        </button>
        <button type="button" className={styles.primaryButton} aria-label="New conversation" onClick={() => dispatch({ type: "OPEN_NEW" })}>
          <PlusIcon />
        </button>
      </div>
      <div className={`${styles.caption} type-mono`}>
        <LockIcon size={13} /> End-to-end encrypted · keys on this device
      </div>

      <StoryTray />

      <label className={styles.search}>
        <SearchIcon size={18} />
        <input
          ref={searchRef}
          type="search"
          placeholder="Search chats and people"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          aria-label="Search chats"
        />
        <kbd className={`${styles.kbd} type-mono`}>Ctrl K</kbd>
      </label>

      <div className={styles.chips} role="group" aria-label="Filter chats">
        {chip("all", "All")}
        {chip("unread", unreadCount > 0 ? `Unread · ${unreadCount}` : "Unread")}
        {chip("groups", "Groups")}
        {chip("verified", "Verified")}
      </div>

      {state.friendRequestsIncoming.length > 0 && (
        <button type="button" className={styles.requestsBanner} onClick={() => dispatch({ type: "OPEN_FRIEND_REQUESTS" })}>
          <span>
            {state.friendRequestsIncoming.length === 1 ? "1 friend request" : `${state.friendRequestsIncoming.length} friend requests`}
          </span>
          <span className={styles.requestsBannerBadge}>{state.friendRequestsIncoming.length}</span>
        </button>
      )}

      <div className={styles.list}>
        {state.conversations.length === 0 ? (
          <p className={`${styles.empty} type-secondary`}>No conversations yet.</p>
        ) : shown.length === 0 ? (
          <p className={`${styles.empty} type-secondary`}>No chats match.</p>
        ) : (
          shown.map((original) => {
            // A newer protocol-3 message (kept on this device only) is what the row previews.
            const fresh = v3Latest.get(original.id);
            const last = original.messages[original.messages.length - 1];
            const selfId = state.identity?.userId;
            const c =
              fresh && selfId && fresh.ts > (last?.createdAt ?? 0)
                ? (() => {
                    const message = toMessage(fresh, selfId, original.kind === "group", original.kind === "dm" ? (original.userId ?? null) : null);
                    return { ...original, messages: [...original.messages, message], time: message.time };
                  })()
                : original;
            return (
            <ConversationRow
              key={c.id}
              conversation={c}
              active={c.id === activeConversationId}
              contactsById={contactsById}
              online={c.kind === "dm" && !!c.userId && live.online.has(c.userId)}
              typing={live.typingIn(c.id).length > 0}
              muted={c.kind === "dm" && !!c.userId && prefs.muted.includes(c.userId)}
              onOpen={(id) => dispatch({ type: "OPEN_THREAD", conversationId: id })}
            />
            );
          })
        )}
      </div>
    </div>
  );
}
