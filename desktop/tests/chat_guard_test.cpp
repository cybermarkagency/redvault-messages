// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The real ChatSession against the fake server: nothing goes out of a chat that can't take it. Each refusal is checked
// by counting what reached the server, next to a control showing the same call does send when nothing is wrong.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QSettings>
#include <QUrl>

#include <memory>
#include <mutex>
#include <set>

#include "app_test_support.hpp"
#include "chat_session.hpp"
#include "key_trust_fake.hpp"
#include "prefs.hpp"
#include "redvault/api/api_client.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/data/key_trust.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/net/cookie_jar.hpp"
#include "redvault/util/text.hpp"

using namespace redvault;
using redvault::app::ChatSession;
using redvault::data::KeyTrust;
using redvault::test::qt;
using redvault::test::settle;
using redvault::test::waitFor;
using namespace redvault::test::keytrust;
namespace refusal = redvault::app::device_key_refusal;

namespace {

/// ChatSession calls from its worker threads.
class LockedTransport final : public net::HttpTransport {
public:
    explicit LockedTransport(Server& server, std::string caller) : server_(server), caller_(std::move(caller)) {}
    net::HttpResponse send(const net::HttpRequest& request) override {
        std::lock_guard lock(mutex);
        server_.caller = caller_;
        return server_.handle(request);
    }
    std::mutex mutex;

private:
    Server& server_;
    std::string caller_;
};

constexpr const char* kPaused = "Sending is paused because this contact's safety number changed. Verify it first.";

struct Session {
    World w;
    std::shared_ptr<LockedTransport> transport;
    std::shared_ptr<api::ApiClient> api;
    std::shared_ptr<const data::SelfIdentity> identity;
    std::unique_ptr<ChatSession> chats;
    std::string bobsMessage, myMessage, pollId;

    /// What a signal said last.
    QString pollAnswer, forwardError;
    /// The chat's error line: the next successful poll clears it, so what matters is what it last said.
    QString shown;
    bool answered = false;
    bool editedOk = true, editAnswered = false;

    Session() {
        qt();
        QSettings().clear();
        transport = std::make_shared<LockedTransport>(w.server, w.alice.userId);
        api = std::make_shared<api::ApiClient>(kBase, transport, std::make_shared<net::SessionCookieJar>());
        identity = std::make_shared<const data::SelfIdentity>(w.alice.identity());
        const std::string now = util::formatIsoMillis(util::nowMillis());
        // Group: where the device-key refusals are tested. Direct chat: where the key pause is.
        for (Chat* chat : {&w.group, &w.dm}) {
            const crypto::SecretBytes& key = chat->isGroup ? w.groupKey : w.dmKey;
            const auto hello = crypto::encryptWithConversationKey("hello", key);
            chat->messages.push_back(w.server.message(*chat, w.bob.userId, base64Encode(hello.ciphertext), base64Encode(hello.nonce), std::nullopt, std::nullopt, now));
            const auto mine = crypto::encryptWithConversationKey("mine", key);
            chat->messages.push_back(w.server.message(*chat, w.alice.userId, base64Encode(mine.ciphertext), base64Encode(mine.nonce), std::nullopt, std::nullopt, now));
            const auto poll = crypto::encryptWithConversationKey(model::rich_message::serializePoll("Which?", {"one", "two"}, false), key);
            chat->messages.push_back(w.server.message(*chat, w.alice.userId, base64Encode(poll.ciphertext), base64Encode(poll.nonce), std::nullopt, std::nullopt, now));
        }
        chats = std::make_unique<ChatSession>(api, identity, nullptr);
        QObject::connect(chats.get(), &ChatSession::pollDone, [this](const QString& error) {
            pollAnswer = error;
            answered = true;
        });
        QObject::connect(chats.get(), &ChatSession::forwardDone, [this](int, const QString& error) {
            forwardError = error;
            answered = true;
        });
        QObject::connect(chats.get(), &ChatSession::errorChanged, [this] {
            if (!chats->error().isEmpty()) shown = chats->error();
        });
        QObject::connect(chats.get(), &ChatSession::edited, [this](bool ok) {
            editedOk = ok;
            editAnswered = true;
        });
        REQUIRE(waitFor([this] { return chats->metaFor(kGroupId) && chats->metaFor(kDmId); }));
    }

    ~Session() {
        chats.reset();
        settle(20);
    }

    /// Opens a chat and waits for its messages. The ids depend on what the server numbered.
    void open(const Chat& chat) {
        chats->openChat(QString::fromStdString(chat.id));
        bobsMessage = chat.messages[0]["id"].get<std::string>();
        myMessage = chat.messages[1]["id"].get<std::string>();
        pollId = chat.messages[2]["id"].get<std::string>();
        REQUIRE(waitFor([this] { return chats->textOf(QString::fromStdString(bobsMessage)) == QStringLiteral("hello"); }));
    }

