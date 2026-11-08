// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useMemo, useState } from "react";
import { TopBar } from "../components/TopBar.js";
import { ApiError } from "../lib/api.js";
import * as communityApi from "../lib/communityApi.js";
import { type CommunitiesConfigDto, type CommunityListingDto, type CommunityPageDto, formatMoney, type MembershipViewDto, type MyCommunityDto } from "../lib/communityApi.js";
import { loadConversations } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import type { CommunitiesPage } from "../state/reducer.js";
import { CommunityCreate } from "./CommunityCreate.js";
import { CommunityManage } from "./CommunityManage.js";
import { CommunityReportDialog } from "./CommunityReportDialog.js";
import { CreatorDashboard } from "./CreatorDashboard.js";
import { Library } from "./Library.js";
import styles from "./Communities.module.css";

export const CATEGORY_LABELS: Record<string, string> = {
  art_design: "Art & design",
  business: "Business",
  education: "Education",
  entertainment: "Entertainment",
  gaming: "Gaming",
  health_fitness: "Health & fitness",
  lifestyle: "Lifestyle",
  music: "Music",
  news: "News",
  science_technology: "Science & technology",
  sports: "Sports",
  writing: "Writing",
  other: "Other",
};

export function describeError(err: unknown, fallback = "Something went wrong. Try again."): string {
  return err instanceof ApiError ? err.message || fallback : fallback;
}

export function formatDate(iso: string | null): string {
  return iso ? new Date(iso).toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" }) : "—";
}

function CommunityThumb({ community, invite }: { community: { id: string; name: string; hasAvatar: boolean }; invite?: string }) {
  return community.hasAvatar ? (
    <img className={styles.thumb} src={communityApi.communityImageUrl(community.id, "avatar", invite)} alt="" />
  ) : (
    <span className={styles.thumbLetter} aria-hidden="true">
      {community.name.trim().charAt(0).toUpperCase() || "#"}
    </span>
  );
}

const TABS: { page: CommunitiesPage; label: string }[] = [
  { page: "discover", label: "Discover" },
  { page: "mine", label: "My communities" },
  { page: "memberships", label: "Memberships" },
  { page: "create", label: "Create" },
  { page: "library", label: "Library" },
  { page: "creator", label: "Creator dashboard" },
];

export function Communities() {
  const { state, dispatch } = useAppState();
  const screen = state.screen.key === "communities" ? state.screen : { key: "communities" as const };
  const page: CommunitiesPage = screen.page ?? "discover";
  const [config, setConfig] = useState<CommunitiesConfigDto | null>(null);
  const [configError, setConfigError] = useState<string | null>(null);

  useEffect(() => {
    communityApi
      .getCommunitiesConfig()
      .then(setConfig)
      .catch((err) => setConfigError(describeError(err, "Communities aren't reachable right now.")));
  }, []);

  const open = useCallback(
    (next: CommunitiesPage, extra: { communityId?: string; inviteToken?: string } = {}) => dispatch({ type: "OPEN_COMMUNITIES", page: next, ...extra }),
    [dispatch]
  );

  const tabPage = page === "community" || page === "manage" ? null : page;

  return (
    <div className={styles.screen}>
      <TopBar title="Communities" />
      <nav className={styles.tabs} aria-label="Communities">
        {TABS.map((t) => (
          <button
            key={t.page}
            type="button"
            className={`${styles.tab} ${tabPage === t.page ? styles.tabActive : ""}`}
            aria-current={tabPage === t.page ? "page" : undefined}
            onClick={() => open(t.page)}
          >
            {t.label}
          </button>
        ))}
      </nav>
      <div className={styles.body}>
        {configError && <p className={styles.error}>{configError}</p>}
        {config && !config.enabled && (
          <p className={styles.notice}>Communities aren't open on this server yet. Memberships you already have keep working.</p>
        )}
        {page === "discover" && <Discover config={config} onOpen={(id) => open("community", { communityId: id })} />}
        {page === "community" && screen.communityId && (
          <CommunityPage key={screen.communityId} communityId={screen.communityId} inviteToken={screen.inviteToken} config={config} onManage={() => open("manage", { communityId: screen.communityId })} />
        )}
        {page === "manage" && screen.communityId && <CommunityManage communityId={screen.communityId} config={config} onBack={() => open("community", { communityId: screen.communityId })} />}
        {page === "mine" && <MyCommunities onOpen={(id) => open("community", { communityId: id })} />}
        {page === "memberships" && <Memberships checkoutId={screen.checkoutId} onOpen={(id) => open("community", { communityId: id })} />}
        {page === "create" && <CommunityCreate config={config} onCreated={(id) => open("manage", { communityId: id })} onNeedCreator={() => open("creator")} />}
        {page === "library" && <Library />}
        {page === "creator" && <CreatorDashboard config={config} onOpenCommunity={(id) => open("manage", { communityId: id })} onCreate={() => open("create")} />}
      </div>
    </div>
  );
}

