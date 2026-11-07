#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <sodium.h>

#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/group_call_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/crypto/group_call_crypto.hpp"
#include "redvault/model/group_call_keys.hpp"
#include "redvault/model/group_call_link.hpp"

using namespace redvault;
using namespace redvault::crypto;
using namespace redvault::crypto::groupcalls;
using Json = nlohmann::json;
using redvault::test::FakeTransport;
namespace gcm = redvault::model::group_calls;

namespace {

// Produced by the web app's own code (ui/src/lib/groupCallCrypto.ts, sas.ts):
// see tests/vectors/make_group_call_vectors.ts.
const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_GROUP_CALL_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

Bytes b64(const Json& j) {
    return base64Decode(j.get<std::string>());
}

KeyMessage messageOf(const Json& j) {
    return KeyMessage{j["callId"], j["conversationId"], j["epoch"].get<std::int64_t>(), j["from"], j["to"]};
}

Sealed sealedOf(const Json& j) {
    return Sealed{j["ciphertext"], j["nonce"]};
}

}  // namespace

TEST_CASE("opens a room key the web app sealed, and only exactly the one expected", "[calls][group]") {
    const Json& v = vectors();
    const KeyMessage message = messageOf(v["message"]);
    const std::int64_t now = v["sentAt"].get<std::int64_t>();
    const Bytes alicePublic = b64(v["alicePublic"]);
    const Bytes bobPrivate = b64(v["bobPrivate"]);

    const SecretBytes key = openRoomKey(sealedOf(v["sealed"]), alicePublic, bobPrivate, message, now);
    CHECK(Bytes(key.data(), key.data() + key.size()) == b64(v["roomKey"]));

    // Sealed with someone else's key: not from the keyholder's identity key.
    CHECK_THROWS_AS(openRoomKey(sealedOf(v["sealedByMallory"]), alicePublic, bobPrivate, message, now), GroupKeyError);

    // Replayed into another epoch, call or group, or meant for or sent by someone else.
    auto vary = [&](auto&& change) {
        KeyMessage other = message;
        change(other);
        return other;
    };
    for (const KeyMessage& other : {vary([](KeyMessage& m) { m.epoch += 1; }), vary([](KeyMessage& m) { m.callId = "call-2"; }),
                                    vary([](KeyMessage& m) { m.conversationId = "group-2"; }), vary([](KeyMessage& m) { m.to = "carol"; }),
                                    vary([](KeyMessage& m) { m.from = "mallory"; })}) {
        CHECK_THROWS_WITH(openRoomKey(sealedOf(v["sealed"]), alicePublic, bobPrivate, other, now), "The call key didn't match this call.");
    }

    // The five-minute window, both ways.
    CHECK_NOTHROW(openRoomKey(sealedOf(v["sealed"]), alicePublic, bobPrivate, message, now + kMaxKeyAgeMs));
    CHECK_THROWS_WITH(openRoomKey(sealedOf(v["sealed"]), alicePublic, bobPrivate, message, now + kMaxKeyAgeMs + 1), "The call key is too old.");
    CHECK_THROWS_WITH(openRoomKey(sealedOf(v["sealed"]), alicePublic, bobPrivate, message, now - kMaxKeyAgeMs - 1), "The call key is too old.");

    // Garbage, and a key that isn't ours to open.
    CHECK_THROWS_AS(openRoomKey(Sealed{"not base64!", "x"}, alicePublic, bobPrivate, message, now), GroupKeyError);
    CHECK_THROWS_AS(openRoomKey(sealedOf(v["sealed"]), alicePublic, b64(v["alicePrivate"]), message, now), GroupKeyError);
}

TEST_CASE("seals a room key the web app can open: same fields, same checks", "[calls][group]") {
    const Json& v = vectors();
    const KeyMessage message = messageOf(v["message"]);
    const SecretBytes roomKey = newRoomKey();
    CHECK(roomKey.size() == kRoomKeyBytes);

    const Sealed sealed = sealRoomKey(message, roomKey, b64(v["bobPublic"]), b64(v["alicePrivate"]), 1790000000000);
    const SecretBytes opened = openRoomKey(sealed, b64(v["alicePublic"]), b64(v["bobPrivate"]), message, 1790000000000);
    CHECK(Bytes(opened.data(), opened.data() + opened.size()) == Bytes(roomKey.data(), roomKey.data() + roomKey.size()));

    // The plaintext is the JSON the web app's openRoomKey reads: v, type, the five fields, key, sentAt.
    const Bytes cipher = base64Decode(sealed.ciphertext);
    const Bytes nonce = base64Decode(sealed.nonce);
    ensureSodium();
    std::string plain(cipher.size() - 16, '\0');
    const Bytes alicePublic = b64(v["alicePublic"]);
    const Bytes bobPrivate = b64(v["bobPrivate"]);
    REQUIRE(crypto_box_open_easy(reinterpret_cast<unsigned char*>(plain.data()), cipher.data(), cipher.size(), nonce.data(), alicePublic.data(),
                                 bobPrivate.data()) == 0);
    const Json j = Json::parse(plain);
    CHECK(j["v"] == 1);
    CHECK(j["type"] == "group-call-key");
    CHECK(j["callId"] == message.callId);
    CHECK(j["conversationId"] == message.conversationId);
    CHECK(j["epoch"] == 19);
    CHECK(j["from"] == message.from);
    CHECK(j["to"] == message.to);
    CHECK(j["key"] == base64Encode(roomKey));
    CHECK(j["sentAt"] == 1790000000000);
    CHECK(j.size() == 9);
}

