// Edits, forwards, pins, polls and formatting: the same wire format and rules
// as the desktop app (desktop/tests/message_format_test.cpp,
// conversation_repository_test.cpp and text_format_test.cpp).
import { describe, expect, it } from "vitest";
import {
  isReactionEmoji,
  parseStructured,
  serializeEdit,
  serializeEndPoll,
  serializeForwarded,
  serializePin,
  serializePoll,
  serializeReaction,
  serializeUnpin,
  serializeVote,
} from "../lib/richMessage.js";
import { hasFormatting, mentions, parseFormatted, toPlain } from "../lib/textFormat.js";
import { deletedWith, foldEditsAndVotes, orderReactions, pinnedFrom } from "../state/actions.js";
import type { Message } from "../types.js";

const MSG = "3f2504e0-4f89-11d3-9a0c-0305e82c3301";
const MSG2 = "9b2e6a3c-1d4f-4e7a-8b5c-2a1f0e9d8c7b";

describe("new rich message fields", () => {
  it("are byte-for-byte what the desktop app sends", () => {
    expect(serializeEdit("fixed typo", MSG)).toBe(`\u0000rv-msg:{"type":"text","version":1,"text":"fixed typo","edits":"${MSG}"}`);
    expect(serializeForwarded("look at this")).toBe(`\u0000rv-msg:{"type":"text","version":1,"text":"look at this","forwarded":true}`);
    expect(serializePin(MSG, "note 1")).toBe(
      `\u0000rv-msg:{"type":"text","version":1,"text":"📌 Pinned: note 1","pin":{"messageId":"${MSG}","preview":"note 1"}}`
    );
    expect(serializeUnpin(MSG)).toBe(`\u0000rv-msg:{"type":"text","version":1,"text":"Unpinned a message","unpin":"${MSG}"}`);
    expect(serializePoll("Where?", ["Park", " Beach "], false)).toBe(
      `\u0000rv-msg:{"type":"text","version":1,"text":"📊 Poll: Where?\\n1. Park\\n2. Beach","poll":{"question":"Where?","options":["Park","Beach"],"multiple":false}}`
    );
    expect(serializeVote(MSG, [1], ["Park", "Beach"])).toBe(
      `\u0000rv-msg:{"type":"text","version":1,"text":"Voted: Beach","vote":{"pollId":"${MSG}","options":[1]}}`
    );
    expect(serializeEndPoll(MSG)).toBe(`\u0000rv-msg:{"type":"text","version":1,"text":"Ended the poll","endPoll":"${MSG}"}`);
  });

  it("parse back, ids lowercased", () => {
    expect(parseStructured(serializeEdit("x", MSG.toUpperCase()))).toEqual({ kind: "text", text: "x", edits: MSG });
    expect(parseStructured(serializeForwarded("x"))).toEqual({ kind: "text", text: "x", forwarded: true });
    expect(parseStructured(serializePin(MSG, "hi"))).toMatchObject({ pin: { messageId: MSG, preview: "hi" } });
    expect(parseStructured(serializeUnpin(MSG))).toMatchObject({ unpin: MSG });
    expect(parseStructured(serializePoll("Q", ["a", "b"], true))).toMatchObject({ poll: { question: "Q", options: ["a", "b"], multiple: true } });
    expect(parseStructured(serializeVote(MSG, [0, 0, 2], []))).toMatchObject({ vote: { pollId: MSG, options: [0, 2] } });
    expect(parseStructured(serializeEndPoll(MSG))).toMatchObject({ endPoll: MSG });
  });

  it("reject malformed fields instead of half-applying them", () => {
    const body = (extra: string) => `\u0000rv-msg:{"type":"text","version":1,"text":"x",${extra}}`;
    expect(parseStructured(body(`"edits":"nope"`))).toEqual({ kind: "invalid" });
    expect(parseStructured(body(`"forwarded":"yes"`))).toEqual({ kind: "invalid" });
    expect(parseStructured(body(`"poll":{"question":"Q","options":["only one"]}`))).toEqual({ kind: "invalid" });
    expect(parseStructured(body(`"vote":{"pollId":"${MSG}","options":[99]}`))).toEqual({ kind: "invalid" });
    expect(() => serializePoll("Q", ["same", "same"], false)).toThrow();
    expect(() => serializePoll("  ", ["a", "b"], false)).toThrow();
  });
});

