// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// New-post notifications for followed channels, worked out on this device:
// the app checks each followed channel itself (public reads, no cookie), so
// the server still keeps no follower list and followers stay unlimited and
// private. Notifications show while the app is open (in any tab); what was
// already notified is remembered only in this browser.
import { useEffect, useState } from "react";
import * as api from "./api.js";
import { channelAccessToken, hasUnread } from "./channels.js";
import { readPrefs } from "./localPrefs.js";
import { toPlain } from "./textFormat.js";

const UNREAD_EVENT = "redvaultsystem:channel-unread";
let unread = 0;

function notifiedKey(userId: string): string {
  return `redvaultsystem:channel-notified:${userId}`;
}

function readNotified(userId: string): Record<string, string> {
  try {
    const raw = localStorage.getItem(notifiedKey(userId));
    return raw ? (JSON.parse(raw) as Record<string, string>) : {};
  } catch {
    return {};
  }
}

function writeNotified(userId: string, value: Record<string, string>): void {
  try {
    localStorage.setItem(notifiedKey(userId), JSON.stringify(value));
  } catch {
    // Best effort: a post may be notified again later.
  }
}

/** How many followed channels have posts you haven't seen, as of the last check. */
export function useChannelUnread(): number {
  const [count, setCount] = useState(unread);
  useEffect(() => {
    const onChange = () => setCount(unread);
    window.addEventListener(UNREAD_EVENT, onChange);
    return () => window.removeEventListener(UNREAD_EVENT, onChange);
  }, []);
  return count;
}

function setUnread(count: number): void {
  unread = count;
  window.dispatchEvent(new Event(UNREAD_EVENT));
}

/** A notification's text for a post: its text, or what it carries. */
export function postPreview(post: api.ChannelPostDto | undefined): string {
  if (!post) return "New post";
  const text = toPlain(post.body).replace(/\s+/g, " ").trim();
  if (text) return text.length > 140 ? `${text.slice(0, 139)}…` : text;
  if (post.poll) return `${post.poll.quiz ? "Quiz" : "Poll"}: ${post.poll.question}`;
  if (post.checklist) return `Checklist: ${post.checklist.title}`;
  const kind = post.media?.[0]?.kind;
  return kind === "photo" ? "Photo" : kind === "video" ? "Video" : kind === "voice" ? "Voice message" : kind === "round" ? "Video message" : kind === "audio" ? "Audio" : kind === "file" ? "File" : "New post";
}

export function notificationsSupported(): boolean {
  return typeof Notification !== "undefined";
}

/**
 * One check: reads each followed channel, counts those with unseen posts
 * (the Channels badge), and shows a notification for each newly posted one,
 * unless it's muted or open right now.
 */
export async function checkFollowedChannels(
  userId: string,
  options: { currentChannelId: string | null; openChannel: (channelId: string) => void }
): Promise<number> {
  const followed = readPrefs(userId).followedChannels;
  const notified = readNotified(userId);
  let count = 0;
  const queue = [...followed];
  const worker = async () => {
    for (let f = queue.shift(); f; f = queue.shift()) {
      try {
        const access = f.secret ? channelAccessToken(f.secret) : null;
        const channel = await api.getChannel(f.id, access);
        if (!hasUnread(f, channel)) continue;
        count += 1;
        const last = notified[f.id];
        const isNew = channel.lastPostAt && (!last || Date.parse(channel.lastPostAt) > Date.parse(last));
        if (!isNew || f.muted || options.currentChannelId === f.id) continue;
        notified[f.id] = channel.lastPostAt!;
        if (notificationsSupported() && Notification.permission === "granted") {
          const [latest] = await api.getChannelPosts(f.id, undefined, access, 1).catch(() => []);
          const shown = new Notification(channel.name, { body: postPreview(latest), tag: `channel-${f.id}` });
          shown.onclick = () => {
            window.focus();
            options.openChannel(f.id);
            shown.close();
          };
        }
      } catch {
        // Gone, private without the link, or offline: try again next time.
      }
    }
  };
  await Promise.all([worker(), worker(), worker(), worker()]);
  writeNotified(userId, notified);
  setUnread(count);
  return count;
}

export const CHECK_EVERY_MS = 2 * 60 * 1000;
