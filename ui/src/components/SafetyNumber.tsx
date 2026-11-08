// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import styles from "./SafetyNumber.module.css";

export interface SafetyNumberProps {
  /** 12 groups of 5 digits, from e2eCrypto.safetyNumberDigits(publicKeyA, publicKeyB) — a real fingerprint of both participants' actual keys, not a display-string stand-in. */
  groups: string[];
}

export function SafetyNumber({ groups }: SafetyNumberProps) {
  return (
    <div className={styles.grid} role="img" aria-label={`Safety number ${groups.join(" ")}`}>
      {groups.map((group, i) => (
        <span key={i} className={`${styles.group} type-mono`}>
          {group}
        </span>
      ))}
    </div>
  );
}
