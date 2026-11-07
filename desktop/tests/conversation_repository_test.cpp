#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "fake_chat_server.hpp"
#include "redvault/data/conversation_repository.hpp"
#include "redvault/data/friend_repository.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/model/rich_message.hpp"
#include "test_helpers.hpp"

using namespace redvault;
using namespace redvault::data;
using redvault::test::copy;
using redvault::test::FakeChatServer;
using redvault::test::Json;

namespace {

// Reply and reaction targets are validated as UUIDs, so Alice (who is replied to) needs a real one.
const std::string kAlice = "aaaaaaaa-0000-4000-8000-000000000001";
const std::string kThumbsUp = "\xF0\x9F\x91\x8D";

// Message ids must be UUIDs for replies and reactions to point at them.
std::string mid(int n) {
    const std::string digits = std::to_string(n);
    return "10000000-0000-4000-8000-" + std::string(12 - digits.size(), '0') + digits;
}

struct Fixture {
    test::TestUser alice = test::makeUser(kAlice, "alice");
    test::TestUser bob = test::makeUser("b1", "bob");
    test::TestUser carol = test::makeUser("c1", "carol");
    FakeChatServer server;
    std::shared_ptr<test::FakeTransport> transport = std::make_shared<test::FakeTransport>();
    std::shared_ptr<api::ApiClient> api = std::make_shared<api::ApiClient>(
        FakeChatServer::kBase, transport, std::make_shared<net::SessionCookieJar>());
    SelfIdentity me = alice.identity();

    Fixture() {
        server.attach(*transport);
        server.addUser(alice);
        server.addUser(bob);
        server.addUser(carol);
    }

    ConversationMeta hydrate(ConversationRepository& repo, std::size_t index = 0) {
        auto meta = repo.hydrateMeta(api::Json(server.conversations[index]).get<api::ConversationDto>(), me);
        REQUIRE(meta);
        return std::move(*meta);
    }
};

}  // namespace

TEST_CASE("hydrates a DM: other side, their key and the safety number", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    ConversationRepository repo(f.api);

    const auto meta = f.hydrate(repo);
    CHECK(meta.handle == "bob");
    CHECK(meta.otherUserId == "b1");
    CHECK(meta.otherPublicKey == f.bob.keys.publicKey);
    CHECK(copy(meta.conversationKey) == copy(key));
    CHECK(meta.safetyNumber == crypto::safetyNumberDigits(f.alice.keys.publicKey, f.bob.keys.publicKey));
    CHECK(meta.otherUserIds(kAlice) == std::vector<std::string>{"b1"});
}

TEST_CASE("hydrates a group: decrypted name and every member", "[conversations][groups]") {
    Fixture f;
    f.server.addConversation("g1", f.alice, {&f.bob, &f.carol}, true, "Weekend plans");
    f.server.addConversation("g2", f.alice, {&f.bob}, true, "   ");
    ConversationRepository repo(f.api);

    const auto group = f.hydrate(repo, 0);
    CHECK(group.isGroup);
    CHECK(group.handle == "Weekend plans");
    CHECK(group.members.size() == 3);
    CHECK(group.handleOf("c1") == "carol");
    CHECK(group.handleOf("gone") == "Former member");
    auto others = group.otherUserIds(kAlice);
    std::sort(others.begin(), others.end());
    CHECK(others == std::vector<std::string>{"b1", "c1"});

    CHECK(f.hydrate(repo, 1).handle == ConversationRepository::kDefaultGroupName);
}

