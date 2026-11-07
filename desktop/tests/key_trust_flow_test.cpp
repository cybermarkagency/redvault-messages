// Key trust through the real repositories, with several people's apps against one fake server: the same cases as
// android's KeyTrustFlowTest. Every message and key below goes through the real crypto.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fake_transport.hpp"
#include "key_trust_fake.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/api/api_error.hpp"
#include "redvault/api/group_invite_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/drive_repository.hpp"
#include "redvault/data/friend_repository.hpp"
#include "redvault/data/key_trust.hpp"
#include "redvault/data/message_keys.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/net/cookie_jar.hpp"

using namespace redvault;
using namespace redvault::data;
using crypto::base64Decode;
using crypto::base64Encode;
using Json = nlohmann::json;
using namespace redvault::test::keytrust;
using redvault::test::FakeTransport;

namespace {

Catch::Matchers::ExceptionMessageMatcher says(const std::string& text) {
    return Catch::Matchers::Message(text);
}

/// One computer of `who`: its own pins, like the app after sign-in.
struct Device {
    World& world;
    const Person& who;
    std::shared_ptr<MemoryKeyPins> keyPins;
    std::shared_ptr<MemoryContactKeyPins> contactPins = std::make_shared<MemoryContactKeyPins>();
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::shared_ptr<api::ApiClient> api = std::make_shared<api::ApiClient>(kBase, transport, std::make_shared<net::SessionCookieJar>());
    std::map<std::string, std::string> signingPins;
    /// Carried over from an earlier Device, for a pin that outlives one run of the app.
    std::shared_ptr<MessageKeys> keys;
    std::shared_ptr<ConversationRepository> conversations;
    std::shared_ptr<GroupRepository> groups;
    SelfIdentity identity;

    explicit Device(World& w, const Person& person, std::shared_ptr<MemoryKeyPins> pins = std::make_shared<MemoryKeyPins>())
        : world(w), who(person), keyPins(std::move(pins)), identity(person.identity()) {
        transport->handler = [this](const net::HttpRequest& r) {
            world.server.caller = who.userId;
            return world.server.handle(r);
        };
        keys = std::make_shared<MessageKeys>(
            api,
            [this](const std::string& self, const std::string& user, const std::string& key) {
                return signingPins.try_emplace(self + ":" + user, key).first->second;
            },
            keyPins,
            [this](const std::string& self, const std::string& user) -> std::optional<std::string> {
                const auto it = signingPins.find(self + ":" + user);
                if (it == signingPins.end()) return std::nullopt;
                return it->second;
            },
            contactPins);
        keys->begin(identity);
        conversations = std::make_shared<ConversationRepository>(api, ConversationRepository::HiddenIds{}, keys);
        groups = std::make_shared<GroupRepository>(api, keys);
    }

    ConversationMeta meta(const std::string& id) {
        for (const auto& dto : conversations->listConversations()) {
            if (dto.id == id) return std::move(*conversations->hydrateMeta(dto, identity));
        }
        FAIL("no such chat");
        throw std::logic_error("unreachable");
    }

    std::vector<data::Message> read(const std::string& id) {
        const auto m = meta(id);
        return conversations->fetchMessages(m, who.userId).messages;
    }

    data::Message send(const std::string& id, const std::string& text) {
        const auto m = meta(id);
        return conversations->send(m, text);
    }

    std::optional<KeyPin> pin(const std::string& id) { return keyPins->load(who.userId, id); }
    std::optional<std::string> problem(const std::string& id) { return conversations->keyProblem(id); }

    void join(const crypto::SecretBytes& groupKey) {
        const auto wrapped = base64Encode(crypto::sealConversationKey(groupKey, who.keys.publicKey));
        const auto result = api::invites::join(*api, "link1", "auth", wrapped);
        REQUIRE(result.status == "joined");
        groups->confirmSelfAfterJoin(result.conversationId, groupKey);
    }
};

std::vector<std::string> texts(const std::vector<data::Message>& messages) {
    std::vector<std::string> out;
    for (const auto& m : messages) out.push_back(m.text);
    return out;
}

api::FriendDto friendOf(const Person& person) {
    api::FriendDto dto;
    dto.userId = person.userId;
    dto.publicKey = base64Encode(person.keys.publicKey);
    return dto;
}

bool holds(const KeyRing& ring, crypto::ByteView key) {
    return std::any_of(ring.keys.begin(), ring.keys.end(), [&](const auto& entry) {
        return std::equal(entry.second.data(), entry.second.data() + entry.second.size(), key.begin(), key.end());
    });
}

std::vector<std::string> sortedIds(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

}  // namespace

// --- Direct chats ---

TEST_CASE("control: a direct chat pins its key and keeps working", "[keytrust][flow][direct]") {
    World w;
    Device device(w, w.alice);
    device.send(kDmId, "hello");
    CHECK(device.pin(kDmId));
    CHECK_FALSE(device.problem(kDmId));
    CHECK(texts(device.read(kDmId)) == std::vector<std::string>{"hello"});
}

TEST_CASE("a server-chosen key at a new epoch is refused, pins nothing, and nothing can be sent", "[keytrust][flow][direct]") {
    World w;
    Device device(w, w.alice);
    device.send(kDmId, "real");
    const auto pinned = device.pin(kDmId);
    const auto attackerKey = crypto::generateConversationKey();
    w.dm.keyEpoch = 1;
    w.dm.seal(w.alice, 1, attackerKey);

    const auto meta = device.meta(kDmId);
    CHECK(device.problem(kDmId) == KeyTrust::kDirectKeyChanged);
    CHECK_FALSE(std::equal(meta.conversationKey.data(), meta.conversationKey.data() + meta.conversationKey.size(), attackerKey.data(),
                           attackerKey.data() + attackerKey.size()));
    CHECK_FALSE(holds(*device.keys->ringFor(kDmId), attackerKey));
    CHECK(device.pin(kDmId)->commitment == pinned->commitment);

    const auto stored = w.dm.messages.size();
    const int attempts = w.server.sent("POST conversations/" + kDmId + "/messages");
    CHECK_THROWS_MATCHES(device.conversations->send(meta, "secret"), api::UserFacingError, says(KeyTrust::kDirectKeyChanged));
    CHECK(w.server.sent("POST conversations/" + kDmId + "/messages") == attempts);
    CHECK(w.dm.messages.size() == stored);
}

TEST_CASE("a first sight of a chat at a later epoch pins nothing at all", "[keytrust][flow][direct]") {
    World w;
    w.dm.keyEpoch = 1;
    w.dm.seal(w.alice, 1, w.dmKey);
    Device device(w, w.alice);
    device.meta(kDmId);
    CHECK(device.problem(kDmId) == KeyTrust::kDirectKeyChanged);
    CHECK_FALSE(device.pin(kDmId));
    // Nothing pinned, nothing to vouch for what's listed at epoch 0.
    CHECK(device.keys->ringFor(kDmId)->keys.empty());
}

TEST_CASE("a refused direct key still reads history, only under the pinned key", "[keytrust][flow][direct]") {
    World w;
    Device device(w, w.alice);
    device.send(kDmId, "real");
    const auto attackerKey = crypto::generateConversationKey();
    w.dm.keyEpoch = 1;
    w.dm.seal(w.alice, 1, attackerKey);
    CHECK(texts(device.read(kDmId)) == std::vector<std::string>{"real"});
    // The server also swaps the key it lists at epoch 0: that one isn't the pinned key, so it isn't used.
    w.dm.seal(w.alice, 0, crypto::generateConversationKey());
    CHECK(texts(device.read(kDmId)) == std::vector<std::string>{kRefused});
    CHECK(device.keys->ringFor(kDmId)->keys.empty());
}

TEST_CASE("a different key at epoch 0 is dropped, so it can't read or forge", "[keytrust][flow][direct]") {
    World w;
    Device device(w, w.alice);
    device.send(kDmId, "real");
    const auto attackerKey = crypto::generateConversationKey();
    w.dm.seal(w.alice, 0, attackerKey);
    const auto forged = crypto::encryptWithConversationKey("forged", attackerKey);
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(forged.ciphertext), base64Encode(forged.nonce), std::nullopt));

    const auto shown = device.read(kDmId);
    CHECK(device.problem(kDmId) == KeyTrust::kDirectKeyChanged);
    for (const auto& text : texts(shown)) CHECK(text != "forged");
    CHECK(device.keys->ringFor(kDmId)->keys.empty());
}