    int posted(const std::string& chatId) {
        std::lock_guard lock(transport->mutex);
        return w.server.sent("POST conversations/" + chatId + "/messages");
    }

    int requests(const std::string& fragment) {
        std::lock_guard lock(transport->mutex);
        return w.server.sent(fragment);
    }

    QString id(const std::string& text) const { return QString::fromStdString(text); }
};

/// Every one-tap way of sending into the open chat.
enum class Action { Send, React, Edit, Pin, Unpin, Poll, Vote, EndPoll };

constexpr const char* name(Action a) {
    switch (a) {
        case Action::Send: return "send";
        case Action::React: return "react";
        case Action::Edit: return "edit";
        case Action::Pin: return "pin";
        case Action::Unpin: return "unpin";
        case Action::Poll: return "poll";
        case Action::Vote: return "vote";
        case Action::EndPoll: return "end poll";
    }
    return "";
}

void run(Session& s, Action action) {
    switch (action) {
        case Action::Send: s.chats->sendMessage(QStringLiteral("a message")); break;
        case Action::React: s.chats->react(s.id(s.bobsMessage), QString::fromUtf8("\xF0\x9F\x91\x8D")); break;
        case Action::Edit: s.chats->editMessage(s.id(s.myMessage), QStringLiteral("changed")); break;
        case Action::Pin: s.chats->pin(s.id(s.bobsMessage)); break;
        case Action::Unpin: s.chats->unpin(s.id(s.bobsMessage)); break;
        case Action::Poll: s.chats->createPoll(QStringLiteral("Which one?"), {QStringLiteral("a"), QStringLiteral("b")}, false); break;
        case Action::Vote: s.chats->vote(s.id(s.pollId), {QVariant(0)}); break;
        case Action::EndPoll: s.chats->endPoll(s.id(s.pollId)); break;
    }
}

std::string expectedOnDeviceKeys(Action action) {
    switch (action) {
        case Action::Send: return refusal::kMessage;
        case Action::React:
        case Action::Edit: return refusal::kChange;
        case Action::Pin:
        case Action::Unpin: return refusal::kPin;
        case Action::Poll: return refusal::kPoll;
        case Action::Vote:
        case Action::EndPoll: return refusal::kVote;
    }
    return {};
}

/// Polls answer through their dialog; everything else shows in the chat.
std::string shownFor(Session& s, Action action) {
    return (action == Action::Poll ? s.pollAnswer : s.shown).toStdString();
}

const auto kActions = {Action::Send, Action::React, Action::Edit, Action::Pin, Action::Unpin, Action::Poll, Action::Vote, Action::EndPoll};

}  // namespace

TEST_CASE("control: every way of sending reaches the server when nothing is wrong", "[app][chatguard]") {
    const Action action = GENERATE(values(kActions));
    CAPTURE(name(action));
    Session s;
    s.open(s.w.group);
    const int before = s.posted(kGroupId);
    run(s, action);
    CHECK(waitFor([&] { return s.posted(kGroupId) > before; }, 5000));
    settle(50);
    CHECK(s.shown.isEmpty());
}

TEST_CASE("a chat pinned to device keys refuses every older-protocol send, with a reason", "[app][chatguard]") {
    const Action action = GENERATE(values(kActions));
    CAPTURE(name(action));
    Session s;
    s.open(s.w.group);
    s.chats->setDeviceKeyCheck([](const std::string& chatId) { return chatId == kGroupId; });
    const int before = s.posted(kGroupId);
    run(s, action);
    settle();
    CHECK(s.posted(kGroupId) == before);
    CHECK(shownFor(s, action) == expectedOnDeviceKeys(action));
}

TEST_CASE("a vote refused on device keys doesn't look as if it counted", "[app][chatguard]") {
    Session s;
    s.open(s.w.group);
    s.chats->setDeviceKeyCheck([](const std::string& chatId) { return chatId == kGroupId; });
    s.chats->vote(s.id(s.pollId), {QVariant(0)});
    settle();
    CHECK(s.shown.toStdString() == refusal::kVote);
    CHECK(s.posted(kGroupId) == 0);
}

TEST_CASE("a file into a pinned chat stops before anything is uploaded", "[app][chatguard]") {
    Session s;
    s.open(s.w.group);
    s.chats->setDeviceKeyCheck([](const std::string& chatId) { return chatId == kGroupId; });
    s.chats->attachFile(QUrl::fromLocalFile(QStringLiteral("/nonexistent/photo.jpg")));
    settle();
    CHECK(s.shown.toStdString() == refusal::kMessage);
    CHECK(s.requests("v1/files") == 0);
}

