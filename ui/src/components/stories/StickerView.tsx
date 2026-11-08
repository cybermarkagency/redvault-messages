// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState, type CSSProperties } from "react";
import type { MyAnswer } from "../../lib/storyInteractions.js";
import { isClosed } from "../../lib/storyInteractions.js";
import type { StoryFriend } from "../../lib/stories.js";
import type { AnswerableSticker, StickerTotals, StorySticker, StoryAnswer } from "../../lib/storyCrypto.js";
import styles from "./Stories.module.css";

/** Where a sticker sits: the same on every screen size (sizes are in container units, like text and emoji). */
export function stickerStyle(s: StorySticker): CSSProperties {
  return { left: `${s.x * 100}%`, top: `${s.y * 100}%`, transform: `translate(-50%, -50%) rotate(${s.rotation}deg) scale(${s.scale})` };
}

export function stickerLabel(s: StorySticker): string {
  switch (s.type) {
    case "poll":
      return `Poll: ${s.question}`;
    case "quiz":
      return `Quiz: ${s.question}`;
    case "question":
      return `Question: ${s.prompt}`;
    case "slider":
      return `Slider: ${s.prompt}`;
    case "countdown":
      return `Countdown: ${s.title}`;
    case "link":
      return `Link: ${s.label}`;
    case "mention":
      return "Mention";
    case "date":
      return "Date";
    case "location":
      return `Place: ${s.label}`;
  }
}

