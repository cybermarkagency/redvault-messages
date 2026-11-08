// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  cleanStickerText,
  MAX_STICKERS,
  newStickerId,
  newStoryId,
  newStoryKey,
  openAnonymousAnswer,
  openAnswer,
  openStoryMeta,
  openTotals,
  parseAnswer,
  parseStickers,
  parseStoryMeta,
  safeStoryLink,
  sealAnonymousAnswer,
  sealAnswer,
  sealStoryMeta,
  sealTotals,
  StoryCryptoError,
  type PollSticker,
  type QuestionSticker,
  type QuizSticker,
  type SliderSticker,
  type StoryMeta,
} from "../lib/storyCrypto.js";

beforeAll(async () => {
  await crypto.ready();
});

const place = { x: 0.5, y: 0.5, scale: 1, rotation: 0 };
const poll = (over: Partial<PollSticker> = {}): PollSticker => ({ ...place, id: newStickerId(), type: "poll", question: "Pizza or tacos?", options: ["Pizza", "Tacos"], results: "everyone", ...over });
const quiz = (over: Partial<QuizSticker> = {}): QuizSticker => ({ ...place, id: newStickerId(), type: "quiz", question: "2 + 2?", options: ["3", "4", "5"], correct: 1, results: "everyone", ...over });
const question = (over: Partial<QuestionSticker> = {}): QuestionSticker => ({ ...place, id: newStickerId(), type: "question", prompt: "Ask me anything", anonymous: false, ...over });
const slider = (over: Partial<SliderSticker> = {}): SliderSticker => ({ ...place, id: newStickerId(), type: "slider", prompt: "How was your day?", emoji: "😍", results: "everyone", ...over });

