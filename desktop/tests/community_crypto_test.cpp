// Community epoch keys: pinned to the web app with vectors its own code
// produced (vectors/make_community_vectors.ts). Sealed copies are randomized,
// so the desktop opens the web's copies (and refuses the bad ones), and the
// bound payload is compared byte for byte; a copy sealed here opens with the
// same checks the web app makes.
#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include <nlohmann/json.hpp>

#include "test_helpers.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/community_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

using namespace redvault;
using namespace redvault::crypto::community;
using redvault::crypto::Bytes;
using redvault::crypto::SecretBytes;
using redvault::test::b64;
using redvault::test::copy;
using Json = nlohmann::json;

namespace {

const Json& vectors() {
    static const Json v = [] {
        std::ifstream in(std::string(REDVAULT_COMMUNITY_VECTORS));
        REQUIRE(in.good());
        return Json::parse(in);
    }();
    return v;
}

struct Identity {
    Bytes publicKey;
    Bytes privateKey;
};

Identity me() {
    return {b64(vectors()["me"]["publicKey"].get<std::string>()), b64(vectors()["me"]["privateKey"].get<std::string>())};
}

KeyState stateFrom(const Json& j) {
    KeyState state;
    state.conversationId = j["conversationId"].get<std::string>();
    state.keyEpoch = j["keyEpoch"].get<std::int64_t>();
    state.rekeyRequired = j["rekeyRequired"].get<bool>();
    for (const auto& k : j["keys"]) state.keys.push_back({k["epoch"].get<std::int64_t>(), k["wrappedKey"].get<std::string>()});
    return state;
}

KeyRing webRing() {
    const Identity id = me();
    return buildKeyRing(vectors()["communityId"].get<std::string>(), stateFrom(vectors()["ring"]["state"]), id.publicKey, id.privateKey);
}

}  // namespace

TEST_CASE("binds a key to its epoch and community byte for byte as the web app does", "[community]") {
    for (const auto& p : vectors()["payloads"]) {
        const SecretBytes payload = epochKeyPayload(b64(p["key"].get<std::string>()), p["epoch"].get<std::int64_t>(),
                                                    p["communityId"].get<std::string>());
        CHECK(crypto::toHex(payload.view()) == p["payloadHex"].get<std::string>());
    }
}

TEST_CASE("refuses to bind what the web app refuses", "[community]") {
    for (const auto& bad : vectors()["badPayloads"]) {
        INFO(bad["name"].get<std::string>());
        CHECK_THROWS_AS(epochKeyPayload(b64(bad["key"].get<std::string>()), bad["epoch"].get<std::int64_t>(),
                                        bad["communityId"].get<std::string>()),
                        EpochKeyError);
    }
}

TEST_CASE("opens the web app's sealed copies", "[community]") {
    const Identity id = me();
    for (const auto& o : vectors()["opens"]) {
        const SecretBytes key = openEpochKey(b64(o["wrappedKey"].get<std::string>()), o["epoch"].get<std::int64_t>(),
                                             o["communityId"].get<std::string>(), id.publicKey, id.privateKey);
        CHECK(copy(key) == b64(o["key"].get<std::string>()));
    }
}

TEST_CASE("refuses copies for another epoch or community, someone else's, and anything that isn't a v1 community key", "[community]") {
    const Identity id = me();
    for (const auto& r : vectors()["refused"]) {
        INFO(r["name"].get<std::string>());
        CHECK_THROWS_AS(openEpochKey(b64(r["wrappedKey"].get<std::string>()), r["epoch"].get<std::int64_t>(),
                                     r["communityId"].get<std::string>(), id.publicKey, id.privateKey),
                        EpochKeyError);
    }
}

