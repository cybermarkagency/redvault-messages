// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Port of ui/src/lib/recoveryKey.ts (and android/.../crypto/RecoveryKey.kt);
// tests/recovery_key_test.cpp pins all three to the same vector. The key is
// 32 random bytes, shown once as base32 in 13 groups of 4, and never leaves
// the device. From it, keyed BLAKE2b derives:
//   wrapKey   — encrypts a second copy of the private key (server can't open it)
//   authToken — proves the key to the server, which stores only an Argon2id hash

namespace redvault::crypto::recovery {

inline constexpr std::size_t kKeyBytes = 32;

struct RecoveryMaterial {
    std::string recoveryAuth;
    std::string recoveryEncryptedPrivateKey;
    std::string recoveryPrivateKeyNonce;
};

SecretBytes generate();

SecretBytes wrapKey(ByteView key);

SecretBytes authToken(ByteView key);

/// base64(authToken(key)), as sent to the server.
std::string authFor(ByteView key);

/// What the server needs so this key can restore the private key later.
RecoveryMaterial materialFor(ByteView key, ByteView privateKey);

/**
 * Unwraps the private key and checks it really is this account's key (its
 * public key must match), so a wrong or tampered copy is never adopted.
 * Throws CryptoError on any mismatch.
 */
SecretBytes unwrap(ByteView key, std::string_view encryptedPrivateKeyB64, std::string_view nonceB64,
                   std::string_view publicKeyB64);

/// "ABCD-EFGH-…" (52 base32 characters, 13 groups).
std::string format(ByteView key);

/**
 * Accepts any spacing, dashes and case, and the look-alikes 0/1/8 for O/I/B.
 * Empty if it isn't a valid key. (Unlike the Kotlin version, any non-ASCII
 * character rejects the input rather than being filtered out.)
 */
std::optional<SecretBytes> parse(std::string_view text);

}  // namespace redvault::crypto::recovery
