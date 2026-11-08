// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import * as api from "../../lib/api.js";
import { resolveApiUrl } from "../../lib/apiBase.js";
import { formatDuration, nextSpeed, rememberedVote, rememberVote, savedPosition, savePosition } from "../../lib/channelPosts.js";
import { formatBytes } from "../../lib/fileMessage.js";
import { FormattedText } from "../FormattedText.js";
import { FileIcon, PauseIcon, PlayIcon } from "../icons24.js";
import styles from "./ChannelPost.module.css";

/**
 * What a channel post carries besides its header and footer: formatted text,
 * photos, videos, files and recordings, a poll or quiz, or a checklist.
 */
export function PostContent({
  channelId,
  post,
  access,
  isOwner,
  onChange,
}: {
  channelId: string;
  post: api.ChannelPostDto;
  access: string | null;
  isOwner: boolean;
  /** The post changed (a poll closed): replace it. */
  onChange?: (post: api.ChannelPostDto) => void;
}) {
  return (
    <>
      {post.suggested && <span className={`${styles.suggestedTag} type-caption`}>Suggested by a follower</span>}
      {post.media && post.media.length > 0 && <MediaGallery media={post.media} />}
      {post.body && (
        <div className={styles.body}>
          <FormattedText text={post.body} />
        </div>
      )}
      {post.poll && (
        <PollCard
          channelId={channelId}
          postId={post.id}
          poll={post.poll}
          access={access}
          isOwner={isOwner}
          onClosed={(closed) => onChange?.({ ...post, poll: closed })}
        />
      )}
      {post.checklist && <ChecklistCard channelId={channelId} postId={post.id} checklist={post.checklist} access={access} canTick={isOwner || post.checklist.collaborative} />}
    </>
  );
}

/** Photos at full resolution (tap to open the original), videos, round videos, podcasts, voice clips and files. */
export function MediaGallery({ media }: { media: api.ChannelMediaDto[] }) {
  const photos = media.filter((m) => m.kind === "photo");
  const others = media.filter((m) => m.kind !== "photo");
  return (
    <div className={styles.gallery}>
      {photos.length > 0 && (
        <div className={`${styles.photos} ${photos.length === 1 ? styles.photosOne : ""}`}>
          {photos.map((m) => (
            <a key={m.id} href={resolveApiUrl(m.url)} target="_blank" rel="noopener noreferrer" className={styles.photoLink} title="Open the full-resolution photo">
              <img src={resolveApiUrl(m.url)} alt={m.filename ?? "Photo"} loading="lazy" className={styles.photo} width={m.width ?? undefined} height={m.height ?? undefined} />
            </a>
          ))}
        </div>
      )}
      {others.map((m) => {
        if (m.kind === "video") return <video key={m.id} className={styles.video} src={resolveApiUrl(m.url)} controls preload="metadata" playsInline aria-label={m.filename ?? "Video"} />;
        if (m.kind === "round") return <RoundVideo key={m.id} media={m} />;
        if (m.kind === "audio") return <AudioPlayer key={m.id} media={m} variant="podcast" />;
        if (m.kind === "voice") return <AudioPlayer key={m.id} media={m} variant="voice" />;
        return (
          <a key={m.id} className={styles.file} href={resolveApiUrl(m.url)} download={m.filename ?? true} rel="noopener noreferrer">
            <FileIcon size={22} />
            <span className={styles.fileText}>
              <span className={styles.fileName}>{m.filename ?? "File"}</span>
              <span className="type-caption">{formatBytes(m.size)} · Download</span>
            </span>
          </a>
        );
      })}
    </div>
  );
}

/** A round video message: tap to play with sound, tap again to pause. */
function RoundVideo({ media }: { media: api.ChannelMediaDto }) {
  const ref = useRef<HTMLVideoElement>(null);
  const [playing, setPlaying] = useState(false);
  return (
    <button
      type="button"
      className={styles.round}
      aria-label={playing ? "Pause video message" : "Play video message"}
      onClick={() => {
        const el = ref.current;
        if (!el) return;
        if (el.paused) void el.play().catch(() => {});
        else el.pause();
      }}
    >
      <video
        ref={ref}
        className={styles.roundVideo}
        src={resolveApiUrl(media.url)}
        preload="metadata"
        playsInline
        onPlay={() => setPlaying(true)}
        onPause={() => setPlaying(false)}
        onEnded={() => setPlaying(false)}
      />
      {!playing && (
        <span className={styles.roundBadge}>
          <PlayIcon size={16} /> {media.durationMs ? formatDuration(media.durationMs / 1000) : ""}
        </span>
      )}
    </button>
  );
}

/**
 * Podcasts and long audio (resume where you stopped, skip 15 s, 1×/1.5×/2×),
 * and voice clips (the same, compact). Positions stay on this device.
 */
