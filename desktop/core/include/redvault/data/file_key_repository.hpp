#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/data/file_lock.hpp"
#include "redvault/data/vault_repository.hpp"

namespace redvault::data {

/// One of the person's own file-locking keys (never the key itself: see FileKeyRepository::open).
struct FileKey {
    std::string id;  // the vault item's id
    std::string name;
    /// "7F3A-19C2-E04B-D5A8": the same on every device, safe to show.
    std::string fingerprint;
    /// Opening it needs its password as well as the account.
    bool passwordProtected = false;
    /// Brought in from outside the app rather than made here.
    bool imported = false;
    std::int64_t createdMs = 0;
};

/**
 * The person's own keys for locking files, kept as password-vault items of
 * type "filekey" (so they're sealed under the vault key and sync to every
 * device like passwords). A key can also have a password of its own: then
 * the item holds it wrapped under Argon2id(password), and only someone with
 * both the account and that password can use it.
 *
 * Item data: {"type": "filekey", "title", "fingerprint", "imported",
 * "created", "updated"} plus either "key" (base64, no password) or "lock":
 * {"v": 1, "salt", "ops", "mem", "ct", "nonce"} (wrap context
 * "redvault-file-key-v1|<fingerprint>").
 *
 * Blocking, and slow when a password is involved: run it off the UI thread.
 */
class FileKeyRepository {
public:
    explicit FileKeyRepository(std::shared_ptr<VaultRepository> vault);

    std::vector<FileKey> list();

    /// Makes a new random key. An empty password leaves it protected by the account (the vault) alone.
    FileKey create(const std::string& name, const std::string& password);
    /**
     * Brings in a key made outside the app (64 hex characters or base64 of 32
     * bytes). Refuses anything else, and a key that's already here.
     */
    FileKey import(const std::string& name, const std::string& keyText, const std::string& password);

    /// The key itself, to lock or unlock files with. Throws api::UserFacingError on a wrong password.
    crypto::SecretBytes open(const FileKey& key, const std::string& password);
    /// A lock for DriveRepository with this key, borrowing `secret` (from open()).
    static FileLock lockWith(const FileKey& key, const crypto::SecretBytes& secret);

    /// Sets, changes or (empty `newPassword`) removes the key's own password. Files locked with it stay as they are.
    FileKey setPassword(const FileKey& key, const std::string& currentPassword, const std::string& newPassword);
    FileKey rename(const FileKey& key, const std::string& name);
    /// The key as 64 hex characters, for keeping a copy outside the app.
    std::string exportHex(const FileKey& key, const std::string& password);
    /// Deletes it. Files locked with it can't be opened again unless it's imported back.
    void remove(const FileKey& key);

private:
    VaultEntry entryFor(const FileKey& key);
    FileKey save(VaultEntry entry, const crypto::SecretBytes& secret, const std::string& password);

    std::shared_ptr<VaultRepository> vault_;
};

/// Whether a vault item is a file key (the Passwords page leaves these out).
bool isFileKeyItem(const nlohmann::json& data);

}  // namespace redvault::data
