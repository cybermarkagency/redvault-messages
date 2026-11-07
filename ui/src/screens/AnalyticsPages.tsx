// The seven Analytics pages (desktop handoff: Overview, Storage, Uploads,
// Cleanup, PC backup, Share links, Activity & alerts). Everything shown is
// computed on this device (lib/storageAnalytics.ts).
import { type ReactNode, useState } from "react";
import { BarChart, Heatmap, HEAT_RAMP, LineChart, Meter, RankedBars, StackedBar } from "../components/AnalyticsCharts.js";
import {
  AlertTriangleIcon,
  ArrowUpRightIcon,
  CheckIcon,
  ChevronRightIcon,
  InfoIcon,
  LayersIcon,
  LinkIcon,
  LockIcon,
  PenIcon,
  RefreshIcon,
  SparklesIcon,
  TrashIcon,
  UploadIcon,
} from "../components/icons24.js";
import { addDays, type AlertRule, DAY_MS, startOfDay } from "../lib/analyticsJournal.js";
import {
  ALERT_RULES,
  CATEGORY_COLORS,
  CLEANUP_COLORS,
  type FileRow,
  formatStorage,
  type Granularity,
  growthSeries,
  historyTable,
  KEEP_VERSIONS,
  OLD_FILE_DAYS,
  type TimelineItem,
  TRASH_PURGE_DAYS,
} from "../lib/storageAnalytics.js";
import { type CleanupPlan, fmt, type PageProps } from "./Analytics.js";
import styles from "./Analytics.module.css";

const WEEKDAYS = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
const WEEKDAY_NAMES = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"];
const hh = (h: number) => `${String(h).padStart(2, "0")}:00`;
const count = (n: number, one: string, many = `${one}s`) => `${n.toLocaleString()} ${n === 1 ? one : many}`;

function Card({ title, aside, children, className = "" }: { title?: ReactNode; aside?: ReactNode; children: ReactNode; className?: string }) {
  return (
    <section className={`${styles.card} ${className}`}>
      {(title || aside) && (
        <div className={styles.cardHead}>
          {title && <h3 className={styles.cardTitle}>{title}</h3>}
          {aside}
        </div>
      )}
      {children}
    </section>
  );
}

function Stat({ label, value, unit, children }: { label: string; value: ReactNode; unit?: string; children?: ReactNode }) {
  return (
    <section className={styles.stat} aria-label={label}>
      <span className={styles.statLabel}>{label}</span>
      <span className={styles.statValue}>
        {value}
        {unit && <span className={styles.statUnit}> {unit}</span>}
      </span>
      {children}
    </section>
  );
}

function Change({ now, before, suffix }: { now: number; before: number; suffix: string }) {
  if (before === 0) return <span className={styles.statSub}>{now ? `No uploads ${suffix}` : ""}</span>;
  const pct = Math.round(((now - before) / before) * 100);
  return (
    <span className={pct >= 0 ? styles.good : styles.muted}>
      <ArrowUpRightIcon size={13} style={pct < 0 ? { transform: "rotate(90deg)" } : undefined} /> {Math.abs(pct)}% {pct >= 0 ? "" : "less "}vs {suffix}
    </span>
  );
}

function Segmented<T extends string>({ value, options, onChange, label }: { value: T; options: { key: T; label: string; locked?: boolean }[]; onChange: (v: T) => void; label: string }) {
  return (
    <div className={styles.segmented} role="group" aria-label={label}>
      {options.map((o) => (
        <button
          key={o.key}
          type="button"
          aria-pressed={value === o.key}
          aria-disabled={o.locked}
          title={o.locked ? "Part of Premium" : undefined}
          className={`${value === o.key ? styles.segOn : ""} ${o.locked ? styles.segLocked : ""}`}
          onClick={() => !o.locked && onChange(o.key)}
        >
          {o.label}
          {o.locked && <LockIcon size={11} />}
        </button>
      ))}
    </div>
  );
}

function Switch({ checked, onChange, label }: { checked: boolean; onChange: (v: boolean) => void; label: string }) {
  return (
    <button type="button" role="switch" aria-checked={checked} aria-label={label} className={`${styles.switch} ${checked ? styles.switchOn : ""}`} onClick={() => onChange(!checked)}>
      <span />
    </button>
  );
}

function duration(days: number): string {
  if (days < 1) return "less than a day";
  if (days < 60) return count(Math.round(days), "day");
  const months = Math.round(days / 30.44);
  if (months < 24) return count(months, "month");
  const years = Math.floor(months / 12);
  const rest = months % 12;
  return `${count(years, "year")}${rest ? ` ${count(rest, "month")}` : ""}`;
}

function granularityOptions(premium: boolean) {
  return [
    { key: "day" as const, label: "Day" },
    { key: "week" as const, label: "Week" },
    { key: "month" as const, label: "Month", locked: !premium },
  ];
}

function formatForGranularity(g: Granularity) {
  return g === "month" ? (t: number) => new Date(t).toLocaleDateString(undefined, { month: "short" }) : fmt.short;
}

function ForecastCaption({ props }: { props: PageProps }) {
  const { forecast, overview } = props.report;
  const basis = props.premium ? "the last 12 months" : "the last 30 days";
  if (forecast.daysLeft === null || forecast.fullOn === null) {
    return <p className={styles.caption}>{`Storage isn't growing at the moment. Based on ${basis}.`}</p>;
  }
  return (
    <p className={styles.caption}>
      <InfoIcon size={15} /> At this pace you'll fill {formatStorage(overview.limit)} in about <strong>{duration(forecast.daysLeft)}</strong> (around {fmt.month(forecast.fullOn)}). Based on {basis}.
    </p>
  );
}

