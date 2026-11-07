// The community calls that carry no keys: links, pictures and the
// administrators' routes. Requests are checked byte for byte against what the
// web app's communityApi.ts sends, and the picture fetch must refuse every
// path it wasn't made for without asking the server.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/api/community_api.hpp"

using namespace redvault;
using redvault::test::FakeTransport;
using Json = nlohmann::json;
namespace community = redvault::api::community;

namespace {

const std::string kId = "6f9619ff-8b86-4011-b42d-00c04fc964ff";
const std::string kToken = "Zm9vYmFyYmF6cXV4MTIzNDU2Nzg5MGFi";  // 32 base64url characters, like randomBytes(24)

struct Fixture {
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<net::SessionCookieJar> cookies = std::make_shared<net::SessionCookieJar>();
    api::ApiClient api{"https://example.com/app/api", transport, cookies};

    Fixture() { cookies->saveFromResponse(*net::parseUrl("https://example.com/app/api/auth/login"), {"sid=abc; Path=/"}); }
};

}  // namespace

TEST_CASE("community links read as the web app reads them", "[community][links]") {
    const auto link = community::parseLink("https://example.com/app/?community=" + kId + "&invite=" + kToken);
    REQUIRE(link.has_value());
    CHECK(link->communityId == kId);
    CHECK(link->invite == kToken);

    // Any order, upper-case ids, other parameters, a fragment, text around a pasted link.
    const auto pasted = community::parseLink("Join us: https://example.com/?x=1&invite=" + kToken + "&community=6F9619FF-8B86-4011-B42D-00C04FC964FF#top  thanks");
    REQUIRE(pasted.has_value());
    CHECK(pasted->communityId == kId);
    CHECK(pasted->invite == kToken);

    // No invite, or one that can't be a token: the community still opens, without it.
    CHECK(community::parseLink("https://example.com/?community=" + kId) == community::Link{kId, ""});
    CHECK(community::parseLink("https://example.com/?community=" + kId + "&invite=short") == community::Link{kId, ""});
    CHECK(community::parseLink("https://example.com/?community=" + kId + "&invite=has%20space1234") == community::Link{kId, ""});
    CHECK(community::parseLink("https://example.com/?community=" + kId + "&invite=" + std::string(101, 'a')) == community::Link{kId, ""});
    // The first parameter of a name wins, as URLSearchParams.get does; escapes are decoded.
    CHECK(community::parseLink("https://example.com/?community=" + kId + "&community=11111111-2222-4333-8444-555555555555")->communityId == kId);
    CHECK(community::parseLink("https://example.com/?community=6f9619ff%2D8b86-4011-b42d-00c04fc964ff")->communityId == kId);

    // Not community links.
    CHECK_FALSE(community::parseLink("").has_value());
    CHECK_FALSE(community::parseLink("?community=" + kId).has_value());  // not an address
    CHECK_FALSE(community::parseLink("ftp://example.com/?community=" + kId).has_value());
    CHECK_FALSE(community::parseLink("https://example.com/?community=not-an-id").has_value());
    CHECK_FALSE(community::parseLink("https://example.com/?community=" + kId + "0").has_value());
    CHECK_FALSE(community::parseLink("https://example.com/?xcommunity=" + kId).has_value());
    CHECK_FALSE(community::parseLink("https://example.com/#?community=" + kId).has_value());  // only in the fragment
    CHECK_FALSE(community::parseLink("https://example.com/?channel=" + kId).has_value());
    // A group invite link is not a community link.
    CHECK_FALSE(community::parseLink("https://example.com/?invite=abcdefghijkl#" + std::string(43, 'A')).has_value());

    CHECK(community::inviteUrl("https://example.com/app", kId, kToken) == "https://example.com/app/?community=" + kId + "&invite=" + kToken);
    CHECK(community::parseLink(community::inviteUrl("https://example.com/app/", kId, kToken)) == community::Link{kId, kToken});
}

