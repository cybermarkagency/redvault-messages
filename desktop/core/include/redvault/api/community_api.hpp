#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/community_crypto.hpp"

// Paid and free communities (server/src/routes/communityRoutes.ts, mounted at
// /api/communities and /api/community-subscriptions), mirroring
// ui/src/lib/communityApi.ts. Every amount comes from the server in integer
// minor units; this app never decides a price, a commission or whether a
// payment succeeded. Free functions over ApiClient::call; all of them block.
//
// The key-epoch calls are typed (they feed crypto/community_crypto.hpp).
// What the screens only show (listings, pages, dashboards, members, reports,
// memberships) comes back as the server's JSON, in the same shapes the web
// app's DTOs describe.

namespace redvault::api::community {

/// The purchase platform this app reports at checkout: the server lets "web" and "desktop" buy.
inline constexpr const char* kPlatform = "desktop";

/// One community this member's app should look after (GET /communities/key-tasks).
struct KeyTask {
    std::string communityId;
    std::string conversationId;
    std::int64_t keyEpoch = 0;
    bool rekeyRequired = false;
    int pendingEnrollments = 0;
    bool canPost = false;
};

struct Roster {
    std::int64_t keyEpoch = 0;
    bool rekeyRequired = false;
    std::vector<crypto::community::Member> members;
};

// --- Key epochs ---

std::vector<KeyTask> keyTasks(ApiClient& api);
/// Every epoch key this member was given (404 once they're out).
crypto::community::KeyState keys(ApiClient& api, const std::string& communityId);
/// Members who are entitled and waiting for the current key.
std::vector<crypto::community::Member> pendingEnrollments(ApiClient& api, const std::string& communityId);
/// Lets one waiting member in with their sealed copy of the current key. 409 when someone else did, or the epoch moved.
void enroll(ApiClient& api, const std::string& communityId, const crypto::community::Enrollment& enrollment);
/// Who is still in (members who can post only), for a rotation.
Roster roster(ApiClient& api, const std::string& communityId);
/**
 * Installs the next epoch, sealed to exactly the roster. 409 stale_key_epoch
 * when someone else rotated first, member_set_changed when the roster moved.
 */
std::int64_t rotate(ApiClient& api, const std::string& communityId, std::int64_t fromEpoch,
                    const std::vector<crypto::community::WrappedForMember>& keys);

// --- Config, creator profile, payouts ---

Json config(ApiClient& api);
/// {"creator": CreatorDto | null}
Json myCreator(ApiClient& api);
Json becomeCreator(ApiClient& api, const std::string& handle, const std::string& displayName, const std::string& bio,
                   const std::optional<std::string>& profileLink);
/// `changes`: any of displayName, bio, profileLink, avatar (base64 or null), paidMembershipsEnabled.
Json updateCreator(ApiClient& api, const Json& changes);
/// Stripe-hosted onboarding: the URL to open in the browser.
std::string startOnboarding(ApiClient& api, const std::string& country);
Json refreshOnboarding(ApiClient& api);
/// Stripe's Express dashboard: the URL to open in the browser.
std::string dashboardLink(ApiClient& api);
Json creatorDashboard(ApiClient& api);

// --- Discovery and communities ---

Json discover(ApiClient& api, const std::string& query, const std::string& category, int offset);
Json mine(ApiClient& api);

/// What creating a community sends. The id is chosen here, so epoch 0's key is bound to it.
struct CreateInput {
    std::string id;
    std::string kind;
    std::string name;
    std::string description;
    std::string category;
    std::string rules;
    std::string previewText;
    std::string visibility;
    std::string accessType;
    std::optional<std::int64_t> priceMinor;
    std::optional<int> memberLimit;
    bool joinApproval = false;
    bool showMemberCount = true;
    int slowModeSeconds = 0;
    /// Epoch 0's key, sealed to the creator (crypto::community::newCommunityKey).
    std::string wrappedKey;
};

Json create(ApiClient& api, const CreateInput& input);
/// The community's page as this user may see it; `invite` is an invitation token, if any.
Json get(ApiClient& api, const std::string& communityId, const std::string& invite = {});
/// Staff: `changes` as the web app's updateCommunity (with expectedVersion for optimistic locking).
Json update(ApiClient& api, const std::string& communityId, const Json& changes);
Json setPrice(ApiClient& api, const std::string& communityId, std::int64_t amountMinor);
Json publish(ApiClient& api, const std::string& communityId);
Json unpublish(ApiClient& api, const std::string& communityId);
/// `mode`: "end_of_period" or "immediately".
Json close(ApiClient& api, const std::string& communityId, const std::string& mode);

// --- Membership ---

Json join(ApiClient& api, const std::string& communityId, const std::string& invite = {});
void leave(ApiClient& api, const std::string& communityId);

struct Checkout {
    std::string subscriptionId;
    std::string checkoutUrl;
    bool reused = false;
};

/// Starts Stripe Checkout as the desktop app. Strict: no amount, price or commission is sent.
Checkout checkout(ApiClient& api, const std::string& communityId, bool termsAccepted, bool ageConfirmed,
                  const std::string& invite = {});

// --- Members and moderation ---

Json members(ApiClient& api, const std::string& communityId, const std::string& status = {}, int offset = 0);
Json setMember(ApiClient& api, const std::string& communityId, const std::string& userId, const std::optional<std::string>& role,
               const std::optional<bool>& canPost);
void removeMember(ApiClient& api, const std::string& communityId, const std::string& userId, bool ban,
                  const std::optional<std::string>& reason);
void unban(ApiClient& api, const std::string& communityId, const std::string& userId);
void decideJoinRequest(ApiClient& api, const std::string& communityId, const std::string& userId, bool approve);
Json invitations(ApiClient& api, const std::string& communityId);
/// The token is in the answer once; only its hash is kept.
Json createInvitation(ApiClient& api, const std::string& communityId, std::optional<int> maxUses, std::optional<int> expiresInDays);
void revokeInvitation(ApiClient& api, const std::string& communityId, const std::string& invitationId);

/**
 * A report. Nothing is read for you: for a message, `evidencePlaintext` is
 * set only when the reporter chose to share its text (and then
 * disclosureAcknowledged is sent too).
 */
struct ReportInput {
    /// "community", "creator", "user" or "content".
    std::string targetType;
    std::optional<std::string> targetUserId;
    std::string category;
    std::string details;
    std::optional<std::string> evidenceMessageId;
    std::optional<std::string> evidencePlaintext;
};

/// {"id", "platformReview"}
Json report(ApiClient& api, const std::string& communityId, const ReportInput& input);
Json reports(ApiClient& api, const std::string& communityId);
void resolveReport(ApiClient& api, const std::string& communityId, const std::string& reportId, const std::string& status,
                   const std::optional<std::string>& note);

// --- The member's own subscriptions ---

Json memberships(ApiClient& api);
/// Asks the server, which asks Stripe: how a checkout or a renewal went.
Json refreshMembership(ApiClient& api, const std::string& subscriptionId);
Json cancelMembership(ApiClient& api, const std::string& subscriptionId);
Json resumeMembership(ApiClient& api, const std::string& subscriptionId);
/// Stripe's customer portal (payment method): the URL to open in the browser.
std::string membershipPortal(ApiClient& api, const std::string& subscriptionId);
Json receipts(ApiClient& api, const std::string& subscriptionId);

// --- Links and pictures ---

/// What a community link points at: the web app's <site>/?community=<id>&invite=<token>.
struct Link {
    /// In lowercase.
    std::string communityId;
    /// Empty when the link has none (or one that can't be a token: it's dropped, as the web app does).
    std::string invite;