TEST_CASE("a key listed under an extra epoch of a direct chat is never used", "[keytrust][flow][direct]") {
    World w;
    Device device(w, w.alice);
    device.send(kDmId, "real");
    const auto attackerKey = crypto::generateConversationKey();
    // keyEpoch stays 0, so nothing looks wrong: the extra key just sits in the list, for a forged message to name.
    w.dm.seal(w.alice, 7, attackerKey);
    const auto forged = crypto::encryptWithConversationKey("forged", attackerKey);
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(forged.ciphertext), base64Encode(forged.nonce), std::nullopt, 7));
    CHECK(texts(device.read(kDmId)) == std::vector<std::string>{"real", kRefused});
    CHECK_FALSE(device.problem(kDmId));
    CHECK_FALSE(holds(*device.keys->ringFor(kDmId), attackerKey));
}

// --- Publishing notices ---

TEST_CASE("a group with no notice gets its first one, and other members accept it", "[keytrust][flow][publish]") {
    World w;
    Device alice(w, w.alice);
    alice.meta(kGroupId);
    CHECK(w.server.sent("PUT conversations/" + kGroupId + "/key-notice") == 1);
    CHECK(alice.pin(kGroupId)->seq == 1);
    CHECK(w.storedNotice().members == sortedIds({w.alice.userId, w.bob.userId, w.carol.userId}));

    Device bob(w, w.bob);
    bob.meta(kGroupId);
    CHECK_FALSE(bob.problem(kGroupId));
    CHECK(w.server.sent("PUT conversations/" + kGroupId + "/key-notice") == 1);
    CHECK(bob.pin(kGroupId)->seq == w.storedNotice().seq);
}

TEST_CASE("a failed publish pins nothing, so it can't lock the group", "[keytrust][flow][publish]") {
    World w;
    w.server.failNotice = 503;
    Device device(w, w.alice);
    device.send(kGroupId, "still works");
    CHECK_FALSE(device.pin(kGroupId));
    CHECK_FALSE(device.problem(kGroupId));
    CHECK(w.group.messages.size() == 1);
}

TEST_CASE("losing the publish race pins nothing, and the winner is picked up", "[keytrust][flow][publish]") {
    World w;
    w.server.staleNotice = true;
    Device device(w, w.alice);
    device.meta(kGroupId);
    CHECK_FALSE(device.pin(kGroupId));
    // Bob's notice, the one that won, is there on the next load.
    w.server.staleNotice = false;
    w.bob.signingKey = base64Encode(w.bob.signer().publicKey);
    w.group.keyNotice = w.signedNotice(w.bob.signer(), w.bob.userId, w.group, 0, 1, w.group.members, w.groupKey);
    device.meta(kGroupId);
    CHECK_FALSE(device.problem(kGroupId));
    CHECK(device.pin(kGroupId)->seq == 1);
}

TEST_CASE("a rekey carries its notice, and the others verify it", "[keytrust][flow][publish]") {
    World w;
    Device alice(w, w.alice);
    Device bob(w, w.bob);
    alice.meta(kGroupId);
    bob.meta(kGroupId);
    std::erase(w.group.members, w.carol.userId);
    w.group.wrapped.erase(w.carol.userId);
    w.group.rekeyRequired = true;

    alice.keys->rotate(kGroupId);
    CHECK(w.group.keyEpoch == 1);
    const auto notice = w.storedNotice();
    CHECK(notice.epoch == 1);
    CHECK(std::find(notice.members.begin(), notice.members.end(), w.carol.userId) == notice.members.end());
    CHECK(alice.pin(kGroupId)->epoch == 1);

    bob.meta(kGroupId);
    CHECK_FALSE(bob.problem(kGroupId));
    CHECK(bob.pin(kGroupId)->epoch == 1);
    CHECK(bob.pin(kGroupId)->members == sortedIds({w.alice.userId, w.bob.userId}));
}

TEST_CASE("a rekey refuses when the server lists someone no notice named", "[keytrust][flow][publish]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    const int before = w.server.sent("POST conversations/" + kGroupId + "/keys");
    // The server slips Eve in, then asks for a new key so she gets a copy of it.
    w.group.members.push_back(w.eve.userId);
    w.group.seal(w.eve, 0, w.groupKey);
    w.group.rekeyRequired = true;

    CHECK_THROWS_MATCHES(device.keys->rotate(kGroupId), api::UserFacingError, says(KeyTrust::kUnconfirmedMember));
    CHECK(w.server.sent("POST conversations/" + kGroupId + "/keys") == before);
    CHECK(w.group.keyEpoch == 0);
}

TEST_CASE("a routine add doesn't confirm someone the server slipped in", "[keytrust][flow][publish]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    const std::string ghost = "99999999-9999-4999-8999-999999999999";
    w.group.members.push_back(ghost);
    device.groups->add(meta, friendOf(w.eve));
    const auto named = w.storedNotice().members;
    CHECK(std::find(named.begin(), named.end(), w.eve.userId) != named.end());
    CHECK(std::find(named.begin(), named.end(), ghost) == named.end());
    const auto pinned = device.pin(kGroupId)->members;
    CHECK(std::find(pinned.begin(), pinned.end(), ghost) == pinned.end());
}

