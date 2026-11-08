// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { act, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import { decodeFeed, fillKeyGaps, publishStory, type StoryFriend } from "../lib/stories.js";
import * as sapi from "../lib/storiesApi.js";
import { DEFAULT_STORY_PREFS, newStoryId, newStoryKey, openStoryMeta, sealStoryMeta, sealStoryPrefs, unwrapStoryKey, wrapStoryKey, type StoryMeta } from "../lib/storyCrypto.js";
import { sendStoryReply } from "../state/actions.js";

vi.mock("../lib/storiesApi.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/storiesApi.js")>()),
  getStoryConfig: vi.fn(async () => ({ media: true })),
  getStoryFeed: vi.fn(),
  getStoryPrefs: vi.fn(),
  putStoryPrefs: vi.fn(),
  createStory: vi.fn(async (s: sapi.NewStoryDto) => ({ id: s.id, published: s.kind === "text", expiresAt: new Date(Date.now() + 86400_000).toISOString() })),
  uploadStoryMedia: vi.fn(async () => {}),
  deleteStory: vi.fn(async () => {}),
  markStoryViewed: vi.fn(async () => {}),
  getStoryKeyGaps: vi.fn(async () => []),
  addStoryKeys: vi.fn(async (_id: string, keys: unknown[]) => ({ added: keys.length })),
  getStoryArchive: vi.fn(async () => []),
  revokeStoryAccess: vi.fn(async () => ({ removed: 1 })),
  getHighlights: vi.fn(async () => []),
}));
vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  listFriends: vi.fn(),
}));
vi.mock("../state/actions.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../state/actions.js")>()),
  sendStoryReply: vi.fn(async () => {}),
}));

type Keys = { publicKey: Uint8Array; privateKey: Uint8Array };
let me: Keys & { userId: string };
let bob: Keys & { userId: string };
let carol: Keys & { userId: string };
let mallory: Keys;
const ME = "00000000-0000-4000-8000-00000000000a";
const BOB = "00000000-0000-4000-8000-00000000000b";
const CAROL = "00000000-0000-4000-8000-00000000000c";

beforeAll(async () => {
  await crypto.ready();
  me = { ...crypto.generateKeyPair(), userId: ME };
  bob = { ...crypto.generateKeyPair(), userId: BOB };
  carol = { ...crypto.generateKeyPair(), userId: CAROL };
  mallory = crypto.generateKeyPair();
});

const friend = (k: Keys & { userId: string }, handle: string): StoryFriend => ({ userId: k.userId, handle, avatarUrl: null, publicKey: k.publicKey });

/** A story as the server would hand it to `viewer`, made by `author` (or sealed by `sealer`, to fake a forgery). */
function storyDto(author: Keys & { userId: string }, viewer: Keys & { userId: string }, text: string, extra: Partial<sapi.StoryDto> = {}, sealer: Keys = author): sapi.StoryDto {
  const id = newStoryId();
  const key = newStoryKey();
  const meta: StoryMeta = { v: 1, kind: "text", text, background: "sunset", font: "classic", overlays: [], closeFriends: false };
  const sealed = sealStoryMeta(id, key, meta);
  return {
    id,
    authorId: author.userId,
    kind: "text",
    metaCiphertext: crypto.toBase64(sealed.ciphertext),
    metaNonce: crypto.toBase64(sealed.nonce),
    mediaSize: null,
    hasMedia: false,
    allowReplies: true,
    createdAt: new Date(Date.now() - 3600_000).toISOString(),
    expiresAt: new Date(Date.now() + 20 * 3600_000).toISOString(),
    wrappedKey: crypto.toBase64(wrapStoryKey(id, key, viewer.publicKey, sealer.privateKey)),
    ...extra,
  };
}

beforeEach(() => {
  vi.mocked(sapi.createStory).mockClear();
  vi.mocked(sapi.uploadStoryMedia).mockReset().mockResolvedValue(undefined);
  vi.mocked(sapi.deleteStory).mockClear();
  vi.mocked(sapi.addStoryKeys).mockClear();
  vi.mocked(sapi.markStoryViewed).mockClear();
  vi.mocked(sendStoryReply).mockClear();
  localStorage.clear();
});

