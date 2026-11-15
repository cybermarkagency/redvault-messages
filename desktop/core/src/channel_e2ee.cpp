// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/channel_e2ee.hpp"

#include <sodium.h>

#include <algorithm>
#include <cctype>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"

namespace redvault::crypto::channel_e2ee {

namespace {

constexpr std::string_view kWrapLabel = "redvault-channel-v1:wrap";

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

const unsigned char* u8(std::string_view s) {
    return reinterpret_cast<const unsigned char*>(s.data());
}

std::optional<Bytes> decoded(std::string_view text) {
    try {
        return base64Decode(text);
    } catch (const CryptoError&) {
        return std::nullopt;
    }
}

}  // namespace

SecretBytes wrapKey(std::string_view secret) {
    ensureSodium();
    const auto bytes = secret.starts_with('#') ? std::nullopt : calls::linkSecretFromFragment(secret);
    if (!bytes) throw CryptoError("Not a channel link secret");
    SecretBytes key(kKeyBytes);
    crypto_generichash(key.data(), key.size(), u8(kWrapLabel), kWrapLabel.size(), bytes->data(), bytes->size());
    return key;
}

SecretBytes newContentKey() {
    ensureSodium();
    SecretBytes key(kKeyBytes);
    randombytes_buf(key.data(), key.size());
    return key;
}

std::string keyAd(std::string_view channelId, int epoch) {
    return "RVCHK|v1|" + lower(channelId) + "|" + std::to_string(epoch);
}

std::string postAd(std::string_view channelId, int epoch) {
    return "RVCHP|v1|" + lower(channelId) + "|" + std::to_string(epoch);
}

std::string wrap(ByteView contentKey, ByteView wrapKey, std::string_view channelId, int epoch, std::optional<ByteView> nonce) {
    ensureSodium();
    if (contentKey.size() != kKeyBytes || wrapKey.size() != kKeyBytes || epoch < 1) throw CryptoError("Can't wrap that channel key");
    Bytes out(kWrappedBytes);
    if (nonce) {
        if (nonce->size() != kNonceBytes) throw CryptoError("Wrong nonce size");
        std::copy(nonce->begin(), nonce->end(), out.begin());
    } else {
        randombytes_buf(out.data(), kNonceBytes);
    }
    const std::string ad = keyAd(channelId, epoch);
    unsigned long long written = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data() + kNonceBytes, &written, contentKey.data(), contentKey.size(), u8(ad), ad.size(),
                                               nullptr, out.data(), wrapKey.data());
    return base64Encode(out);
}

std::optional<SecretBytes> unwrap(std::string_view wrappedBase64, ByteView wrapKey, std::string_view channelId, int epoch) {
    ensureSodium();
    if (wrapKey.size() != kKeyBytes || epoch < 1) return std::nullopt;
    const auto box = decoded(wrappedBase64);
    if (!box || box->size() != kWrappedBytes) return std::nullopt;
    const std::string ad = keyAd(channelId, epoch);
    SecretBytes key(kKeyBytes);
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(key.data(), &written, nullptr, box->data() + kNonceBytes, box->size() - kNonceBytes, u8(ad),
                                                   ad.size(), box->data(), wrapKey.data()) != 0 ||
        written != kKeyBytes) {
        return std::nullopt;
    }
    return key;
}

Sealed encrypt(std::string_view plaintext, ByteView contentKey, std::string_view channelId, int epoch, std::optional<ByteView> nonce) {
    ensureSodium();
    if (contentKey.size() != kKeyBytes || epoch < 1) throw CryptoError("Can't encrypt with that channel key");
    const std::size_t size = plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES;
    if (size > kMaxCiphertextBytes) throw CryptoError("This post is too long to encrypt");
    Bytes n(kNonceBytes);
    if (nonce) {
        if (nonce->size() != kNonceBytes) throw CryptoError("Wrong nonce size");
        std::copy(nonce->begin(), nonce->end(), n.begin());
    } else {
        randombytes_buf(n.data(), n.size());
    }
    const std::string ad = postAd(channelId, epoch);
    Bytes out(size);
    unsigned long long written = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.data(), &written, u8(plaintext), plaintext.size(), u8(ad), ad.size(), nullptr, n.data(),
                                               contentKey.data());
    return Sealed{base64Encode(n), base64Encode(out)};
}

std::optional<std::string> decrypt(std::string_view nonceBase64, std::string_view ciphertextBase64, ByteView contentKey,
                                   std::string_view channelId, int epoch) {
    ensureSodium();
    if (contentKey.size() != kKeyBytes || epoch < 1) return std::nullopt;
    const auto n = decoded(nonceBase64);
    const auto c = decoded(ciphertextBase64);
    if (!n || !c || n->size() != kNonceBytes || c->size() < crypto_aead_xchacha20poly1305_ietf_ABYTES || c->size() > kMaxCiphertextBytes) {
        return std::nullopt;
    }
    const std::string ad = postAd(channelId, epoch);
    std::string out(c->size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long written = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(out.data()), &written, nullptr, c->data(), c->size(),
                                                   u8(ad), ad.size(), n->data(), contentKey.data()) != 0) {
        return std::nullopt;
    }
    out.resize(static_cast<std::size_t>(written));
    return out;
}

}  // namespace redvault::crypto::channel_e2ee
