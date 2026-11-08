// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { act, cleanup, render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { afterEach, beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import * as api from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import { breachHash, openFromSender, openItem, rememberWrappedKey, sealForRecipient, sealItem, vaultKey, type VaultData } from "../lib/passwordVault.js";
import { lockVault } from "../lib/vaultStore.js";
import { Passwords } from "../screens/Passwords.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  listVaultItems: vi.fn(),
  listVaultShares: vi.fn(),
  listVaultSharedWithMe: vi.fn(),
  createVaultItem: vi.fn(),
  updateVaultItem: vi.fn(),
  deleteVaultItem: vi.fn(),
  shareVaultItem: vi.fn(),
  unshareVaultItem: vi.fn(),
  getBreachRange: vi.fn(),
  getUser: vi.fn(),
}));

const PASSWORD = "correct horse battery staple";
let me: crypto.KeyPair;
let bob: crypto.KeyPair;
let identity: { userId: string; handle: string; publicKey: Uint8Array; privateKey: Uint8Array; subscriptionTier: "free" };
let conversations: unknown[];
vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({ state: { identity, conversations }, dispatch: vi.fn() }),
}));

const ID = (n: number) => `${String(n).padStart(8, "0")}-0000-4000-8000-000000000000`;

function seal(data: VaultData) {
  return sealItem(data, vaultKey(me.privateKey));
}

beforeAll(async () => {
  await crypto.ready();
  me = crypto.generateKeyPair();
  bob = crypto.generateKeyPair();
  rememberWrappedKey(crypto.wrapPrivateKey(me.privateKey, PASSWORD));
});

beforeEach(() => {
  vi.clearAllMocks();
  localStorage.clear();
  lockVault();
  identity = { userId: "me", handle: "me", publicKey: me.publicKey, privateKey: me.privateKey, subscriptionTier: "free" };
  conversations = [{ kind: "dm", id: "c1", userId: "bob-id", handle: "bob", publicKey: bob.publicKey, verified: true }];
  const owner = crypto.generateKeyPair();
  vi.mocked(api.listVaultItems).mockResolvedValue({
    items: [
      { id: ID(1), revision: 3, ...seal({ v: 1, type: "login", title: "Cloud console", username: "admin@example.com", password: "Qp8#nZ2!wR6@yK4$tM9&vL3x", urls: ["https://console.example.net"], fromDesktop: "kept" }) },
      { id: ID(2), revision: 1, ...seal({ v: 1, type: "login", title: "Old forum account", username: "you_1990", password: "password" }) },
      { id: ID(3), revision: 1, ...seal({ v: 1, type: "filekey", title: "Tax archive key", key: "AAAA" }) },
    ],
  });
  vi.mocked(api.listVaultShares).mockResolvedValue({ shares: [{ itemId: ID(1), recipientId: "bob-id" }] });
  vi.mocked(api.listVaultSharedWithMe).mockResolvedValue({
    items: [{ itemId: ID(9), ownerId: "owner-id", sealed: sealForRecipient({ type: "login", title: "Team Wi-Fi login", username: "team", password: "S3cure!Team#2026xyz" }, me.publicKey) }],
  });
  // "password" is in a breach; nothing else is.
  vi.mocked(api.getBreachRange).mockImplementation(async (prefix) => (prefix === breachHash("password").slice(0, 5) ? `${breachHash("password").slice(5)}:10434004\r\n` : "0000000000000000000000000000000000A:1"));
  vi.mocked(api.updateVaultItem).mockImplementation(async (id, revision, sealed) => ({ id, revision: revision + 1, ...sealed }));
  vi.mocked(api.createVaultItem).mockImplementation(async (sealed) => ({ id: ID(7), revision: 1, ...sealed }));
  vi.mocked(api.shareVaultItem).mockResolvedValue(undefined);
  Object.defineProperty(navigator, "clipboard", { value: { writeText: vi.fn().mockResolvedValue(undefined) }, configurable: true });
});

afterEach(() => {
  lockVault();
});

async function unlock(user: ReturnType<typeof userEvent.setup>) {
  render(<Passwords />);
  await user.type(screen.getByLabelText("Account password"), PASSWORD);
  await user.click(within(screen.getByRole("region", { name: "Vault locked" })).getByRole("button", { name: "Unlock" }));
  await screen.findByRole("region", { name: "Vault items" });
}

