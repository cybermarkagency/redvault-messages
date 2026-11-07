import { useEffect, useState } from "react";
import { storyMediaUrl, type Story } from "../../lib/stories.js";
import styles from "./Stories.module.css";

/** A small still of a story: its photo, the first frame of its video, or its text on its background. */
export function StoryThumbImage({ story }: { story: Story }) {
  const [url, setUrl] = useState<string | null>(null);
  useEffect(() => {
    let alive = true;
    // A voice story's thumbnail is drawn, so its audio isn't downloaded just to show a grid.
    if (story.meta.media && story.kind !== "audio") storyMediaUrl(story).then((u) => alive && setUrl(u), () => {});
    return () => {
      alive = false;
    };
  }, [story]);
  if (story.kind === "text") {
    return (
      <span className={styles.thumbText} style={{ background: `var(--story-bg-${story.meta.background ?? "sunset"})` }}>
        {(story.meta.text ?? "").slice(0, 40)}
      </span>
    );
  }
  if (story.kind === "audio") {
    return (
      <span className={styles.thumbText} style={{ background: `var(--story-bg-${story.meta.background ?? "night"})` }}>
        🎙 {(story.meta.text ?? "").slice(0, 30)}
      </span>
    );
  }
  if (!url) return null;
  return story.kind === "video" ? <video src={url} muted preload="metadata" playsInline aria-hidden="true" /> : <img src={url} alt="" />;
}

export function StoryThumb({ story, selected, onClick, label }: { story: Story; selected?: boolean; onClick: () => void; label?: string }) {
  return (
    <button
      type="button"
      className={`${styles.thumb} ${selected ? styles.thumbOn : ""}`}
      aria-pressed={selected}
      aria-label={label ?? `${story.kind === "text" ? "Text" : story.kind === "image" ? "Photo" : story.kind === "audio" ? "Voice" : "Video"} story from ${new Date(story.createdAt).toLocaleDateString()}`}
      onClick={onClick}
    >
      <StoryThumbImage story={story} />
      <span className={styles.thumbDate}>{new Date(story.createdAt).toLocaleDateString(undefined, { month: "short", day: "numeric" })}</span>
    </button>
  );
}
