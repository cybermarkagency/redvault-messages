// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { describe, expect, it } from "vitest";
import { createInitialState, reducer } from "../state/reducer.js";
import type { AppState } from "../state/reducer.js";
import type { DMConversation, GroupConversation, SelfIdentity } from "../types.js";

function fakeIdentity(handle = "me-handle·0000"): SelfIdentity {
  return {
    handle,
    userId: "self-id",
    publicKey: new Uint8Array([1, 2, 3]),
    privateKey: new Uint8Array([4, 5, 6]),
    avatarUrl: null,
    role: "user",
    subscriptionTier: "free",
    mfaEnabled: false,
  };
}

function fakeDm(overrides: Partial<DMConversation> & Pick<DMConversation, "id" | "handle">): DMConversation {
  return {
    kind: "dm",
    verified: false,
    disappearing: "off",
    time: "9:41 AM",
    unread: 0,
    messages: [],
    userId: `${overrides.id}-user`,
    publicKey: new Uint8Array([9]),
    conversationKey: new Uint8Array([9]),
    safetyNumber: Array.from({ length: 12 }, () => "00000"),
    ...overrides,
  };
}

function fakeGroup(overrides: Partial<GroupConversation> = {}): GroupConversation {
  return {
    kind: "group",
    id: "group-1",
    label: "Weekend crew",
    members: [
      { userId: "self-id", handle: "me-handle·0000", avatarUrl: null, role: "owner", publicKey: "AA==", isSelf: true },
      { userId: "friend-a", handle: "friend-a", avatarUrl: null, role: "member", publicKey: "AA==", isSelf: false },
    ],
    myRole: "owner",
    disappearing: "off",
    time: "9:41 AM",
    unread: 0,
    messages: [],
    conversationKey: new Uint8Array([9]),
    ...overrides,
  };
}

/** Signed in, with the same three seeded DM contacts the old mock data used, dispatched the way the real app would. */
function signedInState(): AppState {
  let state = reducer(createInitialState(), {
    type: "AUTH_SUCCEEDED",
    identity: fakeIdentity(),
    acceptMessageRequests: true,
    transparencyMode: false,
  });
  state = reducer(state, {
    type: "CONVERSATIONS_LOADED",
    conversations: [
      fakeDm({
        id: "quiet-harbor",
        handle: "quiet-harbor·4f2a",
        verified: true,
        time: "9:41 AM",
        messages: [
          { from: "them", text: "Made it in. Signal's clean on this end.", time: "9:38 AM" },
          { from: "me", text: "Good. See you at the usual spot.", time: "9:41 AM" },
        ],
      }),
      fakeDm({
        id: "salt-lantern",
        handle: "salt-lantern·9c31",
        verified: false,
        disappearing: "1h",
        time: "Yesterday",
        unread: 2,
        messages: [
          { from: "them", text: "Can you confirm the drop time?", time: "Yesterday", disappearing: true },
          { from: "them", text: "Also — didn't verify yet, when you get a sec.", time: "Yesterday", disappearing: true },
        ],
      }),
      fakeDm({
        id: "dusky-ridge",
        handle: "dusky-ridge·2e77",
        verified: true,
        time: "Mon",
        messages: [{ from: "me", text: "Sent the notes over.", time: "Mon" }],
      }),
    ],
  });
  return state;
}

function dmById(state: AppState, id: string): DMConversation {
  const c = state.conversations.find((x) => x.id === id);
  if (c?.kind !== "dm") throw new Error(`expected a dm conversation for id ${id}`);
  return c;
}

