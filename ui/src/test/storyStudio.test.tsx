// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import "fake-indexeddb/auto";
import { cleanup, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import * as sapi from "../lib/storiesApi.js";
import { listDrafts } from "../lib/storyDrafts.js";
import { DEFAULT_STORY_PREFS, newStoryId, newStoryKey, openStoryMeta, sealStoryMeta, sealStoryPrefs, unwrapStoryKey, wrapStoryKey, type StoryMeta } from "../lib/storyCrypto.js";

// The studio's screens: what a person sees and what leaves the device, with the network and the media
// elements stood in for. Camera, recorder, canvas and sound are checked in a real browser (README).

vi.mock("../lib/storiesApi.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/storiesApi.js")>()),
  getStoryConfig: vi.fn(),
  getStoryFeed: vi.fn(),
  getStoryPrefs: vi.fn(),
  putStoryPrefs: vi.fn(),
  createStory: vi.fn(),
  uploadStoryMedia: vi.fn(async () => {}),
  deleteStory: vi.fn(async () => {}),
  markStoryViewed: vi.fn(async () => {}),
  getStoryKeyGaps: vi.fn(async () => []),
  addStoryKeys: vi.fn(async () => ({ added: 0 })),
  getStoryArchive: vi.fn(async () => []),
  revokeStoryAccess: vi.fn(async () => ({ removed: 0 })),
  getHighlights: vi.fn(async () => []),
  getStoryAnswers: vi.fn(async () => []),
  getMyStoryAnswers: vi.fn(async () => []),
  getStoryTotals: vi.fn(async () => []),
}));
vi.mock("../lib/stories.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/stories.js")>()),
  storyMediaUrl: vi.fn(async () => "blob:test-audio"),
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
let me: Keys;
let bob: Keys;
const ME = "00000000-0000-4000-8000-00000000000a";
const BOB = "00000000-0000-4000-8000-00000000000b";

beforeAll(async () => {
  await crypto.ready();
  me = crypto.generateKeyPair();
  bob = crypto.generateKeyPair();
});

vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({
    state: {
      authStatus: "signed-in",
      identity: { userId: ME, handle: "me", avatarUrl: null, publicKey: me.publicKey, privateKey: me.privateKey, role: "user", subscriptionTier: "free" },
      conversations: [],
      settings: {},
    },
    dispatch: vi.fn(),
  }),
}));

const published = (s: sapi.NewStoryDto) => ({ id: s.id, published: true, expiresAt: new Date(Date.now() + 86400_000).toISOString() });
const ALL_TTLS = [3600, 6 * 3600, 12 * 3600, 86400, 2 * 86400, 7 * 86400];

async function renderStories(feed: sapi.FeedDto = { own: [], authors: [] }, config: sapi.StoryConfigDto = { enabled: true, media: true, interactive: true, ttlSeconds: ALL_TTLS }, beforeRender?: () => void) {
  const { StoriesProvider } = await import("../state/StoriesContext.js");
  const { StoryTray } = await import("../components/stories/StoryTray.js");
  const { StoryOverlays } = await import("../components/stories/StoryOverlays.js");
  const sealed = sealStoryPrefs(DEFAULT_STORY_PREFS, me.privateKey);
  vi.mocked(sapi.getStoryConfig).mockResolvedValue(config);
  vi.mocked(sapi.getStoryPrefs).mockResolvedValue({ prefsCiphertext: crypto.toBase64(sealed.ciphertext), prefsNonce: crypto.toBase64(sealed.nonce), archiveEnabled: false, revision: 1 });
  vi.mocked(api.listFriends).mockResolvedValue([{ userId: BOB, username: "bob", publicKey: crypto.toBase64(bob.publicKey), avatarUrl: null }]);
  vi.mocked(sapi.getStoryFeed).mockResolvedValue(feed);
  beforeRender?.();
  return render(
    <StoriesProvider>
      <StoryTray />
      <StoryOverlays />
    </StoriesProvider>
  );
}

beforeEach(() => {
  for (const fn of Object.values(sapi)) if (typeof fn === "function" && "mockClear" in fn) (fn as unknown as { mockClear(): void }).mockClear();
  vi.mocked(sapi.createStory).mockReset();
  vi.mocked(sapi.createStory).mockImplementation(async (s) => published(s));
  localStorage.clear();
});

