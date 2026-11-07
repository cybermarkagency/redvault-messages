// Replies and reactions, carried inside the ordinary E2EE message body so
// the server learns nothing new about them — to it they're just more
// ciphertext. Mirrored byte-for-byte by the Android app
// (android/.../data/model/RichMessage.kt); change both together.
//
//   reply:    "\u0000rv-msg:" + {"type":"text","version":1,"text":…,"replyTo":{"messageId","senderId","preview"}}
//   reaction: "\u0000rv-reaction:" + {"type":"reaction","version":1,"messageId","emoji"}  any single emoji
//
// A plain (non-reply) text message stays plain text, exactly as before, so
// older clients keep reading everything that isn't a reply or reaction.
// Removing a reaction deletes the reaction message itself server-side.
//
// Optional fields of the "text" type (desktop/core/.../rich_message.hpp has
// the same rules). In each, "text" is what an app that doesn't know the
// field shows instead:
//
//   edit:      {…,"text":<new text>,"edits":<messageId>}  only the original sender may edit
//   forwarded: {…,"forwarded":true}
//   pin:       {…,"text":"📌 Pinned: …","pin":{"messageId","preview"}}
//   unpin:     {…,"text":"Unpinned a message","unpin":<messageId>}
//   poll:      {…,"text":"📊 Poll: …","poll":{"question","options":[…],"multiple":bool}}
//   vote:      {…,"text":"Voted: …","vote":{"pollId","options":[indexes]}}  empty takes it back
//   end poll:  {…,"text":"Ended the poll","endPoll":<pollId>}  only the poll's creator
//   channel:   {…,"forwarded":true,"channelPost":{"channelId","postId","channelName"}}
//              a channel post forwarded into a chat; "text" is the post. Each
//              copy that's shown counts a view for the original post.
//
// Everything parsed here is untrusted input from the other participant.
const TEXT_PREFIX = "\u0000rv-msg:";
const REACTION_PREFIX = "\u0000rv-reaction:";

/** The quick reactions, offered first and shown first, in this order. Any other single emoji is valid too (isReactionEmoji). */
export const REACTION_EMOJI: readonly string[] = ["👍", "❤️", "😂", "😮", "😢", "🙏"];
/** Exactly one emoji (isReactionEmoji). */
export type ReactionEmoji = string;

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const MAX_TEXT = 65_536;
const MAX_PREVIEW = 200;
// Control characters and bidi/zero-width tricks — never render these from a sender in a quote.
const HIDDEN_CHARS = /[\u0000-\u0008\u000b-\u001f\u007f-\u009f​-‏‪-‮⁠-⁩﻿]/g;

export interface ReplyRef {
  messageId: string;
  senderId: string;
  /** A short quote of the replied-to message, carried encrypted with the reply itself. */
  preview: string;
}

export interface PollDef {
  question: string;
  options: string[];
  multiple: boolean;
}

export interface TextExtras {
  /** This is an edit: the new text of that (lowercase) message id. */
  edits?: string;
  forwarded?: boolean;
  pin?: { messageId: string; preview: string };
  unpin?: string;
  poll?: PollDef;
  vote?: { pollId: string; options: number[] };
  endPoll?: string;
  channelPost?: ChannelPostRef;
  storyReply?: StoryReplyRef;
}

/**
 * A reply or emoji reaction to someone's story, sent as a message to its
 * author. Only the story's id and a short label travel with it (never its
 * key), so it shows what was replied to while the story is still around.
 */
export interface StoryReplyRef {
  authorId: string;
  storyId: string;
  kind: "image" | "video" | "text";
  /** "Photo", "Video", or the start of a text story. */
  preview: string;
  reaction: boolean;
}

/** The channel post a forwarded message came from: shown as "Forwarded from <channel>", and counted as a view of it. */
export interface ChannelPostRef {
  channelId: string;
  postId: string;
  channelName: string;
}

const MAX_CHANNEL_NAME = 80;

export type ParsedBody =
  | ({ kind: "text"; text: string; replyTo?: ReplyRef } & TextExtras)
  | { kind: "reaction"; messageId: string; emoji: ReactionEmoji }
  | { kind: "invalid" };

export const MAX_POLL_OPTIONS = 10;
const MAX_POLL_QUESTION = 300;
const MAX_POLL_OPTION = 100;