describe("reactions", () => {
  // The same cases as desktop/tests/message_format_test.cpp and android RichFeaturesTest.kt.
  const ZWJ = "‍";
  it("accept any single emoji", () => {
    const one = ["👍", "❤️", "💩", "👍🏽", `👨${ZWJ}👩${ZWJ}👧`, `🏳️${ZWJ}🌈`, "🇳🇱", "1️⃣", "#⃣", "🏴\u{e0067}\u{e0062}\u{e0073}\u{e0063}\u{e0074}\u{e007f}",
      `👩🏽${ZWJ}❤️${ZWJ}💋${ZWJ}👨🏻`, "☺️", "©️"];
    for (const emoji of one) {
      expect(isReactionEmoji(emoji), emoji).toBe(true);
      expect(parseStructured(serializeReaction(MSG, emoji))).toEqual({ kind: "reaction", messageId: MSG, emoji });
    }
    // At most 16 code points.
    const fifteen = `👍${ZWJ}`.repeat(7) + "👍";
    expect(isReactionEmoji(fifteen)).toBe(true);
    expect(isReactionEmoji(fifteen + ZWJ + "👍")).toBe(false);
  });

  it("reject anything else", () => {
    const notOne = ["", "x", "👍👍", "👍 ", " 👍", "🇳", "🏽", `${ZWJ}👍`, `👍${ZWJ}`, "1", "11⃣", "👍\n", "🇳🇱🇳🇱", "\ud83d"];
    for (const text of notOne) expect(isReactionEmoji(text), JSON.stringify(text)).toBe(false);
    expect(() => serializeReaction(MSG, "👍👍")).toThrow();
  });

  it("show the quick six first, then the rest in the order they were first used", () => {
    const tally = (firstAt: number, senders: string[], myReactionId?: string) => ({ senders: new Set(senders), firstAt, myReactionId });
    const ordered = orderReactions(
      new Map([
        ["🦄", tally(5, ["a"])],
        ["🙏", tally(9, ["a", "b"], "r1")],
        ["🎉", tally(2, ["b"])],
        ["👍", tally(7, ["c"])],
      ])
    );
    expect(ordered.map((r) => r.emoji)).toEqual(["👍", "🙏", "🎉", "🦄"]);
    expect(ordered[1]).toEqual({ emoji: "🙏", count: 2, mine: true, myReactionId: "r1" });
  });
});

describe("formatting", () => {
  it("leaves plain text alone", () => {
    expect(hasFormatting("snake_case_name and 2*3*4 and file_name.txt")).toBe(false);
    expect(hasFormatting("**double** and * spaced * and *never closed")).toBe(false);
  });

  it("finds markers, monospace, spoilers and links", () => {
    expect(parseFormatted("a *bold* and _it_ ~s~")).toEqual([
      "a ",
      { kind: "b", children: ["bold"] },
      " and ",
      { kind: "i", children: ["it"] },
      " ",
      { kind: "s", children: ["s"] },
    ]);
    expect(parseFormatted("`*not bold*`")).toEqual([{ kind: "code", text: "*not bold*" }]);
    expect(parseFormatted("*bold and _italic_*")).toEqual([{ kind: "b", children: ["bold and ", { kind: "i", children: ["italic"] }] }]);
    expect(parseFormatted("||the butler||")).toEqual([{ kind: "spoiler", children: ["the butler"] }]);
    expect(parseFormatted("*one\ntwo*")).toEqual(["*one\ntwo*"]);
    expect(parseFormatted("see https://example.com/a?b=1.")).toEqual(["see ", { kind: "link", url: "https://example.com/a?b=1" }, "."]);
    expect(parseFormatted("(http://example.com/x_(y))")).toEqual(["(", { kind: "link", url: "http://example.com/x_(y)" }, ")"]);
    expect(hasFormatting("javascript:alert(1) file:///c:/x ftp://x xhttps://example.com")).toBe(false);
  });

  it("highlights group members' mentions, yours more", () => {
    const nodes = parseFormatted("hi @Bob and @bobby, email@bob.com @me @carol", { mentions: ["bob", "bobby", "me"], selfHandle: "me" });
    expect(nodes).toContainEqual({ kind: "mention", text: "@Bob", self: false });
    expect(nodes).toContainEqual({ kind: "mention", text: "@bobby", self: false });
    expect(nodes).toContainEqual({ kind: "mention", text: "@me", self: true });
    expect(nodes.filter((n) => typeof n !== "string")).toHaveLength(3);
    expect(mentions("ping @me please", "me")).toBe(true);
    expect(mentions("ping @meg", "me")).toBe(false);
  });

  it("drops markers and hides spoilers in previews", () => {
    expect(toPlain("*bold* and _it_ ~s~ `c` ||secret||")).toBe("bold and it s c •••");
  });

  it("stays fast on hostile input", () => {
    const start = Date.now();
    parseFormatted(" *a".repeat(20000));
    parseFormatted(`${"*_".repeat(5000)}x${"_*".repeat(5000)}`);
    expect(Date.now() - start).toBeLessThan(2000);
  });
});