// --- Discover ---------------------------------------------------------------------

function Discover({ config, onOpen }: { config: CommunitiesConfigDto | null; onOpen: (id: string) => void }) {
  const [q, setQ] = useState("");
  const [category, setCategory] = useState("");
  const [results, setResults] = useState<CommunityListingDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (config && !config.enabled) return;
    const handle = setTimeout(() => {
      communityApi
        .discoverCommunities({ q: q.trim() || undefined, category: category || undefined })
        .then((list) => {
          setResults(list);
          setError(null);
        })
        .catch((err) => setError(describeError(err)));
    }, 250);
    return () => clearTimeout(handle);
  }, [q, category, config]);

  return (
    <>
      <div className={styles.toolbar}>
        <input className={styles.input} placeholder="Search communities" aria-label="Search communities" value={q} onChange={(e) => setQ(e.target.value)} maxLength={80} />
        <select className={styles.select} aria-label="Category" value={category} onChange={(e) => setCategory(e.target.value)}>
          <option value="">All categories</option>
          {(config?.categories ?? Object.keys(CATEGORY_LABELS)).map((c) => (
            <option key={c} value={c}>
              {CATEGORY_LABELS[c] ?? c}
            </option>
          ))}
        </select>
      </div>
      {error && <p className={styles.error}>{error}</p>}
      {results === null && !error && <p className={styles.muted}>Loading…</p>}
      {results?.length === 0 && <div className={styles.empty}>No communities match. Unlisted communities only open from an invite link.</div>}
      <div className={styles.grid}>
        {results?.map((c) => (
          <button key={c.id} type="button" className={styles.card} onClick={() => onOpen(c.id)}>
            <span className={styles.cardHead}>
              <CommunityThumb community={c} />
              <span className={styles.rowMain}>
                <span className={styles.cardTitle}>{c.name}</span>
                <span className={styles.muted}>
                  @{c.creator.handle}
                  {c.creator.verified ? " · verified" : ""}
                </span>
              </span>
            </span>
            <span className={styles.clamp}>{c.description || c.previewText}</span>
            <span className={styles.chips}>
              <span className={styles.chip}>{c.kind === "channel" ? "Channel" : "Group"}</span>
              {c.price ? <span className={styles.chipAccent}>{formatMoney(c.price.amountMinor, c.price.currency)}/month</span> : <span className={styles.chip}>Free</span>}
              {c.memberCount !== null && <span className={styles.chip}>{c.memberCount} members</span>}
              <span className={styles.chip}>{CATEGORY_LABELS[c.category] ?? c.category}</span>
            </span>
          </button>
        ))}
      </div>
    </>
  );
}

// --- One community ------------------------------------------------------------------

const MEMBERSHIP_TEXT: Record<string, string> = {
  pending_enrollment:
    "You're in. Your app is waiting for the community's encryption key, which the creator's or another member's app delivers automatically the next time one of them is online. Nothing to do on your side.",
  pending_approval: "Your request to join was sent. A moderator will review it.",
  banned: "You can't join this community.",
};

