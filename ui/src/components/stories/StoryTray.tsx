import { useAppState } from "../../state/AppStateContext.js";
import { useStoriesOptional } from "../../state/StoriesContext.js";
import { Avatar } from "../Avatar.js";
import { PlusIcon } from "../icons24.js";
import styles from "./Stories.module.css";

/** A ring around an avatar: colourful for something new, green for close friends, grey once seen. */
export function StoryRing({ state, size, children }: { state: "new" | "close" | "seen" | "none"; size: number; children: React.ReactNode }) {
  return (
    <span className={`${styles.ring} ${styles[`ring_${state}`]}`} style={{ width: size + 8, height: size + 8 }}>
      <span className={styles.ringGap}>{children}</span>
    </span>
  );
}

/** The row of stories above the chat list, like Instagram's: yours first, then friends with something new. */
export function StoryTray() {
  const { state } = useAppState();
  const stories = useStoriesOptional();
  const identity = state.identity;
  // Stories switched off on the server: no tray, no "add to your story".
  if (!stories || !identity || !stories.enabled) return null;
  const { own, authors } = stories;
  const ownState = own.length ? "seen" : "none";

  return (
    <div className={styles.tray} role="list" aria-label="Stories">
      <div className={styles.trayItem} role="listitem">
        <button
          type="button"
          className={styles.trayButton}
          aria-label={own.length ? `Your story, ${own.length} ${own.length === 1 ? "post" : "posts"}` : "Add to your story"}
          onClick={() => (own.length ? stories.openViewer({ kind: "feed", authorId: "me" }) : stories.openComposer())}
        >
          <StoryRing state={ownState} size={54}>
            <Avatar avatarUrl={identity.avatarUrl} seed={identity.handle} size={54} />
          </StoryRing>
          <span className={styles.trayName}>Your story</span>
        </button>
        <button type="button" className={styles.addBadge} aria-label="New story" onClick={() => stories.openComposer()}>
          <PlusIcon size={14} />
        </button>
      </div>
      {authors.map((a) => {
        const unseen = a.stories.filter((s) => !s.seen);
        const ring = unseen.length === 0 ? "seen" : unseen.some((s) => s.meta.closeFriends) ? "close" : "new";
        return (
          <div className={styles.trayItem} role="listitem" key={a.author.userId}>
            <button
              type="button"
              className={styles.trayButton}
              aria-label={`${a.author.handle}'s story${unseen.length ? `, ${unseen.length} new` : ""}`}
              onClick={() => stories.openViewer({ kind: "feed", authorId: a.author.userId })}
            >
              <StoryRing state={ring} size={54}>
                <Avatar avatarUrl={a.author.avatarUrl} seed={a.author.handle} size={54} />
              </StoryRing>
              <span className={`${styles.trayName} ${unseen.length ? styles.trayNameNew : ""}`}>{a.author.handle}</span>
            </button>
          </div>
        );
      })}
    </div>
  );
}
