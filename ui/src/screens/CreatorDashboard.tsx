// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useState } from "react";
import * as communityApi from "../lib/communityApi.js";
import { type CommunitiesConfigDto, type CreatorDashboardDto, type CreatorDto, formatMoney } from "../lib/communityApi.js";
import * as crypto from "../lib/e2eCrypto.js";
import { describeError, formatDate } from "./Communities.js";
import { EarningsByProduct, EarningsPanel, type EarningsTab } from "./CreatorEarnings.js";
import styles from "./Communities.module.css";

type Tab = "overview" | "communities" | EarningsTab | "earnings" | "payouts" | "settings";

const TAB_LABELS: Record<Tab, string> = {
  overview: "Overview",
  communities: "Communities",
  "paid-messages": "Paid messages",
  tips: "Tips",
  posts: "Exclusive posts",
  transactions: "Transactions",
  earnings: "Earnings",
  payouts: "Payouts",
  settings: "Settings",
};

const COUNTRY_NAMES: Record<string, string> = { us: "United States", gb: "United Kingdom", ca: "Canada", au: "Australia", nz: "New Zealand", ie: "Ireland", de: "Germany", fr: "France" };

export function CreatorDashboard({ config, onOpenCommunity, onCreate }: { config: CommunitiesConfigDto | null; onOpenCommunity: (id: string) => void; onCreate: () => void }) {
  const [creator, setCreator] = useState<CreatorDto | null | undefined>(undefined);
  const [error, setError] = useState<string | null>(null);
  const [tab, setTab] = useState<Tab>("overview");

  const load = useCallback(async () => {
    try {
      let { creator: mine } = await communityApi.getMyCreator();
      // Back from Stripe's onboarding (or any visit while it's unfinished): ask Stripe how it went.
      if (mine && mine.payments.configured && mine.payments.status !== "not_started" && !mine.payments.ready) {
        mine = (await communityApi.refreshCreatorOnboarding().catch(() => ({ creator: mine }))).creator;
      }
      setCreator(mine);
    } catch (err) {
      setError(describeError(err));
    }
  }, []);
  useEffect(() => {
    void load();
  }, [load]);

  if (error) return <p className={styles.error}>{error}</p>;
  if (creator === undefined) return <p className={styles.muted}>Loading…</p>;
  if (creator === null) return <BecomeCreator config={config} onDone={setCreator} />;

  return (
    <>
      <div className={styles.hero}>
        {creator.hasAvatar ? <img className={styles.thumb} src={communityApi.creatorAvatarUrl(creator.id)} alt="" /> : <span className={styles.thumbLetter}>{creator.displayName.charAt(0).toUpperCase()}</span>}
        <div className={styles.rowMain}>
          <h1 className={styles.heroTitle}>{creator.displayName}</h1>
          <span className={styles.muted}>
            @{creator.handle}
            {creator.verified ? " · verified" : ""}
            {creator.status !== "active" ? ` · ${creator.status}` : ""}
          </span>
        </div>
      </div>
      <div className={styles.chips} role="tablist">
        {(Object.keys(TAB_LABELS) as Tab[]).map((t) => (
          <button key={t} type="button" role="tab" aria-selected={tab === t} className={tab === t ? styles.chipAccent : styles.chip} onClick={() => setTab(t)}>
            {TAB_LABELS[t]}
          </button>
        ))}
      </div>
      {tab === "settings" ? (
        <CreatorSettings creator={creator} config={config} onChange={setCreator} />
      ) : tab === "paid-messages" || tab === "tips" || tab === "posts" || tab === "transactions" ? (
        <EarningsPanel tab={tab} />
      ) : (
        <DashboardData creator={creator} tab={tab} onOpenCommunity={onOpenCommunity} onCreate={onCreate} />
      )}
    </>
  );
}

