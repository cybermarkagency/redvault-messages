// Story drafts: the sealed details are the web app's layout, byte for byte (vectors made by its own code:
// vectors/make_story_draft_vectors.ts), nothing readable is stored, and a draft can't be moved to another
// account or id, opened with another key, or changed on disk without being dropped.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/story_drafts.hpp"
#include "redvault/model/story_drafts.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using namespace redvault::crypto;
using Json = nlohmann::json;
namespace fs = std::filesystem;
namespace ms = redvault::model::stories;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_STORY_DRAFT_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

Bytes b64(const Json& j) {
    return base64Decode(j.get<std::string>());
}

/// A folder of its own, gone when the test ends.
struct TempRoot {
    fs::path path = fs::temp_directory_path() / ("rv-drafts-" + util::randomUuid());
    TempRoot() { fs::create_directories(path); }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}
void writeAll(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// Puts a draft the web app made into a store folder, as this app would have kept it.
void install(const fs::path& root, const Json& userId, const Json& d, const std::string& asId = {}, const std::string& underUser = {}) {
    const std::string id = asId.empty() ? d["id"].get<std::string>() : asId;
    const std::string user = underUser.empty() ? userId.get<std::string>() : underUser;
    const Json record{{"v", 1}, {"id", id}, {"userId", d["userId"]}, {"updatedAt", d["updatedAt"]}, {"nonce", d["nonce"]}, {"sealed", d["sealed"]}, {"hasMedia", !d["media"].is_null()}};
    writeAll(root / user / (id + ".draft"), record.dump());
    if (!d["media"].is_null()) {
        const Bytes media = b64(d["media"]);
        writeAll(root / user / (id + ".media"), std::string(media.begin(), media.end()));
    }
}

std::int64_t at(const Json& d) {
    return d["updatedAt"].get<std::int64_t>() + 1000;
}

ms::DraftContent sample() {
    ms::DraftContent c;
    c.mode = "text";
    c.text = "a surprise party for Marta \xF0\x9F\x8E\x88";
    c.background = "forest";
    c.font = "modern";
    c.overlays.push_back({"emoji", "\xF0\x9F\x94\xA5", 0.5, 0.8, 1.2, 15, "white", false, "classic"});
    const auto stickers = ms::parseStickers(R"([{"id":"pollsticker1","type":"poll","x":0.4,"y":0.6,"scale":1.2,"rotation":5,"question":"Secret plan?","options":["Yes","No"],"results":"everyone"}])");
    c.stickers = stickers;
    c.audience = "custom";
    c.customIds = {"00000000-0000-4000-8000-00000000000c"};
    c.allowReplies = false;
    c.ttlSeconds = 21600;
    return c;
}

const std::string kUser = "00000000-0000-4000-8000-00000000000a";

}  // namespace