describe("posting a story", () => {
  it("seals the key to you and your friends, minus anyone hidden; close friends only for a close-friends story", async () => {
    const friends = [friend(bob, "bob"), friend(carol, "carol")];
    await publishStory(me, { text: "hi", overlays: [], audience: "friends", allowReplies: true }, friends, { ...DEFAULT_STORY_PREFS, hideFrom: [CAROL] });
    let sent = vi.mocked(sapi.createStory).mock.calls[0][0];
    expect(sent.keys.map((k) => k.userId).sort()).toEqual([ME, BOB].sort());
    expect(sent.audience).toBe("friends");
    expect(sent.mediaSize).toBeNull();
    // Bob opens it and reads what it says; the server only ever had ciphertext.
    const bobKey = unwrapStoryKey(sent.id, crypto.fromBase64(sent.keys.find((k) => k.userId === BOB)!.wrappedKey), me.publicKey, bob.privateKey);
    expect(openStoryMeta(sent.id, bobKey, "text", crypto.fromBase64(sent.metaCiphertext), crypto.fromBase64(sent.metaNonce)).text).toBe("hi");
    expect(JSON.stringify(sent)).not.toContain('"hi"');

    await publishStory(me, { text: "just you", overlays: [], audience: "close_friends", allowReplies: false }, friends, { ...DEFAULT_STORY_PREFS, closeFriends: [CAROL] });
    sent = vi.mocked(sapi.createStory).mock.calls[1][0];
    expect(sent.keys.map((k) => k.userId).sort()).toEqual([ME, CAROL].sort());
    expect(sent.audience).toBe("close_friends");
    expect(sent.allowReplies).toBe(false);
  });

  it("uploads exactly the announced number of encrypted bytes, and cleans up if the upload fails", async () => {
    const photo = new Blob([new Uint8Array(5000).fill(7)], { type: "image/jpeg" });
    const media = { kind: "image" as const, blob: photo, mime: "image/jpeg", width: 1080, height: 1920 };
    await publishStory(me, { media, overlays: [], audience: "friends", allowReplies: true }, [friend(bob, "bob")], DEFAULT_STORY_PREFS);
    const sent = vi.mocked(sapi.createStory).mock.calls[0][0];
    const uploaded = vi.mocked(sapi.uploadStoryMedia).mock.calls[0][1];
    expect(sent.kind).toBe("image");
    expect(uploaded.size).toBe(sent.mediaSize);
    expect(uploaded.size).toBeGreaterThan(5000);

    vi.mocked(sapi.uploadStoryMedia).mockRejectedValueOnce(new Error("offline"));
    await expect(publishStory(me, { media, overlays: [], audience: "friends", allowReplies: true }, [friend(bob, "bob")], DEFAULT_STORY_PREFS)).rejects.toThrow("offline");
    expect(sapi.deleteStory).toHaveBeenCalledWith(vi.mocked(sapi.createStory).mock.calls[1][0].id);
  });
});

describe("the feed", () => {
  it("opens friends' stories, drops forgeries and strangers, and puts people with something new first", () => {
    const feed: sapi.FeedDto = {
      own: [{ ...storyDto(me, me, "mine"), viewCount: 2 }],
      authors: [
        { authorId: CAROL, stories: [{ ...storyDto(carol, me, "old news"), seen: true }] },
        { authorId: BOB, stories: [{ ...storyDto(bob, me, "fresh"), seen: false }, { ...storyDto(bob, me, "forged", {}, mallory), seen: false }] },
        { authorId: "00000000-0000-4000-8000-0000000000ff", stories: [{ ...storyDto(bob, me, "who?", { authorId: "00000000-0000-4000-8000-0000000000ff" }), seen: false }] },
      ],
    };
    const decoded = decodeFeed(feed, me, new Map([[BOB, friend(bob, "bob")], [CAROL, friend(carol, "carol")]]));
    expect(decoded.own.map((s) => s.meta.text)).toEqual(["mine"]);
    expect(decoded.own[0].viewCount).toBe(2);
    expect(decoded.authors.map((a) => a.author.handle)).toEqual(["bob", "carol"]);
    expect(decoded.authors[0].stories.map((s) => s.meta.text)).toEqual(["fresh"]);
    expect(decoded.authors[1].allSeen).toBe(true);
  });
});

describe("friends added later", () => {
  it("get the key from the author's app, except anyone hidden or offered with a key that isn't theirs", async () => {
    const id = newStoryId();
    const key = newStoryKey();
    vi.mocked(sapi.getStoryKeyGaps).mockResolvedValueOnce([
      {
        storyId: id,
        users: [
          { userId: BOB, publicKey: crypto.toBase64(bob.publicKey) },
          { userId: CAROL, publicKey: crypto.toBase64(carol.publicKey) },
          // The server swapping in another key for a friend gets nothing.
          { userId: "00000000-0000-4000-8000-00000000000d", publicKey: crypto.toBase64(mallory.publicKey) },
        ],
      },
    ]);
    const friends = new Map([
      [BOB, friend(bob, "bob")],
      [CAROL, friend(carol, "carol")],
      ["00000000-0000-4000-8000-00000000000d", { userId: "00000000-0000-4000-8000-00000000000d", handle: "dan", avatarUrl: null, publicKey: crypto.generateKeyPair().publicKey }],
    ]);
    const added = await fillKeyGaps(me, new Map([[id, key]]), friends, { ...DEFAULT_STORY_PREFS, hideFrom: [CAROL] });
    expect(added).toBe(1);
    const [storyId, keys] = vi.mocked(sapi.addStoryKeys).mock.calls[0];
    expect(storyId).toBe(id);
    expect(keys.map((k) => k.userId)).toEqual([BOB]);
    expect(unwrapStoryKey(id, crypto.fromBase64(keys[0].wrappedKey), me.publicKey, bob.privateKey)).toEqual(key);
  });
});

