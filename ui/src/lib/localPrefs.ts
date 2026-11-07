// Per-device, per-account preferences that never leave this browser:
// favorites, muted contacts, chosen audio/video devices, and small UI
// choices. None of this is sent to the server. Every access is guarded:
// storage can be missing or refuse writes (private windows, quotas).
import { useCallback, useEffect, useState } from "react";

const PREFIX = "redvaultsystem:prefs:";
const CHANGE_EVENT = "redvaultsystem:prefs-changed";

export interface LocalPrefs {
  /** userIds of favorite contacts, in the order they were starred. */
  favorites: string[];
  /** userIds whose calls don't ring and whose chats don't count as unread. */
  muted: string[];
  /** Play a ringtone for incoming calls. */
  ringtone: boolean;
  /** Chosen devices; null = the system default. */
  microphoneId: string | null;
  speakerId: string | null;
  cameraId: string | null;
  /** Desktop chat screen: the details panel is open. */
  chatDetailsOpen: boolean;
  /** Send and show typing indicators (off until turned on; kept across sign-ins on this browser). */
  typingIndicators: boolean;
  /**
   * Channels followed on this device. Kept only here, never on the server,
   * so nobody can list a channel's followers. `seenAt` is the newest post
   * already seen (a post's createdAt), for the unread dot.
   */
  followedChannels: FollowedChannel[];
}

export interface FollowedChannel {
  id: string;
  name: string;
  seenAt: string | null;
  /** A private channel's link secret (base64url), which is what lets this device read it. */
  secret?: string;
  /** No notifications for its new posts (still counted on the Channels badge). */
  muted?: boolean;
}

const DEFAULTS: LocalPrefs = {
  favorites: [],
  muted: [],
  ringtone: true,
  microphoneId: null,
  speakerId: null,
  cameraId: null,
  chatDetailsOpen: true,
  typingIndicators: false,
  followedChannels: [],
};

function key(userId: string): string {
  return `${PREFIX}${userId}`;
}

export function readPrefs(userId: string | undefined): LocalPrefs {
  if (!userId) return DEFAULTS;
  try {
    const raw = localStorage.getItem(key(userId));
    return raw ? { ...DEFAULTS, ...(JSON.parse(raw) as Partial<LocalPrefs>) } : DEFAULTS;
  } catch {
    return DEFAULTS;
  }
}

export function writePrefs(userId: string, changes: Partial<LocalPrefs>): LocalPrefs {
  const next = { ...readPrefs(userId), ...changes };
  try {
    localStorage.setItem(key(userId), JSON.stringify(next));
  } catch {
    // Best effort: the change still applies for this page's lifetime via the event below.
  }
  window.dispatchEvent(new CustomEvent(CHANGE_EVENT, { detail: { userId, prefs: next } }));
  return next;
}

/** Live view of this account's prefs, updated wherever they change. */
export function useLocalPrefs(userId: string | undefined): [LocalPrefs, (changes: Partial<LocalPrefs>) => void] {
  const [prefs, setPrefs] = useState(() => readPrefs(userId));
  useEffect(() => {
    setPrefs(readPrefs(userId));
    const onChange = (e: Event) => {
      const detail = (e as CustomEvent<{ userId: string; prefs: LocalPrefs }>).detail;
      if (detail.userId === userId) setPrefs(detail.prefs);
    };
    window.addEventListener(CHANGE_EVENT, onChange);
    return () => window.removeEventListener(CHANGE_EVENT, onChange);
  }, [userId]);
  const update = useCallback(
    (changes: Partial<LocalPrefs>) => {
      if (userId) setPrefs(writePrefs(userId, changes));
    },
    [userId]
  );
  return [prefs, update];
}

export function toggleIn(list: string[], id: string): string[] {
  return list.includes(id) ? list.filter((x) => x !== id) : [...list, id];
}
