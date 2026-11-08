// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Port of ui/src/lib/fileCrypto.ts (and android/.../crypto/FileCrypto.kt),
// format version 1: libsodium's crypto_secretstream_xchacha20poly1305 over
// 64 KiB plaintext chunks, with a fresh random key and header per file. The
// last chunk carries TAG_FINAL, so truncating, reordering, dropping or
// appending chunks all fail authentication. The key and header travel only
// inside the E2EE message; the server only sees the ciphertext and its SHA-256.
//
// Both directions stream one chunk at a time, so a large file is never held
// in memory whole.

namespace redvault::crypto::files {

/// Must match server/src/services/fileService.ts FILE_CHUNK_BYTES.
inline constexpr std::size_t kChunkBytes = 64 * 1024;
inline constexpr std::size_t kChunkOverheadBytes = 17;  // crypto_secretstream_xchacha20poly1305_ABYTES
inline constexpr std::string_view kAlgorithm = "xchacha20poly1305-secretstream";
inline constexpr int kFormatVersion = 1;

class FileDecryptError : public std::runtime_error {
public:
    FileDecryptError() : std::runtime_error("Unable to decrypt this file. It may be corrupted or tampered with.") {}
};

struct EncryptedFile {
    SecretBytes key;
    Bytes header;
    std::uint64_t plaintextSize = 0;
    std::uint64_t encryptedSize = 0;
    /// Lowercase hex SHA-256 of the ciphertext.
    std::string ciphertextSha256;
};

using ProgressFn = std::function<void(std::uint64_t bytes)>;

/// Same formula as the web's encryptedSizeFor(): an empty file is one empty FINAL chunk.
std::uint64_t encryptedSizeFor(std::uint64_t plaintextSize);

/// Encrypts `input` into `output`, returning what the recipient needs to decrypt it. Progress reports plaintext bytes.
EncryptedFile encrypt(std::istream& input, std::ostream& output, const ProgressFn& onProgress = {});

/**
 * Decrypts a ciphertext stream into `output`. Throws FileDecryptError on any
 * authentication failure, truncation, trailing data, size mismatch or hash
 * mismatch; `output` must then be discarded (the caller deletes it).
 * Progress reports ciphertext bytes.
 */
void decrypt(std::istream& input, std::ostream& output, ByteView key, ByteView header, std::uint64_t encryptedSize,
             std::string_view ciphertextSha256, const ProgressFn& onProgress = {});

}  // namespace redvault::crypto::files
