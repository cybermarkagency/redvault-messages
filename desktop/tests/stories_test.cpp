// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Stories: the crypto and formats are the web app's byte for byte (vectors
// made by its own code: vectors/make_story_vectors.ts), and the API calls
// send what the server expects.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/story_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/model/stories.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using namespace redvault::crypto;
using Json = nlohmann::json;
namespace st = redvault::model::stories;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_STORY_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

Bytes b64(const Json& j) {
    return base64Decode(j.get<std::string>());
}

const std::string kFriend = "00000000-0000-4000-8000-00000000000b";
const std::string kOther = "00000000-0000-4000-8000-00000000000c";

}  // namespace

TEST_CASE("a story key sealed by the web app opens here, for that story and that viewer only", "[stories]") {
    const auto& v = vectors();
    const std::string id = v["storyId"];
    const SecretBytes key = stories::unwrapStoryKey(id, b64(v["wrappedForBob"]), b64(v["alice"]["publicKey"]), b64(v["bob"]["privateKey"]));
    CHECK(Bytes(key.view().begin(), key.view().end()) == b64(v["key"]));
    // Another story's id, another sender, or the wrong viewer: refused.
    CHECK_THROWS_AS(stories::unwrapStoryKey(util::randomUuid(), b64(v["wrappedForBob"]), b64(v["alice"]["publicKey"]), b64(v["bob"]["privateKey"])),
                    CryptoError);
    CHECK_THROWS_AS(stories::unwrapStoryKey(id, b64(v["wrappedForBob"]), b64(v["bob"]["publicKey"]), b64(v["bob"]["privateKey"])), CryptoError);
    CHECK_THROWS_AS(stories::unwrapStoryKey(id, b64(v["wrappedForBob"]), b64(v["alice"]["publicKey"]), b64(v["alice"]["privateKey"])), CryptoError);
}

TEST_CASE("the web app's story details open and parse the same here", "[stories]") {
    const auto& v = vectors();
    const std::string id = v["storyId"];
    const std::string json = stories::openMeta(id, b64(v["key"]), b64(v["metaCiphertext"]), b64(v["metaNonce"]));
    CHECK(json == v["metaJson"].get<std::string>());
    const st::Meta meta = st::parseMeta(json, "image");
    CHECK(meta.closeFriends);
    REQUIRE(meta.overlays.size() == 2);
    CHECK(meta.overlays[0].text == "Golden hour \xF0\x9F\x8C\x85");
    CHECK(meta.overlays[0].color == "yellow");
    CHECK(meta.overlays[0].boxed);
    CHECK(meta.overlays[0].font == "modern");
    CHECK_THAT(meta.overlays[0].scale, Catch::Matchers::WithinAbs(1.2, 1e-9));
    CHECK_THAT(meta.overlays[0].rotation, Catch::Matchers::WithinAbs(356, 1e-9));
    REQUIRE(meta.media);
    CHECK(meta.media->mime == "image/jpeg");
    CHECK(meta.media->encryptedSize == 65553);
    CHECK(meta.media->width == 1080);
    // What this app writes reads back the same (and with the same field names as the web's).
    const st::Meta again = st::parseMeta(st::toJson(meta), "image");
    CHECK(st::toJson(again) == st::toJson(meta));
    CHECK(Json::parse(st::toJson(meta)) == Json::parse(json));
    // Bound to its story: another id, or the wrong key, fails.
    CHECK_THROWS_AS(stories::openMeta(util::randomUuid(), b64(v["key"]), b64(v["metaCiphertext"]), b64(v["metaNonce"])), CryptoError);
    CHECK_THROWS_AS(st::parseMeta(json, "video"), st::StoryFormatError);
}

TEST_CASE("highlight titles and story settings from the web app open here", "[stories]") {
    const auto& v = vectors();
    CHECK(stories::openHighlightTitle(b64(v["titleCiphertext"]), b64(v["titleNonce"]), v["coverId"].get<std::string>(), b64(v["coverKey"])) ==
          v["title"].get<std::string>());
    CHECK(stories::openHighlightTitle(b64(v["titleCiphertext"]), b64(v["titleNonce"]), util::randomUuid(), b64(v["coverKey"])).empty());

    const st::Prefs prefs = st::parsePrefs(stories::openPrefs(b64(v["prefsCiphertext"]), b64(v["prefsNonce"]), b64(v["alice"]["privateKey"])));
    CHECK(prefs.closeFriends == std::vector<std::string>{kFriend});
    CHECK(prefs.hideFrom == std::vector<std::string>{kOther});
    CHECK_FALSE(prefs.shareViews);
    CHECK(prefs.allowReplies);
    CHECK_THROWS_AS(stories::openPrefs(b64(v["prefsCiphertext"]), b64(v["prefsNonce"]), b64(v["bob"]["privateKey"])), CryptoError);
}

