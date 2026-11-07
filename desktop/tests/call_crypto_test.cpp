#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "redvault/api/live_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

using namespace redvault;
using namespace redvault::crypto;
using namespace redvault::crypto::calls;
using Json = nlohmann::json;
using redvault::test::FakeTransport;

namespace {

// Produced by the web app's own code (ui/src/lib/callCrypto.ts, sas.ts, e2eCrypto.ts).
const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_CALL_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

Bytes b64(const Json& j) {
    return base64Decode(j.get<std::string>());
}

Expected offerFromAlice() {
    return Expected{"offer", "call-1", "conv-1", "alice", "bob", std::string("video")};
}

}  // namespace

TEST_CASE("opens a call offer the web app sealed, and only for the call it names", "[calls]") {
    const Json& v = vectors();
    const Sealed sealed{v["sealed"]["ciphertext"], v["sealed"]["nonce"]};
    const auto now = v["sentAt"].get<std::int64_t>();

    const CallSignal s = open(sealed, b64(v["alicePublic"]), b64(v["bobPrivate"]), offerFromAlice(), now);
    CHECK(s.sdp == v["sdp"].get<std::string>());
    CHECK(sdpFingerprint(s.sdp) == "sha-256 AB:CD:EF:01");

    // Another call, direction, kind or a stale replay is refused.
    auto other = offerFromAlice();
    other.callId = "call-2";
    CHECK_THROWS_AS(open(sealed, b64(v["alicePublic"]), b64(v["bobPrivate"]), other, now), CallSignalError);
    auto answer = offerFromAlice();
    answer.type = "answer";
    CHECK_THROWS_AS(open(sealed, b64(v["alicePublic"]), b64(v["bobPrivate"]), answer, now), CallSignalError);
    auto voice = offerFromAlice();
    voice.kind = "voice";
    CHECK_THROWS_AS(open(sealed, b64(v["alicePublic"]), b64(v["bobPrivate"]), voice, now), CallSignalError);
    CHECK_THROWS_AS(open(sealed, b64(v["alicePublic"]), b64(v["bobPrivate"]), offerFromAlice(), now + 10 * 60 * 1000), CallSignalError);

    // Sealed by anyone but Alice's key (a server swapping in its own offer): refused.
    const KeyPair mallory = generateKeyPair();
    CHECK_THROWS_AS(open(sealed, mallory.publicKey, b64(v["bobPrivate"]), offerFromAlice(), now), CallSignalError);
}

TEST_CASE("seals signals the other side (and the web app) can open", "[calls]") {
    const KeyPair alice = generateKeyPair();
    const KeyPair bob = generateKeyPair();
    CallSignal s{"answer", "call-9", "conv-9", "bob", "alice", "voice", "v=0\r\na=fingerprint:sha-256 01:02\r\n", nowMs()};
    const Sealed sealed = seal(s, alice.publicKey, bob.privateKey);
    const CallSignal opened = open(sealed, bob.publicKey, alice.privateKey, Expected{"answer", "call-9", "conv-9", "bob", "alice", std::nullopt});
    CHECK(opened.kind == "voice");

    s.sdp = "v=0\r\n";  // no fingerprint: nothing would pin the media to the key
    CHECK_THROWS_AS(open(seal(s, alice.publicKey, bob.privateKey), bob.publicKey, alice.privateKey,
                         Expected{"answer", "call-9", "conv-9", "bob", "alice", std::nullopt}),
                    CallSignalError);
}

TEST_CASE("the fingerprint line is found the way the regex found it", "[calls]") {
    CHECK(sdpFingerprint("a=fingerprint:sha-256 AB:cd:01\r\n") == "sha-256 AB:cd:01");
    CHECK(sdpFingerprint("v=0\na=fingerprint:sha-256 AB:CD\nm=audio") == "sha-256 AB:CD");
    // Only at the start of a line; a malformed line is skipped for a later good one.
    CHECK(sdpFingerprint("v=0\r\nx a=fingerprint:sha-256 AB\r\n").empty());
    CHECK(sdpFingerprint("a=fingerprint:sha-256\r\na=fingerprint:sha-1 0F\r\n") == "sha-1 0F");
    CHECK(sdpFingerprint("a=fingerprint:sha-256  AB\r\n").empty());  // two spaces
    CHECK(sdpFingerprint("a=fingerprint:sha-256 ZZ\r\n").empty());
    CHECK(sdpFingerprint("").empty());
}

