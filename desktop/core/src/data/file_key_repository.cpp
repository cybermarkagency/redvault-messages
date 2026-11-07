#include "redvault/data/file_key_repository.hpp"

#include <sodium.h>

#include "redvault/api/api_error.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/drive_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

using api::UserFacingError;
using Json = nlohmann::json;

namespace {

constexpr const char* kType = "filekey";

std::string context(const std::string& fingerprint) {
    return "redvault-file-key-v1|" + fingerprint;
}

std::string cleanName(const std::string& name) {
    std::string out = name.substr(0, 80);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out.empty() ? std::string("File key") : out;
}

FileKey fromEntry(const VaultEntry& entry) {
    const Json& d = entry.data;
    FileKey out;
    out.id = entry.id;
    out.name = d.value("title", std::string("File key"));
    out.fingerprint = d.value("fingerprint", std::string());
    out.passwordProtected = d.contains("lock");
    out.imported = d.value("imported", false);
    out.createdMs = d.value("created", std::int64_t{0});
    return out;
}

}  // namespace

bool isFileKeyItem(const Json& data) {
    return data.is_object() && data.value("type", std::string()) == kType;
}

FileKeyRepository::FileKeyRepository(std::shared_ptr<VaultRepository> vault) : vault_(std::move(vault)) {}

std::vector<FileKey> FileKeyRepository::list() {
    std::vector<FileKey> out;
    for (const auto& entry : vault_->list()) {
        if (!entry.incoming && isFileKeyItem(entry.data)) out.push_back(fromEntry(entry));
    }
    return out;
}

FileKey FileKeyRepository::create(const std::string& name, const std::string& password) {
    VaultEntry entry;
    entry.data = {{"type", kType}, {"title", cleanName(name)}, {"imported", false}};
    return save(std::move(entry), crypto::drive::newFileLockKey(), password);
}

FileKey FileKeyRepository::import(const std::string& name, const std::string& keyText, const std::string& password) {
    auto secret = crypto::drive::parseFileLockKey(keyText);
    if (!secret) throw UserFacingError("That isn't a valid key. Paste 64 hex characters or the base64 of a 256-bit key.");
    const std::string fingerprint = crypto::drive::fileLockKeyFingerprint(secret->view());
    for (const auto& existing : list()) {
        if (existing.fingerprint == fingerprint) throw UserFacingError("You already have this key: \"" + existing.name + "\".");
    }
    VaultEntry entry;
    entry.data = {{"type", kType}, {"title", cleanName(name)}, {"imported", true}};
    return save(std::move(entry), *secret, password);
}

crypto::SecretBytes FileKeyRepository::open(const FileKey& key, const std::string& password) {
    const Json data = entryFor(key).data;
    const std::string fingerprint = data.value("fingerprint", std::string());
    try {
        crypto::SecretBytes secret;
        if (data.contains("lock")) {
            if (password.empty()) throw UserFacingError("Enter this key's password.");
            const Json& lock = data.at("lock");
            crypto::drive::PasswordKdf kdf{crypto::base64Decode(lock.at("salt").get<std::string>()), lock.at("ops").get<unsigned long long>(),
                                           lock.at("mem").get<std::size_t>()};
            const auto wrappingKey = crypto::drive::passwordKey(password, kdf);
            try {
                secret = crypto::drive::unwrapKey(
                    {crypto::base64Decode(lock.at("ct").get<std::string>()), crypto::base64Decode(lock.at("nonce").get<std::string>())},
                    wrappingKey.view(), context(fingerprint));
            } catch (const crypto::CryptoError&) {
                throw UserFacingError("Wrong password.");
            }
        } else {
            auto raw = crypto::base64Decode(data.at("key").get<std::string>());
            secret = crypto::SecretBytes::copyOf(raw);
            sodium_memzero(raw.data(), raw.size());
        }
        if (secret.size() != crypto::drive::kFileLockKeyBytes || crypto::drive::fileLockKeyFingerprint(secret.view()) != fingerprint) {
            throw UserFacingError("This key is damaged, so it can't be used.");
        }
        return secret;
    } catch (const UserFacingError&) {
        throw;
    } catch (const std::exception&) {
        throw UserFacingError("This key is damaged, so it can't be used.");
    }
}

FileLock FileKeyRepository::lockWith(const FileKey& key, const crypto::SecretBytes& secret) {
    return FileLock::withKey(key.id, key.fingerprint, secret.view());
}

FileKey FileKeyRepository::setPassword(const FileKey& key, const std::string& currentPassword, const std::string& newPassword) {
    const auto secret = open(key, currentPassword);
    return save(entryFor(key), secret, newPassword);
}

FileKey FileKeyRepository::rename(const FileKey& key, const std::string& name) {
    auto entry = entryFor(key);
    entry.data["title"] = cleanName(name);
    entry.data["updated"] = util::nowMillis();
    return fromEntry(vault_->save(std::move(entry)));
}

std::string FileKeyRepository::exportHex(const FileKey& key, const std::string& password) {
    return crypto::drive::fileLockKeyHex(open(key, password).view());
}

void FileKeyRepository::remove(const FileKey& key) {
    vault_->remove(entryFor(key));
}

VaultEntry FileKeyRepository::entryFor(const FileKey& key) {
    for (auto& entry : vault_->list()) {
        if (entry.id == key.id && !entry.incoming && isFileKeyItem(entry.data)) return entry;
    }
    throw UserFacingError("That key isn't there anymore.");
}

FileKey FileKeyRepository::save(VaultEntry entry, const crypto::SecretBytes& secret, const std::string& password) {
    if (!password.empty()) {
        if (const auto problem = filelock::passwordProblem(password)) throw UserFacingError(*problem);
    }
    const std::string fingerprint = crypto::drive::fileLockKeyFingerprint(secret.view());
    const auto now = util::nowMillis();
    Json& d = entry.data;
    d["fingerprint"] = fingerprint;
    if (!d.contains("created")) d["created"] = now;
    d["updated"] = now;
    d.erase("key");
    d.erase("lock");
    if (password.empty()) {
        d["key"] = crypto::base64Encode(secret.view());
    } else {
        const auto kdf = crypto::drive::newPasswordKdf();
        const auto wrappingKey = crypto::drive::passwordKey(password, kdf);
        const auto wrapped = crypto::drive::wrapKey(secret.view(), wrappingKey.view(), context(fingerprint));
        d["lock"] = {{"v", 1},
                     {"salt", crypto::base64Encode(kdf.salt)},
                     {"ops", kdf.opsLimit},
                     {"mem", kdf.memLimit},
                     {"ct", crypto::base64Encode(wrapped.ciphertext)},
                     {"nonce", crypto::base64Encode(wrapped.nonce)}};
    }
    return fromEntry(vault_->save(std::move(entry)));
}

}  // namespace redvault::data