export function isStructured(plaintext: string): boolean {
  return plaintext.startsWith(TEXT_PREFIX) || plaintext.startsWith(REACTION_PREFIX);
}

export function makePreview(text: string): string {
  const clean = text.replace(HIDDEN_CHARS, "").replace(/\s+/g, " ").trim();
  return clean.length > MAX_PREVIEW ? `${clean.slice(0, MAX_PREVIEW - 1)}…` : clean;
}

export function serializeReply(text: string, replyTo: ReplyRef): string {
  return (
    TEXT_PREFIX +
    JSON.stringify({
      type: "text",
      version: 1,
      text,
      replyTo: { messageId: replyTo.messageId, senderId: replyTo.senderId, preview: makePreview(replyTo.preview) },
    })
  );
}

function textBody(fallback: string, extra: Record<string, unknown>): string {
  return TEXT_PREFIX + JSON.stringify({ type: "text", version: 1, text: fallback, ...extra });
}

/** A new version of the text of one of your own messages. */
export function serializeEdit(text: string, messageId: string): string {
  return textBody(text, { edits: messageId });
}

/** Text forwarded from another chat (a reply's quote isn't carried over). */
export function serializeForwarded(text: string): string {
  return textBody(text, { forwarded: true });
}

/** A channel post forwarded into a chat, keeping where it came from (also when forwarded on again). */
export function serializeChannelForward(text: string, ref: ChannelPostRef): string {
  const channelName = makePreview(ref.channelName).slice(0, MAX_CHANNEL_NAME);
  return textBody(text, { forwarded: true, channelPost: { channelId: ref.channelId.toLowerCase(), postId: ref.postId.toLowerCase(), channelName } });
}

/** A reply (or a quick reaction emoji) to a story, as a message to its author. */
export function serializeStoryReply(text: string, ref: StoryReplyRef): string {
  return textBody(text, {
    storyReply: { authorId: ref.authorId.toLowerCase(), storyId: ref.storyId.toLowerCase(), kind: ref.kind, preview: makePreview(ref.preview).slice(0, 80), reaction: ref.reaction },
  });
}

export function serializePin(messageId: string, preview: string): string {
  const quote = makePreview(preview);
  return textBody(`📌 Pinned: ${quote}`, { pin: { messageId, preview: quote } });
}

export function serializeUnpin(messageId: string): string {
  return textBody("Unpinned a message", { unpin: messageId });
}

/** Throws unless there's a question and 2-10 distinct, non-empty options. */
export function serializePoll(question: string, options: string[], multiple: boolean): string {
  const q = question.trim();
  const clean = options.map((o) => o.trim()).filter(Boolean);
  if (!q || q.length > MAX_POLL_QUESTION) throw new Error("Poll question");
  if (clean.length < 2 || clean.length > MAX_POLL_OPTIONS || new Set(clean).size !== clean.length || clean.some((o) => o.length > MAX_POLL_OPTION)) {
    throw new Error("A poll needs 2 to 10 different options");
  }
  const fallback = [`📊 Poll: ${q}`, ...clean.map((o, i) => `${i + 1}. ${o}`)].join("\n");
  return textBody(fallback, { poll: { question: q, options: clean, multiple } });
}

/** `labels` only make the fallback text; empty `options` takes the vote back. */
export function serializeVote(pollId: string, options: number[], labels: string[]): string {
  const fallback = options.length === 0 ? "Took back a poll vote" : `Voted: ${options.map((i) => labels[i] ?? String(i + 1)).join(", ")}`;
  return textBody(fallback, { vote: { pollId, options } });
}

export function serializeEndPoll(pollId: string): string {
  return textBody("Ended the poll", { endPoll: pollId });
}

/** Throws unless `emoji` is one emoji (isReactionEmoji). */
export function serializeReaction(messageId: string, emoji: ReactionEmoji): string {
  if (!isReactionEmoji(emoji)) throw new Error("Unsupported reaction");
  return REACTION_PREFIX + JSON.stringify({ type: "reaction", version: 1, messageId, emoji });
}