TEST_CASE("an epoch goes in key slot epoch mod 16", "[calls][group]") {
    CHECK(keySlot(1) == 1);
    CHECK(keySlot(15) == 15);
    CHECK(keySlot(16) == 0);
    CHECK(keySlot(19) == 3);
    CHECK(keySlot(35) == 3);
}

TEST_CASE("group safety words match the web app's, in any order, and change with a swapped key", "[calls][group]") {
    const Json& v = vectors();
    std::vector<Participant> people;
    for (const auto& p : v["sasParticipants"]) people.push_back(Participant{p["userId"], b64(p["publicKey"])});
    const std::string callId = v["sasCallId"];
    const auto words = [](const Json& j) {
        std::vector<std::string> out;
        for (const auto& w : j) out.push_back(w);
        return out;
    };

    const auto ours = groupSasWords(callId, people);
    CHECK(ours.size() == 4);
    CHECK(ours == words(v["sasWords"]));

    std::vector<Participant> reversed(people.rbegin(), people.rend());
    CHECK(groupSasWords(callId, reversed) == ours);

    CHECK(groupSasWords("00000000-0000-4000-8000-000000000000", people) == words(v["sasWordsOtherCall"]));
    CHECK(groupSasWords("00000000-0000-4000-8000-000000000000", people) != ours);

    std::vector<Participant> swapped = people;
    swapped[1].publicKey = b64(v["malloryPublic"]);
    CHECK(groupSasWords(callId, swapped) == words(v["sasWordsSwappedKey"]));
    CHECK(groupSasWords(callId, swapped) != ours);
}

// ---------------------------------------------------------------------------------------------------------------------

namespace {

constexpr const char* kCall = "3f2b8c1e-9a4d-4e6f-8b7a-1c2d3e4f5a6b";
constexpr const char* kGroup = "0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d";

/// Three people with real keypairs, and one RoomKeyExchange per person.
struct Party {
    std::string userId;
    KeyPair keys;
};

struct Fixture {
    Party alice{"alice", generateKeyPair()};
    Party bob{"bob", generateKeyPair()};
    Party carol{"carol", generateKeyPair()};
    std::int64_t now = 1790000000000;

    std::map<std::string, Bytes> directory() const {
        return {{alice.userId, alice.keys.publicKey}, {bob.userId, bob.keys.publicKey}, {carol.userId, carol.keys.publicKey}};
    }

    model::group_calls::RoomKeyExchange exchange(const Party& who, bool knowEveryone = true) {
        auto dir = directory();
        if (!knowEveryone) dir.erase("carol");
        model::group_calls::RoomKeyExchange::Config config;
        config.callId = kCall;
        config.conversationId = kGroup;
        config.selfUserId = who.userId;
        config.selfPrivateKey = who.keys.privateKey.view();
        config.publicKeyOf = [dir](const std::string& id) -> std::optional<Bytes> {
            const auto it = dir.find(id);
            if (it == dir.end()) return std::nullopt;
            return it->second;
        };
        config.now = [this] { return now; };
        return model::group_calls::RoomKeyExchange(std::move(config));
    }
};

using model::group_calls::RosterMember;

std::vector<RosterMember> roster(std::initializer_list<const char*> users) {
    std::vector<RosterMember> out;
    for (const char* u : users) out.push_back({u, std::string("p-") + u});
    return out;
}

Bytes bytesOf(const SecretBytes& s) {
    return Bytes(s.data(), s.data() + s.size());
}

}  // namespace