TEST_CASE("forwarding into a pinned chat is refused before anything goes out", "[app][chatguard]") {
    Session s;
    s.open(s.w.dm);
    s.chats->setDeviceKeyCheck([](const std::string& chatId) { return chatId == kGroupId; });
    const int intoGroup = s.posted(kGroupId);
    const int intoDm = s.posted(kDmId);
    SECTION("messages: one good target and one pinned one") {
        s.chats->forwardMessages({s.id(s.bobsMessage)}, {s.id(kDmId), s.id(kGroupId)});
    }
    SECTION("a channel post") {
        s.chats->forwardChannelPost(QStringLiteral("a post"), s.id(kDmId), s.id(kDmId), QStringLiteral("News"), {s.id(kDmId), s.id(kGroupId)});
    }
    REQUIRE(waitFor([&] { return s.answered; }, 5000));
    CHECK(s.forwardError.toStdString() == refusal::kForward);
    settle();
    CHECK(s.posted(kGroupId) == intoGroup);
    CHECK(s.posted(kDmId) == intoDm);
}

TEST_CASE("control: forwarding reaches a chat that isn't pinned", "[app][chatguard]") {
    Session s;
    s.open(s.w.dm);
    const int before = s.posted(kGroupId);
    s.chats->forwardMessages({s.id(s.bobsMessage)}, {s.id(kGroupId)});
    CHECK(waitFor([&] { return s.posted(kGroupId) > before; }, 5000));
    CHECK(s.forwardError.isEmpty());
}

TEST_CASE("a changed contact key pauses every way of sending in a direct chat", "[app][chatguard]") {
    const Action action = GENERATE(values(kActions));
    CAPTURE(name(action));
    Session s;
    s.open(s.w.dm);
    // What this computer pinned for Bob no longer matches what the server says.
    QSettings().setValue(QStringLiteral("pinnedKeys/") + s.id(s.w.alice.userId) + u'_' + s.id(s.w.bob.userId), QStringLiteral("00ff"));
    REQUIRE(s.chats->currentKeyChanged());
    const int before = s.posted(kDmId);
    run(s, action);
    settle();
    CHECK(s.posted(kDmId) == before);
    CHECK(shownFor(s, action) == kPaused);
}

TEST_CASE("forwarding into a chat whose contact key changed is paused too", "[app][chatguard]") {
    Session s;
    s.open(s.w.group);
    QSettings().setValue(QStringLiteral("pinnedKeys/") + s.id(s.w.alice.userId) + u'_' + s.id(s.w.bob.userId), QStringLiteral("00ff"));
    const int before = s.posted(kDmId);
    s.chats->forwardMessages({s.id(s.bobsMessage)}, {s.id(kDmId)});
    REQUIRE(waitFor([&] { return s.answered; }, 5000));
    CHECK(s.forwardError.toStdString() == kPaused);
    CHECK(s.posted(kDmId) == before);
}

TEST_CASE("a refused key says why in the chat, and every way of sending stops", "[app][chatguard]") {
    const Action action = GENERATE(values(kActions));
    CAPTURE(name(action));
    Session s;
    s.open(s.w.dm);
    REQUIRE(s.chats->currentKeyProblem().isEmpty());
    bool told = false;
    QObject::connect(s.chats.get(), &ChatSession::keyProblemChanged, [&] { told = true; });
    {
        std::lock_guard lock(s.transport->mutex);
        s.w.dm.keyEpoch = 1;
        s.w.dm.seal(s.w.alice, 1, crypto::generateConversationKey());
    }
    s.chats->refreshNow();
    REQUIRE(waitFor([&] { return !s.chats->currentKeyProblem().isEmpty() && told; }));
    CHECK(s.chats->currentKeyProblem().toStdString() == KeyTrust::kDirectKeyChanged);
    const int before = s.posted(kDmId);
    run(s, action);
    settle();
    CHECK(s.posted(kDmId) == before);
    CHECK(shownFor(s, action) == KeyTrust::kDirectKeyChanged);
}