TEST_CASE("a member added in the open is confirmed by the notice, so the next rekey goes through", "[keytrust][flow][publish]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    device.groups->add(meta, friendOf(w.eve));
    CHECK(w.server.sent("keyNotice") >= 1);
    const auto pinned = device.pin(kGroupId)->members;
    CHECK(std::find(pinned.begin(), pinned.end(), w.eve.userId) != pinned.end());
    CHECK(w.storedNotice().seq == 2);

    w.group.rekeyRequired = true;
    device.keys->rotate(kGroupId);
    CHECK(w.group.keyEpoch == 1);
}

TEST_CASE("a failed add pins nothing new", "[keytrust][flow][publish]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    const auto before = device.pin(kGroupId);
    w.server.failAdd = true;
    CHECK_THROWS(device.groups->add(meta, friendOf(w.eve)));
    CHECK(device.pin(kGroupId)->seq == before->seq);
    CHECK(device.pin(kGroupId)->members == before->members);
}

TEST_CASE("an approval carries the epoch and the notice", "[keytrust][flow][publish]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    device.groups->approveJoinRequest(meta, w.eve.userId);
    const std::string* request = nullptr;
    for (const auto& r : w.server.requests) {
        if (r.find("/approve") != std::string::npos) request = &r;
    }
    REQUIRE(request != nullptr);
    CHECK(request->find("keyNotice") != std::string::npos);
    CHECK(request->find("keyEpoch") != std::string::npos);
    const auto pinned = device.pin(kGroupId)->members;
    CHECK(std::find(pinned.begin(), pinned.end(), w.eve.userId) != pinned.end());
}

TEST_CASE("a failed approval pins nothing new", "[keytrust][flow][publish]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    const auto before = device.pin(kGroupId);
    w.server.failAdd = true;
    CHECK_THROWS(device.groups->approveJoinRequest(meta, w.eve.userId));
    CHECK(device.pin(kGroupId)->members == before->members);
}

TEST_CASE("someone joining by link publishes their own notice", "[keytrust][flow][publish]") {
    World w;
    Device alice(w, w.alice);
    alice.meta(kGroupId);
    const auto before = w.storedNotice();
    w.server.inviteConversation = kGroupId;
    Device device(w, w.eve);
    device.join(w.groupKey);
    const auto notice = w.storedNotice();
    CHECK(notice.issuerId == w.eve.userId);
    CHECK(notice.seq == before.seq + 1);
    CHECK(std::find(notice.members.begin(), notice.members.end(), w.eve.userId) != notice.members.end());
    const auto pinned = device.pin(kGroupId)->members;
    CHECK(std::find(pinned.begin(), pinned.end(), w.eve.userId) != pinned.end());

    // Alice takes it: the key is the one she pinned, which shows Eve really holds it.
    alice.meta(kGroupId);
    CHECK_FALSE(alice.problem(kGroupId));
    CHECK(alice.pin(kGroupId)->seq == notice.seq);
}

TEST_CASE("a link joiner who loses the publish race is still in", "[keytrust][flow][publish]") {
    World w;
    Device alice(w, w.alice);
    alice.meta(kGroupId);
    w.server.inviteConversation = kGroupId;
    w.server.staleNotice = true;
    Device device(w, w.eve);
    device.join(w.groupKey);
    CHECK(std::find(w.group.members.begin(), w.group.members.end(), w.eve.userId) != w.group.members.end());
    CHECK_FALSE(device.pin(kGroupId));
}

TEST_CASE("a failed notice never blocks joining", "[keytrust][flow][publish]") {
    World w;
    w.server.inviteConversation = kGroupId;
    w.server.failNotice = 500;
    Device device(w, w.eve);
    device.join(w.groupKey);
    CHECK(std::find(w.group.members.begin(), w.group.members.end(), w.eve.userId) != w.group.members.end());
    CHECK_FALSE(device.pin(kGroupId));
}

// --- Checking notices through the ring ---

TEST_CASE("a substituted group key is dropped, sending stops, and older messages still read", "[keytrust][flow][group]") {
    World w;
    Device device(w, w.alice);
    device.send(kGroupId, "before");
    CHECK(texts(device.read(kGroupId)) == std::vector<std::string>{"before"});
    // No member made this: the server has no signing key of theirs, so it can only leave the notice as it was.
    const auto attackerKey = crypto::generateConversationKey();
    w.group.keyEpoch = 1;
    w.group.seal(w.alice, 1, attackerKey);

    const auto meta = device.meta(kGroupId);
    CHECK(device.problem(kGroupId) == KeyTrust::kGroupKeyUnverified);
    CHECK_FALSE(device.keys->ringFor(kGroupId)->keys.contains(1));
    CHECK(meta.conversationKey.empty());
    CHECK(texts(device.read(kGroupId)) == std::vector<std::string>{"before"});

    const auto stored = w.group.messages.size();
    CHECK_THROWS_MATCHES(device.conversations->send(meta, "secret"), api::UserFacingError, says(KeyTrust::kGroupKeyUnverified));
    CHECK(w.group.messages.size() == stored);
}

TEST_CASE("a removed notice stops the group once one has been seen", "[keytrust][flow][group]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    w.group.keyNotice.reset();
    device.meta(kGroupId);
    CHECK(device.problem(kGroupId) == KeyTrust::kGroupKeyUnverified);
    // It isn't quietly replaced with a fresh one either.
    CHECK(w.server.sent("PUT conversations/" + kGroupId + "/key-notice") == 1);
}

TEST_CASE("the refusal lifts once the real notice is back", "[keytrust][flow][group]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    const auto notice = w.group.keyNotice;
    w.group.keyNotice.reset();
    device.meta(kGroupId);
    REQUIRE(device.problem(kGroupId));
    w.group.keyNotice = notice;
    device.meta(kGroupId);
    CHECK_FALSE(device.problem(kGroupId));
    CHECK(device.keys->ringFor(kGroupId)->currentKey() != nullptr);
}

TEST_CASE("an invite link can't hand out a key that was refused", "[keytrust][flow][group]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    w.group.keyEpoch = 1;
    w.group.seal(w.alice, 1, crypto::generateConversationKey());
    device.meta(kGroupId);
    CHECK_THROWS_MATCHES(device.groups->currentKey(meta), api::UserFacingError, says(KeyTrust::kGroupKeyUnverified));
    CHECK_THROWS_MATCHES(device.groups->approveJoinRequest(meta, w.eve.userId), api::UserFacingError, says(KeyTrust::kGroupKeyUnverified));
    CHECK(w.server.sent("/approve") == 0);
    CHECK_THROWS_MATCHES(device.groups->add(meta, friendOf(w.eve)), api::UserFacingError, says(KeyTrust::kGroupKeyUnverified));
    CHECK_THROWS_MATCHES(device.groups->rename(meta, "new name"), api::UserFacingError, says(KeyTrust::kGroupKeyUnverified));
}

