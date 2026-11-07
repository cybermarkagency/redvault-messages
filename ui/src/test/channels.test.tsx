import { fireEvent, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import { channelAccessToken, newChannelSecret, openChannelSecret } from "../lib/channels.js";
import { dayLabel } from "../lib/dayLabel.js";
import * as crypto from "../lib/e2eCrypto.js";
import { readPrefs } from "../lib/localPrefs.js";
import { ChannelView } from "../screens/ChannelView.js";
import { Channels } from "../screens/Channels.js";
import { sendChannelForward } from "../state/actions.js";
import type { SubscriptionTier } from "../types.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  searchChannels: vi.fn(),
  getChannel: vi.fn(),
  getChannelPosts: vi.fn(),
  listMyChannels: vi.fn(),
  createChannel: vi.fn(),
  postToChannel: vi.fn(),
  reportChannel: vi.fn(),
  deleteChannel: vi.fn(),
  setChannelVisibility: vi.fn(),
  setChannelAvatar: vi.fn(),
  removeChannelAvatar: vi.fn(),
  recordChannelViews: vi.fn(async () => {}),
  fetchChannelAvatar: vi.fn(async () => new Blob(["img"])),
  listScheduledPosts: vi.fn(async () => []),
  listMySuggestions: vi.fn(async () => []),
  listChannelSuggestions: vi.fn(async () => []),
}));
vi.mock("../state/actions.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../state/actions.js")>()),
  sendChannelForward: vi.fn(async () => {}),
}));
vi.mock("../lib/avatar.js", () => ({ readImageAsAvatar: vi.fn(async () => "SU1BR0U=") }));

const dispatch = vi.fn();
let identity: { userId: string; role: "user" | "super_admin"; subscriptionTier: SubscriptionTier; publicKey: Uint8Array; privateKey: Uint8Array };
let conversations: { id: string; kind: "dm" | "group"; handle?: string; label?: string; conversationKey?: Uint8Array }[] = [];
vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({ state: { identity, conversations }, dispatch }),
}));

const ID = "11111111-2222-4333-8444-555555555555";
const news = (lastPostAt: string | null = "2026-10-18T09:00:00.000Z", extra: Partial<api.ChannelDto> = {}): api.ChannelDto => ({
  id: ID,
  name: "RedVault news",
  description: "Updates from the team",
  createdAt: "2026-10-01T00:00:00.000Z",
  lastPostAt,
  isPrivate: false,
  avatarUrl: null,
  acceptSuggestions: false,
  ...extra,
});
const own = (extra: Partial<api.OwnChannelDto> = {}): api.OwnChannelDto => ({ ...news(), secretSealed: null, ...extra });
const post = (id: string, body: string, views = 0, createdAt = "2026-10-18T09:00:00.000Z"): api.ChannelPostDto => ({ id, body, createdAt, views });
const P1 = "aaaaaaaa-0000-4000-8000-000000000001";
const P2 = "aaaaaaaa-0000-4000-8000-000000000002";
const P3 = "aaaaaaaa-0000-4000-8000-000000000003";

beforeAll(async () => {
  await crypto.ready();
});

beforeEach(() => {
  vi.clearAllMocks();
  localStorage.clear();
  identity = { userId: "me", role: "user", subscriptionTier: "free", ...crypto.generateKeyPair() };
  conversations = [];
  vi.mocked(api.listMyChannels).mockResolvedValue([]);
  vi.mocked(api.getChannelPosts).mockResolvedValue([]);
});

