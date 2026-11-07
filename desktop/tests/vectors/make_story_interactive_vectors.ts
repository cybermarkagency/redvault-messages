// Produces story_interactive_vectors.json with the web app's own code, so the desktop
// app's sticker parsing, sealed answers and sealed totals are pinned to it byte for byte.
// Not part of any build: copy it into ui/src/test/ as makeStoryInteractiveVectors.test.ts,
// run
//
//   cd ui && npx vitest run src/test/makeStoryInteractiveVectors.test.ts
//
// and delete the copy. The output lands in desktop/tests/vectors/ (next to this file).
//
// Everything is fixed (ids, keys, times): nothing depends on the clock. Strings with
// control or bidi characters are built with String.fromCharCode so no editor or shell
// can turn an escape into an invisible character on the way.
import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import { tallyAnswers, type InboxEntry } from "../lib/storyInteractions.js";
import {
  cleanStickerText,
  openAnonymousAnswer,
  openAnswer,
  openStoryMeta,
  openTotals,
  parseStickers,
  safeStoryLink,
  sealAnonymousAnswer,
  sealAnswer,
  sealStoryMeta,
  sealTotals,
  type AnswerableSticker,
  type StoryAnswer,
  type StoryMeta,
} from "../lib/storyCrypto.js";

const OUT = resolve(process.cwd(), "../desktop/tests/vectors/story_interactive_vectors.json");

const ch = (...codes: number[]) => String.fromCharCode(...codes);
const NBSP = ch(0xa0);
const RLO = ch(0x202e);
const PDF = ch(0x202c);
const LRI = ch(0x2066);
const PDI = ch(0x2069);
const LS = ch(0x2028);
const BOM = ch(0xfeff);
const ZWSP = ch(0x200b);
const LRM = ch(0x200e);
const NEL = ch(0x85);
const NUL = ch(0);
const TAB = ch(9);