TEST_CASE("a rekey won't start from a refused key", "[keytrust][flow][group]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    w.group.keyEpoch = 1;
    w.group.seal(w.alice, 1, crypto::generateConversationKey());
    w.group.rekeyRequired = true;
    CHECK_THROWS_MATCHES(device.keys->rotate(kGroupId), api::UserFacingError, says(KeyTrust::kGroupKeyUnverified));
    CHECK(w.server.sent("POST conversations/" + kGroupId + "/keys") == 0);
}

TEST_CASE("a cached 'no signing key yet' doesn't stick", "[keytrust][flow][group]") {
    World w;
    // Bob published a notice, but his key isn't on the server when Alice first looks...
    w.group.keyNotice = w.signedNotice(w.bob.signer(), w.bob.userId, w.group, 0, 1, w.group.members, w.groupKey);
    Device device(w, w.alice);
    device.meta(kGroupId);
    CHECK(device.problem(kGroupId) == KeyTrust::kGroupKeyUnverified);
    // ...then it is: the same session looks again and the group opens.
    w.bob.signingKey = base64Encode(w.bob.signer().publicKey);
    device.meta(kGroupId);
    CHECK_FALSE(device.problem(kGroupId));
    CHECK(device.pin(kGroupId)->seq == 1);
}

// --- Id spelling ---

TEST_CASE("an issuer id in capitals can't dodge the signing key pin", "[keytrust][flow][ids]") {
    World w;
    w.bob.signingKey = base64Encode(w.bob.signer().publicKey);
    w.group.keyNotice = w.signedNotice(w.bob.signer(), w.bob.userId, w.group, 0, 1, w.group.members, w.groupKey);
    Device device(w, w.alice);
    device.meta(kGroupId);
    REQUIRE_FALSE(device.problem(kGroupId));

    // A new epoch "from Bob", spelled in capitals so it looks like someone with nothing pinned, under a signing key
    // the server made up: everything lowercases to Bob, who is in the verified list, except the key that signed it.
    const auto attacker = crypto::deriveSigningKeyPair(crypto::generateConversationKey());
    w.server.forgedSigningKey = base64Encode(attacker.publicKey);
    const auto attackerKey = crypto::generateConversationKey();
    w.group.keyEpoch = 1;
    w.group.seal(w.alice, 1, attackerKey);
    w.group.keyNotice = w.signedNotice(attacker, upper(w.bob.userId), w.group, 1, 1, w.group.members, attackerKey);

    device.meta(kGroupId);
    CHECK(device.problem(kGroupId) == KeyTrust::kGroupKeyUnverified);
    CHECK_FALSE(device.keys->ringFor(kGroupId)->keys.contains(1));
    // Only one spelling ever reached the pins, and the server was never asked the other.
    CHECK(device.signingPins.size() == 1);
    CHECK(device.signingPins.begin()->first == w.alice.userId + ":" + w.bob.userId);
    CHECK(w.server.sent("users/" + upper(w.bob.userId)) == 0);
}

TEST_CASE("a member's id in capitals can't earn a second signing key pin", "[keytrust][flow][ids]") {
    World w;
    w.bob.signingKey = base64Encode(w.bob.signer().publicKey);
    w.bob.since = "2026-10-01";
    const auto attacker = crypto::deriveSigningKeyPair(crypto::generateConversationKey());
    w.server.forgedSigningKey = base64Encode(attacker.publicKey);
    Device device(w, w.alice);
    device.keys->resolveSenderKeys(w.alice.userId, {w.bob.userId});
    device.keys->resolveSenderKeys(upper(w.alice.userId), {upper(w.bob.userId)});
    CHECK(device.keys->senderSigningKey(upper(w.bob.userId)) == w.bob.signer().publicKey);
    CHECK(device.keys->senderSigningKey(w.bob.userId) == w.bob.signer().publicKey);
    CHECK(device.signingPins.size() == 1);
}

TEST_CASE("a message whose sender is in capitals is still checked against that person's pin", "[keytrust][flow][ids]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "from bob");
    // The server replays a message spelled as Bob in capitals, signed by a key of its own.
    const auto attacker = crypto::deriveSigningKeyPair(crypto::generateConversationKey());
    w.server.forgedSigningKey = base64Encode(attacker.publicKey);
    const auto forged = crypto::encryptWithConversationKey("forged", w.dmKey);
    const auto input = crypto::messageSignatureInput(kDmId, upper(w.bob.userId), 0, forged.nonce, forged.ciphertext);
    w.dm.messages.push_back(w.server.message(w.dm, upper(w.bob.userId), base64Encode(forged.ciphertext), base64Encode(forged.nonce),
                                             base64Encode(crypto::signMessage(input, attacker.secretKey))));

    Device device(w, w.alice);
    const auto shown = device.read(kDmId);
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].text == "from bob");
    CHECK(shown[1].text == "Couldn't verify who sent this message.");
    CHECK(device.signingPins.size() == 1);
    CHECK(w.server.sent("users/" + upper(w.bob.userId)) == 0);
}

TEST_CASE("control: a real message whose sender is in capitals still reads, as the same person", "[keytrust][flow][ids]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "hello");
    w.dm.messages.back()["senderId"] = upper(w.bob.userId);
    Device device(w, w.alice);
    const auto shown = device.read(kDmId);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].text == "hello");
    CHECK(shown[0].senderId == w.bob.userId);
    CHECK_FALSE(shown[0].fromMe);
}

TEST_CASE("our own message in capitals is still ours", "[keytrust][flow][ids]") {
    World w;
    Device device(w, w.alice);
    device.send(kDmId, "mine");
    w.dm.messages.back()["senderId"] = upper(w.alice.userId);
    const auto shown = device.read(kDmId);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].fromMe);
}

TEST_CASE("what the server says about our own signing key doesn't matter", "[keytrust][flow][ids]") {
    World w;
    Device first(w, w.alice);
    first.meta(kGroupId);
    w.alice.signingKey.reset();
    Device second(w, w.alice);
    second.meta(kGroupId);
    CHECK_FALSE(second.problem(kGroupId));
}

// --- M-3 ---

TEST_CASE("an unsigned message from someone known to sign is flagged in a direct chat too", "[keytrust][flow][m3]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "signed");
    const auto stripped = crypto::encryptWithConversationKey("stripped", w.dmKey);
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(stripped.ciphertext), base64Encode(stripped.nonce), std::nullopt));
    Device alice(w, w.alice);
    const auto shown = alice.read(kDmId);
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].text == "signed");
    CHECK_FALSE(shown[0].unverifiedSender);
    CHECK(shown[1].text == "stripped");
    CHECK(shown[1].unverifiedSender);
}

