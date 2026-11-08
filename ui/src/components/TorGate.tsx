// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState, type ReactNode } from "react";
import { fetchRouting, onionUrl, onOnionOrigin, type RoutingInfo } from "../lib/torRouting.js";
import styles from "./TorGate.module.css";

/**
 * Tor routing for the web app (docs/tor-routing-plan.md). On the onion
 * address (Tor Browser) it gets out of the way. On the clearnet address it
 * asks the server how Tor routing stands: in "notice" mode it says where the
 * app is moving and when; in "enforce" mode the app doesn't run here at all
 * (the server refuses the clearnet API anyway) and it says where to go
 * instead. Nothing here sends anything anywhere but this site's own API.
 */
export function TorGate({ children }: { children: ReactNode }) {
  const onion = onOnionOrigin();
  const [routing, setRouting] = useState<RoutingInfo | null>(null);

  useEffect(() => {
    if (onion) return;
    let cancelled = false;
    void fetchRouting().then((info) => {
      if (!cancelled) setRouting(info);
    });
    return () => {
      cancelled = true;
    };
  }, [onion]);

  if (onion || !routing || routing.mode === "off" || routing.mode === "report") return <>{children}</>;
  const url = routing.onionAddress ? onionUrl(routing.onionAddress) : null;

  if (routing.mode === "enforce") {
    return (
      <main className={styles.page}>
        <section className={styles.card} aria-labelledby="tor-required-title">
          <h1 id="tor-required-title" className="type-display">
            Open Redvault in Tor Browser
          </h1>
          <p>
            Messages, photos, files, Stories and voice messages now travel only through Tor, so this address can't be used for them
            anymore. Open the onion address below in Tor Browser and sign in there: your account, chats and keys are the same.
          </p>
          {url ? (
            <OnionAddress url={url} />
          ) : (
            <p className={styles.muted}>The onion address isn't set up on this server yet.</p>
          )}
          <p className={styles.muted}>
            Tor Browser is free from torproject.org. Voice and video calls aren't available in Tor Browser (it turns WebRTC off to protect
            your IP address): use the desktop or Android app for calls.
          </p>
        </section>
      </main>
    );
  }

  const cutoff = routing.cutoffAt ? new Date(routing.cutoffAt).toLocaleDateString(undefined, { dateStyle: "long" }) : null;
  return (
    <>
      <div className={styles.notice} role="status">
        <span>
          Redvault is moving to Tor{cutoff ? `: from ${cutoff}, this address won't work for messages anymore` : ""}. Open it in Tor
          Browser at the onion address{url ? ":" : "."}
        </span>
        {url && <OnionAddress url={url} compact />}
      </div>
      {children}
    </>
  );
}

function OnionAddress({ url, compact = false }: { url: string; compact?: boolean }) {
  const [copied, setCopied] = useState(false);
  return (
    <span className={compact ? styles.addressCompact : styles.address}>
      <code className={styles.code}>{url}</code>
      <button
        type="button"
        className={styles.copy}
        onClick={() => {
          void navigator.clipboard?.writeText(url).then(() => setCopied(true));
        }}
      >
        {copied ? "Copied" : "Copy"}
      </button>
    </span>
  );
}
