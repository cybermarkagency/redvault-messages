// Group invite links: the crypto is pinned to the web app with vectors its
// own code produced (vectors/make_group_invite_vectors.ts), so a link made in
// any app opens in every other; the API calls go out as the web app sends them.
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "test_helpers.hpp"
#include "redvault/api/group_invite_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/group_invite_crypto.hpp"

using namespace redvault;
using namespace redvault::crypto::invites;
using redvault::crypto::Bytes;
using redvault::crypto::SecretBytes;
using redvault::test::b64;
using redvault::test::copy;
using Json = nlohmann::json;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_GROUP_INVITE_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

std::optional<std::string> optionalText(const Json& j) {
    return j.is_null() ? std::nullopt : std::optional<std::string>(j.get<std::string>());
}

void checkSameInput(const LinkInput& mine, const Json& web) {
    CHECK(mine.auth == web["auth"].get<std::string>());
    CHECK(mine.secretCiphertext == web["secretCiphertext"].get<std::string>());
    CHECK(mine.secretNonce == web["secretNonce"].get<std::string>());
    CHECK(mine.previewCiphertext == web["previewCiphertext"].get<std::string>());
    CHECK(mine.previewNonce == web["previewNonce"].get<std::string>());
    CHECK(mine.wrappedKey == optionalText(web["wrappedKey"]));
    CHECK(mine.wrappedKeyNonce == optionalText(web["wrappedKeyNonce"]));
}

/// The randomness the web app used for a link, read back from what it produced.
LinkRandomness randomnessOf(const Json& link) {
    const Json& input = link["input"];
    return LinkRandomness{b64(link["secret"].get<std::string>()), b64(input["secretNonce"].get<std::string>()),
                          b64(input["previewNonce"].get<std::string>()),
                          input["wrappedKeyNonce"].is_null() ? Bytes{} : b64(input["wrappedKeyNonce"].get<std::string>())};
}

}  // namespace

TEST_CASE("derives the same link key and auth token as the web app", "[invites]") {
    const Json& v = vectors();
    const InviteKeys keys = inviteKeys(b64(v["open"]["secret"].get<std::string>()));
    CHECK(copy(keys.key) == b64(v["open"]["key"].get<std::string>()));
    CHECK(keys.auth == v["open"]["auth"].get<std::string>());
    CHECK(inviteKeys(b64(v["approval"]["secret"].get<std::string>())).auth == v["approval"]["auth"].get<std::string>());
}

TEST_CASE("makes a link byte for byte as the web app does, given the same randomness", "[invites]") {
    const Json& v = vectors();
    const Bytes groupKey = b64(v["groupKey"].get<std::string>());

    // Joins without approval: the group key travels under the link's key. The name has quotes, a control
    // character and an emoji, so the preview's JSON must be written exactly as JSON.stringify writes it.
    const NewLink open = newInviteLink(groupKey, v["name"].get<std::string>(), false, randomnessOf(v["open"]));
    checkSameInput(open.input, v["open"]["input"]);
    CHECK(copy(open.secret) == b64(v["open"]["secret"].get<std::string>()));

    const NewLink approval = newInviteLink(groupKey, "Book club", true, randomnessOf(v["approval"]));
    checkSameInput(approval.input, v["approval"]["input"]);
    CHECK_FALSE(approval.input.wrappedKey);

    // "Approve new members" turned off later: the same wrapped key the web app would send.
    const Json& w = v["wrapForApprovalOff"];
    const WrappedKey wrapped = wrapGroupKeyForLink(groupKey, b64(w["secret"].get<std::string>()), b64(w["wrappedKeyNonce"].get<std::string>()));
    CHECK(wrapped.wrappedKey == w["wrappedKey"].get<std::string>());
    CHECK(wrapped.wrappedKeyNonce == w["wrappedKeyNonce"].get<std::string>());
}

