import { useState } from "react";
import type { StoryFriend } from "../../lib/stories.js";
import {
  MAX_STICKER_OPTION,
  MAX_STICKER_TEXT,
  newStickerId,
  parseStickers,
  safeStoryLink,
  type ResultsVisibility,
  type StorySticker,
} from "../../lib/storyCrypto.js";
import styles from "./Stories.module.css";

export type StickerType = StorySticker["type"];

export const STICKER_CHOICES: { type: StickerType; label: string; hint: string; interactive: boolean }[] = [
  { type: "poll", label: "Poll", hint: "Two to four options", interactive: true },
  { type: "quiz", label: "Quiz", hint: "One right answer", interactive: true },
  { type: "question", label: "Question", hint: "Friends write back", interactive: true },
  { type: "slider", label: "Emoji slider", hint: "How much?", interactive: true },
  { type: "countdown", label: "Countdown", hint: "Counts down to a time", interactive: false },
  { type: "link", label: "Link", hint: "A web address", interactive: false },
  { type: "mention", label: "Mention", hint: "Tag a friend", interactive: false },
  { type: "date", label: "Date or time", hint: "Shows in each person's time zone", interactive: false },
  { type: "location", label: "Place", hint: "Typed by you, never read from your device", interactive: false },
];

const SLIDER_EMOJI = ["😍", "🔥", "😂", "👍", "❤️", "😮"];
const POLL_DEADLINES: { label: string; hours: number | null }[] = [
  { label: "When the story ends", hours: null },
  { label: "In 1 hour", hours: 1 },
  { label: "In 6 hours", hours: 6 },
  { label: "In 12 hours", hours: 12 },
  { label: "In 24 hours", hours: 24 },
];