/** "2d 4h", "3h 12m", "45s": time left until `endsAt`, or null once it has passed. */
export function timeLeft(endsAt: number, now: number): string | null {
  const ms = endsAt - now;
  if (ms <= 0) return null;
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s % 60}s`;
  return `${s}s`;
}

function formatWhen(at: number, style: "date" | "time" | "datetime"): string {
  const d = new Date(at);
  const date = new Intl.DateTimeFormat(undefined, { weekday: "short", day: "numeric", month: "short", year: "numeric" }).format(d);
  const time = new Intl.DateTimeFormat(undefined, { hour: "numeric", minute: "2-digit" }).format(d);
  return style === "date" ? date : style === "time" ? time : `${date} · ${time}`;
}

function Countdown({ endsAt, title }: { endsAt: number; title: string }) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  const left = timeLeft(endsAt, now);
  return (
    <div className={styles.stickerCard}>
      <div className={styles.stickerTitle}>{title}</div>
      <div className={styles.stickerCountdown} aria-live="off">
        {left ?? "It's time!"}
      </div>
    </div>
  );
}

const percent = (n: number, total: number) => (total > 0 ? Math.round((n / total) * 100) : 0);

function Bar({ share, on }: { share: number; on?: boolean }) {
  return (
    <span className={`${styles.stickerBar} ${on ? styles.stickerBarOn : ""}`} style={{ width: `${share}%` }} aria-hidden="true" />
  );
}

export interface StickerViewProps {
  sticker: StorySticker;
  /** "edit": a preview in the composer, nothing to press. */
  mode: "edit" | "view";
  friends?: Map<string, StoryFriend>;
  /** Your own story: you see the totals but don't answer. */
  isOwner?: boolean;
  myAnswer?: MyAnswer;
  totals?: StickerTotals;
  /** Seen on this device, answered just now (before the server confirms). */
  busy?: boolean;
  onAnswer?: (sticker: AnswerableSticker, answer: StoryAnswer) => void;
  onOpenLink?: (url: string) => void;
  /** The viewer's clock stops while someone is typing or dragging a slider. */
  onFocusChange?: (focused: boolean) => void;
}

/** One sticker, drawn as plain text and controls (never as markup from the story). */
export function StickerView({ sticker, mode, friends, isOwner, myAnswer, totals, busy, onAnswer, onOpenLink, onFocusChange }: StickerViewProps) {
  const interactive = mode === "view" && !isOwner && !!onAnswer;
  switch (sticker.type) {
    case "poll":
    case "quiz": {
      const closed = isClosed(sticker);
      const answered = myAnswer?.answer ?? null;
      const choice = answered && (answered.k === "poll" || answered.k === "quiz") ? answered.choice : null;
      // Totals go to the author, and to those who answered once the author chose to show them: nobody is nudged before voting.
      const showTotals = totals && totals.counts && totals.total > 0 && (isOwner || (sticker.results === "everyone" && !!myAnswer));
      const reveal = sticker.type === "quiz" && (choice !== null || isOwner);
      return (
        <div className={styles.stickerCard} role="group" aria-label={stickerLabel(sticker)}>
          <div className={styles.stickerTitle}>{sticker.question}</div>
          <ul className={styles.stickerOptions}>
            {sticker.options.map((option, i) => {
              const share = showTotals ? percent(totals!.counts![i], totals!.total) : 0;
              const picked = choice === i;
              const right = reveal && sticker.type === "quiz" && sticker.correct === i;
              const wrong = reveal && picked && sticker.type === "quiz" && sticker.correct !== i;
              const canPress = interactive && !busy && !closed && !(sticker.type === "quiz" && myAnswer);
              return (
                <li key={i}>
                  <button
                    type="button"
                    className={`${styles.stickerOption} ${picked ? styles.stickerPicked : ""} ${right ? styles.stickerRight : ""} ${wrong ? styles.stickerWrong : ""}`}
                    disabled={!canPress}
                    aria-pressed={picked}
                    onClick={() => onAnswer?.(sticker, { k: sticker.type, choice: i })}
                  >
                    {showTotals || (reveal && totals) ? <Bar share={share} on={picked || right} /> : null}
                    <span className={styles.stickerOptionText}>{option}</span>
                    {showTotals ? <span className={styles.stickerPercent}>{share}%</span> : null}
                  </button>
                </li>
              );
            })}
          </ul>
          {sticker.type === "quiz" && reveal && sticker.explanation && <div className={styles.stickerNote}>{sticker.explanation}</div>}
          <div className={styles.stickerNote}>
            {closed
              ? "Voting has ended"
              : isOwner
                ? totals
                  ? `${totals.total} ${totals.total === 1 ? "answer" : "answers"}${sticker.type === "quiz" && totals.correct !== undefined ? ` · ${totals.correct} right` : ""}`
                  : "No answers yet"
                : myAnswer
                  ? sticker.type === "quiz"
                    ? choice === sticker.correct
                      ? "Correct!"
                      : "Not quite"
                    : "Your vote is in. You can change it."
                  : sticker.results === "author"
                    ? "Only the author sees the results"
                    : null}
          </div>
        </div>
      );
    }
    case "question": {
      return <QuestionCard sticker={sticker} interactive={interactive} myAnswer={myAnswer} busy={busy} isOwner={isOwner} totals={totals} onAnswer={onAnswer} onFocusChange={onFocusChange} />;
    }
    case "slider": {
      return <SliderCard sticker={sticker} interactive={interactive} myAnswer={myAnswer} busy={busy} isOwner={isOwner} totals={totals} onAnswer={onAnswer} onFocusChange={onFocusChange} />;
    }
    case "countdown":
      return <Countdown endsAt={sticker.endsAt} title={sticker.title} />;
    case "link":
      return (
        <button
          type="button"
          className={styles.stickerLink}
          disabled={mode === "edit"}
          aria-label={`Link: ${sticker.label}`}
          onClick={() => onOpenLink?.(sticker.url)}
        >
          🔗 {sticker.label}
        </button>
      );
    case "mention": {
      const handle = friends?.get(sticker.userId)?.handle;
      return <div className={styles.stickerPill}>@{handle ?? "someone"}</div>;
    }
    case "date":
      return <div className={styles.stickerPill}>{formatWhen(sticker.at, sticker.style)}</div>;
    case "location":
      return <div className={styles.stickerPill}>📍 {sticker.label}</div>;
  }
}

interface CardProps<T extends AnswerableSticker> {
  sticker: T;
  interactive: boolean;
  isOwner?: boolean;
  myAnswer?: MyAnswer;
  totals?: StickerTotals;
  busy?: boolean;
  onAnswer?: (sticker: AnswerableSticker, answer: StoryAnswer) => void;
  onFocusChange?: (focused: boolean) => void;
}

function QuestionCard({ sticker, interactive, isOwner, myAnswer, busy, totals, onAnswer, onFocusChange }: CardProps<Extract<AnswerableSticker, { type: "question" }>>) {
  const [text, setText] = useState("");
  const sent = !!myAnswer;
  return (
    <div className={styles.stickerCard} role="group" aria-label={stickerLabel(sticker)}>
      <div className={styles.stickerTitle}>{sticker.prompt}</div>
      {sent ? (
        <div className={styles.stickerNote}>Sent ✓{sticker.anonymous ? " · your name isn't shown to the author" : ""}</div>
      ) : isOwner ? (
        <div className={styles.stickerNote}>{totals ? `${totals.total} ${totals.total === 1 ? "answer" : "answers"}` : "Answers appear in Activity"}</div>
      ) : (
        <form
          className={styles.stickerForm}
          onSubmit={(e) => {
            e.preventDefault();
            const t = text.trim();
            if (t && interactive && !busy) onAnswer?.(sticker, { k: "question", text: t });
          }}
        >
          <input
            className={styles.stickerInput}
            value={text}
            maxLength={500}
            placeholder="Type your answer"
            aria-label={`Your answer to: ${sticker.prompt}`}
            disabled={!interactive}
            onChange={(e) => setText(e.target.value)}
            onFocus={() => onFocusChange?.(true)}
            onBlur={() => onFocusChange?.(false)}
          />
          <button type="submit" className={styles.stickerSend} disabled={!interactive || busy || !text.trim()}>
            Send
          </button>
        </form>
      )}
      {!sent && !isOwner && sticker.anonymous && (
        <div className={styles.stickerNote}>Sent without your name. RedVault's server can still tell it was you.</div>
      )}
    </div>
  );
}

function SliderCard({ sticker, interactive, isOwner, myAnswer, busy, totals, onAnswer, onFocusChange }: CardProps<Extract<AnswerableSticker, { type: "slider" }>>) {
  const mine = myAnswer?.answer && myAnswer.answer.k === "slider" ? myAnswer.answer.value : null;
  const [value, setValue] = useState(mine ?? 50);
  useEffect(() => {
    if (mine !== null) setValue(mine);
  }, [mine]);
  const showAverage = totals && totals.average !== undefined && totals.total > 0 && (isOwner || (sticker.results === "everyone" && !!myAnswer));
  const send = () => {
    if (interactive && !busy) onAnswer?.(sticker, { k: "slider", value });
    onFocusChange?.(false);
  };
  return (
    <div className={styles.stickerCard} role="group" aria-label={stickerLabel(sticker)}>
      <div className={styles.stickerTitle}>{sticker.prompt}</div>
      <div className={styles.stickerSliderRow}>
        <input
          type="range"
          min={0}
          max={100}
          step={1}
          className={styles.stickerSlider}
          value={value}
          disabled={!interactive || busy}
          aria-label={`${sticker.prompt} ${value}%`}
          aria-valuetext={`${value}%`}
          onChange={(e) => setValue(Number(e.target.value))}
          onPointerDown={() => onFocusChange?.(true)}
          onPointerUp={send}
          onKeyUp={(e) => (e.key.startsWith("Arrow") || e.key === "Home" || e.key === "End") && send()}
          onBlur={() => onFocusChange?.(false)}
        />
        <span className={styles.stickerEmoji} aria-hidden="true">
          {sticker.emoji}
        </span>
      </div>
      <div className={styles.stickerNote}>
        {isOwner
          ? totals
            ? `${totals.total} ${totals.total === 1 ? "answer" : "answers"}${showAverage ? ` · average ${totals.average}%` : ""}`
            : "No answers yet"
          : showAverage
            ? `Average ${totals!.average}% from ${totals!.total}`
            : myAnswer
              ? "Sent ✓"
              : sticker.results === "author"
                ? "Only the author sees the results"
                : null}
      </div>
    </div>
  );
}
