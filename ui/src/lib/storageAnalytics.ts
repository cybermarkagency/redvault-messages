// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Storage analytics, computed entirely on this device from the account's
// own Files metadata (decrypted here, lib/driveMeta.ts), its share links and
// the encrypted journal of earlier visits (lib/analyticsJournal.ts). Pure
// functions: no network, no clock of their own (`now` is passed in).
//
// What it can't know, by design: file contents, downloads, who opened a
// share link, devices, IP addresses or locations. The server doesn't
// record any of that, so nothing here pretends to.
import type { BackupLinkDto } from "./api.js";
import { addDays, type AlertRule, type AnalyticsJournal, DAY_MS, type JournalEvent, startOfDay } from "./analyticsJournal.js";
import type { DriveEntry } from "./driveMeta.js";

export type Category = "documents" | "photos" | "videos" | "archives" | "other";

export const CATEGORIES: Category[] = ["documents", "photos", "videos", "archives", "other"];

export const CATEGORY_LABELS: Record<Category, string> = {
  documents: "Documents",
  photos: "Photos",
  videos: "Videos",
  archives: "Archives",
  other: "Other",
};

/** Fixed categorical colours, the same everywhere (validated on the #171C1A card surface). */
export const CATEGORY_COLORS: Record<Category, string> = {
  documents: "#3987e5",
  photos: "#d95926",
  videos: "#199e70",
  archives: "#c98500",
  other: "#d55181",
};

/** Cleanup sources use the first four categorical slots, in this order. */
export const CLEANUP_COLORS = { duplicates: "#3987e5", versions: "#d95926", trash: "#199e70", old: "#c98500" } as const;

/** Cleanup keeps this many newest versions of each file. */
export const KEEP_VERSIONS = 3;
/** Cleanup only empties Trash items that have been there this long. */
export const TRASH_PURGE_DAYS = 30;
/** Files not changed for this long are "old" (when files are opened isn't recorded anywhere). */
export const OLD_FILE_DAYS = 365;
/** No new backup file for this long: the backup looks stale. */
export const STALE_BACKUP_DAYS = 7;

export const ALERT_RULES: { rule: AlertRule; title: string; description: string }[] = [
  { rule: "mass-deletion", title: "Mass deletion", description: "More than 100 files permanently deleted since your last visit." },
  { rule: "mass-trashing", title: "Mass trashing", description: "More than 50 files moved to Trash within 10 minutes." },
  { rule: "upload-spike", title: "Upload spikes", description: "Uploads at 5× your usual hourly rate." },
  { rule: "open-links", title: "Open links", description: "A link stays open with no expiry for 30 days." },
];

const DOC_EXT = /\.(pdf|docx?|odt|rtf|txt|md|xlsx?|ods|csv|pptx?|odp|pages|numbers|key|epub|json|xml|html?)$/i;
const ARCHIVE_EXT = /\.(zip|rar|7z|tar|gz|tgz|bz2|xz|zst|iso|dmg)$/i;

export function categoryOf(entry: DriveEntry): Category {
  const mime = entry.mime.toLowerCase();
  if (mime.startsWith("image/")) return "photos";
  if (mime.startsWith("video/")) return "videos";
  if (ARCHIVE_EXT.test(entry.name) || /zip|x-7z|x-rar|x-tar|gzip/.test(mime)) return "archives";
  if (DOC_EXT.test(entry.name) || mime.startsWith("text/") || /pdf|document|sheet|presentation|msword|opendocument/.test(mime)) {
    return "documents";
  }
  return "other";
}

// --- The drive, indexed ---

export interface DriveIndex {
  entries: DriveEntry[];
  byId: Map<string, DriveEntry>;
  /** Every version of each file, oldest first. */
  versionsOf: Map<string, DriveEntry[]>;
  /** The current (newest) version of each file. */
  current: DriveEntry[];
  isCurrent(e: DriveEntry): boolean;
  /** In the Trash: trashed items and everything inside a trashed folder. */
  inTrash: Set<string>;
  /** When each Trash item got there (its own time, or the trashed folder's it's inside). */
  trashedAtOf(id: string): number;
  /** Inside the desktop's PC backup folder (and the folder itself). */
  inBackup: Set<string>;
  /** The top-level folder an item is under ("" at the top level). */
  topFolderOf(id: string): string;
  /** Everything inside a folder, at any depth. */
  descendantsOf(id: string): DriveEntry[];
}

export function indexDrive(entries: DriveEntry[]): DriveIndex {
  const byId = new Map(entries.map((e) => [e.id, e]));
  const children = new Map<string, DriveEntry[]>();
  const versionsOf = new Map<string, DriveEntry[]>();
  for (const e of entries) {
    if (e.parent) children.set(e.parent, [...(children.get(e.parent) ?? []), e]);
    if (e.kind === "file") versionsOf.set(e.fileId, [...(versionsOf.get(e.fileId) ?? []), e]);
  }
  const current: DriveEntry[] = [];
  const currentIds = new Set<string>();
  for (const versions of versionsOf.values()) {
    versions.sort((a, b) => a.version - b.version || a.modified - b.modified);
    const newest = versions[versions.length - 1];
    current.push(newest);
    currentIds.add(newest.id);
  }
  const descendantsOf = (id: string): DriveEntry[] => {
    const out: DriveEntry[] = [];
    const seen = new Set<string>([id]);
    const queue = [id];
    while (queue.length) {
      for (const child of children.get(queue.pop()!) ?? []) {
        if (seen.has(child.id)) continue;
        seen.add(child.id);
        out.push(child);
        queue.push(child.id);
      }
    }
    return out;
  };
  const inTrash = new Set<string>();
  const trashTime = new Map<string, number>();
  for (const e of entries.filter((x) => x.trashed)) {
    inTrash.add(e.id);
    trashTime.set(e.id, Math.max(trashTime.get(e.id) ?? 0, e.trashedAt));
    for (const d of descendantsOf(e.id)) {
      inTrash.add(d.id);
      if (!d.trashed) trashTime.set(d.id, Math.max(trashTime.get(d.id) ?? 0, e.trashedAt));
    }
  }
  const inBackup = new Set<string>();
  for (const f of entries.filter((x) => x.kind === "folder" && x.pcBackup)) {
    inBackup.add(f.id);
    for (const d of descendantsOf(f.id)) inBackup.add(d.id);
  }
  const topCache = new Map<string, string>();
  const topFolderOf = (id: string): string => {
    const cached = topCache.get(id);
    if (cached !== undefined) return cached;
    let top = "";
    let cursor = byId.get(id);
    const seen = new Set<string>();
    while (cursor && cursor.parent && !seen.has(cursor.id)) {
      seen.add(cursor.id);
      const parent = byId.get(cursor.parent);
      if (!parent) break; // its folder is gone: treat as top level, like the desktop
      top = parent.id;
      cursor = parent;
    }
    topCache.set(id, top);
    return top;
  };
  return {
    entries,
    byId,
    versionsOf,
    current,
    isCurrent: (e) => currentIds.has(e.id),
    inTrash,
    trashedAtOf: (id) => trashTime.get(id) ?? 0,
    inBackup,
    topFolderOf,
    descendantsOf,
  };
}

