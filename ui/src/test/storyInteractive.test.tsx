import "fake-indexeddb/auto";
import { act, cleanup, fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import * as sapi from "../lib/storiesApi.js";
import { listDrafts } from "../lib/storyDrafts.js";
import {
  DEFAULT_STORY_PREFS,
  newStickerId,
  newStoryId,
  newStoryKey,
  openAnswer,
  openStoryMeta,
  openTotals,
  sealAnonymousAnswer,
  sealAnswer,
  sealStoryMeta,
  sealStoryPrefs,
  sealTotals,
  unwrapStoryKey,
  wrapStoryKey,
  type PollSticker,
  type QuestionSticker,
  type QuizSticker,
  type SliderSticker,
  type StorySticker,
  type StoryMeta,
} from "../lib/storyCrypto.js";

vi.mock("../lib/storiesApi.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/storiesApi.js")>()),
  getStoryConfig: vi.fn(),
  getStoryFeed: vi.fn(),
  getStoryPrefs: vi.fn(),
  putStoryPrefs: vi.fn(),
  createStory: vi.fn(async (s: sapi.NewStoryDto) => ({ id: s.id, published: true, expiresAt: new Date(Date.now() + 86400_000).toISOString() })),
  uploadStoryMedia: vi.fn(async () => {}),
  deleteStory: vi.fn(async () => {}),
  markStoryViewed: vi.fn(async () => {}),
  getStoryKeyGaps: vi.fn(async () => []),
  addStoryKeys: vi.fn(async () => ({ added: 0 })),
  getStoryArchive: vi.fn(async () => []),
  revokeStoryAccess: vi.fn(async () => ({ removed: 0 })),
  getHighlights: vi.fn(async () => []),
  putStoryAnswer: vi.fn(async () => ({ revision: 0 })),
  getStoryAnswers: vi.fn(async () => []),
  getMyStoryAnswers: vi.fn(async () => []),
  getStoryTotals: vi.fn(async () => []),
  putStoryTotals: vi.fn(async () => undefined),
  reportStory: vi.fn(async () => ({ reported: true })),
  reportStoryAnswer: vi.fn(async () => ({ reported: true })),
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
let carol: Keys;
const ME = "00000000-0000-4000-8000-00000000000a";
const BOB = "00000000-0000-4000-8000-00000000000b";
const CAROL = "00000000-0000-4000-8000-00000000000c";

beforeAll(async () => {
  await crypto.ready();
  me = crypto.generateKeyPair();
  bob = crypto.generateKeyPair();
  carol = crypto.generateKeyPair();
});

const dispatch = vi.fn();
vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({
    state: {
      authStatus: "signed-in",
      identity: { userId: ME, handle: "me", avatarUrl: null, publicKey: me.publicKey, privateKey: me.privateKey, role: "user", subscriptionTier: "free" },
      conversations: [{ kind: "dm", id: "c1", userId: BOB, handle: "bob", publicKey: undefined, conversationKey: new Uint8Array(32), messages: [] }],
      settings: {},
    },
    dispatch,
  }),
}));

const place = { x: 0.5, y: 0.5, scale: 1, rotation: 0 };
const poll = (over: Partial<PollSticker> = {}): PollSticker => ({ ...place, id: newStickerId(), type: "poll", question: "Pizza or tacos?", options: ["Pizza", "Tacos"], results: "everyone", ...over });
const quiz = (over: Partial<QuizSticker> = {}): QuizSticker => ({ ...place, id: newStickerId(), type: "quiz", question: "2 + 2?", options: ["3", "4"], correct: 1, explanation: "Basic maths", results: "everyone", ...over });
const question = (over: Partial<QuestionSticker> = {}): QuestionSticker => ({ ...place, id: newStickerId(), type: "question", prompt: "Ask me anything", anonymous: false, ...over });
const slider = (over: Partial<SliderSticker> = {}): SliderSticker => ({ ...place, id: newStickerId(), type: "slider", prompt: "How was it?", emoji: "😍", results: "everyone", ...over });

