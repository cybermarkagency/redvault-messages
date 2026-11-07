import { useState } from "react";
import * as sapi from "../../lib/storiesApi.js";
import type { Story, StoryFriend } from "../../lib/stories.js";
import { tallyAnswers, type Inbox, type InboxEntry } from "../../lib/storyInteractions.js";
import type { AnswerableSticker } from "../../lib/storyCrypto.js";
import { CloseIcon } from "../icons24.js";
import { stickerLabel } from "./StickerView.js";
import styles from "./Stories.module.css";

const percent = (n: number, total: number) => (total > 0 ? Math.round((n / total) * 100) : 0);

/** The author's view of the answers sealed to them: totals for polls, quizzes and sliders, and each written answer. */
export function StoryAnswersSheet({ story, inbox, friends, onClose, onDone }: { story: Story; inbox: Inbox | null; friends: Map<string, StoryFriend>; onClose: () => void; onDone: (message: string) => void }) {
  const stickers = (story.meta.stickers ?? []).filter((s): s is AnswerableSticker => s.type === "poll" || s.type === "quiz" || s.type === "question" || s.type === "slider");
  return (
    <div className={styles.sheet} role="region" aria-label="Answers to your stickers">
      <div className={styles.sheetHead}>
        <strong>Answers</strong>
        <button type="button" className={styles.chromeButton} aria-label="Close" onClick={onClose}>
          <CloseIcon size={18} />
        </button>
      </div>
      <p className={styles.sheetNote}>
        Only you can open these. Polls, quizzes and sliders show totals, not who chose what. Totals reach your friends while your app is open.
      </p>
      {!inbox && <p className={styles.sheetNote}>Opening answers…</p>}
      {inbox && inbox.unreadable > 0 && <p className={styles.sheetNote}>{inbox.unreadable} answer{inbox.unreadable === 1 ? "" : "s"} couldn't be opened and {inbox.unreadable === 1 ? "isn't" : "aren't"} counted.</p>}
      {inbox &&
        stickers.map((sticker) => (
          <StickerAnswers key={sticker.id} story={story} sticker={sticker} entries={inbox.entries.filter((e) => e.sticker.id === sticker.id)} all={inbox.entries} friends={friends} onDone={onDone} />
        ))}
    </div>
  );
}

function StickerAnswers({ story, sticker, entries, all, friends, onDone }: { story: Story; sticker: AnswerableSticker; entries: InboxEntry[]; all: InboxEntry[]; friends: Map<string, StoryFriend>; onDone: (m: string) => void }) {
  const totals = tallyAnswers(sticker, all);
  const title = sticker.type === "question" || sticker.type === "slider" ? sticker.prompt : sticker.question;
  return (
    <section className={styles.answerGroup} aria-label={stickerLabel(sticker)}>
      <h3 className={styles.answerTitle}>{title}</h3>
      <div className={styles.sheetNote}>
        {totals.total} {totals.total === 1 ? "answer" : "answers"}
        {sticker.type === "poll" && sticker.closesAt !== undefined && Date.now() >= sticker.closesAt ? " · voting has ended" : ""}
        {sticker.type === "quiz" ? ` · ${totals.correct ?? 0} right` : ""}
        {sticker.type === "slider" && totals.total > 0 ? ` · average ${totals.average}%` : ""}
      </div>
      {(sticker.type === "poll" || sticker.type === "quiz") && (
        <ul className={styles.answerBars}>
          {sticker.options.map((o, i) => (
            <li key={i} className={styles.answerBarRow}>
              <span className={styles.answerBarLabel}>
                {o}
                {sticker.type === "quiz" && sticker.correct === i ? " ✓" : ""}
              </span>
              <span className={styles.answerBarTrack} aria-hidden="true">
                <span className={styles.answerBarFill} style={{ width: `${percent(totals.counts![i], totals.total)}%` }} />
              </span>
              <span className={styles.answerBarCount}>
                {totals.counts![i]} · {percent(totals.counts![i], totals.total)}%
              </span>
            </li>
          ))}
        </ul>
      )}
      {sticker.type === "question" && (
        <ul className={styles.sheetList}>
          {entries.length === 0 && <li className={styles.sheetNote}>No answers yet.</li>}
          {entries.map((e) => (
            <QuestionAnswer key={e.envelope} story={story} entry={e} sender={e.responderId ? (friends.get(e.responderId)?.handle ?? "Someone who's no longer your friend") : "Anonymous"} onDone={onDone} />
          ))}
        </ul>
      )}
    </section>
  );
}

function QuestionAnswer({ story, entry, sender, onDone }: { story: Story; entry: InboxEntry; sender: string; onDone: (m: string) => void }) {
  const [open, setOpen] = useState(false);
  const [reason, setReason] = useState("");
  const [share, setShare] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const text = entry.answer.k === "question" ? entry.answer.text : "";
  async function send() {
    setBusy(true);
    setError(null);
    try {
      await sapi.reportStoryAnswer(story.id, { stickerId: entry.sticker.id, envelope: entry.envelope, reason: reason.trim() || "Abusive answer", ...(share ? { evidence: text.slice(0, 600) } : {}) });
      setOpen(false);
      onDone("Reported. RedVault's admins can see who sent it.");
    } catch (err) {
      setError(err instanceof Error && err.message ? err.message : "Couldn't send the report.");
    } finally {
      setBusy(false);
    }
  }
  return (
    <li className={styles.answerRow}>
      <div>
        <div className={styles.answerSender}>{sender}</div>
        <div className={styles.answerText}>{text}</div>
      </div>
      <button type="button" className={styles.sheetButton} aria-expanded={open} onClick={() => setOpen(!open)}>
        Report
      </button>
      {open && (
        <div className={styles.answerReport}>
          <input className={styles.sheetInput} aria-label="What's wrong with this answer?" placeholder="What's wrong?" maxLength={300} value={reason} onChange={(e) => setReason(e.target.value)} />
          <label className={styles.editorCheck}>
            <input type="checkbox" checked={share} onChange={(e) => setShare(e.target.checked)} />
            <span>Include the answer's text in the report</span>
          </label>
          {error && <p className={styles.sheetError}>{error}</p>}
          <button type="button" className={styles.sheetButtonPrimary} disabled={busy} onClick={send}>
            Send report
          </button>
        </div>
      )}
    </li>
  );
}