TEST_CASE("drafts the web app sealed open here, summary, details and photo or video alike", "[stories][drafts]") {
    const auto& v = vectors();
    TempRoot tmp;
    for (const auto& d : v["drafts"]) install(tmp.path, v["userId"], d);
    data::StoryDraftStore store(tmp.path);
    const std::string user = v["userId"];
    const Bytes key = b64(v["privateKey"]);

    const auto list = store.list(user, key, at(v["drafts"][2]));
    REQUIRE(list.size() == 3);
    // Newest first: the video, then the photo, then the text one.
    CHECK(list[0].id == v["drafts"][2]["id"].get<std::string>());
    CHECK(list[2].id == v["drafts"][0]["id"].get<std::string>());

    for (const auto& d : v["drafts"]) {
        INFO(d["name"].get<std::string>());
        const auto summary = std::find_if(list.begin(), list.end(), [&](const ms::DraftSummary& s) { return s.id == d["id"].get<std::string>(); });
        REQUIRE(summary != list.end());
        CHECK(summary->kind == d["summary"]["kind"].get<std::string>());
        CHECK(summary->preview == d["summary"]["preview"].get<std::string>());
        CHECK(summary->stickerCount == d["summary"]["stickerCount"].get<int>());
        CHECK(summary->updatedAtMs == d["updatedAt"].get<std::int64_t>());

        const auto loaded = store.load(user, key, d["id"].get<std::string>());
        REQUIRE(loaded);
        const Json& e = d["expected"];
        CHECK(loaded->content.mode == e["mode"].get<std::string>());
        CHECK(loaded->content.text == e["text"].get<std::string>());
        CHECK(loaded->content.background == e["background"].get<std::string>());
        CHECK(loaded->content.font == e["font"].get<std::string>());
        CHECK(Json::parse(ms::toJson(loaded->content.overlays)) == e["overlays"]);
        CHECK(Json::parse(ms::toJson(loaded->content.stickers)) == e["stickers"]);
        CHECK(loaded->content.audience == e["audience"].get<std::string>());
        CHECK(loaded->content.customIds == e["customIds"].get<std::vector<std::string>>());
        CHECK(loaded->content.allowReplies == e["allowReplies"].get<bool>());
        CHECK(loaded->content.ttlSeconds == e["ttlSeconds"].get<int>());
        if (e["media"].is_null()) {
            CHECK_FALSE(loaded->media);
        } else {
            REQUIRE(loaded->media);
            CHECK(loaded->media->kind == e["media"]["kind"].get<std::string>());
            CHECK(loaded->media->mime == e["media"]["mime"].get<std::string>());
            CHECK(loaded->media->width == e["media"]["width"].get<int>());
            CHECK(loaded->media->height == e["media"]["height"].get<int>());
            if (e["media"].contains("durationMs")) {
                REQUIRE(loaded->media->durationMs);
                CHECK(*loaded->media->durationMs == e["media"]["durationMs"].get<std::int64_t>());
            } else {
                CHECK_FALSE(loaded->media->durationMs);
            }
            const Bytes plain = b64(e["media"]["bytes"]);
            CHECK(loaded->media->bytes == std::string(plain.begin(), plain.end()));
        }
    }
}

TEST_CASE("a draft opens only for its account, its key and its id, and an altered one is dropped", "[stories][drafts]") {
    const auto& v = vectors();
    const std::string user = v["userId"];
    const Bytes key = b64(v["privateKey"]);
    const Json& text = v["drafts"][0];
    const Json& photo = v["drafts"][1];

    SECTION("another account's folder shows nothing, and a draft copied into it doesn't open") {
        TempRoot tmp;
        install(tmp.path, v["userId"], text);
        data::StoryDraftStore store(tmp.path);
        const std::string other = v["otherUserId"];
        CHECK(store.list(other, key, at(text)).empty());
        CHECK_FALSE(store.load(other, key, text["id"].get<std::string>()));
        // Slipped into the other account's folder, it still says whose it is, and was sealed for them.
        install(tmp.path, v["userId"], text, {}, other);
        CHECK_FALSE(store.load(other, b64(v["otherPrivateKey"]), text["id"].get<std::string>()));
        CHECK(store.load(user, key, text["id"].get<std::string>()));
    }
    SECTION("a record in my folder that says it belongs to someone else isn't used") {
        TempRoot tmp;
        install(tmp.path, v["userId"], text);
        data::StoryDraftStore store(tmp.path);
        const fs::path file = tmp.path / user / (text["id"].get<std::string>() + ".draft");
        Json record = Json::parse(readAll(file));
        record["userId"] = v["otherUserId"];
        writeAll(file, record.dump());
        // It would open (my folder, my key, its own id), but it says it is someone else's.
        CHECK(store.list(user, key, at(text)).empty());
        CHECK_FALSE(fs::exists(file));
        install(tmp.path, v["userId"], text);
        writeAll(file, [&] { Json r = Json::parse(readAll(file)); r["userId"] = v["otherUserId"]; return r.dump(); }());
        CHECK_FALSE(store.load(user, key, text["id"].get<std::string>()));
    }
    SECTION("the same account with another key can't open it, so it is cleared") {
        TempRoot tmp;
        install(tmp.path, v["userId"], text);
        data::StoryDraftStore store(tmp.path);
        CHECK(store.list(user, b64(v["otherPrivateKey"]), at(text)).empty());
        CHECK_FALSE(fs::exists(tmp.path / user / (text["id"].get<std::string>() + ".draft")));
    }
    SECTION("a draft changed on disk is dropped, not opened") {
        TempRoot tmp;
        install(tmp.path, v["userId"], text);
        data::StoryDraftStore store(tmp.path);
        const fs::path file = tmp.path / user / (text["id"].get<std::string>() + ".draft");
        Json record = Json::parse(readAll(file));
        Bytes sealed = base64Decode(record["sealed"].get<std::string>());
        sealed[5] ^= 1;
        record["sealed"] = base64Encode(sealed);
        writeAll(file, record.dump());
        CHECK_FALSE(store.load(user, key, text["id"].get<std::string>()));
        CHECK_FALSE(fs::exists(file));
    }
    SECTION("a draft can't be moved to another id") {
        TempRoot tmp;
        const std::string other = "0123456789abcdef01234567";
        install(tmp.path, v["userId"], text, other);
        data::StoryDraftStore store(tmp.path);
        // What sits at that id was sealed for another one.
        CHECK_FALSE(store.load(user, key, other));
        CHECK(store.list(user, key, at(text)).empty());
    }
    SECTION("a photo that was changed on disk makes the draft unusable") {
        TempRoot tmp;
        install(tmp.path, v["userId"], photo);
        data::StoryDraftStore store(tmp.path);
        const fs::path media = tmp.path / user / (photo["id"].get<std::string>() + ".media");
        std::string bytes = readAll(media);
        bytes[bytes.size() / 2] ^= 1;
        writeAll(media, bytes);
        CHECK_FALSE(store.load(user, key, photo["id"].get<std::string>()));
        CHECK_FALSE(fs::exists(media));
    }
    SECTION("a draft whose photo is gone is unusable too") {
        TempRoot tmp;
        install(tmp.path, v["userId"], photo);
        data::StoryDraftStore store(tmp.path);
        fs::remove(tmp.path / user / (photo["id"].get<std::string>() + ".media"));
        CHECK_FALSE(store.load(user, key, photo["id"].get<std::string>()));
    }
}

