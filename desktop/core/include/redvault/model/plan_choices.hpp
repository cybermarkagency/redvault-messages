// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "redvault/api/dtos.hpp"

// The plan chooser in Settings: Free, Pro and Premium side by side, what
// each includes and costs, and what choosing one does. Every action ends in
// a real change on the server: a Stripe checkout (whose webhook moves the
// account), or a call that changes the card subscription
// (server/src/services/billingService.ts).
namespace redvault::model::plans {

/// What pressing a plan's button does.
enum class PlanAction {
    None,      ///< No button: it's your plan, or it can't be chosen from the app.
    Checkout,  ///< Open the plan's Stripe payment page; Stripe's webhook then changes the plan.
    Switch,    ///< Move the card subscription to this plan now (POST /billing/switch).
    Cancel,    ///< Back to Free when the paid period ends (POST /billing/cancel).
    Keep,      ///< Undo a coming switch to Free (POST /billing/resume).
};

struct PlanChoice {
    std::string tier;  ///< "free", "pro" or "premium"
    std::string name;
    std::string price;  ///< "$0", "$8", "$15"
    std::string per;    ///< "per month", or empty for Free
    std::vector<std::string> includes;
    bool current = false;
    /// Under the price: "Your plan", "Your plan until 12 November 2026", "Your plan after …", or empty.
    std::string status;
    PlanAction action = PlanAction::None;
    /// The button's text; empty when there's no button.
    std::string actionLabel;
    /// What the button will do, to confirm before doing it; empty when nothing needs confirming.
    std::string confirm;
    /// Why there's no button, when it isn't simply your plan.
    std::string note;
};

struct PlanInputs {
    /// The account's plan: "free", "pro" or "premium".
    std::string tier = "free";
    /// Where a paid plan is headed; absent on Free, or until it's loaded.
    std::optional<api::PlanStatusDto> status;
    /// Whether the server has a Stripe payment link for each plan.
    bool proCheckout = false;
    bool premiumCheckout = false;
    /// Each plan's Files storage, when the server has Files.
    std::optional<api::FilesStorageDto> filesStorage;
};

inline constexpr const char* kPriceNote = "Prices in US dollars, excluding tax. Paid plans renew monthly until you switch to Free.";

/// Free, Pro and Premium, in that order.
std::vector<PlanChoice> choices(const PlanInputs& in);

/// "2026-11-12" as "12 November 2026"; anything else unchanged.
std::string longDate(const std::string& isoDay);

/// A plan's storage: whole gigabytes as "50 GB", anything else like the file sizes elsewhere.
std::string storageSize(std::int64_t bytes);

}  // namespace redvault::model::plans
