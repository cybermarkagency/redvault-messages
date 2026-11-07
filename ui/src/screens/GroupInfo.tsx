import { useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { GroupGlyph } from "../components/GroupGlyph.js";
import { TopBar } from "../components/TopBar.js";
import { GroupVerifiedBadge } from "../components/VerifiedBadge.js";
import { GroupInviteLink } from "./GroupInviteLink.js";
import { addGroupMember, leaveGroup, removeGroupMember, renameGroup, setGroupRole } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import type { GroupMember } from "../types.js";
import styles from "./GroupInfo.module.css";

const ROLE_LABEL = { owner: "Owner", admin: "Admin", member: "" } as const;

/** Members, roles, rename, add friends, remove, leave. The server enforces who may do what. */
export function GroupInfo({ conversationId }: { conversationId: string }) {
  const { state, dispatch, contactsById } = useAppState();
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [renaming, setRenaming] = useState<string | null>(null);
  const [adding, setAdding] = useState(false);
  const group = state.conversations.find((c) => c.id === conversationId);
  if (!group || group.kind !== "group" || !state.identity) return null;
  const identity = state.identity;
  const canManage = group.myRole === "owner" || group.myRole === "admin";
  const memberIds = new Set(group.members.map((m) => m.userId));
  const addable = [...contactsById.values()].filter((c) => c.userId && !memberIds.has(c.userId));

  async function run(action: () => Promise<void>, failure: string) {
    setBusy(true);
    setError(null);
    try {
      await action();
    } catch (err) {
      setError(err instanceof Error && err.message ? err.message : failure);
    } finally {
      setBusy(false);
    }
  }

  const myRole = group.myRole;
  function canRemove(m: GroupMember): boolean {
    if (m.isSelf || m.role === "owner") return false;
    return myRole === "owner" || (myRole === "admin" && m.role === "member");
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Group info" onBack={() => dispatch({ type: "OPEN_THREAD", conversationId })} />
      <div className={styles.hero}>
        <GroupGlyph memberSeeds={group.members.filter((m) => !m.isSelf).map((m) => m.handle)} size={72} />
        {renaming === null ? (
          <div className={`${styles.label} type-title`}>{group.label}</div>
        ) : (
          <input className={styles.renameInput} value={renaming} maxLength={100} onChange={(e) => setRenaming(e.target.value)} aria-label="Group name" />
        )}
        <GroupVerifiedBadge group={group} contactsById={contactsById} />
        {canManage &&
          (renaming === null ? (
            <button type="button" className={styles.linkButton} onClick={() => setRenaming(group.label)}>
              Rename
            </button>
          ) : (
            <button
              type="button"
              className={styles.linkButton}
              disabled={busy || !renaming.trim()}
              onClick={() =>
                run(async () => {
                  await renameGroup(dispatch, identity, group, renaming);
                  setRenaming(null);
                }, "Couldn't rename the group — try again.")
              }
            >
              Save name
            </button>
          ))}
      </div>
      <div className={styles.body}>
        {error && <p className={styles.error}>{error}</p>}
        <span className={`${styles.sectionLabel} type-caption`}>MEMBERS ({group.members.length})</span>
        {group.members.map((m) => (
          <div key={m.userId} className={styles.memberRow}>
            <Avatar avatarUrl={m.avatarUrl} seed={m.handle} size={32} />
            <button
              type="button"
              className={`${styles.memberName} ${styles.memberNameButton} type-row-primary`}
              disabled={m.isSelf}
              onClick={() => dispatch({ type: "OPEN_PROFILE", userId: m.userId, origin: { screen: "group-info", conversationId } })}
            >
              {m.isSelf ? `${m.handle} (you)` : m.handle}
            </button>
            {ROLE_LABEL[m.role] && <span className="type-caption">{ROLE_LABEL[m.role]}</span>}
            {group.myRole === "owner" && !m.isSelf && m.role !== "owner" && (
              <button
                type="button"
                className={styles.linkButton}
                disabled={busy}
                onClick={() => run(() => setGroupRole(dispatch, identity, group, m.userId, m.role === "admin" ? "member" : "admin"), "Couldn't change their role.")}
              >
                {m.role === "admin" ? "Remove admin" : "Make admin"}
              </button>
            )}
            {canManage && canRemove(m) && (
              <button
                type="button"
                className={styles.linkButton}
                disabled={busy}
                onClick={() => {
                  if (window.confirm(`Remove ${m.handle} from the group?`)) {
                    run(() => removeGroupMember(dispatch, identity, group, m.userId), "Couldn't remove them.");
                  }
                }}
              >
                Remove
              </button>
            )}
          </div>
        ))}

        {canManage && (
          <>
            <button type="button" className={styles.linkButton} onClick={() => setAdding((a) => !a)}>
              {adding ? "Done adding" : "Add friends"}
            </button>
            {adding && addable.length === 0 && <p className="type-secondary">All your friends are already in this group.</p>}
            {adding &&
              addable.map((c) => (
                <div key={c.id} className={styles.memberRow}>
                  <Avatar avatarUrl={c.avatarUrl} seed={c.handle} size={32} />
                  <span className={`${styles.memberName} type-row-primary`}>{c.handle}</span>
                  <button
                    type="button"
                    className={styles.linkButton}
                    disabled={busy}
                    onClick={() => run(() => addGroupMember(dispatch, identity, group, c.userId!), "Couldn't add them.")}
                  >
                    Add
                  </button>
                </div>
              ))}
            {adding && <p className="type-secondary">New members only see messages sent after they join.</p>}
          </>
        )}

        <GroupInviteLink group={group} canManage={canManage} />
      </div>
      <button
        type="button"
        className={styles.leaveButton}
        disabled={busy}
        onClick={() => {
          if (window.confirm("Leave this group? You'll stop receiving its messages.")) {
            run(() => leaveGroup(dispatch, identity, group), "Couldn't leave the group — try again.");
          }
        }}
      >
        Leave group
      </button>
    </div>
  );
}