// Reactions: exactly one emoji, by the same grammar as the desktop
// (rich_message.cpp isReactionEmoji) and Android (RichMessage.kt) apps.
// Change all three together.
const MAX_EMOJI_CODE_POINTS = 16;
const ZWJ = 0x200d;
const VS16 = 0xfe0f;
const KEYCAP = 0x20e3;
/** The emoji blocks, plus the older symbols that are drawn as emoji. */
const PICTOGRAPH_RANGES: [number, number][] = [
  [0x00a9, 0x00a9], [0x00ae, 0x00ae], [0x203c, 0x203c], [0x2049, 0x2049], [0x2122, 0x2122], [0x2139, 0x2139],
  [0x2194, 0x2199], [0x21a9, 0x21aa], [0x231a, 0x231b], [0x2328, 0x2328], [0x23cf, 0x23cf], [0x23e9, 0x23f3],
  [0x23f8, 0x23fa], [0x24c2, 0x24c2], [0x25aa, 0x25ab], [0x25b6, 0x25b6], [0x25c0, 0x25c0], [0x25fb, 0x25fe],
  [0x2600, 0x27bf], [0x2934, 0x2935], [0x2b05, 0x2b07], [0x2b1b, 0x2b1c], [0x2b50, 0x2b50], [0x2b55, 0x2b55],
  [0x3030, 0x3030], [0x303d, 0x303d], [0x3297, 0x3297], [0x3299, 0x3299], [0x1f000, 0x1faff],
];
const isRegional = (c: number) => c >= 0x1f1e6 && c <= 0x1f1ff;
const isSkinTone = (c: number) => c >= 0x1f3fb && c <= 0x1f3ff;
const isTag = (c: number) => c >= 0xe0020 && c <= 0xe007f;
const isPictograph = (c: number) => !isRegional(c) && !isSkinTone(c) && PICTOGRAPH_RANGES.some(([lo, hi]) => c >= lo && c <= hi);

/**
 * Whether `emoji` is exactly one emoji: a pictograph (optionally with VS16,
 * a skin tone and tag characters), a flag or a keycap, or several of those
 * joined by ZWJ.
 */
export function isReactionEmoji(emoji: string): boolean {
  // Iterating a string yields code points; a lone surrogate comes through as itself and fails below.
  const cps = Array.from(emoji, (ch) => ch.codePointAt(0)!);
  const n = cps.length;
  if (n === 0 || n > MAX_EMOJI_CODE_POINTS) return false;
  // A keycap: 0-9, # or *, an optional VS16, then U+20E3.
  if ((cps[0] >= 0x30 && cps[0] <= 0x39) || cps[0] === 0x23 || cps[0] === 0x2a) {
    return (n === 2 && cps[1] === KEYCAP) || (n === 3 && cps[1] === VS16 && cps[2] === KEYCAP);
  }
  let i = 0;
  const at = (k: number) => (k < n ? cps[k] : 0);
  // One element: a flag (two regional indicators), or a pictograph with an
  // optional VS16, then an optional skin tone, then any tag characters.
  const element = (): boolean => {
    if (isRegional(at(i))) {
      if (!isRegional(at(i + 1))) return false;
      i += 2;
      return true;
    }
    if (!isPictograph(at(i))) return false;
    i++;
    if (at(i) === VS16) i++;
    if (isSkinTone(at(i))) i++;
    while (isTag(at(i))) i++;
    return true;
  };
  if (!element()) return false;
  while (i < n) {
    if (cps[i] !== ZWJ) return false;
    i++;
    if (!element()) return false;
  }
  return true;
}

/** Parses a structured body (reply or reaction). Plain text isn't passed here. */
export function parseStructured(plaintext: string): ParsedBody {
  const isReaction = plaintext.startsWith(REACTION_PREFIX);
  let raw: unknown;
  try {
    raw = JSON.parse(plaintext.slice(isReaction ? REACTION_PREFIX.length : TEXT_PREFIX.length));
  } catch {
    return { kind: "invalid" };
  }
  if (!raw || typeof raw !== "object") return { kind: "invalid" };
  const d = raw as Record<string, unknown>;
  if (d.version !== 1) return { kind: "invalid" };

  if (isReaction) {
    if (d.type !== "reaction" || typeof d.messageId !== "string" || !UUID_RE.test(d.messageId)) return { kind: "invalid" };
    if (typeof d.emoji !== "string" || !isReactionEmoji(d.emoji)) return { kind: "invalid" };
    return { kind: "reaction", messageId: d.messageId.toLowerCase(), emoji: d.emoji };
  }

  if (d.type !== "text" || typeof d.text !== "string" || d.text.length > MAX_TEXT) return { kind: "invalid" };
  const extras = parseExtras(d);
  if (extras === null) return { kind: "invalid" };
  const r = d.replyTo as Record<string, unknown> | undefined;
  if (r === undefined) return { kind: "text", text: d.text, ...extras };
  if (
    !r ||
    typeof r !== "object" ||
    typeof r.messageId !== "string" ||
    !UUID_RE.test(r.messageId) ||
    typeof r.senderId !== "string" ||
    !UUID_RE.test(r.senderId) ||
    typeof r.preview !== "string"
  ) {
    return { kind: "invalid" };
  }
  return {
    kind: "text",
    text: d.text,
    replyTo: { messageId: r.messageId.toLowerCase(), senderId: r.senderId.toLowerCase(), preview: makePreview(r.preview) },
    ...extras,
  };
}

