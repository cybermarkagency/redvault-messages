import { useCallback, useEffect, useRef, useState } from "react";
import * as sapi from "../../lib/storiesApi.js";
import { decodeHighlights, decodeStory, saveHighlight, type HighlightView, type Story } from "../../lib/stories.js";
import { MAX_HIGHLIGHT_TITLE } from "../../lib/storyCrypto.js";
import { useAppState } from "../../state/AppStateContext.js";
import { useStoriesOptional } from "../../state/StoriesContext.js";
import { CloseIcon, PlusIcon } from "../icons24.js";
import { useDialogFocus } from "../useDialogFocus.js";
import { StoryThumb, StoryThumbImage } from "./StoryThumb.js";
import styles from "./Stories.module.css";
import formStyles from "../../screens/settings/SettingsPages.module.css";

/** Someone's highlights as this account can see them (only stories it holds keys for). */
export function useHighlights(ownerId: string): { list: HighlightView[] | null; reload: () => void } {
  const { state } = useAppState();
  const stories = useStoriesOptional();
  const identity = state.identity;
  const [list, setList] = useState<HighlightView[] | null>(null);
  const ownerKey = ownerId === identity?.userId ? identity.publicKey : stories?.friends.get(ownerId)?.publicKey;
  const reload = useCallback(() => {
    if (!identity || !ownerKey) {
      setList([]);
      return;
    }
    sapi
      .getHighlights(ownerId)
      .then((dtos) => setList(decodeHighlights(dtos, ownerKey, identity.privateKey)))
      .catch(() => setList([]));
  }, [ownerId, identity, ownerKey]);
  useEffect(reload, [reload]);
  return { list, reload };
}

/** The circles on a profile; yours has a "New" one too. */
export function HighlightsRow({ ownerId }: { ownerId: string }) {
  const { state } = useAppState();
  const stories = useStoriesOptional();
  const { list, reload } = useHighlights(ownerId);
  const [editing, setEditing] = useState(false);
  const isSelf = ownerId === state.identity?.userId;
  if (!stories || !list || (list.length === 0 && !isSelf)) return null;
  return (
    <>
      <div className={styles.highlights} role="list" aria-label="Story highlights">
        {isSelf && (
          <div role="listitem">
            <button type="button" className={styles.highlight} onClick={() => setEditing(true)}>
              <span className={styles.highlightCover}>
                <PlusIcon size={22} />
              </span>
              <span className={styles.highlightTitle}>New</span>
            </button>
          </div>
        )}
        {list.map((h) => {
          const cover = h.stories.find((s) => s.id === h.coverStoryId) ?? h.stories[0];
          return (
            <div role="listitem" key={h.id}>
              <button
                type="button"
                className={styles.highlight}
                aria-label={`Highlight: ${h.title ?? "untitled"}, ${h.stories.length} ${h.stories.length === 1 ? "story" : "stories"}`}
                onClick={() => stories.openViewer({ kind: "list", stories: h.stories, startIndex: 0, ownerId, title: h.title ?? undefined, highlightId: isSelf ? h.id : undefined, coverStoryId: h.coverStoryId })}
              >
                <span className={styles.highlightCover}>
                  <StoryThumbImage story={cover} />
                </span>
                <span className={styles.highlightTitle}>{h.title ?? "Highlight"}</span>
              </button>
            </div>
          );
        })}
      </div>
      {editing && (
        <HighlightEditor
          highlight={null}
          onClose={() => setEditing(false)}
          onSaved={() => {
            setEditing(false);
            reload();
          }}
        />
      )}
    </>
  );
}

