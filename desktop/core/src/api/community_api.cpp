// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/api/community_api.hpp"

#include <algorithm>

#include <nlohmann/json.hpp>

namespace redvault::api::community {

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

std::string communityPath(const std::string& communityId, const std::string& rest = {}) {
    return "communities/" + percentEncode(communityId) + rest;
}

std::string subscriptionPath(const std::string& subscriptionId, const std::string& rest = {}) {
    return "community-subscriptions/" + percentEncode(subscriptionId) + rest;
}

template <typename T>
Json nullable(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

std::vector<crypto::community::Member> membersFrom(const Json& list) {
    std::vector<crypto::community::Member> out;
    for (const auto& m : list) out.push_back({m.at("userId").get<std::string>(), m.at("publicKey").get<std::string>()});
    return out;
}

std::string urlIn(const Json& body) {
    return parse(body, [](const Json& j) { return j.at("url").get<std::string>(); });
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/// A UUID in its 8-4-4-4-12 form, either case (the web app's UUID pattern).
bool isId(std::string_view text) {
    if (text.size() != 36) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? text[i] != '-' : !isHex(text[i])) return false;
    }
    return true;
}

/// An invitation token as the server makes them (base64url), within what the link parser and the server accept.
bool isToken(std::string_view text) {
    if (text.size() < 10 || text.size() > 100) return false;
    for (const char c : text) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/// The address in a paste: the first http(s) link in it, or else the whole (trimmed) text.
std::string_view pastedUrl(std::string_view text) {
    for (std::size_t at = text.find("http"); at != std::string_view::npos; at = text.find("http", at + 1)) {
        const std::string_view rest = text.substr(at);
        const std::size_t scheme = rest.starts_with("https://") ? 8 : rest.starts_with("http://") ? 7 : 0;
        if (scheme == 0) continue;
        std::size_t end = scheme;
        while (end < rest.size() && !isSpace(rest[end])) ++end;
        if (end == scheme) continue;
        return rest.substr(0, end);
    }
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    return text;
}

/// application/x-www-form-urlencoded decoding, as URLSearchParams does it ('+' is a space, a bad escape stays as is).
std::string formDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < text.size() && isHex(text[i + 1]) && isHex(text[i + 2])) {
            out += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

/// URLSearchParams.get(name): the first parameter by that name in a query, decoded.
std::optional<std::string> queryParam(std::string_view query, std::string_view name) {
    for (std::size_t start = 0; start <= query.size();) {
        const std::size_t amp = query.find('&', start);
        const std::string_view pair = query.substr(start, amp == std::string_view::npos ? std::string_view::npos : amp - start);
        if (!pair.empty()) {
            const std::size_t eq = pair.find('=');
            if (formDecode(pair.substr(0, eq)) == name) return eq == std::string_view::npos ? std::string() : formDecode(pair.substr(eq + 1));
        }
        if (amp == std::string_view::npos) break;
        start = amp + 1;
    }
    return std::nullopt;
}

}  // namespace

// --- Key epochs ---

std::vector<KeyTask> keyTasks(ApiClient& api) {
    return parse(api.call("GET", "communities/key-tasks"), [](const Json& list) {
        std::vector<KeyTask> out;
        for (const auto& t : list) {
            out.push_back(KeyTask{t.at("communityId").get<std::string>(), t.at("conversationId").get<std::string>(),
                                  t.at("keyEpoch").get<std::int64_t>(), t.at("rekeyRequired").get<bool>(),
                                  t.at("pendingEnrollments").get<int>(), t.at("canPost").get<bool>()});
        }
        return out;
    });
}

crypto::community::KeyState keys(ApiClient& api, const std::string& communityId) {
    return parse(api.call("GET", communityPath(communityId, "/keys")), [](const Json& j) {
        crypto::community::KeyState state;
        state.conversationId = j.value("conversationId", std::string());
        state.keyEpoch = j.at("keyEpoch").get<std::int64_t>();
        state.rekeyRequired = j.at("rekeyRequired").get<bool>();
        for (const auto& k : j.at("keys")) state.keys.push_back({k.at("epoch").get<std::int64_t>(), k.at("wrappedKey").get<std::string>()});
        return state;
    });
}

std::vector<crypto::community::Member> pendingEnrollments(ApiClient& api, const std::string& communityId) {
    return parse(api.call("GET", communityPath(communityId, "/enrollments")), membersFrom);
}

void enroll(ApiClient& api, const std::string& communityId, const crypto::community::Enrollment& enrollment) {
    api.call("POST", communityPath(communityId, "/enrollments"),
             Json{{"userId", enrollment.userId}, {"epoch", enrollment.epoch}, {"wrappedKey", enrollment.wrappedKey}});
}

Roster roster(ApiClient& api, const std::string& communityId) {
    return parse(api.call("GET", communityPath(communityId, "/roster")), [](const Json& j) {
        return Roster{j.at("keyEpoch").get<std::int64_t>(), j.at("rekeyRequired").get<bool>(), membersFrom(j.at("members"))};
    });
}

std::int64_t rotate(ApiClient& api, const std::string& communityId, std::int64_t fromEpoch,
                    const std::vector<crypto::community::WrappedForMember>& keys) {
    Json list = Json::array();
    for (const auto& k : keys) list.push_back(Json{{"userId", k.userId}, {"wrappedKey", k.wrappedKey}});
    return parse(api.call("POST", communityPath(communityId, "/rotate"), Json{{"fromEpoch", fromEpoch}, {"keys", list}}),
                 [](const Json& j) { return j.at("keyEpoch").get<std::int64_t>(); });
}

// --- Config, creator profile, payouts ---

Json config(ApiClient& api) {
    return api.call("GET", "communities/config");
}

Json myCreator(ApiClient& api) {
    return api.call("GET", "communities/creator/me");
}

Json becomeCreator(ApiClient& api, const std::string& handle, const std::string& displayName, const std::string& bio,
                   const std::optional<std::string>& profileLink) {
    return api.call("POST", "communities/creator",
                    Json{{"handle", handle}, {"displayName", displayName}, {"bio", bio}, {"profileLink", nullable(profileLink)}, {"acceptTerms", true}});
}

Json updateCreator(ApiClient& api, const Json& changes) {
    return api.call("PATCH", "communities/creator/me", Json(changes));
}

std::string startOnboarding(ApiClient& api, const std::string& country) {
    return urlIn(api.call("POST", "communities/creator/me/onboarding", Json{{"country", country}}));
}

Json refreshOnboarding(ApiClient& api) {
    return api.call("POST", "communities/creator/me/onboarding/refresh");
}

std::string dashboardLink(ApiClient& api) {
    return urlIn(api.call("POST", "communities/creator/me/dashboard-link"));
}

Json creatorDashboard(ApiClient& api) {
    return api.call("GET", "communities/creator/me/dashboard");
}

// --- Discovery and communities ---

Json discover(ApiClient& api, const std::string& query, const std::string& category, int offset) {
    std::string params;
    const auto add = [&](const char* key, const std::string& value) {
        params += (params.empty() ? "?" : "&") + std::string(key) + "=" + percentEncode(value);
    };
    if (!query.empty()) add("q", query);
    if (!category.empty()) add("category", category);
    if (offset > 0) add("offset", std::to_string(offset));
    return api.call("GET", "communities/discover" + params);
}

Json mine(ApiClient& api) {
    return api.call("GET", "communities/mine");
}

Json create(ApiClient& api, const CreateInput& input) {
    return api.call("POST", "communities",
                    Json{{"id", input.id},
                         {"kind", input.kind},
                         {"name", input.name},
                         {"description", input.description},
                         {"category", input.category},
                         {"rules", input.rules},
                         {"previewText", input.previewText},
                         {"visibility", input.visibility},
                         {"accessType", input.accessType},
                         {"priceMinor", nullable(input.priceMinor)},
                         {"memberLimit", nullable(input.memberLimit)},
                         {"joinApproval", input.joinApproval},
                         {"showMemberCount", input.showMemberCount},
                         {"slowModeSeconds", input.slowModeSeconds},
                         {"wrappedKey", input.wrappedKey}});
}

Json get(ApiClient& api, const std::string& communityId, const std::string& invite) {
    return api.call("GET", communityPath(communityId, invite.empty() ? std::string() : "?invite=" + percentEncode(invite)));
}

Json update(ApiClient& api, const std::string& communityId, const Json& changes) {
    return api.call("PATCH", communityPath(communityId), Json(changes));
}

Json setPrice(ApiClient& api, const std::string& communityId, std::int64_t amountMinor) {
    return api.call("PUT", communityPath(communityId, "/price"), Json{{"amountMinor", amountMinor}, {"currency", "usd"}});
}

Json publish(ApiClient& api, const std::string& communityId) {
    return api.call("POST", communityPath(communityId, "/publish"));
}

Json unpublish(ApiClient& api, const std::string& communityId) {
    return api.call("POST", communityPath(communityId, "/unpublish"));
}

Json close(ApiClient& api, const std::string& communityId, const std::string& mode) {
    return api.call("POST", communityPath(communityId, "/close"), Json{{"mode", mode}});
}

// --- Membership ---

Json join(ApiClient& api, const std::string& communityId, const std::string& invite) {
    return api.call("POST", communityPath(communityId, "/join"), invite.empty() ? Json::object() : Json{{"inviteToken", invite}});
}

void leave(ApiClient& api, const std::string& communityId) {
    api.call("POST", communityPath(communityId, "/leave"));
}

Checkout checkout(ApiClient& api, const std::string& communityId, bool termsAccepted, bool ageConfirmed, const std::string& invite) {
    Json body{{"platform", kPlatform}, {"termsAccepted", termsAccepted}, {"ageConfirmed", ageConfirmed}};
    if (!invite.empty()) body["inviteToken"] = invite;
    return parse(api.call("POST", communityPath(communityId, "/checkout"), body), [](const Json& j) {
        return Checkout{j.at("subscriptionId").get<std::string>(), j.at("checkoutUrl").get<std::string>(), j.value("reused", false)};
    });
}

// --- Members and moderation ---

Json members(ApiClient& api, const std::string& communityId, const std::string& status, int offset) {
    std::string query = "?offset=" + std::to_string(offset) + "&limit=100";
    if (!status.empty()) query += "&status=" + percentEncode(status);
    return api.call("GET", communityPath(communityId, "/members" + query));
}

Json setMember(ApiClient& api, const std::string& communityId, const std::string& userId, const std::optional<std::string>& role,
               const std::optional<bool>& canPost) {
    Json body = Json::object();
    if (role) body["role"] = *role;
    if (canPost) body["canPost"] = *canPost;
    return api.call("PATCH", communityPath(communityId, "/members/" + percentEncode(userId)), body);
}

void removeMember(ApiClient& api, const std::string& communityId, const std::string& userId, bool ban,
                  const std::optional<std::string>& reason) {
    api.call("POST", communityPath(communityId, "/members/" + percentEncode(userId) + "/remove"), Json{{"ban", ban}, {"reason", nullable(reason)}});
}

void unban(ApiClient& api, const std::string& communityId, const std::string& userId) {
    api.call("POST", communityPath(communityId, "/members/" + percentEncode(userId) + "/unban"));
}

void decideJoinRequest(ApiClient& api, const std::string& communityId, const std::string& userId, bool approve) {
    api.call("POST", communityPath(communityId, "/requests/" + percentEncode(userId)), Json{{"approve", approve}});
}

Json invitations(ApiClient& api, const std::string& communityId) {
    return api.call("GET", communityPath(communityId, "/invitations"));
}

Json createInvitation(ApiClient& api, const std::string& communityId, std::optional<int> maxUses, std::optional<int> expiresInDays) {
    return api.call("POST", communityPath(communityId, "/invitations"), Json{{"maxUses", nullable(maxUses)}, {"expiresInDays", nullable(expiresInDays)}});
}

void revokeInvitation(ApiClient& api, const std::string& communityId, const std::string& invitationId) {
    api.call("DELETE", communityPath(communityId, "/invitations/" + percentEncode(invitationId)));
}

Json report(ApiClient& api, const std::string& communityId, const ReportInput& input) {
    Json body{{"targetType", input.targetType},
              {"targetUserId", nullable(input.targetUserId)},
              {"category", input.category},
              {"details", input.details}};
    // Shared only by the reporter's explicit choice, and then said so.
    if (input.evidenceMessageId && input.evidencePlaintext) {
        body["evidenceMessageId"] = *input.evidenceMessageId;
        body["evidencePlaintext"] = *input.evidencePlaintext;
        body["disclosureAcknowledged"] = true;
    }
    return api.call("POST", communityPath(communityId, "/reports"), body);
}

Json reports(ApiClient& api, const std::string& communityId) {
    return api.call("GET", communityPath(communityId, "/reports"));
}

void resolveReport(ApiClient& api, const std::string& communityId, const std::string& reportId, const std::string& status,
                   const std::optional<std::string>& note) {
    api.call("POST", communityPath(communityId, "/reports/" + percentEncode(reportId) + "/resolve"), Json{{"status", status}, {"note", nullable(note)}});
}

// --- The member's own subscriptions ---

Json memberships(ApiClient& api) {
    return api.call("GET", "community-subscriptions");
}

Json refreshMembership(ApiClient& api, const std::string& subscriptionId) {
    return api.call("POST", subscriptionPath(subscriptionId, "/refresh"));
}

Json cancelMembership(ApiClient& api, const std::string& subscriptionId) {
    return api.call("POST", subscriptionPath(subscriptionId, "/cancel"));
}

Json resumeMembership(ApiClient& api, const std::string& subscriptionId) {
    return api.call("POST", subscriptionPath(subscriptionId, "/resume"));
}

std::string membershipPortal(ApiClient& api, const std::string& subscriptionId) {
    return urlIn(api.call("POST", subscriptionPath(subscriptionId, "/portal")));
}

Json receipts(ApiClient& api, const std::string& subscriptionId) {
    return api.call("GET", subscriptionPath(subscriptionId, "/receipts"));
}

// --- Links and pictures ---

std::optional<Link> parseLink(std::string_view text) {
    const std::string_view url = pastedUrl(text);
    if (!url.starts_with("https://") && !url.starts_with("http://")) return std::nullopt;
    const std::string_view beforeHash = url.substr(0, url.find('#'));
    const std::size_t question = beforeHash.find('?');
    if (question == std::string_view::npos) return std::nullopt;
    const std::string_view query = beforeHash.substr(question + 1);
    const auto community = queryParam(query, "community");
    if (!community || !isId(*community)) return std::nullopt;
    const auto invite = queryParam(query, "invite");
    return Link{lower(*community), invite && isToken(*invite) ? *invite : std::string()};
}

std::string inviteUrl(std::string_view webAppBase, std::string_view communityId, std::string_view invite) {
    std::string base(webAppBase);
    if (base.empty() || base.back() != '/') base += '/';
    // Ids and tokens need no escaping in a query.
    return base + "?community=" + std::string(communityId) + "&invite=" + std::string(invite);
}

std::string imagePath(std::string_view communityId, std::string_view kind, std::string_view invite) {
    if (!isId(communityId) || (kind != "avatar" && kind != "banner") || (!invite.empty() && !isToken(invite))) return {};
    std::string path = "/api/communities/" + lower(communityId) + "/" + std::string(kind);
    if (!invite.empty()) path += "?invite=" + std::string(invite);
    return path;
}

std::string creatorAvatarPath(std::string_view creatorId) {
    if (!isId(creatorId)) return {};
    return "/api/communities/creators/" + lower(creatorId) + "/avatar";
}

// --- Amounts a person types ---

namespace {

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
    return text;
}

bool allDigits(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/// "<1..maxWhole digits>[.<1..2 digits>]" → hundredths.
std::optional<std::int64_t> hundredths(std::string_view text, std::size_t maxWhole) {
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    const std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() || whole.size() > maxWhole || !allDigits(whole)) return std::nullopt;
    if (dot != std::string_view::npos && (fraction.empty() || fraction.size() > 2 || !allDigits(fraction))) return std::nullopt;
    std::int64_t value = std::stoll(std::string(whole)) * 100;
    if (!fraction.empty()) value += std::stoll(std::string(fraction)) * (fraction.size() == 1 ? 10 : 1);
    return value;
}

}  // namespace

