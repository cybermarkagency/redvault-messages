// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { describe, expect, it } from "vitest";
import { isStructured, makePreview, parseStructured, serializeReaction, serializeReply, serializeStoryReply } from "../lib/richMessage.js";

const MSG = "3f2504e0-4f89-11d3-9a0c-0305e82c3301";
const USER = "9b2e6a3c-1d4f-4e7a-8b5c-2a1f0e9d8c7b";

describe("richMessage", () => {
  it("round-trips a reply", () => {
    const body = serializeReply("sounds good", { messageId: MSG, senderId: USER, preview: "lunch at 1?" });
    expect(isStructured(body)).toBe(true);
    expect(parseStructured(body)).toEqual({
      kind: "text",
      text: "sounds good",
      replyTo: { messageId: MSG, senderId: USER, preview: "lunch at 1?" },
    });
  });

  it("round-trips a reaction and rejects anything but one emoji", () => {
    expect(parseStructured(serializeReaction(MSG, "❤️"))).toEqual({ kind: "reaction", messageId: MSG, emoji: "❤️" });
    expect(parseStructured(serializeReaction(MSG, "💩"))).toEqual({ kind: "reaction", messageId: MSG, emoji: "💩" });
    expect(parseStructured(serializeReaction(MSG, "👍").replace("👍", "👍👍"))).toEqual({ kind: "invalid" });
    expect(parseStructured(serializeReaction(MSG, "👍").replace("👍", "xx"))).toEqual({ kind: "invalid" });
  });

  it("matches the Android wire format exactly", () => {
    expect(serializeReaction(MSG, "👍")).toBe(
      `\u0000rv-reaction:{"type":"reaction","version":1,"messageId":"${MSG}","emoji":"👍"}`
    );
    expect(serializeReply("hi", { messageId: MSG, senderId: USER, preview: "yo" })).toBe(
      `\u0000rv-msg:{"type":"text","version":1,"text":"hi","replyTo":{"messageId":"${MSG}","senderId":"${USER}","preview":"yo"}}`
    );
  });

  it("strips hidden characters and caps the quoted preview", () => {
    expect(makePreview("a‮b​c\n\nd")).toBe("abc d");
    expect(makePreview("x".repeat(300))).toHaveLength(200);
  });

  it("treats malformed bodies as invalid, never throws", () => {
    expect(parseStructured("\u0000rv-msg:not json")).toEqual({ kind: "invalid" });
    expect(parseStructured(`\u0000rv-msg:{"type":"text","version":2,"text":"x"}`)).toEqual({ kind: "invalid" });
    expect(parseStructured(`\u0000rv-msg:{"type":"text","version":1,"text":"x","replyTo":{"messageId":"../x"}}`)).toEqual({
      kind: "invalid",
    });
    expect(isStructured("plain text")).toBe(false);
  });

  it("round-trips a story reply, and an old app still shows its text", () => {
    const body = serializeStoryReply("love this", { authorId: USER.toUpperCase(), storyId: MSG, kind: "image", preview: "Photo", reaction: false });
    expect(parseStructured(body)).toEqual({
      kind: "text",
      text: "love this",
      storyReply: { authorId: USER, storyId: MSG, kind: "image", preview: "Photo", reaction: false },
    });
    expect(JSON.parse(body.slice(body.indexOf("{"))).text).toBe("love this");
    const tampered = body.replace('"kind":"image"', '"kind":"pdf"');
    expect(parseStructured(tampered)).toEqual({ kind: "invalid" });
  });
});
