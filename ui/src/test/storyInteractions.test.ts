import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import { ApiError } from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import * as sapi from "../lib/storiesApi.js";
import type { Story, StoryFriend, StoryIdentity } from "../lib/stories.js";
import {
  forgetPublishedTotals,
  isClosed,
  loadInbox,
  loadMyAnswers,
  loadTotals,
  publishTotals,
  submitAnswer,
  tallyAnswers,
  type InboxEntry,
} from "../lib/storyInteractions.js";
import {
  newStickerId,
  newStoryId,
  newStoryKey,
  openAnonymousAnswer,
  openAnswer,
  openTotals,
  sealAnonymousAnswer,
  sealAnswer,
  sealTotals,
  type PollSticker,
  type QuestionSticker,
  type QuizSticker,
  type SliderSticker,
  type StoryAnswer,
  type StorySticker,
} from "../lib/storyCrypto.js";

vi.mock("../lib/storiesApi.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/storiesApi.js")>()),
  putStoryAnswer: vi.fn(async () => ({ revision: 0 })),
  getStoryAnswers: vi.fn(),
  getMyStoryAnswers: vi.fn(),
  getStoryTotals: vi.fn(),
  putStoryTotals: vi.fn(async () => undefined),
}));

type Keys = { publicKey: Uint8Array; privateKey: Uint8Array };
let author: StoryIdentity;
let bob: StoryIdentity;
let carol: StoryIdentity;
let mallory: Keys;
const AUTHOR = "00000000-0000-4000-8000-00000000000a";
const BOB = "00000000-0000-4000-8000-00000000000b";
const CAROL = "00000000-0000-4000-8000-00000000000c";
const MALLORY = "00000000-0000-4000-8000-00000000000d";

beforeAll(async () => {
  await crypto.ready();
  author = { userId: AUTHOR, ...crypto.generateKeyPair() };
  bob = { userId: BOB, ...crypto.generateKeyPair() };
  carol = { userId: CAROL, ...crypto.generateKeyPair() };
  mallory = crypto.generateKeyPair();
});

beforeEach(() => {
  vi.mocked(sapi.putStoryAnswer).mockClear();
  vi.mocked(sapi.putStoryTotals).mockReset().mockResolvedValue(undefined);
  vi.mocked(sapi.getStoryTotals).mockReset().mockResolvedValue([]);
  vi.mocked(sapi.getStoryAnswers).mockReset().mockResolvedValue([]);
  vi.mocked(sapi.getMyStoryAnswers).mockReset().mockResolvedValue([]);
  forgetPublishedTotals();
});

const place = { x: 0.5, y: 0.5, scale: 1, rotation: 0 };
const poll = (over: Partial<PollSticker> = {}): PollSticker => ({ ...place, id: newStickerId(), type: "poll", question: "A or B?", options: ["A", "B", "C"], results: "everyone", ...over });
const quiz = (over: Partial<QuizSticker> = {}): QuizSticker => ({ ...place, id: newStickerId(), type: "quiz", question: "2+2", options: ["3", "4"], correct: 1, results: "everyone", ...over });
const slider = (over: Partial<SliderSticker> = {}): SliderSticker => ({ ...place, id: newStickerId(), type: "slider", prompt: "Mood?", emoji: "😍", results: "everyone", ...over });
const question = (over: Partial<QuestionSticker> = {}): QuestionSticker => ({ ...place, id: newStickerId(), type: "question", prompt: "Ask me", anonymous: false, ...over });

function storyWith(stickers: StorySticker[]): Story {
  return {
    id: newStoryId(),
    authorId: AUTHOR,
    kind: "text",
    meta: { v: 1, kind: "text", text: "hi", background: "sunset", font: "classic", overlays: [], stickers, closeFriends: false },
    key: newStoryKey(),
    createdAt: Date.now(),
    expiresAt: Date.now() + 3600_000,
    allowReplies: true,
    hasMedia: false,
    seen: false,
  };
}

const friend = (who: StoryIdentity, handle: string): StoryFriend => ({ userId: who.userId, handle, avatarUrl: null, publicKey: who.publicKey });
const friends = new Map<string, StoryFriend>();
beforeAll(() => {
  friends.set(BOB, friend(bob, "bob"));
  friends.set(CAROL, friend(carol, "carol"));
});

const b64 = crypto.toBase64;
const row = (stickerId: string, responderId: string | null, envelope: Uint8Array, over: Partial<sapi.AnswerRowDto> = {}): sapi.AnswerRowDto => ({
  stickerId,
  responderId,
  anonymous: responderId === null,
  envelope: b64(envelope),
  revision: 0,
  respondedAt: new Date().toISOString(),
  ...over,
});

