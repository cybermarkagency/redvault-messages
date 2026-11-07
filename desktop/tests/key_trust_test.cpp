// Protocol 1 key trust: the rules on their own, mirroring ui/src/test/keyTrust.test.ts, and the shared vectors.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include <nlohmann/json.hpp>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/key_trust.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using namespace redvault::data;
using crypto::base64Decode;
using crypto::base64Encode;
using Json = nlohmann::json;

namespace {

// Letters in the ids on purpose: only then is capitals a different spelling.
const std::string kSelf = "abcdef11-1111-4111-8111-111111111111";
const std::string kBob = "bcdefa22-2222-4222-8222-222222222222";
const std::string kEve = "cdefab33-3333-4333-8333-333333333333";
const std::string kDm = "deadbeef-4444-4444-8444-444444444444";
const std::string kGroup = "feedface-5555-4555-8555-555555555555";

struct Fixture {
    std::shared_ptr<MemoryKeyPins> pins = std::make_shared<MemoryKeyPins>();
    KeyTrust trust{pins};
    std::map<std::string, crypto::SigningKeyPair> signers;

    Fixture() {
        for (const auto& id : {kSelf, kBob, kEve}) {
            const auto seed = crypto::generateConversationKey();
            signers.emplace(id, crypto::deriveSigningKeyPair(seed));
        }
    }

    api::KeyNoticeDto notice(const std::string& issuer, std::int64_t epoch, std::int64_t seq, const std::vector<std::string>& members,
                             const crypto::SecretBytes& key, const std::string& conversation = kGroup) {
        const auto input = crypto::keyNoticeInput(conversation, issuer, epoch, seq, members, crypto::keyCommitment(key));
        return api::KeyNoticeDto{epoch, seq, issuer, members, base64Encode(crypto::signMessage(input, signers.at(issuer).secretKey))};
    }

    std::optional<std::string> check(const crypto::SecretBytes* key, const std::optional<api::KeyNoticeDto>& notice, std::int64_t epoch) {
        GroupKeyCheck c;
        c.conversationId = kGroup;
        c.selfUserId = kSelf;
        c.epoch = epoch;
        c.key = key;
        c.notice = &notice;
        c.signingKey = [this](const std::string& id) -> std::optional<crypto::Bytes> {
            const auto it = signers.find(id);
            if (it == signers.end()) return std::nullopt;
            return it->second.publicKey;
        };
        return trust.checkGroupKey(c);
    }

