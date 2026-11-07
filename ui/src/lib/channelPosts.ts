// Helpers for channel posts on this device: what kind of media a file is,
// how long things are, the formatting toolbar, and what this device
// remembers (your votes, where you stopped in a podcast). None of what's
// remembered leaves this browser.
import type { ChannelMediaKind, ChannelPollDto } from "./api.js";

const PHOTO_TYPES = new Set(["image/jpeg", "image/png", "image/gif", "image/webp", "image/avif", "image/heic", "image/heif", "image/bmp"]);

/** How a picked file goes into a post: pictures as photos (at full resolution), videos, audio (podcasts), anything else as a file. */
export function mediaKindFor(file: File): ChannelMediaKind {
  const type = file.type.toLowerCase();
  if (PHOTO_TYPES.has(type)) return "photo";
  if (type.startsWith("video/")) return "video";
  if (type.startsWith("audio/")) return "audio";
  return "file";
}

/** A picture's size or a recording's length, when the browser can tell (best effort, never blocks an upload for long). */
export function probeMedia(file: Blob, kind: ChannelMediaKind): Promise<{ width?: number; height?: number; durationMs?: number }> {
  return new Promise((resolve) => {
    const url = URL.createObjectURL(file);
    const done = (info: { width?: number; height?: number; durationMs?: number }) => {
      URL.revokeObjectURL(url);
      resolve(info);
    };
    const timer = setTimeout(() => done({}), 4000);
    const finish = (info: { width?: number; height?: number; durationMs?: number }) => {
      clearTimeout(timer);
      done(info);
    };
    if (kind === "photo") {
      const img = new Image();
      img.onload = () => finish({ width: img.naturalWidth, height: img.naturalHeight });
      img.onerror = () => finish({});
      img.src = url;
      return;
    }
    if (kind === "file") return finish({});
    const el = document.createElement(kind === "audio" || kind === "voice" ? "audio" : "video");
    el.preload = "metadata";
    el.onloadedmetadata = () => {
      const durationMs = Number.isFinite(el.duration) ? Math.round(el.duration * 1000) : undefined;
      const video = el as HTMLVideoElement;
      finish({ durationMs, ...(video.videoWidth ? { width: video.videoWidth, height: video.videoHeight } : {}) });
    };
    el.onerror = () => finish({});
    el.src = url;
  });
}

/** "3:05", or "1:02:03" for an hour or more. */
export function formatDuration(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds < 0) return "0:00";
  const s = Math.floor(seconds);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const rest = String(s % 60).padStart(2, "0");
  return h > 0 ? `${h}:${String(m).padStart(2, "0")}:${rest}` : `${m}:${rest}`;
}

/**
 * The formatting toolbar: wraps the selected text in a marker (*bold*,
 * _italic_, ~strike~, `code`, ||spoiler||), or puts an empty pair at the
 * cursor. Returns the new text and where the selection goes.
 */
export function wrapSelection(text: string, start: number, end: number, marker: string): { text: string; start: number; end: number } {
  const selected = text.slice(start, end);
  const next = text.slice(0, start) + marker + selected + marker + text.slice(end);
  return { text: next, start: start + marker.length, end: end + marker.length };
}

function readJson<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(key);
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

function writeJson(key: string, value: unknown): void {
  try {
    localStorage.setItem(key, JSON.stringify(value));
  } catch {
    // Best effort: without storage it's only forgotten sooner.
  }
}

const VOTES_KEY = "redvaultsystem:channel-votes";

/** What this device remembers of your vote: your choice, and for a quiz the right answer you were shown. */
export interface RememberedVote {
  mine: number[];
  correct?: number | null;
  explanation?: string | null;
}

export function rememberedVote(postId: string): RememberedVote | null {
  return readJson<Record<string, RememberedVote>>(VOTES_KEY, {})[postId] ?? null;
}

export function rememberVote(postId: string, vote: RememberedVote | null, poll?: ChannelPollDto): void {
  const all = readJson<Record<string, RememberedVote>>(VOTES_KEY, {});
  if (vote && vote.mine.length) all[postId] = { ...vote, ...(poll?.quiz ? { correct: poll.correct, explanation: poll.explanation } : {}) };
  else delete all[postId];
  // Keep the most recent 2000.
  const keys = Object.keys(all);
  for (const key of keys.slice(0, Math.max(0, keys.length - 2000))) delete all[key];
  writeJson(VOTES_KEY, all);
}

const POSITIONS_KEY = "redvaultsystem:media-positions";

/** Where you stopped in a podcast or long audio, in seconds (0 when you'd finished or never started). */
export function savedPosition(mediaId: string): number {
  return readJson<Record<string, number>>(POSITIONS_KEY, {})[mediaId] ?? 0;
}

export function savePosition(mediaId: string, seconds: number): void {
  const all = readJson<Record<string, number>>(POSITIONS_KEY, {});
  if (seconds > 0) all[mediaId] = Math.round(seconds);
  else delete all[mediaId];
  const keys = Object.keys(all);
  for (const key of keys.slice(0, Math.max(0, keys.length - 500))) delete all[key];
  writeJson(POSITIONS_KEY, all);
}

/** The playback speeds offered, in order. */
export const SPEEDS = [1, 1.5, 2] as const;

export function nextSpeed(current: number): number {
  const i = SPEEDS.indexOf(current as (typeof SPEEDS)[number]);
  return SPEEDS[(i + 1) % SPEEDS.length];
}