TEST_CASE("fetches, decrypts and folds a DM's messages like Android", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    auto& s = f.server;

    // Listed newest first, as the server does; the result must come out oldest first.
    s.addMessage("c1", mid(8), "b1", "", crypto::generateConversationKey(), "2026-09-25T08:00:00.000Z");  // wrong key
    // A file whose object the server lists as the attachment...
    model::FileDescriptor descriptor{.version = 1,
                                     .objectId = "3f2504e0-4f89-11d3-9a0c-0305e82c3301",
                                     .fileKey = "Y2pxeH+GjZSboqmwt77FzNPa4ejv9v0ECxIZICcuNTw=",
                                     .filename = "photo.jpg",
                                     .mimeType = "image/jpeg",
                                     .originalSize = 10,
                                     .encryptedSize = 51,
                                     .ciphertextSha256 = std::string(64, 'a'),
                                     .encryption = {"xchacha20poly1305-secretstream", 1, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYX", 65536}};
    auto& file = s.addMessage("c1", mid(6), "b1", model::file_message::serialize(descriptor), key, "2026-09-25T06:00:00.000Z");
    file["attachments"] = Json::array({Json{{"objectId", "3F2504E0-4F89-11D3-9A0C-0305E82C3301"}, {"status", "available"}}});
    // ...and the same descriptor pasted without an attachment, which must not render as a file.
    s.addMessage("c1", mid(7), "b1", model::file_message::serialize(descriptor), key, "2026-09-25T07:00:00.000Z");
    s.addMessage("c1", mid(5), "b1", model::rich_message::serializeReaction(mid(1), kThumbsUp), key, "2026-09-25T05:00:00.000Z");
    s.addMessage("c1", mid(4), kAlice, model::rich_message::serializeReaction(mid(1), kThumbsUp), key, "2026-09-25T04:00:00.000Z");
    s.addMessage("c1", mid(3), "b1", model::rich_message::serializeReply("sure", {mid(2), kAlice, "lunch?"}), key, "2026-09-25T03:00:00.000Z");
    s.addMessage("c1", mid(2), kAlice, "lunch?", key, "2026-09-25T02:00:00.000Z");
    s.addMessage("c1", mid(1), "b1", "hello", key, "2026-09-25T01:00:00.000Z");
    auto& deleted = s.addMessage("c1", mid(9), kAlice, "gone", key, "2026-09-25T09:00:00.000Z");
    deleted["ciphertext"] = nullptr;
    deleted["nonce"] = nullptr;
    deleted["deletedAt"] = "2026-09-25T09:30:00.000Z";
    s.addMessage("c1", mid(10), "b1", "hidden on this device", key, "2026-09-25T10:00:00.000Z");
    s.receipts["c1"] = {Json{{"messageId", mid(2)}, {"userId", "b1"}, {"status", "read"}},
                        Json{{"messageId", mid(1)}, {"userId", kAlice}, {"status", "read"}}};

    ConversationRepository repo(f.api, [](const std::string& self) {
        CHECK(self == kAlice);
        return std::set<std::string>{mid(10)};
    });
    const auto meta = f.hydrate(repo);
    const auto result = repo.fetchMessages(meta, kAlice);

    std::vector<std::string> ids;
    for (const auto& m : result.messages) ids.push_back(m.id);
    // Oldest first; reactions folded away; deleted and hidden messages gone.
    CHECK(ids == std::vector<std::string>{mid(1), mid(2), mid(3), mid(6), mid(7), mid(8)});

    const auto& hello = result.messages[0];
    CHECK(hello.text == "hello");
    CHECK_FALSE(hello.fromMe);
    REQUIRE(hello.reactions.size() == 1);
    CHECK(hello.reactions[0].emoji == kThumbsUp);
    CHECK(hello.reactions[0].count == 2);
    CHECK(hello.reactions[0].mine);
    CHECK(hello.reactions[0].myReactionId == mid(4));

    const auto& lunch = result.messages[1];
    CHECK(lunch.fromMe);
    CHECK(lunch.seen);
    CHECK(lunch.delivered);
    CHECK(lunch.ciphertextB64.has_value());

    const auto& reply = result.messages[2];
    CHECK(reply.text == "sure");
    REQUIRE(reply.replyTo);
    CHECK(reply.replyTo->messageId == mid(2));
    CHECK(reply.replyTo->fromMe);

    CHECK(result.messages[3].kind == MessageKind::File);
    CHECK(result.messages[3].text == "photo.jpg");
    CHECK(result.messages[3].fileStatus == "available");
    CHECK(result.messages[4].kind == MessageKind::Text);
    CHECK(result.messages[4].text == "Unreadable message.");
    CHECK(result.messages[5].kind == MessageKind::Undecryptable);

    // Bob's messages without a receipt from alice, reactions excluded: 3, 6, 7, 8 and the hidden 10.
    CHECK(result.unread == 5);
}

TEST_CASE("pages back through history without gaps or duplicates", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    // 260 messages, two per millisecond, so page boundaries fall inside a millisecond.
    for (int i = 0; i < 260; ++i) {
        const int ms = i / 2;
        char at[40];
        std::snprintf(at, sizeof at, "2026-09-25T10:00:%02d.%03dZ", ms / 1000, ms % 1000);
        f.server.addMessage("c1", mid(i + 1), "b1", "m" + std::to_string(i + 1), key, at);
    }
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);

    const auto first = repo.fetchMessages(meta, kAlice);
    REQUIRE(first.messages.size() == 50);
    CHECK(first.messages.front().text == "m211");
    CHECK(first.messages.back().text == "m260");
    CHECK(first.hasOlder);

    f.server.messageListCalls = 0;
    const auto more = repo.fetchMessages(meta, kAlice, 250);
    REQUIRE(more.messages.size() == 250);
    CHECK(f.server.messageListCalls == 2);  // 200, then the rest
    for (std::size_t i = 0; i < more.messages.size(); ++i) {
        CHECK(more.messages[i].text == "m" + std::to_string(i + 11));
    }
    CHECK(more.hasOlder);

    const auto all = repo.fetchMessages(meta, kAlice, 400);
    CHECK(all.messages.size() == 260);
    CHECK(all.messages.front().text == "m1");
    CHECK_FALSE(all.hasOlder);
}

TEST_CASE("edits replace the text only when the original sender made them", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    auto& s = f.server;
    s.addMessage("c1", mid(1), "b1", "see you at 5", key, "2026-09-25T01:00:00.000Z");
    s.addMessage("c1", mid(2), "b1", model::rich_message::serializeEdit("see you at 6", mid(1)), key, "2026-09-25T01:01:00.000Z");
    s.addMessage("c1", mid(3), "b1", model::rich_message::serializeEdit("see you at 7", mid(1)), key, "2026-09-25T01:02:00.000Z");
    s.addMessage("c1", mid(4), kAlice, "ok", key, "2026-09-25T01:03:00.000Z");
    // Alice can't rewrite Bob's message, and Bob can't rewrite Alice's.
    s.addMessage("c1", mid(5), kAlice, model::rich_message::serializeEdit("forged", mid(1)), key, "2026-09-25T01:04:00.000Z");
    s.addMessage("c1", mid(6), "b1", model::rich_message::serializeEdit("forged too", mid(4)), key, "2026-09-25T01:05:00.000Z");
    s.addMessage("c1", mid(7), "b1", model::rich_message::serializeForwarded("a link"), key, "2026-09-25T01:06:00.000Z");
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);

    const auto result = repo.fetchMessages(meta, kAlice);
    std::vector<std::string> ids;
    for (const auto& m : result.messages) ids.push_back(m.id);
    // Edit messages are folded away, forged ones included.
    CHECK(ids == std::vector<std::string>{mid(1), mid(4), mid(7)});

    const auto& edited = result.messages[0];
    CHECK(edited.text == "see you at 7");
    REQUIRE(edited.editedAtMs);
    CHECK(*edited.editedAtMs == *util::parseIsoMillis("2026-09-25T01:02:00.000Z"));
    REQUIRE(edited.earlierVersions.size() == 2);
    CHECK(edited.earlierVersions[0].text == "see you at 5");
    CHECK(edited.earlierVersions[1].text == "see you at 6");
    CHECK(edited.editMessageIds == std::vector<std::string>{mid(2), mid(3)});

    CHECK(result.messages[1].text == "ok");
    CHECK_FALSE(result.messages[1].editedAtMs);
    CHECK(result.messages[2].forwarded);
    // Bob's 1 and 7 are unread; his edits aren't counted as new messages.
    CHECK(result.unread == 2);

    repo.sendEdit(meta, mid(4), "  okay  ");
    const auto bodies = f.server.bodiesFor("conversations/c1/messages");
    REQUIRE(bodies.size() == 1);
    CHECK(crypto::decryptWithConversationKey(crypto::base64Decode(bodies[0]["ciphertext"].get<std::string>()),
                                             crypto::base64Decode(bodies[0]["nonce"].get<std::string>()), key) ==
          model::rich_message::serializeEdit("okay", mid(4)));
    CHECK_THROWS_AS(repo.sendEdit(meta, mid(4), "   "), std::invalid_argument);
}

