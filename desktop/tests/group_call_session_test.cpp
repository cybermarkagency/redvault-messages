// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/group_call_crypto.hpp"
#include "redvault/groupcall/session.hpp"
#include "redvault/model/group_call_link.hpp"

using namespace redvault;
using Json = nlohmann::json;
using lk::GroupCallSession;
using redvault::test::FakeTransport;
namespace gc = redvault::crypto::groupcalls;

namespace {

constexpr const char* kCall = "3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b";
constexpr const char* kGroup = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";
const std::string kSite = "https://www.redvaultsystems.com/redvaultmessages/";

/// Everything the session did to its room, in order.
struct FakeRoom final : lk::Room {
    std::vector<std::string>* log;
    lk::RoomCallbacks callbacks;
    std::string identity = "p-self";
    std::vector<lk::RemoteParticipant> remotes;
    std::vector<std::vector<std::uint8_t>> keys;
    std::vector<std::int64_t> epochs;
    bool failMicrophone = false;
    bool failCamera = false;
    bool failScreen = false;
    bool encrypting = true;
    bool encryptionCheckThrows = false;
    int audioFrames = 0;
    bool micMuted = false;

    FakeRoom(std::vector<std::string>* l, lk::RoomCallbacks c) : log(l), callbacks(std::move(c)) {}
    std::string localIdentity() const override { return identity; }
    std::vector<lk::RemoteParticipant> participants() const override { return remotes; }
    void setKey(const std::vector<std::uint8_t>& key, std::int64_t epoch) override {
        keys.push_back(key);
        epochs.push_back(epoch);
        log->push_back("setKey " + std::to_string(epoch));
    }
    bool frameEncryptionActive() override {
        if (encryptionCheckThrows) throw lk::FfiError("no answer");
        return encrypting;
    }
    void publishMicrophone() override {
        if (failMicrophone) throw lk::FfiError("no mic");
        log->push_back("publishMicrophone");
    }
    void sendAudio(const std::int16_t*) override { ++audioFrames; }
    void setMicrophoneMuted(bool muted) override {
        micMuted = muted;
        log->push_back(muted ? "mute" : "unmute");
    }
    void readAudio(std::int16_t* out, std::size_t samples) override { std::fill(out, out + samples, std::int16_t{42}); }
    void publishVideo(lk::VideoSource s, int, int) override {
        const bool screen = s == lk::VideoSource::Screen;
        if (screen ? failScreen : failCamera) throw lk::FfiError("no video");
        log->push_back(screen ? "publishScreen" : "publishCamera");
    }
    void sendVideo(lk::VideoSource, const lk::VideoFrame&) override {}
    void setVideoMuted(lk::VideoSource s, bool muted) override {
        log->push_back(std::string(s == lk::VideoSource::Screen ? "screen" : "camera") + (muted ? " off" : " on"));
    }
    void unpublishVideo(lk::VideoSource s) override { log->push_back(s == lk::VideoSource::Screen ? "unpublishScreen" : "unpublishCamera"); }
    void disconnect() override { log->push_back("disconnect"); }
};

struct Person {
    std::string userId;
    std::string handle;
    crypto::KeyPair keys;
};

/// A session wired to a fake server, a fake LiveKit room and two manual queues, so every step is deterministic.
struct Harness {
    Person self{"alice", "alice", crypto::generateKeyPair()};
    Person bob{"bob", "bob", crypto::generateKeyPair()};
    Person carol{"carol", "carol", crypto::generateKeyPair()};

    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::vector<std::string> log;  // room operations and API calls, interleaved
    std::vector<std::string> announced;
    std::vector<std::string> requests;
    std::vector<Json> keyPosts;
    std::deque<std::function<void()>> posts;
    std::deque<std::function<void()>> tasks;
    FakeRoom* room = nullptr;
    std::string roomUrl;
    std::string roomToken;
    int roomsCreated = 0;
    bool connectFails = false;
    bool announceOk = true;
    bool otherCall = false;
    std::string tier = "premium";
    bool created = true;      // the next start/join response says created
    int keysStatus = 204;
    bool serverHasActive = false;  // what GET /active says
    std::string joinKind = "video";
    int changes = 0;
    int activeChanges = 0;
    std::map<std::string, lk::VideoFrame> frames;
    std::unique_ptr<GroupCallSession> session;