TEST_CASE("the keyholder makes a key for each epoch and seals it to everyone else", "[calls][group]") {
    Fixture f;
    auto alice = f.exchange(f.alice);
    const auto out = alice.onRoster(1, "alice", roster({"alice", "bob", "carol"}));

    REQUIRE(out.install);
    CHECK(out.install->epoch == 1);
    CHECK(out.install->key.size() == kRoomKeyBytes);
    CHECK(alice.keyReady());
    CHECK(alice.holdsKey());
    CHECK(out.sendEpoch == 1);
    REQUIRE(out.toSend.size() == 2);
    CHECK(out.toSend[0].userId == "bob");
    CHECK(out.toSend[1].userId == "carol");

    // Each recipient opens their copy as exactly the keyholder's key for that epoch.
    const KeyMessage forBob{kCall, kGroup, 1, "alice", "bob"};
    const SecretBytes opened = openRoomKey(Sealed{out.toSend[0].key.ciphertext, out.toSend[0].key.nonce}, f.alice.keys.publicKey, f.bob.keys.privateKey, forBob, f.now);
    CHECK(bytesOf(opened) == bytesOf(out.install->key));
    // ...and it can't be opened as the copy meant for carol.
    const KeyMessage forCarol{kCall, kGroup, 1, "alice", "carol"};
    CHECK_THROWS_AS(openRoomKey(Sealed{out.toSend[0].key.ciphertext, out.toSend[0].key.nonce}, f.alice.keys.publicKey, f.bob.keys.privateKey, forCarol, f.now), GroupKeyError);

    // A new epoch means a fresh key.
    const auto next = alice.onRoster(2, "alice", roster({"alice", "bob"}));
    REQUIRE(next.install);
    CHECK(bytesOf(next.install->key) != bytesOf(out.install->key));
    CHECK(next.toSend.size() == 1);
    CHECK(alice.epoch() == 2);
}

TEST_CASE("a participant opens the keyholder's key, and only for the current epoch", "[calls][group]") {
    Fixture f;
    auto alice = f.exchange(f.alice);
    auto bob = f.exchange(f.bob);
    const auto members = roster({"alice", "bob"});
    const auto sent = alice.onRoster(1, "alice", members);

    const auto joined = bob.onRoster(1, "alice", members);
    CHECK_FALSE(joined.install);
    CHECK_FALSE(bob.keyReady());
    CHECK(joined.sas);
    CHECK(joined.sas->size() == 4);

    const auto got = bob.onKey(1, "alice", Sealed{sent.toSend.at(0).key.ciphertext, sent.toSend.at(0).key.nonce});
    REQUIRE(got.install);
    CHECK(got.clearNotice);
    CHECK(bytesOf(got.install->key) == bytesOf(sent.install->key));
    CHECK(bob.keyReady());

    // Both see the same safety words.
    CHECK(*joined.sas == *alice.onRoster(2, "alice", members).sas);
}

TEST_CASE("a key that arrives before its roster is kept until the roster does", "[calls][group]") {
    Fixture f;
    auto alice = f.exchange(f.alice);
    auto bob = f.exchange(f.bob);
    const auto members = roster({"alice", "bob"});
    const auto sent = alice.onRoster(1, "alice", members);
    const Sealed sealed{sent.toSend.at(0).key.ciphertext, sent.toSend.at(0).key.nonce};

    const auto early = bob.onKey(1, "alice", sealed);
    CHECK_FALSE(early.install);
    CHECK_FALSE(early.notice);
    CHECK_FALSE(bob.holdsKey());

    const auto out = bob.onRoster(1, "alice", members);
    REQUIRE(out.install);
    CHECK(bytesOf(out.install->key) == bytesOf(sent.install->key));
    CHECK(bob.keyReady());
}

TEST_CASE("keys that are stale, from the wrong person or unverifiable are not used", "[calls][group]") {
    Fixture f;
    auto alice = f.exchange(f.alice);
    auto bob = f.exchange(f.bob);
    const auto members = roster({"alice", "bob"});
    const auto sent = alice.onRoster(1, "alice", members);
    const Sealed sealed{sent.toSend.at(0).key.ciphertext, sent.toSend.at(0).key.nonce};
    bob.onRoster(1, "alice", members);

    // From someone who isn't the keyholder.
    CHECK_FALSE(bob.onKey(1, "carol", sealed).install);
    CHECK_FALSE(bob.holdsKey());

    // The epoch moves on: the old key is for an epoch that's gone.
    bob.onRoster(2, "alice", roster({"alice", "bob", "carol"}));
    CHECK_FALSE(bob.onKey(1, "alice", sealed).install);
    CHECK_FALSE(bob.holdsKey());

    // Right epoch and sender, but sealed by someone else's key: a notice, no key.
    auto mallory = generateKeyPair();
    const KeyMessage forged{kCall, kGroup, 2, "alice", "bob"};
    const Sealed fake = sealRoomKey(forged, newRoomKey(), f.bob.keys.publicKey, mallory.privateKey, f.now);
    const auto bad = bob.onKey(2, "alice", fake);
    CHECK_FALSE(bad.install);
    REQUIRE(bad.notice);
    CHECK(bad.notice->find("couldn't be verified") != std::string::npos);
    CHECK_FALSE(bob.holdsKey());

    // A stale timestamp.
    f.now += kMaxKeyAgeMs + 1000;
    const KeyMessage real{kCall, kGroup, 2, "alice", "bob"};
    const Sealed old = sealRoomKey(real, newRoomKey(), f.bob.keys.publicKey, f.alice.keys.privateKey, f.now - kMaxKeyAgeMs - 1000);
    const auto stale = bob.onKey(2, "alice", old);
    CHECK_FALSE(stale.install);
    REQUIRE(stale.notice);
    CHECK(*stale.notice == "The call key is too old.");
}