TEST_CASE("control: a message from before they started signing stays legacy in a direct chat", "[keytrust][flow][m3]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "signed");
    const auto old = crypto::encryptWithConversationKey("from before", w.dmKey);
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(old.ciphertext), base64Encode(old.nonce), std::nullopt, 0, "2026-09-01T00:00:00.000Z"));
    Device alice(w, w.alice);
    const auto shown = alice.read(kDmId);
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].text == "from before");
    CHECK_FALSE(shown[0].unverifiedSender);
    CHECK_FALSE(shown[1].unverifiedSender);
}

TEST_CASE("an unsigned reaction from a signer in a direct chat is dropped", "[keytrust][flow][m3]") {
    World w;
    Device bob(w, w.bob);
    const auto sent = bob.send(kDmId, "signed");
    const auto reaction = crypto::encryptWithConversationKey(model::rich_message::serializeReaction(sent.id, "\xF0\x9F\x91\x8D"), w.dmKey);
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(reaction.ciphertext), base64Encode(reaction.nonce), std::nullopt));
    Device alice(w, w.alice);
    const auto shown = alice.read(kDmId);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].reactions.empty());
}

// --- Forged answers: nothing is sealed to a key that changed ---

namespace {

/// This computer has seen their real key.
void seen(Device& device, const Person& person) {
    REQUIRE_FALSE(device.contactPins->keyChanged(device.who.userId, person.userId, base64Encode(person.keys.publicKey)));
}

/// The server starts handing out another key for them.
std::string swap(World& w, const Person& person) {
    return w.server.forgedPublicKeys[person.userId] = base64Encode(crypto::generateKeyPair().publicKey);
}

api::FriendDto swapped(const Person& person, const std::string& key) {
    api::FriendDto dto = friendOf(person);
    dto.publicKey = key;
    return dto;
}

const std::string kContactChanged = KeyTrust::kContactKeyChanged;

}  // namespace

TEST_CASE("a rekey refuses to seal the new key to a swapped member key", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    seen(device, w.bob);
    swap(w, w.bob);
    w.group.rekeyRequired = true;
    CHECK_THROWS_MATCHES(device.keys->rotate(kGroupId), api::UserFacingError, says(kContactChanged));
    CHECK(w.server.sent("POST conversations/" + kGroupId + "/keys") == 0);
    CHECK(w.group.keyEpoch == 0);
}

TEST_CASE("control: a rekey goes through while everyone's key is the one seen before", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    seen(device, w.bob);
    seen(device, w.carol);
    w.group.rekeyRequired = true;
    device.keys->rotate(kGroupId);
    CHECK(w.group.keyEpoch == 1);
}

TEST_CASE("a rekey seals our own copy to our own key, whatever the server says that is", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    swap(w, w.alice);
    w.group.rekeyRequired = true;
    device.keys->rotate(kGroupId);
    REQUIRE(w.group.keyEpoch == 1);
    const auto meta = device.meta(kGroupId);
    CHECK_FALSE(device.problem(kGroupId));
    CHECK_FALSE(meta.conversationKey.empty());
    CHECK(device.keys->ringFor(kGroupId)->currentEpoch == 1);
}

TEST_CASE("an add or an approval refuses to seal to a swapped key", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    seen(device, w.eve);
    const auto forged = swap(w, w.eve);
    CHECK_THROWS_MATCHES(device.groups->approveJoinRequest(meta, w.eve.userId), api::UserFacingError, says(kContactChanged));
    CHECK(w.server.sent("/approve") == 0);
    CHECK_THROWS_MATCHES(device.groups->add(meta, swapped(w.eve, forged)), api::UserFacingError, says(kContactChanged));
    CHECK(w.server.sent("POST conversations/" + kGroupId + "/participants") == 0);
}

TEST_CASE("control: an add and an approval still seal to a key seen before", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    const auto meta = device.meta(kGroupId);
    seen(device, w.eve);
    seen(device, w.dave);
    device.groups->add(meta, friendOf(w.eve));
    CHECK(w.server.sent("POST conversations/" + kGroupId + "/participants") == 1);
    device.groups->approveJoinRequest(device.meta(kGroupId), w.dave.userId);
    CHECK(w.server.sent("/approve") == 1);
}

TEST_CASE("a new group refuses to seal to a swapped key", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    seen(device, w.bob);
    const auto forged = swap(w, w.bob);
    CHECK_THROWS_MATCHES(device.groups->create(device.identity, "name", {swapped(w.bob, forged)}), api::UserFacingError, says(kContactChanged));
    CHECK(w.server.sent("POST conversations ") == 0);
}

TEST_CASE("control: a new group seals to a key seen before", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    seen(device, w.bob);
    CHECK_FALSE(device.groups->create(device.identity, "name", {friendOf(w.bob)}).empty());
    CHECK(w.server.sent("POST conversations ") == 1);
}

TEST_CASE("accepting a friend request refuses to seal to a swapped key", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    seen(device, w.bob);
    FriendRepository friends(device.api, device.contactPins);
    const FriendRequestView request{"11110000-0000-4000-8000-000000000001", w.bob.userId, "bob", std::nullopt};
    SECTION("control") {
        CHECK_FALSE(friends.accept(device.identity, request).empty());
        CHECK(w.server.sent("/accept") == 1);
    }
    SECTION("swapped") {
        swap(w, w.bob);
        CHECK_THROWS_MATCHES(friends.accept(device.identity, request), api::UserFacingError, says(kContactChanged));
        CHECK(w.server.sent("/accept") == 0);
    }
}