describe("answering", () => {
  it("a poll vote is sealed so the author (and the voter) can read it, and may be changed", async () => {
    const p = poll();
    const story = storyWith([p]);
    await submitAnswer(bob, story, author.publicKey, p, { k: "poll", choice: 2 });
    const [storyId, stickerId, body] = vi.mocked(sapi.putStoryAnswer).mock.calls[0];
    expect([storyId, stickerId]).toEqual([story.id, p.id]);
    expect(body).toMatchObject({ anonymous: false, final: false });
    const envelope = crypto.fromBase64(body.envelope);
    expect(openAnswer(story.id, p, envelope, bob.publicKey, author.privateKey)).toEqual({ k: "poll", choice: 2 });
    // The server holds an envelope it cannot open: only the two keys can.
    expect(() => openAnswer(story.id, p, envelope, bob.publicKey, mallory.privateKey)).toThrow();
    expect(JSON.stringify(body)).not.toContain("choice");
  });

  it("quiz and question answers are final, polls and sliders aren't", async () => {
    const story = storyWith([poll(), quiz(), slider(), question()]);
    const [p, qz, sl, qn] = story.meta.stickers as [PollSticker, QuizSticker, SliderSticker, QuestionSticker];
    await submitAnswer(bob, story, author.publicKey, p, { k: "poll", choice: 0 });
    await submitAnswer(bob, story, author.publicKey, qz, { k: "quiz", choice: 1 });
    await submitAnswer(bob, story, author.publicKey, sl, { k: "slider", value: 80 });
    await submitAnswer(bob, story, author.publicKey, qn, { k: "question", text: "hello" });
    expect(vi.mocked(sapi.putStoryAnswer).mock.calls.map((c) => c[2].final)).toEqual([false, true, false, true]);
  });

  it("a question marked without a name carries no sender: only the author can open it, and the voter can't read it back", async () => {
    const q = question({ anonymous: true });
    const story = storyWith([q]);
    await submitAnswer(bob, story, author.publicKey, q, { k: "question", text: "I like your stories" });
    const body = vi.mocked(sapi.putStoryAnswer).mock.calls[0][2];
    expect(body).toMatchObject({ anonymous: true, final: true });
    const envelope = crypto.fromBase64(body.envelope);
    expect(openAnonymousAnswer(story.id, q, envelope, author.publicKey, author.privateKey)).toEqual({ k: "question", text: "I like your stories" });
    // Not readable as a named answer, and not by Bob.
    expect(() => openAnswer(story.id, q, envelope, bob.publicKey, author.privateKey)).toThrow();
    expect(() => openAnonymousAnswer(story.id, q, envelope, bob.publicKey, bob.privateKey)).toThrow();
    // His own app knows he answered (the server says so) without being able to show what.
    vi.mocked(sapi.getMyStoryAnswers).mockResolvedValue([{ stickerId: q.id, anonymous: true, envelope: body.envelope, revision: 0 }]);
    expect((await loadMyAnswers(bob, story, author.publicKey)).get(q.id)).toEqual({ answer: null, anonymous: true });
  });

  it("what you already sent is read back from the server for this device", async () => {
    const p = poll();
    const story = storyWith([p]);
    const env = sealAnswer(story.id, p, { k: "poll", choice: 1 }, author.publicKey, bob.privateKey);
    vi.mocked(sapi.getMyStoryAnswers).mockResolvedValue([
      { stickerId: p.id, anonymous: false, envelope: b64(env), revision: 0 },
      // A sticker this story doesn't have, or an envelope that doesn't open: ignored / shown as answered only.
      { stickerId: newStickerId(), anonymous: false, envelope: b64(env), revision: 0 },
    ]);
    const mine = await loadMyAnswers(bob, story, author.publicKey);
    expect(mine.size).toBe(1);
    expect(mine.get(p.id)).toEqual({ answer: { k: "poll", choice: 1 }, anonymous: false });
    vi.mocked(sapi.getMyStoryAnswers).mockResolvedValue([{ stickerId: p.id, anonymous: false, envelope: b64(new Uint8Array(100)), revision: 0 }]);
    expect((await loadMyAnswers(bob, story, author.publicKey)).get(p.id)).toEqual({ answer: null, anonymous: false });
  });

  it("a poll past its deadline is closed", () => {
    expect(isClosed(poll({ closesAt: Date.now() - 1 }))).toBe(true);
    expect(isClosed(poll({ closesAt: Date.now() + 60_000 }))).toBe(false);
    expect(isClosed(poll())).toBe(false);
    expect(isClosed(quiz())).toBe(false);
  });
});