describe("sticker details", () => {
  it("ids are random, in the server's format", () => {
    const ids = new Set(Array.from({ length: 200 }, newStickerId));
    expect(ids.size).toBe(200);
    for (const id of ids) expect(id).toMatch(/^[a-z0-9]{6,16}$/);
  });

  it("every kind of sticker survives sealing and opening with the story", () => {
    const stickers = [
      poll({ closesAt: Date.now() + 3600_000 }),
      quiz({ explanation: "Basic maths" }),
      question({ anonymous: true }),
      slider(),
      { ...place, id: newStickerId(), type: "countdown" as const, title: "Launch day", endsAt: Date.now() + 86_400_000 },
      { ...place, id: newStickerId(), type: "link" as const, url: "https://example.com/a?b=1", label: "My site" },
      { ...place, id: newStickerId(), type: "mention" as const, userId: "0f8fad5b-d9cb-469f-a165-70867728950e" },
      { ...place, id: newStickerId(), type: "date" as const, at: Date.UTC(2026, 11, 25), style: "date" as const },
      { ...place, id: newStickerId(), type: "location" as const, label: "Lisbon" },
    ];
    const meta: StoryMeta = { v: 1, kind: "text", text: "hello", background: "ocean", font: "modern", overlays: [], stickers, closeFriends: false };
    const id = newStoryId();
    const key = newStoryKey();
    const sealed = sealStoryMeta(id, key, meta);
    const opened = openStoryMeta(id, key, "text", sealed.ciphertext, sealed.nonce);
    expect(opened.stickers).toEqual(stickers);
  });

  it("a story without stickers (from before they existed) opens as it always did", () => {
    const meta: StoryMeta = { v: 1, kind: "text", text: "old", background: "sunset", font: "classic", overlays: [], closeFriends: false };
    const opened = parseStoryMeta(JSON.parse(JSON.stringify(meta)), "text");
    expect(opened.stickers).toBeUndefined();
    expect(opened.text).toBe("old");
  });

  it("drops what is malformed or from a newer app, and keeps the rest of the story", () => {
    const good = poll();
    const raw = [
      good,
      { ...good, question: "Same id, different question" },
      { ...poll(), options: ["only one"] },
      { ...poll(), options: ["a", "b", "c", "d", "e"] },
      { ...quiz(), correct: 7 },
      { ...poll(), id: "BAD ID!" },
      { ...poll(), x: 2 },
      { ...poll(), question: "" },
      { ...poll(), question: "x".repeat(121) },
      { ...slider(), emoji: "" },
      { type: "hologram", id: newStickerId(), ...place },
      "nope",
      null,
    ];
    const kept = parseStickers(raw);
    expect(kept.map((s) => s.id)).toEqual([good.id]);
    // The story around them still opens.
    const meta = parseStoryMeta({ v: 1, kind: "text", text: "hi", overlays: [], closeFriends: false, stickers: raw }, "text");
    expect(meta.stickers).toHaveLength(1);
  });

  it("allows at most MAX_STICKERS", () => {
    const many = Array.from({ length: 30 }, () => poll());
    expect(parseStickers(many)).toHaveLength(MAX_STICKERS);
  });

  it("sticker text is plain text: markup stays inert text and control or bidi characters are removed", () => {
    expect(cleanStickerText("<img src=x onerror=alert(1)>", 60)).toBe("<img src=x onerror=alert(1)>");
    expect(cleanStickerText("a\u0000b‮c\n\td", 60)).toBe("a b c d");
    expect(cleanStickerText("   ", 60)).toBeNull();
    expect(cleanStickerText("x".repeat(61), 60)).toBeNull();
    expect(cleanStickerText(42, 60)).toBeNull();
    const [kept] = parseStickers([poll({ question: "<script>alert(1)</script>" })]);
    expect((kept as PollSticker).question).toBe("<script>alert(1)</script>");
  });

  it("links: https only, no credentials, a real host, a sane length", () => {
    expect(safeStoryLink("https://example.com/path?q=1#x")).toBe("https://example.com/path?q=1#x");
    expect(safeStoryLink("  https://example.com  ")).toBe("https://example.com/");
    for (const bad of [
      "http://example.com",
      "javascript:alert(1)",
      "JaVaScRiPt:alert(1)",
      "data:text/html,<script>alert(1)</script>",
      "file:///etc/passwd",
      "ftp://example.com",
      "blob:https://example.com/x",
      "https://user:pass@example.com",
      "https://localhost",
      "https://",
      "//example.com",
      "example.com",
      "https://exa mple.com",
      `https://example.com/${"a".repeat(300)}`,
      42,
      null,
    ]) {
      expect(safeStoryLink(bad), String(bad)).toBeNull();
    }
    // A look-alike host is shown in the form the browser spells it, so it can't pass for the real one.
    expect(safeStoryLink("https://аpple.com")).toBe("https://xn--pple-43d.com/");
    // A link sticker with a bad URL is dropped, not shown.
    expect(parseStickers([{ ...place, id: newStickerId(), type: "link", url: "javascript:alert(1)", label: "Click" }])).toEqual([]);
  });

  it("a date or countdown needs a real time", () => {
    expect(parseStickers([{ ...place, id: newStickerId(), type: "countdown", title: "x", endsAt: "tomorrow" }])).toEqual([]);
    expect(parseStickers([{ ...place, id: newStickerId(), type: "date", at: -1, style: "date" }])).toEqual([]);
    expect(parseStickers([{ ...place, id: newStickerId(), type: "date", at: Date.now(), style: "weekday" }])).toEqual([]);
  });
});