export function AudioPlayer({ media, variant }: { media: api.ChannelMediaDto; variant: "podcast" | "voice" }) {
  const ref = useRef<HTMLAudioElement>(null);
  const [playing, setPlaying] = useState(false);
  const [time, setTime] = useState(0);
  const [duration, setDuration] = useState((media.durationMs ?? 0) / 1000);
  const [speed, setSpeed] = useState(1);
  const lastSaved = useRef(0);
  const podcast = variant === "podcast";

  useEffect(() => {
    if (ref.current) ref.current.playbackRate = speed;
  }, [speed]);

  function onLoaded() {
    const el = ref.current!;
    if (Number.isFinite(el.duration)) setDuration(el.duration);
    if (podcast) {
      const resumeAt = savedPosition(media.id);
      if (resumeAt > 5 && (!Number.isFinite(el.duration) || resumeAt < el.duration - 5)) {
        el.currentTime = resumeAt;
        setTime(resumeAt);
      }
    }
  }

  function onTime() {
    const el = ref.current!;
    setTime(el.currentTime);
    if (podcast && Math.abs(el.currentTime - lastSaved.current) >= 5) {
      lastSaved.current = el.currentTime;
      savePosition(media.id, el.currentTime);
    }
  }

  const toggle = () => {
    const el = ref.current;
    if (!el) return;
    if (el.paused) void el.play().catch(() => {});
    else el.pause();
  };
  const skip = (seconds: number) => {
    const el = ref.current;
    if (el) el.currentTime = Math.max(0, Math.min((Number.isFinite(el.duration) ? el.duration : duration) || 0, el.currentTime + seconds));
  };

  return (
    <div className={`${styles.audio} ${podcast ? "" : styles.audioVoice}`}>
      <audio
        ref={ref}
        src={resolveApiUrl(media.url)}
        preload="metadata"
        onLoadedMetadata={onLoaded}
        onTimeUpdate={onTime}
        onPlay={() => setPlaying(true)}
        onPause={() => {
          setPlaying(false);
          if (podcast && ref.current) savePosition(media.id, ref.current.currentTime);
        }}
        onEnded={() => {
          setPlaying(false);
          if (podcast) savePosition(media.id, 0);
        }}
      />
      <button type="button" className={styles.playButton} onClick={toggle} aria-label={playing ? "Pause" : "Play"}>
        {playing ? <PauseIcon size={18} /> : <PlayIcon size={18} />}
      </button>
      <div className={styles.audioMain}>
        {podcast && media.filename && <span className={styles.audioTitle}>{media.filename}</span>}
        <input
          type="range"
          className={styles.seek}
          min={0}
          max={duration || 0}
          step={0.1}
          value={Math.min(time, duration || 0)}
          aria-label="Position"
          onChange={(e) => {
            const el = ref.current;
            if (el) el.currentTime = Number(e.target.value);
            setTime(Number(e.target.value));
          }}
        />
        <span className={`${styles.audioTime} type-caption`}>
          {formatDuration(time)} / {formatDuration(duration)}
          {podcast && time === 0 && savedPosition(media.id) > 5 && ` · resumes at ${formatDuration(savedPosition(media.id))}`}
        </span>
      </div>
      {podcast && (
        <>
          <button type="button" className={styles.audioSmall} onClick={() => skip(-15)} aria-label="Back 15 seconds">
            −15
          </button>
          <button type="button" className={styles.audioSmall} onClick={() => skip(15)} aria-label="Forward 15 seconds">
            +15
          </button>
        </>
      )}
      <button type="button" className={styles.audioSmall} onClick={() => setSpeed(nextSpeed(speed))} aria-label={`Playback speed ${speed}×`}>
        {speed}×
      </button>
    </div>
  );
}