    bool operator==(const Link&) const = default;
};

/**
 * A community link in a paste or a clicked address, read as the web app's
 * parseCommunityDeepLink: the first http(s) address in `text` (else all of
 * it), whose `community` parameter is a community id; `invite` is kept only
 * if it looks like a token (10 to 100 base64url characters). nullopt for
 * anything else.
 */
std::optional<Link> parseLink(std::string_view text);

/// The shareable invite link Manage hands out: <web app base>?community=<id>&invite=<token>.
std::string inviteUrl(std::string_view webAppBase, std::string_view communityId, std::string_view invite);

/**
 * Where a community's picture ("avatar") or banner ("banner") is served, as
 * the web app's communityImageUrl: "/api/communities/<id>/<kind>", with
 * "?invite=<token>" for a community seen through an invite link. Empty when
 * `communityId` isn't an id, `kind` isn't one of the two, or `invite` isn't a
 * token, so only paths ApiClient::fetchCommunityImage accepts come out.
 */
std::string imagePath(std::string_view communityId, std::string_view kind, std::string_view invite = {});
/// "/api/communities/creators/<id>/avatar", or empty when `creatorId` isn't an id.
std::string creatorAvatarPath(std::string_view creatorId);

// --- Amounts a person types (the server checks every one against its own rules) ---

/// "10", "9.99", "$4.5" → cents, as the web app's parseDollars; nullopt for anything else (at most 999999.99).
std::optional<std::int64_t> parseDollars(std::string_view text);
/// A commission in percent ("12", "12.5", "12.25%") → basis points; nullopt for anything else (at most 99.99%).
std::optional<int> parseRateBps(std::string_view text);

// --- RedVault administrators (/api/admin/communities; the server lets only super admins in and audits every change) ---

namespace admin {

/// Ledger totals, the commission rule versions (newest first) and how many transactions don't match Stripe.
Json overview(ApiClient& api);
/// `reconciliation`: "" (all), "pending", "matched" or "mismatch".
Json transactions(ApiClient& api, const std::string& reconciliation = {});
/**
 * Refunds a payment through Stripe: `amountMinor` in cents, or nullopt for
 * all of it. The server checks the amount against what's left to refund and
 * takes the creator's share back; `reason` (1–200 characters) goes in the
 * audit log.
 */
Json refund(ApiClient& api, const std::string& transactionId, std::optional<std::int64_t> amountMinor, const std::string& reason);
/// A new commission version for memberships started from now on (the server enforces 10–15% and sane price limits).
Json addCommissionRule(ApiClient& api, int rateBps, std::int64_t minPriceMinor, std::int64_t maxPriceMinor, const std::string& note);
Json creators(ApiClient& api);
/// `status`: "active" or "suspended" (suspending a creator suspends their communities and pauses billing).
Json setCreator(ApiClient& api, const std::string& creatorId, const std::optional<std::string>& status, const std::optional<bool>& verified);
/// `status`: "" (all) or a community status.
Json communities(ApiClient& api, const std::string& status = {});
/// `action`: "suspend", "unsuspend" or "close" (immediately). Anything else throws without asking the server.
Json communityAction(ApiClient& api, const std::string& communityId, const std::string& action);
/// `status`: "" (all), "open", "actioned" or "dismissed".
Json reports(ApiClient& api, const std::string& status = "open");
/// `status`: "actioned" or "dismissed".
void resolveReport(ApiClient& api, const std::string& reportId, const std::string& status, const std::optional<std::string>& note);
/// Checks recorded transactions against Stripe now: {"checked", "mismatches", "unbalancedJournals"}.
Json reconcile(ApiClient& api);

}  // namespace admin

}  // namespace redvault::api::community
