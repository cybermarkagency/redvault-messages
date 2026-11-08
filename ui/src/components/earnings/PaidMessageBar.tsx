// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useState } from "react";
import type { PaidSend } from "../../lib/api.js";
import { PaymentNotCompleted, startCheckout, waitForPurchase } from "../../lib/earnings.js";
import * as eapi from "../../lib/earningsApi.js";
import { formatUsd, type QuoteDto } from "../../lib/earningsApi.js";
import styles from "./Earnings.module.css";

/**
 * Paid messages in a 1:1 chat: what sending costs right now, and the paid
 * message (or free safety contact) the next send uses. Null quote: free.
 */
export function usePaidMessaging(userId: string | null | undefined, conversationId: string) {
  const [quote, setQuote] = useState<QuoteDto | null>(null);
  const [safety, setSafety] = useState(false);
  const refresh = useCallback(() => {
    if (!userId) return;
    eapi
      .getQuote(userId, conversationId)
      .then((q) => setQuote(q.required ? q : null))
      .catch(() => setQuote(null));
  }, [userId, conversationId]);
  useEffect(() => {
    setQuote(null);
    setSafety(false);
    refresh();
  }, [refresh]);

  /** What the next send needs, or null when it can't go out yet (pay first). */
  const sendOptions = (): PaidSend | null => {
    if (!quote) return {};
    if (safety) return { safetyContact: true };
    if (quote.availablePurchaseId) return { entitlementId: quote.availablePurchaseId };
    return null;
  };

  const afterSend = () => {
    setSafety(false);
    refresh();
  };

  return { quote, refresh, safety, setSafety, sendOptions, afterSend };
}

export function PaidMessageBar({ name, creatorUserId, paid }: { name: string; creatorUserId: string; paid: ReturnType<typeof usePaidMessaging> }) {
  const { quote } = paid;
  const [status, setStatus] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  if (!quote) return null;
  const price = formatUsd(quote.priceMinor);

  async function pay() {
    setError(null);
    setStatus("Opening the payment page…");
    try {
      const started = await startCheckout({ product: "paid_message", creatorUserId });
      if (started.status !== "checkout_pending") {
        setStatus(null);
        paid.refresh();
        return;
      }
      setStatus("Waiting for the payment provider to confirm…");
      await waitForPurchase(started.purchaseId);
      setStatus(null);
      paid.refresh();
    } catch (err) {
      setStatus(null);
      setError(err instanceof PaymentNotCompleted || err instanceof Error ? err.message : "The payment didn't go through.");
    }
  }

  return (
    <div className={styles.bar} role="region" aria-label="Paid messages">
      {quote.availablePurchaseId ? (
        <span>Paid. Your next message to {name} goes through.</span>
      ) : paid.safety ? (
        <span>
          Safety contact: for something urgent about {name}'s safety or account. Free once every 30 days; misuse can be reported.{" "}
          <button type="button" className={styles.link} onClick={() => paid.setSafety(false)}>
            Cancel
          </button>
        </span>
      ) : (
        <>
          <span>
            {quote.mode === "first_contact" ? `${name} charges ${price} to start a chat (then it's free).` : `${name} charges ${price} for each message.`} Messages stay end-to-end
            encrypted.
          </span>
          <span className={styles.barActions}>
            {quote.purchasable ? (
              <button type="button" className={styles.primary} onClick={pay} disabled={status !== null}>
                Pay {price}
              </button>
            ) : (
              <span className={styles.muted}>Payments aren't available right now.</span>
            )}
            {quote.safetyContactAvailable && (
              <button type="button" className={styles.link} onClick={() => paid.setSafety(true)}>
                Safety contact (free)
              </button>
            )}
          </span>
        </>
      )}
      {status && <span className={styles.muted}>{status}</span>}
      {error && <span className={styles.error}>{error}</span>}
    </div>
  );
}
