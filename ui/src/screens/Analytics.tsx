// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { type ReactNode, useCallback, useEffect, useMemo, useRef, useState } from "react";
import {
  ActivityIcon,
  CalendarIcon,
  ChartIcon,
  CheckIcon,
  ChevronDownIcon,
  DatabaseIcon,
  DownloadIcon,
  GridIcon,
  InfoIcon,
  LinkIcon,
  LockIcon,
  MonitorIcon,
  SparklesIcon,
  UploadIcon,
} from "../components/icons24.js";
import { useDialogFocus } from "../components/useDialogFocus.js";
import {
  addDays,
  type AnalyticsJournal,
  type AnalyticsSettings,
  emptyJournal,
  recordCleanup,
  startOfDay,
  syncJournal,
  updateJournal,
  withSettings,
} from "../lib/analyticsJournal.js";
import { ApiError, type BackupLinkDto, deleteBackupItem, deleteBackupLink, listBackupItems, listBackupLinks, updateBackupMeta } from "../lib/api.js";
import { decodeDriveEntry, driveKey, type DriveEntry, trashedMeta } from "../lib/driveMeta.js";
import { toBase64 } from "../lib/e2eCrypto.js";
import { type AnalyticsReport, computeAnalytics, formatStorage, indexDrive, type Range, reportToCsv } from "../lib/storageAnalytics.js";
import { useAppState } from "../state/AppStateContext.js";
import type { AnalyticsPage } from "../state/reducer.js";
import { type SelfIdentity, type SubscriptionTier, tierAtLeast } from "../types.js";
import styles from "./Analytics.module.css";
import { ActivityPage, BackupPage, CleanupPage, LinksPage, OverviewPage, StoragePage, UploadsPage } from "./AnalyticsPages.js";

export type Preset = "7d" | "30d" | "90d" | "12m" | "custom";

export const PRESETS: { key: Exclude<Preset, "custom">; label: string; premium: boolean }[] = [
  { key: "7d", label: "Last 7 days", premium: false },
  { key: "30d", label: "Last 30 days", premium: false },
  { key: "90d", label: "Last 90 days", premium: true },
  { key: "12m", label: "Last 12 months", premium: true },
];

const PAGES: { key: AnalyticsPage; label: string; icon: ReactNode; subtitle: string }[] = [
  { key: "overview", label: "Overview", icon: <GridIcon size={18} />, subtitle: "Built from each file's size, type and dates. File contents stay encrypted and are never read for analytics." },
  { key: "storage", label: "Storage", icon: <DatabaseIcon size={18} />, subtitle: "History, growth and what's using your space." },
  { key: "uploads", label: "Uploads", icon: <UploadIcon size={18} />, subtitle: "How much you upload, how often, and when." },
  { key: "cleanup", label: "Cleanup", icon: <SparklesIcon size={18} />, subtitle: "Free up space. Nothing is deleted until you review it and confirm." },
  { key: "backup", label: "PC backup", icon: <MonitorIcon size={18} />, subtitle: "Health, size and growth of your PC backup." },
  { key: "links", label: "Share links", icon: <LinkIcon size={18} />, subtitle: "What your links expose, how long they've been open, and which ones to close." },
  { key: "activity", label: "Activity & alerts", icon: <ActivityIcon size={18} />, subtitle: "Everything that changed in your files, and alerts for unusual activity." },
];

export function retentionDaysFor(tier: SubscriptionTier): number {
  return tier === "premium" ? 365 : 30;
}

export function toInputDate(ms: number): string {
  const d = new Date(ms);
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}

function fromInputDate(value: string): number | null {
  const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(value);
  if (!m) return null;
  return new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3])).getTime();
}