TEST_CASE("sharing a file refuses to seal to a swapped key", "[keytrust][flow][seal]") {
    World w;
    Device device(w, w.alice);
    seen(device, w.bob);
    const auto dir = std::filesystem::temp_directory_path() / ("redvault-share-" + w.bob.userId);
    auto identity = std::make_shared<const SelfIdentity>(w.alice.identity());
    DriveRepository drive(device.api, identity, dir, {}, device.contactPins);
    DriveEntry entry;
    entry.id = "22220000-0000-4000-8000-000000000001";
    entry.kind = "file";
    entry.name = "report";
    entry.fileId = entry.id;
    entry.meta = Json::object();
    SECTION("control") {
        drive.share(entry, w.bob.userId);
        CHECK(w.server.sent("/shares") == 1);
    }
    SECTION("swapped") {
        swap(w, w.bob);
        CHECK_THROWS_MATCHES(drive.share(entry, w.bob.userId), api::UserFacingError, says(kContactChanged));
        CHECK(w.server.sent("/shares") == 0);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("a contact key pin has one spelling, and only a different key counts as changed", "[keytrust][seal]") {
    MemoryContactKeyPins pins;
    const std::string self = "abcdef11-1111-4111-8111-111111111111";
    const std::string bob = "bcdefa22-2222-4222-8222-222222222222";
    const auto first = base64Encode(crypto::generateKeyPair().publicKey);
    const auto other = base64Encode(crypto::generateKeyPair().publicKey);
    CHECK_FALSE(pins.keyChanged(self, bob, first));
    CHECK_FALSE(pins.keyChanged(self, bob, first));
    CHECK(pins.keyChanged(self, bob, other));
    CHECK(pins.keyChanged(upper(self), upper(bob), other));
    CHECK_FALSE(pins.keyChanged(upper(self), upper(bob), first));
    CHECK_THROWS_MATCHES(requireUnchangedKey(&pins, self, upper(bob), other), api::UserFacingError, says(kContactChanged));
    CHECK_NOTHROW(requireUnchangedKey(nullptr, self, bob, other));
}

// --- Forged answers: what the ring opens ---

TEST_CASE("only older epochs are opened from the server's list", "[keytrust][flow][epochs]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    const auto older = crypto::generateConversationKey();
    const auto future = crypto::generateConversationKey();
    const auto negative = crypto::generateConversationKey();
    const auto current = crypto::generateConversationKey();
    w.group.seal(w.alice, 1, older);
    w.group.seal(w.alice, 2, current);
    w.group.seal(w.alice, 999, future);
    w.group.seal(w.alice, -1, negative);
    w.group.keyEpoch = 2;
    w.group.keyNotice = std::nullopt;
    Device fresh(w, w.alice);
    fresh.meta(kGroupId);
    const auto ring = fresh.keys->ringFor(kGroupId);
    REQUIRE(ring);
    CHECK(holds(*ring, older));
    CHECK_FALSE(holds(*ring, future));
    CHECK_FALSE(holds(*ring, negative));
    CHECK(ring->keys.contains(0));
    CHECK(ring->keys.contains(1));
    CHECK(ring->keys.contains(2));
    CHECK(ring->keys.size() == 3);
}

TEST_CASE("an older key that isn't the one verified at that epoch is left out", "[keytrust][flow][epochs]") {
    World w;
    Device alice(w, w.alice);
    alice.send(kGroupId, "at epoch 0");
    w.group.rekeyRequired = true;
    alice.keys->rotate(kGroupId);
    REQUIRE(alice.pin(kGroupId)->past.contains(0));
    CHECK(texts(alice.read(kGroupId)) == std::vector<std::string>{"at epoch 0"});

    // The server replaces its copy of the old key, and forges a message under it.
    const auto attackerKey = crypto::generateConversationKey();
    w.group.seal(w.alice, 0, attackerKey);
    const auto forged = crypto::encryptWithConversationKey("forged", attackerKey);
    w.group.messages.push_back(w.server.message(w.group, w.bob.userId, base64Encode(forged.ciphertext), base64Encode(forged.nonce), std::nullopt, 0));
    const auto shown = texts(alice.read(kGroupId));
    CHECK(std::find(shown.begin(), shown.end(), "forged") == shown.end());
    CHECK_FALSE(holds(*alice.keys->ringFor(kGroupId), attackerKey));
    CHECK_FALSE(alice.problem(kGroupId));
}

TEST_CASE("a pin keeps the commitments of the epochs before it, through several rekeys", "[keytrust][flow][epochs]") {
    World w;
    Device alice(w, w.alice);
    alice.meta(kGroupId);
    const auto first = alice.pin(kGroupId)->commitment;
    w.group.rekeyRequired = true;
    alice.keys->rotate(kGroupId);
    const auto second = alice.pin(kGroupId)->commitment;
    w.group.rekeyRequired = true;
    alice.keys->rotate(kGroupId);
    const auto past = alice.pin(kGroupId)->past;
    REQUIRE(past.size() == 2);
    CHECK(past.at(0) == first);
    CHECK(past.at(1) == second);
}

// --- Forged answers: signers ---

TEST_CASE("a pinned signer stays a signer when the server says nothing", "[keytrust][flow][signer]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "signed");
    Device first(w, w.alice);
    first.read(kDmId);
    const auto stripped = crypto::encryptWithConversationKey("stripped", w.dmKey);
    // Dated before they started: the server would have this count as an old message.
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(stripped.ciphertext), base64Encode(stripped.nonce), std::nullopt, 0, "2026-09-01T00:00:00.000Z"));
    w.server.saysNothingAboutSigning = true;
    Device second(w, w.alice);
    second.signingPins = first.signingPins;
    const auto shown = second.read(kDmId);
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].text == "stripped");
    CHECK(shown[0].unverifiedSender);
    CHECK(shown[1].text == "signed");
    CHECK_FALSE(shown[1].unverifiedSender);
}

TEST_CASE("a pinned signer stays a signer when the server fails to answer", "[keytrust][flow][signer]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "signed");
    Device first(w, w.alice);
    first.read(kDmId);
    Device second(w, w.alice);
    second.signingPins = first.signingPins;
    w.server.answersFailForUsers = true;
    second.keys->resolveSenderKeys(w.alice.userId, {w.bob.userId});
    const auto key = second.keys->senderSigningKey(w.bob.userId);
    REQUIRE(key);
    CHECK(*key == w.bob.signer().publicKey);
    api::MessageDto unsigned_;
    unsigned_.senderId = w.bob.userId;
    unsigned_.nonce = "AAAA";
    unsigned_.ciphertext = "AAAA";
    unsigned_.createdAt = "2026-09-01T00:00:00.000Z";
    CHECK(second.keys->checkSender(unsigned_, kDmId) == SenderCheck::Unverified);
}

TEST_CASE("control: someone nobody pinned is still legacy when the server says nothing", "[keytrust][flow][signer]") {
    World w;
    Device device(w, w.alice);
    device.meta(kDmId);
    w.server.saysNothingAboutSigning = true;
    device.keys->resolveSenderKeys(w.alice.userId, {w.bob.userId});
    api::MessageDto unsigned_;
    unsigned_.senderId = w.bob.userId;
    unsigned_.nonce = "AAAA";
    unsigned_.ciphertext = "AAAA";
    unsigned_.createdAt = "2026-10-02T00:00:00.000Z";
    CHECK(device.keys->checkSender(unsigned_, kDmId) == SenderCheck::Legacy);
}