    explicit Harness(bool premium = true) {
        if (!premium) tier = "free";
        transport->handler = [this](const net::HttpRequest& r) { return respond(r); };
        auto self_ = std::make_shared<data::SelfIdentity>();
        self_->userId = self.userId;
        self_->handle = self.handle;
        self_->publicKey = self.keys.publicKey;
        self_->privateKey = crypto::SecretBytes::copyOf(self.keys.privateKey.view());
        self_->subscriptionTier = tier;

        GroupCallSession::Environment env;
        env.api = std::make_shared<api::ApiClient>("https://example.com/api", transport, std::make_shared<net::SessionCookieJar>());
        env.self = self_;
        env.siteBase = kSite;
        env.group = [this](const std::string& id) -> std::optional<GroupCallSession::GroupInfo> {
            if (id != kGroup) return std::nullopt;
            GroupCallSession::GroupInfo g;
            g.label = "Weekend plans";
            g.members = {{self.userId, self.handle, "", self.keys.publicKey},
                         {bob.userId, bob.handle, "", bob.keys.publicKey},
                         {carol.userId, carol.handle, "", carol.keys.publicKey}};
            return g;
        };
        env.tier = [this] { return tier; };
        env.otherCallActive = [this] { return otherCall; };
        env.post = [this](std::function<void()> fn) { posts.push_back(std::move(fn)); };
        env.background = [this](std::function<void()> fn) { tasks.push_back(std::move(fn)); };
        env.announce = [this](const std::string& conversationId, const std::string& text) {
            log.push_back("announce");
            announced.push_back(conversationId + "|" + text);
            return announceOk;
        };
        env.roomFactory = [this](const lk::RoomConfig& config, lk::RoomCallbacks callbacks) -> std::unique_ptr<lk::Room> {
            if (connectFails) throw lk::FfiError("unreachable");
            roomUrl = config.url;
            roomToken = config.token;
            ++roomsCreated;
            log.push_back("connect");
            auto r = std::make_unique<FakeRoom>(&log, std::move(callbacks));
            room = r.get();
            return r;
        };
        env.now = [] { return std::int64_t{1790000000000}; };

        GroupCallSession::Hooks hooks;
        hooks.changed = [this] { ++changes; };
        hooks.activeChanged = [this] { ++activeChanges; };
        hooks.video = [this](const std::string& key, const lk::VideoFrame& f) { frames[key] = f; };
        session = std::make_unique<GroupCallSession>(std::move(env), std::move(hooks));
    }

    net::HttpResponse respond(const net::HttpRequest& r) {
        const std::string path = r.url.substr(std::string("https://example.com/api/").size());
        requests.push_back(r.method + " " + path);
        log.push_back("api " + r.method + " " + path);
        auto joinBody = [&](bool isCreated) {
            return Json{{"callId", kCall}, {"conversationId", kGroup}, {"kind", joinKind}, {"url", "/redvaultmessages/livekit"},
                        {"token", "jwt-token"}, {"identity", "p-self"}, {"created", isCreated}}
                .dump();
        };
        if (path == "group-calls" && r.method == "POST") return {201, {}, joinBody(created)};
        if (path == std::string("group-calls/") + kCall + "/join") return {200, {}, joinBody(false)};
        if (path == std::string("group-calls/") + kCall + "/keys") {
            keyPosts.push_back(Json::parse(r.body));
            if (keysStatus == 204) return {204, {}, ""};
            return {keysStatus, {}, R"({"error":"stale_epoch","message":"The call changed; send the key for the current epoch."})"};
        }
        if (path == "group-calls/active") {
            if (!serverHasActive) return {200, {}, R"({"calls":[]})"};
            return {200, {}, R"({"calls":[{"callId":")" + std::string(kCall) + R"(","conversationId":")" + kGroup + R"(","kind":"video","participants":2}]})"};
        }
        return {204, {}, ""};
    }

    /// Runs queued work until nothing is left.
    void pump() {
        for (int guard = 0; guard < 1000 && (!tasks.empty() || !posts.empty()); ++guard) {
            if (!tasks.empty()) {
                auto fn = std::move(tasks.front());
                tasks.pop_front();
                fn();
            } else {
                auto fn = std::move(posts.front());
                posts.pop_front();
                fn();
            }
        }
    }

    api::live::LiveEvent roster(std::int64_t epoch, const std::string& keyholder, std::vector<std::pair<std::string, std::string>> members) {
        api::live::LiveEvent e;
        e.type = "group-roster";
        e.callId = kCall;
        e.conversationId = kGroup;
        e.epoch = epoch;
        e.keyholder = keyholder;
        for (const auto& [user, identity] : members) e.members.push_back({user, identity});
        return e;
    }

    api::live::LiveEvent keyFor(const Person& from, const Person& to, std::int64_t epoch, const crypto::SecretBytes& key) {
        const gc::KeyMessage message{kCall, kGroup, epoch, from.userId, to.userId};
        const auto sealed = gc::sealRoomKey(message, key, to.keys.publicKey, from.keys.privateKey, 1790000000000);
        api::live::LiveEvent e;
        e.type = "group-key";
        e.callId = kCall;
        e.epoch = epoch;
        e.from = from.userId;
        e.sealed = api::live::SealedSignal{sealed.ciphertext, sealed.nonce};
        return e;
    }

