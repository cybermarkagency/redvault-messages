#pragma once

#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

namespace redvault::crypto {

// Standard, padded base64: the network/DB boundary format the server, web
// client and Android client use for every key, nonce and ciphertext
// (libsodium's base64_variants.ORIGINAL).

std::string base64Encode(ByteView bytes);

/// Throws CryptoError on anything that isn't strict, padded standard base64.
Bytes base64Decode(std::string_view text);

}  // namespace redvault::crypto
