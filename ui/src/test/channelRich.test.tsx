import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import { mediaKindFor, nextSpeed, savePosition, wrapSelection } from "../lib/channelPosts.js";
import { checkFollowedChannels, postPreview } from "../lib/channelWatch.js";
import { writePrefs } from "../lib/localPrefs.js";
import { Composer } from "../components/channel/Composer.js";
import { AudioPlayer, MediaGallery, PostContent } from "../components/channel/PostContent.js";
import { SuggestionReview, SuggestPost } from "../components/channel/Suggestions.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  postToChannel: vi.fn(),
  startChannelMedia: vi.fn(),
  startSuggestionMedia: vi.fn(),
  uploadChannelMedia: vi.fn(async () => {}),
  voteInChannelPoll: vi.fn(),
  closeChannelPoll: vi.fn(),
  setChannelChecklistItem: vi.fn(),
  suggestChannelPost: vi.fn(),
  listMySuggestions: vi.fn(async () => []),
  listChannelSuggestions: vi.fn(async () => []),
  approveChannelSuggestion: vi.fn(),
  rejectChannelSuggestion: vi.fn(),
  getChannel: vi.fn(),
  getChannelPosts: vi.fn(),
}));

const ID = "11111111-2222-4333-8444-555555555555";
const POST = "aaaaaaaa-0000-4000-8000-000000000001";
const channel: api.ChannelDto = {
  id: ID,
  name: "RedVault news",
  description: "",
  createdAt: "2026-10-01T00:00:00.000Z",
  lastPostAt: "2026-10-18T09:00:00.000Z",
  isPrivate: false,
  avatarUrl: null,
  acceptSuggestions: true,
};
const post = (extra: Partial<api.ChannelPostDto> = {}): api.ChannelPostDto => ({ id: POST, body: "", createdAt: "2026-10-18T09:00:00.000Z", views: 0, ...extra });
const poll = (extra: Partial<api.ChannelPollDto> = {}): api.ChannelPollDto => ({
  question: "Next feature?",
  options: ["Stories", "Bots"],
  multiple: false,
  quiz: false,
  closed: false,
  counts: [0, 0],
  voters: 0,
  correct: null,
  explanation: null,
  ...extra,
});
const media = (extra: Partial<api.ChannelMediaDto>): api.ChannelMediaDto => ({
  id: "m1",
  kind: "photo",
  mime: "image/png",
  size: 1000,
  filename: null,
  durationMs: null,
  width: null,
  height: null,
  url: "/api/channel-media/m1?exp=1&sig=x",
  ...extra,
});

beforeEach(() => {
  vi.clearAllMocks();
  localStorage.clear();
});

describe("formatted posts", () => {
  it("render bold, italic, code and links", () => {
    render(<PostContent channelId={ID} post={post({ body: "*Big* _news_ `v2` https://example.com" })} access={null} isOwner={false} />);
    expect(screen.getByText("Big").tagName).toBe("STRONG");
    expect(screen.getByText("news").tagName).toBe("EM");
    expect(screen.getByText("v2").tagName).toBe("CODE");
    expect(screen.getByRole("link", { name: "https://example.com" })).toBeInTheDocument();
  });

  it("the toolbar wraps the selection in a marker", () => {
    expect(wrapSelection("say hello now", 4, 9, "*")).toEqual({ text: "say *hello* now", start: 5, end: 10 });
    expect(wrapSelection("x", 1, 1, "||").text).toBe("x||||");
  });
});