TEST_CASE("polls tally each person's latest vote until the creator ends them", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("g1", f.alice, {&f.bob, &f.carol}, true, "Plans");
    auto& s = f.server;
    namespace rm = model::rich_message;
    s.addMessage("g1", mid(1), "b1", rm::serializePoll("Where?", {"Park", "Beach", "Cafe"}, false), key, "2026-09-25T01:00:00.000Z");
    s.addMessage("g1", mid(2), kAlice, rm::serializeVote(mid(1), {0}, {}), key, "2026-09-25T01:01:00.000Z");
    s.addMessage("g1", mid(3), "c1", rm::serializeVote(mid(1), {1}, {}), key, "2026-09-25T01:02:00.000Z");
    s.addMessage("g1", mid(4), kAlice, rm::serializeVote(mid(1), {2, 0}, {}), key, "2026-09-25T01:03:00.000Z");  // single choice: first counts
    s.addMessage("g1", mid(5), "c1", rm::serializeEndPoll(mid(1)), key, "2026-09-25T01:04:00.000Z");             // not the creator: ignored
    s.addMessage("g1", mid(6), "b1", rm::serializeEndPoll(mid(1)), key, "2026-09-25T01:05:00.000Z");
    s.addMessage("g1", mid(7), "c1", rm::serializeVote(mid(1), {2}, {}), key, "2026-09-25T01:06:00.000Z");       // too late
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);

    const auto result = repo.fetchMessages(meta, kAlice);
    REQUIRE(result.messages.size() == 1);
    const auto& poll = result.messages[0];
    CHECK(poll.kind == MessageKind::Poll);
    REQUIRE(poll.poll);
    CHECK(poll.poll->question == "Where?");
    CHECK(poll.poll->closed);
    CHECK(poll.poll->counts == std::vector<int>{0, 1, 1});
    CHECK(poll.poll->mine == std::vector<int>{2});
    CHECK(poll.poll->voters == 2);
    // Only the poll itself is new; votes and its end aren't messages of their own.
    CHECK(result.unread == 1);

    CHECK_THROWS_AS(rm::serializePoll("Q", {"only one"}, false), std::invalid_argument);
    CHECK_THROWS_AS(rm::serializePoll("Q", {"same", "same"}, false), std::invalid_argument);
    CHECK_THROWS_AS(rm::serializePoll("  ", {"a", "b"}, false), std::invalid_argument);
}

