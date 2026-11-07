import { render, screen, waitFor, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import { openJournal } from "../lib/analyticsJournal.js";
import * as api from "../lib/api.js";
import type { BackupItemDto } from "../lib/api.js";
import { driveKey, openDriveMeta, sealDriveMeta } from "../lib/driveMeta.js";
import * as crypto from "../lib/e2eCrypto.js";
import { Analytics } from "../screens/Analytics.js";
import type { AnalyticsPage } from "../state/reducer.js";
import type { SubscriptionTier } from "../types.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  listBackupItems: vi.fn(),
  listBackupLinks: vi.fn(),
  deleteBackupItem: vi.fn(),
  deleteBackupLink: vi.fn(),
  updateBackupMeta: vi.fn(),
  getAnalyticsJournal: vi.fn(),
  saveAnalyticsJournal: vi.fn(),
}));

const dispatch = vi.fn();
let identity: { userId: string; privateKey: Uint8Array; subscriptionTier: SubscriptionTier } | null = null;
let page: AnalyticsPage = "overview";
vi.mock("../state/AppStateContext.js", () => ({
  useAppState: () => ({ state: { identity, screen: { key: "analytics", page } }, dispatch }),
}));

const MB = 1024 ** 2;
const DAY = 86_400_000;
let privateKey: Uint8Array;
let saved: { ciphertext: string; nonce: string; revision: number } | null;

function item(id: string, meta: Record<string, unknown>, encryptedSize: number): BackupItemDto {
  const sealed = sealDriveMeta(JSON.stringify({ v: 1, fileId: id, version: 1, ...meta }), driveKey(privateKey));
  return {
    id,
    kind: meta.kind === "folder" ? "folder" : "file",
    encryptedSize,
    ciphertextSha256: null,
    metaCiphertext: crypto.toBase64(sealed.ciphertext),
    metaNonce: crypto.toBase64(sealed.nonce),
    sharedWith: [],
  };
}

const ID = (n: number) => `${String(n).padStart(8, "0")}-0000-4000-8000-000000000000`;

function seed(tier: SubscriptionTier, at: AnalyticsPage = "overview") {
  identity = { userId: "me", privateKey, subscriptionTier: tier };
  page = at;
  const now = Date.now();
  const items = [
    item(ID(1), { kind: "file", name: "holiday.mp4", mime: "video/mp4", size: 40 * MB, modified: now - 3600_000 }, 40 * MB),
    item(ID(2), { kind: "file", name: "notes.txt", mime: "text/plain", size: MB, modified: now - 2 * 3600_000 }, MB),
    item(ID(3), { kind: "file", name: "notes.txt", mime: "text/plain", size: MB, modified: now - 3600_000 }, MB),
    item(ID(4), { kind: "file", name: "old-report.pdf", mime: "application/pdf", size: MB / 2, modified: now - 400 * DAY, key: "AAAA" }, MB / 2),
  ];
  vi.mocked(api.listBackupItems).mockResolvedValue({ items, usage: { used: 42.5 * MB, limit: 100 * MB, remaining: 57.5 * MB, tier } });
  vi.mocked(api.listBackupLinks).mockResolvedValue({ links: [{ id: "linkAAAAAAAAAAAAAAAA", backupId: ID(1), hasPassword: false, allowDownload: true, expiresAt: null }] });
  vi.mocked(api.deleteBackupItem).mockResolvedValue(undefined);
  vi.mocked(api.deleteBackupLink).mockResolvedValue(undefined);
  vi.mocked(api.updateBackupMeta).mockImplementation(async (id) => items.find((i) => i.id === id)!);
  saved = null;
  vi.mocked(api.getAnalyticsJournal).mockImplementation(async () => ({ journal: saved }));
  vi.mocked(api.saveAnalyticsJournal).mockImplementation(async (input) => {
    saved = { ciphertext: input.ciphertext, nonce: input.nonce, revision: input.revision + 1 };
    return { revision: saved.revision };
  });
  return items;
}

beforeAll(async () => {
  await crypto.ready();
  privateKey = crypto.generateKeyPair().privateKey;
});

beforeEach(() => {
  vi.clearAllMocks();
});

