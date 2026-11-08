// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <filesystem>
#include <map>
#include <set>
#include <thread>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/device_store.hpp"
#include "redvault/e2ev3/key_vault.hpp"
#include "redvault/e2ev3/pairing.hpp"
#include "redvault/e2ev3/send_queue.hpp"
#include "redvault/util/text.hpp"

// Protocol 3 on the desktop: key storage, the device logic and pairing, against
// the real engine (crypto-core through its C ABI) and a small in-memory stand-in
// for the server's routes. The real server is exercised by
// desktop/tools/v3_server_run (a dev database), not here.

namespace e2 = redvault::e2ev3;
using e2::Json;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("rv-e2ev3-" + redvault::util::randomUuid());
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

/// The routes the devices use, kept in memory. Shared between the "clients"; each client is a Session for one account.
struct FakeState {
    struct Device {
        Json identity;
        Json oneTimeKeys = Json::array();
        Json fallbackKey;
    };
    std::mutex mutex;
    std::map<std::string, Device> devices;
    std::map<std::string, Json> lists;
    std::map<std::string, std::vector<Json>> mailboxes;
    struct Enrollment {
        std::string id, userId, newDeviceId;
        std::vector<Json> messages;
    };
    std::map<std::string, Enrollment> enrollments;
    std::map<std::string, Json> backups;
    std::map<std::string, std::vector<std::string>> groups;  // conversation -> member user ids
    // rv-signal-v1 directory: device id -> the device's stored upload, and its unused one-time keys.
    struct SignalEntry {
        Json certificate, registrationId, identityKey, signedPreKey, lastResort;
        std::vector<Json> preKeys, kyberPreKeys;
    };
    std::map<std::string, SignalEntry> signal;
    bool signalOn = true;
    bool withholdSignal = false;  // a server that hides Signal keys
    std::map<std::string, std::vector<Json>> rosters;  // M6: conversation -> signed rosters, oldest first
    int counter = 0;
    // Tor down (docs/tor-by-default.md): these calls are refused at the gate, as the app's transport does ("*": all).
    std::set<std::string> torDown;
    // Every message request that reached the server: (kind, conversation, bodies), oldest first.
    std::vector<std::pair<std::string, std::vector<std::string>>> posted;
};

class FakeServer final : public e2::V3Server {
public:
    FakeServer(std::shared_ptr<FakeState> state, std::string userId) : s_(std::move(state)), user_(std::move(userId)) {}

    Json config() override { return Json{{"enabled", true}, {"resetDelayHours", 72}, {"signal", s_->signalOn}}; }

