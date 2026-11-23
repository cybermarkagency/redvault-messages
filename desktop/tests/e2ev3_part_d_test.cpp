// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>
#include <sodium.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/device_store.hpp"
#include "redvault/e2ev3/key_vault.hpp"
#include "redvault/e2ev3/pairing.hpp"
#include "redvault/e2ev3/send_queue.hpp"
#include "redvault/e2ev3/stories.hpp"
#include "redvault/util/text.hpp"

// Part D on the desktop (docs/signal-integration/12-part-d-stories.md): the shared story vectors (Android reads the
// same file), the story-key store, and stories over device keys against the real engine and an in-memory server that
// applies the "API (as built)" rules: device lists and key claims only between friends or within a shared
// conversation, story-addressed mailbox items by the story rule (access rows, live, author or viewer), at most 100
// devices per request.

namespace e2 = redvault::e2ev3;
namespace sc = redvault::crypto::stories;
namespace fs = std::filesystem;
using e2::Json;
using redvault::api::ApiError;

namespace {

Json storyVectors() {
    std::ifstream in(REDVAULT_V3_STORY_VECTORS, std::ios::binary);
    REQUIRE(in.good());
    return Json::parse(in);
}

redvault::crypto::SecretBytes keyOf(std::uint8_t seed) {
    redvault::crypto::Bytes raw(32);
    for (std::size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<std::uint8_t>(seed + i);
    return redvault::crypto::SecretBytes::copyOf(raw);
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("rv-d-" + redvault::util::randomUuid());
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

// --- The shared vectors ---

TEST_CASE("part D vectors: every storykey and storyanswer decode case, with now = 2000", "[e2ev3][stories][partd]") {
    const Json v = storyVectors();
    REQUIRE(v.at("decode").size() == 11);
    const std::int64_t now = v.at("now").get<std::int64_t>();
    for (const Json& c : v.at("decode")) {
        INFO(c.at("name").get<std::string>());
        const Json& envelope = c.at("envelope");
        // The mailbox item names the story id it was addressed with, which is the envelope's own c.
        const auto decoded = e2::decodeEnvelope(envelope.dump(), envelope.at("c").get<std::string>(), now);
        const std::string accept = c.at("accept").get<std::string>();
        const auto expected = accept == "storykey"      ? e2::DecodedEnvelope::Kind::StoryKey
                              : accept == "storyanswer" ? e2::DecodedEnvelope::Kind::StoryAnswer
                                                        : e2::DecodedEnvelope::Kind::Ignore;
        CHECK(decoded.kind == expected);
        // Addressed with another id than the story's, nothing is ever accepted.
        CHECK(e2::decodeEnvelope(envelope.dump(), "eeeeeeee-0000-4000-8000-000000000001", now).kind == e2::DecodedEnvelope::Kind::Ignore);
        if (expected == e2::DecodedEnvelope::Kind::StoryKey) {
            REQUIRE(decoded.storyKey);
            CHECK(decoded.storyKey->storyId == envelope.at("storyId").get<std::string>());
            CHECK(decoded.storyKey->expiresAt == envelope.at("expiresAt").get<std::int64_t>());
            CHECK(redvault::crypto::base64Encode(decoded.storyKey->key->view()) == envelope.at("key").get<std::string>());
        }
        if (expected == e2::DecodedEnvelope::Kind::StoryAnswer) {
            REQUIRE(decoded.storyAnswer);
            CHECK(decoded.storyAnswer->stickerId == envelope.at("stickerId").get<std::string>());
            CHECK(decoded.storyAnswer->rev == envelope.at("rev").get<std::int64_t>());
            CHECK(decoded.storyAnswer->final == envelope.at("final").get<bool>());
            CHECK(decoded.storyAnswer->payload == envelope.at("payload"));
        }
    }
}

TEST_CASE("part D vectors: every RVSD case parses exactly as the server does", "[e2ev3][stories][partd]") {
    const Json v = storyVectors();
    REQUIRE(v.at("rvsd").size() == 5);
    for (const Json& c : v.at("rvsd")) {
        INFO(c.at("name").get<std::string>());
        const auto bytes = redvault::crypto::base64Decode(c.at("envelope").get<std::string>());
        const auto copies = sc::parseRvsd(bytes);
        REQUIRE(copies.has_value() == c.at("ok").get<bool>());
        if (!copies) continue;
        const Json& expected = c.at("copies");
        REQUIRE(copies->size() == expected.size());
        for (std::size_t i = 0; i < copies->size(); ++i) {
            CHECK((*copies)[i].deviceId == expected[i].at("deviceId").get<std::string>());
            CHECK((*copies)[i].sealed.size() == expected[i].at("sealedLength").get<std::size_t>());
        }
        // Written back, it is byte for byte the same.
        CHECK(sc::encodeRvsd(*copies) == bytes);
    }
}

TEST_CASE("part D: RVSD refuses a device twice, a short copy, too many devices and too many bytes", "[e2ev3][stories][partd]") {
    const std::string a = "aaaaaaaa-0000-4000-8000-000000000001";
    const redvault::crypto::Bytes copy(48, 7);
    CHECK_THROWS(sc::encodeRvsd({{a, copy}, {a, copy}}));
    CHECK_THROWS(sc::encodeRvsd({{a, redvault::crypto::Bytes(47, 7)}}));
    std::vector<sc::RvsdCopy> eleven;
    for (int i = 0; i < 11; ++i) eleven.push_back({"aaaaaaaa-0000-4000-8000-0000000000" + std::to_string(10 + i), copy});
    CHECK_THROWS(sc::encodeRvsd(eleven));
    CHECK_THROWS(sc::encodeRvsd({{a, redvault::crypto::Bytes(8200, 7)}}));
    CHECK_THROWS(sc::encodeRvsd({}));
    // Trailing bytes are refused on parse.
    auto bytes = sc::encodeRvsd({{a, copy}});
    bytes.push_back(0);
    CHECK_FALSE(sc::parseRvsd(bytes).has_value());
}

TEST_CASE("part D: an anonymous answer opens on each author device and names nobody", "[e2ev3][stories][partd]") {
    redvault::crypto::ensureSodium();
    // Two author devices' Curve25519 key pairs (the engine keeps the real ones; see the notes on openSealed).
    std::vector<std::pair<std::string, redvault::crypto::Bytes>> devices;
    std::vector<redvault::crypto::Bytes> secrets;
    for (int i = 0; i < 2; ++i) {
        redvault::crypto::Bytes pk(crypto_box_PUBLICKEYBYTES), sk(crypto_box_SECRETKEYBYTES);
        crypto_box_keypair(pk.data(), sk.data());
        devices.emplace_back("aaaaaaaa-0000-4000-8000-00000000000" + std::to_string(i + 1), pk);
        secrets.push_back(sk);
    }
    const std::string storyId = "dddddddd-0000-4000-8000-000000000001";
    const auto stickers = redvault::model::stories::parseStickers(
        R"([{"type":"question","id":"q1abcdef","x":0.5,"y":0.5,"scale":1,"rotation":0,"prompt":"Ask me","anonymous":true}])");
    REQUIRE(stickers.size() == 1);
    redvault::model::stories::Answer answer;
    answer.kind = "question";
    answer.text = "who are you?";
    const auto rvsd = sc::sealAnonymousForDevices(storyId, stickers[0], answer, devices);
    const auto copies = sc::parseRvsd(rvsd);
    REQUIRE(copies);
    REQUIRE(copies->size() == 2);
    for (std::size_t i = 0; i < devices.size(); ++i) {
        const auto opened = sc::openRvsdCopy(storyId, stickers[0], rvsd, devices[i].first, devices[i].second, secrets[i]);
        CHECK(opened.text == "who are you?");
        // Through the device-side helper, with that device's opener.
        const auto pk = devices[i].second;
        const auto sk = secrets[i];
        const e2::SealedOpener open = [pk, sk](redvault::crypto::ByteView sealed) -> std::optional<redvault::crypto::Bytes> {
            redvault::crypto::Bytes out(sealed.size() - crypto_box_SEALBYTES);
            if (crypto_box_seal_open(out.data(), sealed.data(), sealed.size(), pk.data(), sk.data()) != 0) return std::nullopt;
            return out;
        };
        const auto viaDevice = e2::openStoryAnswer(devices[i].first, storyId, stickers[0], rvsd, open);
        REQUIRE(viaDevice);
        CHECK(viaDevice->text == "who are you?");
        // Another device id has no copy here.
        CHECK_FALSE(e2::openStoryAnswer("bbbbbbbb-0000-4000-8000-000000000009", storyId, stickers[0], rvsd, open));
    }
    // One device's key doesn't open another's copy.
    CHECK_THROWS(sc::openRvsdCopy(storyId, stickers[0], rvsd, devices[0].first, devices[1].second, secrets[1]));
    // Another story or sticker: refused.
    CHECK_THROWS(sc::openRvsdCopy("dddddddd-0000-4000-8000-000000000002", stickers[0], rvsd, devices[0].first, devices[0].second, secrets[0]));
}

TEST_CASE("part D: story envelopes round-trip and refuse what wouldn't decode", "[e2ev3][stories][partd]") {
    const std::string storyId = "dddddddd-0000-4000-8000-000000000001";
    const auto key = keyOf(3);
    const std::string id = redvault::util::randomUuid();
    const auto decoded = e2::decodeEnvelope(e2::encodeStoryKey(storyId, key, 90000000, id, 1000), storyId, 2000);
    REQUIRE(decoded.kind == e2::DecodedEnvelope::Kind::StoryKey);
    CHECK(decoded.id == id);
    CHECK(decoded.storyKey->key->view().size() == 32);
    CHECK(std::equal(decoded.storyKey->key->view().begin(), decoded.storyKey->key->view().end(), key.view().begin()));
    const auto answer = e2::decodeEnvelope(e2::encodeStoryAnswer(storyId, "poll1", 2, false, Json{{"choice", 1}}, id, 1000), storyId, 2000);
    REQUIRE(answer.kind == e2::DecodedEnvelope::Kind::StoryAnswer);
    CHECK(answer.storyAnswer->rev == 2);
    CHECK_THROWS(e2::encodeStoryAnswer(storyId, "", 1, true, Json{{"choice", 1}}, id, 1000));
    CHECK_THROWS(e2::encodeStoryAnswer(storyId, "poll1", 0, true, Json{{"choice", 1}}, id, 1000));
    CHECK_THROWS(e2::encodeStoryAnswer(storyId, "poll1", 1, true, Json("1"), id, 1000));
    CHECK_THROWS(e2::encodeStoryKey("nope", key, 90000000, id, 1000));
    // A text that replies to a story carries the reference; malformed, the whole text is ignored.
    e2::Envelope e;
    e.id = id;
    e.c = "11111111-1111-4111-8111-111111111111";
    e.ts = 1000;
    e.body = "nice";
    e.extras.storyReply = redvault::model::StoryReplyRef{"aaaaaaaa-0000-4000-8000-000000000001", storyId, "image", "Photo", false};
    const auto reply = e2::decodeEnvelope(e2::encodeEnvelope(e), e.c, 2000);
    REQUIRE(reply.kind == e2::DecodedEnvelope::Kind::Message);
    REQUIRE(reply.extras.storyReply);
    CHECK(*reply.extras.storyReply == *e.extras.storyReply);
    Json bad = Json::parse(e2::encodeEnvelope(e));
    bad["storyReply"]["reaction"] = nullptr;
    CHECK(e2::decodeEnvelope(bad.dump(), e.c, 2000).kind == e2::DecodedEnvelope::Kind::Ignore);
    bad["storyReply"]["reaction"] = true;
    bad["storyReply"]["preview"] = std::string(81, 'x');
    CHECK(e2::decodeEnvelope(bad.dump(), e.c, 2000).kind == e2::DecodedEnvelope::Kind::Ignore);
}

TEST_CASE("part D: the story-key store is sealed, keeps the latest answer, and keeps K until the story is gone", "[e2ev3][stories][partd]") {
    TempDir dir;
    const auto file = dir.path / "stories";
    const auto sealKey = redvault::crypto::randomBytes(32);
    std::int64_t clock = 1000;
    const std::string s1 = "dddddddd-0000-4000-8000-000000000001", s2 = "dddddddd-0000-4000-8000-000000000002";
    const std::string alice = "aaaaaaaa-0000-4000-8000-000000000001", bob = "bbbbbbbb-0000-4000-8000-000000000001";
    {
        e2::StoryStore store(file, sealKey);
        store.setClock([&] { return clock; });
        REQUIRE(store.putKey(s1, alice, keyOf(1), 5000));
        CHECK_FALSE(store.putKey(s1, alice, keyOf(2), 5000));  // the first key from that author stays
        CHECK(store.putKey(s1, bob, keyOf(2), 5000));  // someone else's key for that id is kept apart (and never used for alice's story)
        CHECK(store.putKey(s2, alice, keyOf(3), 900));  // already ended: kept (a highlight still opens), until forgotten
        REQUIRE(store.putKey(s2, bob, keyOf(4), 2000, true));  // own: kept past its end
        CHECK(store.putAnswer({s1, "poll1", bob, 1, false, R"({"choice":0})", 1}));
        CHECK(store.putAnswer({s1, "poll1", bob, 3, false, R"({"choice":2})", 2}));
        CHECK_FALSE(store.putAnswer({s1, "poll1", bob, 2, false, R"({"choice":1})", 3}));  // older rev
        CHECK(store.putAnswer({s1, "quiz12", bob, 1, true, R"({"choice":1})", 4}));
        CHECK_FALSE(store.putAnswer({s1, "quiz12", bob, 2, true, R"({"choice":0})", 5}));  // final
    }
    // Nothing readable without the key.
    {
        std::ifstream in(file, std::ios::binary);
        const std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(blob.find("poll1") == std::string::npos);
        CHECK(blob.find(redvault::crypto::base64Encode(keyOf(1).view())) == std::string::npos);
    }
    {
        e2::StoryStore other(file, redvault::crypto::randomBytes(32));
        CHECK(other.keys().empty());
    }
    e2::StoryStore store(file, sealKey);
    store.setClock([&] { return clock; });
    REQUIRE(store.key(s1, alice));
    CHECK(store.answer(s1, "poll1", bob)->rev == 3);
    CHECK(store.answer(s1, "quiz12", bob)->payload == R"({"choice":1})");
    clock = 6000;
    // Past its end time K stays (archive and highlights still open); it goes when the story is gone.
    REQUIRE(store.key(s1, alice));
    store.forget(s1);
    CHECK_FALSE(store.key(s1, alice));
    CHECK_FALSE(store.key(s1, bob));
    CHECK(store.answers(s1).empty());  // its answers go with it
    REQUIRE(store.key(s2, bob));  // own
    store.forget(s2);
    CHECK_FALSE(store.key(s2, bob));
    CHECK(store.keys().empty());
}

#ifdef REDVAULT_HAS_E2E_SIGNAL

namespace {

struct Story {
    std::string author;
    std::set<std::string> access;  // the author's row included
    std::int64_t expiresAt = 0;
    bool highlighted = false;
    bool deleted = false;
    bool published = true;
};

struct State {
    struct Device {
        Json identity;
        Json oneTimeKeys = Json::array();
        Json fallbackKey;
    };
    struct SignalEntry {
        Json certificate, registrationId, identityKey, signedPreKey, lastResort;
        std::vector<Json> preKeys, kyberPreKeys;
    };
    struct Enrollment {
        std::string id, userId, newDeviceId;
        std::vector<Json> messages;
    };
    std::recursive_mutex mutex;
    std::map<std::string, Device> devices;
    std::map<std::string, Json> lists;
    std::map<std::string, SignalEntry> signal;
    std::map<std::string, std::vector<Json>> mailboxes;
    std::map<std::string, Enrollment> enrollments;
    std::set<std::pair<std::string, std::string>> friends;
    std::map<std::string, std::set<std::string>> conversations;
    std::map<std::string, Story> stories;
    int counter = 0;
    std::int64_t now = 1'000'000;
    /// Every message request that reached the server: (conversation, how many devices); refused ones marked "!".
    std::vector<std::pair<std::string, std::size_t>> requests;
    // Gap 1: each accepted request's allDevices mark.
    std::vector<bool> marked;
    std::set<std::string> torDown;

    bool areFriends(const std::string& a, const std::string& b) { return friends.count({std::min(a, b), std::max(a, b)}) > 0; }
    void befriend(const std::string& a, const std::string& b) { friends.insert({std::min(a, b), std::max(a, b)}); }
    bool bothIn(const std::string& conv, const std::string& a, const std::string& b) {
        const auto it = conversations.find(conv);
        return it != conversations.end() && it->second.count(a) && it->second.count(b);
    }
};

ApiError refusal(int status, const std::string& code) {
    return ApiError(status, code, code);
}

class FakeServer final : public e2::V3Server {
public:
    FakeServer(std::shared_ptr<State> state, std::string userId) : s_(std::move(state)), user_(std::move(userId)) {}

    Json config() override { return Json{{"enabled", true}, {"resetDelayHours", 72}, {"signal", true}}; }
    void gate(const char* call) const {
        if (s_->torDown.count("*") || s_->torDown.count(call)) throw ApiError(0, "tor_unavailable", "Tor isn't connected");
    }
    /// The server's mayLookUp / mayClaim: yourself; within a conversation both are in; under a device-keys story id its
    /// story rule (author <-> viewers with access while live or highlighted; viewer -> author while published and live);
    /// otherwise friends (lookups only).
    void mayLookUp(const std::string& target, const std::string& conv, bool claim) const {
        if (target == user_) return;
        if (!conv.empty() && !s_->conversations.count(conv) && s_->stories.count(conv)) {
            const Story& st = s_->stories[conv];
            const bool live = st.expiresAt > s_->now;
            const bool ok = !st.deleted && (st.author == user_ ? (live || st.highlighted) && st.access.count(target)
                                                                : st.author == target && st.published && live && st.access.count(user_));
            if (!ok) throw refusal(404, "not_found");
            return;
        }
        if (!conv.empty()) {
            if (!s_->bothIn(conv, user_, target)) throw refusal(404, "not_found");
            return;
        }
        if (claim || !s_->areFriends(user_, target)) throw refusal(404, "not_found");
    }

    Json signalUpload(const std::string& deviceId, const Json& upload) override {
        std::lock_guard lock(s_->mutex);
        auto& e = s_->signal[deviceId];
        e.certificate = upload.at("certificate");
        e.registrationId = upload.at("registrationId");
        e.identityKey = upload.at("identityKey");
        e.signedPreKey = upload.at("signedPreKey");
        e.lastResort = upload.at("lastResortKyberPreKey");
        for (const auto& k : upload.at("preKeys")) e.preKeys.push_back(k);
        for (const auto& k : upload.at("kyberPreKeys")) e.kyberPreKeys.push_back(k);
        return Json{{"enabled", true}, {"preKeys", e.preKeys.size()}, {"kyberPreKeys", e.kyberPreKeys.size()}};
    }
    Json signalStatus(const std::string& deviceId) override {
        std::lock_guard lock(s_->mutex);
        if (!s_->signal.count(deviceId)) return Json{{"enabled", false}, {"preKeys", 0}, {"kyberPreKeys", 0}};
        const auto& e = s_->signal[deviceId];
        return Json{{"enabled", true}, {"preKeys", e.preKeys.size()}, {"kyberPreKeys", e.kyberPreKeys.size()}};
    }
    Json signalClaim(const std::string& conversationId, const Json& devices) override {
        gate("claim");
        std::lock_guard lock(s_->mutex);
        REQUIRE(devices.size() <= 100);
        Json out = Json::array();
        for (const auto& want : devices) {
            mayLookUp(want.at("userId").get<std::string>(), conversationId, true);
            const std::string id = want.at("deviceId").get<std::string>();
            Json item = {{"userId", want.at("userId")}, {"deviceId", id}, {"certificate", nullptr}, {"bundle", nullptr}};
            if (s_->signal.count(id)) {
                auto& e = s_->signal[id];
                Json pre = nullptr;
                if (!e.preKeys.empty()) {
                    pre = e.preKeys.front();
                    e.preKeys.erase(e.preKeys.begin());
                }
                Json kyber = e.lastResort;
                bool lastResort = true;
                if (!e.kyberPreKeys.empty()) {
                    kyber = e.kyberPreKeys.front();
                    e.kyberPreKeys.erase(e.kyberPreKeys.begin());
                    lastResort = false;
                }
                item["certificate"] = e.certificate;
                item["bundle"] = {{"registrationId", e.registrationId}, {"identityKey", e.identityKey}, {"signedPreKey", e.signedPreKey},
                                  {"kyberPreKey", kyber}, {"lastResort", lastResort}, {"preKey", pre}};
            }
            out.push_back(item);
        }
        return out;
    }
    Json signalCertificates(const std::string& userId, const std::string& conversationId) override {
        gate("getDeviceList");
        std::lock_guard lock(s_->mutex);
        mayLookUp(userId, conversationId, false);
        Json out = Json::array();
        if (!s_->lists.count(userId)) return out;
        for (const auto& d : s_->lists[userId].at("devices")) {
            const std::string id = d.at("deviceId").get<std::string>();
            if (s_->signal.count(id)) out.push_back({{"deviceId", id}, {"certificate", s_->signal[id].certificate}});
        }
        return out;
    }
    Json registerDevice(const Json& upload) override {
        std::lock_guard lock(s_->mutex);
        auto& d = s_->devices[upload.at("identity").at("deviceId").get<std::string>()];
        d.identity = upload.at("identity");
        for (const auto& k : upload.at("oneTimeKeys")) d.oneTimeKeys.push_back(k);
        d.fallbackKey = upload.value("fallbackKey", Json(nullptr));
        return Json{{"oneTimeKeyCount", d.oneTimeKeys.size()}};
    }
    Json listDevices() override { return Json::array(); }
    void removeDevice(const std::string& deviceId) override {
        std::lock_guard lock(s_->mutex);
        s_->devices.erase(deviceId);
    }
    Json getDeviceList(const std::string& userId, const std::string& conversationId) override {
        gate("getDeviceList");
        std::lock_guard lock(s_->mutex);
        mayLookUp(userId, conversationId, false);
        return Json{{"deviceList", s_->lists.count(userId) ? s_->lists[userId] : Json(nullptr)}, {"pendingReset", nullptr}};
    }
    Json putDeviceList(const Json& list, const Json&) override {
        std::lock_guard lock(s_->mutex);
        const std::string userId = list.at("userId");
        const int current = s_->lists.count(userId) ? s_->lists[userId].at("version").get<int>() : 0;
        if (list.at("version").get<int>() != current + 1) throw refusal(409, "stale_device_list");
        s_->lists[userId] = list;
        return Json{{"status", "accepted"}};
    }
    void cancelReset(const std::string&) override {}
    Json claimKeys(const std::string& conversationId, const Json& devices) override {
        gate("claim");
        std::lock_guard lock(s_->mutex);
        Json out = Json::array();
        for (const auto& want : devices) {
            mayLookUp(want.at("userId").get<std::string>(), conversationId, true);
            auto& d = s_->devices.at(want.at("deviceId").get<std::string>());
            Json key = nullptr;
            if (!d.oneTimeKeys.empty()) {
                key = d.oneTimeKeys.front();
                d.oneTimeKeys.erase(d.oneTimeKeys.begin());
            }
            out.push_back({{"userId", want.at("userId")}, {"deviceId", want.at("deviceId")}, {"oneTimeKey", key}, {"fallbackKey", d.fallbackKey}});
        }
        return out;
    }
    /// The mailbox rule for an id, as built: a conversation's rule, else a device-keys story's, else nothing but yourself.
    void checkSend(const std::string& conv, const Json& messages) {
        std::set<std::string> addressed;
        for (const auto& m : messages) addressed.insert(m.at("userId").get<std::string>());
        if (!conv.empty() && s_->conversations.count(conv)) {
            for (const auto& u : addressed) {
                if (u != user_ && !s_->bothIn(conv, user_, u)) throw refusal(404, "not_found");
            }
            return;
        }
        if (!conv.empty() && s_->stories.count(conv)) {
            const Story& st = s_->stories[conv];
            const bool live = st.expiresAt > s_->now;
            bool ok = !st.deleted;
            if (st.author == user_) {
                ok = ok && (live || st.highlighted);
                for (const auto& u : addressed) ok = ok && (u == user_ || st.access.count(u));
            } else {
                ok = ok && st.published && live && st.access.count(user_);
                for (const auto& u : addressed) ok = ok && (u == st.author || u == user_);
            }
            if (!ok) throw refusal(404, "story_recipient_refused");
            return;
        }
        for (const auto& u : addressed) {
            if (u != user_) throw refusal(404, "not_found");
        }
    }
    void sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages, bool allDevices) override {
        gate("sendMessages");
        std::lock_guard lock(s_->mutex);
        if (messages.empty() || messages.size() > 100) throw refusal(400, "validation_error");
        try {
            checkSend(conversationId, messages);
        } catch (const ApiError&) {
            s_->requests.emplace_back("!" + conversationId, messages.size());
            throw;
        }
        s_->requests.emplace_back(conversationId, messages.size());
        s_->marked.push_back(allDevices);
        for (const auto& m : messages) {
            s_->mailboxes[m.at("deviceId").get<std::string>()].push_back(
                {{"id", "m" + std::to_string(++s_->counter)},
                 {"senderUserId", user_},
                 {"senderDeviceId", senderDeviceId},
                 {"conversationId", conversationId.empty() ? Json(nullptr) : Json(conversationId)},
                 {"body", m.at("body")}});
        }
    }
    Json conversationDeviceLists(const std::string&) override { return Json{{"members", Json::array()}}; }
    void sendGroupMessage(const std::string&, const std::string&, const std::string&, const Json&, bool) override {
        throw refusal(404, "not_found");  // never used for stories
    }
    Json inbox(const std::string& deviceId) override {
        std::lock_guard lock(s_->mutex);
        return Json(s_->mailboxes[deviceId]);
    }
    void ack(const std::string& deviceId, const Json& ids) override {
        std::lock_guard lock(s_->mutex);
        std::erase_if(s_->mailboxes[deviceId], [&](const Json& item) {
            for (const auto& id : ids) {
                if (id == item.at("id")) return true;
            }
            return false;
        });
    }
    Json createEnrollment(const std::string& newDeviceId) override {
        std::lock_guard lock(s_->mutex);
        const std::string id = "e" + std::to_string(++s_->counter);
        s_->enrollments[id] = {id, user_, newDeviceId, {}};
        return Json{{"id", id}, {"newDeviceId", newDeviceId}};
    }
    Json listEnrollments() override {
        std::lock_guard lock(s_->mutex);
        Json out = Json::array();
        for (const auto& [id, e] : s_->enrollments) {
            if (e.userId == user_) out.push_back({{"id", id}, {"newDeviceId", e.newDeviceId}});
        }
        return out;
    }
    Json postEnrollmentMessage(const std::string& id, const std::string& from, const std::string& body) override {
        std::lock_guard lock(s_->mutex);
        auto& e = s_->enrollments.at(id);
        e.messages.push_back({{"seq", e.messages.size() + 1}, {"fromDeviceId", from}, {"body", body}});
        return Json{{"seq", e.messages.size()}};
    }
    Json enrollmentMessages(const std::string& id, int) override {
        std::lock_guard lock(s_->mutex);
        const auto it = s_->enrollments.find(id);
        return it == s_->enrollments.end() ? Json::array() : Json(it->second.messages);
    }
    void deleteEnrollment(const std::string& id) override {
        std::lock_guard lock(s_->mutex);
        s_->enrollments.erase(id);
    }
    void putRecoveryBackup(const std::string&, const std::string&) override {}
    Json getRecoveryBackup() override { return Json::object(); }
    Json events(int) override { return Json::array(); }

private:
    std::shared_ptr<State> s_;
    std::string user_;
};

e2::WaitOptions quick() {
    e2::WaitOptions wait;
    wait.intervalMs = 5;
    wait.timeoutMs = 20000;
    return wait;
}

/// One computer of one account: its device, its device-key history and its story-key store.
struct Computer {
    TempDir dir;
    e2::Backend backend;
    std::shared_ptr<e2::QueueingV3Server> queueing;
    std::unique_ptr<e2::V3Device> device;
    std::unique_ptr<e2::ChatHistory> history;
    std::unique_ptr<e2::StoryStore> stories;
    std::shared_ptr<State> state;
    std::string userId;

    Computer(const std::shared_ptr<State>& s, const std::string& user, bool queued = false) : state(s), userId(user) {
        backend.store = std::make_shared<e2::DeviceStore>(dir.path, e2::makeFileVault(dir.path / "keys"));
        auto server = std::make_shared<FakeServer>(s, user);
        if (queued) {
            queueing = std::make_shared<e2::QueueingV3Server>(server);
            backend.server = queueing;
        } else {
            backend.server = server;
        }
    }
    void firstDevice(char seed) {
        device = e2::setUpFirstDevice(userId, redvault::crypto::toBytes(std::string(32, seed)), backend);
        ready();
    }
    void linkTo(Computer& first) {
        auto second = e2::startNewDevice(userId, backend);
        auto joining = e2::NewDevicePairing::begin(*second);
        std::unique_ptr<e2::TrustedPairing> answering;
        std::thread t([&] { answering = std::move(e2::TrustedPairing::begin(*first.device, joining->enrollmentId(), quick()).pairing); });
        joining->codes(quick());
        t.join();
        std::thread t2([&] { answering->confirm(quick()); });
        joining->confirm(quick());
        t2.join();
        device = std::move(second);
        ready();
    }
    std::vector<e2::ChatRecord> receive() { return e2::receiveChat(*device, *history, 0, nullptr, nullptr, stories.get()); }
    /// Lists and claims under the story id, as the app does.
    e2::StoryFanOut fanOut() {
        e2::StoryFanOut f;
        f.listVia = [](const std::string& storyId, const std::string&) { return storyId; };
        return f;
    }

private:
    void ready() {
        history = std::make_unique<e2::ChatHistory>(dir.path / ("history-" + device->deviceId()), device->deriveKey("redvault-v3-chat-history"));
        stories = std::make_unique<e2::StoryStore>(dir.path / ("stories-" + device->deviceId()), device->deriveKey("redvault-v3-story-keys"));
        stories->setClock([st = state] {
            std::lock_guard lock(st->mutex);
            return st->now;
        });
        if (queueing) queueing->attach(std::make_shared<e2::SendQueue>(dir.path / "sendq", device->deriveKey("redvault-v3-send-queue")));
        REQUIRE(e2::maintainSignal(*device, backend.server->config()) != "off");
    }
};

std::string uuid() {
    return redvault::util::randomUuid();
}

void upkeep(const std::vector<Computer*>& computers) {
    for (int round = 0; round < 2; ++round) {
        for (Computer* c : computers) REQUIRE(e2::maintainSignal(*c->device, c->backend.server->config()) != "off");
    }
}

/// Friends with a 1:1 chat, as the app has for everyone it posts stories to.
void friendsWithChat(State& s, const std::string& a, const std::string& b) {
    s.befriend(a, b);
    s.conversations[uuid()] = {a, b};
}

/// The server's POST /stories for a device-keys story: the author and the viewers get access rows.
std::string createStory(State& s, const std::string& author, const std::vector<std::string>& viewers, std::int64_t ttlMs = 3600 * 1000) {
    const std::string id = uuid();
    Story st;
    st.author = author;
    st.access.insert(author);
    for (const auto& v : viewers) {
        REQUIRE(s.areFriends(author, v));
        st.access.insert(v);
    }
    st.expiresAt = s.now + ttlMs;
    s.stories[id] = st;
    return id;
}

bool opens(Computer& c, const std::string& storyId, const std::string& author, const redvault::crypto::SecretBytes& expected) {
    const auto k = c.stories->key(storyId, author);
    if (!k) return false;
    // The key opens the story's details the author sealed with it.
    const auto sealed = sc::sealMeta(storyId, expected, R"({"kind":"text","text":"hello"})");
    return sc::openMeta(storyId, *k->key, sealed.ciphertext, sealed.nonce) == R"({"kind":"text","text":"hello"})";
}

const auto kQuestion = R"([{"type":"question","id":"q1abcdef","x":0.5,"y":0.5,"scale":1,"rotation":0,"prompt":"Ask me","anonymous":true}])";

}  // namespace

TEST_CASE("part D: a story posted to three viewers, one with a linked device, opens on every viewer device and nowhere else",
          "[e2ev3][stories][partd][engine]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid(), carol = uuid(), dave = uuid(), eve = uuid();
    Computer a1(state, alice), a2(state, alice), b(state, bob), c1(state, carol), c2(state, carol), d(state, dave), e(state, eve);
    a1.firstDevice('A');
    a2.linkTo(a1);
    b.firstDevice('B');
    c1.firstDevice('C');
    c2.linkTo(c1);
    d.firstDevice('D');
    e.firstDevice('E');
    for (const auto& other : {bob, carol, dave, eve}) friendsWithChat(*state, alice, other);
    upkeep({&a1, &a2, &b, &c1, &c2, &d, &e});

    const std::string storyId = createStory(*state, alice, {bob, carol, dave});
    const auto key = sc::newStoryKey();
    const std::int64_t expiresAt = state->stories[storyId].expiresAt;
    state->marked.clear();
    const auto sent = e2::sendStoryKey(*a1.device, *a1.stories, storyId, key, expiresAt, {bob, carol, dave}, a1.fanOut());
    CHECK(sent.unreachable == 0);
    // Gap 1: nothing left out and nothing already done, so every request was marked allDevices.
    CHECK_FALSE(state->marked.empty());
    for (const bool m : state->marked) CHECK(m);
    CHECK_FALSE(sent.waiting);
    CHECK(sent.delivered == 5);  // bob, carol's two devices, dave, and alice's other device
    CHECK_FALSE(a1.stories->hasDeliveries());
    // Every request was addressed with the story id, at most 100 devices each.
    for (const auto& [conv, count] : state->requests) {
        if (conv == storyId) CHECK(count <= 100);
    }

    for (Computer* viewer : {&b, &c1, &c2, &d}) {
        CHECK(viewer->receive().empty());  // nothing shows in any chat
        CHECK(opens(*viewer, storyId, alice, key));
        CHECK(viewer->history->records(storyId).empty());
    }
    // The author's other device keeps it as its own; the posting device too.
    a2.receive();
    REQUIRE(a2.stories->key(storyId, alice));
    CHECK(a2.stories->key(storyId, alice)->own);
    CHECK(opens(a1, storyId, alice, key));

    // A friend who isn't a viewer gets nothing, and the server refuses to address one.
    e.receive();
    CHECK(e.stories->keys().empty());
    CHECK(state->mailboxes[e.device->deviceId()].empty());
    const auto refused = e2::sendStoryKey(*a1.device, *a1.stories, storyId, key, expiresAt, {eve}, a1.fanOut());
    // Under the story id the server hands out no list for an account without access: only alice's other device gets it.
    CHECK(refused.delivered == 1);
    CHECK_FALSE(a1.stories->hasDeliveries());
    CHECK(state->mailboxes[e.device->deviceId()].empty());
    // Addressed to eve's device anyway, the mailbox refuses the whole request (the story rule).
    std::vector<Json> eveDevices;
    for (const Json& dev : e2::verifiedDevices(*a1.device, eve).devices) eveDevices.push_back(dev);
    REQUIRE_THROWS_AS(e2::sendToDevices(*a1.device, storyId, eveDevices, e2::encodeStoryKey(storyId, key, expiresAt, uuid(), state->now)), ApiError);
    e.receive();
    CHECK(e.stories->keys().empty());

    // A viewer may address the author (that's how answers go), so a "key" from bob reaches alice's devices: it's kept
    // apart under bob, and the story, whose author the server names as alice, still opens with alice's key only.
    std::vector<Json> aliceDevices;
    for (const Json& dev : e2::verifiedDevices(*b.device, alice).devices) aliceDevices.push_back(dev);
    e2::FanOut via;
    via.claimVia = b.fanOut().claimVia;
    const auto forged = sc::newStoryKey();
    e2::sendToDevices(*b.device, storyId, aliceDevices, e2::encodeStoryKey(storyId, forged, expiresAt, uuid(), state->now), nullptr, via);
    a2.receive();
    REQUIRE(a2.stories->key(storyId, bob));
    CHECK(opens(a2, storyId, alice, key));
    // And the server never lets a viewer address another viewer with the story's id.
    friendsWithChat(*state, bob, dave);
    std::vector<Json> daveDevices;
    for (const Json& dev : e2::verifiedDevices(*b.device, dave).devices) daveDevices.push_back(dev);
    REQUIRE_THROWS_AS(e2::sendToDevices(*b.device, storyId, daveDevices, e2::encodeStoryKey(storyId, forged, expiresAt, uuid(), state->now),
                                        nullptr, via),
                      ApiError);
}

TEST_CASE("part D: keys are claimed under the story id, so a viewer with no chat at all is reached", "[e2ev3][stories][partd][engine]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid();
    Computer a(state, alice), b(state, bob);
    a.firstDevice('A');
    b.firstDevice('B');
    state->befriend(alice, bob);  // friends, but no chat (no conversation to claim through)
    upkeep({&a, &b});
    const std::string storyId = createStory(*state, alice, {bob});
    const auto key = sc::newStoryKey();
    const auto sent = e2::sendStoryKey(*a.device, *a.stories, storyId, key, state->stories[storyId].expiresAt, {bob}, a.fanOut());
    CHECK(sent.unreachable == 0);
    CHECK(sent.delivered == 1);
    CHECK_FALSE(a.stories->hasDeliveries());
    b.receive();
    CHECK(opens(b, storyId, alice, key));
    // The story id opens nothing for an account without access: no list, no claim.
    const std::string other = createStory(*state, alice, {});
    REQUIRE_THROWS_AS(a.backend.server->getDeviceList(bob, other), ApiError);
    REQUIRE_THROWS_AS(a.backend.server->claimKeys(other, Json::array({Json{{"userId", bob}, {"deviceId", b.device->deviceId()}}})), ApiError);
}

TEST_CASE("part D: named answers reach the author's devices, the latest rev wins, and anonymous ones name nobody",
          "[e2ev3][stories][partd][engine]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid(), carol = uuid();
    Computer a1(state, alice), a2(state, alice), b(state, bob), c1(state, carol), c2(state, carol);
    a1.firstDevice('A');
    a2.linkTo(a1);
    b.firstDevice('B');
    c1.firstDevice('C');
    c2.linkTo(c1);
    friendsWithChat(*state, alice, bob);
    friendsWithChat(*state, alice, carol);
    upkeep({&a1, &a2, &b, &c1, &c2});
    const std::string storyId = createStory(*state, alice, {bob, carol});
    const std::int64_t expiresAt = state->stories[storyId].expiresAt;
    const auto key = sc::newStoryKey();
    e2::sendStoryKey(*a1.device, *a1.stories, storyId, key, expiresAt, {bob, carol}, a1.fanOut());
    for (Computer* c : {&a2, &b, &c1, &c2}) c->receive();

    e2::sendStoryAnswer(*b.device, *b.stories, storyId, alice, "poll1abc", false, Json{{"choice", 0}}, expiresAt, b.fanOut());
    e2::sendStoryAnswer(*b.device, *b.stories, storyId, alice, "poll1abc", false, Json{{"choice", 2}}, expiresAt, b.fanOut());
    e2::sendStoryAnswer(*c1.device, *c1.stories, storyId, alice, "poll1abc", true, Json{{"choice", 1}}, expiresAt, c1.fanOut());
    CHECK_THROWS_AS(e2::sendStoryAnswer(*c1.device, *c1.stories, storyId, alice, "poll1abc", true, Json{{"choice", 0}}, expiresAt, c1.fanOut()),
                    e2::EngineError);
    for (Computer* author : {&a1, &a2}) {
        CHECK(author->receive().empty());
        const auto fromBob = author->stories->answer(storyId, "poll1abc", bob);
        REQUIRE(fromBob);
        CHECK(fromBob->rev == 2);
        CHECK(Json::parse(fromBob->payload) == Json{{"choice", 2}});
        const auto fromCarol = author->stories->answer(storyId, "poll1abc", carol);
        REQUIRE(fromCarol);
        CHECK(fromCarol->final);
    }
    // Carol's other device learns her own answer; nobody else does.
    c2.receive();
    REQUIRE(c2.stories->answer(storyId, "poll1abc", carol));
    CHECK_FALSE(c2.stories->answer(storyId, "poll1abc", bob));
    b.receive();
    CHECK_FALSE(b.stories->answer(storyId, "poll1abc", carol));
    // A viewer can't answer to anyone but the author (the server's rule), nor after the story ends.
    state->now = expiresAt + 1;
    e2::sendStoryAnswer(*b.device, *b.stories, storyId, alice, "quiz1abc", true, Json{{"choice", 0}}, expiresAt + 60000, b.fanOut());
    a1.receive();
    CHECK_FALSE(a1.stories->answer(storyId, "quiz1abc", bob));
    state->now = expiresAt - 1000;

    // Anonymous: one sealed copy per author device from the verified list, and nothing in it names bob.
    const auto stickers = redvault::model::stories::parseStickers(kQuestion);
    redvault::model::stories::Answer answer;
    answer.kind = "question";
    answer.text = "a secret question";
    const auto rvsd = e2::sealStoryAnswer(storyId, alice, stickers[0], answer, e2::engineSealer(*b.device, b.fanOut()));
    const auto copies = sc::parseRvsd(rvsd);
    REQUIRE(copies);
    std::set<std::string> ids;
    for (const auto& c : *copies) ids.insert(c.deviceId);
    CHECK(ids == std::set<std::string>{a1.device->deviceId(), a2.device->deviceId()});
    const std::string bytes(rvsd.begin(), rvsd.end());
    CHECK(bytes.find(bob) == std::string::npos);
    CHECK(bytes.find(b.device->deviceId()) == std::string::npos);
    CHECK(bytes.find("a secret question") == std::string::npos);
    // Each author device opens its own copy inside the engine and learns the answer, not who sent it.
    for (Computer* author : {&a1, &a2}) {
        const auto opened = e2::openStoryAnswer(author->device->deviceId(), storyId, stickers[0], rvsd, e2::engineOpener(*author->device));
        REQUIRE(opened);
        CHECK(opened->text == "a secret question");
    }
    // One device's copy doesn't open on another, and a viewer can't open any.
    CHECK_FALSE(e2::openStoryAnswer(a2.device->deviceId(), storyId, stickers[0], rvsd, e2::engineOpener(*a1.device)));
    CHECK_FALSE(e2::openStoryAnswer(c1.device->deviceId(), storyId, stickers[0], rvsd, e2::engineOpener(*c1.device)));
    // The engine refuses a certificate that isn't the one pinned for that device: no copy is made for it.
    const e2::StorySealer forged = [&](const std::string&, const std::string&, redvault::crypto::ByteView payload) {
        std::vector<sc::RvsdCopy> out;
        Json certificate;
        {
            std::lock_guard lock(state->mutex);
            certificate = state->signal[c1.device->deviceId()].certificate;  // carol's, claimed to be alice's
        }
        const Json sealed = b.device->op("storySeal", Json{{"certificate", certificate}, {"plaintext", redvault::crypto::base64Encode(payload)}});
        if (sealed.value("ok", false)) out.push_back({a1.device->deviceId(), redvault::crypto::base64Decode(sealed.at("sealed").get<std::string>())});
        return out;
    };
    // Bob's engine never verified carol's list, so it refuses her certificate; had it sealed, it would be to carol, not alice.
    try {
        const auto misdirected = e2::sealStoryAnswer(storyId, alice, stickers[0], answer, forged);
        CHECK_FALSE(e2::openStoryAnswer(a1.device->deviceId(), storyId, stickers[0], misdirected, e2::engineOpener(*a1.device)));
    } catch (const e2::EngineError& error) {
        CHECK(error.code() == "story_seal_unavailable");
    }
}

TEST_CASE("part D: replies and reactions to a story go over device keys in the 1:1 chat", "[e2ev3][stories][partd][engine]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid();
    Computer a(state, alice), b(state, bob);
    a.firstDevice('A');
    b.firstDevice('B');
    const std::string chat = uuid();
    state->befriend(alice, bob);
    state->conversations[chat] = {alice, bob};
    upkeep({&a, &b});
    const std::string storyId = uuid();
    e2::OutgoingMessage reply;
    reply.text = "love this";
    reply.extras.storyReply = redvault::model::StoryReplyRef{alice, storyId, "image", "Photo", false};
    e2::sendChatMessage(*b.device, *b.history, chat, alice, reply);
    e2::OutgoingMessage reaction;
    reaction.text = "\xF0\x9F\x94\xA5";
    reaction.extras.storyReply = redvault::model::StoryReplyRef{alice, storyId, "image", "Photo", true};
    e2::sendChatMessage(*b.device, *b.history, chat, alice, reaction);
    const auto got = a.receive();
    REQUIRE(got.size() == 2);
    REQUIRE(got[0].storyReply);
    CHECK(got[0].text == "love this");
    CHECK_FALSE(got[0].storyReply->reaction);
    CHECK(got[0].storyReply->storyId == storyId);
    REQUIRE(got[1].storyReply);
    CHECK(got[1].storyReply->reaction);
    // Kept in the history as sent (the reference only, never the story's key).
    const auto shown = a.history->list(chat);
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].storyReply);
    const auto mine = b.history->list(chat);
    REQUIRE(mine.size() == 2);
    CHECK(mine[1].storyReply->reaction);
}

