// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace redvault::crypto {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

class CryptoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Calls sodium_init() once. Every entry point in this library calls it, so callers never need to.
void ensureSodium();

/**
 * Heap buffer for key material (private keys, wrap keys, conversation keys).
 * Allocated with sodium_malloc, so it sits between guard pages, is mlocked
 * where the OS allows (kept out of swap), and is zeroed when freed. Move-only,
 * so a secret is never silently copied into ordinary memory.
 */
class SecretBytes {
public:
    SecretBytes() = default;
    explicit SecretBytes(std::size_t size);
    static SecretBytes copyOf(ByteView bytes);
    ~SecretBytes();

    SecretBytes(SecretBytes&& other) noexcept;
    SecretBytes& operator=(SecretBytes&& other) noexcept;
    SecretBytes(const SecretBytes&) = delete;
    SecretBytes& operator=(const SecretBytes&) = delete;

    std::uint8_t* data() noexcept { return data_; }
    const std::uint8_t* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    ByteView view() const noexcept { return {data_, size_}; }
    operator ByteView() const noexcept { return view(); }

    /// Shrinks the logical size (e.g. after a variable-length decrypt). Never grows.
    void truncate(std::size_t size) noexcept;

private:
    std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

/// The UTF-8 bytes of a string, as libsodium.js and Kotlin's toByteArray(UTF_8) see them.
Bytes toBytes(std::string_view utf8);

/// Lowercase hex.
std::string toHex(ByteView bytes);

Bytes sha256(ByteView input);

Bytes randomBytes(std::size_t size);

/// Constant-time equality for secrets (sodium_memcmp); false if sizes differ.
bool constantTimeEquals(ByteView a, ByteView b);

}  // namespace redvault::crypto
