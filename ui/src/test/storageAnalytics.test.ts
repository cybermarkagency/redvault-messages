import sodium from "libsodium-wrappers-sumo";
import { beforeAll, beforeEach, describe, expect, it, vi } from "vitest";
import {
  type AnalyticsJournal,
  addDays,
  DAY_MS,
  openJournal,
  recordCleanup,
  sealJournal,
  startOfDay,
  syncJournal,
  updateJournal,
} from "../lib/analyticsJournal.js";
import * as api from "../lib/api.js";
import type { BackupItemDto, BackupLinkDto } from "../lib/api.js";
import { decodeDriveEntry, type DriveEntry, driveKey } from "../lib/driveMeta.js";
import * as crypto from "../lib/e2eCrypto.js";
import { indexDrive } from "../lib/storageAnalytics.js";

vi.mock("../lib/api.js", async (importOriginal) => ({
  ...(await importOriginal<typeof import("../lib/api.js")>()),
  getAnalyticsJournal: vi.fn(),
  saveAnalyticsJournal: vi.fn(),
}));

const GB = 1024 ** 3;
const MB = 1024 ** 2;
const NOW = new Date(2026, 8, 30, 15, 0, 0).getTime();

let seq = 0;
function entry(overrides: Partial<DriveEntry> = {}): DriveEntry {
  const id = overrides.id ?? `00000000-0000-4000-8000-${String(++seq).padStart(12, "0")}`;
  return {
    id,
    kind: "file",
    name: `file-${seq}.bin`,
    parent: "",
    mime: "application/octet-stream",
    size: MB,
    storedBytes: MB,
    modified: NOW - DAY_MS,
    fileId: id,
    version: 1,
    trashed: false,
    trashedAt: 0,
    pcBackup: false,
    linkIds: [],
    sharedWith: [],
    locked: false,
    unreadable: false,
    meta: {},
    ...overrides,
  };
}

function link(overrides: Partial<BackupLinkDto> & { backupId: string }): BackupLinkDto {
  return { id: `link${++seq}abcdefghijklmnop`, hasPassword: false, allowDownload: true, expiresAt: null, ...overrides };
}

beforeAll(async () => {
  await crypto.ready();
});

describe("Files metadata (desktop format)", () => {
  it("decrypts metadata sealed exactly the way the desktop app seals it", () => {
    const { privateKey } = crypto.generateKeyPair();
    // desktop/core/src/drive_crypto.cpp: key = BLAKE2b-256(key=private key, "redvault-drive-v1"),
    // JSON padded with spaces to a 128-byte bucket, XChaCha20-Poly1305 with no associated data.
    const key = sodium.crypto_generichash(32, "redvault-drive-v1", privateKey);
    const json = JSON.stringify({
      v: 1,
      kind: "file",
      name: "Tax return 2025.pdf",
      parent: "folder-1",
      modified: NOW - 3 * DAY_MS,
      trashed: false,
      trashedAt: 0,
      mime: "application/pdf",
      size: 12345,
      fileId: "file-abc",
      version: 2,
      key: "AAAA",
      links: { linkOne: "secret" },
    });
    const plain = json + " ".repeat(Math.ceil((json.length + 1) / 128) * 128 - json.length);
    expect(plain.length % 128).toBe(0);
    const nonce = new Uint8Array(24).fill(7);
    const ciphertext = sodium.crypto_aead_xchacha20poly1305_ietf_encrypt(plain, null, null, nonce, key);
    const item: BackupItemDto = {
      id: "11111111-2222-4333-8444-555555555555",
      kind: "file",
      encryptedSize: 12400,
      ciphertextSha256: "ab".repeat(32),
      metaCiphertext: crypto.toBase64(ciphertext),
      metaNonce: crypto.toBase64(nonce),
      sharedWith: ["user-2"],
    };

    const e = decodeDriveEntry(item, driveKey(privateKey));
    expect(e).toMatchObject({
      name: "Tax return 2025.pdf",
      parent: "folder-1",
      mime: "application/pdf",
      size: 12345,
      storedBytes: 12400,
      fileId: "file-abc",
      version: 2,
      trashed: false,
      linkIds: ["linkOne"],
      sharedWith: ["user-2"],
      locked: false,
      unreadable: false,
    });
  });

  it("counts an item it can't decrypt by its encrypted size only", () => {
    const mine = crypto.generateKeyPair().privateKey;
    const other = crypto.generateKeyPair().privateKey;
    const sealed = crypto.encryptWithConversationKey('{"name":"x"}', driveKey(other));
    const e = decodeDriveEntry(
      { id: "a", kind: "file", encryptedSize: 999, ciphertextSha256: null, metaCiphertext: crypto.toBase64(sealed.ciphertext), metaNonce: crypto.toBase64(sealed.nonce), sharedWith: [] },
      driveKey(mine)
    );
    expect(e.unreadable).toBe(true);
    expect(e.storedBytes).toBe(999);
    expect(e.name).toBe("Encrypted file");
  });
});