TEST_CASE("an unknown identity key means no safety words and no key for that person", "[calls][group]") {
    Fixture f;
    // Alice doesn't know carol's key (not in her member list yet).
    auto alice = f.exchange(f.alice, false);
    const auto out = alice.onRoster(1, "alice", roster({"alice", "bob", "carol"}));
    REQUIRE(out.sas);
    CHECK(out.sas->empty());
    REQUIRE(out.toSend.size() == 1);
    CHECK(out.toSend[0].userId == "bob");

    // A key from a keyholder this app has no identity key for is refused with a notice.
    auto bob = f.exchange(f.bob, false);
    bob.onRoster(1, "carol", roster({"carol", "bob"}));
    const auto refused = bob.onKey(1, "carol", Sealed{"AAAA", "AAAA"});
    CHECK_FALSE(refused.install);
    REQUIRE(refused.notice);
}

TEST_CASE("an old roster is ignored", "[calls][group]") {
    Fixture f;
    auto bob = f.exchange(f.bob);
    bob.onRoster(5, "alice", roster({"alice", "bob"}));
    const auto stale = bob.onRoster(4, "alice", roster({"alice", "bob", "carol"}));
    CHECK_FALSE(stale.rosterChanged);
    CHECK(bob.epoch() == 5);
    CHECK(bob.members().size() == 2);
    CHECK(bob.userOfIdentity("p-alice") == "alice");
    CHECK_FALSE(bob.userOfIdentity("p-carol"));
}

// ---------------------------------------------------------------------------------------------------------------------

