// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <atomic>

#include "redvault/api/api_error.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "redvault/api/api_client.hpp"
#include "redvault/api/drive_api.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/file_lock.hpp"
#include "redvault/data/key_trust.hpp"

namespace redvault::data {

/// One file or folder in Files, decrypted on this device.
struct DriveEntry {
    std::string id;
    std::string kind;  // "file" or "folder"
    std::string name;
    /// The folder it's in (empty: the top level).
    std::string parent;
    std::string mimeType;
    std::int64_t size = 0;           // plaintext bytes
    std::int64_t encryptedSize = 0;  // what counts toward storage
    std::int64_t modifiedMs = 0;
    /// Versions of one file share a fileId; the highest `version` is the current one.
    std::string fileId;
    int version = 1;
    bool trashed = false;
    std::int64_t trashedMs = 0;
    /// Whom the owner shared it with (user ids). Only for the owner's own entries.
    std::vector<std::string> sharedWith;
    /// Someone else's file, shared with this account (read-only).
    bool incoming = false;
    std::string ownerId;
    /// Everything that was sealed: the file key and header, link secrets, ...
    nlohmann::json meta;

    /// Locked under a key or password of the person's own (data/file_lock.hpp).
    bool locked() const { return filelock::isLocked(meta); }
    /// Locked, and its key hasn't been unlocked in this copy (see DriveRepository::unlock).
    bool needsUnlock() const { return locked() && !meta.contains("key"); }
};

struct DriveSnapshot {
    std::vector<DriveEntry> entries;   // own files (every version) and folders
    std::vector<DriveEntry> incoming;  // shared with this account
    std::vector<api::drive::Link> links;
    api::drive::Usage usage;
};

enum class DriveStage { Encrypting, Uploading, Downloading, Decrypting };

/// Shared between the UI and a transfer running on a worker: setting it stops the transfer at the next chunk.
struct TransferControl {
    std::atomic<bool> cancelled{false};
};

/// The person cancelled the transfer.
class TransferCancelled : public api::UserFacingError {
public:
    using api::UserFacingError::UserFacingError;
};

/// A resumable upload that didn't finish (no connection, Tor down, the app quit), from the sealed journal.
struct PendingUpload {
    std::string objectId;
    std::string name;
    std::string parent;
    std::int64_t size = 0;
    int partCount = 0;
};

/**
 * Encrypted cloud storage on this device: encrypts files (and their names,
 * folders and types) before upload, decrypts them after download, and keeps
 * every readable detail in metadata sealed under the drive key
 * (crypto/drive_crypto.hpp). Blocking; run it off the UI thread.
 */
class DriveRepository {
public:
    using ProgressFn = std::function<void(DriveStage stage, std::uint64_t done, std::uint64_t total)>;
    using Sleep = std::function<void(std::chrono::milliseconds)>;

    /// `contacts`: public key pins, checked before anything is sealed to someone.
    DriveRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity, std::filesystem::path cacheDir,
                    Sleep sleep = {}, std::shared_ptr<ContactKeyPins> contacts = nullptr);

    DriveSnapshot refresh();

    DriveEntry createFolder(const std::string& name, const std::string& parent);

    /**
     * Encrypts and uploads a file into `parent`. With `replacing`, it becomes
     * that file's next version. Throws UserFacingError with the server's own
     * wording when it doesn't fit the plan's storage ("Not enough storage: ...").
     *
     * With `lock`, the file's key is also locked under that key or password
     * (a password must pass filelock::passwordProblem). The returned entry
     * comes back unlocked, for this session only.
     */
    DriveEntry upload(const std::filesystem::path& file, const std::string& parent, const std::optional<DriveEntry>& replacing,
                      const ProgressFn& onProgress = {}, const nlohmann::json& extraMeta = nlohmann::json::object(),
                      const FileLock* lock = nullptr, std::shared_ptr<TransferControl> control = {});