describe("analytics journal", () => {
  const pk = () => crypto.generateKeyPair().privateKey;

  function visit(prev: AnalyticsJournal | null, entries: DriveEntry[], links: BackupLinkDto[], now: number, retentionDays = 30) {
    const index = indexDrive(entries);
    return updateJournal(prev, { entries, inTrash: index.inTrash, links, snapshot: { u: 1, f: entries.length, b: 0, t: 0 }, now, retentionDays });
  }

  it("notices deletions, renames, restores and share links between visits, but not on the first", () => {
    const a = entry({ name: "a.txt" });
    const b = entry({ name: "b.txt", storedBytes: 5 * MB });
    const c = entry({ name: "c.txt", trashed: true, trashedAt: NOW - DAY_MS });
    const kept = link({ backupId: a.id });
    const gone = link({ backupId: a.id });
    const first = visit(null, [a, b, c], [kept, gone], NOW - DAY_MS);
    expect(first.events).toEqual([]);
    expect(first.links[kept.id].s).toBe(0);

    const later = link({ backupId: a.id, expiresAt: new Date(NOW - 1000).toISOString() });
    const second = visit(first, [{ ...a, name: "a-renamed.txt" }, { ...c, trashed: false, trashedAt: 0 }], [kept, later], NOW);
    const kinds = second.events.map((e) => [e.k, e.n ?? null, e.c ?? null]);
    expect(kinds).toEqual(
      expect.arrayContaining([
        ["deleted", null, 1],
        ["renamed", "a-renamed.txt", null],
        ["restored", "c.txt", null],
        ["link-created", "a-renamed.txt", null],
        ["link-expired", "a-renamed.txt", null],
        ["link-removed", null, null],
      ])
    );
    expect(second.events.find((e) => e.k === "deleted")).toMatchObject({ b: 5 * MB, o: 3 });
    expect(second.snapshots.map((s) => s.d)).toEqual([startOfDay(NOW - DAY_MS), startOfDay(NOW)]);
  });

  it("keeps one snapshot per day and drops anything older than the plan's history", () => {
    let j = visit(null, [entry()], [], NOW - 40 * DAY_MS, 365);
    j = { ...j, events: [{ t: NOW - 40 * DAY_MS, k: "deleted", c: 1 }] };
    j = visit(j, [entry()], [], NOW - 1000, 30);
    j = visit(j, [entry()], [], NOW, 30);
    expect(j.snapshots.map((s) => s.d)).toEqual([startOfDay(NOW)]);
    expect(j.events.filter((e) => e.t < NOW - 31 * DAY_MS)).toEqual([]);
  });

  it("records a cleanup as such, so the next visit doesn't call it a deletion", () => {
    const a = entry();
    const b = entry();
    const first = visit(null, [a, b], [], NOW - DAY_MS);
    const cleaned = recordCleanup(first, { label: "Emptied the Trash", itemIds: [b.id], linkIds: [], bytes: MB, count: 1, now: NOW - 1000 });
    const next = visit(cleaned, [a], [], NOW);
    expect(next.events.map((e) => e.k)).toEqual(["cleanup"]);
  });

  it("is sealed so only this account can open it, in 1 KB steps", () => {
    const key = pk();
    const j = visit(null, [entry({ name: "secret-plans.pdf" })], [], NOW);
    const sealed = sealJournal(j, key);
    expect((crypto.fromBase64(sealed.ciphertext).length - 16) % 1024).toBe(0);
    expect(atob(sealed.ciphertext)).not.toContain("secret");
    expect(openJournal(sealed, key)).toEqual(j);
    expect(openJournal(sealed, pk())).toBeNull();
  });

  it("starts over from the other device's version when a save conflicts", async () => {
    const key = pk();
    const theirs = visit(null, [entry()], [], NOW - DAY_MS);
    vi.mocked(api.getAnalyticsJournal)
      .mockResolvedValueOnce({ journal: null })
      .mockResolvedValueOnce({ journal: { ...sealJournal(theirs, key), revision: 1 } });
    vi.mocked(api.saveAnalyticsJournal)
      .mockRejectedValueOnce(new api.ApiError(409, "analytics_conflict", "conflict"))
      .mockResolvedValueOnce({ revision: 2 });
    const seen: (AnalyticsJournal | null)[] = [];
    const result = await syncJournal(key, (prev) => {
      seen.push(prev);
      return visit(prev, [entry()], [], NOW);
    });
    expect(seen[0]).toBeNull();
    expect(seen[1]).toEqual(theirs);
    expect(vi.mocked(api.saveAnalyticsJournal).mock.calls[1][0].revision).toBe(1);
    expect(result.snapshots).toHaveLength(2);
  });
});
