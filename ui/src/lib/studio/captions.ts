// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Captions (subtitles) for video and audio stories: typed by the author, or imported from an .srt file.
// Nothing is transcribed for you: speech recognition would mean sending the recording to a service.
import { MAX_CAPTIONS, MAX_CAPTION_END_MS, MAX_CAPTION_TEXT, parseCaptions, type StoryCaption } from "../storyCrypto.js";

/** "1:23.4" for 83,400 ms. */
export function formatClock(ms: number): string {
  const total = Math.max(0, Math.round(ms));
  const minutes = Math.floor(total / 60_000);
  const seconds = (total % 60_000) / 1000;
  return `${minutes}:${seconds.toFixed(1).padStart(4, "0")}`;
}

/** Reads "83.4", "1:23.4", "0:01:23,400" or "00:01:23.4" as milliseconds; null if it isn't a time. */
export function parseClock(text: string): number | null {
  const parts = text.trim().replace(",", ".").split(":");
  if (parts.length === 0 || parts.length > 3 || parts.some((p) => !/^\d+(\.\d+)?$/.test(p))) return null;
  const nums = parts.map(Number);
  let seconds = 0;
  for (const n of nums) seconds = seconds * 60 + n;
  const ms = Math.round(seconds * 1000);
  return Number.isFinite(ms) && ms >= 0 && ms <= MAX_CAPTION_END_MS ? ms : null;
}

/** Reads SubRip (.srt) text. Cues that don't fit the limits are left out, as everywhere captions are read. */
export function parseSrt(text: string): StoryCaption[] {
  const out: unknown[] = [];
  const blocks = text.replace(/\r/g, "").split(/\n{2,}/);
  for (const block of blocks) {
    const lines = block.split("\n").filter((l) => l.trim() !== "");
    const timing = lines.findIndex((l) => l.includes("-->"));
    if (timing < 0) continue;
    const [a, b] = lines[timing].split("-->");
    const start = parseClock(a);
    const end = parseClock((b ?? "").trim().split(/\s+/)[0] ?? "");
    if (start === null || end === null) continue;
    // Markup in SRT (<i>, {\an8}) is dropped: the viewer shows plain text only.
    const words = lines
      .slice(timing + 1)
      .join(" ")
      .replace(/<[^>]*>/g, "")
      .replace(/\{[^}]*\}/g, "");
    out.push({ startMs: start, endMs: end, text: words });
  }
  return parseCaptions(out);
}

export function toSrt(captions: readonly StoryCaption[]): string {
  const stamp = (ms: number) => {
    const h = Math.floor(ms / 3_600_000);
    const m = Math.floor((ms % 3_600_000) / 60_000);
    const s = Math.floor((ms % 60_000) / 1000);
    const milli = ms % 1000;
    const p = (n: number, w = 2) => String(n).padStart(w, "0");
    return `${p(h)}:${p(m)}:${p(s)},${p(milli, 3)}`;
  };
  return captions.map((c, i) => `${i + 1}\n${stamp(c.startMs)} --> ${stamp(c.endMs)}\n${c.text}\n`).join("\n");
}

/** Spreads typed lines evenly over a recording: a quick way to caption speech you wrote down. */
export function evenCaptions(lines: readonly string[], durationMs: number): StoryCaption[] {
  const clean = lines.map((l) => l.trim()).filter(Boolean).slice(0, MAX_CAPTIONS);
  if (clean.length === 0 || durationMs <= 0) return [];
  const slot = Math.floor(durationMs / clean.length);
  // The last line runs to the very end, so rounding never leaves it short.
  return parseCaptions(clean.map((text, i) => ({ startMs: i * slot, endMs: i === clean.length - 1 ? durationMs : (i + 1) * slot, text: text.slice(0, MAX_CAPTION_TEXT) })));
}
