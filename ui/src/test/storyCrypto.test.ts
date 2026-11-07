import { beforeAll, describe, expect, it } from "vitest";
import * as crypto from "../lib/e2eCrypto.js";
import {
  DEFAULT_STORY_PREFS,
  newStoryId,
  newStoryKey,
  openHighlightTitle,
  openStoryMeta,
  openStoryPrefs,
  parseStoryMeta,
  sealHighlightTitle,
  sealStoryMeta,
  sealStoryPrefs,
  StoryCryptoError,
  unwrapStoryKey,
  wrapStoryKey,
  type StoryMeta,
} from "../lib/storyCrypto.js";

beforeAll(async () => {
  await crypto.ready();
});

const textMeta: StoryMeta = {
  v: 1,
  kind: "text",
  text: "Hello friends",
  background: "ocean",
  font: "modern",
  overlays: [{ type: "emoji", text: "🔥", x: 0.5, y: 0.2, scale: 1.4, rotation: 10, color: "white", boxed: false, font: "classic" }],
  closeFriends: false,
};

describe("story keys", () => {
  it("only the friend it was sealed to opens it, and they know the author made it", () => {
    const author = crypto.generateKeyPair();
    const friend = crypto.generateKeyPair();
    const stranger = crypto.generateKeyPair();
    const id = newStoryId();
    expect(id).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    const key = newStoryKey();
    const wrapped = wrapStoryKey(id, key, friend.publicKey, author.privateKey);
    expect(wrapped.length).toBe(93);
    expect(unwrapStoryKey(id, wrapped, author.publicKey, friend.privateKey)).toEqual(key);
    // Someone else's private key can't open it.
    expect(() => unwrapStoryKey(id, wrapped, author.publicKey, stranger.privateKey)).toThrow(StoryCryptoError);
    // A key sealed by someone else isn't accepted as this author's (no forged stories).
    const forged = wrapStoryKey(id, key, friend.publicKey, stranger.privateKey);
    expect(() => unwrapStoryKey(id, forged, author.publicKey, friend.privateKey)).toThrow(StoryCryptoError);
  });

  it("a key can't be moved onto another story", () => {
    const author = crypto.generateKeyPair();
    const friend = crypto.generateKeyPair();
    const id = newStoryId();
    const wrapped = wrapStoryKey(id, newStoryKey(), friend.publicKey, author.privateKey);
    expect(() => unwrapStoryKey(newStoryId(), wrapped, author.publicKey, friend.privateKey)).toThrow(/another story/);
  });

  it("the author's own copy opens with their own keys", () => {
    const author = crypto.generateKeyPair();
    const id = newStoryId();
    const key = newStoryKey();
    expect(unwrapStoryKey(id, wrapStoryKey(id, key, author.publicKey, author.privateKey), author.publicKey, author.privateKey)).toEqual(key);
  });
});

describe("story details", () => {
  it("round-trip, bound to the story id", () => {
    const id = newStoryId();
    const key = newStoryKey();
    const sealed = sealStoryMeta(id, key, textMeta);
    expect(openStoryMeta(id, key, "text", sealed.ciphertext, sealed.nonce)).toEqual(textMeta);
    // The same ciphertext presented as another story's fails.
    expect(() => openStoryMeta(newStoryId(), key, "text", sealed.ciphertext, sealed.nonce)).toThrow(StoryCryptoError);
    // So does the wrong kind (the server says what kind it is; the details must agree).
    expect(() => openStoryMeta(id, key, "image", sealed.ciphertext, sealed.nonce)).toThrow(StoryCryptoError);
  });

  it("refuse malformed media and unknown colours fall back safely", () => {
    expect(() => parseStoryMeta({ v: 1, kind: "image", overlays: [], media: { key: "x" } }, "image")).toThrow();
    expect(() =>
      parseStoryMeta(
        { v: 1, kind: "image", overlays: [], media: { key: "AAAA", header: "AAAA", sha256: "a".repeat(64), encryptedSize: 10, chunkSize: 65536, mime: "text/html", width: 1, height: 1 } },
        "image"
      )
    ).toThrow();
    const ok = parseStoryMeta(
      {
        v: 1,
        kind: "image",
        overlays: [{ type: "text", text: "hi", x: 0.1, y: 0.9, scale: 99, rotation: -30, color: "url(evil)", boxed: true, font: "comic" }],
        media: { key: "AAAA", header: "AAAA", sha256: "a".repeat(64), encryptedSize: 10, chunkSize: 65536, mime: "image/jpeg", width: 1080, height: 1920 },
      },
      "image"
    );
    expect(ok.overlays[0]).toMatchObject({ color: "white", font: "classic", scale: 4, rotation: 330 });
    // An overlay off the frame is refused rather than drawn somewhere odd.
    expect(() => parseStoryMeta({ ...textMeta, overlays: [{ type: "text", text: "x", x: 2, y: 0.5 }] }, "text")).toThrow();
    // A video needs its length.
    expect(() =>
      parseStoryMeta(
        { v: 1, kind: "video", overlays: [], media: { key: "AAAA", header: "AAAA", sha256: "a".repeat(64), encryptedSize: 10, chunkSize: 65536, mime: "video/mp4", width: 1, height: 1 } },
        "video"
      )
    ).toThrow();
  });
});

describe("highlight titles and settings", () => {
  it("a title opens with the cover story's key only", () => {
    const id = newStoryId();
    const key = newStoryKey();
    const sealed = sealHighlightTitle("  Summer trip  ", id, key);
    expect(openHighlightTitle(sealed.ciphertext, sealed.nonce, id, key)).toBe("Summer trip");
    expect(openHighlightTitle(sealed.ciphertext, sealed.nonce, id, newStoryKey())).toBeNull();
    expect(openHighlightTitle(sealed.ciphertext, sealed.nonce, newStoryId(), key)).toBeNull();
  });

  it("settings are sealed with a key only this account can derive", () => {
    const me = crypto.generateKeyPair();
    const other = crypto.generateKeyPair();
    const friendId = "11111111-2222-4333-8444-555555555555";
    const prefs = { ...DEFAULT_STORY_PREFS, closeFriends: [friendId, friendId.toUpperCase()], shareViews: false };
    const sealed = sealStoryPrefs(prefs, me.privateKey);
    const opened = openStoryPrefs(sealed.ciphertext, sealed.nonce, me.privateKey);
    expect(opened.closeFriends).toEqual([friendId]);
    expect(opened.shareViews).toBe(false);
    expect(opened.allowReplies).toBe(true);
    expect(() => openStoryPrefs(sealed.ciphertext, sealed.nonce, other.privateKey)).toThrow(StoryCryptoError);
  });
});
