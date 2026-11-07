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

// Reactions, edits, deletes and files on device keys (docs/multi-device-threat-model.md
// 9.3), against the vectors every client shares (the web loads the same file in
// ui/src/test/e2eV3Content.test.ts), and erasure against the real history file.

namespace e2 = redvault::e2ev3;
namespace fs = std::filesystem;
using e2::Json;

namespace {

Json vectors() {
    std::ifstream in(REDVAULT_V3_CONTENT_VECTORS, std::ios::binary);
    REQUIRE(in.good());
    return Json::parse(in);
}

const std::string kChat = "11111111-1111-4111-8111-111111111111";
const std::string kMessage = "aaaaaaaa-0000-4000-8000-000000000001";

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
    if (t == "text") {
        r.text = v.at("body").get<std::string>();
    } else {
        r.op = e2::ChatOp{t, v.at("target").get<std::string>(), v.value("body", std::string()), v.value("emoji", std::string())};
    }
    return r;
}

Json shown(const std::vector<e2::ChatRecord>& folded) {
    Json out = Json::array();
    for (const e2::ChatRecord& m : folded) {
        Json reactions = Json::array();
        for (const e2::ChatReaction& r : m.reactions) reactions.push_back({{"user", r.userId}, {"emoji", r.emoji}});
        out.push_back({{"id", m.id}, {"text", m.text}, {"edited", m.edited}, {"deleted", m.deleted}, {"reactions", reactions}});
    }
    return out;
}

redvault::model::FileDescriptor sampleFile() {
    return redvault::model::FileDescriptor{
        .type = "file",
        .version = 1,
        .objectId = "33333333-3333-4333-8333-333333333333",
        .fileKey = "c2VjcmV0LWZpbGUta2V5LXNlY3JldC1maWxlLWtleSE=",
        .filename = "notes.txt",
        .mimeType = "text/plain",
        .originalSize = 5,
        .encryptedSize = 46,
        .ciphertextSha256 = std::string(64, 'a'),
        .encryption = {.algorithm = "xchacha20poly1305-secretstream", .version = 1, .header = "aGVhZGVyaGVhZGVyaGVhZGVyaGVhZGVy", .chunkSize = 65536},
    };
}

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("rv-e2ev3-content-" + redvault::util::randomUuid());
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

/// What the history file really holds, opened the way ChatHistory opens it.
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

/// A history file as a build from before these types wrote it.
void writeFile(const fs::path& path, const redvault::crypto::Bytes& key, const std::string& plain) {
    const std::size_t nonce = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
    std::string blob(nonce + plain.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    randombytes_buf(blob.data(), nonce);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(reinterpret_cast<unsigned char*>(blob.data()) + nonce, &length,
                                               reinterpret_cast<const unsigned char*>(plain.data()), plain.size(), nullptr, 0, nullptr,
                                               reinterpret_cast<const unsigned char*>(blob.data()), key.data());
    blob.resize(nonce + length);
    std::ofstream(path, std::ios::binary) << blob;
}

e2::ChatRecord message(const std::string& id, const std::string& from, std::int64_t ts, const std::string& text) {
    return e2::ChatRecord{kChat, id, from, "d", ts, ts, text, "in", "received"};
}

e2::ChatRecord opRecord(const std::string& id, const std::string& from, std::int64_t ts, const e2::ChatOp& op) {
    auto r = message(id, from, ts, "");
    r.op = op;
    return r;
}

const std::string kEdit = "aaaaaaaa-0000-4000-8000-000000000002";
const std::string kReaction = "aaaaaaaa-0000-4000-8000-000000000003";
const std::string kDelete = "aaaaaaaa-0000-4000-8000-000000000004";

}  // namespace

