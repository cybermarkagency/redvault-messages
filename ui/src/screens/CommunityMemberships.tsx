// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useMemo, useState } from "react";
import { ApiError } from "../lib/api.js";
import * as communityApi from "../lib/communityApi.js";
import { type CommunitiesConfigDto, type CommunityPageDto, formatMoney, type MembershipViewDto } from "../lib/communityApi.js";
import { describeError, formatDate } from "../lib/communityFormat.js";
import styles from "./Communities.module.css";

// Paid community memberships on the website: buying one from a community's
// link (Stripe's hosted checkout), and the memberships you pay for: cancel,
// keep, change the card, receipts. Reading and joining communities happens
// in the apps; the Android app can't sell memberships, so they're bought here.

const MEMBERSHIP_TEXT: Record<string, string> = {
  pending_enrollment: "You're in. Open the community in the app: it gets the community's encryption key from the creator's or another member's app.",
  pending_approval: "Your request to join was sent. A moderator will review it.",
  banned: "You can't join this community.",
};

/** One community, from its link: what it costs, and Subscribe. Everything else about it is in the app. */
export function CommunityPurchase({ communityId, inviteToken, onMemberships }: { communityId: string; inviteToken?: string; onMemberships: () => void }) {
  const [config, setConfig] = useState<CommunitiesConfigDto | null>(null);
  const [community, setCommunity] = useState<CommunityPageDto | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [termsAccepted, setTermsAccepted] = useState(false);
  const [ageConfirmed, setAgeConfirmed] = useState(false);
  const [message, setMessage] = useState<string | null>(null);

  useEffect(() => {
    communityApi
      .getCommunitiesConfig()
      .then(setConfig)
      .catch(() => setConfig(null));
  }, []);

  useEffect(() => {
    communityApi
      .getCommunity(communityId, inviteToken)
      .then((c) => {
        setCommunity(c);
        setError(null);
      })
      .catch((err) =>
        setError(err instanceof ApiError && err.status === 404 ? "This community doesn't exist, or it opens only from its invite link." : describeError(err))
      );
  }, [communityId, inviteToken]);

  if (error) return <p className={styles.error}>{error}</p>;
  if (!community) return <p className={styles.muted}>Loading…</p>;

  const membership = community.viewer.membership;
  const subscription = community.viewer.subscription;
  const isActiveMember = membership?.status === "active";
  const price = community.price;
  const canBuyHere = !!config?.paymentsEnabled && (config?.purchasePlatforms ?? []).includes("web");
  const closedForJoining = community.status !== "published" || community.newMembersPaused;

  async function subscribe() {
    setBusy(true);
    setMessage(null);
    try {
      const { checkoutUrl } = await communityApi.startCommunityCheckout(communityId, { platform: "web", termsAccepted, ageConfirmed, inviteToken });
      // Stripe's hosted checkout. Access starts only when Stripe confirms the payment to the server.
      window.location.assign(checkoutUrl);
    } catch (err) {
      setMessage(describeError(err));
      setBusy(false);
    }
  }

  return (
    <>
      <div className={styles.hero}>
        <div className={styles.rowMain}>
          <h1 className={styles.heroTitle}>{community.name}</h1>
          <span className={styles.muted}>
            by {community.creator.displayName} (@{community.creator.handle}){community.creator.verified ? " · verified creator" : ""}
          </span>
        </div>
      </div>
      <div className={styles.chips}>
        <span className={styles.chip}>{community.kind === "channel" ? "Channel: the creator posts, members read" : "Group: members talk together"}</span>
        {community.memberCount !== null && <span className={styles.chip}>{community.memberCount} members</span>}
        {community.status !== "published" && <span className={styles.chipWarn}>{community.status}</span>}
        <span className={styles.chip}>Code {community.code}</span>
      </div>
      {community.description && <p className={styles.text}>{community.description}</p>}

      {isActiveMember && (
        <div className={styles.panelAccent}>
          <strong>You're a member. Open it in the RedVault Messages app.</strong>
          {subscription && (
            <span className={styles.muted}>
              {subscription.entitlementStatus === "cancel_scheduled"
                ? `Your membership ends on ${formatDate(subscription.accessUntil)}.`
                : subscription.renewsOn
                  ? `Renews on ${formatDate(subscription.renewsOn)}.`
                  : null}
            </span>
          )}
          {subscription && (
            <div className={styles.actions}>
              <button type="button" className={styles.secondaryButton} onClick={onMemberships}>
                Manage membership
              </button>
            </div>
          )}
        </div>
      )}

      {membership && MEMBERSHIP_TEXT[membership.status] && <p className={styles.notice}>{MEMBERSHIP_TEXT[membership.status]}</p>}
      {subscription?.entitlementStatus === "pending" && (
        <p className={styles.notice}>Your payment is being confirmed with Stripe. Your community memberships show it once it's done.</p>
      )}

      {!isActiveMember && membership?.status !== "banned" && !(membership && membership.status.startsWith("pending")) && (
        <div className={styles.panel}>
          {community.accessType === "free" ? (
            <span className={styles.muted}>This community is free to join: open its link in the RedVault Messages app.</span>
          ) : subscription && (subscription.entitlementStatus === "cancel_scheduled" || subscription.entitlementStatus === "active") ? (
            <span className={styles.muted}>You still have a paid membership until {formatDate(subscription.accessUntil)}. Rejoin from the app.</span>
          ) : price ? (
            <>
              <span className={styles.price}>
                {formatMoney(price.amountMinor, price.currency)} <span className={styles.muted}>/ month</span>
              </span>
              <span className={styles.muted}>
                Billed monthly by RedVault through Stripe. Your card statement shows RedVault; this community's code is {community.code}.
              </span>
              <label className={styles.check}>
                <input type="checkbox" checked={termsAccepted} onChange={(e) => setTermsAccepted(e.target.checked)} />
                <span>
                  I agree to the community membership terms. This membership <strong>renews automatically every month</strong> at{" "}
                  {formatMoney(price.amountMinor, price.currency)} until I cancel. I can cancel any time under Community memberships on this website
                  and keep access until the end of the period I paid for. Refunds follow the terms.
                </span>
              </label>
              <label className={styles.check}>
                <input type="checkbox" checked={ageConfirmed} onChange={(e) => setAgeConfirmed(e.target.checked)} />
                <span>I'm 18 or older.</span>
              </label>
              {!canBuyHere && <p className={styles.noticeWarn}>Memberships can't be bought here right now.</p>}
              <div className={styles.actions}>
                <button
                  type="button"
                  className={styles.primaryButton}
                  disabled={busy || !termsAccepted || !ageConfirmed || !canBuyHere || closedForJoining}
                  onClick={subscribe}
                >
                  {busy ? "Opening checkout…" : "Subscribe"}
                </button>
              </div>
              {closedForJoining && <span className={styles.muted}>Not accepting new members right now.</span>}
            </>
          ) : (
            <span className={styles.muted}>This community has no price yet.</span>
          )}
        </div>
      )}

      {message && <p className={styles.notice}>{message}</p>}

      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>PRIVACY</span>
        <p className={styles.muted}>
          Messages, images, voice notes and files inside are end-to-end encrypted: neither RedVault nor Stripe can read them. RedVault does see
          this public listing, who is a member, and the state of each membership's billing.
        </p>
      </div>
    </>
  );
}

// --- Memberships (subscriptions) ---------------------------------------------------------

const ENTITLEMENT_TEXT: Record<string, string> = {
  pending: "Processing payment",
  active: "Active",
  trialing: "Trial",
  grace_period: "Payment failed",
  cancel_scheduled: "Cancelled — access until period end",
  expired: "Ended",
  canceled: "Not completed",
  refunded: "Refunded",
  disputed: "Payment under review",
  suspended: "Suspended",
};

export function Memberships({ checkoutId, canceled = false }: { checkoutId?: string; canceled?: boolean }) {
  const [list, setList] = useState<MembershipViewDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busyId, setBusyId] = useState<string | null>(null);
  const [confirming, setConfirming] = useState<string | null>(canceled ? null : (checkoutId ?? null));
  const [receipts, setReceipts] = useState<Record<string, Awaited<ReturnType<typeof communityApi.membershipReceipts>>>>({});

  const load = useCallback(() => {
    communityApi
      .listMyMemberships()
      .then((l) => {
        setList(l);
        setError(null);
      })
      .catch((err) => setError(describeError(err)));
  }, []);
  useEffect(load, [load]);

  // Back from Stripe: ask the server (which asks Stripe) until the payment is confirmed.
  useEffect(() => {
    if (!confirming) return;
    let tries = 0;
    let stopped = false;
    const tick = async () => {
      tries++;
      try {
        const view = await communityApi.refreshMembership(confirming);
        if (view.entitlementStatus !== "pending" || tries >= 20) {
          setConfirming(null);
          load();
          return;
        }
      } catch {
        if (tries >= 20) setConfirming(null);
      }
      if (!stopped) setTimeout(tick, 3000);
    };
    void tick();
    return () => {
      stopped = true;
    };
  }, [confirming, load]);

  async function act(subscriptionId: string, fn: () => Promise<unknown>) {
    setBusyId(subscriptionId);
    setError(null);
    try {
      await fn();
      load();
    } catch (err) {
      setError(describeError(err));
    } finally {
      setBusyId(null);
    }
  }

  async function portal(subscriptionId: string) {
    setBusyId(subscriptionId);
    try {
      const { url } = await communityApi.membershipPortal(subscriptionId);
      window.location.assign(url);
    } catch (err) {
      setError(describeError(err));
      setBusyId(null);
    }
  }

  const sorted = useMemo(() => [...(list ?? [])].sort((a, b) => b.createdAt.localeCompare(a.createdAt)), [list]);

  return (
    <>
      {canceled && <p className={styles.notice}>Checkout canceled. You weren't charged.</p>}
      {confirming && <p className={styles.notice}>Confirming your payment with Stripe… Access starts as soon as Stripe confirms it, not before.</p>}
      {error && <p className={styles.error}>{error}</p>}
      {list === null && !error && <p className={styles.muted}>Loading…</p>}
      {list?.length === 0 && <div className={styles.empty}>No paid memberships. Communities are found and joined in the app; a paid one can be bought here from its link.</div>}
      {sorted.map((m) => {
        const chipClass =
          m.entitlementStatus === "active" ? styles.chipAccent : m.entitlementStatus === "grace_period" || m.entitlementStatus === "pending" || m.entitlementStatus === "cancel_scheduled" ? styles.chipWarn : m.entitlementStatus === "refunded" || m.entitlementStatus === "disputed" || m.entitlementStatus === "suspended" ? styles.chipDanger : styles.chip;
        return (
          <div key={m.id} className={styles.panel}>
            <div className={styles.cardHead}>
              <span className={styles.rowMain}>
                <span className={styles.cardTitle}>{m.community.name}</span>
                <span className={styles.muted}>
                  @{m.creatorHandle ?? "creator"} · code {m.community.code}
                </span>
              </span>
            </div>
            <div className={styles.chips}>
              <span className={chipClass}>{ENTITLEMENT_TEXT[m.entitlementStatus] ?? m.entitlementStatus}</span>
              {m.price && <span className={styles.chip}>{formatMoney(m.price.amountMinor, m.price.currency)}/month</span>}
            </div>
            <span className={styles.muted}>
              {m.renewsOn && `Renews on ${formatDate(m.renewsOn)}. `}
              {m.entitlementStatus === "cancel_scheduled" && `Access until ${formatDate(m.accessUntil)}; you won't be charged again. `}
              {m.entitlementStatus === "grace_period" && `Update your card by ${formatDate(m.graceUntil)} to keep access. `}
              {m.entitlementStatus === "expired" && m.endedAt && `Ended on ${formatDate(m.endedAt)}. `}
            </span>
            <div className={styles.actions}>
              {(m.entitlementStatus === "active" || m.entitlementStatus === "grace_period") && !m.cancelAtPeriodEnd && (
                <button
                  type="button"
                  className={styles.smallDanger}
                  disabled={busyId === m.id}
                  onClick={() => {
                    if (window.confirm(`Cancel your ${m.community.name} membership? You won't be charged again and keep access until ${formatDate(m.accessUntil)}.`)) {
                      void act(m.id, () => communityApi.cancelMembership(m.id));
                    }
                  }}
                >
                  Cancel membership
                </button>
              )}
              {m.entitlementStatus === "cancel_scheduled" && m.billingStatus !== "canceled" && (
                <button type="button" className={styles.small} disabled={busyId === m.id} onClick={() => act(m.id, () => communityApi.resumeMembership(m.id))}>
                  Keep membership
                </button>
              )}
              {m.billingStatus !== "checkout_pending" && m.billingStatus !== "checkout_expired" && (
                <button type="button" className={styles.small} disabled={busyId === m.id} onClick={() => portal(m.id)}>
                  Manage payment method
                </button>
              )}
              <button
                type="button"
                className={styles.small}
                onClick={async () => {
                  if (receipts[m.id]) setReceipts(({ [m.id]: _, ...rest }) => rest);
                  else setReceipts({ ...receipts, [m.id]: await communityApi.membershipReceipts(m.id).catch(() => []) });
                }}
              >
                {receipts[m.id] ? "Hide receipts" : "Receipts"}
              </button>
            </div>
            {receipts[m.id] && (
              <div className={styles.tableWrap}>
                <table className={styles.table}>
                  <thead>
                    <tr>
                      <th>Date</th>
                      <th>What</th>
                      <th>Period</th>
                      <th className={styles.num}>Amount</th>
                    </tr>
                  </thead>
                  <tbody>
                    {receipts[m.id].length === 0 && (
                      <tr>
                        <td colSpan={4}>No payments yet.</td>
                      </tr>
                    )}
                    {receipts[m.id].map((r) => (
                      <tr key={r.id}>
                        <td>{formatDate(r.occurredAt)}</td>
                        <td>{r.kind === "payment" ? "Payment" : r.kind === "refund" ? "Refund" : r.kind}</td>
                        <td>{r.periodStart ? `${formatDate(r.periodStart)} – ${formatDate(r.periodEnd)}` : "—"}</td>
                        <td className={styles.num}>{(r.kind === "refund" ? "−" : "") + formatMoney(r.amountMinor, r.currency)}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
          </div>
        );
      })}
    </>
  );
}