describe("answers to stickers", () => {
  let author: crypto.KeyPair;
  let bob: crypto.KeyPair;
  let carol: crypto.KeyPair;
  let storyId: string;
  // After libsodium is ready (the file-level beforeAll): keys can't be made while tests are being collected.
  beforeAll(() => {
    author = crypto.generateKeyPair();
    bob = crypto.generateKeyPair();
    carol = crypto.generateKeyPair();
    storyId = newStoryId();
  });

  it("an answer opens for the author and the sender, and for nobody else", () => {
    const p = poll();
    const env = sealAnswer(storyId, p, { k: "poll", choice: 1 }, author.publicKey, bob.privateKey);
    expect(openAnswer(storyId, p, env, bob.publicKey, author.privateKey)).toEqual({ k: "poll", choice: 1 });
    // The sender can read their own answer again (a second device, a reload).
    expect(openAnswer(storyId, p, env, author.publicKey, bob.privateKey)).toEqual({ k: "poll", choice: 1 });
    // Nobody else.
    expect(() => openAnswer(storyId, p, env, bob.publicKey, carol.privateKey)).toThrow(StoryCryptoError);
    expect(() => openAnswer(storyId, p, env, carol.publicKey, author.privateKey)).toThrow(StoryCryptoError);
  });

  it("the author knows who sent an answer: it can't be passed off as someone else's", () => {
    const p = poll();
    const fromBob = sealAnswer(storyId, p, { k: "poll", choice: 0 }, author.publicKey, bob.privateKey);
    // A server relabelling Bob's answer as Carol's fails to open under Carol's key.
    expect(() => openAnswer(storyId, p, fromBob, carol.publicKey, author.privateKey)).toThrow(/wasn't made by that person/);
  });

  it("tampering is caught", () => {
    const q = question();
    const env = sealAnswer(storyId, q, { k: "question", text: "hello" }, author.publicKey, bob.privateKey);
    for (const index of [0, 10, env.length - 1]) {
      const bad = env.slice();
      bad[index] ^= 1;
      expect(() => openAnswer(storyId, q, bad, bob.publicKey, author.privateKey)).toThrow(StoryCryptoError);
    }
    expect(() => openAnswer(storyId, q, env.slice(0, env.length - 3), bob.publicKey, author.privateKey)).toThrow(StoryCryptoError);
    expect(() => openAnswer(storyId, q, new Uint8Array(10), bob.publicKey, author.privateKey)).toThrow(StoryCryptoError);
  });

  it("an answer can't be moved onto another story or another sticker", () => {
    const p1 = poll();
    const p2 = poll();
    const env = sealAnswer(storyId, p1, { k: "poll", choice: 0 }, author.publicKey, bob.privateKey);
    expect(() => openAnswer(storyId, p2, env, bob.publicKey, author.privateKey)).toThrow(/another sticker/);
    expect(() => openAnswer(newStoryId(), p1, env, bob.publicKey, author.privateKey)).toThrow(/another story/);
  });

  it("answers that don't fit their sticker are refused when sealing and when opening", () => {
    const p = poll();
    expect(() => sealAnswer(storyId, p, { k: "poll", choice: 2 }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    expect(() => sealAnswer(storyId, p, { k: "poll", choice: -1 }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    expect(() => sealAnswer(storyId, p, { k: "poll", choice: 0.5 }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    expect(() => sealAnswer(storyId, p, { k: "quiz", choice: 0 }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    expect(() => sealAnswer(storyId, slider(), { k: "slider", value: 101 }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    expect(() => sealAnswer(storyId, slider(), { k: "slider", value: 50.5 }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    expect(() => sealAnswer(storyId, question(), { k: "question", text: "  " }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    expect(() => sealAnswer(storyId, question(), { k: "question", text: "x".repeat(501) }, author.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    // An envelope for a 4-option poll can't be opened as a 2-option one with the same id.
    const wide = poll({ options: ["a", "b", "c", "d"] });
    const env = sealAnswer(storyId, wide, { k: "poll", choice: 3 }, author.publicKey, bob.privateKey);
    expect(() => openAnswer(storyId, { ...wide, options: ["a", "b"] }, env, bob.publicKey, author.privateKey)).toThrow(StoryCryptoError);
    expect(parseAnswer({ k: "slider", value: 0 }, slider())).toEqual({ k: "slider", value: 0 });
  });

  it("an answer without a name opens only for the author and holds nothing about the sender", () => {
    const q = question({ anonymous: true });
    const env = sealAnonymousAnswer(storyId, q, { k: "question", text: "who are you?" }, author.publicKey);
    expect(openAnonymousAnswer(storyId, q, env, author.publicKey, author.privateKey)).toEqual({ k: "question", text: "who are you?" });
    expect(() => openAnonymousAnswer(storyId, q, env, carol.publicKey, carol.privateKey)).toThrow(StoryCryptoError);
    // The sender cannot read it back either; the sealed box has no sender key in it at all.
    expect(() => openAnonymousAnswer(storyId, q, env, bob.publicKey, bob.privateKey)).toThrow(StoryCryptoError);
    // Two senders' envelopes are the same size and share no structure that names either.
    const a = sealAnonymousAnswer(storyId, q, { k: "question", text: "same words" }, author.publicKey);
    const b = sealAnonymousAnswer(storyId, q, { k: "question", text: "same words" }, author.publicKey);
    expect(a.length).toBe(b.length);
    expect(a).not.toEqual(b);
    // Moving it elsewhere fails here too.
    expect(() => openAnonymousAnswer(newStoryId(), q, env, author.publicKey, author.privateKey)).toThrow(/another story/);
    expect(() => openAnonymousAnswer(storyId, question({ anonymous: true }), env, author.publicKey, author.privateKey)).toThrow(/another sticker/);
  });

  it("an authenticated answer isn't accepted as an anonymous one, or the other way round", () => {
    const q = question();
    const named = sealAnswer(storyId, q, { k: "question", text: "hi" }, author.publicKey, bob.privateKey);
    const unnamed = sealAnonymousAnswer(storyId, q, { k: "question", text: "hi" }, author.publicKey);
    expect(() => openAnonymousAnswer(storyId, q, named, author.publicKey, author.privateKey)).toThrow(StoryCryptoError);
    expect(() => openAnswer(storyId, q, unnamed, bob.publicKey, author.privateKey)).toThrow(StoryCryptoError);
  });
});

describe("published totals", () => {
  let storyId: string;
  let key: Uint8Array;
  beforeAll(() => {
    storyId = newStoryId();
    key = newStoryKey();
  });

  it("round trip for a poll, a quiz and a slider", () => {
    const p = poll();
    const sealedPoll = sealTotals(storyId, p.id, 3, { total: 5, counts: [2, 3] }, key);
    expect(openTotals(storyId, p, 3, sealedPoll.ciphertext, sealedPoll.nonce, key)).toEqual({ total: 5, counts: [2, 3] });
    const qz = quiz();
    const sealedQuiz = sealTotals(storyId, qz.id, 1, { total: 4, counts: [1, 3, 0], correct: 3 }, key);
    expect(openTotals(storyId, qz, 1, sealedQuiz.ciphertext, sealedQuiz.nonce, key)).toEqual({ total: 4, counts: [1, 3, 0], correct: 3 });
    const sl = slider();
    const sealedSlider = sealTotals(storyId, sl.id, 2, { total: 2, average: 62.5 }, key);
    expect(openTotals(storyId, sl, 2, sealedSlider.ciphertext, sealedSlider.nonce, key)).toEqual({ total: 2, average: 62.5 });
  });

  it("is bound to its story, sticker and revision, and to the story key", () => {
    const p = poll();
    const sealed = sealTotals(storyId, p.id, 2, { total: 1, counts: [1, 0] }, key);
    const open = (over: { story?: string; sticker?: PollSticker; revision?: number; key?: Uint8Array }) =>
      openTotals(over.story ?? storyId, over.sticker ?? p, over.revision ?? 2, sealed.ciphertext, sealed.nonce, over.key ?? key);
    expect(open({}).total).toBe(1);
    expect(() => open({ story: newStoryId() })).toThrow(StoryCryptoError);
    expect(() => open({ sticker: poll() })).toThrow(StoryCryptoError);
    // A server can't pass old totals off as a newer revision, or the other way round.
    expect(() => open({ revision: 3 })).toThrow(StoryCryptoError);
    expect(() => open({ revision: 1 })).toThrow(StoryCryptoError);
    expect(() => open({ key: newStoryKey() })).toThrow(StoryCryptoError);
    const bad = sealed.ciphertext.slice();
    bad[0] ^= 1;
    expect(() => openTotals(storyId, p, 2, bad, sealed.nonce, key)).toThrow(StoryCryptoError);
  });

  it("totals that don't fit the sticker are refused", () => {
    const p = poll();
    const wrongLength = sealTotals(storyId, p.id, 1, { total: 3, counts: [1, 1, 1] }, key);
    expect(() => openTotals(storyId, p, 1, wrongLength.ciphertext, wrongLength.nonce, key)).toThrow(/malformed/);
    const negative = sealTotals(storyId, p.id, 1, { total: 3, counts: [-1, 4] }, key);
    expect(() => openTotals(storyId, p, 1, negative.ciphertext, negative.nonce, key)).toThrow(/malformed/);
    const sl = slider();
    const outOfRange = sealTotals(storyId, sl.id, 1, { total: 1, average: 140 }, key);
    expect(() => openTotals(storyId, sl, 1, outOfRange.ciphertext, outOfRange.nonce, key)).toThrow(/malformed/);
    expect(() => sealTotals(storyId, p.id, 0, { total: 0, counts: [0, 0] }, key)).toThrow(StoryCryptoError);
  });
});
