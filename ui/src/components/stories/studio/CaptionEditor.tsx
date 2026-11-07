import { useState } from "react";
import { evenCaptions, formatClock, parseClock, parseSrt } from "../../../lib/studio/captions.js";
import { MAX_CAPTIONS, MAX_CAPTION_TEXT, parseCaptions, type StoryCaption } from "../../../lib/storyCrypto.js";
import styles from "../Stories.module.css";

interface Props {
  captions: StoryCaption[];
  /** The length of the recording, so lines can't run past its end. */
  durationMs: number;
  onChange: (captions: StoryCaption[]) => void;
  onClose: () => void;
}

interface Row {
  start: string;
  end: string;
  text: string;
}

const toRows = (c: StoryCaption[]): Row[] => c.map((x) => ({ start: formatClock(x.startMs), end: formatClock(x.endMs), text: x.text }));

/**
 * Subtitles for a video or audio story, typed by the author or read from an .srt file. Nothing is transcribed
 * for you: speech recognition would mean sending the recording to a service.
 */
export function CaptionEditor({ captions, durationMs, onChange, onClose }: Props) {
  const [rows, setRows] = useState<Row[]>(() => toRows(captions));
  const [lines, setLines] = useState("");
  const [error, setError] = useState<string | null>(null);

  function apply(next: Row[]): boolean {
    const parsed: StoryCaption[] = [];
    for (const r of next) {
      const startMs = parseClock(r.start);
      const endMs = parseClock(r.end);
      if (startMs === null || endMs === null || !r.text.trim()) {
        setError("Each line needs a start time, an end time and some text (like 0:03.5).");
        return false;
      }
      if (endMs <= startMs) {
        setError("A line has to end after it starts.");
        return false;
      }
      parsed.push({ startMs, endMs: Math.min(endMs, durationMs), text: r.text });
    }
    const clean = parseCaptions(parsed.filter((c) => c.startMs < durationMs));
    setError(null);
    onChange(clean);
    return true;
  }

  const edit = (i: number, change: Partial<Row>) => setRows((list) => list.map((r, j) => (j === i ? { ...r, ...change } : r)));

  async function importSrt(file: File | undefined) {
    if (!file) return;
    const text = await new Promise<string>((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => resolve(String(reader.result));
      reader.onerror = () => reject(reader.error);
      reader.readAsText(file);
    }).catch(() => "");
    const cues = parseSrt(text).filter((c) => c.startMs < durationMs);
    if (cues.length === 0) return setError("No captions could be read from that file.");
    setRows(toRows(cues));
    setError(null);
  }

  return (
    <div className={`${styles.textEditor} ${styles.editorPanel}`} role="group" aria-label="Captions">
      <strong>Captions</strong>
      <p className={styles.sheetNote}>Lines shown over the recording at the times you choose. Up to {MAX_CAPTIONS} lines of {MAX_CAPTION_TEXT} characters.</p>
      <ul className={styles.captionRows}>
        {rows.map((r, i) => (
          <li key={i} className={styles.captionRow}>
            <input className={styles.sheetInput} aria-label={`Line ${i + 1} starts at`} value={r.start} onChange={(e) => edit(i, { start: e.target.value })} />
            <input className={styles.sheetInput} aria-label={`Line ${i + 1} ends at`} value={r.end} onChange={(e) => edit(i, { end: e.target.value })} />
            <input className={styles.sheetInput} aria-label={`Line ${i + 1} text`} maxLength={MAX_CAPTION_TEXT} value={r.text} onChange={(e) => edit(i, { text: e.target.value })} />
            <button type="button" className={styles.sheetButton} aria-label={`Remove line ${i + 1}`} onClick={() => setRows((list) => list.filter((_, j) => j !== i))}>
              ×
            </button>
          </li>
        ))}
      </ul>
      <div className={styles.editorRow}>
        <button type="button" className={styles.sheetButton} disabled={rows.length >= MAX_CAPTIONS} onClick={() => setRows([...rows, { start: formatClock(0), end: formatClock(Math.min(2000, durationMs)), text: "" }])}>
          Add a line
        </button>
        <label className={styles.sheetButton}>
          Import .srt
          <input type="file" accept=".srt,text/plain" hidden onChange={(e) => { void importSrt(e.target.files?.[0]); e.target.value = ""; }} />
        </label>
      </div>
      <label className={styles.editorField}>
        Or type lines and spread them evenly
        <textarea className={styles.sheetInput} rows={3} value={lines} placeholder={"One line per caption"} onChange={(e) => setLines(e.target.value)} />
      </label>
      <button
        type="button"
        className={styles.sheetButton}
        disabled={!lines.trim()}
        onClick={() => {
          setRows(toRows(evenCaptions(lines.split("\n"), durationMs)));
          setLines("");
          setError(null);
        }}
      >
        Spread these lines
      </button>
      {error && (
        <p className={styles.sheetError} role="alert">
          {error}
        </p>
      )}
      <div className={styles.textEditorRow}>
        <button type="button" className={styles.sheetButton} onClick={onClose}>
          Cancel
        </button>
        <span className={styles.headerSpacer} />
        <button
          type="button"
          className={styles.sheetButtonPrimary}
          onClick={() => {
            if (rows.length === 0) {
              onChange([]);
              return onClose();
            }
            if (apply(rows)) onClose();
          }}
        >
          Done
        </button>
      </div>
    </div>
  );
}