TEST_CASE("drafts saved here come back as they were, with nothing readable on disk", "[stories][drafts]") {
    TempRoot tmp;
    data::StoryDraftStore store(tmp.path);
    const KeyPair me = generateKeyPair();
    const ms::DraftContent content = sample();

    const std::string id = store.save(kUser, me.privateKey, content, std::nullopt);
    CHECK(data::isDraftId(id));
    const auto list = store.list(kUser, me.privateKey);
    REQUIRE(list.size() == 1);
    CHECK(list[0].id == id);
    CHECK(list[0].kind == "text");
    CHECK(list[0].stickerCount == 1);
    CHECK(list[0].preview == "a surprise party for Marta \xF0\x9F\x8E\x88");

    const auto back = store.load(kUser, me.privateKey, id);
    REQUIRE(back);
    CHECK(back->content.text == content.text);
    CHECK(back->content.background == "forest");
    CHECK(back->content.font == "modern");
    CHECK(back->content.audience == "custom");
    CHECK(back->content.customIds == content.customIds);
    CHECK_FALSE(back->content.allowReplies);
    CHECK(back->content.ttlSeconds == 21600);
    REQUIRE(back->content.overlays.size() == 1);
    CHECK(back->content.overlays[0].text == "\xF0\x9F\x94\xA5");
    CHECK_THAT(back->content.overlays[0].scale, Catch::Matchers::WithinAbs(1.2, 1e-9));
    CHECK(ms::toJson(back->content.stickers) == ms::toJson(content.stickers));
    CHECK_FALSE(back->media);

    // Not the text, the question, the choices or who it is for, in the clear.
    const std::string onDisk = readAll(tmp.path / kUser / (id + ".draft"));
    for (const char* secret : {"surprise party", "Marta", "Secret plan", "forest", "modern", "00000000-0000-4000-8000-00000000000c", "Yes", "poll"}) {
        INFO(secret);
        CHECK(onDisk.find(secret) == std::string::npos);
    }
    // What is in the clear is the account, the time and the nonce.
    const Json record = Json::parse(onDisk);
    CHECK(record["userId"] == kUser);
    CHECK(record["id"] == id);
}

