// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useAppState } from "../state/AppStateContext.js";
import styles from "./Site.module.css";
import { DownloadLinks } from "./DownloadLinks.js";

/** Signed out: what RedVault Messages is, where to get it, and what this website is still for. */
export function Landing() {
  const { dispatch } = useAppState();
  return (
    <div className={styles.page}>
      <h1 className={`${styles.title} type-display`}>RedVault Messages runs in the apps</h1>
      <p className={styles.lead}>
        End-to-end encrypted messages, calls, files, channels and communities are in the RedVault Messages apps for Android, Windows and Linux.
        Download the app for your device to create an account and start messaging.
      </p>
      <DownloadLinks />

      <div className={styles.cards}>
        <section className={styles.card} aria-labelledby="landing-plans">
          <h2 id="landing-plans" className={styles.cardTitle}>
            Plans and payments
          </h2>
          <p className={styles.text}>Sign in with your app account to upgrade to Pro or Premium, change or cancel your plan, and manage paid community memberships.</p>
          <div className={styles.actions}>
            <button type="button" className={styles.primary} onClick={() => dispatch({ type: "NAVIGATE", screen: { key: "plans" } })}>
              Sign in
            </button>
          </div>
        </section>
        <section className={styles.card} aria-labelledby="landing-transparency">
          <h2 id="landing-transparency" className={styles.cardTitle}>
            Transparency feed
          </h2>
          <p className={styles.text}>A live, public record of messages the server relays: commitments and, only when the sender opted in, ciphertext. Never plaintext.</p>
          <div className={styles.actions}>
            <button type="button" className={styles.secondary} onClick={() => dispatch({ type: "NAVIGATE", screen: { key: "transparency" } })}>
              View the feed
            </button>
          </div>
        </section>
      </div>

      <p className={styles.muted}>
        Accounts are created in the apps: there's no sign-up on the website. Site administrators sign in here for the admin panel.
      </p>
    </div>
  );
}
