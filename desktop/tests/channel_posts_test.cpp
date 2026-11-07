// Channels part three: richer posts. The API calls go out as the web app
// sends them (ui/src/lib/api.ts), posts parse in every shape the server
// sends (server/src/routes/channelRoutes.ts postDto), uploads stream from
// disk with the cookie, media links are fetched without it, and what this
// computer remembers (votes, podcast positions, which new posts were
// notified) follows ui/src/lib/channelPosts.ts and channelWatch.ts.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/channel_api.hpp"
#include "redvault/model/channel_posts.hpp"
#include "redvault/model/channels.hpp"

using namespace redvault;
namespace channels = redvault::api::channels;
namespace posts = redvault::model::channel_posts;
namespace follows = redvault::model::channels;
using Json = nlohmann::json;

namespace {

const std::string kChannel = "6f1e2d3c-4b5a-4968-8776-655443322110";
const std::string kPost = "aaaaaaaa-0000-4000-8000-000000000001";
const std::string kMedia = "bbbbbbbb-1111-4111-8111-000000000002";
const std::string kSig = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJ0123456";  // 43 base64url characters
const std::string kMediaUrl = "/api/channel-media/" + kMedia + "?exp=1792354770&sig=" + kSig;
const std::string kAccess = "KQ5oUWp7EuQY/qlUwD5APFsYvK80h+szX5tNzQAxJlk=";

struct Harness {
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::shared_ptr<net::SessionCookieJar> cookies = std::make_shared<net::SessionCookieJar>();
    api::ApiClient api{"https://example.com/api/", transport, cookies};

    Harness() { cookies->saveFromResponse(*net::parseUrl("https://example.com/api/auth/login"), {"sid=abc; Path=/"}); }

    std::string cookie() const { return test::FakeTransport::headerOf(transport->last(), "cookie"); }
    std::string header(const std::string& name) const { return test::FakeTransport::headerOf(transport->last(), name); }
    Json body() const { return Json::parse(transport->last().body); }
};

/// A post as the server's postDto writes it.
Json postJson(const Json& extra) {
    Json post{{"id", kPost}, {"body", ""}, {"createdAt", "2026-10-19T09:00:00.000Z"}, {"views", 3}, {"suggested", false},
              {"poll", nullptr}, {"checklist", nullptr}, {"media", Json::array()}};
    for (const auto& [key, value] : extra.items()) post[key] = value;
    return post;
}

Json mediaJson(const char* kind, const Json& extra = Json::object()) {
    Json m{{"id", kMedia}, {"kind", kind}, {"mime", "image/jpeg"}, {"size", 12345}, {"filename", "beach.jpg"},
           {"durationMs", nullptr}, {"width", 4032}, {"height", 3024}, {"url", kMediaUrl}};
    for (const auto& [key, value] : extra.items()) m[key] = value;
    return m;
}

channels::Channel followedChannel(std::optional<std::string> lastPostAt) {
    channels::Channel c;
    c.id = kChannel;
    c.name = "News";
    c.lastPostAt = std::move(lastPostAt);
    return c;
}

}  // namespace

// --- Post shapes ---