describe("the author's inbox", () => {
  it("opens named and unnamed answers, and counts what can't be opened without ever counting it", async () => {
    const p = poll();
    const q = question({ anonymous: true });
    const story = storyWith([p, q]);
    const fromBob = sealAnswer(story.id, p, { k: "poll", choice: 0 }, author.publicKey, bob.privateKey);
    const fromCarolAnon = sealAnonymousAnswer(story.id, q, { k: "question", text: "who are you?" }, author.publicKey);
    // Mallory (not a friend) and a relabelled answer (Carol's, shown as Bob's) and a damaged one.
    const fromMallory = sealAnswer(story.id, p, { k: "poll", choice: 1 }, author.publicKey, mallory.privateKey);
    const carolsPoll = sealAnswer(story.id, p, { k: "poll", choice: 2 }, author.publicKey, carol.privateKey);
    const damaged = fromBob.slice();
    damaged[damaged.length - 1] ^= 1;
    vi.mocked(sapi.getStoryAnswers).mockResolvedValue([
      row(p.id, BOB, fromBob),
      row(q.id, null, fromCarolAnon),
      row(p.id, MALLORY, fromMallory),
      row(p.id, BOB, carolsPoll),
      row(p.id, BOB, damaged),
      row(newStickerId(), BOB, fromBob),
    ]);
    const inbox = await loadInbox(author, story, friends);
    expect(inbox.entries.map((e) => [e.sticker.id, e.responderId, e.answer])).toEqual([
      [p.id, BOB, { k: "poll", choice: 0 }],
      [q.id, null, { k: "question", text: "who are you?" }],
    ]);
    expect(inbox.unreadable).toBe(4);
    expect(tallyAnswers(p, inbox.entries)).toEqual({ total: 1, counts: [1, 0, 0] });
  });

  it("a server that labels an answer as someone else's gets it rejected, not counted", async () => {
    const p = poll();
    const story = storyWith([p]);
    const fromCarol = sealAnswer(story.id, p, { k: "poll", choice: 0 }, author.publicKey, carol.privateKey);
    vi.mocked(sapi.getStoryAnswers).mockResolvedValue([row(p.id, BOB, fromCarol)]);
    const inbox = await loadInbox(author, story, friends);
    expect(inbox.entries).toEqual([]);
    expect(inbox.unreadable).toBe(1);
  });
});

describe("totals", () => {
  const entry = (sticker: PollSticker | QuizSticker | SliderSticker | QuestionSticker, answer: StoryAnswer, over: Partial<InboxEntry> = {}): InboxEntry => ({
    sticker,
    responderId: BOB,
    answer,
    at: Date.now(),
    revision: 0,
    envelope: "",
    ...over,
  });

  it("a poll counts each option, and ignores votes sent after its deadline", () => {
    const closes = Date.now() - 3600_000;
    const p = poll({ closesAt: closes });
    const totals = tallyAnswers(p, [
      entry(p, { k: "poll", choice: 0 }, { at: closes - 60_000 }),
      entry(p, { k: "poll", choice: 0 }, { at: closes }),
      entry(p, { k: "poll", choice: 2 }, { at: closes + 120_000 }),
    ]);
    expect(totals).toEqual({ total: 2, counts: [2, 0, 0] });
  });

  it("a quiz counts right answers, and only first answers", () => {
    const qz = quiz();
    const totals = tallyAnswers(qz, [
      entry(qz, { k: "quiz", choice: 1 }),
      entry(qz, { k: "quiz", choice: 0 }),
      // A replaced answer (the server says revision 1) is not a first answer: never counted.
      entry(qz, { k: "quiz", choice: 1 }, { revision: 1 }),
    ]);
    expect(totals).toEqual({ total: 2, counts: [1, 1], correct: 1 });
  });

  it("a slider averages; a question just counts", () => {
    const sl = slider();
    expect(tallyAnswers(sl, [entry(sl, { k: "slider", value: 100 }), entry(sl, { k: "slider", value: 51 })])).toEqual({ total: 2, average: 75.5 });
    expect(tallyAnswers(sl, [])).toEqual({ total: 0, average: 0 });
    const qn = question();
    expect(tallyAnswers(qn, [entry(qn, { k: "question", text: "a" }), entry(qn, { k: "question", text: "b" })])).toEqual({ total: 2 });
  });

  it("answers to other stickers are not mixed in", () => {
    const a = poll();
    const b = poll();
    expect(tallyAnswers(a, [entry(a, { k: "poll", choice: 1 }), entry(b, { k: "poll", choice: 1 })])).toEqual({ total: 1, counts: [0, 1, 0] });
  });
});