describe("Channels list", () => {
  it("finds channels in the directory and opens one", async () => {
    vi.mocked(api.searchChannels).mockResolvedValue([news()]);
    const user = userEvent.setup();
    render(<Channels />);
    expect(screen.getByText(/no limit on followers/i)).toBeInTheDocument();
    await user.click(await screen.findByRole("button", { name: /redvault news/i }));
    expect(dispatch).toHaveBeenCalledWith({ type: "OPEN_CHANNELS", channelId: ID });
  });

  it("asks Free accounts for Premium before they can create one", async () => {
    vi.mocked(api.searchChannels).mockResolvedValue([]);
    const user = userEvent.setup();
    render(<Channels />);
    await user.click(screen.getByRole("tab", { name: "Yours" }));
    expect(await screen.findByText(/creating a channel needs premium/i)).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "New channel" })).not.toBeInTheDocument();
  });

  it("lets a Premium member create a channel", async () => {
    identity = { ...identity, subscriptionTier: "premium" };
    vi.mocked(api.searchChannels).mockResolvedValue([]);
    vi.mocked(api.createChannel).mockResolvedValue(own());
    const user = userEvent.setup();
    render(<Channels />);
    await user.click(screen.getByRole("tab", { name: "Yours" }));
    await user.click(await screen.findByRole("button", { name: "New channel" }));
    await user.type(screen.getByRole("textbox", { name: "Channel name" }), "RedVault news");
    await user.type(screen.getByRole("textbox", { name: "Description" }), "Updates");
    await user.click(screen.getByRole("button", { name: "Create channel" }));
    await waitFor(() => expect(api.createChannel).toHaveBeenCalledWith("RedVault news", "Updates"));
    expect(dispatch).toHaveBeenCalledWith({ type: "OPEN_CHANNELS", channelId: ID });
  });
});

describe("Following a channel", () => {
  it("is kept on this device, shows new posts, and opening the channel counts them as seen", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news("2026-10-18T09:00:00.000Z"));
    const user = userEvent.setup();
    const { unmount } = render(<ChannelView channelId={ID} />);
    await user.click(await screen.findByRole("button", { name: "Follow" }));
    expect(readPrefs("me").followedChannels).toEqual([{ id: ID, name: "RedVault news", seenAt: "2026-10-18T09:00:00.000Z" }]);
    expect(screen.getByRole("button", { name: "Following" })).toHaveAttribute("aria-pressed", "true");
    unmount();

    vi.mocked(api.getChannel).mockResolvedValue(news("2026-10-18T12:00:00.000Z"));
    const list = render(<Channels />);
    const row = await screen.findByRole("button", { name: /redvault news/i });
    await waitFor(() => expect(within(row).getByLabelText("New posts")).toBeInTheDocument());
    list.unmount();

    render(<ChannelView channelId={ID} />);
    await waitFor(() => expect(readPrefs("me").followedChannels[0].seenAt).toBe("2026-10-18T12:00:00.000Z"));
  });
});

