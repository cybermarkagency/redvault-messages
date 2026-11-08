// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Interactive story stickers: parsing, sealed answers and sealed totals are the web app's byte for
// byte (vectors made by its own code: vectors/make_story_interactive_vectors.ts), and everything a
// malicious server or sender could change is refused.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/story_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/model/stories.hpp"
#include "redvault/model/story_stickers.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using namespace redvault::crypto;
using Json = nlohmann::json;
namespace st = redvault::model::stories;
namespace sc = redvault::crypto::stories;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_STORY_INTERACTIVE_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

Bytes b64(const Json& j) {
    return base64Decode(j.get<std::string>());
}

const std::string& storyId() {
    static const std::string id = vectors()["storyId"].get<std::string>();
    return id;
}

st::Sticker stickerFromJson(const Json& j) {
    const auto parsed = st::parseStickers(Json::array({j}).dump());
    REQUIRE(parsed.size() == 1);
    return parsed.front();
}

st::Sticker rawSticker(const char* name) {
    return stickerFromJson(vectors()["stickers"]["raw"][name]);
}

st::Answer pollAnswer(int choice) {
    st::Answer a;
    a.kind = "poll";
    a.choice = choice;
    return a;
}
st::Answer sliderAnswer(int value) {
    st::Answer a;
    a.kind = "slider";
    a.value = value;
    return a;
}
st::Answer textAnswer(const std::string& text) {
    st::Answer a;
    a.kind = "question";
    a.text = text;
    return a;
}

/// Compares what the web app made with what this app made, as JSON (key order and 1 versus 1.0 don't matter).
bool sameJson(const std::string& mine, const Json& theirs) {
    return Json::parse(mine) == theirs;
}

Json answerJson(const st::Answer& a) {
    return Json::parse(st::toJson(a));
}

}  // namespace

// --- Stickers -------------------------------------------------------------------------------------

TEST_CASE("stickers parse exactly as the web app's parser does", "[stories][stickers]") {
    for (const auto& c : vectors()["stickers"]["parseCases"]) {
        INFO(c["name"].get<std::string>());
        const auto parsed = st::parseStickers(c["raw"].dump());
        CHECK(sameJson(st::toJson(parsed), c["expected"]));
        // What this app writes reads back as the same stickers (so a story it posts shows the same to everyone).
        const auto again = st::parseStickers(st::toJson(parsed));
        CHECK(st::toJson(again) == st::toJson(parsed));
    }
}

TEST_CASE("sticker text is cleaned and counted the way the web app does it", "[stories][stickers]") {
    for (const auto& c : vectors()["stickers"]["clean"]) {
        const std::size_t max = c["max"].get<std::size_t>();
        const auto& expected = c["expected"];
        if (!c["input"].is_string()) {
            // The web app also refuses a number, null or list; here only text can get that far.
            CHECK(expected.is_null());
            continue;
        }
        const auto got = st::cleanStickerText(c["input"].get<std::string>(), max);
        INFO(c["input"].dump());
        if (expected.is_null()) {
            CHECK_FALSE(got);
        } else {
            REQUIRE(got);
            CHECK(*got == expected.get<std::string>());
        }
    }
}

TEST_CASE("links: what the web app accepts is spelled the same here, and what it refuses is refused", "[stories][stickers]") {
    // Inputs the web's URL parser rewrites in ways this app doesn't copy; this app refuses them instead (see the tests below).
    const std::set<std::string> stricter = {"https:example.com"};
    for (const auto& c : vectors()["stickers"]["links"]) {
        const std::string input = c["input"].get<std::string>();
        INFO(input.substr(0, 80));
        const auto got = st::safeStoryLink(input);
        if (c["expected"].is_null()) {
            CHECK_FALSE(got);
        } else if (stricter.count(input)) {
            CHECK_FALSE(got);
        } else {
            REQUIRE(got);
            CHECK(*got == c["expected"].get<std::string>());
        }
    }
}

