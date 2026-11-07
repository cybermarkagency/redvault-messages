import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import { messages, resetFakeBackend, seedOtherUser, sendFriendRequestAsOther } from "./fakeApiBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

// App now talks to a real backend over ../lib/api.js. Rather than requiring
// a live server for these UI tests, this fakes that module with a small
// in-memory backend (./fakeApiBackend.js) — real crypto (e2eCrypto.js) still
// runs for real, so these tests exercise the actual
// register/login/add-contact/send wiring, just without a network hop.
vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  return backend.fakeApiModule;
});

// jsdom has no real <canvas> 2D rendering context, so the file-picker's
// resize-to-JPEG step (lib/avatar.ts) can't run in these tests — this stub
// keeps the same validation the real function does (so rejection tests
// still mean something) and only fakes the canvas-dependent success path.
// Everything downstream of it (the real upload action, the fake network
// layer, the reducer update, the re-render) still runs for real.
vi.mock("../lib/avatar.js", () => ({
  readImageAsAvatar: async (file: File) => {
    if (!file.type.startsWith("image/")) throw new Error("That's not an image file.");
    if (file.size > 10 * 1024 * 1024) throw new Error("That image is too large (max 10MB).");
    return "ZmFrZS1pbWFnZS1ieXRlcw==";
  },
}));

