// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Paid messages, tips and exclusive posts: the post crypto and body format
// are the web app's byte for byte (vectors made by its own code:
// vectors/make_post_vectors.ts), amounts read and show the same, and the API
// calls send what the server expects (never a price or a "paid" claim).
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/dtos.hpp"
#include "redvault/api/earnings_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/post_crypto.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/model/earnings.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using namespace redvault::crypto;
using Json = nlohmann::json;
namespace em = redvault::model::earnings;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_POST_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

Bytes b64(const Json& j) {
    return base64Decode(j.get<std::string>());
}

Bytes bytesOf(const SecretBytes& key) {
    return Bytes(key.view().begin(), key.view().end());
}

}  // namespace

TEST_CASE("a post key sealed by the web app opens here, for that post and that buyer only", "[earnings]") {
    const auto& v = vectors();
    const std::string id = v["postId"];
    const SecretBytes key = posts::unwrapPostKey(id, b64(v["wrappedForBob"]), b64(v["alice"]["publicKey"]), b64(v["bob"]["privateKey"]));
    CHECK(bytesOf(key) == b64(v["key"]));
    // The author's own copy opens with their own keys.
    CHECK(bytesOf(posts::unwrapPostKey(id, b64(v["wrappedForAlice"]), b64(v["alice"]["publicKey"]), b64(v["alice"]["privateKey"]))) == b64(v["key"]));
    // Another post, another sender, or the wrong reader: refused.
    CHECK_THROWS_AS(posts::unwrapPostKey(util::randomUuid(), b64(v["wrappedForBob"]), b64(v["alice"]["publicKey"]), b64(v["bob"]["privateKey"])),
                    CryptoError);
    CHECK_THROWS_AS(posts::unwrapPostKey(id, b64(v["wrappedForBob"]), b64(v["bob"]["publicKey"]), b64(v["bob"]["privateKey"])), CryptoError);
    CHECK_THROWS_AS(posts::unwrapPostKey(id, b64(v["wrappedForBob"]), b64(v["alice"]["publicKey"]), b64(v["alice"]["privateKey"])), CryptoError);
}

TEST_CASE("a story key can't pass for a post key, or the other way round", "[earnings]") {
    const auto& v = vectors();
    const std::string id = v["postId"];
    const Bytes key = b64(v["key"]);
    const Bytes storyWrapped = stories::wrapStoryKey(id, key, b64(v["bob"]["publicKey"]), b64(v["alice"]["privateKey"]));
    CHECK_THROWS_AS(posts::unwrapPostKey(id, storyWrapped, b64(v["alice"]["publicKey"]), b64(v["bob"]["privateKey"])), CryptoError);
    CHECK_THROWS_AS(stories::unwrapStoryKey(id, b64(v["wrappedForBob"]), b64(v["alice"]["publicKey"]), b64(v["bob"]["privateKey"])), CryptoError);
}

TEST_CASE("the web app's post bodies open and parse the same here", "[earnings]") {
    const auto& v = vectors();
    const std::string id = v["postId"];
    const Bytes key = b64(v["key"]);

    const std::string text = posts::openBody(id, key, b64(v["textBody"]["ciphertext"]), b64(v["textBody"]["nonce"]));
    CHECK(text == v["textBody"]["json"].get<std::string>());
    const em::PostBody textBody = em::parseBody(text);
    CHECK(textBody.text == Json::parse(v["textBody"]["json"].get<std::string>())["text"].get<std::string>());
    CHECK_FALSE(textBody.media);

    const std::string media = posts::openBody(id, key, b64(v["mediaBody"]["ciphertext"]), b64(v["mediaBody"]["nonce"]));
    const em::PostBody mediaBody = em::parseBody(media);
    REQUIRE(mediaBody.media);
    CHECK(mediaBody.media->mime == "video/mp4");
    CHECK(mediaBody.media->encryptedSize == 65553);
    CHECK(mediaBody.media->chunkSize == 65536);
    // What this app writes is what the web app wrote.
    CHECK(em::toJson(mediaBody) == v["mediaBody"]["json"].get<std::string>());
    CHECK(em::toJson(textBody) == v["textBody"]["json"].get<std::string>());

    // Bound to the post: another id, or a flipped byte, doesn't open.
    CHECK_THROWS_AS(posts::openBody(util::randomUuid(), key, b64(v["textBody"]["ciphertext"]), b64(v["textBody"]["nonce"])), CryptoError);
    Bytes tampered = b64(v["textBody"]["ciphertext"]);
    tampered[3] ^= 1;
    CHECK_THROWS_AS(posts::openBody(id, key, tampered, b64(v["textBody"]["nonce"])), CryptoError);
}