TEST_CASE("links: anything this app would have to guess at is refused", "[stories][stickers]") {
    for (const char* bad : {
             "https://b\xC3\xBC" "cher.example/",          // an international host name: the web app would rewrite it to punycode
             "https://example.com/a b",                       // a space
             "https://example.com/a\tb",                      // a tab
             "https://example.com\\evil.com",                 // a backslash
             "https://example.com/../a",                      // a dot segment
             "https://example.com/a/./b",
             "https://example.com/%2e%2e/a",
             "https://0x7f.1/",                               // a number in disguise
             "https://127.1/",
             "https://1.2.3/",
             "https://999.1.1.1/",
             "https://127.0.0.01/",
             "https://[::1]/",
             "https://example.com:0/",
             "https://example.com:99999/",
             "https://example.com:/",
             "https://example.com:443x/",
             "https://-bad.example.com/",
             "https://exa_mple.com/",
             "https://example..com/",
             "https://example.com./",
             "https://example.com/'quote",
             "https://example.com/<script>",
             "https://@example.com/",
             "https://example.com@evil.com/",
         }) {
        INFO(bad);
        CHECK_FALSE(st::safeStoryLink(bad));
    }
    // The same address with only what both apps agree on.
    CHECK(st::safeStoryLink("https://a.co") == std::optional<std::string>("https://a.co/"));
    CHECK(st::safeStoryLink("https://example.com/a%20b?c=d%26e") == std::optional<std::string>("https://example.com/a%20b?c=d%26e"));
    CHECK(st::safeStoryLink("https://example.com/..a/b..") == std::optional<std::string>("https://example.com/..a/b.."));
}

TEST_CASE("a sealed story from the web app carries its stickers through", "[stories][stickers]") {
    const auto& v = vectors();
    const auto& s = v["story"];
    const std::string json = sc::openMeta(storyId(), b64(v["key"]), b64(s["metaCiphertext"]), b64(s["metaNonce"]));
    const st::Meta meta = st::parseMeta(json, "text");
    CHECK(meta.text == "Pick one!");
    CHECK(sameJson(st::toJson(meta.stickers), s["expectedStickers"]));
    CHECK(meta.stickers.size() == 9);
    // What this app writes back out (a story it re-posts, or a draft) keeps them.
    const st::Meta again = st::parseMeta(st::toJson(meta), "text");
    CHECK(sameJson(st::toJson(again.stickers), s["expectedStickers"]));
    CHECK(Json::parse(st::toJson(meta)).contains("stickers"));

    // The same story as a careless sender might make it: the unknown sticker goes, the rest stay.
    const std::string sloppy = sc::openMeta(storyId(), b64(s["sloppyKey"]), b64(s["sloppyCiphertext"]), b64(s["sloppyNonce"]));
    CHECK(st::parseMeta(sloppy, "text").stickers.size() == 9);
}

TEST_CASE("a story without stickers (or from an older app) shows as before", "[stories][stickers]") {
    const st::Meta plain = st::parseMeta(R"({"v":1,"kind":"text","text":"hi","overlays":[]})", "text");
    CHECK(plain.stickers.empty());
    CHECK_FALSE(Json::parse(st::toJson(plain)).contains("stickers"));
    // A bad sticker list is no reason to hide a story.
    CHECK(st::parseMeta(R"({"v":1,"kind":"text","text":"hi","overlays":[],"stickers":"nope"})", "text").stickers.empty());
    CHECK(st::parseMeta(R"({"v":1,"kind":"text","text":"hi","overlays":[],"stickers":[1,null,{}]})", "text").stickers.empty());
}

