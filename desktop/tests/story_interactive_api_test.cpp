// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Audiences, lifetimes and the interactive stories API: who gets a story, and that every call
// sends exactly what the server expects (docs/stories-advanced/api.md).
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/story_api.hpp"
#include "redvault/model/stories.hpp"

using namespace redvault;
using Json = nlohmann::json;
namespace st = redvault::model::stories;

namespace {
const std::string kFriend = "00000000-0000-4000-8000-00000000000b";
}

TEST_CASE("the three audiences: friends, close friends, and people picked one by one", "[stories][audience]") {
    const std::string a = "00000000-0000-4000-8000-00000000000b";
    const std::string b = "00000000-0000-4000-8000-00000000000c";
    const std::string c = "00000000-0000-4000-8000-00000000000d";
    const std::string stranger = "00000000-0000-4000-8000-0000000000ff";
    st::Prefs prefs;
    prefs.hideFrom = {b};
    prefs.closeFriends = {a, b};
    const std::vector<std::string> friends = {a, b, c};

    CHECK(st::recipients(friends, prefs, "friends", {}) == std::vector<std::string>{a, c});
    CHECK(st::recipients(friends, prefs, "close_friends", {}) == std::vector<std::string>{a});
    // People picked one by one: only them, never someone hidden, and never someone who isn't a friend.
    CHECK(st::recipients(friends, prefs, "custom", {c}) == std::vector<std::string>{c});
    CHECK(st::recipients(friends, prefs, "custom", {a, b, c, stranger}) == std::vector<std::string>{a, c});
    CHECK(st::recipients(friends, prefs, "custom", {}).empty());
    CHECK(st::recipients(friends, prefs, "custom", {stranger}).empty());
    // The old two-way form still means the same.
    CHECK(st::recipients(friends, prefs, true) == st::recipients(friends, prefs, "close_friends", {}));
}

TEST_CASE("how long a story stays up is worded as the web app words it", "[stories][audience]") {
    CHECK(st::lifetimeLabel(3600) == "1 hour");
    CHECK(st::lifetimeLabel(6 * 3600) == "6 hours");
    CHECK(st::lifetimeLabel(12 * 3600) == "12 hours");
    CHECK(st::lifetimeLabel(24 * 3600) == "24 hours");
    CHECK(st::lifetimeLabel(48 * 3600) == "48 hours");
    CHECK(st::lifetimeLabel(7 * 86400) == "7 days");
    CHECK(st::kDefaultStorySeconds == 86400);
}

