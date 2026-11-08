// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Channels: following lives only on this computer (model/channels.hpp, the
// web app's lib/channels.ts), and reading goes out without the session
// cookie (with a private channel's access token when there is one), so the
// server can't tie a reader to an account. Each computer counts a post's view
// once (lib/channelViews.ts). The private link's crypto is pinned to the web
// app in channel_crypto_test.cpp.
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/channel_api.hpp"
#include "redvault/model/channels.hpp"

using namespace redvault;
using namespace redvault::model::channels;
using redvault::api::channels::Channel;
using Json = nlohmann::json;

namespace {

const std::string kId = "6f1e2d3c-4b5a-4968-8776-655443322110";
const std::string kOther = "11111111-2222-4333-8444-555555555555";
const std::string kBase = "https://www.redvaultsystems.com/redvaultmessages/";

Channel channel(const std::string& id, const std::string& name, std::optional<std::string> lastPostAt) {
    return Channel{id, name, "About things", "2026-10-01T00:00:00.000Z", std::move(lastPostAt)};
}

}  // namespace

TEST_CASE("following starts with everything already posted counted as seen", "[channels]") {
    Followed list;
    list = follow(list, channel(kId, "News", "2026-10-18T09:00:00.000Z"));
    REQUIRE(list.size() == 1);
    CHECK(list[0] == FollowedChannel{kId, "News", "2026-10-18T09:00:00.000Z"});
    CHECK(isFollowing(list, kId));
    CHECK_FALSE(isFollowing(list, kOther));
    // Following twice changes nothing.
    CHECK(follow(list, channel(kId, "Renamed", std::nullopt)) == list);
    CHECK_FALSE(hasUnread(list[0], channel(kId, "News", "2026-10-18T09:00:00.000Z")));

    list = follow(list, channel(kOther, "Quiet", std::nullopt));
    CHECK(list.size() == 2);
    CHECK_FALSE(list[1].seenAt);
    CHECK(unfollow(list, kId) == Followed{list[1]});
}

TEST_CASE("the unread dot: a post newer than the newest one seen", "[channels]") {
    const FollowedChannel seen{kId, "News", "2026-10-18T09:00:00.000Z"};
    CHECK(hasUnread(seen, channel(kId, "News", "2026-10-18T09:00:00.001Z")));
    CHECK_FALSE(hasUnread(seen, channel(kId, "News", "2026-10-18T09:00:00.000Z")));
    CHECK_FALSE(hasUnread(seen, channel(kId, "News", "2026-10-17T23:59:59.999Z")));
    // No posts yet: nothing to read.
    CHECK_FALSE(hasUnread(seen, channel(kId, "News", std::nullopt)));
    // Followed before the first post: the first post is new.
    CHECK(hasUnread(FollowedChannel{kId, "News", std::nullopt}, channel(kId, "News", "2026-10-18T09:00:00.000Z")));
    // A time that doesn't parse never counts as newer (like Date.parse giving NaN).
    CHECK_FALSE(hasUnread(FollowedChannel{kId, "News", "yesterday"}, channel(kId, "News", "2026-10-18T09:00:00.000Z")));

    // Opening the channel marks everything seen, and keeps the name current.
    const Followed after = markSeen({seen}, channel(kId, "Daily news", "2026-10-18T10:00:00.000Z"));
    CHECK(after == Followed{FollowedChannel{kId, "Daily news", "2026-10-18T10:00:00.000Z"}});
    CHECK_FALSE(hasUnread(after[0], channel(kId, "Daily news", "2026-10-18T10:00:00.000Z")));
}