    api::live::LiveEvent incoming(const std::string& host = "bob") {
        api::live::LiveEvent e;
        e.type = "group-incoming";
        e.callId = kCall;
        e.conversationId = kGroup;
        e.hostId = host;
        e.kind = "video";
        return e;
    }

    std::size_t indexOf(const std::string& entry) const {
        const auto it = std::find(log.begin(), log.end(), entry);
        return it == log.end() ? log.size() : static_cast<std::size_t>(it - log.begin());
    }
    bool did(const std::string& entry) const { return indexOf(entry) < log.size(); }
};

}  // namespace

TEST_CASE("the starter connects, hands out the key, then publishes, then posts the link", "[groupcall][session]") {
    Harness h;
    CHECK(h.session->start(kGroup, "video").empty());
    CHECK(h.session->view().phase == "connecting");
    CHECK(h.session->view().kind == "video");
    CHECK(h.session->view().groupLabel == "Weekend plans");
    h.pump();

    // LiveKit's path on the site became a websocket URL, and the token was passed on as is.
    CHECK(h.roomUrl == "wss://www.redvaultsystems.com/redvaultmessages/livekit");
    CHECK(h.roomToken == "jwt-token");
    CHECK(h.session->view().phase == "active");
    CHECK(h.session->view().connectedAt == 1790000000000);
    CHECK(h.requests.front() == "POST group-calls");

    // The link is posted once the starter is connected, word for word as the web app writes it.
    REQUIRE(h.announced.size() == 1);
    const std::string link = model::group_calls::joinUrl(kSite, kCall);
    CHECK(h.announced[0] == std::string(kGroup) + "|" + model::group_calls::inviteText("video", link));
    CHECK(h.indexOf("connect") < h.indexOf("announce"));

    // Nothing is published before a key is held.
    CHECK_FALSE(h.did("publishMicrophone"));
    CHECK_FALSE(h.session->view().published);

    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();
    CHECK(h.session->view().keyReady);
    REQUIRE(h.room->keys.size() == 1);
    CHECK(h.room->keys[0].size() == 32);
    CHECK(h.room->epochs[0] == 1);
    CHECK(h.indexOf("setKey 1") < h.indexOf("publishMicrophone"));
    CHECK(h.indexOf("publishMicrophone") < h.indexOf("publishCamera"));
    CHECK(h.session->view().published);
    // Alone in the call there is nobody to send a key to.
    CHECK(h.keyPosts.empty());

    // Bob joins: a new epoch and a fresh key, sealed to bob and delivered before anything slow.
    h.session->onLiveEvent(h.roster(2, "alice", {{"alice", "p-self"}, {"bob", "p-bob"}}));
    h.pump();
    REQUIRE(h.keyPosts.size() == 1);
    CHECK(h.keyPosts[0]["epoch"] == 2);
    REQUIRE(h.keyPosts[0]["keys"].size() == 1);
    CHECK(h.keyPosts[0]["keys"][0]["userId"] == "bob");
    const crypto::calls::Sealed forBob{h.keyPosts[0]["keys"][0]["key"]["ciphertext"], h.keyPosts[0]["keys"][0]["key"]["nonce"]};
    const auto opened = gc::openRoomKey(forBob, h.self.keys.publicKey, h.bob.keys.privateKey, gc::KeyMessage{kCall, kGroup, 2, "alice", "bob"}, 1790000000000);
    REQUIRE(h.room->keys.size() == 2);
    CHECK(std::vector<std::uint8_t>(opened.data(), opened.data() + opened.size()) == h.room->keys[1]);
    CHECK(h.room->keys[0] != h.room->keys[1]);
    // The camera is published once, not again for every epoch.
    CHECK(std::count(h.log.begin(), h.log.end(), "publishCamera") == 1);
    CHECK(std::count(h.log.begin(), h.log.end(), "publishMicrophone") == 1);
    CHECK(h.session->view().sas.size() == 4);
}