function CommunityPage({ communityId, inviteToken, config, onManage }: { communityId: string; inviteToken?: string; config: CommunitiesConfigDto | null; onManage: () => void }) {
  const { state, dispatch } = useAppState();
  const [community, setCommunity] = useState<CommunityPageDto | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const [termsAccepted, setTermsAccepted] = useState(false);
  const [ageConfirmed, setAgeConfirmed] = useState(false);
  const [reporting, setReporting] = useState(false);
  const [message, setMessage] = useState<string | null>(null);

  const load = useCallback(() => {
    communityApi
      .getCommunity(communityId, inviteToken)
      .then((c) => {
        setCommunity(c);
        setError(null);
      })
      .catch((err) => setError(err instanceof ApiError && err.status === 404 ? "This community doesn't exist, or you need an invite link to see it." : describeError(err)));
  }, [communityId, inviteToken]);
  useEffect(load, [load]);

  if (error) return <p className={styles.error}>{error}</p>;
  if (!community) return <p className={styles.muted}>Loading…</p>;

  const membership = community.viewer.membership;
  const subscription = community.viewer.subscription;
  const isActiveMember = membership?.status === "active";
  const price = community.price;
  const canBuyHere = !!config?.paymentsEnabled && (config?.purchasePlatforms ?? []).includes("web");
  const closedForJoining = community.status !== "published" || community.newMembersPaused;

  async function act(fn: () => Promise<unknown>, success?: string) {
    setBusy(true);
    setMessage(null);
    setError(null);
    try {
      await fn();
      if (success) setMessage(success);
      load();
      if (state.identity) await loadConversations(dispatch, state.identity);
    } catch (err) {
      setMessage(describeError(err));
    } finally {
      setBusy(false);
    }
  }

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

  async function openChat() {
    if (!community?.conversationId) return;
    if (state.identity && !state.conversations.some((c) => c.id === community.conversationId)) await loadConversations(dispatch, state.identity);
    dispatch({ type: "OPEN_THREAD", conversationId: community.conversationId });
  }

  return (
    <>
      {community.hasBanner && <img className={styles.banner} src={communityApi.communityImageUrl(community.id, "banner", inviteToken)} alt="" />}
      <div className={styles.hero}>
        <CommunityThumb community={community} invite={inviteToken} />
        <div className={styles.rowMain}>
          <h1 className={styles.heroTitle}>{community.name}</h1>
          <span className={styles.muted}>
            by {community.creator.displayName} (@{community.creator.handle}){community.creator.verified ? " · verified creator" : ""}
          </span>
        </div>
      </div>
      <div className={styles.chips}>
        <span className={styles.chip}>{community.kind === "channel" ? "Channel: the creator posts, members read" : "Group: members talk together"}</span>
        <span className={styles.chip}>{CATEGORY_LABELS[community.category] ?? community.category}</span>
        {community.memberCount !== null && <span className={styles.chip}>{community.memberCount} members</span>}
        {community.status !== "published" && <span className={styles.chipWarn}>{community.status}</span>}
        <span className={styles.chip}>Code {community.code}</span>
      </div>
      {community.description && <p className={styles.text}>{community.description}</p>}

      {isActiveMember && (
        <div className={styles.panelAccent}>
          <strong>You're a member{membership?.role !== "member" ? ` (${membership?.role})` : ""}.</strong>
          {subscription && (
            <span className={styles.muted}>
              {subscription.entitlementStatus === "cancel_scheduled"
                ? `Your membership ends on ${formatDate(subscription.accessUntil)}.`
                : subscription.renewsOn
                  ? `Renews on ${formatDate(subscription.renewsOn)}.`
                  : null}
            </span>
          )}
          <div className={styles.actions}>
            <button type="button" className={styles.primaryButton} onClick={openChat}>
              Open {community.kind === "channel" ? "channel" : "chat"}
            </button>
            {community.viewer.isStaff && (
              <button type="button" className={styles.secondaryButton} onClick={onManage}>
                Manage
              </button>
            )}
            {membership?.role !== "owner" && (
              <button
                type="button"
                className={styles.dangerButton}
                disabled={busy}
                onClick={() => {
                  const note = subscription ? " Your subscription stops renewing; you won't be charged again. You can come back until the paid period ends." : "";
                  if (window.confirm(`Leave ${community.name}?${note}`)) void act(() => communityApi.leaveCommunity(communityId), "You left the community.");
                }}
              >
                Leave
              </button>
            )}
          </div>
        </div>
      )}

      {membership && MEMBERSHIP_TEXT[membership.status] && <p className={styles.notice}>{MEMBERSHIP_TEXT[membership.status]}</p>}
      {subscription?.entitlementStatus === "pending" && <p className={styles.notice}>Your payment is being confirmed with Stripe. This page updates once it's done.</p>}

      {!isActiveMember && membership?.status !== "banned" && !(membership && membership.status.startsWith("pending")) && (
        <div className={styles.panel}>
          {community.accessType === "free" ? (
            <>
              <strong>Free to join</strong>
              {community.staff === null && closedForJoining && <span className={styles.muted}>Not accepting new members right now.</span>}
              <div className={styles.actions}>
                <button
                  type="button"
                  className={styles.primaryButton}
                  disabled={busy || closedForJoining}
                  onClick={() => act(() => communityApi.joinCommunity(communityId, inviteToken), "Joined. Waiting for the community's key…")}
                >
                  Join
                </button>
              </div>
            </>
          ) : subscription && (subscription.entitlementStatus === "cancel_scheduled" || subscription.entitlementStatus === "active") ? (
            <>
              <strong>You still have a paid membership until {formatDate(subscription.accessUntil)}.</strong>
              <div className={styles.actions}>
                <button type="button" className={styles.primaryButton} disabled={busy} onClick={() => act(() => communityApi.joinCommunity(communityId), "Welcome back.")}>
                  Rejoin
                </button>
              </div>
            </>
          ) : price ? (
            <>
              <span className={styles.price}>
                {formatMoney(price.amountMinor, price.currency)} <span className={styles.muted}>/ month</span>
              </span>
              <span className={styles.muted}>Billed monthly by RedVault through Stripe. Your card statement shows RedVault; this community's code is {community.code}.</span>
              <label className={styles.check}>
                <input type="checkbox" checked={termsAccepted} onChange={(e) => setTermsAccepted(e.target.checked)} />
                <span>
                  I agree to the community membership terms. This membership <strong>renews automatically every month</strong> at {formatMoney(price.amountMinor, price.currency)} until I cancel. I can cancel any time
                  in Communities → Memberships and keep access until the end of the period I paid for. Refunds follow the terms.
                </span>
              </label>
              <label className={styles.check}>
                <input type="checkbox" checked={ageConfirmed} onChange={(e) => setAgeConfirmed(e.target.checked)} />
                <span>I'm 18 or older.</span>
              </label>
              {!canBuyHere && <p className={styles.noticeWarn}>Memberships can't be bought here right now.</p>}
              <div className={styles.actions}>
                <button type="button" className={styles.primaryButton} disabled={busy || !termsAccepted || !ageConfirmed || !canBuyHere || closedForJoining} onClick={subscribe}>
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

      {community.previewText && (
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>PREVIEW</span>
          <p className={styles.quote}>{community.previewText}</p>
        </div>
      )}
      {community.rules && (
        <div className={styles.section}>
          <span className={`${styles.sectionLabel} type-caption`}>RULES</span>
          <p className={styles.text}>{community.rules}</p>
        </div>
      )}
      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>PRIVACY</span>
        <p className={styles.muted}>
          Messages, images, voice notes and files inside are end-to-end encrypted: neither RedVault nor Stripe can read them. RedVault does see this public listing, who is a member, and
          the state of each membership's billing. When someone leaves, the members' apps switch to a new key they never get.
        </p>
      </div>
      <div className={styles.actions}>
        <button type="button" className={styles.small} onClick={() => setReporting(true)}>
          Report this community
        </button>
      </div>
      {reporting && (
        <CommunityReportDialog communityId={community.id} target={{ kind: "community" }} onClose={() => setReporting(false)} onDone={() => setMessage("Report sent. Thank you.")} />
      )}
    </>
  );
}

// --- My communities -----------------------------------------------------------------------

const STATUS_CHIP: Record<string, { label: string; tone: "accent" | "warn" | "danger" | "plain" }> = {
  active: { label: "Member", tone: "accent" },
  pending_enrollment: { label: "Waiting for key", tone: "warn" },
  pending_approval: { label: "Requested", tone: "warn" },
  banned: { label: "Banned", tone: "danger" },
};

function MyCommunities({ onOpen }: { onOpen: (id: string) => void }) {
  const [list, setList] = useState<MyCommunityDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    communityApi.myCommunities().then(setList).catch((err) => setError(describeError(err)));
  }, []);
  if (error) return <p className={styles.error}>{error}</p>;
  if (!list) return <p className={styles.muted}>Loading…</p>;
  if (list.length === 0) return <div className={styles.empty}>You haven't joined any communities yet.</div>;
  return (
    <div className={styles.list}>
      {list.map((c) => {
        const chip = STATUS_CHIP[c.membership.status] ?? { label: c.membership.status, tone: "plain" as const };
        const chipClass = chip.tone === "accent" ? styles.chipAccent : chip.tone === "warn" ? styles.chipWarn : chip.tone === "danger" ? styles.chipDanger : styles.chip;
        return (
          <div key={c.id} className={styles.row}>
            <span className={styles.cardHead}>
              <CommunityThumb community={c} />
              <span className={styles.rowMain}>
                <span className={styles.cardTitle}>{c.name}</span>
                <span className={styles.muted}>
                  {c.kind === "channel" ? "Channel" : "Group"}
                  {c.membership.role !== "member" ? ` · ${c.membership.role}` : ""}
                  {c.status !== "published" ? ` · ${c.status}` : ""}
                </span>
              </span>
            </span>
            <span className={styles.rowActions}>
              <span className={chipClass}>{chip.label}</span>
              <button type="button" className={styles.small} onClick={() => onOpen(c.id)}>
                Open
              </button>
            </span>
          </div>
        );
      })}
    </div>
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

function Memberships({ checkoutId, onOpen }: { checkoutId?: string; onOpen: (id: string) => void }) {
  const [list, setList] = useState<MembershipViewDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busyId, setBusyId] = useState<string | null>(null);
  const [confirming, setConfirming] = useState<string | null>(checkoutId ?? null);
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
      {confirming && <p className={styles.notice}>Confirming your payment with Stripe… Access starts as soon as Stripe confirms it, not before.</p>}
      {error && <p className={styles.error}>{error}</p>}
      {list === null && !error && <p className={styles.muted}>Loading…</p>}
      {list?.length === 0 && <div className={styles.empty}>No paid memberships.</div>}
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
              <button type="button" className={styles.small} onClick={() => onOpen(m.community.id)}>
                View community
              </button>
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