TEST_CASE("part D: a friend added after posting gets access first, then the key", "[e2ev3][stories][partd][engine]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid(), frank = uuid();
    Computer a(state, alice), b(state, bob), f(state, frank);
    a.firstDevice('A');
    b.firstDevice('B');
    f.firstDevice('F');
    friendsWithChat(*state, alice, bob);
    upkeep({&a, &b, &f});
    const std::string storyId = createStory(*state, alice, {bob});
    const auto key = sc::newStoryKey();
    e2::sendStoryKey(*a.device, *a.stories, storyId, key, state->stories[storyId].expiresAt, {bob}, a.fanOut());
    friendsWithChat(*state, alice, frank);

    // The key-gap list names frank; access is added, then the key goes.
    std::vector<std::string> order;
    const int handed = e2::fillStoryGaps(
        *a.device, *a.stories, {{storyId, {frank}}},
        [&](const std::string& id, const std::vector<std::string>& users) {
            order.push_back("access");
            for (const auto& u : users) state->stories[id].access.insert(u);
        },
        a.fanOut());
    CHECK(handed == 1);
    CHECK(order == std::vector<std::string>{"access"});
    f.receive();
    CHECK(opens(f, storyId, alice, key));

    // A gap for a story this device holds no key of is left alone (another of the author's devices posted it, say).
    CHECK(e2::fillStoryGaps(*a.device, *a.stories, {{uuid(), {frank}}}, [](const std::string&, const std::vector<std::string>&) { FAIL(); },
                            a.fanOut()) == 0);
}