    /// Refuses the call the way the Tor-only transport does when the gateway isn't connected: nothing reaches the server.
    void gate(const char* call) const {
        if (s_->torDown.count("*") || s_->torDown.count(call)) throw redvault::api::ApiError(0, "tor_unavailable", "Tor isn't connected");
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
    Json rosters(const std::string& conversationId, int afterEpoch) override {
        std::lock_guard lock(s_->mutex);
        Json out = Json::array();
        for (const auto& r : s_->rosters[conversationId]) {
            if (r.at("epoch").get<int>() > afterEpoch) out.push_back(r);
        }
        return out;
    }
    Json postRoster(const std::string& conversationId, const Json& roster) override {
        std::lock_guard lock(s_->mutex);
        auto& chain = s_->rosters[conversationId];
        // The real server checks signatures, members and `prev` too (server tests); here: only the next epoch.
        if (roster.at("epoch").get<int>() != static_cast<int>(chain.size()) + 1) throw redvault::api::ApiError(409, "stale_roster", "stale");
        chain.push_back(roster);
        return Json{{"epoch", roster.at("epoch")}};
    }
    Json signalStatus(const std::string& deviceId) override {
        std::lock_guard lock(s_->mutex);
        if (!s_->signal.count(deviceId)) return Json{{"enabled", false}, {"preKeys", 0}, {"kyberPreKeys", 0}};
        const auto& e = s_->signal[deviceId];
        return Json{{"enabled", true}, {"preKeys", e.preKeys.size()}, {"kyberPreKeys", e.kyberPreKeys.size()}};
    }
    Json signalClaim(const std::string&, const Json& devices) override {
        std::lock_guard lock(s_->mutex);
        Json out = Json::array();
        for (const auto& want : devices) {
            const std::string id = want.at("deviceId").get<std::string>();
            Json item = {{"userId", want.at("userId")}, {"deviceId", id}, {"certificate", nullptr}, {"bundle", nullptr}};
            if (s_->signal.count(id) && !s_->withholdSignal) {
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
    Json registerDevice(const Json& upload) override {
        std::lock_guard lock(s_->mutex);
        auto& d = s_->devices[upload.at("identity").at("deviceId").get<std::string>()];
        d.identity = upload.at("identity");
        for (const auto& k : upload.at("oneTimeKeys")) d.oneTimeKeys.push_back(k);
        d.fallbackKey = upload.value("fallbackKey", Json(nullptr));
        return Json{{"oneTimeKeyCount", d.oneTimeKeys.size()}};
    }
    Json listDevices() override {
        std::lock_guard lock(s_->mutex);
        Json out = Json::array();
        const Json list = s_->lists.count(user_) ? s_->lists[user_] : Json(nullptr);
        for (const auto& [id, d] : s_->devices) {
            if (d.identity.at("userId") != user_) continue;
            bool listed = false;
            if (!list.is_null()) {
                for (const auto& l : list.at("devices")) listed = listed || l.at("deviceId") == id;
            }
            out.push_back({{"deviceId", id}, {"listed", listed}});
        }
        return out;
    }
    void removeDevice(const std::string& deviceId) override {
        std::lock_guard lock(s_->mutex);
        s_->devices.erase(deviceId);
    }
    Json getDeviceList(const std::string& userId, const std::string&) override {
        gate("getDeviceList");
        std::lock_guard lock(s_->mutex);
        return Json{{"deviceList", s_->lists.count(userId) ? s_->lists[userId] : Json(nullptr)}, {"pendingReset", nullptr}};
    }
    Json putDeviceList(const Json& list, const Json&) override {
        std::lock_guard lock(s_->mutex);
        const std::string userId = list.at("userId");
        const int current = s_->lists.count(userId) ? s_->lists[userId].at("version").get<int>() : 0;
        if (list.at("version").get<int>() != current + 1) throw redvault::api::ApiError(409, "stale_device_list", "stale");
        s_->lists[userId] = list;
        return Json{{"status", "accepted"}};
    }
    void cancelReset(const std::string&) override {}
    Json claimKeys(const std::string&, const Json& devices) override {
        gate("claimKeys");
        std::lock_guard lock(s_->mutex);
        Json out = Json::array();
        for (const auto& want : devices) {
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
    void sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages) override {
        gate("sendMessages");
        std::lock_guard lock(s_->mutex);
        std::vector<std::string> bodies;
        for (const auto& m : messages) bodies.push_back(m.at("body").get<std::string>());
        s_->posted.emplace_back("messages", bodies);
        for (const auto& m : messages) {
            s_->mailboxes[m.at("deviceId")].push_back({{"id", "m" + std::to_string(++s_->counter)},
                                                       {"senderUserId", user_},
                                                       {"senderDeviceId", senderDeviceId},
                                                       {"conversationId", conversationId.empty() ? Json(nullptr) : Json(conversationId)},
                                                       {"body", m.at("body")}});
        }
    }
    Json conversationDeviceLists(const std::string& conversationId) override {
        gate("conversationDeviceLists");
        std::lock_guard lock(s_->mutex);
        Json members = Json::array();
        for (const auto& userId : s_->groups.at(conversationId)) {
            members.push_back({{"userId", userId}, {"deviceList", s_->lists.count(userId) ? s_->lists[userId] : Json(nullptr)}, {"pendingReset", nullptr}});
        }
        return Json{{"members", members}};
    }
    void sendGroupMessage(const std::string& senderDeviceId, const std::string& conversationId, const std::string& body,
                          const Json& recipients) override {
        gate("sendGroupMessage");
        std::lock_guard lock(s_->mutex);
        s_->posted.emplace_back("group", std::vector<std::string>{body});
        for (const auto& r : recipients) {
            s_->mailboxes[r.at("deviceId")].push_back({{"id", "m" + std::to_string(++s_->counter)},
                                                       {"senderUserId", user_},
                                                       {"senderDeviceId", senderDeviceId},
                                                       {"conversationId", conversationId},
                                                       {"body", body}});
        }
    }
    Json inbox(const std::string& deviceId) override {
        gate("inbox");
        std::lock_guard lock(s_->mutex);
        return Json(s_->mailboxes[deviceId]);
    }
    void ack(const std::string& deviceId, const Json& ids) override {
        std::lock_guard lock(s_->mutex);
        auto& box = s_->mailboxes[deviceId];
        std::erase_if(box, [&](const Json& item) {
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
    void putRecoveryBackup(const std::string& backup, const std::string& signature) override {
        std::lock_guard lock(s_->mutex);
        s_->backups[user_] = {{"backup", backup}, {"signature", signature}};
    }
    Json getRecoveryBackup() override {
        std::lock_guard lock(s_->mutex);
        return s_->backups.at(user_);
    }
    Json events(int) override { return Json::array(); }

private:
    std::shared_ptr<FakeState> s_;
    std::string user_;
};

/// One computer: its own store folder and file-based key vault (DPAPI and the keyring have their own tests).
struct Computer {
    TempDir dir;
    e2::Backend backend;
    Computer(const std::shared_ptr<FakeState>& state, const std::string& userId) {
        backend.store = std::make_shared<e2::DeviceStore>(dir.path, e2::makeFileVault(dir.path / "keys"));
        backend.server = std::make_shared<FakeServer>(state, userId);
    }
};

e2::WaitOptions quick() {
    e2::WaitOptions wait;
    wait.intervalMs = 5;
    wait.timeoutMs = 20000;
    return wait;
}

std::string bytesFor(char fill) {
    return std::string(32, fill);
}

}  // namespace

TEST_CASE("the engine answers through the C ABI", "[e2ev3]") {
    const Json answer = e2::runEngine(Json{{"op", "version"}});
    REQUIRE(answer.value("ok", false));
    const Json bad = e2::runEngine(Json{{"op", "no-such-op"}});
    REQUIRE_FALSE(bad.value("ok", true));
    REQUIRE(bad.contains("error"));
}

TEST_CASE("a file vault keeps a key and forgets it", "[e2ev3][vault]") {
    TempDir dir;
    auto vault = e2::makeFileVault(dir.path);
    const auto secret = redvault::crypto::randomBytes(32);
    vault->put("alice/dev1", secret);
    const auto back = vault->get("alice/dev1");
    REQUIRE(back.has_value());
    REQUIRE(redvault::crypto::constantTimeEquals(back->view(), secret));
    REQUIRE_FALSE(vault->get("alice/dev2").has_value());
    vault->erase("alice/dev1");
    REQUIRE_FALSE(vault->get("alice/dev1").has_value());
    REQUIRE(e2::isWeakProtection(vault->protection()));
}

#ifdef _WIN32
TEST_CASE("DPAPI protects a key for this Windows user, bound to its id", "[e2ev3][vault]") {
    TempDir dir;
    auto vault = e2::makeDpapiVault(dir.path);
    REQUIRE(vault);
    const auto secret = redvault::crypto::randomBytes(32);
    vault->put("alice/dev1", secret);

    // The file on disk isn't the key.
    std::size_t files = 0;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        ++files;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        REQUIRE(blob.find(std::string(secret.begin(), secret.end())) == std::string::npos);
    }
    REQUIRE(files == 1);

    const auto back = vault->get("alice/dev1");
    REQUIRE(back.has_value());
    REQUIRE(redvault::crypto::constantTimeEquals(back->view(), secret));

    // The same blob copied under another device's name doesn't open (the id is the DPAPI entropy).
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        auto copy = entry.path();
        copy.replace_filename(std::string(40, 'a') + ".dpapi");
        fs::copy_file(entry.path(), copy);
    }
    REQUIRE_FALSE(vault->get("alice/other").has_value());
    vault->erase("alice/dev1");
    REQUIRE_FALSE(vault->get("alice/dev1").has_value());
}
#endif

TEST_CASE("the device store keeps state under a key only the vault holds", "[e2ev3][store]") {
    TempDir dir;
    e2::DeviceStore store(dir.path / "devices", e2::makeFileVault(dir.path / "keys"));
    REQUIRE_FALSE(store.load("alice").has_value());
    const auto created = store.create("alice");
    REQUIRE(created.storageKey.size() > 40);
    REQUIRE_FALSE(created.state.has_value());
    // Without a state the device isn't usable yet, but its key survives a restart.
    store.saveState("alice", created.deviceId, "STATE-1");
    const auto loaded = store.load("alice");
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->deviceId == created.deviceId);
    REQUIRE(loaded->storageKey == created.storageKey);
    REQUIRE(loaded->state == "STATE-1");
    REQUIRE_THROWS(store.saveState("alice", "someone-else", "x"));

    // Losing the vault entry loses the device, quietly.
    e2::DeviceStore otherVault(dir.path / "devices", e2::makeFileVault(dir.path / "elsewhere"));
    REQUIRE_FALSE(otherVault.load("alice").has_value());

    store.wipe("alice");
    REQUIRE_FALSE(store.load("alice").has_value());
}

TEST_CASE("first device, pairing by comparing codes, revoking, and the recovery key", "[e2ev3][pairing]") {
    auto state = std::make_shared<FakeState>();
    Computer first(state, "alice");
    Computer second(state, "alice");

    auto one = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), first.backend);
    REQUIRE(one);
    REQUIRE(one->must("deviceList").value("trusted", false));
    // The state survives a restart of the app.
    auto reopened = e2::V3Device::open("alice", first.backend);
    REQUIRE(reopened);
    REQUIRE(reopened->deviceId() == one->deviceId());

    // A new computer asks to pair; the trusted one answers.
    auto two = e2::startNewDevice("alice", second.backend);
    auto joining = e2::NewDevicePairing::begin(*two);
    std::vector<int> theirCodes;
    std::unique_ptr<e2::TrustedPairing> answering;
    std::thread trustedSide([&] {
        auto started = e2::TrustedPairing::begin(*one, joining->enrollmentId(), quick());
        theirCodes = started.codes;
        answering = std::move(started.pairing);
    });
    const auto myCodes = joining->codes(quick());
    trustedSide.join();
    REQUIRE(myCodes == theirCodes);
    REQUIRE(myCodes.size() >= 3);

    // Both people say the numbers match.
    std::thread approve([&] { answering->confirm(quick()); });
    const Json list = joining->confirm(quick());
    approve.join();
    REQUIRE(list.at("devices").size() == 2);
    REQUIRE(two->must("deviceList").value("trusted", false));

    // Revoking the second device from the first.
    e2::revokeDevice(*one, two->deviceId());
    REQUIRE(state->lists["alice"].at("devices").size() == 1);
    REQUIRE(state->lists["alice"].at("version").get<int>() == 3);

    // The recovery key restores a device with no other device left.
    const std::string recovery = e2::createRecoveryBackup(*one);
    REQUIRE(recovery.size() == 64 + 15);
    REQUIRE(e2::parseRecoveryKey(recovery).has_value());
    REQUIRE_FALSE(e2::parseRecoveryKey("not a key").has_value());
    Computer third(state, "alice");
    REQUIRE_THROWS_AS(e2::restoreWithRecoveryKey("alice", "00-11", third.backend), e2::EngineError);
    auto restored = e2::restoreWithRecoveryKey("alice", recovery, third.backend);
    REQUIRE(restored);
    REQUIRE(state->lists["alice"].at("devices").size() == 2);
    REQUIRE(restored->must("deviceList").value("trusted", false));
}

TEST_CASE("a wrong comparison approves nothing", "[e2ev3][pairing]") {
    auto state = std::make_shared<FakeState>();
    Computer first(state, "alice");
    Computer second(state, "alice");
    auto one = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), first.backend);
    auto two = e2::startNewDevice("alice", second.backend);
    auto joining = e2::NewDevicePairing::begin(*two);
    std::unique_ptr<e2::TrustedPairing> answering;
    std::thread trustedSide([&] { answering = std::move(e2::TrustedPairing::begin(*one, joining->enrollmentId(), quick()).pairing); });
    joining->codes(quick());
    trustedSide.join();
    // The person on the trusted device says "they don't match".
    answering->reject();
    joining->cancel();
    REQUIRE(state->lists["alice"].at("devices").size() == 1);
    REQUIRE(state->enrollments.empty());
}

