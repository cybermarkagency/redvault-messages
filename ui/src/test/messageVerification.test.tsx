import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { loadMessageVerification } from "../state/actions.js";
import type { Message } from "../types.js";
import {
  conversations,
  markConversationReadAsOther,
  resetFakeBackend,
  seedOtherUser,
  sendFriendRequestAsOther,
  verifications,
} from "./fakeApiBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

// Same faking strategy as App.test.tsx: real crypto, fake network layer.
vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  return backend.fakeApiModule;
});

describe("message verification (notification.md)", () => {
  beforeEach(() => {
    document.documentElement.removeAttribute("data-theme");
    resetFakeBackend();
    localStorage.clear();
    vi.useFakeTimers({ shouldAdvanceTime: true });
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  async function registerViaUi(user: ReturnType<typeof userEvent.setup>) {
    render(<App />);
    const handle = (screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement).value;
    const password = "correct horse battery staple";
    await user.type(screen.getByPlaceholderText(/choose a password/i), password);
    await user.type(screen.getByPlaceholderText(/confirm password/i), password);
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
    return handle;
  }

  async function befriend(user: ReturnType<typeof userEvent.setup>, selfHandle: string, stranger: { userId: string; username: string }) {
    await sendFriendRequestAsOther(stranger.userId, selfHandle);
    await vi.advanceTimersByTimeAsync(5000);
    await waitFor(() => expect(screen.getByText(/1 friend request/i)).toBeInTheDocument());
    await user.click(screen.getByText(/1 friend request/i));
    await waitFor(() => expect(screen.getByText(new RegExp(stranger.username, "i"))).toBeInTheDocument());
    await user.click(screen.getByRole("button", { name: /^accept$/i }));
    await waitFor(() => expect(screen.getByText(/verify safety number/i)).toBeInTheDocument());
    await user.click(screen.getByRole("button", { name: /mark as verified/i }));
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
  }

  it("shows a real Message Security checklist and matching commitments in the details screen", async () => {
    const user = userEvent.setup();
    const stranger = await seedOtherUser("dusky-ridge");
    const handle = await registerViaUi(user);
    await befriend(user, handle, stranger);

    await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());
    await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);

    await user.type(screen.getByPlaceholderText(/message/i), "made it in");
    await user.click(screen.getByRole("button", { name: /^send$/i }));
    await waitFor(() => expect(screen.getByText("made it in")).toBeInTheDocument());

    // Compact status first — a real, non-decorative claim.
    await waitFor(() => expect(screen.getByText(/✓ Sent, server's copy matches/)).toBeInTheDocument());

    // Expand → the 4-line checklist.
    await user.click(screen.getByText(/✓ Sent, server's copy matches/));
    expect(screen.getByText("Message Security")).toBeInTheDocument();
    expect(screen.getByText("Encrypted locally")).toBeInTheDocument();
    expect(screen.getByText("Relayed as ciphertext")).toBeInTheDocument();
    expect(screen.getByText("Server's copy matches")).toBeInTheDocument();
    expect(screen.getByText("Delivered")).toBeInTheDocument();

    // View Verification Details → local and network commitments really match.
    await user.click(screen.getByText("View Verification Details"));
    await waitFor(() => expect(screen.getByText("Verification Details")).toBeInTheDocument());
    expect(screen.getByText(/^MATCH ✓$/)).toBeInTheDocument();

    const localLabel = screen.getByText("Local commitment");
    const networkLabel = screen.getByText("Network commitment");
    const localValue = localLabel.nextElementSibling!.textContent;
    const networkValue = networkLabel.nextElementSibling!.textContent;
    expect(localValue).toBeTruthy();
    expect(localValue).toBe(networkValue);
    expect(localValue).toMatch(/^[0-9a-f]{64}$/); // hex SHA-256
  });

  it("loadMessageVerification reports a real failure, not a fabricated match, when the transparency record disagrees with the local re-hash", async () => {
    const user = userEvent.setup();
    const stranger = await seedOtherUser("dusky-ridge");
    const handle = await registerViaUi(user);
    await befriend(user, handle, stranger);

    await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());
    await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);

    await user.type(screen.getByPlaceholderText(/message/i), "tamper test");
    await user.click(screen.getByRole("button", { name: /^send$/i }));
    await waitFor(() => expect(screen.getByText("tamper test")).toBeInTheDocument());

    // Corrupt the transparency system's independently-recorded commitment —
    // simulating the relay having stored/reported something other than
    // what was actually sent.
    const [[messageId, record]] = [...verifications.entries()];
    record.commitment = "0".repeat(64);

    // Exercise the exact on-demand re-verification path the UI uses (see
    // MessageBubble's onRequestVerification), bypassing the cached
    // send-time `security` so this genuinely re-checks against the
    // (now-tampered) server record instead of reusing the earlier result.
    const message: Message = {
      id: messageId,
      from: "me",
      text: "tamper test",
      time: "now",
      ciphertextB64: (await import("./fakeApiBackend.js")).messages.find((m) => m.id === messageId)!.ciphertext,
      nonceB64: (await import("./fakeApiBackend.js")).messages.find((m) => m.id === messageId)!.nonce,
    };
    const dispatched: unknown[] = [];
    await loadMessageVerification((action) => dispatched.push(action), "irrelevant-conversation-id", message);

    expect(dispatched).toHaveLength(1);
    const action = dispatched[0] as { type: string; security: { verification: string } };
    expect(action.type).toBe("MESSAGE_SECURITY_UPDATED");
    expect(action.security.verification).toBe("failed");
  });

  it("Delivered and Read are distinct, real, receipt-driven events — never both-or-nothing", async () => {
    const user = userEvent.setup();
    const stranger = await seedOtherUser("dusky-ridge");
    const handle = await registerViaUi(user);
    await befriend(user, handle, stranger);

    await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());
    await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);

    await user.type(screen.getByPlaceholderText(/message/i), "hello");
    await user.click(screen.getByRole("button", { name: /^send$/i }));
    await waitFor(() => expect(screen.getByText("hello")).toBeInTheDocument());

    const conversation = conversations.find(
      (c) => c.participants.some((p) => p.userId === stranger.userId) && c.participants.length === 2
    )!;

    // The recipient's own client acknowledges delivery only — not read.
    await markConversationReadAsOther(stranger.userId, conversation.id, "delivered");
    await vi.advanceTimersByTimeAsync(3500); // next thread poll
    await waitFor(() => expect(screen.getByText(/✓✓ Delivered/)).toBeInTheDocument());

    // Now the recipient actually reads it.
    await markConversationReadAsOther(stranger.userId, conversation.id, "read");
    await vi.advanceTimersByTimeAsync(3500);
    await waitFor(() => expect(screen.getByText(/✓✓ Read/)).toBeInTheDocument());
  });
});