TEST_CASE("joining by a link: events that beat the connection are held, and nothing is sent before the key", "[groupcall][session]") {
    Harness h;
    h.joinKind = "voice";
    CHECK(h.session->joinByLink(kCall).empty());
    CHECK(h.session->view().phase == "connecting");

    // The room's roster and the keyholder's key arrive while we are still connecting (key first, even).
    crypto::SecretBytes roomKey = gc::newRoomKey();
    h.session->onLiveEvent(h.keyFor(h.bob, h.self, 3, roomKey));
    h.session->onLiveEvent(h.roster(3, "bob", {{"bob", "p-bob"}, {"alice", "p-self"}}));
    h.pump();

    CHECK(h.roomsCreated == 1);
    CHECK(h.session->view().phase == "active");
    CHECK(h.session->view().kind == "voice");
    CHECK(h.announced.empty());  // a link doesn't make anyone the starter
    REQUIRE(h.room->keys.size() == 1);
    CHECK(h.room->keys[0] == std::vector<std::uint8_t>(roomKey.data(), roomKey.data() + roomKey.size()));
    CHECK(h.room->epochs[0] == 3);
    CHECK(h.indexOf("connect") < h.indexOf("setKey 3"));
    CHECK(h.indexOf("setKey 3") < h.indexOf("publishMicrophone"));
    CHECK_FALSE(h.did("publishCamera"));  // a voice call
    CHECK(h.session->view().published);
    CHECK(h.keyPosts.empty());
}

TEST_CASE("a member who isn't the keyholder publishes nothing until the keyholder's key arrives", "[groupcall][session]") {
    Harness h;
    h.session->joinByLink(kCall);
    h.pump();
    h.session->onLiveEvent(h.roster(1, "bob", {{"bob", "p-bob"}, {"alice", "p-self"}}));
    h.pump();
    CHECK_FALSE(h.session->view().keyReady);
    CHECK_FALSE(h.did("publishMicrophone"));
    CHECK(h.room->keys.empty());

    // A key from someone who isn't the keyholder, and one sealed by a stranger: refused, nothing published.
    h.session->onLiveEvent(h.keyFor(h.carol, h.self, 1, gc::newRoomKey()));
    auto stranger = crypto::generateKeyPair();
    const gc::KeyMessage forged{kCall, kGroup, 1, "bob", "alice"};
    const auto fake = gc::sealRoomKey(forged, gc::newRoomKey(), h.self.keys.publicKey, stranger.privateKey, 1790000000000);
    api::live::LiveEvent bad;
    bad.type = "group-key";
    bad.callId = kCall;
    bad.epoch = 1;
    bad.from = "bob";
    bad.sealed = api::live::SealedSignal{fake.ciphertext, fake.nonce};
    h.session->onLiveEvent(bad);
    h.pump();
    CHECK_FALSE(h.did("publishMicrophone"));
    CHECK(h.room->keys.empty());
    CHECK_FALSE(h.session->view().notice.empty());

    // The real one.
    crypto::SecretBytes key = gc::newRoomKey();
    h.session->onLiveEvent(h.keyFor(h.bob, h.self, 1, key));
    h.pump();
    CHECK(h.session->view().keyReady);
    CHECK(h.session->view().notice.empty());
    CHECK(h.indexOf("setKey 1") < h.indexOf("publishMicrophone"));
}

TEST_CASE("a refused start shows the server's own reason", "[groupcall][session]") {
    Harness h(false);
    h.transport->handler = [&h](const net::HttpRequest& r) -> net::HttpResponse {
        h.requests.push_back(r.method + " " + r.url);
        return {403, {}, R"({"error":"premium_required","message":"Group video calls are a Premium feature."})"};
    };
    CHECK(h.session->start(kGroup, "video").empty());
    h.pump();
    CHECK(h.session->view().phase == "ended");
    CHECK(h.session->view().endedMessage == "Group video calls are a Premium feature.");
    CHECK(h.roomsCreated == 0);
    CHECK_FALSE(h.session->inCall());
    h.session->dismiss();
    CHECK_FALSE(h.session->view().present);
}

TEST_CASE("a server we can't reach in time ends the attempt and tells the server we left", "[groupcall][session]") {
    Harness h;
    h.connectFails = true;
    h.session->start(kGroup, "voice");
    h.pump();
    CHECK(h.session->view().phase == "ended");
    CHECK(h.session->view().endedMessage == "Couldn't reach the call server. Try again in a minute.");
    CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/leave"));
}

TEST_CASE("leaving disconnects, tells the server, and shows the closing banner", "[groupcall][session]") {
    Harness h;
    h.session->start(kGroup, "voice");
    h.pump();
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();
    h.session->leave();
    h.pump();
    CHECK(h.did("disconnect"));
    CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/leave"));
    CHECK(h.session->view().phase == "ended");
    CHECK(h.session->view().endedMessage == "You left the call");
    CHECK(h.session->view().tiles.empty());
    // Events for a call we left are ignored.
    h.session->onLiveEvent(h.roster(5, "alice", {{"alice", "p-self"}}));
    CHECK(h.session->view().phase == "ended");
}

