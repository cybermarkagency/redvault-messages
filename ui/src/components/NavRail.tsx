import type { ReactNode } from "react";
import { useLocalPrefs } from "../lib/localPrefs.js";
import { useUnreadAnnouncements } from "../lib/useUnreadAnnouncements.js";
import { useChannelUnread } from "../lib/channelWatch.js";
import { useAppState } from "../state/AppStateContext.js";
import type { Screen } from "../state/reducer.js";
import { Avatar } from "./Avatar.js";
import { BellIcon, ChartIcon, ChatIcon, CloudIcon, CommunitiesIcon, GlobeIcon, KeyIcon, LockIcon, PhoneIcon, ShieldIcon, SlidersIcon, UserIcon, UsersIcon } from "./icons24.js";
import { useIncomingCount } from "../lib/shareWatch.js";
import styles from "./NavRail.module.css";

export type Section = "chats" | "calls" | "contacts" | "communities" | "analytics" | "passwords" | "files" | "privacy" | "settings" | "admin" | "announcements" | "channels";

const SETTINGS_SCREENS = new Set<Screen["key"]>([
  "settings",
  "change-password",
  "change-username",
  "mfa-setup",
  "mfa-disable",
  "delete-account",
  "recovery-key",
  "transparency-feed",
]);

/** Which rail item a screen belongs to. */
export function sectionOf(screen: Screen): Section {
  if (screen.key === "calls" || screen.key === "join-call") return "calls";
  if (screen.key === "contacts") return "contacts";
  if (screen.key === "admin") return "admin";
  if (screen.key === "announcements") return "announcements";
  if (screen.key === "communities") return "communities";
  if (screen.key === "channels") return "channels";
  if (screen.key === "analytics") return "analytics";
  if (screen.key === "passwords") return "passwords";
  if (screen.key === "files") return "files";
  if (screen.key === "settings" && screen.section === "privacy") return "privacy";
  if (SETTINGS_SCREENS.has(screen.key)) return "settings";
  return "chats";
}

/** Top-level screens: on a phone these show the tab bar; everything else has a back button instead. */
export function isTopLevel(screen: Screen): boolean {
  return (
    screen.key === "chats" ||
    (screen.key === "calls" && !screen.userId) ||
    (screen.key === "contacts" && !screen.userId) ||
    screen.key === "settings" ||
    screen.key === "analytics" ||
    screen.key === "passwords" ||
    screen.key === "files" ||
    screen.key === "announcements" ||
    (screen.key === "communities" && (screen.page === undefined || !["community", "manage"].includes(screen.page))) ||
    (screen.key === "channels" && !screen.channelId) ||
    screen.key === "admin"
  );
}

function Item({ label, icon, active, badge, onClick }: { label: string; icon: ReactNode; active: boolean; badge?: number; onClick: () => void }) {
  return (
    <button type="button" className={`${styles.item} ${active ? styles.active : ""}`} aria-current={active ? "page" : undefined} onClick={onClick}>
      <span className={styles.icon}>
        {icon}
        {badge ? <span className={styles.badge} aria-label={`${badge} unread`}>{badge > 99 ? "99+" : badge}</span> : null}
      </span>
      <span className={styles.label}>{label}</span>
    </button>
  );
}

/**
 * The desktop nav rail (76px) or, on a phone, the bottom tab bar. The
 * lock tile at the top locks the app right away.
 */
export function NavRail({ variant }: { variant: "rail" | "tabs" }) {
  const { state, dispatch } = useAppState();
  const [prefs] = useLocalPrefs(state.identity?.userId);
  const section = sectionOf(state.screen);
  const identity = state.identity;
  const unread = state.conversations.reduce((n, c) => {
    const muted = c.kind === "dm" && c.userId !== undefined && prefs.muted.includes(c.userId);
    return muted ? n : n + c.unread;
  }, 0);
  const isSuperAdmin = identity?.role === "super_admin";
  const unreadAnnouncements = useUnreadAnnouncements(identity?.userId);
  const unreadChannels = useChannelUnread();
  const incomingFiles = useIncomingCount();

  const chats = <Item label="Chats" icon={<ChatIcon />} active={section === "chats"} badge={unread} onClick={() => dispatch({ type: "GO_CHATS" })} />;
  const calls = <Item label="Calls" icon={<PhoneIcon />} active={section === "calls"} onClick={() => dispatch({ type: "OPEN_CALLS" })} />;
  const contacts = <Item label="Contacts" icon={<UserIcon />} active={section === "contacts"} onClick={() => dispatch({ type: "OPEN_CONTACTS" })} />;
  const communities = (
    <Item label="Communities" icon={<CommunitiesIcon />} active={section === "communities"} onClick={() => dispatch({ type: "OPEN_COMMUNITIES" })} />
  );
  const analytics = (
    <Item label="Analytics" icon={<ChartIcon />} active={section === "analytics"} onClick={() => dispatch({ type: "OPEN_ANALYTICS" })} />
  );
  const passwords = (
    <Item label="Passwords" icon={<KeyIcon />} active={section === "passwords"} onClick={() => dispatch({ type: "OPEN_PASSWORDS" })} />
  );
  const files = (
    <Item label="Files" icon={<CloudIcon />} active={section === "files"} badge={incomingFiles} onClick={() => dispatch({ type: "OPEN_FILES" })} />
  );
  const inbox = (
    <Item
      label="Inbox"
      icon={<BellIcon />}
      active={section === "announcements"}
      badge={unreadAnnouncements}
      onClick={() => dispatch({ type: "OPEN_ANNOUNCEMENTS" })}
    />
  );
  const channels = (
    <Item label="Channels" icon={<GlobeIcon />} active={section === "channels"} badge={unreadChannels} onClick={() => dispatch({ type: "OPEN_CHANNELS" })} />
  );
  const settings = (
    <Item label="Settings" icon={<SlidersIcon />} active={section === "settings"} onClick={() => dispatch({ type: "OPEN_SETTINGS" })} />
  );
  const admin = isSuperAdmin ? (
    <Item label="Admin" icon={<UsersIcon />} active={section === "admin"} onClick={() => dispatch({ type: "OPEN_ADMIN" })} />
  ) : null;

  if (variant === "tabs") {
    return (
      <nav className={styles.tabs} aria-label="Main">
        {chats}
        {calls}
        {contacts}
        {communities}
        {channels}
        {files}
        {analytics}
        {passwords}
        {inbox}
        {settings}
        {admin}
      </nav>
    );
  }

  return (
    <nav className={styles.rail} aria-label="Main">
      <button type="button" className={styles.lock} aria-label="Lock Redvaultsystem" title="Lock now" onClick={() => dispatch({ type: "LOCK" })}>
        <LockIcon size={22} />
      </button>
      <div className={styles.group}>
        {chats}
        {calls}
        {contacts}
        {communities}
        {channels}
        {files}
        {analytics}
        {passwords}
        {inbox}
      </div>
      <div className={styles.spacer} />
      <div className={styles.group}>
        <Item
          label="Privacy"
          icon={<ShieldIcon />}
          active={section === "privacy"}
          onClick={() => dispatch({ type: "OPEN_SETTINGS", section: "privacy" })}
        />
        {settings}
        {admin}
      </div>
      {identity && (
        <button
          type="button"
          className={styles.you}
          aria-label="Your account"
          onClick={() => dispatch({ type: "OPEN_SETTINGS", section: "account" })}
        >
          <Avatar avatarUrl={identity.avatarUrl} seed={identity.handle} size={40} online={state.settings.showOnlineStatus} />
        </button>
      )}
    </nav>
  );
}