std::optional<std::int64_t> parseDollars(std::string_view text) {
    text = trimmed(text);
    if (text.starts_with('$')) text.remove_prefix(1);
    return hundredths(text, 6);
}

std::optional<int> parseRateBps(std::string_view text) {
    text = trimmed(text);
    if (text.ends_with('%')) text = trimmed(text.substr(0, text.size() - 1));
    const auto bps = hundredths(text, 2);
    return bps ? std::optional<int>(static_cast<int>(*bps)) : std::nullopt;
}

// --- RedVault administrators ---

namespace admin {

Json overview(ApiClient& api) {
    return api.call("GET", "admin/communities/overview");
}

Json transactions(ApiClient& api, const std::string& reconciliation) {
    return api.call("GET", "admin/communities/transactions" + (reconciliation.empty() ? std::string() : "?reconciliation=" + percentEncode(reconciliation)));
}

Json refund(ApiClient& api, const std::string& transactionId, std::optional<std::int64_t> amountMinor, const std::string& reason) {
    return api.call("POST", "admin/communities/transactions/" + percentEncode(transactionId) + "/refund",
                    Json{{"amountMinor", nullable(amountMinor)}, {"reason", reason}});
}

Json addCommissionRule(ApiClient& api, int rateBps, std::int64_t minPriceMinor, std::int64_t maxPriceMinor, const std::string& note) {
    return api.call("POST", "admin/communities/commission-rules",
                    Json{{"rateBps", rateBps}, {"minPriceMinor", minPriceMinor}, {"maxPriceMinor", maxPriceMinor}, {"note", note}});
}

Json creators(ApiClient& api) {
    return api.call("GET", "admin/communities/creators");
}

Json setCreator(ApiClient& api, const std::string& creatorId, const std::optional<std::string>& status, const std::optional<bool>& verified) {
    Json body = Json::object();
    if (status) body["status"] = *status;
    if (verified) body["verified"] = *verified;
    return api.call("PATCH", "admin/communities/creators/" + percentEncode(creatorId), body);
}

Json communities(ApiClient& api, const std::string& status) {
    return api.call("GET", "admin/communities/communities" + (status.empty() ? std::string() : "?status=" + percentEncode(status)));
}

Json communityAction(ApiClient& api, const std::string& communityId, const std::string& action) {
    if (action != "suspend" && action != "unsuspend" && action != "close") throw UserFacingError("Unknown community action.");
    return api.call("POST", "admin/communities/communities/" + percentEncode(communityId) + "/" + action);
}

Json reports(ApiClient& api, const std::string& status) {
    return api.call("GET", "admin/communities/reports" + (status.empty() ? std::string() : "?status=" + percentEncode(status)));
}

void resolveReport(ApiClient& api, const std::string& reportId, const std::string& status, const std::optional<std::string>& note) {
    api.call("POST", "admin/communities/reports/" + percentEncode(reportId) + "/resolve", Json{{"status", status}, {"note", nullable(note)}});
}

Json reconcile(ApiClient& api) {
    return api.call("POST", "admin/communities/reconcile");
}

}  // namespace admin

}  // namespace redvault::api::community
