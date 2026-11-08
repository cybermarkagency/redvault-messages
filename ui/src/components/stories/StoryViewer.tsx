// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { ApiError } from "../../lib/api.js";
import * as sapi from "../../lib/storiesApi.js";
import { decodeHighlights, saveHighlight, storyAge, storyMediaUrl, storyPreview, type HighlightView, type Story } from "../../lib/stories.js";
import { captionAt, MAX_HIGHLIGHT_TITLE, type AnswerableSticker, type StickerTotals, type StoryAnswer } from "../../lib/storyCrypto.js";
import {
  answerableStickers,
  isAnonymousSticker,
  loadMyAnswers,
  loadTotals,
  submitAnswer,
  syncTotals,
  tallyAnswers,
  type Inbox,
  type MyAnswer,
} from "../../lib/storyInteractions.js";
import { sendStoryReply } from "../../state/actions.js";
import { useAppState } from "../../state/AppStateContext.js";
import { useStories, type ViewerTarget } from "../../state/StoriesContext.js";
import type { DMConversation } from "../../types.js";
import { Avatar } from "../Avatar.js";
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  CloseIcon,
  DotsIcon,
  EyeIcon,
  HeartIcon,
  PauseIcon,
  PlayIcon,
  SendIcon,
  SpeakerIcon,
  SpeakerOffIcon,
  StarIcon,
} from "../icons24.js";
import { useDialogFocus } from "../useDialogFocus.js";
import { StickerView, stickerStyle } from "./StickerView.js";
import { StoryAnswersSheet } from "./StoryAnswersSheet.js";
import { StoryFrame } from "./StoryFrame.js";
import { StoryReportSheet } from "./StoryReportSheet.js";
import styles from "./Stories.module.css";

/** How long a photo or text story stays up. */
export const STORY_SECONDS = 5;
/** A story with something to answer stays longer: reading a question and choosing takes more than five seconds. */
export const STICKER_STORY_SECONDS = 20;
const HOLD_MS = 220;
export const QUICK_REACTIONS = ["❤️", "😂", "😮", "😢", "👏", "🔥"];

interface Owner {
  userId: string;
  handle: string;
  avatarUrl: string | null;
  isSelf: boolean;
}

interface Group {
  owner: Owner;
  stories: Story[];
}

// Sound stays on or off from one story to the next, like Instagram.
let soundOn = true;

function firstUnseen(stories: Story[]): number {
  const i = stories.findIndex((s) => !s.seen);
  return i < 0 ? 0 : i;
}

type Sheet = "viewers" | "highlight" | "menu" | "answers" | "report" | "link" | null;

const INTERACTION_REFRESH_MS = 20_000;