TEST_CASE("a pairing wait can be cancelled", "[e2ev3][pairing]") {
    auto state = std::make_shared<FakeState>();
    Computer first(state, "alice");
    Computer second(state, "alice");
    auto one = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), first.backend);
    auto two = e2::startNewDevice("alice", second.backend);
    auto joining = e2::NewDevicePairing::begin(*two);
    std::atomic<bool> stop{false};
    auto wait = quick();
    wait.cancelled = [&] { return stop.load(); };
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop = true;
    });
    try {
        joining->codes(wait);
        FAIL("should have been cancelled");
    } catch (const e2::EngineError& error) {
        REQUIRE(error.code() == "cancelled");
    }
    canceller.join();
}

TEST_CASE("one-to-one messages reach the contact and the sender's other devices", "[e2ev3][messages]") {
    auto state = std::make_shared<FakeState>();
    Computer aliceOne(state, "alice");
    Computer aliceTwo(state, "alice");
    Computer bobOne(state, "bob");

    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), aliceOne.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bobOne.backend);

    auto a2 = e2::startNewDevice("alice", aliceTwo.backend);
    auto joining = e2::NewDevicePairing::begin(*a2);
    std::unique_ptr<e2::TrustedPairing> answering;
    std::thread trustedSide([&] { answering = std::move(e2::TrustedPairing::begin(*a1, joining->enrollmentId(), quick()).pairing); });
    joining->codes(quick());
    trustedSide.join();
    std::thread approve([&] { answering->confirm(quick()); });
    joining->confirm(quick());
    approve.join();

    REQUIRE(e2::sendText(*a1, "chat-1", "bob", "hello bob") == 2);  // bob's device and alice's second device

    const auto atBob = e2::receive(*b1);
    REQUIRE(atBob.size() == 1);
    REQUIRE(atBob[0].error.empty());
    REQUIRE(atBob[0].text == "hello bob");
    REQUIRE(atBob[0].senderUserId == "alice");

    const auto atAlice = e2::receive(*a2);
    REQUIRE(atAlice.size() == 1);
    REQUIRE(atAlice[0].text == "hello bob");

    // The reply goes to both of alice's devices.
    REQUIRE(e2::sendText(*b1, "chat-1", "alice", "hi alice") == 2);
    REQUIRE(e2::receive(*a1).at(0).text == "hi alice");
    REQUIRE(e2::receive(*a2).at(0).text == "hi alice");

    // Mailboxes are emptied by acknowledgement; nothing is replayed.
    REQUIRE(e2::receive(*a1).empty());

    // Both accounts see the same safety number for each other.
    const auto fromAlice = e2::deviceKeySafetyNumber(*a1, "bob", "chat-1");
    const auto fromBob = e2::deviceKeySafetyNumber(*b1, "alice", "chat-1");
    REQUIRE(fromAlice.has_value());
    REQUIRE(fromAlice == fromBob);
    REQUIRE(fromAlice->size() == 12);
}

TEST_CASE("safety numbers: the QR code, verifying by hand, and accepting changed keys", "[e2ev3][safety]") {
    auto state = std::make_shared<FakeState>();
    Computer aliceOne(state, "alice"), bobOne(state, "bob");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), aliceOne.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bobOne.backend);

    const auto atAlice = e2::deviceKeySafety(*a1, "bob");
    const auto atBob = e2::deviceKeySafety(*b1, "alice");
    REQUIRE(atAlice.has_value());
    REQUIRE(atBob.has_value());
    REQUIRE(atAlice->groups == atBob->groups);
    REQUIRE(atAlice->groups.size() == 12);
    REQUIRE_FALSE(atAlice->verified);
    REQUIRE_FALSE(atAlice->keyChanged);
    REQUIRE(atBob->qrText.rfind("RVDK1:", 0) == 0);

    // Alice's phone scans the code on Bob's screen; her own code (the wrong way round) doesn't count.
    REQUIRE_FALSE(e2::verifyContactQr(*a1, "bob", atAlice->accountKey, atAlice->qrText));
    REQUIRE(e2::verifyContactQr(*a1, "bob", atAlice->accountKey, atBob->qrText));
    REQUIRE(e2::deviceKeySafety(*a1, "bob")->verified);

    // Bob compares the digits instead, and can take it back.
    e2::setContactVerified(*b1, "alice", atBob->accountKey, true);
    REQUIRE(e2::deviceKeySafety(*b1, "alice")->verified);
    e2::setContactVerified(*b1, "alice", atBob->accountKey, false);
    REQUIRE_FALSE(e2::deviceKeySafety(*b1, "alice")->verified);
    e2::setContactVerified(*b1, "alice", atBob->accountKey, true);

    // Alice starts over on a new computer: a new account key (made elsewhere, then her list on the
    // server is replaced, as an account reset does). Bob sees a changed number, not verified.
    auto elsewhere = std::make_shared<FakeState>();
    Computer aliceNew(elsewhere, "alice");
    auto a3 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('C')), aliceNew.backend);
    state->lists["alice"] = elsewhere->lists["alice"];
    elsewhere->lists["bob"] = state->lists["bob"];
    const auto changed = e2::deviceKeySafety(*b1, "alice");
    REQUIRE(changed.has_value());
    REQUIRE(changed->keyChanged);
    REQUIRE_FALSE(changed->verified);
    REQUIRE(changed->accountKey != atBob->accountKey);
    REQUIRE(changed->groups != atBob->groups);
    // Marking the old key verified again can't stand in for the new one.
    REQUIRE_THROWS(e2::setContactVerified(*b1, "alice", changed->accountKey, true));

    // Bob compares the new number with Alice and accepts the new keys.
    e2::acceptContactKey(*b1, "alice", changed->accountKey, true);
    const auto accepted = e2::deviceKeySafety(*b1, "alice");
    REQUIRE_FALSE(accepted->keyChanged);
    REQUIRE(accepted->verified);
    REQUIRE(accepted->groups == changed->groups);
    REQUIRE(e2::deviceKeySafety(*a3, "bob")->groups == accepted->groups);
}