describe("onboarding / auth", () => {
  it("starts on the onboarding screen, signed out, with no identity", () => {
    const state = createInitialState();
    expect(state.screen).toEqual({ key: "onboarding" });
    expect(state.identity).toBeNull();
    expect(state.authStatus).toBe("signed-out");
  });

  it("REGENERATE_CANDIDATE changes the candidate handle", () => {
    const state = createInitialState();
    const next = reducer(state, { type: "REGENERATE_CANDIDATE" });
    // Extremely unlikely to collide given the word-list x hex-suffix space.
    expect(next.onboardingCandidateHandle).not.toBe(state.onboardingCandidateHandle);
  });

  it("AUTH_STARTED marks authenticating and clears any previous error", () => {
    let state = reducer(createInitialState(), { type: "AUTH_FAILED", message: "nope" });
    expect(state.authError).toBe("nope");
    state = reducer(state, { type: "AUTH_STARTED" });
    expect(state.authStatus).toBe("authenticating");
    expect(state.authError).toBeNull();
  });

  it("AUTH_SUCCEEDED sets the identity, signs in, and opens chats", () => {
    const identity = fakeIdentity();
    const state = reducer(createInitialState(), {
      type: "AUTH_SUCCEEDED",
      identity,
      acceptMessageRequests: true,
      transparencyMode: false,
    });
    expect(state.identity).toEqual(identity);
    expect(state.authStatus).toBe("signed-in");
    expect(state.screen).toEqual({ key: "chats" });
  });

  it("AVATAR_UPDATED updates the signed-in identity's avatarUrl", () => {
    let state = reducer(createInitialState(), {
      type: "AUTH_SUCCEEDED",
      identity: fakeIdentity(),
      acceptMessageRequests: true,
      transparencyMode: false,
    });
    expect(state.identity?.avatarUrl).toBeNull();

    state = reducer(state, { type: "AVATAR_UPDATED", avatarUrl: "https://example.test/avatar.jpg" });
    expect(state.identity?.avatarUrl).toBe("https://example.test/avatar.jpg");

    state = reducer(state, { type: "AVATAR_UPDATED", avatarUrl: null });
    expect(state.identity?.avatarUrl).toBeNull();
  });

  it("AVATAR_UPDATED is a no-op when signed out (no identity to update)", () => {
    const state = reducer(createInitialState(), { type: "AVATAR_UPDATED", avatarUrl: "https://example.test/x.jpg" });
    expect(state.identity).toBeNull();
  });

  it("AUTH_SUCCEEDED carries the server's acceptMessageRequests value into settings", () => {
    const state = reducer(createInitialState(), {
      type: "AUTH_SUCCEEDED",
      identity: fakeIdentity(),
      acceptMessageRequests: false,
      transparencyMode: false,
    });
    expect(state.settings.acceptMessageRequests).toBe(false);
  });

  it("AUTH_SUCCEEDED carries the server's transparencyMode value into settings", () => {
    const state = reducer(createInitialState(), {
      type: "AUTH_SUCCEEDED",
      identity: fakeIdentity(),
      acceptMessageRequests: true,
      transparencyMode: true,
    });
    expect(state.settings.transparencyMode).toBe(true);
  });

  it("AUTH_FAILED records the error and stays signed out", () => {
    const state = reducer(createInitialState(), { type: "AUTH_FAILED", message: "Wrong handle or password." });
    expect(state.authStatus).toBe("signed-out");
    expect(state.authError).toBe("Wrong handle or password.");
  });

  it("LOGOUT resets to a fresh initial state", () => {
    const signedIn = signedInState();
    const state = reducer(signedIn, { type: "LOGOUT" });
    expect(state.identity).toBeNull();
    expect(state.authStatus).toBe("signed-out");
    expect(state.screen).toEqual({ key: "onboarding" });
    expect(state.conversations).toHaveLength(0);
  });
});