/** Your stories you can put in a highlight: today's and your archive. */
export function useOwnStoryLibrary(): { stories: Story[] | null; reload: () => void } {
  const { state } = useAppState();
  const ctx = useStoriesOptional();
  const identity = state.identity;
  const own = ctx?.own;
  const [archived, setArchived] = useState<Story[] | null>(null);
  const reload = useCallback(() => {
    if (!identity) return;
    sapi
      .getStoryArchive()
      .then((dtos) =>
        setArchived(
          dtos
            .map((d) => decodeStory(d, identity.publicKey, identity.privateKey, { seen: true, archivedAt: d.archivedAt ? new Date(d.archivedAt).getTime() : null }))
            .filter((s): s is Story => s !== null)
        )
      )
      .catch(() => setArchived([]));
  }, [identity]);
  useEffect(reload, [reload]);
  if (archived === null) return { stories: null, reload };
  const ids = new Set(archived.map((s) => s.id));
  const all = [...(own ?? []).filter((s) => !ids.has(s.id)), ...archived].sort((a, b) => b.createdAt - a.createdAt);
  return { stories: all, reload };
}

/** Make or change a highlight: its name and which of your stories are in it (the first one picked is the cover). */
export function HighlightEditor({ highlight, onClose, onSaved }: { highlight: HighlightView | null; onClose: () => void; onSaved: () => void }) {
  const { stories: library } = useOwnStoryLibrary();
  const [title, setTitle] = useState(highlight?.title ?? "");
  const [picked, setPicked] = useState<string[]>(highlight ? [highlight.coverStoryId, ...highlight.stories.map((s) => s.id).filter((id) => id !== highlight.coverStoryId)] : []);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const root = useRef<HTMLDivElement>(null);
  useDialogFocus(root, onClose);

  // Stories already in it stay pickable even if they've left the archive list.
  const pool = (() => {
    const all = [...(library ?? [])];
    for (const s of highlight?.stories ?? []) if (!all.some((x) => x.id === s.id)) all.push(s);
    return all;
  })();

  async function save() {
    const chosen = picked.map((id) => pool.find((s) => s.id === id)).filter((s): s is Story => !!s);
    if (!title.trim() || chosen.length === 0) return;
    setBusy(true);
    setError(null);
    try {
      // Kept in the order the stories were posted; the first one picked is the cover.
      const ordered = [...chosen].sort((a, b) => a.createdAt - b.createdAt);
      await saveHighlight(highlight?.id ?? null, title, ordered, chosen[0]);
      onSaved();
    } catch {
      setError("Couldn't save the highlight. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.viewer} role="dialog" aria-modal="true" aria-label={highlight ? "Edit highlight" : "New highlight"} ref={root}>
      <div className={styles.sheet} style={{ position: "relative", maxHeight: "90dvh", width: "min(560px, 100vw)", borderRadius: "var(--radius-dialog)" }}>
        <div className={styles.sheetHead}>
          <strong>{highlight ? "Edit highlight" : "New highlight"}</strong>
          <button type="button" className={styles.chromeButton} aria-label="Close" onClick={onClose}>
            <CloseIcon size={18} />
          </button>
        </div>
        <div className={styles.sheetForm}>
          <input className={styles.sheetInput} aria-label="Highlight name" placeholder="Name" maxLength={MAX_HIGHLIGHT_TITLE} value={title} onChange={(e) => setTitle(e.target.value)} />
        </div>
        <p className={styles.sheetNote}>
          Pick stories from today and your archive. The first one you pick is the cover. Friends see the stories in it that they could already see.
        </p>
        {library === null ? (
          <p className={styles.sheetNote}>Loading your stories…</p>
        ) : pool.length === 0 ? (
          <p className={styles.sheetNote}>No stories yet. Turn on "Save to archive" in Story settings to keep stories after 24 hours.</p>
        ) : (
          <div className={styles.grid}>
            {pool.map((s) => (
              <StoryThumb
                key={s.id}
                story={s}
                selected={picked.includes(s.id)}
                onClick={() => setPicked(picked.includes(s.id) ? picked.filter((id) => id !== s.id) : [...picked, s.id])}
              />
            ))}
          </div>
        )}
        {error && <p className={styles.sheetError}>{error}</p>}
        <div className={styles.sheetForm}>
          <span className={styles.headerSpacer} />
          <button type="button" className={formStyles.smallButton} onClick={onClose}>
            Cancel
          </button>
          <button type="button" className={styles.sheetButtonPrimary} disabled={busy || !title.trim() || picked.length === 0} onClick={save}>
            {busy ? "Saving…" : "Save"}
          </button>
        </div>
      </div>
    </div>
  );
}
