// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/bytes.hpp"

#include <sodium.h>

#include <algorithm>
#include <mutex>
#include <new>
#include <utility>

namespace redvault::crypto {

void ensureSodium() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (sodium_init() < 0) throw CryptoError("libsodium failed to initialise");
    });
}

SecretBytes::SecretBytes(std::size_t size) {
    if (size == 0) return;
    ensureSodium();
    data_ = static_cast<std::uint8_t*>(sodium_malloc(size));
    if (data_ == nullptr) throw std::bad_alloc();
    size_ = size;
}

SecretBytes SecretBytes::copyOf(ByteView bytes) {
    SecretBytes out(bytes.size());
    std::copy(bytes.begin(), bytes.end(), out.data());
    return out;
}

SecretBytes::~SecretBytes() {
    // sodium_free zeroes the whole allocation before releasing it.
    if (data_ != nullptr) sodium_free(data_);
}

SecretBytes::SecretBytes(SecretBytes&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}

SecretBytes& SecretBytes::operator=(SecretBytes&& other) noexcept {
    if (this != &other) {
        if (data_ != nullptr) sodium_free(data_);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void SecretBytes::truncate(std::size_t size) noexcept {
    if (size >= size_) return;
    sodium_memzero(data_ + size, size_ - size);
    size_ = size;
}

Bytes toBytes(std::string_view utf8) {
    return {utf8.begin(), utf8.end()};
}

std::string toHex(ByteView bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 0x0f]);
    }
    return out;
}

Bytes sha256(ByteView input) {
    ensureSodium();
    Bytes out(crypto_hash_sha256_BYTES);
    crypto_hash_sha256(out.data(), input.data(), input.size());
    return out;
}

Bytes randomBytes(std::size_t size) {
    ensureSodium();
    Bytes out(size);
    randombytes_buf(out.data(), out.size());
    return out;
}

bool constantTimeEquals(ByteView a, ByteView b) {
    ensureSodium();
    return a.size() == b.size() && sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace redvault::crypto