TEST_CASE("picture paths are the web app's, and nothing else is fetched", "[community][pictures]") {
    CHECK(community::imagePath(kId, "avatar") == "/api/communities/" + kId + "/avatar");
    CHECK(community::imagePath("6F9619FF-8B86-4011-B42D-00C04FC964FF", "banner", kToken) == "/api/communities/" + kId + "/banner?invite=" + kToken);
    CHECK(community::creatorAvatarPath(kId) == "/api/communities/creators/" + kId + "/avatar");
    CHECK(community::imagePath(kId, "photo").empty());
    CHECK(community::imagePath("../auth/me", "avatar").empty());
    CHECK(community::imagePath(kId, "avatar", "bad token&x=1").empty());
    CHECK(community::creatorAvatarPath("me").empty());

    Fixture f;
    for (const auto& path : {community::imagePath(kId, "avatar"), community::imagePath(kId, "banner", kToken), community::creatorAvatarPath(kId)}) {
        f.transport->enqueue(200, std::string("\x89PNG", 4));
        CHECK(f.api.fetchCommunityImage(path) == std::string("\x89PNG", 4));
        CHECK(f.transport->last().method == "GET");
        CHECK(f.transport->last().url == "https://example.com/app" + path);
        CHECK(FakeTransport::headerOf(f.transport->last(), "cookie") == "sid=abc");
    }

    const auto before = f.transport->requests.size();
    const std::vector<std::string> refused{
        "https://evil.example/x.png",
        "/api/users/" + kId + "/avatar",
        "/api/communities/" + kId,
        "/api/communities/" + kId + "/avatar?invite=" + kToken + "&x=1",
        "/api/communities/" + kId + "/avatar?invite=short",
        "/api/communities/" + kId + "/members",
        "/api/communities/creators/" + kId + "/avatar?invite=" + kToken,
        "/api/communities/creators/me/avatar",
        "/api/communities/%2e%2e/auth/me/avatar",
        "/api/communities/" + kId + "/avatar/../../../auth/me",
        "/api/communities/" + kId + "/avatar\n",
        "api/communities/" + kId + "/avatar",
        "",
    };
    for (const auto& bad : refused) {
        CHECK_THROWS_AS(f.api.fetchCommunityImage(bad), api::ApiError);
    }
    CHECK(f.transport->requests.size() == before);

    // No picture: the server's 404 is an error, so the letter stays.
    f.transport->enqueue(404, "");
    CHECK_THROWS_AS(f.api.fetchCommunityImage(community::imagePath(kId, "avatar")), api::ApiError);
}