export function rangeFor(preset: Preset, now: number, retentionDays: number, custom: { from: string; to: string }): Range {
  const today = startOfDay(now);
  const earliest = addDays(now, -retentionDays);
  switch (preset) {
    case "7d":
      return { from: addDays(now, -6), to: now };
    case "30d":
      return { from: addDays(now, -29), to: now };
    case "90d":
      return { from: Math.max(earliest, addDays(now, -89)), to: now };
    case "12m":
      return { from: Math.max(earliest, addDays(now, -364)), to: now };
    case "custom": {
      const from = Math.max(earliest, Math.min(fromInputDate(custom.from) ?? addDays(now, -6), today));
      const toDay = Math.min(today, Math.max(from, fromInputDate(custom.to) ?? today));
      return { from, to: toDay === today ? now : addDays(toDay, 1) - 1 };
    }
  }
}

export const fmt = {
  short: (t: number) => new Date(t).toLocaleDateString(undefined, { month: "short", day: "numeric" }),
  full: (t: number) => new Date(t).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }),
  month: (t: number) => new Date(t).toLocaleDateString(undefined, { month: "short", year: "numeric" }),
  time: (t: number) => new Date(t).toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" }),
  when: (t: number, now: number) => {
    const day = startOfDay(t);
    if (day === startOfDay(now)) return `Today ${fmt.time(t)}`;
    if (day === addDays(now, -1)) return `Yesterday ${fmt.time(t)}`;
    return fmt.short(t);
  },
  ago: (t: number, now: number) => {
    const minutes = Math.max(0, Math.round((now - t) / 60_000));
    if (minutes < 60) return minutes <= 1 ? "just now" : `${minutes} minutes ago`;
    const hours = Math.round(minutes / 60);
    if (hours < 24) return `${hours} hour${hours === 1 ? "" : "s"} ago`;
    const days = Math.round(hours / 24);
    return `${days} day${days === 1 ? "" : "s"} ago`;
  },
};

// --- Data ---

export interface Loaded {
  entries: DriveEntry[];
  links: BackupLinkDto[];
  usage: { used: number; limit: number };
  journal: AnalyticsJournal | null;
  journalError: string | null;
  now: number;
}

function useStorageData(identity: SelfIdentity) {
  const [data, setData] = useState<Loaded | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);
  const retentionDays = retentionDaysFor(identity.subscriptionTier);

  const load = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const [{ items, usage }, { links }] = await Promise.all([listBackupItems(), listBackupLinks()]);
      const key = driveKey(identity.privateKey);
      const entries = items.map((item) => decodeDriveEntry(item, key));
      const now = Date.now();
      const index = indexDrive(entries);
      const live = (e: DriveEntry) => !index.inTrash.has(e.id);
      let journal: AnalyticsJournal | null = null;
      let journalError: string | null = null;
      try {
        journal = await syncJournal(identity.privateKey, (prev) => {
          // Today's score goes into the journal too, for "up 4 this month".
          const today = computeAnalytics({ entries, links, usage, journal: prev, now, range: { from: addDays(now, -29), to: now }, retentionDays });
          const snapshot = { u: usage.used, f: index.current.filter(live).length, b: today.backup.bytes, t: today.cleanup.trash.allBytes, s: today.score.value };
          return updateJournal(prev, { entries, inTrash: index.inTrash, links, snapshot, now, retentionDays });
        });
      } catch (err) {
        journalError = err instanceof ApiError ? err.message : "History couldn't be saved this time.";
      }
      setData({ entries, links, usage: { used: usage.used, limit: usage.limit }, journal, journalError, now });
    } catch (err) {
      if (err instanceof ApiError && err.code === "backup_disabled") setError("Files aren't enabled on this server, so there's nothing to analyse yet.");
      else setError(err instanceof ApiError ? err.message : "Couldn't load your storage. Check your connection and try again.");
    } finally {
      setLoading(false);
    }
  }, [identity.privateKey, retentionDays]);

  useEffect(() => {
    void load();
  }, [load]);

  /** Saves a settings change with the journal (alert switches, dismissed alerts, keep-versions). */
  const saveSettings = useCallback(
    async (change: (s: AnalyticsSettings) => AnalyticsSettings) => {
      setData((d) => (d ? { ...d, journal: withSettings(d.journal ?? emptyJournal(d.now), change) } : d));
      try {
        const saved = await syncJournal(identity.privateKey, (prev) => withSettings(prev ?? emptyJournal(Date.now()), change));
        setData((d) => (d ? { ...d, journal: saved } : d));
      } catch {
        setData((d) => (d ? { ...d, journalError: "That setting couldn't be saved. Try again." } : d));
      }
    },
    [identity.privateKey]
  );

  return { data, error, loading, reload: load, retentionDays, saveSettings };
}

