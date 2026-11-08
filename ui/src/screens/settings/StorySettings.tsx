// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useState } from "react";
import { Avatar } from "../../components/Avatar.js";
import { HighlightEditor, useHighlights, useOwnStoryLibrary } from "../../components/stories/Highlights.js";
import { StoryThumb } from "../../components/stories/StoryThumb.js";
import * as sapi from "../../lib/storiesApi.js";
import type { HighlightView, StoryFriend } from "../../lib/stories.js";
import type { StoryPrefs } from "../../lib/storyCrypto.js";
import { ToggleSwitch } from "../../components/ToggleSwitch.js";
import { useAppState } from "../../state/AppStateContext.js";
import { useStoriesOptional } from "../../state/StoriesContext.js";
import styles from "./SettingsPages.module.css";

/**
 * Who sees your stories, whether others see that you viewed theirs, the
 * archive and highlights. These settings are encrypted on this device and
 * synced to your other devices; the server can't read them.
 */
export function StorySettings() {
  const { state } = useAppState();
  const stories = useStoriesOptional();
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [picker, setPicker] = useState<"closeFriends" | "hideFrom" | null>(null);
  const [archiveOpen, setArchiveOpen] = useState(false);
  const [editing, setEditing] = useState<HighlightView | "new" | null>(null);
  const userId = state.identity?.userId ?? "";
  const highlights = useHighlights(userId);

  if (!stories) return null;
  const { prefs } = stories;
  const friends = [...stories.friends.values()].sort((a, b) => a.handle.localeCompare(b.handle));

  async function change(fn: (p: StoryPrefs) => StoryPrefs, archive?: boolean) {
    setBusy(true);
    setError(null);
    try {
      await stories!.updatePrefs(fn, archive);
    } catch {
      setError("Couldn't save. Try again.");
    } finally {
      setBusy(false);
    }
  }

  const names = (ids: string[]) => {
    const known = ids.map((id) => stories.friends.get(id)?.handle).filter(Boolean);
    if (known.length === 0) return "Nobody";
    return known.length <= 3 ? known.join(", ") : `${known.slice(0, 3).join(", ")} and ${known.length - 3} more`;
  };

  async function removeHighlight(h: HighlightView) {
    if (!window.confirm(`Delete the highlight "${h.title ?? "Highlight"}"? The stories in it aren't deleted.`)) return;
    try {
      await sapi.deleteHighlight(h.id);
      highlights.reload();
    } catch {
      setError("Couldn't delete the highlight. Try again.");
    }
  }

  return (
    <>
      <p className={styles.lead}>
        Stories are photos, videos and text your friends can see for 24 hours. They're end-to-end encrypted: each one has its own key, sealed only
        to the friends you share it with. Every account, on every plan, can post them.
      </p>
      {error && <p className={styles.error}>{error}</p>}

      <h3 className={`${styles.subhead} type-caption`}>WHO SEES YOUR STORIES</h3>
      <div className={styles.group}>
        <button type="button" className={styles.row} onClick={() => setPicker("closeFriends")}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Close friends</span>
            <span className={styles.rowDetail}>{names(prefs.closeFriends)}. Share a story with just them by picking "Close friends" when you post.</span>
          </span>
        </button>
        <button type="button" className={styles.row} onClick={() => setPicker("hideFrom")}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Hide story from</span>
            <span className={styles.rowDetail}>{names(prefs.hideFrom)}. They won't get your stories, including ones already up.</span>
          </span>
        </button>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Allow replies</span>
            <span className={styles.rowDetail}>Friends can reply and react; replies arrive as messages in your chat with them. You can change it for each story.</span>
          </span>
          <ToggleSwitch label="Allow replies" checked={prefs.allowReplies} onChange={(v) => change((p) => ({ ...p, allowReplies: v }))} />
        </div>
      </div>

      {picker && (
        <FriendPicker
          title={picker === "closeFriends" ? "Close friends" : "Hide story from"}
          friends={friends}
          chosen={prefs[picker]}
          onCancel={() => setPicker(null)}
          onSave={async (ids) => {
            const which = picker;
            setPicker(null);
            await change((p) => ({ ...p, [which]: ids }));
          }}
        />
      )}

      <h3 className={`${styles.subhead} type-caption`}>VIEWING</h3>
      <div className={styles.group}>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Share that you viewed stories</span>
            <span className={styles.rowDetail}>
              When on, friends see you in their story's viewer list (never when you looked). When off, this app never tells the server you saw a story.
            </span>
          </span>
          <ToggleSwitch label="Share that you viewed stories" checked={prefs.shareViews} onChange={(v) => change((p) => ({ ...p, shareViews: v }))} />
        </div>
      </div>

      <h3 className={`${styles.subhead} type-caption`}>ARCHIVE AND HIGHLIGHTS</h3>
      <div className={styles.group}>
        <div className={styles.row}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Save to archive</span>
            <span className={styles.rowDetail}>Keep your stories, encrypted and visible only to you, after their 24 hours. Off: they're deleted (unless in a highlight).</span>
          </span>
          <ToggleSwitch label="Save to archive" checked={stories.archiveEnabled} onChange={(v) => change((p) => p, v)} />
        </div>
        <button type="button" className={styles.row} onClick={() => setArchiveOpen(!archiveOpen)}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>Story archive</span>
            <span className={styles.rowDetail}>Your stories from today and the archive.</span>
          </span>
        </button>
        {archiveOpen && <ArchiveGrid />}
        {highlights.list?.map((h) => (
          <div key={h.id} className={styles.row}>
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>{h.title ?? "Highlight"}</span>
              <span className={styles.rowDetail}>
                {h.stories.length} {h.stories.length === 1 ? "story" : "stories"}
              </span>
            </span>
            <span className={styles.person}>
              <button type="button" className={styles.smallButton} onClick={() => setEditing(h)}>
                Edit
              </button>
              <button type="button" className={styles.dangerButton} onClick={() => removeHighlight(h)}>
                Delete
              </button>
            </span>
          </div>
        ))}
        <button type="button" className={styles.row} onClick={() => setEditing("new")}>
          <span className={styles.rowText}>
            <span className={styles.rowTitle}>New highlight</span>
            <span className={styles.rowDetail}>Highlights stay on your profile for friends who could see the stories in them.</span>
          </span>
        </button>
      </div>

      <h3 className={`${styles.subhead} type-caption`}>MUTED</h3>
      <div className={styles.group}>
        {prefs.muted.length === 0 ? (
          <div className={styles.row}>
            <span className={styles.rowDetail}>Nobody's muted. Mute someone from the ⋯ menu on their story: they won't know.</span>
          </div>
        ) : (
          prefs.muted.map((id) => {
            const f = stories.friends.get(id);
            return (
              <div key={id} className={styles.row}>
                <span className={styles.person}>
                  <Avatar avatarUrl={f?.avatarUrl ?? null} seed={f?.handle ?? id} size={32} />
                  {f?.handle ?? "Someone who's no longer your friend"}
                </span>
                <button type="button" className={styles.smallButton} disabled={busy} onClick={() => change((p) => ({ ...p, muted: p.muted.filter((u) => u !== id) }))}>
                  Unmute
                </button>
              </div>
            );
          })
        )}
      </div>

      <h3 className={`${styles.subhead} type-caption`}>WHAT THE SERVER SEES</h3>
      <div className={styles.group}>
        <div className={styles.row}>
          <span className={styles.rowDetail}>
            When you posted, how big it is, whether it's a photo, video or text, and which friends got a key. Not what it shows, its text or stickers,
            your close friends list or who you hide stories from. Anyone who can see a story could still photograph their screen.
          </span>
        </div>
      </div>

      {editing && (
        <HighlightEditor
          highlight={editing === "new" ? null : editing}
          onClose={() => setEditing(null)}
          onSaved={() => {
            setEditing(null);
            highlights.reload();
          }}
        />
      )}
    </>
  );
}