TEST_CASE("the shared vectors: every fold case, in both arrival orders", "[e2ev3][content]") {
    const Json v = vectors();
    REQUIRE(v.at("fold").size() >= 6);
    for (const Json& c : v.at("fold")) {
        const std::string name = c.at("name").get<std::string>();
        INFO(name);
        std::vector<e2::ChatRecord> records;
        std::int64_t arrived = 0;
        for (const Json& record : c.at("records")) records.push_back(toRecord(record, arrived++, v.at("conversation").get<std::string>()));

        CHECK(shown(e2::foldChat(records)) == c.at("expect"));
        std::reverse(records.begin(), records.end());
        CHECK(shown(e2::foldChat(records)) == c.at("expect"));
    }
}

TEST_CASE("the shared vectors: every fold case against the real history, in both arrival orders", "[e2ev3][content]") {
    const Json v = vectors();
    const auto key = redvault::crypto::randomBytes(32);
    for (const Json& c : v.at("fold")) {
        const std::string name = c.at("name").get<std::string>();
        INFO(name);
        std::vector<e2::ChatRecord> records;
        std::int64_t arrived = 0;
        for (const Json& record : c.at("records")) records.push_back(toRecord(record, arrived++, v.at("conversation").get<std::string>()));
        for (const bool reversed : {false, true}) {
            TempDir dir;
            if (reversed) std::reverse(records.begin(), records.end());
            e2::ChatHistory history(dir.path / "h", key);
            for (const e2::ChatRecord& r : records) REQUIRE(history.add(r));
            CHECK(shown(history.list(v.at("conversation").get<std::string>())) == c.at("expect"));
        }
    }
}

TEST_CASE("the shared vectors: every decode case", "[e2ev3][content]") {
    const Json v = vectors();
    REQUIRE(v.at("decode").size() >= 9);
    for (const Json& c : v.at("decode")) {
        const std::string name = c.at("name").get<std::string>();
        INFO(name);
        const auto decoded = e2::decodeEnvelope(c.at("envelope").dump(), v.at("conversation").get<std::string>(), 2000);
        const std::string accept = c.at("accept").get<std::string>();
        const auto expected = accept == "op"        ? e2::DecodedEnvelope::Kind::Op
                              : accept == "message" ? e2::DecodedEnvelope::Kind::Message
                                                    : e2::DecodedEnvelope::Kind::Ignore;
        CHECK(decoded.kind == expected);
    }
}

TEST_CASE("the envelopes round-trip, and a file's key travels only inside one", "[e2ev3][content]") {
    const std::string target = "aaaaaaaa-0000-4000-8000-000000000002";
    const auto react = e2::decodeEnvelope(e2::encodeOp(kChat, {"reaction", target, {}, "\xF0\x9F\x91\x8D"}, kMessage, 1000), kChat, 2000);
    REQUIRE(react.kind == e2::DecodedEnvelope::Kind::Op);
    CHECK(react.op->t == "reaction");
    CHECK(react.op->target == target);
    CHECK(react.op->emoji == "\xF0\x9F\x91\x8D");
    const auto edit = e2::decodeEnvelope(e2::encodeOp(kChat, {"edit", target, "new", {}}, kMessage, 1000), kChat, 2000);
    REQUIRE(edit.kind == e2::DecodedEnvelope::Kind::Op);
    CHECK(edit.op->body == "new");
    const auto del = e2::decodeEnvelope(e2::encodeOp(kChat, {"delete", target, {}, {}}, kMessage, 1000), kChat, 2000);
    REQUIRE(del.kind == e2::DecodedEnvelope::Kind::Op);
    CHECK(del.op->t == "delete");

    const auto file = e2::decodeEnvelope(e2::encodeFile(kChat, sampleFile(), kMessage, 1000), kChat, 2000);
    REQUIRE(file.kind == e2::DecodedEnvelope::Kind::Message);
    REQUIRE(file.file.has_value());
    CHECK(*file.file == sampleFile());
    CHECK(file.text.empty());

    CHECK_THROWS_AS(e2::encodeOp(kChat, {"edit", target, "", {}}, kMessage, 1000), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeOp(kChat, {"edit", target, std::string(16 * 1024 + 1, 'x'), {}}, kMessage, 1000), e2::EngineError);
    CHECK_THROWS_AS(e2::encodeOp(kChat, {"reaction", target, {}, std::string(33, 'x')}, kMessage, 1000), e2::EngineError);
}

