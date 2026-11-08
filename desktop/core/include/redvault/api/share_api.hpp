// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/api/drive_api.hpp"

// Public share links for Files, the owner's side (server/src/routes/shareRoutes.ts,
// docs/public-share-links.md). Only verifiers, ciphertext and limits cross
// these calls; the link's secret, the share key and every name are sealed on
// this device (crypto/share_crypto.hpp). All of them block.

namespace redvault::api::shares {

struct Password {
    std::string salt;  // base64
    unsigned long long ops = 0;
    std::size_t mem = 0;
};

struct UploadLimits {
    std::optional<std::int64_t> maxFileSize;
    std::optional<std::int64_t> allowance;
    std::optional<std::int64_t> maxFiles;
    std::int64_t count = 0;
    std::int64_t bytes = 0;
    std::int64_t inProgress = 0;
};

struct Link {
    std::string id;
    std::string publicId;
    std::string backupId;  // the shared file or folder (never in the link)
    std::string kind;      // "file" or "folder"
    std::string permission;  // "viewer" or "contributor"
    int keyEpoch = 1;
    std::optional<Password> password;
    std::string envelope;
    std::string envelopeNonce;
    std::string ownerBox;
    std::string ownerBoxNonce;
    std::string uploadKeySalt;
    std::optional<std::string> expiresAt;
    bool expired = false;
    bool revoked = false;
    std::optional<std::int64_t> maxDownloads;
    std::int64_t downloadCount = 0;
    std::int64_t openCount = 0;
    UploadLimits upload;
    /// Files that came in through it and wait for review.
    int incoming = 0;
};

struct LinkList {
    std::vector<Link> links;
    int incoming = 0;
};

struct OwnerItem {
    std::string itemId;
    std::string backupId;
    std::string kind;
    std::string metaCiphertext;
    std::string metaNonce;
};

struct ItemInput {
    std::string itemId;
    std::string backupId;
    std::string metaCiphertext;
    std::string metaNonce;
};

struct Incoming {
    std::string id;
    std::optional<std::string> shareId;  // null once its link was deleted
    std::int64_t encryptedSize = 0;
    std::string ciphertextSha256;
    std::string sealedMeta;  // base64, sealed to the link's upload key
    std::string keySalt;     // base64
};

LinkList list(ApiClient& api);
/// `body`: NewShareLinkInput (see ui/src/lib/api.ts).
Link create(ApiClient& api, const Json& body);
Link update(ApiClient& api, const std::string& id, const Json& body);
Link setPassword(ApiClient& api, const std::string& id, const Json& body);
Link regenerate(ApiClient& api, const std::string& id, const Json& body);
Link revoke(ApiClient& api, const std::string& id);
void remove(ApiClient& api, const std::string& id);

struct Items {
    int keyEpoch = 1;
    std::vector<OwnerItem> items;
};
Items items(ApiClient& api, const std::string& id);
int upsertItems(ApiClient& api, const std::string& id, int keyEpoch, const std::vector<ItemInput>& items);
int removeItems(ApiClient& api, const std::string& id, const std::vector<std::string>& itemIds);

std::vector<Incoming> incoming(ApiClient& api);
/// Moves an incoming file into Files under metadata sealed with the Files key; returns the new entry.
drive::Entry accept(ApiClient& api, const std::string& id, const std::string& metaCiphertext, const std::string& metaNonce);
void reject(ApiClient& api, const std::string& id);

}  // namespace redvault::api::shares
