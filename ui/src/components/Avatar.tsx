// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { resolveApiUrl } from "../lib/apiBase.js";
import { avatarTint, initials } from "../lib/avatarTint.js";
import styles from "./Avatar.module.css";

export interface AvatarProps {
  /** Real, server-hosted profile picture — visible to every user. Falls back to initials on a tint when absent. */
  avatarUrl?: string | null;
  seed: string;
  size?: number;
  /** People are circles; groups are rounded squares. */
  shape?: "circle" | "square";
  /** Shows a green dot: the contact shares their online status and is online now. */
  online?: boolean;
  /** Draws the accent ring the design uses for the selected/favorite contact. */
  ring?: boolean;
}

export function Avatar({ avatarUrl, seed, size = 40, shape = "circle", online, ring }: AvatarProps) {
  const radius = shape === "circle" ? "50%" : Math.round(size * 0.3);
  return (
    <span
      className={`${styles.wrap} ${ring ? styles.ring : ""}`}
      style={{ width: size, height: size, borderRadius: radius, flexShrink: 0 }}
    >
      {avatarUrl ? (
        <img
          src={resolveApiUrl(avatarUrl)}
          alt=""
          referrerPolicy="no-referrer"
          width={size}
          height={size}
          className={styles.image}
          style={{ borderRadius: radius }}
        />
      ) : (
        <span
          className={styles.initials}
          aria-hidden="true"
          style={{ background: avatarTint(seed), borderRadius: radius, fontSize: Math.max(11, Math.round(size * 0.36)) }}
        >
          {initials(seed)}
        </span>
      )}
      {online && <span className={styles.online} aria-label="Online" role="img" style={{ width: Math.max(10, size * 0.26), height: Math.max(10, size * 0.26) }} />}
    </span>
  );
}
