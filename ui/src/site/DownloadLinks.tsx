// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { APP_DOWNLOADS } from "../lib/downloads.js";
import styles from "./Site.module.css";

/** The three app downloads, as cards. */
export function DownloadLinks() {
  return (
    <div className={styles.downloads}>
      {APP_DOWNLOADS.map((d) => (
        <a key={d.platform} className={styles.download} href={d.url} rel="noopener noreferrer">
          <span className={styles.downloadPlatform}>{d.platform}</span>
          <span className={styles.downloadLabel}>{d.label}</span>
          <span className={styles.downloadFile}>{d.file}</span>
        </a>
      ))}
    </div>
  );
}