// --- The screens ------------------------------------------------------------------------

const dispatch = vi.fn();
let conversations: unknown[] = [];
vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({
    state: {
      authStatus: "signed-in",
      identity: { userId: ME, handle: "me", avatarUrl: null, publicKey: me.publicKey, privateKey: me.privateKey, role: "user", subscriptionTier: "free" },
      conversations,
      settings: {},
    },
    dispatch,
  }),
}));

async function renderStories(prefs = DEFAULT_STORY_PREFS) {
  const { StoriesProvider } = await import("../state/StoriesContext.js");
  const { StoryTray } = await import("../components/stories/StoryTray.js");
  const { StoryOverlays } = await import("../components/stories/StoryOverlays.js");
  const sealed = sealStoryPrefs(prefs, me.privateKey);
  vi.mocked(sapi.getStoryPrefs).mockResolvedValue({ prefsCiphertext: crypto.toBase64(sealed.ciphertext), prefsNonce: crypto.toBase64(sealed.nonce), archiveEnabled: false, revision: 1 });
  vi.mocked(api.listFriends).mockResolvedValue([{ userId: BOB, username: "bob", publicKey: crypto.toBase64(bob.publicKey), avatarUrl: null }]);
  vi.mocked(sapi.getStoryFeed).mockResolvedValue({ own: [], authors: [{ authorId: BOB, stories: [{ ...storyDto(bob, me, "Sunset at the lake"), seen: false }] }] });
  return render(
    <StoriesProvider>
      <StoryTray />
      <StoryOverlays />
    </StoriesProvider>
  );
}

describe("stories on screen", () => {
  beforeEach(() => {
    conversations = [{ kind: "dm", id: "c1", userId: BOB, handle: "bob", publicKey: undefined, conversationKey: new Uint8Array(32), messages: [] }];
  });

  it("the tray shows a friend's new story; opening it shows it, counts the view and lets you reply", async () => {
    const user = userEvent.setup();
    await renderStories();
    const ring = await screen.findByRole("button", { name: "bob's story, 1 new" });
    expect(screen.getByRole("button", { name: "Add to your story" })).toBeInTheDocument();
    await user.click(ring);
    const viewer = await screen.findByRole("dialog", { name: "bob's story" });
    expect(within(viewer).getByText("Sunset at the lake")).toBeInTheDocument();
    await waitFor(() => expect(sapi.markStoryViewed).toHaveBeenCalledTimes(1));
    await user.click(within(viewer).getByRole("button", { name: "Pause" }));
    await user.type(within(viewer).getByRole("textbox", { name: "Reply to bob" }), "Gorgeous!{Enter}");
    await waitFor(() => expect(sendStoryReply).toHaveBeenCalled());
    const [, conversationId, , text, ref] = vi.mocked(sendStoryReply).mock.calls[0];
    expect(conversationId).toBe("c1");
    expect(text).toBe("Gorgeous!");
    expect(ref).toMatchObject({ authorId: BOB, kind: "text", reaction: false, preview: "Sunset at the lake" });
    // Closing goes back; the ring has turned grey.
    await user.click(within(viewer).getByRole("button", { name: "Close stories" }));
    expect(await screen.findByRole("button", { name: "bob's story" })).toBeInTheDocument();
  });

  it("with sharing views off, nothing tells the server you looked", async () => {
    const user = userEvent.setup();
    await renderStories({ ...DEFAULT_STORY_PREFS, shareViews: false });
    await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
    await screen.findByText("Sunset at the lake");
    await act(async () => {
      await new Promise((r) => setTimeout(r, 50));
    });
    expect(sapi.markStoryViewed).not.toHaveBeenCalled();
  });

  it("anyone can post a text story from the tray", async () => {
    const user = userEvent.setup();
    await renderStories();
    await user.click(await screen.findByRole("button", { name: "New story" }));
    const composer = await screen.findByRole("dialog", { name: "New story" });
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    fireEvent.change(within(composer).getByRole("textbox", { name: "Story text" }), { target: { value: "First story!" } });
    await user.click(within(composer).getByRole("button", { name: "Share" }));
    await waitFor(() => expect(sapi.createStory).toHaveBeenCalled());
    const sent = vi.mocked(sapi.createStory).mock.calls[0][0];
    expect(sent.kind).toBe("text");
    expect(sent.keys.map((k) => k.userId).sort()).toEqual([ME, BOB].sort());
    await waitFor(() => expect(screen.queryByRole("dialog", { name: "New story" })).not.toBeInTheDocument());
  });
});
