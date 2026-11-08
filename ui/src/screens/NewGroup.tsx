// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useState } from "react";
import { Avatar } from "../components/Avatar.js";
import { CheckIcon } from "../components/icons.js";
import { TopBar } from "../components/TopBar.js";
import { DMVerifiedBadge } from "../components/VerifiedBadge.js";
import { createGroup } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./NewGroup.module.css";

/** Pick a name and some of your friends. The group key is sealed for each of them; the name is encrypted with it. */
export function NewGroup() {
  const { state, dispatch, contactsById } = useAppState();
  const { name, memberIds } = state.newGroupDraft;
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const contacts = [...contactsById.values()].filter((c) => c.userId);
  const canCreate = name.trim().length > 0 && memberIds.length >= 1 && !busy;

  async function create() {
    if (!canCreate || !state.identity) return;
    const userIds = memberIds.map((id) => contactsById.get(id)?.userId).filter((id): id is string => Boolean(id));
    setBusy(true);
    setError(null);
    try {
      await createGroup(dispatch, state.identity, name, userIds);
    } catch (err) {
      setError(err instanceof Error && err.message ? err.message : "Couldn't create the group — try again.");
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className={styles.screen}>
      <TopBar title="New group" onBack={() => dispatch({ type: "OPEN_NEW" })} />
      <div className={styles.body}>
        <input
          className={styles.nameInput}
          placeholder="Group name"
          maxLength={100}
          value={name}
          onChange={(e) => dispatch({ type: "UPDATE_NEW_GROUP_NAME", name: e.target.value })}
        />

        <span className={`${styles.sectionLabel} type-caption`}>MEMBERS ({memberIds.length} selected)</span>
        <div className={styles.memberList}>
          {contacts.length === 0 && <p className="type-secondary">Add a friend first — groups can only include your friends.</p>}
          {contacts.map((contact) => {
            const selected = memberIds.includes(contact.id);
            return (
              <button
                key={contact.id}
                type="button"
                className={`${styles.memberRow} ${selected ? styles.memberRowSelected : ""}`}
                onClick={() => dispatch({ type: "TOGGLE_NEW_GROUP_MEMBER", contactId: contact.id })}
              >
                <Avatar avatarUrl={contact.avatarUrl} seed={contact.handle} size={32} />
                <span className={styles.memberBody}>
                  <div className={`${styles.memberName} type-row-primary`}>{contact.handle}</div>
                </span>
                <DMVerifiedBadge contact={contact} />
                <span className={`${styles.checkbox} ${selected ? styles.checkboxChecked : ""}`}>
                  {selected && <CheckIcon />}
                </span>
              </button>
            );
          })}
        </div>
        {error && <p className="type-secondary" style={{ color: "var(--danger)" }}>{error}</p>}
      </div>
      <div className={styles.footer}>
        <button type="button" className={styles.createButton} disabled={!canCreate} onClick={create}>
          {busy ? "Creating…" : "Create"}
        </button>
      </div>
    </div>
  );
}