TEST_CASE("a file into a chat whose key was refused stops before anything is uploaded", "[app][chatguard]") {
    Session s;
    s.open(s.w.dm);
    {
        std::lock_guard lock(s.transport->mutex);
        s.w.dm.keyEpoch = 1;
        s.w.dm.seal(s.w.alice, 1, crypto::generateConversationKey());
    }
    s.chats->refreshNow();
    REQUIRE(waitFor([&] { return !s.chats->currentKeyProblem().isEmpty(); }));
    s.chats->attachFile(QUrl::fromLocalFile(QStringLiteral("/nonexistent/photo.jpg")));
    settle();
    // Said at once by the guard: the send itself would only refuse after the upload.
    CHECK(s.shown.toStdString() == KeyTrust::kDirectKeyChanged);
    CHECK(s.requests("v1/files") == 0);
}

TEST_CASE("forwarding into a chat whose key was refused is refused before anything goes out", "[app][chatguard]") {
    Session s;
    s.open(s.w.group);
    {
        std::lock_guard lock(s.transport->mutex);
        s.w.dm.keyEpoch = 1;
        s.w.dm.seal(s.w.alice, 1, crypto::generateConversationKey());
    }
    s.chats->refreshNow();
    REQUIRE(waitFor([&] { return s.chats->sendRefusal(kDmId, refusal::kForward).toStdString() == KeyTrust::kDirectKeyChanged; }));
    const int intoGroup = s.posted(kGroupId);
    s.chats->forwardMessages({s.id(s.bobsMessage)}, {s.id(kGroupId), s.id(kDmId)});
    REQUIRE(waitFor([&] { return s.answered; }, 5000));
    CHECK(s.forwardError.toStdString() == KeyTrust::kDirectKeyChanged);
    settle();
    CHECK(s.posted(kGroupId) == intoGroup);  // not half done
}

TEST_CASE("a new chat refuses to seal to a swapped key", "[app][chatguard][seal]") {
    Session s;
    const std::string self = s.w.alice.userId;
    REQUIRE_FALSE(redvault::app::prefs::contactKeyPinStore()->keyChanged(self, s.w.eve.userId, base64Encode(s.w.eve.keys.publicKey)));
    {
        std::lock_guard lock(s.transport->mutex);
        s.w.server.forgedPublicKeys[s.w.eve.userId] = base64Encode(crypto::generateKeyPair().publicKey);
    }
    QString answer;
    bool done = false;
    QObject::connect(s.chats.get(), &ChatSession::groupActionDone, [&](const QString& error) {
        answer = error;
        done = true;
    });
    s.chats->startMessageRequest(QString::fromStdString(s.w.eve.userId));
    REQUIRE(waitFor([&] { return done; }));
    CHECK(answer.toStdString() == KeyTrust::kContactKeyChanged);
    CHECK(s.requests("POST conversations ") == 0);
}

TEST_CASE("control: a new chat seals to a key seen before", "[app][chatguard][seal]") {
    Session s;
    REQUIRE_FALSE(redvault::app::prefs::contactKeyPinStore()->keyChanged(s.w.alice.userId, s.w.eve.userId, base64Encode(s.w.eve.keys.publicKey)));
    QString answer = QStringLiteral("not answered");
    bool done = false;
    QObject::connect(s.chats.get(), &ChatSession::groupActionDone, [&](const QString& error) {
        answer = error;
        done = true;
    });
    s.chats->startMessageRequest(QString::fromStdString(s.w.eve.userId));
    REQUIRE(waitFor([&] { return done; }));
    CHECK(answer.isEmpty());
    CHECK(s.requests("POST conversations ") == 1);
}

TEST_CASE("a pinned chat the server calls a community stays an ordinary chat, with its checks", "[app][chatguard][community]") {
    Session s;
    s.open(s.w.dm);
    REQUIRE(s.chats->currentKeyProblem().isEmpty());
    {
        std::lock_guard lock(s.transport->mutex);
        s.w.dm.keyEpoch = 1;
        s.w.dm.seal(s.w.alice, 1, crypto::generateConversationKey());
        s.w.dm.community = Json{{"id", "x"}, {"name", "x"}, {"kind", "group"}, {"status", "active"}, {"role", "member"}, {"canPost", true}, {"keyEpoch", 0}, {"rekeyRequired", false}};
    }
    s.chats->refreshNow();
    REQUIRE(waitFor([&] { return !s.chats->currentKeyProblem().isEmpty(); }));
    CHECK(s.chats->currentKeyProblem().toStdString() == KeyTrust::kDirectKeyChanged);
    const int before = s.posted(kDmId);
    s.chats->sendMessage(QStringLiteral("a message"));
    settle();
    CHECK(s.posted(kDmId) == before);
}

TEST_CASE("an unrefused chat shows no key problem", "[app][chatguard]") {
    Session s;
    s.open(s.w.group);
    CHECK(s.chats->currentKeyProblem().isEmpty());
    CHECK(s.chats->sendRefusal(kGroupId, refusal::kMessage).isEmpty());
}