TEST_CASE("stories made here open with the same functions (as the web app's do)", "[stories]") {
    const KeyPair author = generateKeyPair();
    const KeyPair viewer = generateKeyPair();
    const std::string id = util::randomUuid();
    const SecretBytes key = stories::newStoryKey();
    const Bytes wrapped = stories::wrapStoryKey(id, key, viewer.publicKey, author.privateKey);
    CHECK(wrapped.size() == stories::kWrappedKeyBytes);
    const SecretBytes opened = stories::unwrapStoryKey(id, wrapped, author.publicKey, viewer.privateKey);
    CHECK(Bytes(opened.view().begin(), opened.view().end()) == Bytes(key.view().begin(), key.view().end()));

    st::Meta meta;
    meta.kind = "text";
    meta.text = "Hello";
    meta.background = "ocean";
    meta.overlays.push_back({"emoji", "\xF0\x9F\x94\xA5", 0.5, 0.8, 1, 0, "white", false, "classic"});
    const auto sealed = stories::sealMeta(id, key, st::toJson(meta));
    const st::Meta back = st::parseMeta(stories::openMeta(id, key, sealed.ciphertext, sealed.nonce), "text");
    CHECK(back.text == "Hello");
    CHECK(back.background == "ocean");
    CHECK(back.overlays.size() == 1);

    const auto title = stories::sealHighlightTitle("   A very long highlight name that goes on and on and on   ", id, key);
    CHECK(stories::openHighlightTitle(title.ciphertext, title.nonce, id, key) == "A very long highlight name that goes on ");

    st::Prefs prefs;
    prefs.muted = {kOther};
    const auto sealedPrefs = stories::sealPrefs(st::toJson(prefs), author.privateKey);
    CHECK(st::parsePrefs(stories::openPrefs(sealedPrefs.ciphertext, sealedPrefs.nonce, author.privateKey)).muted == prefs.muted);
}

TEST_CASE("malformed story details are refused, odd values fall back safely", "[stories]") {
    // Not JSON, wrong version, empty text, overlay off the frame.
    CHECK_THROWS_AS(st::parseMeta("nope", "text"), st::StoryFormatError);
    CHECK_THROWS_AS(st::parseMeta(R"({"v":2,"kind":"text","text":"hi","overlays":[]})", "text"), st::StoryFormatError);
    CHECK_THROWS_AS(st::parseMeta(R"({"v":1,"kind":"text","text":"   ","overlays":[]})", "text"), st::StoryFormatError);
    CHECK_THROWS_AS(st::parseMeta(R"({"v":1,"kind":"text","text":"hi","overlays":[{"type":"text","text":"x","x":2,"y":0.5}]})", "text"),
                    st::StoryFormatError);
    // A page as "media", or a video without its length.
    const std::string media = R"("key":"AAAA","header":"AAAA","sha256":")" + std::string(64, 'a') + R"(","encryptedSize":10,"chunkSize":65536,"width":1,"height":1)";
    CHECK_THROWS_AS(st::parseMeta(R"({"v":1,"kind":"image","overlays":[],"media":{)" + media + R"(,"mime":"text/html"}})", "image"), st::StoryFormatError);
    CHECK_THROWS_AS(st::parseMeta(R"({"v":1,"kind":"video","overlays":[],"media":{)" + media + R"(,"mime":"video/mp4"}})", "video"), st::StoryFormatError);
    CHECK(st::parseMeta(R"({"v":1,"kind":"video","overlays":[],"media":{)" + media + R"(,"mime":"video/mp4","durationMs":4200}})", "video")
              .media->durationMs == 4200);
    // Unknown colours, fonts and backgrounds become the defaults; scale and rotation are kept in range.
    const st::Meta m = st::parseMeta(
        R"j({"v":1,"kind":"text","text":"hi","background":"url(x)","font":"comic","overlays":[{"type":"text","text":"x","x":0.1,"y":0.9,"scale":99,"rotation":-30,"color":"#fff","font":"comic"}]})j",
        "text");
    CHECK(m.background == "sunset");
    CHECK(m.font == "classic");
    CHECK(m.overlays[0].color == "white");
    CHECK(m.overlays[0].scale == 4);
    CHECK(m.overlays[0].rotation == 330);
}

