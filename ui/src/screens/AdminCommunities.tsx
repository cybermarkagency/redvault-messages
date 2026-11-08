// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useState } from "react";
import * as communityApi from "../lib/communityApi.js";
import { type AdminOverviewDto, type AdminTransactionDto, formatMoney, formatRate, type ReportDto } from "../lib/communityApi.js";
import { describeError, formatDate } from "./Communities.js";
import { parseDollars } from "./CommunityCreate.js";
import styles from "./Communities.module.css";

type Section = "overview" | "transactions" | "creators" | "communities" | "reports";

/** The admin panel's Communities tab: commission, money, creators, communities and reports. Every action is audited server-side. */
export function AdminCommunities() {
  const [section, setSection] = useState<Section>("overview");
  return (
    <div className={styles.body}>
      <div className={styles.chips} role="tablist">
        {(["overview", "transactions", "creators", "communities", "reports"] as Section[]).map((s) => (
          <button key={s} type="button" role="tab" aria-selected={section === s} className={section === s ? styles.chipAccent : styles.chip} onClick={() => setSection(s)}>
            {s[0].toUpperCase() + s.slice(1)}
          </button>
        ))}
      </div>
      {section === "overview" && <Overview />}
      {section === "transactions" && <Transactions />}
      {section === "creators" && <Creators />}
      {section === "communities" && <CommunitiesList />}
      {section === "reports" && <PlatformReports />}
    </div>
  );
}