TEST_CASE("the follow list round-trips through the web app's JSON shape", "[channels]") {
    const Followed list{FollowedChannel{kId, "News \"daily\"", "2026-10-18T09:00:00.000Z"}, FollowedChannel{kOther, "Quiet", std::nullopt}};
    const std::string text = toJson(list);
    CHECK(Json::parse(text) == Json::parse(R"([{"id":")" + kId + R"(","name":"News \"daily\"","seenAt":"2026-10-18T09:00:00.000Z"},)" +
                                           R"({"id":")" + kOther + R"(","name":"Quiet","seenAt":null}])"));
    CHECK(fromJson(text) == list);

    // It's only a preference: anything unreadable is an empty list, and odd entries are skipped.
    CHECK(fromJson("").empty());
    CHECK(fromJson("not json").empty());
    CHECK(fromJson(R"({"id":"x"})").empty());
    CHECK(fromJson(R"([1, {"name":"no id"}, {"id":"a","name":5,"seenAt":7}, {"id":"a"}])") == Followed{FollowedChannel{"a", "", std::nullopt}});
}

TEST_CASE("channel links: the id, and a private channel's secret in the fragment", "[channels]") {
    const ChannelLink plain{kId, std::nullopt};
    CHECK(channelUrl(kBase, kId) == kBase + "?channel=" + kId);
    CHECK(parseChannelLink(channelUrl(kBase, kId)) == plain);
    CHECK(parseChannelLink("Have a look: " + kBase + "?channel=" + kId + " it's good") == plain);
    // A fragment that isn't a secret (an anchor, say) is no secret.
    CHECK(parseChannelLink(kBase + "?x=1&channel=6F1E2D3C-4B5A-4968-8776-655443322110#top") == plain);
    CHECK_FALSE(parseChannelLink(kBase + "?channel=not-an-id"));
    CHECK_FALSE(parseChannelLink(kBase + "?channel=" + kId + "0"));
    CHECK_FALSE(parseChannelLink(kBase + "?mychannel=" + kId));
    CHECK_FALSE(parseChannelLink("channel=" + kId));
    CHECK_FALSE(parseChannelLink("hello"));

    const std::string secret = "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA";
    const ChannelLink withSecret{kId, secret};
    CHECK(channelUrl(kBase, kId, secret) == kBase + "?channel=" + kId + "#" + secret);
    CHECK(channelUrl(kBase, kId, std::string()) == kBase + "?channel=" + kId);
    CHECK(parseChannelLink(channelUrl(kBase, kId, secret)) == withSecret);
    // In a message: the link ends at the first space or line break.
    CHECK(parseChannelLink("Join: " + channelUrl(kBase, kId, secret) + "\nbye") == withSecret);
    CHECK(parseChannelLink("Join: " + channelUrl(kBase, kId, secret) + "\n") == withSecret);
    CHECK(parseChannelLink("Join: " + channelUrl(kBase, kId, secret) + " bye") == withSecret);
    CHECK(parseChannelLink(kBase + "?channel=" + kId + "&x=1#" + secret) == withSecret);
    // Only exactly a secret: one character short, or one too many, isn't.
    CHECK(parseChannelLink(kBase + "?channel=" + kId + "#" + secret.substr(1)) == plain);
    CHECK(parseChannelLink(kBase + "?channel=" + kId + "#" + secret + ".") == plain);
}

TEST_CASE("a private channel's secret is kept with the follow", "[channels]") {
    const std::string secret = "AQIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyA";
    const std::string newer = "-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_v7-_s";
    Channel secretChannel = channel(kId, "Insiders", "2026-10-18T09:00:00.000Z");
    secretChannel.isPrivate = true;

    Followed list = follow({}, secretChannel, secret);
    CHECK(list == Followed{FollowedChannel{kId, "Insiders", "2026-10-18T09:00:00.000Z", secret}});
    // An empty secret is none (the web app's `...(secret ? { secret } : {})`).
    CHECK_FALSE(follow({}, secretChannel, std::string())[0].secret);

    // Opening it with a newer link keeps that one; without one, the old stays.
    CHECK(markSeen(list, secretChannel)[0].secret == secret);
    CHECK(markSeen(list, secretChannel, newer)[0].secret == newer);

    const std::string text = toJson(list);
    CHECK(Json::parse(text) == Json::parse(R"([{"id":")" + kId + R"(","name":"Insiders","seenAt":"2026-10-18T09:00:00.000Z","secret":")" +
                                           secret + R"("}])"));
    CHECK(fromJson(text) == list);
    // A public channel's follow has no "secret" at all, like the web app's.
    CHECK_FALSE(Json::parse(toJson(follow({}, channel(kId, "News", std::nullopt)))).at(0).contains("secret"));
    CHECK(fromJson(R"([{"id":"a","secret":7},{"id":"b","secret":""}])") ==
          Followed{FollowedChannel{"a", "", std::nullopt, std::nullopt}, FollowedChannel{"b", "", std::nullopt, std::nullopt}});
}

