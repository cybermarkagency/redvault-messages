// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/e2e_crypto.hpp"

#include <sodium.h>

#include <algorithm>

#include "redvault/crypto/base64.hpp"

namespace redvault::crypto {

static_assert(kBoxPublicKeyBytes == crypto_box_PUBLICKEYBYTES);
static_assert(kBoxSecretKeyBytes == crypto_box_SECRETKEYBYTES);
static_assert(kBoxSealBytes == crypto_box_SEALBYTES);
static_assert(kAeadKeyBytes == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
static_assert(kAeadNonceBytes == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(kAeadTagBytes == crypto_aead_xchacha20poly1305_ietf_ABYTES);
static_assert(kPwhashSaltBytes == crypto_pwhash_SALTBYTES);
static_assert(kPwhashOpsLimitInteractive == crypto_pwhash_argon2id_OPSLIMIT_INTERACTIVE);
static_assert(kPwhashMemLimitInteractive == crypto_pwhash_argon2id_MEMLIMIT_INTERACTIVE);

KeyPair generateKeyPair() {
    ensureSodium();
    KeyPair pair{Bytes(kBoxPublicKeyBytes), SecretBytes(kBoxSecretKeyBytes)};
    if (crypto_box_keypair(pair.publicKey.data(), pair.privateKey.data()) != 0) {
        throw CryptoError("Keypair generation failed");
    }
    return pair;
}

Bytes publicKeyOf(ByteView privateKey) {
    ensureSodium();
    if (privateKey.size() != crypto_scalarmult_SCALARBYTES) throw CryptoError("Malformed private key");
    Bytes publicKey(crypto_scalarmult_BYTES);
    if (crypto_scalarmult_base(publicKey.data(), privateKey.data()) != 0) throw CryptoError("Couldn't derive the public key");
    return publicKey;
}

void requireSafeKdf(const KdfSettings& kdf) {
    const bool inRange = kdf.ops >= kKdfInteractive.ops && kdf.ops <= kKdfMax.ops && kdf.mem >= kKdfInteractive.mem && kdf.mem <= kKdfMax.mem;
    if (!inRange) throw UnsafeKdfError();
}

SecretBytes deriveWrapKey(std::string_view password, ByteView kdfSalt, const KdfSettings& kdf) {
    requireSafeKdf(kdf);
    ensureSodium();
    if (kdfSalt.size() != kPwhashSaltBytes) throw CryptoError("kdfSalt must be 16 bytes");
    // libsodium.js hashes a string password as its UTF-8 bytes; std::string is already UTF-8.
    SecretBytes key(kAeadKeyBytes);
    if (crypto_pwhash(key.data(), key.size(), password.data(), password.size(), kdfSalt.data(),
                      static_cast<unsigned long long>(kdf.ops), static_cast<std::size_t>(kdf.mem), crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw CryptoError("Key derivation failed");
    }
    return key;
}

namespace {
// crypto_kdf context and subkey id for the sign-in key. Never change: every app must derive the same key.
constexpr char kSignInKdfContext[crypto_kdf_CONTEXTBYTES + 1] = "rvauthk1";
constexpr std::uint64_t kSignInKdfSubkeyId = 1;
}  // namespace

std::string deriveSignInKey(std::string_view password, ByteView kdfSalt, const KdfSettings& kdf) {
    const SecretBytes wrapKey = deriveWrapKey(password, kdfSalt, kdf);
    SecretBytes signInKey(crypto_kdf_KEYBYTES);
    if (crypto_kdf_derive_from_key(signInKey.data(), signInKey.size(), kSignInKdfSubkeyId, kSignInKdfContext, wrapKey.data()) != 0) {
        throw CryptoError("Sign-in key derivation failed");
    }
    return base64Encode(signInKey);
}

WrappedPrivateKey wrapPrivateKey(ByteView privateKey, std::string_view password) {
    Bytes kdfSalt = randomBytes(kPwhashSaltBytes);
    SecretBytes wrapKey = deriveWrapKey(password, kdfSalt, kKdfCurrent);
    AeadCiphertext sealed = aeadEncrypt(privateKey, wrapKey);
    return {std::move(sealed.ciphertext), std::move(sealed.nonce), std::move(kdfSalt), kKdfCurrent};
}

SecretBytes unwrapPrivateKey(const WrappedPrivateKey& wrapped, std::string_view password) {
    SecretBytes wrapKey = deriveWrapKey(password, wrapped.kdfSalt, wrapped.kdf.value_or(kKdfInteractive));
    return aeadDecrypt(wrapped.encryptedPrivateKey, wrapped.nonce, wrapKey);
}

SecretBytes generateConversationKey() {
    ensureSodium();
    SecretBytes key(kAeadKeyBytes);
    randombytes_buf(key.data(), key.size());
    return key;
}

Bytes sealConversationKey(ByteView conversationKey, ByteView recipientPublicKey) {
    ensureSodium();
    if (recipientPublicKey.size() != kBoxPublicKeyBytes) throw CryptoError("Recipient public key must be 32 bytes");
    Bytes sealed(conversationKey.size() + kBoxSealBytes);
    if (crypto_box_seal(sealed.data(), conversationKey.data(), conversationKey.size(), recipientPublicKey.data()) != 0) {
        throw CryptoError("Failed to seal conversation key");
    }
    return sealed;
}

SecretBytes unsealConversationKey(ByteView sealed, ByteView recipientPublicKey, ByteView recipientPrivateKey) {
    ensureSodium();
    if (sealed.size() < kBoxSealBytes) throw CryptoError("Sealed key too short");
    if (recipientPublicKey.size() != kBoxPublicKeyBytes || recipientPrivateKey.size() != kBoxSecretKeyBytes) {
        throw CryptoError("Malformed keypair");
    }
    SecretBytes opened(sealed.size() - kBoxSealBytes);
    if (crypto_box_seal_open(opened.data(), sealed.data(), sealed.size(), recipientPublicKey.data(),
                             recipientPrivateKey.data()) != 0) {
        throw CryptoError("Failed to unseal conversation key: wrong keypair or corrupted data");
    }
    return opened;
}

AeadCiphertext encryptWithConversationKey(std::string_view plaintext, ByteView conversationKey) {
    return aeadEncrypt(ByteView(reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()),
                       conversationKey);
}

std::string decryptWithConversationKey(ByteView ciphertext, ByteView nonce, ByteView conversationKey) {
    SecretBytes plaintext = aeadDecrypt(ciphertext, nonce, conversationKey);
    return {reinterpret_cast<const char*>(plaintext.data()), plaintext.size()};
}

std::vector<std::string> safetyNumberDigits(ByteView publicKeyA, ByteView publicKeyB) {
    // std::lexicographical_compare on uint8_t is already an unsigned byte comparison.
    const bool aFirst = !std::lexicographical_compare(publicKeyB.begin(), publicKeyB.end(), publicKeyA.begin(),
                                                      publicKeyA.end());
    ByteView first = aFirst ? publicKeyA : publicKeyB;
    ByteView second = aFirst ? publicKeyB : publicKeyA;
    Bytes hash = sha256(toBytes(base64Encode(first) + ":" + base64Encode(second)));

    std::string digits;
    digits.reserve(60);
    for (std::size_t i = 0; i < 30; ++i) {
        const int value = hash[i] % 100;
        digits.push_back(static_cast<char>('0' + value / 10));
        digits.push_back(static_cast<char>('0' + value % 10));
    }
    std::vector<std::string> groups;
    groups.reserve(12);
    for (std::size_t i = 0; i < 12; ++i) groups.push_back(digits.substr(i * 5, 5));
    return groups;
}

static_assert(kSignPublicKeyBytes == crypto_sign_PUBLICKEYBYTES);
static_assert(kSignSecretKeyBytes == crypto_sign_SECRETKEYBYTES);
static_assert(kSignatureBytes == crypto_sign_BYTES);

namespace {

// crypto_kdf context and subkey id for the signing seed. Never change: every app must derive the same key.
constexpr char kSigningKdfContext[crypto_kdf_CONTEXTBYTES + 1] = "rvsignk1";
constexpr std::uint64_t kSigningKdfSubkeyId = 1;

std::string lowercaseUuid(std::string_view id) {
    if (id.size() != 36) throw CryptoError("ids must be UUIDs");
    std::string out(id);
    for (std::size_t i = 0; i < out.size(); ++i) {
        char& c = out[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') throw CryptoError("ids must be UUIDs");
            continue;
        }
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) throw CryptoError("ids must be UUIDs");
    }
    return out;
}

void appendU32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

}  // namespace

SigningKeyPair deriveSigningKeyPair(ByteView accountPrivateKey) {
    ensureSodium();
    if (accountPrivateKey.size() != crypto_kdf_KEYBYTES) throw CryptoError("Signing key derivation failed: bad account key");
    SecretBytes seed(crypto_sign_SEEDBYTES);
    if (crypto_kdf_derive_from_key(seed.data(), seed.size(), kSigningKdfSubkeyId, kSigningKdfContext,
                                   accountPrivateKey.data()) != 0) {
        throw CryptoError("Signing key derivation failed");
    }
    SigningKeyPair pair{Bytes(kSignPublicKeyBytes), SecretBytes(kSignSecretKeyBytes)};
    if (crypto_sign_seed_keypair(pair.publicKey.data(), pair.secretKey.data(), seed.data()) != 0) {
        throw CryptoError("Signing key derivation failed");
    }
    return pair;
}

Bytes messageSignatureInput(std::string_view conversationId, std::string_view senderId, std::uint32_t keyEpoch,
                            ByteView nonce, ByteView ciphertext) {
    const std::string conversation = lowercaseUuid(conversationId);
    const std::string sender = lowercaseUuid(senderId);
    Bytes out;
    out.reserve(kMessageSignatureContext.size() + 1 + 72 + 8 + nonce.size() + ciphertext.size());
    out.insert(out.end(), kMessageSignatureContext.begin(), kMessageSignatureContext.end());
    out.push_back(0);
    out.insert(out.end(), conversation.begin(), conversation.end());
    out.insert(out.end(), sender.begin(), sender.end());
    appendU32(out, keyEpoch);
    appendU32(out, static_cast<std::uint32_t>(nonce.size()));
    out.insert(out.end(), nonce.begin(), nonce.end());
    out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    return out;
}

namespace {
// crypto_kdf context and subkey id for a key commitment. Never change: every app must derive the same value.
constexpr char kKeyCommitmentContext[crypto_kdf_CONTEXTBYTES + 1] = "rvkeycm1";
constexpr std::uint64_t kKeyCommitmentSubkeyId = 1;
constexpr std::string_view kKeyNoticeContext = "redvault-group-key-v1";
constexpr std::size_t kKeyCommitmentBytes = 32;
}  // namespace

Bytes keyCommitment(ByteView conversationKey) {
    ensureSodium();
    if (conversationKey.size() != crypto_kdf_KEYBYTES) throw CryptoError("A conversation key is 32 bytes");
    Bytes commitment(kKeyCommitmentBytes);
    if (crypto_kdf_derive_from_key(commitment.data(), commitment.size(), kKeyCommitmentSubkeyId, kKeyCommitmentContext,
                                   conversationKey.data()) != 0) {
        throw CryptoError("Key commitment failed");
    }
    return commitment;
}

Bytes keyNoticeInput(std::string_view conversationId, std::string_view issuerId, std::int64_t epoch, std::int64_t seq,
                     const std::vector<std::string>& members, ByteView commitment) {
    if (epoch < 0 || epoch > 0xffffffffLL || seq < 0 || seq > 0xffffffffLL) throw CryptoError("Bad key notice epoch or sequence number");
    const std::string conversation = lowercaseUuid(conversationId);
    const std::string issuer = lowercaseUuid(issuerId);
    std::vector<std::string> sorted;
    sorted.reserve(members.size());
    for (const auto& member : members) sorted.push_back(lowercaseUuid(member));
    std::sort(sorted.begin(), sorted.end());
    Bytes out;
    out.reserve(kKeyNoticeContext.size() + 1 + 72 + 12 + 36 * sorted.size() + commitment.size());
    out.insert(out.end(), kKeyNoticeContext.begin(), kKeyNoticeContext.end());
    out.push_back(0);
    out.insert(out.end(), conversation.begin(), conversation.end());
    out.insert(out.end(), issuer.begin(), issuer.end());
    appendU32(out, static_cast<std::uint32_t>(epoch));
    appendU32(out, static_cast<std::uint32_t>(seq));
    appendU32(out, static_cast<std::uint32_t>(sorted.size()));
    for (const auto& member : sorted) out.insert(out.end(), member.begin(), member.end());
    out.insert(out.end(), commitment.begin(), commitment.end());
    return out;
}

Bytes signMessage(ByteView input, ByteView secretKey) {
    ensureSodium();
    if (secretKey.size() != kSignSecretKeyBytes) throw CryptoError("Signing failed: bad key");
    Bytes signature(kSignatureBytes);
    if (crypto_sign_detached(signature.data(), nullptr, input.data(), input.size(), secretKey.data()) != 0) {
        throw CryptoError("Signing failed");
    }
    return signature;
}

bool verifyMessageSignature(ByteView signature, ByteView input, ByteView publicKey) {
    ensureSodium();
    if (signature.size() != kSignatureBytes || publicKey.size() != kSignPublicKeyBytes) return false;
    return crypto_sign_verify_detached(signature.data(), input.data(), input.size(), publicKey.data()) == 0;
}

AeadCiphertext aeadEncrypt(ByteView plaintext, ByteView key) {
    return aeadEncrypt(plaintext, key, randomBytes(kAeadNonceBytes));
}

AeadCiphertext aeadEncrypt(ByteView plaintext, ByteView key, ByteView nonce) {
    ensureSodium();
    if (key.size() != kAeadKeyBytes || nonce.size() != kAeadNonceBytes) {
        throw CryptoError("AEAD encryption failed: malformed key or nonce");
    }
    AeadCiphertext out{Bytes(plaintext.size() + kAeadTagBytes), Bytes(nonce.begin(), nonce.end())};
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(out.ciphertext.data(), &length, plaintext.data(), plaintext.size(),
                                                   nullptr, 0, nullptr, nonce.data(), key.data()) != 0) {
        throw CryptoError("AEAD encryption failed");
    }
    out.ciphertext.resize(static_cast<std::size_t>(length));
    return out;
}

SecretBytes aeadDecrypt(ByteView ciphertext, ByteView nonce, ByteView key) {
    ensureSodium();
    if (ciphertext.size() < kAeadTagBytes || nonce.size() != kAeadNonceBytes || key.size() != kAeadKeyBytes) {
        throw CryptoError("AEAD decryption failed: malformed input");
    }
    // Keep at least one byte allocated so an empty plaintext still has a valid output pointer.
    SecretBytes plaintext(std::max<std::size_t>(ciphertext.size() - kAeadTagBytes, 1));
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plaintext.data(), &length, nullptr, ciphertext.data(),
                                                   ciphertext.size(), nullptr, 0, nonce.data(), key.data()) != 0) {
        throw CryptoError("AEAD decryption failed: wrong key, wrong nonce, or tampered ciphertext");
    }
    plaintext.truncate(static_cast<std::size_t>(length));
    return plaintext;
}

}  // namespace redvault::crypto