describe("conversation list / thread navigation", () => {
  it("OPEN_THREAD navigates to the thread and clears its unread count", () => {
    let state = signedInState();
    const target = state.conversations.find((c) => c.unread > 0)!;
    expect(target.unread).toBeGreaterThan(0);

    state = reducer(state, { type: "OPEN_THREAD", conversationId: target.id });
    expect(state.screen).toEqual({ key: "thread", conversationId: target.id });
    expect(state.conversations.find((c) => c.id === target.id)!.unread).toBe(0);
  });


  it("MESSAGE_SENT (real DM path) appends the confirmed message", () => {
    let state = signedInState();
    const conversationId = "quiet-harbor";
    const before = dmById(state, conversationId).messages.length;

    state = reducer(state, {
      type: "MESSAGE_SENT",
      conversationId,
      message: { id: "msg-1", from: "me", text: "on my way", time: "9:42 AM", createdAt: Date.now() },
    });
    const dm = dmById(state, conversationId);
    expect(dm.messages).toHaveLength(before + 1);
    expect(dm.messages.at(-1)).toMatchObject({ from: "me", text: "on my way", id: "msg-1" });
  });

  it("MESSAGES_LOADED merges by id instead of duplicating on repeated polls", () => {
    // Real DM messages always carry a server id (from initial load or
    // MESSAGE_SENT); start from an empty-history contact so the fixture
    // doesn't mix in id-less messages a real poll would never produce.
    let state = signedInState();
    const contact = { kind: "dm" as const, id: "new-friend", handle: "new-friend·aaaa", verified: false, disappearing: "off" as const, time: "9:41 AM", unread: 0, messages: [] };
    state = reducer(state, { type: "CONTACT_READY", contact });
    const conversationId = "new-friend";

    const polled = [{ id: "srv-1", from: "them" as const, text: "hey", time: "9:43 AM", createdAt: Date.now() }];
    state = reducer(state, { type: "MESSAGES_LOADED", conversationId, messages: polled });
    expect(dmById(state, conversationId).messages).toHaveLength(1);

    // Polling again with the same message shouldn't duplicate it.
    state = reducer(state, { type: "MESSAGES_LOADED", conversationId, messages: polled });
    expect(dmById(state, conversationId).messages).toHaveLength(1);
  });

  it("SET_DISAPPEARING updates only the target conversation", () => {
    let state = signedInState();
    const [first, second] = state.conversations;
    state = reducer(state, { type: "SET_DISAPPEARING", conversationId: first.id, duration: "30s" });
    expect(state.conversations.find((c) => c.id === first.id)!.disappearing).toBe("30s");
    expect(state.conversations.find((c) => c.id === second.id)!.disappearing).toBe(second.disappearing);
  });

  it("MESSAGE_DELETED (real DM path) removes only the matching message by id", () => {
    let state = signedInState();
    const conversationId = "quiet-harbor";
    state = reducer(state, {
      type: "MESSAGE_SENT",
      conversationId,
      message: { id: "to-delete", from: "me", text: "oops", time: "9:44 AM", createdAt: Date.now() },
    });
    const before = dmById(state, conversationId).messages.length;

    state = reducer(state, { type: "MESSAGE_DELETED", conversationId, messageId: "to-delete" });
    const dm = dmById(state, conversationId);
    expect(dm.messages).toHaveLength(before - 1);
    expect(dm.messages.some((m) => m.id === "to-delete")).toBe(false);
  });

});

describe("add contact & verify (mermaid: new-contact -> verify -> chats)", () => {
  it("CONTACT_READY adds a pending unverified contact and opens verify with origin new-contact", () => {
    let state = signedInState();
    const contact = fakeDm({ id: "new-friend", handle: "new-friend·aaaa", verified: false });
    state = reducer(state, { type: "CONTACT_READY", contact });

    expect(state.screen).toEqual({ key: "verify", contactId: "new-friend", origin: "new-contact" });
    expect(dmById(state, "new-friend")).toMatchObject({ verified: false });
  });

  it("CONTACT_LOOKUP_FAILED records the error without navigating", () => {
    let state = signedInState();
    state = reducer(state, { type: "OPEN_NEW_CONTACT" });
    state = reducer(state, { type: "CONTACT_LOOKUP_FAILED", message: "No one with that handle." });
    expect(state.screen).toEqual({ key: "new-contact" });
    expect(state.contactError).toBe("No one with that handle.");
  });

  it("MARK_VERIFIED from the new-contact origin sets verified and returns to chats", () => {
    let state = signedInState();
    const contact = fakeDm({ id: "new-friend", handle: "new-friend·aaaa" });
    state = reducer(state, { type: "CONTACT_READY", contact });
    state = reducer(state, { type: "MARK_VERIFIED" });

    expect(state.screen).toEqual({ key: "chats" });
    expect(dmById(state, "new-friend").verified).toBe(true);
  });

  it("DECLINE_VERIFY still keeps the contact but leaves it unverified, same navigation as accept", () => {
    let state = signedInState();
    const contact = fakeDm({ id: "new-friend", handle: "new-friend·aaaa" });
    state = reducer(state, { type: "CONTACT_READY", contact });
    state = reducer(state, { type: "DECLINE_VERIFY" });

    expect(state.screen).toEqual({ key: "chats" });
    expect(dmById(state, "new-friend").verified).toBe(false);
  });

  it("OPEN_VERIFY_FROM_THREAD -> MARK_VERIFIED returns to that thread, not chats", () => {
    let state = signedInState();
    const unverified = state.conversations.find((c) => c.kind === "dm" && !c.verified)!;

    state = reducer(state, { type: "OPEN_VERIFY_FROM_THREAD", contactId: unverified.id });
    expect(state.screen).toEqual({ key: "verify", contactId: unverified.id, origin: "thread" });

    state = reducer(state, { type: "MARK_VERIFIED" });
    expect(state.screen).toEqual({ key: "thread", conversationId: unverified.id });
    expect(state.conversations.find((c) => c.id === unverified.id)).toMatchObject({ verified: true });
  });
});

