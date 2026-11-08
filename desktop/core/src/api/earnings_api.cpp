// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/api/earnings_api.hpp"

#include <sodium.h>

#include <nlohmann/json.hpp>

#include "redvault/crypto/bytes.hpp"

namespace redvault::api::earnings {

namespace {

using net::percentEncode;

template <typename F>
auto parse(const Json& j, F&& f) -> decltype(f(j)) {
    try {
        return f(j);
    } catch (const Json::exception& e) {
        throw ApiError(0, "malformed_response", std::string("Unexpected response: ") + e.what());
    }
}

std::optional<std::string> optString(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

std::optional<std::int64_t> optInt(const Json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<std::int64_t>();
}

Rule ruleFrom(const Json& rules, const char* product) {
    Rule r;
    const auto it = rules.find(product);
    if (it == rules.end() || !it->is_object()) return r;
    r.ratePercent = it->value("ratePercent", 0.0);
    r.minMinor = it->value("minMinor", std::int64_t{0});
    r.maxMinor = it->value("maxMinor", std::int64_t{0});
    return r;
}

Settings settingsFrom(const Json& j) {
    Settings s;
    s.available = j.value("available", false);
    s.creatorReady = j.value("creatorReady", false);
    s.paidMessagesMode = j.value("paidMessagesMode", std::string("off"));
    s.paidMessagePriceMinor = optInt(j, "paidMessagePriceMinor");
    s.tipsEnabled = j.value("tipsEnabled", false);
    s.acceptsMessageRequests = j.value("acceptsMessageRequests", true);
    if (const auto it = j.find("exemptions"); it != j.end() && it->is_array()) {
        for (const auto& id : *it) {
            if (id.is_string()) s.exemptions.push_back(id.get<std::string>());
        }
    }
    return s;
}

Purchase purchaseFrom(const Json& j) {
    Purchase p;
    p.id = j.at("id").get<std::string>();
    p.product = j.at("product").get<std::string>();
    p.status = j.at("status").get<std::string>();
    p.amountMinor = j.at("amountMinor").get<std::int64_t>();
    p.currency = j.value("currency", std::string("usd"));
    p.commissionPercent = j.value("commissionPercent", 0.0);
    p.platformFeeMinor = j.value("platformFeeMinor", std::int64_t{0});
    p.creatorUserId = optString(j, "creatorUserId");
    p.postId = optString(j, "postId");
    p.showName = j.value("showName", true);
    if (const auto it = j.find("buyer"); it != j.end() && it->is_object()) p.buyerUsername = optString(*it, "username");
    p.createdAt = j.value("createdAt", std::string());
    p.paidAt = optString(j, "paidAt");
    p.refundedAt = optString(j, "refundedAt");
    p.useBy = optString(j, "useBy");
    p.keyDelivered = j.value("keyDelivered", false);
    return p;
}

std::vector<Purchase> purchaseList(const Json& list) {
    std::vector<Purchase> out;
    for (const auto& p : list) out.push_back(purchaseFrom(p));
    return out;
}

Post postFrom(const Json& j) {
    Post p;
    p.id = j.at("id").get<std::string>();
    p.authorId = j.at("authorId").get<std::string>();
    p.priceMinor = j.at("priceMinor").get<std::int64_t>();
    p.teaser = j.value("teaser", std::string());
    p.status = j.value("status", std::string());
    p.createdAt = j.value("createdAt", std::string());
    p.hasMedia = j.value("hasMedia", false);
    if (const auto size = optInt(j, "mediaSize"); size && *size > 0) p.mediaSize = static_cast<std::uint64_t>(*size);
    p.mine = j.value("mine", false);
    p.owned = j.value("owned", false);
    p.awaitingKey = j.value("awaitingKey", false);
    p.locked = j.value("locked", true);
    if (const auto it = j.find("body"); it != j.end() && it->is_object()) {
        p.body = PostBodyRef{it->at("ciphertext").get<std::string>(), it->at("nonce").get<std::string>(), it->at("wrappedKey").get<std::string>()};
    }
    if (const auto it = j.find("creator"); it != j.end() && it->is_object()) {
        p.creatorHandle = it->value("handle", std::string());
        p.creatorDisplayName = it->value("displayName", std::string());
    }
    p.purchasedAt = optString(j, "purchasedAt");
    return p;
}

std::vector<Post> postList(const Json& list) {
    std::vector<Post> out;
    for (const auto& p : list) out.push_back(postFrom(p));
    return out;
}

std::vector<Money> moneyList(const Json& list) {
    std::vector<Money> out;
    if (!list.is_array()) return out;
    for (const auto& m : list) out.push_back({m.value("currency", std::string("usd")), m.value("amountMinor", std::int64_t{0})});
    return out;
}

Creator creatorFrom(const Json& c) {
    Creator out;
    out.id = c.at("id").get<std::string>();
    out.handle = c.value("handle", std::string());
    out.displayName = c.value("displayName", std::string());
    out.status = c.value("status", std::string());
    out.paidMembershipsEnabled = c.value("paidMembershipsEnabled", false);
    if (const auto it = c.find("payments"); it != c.end() && it->is_object()) {
        out.paymentsConfigured = it->value("configured", false);
        out.paymentsStatus = it->value("status", std::string("not_started"));
        out.requirementsDue = it->value("requirementsDue", false);
        out.paymentsReady = it->value("ready", false);
    }
    return out;
}

std::string postPath(const std::string& id, const char* rest = "") {
    return "earnings/posts/" + percentEncode(id) + rest;
}

}  // namespace

Config config(ApiClient& api) {
    return parse(api.call("GET", "earnings/config"), [](const Json& j) {
        Config c;
        c.enabled = j.value("enabled", false);
        if (const auto it = j.find("purchasePlatforms"); it != j.end() && it->is_array()) c.purchasePlatforms = it->get<std::vector<std::string>>();
        c.currency = j.value("currency", std::string("usd"));
        if (const auto it = j.find("tipPresetsMinor"); it != j.end() && it->is_array()) c.tipPresetsMinor = it->get<std::vector<std::int64_t>>();
        c.postMedia = j.value("postMedia", false);
        if (const auto it = j.find("rules"); it != j.end() && it->is_object()) {
            c.paidMessage = ruleFrom(*it, "paid_message");
            c.tip = ruleFrom(*it, "tip");
            c.exclusivePost = ruleFrom(*it, "exclusive_post");
        }
        return c;
    });
}

Settings settings(ApiClient& api) {
    return parse(api.call("GET", "earnings/settings"), settingsFrom);
}

Settings updateSettings(ApiClient& api, const SettingsPatch& patch) {
    Json body = Json::object();
    if (patch.paidMessagesMode) body["paidMessagesMode"] = *patch.paidMessagesMode;
    if (patch.clearPrice) {
        body["paidMessagePriceMinor"] = nullptr;
    } else if (patch.paidMessagePriceMinor) {
        body["paidMessagePriceMinor"] = *patch.paidMessagePriceMinor;
    }
    if (patch.tipsEnabled) body["tipsEnabled"] = *patch.tipsEnabled;
    return parse(api.call("PUT", "earnings/settings", body), settingsFrom);
}

void setExemption(ApiClient& api, const std::string& userId, bool exempt) {
    api.call(exempt ? "PUT" : "DELETE", "earnings/exemptions/" + percentEncode(userId));
}

Offer offer(ApiClient& api, const std::string& userId) {
    return parse(api.call("GET", "earnings/creators/" + percentEncode(userId)), [](const Json& j) {
        Offer o;
        o.available = j.value("available", false);
        if (const auto it = j.find("creator"); it != j.end() && it->is_object()) {
            o.creatorHandle = it->value("handle", std::string());
            o.creatorDisplayName = it->value("displayName", std::string());
        }
        if (const auto it = j.find("paidMessages"); it != j.end() && it->is_object()) {
            o.paidMessagesMode = it->value("mode", std::string());
            o.paidMessagePriceMinor = it->value("priceMinor", std::int64_t{0});
        }
        o.tips = j.value("tips", false);
        o.posts = j.value("posts", 0);
        return o;
    });
}

Quote quote(ApiClient& api, const std::string& userId, const std::optional<std::string>& conversationId) {
    std::string path = "earnings/quote/" + percentEncode(userId);
    if (conversationId) path += "?conversationId=" + percentEncode(*conversationId);
    return parse(api.call("GET", path), [](const Json& j) {
        Quote q;
        q.required = j.value("required", false);
        q.reason = j.value("reason", std::string());
        q.mode = j.value("mode", std::string("off"));
        q.priceMinor = optInt(j, "priceMinor");
        q.purchasable = j.value("purchasable", false);
        q.availablePurchaseId = optString(j, "availablePurchaseId");
        q.safetyContactAvailable = j.value("safetyContactAvailable", false);
        return q;
    });
}

Started startPurchase(ApiClient& api, const StartPurchase& input) {
    Json body{{"product", input.product}, {"idempotencyKey", input.idempotencyKey}, {"platform", "desktop"}};
    if (input.creatorUserId) body["creatorUserId"] = *input.creatorUserId;
    if (input.postId) body["postId"] = *input.postId;
    if (input.amountMinor) body["amountMinor"] = *input.amountMinor;
    if (input.showName) body["showName"] = *input.showName;
    return parse(api.call("POST", "earnings/purchases", body), [](const Json& j) {
        return Started{j.at("purchaseId").get<std::string>(), j.at("status").get<std::string>(), optString(j, "checkoutUrl")};
    });
}

Purchase purchase(ApiClient& api, const std::string& purchaseId) {
    return parse(api.call("GET", "earnings/purchases/" + percentEncode(purchaseId)), purchaseFrom);
}

std::vector<Purchase> myPurchases(ApiClient& api) {
    return parse(api.call("GET", "earnings/purchases"), purchaseList);
}

void refund(ApiClient& api, const std::string& purchaseId) {
    api.call("POST", "earnings/purchases/" + percentEncode(purchaseId) + "/refund");
}

Dashboard dashboard(ApiClient& api) {
    return parse(api.call("GET", "earnings/dashboard"), [](const Json& j) {
        Dashboard d;
        d.available = j.value("available", false);
        d.creatorReady = j.value("creatorReady", false);
        for (const auto& p : j.at("products")) {
            d.products.push_back({p.at("product").get<std::string>(), p.value("commissionPercent", 0.0), p.value("count", 0),
                                  p.value("grossMinor", std::int64_t{0}), p.value("platformFeeMinor", std::int64_t{0}),
                                  p.value("creatorNetMinor", std::int64_t{0}), p.value("refundedMinor", std::int64_t{0}), p.value("refunds", 0),
                                  p.value("disputes", 0)});
        }
        const Json& totals = j.at("totals");
        d.grossMinor = totals.value("grossMinor", std::int64_t{0});
        d.platformFeeMinor = totals.value("platformFeeMinor", std::int64_t{0});
        d.creatorNetMinor = totals.value("creatorNetMinor", std::int64_t{0});
        d.transactions = purchaseList(j.at("transactions"));
        d.awaitingKeys = j.value("awaitingKeys", 0);
        if (const auto it = j.find("payouts"); it != j.end() && it->is_object()) {
            if (const auto b = it->find("balance"); b != it->end() && b->is_object()) {
                d.hasBalance = true;
                d.balanceAvailable = moneyList(b->value("available", Json::array()));
                d.balancePending = moneyList(b->value("pending", Json::array()));
            }
            if (const auto r = it->find("recent"); r != it->end() && r->is_array()) {
                for (const auto& p : *r) {
                    d.payouts.push_back({p.value("id", std::string()), p.value("amountMinor", std::int64_t{0}), p.value("currency", std::string("usd")),
                                         p.value("status", std::string()), optString(p, "arrivalDate")});
                }
            }
        }
        if (const auto it = j.find("settings"); it != j.end() && it->is_object()) d.settings = settingsFrom(*it);
        return d;
    });
}

std::vector<Purchase> tipsReceived(ApiClient& api) {
    return parse(api.call("GET", "earnings/tips"), purchaseList);
}

void createPost(ApiClient& api, const NewPost& post) {
    Json body{{"id", post.id},
              {"priceMinor", post.priceMinor},
              {"teaser", post.teaser},
              {"bodyCiphertext", post.bodyCiphertext},
              {"bodyNonce", post.bodyNonce},
              {"wrappedKey", post.wrappedKey},
              {"mediaSize", post.mediaSize ? Json(*post.mediaSize) : Json(nullptr)}};
    api.call("POST", "earnings/posts", body);
}

void uploadPostMedia(ApiClient& api, const std::string& postId, std::string ciphertext) {
    api.callRaw("PUT", postPath(postId, "/media"), std::move(ciphertext), "application/octet-stream");
}

std::string downloadPostMedia(ApiClient& api, const std::string& postId) {
    return api.callRaw("GET", postPath(postId, "/media"));
}

Post post(ApiClient& api, const std::string& postId) {
    return parse(api.call("GET", postPath(postId)), postFrom);
}

std::vector<Post> creatorPosts(ApiClient& api, const std::string& userId) {
    return parse(api.call("GET", "earnings/creators/" + percentEncode(userId) + "/posts"), postList);
}

void unpublishPost(ApiClient& api, const std::string& postId) {
    api.call("POST", postPath(postId, "/unpublish"));
}

std::vector<Post> library(ApiClient& api) {
    return parse(api.call("GET", "earnings/library"), postList);
}

std::vector<KeyRequest> keyRequests(ApiClient& api) {
    return parse(api.call("GET", "earnings/key-requests"), [](const Json& list) {
        std::vector<KeyRequest> out;
        for (const auto& r : list) {
            out.push_back({r.at("purchaseId").get<std::string>(), r.at("postId").get<std::string>(), r.at("buyerId").get<std::string>(),
                           r.at("buyerPublicKey").get<std::string>()});
        }
        return out;
    });
}

void deliverPostKey(ApiClient& api, const std::string& postId, const std::string& purchaseId, const std::string& wrappedKey) {
    api.call("POST", postPath(postId, "/keys"), Json{{"purchaseId", purchaseId}, {"wrappedKey", wrappedKey}});
}

CreatorSetup creatorSetup(ApiClient& api) {
    return parse(api.call("GET", "communities/config"), [](const Json& j) {
        CreatorSetup s;
        s.communitiesEnabled = j.value("enabled", false);
        s.paymentsEnabled = j.value("paymentsEnabled", false);
        if (const auto it = j.find("creatorCountries"); it != j.end() && it->is_array()) s.creatorCountries = it->get<std::vector<std::string>>();
        s.creatorTermsVersion = j.value("creatorTermsVersion", std::string());
        return s;
    });
}

std::optional<Creator> myCreator(ApiClient& api) {
    return parse(api.call("GET", "communities/creator/me"), [](const Json& j) -> std::optional<Creator> {
        const auto it = j.find("creator");
        if (it == j.end() || !it->is_object()) return std::nullopt;
        return creatorFrom(*it);
    });
}

Creator becomeCreator(ApiClient& api, const std::string& handle, const std::string& displayName) {
    Json body{{"handle", handle}, {"displayName", displayName}, {"bio", ""}, {"profileLink", nullptr}, {"acceptTerms", true}};
    return parse(api.call("POST", "communities/creator", body), [](const Json& j) { return creatorFrom(j.at("creator")); });
}

Creator enablePayouts(ApiClient& api) {
    return parse(api.call("PATCH", "communities/creator/me", Json{{"paidMembershipsEnabled", true}}),
                 [](const Json& j) { return creatorFrom(j.at("creator")); });
}

std::string startPayoutSetup(ApiClient& api, const std::string& country) {
    return parse(api.call("POST", "communities/creator/me/onboarding", Json{{"country", country}}),
                 [](const Json& j) { return j.at("url").get<std::string>(); });
}

Creator refreshPayoutSetup(ApiClient& api) {
    return parse(api.call("POST", "communities/creator/me/onboarding/refresh"), [](const Json& j) { return creatorFrom(j.at("creator")); });
}

std::string stripeDashboardLink(ApiClient& api) {
    return parse(api.call("POST", "communities/creator/me/dashboard-link"), [](const Json& j) { return j.at("url").get<std::string>(); });
}

std::string newIdempotencyKey() {
    crypto::ensureSodium();
    unsigned char bytes[16];
    randombytes_buf(bytes, sizeof bytes);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "p_";
    for (const unsigned char b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 0x0f];
    }
    return out;
}

}  // namespace redvault::api::earnings