    /**
     * Resumable transfers (docs/tor-routing-plan.md), for Tor, where a big
     * file can take hours and the circuit can drop:
     * - uploads go in parts (kPartSize). The encrypted copy and a journal
     *   sealed under `journalKey` stay in `journalDir` until the server has
     *   every part, so an interrupted upload, even across a restart, resumes
     *   from the first missing part (each part checked by SHA-256);
     * - downloads continue where they stopped (HTTP Range), and the
     *   ciphertext is verified against its SHA-256 before it's decrypted.
     * Needs the server's parts support (db/migrations/034).
     */
    void enableResumable(std::filesystem::path journalDir, crypto::SecretBytes journalKey);
    bool resumable() const { return !journalDir_.empty(); }
    /// Uploads that didn't finish, oldest first. Records that don't open (another account's key) are skipped.
    std::vector<PendingUpload> pendingUploads() const;
    /// Sends what the server is missing of a pending upload, then completes it.
    DriveEntry resumeUpload(const std::string& objectId, const ProgressFn& onProgress = {}, std::shared_ptr<TransferControl> control = {});
    /// Cancels a pending upload for good: the server drops it, and the local copy and journal go.
    void abandonUpload(const std::string& objectId);
    /// crypto_kdf_derive_from_key(32, 1, "rvxfer01", account private key).
    static crypto::SecretBytes deriveJournalKey(const crypto::SecretBytes& accountPrivateKey);
    static constexpr std::uint64_t kPartSize = 8ull * 1024 * 1024;

    /**
     * A copy of a locked entry with its file key unlocked, in memory only
     * (it's never saved that way). Throws UserFacingError for a wrong key or
     * password. Slow for a password (Argon2id).
     */
    DriveEntry unlock(const DriveEntry& entry, const FileLock& with) const;

    /**
     * Locks an entry's key under `lock`, changes its lock, or (null) removes
     * it. The entry must be unlocked first (or not locked). Only this version
     * changes; the file itself isn't re-encrypted or re-uploaded.
     */
    DriveEntry setLock(const DriveEntry& entry, const FileLock* lock);

    /// Downloads and decrypts to `destination` (a file path). Nothing half-decrypted is left behind on failure.
    /// A locked entry must be unlocked first.
    void download(const DriveEntry& entry, const std::filesystem::path& destination, const ProgressFn& onProgress = {},
                  std::shared_ptr<TransferControl> control = {});
    /// Downloads and decrypts into this session's cache (for previews); returns the path.
    std::filesystem::path downloadToCache(const DriveEntry& entry, const ProgressFn& onProgress = {});

    /// Rename, move, trash or restore: `entry.name`, `.parent`, `.trashed` are re-sealed as they are.
    DriveEntry save(const DriveEntry& entry);
    /// Deletes for good (every version when `allVersions`), freeing the space.
    void remove(const DriveEntry& entry);

    /// Shares the current version with another account: its metadata sealed to their public key.
    /// A locked entry must be unlocked first; the recipient gets it without the lock.
    void share(const DriveEntry& entry, const std::string& recipientId);
    void unshare(const DriveEntry& entry, const std::string& recipientId);

    struct LinkOptions {
        std::string password;  // empty: no password
        std::optional<int> expiresInDays = 7;
        bool allowDownload = false;
    };
    /// A share link; returns its URL (the secret is in the fragment). The secret is kept in the file's own metadata so the link can be copied again.
    /// A locked entry must be unlocked first.
    std::string createLink(DriveEntry& entry, const LinkOptions& options, const std::string& webAppBase);
    void deleteLink(DriveEntry& entry, const std::string& linkId);
    /// The URL of an existing link of this file, or empty.
    std::string linkUrl(const DriveEntry& entry, const std::string& linkId, const std::string& webAppBase) const;

    void clearCache();

private:
    DriveEntry decode(const api::drive::Entry& entry) const;
    std::string sealedMetaJson(const DriveEntry& entry) const;
    /// The metadata as someone it's shared with gets it: the file key in the clear (inside their sealed copy), no lock.
    std::string recipientMetaJson(const DriveEntry& entry) const;

    DriveEntry sendParts(const nlohmann::json& journal, const ProgressFn& onProgress, const std::shared_ptr<TransferControl>& control);
    void writeJournal(const std::string& objectId, const nlohmann::json& journal) const;
    std::optional<nlohmann::json> readJournal(const std::string& objectId) const;

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const SelfIdentity> identity_;
    std::shared_ptr<ContactKeyPins> contacts_;
    std::filesystem::path cacheDir_;
    Sleep sleep_;
    std::filesystem::path journalDir_;
    crypto::SecretBytes journalKey_;
};

}  // namespace redvault::data
