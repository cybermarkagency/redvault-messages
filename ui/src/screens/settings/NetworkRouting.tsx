// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useState } from "react";
import { GlobeIcon, PhoneIcon } from "../../components/icons24.js";
import { useAppState } from "../../state/AppStateContext.js";
import { fetchRouting, onOnionOrigin, TOR_UNAVAILABLE_MESSAGE, webRtcAvailable, type RoutingInfo } from "../../lib/torRouting.js";
import styles from "./SettingsPages.module.css";

const TOR_FEATURES = ["Text messages", "Encrypted photos", "Encrypted files", "Private Stories", "Voice messages"];

/**
 * Settings › Network routing (docs/tor-routing-plan.md): how each kind of
 * traffic leaves this browser. Read-only on purpose: there's no switch that
 * sends Tor-only features any other way.
 */
export function NetworkRouting() {
  const { state } = useAppState();
  const onion = onOnionOrigin();
  const [routing, setRouting] = useState<RoutingInfo | null>(null);
  useEffect(() => {
    let cancelled = false;
    void fetchRouting().then((info) => {
      if (!cancelled) setRouting(info);
    });
    return () => {
      cancelled = true;
    };
  }, []);

  const route = onion ? "Tor only" : "HTTPS";
  const calls = webRtcAvailable() ? "WebRTC + TURN relay" : "Not available here";
  return (
    <>
      <p className={styles.lead}>
        {onion
          ? "You're using Redvault at its onion address, so everything this page sends goes through Tor Browser's Tor connection, to this site only. Nothing is sent any other way: if Tor or the service can't be reached, what you write waits here, encrypted, and goes once it can."
          : "You're using Redvault at its regular web address: messages go over HTTPS to the message relay, which sees your IP address. For Tor, open the onion address in Tor Browser."}
      </p>
      {state.outbox.offline && <p className={styles.warn}>{onion ? TOR_UNAVAILABLE_MESSAGE : "Can't reach the server right now."}</p>}
      <h3 className={`${styles.subhead} type-caption`}>HOW EACH KIND OF TRAFFIC TRAVELS</h3>
      <div className={styles.group}>
        {TOR_FEATURES.map((feature) => (
          <div key={feature} className={styles.row}>
            <span className={styles.person}>
              <GlobeIcon size={20} />
              <span className={styles.rowText}>
                <span className={styles.rowTitle}>{feature}</span>
                <span className={styles.rowDetail}>End-to-end encrypted</span>
              </span>
            </span>
            <span className={onion ? styles.ok : styles.rowDetail}>{route}</span>
          </div>
        ))}
        <div className={styles.row}>
          <span className={styles.person}>
            <PhoneIcon size={20} />
            <span className={styles.rowText}>
              <span className={styles.rowTitle}>Voice and video calls</span>
              <span className={styles.rowDetail}>End-to-end encrypted media, through the TURN relay</span>
            </span>
          </span>
          <span className={styles.rowDetail}>{calls}</span>
        </div>
      </div>
      <p className={styles.lead}>
        Calls never go through Tor: Tor can't carry live audio and video well. They're relayed through the call server's TURN relay, so
        the people you call don't learn your IP address, but the relay does while the call lasts. Tor Browser turns WebRTC off entirely
        (it would reveal your IP address), so calls only work in the desktop and Android apps there.
      </p>
      <p className={styles.lead}>
        Tor hides your IP address from the message server; it doesn't make you anonymous by itself. What you write, who you write to and
        your device can still identify you.
      </p>
      {routing && routing.mode !== "off" && routing.onionAddress && !onion && (
        <>
          <h3 className={`${styles.subhead} type-caption`}>ONION ADDRESS</h3>
          <div className={styles.group}>
            <div className={styles.row}>
              <span className={styles.mono}>{routing.onionAddress}</span>
            </div>
          </div>
        </>
      )}
    </>
  );
}
