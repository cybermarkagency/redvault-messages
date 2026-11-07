import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import sodium from "libsodium-wrappers-sumo";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { callLinkUrl, newCallLinkSecret } from "../lib/callLinkCrypto.js";
import { groupCallLinkUrl } from "../lib/groupCallLink.js";
import { resetFakeBackend, seedOtherUser, sendFriendRequestAsOther, setCallsEnabled, setNewUserTier, startedCalls } from "./fakeApiBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

vi.mock("../lib/api.js", async () => (await import("./fakeApiBackend.js")).fakeApiModule);

// jsdom has no WebRTC; the call buttons only need it to exist.
class FakePeerConnection {}
class FakeMediaStream {
  getTracks() {
    return [];
  }
  addTrack() {}
}

describe("calls (UI)", () => {
  beforeEach(() => {
    resetFakeBackend();
    localStorage.clear();
    vi.useFakeTimers({ shouldAdvanceTime: true });
    vi.stubGlobal("RTCPeerConnection", FakePeerConnection);
    vi.stubGlobal("MediaStream", FakeMediaStream);
    Object.defineProperty(navigator, "mediaDevices", {
      configurable: true,
      value: { getUserMedia: vi.fn(async () => Promise.reject(new Error("NotAllowedError"))) },
    });
  });
  afterEach(() => {
    vi.useRealTimers();
    vi.unstubAllGlobals();
  });

  async function openThreadWithFriend(user: ReturnType<typeof userEvent.setup>) {
    const friend = await seedOtherUser("quiet-harbor");
    render(<App />);
    const handle = (screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement).value;
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));

    await sendFriendRequestAsOther(friend.userId, handle);
    await vi.advanceTimersByTimeAsync(5000);
    await user.click(await screen.findByText(/1 friend request/i));
    await user.click(await screen.findByRole("button", { name: /^accept$/i }));
    await user.click(await screen.findByRole("button", { name: /mark as verified/i }));
    await waitFor(() => expect(screen.getByText(/quiet-harbor/i)).toBeInTheDocument());
    await user.click(screen.getByText(/quiet-harbor/i).closest("button")!);
  }

  it("Free can start a one-to-one voice call", async () => {
    setCallsEnabled(true);
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);

    await user.click(await screen.findByRole("button", { name: /voice call quiet-harbor/i }));
    // The call screen opens and asks for the microphone first (refused here).
    await waitFor(() => expect(screen.getByText(/couldn't use your microphone/i)).toBeInTheDocument());
    expect(navigator.mediaDevices.getUserMedia).toHaveBeenCalledWith({ audio: true, video: false });
    expect(startedCalls).toEqual([]);
  });

  it("Free can start a one-to-one video call", async () => {
    setCallsEnabled(true);
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);

    await user.click(await screen.findByRole("button", { name: /video call quiet-harbor/i }));
    await waitFor(() => expect(navigator.mediaDevices.getUserMedia).toHaveBeenCalledWith({ audio: true, video: true }));
  });

  async function openJoinWithLinkDialog(user: ReturnType<typeof userEvent.setup>) {
    setCallsEnabled(true);
    await sodium.ready;
    render(<App />);
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));

    await user.click(await screen.findByRole("button", { name: /calls and call links/i }));
    await user.click(await screen.findByRole("button", { name: /join with a link/i }));
    return screen.getByPlaceholderText(/join=/i);
  }

  it("recognizes a group call link posted in a chat, and says so when group calls aren't available here", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    const field = await openJoinWithLinkDialog(user);
    await user.click(field);
    await user.paste(`📞 Group video call started. Paste this link:\n${groupCallLinkUrl("3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b")}`);
    await user.click(screen.getByRole("button", { name: /^continue$/i }));
    // Recognized as a group call link (not "doesn't look like a call link"); the fake server has no group calls.
    expect(await screen.findByRole("alert")).toHaveTextContent(/group calls aren't available/i);
  });

  it("lets someone on any plan paste a call link on the Calls screen to ask to join", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    const field = await openJoinWithLinkDialog(user);

    // Something that isn't a call link stays on the dialog with an explanation.
    await user.type(field, "hello there");
    await user.click(screen.getByRole("button", { name: /^continue$/i }));
    expect(await screen.findByRole("alert")).toHaveTextContent(/doesn't look like a call link/i);

    // A whole link opens the join screen for it (this fake server has no such link).
    await user.clear(field);
    await user.click(field);
    await user.paste(callLinkUrl("abcdefghijkl", newCallLinkSecret()));
    await user.click(screen.getByRole("button", { name: /^continue$/i }));
    expect(await screen.findByText(/this call link doesn't exist or was deleted/i)).toBeInTheDocument();
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
  });

  it("shows no call buttons when the server has no call relay", async () => {
    setNewUserTier("premium");
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);
    await screen.findByRole("button", { name: /attach file/i });
    expect(screen.queryByRole("button", { name: /voice call/i })).not.toBeInTheDocument();
  });
});