function ArchiveGrid() {
  const stories = useStoriesOptional();
  const { state } = useAppState();
  const { stories: library } = useOwnStoryLibrary();
  if (!stories || !state.identity) return null;
  if (library === null) return <div className={styles.row}>Loading…</div>;
  if (library.length === 0) return <div className={styles.row}>Nothing here yet.</div>;
  return (
    <div className={styles.row} style={{ display: "block" }}>
      <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fill, minmax(90px, 1fr))", gap: 6 }}>
        {library.map((s, i) => (
          <StoryThumb key={s.id} story={s} onClick={() => stories.openViewer({ kind: "list", stories: library, startIndex: i, ownerId: state.identity!.userId, title: "Archive" })} />
        ))}
      </div>
    </div>
  );
}

function FriendPicker({
  title,
  friends,
  chosen,
  onCancel,
  onSave,
}: {
  title: string;
  friends: StoryFriend[];
  chosen: string[];
  onCancel: () => void;
  onSave: (ids: string[]) => void;
}) {
  const [picked, setPicked] = useState<string[]>(chosen);
  const [query, setQuery] = useState("");
  const shown = friends.filter((f) => f.handle.toLowerCase().includes(query.trim().toLowerCase()));
  return (
    <div className={styles.group} role="group" aria-label={title} style={{ padding: 12 }}>
      <div className={styles.row} style={{ borderBottom: "none" }}>
        <strong>{title}</strong>
        <input className={styles.labelInput} placeholder="Search friends" aria-label="Search friends" value={query} onChange={(e) => setQuery(e.target.value)} />
      </div>
      {friends.length === 0 && <div className={styles.row}>Add friends first.</div>}
      {shown.map((f) => (
        <label key={f.userId} className={styles.row}>
          <span className={styles.person}>
            <Avatar avatarUrl={f.avatarUrl} seed={f.handle} size={32} />
            {f.handle}
          </span>
          <input
            type="checkbox"
            checked={picked.includes(f.userId)}
            onChange={(e) => setPicked(e.target.checked ? [...picked, f.userId] : picked.filter((id) => id !== f.userId))}
          />
        </label>
      ))}
      <div className={styles.row} style={{ justifyContent: "flex-end", borderBottom: "none" }}>
        <button type="button" className={styles.smallButton} onClick={onCancel}>
          Cancel
        </button>
        <button type="button" className={styles.smallButton} onClick={() => onSave(picked)}>
          Save ({picked.length})
        </button>
      </div>
    </div>
  );
}