describe("the owner's composer", () => {
  it("formats with the toolbar and posts the text", async () => {
    vi.mocked(api.postToChannel).mockResolvedValue(post({ body: "*hello*" }));
    const onPosted = vi.fn();
    const user = userEvent.setup();
    render(<Composer channel={channel} onPosted={onPosted} />);
    const box = screen.getByRole("textbox", { name: "New post" }) as HTMLTextAreaElement;
    await user.type(box, "hello");
    box.setSelectionRange(0, 5);
    await user.click(screen.getByRole("button", { name: "Attach, format and more" }));
    await user.click(screen.getByRole("button", { name: "Bold (*text*)" }));
    expect(box.value).toBe("*hello*");
    await user.click(screen.getByRole("button", { name: "Post" }));
    await waitFor(() => expect(api.postToChannel).toHaveBeenCalledWith(ID, { body: "*hello*" }));
    expect(onPosted).toHaveBeenCalledWith(expect.objectContaining({ body: "*hello*" }), false);
  });

  it("builds a poll, a quiz and a checklist", async () => {
    vi.mocked(api.postToChannel).mockResolvedValue(post());
    const user = userEvent.setup();
    render(<Composer channel={channel} onPosted={vi.fn()} />);

    await user.click(screen.getByRole("button", { name: "Attach, format and more" }));
    await user.click(screen.getByRole("button", { name: "Quiz" }));
    await user.type(screen.getByRole("textbox", { name: "Question" }), "2+2?");
    await user.type(screen.getByRole("textbox", { name: "Option 1" }), "3");
    await user.type(screen.getByRole("textbox", { name: "Option 2" }), "4");
    await user.click(screen.getByRole("radio", { name: "Option 2 is the right answer" }));
    await user.type(screen.getByRole("textbox", { name: "Explanation" }), "Sums");
    await user.click(screen.getByRole("button", { name: "Post" }));
    await waitFor(() =>
      expect(api.postToChannel).toHaveBeenLastCalledWith(ID, { body: "", poll: { question: "2+2?", options: ["3", "4"], multiple: false, quiz: { correct: 1, explanation: "Sums" } } })
    );

    await user.click(screen.getByRole("button", { name: "Attach, format and more" }));
    await user.click(screen.getByRole("button", { name: "Checklist" }));
    await user.type(screen.getByRole("textbox", { name: "Checklist title" }), "Launch");
    await user.type(screen.getByRole("textbox", { name: "Item 1" }), "Ship it");
    await user.click(screen.getByRole("checkbox", { name: /followers can tick items too/i }));
    await user.click(screen.getByRole("button", { name: "Post" }));
    await waitFor(() =>
      expect(api.postToChannel).toHaveBeenLastCalledWith(ID, { body: "", checklist: { title: "Launch", items: ["Ship it"], collaborative: true } })
    );
  });

  it("uploads an attachment with progress, then posts it", async () => {
    vi.mocked(api.startChannelMedia).mockResolvedValue({ id: "media-1", uploadUrl: "/api/channel-uploads/media-1" });
    vi.mocked(api.postToChannel).mockResolvedValue(post());
    const user = userEvent.setup();
    render(<Composer channel={channel} onPosted={vi.fn()} />);
    const file = new File(["%PDF-1.7 …"], "report.pdf", { type: "application/pdf" });
    fireEvent.change(screen.getByLabelText("Add files"), { target: { files: [file] } });

    expect(await screen.findByText(/ready/i)).toBeInTheDocument();
    expect(api.startChannelMedia).toHaveBeenCalledWith(ID, expect.objectContaining({ kind: "file", mime: "application/pdf", size: file.size, filename: "report.pdf" }));
    expect(api.uploadChannelMedia).toHaveBeenCalledWith("/api/channel-uploads/media-1", file, expect.any(Function));
    await user.click(screen.getByRole("button", { name: "Post" }));
    await waitFor(() => expect(api.postToChannel).toHaveBeenCalledWith(ID, { body: "", media: ["media-1"] }));
  });

  it("posts with Ctrl+Enter, while Enter only adds a line", async () => {
    vi.mocked(api.postToChannel).mockResolvedValue(post({ body: "One\nTwo" }));
    const user = userEvent.setup();
    render(<Composer channel={channel} onPosted={vi.fn()} />);
    const box = screen.getByRole("textbox", { name: "New post" });
    await user.type(box, "One{Enter}Two");
    expect(api.postToChannel).not.toHaveBeenCalled();
    await user.keyboard("{Control>}{Enter}{/Control}");
    await waitFor(() => expect(api.postToChannel).toHaveBeenCalledWith(ID, { body: "One\nTwo" }));
  });

  it("schedules a post for later", async () => {
    vi.mocked(api.postToChannel).mockResolvedValue(post({ body: "Later" }));
    const onPosted = vi.fn();
    const user = userEvent.setup();
    render(<Composer channel={channel} onPosted={onPosted} />);
    await user.type(screen.getByRole("textbox", { name: "New post" }), "Later");
    await user.click(screen.getByRole("button", { name: "Attach, format and more" }));
    await user.click(screen.getByRole("button", { name: "Schedule" }));
    fireEvent.change(screen.getByLabelText("Post at"), { target: { value: "2099-01-01T09:00" } });
    await user.click(screen.getByRole("button", { name: "Schedule post" }));
    await waitFor(() => expect(api.postToChannel).toHaveBeenCalledWith(ID, { body: "Later", publishAt: new Date("2099-01-01T09:00").toISOString() }));
    expect(onPosted).toHaveBeenCalledWith(expect.anything(), true);
  });

  it("knows which kind each file goes in as", () => {
    expect(mediaKindFor(new File([""], "a.jpg", { type: "image/jpeg" }))).toBe("photo");
    expect(mediaKindFor(new File([""], "a.svg", { type: "image/svg+xml" }))).toBe("file");
    expect(mediaKindFor(new File([""], "a.mp4", { type: "video/mp4" }))).toBe("video");
    expect(mediaKindFor(new File([""], "a.mp3", { type: "audio/mpeg" }))).toBe("audio");
    expect(mediaKindFor(new File([""], "a.zip", { type: "application/zip" }))).toBe("file");
  });
});

