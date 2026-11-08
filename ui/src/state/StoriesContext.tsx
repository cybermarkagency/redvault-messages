// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stories for the signed-in account: the feed (refreshed every minute while
// the tab is visible), story settings, and which story viewer or composer is
// open. Every plan can post; see docs/stories.md.
import { createContext, type ReactNode, useCallback, useContext, useEffect, useMemo, useRef, useState } from "react";
import * as api from "../lib/api.js";
import { ApiError } from "../lib/api.js";
import * as crypto from "../lib/e2eCrypto.js";
import * as sapi from "../lib/storiesApi.js";
import {
  clearStoryMedia,
  decodeFeed,
  fillKeyGaps,
  loadStoryPrefs,
  publishStory,
  rememberSeen,
  saveStoryPrefs,
  type AuthorStories,
  type PostStage,
  type Story,
  type StoryDraft,
  type StoryFriend,
} from "../lib/stories.js";
import { DEFAULT_STORY_PREFS, wrapStoryKey, type StoryPrefs } from "../lib/storyCrypto.js";
import { answerableStickers, forgetPublishedTotals, syncTotals } from "../lib/storyInteractions.js";
import { useAppState } from "./AppStateContext.js";

const REFRESH_MS = 60_000;
const KEY_GAP_EVERY_MS = 5 * 60_000;
/** The author's app opens answers and publishes totals this often per story in the background (opening the story does it at once). */
const TOTALS_EVERY_MS = 3 * 60_000;

export type ViewerTarget =
  /** From the tray: this author's stories, then on to the next person's. "me" is your own. */
  | { kind: "feed"; authorId: string | "me" }
  /** A highlight or your archive: just these. */
  | { kind: "list"; stories: Story[]; startIndex: number; ownerId: string; title?: string; highlightId?: string; coverStoryId?: string };

export interface StoriesValue {
  loaded: boolean;
  /** The server has Stories switched on (STORIES_ENABLED). When off the apps show no Stories at all. */
  enabled: boolean;
  mediaEnabled: boolean;
  /** Polls, quizzes, questions and sliders are on (the server says so). */
  interactiveEnabled: boolean;
  /** How long a story may last, in seconds (the server's list). */
  ttlChoices: number[];
  own: Story[];
  /** Tray order, muted people left out. */
  authors: AuthorStories[];
  muted: AuthorStories[];
  friends: Map<string, StoryFriend>;
  prefs: StoryPrefs;
  archiveEnabled: boolean;
  viewer: ViewerTarget | null;
  composerOpen: boolean;
  refresh(): Promise<void>;
  openViewer(target: ViewerTarget): void;
  closeViewer(): void;
  openComposer(): void;
  closeComposer(): void;
  /** Rings go grey; the author learns you saw it only if you share views. */
  markSeen(story: Story): void;
  publish(draft: StoryDraft, onStage?: (stage: PostStage) => void): Promise<void>;
  remove(storyId: string): Promise<void>;
  /** Applies a change to your story settings (merged again if another device saved first). */
  updatePrefs(change: (p: StoryPrefs) => StoryPrefs, archiveEnabled?: boolean): Promise<void>;
}

const StoriesCtx = createContext<StoriesValue | null>(null);

export function useStories(): StoriesValue {
  const v = useContext(StoriesCtx);
  if (!v) throw new Error("useStories needs a StoriesProvider");
  return v;
}

/** For places that may render without stories (tests, pre-sign-in screens). */
export function useStoriesOptional(): StoriesValue | null {
  return useContext(StoriesCtx);
}

