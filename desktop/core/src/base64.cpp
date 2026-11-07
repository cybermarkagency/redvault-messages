#include "redvault/crypto/base64.hpp"

#include <sodium.h>

namespace redvault::crypto {

std::string base64Encode(ByteView bytes) {
    ensureSodium();
    constexpr int kVariant = sodium_base64_VARIANT_ORIGINAL;
    // The encoded length includes the trailing NUL that sodium writes.
    std::string out(sodium_base64_ENCODED_LEN(bytes.size(), kVariant), '\0');
    sodium_bin2base64(out.data(), out.size(), bytes.data(), bytes.size(), kVariant);
    out.pop_back();
    return out;
}

Bytes base64Decode(std::string_view text) {
    ensureSodium();
    Bytes out(text.size() / 4 * 3 + 3);
    std::size_t length = 0;
    // A null b64_end makes sodium reject any trailing, unparsed characters.
    if (sodium_base642bin(out.data(), out.size(), text.data(), text.size(), nullptr, &length, nullptr,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        throw CryptoError("Invalid base64");
    }
    out.resize(length);
    return out;
}

}  // namespace redvault::crypto
