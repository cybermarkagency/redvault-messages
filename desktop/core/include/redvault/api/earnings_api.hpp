#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"

// Creator earnings (server/src/routes/earningRoutes.ts), mirroring
// ui/src/lib/earningsApi.ts: paid messages, tips and exclusive posts, plus
// the creator profile and Stripe payout setup they share with paid
// communities. Every price and fee comes from the server, and only the
// server (after the payment provider's verified webhook) says a purchase is
// paid: nothing here can. Posts are ciphertext. Free functions over
// ApiClient; all of them block.

namespace redvault::api::earnings {

/// The most a post's encrypted photo or video may be (the server's MAX_POST_MEDIA).
inline constexpr std::uint64_t kMaxPostMediaBytes = 200ull * 1024 * 1024;

struct Rule {
    double ratePercent = 0;
    std::int64_t minMinor = 0;
    std::int64_t maxMinor = 0;
};

struct Config {
    bool enabled = false;
    /// Where buying works ("web", "desktop"); never the mobile apps.
    std::vector<std::string> purchasePlatforms;
    std::string currency = "usd";
    std::vector<std::int64_t> tipPresetsMinor;
    bool postMedia = false;
    Rule paidMessage;
    Rule tip;
    Rule exclusivePost;
};

struct Settings {
    bool available = false;
    bool creatorReady = false;
    std::string paidMessagesMode = "off";  // "off" | "first_contact" | "per_message"
    std::optional<std::int64_t> paidMessagePriceMinor;
    bool tipsEnabled = false;
    bool acceptsMessageRequests = true;
    std::vector<std::string> exemptions;
};

struct SettingsPatch {
    std::optional<std::string> paidMessagesMode;
    /// Set with `clearPrice` false; `clearPrice` sends null (no price).
    std::optional<std::int64_t> paidMessagePriceMinor;
    bool clearPrice = false;
    std::optional<bool> tipsEnabled;
};

struct Offer {
    bool available = false;
    std::string creatorHandle;
    std::string creatorDisplayName;
    std::optional<std::string> paidMessagesMode;
    std::int64_t paidMessagePriceMinor = 0;
    bool tips = false;
    int posts = 0;
};

struct Quote {
    bool required = false;
    std::string reason;
    std::string mode;
    std::optional<std::int64_t> priceMinor;
    bool purchasable = false;
    std::optional<std::string> availablePurchaseId;
    bool safetyContactAvailable = false;
};

struct Purchase {
    std::string id;
    std::string product;  // "paid_message" | "tip" | "exclusive_post"
    std::string status;
    std::int64_t amountMinor = 0;
    std::string currency;
    double commissionPercent = 0;
    std::int64_t platformFeeMinor = 0;
    std::optional<std::string> creatorUserId;
    std::optional<std::string> postId;
    bool showName = true;
    /// The creator's view only, when the buyer let their name show.
    std::optional<std::string> buyerUsername;
    std::string createdAt;
    std::optional<std::string> paidAt;
    std::optional<std::string> refundedAt;
    std::optional<std::string> useBy;
    bool keyDelivered = false;
};

struct StartPurchase {
    std::string product;
    std::optional<std::string> creatorUserId;
    std::optional<std::string> postId;
    /// Tips only: the amount the person chose (the server checks it).
    std::optional<std::int64_t> amountMinor;
    std::optional<bool> showName;
    /// Fresh per purchase attempt, so a retried request never starts a second checkout.
    std::string idempotencyKey;
};

struct Started {
    std::string purchaseId;
    std::string status;
    /// Stripe's hosted payment page; absent when an unused purchase is already there (never charged twice).
    std::optional<std::string> checkoutUrl;
};

struct PostBodyRef {
    std::string ciphertext;
    std::string nonce;
    std::string wrappedKey;
};

struct Post {
    std::string id;
    std::string authorId;
    std::int64_t priceMinor = 0;
    std::string teaser;
    std::string status;  // "draft" | "published" | "unpublished" | "removed"
    std::string createdAt;
    bool hasMedia = false;
    std::optional<std::uint64_t> mediaSize;
    bool mine = false;
    bool owned = false;
    bool awaitingKey = false;
    bool locked = true;
    std::optional<PostBodyRef> body;
    std::string creatorHandle;
    std::string creatorDisplayName;
    std::optional<std::string> purchasedAt;
};

struct NewPost {
    std::string id;
    std::int64_t priceMinor = 0;
    std::string teaser;
    std::string bodyCiphertext;
    std::string bodyNonce;
    std::string wrappedKey;
    std::optional<std::uint64_t> mediaSize;
};

struct ProductStats {
    std::string product;
    double commissionPercent = 0;
    int count = 0;
    std::int64_t grossMinor = 0;
    std::int64_t platformFeeMinor = 0;
    std::int64_t creatorNetMinor = 0;
    std::int64_t refundedMinor = 0;
    int refunds = 0;
    int disputes = 0;
};

struct Money {
    std::string currency;
    std::int64_t amountMinor = 0;
};

struct Payout {
    std::string id;
    std::int64_t amountMinor = 0;
    std::string currency;
    std::string status;
    std::optional<std::string> arrivalDate;
};

struct Dashboard {
    bool available = false;
    bool creatorReady = false;
    std::vector<ProductStats> products;
    std::int64_t grossMinor = 0;
    std::int64_t platformFeeMinor = 0;
    std::int64_t creatorNetMinor = 0;
    std::vector<Purchase> transactions;
    int awaitingKeys = 0;
    bool hasBalance = false;
    std::vector<Money> balanceAvailable;
    std::vector<Money> balancePending;
    std::vector<Payout> payouts;
    Settings settings;
};

struct KeyRequest {
    std::string purchaseId;
    std::string postId;
    std::string buyerId;
    /// base64
    std::string buyerPublicKey;
};

/// The creator profile shared with paid communities (/api/communities/creator), and its Stripe payout setup.
struct Creator {
    std::string id;
    std::string handle;
    std::string displayName;
    std::string status;
    bool paidMembershipsEnabled = false;
    bool paymentsConfigured = false;
    std::string paymentsStatus;  // "not_started" | "pending" | "rejected" | …
    bool requirementsDue = false;
    bool paymentsReady = false;
};

struct CreatorSetup {
    bool communitiesEnabled = false;
    bool paymentsEnabled = false;
    std::vector<std::string> creatorCountries;
    std::string creatorTermsVersion;
};

Config config(ApiClient& api);
Settings settings(ApiClient& api);
Settings updateSettings(ApiClient& api, const SettingsPatch& patch);
void setExemption(ApiClient& api, const std::string& userId, bool exempt);
Offer offer(ApiClient& api, const std::string& userId);
Quote quote(ApiClient& api, const std::string& userId, const std::optional<std::string>& conversationId = std::nullopt);

/// Starts a purchase from this desktop app (platform "desktop").
Started startPurchase(ApiClient& api, const StartPurchase& input);
Purchase purchase(ApiClient& api, const std::string& purchaseId);
std::vector<Purchase> myPurchases(ApiClient& api);
/// The creator refunds one of their sales in full.
void refund(ApiClient& api, const std::string& purchaseId);
Dashboard dashboard(ApiClient& api);
std::vector<Purchase> tipsReceived(ApiClient& api);

void createPost(ApiClient& api, const NewPost& post);
/// The encrypted photo or video, exactly the announced size.
void uploadPostMedia(ApiClient& api, const std::string& postId, std::string ciphertext);
std::string downloadPostMedia(ApiClient& api, const std::string& postId);
Post post(ApiClient& api, const std::string& postId);
std::vector<Post> creatorPosts(ApiClient& api, const std::string& userId);
void unpublishPost(ApiClient& api, const std::string& postId);
std::vector<Post> library(ApiClient& api);
std::vector<KeyRequest> keyRequests(ApiClient& api);
void deliverPostKey(ApiClient& api, const std::string& postId, const std::string& purchaseId, const std::string& wrappedKey);

CreatorSetup creatorSetup(ApiClient& api);
/// Nullopt before a creator profile exists.
std::optional<Creator> myCreator(ApiClient& api);
Creator becomeCreator(ApiClient& api, const std::string& handle, const std::string& displayName);
Creator enablePayouts(ApiClient& api);
/// Stripe's hosted onboarding page (opened in the browser); RedVault never sees what's entered there.
std::string startPayoutSetup(ApiClient& api, const std::string& country);
Creator refreshPayoutSetup(ApiClient& api);
/// A one-time link to the creator's Stripe Express dashboard.
std::string stripeDashboardLink(ApiClient& api);

/// "p_" + 32 hex characters: one per purchase attempt.
std::string newIdempotencyKey();

}  // namespace redvault::api::earnings
