import { type FormEvent, useCallback, useEffect, useRef, useState } from "react";
import * as api from "../../lib/api.js";
import { FormattedText } from "../FormattedText.js";
import { ImageIcon } from "../icons24.js";
import styles from "./Composer.module.css";
import { UploadList, useUploads } from "./Composer.js";
import { MediaGallery } from "./PostContent.js";

const when = (iso: string) => new Date(iso).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
const STATUS: Record<api.ChannelSuggestionDto["status"], string> = { pending: "Waiting for the owner", approved: "Published", rejected: "Not published" };

/** Owner: suggestions waiting, each to publish (text editable, now or scheduled) or turn down. Who sent them is never shown. */
export function SuggestionReview({ channelId, onPublished }: { channelId: string; onPublished: (post: api.ChannelPostDto, scheduled: boolean) => void }) {
  const [list, setList] = useState<api.ChannelSuggestionDto[] | null>(null);
  const [editing, setEditing] = useState<{ id: string; body: string; publishAt: string } | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(() => {
    api
      .listChannelSuggestions(channelId)
      .then(setList)
      .catch(() => setList([]));
  }, [channelId]);
  useEffect(load, [load]);

  async function run(action: () => Promise<void>) {
    setBusy(true);
    setError(null);
    try {
      await action();
      load();
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : "Something went wrong. Try again.");
    } finally {
      setBusy(false);
    }
  }

  if (!list || list.length === 0) return null;
  return (
    <details className={styles.review} aria-label="Suggested posts">
      <summary className="type-row-primary">Suggested posts ({list.length})</summary>
      <p className="type-secondary">From followers. Published, they appear as the channel's, marked as suggested; who sent them is never shown.</p>
      {error && <p className={styles.error}>{error}</p>}
      {list.map((s) => (
        <div key={s.id} className={styles.builder}>
          {s.media.length > 0 && <MediaGallery media={s.media} />}
          {editing?.id === s.id ? (
            <>
              <textarea
                className={styles.textarea}
                aria-label="Edit before publishing"
                maxLength={4000}
                rows={3}
                value={editing.body}
                onChange={(e) => setEditing({ ...editing, body: e.target.value })}
              />
              <label className={styles.schedule}>
                Post at (optional)
                <input type="datetime-local" className={styles.input} aria-label="Post at" value={editing.publishAt} onChange={(e) => setEditing({ ...editing, publishAt: e.target.value })} />
              </label>
              <div className={styles.attach}>
                <button
                  type="button"
                  className={styles.primary}
                  disabled={busy}
                  onClick={() =>
                    run(async () => {
                      const at = editing.publishAt ? new Date(editing.publishAt) : null;
                      const scheduled = !!at && at.getTime() > Date.now();
                      const post = await api.approveChannelSuggestion(channelId, s.id, { body: editing.body, publishAt: scheduled ? at!.toISOString() : null });
                      setEditing(null);
                      onPublished(post, scheduled);
                    })
                  }
                >
                  {editing.publishAt ? "Schedule" : "Publish"}
                </button>
                <button type="button" className={styles.secondary} disabled={busy} onClick={() => setEditing(null)}>
                  Cancel
                </button>
              </div>
            </>
          ) : (
            <>
              {s.body && (
                <div style={{ whiteSpace: "pre-wrap", overflowWrap: "anywhere" }}>
                  <FormattedText text={s.body} />
                </div>
              )}
              <span className="type-caption">Sent {when(s.createdAt)}</span>
              <div className={styles.attach}>
                <button type="button" className={styles.primary} disabled={busy} onClick={() => setEditing({ id: s.id, body: s.body, publishAt: "" })}>
                  Review and publish
                </button>
                <button type="button" className={styles.secondary} disabled={busy} onClick={() => run(() => api.rejectChannelSuggestion(channelId, s.id))}>
                  Turn down
                </button>
              </div>
            </>
          )}
        </div>
      ))}
    </details>
  );
}

/** A follower: suggest a post (text and up to four photos), and see what became of earlier ones. */
export function SuggestPost({ channel, access }: { channel: api.ChannelDto; access: string | null }) {
  const [open, setOpen] = useState(false);
  const [body, setBody] = useState("");
  const [mine, setMine] = useState<api.ChannelSuggestionDto[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [sent, setSent] = useState(false);
  const photos = useRef<HTMLInputElement>(null);
  const uploads = useUploads((meta) => api.startSuggestionMedia(channel.id, meta, access), 4);

  useEffect(() => {
    api
      .listMySuggestions(channel.id)
      .then(setMine)
      .catch(() => {});
  }, [channel.id]);

  async function submit(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError(null);
    try {
      const suggestion = await api.suggestChannelPost(channel.id, { body, media: uploads.ready }, access);
      setMine([suggestion, ...mine]);
      setBody("");
      uploads.clear();
      setOpen(false);
      setSent(true);
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : "Couldn't send your suggestion. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <section className={styles.suggest} aria-label="Suggest a post">
      {!open ? (
        <div className={styles.submitRow}>
          <span className="type-secondary">{sent ? "Sent. The owner will look at it." : "You can suggest a post to the owner."}</span>
          <button type="button" className={styles.secondary} onClick={() => setOpen(true)}>
            Suggest a post
          </button>
        </div>
      ) : (
        <form className={styles.actions} onSubmit={submit}>
          <textarea
            className={styles.textarea}
            placeholder="Your post: fan art, a review, news… *bold*, _italic_ and links work"
            aria-label="Suggested post"
            maxLength={4000}
            rows={3}
            value={body}
            onChange={(e) => setBody(e.target.value)}
          />
          <UploadList uploads={uploads.uploads} onRemove={uploads.remove} />
          <p className="type-secondary">The owner sees your post but not who sent it, and may edit it before publishing.</p>
          {error && <p className={styles.error}>{error}</p>}
          <div className={styles.submitRow}>
            <span className={styles.attach}>
              <input
                ref={photos}
                type="file"
                accept="image/jpeg,image/png,image/gif,image/webp,image/avif,image/heic"
                multiple
                className={styles.hiddenInput}
                aria-label="Add photos"
                onChange={(e) => {
                  for (const file of Array.from(e.target.files ?? [])) void uploads.add(file, "photo");
                  e.target.value = "";
                }}
              />
              <button type="button" className={styles.chip} disabled={uploads.full} onClick={() => photos.current?.click()}>
                <ImageIcon size={16} /> Photos
              </button>
              <button type="button" className={styles.secondary} onClick={() => setOpen(false)}>
                Cancel
              </button>
            </span>
            <button type="submit" className={styles.primary} disabled={busy || uploads.busy || (!body.trim() && uploads.ready.length === 0)}>
              {busy ? "Sending…" : "Send suggestion"}
            </button>
          </div>
        </form>
      )}
      {mine.length > 0 && (
        <details>
          <summary className="type-caption">Your suggestions ({mine.length})</summary>
          <ul className={styles.uploads}>
            {mine.map((s) => (
              <li key={s.id} className={styles.upload}>
                <span className={styles.uploadName}>{s.body || "Photos"}</span>
                <span className="type-caption">{STATUS[s.status]}</span>
              </li>
            ))}
          </ul>
        </details>
      )}
    </section>
  );
}