TEST_CASE("envelopes fill 256-byte buckets, and still decode", "[e2ev3][content][padding]") {
    const std::string target = "aaaaaaaa-0000-4000-8000-000000000002";
    const auto bucketed = [](const std::string& envelope) {
        REQUIRE_FALSE(envelope.empty());
        CHECK(envelope.size() % 256 == 0);
        CHECK(envelope.back() == ' ');
    };
    const auto op = e2::encodeOp(kChat, {"reaction", target, {}, "x"}, kMessage, 1000);
    CHECK(op.size() == 256);
    bucketed(op);
    const auto file = e2::encodeFile(kChat, sampleFile(), kMessage, 1000);
    bucketed(file);
    REQUIRE(e2::decodeEnvelope(file, kChat, 2000).kind == e2::DecodedEnvelope::Kind::Message);
    REQUIRE(*e2::decodeEnvelope(file, kChat, 2000).file == sampleFile());

    for (const std::size_t length : {std::size_t{1}, std::size_t{100}, std::size_t{255}, std::size_t{256}, std::size_t{600}, std::size_t{5000}}) {
        CAPTURE(length);
        const std::string text(length, 'x');
        e2::Envelope envelope{.v = 1, .t = "text", .id = kMessage, .c = kChat, .ts = 1000, .body = text};
        const auto encoded = e2::encodeEnvelope(envelope);
        bucketed(encoded);
        const auto decoded = e2::decodeEnvelope(encoded, kChat, 2000);
        REQUIRE(decoded.kind == e2::DecodedEnvelope::Kind::Message);
        CHECK(decoded.text == text);
    }
    // Two lengths in one bucket can't be told apart.
    e2::Envelope shortOne{.v = 1, .t = "text", .id = kMessage, .c = kChat, .ts = 1000, .body = "a"};
    e2::Envelope longer = shortOne;
    longer.body = "a slightly longer message";
    CHECK(e2::encodeEnvelope(shortOne).size() == e2::encodeEnvelope(longer).size());
}

TEST_CASE("an envelope exactly filling a bucket gets a whole bucket more", "[e2ev3][content][padding]") {
    e2::Envelope envelope{.v = 1, .t = "text", .id = kMessage, .c = kChat, .ts = 1000, .body = ""};
    for (std::size_t length = 150; length < 260; ++length) {
        envelope.body = std::string(length, 'x');
        const auto encoded = e2::encodeEnvelope(envelope);
        CHECK(encoded.size() % 256 == 0);
        CHECK(encoded.back() == ' ');
    }
}