TEST_CASE("opens a link the web app made: the member's URL and the joiner's preview", "[invites]") {
    const Json& v = vectors();
    const Bytes groupKey = b64(v["groupKey"].get<std::string>());
    const Json& input = v["open"]["input"];

    const auto opened = openInviteLink(v["linkId"].get<std::string>(), input["secretCiphertext"].get<std::string>(),
                                       input["secretNonce"].get<std::string>(), groupKey, v["webBase"].get<std::string>());
    REQUIRE(opened);
    CHECK(copy(opened->secret) == b64(v["open"]["secret"].get<std::string>()));
    CHECK(opened->url == v["open"]["url"].get<std::string>());
    CHECK(inviteUrl(v["webBase"].get<std::string>(), v["linkId"].get<std::string>(), opened->secret) == v["open"]["url"].get<std::string>());
    CHECK(secretToFragment(opened->secret) == v["open"]["fragment"].get<std::string>());

    // Someone without the group key can't read the link's secret.
    const Bytes wrongKey(32, 9);
    CHECK_FALSE(openInviteLink(v["linkId"].get<std::string>(), input["secretCiphertext"].get<std::string>(),
                               input["secretNonce"].get<std::string>(), wrongKey, v["webBase"].get<std::string>()));

    const Bytes secret = b64(v["open"]["secret"].get<std::string>());
    const OpenedPreview preview = openInvitePreview(input["previewCiphertext"].get<std::string>(), input["previewNonce"].get<std::string>(),
                                                    optionalText(input["wrappedKey"]), optionalText(input["wrappedKeyNonce"]), secret);
    CHECK(preview.name == v["open"]["previewName"].get<std::string>());
    CHECK(preview.name == v["name"].get<std::string>());
    REQUIRE(preview.groupKey);
    CHECK(copy(*preview.groupKey) == groupKey);

    const Json& ai = v["approval"]["input"];
    const OpenedPreview asking = openInvitePreview(ai["previewCiphertext"].get<std::string>(), ai["previewNonce"].get<std::string>(),
                                                   std::nullopt, std::nullopt, b64(v["approval"]["secret"].get<std::string>()));
    CHECK(asking.name == "Book club");
    CHECK_FALSE(asking.groupKey);

    // A link that was reset: the old secret doesn't open the new preview.
    CHECK_THROWS_AS(openInvitePreview(input["previewCiphertext"].get<std::string>(), input["previewNonce"].get<std::string>(), std::nullopt,
                                      std::nullopt, b64(v["approval"]["secret"].get<std::string>())),
                    crypto::CryptoError);
}

TEST_CASE("reads previews the way the web app does when the name is missing or odd", "[invites]") {
    const Json& v = vectors();
    const Bytes secret = b64(v["open"]["secret"].get<std::string>());
    for (const auto& odd : v["oddPreviews"]) {
        INFO(odd["json"].get<std::string>());
        const OpenedPreview preview =
            openInvitePreview(odd["previewCiphertext"].get<std::string>(), odd["previewNonce"].get<std::string>(), std::nullopt, std::nullopt, secret);
        CHECK(preview.name == odd["name"].get<std::string>());
    }
}

TEST_CASE("joins with the group key sealed to yourself, which the web app's sealing matches", "[invites]") {
    const Json& v = vectors();
    const Bytes groupKey = b64(v["groupKey"].get<std::string>());
    const Bytes joinerPublic = b64(v["joiner"]["publicKey"].get<std::string>());
    const Bytes joinerPrivate = b64(v["joiner"]["privateKey"].get<std::string>());

    const SecretBytes fromWeb = crypto::unsealConversationKey(b64(v["joiner"]["sealedGroupKey"].get<std::string>()), joinerPublic, joinerPrivate);
    CHECK(copy(fromWeb) == groupKey);
    const SecretBytes fromDesktop = crypto::unsealConversationKey(crypto::sealConversationKey(groupKey, joinerPublic), joinerPublic, joinerPrivate);
    CHECK(copy(fromDesktop) == groupKey);
}

TEST_CASE("reads pasted invite links exactly as the web app does", "[invites]") {
    for (const auto& c : vectors()["parseCases"]) {
        const std::string text = c["text"];
        INFO(text);
        const auto parsed = parseInvite(text);
        if (c["result"].is_null()) {
            CHECK_FALSE(parsed);
        } else {
            REQUIRE(parsed);
            CHECK(parsed->linkId == c["result"]["linkId"].get<std::string>());
            CHECK(parsed->secret == c["result"]["secret"].get<std::string>());
        }
    }
}

