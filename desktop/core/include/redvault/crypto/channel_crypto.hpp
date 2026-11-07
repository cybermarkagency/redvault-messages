#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Private channel links, byte-compatible with the web app (ui/src/lib/channels.ts):
// <web app>/?channel=<id>#<secret>. The 32-byte secret stays in the URL
// fragment, which no server ever sees; readers send an access token derived
// from it (X-Channel-Access), and the server keeps only that token's SHA-256.
// The owner keeps the secret sealed to their own public key on the server,
// so their devices can show the link again. Posts are still plain text on
// the server, as in public channels: the link only decides who can read.
//
// The secret travels as its base64url text (no padding, 43 characters), and
// that text is what gets sealed. Pinned to the web app by
// tests/channel_crypto_test.cpp (vectors/channel_vectors.json, made with the
// web app's own code).

namespace redvault::crypto::channels {

inline constexpr std::size_t kSecretBytes = 32;

/// A new private link's secret, as the link carries it (base64url, no padding).
std::string newSecret();

/// The secret from a link's fragment (a leading '#' is fine), or nullopt unless it's exactly 32 bytes of strict base64url.
std::optional<std::string> secretFromFragment(std::string_view fragment);

/// What a reader shows the server (base64): derived from the secret, which can't be worked back from it. Throws CryptoError on a malformed secret.
std::string accessToken(std::string_view secret);

/// The secret sealed to the owner's public key (base64), for the server to keep.
std::string sealSecret(std::string_view secret, ByteView ownerPublicKey);

/// The owner's secret, unsealed with their key pair; nullopt when there's none, it won't open, or it isn't a secret.
std::optional<std::string> openSecret(const std::optional<std::string>& sealed, ByteView publicKey, ByteView privateKey);

}  // namespace redvault::crypto::channels
