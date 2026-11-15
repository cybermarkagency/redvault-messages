// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include <catch2/catch_test_macros.hpp>
#include <sodium.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/util/text.hpp"

// Part A on device keys (docs/signal-integration/08-part-a-chat-features.md):
// replies, forwards, polls, pins, receipts and disappearing messages, against
// the cases every client shares (Android loads the same file), plus the
// receipt batching and the erasure of expired messages from the history file.

namespace e2 = redvault::e2ev3;
namespace fs = std::filesystem;
using e2::Json;

namespace {

Json partA() {
    std::ifstream in(REDVAULT_V3_PART_A_VECTORS, std::ios::binary);
    REQUIRE(in.good());
    return Json::parse(in);
}

const std::string kChat = "11111111-1111-4111-8111-111111111111";

/// One vector record as the history stores it.
e2::ChatRecord toRecord(const Json& v, std::int64_t arrivedAt, const std::string& conversation) {
    e2::ChatRecord r;
    r.conversationId = conversation;
    r.id = v.at("id").get<std::string>();
    r.from = v.at("from").get<std::string>();
    r.fromDeviceId = "d";
    r.ts = v.at("ts").get<std::int64_t>();
    r.arrivedAt = arrivedAt;
    r.direction = "in";
    r.status = "received";
    const std::string t = v.at("t").get<std::string>();
    if (t == "text" || t == "poll") {
        r.text = t == "poll" ? v.at("question").get<std::string>() : v.at("body").get<std::string>();
        if (t == "poll") r.poll = e2::ChatPoll{v.at("options").get<std::vector<std::string>>(), v.at("multi").get<bool>(), {}, false, {}};
        if (v.contains("reply")) {
            r.replyTo = v.at("reply").at("target").get<std::string>();
            r.replyPreview = v.at("reply").value("preview", std::string());
        }
        r.forwarded = v.value("fwd", false);
        r.exp = v.value("exp", 0);
        return r;
    }
    e2::ChatOp op{t, v.value("target", std::string()), v.value("body", std::string()), v.value("emoji", std::string())};
    if (v.contains("choices")) op.choices = v.at("choices").get<std::vector<int>>();
    op.pinned = v.value("pinned", false);
    if (v.contains("targets")) op.targets = v.at("targets").get<std::vector<std::string>>();
    op.status = v.value("status", std::string());
    r.op = op;
    return r;
}

/// Every field the vectors compare, in their shape.
Json shown(const std::vector<e2::ChatRecord>& folded) {
    Json out = Json::array();
    for (const e2::ChatRecord& m : folded) {
        Json reactions = Json::array();
        for (const e2::ChatReaction& r : m.reactions) reactions.push_back({{"user", r.userId}, {"emoji", r.emoji}});
        Json receipts = Json::array();
        for (const e2::ChatReceipt& r : m.receipts) receipts.push_back({{"user", r.userId}, {"status", r.status}});
        Json poll = nullptr;
        if (m.poll) poll = {{"options", m.poll->options}, {"multi", m.poll->multi}, {"votes", m.poll->votes}, {"ended", m.poll->ended}};
        out.push_back({{"id", m.id},
                       {"text", m.text},
                       {"edited", m.edited},
                       {"deleted", m.deleted},
                       {"reactions", reactions},
                       {"replyTo", m.replyTo.empty() ? Json(nullptr) : Json(m.replyTo)},
                       {"forwarded", m.forwarded},
                       {"pinned", m.pinned},
                       {"poll", poll},
                       {"receipts", receipts},
                       {"expiresAt", m.expiresAt ? Json(*m.expiresAt) : Json(nullptr)}});
    }
    return out;
}

std::optional<std::int64_t> nowOf(const Json& c) {
    return c.contains("now") && !c.at("now").is_null() ? std::optional<std::int64_t>(c.at("now").get<std::int64_t>()) : std::nullopt;
}

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("rv-e2ev3-parta-" + redvault::util::randomUuid());
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

std::string decryptFile(const fs::path& path, const redvault::crypto::Bytes& key) {
    std::ifstream in(path, std::ios::binary);
    const std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t nonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    REQUIRE(blob.size() > nonce);
    std::string plain(blob.size() - nonce, '\0');
    unsigned long long length = 0;
    const auto* data = reinterpret_cast<const unsigned char*>(blob.data());
    REQUIRE(crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()), &length, nullptr, data + nonce,
                                                       blob.size() - nonce, nullptr, 0, data, key.data()) == 0);
    plain.resize(length);
    return plain;
}

