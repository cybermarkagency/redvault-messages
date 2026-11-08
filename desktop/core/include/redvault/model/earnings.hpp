// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// Paid messages, tips and exclusive posts on this device, as the web app has
// them (ui/src/lib/postCrypto.ts, earningsApi.ts). Every price, fee and
// payment status comes from the server; what's here only shows amounts,
// reads a typed amount, and parses a decrypted post body (untrusted input:
// someone else wrote it).

namespace redvault::model::earnings {

class PostFormatError : public std::runtime_error {
public:
    PostFormatError() : std::runtime_error("Malformed post") {}
};

inline constexpr std::size_t kMaxPostText = 10'000;  // UTF-16 units, like the web
inline constexpr std::size_t kMaxTeaser = 280;
/// The tip buttons ($1, $3, $5, $10); the server checks any amount against its own range.
inline constexpr std::int64_t kTipPresetsMinor[] = {100, 300, 500, 1000};

/// An encrypted photo or video (crypto/file_crypto.hpp); key and header base64.
struct PostMedia {
    std::string key;
    std::string header;
    std::string sha256;
    std::uint64_t encryptedSize = 0;
    std::uint64_t chunkSize = 0;
    std::string mime;
};

struct PostBody {
    std::string text;
    std::optional<PostMedia> media;
};

/// Throws PostFormatError unless it's a well-formed post body (the web's checks).
PostBody parseBody(std::string_view json);
/// The JSON the web app writes: {"v":1,"text":…,"media":{…}} in that order.
std::string toJson(const PostBody& body);

/// "$3.00": every amount is US cents from the server.
std::string formatUsd(std::int64_t minor);
/// "3", "3.5", "$3.50" → 350. Nullopt when it isn't an amount (or has more than 2 decimals).
std::optional<std::int64_t> parseUsd(std::string_view text);

/// "Paid", "Refunded", …: a purchase status as people read it.
std::string statusName(std::string_view status);
/// "Paid messages", "Tips", "Exclusive posts".
std::string productName(std::string_view product);

}  // namespace redvault::model::earnings