describe("polls, quizzes and checklists", () => {
  it("vote, see the results, and take the vote back", async () => {
    vi.mocked(api.voteInChannelPoll)
      .mockResolvedValueOnce({ poll: poll({ counts: [3, 1], voters: 4 }), mine: [1] })
      .mockResolvedValueOnce({ poll: poll({ counts: [3, 0], voters: 3 }), mine: [] });
    const user = userEvent.setup();
    render(<PostContent channelId={ID} post={post({ poll: poll() })} access="tok" isOwner={false} />);
    expect(screen.getByText(/results show after you vote/i)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Bots" }));
    expect(api.voteInChannelPoll).toHaveBeenCalledWith(ID, POST, [1], "tok");
    expect(await screen.findByText("75%")).toBeInTheDocument();
    expect(screen.getByText("4 votes")).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Take back my vote" }));
    expect(await screen.findByRole("button", { name: "Bots" })).toBeInTheDocument();
  });

  it("a quiz shows whether you were right, and the explanation", async () => {
    vi.mocked(api.voteInChannelPoll).mockResolvedValue({ poll: poll({ quiz: true, counts: [1, 0], voters: 1, correct: 1, explanation: "Basic sums" }), mine: [0] });
    const user = userEvent.setup();
    render(<PostContent channelId={ID} post={post({ poll: poll({ quiz: true, options: ["3", "4"] }) })} access={null} isOwner={false} />);
    await user.click(screen.getByRole("button", { name: "3" }));
    expect(await screen.findByText(/right answer/)).toBeInTheDocument();
    expect(screen.getByText("Basic sums")).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Take back my vote" })).not.toBeInTheDocument();
  });

  it("the owner can close a poll", async () => {
    vi.mocked(api.closeChannelPoll).mockResolvedValue(post({ poll: poll({ closed: true }) }));
    const user = userEvent.setup();
    render(<PostContent channelId={ID} post={post({ poll: poll() })} access={null} isOwner />);
    await user.click(screen.getByRole("button", { name: "Close poll" }));
    await waitFor(() => expect(api.closeChannelPoll).toHaveBeenCalledWith(ID, POST));
    expect(await screen.findByText(/poll · closed/i)).toBeInTheDocument();
  });

  it("checklists: the owner ticks; followers only when it's collaborative", async () => {
    vi.mocked(api.setChannelChecklistItem).mockResolvedValue({ title: "Launch", items: [{ text: "Ship", done: true }], collaborative: false });
    const user = userEvent.setup();
    const list = { title: "Launch", items: [{ text: "Ship", done: false }], collaborative: false };
    const owner = render(<PostContent channelId={ID} post={post({ checklist: list })} access={null} isOwner />);
    await user.click(screen.getByRole("checkbox", { name: "Ship" }));
    await waitFor(() => expect(api.setChannelChecklistItem).toHaveBeenCalledWith(ID, POST, 0, true, null));
    expect(await screen.findByText(/1 of 1 done/)).toBeInTheDocument();
    owner.unmount();

    render(<PostContent channelId={ID} post={post({ checklist: list })} access={null} isOwner={false} />);
    expect(screen.getByRole("checkbox", { name: "Ship" })).toBeDisabled();
  });
});

describe("media", () => {
  it("shows full-resolution photos, videos, round videos and files", () => {
    const { container } = render(
      <MediaGallery
        media={[
          media({ id: "p", kind: "photo" }),
          media({ id: "v", kind: "video", mime: "video/mp4", url: "/api/channel-media/v?exp=1&sig=x" }),
          media({ id: "r", kind: "round", mime: "video/webm", durationMs: 12_000 }),
          media({ id: "f", kind: "file", mime: "application/pdf", filename: "report.pdf", size: 2_000_000 }),
        ]}
      />
    );
    expect(screen.getByRole("img", { name: "Photo" })).toHaveAttribute("src", expect.stringContaining("/channel-media/m1"));
    const photoLink = screen.getByRole("img", { name: "Photo" }).closest("a")!;
    expect(photoLink).toHaveAttribute("target", "_blank");
    expect(photoLink).toHaveAttribute("title", "Open the full-resolution photo");
    expect(container.querySelector("video[controls]")).not.toBeNull();
    expect(screen.getByRole("button", { name: "Play video message" })).toBeInTheDocument();
    expect(screen.getByRole("link", { name: /report\.pdf/ })).toHaveAttribute("download", "report.pdf");
  });

  it("podcasts resume where you stopped and play at 1x, 1.5x or 2x", async () => {
    savePosition("pod", 125);
    const user = userEvent.setup();
    const { container } = render(<AudioPlayer media={media({ id: "pod", kind: "audio", mime: "audio/mpeg", filename: "Episode 1", durationMs: 3_600_000 })} variant="podcast" />);
    expect(screen.getByText(/resumes at 2:05/)).toBeInTheDocument();
    const audio = container.querySelector("audio")!;
    await user.click(screen.getByRole("button", { name: "Playback speed 1×" }));
    expect(audio.playbackRate).toBe(1.5);
    await user.click(screen.getByRole("button", { name: "Playback speed 1.5×" }));
    expect(audio.playbackRate).toBe(2);
    expect(nextSpeed(2)).toBe(1);
    expect(screen.getByRole("button", { name: "Back 15 seconds" })).toBeInTheDocument();
  });

  it("voice clips get the compact player", () => {
    render(<AudioPlayer media={media({ id: "v", kind: "voice", mime: "audio/webm", durationMs: 7000 })} variant="voice" />);
    expect(screen.getByText("0:00 / 0:07")).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Back 15 seconds" })).not.toBeInTheDocument();
  });
});

describe("suggested posts", () => {
  it("a follower suggests a post", async () => {
    vi.mocked(api.suggestChannelPost).mockResolvedValue({ id: "s1", body: "Fan art", status: "pending", createdAt: "", media: [] });
    const user = userEvent.setup();
    render(<SuggestPost channel={channel} access="tok" />);
    await user.click(screen.getByRole("button", { name: "Suggest a post" }));
    await user.type(screen.getByRole("textbox", { name: "Suggested post" }), "Fan art");
    await user.click(screen.getByRole("button", { name: "Send suggestion" }));
    await waitFor(() => expect(api.suggestChannelPost).toHaveBeenCalledWith(ID, { body: "Fan art", media: [] }, "tok"));
    expect(await screen.findByText(/sent\. the owner will look at it/i)).toBeInTheDocument();
    expect(screen.getByText("Your suggestions (1)")).toBeInTheDocument();
  });

  it("the owner edits and publishes one, or turns it down", async () => {
    vi.mocked(api.listChannelSuggestions).mockResolvedValue([
      { id: "s1", body: "Fan art", status: "pending", createdAt: "2026-10-18T09:00:00.000Z", media: [] },
      { id: "s2", body: "Spam", status: "pending", createdAt: "2026-10-18T09:00:00.000Z", media: [] },
    ]);
    vi.mocked(api.approveChannelSuggestion).mockResolvedValue(post({ body: "Fan art from a follower", suggested: true }));
    vi.mocked(api.rejectChannelSuggestion).mockResolvedValue();
    const onPublished = vi.fn();
    const user = userEvent.setup();
    render(<SuggestionReview channelId={ID} onPublished={onPublished} />);
    await user.click(await screen.findByText("Suggested posts (2)"));
    await user.click(screen.getAllByRole("button", { name: "Turn down" })[1]);
    await waitFor(() => expect(api.rejectChannelSuggestion).toHaveBeenCalledWith(ID, "s2"));
    await user.click(screen.getAllByRole("button", { name: "Review and publish" })[0]);
    const box = screen.getByRole("textbox", { name: "Edit before publishing" });
    await user.clear(box);
    await user.type(box, "Fan art from a follower");
    await user.click(screen.getByRole("button", { name: "Publish" }));
    await waitFor(() => expect(api.approveChannelSuggestion).toHaveBeenCalledWith(ID, "s1", { body: "Fan art from a follower", publishAt: null }));
    expect(onPublished).toHaveBeenCalledWith(expect.objectContaining({ suggested: true }), false);
  });

  it("a published suggestion is marked, never with who sent it", () => {
    render(<PostContent channelId={ID} post={post({ body: "Fan art", suggested: true })} access={null} isOwner={false} />);
    expect(screen.getByText("Suggested by a follower")).toBeInTheDocument();
  });
});

describe("new-post notifications, checked on this device", () => {
  it("notify once per new post, skip muted channels, and count unseen ones", async () => {
    const shown: { title: string; body: string }[] = [];
    class FakeNotification {
      static permission = "granted";
      onclick: (() => void) | null = null;
      constructor(title: string, options: { body: string }) {
        shown.push({ title, body: options.body });
      }
      close() {}
    }
    vi.stubGlobal("Notification", FakeNotification);
    const other = { ...channel, id: "22222222-2222-4333-8444-555555555555", name: "Muted one" };
    writePrefs("me", {
      followedChannels: [
        { id: ID, name: "RedVault news", seenAt: "2026-10-17T09:00:00.000Z" },
        { id: other.id, name: "Muted one", seenAt: "2026-10-17T09:00:00.000Z", muted: true },
      ],
    });
    vi.mocked(api.getChannel).mockImplementation(async (id) => (id === ID ? channel : other));
    vi.mocked(api.getChannelPosts).mockResolvedValue([post({ body: "*Big* news today" })]);
    const openChannel = vi.fn();

    expect(await checkFollowedChannels("me", { currentChannelId: null, openChannel })).toBe(2);
    expect(shown).toEqual([{ title: "RedVault news", body: "Big news today" }]);
    expect(api.getChannelPosts).toHaveBeenCalledWith(ID, undefined, null, 1);

    // The same post isn't notified twice.
    await checkFollowedChannels("me", { currentChannelId: null, openChannel });
    expect(shown).toHaveLength(1);
    vi.unstubAllGlobals();
  });

  it("describe posts that have no text", () => {
    expect(postPreview(post({ poll: poll() }))).toBe("Poll: Next feature?");
    expect(postPreview(post({ media: [media({ kind: "voice" })] }))).toBe("Voice message");
    expect(postPreview(post({ checklist: { title: "Launch", items: [], collaborative: false } }))).toBe("Checklist: Launch");
  });
});

describe("round video", () => {
  it("is shown in a circle with its length", () => {
    render(<MediaGallery media={[media({ id: "r", kind: "round", mime: "video/webm", durationMs: 12_000 })]} />);
    const button = screen.getByRole("button", { name: "Play video message" });
    expect(within(button).getByText(/0:12/)).toBeInTheDocument();
  });
});
