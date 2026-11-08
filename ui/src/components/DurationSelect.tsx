// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { DURATION_KEYS, DURATION_LABELS, type DurationKey } from "../types.js";
import styles from "./DurationSelect.module.css";

export interface DurationSelectProps {
  value: DurationKey;
  onChange: (value: DurationKey) => void;
  "aria-label"?: string;
}

export function DurationSelect({ value, onChange, ...aria }: DurationSelectProps) {
  return (
    <div className={styles.track} role="radiogroup" aria-label={aria["aria-label"] ?? "Disappearing messages"}>
      {DURATION_KEYS.map((key) => (
        <button
          key={key}
          type="button"
          role="radio"
          aria-checked={value === key}
          className={`${styles.option} ${value === key ? styles.optionActive : ""}`}
          onClick={() => onChange(key)}
        >
          {DURATION_LABELS[key]}
        </button>
      ))}
    </div>
  );
}
