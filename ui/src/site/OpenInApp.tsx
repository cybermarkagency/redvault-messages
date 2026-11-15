// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useState } from "react";
import { APP_LINK_NAMES, type AppLinkKind } from "../lib/siteLinks.js";
import { useAppState } from "../state/AppStateContext.js";
import styles from "./Site.module.css";
import { DownloadLinks } from "./DownloadLinks.js";

/**
 * A link that belongs to the apps (an invite, a call, a channel, a
 * community…). Its secret was taken out of the address bar as the page
 * opened and is never sent anywhere; Copy link puts the whole link, as it
 * was opened, on this device's clipboard to paste into the app.
 */
export function OpenInApp({
  what,
  communityId,
  inviteToken,
  originalLink,
}: {
  what: AppLinkKind;
  communityId?: string;
  inviteToken?: string;
  /** The address as it was opened, kept in memory only for Copy link. */
  originalLink: string | null;
}) {
  const { dispatch } = useAppState();
  const [copied, setCopied] = useState(false);

  return (
    <div className={styles.page}>
      <h1 className={`${styles.title} type-display`}>Open this in the RedVault Messages app</h1>
      <p className={styles.lead}>
        This is {APP_LINK_NAMES[what]}. It opens in the RedVault Messages app for Android, Windows or Linux: messaging isn't available on the website.
        Open the link from the app, or copy it and paste it there.
      </p>
      {originalLink && navigator.clipboard && (
        <div className={styles.actions}>
          <button
            type="button"
            className={styles.secondary}
            onClick={() => {
              void navigator.clipboard.writeText(originalLink).then(
                () => setCopied(true),
                () => setCopied(false)
              );
            }}
          >
            {copied ? "Copied" : "Copy link"}
          </button>
        </div>
      )}
      {what === "community" && communityId && (
        <section className={styles.card} aria-labelledby="buy-membership">
          <h2 id="buy-membership" className={styles.cardTitle}>
            Paid community?
          </h2>
          <p className={styles.text}>A membership to a paid community can be bought here in the browser, then used in the app.</p>
          <div className={styles.actions}>
            <button
              type="button"
              className={styles.primary}
              onClick={() => dispatch({ type: "NAVIGATE", screen: { key: "community", communityId, inviteToken } })}
            >
              Buy a membership
            </button>
          </div>
        </section>
      )}
      <h2 className={styles.cardTitle}>Don't have the app yet?</h2>
      <DownloadLinks />
    </div>
  );
}

/** Back from Stripe's payout setup for a creator: that's managed in the app. */
export function CreatorOnboardingReturn() {
  return (
    <div className={styles.page}>
      <h1 className={`${styles.title} type-display`}>Back to the app</h1>
      <p className={styles.lead}>
        You're back from Stripe. Go back to the RedVault Messages app: its creator dashboard shows whether your payout details are complete.
      </p>
      <DownloadLinks />
    </div>
  );
}
