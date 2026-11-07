import { acknowledgeRecoveryKey } from "./recoveryKeyUi.js";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "../App.js";
import * as e2e from "../lib/e2eCrypto.js";
import { resetFakeBackend, seedOtherUser, sendFriendRequestAsOther, setNewUserTier } from "./fakeApiBackend.js";
import {
  apiCalls,
  FAKE_MAX_FILE_SIZE,
  resetFakeFileBackend,
  savedFiles,
  setFakeFileStatus,
  storedObjects,
} from "./fakeFileBackend.js";
import { tickSignUpBoxes } from "./signUpUi.js";

// Same approach as App.test.tsx: real UI + real crypto, fake network. The
// file endpoints and the blob transfer layer are faked by fakeFileBackend,
// which records everything that would have crossed the wire.
vi.mock("../lib/api.js", async () => {
  const backend = await import("./fakeApiBackend.js");
  const files = await import("./fakeFileBackend.js");
  return files.withFakeFileApi(backend.fakeApiModule);
});
vi.mock("../lib/fileTransfer.js", async () => (await import("./fakeFileBackend.js")).fakeFileTransferModule());

describe("E2EE file sharing (UI)", () => {
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

  async function openThreadWithFriend(user: ReturnType<typeof userEvent.setup>) {
    const stranger = await seedOtherUser("quiet-harbor");
    render(<App />);
    const handle = (screen.getByPlaceholderText(/choose a handle/i) as HTMLInputElement).value;
    await user.type(screen.getByPlaceholderText(/choose a password/i), "correct horse battery staple");
    await user.type(screen.getByPlaceholderText(/confirm password/i), "correct horse battery staple");
    await tickSignUpBoxes();
    await user.click(screen.getByRole("button", { name: /^create account$/i }));
    await acknowledgeRecoveryKey();
    await waitFor(() => expect(screen.getAllByText("Chats").length).toBeGreaterThan(0));

    await sendFriendRequestAsOther(stranger.userId, handle);
    await vi.advanceTimersByTimeAsync(5000);
    await waitFor(() => expect(screen.getByText(/1 friend request/i)).toBeInTheDocument());
    await user.click(screen.getByText(/1 friend request/i));
    await user.click(await screen.findByRole("button", { name: /^accept$/i }));
    await user.click(await screen.findByRole("button", { name: /mark as verified/i }));
    await waitFor(() => expect(screen.getByText(/quiet-harbor/i)).toBeInTheDocument());
    await user.click(screen.getByText(/quiet-harbor/i).closest("button")!);
    await screen.findByRole("button", { name: /attach file/i });
  }

  function pickFile(file: File) {
    fireEvent.change(screen.getByTestId("file-input"), { target: { files: [file] } });
  }

  it("encrypts locally, shows name/size, only reports Sent after upload + message, and (5) never sends the key or filename to the API", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);

    const secretText = "Passport no. X1234567 — top secret contents ".repeat(200);
    const plaintext = new TextEncoder().encode(secretText);
    pickFile(new File([plaintext], "john-passport.pdf", { type: "application/pdf" }));

    // Local preview only — nothing has been sent yet.
    expect(screen.getByText(/john-passport\.pdf · \d+(\.\d)? KB/)).toBeInTheDocument();
    expect(apiCalls.some((c) => c.fn === "requestFileUpload")).toBe(false);

    await user.click(screen.getByRole("button", { name: /send file/i }));
    await waitFor(() => expect(screen.getByText("Sent")).toBeInTheDocument());
    expect(screen.getByText("john-passport.pdf")).toBeInTheDocument(); // the file bubble

    // What left the device.
    const sendCall = apiCalls.find((c) => c.fn === "sendMessage" && (c.args[3] as string[] | undefined)?.length);
    expect(sendCall).toBeDefined();
    const objectId = (sendCall!.args[3] as string[])[0];
    const everythingSent = JSON.stringify(apiCalls.map((c) => c.args));
    expect(everythingSent).not.toContain("john-passport");
    expect(everythingSent).not.toContain("application/pdf");
    expect(everythingSent).not.toContain("Passport no.");

    // Recover the real file key from the only place it exists — the E2EE
    // message the recipient would decrypt — and prove it never went out in
    // the clear through any API call or into storage.
    const { messages } = await import("./fakeApiBackend.js");
    const conversationKey = await conversationKeyForTest();
    const descriptorText = e2e.decryptWithConversationKey(
      e2e.fromBase64(messages[0].ciphertext),
      e2e.fromBase64(messages[0].nonce),
      conversationKey
    );
    const fileKey = JSON.parse(descriptorText.slice(descriptorText.indexOf("{"))).fileKey as string;
    expect(fileKey).toHaveLength(44); // base64 of 32 bytes
    expect(everythingSent).not.toContain(fileKey);
    expect(messages[0].ciphertext).not.toContain(fileKey);

    // Storage only ever holds ciphertext.
    const stored = storedObjects.get(objectId)!;
    expect(stored.length).toBeGreaterThan(plaintext.length);
    expect(new TextDecoder("utf-8", { fatal: false }).decode(stored)).not.toContain("Passport no.");
  });

  it("downloads, verifies and decrypts back to the identical bytes, saved as an opaque download", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);
    const plaintext = new Uint8Array(150_000).map((_, i) => (i * 31) % 251);
    pickFile(new File([plaintext], "data.bin", { type: "application/octet-stream" }));
    await user.click(screen.getByRole("button", { name: /send file/i }));
    await waitFor(() => expect(screen.getByText("Sent")).toBeInTheDocument());

    await user.click(screen.getByRole("button", { name: /^download$/i }));
    await waitFor(() => expect(savedFiles).toHaveLength(1));
    expect(savedFiles[0].filename).toBe("data.bin");
    expect(savedFiles[0].type).toBe("application/octet-stream");
    expect(savedFiles[0].bytes).toEqual(plaintext);
  });

  it("shows 'File deleted' / 'Unable to decrypt' instead of failing silently", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);
    pickFile(new File([new Uint8Array(5000)], "a.txt"));
    await user.click(screen.getByRole("button", { name: /send file/i }));
    await waitFor(() => expect(screen.getByText("Sent")).toBeInTheDocument());
    const objectId = [...storedObjects.keys()][0];

    // Tampered in storage -> AEAD/hash check fails.
    storedObjects.get(objectId)![100] ^= 1;
    await user.click(screen.getByRole("button", { name: /^download$/i }));
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent(/unable to decrypt/i));
    expect(savedFiles).toHaveLength(0);

    // Deleted server-side -> distinct message.
    setFakeFileStatus(objectId, "deleted");
    await user.click(screen.getByRole("button", { name: /^download$/i }));
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent(/file deleted/i));
  });

  it("rejects a file over the size limit before encrypting or uploading anything", async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);
    const big = new File([new Uint8Array(1)], "huge.iso");
    Object.defineProperty(big, "size", { value: FAKE_MAX_FILE_SIZE + 1 });
    pickFile(big);
    await user.click(screen.getByRole("button", { name: /send file/i }));
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent(/file too large/i));
    expect(apiCalls.some((c) => c.fn === "requestFileUpload")).toBe(false);
    expect(screen.queryByText("Sent")).not.toBeInTheDocument();
  });

  it("a Free account can still message, but can't send files — it's told to upgrade and nothing is uploaded", async () => {
    setNewUserTier("free");
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
    await openThreadWithFriend(user);

    await user.click(screen.getByRole("button", { name: /attach file/i }));
    expect(screen.getByRole("alert")).toHaveTextContent(/pro feature/i);

    // Even a file that reaches the hidden input is refused before encrypting or uploading.
    pickFile(new File([new TextEncoder().encode("hello")], "notes.txt"));
    expect(screen.queryByRole("button", { name: /send file/i })).not.toBeInTheDocument();
    expect(screen.getByRole("alert")).toHaveTextContent(/upgrade to pro/i);
    expect(apiCalls.some((c) => c.fn === "requestFileUpload")).toBe(false);

    await user.type(screen.getByPlaceholderText("Message"), "text is fine on Free");
    await user.click(screen.getByRole("button", { name: /^send$/i }));
    await waitFor(() => expect(screen.getByText("text is fine on Free")).toBeInTheDocument());
  });
});

/** The signed-in user's conversation key, recovered the same way the app does — for inspecting what the E2EE message really contains. */
async function conversationKeyForTest(): Promise<Uint8Array> {
  const { conversations, usersById } = await import("./fakeApiBackend.js");
  const conversation = conversations[0];
  // The self user is the one whose private key we can unwrap: registered via the UI with this password.
  for (const p of conversation.participants) {
    const u = usersById.get(p.userId)!;
    try {
      const privateKey = e2e.unwrapPrivateKey(
        {
          encryptedPrivateKey: e2e.fromBase64(u.encryptedPrivateKey),
          nonce: e2e.fromBase64(u.privateKeyNonce),
          kdfSalt: e2e.fromBase64(u.privateKeyKdfSalt),
        },
        "correct horse battery staple"
      );
      return e2e.unsealConversationKey(e2e.fromBase64(p.wrappedKey), e2e.fromBase64(u.publicKey), privateKey);
    } catch {
      // not the self user
    }
  }
  throw new Error("no unwrappable participant");
}