/** Everything a file takes up: all its versions. */
function fileBytes(index: DriveIndex, file: DriveEntry): number {
  return (index.versionsOf.get(file.fileId) ?? [file]).reduce((n, v) => n + v.storedBytes, 0);
}

function allVersionIds(index: DriveIndex, file: DriveEntry): string[] {
  return (index.versionsOf.get(file.fileId) ?? [file]).map((v) => v.id);
}

// --- Report types ---

export interface Range {
  from: number;
  to: number;
}

export type Granularity = "day" | "week" | "month";

export interface AnalyticsInput {
  entries: DriveEntry[];
  links: BackupLinkDto[];
  usage: { used: number; limit: number };
  journal: AnalyticsJournal | null;
  now: number;
  range: Range;
  /** How far back history goes: Pro 30 days, Premium 365. */
  retentionDays: number;
}

export interface Breakdown {
  key: string;
  label: string;
  bytes: number;
  files: number;
  color?: string;
}

export interface FileRow {
  entry: DriveEntry;
  /** All versions together. */
  bytes: number;
  category: Category;
  folder: string;
}

export interface HistoryPoint {
  day: number;
  used: number;
  /** True when rebuilt from upload dates rather than seen on a visit that day. */
  estimated: boolean;
}

export interface SeriesPoint {
  t: number;
  value: number;
  estimated?: boolean;
  forecast?: boolean;
}

export interface Bucket {
  start: number;
  count: number;
  bytes: number;
}

export type TimelineKind = "upload" | "version" | "trash" | "deleted" | "cleanup" | "renamed" | "restored" | "link" | "alert";

export interface TimelineItem {
  at: number;
  kind: TimelineKind;
  title: string;
  detail: string;
  bytes?: number;
}

export type LinkStatus = "active" | "no-expiry" | "expired";

export interface LinkRow {
  id: string;
  backupId: string;
  name: string;
  bytes: number;
  hasPassword: boolean;
  allowDownload: boolean;
  expiresAt: number | null;
  /** When this app first saw the link; null if it existed before analytics started. */
  created: number | null;
  status: LinkStatus;
  /** Days it's been open, as far as this app knows (from analytics' start when it predates that). */
  openDays: number;
}

export interface AlertItem {
  id: string;
  rule: AlertRule;
  at: number;
  title: string;
  detail: string;
  advice: string;
  status: "open" | "resolved";
}

export interface CleanupCategory {
  itemIds: string[];
  count: number;
  bytes: number;
}

export interface Recommendation {
  id: string;
  title: string;
  detail: string;
  bytes: number;
  target: "duplicates" | "versions" | "trash" | "old" | "links" | "backup" | "upgrade";
}

export interface ScoreFactor {
  name: string;
  ok: boolean;
  detail: string;
}

export interface AnalyticsReport {
  overview: { used: number; limit: number; available: number; percent: number; files: number; folders: number; versions: number; unreadable: number; locked: number };
  score: { value: number; label: "Excellent" | "Good" | "Fair" | "Needs attention"; factors: ScoreFactor[]; change30: number | null };
  byCategory: Breakdown[];
  byFolder: Breakdown[];
  largest: FileRow[];
  recent: FileRow[];
  /** Daily storage used across the whole history window. */
  history: HistoryPoint[];
  growth: { day: number | null; week: number | null; month: number | null };
  forecast: { perDay: number; daysLeft: number | null; fullOn: number | null; basisDays: number };
  uploads: {
    count: number;
    bytes: number;
    prevCount: number;
    prevBytes: number;
    activeDays: number;
    rangeDays: number;
    perDay: number;
    unit: "day" | "week" | "month";
    buckets: Bucket[];
    /** Average per bucket over the previous period of the same length. */
    prevAverage: { count: number; bytes: number };
    /** [weekday Mon..Sun][hour 0..23] upload counts in the range. */
    heat: number[][];
    busiest: { weekday: number; hour: number } | null;
    quietest: { weekday: number; fromHour: number; toHour: number } | null;
  };
  cleanup: {
    duplicates: CleanupCategory & { groups: { name: string; keep: DriveEntry; extras: DriveEntry[] }[] };
    versions: CleanupCategory;
    trash: CleanupCategory & { allItems: number; allBytes: number };
    old: CleanupCategory & { files: FileRow[] };
    total: number;
    percentOfUsed: number;
  };
  recommendations: Recommendation[];
  backup: {
    configured: boolean;
    status: "healthy" | "stale" | "empty" | "none";
    files: number;
    bytes: number;
    lastBackupAt: number | null;
    addedInRange: number;
    perDay: number;
    folders: string[];
    oldVersions: { count: number; bytes: number };
    /** Previous versions by age: under 30 days, 30–90, 90 days–1 year, over 1 year. */
    ages: { label: string; count: number; bytes: number }[];
    /** Previous versions with their age, for "keep versions for…". */
    versionAges: { id: string; ageDays: number; bytes: number }[];
    series: SeriesPoint[];
  };
  links: {
    rows: LinkRow[];
    active: number;
    withExpiry: number;
    expired: number;
    exposedBytes: number;
    openNoExpiry: number;
    oldestOpenDays: number | null;
    mostShared: { name: string; links: number }[];
    directShares: { files: number; people: number };
  };
  alerts: { rules: { rule: AlertRule; enabled: boolean; count: number; openNow: number }[]; items: AlertItem[] };
  timeline: TimelineItem[];
  /** History before this is rebuilt from upload dates (and can't include files deleted before then). */
  journalStarted: number | null;
}

