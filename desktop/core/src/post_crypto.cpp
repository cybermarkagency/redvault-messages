// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/post_crypto.hpp"

#include <sodium.h>

#include <cstring>

namespace redvault::crypto::posts {

namespace {

constexpr std::uint8_t kMagic[4] = {0x52, 0x56, 0x45, 0x50};  // "RVEP"
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kIdBytes = 16;
constexpr std::size_t kPayloadBytes = 4 + 1 + kIdBytes + kKeyBytes;
constexpr std::string_view kBodyLabel = "redvault-post-body-v1";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Bytes uuidBytes(std::string_view id) {
    std::string hex;
    for (const char c : id) {
        if (c != '-') hex += c;
    }
    if (hex.size() != 32) throw CryptoError("Not a post id");
    Bytes out(kIdBytes);
    for (std::size_t i = 0; i < kIdBytes; ++i) {
        const int hi = hexValue(hex[i * 2]);
        const int lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) throw CryptoError("Not a post id");
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return out;
}

/// label bytes || id (16): the additional data that binds a body to one post.
Bytes boundTo(std::string_view id) {
    Bytes out(kBodyLabel.begin(), kBodyLabel.end());
    const Bytes idBytes = uuidBytes(id);
    out.insert(out.end(), idBytes.begin(), idBytes.end());
    return out;
}

}  // namespace

SecretBytes newPostKey() {
    ensureSodium();
    SecretBytes key(kKeyBytes);
    crypto_aead_xchacha20poly1305_ietf_keygen(key.data());
    return key;
}

Bytes wrapPostKey(std::string_view postId, ByteView key, ByteView recipientPublicKey, ByteView authorPrivateKey) {
    ensureSodium();
    if (key.size() != kKeyBytes) throw CryptoError("Post keys are 32 bytes");
    if (recipientPublicKey.size() != crypto_box_PUBLICKEYBYTES || authorPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw CryptoError("Wrong key size");
    }
    SecretBytes payload(kPayloadBytes);
    std::memcpy(payload.data(), kMagic, 4);
    payload.data()[4] = kVersion;
    const Bytes id = uuidBytes(postId);
    std::memcpy(payload.data() + 5, id.data(), kIdBytes);
    std::memcpy(payload.data() + 5 + kIdBytes, key.data(), kKeyBytes);

    Bytes out(crypto_box_NONCEBYTES + kPayloadBytes + crypto_box_MACBYTES);
    randombytes_buf(out.data(), crypto_box_NONCEBYTES);
    if (crypto_box_easy(out.data() + crypto_box_NONCEBYTES, payload.data(), payload.size(), out.data(), recipientPublicKey.data(),
                        authorPrivateKey.data()) != 0) {
        throw CryptoError("Couldn't seal the post key");
    }
    return out;
}

SecretBytes unwrapPostKey(std::string_view postId, ByteView wrapped, ByteView authorPublicKey, ByteView readerPrivateKey) {
    ensureSodium();
    if (wrapped.size() != kWrappedKeyBytes) throw CryptoError("Malformed post key");
    if (authorPublicKey.size() != crypto_box_PUBLICKEYBYTES || readerPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw CryptoError("Wrong key size");
    }
    SecretBytes payload(kPayloadBytes);
    if (crypto_box_open_easy(payload.data(), wrapped.data() + crypto_box_NONCEBYTES, wrapped.size() - crypto_box_NONCEBYTES, wrapped.data(),
                             authorPublicKey.data(), readerPrivateKey.data()) != 0) {
        throw CryptoError("This post key wasn't made by the creator for you");
    }
    if (std::memcmp(payload.data(), kMagic, 4) != 0 || payload.data()[4] != kVersion) throw CryptoError("Unknown post key format");
    const Bytes id = uuidBytes(postId);
    if (sodium_memcmp(payload.data() + 5, id.data(), kIdBytes) != 0) throw CryptoError("This key belongs to another post");
    return SecretBytes::copyOf(ByteView(payload.data() + 5 + kIdBytes, kKeyBytes));
}

Sealed sealBody(std::string_view postId, ByteView key, std::string_view bodyJson) {
    ensureSodium();
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw CryptoError("Wrong key size");
    const Bytes ad = boundTo(postId);
    Sealed out;
    out.nonce.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    randombytes_buf(out.nonce.data(), out.nonce.size());
    out.ciphertext.resize(bodyJson.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.ciphertext.data(), &length, reinterpret_cast<const unsigned char*>(bodyJson.data()),
                                               bodyJson.size(), ad.data(), ad.size(), nullptr, out.nonce.data(), key.data());
    out.ciphertext.resize(static_cast<std::size_t>(length));
    return out;
}

std::string openBody(std::string_view postId, ByteView key, ByteView ciphertext, ByteView nonce) {
    ensureSodium();
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES || nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES ||
        ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        throw CryptoError("This post can't be opened");
    }
    const Bytes ad = boundTo(postId);
    std::string out(ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(out.data()), &length, nullptr, ciphertext.data(),
                                                   ciphertext.size(), ad.data(), ad.size(), nonce.data(), key.data()) != 0) {
        throw CryptoError("This post can't be opened");
    }
    out.resize(static_cast<std::size_t>(length));
    return out;
}

}  // namespace redvault::crypto::posts