TEST_CASE("group call endpoints send and parse the server's shapes", "[calls][group][api]") {
    auto transport = std::make_shared<FakeTransport>();
    api::ApiClient client{"https://example.com/api", transport, std::make_shared<net::SessionCookieJar>()};

    transport->enqueue(201, R"({"callId":"c1","conversationId":"g1","kind":"video","url":"/redvaultmessages/livekit","token":"jwt","identity":"p-abc","created":true})");
    const auto started = api::groupcalls::start(client, "g1", "video");
    CHECK(transport->last().url == "https://example.com/api/group-calls");
    CHECK(Json::parse(transport->last().body) == Json{{"conversationId", "g1"}, {"kind", "video"}});
    CHECK(started.callId == "c1");
    CHECK(started.kind == "video");
    CHECK(started.url == "/redvaultmessages/livekit");
    CHECK(started.token == "jwt");
    CHECK(started.identity == "p-abc");
    CHECK(started.created);

    // An older server doesn't say `created`: that's a join.
    transport->enqueue(200, R"({"callId":"c1","conversationId":"g1","kind":"video","url":"wss://x/y","token":"t","identity":"p-d"})");
    const auto joined = api::groupcalls::join(client, "c1");
    CHECK(transport->last().url == "https://example.com/api/group-calls/c1/join");
    CHECK_FALSE(joined.created);

    transport->enqueue(200, R"({"calls":[{"callId":"c1","conversationId":"g1","kind":"voice","participants":3}]})");
    const auto active = api::groupcalls::active(client);
    CHECK(transport->last().url == "https://example.com/api/group-calls/active");
    REQUIRE(active.size() == 1);
    CHECK(active[0].participants == 3);

    transport->enqueue(204, "");
    api::groupcalls::decline(client, "c1");
    CHECK(transport->last().url == "https://example.com/api/group-calls/c1/decline");
    transport->enqueue(204, "");
    api::groupcalls::leave(client, "c1");
    CHECK(transport->last().url == "https://example.com/api/group-calls/c1/leave");

    transport->enqueue(204, "");
    api::groupcalls::sendKeys(client, "c1", 4, {{"u2", {"Y3Q=", "bm9uY2U="}}});
    CHECK(transport->last().url == "https://example.com/api/group-calls/c1/keys");
    CHECK(Json::parse(transport->last().body) ==
          Json{{"epoch", 4}, {"keys", Json::array({Json{{"userId", "u2"}, {"key", Json{{"ciphertext", "Y3Q="}, {"nonce", "bm9uY2U="}}}}})}});

    // A shape the app doesn't understand is a malformed response, not a crash.
    transport->enqueue(200, R"({"callId":"c1"})");
    try {
        api::groupcalls::join(client, "c1");
        FAIL("expected an error");
    } catch (const api::ApiError& e) {
        CHECK(e.code() == "malformed_response");
    }

    // The server's refusals reach the caller with their codes.
    transport->enqueue(409, R"({"error":"stale_epoch","message":"The call changed; send the key for the current epoch."})");
    try {
        api::groupcalls::sendKeys(client, "c1", 3, {});
        FAIL("expected an error");
    } catch (const api::ApiError& e) {
        CHECK(e.status() == 409);
        CHECK(e.code() == "stale_epoch");
    }
}

TEST_CASE("group call events on the live channel keep their roster and key", "[calls][group][api]") {
    auto transport = std::make_shared<FakeTransport>();
    api::ApiClient client{"https://example.com/api", transport, std::make_shared<net::SessionCookieJar>()};
    transport->enqueue(200, R"({"cursor":9,"callsEnabled":true,"groupCallsEnabled":true,"events":[
        {"type":"group-incoming","callId":"c1","conversationId":"g1","hostId":"u1","kind":"video"},
        {"type":"group-roster","callId":"c1","conversationId":"g1","epoch":4,"keyholder":"u1",
         "members":[{"userId":"u1","identity":"p-a"},{"userId":"u2","identity":"p-b"}]},
        {"type":"group-key","callId":"c1","epoch":4,"from":"u1","key":{"ciphertext":"Y3Q=","nonce":"bm9uY2U="}},
        {"type":"group-ring-stop","callId":"c1"},
        {"type":"group-ended","callId":"c1","conversationId":"g1"}]})");
    const auto events = api::live::events(client, 8);
    REQUIRE(events.events.size() == 5);
    CHECK(events.events[0].type == "group-incoming");
    CHECK(events.events[0].hostId == "u1");
    CHECK(events.events[0].kind == "video");
    CHECK(events.events[1].epoch == 4);
    CHECK(events.events[1].keyholder == "u1");
    REQUIRE(events.events[1].members.size() == 2);
    CHECK(events.events[1].members[1].identity == "p-b");
    CHECK(events.events[2].from == "u1");
    REQUIRE(events.events[2].sealed);
    CHECK(events.events[2].sealed->ciphertext == "Y3Q=");
    CHECK(events.events[3].type == "group-ring-stop");
    CHECK(events.events[4].conversationId == "g1");
}

TEST_CASE("the signaling address: LiveKit's path on the site becomes a websocket URL", "[calls][group]") {
    const std::string site = "https://www.redvaultsystems.com/redvaultmessages/";
    CHECK(api::groupcalls::signalingUrl("/redvaultmessages/livekit", site) == "wss://www.redvaultsystems.com/redvaultmessages/livekit");
    CHECK(api::groupcalls::signalingUrl("/livekit", "http://localhost:5173/redvaultmessages/") == "ws://localhost:5173/livekit");
    CHECK(api::groupcalls::signalingUrl("wss://livekit.example.com", site) == "wss://livekit.example.com");
    CHECK(api::groupcalls::signalingUrl("", site).empty());
    CHECK(api::groupcalls::signalingUrl("/x", "not a url").empty());
}

TEST_CASE("the invite posted in the chat is the web app's text, word for word", "[calls][group]") {
    const std::string url = gcm::joinUrl("https://www.redvaultsystems.com/redvaultmessages/", kCall);
    // ui/src/lib/groupCallLink.ts groupCallInviteText: U+1F4DE, then the sentence, then the link on its own line.
    const std::string expected = std::string("\xF0\x9F\x93\x9E") +
                                 " Group video call started. To join, open Calls, tap \"Join with a link\" and paste this link:\n" + url;
    CHECK(gcm::inviteText("video", url) == expected);
    CHECK(gcm::inviteText("voice", url).find("Group voice call started.") != std::string::npos);
    // Joining from the message we posted ourselves works the same as from the web app's.
    CHECK(gcm::parseCallLink(gcm::inviteText("video", url)) == std::string(kCall));
}
