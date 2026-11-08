// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import sodium from "libsodium-wrappers-sumo";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import {
  groupInviteUrl,
  inviteKeys,
  inviteSecretFromFragment,
  newGroupInviteLink,
  openGroupInviteLink,
  openGroupInvitePreview,
  parseGroupInvite,
} from "../lib/groupInviteCrypto.js";
import { setKeyRingOwner } from "../lib/groupKeys.js";
import { GroupInviteLink } from "../screens/GroupInviteLink.js";
import { JoinGroup } from "../screens/JoinGroup.js";
import type { GroupConversation } from "../types.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  openGroupInvite: vi.fn(),
  joinGroupInvite: vi.fn(),
  denyGroupJoinRequest: vi.fn(),
  getGroupInviteLink: vi.fn(),
  setGroupInviteLink: vi.fn(),
  setGroupInviteApproval: vi.fn(),
  deleteGroupInviteLink: vi.fn(),
  listGroupJoinRequests: vi.fn(),
  approveGroupJoinRequest: vi.fn(),
  getUser: vi.fn(),
  listConversations: vi.fn(async () => []),
  listParticipants: vi.fn(async () => []),
}));

const dispatch = vi.fn();
let identity: { userId: string; handle: string; publicKey: Uint8Array; privateKey: Uint8Array };
vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({ state: { identity, conversations: [], settings: {} }, dispatch, contactsById: new Map() }),
}));

const b64url = (bytes: Uint8Array) => sodium.to_base64(bytes, sodium.base64_variants.URLSAFE_NO_PADDING);
let groupKey: Uint8Array;

beforeAll(async () => {
  await crypto.ready();
});

beforeEach(() => {
  vi.clearAllMocks();
  identity = { userId: "me", handle: "me", ...crypto.generateKeyPair() };
  groupKey = crypto.generateConversationKey();
});

/** What the server would answer someone opening the link, from what the admin's device sent it. */
function previewFrom(input: api.GroupInviteLinkInput, overrides: Partial<api.GroupInvitePreviewDto> = {}): api.GroupInvitePreviewDto {
  return {
    conversationId: "g1",
    previewCiphertext: input.previewCiphertext,
    previewNonce: input.previewNonce,
    memberCount: 3,
    approval: input.wrappedKey === null,
    wrappedKey: input.wrappedKey,
    wrappedKeyNonce: input.wrappedKeyNonce,
    member: false,
    requested: false,
    ...overrides,
  };
}

describe("group invite link crypto", () => {
  it("opens the name and group key with the secret, and nothing with another secret", () => {
    const { secret, input } = newGroupInviteLink(groupKey, "Book club", false);
    const opened = openGroupInvitePreview(previewFrom(input), secret);
    expect(opened.name).toBe("Book club");
    expect(opened.groupKey).toEqual(groupKey);
    expect(() => openGroupInvitePreview(previewFrom(input), sodium.randombytes_buf(32))).toThrow();
  });

  it("carries no group key when an admin approves joins", () => {
    const { secret, input } = newGroupInviteLink(groupKey, "Book club", true);
    expect(input.wrappedKey).toBeNull();
    expect(openGroupInvitePreview(previewFrom(input), secret).groupKey).toBeNull();
  });

  it("gives the server an auth token that isn't the secret or the encryption key", () => {
    const { secret, input } = newGroupInviteLink(groupKey, "Book club", true);
    const { key, auth } = inviteKeys(secret);
    expect(input.auth).toBe(auth);
    expect(auth).not.toBe(crypto.toBase64(secret));
    expect(auth).not.toBe(crypto.toBase64(key));
  });

  it("lets every member rebuild the link from the group key; the secret is only in the fragment", () => {
    const { secret, input } = newGroupInviteLink(groupKey, "Book club", true);
    const link: api.GroupInviteLinkDto = { id: "L1", secretCiphertext: input.secretCiphertext, secretNonce: input.secretNonce, approval: true, createdAt: "" };
    const opened = openGroupInviteLink(link, groupKey)!;
    expect(opened.secret).toEqual(secret);
    const url = new URL(opened.url);
    expect(url.searchParams.get("invite")).toBe("L1");
    expect(url.search).not.toContain(b64url(secret));
    expect(inviteSecretFromFragment(url.hash)).toEqual(secret);
    expect(parseGroupInvite(`Join us: ${opened.url}`)).toEqual({ linkId: "L1", secret: b64url(secret) });
    expect(parseGroupInvite(groupInviteUrl("L1", secret).split("#")[0])).toBeNull();
  });
});