describe("friend requests (accept messages from anyone)", () => {
  const fakeRequest = (overrides: Partial<{ id: string; userId: string; handle: string }> = {}) => ({
    id: overrides.id ?? "req-1",
    userId: overrides.userId ?? "stranger-user",
    handle: overrides.handle ?? "stranger·zzzz",
    avatarUrl: null,
    createdAt: new Date().toISOString(),
  });

  it("OPEN_FRIEND_REQUESTS navigates to the requests screen", () => {
    const state = reducer(signedInState(), { type: "OPEN_FRIEND_REQUESTS" });
    expect(state.screen).toEqual({ key: "friend-requests" });
  });

  it("FRIEND_REQUESTS_LOADED replaces both pending lists without touching the normal conversation list", () => {
    let state = signedInState();
    const conversationsBefore = state.conversations;
    const incoming = [fakeRequest({ id: "in-1" })];
    const outgoing = [fakeRequest({ id: "out-1", userId: "other-user", handle: "other·aaaa" })];

    state = reducer(state, { type: "FRIEND_REQUESTS_LOADED", incoming, outgoing });

    expect(state.friendRequestsIncoming).toEqual(incoming);
    expect(state.friendRequestsOutgoing).toEqual(outgoing);
    expect(state.conversations).toBe(conversationsBefore);
  });

  it("FRIEND_REQUEST_SENT appends to the outgoing list", () => {
    let state = signedInState();
    const request = fakeRequest({ id: "out-1" });
    state = reducer(state, { type: "FRIEND_REQUEST_SENT", request });
    expect(state.friendRequestsOutgoing).toEqual([request]);
  });

  it("FRIEND_REQUEST_ACCEPTED removes it from the incoming list", () => {
    let state = signedInState();
    const request = fakeRequest({ id: "in-1" });
    state = reducer(state, { type: "FRIEND_REQUESTS_LOADED", incoming: [request], outgoing: [] });

    state = reducer(state, { type: "FRIEND_REQUEST_ACCEPTED", requestId: "in-1" });
    expect(state.friendRequestsIncoming).toEqual([]);
  });

  it("FRIEND_REQUEST_DENIED removes it from the incoming list", () => {
    let state = signedInState();
    const request = fakeRequest({ id: "in-1" });
    state = reducer(state, { type: "FRIEND_REQUESTS_LOADED", incoming: [request], outgoing: [] });

    state = reducer(state, { type: "FRIEND_REQUEST_DENIED", requestId: "in-1" });
    expect(state.friendRequestsIncoming).toEqual([]);
  });

  it("FRIEND_REQUEST_CANCELED removes it from the outgoing list", () => {
    let state = signedInState();
    const request = fakeRequest({ id: "out-1" });
    state = reducer(state, { type: "FRIEND_REQUESTS_LOADED", incoming: [], outgoing: [request] });

    state = reducer(state, { type: "FRIEND_REQUEST_CANCELED", requestId: "out-1" });
    expect(state.friendRequestsOutgoing).toEqual([]);
  });

  it("OPEN_PROFILE navigates to the profile screen with the given origin", () => {
    const state = reducer(signedInState(), {
      type: "OPEN_PROFILE",
      userId: "stranger-user",
      origin: { screen: "new-contact" },
    });
    expect(state.screen).toEqual({ key: "profile", userId: "stranger-user", origin: { screen: "new-contact" } });
  });

  it("UPDATE_SETTINGS can flip acceptMessageRequests like any other setting", () => {
    let state = signedInState();
    expect(state.settings.acceptMessageRequests).toBe(true);
    state = reducer(state, { type: "UPDATE_SETTINGS", settings: { acceptMessageRequests: false } });
    expect(state.settings.acceptMessageRequests).toBe(false);
  });
});