// --- Overview ---

export function OverviewPage(props: PageProps) {
  const { report, premium, go, range, rangeLabel } = props;
  const [granularity, setGranularity] = useState<Granularity>("day");
  const { overview, score, uploads, cleanup } = report;
  const series = growthSeries(report, granularity, range, granularity === "week" && !premium ? 5 : undefined);
  const openAlerts = report.alerts.items.filter((a) => a.status === "open").slice(0, 2);
  const typeTotal = report.byCategory.reduce((n, c) => n + c.bytes, 0);
  return (
    <div className={styles.pageBody}>
      <div className={styles.stats4}>
        <Stat label="Storage used" value={formatStorage(overview.used)} unit={`of ${formatStorage(overview.limit)}`}>
          <Meter percent={overview.percent} label="Storage used" />
          <span className={styles.statSub}>
            {overview.percent.toFixed(0)}% used · {formatStorage(overview.available)} free{overview.percent >= 90 ? " · almost full" : ""}
          </span>
        </Stat>
        <Stat label="Storage score" value={score.value} unit="/ 100">
          <span className={score.value >= 75 ? styles.good : styles.warn}>
            {score.value >= 75 ? <CheckIcon size={14} /> : <AlertTriangleIcon size={14} />} {score.label}
            {score.change30 !== null && score.change30 !== 0 ? ` · ${score.change30 > 0 ? "up" : "down"} ${Math.abs(score.change30)} this month` : ""}
          </span>
          <span className={styles.statSub}>Free space, duplicates and backup health</span>
        </Stat>
        <Stat label={`Uploads · ${rangeLabel}`} value={uploads.count.toLocaleString()}>
          <span className={styles.statText}>{formatStorage(uploads.bytes)} uploaded</span>
          <Change now={uploads.count} before={uploads.prevCount} suffix="previous period" />
        </Stat>
        <Stat label="Recoverable space" value={formatStorage(cleanup.total)}>
          <span className={styles.statText}>Duplicates, old versions and Trash</span>
          <button type="button" className={styles.textLink} onClick={() => go("cleanup")}>
            Review cleanup <ChevronRightIcon size={14} />
          </button>
        </Stat>
      </div>

      <div className={styles.split}>
        <Card title="Storage growth" aside={<Segmented label="Growth by" value={granularity} options={granularityOptions(premium)} onChange={setGranularity} />}>
          <LineChart points={series.map((p) => ({ t: p.t, value: p.value, forecast: p.forecast }))} formatY={formatStorage} formatX={formatForGranularity(granularity)} label="Storage growth" height={210} />
          <ForecastCaption props={props} />
        </Card>
        <div className={styles.column}>
          <Card
            title="Alerts"
            aside={
              <button type="button" className={styles.textLink} onClick={() => go("activity")}>
                All alerts
              </button>
            }
          >
            {openAlerts.length === 0 ? (
              <p className={styles.muted}>
                <CheckIcon size={14} /> No open alerts.
              </p>
            ) : (
              <ul className={styles.miniAlerts}>
                {openAlerts.map((a) => (
                  <li key={a.id}>
                    {a.rule === "open-links" ? <LinkIcon size={17} /> : <AlertTriangleIcon size={17} />}
                    <span>
                      <strong>{a.title}</strong>
                      <span className={styles.muted}>{a.detail}</span>
                    </span>
                  </li>
                ))}
              </ul>
            )}
          </Card>
          <Card title="Recommendations">
            {report.recommendations.length === 0 ? (
              <p className={styles.muted}>Nothing to clean up right now.</p>
            ) : (
              <ul className={styles.recList}>
                {report.recommendations.slice(0, 3).map((r) => (
                  <li key={r.id}>
                    <button type="button" className={styles.recButton} onClick={() => go(r.target === "links" ? "links" : r.target === "backup" ? "backup" : "cleanup")}>
                      {r.title}
                    </button>
                    {r.bytes > 0 && <span className={styles.goodMono}>{formatStorage(r.bytes)}</span>}
                  </li>
                ))}
              </ul>
            )}
          </Card>
        </div>
      </div>

      <div className={styles.halves}>
        <Card
          title="By file type"
          aside={
            <button type="button" className={styles.textLink} onClick={() => go("storage")}>
              Details
            </button>
          }
        >
          <StackedBar segments={report.byCategory.map((c) => ({ key: c.key, value: c.bytes, color: c.color!, label: `${c.label} ${formatStorage(c.bytes)}` }))} label="Storage by file type" />
          <ul className={styles.legendRow}>
            {report.byCategory.map((c) => (
              <li key={c.key}>
                <span className={styles.legendKey}>
                  <span className={styles.swatch} style={{ background: c.color }} /> {c.label}
                </span>
                <span className={styles.mono}>{formatStorage(c.bytes)}</span>
              </li>
            ))}
            {typeTotal === 0 && <li className={styles.muted}>No files yet.</li>}
          </ul>
        </Card>
        <Card title="Top folders">
          <RankedBars rows={report.byFolder.slice(0, 5).map((f) => ({ key: f.key, label: f.label, value: f.bytes }))} formatValue={formatStorage} label="Top folders" />
        </Card>
      </div>
    </div>
  );
}

// --- Storage ---