// --- Screen ---

export function Analytics() {
  const { state, dispatch } = useAppState();
  const identity = state.identity;
  if (!identity) return null;
  const seePlans = () => dispatch({ type: "OPEN_SETTINGS", section: "account" });
  if (!tierAtLeast(identity.subscriptionTier, "pro")) return <AnalyticsLocked onSeePlans={seePlans} />;
  const page = state.screen.key === "analytics" ? (state.screen.page ?? "overview") : "overview";
  return <AnalyticsShell identity={identity} page={page} onPage={(p) => dispatch({ type: "OPEN_ANALYTICS", page: p })} onSeePlans={seePlans} />;
}

function AnalyticsLocked({ onSeePlans }: { onSeePlans: () => void }) {
  return (
    <div className={styles.lockedPage}>
      <div className={styles.locked}>
        <span className={styles.lockedIcon}>
          <ChartIcon size={28} />
        </span>
        <h1 className={styles.pageTitle}>Storage analytics</h1>
        <p className={styles.subtitle}>
          See what's using your Files storage, what's growing, what you can clean up, and what you're sharing. Built from each file's size, type and dates on your
          device: file contents stay encrypted and are never read for analytics.
        </p>
        <ul className={styles.lockedList}>
          <li>
            <CheckIcon size={16} /> <strong>Pro:</strong> usage, growth and forecast, file types and folders, uploads, cleanup, PC backup, share links and alerts, 30 days
            of history
          </li>
          <li>
            <CheckIcon size={16} /> <strong>Premium:</strong> everything in Pro, 12 months of history, custom ranges up to 12 months, CSV and PDF reports
          </li>
        </ul>
        <button type="button" className={styles.primary} onClick={onSeePlans}>
          See plans
        </button>
      </div>
    </div>
  );
}

export type CleanupPlan = {
  title: string;
  rows: { label: string; count: string; rule: string; bytes: number }[];
  bytes: number;
  /** Deleted for good. */
  deleteIds: string[];
  /** Moved to the Trash (every version of each file). */
  trashIds: string[];
  linkIds: string[];
  warning: string;
  action: string;
  label: string;
};

export interface PageProps {
  report: AnalyticsReport;
  data: Loaded;
  range: Range;
  rangeLabel: string;
  premium: boolean;
  busy: boolean;
  go: (page: AnalyticsPage) => void;
  confirm: (plan: CleanupPlan) => void;
  saveSettings: (change: (s: AnalyticsSettings) => AnalyticsSettings) => void;
}

