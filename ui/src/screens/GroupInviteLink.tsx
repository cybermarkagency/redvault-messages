// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { ToggleSwitch } from "../components/ToggleSwitch.js";
import * as api from "../lib/api.js";
import { ready } from "../lib/e2eCrypto.js";
import { newGroupInviteLink, openGroupInviteLink, wrapGroupKeyForLink } from "../lib/groupInviteCrypto.js";
import { approveJoinRequest } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import type { GroupConversation } from "../types.js";
import styles from "./GroupInfo.module.css";
import linkStyles from "./GroupInviteLink.module.css";

interface Request {
  userId: string;
  handle: string;
  avatarUrl: string | null;
}

/**
 * Group info > Invite link: every member can copy the group's link; the owner
 * and admins turn it on or off, reset it, choose whether they approve each
 * join, and answer requests to join.
 */
export function GroupInviteLink({ group, canManage }: { group: GroupConversation; canManage: boolean }) {
  const { state, dispatch } = useAppState();
  const identity = state.identity!;
  const groupKey = group.conversationKey;
  /** undefined while loading. */
  const [link, setLink] = useState<api.GroupInviteLinkDto | null | undefined>(undefined);
  const [opened, setOpened] = useState<{ secret: Uint8Array; url: string } | null>(null);
  const [requests, setRequests] = useState<Request[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [copied, setCopied] = useState(false);

  const showLink = useCallback(
    (next: api.GroupInviteLinkDto | null) => {
      setLink(next);
      setOpened(next && groupKey ? openGroupInviteLink(next, groupKey) : null);
    },
    [groupKey]
  );

  const loadRequests = useCallback(async () => {
    if (!canManage) return;
    const pending = await api.listGroupJoinRequests(group.id);
    const named = await Promise.all(
      pending.map(async (r) => {
        const user = await api.getUser(r.userId).catch(() => null);
        return { userId: r.userId, handle: user?.username ?? "Unknown account", avatarUrl: user?.avatarUrl ?? null };
      })
    );
    setRequests(named);
  }, [canManage, group.id]);

  useEffect(() => {
    let cancelled = false;
    (async () => {
      await ready();
      try {
        const { link: current } = await api.getGroupInviteLink(group.id);
        if (!cancelled) showLink(current);
        if (!cancelled) await loadRequests();
      } catch {
        if (!cancelled) setLink(null);
      }
    })();
    return () => {
      cancelled = true;
    };
  }, [group.id, showLink, loadRequests]);

  async function run(action: () => Promise<void>, failure: string) {
    setBusy(true);
    setError(null);
    try {
      await action();
    } catch (err) {
      setError(err instanceof api.ApiError ? err.message : failure);
    } finally {
      setBusy(false);
    }
  }

  /** A new link (turning it on, or a reset), keeping the approval setting. */
  async function newLink(approval: boolean) {
    if (!groupKey) return;
    await ready();
    const { input } = newGroupInviteLink(groupKey, group.label, approval);
    showLink((await api.setGroupInviteLink(group.id, input)).link);
  }

  async function setApproval(approval: boolean) {
    if (!groupKey || !opened) return;
    const wrapped = approval ? undefined : wrapGroupKeyForLink(groupKey, opened.secret);
    showLink((await api.setGroupInviteApproval(group.id, approval, wrapped)).link);
  }

  async function copy() {
    if (!opened) return;
    try {
      await navigator.clipboard.writeText(opened.url);
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } catch {
      setError("Couldn't copy. Select the link and copy it yourself.");
    }
  }

  if (link === undefined || (!link && !canManage)) return null;

  return (
    <>
      <span className={`${styles.sectionLabel} type-caption`}>INVITE LINK</span>
      {error && <p className={styles.error}>{error}</p>}
      {!link ? (
        <div className={linkStyles.panel}>
          <p className={`${linkStyles.hint} type-secondary`}>
            Anyone with the link can join, or ask to join if you approve new members. The group stays end-to-end encrypted: the link's
            secret never reaches the server.
          </p>
          <button type="button" className={linkStyles.primary} disabled={busy || !groupKey} onClick={() => run(() => newLink(true), "Couldn't create the link.")}>
            Create invite link
          </button>
        </div>
      ) : (
        <div className={linkStyles.panel}>
          {opened ? (
            <div className={linkStyles.linkRow}>
              <input className={linkStyles.url} readOnly value={opened.url} aria-label="Invite link" onFocus={(e) => e.currentTarget.select()} />
              <button type="button" className={linkStyles.primary} onClick={copy}>
                {copied ? "Copied" : "Copy"}
              </button>
            </div>
          ) : (
            <p className="type-secondary">This group's link couldn't be opened on this device.</p>
          )}
          {canManage && (
            <>
              <div className={linkStyles.toggleRow}>
                <span>
                  <div className="type-row-primary">Approve new members</div>
                  <div className="type-secondary">
                    {link.approval ? "An admin lets in each person who opens the link." : "Anyone who opens the link joins straight away."}
                  </div>
                </span>
                <ToggleSwitch
                  label="Approve new members"
                  checked={link.approval}
                  onChange={(v) => run(() => setApproval(v), "Couldn't change that setting.")}
                />
              </div>
              <div className={linkStyles.actions}>
                <button
                  type="button"
                  className={styles.linkButton}
                  disabled={busy}
                  onClick={() => {
                    if (window.confirm("Reset the invite link? The current link stops working.")) run(() => newLink(link.approval), "Couldn't reset the link.");
                  }}
                >
                  Reset link
                </button>
                <button
                  type="button"
                  className={styles.linkButton}
                  disabled={busy}
                  onClick={() => {
                    if (window.confirm("Turn off the invite link? It stops working, and pending requests are dropped.")) {
                      run(async () => {
                        await api.deleteGroupInviteLink(group.id);
                        showLink(null);
                        setRequests([]);
                      }, "Couldn't turn off the link.");
                    }
                  }}
                >
                  Turn off link
                </button>
              </div>
            </>
          )}
        </div>
      )}

      {canManage && requests.length > 0 && (
        <>
          <span className={`${styles.sectionLabel} type-caption`}>REQUESTS TO JOIN ({requests.length})</span>
          {requests.map((r) => (
            <div key={r.userId} className={styles.memberRow}>
              <Avatar avatarUrl={r.avatarUrl} seed={r.handle} size={32} />
              <span className={`${styles.memberName} type-row-primary`}>{r.handle}</span>
              <button
                type="button"
                className={styles.linkButton}
                disabled={busy}
                onClick={() =>
                  run(async () => {
                    await approveJoinRequest(dispatch, identity, group, r.userId);
                    await loadRequests();
                  }, "Couldn't let them in.")
                }
              >
                Approve
              </button>
              <button
                type="button"
                className={styles.linkButton}
                disabled={busy}
                onClick={() =>
                  run(async () => {
                    await api.denyGroupJoinRequest(group.id, r.userId);
                    await loadRequests();
                  }, "Couldn't decline the request.")
                }
              >
                Decline
              </button>
            </div>
          ))}
        </>
      )}
    </>
  );
}
