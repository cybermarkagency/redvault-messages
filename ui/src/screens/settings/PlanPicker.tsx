import { useEffect, useState } from "react";
import {
  ApiError,
  cancelPlan,
  getBillingConfig,
  getPlanStatus,
  paymentLinkFor,
  resumePlan,
  switchPlan,
  type PlanStatus,
} from "../../lib/api.js";
import { type FilesStorageLimits, type PlanChoice, PRICE_NOTE, planChoices } from "../../lib/planChoices.js";
import { useAppState } from "../../state/AppStateContext.js";
import styles from "./PlanPicker.module.css";

interface BillingLinks {
  paymentLink: string | null;
  premiumPaymentLink: string | null;
  filesStorage: FilesStorageLimits | null;
}

/**
 * Settings > Account: Free, Pro and Premium side by side, the account's own
 * plan marked, and the one button on each that changes to it (see
 * lib/planChoices.ts for which button each plan gets, and when).
 */
export function PlanPicker() {
  const { state, dispatch } = useAppState();
  const identity = state.identity;
  const tier = identity?.subscriptionTier ?? "free";
  const [links, setLinks] = useState<BillingLinks>({ paymentLink: null, premiumPaymentLink: null, filesStorage: null });
  const [status, setStatus] = useState<PlanStatus | null>(null);
  /** The plan whose button was pressed and is waiting for "yes". */
  const [confirming, setConfirming] = useState<PlanChoice["tier"] | null>(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    Promise.resolve()
      .then(() => getBillingConfig())
      .then((config) => {
        if (cancelled) return;
        setLinks({
          paymentLink: config?.paymentLink ?? null,
          premiumPaymentLink: config?.premiumPaymentLink ?? null,
          filesStorage: config?.filesStorage ?? null,
        });
      })
      .catch(() => {
        /* billing not configured: no checkout buttons */
      });
    Promise.resolve()
      .then(() => getPlanStatus())
      .then((planStatus) => {
        if (!cancelled) setStatus(planStatus ?? null);
      })
      .catch(() => {
        /* older server or a transient error: the cards show the plan without its buttons */
      });
    return () => {
      cancelled = true;
    };
  }, [tier]);

  if (!identity) return null;

  const choices = planChoices({
    tier,
    status,
    proCheckout: !!links.paymentLink,
    premiumCheckout: !!links.premiumPaymentLink,
    filesStorage: links.filesStorage,
  });

  async function run(choice: PlanChoice) {
    const change: Record<PlanChoice["action"], (() => Promise<PlanStatus>) | null> = {
      switch: () => switchPlan(choice.tier as "pro" | "premium"),
      cancel: cancelPlan,
      keep: resumePlan,
      checkout: null,
      none: null,
    };
    const action = change[choice.action];
    if (!action) return;
    setBusy(true);
    setError(null);
    try {
      const next = await action();
      setStatus(next);
      setConfirming(null);
      if (next.tier !== identity?.subscriptionTier) dispatch({ type: "SUBSCRIPTION_TIER_SET", tier: next.tier });
    } catch (err) {
      setError(err instanceof ApiError ? err.message : "Couldn't change your plan. Try again.");
    } finally {
      setBusy(false);
    }
  }

  function actionButton(choice: PlanChoice) {
    if (choice.action === "checkout") {
      const link = choice.tier === "premium" ? links.premiumPaymentLink : links.paymentLink;
      return (
        <a className={styles.primary} href={paymentLinkFor(link!, identity!.userId)} target="_blank" rel="noopener noreferrer">
          {choice.actionLabel}
        </a>
      );
    }
    if (choice.action === "keep") {
      return (
        <>
          <span className={styles.confirm}>Your subscription won't renew. You won't be charged again.</span>
          <button type="button" className={styles.primary} disabled={busy} onClick={() => run(choice)}>
            {busy ? "Switching…" : choice.actionLabel}
          </button>
        </>
      );
    }
    const style = choice.action === "cancel" ? styles.danger : choice.tier === "premium" ? styles.primary : styles.secondary;
    if (confirming === choice.tier) {
      return (
        <>
          <span className={styles.confirm}>{choice.confirm}</span>
          <button type="button" className={style} disabled={busy} onClick={() => run(choice)}>
            {busy ? "Switching…" : choice.actionLabel}
          </button>
          <button type="button" className={styles.secondary} disabled={busy} onClick={() => setConfirming(null)}>
            {choice.action === "cancel" ? `Keep ${choices.find((c) => c.current)!.name}` : "Cancel"}
          </button>
        </>
      );
    }
    return (
      <button
        type="button"
        className={style}
        disabled={busy}
        onClick={() => {
          setError(null);
          setConfirming(choice.tier);
        }}
      >
        {choice.actionLabel}…
      </button>
    );
  }

  const offersCheckout = choices.some((c) => c.action === "checkout");
  return (
    <div className={styles.picker}>
      <div className={styles.grid}>
        {choices.map((choice) => (
          <section
            key={choice.tier}
            aria-label={choice.current ? `${choice.name} (your plan)` : choice.name}
            className={`${styles.card} ${choice.current ? styles.cardCurrent : ""}`}
          >
            <div className={styles.header}>
              <h3 className={styles.name}>{choice.name}</h3>
              {choice.current && <span className={styles.badge}>Current</span>}
            </div>
            <div className={styles.price}>
              <span className={styles.amount}>{choice.price}</span>
              {choice.per && <span className={styles.per}>{choice.per}</span>}
            </div>
            {choice.status && <div className={choice.current ? styles.statusCurrent : styles.status}>{choice.status}</div>}
            <ul className={styles.includes}>
              {choice.includes.map((line) => (
                <li key={line}>{line}</li>
              ))}
            </ul>
            {choice.action !== "none" && <div className={styles.actions}>{actionButton(choice)}</div>}
            {choice.note && <p className={styles.note}>{choice.note}</p>}
          </section>
        ))}
      </div>
      {error && (
        <p className={styles.error} role="alert">
          {error}
        </p>
      )}
      <p className={styles.footer}>
        {PRICE_NOTE}
        {offersCheckout && " Payment is handled by Stripe. Your plan changes here within a minute of paying."}
      </p>
    </div>
  );
}
