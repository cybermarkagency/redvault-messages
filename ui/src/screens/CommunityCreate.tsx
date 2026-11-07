import { useEffect, useState } from "react";
import * as communityApi from "../lib/communityApi.js";
import { type CommunitiesConfigDto, type CreatorDto, formatMoney, formatRate } from "../lib/communityApi.js";
import { newCommunityKey } from "../lib/communityCrypto.js";
import * as crypto from "../lib/e2eCrypto.js";
import { loadConversations } from "../state/actions.js";
import { useAppState } from "../state/AppStateContext.js";
import { CATEGORY_LABELS, describeError } from "./Communities.js";
import styles from "./Communities.module.css";

/** Dollars typed by a person → integer cents, or null if it isn't a plain amount. */
export function parseDollars(raw: string): number | null {
  const trimmed = raw.trim().replace(/^\$/, "");
  if (!/^\d{1,6}(\.\d{1,2})?$/.test(trimmed)) return null;
  const [whole, fraction = ""] = trimmed.split(".");
  return Number(whole) * 100 + Number(fraction.padEnd(2, "0"));
}

export function CommunityCreate({ config, onCreated, onNeedCreator }: { config: CommunitiesConfigDto | null; onCreated: (id: string) => void; onNeedCreator: () => void }) {
  const { state, dispatch } = useAppState();
  const [creator, setCreator] = useState<CreatorDto | null | undefined>(undefined);
  const [kind, setKind] = useState<"channel" | "group">("channel");
  const [accessType, setAccessType] = useState<"free" | "paid">("paid");
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [category, setCategory] = useState("education");
  const [visibility, setVisibility] = useState<"unlisted" | "discoverable">("unlisted");
  const [price, setPrice] = useState("10.00");
  const [rules, setRules] = useState("");
  const [previewText, setPreviewText] = useState("");
  const [memberLimit, setMemberLimit] = useState("");
  const [joinApproval, setJoinApproval] = useState(false);
  const [slowMode, setSlowMode] = useState("0");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    communityApi
      .getMyCreator()
      .then((r) => setCreator(r.creator))
      .catch(() => setCreator(null));
  }, []);

  if (creator === undefined) return <p className={styles.muted}>Loading…</p>;
  if (creator === null) {
    return (
      <div className={styles.panel}>
        <strong>Set up a creator profile first</strong>
        <span className={styles.muted}>Communities are published under a public creator handle, separate from your private RedVault username.</span>
        <div className={styles.actions}>
          <button type="button" className={styles.primaryButton} onClick={onNeedCreator}>
            Become a creator
          </button>
        </div>
      </div>
    );
  }

  const paidPossible = !!config?.paymentsEnabled && creator.paidMembershipsEnabled;
  const cents = accessType === "paid" ? parseDollars(price) : null;
  const priceProblem =
    accessType !== "paid"
      ? null
      : cents === null
        ? "Enter a monthly price like 10 or 9.99."
        : config && (cents < config.minPriceMinor || cents > config.maxPriceMinor)
          ? `Between ${formatMoney(config.minPriceMinor, config.currency)} and ${formatMoney(config.maxPriceMinor, config.currency)}.`
          : null;
  const commission = config && cents ? Math.floor((cents * config.commissionBps + 5000) / 10000) : null;

  async function create() {
    if (!state.identity) return;
    setBusy(true);
    setError(null);
    try {
      await crypto.ready();
      // The community's first key is made here, in this browser, sealed to you; the server never sees it.
      const id = window.crypto.randomUUID();
      const { wrappedKey } = newCommunityKey(id, state.identity.publicKey);
      const created = await communityApi.createCommunity({
        id,
        kind,
        name: name.trim(),
        description,
        category,
        rules,
        previewText,
        visibility,
        accessType,
        priceMinor: accessType === "paid" ? cents : null,
        memberLimit: memberLimit.trim() ? Number(memberLimit) : null,
        joinApproval: accessType === "free" ? joinApproval : false,
        showMemberCount: true,
        slowModeSeconds: Number(slowMode) || 0,
        wrappedKey,
      });
      await loadConversations(dispatch, state.identity);
      onCreated(created.id);
    } catch (err) {
      setError(describeError(err));
      setBusy(false);
    }
  }

  return (
    <div className={styles.form}>
      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>TYPE</span>
        <div className={styles.radioRow}>
          <button type="button" className={kind === "channel" ? styles.radioOn : styles.radio} aria-pressed={kind === "channel"} onClick={() => setKind("channel")}>
            <span className={styles.radioTitle}>Channel</span>
            You and your staff post; members read and receive.
          </button>
          <button type="button" className={kind === "group" ? styles.radioOn : styles.radio} aria-pressed={kind === "group"} onClick={() => setKind("group")}>
            <span className={styles.radioTitle}>Group</span>
            Members talk together, with moderators.
          </button>
        </div>
      </div>

      <div className={styles.section}>
        <span className={`${styles.sectionLabel} type-caption`}>ACCESS</span>
        <div className={styles.radioRow}>
          <button
            type="button"
            className={accessType === "paid" ? styles.radioOn : styles.radio}
            aria-pressed={accessType === "paid"}
            disabled={!paidPossible}
            onClick={() => setAccessType("paid")}
          >
            <span className={styles.radioTitle}>Paid, monthly</span>
            {paidPossible ? "Members subscribe through Stripe." : "Turn on paid memberships and set up payouts in the Creator dashboard first."}
          </button>
          <button type="button" className={accessType === "free" ? styles.radioOn : styles.radio} aria-pressed={accessType === "free"} onClick={() => setAccessType("free")}>
            <span className={styles.radioTitle}>Free</span>
            Anyone you let in joins at no cost.
          </button>
        </div>
      </div>

      <label className={styles.field}>
        Name
        <input className={styles.input} value={name} maxLength={80} onChange={(e) => setName(e.target.value)} />
      </label>
      <label className={styles.field}>
        Description (public)
        <textarea className={styles.textarea} value={description} maxLength={2000} onChange={(e) => setDescription(e.target.value)} />
      </label>
      <div className={styles.fieldRow}>
        <label className={styles.field}>
          Category
          <select className={styles.select} value={category} onChange={(e) => setCategory(e.target.value)}>
            {(config?.categories ?? Object.keys(CATEGORY_LABELS)).map((c) => (
              <option key={c} value={c}>
                {CATEGORY_LABELS[c] ?? c}
              </option>
            ))}
          </select>
        </label>
        <label className={styles.field}>
          Who can find it
          <select className={styles.select} value={visibility} onChange={(e) => setVisibility(e.target.value as "unlisted" | "discoverable")}>
            <option value="unlisted">Unlisted: only with an invite link</option>
            <option value="discoverable">Listed in Discover</option>
          </select>
        </label>
        {accessType === "paid" && (
          <label className={styles.field}>
            Monthly price (USD)
            <input className={styles.input} inputMode="decimal" value={price} onChange={(e) => setPrice(e.target.value)} />
          </label>
        )}
      </div>
      {accessType === "paid" && (
        <p className={priceProblem ? styles.error : styles.notice}>
          {priceProblem ??
            (config && cents && commission !== null
              ? `Of each ${formatMoney(cents, config.currency)} payment, RedVault keeps ${formatRate(config.commissionBps)} (${formatMoney(commission, config.currency)}) and you earn ${formatMoney(cents - commission, config.currency)}. RedVault pays Stripe's processing fees. Taxes, refunds and disputes can change what you receive; see the creator terms.`
              : null)}
        </p>
      )}
      <label className={styles.field}>
        Rules
        <textarea className={styles.textarea} value={rules} maxLength={4000} onChange={(e) => setRules(e.target.value)} />
      </label>
      <label className={styles.field}>
        Preview (public: what people see before joining)
        <textarea className={styles.textarea} value={previewText} maxLength={1000} onChange={(e) => setPreviewText(e.target.value)} />
      </label>
      <div className={styles.fieldRow}>
        <label className={styles.field}>
          Member limit (optional)
          <input className={styles.input} inputMode="numeric" value={memberLimit} onChange={(e) => setMemberLimit(e.target.value.replace(/\D/g, ""))} />
        </label>
        <label className={styles.field}>
          Slow mode
          <select className={styles.select} value={slowMode} onChange={(e) => setSlowMode(e.target.value)}>
            <option value="0">Off</option>
            <option value="10">10 seconds</option>
            <option value="60">1 minute</option>
            <option value="300">5 minutes</option>
          </select>
        </label>
      </div>
      {accessType === "free" && (
        <label className={styles.check}>
          <input type="checkbox" checked={joinApproval} onChange={(e) => setJoinApproval(e.target.checked)} />
          <span>A moderator approves each person who asks to join.</span>
        </label>
      )}
      <p className={styles.muted}>The name, description, rules, preview and price are public listing information RedVault can see. Everything posted inside is end-to-end encrypted.</p>
      {error && <p className={styles.error}>{error}</p>}
      <div className={styles.actions}>
        <button type="button" className={styles.primaryButton} disabled={busy || !name.trim() || !!priceProblem} onClick={create}>
          {busy ? "Creating…" : "Create as draft"}
        </button>
      </div>
    </div>
  );
}
