// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import type { ReactNode } from "react";
import { onOnionOrigin } from "../lib/torRouting.js";
import styles from "./TorGate.module.css";

/**
 * The app's encryption (libsodium: key derivation, sealed boxes, signatures) is WebAssembly, with no
 * fallback. Some browsers don't offer it: Tor Browser removes it on non-HTTPS pages at its "Safer"
 * and "Safest" security levels, and an onion address is plain http. Without this, the app would
 * start and then fail at the first sign-in with a message about a wrong password. This says what's
 * actually wrong, and what to do about it.
 */
export function WebAssemblyGate({ children }: { children: ReactNode }) {
  if (typeof WebAssembly === "object" && WebAssembly !== null) return <>{children}</>;
  const onion = onOnionOrigin();
  return (
    <main className={styles.page}>
      <section className={styles.card} aria-labelledby="no-wasm-title">
        <h1 id="no-wasm-title" className="type-display">
          This browser can't run Redvault's encryption
        </h1>
        <p>
          Redvault encrypts everything on your device using WebAssembly, and this browser has it turned off, so the app can't start
          here. Nothing was sent anywhere.
        </p>
        {onion ? (
          <p>
            In Tor Browser this happens at the <strong>Safer</strong> and <strong>Safest</strong> security levels. Open the shield
            icon in the toolbar (or Settings, Privacy &amp; Security), choose <strong>Standard</strong>, and reload this page.
          </p>
        ) : (
          <p>Turn WebAssembly back on in this browser's settings, or open Redvault in another browser.</p>
        )}
        <p className={styles.muted}>
          The Redvault apps for Windows, Linux and Android don't depend on the browser and are the most private way to use it.
        </p>
      </section>
    </main>
  );
}
