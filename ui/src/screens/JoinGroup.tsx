import { useEffect, useState } from "react";
import { ChevronLeftIcon, LockIcon, UsersIcon } from "../components/icons24.js";
import * as api from "../lib/api.js";
import { ready } from "../lib/e2eCrypto.js";
import { inviteKeys, inviteSecretFromFragment, openGroupInvitePreview } from "../lib/groupInviteCrypto.js";
import { joinGroupFromInvite } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./JoinCall.module.css";

interface InvitePreview {
  conversationId: string;
  name: string;
  memberCount: number;
  approval: boolean;
  groupKey: Uint8Array | null;
  secret: Uint8Array;
  member: boolean;
  requested: boolean;
}

/** Someone opened a group invite link: the group's name and size, and "Join group" (or "Ask to join"). */
export function JoinGroup({ linkId, secret }: { linkId: string; secret: string }) {
  const { state, dispatch } = useAppState();
  const [preview, setPreview] = useState<InvitePreview | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      await ready();
      const key = inviteSecretFromFragment(secret);
      if (!key) {
        setError("This link is incomplete. Ask for it again, and open the whole link.");
        return;
      }
      try {
        const info = await api.openGroupInvite(linkId, inviteKeys(key).auth);
        let opened: { name: string; groupKey: Uint8Array | null };
        try {
          opened = openGroupInvitePreview(info, key);
        } catch {
          if (!cancelled) setError("This link's secret doesn't match. It may have been reset: ask the group for a new one.");
          return;
        }
        if (!cancelled) {
          setPreview({ ...opened, conversationId: info.conversationId, memberCount: info.memberCount, approval: info.approval, secret: key, member: info.member, requested: info.requested });
        }
      } catch (err) {
        if (!cancelled) setError(err instanceof api.ApiError && err.status === 404 ? err.message : "Couldn't open this link. Try again.");
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [linkId, secret]);

  async function join() {
    if (!preview || !state.identity) return;
    setBusy(true);
    setError(null);
    try {
      const status = await joinGroupFromInvite(dispatch, state.identity, linkId, preview.secret, preview.groupKey);
      if (status === "requested") setPreview({ ...preview, requested: true });
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : "Couldn't join the group. Try again.");
    } finally {
      setBusy(false);
    }
  }

  async function cancelRequest() {
    if (!preview || !state.identity) return;
    setBusy(true);
    try {
      await api.denyGroupJoinRequest(preview.conversationId, state.identity.userId);
      setPreview({ ...preview, requested: false });
    } catch {
      setError("Couldn't cancel your request. Try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <button type="button" className={styles.back} aria-label="Back" onClick={() => dispatch({ type: "GO_CHATS" })}>
        <ChevronLeftIcon />
      </button>
      <div className={styles.card}>
        {!preview ? (
          error ? (
            <p className={styles.error} role="alert">
              {error}
            </p>
          ) : (
            <p className={styles.muted}>Opening the link…</p>
          )
        ) : (
          <>
            <UsersIcon size={40} />
            <h1 className={`${styles.title} type-display`}>{preview.name}</h1>
            <p className={styles.muted}>
              Group · {preview.memberCount} {preview.memberCount === 1 ? "member" : "members"}
            </p>
            <p className={styles.note}>
              <LockIcon size={14} /> Messages in this group are end-to-end encrypted. You'll only see messages sent after you join.
              {preview.approval && " An admin of the group has to approve you first."}
            </p>
            {error && (
              <p className={styles.error} role="alert">
                {error}
              </p>
            )}
            {preview.member ? (
              <button type="button" className={styles.join} onClick={() => dispatch({ type: "OPEN_THREAD", conversationId: preview.conversationId })}>
                Open group
              </button>
            ) : preview.requested ? (
              <>
                <p className={styles.muted}>Request sent. The group will appear in your chats once an admin approves it.</p>
                <button type="button" className={styles.secondary} disabled={busy} onClick={cancelRequest}>
                  Cancel request
                </button>
              </>
            ) : (
              <button type="button" className={styles.join} disabled={busy} onClick={join}>
                {busy ? "Joining…" : preview.approval ? "Ask to join" : "Join group"}
              </button>
            )}
          </>
        )}
      </div>
    </div>
  );
}