function BecomeCreator({ config, onDone }: { config: CommunitiesConfigDto | null; onDone: (c: CreatorDto) => void }) {
  const [handle, setHandle] = useState("");
  const [displayName, setDisplayName] = useState("");
  const [bio, setBio] = useState("");
  const [link, setLink] = useState("");
  const [accepted, setAccepted] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function submit() {
    setBusy(true);
    setError(null);
    try {
      const { creator } = await communityApi.becomeCreator({ handle, displayName, bio, profileLink: link.trim() || null, acceptTerms: true });
      onDone(creator);
    } catch (err) {
      setError(describeError(err));
      setBusy(false);
    }
  }

  return (
    <div className={styles.form}>
      <div className={styles.panelAccent}>
        <strong>Become a creator</strong>
        <span className={styles.muted}>
          Run free or paid channels and groups from this account. Your creator handle is public and separate from your private RedVault username. To be paid, you'll verify your identity
          and bank details with Stripe; RedVault never sees them.
        </span>
      </div>
      <div className={styles.fieldRow}>
        <label className={styles.field}>
          Creator handle (public)
          <input className={styles.input} value={handle} maxLength={32} placeholder="a–z, 0–9, _" onChange={(e) => setHandle(e.target.value.toLowerCase())} />
        </label>
        <label className={styles.field}>
          Display name
          <input className={styles.input} value={displayName} maxLength={60} onChange={(e) => setDisplayName(e.target.value)} />
        </label>
      </div>
      <label className={styles.field}>
        About (public)
        <textarea className={styles.textarea} value={bio} maxLength={1000} onChange={(e) => setBio(e.target.value)} />
      </label>
      <label className={styles.field}>
        Link (optional, https://)
        <input className={styles.input} value={link} maxLength={200} onChange={(e) => setLink(e.target.value)} />
      </label>
      <label className={styles.check}>
        <input type="checkbox" checked={accepted} onChange={(e) => setAccepted(e.target.checked)} />
        <span>
          I accept the creator terms (version {config?.creatorTermsVersion ?? "current"}): I'm responsible for what I publish; no illegal content, scams, sexual content involving minors,
          intimate images shared without consent, or other prohibited material; RedVault keeps a commission on each payment as shown when I set a price; payouts need Stripe's identity
          checks; RedVault can suspend communities that break the rules.
        </span>
      </label>
      {error && <p className={styles.error}>{error}</p>}
      <div className={styles.actions}>
        <button type="button" className={styles.primaryButton} disabled={busy || !accepted || handle.length < 3 || !displayName.trim()} onClick={submit}>
          Create creator profile
        </button>
      </div>
    </div>
  );
}

function Stat({ label, value, hint }: { label: string; value: string; hint?: string }) {
  return (
    <div className={styles.stat}>
      <span className={styles.muted}>{label}</span>
      <span className={styles.statValue}>{value}</span>
      {hint && <span className={styles.muted}>{hint}</span>}
    </div>
  );
}

function DashboardData({ creator, tab, onOpenCommunity, onCreate }: { creator: CreatorDto; tab: Tab; onOpenCommunity: (id: string) => void; onCreate: () => void }) {
  const [data, setData] = useState<CreatorDashboardDto | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    communityApi
      .getCreatorDashboard()
      .then(setData)
      .catch((err) => setError(describeError(err)));
  }, []);

  if (error) return <p className={styles.error}>{error}</p>;
  if (!data) return <p className={styles.muted}>Loading…</p>;
  const cur = data.totals.currency;
  const money = (n: number) => formatMoney(n, cur);

  if (tab === "overview") {
    return (
      <>
        <div className={styles.stats}>
          <Stat label="Paying members" value={String(data.totals.payingMembers)} />
          <Stat label="Communities" value={String(data.communities.length)} />
          <Stat label="Monthly recurring (estimate)" value={money(data.totals.monthlyRecurringEstimateMinor)} hint="If every renewing member renews" />
          <Stat label="Gross paid" value={money(data.totals.grossPaidMinor)} hint="All time, before commission" />
          <Stat label="RedVault commission" value={money(data.totals.platformCommissionMinor)} />
          <Stat label="Refunds" value={money(data.totals.refundsMinor)} />
          <Stat label="Disputes" value={money(data.totals.disputesMinor)} hint="Borne by RedVault" />
          <Stat label="Your earnings" value={money(data.totals.creatorEarningsMinor)} hint="Earned, not the same as paid out" />
        </div>
        <div className={styles.stats}>
          <Stat label="Still subscribed" value={`${data.retention.stillActive} of ${data.retention.everPaid}`} hint="Members who ever paid" />
          <Stat label="Renewals failing now" value={String(data.failedRenewals)} hint="In their grace period" />
        </div>
        <MonthsTable data={data} money={money} />
        <EarningsByProduct />
      </>
    );
  }

  if (tab === "communities") {
    return (
      <>
        <div className={styles.actions}>
          <button type="button" className={styles.primaryButton} onClick={onCreate}>
            New community
          </button>
        </div>
        {data.communities.length === 0 && <div className={styles.empty}>No communities yet.</div>}
        <div className={styles.list}>
          {data.communities.map((c) => (
            <div key={c.id} className={styles.row}>
              <span className={styles.rowMain}>
                <span className={styles.cardTitle}>{c.name}</span>
                <span className={styles.muted}>
                  {c.kind === "channel" ? "Channel" : "Group"} · {c.accessType === "paid" && c.price ? `${money(c.price.amountMinor)}/month` : "Free"} · {c.activeMembers} members
                  {c.accessType === "paid" ? ` · ${c.payingMembers} paying` : ""}
                </span>
              </span>
              <span className={styles.rowActions}>
                <span className={c.status === "published" ? styles.chipAccent : styles.chipWarn}>{c.status}</span>
                <button type="button" className={styles.small} onClick={() => onOpenCommunity(c.id)}>
                  Manage
                </button>
              </span>
            </div>
          ))}
        </div>
      </>
    );
  }

  if (tab === "earnings") {
    return (
      <>
        <p className={styles.notice}>
          Gross is what members paid (before tax). RedVault keeps its commission from each payment and pays Stripe's processing fees. Your earnings go to your Stripe balance with each
          payment; refunds take your share back. Earnings aren't payouts: Stripe pays your balance out to your bank on its schedule (see Payouts).
        </p>
        <div className={styles.stats}>
          <Stat label="Gross paid" value={money(data.totals.grossPaidMinor)} />
          <Stat label="RedVault commission" value={money(data.totals.platformCommissionMinor)} />
          <Stat label="Refunds" value={money(data.totals.refundsMinor)} />
          <Stat label="Your earnings" value={money(data.totals.creatorEarningsMinor)} />
        </div>
        <MonthsTable data={data} money={money} />
      </>
    );
  }

  // Payouts
  return (
    <>
      {!data.payouts.ready && <p className={styles.noticeWarn}>Payouts aren't set up yet. Finish Stripe onboarding in Settings.</p>}
      {data.payouts.balance && (
        <div className={styles.stats}>
          {data.payouts.balance.available.map((b) => (
            <Stat key={`a-${b.currency}`} label="Available for payout" value={formatMoney(b.amountMinor, b.currency)} hint="In your Stripe balance" />
          ))}
          {data.payouts.balance.pending.map((b) => (
            <Stat key={`p-${b.currency}`} label="Pending" value={formatMoney(b.amountMinor, b.currency)} hint="Not yet available" />
          ))}
        </div>
      )}
      <div className={styles.tableWrap}>
        <table className={styles.table}>
          <thead>
            <tr>
              <th>Created</th>
              <th>Status</th>
              <th>Arrives</th>
              <th className={styles.num}>Amount</th>
            </tr>
          </thead>
          <tbody>
            {data.payouts.recent.length === 0 && (
              <tr>
                <td colSpan={4}>No payouts yet.</td>
              </tr>
            )}
            {data.payouts.recent.map((p) => (
              <tr key={p.id}>
                <td>{formatDate(p.createdAt)}</td>
                <td>{p.status.replace("_", " ")}</td>
                <td>{formatDate(p.arrivalDate)}</td>
                <td className={styles.num}>{formatMoney(p.amountMinor, p.currency)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      {creator.payments.status !== "not_started" && (
        <div className={styles.actions}>
          <button
            type="button"
            className={styles.secondaryButton}
            disabled={busy}
            onClick={async () => {
              setBusy(true);
              try {
                window.location.assign((await communityApi.creatorDashboardLink()).url);
              } catch (err) {
                setError(describeError(err));
                setBusy(false);
              }
            }}
          >
            Open Stripe dashboard
          </button>
        </div>
      )}
    </>
  );
}

function MonthsTable({ data, money }: { data: CreatorDashboardDto; money: (n: number) => string }) {
  return (
    <div className={styles.tableWrap}>
      <table className={styles.table}>
        <thead>
          <tr>
            <th>Month</th>
            <th className={styles.num}>New</th>
            <th className={styles.num}>Renewals</th>
            <th className={styles.num}>Cancellations</th>
            <th className={styles.num}>Refunds</th>
            <th className={styles.num}>Gross</th>
            <th className={styles.num}>Your net</th>
          </tr>
        </thead>
        <tbody>
          {[...data.months].reverse().map((m) => (
            <tr key={m.month}>
              <td>{m.month}</td>
              <td className={styles.num}>{m.newMembers}</td>
              <td className={styles.num}>{m.renewals}</td>
              <td className={styles.num}>{m.cancellations}</td>
              <td className={styles.num}>{m.refunds}</td>
              <td className={styles.num}>{money(m.grossMinor)}</td>
              <td className={styles.num}>{money(m.creatorNetMinor)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function CreatorSettings({ creator, config, onChange }: { creator: CreatorDto; config: CommunitiesConfigDto | null; onChange: (c: CreatorDto) => void }) {
  const [displayName, setDisplayName] = useState(creator.displayName);
  const [bio, setBio] = useState(creator.bio ?? "");
  const [link, setLink] = useState(creator.profileLink ?? "");
  const [country, setCountry] = useState(config?.creatorCountries[0] ?? "us");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  async function run(fn: () => Promise<{ creator: CreatorDto } | void>, done?: string) {
    setBusy(true);
    setError(null);
    setMessage(null);
    try {
      const result = await fn();
      if (result) onChange(result.creator);
      if (done) setMessage(done);
    } catch (err) {
      setError(describeError(err));
    } finally {
      setBusy(false);
    }
  }

  async function pickAvatar(file: File | undefined) {
    if (!file) return;
    if (file.size > 300 * 1024) {
      setError("Images can be at most 300 KB.");
      return;
    }
    await crypto.ready();
    const bytes = new Uint8Array(await file.arrayBuffer());
    await run(() => communityApi.updateCreator({ avatar: crypto.toBase64(bytes) }), "Picture updated.");
  }

  const payments = creator.payments;
  return (
    <div className={styles.form}>
      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>PUBLIC PROFILE</span>
        <label className={styles.field}>
          Display name
          <input className={styles.input} value={displayName} maxLength={60} onChange={(e) => setDisplayName(e.target.value)} />
        </label>
        <label className={styles.field}>
          About
          <textarea className={styles.textarea} value={bio} maxLength={1000} onChange={(e) => setBio(e.target.value)} />
        </label>
        <label className={styles.field}>
          Link (https://)
          <input className={styles.input} value={link} maxLength={200} onChange={(e) => setLink(e.target.value)} />
        </label>
        <label className={styles.field}>
          Picture (PNG, JPEG or WebP, up to 300 KB)
          <input type="file" accept="image/png,image/jpeg,image/webp" onChange={(e) => void pickAvatar(e.target.files?.[0])} />
        </label>
        <div className={styles.actions}>
          <button type="button" className={styles.primaryButton} disabled={busy} onClick={() => run(() => communityApi.updateCreator({ displayName, bio, profileLink: link.trim() || null }), "Saved.")}>
            Save profile
          </button>
        </div>
      </div>

      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>PAID MEMBERSHIPS AND PAYOUTS</span>
        {!payments.configured ? (
          <p className={styles.notice}>Paid memberships aren't available on this server yet. Free communities work.</p>
        ) : (
          <>
            <label className={styles.check}>
              <input
                type="checkbox"
                checked={creator.paidMembershipsEnabled}
                disabled={busy}
                onChange={(e) => run(() => communityApi.updateCreator({ paidMembershipsEnabled: e.target.checked }))}
              />
              <span>Offer paid memberships</span>
            </label>
            <p className={payments.ready ? styles.success : styles.notice}>
              {payments.ready
                ? "Payouts are set up. You can publish paid communities."
                : payments.status === "not_started"
                  ? "To publish a paid community, verify your identity and add a bank account with Stripe. RedVault never sees those details."
                  : payments.status === "rejected"
                    ? "Stripe can't offer payouts to this account."
                    : payments.requirementsDue
                      ? "Stripe needs more information before you can be paid."
                      : "Stripe is reviewing your details."}
            </p>
            {creator.paidMembershipsEnabled && !payments.ready && payments.status !== "rejected" && (
              <div className={styles.toolbar}>
                {payments.status === "not_started" && (
                  <select className={styles.select} aria-label="Country" value={country} onChange={(e) => setCountry(e.target.value)}>
                    {(config?.creatorCountries ?? ["us"]).map((c) => (
                      <option key={c} value={c}>
                        {COUNTRY_NAMES[c] ?? c.toUpperCase()}
                      </option>
                    ))}
                  </select>
                )}
                <button
                  type="button"
                  className={styles.primaryButton}
                  style={{ width: "auto" }}
                  disabled={busy}
                  onClick={() =>
                    run(async () => {
                      const { url } = await communityApi.startCreatorOnboarding(country);
                      window.location.assign(url);
                    })
                  }
                >
                  {payments.status === "not_started" ? "Set up payouts with Stripe" : "Continue with Stripe"}
                </button>
                {payments.status !== "not_started" && (
                  <button type="button" className={styles.secondaryButton} style={{ width: "auto" }} disabled={busy} onClick={() => run(() => communityApi.refreshCreatorOnboarding(), "Status updated.")}>
                    Check status
                  </button>
                )}
              </div>
            )}
          </>
        )}
      </div>
      {message && <p className={styles.success}>{message}</p>}
      {error && <p className={styles.error}>{error}</p>}
    </div>
  );
}
