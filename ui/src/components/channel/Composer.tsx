import { type FormEvent, useRef, useState } from "react";
import * as api from "../../lib/api.js";
import { mediaKindFor, probeMedia, wrapSelection } from "../../lib/channelPosts.js";
import { formatBytes } from "../../lib/fileMessage.js";
import { CalendarIcon, CheckIcon, FileIcon, ImageIcon, MicIcon, PlusIcon, SendIcon, VideoIcon } from "../icons24.js";
import styles from "./Composer.module.css";
import { RoundRecorder, VoiceRecorder } from "./Recorders.js";

export interface Upload {
  key: number;
  name: string;
  kind: api.ChannelMediaKind;
  size: number;
  progress: number;
  mediaId: string | null;
  error: string | null;
}

let nextKey = 1;

/** Uploads picked or recorded files one by one as they're added, with progress; `start` asks the server for each upload. */
export function useUploads(start: (meta: api.ChannelUploadMeta) => Promise<{ id: string; uploadUrl: string }>, max: number) {
  const [uploads, setUploads] = useState<Upload[]>([]);
  const patch = (key: number, change: Partial<Upload>) => setUploads((list) => list.map((u) => (u.key === key ? { ...u, ...change } : u)));

  async function add(file: File, kind: api.ChannelMediaKind = mediaKindFor(file), known: { durationMs?: number } = {}) {
    const key = nextKey++;
    setUploads((list) => (list.length >= max ? list : [...list, { key, name: file.name, kind, size: file.size, progress: 0, mediaId: null, error: null }]));
    try {
      const probed = { ...(await probeMedia(file, kind)), ...known };
      const started = await start({
        kind,
        mime: file.type || "application/octet-stream",
        size: file.size,
        filename: file.name,
        durationMs: probed.durationMs ?? null,
        width: probed.width ?? null,
        height: probed.height ?? null,
      });
      await api.uploadChannelMedia(started.uploadUrl, file, (progress) => patch(key, { progress }));
      patch(key, { mediaId: started.id, progress: 1 });
    } catch (err) {
      patch(key, { error: err instanceof api.ApiError ? err.message : "The upload failed." });
    }
  }

  return {
    uploads,
    add,
    remove: (key: number) => setUploads((list) => list.filter((u) => u.key !== key)),
    clear: () => setUploads([]),
    busy: uploads.some((u) => !u.mediaId && !u.error),
    ready: uploads.filter((u) => u.mediaId).map((u) => u.mediaId!),
    full: uploads.length >= max,
  };
}

const KIND_LABEL: Record<api.ChannelMediaKind, string> = {
  photo: "Photo",
  video: "Video",
  file: "File",
  audio: "Audio",
  voice: "Voice clip",
  round: "Round video",
};

export function UploadList({ uploads, onRemove }: { uploads: Upload[]; onRemove: (key: number) => void }) {
  if (uploads.length === 0) return null;
  return (
    <ul className={styles.uploads}>
      {uploads.map((u) => (
        <li key={u.key} className={styles.upload}>
          <span className={styles.uploadName}>
            {KIND_LABEL[u.kind]}: {u.name} · {formatBytes(u.size)}
          </span>
          {u.error ? (
            <span className={styles.error}>{u.error}</span>
          ) : u.mediaId ? (
            <span className={styles.uploadDone}>
              <CheckIcon size={14} /> Ready
            </span>
          ) : (
            <progress className={styles.progress} value={u.progress} max={1} aria-label={`Uploading ${u.name}`} />
          )}
          <button type="button" className={styles.linkButton} onClick={() => onRemove(u.key)} aria-label={`Remove ${u.name}`}>
            Remove
          </button>
        </li>
      ))}
    </ul>
  );
}

const MARKERS: { label: string; title: string; marker: string }[] = [
  { label: "B", title: "Bold (*text*)", marker: "*" },
  { label: "I", title: "Italic (_text_)", marker: "_" },
  { label: "S", title: "Strikethrough (~text~)", marker: "~" },
  { label: "</>", title: "Code (`text`)", marker: "`" },
  { label: "||", title: "Spoiler (||text||)", marker: "||" },
];

