// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { act, render, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { groupCallInviteText, groupCallLinkUrl, parseGroupCallLink } from "../lib/groupCallLink.js";
import { parseCallLink } from "../lib/callLinkCrypto.js";
import { GroupCallProvider, useGroupCall } from "../state/GroupCallContext.js";
import type { GroupConversation } from "../types.js";

const CALL_ID = "3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b";

describe("group call links", () => {
  it("reads the call id out of a link, or out of the chat message that carries it", () => {
    const url = groupCallLinkUrl(CALL_ID);
    expect(new URL(url).searchParams.get("group-call")).toBe(CALL_ID);
    expect(parseGroupCallLink(url)).toBe(CALL_ID);
    expect(parseGroupCallLink(`  ${url}\n`)).toBe(CALL_ID);
    expect(parseGroupCallLink(groupCallInviteText("video", url))).toBe(CALL_ID);
    expect(parseGroupCallLink(`https://other.example/?group-call=${CALL_ID.toUpperCase()}`)).toBe(CALL_ID);
  });

  it("rejects anything that isn't a group call link", () => {
    expect(parseGroupCallLink("")).toBeNull();
    expect(parseGroupCallLink("hello there")).toBeNull();
    expect(parseGroupCallLink("https://example.com/?group-call=not-an-id")).toBeNull();
    expect(parseGroupCallLink(`https://example.com/?join=${CALL_ID}`)).toBeNull();
    expect(parseGroupCallLink(`https://example.com/?group-call=${CALL_ID}0`)).toBeNull();
  });

  it("keeps the two kinds of link apart", () => {
    expect(parseCallLink(groupCallLinkUrl(CALL_ID))).toBeNull();
    expect(parseGroupCallLink(`https://example.com/?join=abcdefghijkl#${"A".repeat(43)}`)).toBeNull();
  });
});

// ---- The call context: when the link is posted, and joining from one ----

const { group, dispatch, sendDmMessage, api } = vi.hoisted(() => ({
  group: {
    kind: "group",
    id: "group-1",
    label: "Weekend plans",
    members: [],
    conversationKey: new Uint8Array(32),
  } as unknown as GroupConversation,
  dispatch: vi.fn(),
  sendDmMessage: vi.fn(async (..._args: unknown[]) => {}),
  api: {
    startGroupCall: vi.fn(),
    joinGroupCall: vi.fn(),
    listActiveGroupCalls: vi.fn(async () => ({ calls: [] })),
    leaveGroupCall: vi.fn(async (..._args: unknown[]) => {}),
    declineGroupCall: vi.fn(async () => {}),
    sendGroupCallKeys: vi.fn(async () => {}),
    leaveGroupCallOnUnload: vi.fn(),
  },
}));

vi.mock("../lib/api.js", () => ({
  ...api,
  ApiError: class ApiError extends Error {
    constructor(
      public status: number,
      public code: string,
      message: string
    ) {
      super(message);
    }
  },
}));
vi.mock("../state/actions.js", () => ({ sendDmMessage }));
vi.mock("../state/AppStateContext.js", () => {
  const state = {
    authStatus: "signed-in",
    identity: { userId: "me", publicKey: new Uint8Array(32), privateKey: new Uint8Array(64), avatarUrl: null },
    conversations: [group],
  };
  return { useAppState: () => ({ state, dispatch }) };
});
vi.mock("../state/LiveContext.js", () => ({ useLive: () => ({ groupCallsAvailable: true, subscribe: () => () => {} }) }));
vi.mock("../state/CallContext.js", () => ({ useCall: () => ({ call: null }) }));
// LiveKit itself isn't under test: a room that connects at once.
vi.mock("livekit-client", async (importOriginal) => {
  class FakeRoom {
    remoteParticipants = new Map();
    localParticipant = {
      identity: "p-me",
      isMicrophoneEnabled: false,
      isSpeaking: false,
      getTrackPublication: () => undefined,
      setMicrophoneEnabled: async () => {},
      setCameraEnabled: async () => {},
    };
    on() {
      return this;
    }
    async setE2EEEnabled() {}
    async connect() {}
    async disconnect() {}
  }
  return { ...(await importOriginal<typeof import("livekit-client")>()), Room: FakeRoom, isE2EESupported: () => true };
});