TEST_CASE("pins keep the newest three and show as notices", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    auto& s = f.server;
    namespace rm = model::rich_message;
    for (int i = 1; i <= 4; ++i) {
        s.addMessage("c1", mid(i), "b1", "note " + std::to_string(i), key, "2026-09-25T01:0" + std::to_string(i) + ":00.000Z");
    }
    s.addMessage("c1", mid(11), kAlice, rm::serializePin(mid(1), "note 1"), key, "2026-09-25T02:01:00.000Z");
    s.addMessage("c1", mid(12), "b1", rm::serializePin(mid(2), "note 2"), key, "2026-09-25T02:02:00.000Z");
    s.addMessage("c1", mid(13), kAlice, rm::serializePin(mid(3), "note 3"), key, "2026-09-25T02:03:00.000Z");
    s.addMessage("c1", mid(14), kAlice, rm::serializePin(mid(4), "note 4"), key, "2026-09-25T02:04:00.000Z");
    s.addMessage("c1", mid(15), "b1", rm::serializeUnpin(mid(3)), key, "2026-09-25T02:05:00.000Z");
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);

    const auto result = repo.fetchMessages(meta, kAlice);
    std::vector<std::string> pinned;
    for (const auto& p : result.pinned) pinned.push_back(p.messageId);
    // 1 dropped when 4 was pinned (three at most), then 3 unpinned.
    CHECK(pinned == std::vector<std::string>{mid(2), mid(4)});
    CHECK(result.pinned.back().preview == "note 4");
    // The four notes plus five notices; notices aren't unread.
    REQUIRE(result.messages.size() == 9);
    CHECK(result.messages[4].kind == MessageKind::Notice);
    CHECK(result.messages[4].notice == "pin");
    CHECK(result.messages[8].notice == "unpin");
    CHECK(result.unread == 4);

    // Deleted for everyone: its pin drops out, though the notice stays in the timeline.
    for (auto& m : s.messages["c1"]) {
        if (m["id"] != mid(4)) continue;
        m["deletedAt"] = "2026-09-25T03:00:00.000Z";
        m["ciphertext"] = nullptr;
        m["nonce"] = nullptr;
    }
    const auto after = repo.fetchMessages(meta, kAlice);
    REQUIRE(after.pinned.size() == 1);
    CHECK(after.pinned[0].messageId == mid(2));
}