TEST_CASE("each computer counts a post's view once", "[channels]") {
    std::vector<std::string> remembered;
    // Lowercased, each once, in order.
    CHECK(uncountedViews(remembered, {"AAAA", "bbbb", "aaaa", "cccc"}) == std::vector<std::string>{"aaaa", "bbbb", "cccc"});
    CHECK(remembered == std::vector<std::string>{"aaaa", "bbbb", "cccc"});
    // Seen again (in the channel or as a forwarded copy): not counted again.
    CHECK(uncountedViews(remembered, {"BBBB", "dddd"}) == std::vector<std::string>{"dddd"});
    CHECK(uncountedViews(remembered, {"aaaa"}).empty());
    CHECK(uncountedViews(remembered, {}).empty());

    // Only the newest kMaxRememberedViews are kept; the oldest are forgotten first.
    std::vector<std::string> many;
    for (std::size_t i = 0; i < kMaxRememberedViews + 10; ++i) many.push_back("p" + std::to_string(i));
    std::vector<std::string> list;
    CHECK(uncountedViews(list, many).size() == kMaxRememberedViews + 10);
    REQUIRE(list.size() == kMaxRememberedViews);
    CHECK(list.front() == "p10");
    CHECK(list.back() == "p" + std::to_string(kMaxRememberedViews + 9));
}

// --- The API calls ---

const std::string kAccess = "KQ5oUWp7EuQY/qlUwD5APFsYvK80h+szX5tNzQAxJlk=";

namespace {

struct Harness {
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::shared_ptr<net::SessionCookieJar> cookies = std::make_shared<net::SessionCookieJar>();
    api::ApiClient api{"https://example.com/api/", transport, cookies};

    Harness() { cookies->saveFromResponse(*net::parseUrl("https://example.com/api/auth/login"), {"sid=abc; Path=/"}); }
};

std::string channelJson(const std::string& id, const char* lastPostAt = "null") {
    return R"({"id":")" + id + R"(","name":"News","description":"","createdAt":"2026-10-01T00:00:00.000Z","lastPostAt":)" + lastPostAt + "}";
}

}  // namespace

TEST_CASE("reading channels sends no session cookie, and takes none back", "[channels][api][privacy]") {
    Harness h;
    h.transport->enqueue(200, "[" + channelJson(kId, R"("2026-10-18T09:00:00.000Z")") + "]", {{"set-cookie", "sid=tracker; Path=/"}});
    const auto found = api::channels::search(h.api, "  rust news ", 30);
    REQUIRE(found.size() == 1);
    CHECK(found[0].lastPostAt == "2026-10-18T09:00:00.000Z");
    CHECK(h.transport->last().url == "https://example.com/api/public/channels?limit=30&offset=30&q=rust%20news");
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie").empty());

    h.transport->enqueue(200, channelJson(kId));
    CHECK_FALSE(api::channels::get(h.api, kId).lastPostAt);
    CHECK(h.transport->last().url == "https://example.com/api/public/channels/" + kId);
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie").empty());

    h.transport->enqueue(200, R"([{"id":"p1","body":"Hello","createdAt":"2026-10-18T09:00:00.000Z"}])");
    const auto posts = api::channels::posts(h.api, kId, std::string("2026-10-18T09:00:00.000Z"));
    REQUIRE(posts.size() == 1);
    CHECK(posts[0].body == "Hello");
    CHECK(h.transport->last().url == "https://example.com/api/public/channels/" + kId +
                                         "/posts?limit=30&before=2026-10-18T09%3A00%3A00.000Z");
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie").empty());

    // The session is untouched by what a public read answered, and signed-in calls still carry it.
    h.transport->enqueue(200, "[]");
    api::channels::mine(h.api);
    CHECK(h.transport->last().url == "https://example.com/api/channels/mine");
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie") == "sid=abc");

    h.transport->enqueue(200, "[]");
    api::channels::search(h.api, "   ");
    CHECK(h.transport->last().url == "https://example.com/api/public/channels?limit=30&offset=0");
}