TEST_CASE("the helpers that decide what can be answered", "[stories][stickers]") {
    CHECK(st::isAnswerable(rawSticker("poll")));
    CHECK(st::isAnswerable(rawSticker("quiz")));
    CHECK(st::isAnswerable(rawSticker("question")));
    CHECK(st::isAnswerable(rawSticker("slider")));
    CHECK_FALSE(st::isAnswerable(rawSticker("countdown")));
    CHECK_FALSE(st::isAnswerable(rawSticker("link")));
    CHECK(st::isAnonymousSticker(rawSticker("question")));
    CHECK_FALSE(st::isAnonymousSticker(rawSticker("poll")));
    CHECK(st::isFinalSticker(rawSticker("quiz")));
    CHECK(st::isFinalSticker(rawSticker("question")));
    CHECK_FALSE(st::isFinalSticker(rawSticker("poll")));
    CHECK_FALSE(st::isFinalSticker(rawSticker("slider")));
    const st::Sticker poll = rawSticker("poll");  // closes at 1_900_000_000_000
    CHECK_FALSE(st::isClosed(poll, 1'899'999'999'999));
    CHECK(st::isClosed(poll, 1'900'000'000'000));
    CHECK_FALSE(st::isClosed(rawSticker("quiz"), 9'000'000'000'000));
}

// --- Answers -------------------------------------------------------------------------------------

TEST_CASE("answers sealed by the web app open for the author, and for their sender", "[stories][answers]") {
    const auto& v = vectors();
    std::map<std::string, st::Sticker> byId;
    for (const char* name : {"poll", "quiz", "question", "slider"}) {
        const auto s = rawSticker(name);
        byId[s.id] = s;
    }
    for (const auto& c : v["answers"]["named"]) {
        INFO(c["name"].get<std::string>());
        const st::Sticker& sticker = byId.at(c["stickerId"].get<std::string>());
        const std::string who = c["responder"].get<std::string>();
        const Bytes envelope = b64(c["envelope"]);
        const Bytes responderPublic = b64(v[who]["publicKey"]);
        const Bytes responderPrivate = b64(v[who]["privateKey"]);
        const Bytes authorPublic = b64(v["alice"]["publicKey"]);
        const Bytes authorPrivate = b64(v["alice"]["privateKey"]);

        // The author opens it with the sender's public key...
        const st::Answer answer = sc::openAnswer(storyId(), sticker, envelope, responderPublic, authorPrivate);
        CHECK(answerJson(answer) == c["expected"]);
        // ...and the sender can open their own again with the author's.
        CHECK(answerJson(sc::openAnswer(storyId(), sticker, envelope, authorPublic, responderPrivate)) == c["expected"]);

        // Someone else's key, another story, another sticker, or as a nameless answer: refused.
        const Bytes otherPublic = b64(v[who == "bob" ? "carol" : "bob"]["publicKey"]);
        CHECK_THROWS_AS(sc::openAnswer(storyId(), sticker, envelope, otherPublic, authorPrivate), CryptoError);
        CHECK_THROWS_AS(sc::openAnswer(util::randomUuid(), sticker, envelope, responderPublic, authorPrivate), CryptoError);
        for (const auto& other : byId) {
            if (other.first != sticker.id) CHECK_THROWS_AS(sc::openAnswer(storyId(), other.second, envelope, responderPublic, authorPrivate), CryptoError);
        }
        CHECK_THROWS_AS(sc::openAnonymousAnswer(storyId(), sticker, envelope, authorPublic, authorPrivate), CryptoError);
    }
}

TEST_CASE("an answer without a name from the web app opens only for the author", "[stories][answers]") {
    const auto& v = vectors();
    const st::Sticker question = rawSticker("question");
    const Bytes authorPublic = b64(v["alice"]["publicKey"]);
    const Bytes authorPrivate = b64(v["alice"]["privateKey"]);
    for (const auto& c : v["answers"]["anonymous"]) {
        const Bytes envelope = b64(c["envelope"]);
        CHECK(answerJson(sc::openAnonymousAnswer(storyId(), question, envelope, authorPublic, authorPrivate)) == c["expected"]);
        // Nobody else can open it, and it never opens as a named answer.
        const Bytes bobPublic = b64(v["bob"]["publicKey"]);
        const Bytes bobPrivate = b64(v["bob"]["privateKey"]);
        CHECK_THROWS_AS(sc::openAnonymousAnswer(storyId(), question, envelope, bobPublic, bobPrivate), CryptoError);
        CHECK_THROWS_AS(sc::openAnswer(storyId(), question, envelope, bobPublic, authorPrivate), CryptoError);
        CHECK_THROWS_AS(sc::openAnonymousAnswer(util::randomUuid(), question, envelope, authorPublic, authorPrivate), CryptoError);
        CHECK_THROWS_AS(sc::openAnonymousAnswer(storyId(), rawSticker("poll"), envelope, authorPublic, authorPrivate), CryptoError);
    }
}