namespace {

e2::ChatHistory historyFor(e2::V3Device& device, const fs::path& dir) {
    return e2::ChatHistory(dir / ("history-" + device.deviceId()), device.deriveKey("redvault-v3-chat-history"));
}

}  // namespace

TEST_CASE("chat envelopes and the encrypted local history", "[e2ev3][chat]") {
    e2::Envelope e;
    e.id = "0b6f2c1e-0000-4000-8000-000000000001";
    e.c = "chat-1";
    e.ts = 1767225600000;
    e.body = "h\xC3\xA9llo \xF0\x9F\x91\x8B";
    const auto back = e2::decodeEnvelope(e2::encodeEnvelope(e), "chat-1", e.ts);
    REQUIRE(back.kind == e2::DecodedEnvelope::Kind::Message);
    REQUIRE(back.text == e.body);
    REQUIRE(back.ts == e.ts);
    REQUIRE(e2::decodeEnvelope("not json", "chat-1", e.ts).kind == e2::DecodedEnvelope::Kind::Ignore);

    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    e2::ChatRecord r{"chat-1", "id-1", "alice", "dev", 5, 6, "secret words", "in", "received"};
    {
        e2::ChatHistory h(dir.path / "h", key);
        REQUIRE(h.add(r));
        REQUIRE_FALSE(h.add(r));  // a retry or own-device copy
        h.pin("chat-1");
    }
    std::ifstream in(dir.path / "h", std::ios::binary);
    const std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(blob.find("secret words") == std::string::npos);
    e2::ChatHistory reopened(dir.path / "h", key);
    REQUIRE(reopened.list("chat-1").size() == 1);
    REQUIRE(reopened.pinned("chat-1"));
    e2::ChatHistory wrongKey(dir.path / "h", redvault::crypto::randomBytes(32));
    REQUIRE(wrongKey.list("chat-1").empty());
}

TEST_CASE("v3 chat: one-to-one with own-device copies, history and the pin", "[e2ev3][chat]") {
    auto state = std::make_shared<FakeState>();
    Computer a1c(state, "alice"), a2c(state, "alice"), b1c(state, "bob");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), a1c.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), b1c.backend);
    auto a2 = e2::startNewDevice("alice", a2c.backend);
    auto joining = e2::NewDevicePairing::begin(*a2);
    std::unique_ptr<e2::TrustedPairing> answering;
    std::thread t([&] { answering = std::move(e2::TrustedPairing::begin(*a1, joining->enrollmentId(), quick()).pairing); });
    joining->codes(quick());
    t.join();
    std::thread t2([&] { answering->confirm(quick()); });
    joining->confirm(quick());
    t2.join();

    auto ha1 = historyFor(*a1, a1c.dir.path);
    auto ha2 = historyFor(*a2, a2c.dir.path);
    auto hb1 = historyFor(*b1, b1c.dir.path);
    const std::string chat = "chat-1";
    REQUIRE_FALSE(ha1.pinned(chat));
    const auto sent = e2::sendChat(*a1, ha1, chat, "bob", "hello bob");
    REQUIRE(sent.delivered == 2);
    REQUIRE(ha1.pinned(chat));
    REQUIRE(ha1.list(chat).at(0).status == "sent");

    const auto atBob = e2::receiveChat(*b1, hb1);
    REQUIRE(atBob.size() == 1);
    REQUIRE(atBob[0].text == "hello bob");
    REQUIRE(atBob[0].direction == "in");
    REQUIRE(hb1.pinned(chat));
    const auto atA2 = e2::receiveChat(*a2, ha2);
    REQUIRE(atA2.size() == 1);
    REQUIRE(atA2[0].direction == "out");
    REQUIRE(atA2[0].id == sent.id);
    REQUIRE(e2::receiveChat(*b1, hb1).empty());

    // A server that moves a ciphertext into another chat gets it dropped: the envelope names its chat.
    e2::sendChat(*a1, ha1, chat, "bob", "second");
    {
        std::lock_guard lock(state->mutex);
        for (auto& item : state->mailboxes[b1->deviceId()]) item["conversationId"] = "chat-other";
    }
    REQUIRE(e2::receiveChat(*b1, hb1).empty());
    REQUIRE(state->mailboxes[b1->deviceId()].empty());  // acknowledged, shown nowhere
    REQUIRE(hb1.list("chat-other").empty());
}

TEST_CASE("v3 chat: a group with devices of three accounts", "[e2ev3][chat][group]") {
    auto state = std::make_shared<FakeState>();
    Computer ac(state, "alice"), bc(state, "bob"), cc(state, "carol");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    auto c = e2::setUpFirstDevice("carol", redvault::crypto::toBytes(bytesFor('C')), cc.backend);
    const std::string group = "group-1";
    state->groups[group] = {"alice", "bob", "carol"};
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    auto hc = historyFor(*c, cc.dir.path);

    const auto sent = e2::sendGroupChat(*a, ha, group, "hello group");
    REQUIRE(sent.delivered == 2);
    const auto atB = e2::receiveChat(*b, hb);
    REQUIRE(atB.size() == 1);
    REQUIRE(atB[0].text == "hello group");
    REQUIRE(atB[0].from == "alice");
    REQUIRE(e2::receiveChat(*c, hc).at(0).text == "hello group");

    // Bob replies: his own session, his own room keys.
    e2::sendGroupChat(*b, hb, group, "hi from bob");
    REQUIRE(e2::receiveChat(*a, ha).at(0).text == "hi from bob");
    REQUIRE(e2::receiveChat(*c, hc).at(0).text == "hi from bob");

    // Carol leaves: alice's next message uses a new session carol never gets.
    state->groups[group] = {"alice", "bob"};
    e2::sendGroupChat(*a, ha, group, "without carol");
    REQUIRE(e2::receiveChat(*b, hb).at(0).text == "without carol");
    REQUIRE(hc.list(group).size() == 2);  // carol's history has the two she was part of
}

