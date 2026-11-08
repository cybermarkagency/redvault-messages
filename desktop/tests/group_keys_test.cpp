// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#include "fake_chat_server.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/friend_repository.hpp"
#include "redvault/data/message_keys.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/util/text.hpp"
#include "test_helpers.hpp"

// Group key epochs and message signatures (db/migrations/030,
// docs/group-keys-and-signatures.md): the signing key and signed bytes pinned
// to the web app's own vectors, and the repository's behaviour against the
// fake server, which makes the same checks as the real one.

using namespace redvault;
using namespace redvault::data;
using crypto::base64Decode;
using crypto::base64Encode;
using redvault::test::FakeChatServer;
using redvault::test::Json;

namespace {

Json vectors() {
    std::ifstream in(std::string(REDVAULT_SIGNATURE_VECTORS));
    REQUIRE(in);
    return Json::parse(in);
}

std::string hex(crypto::ByteView bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

const std::string kGroup = "9f000000-0000-4000-8000-000000000001";
const std::string kAlice = "aaaaaaaa-0000-4000-8000-000000000001";
const std::string kBob = "bbbbbbbb-0000-4000-8000-000000000002";
const std::string kCarol = "cccccccc-0000-4000-8000-000000000003";

struct Fixture {
    test::TestUser alice = test::makeUser(kAlice, "alice");
    test::TestUser bob = test::makeUser(kBob, "bob");
    test::TestUser carol = test::makeUser(kCarol, "carol");
    FakeChatServer server;
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::shared_ptr<api::ApiClient> api =
        std::make_shared<api::ApiClient>(FakeChatServer::kBase, transport, std::make_shared<net::SessionCookieJar>());
    std::map<std::string, std::string> pinned;
    std::shared_ptr<MessageKeys> keys = std::make_shared<MessageKeys>(
        api, [this](const std::string& self, const std::string& user, const std::string& key) {
            return pinned.try_emplace(self + ":" + user, key).first->second;
        });
    SelfIdentity me = alice.identity();
    ConversationRepository repo{api, {}, keys};
    crypto::SecretBytes groupKey;

    Fixture() {
        server.attach(*transport);
        server.selfId = kAlice;
        server.addUser(alice);
        server.addUser(bob);
        server.addUser(carol);
        keys->begin(me);
        groupKey = server.addConversation(kGroup, alice, {&bob, &carol}, true, std::string("Weekend plans"));
    }

    Json& group() { return server.conversations.front(); }

    ConversationMeta hydrate() {
        auto meta = repo.hydrateMeta(Json(group()).get<api::ConversationDto>(), me);
        REQUIRE(meta);
        return std::move(*meta);
    }

    /// Carol is removed: the server marks the group as needing a new key.
    void removeCarol() {
        auto& people = server.participants[kGroup];
        people.erase(std::remove_if(people.begin(), people.end(), [](const Json& p) { return p["userId"] == kCarol; }), people.end());
        group()["rekeyRequired"] = true;
    }

    /// Bob's signing key, registered and served like the real server does.
    crypto::SigningKeyPair bobSigns() {
        auto pair = crypto::deriveSigningKeyPair(bob.keys.privateKey);
        server.users[kBob]["signingPublicKey"] = base64Encode(pair.publicKey);
        server.users[kBob]["signingKeySince"] = "2026-01-01";
        return pair;
    }

    Json& addFromBob(const std::string& id, const std::string& plaintext, crypto::ByteView key, int epoch,
                     const crypto::SigningKeyPair* signer, const std::string& createdAt) {
        Json& message = server.addMessage(kGroup, id, kBob, plaintext, key, createdAt);
        message["keyEpoch"] = epoch;
        if (signer != nullptr) {
            const auto input = crypto::messageSignatureInput(kGroup, kBob, static_cast<std::uint32_t>(epoch),
                                                             base64Decode(message["nonce"].get<std::string>()),
                                                             base64Decode(message["ciphertext"].get<std::string>()));
            message["signature"] = base64Encode(crypto::signMessage(input, signer->secretKey));
        }
        return message;
    }
};

std::string mid(int n) {
    const std::string digits = std::to_string(n);
    return "10000000-0000-4000-8000-" + std::string(12 - digits.size(), '0') + digits;
}

}  // namespace

TEST_CASE("derives the web's signing keys and signs exactly the web's bytes", "[signatures]") {
    const Json v = vectors();
    std::vector<crypto::SigningKeyPair> accounts;
    for (const auto& account : v["accounts"]) {
        accounts.push_back(crypto::deriveSigningKeyPair(base64Decode(account["accountPrivateKey"].get<std::string>())));
        CHECK(base64Encode(accounts.back().publicKey) == account["signingPublicKey"].get<std::string>());
    }
    for (const auto& c : v["cases"]) {
        const auto input = crypto::messageSignatureInput(c["conversationId"].get<std::string>(), c["senderId"].get<std::string>(),
                                                         c["keyEpoch"].get<std::uint32_t>(), base64Decode(c["nonce"].get<std::string>()),
                                                         base64Decode(c["ciphertext"].get<std::string>()));
        CHECK(hex(input) == c["inputHex"].get<std::string>());
        const auto& pair = accounts[c["account"].get<std::size_t>()];
        const auto expected = base64Decode(c["signature"].get<std::string>());
        CHECK(crypto::signMessage(input, pair.secretKey) == expected);
        CHECK(crypto::verifyMessageSignature(expected, input, pair.publicKey));
        CHECK_FALSE(crypto::verifyMessageSignature(expected, input, accounts[1 - c["account"].get<std::size_t>()].publicKey));
    }
    CHECK_THROWS_AS(crypto::messageSignatureInput("conv-1", kAlice, 0, crypto::Bytes(24), crypto::Bytes(1)), crypto::CryptoError);
}

TEST_CASE("a group someone left gets a fresh key for the remaining members only", "[groups][epochs]") {
    Fixture f;
    f.removeCarol();
    f.hydrate();  // any member's app rotates on load

    const auto rotations = f.server.bodiesFor("conversations/" + kGroup + "/keys");
    REQUIRE(rotations.size() == 1);
    const Json& body = rotations.front();
    CHECK(body["fromEpoch"] == 0);
    std::map<std::string, std::string> copies;
    for (const auto& key : body["keys"]) copies[key["userId"].get<std::string>()] = key["wrappedKey"].get<std::string>();
    CHECK(copies.size() == 2);
    CHECK_FALSE(copies.contains(kCarol));

    const auto newKey = crypto::unsealConversationKey(base64Decode(copies.at(kBob)), f.bob.keys.publicKey, f.bob.keys.privateKey);
    CHECK(test::copy(newKey) != test::copy(f.groupKey));
    CHECK(crypto::decryptWithConversationKey(base64Decode(body["nameCiphertext"].get<std::string>()),
                                             base64Decode(body["nameNonce"].get<std::string>()), newKey) == "Weekend plans");
    CHECK(f.group()["keyEpoch"] == 1);
}

TEST_CASE("sends under the current epoch, signed; a send that finds the group waiting rotates first", "[groups][epochs][signatures]") {
    Fixture f;
    auto meta = f.hydrate();
    f.removeCarol();
    f.repo.send(meta, "after the change");

    REQUIRE(f.server.signingKey);
    const auto sent = f.server.bodiesFor("conversations/" + kGroup + "/messages");
    REQUIRE(sent.size() == 2);  // refused while the group waited, then sent under epoch 1
    const Json& body = sent.back();
    CHECK(body["keyEpoch"] == 1);
    const auto input = crypto::messageSignatureInput(kGroup, kAlice, 1, base64Decode(body["nonce"].get<std::string>()),
                                                     base64Decode(body["ciphertext"].get<std::string>()));
    CHECK(crypto::verifyMessageSignature(base64Decode(body["signature"].get<std::string>()), input, base64Decode(*f.server.signingKey)));
}

TEST_CASE("reads each message with its own epoch; checks signatures; one broken message hides nothing else", "[groups][signatures]") {
    Fixture f;
    const auto bob = f.bobSigns();
    f.addFromBob(mid(1), "signed", f.groupKey, 0, &bob, "2026-02-01T00:00:01.000Z");
    // Forged as Bob with Carol's key.
    const auto carol = crypto::deriveSigningKeyPair(f.carol.keys.privateKey);
    f.addFromBob(mid(2), "forged", f.groupKey, 0, &carol, "2026-02-01T00:00:02.000Z");
    // Unsigned after Bob's apps started signing (an older app of his, or a forgery), and an unsigned reaction.
    f.addFromBob(mid(3), "unsigned", f.groupKey, 0, nullptr, "2026-02-01T00:00:03.000Z");
    f.addFromBob(mid(4), model::rich_message::serializeReaction(mid(1), "\xF0\x9F\x91\x8D"), f.groupKey, 0, nullptr,
                 "2026-02-01T00:00:04.000Z");
    // Damaged ciphertext.
    Json& broken = f.addFromBob(mid(5), "x", f.groupKey, 0, &bob, "2026-02-01T00:00:05.000Z");
    broken["ciphertext"] = base64Encode(crypto::Bytes(40));
    // From before Bob's key existed: fine unsigned.
    f.addFromBob(mid(6), "old", f.groupKey, 0, nullptr, "2025-12-31T00:00:00.000Z");

    const auto meta = f.hydrate();
    const auto result = f.repo.fetchMessages(meta, kAlice);
    std::vector<std::pair<std::string, bool>> shown;
    for (const auto& m : result.messages) shown.emplace_back(m.text, m.unverifiedSender);
    CHECK(shown == std::vector<std::pair<std::string, bool>>{
                       {"old", false},
                       {"signed", false},
                       {"Couldn't verify who sent this message.", false},
                       {"unsigned", true},
                       {"Couldn't verify who sent this message.", false},
                   });
    CHECK(result.messages[1].reactions.empty());
}

TEST_CASE("messages from before and after a rotation both open", "[groups][epochs]") {
    Fixture f;
    const auto bob = f.bobSigns();
    f.addFromBob(mid(1), "before", f.groupKey, 0, &bob, "2026-02-01T00:00:01.000Z");
    f.removeCarol();
    f.hydrate();  // rotates
    const auto copies = f.server.bodiesFor("conversations/" + kGroup + "/keys").front()["keys"];
    std::string bobsCopy;
    for (const auto& key : copies) {
        if (key["userId"] == kBob) bobsCopy = key["wrappedKey"].get<std::string>();
    }
    const auto next = crypto::unsealConversationKey(base64Decode(bobsCopy), f.bob.keys.publicKey, f.bob.keys.privateKey);
    f.addFromBob(mid(2), "after", next, 1, &bob, "2026-02-01T00:00:02.000Z");

    const auto meta = f.hydrate();
    const auto result = f.repo.fetchMessages(meta, kAlice);
    REQUIRE(result.messages.size() == 2);
    CHECK(result.messages[0].text == "before");
    CHECK(result.messages[1].text == "after");
}

TEST_CASE("the first signing key seen for someone is pinned", "[signatures]") {
    Fixture f;
    const auto bob = f.bobSigns();
    f.addFromBob(mid(1), "one", f.groupKey, 0, &bob, "2026-02-01T00:00:01.000Z");
    f.repo.fetchMessages(f.hydrate(), kAlice);
    CHECK(f.pinned.contains(kAlice + ":" + kBob));

    // A new session: the server now serves another key for Bob and a message signed with it.
    const auto swapped = crypto::deriveSigningKeyPair(crypto::generateKeyPair().privateKey);
    f.server.users[kBob]["signingPublicKey"] = base64Encode(swapped.publicKey);
    f.addFromBob(mid(2), "swapped", f.groupKey, 0, &swapped, "2026-02-01T00:00:02.000Z");
    f.keys->end();
    f.keys->begin(f.me);
    const auto result = f.repo.fetchMessages(f.hydrate(), kAlice);
    REQUIRE(result.messages.size() == 2);
    CHECK(result.messages[0].text == "one");
    CHECK(result.messages[1].text == "Couldn't verify who sent this message.");
}

TEST_CASE("someone who registers a signing key mid-session is not treated as a forger", "[signatures]") {
    Fixture f;
    // Bob has no key yet when the chat is first read; "no key" must not stay cached for the session.
    f.addFromBob(mid(1), "before", f.groupKey, 0, nullptr, "2026-02-01T00:00:01.000Z");
    auto first = f.repo.fetchMessages(f.hydrate(), kAlice);
    REQUIRE(first.messages.size() == 1);
    CHECK(first.messages[0].text == "before");

    const auto bob = f.bobSigns();
    f.addFromBob(mid(2), "after", f.groupKey, 0, &bob, "2026-02-01T00:00:02.000Z");
    const auto second = f.repo.fetchMessages(f.hydrate(), kAlice);
    REQUIRE(second.messages.size() == 2);
    CHECK(second.messages[1].text == "after");
    CHECK_FALSE(second.messages[1].unverifiedSender);
}