    std::optional<std::string> check(const crypto::SecretBytes& key, const api::KeyNoticeDto& notice) {
        return check(&key, std::optional<api::KeyNoticeDto>(notice), notice.epoch);
    }
};

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::vector<std::string> sorted(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

crypto::SecretBytes newKey() {
    return crypto::generateConversationKey();
}

}  // namespace

// --- Shared vectors ---

TEST_CASE("commitment and signed bytes match the shared vectors byte for byte", "[keytrust][vectors]") {
    std::ifstream in(REDVAULT_KEY_NOTICE_VECTORS);
    REQUIRE(in);
    std::stringstream text;
    text << in.rdbuf();
    const Json vectors = Json::parse(text.str());
    REQUIRE(vectors["cases"].size() >= 3);
    for (const auto& v : vectors["cases"]) {
        const auto key = base64Decode(v["key"].get<std::string>());
        const auto commitment = crypto::keyCommitment(key);
        CHECK(base64Encode(commitment) == v["commitment"].get<std::string>());
        const auto& n = v["notice"];
        const auto input = crypto::keyNoticeInput(v["conversationId"].get<std::string>(), n["issuerId"].get<std::string>(), n["epoch"].get<std::int64_t>(),
                                                  n["seq"].get<std::int64_t>(), n["members"].get<std::vector<std::string>>(), commitment);
        CHECK(base64Encode(input) == v["input"].get<std::string>());
    }
}

TEST_CASE("the signed bytes refuse anything that isn't a UUID or doesn't fit", "[keytrust]") {
    const auto commitment = crypto::keyCommitment(newKey());
    CHECK_THROWS_AS(crypto::keyNoticeInput("not-a-uuid", kSelf, 0, 1, {kSelf}, commitment), crypto::CryptoError);
    CHECK_THROWS_AS(crypto::keyNoticeInput(kGroup, "not-a-uuid", 0, 1, {kSelf}, commitment), crypto::CryptoError);
    CHECK_THROWS_AS(crypto::keyNoticeInput(kGroup, kSelf, 0, 1, {"not-a-uuid"}, commitment), crypto::CryptoError);
    CHECK_THROWS_AS(crypto::keyNoticeInput(kGroup, kSelf, -1, 1, {kSelf}, commitment), crypto::CryptoError);
    CHECK_THROWS_AS(crypto::keyNoticeInput(kGroup, kSelf, 0x100000000LL, 1, {kSelf}, commitment), crypto::CryptoError);
    // Capitals are the same id, so the same bytes.
    CHECK(crypto::keyNoticeInput(kGroup, upper(kSelf), 0, 1, {upper(kBob)}, commitment) ==
          crypto::keyNoticeInput(kGroup, kSelf, 0, 1, {kBob}, commitment));
}

// --- Direct chats ---

TEST_CASE("control: a direct chat's same key keeps working", "[keytrust][direct]") {
    Fixture f;
    const auto key = newKey();
    CHECK_FALSE(f.trust.checkDirectKey(kSelf, kDm, 0, &key));
    CHECK_FALSE(f.trust.checkDirectKey(kSelf, kDm, 0, &key));
}

TEST_CASE("a different key at the same epoch is refused, and the pin stays", "[keytrust][direct]") {
    Fixture f;
    const auto real = newKey();
    const auto swapped = newKey();
    CHECK_FALSE(f.trust.checkDirectKey(kSelf, kDm, 0, &real));
    CHECK(f.trust.checkDirectKey(kSelf, kDm, 0, &swapped) == KeyTrust::kDirectKeyChanged);
    CHECK(f.trust.matchesPin(kSelf, kDm, real));
    CHECK_FALSE(f.trust.matchesPin(kSelf, kDm, swapped));
}

TEST_CASE("a later epoch is refused and pins nothing", "[keytrust][direct]") {
    Fixture f;
    const auto key = newKey();
    CHECK(f.trust.checkDirectKey(kSelf, kDm, 1, &key) == KeyTrust::kDirectKeyChanged);
    CHECK_FALSE(f.trust.pin(kSelf, kDm));
    CHECK_FALSE(f.trust.matchesPin(kSelf, kDm, key));
}

TEST_CASE("no key at all is its own answer, and pins nothing", "[keytrust][direct]") {
    Fixture f;
    CHECK(f.trust.checkDirectKey(kSelf, kDm, 0, nullptr) == KeyTrust::kDirectKeyUnopened);
    CHECK_FALSE(f.trust.pin(kSelf, kDm));
}

TEST_CASE("pins are per account and per chat, whatever the spelling of the ids", "[keytrust][direct][ids]") {
    Fixture f;
    const auto real = newKey();
    const auto swapped = newKey();
    CHECK_FALSE(f.trust.checkDirectKey(kSelf, kDm, 0, &real));
    // Capitals are the same chat: they don't get a fresh pin.
    CHECK(f.trust.checkDirectKey(kSelf, upper(kDm), 0, &swapped) == KeyTrust::kDirectKeyChanged);
    CHECK(f.trust.checkDirectKey(upper(kSelf), kDm, 0, &swapped) == KeyTrust::kDirectKeyChanged);
    // Control: someone else's copy of this computer has its own pin.
    CHECK_FALSE(f.trust.checkDirectKey(kBob, kDm, 0, &swapped));
}

// --- Group notices ---

TEST_CASE("control: a signed notice is accepted, then a new epoch from a member", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    CHECK_FALSE(f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1)));
    const auto k2 = newKey();
    CHECK_FALSE(f.check(k2, f.notice(kBob, 1, 1, {kSelf, kBob}, k2)));
    CHECK(f.trust.pin(kSelf, kGroup)->epoch == 1);
}