TEST_CASE("custom reactions follow the quick six, in the order first used", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    auto& s = f.server;
    namespace rm = model::rich_message;
    s.addMessage("c1", mid(1), "b1", "hello", key, "2026-09-25T01:00:00.000Z");
    s.addMessage("c1", mid(2), "b1", rm::serializeReaction(mid(1), "🦄"), key, "2026-09-25T01:01:00.000Z");
    s.addMessage("c1", mid(3), kAlice, rm::serializeReaction(mid(1), "🎉"), key, "2026-09-25T01:02:00.000Z");
    s.addMessage("c1", mid(4), kAlice, rm::serializeReaction(mid(1), "🦄"), key, "2026-09-25T01:03:00.000Z");
    s.addMessage("c1", mid(5), "b1", rm::serializeReaction(mid(1), "🙏"), key, "2026-09-25T01:04:00.000Z");
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);

    const auto result = repo.fetchMessages(meta, kAlice);
    REQUIRE(result.messages.size() == 1);
    const auto& reactions = result.messages[0].reactions;
    REQUIRE(reactions.size() == 3);
    CHECK(reactions[0].emoji == "🙏");
    CHECK(reactions[1].emoji == "🦄");
    CHECK(reactions[1].count == 2);
    CHECK(reactions[1].mine);
    CHECK(reactions[2].emoji == "🎉");
    CHECK(result.unread == 1);
}

TEST_CASE("formats ISO timestamps the way the server parses them", "[util]") {
    CHECK(util::formatIsoMillis(0) == "1970-01-01T00:00:00.000Z");
    CHECK(util::formatIsoMillis(1790000000123) == "2026-09-21T14:13:20.123Z");
    CHECK(util::parseIsoMillis(util::formatIsoMillis(1790000000123)) == 1790000000123);
}

TEST_CASE("sends only ciphertext and checks the server's commitment", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);

    const auto sent = repo.send(meta, std::string("\0\0  hello bob  ", 15));
    CHECK(sent.text == "hello bob");
    CHECK(sent.fromMe);
    REQUIRE(sent.security);
    CHECK(sent.security->verification == VerificationStatus::Verified);
    CHECK(sent.security->localCommitment == sent.security->networkCommitment);
    CHECK(sent.deleteAtMs.has_value());

    const auto body = f.server.bodiesFor("conversations/c1/messages").back();
    CHECK(body.dump().find("hello") == std::string::npos);
    CHECK_FALSE(body.contains("attachments"));
    CHECK(crypto::decryptWithConversationKey(crypto::base64Decode(body["ciphertext"].get<std::string>()),
                                             crypto::base64Decode(body["nonce"].get<std::string>()), key) == "hello bob");

    f.server.tamperCommitments = true;
    CHECK(repo.send(meta, "again").security->verification == VerificationStatus::Failed);

    CHECK_THROWS_AS(repo.send(meta, "   "), std::invalid_argument);
}

TEST_CASE("replies and reactions travel inside the encrypted body", "[conversations]") {
    Fixture f;
    const auto key = f.server.addConversation("c1", f.alice, {&f.bob});
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);

    const auto reply = repo.send(meta, "yes", ReplyTarget{{mid(1), "b1", "coming?\n\n"}, false});
    REQUIRE(reply.replyTo);
    CHECK(reply.replyTo->preview == "coming?");
    repo.sendReaction(meta, mid(1), kThumbsUp);

    const auto bodies = f.server.bodiesFor("conversations/c1/messages");
    REQUIRE(bodies.size() == 2);
    auto open = [&](const Json& body) {
        return crypto::decryptWithConversationKey(crypto::base64Decode(body["ciphertext"].get<std::string>()),
                                                  crypto::base64Decode(body["nonce"].get<std::string>()), key);
    };
    CHECK(open(bodies[0]) == model::rich_message::serializeReply("yes", {mid(1), "b1", "coming?"}));
    CHECK(open(bodies[1]) == model::rich_message::serializeReaction(mid(1), kThumbsUp));
}