// The matchMedia polyfill in test/setup.ts always reports matches:false, so
// these tests exercise the phone-style single-pane layout (sidebar and
// main pane are never both mounted at once) — the same as a narrow window.
describe("App", () => {
  beforeEach(() => {
    document.documentElement.removeAttribute("data-theme");
    resetFakeBackend();
    localStorage.clear();
  });

  async function registerViaUi(user: ReturnType<typeof userEvent.setup>, password = "correct horse battery staple") {
    render(<App />);
    const handle = (screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement).value;
    await user.type(screen.getByPlaceholderText(/choose a password/i), password);
    await user.type(screen.getByPlaceholderText(/confirm password/i), password);
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
    return handle;
  }

  it("shows onboarding first, and says a recovery key comes after Continue", () => {
    render(<App />);
    expect(screen.getByText("Redvaultsystem")).toBeInTheDocument();
    expect(screen.getByText(/no password reset by email/i)).toBeInTheDocument();
  });

  it("sign-up shows a recovery key once, and only continues after it's marked saved", async () => {
    const user = userEvent.setup();
    render(<App />);
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    const key = await screen.findByLabelText("Recovery key");
    expect(key.textContent).toMatch(/^([A-Z2-7]{4}-){12}[A-Z2-7]{4}$/);
    expect(screen.getByRole("button", { name: /^done$/i })).toBeDisabled();
    await user.click(screen.getByRole("checkbox", { name: /saved my recovery key/i }));
    await user.click(screen.getByRole("button", { name: /^done$/i }));
    await waitFor(() => expect(screen.getByText(/no conversations yet/i)).toBeInTheDocument());
  });

  it("registering opens an empty conversation list (no more seeded mock contacts)", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);
    expect(screen.getByText(/no conversations yet/i)).toBeInTheDocument();
  });

  it("a first-time visitor (nothing in local storage) still defaults to Create, not Log in", () => {
    render(<App />);
    expect(screen.getByPlaceholderText(/choose a handle/i)).toBeInTheDocument();
    expect(screen.queryByPlaceholderText(/^handle$/i)).not.toBeInTheDocument();
  });

  it("reloading after registering lands on Log in with the handle already filled in, not a blank Create screen", async () => {
    const user = userEvent.setup();
    const password = "correct horse battery staple";
    const { unmount } = render(<App />);
    const handle = (screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement).value;
    await user.type(screen.getByPlaceholderText(/choose a password/i), password);
    await user.type(screen.getByPlaceholderText(/confirm password/i), password);
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));

    // A reload loses the in-memory session no matter what (there's no way
    // around that — the private key only ever lives in memory), but the
    // account itself is still there. The fix: land on Log in with the
    // handle ready, instead of silently offering to create a *new* account
    // that looks like "my old one vanished."
    unmount();
    render(<App />);
    expect(screen.getByPlaceholderText(/^handle$/i)).toHaveValue(handle);
    expect(screen.queryByPlaceholderText(/choose a handle/i)).not.toBeInTheDocument();
  });

  it("lets the user type their own handle instead of the generated suggestion", async () => {
    const user = userEvent.setup();
    render(<App />);

    const handleInput = screen.getByPlaceholderText(/choose a handle/i);
    await user.clear(handleInput);
    await user.type(handleInput, "my-own-custom-handle");
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();

    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));

    // Confirm it's really the chosen handle that got registered, not the
    // suggestion — log out and log back in with it. Logging out lands back
    // on Log in (not Create) with the handle already filled in.
    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await user.click(screen.getByRole("button", { name: /^log out$/i }));
    await waitFor(() => expect(screen.getByText("Redvaultsystem")).toBeInTheDocument());
    expect(screen.getByPlaceholderText(/^handle$/i)).toHaveValue("my-own-custom-handle");
    await user.type(screen.getByPlaceholderText(/^password$/i), "correct horse battery staple");
    await user.click(screen.getByRole("button", { name: /^log in$/i }));

    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
  });

  it("a too-short handle keeps Continue disabled", async () => {
    const user = userEvent.setup();
    render(<App />);

    const handleInput = screen.getByPlaceholderText(/choose a handle/i);
    await user.clear(handleInput);
    await user.type(handleInput, "ab");
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");

    await tickSignUpBoxes();
    expect(screen.getByRole("button", { name: /^create account$/i })).toBeDisabled();
    expect(screen.getByText(/at least 3 characters/i)).toBeInTheDocument();
  });

  it("\"Suggest one instead\" replaces the field with a fresh generated handle", async () => {
    const user = userEvent.setup();
    render(<App />);

    const handleInput = screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement;
    const original = handleInput.value;
    await user.click(screen.getByRole("button", { name: /suggest one instead/i }));

    expect(handleInput.value).not.toBe(original);
  });

  it("registering with mismatched passwords keeps Continue disabled", async () => {
    const user = userEvent.setup();
    render(<App />);
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "different password entirely");
    await tickSignUpBoxes();
    expect(screen.getByRole("button", { name: /^create account$/i })).toBeDisabled();
  });

  it("registering with a handle that's already taken shows \"Username is already taken\"", async () => {
    const user = userEvent.setup();
    await seedOtherUser("already-taken-handle");

    render(<App />);
    const handleInput = screen.getByPlaceholderText(/choose a handle/i);
    await user.clear(handleInput);
    await user.type(handleInput, "already-taken-handle");
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));

    await waitFor(() => expect(screen.getByText(/username is already taken/i)).toBeInTheDocument());
    // Stays on the registration screen — no identity was created.
    expect(screen.getByPlaceholderText(/choose a handle/i)).toBeInTheDocument();
  });

  it("a duplicate handle is rejected case-insensitively, matching the server's normalization", async () => {
    const user = userEvent.setup();
    await seedOtherUser("CaseSensitiveHandle");

    render(<App />);
    const handleInput = screen.getByPlaceholderText(/choose a handle/i);
    await user.clear(handleInput);
    await user.type(handleInput, "casesensitivehandle");
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));

    await waitFor(() => expect(screen.getByText(/username is already taken/i)).toBeInTheDocument());
  });

  it("shows an error and stays on new-contact for an unknown handle", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);

    await user.click(screen.getByRole("button", { name: /new conversation/i }));
    await user.click(screen.getByRole("button", { name: /add friend/i }));
    await user.type(screen.getByPlaceholderText(/their-handle/i), "nobody-here");
    await user.click(screen.getByRole("button", { name: /^look up$/i }));

    await waitFor(() => expect(screen.getByText(/no user with that handle/i)).toBeInTheDocument());
    expect(screen.queryByText(/verify safety number/i)).not.toBeInTheDocument();
  });

  it("looking up an existing handle shows a preview card with a send-friend-request action", async () => {
    const user = userEvent.setup();
    await seedOtherUser("salt-lantern");
    await registerViaUi(user);

    await user.click(screen.getByRole("button", { name: /new conversation/i }));
    await user.click(screen.getByRole("button", { name: /add friend/i }));
    await user.type(screen.getByPlaceholderText(/their-handle/i), "salt-lantern");
    await user.click(screen.getByRole("button", { name: /^look up$/i }));

    await waitFor(() => expect(screen.getByText(/salt-lantern/i)).toBeInTheDocument());
    await user.click(screen.getByRole("button", { name: /send friend request/i }));
    await waitFor(() => expect(screen.getByRole("button", { name: /request sent/i })).toBeInTheDocument());
  });

  describe("friends & messaging", () => {
    // Installed before render (not after registration) so the sidebar's
    // conversations/friend-requests poll — a setInterval scheduled the
    // moment sign-in succeeds — is itself a fake timer we can advance, the
    // same fix applied to the idle-lock tests in lock.test.tsx.
    beforeEach(() => {
      vi.useFakeTimers({ shouldAdvanceTime: true });
    });
    afterEach(() => {
      vi.useRealTimers();
    });

    /**
     * Simulates `strangerHandle` (already registered) sending `selfHandle` a
     * friend request, then drives the real UI to accept it and mark the
     * resulting DM as verified — a non-friend can no longer message
     * directly, so accepting a friend request (not "add contact") is what
     * now creates the conversation and reaches Verify.
     */
    async function befriendViaIncomingRequest(
      user: ReturnType<typeof userEvent.setup>,
      selfHandle: string,
      stranger: { userId: string; username: string }
    ) {
      await sendFriendRequestAsOther(stranger.userId, selfHandle);
      await vi.advanceTimersByTimeAsync(5000); // the next conversations/friend-requests poll

      await waitFor(() => expect(screen.getByText(/1 friend request/i)).toBeInTheDocument());
      await user.click(screen.getByText(/1 friend request/i));
      await waitFor(() => expect(screen.getByText(new RegExp(stranger.username, "i"))).toBeInTheDocument());

      await user.click(screen.getByRole("button", { name: /^accept$/i }));
      await waitFor(() => expect(screen.getByText(/verify safety number/i)).toBeInTheDocument());
      await user.click(screen.getByRole("button", { name: /mark as verified/i }));
      await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
    }

    it("full friend-request -> accept -> verify -> mark as verified flow shows a real 60-digit safety number", async () => {
      const user = userEvent.setup();
      const stranger = await seedOtherUser("salt-lantern");
      const handle = await registerViaUi(user);

      await sendFriendRequestAsOther(stranger.userId, handle);
      await vi.advanceTimersByTimeAsync(5000);

      await waitFor(() => expect(screen.getByText(/1 friend request/i)).toBeInTheDocument());
      await user.click(screen.getByText(/1 friend request/i));
      await waitFor(() => expect(screen.getByText(/salt-lantern/i)).toBeInTheDocument());

      await user.click(screen.getByRole("button", { name: /^accept$/i }));
      await waitFor(() => expect(screen.getByText(/verify safety number/i)).toBeInTheDocument());
      // 12 groups of 5 digits.
      expect(screen.getAllByText(/^\d{5}$/).length).toBe(12);

      await user.click(screen.getByRole("button", { name: /mark as verified/i }));
      await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
      expect(screen.getByText(/salt-lantern/i)).toBeInTheDocument();
    });

    it("verified status survives a reload — it's persisted locally, not just kept in memory", async () => {
      const user = userEvent.setup();
      const password = "correct horse battery staple";
      const stranger = await seedOtherUser("dusky-ridge");

      const { unmount } = render(<App />);
      const handle = (screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement).value;
      await user.type(screen.getByPlaceholderText(/choose a password/i), password);
      await user.type(screen.getByPlaceholderText(/confirm password/i), password);
      await tickSignUpBoxes();
      await user.click(screen.getByRole("button", { name: /^create account$/i }));
      await acknowledgeRecoveryKey();
      await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));

      await befriendViaIncomingRequest(user, handle, stranger);

      // Simulate a reload: the whole app (and the in-memory session/private
      // key with it) is gone. Logging back in re-hydrates conversations from
      // the server, which never learns about "verified" — that has to come
      // from somewhere else, or it's a fresh, real bug, not a test artifact.
      unmount();
      render(<App />);
      // Lands on Log in (not Create) with the handle already filled in.
      expect(screen.getByPlaceholderText(/^handle$/i)).toHaveValue(handle);
      await user.type(screen.getByPlaceholderText(/^password$/i), password);
      await user.click(screen.getByRole("button", { name: /^log in$/i }));
      await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());

      await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);
      expect(screen.getByText("Verified")).toBeInTheDocument();
      expect(screen.queryByText(/not verified — confirm/i)).not.toBeInTheDocument();
    });

    it("denying a friend request removes it without adding the conversation to chats", async () => {
      const user = userEvent.setup();
      const handle = await registerViaUi(user);
      const stranger = await seedOtherUser("hollow-bramble");

      await sendFriendRequestAsOther(stranger.userId, handle);
      await vi.advanceTimersByTimeAsync(5000);

      await waitFor(() => expect(screen.getByText(/1 friend request/i)).toBeInTheDocument());
      await user.click(screen.getByText(/1 friend request/i));
      await waitFor(() => expect(screen.getByText(/hollow-bramble/i)).toBeInTheDocument());

      await user.click(screen.getByRole("button", { name: /^deny$/i }));
      await waitFor(() => expect(screen.getByText(/no pending requests/i)).toBeInTheDocument());

      await user.click(screen.getByRole("button", { name: /^back$/i }));
      await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
      expect(screen.getByText(/no conversations yet/i)).toBeInTheDocument();
    });

    it("turning off \"accept messages from anyone\" rejects a stranger's attempt to send a friend request", async () => {
      const user = userEvent.setup();
      const handle = await registerViaUi(user);

      await user.click(screen.getByRole("button", { name: /^settings$/i }));
      const toggle = screen.getByRole("switch", { name: /accept messages from anyone/i });
      expect(toggle).toHaveAttribute("aria-checked", "true");
      await user.click(toggle);
      await waitFor(() => expect(toggle).toHaveAttribute("aria-checked", "false"));

      const stranger = await seedOtherUser("locked-out");
      await expect(sendFriendRequestAsOther(stranger.userId, handle)).rejects.toThrow(/accepting message requests/i);
    });

    it("sending a message shows up in the thread, end to end through real encryption", async () => {
      const user = userEvent.setup();
      const stranger = await seedOtherUser("dusky-ridge");
      const handle = await registerViaUi(user);

      await befriendViaIncomingRequest(user, handle, stranger);

      await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());
      await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);

      await user.type(screen.getByPlaceholderText(/message/i), "made it in");
      await user.click(screen.getByRole("button", { name: /^send$/i }));

      await waitFor(() => expect(screen.getByText("made it in")).toBeInTheDocument());
      // The server only ever stored ciphertext — never the plaintext directly.
      expect(messages).toHaveLength(1);
      expect(messages[0].ciphertext).not.toContain("made it in");
    });

    describe("disappearing messages", () => {
      it("a message set to disappear \"Instantly\" is actually gone from the thread a few seconds later", async () => {
        const user = userEvent.setup();
        const stranger = await seedOtherUser("dusky-ridge");
        const handle = await registerViaUi(user);

        await befriendViaIncomingRequest(user, handle, stranger);
        await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());
        await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);

        await user.click(screen.getByRole("radio", { name: "Instantly" }));
        await waitFor(() => expect(screen.getByRole("radio", { name: "Instantly" })).toHaveAttribute("aria-checked", "true"));

        await user.type(screen.getByPlaceholderText(/message/i), "gone soon");
        await user.click(screen.getByRole("button", { name: /^send$/i }));
        await waitFor(() => expect(screen.getByText("gone soon")).toBeInTheDocument());

        // Past the 1-second TTL, plus enough for the thread's poll to catch it.
        await vi.advanceTimersByTimeAsync(4000);
        await waitFor(() => expect(screen.queryByText("gone soon")).not.toBeInTheDocument());
      });

      it("a message with disappearing off never vanishes on its own", async () => {
        const user = userEvent.setup();
        const stranger = await seedOtherUser("still-willow");
        const handle = await registerViaUi(user);

        await befriendViaIncomingRequest(user, handle, stranger);
        await waitFor(() => expect(screen.getByText(/still-willow/i)).toBeInTheDocument());
        await user.click(screen.getByText(/still-willow/i).closest("button")!);

        await user.type(screen.getByPlaceholderText(/message/i), "sticks around");
        await user.click(screen.getByRole("button", { name: /^send$/i }));
        await waitFor(() => expect(screen.getByText("sticks around")).toBeInTheDocument());

        await vi.advanceTimersByTimeAsync(4000);
        expect(screen.getByText("sticks around")).toBeInTheDocument();
      });
    });

    it("deleting your own message removes it locally and from the backend", async () => {
      const user = userEvent.setup();
      const confirmSpy = vi.spyOn(window, "confirm").mockReturnValue(true);
      const stranger = await seedOtherUser("dusky-ridge");
      const handle = await registerViaUi(user);

      await befriendViaIncomingRequest(user, handle, stranger);
      await waitFor(() => expect(screen.getByText(/dusky-ridge/i)).toBeInTheDocument());
      await user.click(screen.getByText(/dusky-ridge/i).closest("button")!);

      await user.type(screen.getByPlaceholderText(/message/i), "oops wrong chat");
      await user.click(screen.getByRole("button", { name: /^send$/i }));
      await waitFor(() => expect(screen.getByText("oops wrong chat")).toBeInTheDocument());
      expect(messages).toHaveLength(1);

      await user.click(screen.getByRole("button", { name: /delete for everyone/i }));
      expect(confirmSpy).toHaveBeenCalled();

      await waitFor(() => expect(screen.queryByText("oops wrong chat")).not.toBeInTheDocument());
      // Actually gone from the backend, not just hidden client-side.
      expect(messages).toHaveLength(0);

      confirmSpy.mockRestore();
    });

    it("profile photo: a contact's real avatar shows up in the chat list and thread header once added", async () => {
      const user = userEvent.setup();
      const other = await seedOtherUser("photo-haver");
      other.avatarUrl = "https://fake-avatar-bucket.test/avatars/photo-haver.jpg";
      const handle = await registerViaUi(user);

      await befriendViaIncomingRequest(user, handle, other);

      // Chat list row.
      await waitFor(() => expect(screen.getByText(/photo-haver/i)).toBeInTheDocument());
      const rowImg = document.querySelector("img") as HTMLImageElement | null;
      expect(rowImg?.src).toBe(other.avatarUrl);

      // Thread header.
      await user.click(screen.getByText(/photo-haver/i).closest("button")!);
      const headerImgs = document.querySelectorAll("img");
      expect([...headerImgs].some((img) => img.src === other.avatarUrl)).toBe(true);
    });
  });

  it("logging out and back in with the right password restores the identity", async () => {
    const user = userEvent.setup();
    const password = "correct horse battery staple";
    const handle = await registerViaUi(user, password);

    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await user.click(screen.getByRole("button", { name: /^log out$/i }));
    await waitFor(() => expect(screen.getByText("Redvaultsystem")).toBeInTheDocument());

    // Lands on Log in (not Create) with the handle already filled in.
    expect(screen.getByPlaceholderText(/^handle$/i)).toHaveValue(handle);
    await user.type(screen.getByPlaceholderText(/^password$/i), password);
    await user.click(screen.getByRole("button", { name: /^log in$/i }));

    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));
  });

  it("logging in with the wrong password shows an error instead of crashing", async () => {
    const user = userEvent.setup();
    const handle = await registerViaUi(user, "correct horse battery staple");
    await user.click(screen.getByRole("button", { name: /^settings$/i }));
    await user.click(screen.getByRole("button", { name: /^log out$/i }));
    await waitFor(() => expect(screen.getByText("Redvaultsystem")).toBeInTheDocument());

    expect(screen.getByPlaceholderText(/^handle$/i)).toHaveValue(handle);
    await user.type(screen.getByPlaceholderText(/^password$/i), "totally wrong password");
    await user.click(screen.getByRole("button", { name: /^log in$/i }));

    await waitFor(() => expect(screen.getByText(/wrong handle or password/i)).toBeInTheDocument());
  });

  it("locking covers the surface and unlocking returns to the same screen", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);

    await user.click(screen.getByRole("button", { name: /^lock redvaultsystem$/i }));
    expect(screen.getByText(/redvaultsystem is locked/i)).toBeInTheDocument();
    expect(screen.queryAllByText("Chats")).toHaveLength(0);

    await user.click(screen.getByRole("button", { name: /^unlock$/i }));
    expect(screen.getAllByText("Chats").length).toBeGreaterThan(0);
  });

  it("profile photo: rejecting a non-image file shows an error", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);
    await user.click(screen.getByRole("button", { name: /^settings$/i }));

    // The hidden file input is reached through its accessible name via the label text
    // it's associated with — query it directly since it has no visible label.
    const fileInput = document.querySelector('input[type="file"]') as HTMLInputElement;
    expect(fileInput).toBeInTheDocument();

    const notAnImage = new File(["hello"], "notes.txt", { type: "text/plain" });
    fireEvent.change(fileInput, { target: { files: [notAnImage] } });
    await waitFor(() => expect(screen.getByText(/not an image/i)).toBeInTheDocument());
    expect(screen.getByRole("button", { name: /add photo/i })).toBeInTheDocument();
  });

  it("profile photo: once uploaded, shows up in Settings as an image, and Remove clears it back to the key glyph", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);
    await user.click(screen.getByRole("button", { name: /^settings$/i }));

    const fileInput = document.querySelector('input[type="file"]') as HTMLInputElement;
    const aRealLookingImage = new File(["fake-image-bytes"], "me.jpg", { type: "image/jpeg" });
    fireEvent.change(fileInput, { target: { files: [aRealLookingImage] } });

    // readImageAsAvatar is mocked (jsdom has no real <canvas> 2D context to
    // resize through) — everything after it, including the network round
    // trip and the reducer update, is real.
    await waitFor(() => expect(screen.getByRole("button", { name: /change photo/i })).toBeInTheDocument());
    const img = document.querySelector("img") as HTMLImageElement | null;
    expect(img?.src).toContain("fake-avatar-bucket");

    await user.click(screen.getByRole("button", { name: /^remove$/i }));
    await waitFor(() => expect(screen.getByRole("button", { name: /add photo/i })).toBeInTheDocument());
    expect(document.querySelector("img")).toBeNull();
  });

  it("Settings toggles and the default-disappearing selector reflect clicks immediately", async () => {
    const user = userEvent.setup();
    await registerViaUi(user);
    await user.click(screen.getByRole("button", { name: /^settings$/i }));

    const readReceipts = screen.getByRole("switch", { name: /read receipts/i });
    const typingIndicators = screen.getByRole("switch", { name: /typing indicators/i });
    expect(readReceipts).toHaveAttribute("aria-checked", "false");
    expect(typingIndicators).toHaveAttribute("aria-checked", "false");

    await user.click(readReceipts);
    expect(readReceipts).toHaveAttribute("aria-checked", "true");
    expect(typingIndicators).toHaveAttribute("aria-checked", "false"); // unaffected by the other toggle

    await user.click(typingIndicators);
    expect(typingIndicators).toHaveAttribute("aria-checked", "true");

    // Default disappearing: "Off" starts selected; picking "1 hour" moves the selection.
    expect(screen.getByRole("radio", { name: "Off" })).toHaveAttribute("aria-checked", "true");
    await user.click(screen.getByRole("radio", { name: "1 hour" }));
    expect(screen.getByRole("radio", { name: "1 hour" })).toHaveAttribute("aria-checked", "true");
    expect(screen.getByRole("radio", { name: "Off" })).toHaveAttribute("aria-checked", "false");
  });
});