TEST_CASE("a huge call setup from the other side neither crashes nor gets in", "[calls]") {
    // std::regex recursed once per character here: a 30 KB line overflowed an 8 MiB stack, 5 KB a 1 MiB one.
    const std::string longLine = "a=fingerprint:sha-256 " + std::string(200'000, 'F') + "\r\n";
    CHECK(sdpFingerprint(longLine).size() == 200'000 + 8);
    CHECK(sdpFingerprint("a=fingerprint:" + std::string(200'000, 'x')).empty());

    const KeyPair alice = generateKeyPair();
    const KeyPair bob = generateKeyPair();
    CallSignal s{"offer", "call-1", "conv-1", "alice", "bob", "voice", "v=0\r\n" + longLine, nowMs()};
    CHECK_THROWS_AS(open(seal(s, bob.publicKey, alice.privateKey), alice.publicKey, bob.privateKey,
                         Expected{"offer", "call-1", "conv-1", "alice", "bob", std::nullopt}),
                    CallSignalError);
}

TEST_CASE("call links: the web app's secret, name and sealed offer", "[calls]") {
    const Json& v = vectors();
    const auto secret = linkSecretFromFragment(v["linkSecretFragment"].get<std::string>());
    REQUIRE(secret);
    CHECK(linkSecretToFragment(*secret) == v["linkSecretFragment"].get<std::string>());
    CHECK(openLinkName(Sealed{v["linkName"]["ciphertext"], v["linkName"]["nonce"]}, *secret) == "Weekly design sync");

    const Expected expected{"offer", "call-1", "link:abcdefghijkl", "guest", "owner", std::nullopt};
    const auto now = v["sentAt"].get<std::int64_t>();
    CHECK(openWithLink(Sealed{v["linkSealed"]["ciphertext"], v["linkSealed"]["nonce"]}, *secret, expected, now).kind == "video");
    const SecretBytes wrong = newLinkSecret();
    CHECK_THROWS_AS(openWithLink(Sealed{v["linkSealed"]["ciphertext"], v["linkSealed"]["nonce"]}, wrong, expected, now), CallSignalError);

    CHECK_FALSE(linkSecretFromFragment("too-short"));
    CHECK(linkUrl("https://www.example.com/redvaultmessages/", "abcdefghijkl", *secret) ==
          "https://www.example.com/redvaultmessages/?join=abcdefghijkl#" + v["linkSecretFragment"].get<std::string>());

    // The owner keeps the secret sealed to their own key.
    const KeyPair owner = generateKeyPair();
    const SecretBytes opened = openOwnerSecret(sealSecretForOwner(*secret, owner.publicKey), owner.publicKey, owner.privateKey);
    CHECK(Bytes(opened.view().begin(), opened.view().end()) == Bytes(secret->view().begin(), secret->view().end()));
}

TEST_CASE("safety words match the web app's, whichever side computes them", "[calls]") {
    for (const auto& c : vectors()["sas"]) {
        const auto expected = c["words"].get<std::vector<std::string>>();
        CHECK(sasWords(c["a"].get<std::string>(), c["b"].get<std::string>()) == expected);
        CHECK(sasWords(c["b"].get<std::string>(), c["a"].get<std::string>()) == expected);
    }
}

TEST_CASE("keys for on-device data are derived like the web app's", "[calls]") {
    const Json& v = vectors();
    const Bytes priv = b64(v["alicePrivate"]);
    CHECK(toHex(deriveLocalKey(priv, "redvault-call-history-v1")) == v["callHistoryKeyHex"].get<std::string>());
    CHECK(toHex(deriveLocalKey(priv, "redvault-device-label-v1")) == v["deviceLabelKeyHex"].get<std::string>());
}

TEST_CASE("transparency log: checkpoint signatures and Merkle proofs from the server", "[calls]") {
    const Json& v = vectors();
    const auto& cp = v["checkpoint"];
    CHECK(checkpointSignatureValid(cp["treeSize"], cp["rootHash"].get<std::string>(), cp["signedAt"].get<std::string>(),
                                   cp["signature"].get<std::string>(), cp["publicKey"].get<std::string>()));
    CHECK_FALSE(checkpointSignatureValid(43, cp["rootHash"].get<std::string>(), cp["signedAt"].get<std::string>(),
                                         cp["signature"].get<std::string>(), cp["publicKey"].get<std::string>()));

    const auto& m = v["merkle"];
    auto hexes = [](const Json& list) {
        std::vector<Bytes> out;
        for (const auto& h : list) out.push_back(merkle::fromHex(h.get<std::string>()));
        return out;
    };
    const auto leaves = hexes(m["leaves"]);
    const Bytes root13 = merkle::fromHex(m["root13"].get<std::string>());
    CHECK(merkle::verifyInclusion(merkle::leafHash(leaves[5]), 5, 13, hexes(m["proof5in13"]), root13));
    CHECK(merkle::verifyInclusion(merkle::leafHash(leaves[12]), 12, 13, hexes(m["proof12in13"]), root13));
    CHECK_FALSE(merkle::verifyInclusion(merkle::leafHash(leaves[6]), 5, 13, hexes(m["proof5in13"]), root13));
    CHECK(merkle::verifyConsistency(7, 13, merkle::fromHex(m["root7"].get<std::string>()), root13, hexes(m["consistency7to13"])));
    CHECK(merkle::verifyConsistency(4, 13, merkle::fromHex(m["root4"].get<std::string>()), root13, hexes(m["consistency4to13"])));
    CHECK_FALSE(merkle::verifyConsistency(7, 13, merkle::fromHex(m["root4"].get<std::string>()), root13, hexes(m["consistency7to13"])));
}

TEST_CASE("live channel and call endpoints parse the server's shapes", "[calls][api]") {
    auto transport = std::make_shared<FakeTransport>();
    api::ApiClient client{"https://example.com/api", transport, std::make_shared<net::SessionCookieJar>()};

    transport->enqueue(200, R"({"cursor":7,"callsEnabled":true,"events":[
        {"type":"incoming","callId":"c1","conversationId":"conv","linkId":null,"callerId":"u2","kind":"video",
         "offer":{"ciphertext":"Y3Q=","nonce":"bm9uY2U="},"iceServers":[{"urls":["turn:t:3478"],"username":"u","credential":"p"}]},
        {"type":"typing","conversationId":"conv","userId":"u2"},
        {"type":"some-future-event"},
        {"type":"ended","callId":"c1","reason":"missed"}]})");
    const auto events = api::live::events(client, 3);
    CHECK(transport->last().url == "https://example.com/api/live/events?after=3");
    CHECK(events.cursor == 7);
    // This server predates group calls, so it doesn't say; the app treats that as off.
    CHECK(events.callsEnabled);
    CHECK_FALSE(events.groupCallsEnabled);
    REQUIRE(events.events.size() == 4);
    CHECK(events.events[0].sealed->ciphertext == "Y3Q=");
    CHECK(events.events[0].iceServers.at(0).urls.at(0) == "turn:t:3478");
    CHECK(events.events[1].userId == "u2");
    CHECK(events.events[3].reason == "missed");

    transport->enqueue(200, R"({"cursor":8,"callsEnabled":true,"groupCallsEnabled":true,"events":[]})");
    CHECK(api::live::events(client, 7).groupCallsEnabled);

    transport->enqueue(201, R"({"callId":"c2","calleeId":"u3","iceServers":[]})");
    CHECK(api::live::startCall(client, "conv", "voice").callId == "c2");
    CHECK(Json::parse(transport->last().body) == Json{{"conversationId", "conv"}, {"kind", "voice"}});

    transport->enqueue(200, R"({"checkpoint":null})");
    CHECK_FALSE(api::live::latestCheckpoint(client));
    transport->enqueue(200, R"([])");
    api::live::logEntries(client, std::nullopt, 50, "#12 ab");
    CHECK(transport->last().url == "https://example.com/api/public/transparency-log/entries?limit=50&q=%2312%20ab");

    transport->enqueue(200, R"([{"id":"s1","current":true,"awaitingTwoFactor":false,"expiresOn":"2026-10-28","label":null}])");
    const auto sessions = api::live::listSessions(client);
    CHECK(sessions.at(0).current);
    CHECK_FALSE(sessions.at(0).label);
}
