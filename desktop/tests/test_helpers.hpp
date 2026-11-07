#pragma once

#include <string>
#include <string_view>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/bytes.hpp"

namespace redvault::test {

inline crypto::Bytes b64(std::string_view text) {
    return crypto::base64Decode(text);
}

inline crypto::Bytes copy(crypto::ByteView view) {
    return {view.begin(), view.end()};
}

inline std::string joined(const std::vector<std::string>& groups) {
    std::string out;
    for (const auto& group : groups) out += group;
    return out;
}

}  // namespace redvault::test
