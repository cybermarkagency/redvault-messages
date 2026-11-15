// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/community.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/e2ev3/device_store.hpp"
#include "redvault/e2ev3/key_vault.hpp"
#include "redvault/e2ev3/pairing.hpp"
#include "redvault/e2ev3/send_queue.hpp"
#include "redvault/util/text.hpp"

// Part C2 on the desktop (docs/signal-integration/11-part-c2-communities.md): paid communities as full Signal
// groups, against the real engine and an in-memory server that applies the community rules of the "API (as built)"
// section: entitlement on every call, refusals that never filter (409 recipient_not_entitled with details.userIds),
// staff-only rosters, staff-only posting in a channel, and at most 100 devices per request.

#ifdef REDVAULT_HAS_E2E_SIGNAL

namespace e2 = redvault::e2ev3;
using e2::Json;
using redvault::api::ApiError;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("rv-c2-" + redvault::util::randomUuid());
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct Community {
    std::string id, conversationId, kind;
    std::set<std::string> staff, entitled, pending;
    /// Accounts the device-lists route still shows though they aren't entitled (a purge the view hasn't caught up with).
    std::set<std::string> staleListed;
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
    std::map<std::string, std::vector<Json>> rosters;
    std::map<std::string, Community> communities;  // by conversation id
    struct Event {
        std::int64_t seq;
        std::string communityId, kind, userId;
    };
    std::vector<Event> events;
    std::int64_t seq = 0;
    int counter = 0;
    /// Every message request that reached the server: (kind, how many devices). Refused ones too, marked "!".
    std::vector<std::pair<std::string, std::size_t>> requests;
    /// The next group-message requests to refuse with 503, once each (a server restarting).
    int failGroupRequests = 0;
    std::set<std::string> torDown;

    Community* byConversation(const std::string& conversationId) {
        const auto it = communities.find(conversationId);
        return it == communities.end() ? nullptr : &it->second;
    }
    Community& byId(const std::string& communityId) {
        for (auto& [conv, c] : communities) {
            if (c.id == communityId) return c;
        }
        throw ApiError(404, "not_found", "no such community");
    }
    void event(const Community& c, const std::string& kind, const std::string& userId) { events.push_back({++seq, c.id, kind, userId}); }
};

ApiError refusal(int status, const std::string& code, const std::set<std::string>& users = {}) {
    std::string details;
    if (!users.empty()) details = Json{{"userIds", std::vector<std::string>(users.begin(), users.end())}}.dump();
    return ApiError(status, code, code, details);
}

class FakeServer final : public e2::V3Server {
public:
    FakeServer(std::shared_ptr<State> state, std::string userId) : s_(std::move(state)), user_(std::move(userId)) {}

