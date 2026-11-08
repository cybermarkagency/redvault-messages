// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/model/plan_choices.hpp"

#include <array>
#include <regex>

#include "redvault/model/file_message.hpp"

namespace redvault::model::plans {

namespace {

constexpr std::int64_t kGiB = 1024LL * 1024 * 1024;

std::string nameOf(const std::string& tier) {
    if (tier == "premium") return "Premium";
    if (tier == "pro") return "Pro";
    return "Free";
}

PlanChoice base(const std::string& tier, const std::optional<api::FilesStorageDto>& files) {
    PlanChoice c;
    c.tier = tier;
    c.name = nameOf(tier);
    const auto storage = [&](std::int64_t bytes) { return storageSize(bytes) + " of Files storage"; };
    if (tier == "free") {
        c.price = "$0";
        c.includes = {"Unlimited end-to-end encrypted messages and groups", "One-to-one voice and video calls",
                      "Open files people send you", "Password vault"};
        if (files) c.includes.push_back(storage(files->free));
    } else if (tier == "pro") {
        c.price = "$8";
        c.per = "per month";
        c.includes = {"Everything in Free", "Send files in chats", "Voice call links"};
        if (files) c.includes.push_back(storage(files->pro));
    } else {
        c.price = "$15";
        c.per = "per month";
        c.includes = {"Everything in Pro", "Video call links and group voice and video calls", "Screen sharing in calls"};
        if (files) c.includes.push_back(storage(files->premium));
    }
    return c;
}

}  // namespace

std::string storageSize(std::int64_t bytes) {
    if (bytes > 0 && bytes % kGiB == 0) return std::to_string(bytes / kGiB) + " GB";
    return file_message::formatBytes(bytes);
}

std::string longDate(const std::string& isoDay) {
    static const std::regex kDay(R"((\d{4})-(\d{2})-(\d{2}))");
    static constexpr std::array<const char*, 12> kMonths = {"January", "February", "March",     "April",   "May",      "June",
                                                            "July",    "August",   "September", "October", "November", "December"};
    std::smatch m;
    if (!std::regex_match(isoDay, m, kDay)) return isoDay;
    const int month = std::stoi(m[2].str());
    const int day = std::stoi(m[3].str());
    if (month < 1 || month > 12 || day < 1 || day > 31) return isoDay;
    return std::to_string(day) + " " + kMonths[static_cast<std::size_t>(month - 1)] + " " + m[1].str();
}

std::vector<PlanChoice> choices(const PlanInputs& in) {
    const std::string tier = in.tier == "pro" || in.tier == "premium" ? in.tier : "free";
    const api::PlanStatusDto* s = in.status ? &*in.status : nullptr;
    const std::string source = s && s->source ? *s->source : "";
    const bool card = source == "stripe";
    // "Switch to Free" was chosen: the card subscription ends with the paid period.
    const bool ending = card && s->canResume;
    const std::string endsOn = s && s->endsOn ? longDate(*s->endsOn) : "";

    std::vector<PlanChoice> out;
    for (const char* t : {"free", "pro", "premium"}) {
        PlanChoice c = base(t, in.filesStorage);
        c.current = c.tier == tier;

        if (c.current) {
            if (tier != "free" && !endsOn.empty()) {
                c.status = "Your plan until " + endsOn + (source == "crypto" ? " (prepaid)" : "");
            } else if (tier != "free" && s && s->renews) {
                c.status = "Your plan · renews monthly";
            } else {
                c.status = "Your plan";
            }
            if (ending) {
                c.action = PlanAction::Keep;
                c.actionLabel = "Keep " + c.name;
            }
            out.push_back(std::move(c));
            continue;
        }

        if (c.tier == "free") {
            // You're on a paid plan. Until its status loads there's nothing to offer.
            if (!s) {
            } else if ((ending || source == "crypto") && !endsOn.empty()) {
                c.status = "Your plan after " + endsOn;
            } else if (card && s->canCancel) {
                c.action = PlanAction::Cancel;
                c.actionLabel = "Switch to Free";
                c.confirm = "Your subscription stops renewing and you won't be charged again. You keep " + nameOf(tier) +
                            " until the end of the period you've paid for, then move to Free: messages and calls, but no "
                            "sending files. Files you've already sent stay available until they expire.";
            } else if (source == "granted") {
                c.note = "Your plan was set by an administrator.";
            } else {
                c.note = "This server can't change plans from the app.";
            }
            out.push_back(std::move(c));
            continue;
        }

        // A paid plan you're not on.
        const bool checkout = c.tier == "pro" ? in.proCheckout : in.premiumCheckout;
        if (tier == "free") {
            if (checkout) {
                c.action = PlanAction::Checkout;
                c.actionLabel = "Choose " + c.name;
                c.confirm = "This opens Stripe's secure payment page in your browser. Your plan changes to " + c.name +
                            " here as soon as the payment goes through.";
            } else {
                c.note = "Not offered on this server yet.";
            }
        } else if (!s) {
            // Waiting for the paid plan's status.
        } else if (card) {
            if (s->canSwitchTo && *s->canSwitchTo == c.tier) {
                c.action = PlanAction::Switch;
                c.actionLabel = "Switch to " + c.name;
                c.confirm = c.tier == "premium"
                                ? "You're charged the difference for the rest of this billing period now, then $15 a month. "
                                  "Premium starts as soon as that payment goes through."
                                : "You move to Pro now, and the unused part of Premium is credited to your next payments. "
                                  "Then $8 a month.";
                if (ending) c.confirm += " Your subscription renews again: it no longer ends on " + endsOn + ".";
            } else {
                c.note = "This server can't change plans from the app.";
            }
        } else if (source == "crypto" && c.tier == "premium" && checkout) {
            c.action = PlanAction::Checkout;
            c.actionLabel = "Choose Premium";
            c.confirm = "This opens Stripe's secure payment page in your browser to start a Premium subscription. Your plan "
                        "changes to Premium here as soon as the payment goes through.";
        } else if (source == "granted") {
            c.note = "Your plan was set by an administrator.";
        } else {
            c.note = "Not offered on this server yet.";
        }
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace redvault::model::plans
