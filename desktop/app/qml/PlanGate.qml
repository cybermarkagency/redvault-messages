// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

pragma Singleton
import QtQuick

// One place for "this is part of a paid plan". A screen that finds the user's
// plan doesn't include a feature calls PlanGate.ask(tier, feature); the shell
// shows the upgrade dialog, whose "See plans" opens Settings > Account and plan.
// Paid buttons show a lock (IconButton/RvButton `locked`) instead of looking
// available and refusing only after a click.
QtObject {
    /// tier: "pro" or "premium"; feature: what was tapped, as a short phrase
    /// ("Sending files", "Screen sharing").
    signal upgradeRequested(string tier, string feature)
    signal plansRequested()

    function ask(tier, feature) { upgradeRequested(tier, feature) }
    function openPlans() { plansRequested() }

    function tierName(tier) { return tier === "premium" ? "Premium" : "Pro" }
}