function AnalyticsShell({ identity, page, onPage, onSeePlans }: { identity: SelfIdentity; page: AnalyticsPage; onPage: (p: AnalyticsPage) => void; onSeePlans: () => void }) {
  const premium = identity.subscriptionTier === "premium";
  const { data, error, loading, reload, retentionDays, saveSettings } = useStorageData(identity);
  const [preset, setPreset] = useState<Preset>("30d");
  const [custom, setCustom] = useState(() => ({ from: toInputDate(addDays(Date.now(), -6)), to: toInputDate(Date.now()) }));
  const [plan, setPlan] = useState<CleanupPlan | null>(null);
  const [progress, setProgress] = useState<{ done: number; total: number } | null>(null);
  const [notice, setNotice] = useState<string | null>(null);

  const range = useMemo(() => rangeFor(preset, data?.now ?? Date.now(), retentionDays, custom), [preset, data?.now, retentionDays, custom]);
  const rangeLabel = preset === "custom" ? `${fmt.short(range.from)} – ${fmt.short(range.to)}` : PRESETS.find((p) => p.key === preset)!.label;
  const report = useMemo(() => (data ? computeAnalytics({ ...data, range, retentionDays }) : null), [data, range, retentionDays]);
  const meta = PAGES.find((p) => p.key === page)!;

  async function run(p: CleanupPlan) {
    setPlan(null);
    const total = p.deleteIds.length + p.trashIds.length + p.linkIds.length;
    let done = 0;
    let failed = 0;
    setProgress({ done, total });
    const removed: string[] = [];
    const removedLinks: string[] = [];
    const tick = () => setProgress({ done: ++done, total });
    // One at a time: the server rate-limits these, and a failure part-way leaves a clear result.
    for (const id of p.linkIds) {
      try {
        await deleteBackupLink(id);
        removedLinks.push(id);
      } catch (err) {
        if (err instanceof ApiError && err.status === 404) removedLinks.push(id);
        else failed += 1;
      }
      tick();
    }
    for (const id of p.deleteIds) {
      try {
        await deleteBackupItem(id);
        removed.push(id);
      } catch (err) {
        if (err instanceof ApiError && err.status === 404) removed.push(id);
        else failed += 1;
      }
      tick();
    }
    if (p.trashIds.length && data) {
      const key = driveKey(identity.privateKey);
      const byId = new Map(data.entries.map((e) => [e.id, e]));
      const now = Date.now();
      for (const id of p.trashIds) {
        const entry = byId.get(id);
        try {
          if (!entry) throw new Error("gone");
          const sealed = trashedMeta(entry, key, now);
          await updateBackupMeta(id, toBase64(sealed.ciphertext), toBase64(sealed.nonce));
        } catch {
          failed += 1;
        }
        tick();
      }
    }
    if (removed.length || removedLinks.length) {
      const now = Date.now();
      try {
        await syncJournal(identity.privateKey, (prev) =>
          recordCleanup(prev ?? emptyJournal(now), { label: p.label, itemIds: removed, linkIds: removedLinks, bytes: p.bytes, count: removed.length + removedLinks.length, now })
        );
      } catch {
        // The cleanup happened; only its note in the history is missing.
      }
    }
    setProgress(null);
    setNotice(failed ? `Done, but ${failed} item${failed === 1 ? "" : "s"} couldn't be changed. Try again in a minute.` : `${p.label}: done.`);
    await reload();
  }

  function exportCsv() {
    if (!report || !premium) return;
    const blob = new Blob([reportToCsv(report, range)], { type: "text/csv;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `storage-report-${toInputDate(range.from)}-to-${toInputDate(range.to)}.csv`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  const props: PageProps | null =
    report && data
      ? { report, data, range, rangeLabel, premium, busy: progress !== null, go: onPage, confirm: setPlan, saveSettings: (c) => void saveSettings(c) }
      : null;

  return (
    <div className={styles.shell}>
      <nav className={`${styles.sidebar} ${styles.noPrint}`} aria-label="Analytics">
        <div className={styles.sideHead}>
          <h1 className={styles.sideTitle}>Analytics</h1>
          <span className={styles.planPill}>{premium ? "Premium" : "Pro"}</span>
        </div>
        <div className={styles.sideNav}>
          {PAGES.map((p) => (
            <button key={p.key} type="button" className={`${styles.sideItem} ${page === p.key ? styles.sideItemOn : ""}`} aria-current={page === p.key ? "page" : undefined} onClick={() => onPage(p.key)}>
              {p.icon}
              <span>{p.label}</span>
            </button>
          ))}
        </div>
        <div className={styles.planCard}>
          <strong>{premium ? "Premium plan" : "Pro plan"}</strong>
          <span>
            {premium
              ? "12 months of history, custom ranges up to 12 months, CSV and PDF reports."
              : "30 days of history and custom ranges up to 30 days. CSV and PDF reports need Premium."}
          </span>
          {!premium && (
            <button type="button" className={styles.primary} onClick={onSeePlans}>
              Upgrade to Premium
            </button>
          )}
        </div>
      </nav>

      <main className={styles.main}>
        <header className={styles.header}>
          <div className={styles.headText}>
            <h2 className={styles.pageTitle}>{meta.label}</h2>
            <p className={styles.subtitle}>{meta.subtitle}</p>
          </div>
          <div className={`${styles.headActions} ${styles.noPrint}`}>
            <RangeMenu preset={preset} setPreset={setPreset} custom={custom} setCustom={setCustom} premium={premium} label={rangeLabel} retentionDays={retentionDays} />
            <ReportButton label="CSV" premium={premium} disabled={!report} onClick={exportCsv} />
            <ReportButton label="PDF" premium={premium} disabled={!report} onClick={() => window.print()} />
          </div>
        </header>

        {notice && (
          <p className={styles.notice} role="status">
            <CheckIcon size={16} /> {notice}
            <button type="button" className={styles.linkButton} onClick={() => setNotice(null)}>
              Dismiss
            </button>
          </p>
        )}
        {error && (
          <p className={styles.error} role="alert">
            {error}{" "}
            <button type="button" className={styles.linkButton} onClick={() => void reload()}>
              Try again
            </button>
          </p>
        )}
        {data?.journalError && (
          <p className={styles.warnNote}>
            <InfoIcon size={16} /> {data.journalError} Today's numbers are still accurate.
          </p>
        )}
        {report && report.overview.unreadable > 0 && (
          <p className={styles.warnNote}>
            <InfoIcon size={16} /> {report.overview.unreadable} item{report.overview.unreadable === 1 ? "" : "s"} couldn't be decrypted on this device and{" "}
            {report.overview.unreadable === 1 ? "is" : "are"} counted by size only.
          </p>
        )}
        {!props && !error && loading && <p className={styles.subtitle}>Decrypting your Files details on this device…</p>}
        {props && page === "overview" && <OverviewPage {...props} />}
        {props && page === "storage" && <StoragePage {...props} />}
        {props && page === "uploads" && <UploadsPage {...props} />}
        {props && page === "cleanup" && <CleanupPage {...props} />}
        {props && page === "backup" && <BackupPage {...props} />}
        {props && page === "links" && <LinksPage {...props} />}
        {props && page === "activity" && <ActivityPage {...props} />}
      </main>

      {plan && <ConfirmDialog plan={plan} onCancel={() => setPlan(null)} onConfirm={() => void run(plan)} />}
      {progress && (
        <div className={styles.backdrop}>
          <div className={styles.dialog} role="alertdialog" aria-live="polite" aria-label="Working">
            <h2 className={styles.dialogTitle}>Working…</h2>
            <span className={styles.progressTrack}>
              <span className={styles.progressFill} style={{ width: `${(progress.done / Math.max(1, progress.total)) * 100}%` }} />
            </span>
            <p className={styles.subtitle}>
              {progress.done} of {progress.total}
            </p>
          </div>
        </div>
      )}
    </div>
  );
}

function ReportButton({ label, premium, disabled, onClick }: { label: string; premium: boolean; disabled: boolean; onClick: () => void }) {
  return (
    <button
      type="button"
      className={`${styles.headButton} ${premium ? "" : styles.headButtonLocked}`}
      aria-disabled={!premium || disabled}
      title={premium ? "Download a report for this range" : "Reports are part of Premium"}
      onClick={premium && !disabled ? onClick : undefined}
    >
      <DownloadIcon size={16} /> {label}
      {!premium && <span className={styles.premiumTag}>Premium</span>}
    </button>
  );
}

function RangeMenu({
  preset,
  setPreset,
  custom,
  setCustom,
  premium,
  label,
  retentionDays,
}: {
  preset: Preset;
  setPreset: (p: Preset) => void;
  custom: { from: string; to: string };
  setCustom: (c: { from: string; to: string }) => void;
  premium: boolean;
  label: string;
  retentionDays: number;
}) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const onDown = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) setOpen(false);
    };
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setOpen(false);
    };
    document.addEventListener("mousedown", onDown);
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("mousedown", onDown);
      document.removeEventListener("keydown", onKey);
    };
  }, [open]);
  const today = toInputDate(Date.now());
  const earliest = toInputDate(addDays(Date.now(), -retentionDays));
  return (
    <div className={styles.rangeWrap} ref={ref}>
      <button type="button" className={styles.headButton} aria-haspopup="dialog" aria-expanded={open} onClick={() => setOpen(!open)}>
        <CalendarIcon size={16} /> {label} <ChevronDownIcon size={16} />
      </button>
      {open && (
        <div className={styles.rangeMenu} role="dialog" aria-label="Date range">
          {PRESETS.map((p) => {
            const locked = p.premium && !premium;
            return (
              <button
                key={p.key}
                type="button"
                className={`${styles.rangeItem} ${preset === p.key ? styles.rangeItemOn : ""}`}
                aria-disabled={locked}
                aria-pressed={preset === p.key}
                title={locked ? "Part of Premium" : undefined}
                onClick={() => {
                  if (locked) return;
                  setPreset(p.key);
                  setOpen(false);
                }}
              >
                <span className={styles.rangeCheck}>{preset === p.key && <CheckIcon size={16} />}</span>
                <span className={locked ? styles.rangeLocked : undefined}>{p.label}</span>
                {locked && (
                  <span className={styles.premiumTag}>
                    <LockIcon size={11} /> Premium
                  </span>
                )}
              </button>
            );
          })}
          <div className={styles.rangeCustom}>
            <strong>Custom range</strong>
            <div className={styles.rangeInputs}>
              <input
                type="date"
                aria-label="From"
                value={custom.from}
                min={earliest}
                max={custom.to}
                onChange={(e) => {
                  setCustom({ ...custom, from: e.target.value });
                  setPreset("custom");
                }}
              />
              <input
                type="date"
                aria-label="To"
                value={custom.to}
                min={custom.from}
                max={today}
                onChange={(e) => {
                  setCustom({ ...custom, to: e.target.value });
                  setPreset("custom");
                }}
              />
            </div>
            <span className={styles.rangeNote}>{premium ? "Premium: custom ranges up to 12 months." : "Pro: custom ranges up to 30 days. Premium goes back 12 months."}</span>
          </div>
        </div>
      )}
    </div>
  );
}