TEST_CASE("takes the secret out of a fragment exactly as the web app does", "[invites]") {
    for (const auto& c : vectors()["fragmentCases"]) {
        const std::string fragment = c["fragment"];
        INFO(fragment);
        const auto secret = secretFromFragment(fragment);
        if (c["secret"].is_null()) {
            CHECK_FALSE(secret);
        } else {
            REQUIRE(secret);
            CHECK(crypto::base64Encode(*secret) == c["secret"].get<std::string>());
        }
    }
}

TEST_CASE("a fresh link opens with its own secret and carries the key only without approval", "[invites]") {
    const SecretBytes groupKey = crypto::generateConversationKey();
    const NewLink a = newInviteLink(groupKey, "Hikers", false);
    const NewLink b = newInviteLink(groupKey, "Hikers", false);
    CHECK(copy(a.secret) != copy(b.secret));
    CHECK(a.input.secretNonce != b.input.secretNonce);
    REQUIRE(a.input.wrappedKey);

    const OpenedPreview preview = openInvitePreview(a.input.previewCiphertext, a.input.previewNonce, a.input.wrappedKey, a.input.wrappedKeyNonce, a.secret);
    CHECK(preview.name == "Hikers");
    REQUIRE(preview.groupKey);
    CHECK(copy(*preview.groupKey) == copy(groupKey));

    const auto opened = openInviteLink("L1", a.input.secretCiphertext, a.input.secretNonce, groupKey, "https://example.com/app/");
    REQUIRE(opened);
    const auto parsed = parseInvite("Join: " + opened->url);
    REQUIRE(parsed);
    CHECK(parsed->linkId == "L1");
    const auto secret = secretFromFragment(parsed->secret);
    REQUIRE(secret);
    CHECK(copy(*secret) == copy(a.secret));
    CHECK(inviteKeys(*secret).auth == a.input.auth);

    CHECK_FALSE(newInviteLink(groupKey, "Hikers", true).input.wrappedKey);
}

// --- The API calls ---

namespace {

struct Harness {
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::shared_ptr<net::SessionCookieJar> cookies = std::make_shared<net::SessionCookieJar>();
    api::ApiClient api{"https://example.com/api/", transport, cookies};
};

const std::string kGroup = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";
const std::string kUser = "22222222-2222-4222-8222-222222222222";
const std::string kLinkJson =
    R"({"link":{"id":"L1","secretCiphertext":"c2VjcmV0","secretNonce":"bm9uY2U=","approval":true,"createdAt":"2026-10-18T09:00:00.000Z"}})";

}  // namespace

TEST_CASE("the group's link: read, turned on, approval switched, turned off", "[invites][api]") {
    Harness h;
    h.transport->enqueue(200, R"({"link":null})");
    CHECK_FALSE(api::invites::getLink(h.api, kGroup));
    CHECK(h.transport->last().method == "GET");
    CHECK(h.transport->last().url == "https://example.com/api/group-invites/groups/" + kGroup + "/link");

    h.transport->enqueue(200, kLinkJson);
    const SecretBytes groupKey = crypto::generateConversationKey();
    const NewLink link = newInviteLink(groupKey, "Book club", true);
    const auto created = api::invites::setLink(h.api, kGroup, link.input);
    CHECK(created.id == "L1");
    CHECK(created.approval);
    CHECK(h.transport->last().method == "PUT");
    const Json sent = Json::parse(h.transport->last().body);
    CHECK(sent["auth"] == link.input.auth);
    CHECK(sent["previewCiphertext"] == link.input.previewCiphertext);
    // Approval on: the server is told explicitly there's no wrapped key.
    CHECK(sent.contains("wrappedKey"));
    CHECK(sent["wrappedKey"].is_null());
    CHECK(sent["wrappedKeyNonce"].is_null());

    h.transport->enqueue(200, kLinkJson);
    api::invites::setApproval(h.api, kGroup, false, WrappedKey{"d3JhcHBlZA==", "bm9uY2U="});
    CHECK(h.transport->last().method == "PATCH");
    CHECK(Json::parse(h.transport->last().body) == Json{{"approval", false}, {"wrappedKey", "d3JhcHBlZA=="}, {"wrappedKeyNonce", "bm9uY2U="}});
    h.transport->enqueue(200, kLinkJson);
    api::invites::setApproval(h.api, kGroup, true);
    CHECK(Json::parse(h.transport->last().body) == Json{{"approval", true}});

    h.transport->enqueue(204, "");
    api::invites::deleteLink(h.api, kGroup);
    CHECK(h.transport->last().method == "DELETE");
    CHECK(h.transport->last().url == "https://example.com/api/group-invites/groups/" + kGroup + "/link");
}

