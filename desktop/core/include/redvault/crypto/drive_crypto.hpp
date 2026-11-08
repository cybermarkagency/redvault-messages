// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Encrypted cloud storage ("Files"), on the device
// (server/src/services/backupService.ts only stores ciphertext):
//
//   - Each file is encrypted with its own random key (crypto/file_crypto.hpp,
//     the same format as files sent in chats).
//   - Its metadata (name, folder, type, size, dates, version and that key)
//     is JSON sealed with XChaCha20-Poly1305 under the drive key, derived
//     from the account's private key ("redvault-drive-v1"), padded to
//     128-byte buckets.
//   - Shared with a person: the metadata sealed to their public key
//     (crypto::vault::sealForRecipient).
//   - Share link: the metadata sealed under a key from the link's secret,
//     which lives only in the URL fragment, mixed with an Argon2id hash of a
//     password when the link has one. The web viewer (ui/src/lib/
//     fileLink.ts) derives the same key.
//   - Locked files: the file's key can also be locked (wrapped) under a
//     key of the person's own (made here or imported, kept in the password
//     vault) or under a password, on top of the drive key
//     (data/file_lock.hpp).

namespace redvault::crypto::drive {

/// BLAKE2b(32, "redvault-drive-v1", key = private key).
SecretBytes driveKey(ByteView privateKey);

struct SealedMeta {
    Bytes ciphertext;
    Bytes nonce;
};

SealedMeta sealMeta(std::string_view json, ByteView key);
std::string openMeta(ByteView ciphertext, ByteView nonce, ByteView key);

/// 16 random bytes for a link's password.
Bytes newPasswordSalt();

/**
 * The key a link's metadata is sealed under. No password: the secret
 * itself. With one: BLAKE2b(32, secret, key = Argon2id(password, salt,
 * interactive limits)). Slow with a password (Argon2id): call off the UI thread.
 */
SecretBytes linkKey(ByteView secret, std::string_view password, ByteView salt);

/// https://host/redvaultmessages/?file=<linkId>#<secret, base64url>.
std::string fileLinkUrl(std::string_view webAppBase, std::string_view linkId, ByteView secret);

// --- Locking a file's key under a key or password of the person's own ---

constexpr std::size_t kFileLockKeyBytes = 32;

/// A new 256-bit key for locking files.
SecretBytes newFileLockKey();

/**
 * A 256-bit key made outside the app: 64 hex characters (openssl rand -hex
 * 32), optionally grouped with dashes or colons, or base64/base64url of 32
 * bytes. Whitespace is ignored. nullopt for anything else, including the
 * wrong length.
 */
std::optional<SecretBytes> parseFileLockKey(std::string_view text);

/// 64 lowercase hex characters: how a key is shown for backing it up outside the app.
std::string fileLockKeyHex(ByteView key);

/**
 * A short code identifying a key without revealing it, the same on every
 * device ("7F3A-19C2-E04B-D5A8"): BLAKE2b(8, "redvault-file-key-fingerprint-v1",
 * key = the key).
 */
std::string fileLockKeyFingerprint(ByteView key);

/// Argon2id settings stored next to whatever a password protects, so they can be raised later.
struct PasswordKdf {
    Bytes salt;
    unsigned long long opsLimit = 0;
    std::size_t memLimit = 0;
};

/// A fresh salt with libsodium's MODERATE limits (about 256 MiB, under a second on a desktop).
PasswordKdf newPasswordKdf();

/**
 * Argon2id(password) under `kdf`. Refuses settings outside libsodium's
 * interactive..sensitive range, so stored metadata can't make this app
 * spend unbounded time or memory. Slow: call off the UI thread.
 */
SecretBytes passwordKey(std::string_view password, const PasswordKdf& kdf);

struct WrappedKey {
    Bytes ciphertext;
    Bytes nonce;
};

/**
 * Locks `key` under `wrappingKey` with XChaCha20-Poly1305. `context` is
 * authenticated with it (not stored), so a wrapped key only opens for the
 * purpose and file it was made for.
 */
WrappedKey wrapKey(ByteView key, ByteView wrappingKey, std::string_view context);
/// Throws CryptoError for a wrong key or anything altered.
SecretBytes unwrapKey(const WrappedKey& wrapped, ByteView wrappingKey, std::string_view context);

}  // namespace redvault::crypto::drive