/** A text story by `author` for `viewer`, carrying stickers. */
function storyDto(author: Keys & { userId: string }, viewer: Keys, stickers: StorySticker[], over: Partial<sapi.StoryDto> = {}): { dto: sapi.StoryDto; id: string; key: Uint8Array } {
  const id = newStoryId();
  const key = newStoryKey();
  const meta: StoryMeta = { v: 1, kind: "text", text: "Weekend plans", background: "sunset", font: "classic", overlays: [], stickers, closeFriends: false };
  const sealed = sealStoryMeta(id, key, meta);
  return {
    id,
    key,
    dto: {
      id,
      authorId: author.userId,
      kind: "text",
      metaCiphertext: crypto.toBase64(sealed.ciphertext),
      metaNonce: crypto.toBase64(sealed.nonce),
      mediaSize: null,
      hasMedia: false,
      allowReplies: true,
      createdAt: new Date(Date.now() - 600_000).toISOString(),
      expiresAt: new Date(Date.now() + 3 * 3600_000).toISOString(),
      wrappedKey: crypto.toBase64(wrapStoryKey(id, key, viewer.publicKey, author.privateKey)),
      ...over,
    },
  };
}

type Setup = {
  feed?: sapi.FeedDto;
  config?: sapi.StoryConfigDto;
  friends?: { userId: string; username: string; publicKey: string; avatarUrl: null }[];
};

async function renderStories(setup: Setup = {}) {
  const { StoriesProvider } = await import("../state/StoriesContext.js");
  const { StoryTray } = await import("../components/stories/StoryTray.js");
  const { StoryOverlays } = await import("../components/stories/StoryOverlays.js");
  const sealed = sealStoryPrefs(DEFAULT_STORY_PREFS, me.privateKey);
  vi.mocked(sapi.getStoryConfig).mockResolvedValue(setup.config ?? { media: true, interactive: true, ttlSeconds: [3600, 6 * 3600, 12 * 3600, 86400, 2 * 86400, 7 * 86400] });
  vi.mocked(sapi.getStoryPrefs).mockResolvedValue({ prefsCiphertext: crypto.toBase64(sealed.ciphertext), prefsNonce: crypto.toBase64(sealed.nonce), archiveEnabled: false, revision: 1 });
  vi.mocked(api.listFriends).mockResolvedValue(setup.friends ?? [{ userId: BOB, username: "bob", publicKey: crypto.toBase64(bob.publicKey), avatarUrl: null }]);
  vi.mocked(sapi.getStoryFeed).mockResolvedValue(setup.feed ?? { own: [], authors: [] });
  return render(
    <StoriesProvider>
      <StoryTray />
      <StoryOverlays />
    </StoriesProvider>
  );
}

const bobsFeed = (stickers: StorySticker[]) => {
  const s = storyDto({ userId: BOB, ...bob }, me, stickers);
  return { story: s, feed: { own: [], authors: [{ authorId: BOB, stories: [{ ...s.dto, seen: false }] }] } as sapi.FeedDto };
};

async function openBobsStory(stickers: StorySticker[], setup: Setup = {}) {
  const user = userEvent.setup();
  const { story, feed } = bobsFeed(stickers);
  await renderStories({ ...setup, feed });
  await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
  const viewer = await screen.findByRole("dialog", { name: "bob's story" });
  // Keep the story on screen while the test works.
  await user.click(within(viewer).getByRole("button", { name: "Pause" }));
  return { user, viewer, story };
}

beforeEach(() => {
  for (const fn of Object.values(sapi)) if (typeof fn === "function" && "mockClear" in fn) (fn as unknown as { mockClear(): void }).mockClear();
  vi.mocked(sapi.getStoryAnswers).mockResolvedValue([]);
  vi.mocked(sapi.getMyStoryAnswers).mockResolvedValue([]);
  vi.mocked(sapi.getStoryTotals).mockResolvedValue([]);
  localStorage.clear();
});

afterEach(async () => {
  cleanup();
  await new Promise<void>((resolve) => {
    const req = indexedDB.deleteDatabase("redvault-story-drafts");
    req.onsuccess = req.onerror = req.onblocked = () => resolve();
  });
});