TEST_CASE("leaving while still connecting doesn't leave a room behind", "[groupcall][session]") {
    Harness h;
    h.session->start(kGroup, "voice");
    // Run only the API step, then leave before the room connects.
    h.tasks.front()();
    h.tasks.pop_front();
    h.posts.front()();
    h.posts.pop_front();
    h.session->leave();
    h.pump();
    CHECK(h.session->view().endedMessage == "You left the call");
    CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/leave"));
    // The connection that was already under way is dropped as soon as it completes.
    CHECK(h.roomsCreated == 1);
    CHECK(h.did("disconnect"));
    CHECK_FALSE(h.did("setKey 1"));
}

TEST_CASE("the call ending, or the connection dropping, ends it here too", "[groupcall][session]") {
    {
        Harness h;
        h.session->start(kGroup, "voice");
        h.pump();
        api::live::LiveEvent ended;
        ended.type = "group-ended";
        ended.callId = kCall;
        ended.conversationId = kGroup;
        h.session->onLiveEvent(ended);
        h.pump();
        CHECK(h.session->view().endedMessage == "Call ended");
        CHECK(h.session->active().empty());
    }
    {
        Harness h;
        h.session->start(kGroup, "voice");
        h.pump();
        h.room->callbacks.onState(lk::RoomState::Reconnecting, "");
        h.pump();
        CHECK(h.session->view().phase == "reconnecting");
        h.room->callbacks.onState(lk::RoomState::Connected, "");
        h.pump();
        CHECK(h.session->view().phase == "active");
        h.room->callbacks.onState(lk::RoomState::Disconnected, "SIGNAL_CLOSE");
        h.pump();
        CHECK(h.session->view().endedMessage == "Call dropped");
        CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/leave"));
    }
}

TEST_CASE("a call rings, can be declined or answered, and stops ringing on its own", "[groupcall][session]") {
    {
        Harness h;
        h.session->onLiveEvent(h.incoming("bob"));
        CHECK(h.session->view().phase == "incoming");
        CHECK(h.session->view().hostHandle == "bob");
        CHECK(h.session->view().groupLabel == "Weekend plans");
        REQUIRE(h.session->active().count(kGroup) == 1);
        h.session->decline();
        h.pump();
        CHECK_FALSE(h.session->view().present);
        CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/decline"));
    }
    {
        Harness h;
        h.session->onLiveEvent(h.incoming("bob"));
        api::live::LiveEvent stop;
        stop.type = "group-ring-stop";
        stop.callId = kCall;
        h.session->onLiveEvent(stop);
        CHECK_FALSE(h.session->view().present);
        // The call may still be on: the chat keeps its Join button.
        CHECK(h.session->active().count(kGroup) == 1);
    }
    {
        Harness h;
        h.session->onLiveEvent(h.incoming("bob"));
        h.session->accept();
        h.pump();
        CHECK(h.session->view().phase == "active");
        CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/join"));
        CHECK(h.announced.empty());
    }
    {
        // Your own call doesn't ring you, and neither does one while you are busy.
        Harness h;
        h.session->onLiveEvent(h.incoming("alice"));
        CHECK_FALSE(h.session->view().present);
        Harness busy;
        busy.otherCall = true;
        busy.session->onLiveEvent(busy.incoming("bob"));
        CHECK_FALSE(busy.session->view().present);
        CHECK(busy.session->active().count(kGroup) == 1);
    }
    {
        // Pasting the link of the call that is ringing is the same as answering it.
        Harness h;
        h.session->onLiveEvent(h.incoming("bob"));
        CHECK(h.session->joinByLink(kCall).empty());
        h.pump();
        CHECK(h.session->view().phase == "active");
        CHECK(h.roomsCreated == 1);
    }
}

TEST_CASE("nobody is in two calls at once", "[groupcall][session]") {
    Harness h;
    h.session->start(kGroup, "voice");
    h.pump();
    CHECK_FALSE(h.session->start(kGroup, "voice").empty());
    CHECK_FALSE(h.session->joinByLink(kCall).empty());

    Harness other;
    other.otherCall = true;
    CHECK(other.session->start(kGroup, "voice") == "You're already in a call. Leave it first.");
    CHECK(other.session->joinByLink(kCall) == "You're already in a call. Leave it first.");
    CHECK_FALSE(other.session->view().present);
}

TEST_CASE("a call that is already going on is joined, not started a second time", "[groupcall][session]") {
    Harness h;
    h.serverHasActive = true;
    h.session->refreshActive();
    h.pump();
    REQUIRE(h.session->active().count(kGroup) == 1);
    CHECK(h.session->active().at(kGroup).participants == 2);
    h.requests.clear();
    h.session->start(kGroup, "video");
    h.pump();
    CHECK(h.requests.front() == "POST group-calls/" + std::string(kCall) + "/join");
    CHECK(h.announced.empty());
}