TEST_CASE("who gets a story, ages and previews", "[stories]") {
    st::Prefs prefs;
    prefs.hideFrom = {kOther};
    prefs.closeFriends = {kFriend};
    const std::vector<std::string> friends = {kFriend, kOther, "00000000-0000-4000-8000-00000000000d"};
    CHECK(st::recipients(friends, prefs, false) == std::vector<std::string>{kFriend, "00000000-0000-4000-8000-00000000000d"});
    CHECK(st::recipients(friends, prefs, true) == std::vector<std::string>{kFriend});
    const std::int64_t now = 1'800'000'000'000;
    CHECK(st::age(now - 10'000, now) == "now");
    CHECK(st::age(now - 12 * 60'000, now) == "12m");
    CHECK(st::age(now - 3 * 3'600'000, now) == "3h");
    CHECK(st::age(now - 50 * 3'600'000ll, now) == "2d");
    st::Meta photo;
    photo.kind = "image";
    CHECK(st::preview(photo) == "Photo");
    // Seen on this device: kept for two days.
    const auto seen = st::parseSeen(st::toJson(std::vector<st::SeenEntry>{{"a", now - 1000}, {"b", now - 49 * 3'600'000ll}}), now);
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].storyId == "a");
}

TEST_CASE("a story reply is written exactly as the web app writes it, and read back", "[stories]") {
    const auto& v = vectors();
    const model::StoryReplyRef ref{"00000000-0000-4000-8000-00000000000A", v["storyId"].get<std::string>(), "image", "Photo", false};
    const std::string body = model::rich_message::serializeStoryReply("Love this \xF0\x9F\x98\x8D", ref);
    CHECK(body == model::rich_message::kTextPrefix + v["storyReplyBody"].get<std::string>());
    const auto parsed = model::rich_message::parse(body);
    const auto* text = std::get_if<model::parsed::Text>(&parsed);
    REQUIRE(text);
    REQUIRE(text->storyReply);
    CHECK(text->storyReply->authorId == "00000000-0000-4000-8000-00000000000a");
    CHECK(text->storyReply->kind == "image");
    // A kind that isn't one: refused.
    std::string tampered = body;
    tampered.replace(tampered.find("\"image\""), 7, "\"pdf\"");
    CHECK(std::holds_alternative<model::parsed::Invalid>(model::rich_message::parse(tampered)));
}

TEST_CASE("the stories API sends what the server expects", "[stories]") {
    auto transport = std::make_shared<test::FakeTransport>();
    auto cookies = std::make_shared<net::SessionCookieJar>();
    api::ApiClient api{"https://example.com/api/", transport, cookies};

    transport->enqueue(201, R"({"id":"x","published":false,"expiresAt":"2026-10-20T00:00:00.000Z"})");
    api::stories::NewStory s{"5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6", "image", "Y3Q=", "bm9uY2U=", 2048, "close_friends", false, {{kFriend, "a2V5"}}};
    CHECK_FALSE(api::stories::create(api, s));
    CHECK(transport->last().method == "POST");
    CHECK(transport->last().url == "https://example.com/api/stories");
    const Json sent = Json::parse(transport->last().body);
    CHECK(sent["mediaSize"] == 2048);
    CHECK(sent["audience"] == "close_friends");
    CHECK(sent["allowReplies"] == false);
    CHECK(sent["keys"][0]["userId"] == kFriend);

    // Media goes up as raw bytes.
    transport->enqueue(200, R"({"id":"x","published":true})");
    api::stories::uploadMedia(api, s.id, std::string("\x00\x01\x02", 3));
    CHECK(transport->last().method == "PUT");
    CHECK(transport->last().url == "https://example.com/api/stories/5c1d0f7e-2b3a-4c8d-9e6f-a1b2c3d4e5f6/media");
    CHECK(test::FakeTransport::headerOf(transport->last(), "content-type") == "application/octet-stream");
    CHECK(transport->last().body == std::string("\x00\x01\x02", 3));

    transport->enqueue(200, std::string("\xff\xfe", 2));
    CHECK(api::stories::downloadMedia(api, s.id) == std::string("\xff\xfe", 2));

    transport->enqueue(200, R"({"own":[{"id":"a","authorId":"me","kind":"text","metaCiphertext":"Yg==","metaNonce":"bg==","mediaSize":null,"hasMedia":false,"allowReplies":true,"createdAt":"t","expiresAt":"u","wrappedKey":"aw==","viewCount":3}],
                               "authors":[{"authorId":"bob","stories":[{"id":"b","authorId":"bob","kind":"image","metaCiphertext":"Yg==","metaNonce":"bg==","mediaSize":99,"hasMedia":true,"allowReplies":false,"createdAt":"t","expiresAt":"u","wrappedKey":"aw==","seen":true}]}]})");
    const auto feed = api::stories::feed(api);
    CHECK(feed.own[0].viewCount == 3);
    CHECK_FALSE(feed.own[0].mediaSize);
    CHECK(feed.authors[0].stories[0].seen);
    CHECK(*feed.authors[0].stories[0].mediaSize == 99);

    transport->enqueue(200, R"({"removed":2})");
    CHECK(api::stories::revoke(api, kOther, std::vector<std::string>{"a", "b"}) == 2);
    CHECK(Json::parse(transport->last().body) == Json{{"userId", kOther}, {"storyIds", {"a", "b"}}});

    transport->enqueue(409, R"({"error":"prefs_conflict","message":"Someone else saved first"})");
    CHECK_THROWS_AS(api::stories::putPrefs(api, "Y3Q=", "bm9uY2U=", true, 4), api::ApiError);
    CHECK(Json::parse(transport->last().body)["revision"] == 4);
}
