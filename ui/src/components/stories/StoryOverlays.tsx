import { useStoriesOptional } from "../../state/StoriesContext.js";
import { StoryComposer } from "./StoryComposer.js";
import { StoryViewer } from "./StoryViewer.js";

/** The story viewer or composer, over everything else while open. */
export function StoryOverlays() {
  const stories = useStoriesOptional();
  if (!stories) return null;
  const { viewer } = stories;
  return (
    <>
      {viewer && <StoryViewer key={viewer.kind === "feed" ? `feed:${viewer.authorId}` : `list:${viewer.ownerId}:${viewer.highlightId ?? viewer.title ?? ""}`} target={viewer} />}
      {stories.composerOpen && <StoryComposer />}
    </>
  );
}