it("writes the vectors", async () => {
  const sodium = await crypto.ready();
  const b64 = crypto.toBase64;
  const pair = (seedByte: number) => {
    const kp = sodium.crypto_box_seed_keypair(new Uint8Array(32).fill(seedByte));
    return { publicKey: kp.publicKey, privateKey: kp.privateKey };
  };
  const alice = pair(11); // the author
  const bob = pair(12);
  const carol = pair(13);
  const copy = (s: string) => {
    const src = sodium.from_string(s);
    const out = new Uint8Array(src.length);
    out.set(src);
    return out;
  };

  const storyId = "7d3f2a10-5b6c-4e8d-9a1b-2c3d4e5f6a7b";
  const key = new Uint8Array(32).map((_, i) => 255 - i);

  // --- 1. Stickers ---------------------------------------------------------------------------
  const place = { x: 0.5, y: 0.5, scale: 1, rotation: 0 };
  const at = (n: number) => ({ x: 0.1 * n, y: 0.2 + 0.05 * n, scale: 1 + n / 10, rotation: 15 * n });
  const raw = {
    poll: { id: "pollabc123", type: "poll", ...at(1), question: "Pizza or tacos? 🌮", options: ["Pizza", "Tacos", "Both"], results: "everyone", closesAt: 1_900_000_000_000 },
    quiz: { id: "quizabc123", type: "quiz", ...at(2), question: "2 + 2?", options: ["3", "4", "5"], correct: 1, results: "author", explanation: "Basic maths" },
    question: { id: "questabc12", type: "question", ...at(3), prompt: "Ask me anything", anonymous: true },
    slider: { id: "sliderabc1", type: "slider", ...at(4), prompt: "How was your day?", emoji: "🔥", results: "everyone" },
    countdown: { id: "countabc12", type: "countdown", ...at(5), title: "Launch day", endsAt: 1_900_086_400_000 },
    link: { id: "linkabc123", type: "link", ...at(6), url: "https://Example.com/Path?x=1#top", label: "My site" },
    mention: { id: "mentionabc", type: "mention", ...at(7), userId: "0F8FAD5B-D9CB-469F-A165-70867728950E" },
    date: { id: "dateabc123", type: "date", ...at(8), at: 1_900_000_000_000, style: "time" },
    location: { id: "placeabc12", type: "location", ...at(9), label: `  Lisbon ${NBSP}${NBSP} Portugal ` },
  };
  const everything = Object.values(raw);
  const manyValid = Array.from({ length: 14 }, (_, i) => ({ id: `place${String(i).padStart(6, "0")}`, type: "location", ...place, label: `Spot ${i}` }));
  const long120 = "😍".repeat(60);
  const parseCases: { name: string; raw: unknown; expected: unknown }[] = [
    { name: "every kind of sticker", raw: everything },
    {
      name: "bad ones are dropped one by one, the good ones stay",
      raw: [
        { ...raw.poll },
        { id: "gifabc1234", type: "gif", ...place, url: "https://example.com/a.gif" },
        { ...raw.quiz, id: "quizbad001", correct: 3 },
        { ...raw.quiz, id: "quizbad002", correct: 1.5 },
        { ...raw.quiz, id: "quizbad003", correct: -1 },
        { ...raw.poll, id: "pollbad001", options: ["only one"] },
        { ...raw.poll, id: "pollbad002", options: ["a", "b", "c", "d", "e"] },
        { ...raw.poll, id: "pollbad003", options: ["a", "x".repeat(41)] },
        { ...raw.poll, id: "pollbad004", options: ["a", 5] },
        { ...raw.poll, id: "pollbad005", options: "ab" },
        { ...raw.poll, id: "pollbad006", question: "   " },
        { ...raw.question, id: "questbad01", prompt: "x".repeat(121) },
        { ...raw.slider, id: "sliderbad1", emoji: "" },
        { ...raw.slider, id: "sliderbad2", emoji: "123456789" },
        { ...raw.countdown, id: "countbad01", endsAt: 0 },
        { ...raw.countdown, id: "countbad02", endsAt: 9e15 },
        { ...raw.countdown, id: "countbad03", title: "x".repeat(61) },
        { ...raw.link, id: "linkbad001", url: "http://example.com/" },
        { ...raw.link, id: "linkbad002", url: "https://user:pw@example.com/" },
        { ...raw.link, id: "linkbad003", url: "javascript:alert(1)" },
        { ...raw.link, id: "linkbad004", url: "https://localhost/" },
        { ...raw.link, id: "linkbad005", label: "" },
        { ...raw.mention, id: "mentionbad", userId: "not-a-uuid" },
        { ...raw.date, id: "datebad001", style: "weekday" },
        { ...raw.date, id: "datebad002", at: -5 },
        { ...raw.location, id: "placebad01", label: "y".repeat(61) },
        { ...raw.location, id: "short", label: "id too short" },
        { ...raw.location, id: "UPPERCASE01", label: "id uppercase" },
        { ...raw.location, id: "waytoolongidwaytoolong", label: "id too long" },
        { ...raw.location, id: "placebad02a", x: 1.5 },
        { ...raw.location, id: "placebad02b", y: -0.1 },
        { ...raw.location, id: "placebad02c", x: "0.5" },
        "not an object",
        null,
        42,
        [raw.poll],
      ],
    },
    { name: "a duplicate id keeps the first one", raw: [raw.location, { ...raw.location, label: "Second" }, { ...raw.poll, id: raw.location.id }] },
    { name: "only the first twelve are looked at", raw: manyValid },
    { name: "the cut is made before parsing", raw: [...Array.from({ length: 12 }, (_, i) => ({ id: `bad${String(i).padStart(7, "0")}`, type: "nope" })), raw.poll] },
    { name: "not an array", raw: { id: "pollabc123", type: "poll" } },
    { name: "missing", raw: null },
    {
      name: "size and angle are brought into range",
      raw: [
        { ...raw.location, id: "range00001", scale: 9, rotation: -30 },
        { ...raw.location, id: "range00002", scale: 0.1, rotation: 725 },
        { ...raw.location, id: "range00003", scale: "big", rotation: "spin" },
        { id: "range00004", type: "location", x: 0.3, y: 0.3, label: "no size at all" },
        { ...raw.location, id: "range00005", scale: 1.5, rotation: 360 },
      ],
    },
    {
      name: "text is cleaned the same way",
      raw: [
        { ...raw.poll, id: "clean00001", question: `${RLO}Win${PDF} a ${LRI}prize${PDI}${NUL}?`, options: [`  one${TAB}two  `, `a${NBSP}${NBSP}b`] },
        { ...raw.question, id: "clean00002", prompt: `${BOM}hello${LS}there ${ZWSP}${LRM}` },
        { ...raw.slider, id: "clean00003", prompt: long120, emoji: "❤️" },
        { ...raw.slider, id: "clean00004", prompt: `${long120}x` },
      ],
    },
    { name: "a quiz keeps going without a bad explanation", raw: [{ ...raw.quiz, id: "quizexp001", explanation: "x".repeat(121) }, { ...raw.quiz, id: "quizexp002", explanation: null }] },
  ];
  for (const c of parseCases) c.expected = parseStickers(c.raw);
  expect(parseCases[0].expected).toHaveLength(9);
  expect(parseCases[3].expected).toHaveLength(12);
  expect(parseCases[4].expected).toHaveLength(0);
  expect(parseCases[5].expected).toHaveLength(0);

  const cleanInputs: { input: unknown; max: number }[] = [
    { input: "  hello   world  ", max: 120 },
    { input: `a${NUL}b`, max: 40 },
    { input: `tab${TAB}here`, max: 40 },
    { input: `${RLO}evil${PDF}`, max: 40 },
    { input: `x${LRI}y${PDI}z`, max: 40 },
    { input: `a${NBSP}${NBSP}b`, max: 40 },
    { input: `a${LS}b`, max: 40 },
    { input: `${BOM}a`, max: 40 },
    { input: `a${ZWSP}b`, max: 40 },
    { input: `a${LRM}b`, max: 40 },
    { input: `a${NEL}b`, max: 40 },
    { input: "😍".repeat(60), max: 120 },
    { input: "😍".repeat(61), max: 120 },
    { input: "x".repeat(40), max: 40 },
    { input: "x".repeat(41), max: 40 },
    { input: "   ", max: 40 },
    { input: "", max: 40 },
    { input: 5, max: 40 },
    { input: null, max: 40 },
    { input: ["a"], max: 40 },
    { input: "Zażółć gęślą jaźń — ñandú 日本語", max: 120 },
  ];
  const clean = cleanInputs.map((c) => ({ ...c, expected: cleanStickerText(c.input, c.max) }));

  const linkInputs: string[] = [
    "https://example.com",
    "https://Example.COM/Path?Q=1#Frag",
    "  https://example.com/a  ",
    "https://example.com:443/x",
    "https://example.com:8443/x",
    "https://example.com:0443/x",
    "HTTPS://EXAMPLE.COM/",
    "https://example.com?x=1",
    "https://example.com#top",
    "https://example.com/?",
    "https://example.com/a/b/c.html?q=%20x&r=2#frag%20x",
    "https://sub.domain.example.co.uk/",
    "https://127.0.0.1/",
    "https://xn--bcher-kva.example/",
    "http://example.com",
    "ftp://example.com",
    "javascript:alert(1)",
    "data:text/html,hi",
    "https://user:pw@example.com",
    "https://user@example.com",
    "https://localhost",
    "https://example",
    "https://",
    "https:example.com",
    "//example.com",
    "example.com",
    "https://exa mple.com",
    "",
    "https://example.com/" + "a".repeat(300),
    "https://example.com/" + "a".repeat(270),
  ];
  const links = linkInputs.map((input) => ({ input, expected: safeStoryLink(input) }));

  // A story that carries them all, sealed with a fixed key: the whole path from the wire.
  const meta: StoryMeta = { v: 1, kind: "text", text: "Pick one!", background: "ocean", font: "modern", overlays: [], stickers: parseStickers(everything), closeFriends: false };
  const sealedMeta = sealStoryMeta(storyId, key, meta);
  expect(openStoryMeta(storyId, key, "text", sealedMeta.ciphertext, sealedMeta.nonce).stickers).toHaveLength(9);
  // The same story as an author's app might send it (unclean values the reader must tidy), sealed.
  const sloppyMeta = { v: 1, kind: "text", text: "Pick one!", background: "ocean", font: "modern", overlays: [], stickers: [...everything, { id: "gifabc1234", type: "gif" }], closeFriends: false };
  const sloppyKey = new Uint8Array(32).fill(42);
  const sloppySealed = sealStoryMeta(storyId, sloppyKey, sloppyMeta as unknown as StoryMeta);

  // --- 2. Answers ----------------------------------------------------------------------------
  const sticker = (name: keyof typeof raw): AnswerableSticker => parseStickers([raw[name]])[0] as AnswerableSticker;
  const pollS = sticker("poll");
  const quizS = sticker("quiz");
  const questionS = sticker("question");
  const sliderS = sticker("slider");

  const named: { name: string; stickerId: string; responder: "bob" | "carol"; sent: StoryAnswer; expected: StoryAnswer; envelope: string }[] = [];
  const addNamed = (name: string, s: AnswerableSticker, who: "bob" | "carol", sent: StoryAnswer, expected?: StoryAnswer) => {
    const sk = who === "bob" ? bob.privateKey : carol.privateKey;
    const envelope = sealAnswer(storyId, s, sent, alice.publicKey, sk);
    const pk = who === "bob" ? bob.publicKey : carol.publicKey;
    const opened = openAnswer(storyId, s, envelope, pk, alice.privateKey);
    expect(opened).toEqual(expected ?? sent);
    // The sender opens their own answer again with the author's public key.
    expect(openAnswer(storyId, s, envelope, alice.publicKey, sk)).toEqual(expected ?? sent);
    named.push({ name, stickerId: s.id, responder: who, sent, expected: expected ?? sent, envelope: b64(envelope) });
  };
  addNamed("poll choice 2", pollS, "bob", { k: "poll", choice: 2 });
  addNamed("poll choice 0 from someone else", pollS, "carol", { k: "poll", choice: 0 });
  addNamed("quiz right answer", quizS, "bob", { k: "quiz", choice: 1 });
  addNamed("slider at 73", sliderS, "bob", { k: "slider", value: 73 });
  addNamed("slider at the ends", sliderS, "carol", { k: "slider", value: 0 });
  addNamed("question with emoji, trimmed", questionS, "bob", { k: "question", text: `  Hi 👋 ${NBSP}there!${NBSP} ` }, { k: "question", text: `Hi 👋 ${NBSP}there!` });
  addNamed("question at 500 characters", questionS, "carol", { k: "question", text: "q".repeat(500) });

  const anonEnvelope = sealAnonymousAnswer(storyId, questionS, { k: "question", text: "  who are you?  " }, alice.publicKey);
  expect(openAnonymousAnswer(storyId, questionS, anonEnvelope, alice.publicKey, alice.privateKey)).toEqual({ k: "question", text: "who are you?" });
  const anonymous = [{ name: "question without a name", stickerId: questionS.id, expected: { k: "question", text: "who are you?" }, envelope: b64(anonEnvelope) }];

  // Hand-made payloads: the layout written out here is independent of lib/storyCrypto.ts.
  const idBytes = (id: string) => {
    const hex = id.replace(/-/g, "");
    return Uint8Array.from(hex.match(/../g)!.map((h) => parseInt(h, 16)));
  };
  function payload(magic: string, version: number, forStory: string, stickerId: string, body: string): Uint8Array {
    const sid = copy(stickerId);
    const json = copy(body);
    const out = new Uint8Array(4 + 1 + 16 + 1 + sid.length + json.length);
    out.set(copy(magic), 0);
    out[4] = version;
    out.set(idBytes(forStory), 5);
    out[21] = sid.length;
    out.set(sid, 22);
    out.set(json, 22 + sid.length);
    return out;
  }
  const boxNamed = (p: Uint8Array, from = bob) => {
    const nonce = sodium.randombytes_buf(sodium.crypto_box_NONCEBYTES);
    const box = sodium.crypto_box_easy(p, nonce, alice.publicKey, from.privateKey);
    const out = new Uint8Array(nonce.length + box.length);
    out.set(nonce, 0);
    out.set(box, nonce.length);
    return b64(out);
  };
  const boxAnon = (p: Uint8Array) => b64(sodium.crypto_box_seal(p, alice.publicKey));
  const otherStory = "11111111-2222-4333-8444-555555555555";

  // Each of these decrypts fine and must still be refused, for the reason in the name.
  const badAnswers: { name: string; stickerId: string; kind: "named" | "anonymous"; envelope: string }[] = [
    { name: "choice outside the options", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, pollS.id, '{"k":"poll","choice":3}')) },
    { name: "negative choice", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, pollS.id, '{"k":"poll","choice":-1}')) },
    { name: "fractional choice", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, pollS.id, '{"k":"poll","choice":1.5}')) },
    { name: "string choice", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, pollS.id, '{"k":"poll","choice":"1"}')) },
    { name: "an answer of another kind", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, pollS.id, '{"k":"quiz","choice":0}')) },
    { name: "slider above 100", stickerId: sliderS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, sliderS.id, '{"k":"slider","value":101}')) },
    { name: "slider below 0", stickerId: sliderS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, sliderS.id, '{"k":"slider","value":-1}')) },
    { name: "fractional slider", stickerId: sliderS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, sliderS.id, '{"k":"slider","value":50.5}')) },
    { name: "empty text", stickerId: questionS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, questionS.id, '{"k":"question","text":"   "}')) },
    { name: "text over 500", stickerId: questionS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, questionS.id, JSON.stringify({ k: "question", text: "q".repeat(501) }))) },
    { name: "not JSON", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, pollS.id, "choice=1")) },
    { name: "sealed for another story", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, otherStory, pollS.id, '{"k":"poll","choice":1}')) },
    { name: "sealed for another sticker", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 1, storyId, quizS.id, '{"k":"poll","choice":1}')) },
    { name: "unknown version", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSR", 2, storyId, pollS.id, '{"k":"poll","choice":1}')) },
    { name: "wrong magic for a named answer", stickerId: pollS.id, kind: "named", envelope: boxNamed(payload("RVSA", 1, storyId, pollS.id, '{"k":"poll","choice":1}')) },
    { name: "a sticker id that runs past the end", stickerId: pollS.id, kind: "named", envelope: boxNamed(Uint8Array.from([...copy("RVSR"), 1, ...idBytes(storyId), 200, 1, 2, 3])) },
    { name: "too short to hold a header", stickerId: pollS.id, kind: "named", envelope: boxNamed(copy("RVSR")) },
    { name: "anonymous: choice outside the options", stickerId: pollS.id, kind: "anonymous", envelope: boxAnon(payload("RVSA", 1, storyId, pollS.id, '{"k":"poll","choice":9}')) },
    { name: "anonymous: wrong magic", stickerId: questionS.id, kind: "anonymous", envelope: boxAnon(payload("RVSR", 1, storyId, questionS.id, '{"k":"question","text":"hi"}')) },
    { name: "anonymous: another story", stickerId: questionS.id, kind: "anonymous", envelope: boxAnon(payload("RVSA", 1, otherStory, questionS.id, '{"k":"question","text":"hi"}')) },
  ];
  // The mistakes this one asserts (not decrypted by the author): a flipped byte, a cut-off envelope, junk.
  const flip = (e: string) => {
    const bytes = crypto.fromBase64(e).slice();
    bytes[bytes.length - 1] ^= 1;
    return b64(bytes);
  };
  const cut = (e: string) => b64(crypto.fromBase64(e).slice(0, 30));
  const brokenEnvelopes = [
    { name: "a flipped byte", stickerId: pollS.id, kind: "named" as const, envelope: flip(named[0].envelope) },
    { name: "a cut-off envelope", stickerId: pollS.id, kind: "named" as const, envelope: cut(named[0].envelope) },
    { name: "empty", stickerId: pollS.id, kind: "named" as const, envelope: "" },
    { name: "anonymous: a flipped byte", stickerId: questionS.id, kind: "anonymous" as const, envelope: flip(anonymous[0].envelope) },
    { name: "anonymous: cut off", stickerId: questionS.id, kind: "anonymous" as const, envelope: cut(anonymous[0].envelope) },
    { name: "anonymous: empty", stickerId: questionS.id, kind: "anonymous" as const, envelope: "" },
  ];

  // --- 3. Totals -----------------------------------------------------------------------------
  const totalsCases: { name: string; stickerId: string; revision: number; nonce: string; ciphertext: string; expected: unknown }[] = [];
  const addTotals = (name: string, s: AnswerableSticker, revision: number, totals: Parameters<typeof sealTotals>[3]) => {
    const sealed = sealTotals(storyId, s.id, revision, totals, key);
    expect(openTotals(storyId, s, revision, sealed.ciphertext, sealed.nonce, key)).toEqual(totals);
    totalsCases.push({ name, stickerId: s.id, revision, nonce: b64(sealed.nonce), ciphertext: b64(sealed.ciphertext), expected: totals });
  };
  addTotals("poll", pollS, 3, { total: 4, counts: [1, 3, 0] });
  addTotals("poll with nobody", pollS, 1, { total: 0, counts: [0, 0, 0] });
  addTotals("quiz", quizS, 12, { total: 5, counts: [1, 3, 1], correct: 3 });
  addTotals("slider", sliderS, 2, { total: 2, average: 55.5 });
  addTotals("slider with no answers", sliderS, 7, { total: 0, average: 0 });

  const resultAd = (forStory: string, stickerId: string, revision: number) => {
    const tag = copy("redvault-story-result-v1");
    const sid = copy(stickerId);
    const out = new Uint8Array(tag.length + 16 + 4 + sid.length);
    out.set(tag, 0);
    out.set(idBytes(forStory), tag.length);
    new DataView(out.buffer).setUint32(tag.length + 16, revision, false);
    out.set(sid, tag.length + 16 + 4);
    return out;
  };
  const craftTotals = (s: AnswerableSticker, revision: number, body: unknown) => {
    const nonce = sodium.randombytes_buf(sodium.crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(JSON.stringify(body), resultAd(storyId, s.id, revision), null, nonce, key);
    return { stickerId: s.id, revision, nonce: b64(nonce), ciphertext: b64(ciphertext) };
  };
  // Authentic (the story key sealed them) but not totals that fit the sticker.
  const badTotals = [
    { name: "too few counts for the options", ...craftTotals(pollS, 1, { total: 2, counts: [1, 1] }) },
    { name: "too many counts", ...craftTotals(pollS, 1, { total: 2, counts: [1, 1, 0, 0] }) },
    { name: "a negative count", ...craftTotals(pollS, 1, { total: 2, counts: [3, -1, 0] }) },
    { name: "a fractional count", ...craftTotals(pollS, 1, { total: 2, counts: [1.5, 0.5, 0] }) },
    { name: "a count over a million", ...craftTotals(pollS, 1, { total: 2, counts: [2000000, 0, 0] }) },
    { name: "a poll with no counts", ...craftTotals(pollS, 1, { total: 2 }) },
    { name: "a quiz with no correct count", ...craftTotals(quizS, 1, { total: 2, counts: [1, 1, 0] }) },
    { name: "a slider average above 100", ...craftTotals(sliderS, 1, { total: 2, average: 101 }) },
    { name: "a slider average below 0", ...craftTotals(sliderS, 1, { total: 2, average: -0.5 }) },
    { name: "a slider with no average", ...craftTotals(sliderS, 1, { total: 2 }) },
    { name: "a total that is not a number", ...craftTotals(sliderS, 1, { total: "2", average: 5 }) },
    { name: "a negative total", ...craftTotals(sliderS, 1, { total: -2, average: 5 }) },
    { name: "not an object", ...craftTotals(sliderS, 1, 7) },
  ];

  // --- 4. Counting ---------------------------------------------------------------------------
  const entry = (s: AnswerableSticker, answer: StoryAnswer, atMs: number, revision = 0): InboxEntry => ({ sticker: s, responderId: null, answer, at: atMs, revision, envelope: "x" });
  const T = 1_800_000_000_000;
  const pollDeadline = { ...pollS, closesAt: T } as AnswerableSticker;
  const tallyCases = [
    { name: "poll counts every vote", sticker: pollS, entries: [entry(pollS, { k: "poll", choice: 0 }, T), entry(pollS, { k: "poll", choice: 2 }, T), entry(pollS, { k: "poll", choice: 2 }, T)] },
    {
      name: "a poll ignores votes after its deadline (to the millisecond given)",
      sticker: pollDeadline,
      entries: [entry(pollDeadline, { k: "poll", choice: 0 }, T - 1), entry(pollDeadline, { k: "poll", choice: 1 }, T), entry(pollDeadline, { k: "poll", choice: 1 }, T + 1), entry(pollDeadline, { k: "poll", choice: 2 }, T + 60_000)],
    },
    { name: "a poll ignores answers of another kind", sticker: pollS, entries: [entry(pollS, { k: "slider", value: 5 }, T), entry(pollS, { k: "poll", choice: 1 }, T)] },
    { name: "a poll ignores other stickers' answers", sticker: pollS, entries: [entry(quizS, { k: "quiz", choice: 1 }, T), entry(pollS, { k: "poll", choice: 1 }, T)] },
    { name: "a quiz counts only first answers, and the right ones", sticker: quizS, entries: [entry(quizS, { k: "quiz", choice: 1 }, T), entry(quizS, { k: "quiz", choice: 0 }, T), entry(quizS, { k: "quiz", choice: 1 }, T, 1), entry(quizS, { k: "quiz", choice: 2 }, T, 4)] },
    { name: "slider average, rounded to a tenth", sticker: sliderS, entries: [10, 21, 33].map((v) => entry(sliderS, { k: "slider", value: v }, T)) },
    { name: "slider average of two", sticker: sliderS, entries: [1, 2].map((v) => entry(sliderS, { k: "slider", value: v }, T)) },
    { name: "slider average rounding down", sticker: sliderS, entries: [10, 10, 11].map((v) => entry(sliderS, { k: "slider", value: v }, T)) },
    { name: "slider with a single zero", sticker: sliderS, entries: [entry(sliderS, { k: "slider", value: 0 }, T)] },
    { name: "slider with nobody", sticker: sliderS, entries: [] },
    { name: "question counts written answers", sticker: questionS, entries: [entry(questionS, { k: "question", text: "a" }, T), entry(questionS, { k: "question", text: "b" }, T), entry(pollS, { k: "poll", choice: 0 }, T)] },
    { name: "nobody answered a poll", sticker: pollS, entries: [] },
    { name: "nobody answered a quiz", sticker: quizS, entries: [] },
  ].map((c) => ({
    name: c.name,
    stickerId: c.sticker.id,
    // The deadline is part of the sticker, so the case carries the sticker it was counted with.
    sticker: c.sticker,
    entries: c.entries.map((e) => ({ stickerId: e.sticker.id, answer: e.answer, at: e.at, revision: e.revision })),
    expected: tallyAnswers(c.sticker, c.entries),
  }));

  writeFileSync(
    OUT,
    JSON.stringify(
      {
        note: "Made by desktop/tests/vectors/make_story_interactive_vectors.ts with the web app's own code.",
        alice: { publicKey: b64(alice.publicKey), privateKey: b64(alice.privateKey) },
        bob: { publicKey: b64(bob.publicKey), privateKey: b64(bob.privateKey) },
        carol: { publicKey: b64(carol.publicKey), privateKey: b64(carol.privateKey) },
        storyId,
        key: b64(key),
        stickers: { raw, parseCases, clean, links },
        story: {
          metaCiphertext: b64(sealedMeta.ciphertext),
          metaNonce: b64(sealedMeta.nonce),
          expectedStickers: meta.stickers,
          sloppyKey: b64(sloppyKey),
          sloppyCiphertext: b64(sloppySealed.ciphertext),
          sloppyNonce: b64(sloppySealed.nonce),
        },
        answers: { named, anonymous, bad: badAnswers, broken: brokenEnvelopes },
        totals: { cases: totalsCases, bad: badTotals },
        tally: tallyCases,
      },
      null,
      2
    ) + "\n"
  );
});