export function StoriesProvider({ children }: { children: ReactNode }) {
  const { state } = useAppState();
  const identity = state.authStatus === "signed-in" ? state.identity : null;
  const userId = identity?.userId;
  const [loaded, setLoaded] = useState(false);
  const [enabled, setEnabled] = useState(true);
  const enabledRef = useRef(true);
  const [mediaEnabled, setMediaEnabled] = useState(false);
  const [interactiveEnabled, setInteractiveEnabled] = useState(false);
  const [ttlChoices, setTtlChoices] = useState<number[]>([]);
  const [own, setOwn] = useState<Story[]>([]);
  const [allAuthors, setAllAuthors] = useState<AuthorStories[]>([]);
  const [friends, setFriends] = useState<Map<string, StoryFriend>>(new Map());
  const [prefs, setPrefs] = useState<StoryPrefs>(DEFAULT_STORY_PREFS);
  const [archiveEnabled, setArchiveEnabled] = useState(false);
  const revision = useRef(0);
  const [viewer, setViewer] = useState<ViewerTarget | null>(null);
  const [composerOpen, setComposerOpen] = useState(false);
  const lastGapFill = useRef(0);
  const lastTotals = useRef(new Map<string, number>());
  const interactiveRef = useRef(false);
  interactiveRef.current = interactiveEnabled;
  const prefsRef = useRef(prefs);
  prefsRef.current = prefs;
  const identityRef = useRef(identity);
  identityRef.current = identity;
  const conversationsRef = useRef(state.conversations);
  conversationsRef.current = state.conversations;

  /** Friends with the key their stories are sealed with. A friend whose chat shows a different key is left out. */
  const loadFriends = useCallback(async (): Promise<Map<string, StoryFriend>> => {
    const list = await api.listFriends();
    const chatKeys = new Map<string, Uint8Array>();
    for (const c of conversationsRef.current) if (c.kind === "dm" && c.userId && c.publicKey) chatKeys.set(c.userId, c.publicKey);
    const map = new Map<string, StoryFriend>();
    for (const f of list) {
      let publicKey: Uint8Array;
      try {
        publicKey = crypto.fromBase64(f.publicKey);
      } catch {
        continue;
      }
      const chatKey = chatKeys.get(f.userId);
      if (chatKey && crypto.toBase64(chatKey) !== f.publicKey) continue;
      map.set(f.userId, { userId: f.userId, handle: f.username, avatarUrl: f.avatarUrl, publicKey });
    }
    return map;
  }, []);

  const refresh = useCallback(async () => {
    const me = identityRef.current;
    if (!me || !enabledRef.current) return;
    try {
      await crypto.ready();
      const [feedDto, friendMap] = await Promise.all([sapi.getStoryFeed(), loadFriends()]);
      if (identityRef.current?.userId !== me.userId) return;
      const feed = decodeFeed(feedDto, me, friendMap);
      setFriends(friendMap);
      setOwn(feed.own);
      setAllAuthors(feed.authors);
      setLoaded(true);
      // Stickers whose results everyone sees: open the answers sealed to you and publish the totals.
      if (interactiveRef.current) {
        for (const story of feed.own) {
          if (!answerableStickers(story).some((x) => x.type !== "question" && x.results === "everyone")) continue;
          if (Date.now() - (lastTotals.current.get(story.id) ?? 0) < TOTALS_EVERY_MS) continue;
          lastTotals.current.set(story.id, Date.now());
          syncTotals(me, story, friendMap).catch(() => lastTotals.current.delete(story.id));
        }
      }
      if (Date.now() - lastGapFill.current > KEY_GAP_EVERY_MS) {
        lastGapFill.current = Date.now();
        const ownKeys = new Map(feed.own.map((s) => [s.id, s.key]));
        fillKeyGaps(me, ownKeys, friendMap, prefsRef.current).catch(() => {
          lastGapFill.current = 0;
        });
      }
    } catch {
      // Offline or signed out mid-way: the tray keeps what it had.
    }
  }, [loadFriends]);

  // Sign-in: settings and config first (they decide who gets keys), then the feed.
  useEffect(() => {
    if (!userId) {
      setLoaded(false);
      setOwn([]);
      setAllAuthors([]);
      setFriends(new Map());
      setPrefs(DEFAULT_STORY_PREFS);
      enabledRef.current = true;
      setEnabled(true);
      setInteractiveEnabled(false);
      setTtlChoices([]);
      forgetPublishedTotals();
      lastTotals.current.clear();
      setViewer(null);
      setComposerOpen(false);
      lastGapFill.current = 0;
      clearStoryMedia();
      return;
    }
    let cancelled = false;
    (async () => {
      const me = identityRef.current;
      if (!me) return;
      try {
        await crypto.ready();
        // The settings come first and alone: with Stories off the server answers every other route with 404, which
        // must not swallow the "off" in the settings themselves.
        const config = await sapi.getStoryConfig();
        if (cancelled) return;
        // A server that predates the switch says nothing: that means on.
        enabledRef.current = config.enabled !== false;
        setEnabled(enabledRef.current);
        setMediaEnabled(config.media);
        setInteractiveEnabled(config.interactive === true);
        setTtlChoices(Array.isArray(config.ttlSeconds) ? config.ttlSeconds : []);
        if (enabledRef.current) {
          const loadedPrefs = await loadStoryPrefs(me);
          if (cancelled) return;
          setPrefs(loadedPrefs.prefs);
          setArchiveEnabled(loadedPrefs.archiveEnabled);
          revision.current = loadedPrefs.revision;
        }
      } catch {
        // Defaults until the next try; the feed still loads.
      }
      if (!cancelled) await refresh();
    })();
    const interval = setInterval(() => {
      if (!document.hidden) void refresh();
    }, REFRESH_MS);
    const onVisible = () => {
      if (!document.hidden) void refresh();
    };
    document.addEventListener("visibilitychange", onVisible);
    return () => {
      cancelled = true;
      clearInterval(interval);
      document.removeEventListener("visibilitychange", onVisible);
    };
  }, [userId, refresh]);

  const markSeen = useCallback(
    (story: Story) => {
      const me = identityRef.current;
      if (!me || story.authorId === me.userId || story.seen) return;
      rememberSeen(me.userId, story.id);
      if (prefsRef.current.shareViews) sapi.markStoryViewed(story.id).catch(() => {});
      setAllAuthors((list) =>
        list.map((a) => {
          if (a.author.userId !== story.authorId) return a;
          const stories = a.stories.map((s) => (s.id === story.id ? { ...s, seen: true } : s));
          return { ...a, stories, allSeen: stories.every((s) => s.seen) };
        })
      );
    },
    []
  );

  const publish = useCallback(
    async (draft: StoryDraft, onStage?: (stage: PostStage) => void) => {
      const me = identityRef.current;
      if (!me) throw new Error("Not signed in");
      const friendMap = await loadFriends();
      await publishStory(me, draft, [...friendMap.values()], prefsRef.current, onStage);
      await refresh();
    },
    [loadFriends, refresh]
  );

  const remove = useCallback(
    async (storyId: string) => {
      await sapi.deleteStory(storyId);
      setOwn((list) => list.filter((s) => s.id !== storyId));
      await refresh();
    },
    [refresh]
  );

  const updatePrefs = useCallback(
    async (change: (p: StoryPrefs) => StoryPrefs, archive?: boolean) => {
      const me = identityRef.current;
      if (!me) return;
      const before = prefsRef.current;
      let base = { prefs: before, archiveEnabled, revision: revision.current };
      let saved: Awaited<ReturnType<typeof saveStoryPrefs>> | null = null;
      for (let attempt = 0; attempt < 3 && !saved; attempt++) {
        try {
          saved = await saveStoryPrefs(me, change(base.prefs), archive ?? base.archiveEnabled, base.revision);
        } catch (err) {
          if (!(err instanceof ApiError) || err.status !== 409) throw err;
          base = await loadStoryPrefs(me);
        }
      }
      if (!saved) throw new Error("Couldn't save your story settings. Try again.");
      revision.current = saved.revision;
      setPrefs(saved.prefs);
      setArchiveEnabled(saved.archiveEnabled);

      // Who can see what you've already posted follows the new settings.
      const after = saved.prefs;
      const nowHidden = after.hideFrom.filter((u) => !before.hideFrom.includes(u));
      for (const u of nowHidden) await sapi.revokeStoryAccess(u).catch(() => {});
      const closeIds = own.filter((s) => s.meta.closeFriends).map((s) => s.id);
      if (closeIds.length) {
        const leftClose = before.closeFriends.filter((u) => !after.closeFriends.includes(u));
        for (const u of leftClose) await sapi.revokeStoryAccess(u, closeIds).catch(() => {});
        const joinedClose = after.closeFriends.filter((u) => !before.closeFriends.includes(u) && !after.hideFrom.includes(u));
        for (const u of joinedClose) {
          const friend = friends.get(u);
          if (!friend) continue;
          for (const s of own.filter((x) => x.meta.closeFriends)) {
            const wrappedKey = crypto.toBase64(wrapStoryKey(s.id, s.key, friend.publicKey, me.privateKey));
            await sapi.addStoryKeys(s.id, [{ userId: u, wrappedKey }]).catch(() => {});
          }
        }
      }
      // Un-hidden friends get keys for "friends" stories on the next top-up.
      if (before.hideFrom.some((u) => !after.hideFrom.includes(u))) lastGapFill.current = 0;
      await refresh();
    },
    [archiveEnabled, own, friends, refresh]
  );

  const value = useMemo<StoriesValue>(() => {
    const muted = new Set(prefs.muted);
    return {
      loaded,
      enabled,
      mediaEnabled,
      interactiveEnabled,
      ttlChoices,
      own,
      authors: allAuthors.filter((a) => !muted.has(a.author.userId)),
      muted: allAuthors.filter((a) => muted.has(a.author.userId)),
      friends,
      prefs,
      archiveEnabled,
      viewer,
      composerOpen,
      refresh,
      openViewer: (target) => {
        setComposerOpen(false);
        setViewer(target);
      },
      closeViewer: () => setViewer(null),
      openComposer: () => {
        setViewer(null);
        setComposerOpen(true);
        // Friends added since the last refresh count for "who sees it" and "choose people".
        void refresh();
      },
      closeComposer: () => setComposerOpen(false),
      markSeen,
      publish,
      remove,
      updatePrefs,
    };
  }, [loaded, enabled, mediaEnabled, interactiveEnabled, ttlChoices, own, allAuthors, friends, prefs, archiveEnabled, viewer, composerOpen, refresh, markSeen, publish, remove, updatePrefs]);

  return <StoriesCtx.Provider value={value}>{children}</StoriesCtx.Provider>;
}