TEST_CASE("part D: one viewer the mailbox refuses doesn't cost the others the key", "[e2ev3][stories][partd][engine]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid(), carol = uuid(), dave = uuid();
    Computer a(state, alice), b(state, bob), c(state, carol), d(state, dave);
    a.firstDevice('A');
    b.firstDevice('B');
    c.firstDevice('C');
    d.firstDevice('D');
    for (const auto& other : {bob, carol, dave}) friendsWithChat(*state, alice, other);
    upkeep({&a, &b, &c, &d});
    // Dave was listed when the delivery was made but has no access by the time it goes (unfriended, hidden, revoked):
    // his list is still readable through the 1:1 chat, and the mailbox refuses any batch under the story id that names him.
    const std::string storyId = createStory(*state, alice, {bob, carol});
    const auto chatWith = [state, alice](const std::string& other) {
        for (const auto& [id, members] : state->conversations) {
            if (state->bothIn(id, alice, other)) return id;
        }
        return std::string();
    };
    e2::StoryFanOut viaChats;
    viaChats.listVia = [&](const std::string& story, const std::string& userId) { return userId == alice ? story : chatWith(userId); };
    const auto key = sc::newStoryKey();
    const auto sent = e2::sendStoryKey(*a.device, *a.stories, storyId, key, state->stories[storyId].expiresAt, {bob, dave, carol}, viaChats);
    CHECK_FALSE(sent.waiting);
    CHECK(sent.delivered == 2);  // bob and carol; dave is dropped, not retried
    CHECK_FALSE(a.stories->hasDeliveries());
    b.receive();
    c.receive();
    d.receive();
    CHECK(opens(b, storyId, alice, key));
    CHECK(opens(c, storyId, alice, key));
    CHECK(d.stories->keys().empty());
    CHECK(state->mailboxes[d.device->deviceId()].empty());

    // A story nobody may get any more (deleted) is given up after a bounded number of tries, never one request per viewer.
    const std::string gone = createStory(*state, alice, {bob, carol});
    state->stories[gone].deleted = true;
    const auto before = state->requests.size();
    const auto none = e2::sendStoryKey(*a.device, *a.stories, gone, key, state->stories[gone].expiresAt, {bob, carol, dave}, viaChats);
    CHECK(none.delivered == 0);
    CHECK_FALSE(a.stories->hasDeliveries());
    CHECK(state->requests.size() - before <= 8);
}

