import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import { addDays, type AnalyticsJournal, DAY_MS, startOfDay } from "../lib/analyticsJournal.js";
import type { BackupLinkDto } from "../lib/api.js";
import { type DriveEntry, driveKey, openDriveMeta, trashedMeta } from "../lib/driveMeta.js";
import * as crypto from "../lib/e2eCrypto.js";
import { categoryOf, computeAnalytics, growthSeries, reportToCsv } from "../lib/storageAnalytics.js";
import { rangeFor } from "../screens/Analytics.js";

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

const folder = (overrides: Partial<DriveEntry> = {}) => entry({ kind: "folder", size: 0, storedBytes: 0, name: "Folder", ...overrides });

function link(overrides: Partial<BackupLinkDto> & { backupId: string }): BackupLinkDto {
  return { id: `link${++seq}abcdefghijklmnop`, hasPassword: false, allowDownload: true, expiresAt: null, ...overrides };
}

function report(entries: DriveEntry[], opts: { links?: BackupLinkDto[]; used?: number; limit?: number; journal?: AnalyticsJournal | null; days?: number } = {}) {
  const used = opts.used ?? entries.reduce((n, e) => n + e.storedBytes, 0);
  return computeAnalytics({
    entries,
    links: opts.links ?? [],
    usage: { used, limit: opts.limit ?? 50 * GB },
    journal: opts.journal ?? null,
    now: NOW,
    range: { from: addDays(NOW, -((opts.days ?? 30) - 1)), to: NOW },
    retentionDays: 30,
  });
}

beforeAll(async () => {
  await crypto.ready();
});

beforeEach(() => {
  seq = 0;
});

