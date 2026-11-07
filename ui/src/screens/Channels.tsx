import { type FormEvent, useEffect, useState } from "react";
import { ChannelAvatar } from "../components/ChannelAvatar.js";
import { TopBar } from "../components/TopBar.js";
import * as api from "../lib/api.js";
import { channelAccessToken, hasUnread, openChannelSecret } from "../lib/channels.js";
import { notificationsSupported } from "../lib/channelWatch.js";
import { useLocalPrefs } from "../lib/localPrefs.js";
import { useAppState } from "../state/AppStateContext.js";
import { tierAtLeast } from "../types.js";
import styles from "./Channels.module.css";

type Tab = "following" | "discover" | "yours";

const when = (iso: string) => new Date(iso).toLocaleDateString(undefined, { day: "numeric", month: "short", year: "numeric" });

/**
 * Channels: public, one-way. Following (kept on this device), Discover (the
 * public directory) and Yours (Premium members create channels here).
 */
export function Channels() {
  const { state } = useAppState();
  const [prefs] = useLocalPrefs(state.identity?.userId);
  const [tab, setTab] = useState<Tab>(prefs.followedChannels.length > 0 ? "following" : "discover");

  return (
    <div className={styles.screen}>
      <TopBar title="Channels" />
      <div className={styles.body}>
        <p className={`${styles.notice} type-secondary`}>
          Channels aren't end-to-end encrypted like your chats: anyone can read a public channel, and anyone with the link can read a
          private one. Who follows a channel is kept only on each person's device, so there's no limit on followers.
        </p>
        <NotificationPrompt />
        <div className={styles.tabs} role="tablist" aria-label="Channels">
          {(
            [
              ["following", "Following"],
              ["discover", "Discover"],
              ["yours", "Yours"],
            ] as const
          ).map(([key, label]) => (
            <button key={key} type="button" role="tab" aria-selected={tab === key} className={styles.tab} onClick={() => setTab(key)}>
              {label}
            </button>
          ))}
        </div>
        {tab === "following" && <Following onDiscover={() => setTab("discover")} />}
        {tab === "discover" && <Discover />}
        {tab === "yours" && <Yours />}
      </div>
    </div>
  );
}

/** Asks once for permission to show new-post notifications (checked on this device; see lib/channelWatch.ts). */
function NotificationPrompt() {
  const [permission, setPermission] = useState<NotificationPermission | "unsupported">(() =>
    notificationsSupported() ? Notification.permission : "unsupported"
  );
  if (permission === "unsupported" || permission === "granted") return null;
  if (permission === "denied") {
    return <p className="type-secondary">Notifications for new posts are blocked for this site in your browser's settings.</p>;
  }
  return (
    <div className={styles.notice}>
      <span>Get a notification when a channel you follow posts, while RedVault is open in a tab.</span>{" "}
      <button
        type="button"
        className={styles.linkButton}
        onClick={() => {
          void Notification.requestPermission().then(setPermission);
        }}
      >
        Turn on notifications
      </button>
    </div>
  );
}

function ChannelRow({ channel, unread = false, access = null }: { channel: api.ChannelDto; unread?: boolean; access?: string | null }) {
  const { dispatch } = useAppState();
  return (
    <button type="button" className={styles.row} onClick={() => dispatch({ type: "OPEN_CHANNELS", channelId: channel.id })}>
      <ChannelAvatar name={channel.name} avatarUrl={channel.avatarUrl} access={access} size={40} />
      <span className={styles.rowText}>
        <span className={`${styles.rowTitle} type-row-primary`}>
          {channel.name}
          {channel.isPrivate && <span className={`${styles.privateTag} type-caption`}>Private</span>}
          {unread && <span className={styles.dot} aria-label="New posts" />}
        </span>
        {channel.description && <span className={`${styles.rowDesc} type-secondary`}>{channel.description}</span>}
      </span>
      <span className={`${styles.rowMeta} type-caption`}>{channel.lastPostAt ? when(channel.lastPostAt) : "No posts yet"}</span>
    </button>
  );
}

function Following({ onDiscover }: { onDiscover: () => void }) {
  const { state } = useAppState();
  const [prefs, update] = useLocalPrefs(state.identity?.userId);
  const followed = prefs.followedChannels;
  /** id -> the channel, or null once it's known to be gone. */
  const [channels, setChannels] = useState<Map<string, api.ChannelDto | null>>(new Map());
  const ids = followed.map((f) => f.id).join(",");

  useEffect(() => {
    let cancelled = false;
    Promise.all(
      followed.map(async (f) => {
        try {
          return [f.id, await api.getChannel(f.id, f.secret ? channelAccessToken(f.secret) : null)] as const;
        } catch (err) {
          return [f.id, err instanceof api.ApiError && err.status === 404 ? null : undefined] as const;
        }
      })
    ).then((results) => {
      if (cancelled) return;
      const next = new Map<string, api.ChannelDto | null>();
      for (const [id, channel] of results) if (channel !== undefined) next.set(id, channel);
      setChannels(next);
    });
    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ids]);

  if (followed.length === 0) {
    return (
      <div className={styles.empty}>
        <p className="type-secondary">You don't follow any channels yet.</p>
        <button type="button" className={styles.secondaryButton} onClick={onDiscover}>
          Discover channels
        </button>
      </div>
    );
  }

  return (
    <div className={styles.list}>
      {followed.map((f) => {
        const channel = channels.get(f.id);
        if (channel === null) {
          return (
            <div key={f.id} className={styles.row}>
              <span className={styles.rowText}>
                <span className={`${styles.rowTitle} type-row-primary`}>{f.name}</span>
                <span className={`${styles.rowDesc} type-secondary`}>This channel was deleted, or made private and you don't have its new link.</span>
              </span>
              <button
                type="button"
                className={styles.linkButton}
                onClick={() => update({ followedChannels: followed.filter((x) => x.id !== f.id) })}
              >
                Remove
              </button>
            </div>
          );
        }
        const access = f.secret ? channelAccessToken(f.secret) : null;
        return channel ? (
          <ChannelRow key={f.id} channel={channel} unread={hasUnread(f, channel)} access={access} />
        ) : (
          <ChannelRow key={f.id} channel={{ id: f.id, name: f.name, description: "", createdAt: "", lastPostAt: null, isPrivate: !!f.secret, avatarUrl: null, acceptSuggestions: false }} />
        );
      })}
    </div>
  );
}