e2::ChatRecord text(const std::string& id, const std::string& from, std::int64_t ts, const std::string& body) {
    return e2::ChatRecord{kChat, id, from, "d", ts, ts, body, "in", "received"};
}

e2::ChatRecord opRecord(const std::string& id, const std::string& from, std::int64_t ts, const e2::ChatOp& op) {
    auto r = text(id, from, ts, "");
    r.op = op;
    return r;
}

std::string idN(int n) {
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "cccccccc-0000-4000-8000-%012d", n);
    return buffer;
}

}  // namespace

TEST_CASE("part A vectors: every fold case, in both arrival orders", "[e2ev3][content][parta]") {
    const Json v = partA();
    REQUIRE(v.at("fold").size() == 21);
    for (const Json& c : v.at("fold")) {
        INFO(c.at("name").get<std::string>());
        std::vector<e2::ChatRecord> records;
        std::int64_t arrived = 0;
        for (const Json& record : c.at("records")) records.push_back(toRecord(record, arrived++, v.at("conversation").get<std::string>()));
        CHECK(shown(e2::foldChat(records, nowOf(c))) == c.at("expect"));
        std::reverse(records.begin(), records.end());
        CHECK(shown(e2::foldChat(records, nowOf(c))) == c.at("expect"));
    }
}

TEST_CASE("part A vectors: every fold case against the real history, in both arrival orders", "[e2ev3][content][parta]") {
    const Json v = partA();
    const auto key = redvault::crypto::randomBytes(32);
    for (const Json& c : v.at("fold")) {
        INFO(c.at("name").get<std::string>());
        std::vector<e2::ChatRecord> records;
        std::int64_t arrived = 0;
        for (const Json& record : c.at("records")) records.push_back(toRecord(record, arrived++, v.at("conversation").get<std::string>()));
        const std::int64_t now = nowOf(c).value_or(0);  // 0: nothing expires
        for (const bool reversed : {false, true}) {
            TempDir dir;
            if (reversed) std::reverse(records.begin(), records.end());
            e2::ChatHistory history(dir.path / "h", key);
            history.setClock([now] { return now; });
            for (const e2::ChatRecord& r : records) REQUIRE(history.add(r));
            CHECK(shown(history.list(v.at("conversation").get<std::string>())) == c.at("expect"));
            e2::ChatHistory reopened(dir.path / "h", key);  // what was stored folds the same after a restart
            reopened.setClock([now] { return now; });
            CHECK(shown(reopened.list(v.at("conversation").get<std::string>())) == c.at("expect"));
        }
    }
}

TEST_CASE("part A vectors: every decode case, with now = 2000", "[e2ev3][content][parta]") {
    const Json v = partA();
    REQUIRE(v.at("decode").size() == 24);
    for (const Json& c : v.at("decode")) {
        INFO(c.at("name").get<std::string>());
        const auto decoded = e2::decodeEnvelope(c.at("envelope").dump(), v.at("conversation").get<std::string>(), 2000);
        const std::string accept = c.at("accept").get<std::string>();
        const auto expected = accept == "op"        ? e2::DecodedEnvelope::Kind::Op
                              : accept == "message" ? e2::DecodedEnvelope::Kind::Message
                                                    : e2::DecodedEnvelope::Kind::Ignore;
        CHECK(decoded.kind == expected);
    }
}