TEST_CASE("decoding follows the web's other rules", "[e2ev3][content]") {
    using Kind = e2::DecodedEnvelope::Kind;
    const auto text = [](std::int64_t ts) {
        e2::Envelope e;
        e.id = kMessage;
        e.c = kChat;
        e.ts = ts;
        e.body = "hi";
        return e2::encodeEnvelope(e);
    };
    CHECK(e2::decodeEnvelope(text(1000), kChat, 2000).kind == Kind::Message);
    CHECK(e2::decodeEnvelope("not json", kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope("[1]", kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(text(1000), "", 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(R"({"v":2,"t":"text"})", kChat, 2000).kind == Kind::Newer);
    CHECK(e2::decodeEnvelope(R"({"v":0,"t":"text"})", kChat, 2000).kind == Kind::Ignore);
    CHECK(e2::decodeEnvelope(R"({"t":"text"})", kChat, 2000).kind == Kind::Ignore);

    // The sender's clock is clamped to [now - 30 days, now + 5 minutes].
    constexpr std::int64_t kNow = 40LL * 24 * 3600 * 1000;
    CHECK(e2::decodeEnvelope(text(1000), kChat, kNow).ts == kNow - 30LL * 24 * 3600 * 1000);
    CHECK(e2::decodeEnvelope(text(kNow * 2), kChat, kNow).ts == kNow + 5 * 60 * 1000);
}

TEST_CASE("a delete erases the message's text, edits and reactions from the history file", "[e2ev3][content]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    const fs::path file = dir.path / "h";
    e2::ChatHistory history(file, key);
    REQUIRE(history.add(message(kMessage, "alice", 1000, "the secret words")));
    REQUIRE(history.add(opRecord(kEdit, "alice", 2000, {"edit", kMessage, "the edited words", {}})));
    REQUIRE(history.add(opRecord(kReaction, "bob", 2500, {"reaction", kMessage, {}, "\xF0\x9F\x94\xA5"})));
    const std::string before = decryptFile(file, key);
    REQUIRE(before.find("the secret words") != std::string::npos);
    REQUIRE(before.find("the edited words") != std::string::npos);

    REQUIRE(history.add(opRecord(kDelete, "alice", 3000, {"delete", kMessage, {}, {}})));

    const std::string after = decryptFile(file, key);
    CHECK(after.find("the secret words") == std::string::npos);
    CHECK(after.find("the edited words") == std::string::npos);
    CHECK(after.find(kReaction) == std::string::npos);
    CHECK(after.find(kEdit) == std::string::npos);

    // What's left: the emptied message and the delete, which keeps it deleted.
    e2::ChatHistory reopened(file, key);
    const auto stored = reopened.records(kChat);
    REQUIRE(stored.size() == 2);
    const auto shownNow = reopened.list(kChat);
    REQUIRE(shownNow.size() == 1);
    CHECK(shownNow[0].deleted);
    CHECK(shownNow[0].text.empty());
    CHECK_FALSE(shownNow[0].edited);
    CHECK(shownNow[0].reactions.empty());
}

TEST_CASE("a delete erases a file's key from the history file", "[e2ev3][content]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    const fs::path file = dir.path / "h";
    e2::ChatHistory history(file, key);
    auto attachment = message(kMessage, "alice", 1000, "");
    attachment.file = sampleFile();
    REQUIRE(history.add(attachment));
    REQUIRE(decryptFile(file, key).find(sampleFile().fileKey) != std::string::npos);

    {
        e2::ChatHistory reopened(file, key);  // the descriptor survives a restart
        REQUIRE(reopened.list(kChat).at(0).file == sampleFile());
    }

    REQUIRE(history.add(opRecord(kDelete, "alice", 3000, {"delete", kMessage, {}, {}})));
    const std::string after = decryptFile(file, key);
    CHECK(after.find(sampleFile().fileKey) == std::string::npos);
    CHECK(after.find("notes.txt") == std::string::npos);
    const auto shownNow = history.list(kChat);
    REQUIRE(shownNow.size() == 1);
    CHECK(shownNow[0].deleted);
    CHECK_FALSE(shownNow[0].file.has_value());
}

TEST_CASE("a delete that arrives first keeps the message's text off the disk", "[e2ev3][content]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    const fs::path file = dir.path / "h";
    e2::ChatHistory history(file, key);
    REQUIRE(history.add(opRecord(kDelete, "alice", 3000, {"delete", kMessage, {}, {}})));
    REQUIRE(history.list(kChat).empty());  // waits for its message

    REQUIRE(history.add(message(kMessage, "alice", 1000, "the secret words")));
    CHECK(decryptFile(file, key).find("the secret words") == std::string::npos);
    REQUIRE(history.list(kChat).size() == 1);
    CHECK(history.list(kChat)[0].deleted);

    // An edit or reaction that turns up afterwards goes too.
    REQUIRE(history.add(opRecord(kEdit, "alice", 2000, {"edit", kMessage, "the edited words", {}})));
    CHECK(decryptFile(file, key).find("the edited words") == std::string::npos);
}

TEST_CASE("someone else's delete erases nothing", "[e2ev3][content]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    e2::ChatHistory history(dir.path / "h", key);
    REQUIRE(history.add(message(kMessage, "alice", 1000, "the secret words")));
    REQUIRE(history.add(opRecord(kEdit, "alice", 2000, {"edit", kMessage, "the edited words", {}})));
    REQUIRE(history.add(opRecord(kDelete, "mallory", 3000, {"delete", kMessage, {}, {}})));
    CHECK(decryptFile(dir.path / "h", key).find("the secret words") != std::string::npos);
    const auto shownNow = history.list(kChat);
    REQUIRE(shownNow.size() == 1);
    CHECK_FALSE(shownNow[0].deleted);
    CHECK(shownNow[0].text == "the edited words");
}

TEST_CASE("the same ids in another conversation are left alone", "[e2ev3][content]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    e2::ChatHistory history(dir.path / "h", key);
    REQUIRE(history.add(message(kMessage, "alice", 1000, "here")));
    auto other = message(kMessage, "alice", 1000, "there");
    other.conversationId = "22222222-2222-4222-8222-222222222222";
    REQUIRE(history.add(other));
    REQUIRE(history.add(opRecord(kDelete, "alice", 3000, {"delete", kMessage, {}, {}})));
    CHECK(history.list(kChat)[0].deleted);
    CHECK(history.list(other.conversationId)[0].text == "there");
}

TEST_CASE("a file can't be edited", "[e2ev3][content]") {
    auto attachment = message(kMessage, "alice", 1000, "");
    attachment.file = sampleFile();
    const auto folded = e2::foldChat({attachment, opRecord(kEdit, "alice", 2000, {"edit", kMessage, "words", {}})});
    REQUIRE(folded.size() == 1);
    CHECK(folded[0].text.empty());
    CHECK_FALSE(folded[0].edited);
}

TEST_CASE("reactions from several people come out in the same order however they arrived", "[e2ev3][content]") {
    const auto a = opRecord(kEdit, "carol", 2000, {"reaction", kMessage, {}, "a"});
    const auto b = opRecord(kReaction, "bob", 2500, {"reaction", kMessage, {}, "b"});
    const auto first = e2::foldChat({message(kMessage, "alice", 1000, "hi"), a, b});
    const auto second = e2::foldChat({b, a, message(kMessage, "alice", 1000, "hi")});
    CHECK(shown(first) == shown(second));
    REQUIRE(first.at(0).reactions.size() == 2);
    CHECK(first[0].reactions[0].userId == "bob");
}

TEST_CASE("a history written before ops and files still opens, and can take them", "[e2ev3][content]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    const fs::path file = dir.path / "h";
    const Json old = {{"records", Json::array({Json{{"conversationId", kChat}, {"id", kMessage}, {"from", "alice"}, {"fromDeviceId", "d"}, {"ts", 5},
                                                    {"arrivedAt", 6}, {"text", "old words"}, {"direction", "in"}, {"status", "received"}}})},
                      {"pins", Json::array({kChat})}};
    writeFile(file, key, old.dump());

    e2::ChatHistory history(file, key);
    REQUIRE(history.pinned(kChat));
    const auto records = history.records(kChat);
    REQUIRE(records.size() == 1);
    CHECK(records[0].text == "old words");
    CHECK_FALSE(records[0].op.has_value());
    CHECK_FALSE(records[0].file.has_value());

    REQUIRE(history.add(opRecord(kEdit, "alice", 2000, {"edit", kMessage, "new words", {}})));
    e2::ChatHistory reopened(file, key);
    CHECK(reopened.list(kChat).at(0).text == "new words");
    CHECK(reopened.records(kChat).size() == 2);
}

TEST_CASE("a stored record with neither field round-trips without gaining them", "[e2ev3][content]") {
    TempDir dir;
    const auto key = redvault::crypto::randomBytes(32);
    const fs::path file = dir.path / "h";
    e2::ChatHistory history(file, key);
    REQUIRE(history.add(message(kMessage, "alice", 1000, "plain")));
    const Json stored = Json::parse(decryptFile(file, key));
    const Json& record = stored.at("records").at(0);
    CHECK_FALSE(record.contains("op"));
    CHECK_FALSE(record.contains("file"));
}