describe("publishing totals", () => {
  const inboxOf = (entries: InboxEntry[]) => ({ entries, unreadable: 0 });

  it("seals only the totals with the story key, as the next revision", async () => {
    const p = poll();
    const story = storyWith([p]);
    const entries: InboxEntry[] = [{ sticker: p, responderId: BOB, answer: { k: "poll", choice: 1 }, at: Date.now(), revision: 0, envelope: "" }];
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([{ stickerId: p.id, revision: 4, nonce: "", ciphertext: "" }]);
    expect(await publishTotals(story, inboxOf(entries))).toBe(1);
    const [storyId, stickerId, body] = vi.mocked(sapi.putStoryTotals).mock.calls[0];
    expect([storyId, stickerId, body.revision]).toEqual([story.id, p.id, 5]);
    expect(openTotals(story.id, p, 5, crypto.fromBase64(body.ciphertext), crypto.fromBase64(body.nonce), story.key)).toEqual({ total: 1, counts: [0, 1, 0] });
    // Who voted is nowhere in what was sent.
    expect(JSON.stringify(body)).not.toContain(BOB);
  });

  it("sends only what changed", async () => {
    const p = poll();
    const story = storyWith([p]);
    const one: InboxEntry = { sticker: p, responderId: BOB, answer: { k: "poll", choice: 0 }, at: Date.now(), revision: 0, envelope: "" };
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([{ stickerId: p.id, revision: 1, nonce: "", ciphertext: "" }]);
    expect(await publishTotals(story, inboxOf([one]))).toBe(1);
    expect(await publishTotals(story, inboxOf([one]))).toBe(0);
    expect(await publishTotals(story, inboxOf([one, { ...one, responderId: CAROL }]))).toBe(1);
    expect(sapi.putStoryTotals).toHaveBeenCalledTimes(2);
  });

  it("leaves out stickers whose results only you see, and questions", async () => {
    const story = storyWith([poll({ results: "author" }), question(), poll()]);
    await publishTotals(story, inboxOf([]));
    expect(sapi.putStoryTotals).toHaveBeenCalledTimes(1);
    expect(vi.mocked(sapi.putStoryTotals).mock.calls[0][1]).toBe((story.meta.stickers![2] as PollSticker).id);
    expect(await publishTotals(storyWith([question()]), inboxOf([]))).toBe(0);
    expect(await publishTotals(storyWith([]), inboxOf([]))).toBe(0);
  });

  it("losing a race to another device is fine; other failures are not", async () => {
    const story = storyWith([poll()]);
    vi.mocked(sapi.putStoryTotals).mockRejectedValueOnce(new ApiError(409, "stale_revision", "out of date"));
    expect(await publishTotals(story, inboxOf([]))).toBe(0);
    vi.mocked(sapi.putStoryTotals).mockRejectedValueOnce(new ApiError(500, "boom", "boom"));
    await expect(publishTotals(story, inboxOf([]))).rejects.toThrow("boom");
  });

  it("a viewer reads totals the author published, and nothing forged", async () => {
    const p = poll();
    const sl = slider({ results: "author" });
    const story = storyWith([p, sl]);
    const good = sealTotals(story.id, p.id, 2, { total: 3, counts: [1, 1, 1] }, story.key);
    const forged = sealTotals(story.id, p.id, 3, { total: 99, counts: [99, 0, 0] }, newStoryKey());
    const forSlider = sealTotals(story.id, sl.id, 1, { total: 1, average: 10 }, story.key);
    const rows = (revision: number, s: { ciphertext: Uint8Array; nonce: Uint8Array }, stickerId = p.id) => ({ stickerId, revision, nonce: b64(s.nonce), ciphertext: b64(s.ciphertext) });
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([rows(2, good)]);
    expect((await loadTotals(story)).get(p.id)).toEqual({ total: 3, counts: [1, 1, 1] });
    // Sealed with another key, or served as a different revision than it was sealed for: dropped.
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([rows(3, forged)]);
    expect((await loadTotals(story)).size).toBe(0);
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([rows(5, good)]);
    expect((await loadTotals(story)).size).toBe(0);
    // A sticker the story doesn't have.
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([rows(2, good, newStickerId())]);
    expect((await loadTotals(story)).size).toBe(0);
    // Whatever the server serves for a sticker marked "only me" is still opened (the key is shared), so the app is the one that hides it.
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([rows(1, forSlider, sl.id)]);
    expect((await loadTotals(story)).get(sl.id)).toEqual({ total: 1, average: 10 });
  });
});