describe("answering stickers in the viewer", () => {
  it("a poll vote goes to the server sealed, and the author can read it", async () => {
    const p = poll();
    const { user, viewer, story } = await openBobsStory([p]);
    expect(within(viewer).getByText("Pizza or tacos?")).toBeInTheDocument();
    await user.click(within(viewer).getByRole("button", { name: "Tacos" }));
    await waitFor(() => expect(sapi.putStoryAnswer).toHaveBeenCalledTimes(1));
    const [storyId, stickerId, body] = vi.mocked(sapi.putStoryAnswer).mock.calls[0];
    expect([storyId, stickerId]).toEqual([story.id, p.id]);
    expect(body).toMatchObject({ anonymous: false, final: false });
    expect(openAnswer(storyId, p, crypto.fromBase64(body.envelope), me.publicKey, bob.privateKey)).toEqual({ k: "poll", choice: 1 });
    expect(await within(viewer).findByText("Your vote is in. You can change it.")).toBeInTheDocument();
  });

  it("a quiz answer is final, and shows whether it was right and why", async () => {
    const { user, viewer } = await openBobsStory([quiz()]);
    await user.click(within(viewer).getByRole("button", { name: "3" }));
    await waitFor(() => expect(sapi.putStoryAnswer).toHaveBeenCalledTimes(1));
    expect(vi.mocked(sapi.putStoryAnswer).mock.calls[0][2].final).toBe(true);
    expect(await within(viewer).findByText("Not quite")).toBeInTheDocument();
    expect(within(viewer).getByText("Basic maths")).toBeInTheDocument();
    // It can't be answered a second time.
    expect(within(viewer).getByRole("button", { name: "4" })).toBeDisabled();
  });

  it("a question marked without a name says so before and after, and is sent without a sender", async () => {
    const q = question({ anonymous: true });
    const { user, viewer, story } = await openBobsStory([q]);
    expect(within(viewer).getByText(/Sent without your name\. RedVault's server can still tell it was you\./)).toBeInTheDocument();
    await user.type(within(viewer).getByRole("textbox", { name: /Your answer to: Ask me anything/ }), "Love it");
    await user.click(within(viewer).getByRole("button", { name: "Send" }));
    await waitFor(() => expect(sapi.putStoryAnswer).toHaveBeenCalled());
    const body = vi.mocked(sapi.putStoryAnswer).mock.calls[0][2];
    expect(body).toMatchObject({ anonymous: true, final: true });
    // Only Bob's keys open it, and it holds nothing about Me.
    const { openAnonymousAnswer } = await import("../lib/storyCrypto.js");
    expect(openAnonymousAnswer(story.id, q, crypto.fromBase64(body.envelope), bob.publicKey, bob.privateKey)).toEqual({ k: "question", text: "Love it" });
    expect(await within(viewer).findByText(/Sent ✓/)).toBeInTheDocument();
  });

  it("a slider is sent when you let go", async () => {
    const sl = slider();
    const { viewer, story } = await openBobsStory([sl]);
    const input = within(viewer).getByRole("slider");
    fireEvent.change(input, { target: { value: "80" } });
    fireEvent.pointerUp(input);
    await waitFor(() => expect(sapi.putStoryAnswer).toHaveBeenCalledTimes(1));
    const body = vi.mocked(sapi.putStoryAnswer).mock.calls[0][2];
    expect(openAnswer(story.id, sl, crypto.fromBase64(body.envelope), me.publicKey, bob.privateKey)).toEqual({ k: "slider", value: 80 });
  });

  it("shows the totals the author published, and nothing that doesn't verify", async () => {
    const p = poll();
    const { story, feed } = bobsFeed([p]);
    const good = sealTotals(story.id, p.id, 1, { total: 4, counts: [1, 3] }, story.key);
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([{ stickerId: p.id, revision: 1, nonce: crypto.toBase64(good.nonce), ciphertext: crypto.toBase64(good.ciphertext) }]);
    const user = userEvent.setup();
    await renderStories({ feed });
    await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
    const viewer = await screen.findByRole("dialog", { name: "bob's story" });
    await within(viewer).findByText("Pizza or tacos?");
    // Nobody is shown the result before they have answered.
    await waitFor(() => expect(sapi.getStoryTotals).toHaveBeenCalled());
    expect(within(viewer).queryByText("75%")).not.toBeInTheDocument();
    await user.click(within(viewer).getByRole("button", { name: "Pizza" }));
    expect(await within(viewer).findByText("75%")).toBeInTheDocument();
    expect(within(viewer).getByText("25%")).toBeInTheDocument();
  });

  it("results marked 'only me' are not shown to viewers, whatever the server serves", async () => {
    const p = poll({ results: "author" });
    const { story, feed } = bobsFeed([p]);
    const served = sealTotals(story.id, p.id, 1, { total: 4, counts: [1, 3] }, story.key);
    vi.mocked(sapi.getStoryTotals).mockResolvedValue([{ stickerId: p.id, revision: 1, nonce: crypto.toBase64(served.nonce), ciphertext: crypto.toBase64(served.ciphertext) }]);
    const user = userEvent.setup();
    await renderStories({ feed });
    await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
    const viewer = await screen.findByRole("dialog", { name: "bob's story" });
    await within(viewer).findByText("Only the author sees the results");
    expect(within(viewer).queryByText("75%")).not.toBeInTheDocument();
  });

  it("with stickers switched off on the server they are shown but can't be pressed", async () => {
    const { viewer } = await openBobsStory([poll()], { config: { media: true } });
    expect(within(viewer).getByText("Pizza or tacos?")).toBeInTheDocument();
    expect(within(viewer).getByRole("button", { name: "Pizza" })).toBeDisabled();
    expect(sapi.getMyStoryAnswers).not.toHaveBeenCalled();
  });

  it("a story whose time is up takes no answers", async () => {
    const p = poll();
    const { feed } = bobsFeed([p]);
    feed.authors[0].stories[0].expiresAt = new Date(Date.now() - 1000).toISOString();
    const user = userEvent.setup();
    await renderStories({ feed });
    await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
    const viewer = await screen.findByRole("dialog", { name: "bob's story" });
    expect(within(viewer).getByRole("button", { name: "Pizza" })).toBeDisabled();
  });
});

describe("what a story can carry", () => {
  it("markup in a sticker is only ever text", async () => {
    const img = "<img src=x onerror=window.__pwned=1>";
    const script = "<script>window.__pwned=2</script>";
    const stickers: StorySticker[] = [
      poll({ question: img, options: [script, "<b>bold</b>"] }),
      { ...place, id: newStickerId(), type: "location", label: img },
      { ...place, id: newStickerId(), type: "countdown", title: script, endsAt: Date.now() + 3600_000 },
    ];
    const { viewer } = await openBobsStory(stickers);
    expect(within(viewer).getAllByText(img).length).toBeGreaterThan(0);
    expect(within(viewer).getAllByText(script).length).toBeGreaterThan(0);
    expect(within(viewer).getByText("<b>bold</b>")).toBeInTheDocument();
    expect(viewer.querySelector('img[src="x"]')).toBeNull();
    expect(viewer.querySelector("script")).toBeNull();
    expect(viewer.querySelector("b")).toBeNull();
    expect((window as unknown as { __pwned?: number }).__pwned).toBeUndefined();
  });

  it("a link asks first, shows where it goes, and opens without handing over the opener", async () => {
    const open = vi.spyOn(window, "open").mockImplementation(() => null);
    const stickers: StorySticker[] = [{ ...place, id: newStickerId(), type: "link", url: "https://example.com/menu?x=1", label: "Our menu" }];
    const { user, viewer } = await openBobsStory(stickers);
    await user.click(within(viewer).getByRole("button", { name: "Link: Our menu" }));
    const confirm = await screen.findByRole("alertdialog", { name: "Open this link?" });
    expect(within(confirm).getByText("example.com")).toBeInTheDocument();
    expect(within(confirm).getByText("https://example.com/menu?x=1")).toBeInTheDocument();
    expect(open).not.toHaveBeenCalled();
    await user.click(within(confirm).getByRole("button", { name: "Open" }));
    expect(open).toHaveBeenCalledWith("https://example.com/menu?x=1", "_blank", "noopener,noreferrer");
    open.mockRestore();
  });

  it("a link that isn't https never reaches the screen", async () => {
    const meta = { v: 1, kind: "text", text: "x", overlays: [], closeFriends: false, stickers: [{ ...place, id: newStickerId(), type: "link", url: "javascript:alert(1)", label: "Click me" }] };
    const id = newStoryId();
    const key = newStoryKey();
    const sealed = sealStoryMeta(id, key, meta as unknown as StoryMeta);
    const feed: sapi.FeedDto = {
      own: [],
      authors: [
        {
          authorId: BOB,
          stories: [
            {
              id,
              authorId: BOB,
              kind: "text",
              metaCiphertext: crypto.toBase64(sealed.ciphertext),
              metaNonce: crypto.toBase64(sealed.nonce),
              mediaSize: null,
              hasMedia: false,
              allowReplies: true,
              createdAt: new Date().toISOString(),
              expiresAt: new Date(Date.now() + 3600_000).toISOString(),
              wrappedKey: crypto.toBase64(wrapStoryKey(id, key, me.publicKey, bob.privateKey)),
              seen: false,
            },
          ],
        },
      ],
    };
    const user = userEvent.setup();
    await renderStories({ feed });
    await user.click(await screen.findByRole("button", { name: "bob's story, 1 new" }));
    const viewer = await screen.findByRole("dialog", { name: "bob's story" });
    expect(within(viewer).queryByText(/Click me/)).not.toBeInTheDocument();
  });

  it("a mention shows the friend's name, a date shows in your own format", async () => {
    const stickers: StorySticker[] = [
      { ...place, id: newStickerId(), type: "mention", userId: BOB },
      { ...place, id: newStickerId(), type: "date", at: Date.UTC(2030, 5, 15, 12, 0), style: "date" },
    ];
    const { viewer } = await openBobsStory(stickers);
    expect(within(viewer).getByText("@bob")).toBeInTheDocument();
    expect(within(viewer).getByText(/2030/)).toBeInTheDocument();
  });
});

describe("the author's side", () => {
  async function openOwnStory(stickers: StorySticker[], answers: (story: { id: string }) => sapi.AnswerRowDto[]) {
    const user = userEvent.setup();
    const s = storyDto({ userId: ME, ...me }, me, stickers);
    vi.mocked(sapi.getStoryAnswers).mockResolvedValue(answers(s));
    await renderStories({
      feed: { own: [{ ...s.dto, viewCount: 0 }], authors: [] },
      friends: [
        { userId: BOB, username: "bob", publicKey: crypto.toBase64(bob.publicKey), avatarUrl: null },
        { userId: CAROL, username: "carol", publicKey: crypto.toBase64(carol.publicKey), avatarUrl: null },
      ],
    });
    await user.click(await screen.findByRole("button", { name: /Your story, 1 post/ }));
    const viewer = await screen.findByRole("dialog", { name: "Your story" });
    await user.click(within(viewer).getByRole("button", { name: "Pause" }));
    return { user, viewer, story: s };
  }
  const row = (storyId: string, stickerId: string, responderId: string | null, envelope: Uint8Array): sapi.AnswerRowDto => ({
    stickerId,
    responderId,
    anonymous: responderId === null,
    envelope: crypto.toBase64(envelope),
    revision: 0,
    respondedAt: new Date().toISOString(),
  });

  it("opens the answers sealed to you, publishes only the totals, and shows who said what only where it was signed", async () => {
    const p = poll();
    const q = question({ anonymous: true });
    const { user, viewer, story } = await openOwnStory([p, q], (s) => [
      row(s.id, p.id, BOB, sealAnswer(s.id, p, { k: "poll", choice: 0 }, me.publicKey, bob.privateKey)),
      row(s.id, p.id, CAROL, sealAnswer(s.id, p, { k: "poll", choice: 0 }, me.publicKey, carol.privateKey)),
      row(s.id, q.id, null, sealAnonymousAnswer(s.id, q, { k: "question", text: "You are great" }, me.publicKey)),
    ]);
    // The totals went out sealed with the story's key: counts, not people.
    await waitFor(() => expect(sapi.putStoryTotals).toHaveBeenCalled());
    const [storyId, stickerId, body] = vi.mocked(sapi.putStoryTotals).mock.calls[0];
    expect([storyId, stickerId, body.revision]).toEqual([story.id, p.id, 1]);
    expect(openTotals(storyId, p, 1, crypto.fromBase64(body.ciphertext), crypto.fromBase64(body.nonce), story.key)).toEqual({ total: 2, counts: [2, 0] });
    expect(JSON.stringify(body)).not.toContain(BOB);
    // The question's totals are never published (it has none to show).
    expect(vi.mocked(sapi.putStoryTotals).mock.calls.every((c) => c[1] === p.id)).toBe(true);

    await user.click(await within(viewer).findByRole("button", { name: "Answers (3)" }));
    const sheet = await screen.findByRole("region", { name: "Answers to your stickers" });
    expect(within(sheet).getByText("Anonymous")).toBeInTheDocument();
    expect(within(sheet).getByText("You are great")).toBeInTheDocument();
    expect(within(sheet).getByText("2 answers", { exact: false })).toBeInTheDocument();
  });

  it("can report an abusive answer even when it came without a name", async () => {
    const q = question({ anonymous: true });
    const env = (id: string) => sealAnonymousAnswer(id, q, { k: "question", text: "Something nasty" }, me.publicKey);
    let sent!: Uint8Array;
    const { user, viewer } = await openOwnStory([q], (s) => {
      sent = env(s.id);
      return [row(s.id, q.id, null, sent)];
    });
    await user.click(await within(viewer).findByRole("button", { name: "Answers (1)" }));
    const sheet = await screen.findByRole("region", { name: "Answers to your stickers" });
    await user.click(within(sheet).getByRole("button", { name: "Report" }));
    await user.type(within(sheet).getByRole("textbox", { name: "What's wrong with this answer?" }), "Harassment");
    await user.click(within(sheet).getByRole("checkbox", { name: /Include the answer's text/ }));
    await user.click(within(sheet).getByRole("button", { name: "Send report" }));
    await waitFor(() => expect(sapi.reportStoryAnswer).toHaveBeenCalledTimes(1));
    const [, body] = vi.mocked(sapi.reportStoryAnswer).mock.calls[0];
    expect(body).toEqual({ stickerId: q.id, envelope: crypto.toBase64(sent), reason: "Harassment", evidence: "Something nasty" });
  });

  it("answers that don't verify are not counted and the author is told", async () => {
    const p = poll();
    const { user, viewer } = await openOwnStory([p], (s) => {
      const damaged = sealAnswer(s.id, p, { k: "poll", choice: 1 }, me.publicKey, bob.privateKey);
      damaged[damaged.length - 2] ^= 1;
      return [row(s.id, p.id, BOB, damaged)];
    });
    await user.click(await within(viewer).findByRole("button", { name: "Answers (0)" }));
    const sheet = await screen.findByRole("region", { name: "Answers to your stickers" });
    expect(within(sheet).getByText(/1 answer couldn't be opened/)).toBeInTheDocument();
  });
});

describe("reporting a story", () => {
  async function openReport(stickers: StorySticker[]) {
    const { user, viewer } = await openBobsStory(stickers);
    await user.click(within(viewer).getByRole("button", { name: "More" }));
    await user.click(within(viewer).getByRole("menuitem", { name: "Report story" }));
    const sheet = await screen.findByRole("region", { name: "Report this story" });
    return { user, sheet };
  }

  it("sends nothing from the story unless asked, and shows what would be sent", async () => {
    const { user, sheet } = await openReport([poll()]);
    await user.selectOptions(within(sheet).getByRole("combobox", { name: "What's wrong?" }), "Spam");
    await user.click(within(sheet).getByRole("button", { name: "Send report" }));
    await waitFor(() => expect(sapi.reportStory).toHaveBeenCalledTimes(1));
    expect(vi.mocked(sapi.reportStory).mock.calls[0][1]).toEqual({ reason: "Spam" });
  });

  it("includes the words on the story only when the box is ticked", async () => {
    const { user, sheet } = await openReport([poll()]);
    const box = within(sheet).getByRole("checkbox", { name: /Include the words on this story/ });
    expect(within(sheet).queryByText(/Weekend plans \| Pizza or tacos\?/)).not.toBeInTheDocument();
    await user.click(box);
    expect(within(sheet).getByText(/Weekend plans \| Pizza or tacos\? \[Pizza \/ Tacos\]/)).toBeInTheDocument();
    await user.type(within(sheet).getByRole("textbox", { name: "Details (optional)" }), "again");
    await user.click(within(sheet).getByRole("button", { name: "Send report" }));
    await waitFor(() => expect(sapi.reportStory).toHaveBeenCalledTimes(1));
    expect(vi.mocked(sapi.reportStory).mock.calls[0][1]).toEqual({ reason: "Spam: again", evidence: "Weekend plans | Pizza or tacos? [Pizza / Tacos]" });
  });

  it("says so when it was already reported", async () => {
    vi.mocked(sapi.reportStory).mockRejectedValueOnce(new api.ApiError(409, "already_reported", "dup"));
    const { user, sheet } = await openReport([]);
    await user.click(within(sheet).getByRole("button", { name: "Send report" }));
    expect(await within(sheet).findByText("You already reported this story.")).toBeInTheDocument();
  });
});

describe("making a story with stickers", () => {
  async function openComposer(setup: Setup = {}) {
    const user = userEvent.setup();
    await renderStories(setup);
    await user.click(await screen.findByRole("button", { name: "New story" }));
    const composer = await screen.findByRole("dialog", { name: "New story" });
    await user.click(within(composer).getByRole("button", { name: /Text/ }));
    fireEvent.change(within(composer).getByRole("textbox", { name: "Story text" }), { target: { value: "Friday plans" } });
    return { user, composer };
  }

  async function addPoll(user: ReturnType<typeof userEvent.setup>, composer: HTMLElement) {
    await user.click(within(composer).getByRole("button", { name: /Add a sticker/ }));
    const sheet = await screen.findByRole("dialog", { name: "Add a sticker" });
    await user.click(within(sheet).getByRole("button", { name: /Poll/ }));
    const editor = await screen.findByRole("group", { name: "Poll sticker" });
    await user.type(within(editor).getByRole("textbox", { name: "Question" }), "Pizza or tacos?");
    await user.type(within(editor).getByRole("textbox", { name: "Option 1" }), "Pizza");
    await user.type(within(editor).getByRole("textbox", { name: "Option 2" }), "Tacos");
    await user.click(within(editor).getByRole("button", { name: "Add" }));
  }

  it("a poll, a one-hour life and two chosen friends: all of it inside the encrypted story", async () => {
    const { user, composer } = await openComposer({
      friends: [
        { userId: BOB, username: "bob", publicKey: crypto.toBase64(bob.publicKey), avatarUrl: null },
        { userId: CAROL, username: "carol", publicKey: crypto.toBase64(carol.publicKey), avatarUrl: null },
      ],
    });
    await addPoll(user, composer);
    expect(within(composer).getByRole("button", { name: /Poll: Pizza or tacos\?/ })).toBeInTheDocument();

    await user.selectOptions(within(composer).getByRole("combobox", { name: "Disappears after" }), "1 hour");
    await user.click(within(composer).getByRole("radio", { name: /Choose people/ }));
    const people = await screen.findByRole("dialog", { name: "Choose who sees this story" });
    await user.click(within(people).getByRole("checkbox", { name: "carol" }));
    await user.click(within(people).getByRole("button", { name: "Done" }));
    expect(within(composer).getByTestId("story-summary")).toHaveTextContent("Only the 1 friend you chose can see this · gone after 1 hour · end-to-end encrypted");

    await user.click(within(composer).getByRole("button", { name: "Share" }));
    await waitFor(() => expect(sapi.createStory).toHaveBeenCalledTimes(1));
    const sent = vi.mocked(sapi.createStory).mock.calls[0][0];
    expect(sent.ttlSeconds).toBe(3600);
    expect(sent.audience).toBe("custom");
    expect(sent.keys.map((k) => k.userId).sort()).toEqual([ME, CAROL].sort());
    // Carol opens it and finds the poll; the request itself never held the question.
    const key = unwrapStoryKey(sent.id, crypto.fromBase64(sent.keys.find((k) => k.userId === CAROL)!.wrappedKey), me.publicKey, carol.privateKey);
    const meta = openStoryMeta(sent.id, key, "text", crypto.fromBase64(sent.metaCiphertext), crypto.fromBase64(sent.metaNonce));
    expect(meta.stickers).toHaveLength(1);
    expect(meta.stickers![0]).toMatchObject({ type: "poll", question: "Pizza or tacos?", options: ["Pizza", "Tacos"], results: "everyone" });
    expect(JSON.stringify(sent)).not.toMatch(/Pizza|tacos|Friday/);
  });

  it("polls and questions are not offered when the server has them off", async () => {
    const { user, composer } = await openComposer({ config: { media: true } });
    await user.click(within(composer).getByRole("button", { name: /Add a sticker/ }));
    const sheet = await screen.findByRole("dialog", { name: "Add a sticker" });
    expect(within(sheet).queryByRole("button", { name: /Poll/ })).not.toBeInTheDocument();
    expect(within(sheet).getByRole("button", { name: /Countdown/ })).toBeInTheDocument();
    expect(within(sheet).getByText(/aren't available on this server/)).toBeInTheDocument();
  });

  it("a poll needs two options and a question; a link needs https", async () => {
    const { user, composer } = await openComposer();
    await user.click(within(composer).getByRole("button", { name: /Add a sticker/ }));
    let sheet = await screen.findByRole("dialog", { name: "Add a sticker" });
    await user.click(within(sheet).getByRole("button", { name: /Poll/ }));
    let editor = await screen.findByRole("group", { name: "Poll sticker" });
    await user.type(within(editor).getByRole("textbox", { name: "Question" }), "Only a question");
    await user.click(within(editor).getByRole("button", { name: "Add" }));
    expect(within(editor).getByText(/Fill in every field/)).toBeInTheDocument();
    await user.click(within(editor).getByRole("button", { name: "Cancel" }));

    await user.click(within(composer).getByRole("button", { name: /Add a sticker/ }));
    sheet = await screen.findByRole("dialog", { name: "Add a sticker" });
    await user.click(within(sheet).getByRole("button", { name: /Link/ }));
    editor = await screen.findByRole("group", { name: "Link sticker" });
    const url = within(editor).getByRole("textbox", { name: "Web address" });
    await user.clear(url);
    await user.type(url, "javascript:alert(1)");
    await user.click(within(editor).getByRole("button", { name: "Add" }));
    expect(within(editor).getByText(/Use a full https:\/\/ address/)).toBeInTheDocument();
    expect(sapi.createStory).not.toHaveBeenCalled();
  });

  it("an unfinished story can be saved as an encrypted draft and picked up again", async () => {
    const { user, composer } = await openComposer();
    await addPoll(user, composer);
    await user.click(within(composer).getByRole("button", { name: "Close" }));
    const prompt = await screen.findByRole("alertdialog", { name: "Leave this story?" });
    await user.click(within(prompt).getByRole("button", { name: "Save draft" }));
    await waitFor(() => expect(screen.queryByRole("dialog", { name: "New story" })).not.toBeInTheDocument());
    expect(sapi.createStory).not.toHaveBeenCalled();
    // Only ciphertext on disk.
    const rows = await new Promise<unknown[]>((resolve) => {
      const open = indexedDB.open("redvault-story-drafts", 1);
      open.onsuccess = () => {
        const req = open.result.transaction("drafts").objectStore("drafts").getAll();
        req.onsuccess = () => {
          open.result.close();
          resolve(req.result);
        };
      };
    });
    expect(rows).toHaveLength(1);
    expect(JSON.stringify(rows, (_k, v) => (v instanceof Uint8Array ? Array.from(v).join(",") : v))).not.toMatch(/Pizza|Friday/);

    await user.click(await screen.findByRole("button", { name: "New story" }));
    const again = await screen.findByRole("dialog", { name: "New story" });
    const drafts = await within(again).findByRole("region", { name: "Drafts" });
    expect(within(drafts).getByText(/Friday plans · 1 sticker/)).toBeInTheDocument();
    await user.click(within(drafts).getByRole("button", { name: "Continue" }));
    expect(await within(again).findByRole("textbox", { name: "Story text" })).toHaveValue("Friday plans");
    expect(within(again).getByRole("button", { name: /Poll: Pizza or tacos\?/ })).toBeInTheDocument();

    // Sharing it removes the draft.
    await user.click(within(again).getByRole("button", { name: "Share" }));
    await waitFor(() => expect(sapi.createStory).toHaveBeenCalledTimes(1));
    await waitFor(async () => expect(await listDrafts({ userId: ME, publicKey: me.publicKey, privateKey: me.privateKey })).toEqual([]));
  });

  it("discarding leaves no draft behind", async () => {
    const { user, composer } = await openComposer();
    await user.click(within(composer).getByRole("button", { name: "Close" }));
    const prompt = await screen.findByRole("alertdialog", { name: "Leave this story?" });
    await user.click(within(prompt).getByRole("button", { name: "Discard" }));
    await waitFor(() => expect(screen.queryByRole("dialog", { name: "New story" })).not.toBeInTheDocument());
    expect(await listDrafts({ userId: ME, publicKey: me.publicKey, privateKey: me.privateKey })).toEqual([]);
  });

  it("sharing needs at least one chosen friend when the audience is 'choose people'", async () => {
    const { user, composer } = await openComposer();
    await user.click(within(composer).getByRole("radio", { name: /Choose people/ }));
    const people = await screen.findByRole("dialog", { name: "Choose who sees this story" });
    await user.click(within(people).getByRole("button", { name: "Done" }));
    expect(within(composer).getByRole("button", { name: "Share" })).toBeDisabled();
    await act(async () => {});
    expect(sapi.createStory).not.toHaveBeenCalled();
  });
});