afterEach(async () => {
  cleanup();
  await new Promise<void>((resolve) => {
    const req = indexedDB.deleteDatabase("redvault-story-drafts");
    req.onsuccess = req.onerror = req.onblocked = () => resolve();
  });
});

const myIdentity = () => ({ userId: ME, publicKey: me.publicKey, privateKey: me.privateKey });

describe("Stories switched off on the server", () => {
  it("leave no tray and fetch nothing, even though the server answers every other route with 404", async () => {
    const off = Object.assign(new Error("stories_disabled"), { status: 404 });
    // What the real server does with Stories off: only the settings answer.
    const view = await renderStories({ own: [], authors: [] }, { enabled: false, media: true }, () => {
      vi.mocked(sapi.getStoryPrefs).mockRejectedValue(off);
      vi.mocked(sapi.getStoryFeed).mockRejectedValue(off);
    });
    await waitFor(() => expect(sapi.getStoryConfig).toHaveBeenCalled());
    await waitFor(() => expect(screen.queryByRole("button", { name: "New story" })).not.toBeInTheDocument());
    await new Promise((r) => setTimeout(r, 100));
    expect(view.container.querySelector('[aria-label="Stories"]')).toBeNull();
    expect(sapi.getStoryPrefs).not.toHaveBeenCalled();
    expect(sapi.getStoryFeed).not.toHaveBeenCalled();
  });

  it("an older server that says nothing about the switch means on", async () => {
    await renderStories({ own: [], authors: [] }, { media: true });
    expect(await screen.findByRole("button", { name: "New story" })).toBeInTheDocument();
    expect(sapi.getStoryFeed).toHaveBeenCalled();
  });
});