TEST_CASE("requests to join: listed, approved with a sealed key, declined", "[invites][api]") {
    Harness h;
    h.transport->enqueue(200, R"([{"userId":")" + kUser + R"(","createdAt":"2026-10-18T09:00:00.000Z"}])");
    const auto requests = api::invites::listRequests(h.api, kGroup);
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].userId == kUser);
    CHECK(h.transport->last().url == "https://example.com/api/group-invites/groups/" + kGroup + "/requests");

    h.transport->enqueue(204, "");
    api::invites::approve(h.api, kGroup, kUser, "c2VhbGVk");
    CHECK(h.transport->last().method == "POST");
    CHECK(h.transport->last().url == "https://example.com/api/group-invites/groups/" + kGroup + "/requests/" + kUser + "/approve");
    CHECK(Json::parse(h.transport->last().body) == Json{{"wrappedKey", "c2VhbGVk"}});

    h.transport->enqueue(204, "");
    api::invites::deny(h.api, kGroup, kUser);
    CHECK(h.transport->last().method == "DELETE");
    CHECK(h.transport->last().url == "https://example.com/api/group-invites/groups/" + kGroup + "/requests/" + kUser);
}

TEST_CASE("opening and joining a link send only the auth token, never the secret", "[invites][api]") {
    Harness h;
    const SecretBytes groupKey = crypto::generateConversationKey();
    const NewLink link = newInviteLink(groupKey, "Book club", false);
    const std::string fragment = secretToFragment(link.secret);

    h.transport->enqueue(200, Json{{"conversationId", kGroup},
                                   {"previewCiphertext", link.input.previewCiphertext},
                                   {"previewNonce", link.input.previewNonce},
                                   {"memberCount", 3},
                                   {"approval", false},
                                   {"wrappedKey", *link.input.wrappedKey},
                                   {"wrappedKeyNonce", *link.input.wrappedKeyNonce},
                                   {"member", false},
                                   {"requested", false}}
                                  .dump());
    const auto preview = api::invites::open(h.api, "L1_x-", link.input.auth);
    CHECK(h.transport->last().url == "https://example.com/api/group-invites/links/L1_x-/open");
    CHECK(Json::parse(h.transport->last().body) == Json{{"auth", link.input.auth}});
    CHECK(preview.memberCount == 3);
    CHECK_FALSE(preview.approval);
    const OpenedPreview opened = openInvitePreview(preview.previewCiphertext, preview.previewNonce, preview.wrappedKey, preview.wrappedKeyNonce, link.secret);
    CHECK(opened.name == "Book club");

    h.transport->enqueue(200, R"({"status":"joined","conversationId":")" + kGroup + R"("})");
    const auto joined = api::invites::join(h.api, "L1_x-", link.input.auth, std::string("c2VhbGVk"));
    CHECK(joined.status == "joined");
    CHECK(joined.conversationId == kGroup);
    CHECK(Json::parse(h.transport->last().body) == Json{{"auth", link.input.auth}, {"wrappedKey", "c2VhbGVk"}});

    h.transport->enqueue(200, R"({"status":"requested","conversationId":")" + kGroup + R"("})");
    CHECK(api::invites::join(h.api, "L1_x-", link.input.auth, std::nullopt).status == "requested");
    CHECK(Json::parse(h.transport->last().body) == Json{{"auth", link.input.auth}, {"wrappedKey", nullptr}});

    // Nothing that went out carries the secret, in any encoding.
    for (const auto& request : h.transport->requests) {
        CHECK(request.url.find(fragment) == std::string::npos);
        CHECK(request.body.find(fragment) == std::string::npos);
        CHECK(request.body.find(crypto::base64Encode(link.secret)) == std::string::npos);
    }
}
