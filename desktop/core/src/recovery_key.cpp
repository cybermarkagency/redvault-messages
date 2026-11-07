#include "redvault/crypto/recovery_key.hpp"

#include <sodium.h>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::crypto::recovery {
namespace {

constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

SecretBytes keyedHash(ByteView key, std::string_view label) {
    ensureSodium();
    SecretBytes out(32);
    if (crypto_generichash(out.data(), out.size(), reinterpret_cast<const unsigned char*>(label.data()), label.size(),
                           key.data(), key.size()) != 0) {
        throw CryptoError("Key derivation failed");
    }
    return out;
}

}  // namespace

SecretBytes generate() {
    ensureSodium();
    SecretBytes key(kKeyBytes);
    randombytes_buf(key.data(), key.size());
    return key;
}

SecretBytes wrapKey(ByteView key) {
    return keyedHash(key, "redvault-recovery-wrap-v1");
}

SecretBytes authToken(ByteView key) {
    return keyedHash(key, "redvault-recovery-auth-v1");
}

std::string authFor(ByteView key) {
    return base64Encode(authToken(key));
}

RecoveryMaterial materialFor(ByteView key, ByteView privateKey) {
    AeadCiphertext sealed = aeadEncrypt(privateKey, wrapKey(key));
    return {authFor(key), base64Encode(sealed.ciphertext), base64Encode(sealed.nonce)};
}

SecretBytes unwrap(ByteView key, std::string_view encryptedPrivateKeyB64, std::string_view nonceB64,
                   std::string_view publicKeyB64) {
    SecretBytes privateKey =
        aeadDecrypt(base64Decode(encryptedPrivateKeyB64), base64Decode(nonceB64), wrapKey(key));
    Bytes derived(kBoxPublicKeyBytes);
    if (privateKey.size() != kBoxSecretKeyBytes || crypto_scalarmult_base(derived.data(), privateKey.data()) != 0 ||
        base64Encode(derived) != publicKeyB64) {
        throw CryptoError("Recovered key doesn't match this account");
    }
    return privateKey;
}

std::string format(ByteView key) {
    std::string chars;
    unsigned bits = 0;
    unsigned value = 0;
    for (std::uint8_t b : key) {
        value = (value << 8) | b;
        bits += 8;
        while (bits >= 5) {
            chars.push_back(kAlphabet[(value >> (bits - 5)) & 31]);
            bits -= 5;
        }
        value &= (1u << bits) - 1;
    }
    if (bits > 0) chars.push_back(kAlphabet[(value << (5 - bits)) & 31]);

    std::string out;
    for (std::size_t i = 0; i < chars.size(); i += 4) {
        if (i > 0) out.push_back('-');
        out += chars.substr(i, 4);
    }
    return out;
}

std::optional<SecretBytes> parse(std::string_view text) {
    std::string clean;
    for (char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (c >= 0x80) return std::nullopt;
        if (!std::isalnum(c)) continue;
        char upper = static_cast<char>(std::toupper(c));
        if (upper == '0') upper = 'O';
        if (upper == '1') upper = 'I';
        if (upper == '8') upper = 'B';
        clean.push_back(upper);
    }
    if (clean.size() != 52) return std::nullopt;

    SecretBytes out(kKeyBytes);
    unsigned bits = 0;
    unsigned value = 0;
    std::size_t index = 0;
    for (char ch : clean) {
        const auto v = kAlphabet.find(ch);
        if (v == std::string_view::npos) return std::nullopt;
        value = (value << 5) | static_cast<unsigned>(v);
        bits += 5;
        if (bits >= 8) {
            out.data()[index++] = static_cast<std::uint8_t>((value >> (bits - 8)) & 0xff);
            bits -= 8;
            value &= (1u << bits) - 1;
        }
    }
    // The 4 spare bits must be zero, as format() writes them.
    if (index != kKeyBytes || value != 0) return std::nullopt;
    return out;
}

}  // namespace redvault::crypto::recovery