TEST_CASE("our own signing key comes from our own keys, not from the server", "[keytrust][flow][signer]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    w.server.forgedSigningKey = base64Encode(crypto::deriveSigningKeyPair(crypto::generateConversationKey()).publicKey);
    w.alice.signingKey = w.server.forgedSigningKey;
    Device fresh(w, w.alice);
    fresh.keys->resolveSenderKeys(w.alice.userId, {w.alice.userId});
    const auto key = fresh.keys->senderSigningKey(w.alice.userId);
    REQUIRE(key);
    CHECK(*key == w.alice.signer().publicKey);
    // Checking a message goes by the same key: ours opens, one the server's key signed doesn't.
    const auto message = [&](const crypto::SigningKeyPair& signer) {
        const auto sealed = crypto::encryptWithConversationKey("mine", w.dmKey);
        api::MessageDto dto;
        dto.senderId = w.alice.userId;
        dto.keyEpoch = 0;
        dto.nonce = base64Encode(sealed.nonce);
        dto.ciphertext = base64Encode(sealed.ciphertext);
        dto.createdAt = "2026-10-02T00:00:00.000Z";
        dto.signature = base64Encode(crypto::signMessage(crypto::messageSignatureInput(kDmId, w.alice.userId, 0, sealed.nonce, sealed.ciphertext), signer.secretKey));
        return dto;
    };
    CHECK(fresh.keys->checkSender(message(w.alice.signer()), kDmId) == SenderCheck::Verified);
    CHECK(fresh.keys->checkSender(message(crypto::deriveSigningKeyPair(crypto::generateConversationKey())), kDmId) == SenderCheck::Invalid);

    w.server.answersFailForUsers = true;
    Device offline(w, w.alice);
    offline.keys->resolveSenderKeys(w.alice.userId, {w.alice.userId});
    REQUIRE(offline.keys->senderSigningKey(w.alice.userId));
    CHECK(*offline.keys->senderSigningKey(w.alice.userId) == w.alice.signer().publicKey);
    CHECK(offline.keys->checkSender(message(w.alice.signer()), kDmId) == SenderCheck::Verified);
}

TEST_CASE("a signature for another chat doesn't count here", "[keytrust][flow][signer]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "signed");
    const auto moved = crypto::encryptWithConversationKey("moved", w.dmKey);
    const auto input = crypto::messageSignatureInput(kGroupId, w.bob.userId, 0, moved.nonce, moved.ciphertext);
    const auto signature = base64Encode(crypto::signMessage(input, w.bob.signer().secretKey));
    // The server says it belongs to the group it was signed for, and shows it in the direct chat.
    w.dm.messages.push_back(w.server.message(w.group, w.bob.userId, base64Encode(moved.ciphertext), base64Encode(moved.nonce), signature, 0));
    Device alice(w, w.alice);
    const auto shown = texts(alice.read(kDmId));
    CHECK(std::find(shown.begin(), shown.end(), "moved") == shown.end());
}

TEST_CASE("control: the same signature for this chat does count", "[keytrust][flow][signer]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "signed");
    const auto here = crypto::encryptWithConversationKey("here", w.dmKey);
    const auto input = crypto::messageSignatureInput(kDmId, w.bob.userId, 0, here.nonce, here.ciphertext);
    const auto signature = base64Encode(crypto::signMessage(input, w.bob.signer().secretKey));
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(here.ciphertext), base64Encode(here.nonce), signature, 0));
    Device alice(w, w.alice);
    const auto shown = texts(alice.read(kDmId));
    CHECK(std::find(shown.begin(), shown.end(), "here") != shown.end());
}

// --- Forged answers: communities, first notices, joiners ---

namespace {

Json communityFlag() {
    return Json{{"id", "x"}, {"name", "x"}, {"kind", "group"}, {"status", "active"}, {"role", "member"}, {"canPost", true}, {"keyEpoch", 0}, {"rekeyRequired", false}};
}

}  // namespace

TEST_CASE("a pinned chat can't be turned into a community", "[keytrust][flow][community]") {
    World w;
    Device device(w, w.alice);
    device.meta(kDmId);
    w.dm.keyEpoch = 1;
    w.dm.seal(w.alice, 1, crypto::generateConversationKey());
    w.dm.community = communityFlag();
    const auto meta = device.meta(kDmId);
    CHECK_FALSE(meta.community);
    REQUIRE(device.keys->ringFor(kDmId));
    CHECK(device.keys->ringFor(kDmId)->keyProblem == std::optional<std::string>(KeyTrust::kDirectKeyChanged));
}

TEST_CASE("a pinned group can't be turned into a community either", "[keytrust][flow][community]") {
    World w;
    Device device(w, w.alice);
    device.meta(kGroupId);
    w.group.keyEpoch = 1;
    w.group.seal(w.alice, 1, crypto::generateConversationKey());
    w.group.community = communityFlag();
    const auto meta = device.meta(kGroupId);
    CHECK_FALSE(meta.community);
    CHECK(device.problem(kGroupId));
}

TEST_CASE("control: a real community with no pin stays a community", "[keytrust][flow][community]") {
    World w;
    Device device(w, w.alice);
    w.group.community = communityFlag();
    const auto listed = device.conversations->listConversations();
    const auto dto = std::find_if(listed.begin(), listed.end(), [](const api::ConversationDto& c) { return c.id == kGroupId; });
    REQUIRE(dto != listed.end());
    CHECK(device.conversations->isCommunity(*dto));
    device.conversations->refreshKeys(*dto);
    CHECK_FALSE(device.keys->ringFor(kGroupId));
    CHECK_FALSE(device.pin(kGroupId));
}

TEST_CASE("a computer with no pin publishes no first notice for a later epoch", "[keytrust][flow][first]") {
    World w;
    w.group.keyEpoch = 1;
    w.group.seal(w.alice, 1, crypto::generateConversationKey());
    Device device(w, w.alice);
    device.meta(kGroupId);
    CHECK(w.server.sent("key-notice") == 0);
    CHECK_FALSE(device.pin(kGroupId));
    CHECK_FALSE(device.problem(kGroupId));
}

TEST_CASE("a link joiner won't vouch for a notice no member signed", "[keytrust][flow][joiner]") {
    World w;
    Device alice(w, w.alice);
    Device bob(w, w.bob);
    bob.send(kGroupId, "so bob signs");
    alice.meta(kGroupId);
    const auto genuine = w.storedNotice();
    const int published = w.server.sent("conversations/" + kGroupId + "/key-notice");
    const auto attacker = crypto::deriveSigningKeyPair(crypto::generateConversationKey());
    const std::string ghost = "99999999-9999-4999-8999-999999999999";
    auto members = genuine.members;
    members.push_back(ghost);
    const auto otherKey = crypto::generateConversationKey();
    SECTION("signed with a key the attacker made up") {
        w.group.keyNotice = w.signedNotice(attacker, w.bob.userId, w.group, genuine.epoch, genuine.seq, members, w.groupKey);
    }
    SECTION("a real member's key, for a group key that isn't this one") {
        w.group.keyNotice = w.signedNotice(w.bob.signer(), w.bob.userId, w.group, genuine.epoch, genuine.seq, members, otherKey);
    }
    w.server.inviteConversation = kGroupId;
    Device device(w, w.eve);
    device.join(w.groupKey);
    CHECK(std::find(w.group.members.begin(), w.group.members.end(), w.eve.userId) != w.group.members.end());
    CHECK(w.server.sent("conversations/" + kGroupId + "/key-notice") == published);
    CHECK_FALSE(device.pin(kGroupId));
}