TEST_CASE("the administrators' calls send what the web app sends", "[community][admin]") {
    Fixture f;
    const auto expect = [&](const char* method, const std::string& path) {
        CHECK(f.transport->last().method == method);
        CHECK(f.transport->last().url == "https://example.com/app/api/" + path);
    };
    const auto body = [&] { return Json::parse(f.transport->last().body); };

    f.transport->enqueue(200, R"({"platformCommissionNetMinor":120,"rules":[]})");
    CHECK(community::admin::overview(f.api).at("platformCommissionNetMinor") == 120);
    expect("GET", "admin/communities/overview");

    f.transport->enqueue(200, "[]");
    community::admin::transactions(f.api);
    expect("GET", "admin/communities/transactions");
    f.transport->enqueue(200, "[]");
    community::admin::transactions(f.api, "mismatch");
    expect("GET", "admin/communities/transactions?reconciliation=mismatch");

    f.transport->enqueue(200, R"({"refundId":"re_1"})");
    community::admin::refund(f.api, kId, std::nullopt, "Duplicate charge");
    expect("POST", "admin/communities/transactions/" + kId + "/refund");
    CHECK(body() == Json{{"amountMinor", nullptr}, {"reason", "Duplicate charge"}});
    f.transport->enqueue(200, R"({"refundId":"re_2"})");
    community::admin::refund(f.api, kId, 499, "Partial");
    CHECK(body() == Json{{"amountMinor", 499}, {"reason", "Partial"}});

    f.transport->enqueue(201, R"({"id":"r","version":2})");
    community::admin::addCommissionRule(f.api, 1250, 100, 50000, "Launch rate");
    expect("POST", "admin/communities/commission-rules");
    CHECK(body() == Json{{"rateBps", 1250}, {"minPriceMinor", 100}, {"maxPriceMinor", 50000}, {"note", "Launch rate"}});

    f.transport->enqueue(200, "[]");
    community::admin::creators(f.api);
    expect("GET", "admin/communities/creators");
    f.transport->enqueue(200, "{}");
    community::admin::setCreator(f.api, kId, std::nullopt, true);
    expect("PATCH", "admin/communities/creators/" + kId);
    CHECK(body() == Json{{"verified", true}});
    f.transport->enqueue(200, "{}");
    community::admin::setCreator(f.api, kId, std::string("suspended"), std::nullopt);
    CHECK(body() == Json{{"status", "suspended"}});

    f.transport->enqueue(200, "[]");
    community::admin::communities(f.api);
    expect("GET", "admin/communities/communities");
    for (const char* action : {"suspend", "unsuspend", "close"}) {
        f.transport->enqueue(200, R"({"status":"suspended"})");
        community::admin::communityAction(f.api, kId, action);
        expect("POST", "admin/communities/communities/" + kId + "/" + action);
    }
    const auto before = f.transport->requests.size();
    CHECK_THROWS_AS(community::admin::communityAction(f.api, kId, "delete"), api::UserFacingError);
    CHECK_THROWS_AS(community::admin::communityAction(f.api, kId, "suspend/../close"), api::UserFacingError);
    CHECK(f.transport->requests.size() == before);

    f.transport->enqueue(200, "[]");
    community::admin::reports(f.api);
    expect("GET", "admin/communities/reports?status=open");
    f.transport->enqueue(204, "");
    community::admin::resolveReport(f.api, kId, "dismissed", std::nullopt);
    expect("POST", "admin/communities/reports/" + kId + "/resolve");
    CHECK(body() == Json{{"status", "dismissed"}, {"note", nullptr}});

    f.transport->enqueue(200, R"({"checked":3,"mismatches":0,"unbalancedJournals":0})");
    CHECK(community::admin::reconcile(f.api).at("checked") == 3);
    expect("POST", "admin/communities/reconcile");

    // Ids that reach a path are escaped, so they can't step out of it.
    f.transport->enqueue(200, "{}");
    community::admin::setCreator(f.api, "../../auth/me", std::nullopt, true);
    expect("PATCH", "admin/communities/creators/..%2F..%2Fauth%2Fme");
}

TEST_CASE("amounts a person types are read as the web app reads them", "[community][money]") {
    CHECK(community::parseDollars("10") == 1000);
    CHECK(community::parseDollars("9.99") == 999);
    CHECK(community::parseDollars(" $4.5 ") == 450);
    CHECK(community::parseDollars("0.05") == 5);
    CHECK(community::parseDollars("999999.99") == 99999999);
    for (const char* bad : {"", "$", "abc", "1,000", "1.", ".5", "1.234", "-5", "1e3", "1000000", "5 dollars", "4.5.1"}) {
        CHECK_FALSE(community::parseDollars(bad).has_value());
    }

    CHECK(community::parseRateBps("12") == 1200);
    CHECK(community::parseRateBps("12.5") == 1250);
    CHECK(community::parseRateBps("12.25 %") == 1225);
    CHECK(community::parseRateBps("10%") == 1000);
    for (const char* bad : {"", "%", "twelve", "100", "12.555", "-12", "12,5", "0x10"}) {
        CHECK_FALSE(community::parseRateBps(bad).has_value());
    }
}