TEST_CASE("posts parse in every shape: text, media, a poll, a quiz, a checklist", "[channels][posts]") {
    Harness h;
    const Json page = Json::array({
        postJson({{"body", "*Big news* _today_"}, {"suggested", true}}),
        postJson({{"media", Json::array({mediaJson("photo"), mediaJson("voice", {{"mime", "audio/ogg"}, {"filename", nullptr},
                                                                                  {"durationMs", 8300}, {"width", nullptr}, {"height", nullptr}})})}}),
        postJson({{"poll", {{"question", "Next feature?"}, {"options", {"Stories", "Bots"}}, {"multiple", true}, {"quiz", false},
                            {"closed", false}, {"counts", {2, 1}}, {"voters", 3}, {"correct", nullptr}, {"explanation", nullptr}}}}),
        postJson({{"poll", {{"question", "2+2?"}, {"options", {"3", "4"}}, {"multiple", false}, {"quiz", true}, {"closed", true},
                            {"counts", {1, 4}}, {"voters", 5}, {"correct", 1}, {"explanation", "Arithmetic."}}}}),
        postJson({{"checklist", {{"title", "Launch"}, {"items", Json::array({{{"text", "Docs"}, {"done", true}}, {{"text", "Ship"}, {"done", false}}})},
                                 {"collaborative", true}}}}),
        // An older server's post: no part-three fields at all.
        Json{{"id", kPost}, {"body", "Hello"}, {"createdAt", "2026-10-01T00:00:00.000Z"}, {"views", 0}},
    });
    h.transport->enqueue(200, page.dump());
    const auto list = channels::posts(h.api, kChannel, std::nullopt, kAccess);
    REQUIRE(list.size() == 6);
    CHECK(h.header("x-channel-access") == kAccess);
    CHECK(h.cookie().empty());

    CHECK(list[0].body == "*Big news* _today_");
    CHECK(list[0].suggested);
    CHECK_FALSE(list[0].poll);
    CHECK(list[0].media.empty());

    REQUIRE(list[1].media.size() == 2);
    CHECK(list[1].media[0] == channels::Media{kMedia, "photo", "image/jpeg", 12345, "beach.jpg", std::nullopt, 4032, 3024, kMediaUrl});
    CHECK(list[1].media[1].kind == "voice");
    CHECK_FALSE(list[1].media[1].filename);
    CHECK(list[1].media[1].durationMs == 8300);
    CHECK_FALSE(list[1].media[1].width);

    REQUIRE(list[2].poll);
    CHECK(list[2].poll->multiple);
    CHECK_FALSE(list[2].poll->quiz);
    CHECK(list[2].poll->counts == std::vector<int>{2, 1});
    CHECK(list[2].poll->voters == 3);
    CHECK_FALSE(list[2].poll->correct);

    REQUIRE(list[3].poll);
    CHECK(list[3].poll->quiz);
    CHECK(list[3].poll->closed);
    CHECK(list[3].poll->correct == 1);
    CHECK(list[3].poll->explanation == "Arithmetic.");

    REQUIRE(list[4].checklist);
    CHECK(*list[4].checklist == channels::Checklist{"Launch", {{"Docs", true}, {"Ship", false}}, true});

    CHECK(list[5].body == "Hello");
    CHECK_FALSE(list[5].suggested);
    CHECK(list[5].media.empty());
}

TEST_CASE("a channel says whether it takes suggested posts", "[channels][posts]") {
    Harness h;
    h.transport->enqueue(200, R"({"id":")" + kChannel + R"(","name":"News","description":"","createdAt":"2026-10-01T00:00:00.000Z",)"
                                   R"("lastPostAt":null,"isPrivate":false,"acceptSuggestions":true,"avatarUrl":null})");
    CHECK(channels::get(h.api, kChannel).acceptSuggestions);
    h.transport->enqueue(200, R"({"id":")" + kChannel + R"(","name":"News","description":"","createdAt":"2026-10-01T00:00:00.000Z","lastPostAt":null})");
    CHECK_FALSE(channels::get(h.api, kChannel).acceptSuggestions);

    h.transport->enqueue(200, R"({"id":")" + kChannel + R"(","name":"News","acceptSuggestions":true,"secretSealed":null})");
    CHECK(channels::setAcceptSuggestions(h.api, kChannel, true).acceptSuggestions);
    CHECK(h.transport->last().method == "PUT");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/settings");
    CHECK(h.body() == Json{{"acceptSuggestions", true}});
    CHECK(h.cookie() == "sid=abc");
}

