// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { act, renderHook, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { useGroupCallDeepLink, parseGroupCallDeepLink } from "../hooks/useGroupCallDeepLink.js";
import { groupCallLinkUrl, groupCallStartUrl, parseGroupCallStart } from "../lib/groupCallLink.js";

const CALL_ID = "3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b";
const GROUP_ID = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";

const { appState, groupCalls } = vi.hoisted(() => ({
  appState: {
    authStatus: "signed-in",
    locked: false,
    conversations: [] as { kind: string; id: string; label: string }[],
  },
  groupCalls: {
    available: true,
    joinByLink: vi.fn(async (_id: string) => {}),
    start: vi.fn(),
  },
}));

vi.mock("../state/AppStateContext.js", () => ({ useAppState: () => ({ state: appState, dispatch: vi.fn() }) }));
vi.mock("../state/GroupCallContext.js", () => ({ useGroupCall: () => groupCalls }));

const group = { kind: "group", id: GROUP_ID, label: "Weekend plans" };

function openedWith(search: string) {
  window.history.replaceState(null, "", `/${search}`);
}

describe("group call addresses", () => {
  it("tells a join address from a start address, and ignores anything else", () => {
    expect(parseGroupCallDeepLink(groupCallLinkUrl(CALL_ID))).toEqual({ join: CALL_ID });
    expect(parseGroupCallDeepLink(groupCallStartUrl(GROUP_ID, "video"))).toEqual({ start: GROUP_ID, kind: "video" });
    expect(parseGroupCallStart(groupCallStartUrl(GROUP_ID, "voice"))).toEqual({ conversationId: GROUP_ID, kind: "voice" });
    expect(parseGroupCallDeepLink("https://example.com/")).toBeNull();
    expect(parseGroupCallDeepLink(`https://example.com/?start-group-call=${GROUP_ID}`)).toBeNull();
    expect(parseGroupCallDeepLink(`https://example.com/?start-group-call=${GROUP_ID}&kind=screen`)).toBeNull();
    expect(parseGroupCallDeepLink("https://example.com/?start-group-call=nope&kind=video")).toBeNull();
  });
});

describe("opening the app from a group call address", () => {
  beforeEach(() => {
    vi.clearAllMocks();
    sessionStorage.clear();
    appState.authStatus = "signed-in";
    appState.locked = false;
    appState.conversations = [group];
    groupCalls.available = true;
    groupCalls.joinByLink.mockResolvedValue(undefined);
  });
  afterEach(() => {
    vi.useRealTimers();
    openedWith("");
  });

  it("joins the call once signed in, and takes the address out of the address bar", async () => {
    openedWith(`?group-call=${CALL_ID}`);
    renderHook(() => useGroupCallDeepLink());
    await waitFor(() => expect(groupCalls.joinByLink).toHaveBeenCalledWith(CALL_ID));
    expect(groupCalls.joinByLink).toHaveBeenCalledTimes(1);
    expect(window.location.search).toBe("");
    expect(sessionStorage.length).toBe(0);
  });

  it("waits for sign-in, then for group calls to be available", async () => {
    appState.authStatus = "signed-out";
    groupCalls.available = false;
    openedWith(`?group-call=${CALL_ID}`);
    const { rerender } = renderHook(() => useGroupCallDeepLink());
    expect(groupCalls.joinByLink).not.toHaveBeenCalled();

    appState.authStatus = "signed-in";
    rerender();
    expect(groupCalls.joinByLink).not.toHaveBeenCalled();

    groupCalls.available = true;
    rerender();
    await waitFor(() => expect(groupCalls.joinByLink).toHaveBeenCalledWith(CALL_ID));
  });

  it("doesn't join while the app is locked", async () => {
    appState.locked = true;
    openedWith(`?group-call=${CALL_ID}`);
    const { rerender } = renderHook(() => useGroupCallDeepLink());
    expect(groupCalls.joinByLink).not.toHaveBeenCalled();
    appState.locked = false;
    rerender();
    await waitFor(() => expect(groupCalls.joinByLink).toHaveBeenCalledTimes(1));
  });

  it("starts a call in the group named in a start address, once that chat has loaded", async () => {
    appState.conversations = [];
    openedWith(`?start-group-call=${GROUP_ID}&kind=video`);
    const { rerender } = renderHook(() => useGroupCallDeepLink());
    expect(groupCalls.start).not.toHaveBeenCalled();

    appState.conversations = [group];
    rerender();
    await waitFor(() => expect(groupCalls.start).toHaveBeenCalledWith(group, "video"));
    expect(groupCalls.start).toHaveBeenCalledTimes(1);
    expect(window.location.search).toBe("");
  });

  it("says so when the call can't be joined", async () => {
    groupCalls.joinByLink.mockRejectedValue(new Error("This call has ended."));
    openedWith(`?group-call=${CALL_ID}`);
    const { result } = renderHook(() => useGroupCallDeepLink());
    await waitFor(() => expect(result.current.notice).toBe("This call has ended."));
    act(() => result.current.dismiss());
    expect(result.current.notice).toBeNull();
  });

  it("gives up with a message if the chat or group calls never turn up", async () => {
    vi.useFakeTimers();
    groupCalls.available = false;
    openedWith(`?group-call=${CALL_ID}`);
    const { result } = renderHook(() => useGroupCallDeepLink());
    expect(result.current.notice).toBeNull();
    await act(async () => {
      await vi.advanceTimersByTimeAsync(16_000);
    });
    expect(result.current.notice).toMatch(/couldn't open that group call/i);
    expect(groupCalls.joinByLink).not.toHaveBeenCalled();
  });

  it("does nothing for an ordinary address", () => {
    openedWith("?something=else");
    renderHook(() => useGroupCallDeepLink());
    expect(groupCalls.joinByLink).not.toHaveBeenCalled();
    expect(groupCalls.start).not.toHaveBeenCalled();
    expect(window.location.search).toBe("?something=else");
  });
});