TEST_CASE("part D: K is kept past the story's end time and forgotten once the story is gone", "[e2ev3][stories][partd][engine]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid();
    Computer a(state, alice), b(state, bob);
    a.firstDevice('A');
    b.firstDevice('B');
    friendsWithChat(*state, alice, bob);
    upkeep({&a, &b});
    const std::string storyId = createStory(*state, alice, {bob}, 60 * 1000);
    const auto key = sc::newStoryKey();
    const std::int64_t expiresAt = state->stories[storyId].expiresAt;
    e2::sendStoryKey(*a.device, *a.stories, storyId, key, expiresAt, {bob}, a.fanOut());
    b.receive();
    REQUIRE(b.stories->key(storyId, alice));
    // Past its end time (a highlight or the archive may still show it): receive passes keep it.
    state->now = expiresAt + 1;
    b.receive();
    CHECK(opens(b, storyId, alice, key));
    CHECK(a.stories->key(storyId, alice));
    // The app finds the server lists it nowhere any more (deleted, revoked): forgotten, with what waited for it.
    b.stories->forget(storyId);
    CHECK_FALSE(b.stories->key(storyId, alice));
    CHECK(b.stories->keys().empty());
    // A late key for a highlighted story is kept (the author may still send it after the end time).
    const std::string late = createStory(*state, alice, {bob}, 60 * 1000);
    state->stories[late].highlighted = true;
    state->now = state->stories[late].expiresAt + 5;
    e2::sendStoryKey(*a.device, *a.stories, late, key, state->now + 60 * 1000, {bob}, a.fanOut());
    b.receive();
    CHECK(opens(b, late, alice, key));
}