TEST_CASE("v3 chat: no silent fallback when the contact has no device keys, and failed messages can be dropped", "[e2ev3][chat]") {
    auto state = std::make_shared<FakeState>();
    Computer ac(state, "alice");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto ha = historyFor(*a, ac.dir.path);
    try {
        e2::sendChat(*a, ha, "chat-9", "bob", "anyone there?");
        FAIL("should have refused");
    } catch (const e2::EngineError& error) {
        REQUIRE(error.code() == "contact_has_no_device_keys");
    }
    REQUIRE_FALSE(ha.pinned("chat-9"));
    REQUIRE(ha.list("chat-9").empty());  // refused before a message was made: nothing to retry
    e2::ChatRecord failed{"chat-9", "x1", "alice", "d", 1, 1, "text", "out", "failed"};
    ha.add(failed);
    REQUIRE(ha.remove("chat-9", "x1"));
    REQUIRE_FALSE(ha.remove("chat-9", "x1"));
}

namespace {

redvault::model::FileDescriptor fileForTest() {
    return redvault::model::FileDescriptor{
        .type = "file",
        .version = 1,
        .objectId = "33333333-3333-4333-8333-333333333333",
        .fileKey = "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=",
        .filename = "notes.txt",
        .mimeType = "text/plain",
        .originalSize = 5,
        .encryptedSize = 46,
        .ciphertextSha256 = std::string(64, 'a'),
        .encryption = {.algorithm = "xchacha20poly1305-secretstream", .version = 1, .header = "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", .chunkSize = 65536},
    };
}

}  // namespace

TEST_CASE("v3 chat: reactions, edits and deletes, one-to-one, in everyone's history", "[e2ev3][chat][content]") {
    auto state = std::make_shared<FakeState>();
    Computer ac(state, "alice"), bc(state, "bob");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    const std::string chat = "chat-content";

    const std::string first = e2::sendChat(*a, ha, chat, "bob", "hello bob").id;
    REQUIRE(e2::receiveChat(*b, hb).size() == 1);

    // Bob reacts, Alice edits: each sees both.
    e2::sendChatOp(*b, hb, chat, "alice", {"reaction", first, {}, "\xF0\x9F\x91\x8D"});
    const auto atAlice = e2::receiveChat(*a, ha);
    REQUIRE(atAlice.size() == 1);
    REQUIRE(atAlice[0].op.has_value());  // a hidden record: what the caller counts as "new" has to tell
    e2::sendChatOp(*a, ha, chat, "bob", {"edit", first, "hello again", {}});
    REQUIRE(e2::receiveChat(*b, hb).size() == 1);
    for (auto* history : {&ha, &hb}) {
        const auto shown = history->list(chat);
        REQUIRE(shown.size() == 1);
        REQUIRE(shown[0].text == "hello again");
        REQUIRE(shown[0].edited);
        REQUIRE(shown[0].reactions.size() == 1);
        REQUIRE(shown[0].reactions[0].userId == "bob");
    }

    // Bob's forged edit and delete change nothing on either side.
    e2::sendChatOp(*b, hb, chat, "alice", {"edit", first, "forged", {}});
    e2::sendChatOp(*b, hb, chat, "alice", {"delete", first, {}, {}});
    e2::receiveChat(*a, ha);
    REQUIRE(ha.list(chat).at(0).text == "hello again");
    REQUIRE_FALSE(ha.list(chat).at(0).deleted);

    // Alice deletes for everyone.
    e2::sendChatOp(*a, ha, chat, "bob", {"delete", first, {}, {}});
    REQUIRE(ha.list(chat).at(0).deleted);
    e2::receiveChat(*b, hb);
    for (auto* history : {&ha, &hb}) {
        const auto shown = history->list(chat);
        REQUIRE(shown.size() == 1);
        REQUIRE(shown[0].deleted);
        REQUIRE(shown[0].text.empty());
        REQUIRE(shown[0].reactions.empty());
        for (const e2::ChatRecord& r : history->records(chat)) {
            REQUIRE(r.text.empty());  // not only hidden: gone
            if (r.op) REQUIRE(r.op->t == "delete");
        }
    }
}

TEST_CASE("v3 chat: a file travels in the envelope, and a delete takes its key away", "[e2ev3][chat][content]") {
    auto state = std::make_shared<FakeState>();
    Computer ac(state, "alice"), bc(state, "bob");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    const std::string chat = "chat-file";

    const auto sent = e2::sendChatFile(*a, ha, chat, "bob", fileForTest());
    {
        std::lock_guard lock(state->mutex);
        REQUIRE(state->mailboxes[b->deviceId()].at(0).dump().find(fileForTest().fileKey) == std::string::npos);
    }
    const auto atBob = e2::receiveChat(*b, hb);
    REQUIRE(atBob.size() == 1);
    REQUIRE(atBob[0].file == fileForTest());
    REQUIRE(atBob[0].text.empty());
    REQUIRE(ha.list(chat).at(0).file == fileForTest());

    e2::sendChatOp(*a, ha, chat, "bob", {"edit", sent.id, "words", {}});  // not valid on a file
    e2::receiveChat(*b, hb);
    REQUIRE_FALSE(hb.list(chat).at(0).edited);

    e2::sendChatOp(*a, ha, chat, "bob", {"delete", sent.id, {}, {}});
    e2::receiveChat(*b, hb);
    for (auto* history : {&ha, &hb}) {
        for (const e2::ChatRecord& r : history->records(chat)) REQUIRE_FALSE(r.file.has_value());
    }
}

TEST_CASE("v3 chat: reactions, deletes and files in a group", "[e2ev3][chat][content][group]") {
    auto state = std::make_shared<FakeState>();
    Computer ac(state, "alice"), bc(state, "bob"), cc(state, "carol");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    auto c = e2::setUpFirstDevice("carol", redvault::crypto::toBytes(bytesFor('C')), cc.backend);
    const std::string group = "group-content";
    state->groups[group] = {"alice", "bob", "carol"};
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    auto hc = historyFor(*c, cc.dir.path);

    const std::string text = e2::sendGroupChat(*a, ha, group, "hello group").id;
    const std::string file = e2::sendChatFile(*a, ha, group, {}, fileForTest()).id;
    REQUIRE(e2::receiveChat(*b, hb).size() == 2);
    e2::sendChatOp(*b, hb, group, {}, {"reaction", text, {}, "\xF0\x9F\x94\xA5"});
    REQUIRE(e2::receiveChat(*c, hc).size() == 3);
    REQUIRE(hc.list(group).at(0).reactions.size() == 1);
    REQUIRE(hc.list(group).at(1).file == fileForTest());

    e2::sendChatOp(*a, ha, group, {}, {"delete", file, {}, {}});
    e2::receiveChat(*b, hb);
    e2::receiveChat(*c, hc);
    REQUIRE(hc.list(group).at(1).deleted);
    REQUIRE_FALSE(hc.list(group).at(1).file.has_value());
    REQUIRE_FALSE(hc.list(group).at(0).deleted);
}

TEST_CASE("v3 chat: a failed op isn't kept, a failed file is", "[e2ev3][chat][content]") {
    auto state = std::make_shared<FakeState>();
    Computer ac(state, "alice");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto ha = historyFor(*a, ac.dir.path);
    const std::string group = "group-gone";  // the fake's route throws for a chat it doesn't know
    REQUIRE_THROWS(e2::sendChatOp(*a, ha, group, {}, {"reaction", "aaaaaaaa-0000-4000-8000-000000000001", {}, "x"}));
    REQUIRE(ha.records(group).empty());
    REQUIRE_THROWS(e2::sendChatFile(*a, ha, group, {}, fileForTest()));
    const auto records = ha.records(group);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].status == "failed");
    REQUIRE(records[0].file == fileForTest());
}

