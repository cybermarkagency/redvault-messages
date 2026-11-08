// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

// The fail-closed outbox (docs/tor-routing-plan.md): messages and files queued
// while Tor is down are sealed on disk, survive a restart, and can only be read
// with the signed-in account's key.

#include <catch2/catch_test_macros.hpp>
#include <sodium.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

#include "redvault/data/outbox.hpp"
#include "redvault/util/text.hpp"

namespace fs = std::filesystem;
using namespace redvault;

namespace {

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("redvault-outbox-test-" + util::randomUuid());
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

crypto::SecretBytes accountKey(std::uint8_t fill) {
    crypto::SecretBytes key(32);
    for (std::size_t i = 0; i < key.size(); ++i) key.data()[i] = static_cast<std::uint8_t>(fill + i);
    return key;
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Every byte the outbox left on disk, concatenated.
std::string everythingOnDisk(const fs::path& dir) {
    std::string all;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file()) all += readAll(entry.path());
    }
    return all;
}

}  // namespace

TEST_CASE("outbox: queued text survives a restart, in order, sealed on disk", "[outbox]") {
    TempDir temp;
    const std::string secret = "meet at the usual place at 9";
    {
        data::Outbox outbox(temp.path, data::deriveOutboxKey(accountKey(1)));
        outbox.addText("conv-1", secret);
        outbox.addText("conv-2", "second", std::string("msg-7"), "the quoted bit", true);
    }
    // Nothing readable on disk: not the text, not the conversation, not the reply.
    const std::string disk = everythingOnDisk(temp.path);
    CHECK(disk.find(secret) == std::string::npos);
    CHECK(disk.find("conv-1") == std::string::npos);
    CHECK(disk.find("the quoted bit") == std::string::npos);

    data::Outbox reopened(temp.path, data::deriveOutboxKey(accountKey(1)));
    const auto items = reopened.items();
    REQUIRE(items.size() == 2);
    CHECK(items[0].text == secret);
    CHECK(items[0].conversationId == "conv-1");
    CHECK(items[1].replyToId == std::optional<std::string>("msg-7"));
    CHECK(items[1].replyPreview == "the quoted bit");
    CHECK(items[1].replyFromMe);
    CHECK(reopened.countFor("conv-2") == 1);
}

TEST_CASE("outbox: ids are valid clientMessageIds and never repeat", "[outbox]") {
    const std::regex serverPattern("^[A-Za-z0-9_-]{16,64}$");
    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i) {
        const std::string id = data::Outbox::newClientMessageId();
        CHECK(std::regex_match(id, serverPattern));
        CHECK(seen.insert(id).second);
    }
}

