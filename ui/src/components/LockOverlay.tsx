// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { LockIcon } from "./icons.js";
import styles from "./LockOverlay.module.css";

export interface LockOverlayProps {
  onUnlock: () => void;
}

/**
 * Covers the entire conversation surface. Backs both the manual/timeout
 * lock and the OS app-switcher thumbnail — one rendering, so there's
 * exactly one "locked" state to build and audit, not two.
 */
export function LockOverlay({ onUnlock }: LockOverlayProps) {
  return (
    <div className={styles.overlay}>
      <div className={styles.iconWrap}>
        <LockIcon width="1.4em" height="1.4em" />
      </div>
      <div className={`${styles.label} type-title`}>Redvaultsystem is locked</div>
      <p className={`${styles.hint} type-secondary`}>
        Conversations, previews, and notifications stay hidden until you unlock.
      </p>
      <button type="button" className={styles.unlockButton} onClick={onUnlock}>
        Unlock
      </button>
    </div>
  );
}