    Json config() override { return Json{{"enabled", true}, {"resetDelayHours", 72}, {"signal", true}}; }
    void gate(const char* call) const {
        if (s_->torDown.count("*") || s_->torDown.count(call)) throw ApiError(0, "tor_unavailable", "Tor isn't connected");
    }
    /// The first row of the rules table: the caller is enrolled and entitled.
    Community* entitledCaller(const std::string& conversationId) const {
        Community* c = s_->byConversation(conversationId);
        if (c && !c->entitled.count(user_)) throw refusal(403, "membership_expired");
        return c;
    }
    void refuseUnentitled(const Community& c, const Json& list) const {
        std::set<std::string> bad;
        for (const Json& m : list) {
            const std::string u = m.at("userId").get<std::string>();
            if (u != user_ && !c.entitled.count(u)) bad.insert(u);
        }
        if (!bad.empty()) throw refusal(409, "recipient_not_entitled", bad);
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
        std::lock_guard lock(s_->mutex);
        REQUIRE(devices.size() <= 100);
        if (Community* c = entitledCaller(conversationId)) {
            for (const auto& want : devices) {
                if (!c->entitled.count(want.at("userId").get<std::string>())) throw refusal(404, "not_found");
            }
        }
        Json out = Json::array();
        for (const auto& want : devices) {
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
    Json rosters(const std::string& conversationId, int afterEpoch) override {
        std::lock_guard lock(s_->mutex);
        entitledCaller(conversationId);
        Json out = Json::array();
        for (const auto& r : s_->rosters[conversationId]) {
            if (r.at("epoch").get<int>() > afterEpoch) out.push_back(r);
        }
        return out;
    }
    Json postRoster(const std::string& conversationId, const Json& roster) override {
        std::lock_guard lock(s_->mutex);
        if (Community* c = entitledCaller(conversationId)) {
            if (!c->staff.count(user_)) throw refusal(403, "not_community_staff");
            std::set<std::string> notEntitled, notStaff;
            for (const auto& m : roster.at("members")) {
                const std::string u = m.get<std::string>();
                if (!c->entitled.count(u)) notEntitled.insert(u);
                if (c->kind == "channel" && !c->staff.count(u)) notStaff.insert(u);
            }
            if (!notEntitled.empty()) throw refusal(409, "roster_member_not_entitled", notEntitled);
            if (!notStaff.empty()) throw refusal(409, "roster_member_not_staff", notStaff);
        }
        auto& chain = s_->rosters[conversationId];
        if (roster.at("epoch").get<int>() != static_cast<int>(chain.size()) + 1) throw refusal(409, "stale_roster");
        chain.push_back(roster);
        return Json{{"epoch", roster.at("epoch")}};
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
        if (Community* c = entitledCaller(conversationId); c && !c->entitled.count(userId)) throw refusal(404, "not_found");
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
    Json claimKeys(const std::string&, const Json& devices) override {
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
    void deliver(const std::string& senderDeviceId, const std::string& conversationId, const std::string& deviceId, const Json& body) {
        s_->mailboxes[deviceId].push_back({{"id", "m" + std::to_string(++s_->counter)},
                                          {"senderUserId", user_},
                                          {"senderDeviceId", senderDeviceId},
                                          {"conversationId", conversationId.empty() ? Json(nullptr) : Json(conversationId)},
                                          {"body", body}});
    }
    void sendMessages(const std::string& senderDeviceId, const std::string& conversationId, const Json& messages) override {
        gate("sendMessages");
        std::lock_guard lock(s_->mutex);
        if (messages.empty() || messages.size() > 100) throw refusal(400, "validation_error");
        try {
            if (Community* c = entitledCaller(conversationId)) refuseUnentitled(*c, messages);
        } catch (const ApiError&) {
            s_->requests.emplace_back("!messages", messages.size());
            throw;
        }
        s_->requests.emplace_back("messages", messages.size());
        for (const auto& m : messages) deliver(senderDeviceId, conversationId, m.at("deviceId").get<std::string>(), m.at("body"));
    }
    Json conversationDeviceLists(const std::string& conversationId) override {
        std::lock_guard lock(s_->mutex);
        Community* c = entitledCaller(conversationId);
        REQUIRE(c != nullptr);
        std::set<std::string> shown = c->entitled;
        shown.insert(c->staleListed.begin(), c->staleListed.end());
        // A channel's reader sees only the staff and themself.
        if (c->kind == "channel" && !c->staff.count(user_)) {
            shown = c->staff;
            shown.insert(user_);
        }
        Json members = Json::array();
        for (const auto& userId : shown) {
            members.push_back({{"userId", userId}, {"deviceList", s_->lists.count(userId) ? s_->lists[userId] : Json(nullptr)}, {"pendingReset", nullptr}});
        }
        return Json{{"members", members}};
    }
    void sendGroupMessage(const std::string& senderDeviceId, const std::string& conversationId, const std::string& body,
                          const Json& recipients) override {
        gate("sendGroupMessage");
        std::lock_guard lock(s_->mutex);
        if (recipients.empty() || recipients.size() > 100) throw refusal(400, "validation_error");
        if (s_->failGroupRequests > 0) {
            --s_->failGroupRequests;
            s_->requests.emplace_back("!group", recipients.size());
            throw refusal(503, "unavailable");
        }
        try {
            if (Community* c = entitledCaller(conversationId)) {
                if (c->kind == "channel" && !c->staff.count(user_)) throw refusal(403, "cannot_post");
                refuseUnentitled(*c, recipients);
            }
        } catch (const ApiError&) {
            s_->requests.emplace_back("!group", recipients.size());
            throw;
        }
        s_->requests.emplace_back("group", recipients.size());
        for (const auto& r : recipients) deliver(senderDeviceId, conversationId, r.at("deviceId").get<std::string>(), body);
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

/// The community routes, as the server answers them for `user`.
class FakeDirectory final : public e2::CommunityDirectory {
public:
    FakeDirectory(std::shared_ptr<State> state, std::string userId) : s_(std::move(state)), user_(std::move(userId)) {}
    Json membershipTasks() override {
        std::lock_guard lock(s_->mutex);
        Json out = Json::array();
        for (const auto& [conv, c] : s_->communities) {
            if (!c.staff.count(user_) || !c.entitled.count(user_)) continue;
            std::int64_t latest = 0;
            for (const auto& e : s_->events) {
                if (e.communityId == c.id) latest = e.seq;
            }
            out.push_back({{"communityId", c.id}, {"conversationId", conv}, {"kind", c.kind}, {"latestSeq", latest}, {"waiting", c.pending.size()}});
        }
        return out;
    }
    Json membershipTasksFor(const std::string& communityId, std::int64_t after, int limit) override {
        std::lock_guard lock(s_->mutex);
        Community& c = staffOnly(communityId);
        Json events = Json::array();
        std::int64_t cursor = after;
        bool more = false;
        for (const auto& e : s_->events) {
            if (e.communityId != c.id || e.seq <= after) continue;
            if (static_cast<int>(events.size()) == limit) {
                more = true;
                break;
            }
            events.push_back({{"seq", e.seq}, {"kind", e.kind}, {"userId", e.userId}, {"at", "2026-11-08T12:00:00.000Z"}});
            cursor = e.seq;
        }
        return Json{{"communityId", c.id}, {"conversationId", c.conversationId}, {"kind", c.kind}, {"cursor", cursor}, {"hasMore", more},
                    {"events", events}, {"waiting", std::vector<std::string>(c.pending.begin(), c.pending.end())}, {"lapsed", Json::array()}};
    }
    Json admit(const std::string& communityId, const std::string& userId) override {
        std::lock_guard lock(s_->mutex);
        Community& c = staffOnly(communityId);
        if (!c.pending.erase(userId)) throw refusal(409, "not_pending");
        c.entitled.insert(userId);
        s_->event(c, "join", userId);
        ++admits;
        return Json{{"membership", {{"status", "active"}, {"role", "member"}, {"canPost", true}}}};
    }
    Json recipients(const std::string& communityId) override {
        std::lock_guard lock(s_->mutex);
        Community& c = staffOnly(communityId);
        return Json{{"communityId", c.id}, {"conversationId", c.conversationId}, {"userIds", std::vector<std::string>(c.entitled.begin(), c.entitled.end())}};
    }
    std::vector<std::string> staff(const std::string& communityId) override {
        std::lock_guard lock(s_->mutex);
        Community& c = staffOnly(communityId);
        return std::vector<std::string>(c.staff.begin(), c.staff.end());
    }
    int admits = 0;

private:
    Community& staffOnly(const std::string& communityId) {
        Community& c = s_->byId(communityId);
        if (!c.entitled.count(user_)) throw refusal(404, "not_found");
        if (!c.staff.count(user_)) throw refusal(403, "forbidden");
        return c;
    }
    std::shared_ptr<State> s_;
    std::string user_;
};

e2::WaitOptions quick() {
    e2::WaitOptions wait;
    wait.intervalMs = 5;
    wait.timeoutMs = 20000;
    return wait;
}

/// One computer of one account: its store, its server, its communities and its device-key history.
struct Computer {
    TempDir dir;
    e2::Backend backend;
    std::shared_ptr<FakeDirectory> directory;
    std::shared_ptr<e2::QueueingV3Server> queueing;
    std::unique_ptr<e2::V3Device> device;
    std::unique_ptr<e2::ChatHistory> history;
    std::string userId;

    Computer(const std::shared_ptr<State>& state, const std::string& user, bool queued = false) : userId(user) {
        backend.store = std::make_shared<e2::DeviceStore>(dir.path, e2::makeFileVault(dir.path / "keys"));
        auto server = std::make_shared<FakeServer>(state, user);
        if (queued) {
            queueing = std::make_shared<e2::QueueingV3Server>(server);
            backend.server = queueing;
        } else {
            backend.server = server;
        }
        directory = std::make_shared<FakeDirectory>(state, user);
    }
    void firstDevice(char seed) {
        device = e2::setUpFirstDevice(userId, redvault::crypto::toBytes(std::string(32, seed)), withCommunities());
        ready();
    }
    void linkTo(Computer& first) {
        auto second = e2::startNewDevice(userId, withCommunities());
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
    e2::Communities& communities() { return *backend.communities; }
    void know(const std::string& communityId, const std::string& conversationId, const std::string& kind, bool staff) {
        communities().set(e2::CommunityInfo{communityId, conversationId, kind, staff, kind == "group" || staff});
    }
    std::vector<e2::ChatRecord> receive() { return e2::receiveChat(*device, *history); }
    std::vector<std::string> texts() {
        std::vector<std::string> out;
        for (const auto& r : receive()) {
            if (!r.op) out.push_back(r.text);
        }
        return out;
    }

private:
    const e2::Backend& withCommunities() {
        backend.communities = std::make_shared<e2::Communities>(directory, dir.path / "communities", redvault::crypto::randomBytes(32));
        return backend;
    }
    void ready() {
        if (queueing) {
            queueing->attach(std::make_shared<e2::SendQueue>(dir.path / "sendq", device->deriveKey("redvault-v3-send-queue")));
        }
        history = std::make_unique<e2::ChatHistory>(dir.path / ("history-" + device->deviceId()), device->deriveKey("redvault-v3-chat-history"));
        REQUIRE(e2::maintainSignal(*device, backend.server->config()) != "off");
    }
};

e2::OutgoingMessage textMessage(const std::string& text) {
    e2::OutgoingMessage m;
    m.text = text;
    return m;
}

std::string uuid() {
    return redvault::util::randomUuid();
}

/// Every device keeps Signal up to date, as the apps do on each refresh (a linked device's capability lands in the list).
void upkeep(const std::vector<Computer*>& computers) {
    for (int round = 0; round < 2; ++round) {
        for (Computer* c : computers) REQUIRE(e2::maintainSignal(*c->device, c->backend.server->config()) != "off");
    }
}

}  // namespace

TEST_CASE("part C2: a group community with three members, one on a linked device, and no history before joining", "[e2ev3][c2][community]") {
    auto state = std::make_shared<State>();
    const std::string owner = uuid(), bob = uuid(), carol = uuid(), dave = uuid();
    const std::string conv = uuid(), cid = uuid();
    state->communities[conv] = Community{cid, conv, "group", {owner}, {owner}, {bob, carol}, {}};

    Computer o(state, owner), b(state, bob), c1(state, carol), c2(state, carol);
    o.firstDevice('O');
    b.firstDevice('B');
    c1.firstDevice('C');
    c2.linkTo(c1);
    upkeep({&o, &b, &c1, &c2});
    o.know(cid, conv, "group", true);
    for (Computer* m : {&b, &c1, &c2}) m->know(cid, conv, "group", false);

    // Before the owner's app admits them, a member can't post: there is no roster with them in it.
    REQUIRE_THROWS(e2::sendChatMessage(*b.device, *b.history, conv, "", textMessage("too early")));

    // The owner's app runs the membership tasks: both waiting members are admitted, and the roster lists all three.
    const auto round = e2::runMembershipTasks(*o.device);
    REQUIRE(round.errors.empty());
    REQUIRE(round.admitted == 2);
    REQUIRE(round.rosters == 1);
    REQUIRE(state->rosters[conv].size() == 1);
    REQUIRE(state->rosters[conv][0].at("members").size() == 3);
    REQUIRE(state->rosters[conv][0].at("issuer") == owner);
    // The cursor moved: the next round has nothing to do.
    const auto again = e2::runMembershipTasks(*o.device);
    REQUIRE(again.communities == 0);

    // Every member's device that sends shares its own sender key; each message reaches every other device.
    for (Computer* m : {&b, &c1, &c2}) e2::noteCommunityJoined(*m->history, conv);
    e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("welcome"));
    for (Computer* m : {&b, &c1, &c2}) REQUIRE(m->texts() == std::vector<std::string>{"welcome"});
    e2::sendChatMessage(*c2.device, *c2.history, conv, "", textMessage("from carol's laptop"));
    REQUIRE(o.texts() == std::vector<std::string>{"from carol's laptop"});
    REQUIRE(b.texts() == std::vector<std::string>{"from carol's laptop"});
    REQUIRE(c1.receive().at(0).direction == "out");  // carol's other device has it as hers
    // A reaction (Part A) goes the same way.
    e2::ChatOp react;
    react.t = "reaction";
    react.target = o.history->list(conv).at(0).id;
    react.emoji = "+1";
    e2::sendChatOp(*b.device, *b.history, conv, "", react);
    o.receive();
    REQUIRE(o.history->list(conv).at(0).reactions.size() == 1);

    // The community is device to device only: the server holds no message once each device took its copy.
    for (Computer* m : {&o, &b, &c1, &c2}) m->receive();
    for (const auto& [device, box] : state->mailboxes) REQUIRE(box.empty());

    // dave joins later: he never sees what came before, and his chat says so.
    state->communities[conv].pending.insert(dave);
    Computer d(state, dave);
    d.firstDevice('D');
    d.know(cid, conv, "group", false);
    REQUIRE(e2::runMembershipTasks(*o.device).admitted == 1);
    REQUIRE(state->rosters[conv].size() == 2);
    e2::noteCommunityJoined(*d.history, conv);
    REQUIRE(e2::communityNotice(*d.history, conv) == e2::kBeforeYouJoined);
    REQUIRE(d.receive().empty());
    e2::sendChatMessage(*b.device, *b.history, conv, "", textMessage("hi dave"));
    REQUIRE(d.texts() == std::vector<std::string>{"hi dave"});
    REQUIRE(d.history->list(conv).size() == 1);

    // bob leaves: the next roster drops him, and the owner's next message uses a sender key he never gets.
    state->communities[conv].entitled.erase(bob);
    state->event(state->communities[conv], "leave", bob);
    b.receive();
    REQUIRE(e2::runMembershipTasks(*o.device).rosters == 1);
    REQUIRE(state->rosters[conv].size() == 3);
    e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("after bob left"));
    REQUIRE(state->mailboxes[b.device->deviceId()].empty());
    REQUIRE(d.texts() == std::vector<std::string>{"after bob left"});
}

TEST_CASE("part C2: a channel's staff post reaches the readers, readers can't post, and a removed reader can't read after rotation",
          "[e2ev3][c2][community][channel]") {
    auto state = std::make_shared<State>();
    const std::string owner = uuid(), m1 = uuid(), m2 = uuid();
    const std::string conv = uuid(), cid = uuid();
    state->communities[conv] = Community{cid, conv, "channel", {owner}, {owner}, {m1, m2}, {}};
    Computer o(state, owner), r1(state, m1), r2(state, m2);
    o.firstDevice('O');
    r1.firstDevice('1');
    r2.firstDevice('2');
    o.know(cid, conv, "channel", true);
    r1.know(cid, conv, "channel", false);
    r2.know(cid, conv, "channel", false);

    // The owner's app admits both, issues the staff-only roster and shares its sender key with the readers.
    const auto round = e2::runMembershipTasks(*o.device);
    REQUIRE(round.errors.empty());
    REQUIRE(round.admitted == 2);
    REQUIRE(state->rosters[conv].size() == 1);
    REQUIRE(state->rosters[conv][0].at("members") == Json::array({owner}));
    REQUIRE(round.shared == 2);

    e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("first post"));
    REQUIRE(r1.texts() == std::vector<std::string>{"first post"});
    REQUIRE(r2.texts() == std::vector<std::string>{"first post"});
    const std::string firstPost = r1.history->list(conv).at(0).id;

    // A reader can't post: the app refuses, and the engine would too (a reader isn't in the roster).
    try {
        e2::sendChatMessage(*r1.device, *r1.history, conv, "", textMessage("may I?"));
        FAIL("a reader posted");
    } catch (const e2::EngineError& error) {
        REQUIRE(error.code() == "cannot_post");
    }
    // A reader's reaction goes pairwise to the staff only; the other reader never learns of it.
    e2::ChatOp react;
    react.t = "reaction";
    react.target = firstPost;
    react.emoji = "+1";
    e2::sendChatOp(*r1.device, *r1.history, conv, "", react);
    REQUIRE(state->mailboxes[r2.device->deviceId()].empty());
    o.receive();
    REQUIRE(o.history->list(conv).at(0).reactions.size() == 1);
    // A text sent pairwise by a reader (an older or broken app) is never taken as a post.
    e2::sendChat(*r1.device, *r1.history, conv, owner, "sneaky post");
    REQUIRE(o.receive().empty());
    REQUIRE(o.history->list(conv).size() == 1);

    // Keep the second post's ciphertext for a dishonest-server check below.
    e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("second post"));
    const Json oldBody = Json::parse(state->mailboxes[r2.device->deviceId()].back().at("body").get<std::string>());
    REQUIRE(r2.texts() == std::vector<std::string>{"second post"});
    r1.receive();

    // m2 leaves. The owner's app rotates: the staff roster again, at the next epoch, so a new sender key.
    state->communities[conv].entitled.erase(m2);
    state->event(state->communities[conv], "leave", m2);
    const auto rotation = e2::runMembershipTasks(*o.device);
    REQUIRE(rotation.errors.empty());
    REQUIRE(rotation.rotations == 1);
    REQUIRE(state->rosters[conv].size() == 2);
    REQUIRE(state->rosters[conv][1].at("members") == Json::array({owner}));

    e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("after m2 left"));
    REQUIRE(r1.texts() == std::vector<std::string>{"after m2 left"});
    REQUIRE(state->mailboxes[r2.device->deviceId()].empty());
    // Even handed the ciphertext by a dishonest server, m2 can't read it: the new sender key never reached them.
    Json leaked;
    {
        // Encrypt one more post and copy its body before anyone takes it.
        e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("secret"));
        leaked = Json::parse(state->mailboxes[r1.device->deviceId()].back().at("body").get<std::string>());
    }
    REQUIRE(leaked.at("distributionId") != oldBody.at("distributionId"));
    REQUIRE(leaked.at("epoch").get<int>() == 2);
    const Json chain = Json(state->rosters[conv]);
    REQUIRE(r2.device->op("rosterVerify", Json{{"groupId", conv}, {"chain", chain}}).value("ok", false));
    const Json opened = r2.device->op("groupSignalDecrypt", Json{{"sender", o.device->identity()}, {"message", leaked}});
    REQUIRE_FALSE(opened.value("ok", true));
    REQUIRE(opened.value("error", std::string()) == "no_sender_key");
    REQUIRE(r1.texts() == std::vector<std::string>{"secret"});
}