TEST_CASE("posting sends what the web app's composer sends", "[channels][posts]") {
    Harness h;
    const auto posted = [&](const channels::NewPost& post) {
        h.transport->enqueue(201, postJson({}).dump());
        channels::post(h.api, kChannel, post);
        CHECK(h.transport->last().method == "POST");
        CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/posts");
        CHECK(h.cookie() == "sid=abc");
        return h.body();
    };

    CHECK(posted({"Hello *world*", std::nullopt, std::nullopt, {}, std::nullopt}) == Json{{"body", "Hello *world*"}});
    // The old text-only call is the same post.
    h.transport->enqueue(201, postJson({}).dump());
    channels::post(h.api, kChannel, std::string("Hi"));
    CHECK(h.body() == Json{{"body", "Hi"}});

    channels::NewPost poll{"", channels::NewPost::NewPoll{"Next feature?", {"Stories", "Bots"}, true, std::nullopt}, std::nullopt, {}, std::nullopt};
    CHECK(posted(poll) == Json{{"body", ""}, {"poll", {{"question", "Next feature?"}, {"options", {"Stories", "Bots"}}, {"multiple", true}, {"quiz", nullptr}}}});

    channels::NewPost quiz{"", channels::NewPost::NewPoll{"2+2?", {"3", "4"}, false, channels::NewPost::NewPoll::Quiz{1, "Arithmetic."}},
                           std::nullopt, {}, "2026-12-24T18:30:00.000Z"};
    CHECK(posted(quiz) == Json{{"body", ""},
                               {"poll", {{"question", "2+2?"}, {"options", {"3", "4"}}, {"multiple", false}, {"quiz", {{"correct", 1}, {"explanation", "Arithmetic."}}}}},
                               {"publishAt", "2026-12-24T18:30:00.000Z"}});

    channels::NewPost checklist{"Before launch", std::nullopt, channels::NewPost::NewChecklist{"Launch", {"Docs", "Ship"}, true}, {}, std::nullopt};
    CHECK(posted(checklist) == Json{{"body", "Before launch"}, {"checklist", {{"title", "Launch"}, {"items", {"Docs", "Ship"}}, {"collaborative", true}}}});

    channels::NewPost media{"Photos", std::nullopt, std::nullopt, {kMedia, "cccccccc-0000-4000-8000-000000000003"}, std::nullopt};
    CHECK(posted(media) == Json{{"body", "Photos"}, {"media", {kMedia, "cccccccc-0000-4000-8000-000000000003"}}});
}

// --- Uploads and media links ---

TEST_CASE("an upload starts, then streams the file from disk with the cookie", "[channels][posts][uploads]") {
    Harness h;
    h.transport->enqueue(201, R"({"id":")" + kMedia + R"(","kind":"video","size":11,"uploadUrl":"/api/channel-uploads/)" + kMedia + R"("})");
    const auto started = channels::startMedia(h.api, kChannel, {"video", "video/mp4", 11, "clip.mp4", 2500, 1920, 1080});
    CHECK(started.id == kMedia);
    CHECK(started.uploadUrl == "/api/channel-uploads/" + kMedia);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/media");
    CHECK(h.body() == Json{{"kind", "video"}, {"mime", "video/mp4"}, {"size", 11}, {"filename", "clip.mp4"}, {"durationMs", 2500}, {"width", 1920}, {"height", 1080}});
    CHECK(h.cookie() == "sid=abc");

    // A name outside the ANSI code page: on Windows only a wide path opens it.
    const auto path = std::filesystem::temp_directory_path() / std::filesystem::path(u8"redvault-caf\xC3\xA9-\xE5\x86\x99\xE7\x9C\x9F-upload.bin");
    {
        std::ofstream out(path, std::ios::binary);
        out << "hello world";
    }
    std::vector<std::int64_t> progress;
    h.transport->enqueue(200, R"({"id":")" + kMedia + R"(","status":"uploaded"})");
    channels::uploadMedia(h.api, started.uploadUrl, path, 11, [&](std::int64_t sent) { progress.push_back(sent); });
    const auto& put = h.transport->last();
    CHECK(put.method == "PUT");
    CHECK(put.url == "https://example.com/api/channel-uploads/" + kMedia);
    CHECK(put.body == "hello world");
    CHECK(test::FakeTransport::headerOf(put, "content-type") == "application/octet-stream");
    CHECK(test::FakeTransport::headerOf(put, "cookie") == "sid=abc");
    REQUIRE_FALSE(progress.empty());
    CHECK(progress.back() == 11);

    // Only the upload URL the server hands out: the bytes (and the cookie) go nowhere else.
    const auto sent = h.transport->requests.size();
    for (const std::string& bad : std::vector<std::string>{"https://evil.example/upload", "/api/channels/" + kChannel, "/api/channel-uploads/x",
                                                           "/api/channel-uploads/" + kMedia + "/../../users"}) {
        INFO(bad);
        CHECK_THROWS_AS(channels::uploadMedia(h.api, bad, path, 11), api::ApiError);
    }
    CHECK(h.transport->requests.size() == sent);
    std::filesystem::remove(path);
}