const joinDto = (created: boolean) => ({ callId: CALL_ID, conversationId: group.id, kind: "video", url: "/livekit", token: "t", identity: "p-me", created });

describe("group call context", () => {
  let ctx: ReturnType<typeof useGroupCall>;
  function Probe() {
    ctx = useGroupCall();
    return null;
  }
  const mount = async () => {
    render(
      <GroupCallProvider>
        <Probe />
      </GroupCallProvider>
    );
    await waitFor(() => expect(ctx).toBeDefined());
  };

  beforeEach(() => {
    vi.clearAllMocks();
    vi.stubGlobal("Worker", class {});
    // A browser with WebRTC (Tor Browser has none, and gets a "calls aren't available" message instead).
    vi.stubGlobal("RTCPeerConnection", class {});
  });

  it("posts the call's link in the chat once the starter is connected", async () => {
    api.startGroupCall.mockResolvedValue(joinDto(true));
    await mount();
    act(() => ctx.start(group, "video"));
    await waitFor(() => expect(sendDmMessage).toHaveBeenCalledTimes(1));
    expect(ctx.call?.phase).toBe("active");
    const [, conversationId, key, text] = sendDmMessage.mock.calls[0] as unknown as [unknown, string, Uint8Array, string];
    expect(conversationId).toBe(group.id);
    expect(key).toBe(group.conversationKey);
    expect(text).toContain("Group video call started");
    expect(parseGroupCallLink(text)).toBe(CALL_ID);
  });

  it("doesn't post a link when pressing call just joined a call that was already going", async () => {
    api.startGroupCall.mockResolvedValue(joinDto(false));
    await mount();
    act(() => ctx.start(group, "video"));
    await waitFor(() => expect(ctx.call?.phase).toBe("active"));
    expect(sendDmMessage).not.toHaveBeenCalled();
  });

  it("doesn't post a link when the starter never got connected", async () => {
    api.startGroupCall.mockResolvedValue(joinDto(true));
    const livekit = await import("livekit-client");
    const connect = vi.spyOn(livekit.Room.prototype, "connect").mockRejectedValueOnce(new Error("unreachable"));
    await mount();
    act(() => ctx.start(group, "video"));
    await waitFor(() => expect(ctx.call?.phase).toBe("ended"));
    expect(sendDmMessage).not.toHaveBeenCalled();
    expect(api.leaveGroupCall).toHaveBeenCalledWith(CALL_ID);
    connect.mockRestore();
  });

  it("joins the call a pasted link points to, without starting one or posting anything", async () => {
    api.joinGroupCall.mockResolvedValue(joinDto(false));
    await mount();
    await act(async () => ctx.joinByLink(CALL_ID));
    expect(api.joinGroupCall).toHaveBeenCalledWith(CALL_ID);
    expect(api.startGroupCall).not.toHaveBeenCalled();
    await waitFor(() => expect(ctx.call?.phase).toBe("active"));
    expect(ctx.call).toMatchObject({ conversationId: group.id, kind: "video", groupLabel: "Weekend plans" });
    expect(sendDmMessage).not.toHaveBeenCalled();
  });

  it("tells you when the link's call isn't on (never started, over, or not your group)", async () => {
    const { ApiError } = await import("../lib/api.js");
    api.joinGroupCall.mockRejectedValue(new ApiError(404, "not_found", "This call has ended."));
    await mount();
    await expect(ctx.joinByLink(CALL_ID)).rejects.toThrow("This call has ended.");
    expect(ctx.call).toBeNull();
    expect(api.startGroupCall).not.toHaveBeenCalled();
  });

  it("won't join a link while you're already in a call", async () => {
    api.startGroupCall.mockResolvedValue(joinDto(false));
    await mount();
    act(() => ctx.start(group, "video"));
    await waitFor(() => expect(ctx.call?.phase).toBe("active"));
    await expect(ctx.joinByLink("11111111-2222-4333-8444-555555555555")).rejects.toThrow(/already in a call/i);
    expect(api.joinGroupCall).not.toHaveBeenCalled();
  });
});