TEST_CASE("a server-chosen key is refused: no real member signed it", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1));
    const auto forged = f.notice(kBob, 1, 1, {kSelf, kBob}, k1);  // Signed for another key.
    const auto other = newKey();
    CHECK(f.check(other, forged) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("a new epoch from someone outside the group is refused, even with a valid signature", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1));
    const auto k2 = newKey();
    CHECK(f.check(k2, f.notice(kEve, 1, 1, {kSelf, kBob, kEve}, k2)) == KeyTrust::kGroupKeyUnverified);
    CHECK(f.trust.pin(kSelf, kGroup)->epoch == 0);
}

TEST_CASE("a tampered member list is refused: a hidden member can't be added to a signed notice", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    auto tampered = f.notice(kSelf, 0, 1, {kSelf, kBob}, k1);
    tampered.members.push_back(kEve);
    CHECK(f.check(k1, tampered) == KeyTrust::kGroupKeyUnverified);
    CHECK_FALSE(f.trust.pin(kSelf, kGroup));
}

TEST_CASE("rollback of an epoch or a sequence is refused, and so is a removed notice", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    f.check(k1, f.notice(kSelf, 0, 2, {kSelf, kBob}, k1));
    CHECK(f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1)) == KeyTrust::kGroupKeyUnverified);
    const auto k2 = newKey();
    f.check(k2, f.notice(kBob, 1, 1, {kSelf, kBob}, k2));
    CHECK(f.check(k1, f.notice(kSelf, 0, 3, {kSelf, kBob}, k1)) == KeyTrust::kGroupKeyUnverified);
    CHECK(f.check(&k2, std::nullopt, 1) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("a different key under an epoch already seen is refused", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1));
    const auto other = newKey();
    CHECK(f.check(other, f.notice(kBob, 0, 2, {kSelf, kBob}, other)) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("a notice that leaves this person out is refused", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    CHECK(f.check(k1, f.notice(kBob, 0, 1, {kBob, kEve}, k1)) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("an issuer who isn't in their own member list is refused", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    CHECK(f.check(k1, f.notice(kBob, 0, 1, {kSelf, kEve}, k1)) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("a notice for another epoch than the key's is refused", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    const std::optional<api::KeyNoticeDto> notice = f.notice(kSelf, 0, 1, {kSelf, kBob}, k1);
    CHECK(f.check(&k1, notice, 3) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("a notice from a conversation it wasn't signed for is refused", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    CHECK(f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1, kDm)) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("an unknown issuer, a junk signature and a notice that wouldn't parse all fail", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    auto junk = f.notice(kSelf, 0, 1, {kSelf, kBob}, k1);
    junk.signature = "not base64!";
    CHECK(f.check(k1, junk) == KeyTrust::kGroupKeyUnverified);

    const auto noKey = f.notice(kBob, 0, 1, {kSelf, kBob}, k1);
    f.signers.erase(kBob);
    CHECK(f.check(k1, noKey) == KeyTrust::kGroupKeyUnverified);

    // What dtos.cpp keeps when a notice won't parse: never treated as no notice at all.
    CHECK(f.check(&k1, std::optional<api::KeyNoticeDto>(api::KeyNoticeDto{}), 0) == KeyTrust::kGroupKeyUnverified);
}

TEST_CASE("a notice with no key to check it against says so", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    const std::optional<api::KeyNoticeDto> notice = f.notice(kSelf, 0, 1, {kSelf, kBob}, k1);
    CHECK(f.check(nullptr, notice, 0) == KeyTrust::kGroupKeyUnopened);
}

TEST_CASE("control: a group with no notice yet works until one has been seen", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    CHECK_FALSE(f.check(&k1, std::nullopt, 0));
    CHECK_FALSE(f.trust.pin(kSelf, kGroup));
}

TEST_CASE("control: a joiner confirming an unchanged key needs no earlier membership", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1));
    // Same epoch, same commitment: the key itself shows Eve holds it.
    CHECK_FALSE(f.check(k1, f.notice(kEve, 0, 2, {kSelf, kBob, kEve}, k1)));
    CHECK(f.trust.pin(kSelf, kGroup)->seq == 2);
}

TEST_CASE("members no notice named are flagged, so a rotation won't seal the key to them", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1));
    CHECK(f.trust.unconfirmedMembers(kSelf, kGroup, {kSelf, kBob, kEve}) == std::vector<std::string>{kEve});
    CHECK(f.trust.unconfirmedMembers(kSelf, kGroup, {kSelf, kBob}).empty());
    // Capitals are the same member, not an unconfirmed one.
    CHECK(f.trust.unconfirmedMembers(kSelf, kGroup, {upper(kBob)}).empty());
    // Control: nothing verified yet, nothing to compare with.
    CHECK(f.trust.unconfirmedMembers(kSelf, kDm, {kEve}).empty());
}

TEST_CASE("the next sequence number passes what's pinned and what the server has", "[keytrust][group]") {
    Fixture f;
    const auto k1 = newKey();
    CHECK(f.trust.nextSeq(kSelf, kGroup, 0, std::nullopt) == 1);
    f.check(k1, f.notice(kSelf, 0, 4, {kSelf, kBob}, k1));
    CHECK(f.trust.nextSeq(kSelf, kGroup, 0, std::nullopt) == 5);
    CHECK(f.trust.nextSeq(kSelf, kGroup, 0, api::KeyNoticeDto{0, 9, kBob, {}, ""}) == 10);
    CHECK(f.trust.nextSeq(kSelf, kGroup, 1, std::nullopt) == 1);
    CHECK(f.trust.nextSeq(kSelf, kGroup, 1, api::KeyNoticeDto{0, 9, kBob, {}, ""}) == 1);
}

// --- Id spelling ---

TEST_CASE("an issuer in capitals is the same person: their pinned key has to have signed it", "[keytrust][ids]") {
    Fixture f;
    const auto k1 = newKey();
    f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1));
    const auto k2 = newKey();
    // Everything spells Bob in capitals, under a key that isn't his.
    const std::string shouted = upper(kBob);
    const auto attacker = crypto::deriveSigningKeyPair(newKey());
    const auto input = crypto::keyNoticeInput(kGroup, shouted, 1, 1, {kSelf, shouted}, crypto::keyCommitment(k2));
    const api::KeyNoticeDto forged{1, 1, shouted, {kSelf, shouted}, base64Encode(crypto::signMessage(input, attacker.secretKey))};
    CHECK(f.check(k2, forged) == KeyTrust::kGroupKeyUnverified);

    // Control: Bob himself, in capitals, is still Bob.
    const auto genuine = crypto::keyNoticeInput(kGroup, shouted, 1, 1, {kSelf, shouted}, crypto::keyCommitment(k2));
    const api::KeyNoticeDto real{1, 1, shouted, {kSelf, shouted}, base64Encode(crypto::signMessage(genuine, f.signers.at(kBob).secretKey))};
    CHECK_FALSE(f.check(k2, real));
    // Pinned lowercase, always.
    CHECK(f.trust.pin(kSelf, kGroup)->members == std::vector<std::string>{kSelf, kBob});
}

TEST_CASE("this person in capitals in a notice still counts as named", "[keytrust][ids]") {
    Fixture f;
    const auto k1 = newKey();
    CHECK_FALSE(f.check(k1, f.notice(kBob, 0, 1, {upper(kSelf), kBob}, k1)));
}

// --- Stored pins ---

TEST_CASE("stored pins round-trip, and text that doesn't parse reads as no pin", "[keytrust]") {
    std::map<std::string, std::string> settings;
    auto stored = std::make_shared<StoredKeyPins>(
        [&](const std::string& self, const std::string& chat) {
            const auto it = settings.find(self + "/" + chat);
            return it == settings.end() ? std::string() : it->second;
        },
        [&](const std::string& self, const std::string& chat, const std::string& json) { settings[self + "/" + chat] = json; });

    stored->save(upper(kSelf), upper(kGroup), KeyPin{3, 7, "abc=", {kSelf, kBob}});
    // Keyed lowercase, whatever it was given.
    REQUIRE(settings.contains(kSelf + "/" + kGroup));
    const auto pin = stored->load(kSelf, kGroup);
    REQUIRE(pin);
    CHECK(pin->epoch == 3);
    CHECK(pin->seq == 7);
    CHECK(pin->commitment == "abc=");
    CHECK(pin->members == std::vector<std::string>{kSelf, kBob});

    settings[kSelf + "/" + kGroup] = "not json";
    CHECK_FALSE(stored->load(kSelf, kGroup));
    settings[kSelf + "/" + kGroup] = R"({"epoch":"x"})";
    CHECK_FALSE(stored->load(kSelf, kGroup));
    CHECK_FALSE(stored->load(kSelf, kDm));
}

// --- Forged answers ---

TEST_CASE("the same pin holds for another spelling of the ids, this account's too", "[keytrust][forged]") {
    Fixture f;
    const auto first = crypto::generateConversationKey();
    const auto other = crypto::generateConversationKey();
    CHECK_FALSE(f.trust.checkDirectKey(kSelf, kDm, 0, &first));
    CHECK(f.trust.checkDirectKey(kSelf, upper(kDm), 0, &other) == KeyTrust::kDirectKeyChanged);
    CHECK(f.trust.checkDirectKey(upper(kSelf), kDm, 0, &other) == KeyTrust::kDirectKeyChanged);
    CHECK_FALSE(f.trust.checkDirectKey(upper(kSelf), upper(kDm), 0, &first));
    CHECK(f.trust.isPinned(upper(kSelf), upper(kDm)));
    CHECK_FALSE(f.trust.isPinned(kSelf, kGroup));
}

TEST_CASE("an epoch or sequence past 32 bits can't sign or verify as the real one", "[keytrust][forged]") {
    Fixture f;
    const auto k = crypto::generateConversationKey();
    const auto real = f.notice(kSelf, 1, 1, {kSelf, kBob}, k);
    CHECK_FALSE(f.check(k, real));
    const auto commitment = crypto::keyCommitment(k);
    const std::int64_t past = 1 + (std::int64_t{1} << 32);
    CHECK_THROWS(crypto::keyNoticeInput(kGroup, kSelf, past, 1, {kSelf}, commitment));
    CHECK_THROWS(crypto::keyNoticeInput(kGroup, kSelf, 1, past, {kSelf}, commitment));
    CHECK_THROWS(crypto::keyNoticeInput(kGroup, kSelf, -1, 1, {kSelf}, commitment));
    CHECK_THROWS(crypto::keyNoticeInput(kGroup, kSelf, 1, -1, {kSelf}, commitment));
    CHECK_THROWS(crypto::keyNoticeInput("not-a-uuid", kSelf, 1, 1, {kSelf}, commitment));
    CHECK_THROWS(crypto::keyNoticeInput(kGroup, "not-a-uuid", 1, 1, {kSelf}, commitment));
    CHECK_THROWS(crypto::keyNoticeInput(kGroup, kSelf, 1, 1, {"not-a-uuid"}, commitment));
    auto wrapped = real;
    wrapped.epoch = past;
    CHECK(f.check(&k, std::optional<api::KeyNoticeDto>(wrapped), wrapped.epoch) == KeyTrust::kGroupKeyUnverified);
    CHECK(f.pins->load(kSelf, kGroup)->epoch == 1);
}

TEST_CASE("control: the largest epoch and sequence that fit still sign", "[keytrust][forged]") {
    const auto k = crypto::generateConversationKey();
    CHECK_NOTHROW(crypto::keyNoticeInput(kGroup, kSelf, 0xffffffffLL, 0xffffffffLL, {kSelf}, crypto::keyCommitment(k)));
}

TEST_CASE("a notice with a fraction for a number doesn't parse as that number", "[keytrust][forged]") {
    Json j{{"epoch", 1}, {"seq", 1}, {"issuerId", kSelf}, {"members", Json::array({kSelf})}, {"signature", "AAAA"}};
    CHECK(j.get<api::KeyNoticeDto>().epoch == 1);
    j["epoch"] = 1.5;
    CHECK_THROWS(j.get<api::KeyNoticeDto>());
    j["epoch"] = 1;
    j["seq"] = 1.5;
    CHECK_THROWS(j.get<api::KeyNoticeDto>());
}

TEST_CASE("the same sequence number with other members is refused", "[keytrust][forged]") {
    Fixture f;
    const auto k = crypto::generateConversationKey();
    CHECK_FALSE(f.check(k, f.notice(kSelf, 0, 1, {kSelf, kBob}, k)));
    CHECK(f.check(k, f.notice(kBob, 0, 1, {kSelf, kBob, kEve}, k)) == KeyTrust::kGroupKeyUnverified);
    CHECK(f.pins->load(kSelf, kGroup)->members == sorted({kSelf, kBob}));
    CHECK_FALSE(f.check(k, f.notice(kSelf, 0, 1, {kSelf, kBob}, k)));
    // Another order is the same list.
    CHECK_FALSE(f.check(k, f.notice(kSelf, 0, 1, {kBob, kSelf}, k)));
}

TEST_CASE("a notice can't be made without the key, even with a signing key of one's own", "[keytrust][forged]") {
    Fixture f;
    const auto k = crypto::generateConversationKey();
    const auto signingKey = [&](const std::string& id) -> std::optional<crypto::Bytes> {
        const auto it = f.signers.find(id);
        if (it == f.signers.end()) return std::nullopt;
        return it->second.publicKey;
    };
    const auto forged = f.notice(kEve, 0, 9, {kBob, kEve}, crypto::generateConversationKey());
    CHECK_FALSE(f.trust.noticeSignatureOk(kGroup, forged, k, signingKey));
    CHECK(f.trust.noticeSignatureOk(kGroup, f.notice(kBob, 0, 9, {kBob}, k), k, signingKey));
    CHECK_FALSE(f.trust.noticeSignatureOk(kGroup, f.notice(kBob, 0, 9, {kBob}, k), k, [](const std::string&) { return std::nullopt; }));
    CHECK_FALSE(f.trust.noticeSignatureOk(kDm, f.notice(kBob, 0, 9, {kBob}, k), k, signingKey));
}

TEST_CASE("a server-chosen public key doesn't match the private key", "[keytrust][forged]") {
    const auto pair = crypto::generateKeyPair();
    CHECK(crypto::publicKeyOf(pair.privateKey) == pair.publicKey);
    CHECK(crypto::publicKeyOf(pair.privateKey) != crypto::generateKeyPair().publicKey);
    CHECK_THROWS(crypto::publicKeyOf(crypto::Bytes(3, 1)));
}

// --- Who a notice names ---

TEST_CASE("a notice names the verified members, not the server's list", "[keytrust][forged]") {
    Fixture f;
    const auto k = crypto::generateConversationKey();
    CHECK_FALSE(f.check(k, f.notice(kSelf, 0, 1, {kSelf, kBob}, k)));
    CHECK(sorted(f.trust.membersToSign(kSelf, kGroup, {kSelf, kBob, kEve}, {kDm}, std::nullopt)) == sorted({kSelf, kBob, kDm}));
    CHECK(f.trust.membersToSign(kSelf, kGroup, {kSelf}, {}, std::nullopt) == std::vector<std::string>{kSelf});
    // Added twice, or in capitals, it's still one person.
    CHECK(sorted(f.trust.membersToSign(kSelf, kGroup, {kSelf, kBob}, {upper(kBob), kSelf}, std::nullopt)) == sorted({kSelf, kBob}));
}

TEST_CASE("someone joining names the last notice, not the server's list", "[keytrust][forged]") {
    Fixture f;
    const auto k = crypto::generateConversationKey();
    const auto latest = f.notice(kBob, 0, 1, {kBob, kDm}, k);
    CHECK(sorted(f.trust.membersToSign(kSelf, kGroup, {kSelf, kBob, kDm, kEve}, {kSelf}, latest)) == sorted({kSelf, kBob, kDm}));
}

TEST_CASE("control: with nothing verified the server's list is all there is", "[keytrust][forged]") {
    Fixture f;
    CHECK(sorted(f.trust.membersToSign(kSelf, kGroup, {kSelf, kBob}, {}, std::nullopt)) == sorted({kSelf, kBob}));
}

// --- Older epochs ---

TEST_CASE("an older epoch this computer verified can't be swapped later", "[keytrust][forged]") {
    Fixture f;
    const auto k1 = crypto::generateConversationKey();
    const auto k2 = crypto::generateConversationKey();
    CHECK_FALSE(f.check(k1, f.notice(kSelf, 0, 1, {kSelf, kBob}, k1)));
    CHECK_FALSE(f.check(k2, f.notice(kBob, 1, 1, {kSelf, kBob}, k2)));
    CHECK(f.trust.matchesVerifiedEpoch(kSelf, kGroup, 0, k1));
    CHECK_FALSE(f.trust.matchesVerifiedEpoch(kSelf, kGroup, 0, crypto::generateConversationKey()));
    CHECK(f.trust.matchesVerifiedEpoch(kSelf, kGroup, 1, k2));
    CHECK_FALSE(f.trust.matchesVerifiedEpoch(kSelf, kGroup, 1, k1));
    // Never verified here: nothing to compare with.
    CHECK(f.trust.matchesVerifiedEpoch(kSelf, kGroup, 7, crypto::generateConversationKey()));
    CHECK(f.trust.matchesVerifiedEpoch(kSelf, kDm, 0, crypto::generateConversationKey()));
}

TEST_CASE("past commitments survive being stored, and a pin from before them still loads", "[keytrust][forged]") {
    std::map<std::string, std::string> stored;
    const auto name = [](const std::string& self, const std::string& conversation) { return util::toLowerAscii(self + ":" + conversation); };
    StoredKeyPins pins([&](const std::string& self, const std::string& conversation) { return stored[name(self, conversation)]; },
                       [&](const std::string& self, const std::string& conversation, const std::string& value) { stored[name(self, conversation)] = value; });
    pins.save(kSelf, kGroup, KeyPin{2, 3, "abc=", {kSelf}, {{0, "zero="}, {1, "one="}}});
    const auto pin = pins.load(upper(kSelf), upper(kGroup));
    REQUIRE(pin);
    REQUIRE(pin->past.size() == 2);
    CHECK(pin->past.at(1) == "one=");
    REQUIRE(stored.size() == 1);
    auto& raw = stored.begin()->second;
    Json older = Json::parse(raw);
    older.erase("past");
    raw = older.dump();
    const auto earlier = pins.load(kSelf, kGroup);
    REQUIRE(earlier);
    CHECK(earlier->past.empty());
    CHECK(earlier->seq == 3);
}
