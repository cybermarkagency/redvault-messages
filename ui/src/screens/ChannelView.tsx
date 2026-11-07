import { type ChangeEvent, type FormEvent, useEffect, useLayoutEffect, useRef, useState } from "react";
import { ChannelAvatar } from "../components/ChannelAvatar.js";
import { Composer } from "../components/channel/Composer.js";
import { PostContent } from "../components/channel/PostContent.js";
import { SuggestionReview, SuggestPost } from "../components/channel/Suggestions.js";
import { BellIcon, BellOffIcon, ChevronLeftIcon, CloseIcon, PanelRightIcon } from "../components/icons24.js";
import { ToggleSwitch } from "../components/ToggleSwitch.js";
import { TopBar } from "../components/TopBar.js";
import * as api from "../lib/api.js";
import { readImageAsAvatar } from "../lib/avatar.js";
import { countChannelViews } from "../lib/channelViews.js";
import {
  channelAccessToken,
  channelUrl,
  follow,
  isFollowing,
  markSeen,
  newChannelSecret,
  openChannelSecret,
  sealChannelSecret,
  unfollow,
} from "../lib/channels.js";
import { dayLabel } from "../lib/dayLabel.js";
import { ready } from "../lib/e2eCrypto.js";
import { readPrefs, useLocalPrefs } from "../lib/localPrefs.js";
import { sendChannelForward } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./Channels.module.css";

const PAGE = 30;
const when = (iso: string) => new Date(iso).toLocaleString(undefined, { dateStyle: "medium", timeStyle: "short" });
const clock = (iso: string) => new Date(iso).toLocaleTimeString(undefined, { hour: "numeric", minute: "2-digit" });
const views = (n: number) => `${n.toLocaleString()} ${n === 1 ? "view" : "views"}`;

/**
 * One channel, laid out like a chat: every post since it began as a bubble,
 * oldest first and opening at the newest, each shown as the channel's (its name
 * and photo, never who posted it) with a view count: formatted text, photos,
 * videos, files, recordings, polls, quizzes and checklists. Only the owner
 * posts, so only the owner gets a composer; everyone else gets a read-only bar
 * with Follow, notifications and Suggest a post. Channels aren't end-to-end
 * encrypted, so nothing here borrows a chat's lock or security details. The
 * info panel has the description, Copy link, Report and the owner's settings.
 * A super admin gets all of the owner's tools but the public/private switch
 * and the private link, which only the owner's account can seal and open.
 */
