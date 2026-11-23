// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { type ReactNode, useState } from "react";
import { OpenSourceDialog, SourceCodeLink } from "../components/OpenSourceDialog.js";
import { logout } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import { TIER_LABELS } from "../types.js";
import styles from "./Site.module.css";

/** The header and footer every page of the website shares. */
export function SiteFrame({ children }: { children: ReactNode }) {
  const { state, dispatch } = useAppState();
  const identity = state.authStatus === "signed-in" ? state.identity : null;
  const [licences, setLicences] = useState(false);

  return (
    <div className={styles.site}>
      <header className={styles.header}>
        <button type="button" className={`${styles.brand} type-wordmark`} onClick={() => dispatch({ type: "NAVIGATE", screen: { key: "home" } })}>
          RedVault Messages
        </button>
        <span className={styles.headerSpacer} />
        {identity ? (
          <>
            <span className={styles.who}>
              {identity.handle} · {TIER_LABELS[identity.subscriptionTier] ?? "Free"}
            </span>
            <button type="button" className={styles.headerButtonQuiet} onClick={() => void logout(dispatch)}>
              Sign out
            </button>
          </>
        ) : (
          state.screen.key !== "sign-in" && (
            <button type="button" className={styles.headerButton} onClick={() => dispatch({ type: "NAVIGATE", screen: { key: "sign-in" } })}>
              Sign in
            </button>
          )
        )}
      </header>
      <main className={styles.main}>{children}</main>
      <footer className={styles.footer}>
        {/* The same product links as the footer of the rest of redvaultsystems.com (same-site paths). */}
        <nav className={styles.footerProducts} aria-label="Products">
          <a className={styles.footerLink} href="/cloud-agent">
            RedVaultCloud
          </a>
          <a className={styles.footerLink} href="/backup-agent">
            RedVaultBackup
          </a>
          <a className={styles.footerLink} href="/encrypted-messages/">
            RedVaultMessages
          </a>
        </nav>
        <a className={styles.footerLink} href="terms.html" target="_blank" rel="noopener noreferrer">
          Terms of Service
        </a>
        <a className={styles.footerLink} href="privacy.html" target="_blank" rel="noopener noreferrer">
          Privacy Policy
        </a>
        <button type="button" className={styles.footerLink} onClick={() => dispatch({ type: "NAVIGATE", screen: { key: "transparency" } })}>
          Transparency feed
        </button>
        <SourceCodeLink className={styles.footerLink} />
        <button type="button" className={styles.footerLink} onClick={() => setLicences(true)}>
          Open-source licences
        </button>
        <span>RedVault Messages is free software (AGPL-3.0-only).</span>
      </footer>
      {licences && <OpenSourceDialog onClose={() => setLicences(false)} />}
    </div>
  );
}