describe("lock overlay as a route guard", () => {
  it("LOCK/UNLOCK don't change the current screen", () => {
    let state = signedInState();
    state = reducer(state, { type: "OPEN_SETTINGS" });
    const screenBefore = state.screen;

    state = reducer(state, { type: "LOCK" });
    expect(state.locked).toBe(true);
    expect(state.screen).toEqual(screenBefore);

    state = reducer(state, { type: "UNLOCK" });
    expect(state.locked).toBe(false);
    expect(state.screen).toEqual(screenBefore);
  });
});

describe("settings", () => {
  it("UPDATE_SETTINGS merges partial updates", () => {
    let state = signedInState();
    state = reducer(state, { type: "UPDATE_SETTINGS", settings: { readReceipts: true } });
    expect(state.settings.readReceipts).toBe(true);
    expect(state.settings.appLock).toBe(true); // untouched default preserved
  });

  it("UPDATE_SETTINGS toggles read receipts and typing indicators independently", () => {
    let state = signedInState();
    expect(state.settings.readReceipts).toBe(false);
    expect(state.settings.typingIndicators).toBe(false);

    state = reducer(state, { type: "UPDATE_SETTINGS", settings: { typingIndicators: true } });
    expect(state.settings.typingIndicators).toBe(true);
    expect(state.settings.readReceipts).toBe(false); // unaffected
  });
});

describe("default disappearing messages (applies only to conversations created from now on)", () => {

  it("changing the default does NOT affect conversations that already existed", () => {
    let state = signedInState();
    const existingId = state.conversations[0].id;
    const existingBefore = state.conversations[0].disappearing;

    state = reducer(state, {
      type: "UPDATE_SETTINGS",
      settings: { defaultDisappearing: existingBefore === "1h" ? "30s" : "1h" },
    });

    const existingAfter = state.conversations.find((c) => c.id === existingId)!.disappearing;
    expect(existingAfter).toBe(existingBefore);
  });
});

describe("server-backed groups", () => {
  it("GROUP_CREATED adds the group, clears the draft and opens it", () => {
    let state = signedInState();
    state = reducer(state, { type: "OPEN_NEW_GROUP" });
    state = reducer(state, { type: "UPDATE_NEW_GROUP_NAME", name: "Weekend crew" });
    state = reducer(state, { type: "GROUP_CREATED", group: fakeGroup() });
    expect(state.screen).toEqual({ key: "thread", conversationId: "group-1" });
    expect(state.newGroupDraft).toEqual({ name: "", memberIds: [] });
    expect(state.conversations.find((c) => c.id === "group-1")).toMatchObject({ kind: "group", label: "Weekend crew" });
  });

  it("CONVERSATIONS_LOADED keeps server groups alongside DMs (no local-only groups survive)", () => {
    let state = signedInState();
    const dms = state.conversations.filter((c) => c.kind === "dm");
    state = reducer(state, { type: "CONVERSATIONS_LOADED", conversations: [...dms, fakeGroup()] });
    expect(state.conversations.filter((c) => c.kind === "group").map((c) => c.id)).toEqual(["group-1"]);
    state = reducer(state, { type: "CONVERSATIONS_LOADED", conversations: dms });
    expect(state.conversations.some((c) => c.kind === "group")).toBe(false);
  });

  it("MESSAGE_SENT and MESSAGE_DELETED work for groups like DMs", () => {
    let state = signedInState();
    state = reducer(state, { type: "GROUP_CREATED", group: fakeGroup() });
    state = reducer(state, { type: "MESSAGE_SENT", conversationId: "group-1", message: { id: "m1", from: "me", text: "hi all", time: "9:42 AM" } });
    expect(state.conversations.find((c) => c.id === "group-1")!.messages.map((m) => m.text)).toEqual(["hi all"]);
    state = reducer(state, { type: "MESSAGE_DELETED", conversationId: "group-1", messageId: "m1" });
    expect(state.conversations.find((c) => c.id === "group-1")!.messages).toEqual([]);
  });

  it("LEAVE_GROUP removes the conversation and returns to chats", () => {
    let state = signedInState();
    state = reducer(state, { type: "GROUP_CREATED", group: fakeGroup() });
    state = reducer(state, { type: "LEAVE_GROUP", conversationId: "group-1" });
    expect(state.screen).toEqual({ key: "chats" });
    expect(state.conversations.find((c) => c.id === "group-1")).toBeUndefined();
  });
});