TEST_CASE("a photo or video is kept encrypted and comes back byte for byte", "[stories][drafts]") {
    TempRoot tmp;
    data::StoryDraftStore store(tmp.path);
    const KeyPair me = generateKeyPair();
    data::DraftMediaInput video;
    video.kind = "video";
    video.mime = "video/mp4";
    video.width = 1080;
    video.height = 1920;
    video.durationMs = 4200;
    for (int i = 0; i < 200'000; ++i) video.bytes.push_back(static_cast<char>((i * 7 + 3) % 251));

    ms::DraftContent content = sample();
    content.mode = "media";
    content.text.clear();
    const std::string id = store.save(kUser, me.privateKey, content, video);

    const std::string stored = readAll(tmp.path / kUser / (id + ".media"));
    CHECK(stored.size() > video.bytes.size());
    CHECK(stored.compare(0, 64, video.bytes, 0, 64) != 0);  // not the video's bytes, not even a slice of them
    CHECK(stored.find(video.bytes.substr(1000, 64)) == std::string::npos);

    const auto back = store.load(kUser, me.privateKey, id);
    REQUIRE(back);
    CHECK(back->content.mode == "media");
    REQUIRE(back->media);
    CHECK(back->media->kind == "video");
    CHECK(back->media->mime == "video/mp4");
    CHECK(back->media->width == 1080);
    CHECK(back->media->height == 1920);
    REQUIRE(back->media->durationMs);
    CHECK(*back->media->durationMs == 4200);
    CHECK(back->media->bytes == video.bytes);
    const auto list = store.list(kUser, me.privateKey);
    REQUIRE(list.size() == 1);
    CHECK(list[0].kind == "video");
    CHECK(list[0].preview == "Video");

    // Saved over as a plain text draft: the photo or video is gone from the disk.
    content.mode = "text";
    content.text = "now just words";
    store.save(kUser, me.privateKey, content, std::nullopt, id);
    CHECK_FALSE(fs::exists(tmp.path / kUser / (id + ".media")));
    const auto words = store.load(kUser, me.privateKey, id);
    REQUIRE(words);
    CHECK_FALSE(words->media);
    CHECK(words->content.mode == "text");
}

TEST_CASE("saving over an id replaces that draft, ten at most, thirty days at most", "[stories][drafts]") {
    TempRoot tmp;
    data::StoryDraftStore store(tmp.path);
    const KeyPair me = generateKeyPair();
    ms::DraftContent content = sample();
    const std::int64_t base = 1'800'000'000'000;

    const std::string first = store.save(kUser, me.privateKey, content, std::nullopt, std::nullopt, base);
    content.text = "changed my mind";
    CHECK(store.save(kUser, me.privateKey, content, std::nullopt, first, base + 1000) == first);
    auto list = store.list(kUser, me.privateKey, base + 2000);
    REQUIRE(list.size() == 1);
    CHECK(list[0].preview == "changed my mind");
    CHECK(list[0].updatedAtMs == base + 1000);

    // Ten at most: the eleventh and twelfth push the oldest out.
    std::vector<std::string> ids{first};
    for (int i = 1; i < 12; ++i) {
        content.text = "draft " + std::to_string(i);
        ids.push_back(store.save(kUser, me.privateKey, content, std::nullopt, std::nullopt, base + 10'000 + i * 1000));
    }
    list = store.list(kUser, me.privateKey, base + 100'000);
    REQUIRE(list.size() == data::StoryDraftStore::kMaxDrafts);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(std::none_of(list.begin(), list.end(), [&](const ms::DraftSummary& s) { return s.id == ids[i]; }));
    }
    CHECK(list.front().preview == "draft 11");  // newest first
    // Saving over an existing one with ten already there takes nobody's place.
    content.text = "again";
    store.save(kUser, me.privateKey, content, std::nullopt, ids[5], base + 200'000);
    CHECK(store.list(kUser, me.privateKey, base + 201'000).size() == data::StoryDraftStore::kMaxDrafts);

    // Thirty days, then gone (and deleted from the disk): the one saved last is still just in time.
    const std::int64_t later = base + 200'000 + data::StoryDraftStore::kKeepMs;
    list = store.list(kUser, me.privateKey, later);
    REQUIRE(list.size() == 1);
    CHECK(list[0].preview == "again");
    const auto draftFiles = [&] {
        std::size_t n = 0;
        for (const auto& e : fs::directory_iterator(tmp.path / kUser)) {
            if (e.path().extension() == ".draft") ++n;
        }
        return n;
    };
    CHECK(draftFiles() == 1);
    CHECK(store.list(kUser, me.privateKey, later + 1).empty());
    CHECK(draftFiles() == 0);
}

TEST_CASE("one account never sees or removes another's drafts, and no id is ever a path", "[stories][drafts]") {
    TempRoot tmp;
    data::StoryDraftStore store(tmp.path);
    const KeyPair me = generateKeyPair();
    const KeyPair other = generateKeyPair();
    const std::string otherUser = "00000000-0000-4000-8000-00000000000b";
    const std::string mine = store.save(kUser, me.privateKey, sample(), std::nullopt);
    const std::string theirs = store.save(otherUser, other.privateKey, sample(), std::nullopt);

    CHECK(store.list(kUser, me.privateKey).size() == 1);
    CHECK(store.list(otherUser, other.privateKey).size() == 1);
    CHECK_FALSE(store.load(kUser, me.privateKey, theirs));
    store.remove(kUser, theirs);  // not in my folder: nothing happens to theirs
    CHECK(store.load(otherUser, other.privateKey, theirs));
    store.remove(kUser, mine);
    CHECK(store.list(kUser, me.privateKey).empty());

    // Nothing but what save() makes is accepted as an id or an account.
    for (const char* bad : {"", "..", "../x", "a/b", "a\\b", "ABCDEF0123456789ABCDEF01", "0123456789abcdef0123456", "0123456789abcdef012345678"}) {
        INFO(bad);
        CHECK_FALSE(data::isDraftId(bad));
        CHECK_FALSE(store.load(kUser, me.privateKey, bad));
        CHECK_THROWS_AS(store.save(kUser, me.privateKey, sample(), std::nullopt, std::string(bad)), std::invalid_argument);
        store.remove(kUser, bad);
    }
    for (const char* bad : {"", "..", "../etc", "a/b", "a b", "not-an-account"}) {
        INFO(bad);
        CHECK(store.folderOf(bad).empty());
        CHECK(store.list(bad, me.privateKey).empty());
        CHECK_THROWS_AS(store.save(bad, me.privateKey, sample(), std::nullopt), std::invalid_argument);
    }
}

TEST_CASE("leftovers of a save that never finished are cleaned up", "[stories][drafts]") {
    TempRoot tmp;
    data::StoryDraftStore store(tmp.path);
    const KeyPair me = generateKeyPair();
    const std::string id = store.save(kUser, me.privateKey, sample(), std::nullopt);
    writeAll(tmp.path / kUser / (id + ".draft.tmp"), "half");
    writeAll(tmp.path / kUser / "0123456789abcdef01234567.media", "a photo with no draft");
    CHECK(store.list(kUser, me.privateKey).size() == 1);
    CHECK_FALSE(fs::exists(tmp.path / kUser / (id + ".draft.tmp")));
    CHECK_FALSE(fs::exists(tmp.path / kUser / "0123456789abcdef01234567.media"));
    CHECK(store.load(kUser, me.privateKey, id));
}

TEST_CASE("a draft with an odd field opens with that field back at its default", "[stories][drafts]") {
    const std::string json = R"j({"v":1,"content":{"mode":"media","text":5,"background":"url(x)","font":"comic","overlays":[{"type":"text","text":"ok","x":0.5,"y":0.5},{"type":"text","text":"off","x":9,"y":0.5},"nope"],"stickers":[{"id":"bad","type":"poll"}],"audience":"everyone","customIds":["a",3,null,"b"],"allowReplies":"no","ttlSeconds":"soon"},"summary":{"preview":"x"},"media":null})j";
    const auto d = ms::parseSealedDraft(json);
    REQUIRE(d);
    CHECK(d->content.mode == "text");  // it says media, but has none to show
    CHECK(d->content.text.empty());
    CHECK(d->content.background == "sunset");
    CHECK(d->content.font == "classic");
    REQUIRE(d->content.overlays.size() == 1);
    CHECK(d->content.overlays[0].text == "ok");
    CHECK(d->content.stickers.empty());
    CHECK(d->content.audience == "friends");
    CHECK(d->content.customIds == std::vector<std::string>{"a", "b"});
    CHECK(d->content.allowReplies);
    CHECK(d->content.ttlSeconds == 86400);
    // Not a draft at all.
    CHECK_FALSE(ms::parseSealedDraft("nope"));
    CHECK_FALSE(ms::parseSealedDraft(R"({"v":2,"content":{}})"));
    CHECK_FALSE(ms::parseSealedDraft(R"({"v":1})"));
    CHECK_FALSE(ms::parseSealedDraft(R"([1])"));
}
