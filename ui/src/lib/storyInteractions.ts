// Interactive stickers on this device: answering, reading what the author published, and, for the
// author, opening the answers sealed to them and publishing totals. The crypto is in
// lib/storyCrypto.ts; the design and what the server can learn in docs/stories-advanced/.
import * as crypto from "./e2eCrypto.js";
import { ApiError } from "./api.js";
import * as sapi from "./storiesApi.js";
import type { Story, StoryFriend, StoryIdentity } from "./stories.js";
import {
  isAnswerable,
  openAnonymousAnswer,
  openAnswer,
  openTotals,
  sealAnonymousAnswer,
  sealAnswer,
  sealTotals,
  type AnswerableSticker,
  type StickerTotals,
  type StoryAnswer,
} from "./storyCrypto.js";

export function answerableStickers(story: Story): AnswerableSticker[] {
  return (story.meta.stickers ?? []).filter(isAnswerable);
}

function stickerById(story: Story, stickerId: string): AnswerableSticker | undefined {
  return answerableStickers(story).find((s) => s.id === stickerId);
}

/** A question marked "without my name" is sent with no sender inside the envelope. */
export const isAnonymousSticker = (s: AnswerableSticker): boolean => s.type === "question" && s.anonymous;

/** Quiz and question answers are final: a second one is refused by the server. Polls and sliders can be changed. */
export const isFinalSticker = (s: AnswerableSticker): boolean => s.type === "quiz" || s.type === "question";

/** Whether a poll still takes votes (the server accepts them until the story ends; this is the author's chosen deadline). */
export function isClosed(sticker: AnswerableSticker, now = Date.now()): boolean {
  return sticker.type === "poll" && sticker.closesAt !== undefined && now >= sticker.closesAt;
}

// --- The viewer's side ---------------------------------------------------------------------------

export interface MyAnswer {
  /** Null when it can't be read back: answers sent without a name can't be, even by their sender. */
  answer: StoryAnswer | null;
  anonymous: boolean;
}

export async function submitAnswer(identity: StoryIdentity, story: Story, authorPublicKey: Uint8Array, sticker: AnswerableSticker, answer: StoryAnswer): Promise<void> {
  await crypto.ready();
  const anonymous = isAnonymousSticker(sticker);
  const envelope = anonymous
    ? sealAnonymousAnswer(story.id, sticker, answer, authorPublicKey)
    : sealAnswer(story.id, sticker, answer, authorPublicKey, identity.privateKey);
  await sapi.putStoryAnswer(story.id, sticker.id, { envelope: crypto.toBase64(envelope), anonymous, final: isFinalSticker(sticker) });
}

/** What this account already answered on a story (so another device or a reload shows it). */
export async function loadMyAnswers(identity: StoryIdentity, story: Story, authorPublicKey: Uint8Array): Promise<Map<string, MyAnswer>> {
  await crypto.ready();
  const out = new Map<string, MyAnswer>();
  for (const row of await sapi.getMyStoryAnswers(story.id)) {
    const sticker = stickerById(story, row.stickerId);
    if (!sticker) continue;
    if (row.anonymous) {
      out.set(row.stickerId, { answer: null, anonymous: true });
      continue;
    }
    try {
      out.set(row.stickerId, { answer: openAnswer(story.id, sticker, crypto.fromBase64(row.envelope), authorPublicKey, identity.privateKey), anonymous: false });
    } catch {
      out.set(row.stickerId, { answer: null, anonymous: false });
    }
  }
  return out;
}

/** The totals the author published, for stickers that show results. Anything that doesn't verify is left out. */
export async function loadTotals(story: Story): Promise<Map<string, StickerTotals>> {
  await crypto.ready();
  const out = new Map<string, StickerTotals>();
  for (const row of await sapi.getStoryTotals(story.id)) {
    const sticker = stickerById(story, row.stickerId);
    if (!sticker || sticker.type === "question") continue;
    try {
      out.set(row.stickerId, openTotals(story.id, sticker, row.revision, crypto.fromBase64(row.ciphertext), crypto.fromBase64(row.nonce), story.key));
    } catch {
      // Forged, for another sticker, or malformed: not shown.
    }
  }
  return out;
}

// --- The author's side -----------------------------------------------------------------------------

export interface InboxEntry {
  sticker: AnswerableSticker;
  /** Null for an answer sent without a name. */
  responderId: string | null;
  answer: StoryAnswer;
  /** Epoch ms, to the minute. */
  at: number;
  revision: number;
  /** The sealed envelope as stored (base64): how an answer is pointed at when reporting it. */
  envelope: string;
}