/** A poll or quiz. You see the results once you've voted (or it's closed); a quiz shows whether you were right. */
function PollCard({
  channelId,
  postId,
  poll: initial,
  access,
  isOwner,
  onClosed,
}: {
  channelId: string;
  postId: string;
  poll: api.ChannelPollDto;
  access: string | null;
  isOwner: boolean;
  onClosed: (poll: api.ChannelPollDto) => void;
}) {
  const [poll, setPoll] = useState(initial);
  const [mine, setMine] = useState(() => rememberedVote(postId));
  const [picked, setPicked] = useState<number[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const showResults = mine !== null || poll.closed;
  const correct = poll.correct ?? mine?.correct ?? null;
  const explanation = poll.explanation ?? mine?.explanation ?? null;

  async function vote(options: number[]) {
    setBusy(true);
    setError(null);
    try {
      const result = await api.voteInChannelPoll(channelId, postId, options, access);
      setPoll(result.poll);
      const remembered = result.mine.length ? { mine: result.mine, ...(result.poll.quiz ? { correct: result.poll.correct, explanation: result.poll.explanation } : {}) } : null;
      rememberVote(postId, remembered, result.poll);
      setMine(remembered);
      setPicked([]);
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : "Couldn't send your vote. Try again.");
    } finally {
      setBusy(false);
    }
  }

  async function close() {
    setBusy(true);
    try {
      const updated = await api.closeChannelPoll(channelId, postId);
      if (updated.poll) {
        setPoll(updated.poll);
        onClosed(updated.poll);
      }
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : "Couldn't close the poll.");
    } finally {
      setBusy(false);
    }
  }

  const kind = poll.quiz ? "Quiz" : poll.multiple ? "Poll · pick any" : "Poll";
  return (
    <div className={styles.poll} role="group" aria-label={`${kind}: ${poll.question}`}>
      <div className={`${styles.pollKind} type-caption`}>
        {kind}
        {poll.closed ? " · closed" : ""}
      </div>
      <div className={styles.pollQuestion}>{poll.question}</div>
      <div className={styles.pollOptions}>
        {poll.options.map((option, i) => {
          if (!showResults) {
            if (poll.multiple) {
              const on = picked.includes(i);
              return (
                <label key={i} className={`${styles.pollOption} ${on ? styles.pollOptionOn : ""}`}>
                  <input type="checkbox" checked={on} disabled={busy} onChange={() => setPicked(on ? picked.filter((p) => p !== i) : [...picked, i])} />
                  {option}
                </label>
              );
            }
            return (
              <button key={i} type="button" className={styles.pollOption} disabled={busy} onClick={() => vote([i])}>
                {option}
              </button>
            );
          }
          const total = poll.voters || 0;
          const percent = total ? Math.round(((poll.counts[i] ?? 0) / total) * 100) : 0;
          const chosen = mine?.mine.includes(i) ?? false;
          const right = poll.quiz && correct === i;
          const wrong = poll.quiz && chosen && correct !== null && correct !== i;
          return (
            <div key={i} className={`${styles.pollResult} ${right ? styles.pollRight : ""} ${wrong ? styles.pollWrong : ""}`}>
              <span className={styles.pollBar} style={{ width: `${percent}%` }} />
              <span className={styles.pollResultText}>
                <span>
                  {chosen ? "✓ " : ""}
                  {option}
                  {right ? " (right answer)" : ""}
                </span>
                <span>{percent}%</span>
              </span>
            </div>
          );
        })}
      </div>
      {!showResults && poll.multiple && (
        <button type="button" className={styles.pollVote} disabled={busy || picked.length === 0} onClick={() => vote(picked)}>
          Vote
        </button>
      )}
      {poll.quiz && showResults && explanation && <p className={styles.pollExplanation}>{explanation}</p>}
      {error && <p className={styles.error}>{error}</p>}
      <div className={`${styles.pollFoot} type-caption`}>
        <span>
          {poll.voters} {poll.voters === 1 ? "vote" : "votes"}
          {!showResults && " · results show after you vote"}
        </span>
        <span>
          {mine && !poll.quiz && !poll.closed && (
            <button type="button" className={styles.linkButton} disabled={busy} onClick={() => vote([])}>
              Take back my vote
            </button>
          )}
          {isOwner && !poll.closed && (
            <button type="button" className={styles.linkButton} disabled={busy} onClick={close}>
              {poll.quiz ? "Close and show the answer" : "Close poll"}
            </button>
          )}
        </span>
      </div>
    </div>
  );
}

/** A checklist: the owner ticks items, and so can followers when it's collaborative. */
function ChecklistCard({
  channelId,
  postId,
  checklist,
  access,
  canTick,
}: {
  channelId: string;
  postId: string;
  checklist: api.ChannelChecklistDto;
  access: string | null;
  canTick: boolean;
}) {
  const [items, setItems] = useState(checklist.items);
  const [error, setError] = useState<string | null>(null);
  const done = items.filter((i) => i.done).length;

  async function toggle(index: number) {
    const before = items;
    const next = items.map((item, i) => (i === index ? { ...item, done: !item.done } : item));
    setItems(next);
    setError(null);
    try {
      setItems((await api.setChannelChecklistItem(channelId, postId, index, next[index].done, access)).items);
    } catch (err) {
      setItems(before);
      setError(err instanceof api.ApiError ? err.message : "Couldn't update the checklist.");
    }
  }

  return (
    <div className={styles.checklist} role="group" aria-label={`Checklist: ${checklist.title}`}>
      <div className={styles.pollQuestion}>{checklist.title}</div>
      <div className={`${styles.pollKind} type-caption`}>
        {done} of {items.length} done{checklist.collaborative ? " · anyone following can tick items" : ""}
      </div>
      {items.map((item, i) => (
        <label key={i} className={`${styles.checkItem} ${item.done ? styles.checkDone : ""}`}>
          <input type="checkbox" checked={item.done} disabled={!canTick} onChange={() => toggle(i)} />
          <span>{item.text}</span>
        </label>
      ))}
      {error && <p className={styles.error}>{error}</p>}
    </div>
  );
}
