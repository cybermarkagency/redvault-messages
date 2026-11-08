// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "redvault/api/dtos.hpp"
#include "redvault/model/plan_choices.hpp"

using namespace redvault;
using namespace redvault::model::plans;

namespace {

api::PlanStatusDto status(const std::string& tier, const std::string& source) {
    api::PlanStatusDto s;
    s.tier = tier;
    s.source = source;
    return s;
}

/// A renewing card subscription that can move to `other` and be cancelled.
api::PlanStatusDto renewingCard(const std::string& tier, const std::string& other) {
    auto s = status(tier, "stripe");
    s.renews = true;
    s.canCancel = true;
    s.canSwitchTo = other;
    return s;
}

/// The same after "Switch to Free": ends on 12 November 2026.
api::PlanStatusDto endingCard(const std::string& tier, const std::string& other) {
    auto s = status(tier, "stripe");
    s.endsOn = "2026-11-12";
    s.canResume = true;
    s.canSwitchTo = other;
    return s;
}

const PlanChoice& plan(const std::vector<PlanChoice>& all, const std::string& tier) {
    for (const auto& c : all) {
        if (c.tier == tier) return c;
    }
    FAIL("no such plan");
    return all.front();
}

}  // namespace

TEST_CASE("the three plans, their prices and what each includes", "[plans]") {
    const auto all = choices({.tier = "free",
                              .proCheckout = true,
                              .premiumCheckout = true,
                              .filesStorage = api::FilesStorageDto{5LL << 30, 50LL << 30, 100LL << 30}});
    REQUIRE(all.size() == 3);
    CHECK(all[0].name == "Free");
    CHECK(all[1].name == "Pro");
    CHECK(all[2].name == "Premium");
    CHECK(all[0].price == "$0");
    CHECK(all[0].per.empty());
    CHECK(all[1].price == "$8");
    CHECK(all[2].price == "$15");
    CHECK(all[2].per == "per month");
    CHECK(all[0].includes.back() == "5 GB of Files storage");
    CHECK(all[1].includes.back() == "50 GB of Files storage");
    CHECK(all[2].includes.back() == "100 GB of Files storage");
    CHECK(all[2].includes.front() == "Everything in Pro");
}

TEST_CASE("on Free, a paid plan is chosen through Stripe's checkout", "[plans]") {
    const auto all = choices({.tier = "free", .proCheckout = true, .premiumCheckout = true});
    CHECK(plan(all, "free").current);
    CHECK(plan(all, "free").status == "Your plan");
    CHECK(plan(all, "free").action == PlanAction::None);
    CHECK(plan(all, "pro").action == PlanAction::Checkout);
    CHECK(plan(all, "pro").actionLabel == "Choose Pro");
    CHECK(plan(all, "premium").action == PlanAction::Checkout);
    CHECK(plan(all, "premium").confirm.find("payment page") != std::string::npos);
    // No Files on the server: no storage line.
    CHECK(plan(all, "free").includes.back() == "Password vault");

    // Without payment links there's nothing to press.
    const auto none = choices({.tier = "free"});
    CHECK(plan(none, "pro").action == PlanAction::None);
    CHECK(plan(none, "premium").note == "Not offered on this server yet.");
}

TEST_CASE("on a renewing card subscription, plans switch now and Free waits for the period's end", "[plans]") {
    const auto pro = choices({.tier = "pro", .status = renewingCard("pro", "premium"), .proCheckout = true, .premiumCheckout = true});
    CHECK(plan(pro, "pro").current);
    CHECK(plan(pro, "pro").status == "Your plan · renews monthly");
    CHECK(plan(pro, "pro").action == PlanAction::None);
    CHECK(plan(pro, "premium").action == PlanAction::Switch);
    CHECK(plan(pro, "premium").actionLabel == "Switch to Premium");
    CHECK(plan(pro, "premium").confirm.find("charged the difference") != std::string::npos);
    CHECK(plan(pro, "free").action == PlanAction::Cancel);
    CHECK(plan(pro, "free").actionLabel == "Switch to Free");
    CHECK(plan(pro, "free").confirm.find("You keep Pro until the end") != std::string::npos);

    const auto premium = choices({.tier = "premium", .status = renewingCard("premium", "pro")});
    CHECK(plan(premium, "pro").action == PlanAction::Switch);
    CHECK(plan(premium, "pro").confirm.find("credited") != std::string::npos);
    CHECK(plan(premium, "free").confirm.find("You keep Premium") != std::string::npos);
}