TEST_CASE("part A envelopes round-trip through encode and decode", "[e2ev3][content][parta]") {
    const std::string id = idN(1);
    const std::string target = idN(2);
    e2::Envelope e;
    e.id = id;
    e.c = kChat;
    e.ts = 1000;
    e.body = "yes";
    e.extras = e2::MessageExtras{target, "hi?", true, 3600};
    const auto reply = e2::decodeEnvelope(e2::encodeEnvelope(e), kChat, 2000);
    REQUIRE(reply.kind == e2::DecodedEnvelope::Kind::Message);
    CHECK(reply.extras.replyTo == target);
    CHECK(reply.extras.replyPreview == "hi?");
    CHECK(reply.extras.forwarded);
    CHECK(reply.extras.exp == 3600);

    const auto poll = e2::decodeEnvelope(e2::encodePoll(kChat, "Lunch?", {"A", "B"}, true, id, 1000, e2::MessageExtras{{}, {}, false, 60}), kChat, 2000);
    REQUIRE(poll.kind == e2::DecodedEnvelope::Kind::Message);
    CHECK(poll.text == "Lunch?");
    REQUIRE(poll.poll.has_value());
    CHECK(poll.poll->options == std::vector<std::string>{"A", "B"});
    CHECK(poll.poll->multi);
    CHECK(poll.extras.exp == 60);

    e2::ChatOp vote{"vote", target, {}, {}, {2, 0}};
    const auto v = e2::decodeEnvelope(e2::encodeOp(kChat, vote, id, 1000), kChat, 2000);
    REQUIRE(v.kind == e2::DecodedEnvelope::Kind::Op);
    CHECK(v.op->choices == std::vector<int>{2, 0});
    e2::ChatOp pin{"pin", target};
    pin.pinned = true;
    CHECK(e2::decodeEnvelope(e2::encodeOp(kChat, pin, id, 1000), kChat, 2000).op->pinned);
    e2::ChatOp receipt;
    receipt.t = "receipt";
    receipt.targets = {target, idN(3)};
    receipt.status = "read";
    const auto r = e2::decodeEnvelope(e2::encodeOp(kChat, receipt, id, 1000), kChat, 2000);
    REQUIRE(r.kind == e2::DecodedEnvelope::Kind::Op);
    CHECK(r.op->targets == receipt.targets);
    CHECK(r.op->status == "read");
    CHECK(e2::decodeEnvelope(e2::encodeOp(kChat, e2::ChatOp{"end", target}, id, 1000), kChat, 2000).kind == e2::DecodedEnvelope::Kind::Op);

    // What wouldn't decode isn't made.
    CHECK_THROWS_AS(e2::encodePoll(kChat, "Q", {"A"}, false, id, 1000), e2::EngineError);
    CHECK_THROWS_AS(e2::encodePoll(kChat, "Q", {"A", "A"}, false, id, 1000), e2::EngineError);
    CHECK_THROWS_AS(e2::encodePoll(kChat, "", {"A", "B"}, false, id, 1000), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeOp(kChat, e2::ChatOp{"vote", target, {}, {}, {1, 1}}, id, 1000), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeOp(kChat, e2::ChatOp{"vote", target, {}, {}, {10}}, id, 1000), e2::EngineError);
    receipt.status = "seen";
    CHECK_THROWS_AS(e2::encodeOp(kChat, receipt, id, 1000), e2::EngineError);
    receipt.status = "read";
    receipt.targets.assign(101, target);
    CHECK_THROWS_AS(e2::encodeOp(kChat, receipt, id, 1000), e2::EngineError);
    e.extras = e2::MessageExtras{"nope", "", false, 0};
    CHECK_THROWS_AS(e2::encodeEnvelope(e), e2::EngineError);
    e.extras = e2::MessageExtras{{}, {}, false, 2419201};
    CHECK_THROWS_AS(e2::encodeEnvelope(e), e2::EngineError);
}