#ifdef REDVAULT_HAS_E2E_SIGNAL
// --- rv-signal-v1 on the desktop (development only; docs/signal-integration/) ---

namespace {

/// The mailbox bodies waiting for `deviceId`, as the server holds them.
std::vector<Json> mailboxBodies(const std::shared_ptr<FakeState>& state, const std::string& deviceId) {
    std::lock_guard lock(state->mutex);
    std::vector<Json> out;
    for (const auto& item : state->mailboxes[deviceId]) out.push_back(Json::parse(item.at("body").get<std::string>()));
    return out;
}

}  // namespace

TEST_CASE("signal: the engine in this build has libsignal", "[e2ev3][signal]") {
    REQUIRE(e2::engineHasSignal());
    const Json version = e2::runEngine(Json{{"op", "version"}});
    REQUIRE(version.at("signal").at("libsignal") == "v0.106.2");
}

TEST_CASE("signal: devices turn Signal on, declare it in the signed list and keep keys topped up", "[e2ev3][signal]") {
    auto state = std::make_shared<FakeState>();
    Computer c(state, "alice");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), c.backend);
    const Json config = c.backend.server->config();
    REQUIRE(e2::maintainSignal(*a1, config) == "enabled");
    REQUIRE(state->signal.count(a1->deviceId()) == 1);
    REQUIRE(state->signal[a1->deviceId()].preKeys.size() == 20);
    REQUIRE(state->lists["alice"].at("capabilities").at(0).at("deviceId") == a1->deviceId());
    REQUIRE(e2::maintainSignal(*a1, config) == "ok");  // nothing more to do
    // Keys run low: the next upkeep tops them up.
    state->signal[a1->deviceId()].preKeys.resize(3);
    REQUIRE(e2::maintainSignal(*a1, config) == "ok");
    REQUIRE(state->signal[a1->deviceId()].preKeys.size() == 23);
    // A server with Signal off: nothing happens.
    REQUIRE(e2::maintainSignal(*a1, Json{{"enabled", true}}) == "off");
}

TEST_CASE("signal: one-to-one chat with own-device copies goes over rv-signal-v1", "[e2ev3][signal][chat]") {
    auto state = std::make_shared<FakeState>();
    Computer a1c(state, "alice"), a2c(state, "alice"), b1c(state, "bob");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), a1c.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), b1c.backend);
    auto a2 = e2::startNewDevice("alice", a2c.backend);
    auto joining = e2::NewDevicePairing::begin(*a2);
    std::unique_ptr<e2::TrustedPairing> answering;
    std::thread t([&] { answering = std::move(e2::TrustedPairing::begin(*a1, joining->enrollmentId(), quick()).pairing); });
    joining->codes(quick());
    t.join();
    std::thread t2([&] { answering->confirm(quick()); });
    joining->confirm(quick());
    t2.join();
    const Json config = a1c.backend.server->config();
    for (auto* d : {a1.get(), a2.get(), b1.get()}) REQUIRE(e2::maintainSignal(*d, config) != "off");

    auto ha1 = historyFor(*a1, a1c.dir.path);
    auto ha2 = historyFor(*a2, a2c.dir.path);
    auto hb1 = historyFor(*b1, b1c.dir.path);
    const std::string chat = "chat-signal";
    const auto sent = e2::sendChat(*a1, ha1, chat, "bob", "hello over signal");
    REQUIRE(sent.delivered == 2);
    // What the server holds: Signal first messages, each with the sender's certificate. Never Olm.
    for (const std::string& id : {b1->deviceId(), a2->deviceId()}) {
        const auto bodies = mailboxBodies(state, id);
        REQUIRE(bodies.size() == 1);
        REQUIRE(bodies[0].value("protocol", std::string()) == "rv-signal-v1");
        REQUIRE(bodies[0].value("type", std::string()) == "prekey");
        REQUIRE(bodies[0].at("certificate").at("deviceId") == a1->deviceId());
        REQUIRE(bodies[0].dump().find("hello over signal") == std::string::npos);
    }
    const auto atBob = e2::receiveChat(*b1, hb1);
    REQUIRE(atBob.size() == 1);
    REQUIRE(atBob[0].text == "hello over signal");
    const auto atA2 = e2::receiveChat(*a2, ha2);
    REQUIRE(atA2.size() == 1);
    REQUIRE(atA2[0].direction == "out");

    // Bob answers: an established session, so a plain Signal message without a certificate.
    e2::sendChat(*b1, hb1, chat, "alice", "hi alice");
    const auto toA1 = mailboxBodies(state, a1->deviceId());
    REQUIRE(toA1.size() == 1);
    REQUIRE(toA1[0].value("protocol", std::string()) == "rv-signal-v1");
    const auto atA1 = e2::receiveChat(*a1, ha1);
    REQUIRE(atA1.size() == 1);
    REQUIRE(atA1[0].text == "hi alice");

    // Olm chat between these devices is refused by the engine itself.
    const Json olm = a1->op("encrypt", Json{{"recipient", b1->identity()}, {"plaintext", "downgrade?"}});
    REQUIRE(olm.value("error", std::string()) == "downgrade_refused");
}

TEST_CASE("signal: a contact without Signal keeps getting Olm; a hidden Signal device gets nothing", "[e2ev3][signal][chat]") {
    auto state = std::make_shared<FakeState>();
    Computer a1c(state, "alice"), b1c(state, "bob");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), a1c.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), b1c.backend);
    REQUIRE(e2::maintainSignal(*a1, a1c.backend.server->config()) == "enabled");
    auto ha1 = historyFor(*a1, a1c.dir.path);
    auto hb1 = historyFor(*b1, b1c.dir.path);

    // Bob has no Signal (like the web app): Olm, as before.
    e2::sendChat(*a1, ha1, "chat-mixed", "bob", "olm for bob");
    auto bodies = mailboxBodies(state, b1->deviceId());
    REQUIRE(bodies.size() == 1);
    REQUIRE_FALSE(bodies[0].contains("protocol"));
    REQUIRE(e2::receiveChat(*b1, hb1).at(0).text == "olm for bob");

    // Bob turns Signal on, but the server hides his keys from alice: no Olm copy is sent instead.
    REQUIRE(e2::maintainSignal(*b1, b1c.backend.server->config()) == "enabled");
    state->withholdSignal = true;
    const auto sent = e2::sendChat(*a1, ha1, "chat-mixed", "bob", "must not go over olm");
    REQUIRE(sent.delivered == 0);
    REQUIRE(mailboxBodies(state, b1->deviceId()).empty());
}
TEST_CASE("signal: the chat says which protocol it uses, and turning Signal off moves it back", "[e2ev3][signal][m5]") {
    auto state = std::make_shared<FakeState>();
    Computer a1c(state, "alice"), b1c(state, "bob");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), a1c.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), b1c.backend);
    const std::string chat = "chat-m5";
    REQUIRE(e2::chatProtocol(*a1, chat, "bob") == "olm");
    REQUIRE(e2::maintainSignal(*a1, a1c.backend.server->config()) == "enabled");
    REQUIRE(e2::chatProtocol(*a1, chat, "bob") == "olm");  // bob has no Signal yet
    REQUIRE(e2::maintainSignal(*b1, b1c.backend.server->config()) == "enabled");
    REQUIRE(e2::chatProtocol(*a1, chat, "bob") == "signal");

    auto ha1 = historyFor(*a1, a1c.dir.path);
    auto hb1 = historyFor(*b1, b1c.dir.path);
    REQUIRE(e2::sendChat(*a1, ha1, chat, "bob", "over signal").delivered == 1);
    REQUIRE(e2::receiveChat(*b1, hb1).at(0).text == "over signal");

    // Bob turns Signal off for his device (signed): alice's chat goes back to Olm, and still works.
    e2::turnOffSignal(*b1, b1->deviceId());
    REQUIRE_FALSE(state->lists["bob"].contains("capabilities"));
    REQUIRE(e2::chatProtocol(*a1, chat, "bob") == "olm");
    REQUIRE(e2::sendChat(*a1, ha1, chat, "bob", "back on olm").delivered == 1);
    REQUIRE_FALSE(mailboxBodies(state, b1->deviceId()).at(0).contains("protocol"));
    // Bob's own device stopped insisting on Signal: it opens the Olm message, and upkeep leaves Signal off.
    const auto atBob = e2::receiveChat(*b1, hb1);
    REQUIRE(atBob.size() == 1);
    REQUIRE(atBob[0].text == "back on olm");
    REQUIRE(e2::maintainSignal(*b1, b1c.backend.server->config()) == "off");
    REQUIRE_FALSE(state->lists["bob"].contains("capabilities"));
}

