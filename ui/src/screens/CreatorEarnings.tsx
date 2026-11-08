// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { useCallback, useEffect, useState } from "react";
import { PostCard } from "../components/earnings/PostCard.js";
import { publishPost } from "../lib/earnings.js";
import * as eapi from "../lib/earningsApi.js";
import { formatUsd, type DashboardDto, type EarningProduct, type PurchaseDto } from "../lib/earningsApi.js";
import { useAppState } from "../state/AppStateContext.js";
import { describeError, formatDate } from "./Communities.js";
import styles from "./Communities.module.css";

export type EarningsTab = "paid-messages" | "tips" | "posts" | "transactions";

const PRODUCT_NAMES: Record<EarningProduct, string> = { paid_message: "Paid messages", tip: "Tips", exclusive_post: "Exclusive posts" };
const STATUS_NAMES: Partial<Record<eapi.PurchaseStatus, string>> = {
  checkout_pending: "Awaiting payment",
  paid: "Paid",
  consuming: "Sending",
  consumed: "Delivered",
  refund_pending: "Refunding",
  refunded: "Refunded",
  disputed: "Disputed",
  expired: "Expired",
  canceled: "Canceled",
  failed: "Failed",
};

function Stat({ label, value, hint }: { label: string; value: string; hint?: string }) {
  return (
    <div className={styles.stat}>
      <span className={styles.muted}>{label}</span>
      <span className={styles.statValue}>{value}</span>
      {hint && <span className={styles.muted}>{hint}</span>}
    </div>
  );
}

function useDashboard() {
  const [data, setData] = useState<DashboardDto | null>(null);
  const [error, setError] = useState<string | null>(null);
  const reload = useCallback(() => {
    eapi
      .getEarningsDashboard()
      .then((d) => {
        setData(d);
        setError(null);
      })
      .catch((err) => setError(describeError(err)));
  }, []);
  useEffect(reload, [reload]);
  return { data, error, reload };
}