TEST_CASE("a link joiner names the last notice, not the server's list", "[keytrust][flow][joiner]") {
    World w;
    Device alice(w, w.alice);
    alice.meta(kGroupId);
    const std::string ghost = "99999999-9999-4999-8999-999999999999";
    w.group.members.push_back(ghost);
    // Carol has gone, and the server says so.
    std::erase(w.group.members, w.carol.userId);
    w.server.inviteConversation = kGroupId;
    Device device(w, w.eve);
    device.join(w.groupKey);
    CHECK(w.storedNotice().issuerId == w.eve.userId);
    CHECK(sortedIds(w.storedNotice().members) == sortedIds({w.alice.userId, w.bob.userId, w.eve.userId}));
}

TEST_CASE("an add names whoever joined since this computer last looked", "[keytrust][flow][notice-members]") {
    World w;
    Device alice(w, w.alice);
    const auto stale = alice.meta(kGroupId);
    Device bob(w, w.bob);
    bob.groups->add(bob.meta(kGroupId), friendOf(w.dave));
    const int seqBefore = static_cast<int>(w.storedNotice().seq);
    alice.groups->add(stale, friendOf(w.eve));
    const auto notice = w.storedNotice();
    CHECK(notice.seq == seqBefore + 1);
    CHECK(sortedIds(notice.members) == sortedIds({w.alice.userId, w.bob.userId, w.carol.userId, w.dave.userId, w.eve.userId}));
}

TEST_CASE("an add leaves out whoever the server no longer lists", "[keytrust][flow][notice-members]") {
    World w;
    Device alice(w, w.alice);
    const auto meta = alice.meta(kGroupId);
    std::erase(w.group.members, w.carol.userId);
    alice.groups->add(meta, friendOf(w.eve));
    CHECK(sortedIds(w.storedNotice().members) == sortedIds({w.alice.userId, w.bob.userId, w.eve.userId}));
}

TEST_CASE("an approval doesn't confirm someone the server slipped in either", "[keytrust][flow][notice-members]") {
    World w;
    Device alice(w, w.alice);
    const auto meta = alice.meta(kGroupId);
    const std::string ghost = "99999999-9999-4999-8999-999999999999";
    w.group.members.push_back(ghost);
    alice.groups->approveJoinRequest(meta, w.eve.userId);
    const auto named = alice.pin(kGroupId)->members;
    CHECK(std::find(named.begin(), named.end(), w.eve.userId) != named.end());
    CHECK(std::find(named.begin(), named.end(), ghost) == named.end());
}

TEST_CASE("an add refuses from a group whose key was refused", "[keytrust][flow][notice-members]") {
    World w;
    Device alice(w, w.alice);
    const auto meta = alice.meta(kGroupId);
    w.group.keyNotice = std::nullopt;  // Once seen, a notice can't disappear.
    CHECK_THROWS_AS(alice.groups->add(meta, friendOf(w.eve)), api::UserFacingError);
    CHECK(w.server.sent("POST conversations/" + kGroupId + "/participants") == 0);
}

TEST_CASE("a notice with a fraction for a number can't be verified", "[keytrust][flow][notice-input]") {
    World w;
    Device alice(w, w.alice);
    alice.meta(kGroupId);
    Json forged = *w.group.keyNotice;
    forged["seq"] = forged["seq"].get<double>() + 1.5;
    w.group.keyNotice = forged;
    alice.meta(kGroupId);
    CHECK(alice.problem(kGroupId) == std::optional<std::string>(KeyTrust::kGroupKeyUnverified));
}

// --- M-2 ---

TEST_CASE("a replay under another id shows once, and the original stays", "[keytrust][flow][m2]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "pay me once");
    const Json original = w.dm.messages.front();
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, original["ciphertext"].get<std::string>(), original["nonce"].get<std::string>(),
                                             original["signature"].get<std::string>()));
    REQUIRE(w.dm.messages.size() == 2);
    Device reader(w, w.alice);
    const auto meta = reader.meta(kDmId);
    const auto result = reader.conversations->fetchMessages(meta, w.alice.userId);
    REQUIRE(texts(result.messages) == std::vector<std::string>{"pay me once"});
    CHECK(result.messages[0].id == original["id"].get<std::string>());
    CHECK(result.unread == 1);  // a replay doesn't count as unread either
}

TEST_CASE("a replay claiming another epoch never shows its content twice", "[keytrust][flow][m2]") {
    World w;
    Device device(w, w.alice);
    device.send(kGroupId, "only once");
    std::erase(w.group.members, w.carol.userId);
    w.group.wrapped.erase(w.carol.userId);
    w.group.rekeyRequired = true;
    device.keys->rotate(kGroupId);
    const Json original = w.group.messages.front();
    // The same bytes, said to be from the new epoch: its key doesn't open them.
    w.group.messages.push_back(w.server.message(w.group, w.alice.userId, original["ciphertext"].get<std::string>(), original["nonce"].get<std::string>(), std::nullopt, 1));
    const auto all = texts(device.read(kGroupId));
    CHECK(std::count(all.begin(), all.end(), "only once") == 1);
    CHECK(std::count(all.begin(), all.end(), kRefused) == 1);  // shown as an error, not hidden
}

TEST_CASE("damaged messages that share a nonce each still show their error", "[keytrust][flow][m2]") {
    World w;
    const std::string nonce = base64Encode(crypto::Bytes(24, 0));
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(crypto::Bytes(40, 1)), nonce, std::nullopt));
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, base64Encode(crypto::Bytes(40, 2)), nonce, std::nullopt));
    Device device(w, w.alice);
    CHECK(texts(device.read(kDmId)) == std::vector<std::string>{kRefused, kRefused});
}

TEST_CASE("control: the same ciphertext under different nonces isn't a replay", "[keytrust][flow][m2]") {
    World w;
    const std::string ciphertext = base64Encode(crypto::Bytes(40, 1));
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, ciphertext, base64Encode(crypto::Bytes(24, 1)), std::nullopt));
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, ciphertext, base64Encode(crypto::Bytes(24, 2)), std::nullopt));
    Device device(w, w.alice);
    CHECK(device.read(kDmId).size() == 2);
}

TEST_CASE("another base64 spelling of the same bytes is still the same message", "[keytrust][flow][m2]") {
    World w;
    Device bob(w, w.bob);
    bob.send(kDmId, "once");
    const Json original = w.dm.messages.front();
    const std::string ciphertext = original["ciphertext"].get<std::string>();
    // "once" makes 20 bytes of ciphertext, so its last base64 character has two bits that decode to nothing.
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto last = ciphertext.find_last_not_of('=');
    std::string respelled = ciphertext;
    respelled[last] = kAlphabet[kAlphabet.find(ciphertext[last]) | 1];
    REQUIRE(respelled != ciphertext);
    w.dm.messages.push_back(w.server.message(w.dm, w.bob.userId, respelled, original["nonce"].get<std::string>(), original["signature"].get<std::string>()));
    Device device(w, w.alice);
    CHECK(texts(device.read(kDmId)) == std::vector<std::string>{"once"});
}