function Overview() {
  const [data, setData] = useState<AdminOverviewDto | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [rate, setRate] = useState("12");
  const [min, setMin] = useState("1.00");
  const [max, setMax] = useState("500.00");
  const [note, setNote] = useState("");
  const [message, setMessage] = useState<string | null>(null);

  const load = useCallback(() => {
    communityApi
      .adminCommunityOverview()
      .then((d) => {
        setData(d);
        const current = d.rules[0];
        if (current) {
          setRate(String(current.rateBps / 100));
          setMin((current.minPriceMinor / 100).toFixed(2));
          setMax((current.maxPriceMinor / 100).toFixed(2));
        }
      })
      .catch((err) => setError(describeError(err)));
  }, []);
  useEffect(load, [load]);

  async function addRule() {
    setMessage(null);
    const rateBps = Math.round(Number(rate) * 100);
    const minPriceMinor = parseDollars(min);
    const maxPriceMinor = parseDollars(max);
    if (!Number.isFinite(rateBps) || minPriceMinor === null || maxPriceMinor === null) {
      setMessage("Check the numbers.");
      return;
    }
    if (!window.confirm(`Set the commission to ${formatRate(rateBps)} for NEW memberships from now on? Existing memberships keep their rate.`)) return;
    try {
      await communityApi.adminAddCommissionRule({ rateBps, minPriceMinor, maxPriceMinor, note });
      setMessage("Saved as a new version.");
      load();
    } catch (err) {
      setMessage(describeError(err));
    }
  }

  if (error) return <p className={styles.error}>{error}</p>;
  if (!data) return <p className={styles.muted}>Loading…</p>;
  const usd = (n: number) => formatMoney(n, "usd");
  return (
    <>
      <div className={styles.stats}>
        <div className={styles.stat}>
          <span className={styles.muted}>Net commission (ledger)</span>
          <span className={styles.statValue}>{usd(data.platformCommissionNetMinor)}</span>
        </div>
        <div className={styles.stat}>
          <span className={styles.muted}>Stripe processing fees</span>
          <span className={styles.statValue}>{usd(data.processingFeesMinor)}</span>
        </div>
        <div className={styles.stat}>
          <span className={styles.muted}>Refund losses</span>
          <span className={styles.statValue}>{usd(data.refundLossesMinor)}</span>
        </div>
        <div className={styles.stat}>
          <span className={styles.muted}>Dispute losses</span>
          <span className={styles.statValue}>{usd(data.disputeLossesMinor)}</span>
        </div>
        <div className={styles.stat}>
          <span className={styles.muted}>Platform balance at Stripe (ledger)</span>
          <span className={styles.statValue}>{usd(data.providerClearingMinor)}</span>
        </div>
      </div>
      {(data.mismatches > 0 || data.unbalancedJournals > 0) && (
        <p className={styles.noticeWarn}>
          {data.mismatches} transaction(s) don't match Stripe and {data.unbalancedJournals} ledger journal(s) don't balance. See Transactions.
        </p>
      )}
      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>COMMISSION AND PRICE LIMITS</span>
        <span className={styles.muted}>A change is a new version for memberships started from now on. Recorded transactions and existing memberships never change.</span>
        <div className={styles.fieldRow}>
          <label className={styles.field}>
            Commission (10–15%)
            <input className={styles.input} inputMode="decimal" value={rate} onChange={(e) => setRate(e.target.value)} />
          </label>
          <label className={styles.field}>
            Minimum monthly price (USD)
            <input className={styles.input} inputMode="decimal" value={min} onChange={(e) => setMin(e.target.value)} />
          </label>
          <label className={styles.field}>
            Maximum monthly price (USD)
            <input className={styles.input} inputMode="decimal" value={max} onChange={(e) => setMax(e.target.value)} />
          </label>
        </div>
        <label className={styles.field}>
          Note (why, for the audit log)
          <input className={styles.input} value={note} maxLength={500} onChange={(e) => setNote(e.target.value)} />
        </label>
        <div className={styles.actions}>
          <button type="button" className={styles.primaryButton} onClick={addRule}>
            Save new version
          </button>
        </div>
        {message && <p className={styles.notice}>{message}</p>}
        <div className={styles.tableWrap}>
          <table className={styles.table}>
            <thead>
              <tr>
                <th>Version</th>
                <th>Rate</th>
                <th>Price limits</th>
                <th>From</th>
                <th>Note</th>
              </tr>
            </thead>
            <tbody>
              {data.rules.map((r) => (
                <tr key={r.id}>
                  <td>{r.version}</td>
                  <td>{formatRate(r.rateBps)}</td>
                  <td>
                    {usd(r.minPriceMinor)} – {usd(r.maxPriceMinor)}
                  </td>
                  <td>{formatDate(r.effectiveFrom)}</td>
                  <td>{r.note}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>
    </>
  );
}

function Transactions() {
  const [filter, setFilter] = useState<"" | "pending" | "matched" | "mismatch">("");
  const [list, setList] = useState<AdminTransactionDto[] | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const load = useCallback(() => {
    communityApi
      .adminCommunityTransactions(filter || undefined)
      .then(setList)
      .catch((err) => setMessage(describeError(err)));
  }, [filter]);
  useEffect(load, [load]);

  async function refund(t: AdminTransactionDto) {
    const raw = window.prompt(`Refund how much? Blank = the whole ${formatMoney(t.grossAmountMinor, t.currency)}. The creator's share is taken back from their Stripe balance.`, "");
    if (raw === null) return;
    const amount = raw.trim() ? parseDollars(raw) : null;
    if (raw.trim() && amount === null) return setMessage("Enter an amount like 5 or 4.99.");
    const reason = window.prompt("Reason (kept in the audit log):", "") ?? "";
    if (!reason.trim()) return setMessage("A reason is required.");
    try {
      await communityApi.adminRefundCommunityPayment(t.id, amount, reason.trim());
      setMessage("Refund sent to Stripe.");
      load();
    } catch (err) {
      setMessage(describeError(err));
    }
  }

  return (
    <>
      <div className={styles.toolbar}>
        <select className={styles.select} aria-label="Reconciliation" value={filter} onChange={(e) => setFilter(e.target.value as typeof filter)}>
          <option value="">All</option>
          <option value="mismatch">Mismatched with Stripe</option>
          <option value="pending">Awaiting Stripe fees</option>
          <option value="matched">Matched</option>
        </select>
        <button
          type="button"
          className={styles.small}
          onClick={() =>
            communityApi
              .adminReconcileCommunities()
              .then((r) => {
                setMessage(`Checked ${r.checked}; ${r.mismatches} mismatch(es); ${r.unbalancedJournals} unbalanced journal(s).`);
                load();
              })
              .catch((err) => setMessage(describeError(err)))
          }
        >
          Reconcile now
        </button>
      </div>
      {message && <p className={styles.notice}>{message}</p>}
      <div className={styles.tableWrap}>
        <table className={styles.table}>
          <thead>
            <tr>
              <th>Date</th>
              <th>Kind</th>
              <th className={styles.num}>Gross</th>
              <th className={styles.num}>Commission</th>
              <th className={styles.num}>Creator</th>
              <th className={styles.num}>Fees</th>
              <th>Reconciliation</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {list?.map((t) => (
              <tr key={t.id}>
                <td>{formatDate(t.occurredAt)}</td>
                <td>{t.kind}</td>
                <td className={styles.num}>{formatMoney(t.grossAmountMinor, t.currency)}</td>
                <td className={styles.num}>
                  {formatMoney(t.commissionAmountMinor, t.currency)} <span className={styles.muted}>({formatRate(t.commissionBps)})</span>
                </td>
                <td className={styles.num}>{formatMoney(t.creatorAmountMinor, t.currency)}</td>
                <td className={styles.num}>{t.processingFeeMinor === null ? "—" : formatMoney(t.processingFeeMinor, t.currency)}</td>
                <td title={t.reconciliationNote ?? undefined}>{t.reconciliationStatus}</td>
                <td>
                  {t.kind === "payment" && (
                    <button type="button" className={styles.smallDanger} onClick={() => refund(t)}>
                      Refund
                    </button>
                  )}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </>
  );
}

function Creators() {
  const [list, setList] = useState<Awaited<ReturnType<typeof communityApi.adminCommunityCreators>> | null>(null);
  const [error, setError] = useState<string | null>(null);
  const load = useCallback(() => {
    communityApi.adminCommunityCreators().then(setList).catch((err) => setError(describeError(err)));
  }, []);
  useEffect(load, [load]);
  const act = (id: string, change: { status?: "active" | "suspended"; verified?: boolean }, confirmText?: string) => {
    if (confirmText && !window.confirm(confirmText)) return;
    communityApi.adminSetCreator(id, change).then(load).catch((err) => setError(describeError(err)));
  };
  return (
    <>
      {error && <p className={styles.error}>{error}</p>}
      <div className={styles.list}>
        {list?.length === 0 && <div className={styles.empty}>No creators yet.</div>}
        {list?.map((c) => (
          <div key={c.id} className={styles.row}>
            <span className={styles.rowMain}>
              <span className={styles.cardTitle}>
                {c.displayName} @{c.handle}
              </span>
              <span className={styles.muted}>
                {c.status} · since {formatDate(c.createdAt)} · payouts: {c.payments ? `${c.payments.status}${c.payments.requirementsDue ? " (info due)" : ""}${c.payments.livemode ? "" : " · test mode"}` : "not set up"}
              </span>
            </span>
            <span className={styles.rowActions}>
              <button type="button" className={styles.small} onClick={() => act(c.id, { verified: !c.verified })}>
                {c.verified ? "Remove badge" : "Verify"}
              </button>
              {c.status === "active" ? (
                <button type="button" className={styles.smallDanger} onClick={() => act(c.id, { status: "suspended" }, "Suspend this creator? All their communities are suspended and billing pauses.")}>
                  Suspend
                </button>
              ) : (
                <button type="button" className={styles.small} onClick={() => act(c.id, { status: "active" })}>
                  Reinstate
                </button>
              )}
            </span>
          </div>
        ))}
      </div>
    </>
  );
}

function CommunitiesList() {
  const [list, setList] = useState<Awaited<ReturnType<typeof communityApi.adminCommunities>> | null>(null);
  const [error, setError] = useState<string | null>(null);
  const load = useCallback(() => {
    communityApi.adminCommunities().then(setList).catch((err) => setError(describeError(err)));
  }, []);
  useEffect(load, [load]);
  const act = (id: string, action: "suspend" | "unsuspend" | "close", text: string) => {
    if (!window.confirm(text)) return;
    communityApi.adminCommunityAction(id, action).then(load).catch((err) => setError(describeError(err)));
  };
  return (
    <>
      {error && <p className={styles.error}>{error}</p>}
      <div className={styles.list}>
        {list?.length === 0 && <div className={styles.empty}>No communities yet.</div>}
        {list?.map((c) => (
          <div key={c.id} className={styles.row}>
            <span className={styles.rowMain}>
              <span className={styles.cardTitle}>{c.name}</span>
              <span className={styles.muted}>
                {c.kind} · {c.accessType} · {c.status} · {c.visibility} · epoch {c.keyEpoch}
              </span>
            </span>
            <span className={styles.rowActions}>
              {c.status === "suspended" ? (
                <button type="button" className={styles.small} onClick={() => act(c.id, "unsuspend", "Lift the suspension? Billing resumes.")}>
                  Unsuspend
                </button>
              ) : c.status !== "closed" ? (
                <button type="button" className={styles.smallDanger} onClick={() => act(c.id, "suspend", "Suspend? Nobody can read or post, and billing pauses.")}>
                  Suspend
                </button>
              ) : null}
              {c.status !== "closed" && (
                <button type="button" className={styles.smallDanger} onClick={() => act(c.id, "close", "Close now? Subscriptions end immediately and every member loses access. Refund separately if owed.")}>
                  Close now
                </button>
              )}
            </span>
          </div>
        ))}
      </div>
    </>
  );
}

function PlatformReports() {
  const [list, setList] = useState<ReportDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const load = useCallback(() => {
    communityApi.adminCommunityReports("open").then(setList).catch((err) => setError(describeError(err)));
  }, []);
  useEffect(load, [load]);
  const resolve = (id: string, status: "actioned" | "dismissed") => communityApi.adminResolveCommunityReport(id, status, null).then(load).catch((err) => setError(describeError(err)));
  return (
    <>
      <p className={styles.muted}>Reports RedVault reviews: about communities and creators, and every child-safety, non-consensual imagery or other illegal-content report. Message text appears only when the reporter chose to share it.</p>
      {error && <p className={styles.error}>{error}</p>}
      {list?.length === 0 && <div className={styles.empty}>No open reports.</div>}
      {list?.map((r) => (
        <div key={r.id} className={styles.panel}>
          <div className={styles.chips}>
            <span className={r.category === "child_safety" || r.category === "nonconsensual_imagery" ? styles.chipDanger : styles.chipWarn}>{r.category.replace("_", " ")}</span>
            <span className={styles.chip}>{r.targetType}</span>
            <span className={styles.muted}>{formatDate(r.createdAt)}</span>
          </div>
          {r.details && <p className={styles.text}>{r.details}</p>}
          {r.evidencePlaintext && <p className={styles.quote}>{r.evidencePlaintext}</p>}
          {r.evidenceCiphertextSha256 && <span className={styles.muted}>Stored ciphertext SHA-256: <span className={styles.code}>{r.evidenceCiphertextSha256}</span></span>}
          <span className={styles.muted}>Community {r.communityId ?? "—"}{r.targetUserId ? ` · user ${r.targetUserId}` : ""}</span>
          <div className={styles.actions}>
            <button type="button" className={styles.small} onClick={() => resolve(r.id, "actioned")}>
              Actioned
            </button>
            <button type="button" className={styles.small} onClick={() => resolve(r.id, "dismissed")}>
              Dismiss
            </button>
          </div>
        </div>
      ))}
    </>
  );
}