function FileTable({ rows, now, showFolder = true }: { rows: FileRow[]; now: number; showFolder?: boolean }) {
  if (rows.length === 0) return <p className={styles.muted}>Nothing here for this period.</p>;
  return (
    <table className={styles.fileTable}>
      <tbody>
        {rows.map((r) => (
          <tr key={r.entry.id}>
            <td className={styles.fileCell}>
              <span className={styles.fileName}>{r.entry.name}</span>
              {r.entry.locked && (
                <span className={styles.lockTag}>
                  <LockIcon size={11} /> Locked
                </span>
              )}
              {r.entry.version > 1 && <span className={styles.muted}> v{r.entry.version}</span>}
            </td>
            {showFolder && <td className={styles.muted}>{r.folder}</td>}
            <td className={styles.mono}>{formatStorage(showFolder ? r.bytes : r.entry.storedBytes)}</td>
            <td className={styles.dateCell}>{fmt.when(r.entry.modified, now)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function StoragePage(props: PageProps) {
  const { report, premium, range, data } = props;
  const [granularity, setGranularity] = useState<Granularity>(premium && range.to - range.from > 100 * DAY_MS ? "month" : "day");
  const [table, setTable] = useState(false);
  const series = growthSeries(report, granularity, range, granularity === "week" && !premium ? 5 : undefined);
  const rows = historyTable(report, granularity, range);
  const label = granularity === "month" ? fmt.month : fmt.short;
  return (
    <div className={styles.pageBody}>
      <Card
        title="Storage history"
        aside={
          <div className={styles.cardTools}>
            {report.forecast.fullOn && <span className={styles.muted}>Full around {fmt.month(report.forecast.fullOn)} at this pace</span>}
            <Segmented label="History by" value={granularity} options={granularityOptions(premium)} onChange={setGranularity} />
            <button type="button" className={styles.segButton} aria-pressed={table} onClick={() => setTable(!table)}>
              {table ? "Chart" : "Table"}
            </button>
          </div>
        }
      >
        {table ? (
          <table className={styles.historyGrid} aria-label="Storage history">
            <tbody>
              <tr>
                {rows.slice(-36).map((r) => (
                  <td key={r.t}>
                    <span>{label(r.t)}</span>
                    <span className={styles.mono}>
                      {formatStorage(r.used)}
                      {r.estimated ? "*" : ""}
                    </span>
                  </td>
                ))}
              </tr>
            </tbody>
          </table>
        ) : (
          <LineChart points={series.map((p) => ({ t: p.t, value: p.value, forecast: p.forecast }))} formatY={formatStorage} formatX={formatForGranularity(granularity)} label="Storage history" height={180} />
        )}
        <p className={styles.footnote}>
          {report.journalStarted
            ? `Recorded each time you open Analytics (since ${fmt.full(report.journalStarted)}); days in between are estimated from upload dates${table ? " (marked *)" : ""}.`
            : "Estimated from upload dates."}{" "}
          {premium ? "Kept for 12 months." : "Pro keeps 30 days; Premium keeps 12 months."}
        </p>
      </Card>
      <div className={styles.halves}>
        <Card title="By file type">
          <RankedBars
            swatches
            rows={report.byCategory.map((c) => ({ key: c.key, label: c.label, value: c.bytes, color: c.color, count: count(c.files, "file") }))}
            formatValue={formatStorage}
            label="Storage by file type"
          />
        </Card>
        <Card title="By folder">
          <RankedBars rows={report.byFolder.slice(0, 6).map((f) => ({ key: f.key, label: f.label, value: f.bytes }))} formatValue={formatStorage} label="Storage by folder" />
          <p className={styles.footnote}>Top-level folders, including everything inside and previous versions.</p>
        </Card>
      </div>
      <div className={styles.split}>
        <Card title="Largest files">
          <FileTable rows={report.largest.slice(0, 6)} now={data.now} />
        </Card>
        <Card title="Recently added">
          <FileTable rows={report.recent.slice(0, 6)} now={data.now} showFolder={false} />
        </Card>
      </div>
    </div>
  );
}

// --- Uploads ---

export function UploadsPage(props: PageProps) {
  const { report } = props;
  const u = report.uploads;
  const [measure, setMeasure] = useState<"files" | "data">("files");
  const prevLabel = `previous ${u.rangeDays} days`;
  const unitLabel = u.unit === "day" ? fmt.short : u.unit === "week" ? (t: number) => `Wk ${fmt.short(t)}` : (t: number) => new Date(t).toLocaleDateString(undefined, { month: "short" });
  const value = (b: { count: number; bytes: number }) => (measure === "files" ? b.count : b.bytes);
  const format = measure === "files" ? (v: number) => `${Math.round(v)}` : formatStorage;
  return (
    <div className={styles.pageBody}>
      <div className={styles.stats4}>
        <Stat label="Files uploaded" value={u.count.toLocaleString()}>
          <Change now={u.count} before={u.prevCount} suffix={prevLabel} />
        </Stat>
        <Stat label="Data uploaded" value={formatStorage(u.bytes)}>
          <Change now={u.bytes} before={u.prevBytes} suffix={prevLabel} />
        </Stat>
        <Stat label="Frequency" value={u.perDay < 10 ? u.perDay.toFixed(1) : Math.round(u.perDay).toLocaleString()} unit="a day">
          <span className={styles.statText}>
            Uploads on {u.activeDays} of {u.rangeDays} days
          </span>
        </Stat>
        <Stat label="Busiest time" value={u.busiest ? `${WEEKDAYS[u.busiest.weekday]} ${hh(u.busiest.hour)}` : "—"}>
          <span className={styles.statText}>{u.quietest ? `Quietest: ${WEEKDAY_NAMES[u.quietest.weekday]} ${hh(u.quietest.fromHour)}–${hh(u.quietest.toHour % 24)}` : "No uploads in this period"}</span>
        </Stat>
      </div>
      <Card
        title={u.unit === "day" ? "Daily uploads" : u.unit === "week" ? "Weekly uploads" : "Monthly uploads"}
        aside={
          <div className={styles.cardTools}>
            <span className={styles.muted}>Dashed line: {prevLabel} average</span>
            <Segmented
              label="Measure"
              value={measure}
              options={[
                { key: "files", label: "Files" },
                { key: "data", label: "Data" },
              ]}
              onChange={setMeasure}
            />
          </div>
        }
      >
        <BarChart
          data={u.buckets.map((b) => ({ key: b.start, label: unitLabel(b.start), value: value(b), detail: `${unitLabel(b.start)} · ${count(b.count, "file")} · ${formatStorage(b.bytes)}` }))}
          formatValue={format}
          label={measure === "files" ? "Files uploaded" : "Data uploaded"}
          tickEvery={Math.max(1, Math.ceil(u.buckets.length / 5))}
          reference={value(u.prevAverage)}
          height={190}
        />
      </Card>
      <Card title="Peak hours and days" aside={<span className={styles.muted}>Uploads per hour, {props.rangeLabel.toLowerCase()}</span>}>
        <Heatmap rows={u.heat} rowLabels={WEEKDAYS} label="Uploads by weekday and hour" formatCell={(r, c, v) => `${WEEKDAY_NAMES[r]} ${hh(c)} · ${count(v, "upload")}`} />
        <div className={styles.heatLegend}>
          <span>Fewer</span>
          {HEAT_RAMP.slice(1).map((c) => (
            <span key={c} className={styles.heatKey} style={{ background: c }} />
          ))}
          <span>More</span>
          {u.busiest && (
            <span className={styles.muted}>
              Peak: {WEEKDAY_NAMES[u.busiest.weekday]}s {hh(u.busiest.hour)}–{hh((u.busiest.hour + 1) % 24)}
              {u.quietest ? ` · Quietest: ${WEEKDAY_NAMES[u.quietest.weekday]}s ${hh(u.quietest.fromHour)}–${hh(u.quietest.toHour % 24)}` : ""}
            </span>
          )}
        </div>
      </Card>
    </div>
  );
}

// --- Cleanup ---

type CleanupKey = "duplicates" | "versions" | "trash" | "old";

export function CleanupPage(props: PageProps) {
  const { report, busy, confirm } = props;
  const c = report.cleanup;
  const [selected, setSelected] = useState<Set<CleanupKey>>(new Set(["duplicates", "versions", "trash"]));
  const [open, setOpen] = useState<CleanupKey | null>(null);
  const cats: { key: CleanupKey; title: string; unit: [string, string]; rule: string; count: number; bytes: number; color: string; items: string[] }[] = [
    {
      key: "duplicates",
      title: "Duplicates",
      unit: ["file", "files"],
      rule: "The newest copy of each file is kept.",
      count: c.duplicates.count,
      bytes: c.duplicates.bytes,
      color: CLEANUP_COLORS.duplicates,
      items: c.duplicates.groups.slice(0, 8).map((g) => `${g.name} · ${g.extras.length + 1} copies`),
    },
    {
      key: "versions",
      title: "Previous versions",
      unit: ["version", "versions"],
      rule: `The latest ${KEEP_VERSIONS} versions of each file are kept.`,
      count: c.versions.count,
      bytes: c.versions.bytes,
      color: CLEANUP_COLORS.versions,
      items: [],
    },
    {
      key: "trash",
      title: "Trash",
      unit: ["item", "items"],
      rule: `Only items in Trash for more than ${TRASH_PURGE_DAYS} days.`,
      count: c.trash.count,
      bytes: c.trash.bytes,
      color: CLEANUP_COLORS.trash,
      items: c.trash.allItems > c.trash.count ? [`${count(c.trash.allItems - c.trash.count, "newer item")} stay in the Trash for now`] : [],
    },
    {
      key: "old",
      title: "Old files",
      unit: ["file", "files"],
      rule: `Not changed in ${OLD_FILE_DAYS / 30.4 > 11 ? "12 months" : `${OLD_FILE_DAYS} days`}. Moved to Trash, not deleted.`,
      count: c.old.count,
      bytes: c.old.bytes,
      color: CLEANUP_COLORS.old,
      items: c.old.files.slice(0, 8).map((r) => `${r.entry.name} · ${formatStorage(r.bytes)} · ${fmt.full(r.entry.modified)}`),
    },
  ];
  const chosen = cats.filter((x) => selected.has(x.key) && x.count > 0);
  const freed = chosen.reduce((n, x) => n + x.bytes, 0);

  function review() {
    const deleteIds = [
      ...(selected.has("duplicates") ? c.duplicates.itemIds : []),
      ...(selected.has("versions") ? c.versions.itemIds : []),
      ...(selected.has("trash") ? c.trash.itemIds : []),
    ];
    const hasDelete = chosen.some((x) => x.key !== "old");
    confirm({
      title: hasDelete ? `Free up ${formatStorage(freed)}?` : `Move ${count(c.old.count, "old file")} to Trash?`,
      rows: chosen.map((x) => ({ label: x.title, count: count(x.count, x.unit[0], x.unit[1]), rule: x.rule, bytes: x.bytes })),
      bytes: freed,
      deleteIds,
      trashIds: selected.has("old") ? c.old.itemIds : [],
      linkIds: [],
      warning: selected.has("old")
        ? "Duplicates, old versions and Trash items are deleted permanently and can't be recovered. Old files go to Trash first."
        : "Duplicates, old versions and Trash items are deleted permanently and can't be recovered.",
      action: hasDelete ? `Delete ${formatStorage(freed)}` : "Move to Trash",
      label: "Cleaned up storage",
    });
  }

  return (
    <div className={styles.pageBody}>
      <Card>
        <div className={styles.recoverHead}>
          <span className={styles.muted}>Recoverable space</span>
          <span className={styles.bigValue}>{formatStorage(c.total)}</span>
          <span className={styles.muted}>{c.percentOfUsed.toFixed(0)}% of storage used</span>
        </div>
        <StackedBar segments={cats.map((x) => ({ key: x.key, value: x.bytes, color: x.color, label: `${x.title} ${formatStorage(x.bytes)}` }))} label="Recoverable space by source" />
        <ul className={styles.legendInline}>
          {cats.map((x) => (
            <li key={x.key}>
              <span className={styles.swatch} style={{ background: x.color }} /> {x.title} <span className={styles.mono}>{formatStorage(x.bytes)}</span>
            </li>
          ))}
        </ul>
      </Card>
      <div className={styles.split}>
        <div className={styles.column}>
          {cats.map((x) => (
            <div key={x.key} className={`${styles.cleanRow} ${selected.has(x.key) && x.count > 0 ? styles.cleanRowOn : ""}`}>
              <div className={styles.cleanMain}>
                <input
                  type="checkbox"
                  className={styles.check}
                  aria-label={x.title}
                  checked={selected.has(x.key) && x.count > 0}
                  disabled={x.count === 0}
                  onChange={(e) => {
                    const next = new Set(selected);
                    if (e.target.checked) next.add(x.key);
                    else next.delete(x.key);
                    setSelected(next);
                  }}
                />
                <span className={styles.cleanBar} style={{ background: x.color }} aria-hidden="true" />
                <span className={styles.cleanText}>
                  <span>
                    <strong>{x.title}</strong>
                    <span className={styles.muted}> · {count(x.count, x.unit[0], x.unit[1])}</span>
                  </span>
                  <span className={styles.muted}>{x.rule}</span>
                </span>
                <span className={styles.mono}>{formatStorage(x.bytes)}</span>
                <button type="button" className={styles.smallButton} aria-expanded={open === x.key} disabled={x.count === 0} onClick={() => setOpen(open === x.key ? null : x.key)}>
                  Review items
                </button>
              </div>
              {open === x.key && (
                <ul className={styles.reviewList}>
                  {x.items.length ? x.items.map((i) => <li key={i}>{i}</li>) : <li>{count(x.count, x.unit[0], x.unit[1])} across your files.</li>}
                  {x.key === "old" && c.old.count > 8 && <li>and {c.old.count - 8} more</li>}
                </ul>
              )}
            </div>
          ))}
          <p className={styles.lockNote}>
            <LockIcon size={15} /> Files locked with a key are compared by name and size only. Their contents are never opened for cleanup.
          </p>
        </div>
        <Card title="Recommendations">
          {report.recommendations.length === 0 ? (
            <p className={styles.muted}>Nothing to clean up right now.</p>
          ) : (
            <ul className={styles.recDetailed}>
              {report.recommendations.map((r) => (
                <li key={r.id}>
                  <span className={styles.recTop}>
                    <strong>{r.title}</strong>
                    {r.bytes > 0 && <span className={styles.goodMono}>{formatStorage(r.bytes)}</span>}
                  </span>
                  <span className={styles.muted}>{r.detail}</span>
                </li>
              ))}
            </ul>
          )}
        </Card>
      </div>
      <div className={styles.actionBar}>
        <span>
          <strong>{count(chosen.length, "category", "categories")} selected</strong> · <span className={styles.goodMono}>{formatStorage(freed)}</span> can be freed
        </span>
        <button type="button" className={styles.primary} disabled={busy || chosen.length === 0} onClick={review}>
          Review and clean up…
        </button>
      </div>
    </div>
  );
}

// --- PC backup ---

const KEEP_OPTIONS: { days: number | null; label: string }[] = [
  { days: 30, label: "30 days" },
  { days: 90, label: "90 days" },
  { days: 365, label: "1 year" },
  { days: null, label: "Forever" },
];

export function BackupPage(props: PageProps) {
  const { report, data, busy, confirm, saveSettings, rangeLabel } = props;
  const b = report.backup;
  const keepDays = data.journal?.settings?.keepBackupDays === undefined ? 365 : data.journal.settings.keepBackupDays;
  const removable = keepDays === null ? [] : b.versionAges.filter((v) => v.ageDays > keepDays);
  const maxAge = Math.max(1, ...b.ages.map((a) => a.bytes));

  if (!b.configured) {
    return (
      <div className={styles.pageBody}>
        <Card>
          <p className={styles.subtitle}>PC backup isn't set up. Turn it on in the desktop app's Files page to keep folders on your computer backed up, end-to-end encrypted.</p>
        </Card>
      </div>
    );
  }
  const status = {
    healthy: { title: "Healthy", cls: styles.bannerGood },
    stale: { title: "No new files lately", cls: styles.bannerWarn },
    empty: { title: "Set up, but nothing backed up yet", cls: styles.bannerWarn },
    none: { title: "Not set up", cls: styles.bannerWarn },
  }[b.status];
  return (
    <div className={styles.pageBody}>
      <div className={`${styles.banner} ${status.cls}`}>
        <span className={styles.bannerIcon}>{b.status === "healthy" ? <CheckIcon size={20} /> : <AlertTriangleIcon size={20} />}</span>
        <span>
          <strong>{status.title}</strong>
          <span>
            {b.lastBackupAt ? `Last file backed up ${fmt.ago(b.lastBackupAt, data.now)}` : "No files yet"} · {count(b.files, "file")} protected
          </span>
        </span>
        <span className={styles.bannerNote}>Backups run from the desktop app</span>
      </div>
      <div className={styles.stats4}>
        <Stat label="Backup size" value={formatStorage(b.bytes)}>
          <span className={styles.statText}>{count(b.files, "file")} protected</span>
        </Stat>
        <Stat label={`Growth · ${rangeLabel}`} value={`+${formatStorage(b.addedInRange)}`}>
          <span className={styles.statText}>About {formatStorage(Math.round(b.perDay))} a day</span>
        </Stat>
        <Stat label="Old versions" value={formatStorage(b.oldVersions.bytes)}>
          <span className={styles.statText}>{count(b.oldVersions.count, "version")} kept</span>
        </Stat>
        <Stat label="Folders backed up" value={b.folders.length || "—"}>
          <span className={styles.statText}>{b.folders.slice(0, 3).join(", ") || "Files at the top of the backup"}</span>
        </Stat>
      </div>
      <div className={styles.split}>
        <Card title="Backup size over time">
          <LineChart points={b.series.map((p) => ({ t: p.t, value: p.value }))} formatY={formatStorage} formatX={fmt.short} label="Backup size over time" height={190} />
          <p className={styles.footnote}>From the upload dates of files still in the backup.</p>
        </Card>
        <Card title="Health checks">
          <ul className={styles.checks}>
            <li>
              {b.lastBackupAt && b.status === "healthy" ? <CheckIcon size={17} className={styles.good} /> : <AlertTriangleIcon size={17} className={styles.warn} />}
              <span>
                <strong>Last file backed up</strong>
                <span className={styles.muted}>{b.lastBackupAt ? fmt.when(b.lastBackupAt, data.now) : "Nothing yet"}</span>
              </span>
            </li>
            <li>
              <CheckIcon size={17} className={styles.good} />
              <span>
                <strong>Encrypted and hashed</strong>
                <span className={styles.muted}>Every file, on the PC, before upload</span>
              </span>
            </li>
            <li>
              {b.folders.length ? <CheckIcon size={17} className={styles.good} /> : <InfoIcon size={17} />}
              <span>
                <strong>Folders backed up</strong>
                <span className={styles.muted}>{b.folders.join(", ") || "None yet"}</span>
              </span>
            </li>
            <li>
              <InfoIcon size={17} />
              <span>
                <strong>Skipped files and restore tests</strong>
                <span className={styles.muted}>Shown in the desktop app that runs the backup</span>
              </span>
            </li>
          </ul>
        </Card>
      </div>
      <Card
        title="Old versions by age"
        aside={
          <div className={styles.cardTools}>
            <label className={styles.muted}>
              Keep versions for{" "}
              <select
                className={styles.select}
                value={keepDays === null ? "forever" : String(keepDays)}
                onChange={(e) => saveSettings((s) => ({ ...s, keepBackupDays: e.target.value === "forever" ? null : Number(e.target.value) }))}
              >
                {KEEP_OPTIONS.map((o) => (
                  <option key={o.label} value={o.days === null ? "forever" : String(o.days)}>
                    {o.label}
                  </option>
                ))}
              </select>
            </label>
            <button
              type="button"
              className={styles.textLink}
              disabled={busy || removable.length === 0}
              onClick={() =>
                confirm({
                  title: `Remove ${count(removable.length, "old version")}?`,
                  rows: [{ label: "Backup versions", count: count(removable.length, "version"), rule: `Older than ${KEEP_OPTIONS.find((o) => o.days === keepDays)?.label}. The current version of each file stays.`, bytes: removable.reduce((n, v) => n + v.bytes, 0) }],
                  bytes: removable.reduce((n, v) => n + v.bytes, 0),
                  deleteIds: removable.map((v) => v.id),
                  trashIds: [],
                  linkIds: [],
                  warning: "Old backup versions are deleted permanently and can't be recovered.",
                  action: `Delete ${formatStorage(removable.reduce((n, v) => n + v.bytes, 0))}`,
                  label: "Removed old backup versions",
                })
              }
            >
              Clean up old versions
            </button>
          </div>
        }
      >
        <div className={styles.ages}>
          {b.ages.map((a) => (
            <div key={a.label}>
              <span className={styles.muted}>{a.label}</span>
              <span className={styles.ageTrack}>
                <span style={{ width: `${(a.bytes / maxAge) * 100}%` }} />
              </span>
              <span>
                <span className={styles.mono}>{formatStorage(a.bytes)}</span>
                <span className={styles.muted}> · {count(a.count, "version")}</span>
              </span>
            </div>
          ))}
        </div>
      </Card>
    </div>
  );
}

// --- Share links ---

export function LinksPage(props: PageProps) {
  const { report, busy, confirm, data } = props;
  const l = report.links;
  const [filter, setFilter] = useState<"all" | "active" | "expired">("all");
  const rows = l.rows.filter((r) => filter === "all" || (filter === "expired" ? r.status === "expired" : r.status !== "expired"));
  const maxShared = Math.max(1, ...l.mostShared.map((m) => m.links));
  const revoke = (id: string, name: string) =>
    confirm({
      title: "Revoke this link?",
      rows: [{ label: name, count: "1 link", rule: "Anyone with the link loses access right away.", bytes: 0 }],
      bytes: 0,
      deleteIds: [],
      trashIds: [],
      linkIds: [id],
      warning: "A revoked link stops working for good. The file itself stays in your Files.",
      action: "Revoke link",
      label: "Revoked a share link",
    });
  return (
    <div className={styles.pageBody}>
      <div className={styles.stats4}>
        <Stat label="Active links" value={l.active}>
          <span className={styles.statText}>{count(l.withExpiry, "has", "have")} an expiry date</span>
        </Stat>
        <Stat label="Expired" value={l.expired}>
          <span className={styles.statText}>Closed automatically</span>
        </Stat>
        <Stat label="Data exposed by active links" value={formatStorage(l.exposedBytes)}>
          <span className={styles.statText}>Total size anyone with a link can open</span>
        </Stat>
        <Stat label="Open with no expiry" value={l.openNoExpiry}>
          {l.openNoExpiry > 0 ? (
            <span className={styles.warn}>
              <AlertTriangleIcon size={13} />{" "}
              {l.rows.some((r) => r.status === "no-expiry" && r.created === null) ? "Open since before analytics started" : `Oldest open ${count(l.oldestOpenDays ?? 0, "day")}`}
            </span>
          ) : (
            <span className={styles.statText}>Every link expires</span>
          )}
        </Stat>
      </div>
      <div className={styles.split}>
        <Card
          title="Links"
          aside={
            <Segmented
              label="Show"
              value={filter}
              options={[
                { key: "all", label: "All" },
                { key: "active", label: "Active" },
                { key: "expired", label: "Expired" },
              ]}
              onChange={setFilter}
            />
          }
        >
          {rows.length === 0 ? (
            <p className={styles.muted}>No links here. Links you create in the desktop app show up here.</p>
          ) : (
            <div className={styles.tableWrap}>
              <table className={styles.table}>
                <thead>
                  <tr>
                    <th scope="col">File</th>
                    <th scope="col">Created</th>
                    <th scope="col">Expires</th>
                    <th scope="col">Password</th>
                    <th scope="col">Exposes</th>
                    <th scope="col">Status</th>
                    <th scope="col">
                      <span className={styles.srOnly}>Actions</span>
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {rows.map((r) => (
                    <tr key={r.id}>
                      <td className={styles.nameCell}>
                        <span className={styles.fileName}>{r.name}</span>
                      </td>
                      <td>{r.created ? fmt.short(r.created) : "Before analytics"}</td>
                      <td className={r.status === "no-expiry" ? styles.warn : undefined}>{r.expiresAt ? fmt.short(r.expiresAt) : "No expiry"}</td>
                      <td>{r.hasPassword ? "Password" : "None"}</td>
                      <td className={styles.mono}>{formatStorage(r.bytes)}</td>
                      <td>
                        <span className={r.status === "active" ? styles.tagGood : r.status === "no-expiry" ? styles.tagWarn : styles.tagMuted}>
                          {r.status === "active" ? "Active" : r.status === "no-expiry" ? "No expiry" : "Expired"}
                        </span>
                      </td>
                      <td>
                        {r.status !== "expired" && (
                          <button type="button" className={styles.revoke} disabled={busy} onClick={() => revoke(r.id, r.name)}>
                            Revoke
                          </button>
                        )}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </Card>
        <Card title="Most shared files">
          {l.mostShared.length === 0 ? (
            <p className={styles.muted}>No active links.</p>
          ) : (
            <ul className={styles.shared}>
              {l.mostShared.map((m) => (
                <li key={m.name}>
                  <span className={styles.sharedTop}>
                    <span className={styles.fileName}>{m.name}</span>
                    <span className={styles.mono}>{count(m.links, "link")}</span>
                  </span>
                  <span className={styles.ageTrack}>
                    <span style={{ width: `${(m.links / maxShared) * 100}%` }} />
                  </span>
                </li>
              ))}
            </ul>
          )}
          {l.directShares.files > 0 && (
            <p className={styles.footnote}>
              Also shared directly: {count(l.directShares.files, "file")} with {count(l.directShares.people, "person", "people")}.
            </p>
          )}
          <p className={styles.footnote}>
            <InfoIcon size={13} /> Link contents stay end-to-end encrypted. Opens aren't counted: opening a link never tells the server who opened it. Links made before{" "}
            {report.journalStarted ? fmt.full(report.journalStarted) : "today"} show as "Before analytics".
          </p>
          <span className={styles.srOnly}>{data.now}</span>
        </Card>
      </div>
    </div>
  );
}

// --- Activity & alerts ---

const TIMELINE_FILTERS: { key: string; label: string; kinds: TimelineItem["kind"][] }[] = [
  { key: "all", label: "All", kinds: [] },
  { key: "uploads", label: "Uploads", kinds: ["upload"] },
  { key: "versions", label: "Versions", kinds: ["version"] },
  { key: "trash", label: "Trash", kinds: ["trash"] },
  { key: "deletions", label: "Deletions", kinds: ["deleted", "cleanup"] },
  { key: "renames", label: "Renames & moves", kinds: ["renamed"] },
  { key: "restores", label: "Restores", kinds: ["restored"] },
  { key: "links", label: "Links", kinds: ["link"] },
];

const KIND_ICON: Record<TimelineItem["kind"], ReactNode> = {
  upload: <UploadIcon size={16} />,
  version: <LayersIcon size={16} />,
  trash: <TrashIcon size={16} />,
  deleted: <TrashIcon size={16} />,
  cleanup: <SparklesIcon size={16} />,
  renamed: <PenIcon size={16} />,
  restored: <RefreshIcon size={16} />,
  link: <LinkIcon size={16} />,
  alert: <AlertTriangleIcon size={16} />,
};

export function ActivityPage(props: PageProps) {
  const { report, data, saveSettings } = props;
  const [filter, setFilter] = useState("all");
  const kinds = TIMELINE_FILTERS.find((f) => f.key === filter)!.kinds;
  const items = report.timeline.filter((t) => filter === "all" || kinds.includes(t.kind)).slice(0, 150);
  const groups: { label: string; items: TimelineItem[] }[] = [];
  for (const t of items) {
    const day = startOfDay(t.at);
    const label = day === startOfDay(data.now) ? "Today" : day === addDays(data.now, -1) ? "Yesterday" : fmt.short(t.at);
    const last = groups[groups.length - 1];
    if (last && last.label === label) last.items.push(t);
    else groups.push({ label, items: [t] });
  }
  const setRule = (rule: AlertRule, on: boolean) =>
    saveSettings((s) => {
      const off = new Set(s.alertsOff ?? []);
      if (on) off.delete(rule);
      else off.add(rule);
      return { ...s, alertsOff: [...off] };
    });
  return (
    <div className={styles.pageBody}>
      <div className={styles.stats4}>
        {ALERT_RULES.map((r) => {
          const state = report.alerts.rules.find((x) => x.rule === r.rule)!;
          const status =
            r.rule === "open-links"
              ? state.count
                ? `${state.count} open now`
                : "None open"
              : state.count
                ? `${state.count} in this period`
                : "None in this period";
          return (
            <section key={r.rule} className={styles.ruleCard} aria-label={r.title}>
              <span className={styles.ruleHead}>
                <strong>{r.title}</strong>
                <Switch checked={state.enabled} onChange={(on) => setRule(r.rule, on)} label={`${r.title} alerts`} />
              </span>
              <span className={styles.muted}>{r.description}</span>
              <span className={state.count && state.enabled ? styles.warnSmall : styles.mutedSmall}>{state.enabled ? status : "Off"}</span>
            </section>
          );
        })}
      </div>
      <div className={styles.activitySplit}>
        <Card title="Recent alerts">
          {report.alerts.items.length === 0 ? (
            <p className={styles.muted}>
              <CheckIcon size={14} /> No alerts in this period.
            </p>
          ) : (
            <ul className={styles.alertList}>
              {report.alerts.items.slice(0, 8).map((a) => (
                <li key={a.id}>
                  {a.status === "open" ? <AlertTriangleIcon size={18} className={styles.warn} /> : <CheckIcon size={18} className={styles.muted} />}
                  <span className={styles.alertBody}>
                    <span>
                      <strong>{a.title}</strong> <span className={a.status === "open" ? styles.tagWarn : styles.tagMuted}>{a.status === "open" ? "Open" : "Resolved"}</span>
                    </span>
                    <span>
                      {a.detail}
                      {a.rule !== "open-links" ? ` · ${fmt.short(a.at)}, ${fmt.time(a.at)}` : ""}
                    </span>
                    {(a.status !== "open" || a.advice) && <span className={styles.muted}>{a.status === "open" ? a.advice : "Marked as expected."}</span>}
                    {a.status === "open" && a.rule !== "open-links" && (
                      <button type="button" className={styles.textLink} onClick={() => saveSettings((s) => ({ ...s, dismissed: [...(s.dismissed ?? []), a.id] }))}>
                        Mark as expected
                      </button>
                    )}
                    {a.rule === "open-links" && (
                      <button type="button" className={styles.textLink} onClick={() => props.go("links")}>
                        Review on Share links
                      </button>
                    )}
                  </span>
                </li>
              ))}
            </ul>
          )}
        </Card>
        <Card title="Activity timeline">
          <div className={styles.chips} role="group" aria-label="Filter activity">
            {TIMELINE_FILTERS.map((f) => (
              <button key={f.key} type="button" className={`${styles.chip} ${filter === f.key ? styles.chipOn : ""}`} aria-pressed={filter === f.key} onClick={() => setFilter(f.key)}>
                {f.label}
              </button>
            ))}
          </div>
          {groups.length === 0 ? (
            <p className={styles.muted}>No activity in this period.</p>
          ) : (
            groups.map((g) => (
              <div key={g.label} className={styles.dayGroup}>
                <span className={styles.dayLabel}>{g.label}</span>
                <ol className={styles.timeline}>
                  {g.items.map((t, i) => (
                    <li key={`${t.at}-${i}`}>
                      <span className={styles.timelineIcon}>{KIND_ICON[t.kind]}</span>
                      <span className={styles.timelineText}>
                        <strong>{t.title}</strong>
                        <span className={styles.muted}>{t.detail}</span>
                      </span>
                      <time className={styles.mono} dateTime={new Date(t.at).toISOString()}>
                        {fmt.time(t.at)}
                      </time>
                    </li>
                  ))}
                </ol>
              </div>
            ))
          )}
          <p className={styles.footnote}>Which device did what isn't recorded anywhere. Deletions, renames and link changes are noticed each time you open Analytics.</p>
        </Card>
      </div>
    </div>
  );
}