TEST_CASE("answers that open but don't fit, and damaged ones, are refused", "[stories][answers]") {
    const auto& v = vectors();
    std::map<std::string, st::Sticker> byId;
    for (const char* name : {"poll", "quiz", "question", "slider"}) {
        const auto s = rawSticker(name);
        byId[s.id] = s;
    }
    const Bytes authorPublic = b64(v["alice"]["publicKey"]);
    const Bytes authorPrivate = b64(v["alice"]["privateKey"]);
    const Bytes bobPublic = b64(v["bob"]["publicKey"]);
    int checked = 0;
    for (const char* group : {"bad", "broken"}) {
        for (const auto& c : v["answers"][group]) {
            INFO(std::string(group) + ": " + c["name"].get<std::string>());
            const st::Sticker& sticker = byId.at(c["stickerId"].get<std::string>());
            const std::string envelope = c["envelope"].get<std::string>();
            Bytes bytes;
            if (!envelope.empty()) bytes = base64Decode(envelope);
            if (c["kind"].get<std::string>() == "named") {
                CHECK_THROWS_AS(sc::openAnswer(storyId(), sticker, bytes, bobPublic, authorPrivate), CryptoError);
            } else {
                CHECK_THROWS_AS(sc::openAnonymousAnswer(storyId(), sticker, bytes, authorPublic, authorPrivate), CryptoError);
            }
            ++checked;
        }
    }
    CHECK(checked == 26);
}