TEST_CASE("builds the same key ring as the web app: reused, foreign and mislabelled copies are rejected", "[community]") {
    const KeyRing ring = webRing();
    const Json& expected = vectors()["ring"]["expected"];
    CHECK(ring.currentEpoch == expected["currentEpoch"].get<std::int64_t>());
    CHECK(ring.rekeyRequired == expected["rekeyRequired"].get<bool>());
    CHECK(ring.conversationId == vectors()["ring"]["state"]["conversationId"].get<std::string>());
    REQUIRE(ring.keys.size() == expected["keys"].size());
    for (const auto& [epoch, key] : expected["keys"].items()) {
        const SecretBytes* held = ring.key(std::stoll(epoch));
        REQUIRE(held != nullptr);
        CHECK(copy(*held) == b64(key.get<std::string>()));
    }
    CHECK(ring.rejected == expected["rejected"].get<std::vector<std::int64_t>>());
    CHECK(ring.hasCurrentKey());
}

TEST_CASE("reads the web app's posts by epoch, and nothing under the wrong or a missing epoch", "[community]") {
    const KeyRing ring = webRing();
    for (const auto& m : vectors()["messages"]) {
        const auto text = decryptForCommunity(ring, m["epoch"].get<std::int64_t>(), b64(m["ciphertext"].get<std::string>()),
                                              b64(m["nonce"].get<std::string>()));
        if (m["plaintext"].is_null()) {
            CHECK_FALSE(text.has_value());
        } else {
            REQUIRE(text.has_value());
            CHECK(*text == m["plaintext"].get<std::string>());
        }
    }
}

TEST_CASE("posts under the current epoch, readable with the web app's key for it", "[community]") {
    const KeyRing ring = webRing();
    const auto sealed = encryptForCommunity(ring, "from the desktop");
    CHECK(sealed.keyEpoch == 3);
    const Bytes webKey = b64(vectors()["ring"]["expected"]["keys"]["3"].get<std::string>());
    CHECK(crypto::decryptWithConversationKey(sealed.body.ciphertext, sealed.body.nonce, webKey) == "from the desktop");

    KeyRing without = webRing();
    without.currentEpoch = 9;  // the community moved on and this member hasn't got the key yet
    CHECK_THROWS_AS(encryptForCommunity(without, "x"), EpochKeyError);
}

TEST_CASE("opens the web app's rotation, enrollment and new community key", "[community]") {
    const Identity id = me();
    const Json& v = vectors();
    const std::string communityId = v["communityId"].get<std::string>();
    const KeyRing ring = webRing();

    const auto fromEpoch = v["rotation"]["fromEpoch"].get<std::int64_t>();
    const SecretBytes rotated = openEpochKey(b64(v["rotation"]["mine"].get<std::string>()), fromEpoch + 1, communityId, id.publicKey, id.privateKey);
    CHECK(copy(rotated) == b64(v["rotation"]["key"].get<std::string>()));
    for (const auto& [epoch, key] : ring.keys) CHECK_FALSE(crypto::constantTimeEquals(key.view(), rotated.view()));

    const SecretBytes enrolled = openEpochKey(b64(v["enrollment"]["wrappedKey"].get<std::string>()),
                                              v["enrollment"]["epoch"].get<std::int64_t>(), communityId, id.publicKey, id.privateKey);
    CHECK(copy(enrolled) == copy(*ring.key(ring.currentEpoch)));

    const SecretBytes created = openEpochKey(b64(v["newCommunityKey"]["wrappedKey"].get<std::string>()), 0, communityId, id.publicKey, id.privateKey);
    CHECK(copy(created) == b64(v["newCommunityKey"]["key"].get<std::string>()));
}