TEST_CASE("owning a channel: create, edit, post, delete, and reporting someone else's", "[channels][api]") {
    Harness h;
    h.transport->enqueue(201, channelJson(kId));
    CHECK(api::channels::create(h.api, "News", "About news").id == kId);
    CHECK(h.transport->last().method == "POST");
    CHECK(h.transport->last().url == "https://example.com/api/channels");
    CHECK(Json::parse(h.transport->last().body) == Json{{"name", "News"}, {"description", "About news"}});
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie") == "sid=abc");

    h.transport->enqueue(200, channelJson(kId));
    api::channels::update(h.api, kId, "News 2", "");
    CHECK(h.transport->last().method == "PATCH");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId);

    h.transport->enqueue(201, R"({"id":"p1","body":"Hi","createdAt":"2026-10-18T09:00:00.000Z"})");
    CHECK(api::channels::post(h.api, kId, "Hi").id == "p1");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId + "/posts");
    CHECK(Json::parse(h.transport->last().body) == Json{{"body", "Hi"}});

    h.transport->enqueue(204, "");
    api::channels::removePost(h.api, kId, "p1");
    CHECK(h.transport->last().method == "DELETE");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId + "/posts/p1");

    h.transport->enqueue(204, "");
    api::channels::remove(h.api, kId);
    CHECK(h.transport->last().method == "DELETE");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId);

    h.transport->enqueue(204, "");
    api::channels::report(h.api, kOther, "Spam");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kOther + "/report");
    CHECK(Json::parse(h.transport->last().body) == Json{{"reason", "Spam"}});

    // A Free or Pro account asking to create one: the server's own words come through.
    h.transport->enqueue(403, R"({"error":"premium_required","message":"Creating a channel needs Premium."})");
    try {
        api::channels::create(h.api, "News", "");
        FAIL("expected an error");
    } catch (const api::ApiError& e) {
        CHECK(e.status() == 403);
        CHECK(e.code() == "premium_required");
        CHECK(std::string(e.what()) == "Creating a channel needs Premium.");
    }
}