TEST_CASE("verify() is unavailable without a record, and disappearing sends null", "[conversations]") {
    Fixture f;
    f.server.addConversation("c1", f.alice, {&f.bob});
    ConversationRepository repo(f.api);

    Message old;
    old.id = mid(1);
    CHECK(repo.verify(old).verification == VerificationStatus::Unavailable);
    old.ciphertextB64 = "AA==";
    old.nonceB64 = "AA==";
    CHECK(repo.verify(old).verification == VerificationStatus::Unavailable);  // server has no record: 404

    repo.setDisappearing("c1", Disappearing::off());
    repo.setDisappearing("c1", Disappearing::fromSeconds(3600));
    const auto bodies = f.server.bodiesFor("conversations/c1/disappearing");
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0].dump() == R"({"seconds":null})");
    CHECK(bodies[1].dump() == R"({"seconds":3600})");
    CHECK(Disappearing::fromSeconds(42).label == "Off");
}

TEST_CASE("accepting a friend request seals one fresh key to both sides", "[friends]") {
    Fixture f;
    FriendRepository friends(f.api);
    const auto id = friends.accept(f.me, FriendRequestView{"r1", "b1", "bob", std::nullopt});
    CHECK(id == "c-accepted");

    const auto body = f.server.bodiesFor("friend-requests/r1/accept").at(0);
    const auto& participants = body["participants"];
    REQUIRE(participants.size() == 2);
    CHECK(participants[0]["userId"] == kAlice);
    CHECK(participants[1]["userId"] == "b1");
    const auto mine = crypto::unsealConversationKey(crypto::base64Decode(participants[0]["wrappedKey"].get<std::string>()),
                                                    f.alice.keys.publicKey, f.alice.keys.privateKey);
    const auto theirs = crypto::unsealConversationKey(crypto::base64Decode(participants[1]["wrappedKey"].get<std::string>()),
                                                      f.bob.keys.publicKey, f.bob.keys.privateKey);
    CHECK(copy(mine) == copy(theirs));
}

TEST_CASE("groups seal the key per member and encrypt the name", "[groups]") {
    Fixture f;
    GroupRepository groups(f.api);
    const api::FriendDto bob{"b1", "bob", f.bob.publicKeyB64(), std::nullopt};
    const api::FriendDto carol{"c1", "carol", f.carol.publicKeyB64(), std::nullopt};
    CHECK(groups.create(f.me, "  Book club ", {bob, carol}) == "g-new");

    const auto body = f.server.bodiesFor("conversations").at(0);
    CHECK(body["isGroup"] == true);
    CHECK(body.dump().find("Book club") == std::string::npos);
    REQUIRE(body["participants"].size() == 3);
    const auto carolsKey = crypto::unsealConversationKey(
        crypto::base64Decode(body["participants"][2]["wrappedKey"].get<std::string>()), f.carol.keys.publicKey,
        f.carol.keys.privateKey);
    CHECK(crypto::decryptWithConversationKey(crypto::base64Decode(body["nameCiphertext"].get<std::string>()),
                                             crypto::base64Decode(body["nameNonce"].get<std::string>()),
                                             carolsKey) == "Book club");

    // Adding a member seals the existing group key for them.
    const auto key = f.server.addConversation("g1", f.alice, {&f.bob}, true, "Book club");
    ConversationRepository repo(f.api);
    const auto meta = f.hydrate(repo);
    groups.add(meta, carol);
    const auto added = f.server.bodiesFor("conversations/g1/participants").at(0);
    CHECK(added["userId"] == "c1");
    CHECK(copy(crypto::unsealConversationKey(crypto::base64Decode(added["wrappedKey"].get<std::string>()),
                                             f.carol.keys.publicKey, f.carol.keys.privateKey)) == copy(key));

    groups.rename(meta, "Reading club");
    const auto renamed = f.server.bodiesFor("conversations/g1/name").at(0);
    CHECK(crypto::decryptWithConversationKey(crypto::base64Decode(renamed["nameCiphertext"].get<std::string>()),
                                             crypto::base64Decode(renamed["nameNonce"].get<std::string>()),
                                             key) == "Reading club");
}
