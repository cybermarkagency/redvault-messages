#include <catch2/catch_test_macros.hpp>

#include <sodium.h>

#include <string>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/crypto/vault_crypto.hpp"
#include "redvault/data/drive_repository.hpp"
#include "redvault/data/file_key_repository.hpp"
#include "redvault/data/file_lock.hpp"

using namespace redvault;
using namespace redvault::crypto;
using Json = nlohmann::json;

namespace {

const std::string kHexKey = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
const std::string kStrongPassword = "correct-horse-battery-staple-42";

std::string messageOf(const std::function<void()>& block) {
    try {
        block();
    } catch (const api::UserFacingError& e) {
        return e.what();
    }
    return "(no error)";
}

}  // namespace

TEST_CASE("an imported key is accepted as hex or base64, and nothing else", "[filelock]") {
    const auto fromHex = drive::parseFileLockKey(kHexKey);
    REQUIRE(fromHex);
    CHECK(drive::fileLockKeyHex(fromHex->view()) == kHexKey);

    // Uppercase, grouped, wrapped over lines: all the same key.
    CHECK(drive::parseFileLockKey("00010203-04050607-08090A0B-0C0D0E0F\n10111213 14151617 18191A1B 1C1D1E1F"));
    const auto b64 = base64Encode(fromHex->view());
    const auto fromB64 = drive::parseFileLockKey(b64);
    REQUIRE(fromB64);
    CHECK(constantTimeEquals(fromB64->view(), fromHex->view()));
    std::string url = b64;
    for (char& c : url) c = c == '+' ? '-' : c == '/' ? '_' : c;
    while (!url.empty() && url.back() == '=') url.pop_back();
    CHECK(drive::parseFileLockKey(url));

    CHECK_FALSE(drive::parseFileLockKey(""));
    CHECK_FALSE(drive::parseFileLockKey(kHexKey.substr(2)));           // 31 bytes
    CHECK_FALSE(drive::parseFileLockKey(kHexKey + "00"));              // 33 bytes
    CHECK_FALSE(drive::parseFileLockKey("not a key at all"));
    CHECK_FALSE(drive::parseFileLockKey(base64Encode(toBytes("sixteen byte key"))));
}

TEST_CASE("a key's fingerprint is stable, short, and differs between keys", "[filelock]") {
    const auto key = drive::parseFileLockKey(kHexKey);
    const std::string fingerprint = drive::fileLockKeyFingerprint(key->view());
    CHECK(fingerprint.size() == 19);
    CHECK(fingerprint[4] == '-');
    CHECK(drive::fileLockKeyFingerprint(drive::parseFileLockKey(kHexKey)->view()) == fingerprint);
    CHECK(drive::fileLockKeyFingerprint(drive::newFileLockKey().view()) != fingerprint);
    // It doesn't give the key away.
    CHECK(kHexKey.find(fingerprint.substr(0, 4)) == std::string::npos);
}

TEST_CASE("a wrapped key only opens with the same key and context", "[filelock]") {
    const auto fileKey = drive::newFileLockKey();
    const auto lockKey = drive::newFileLockKey();
    const auto wrapped = drive::wrapKey(fileKey.view(), lockKey.view(), "ctx-a");
    CHECK(constantTimeEquals(drive::unwrapKey(wrapped, lockKey.view(), "ctx-a").view(), fileKey.view()));
    CHECK_THROWS_AS(drive::unwrapKey(wrapped, drive::newFileLockKey().view(), "ctx-a"), CryptoError);
    CHECK_THROWS_AS(drive::unwrapKey(wrapped, lockKey.view(), "ctx-b"), CryptoError);
    auto tampered = wrapped;
    tampered.ciphertext[0] ^= 1;
    CHECK_THROWS_AS(drive::unwrapKey(tampered, lockKey.view(), "ctx-a"), CryptoError);
}

TEST_CASE("password settings stored with a lock can't be pushed out of range", "[filelock]") {
    auto kdf = drive::newPasswordKdf();
    kdf.memLimit = 8ull * 1024 * 1024 * 1024;  // 8 GiB
    CHECK_THROWS_AS(drive::passwordKey("pw", kdf), CryptoError);
    kdf = drive::newPasswordKdf();
    kdf.opsLimit = 1;
    CHECK_THROWS_AS(drive::passwordKey("pw", kdf), CryptoError);
    kdf = drive::newPasswordKdf();
    kdf.salt.pop_back();
    CHECK_THROWS_AS(drive::passwordKey("pw", kdf), CryptoError);
}