describe("storage analytics report", () => {
  it("classifies types into the design's five categories", () => {
    expect(categoryOf(entry({ mime: "image/jpeg" }))).toBe("photos");
    expect(categoryOf(entry({ mime: "video/mp4" }))).toBe("videos");
    expect(categoryOf(entry({ name: "a.docx" }))).toBe("documents");
    expect(categoryOf(entry({ name: "a.zip" }))).toBe("archives");
    expect(categoryOf(entry({ mime: "audio/mpeg" }))).toBe("other");
  });

  it("totals usage, types and top-level folders, counting previous versions where the file is", () => {
    const photos = folder({ name: "Photos" });
    const trip = folder({ name: "Trip", parent: photos.id });
    const v1 = entry({ name: "beach.jpg", mime: "image/jpeg", parent: trip.id, storedBytes: 2 * MB, fileId: "beach", version: 1 });
    const v2 = entry({ name: "beach.jpg", mime: "image/jpeg", parent: trip.id, storedBytes: 3 * MB, fileId: "beach", version: 2, locked: true });
    const doc = entry({ name: "cv.pdf", storedBytes: MB });
    const r = report([photos, trip, v1, v2, doc], { limit: 10 * MB });

    expect(r.overview).toMatchObject({ used: 6 * MB, limit: 10 * MB, available: 4 * MB, files: 2, folders: 2, versions: 1, locked: 1 });
    expect(r.byCategory.map((c) => [c.key, c.bytes, c.files, c.color])).toEqual([
      ["documents", MB, 1, "#3987e5"],
      ["photos", 5 * MB, 1, "#d95926"],
    ]);
    expect(r.byFolder.map((f) => [f.label, f.bytes])).toEqual([
      ["Photos", 5 * MB],
      ["Top level", MB],
    ]);
    expect(r.largest[0]).toMatchObject({ entry: { id: v2.id }, bytes: 5 * MB, folder: "Photos", category: "photos" });
  });

  it("keeps the latest 3 versions of each file", () => {
    const versions = [1, 2, 3, 4, 5].map((v) => entry({ name: "plan.docx", fileId: "plan", version: v, storedBytes: v * MB }));
    const r = report(versions);
    expect(r.cleanup.versions).toMatchObject({ count: 2, bytes: 3 * MB });
    expect(new Set(r.cleanup.versions.itemIds)).toEqual(new Set([versions[0].id, versions[1].id]));
  });

  it("only empties Trash items older than 30 days, including everything inside a trashed folder", () => {
    const old = folder({ name: "Old", trashed: true, trashedAt: NOW - 40 * DAY_MS });
    const inside = entry({ parent: old.id, storedBytes: 4 * MB });
    const recent = entry({ trashed: true, trashedAt: NOW - 5 * DAY_MS, storedBytes: 6 * MB });
    const kept = entry({ storedBytes: MB });
    const r = report([old, inside, recent, kept]);

    expect(r.cleanup.trash).toMatchObject({ count: 1, bytes: 4 * MB, allItems: 2, allBytes: 10 * MB });
    expect(new Set(r.cleanup.trash.itemIds)).toEqual(new Set([old.id, inside.id]));
    expect(r.overview.files).toBe(1);
  });

  it("finds likely duplicates by name and size and keeps the newest copy", () => {
    const oldest = entry({ name: "Report.pdf", size: 5000, storedBytes: 5100, modified: NOW - 10 * DAY_MS });
    const newest = entry({ name: "report.pdf", size: 5000, storedBytes: 5100, modified: NOW - 2 * DAY_MS });
    const different = entry({ name: "report.pdf", size: 7000, storedBytes: 7100 });
    const r = report([oldest, newest, different]);

    expect(r.cleanup.duplicates.groups).toHaveLength(1);
    expect(r.cleanup.duplicates.groups[0].keep.id).toBe(newest.id);
    expect(r.cleanup.duplicates).toMatchObject({ count: 1, bytes: 5100, itemIds: [oldest.id] });
    expect(r.recommendations[0]).toMatchObject({ title: "Remove 1 duplicate file", bytes: 5100, target: "duplicates" });
  });

  it("offers files unchanged for a year as old files, to go to the Trash", () => {
    const ancient = entry({ name: "2019.mov", mime: "video/quicktime", modified: NOW - 400 * DAY_MS, storedBytes: GB });
    const r = report([ancient, entry()]);
    expect(r.cleanup.old).toMatchObject({ count: 1, bytes: GB, itemIds: [ancient.id] });
    expect(r.cleanup.total).toBe(GB);
    expect(r.recommendations.find((x) => x.id === "old-videos")).toMatchObject({ title: "Archive old videos", target: "old" });
  });

  it("rebuilds history from upload dates, prefers what earlier visits saw, and forecasts when storage fills up", () => {
    const entries = Array.from({ length: 20 }, (_, i) => entry({ storedBytes: GB, modified: addDays(NOW, -(19 - i)) + 3600_000 }));
    const journal: AnalyticsJournal = {
      v: 1,
      started: addDays(NOW, -10),
      snapshots: [{ d: addDays(NOW, -10), u: 99 * GB, f: 10, b: 0, t: 0, s: 70 }],
      state: {},
      links: {},
      events: [],
    };
    const r = report(entries, { used: 20 * GB, limit: 50 * GB, journal });
    const at = (days: number) => r.history.find((p) => p.day === addDays(NOW, -days))!;
    expect(at(0)).toMatchObject({ used: 20 * GB, estimated: false });
    expect(at(10)).toMatchObject({ used: 99 * GB, estimated: false });
    expect(at(15)).toMatchObject({ used: 5 * GB, estimated: true });
    // After a seen day, estimates carry its gap forward (files deleted since, mostly).
    expect(at(9)).toMatchObject({ used: 100 * GB, estimated: true });
    expect(r.score.change30).toBe(r.score.value - 70);

    const steady = report(entries, { used: 20 * GB, limit: 50 * GB });
    expect(steady.growth).toEqual({ day: GB, week: 7 * GB, month: 20 * GB });
    // A straight-line fit over the last 30 days (10 of them before any uploads): ~0.76 GB/day, 30 GB left.
    expect(steady.forecast.perDay).toBeGreaterThan(0.7 * GB);
    expect(steady.forecast.perDay).toBeLessThan(0.85 * GB);
    expect(steady.forecast.daysLeft).toBeGreaterThan(33);
    expect(steady.forecast.daysLeft).toBeLessThan(45);

    // The growth chart ends in a forecast stretch (about a sixth of its width), never past the limit.
    const series = growthSeries(steady, "day", { from: addDays(NOW, -29), to: NOW });
    const future = series.filter((p) => p.forecast);
    expect(series.filter((p) => !p.forecast)).toHaveLength(30);
    expect(future).toHaveLength(6);
    expect(future.every((p) => p.value > 20 * GB && p.value <= 50 * GB)).toBe(true);
    // Pro's Week view: the last 5 weeks only.
    expect(growthSeries(steady, "week", { from: addDays(NOW, -29), to: NOW }, 5).filter((p) => !p.forecast)).toHaveLength(5);
  });

  it("summarises uploads by day and weekday × hour, and compares with the previous period", () => {
    const at = new Date(2026, 8, 29, 20, 30).getTime(); // a Tuesday evening
    const r = report(
      [
        entry({ modified: at, storedBytes: 3 * MB }),
        entry({ modified: at + 60_000, storedBytes: MB }),
        entry({ modified: addDays(NOW, -10) }), // the previous 7-day period
        entry({ modified: NOW - 90 * DAY_MS }),
      ],
      { days: 7 }
    );
    expect(r.uploads).toMatchObject({ count: 2, bytes: 4 * MB, prevCount: 1, prevBytes: MB, activeDays: 1, unit: "day", busiest: { weekday: 1, hour: 20 } });
    expect(r.uploads.buckets).toHaveLength(7);
    expect(r.uploads.buckets.find((b) => b.start === startOfDay(at))).toMatchObject({ count: 2, bytes: 4 * MB });
    expect(r.uploads.heat[1][20]).toBe(2);
    expect(r.uploads.quietest).not.toBeNull();
    expect(r.recent.map((x) => x.entry.modified)).toEqual([at + 60_000, at]);
  });

  it("raises the four alert kinds, honours switched-off rules and 'mark as expected', and lowers the score", () => {
    const file = entry({ name: "passport.jpg", storedBytes: 2 * MB });
    const trashed = Array.from({ length: 60 }, (_, i) => entry({ modified: NOW - (40 + i) * DAY_MS, trashed: true, trashedAt: NOW - 2 * DAY_MS + i * 1000 }));
    const burst = Array.from({ length: 30 }, (_, i) => entry({ modified: NOW - 3 * DAY_MS + i * 1000 }));
    const quiet = Array.from({ length: 10 }, (_, i) => entry({ modified: NOW - (10 + i) * DAY_MS }));
    const links = [link({ backupId: file.id }), link({ backupId: file.id, hasPassword: true, expiresAt: new Date(NOW + DAY_MS).toISOString() })];
    const journal: AnalyticsJournal = {
      v: 1,
      started: NOW - 20 * DAY_MS,
      snapshots: [],
      state: {},
      links: {},
      events: [{ t: NOW - 2 * DAY_MS, k: "deleted", c: 120, b: 3 * GB, o: 400 }],
    };
    const r = report([file, ...trashed, ...burst, ...quiet], { links, journal });
    expect(r.alerts.items.map((a) => a.rule).sort()).toEqual(["mass-deletion", "mass-trashing", "open-links", "upload-spike"]);
    expect(r.alerts.items.find((a) => a.rule === "mass-trashing")!.detail).toBe("60 files moved to Trash");
    expect(r.alerts.items.find((a) => a.rule === "upload-spike")!.detail).toBe("30 files in 1 hour");
    expect(r.alerts.rules.find((x) => x.rule === "open-links")).toMatchObject({ enabled: true, count: 1 });
    expect(r.score.factors.find((f) => f.name === "Security")).toMatchObject({ ok: false });
    expect(r.links).toMatchObject({ active: 2, withExpiry: 1, openNoExpiry: 1, oldestOpenDays: 20, exposedBytes: 4 * MB });
    expect(r.timeline.some((t) => t.kind === "alert" && t.title === "Upload spike")).toBe(true);

    const spike = r.alerts.items.find((a) => a.rule === "upload-spike")!;
    const quieter = report([file, ...trashed, ...burst, ...quiet], {
      links,
      journal: { ...journal, settings: { alertsOff: ["mass-deletion"], dismissed: [spike.id] } },
    });
    expect(quieter.alerts.items.map((a) => a.rule)).not.toContain("mass-deletion");
    expect(quieter.alerts.items.find((a) => a.rule === "upload-spike")!.status).toBe("resolved");
    expect(quieter.alerts.rules.find((x) => x.rule === "mass-deletion")!.enabled).toBe(false);
  });

  it("reports PC backup health and old versions by age", () => {
    const pc = folder({ name: "PC backup", pcBackup: true });
    const docs = folder({ name: "Documents", parent: pc.id });
    const oldV1 = entry({ parent: docs.id, fileId: "f", version: 1, modified: NOW - 400 * DAY_MS });
    const oldV2 = entry({ parent: docs.id, fileId: "f", version: 2, modified: NOW - 40 * DAY_MS });
    const oldV3 = entry({ parent: docs.id, fileId: "f", version: 3, modified: NOW - 20 * DAY_MS });
    const r = report([pc, docs, oldV1, oldV2, oldV3]);
    expect(r.backup).toMatchObject({ configured: true, files: 1, bytes: 3 * MB, lastBackupAt: NOW - 20 * DAY_MS, status: "stale", folders: ["Documents"] });
    expect(r.backup.oldVersions).toEqual({ count: 2, bytes: 2 * MB });
    expect(r.backup.ages.map((a) => a.count)).toEqual([0, 1, 0, 1]);
    expect(r.backup.versionAges.filter((v) => v.ageDays > 365).map((v) => v.id)).toEqual([oldV1.id]);
    expect(report([entry()]).backup.status).toBe("none");
  });

  it("groups uploads within an hour into one timeline row", () => {
    const t = new Date(2026, 8, 30, 14, 0).getTime();
    const r = report([entry({ name: "a.jpg", modified: t }), entry({ name: "b.jpg", modified: t + 60_000 }), entry({ name: "c.jpg", modified: t - 5 * 3600_000 })]);
    const uploads = r.timeline.filter((x) => x.kind === "upload");
    expect(uploads.map((u) => u.title)).toEqual(["Uploaded 2 files", "Uploaded a file"]);
    expect(uploads[0].detail).toBe("b.jpg, a.jpg");
  });

  it("exports CSV with file names that can't run as spreadsheet formulas", () => {
    const csv = reportToCsv(report([entry({ name: '=HYPERLINK("http://x","y")' })]), { from: NOW - DAY_MS, to: NOW });
    expect(csv).toContain(`"'=HYPERLINK(""http://x"",""y"")"`);
    expect(csv).not.toMatch(/(^|,)=HYPERLINK/m);
  });

  it("moves a file to the Trash by re-sealing its own metadata, keeping every other field", () => {
    const key = driveKey(crypto.generateKeyPair().privateKey);
    const e = entry({ meta: { v: 1, name: "x.pdf", key: "AAAA", header: "BBBB", links: { l1: "s" } } });
    const sealed = trashedMeta(e, key, NOW);
    expect(JSON.parse(openDriveMeta(sealed.ciphertext, sealed.nonce, key))).toEqual({ v: 1, name: "x.pdf", key: "AAAA", header: "BBBB", links: { l1: "s" }, trashed: true, trashedAt: NOW });
    expect(() => trashedMeta({ ...e, meta: null }, key, NOW)).toThrow();
  });
});