TEST_CASE("part C2: recipient_not_entitled drops the named accounts and tries once more; membership_expired fails the send",
          "[e2ev3][c2][community]") {
    auto state = std::make_shared<State>();
    const std::string owner = uuid(), bob = uuid(), carol = uuid();
    const std::string conv = uuid(), cid = uuid();
    state->communities[conv] = Community{cid, conv, "group", {owner}, {owner}, {bob, carol}, {}};
    Computer o(state, owner), b(state, bob), c(state, carol);
    o.firstDevice('O');
    b.firstDevice('B');
    c.firstDevice('C');
    o.know(cid, conv, "group", true);
    b.know(cid, conv, "group", false);
    c.know(cid, conv, "group", false);
    REQUIRE(e2::runMembershipTasks(*o.device).rosters == 1);

    // bob's account was purged: no leave event, the roster still names him and the device-list view lags behind.
    // The server refuses, naming him; the app drops him and the message still reaches carol.
    state->communities[conv].entitled.erase(bob);
    state->communities[conv].staleListed.insert(bob);
    const auto sent = e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("still here?"));
    REQUIRE(sent.delivered == 1);
    REQUIRE(c.texts() == std::vector<std::string>{"still here?"});
    REQUIRE(state->mailboxes[b.device->deviceId()].empty());
    bool refused = false;
    for (const auto& [kind, n] : state->requests) refused = refused || kind[0] == '!';
    REQUIRE(refused);
    REQUIRE(o.history->list(conv).back().status == "sent");

    // carol's entitlement ends: her own sends fail with membership_expired, shown as failed.
    state->communities[conv].entitled.erase(carol);
    try {
        e2::sendChatMessage(*c.device, *c.history, conv, "", textMessage("am I still in?"));
        FAIL("a lapsed member sent");
    } catch (const ApiError& error) {
        REQUIRE(error.code() == "membership_expired");
    }
    REQUIRE(c.history->list(conv).back().status == "failed");
}

