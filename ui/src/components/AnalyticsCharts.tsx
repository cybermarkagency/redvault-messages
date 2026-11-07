// SVG charts for the analytics pages, per the desktop handoff: one series
// each, one y-axis, blue (#3987e5) for single series, fixed colours for
// file types, 4px rounded bar tops with 2px gaps, a hover tooltip on every
// mark, and a table view where a chart carries the page.
import { useEffect, useState, type PointerEvent, type ReactNode } from "react";
import styles from "./AnalyticsCharts.module.css";

export const SERIES_BLUE = "#3987e5";
/** One blue ramp, dark to light: zero, then increasing counts. */
export const HEAT_RAMP = ["#1a2230", "#0d366b", "#184f95", "#256abf", "#3987e5", "#6da7ec", "#9ec5f4"];

/** Drawn at the plot's real width, so text and marks keep their size at any card width. */
function usePlotWidth(): [(el: HTMLDivElement | null) => void, number] {
  const [el, setEl] = useState<HTMLDivElement | null>(null);
  const [width, setWidth] = useState(640);
  useEffect(() => {
    if (!el) return;
    const now = Math.round(el.getBoundingClientRect().width);
    if (now > 0) setWidth(now);
    if (typeof ResizeObserver === "undefined") return;
    const observer = new ResizeObserver(([entry]) => {
      const w = Math.round(entry.contentRect.width);
      if (w > 0) setWidth(w);
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, [el]);
  return [setEl, width];
}

function niceStep(span: number, ticks: number): number {
  const raw = span / Math.max(1, ticks);
  const exp = Math.pow(10, Math.floor(Math.log10(raw || 1)));
  const f = raw / exp;
  return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * exp;
}

function Tooltip({ x, y, width, children }: { x: number; y: number; width: number; children: ReactNode }) {
  const flip = x / width > 0.6;
  return (
    <div className={styles.tooltip} style={{ left: x, top: y, transform: flip ? "translate(calc(-100% - 12px), -50%)" : "translate(12px, -50%)" }} role="status">
      {children}
    </div>
  );
}

export interface LinePoint {
  t: number;
  value: number;
  /** Drawn dashed and in the shaded forecast zone. */
  forecast?: boolean;
}

/** A single-series line with area fill, crosshair tooltip, and a shaded, dashed forecast zone at the end. */
export function LineChart({
  points,
  formatY,
  formatX,
  label,
  height = 200,
  fromZero = false,
}: {
  points: LinePoint[];
  formatY: (v: number) => string;
  formatX: (t: number) => string;
  label: string;
  height?: number;
  fromZero?: boolean;
}) {
  const [hover, setHover] = useState<number | null>(null);
  const [plotRef, W] = usePlotWidth();
  if (points.length === 0) return <p className={styles.empty}>No data for this period yet.</p>;
  const padL = 58;
  const padR = 8;
  const padT = 10;
  const padB = 24;
  const values = points.map((p) => p.value);
  let lo = fromZero ? 0 : Math.min(...values);
  let hi = Math.max(...values);
  if (hi === lo) {
    hi = hi + (hi || 1) * 0.1;
    lo = fromZero ? 0 : Math.max(0, lo - (lo || 1) * 0.1);
  }
  const step = niceStep(hi - lo, 3);
  lo = Math.floor(lo / step) * step;
  hi = Math.ceil(hi / step) * step;
  const ticks: number[] = [];
  for (let v = lo; v <= hi + step / 2; v += step) ticks.push(v);
  const t0 = points[0].t;
  const t1 = points[points.length - 1].t;
  const sx = (t: number) => padL + (t1 === t0 ? (W - padL - padR) / 2 : ((t - t0) / (t1 - t0)) * (W - padL - padR));
  const sy = (v: number) => padT + (1 - (v - lo) / (hi - lo || 1)) * (height - padT - padB);
  const actual = points.filter((p) => !p.forecast);
  const lastActual = actual[actual.length - 1];
  const future = points.filter((p) => p.forecast);
  const path = (pts: LinePoint[]) => pts.map((p, i) => `${i ? "L" : "M"}${sx(p.t).toFixed(1)},${sy(p.value).toFixed(1)}`).join(" ");
  const area = actual.length > 1 ? `${path(actual)} L${sx(lastActual.t).toFixed(1)},${sy(lo)} L${sx(actual[0].t).toFixed(1)},${sy(lo)} Z` : "";
  const xTicks = [...new Set([0, Math.round((points.length - 1) * 0.25), Math.round((points.length - 1) * 0.5), Math.round((points.length - 1) * 0.75), points.length - 1])].map((i) => points[i]);

  const onMove = (e: PointerEvent<SVGRectElement>) => {
    const rect = e.currentTarget.getBoundingClientRect();
    const x = e.clientX - rect.left + padL;
    let best = 0;
    for (let i = 1; i < points.length; i++) if (Math.abs(sx(points[i].t) - x) < Math.abs(sx(points[best].t) - x)) best = i;
    setHover(best);
  };
  const h = hover !== null ? points[hover] : null;

  return (
    <figure className={styles.figure}>
      <div className={styles.plot} ref={plotRef}>
        <svg width={W} height={height} className={styles.svg} role="img" aria-label={label}>
          {future.length > 0 && lastActual && (
            <>
              <rect x={sx(lastActual.t)} y={padT} width={Math.max(0, W - padR - sx(lastActual.t))} height={height - padT - padB} className={styles.forecastZone} />
              <text x={W - padR - 6} y={height - padB - 8} textAnchor="end" className={styles.forecastLabel}>
                Forecast
              </text>
            </>
          )}
          {ticks.map((t, i) => (
            <g key={t}>
              <line x1={padL} x2={W - padR} y1={sy(t)} y2={sy(t)} className={i === 0 ? styles.baseline : styles.grid} />
              <text x={padL - 10} y={sy(t)} className={styles.axisText} textAnchor="end" dominantBaseline="middle">
                {formatY(t)}
              </text>
            </g>
          ))}
          {xTicks.map((p, i) => (
            <text key={p.t} x={sx(p.t)} y={height - 6} className={styles.axisLabel} textAnchor={i === 0 ? "start" : i === xTicks.length - 1 ? "end" : "middle"}>
              {formatX(p.t)}
            </text>
          ))}
          {area && <path d={area} className={styles.area} />}
          <path d={path(actual)} className={styles.line} />
          {future.length > 0 && lastActual && <path d={path([lastActual, ...future])} className={styles.lineForecast} />}
          {lastActual && <circle cx={sx(lastActual.t)} cy={sy(lastActual.value)} r={3.5} className={styles.dot} />}
          {h && (
            <g>
              <line x1={sx(h.t)} x2={sx(h.t)} y1={padT} y2={sy(lo)} className={styles.crosshair} />
              <circle cx={sx(h.t)} cy={sy(h.value)} r={5} className={styles.marker} />
            </g>
          )}
          <rect x={padL} y={0} width={Math.max(0, W - padL - padR)} height={height - padB} fill="transparent" onPointerMove={onMove} onPointerLeave={() => setHover(null)} />
        </svg>
        {h && (
          <Tooltip x={sx(h.t)} y={sy(h.value)} width={W}>
            <strong>{formatY(h.value)}</strong>
            <span>
              {formatX(h.t)}
              {h.forecast ? " · forecast" : ""}
            </span>
          </Tooltip>
        )}
      </div>
    </figure>
  );
}

export interface BarDatum {
  key: string | number;
  label: string;
  value: number;
  /** Tooltip line under the value. */
  detail?: string;
}

/** Vertical bars from a zero baseline, a per-bar tooltip, and an optional dashed reference line. */
export function BarChart({
  data,
  formatValue,
  label,
  height = 180,
  tickEvery = 1,
  reference,
}: {
  data: BarDatum[];
  formatValue: (v: number) => string;
  label: string;
  height?: number;
  tickEvery?: number;
  reference?: number;
}) {
  const [hover, setHover] = useState<number | null>(null);
  const [plotRef, W] = usePlotWidth();
  if (data.length === 0) return <p className={styles.empty}>No data for this period yet.</p>;
  const padL = 56;
  const padT = 10;
  const padB = 24;
  const top = Math.max(...data.map((d) => d.value), reference ?? 0, 1);
  const step = niceStep(top, 3);
  const hi = Math.ceil(top / step) * step;
  const ticks: number[] = [];
  for (let v = 0; v <= hi + step / 2; v += step) ticks.push(v);
  const slot = (W - padL) / data.length;
  const gap = 2;
  const barW = Math.max(1, slot - gap);
  const sy = (v: number) => padT + (1 - v / hi) * (height - padT - padB);
  const r = Math.min(4, barW / 2);
  const bar = (x: number, v: number) => {
    const y = sy(v);
    const b = sy(0);
    if (b - y < 0.5) return "";
    const rr = Math.min(r, b - y);
    return `M${x},${b} L${x},${y + rr} Q${x},${y} ${x + rr},${y} L${x + barW - rr},${y} Q${x + barW},${y} ${x + barW},${y + rr} L${x + barW},${b} Z`;
  };
  const h = hover !== null ? data[hover] : null;

  return (
    <figure className={styles.figure}>
      <div className={styles.plot} ref={plotRef}>
        <svg width={W} height={height} className={styles.svg} role="img" aria-label={label}>
          {ticks.map((t, i) => (
            <g key={t}>
              <line x1={padL} x2={W} y1={sy(t)} y2={sy(t)} className={i === 0 ? styles.baseline : styles.grid} />
              <text x={padL - 10} y={sy(t)} className={styles.axisText} textAnchor="end" dominantBaseline="middle">
                {formatValue(t)}
              </text>
            </g>
          ))}
          {data.map((d, i) => {
            const x = padL + i * slot + gap / 2;
            return (
              <g key={d.key}>
                <path d={bar(x, d.value)} className={hover === i ? styles.barHover : styles.bar} />
                {i % tickEvery === 0 && (
                  <text x={x + barW / 2} y={height - 6} className={styles.axisLabel} textAnchor="middle">
                    {d.label}
                  </text>
                )}
                <rect x={padL + i * slot} y={0} width={slot} height={height - padB} fill="transparent" onPointerEnter={() => setHover(i)} onPointerLeave={() => setHover(null)} />
              </g>
            );
          })}
          {reference !== undefined && reference > 0 && <line x1={padL} x2={W} y1={sy(reference)} y2={sy(reference)} className={styles.reference} />}
        </svg>
        {h && hover !== null && (
          <Tooltip x={padL + hover * slot + slot / 2} y={sy(h.value)} width={W}>
            <strong>{formatValue(h.value)}</strong>
            <span>{h.detail ?? h.label}</span>
          </Tooltip>
        )}
      </div>
    </figure>
  );
}

/** Uploads per hour × weekday, one blue ramp. Each cell has a tooltip. */
export function Heatmap({ rows, rowLabels, formatCell, label }: { rows: number[][]; rowLabels: string[]; formatCell: (row: number, col: number, value: number) => string; label: string }) {
  const [hover, setHover] = useState<{ r: number; c: number } | null>(null);
  const max = Math.max(1, ...rows.flat());
  const level = (v: number) => (v === 0 ? 0 : Math.min(HEAT_RAMP.length - 1, 1 + Math.floor((v / max) * (HEAT_RAMP.length - 1.001))));
  return (
    <div className={styles.heat} role="img" aria-label={label} onPointerLeave={() => setHover(null)}>
      {rows.map((cols, r) => (
        <div key={r} className={styles.heatRow}>
          <span className={styles.heatRowLabel}>{rowLabels[r]}</span>
          {cols.map((v, c) => (
            <span key={c} className={styles.heatCell} style={{ background: HEAT_RAMP[level(v)] }} onPointerEnter={() => setHover({ r, c })}>
              {hover?.r === r && hover.c === c && <span className={`${styles.heatTip} ${c > 16 ? styles.heatTipLeft : ""}`}>{formatCell(r, c, v)}</span>}
            </span>
          ))}
        </div>
      ))}
      <div className={styles.heatAxis} aria-hidden="true">
        <span>00:00</span>
        <span>06:00</span>
        <span>12:00</span>
        <span>18:00</span>
        <span>23:00</span>
      </div>
    </div>
  );
}

/** One horizontal bar split into coloured segments with 2px gaps. The caller shows the legend. */
export function StackedBar({ segments, label, height = 12 }: { segments: { key: string; value: number; color: string; label: string }[]; label: string; height?: number }) {
  const total = segments.reduce((n, s) => n + s.value, 0);
  return (
    <div className={styles.stacked} style={{ height }} role="img" aria-label={`${label}: ${segments.map((s) => s.label).join(", ")}`}>
      {total > 0 &&
        segments
          .filter((s) => s.value > 0)
          .map((s) => <span key={s.key} title={s.label} style={{ flexGrow: s.value, background: s.color }} />)}
    </div>
  );
}

/** Ranked horizontal bars: label, bar, then an optional count and the value (text in text ink, never the bar colour). */
export function RankedBars({
  rows,
  formatValue,
  label,
  swatches = false,
}: {
  rows: { key: string; label: string; value: number; color?: string; count?: string }[];
  formatValue: (v: number) => string;
  label: string;
  swatches?: boolean;
}) {
  if (rows.length === 0) return <p className={styles.empty}>Nothing here yet.</p>;
  const max = Math.max(...rows.map((r) => r.value), 1);
  const withCount = rows.some((r) => r.count);
  return (
    <ul className={`${styles.ranked} ${withCount ? styles.rankedWithCount : ""}`} aria-label={label}>
      {rows.map((r) => (
        <li key={r.key} title={`${r.label}: ${formatValue(r.value)}`}>
          <span className={styles.rankedLabel}>
            {swatches && <span className={styles.swatch} style={{ background: r.color ?? SERIES_BLUE }} aria-hidden="true" />}
            <span className={styles.rankedName}>{r.label}</span>
          </span>
          <span className={styles.rankedTrack} aria-hidden="true">
            <span className={styles.rankedFill} style={{ width: `${Math.max(1.5, (r.value / max) * 100)}%`, background: r.color ?? SERIES_BLUE }} />
          </span>
          {withCount && <span className={styles.rankedCount}>{r.count}</span>}
          <span className={styles.rankedValue}>{formatValue(r.value)}</span>
        </li>
      ))}
    </ul>
  );
}

/** Capacity meter in the series blue; warning / critical as it fills (the caller says so in text too). */
export function Meter({ percent, label }: { percent: number; label: string }) {
  const level = percent >= 90 ? styles.meterCritical : percent >= 75 ? styles.meterWarning : "";
  return (
    <span className={styles.meter} role="meter" aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(percent)} aria-label={label}>
      <span className={`${styles.meterFill} ${level}`} style={{ width: `${Math.max(percent > 0 ? 1 : 0, Math.min(100, percent))}%` }} />
    </span>
  );
}