TEST_CASE("the interactive stories API sends what the server expects", "[stories][api]") {
    auto transport = std::make_shared<test::FakeTransport>();
    auto cookies = std::make_shared<net::SessionCookieJar>();
    api::ApiClient api{"https://example.com/api/", transport, cookies};
    const std::string story = "5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6";
    const std::string base = "https://example.com/api/stories/" + story;

    // The server's config, from an older server (no stickers, 24 hours only) and a current one.
    transport->enqueue(200, R"({"media":true})");
    auto cfg = api::stories::config(api);
    CHECK(cfg.media);
    CHECK_FALSE(cfg.interactive);
    CHECK(cfg.ttlSeconds.empty());
    transport->enqueue(200, R"({"media":false,"interactive":true,"ttlSeconds":[3600,21600,43200,86400,172800,604800,"junk",-5,0]})");
    cfg = api::stories::config(api);
    CHECK_FALSE(cfg.media);
    CHECK(cfg.interactive);
    CHECK(cfg.ttlSeconds == std::vector<int>{3600, 21600, 43200, 86400, 172800, 604800});
    transport->enqueue(200, R"({"media":true,"interactive":"yes"})");
    CHECK_FALSE(api::stories::config(api).interactive);
    transport->enqueue(200, R"({"media":true})");
    CHECK(api::stories::mediaEnabled(api));

    // Posting with a lifetime; left out when not chosen.
    transport->enqueue(201, R"({"id":"x","published":true,"expiresAt":"2026-10-20T00:00:00.000Z"})");
    api::stories::NewStory s{story, "text", "Y3Q=", "bm9uY2U=", std::nullopt, "custom", true, {{kFriend, "a2V5"}}};
    s.ttlSeconds = 6 * 3600;
    CHECK(api::stories::create(api, s));
    CHECK(Json::parse(transport->last().body)["ttlSeconds"] == 21600);
    CHECK(Json::parse(transport->last().body)["audience"] == "custom");
    transport->enqueue(201, R"({"id":"x","published":true,"expiresAt":"2026-10-20T00:00:00.000Z"})");
    s.ttlSeconds.reset();
    api::stories::create(api, s);
    CHECK_FALSE(Json::parse(transport->last().body).contains("ttlSeconds"));

    // Answering.
    transport->enqueue(200, R"({"revision":2})");
    CHECK(api::stories::putAnswer(api, story, "pollabc123", "ZW52ZWxvcGU=", false, false) == 2);
    CHECK(transport->last().method == "PUT");
    CHECK(transport->last().url == base + "/responses/pollabc123");
    CHECK(Json::parse(transport->last().body) == Json{{"envelope", "ZW52ZWxvcGU="}, {"anonymous", false}, {"final", false}});
    transport->enqueue(409, R"({"error":"response_final","message":"You already answered that."})");
    try {
        api::stories::putAnswer(api, story, "quizabc123", "ZQ==", false, true);
        FAIL("a second answer to a quiz must be refused");
    } catch (const api::ApiError& e) {
        CHECK(e.status() == 409);
        CHECK(e.code() == "response_final");
    }
    // A sticker id that isn't a plain id can't reach another path.
    transport->enqueue(200, R"({"revision":0})");
    api::stories::putAnswer(api, story, "a/../b", "ZQ==", true, true);
    CHECK(transport->last().url == base + "/responses/a%2F..%2Fb");

    // The author's inbox: a nameless answer has no responder.
    transport->enqueue(200, R"([{"stickerId":"pollabc123","responderId":"u1","anonymous":false,"envelope":"ZQ==","revision":0,"respondedAt":"2026-05-04T12:34:00.000Z"},
                               {"stickerId":"questabc12","responderId":null,"anonymous":true,"envelope":"Zg==","revision":1,"respondedAt":"2026-05-04T12:35:00.000Z"}])");
    const auto rows = api::stories::answers(api, story);
    CHECK(transport->last().method == "GET");
    CHECK(transport->last().url == base + "/responses");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].responderId == "u1");
    CHECK(rows[1].responderId.empty());
    CHECK(rows[1].anonymous);
    CHECK(rows[1].revision == 1);
    CHECK(rows[1].respondedAt == "2026-05-04T12:35:00.000Z");

    transport->enqueue(200, R"([{"stickerId":"pollabc123","anonymous":false,"envelope":"ZQ==","revision":0}])");
    const auto mine = api::stories::myAnswers(api, story);
    CHECK(transport->last().url == base + "/responses/mine");
    REQUIRE(mine.size() == 1);
    CHECK(mine[0].stickerId == "pollabc123");

    // Totals, out and in.
    transport->enqueue(204, "");
    api::stories::putTotals(api, story, "pollabc123", 3, "bm9uY2U=", "Y2lwaGVy");
    CHECK(transport->last().method == "PUT");
    CHECK(transport->last().url == base + "/results/pollabc123");
    CHECK(Json::parse(transport->last().body) == Json{{"revision", 3}, {"nonce", "bm9uY2U="}, {"ciphertext", "Y2lwaGVy"}});
    transport->enqueue(409, R"({"error":"stale_revision","message":"A newer version is stored."})");
    CHECK_THROWS_AS(api::stories::putTotals(api, story, "pollabc123", 2, "bg==", "Yw=="), api::ApiError);
    transport->enqueue(200, R"([{"stickerId":"pollabc123","revision":3,"nonce":"bg==","ciphertext":"Yw=="}])");
    const auto totals = api::stories::totals(api, story);
    CHECK(transport->last().url == base + "/results");
    REQUIRE(totals.size() == 1);
    CHECK(totals[0].revision == 3);

    // Reports: the evidence goes only when the reporter chose it.
    transport->enqueue(201, R"({"reported":true})");
    api::stories::reportStory(api, story, "Spam", std::nullopt);
    CHECK(transport->last().method == "POST");
    CHECK(transport->last().url == base + "/report");
    CHECK(Json::parse(transport->last().body) == Json{{"reason", "Spam"}});
    transport->enqueue(201, R"({"reported":true})");
    api::stories::reportStory(api, story, "Spam: x", std::string("the words"));
    CHECK(Json::parse(transport->last().body) == Json{{"reason", "Spam: x"}, {"evidence", "the words"}});
    transport->enqueue(201, R"({"reported":true})");
    api::stories::reportStory(api, story, "Spam", std::string());
    CHECK_FALSE(Json::parse(transport->last().body).contains("evidence"));
    transport->enqueue(409, R"({"error":"already_reported","message":"You already reported this."})");
    CHECK_THROWS_AS(api::stories::reportStory(api, story, "Spam", std::nullopt), api::ApiError);

    transport->enqueue(201, R"({"reported":true})");
    api::stories::reportAnswer(api, story, "questabc12", "Zg==", "Abusive answer", std::string("rude words"));
    CHECK(transport->last().url == base + "/responses/report");
    CHECK(Json::parse(transport->last().body) ==
          Json{{"stickerId", "questabc12"}, {"envelope", "Zg=="}, {"reason", "Abusive answer"}, {"evidence", "rude words"}});
}
