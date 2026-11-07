// The plan picker in Settings: Free, Pro and Premium side by side, what each
// includes and costs, and what choosing one does. Every action ends in a
// real change on the server: a Stripe checkout (whose webhook moves the
// account), or a call that changes the card subscription
// (server/src/services/billingService.ts). The same rules as the desktop
// app's desktop/core/src/model/plan_choices.cpp, so both offer the same choices.

import { formatBytes } from "./fileMessage.js";
import type { PlanStatus } from "./api.js";
import { type SubscriptionTier, TIER_LABELS } from "../types.js";

/** What pressing a plan's button does. */
export type PlanAction =
  /** No button: it's your plan, or it can't be chosen here. */
  | "none"
  /** Open the plan's Stripe payment page; Stripe's webhook then changes the plan. */
  | "checkout"
  /** Move the card subscription to this plan now (POST /billing/switch). */
  | "switch"
  /** Back to Free when the paid period ends (POST /billing/cancel). */
  | "cancel"
  /** Undo a coming switch to Free (POST /billing/resume). */
  | "keep";

export interface PlanChoice {
  tier: SubscriptionTier;
  name: string;
  /** "$0", "$8", "$15" */
  price: string;
  /** "per month", or empty for Free. */
  per: string;
  includes: string[];
  current: boolean;
  /** Under the price: "Your plan", "Your plan until 12 November 2026", "Your plan after …", or empty. */
  status: string;
  action: PlanAction;
  /** The button's text; empty when there's no button. */
  actionLabel: string;
  /** What the button will do, to confirm before doing it; empty when nothing needs confirming. */
  confirm: string;
  /** Why there's no button, when it isn't simply your plan. */
  note: string;
}

/** Each plan's Files storage, from GET /billing/config. */
export interface FilesStorageLimits {
  free: number;
  pro: number;
  premium: number;
}

export interface PlanInputs {
  tier: SubscriptionTier;
  /** Where the plan is headed; null until it's loaded (or on an older server). */
  status: PlanStatus | null;
  /** Whether the server has a Stripe payment link for each plan. */
  proCheckout: boolean;
  premiumCheckout: boolean;
  filesStorage: FilesStorageLimits | null;
}

export const PRICE_NOTE = "Prices in US dollars, excluding tax. Paid plans renew monthly until you switch to Free.";

const GiB = 1024 * 1024 * 1024;

/** A plan's storage: whole gigabytes as "50 GB", anything else like the file sizes elsewhere. */
export function storageSize(bytes: number): string {
  return bytes > 0 && bytes % GiB === 0 ? `${bytes / GiB} GB` : formatBytes(bytes);
}

/** "2026-10-26" in the reader's own date format, without a time zone shifting the day. */
export function formatDay(day: string): string {
  const [y, m, d] = day.split("-").map(Number);
  return new Date(y, m - 1, d).toLocaleDateString(undefined, { day: "numeric", month: "long", year: "numeric" });
}

function base(tier: SubscriptionTier, files: FilesStorageLimits | null): PlanChoice {
  const storage = (bytes: number) => `${storageSize(bytes)} of Files storage`;
  const plan = {
    free: {
      price: "$0",
      per: "",
      includes: ["Unlimited end-to-end encrypted messages and groups", "One-to-one voice and video calls", "Open files people send you", "Password vault"],
    },
    pro: { price: "$8", per: "per month", includes: ["Everything in Free", "Send files in chats", "Voice call links"] },
    premium: {
      price: "$15",
      per: "per month",
      includes: ["Everything in Pro", "Video call links and group voice and video calls", "Screen sharing in calls"],
    },
  }[tier];
  return {
    tier,
    name: TIER_LABELS[tier],
    price: plan.price,
    per: plan.per,
    includes: files ? [...plan.includes, storage(files[tier])] : plan.includes,
    current: false,
    status: "",
    action: "none",
    actionLabel: "",
    confirm: "",
    note: "",
  };
}

/** Free, Pro and Premium, in that order. */
export function planChoices(input: PlanInputs): PlanChoice[] {
  const tier = input.tier;
  const s = input.status;
  const source = s?.source ?? null;
  const card = source === "stripe";
  // "Switch to Free" was chosen: the card subscription ends with the paid period.
  const ending = card && !!s?.canResume;
  const endsOn = s?.endsOn ? formatDay(s.endsOn) : "";

  return (["free", "pro", "premium"] as const).map((t) => {
    const c = base(t, input.filesStorage);
    c.current = t === tier;

    if (c.current) {
      if (tier !== "free" && endsOn) c.status = `Your plan until ${endsOn}${source === "crypto" ? " (prepaid)" : ""}`;
      else if (tier !== "free" && s?.renews) c.status = "Your plan · renews monthly";
      else c.status = "Your plan";
      if (ending) {
        c.action = "keep";
        c.actionLabel = `Keep ${c.name}`;
      }
      return c;
    }

    if (t === "free") {
      // You're on a paid plan. Until its status loads there's nothing to offer.
      if (!s) {
        // nothing yet
      } else if ((ending || source === "crypto") && endsOn) {
        c.status = `Your plan after ${endsOn}`;
      } else if (card && s.canCancel) {
        c.action = "cancel";
        c.actionLabel = "Switch to Free";
        c.confirm =
          `Your subscription stops renewing and you won't be charged again. You keep ${TIER_LABELS[tier]} until the end of the ` +
          "period you've paid for, then move to Free: messages and calls, but no sending files. Files you've already sent stay " +
          "available until they expire.";
      } else if (source === "granted") {
        c.note = "Your plan was set by an administrator.";
      } else {
        c.note = "This server can't change plans from the app.";
      }
      return c;
    }

    // A paid plan you're not on.
    const checkout = t === "pro" ? input.proCheckout : input.premiumCheckout;
    if (tier === "free") {
      if (checkout) {
        c.action = "checkout";
        c.actionLabel = `Choose ${c.name}`;
      } else {
        c.note = "Not offered on this server yet.";
      }
    } else if (!s) {
      // Waiting for the paid plan's status.
    } else if (card) {
      if (s.canSwitchTo === t) {
        c.action = "switch";
        c.actionLabel = `Switch to ${c.name}`;
        c.confirm =
          t === "premium"
            ? "You're charged the difference for the rest of this billing period now, then $15 a month. Premium starts as soon as that payment goes through."
            : "You move to Pro now, and the unused part of Premium is credited to your next payments. Then $8 a month.";
        if (ending) c.confirm += ` Your subscription renews again: it no longer ends on ${endsOn}.`;
      } else {
        c.note = "This server can't change plans from the app.";
      }
    } else if (source === "crypto" && t === "premium" && checkout) {
      c.action = "checkout";
      c.actionLabel = "Choose Premium";
    } else if (source === "granted") {
      c.note = "Your plan was set by an administrator.";
    } else {
      c.note = "Not offered on this server yet.";
    }
    return c;
  });
}