TEST_CASE("a link into a group that isn't loaded is turned down and the server told", "[groupcall][session]") {
    Harness h;
    h.transport->handler = [&h](const net::HttpRequest& r) -> net::HttpResponse {
        h.log.push_back("api " + r.method + " " + r.url.substr(std::string("https://example.com/api/").size()));
        if (r.url.find("/join") != std::string::npos) {
            return {200, {}, Json{{"callId", kCall}, {"conversationId", "someone-elses-group"}, {"kind", "voice"}, {"url", "wss://x/y"},
                                  {"token", "t"}, {"identity", "p-x"}, {"created", false}}.dump()};
        }
        return {204, {}, ""};
    };
    h.session->joinByLink(kCall);
    h.pump();
    CHECK(h.session->view().endedMessage.find("isn't loaded yet") != std::string::npos);
    CHECK(h.roomsCreated == 0);
    CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/leave"));
}

TEST_CASE("a stale-epoch refusal from the server is not an error, other failures are", "[groupcall][session]") {
    Harness h;
    h.session->start(kGroup, "voice");
    h.pump();
    h.keysStatus = 409;
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}, {"bob", "p-bob"}}));
    h.pump();
    REQUIRE(h.keyPosts.size() == 1);
    CHECK(h.session->view().notice.empty());

    h.keysStatus = 500;
    h.session->onLiveEvent(h.roster(2, "alice", {{"alice", "p-self"}, {"bob", "p-bob"}, {"carol", "p-carol"}}));
    h.pump();
    CHECK(h.session->view().notice == "Couldn't hand out the call key. Others may not hear you.");
}

TEST_CASE("controls: mute before and after publishing, the camera, and screen sharing needs Premium", "[groupcall][session]") {
    Harness h;
    h.session->start(kGroup, "video");
    h.pump();
    // Muting while the room is up but before the key arrives is remembered and applied.
    h.session->toggleMute();
    h.pump();
    CHECK(h.session->view().muted);
    CHECK(h.room->micMuted);
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();
    CHECK(h.session->view().published);

    h.session->toggleCamera();
    h.pump();
    CHECK(h.session->view().cameraOff);
    CHECK(h.did("camera off"));
    h.session->toggleCamera();
    h.pump();
    CHECK_FALSE(h.session->view().cameraOff);
    h.session->cameraFailed();
    h.pump();
    CHECK(h.session->view().cameraOff);
    CHECK(h.session->view().notice == "Couldn't use your camera.");

    CHECK(h.session->toggleScreenShare().empty());
    h.pump();
    CHECK(h.session->view().sharingScreen);
    CHECK(h.did("publishScreen"));
    bool hasScreenTile = false;
    for (const auto& t : h.session->view().tiles) hasScreenTile = hasScreenTile || t.key == "self:screen";
    CHECK(hasScreenTile);
    h.session->screenStopped();
    h.pump();
    CHECK_FALSE(h.session->view().sharingScreen);
    CHECK(h.did("unpublishScreen"));

    Harness basic(false);
    basic.joinKind = "voice";
    basic.session->start(kGroup, "voice");
    basic.pump();
    basic.session->onLiveEvent(basic.roster(1, "alice", {{"alice", "p-self"}}));
    basic.pump();
    CHECK(basic.session->toggleScreenShare() == "Screen sharing is part of Premium.");
    // A voice call has no camera to toggle.
    basic.session->toggleCamera();
    basic.pump();
    CHECK_FALSE(basic.did("camera off"));
}

TEST_CASE("a camera or microphone that can't be published is reported without losing the call", "[groupcall][session]") {
    {
        Harness h;
        h.session->start(kGroup, "video");
        h.pump();
        h.room->failCamera = true;
        h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
        h.pump();
        CHECK(h.session->view().phase == "active");
        CHECK(h.session->view().published);
        CHECK(h.session->view().cameraOff);
        CHECK(h.session->view().notice == "Couldn't use your camera.");
    }
    {
        Harness h;
        h.session->start(kGroup, "voice");
        h.pump();
        h.room->failMicrophone = true;
        h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
        h.pump();
        CHECK(h.session->view().phase == "ended");
        CHECK(h.did("api POST group-calls/" + std::string(kCall) + "/leave"));
    }
}