/** Paid messages, tips and exclusive posts, per product: for the dashboard's Overview and Earnings tabs. */
export function EarningsByProduct() {
  const { data, error } = useDashboard();
  if (error) return null;
  if (!data) return null;
  return (
    <section className={styles.section} aria-label="Paid messages, tips and posts">
      <h2 className={styles.sectionLabel}>Paid messages, tips and exclusive posts</h2>
      <div className={styles.tableWrap}>
        <table className={styles.table}>
          <thead>
            <tr>
              <th>Product</th>
              <th>RedVault's rate</th>
              <th className={styles.num}>Sales</th>
              <th className={styles.num}>Gross</th>
              <th className={styles.num}>RedVault fee</th>
              <th className={styles.num}>Refunded</th>
              <th className={styles.num}>Your earnings</th>
            </tr>
          </thead>
          <tbody>
            {data.products.map((p) => (
              <tr key={p.product}>
                <td>{PRODUCT_NAMES[p.product]}</td>
                <td>{p.commissionPercent}%</td>
                <td className={styles.num}>{p.count}</td>
                <td className={styles.num}>{formatUsd(p.grossMinor)}</td>
                <td className={styles.num}>{formatUsd(p.platformFeeMinor)}</td>
                <td className={styles.num}>{formatUsd(p.refundedMinor)}</td>
                <td className={styles.num}>{formatUsd(p.creatorNetMinor)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}

export function EarningsPanel({ tab }: { tab: EarningsTab }) {
  const { data, error, reload } = useDashboard();
  if (error) return <p className={styles.error}>{error}</p>;
  if (!data) return <p className={styles.muted}>Loading…</p>;
  const off = !data.available ? (
    <p className={styles.notice}>Paid messages, tips and exclusive posts aren't switched on on this server yet. Nothing can be charged until they are.</p>
  ) : !data.creatorReady ? (
    <p className={styles.noticeWarn}>Finish payout setup (Settings tab) before charging for anything. Stripe verifies who you are; RedVault never sees your bank details.</p>
  ) : null;
  if (tab === "paid-messages") return <PaidMessagesPanel data={data} notice={off} onChange={reload} />;
  if (tab === "tips") return <TipsPanel data={data} notice={off} onChange={reload} />;
  if (tab === "posts") return <PostsPanel data={data} notice={off} />;
  return <TransactionsPanel data={data} onChange={reload} />;
}

function PaidMessagesPanel({ data, notice, onChange }: { data: DashboardDto; notice: React.ReactNode; onChange: () => void }) {
  const s = data.settings;
  const [mode, setMode] = useState<eapi.PaidMessageMode>(s.paidMessagesMode);
  const [price, setPrice] = useState(s.paidMessagePriceMinor ? (s.paidMessagePriceMinor / 100).toFixed(2) : "3.00");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<string | null>(null);
  const stats = data.products.find((p) => p.product === "paid_message")!;

  async function save() {
    const cents = Math.round(Number(price) * 100);
    if (mode !== "off" && (!Number.isFinite(cents) || cents <= 0)) {
      setMessage("Enter a price, like 3.00.");
      return;
    }
    setBusy(true);
    setMessage(null);
    try {
      await eapi.updateEarningSettings({ paidMessagesMode: mode, paidMessagePriceMinor: mode === "off" ? null : cents });
      setMessage("Saved.");
      onChange();
    } catch (err) {
      setMessage(describeError(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <>
      {notice}
      <p className={styles.text}>
        Charge people who aren't your friends to message you. Friends, people you let in, and chats you started are always free. Blocking and reporting are always free, and
        nobody can pay their way past a block. Messages stay end-to-end encrypted: paying never lets anyone (RedVault or Stripe) read them.
      </p>
      {!s.acceptsMessageRequests && <p className={styles.noticeWarn}>You don't accept message requests, so strangers can't start a chat with you at all. Turn them on in Privacy settings.</p>}
      <div className={styles.form}>
        <div className={styles.radioRow} role="radiogroup" aria-label="Paid messages">
          {(
            [
              ["off", "Off", "Anyone who can message you does so for free."],
              ["first_contact", "First message", "One payment to start a chat; the rest of that chat is free."],
              ["per_message", "Every message", "Each message from someone who isn't exempt costs the price."],
            ] as const
          ).map(([value, title, hint]) => (
            <button key={value} type="button" role="radio" aria-checked={mode === value} className={`${styles.radio} ${mode === value ? styles.radioOn : ""}`} onClick={() => setMode(value)}>
              <span className={styles.radioTitle}>{title}</span>
              <span className={styles.muted}>{hint}</span>
            </button>
          ))}
        </div>
        {mode !== "off" && (
          <label className={styles.field}>
            Price (USD)
            <input className={styles.input} inputMode="decimal" value={price} onChange={(e) => setPrice(e.target.value)} aria-label="Price in US dollars" />
            <span className={styles.muted}>Between $1.00 and $100.00. RedVault keeps {stats.commissionPercent}% of each paid message.</span>
          </label>
        )}
        <div className={styles.actions}>
          <button type="button" className={styles.primaryButton} disabled={busy} onClick={save}>
            Save
          </button>
          {message && <span className={styles.muted}>{message}</span>}
        </div>
      </div>
      <div className={styles.stats}>
        <Stat label="Paid messages" value={String(stats.count)} />
        <Stat label="Your earnings" value={formatUsd(stats.creatorNetMinor)} />
        <Stat label="Exempt people" value={String(s.exemptions.length)} hint="Add them from a person's profile" />
      </div>
      <p className={styles.muted}>
        Unused paid messages are refunded after 14 days. A safety contact (once every 30 days per person) always gets through for free, so nobody has to pay to warn you about
        something urgent; misuse can be reported.
      </p>
    </>
  );
}

function TipsPanel({ data, notice, onChange }: { data: DashboardDto; notice: React.ReactNode; onChange: () => void }) {
  const [tips, setTips] = useState<PurchaseDto[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const stats = data.products.find((p) => p.product === "tip")!;
  useEffect(() => {
    eapi.listTipsReceived().then(setTips, (err) => setError(describeError(err)));
  }, []);

  async function toggle() {
    try {
      await eapi.updateEarningSettings({ tipsEnabled: !data.settings.tipsEnabled });
      onChange();
    } catch (err) {
      setError(describeError(err));
    }
  }

  return (
    <>
      {notice}
      <p className={styles.text}>
        Tips are voluntary thank-yous: they don't unlock anything, and nobody has to tip to talk to you. People choose $1, $3, $5, $10 or another amount, and can tip without
        showing you who they are (RedVault and Stripe still know who paid). RedVault keeps {stats.commissionPercent}% of each tip.
      </p>
      <div className={styles.actions}>
        <button type="button" className={data.settings.tipsEnabled ? styles.secondaryButton : styles.primaryButton} onClick={toggle}>
          {data.settings.tipsEnabled ? "Stop taking tips" : "Take tips"}
        </button>
      </div>
      <div className={styles.stats}>
        <Stat label="Tips" value={String(stats.count)} />
        <Stat label="Your earnings" value={formatUsd(stats.creatorNetMinor)} />
      </div>
      {error && <p className={styles.error}>{error}</p>}
      {tips?.length === 0 && <div className={styles.empty}>No tips yet.</div>}
      <div className={styles.list}>
        {tips?.map((t) => (
          <div key={t.id} className={styles.row}>
            <span className={styles.rowMain}>
              <span className={styles.cardTitle}>{formatUsd(t.amountMinor)}</span>
              <span className={styles.muted}>
                {t.buyer ? `From ${t.buyer.username}` : "From a supporter"} · {formatDate(t.paidAt ?? t.createdAt)}
              </span>
            </span>
            <span className={t.status === "consumed" ? styles.chipAccent : styles.chipWarn}>{STATUS_NAMES[t.status] ?? t.status}</span>
          </div>
        ))}
      </div>
    </>
  );
}

function PostsPanel({ data, notice }: { data: DashboardDto; notice: React.ReactNode }) {
  const { state } = useAppState();
  const identity = state.identity;
  const [posts, setPosts] = useState<eapi.PostDto[] | null>(null);
  const [text, setText] = useState("");
  const [teaser, setTeaser] = useState("");
  const [price, setPrice] = useState("5.00");
  const [file, setFile] = useState<File | null>(null);
  const [stage, setStage] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const stats = data.products.find((p) => p.product === "exclusive_post")!;

  const load = useCallback(() => {
    if (!identity) return;
    eapi.listCreatorPosts(identity.userId).then(setPosts, (err) => setError(describeError(err)));
  }, [identity]);
  useEffect(load, [load]);

  async function publish() {
    if (!identity) return;
    const cents = Math.round(Number(price) * 100);
    if (!text.trim() && !file) {
      setError("Write something or add a photo or video.");
      return;
    }
    if (!Number.isFinite(cents) || cents <= 0) {
      setError("Enter a price, like 5.00.");
      return;
    }
    setError(null);
    try {
      await publishPost(
        identity,
        { text, priceMinor: cents, teaser, media: file ? { blob: file, mime: file.type } : undefined },
        (s) => setStage(s === "encrypting" ? "Encrypting…" : "Uploading…")
      );
      setText("");
      setTeaser("");
      setFile(null);
      load();
    } catch (err) {
      setError(describeError(err, err instanceof Error ? err.message : undefined));
    } finally {
      setStage(null);
    }
  }

  return (
    <>
      {notice}
      <p className={styles.text}>
        Sell a post once to each buyer. It's encrypted with its own key on this device; the server stores only ciphertext. After a payment is confirmed, your app seals the key
        to the buyer (while it's open: keep it running now and then). A buyer who doesn't get the key within 72 hours is refunded automatically. RedVault keeps {stats.commissionPercent}%.
        Anyone who can see a post can still photograph their screen.
      </p>
      {data.awaitingKeys > 0 && <p className={styles.notice}>{data.awaitingKeys} buyer(s) are waiting for a key. They get it within a minute while this app is open.</p>}
      <div className={styles.form}>
        <label className={styles.field}>
          Post (encrypted)
          <textarea className={styles.textarea} rows={4} maxLength={10000} value={text} onChange={(e) => setText(e.target.value)} aria-label="Post text" />
        </label>
        {data.settings.available && (
          <label className={styles.field}>
            Photo or video (optional, encrypted)
            <input type="file" accept="image/jpeg,image/png,image/webp,video/mp4,video/webm,video/quicktime" onChange={(e) => setFile(e.target.files?.[0] ?? null)} aria-label="Photo or video" />
          </label>
        )}
        <label className={styles.field}>
          Public teaser (optional)
          <input className={styles.input} maxLength={280} value={teaser} onChange={(e) => setTeaser(e.target.value)} aria-label="Public teaser" />
          <span className={styles.muted}>Not encrypted: anyone who can see your posts can read it, and so can the server. Leave it empty to show only the price.</span>
        </label>
        <label className={styles.field}>
          Price (USD)
          <input className={styles.input} inputMode="decimal" value={price} onChange={(e) => setPrice(e.target.value)} aria-label="Price in US dollars" />
        </label>
        {error && <p className={styles.error}>{error}</p>}
        <div className={styles.actions}>
          <button type="button" className={styles.primaryButton} disabled={stage !== null || !data.available || !data.creatorReady} onClick={publish}>
            {stage ?? "Publish"}
          </button>
        </div>
      </div>
      <div className={styles.stats}>
        <Stat label="Sold" value={String(stats.count)} />
        <Stat label="Your earnings" value={formatUsd(stats.creatorNetMinor)} />
      </div>
      {posts?.length === 0 && <div className={styles.empty}>No posts yet.</div>}
      <div className={styles.list}>
        {posts?.map((p) => (
          <PostCard key={p.id} post={p} onChanged={load} />
        ))}
      </div>
    </>
  );
}

function TransactionsPanel({ data, onChange }: { data: DashboardDto; onChange: () => void }) {
  const [error, setError] = useState<string | null>(null);
  async function refund(p: PurchaseDto) {
    if (!window.confirm(`Refund ${formatUsd(p.amountMinor)}? The buyer gets it all back and your share is taken back.`)) return;
    try {
      await eapi.refundPurchase(p.id);
      onChange();
    } catch (err) {
      setError(describeError(err));
    }
  }
  return (
    <>
      <p className={styles.notice}>
        Each payment from Stripe, as confirmed by its webhook. RedVault's fee is taken from each payment; Stripe's processing fees are RedVault's to pay. Payouts to your bank
        follow Stripe's schedule (Payouts tab).
      </p>
      <div className={styles.stats}>
        <Stat label="Gross" value={formatUsd(data.totals.grossMinor)} />
        <Stat label="RedVault fees" value={formatUsd(data.totals.platformFeeMinor)} />
        <Stat label="Your earnings" value={formatUsd(data.totals.creatorNetMinor)} />
      </div>
      {error && <p className={styles.error}>{error}</p>}
      {data.transactions.length === 0 && <div className={styles.empty}>No payments yet.</div>}
      <div className={styles.tableWrap}>
        <table className={styles.table}>
          <thead>
            <tr>
              <th>Date</th>
              <th>Product</th>
              <th>From</th>
              <th className={styles.num}>Amount</th>
              <th className={styles.num}>RedVault fee</th>
              <th>Status</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {data.transactions.map((t) => (
              <tr key={t.id}>
                <td>{formatDate(t.paidAt ?? t.createdAt)}</td>
                <td>{PRODUCT_NAMES[t.product]}</td>
                <td>{t.buyer ? t.buyer.username : t.product === "tip" ? "A supporter" : "—"}</td>
                <td className={styles.num}>{formatUsd(t.amountMinor)}</td>
                <td className={styles.num}>{formatUsd(t.platformFeeMinor)}</td>
                <td>{STATUS_NAMES[t.status] ?? t.status}</td>
                <td>
                  {(t.status === "paid" || t.status === "consumed") && (
                    <button type="button" className={styles.small} onClick={() => refund(t)}>
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