TEST_CASE("a file locked with a key opens only with that key, for that file", "[filelock]") {
    const auto fileKey = drive::newFileLockKey();
    const auto mine = drive::parseFileLockKey(kHexKey);
    const auto fingerprint = drive::fileLockKeyFingerprint(mine->view());
    const auto lock = data::filelock::lock(fileKey.view(), data::FileLock::withKey("item-1", fingerprint, mine->view()), "file-1");

    CHECK(lock.at("by") == "key");
    CHECK(lock.at("keyId") == "item-1");
    CHECK(lock.at("fp") == fingerprint);
    CHECK_FALSE(lock.contains("key"));
    // The file key isn't anywhere in the lock in the clear.
    CHECK(lock.dump().find(base64Encode(fileKey.view())) == std::string::npos);

    const auto opened = data::filelock::unlock(lock, data::FileLock::withKey("item-1", fingerprint, mine->view()), "file-1");
    CHECK(constantTimeEquals(opened.view(), fileKey.view()));

    const auto other = drive::newFileLockKey();
    CHECK(messageOf([&] { data::filelock::unlock(lock, data::FileLock::withKey("x", "", other.view()), "file-1"); }) ==
          "That's not the key this file is locked with (it needs the key " + fingerprint.substr(0, 4) + "-\xE2\x80\xA6).");
    CHECK(messageOf([&] { data::filelock::unlock(lock, data::FileLock::withPassword(kStrongPassword), "file-1"); }) ==
          "This file is locked with a key, not a password.");
    // Copied onto another file, it doesn't open.
    CHECK(messageOf([&] { data::filelock::unlock(lock, data::FileLock::withKey("item-1", fingerprint, mine->view()), "file-2"); }) ==
          "This file's lock is damaged, so it can't be opened.");
}

TEST_CASE("a file locked with a password opens only with that password", "[filelock]") {
    const auto fileKey = drive::newFileLockKey();
    const auto lock = data::filelock::lock(fileKey.view(), data::FileLock::withPassword(kStrongPassword), "file-1");
    CHECK(lock.at("by") == "password");
    CHECK(lock.at("ops").get<unsigned long long>() >= crypto_pwhash_OPSLIMIT_MODERATE);
    CHECK(lock.dump().find(kStrongPassword) == std::string::npos);

    const auto opened = data::filelock::unlock(lock, data::FileLock::withPassword(kStrongPassword), "file-1");
    CHECK(constantTimeEquals(opened.view(), fileKey.view()));
    CHECK(messageOf([&] { data::filelock::unlock(lock, data::FileLock::withPassword("correct-horse-battery-staple-43"), "file-1"); }) ==
          "Wrong password.");

    Json newer = lock;
    newer["v"] = 2;
    CHECK(messageOf([&] { data::filelock::unlock(newer, data::FileLock::withPassword(kStrongPassword), "file-1"); }) ==
          "This file was locked by a newer version of the app. Update the app to open it.");
}

TEST_CASE("weak passwords are refused for locking, and the generator's aren't", "[filelock]") {
    CHECK(data::filelock::passwordProblem("") == std::optional<std::string>("Enter a password."));
    CHECK(data::filelock::passwordProblem("password"));
    CHECK(data::filelock::passwordProblem("abc123"));
    CHECK_FALSE(data::filelock::passwordProblem(kStrongPassword));
    CHECK(messageOf([] { data::filelock::lock(drive::newFileLockKey().view(), data::FileLock::withPassword("hunter2"), "f"); }) ==
          "That password is too weak to protect files. Make it longer, or generate one.");

    const auto characters = data::filelock::suggestPassword();
    CHECK(characters.size() == 24);
    CHECK_FALSE(data::filelock::passwordProblem(characters));
    const auto words = data::filelock::suggestPassword(true);
    CHECK_FALSE(data::filelock::passwordProblem(words));
    CHECK(data::filelock::suggestPassword() != characters);
}

TEST_CASE("a Files entry knows whether it's locked and still needs unlocking", "[filelock]") {
    data::DriveEntry entry;
    entry.meta = {{"key", "k"}};
    CHECK_FALSE(entry.locked());
    CHECK_FALSE(entry.needsUnlock());

    entry.meta = {{"lock", {{"v", 1}, {"by", "key"}, {"keyId", "item-1"}, {"fp", "AAAA-BBBB-CCCC-DDDD"}}}};
    CHECK(entry.locked());
    CHECK(entry.needsUnlock());
    CHECK(data::filelock::lockKind(entry.meta) == "key");
    CHECK(data::filelock::lockKeyId(entry.meta) == "item-1");
    CHECK(data::filelock::lockFingerprint(entry.meta) == "AAAA-BBBB-CCCC-DDDD");

    entry.meta["key"] = "unlocked in memory";
    CHECK_FALSE(entry.needsUnlock());
}

TEST_CASE("file keys are told apart from passwords in the vault", "[filelock]") {
    CHECK(data::isFileKeyItem(Json{{"type", "filekey"}}));
    CHECK_FALSE(data::isFileKeyItem(Json{{"type", "login"}}));
    CHECK_FALSE(data::isFileKeyItem(Json::array()));
}