describe("A channel's page", () => {
  it("shows each post as the channel's, with its view count, and lets anyone else report it", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.getChannelPosts).mockResolvedValue([post(P1, "Hello everyone", 3), post(P2, "Second", 1)]);
    vi.mocked(api.reportChannel).mockResolvedValue();
    vi.spyOn(window, "prompt").mockReturnValue("Scam links");
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} />);

    const first = (await screen.findByText("Hello everyone")).closest("article")!;
    expect(within(first).getByText("RedVault news")).toBeInTheDocument();
    expect(within(first).getByText(/3 views/)).toBeInTheDocument();
    expect(screen.getByText(/public channel · not end-to-end encrypted/i)).toBeInTheDocument();
    expect(screen.queryByRole("textbox", { name: "New post" })).not.toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Channel info" }));
    expect(screen.queryByRole("button", { name: "Delete channel" })).not.toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Report" }));
    await waitFor(() => expect(api.reportChannel).toHaveBeenCalledWith(ID, "Scam links"));
  });

  it("reads like a chat: oldest post first, newest last, split by day", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.getChannelPosts).mockResolvedValue([post(P1, "Newest", 1, "2026-10-18T10:00:00.000Z"), post(P2, "Same day", 1, "2026-10-18T09:00:00.000Z"), post(P3, "Oldest", 1, "2026-10-16T09:00:00.000Z")]);
    render(<ChannelView channelId={ID} />);
    await screen.findByText("Newest");
    const articles = screen.getAllByRole("article").map((a) => a.textContent ?? "");
    expect(articles[0]).toContain("Oldest");
    expect(articles[1]).toContain("Same day");
    expect(articles[2]).toContain("Newest");
    expect(screen.getByText(dayLabel(new Date("2026-10-16T09:00:00.000Z").getTime()))).toBeInTheDocument();
    expect(screen.getAllByText(dayLabel(new Date("2026-10-18T10:00:00.000Z").getTime()))).toHaveLength(1);
  });

  it("gives readers a read-only bar, never a place to type a post", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.getChannelPosts).mockResolvedValue([post(P1, "Hello everyone")]);
    render(<ChannelView channelId={ID} />);
    await screen.findByText("Hello everyone");
    const bar = screen.getByRole("group", { name: "Read-only channel" });
    expect(within(bar).getByText(/only the owner posts/i)).toBeInTheDocument();
    expect(within(bar).getByRole("button", { name: "Follow" })).toBeInTheDocument();
    expect(screen.queryByRole("textbox")).not.toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Post" })).not.toBeInTheDocument();
  });

  it("never dresses a post up as encrypted: no lock, encrypted label or security details", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.listMyChannels).mockResolvedValue([own()]);
    vi.mocked(api.getChannelPosts).mockResolvedValue([post(P1, "Hello everyone")]);
    render(<ChannelView channelId={ID} />);
    const bubble = (await screen.findByText("Hello everyone")).closest("article")!;
    expect(bubble.textContent).not.toMatch(/encrypt|verified|relayed/i);
    expect(screen.queryByRole("button", { name: /security|details/i })).not.toBeInTheDocument();
    // The composer doesn't carry a chat's "Encrypted on this device" line either.
    expect(screen.getByRole("textbox", { name: "New post" }).closest("form")!.textContent).not.toMatch(/encrypt/i);
  });

  it("counts each post once per device, however often it's opened", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.getChannelPosts).mockResolvedValue([post(P1, "Hello everyone"), post(P2, "Second")]);
    const first = render(<ChannelView channelId={ID} />);
    await screen.findByText("Hello everyone");
    await waitFor(() => expect(api.recordChannelViews).toHaveBeenCalledWith([P1, P2]));
    first.unmount();
    render(<ChannelView channelId={ID} />);
    await screen.findByText("Hello everyone");
    expect(api.recordChannelViews).toHaveBeenCalledTimes(1);
  });

  it("gives the owner a composer, and their post shows at the bottom", async () => {
    identity = { ...identity, subscriptionTier: "premium" };
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.listMyChannels).mockResolvedValue([own()]);
    vi.mocked(api.getChannelPosts).mockResolvedValue([post(P1, "Older post")]);
    vi.mocked(api.postToChannel).mockResolvedValue(post(P2, "Fresh news", 0, "2026-10-18T10:00:00.000Z"));
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} />);

    expect(await screen.findByText(/posted as redvault news/i)).toBeInTheDocument();
    await user.type(screen.getByRole("textbox", { name: "New post" }), "Fresh news");
    await user.click(screen.getByRole("button", { name: "Post" }));
    await waitFor(() => expect(api.postToChannel).toHaveBeenCalledWith(ID, { body: "Fresh news" }));
    await screen.findByText("Fresh news");
    const articles = screen.getAllByRole("article").map((a) => a.textContent ?? "");
    expect(articles).toHaveLength(2);
    expect(articles[0]).toContain("Older post");
    expect(articles[1]).toContain("Fresh news");
    await user.click(screen.getByRole("button", { name: "Channel info" }));
    expect(screen.getByRole("button", { name: "Delete channel" })).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Report" })).not.toBeInTheDocument();
  });

  it("gives a super admin the owner's tools on someone else's channel, except who can read it", async () => {
    identity = { ...identity, role: "super_admin" };
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.listMyChannels).mockResolvedValue([]);
    vi.mocked(api.getChannelPosts).mockResolvedValue([]);
    vi.mocked(api.listScheduledPosts).mockResolvedValue([post(P1, "Coming soon")]);
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} />);

    expect(await screen.findByRole("textbox", { name: "New post" })).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Channel info" }));
    expect(screen.getByRole("button", { name: "Edit" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Add photo" })).toBeInTheDocument();
    expect(screen.getByRole("switch", { name: "Suggested posts" })).toBeInTheDocument();
    expect(await screen.findByText("Coming soon")).toBeInTheDocument();
    expect(screen.queryByRole("switch", { name: "Private channel" })).not.toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Report" })).not.toBeInTheDocument();
  });

  it("lets the owner set the channel's photo", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.listMyChannels).mockResolvedValue([own()]);
    vi.mocked(api.setChannelAvatar).mockResolvedValue(own({ avatarUrl: `/api/public/channels/${ID}/avatar?v=1` }));
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} />);
    await user.click(await screen.findByRole("button", { name: "Channel info" }));
    await screen.findByRole("button", { name: "Add photo" });
    fireEvent.change(screen.getByLabelText("Channel photo"), { target: { files: [new File(["x"], "me.png", { type: "image/png" })] } });
    await waitFor(() => expect(api.setChannelAvatar).toHaveBeenCalledWith(ID, "SU1BR0U="));
    expect(await screen.findByRole("button", { name: "Change photo" })).toBeInTheDocument();
  });

  it("lets a super admin take down someone else's channel", async () => {
    identity = { ...identity, role: "super_admin" };
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.deleteChannel).mockResolvedValue();
    vi.spyOn(window, "confirm").mockReturnValue(true);
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} />);
    await user.click(await screen.findByRole("button", { name: "Channel info" }));
    await user.click(screen.getByRole("button", { name: "Delete channel" }));
    await waitFor(() => expect(api.deleteChannel).toHaveBeenCalledWith(ID));
    expect(dispatch).toHaveBeenCalledWith({ type: "OPEN_CHANNELS" });
  });

  it("forwards a post into a chat as the channel's, keeping which post it was", async () => {
    conversations = [{ id: "c1", kind: "dm", handle: "quiet-harbor", conversationKey: new Uint8Array(32) }];
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.getChannelPosts).mockResolvedValue([post(P1, "Hello everyone")]);
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} />);
    await user.click(await screen.findByRole("button", { name: "Forward" }));
    await user.click(within(screen.getByRole("dialog", { name: "Forward post" })).getByRole("button", { name: "quiet-harbor" }));
    await waitFor(() =>
      expect(sendChannelForward).toHaveBeenCalledWith("c1", conversations[0].conversationKey, "Hello everyone", {
        channelId: ID,
        postId: P1,
        channelName: "RedVault news",
      })
    );
    expect(await screen.findByText("Forwarded.")).toBeInTheDocument();
  });
});