TEST_CASE("part C2: membership tasks admit, keep a cursor, and issue the staff roster on a role change", "[e2ev3][c2][community][tasks]") {
    auto state = std::make_shared<State>();
    const std::string owner = uuid(), mod = uuid(), reader = uuid();
    const std::string conv = uuid(), cid = uuid();
    state->communities[conv] = Community{cid, conv, "channel", {owner}, {owner}, {mod, reader}, {}};
    Computer o(state, owner), m(state, mod), r(state, reader);
    o.firstDevice('O');
    m.firstDevice('M');
    r.firstDevice('R');
    o.know(cid, conv, "channel", true);
    m.know(cid, conv, "channel", false);
    r.know(cid, conv, "channel", false);
    REQUIRE(e2::runMembershipTasks(*o.device).admitted == 2);
    const std::int64_t cursor = o.communities().cursor(cid);
    REQUIRE(cursor == 2);


    // mod becomes a moderator: a role event, so the owner's app issues the staff roster with them.
    state->communities[conv].staff.insert(mod);
    state->event(state->communities[conv], "role", mod);
    const auto round = e2::runMembershipTasks(*o.device);
    REQUIRE(round.errors.empty());
    REQUIRE(round.rosters == 1);
    REQUIRE(state->rosters[conv].back().at("members").size() == 2);
    // Now staff, mod posts; the reader reads it; the owner's devices too.
    m.know(cid, conv, "channel", true);
    e2::sendChatMessage(*m.device, *m.history, conv, "", textMessage("from the moderator"));
    REQUIRE(r.texts() == std::vector<std::string>{"from the moderator"});
    REQUIRE(o.texts() == std::vector<std::string>{"from the moderator"});

    // A reader's app never gets the membership tasks (the server says forbidden) and never issues a roster.
    REQUIRE(e2::runMembershipTasks(*r.device).communities == 0);
    REQUIRE_THROWS(e2::issueCommunityRoster(*r.device, e2::CommunityInfo{cid, conv, "channel", false, false}, {reader}));
}

