// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { ReactNode } from "react";
import { BackIcon } from "./icons.js";
import styles from "./TopBar.module.css";

export interface TopBarProps {
  title: ReactNode;
  onBack?: () => void;
  actions?: ReactNode;
}

export function TopBar({ title, onBack, actions }: TopBarProps) {
  return (
    <div className={styles.bar}>
      {onBack && (
        <button type="button" className={styles.iconButton} onClick={onBack} aria-label="Back">
          <BackIcon />
        </button>
      )}
      <div className={`${styles.title} type-title`}>{title}</div>
      {actions && <div className={styles.actions}>{actions}</div>}
    </div>
  );
}