function ConfirmDialog({ plan, onCancel, onConfirm }: { plan: CleanupPlan; onCancel: () => void; onConfirm: () => void }) {
  const ref = useRef<HTMLDivElement>(null);
  const [understood, setUnderstood] = useState(false);
  useDialogFocus(ref, onCancel);
  return (
    <div className={styles.backdrop} onClick={onCancel}>
      <div ref={ref} className={styles.dialog} role="alertdialog" aria-modal="true" aria-labelledby="confirm-title" onClick={(e) => e.stopPropagation()}>
        <h2 id="confirm-title" className={styles.dialogTitle}>
          {plan.title}
        </h2>
        <ul className={styles.confirmRows}>
          {plan.rows.map((r) => (
            <li key={r.label}>
              <span>
                <strong>
                  {r.label} · {r.count}
                </strong>
                <span className={styles.muted}>{r.rule}</span>
              </span>
              <span className={styles.mono}>{r.bytes ? formatStorage(r.bytes) : ""}</span>
            </li>
          ))}
        </ul>
        <p className={styles.dangerNote}>{plan.warning}</p>
        <label className={styles.understand}>
          <input type="checkbox" checked={understood} onChange={(e) => setUnderstood(e.target.checked)} />
          I understand this can't be undone
        </label>
        <div className={styles.dialogActions}>
          <button type="button" className={styles.secondary} onClick={onCancel}>
            Cancel
          </button>
          <button type="button" className={understood ? styles.danger : styles.dangerOff} disabled={!understood} onClick={onConfirm}>
            {plan.action}
          </button>
        </div>
      </div>
    </div>
  );
}