TEST_CASE("part C2: one post to more than 100 devices goes in ordered batches of at most 100, and a failed batch is retried",
          "[e2ev3][c2][community][batches]") {
    auto state = std::make_shared<State>();
    const std::string owner = uuid(), conv = uuid(), cid = uuid();
    constexpr int kReaders = 103;
    std::vector<std::string> readers;
    for (int i = 0; i < kReaders; ++i) readers.push_back(uuid());
    state->communities[conv] = Community{cid, conv, "channel", {owner}, {owner}, {}, {}};
    for (const auto& r : readers) state->communities[conv].pending.insert(r);
    Computer o(state, owner);
    o.firstDevice('O');
    o.know(cid, conv, "channel", true);
    std::vector<std::unique_ptr<Computer>> rs;
    for (const auto& r : readers) {
        rs.push_back(std::make_unique<Computer>(state, r));
        rs.back()->firstDevice('r');
        rs.back()->know(cid, conv, "channel", false);
    }
    // Admit everyone and share the key: 103 sender-key messages in two requests.
    state->requests.clear();
    const auto round = e2::runMembershipTasks(*o.device);
    REQUIRE(round.errors.empty());
    REQUIRE(round.admitted == kReaders);
    REQUIRE(round.shared == kReaders);
    REQUIRE(state->requests == std::vector<std::pair<std::string, std::size_t>>{{"messages", 100}, {"messages", 3}});

    // One post: encrypted once, sent in two batches; the first try of the first batch meets a 503 and is tried again.
    state->requests.clear();
    state->failGroupRequests = 1;
    const auto sent = e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("to everyone"));
    REQUIRE(sent.delivered == kReaders);
    REQUIRE(state->requests == std::vector<std::pair<std::string, std::size_t>>{{"!group", 100}, {"group", 100}, {"group", 3}});
    std::set<std::string> bodies;
    for (const auto& r : rs) bodies.insert(state->mailboxes[r->device->deviceId()].back().at("body").get<std::string>());
    REQUIRE(bodies.size() == 1);  // the same ciphertext for every device
    REQUIRE(rs.front()->texts() == std::vector<std::string>{"to everyone"});
    REQUIRE(rs.back()->texts() == std::vector<std::string>{"to everyone"});
}