type Extra = null | "poll" | "quiz" | "checklist";

/**
 * The owner's composer, at the foot of the channel like a chat's: the message
 * row, and behind + the formatting toolbar and one of photos/videos/files/
 * recordings, a poll, a quiz or a checklist; posted now or scheduled. Enter
 * adds a line, since a post goes out to everyone: Ctrl+Enter posts.
 */
export function Composer({ channel, onPosted }: { channel: api.ChannelDto; onPosted: (post: api.ChannelPostDto, scheduled: boolean) => void }) {
  const [body, setBody] = useState("");
  const [extra, setExtra] = useState<Extra>(null);
  const [recording, setRecording] = useState<null | "voice" | "round">(null);
  const [question, setQuestion] = useState("");
  const [options, setOptions] = useState(["", ""]);
  const [multiple, setMultiple] = useState(false);
  const [correct, setCorrect] = useState(0);
  const [explanation, setExplanation] = useState("");
  const [listTitle, setListTitle] = useState("");
  const [items, setItems] = useState([""]);
  const [collaborative, setCollaborative] = useState(false);
  const [scheduling, setScheduling] = useState(false);
  const [moreOpen, setMoreOpen] = useState(false);
  const [publishAt, setPublishAt] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const text = useRef<HTMLTextAreaElement>(null);
  const mediaInput = useRef<HTMLInputElement>(null);
  const fileInput = useRef<HTMLInputElement>(null);
  const uploads = useUploads((meta) => api.startChannelMedia(channel.id, meta), 10);
  const hasMedia = uploads.uploads.length > 0;

  function format(marker: string) {
    const el = text.current;
    if (!el) return;
    const next = wrapSelection(body, el.selectionStart, el.selectionEnd, marker);
    setBody(next.text);
    requestAnimationFrame(() => {
      el.focus();
      el.setSelectionRange(next.start, next.end);
    });
  }

  function chooseExtra(next: Extra) {
    setError(null);
    setExtra(extra === next ? null : next);
  }

  function reset() {
    setBody("");
    setExtra(null);
    setQuestion("");
    setOptions(["", ""]);
    setMultiple(false);
    setCorrect(0);
    setExplanation("");
    setListTitle("");
    setItems([""]);
    setCollaborative(false);
    setScheduling(false);
    setPublishAt("");
    setMoreOpen(false);
    uploads.clear();
  }

  async function submit(e?: FormEvent) {
    e?.preventDefault();
    setBusy(true);
    setError(null);
    try {
      const post: api.NewChannelPost = { body };
      if (extra === "poll" || extra === "quiz") {
        post.poll = {
          question,
          options: options.map((o) => o.trim()).filter(Boolean),
          multiple: extra === "poll" && multiple,
          quiz: extra === "quiz" ? { correct, explanation } : null,
        };
      } else if (extra === "checklist") {
        post.checklist = { title: listTitle, items: items.map((i) => i.trim()).filter(Boolean), collaborative };
      } else if (uploads.ready.length) {
        post.media = uploads.ready;
      }
      const scheduledFor = scheduling && publishAt ? new Date(publishAt) : null;
      if (scheduledFor && scheduledFor.getTime() > Date.now()) post.publishAt = scheduledFor.toISOString();
      const created = await api.postToChannel(channel.id, post);
      onPosted(created, Boolean(post.publishAt));
      reset();
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : "Couldn't post. Try again.");
    } finally {
      setBusy(false);
    }
  }

  const canPost = !busy && !uploads.busy && !!(body.trim() || extra || uploads.ready.length) && !(hasMedia && uploads.ready.length === 0 && !body.trim());

  const submitLabel = busy ? "Posting…" : scheduling && publishAt ? "Schedule post" : "Post";

  return (
    <form className={styles.chatComposer} onSubmit={submit}>
      {recording === "voice" && (
        <VoiceRecorder
          onCancel={() => setRecording(null)}
          onDone={(file, durationMs) => {
            setRecording(null);
            void uploads.add(file, "voice", { durationMs });
          }}
        />
      )}
      {recording === "round" && (
        <RoundRecorder
          onCancel={() => setRecording(null)}
          onDone={(file, durationMs) => {
            setRecording(null);
            void uploads.add(file, "round", { durationMs });
          }}
        />
      )}

      <UploadList uploads={uploads.uploads} onRemove={uploads.remove} />

      {(extra === "poll" || extra === "quiz") && (
        <div className={styles.builder} role="group" aria-label={extra === "quiz" ? "Quiz" : "Poll"}>
          <input className={styles.input} placeholder={extra === "quiz" ? "Quiz question" : "Poll question"} aria-label="Question" maxLength={300} value={question} onChange={(e) => setQuestion(e.target.value)} />
          {options.map((option, i) => (
            <div key={i} className={styles.optionRow}>
              {extra === "quiz" && (
                <input type="radio" name="correct" checked={correct === i} onChange={() => setCorrect(i)} aria-label={`Option ${i + 1} is the right answer`} />
              )}
              <input
                className={styles.input}
                placeholder={`Option ${i + 1}`}
                aria-label={`Option ${i + 1}`}
                maxLength={100}
                value={option}
                onChange={(e) => setOptions(options.map((o, j) => (j === i ? e.target.value : o)))}
              />
              {options.length > 2 && (
                <button
                  type="button"
                  className={styles.linkButton}
                  onClick={() => {
                    setOptions(options.filter((_, j) => j !== i));
                    if (correct >= i && correct > 0) setCorrect(correct - 1);
                  }}
                >
                  Remove
                </button>
              )}
            </div>
          ))}
          {options.length < 10 && (
            <button type="button" className={styles.linkButton} onClick={() => setOptions([...options, ""])}>
              Add an option
            </button>
          )}
          {extra === "poll" && (
            <label className={styles.check}>
              <input type="checkbox" checked={multiple} onChange={(e) => setMultiple(e.target.checked)} /> Let people pick more than one
            </label>
          )}
          {extra === "quiz" && (
            <input className={styles.input} placeholder="Why it's right (optional, shown after answering)" aria-label="Explanation" maxLength={200} value={explanation} onChange={(e) => setExplanation(e.target.value)} />
          )}
        </div>
      )}

      {extra === "checklist" && (
        <div className={styles.builder} role="group" aria-label="Checklist">
          <input className={styles.input} placeholder="Checklist title" aria-label="Checklist title" maxLength={200} value={listTitle} onChange={(e) => setListTitle(e.target.value)} />
          {items.map((item, i) => (
            <div key={i} className={styles.optionRow}>
              <input
                className={styles.input}
                placeholder={`Item ${i + 1}`}
                aria-label={`Item ${i + 1}`}
                maxLength={200}
                value={item}
                onChange={(e) => setItems(items.map((x, j) => (j === i ? e.target.value : x)))}
              />
              {items.length > 1 && (
                <button type="button" className={styles.linkButton} onClick={() => setItems(items.filter((_, j) => j !== i))}>
                  Remove
                </button>
              )}
            </div>
          ))}
          {items.length < 30 && (
            <button type="button" className={styles.linkButton} onClick={() => setItems([...items, ""])}>
              Add an item
            </button>
          )}
          <label className={styles.check}>
            <input type="checkbox" checked={collaborative} onChange={(e) => setCollaborative(e.target.checked)} /> Followers can tick items too
          </label>
        </div>
      )}

      {scheduling && (
        <label className={styles.schedule}>
          <CalendarIcon size={16} /> Post at
          <input type="datetime-local" className={styles.input} aria-label="Post at" value={publishAt} onChange={(e) => setPublishAt(e.target.value)} />
        </label>
      )}

      {error && <p className={styles.error}>{error}</p>}

      <input
        ref={mediaInput}
        type="file"
        accept="image/*,video/*,audio/*"
        multiple
        className={styles.hiddenInput}
        aria-label="Add photos, videos or audio"
        onChange={(e) => {
          for (const file of Array.from(e.target.files ?? [])) void uploads.add(file);
          e.target.value = "";
        }}
      />
      <input
        ref={fileInput}
        type="file"
        multiple
        className={styles.hiddenInput}
        aria-label="Add files"
        onChange={(e) => {
          for (const file of Array.from(e.target.files ?? [])) void uploads.add(file, "file");
          e.target.value = "";
        }}
      />

      {moreOpen && (
        <div className={styles.morePanel}>
          <div className={styles.toolbar} role="toolbar" aria-label="Formatting">
            {MARKERS.map((m) => (
              <button key={m.marker} type="button" className={styles.tool} title={m.title} aria-label={m.title} onClick={() => format(m.marker)}>
                {m.label}
              </button>
            ))}
            <span className={styles.toolHint}>Links are made automatically</span>
          </div>
          <div className={styles.attach}>
            <button type="button" className={styles.chip} disabled={!!extra || uploads.full} onClick={() => mediaInput.current?.click()}>
              <ImageIcon size={16} /> Photo, video or audio
            </button>
            <button type="button" className={styles.chip} disabled={!!extra || uploads.full} onClick={() => fileInput.current?.click()}>
              <FileIcon size={16} /> File
            </button>
            <button type="button" className={styles.chip} disabled={!!extra || uploads.full || !!recording} onClick={() => setRecording("voice")}>
              <MicIcon size={16} /> Voice
            </button>
            <button type="button" className={styles.chip} disabled={!!extra || uploads.full || !!recording} onClick={() => setRecording("round")}>
              <VideoIcon size={16} /> Round video
            </button>
            <button type="button" className={`${styles.chip} ${extra === "poll" ? styles.chipOn : ""}`} disabled={hasMedia} onClick={() => chooseExtra("poll")}>
              Poll
            </button>
            <button type="button" className={`${styles.chip} ${extra === "quiz" ? styles.chipOn : ""}`} disabled={hasMedia} onClick={() => chooseExtra("quiz")}>
              Quiz
            </button>
            <button type="button" className={`${styles.chip} ${extra === "checklist" ? styles.chipOn : ""}`} disabled={hasMedia} onClick={() => chooseExtra("checklist")}>
              Checklist
            </button>
            <button type="button" className={`${styles.chip} ${scheduling ? styles.chipOn : ""}`} onClick={() => setScheduling(!scheduling)}>
              <CalendarIcon size={16} /> Schedule
            </button>
          </div>
        </div>
      )}

      <div className={styles.chatRow}>
        <button
          type="button"
          className={`${styles.chatIcon} ${moreOpen ? styles.chatIconOn : ""}`}
          aria-label="Attach, format and more"
          aria-expanded={moreOpen}
          title="Attach, format and more"
          onClick={() => setMoreOpen(!moreOpen)}
        >
          <PlusIcon />
        </button>
        <textarea
          ref={text}
          className={styles.chatInput}
          placeholder="Write a post for everyone following this channel"
          aria-label="New post"
          maxLength={4000}
          rows={1}
          value={body}
          onChange={(e) => setBody(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter" && (e.ctrlKey || e.metaKey) && canPost) {
              e.preventDefault();
              void submit();
            }
          }}
        />
        <button type="submit" className={styles.chatSend} disabled={!canPost} aria-label={submitLabel} title={submitLabel}>
          <SendIcon />
        </button>
      </div>
      <div className={styles.chatHint}>
        <span>
          Posted as {channel.name}
          {uploads.busy ? " · uploading…" : ""}
        </span>
        <span>{body.length}/4000</span>
      </div>
    </form>
  );
}

