#include "redvault/data/file_lock.hpp"

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/crypto/vault_crypto.hpp"

namespace redvault::data {

using api::UserFacingError;
using Json = nlohmann::json;

FileLock FileLock::withKey(std::string keyId, std::string fingerprint, crypto::ByteView key) {
    FileLock out;
    out.kind = Kind::Key;
    out.keyId = std::move(keyId);
    out.fingerprint = std::move(fingerprint);
    out.key = key;
    return out;
}

FileLock FileLock::withPassword(std::string password) {
    FileLock out;
    out.kind = Kind::Password;
    out.password = std::move(password);
    return out;
}

namespace filelock {

namespace {

std::string context(std::string_view by, const std::string& fileId) {
    return "redvault-file-lock-v1|" + std::string(by) + "|" + fileId;
}

crypto::drive::WrappedKey wrappedFrom(const Json& lock) {
    return {crypto::base64Decode(lock.at("ct").get<std::string>()), crypto::base64Decode(lock.at("nonce").get<std::string>())};
}

}  // namespace

Json lock(crypto::ByteView fileKey, const FileLock& with, const std::string& fileId) {
    if (with.kind == FileLock::Kind::Key) {
        if (with.key.size() != crypto::drive::kFileLockKeyBytes || with.keyId.empty()) throw UserFacingError("Choose a key to lock the file with.");
        const auto wrapped = crypto::drive::wrapKey(fileKey, with.key, context("key", fileId));
        return {{"v", 1},
                {"by", "key"},
                {"keyId", with.keyId},
                {"fp", with.fingerprint.empty() ? crypto::drive::fileLockKeyFingerprint(with.key) : with.fingerprint},
                {"ct", crypto::base64Encode(wrapped.ciphertext)},
                {"nonce", crypto::base64Encode(wrapped.nonce)}};
    }
    if (const auto problem = passwordProblem(with.password)) throw UserFacingError(*problem);
    const auto kdf = crypto::drive::newPasswordKdf();
    const auto key = crypto::drive::passwordKey(with.password, kdf);
    const auto wrapped = crypto::drive::wrapKey(fileKey, key.view(), context("password", fileId));
    return {{"v", 1},
            {"by", "password"},
            {"salt", crypto::base64Encode(kdf.salt)},
            {"ops", kdf.opsLimit},
            {"mem", kdf.memLimit},
            {"ct", crypto::base64Encode(wrapped.ciphertext)},
            {"nonce", crypto::base64Encode(wrapped.nonce)}};
}

crypto::SecretBytes unlock(const Json& lock, const FileLock& with, const std::string& fileId) {
    try {
        if (lock.value("v", 0) != 1) throw UserFacingError("This file was locked by a newer version of the app. Update the app to open it.");
        const std::string by = lock.value("by", std::string());
        if (by == "key") {
            if (with.kind != FileLock::Kind::Key) throw UserFacingError("This file is locked with a key, not a password.");
            const std::string fingerprint = lock.value("fp", std::string());
            if (crypto::drive::fileLockKeyFingerprint(with.key) != fingerprint) {
                throw UserFacingError("That's not the key this file is locked with (it needs the key " + fingerprint.substr(0, 4) + "-\xE2\x80\xA6).");
            }
            return crypto::drive::unwrapKey(wrappedFrom(lock), with.key, context("key", fileId));
        }
        if (by == "password") {
            if (with.kind != FileLock::Kind::Password) throw UserFacingError("This file is locked with a password, not a key.");
            crypto::drive::PasswordKdf kdf{crypto::base64Decode(lock.at("salt").get<std::string>()), lock.at("ops").get<unsigned long long>(),
                                           lock.at("mem").get<std::size_t>()};
            const auto key = crypto::drive::passwordKey(with.password, kdf);
            try {
                return crypto::drive::unwrapKey(wrappedFrom(lock), key.view(), context("password", fileId));
            } catch (const crypto::CryptoError&) {
                throw UserFacingError("Wrong password.");
            }
        }
        throw UserFacingError("This file's lock isn't one this app knows. Update to open it.");
    } catch (const UserFacingError&) {
        throw;
    } catch (const std::exception&) {
        throw UserFacingError("This file's lock is damaged, so it can't be opened.");
    }
}

bool isLocked(const Json& meta) {
    return meta.is_object() && meta.contains("lock") && meta.at("lock").is_object();
}

std::string lockKind(const Json& meta) {
    return isLocked(meta) ? meta.at("lock").value("by", std::string()) : std::string();
}

std::string lockKeyId(const Json& meta) {
    return lockKind(meta) == "key" ? meta.at("lock").value("keyId", std::string()) : std::string();
}

std::string lockFingerprint(const Json& meta) {
    return lockKind(meta) == "key" ? meta.at("lock").value("fp", std::string()) : std::string();
}

std::optional<std::string> passwordProblem(std::string_view password) {
    if (password.empty()) return "Enter a password.";
    if (crypto::vault::estimateStrength(password).strength == crypto::vault::Strength::Weak) {
        return "That password is too weak to protect files. Make it longer, or generate one.";
    }
    return std::nullopt;
}

std::string suggestPassword(bool words) {
    crypto::vault::GeneratorOptions options;
    options.words = words;
    options.length = words ? 6 : 24;
    return crypto::vault::generatePassword(options);
}

}  // namespace filelock

}  // namespace redvault::data