export function ChannelView({ channelId, secret: linkSecretProp }: { channelId: string; secret?: string }) {
  const { state, dispatch } = useAppState();
  const identity = state.identity;
  const [prefs, update] = useLocalPrefs(identity?.userId);
  const [channel, setChannel] = useState<api.ChannelDto | null>(null);
  const [posts, setPosts] = useState<api.ChannelPostDto[]>([]);
  const [older, setOlder] = useState(false);
  const [mine, setMine] = useState(false);
  /** A private channel's link secret, from the link just opened, a followed channel, or the owner's own copy. */
  const [secret, setSecret] = useState<string | null>(null);
  const [scheduled, setScheduled] = useState<api.ChannelPostDto[]>([]);
  const [editing, setEditing] = useState<{ name: string; description: string } | null>(null);
  const [forwarding, setForwarding] = useState<api.ChannelPostDto | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [infoOpen, setInfoOpen] = useState(false);
  const fileInput = useRef<HTMLInputElement>(null);
  const thread = useRef<HTMLDivElement>(null);
  /** Where the thread scrolls after the next change: the newest post, or back to the same post once older ones are added above (the distance from the bottom). */
  const keepScroll = useRef<"newest" | number | null>("newest");
  const isAdmin = identity?.role === "super_admin";
  const canManage = mine || isAdmin;
  const following = isFollowing(prefs.followedChannels, channelId);
  const access = secret ? channelAccessToken(secret) : null;

  useEffect(() => {
    let cancelled = false;
    keepScroll.current = "newest";
    setChannel(null);
    setError(null);
    (async () => {
      await ready();
      const own = (await api.listMyChannels().catch(() => [] as api.OwnChannelDto[])).find((c) => c.id === channelId) ?? null;
      const ownSecret = own && identity?.publicKey ? openChannelSecret(own.secretSealed, identity.publicKey, identity.privateKey) : null;
      const followedSecret = readPrefs(identity?.userId).followedChannels.find((f) => f.id === channelId)?.secret ?? null;
      const known = linkSecretProp ?? ownSecret ?? followedSecret;
      const token = known ? channelAccessToken(known) : null;
      try {
        const [found, firstPage] = await Promise.all([api.getChannel(channelId, token), api.getChannelPosts(channelId, undefined, token)]);
        if (cancelled) return;
        setMine(own !== null);
        if (own || isAdmin) setScheduled(await api.listScheduledPosts(channelId).catch(() => []));
        setSecret(found.isPrivate ? known : null);
        setChannel(found);
        setPosts(firstPage);
        setOlder(firstPage.length === PAGE);
        countChannelViews(firstPage.map((p) => p.id));
      } catch (err) {
        if (cancelled) return;
        setError(
          err instanceof api.ApiError && err.status === 404
            ? `${err.message} A private channel opens only from its private link.`
            : "Couldn't load this channel. Try again."
        );
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [channelId, linkSecretProp, identity]);

  // Opening a followed channel counts its posts as seen (and keeps the private link it was opened with).
  useEffect(() => {
    if (!channel || !isFollowing(prefs.followedChannels, channel.id)) return;
    const entry = prefs.followedChannels.find((f) => f.id === channel.id)!;
    const newSecret = channel.isPrivate && secret && secret !== entry.secret ? secret : null;
    if (entry.seenAt !== channel.lastPostAt || entry.name !== channel.name || newSecret) {
      update({ followedChannels: markSeen(prefs.followedChannels, channel, newSecret) });
    }
  }, [channel, secret, prefs.followedChannels, update]);

  useLayoutEffect(() => {
    const el = thread.current;
    if (!el || keepScroll.current === null) return;
    el.scrollTop = keepScroll.current === "newest" ? el.scrollHeight : el.scrollHeight - keepScroll.current;
    keepScroll.current = null;
  }, [channel, posts]);

  async function run(action: () => Promise<void>, failure: string) {
    setBusy(true);
    setError(null);
    setNotice(null);
    try {
      await action();
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : failure);
    } finally {
      setBusy(false);
    }
  }

  /** A post from the composer (or an approved suggestion): the newest in the thread, or with the scheduled ones. */
  function added(post: api.ChannelPostDto, isScheduled: boolean) {
    if (!channel) return;
    if (isScheduled) {
      setScheduled((list) => [...list, post].sort((a, b) => a.createdAt.localeCompare(b.createdAt)));
      setNotice(`Scheduled for ${when(post.createdAt)}.`);
      return;
    }
    keepScroll.current = "newest";
    setPosts((list) => [post, ...list]);
    setChannel({ ...channel, lastPostAt: post.createdAt });
    countChannelViews([post.id]);
  }

  function replacePost(post: api.ChannelPostDto) {
    setPosts((list) => list.map((p) => (p.id === post.id ? post : p)));
  }

  async function saveDetails(e: FormEvent) {
    e.preventDefault();
    if (!editing) return;
    await run(async () => {
      setChannel(await api.updateChannel(channelId, editing.name, editing.description));
      setEditing(null);
    }, "Couldn't save the changes.");
  }

  async function changePhoto(e: ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0];
    e.target.value = "";
    if (!file) return;
    await run(async () => {
      setChannel(await api.setChannelAvatar(channelId, await readImageAsAvatar(file)));
    }, "Couldn't use that photo.");
  }

  /** Private: a fresh link (the old one stops working). Public: no link needed. */
  async function setPrivate(makePrivate: boolean) {
    if (!channel || !identity) return;
    await run(async () => {
      await ready();
      if (!makePrivate) {
        setChannel(await api.setChannelVisibility(channelId, { isPrivate: false }));
        setSecret(null);
        return;
      }
      const fresh = newChannelSecret();
      const updated = await api.setChannelVisibility(channelId, {
        isPrivate: true,
        access: channelAccessToken(fresh),
        secretSealed: sealChannelSecret(fresh, identity.publicKey),
      });
      setSecret(fresh);
      setChannel(updated);
      if (following) update({ followedChannels: markSeen(prefs.followedChannels, updated, fresh) });
    }, "Couldn't change who can read the channel.");
  }

  async function copy(text: string) {
    try {
      await navigator.clipboard.writeText(text);
      setNotice("Link copied.");
    } catch {
      setNotice(text);
    }
  }

  async function loadOlder() {
    const last = posts[posts.length - 1];
    if (!last) return;
    const page = await api.getChannelPosts(channelId, last.createdAt, access).catch(() => []);
    if (thread.current) keepScroll.current = thread.current.scrollHeight - thread.current.scrollTop;
    setPosts([...posts, ...page]);
    setOlder(page.length === PAGE);
    countChannelViews(page.map((p) => p.id));
  }

  function report() {
    const reason = window.prompt("What's wrong with this channel? Your report goes to the RedVault admins.");
    if (!reason?.trim()) return;
    void run(async () => {
      await api.reportChannel(channelId, reason);
      setNotice("Thanks. The admins will look at it.");
    }, "Couldn't send the report.");
  }

  const back = () => dispatch({ type: "OPEN_CHANNELS" });
  if (!channel) {
    return (
      <div className={styles.screen}>
        <TopBar title="Channel" onBack={back} />
        <div className={styles.body}>{error ? <p className={styles.error}>{error}</p> : <p className="type-secondary">Loading…</p>}</div>
      </div>
    );
  }

  const link = channel.isPrivate ? (secret ? channelUrl(channel.id, secret) : null) : channelUrl(channel.id);
  const followEntry = prefs.followedChannels.find((f) => f.id === channel.id);
  const forwardTargets = state.conversations.filter((c) => c.conversationKey);
  const followButton = (
    <button
      type="button"
      className={following ? styles.secondaryButton : styles.primaryButton}
      aria-pressed={following}
      onClick={() =>
        update({
          followedChannels: following ? unfollow(prefs.followedChannels, channel.id) : follow(prefs.followedChannels, channel, secret),
        })
      }
    >
      {following ? "Following" : "Follow"}
    </button>
  );
  const muteButton = following && (
    <button
      type="button"
      className={styles.secondaryButton}
      aria-pressed={!followEntry?.muted}
      title={followEntry?.muted ? "Notifications are off for this channel" : "You're notified of new posts"}
      onClick={() => update({ followedChannels: prefs.followedChannels.map((f) => (f.id === channel.id ? { ...f, muted: !f.muted } : f)) })}
    >
      {followEntry?.muted ? <BellOffIcon size={16} /> : <BellIcon size={16} />} {followEntry?.muted ? "Muted" : "Notifications on"}
    </button>
  );
  let lastDay = "";

  return (
    <div className={styles.chatLayout}>
      <div className={styles.chatScreen}>
        <header className={styles.chatHeader}>
          <button type="button" className={styles.iconButton} aria-label="Back" onClick={back}>
            <ChevronLeftIcon />
          </button>
          <button type="button" className={styles.headerButton} onClick={() => setInfoOpen(!infoOpen)}>
            <ChannelAvatar name={channel.name} avatarUrl={channel.avatarUrl} access={access} size={42} />
            <span className={styles.headerText}>
              <span className={styles.headerName}>{channel.name}</span>
              <span className={`${styles.headerStatus} type-secondary`}>{channel.isPrivate ? "Private channel" : "Public channel"} · not end-to-end encrypted</span>
            </span>
          </button>
          <button
            type="button"
            className={`${styles.iconButton} ${infoOpen ? styles.iconButtonOn : ""}`}
            aria-label="Channel info"
            aria-pressed={infoOpen}
            title="Channel info"
            onClick={() => setInfoOpen(!infoOpen)}
          >
            <PanelRightIcon />
          </button>
        </header>

        {canManage && (scheduled.length > 0 || channel.acceptSuggestions) && (
          <div className={styles.ownerStrip}>
            {channel.acceptSuggestions && <SuggestionReview channelId={channel.id} onPublished={added} />}
            {scheduled.length > 0 && (
              <details className={styles.stripSection}>
                <summary className="type-row-primary">Scheduled ({scheduled.length})</summary>
                <div className={styles.stripList} role="group" aria-label="Scheduled posts">
                  {scheduled.map((post) => (
                    <article key={post.id} className={styles.scheduledPost}>
                      <span className={`${styles.publicLabel} type-caption`}>Scheduled for {when(post.createdAt)}</span>
                      <PostContent channelId={channel.id} post={post} access={access} isOwner />
                      <div className={styles.buttons}>
                        <button
                          type="button"
                          className={styles.secondaryButton}
                          disabled={busy}
                          onClick={() =>
                            void run(async () => {
                              const published = await api.publishPostNow(channel.id, post.id);
                              setScheduled((list) => list.filter((p) => p.id !== post.id));
                              added(published, false);
                            }, "Couldn't publish it.")
                          }
                        >
                          Publish now
                        </button>
                        <button
                          type="button"
                          className={styles.linkButton}
                          disabled={busy}
                          onClick={() => {
                            if (!window.confirm("Delete this scheduled post?")) return;
                            void run(async () => {
                              await api.deleteChannelPost(channel.id, post.id);
                              setScheduled((list) => list.filter((p) => p.id !== post.id));
                            }, "Couldn't delete it.");
                          }}
                        >
                          Delete
                        </button>
                      </div>
                    </article>
                  ))}
                </div>
              </details>
            )}
          </div>
        )}

        <div ref={thread} className={styles.thread}>
          {older && (
            <button type="button" className={styles.olderButton} onClick={loadOlder}>
              Show older posts
            </button>
          )}
          {posts.length === 0 && <p className={`${styles.threadEmpty} type-secondary`}>No posts yet.</p>}
          {[...posts].reverse().map((post) => {
            const day = dayLabel(new Date(post.createdAt).getTime());
            const separator = day !== lastDay ? <div className={styles.daySeparator}>{day}</div> : null;
            lastDay = day;
            return (
              <div key={post.id}>
                {separator}
                <div className={styles.postRow}>
                  <ChannelAvatar name={channel.name} avatarUrl={channel.avatarUrl} access={access} size={32} />
                  <article className={styles.postBubble}>
                    <div className={styles.postName}>{channel.name}</div>
                    <PostContent channelId={channel.id} post={post} access={access} isOwner={canManage} onChange={replacePost} />
                    <div className={styles.bubbleFoot}>
                      <span className={styles.postActions}>
                        <button type="button" className={styles.linkButton} onClick={() => setForwarding(post)}>
                          Forward
                        </button>
                        {canManage && (
                          <button
                            type="button"
                            className={styles.linkButton}
                            disabled={busy}
                            onClick={() => {
                              if (!window.confirm("Delete this post?")) return;
                              void run(async () => {
                                await api.deleteChannelPost(channel.id, post.id);
                                setPosts((list) => list.filter((p) => p.id !== post.id));
                              }, "Couldn't delete the post.");
                            }}
                          >
                            Delete
                          </button>
                        )}
                      </span>
                      <span className={styles.bubbleMeta}>
                        <time dateTime={post.createdAt} title={when(post.createdAt)}>
                          {clock(post.createdAt)}
                        </time>{" "}
                        · {views(post.views)}
                      </span>
                    </div>
                  </article>
                </div>
              </div>
            );
          })}
        </div>

        {(notice || error) && (
          <div className={styles.threadNotice} role="status">
            {notice && <p className="type-secondary">{notice}</p>}
            {error && <p className={styles.error}>{error}</p>}
          </div>
        )}

        {canManage ? (
          <Composer channel={channel} onPosted={added} />
        ) : (
          <div className={styles.composerWrap}>
            <div className={styles.readerBar} role="group" aria-label="Read-only channel">
              <div className={styles.readerRow}>
                <span className={`${styles.readerText} type-secondary`}>Only the owner posts in this channel.</span>
                {muteButton}
                {followButton}
              </div>
              {channel.acceptSuggestions && <SuggestPost channel={channel} access={access} />}
            </div>
          </div>
        )}
      </div>

      {infoOpen && (
        <aside className={styles.infoPanel} aria-label="Channel info">
          <div className={styles.infoTop}>
            <div className="type-row-primary">Channel info</div>
            <button type="button" className={styles.iconButton} aria-label="Close channel info" onClick={() => setInfoOpen(false)}>
              <CloseIcon />
            </button>
          </div>
          {editing ? (
            <form className={styles.form} onSubmit={saveDetails}>
              <input
                className={styles.input}
                aria-label="Channel name"
                maxLength={80}
                value={editing.name}
                onChange={(e) => setEditing({ ...editing, name: e.target.value })}
              />
              <textarea
                className={styles.textarea}
                aria-label="Description"
                maxLength={500}
                rows={3}
                value={editing.description}
                onChange={(e) => setEditing({ ...editing, description: e.target.value })}
              />
              <div className={styles.buttons}>
                <button type="submit" className={styles.primaryButton} disabled={busy || !editing.name.trim()}>
                  Save
                </button>
                <button type="button" className={styles.secondaryButton} disabled={busy} onClick={() => setEditing(null)}>
                  Cancel
                </button>
              </div>
            </form>
          ) : (
            <div className={styles.header}>
              <div className={styles.headerTop}>
                <ChannelAvatar name={channel.name} avatarUrl={channel.avatarUrl} access={access} size={64} />
                <div className={styles.infoText}>
                  <h2 className={`${styles.channelName} type-title`}>{channel.name}</h2>
                  <p className={`${styles.publicLabel} type-caption`}>
                    {channel.isPrivate
                      ? "Private channel · only people with its link can read it · not end-to-end encrypted"
                      : "Public channel · anyone can read it · not end-to-end encrypted"}
                  </p>
                </div>
              </div>
              {channel.description && <p className={styles.description}>{channel.description}</p>}
              <div className={styles.buttons}>
                {canManage && followButton}
                {canManage && muteButton}
                {link && (
                  <button type="button" className={styles.secondaryButton} onClick={() => copy(link)}>
                    Copy link
                  </button>
                )}
                {canManage && (
                  <>
                    <button type="button" className={styles.secondaryButton} onClick={() => setEditing({ name: channel.name, description: channel.description })}>
                      Edit
                    </button>
                    <button type="button" className={styles.secondaryButton} disabled={busy} onClick={() => fileInput.current?.click()}>
                      {channel.avatarUrl ? "Change photo" : "Add photo"}
                    </button>
                    {channel.avatarUrl && (
                      <button
                        type="button"
                        className={styles.linkButton}
                        disabled={busy}
                        onClick={() =>
                          run(async () => {
                            await api.removeChannelAvatar(channelId);
                            setChannel({ ...channel, avatarUrl: null });
                          }, "Couldn't remove the photo.")
                        }
                      >
                        Remove photo
                      </button>
                    )}
                    <input ref={fileInput} type="file" accept="image/*" className={styles.fileInput} onChange={changePhoto} aria-label="Channel photo" />
                  </>
                )}
                {!canManage && (
                  <button type="button" className={styles.linkButton} disabled={busy} onClick={report}>
                    Report
                  </button>
                )}
                {canManage && (
                  <button
                    type="button"
                    className={styles.dangerButton}
                    disabled={busy}
                    onClick={() => {
                      if (!window.confirm(`Delete "${channel.name}" and all its posts? This can't be undone.`)) return;
                      void run(async () => {
                        await api.deleteChannel(channel.id);
                        update({ followedChannels: unfollow(prefs.followedChannels, channel.id) });
                        back();
                      }, "Couldn't delete the channel.");
                    }}
                  >
                    Delete channel
                  </button>
                )}
              </div>
            </div>
          )}

          {canManage && (
            <div className={styles.visibility}>
              {mine && (
                <div className={styles.toggleRow}>
                  <span>
                    <div className="type-row-primary">Private channel</div>
                    <div className="type-secondary">
                      {channel.isPrivate
                        ? "Not in Discover. Only people with the private link can read it."
                        : "Off: anyone can find it in Discover and read it."}
                    </div>
                  </span>
                  <ToggleSwitch
                    label="Private channel"
                    checked={channel.isPrivate}
                    onChange={(on) => {
                      const ask = on
                        ? "Make this channel private? It leaves Discover, and only people with its private link can read it. Followers who don't have that link lose access."
                        : "Make this channel public? Anyone can find it in Discover and read every post.";
                      if (window.confirm(ask)) void setPrivate(on);
                    }}
                  />
                </div>
              )}
              <div className={styles.toggleRow}>
                <span>
                  <div className="type-row-primary">Suggested posts</div>
                  <div className="type-secondary">
                    {channel.acceptSuggestions
                      ? "Followers can send you posts to review. You see the post, never who sent it."
                      : "Off: only you post in this channel."}
                  </div>
                </span>
                <ToggleSwitch
                  label="Suggested posts"
                  checked={channel.acceptSuggestions}
                  onChange={(on) =>
                    void run(async () => {
                      setChannel(await api.setChannelSettings(channelId, { acceptSuggestions: on }));
                    }, "Couldn't change that setting.")
                  }
                />
              </div>
              {mine && channel.isPrivate && link && (
                <>
                  <div className={styles.linkRow}>
                    <input className={styles.linkField} readOnly value={link} aria-label="Private link" onFocus={(e) => e.currentTarget.select()} />
                    <button type="button" className={styles.primaryButton} onClick={() => copy(link)}>
                      Copy
                    </button>
                  </div>
                  <div>
                    <button
                      type="button"
                      className={styles.linkButton}
                      disabled={busy}
                      onClick={() => {
                        if (window.confirm("Reset the private link? Everyone who has the current link, followers included, loses access until you share the new one.")) {
                          void setPrivate(true);
                        }
                      }}
                    >
                      Reset link
                    </button>
                  </div>
                </>
              )}
            </div>
          )}
        </aside>
      )}

      {forwarding && (
        <div className={styles.overlay} role="dialog" aria-modal="true" aria-label="Forward post">
          <div className={styles.dialog}>
            <div className="type-row-primary">Forward to…</div>
            <p className="type-secondary">The copy in that chat is end-to-end encrypted like the rest of the chat, and shown as a post from {channel.name}. Each copy that's seen counts as a view.</p>
            {forwardTargets.length === 0 && <p className="type-secondary">You don't have a chat to forward to yet.</p>}
            <div className={styles.dialogList}>
              {forwardTargets.map((target) => (
                <button
                  key={target.id}
                  type="button"
                  className={styles.dialogRow}
                  disabled={busy}
                  onClick={() =>
                    run(async () => {
                      await sendChannelForward(target.id, target.conversationKey!, forwarding.body, {
                        channelId: channel.id,
                        postId: forwarding.id,
                        channelName: channel.name,
                      });
                      setForwarding(null);
                      setNotice("Forwarded.");
                    }, "Couldn't forward it. Try again.")
                  }
                >
                  {target.kind === "group" ? target.label : target.handle}
                </button>
              ))}
            </div>
            <button type="button" className={styles.secondaryButton} onClick={() => setForwarding(null)}>
              Cancel
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
