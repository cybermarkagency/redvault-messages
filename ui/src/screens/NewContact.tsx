import { useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { TopBar } from "../components/TopBar.js";
import { lookupContact, sendFriendRequest, type ContactPreview } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import type { DMConversation } from "../types.js";
import styles from "./NewContact.module.css";

/** "Add friend" — lookup-first UX: look up a handle, see a preview, then send a friend request. Messaging unlocks once they accept (state/actions.ts's acceptFriendRequest). */
export function NewContact() {
  const { state, dispatch } = useAppState();
  const [handle, setHandle] = useState("");
  const [looking, setLooking] = useState(false);
  const [preview, setPreview] = useState<ContactPreview | null>(null);
  const [sending, setSending] = useState(false);

  async function submit() {
    if (!state.identity || !handle.trim() || looking) return;
    setLooking(true);
    setPreview(null);
    try {
      const result = await lookupContact(dispatch, state.identity, handle);
      setPreview(result);
    } finally {
      setLooking(false);
    }
  }

  if (!state.identity) return null;

  const existingConversation = preview
    ? state.conversations.find((c): c is DMConversation => c.kind === "dm" && c.userId === preview.userId)
    : undefined;
  const outgoingRequest = preview
    ? state.friendRequestsOutgoing.find((r) => r.userId === preview.userId)
    : undefined;
  const incomingRequest = preview
    ? state.friendRequestsIncoming.find((r) => r.userId === preview.userId)
    : undefined;

  async function handleSend() {
    if (!preview) return;
    setSending(true);
    try {
      await sendFriendRequest(dispatch, preview.userId, preview.handle, preview.avatarUrl);
    } catch {
      // Error surfaced via state.contactError below.
    } finally {
      setSending(false);
    }
  }

  function openProfile() {
    if (!preview) return;
    dispatch({ type: "OPEN_PROFILE", userId: preview.userId, origin: { screen: "new-contact" } });
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Add friend" onBack={() => dispatch({ type: "OPEN_NEW" })} />
      <div className={styles.body}>
        <p className={`${styles.hint} type-secondary`}>
          Look up the handle they shared out-of-band, then send a friend request. Messaging
          unlocks once they accept.
        </p>
        <input
          className={styles.textarea}
          style={{ minHeight: "auto" }}
          placeholder="their-handle·xxxx"
          value={handle}
          onChange={(e) => {
            setHandle(e.target.value);
            setPreview(null);
          }}
          autoCapitalize="none"
          autoCorrect="off"
        />
        {state.contactError && (
          <p className={`${styles.hint} type-secondary`} style={{ color: "var(--warn)" }}>
            {state.contactError}
          </p>
        )}
        <button type="button" className={styles.primaryButton} disabled={!handle.trim() || looking} onClick={submit}>
          {looking ? "Looking…" : "Look up"}
        </button>

        {preview?.isSelf && <p className={`${styles.hint} type-secondary`}>That's your own handle.</p>}

        {preview && !preview.isSelf && (
          <div className={styles.previewCard}>
            <button type="button" className={styles.previewHeader} onClick={openProfile}>
              <Avatar avatarUrl={preview.avatarUrl} seed={preview.handle} size={40} />
              <span className={`${styles.previewHandle} type-mono`}>{preview.handle}</span>
            </button>

            {preview.deleted ? (
              <p className={`${styles.hint} type-secondary`}>This account has been deleted.</p>
            ) : existingConversation ? (
              <button
                type="button"
                className={styles.primaryButton}
                onClick={() => dispatch({ type: "OPEN_THREAD", conversationId: existingConversation.id })}
              >
                Already friends — open chat
              </button>
            ) : incomingRequest ? (
              <button type="button" className={styles.primaryButton} onClick={openProfile}>
                They already sent you a request — view profile
              </button>
            ) : outgoingRequest ? (
              <button type="button" className={styles.primaryButton} disabled>
                Request sent
              </button>
            ) : (
              <button type="button" className={styles.primaryButton} disabled={sending} onClick={handleSend}>
                {sending ? "Sending…" : "Send friend request"}
              </button>
            )}
            <button type="button" className={styles.secondaryButton} onClick={openProfile}>
              View profile
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