describe("folding edits, votes and pins", () => {
  const message = (id: string, from: string, text: string, at: number, extra: Partial<Message> = {}): Message => ({
    id,
    from,
    text,
    time: "",
    createdAt: at,
    ...extra,
  });

  it("applies only the original sender's edits", () => {
    const original = message(MSG, "bob", "see you at 5", 1);
    const senders = new Map([[MSG, "bob-id"]]);
    const folded = foldEditsAndVotes(
      original,
      [
        { id: "e1", targetId: MSG, senderId: "bob-id", text: "see you at 6", at: 2 },
        { id: "e2", targetId: MSG, senderId: "alice-id", text: "forged", at: 3 },
        { id: "e3", targetId: MSG, senderId: "bob-id", text: "see you at 7", at: 4 },
      ],
      [],
      senders,
      "alice-id"
    );
    expect(folded.text).toBe("see you at 7");
    expect(folded.editedAt).toBe(4);
    expect(folded.earlierVersions?.map((v) => v.text)).toEqual(["see you at 5", "see you at 6"]);
    expect(folded.editMessageIds).toEqual(["e1", "e3"]);
  });

  it("tallies each person's latest vote until the creator ends the poll", () => {
    const poll = message(MSG, "bob", "Where?", 1, {
      poll: { question: "Where?", options: ["Park", "Beach", "Cafe"], multiple: false, closed: false, counts: [0, 0, 0], mine: [], voters: 0 },
    });
    const folded = foldEditsAndVotes(
      poll,
      [],
      [
        { pollId: MSG, senderId: "alice-id", at: 2, end: false, options: [0] },
        { pollId: MSG, senderId: "carol-id", at: 3, end: false, options: [1] },
        { pollId: MSG, senderId: "alice-id", at: 4, end: false, options: [2, 0] },
        { pollId: MSG, senderId: "carol-id", at: 5, end: true, options: [] },
        { pollId: MSG, senderId: "bob-id", at: 6, end: true, options: [] },
        { pollId: MSG, senderId: "carol-id", at: 7, end: false, options: [2] },
      ],
      new Map([[MSG, "bob-id"]]),
      "alice-id"
    );
    expect(folded.poll).toMatchObject({ closed: true, counts: [0, 1, 1], mine: [2], voters: 2 });
  });

  it("deletes a message's edits and your own pins of it along with it", () => {
    const original = message(MSG, "me", "hi", 1, { editMessageIds: ["e1"] });
    const all = [
      original,
      message("n1", "me", "hi", 2, { notice: { kind: "pin", targetId: MSG } }),
      message("n2", "bob", "hi", 3, { notice: { kind: "pin", targetId: MSG } }),
      message("n3", "me", "other", 4, { notice: { kind: "pin", targetId: MSG2 } }),
    ];
    expect(deletedWith(original, all)).toEqual(["e1", "n1"]);
  });

  it("drops a pin once its message is gone", () => {
    const notices = [
      message("n1", "bob", "a", 1, { notice: { kind: "pin", targetId: MSG } }),
      message("n2", "bob", "b", 2, { notice: { kind: "pin", targetId: MSG2, targetGone: true } }),
    ];
    expect(pinnedFrom(notices).map((p) => p.messageId)).toEqual([MSG]);
  });

  it("keeps the newest three pins", () => {
    const notices = [MSG, MSG2, "a", "b"].map((target, i) => message(`n${i}`, "me", `note ${i}`, i, { notice: { kind: "pin", targetId: target } }));
    notices.push(message("n9", "bob", "", 9, { notice: { kind: "unpin", targetId: "a" } }));
    expect(pinnedFrom(notices).map((p) => p.messageId)).toEqual([MSG2, "b"]);
  });
});
