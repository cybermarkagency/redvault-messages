// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useRef, useState } from "react";
import { PaymentNotCompleted, startCheckout, waitForPurchase } from "../../lib/earnings.js";
import { formatUsd } from "../../lib/earningsApi.js";
import { useDialogFocus } from "../useDialogFocus.js";
import styles from "./Earnings.module.css";

const PRESETS = [100, 300, 500, 1000];

/** A voluntary tip: it doesn't unlock anything. */
export function TipDialog({ creatorUserId, name, onClose }: { creatorUserId: string; name: string; onClose: () => void }) {
  const [amount, setAmount] = useState<number | "custom">(300);
  const [custom, setCustom] = useState("");
  const [showName, setShowName] = useState(true);
  const [status, setStatus] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [done, setDone] = useState(false);
  const root = useRef<HTMLDivElement>(null);
  useDialogFocus(root, onClose);

  const cents = amount === "custom" ? Math.round(Number(custom) * 100) : amount;

  async function tip() {
    if (!Number.isFinite(cents) || cents < 100 || cents > 50000) {
      setError("Tips are between $1.00 and $500.00.");
      return;
    }
    setError(null);
    setStatus("Opening the payment page…");
    try {
      const started = await startCheckout({ product: "tip", creatorUserId, amountMinor: cents, showName });
      setStatus("Waiting for the payment provider to confirm…");
      await waitForPurchase(started.purchaseId);
      setStatus(null);
      setDone(true);
    } catch (err) {
      setStatus(null);
      setError(err instanceof PaymentNotCompleted || err instanceof Error ? err.message : "The tip didn't go through.");
    }
  }

  return (
    <div className={styles.backdrop}>
      <div className={styles.dialog} role="dialog" aria-modal="true" aria-label={`Tip ${name}`} ref={root}>
        <h2 className={styles.dialogTitle}>Tip {name}</h2>
        {done ? (
          <>
            <p>Thank you! Your tip went through.</p>
            <div className={styles.actions}>
              <button type="button" className={styles.primary} onClick={onClose}>
                Done
              </button>
            </div>
          </>
        ) : (
          <>
            <p className={styles.muted}>Tips are voluntary and don't unlock anything. You pay on Stripe's page; RedVault never sees your card.</p>
            <div className={styles.presets} role="radiogroup" aria-label="Amount">
              {PRESETS.map((p) => (
                <button key={p} type="button" role="radio" aria-checked={amount === p} className={amount === p ? styles.presetOn : styles.preset} onClick={() => setAmount(p)}>
                  {formatUsd(p)}
                </button>
              ))}
              <button type="button" role="radio" aria-checked={amount === "custom"} className={amount === "custom" ? styles.presetOn : styles.preset} onClick={() => setAmount("custom")}>
                Other
              </button>
            </div>
            {amount === "custom" && (
              <label className={styles.field}>
                Amount (USD)
                <input className={styles.input} inputMode="decimal" placeholder="7.50" value={custom} onChange={(e) => setCustom(e.target.value)} aria-label="Tip amount in US dollars" />
              </label>
            )}
            <label className={styles.check}>
              <input type="checkbox" checked={showName} onChange={(e) => setShowName(e.target.checked)} />
              Show {name} who it's from
            </label>
            <p className={styles.muted}>
              {showName
                ? "They'll see your username with the tip."
                : "They'll see “a supporter”. RedVault and Stripe still know who paid (for refunds and fraud checks), so this isn't anonymous."}
            </p>
            {status && <p className={styles.muted}>{status}</p>}
            {error && <p className={styles.error}>{error}</p>}
            <div className={styles.actions}>
              <button type="button" className={styles.secondary} onClick={onClose}>
                Cancel
              </button>
              <button type="button" className={styles.primary} disabled={status !== null} onClick={tip}>
                Tip {Number.isFinite(cents) && cents > 0 ? formatUsd(cents) : ""}
              </button>
            </div>
          </>
        )}
      </div>
    </div>
  );
}