describe("date ranges", () => {
  const custom = (from: string, to: string) => ({ from, to });

  it("limits Pro to 30 days of history and Premium to 12 months", () => {
    expect(rangeFor("12m", NOW, 30, custom("", "")).from).toBe(addDays(NOW, -30));
    expect(rangeFor("90d", NOW, 30, custom("", "")).from).toBe(addDays(NOW, -30));
    expect(rangeFor("12m", NOW, 365, custom("", "")).from).toBe(addDays(NOW, -364));
    expect(rangeFor("90d", NOW, 365, custom("", "")).from).toBe(addDays(NOW, -89));
    expect(rangeFor("custom", NOW, 30, custom("2025-01-01", "2026-09-10")).from).toBe(addDays(NOW, -30));
  });

  it("covers whole days", () => {
    expect(rangeFor("7d", NOW, 30, custom("", ""))).toEqual({ from: addDays(NOW, -6), to: NOW });
    expect(rangeFor("30d", NOW, 30, custom("", ""))).toEqual({ from: addDays(NOW, -29), to: NOW });
    const r = rangeFor("custom", NOW, 30, custom("2026-09-20", "2026-09-22"));
    expect(r).toEqual({ from: new Date(2026, 8, 20).getTime(), to: new Date(2026, 8, 23).getTime() - 1 });
  });
});
