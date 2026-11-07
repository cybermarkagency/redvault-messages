#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/api/dtos.hpp"

// Encrypted cloud storage ("Files": server/src/routes/backupRoutes.ts) and
// the password manager (server/src/routes/vaultRoutes.ts). Only ciphertext,
// sizes and hashes cross these calls; everything readable is sealed on the
// device (crypto/drive_crypto.hpp, crypto/vault_crypto.hpp). All of them block.

namespace redvault::api::drive {

struct Usage {
    std::int64_t used = 0;
    std::int64_t limit = 0;
    std::int64_t remaining = 0;
    std::string tier;
};

struct Entry {
    std::string id;
    std::string kind;  // "file" or "folder"
    std::int64_t encryptedSize = 0;
    std::string ciphertextSha256;  // hex, files only
    std::string metaCiphertext;    // base64
    std::string metaNonce;         // base64
    std::vector<std::string> sharedWith;  // user ids
};

struct Listing {
    std::vector<Entry> items;
    Usage usage;
};

struct SharedEntry {
    std::string id;
    std::string ownerId;
    std::int64_t encryptedSize = 0;
    std::string ciphertextSha256;
    std::string sealedMeta;  // base64, sealed to this user's public key
};

struct Link {
    std::string id;
    std::string backupId;
    std::string sealedMeta;
    std::string metaNonce;
    bool hasPassword = false;
    bool allowDownload = false;
    std::optional<std::string> expiresAt;
};

struct UploadAuth {
    std::string objectId;
    TransferTargetDto upload;
    /// Resumable uploads only: each part's size (the last one is the rest) and how many there are.
    std::uint64_t partSize = 0;
    int partCount = 0;
};

/// One part the server already has (resumable uploads).
struct ReceivedPart {
    int partNumber = 0;
    std::uint64_t size = 0;
    std::string sha256;  // lowercase hex
};

Usage usage(ApiClient& api);
Listing list(ApiClient& api);
std::vector<SharedEntry> sharedWithMe(ApiClient& api);
Entry createFolder(ApiClient& api, const std::string& metaCiphertext, const std::string& metaNonce);
Entry updateMeta(ApiClient& api, const std::string& id, const std::string& metaCiphertext, const std::string& metaNonce);
/// ApiError 413 "backup_storage_full" (the message is ready to show) when it doesn't fit the plan.
UploadAuth requestUpload(ApiClient& api, std::int64_t encryptedSize, const std::string& ciphertextSha256Hex,
                         const std::string& metaCiphertext, const std::string& metaNonce);
void completeUpload(ApiClient& api, const std::string& id);
/// A resumable upload (db/migrations/034): `partsSha256Hex` is the SHA-256 over every part's digest, in order.
UploadAuth requestUploadParts(ApiClient& api, std::int64_t encryptedSize, const std::string& ciphertextSha256Hex,
                              const std::string& metaCiphertext, const std::string& metaNonce, std::uint64_t partSize,
                              const std::string& partsSha256Hex);
/// The parts the server has of a resumable upload.
std::vector<ReceivedPart> listParts(ApiClient& api, const std::string& id);
TransferTargetDto download(ApiClient& api, const std::string& id);
void remove(ApiClient& api, const std::string& id);
void share(ApiClient& api, const std::string& id, const std::string& recipientId, const std::string& sealedMeta);
void unshare(ApiClient& api, const std::string& id, const std::string& recipientId);
Link createLink(ApiClient& api, const std::string& id, const std::string& sealedMeta, const std::string& metaNonce,
                const std::optional<std::string>& passwordSalt, bool allowDownload, std::optional<int> expiresInDays);
std::vector<Link> links(ApiClient& api);
void deleteLink(ApiClient& api, const std::string& linkId);

}  // namespace redvault::api::drive

namespace redvault::api::vault {

struct Item {
    std::string id;
    std::string ciphertext;  // base64
    std::string nonce;       // base64
    int revision = 1;
};

struct Share {
    std::string itemId;
    std::string recipientId;
};

struct SharedItem {
    std::string itemId;
    std::string ownerId;
    std::string sealed;  // base64, sealed to this user's public key
};

std::vector<Item> list(ApiClient& api);
Item create(ApiClient& api, const std::string& ciphertext, const std::string& nonce);
/// ApiError 409 "vault_conflict" when another device changed it since `revision`.
Item update(ApiClient& api, const std::string& id, int revision, const std::string& ciphertext, const std::string& nonce);
void remove(ApiClient& api, const std::string& id);
std::vector<Share> shares(ApiClient& api);
std::vector<SharedItem> sharedWithMe(ApiClient& api);
void share(ApiClient& api, const std::string& itemId, const std::string& recipientId, const std::string& sealed);
void unshare(ApiClient& api, const std::string& itemId, const std::string& recipientId);
/// "SUFFIX:COUNT" lines for a 5-character SHA-1 prefix (crypto::vault::breachCount reads them).
std::string breachRange(ApiClient& api, const std::string& prefix);

}  // namespace redvault::api::vault