TEST_CASE("the call screen shows everyone, by name, speaking and muted", "[groupcall][session]") {
    Harness h;
    h.session->start(kGroup, "video");
    h.pump();
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}, {"bob", "p-bob"}, {"carol", "p-carol"}}));
    h.pump();

    lk::RemoteParticipant bob;
    bob.identity = "p-bob";
    bob.micPublished = true;
    bob.micMuted = false;
    bob.cameraPublished = true;
    bob.cameraMuted = false;
    bob.cameraReceiving = true;
    lk::RemoteParticipant carol;
    carol.identity = "p-carol";
    carol.micPublished = true;
    carol.micMuted = true;
    carol.screenPublished = true;
    carol.screenReceiving = true;
    lk::RemoteParticipant stranger;
    stranger.identity = "p-unknown";
    h.room->remotes = {stranger, carol, bob};
    h.room->callbacks.onParticipantsChanged();
    h.room->callbacks.onActiveSpeakers({"p-bob"});
    h.pump();

    const auto& tiles = h.session->view().tiles;
    REQUIRE(tiles.size() == 5);
    CHECK(tiles[0].key == "self");
    CHECK(tiles[0].handle == "You");
    // In the order people joined, a screen right after its owner; people the roster doesn't know come last.
    CHECK(tiles[1].key == "p-bob:camera");
    CHECK(tiles[1].handle == "bob");
    CHECK(tiles[1].userId == "bob");
    CHECK(tiles[1].videoOn);
    CHECK(tiles[1].speaking);
    CHECK_FALSE(tiles[1].micMuted);
    CHECK(tiles[2].key == "p-carol:camera");
    CHECK(tiles[2].micMuted);
    CHECK_FALSE(tiles[2].videoOn);
    CHECK(tiles[3].key == "p-carol:screen");
    CHECK(tiles[3].screen);
    CHECK(tiles[3].videoOn);
    CHECK(tiles[4].handle == "Joining...");

    // Pictures are handed on with the tile they belong to.
    lk::VideoFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.data.assign(6, 9);
    h.room->callbacks.onVideo("p-bob", lk::VideoSource::Camera, frame);
    h.room->callbacks.onVideo("p-carol", lk::VideoSource::Screen, frame);
    CHECK(h.frames.count("p-bob:camera") == 1);
    CHECK(h.frames.count("p-carol:screen") == 1);
}

TEST_CASE("the link is not posted if the starter never connects, and a failed post is reported", "[groupcall][session]") {
    {
        Harness h;
        h.connectFails = true;
        h.session->start(kGroup, "voice");
        h.pump();
        CHECK(h.announced.empty());
    }
    {
        Harness h;
        h.announceOk = false;
        h.session->start(kGroup, "voice");
        h.pump();
        CHECK(h.session->view().notice.find("Couldn't post the call link") != std::string::npos);
        CHECK(h.session->view().phase == "active");
    }
}

TEST_CASE("audio and video go to the room only while a call is up", "[groupcall][session]") {
    Harness h;
    std::int16_t frame[lk::kFrameSamples] = {};
    std::int16_t out[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    h.session->sendAudio(frame);  // no call: harmlessly ignored
    h.session->readAudio(out, 8);
    CHECK(out[0] == 0);

    h.session->start(kGroup, "voice");
    h.pump();
    h.session->sendAudio(frame);
    h.session->readAudio(out, 8);
    CHECK(h.room->audioFrames == 1);
    CHECK(out[0] == 42);
    h.session->leave();
    h.pump();
    h.session->readAudio(out, 8);
    CHECK(out[0] == 0);
}

// --- End-to-end encryption failures ---

namespace {

/// Starts a call and stops just before the key is handed out, so a test can decide what the room reports.
void startedAndConnected(Harness& h) {
    h.joinKind = "voice";
    h.session->start(kGroup, "voice");
    h.pump();
    REQUIRE(h.room != nullptr);
}

api::live::LiveEvent removed(const std::string& callId = kCall) {
    api::live::LiveEvent e;
    e.type = "group-removed";
    e.callId = callId;
    return e;
}

}  // namespace

TEST_CASE("nothing is published when frame encryption isn't on", "[groupcall][session][e2ee]") {
    Harness h;
    startedAndConnected(h);
    h.room->encrypting = false;
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();

    CHECK(h.did("setKey 1"));  // the key went in ...
    CHECK_FALSE(h.did("publishMicrophone"));  // ... and still nothing left
    CHECK_FALSE(h.did("publishCamera"));
    CHECK(h.session->view().phase == "ended");
    CHECK(h.session->view().endedMessage.find("encryption") != std::string::npos);
    CHECK(h.did("disconnect"));
    CHECK_FALSE(h.session->view().published);
}

TEST_CASE("control: with frame encryption on, publishing goes ahead", "[groupcall][session][e2ee]") {
    Harness h;
    startedAndConnected(h);
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();
    CHECK(h.did("publishMicrophone"));
    CHECK(h.session->view().published);
    CHECK(h.session->view().phase == "active");
    CHECK(h.session->view().notice.empty());
}

TEST_CASE("a room that throws while confirming encryption counts as not encrypted", "[groupcall][session][e2ee]") {
    Harness h;
    startedAndConnected(h);
    h.room->encryptionCheckThrows = true;
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();
    CHECK_FALSE(h.did("publishMicrophone"));
    CHECK(h.session->view().phase == "ended");
}

TEST_CASE("someone's frames failing to decrypt is shown, and clears when they recover", "[groupcall][session][e2ee]") {
    Harness h;
    startedAndConnected(h);
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}, {"bob", "p-bob"}}));
    h.pump();
    REQUIRE(h.session->view().notice.empty());

    h.room->callbacks.onEncryption("p-bob", lk::E2eeState::DecryptionFailed);
    h.pump();
    CHECK(h.session->view().notice.find("decrypted") != std::string::npos);
    CHECK(h.session->view().phase == "active");  // their problem doesn't end the call for everyone else

    h.room->callbacks.onEncryption("p-carol", lk::E2eeState::MissingKey);
    h.pump();
    h.room->callbacks.onEncryption("p-bob", lk::E2eeState::Ok);
    h.pump();
    CHECK_FALSE(h.session->view().notice.empty());  // carol's still broken
    h.room->callbacks.onEncryption("p-carol", lk::E2eeState::Ok);
    h.pump();
    CHECK(h.session->view().notice.empty());
}