/** Full-screen story viewer: tap the right side for the next story, the left for the previous, hold to pause. */
export function StoryViewer({ target }: { target: ViewerTarget }) {
  const { state, dispatch } = useAppState();
  const stories = useStories();
  const identity = state.identity!;
  const self: Owner = { userId: identity.userId, handle: identity.handle, avatarUrl: identity.avatarUrl, isSelf: true };

  // The line-up is fixed when the viewer opens, so it doesn't reshuffle as rings go grey.
  const [groups, setGroups] = useState<Group[]>(() => {
    if (target.kind === "list") {
      const f = stories.friends.get(target.ownerId);
      const owner = target.ownerId === identity.userId ? self : { userId: target.ownerId, handle: f?.handle ?? "Someone", avatarUrl: f?.avatarUrl ?? null, isSelf: false };
      return [{ owner, stories: target.stories }];
    }
    if (target.authorId === "me") return [{ owner: self, stories: stories.own }];
    const inTray = stories.authors.some((a) => a.author.userId === target.authorId);
    const list = inTray ? stories.authors : stories.muted.filter((a) => a.author.userId === target.authorId);
    return list.map((a) => ({ owner: { ...a.author, isSelf: false }, stories: a.stories }));
  });
  const [groupIndex, setGroupIndex] = useState(() => {
    if (target.kind === "list" || target.authorId === "me") return 0;
    return Math.max(0, groups.findIndex((g) => g.owner.userId === target.authorId));
  });
  const [storyIndex, setStoryIndex] = useState(() => {
    if (target.kind === "list") return Math.min(target.startIndex, target.stories.length - 1);
    if (target.authorId === "me") return 0;
    const g = groups[Math.max(0, groups.findIndex((x) => x.owner.userId === target.authorId))];
    return g ? firstUnseen(g.stories) : 0;
  });

  const group = groups[groupIndex];
  const story: Story | undefined = group?.stories[storyIndex];
  const close = stories.closeViewer;

  const [mediaUrl, setMediaUrl] = useState<string | null>(null);
  const [ready, setReady] = useState(false);
  const [failed, setFailed] = useState(false);
  const [progress, setProgress] = useState(0);
  const [held, setHeld] = useState(false);
  const [userPaused, setUserPaused] = useState(false);
  const [typing, setTyping] = useState(false);
  const [sheet, setSheet] = useState<Sheet>(null);
  const [muted, setMuted] = useState(!soundOn);
  const [toast, setToast] = useState<string | null>(null);
  const [mine, setMine] = useState<Map<string, MyAnswer>>(new Map());
  const [totals, setTotals] = useState<Map<string, StickerTotals>>(new Map());
  const [inbox, setInbox] = useState<Inbox | null>(null);
  const [busySticker, setBusySticker] = useState<string | null>(null);
  const [stickerFocus, setStickerFocus] = useState(false);
  const [stickerHover, setStickerHover] = useState(false);
  const [pendingLink, setPendingLink] = useState<string | null>(null);
  const elapsed = useRef(0);
  const mediaRef = useRef<HTMLMediaElement>(null);
  const [caption, setCaption] = useState<string | null>(null);
  const pressAt = useRef(0);
  const root = useRef<HTMLDivElement>(null);
  const [tabHidden, setTabHidden] = useState(document.hidden);

  useDialogFocus(root, sheet ? () => setSheet(null) : close);

  useEffect(() => {
    const onVis = () => setTabHidden(document.hidden);
    document.addEventListener("visibilitychange", onVis);
    return () => document.removeEventListener("visibilitychange", onVis);
  }, []);

  const paused = held || userPaused || typing || stickerFocus || stickerHover || sheet !== null || !ready || tabHidden;

  const next = useCallback(() => {
    if (!group) return close();
    if (storyIndex + 1 < group.stories.length) setStoryIndex(storyIndex + 1);
    else if (groupIndex + 1 < groups.length) {
      setGroupIndex(groupIndex + 1);
      setStoryIndex(target.kind === "feed" ? firstUnseen(groups[groupIndex + 1].stories) : 0);
    } else close();
  }, [group, groups, groupIndex, storyIndex, close, target.kind]);

  const prev = useCallback(() => {
    if (storyIndex > 0) setStoryIndex(storyIndex - 1);
    else if (groupIndex > 0) {
      setGroupIndex(groupIndex - 1);
      setStoryIndex(0);
    } else {
      elapsed.current = 0;
      setProgress(0);
      if (mediaRef.current) mediaRef.current.currentTime = 0;
    }
  }, [groupIndex, storyIndex]);

  const nextRef = useRef(next);
  nextRef.current = next;

  // A new story: fetch and decrypt its media (and the next one's, ahead of time).
  useEffect(() => {
    elapsed.current = 0;
    setProgress(0);
    setCaption(null);
    setFailed(false);
    setMediaUrl(null);
    if (!story) return;
    setReady(story.kind === "text");
    let alive = true;
    if (story.meta.media) {
      storyMediaUrl(story)
        .then((url) => alive && setMediaUrl(url))
        .catch(() => {
          if (!alive) return;
          setFailed(true);
          setReady(true);
        });
    }
    const upcoming = group?.stories[storyIndex + 1] ?? groups[groupIndex + 1]?.stories[0];
    if (upcoming?.meta.media) storyMediaUrl(upcoming).catch(() => {});
    return () => {
      alive = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [story?.id]);

  useEffect(() => {
    if (story && ready && !failed && !group.owner.isSelf) stories.markSeen(story);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [story?.id, ready, failed]);

  // Stickers you can answer: what you already sent and the totals the author published (yours: the answers sealed to you).
  const ownerIsSelf = group?.owner.isSelf ?? false;
  const authorId = group?.owner.userId;
  useEffect(() => {
    setMine(new Map());
    setTotals(new Map());
    setInbox(null);
    setStickerFocus(false);
    setStickerHover(false);
    if (!story || !stories.interactiveEnabled || answerableStickers(story).length === 0) return;
    let alive = true;
    const load = () => {
      if (ownerIsSelf) {
        syncTotals(identity, story, stories.friends).then((i) => alive && setInbox(i), () => {});
        return;
      }
      const authorKey = authorId ? stories.friends.get(authorId)?.publicKey : undefined;
      if (!authorKey) return;
      loadMyAnswers(identity, story, authorKey).then((m) => alive && setMine(m), () => {});
      loadTotals(story).then((t) => alive && setTotals(t), () => {});
    };
    load();
    const timer = setInterval(() => !document.hidden && load(), INTERACTION_REFRESH_MS);
    return () => {
      alive = false;
      clearInterval(timer);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [story?.id, stories.interactiveEnabled]);

  // The clock: photos and text stories for STORY_SECONDS, videos for their length.
  useEffect(() => {
    if (!story || paused) return;
    let raf = 0;
    let last = performance.now();
    const tick = (t: number) => {
      if ((story.kind === "video" || story.kind === "audio") && !failed) {
        const v = mediaRef.current;
        // A video recorded in the browser has no length in its header (the element says Infinity): use the one in the details.
        const total = v && Number.isFinite(v.duration) && v.duration > 0 ? v.duration : (story.meta.media?.durationMs ?? 0) / 1000;
        if (v && total > 0) setProgress(Math.min(1, v.currentTime / total));
        if (v && story.meta.captions) setCaption(captionAt(story.meta.captions, v.currentTime * 1000));
      } else {
        elapsed.current += t - last;
        last = t;
        const seconds = !group.owner.isSelf && stories.interactiveEnabled && answerableStickers(story).length > 0 ? STICKER_STORY_SECONDS : STORY_SECONDS;
        const p = elapsed.current / (seconds * 1000);
        if (p >= 1) return nextRef.current();
        setProgress(p);
      }
      raf = requestAnimationFrame(tick);
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [story, paused, failed]);

  useEffect(() => {
    const v = mediaRef.current;
    if (!v) return;
    if (paused) v.pause();
    else
      v.play().catch(() => {
        // The browser wants a tap before playing sound: play muted instead.
        v.muted = true;
        setMuted(true);
        v.play().catch(() => {});
      });
  }, [paused, mediaUrl]);

  useEffect(() => {
    if (!toast) return;
    const t = setTimeout(() => setToast(null), 1800);
    return () => clearTimeout(t);
  }, [toast]);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const el = e.target as HTMLElement | null;
      if (sheet || (el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA" || el.tagName === "SELECT"))) return;
      // Space on a focused button presses it (a poll option, say) rather than pausing.
      if (e.key === " " && el && el.tagName === "BUTTON") return;
      if (e.key === "ArrowRight") {
        e.preventDefault();
        nextRef.current();
      } else if (e.key === "ArrowLeft") {
        e.preventDefault();
        prev();
      } else if (e.key === " ") {
        e.preventDefault();
        setUserPaused((p) => !p);
      }
    };
    document.addEventListener("keydown", onKey);
    return () => document.removeEventListener("keydown", onKey);
  }, [prev, sheet]);

  if (!group || !story) return null;

  const conversation = state.conversations.find((c): c is DMConversation => c.kind === "dm" && c.userId === group.owner.userId);
  const canReply = !group.owner.isSelf && story.allowReplies && !!conversation?.conversationKey;
  const isMuted = stories.prefs.muted.includes(group.owner.userId);

  function onPointerDown(e: React.PointerEvent) {
    if (e.button !== 0) return;
    pressAt.current = Date.now();
    setHeld(true);
  }
  function onPointerUp(e: React.PointerEvent) {
    setHeld(false);
    if (!pressAt.current || Date.now() - pressAt.current > HOLD_MS) return;
    pressAt.current = 0;
    const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
    if (e.clientX - rect.left < rect.width * 0.3) prev();
    else next();
  }

  async function reply(text: string, reaction: boolean) {
    if (!conversation?.conversationKey || !story) return;
    try {
      await sendStoryReply(dispatch, conversation.id, conversation.conversationKey, text, {
        authorId: group.owner.userId,
        storyId: story.id,
        // Older apps only know photo, video and text in a reply: a voice story is sent as text (its preview says "Voice").
        kind: story.kind === "audio" ? "text" : story.kind,
        preview: storyPreview(story),
        reaction,
      });
      setToast(reaction ? `Sent ${text}` : "Reply sent");
    } catch {
      setToast("Couldn't send. Try again.");
    }
  }

  async function removeStory() {
    if (!story || !window.confirm("Delete this story? It's removed for everyone.")) return;
    try {
      await stories.remove(story.id);
    } catch {
      setToast("Couldn't delete. Try again.");
      return;
    }
    setSheet(null);
    const left = group.stories.filter((s) => s.id !== story.id);
    if (left.length === 0) return close();
    setGroups(groups.map((g, i) => (i === groupIndex ? { ...g, stories: left } : g)));
    setStoryIndex(Math.min(storyIndex, left.length - 1));
  }

  /** Your highlight: take this story out of it (the highlight goes when it's empty). */
  async function removeFromHighlight() {
    if (target.kind !== "list" || !target.highlightId || !story) return;
    const left = group.stories.filter((s) => s.id !== story.id);
    try {
      if (left.length === 0) await sapi.deleteHighlight(target.highlightId);
      // The cover's key seals the title: a new cover means sealing it again.
      else if (story.id === target.coverStoryId) await saveHighlight(target.highlightId, target.title ?? "Highlight", left, left[0]);
      else await sapi.updateHighlight(target.highlightId, { storyIds: left.map((s) => s.id) });
    } catch {
      setToast("Couldn't change the highlight. Try again.");
      return;
    }
    setSheet(null);
    if (left.length === 0) return close();
    setGroups(groups.map((g, i) => (i === groupIndex ? { ...g, stories: left } : g)));
    setStoryIndex(Math.min(storyIndex, left.length - 1));
    setToast("Removed from highlight");
  }

  async function toggleMute() {
    const id = group.owner.userId;
    try {
      await stories.updatePrefs((p) => ({ ...p, muted: isMuted ? p.muted.filter((u) => u !== id) : [...p.muted, id] }));
      setToast(isMuted ? `Unmuted ${group.owner.handle}` : `Muted ${group.owner.handle}'s stories`);
    } catch {
      setToast("Couldn't save. Try again.");
    }
    setSheet(null);
  }

  const live = story.expiresAt > Date.now();

  async function answer(sticker: AnswerableSticker, a: StoryAnswer) {
    const authorKey = group.owner.isSelf ? identity.publicKey : stories.friends.get(group.owner.userId)?.publicKey;
    if (!authorKey || !story) return setToast("Can't answer right now.");
    setBusySticker(sticker.id);
    try {
      await submitAnswer(identity, story, authorKey, sticker, a);
      const anonymous = isAnonymousSticker(sticker);
      setMine((m) => new Map(m).set(sticker.id, { answer: anonymous ? null : a, anonymous }));
      setToast(sticker.type === "question" ? "Sent" : "Answer sent");
      loadTotals(story).then(setTotals, () => {});
    } catch (err) {
      if (err instanceof ApiError && err.status === 409) {
        setToast("You already answered that.");
        loadMyAnswers(identity, story, authorKey).then(setMine, () => {});
      } else if (err instanceof ApiError && err.status === 404) setToast("This story has ended.");
      else setToast("Couldn't send. Try again.");
    } finally {
      setBusySticker(null);
    }
  }

  // Your own story's totals, worked out from the answers sealed to you.
  const ownTotals = inbox ? new Map(answerableStickers(story).map((x) => [x.id, tallyAnswers(x, inbox.entries)])) : null;
  const hasAnswerable = answerableStickers(story).length > 0;

  return (
    <div className={styles.viewer} ref={root} role="dialog" aria-modal="true" aria-label={`${group.owner.isSelf ? "Your" : `${group.owner.handle}'s`} story`}>
      {groupIndex > 0 || storyIndex > 0 ? (
        <button type="button" className={`${styles.sideNav} ${styles.sideNavLeft}`} aria-label="Previous story" onClick={prev}>
          <ChevronLeftIcon size={22} />
        </button>
      ) : null}
      <div className={styles.stage}>
        <div className={styles.tapArea} onPointerDown={onPointerDown} onPointerUp={onPointerUp} onPointerLeave={() => setHeld(false)} onPointerCancel={() => setHeld(false)}>
          <StoryFrame
            meta={story.meta}
            mediaUrl={mediaUrl}
            mediaRef={mediaRef}
            audioProgress={progress}
            caption={caption}
            muted={muted}
            onMediaReady={() => setReady(true)}
            onVideoEnded={() => nextRef.current()}
            label={story.kind === "text" ? story.meta.text : story.kind === "image" ? "Photo" : story.kind === "audio" ? story.meta.text || "Voice story" : "Video"}
          >
            {story.meta.stickers?.map((sticker) => (
              <div
                key={sticker.id}
                className={`${styles.overlay} ${styles.stickerWrap}`}
                style={stickerStyle(sticker)}
                onPointerDown={(e) => e.stopPropagation()}
                onPointerUp={(e) => e.stopPropagation()}
                onPointerEnter={() => setStickerHover(true)}
                onPointerLeave={() => setStickerHover(false)}
              >
                <StickerView
                  sticker={sticker}
                  mode="view"
                  friends={stories.friends}
                  isOwner={group.owner.isSelf}
                  myAnswer={mine.get(sticker.id)}
                  totals={(group.owner.isSelf ? ownTotals : totals)?.get(sticker.id)}
                  busy={busySticker === sticker.id}
                  onAnswer={stories.interactiveEnabled && live ? answer : undefined}
                  onOpenLink={(url) => {
                    setPendingLink(url);
                    setSheet("link");
                  }}
                  onFocusChange={setStickerFocus}
                />
              </div>
            ))}
          </StoryFrame>
          {failed && <div className={styles.failed}>This story couldn't be opened.</div>}
        </div>

        <div className={styles.top}>
          <div className={styles.bars} aria-hidden="true">
            {group.stories.map((s, i) => (
              <span key={s.id} className={styles.bar}>
                <span className={styles.barFill} style={{ transform: `scaleX(${i < storyIndex ? 1 : i === storyIndex ? progress : 0})` }} />
              </span>
            ))}
          </div>
          <div className={styles.header}>
            <Avatar avatarUrl={group.owner.avatarUrl} seed={group.owner.handle} size={32} />
            <span className={styles.headerName}>{group.owner.isSelf ? "Your story" : group.owner.handle}</span>
            <span className={styles.headerAge}>{storyAge(story.createdAt)}</span>
            {target.kind === "list" && target.title && <span className={styles.headerAge}>· {target.title}</span>}
            {story.meta.closeFriends && (
              <span className={styles.closeBadge}>
                <StarIcon size={11} /> Close friends
              </span>
            )}
            <span className={styles.headerSpacer} />
            <button type="button" className={styles.chromeButton} aria-label={userPaused ? "Play" : "Pause"} onClick={() => setUserPaused(!userPaused)}>
              {userPaused ? <PlayIcon size={18} /> : <PauseIcon size={18} />}
            </button>
            {(story.kind === "video" || story.kind === "audio") && (
              <button
                type="button"
                className={styles.chromeButton}
                aria-label={muted ? "Turn sound on" : "Turn sound off"}
                onClick={() => {
                  soundOn = muted;
                  setMuted(!muted);
                }}
              >
                {muted ? <SpeakerOffIcon size={18} /> : <SpeakerIcon size={18} />}
              </button>
            )}
            <button type="button" className={styles.chromeButton} aria-label="More" aria-haspopup="menu" onClick={() => setSheet(sheet === "menu" ? null : "menu")}>
              <DotsIcon size={18} />
            </button>
            <button type="button" className={styles.chromeButton} aria-label="Close stories" onClick={close}>
              <CloseIcon size={20} />
            </button>
          </div>
        </div>

        <div className={styles.bottom}>
          {group.owner.isSelf ? (
            <div className={styles.ownBar}>
              <button type="button" className={styles.ownAction} onClick={() => setSheet("viewers")}>
                <EyeIcon size={18} />
                <span>{story.viewCount ? `Seen by ${story.viewCount}` : "Activity"}</span>
              </button>
              {hasAnswerable && stories.interactiveEnabled && (
                <button type="button" className={styles.ownAction} onClick={() => setSheet("answers")}>
                  <SendIcon size={18} />
                  <span>{inbox ? `Answers (${inbox.entries.length})` : "Answers"}</span>
                </button>
              )}
              <button type="button" className={styles.ownAction} onClick={() => setSheet("highlight")}>
                <HeartIcon size={18} />
                <span>Highlight</span>
              </button>
            </div>
          ) : canReply ? (
            <ReplyBar
              handle={group.owner.handle}
              onFocusChange={setTyping}
              onSend={(text) => reply(text, false)}
              onReact={(emoji) => reply(emoji, true)}
            />
          ) : null}
        </div>

        {toast && (
          <div className={styles.toast} role="status">
            {toast}
          </div>
        )}

        {sheet === "menu" && (
          <div className={styles.menu} role="menu">
            {group.owner.isSelf ? (
              <>
                {target.kind === "list" && target.highlightId && (
                  <button type="button" role="menuitem" onClick={removeFromHighlight}>
                    Remove from highlight
                  </button>
                )}
                <button type="button" role="menuitem" className={styles.menuDanger} onClick={removeStory}>
                  Delete story
                </button>
                <button type="button" role="menuitem" onClick={() => setSheet("highlight")}>
                  Add to highlight
                </button>
                <button
                  type="button"
                  role="menuitem"
                  onClick={() => {
                    close();
                    dispatch({ type: "OPEN_SETTINGS", section: "stories" });
                  }}
                >
                  Story settings
                </button>
              </>
            ) : (
              <>
                <button type="button" role="menuitem" onClick={toggleMute}>
                  {isMuted ? `Unmute ${group.owner.handle}` : `Mute ${group.owner.handle}'s stories`}
                </button>
                {conversation && (
                  <button
                    type="button"
                    role="menuitem"
                    onClick={() => {
                      close();
                      dispatch({ type: "OPEN_PROFILE", userId: group.owner.userId, origin: { screen: "thread", conversationId: conversation.id } });
                    }}
                  >
                    View profile
                  </button>
                )}
                <button type="button" role="menuitem" onClick={() => setSheet("report")}>
                  Report story
                </button>
              </>
            )}
            <button type="button" role="menuitem" onClick={() => setSheet(null)}>
              Cancel
            </button>
          </div>
        )}
        {sheet === "viewers" && <ViewersSheet story={story} onClose={() => setSheet(null)} />}
        {sheet === "highlight" && <HighlightSheet story={story} onClose={() => setSheet(null)} onDone={(msg) => setToast(msg)} />}
        {sheet === "answers" && <StoryAnswersSheet story={story} inbox={inbox} friends={stories.friends} onClose={() => setSheet(null)} onDone={(msg) => setToast(msg)} />}
        {sheet === "report" && <StoryReportSheet story={story} onClose={() => setSheet(null)} onDone={(msg) => setToast(msg)} />}
        {sheet === "link" && pendingLink && <LinkSheet url={pendingLink} onClose={() => setSheet(null)} />}
      </div>
      {groupIndex + 1 < groups.length || storyIndex + 1 < group.stories.length ? (
        <button type="button" className={`${styles.sideNav} ${styles.sideNavRight}`} aria-label="Next story" onClick={next}>
          <ChevronRightIcon size={22} />
        </button>
      ) : null}
    </div>
  );
}

function ReplyBar({ handle, onFocusChange, onSend, onReact }: { handle: string; onFocusChange: (f: boolean) => void; onSend: (t: string) => void; onReact: (e: string) => void }) {
  const [text, setText] = useState("");
  const [focused, setFocused] = useState(false);
  const setFocus = (f: boolean) => {
    setFocused(f);
    onFocusChange(f);
  };
  return (
    <div className={styles.replyWrap}>
      {focused && !text && (
        <div className={styles.quickReactions} aria-label="Quick reactions">
          {QUICK_REACTIONS.map((e) => (
            <button key={e} type="button" className={styles.quickReaction} aria-label={`React ${e}`} onPointerDown={(ev) => ev.preventDefault()} onClick={() => onReact(e)}>
              {e}
            </button>
          ))}
        </div>
      )}
      <form
        className={styles.replyBar}
        onSubmit={(e) => {
          e.preventDefault();
          const t = text.trim();
          if (!t) return;
          onSend(t);
          setText("");
          (document.activeElement as HTMLElement | null)?.blur();
        }}
      >
        <input
          className={styles.replyInput}
          placeholder={`Reply to ${handle}…`}
          aria-label={`Reply to ${handle}`}
          value={text}
          maxLength={2000}
          onChange={(e) => setText(e.target.value)}
          onFocus={() => setFocus(true)}
          onBlur={() => setFocus(false)}
        />
        {text.trim() ? (
          <button type="submit" className={styles.chromeButton} aria-label="Send reply">
            <SendIcon size={20} />
          </button>
        ) : (
          <button type="button" className={styles.chromeButton} aria-label="Like story" onClick={() => onReact("❤️")}>
            <HeartIcon size={22} />
          </button>
        )}
      </form>
    </div>
  );
}

function ViewersSheet({ story, onClose }: { story: Story; onClose: () => void }) {
  const stories = useStories();
  const [ids, setIds] = useState<string[] | null>(null);
  const [error, setError] = useState(false);
  useEffect(() => {
    sapi.getStoryViewers(story.id).then(setIds, () => setError(true));
  }, [story.id]);
  return (
    <div className={styles.sheet} role="region" aria-label="Who saw this story">
      <div className={styles.sheetHead}>
        <strong>
          <EyeIcon size={16} /> {ids ? `Seen by ${ids.length}` : "Activity"}
        </strong>
        <button type="button" className={styles.chromeButton} aria-label="Close" onClick={onClose}>
          <CloseIcon size={18} />
        </button>
      </div>
      <p className={styles.sheetNote}>Friends who turned off sharing views aren't listed. No times are kept.</p>
      {error && <p className={styles.sheetNote}>Couldn't load who saw it.</p>}
      {ids?.length === 0 && <p className={styles.sheetNote}>No views yet.</p>}
      <ul className={styles.sheetList}>
        {ids?.map((id) => {
          const f = stories.friends.get(id);
          return (
            <li key={id} className={styles.sheetRow}>
              <Avatar avatarUrl={f?.avatarUrl ?? null} seed={f?.handle ?? id} size={32} />
              <span>{f?.handle ?? "Someone who's no longer your friend"}</span>
            </li>
          );
        })}
      </ul>
    </div>
  );
}

function HighlightSheet({ story, onClose, onDone }: { story: Story; onClose: () => void; onDone: (msg: string) => void }) {
  const { state } = useAppState();
  const identity = state.identity!;
  const [list, setList] = useState<HighlightView[] | null>(null);
  const [title, setTitle] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    sapi
      .getHighlights(identity.userId)
      .then((dtos) => setList(decodeHighlights(dtos, identity.publicKey, identity.privateKey)))
      .catch(() => setError("Couldn't load your highlights."));
  }, [identity]);
  const existing = useMemo(() => list ?? [], [list]);

  async function addTo(h: HighlightView) {
    setBusy(true);
    setError(null);
    try {
      await sapi.updateHighlight(h.id, { storyIds: [...h.stories.map((s) => s.id), story.id] });
      onDone(`Added to ${h.title ?? "highlight"}`);
      onClose();
    } catch {
      setError("Couldn't add it. Try again.");
    } finally {
      setBusy(false);
    }
  }

  async function create() {
    const t = title.trim();
    if (!t) return;
    setBusy(true);
    setError(null);
    try {
      await saveHighlight(null, t, [story], story);
      onDone(`Added to ${t}`);
      onClose();
    } catch {
      setError("Couldn't create it. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.sheet} role="region" aria-label="Add to highlight">
      <div className={styles.sheetHead}>
        <strong>Add to highlight</strong>
        <button type="button" className={styles.chromeButton} aria-label="Close" onClick={onClose}>
          <CloseIcon size={18} />
        </button>
      </div>
      <p className={styles.sheetNote}>Highlights stay on your profile after 24 hours, for friends who could see these stories.</p>
      {error && <p className={styles.sheetError}>{error}</p>}
      <ul className={styles.sheetList}>
        {existing.map((h) => {
          const has = h.stories.some((s) => s.id === story.id);
          return (
            <li key={h.id} className={styles.sheetRow}>
              <span className={styles.sheetGrow}>{h.title ?? "Highlight"}</span>
              <button type="button" className={styles.sheetButton} disabled={busy || has} onClick={() => addTo(h)}>
                {has ? "Added" : "Add"}
              </button>
            </li>
          );
        })}
      </ul>
      <form
        className={styles.sheetForm}
        onSubmit={(e) => {
          e.preventDefault();
          void create();
        }}
      >
        <input
          className={styles.sheetInput}
          placeholder="New highlight name"
          aria-label="New highlight name"
          maxLength={MAX_HIGHLIGHT_TITLE}
          value={title}
          onChange={(e) => setTitle(e.target.value)}
        />
        <button type="submit" className={styles.sheetButton} disabled={busy || !title.trim()}>
          Create
        </button>
      </form>
    </div>
  );
}

/** A link sticker: say where it goes before leaving. The address is shown as the browser spells it. */
function LinkSheet({ url, onClose }: { url: string; onClose: () => void }) {
  let host = url;
  try {
    host = new URL(url).hostname;
  } catch {
    // Only https links get this far (lib/storyCrypto.ts safeStoryLink); shown whole if it somehow isn't one.
  }
  return (
    <div className={styles.sheet} role="alertdialog" aria-modal="true" aria-label="Open this link?">
      <strong>Open this link?</strong>
      <p className={styles.sheetNote}>
        A friend's story links to <strong>{host}</strong>. RedVault hasn't checked it.
      </p>
      <p className={styles.linkUrl}>{url}</p>
      <div className={styles.textEditorRow}>
        <button type="button" className={styles.sheetButton} onClick={onClose}>
          Stay here
        </button>
        <span className={styles.headerSpacer} />
        <button
          type="button"
          className={styles.sheetButtonPrimary}
          onClick={() => {
            window.open(url, "_blank", "noopener,noreferrer");
            onClose();
          }}
        >
          Open
        </button>
      </div>
    </div>
  );
}
