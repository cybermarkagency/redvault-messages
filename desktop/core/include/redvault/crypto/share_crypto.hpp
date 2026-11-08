// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "redvault/crypto/bytes.hpp"

// Public share links for Files (docs/public-share-links.md): the same keys,
// envelopes and sealed metadata as the web app (ui/src/lib/shareCrypto.ts),
// pinned to it by tests/vectors/share_vectors.json.
//
// A link's 32-byte secret lives only in the URL fragment. From it (and an
// Argon2id hash of the password, when the link has one) come two tokens the
// server keeps only SHA-256 digests of (lookup, access) and a wrap key that
// never leaves the device. The wrap key opens the envelope, which holds the
// link's share key; the share key opens each shared item's metadata. The
// owner keeps the secret and share key in a box sealed under their Files key.

namespace redvault::crypto::share {

/// crypto_kdf context of every share key.
constexpr const char* kKdfContext = "rvshare1";
/// Argon2id13 cost for new links (stored per link).
constexpr unsigned long long kDefaultOps = 3;
constexpr std::size_t kDefaultMem = 64ull * 1024 * 1024;

struct PasswordParams {
    Bytes salt;  // 16 bytes
    unsigned long long ops = kDefaultOps;
    std::size_t mem = kDefaultMem;
};

struct ShareKeys {
    SecretBytes lookup;  // sent as X-Share-Lookup; the server keeps SHA-256
    SecretBytes access;  // sent as X-Share-Access; the server keeps SHA-256
    SecretBytes wrap;    // never leaves the device: opens the envelope
};

struct Sealed {
    Bytes ciphertext;
    Bytes nonce;
};

struct Envelope {
    Bytes key;                    // the share key (32 bytes)
    std::optional<Bytes> upload;  // the owner's upload public key
    std::string root;             // item id of what was shared
};

/// The owner's own copy of a link's secrets, sealed under their Files key.
struct OwnerBox {
    std::string secret;  // base64url, what goes after # in the link
    std::string key;     // the share key, base64
    std::string root;    // item id of what was shared
    std::string label;   // only the owner sees it
    std::int64_t created = 0;  // epoch ms
};

struct KeyPair {
    Bytes publicKey;
    SecretBytes privateKey;
};

SecretBytes newLinkSecret();
Bytes newShareKey();
Bytes newSalt();
PasswordParams newPasswordParams();

SecretBytes lookupToken(ByteView secret);

/**
 * The link's keys. `password` must already be NFC-normalised UTF-8 (the app
 * does that with QString::normalized), and is required exactly when `params`
 * is set. Slow with a password (Argon2id): call off the UI thread. Throws
 * CryptoError for out-of-range parameters.
 */
ShareKeys deriveKeys(ByteView secret, std::optional<std::string_view> password, const std::optional<PasswordParams>& params);

/// What the server keeps for a token: base64 of its SHA-256.
std::string verifierOf(ByteView token);

/// `json` padded with spaces to the next multiple of `bucket` bytes (at least one space).
Bytes padJson(const nlohmann::json& json, std::size_t bucket);

Sealed sealEnvelope(ByteView wrap, const Envelope& envelope);
/// nullopt for a wrong key (a wrong password) or anything altered.
std::optional<Envelope> openEnvelope(ByteView wrap, ByteView ciphertext, ByteView nonce);

/// `meta` is a folder {"v","kind","name","parent"} or a file (also "mime","size","modified","key","header","chunk","sha").
Sealed sealItem(ByteView shareKey, std::string_view itemId, const nlohmann::json& meta);
/// The item's metadata, checked field by field; nullopt if it doesn't open under this key and id, or is malformed.
std::optional<nlohmann::json> openItem(ByteView shareKey, std::string_view itemId, ByteView ciphertext, ByteView nonce);

Sealed sealOwnerBox(ByteView driveKey, const OwnerBox& box);
std::optional<OwnerBox> openOwnerBox(ByteView driveKey, ByteView ciphertext, ByteView nonce);

/// The owner's upload key pair for one link: derived from the Files key and the link's salt, never stored.
KeyPair uploadKeyPair(ByteView driveKey, ByteView salt);

/// What an anonymous uploader sealed to the owner (name, mime, size, modified, sent, key, header, chunk, sha), checked; or nullopt.
std::optional<nlohmann::json> openContribution(const KeyPair& pair, ByteView sealed);

/// https://host/redvaultmessages/share/?id=<publicId>#<secret, base64url>.
std::string shareUrl(std::string_view webAppBase, std::string_view publicId, ByteView secret);

}  // namespace redvault::crypto::share