TEST_CASE("answers made here have the web app's layout and open with this app's functions", "[stories][answers]") {
    const KeyPair author = generateKeyPair();
    const KeyPair viewer = generateKeyPair();
    const std::string id = util::randomUuid();
    const st::Sticker poll = rawSticker("poll");
    const st::Sticker question = rawSticker("question");
    const st::Sticker slider = rawSticker("slider");

    const Bytes named = sc::sealAnswer(id, poll, pollAnswer(1), author.publicKey, viewer.privateKey);
    CHECK(answerJson(sc::openAnswer(id, poll, named, viewer.publicKey, author.privateKey)) == Json::parse(R"({"k":"poll","choice":1})"));
    // nonce (24) + MAC (16) + "RVSR" 1 16 (id length) (id) + the JSON
    const std::string body = R"({"k":"poll","choice":1})";
    CHECK(named.size() == 24 + 16 + 4 + 1 + 16 + 1 + poll.id.size() + body.size());

    const Bytes nameless = sc::sealAnonymousAnswer(id, question, textAnswer("  hello  "), author.publicKey);
    CHECK(sc::openAnonymousAnswer(id, question, nameless, author.publicKey, author.privateKey).text == "hello");
    CHECK(nameless.size() == 48 + 4 + 1 + 16 + 1 + question.id.size() + std::string(R"({"k":"question","text":"hello"})").size());
    // Sealed afresh every time.
    CHECK(sc::sealAnswer(id, poll, pollAnswer(1), author.publicKey, viewer.privateKey) != named);

    // Anything that doesn't fit its sticker can't even be sealed.
    CHECK_THROWS_AS(sc::sealAnswer(id, poll, pollAnswer(3), author.publicKey, viewer.privateKey), CryptoError);
    CHECK_THROWS_AS(sc::sealAnswer(id, poll, pollAnswer(-1), author.publicKey, viewer.privateKey), CryptoError);
    CHECK_THROWS_AS(sc::sealAnswer(id, poll, sliderAnswer(5), author.publicKey, viewer.privateKey), CryptoError);
    CHECK_THROWS_AS(sc::sealAnswer(id, slider, sliderAnswer(101), author.publicKey, viewer.privateKey), CryptoError);
    CHECK_THROWS_AS(sc::sealAnswer(id, slider, sliderAnswer(-1), author.publicKey, viewer.privateKey), CryptoError);
    CHECK_THROWS_AS(sc::sealAnonymousAnswer(id, question, textAnswer("   "), author.publicKey), CryptoError);
    CHECK_THROWS_AS(sc::sealAnonymousAnswer(id, question, textAnswer(std::string(501, 'x')), author.publicKey), CryptoError);
    CHECK_NOTHROW(sc::sealAnonymousAnswer(id, question, textAnswer(std::string(500, 'x')), author.publicKey));
    // 250 emoji are 500 UTF-16 units; one more is too long.
    std::string emoji;
    for (int i = 0; i < 250; ++i) emoji += "\xF0\x9F\x98\x8D";
    CHECK_NOTHROW(sc::sealAnonymousAnswer(id, question, textAnswer(emoji), author.publicKey));
    CHECK_THROWS_AS(sc::sealAnonymousAnswer(id, question, textAnswer(emoji + "\xF0\x9F\x98\x8D"), author.publicKey), CryptoError);
    // Not a story id.
    CHECK_THROWS_AS(sc::sealAnswer("not-an-id", poll, pollAnswer(1), author.publicKey, viewer.privateKey), CryptoError);
    // The wrong key sizes.
    CHECK_THROWS_AS(sc::sealAnswer(id, poll, pollAnswer(1), Bytes(5), viewer.privateKey), CryptoError);
    CHECK_THROWS_AS(sc::openAnswer(id, poll, named, viewer.publicKey, Bytes(5)), CryptoError);
}

// --- Totals --------------------------------------------------------------------------------------

TEST_CASE("totals sealed by the web app open here, for that story, sticker and revision only", "[stories][totals]") {
    const auto& v = vectors();
    const Bytes key = b64(v["key"]);
    std::map<std::string, st::Sticker> byId;
    for (const char* name : {"poll", "quiz", "question", "slider"}) {
        const auto s = rawSticker(name);
        byId[s.id] = s;
    }
    for (const auto& c : v["totals"]["cases"]) {
        INFO(c["name"].get<std::string>());
        const st::Sticker& sticker = byId.at(c["stickerId"].get<std::string>());
        const auto revision = c["revision"].get<std::uint32_t>();
        const Bytes ciphertext = b64(c["ciphertext"]);
        const Bytes nonce = b64(c["nonce"]);
        const st::Totals totals = sc::openTotals(storyId(), sticker, revision, ciphertext, nonce, key);
        CHECK(sameJson(st::toJson(totals), c["expected"]));

        CHECK_THROWS_AS(sc::openTotals(storyId(), sticker, revision + 1, ciphertext, nonce, key), CryptoError);
        CHECK_THROWS_AS(sc::openTotals(storyId(), sticker, revision == 1 ? 2 : revision - 1, ciphertext, nonce, key), CryptoError);
        CHECK_THROWS_AS(sc::openTotals(util::randomUuid(), sticker, revision, ciphertext, nonce, key), CryptoError);
        CHECK_THROWS_AS(sc::openTotals(storyId(), sticker, revision, ciphertext, nonce, Bytes(32, 7)), CryptoError);
        for (const auto& other : byId) {
            if (other.first != sticker.id) CHECK_THROWS_AS(sc::openTotals(storyId(), other.second, revision, ciphertext, nonce, key), CryptoError);
        }
        Bytes damaged = ciphertext;
        damaged.back() ^= 1;
        CHECK_THROWS_AS(sc::openTotals(storyId(), sticker, revision, damaged, nonce, key), CryptoError);
        Bytes otherNonce = nonce;
        otherNonce[0] ^= 1;
        CHECK_THROWS_AS(sc::openTotals(storyId(), sticker, revision, ciphertext, otherNonce, key), CryptoError);
    }
}