describe("stories with several slides", () => {
  async function twoSlides() {
    const user = userEvent.setup();
    await renderStories();
    await user.click(await screen.findByRole("button", { name: "New story" }));
    const composer = await screen.findByRole("dialog", { name: "New story" });
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    fireEvent.change(within(composer).getByRole("textbox", { name: "Story text" }), { target: { value: "First slide" } });
    await user.click(within(composer).getByRole("button", { name: "Add a slide" }));
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    fireEvent.change(within(composer).getByRole("textbox", { name: "Story text" }), { target: { value: "Second slide" } });
    return { user, composer };
  }

  it("are shared one after the other with the same audience and lifetime, each sealed on its own", async () => {
    const { user, composer } = await twoSlides();
    await user.selectOptions(within(composer).getByRole("combobox", { name: "Disappears after" }), "6 hours");
    await user.click(within(composer).getByRole("button", { name: "Share 2 slides" }));
    await waitFor(() => expect(sapi.createStory).toHaveBeenCalledTimes(2));

    const sent = vi.mocked(sapi.createStory).mock.calls.map((c) => c[0]);
    expect(sent[0].id).not.toBe(sent[1].id);
    expect(sent.map((s) => s.ttlSeconds)).toEqual([21600, 21600]);
    expect(sent.map((s) => s.audience)).toEqual(["friends", "friends"]);
    const words = sent.map((s) => {
      const wrapped = crypto.fromBase64(s.keys.find((k) => k.userId === ME)!.wrappedKey);
      const key = unwrapStoryKey(s.id, wrapped, me.publicKey, me.privateKey);
      return openStoryMeta(s.id, key, "text", crypto.fromBase64(s.metaCiphertext), crypto.fromBase64(s.metaNonce)).text;
    });
    // In the order they were made, and the requests held no readable words.
    expect(words).toEqual(["First slide", "Second slide"]);
    expect(JSON.stringify(sent)).not.toMatch(/First slide|Second slide/);
    await waitFor(() => expect(screen.queryByRole("dialog", { name: "New story" })).not.toBeInTheDocument());
  });

  it("leave out a slide nobody filled in", async () => {
    const user = userEvent.setup();
    await renderStories();
    await user.click(await screen.findByRole("button", { name: "New story" }));
    const composer = await screen.findByRole("dialog", { name: "New story" });
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    fireEvent.change(within(composer).getByRole("textbox", { name: "Story text" }), { target: { value: "Only one" } });
    await user.click(within(composer).getByRole("button", { name: "Add a slide" }));
    // Slide 2 is a text slide nobody wrote anything in; back on slide 1, only one slide counts.
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    await user.click(within(composer).getByRole("button", { name: "Slide 1" }));
    await user.click(within(composer).getByRole("button", { name: "Share" }));
    await waitFor(() => expect(sapi.createStory).toHaveBeenCalledTimes(1));
  });

  it("when the connection drops part-way, what wasn't shared is kept as a draft and can be sent again", async () => {
    vi.mocked(sapi.createStory)
      .mockImplementationOnce(async (s) => published(s))
      .mockRejectedValueOnce(new Error("offline"));
    const { user, composer } = await twoSlides();
    await user.click(within(composer).getByRole("button", { name: "Share 2 slides" }));

    expect(await within(composer).findByText(/1 of 2 slides shared\. offline/)).toBeInTheDocument();
    expect(within(composer).getByText(/What's left is saved as a draft/)).toBeInTheDocument();
    // The composer is still open on what is left, and a draft holds exactly that, encrypted.
    expect(within(composer).getByRole("textbox", { name: "Story text" })).toHaveValue("Second slide");
    const drafts = await listDrafts(myIdentity());
    expect(drafts).toHaveLength(1);
    expect(drafts[0].slideCount).toBe(1);

    // Trying again shares the remaining slide and removes the draft.
    await user.click(within(composer).getByRole("button", { name: "Share" }));
    await waitFor(() => expect(sapi.createStory).toHaveBeenCalledTimes(3));
    await waitFor(async () => expect(await listDrafts(myIdentity())).toEqual([]));
  });

  it("an unfinished slide stops the share and says which one", async () => {
    const user = userEvent.setup();
    await renderStories();
    await user.click(await screen.findByRole("button", { name: "New story" }));
    const composer = await screen.findByRole("dialog", { name: "New story" });
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    fireEvent.change(within(composer).getByRole("textbox", { name: "Story text" }), { target: { value: "Words" } });
    await user.click(within(composer).getByRole("button", { name: "Add a slide" }));
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    // Slide 2 is a text slide with no words, but with an emoji on it so that it counts as started.
    await user.click(within(composer).getByRole("button", { name: "Add an emoji" }));
    const picker = await screen.findByRole("dialog", { name: "Choose an emoji" });
    // The picker fills in a moment after it opens.
    await waitFor(() => expect(picker.querySelector('button[class*="emoji"]')).not.toBeNull());
    await user.click(picker.querySelector('button[class*="emoji"]') as HTMLElement);
    expect(await within(composer).findByLabelText(/^Sticker /)).toBeInTheDocument();
    // Share is held back while the slide being edited is that one; from slide 1 it is pressed and says which slide is unfinished.
    await user.click(within(composer).getByRole("button", { name: "Slide 1" }));
    await user.click(within(composer).getByRole("button", { name: "Share 2 slides" }));
    expect(await within(composer).findByText("Slide 2 needs some text.")).toBeInTheDocument();
    expect(sapi.createStory).not.toHaveBeenCalled();
  });
});

describe("an audio story in the viewer", () => {
  let clock = 0;
  beforeAll(() => {
    Object.defineProperty(HTMLMediaElement.prototype, "currentTime", { configurable: true, get: () => clock, set: () => {} });
    Object.defineProperty(HTMLMediaElement.prototype, "duration", { configurable: true, get: () => 4 });
    HTMLMediaElement.prototype.play = vi.fn(async () => {});
    HTMLMediaElement.prototype.pause = vi.fn();
  });

  function bobsAudio(captions: { startMs: number; endMs: number; text: string }[], waveform: number[]) {
    const id = newStoryId();
    const key = newStoryKey();
    const meta = {
      v: 1,
      kind: "audio",
      text: "Road trip",
      background: "night",
      overlays: [],
      closeFriends: false,
      waveform,
      captions,
      media: { key: "a2V5a2V5a2V5a2V5", header: "aGVhZGVyaGVhZGVy", sha256: "a".repeat(64), encryptedSize: 5000, chunkSize: 65536, mime: "audio/webm", width: 1, height: 1, durationMs: 4000 },
    } as unknown as StoryMeta;
    const sealed = sealStoryMeta(id, key, meta);
    const dto: sapi.StoryDto = {
      id,
      authorId: BOB,
      kind: "audio",
      metaCiphertext: crypto.toBase64(sealed.ciphertext),
      metaNonce: crypto.toBase64(sealed.nonce),
      mediaSize: 5000,
      hasMedia: true,
      allowReplies: true,
      createdAt: new Date(Date.now() - 600_000).toISOString(),
      expiresAt: new Date(Date.now() + 3 * 3600_000).toISOString(),
      wrappedKey: crypto.toBase64(wrapStoryKey(id, key, me.publicKey, bob.privateKey)),
    };
    return { authors: [{ authorId: BOB, stories: [{ ...dto, seen: false }] }], own: [] } as sapi.FeedDto;
  }

  const waitForAudio = async (viewer: HTMLElement) => {
    await waitFor(() => expect(viewer.querySelector("audio")).not.toBeNull());
    return viewer.querySelector("audio") as HTMLAudioElement;
  };

  it("draws the picture of the sound and shows each caption only while it is spoken", async () => {
    clock = 0.5;
    const waveform = Array.from({ length: 24 }, (_, i) => 10 + ((i * 7) % 80));
    const user = userEvent.setup();
    await renderStories(bobsAudio([{ startMs: 0, endMs: 2000, text: "First line" }, { startMs: 2000, endMs: 4000, text: "Second line" }], waveform));
    await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
    const viewer = await screen.findByRole("dialog", { name: "bob's story" });

    expect(within(viewer).getByText("Road trip")).toBeInTheDocument();
    expect(viewer.querySelectorAll('[class*="waveform"] > span')).toHaveLength(24);
    // The clock starts when the sound has loaded (jsdom never loads media by itself).
    fireEvent.loadedData(await waitForAudio(viewer));
    expect(await within(viewer).findByText("First line")).toBeInTheDocument();
    expect(within(viewer).queryByText("Second line")).not.toBeInTheDocument();

    clock = 2.5;
    expect(await within(viewer).findByText("Second line")).toBeInTheDocument();
    expect(within(viewer).queryByText("First line")).not.toBeInTheDocument();

    clock = 3.99;
    expect(await within(viewer).findByText("Second line")).toBeInTheDocument();
  });

  it("shows no caption where there is none", async () => {
    clock = 1;
    const user = userEvent.setup();
    await renderStories(bobsAudio([{ startMs: 3000, endMs: 4000, text: "Late line" }], Array.from({ length: 16 }, () => 50)));
    await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
    const viewer = await screen.findByRole("dialog", { name: "bob's story" });
    fireEvent.loadedData(await waitForAudio(viewer));
    await new Promise((r) => setTimeout(r, 150));
    expect(within(viewer).queryByText("Late line")).not.toBeInTheDocument();
    // The same loop shows it once its time comes, so the silence above was the loop running, not stalled.
    clock = 3.5;
    expect(await within(viewer).findByText("Late line")).toBeInTheDocument();
  });
});

describe("a story frame", () => {
  it("moves an emoji or text with its own element, so the animation never overwrites its placement", async () => {
    const { StoryFrame } = await import("../components/stories/StoryFrame.js");
    const meta = {
      v: 1,
      kind: "text",
      text: "Big news!",
      background: "ember",
      font: "strong",
      closeFriends: false,
      overlays: [{ type: "emoji", text: "📣", x: 0.5, y: 0.2, scale: 1.6, rotation: 10, color: "white", boxed: false, font: "classic", anim: "pulse" }],
    } as unknown as StoryMeta;
    const { container } = render(<StoryFrame meta={meta} mediaUrl={null} label="Story" />);
    const outer = screen.getByText("📣").parentElement!;
    // The placed element carries the position and turn; the span inside carries the motion.
    expect(outer.getAttribute("style")).toMatch(/rotate\(10deg\)/);
    expect(outer.className).not.toMatch(/anim_/);
    expect(screen.getByText("📣").className).toMatch(/anim_pulse/);
    expect(container.querySelector("script")).toBeNull();
  });
});
