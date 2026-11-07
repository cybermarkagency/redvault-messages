import { useState } from "react";
import { ApiError } from "../../lib/api.js";
import * as sapi from "../../lib/storiesApi.js";
import type { Story } from "../../lib/stories.js";
import { CloseIcon } from "../icons24.js";
import styles from "./Stories.module.css";

const REASONS = ["Spam", "Harassment or bullying", "Nudity or sexual content", "Violence or threats", "Scam or fraud", "Something else"];

/** The words on a story that a reporter may choose to include: what they can read, nothing else. */
export function storyEvidence(story: Story): string {
  const m = story.meta;
  const parts: string[] = [];
  if (m.text) parts.push(m.text);
  for (const o of m.overlays) if (o.type === "text") parts.push(o.text);
  for (const s of m.stickers ?? []) {
    if (s.type === "poll" || s.type === "quiz") parts.push(`${s.question} [${s.options.join(" / ")}]`);
    else if (s.type === "question" || s.type === "slider") parts.push(s.prompt);
    else if (s.type === "countdown") parts.push(s.title);
    else if (s.type === "link") parts.push(`${s.label} ${s.url}`);
    else if (s.type === "location") parts.push(s.label);
  }
  return parts.join(" | ").slice(0, 600);
}

/**
 * Report a story to RedVault's admins. Nothing is attached unless the reporter ticks the box, and the
 * box shows exactly what would go. A photo or video is never sent: the server can't read it, and the
 * app doesn't send it for you.
 */
export function StoryReportSheet({ story, onClose, onDone }: { story: Story; onClose: () => void; onDone: (message: string) => void }) {
  const [reason, setReason] = useState(REASONS[0]);
  const [detail, setDetail] = useState("");
  const [share, setShare] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const evidence = storyEvidence(story);

  async function send() {
    setBusy(true);
    setError(null);
    try {
      const why = `${reason}${detail.trim() ? `: ${detail.trim()}` : ""}`.slice(0, 300);
      await sapi.reportStory(story.id, { reason: why, ...(share && evidence ? { evidence } : {}) });
      onDone("Reported. Thank you.");
      onClose();
    } catch (err) {
      setError(err instanceof ApiError && err.status === 409 ? "You already reported this story." : "Couldn't send the report. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.sheet} role="region" aria-label="Report this story">
      <div className={styles.sheetHead}>
        <strong>Report this story</strong>
        <button type="button" className={styles.chromeButton} aria-label="Close" onClick={onClose}>
          <CloseIcon size={18} />
        </button>
      </div>
      <p className={styles.sheetNote}>Reports go to RedVault's admins. They can't open the story themselves: it's encrypted, and only you choose what to share.</p>
      <label className={styles.editorField}>
        What's wrong?
        <select className={styles.sheetInput} value={reason} onChange={(e) => setReason(e.target.value)}>
          {REASONS.map((r) => (
            <option key={r}>{r}</option>
          ))}
        </select>
      </label>
      <label className={styles.editorField}>
        Details (optional)
        <input className={styles.sheetInput} maxLength={200} value={detail} onChange={(e) => setDetail(e.target.value)} />
      </label>
      {evidence ? (
        <label className={styles.editorCheck}>
          <input type="checkbox" checked={share} onChange={(e) => setShare(e.target.checked)} />
          <span>
            Include the words on this story in the report
            {share && <span className={styles.evidencePreview}>{evidence}</span>}
          </span>
        </label>
      ) : (
        <p className={styles.sheetNote}>There's no text on this story to include.</p>
      )}
      {error && <p className={styles.sheetError}>{error}</p>}
      <div className={styles.textEditorRow}>
        <button type="button" className={styles.sheetButton} onClick={onClose}>
          Cancel
        </button>
        <span className={styles.headerSpacer} />
        <button type="button" className={styles.sheetButtonPrimary} disabled={busy} onClick={send}>
          Send report
        </button>
      </div>
    </div>
  );
}
