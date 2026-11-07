#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "redvault/crypto/bytes.hpp"

// Locked files in Files: a file's own random key can be locked (wrapped)
// under a key of the person's own or under a password, on top of the drive
// key its metadata is always sealed under. So opening a locked file needs
// the account AND that key or password; the server, B2 and anyone who gets
// into the account alone see nothing more than before.
//
// The lock lives in the file's sealed metadata as "lock", in place of the
// plain "key":
//
//   {"v": 1, "by": "key", "keyId": <vault item id>, "fp": <fingerprint>, "ct", "nonce"}
//   {"v": 1, "by": "password", "salt", "ops", "mem", "ct", "nonce"}
//
// The wrap authenticates "redvault-file-lock-v1|<by>|<fileId>", so a lock
// can't be moved onto another file. Keys themselves are kept in the
// password vault (data/file_key_repository.hpp).

namespace redvault::data {

/// What locks (or unlocks) a file's key: a key of the person's own, or a password.
struct FileLock {
    enum class Kind { Key, Password };
    Kind kind = Kind::Password;
    /// Kind::Key: the vault item holding the key, its fingerprint, and the key itself (borrowed, not copied).
    std::string keyId;
    std::string fingerprint;
    crypto::ByteView key;
    /// Kind::Password.
    std::string password;

    static FileLock withKey(std::string keyId, std::string fingerprint, crypto::ByteView key);
    static FileLock withPassword(std::string password);
};

namespace filelock {

/// The "lock" object for a file's metadata. Password locks are slow (Argon2id): call off the UI thread.
nlohmann::json lock(crypto::ByteView fileKey, const FileLock& with, const std::string& fileId);

/**
 * The file's key from its "lock" object. Throws api::UserFacingError with
 * wording for the person: wrong password, a different key (naming its
 * fingerprint), or a lock that's damaged.
 */
crypto::SecretBytes unlock(const nlohmann::json& lock, const FileLock& with, const std::string& fileId);

/// Whether this file's metadata has a lock.
bool isLocked(const nlohmann::json& meta);
/// "key", "password", or empty when not locked.
std::string lockKind(const nlohmann::json& meta);
/// For a key lock: which key (vault item id and fingerprint); empty otherwise.
std::string lockKeyId(const nlohmann::json& meta);
std::string lockFingerprint(const nlohmann::json& meta);

/**
 * Why a password isn't good enough to protect files or keys (nullopt: it
 * is). It must not be empty and must rate at least "Fair" by the password
 * manager's strength estimate: a weak password is the only thing between an
 * attacker and the files.
 */
std::optional<std::string> passwordProblem(std::string_view password);

/// A strong password from the password manager's generator: 24 characters, or 6 words.
std::string suggestPassword(bool words = false);

}  // namespace filelock

}  // namespace redvault::data