/** A local date-time field value ("2026-05-04T18:30") for a time, and back. */
const toLocalInput = (ms: number) => {
  const d = new Date(ms);
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`;
};
const fromLocalInput = (v: string): number | null => {
  const ms = new Date(v).getTime();
  return Number.isFinite(ms) ? ms : null;
};

interface Props {
  type: StickerType;
  initial?: StorySticker;
  friends: Map<string, StoryFriend>;
  onSave: (sticker: StorySticker) => void;
  onCancel: () => void;
}

/** The form for one sticker. What it builds is checked by the same parser the receiving app uses. */
export function StickerEditor({ type, initial, friends, onSave, onCancel }: Props) {
  const pick = <K extends StorySticker["type"]>(t: K): Extract<StorySticker, { type: K }> | undefined => (initial && initial.type === t ? (initial as Extract<StorySticker, { type: K }>) : undefined);
  const [question, setQuestion] = useState(pick("poll")?.question ?? pick("quiz")?.question ?? pick("question")?.prompt ?? pick("slider")?.prompt ?? "");
  const [options, setOptions] = useState<string[]>(pick("poll")?.options ?? pick("quiz")?.options ?? ["", ""]);
  const [correct, setCorrect] = useState(pick("quiz")?.correct ?? 0);
  const [explanation, setExplanation] = useState(pick("quiz")?.explanation ?? "");
  const [results, setResults] = useState<ResultsVisibility>(pick("poll")?.results ?? pick("quiz")?.results ?? pick("slider")?.results ?? "everyone");
  const [deadlineHours, setDeadlineHours] = useState<number | null>(null);
  const [anonymous, setAnonymous] = useState(pick("question")?.anonymous ?? false);
  const [emoji, setEmoji] = useState(pick("slider")?.emoji ?? SLIDER_EMOJI[0]);
  const [title, setTitle] = useState(pick("countdown")?.title ?? "");
  const [when, setWhen] = useState(toLocalInput(pick("countdown")?.endsAt ?? pick("date")?.at ?? Date.now() + 24 * 3600_000));
  const [dateStyle, setDateStyle] = useState<"date" | "time" | "datetime">(pick("date")?.style ?? "datetime");
  const [url, setUrl] = useState(pick("link")?.url ?? "https://");
  const [label, setLabel] = useState(pick("link")?.label ?? pick("location")?.label ?? "");
  const [mention, setMention] = useState(pick("mention")?.userId ?? "");
  const [error, setError] = useState<string | null>(null);

  const trimmedOptions = options.map((o) => o.trim());

  function save() {
    const place = { id: initial?.id ?? newStickerId(), x: initial?.x ?? 0.5, y: initial?.y ?? 0.5, scale: initial?.scale ?? 1, rotation: initial?.rotation ?? 0 };
    let candidate: unknown;
    const at = fromLocalInput(when);
    switch (type) {
      case "poll":
        candidate = {
          ...place,
          type,
          question: question.trim(),
          options: trimmedOptions,
          results,
          ...(deadlineHours !== null ? { closesAt: Date.now() + deadlineHours * 3600_000 } : pick("poll")?.closesAt ? { closesAt: pick("poll")!.closesAt } : {}),
        };
        break;
      case "quiz":
        candidate = { ...place, type, question: question.trim(), options: trimmedOptions, correct, results, ...(explanation.trim() ? { explanation: explanation.trim() } : {}) };
        break;
      case "question":
        candidate = { ...place, type, prompt: question.trim(), anonymous };
        break;
      case "slider":
        candidate = { ...place, type, prompt: question.trim(), emoji, results };
        break;
      case "countdown":
        if (at !== null && at <= Date.now()) return setError("Pick a time in the future.");
        candidate = { ...place, type, title: title.trim(), endsAt: at };
        break;
      case "link":
        if (!safeStoryLink(url)) return setError("Use a full https:// address (no sign-in details in it).");
        candidate = { ...place, type, url: safeStoryLink(url), label: label.trim() || new URL(url).hostname };
        break;
      case "mention":
        if (!friends.has(mention)) return setError("Choose a friend.");
        candidate = { ...place, type, userId: mention };
        break;
      case "date":
        candidate = { ...place, type, at, style: dateStyle };
        break;
      case "location":
        candidate = { ...place, type, label: label.trim() };
        break;
    }
    const [sticker] = parseStickers([candidate]);
    if (!sticker) return setError("Fill in every field. Options can be up to 40 characters; questions up to 120.");
    onSave(sticker);
  }

  const optionFields = (
    <fieldset className={styles.editorFieldset}>
      <legend>Options</legend>
      {options.map((o, i) => (
        <div key={i} className={styles.editorRow}>
          {type === "quiz" && (
            <input type="radio" name="correct" checked={correct === i} aria-label={`Option ${i + 1} is the right answer`} onChange={() => setCorrect(i)} />
          )}
          <input
            className={styles.sheetInput}
            value={o}
            maxLength={MAX_STICKER_OPTION}
            placeholder={`Option ${i + 1}`}
            aria-label={`Option ${i + 1}`}
            onChange={(e) => setOptions(options.map((x, j) => (j === i ? e.target.value : x)))}
          />
          {options.length > 2 && (
            <button
              type="button"
              className={styles.sheetButton}
              aria-label={`Remove option ${i + 1}`}
              onClick={() => {
                setOptions(options.filter((_, j) => j !== i));
                if (correct >= options.length - 1) setCorrect(0);
              }}
            >
              ×
            </button>
          )}
        </div>
      ))}
      {options.length < 4 && (
        <button type="button" className={styles.sheetButton} onClick={() => setOptions([...options, ""])}>
          Add an option
        </button>
      )}
      {type === "quiz" && <p className={styles.sheetNote}>The right answer is revealed after someone answers. It isn't secret from their app, which holds the whole story.</p>}
    </fieldset>
  );

  const resultsField = (
    <label className={styles.editorField}>
      Who sees the results
      <select className={styles.sheetInput} value={results} onChange={(e) => setResults(e.target.value as ResultsVisibility)}>
        <option value="everyone">Everyone who can see the story</option>
        <option value="author">Only me</option>
      </select>
      <span className={styles.sheetNote}>Totals only, never who chose what. They update while your app is open.</span>
    </label>
  );

  return (
    <div className={`${styles.textEditor} ${styles.editorPanel}`} role="group" aria-label={`${STICKER_CHOICES.find((c) => c.type === type)?.label} sticker`}>
      {(type === "poll" || type === "quiz" || type === "question" || type === "slider") && (
        <label className={styles.editorField}>
          {type === "question" ? "Your question" : type === "slider" ? "Your prompt" : "Question"}
          <input className={styles.sheetInput} autoFocus value={question} maxLength={MAX_STICKER_TEXT} onChange={(e) => setQuestion(e.target.value)} />
        </label>
      )}
      {(type === "poll" || type === "quiz") && optionFields}
      {type === "quiz" && (
        <label className={styles.editorField}>
          Explanation (optional)
          <input className={styles.sheetInput} value={explanation} maxLength={MAX_STICKER_TEXT} onChange={(e) => setExplanation(e.target.value)} />
        </label>
      )}
      {type === "poll" && (
        <label className={styles.editorField}>
          Voting closes
          <select className={styles.sheetInput} value={deadlineHours ?? ""} onChange={(e) => setDeadlineHours(e.target.value === "" ? null : Number(e.target.value))}>
            {POLL_DEADLINES.map((d) => (
              <option key={d.label} value={d.hours ?? ""}>
                {d.label}
              </option>
            ))}
          </select>
          <span className={styles.sheetNote}>The deadline is exact in the app; the count uses times to the minute.</span>
        </label>
      )}
      {(type === "poll" || type === "quiz" || type === "slider") && resultsField}
      {type === "question" && (
        <label className={styles.editorCheck}>
          <input type="checkbox" checked={anonymous} onChange={(e) => setAnonymous(e.target.checked)} />
          <span>
            Answers come without a name
            <span className={styles.sheetNote}>You won't be shown who wrote them. RedVault's server can still tell who sent one, and you may be able to guess from when it arrives.</span>
          </span>
        </label>
      )}
      {type === "slider" && (
        <div className={styles.editorField} role="radiogroup" aria-label="Emoji">
          Emoji
          <div className={styles.editorRow}>
            {SLIDER_EMOJI.map((e) => (
              <button key={e} type="button" role="radio" aria-checked={emoji === e} className={`${styles.sheetButton} ${emoji === e ? styles.editorPicked : ""}`} onClick={() => setEmoji(e)}>
                {e}
              </button>
            ))}
          </div>
        </div>
      )}
      {type === "countdown" && (
        <>
          <label className={styles.editorField}>
            What is it counting down to?
            <input className={styles.sheetInput} autoFocus value={title} maxLength={60} onChange={(e) => setTitle(e.target.value)} />
          </label>
          <label className={styles.editorField}>
            When
            <input type="datetime-local" className={styles.sheetInput} value={when} onChange={(e) => setWhen(e.target.value)} />
          </label>
        </>
      )}
      {type === "date" && (
        <>
          <label className={styles.editorField}>
            When
            <input type="datetime-local" className={styles.sheetInput} value={when} onChange={(e) => setWhen(e.target.value)} />
          </label>
          <label className={styles.editorField}>
            Show
            <select className={styles.sheetInput} value={dateStyle} onChange={(e) => setDateStyle(e.target.value as typeof dateStyle)}>
              <option value="datetime">Date and time</option>
              <option value="date">Date only</option>
              <option value="time">Time only</option>
            </select>
          </label>
        </>
      )}
      {type === "link" && (
        <>
          <label className={styles.editorField}>
            Web address
            <input className={styles.sheetInput} autoFocus inputMode="url" value={url} maxLength={300} onChange={(e) => setUrl(e.target.value)} />
          </label>
          <label className={styles.editorField}>
            Text on the sticker
            <input className={styles.sheetInput} value={label} maxLength={40} placeholder="Defaults to the site name" onChange={(e) => setLabel(e.target.value)} />
          </label>
          <p className={styles.sheetNote}>Friends are asked before the link opens, and see the site it goes to.</p>
        </>
      )}
      {type === "location" && (
        <label className={styles.editorField}>
          Place name
          <input className={styles.sheetInput} autoFocus value={label} maxLength={60} onChange={(e) => setLabel(e.target.value)} />
          <span className={styles.sheetNote}>Only what you type is shared. Your device's location is never read.</span>
        </label>
      )}
      {type === "mention" && (
        <label className={styles.editorField}>
          Friend
          <select className={styles.sheetInput} value={mention} onChange={(e) => setMention(e.target.value)}>
            <option value="">Choose…</option>
            {[...friends.values()].map((f) => (
              <option key={f.userId} value={f.userId}>
                {f.handle}
              </option>
            ))}
          </select>
          <span className={styles.sheetNote}>They only see it if they can see this story. Nobody is notified.</span>
        </label>
      )}
      {error && <p className={styles.sheetError}>{error}</p>}
      <div className={styles.textEditorRow}>
        <button type="button" className={styles.sheetButton} onClick={onCancel}>
          Cancel
        </button>
        <span className={styles.headerSpacer} />
        <button type="button" className={styles.sheetButtonPrimary} onClick={save}>
          {initial ? "Save" : "Add"}
        </button>
      </div>
    </div>
  );
}