TEST_CASE("outbox: another account's key can't read it, and a tampered record is skipped", "[outbox]") {
    TempDir temp;
    {
        data::Outbox outbox(temp.path, data::deriveOutboxKey(accountKey(1)));
        outbox.addText("conv-1", "hello");
        outbox.addText("conv-1", "world");
    }
    CHECK(data::Outbox(temp.path, data::deriveOutboxKey(accountKey(2))).items().empty());

    // Flip one byte of one record: that record no longer opens, the other still does.
    fs::path victim;
    for (const auto& entry : fs::directory_iterator(temp.path)) {
        if (entry.path().extension() == ".item") victim = entry.path();
    }
    std::string bytes = readAll(victim);
    bytes[bytes.size() - 3] ^= 0x01;
    std::ofstream(victim, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(data::Outbox(temp.path, data::deriveOutboxKey(accountKey(1))).items().size() == 1);

    // A record renamed to another id is refused too (the id is bound into the seal).
    const fs::path renamed = temp.path / (data::Outbox::newClientMessageId() + ".item");
    fs::copy_file(victim, renamed);
    fs::remove(victim);
    CHECK(data::Outbox(temp.path, data::deriveOutboxKey(accountKey(1))).items().size() == 1);
}

TEST_CASE("outbox: a queued file is a sealed copy that opens back byte for byte", "[outbox]") {
    TempDir temp;
    const fs::path source = temp.path / "holiday.jpg";
    std::string original(3 * 64 * 1024 + 123, '\0');  // spans several chunks
    randombytes_buf(original.data(), original.size());
    original.replace(0, 4, "\xFF\xD8\xFF\xE0");
    std::ofstream(source, std::ios::binary).write(original.data(), static_cast<std::streamsize>(original.size()));

    const fs::path box = temp.path / "outbox";
    data::Outbox outbox(box, data::deriveOutboxKey(accountKey(1)));
    const auto item = outbox.addFile("conv-1", source, "holiday.jpg", "image/jpeg");
    fs::remove(source);  // the original can go away after queueing

    CHECK(everythingOnDisk(box).find(original.substr(100, 64)) == std::string::npos);
    const fs::path opened = temp.path / "opened.jpg";
    outbox.openFile(item, opened);
    CHECK(readAll(opened) == original);
    CHECK(outbox.items().at(0).kind == data::OutboxItem::Kind::File);
    CHECK(outbox.items().at(0).mimeType == "image/jpeg");
}

TEST_CASE("outbox: an exact multiple of the chunk size and an empty file round-trip", "[outbox]") {
    TempDir temp;
    data::Outbox outbox(temp.path / "box", data::deriveOutboxKey(accountKey(3)));
    for (const std::size_t size : {std::size_t{0}, std::size_t{64 * 1024}, std::size_t{2 * 64 * 1024}}) {
        const fs::path source = temp.path / ("f" + std::to_string(size));
        std::string data(size, 'x');
        std::ofstream(source, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
        const auto item = outbox.addFile("c", source, "f", "application/octet-stream");
        const fs::path out = temp.path / ("o" + std::to_string(size));
        outbox.openFile(item, out);
        CHECK(readAll(out) == data);
    }
}

TEST_CASE("outbox: a tampered or truncated queued file is refused, never half-delivered", "[outbox]") {
    TempDir temp;
    const fs::path source = temp.path / "doc.pdf";
    std::string original(200 * 1024, 'a');
    std::ofstream(source, std::ios::binary).write(original.data(), static_cast<std::streamsize>(original.size()));
    data::Outbox outbox(temp.path / "box", data::deriveOutboxKey(accountKey(1)));
    const auto item = outbox.addFile("c", source, "doc.pdf", "application/pdf");
    const fs::path sealed = temp.path / "box" / (item.id + ".file");

    std::string bytes = readAll(sealed);
    std::string truncated = bytes.substr(0, bytes.size() - 50);
    std::ofstream(sealed, std::ios::binary | std::ios::trunc).write(truncated.data(), static_cast<std::streamsize>(truncated.size()));
    CHECK_THROWS_AS(outbox.openFile(item, temp.path / "out1"), crypto::CryptoError);
    CHECK_FALSE(fs::exists(temp.path / "out1"));

    bytes[bytes.size() / 2] ^= 0x40;
    std::ofstream(sealed, std::ios::binary | std::ios::trunc).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK_THROWS_AS(outbox.openFile(item, temp.path / "out2"), crypto::CryptoError);
    CHECK_FALSE(fs::exists(temp.path / "out2"));
}

TEST_CASE("outbox: attempts are recorded and removal deletes everything", "[outbox]") {
    TempDir temp;
    const fs::path source = temp.path / "a.bin";
    std::ofstream(source, std::ios::binary) << "abc";
    data::Outbox outbox(temp.path / "box", data::deriveOutboxKey(accountKey(1)));
    const auto text = outbox.addText("c", "hi");
    const auto file = outbox.addFile("c", source, "a.bin", "application/octet-stream");
    outbox.recordAttempt(text.id, "Tor isn't connected");
    outbox.recordAttempt(text.id, "Tor isn't connected");
    const auto items = outbox.items();
    REQUIRE(items.size() == 2);
    CHECK(items[0].attempts == 2);
    CHECK(items[0].lastError == "Tor isn't connected");
    outbox.remove(text.id);
    outbox.remove(file.id);
    CHECK(outbox.items().empty());
    CHECK(fs::is_empty(temp.path / "box"));
}

TEST_CASE("outbox: the key is derived from the account key, the same on every client", "[outbox]") {
    // crypto_kdf_derive_from_key(32, 1, "rvoutbx1", key) for key = 00 01 .. 1f (Android uses the same).
    crypto::SecretBytes key(32);
    for (std::size_t i = 0; i < 32; ++i) key.data()[i] = static_cast<std::uint8_t>(i);
    const auto derived = data::deriveOutboxKey(key);
    unsigned char expected[32];
    crypto_kdf_derive_from_key(expected, sizeof expected, 1, "rvoutbx1", key.data());
    CHECK(std::equal(expected, expected + 32, derived.data()));
    CHECK_FALSE(std::equal(key.data(), key.data() + 32, derived.data()));
}
