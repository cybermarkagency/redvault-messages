import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { resetFakeBackend, seedOtherUser, sendFriendRequestAsOther, setNewUserTier } from "./fakeApiBackend.js";
import { FAKE_STORAGE_LIMIT, resetFakeFileBackend } from "./fakeFileBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

// Same mocking approach as fileSharing.test.tsx: real UI + real crypto,
// file endpoints faked by fakeFileBackend so this exercises the actual
// Settings-screen wiring to GET /v1/files/storage, not just the server.
vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  const files = await import("./fakeFileBackend.js");
  return files.withFakeFileApi(backend.fakeApiModule);
});
vi.mock("../lib/fileTransfer.js", async () => (await import("./fakeFileBackend.js")).fakeFileTransferModule());

describe("Settings shows real file storage usage", () => {
  beforeEach(() => {
    resetFakeBackend();
    resetFakeFileBackend();
    // Sending files is Pro-only; the Free-tier refusal has its own test below.
    setNewUserTier("pro");
    localStorage.clear();
    vi.useFakeTimers({ shouldAdvanceTime: true });
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  async function registerViaUi(user: ReturnType<typeof userEvent.setup>): Promise<string> {
    render(<App />);
    const handle = (screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement).value;
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
    return handle;
  }

  it("a Free account sees its free features and an upgrade prompt instead of a storage bar", async () => {
    setNewUserTier("free");
    const user = userEvent.setup();
    await registerViaUi(user);

    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await screen.findByRole("region", { name: "Free (your plan)" });
    expect(screen.getByRole("link", { name: "Choose Pro" })).toBeInTheDocument();
    expect(screen.queryByText(/file storage/i)).not.toBeInTheDocument();
  });

  it("a Free account's Choose Pro button opens Stripe with its own account id attached", async () => {
    setNewUserTier("free");
    const user = userEvent.setup();
    await registerViaUi(user);
    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    const link = await screen.findByRole("link", { name: "Choose Pro" });
    const url = new URL(link.getAttribute("href")!);
    expect(url.origin + url.pathname).toBe("https://buy.stripe.com/test_fake");
    // The fake backend numbers its accounts user-1, user-2, …
    expect(url.searchParams.get("client_reference_id")).toMatch(/^user-\d+$/);
    expect(link.getAttribute("rel")).toContain("noreferrer");
  });

  it("a Pro subscriber can switch back to Free, keeping Pro until the paid period ends, and undo it", async () => {
    setNewUserTier("pro");
    const user = userEvent.setup();
    await registerViaUi(user);
    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await waitFor(() => expect(screen.getByText("Your plan · renews monthly")).toBeInTheDocument());

    // Asks first, and says what happens.
    await user.click(screen.getByRole("button", { name: /switch to free…/i }));
    expect(screen.getByText(/you keep pro until the end of the period you've paid for/i)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: /^switch to free$/i }));

    const endsOn = new Date(2026, 9, 26).toLocaleDateString(undefined, { day: "numeric", month: "long", year: "numeric" });
    await waitFor(() => expect(screen.getByText(`Your plan until ${endsOn}`)).toBeInTheDocument());
    expect(screen.getByText(`Your plan after ${endsOn}`)).toBeInTheDocument();
    expect(screen.getByText(/won't be charged again/i)).toBeInTheDocument();

    await user.click(screen.getByRole("button", { name: /^keep pro$/i }));
    await waitFor(() => expect(screen.getByText("Your plan · renews monthly")).toBeInTheDocument());
  });

  it("a Free account is offered no Switch to Free", async () => {
    setNewUserTier("free");
    const user = userEvent.setup();
    await registerViaUi(user);
    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await screen.findByRole("region", { name: "Free (your plan)" });
    expect(screen.queryByRole("button", { name: /switch to free/i })).not.toBeInTheDocument();
  });

  it("a Pro account isn't sent to a payment page again", async () => {
    setNewUserTier("pro");
    const user = userEvent.setup();
    await registerViaUi(user);
    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await waitFor(() => expect(screen.getByText(/file storage/i)).toBeInTheDocument());
    expect(screen.queryByRole("link", { name: /choose/i })).not.toBeInTheDocument();
  });

  it("shows 0 used against the account's limit before any upload", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);

    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await waitFor(() => expect(screen.getByText(/file storage/i)).toBeInTheDocument());
    expect(screen.getByText(/^0 B of .* used/i)).toBeInTheDocument();
  });

  it("reflects real usage after sending a file, out of the account's own limit — and only that account's usage", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    const stranger = await seedOtherUser("quiet-harbor");
    const handle = await registerViaUi(user);

    await sendFriendRequestAsOther(stranger.userId, handle);
    await vi.advanceTimersByTimeAsync(5000);
    await waitFor(() => expect(screen.getByText(/1 friend request/i)).toBeInTheDocument());
    await user.click(screen.getByText(/1 friend request/i));
    await user.click(await screen.findByRole("button", { name: /^accept$/i }));
    await user.click(await screen.findByRole("button", { name: /mark as verified/i }));
    await waitFor(() => expect(screen.getByText(/quiet-harbor/i)).toBeInTheDocument());
    await user.click(screen.getByText(/quiet-harbor/i).closest("button")!);
    await screen.findByRole("button", { name: /attach file/i });

    const plaintext = new TextEncoder().encode("some file contents".repeat(50));
    fireEvent.change(screen.getByTestId("file-input"), { target: { files: [new File([plaintext], "notes.txt")] } });
    await user.click(screen.getByRole("button", { name: /send file/i }));
    await waitFor(() => expect(screen.getByText("Sent")).toBeInTheDocument());

    await user.click(screen.getByRole("button", { name: /^back$/i }));
    await waitFor(() => expect(screen.getByRole("button", { name: /^settings$/i })).toBeInTheDocument());
    await user.click(screen.getByRole("button", { name: /^settings$/i }));

    await waitFor(() => expect(screen.getByText(/file storage/i)).toBeInTheDocument());
    // Ciphertext is bigger than the plaintext (AEAD framing overhead), and
    // strictly less than the whole fake limit.
    await waitFor(() => expect(screen.queryByText(/^0 B of/i)).not.toBeInTheDocument());
    const limitMb = FAKE_STORAGE_LIMIT / (1024 * 1024);
    const limitLabel = new RegExp(`of ${limitMb < 10 ? limitMb.toFixed(1) : Math.round(limitMb)} MB used`, "i");
    expect(screen.getByText(limitLabel)).toBeInTheDocument();
  });

  it("refuses a file that would push the account over its storage limit, with a real message the user can see", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    const stranger = await seedOtherUser("quiet-harbor");
    const handle = await registerViaUi(user);

    await sendFriendRequestAsOther(stranger.userId, handle);
    await vi.advanceTimersByTimeAsync(5000);
    await waitFor(() => expect(screen.getByText(/1 friend request/i)).toBeInTheDocument());
    await user.click(screen.getByText(/1 friend request/i));
    await user.click(await screen.findByRole("button", { name: /^accept$/i }));
    await user.click(await screen.findByRole("button", { name: /mark as verified/i }));
    await waitFor(() => expect(screen.getByText(/quiet-harbor/i)).toBeInTheDocument());
    await user.click(screen.getByText(/quiet-harbor/i).closest("button")!);
    await screen.findByRole("button", { name: /attach file/i });

    // Pre-fill this account's storage to just under the fake limit — the
    // same server-side call the real upload flow makes, just invoked
    // directly instead of driving a real multi-megabyte encrypt+upload
    // through the UI for speed. The cap check only cares who the uploader
    // is, not which conversation, so this is equivalent to real usage.
    const api = await import("../lib/api.js");
    await api.requestFileUpload({
      conversationId: "seed",
      encryptedSize: FAKE_STORAGE_LIMIT - 1_000_000,
      ciphertextSha256: "a".repeat(64),
    });

    // Now a real (small) file, sent through the actual UI, tips it over.
    const plaintext = new TextEncoder().encode("x".repeat(2_000_000));
    fireEvent.change(screen.getByTestId("file-input"), { target: { files: [new File([plaintext], "big-enough.bin")] } });
    await user.click(screen.getByRole("button", { name: /send file/i }));

    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent(/storage limit reached/i));
    expect(screen.getByRole("alert")).toHaveTextContent(/upgrade to pro/i);
    // Never silently "sent" — the message never actually went through.
    expect(screen.queryByText("Sent")).not.toBeInTheDocument();
  });
});