TEST_CASE("a post this app seals opens with the same calls (round trip)", "[earnings]") {
    const auto alice = generateKeyPair();
    const auto bob = generateKeyPair();
    const std::string id = util::randomUuid();
    const SecretBytes key = posts::newPostKey();
    const Bytes wrapped = posts::wrapPostKey(id, key.view(), bob.publicKey, alice.privateKey.view());
    CHECK(wrapped.size() == posts::kWrappedKeyBytes);
    CHECK(bytesOf(posts::unwrapPostKey(id, wrapped, alice.publicKey, bob.privateKey.view())) == bytesOf(key));
    const auto sealed = posts::sealBody(id, key.view(), em::toJson({"hello", std::nullopt}));
    CHECK(em::parseBody(posts::openBody(id, key.view(), sealed.ciphertext, sealed.nonce)).text == "hello");
}

TEST_CASE("post bodies someone else wrote are checked like the web app does", "[earnings]") {
    CHECK_THROWS_AS(em::parseBody("not json"), em::PostFormatError);
    CHECK_THROWS_AS(em::parseBody(R"({"v":2,"text":"x"})"), em::PostFormatError);
    CHECK_THROWS_AS(em::parseBody(R"({"v":1})"), em::PostFormatError);
    CHECK_THROWS_AS(em::parseBody(Json{{"v", 1}, {"text", std::string(10'001, 'a')}}.dump()), em::PostFormatError);
    CHECK(em::parseBody(Json{{"v", 1}, {"text", std::string(10'000, 'a')}}.dump()).text.size() == 10'000);
    const Json media{{"key", "a2V5"}, {"header", "aGVhZGVy"}, {"sha256", std::string(64, 'c')}, {"encryptedSize", 10}, {"chunkSize", 65536}, {"mime", "image/png"}};
    CHECK(em::parseBody(Json{{"v", 1}, {"text", ""}, {"media", media}}.dump()).media->mime == "image/png");
    for (const auto& [field, bad] : std::vector<std::pair<std::string, Json>>{
             {"mime", "application/pdf"}, {"sha256", "XYZ"}, {"encryptedSize", 0}, {"chunkSize", -1}, {"key", ""}, {"header", std::string(65, 'a')}}) {
        Json m = media;
        m[field] = bad;
        CHECK_THROWS_AS(em::parseBody(Json{{"v", 1}, {"text", ""}, {"media", m}}.dump()), em::PostFormatError);
    }
}

TEST_CASE("amounts read and show like the web app's", "[earnings]") {
    CHECK(em::formatUsd(300) == "$3.00");
    CHECK(em::formatUsd(5) == "$0.05");
    CHECK(em::formatUsd(123456) == "$1234.56");
    CHECK(em::parseUsd("3") == 300);
    CHECK(em::parseUsd(" $7.5 ") == 750);
    CHECK(em::parseUsd("0.99") == 99);
    CHECK(em::parseUsd("10.") == 1000);
    CHECK_FALSE(em::parseUsd("1.234"));
    CHECK_FALSE(em::parseUsd("-3"));
    CHECK_FALSE(em::parseUsd("three"));
    CHECK_FALSE(em::parseUsd(""));
    CHECK(em::statusName("consumed") == "Delivered");
    CHECK(em::productName("exclusive_post") == "Exclusive posts");
}

TEST_CASE("a paid send carries the paid message or the safety contact, nothing else", "[earnings]") {
    using api::SendMessageRequest;
    CHECK(Json(SendMessageRequest{.ciphertext = "c", .nonce = "n"}).dump() == R"({"ciphertext":"c","nonce":"n"})");
    CHECK(Json(SendMessageRequest{.ciphertext = "c", .nonce = "n", .entitlementId = "p1"}).dump() == R"({"ciphertext":"c","entitlementId":"p1","nonce":"n"})");
    CHECK(Json(SendMessageRequest{.ciphertext = "c", .nonce = "n", .safetyContact = true}).dump() == R"({"ciphertext":"c","nonce":"n","safetyContact":true})");
}

TEST_CASE("the earnings API sends what the server expects", "[earnings]") {
    auto transport = std::make_shared<test::FakeTransport>();
    auto cookies = std::make_shared<net::SessionCookieJar>();
    api::ApiClient api{"https://example.com/api/", transport, cookies};
    const std::string creator = "00000000-0000-4000-8000-00000000000b";

    // Buying: the product and who from, a fresh idempotency key, and this platform. No price for a paid message.
    transport->enqueue(201, R"({"purchaseId":"p1","status":"checkout_pending","checkoutUrl":"https://checkout.stripe.com/c/pay/x"})");
    const auto key = api::earnings::newIdempotencyKey();
    CHECK(key.size() == 34);
    CHECK(key != api::earnings::newIdempotencyKey());
    const auto started = api::earnings::startPurchase(api, {.product = "paid_message", .creatorUserId = creator, .idempotencyKey = key});
    CHECK(started.purchaseId == "p1");
    CHECK(*started.checkoutUrl == "https://checkout.stripe.com/c/pay/x");
    CHECK(transport->last().url == "https://example.com/api/earnings/purchases");
    const Json sent = Json::parse(transport->last().body);
    CHECK(sent == Json{{"product", "paid_message"}, {"creatorUserId", creator}, {"idempotencyKey", key}, {"platform", "desktop"}});

    // A tip carries the amount the person chose (the server checks it) and whether to show their name.
    transport->enqueue(201, R"({"purchaseId":"p2","status":"checkout_pending","checkoutUrl":"https://checkout.stripe.com/c/pay/y"})");
    api::earnings::startPurchase(api, {.product = "tip", .creatorUserId = creator, .amountMinor = 500, .showName = false, .idempotencyKey = key});
    CHECK(Json::parse(transport->last().body)["amountMinor"] == 500);
    CHECK(Json::parse(transport->last().body)["showName"] == false);

    // An unused paid message already there: no checkout, nothing charged twice.
    transport->enqueue(200, R"({"purchaseId":"p1","status":"paid","checkoutUrl":null})");
    CHECK_FALSE(api::earnings::startPurchase(api, {.product = "paid_message", .creatorUserId = creator, .idempotencyKey = key}).checkoutUrl);

    transport->enqueue(200, R"({"required":true,"reason":"paid_messages","mode":"per_message","priceMinor":300,"currency":"usd","purchasable":true,"availablePurchaseId":null,"safetyContactAvailable":true})");
    const auto q = api::earnings::quote(api, creator, std::string("c-1"));
    CHECK(transport->last().url == "https://example.com/api/earnings/quote/" + creator + "?conversationId=c-1");
    CHECK(q.required);
    CHECK(*q.priceMinor == 300);
    CHECK_FALSE(q.availablePurchaseId);
    CHECK(q.safetyContactAvailable);

    transport->enqueue(200, R"({"id":"p1","product":"paid_message","status":"paid","amountMinor":300,"currency":"usd","commissionPercent":10,"platformFeeMinor":30,
                               "creatorUserId":"c","postId":null,"paidMessageMode":"per_message","showName":true,"buyer":{"userId":"u","username":"sam"},
                               "createdAt":"t","paidAt":"t2","refundedAt":null,"useBy":"t3","keyDelivered":false})");
    const auto p = api::earnings::purchase(api, "p1");
    CHECK(p.status == "paid");
    CHECK(p.platformFeeMinor == 30);
    CHECK(*p.buyerUsername == "sam");
    CHECK_FALSE(p.postId);

    // Settings: clearing the price sends null.
    transport->enqueue(200, R"({"available":true,"creatorReady":true,"paidMessagesMode":"off","paidMessagePriceMinor":null,"tipsEnabled":true,"acceptsMessageRequests":true,"exemptions":["x"]})");
    const auto s = api::earnings::updateSettings(api, {.paidMessagesMode = std::string("off"), .clearPrice = true});
    CHECK(Json::parse(transport->last().body) == Json{{"paidMessagesMode", "off"}, {"paidMessagePriceMinor", nullptr}});
    CHECK(s.exemptions == std::vector<std::string>{"x"});
    CHECK_FALSE(s.paidMessagePriceMinor);

    transport->enqueue(204, "");
    api::earnings::setExemption(api, creator, false);
    CHECK(transport->last().method == "DELETE");
    CHECK(transport->last().url == "https://example.com/api/earnings/exemptions/" + creator);

    // Posts: ciphertext only, and media as raw bytes.
    transport->enqueue(201, R"({"id":"x","status":"draft"})");
    api::earnings::createPost(api, {"7a3e9c1d-4b2f-4e6a-8d5c-0f1e2d3c4b5a", 500, "teaser", "Y3Q=", "bm9uY2U=", "a2V5", 2048});
    const Json post = Json::parse(transport->last().body);
    CHECK(post["mediaSize"] == 2048);
    CHECK(post["wrappedKey"] == "a2V5");
    transport->enqueue(200, R"({"id":"x","status":"published"})");
    api::earnings::uploadPostMedia(api, "7a3e9c1d-4b2f-4e6a-8d5c-0f1e2d3c4b5a", std::string("\x00\x01", 2));
    CHECK(test::FakeTransport::headerOf(transport->last(), "content-type") == "application/octet-stream");
    CHECK(transport->last().url == "https://example.com/api/earnings/posts/7a3e9c1d-4b2f-4e6a-8d5c-0f1e2d3c4b5a/media");

    transport->enqueue(200, R"([{"id":"a","authorId":"c","creatorId":"k","priceMinor":500,"currency":"usd","teaser":"","status":"published","createdAt":"t","hasMedia":false,
                                "mediaSize":null,"mine":false,"owned":true,"awaitingKey":false,"locked":false,"body":{"ciphertext":"Yw==","nonce":"bg==","wrappedKey":"aw=="},
                                "creator":{"handle":"maker","displayName":"Maker"},"purchasedAt":"t"}])");
    const auto lib = api::earnings::library(api);
    REQUIRE(lib.size() == 1);
    CHECK(lib[0].owned);
    CHECK(lib[0].body->wrappedKey == "aw==");
    CHECK(lib[0].creatorHandle == "maker");

    transport->enqueue(204, "");
    api::earnings::deliverPostKey(api, "a", "p9", "c2VhbGVk");
    CHECK(Json::parse(transport->last().body) == Json{{"purchaseId", "p9"}, {"wrappedKey", "c2VhbGVk"}});

    // Payment required on a send: the code survives for the app to show the pay bar.
    transport->enqueue(402, R"({"error":"payment_required","message":"Pay first"})");
    try {
        api.sendMessage("conv-1", {.ciphertext = "c", .nonce = "n"});
        FAIL("expected an error");
    } catch (const api::ApiError& e) {
        CHECK(e.code() == "payment_required");
    }

    // Payout setup goes through the shared creator profile.
    transport->enqueue(200, R"({"creator":null})");
    CHECK_FALSE(api::earnings::myCreator(api));
    transport->enqueue(200, R"({"creator":{"id":"k","handle":"maker","displayName":"Maker","status":"active","paidMembershipsEnabled":true,
                               "payments":{"configured":true,"status":"pending","transfersStatus":null,"requirementsDue":true,"ready":false}}})");
    const auto c = api::earnings::refreshPayoutSetup(api);
    CHECK(c.paymentsStatus == "pending");
    CHECK(c.requirementsDue);
    CHECK_FALSE(c.paymentsReady);
}