TEST_CASE("a follower's suggestion photos start with the private link's token", "[channels][posts][uploads]") {
    Harness h;
    h.transport->enqueue(201, R"({"id":")" + kMedia + R"(","kind":"photo","size":5,"uploadUrl":"/api/channel-uploads/)" + kMedia + R"("})");
    channels::startSuggestionMedia(h.api, kChannel, {"photo", "image/png", 5, "art.png", std::nullopt, 640, 480}, kAccess);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/suggestions/media");
    CHECK(h.header("x-channel-access") == kAccess);
    CHECK(h.cookie() == "sid=abc");
    CHECK(h.body()["durationMs"].is_null());
}

TEST_CASE("media links are fetched without the cookie, whole or a range at a time", "[channels][posts][privacy]") {
    Harness h;
    CHECK(channels::isMediaUrl(kMediaUrl));
    h.transport->enqueue(200, "JPEG");
    CHECK(channels::fetchMedia(h.api, kMediaUrl) == "JPEG");
    CHECK(h.transport->last().url == "https://example.com" + kMediaUrl);
    CHECK(h.cookie().empty());

    h.transport->enqueue(206, "byte");
    CHECK(channels::fetchMediaRange(h.api, kMediaUrl, 100, 103) == "byte");
    CHECK(h.header("range") == "bytes=100-103");
    CHECK(h.cookie().empty());

    h.transport->enqueue(200, "the whole file");
    std::ostringstream sink;
    channels::downloadMedia(h.api, kMediaUrl, sink);
    CHECK(sink.str() == "the whole file");
    CHECK(h.cookie().empty());

    // Expired links say so.
    h.transport->enqueue(410, R"({"error":"media_link_expired","message":"This link has expired. Reload the channel."})");
    CHECK_THROWS_AS(channels::fetchMedia(h.api, kMediaUrl), api::ApiError);

    const auto sent = h.transport->requests.size();
    for (const std::string& bad : std::vector<std::string>{"https://evil.example/a.jpg", "/api/channel-media/" + kMedia,
                                                           "/api/channel-media/" + kMedia + "?exp=1&sig=short",
                                                           "/api/channel-media/" + kMedia + "?exp=1&sig=" + kSig + "&x=1",
                                                           "/api/users/" + kMedia + "/avatar"}) {
        INFO(bad);
        CHECK_FALSE(channels::isMediaUrl(bad));
        CHECK_THROWS_AS(channels::fetchMedia(h.api, bad), api::ApiError);
    }
    CHECK(h.transport->requests.size() == sent);
}

// --- Polls, checklists, scheduling, suggestions ---