TEST_CASE("a private channel's reads carry its access token, still without the cookie", "[channels][api][privacy]") {
    Harness h;
    h.transport->enqueue(200, R"({"id":")" + kId + R"(","name":"Insiders","description":"","createdAt":"2026-10-01T00:00:00.000Z",)"
                                   R"("lastPostAt":null,"isPrivate":true,"avatarUrl":"/api/public/channels/)" + kId + R"(/avatar?v=1760781600000"})");
    const auto found = api::channels::get(h.api, kId, kAccess);
    CHECK(found.isPrivate);
    CHECK(found.avatarUrl == "/api/public/channels/" + kId + "/avatar?v=1760781600000");
    CHECK_FALSE(found.secretSealed);
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "x-channel-access") == kAccess);
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie").empty());

    h.transport->enqueue(200, R"([{"id":"p1","body":"Hello","createdAt":"2026-10-18T09:00:00.000Z","views":1234}])");
    const auto posts = api::channels::posts(h.api, kId, std::nullopt, kAccess);
    REQUIRE(posts.size() == 1);
    CHECK(posts[0].views == 1234);
    CHECK(h.transport->last().url == "https://example.com/api/public/channels/" + kId + "/posts?limit=30");
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "x-channel-access") == kAccess);
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie").empty());

    // A public channel's reads send no token at all.
    h.transport->enqueue(200, channelJson(kId));
    const auto open = api::channels::get(h.api, kId);
    CHECK_FALSE(open.isPrivate);
    CHECK_FALSE(open.avatarUrl);
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "x-channel-access").empty());

    // The photo: fetched without the cookie, with the token, and only from the URL shape the server hands out.
    h.transport->enqueue(200, "JPEGBYTES", {{"content-type", "image/jpeg"}});
    CHECK(api::channels::avatar(h.api, "/api/public/channels/" + kId + "/avatar?v=1760781600000", kAccess) == "JPEGBYTES");
    CHECK(h.transport->last().method == "GET");
    CHECK(h.transport->last().url == "https://example.com/api/public/channels/" + kId + "/avatar?v=1760781600000");
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "x-channel-access") == kAccess);
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie").empty());
    const auto requestsBefore = h.transport->requests.size();
    for (const std::string& bad : std::vector<std::string>{"https://evil.example/x.jpg", "/api/users/" + kId + "/avatar", "/api/public/channels/" + kId + "/avatar?v=1&x=2",
                                  "/api/public/channels/../../users/avatar", "/api/public/channels/" + kId + "/avatar/../posts"}) {
        INFO(bad);
        CHECK_THROWS_AS(api::channels::avatar(h.api, bad), api::ApiError);
    }
    CHECK(h.transport->requests.size() == requestsBefore);

    // Views: post ids only, no cookie, no token.
    h.transport->enqueue(204, "");
    api::channels::recordViews(h.api, {"aaaaaaaa-0000-4000-8000-000000000001", "aaaaaaaa-0000-4000-8000-000000000002"});
    CHECK(h.transport->last().method == "POST");
    CHECK(h.transport->last().url == "https://example.com/api/public/channels/views");
    CHECK(Json::parse(h.transport->last().body) ==
          Json{{"postIds", {"aaaaaaaa-0000-4000-8000-000000000001", "aaaaaaaa-0000-4000-8000-000000000002"}}});
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie").empty());
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "x-channel-access").empty());
    // Nothing to report sends nothing; more than the server takes at once is refused here.
    const auto sent = h.transport->requests.size();
    api::channels::recordViews(h.api, {});
    CHECK_THROWS_AS(api::channels::recordViews(h.api, std::vector<std::string>(api::channels::kMaxViewsPerReport + 1, "x")), std::invalid_argument);
    CHECK(h.transport->requests.size() == sent);
}

TEST_CASE("the owner's photo, and public or private", "[channels][api]") {
    Harness h;
    const std::string own = R"({"id":")" + kId + R"(","name":"News","description":"","createdAt":"2026-10-01T00:00:00.000Z",)"
                            R"("lastPostAt":null,"isPrivate":true,"avatarUrl":null,"secretSealed":"c2VhbGVk"})";
    h.transport->enqueue(200, "[" + own + "]");
    const auto mine = api::channels::mine(h.api);
    REQUIRE(mine.size() == 1);
    CHECK(mine[0].secretSealed == "c2VhbGVk");

    h.transport->enqueue(200, own);
    CHECK(api::channels::setPrivate(h.api, kId, kAccess, "c2VhbGVk").isPrivate);
    CHECK(h.transport->last().method == "PUT");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId + "/visibility");
    CHECK(Json::parse(h.transport->last().body) == Json{{"isPrivate", true}, {"access", kAccess}, {"secretSealed", "c2VhbGVk"}});
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie") == "sid=abc");

    h.transport->enqueue(200, channelJson(kId));
    CHECK_FALSE(api::channels::setPublic(h.api, kId).isPrivate);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId + "/visibility");
    CHECK(Json::parse(h.transport->last().body) == Json{{"isPrivate", false}});

    h.transport->enqueue(200, channelJson(kId));
    api::channels::setAvatar(h.api, kId, "/9j/4AAQ");
    CHECK(h.transport->last().method == "PUT");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId + "/avatar");
    CHECK(Json::parse(h.transport->last().body) == Json{{"image", "/9j/4AAQ"}});
    CHECK(test::FakeTransport::headerOf(h.transport->last(), "cookie") == "sid=abc");

    h.transport->enqueue(204, "");
    api::channels::removeAvatar(h.api, kId);
    CHECK(h.transport->last().method == "DELETE");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kId + "/avatar");
}
