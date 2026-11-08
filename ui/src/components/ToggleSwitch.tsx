// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import styles from "./ToggleSwitch.module.css";

export interface ToggleSwitchProps {
  checked: boolean;
  onChange: (checked: boolean) => void;
  label: string;
}

export function ToggleSwitch({ checked, onChange, label }: ToggleSwitchProps) {
  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-label={label}
      className={`${styles.track} ${checked ? styles.trackOn : ""}`}
      onClick={() => onChange(!checked)}
    >
      <span className={`${styles.thumb} ${checked ? styles.thumbOn : ""}`} />
    </button>
  );
}
