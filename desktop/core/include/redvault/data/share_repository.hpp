// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

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
#include "redvault/api/share_api.hpp"
#include "redvault/crypto/share_crypto.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/drive_repository.hpp"

namespace redvault::data {

/// A public share link as its owner sees it: what the Files key opens of it.
struct OwnerShare {
    api::shares::Link link;
    /// nullopt when this account's Files key can't open the owner's copy.
    std::optional<crypto::share::OwnerBox> box;
    /// Empty when `box` is missing.
    std::string url;
    /// "active", "expired" or "revoked".
    std::string status;
};

struct ShareOptions {
    std::string permission = "viewer";  // or "contributor" (folders only)
    std::string label;
    /// NFC-normalised UTF-8; empty: no password.
    std::string password;
    std::optional<std::int64_t> expiresAtMs;
    std::optional<std::int64_t> maxDownloads;
    std::optional<std::int64_t> maxFileSize;
    std::optional<std::int64_t> allowance;
    std::optional<std::int64_t> maxFiles;
};

/// A file someone uploaded through a Contributor link, waiting for review.
struct IncomingFile {
    api::shares::Incoming dto;
    /// What the uploader sealed (name, mime, size, modified, sent, key, header, chunk, sha); nullopt if it can't be opened here.
    std::optional<nlohmann::json> meta;
    std::string name;    // the file's own name
    std::string folder;  // the uploader's folder path, if any ("Reports/2026")
};

struct ShareItemPlan {
    std::string itemId;
    std::string backupId;
    nlohmann::json meta;
};

struct ReconcileResult {
    int added = 0;
    int updated = 0;
    int removed = 0;
};

/// A folder link shows at most this many items (the server enforces it too).
constexpr std::size_t kMaxShareItems = 5000;

/**
 * Public share links for Files on this device (docs/public-share-links.md),
 * the same as the web app's lib/shareOwner.ts: making links, keeping a folder
 * link's items in step with Files, passwords, regenerating, and the files
 * people upload through Contributor links. Blocking; run it off the UI thread.
 */
class ShareRepository {
public:
    using ProgressFn = std::function<void(std::uint64_t done, std::uint64_t total)>;
    using Sleep = std::function<void(std::chrono::milliseconds)>;

    ShareRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity, std::filesystem::path cacheDir,
                    std::string webAppBase, Sleep sleep = {});

    struct Listing {
        std::vector<OwnerShare> links;
        std::vector<IncomingFile> incoming;
    };
    Listing refresh();

    OwnerShare decode(const api::shares::Link& link) const;
    IncomingFile decodeIncoming(const api::shares::Incoming& dto) const;

    /// Makes a link to a file or folder, then adds its items. Slow with a password (Argon2id).
    OwnerShare create(const DriveEntry& root, const std::vector<DriveEntry>& entries, const ShareOptions& options);

    /**
     * What a link should show: the shared file, or the folder and everything
     * in it outside the Trash, with the newest version of each file. Locked
     * files are left out. Item ids already in the link are kept.
     */
    static std::vector<ShareItemPlan> plan(const std::string& rootBackupId, const std::string& rootItemId, const std::vector<DriveEntry>& entries,
                                           const std::vector<api::shares::OwnerItem>& existing);

    /// Brings a link's items in line with Files (adds, re-seals, removes).
    ReconcileResult reconcile(const OwnerShare& share, const std::vector<DriveEntry>& entries,
                              const std::optional<std::vector<api::shares::OwnerItem>>& known = std::nullopt);
    /// Every live link; one failing doesn't stop the rest.
    void reconcileAll(const std::vector<OwnerShare>& shares, const std::vector<DriveEntry>& entries);

    /// Permission, expiry, limits (an ApiClient JSON body as in ui/src/lib/api.ts ShareSettingsInput), and a new name if given.
    OwnerShare updateSettings(const OwnerShare& share, const nlohmann::json& settings, const std::optional<std::string>& label);
    /// Sets, changes or (empty) removes the password; the share key and items stay.
    OwnerShare setPassword(const OwnerShare& share, const std::string& password);
    /// A new address, secret and share key; the old link stops working.
    OwnerShare regenerate(const OwnerShare& share, const std::string& password, const std::vector<DriveEntry>& entries);
    OwnerShare revoke(const OwnerShare& share);
    void remove(const OwnerShare& share);

    /// Downloads and decrypts an incoming file to `destination`. Nothing half-decrypted is left on failure.
    void downloadIncoming(const IncomingFile& file, const std::filesystem::path& destination, const ProgressFn& onProgress = {});
    /// Moves an incoming file into Files (into `parent`), re-sealing it with the Files key. No bytes move.
    DriveEntry acceptIncoming(const IncomingFile& file, const std::string& parent);
    void rejectIncoming(const IncomingFile& file);

private:
    nlohmann::json passwordJson(const std::optional<crypto::share::PasswordParams>& params) const;

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const SelfIdentity> identity_;
    std::filesystem::path cacheDir_;
    std::string webAppBase_;
    Sleep sleep_;
};

}  // namespace redvault::data
