#include "redvault/crypto/commitment.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "redvault/crypto/base64.hpp"

namespace redvault::crypto::commitment {

std::string canonicalEnvelopeString(std::string_view messageId, ByteView ciphertext, ByteView nonce,
                                    int protocolVersion) {
    if (protocolVersion != kProtocolVersion) {
        throw std::invalid_argument("Unsupported commitment protocol version: " + std::to_string(protocolVersion));
    }
    std::string out = "v" + std::to_string(protocolVersion) + ":";
    out += messageId;
    out += ":" + base64Encode(ciphertext) + ":" + base64Encode(nonce);
    return out;
}

std::string computeHex(std::string_view messageId, ByteView ciphertext, ByteView nonce) {
    return toHex(sha256(toBytes(canonicalEnvelopeString(messageId, ciphertext, nonce))));
}

bool matches(std::string_view localHex, std::string_view networkHex) {
    return std::ranges::equal(localHex, networkHex, [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

}  // namespace redvault::crypto::commitment