TEST_CASE("signal: a device that can't be reached is counted, not hidden", "[e2ev3][signal][m5]") {
    auto state = std::make_shared<FakeState>();
    Computer a1c(state, "alice"), b1c(state, "bob");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), a1c.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), b1c.backend);
    for (auto* d : {a1.get(), b1.get()}) e2::maintainSignal(*d, a1c.backend.server->config());
    auto ha1 = historyFor(*a1, a1c.dir.path);
    state->withholdSignal = true;
    const auto sent = e2::sendChat(*a1, ha1, "chat-m5b", "bob", "who gets this?");
    REQUIRE(sent.delivered == 0);
    REQUIRE(sent.unreachable == 1);
}


TEST_CASE("signal: a group goes over sender keys, and a removed member reads nothing new", "[e2ev3][signal][m6]") {
    auto state = std::make_shared<FakeState>();
    Computer a1c(state, "alice"), b1c(state, "bob"), c1c(state, "carol");
    auto a1 = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), a1c.backend);
    auto b1 = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), b1c.backend);
    auto c1 = e2::setUpFirstDevice("carol", redvault::crypto::toBytes(bytesFor('C')), c1c.backend);
    for (auto* d : {a1.get(), b1.get(), c1.get()}) REQUIRE(e2::maintainSignal(*d, a1c.backend.server->config()) == "enabled");
    const std::string group = "group-signal";
    state->groups[group] = {"alice", "bob", "carol"};
    auto ha = historyFor(*a1, a1c.dir.path);
    auto hb = historyFor(*b1, b1c.dir.path);
    auto hc = historyFor(*c1, c1c.dir.path);

    const auto sent = e2::sendGroupChat(*a1, ha, group, "hello group");
    REQUIRE(sent.delivered == 2);
    REQUIRE(state->rosters[group].size() == 1);  // epoch 1, signed by alice
    // bob holds a sender key inside a Signal message, then the group message itself: never Megolm.
    const auto atBobBodies = mailboxBodies(state, b1->deviceId());
    REQUIRE(atBobBodies.size() == 2);
    REQUIRE(atBobBodies[0].value("protocol", std::string()) == "rv-signal-v1");
    REQUIRE(atBobBodies[1].value("protocol", std::string()) == "rv-signal-group-v1");
    REQUIRE(atBobBodies[1].dump().find("hello group") == std::string::npos);
    REQUIRE(e2::receiveChat(*b1, hb).at(0).text == "hello group");
    REQUIRE(e2::receiveChat(*c1, hc).at(0).text == "hello group");

    // bob answers under his own sender key.
    e2::sendGroupChat(*b1, hb, group, "hi from bob");
    REQUIRE(e2::receiveChat(*a1, ha).at(0).text == "hi from bob");
    REQUIRE(e2::receiveChat(*c1, hc).at(0).text == "hi from bob");

    // carol leaves: alice's next send signs epoch 2 without her and uses a new sender key carol never gets.
    state->groups[group] = {"alice", "bob"};
    e2::sendGroupChat(*a1, ha, group, "after carol left");
    REQUIRE(state->rosters[group].size() == 2);
    REQUIRE(state->rosters[group][1].at("members").size() == 2);
    REQUIRE(e2::receiveChat(*b1, hb).at(0).text == "after carol left");
    REQUIRE(mailboxBodies(state, c1->deviceId()).empty());
}

#endif

// --- Tor by default: device-key sends wait for Tor, encrypted once (docs/tor-by-default.md, rule 5) ---

namespace {

/// One computer whose device talks through QueueingV3Server, with its queue in its own folder.
struct QueuedComputer {
    TempDir dir;
    e2::Backend backend;
    std::shared_ptr<e2::QueueingV3Server> server;
    QueuedComputer(const std::shared_ptr<FakeState>& state, const std::string& userId) {
        backend.store = std::make_shared<e2::DeviceStore>(dir.path, e2::makeFileVault(dir.path / "keys"));
        server = std::make_shared<e2::QueueingV3Server>(std::make_shared<FakeServer>(state, userId));
        backend.server = server;
    }
    fs::path queueFile(const e2::V3Device& device) const { return dir.path / ("sendq-" + device.deviceId()); }
    void attach(e2::V3Device& device) {
        server->attach(std::make_shared<e2::SendQueue>(queueFile(device), device.deriveKey("redvault-v3-send-queue")));
    }
};

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::vector<std::string> texts(const std::vector<e2::ChatRecord>& records) {
    std::vector<std::string> out;
    for (const auto& r : records) {
        if (!r.op) out.push_back(r.text);
    }
    return out;
}

}  // namespace

