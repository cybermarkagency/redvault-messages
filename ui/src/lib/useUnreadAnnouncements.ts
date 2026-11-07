import { useCallback, useEffect, useState } from "react";
import * as api from "./api.js";

const POLL_MS = 60_000;
const CHANGED = "announcements-changed";

/** Tells the nav badge to recount (after reading, voting or replying). */
export function announcementsChanged(): void {
  window.dispatchEvent(new Event(CHANGED));
}

/** How many announcements from the admin this account hasn't read; re-counted every minute and whenever something changes. */
export function useUnreadAnnouncements(userId: string | undefined): number {
  const [unread, setUnread] = useState(0);
  const refresh = useCallback(() => {
    if (!userId) return;
    Promise.resolve()
      .then(() => api.listAnnouncements())
      .then((list) => setUnread(list.filter((a) => !a.read).length))
      .catch(() => {});
  }, [userId]);

  useEffect(() => {
    if (!userId) {
      setUnread(0);
      return;
    }
    refresh();
    const timer = setInterval(refresh, POLL_MS);
    window.addEventListener(CHANGED, refresh);
    return () => {
      clearInterval(timer);
      window.removeEventListener(CHANGED, refresh);
    };
  }, [userId, refresh]);

  return unread;
}
