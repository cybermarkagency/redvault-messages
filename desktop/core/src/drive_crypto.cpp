#include "redvault/crypto/drive_crypto.hpp"

#include <sodium.h>

#include <cctype>

#include "redvault/crypto/call_crypto.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

namespace redvault::crypto::drive {

namespace {

constexpr std::size_t kPadBucket = 128;

}  // namespace

SecretBytes driveKey(ByteView privateKey) {
    return calls::deriveLocalKey(privateKey, "redvault-drive-v1");
}

SealedMeta sealMeta(std::string_view json, ByteView key) {
    std::string plain(json);
    plain.append(((plain.size() / kPadBucket) + 1) * kPadBucket - plain.size(), ' ');
    const auto sealed = encryptWithConversationKey(plain, key);
    return {sealed.ciphertext, sealed.nonce};
}

std::string openMeta(ByteView ciphertext, ByteView nonce, ByteView key) {
    std::string plain = decryptWithConversationKey(ciphertext, nonce, key);
    while (!plain.empty() && plain.back() == ' ') plain.pop_back();
    return plain;
}

Bytes newPasswordSalt() {
    ensureSodium();
    Bytes salt(crypto_pwhash_SALTBYTES);
    randombytes_buf(salt.data(), salt.size());
    return salt;
}

SecretBytes linkKey(ByteView secret, std::string_view password, ByteView salt) {
    ensureSodium();
    if (password.empty()) return SecretBytes::copyOf(secret);
    if (salt.size() != crypto_pwhash_SALTBYTES) throw CryptoError("Wrong salt size");
    SecretBytes stretched(32);
    if (crypto_pwhash(stretched.data(), stretched.size(), password.data(), password.size(), salt.data(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
                      crypto_pwhash_MEMLIMIT_INTERACTIVE, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw CryptoError("Out of memory deriving the link key");
    }
    SecretBytes key(32);
    crypto_generichash(key.data(), key.size(), secret.data(), secret.size(), stretched.data(), stretched.size());
    return key;
}

SecretBytes newFileLockKey() {
    ensureSodium();
    SecretBytes key(kFileLockKeyBytes);
    randombytes_buf(key.data(), key.size());
    return key;
}

std::optional<SecretBytes> parseFileLockKey(std::string_view text) {
    ensureSodium();
    std::string compact;
    compact.reserve(text.size());
    for (const char c : text) {
        if (!std::isspace(static_cast<unsigned char>(c))) compact += c;
    }
    std::string hex;
    for (const char c : compact) {
        if (c != '-' && c != ':') hex += c;
    }
    struct Wipe {
        std::string& a;
        std::string& b;
        ~Wipe() {
            sodium_memzero(a.data(), a.size());
            sodium_memzero(b.data(), b.size());
        }
    } wipe{compact, hex};

    SecretBytes key(kFileLockKeyBytes);
    std::size_t length = 0;
    if (hex.size() == kFileLockKeyBytes * 2 &&
        sodium_hex2bin(key.data(), key.size(), hex.data(), hex.size(), nullptr, &length, nullptr) == 0 && length == kFileLockKeyBytes) {
        return key;
    }
    for (const int variant : {sodium_base64_VARIANT_ORIGINAL, sodium_base64_VARIANT_ORIGINAL_NO_PADDING, sodium_base64_VARIANT_URLSAFE,
                              sodium_base64_VARIANT_URLSAFE_NO_PADDING}) {
        length = 0;
        if (sodium_base642bin(key.data(), key.size(), compact.data(), compact.size(), nullptr, &length, nullptr, variant) == 0 &&
            length == kFileLockKeyBytes) {
            return key;
        }
    }
    return std::nullopt;
}

std::string fileLockKeyHex(ByteView key) {
    return toHex(key);
}

std::string fileLockKeyFingerprint(ByteView key) {
    ensureSodium();
    static constexpr std::string_view kLabel = "redvault-file-key-fingerprint-v1";
    unsigned char digest[8];
    crypto_generichash(digest, sizeof digest, reinterpret_cast<const unsigned char*>(kLabel.data()), kLabel.size(), key.data(), key.size());
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (std::size_t i = 0; i < sizeof digest; ++i) {
        if (i > 0 && i % 2 == 0) out += '-';
        out += digits[digest[i] >> 4];
        out += digits[digest[i] & 15];
    }
    return out;
}

PasswordKdf newPasswordKdf() {
    return {newPasswordSalt(), crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE};
}

SecretBytes passwordKey(std::string_view password, const PasswordKdf& kdf) {
    ensureSodium();
    if (kdf.salt.size() != crypto_pwhash_SALTBYTES) throw CryptoError("Wrong salt size");
    if (kdf.opsLimit < crypto_pwhash_OPSLIMIT_INTERACTIVE || kdf.opsLimit > crypto_pwhash_OPSLIMIT_SENSITIVE ||
        kdf.memLimit < crypto_pwhash_MEMLIMIT_INTERACTIVE || kdf.memLimit > crypto_pwhash_MEMLIMIT_SENSITIVE) {
        throw CryptoError("Password settings out of range");
    }
    SecretBytes key(32);
    if (crypto_pwhash(key.data(), key.size(), password.data(), password.size(), kdf.salt.data(), kdf.opsLimit, kdf.memLimit,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw CryptoError("Out of memory deriving the password key");
    }
    return key;
}

WrappedKey wrapKey(ByteView key, ByteView wrappingKey, std::string_view context) {
    ensureSodium();
    if (wrappingKey.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw CryptoError("Wrong wrapping key size");
    WrappedKey out;
    out.nonce.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    randombytes_buf(out.nonce.data(), out.nonce.size());
    out.ciphertext.resize(key.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.ciphertext.data(), &length, key.data(), key.size(),
                                               reinterpret_cast<const unsigned char*>(context.data()), context.size(), nullptr,
                                               out.nonce.data(), wrappingKey.data());
    out.ciphertext.resize(length);
    return out;
}

SecretBytes unwrapKey(const WrappedKey& wrapped, ByteView wrappingKey, std::string_view context) {
    ensureSodium();
    if (wrappingKey.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES ||
        wrapped.nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES ||
        wrapped.ciphertext.size() <= crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        throw CryptoError("Couldn't unlock the key");
    }
    SecretBytes key(wrapped.ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(key.data(), &length, nullptr, wrapped.ciphertext.data(), wrapped.ciphertext.size(),
                                                   reinterpret_cast<const unsigned char*>(context.data()), context.size(),
                                                   wrapped.nonce.data(), wrappingKey.data()) != 0) {
        throw CryptoError("Couldn't unlock the key");
    }
    key.truncate(length);
    return key;
}

std::string fileLinkUrl(std::string_view webAppBase, std::string_view linkId, ByteView secret) {
    std::string base(webAppBase);
    if (base.empty() || base.back() != '/') base += '/';
    return base + "?file=" + std::string(linkId) + "#" + calls::linkSecretToFragment(secret);
}

}  // namespace redvault::crypto::drive
