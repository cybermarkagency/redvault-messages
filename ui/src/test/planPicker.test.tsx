// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

import { describe, expect, it } from "vitest";
import type { PlanStatus } from "../lib/api.js";
import { formatDay, type PlanInputs, planChoices } from "../lib/planChoices.js";

const GiB = 1024 * 1024 * 1024;
const card = (overrides: Partial<PlanStatus> = {}): PlanStatus => ({
  tier: "pro",
  source: "stripe",
  renews: true,
  endsOn: null,
  canCancel: true,
  canResume: false,
  canSwitchTo: "premium",
  ...overrides,
});
const inputs = (overrides: Partial<PlanInputs> = {}): PlanInputs => ({
  tier: "free",
  status: null,
  proCheckout: true,
  premiumCheckout: true,
  filesStorage: null,
  ...overrides,
});
/** Each plan's [status, action, label, note], Free first. */
const summary = (input: PlanInputs) => planChoices(input).map((c) => [c.status, c.action, c.actionLabel, c.note]);

describe("planChoices", () => {
  it("offers Free accounts both paid plans, with prices and what each includes", () => {
    const choices = planChoices(inputs({ filesStorage: { free: 5 * GiB, pro: 50 * GiB, premium: 100 * GiB } }));
    expect(choices.map((c) => [c.name, c.price, c.per, c.current])).toEqual([
      ["Free", "$0", "", true],
      ["Pro", "$8", "per month", false],
      ["Premium", "$15", "per month", false],
    ]);
    expect(choices[1].includes).toContain("50 GB of Files storage");
    expect(summary(inputs())).toEqual([
      ["Your plan", "none", "", ""],
      ["", "checkout", "Choose Pro", ""],
      ["", "checkout", "Choose Premium", ""],
    ]);
  });

  it("says when a plan isn't offered because its payment link isn't set", () => {
    expect(summary(inputs({ premiumCheckout: false }))[2]).toEqual(["", "none", "", "Not offered on this server yet."]);
  });

  it("lets a card subscriber switch between Pro and Premium, or to Free, asking first", () => {
    const choices = planChoices(inputs({ tier: "pro", status: card() }));
    expect(choices.map((c) => [c.status, c.action, c.actionLabel])).toEqual([
      ["", "cancel", "Switch to Free"],
      ["Your plan · renews monthly", "none", ""],
      ["", "switch", "Switch to Premium"],
    ]);
    expect(choices[0].confirm).toMatch(/you keep pro until the end of the period/i);
    expect(choices[2].confirm).toMatch(/charged the difference/i);
    expect(summary(inputs({ tier: "premium", status: card({ tier: "premium", canSwitchTo: "pro" }) }))[1]).toEqual(["", "switch", "Switch to Pro", ""]);
  });

  it("after Switch to Free: the plan's last day, Keep, and a switch that keeps the subscription", () => {
    const status = card({ renews: false, endsOn: "2026-11-12", canCancel: false, canResume: true });
    const choices = planChoices(inputs({ tier: "pro", status }));
    const day = formatDay("2026-11-12");
    expect(choices.map((c) => [c.status, c.action, c.actionLabel])).toEqual([
      [`Your plan after ${day}`, "none", ""],
      [`Your plan until ${day}`, "keep", "Keep Pro"],
      ["", "switch", "Switch to Premium"],
    ]);
    expect(choices[2].confirm).toContain(`it no longer ends on ${day}`);
  });

  it("explains plans this app can't change", () => {
    const granted = card({ tier: "premium", source: "granted", renews: false, canCancel: false, canSwitchTo: null });
    expect(summary(inputs({ tier: "premium", status: granted })).map((s) => s[3])).toEqual([
      "Your plan was set by an administrator.",
      "Your plan was set by an administrator.",
      "",
    ]);
    const noPrices = card({ canSwitchTo: null });
    expect(summary(inputs({ tier: "pro", status: noPrices }))[2][3]).toBe("This server can't change plans from the app.");
  });

  it("crypto-prepaid Pro: its last day, and a card subscription to Premium", () => {
    const status = card({ source: "crypto", renews: false, endsOn: "2026-11-12", canCancel: false, canSwitchTo: null });
    const day = formatDay("2026-11-12");
    expect(summary(inputs({ tier: "pro", status }))).toEqual([
      [`Your plan after ${day}`, "none", "", ""],
      [`Your plan until ${day} (prepaid)`, "none", "", ""],
      ["", "checkout", "Choose Premium", ""],
    ]);
  });

  it("offers nothing on a paid plan until its status has loaded", () => {
    expect(summary(inputs({ tier: "pro", status: null }))).toEqual([
      ["", "none", "", ""],
      ["Your plan", "none", "", ""],
      ["", "none", "", ""],
    ]);
  });
});
