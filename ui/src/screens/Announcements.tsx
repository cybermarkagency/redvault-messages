import { useEffect, useState } from "react";
import { TopBar } from "../components/TopBar.js";
import * as api from "../lib/api.js";
import { announcementsChanged } from "../lib/useUnreadAnnouncements.js";
import styles from "./Announcements.module.css";

const when = (iso: string) => new Date(iso).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });

/** Messages and polls the admin sent to this account. Plain text, and labelled so: unlike chats they aren't end-to-end encrypted. */
export function Announcements() {
  const [items, setItems] = useState<api.AnnouncementDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const list = await api.listAnnouncements();
        if (cancelled) return;
        setItems(list);
        // Opening the inbox reads everything in it.
        const unread = list.filter((a) => !a.read);
        if (unread.length > 0) {
          await Promise.all(unread.map((a) => api.markAnnouncementRead(a.id).catch(() => {})));
          announcementsChanged();
        }
      } catch {
        if (!cancelled) setError("Couldn't load your inbox.");
      }
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  return (
    <div className={styles.screen}>
      <TopBar title="Inbox" />
      <div className={styles.body}>
        <p className={`${styles.notice} type-secondary`}>
          Messages from the Redvaultsystem team. These aren't end-to-end encrypted like your chats, so the server can read them.
        </p>
        {error && <p className={styles.error}>{error}</p>}
        {items === null && !error && <p className={`${styles.hint} type-secondary`}>Loading…</p>}
        {items?.length === 0 && <p className={`${styles.hint} type-secondary`}>Nothing here yet.</p>}
        {items?.map((item) => <Card key={item.id} item={item} wasUnread={!item.read} />)}
      </div>
    </div>
  );
}

function Card({ item, wasUnread }: { item: api.AnnouncementDto; wasUnread: boolean }) {
  const [myOptionId, setMyOptionId] = useState(item.myOptionId);
  const [replies, setReplies] = useState<api.AnnouncementReplyDto[]>([]);
  const [draft, setDraft] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!item.allowReplies) return;
    let cancelled = false;
    api
      .getAnnouncement(item.id)
      .then((detail) => {
        if (!cancelled) setReplies(detail.replies);
      })
      .catch(() => {});
    return () => {
      cancelled = true;
    };
  }, [item.id, item.allowReplies]);

  async function vote(optionId: string) {
    if (busy || myOptionId) return;
    setBusy(true);
    setError(null);
    try {
      await api.voteInAnnouncement(item.id, optionId);
      setMyOptionId(optionId);
    } catch (err) {
      if (err instanceof api.ApiError && err.code === "already_voted") setError("You've already voted in this poll.");
      else setError("Couldn't record your vote. Try again.");
    } finally {
      setBusy(false);
    }
  }

  async function sendReply() {
    const body = draft.trim();
    if (!body || busy) return;
    setBusy(true);
    setError(null);
    try {
      const reply = await api.replyToAnnouncement(item.id, body);
      setReplies((current) => [...current, reply]);
      setDraft("");
    } catch (err) {
      setError(err instanceof api.ApiError && err.status === 403 ? "Replies are turned off for this message." : "Couldn't send your reply. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <article className={`${styles.card} ${wasUnread ? styles.cardUnread : ""}`}>
      <div className={`${styles.meta} type-caption`}>
        <span>{item.kind === "poll" ? "POLL" : "MESSAGE"}</span>
        <span>{when(item.createdAt)}</span>
      </div>
      <p className={`${styles.text} type-row-primary`}>{item.body}</p>

      {item.kind === "poll" &&
        item.options.map((o) =>
          myOptionId ? (
            <div key={o.id} className={`${styles.option} ${o.id === myOptionId ? styles.optionChosen : ""}`}>
              <span>{o.label}</span>
              {o.id === myOptionId && <span>Your vote</span>}
            </div>
          ) : (
            <button key={o.id} type="button" className={styles.option} disabled={busy} onClick={() => vote(o.id)}>
              {o.label}
            </button>
          )
        )}
      {item.kind === "poll" && !myOptionId && <p className={`${styles.hint} type-secondary`}>Pick one. Votes can't be changed.</p>}

      {item.allowReplies ? (
        <>
          {replies.length > 0 && (
            <div className={styles.replies}>
              {replies.map((r) => (
                <div key={r.id} className={styles.reply}>
                  {r.body}
                </div>
              ))}
            </div>
          )}
          <div className={styles.replyForm}>
            <textarea
              className={styles.input}
              rows={1}
              placeholder="Reply to the team"
              aria-label="Reply to the team"
              maxLength={2000}
              value={draft}
              onChange={(e) => setDraft(e.target.value)}
            />
            <button type="button" className={styles.primaryButton} disabled={busy || !draft.trim()} onClick={sendReply}>
              Send
            </button>
          </div>
        </>
      ) : (
        <p className={`${styles.hint} type-secondary`}>Replies are turned off for this one.</p>
      )}
      {error && <p className={styles.error}>{error}</p>}
    </article>
  );
}