describe("Join group screen", () => {
  it("joins with the group key sealed to the joiner, then opens the group", async () => {
    const { secret, input } = newGroupInviteLink(groupKey, "Book club", false);
    vi.mocked(api.openGroupInvite).mockResolvedValue(previewFrom(input));
    vi.mocked(api.joinGroupInvite).mockResolvedValue({ status: "joined", conversationId: "g1" });
    const user = userEvent.setup();
    render(<JoinGroup linkId="L1" secret={b64url(secret)} />);

    expect(await screen.findByRole("heading", { name: "Book club" })).toBeInTheDocument();
    expect(screen.getByText("Group · 3 members")).toBeInTheDocument();
    expect(api.openGroupInvite).toHaveBeenCalledWith("L1", inviteKeys(secret).auth);
    await user.click(screen.getByRole("button", { name: "Join group" }));

    await waitFor(() => expect(dispatch).toHaveBeenCalledWith({ type: "OPEN_THREAD", conversationId: "g1" }));
    const [, auth, sealed] = vi.mocked(api.joinGroupInvite).mock.calls[0];
    expect(auth).toBe(inviteKeys(secret).auth);
    expect(crypto.unsealConversationKey(crypto.fromBase64(sealed!), identity.publicKey, identity.privateKey)).toEqual(groupKey);
  });

  it("asks to join when an admin approves, and can take the request back", async () => {
    const { secret, input } = newGroupInviteLink(groupKey, "Book club", true);
    vi.mocked(api.openGroupInvite).mockResolvedValue(previewFrom(input));
    vi.mocked(api.joinGroupInvite).mockResolvedValue({ status: "requested", conversationId: "g1" });
    vi.mocked(api.denyGroupJoinRequest).mockResolvedValue();
    const user = userEvent.setup();
    render(<JoinGroup linkId="L1" secret={b64url(secret)} />);

    expect(await screen.findByText(/an admin of the group has to approve you first/i)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Ask to join" }));
    expect(await screen.findByText(/request sent/i)).toBeInTheDocument();
    expect(vi.mocked(api.joinGroupInvite).mock.calls[0][2]).toBeNull();

    await user.click(screen.getByRole("button", { name: "Cancel request" }));
    await waitFor(() => expect(api.denyGroupJoinRequest).toHaveBeenCalledWith("g1", "me"));
    expect(await screen.findByRole("button", { name: "Ask to join" })).toBeInTheDocument();
  });

  it("says so when the secret doesn't match the link", async () => {
    const { input } = newGroupInviteLink(groupKey, "Book club", false);
    vi.mocked(api.openGroupInvite).mockResolvedValue(previewFrom(input));
    render(<JoinGroup linkId="L1" secret={b64url(sodium.randombytes_buf(32))} />);
    expect(await screen.findByRole("alert")).toHaveTextContent(/secret doesn't match/i);
  });
});

describe("Group info invite link", () => {
  const group = (): GroupConversation => ({
    kind: "group",
    id: "g1",
    label: "Book club",
    members: [],
    myRole: "owner",
    disappearing: "off" as GroupConversation["disappearing"],
    time: "",
    unread: 0,
    messages: [],
    conversationKey: groupKey,
  });

  it("an admin creates the link (approval on), sees it, and turns approval off with the key wrapped for the link", async () => {
    vi.mocked(api.getGroupInviteLink).mockResolvedValue({ link: null });
    vi.mocked(api.listGroupJoinRequests).mockResolvedValue([]);
    let sent: api.GroupInviteLinkInput | null = null;
    vi.mocked(api.setGroupInviteLink).mockImplementation(async (_id, input) => {
      sent = input;
      return { link: { id: "L1", secretCiphertext: input.secretCiphertext, secretNonce: input.secretNonce, approval: true, createdAt: "" } };
    });
    vi.mocked(api.setGroupInviteApproval).mockImplementation(async (_id, approval) => ({
      link: { id: "L1", secretCiphertext: sent!.secretCiphertext, secretNonce: sent!.secretNonce, approval, createdAt: "" },
    }));
    const user = userEvent.setup();
    render(<GroupInviteLink group={group()} canManage />);

    await user.click(await screen.findByRole("button", { name: "Create invite link" }));
    const field = (await screen.findByRole("textbox", { name: "Invite link" })) as HTMLInputElement;
    const secret = inviteSecretFromFragment(new URL(field.value).hash)!;
    expect(inviteKeys(secret).auth).toBe(sent!.auth);
    expect(sent!.wrappedKey).toBeNull();

    await user.click(screen.getByRole("switch", { name: "Approve new members" }));
    await waitFor(() => expect(api.setGroupInviteApproval).toHaveBeenCalled());
    const [, approval, wrapped] = vi.mocked(api.setGroupInviteApproval).mock.calls[0];
    expect(approval).toBe(false);
    const opened = openGroupInvitePreview(previewFrom({ ...sent!, wrappedKey: wrapped!.wrappedKey, wrappedKeyNonce: wrapped!.wrappedKeyNonce }), secret);
    expect(opened.groupKey).toEqual(groupKey);
  });

  it("an admin approves a request by sealing the group key to the person who asked", async () => {
    const bob = crypto.generateKeyPair();
    const { input } = newGroupInviteLink(groupKey, "Book club", true);
    vi.mocked(api.getGroupInviteLink).mockResolvedValue({
      link: { id: "L1", secretCiphertext: input.secretCiphertext, secretNonce: input.secretNonce, approval: true, createdAt: "" },
    });
    vi.mocked(api.listGroupJoinRequests).mockResolvedValueOnce([{ userId: "bob-id", createdAt: "" }]).mockResolvedValue([]);
    vi.mocked(api.getUser).mockResolvedValue({ userId: "bob-id", username: "bob", publicKey: crypto.toBase64(bob.publicKey), avatarUrl: null, createdAt: "", deleted: false });
    vi.mocked(api.approveGroupJoinRequest).mockResolvedValue();
    // Approving reads the group's latest notice first.
    setKeyRingOwner(identity);
    vi.mocked(api.listConversations).mockResolvedValueOnce([
      { id: "g1", isGroup: true, keyEpoch: 0, wrappedKey: crypto.toBase64(crypto.sealConversationKey(groupKey, identity.publicKey)), keys: [] } as never,
    ]);
    const user = userEvent.setup();
    render(<GroupInviteLink group={group()} canManage />);

    expect(await screen.findByText("REQUESTS TO JOIN (1)")).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Approve" }));
    await waitFor(() => expect(api.approveGroupJoinRequest).toHaveBeenCalled());
    const [conversationId, userId, sealed] = vi.mocked(api.approveGroupJoinRequest).mock.calls[0];
    expect([conversationId, userId]).toEqual(["g1", "bob-id"]);
    expect(crypto.unsealConversationKey(crypto.fromBase64(sealed), bob.publicKey, bob.privateKey)).toEqual(groupKey);
    await waitFor(() => expect(screen.queryByText(/requests to join/i)).not.toBeInTheDocument());
    setKeyRingOwner(null);
  });

  it("shows nothing to a plain member when the group has no link", async () => {
    vi.mocked(api.getGroupInviteLink).mockResolvedValue({ link: null });
    const { container } = render(<GroupInviteLink group={{ ...group(), myRole: "member" }} canManage={false} />);
    await waitFor(() => expect(api.getGroupInviteLink).toHaveBeenCalled());
    expect(container).toBeEmptyDOMElement();
  });
});