TEST_CASE("part A decoding: characters are UTF-16 units, and the other malformed fields", "[e2ev3][content][parta]") {
    using Kind = e2::DecodedEnvelope::Kind;
    const auto base = [](Json extra) {
        Json e = {{"v", 1}, {"t", "text"}, {"id", idN(1)}, {"c", kChat}, {"ts", 1000}, {"body", "x"}};
        for (auto it = extra.begin(); it != extra.end(); ++it) e[it.key()] = it.value();
        return e.dump();
    };
    // 100 emoji are 200 UTF-16 units: the most a preview holds. One more is too many.
    std::string emoji;
    for (int i = 0; i < 100; ++i) emoji += "\xF0\x9F\x91\x8D";
    CHECK(e2::textLength(emoji) == 200);
    CHECK(e2::decodeEnvelope(base({{"reply", {{"target", idN(2)}, {"preview", emoji}}}}), kChat, 2000).kind == Kind::Message);
    CHECK(e2::decodeEnvelope(base({{"reply", {{"target", idN(2)}, {"preview", emoji + "x"}}}}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::previewText(emoji + "more") == emoji);
    CHECK(e2::decodeEnvelope(base({{"reply", "nope"}}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(base({{"reply", nullptr}}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(base({{"reply", {{"target", idN(2)}}}}), kChat, 2000).kind == Kind::Ignore);  // the preview is required
    CHECK(e2::decodeEnvelope(base({{"reply", {{"target", idN(2)}, {"preview", ""}}}}), kChat, 2000).kind == Kind::Message);
    CHECK(e2::decodeEnvelope(base({{"choices", {1}}}), kChat, 2000).kind == Kind::Message);  // a field this type doesn't take: ignored
    // The same receipt target twice is merged.
    const Json twice = {{"v", 1}, {"t", "receipt"}, {"id", idN(1)}, {"c", kChat}, {"ts", 1000}, {"targets", {idN(2), idN(2)}}, {"status", "read"}};
    const auto merged = e2::decodeEnvelope(twice.dump(), kChat, 2000);
    REQUIRE(merged.kind == Kind::Op);
    CHECK(merged.op->targets == std::vector<std::string>{idN(2)});
    const Json nullPin = {{"v", 1}, {"t", "pin"}, {"id", idN(1)}, {"c", kChat}, {"ts", 1000}, {"target", idN(2)}, {"pinned", nullptr}};
    CHECK(e2::decodeEnvelope(nullPin.dump(), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(base({{"fwd", false}}), kChat, 2000).kind == Kind::Message);
    CHECK(e2::decodeEnvelope(base({{"fwd", nullptr}}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(base({{"exp", 0}}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(base({{"exp", 1}}), kChat, 2000).kind == Kind::Message);
    CHECK(e2::decodeEnvelope(base({{"exp", 2419200}}), kChat, 2000).kind == Kind::Message);
    CHECK(e2::decodeEnvelope(base({{"exp", 1.5}}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(base({{"exp", "60"}}), kChat, 2000).kind == Kind::Ignore);
    // A vote's choices are integers; a boolean or a number out of range spoils it.
    const auto vote = [](Json choices) {
        return Json{{"v", 1}, {"t", "vote"}, {"id", idN(1)}, {"c", kChat}, {"ts", 1000}, {"target", idN(2)}, {"choices", choices}}.dump();
    };
    CHECK(e2::decodeEnvelope(vote(Json::array()), kChat, 2000).kind == Kind::Op);
    CHECK(e2::decodeEnvelope(vote({true}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(vote({10}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(vote({-1}), kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(vote({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}), kChat, 2000).kind == Kind::Op);
    // A poll without multi, or with an option too long.
    const Json poll = {{"v", 1}, {"t", "poll"}, {"id", idN(1)}, {"c", kChat}, {"ts", 1000}, {"question", "Q"}, {"options", {"A", "B"}}};
    CHECK(e2::decodeEnvelope(poll.dump(), kChat, 2000).kind == Kind::Ignore);
    Json longOption = poll;
    longOption["multi"] = false;
    longOption["options"] = {"A", std::string(101, 'b')};
    CHECK(e2::decodeEnvelope(longOption.dump(), kChat, 2000).kind == Kind::Ignore);
}

TEST_CASE("part A: an expired message is erased from the history file with its file key and the ops on it", "[e2ev3][content][parta]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    const fs::path file = dir.path / "h";
    std::int64_t now = 2000;
    e2::ChatHistory history(file, key);
    history.setClock([&now] { return now; });
    auto gone = text(idN(1), "alice", 1000, "the secret words");
    gone.exp = 60;
    gone.replyTo = idN(9);
    gone.replyPreview = "what it answers";
    REQUIRE(history.add(gone));
    REQUIRE(history.add(text(idN(2), "bob", 1500, "stays")));
    REQUIRE(history.add(opRecord(idN(3), "bob", 2000, e2::ChatOp{"reaction", idN(1), {}, "x"})));
    e2::ChatOp receipt;
    receipt.t = "receipt";
    receipt.targets = {idN(1), idN(2)};
    receipt.status = "read";
    REQUIRE(history.add(opRecord(idN(4), "carol", 2000, receipt)));
    REQUIRE(history.nextExpiry() == 61000);
    REQUIRE(history.list(kChat).size() == 2);
    CHECK(history.list(kChat)[0].expiresAt == 61000);

    now = 61000;
    CHECK(history.list(kChat).size() == 1);  // already left out of what's shown
    REQUIRE(history.eraseExpired());
    CHECK_FALSE(history.eraseExpired());  // nothing more to do
    const std::string after = decryptFile(file, key);
    CHECK(after.find("the secret words") == std::string::npos);
    CHECK(after.find("what it answers") == std::string::npos);
    CHECK(after.find(idN(3)) == std::string::npos);  // the reaction on it
    const auto stored = history.records(kChat);
    const auto receiptLeft = std::find_if(stored.begin(), stored.end(), [](const e2::ChatRecord& r) { return r.op && r.op->t == "receipt"; });
    REQUIRE(receiptLeft != stored.end());
    CHECK(receiptLeft->op->targets == std::vector<std::string>{idN(2)});  // still covers the message that stays
    CHECK_FALSE(history.nextExpiry().has_value());

    // A late copy of the message (an own-device copy, a retry) is never stored again; a late op goes too.
    CHECK_FALSE(history.add(gone));
    REQUIRE(history.add(opRecord(idN(5), "bob", 3000, e2::ChatOp{"reaction", idN(1), {}, "y"})));
    CHECK(decryptFile(file, key).find(idN(5)) == std::string::npos);
    const auto shownNow = history.list(kChat);
    REQUIRE(shownNow.size() == 1);
    CHECK(shownNow[0].text == "stays");
    REQUIRE(shownNow[0].receipts.size() == 1);
    CHECK(shownNow[0].receipts[0].userId == "carol");
}

TEST_CASE("part A: an own message that expires while waiting for Tor never leaves", "[e2ev3][content][parta]") {
    TempDir dir;
    std::int64_t now = 1000;
    e2::ChatHistory history(dir.path / "h", redvault::crypto::randomBytes(32));
    history.setClock([&now] { return now; });
    auto queued = text(idN(1), "alice", 1000, "soon");
    queued.direction = "out";
    queued.status = "queued";
    queued.exp = 1;
    REQUIRE(history.add(queued));
    REQUIRE(history.queued().size() == 1);
    now = 2000;
    REQUIRE(history.eraseExpired());
    CHECK(history.queued().empty());
    CHECK(history.records(kChat).empty());
}

TEST_CASE("part A: deleting a poll erases its votes, and a deleted message's pin and quote", "[e2ev3][content][parta]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    e2::ChatHistory history(dir.path / "h", key);
    auto poll = text(idN(1), "alice", 1000, "Secret question?");
    poll.poll = e2::ChatPoll{{"Option one", "Option two"}, false, {}, false, {}};
    REQUIRE(history.add(poll));
    REQUIRE(history.add(opRecord(idN(2), "bob", 2000, e2::ChatOp{"vote", idN(1), {}, {}, {1}})));
    e2::ChatOp pin{"pin", idN(1)};
    pin.pinned = true;
    REQUIRE(history.add(opRecord(idN(3), "bob", 2100, pin)));
    REQUIRE(history.list(kChat).at(0).pinned);
    REQUIRE(history.add(opRecord(idN(4), "alice", 3000, e2::ChatOp{"delete", idN(1)})));
    const std::string after = decryptFile(dir.path / "h", key);
    CHECK(after.find("Secret question?") == std::string::npos);
    CHECK(after.find("Option one") == std::string::npos);
    CHECK(after.find(idN(2)) == std::string::npos);
    CHECK(after.find(idN(3)) == std::string::npos);
    const auto shownNow = history.list(kChat);
    REQUIRE(shownNow.size() == 1);
    CHECK(shownNow[0].deleted);
    CHECK_FALSE(shownNow[0].poll.has_value());
    CHECK_FALSE(shownNow[0].pinned);
}

TEST_CASE("part A: a poll's ballots say who voted what, so the app can show your own vote", "[e2ev3][content][parta]") {
    auto poll = text(idN(1), "alice", 1000, "Q?");
    poll.poll = e2::ChatPoll{{"A", "B", "C"}, true, {}, false, {}};
    const auto folded = e2::foldChat({poll, opRecord(idN(2), "bob", 2000, e2::ChatOp{"vote", idN(1), {}, {}, {0, 2}}),
                                      opRecord(idN(3), "alice", 2500, e2::ChatOp{"vote", idN(1), {}, {}, {1}}),
                                      opRecord(idN(4), "carol", 2600, e2::ChatOp{"vote", idN(1), {}, {}, {}})});
    REQUIRE(folded.at(0).poll.has_value());
    const auto& ballots = folded[0].poll->ballots;
    REQUIRE(ballots.size() == 2);  // carol took hers back
    CHECK(ballots[0] == std::pair<std::string, std::vector<int>>{"alice", {1}});
    CHECK(ballots[1] == std::pair<std::string, std::vector<int>>{"bob", {0, 2}});
    CHECK(folded[0].poll->votes == std::vector<int>{1, 1, 1});
}

TEST_CASE("part A: receipts are batched, at most 100 ids and once every 2 s per chat, read first", "[e2ev3][content][parta][receipts]") {
    e2::ReceiptBatcher batcher;
    CHECK_FALSE(batcher.nextDue().has_value());
    for (int i = 0; i < 150; ++i) batcher.add("chat-1", "bob", idN(i), "delivered");
    batcher.add("chat-1", "bob", idN(3), "read");  // read replaces delivered for the same message
    batcher.add("chat-1", "bob", idN(3), "delivered");  // and isn't taken back
    batcher.add("chat-1", "carol", idN(500), "delivered");
    batcher.add("chat-2", "dave", idN(600), "read");
    batcher.add("chat-2", "dave", idN(601), "seen");  // not a status: ignored

    auto first = batcher.due(10000);
    REQUIRE(first.size() == 4);
    CHECK(first[0].conversationId == "chat-1");
    CHECK(first[0].status == "read");
    CHECK(first[0].ids == std::vector<std::string>{idN(3)});
    CHECK(first[1].authorUserId == "bob");
    CHECK(first[1].status == "delivered");
    CHECK(first[1].ids.size() == 100);
    CHECK(std::find(first[1].ids.begin(), first[1].ids.end(), idN(3)) == first[1].ids.end());
    CHECK(first[2].authorUserId == "carol");
    CHECK(first[3].conversationId == "chat-2");
    CHECK(first[3].ids == std::vector<std::string>{idN(600)});

    // The rest of bob's waits for chat-1's next turn, 2 s later.
    CHECK(batcher.due(11999).empty());
    CHECK(batcher.nextDue() == 12000);
    const auto second = batcher.due(12000);
    REQUIRE(second.size() == 1);
    CHECK(second[0].ids.size() == 49);
    CHECK(batcher.empty());
    // A new one in a chat that just sent waits its turn; one in a quiet chat goes at once.
    batcher.add("chat-1", "bob", idN(700), "read");
    batcher.add("chat-3", "erin", idN(800), "read");
    const auto third = batcher.due(12500);
    REQUIRE(third.size() == 1);
    CHECK(third[0].conversationId == "chat-3");
    CHECK(batcher.due(14000).size() == 1);
}

TEST_CASE("part A: a stored message keeps its reply, forward, timer and poll across a restart", "[e2ev3][content][parta]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    auto r = text(idN(1), "alice", 1000, "Q?");
    r.replyTo = idN(2);
    r.replyPreview = "earlier";
    r.forwarded = true;
    r.exp = 3600;
    r.poll = e2::ChatPoll{{"A", "B"}, true, {}, false, {}};
    {
        e2::ChatHistory history(dir.path / "h", key);
        history.setClock([] { return 2000; });  // long before it disappears
        REQUIRE(history.add(r));
        REQUIRE(history.add(text(idN(3), "alice", 1000, "plain")));
    }
    e2::ChatHistory reopened(dir.path / "h", key);
    reopened.setClock([] { return 0; });
    const auto stored = reopened.records(kChat);
    REQUIRE(stored.size() == 2);
    const auto& back = stored[0].id == idN(1) ? stored[0] : stored[1];
    CHECK(back.replyTo == idN(2));
    CHECK(back.replyPreview == "earlier");
    CHECK(back.forwarded);
    CHECK(back.exp == 3600);
    REQUIRE(back.poll.has_value());
    CHECK(back.poll->options == std::vector<std::string>{"A", "B"});
    CHECK(back.poll->multi);
    const Json plain = Json::parse(decryptFile(dir.path / "h", key));
    for (const Json& record : plain.at("records")) {
        if (record.at("id") != idN(3)) continue;
        CHECK_FALSE(record.contains("reply"));
        CHECK_FALSE(record.contains("fwd"));
        CHECK_FALSE(record.contains("exp"));
        CHECK_FALSE(record.contains("poll"));
    }
}