describe("Passwords (web)", () => {
  it("starts locked with no item data on the page, and refuses the wrong password", async () => {
    const user = userEvent.setup();
    render(<Passwords />);
    expect(screen.getByRole("heading", { name: /your vault is locked/i })).toBeInTheDocument();
    expect(api.listVaultItems).not.toHaveBeenCalled();
    expect(document.body.textContent).not.toMatch(/Cloud console/);
    await user.type(screen.getByLabelText("Account password"), "not it");
    await user.click(within(screen.getByRole("region", { name: "Vault locked" })).getByRole("button", { name: "Unlock" }));
    expect(await screen.findByRole("alert")).toHaveTextContent(/not your account password/i);
    expect(api.listVaultItems).not.toHaveBeenCalled();
  });

  it("shows the desktop app's items (never file keys), including read-only copies shared with you", async () => {
    const user = userEvent.setup();
    await unlock(user);
    const list = screen.getByRole("region", { name: "Vault items" });
    expect(within(list).getByText("Cloud console")).toBeInTheDocument();
    expect(within(list).getByText("Old forum account")).toBeInTheDocument();
    expect(within(list).getByText("Team Wi-Fi login")).toBeInTheDocument();
    expect(within(list).queryByText("Tax archive key")).not.toBeInTheDocument();
    const nav = screen.getByRole("navigation", { name: "Password categories" });
    expect(within(nav).getByRole("button", { name: /all items/i })).toHaveTextContent("3");

    await user.click(within(list).getByText("Team Wi-Fi login"));
    expect(screen.getByText(/shared by a contact · read-only/i)).toBeInTheDocument();
    expect(screen.queryByRole("button", { name: "Edit" })).not.toBeInTheDocument();
  });

  it("masks passwords until revealed and copies them", async () => {
    const user = userEvent.setup();
    await unlock(user);
    await user.click(within(screen.getByRole("region", { name: "Vault items" })).getByText("Cloud console"));
    expect(screen.queryByText("Qp8#nZ2!wR6@yK4$tM9&vL3x")).not.toBeInTheDocument();
    expect(screen.getByText(/strong · 24 characters/i)).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Show password" }));
    expect(screen.getByText("Qp8#nZ2!wR6@yK4$tM9&vL3x")).toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Copy password" }));
    expect(await navigator.clipboard.readText()).toBe("Qp8#nZ2!wR6@yK4$tM9&vL3x");
    expect(screen.getByText("Shared with", { selector: "dt" }).nextSibling).toHaveTextContent("@bob");
  });

  it("flags a password found in a breach, checked with only a hash prefix", async () => {
    const user = userEvent.setup();
    await unlock(user);
    await waitFor(() => expect(screen.getByRole("button", { name: /exposed in a breach/i })).toHaveTextContent("1"));
    for (const [prefix] of vi.mocked(api.getBreachRange).mock.calls) expect(prefix).toMatch(/^[0-9A-F]{5}$/);
    await user.click(within(screen.getByRole("region", { name: "Vault items" })).getByText("Old forum account"));
    expect(screen.getByRole("alert")).toHaveTextContent(/appeared in a known data breach/i);
    expect(screen.getByText(/found in 10,434,004 breaches/i)).toBeInTheDocument();
  });

  it("saves edits sealed for the desktop, keeping fields it doesn't know, and re-seals the copy for people it's shared with", async () => {
    const user = userEvent.setup();
    await unlock(user);
    await user.click(within(screen.getByRole("region", { name: "Vault items" })).getByText("Cloud console"));
    await user.click(screen.getByRole("button", { name: "Edit" }));
    const dialog = screen.getByRole("dialog", { name: /edit cloud console/i });
    const username = within(dialog).getByLabelText("Username");
    await user.clear(username);
    await user.type(username, "root@example.com");
    await user.click(within(dialog).getByRole("button", { name: "Save" }));

    await waitFor(() => expect(api.updateVaultItem).toHaveBeenCalledTimes(1));
    const [id, revision, sealed] = vi.mocked(api.updateVaultItem).mock.calls[0];
    expect([id, revision]).toEqual([ID(1), 3]);
    const saved = openItem(sealed.ciphertext, sealed.nonce, vaultKey(me.privateKey));
    expect(saved).toMatchObject({ type: "login", title: "Cloud console", username: "root@example.com", fromDesktop: "kept", v: 1, boundId: ID(1), rev: 1 });
    expect(saved.passwordChanged).toBeUndefined();

    await waitFor(() => expect(api.shareVaultItem).toHaveBeenCalledTimes(1));
    const [sharedId, recipient, copy] = vi.mocked(api.shareVaultItem).mock.calls[0];
    expect([sharedId, recipient]).toEqual([ID(1), "bob-id"]);
    expect(openFromSender(copy, bob.publicKey, bob.privateKey)).toMatchObject({ username: "root@example.com" });
  });

  it("adds a new login with a generated password", async () => {
    const user = userEvent.setup();
    await unlock(user);
    await user.click(screen.getByRole("button", { name: "Add an item" }));
    const dialog = screen.getByRole("dialog", { name: /new item/i });
    await user.type(within(dialog).getByLabelText("Name"), "Work email");
    await user.click(within(dialog).getByRole("button", { name: "Generate a password" }));
    await user.click(within(dialog).getByRole("button", { name: "Save" }));
    await waitFor(() => expect(api.createVaultItem).toHaveBeenCalledTimes(1));
    await waitFor(() => expect(api.updateVaultItem).toHaveBeenCalledTimes(1));
    const [boundId, , boundCopy] = vi.mocked(api.updateVaultItem).mock.calls[0];
    expect(openItem(boundCopy.ciphertext, boundCopy.nonce, vaultKey(me.privateKey))).toMatchObject({ boundId, rev: 1 });
    const data = openItem(vi.mocked(api.createVaultItem).mock.calls[0][0].ciphertext, vi.mocked(api.createVaultItem).mock.calls[0][0].nonce, vaultKey(me.privateKey));
    expect(data).toMatchObject({ type: "login", title: "Work email", urls: [], v: 1 });
    expect(data.password).toHaveLength(24);
    expect(data.passwordChanged).toEqual(expect.any(Number));
    expect(await screen.findByRole("heading", { name: "Work email" })).toBeInTheDocument();
  });

  it("M-24: leaves out an item in another record's place or older than this device saw, and says so", async () => {
    const bound = (title: string, boundId: string, rev: number) => seal({ v: 1, type: "login", title, boundId, rev });
    vi.mocked(api.listVaultItems).mockResolvedValue({
      items: [
        { id: ID(1), revision: 2, ...bound("Moved here", ID(2), 1) },
        { id: ID(2), revision: 4, ...bound("Current copy", ID(2), 5) },
      ],
    });
    const user = userEvent.setup();
    await unlock(user);
    expect(await screen.findByRole("alert")).toHaveTextContent("One item isn't shown");
    expect(screen.getAllByText("Current copy").length).toBeGreaterThan(0);
    expect(screen.queryByText("Moved here")).not.toBeInTheDocument();

    for (const older of [bound("Older copy", ID(2), 4), seal({ v: 1, type: "login", title: "Unbound copy" })]) {
      cleanup();
      lockVault();
      vi.mocked(api.listVaultItems).mockResolvedValue({ items: [{ id: ID(2), revision: 3, ...older }] });
      await unlock(user);
      expect(await screen.findByRole("alert")).toHaveTextContent("One item isn't shown");
      expect(screen.queryByText(/Older copy|Unbound copy/)).not.toBeInTheDocument();
    }
  });

  it("a shared item can't go back to an older copy either", async () => {
    const sharedCopy = (title: string, rev: number) => sealForRecipient({ v: 1, type: "login", title, boundId: ID(9), rev }, me.publicKey);
    vi.mocked(api.listVaultSharedWithMe).mockResolvedValue({ items: [{ itemId: ID(9), ownerId: "owner-id", sealed: sharedCopy("Shared now", 3) }] });
    const user = userEvent.setup();
    await unlock(user);
    expect(screen.getAllByText("Shared now").length).toBeGreaterThan(0);
    cleanup();
    lockVault();
    vi.mocked(api.listVaultSharedWithMe).mockResolvedValue({ items: [{ itemId: ID(9), ownerId: "owner-id", sealed: sharedCopy("Shared before", 2) }] });
    await unlock(user);
    expect(await screen.findByRole("alert")).toHaveTextContent("One item isn't shown");
    expect(screen.queryByText("Shared before")).not.toBeInTheDocument();
  });

  it("control: an item saved before binding still shows", async () => {
    const user = userEvent.setup();
    await unlock(user);
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    expect(screen.getAllByText("Old forum account").length).toBeGreaterThan(0);
  });

  it("Lock now drops every item from the page", async () => {
    const user = userEvent.setup();
    await unlock(user);
    await user.click(screen.getByRole("button", { name: "Lock now" }));
    expect(screen.getByRole("heading", { name: /your vault is locked/i })).toBeInTheDocument();
    expect(document.body.textContent).not.toMatch(/Cloud console|admin@example/);
  });

  it("locks by itself after 5 minutes without activity", async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    try {
      const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime });
      await unlock(user);
      await act(async () => {
        vi.advanceTimersByTime(5 * 60 * 1000 + 10);
      });
      expect(screen.getByRole("heading", { name: /your vault is locked/i })).toBeInTheDocument();
    } finally {
      vi.useRealTimers();
    }
  });
});