describe("Private channels", () => {
  it("the owner makes the channel private: a new link whose secret only their devices can open", async () => {
    vi.mocked(api.getChannel).mockResolvedValue(news());
    vi.mocked(api.listMyChannels).mockResolvedValue([own()]);
    vi.mocked(api.setChannelVisibility).mockImplementation(async (_id, v) => own({ isPrivate: v.isPrivate, secretSealed: v.isPrivate ? v.secretSealed : null }));
    vi.spyOn(window, "confirm").mockReturnValue(true);
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} />);

    await user.click(await screen.findByRole("button", { name: "Channel info" }));
    await user.click(await screen.findByRole("switch", { name: "Private channel" }));
    await waitFor(() => expect(api.setChannelVisibility).toHaveBeenCalled());
    const sent = vi.mocked(api.setChannelVisibility).mock.calls[0][1] as { isPrivate: true; access: string; secretSealed: string };
    const secret = openChannelSecret(sent.secretSealed, identity.publicKey, identity.privateKey)!;
    expect(channelAccessToken(secret)).toBe(sent.access);

    const field = (await screen.findByRole("textbox", { name: "Private link" })) as HTMLInputElement;
    expect(field.value).toContain(`?channel=${ID}#${secret}`);
    expect(screen.getByText(/private channel · only people with its link can read it/i)).toBeInTheDocument();
  });

  it("a reader opens one with its link, and following keeps the link on this device", async () => {
    const secret = newChannelSecret();
    vi.mocked(api.getChannel).mockResolvedValue(news(undefined, { isPrivate: true }));
    const user = userEvent.setup();
    render(<ChannelView channelId={ID} secret={secret} />);
    await user.click(await screen.findByRole("button", { name: "Follow" }));
    expect(api.getChannel).toHaveBeenCalledWith(ID, channelAccessToken(secret));
    expect(api.getChannelPosts).toHaveBeenCalledWith(ID, undefined, channelAccessToken(secret));
    expect(readPrefs("me").followedChannels[0].secret).toBe(secret);
  });

  it("without the link, says it may be private", async () => {
    vi.mocked(api.getChannel).mockRejectedValue(new api.ApiError(404, "not_found", "This channel doesn't exist, was deleted, or is private."));
    render(<ChannelView channelId={ID} />);
    expect(await screen.findByText(/opens only from its private link/i)).toBeInTheDocument();
  });
});