TEST_CASE("encryption failing for what this device sends ends the call", "[groupcall][session][e2ee]") {
    const lk::E2eeState state = GENERATE(lk::E2eeState::EncryptionFailed, lk::E2eeState::InternalError);
    Harness h;
    startedAndConnected(h);
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();
    h.room->callbacks.onEncryption("p-self", state);
    h.pump();
    CHECK(h.session->view().phase == "ended");
    CHECK(h.session->view().endedMessage.find("encryption") != std::string::npos);
    CHECK(h.did("disconnect"));
    CHECK(h.did(std::string("api POST group-calls/") + kCall + "/leave"));
    CHECK_FALSE(h.session->inCall());
}

TEST_CASE("a late report from a call that's over is ignored", "[groupcall][session][e2ee]") {
    Harness h;
    startedAndConnected(h);
    auto report = h.room->callbacks.onEncryption;
    h.session->leave();
    h.pump();
    const std::string banner = h.session->view().endedMessage;
    report("p-bob", lk::E2eeState::DecryptionFailed);
    h.pump();
    CHECK(h.session->view().endedMessage == banner);
    CHECK(h.session->view().notice.empty());
}

// --- Removed from the group ---

TEST_CASE("being removed from the group leaves the call at once", "[groupcall][session][removed]") {
    Harness h;
    startedAndConnected(h);
    h.session->onLiveEvent(h.roster(1, "alice", {{"alice", "p-self"}}));
    h.pump();
    REQUIRE(h.session->inCall());

    h.session->onLiveEvent(removed());
    // At once: the media stops before anything else runs, not when the room finally drops us.
    CHECK_FALSE(h.session->inCall());
    CHECK(h.session->view().phase == "ended");
    CHECK(h.session->view().endedMessage.find("removed") != std::string::npos);
    std::int16_t pcm[lk::kFrameSamples] = {};
    const int before = h.room->audioFrames;
    h.session->sendAudio(pcm);
    CHECK(h.room->audioFrames == before);

    h.pump();
    CHECK(h.did("disconnect"));
    CHECK(h.session->active().empty());
}

TEST_CASE("removal also ends a call still connecting or ringing", "[groupcall][session][removed]") {
    SECTION("connecting") {
        Harness h;
        h.session->joinByLink(kCall);
        h.pump();
        REQUIRE(h.room != nullptr);
        h.session->onLiveEvent(removed());
        h.pump();
        CHECK_FALSE(h.session->inCall());
        CHECK_FALSE(h.did("publishMicrophone"));
    }
    SECTION("ringing") {
        Harness h;
        h.session->onLiveEvent(h.incoming());
        REQUIRE(h.session->view().phase == "incoming");
        h.session->onLiveEvent(removed());
        h.pump();
        CHECK_FALSE(h.session->view().present);  // it just stops ringing
    }
}

TEST_CASE("control: removal from some other call changes nothing", "[groupcall][session][removed]") {
    Harness h;
    startedAndConnected(h);
    h.session->onLiveEvent(removed("another-call"));
    h.pump();
    CHECK(h.session->inCall());
    CHECK(h.session->view().phase == "active");
    CHECK_FALSE(h.did("disconnect"));
}
