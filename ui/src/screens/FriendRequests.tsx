// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { TopBar } from "../components/TopBar.js";
import { acceptFriendRequest, cancelFriendRequest, denyFriendRequest } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./FriendRequests.module.css";

export function FriendRequests() {
  const { state, dispatch } = useAppState();
  const { identity, friendRequestsIncoming, friendRequestsOutgoing } = state;
  const [busyId, setBusyId] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  if (!identity) return null;

  async function handleAccept(requestId: string, requesterId: string) {
    setBusyId(requestId);
    setError(null);
    try {
      await acceptFriendRequest(dispatch, identity!, requestId, requesterId, state.settings.defaultDisappearing);
    } catch {
      setError("Couldn't accept that request — try again.");
    } finally {
      setBusyId(null);
    }
  }

  async function handleDeny(requestId: string) {
    setBusyId(requestId);
    setError(null);
    try {
      await denyFriendRequest(dispatch, requestId);
    } catch {
      setError("Couldn't deny that request — try again.");
    } finally {
      setBusyId(null);
    }
  }

  async function handleCancel(requestId: string) {
    setBusyId(requestId);
    setError(null);
    try {
      await cancelFriendRequest(dispatch, requestId);
    } catch {
      setError("Couldn't cancel that request — try again.");
    } finally {
      setBusyId(null);
    }
  }

  function openProfile(userId: string) {
    dispatch({ type: "OPEN_PROFILE", userId, origin: { screen: "friend-requests" } });
  }

  return (
    <div className={styles.screen}>
      <TopBar title="Friend requests" onBack={() => dispatch({ type: "GO_CHATS" })} />
      <div className={styles.body}>
        {error && <p className={styles.error}>{error}</p>}

        <span className={`${styles.sectionLabel} type-caption`}>REQUESTS</span>
        {friendRequestsIncoming.length === 0 ? (
          <p className={`${styles.empty} type-secondary`}>No pending requests.</p>
        ) : (
          friendRequestsIncoming.map((r) => {
            const busy = busyId === r.id;
            return (
              <div key={r.id} className={styles.card}>
                <button type="button" className={styles.header} onClick={() => openProfile(r.userId)}>
                  <Avatar avatarUrl={r.avatarUrl} seed={r.handle} size={40} />
                  <span className={`${styles.handle} type-mono`}>{r.handle}</span>
                </button>
                <div className={styles.actions}>
                  <button
                    type="button"
                    className={styles.ignoreButton}
                    disabled={busy}
                    onClick={() => handleDeny(r.id)}
                  >
                    Deny
                  </button>
                  <button
                    type="button"
                    className={styles.acceptButton}
                    disabled={busy}
                    onClick={() => handleAccept(r.id, r.userId)}
                  >
                    Accept
                  </button>
                </div>
              </div>
            );
          })
        )}

        <span className={`${styles.sectionLabel} type-caption`}>SENT</span>
        {friendRequestsOutgoing.length === 0 ? (
          <p className={`${styles.empty} type-secondary`}>No outgoing requests.</p>
        ) : (
          friendRequestsOutgoing.map((r) => {
            const busy = busyId === r.id;
            return (
              <div key={r.id} className={styles.card}>
                <button type="button" className={styles.header} onClick={() => openProfile(r.userId)}>
                  <Avatar avatarUrl={r.avatarUrl} seed={r.handle} size={40} />
                  <span className={`${styles.handle} type-mono`}>{r.handle}</span>
                </button>
                <div className={styles.actions}>
                  <button
                    type="button"
                    className={styles.ignoreButton}
                    disabled={busy}
                    onClick={() => handleCancel(r.id)}
                  >
                    Cancel
                  </button>
                </div>
              </div>
            );
          })
        )}
      </div>
    </div>
  );
}