TEST_CASE("part C2: a community post made while Tor is down waits, encrypted once, and goes in order with its batches",
          "[e2ev3][c2][community][tor]") {
    auto state = std::make_shared<State>();
    const std::string owner = uuid(), bob = uuid(), conv = uuid(), cid = uuid();
    state->communities[conv] = Community{cid, conv, "group", {owner}, {owner}, {bob}, {}};
    Computer o(state, owner, true), b(state, bob);
    o.firstDevice('O');
    b.firstDevice('B');
    o.know(cid, conv, "group", true);
    b.know(cid, conv, "group", false);
    REQUIRE(e2::runMembershipTasks(*o.device).rosters == 1);

    state->torDown.insert("sendGroupMessage");
    const auto sent = e2::sendChatMessage(*o.device, *o.history, conv, "", textMessage("over tor"));
    REQUIRE(sent.queued);
    REQUIRE(o.history->list(conv).back().status == "queued");
    REQUIRE(b.texts().empty());
    state->torDown.clear();
    const auto drained = e2::sendQueuedChat(*o.device, *o.history, *o.queueing);
    REQUIRE(drained.sent == 1);
    REQUIRE(o.history->list(conv).back().status == "sent");
    REQUIRE(b.texts() == std::vector<std::string>{"over tor"});
}

TEST_CASE("part C2: the notice about messages from before joining", "[e2ev3][c2][community]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    {
        e2::ChatHistory history(dir.path / "h", key);
        REQUIRE(e2::communityNotice(history, "c1").empty());
        e2::noteCommunityJoined(history, "c1", 1000);
        e2::noteCommunityJoined(history, "c1", 5000);  // the first time stays
        REQUIRE(e2::communityNotice(history, "c1") == std::string("Messages from before you joined aren't available"));
        REQUIRE(e2::communityNotice(history, "other").empty());
    }
    e2::ChatHistory reopened(dir.path / "h", key);
    REQUIRE(reopened.joinedAt("c1") == std::optional<std::int64_t>(1000));
}

#endif
