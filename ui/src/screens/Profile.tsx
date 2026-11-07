import { useEffect, useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { SafetyNumber } from "../components/SafetyNumber.js";
import { HighlightsRow } from "../components/stories/Highlights.js";
import { CreatorOffer } from "../components/earnings/CreatorOffer.js";
import { ExemptionToggle } from "../components/earnings/ExemptionToggle.js";
import { StoryRing } from "../components/stories/StoryTray.js";
import { TopBar } from "../components/TopBar.js";
import * as api from "../lib/api.js";
import {
  acceptFriendRequest,
  cancelFriendRequest,
  denyFriendRequest,
  sendFriendRequest,
  submitReport,
} from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import { useStoriesOptional } from "../state/StoriesContext.js";
import type { DMConversation, ProfileOrigin } from "../types.js";
import styles from "./Profile.module.css";

function formatMemberSince(iso: string): string {
  return new Date(iso).toLocaleDateString(undefined, { year: "numeric", month: "long" });
}

/** Avatar/handle -> profile view: relationship-driven primary action (send/cancel/accept·deny/message), a safety-number section when a real conversation already exists, and inline "Report this user". Reached from Thread's DM header, GroupInfo's member rows, NewContact's preview card, and FriendRequests' cards. */
export function Profile({ userId, origin }: { userId: string; origin: ProfileOrigin }) {
  const { state, dispatch } = useAppState();
  const stories = useStoriesOptional();
  const [user, setUser] = useState<api.UserResult | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [actionError, setActionError] = useState<string | null>(null);

  const [reportOpen, setReportOpen] = useState(false);
  const [reportReason, setReportReason] = useState("");
  const [reportBusy, setReportBusy] = useState(false);
  const [reportError, setReportError] = useState<string | null>(null);
  const [reportSent, setReportSent] = useState(false);
  // null while loading: the Block/Unblock button waits for the real state.
  const [blocked, setBlocked] = useState<boolean | null>(null);
  const [blockBusy, setBlockBusy] = useState(false);
  const [blockError, setBlockError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setBlocked(null);
    api
      .listBlocked()
      .then((list) => {
        if (!cancelled) setBlocked(list.some((b) => b.userId === userId));
      })
      .catch(() => {
        if (!cancelled) setBlocked(false);
      });
    return () => {
      cancelled = true;
    };
  }, [userId]);

  async function toggleBlock() {
    if (blocked === null) return;
    if (!blocked && !window.confirm("Block this person? Neither of you will be able to message the other or send friend requests until you unblock them.")) return;
    setBlockBusy(true);
    setBlockError(null);
    try {
      if (blocked) await api.unblockUser(userId);
      else await api.blockUser(userId);
      setBlocked(!blocked);
    } catch {
      setBlockError(blocked ? "Couldn't unblock — try again." : "Couldn't block — try again.");
    } finally {
      setBlockBusy(false);
    }
  }

  useEffect(() => {
    let cancelled = false;
    setUser(null);
    setLoadError(null);
    api
      .getUser(userId)
      .then((u) => {
        if (!cancelled) setUser(u);
      })
      .catch(() => {
        if (!cancelled) setLoadError("Couldn't load that profile.");
      });
    return () => {
      cancelled = true;
    };
  }, [userId]);

  if (!state.identity) return null;
  const identity = state.identity;

  function goBack() {
    switch (origin.screen) {
      case "new-contact":
        dispatch({ type: "OPEN_NEW_CONTACT" });
        return;
      case "friend-requests":
        dispatch({ type: "OPEN_FRIEND_REQUESTS" });
        return;
      case "thread":
        dispatch({ type: "OPEN_THREAD", conversationId: origin.conversationId });
        return;
      case "group-info":
        dispatch({ type: "OPEN_GROUP_INFO", conversationId: origin.conversationId });
        return;
    }
  }

  const existingConversation = state.conversations.find(
    (c): c is DMConversation => c.kind === "dm" && c.userId === userId
  );
  const outgoingRequest = state.friendRequestsOutgoing.find((r) => r.userId === userId);
  const incomingRequest = state.friendRequestsIncoming.find((r) => r.userId === userId);
  const isSelf = userId === identity.userId;
  const theirStories = stories ? [...stories.authors, ...stories.muted].find((a) => a.author.userId === userId) : undefined;
  const hasStories = isSelf ? (stories?.own.length ?? 0) > 0 : !!theirStories;

  async function handleSendRequest() {
    if (!user) return;
    setBusy(true);
    setActionError(null);
    try {
      await sendFriendRequest(dispatch, user.userId, user.username, user.avatarUrl);
    } catch {
      setActionError("Couldn't send that friend request — try again.");
    } finally {
      setBusy(false);
    }
  }

  async function handleCancel() {
    if (!outgoingRequest) return;
    setBusy(true);
    setActionError(null);
    try {
      await cancelFriendRequest(dispatch, outgoingRequest.id);
    } catch {
      setActionError("Couldn't cancel that request — try again.");
    } finally {
      setBusy(false);
    }
  }

  async function handleAccept() {
    if (!incomingRequest) return;
    setBusy(true);
    setActionError(null);
    try {
      await acceptFriendRequest(
        dispatch,
        identity,
        incomingRequest.id,
        incomingRequest.userId,
        state.settings.defaultDisappearing
      );
    } catch {
      setActionError("Couldn't accept that request — try again.");
    } finally {
      setBusy(false);
    }
  }

  async function handleDeny() {
    if (!incomingRequest) return;
    setBusy(true);
    setActionError(null);
    try {
      await denyFriendRequest(dispatch, incomingRequest.id);
      goBack();
    } catch {
      setActionError("Couldn't deny that request — try again.");
    } finally {
      setBusy(false);
    }
  }

  async function handleSubmitReport() {
    if (!reportReason.trim()) return;
    setReportBusy(true);
    setReportError(null);
    try {
      await submitReport(userId, reportReason);
      setReportSent(true);
      setReportOpen(false);
      setReportReason("");
    } catch {
      setReportError("Couldn't submit that report — try again.");
    } finally {
      setReportBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Profile" onBack={goBack} />
      <div className={styles.body}>
        {loadError && <p className={styles.error}>{loadError}</p>}
        {!user && !loadError && <p className={`${styles.hint} type-secondary`}>Loading…</p>}

        {user && (
          <>
            <div className={styles.hero}>
              {hasStories && stories ? (
                <button
                  type="button"
                  className={styles.storyAvatar}
                  aria-label={isSelf ? "View your story" : `View ${user.username}'s story`}
                  onClick={() => stories.openViewer({ kind: "feed", authorId: isSelf ? "me" : userId })}
                >
                  <StoryRing state={isSelf || theirStories?.allSeen ? "seen" : "new"} size={72}>
                    <Avatar avatarUrl={user.avatarUrl} seed={user.username} size={72} />
                  </StoryRing>
                </button>
              ) : (
                <Avatar avatarUrl={user.avatarUrl} seed={user.username} size={72} />
              )}
              <div className={`${styles.handle} type-title`}>{user.username}</div>
              <div className={`${styles.memberSince} type-secondary`}>
                Member since {formatMemberSince(user.createdAt)}
              </div>
              {user.deleted && <div className={`${styles.deletedBadge} type-caption`}>This account has been deleted</div>}
            </div>

            {!user.deleted && <HighlightsRow ownerId={userId} />}
            {!isSelf && !user.deleted && <CreatorOffer userId={userId} name={user.username} />}
            {!isSelf && !user.deleted && <ExemptionToggle userId={userId} name={user.username} />}

            {existingConversation?.safetyNumber && (
              <div className={styles.section}>
                <span className={`${styles.sectionLabel} type-caption`}>SAFETY NUMBER</span>
                <SafetyNumber groups={existingConversation.safetyNumber} />
              </div>
            )}

            {actionError && <p className={styles.error}>{actionError}</p>}

            {!isSelf && !user.deleted && (
              <div className={styles.primaryAction}>
                {existingConversation ? (
                  <button
                    type="button"
                    className={styles.primaryButton}
                    onClick={() => dispatch({ type: "OPEN_THREAD", conversationId: existingConversation.id })}
                  >
                    Message
                  </button>
                ) : incomingRequest ? (
                  <div className={styles.actionRow}>
                    <button type="button" className={styles.secondaryButton} disabled={busy} onClick={handleDeny}>
                      Deny
                    </button>
                    <button type="button" className={styles.primaryButton} disabled={busy} onClick={handleAccept}>
                      Accept
                    </button>
                  </div>
                ) : outgoingRequest ? (
                  <button type="button" className={styles.secondaryButton} disabled={busy} onClick={handleCancel}>
                    Cancel request
                  </button>
                ) : (
                  <button type="button" className={styles.primaryButton} disabled={busy} onClick={handleSendRequest}>
                    Send friend request
                  </button>
                )}
              </div>
            )}

            {!isSelf && (
              <div className={styles.section}>
                {reportSent ? (
                  <p className={`${styles.hint} type-secondary`}>Report submitted. Thanks for flagging this.</p>
                ) : reportOpen ? (
                  <div className={styles.reportForm}>
                    <textarea
                      className={styles.reportTextarea}
                      placeholder="What's going on?"
                      value={reportReason}
                      onChange={(e) => setReportReason(e.target.value)}
                      maxLength={1000}
                    />
                    {reportError && <p className={styles.error}>{reportError}</p>}
                    <div className={styles.actionRow}>
                      <button
                        type="button"
                        className={styles.secondaryButton}
                        disabled={reportBusy}
                        onClick={() => {
                          setReportOpen(false);
                          setReportReason("");
                          setReportError(null);
                        }}
                      >
                        Cancel
                      </button>
                      <button
                        type="button"
                        className={styles.dangerButton}
                        disabled={reportBusy || !reportReason.trim()}
                        onClick={handleSubmitReport}
                      >
                        {reportBusy ? "Submitting…" : "Submit report"}
                      </button>
                    </div>
                  </div>
                ) : (
                  <button type="button" className={styles.reportLink} onClick={() => setReportOpen(true)}>
                    Report this user
                  </button>
                )}
                {blocked !== null && (
                  <button type="button" className={styles.reportLink} disabled={blockBusy} onClick={toggleBlock}>
                    {blockBusy ? "…" : blocked ? "Unblock this user" : "Block this user"}
                  </button>
                )}
                {blockError && <p className={styles.error}>{blockError}</p>}
              </div>
            )}
          </>
        )}
      </div>
    </div>
  );
}
