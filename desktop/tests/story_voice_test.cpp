// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// Voice stories (kind "audio"), subtitles, loudness bars and animated overlays, read the way the web app and Android
// read them. Pinned to vectors/story_wire_vectors.json, made by the web app's own code
// (android/app/src/test/resources/vectors/make_android_story_vectors.ts) and shared with the Android tests: a feed
// with a voice story, a kind nobody knows, fields nobody reads yet and stories every app must refuse.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/file_crypto.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/model/stories.hpp"

using namespace redvault;
using namespace redvault::crypto;
using Json = nlohmann::json;
namespace st = redvault::model::stories;

namespace {

const Json& wire() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_STORY_WIRE_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

Bytes b64(const Json& j) {
    return base64Decode(j.get<std::string>());
}

std::string hex(ByteView bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

struct Opened {
    st::Meta meta;
    Bytes key;
};

/// What this app makes of bob's feed: the same steps the stories session takes for an account-keys story
/// (the author must be a friend and the story's own author, the key must open for this story, the details must parse).
std::map<std::string, Opened> openFeedForBob() {
    const auto& v = wire();
    std::map<std::string, Bytes> friendKeys;
    for (const auto& f : v["friendsOfBob"]) friendKeys[f["userId"].get<std::string>()] = b64(f["publicKey"]);
    const Bytes bobPrivate = b64(v["keys"]["bob"]["privateKey"]);
    std::map<std::string, Opened> out;
    for (const auto& group : v["feedForBob"]["authors"]) {
        const std::string authorId = group["authorId"];
        const auto f = friendKeys.find(authorId);
        if (f == friendKeys.end()) continue;  // not a friend: nothing of theirs is shown
        for (const auto& s : group["stories"]) {
            const std::string id = s["id"];
            const std::string kind = s["kind"];
            if (s["authorId"].get<std::string>() != authorId || !st::isKnownKind(kind)) continue;
            try {
                const SecretBytes key = stories::unwrapStoryKey(id, b64(s["wrappedKey"]), f->second, bobPrivate);
                const std::string json = stories::openMeta(id, key, b64(s["metaCiphertext"]), b64(s["metaNonce"]));
                out[id] = Opened{st::parseMeta(json, kind), Bytes(key.view().begin(), key.view().end())};
            } catch (const std::exception&) {
                // refused, alone: the author's other stories still show
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("bob's feed shows exactly what the web shows, the voice story included", "[stories][voice]") {
    const auto& v = wire();
    const auto opened = openFeedForBob();
    std::vector<std::string> shown;
    for (const auto& [id, o] : opened) shown.push_back(id);
    std::vector<std::string> web;
    for (const auto& s : v["webVisible"]) web.push_back(s["id"]);
    std::sort(web.begin(), web.end());
    CHECK(shown == web);
    // Every story the web refuses is refused here too.
    for (const auto& id : v["refusedEverywhere"]) CHECK(opened.count(id.get<std::string>()) == 0);
    // What Android used to skip (the voice story) is shown here.
    for (const auto& s : v["skippedByAndroid"]) CHECK(opened.count(s["id"].get<std::string>()) == 1);
}

TEST_CASE("each story the web shows reads the same here, field for field", "[stories][voice]") {
    const auto opened = openFeedForBob();
    for (const auto& want : wire()["webVisible"]) {
        const std::string id = want["id"];
        INFO(id);
        REQUIRE(opened.count(id) == 1);
        const auto& o = opened.at(id);
        const Json& m = want["meta"];
        CHECK(o.key == b64(want["key"]));
        CHECK(o.meta.kind == want["kind"].get<std::string>());
        CHECK(o.meta.closeFriends == m.value("closeFriends", false));
        if (m.contains("text")) CHECK(o.meta.text == m["text"].get<std::string>());
        else CHECK(o.meta.text.empty());
        if (m.contains("background")) CHECK(o.meta.background == m["background"].get<std::string>());
        if (m.contains("font")) CHECK(o.meta.font == m["font"].get<std::string>());
        REQUIRE(o.meta.overlays.size() == m["overlays"].size());
        for (std::size_t i = 0; i < o.meta.overlays.size(); ++i) {
            const auto& w = m["overlays"][i];
            const auto& mine = o.meta.overlays[i];
            CHECK(mine.type == w["type"].get<std::string>());
            CHECK(mine.text == w["text"].get<std::string>());
            CHECK_THAT(mine.x, Catch::Matchers::WithinAbs(w["x"].get<double>(), 1e-12));
            CHECK_THAT(mine.y, Catch::Matchers::WithinAbs(w["y"].get<double>(), 1e-12));
            CHECK_THAT(mine.scale, Catch::Matchers::WithinAbs(w["scale"].get<double>(), 1e-12));
            CHECK_THAT(mine.rotation, Catch::Matchers::WithinAbs(w["rotation"].get<double>(), 1e-12));
            CHECK(mine.color == w["color"].get<std::string>());
            CHECK(mine.boxed == w["boxed"].get<bool>());
            CHECK(mine.font == w["font"].get<std::string>());
            // The web keeps a motion it knows and leaves out the rest ("spin", "none").
            CHECK(mine.anim == w.value("anim", std::string()));
        }
        CHECK(o.meta.stickers.size() == m.value("stickers", Json::array()).size());
        const Json captions = m.value("captions", Json::array());
        REQUIRE(o.meta.captions.size() == captions.size());
        for (std::size_t i = 0; i < captions.size(); ++i) {
            CHECK(o.meta.captions[i].startMs == captions[i]["startMs"].get<std::int64_t>());
            CHECK(o.meta.captions[i].endMs == captions[i]["endMs"].get<std::int64_t>());
            CHECK(o.meta.captions[i].text == captions[i]["text"].get<std::string>());
        }
        if (m.contains("waveform")) {
            REQUIRE(o.meta.waveform);
            CHECK(*o.meta.waveform == m["waveform"].get<std::vector<int>>());
        } else {
            CHECK(!o.meta.waveform);
        }
        if (m.contains("media")) {
            REQUIRE(o.meta.media);
            const auto& w = m["media"];
            CHECK(o.meta.media->mime == w["mime"].get<std::string>());
            CHECK(o.meta.media->width == w["width"].get<int>());
            CHECK(o.meta.media->height == w["height"].get<int>());
            CHECK(o.meta.media->encryptedSize == w["encryptedSize"].get<std::uint64_t>());
            if (w.contains("durationMs")) CHECK(o.meta.media->durationMs == w["durationMs"].get<std::int64_t>());
            else CHECK(!o.meta.media->durationMs);
        } else {
            CHECK(!o.meta.media);
        }
    }
}

TEST_CASE("the voice story: title, look, bars, subtitles and its sound", "[stories][voice]") {
    const auto& v = wire();
    const auto opened = openFeedForBob();
    const std::string audioId = v["skippedByAndroid"][0]["id"];
    const auto& voice = opened.at(audioId).meta;
    CHECK(voice.kind == "audio");
    CHECK(voice.text == "Voice note");
    CHECK(voice.background == "night");
    REQUIRE(voice.waveform);
    CHECK(voice.waveform->size() == 32);
    // The web's lines, sorted, with the empty span and the empty text dropped.
    REQUIRE(voice.captions.size() == 2);
    CHECK(voice.captions[0].text == "first line");
    CHECK(st::captionAt(voice.captions, 0) == "first line");
    CHECK(st::captionAt(voice.captions, 1399) == "first line");
    CHECK(!st::captionAt(voice.captions, 1400));
    CHECK(st::captionAt(voice.captions, 2000) == "second line");
    CHECK(!st::captionAt(voice.captions, 3000));
    // A reply quotes it as text, by its title.
    CHECK(st::preview(voice) == "Voice note");
    CHECK(st::replyKind(voice) == "text");

    // The sound opens with the key and header in the sealed details, and is the web's bytes.
    const auto& media = v["media"][audioId];
    const Bytes cipher = b64(media["ciphertext"]);
    std::istringstream in(std::string(cipher.begin(), cipher.end()));
    std::ostringstream out;
    files::decrypt(in, out, base64Decode(voice.media->key), base64Decode(voice.media->header), voice.media->encryptedSize, voice.media->sha256);
    const std::string plain = out.str();
    CHECK(plain.size() == media["plaintextLength"].get<std::size_t>());
    CHECK(hex(sha256(ByteView(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()))) == media["plaintextSha256"].get<std::string>());
}

TEST_CASE("a voice story is checked as strictly as the web checks it", "[stories][voice]") {
    const std::string base =
        R"({"v":1,"kind":"audio","overlays":[],"closeFriends":false,"media":{"key":"a2V5","header":"aGVhZA","sha256":"4ed7cb3bafd12893b13f7bbdfbda2cada5fe715050635ba7b0933a02494acaca","encryptedSize":3017,"chunkSize":65536,"mime":"audio/wav","durationMs":9000)";
    const st::Meta m = st::parseMeta(base + "}}", "audio");
    // No title, the default look, width and height of 1 whatever is written (or missing).
    CHECK(m.text.empty());
    CHECK(m.background == "night");
    CHECK(m.media->width == 1);
    CHECK(m.media->height == 1);
    CHECK(st::preview(m) == "Voice");
    CHECK(st::parseMeta(base + R"(,"width":0,"height":-3}})", "audio").media->width == 1);
    // A voice story has a length and an audio type; nothing else.
    CHECK_THROWS_AS(st::parseMeta(R"({"v":1,"kind":"audio","overlays":[],"media":{"key":"a2V5","header":"aGVhZA","sha256":"4ed7cb3bafd12893b13f7bbdfbda2cada5fe715050635ba7b0933a02494acaca","encryptedSize":3017,"chunkSize":65536,"mime":"audio/wav"}})", "audio"),
                    st::StoryFormatError);
    std::string video = base;
    video.replace(video.find("audio/wav"), 9, "video/mp4");
    CHECK_THROWS_AS(st::parseMeta(video + "}}", "audio"), st::StoryFormatError);
    CHECK_THROWS_AS(st::parseMeta(base + "}}", "video"), st::StoryFormatError);  // the server says video, the details say audio
    for (const char* mime : {"audio/webm", "audio/ogg", "audio/mp4", "audio/mpeg", "audio/wav", "audio/aac"}) {
        std::string s = base;
        s.replace(s.find("audio/wav"), 9, mime);
        CHECK_NOTHROW(st::parseMeta(s + "}}", "audio"));
    }
    // A title that is only spaces or too long is left out; the story still shows.
    CHECK(st::parseMeta(base + R"(},"text":"   "})", "audio").text.empty());
    CHECK(st::parseMeta(base + R"(},"text":")" + std::string(701, 'a') + R"("})", "audio").text.empty());
    CHECK(st::parseMeta(base + R"(},"text":"Hi","background":"mint","font":"strong"})", "audio").background == "mint");
    CHECK(st::parseMeta(base + R"(},"background":"aurora"})", "audio").background == "night");
    // Kinds this app shows; anything newer is skipped.
    CHECK(st::isKnownKind("audio"));
    CHECK(!st::isKnownKind("reel"));
}

TEST_CASE("loudness bars: 16 to 96 whole numbers from 0 to 100, or none", "[stories][voice]") {
    CHECK(!st::parseWaveform("[]"));
    CHECK(!st::parseWaveform("{}"));
    std::string sixteen = "[";
    for (int i = 0; i < 16; ++i) sixteen += std::to_string(i * 6) + (i < 15 ? "," : "]");
    CHECK(st::parseWaveform(sixteen)->size() == 16);
    std::string fifteen = "[";
    for (int i = 0; i < 15; ++i) fifteen += std::string("1") + (i < 14 ? "," : "]");
    CHECK(!st::parseWaveform(fifteen));
    std::string many = "[";
    for (int i = 0; i < 97; ++i) many += std::string("1") + (i < 96 ? "," : "]");
    CHECK(!st::parseWaveform(many));
    std::string withFloat = sixteen;
    withFloat.replace(1, 1, "0.5");
    CHECK(!st::parseWaveform(withFloat));
    std::string whole = sixteen;
    whole.replace(1, 1, "3.0");  // JavaScript counts 3.0 as a whole number
    CHECK(st::parseWaveform(whole)->front() == 3);
    std::string tooLoud = sixteen;
    tooLoud.replace(1, 1, "101");
    CHECK(!st::parseWaveform(tooLoud));
    std::string text = sixteen;
    text.replace(1, 1, "\"5\"");
    CHECK(!st::parseWaveform(text));
}

TEST_CASE("subtitles keep the lines that make sense, in time order", "[stories][voice]") {
    const auto c = st::parseCaptions(
        R"([{"startMs":2000,"endMs":3000,"text":"two"},{"startMs":0.4,"endMs":999.5,"text":"  one \u0007 "},{"startMs":-1,"endMs":5,"text":"before"},)"
        R"({"startMs":10,"endMs":10,"text":"no span"},{"startMs":0,"endMs":600001,"text":"too late"},{"startMs":"0","endMs":5,"text":"string"},)"
        R"({"startMs":0,"endMs":5,"text":""},{"startMs":0,"endMs":5},"nope",{"startMs":0,"endMs":600000,"text":"to the end"}])");
    REQUIRE(c.size() == 3);
    CHECK(c[0].startMs == 0);
    CHECK(c[0].endMs == 1000);  // Math.round(999.5)
    CHECK(c[0].text == "one");  // tidied like sticker text
    CHECK(c[1].text == "to the end");
    CHECK(c[2].text == "two");
    std::string sixtyOne = "[";
    for (int i = 0; i < 61; ++i) sixtyOne += R"({"startMs":)" + std::to_string(i * 10) + R"(,"endMs":)" + std::to_string(i * 10 + 5) + R"(,"text":"x"})" + (i < 60 ? "," : "]");
    CHECK(st::parseCaptions(sixtyOne).size() == 60);
    CHECK(st::parseCaptions("not json").empty());
}

TEST_CASE("a voice story, subtitles and moving words written here read back the same", "[stories][voice]") {
    st::Meta voice;
    voice.kind = "audio";
    voice.text = "Morning thoughts";
    voice.background = "ocean";
    voice.waveform = std::vector<int>(48, 50);
    voice.captions = {{0, 1500, "Hello"}, {1500, 3000, "world"}};
    st::Overlay word;
    word.type = "text";
    word.text = "hey";
    word.anim = "bounce";
    voice.overlays.push_back(word);
    st::MediaRef media;
    media.key = "a2V5";
    media.header = "aGVhZA";
    media.sha256 = std::string(64, 'a');
    media.encryptedSize = 100;
    media.chunkSize = 65536;
    media.mime = "audio/wav";
    media.width = 1080;  // written as 1 for a voice story
    media.height = 1920;
    media.durationMs = 3000;
    voice.media = media;
    const std::string json = st::toJson(voice);
    const Json j = Json::parse(json);
    CHECK(j["media"]["width"] == 1);
    CHECK(j["media"]["height"] == 1);
    CHECK(j["overlays"][0]["anim"] == "bounce");
    const st::Meta back = st::parseMeta(json, "audio");
    CHECK(back.text == voice.text);
    CHECK(back.background == "ocean");
    CHECK(back.waveform == voice.waveform);
    CHECK(back.captions == voice.captions);
    CHECK(back.overlays[0].anim == "bounce");
    CHECK(back.media->durationMs == 3000);

    // Still words carry no "anim" at all (older apps never see the field), and a photo never carries subtitles.
    st::Meta photo;
    photo.kind = "image";
    photo.overlays.push_back({"text", "still", 0.5, 0.5, 1, 0, "white", false, "classic"});
    photo.captions = {{0, 100, "dropped"}};
    st::MediaRef p = media;
    p.mime = "image/jpeg";
    p.durationMs.reset();
    photo.media = p;
    const Json pj = Json::parse(st::toJson(photo));
    CHECK(!pj["overlays"][0].contains("anim"));
    CHECK(!pj.contains("captions"));
    CHECK(pj["media"]["width"] == 1080);
}