TEST_CASE("voting: one vote per account, a quiz answer shows the right one", "[channels][posts][polls]") {
    Harness h;
    h.transport->enqueue(200, R"({"poll":{"question":"2+2?","options":["3","4"],"multiple":false,"quiz":true,"closed":false,)"
                              R"("counts":[0,1],"voters":1,"correct":1,"explanation":"Arithmetic."},"mine":[1]})");
    const auto result = channels::vote(h.api, kChannel, kPost, {1}, kAccess);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/posts/" + kPost + "/vote");
    CHECK(h.body() == Json{{"options", {1}}});
    CHECK(h.header("x-channel-access") == kAccess);
    CHECK(h.cookie() == "sid=abc");
    CHECK(result.mine == std::vector<int>{1});
    CHECK(result.poll.correct == 1);
    CHECK(result.poll.explanation == "Arithmetic.");

    // Taking a poll vote back is an empty list.
    h.transport->enqueue(200, R"({"poll":{"question":"Q","options":["a","b"],"counts":[0,0],"voters":0},"mine":[]})");
    CHECK(channels::vote(h.api, kChannel, kPost, {}).mine.empty());
    CHECK(h.body() == Json{{"options", Json::array()}});
    CHECK(h.header("x-channel-access").empty());

    h.transport->enqueue(409, R"({"error":"already_answered","message":"You've already answered this quiz."})");
    try {
        channels::vote(h.api, kChannel, kPost, {0});
        FAIL("expected an error");
    } catch (const api::ApiError& e) {
        CHECK(e.code() == "already_answered");
    }

    h.transport->enqueue(200, postJson({{"poll", {{"question", "Q"}, {"options", {"a", "b"}}, {"closed", true}, {"counts", {1, 0}}, {"voters", 1}}}}).dump());
    CHECK(channels::closePoll(h.api, kChannel, kPost).poll->closed);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/posts/" + kPost + "/close");
}

TEST_CASE("checklists, scheduled posts and suggestions", "[channels][posts]") {
    Harness h;
    h.transport->enqueue(200, R"({"title":"Launch","items":[{"text":"Docs","done":true}],"collaborative":true})");
    const auto checklist = channels::setChecklistItem(h.api, kChannel, kPost, 0, true, kAccess);
    CHECK(h.transport->last().method == "PUT");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/posts/" + kPost + "/checklist/0");
    CHECK(h.body() == Json{{"done", true}});
    CHECK(h.header("x-channel-access") == kAccess);
    CHECK(checklist.items.front().done);

    h.transport->enqueue(200, Json::array({postJson({{"body", "Later"}, {"createdAt", "2026-12-24T18:30:00.000Z"}})}).dump());
    const auto later = channels::scheduled(h.api, kChannel);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/scheduled");
    REQUIRE(later.size() == 1);
    CHECK(later[0].createdAt == "2026-12-24T18:30:00.000Z");
    h.transport->enqueue(200, postJson({}).dump());
    channels::publishNow(h.api, kChannel, kPost);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/posts/" + kPost + "/publish");

    const std::string suggestion = R"({"id":"s1","body":"Fan art","status":"pending","createdAt":"2026-10-19T09:00:00.000Z","media":[)" +
                                   mediaJson("photo").dump() + "]}";
    h.transport->enqueue(201, suggestion);
    const auto sent = channels::suggest(h.api, kChannel, "Fan art", {kMedia}, kAccess);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/suggestions");
    CHECK(h.body() == Json{{"body", "Fan art"}, {"media", {kMedia}}});
    CHECK(h.header("x-channel-access") == kAccess);
    CHECK(sent.status == "pending");
    REQUIRE(sent.media.size() == 1);

    h.transport->enqueue(200, "[" + suggestion + "]");
    CHECK(channels::mySuggestions(h.api, kChannel).size() == 1);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/suggestions/mine");
    h.transport->enqueue(200, "[" + suggestion + "]");
    CHECK(channels::suggestions(h.api, kChannel).size() == 1);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/suggestions");

    h.transport->enqueue(201, postJson({{"suggested", true}}).dump());
    CHECK(channels::approveSuggestion(h.api, kChannel, "s1", std::string("Fan art!"), std::nullopt).suggested);
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/suggestions/s1/approve");
    CHECK(h.body() == Json{{"body", "Fan art!"}, {"publishAt", nullptr}});
    h.transport->enqueue(201, postJson({{"suggested", true}}).dump());
    channels::approveSuggestion(h.api, kChannel, "s1", std::nullopt, std::string("2026-12-24T18:30:00.000Z"));
    CHECK(h.body() == Json{{"publishAt", "2026-12-24T18:30:00.000Z"}});

    h.transport->enqueue(204, "");
    channels::rejectSuggestion(h.api, kChannel, "s1");
    CHECK(h.transport->last().method == "POST");
    CHECK(h.transport->last().url == "https://example.com/api/channels/" + kChannel + "/suggestions/s1/reject");
}

// --- What this computer remembers, and how things are shown ---

TEST_CASE("files go in as photos, videos, audio or files", "[channels][posts]") {
    CHECK(posts::mediaKindFor("image/jpeg") == "photo");
    CHECK(posts::mediaKindFor("IMAGE/PNG") == "photo");
    CHECK(posts::mediaKindFor("image/heic") == "photo");
    // Not SVG: it can carry script, so it goes as a file.
    CHECK(posts::mediaKindFor("image/svg+xml") == "file");
    CHECK(posts::mediaKindFor("video/mp4") == "video");
    CHECK(posts::mediaKindFor("audio/mpeg") == "audio");
    CHECK(posts::mediaKindFor("application/pdf") == "file");
    CHECK(posts::mediaKindFor("") == "file");
    CHECK(posts::kindLabel("voice") == "Voice clip");
    CHECK(posts::kindLabel("round") == "Round video");
}

TEST_CASE("durations and playback speeds read like the web app's", "[channels][posts]") {
    CHECK(posts::formatDuration(0) == "0:00");
    CHECK(posts::formatDuration(5.9) == "0:05");
    CHECK(posts::formatDuration(185) == "3:05");
    CHECK(posts::formatDuration(3723) == "1:02:03");
    CHECK(posts::formatDuration(-1) == "0:00");
    CHECK(posts::nextSpeed(1) == 1.5);
    CHECK(posts::nextSpeed(1.5) == 2);
    CHECK(posts::nextSpeed(2) == 1);
    CHECK(posts::nextSpeed(3) == 1);
}

TEST_CASE("this computer remembers your votes, and a quiz's answer as you were shown it", "[channels][posts][polls]") {
    std::string json;
    CHECK_FALSE(posts::rememberedVote(json, kPost));
    json = posts::rememberVote(json, kPost, posts::RememberedVote{{0, 2}, std::nullopt, std::nullopt});
    CHECK(posts::rememberedVote(json, kPost) == posts::RememberedVote{{0, 2}, std::nullopt, std::nullopt});
    CHECK(Json::parse(json) == Json::parse(R"({")" + kPost + R"(":{"mine":[0,2]}})"));

    channels::Poll quiz;
    quiz.quiz = true;
    quiz.correct = 1;
    quiz.explanation = "Arithmetic.";
    json = posts::rememberVote(json, "quiz-post", posts::RememberedVote{{0}, std::nullopt, std::nullopt}, quiz);
    CHECK(posts::rememberedVote(json, "quiz-post") == posts::RememberedVote{{0}, 1, "Arithmetic."});
    CHECK(Json::parse(json)["quiz-post"] == Json{{"mine", {0}}, {"correct", 1}, {"explanation", "Arithmetic."}});

    // A vote taken back is forgotten.
    json = posts::rememberVote(json, kPost, posts::RememberedVote{{}, std::nullopt, std::nullopt});
    CHECK_FALSE(posts::rememberedVote(json, kPost));
    json = posts::rememberVote(json, "quiz-post", std::nullopt);
    CHECK(Json::parse(json).empty());
    // Anything unreadable is nothing remembered.
    CHECK_FALSE(posts::rememberedVote("not json", kPost));

    // The most recent ones are kept.
    std::string many;
    for (std::size_t i = 0; i < posts::kMaxRememberedVotes + 3; ++i) {
        many = posts::rememberVote(many, "p" + std::to_string(i), posts::RememberedVote{{0}, std::nullopt, std::nullopt});
    }
    CHECK(Json::parse(many).size() == posts::kMaxRememberedVotes);
    CHECK_FALSE(posts::rememberedVote(many, "p0"));
    CHECK(posts::rememberedVote(many, "p3"));
}

TEST_CASE("a podcast resumes where you stopped", "[channels][posts]") {
    std::string json = posts::savePosition("", kMedia, 754.6);
    CHECK(posts::savedPosition(json, kMedia) == 755);
    CHECK(posts::savedPosition(json, "other") == 0);
    // Finished: forgotten.
    json = posts::savePosition(json, kMedia, 0);
    CHECK(posts::savedPosition(json, kMedia) == 0);
    CHECK(Json::parse(json).empty());
    std::string many;
    for (std::size_t i = 0; i < posts::kMaxRememberedPositions + 1; ++i) many = posts::savePosition(many, "m" + std::to_string(i), 10);
    CHECK(Json::parse(many).size() == posts::kMaxRememberedPositions);
    CHECK(posts::savedPosition(many, "m0") == 0);
}

TEST_CASE("poll results in whole percent", "[channels][posts][polls]") {
    channels::Poll poll;
    poll.options = {"a", "b", "c"};
    poll.counts = {1, 1, 1};
    poll.voters = 3;
    CHECK(posts::percents(poll) == std::vector<int>{33, 33, 33});
    poll.counts = {1, 0, 1};
    poll.voters = 2;
    CHECK(posts::percents(poll) == std::vector<int>{50, 0, 50});
    // Math.round: halves go up (1/8 = 12.5%).
    poll.options = {"a", "b"};
    poll.counts = {1, 7};
    poll.voters = 8;
    CHECK(posts::percents(poll) == std::vector<int>{13, 88});
    poll.voters = 0;
    CHECK(posts::percents(poll) == std::vector<int>{0, 0});
}

TEST_CASE("a notification says what's new: the text, or what the post carries", "[channels][posts][watch]") {
    CHECK(posts::postPreview(nullptr) == "New post");
    channels::Post post;
    post.body = "*Big news*   _today_\n\nat https://example.com";
    CHECK(posts::postPreview(&post) == "Big news today at https://example.com");
    post.body = std::string(150, 'x');
    CHECK(posts::postPreview(&post) == std::string(139, 'x') + "\xE2\x80\xA6");
    post.body = "";
    post.poll = channels::Poll{"Next feature?", {"a", "b"}, false, false, false, {0, 0}, 0, std::nullopt, std::nullopt};
    CHECK(posts::postPreview(&post) == "Poll: Next feature?");
    post.poll->quiz = true;
    CHECK(posts::postPreview(&post) == "Quiz: Next feature?");
    post.poll.reset();
    post.checklist = channels::Checklist{"Launch", {}, false};
    CHECK(posts::postPreview(&post) == "Checklist: Launch");
    post.checklist.reset();
    for (const auto& [kind, label] : std::vector<std::pair<std::string, std::string>>{
             {"photo", "Photo"}, {"video", "Video"}, {"voice", "Voice message"}, {"round", "Video message"}, {"audio", "Audio"}, {"file", "File"}}) {
        post.media = {channels::Media{kMedia, kind, "", 1, std::nullopt, std::nullopt, std::nullopt, std::nullopt, kMediaUrl}};
        CHECK(posts::postPreview(&post) == label);
    }
    post.media.clear();
    CHECK(posts::postPreview(&post) == "New post");
}

TEST_CASE("followed channels: the badge counts unseen posts, each new post is notified once", "[channels][posts][watch]") {
    std::map<std::string, std::string> notified;
    follows::FollowedChannel followed{kChannel, "News", "2026-10-19T09:00:00.000Z"};

    // Nothing new since you looked.
    CHECK(posts::watch(followed, followedChannel("2026-10-19T09:00:00.000Z"), notified, "") == posts::WatchOutcome{false, false});
    CHECK(notified.empty());

    // A new post: counted and notified, once.
    const auto newer = followedChannel("2026-10-19T10:00:00.000Z");
    CHECK(posts::watch(followed, newer, notified, "") == posts::WatchOutcome{true, true});
    CHECK(notified[kChannel] == "2026-10-19T10:00:00.000Z");
    CHECK(posts::watch(followed, newer, notified, "") == posts::WatchOutcome{true, false});
    // Another one later: notified again.
    CHECK(posts::watch(followed, followedChannel("2026-10-19T11:00:00.000Z"), notified, "") == posts::WatchOutcome{true, true});

    // Muted, or open on screen right now: counted, not notified, and not remembered as notified.
    std::map<std::string, std::string> none;
    followed.muted = true;
    CHECK(posts::watch(followed, newer, none, "") == posts::WatchOutcome{true, false});
    followed.muted = false;
    CHECK(posts::watch(followed, newer, none, kChannel) == posts::WatchOutcome{true, false});
    CHECK(none.empty());

    // Followed before the channel's first post.
    follows::FollowedChannel early{kChannel, "News", std::nullopt};
    CHECK(posts::watch(early, newer, none, "") == posts::WatchOutcome{true, true});
    // A time that doesn't parse never counts as newer (like Date.parse).
    std::map<std::string, std::string> odd{{kChannel, "yesterday"}};
    CHECK(posts::watch(early, newer, odd, "") == posts::WatchOutcome{true, false});

    // Remembered as JSON.
    const auto json = posts::notifiedToJson(notified);
    CHECK(Json::parse(json) == Json{{kChannel, "2026-10-19T11:00:00.000Z"}});
    CHECK(posts::notifiedFromJson(json) == notified);
    CHECK(posts::notifiedFromJson("not json").empty());
    CHECK(posts::notifiedFromJson(R"({"a":7,"b":"2026-10-19T11:00:00.000Z"})") == std::map<std::string, std::string>{{"b", "2026-10-19T11:00:00.000Z"}});
}

TEST_CASE("a followed channel can be muted, and stays muted", "[channels][posts][watch]") {
    channels::Channel c = followedChannel(std::nullopt);
    auto list = follows::follow({}, c);
    CHECK_FALSE(list[0].muted);
    list = follows::setMuted(list, kChannel, true);
    CHECK(list[0].muted);
    const auto json = follows::toJson(list);
    CHECK(Json::parse(json)[0]["muted"] == true);
    CHECK(follows::fromJson(json) == list);
    list = follows::setMuted(list, kChannel, false);
    CHECK_FALSE(Json::parse(follows::toJson(list))[0].contains("muted"));
    // The web app writes "muted": false once toggled back.
    CHECK_FALSE(follows::fromJson(R"([{"id":"a","name":"A","seenAt":null,"muted":false}])")[0].muted);
}
