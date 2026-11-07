import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { resetFakeBackend, seedOtherUser, sendFriendRequestAsOther } from "./fakeApiBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  return backend.fakeApiModule;
});

describe("Transparency Mode + live feed", () => {
  beforeEach(() => {
    document.documentElement.removeAttribute("data-theme");
    resetFakeBackend();
    localStorage.clear();
  });

  async function registerViaUi(user: ReturnType<typeof userEvent.setup>): Promise<string> {
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

  it("shows the sign-up card design: heading, labeled username field, and the no-phone/email/contacts disclaimer", () => {
    render(<App />);
    expect(screen.getByText(/create your identity/i)).toBeInTheDocument();
    expect(screen.getByText("Username")).toBeInTheDocument();
    expect(screen.getByText(/no phone number\./i)).toBeInTheDocument();
    expect(screen.getByText(/no email address\./i)).toBeInTheDocument();
    expect(screen.getByText(/no contact book required\./i)).toBeInTheDocument();
    expect(screen.getByRole("button", { name: /^create account$/i })).toBeInTheDocument();
  });

  it("the live transparency feed is reachable from onboarding without an account, and comes back", async () => {
    const user = userEvent.setup();
    render(<App />);

    await user.click(screen.getByRole("button", { name: /view the live transparency feed/i }));
    await waitFor(() => expect(screen.getByText(/live encrypted network/i)).toBeInTheDocument());
    expect(screen.getByText(/no public transactions yet/i)).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /back/i }));
    await waitFor(() => expect(screen.getByText(/create your identity/i)).toBeInTheDocument());
  });

  it("Transparency mode is off by default, and the toggle persists through the fake backend", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);

    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    const toggle = screen.getByRole("switch", { name: /transparency mode/i });
    expect(toggle).toHaveAttribute("aria-checked", "false");

    await user.click(toggle);
    await waitFor(() => expect(toggle).toHaveAttribute("aria-checked", "true"));
  });

  describe("with a real E2EE conversation", () => {
    beforeEach(() => {
      vi.useFakeTimers({ shouldAdvanceTime: true });
    });
    afterEach(() => {
      vi.useRealTimers();
    });

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

    it("a message sent while Transparency Mode is on appears in the public feed as ciphertext, never plaintext — and disappears again once turned off", async () => {
      const user = userEvent.setup();
      const stranger = await seedOtherUser("dusky-ridge");
      const handle = await registerViaUi(user);

      // Turn Transparency Mode on BEFORE sending — the server (and this
      // fake) checks the sender's current setting live at send time.
      await user.click(screen.getByRole("button", { name: /^settings$/i }));
      await user.click(screen.getByRole("switch", { name: /transparency mode/i }));
      await waitFor(() => expect(screen.getByRole("switch", { name: /transparency mode/i })).toHaveAttribute("aria-checked", "true"));
      await user.click(screen.getByRole("button", { name: /^back$/i }));

      await befriend(user, handle, stranger);
      await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());
      await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);

      await user.type(screen.getByPlaceholderText(/message/i), "hello from a public sender");
      await user.click(screen.getByRole("button", { name: /^send$/i }));
      await waitFor(() => expect(screen.getByText("hello from a public sender")).toBeInTheDocument());

      // Single-pane (phone) layout: leave the thread before the sidebar's
      // Settings button is reachable again.
      await user.click(screen.getByRole("button", { name: /^back$/i }));
      await waitFor(() => expect(screen.getByRole("button", { name: /^settings$/i })).toBeInTheDocument());
      await user.click(screen.getByRole("button", { name: /^settings$/i }));
      await user.click(screen.getByRole("button", { name: /view live transparency feed/i }));
      await waitFor(() => expect(screen.getByText(/live encrypted network/i)).toBeInTheDocument());

      // The ciphertext is there, but never the plaintext.
      await waitFor(() => expect(screen.queryByText(/no public transactions yet/i)).not.toBeInTheDocument());
      expect(screen.queryByText("hello from a public sender")).not.toBeInTheDocument();
      expect(screen.getByText(/encrypted content/i)).toBeInTheDocument();

      // Turn it back off — the message should no longer be public.
      await user.click(screen.getByRole("button", { name: /back/i }));
      await user.click(screen.getByRole("switch", { name: /transparency mode/i }));
      await waitFor(() => expect(screen.getByRole("switch", { name: /transparency mode/i })).toHaveAttribute("aria-checked", "false"));

      await user.click(screen.getByRole("button", { name: /view live transparency feed/i }));
      await waitFor(() => expect(screen.getByText(/no public transactions yet/i)).toBeInTheDocument());
    }, 20000);
  });
});