TEST_CASE("a rotation seals a fresh key for the next epoch to exactly the roster", "[community]") {
    const Identity id = me();
    const Json& v = vectors();
    const std::string communityId = v["communityId"].get<std::string>();
    const KeyRing ring = webRing();
    const auto other = crypto::generateKeyPair();
    const std::vector<Member> roster{{"me", v["me"]["publicKey"].get<std::string>()}, {"them", crypto::base64Encode(other.publicKey)}};

    const Rotation rotation = prepareRotation(ring, 3, roster);
    CHECK(rotation.fromEpoch == 3);
    REQUIRE(rotation.keys.size() == 2);
    CHECK(rotation.keys[0].userId == "me");
    CHECK(rotation.keys[1].userId == "them");
    for (const auto& [epoch, key] : ring.keys) CHECK_FALSE(crypto::constantTimeEquals(key.view(), rotation.key.view()));

    // Each copy opens only as epoch 4 of this community, and for its own member.
    const SecretBytes mine = openEpochKey(b64(rotation.keys[0].wrappedKey), 4, communityId, id.publicKey, id.privateKey);
    CHECK(copy(mine) == copy(rotation.key));
    const SecretBytes theirs = openEpochKey(b64(rotation.keys[1].wrappedKey), 4, communityId, other.publicKey, other.privateKey);
    CHECK(copy(theirs) == copy(rotation.key));
    CHECK_THROWS_AS(openEpochKey(b64(rotation.keys[0].wrappedKey), 3, communityId, id.publicKey, id.privateKey), EpochKeyError);
    CHECK_THROWS_AS(openEpochKey(b64(rotation.keys[0].wrappedKey), 4, v["otherCommunityId"].get<std::string>(), id.publicKey, id.privateKey),
                    EpochKeyError);
    CHECK_THROWS_AS(openEpochKey(b64(rotation.keys[1].wrappedKey), 4, communityId, id.publicKey, id.privateKey), EpochKeyError);

    CHECK_THROWS_AS(prepareRotation(ring, kMaxEpoch, roster), EpochKeyError);
}

TEST_CASE("enrollments carry the current key, and nothing while a rekey is due or the key isn't held", "[community]") {
    const Identity id = me();
    const Json& v = vectors();
    const std::string communityId = v["communityId"].get<std::string>();
    const std::vector<Member> waiting{{"me", v["me"]["publicKey"].get<std::string>()}};

    KeyRing ring = webRing();
    const auto enrollments = prepareEnrollments(ring, waiting);
    REQUIRE(enrollments.size() == 1);
    CHECK(enrollments[0].userId == "me");
    CHECK(enrollments[0].epoch == 3);
    CHECK(copy(openEpochKey(b64(enrollments[0].wrappedKey), 3, communityId, id.publicKey, id.privateKey)) == copy(*ring.key(3)));

    ring.rekeyRequired = true;
    CHECK(prepareEnrollments(ring, waiting).empty());
    ring.rekeyRequired = false;
    ring.currentEpoch = 9;
    CHECK(prepareEnrollments(ring, waiting).empty());
}

TEST_CASE("a ring refuses a replayed old copy presented as a new epoch", "[community]") {
    const Identity id = me();
    const std::string communityId = vectors()["communityId"].get<std::string>();
    const auto created = newCommunityKey(communityId, id.publicKey);
    // The server can't relabel epoch 0's copy as epoch 1 (the payload names epoch 0)...
    KeyState relabelled{"c", 1, false, {{0, created.wrappedKey}, {1, created.wrappedKey}}};
    KeyRing ring = buildKeyRing(communityId, relabelled, id.publicKey, id.privateKey);
    CHECK(ring.keys.size() == 1);
    CHECK(ring.rejected == std::vector<std::int64_t>{1});
    CHECK_FALSE(ring.hasCurrentKey());
    // ...nor get a rotator's copy of the same key accepted for a later epoch.
    KeyState reused{"c", 1, false, {{0, created.wrappedKey}, {1, crypto::base64Encode(sealEpochKey(created.key, 1, communityId, id.publicKey))}}};
    ring = buildKeyRing(communityId, reused, id.publicKey, id.privateKey);
    CHECK(ring.rejected == std::vector<std::int64_t>{1});
}