TEST_CASE("part D: story keys wait for Tor, in order, and are never encrypted twice", "[e2ev3][stories][partd][engine][tor]") {
    auto state = std::make_shared<State>();
    const std::string alice = uuid(), bob = uuid(), carol = uuid();
    Computer a(state, alice, true), b(state, bob), c(state, carol);
    a.firstDevice('A');
    b.firstDevice('B');
    c.firstDevice('C');
    friendsWithChat(*state, alice, bob);
    friendsWithChat(*state, alice, carol);
    upkeep({&a, &b, &c});

    // Tor down before anything is encrypted: the delivery waits in the (sealed) store.
    const std::string first = createStory(*state, alice, {bob});
    const auto k1 = sc::newStoryKey();
    state->torDown.insert("*");
    const auto waited = e2::sendStoryKey(*a.device, *a.stories, first, k1, state->stories[first].expiresAt, {bob}, a.fanOut());
    CHECK(waited.waiting);
    CHECK(a.stories->hasDeliveries());
    CHECK(state->mailboxes[b.device->deviceId()].empty());

    // Tor down only at the send: the engine's ciphertext waits in the send queue, exactly as made.
    state->torDown = {"sendMessages"};
    const std::string second = createStory(*state, alice, {carol});
    const auto k2 = sc::newStoryKey();
    // The earlier one goes first (it can be encrypted now), then this one; both requests are queued.
    const auto queued = e2::sendStoryKey(*a.device, *a.stories, second, k2, state->stories[second].expiresAt, {carol}, a.fanOut());
    CHECK_FALSE(queued.waiting);
    CHECK_FALSE(a.stories->hasDeliveries());
    REQUIRE(a.queueing->queue());
    CHECK_FALSE(a.queueing->queue()->empty());
    const auto requestsBefore = state->requests.size();
    CHECK(state->mailboxes[c.device->deviceId()].empty());

    // Tor back: the queued requests go as they are, in order.
    state->torDown.clear();
    const auto flushed = e2::sendQueuedChat(*a.device, *a.history, *a.queueing);
    CHECK_FALSE(flushed.waiting);
    CHECK(a.queueing->queue()->empty());
    CHECK(state->requests.size() > requestsBefore);
    CHECK(state->requests[requestsBefore].first == first);
    b.receive();
    c.receive();
    CHECK(opens(b, first, alice, k1));
    CHECK(opens(c, second, alice, k2));
}

#endif