export interface Inbox {
  entries: InboxEntry[];
  /** Answers that could not be opened or didn't fit their sticker (never counted). */
  unreadable: number;
}

/** Opens every answer sealed to you on one of your stories. */
export async function loadInbox(identity: StoryIdentity, story: Story, friends: Map<string, StoryFriend>): Promise<Inbox> {
  await crypto.ready();
  const rows = await sapi.getStoryAnswers(story.id);
  const entries: InboxEntry[] = [];
  let unreadable = 0;
  for (const row of rows) {
    const sticker = stickerById(story, row.stickerId);
    if (!sticker) {
      unreadable++;
      continue;
    }
    try {
      const envelope = crypto.fromBase64(row.envelope);
      let answer: StoryAnswer;
      if (row.anonymous || row.responderId === null) {
        answer = openAnonymousAnswer(story.id, sticker, envelope, identity.publicKey, identity.privateKey);
      } else {
        const responder = friends.get(row.responderId);
        if (!responder) throw new Error("not a friend");
        answer = openAnswer(story.id, sticker, envelope, responder.publicKey, identity.privateKey);
      }
      entries.push({ sticker, responderId: row.anonymous ? null : row.responderId, answer, at: new Date(row.respondedAt).getTime(), revision: row.revision, envelope: row.envelope });
    } catch {
      unreadable++;
    }
  }
  return { entries, unreadable };
}

/**
 * The totals for one sticker. A poll ignores answers sent after its deadline (to the minute: the server
 * keeps times only that precisely); a quiz counts only a first answer.
 */
export function tallyAnswers(sticker: AnswerableSticker, entries: InboxEntry[]): StickerTotals {
  const mine = entries.filter((e) => e.sticker.id === sticker.id);
  switch (sticker.type) {
    case "poll": {
      const counts = sticker.options.map(() => 0);
      let total = 0;
      for (const e of mine) {
        if (e.answer.k !== "poll") continue;
        if (sticker.closesAt !== undefined && e.at > sticker.closesAt) continue;
        counts[e.answer.choice]++;
        total++;
      }
      return { total, counts };
    }
    case "quiz": {
      const counts = sticker.options.map(() => 0);
      let total = 0;
      let correct = 0;
      for (const e of mine) {
        if (e.answer.k !== "quiz" || e.revision > 0) continue;
        counts[e.answer.choice]++;
        total++;
        if (e.answer.choice === sticker.correct) correct++;
      }
      return { total, counts, correct };
    }
    case "slider": {
      const values = mine.flatMap((e) => (e.answer.k === "slider" ? [e.answer.value] : []));
      return { total: values.length, average: values.length ? Math.round((values.reduce((a, b) => a + b, 0) / values.length) * 10) / 10 : 0 };
    }
    case "question":
      return { total: mine.filter((e) => e.answer.k === "question").length };
  }
}

const published = new Map<string, string>();

/**
 * Publishes the totals of your stickers that show results, sealed with the story's key. Only what
 * changed is sent. A second device publishing at the same moment just loses the race (409) and the
 * next round catches up. Returns how many stickers were updated.
 */
export async function publishTotals(story: Story, inbox: Inbox): Promise<number> {
  await crypto.ready();
  const shown = answerableStickers(story).filter((s) => s.type !== "question" && s.results === "everyone");
  if (shown.length === 0) return 0;
  const current = new Map((await sapi.getStoryTotals(story.id)).map((r) => [r.stickerId, r.revision]));
  let updated = 0;
  for (const sticker of shown) {
    const totals = tallyAnswers(sticker, inbox.entries);
    const fingerprint = JSON.stringify(totals);
    const cacheKey = `${story.id}:${sticker.id}`;
    if (published.get(cacheKey) === fingerprint && current.has(sticker.id)) continue;
    const revision = (current.get(sticker.id) ?? 0) + 1;
    const sealed = sealTotals(story.id, sticker.id, revision, totals, story.key);
    try {
      await sapi.putStoryTotals(story.id, sticker.id, { revision, nonce: crypto.toBase64(sealed.nonce), ciphertext: crypto.toBase64(sealed.ciphertext) });
      published.set(cacheKey, fingerprint);
      updated++;
    } catch (err) {
      if (!(err instanceof ApiError) || err.status !== 409) throw err;
    }
  }
  return updated;
}

/** Opens your answers and publishes totals for one of your stories. */
export async function syncTotals(identity: StoryIdentity, story: Story, friends: Map<string, StoryFriend>): Promise<Inbox> {
  const inbox = await loadInbox(identity, story, friends);
  await publishTotals(story, inbox);
  return inbox;
}

export function forgetPublishedTotals(): void {
  published.clear();
}
