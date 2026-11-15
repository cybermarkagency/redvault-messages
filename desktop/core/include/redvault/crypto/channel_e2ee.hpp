// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Private channels encrypted with the link (docs/signal-integration/
// 10-part-c1-private-channels.md). Not the Signal protocol: followers are
// anonymous, so everyone holding the current link's secret can open every
// epoch's content key, and with it the whole history.
//
//   wrapKey      = crypto_generichash(32, "redvault-channel-v1:wrap", key = secret)
//   wrapped key  = nonce(24) ‖ XChaCha20-Poly1305(ck[e], ad = "RVCHK|v1|<channelId>|<e>", wrapKey)
//   post         = XChaCha20-Poly1305(plaintext, ad = "RVCHP|v1|<channelId>|<e>", ck[e]), own 24-byte nonce
//
// The channel id is lowercased before it goes into the AD. Pinned to the
// Android app by tests/vectors/channel_e2ee_vectors.json.

namespace redvault::crypto::channel_e2ee {

inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kNonceBytes = 24;
/// nonce(24) ‖ ck(32) ‖ tag(16).
inline constexpr std::size_t kWrappedBytes = 72;
/// The server's limit on a post's ciphertext (64 KiB).
inline constexpr std::size_t kMaxCiphertextBytes = 64 * 1024;

/// The key every epoch's content key is wrapped under, from the link's secret (its base64url text). Throws CryptoError on a malformed secret.
SecretBytes wrapKey(std::string_view secret);

/// A fresh random content key for a new epoch.
SecretBytes newContentKey();

std::string keyAd(std::string_view channelId, int epoch);
std::string postAd(std::string_view channelId, int epoch);

/// The content key wrapped for the server (base64). `nonce` only for test vectors; normally random.
std::string wrap(ByteView contentKey, ByteView wrapKey, std::string_view channelId, int epoch, std::optional<ByteView> nonce = std::nullopt);

/// The content key, or nullopt when it won't open with this wrapKey, channel and epoch.
std::optional<SecretBytes> unwrap(std::string_view wrappedBase64, ByteView wrapKey, std::string_view channelId, int epoch);

/// A post's (or suggestion's) ciphertext, base64 as the server takes it.
struct Sealed {
    std::string nonce;
    std::string ciphertext;
};

/// Encrypts a post's plaintext. Throws CryptoError if the ciphertext would be over kMaxCiphertextBytes.
Sealed encrypt(std::string_view plaintext, ByteView contentKey, std::string_view channelId, int epoch,
               std::optional<ByteView> nonce = std::nullopt);

/// The plaintext, or nullopt for a wrong key, channel or epoch, or any changed byte.
std::optional<std::string> decrypt(std::string_view nonceBase64, std::string_view ciphertextBase64, ByteView contentKey,
                                   std::string_view channelId, int epoch);

}  // namespace redvault::crypto::channel_e2ee
