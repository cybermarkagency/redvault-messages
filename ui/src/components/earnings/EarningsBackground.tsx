// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useEffect, useRef, useState } from "react";
import { deliverPendingKeys, PaymentNotCompleted, waitForPurchase } from "../../lib/earnings.js";
import * as eapi from "../../lib/earningsApi.js";
import { useAppState } from "../../state/AppStateContext.js";
import styles from "./Earnings.module.css";

const PENDING_PURCHASE = "redvaultsystem:pending-purchase";
const KEY_DELIVERY_MS = 60_000;

/**
 * Back from Stripe's payment page (…?purchase=<id>&result=…): the id leaves
 * the address bar at once (on any screen, signed in or not) and is kept for
 * this tab until EarningsBackground can ask the server how it went.
 */
export function usePurchaseReturnLink() {
  useEffect(() => {
    const params = new URLSearchParams(window.location.search);
    const purchaseId = params.get("purchase");
    if (!purchaseId || !/^[0-9a-f-]{36}$/i.test(purchaseId)) return;
    const result = params.get("result");
    try {
      sessionStorage.setItem(PENDING_PURCHASE, JSON.stringify({ purchaseId, result }));
    } catch {
      // Without session storage, the purchase still shows up in the app once paid.
    }
    params.delete("purchase");
    params.delete("result");
    const rest = params.toString();
    window.history.replaceState(null, "", `${window.location.pathname}${rest ? `?${rest}` : ""}`);
  }, []);
}

/**
 * Two things that run while signed in:
 *
 * - Back from Stripe's payment page (usePurchaseReturnLink kept the id): the
 *   result in the address bar proves nothing, so this asks the server, which
 *   only marks a purchase paid after the provider's verified webhook.
 * - A creator's app seals exclusive-post keys to buyers whose payment was
 *   confirmed (only this app can: the server never holds a post's key).
 */
export function EarningsBackground() {
  const { state } = useAppState();
  const identity = state.authStatus === "signed-in" ? state.identity : null;
  const [notice, setNotice] = useState<string | null>(null);

  useEffect(() => {
    if (!identity) return;
    let pending: { purchaseId: string; result: string | null } | null = null;
    try {
      const raw = sessionStorage.getItem(PENDING_PURCHASE);
      sessionStorage.removeItem(PENDING_PURCHASE);
      pending = raw ? (JSON.parse(raw) as { purchaseId: string; result: string | null }) : null;
    } catch {
      pending = null;
    }
    if (!pending) return;
    if (pending.result === "cancel") {
      setNotice("Payment canceled. You weren't charged.");
      return;
    }
    const controller = new AbortController();
    setNotice("Confirming your payment with the payment provider…");
    waitForPurchase(pending.purchaseId, { signal: controller.signal })
      .then((p) =>
        setNotice(
          p.product === "tip"
            ? "Your tip went through. Thank you!"
            : p.product === "exclusive_post"
              ? "Paid. The post unlocks when the creator's app sends your key."
              : "Paid. You can send your message now."
        )
      )
      .catch((err) => {
        if (err instanceof DOMException) return;
        setNotice(err instanceof PaymentNotCompleted ? err.message : "Couldn't confirm the payment yet. It will show up in the app once it's through.");
      });
    return () => controller.abort();
  }, [identity]);

  // The creator's key delivery, every minute while the app is open.
  const conversations = state.conversations;
  const knownKeys = useRef(new Map<string, Uint8Array>());
  useEffect(() => {
    const known = new Map<string, Uint8Array>();
    for (const c of conversations) if (c.kind === "dm" && c.userId && c.publicKey) known.set(c.userId, c.publicKey);
    knownKeys.current = known;
  }, [conversations]);
  useEffect(() => {
    if (!identity) return;
    let stopped = false;
    let creator: boolean | null = null;
    const run = async () => {
      if (stopped || document.hidden) return;
      try {
        if (creator === null) {
          const config = await eapi.getEarningsConfig();
          creator = config.enabled && (await eapi.getEarningSettings()).creatorReady;
        }
        if (creator) await deliverPendingKeys(identity, knownKeys.current);
      } catch {
        // Offline or not a creator: tried again next time.
      }
    };
    const first = setTimeout(run, 5000);
    const interval = setInterval(run, KEY_DELIVERY_MS);
    return () => {
      stopped = true;
      clearTimeout(first);
      clearInterval(interval);
    };
  }, [identity]);

  if (!notice || !identity) return null;
  return (
    <div className={styles.toast} role="status">
      <span>{notice}</span>
      <button type="button" className={styles.link} onClick={() => setNotice(null)}>
        Close
      </button>
    </div>
  );
}
