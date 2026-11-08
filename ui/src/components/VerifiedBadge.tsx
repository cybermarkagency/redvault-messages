// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { DMConversation, GroupConversation } from "../types.js";
import { verifiedCount } from "../types.js";
import styles from "./VerifiedBadge.module.css";

export function DMVerifiedBadge({ contact }: { contact: Pick<DMConversation, "verified"> }) {
  return (
    <span className={`${styles.badge} ${contact.verified ? styles.verified : styles.unverified}`}>
      {contact.verified ? "Verified" : "Not verified"}
    </span>
  );
}

export function GroupVerifiedBadge({
  group,
  contactsById,
}: {
  group: GroupConversation;
  contactsById: Map<string, DMConversation>;
}) {
  const count = verifiedCount(group, contactsById);
  const total = group.members.filter((m) => !m.isSelf).length;
  const allVerified = count === total && total > 0;
  return (
    <span className={`${styles.badge} ${allVerified ? styles.verified : styles.unverified}`}>
      {count} of {total} verified
    </span>
  );
}