TEST_CASE("after Switch to Free, the plan can be kept, or another paid plan chosen", "[plans]") {
    const auto all = choices({.tier = "pro", .status = endingCard("pro", "premium")});
    CHECK(plan(all, "pro").status == "Your plan until 12 November 2026");
    CHECK(plan(all, "pro").action == PlanAction::Keep);
    CHECK(plan(all, "pro").actionLabel == "Keep Pro");
    CHECK(plan(all, "free").status == "Your plan after 12 November 2026");
    CHECK(plan(all, "free").action == PlanAction::None);
    // Choosing Premium keeps the subscription as well (the server does both).
    CHECK(plan(all, "premium").action == PlanAction::Switch);
    CHECK(plan(all, "premium").confirm.find("renews again") != std::string::npos);
}

TEST_CASE("prepaid, admin-given and not-yet-loaded plans", "[plans]") {
    auto crypto = status("pro", "crypto");
    crypto.endsOn = "2026-12-01";
    const auto prepaid = choices({.tier = "pro", .status = crypto, .premiumCheckout = true});
    CHECK(plan(prepaid, "pro").status == "Your plan until 1 December 2026 (prepaid)");
    CHECK(plan(prepaid, "free").status == "Your plan after 1 December 2026");
    CHECK(plan(prepaid, "free").action == PlanAction::None);
    CHECK(plan(prepaid, "premium").action == PlanAction::Checkout);

    const auto granted = choices({.tier = "premium", .status = status("premium", "granted"), .proCheckout = true});
    CHECK(plan(granted, "premium").status == "Your plan");
    CHECK(plan(granted, "free").note == "Your plan was set by an administrator.");
    CHECK(plan(granted, "pro").note == "Your plan was set by an administrator.");

    // A card subscription the server can't switch (no Stripe price ids set up).
    auto noPrices = renewingCard("pro", "premium");
    noPrices.canSwitchTo.reset();
    CHECK(plan(choices({.tier = "pro", .status = noPrices}), "premium").note == "This server can't change plans from the app.");

    // Until a paid plan's status loads, nothing is offered.
    for (const auto& c : choices({.tier = "premium", .proCheckout = true, .premiumCheckout = true})) {
        CHECK(c.action == PlanAction::None);
    }
}

TEST_CASE("dates and sizes read naturally", "[plans]") {
    CHECK(longDate("2026-11-12") == "12 November 2026");
    CHECK(longDate("2027-01-03") == "3 January 2027");
    CHECK(longDate("soon") == "soon");
    CHECK(storageSize(50LL << 30) == "50 GB");
    CHECK(storageSize(1536LL << 20) == "1.5 GB");
}

TEST_CASE("the billing config carries each plan's Files storage", "[plans]") {
    const auto config = api::Json::parse(R"({"paymentLink":"https://buy.stripe.com/x","premiumPaymentLink":null,
        "filesStorage":{"free":5368709120,"pro":53687091200,"premium":107374182400},"crypto":{"coins":[],"plans":[]}})")
                            .get<api::BillingConfigDto>();
    REQUIRE(config.filesStorage);
    CHECK(config.filesStorage->pro == 50LL << 30);
    CHECK_FALSE(config.premiumPaymentLink);
    const auto older = api::Json::parse(R"({"paymentLink":null})").get<api::BillingConfigDto>();
    CHECK_FALSE(older.filesStorage);
}