// --- Helpers ---

const within = (t: number, r: Range) => t >= r.from && t <= r.to;
const sum = <T>(list: T[], f: (x: T) => number) => list.reduce((n, x) => n + f(x), 0);

function plural(n: number, one: string, many = `${one}s`): string {
  return `${n.toLocaleString()} ${n === 1 ? one : many}`;
}

export function formatStorage(bytes: number): string {
  const abs = Math.abs(bytes);
  if (abs < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let value = abs / 1024;
  let i = 0;
  while (value >= 1024 && i < units.length - 1) {
    value /= 1024;
    i += 1;
  }
  return `${bytes < 0 ? "-" : ""}${value < 10 ? value.toFixed(1) : value < 100 ? value.toFixed(1) : Math.round(value)} ${units[i]}`;
}

export function bucketUnit(range: Range): "day" | "week" | "month" {
  const days = (range.to - range.from) / DAY_MS;
  if (days <= 45) return "day";
  if (days <= 190) return "week";
  return "month";
}

export function bucketStart(t: number, unit: Granularity): number {
  const d = new Date(startOfDay(t));
  if (unit === "week") d.setDate(d.getDate() - ((d.getDay() + 6) % 7)); // Monday
  if (unit === "month") d.setDate(1);
  return d.getTime();
}

export function nextBucket(t: number, unit: Granularity): number {
  const d = new Date(t);
  if (unit === "day") d.setDate(d.getDate() + 1);
  else if (unit === "week") d.setDate(d.getDate() + 7);
  else d.setMonth(d.getMonth() + 1);
  return d.getTime();
}

function buckets(range: Range, unit: Granularity): Bucket[] {
  const out: Bucket[] = [];
  for (let start = bucketStart(range.from, unit); start <= range.to; start = nextBucket(start, unit)) out.push({ start, count: 0, bytes: 0 });
  return out;
}

function bucketIndex(list: Bucket[], t: number): number {
  let lo = 0;
  let hi = list.length - 1;
  if (hi < 0 || t < list[0].start) return -1;
  while (lo < hi) {
    const mid = (lo + hi + 1) >> 1;
    if (list[mid].start <= t) lo = mid;
    else hi = mid - 1;
  }
  return lo;
}

function median(values: number[]): number {
  if (!values.length) return 0;
  const sorted = [...values].sort((a, b) => a - b);
  const mid = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[mid] : (sorted[mid - 1] + sorted[mid]) / 2;
}

function linearSlope(points: HistoryPoint[]): number {
  if (points.length < 2) return 0;
  const xs = points.map((p) => p.day / DAY_MS);
  const ys = points.map((p) => p.used);
  const mx = sum(xs, (x) => x) / xs.length;
  const my = sum(ys, (y) => y) / ys.length;
  let num = 0;
  let den = 0;
  for (let i = 0; i < xs.length; i++) {
    num += (xs[i] - mx) * (ys[i] - my);
    den += (xs[i] - mx) ** 2;
  }
  return den === 0 ? 0 : num / den;
}

/** Daily storage used: seen values where a visit recorded one, estimated from upload dates elsewhere. */
function buildHistory(input: AnalyticsInput, index: DriveIndex, from: number): HistoryPoint[] {
  const { journal, now, usage } = input;
  const uploads = index.entries
    .filter((e) => e.kind === "file")
    .map((e) => ({ t: e.modified, b: e.storedBytes }))
    .sort((a, b) => a.t - b.t);
  const snapshots = new Map((journal?.snapshots ?? []).map((s) => [s.d, s.u]));
  const today = startOfDay(now);
  const points: HistoryPoint[] = [];
  let cursor = 0;
  let rebuilt = 0;
  while (cursor < uploads.length && uploads[cursor].t < from) rebuilt += uploads[cursor++].b;
  // Scale estimates so today's estimate matches what the server reports (headers, pending uploads).
  const rebuiltToday = sum(uploads, (u) => u.b);
  const scale = rebuiltToday > 0 ? usage.used / rebuiltToday : 1;
  let offset = 0;
  for (let day = startOfDay(from); day <= today; day = nextBucket(day, "day")) {
    const end = nextBucket(day, "day");
    while (cursor < uploads.length && uploads[cursor].t < end) rebuilt += uploads[cursor++].b;
    const seen = day === today ? usage.used : snapshots.get(day);
    if (seen !== undefined) {
      // Carry the gap between what was seen and what upload dates explain
      // (files deleted since, mostly) forward to the estimated days after.
      offset = seen - rebuilt * scale;
      points.push({ day, used: seen, estimated: false });
    } else {
      points.push({ day, used: Math.max(0, Math.round(rebuilt * scale + offset)), estimated: true });
    }
  }
  return points;
}

/**
 * The growth chart: storage used per day, week or month (the value at the
 * end of each), plus a forecast stretch (about a sixth of the chart) at the
 * current rate, never past the plan's limit.
 */
export function growthSeries(report: AnalyticsReport, granularity: Granularity, range: Range, maxPoints?: number): SeriesPoint[] {
  const inRange = report.history.filter((p) => p.day >= startOfDay(range.from) && p.day <= range.to);
  const byBucket = new Map<number, HistoryPoint>();
  for (const p of inRange) byBucket.set(bucketStart(p.day, granularity), p);
  let points: SeriesPoint[] = [...byBucket.entries()].map(([t, p]) => ({ t, value: p.used, estimated: p.estimated }));
  if (maxPoints !== undefined) points = points.slice(-maxPoints);
  if (points.length === 0) return points;
  const last = points[points.length - 1];
  const extra = Math.max(1, Math.round(points.length * 0.19));
  const perDay = report.forecast.perDay;
  const limit = report.overview.limit;
  let t = last.t;
  for (let i = 0; i < extra; i++) {
    t = nextBucket(t, granularity);
    const days = (t - last.t) / DAY_MS;
    const value = perDay > 0 ? Math.min(limit || Infinity, last.value + perDay * days) : last.value;
    points.push({ t, value, forecast: true });
  }
  return points;
}

// --- The report ---

export function computeAnalytics(input: AnalyticsInput): AnalyticsReport {
  const { entries, links, usage, journal, now, range } = input;
  const settings = journal?.settings ?? {};
  const index = indexDrive(entries);
  const live = (e: DriveEntry) => !index.inTrash.has(e.id);
  const liveCurrent = index.current.filter(live);
  const liveFiles = entries.filter((e) => e.kind === "file" && live(e));
  const historyFrom = addDays(now, -input.retentionDays);
  const rangeDays = Math.max(1, Math.round((Math.min(range.to, now) - range.from) / DAY_MS));
  const folderName = (e: DriveEntry) => {
    const top = index.topFolderOf(e.id);
    return top ? index.byId.get(top)?.name ?? "Folder" : "Top level";
  };
  const row = (entry: DriveEntry): FileRow => ({ entry, bytes: fileBytes(index, entry), category: categoryOf(entry), folder: folderName(entry) });

  const overview = {
    used: usage.used,
    limit: usage.limit,
    available: Math.max(0, usage.limit - usage.used),
    percent: usage.limit > 0 ? Math.min(100, (usage.used / usage.limit) * 100) : 0,
    files: liveCurrent.length,
    folders: entries.filter((e) => e.kind === "folder" && live(e)).length,
    versions: liveFiles.length - liveCurrent.length,
    unreadable: entries.filter((e) => e.unreadable).length,
    locked: liveCurrent.filter((e) => e.locked).length,
  };

  // By type and by folder (live files; every version counts where its current version sits)
  const categories = new Map<Category, Breakdown>(CATEGORIES.map((c) => [c, { key: c, label: CATEGORY_LABELS[c], bytes: 0, files: 0, color: CATEGORY_COLORS[c] }]));
  const folders = new Map<string, Breakdown>();
  for (const file of liveCurrent) {
    const bytes = fileBytes(index, file);
    const c = categories.get(categoryOf(file))!;
    c.bytes += bytes;
    c.files += 1;
    const top = index.topFolderOf(file.id);
    const f = folders.get(top) ?? { key: top || "root", label: top ? index.byId.get(top)?.name ?? "Folder" : "Top level", bytes: 0, files: 0 };
    f.bytes += bytes;
    f.files += 1;
    folders.set(top, f);
  }
  const byCategory = [...categories.values()].filter((c) => c.files > 0);
  const byFolder = [...folders.values()].sort((a, b) => b.bytes - a.bytes);

  const largest = liveCurrent.map(row).sort((a, b) => b.bytes - a.bytes).slice(0, 10);
  const recent = liveFiles
    .filter((e) => within(e.modified, range))
    .sort((a, b) => b.modified - a.modified)
    .slice(0, 10)
    .map(row);

  // History, growth and forecast
  const history = buildHistory(input, index, historyFrom);
  const usedDaysAgo = (days: number) => {
    const target = addDays(now, -days);
    const point = history.find((p) => p.day === target);
    return point ? usage.used - point.used : null;
  };
  const growth = { day: usedDaysAgo(1), week: usedDaysAgo(7), month: usedDaysAgo(30) };
  const basis = history.slice(-Math.min(history.length, input.retentionDays > 30 ? 365 : 30));
  const perDay = linearSlope(basis);
  const daysLeft = perDay > 0 && usage.limit > 0 ? Math.max(0, (usage.limit - usage.used) / perDay) : null;
  const forecast = { perDay, daysLeft, fullOn: daysLeft !== null ? now + daysLeft * DAY_MS : null, basisDays: basis.length };

  // Uploads (every version still stored, Trash included), vs the previous period of the same length
  const uploadedAll = entries.filter((e) => e.kind === "file" && e.modified > 0);
  const uploaded = uploadedAll.filter((e) => within(e.modified, range));
  const prevRange = { from: range.from - (range.to - range.from) - 1, to: range.from - 1 };
  const prevUploaded = uploadedAll.filter((e) => within(e.modified, prevRange));
  const unit = bucketUnit(range);
  const uploadBuckets = buckets(range, unit);
  const heat = Array.from({ length: 7 }, () => new Array<number>(24).fill(0));
  const activeDays = new Set<number>();
  for (const e of uploaded) {
    const i = bucketIndex(uploadBuckets, e.modified);
    if (i >= 0) {
      uploadBuckets[i].count += 1;
      uploadBuckets[i].bytes += e.storedBytes;
    }
    const d = new Date(e.modified);
    heat[(d.getDay() + 6) % 7][d.getHours()] += 1;
    activeDays.add(startOfDay(e.modified));
  }
  const prevBucketCount = Math.max(1, buckets(prevRange, unit).length);
  let busiest: AnalyticsReport["uploads"]["busiest"] = null;
  heat.forEach((hours, weekday) =>
    hours.forEach((count, hour) => {
      if (count > 0 && (!busiest || count > heat[busiest.weekday][busiest.hour])) busiest = { weekday, hour };
    })
  );
  // Quietest: the weekday and 6-hour block (00–06, 06–12, …) with the fewest uploads.
  let quietest: AnalyticsReport["uploads"]["quietest"] = null;
  if (uploaded.length) {
    let best = Infinity;
    heat.forEach((hours, weekday) => {
      for (let from = 0; from < 24; from += 6) {
        const n = sum(hours.slice(from, from + 6), (x) => x);
        if (n < best) {
          best = n;
          quietest = { weekday, fromHour: from, toHour: from + 6 };
        }
      }
    });
  }
  const uploads: AnalyticsReport["uploads"] = {
    count: uploaded.length,
    bytes: sum(uploaded, (e) => e.storedBytes),
    prevCount: prevUploaded.length,
    prevBytes: sum(prevUploaded, (e) => e.storedBytes),
    activeDays: activeDays.size,
    rangeDays,
    perDay: uploaded.length / rangeDays,
    unit,
    buckets: uploadBuckets,
    prevAverage: { count: prevUploaded.length / prevBucketCount, bytes: sum(prevUploaded, (e) => e.storedBytes) / prevBucketCount },
    heat,
    busiest,
    quietest,
  };

  // Cleanup: duplicates (keep the newest copy), versions past the newest 3, Trash older than 30 days, old files (to Trash).
  const groupsByKey = new Map<string, DriveEntry[]>();
  for (const file of liveCurrent) {
    if (file.unreadable || file.size === 0) continue;
    // Name and size only: each file has its own key, so contents can't be compared (and locked files are never opened).
    const key = `${file.name.toLowerCase()}\u0000${file.size}`;
    groupsByKey.set(key, [...(groupsByKey.get(key) ?? []), file]);
  }
  const groups = [...groupsByKey.values()]
    .filter((g) => g.length > 1)
    .map((g) => {
      const sorted = [...g].sort((a, b) => b.modified - a.modified);
      return { name: sorted[0].name, keep: sorted[0], extras: sorted.slice(1) };
    });
  const dupFiles = groups.flatMap((g) => g.extras);
  const dupIds = new Set(dupFiles.flatMap((f) => allVersionIds(index, f)));
  const duplicates = {
    itemIds: [...dupIds],
    count: dupFiles.length,
    bytes: sum(dupFiles, (f) => fileBytes(index, f)),
    groups: groups.sort((a, b) => sum(b.extras, (e) => e.storedBytes) - sum(a.extras, (e) => e.storedBytes)),
  };
  const extraVersions = liveCurrent.flatMap((f) => {
    if (dupIds.has(f.id)) return [];
    const versions = (index.versionsOf.get(f.fileId) ?? []).filter(live);
    return versions.slice(0, Math.max(0, versions.length - KEEP_VERSIONS));
  });
  const versions = { itemIds: extraVersions.map((e) => e.id), count: extraVersions.length, bytes: sum(extraVersions, (e) => e.storedBytes) };
  const trashedAll = entries.filter((e) => index.inTrash.has(e.id));
  const trashCutoff = now - TRASH_PURGE_DAYS * DAY_MS;
  const trashOld = trashedAll.filter((e) => index.trashedAtOf(e.id) > 0 && index.trashedAtOf(e.id) < trashCutoff);
  const trashItems = (list: DriveEntry[]) => list.filter((e) => e.trashed && (e.kind === "folder" || index.isCurrent(e))).length;
  const trash = {
    itemIds: trashOld.map((e) => e.id),
    count: trashItems(trashOld),
    bytes: sum(trashOld, (e) => e.storedBytes),
    allItems: trashItems(trashedAll),
    allBytes: sum(trashedAll, (e) => e.storedBytes),
  };
  const oldRows = liveCurrent
    .filter((e) => e.modified > 0 && e.modified < now - OLD_FILE_DAYS * DAY_MS && !dupIds.has(e.id))
    .map(row)
    .sort((a, b) => a.entry.modified - b.entry.modified);
  const old = { itemIds: oldRows.flatMap((r) => allVersionIds(index, r.entry)), count: oldRows.length, bytes: sum(oldRows, (r) => r.bytes), files: oldRows };
  const cleanupTotal = duplicates.bytes + versions.bytes + trash.bytes + old.bytes;
  const cleanup = { duplicates, versions, trash, old, total: cleanupTotal, percentOfUsed: usage.used > 0 ? (cleanupTotal / usage.used) * 100 : 0 };

  // PC backup (the desktop's backup folder)
  const backupFolders = entries.filter((e) => e.kind === "folder" && e.pcBackup && live(e));
  const backupFiles = liveFiles.filter((e) => index.inBackup.has(e.id));
  const backupCurrent = backupFiles.filter((e) => index.isCurrent(e));
  const backupTimes = backupFiles.map((e) => e.modified).filter((t) => t > 0);
  const lastBackupAt = backupTimes.length ? Math.max(...backupTimes) : null;
  const backupOld = backupFiles.filter((e) => !index.isCurrent(e));
  const ageOf = (e: DriveEntry) => Math.max(0, (now - e.modified) / DAY_MS);
  const ageBands: [string, number, number][] = [
    ["Under 30 days", 0, 30],
    ["30–90 days", 30, 90],
    ["90 days – 1 year", 90, 365],
    ["Over 1 year", 365, Infinity],
  ];
  const backupAdded = backupFiles.filter((e) => within(e.modified, range));
  const backupSeries: SeriesPoint[] = [];
  {
    const sorted = [...backupFiles].sort((a, b) => a.modified - b.modified);
    let cursor = 0;
    let total = 0;
    for (let day = startOfDay(range.from); day <= Math.min(range.to, now); day = nextBucket(day, "day")) {
      const end = nextBucket(day, "day");
      while (cursor < sorted.length && sorted[cursor].modified < end) total += sorted[cursor++].storedBytes;
      backupSeries.push({ t: day, value: total, estimated: true });
    }
  }
  const backup: AnalyticsReport["backup"] = {
    configured: backupFolders.length > 0,
    status: !backupFolders.length
      ? "none"
      : !backupCurrent.length
        ? "empty"
        : lastBackupAt !== null && lastBackupAt < now - STALE_BACKUP_DAYS * DAY_MS
          ? "stale"
          : "healthy",
    files: backupCurrent.length,
    bytes: sum(backupFiles, (e) => e.storedBytes),
    lastBackupAt,
    addedInRange: sum(backupAdded, (e) => e.storedBytes),
    perDay: sum(backupAdded, (e) => e.storedBytes) / rangeDays,
    folders: backupFolders.flatMap((f) => entries.filter((e) => e.kind === "folder" && e.parent === f.id && live(e)).map((e) => e.name)).sort(),
    oldVersions: { count: backupOld.length, bytes: sum(backupOld, (e) => e.storedBytes) },
    ages: ageBands.map(([label, lo, hi]) => {
      const inBand = backupOld.filter((e) => ageOf(e) >= lo && ageOf(e) < hi);
      return { label, count: inBand.length, bytes: sum(inBand, (e) => e.storedBytes) };
    }),
    versionAges: backupOld.map((e) => ({ id: e.id, ageDays: ageOf(e), bytes: e.storedBytes })),
    series: backupSeries,
  };

  // Share links
  const startedAt = journal?.started ?? now;
  const linkRows: LinkRow[] = links.map((l) => {
    const file = index.byId.get(l.backupId);
    const seen = journal?.links[l.id]?.s;
    const created = seen ? seen : null;
    const expiresAt = l.expiresAt ? Date.parse(l.expiresAt) : null;
    const status: LinkStatus = expiresAt !== null && expiresAt <= now ? "expired" : expiresAt === null ? "no-expiry" : "active";
    return {
      id: l.id,
      backupId: l.backupId,
      name: file?.name ?? "A file",
      bytes: file ? file.storedBytes : 0,
      hasPassword: l.hasPassword,
      allowDownload: l.allowDownload,
      expiresAt,
      created,
      status,
      openDays: Math.floor((now - (created ?? startedAt)) / DAY_MS),
    };
  });
  const activeRows = linkRows.filter((l) => l.status !== "expired");
  const noExpiry = linkRows.filter((l) => l.status === "no-expiry");
  const linkCount = new Map<string, number>();
  for (const l of activeRows) linkCount.set(l.backupId, (linkCount.get(l.backupId) ?? 0) + 1);
  const people = new Set<string>();
  const sharedFiles = liveCurrent.filter((e) => e.sharedWith.length > 0);
  for (const f of sharedFiles) f.sharedWith.forEach((p) => people.add(p));
  const linksReport: AnalyticsReport["links"] = {
    rows: [...linkRows].sort((a, b) => (a.status === "expired" ? 1 : 0) - (b.status === "expired" ? 1 : 0) || (b.created ?? 0) - (a.created ?? 0)),
    active: activeRows.length,
    withExpiry: activeRows.filter((l) => l.expiresAt !== null).length,
    expired: linkRows.length - activeRows.length,
    exposedBytes: sum(activeRows, (l) => l.bytes),
    openNoExpiry: noExpiry.length,
    oldestOpenDays: noExpiry.length ? Math.max(...noExpiry.map((l) => l.openDays)) : null,
    mostShared: [...linkCount.entries()]
      .map(([id, n]) => ({ name: index.byId.get(id)?.name ?? "A file", links: n }))
      .sort((a, b) => b.links - a.links)
      .slice(0, 5),
    directShares: { files: sharedFiles.length, people: people.size },
  };

  // Alerts
  const alertsOff = new Set(settings.alertsOff ?? []);
  const dismissed = new Set(settings.dismissed ?? []);
  const found: Omit<AlertItem, "status">[] = [];
  for (const e of journal?.events ?? []) {
    if (e.k === "deleted" && (e.c ?? 0) >= 100) {
      found.push({
        id: `mass-deletion-${e.t}`,
        rule: "mass-deletion",
        at: e.t,
        title: "Mass deletion",
        detail: `${plural(e.c ?? 0, "item")} (${formatStorage(e.b ?? 0)}) permanently deleted since your last visit`,
        advice: "If this wasn't you, change your password and check Linked devices.",
      });
    }
  }
  {
    // Files moved to the Trash, by when; a trashed folder counts the files inside it.
    const moves = entries
      .filter((e) => e.trashed && e.trashedAt > 0 && (e.kind === "folder" || index.isCurrent(e)))
      .map((e) => ({ t: e.trashedAt, n: e.kind === "folder" ? Math.max(1, index.descendantsOf(e.id).filter((d) => d.kind === "file" && index.isCurrent(d)).length) : 1 }))
      .sort((a, b) => a.t - b.t);
    let start = 0;
    let running = 0;
    let lastAlertEnd = -Infinity;
    for (let i = 0; i < moves.length; i++) {
      running += moves[i].n;
      while (moves[i].t - moves[start].t > 10 * 60_000) running -= moves[start++].n;
      if (running > 50 && moves[start].t > lastAlertEnd) {
        let j = i;
        let total = running;
        while (j + 1 < moves.length && moves[j + 1].t - moves[start].t <= 10 * 60_000) total += moves[++j].n;
        found.push({
          id: `mass-trashing-${moves[start].t}`,
          rule: "mass-trashing",
          at: moves[start].t,
          title: "Mass trashing",
          detail: `${plural(total, "file")} moved to Trash`,
          advice: "Restore them from the Trash in the desktop app if that wasn't you.",
        });
        lastAlertEnd = moves[j].t;
        i = j;
        start = j + 1;
        running = 0;
      }
    }
  }
  {
    const perHour = new Map<number, number>();
    for (const e of uploadedAll) {
      const hour = Math.floor(e.modified / 3_600_000);
      perHour.set(hour, (perHour.get(hour) ?? 0) + 1);
    }
    const usual = Math.max(1, median([...perHour.values()]));
    for (const [hour, count] of perHour) {
      if (count >= 20 && count >= 5 * usual) {
        found.push({
          id: `upload-spike-${hour}`,
          rule: "upload-spike",
          at: hour * 3_600_000,
          title: "Upload spike",
          detail: `${plural(count, "file")} in 1 hour`,
          advice: `${Math.round(count / usual)}× your usual rate. If this wasn't you, change your password.`,
        });
      }
    }
  }
  const staleLinks = noExpiry.filter((l) => l.openDays >= 30 || l.created === null);
  if (staleLinks.length) {
    found.push({
      id: "open-links",
      rule: "open-links",
      at: now,
      title: "Open links",
      detail: `${plural(staleLinks.length, "link")} with no expiry for 30+ days`,
      advice: "",
    });
  }
  const alertItems: AlertItem[] = found
    .filter((a) => !alertsOff.has(a.rule) && (a.rule === "open-links" || within(a.at, range)))
    .map((a) => ({ ...a, status: dismissed.has(a.id) ? ("resolved" as const) : ("open" as const) }))
    .sort((a, b) => (a.status === b.status ? b.at - a.at : a.status === "open" ? -1 : 1));
  const alerts = {
    rules: ALERT_RULES.map(({ rule }) => {
      const mine = found.filter((a) => a.rule === rule && (rule === "open-links" || within(a.at, range)));
      return {
        rule,
        enabled: !alertsOff.has(rule),
        count: rule === "open-links" ? staleLinks.length : mine.length,
        openNow: mine.filter((a) => !dismissed.has(a.id)).length,
      };
    }),
    items: alertItems,
  };

  // Score: capacity, reclaimable clutter, backup health, security.
  const factors: ScoreFactor[] = [];
  let score = 100;
  if (overview.percent >= 90) score -= 30;
  else if (overview.percent >= 75) score -= 15;
  factors.push({ name: "Free space", ok: overview.percent < 75, detail: `${overview.percent.toFixed(0)}% used` });
  const clutterShare = usage.used > 0 ? cleanupTotal / usage.used : 0;
  if (clutterShare >= 0.2) score -= 20;
  else if (clutterShare >= 0.05) score -= 8;
  factors.push({ name: "Duplicates and clutter", ok: clutterShare < 0.05, detail: `${formatStorage(cleanupTotal)} could be freed` });
  if (backup.status === "stale") score -= 15;
  else if (backup.status === "none" || backup.status === "empty") score -= 5;
  factors.push({ name: "Backup health", ok: backup.status === "healthy", detail: { healthy: "Backing up", stale: "No new files lately", empty: "Set up but empty", none: "Not set up" }[backup.status] });
  const securityIssues = alertItems.filter((a) => a.status === "open").length;
  score -= Math.min(30, securityIssues * 10);
  factors.push({ name: "Security", ok: securityIssues === 0, detail: securityIssues ? plural(securityIssues, "open alert") : "No open alerts" });
  const value = Math.max(0, Math.min(100, Math.round(score)));
  const monthAgo = (journal?.snapshots ?? []).filter((s) => s.s !== undefined && s.d <= addDays(now, -30)).pop() ?? (journal?.snapshots ?? []).find((s) => s.s !== undefined && s.d < startOfDay(now));
  const scoreReport = {
    value,
    label: (value >= 90 ? "Excellent" : value >= 75 ? "Good" : value >= 50 ? "Fair" : "Needs attention") as AnalyticsReport["score"]["label"],
    factors,
    change30: monthAgo?.s !== undefined ? value - monthAgo.s : null,
  };

  // Recommendations
  const recommendations: Recommendation[] = [];
  if (duplicates.bytes > 0) recommendations.push({ id: "dups", title: `Remove ${plural(duplicates.count, "duplicate file")}`, detail: "The newest copy of each file is kept.", bytes: duplicates.bytes, target: "duplicates" });
  if (versions.bytes > 0) recommendations.push({ id: "versions", title: `Keep ${KEEP_VERSIONS} versions per file`, detail: `Removes ${plural(versions.count, "older version")}.`, bytes: versions.bytes, target: "versions" });
  if (trash.bytes > 0) recommendations.push({ id: "trash", title: `Empty Trash older than ${TRASH_PURGE_DAYS} days`, detail: `${plural(trash.count, "item")} in Trash for more than ${TRASH_PURGE_DAYS} days.`, bytes: trash.bytes, target: "trash" });
  const dupPhotos = dupFiles.filter((f) => categoryOf(f) === "photos");
  if (dupPhotos.length >= 5) recommendations.push({ id: "dup-photos", title: "Remove duplicate photos", detail: `${plural(dupPhotos.length, "photo")} uploaded more than once.`, bytes: sum(dupPhotos, (f) => fileBytes(index, f)), target: "duplicates" });
  const oldVideos = oldRows.filter((r) => r.category === "videos");
  if (oldVideos.length) recommendations.push({ id: "old-videos", title: "Archive old videos", detail: `${plural(oldVideos.length, "video")} haven't changed in over a year.`, bytes: sum(oldVideos, (r) => r.bytes), target: "old" });
  else if (old.bytes > 0) recommendations.push({ id: "old", title: "Review old files", detail: `${plural(old.count, "file")} haven't changed in over a year.`, bytes: old.bytes, target: "old" });
  if (staleLinks.length) recommendations.push({ id: "links", title: `Close ${plural(staleLinks.length, "open link")}`, detail: "No expiry and open for 30+ days.", bytes: 0, target: "links" });
  if (backup.status === "stale") recommendations.push({ id: "backup", title: "Check your PC backup", detail: `No new files in over ${STALE_BACKUP_DAYS} days.`, bytes: 0, target: "backup" });

  // Timeline
  const timeline: TimelineItem[] = [];
  {
    // New files uploaded within the same hour are one row, like "Uploaded 2 files".
    const firstUploads = uploaded.filter((e) => e.version === 1).sort((a, b) => b.modified - a.modified);
    let group: DriveEntry[] = [];
    const flush = () => {
      if (!group.length) return;
      timeline.push({
        at: group[0].modified,
        kind: "upload",
        title: group.length === 1 ? "Uploaded a file" : `Uploaded ${plural(group.length, "file")}`,
        detail: group.slice(0, 2).map((e) => e.name).join(", ") + (group.length > 2 ? ` and ${group.length - 2} more` : ""),
        bytes: sum(group, (e) => e.storedBytes),
      });
      group = [];
    };
    for (const e of firstUploads) {
      if (group.length && (group[0].modified - e.modified > 3_600_000 || startOfDay(group[0].modified) !== startOfDay(e.modified))) flush();
      group.push(e);
    }
    flush();
  }
  for (const e of uploaded.filter((x) => x.version > 1)) {
    timeline.push({ at: e.modified, kind: "version", title: "New version", detail: `${e.name} · version ${e.version}`, bytes: e.storedBytes });
  }
  for (const e of entries.filter((x) => x.trashed && x.trashedAt > 0 && within(x.trashedAt, range) && (x.kind === "folder" || index.isCurrent(x)))) {
    const inside = e.kind === "folder" ? index.descendantsOf(e.id).filter((d) => d.kind === "file" && index.isCurrent(d)).length : 0;
    timeline.push({ at: e.trashedAt, kind: "trash", title: "Moved to Trash", detail: e.kind === "folder" ? `${e.name} (folder, ${plural(inside, "file")})` : e.name });
  }
  for (const e of (journal?.events ?? []).filter((x) => within(x.t, range))) timeline.push(timelineOf(e));
  for (const a of found.filter((x) => x.rule !== "open-links" && within(x.at, range))) {
    timeline.push({ at: a.at, kind: "alert", title: a.title, detail: `${a.detail} · alert${alertsOff.has(a.rule) ? " off" : ""}` });
  }
  timeline.sort((a, b) => b.at - a.at);

  return {
    overview,
    score: scoreReport,
    byCategory,
    byFolder,
    largest,
    recent,
    history,
    growth,
    forecast,
    uploads,
    cleanup,
    recommendations,
    backup,
    links: linksReport,
    alerts,
    timeline: timeline.slice(0, 400),
    journalStarted: journal?.started ?? null,
  };
}

function timelineOf(e: JournalEvent): TimelineItem {
  switch (e.k) {
    case "deleted":
      return { at: e.t, kind: "deleted", title: "Permanently deleted", detail: `${plural(e.c ?? 0, "item")} deleted for good`, bytes: e.b };
    case "cleanup":
      return { at: e.t, kind: "cleanup", title: "Cleaned up", detail: `${e.n ?? "Cleanup"} · ${plural(e.c ?? 0, "item")}`, bytes: e.b };
    case "renamed":
      return { at: e.t, kind: "renamed", title: "Renamed or moved", detail: e.n ?? `${plural(e.c ?? 0, "more item")}` };
    case "restored":
      return { at: e.t, kind: "restored", title: "Restored from Trash", detail: e.n ?? `${plural(e.c ?? 0, "more item")}` };
    case "link-created":
      return { at: e.t, kind: "link", title: "Created a share link", detail: e.n ?? "A file" };
    case "link-removed":
      return { at: e.t, kind: "link", title: "Share link removed", detail: e.n ?? "A file" };
    case "link-expired":
      return { at: e.t, kind: "link", title: "Link expired", detail: e.n ?? "A file" };
  }
}

/** Storage history as a table: one row per month (or week / day), the value at its end. */
export function historyTable(report: AnalyticsReport, granularity: Granularity, range: Range): { t: number; used: number; estimated: boolean }[] {
  return growthSeries(report, granularity, range)
    .filter((p) => !p.forecast)
    .map((p) => ({ t: p.t, used: p.value, estimated: !!p.estimated }));
}

// --- Export ---

function csvCell(value: string | number): string {
  if (typeof value === "number") return String(value);
  // A file name like "=HYPERLINK(...)" must stay text in a spreadsheet, never a formula.
  const text = /^[=+\-@\t\r]/.test(value) ? `'${value}` : value;
  return /[",\n\r']/.test(text) ? `"${text.replace(/"/g, '""')}"` : text;
}

/** A spreadsheet-friendly report (Premium). Cells that look like formulas are neutralised. */
export function reportToCsv(report: AnalyticsReport, range: Range): string {
  const rows: (string | number)[][] = [];
  const iso = (t: number) => new Date(t).toISOString();
  rows.push(["Redvault storage report", `${iso(range.from)} to ${iso(range.to)}`]);
  rows.push([]);
  rows.push(["Section", "Name", "Bytes", "Count", "Detail"]);
  const o = report.overview;
  rows.push(["Overview", "Used", o.used, o.files, `${o.percent.toFixed(1)}% of ${o.limit}`]);
  rows.push(["Overview", "Available", o.available, "", ""]);
  rows.push(["Overview", "Score", report.score.value, "", report.score.label]);
  for (const c of report.byCategory) rows.push(["By type", c.label, c.bytes, c.files, ""]);
  for (const f of report.byFolder) rows.push(["By folder", f.label, f.bytes, f.files, ""]);
  for (const r of report.largest) rows.push(["Largest files", r.entry.name, r.bytes, 1, iso(r.entry.modified || 0)]);
  const c = report.cleanup;
  rows.push(["Cleanup", "Duplicates", c.duplicates.bytes, c.duplicates.count, "newest copy kept"]);
  rows.push(["Cleanup", "Previous versions", c.versions.bytes, c.versions.count, `latest ${KEEP_VERSIONS} kept`]);
  rows.push(["Cleanup", "Trash", c.trash.bytes, c.trash.count, `in Trash over ${TRASH_PURGE_DAYS} days`]);
  rows.push(["Cleanup", "Old files", c.old.bytes, c.old.count, `unchanged ${OLD_FILE_DAYS} days`]);
  rows.push(["Uploads", "In range", report.uploads.bytes, report.uploads.count, `${report.uploads.activeDays} active days`]);
  for (const b of report.uploads.buckets) rows.push([`Uploads by ${report.uploads.unit}`, iso(b.start), b.bytes, b.count, ""]);
  for (const p of report.history.filter((h) => within(h.day, { from: startOfDay(range.from), to: range.to }))) {
    rows.push(["History", iso(p.day), p.used, "", p.estimated ? "estimated" : "seen"]);
  }
  for (const l of report.links.rows) {
    rows.push(["Share links", l.name, l.bytes, "", `${l.status}; ${l.hasPassword ? "password" : "no password"}; ${l.expiresAt ? "expires " + iso(l.expiresAt) : "no expiry"}`]);
  }
  for (const a of report.alerts.items) rows.push(["Alerts", a.title, "", "", `${a.status}: ${a.detail}`]);
  for (const t of report.timeline) rows.push(["Activity", iso(t.at), t.bytes ?? "", "", `${t.title}: ${t.detail}`]);
  return rows.map((r) => r.map(csvCell).join(",")).join("\r\n") + "\r\n";
}