TEST_CASE("authentic totals that don't fit their sticker are refused", "[stories][totals]") {
    const auto& v = vectors();
    const Bytes key = b64(v["key"]);
    std::map<std::string, st::Sticker> byId;
    for (const char* name : {"poll", "quiz", "question", "slider"}) {
        const auto s = rawSticker(name);
        byId[s.id] = s;
    }
    int n = 0;
    for (const auto& c : v["totals"]["bad"]) {
        INFO(c["name"].get<std::string>());
        const st::Sticker& sticker = byId.at(c["stickerId"].get<std::string>());
        CHECK_THROWS_AS(sc::openTotals(storyId(), sticker, c["revision"].get<std::uint32_t>(), b64(c["ciphertext"]), b64(c["nonce"]), key), CryptoError);
        ++n;
    }
    CHECK(n == 13);
}

TEST_CASE("totals made here have the web app's layout and round-trip", "[stories][totals]") {
    const std::string id = util::randomUuid();
    const SecretBytes key = sc::newStoryKey();
    const st::Sticker quiz = rawSticker("quiz");
    const st::Sticker slider = rawSticker("slider");
    const st::Sticker poll = rawSticker("poll");

    st::Totals q;
    q.total = 5;
    q.counts = {1, 3, 1};
    q.correct = 3;
    const auto sealed = sc::sealTotals(id, quiz.id, 4, q, key);
    const st::Totals back = sc::openTotals(id, quiz, 4, sealed.ciphertext, sealed.nonce, key);
    CHECK(back.total == 5);
    CHECK(back.counts == std::vector<int>{1, 3, 1});
    CHECK(back.correct == 3);
    CHECK(sealed.nonce.size() == 24);
    CHECK(sealed.ciphertext.size() == st::toJson(q).size() + 16);

    st::Totals s;
    s.total = 2;
    s.average = 55.5;
    const auto sealedSlider = sc::sealTotals(id, slider.id, 1, s, key);
    CHECK_THAT(*sc::openTotals(id, slider, 1, sealedSlider.ciphertext, sealedSlider.nonce, key).average, Catch::Matchers::WithinAbs(55.5, 1e-9));

    CHECK_THROWS_AS(sc::sealTotals(id, poll.id, 0, st::Totals{}, key), CryptoError);
    CHECK_THROWS_AS(sc::sealTotals(id, "Bad Id", 1, st::Totals{}, key), CryptoError);
    CHECK_THROWS_AS(sc::sealTotals(id, poll.id, 1, st::Totals{}, Bytes(5)), CryptoError);
}

// --- Counting ------------------------------------------------------------------------------------

TEST_CASE("counting answers gives the web app's totals", "[stories][totals]") {
    for (const auto& c : vectors()["tally"]) {
        INFO(c["name"].get<std::string>());
        const st::Sticker sticker = stickerFromJson(c["sticker"]);
        std::vector<st::InboxEntry> entries;
        for (const auto& e : c["entries"]) {
            st::InboxEntry entry;
            entry.stickerId = e["stickerId"].get<std::string>();
            const auto& a = e["answer"];
            entry.answer.kind = a["k"].get<std::string>();
            if (a.contains("choice")) entry.answer.choice = a["choice"].get<int>();
            if (a.contains("value")) entry.answer.value = a["value"].get<int>();
            if (a.contains("text")) entry.answer.text = a["text"].get<std::string>();
            entry.atMs = e["at"].get<std::int64_t>();
            entry.revision = e["revision"].get<int>();
            entries.push_back(std::move(entry));
        }
        const st::Totals totals = st::tally(sticker, entries);
        CHECK(sameJson(st::toJson(totals), c["expected"]));
    }
}
