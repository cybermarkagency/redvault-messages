// Files uploaded to you through your Contributor links: the Files badge and
// a notification ("3 encrypted files were added through your “Project”
// link"), worked out on this device. The server only says that something
// arrived (a content-free live event); which link, how many and the link's
// name come from your own sealed copies. Nobody who uploads is named: they
// aren't known.
import { useEffect, useState } from "react";
import * as api from "./api.js";
import { decodeOwnerShare } from "./shareOwner.js";

const CHANGE_EVENT = "redvaultsystem:incoming-changed";
let incoming = 0;

export function useIncomingCount(): number {
  const [count, setCount] = useState(incoming);
  useEffect(() => {
    const onChange = () => setCount(incoming);
    window.addEventListener(CHANGE_EVENT, onChange);
    return () => window.removeEventListener(CHANGE_EVENT, onChange);
  }, []);
  return count;
}

/** Tells the badge (and an open Files page) that Incoming changed. */
export function setIncomingCount(count: number): void {
  incoming = count;
  window.dispatchEvent(new Event(CHANGE_EVENT));
}

export const INCOMING_REFRESH_EVENT = "redvaultsystem:incoming-refresh";

function notifiedKey(userId: string): string {
  return `redvaultsystem:incoming-notified:${userId}`;
}

/** What this browser already announced; `known` is false the first time (nothing to compare with yet). */
function readNotified(userId: string): { ids: Set<string>; known: boolean } {
  try {
    const raw = localStorage.getItem(notifiedKey(userId));
    return { ids: new Set(raw ? (JSON.parse(raw) as string[]) : []), known: raw !== null };
  } catch {
    return { ids: new Set(), known: false };
  }
}

function writeNotified(userId: string, ids: Set<string>): void {
  try {
    localStorage.setItem(notifiedKey(userId), JSON.stringify([...ids].slice(-2000)));
  } catch {
    // Best effort: a file may be announced again later.
  }
}

/** The notification's text for some files that came in through one link. */
export function arrivalText(count: number, label: string | null): string {
  const files = count === 1 ? "1 encrypted file was" : `${count} encrypted files were`;
  return label ? `${files} added through your “${label}” sharing link.` : `${files} added through one of your sharing links.`;
}

/**
 * One check: the Incoming count for the badge, and one notification per
 * link for files that arrived since the last check (none for the first check
 * on a browser: those are just marked as seen).
 */
export async function checkIncoming(userId: string, driveKey: Uint8Array, options: { onOpen: () => void; quiet: boolean }): Promise<number> {
  const [{ items }, { links }] = await Promise.all([api.listIncomingFiles(), api.listShareLinks()]);
  setIncomingCount(items.length);
  window.dispatchEvent(new Event(INCOMING_REFRESH_EVENT));
  const { ids: notified, known } = readNotified(userId);
  const firstRun = !known;
  const fresh = items.filter((i) => !notified.has(i.id));
  if (fresh.length === 0) {
    if (firstRun) writeNotified(userId, notified);
    return items.length;
  }
  for (const i of fresh) notified.add(i.id);
  writeNotified(userId, notified);
  if (firstRun || options.quiet || typeof Notification === "undefined" || Notification.permission !== "granted") return items.length;
  const byLink = new Map<string, number>();
  for (const i of fresh) byLink.set(i.shareId ?? "", (byLink.get(i.shareId ?? "") ?? 0) + 1);
  for (const [shareId, count] of byLink) {
    const link = links.find((l) => l.id === shareId);
    const label = link ? decodeOwnerShare(link, driveKey).box?.label || null : null;
    const shown = new Notification("New files in Incoming", { body: arrivalText(count, label), tag: `incoming-${shareId || "deleted"}` });
    shown.onclick = () => {
      window.focus();
      options.onOpen();
      shown.close();
    };
  }
  return items.length;
}