describe("Analytics (web)", () => {
  it("is locked on Free, with a way to see the plans, and never loads anything", async () => {
    seed("free");
    render(<Analytics />);
    expect(screen.getByRole("heading", { name: /storage analytics/i })).toBeInTheDocument();
    expect(screen.getByText(/12 months of history, custom ranges up to 12 months, CSV and PDF reports/i)).toBeInTheDocument();
    await userEvent.setup().click(screen.getByRole("button", { name: /see plans/i }));
    expect(dispatch).toHaveBeenCalledWith({ type: "OPEN_SETTINGS", section: "account" });
    expect(api.listBackupItems).not.toHaveBeenCalled();
    expect(api.getAnalyticsJournal).not.toHaveBeenCalled();
  });

  it("has the design's sidebar, and each page opens from it", async () => {
    seed("pro");
    const user = userEvent.setup();
    render(<Analytics />);
    const nav = screen.getByRole("navigation", { name: "Analytics" });
    expect(within(nav).getAllByRole("button").map((b) => b.textContent)).toEqual([
      "Overview",
      "Storage",
      "Uploads",
      "Cleanup",
      "PC backup",
      "Share links",
      "Activity & alerts",
      "Upgrade to Premium",
    ]);
    expect(within(nav).getByRole("button", { name: "Overview" })).toHaveAttribute("aria-current", "page");
    await user.click(within(nav).getByRole("button", { name: "Share links" }));
    expect(dispatch).toHaveBeenCalledWith({ type: "OPEN_ANALYTICS", page: "links" });
  });

  it("Overview on Pro: usage from the decrypted metadata, an encrypted journal, and Premium-only reports and ranges", async () => {
    seed("pro");
    const user = userEvent.setup();
    render(<Analytics />);
    const usage = await screen.findByRole("region", { name: "Storage used" });
    expect(within(usage).getByText("42.5 MB")).toBeInTheDocument();
    expect(within(usage).getByText(/43% used · 57.5 MB free/)).toBeInTheDocument();
    expect(screen.getByText("Videos")).toBeInTheDocument();
    expect(screen.getByText("Documents")).toBeInTheDocument();
    expect(screen.getByRole("region", { name: "Recoverable space" })).toHaveTextContent("1.5 MB");

    const csv = screen.getByRole("button", { name: /csv/i });
    expect(csv).toHaveAttribute("aria-disabled", "true");
    expect(csv).toHaveAttribute("title", "Reports are part of Premium");
    await user.click(screen.getByRole("button", { name: /last 30 days/i }));
    const menu = screen.getByRole("dialog", { name: "Date range" });
    expect(within(menu).getByRole("button", { name: /last 12 months/i })).toHaveAttribute("aria-disabled", "true");
    expect(within(menu).getByRole("button", { name: /last 90 days/i })).toHaveAttribute("aria-disabled", "true");
    expect(within(menu).getByText(/custom ranges up to 30 days/i)).toBeInTheDocument();
    expect(within(screen.getByRole("group", { name: "Growth by" })).getByRole("button", { name: /^month/i })).toHaveAttribute("aria-disabled", "true");

    // The journal went to the server encrypted, and only this key opens it.
    await waitFor(() => expect(saved).not.toBeNull());
    expect(atob(saved!.ciphertext)).not.toContain("holiday");
    const journal = openJournal(saved!, privateKey)!;
    expect(journal.snapshots[0]).toMatchObject({ u: 42.5 * MB, f: 4 });
    expect(journal.snapshots[0].s).toEqual(expect.any(Number));
  });

  it("on Premium: CSV and PDF reports and every range", async () => {
    seed("premium");
    const user = userEvent.setup();
    render(<Analytics />);
    await screen.findByRole("region", { name: "Storage used" });
    expect(screen.getByRole("button", { name: /csv/i })).toHaveAttribute("aria-disabled", "false");
    expect(screen.getByRole("button", { name: /pdf/i })).toHaveAttribute("aria-disabled", "false");
    await user.click(screen.getByRole("button", { name: /last 30 days/i }));
    expect(within(screen.getByRole("dialog", { name: "Date range" })).getByRole("button", { name: /last 12 months/i })).toHaveAttribute("aria-disabled", "false");
  });

  it("Cleanup: nothing is deleted until the review is confirmed; keeps the newest duplicate and sends old files to Trash", async () => {
    const items = seed("pro", "cleanup");
    const user = userEvent.setup();
    render(<Analytics />);
    const old = await screen.findByRole("checkbox", { name: "Old files" });
    expect(old).not.toBeChecked();
    expect(screen.getByRole("checkbox", { name: "Duplicates" })).toBeChecked();
    await user.click(old);

    await user.click(screen.getByRole("button", { name: /review and clean up/i }));
    const dialog = screen.getByRole("alertdialog", { name: /free up/i });
    expect(within(dialog).getByText(/the newest copy of each file is kept/i)).toBeInTheDocument();
    expect(within(dialog).getByText(/old files go to trash first/i)).toBeInTheDocument();
    const del = within(dialog).getByRole("button", { name: /delete/i });
    expect(del).toBeDisabled();
    expect(api.deleteBackupItem).not.toHaveBeenCalled();

    await user.click(within(dialog).getByRole("checkbox", { name: /i understand/i }));
    await user.click(del);
    await waitFor(() => expect(api.updateBackupMeta).toHaveBeenCalledTimes(1));
    // The older notes.txt goes; the newer one stays.
    expect(api.deleteBackupItem).toHaveBeenCalledTimes(1);
    expect(api.deleteBackupItem).toHaveBeenCalledWith(items[1].id);
    // The old file is re-sealed as trashed, keeping its own fields.
    const [id, ciphertext, nonce] = vi.mocked(api.updateBackupMeta).mock.calls[0];
    expect(id).toBe(items[3].id);
    const meta = JSON.parse(openDriveMeta(crypto.fromBase64(ciphertext), crypto.fromBase64(nonce), driveKey(privateKey)));
    expect(meta).toMatchObject({ name: "old-report.pdf", key: "AAAA", trashed: true, trashedAt: expect.any(Number) });
    expect(await screen.findByText(/cleaned up storage: done/i)).toBeInTheDocument();
    expect(openJournal(saved!, privateKey)!.events.map((e) => e.k)).toContain("cleanup");
  });

  it("Share links: revoking asks first, then removes the link", async () => {
    seed("pro", "links");
    const user = userEvent.setup();
    render(<Analytics />);
    expect(await screen.findByRole("region", { name: "Open with no expiry" })).toHaveTextContent("1");
    expect(screen.queryByText(/opens/i, { selector: "th" })).not.toBeInTheDocument();
    await user.click(screen.getByRole("button", { name: "Revoke" }));
    const dialog = screen.getByRole("alertdialog", { name: /revoke this link/i });
    await user.click(within(dialog).getByRole("checkbox", { name: /i understand/i }));
    await user.click(within(dialog).getByRole("button", { name: /revoke link/i }));
    await waitFor(() => expect(api.deleteBackupLink).toHaveBeenCalledWith("linkAAAAAAAAAAAAAAAA"));
  });

  it("Activity & alerts: an alert rule can be switched off, and that's kept in the encrypted journal", async () => {
    seed("pro", "activity");
    const user = userEvent.setup();
    render(<Analytics />);
    const openLinks = await screen.findByRole("switch", { name: /open links alerts/i });
    expect(openLinks).toHaveAttribute("aria-checked", "true");
    await waitFor(() => expect(saved).not.toBeNull());
    await user.click(openLinks);
    await waitFor(() => expect(openJournal(saved!, privateKey)!.settings?.alertsOff).toEqual(["open-links"]));
    expect(screen.getByRole("switch", { name: /open links alerts/i })).toHaveAttribute("aria-checked", "false");
  });

  it("says so when Files isn't enabled on the server", async () => {
    seed("pro");
    vi.mocked(api.listBackupItems).mockRejectedValue(new api.ApiError(503, "backup_disabled", "Secure backup isn't enabled on this server."));
    render(<Analytics />);
    expect(await screen.findByRole("alert")).toHaveTextContent(/files aren't enabled on this server/i);
  });
});