const isUuid = (v: unknown): v is string => typeof v === "string" && UUID_RE.test(v);

/** The optional fields above; null if any is present but malformed (the whole message is then invalid). */
function parseExtras(d: Record<string, unknown>): TextExtras | null {
  const out: TextExtras = {};
  if ("edits" in d) {
    if (!isUuid(d.edits)) return null;
    out.edits = d.edits.toLowerCase();
  }
  if ("forwarded" in d) {
    if (typeof d.forwarded !== "boolean") return null;
    if (d.forwarded) out.forwarded = true;
  }
  if ("pin" in d) {
    const p = d.pin as Record<string, unknown> | null;
    if (!p || typeof p !== "object" || !isUuid(p.messageId) || typeof p.preview !== "string") return null;
    out.pin = { messageId: p.messageId.toLowerCase(), preview: makePreview(p.preview) };
  }
  if ("unpin" in d) {
    if (!isUuid(d.unpin)) return null;
    out.unpin = d.unpin.toLowerCase();
  }
  if ("poll" in d) {
    const p = d.poll as Record<string, unknown> | null;
    if (!p || typeof p !== "object" || typeof p.question !== "string" || !p.question || p.question.length > MAX_POLL_QUESTION) return null;
    if (!Array.isArray(p.options) || p.options.length < 2 || p.options.length > MAX_POLL_OPTIONS) return null;
    if (!p.options.every((o): o is string => typeof o === "string" && o.length > 0 && o.length <= MAX_POLL_OPTION)) return null;
    if ("multiple" in p && typeof p.multiple !== "boolean") return null;
    out.poll = { question: p.question, options: p.options, multiple: p.multiple === true };
  }
  if ("vote" in d) {
    const v = d.vote as Record<string, unknown> | null;
    if (!v || typeof v !== "object" || !isUuid(v.pollId) || !Array.isArray(v.options) || v.options.length > MAX_POLL_OPTIONS) return null;
    const options: number[] = [];
    for (const o of v.options) {
      if (!Number.isInteger(o) || (o as number) < 0 || (o as number) >= MAX_POLL_OPTIONS) return null;
      if (!options.includes(o as number)) options.push(o as number);
    }
    out.vote = { pollId: v.pollId.toLowerCase(), options };
  }
  if ("endPoll" in d) {
    if (!isUuid(d.endPoll)) return null;
    out.endPoll = d.endPoll.toLowerCase();
  }
  if ("storyReply" in d) {
    const r = d.storyReply as Record<string, unknown> | null;
    if (!r || typeof r !== "object" || !isUuid(r.authorId) || !isUuid(r.storyId)) return null;
    if (r.kind !== "image" && r.kind !== "video" && r.kind !== "text") return null;
    if (typeof r.preview !== "string" || typeof r.reaction !== "boolean") return null;
    out.storyReply = { authorId: r.authorId.toLowerCase(), storyId: r.storyId.toLowerCase(), kind: r.kind, preview: makePreview(r.preview).slice(0, 80), reaction: r.reaction };
  }
  if ("channelPost" in d) {
    const c = d.channelPost as Record<string, unknown> | null;
    if (!c || typeof c !== "object" || !isUuid(c.channelId) || !isUuid(c.postId) || typeof c.channelName !== "string") return null;
    const channelName = makePreview(c.channelName).slice(0, MAX_CHANNEL_NAME);
    if (!channelName) return null;
    out.channelPost = { channelId: c.channelId.toLowerCase(), postId: c.postId.toLowerCase(), channelName };
  }
  return out;
}