function Discover() {
  const [query, setQuery] = useState("");
  const [results, setResults] = useState<api.ChannelDto[] | null>(null);
  const [more, setMore] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    const timer = setTimeout(() => {
      api
        .searchChannels(query)
        .then((found) => {
          if (cancelled) return;
          setResults(found);
          setMore(found.length === 30);
          setError(null);
        })
        .catch(() => {
          if (!cancelled) setError("Couldn't load channels. Try again.");
        });
    }, 250);
    return () => {
      cancelled = true;
      clearTimeout(timer);
    };
  }, [query]);

  async function loadMore() {
    if (!results) return;
    const next = await api.searchChannels(query, results.length).catch(() => []);
    setResults([...results, ...next]);
    setMore(next.length === 30);
  }

  return (
    <>
      <input
        className={styles.input}
        type="search"
        placeholder="Search channels"
        aria-label="Search channels"
        value={query}
        onChange={(e) => setQuery(e.target.value)}
      />
      {error && <p className={styles.error}>{error}</p>}
      {results === null && !error && <p className="type-secondary">Loading…</p>}
      {results?.length === 0 && <p className="type-secondary">{query.trim() ? "No channels match." : "No channels yet."}</p>}
      <div className={styles.list}>
        {results?.map((c) => (
          <ChannelRow key={c.id} channel={c} />
        ))}
      </div>
      {more && (
        <button type="button" className={styles.secondaryButton} onClick={loadMore}>
          Show more
        </button>
      )}
    </>
  );
}

function Yours() {
  const { state, dispatch } = useAppState();
  const premium = !!state.identity && tierAtLeast(state.identity.subscriptionTier, "premium");
  const [mine, setMine] = useState<api.OwnChannelDto[] | null>(null);
  const identity = state.identity;
  /** A private channel's photo needs its link's token; the owner's devices unseal the secret. */
  const accessFor = (c: api.OwnChannelDto) => {
    const secret = identity?.publicKey ? openChannelSecret(c.secretSealed, identity.publicKey, identity.privateKey) : null;
    return secret ? channelAccessToken(secret) : null;
  };
  const [creating, setCreating] = useState(false);
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    api
      .listMyChannels()
      .then((list) => {
        if (!cancelled) setMine(list);
      })
      .catch(() => {
        if (!cancelled) setMine([]);
      });
    return () => {
      cancelled = true;
    };
  }, []);

  async function create(e: FormEvent) {
    e.preventDefault();
    setBusy(true);
    setError(null);
    try {
      const channel = await api.createChannel(name, description);
      dispatch({ type: "OPEN_CHANNELS", channelId: channel.id });
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : "Couldn't create the channel. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <>
      {!premium ? (
        <div className={styles.empty}>
          <p className="type-secondary">Creating a channel needs Premium. Anyone can follow channels on any plan.</p>
          <button type="button" className={styles.secondaryButton} onClick={() => dispatch({ type: "OPEN_SETTINGS", section: "account" })}>
            See plans
          </button>
        </div>
      ) : creating ? (
        <form className={styles.form} onSubmit={create}>
          <input className={styles.input} placeholder="Channel name" aria-label="Channel name" maxLength={80} value={name} onChange={(e) => setName(e.target.value)} autoFocus />
          <textarea
            className={styles.textarea}
            placeholder="What's it about? (optional)"
            aria-label="Description"
            maxLength={500}
            rows={3}
            value={description}
            onChange={(e) => setDescription(e.target.value)}
          />
          <p className="type-secondary">
            Your channel starts public: anyone can find it and read everything you post, with no limit on followers. You can make it
            private later. Posts show the channel's name and photo, never who runs it.
          </p>
          {error && <p className={styles.error}>{error}</p>}
          <div className={styles.buttons}>
            <button type="submit" className={styles.primaryButton} disabled={busy || !name.trim()}>
              {busy ? "Creating…" : "Create channel"}
            </button>
            <button type="button" className={styles.secondaryButton} disabled={busy} onClick={() => setCreating(false)}>
              Cancel
            </button>
          </div>
        </form>
      ) : (
        <button type="button" className={styles.primaryButton} onClick={() => setCreating(true)}>
          New channel
        </button>
      )}
      {mine === null && <p className="type-secondary">Loading…</p>}
      {mine?.length === 0 && premium && !creating && <p className="type-secondary">You haven't made a channel yet.</p>}
      <div className={styles.list}>
        {mine?.map((c) => (
          <ChannelRow key={c.id} channel={c} access={c.isPrivate ? accessFor(c) : null} />
        ))}
      </div>
    </>
  );
}