TEST_CASE("tor: a send refused at the Tor gate keeps its ciphertext, which goes out once, as it was", "[e2ev3][chat][tor]") {
    auto state = std::make_shared<FakeState>();
    QueuedComputer ac(state, "alice");
    Computer bc(state, "bob");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    ac.attach(*a);
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    const std::string chat = "chat-tor-1";

    // Tor drops between the key lookups and the post: the engine has encrypted (and saved its state), nothing left.
    state->torDown = {"sendMessages"};
    const auto sent = e2::sendChat(*a, ha, chat, "bob", "hello over tor");
    CHECK(sent.queued);
    REQUIRE(ha.list(chat).size() == 1);
    CHECK(ha.list(chat)[0].status == "queued");
    CHECK(state->posted.empty());
    CHECK(state->mailboxes[b->deviceId()].empty());
    const auto queued = ac.server->queue()->items();
    REQUIRE(queued.size() == 1);
    CHECK(queued[0].carriesRecord);
    CHECK(queued[0].recordId == sent.id);
    const std::string ciphertext = queued[0].messages.at(0).at("body").get<std::string>();

    // A restart finds the same queue; on disk it's sealed (no message text, no ciphertext in the clear).
    {
        e2::SendQueue reopened(ac.queueFile(*a), a->deriveKey("redvault-v3-send-queue"));
        REQUIRE(reopened.items().size() == 1);
        CHECK(reopened.items()[0].messages == queued[0].messages);
        e2::SendQueue wrongKey(ac.queueFile(*a), redvault::crypto::toBytes(bytesFor('X')));
        CHECK(wrongKey.items().empty());
    }
    const std::string onDisk = readFile(ac.queueFile(*a));
    CHECK(onDisk.find("hello over tor") == std::string::npos);
    CHECK(onDisk.find(ciphertext.substr(0, 24)) == std::string::npos);
    CHECK(onDisk.find(b->deviceId()) == std::string::npos);
    CHECK(readFile(ac.dir.path / ("history-" + a->deviceId())).find("hello over tor") == std::string::npos);

    // Still down: nothing goes, nothing is dropped.
    auto drained = e2::sendQueuedChat(*a, ha, *ac.server);
    CHECK(drained.waiting);
    CHECK(drained.sent == 0);
    CHECK(ac.server->queue()->items().size() == 1);

    // Tor is back: exactly the queued ciphertext, once, and the record turns sent.
    state->torDown.clear();
    drained = e2::sendQueuedChat(*a, ha, *ac.server);
    CHECK_FALSE(drained.waiting);
    CHECK(drained.sent == 1);
    CHECK(ac.server->queue()->empty());
    CHECK_FALSE(fs::exists(ac.queueFile(*a)));
    CHECK(ha.list(chat)[0].status == "sent");
    REQUIRE(state->posted.size() == 1);
    CHECK(state->posted[0].second.at(0) == ciphertext);
    const auto atBob = e2::receiveChat(*b, hb);
    REQUIRE(atBob.size() == 1);
    CHECK(atBob[0].text == "hello over tor");
    CHECK(atBob[0].id == sent.id);
    CHECK_FALSE(e2::hasQueuedChat(ha, *ac.server));
}

TEST_CASE("tor: sends made while Tor is down wait in order, and are encrypted only once it's back", "[e2ev3][chat][tor]") {
    auto state = std::make_shared<FakeState>();
    QueuedComputer ac(state, "alice");
    Computer bc(state, "bob");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    ac.attach(*a);
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    const std::string chat = "chat-tor-2";
    const std::string first = e2::sendChat(*a, ha, chat, "bob", "before").id;
    REQUIRE(e2::receiveChat(*b, hb).size() == 1);

    state->torDown = {"*"};
    e2::queueChat(*a, ha, chat, "bob", "one");
    e2::queueChat(*a, ha, chat, "bob", "two");
    e2::queueChatOp(*a, ha, chat, "bob", e2::ChatOp{"reaction", first, {}, "👍"});
    CHECK(ha.queued().size() == 3);
    CHECK(e2::hasQueuedChat(ha, *ac.server));
    CHECK_THROWS_AS(e2::queueChat(*a, ha, chat, "bob", ""), e2::EngineError);  // checked now, not when Tor is back
    const auto postsBefore = state->posted.size();

    auto drained = e2::sendQueuedChat(*a, ha, *ac.server);
    CHECK(drained.waiting);
    CHECK(state->posted.size() == postsBefore);
    CHECK(ha.queued().size() == 3);

    // Back: oldest first, each encrypted now.
    state->torDown.clear();
    drained = e2::sendQueuedChat(*a, ha, *ac.server);
    CHECK_FALSE(drained.waiting);
    CHECK(drained.sent == 3);
    CHECK(ha.queued().empty());
    const auto atBob = e2::receiveChat(*b, hb);
    REQUIRE(texts(atBob) == std::vector<std::string>{"one", "two"});
    const auto shown = hb.list(chat);
    REQUIRE(shown.size() == 3);
    REQUIRE(shown[0].reactions.size() == 1);
    CHECK(shown[0].reactions[0].emoji == "👍");
}

TEST_CASE("tor: a new send waits behind what's queued, so nothing overtakes", "[e2ev3][chat][tor]") {
    auto state = std::make_shared<FakeState>();
    QueuedComputer ac(state, "alice");
    Computer bc(state, "bob");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    ac.attach(*a);
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    const std::string chat = "chat-tor-3";

    state->torDown = {"sendMessages"};
    CHECK(e2::sendChat(*a, ha, chat, "bob", "first", 1000).queued);
    state->torDown.clear();
    // Tor is back, but the first still waits: the second's ciphertext queues behind it.
    CHECK(e2::sendChat(*a, ha, chat, "bob", "second", 2000).queued);
    CHECK(state->posted.empty());
    CHECK(ac.server->queue()->items().size() == 2);

    const auto drained = e2::sendQueuedChat(*a, ha, *ac.server);
    CHECK(drained.sent == 2);
    CHECK(texts(e2::receiveChat(*b, hb)) == std::vector<std::string>{"first", "second"});
}

TEST_CASE("tor: a group send refused at the gate keeps the room's ciphertext and sends it once", "[e2ev3][chat][group][tor]") {
    auto state = std::make_shared<FakeState>();
    QueuedComputer ac(state, "alice");
    Computer bc(state, "bob"), cc(state, "carol");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    auto c = e2::setUpFirstDevice("carol", redvault::crypto::toBytes(bytesFor('C')), cc.backend);
    ac.attach(*a);
    const std::string group = "group-tor";
    state->groups[group] = {"alice", "bob", "carol"};
    auto ha = historyFor(*a, ac.dir.path);
    auto hb = historyFor(*b, bc.dir.path);
    auto hc = historyFor(*c, cc.dir.path);

    state->torDown = {"sendGroupMessage"};
    CHECK(e2::sendGroupChat(*a, ha, group, "hello group over tor").queued);
    CHECK(ha.list(group).at(0).status == "queued");
    state->torDown.clear();
    const auto drained = e2::sendQueuedChat(*a, ha, *ac.server);
    CHECK(drained.sent == 1);
    CHECK(ha.list(group).at(0).status == "sent");
    CHECK(texts(e2::receiveChat(*b, hb)) == std::vector<std::string>{"hello group over tor"});
    CHECK(texts(e2::receiveChat(*c, hc)) == std::vector<std::string>{"hello group over tor"});
    std::size_t groupPosts = 0;
    for (const auto& [kind, bodies] : state->posted) groupPosts += kind == "group" ? 1 : 0;
    CHECK(groupPosts == 1);
}

TEST_CASE("tor: outside a chat send nothing is queued, and other failures still fail", "[e2ev3][tor]") {
    auto state = std::make_shared<FakeState>();
    QueuedComputer ac(state, "alice");
    Computer bc(state, "bob");
    auto a = e2::setUpFirstDevice("alice", redvault::crypto::toBytes(bytesFor('A')), ac.backend);
    auto b = e2::setUpFirstDevice("bob", redvault::crypto::toBytes(bytesFor('B')), bc.backend);
    ac.attach(*a);
    state->torDown = {"sendMessages"};
    // Not inside a delivery: the refusal reaches the caller as it is.
    CHECK_THROWS_AS(ac.server->sendMessages(a->deviceId(), "chat-x", Json::array()), redvault::api::ApiError);
    CHECK(ac.server->queue()->empty());
    state->torDown.clear();
    (void)b;
}
