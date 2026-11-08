// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/share_crypto.hpp"

#include <sodium.h>

#include <regex>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"

namespace redvault::crypto::share {

namespace {

constexpr const char* kEnvelopeAd = "rvshare-envelope-v1";
constexpr const char* kItemAdPrefix = "rvshare-item-v1|";
constexpr const char* kOwnerAd = "rvshare-owner-v1";
constexpr const char* kUploadLabel = "redvault-share-upload-v1";
constexpr const char* kPasswordLabel = "redvault-share-password-v1";

enum Subkey : std::uint64_t { Lookup = 1, Root = 2, PasswordKey = 3, Access = 4, Wrap = 5 };

constexpr unsigned long long kMinOps = 2;
constexpr unsigned long long kMaxOps = 10;
constexpr std::size_t kMinMem = 64ull * 1024 * 1024;
constexpr std::size_t kMaxMem = 256ull * 1024 * 1024;

SecretBytes kdf(ByteView key, Subkey id) {
    if (key.size() != crypto_kdf_KEYBYTES) throw CryptoError("Wrong key size for a share key");
    SecretBytes out(32);
    crypto_kdf_derive_from_key(out.data(), out.size(), id, kKdfContext, key.data());
    return out;
}

Sealed seal(ByteView key, std::string_view ad, ByteView plaintext) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw CryptoError("Wrong key size");
    Sealed out;
    out.nonce.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    randombytes_buf(out.nonce.data(), out.nonce.size());
    out.ciphertext.resize(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.ciphertext.data(), &length, plaintext.data(), plaintext.size(),
                                               reinterpret_cast<const unsigned char*>(ad.data()), ad.size(), nullptr, out.nonce.data(),
                                               key.data());
    out.ciphertext.resize(length);
    return out;
}

std::optional<std::string> open(ByteView key, std::string_view ad, ByteView ciphertext, ByteView nonce) {
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES || nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES ||
        ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return std::nullopt;
    }
    std::string plain(ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(plain.data()), &length, nullptr, ciphertext.data(),
                                                   ciphertext.size(), reinterpret_cast<const unsigned char*>(ad.data()), ad.size(),
                                                   nonce.data(), key.data()) != 0) {
        return std::nullopt;
    }
    plain.resize(length);
    return plain;
}

/// Parses padded JSON; nullopt if it isn't an object.
std::optional<nlohmann::json> parsePadded(std::string plain) {
    while (!plain.empty() && plain.back() == ' ') plain.pop_back();
    auto json = nlohmann::json::parse(plain, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return std::nullopt;
    return json;
}

bool isString(const nlohmann::json& j, const char* key, std::size_t max = 4096) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() && it->get_ref<const std::string&>().size() <= max;
}

/// A whole number from 0 up to 2^53 - 1 (what every platform's JSON can carry exactly).
bool isCount(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>() <= 9007199254740991ull;
    if (it->is_number_integer()) return it->get<std::int64_t>() >= 0 && it->get<std::int64_t>() <= 9007199254740991ll;
    if (it->is_number_float()) {
        const double v = it->get<double>();
        return v >= 0 && v <= 9007199254740991.0 && v == static_cast<double>(static_cast<std::int64_t>(v));
    }
    return false;
}

bool isSha(const nlohmann::json& j, const char* key) {
    static const std::regex hex64("^[0-9a-f]{64}$");
    return isString(j, key) && std::regex_match(j.at(key).get_ref<const std::string&>(), hex64);
}

Bytes concat(std::string_view a, ByteView b) {
    Bytes out(a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

}  // namespace

SecretBytes newLinkSecret() {
    return calls::newLinkSecret();
}

Bytes newShareKey() {
    return randomBytes(32);
}

Bytes newSalt() {
    return randomBytes(16);
}

PasswordParams newPasswordParams() {
    return PasswordParams{newSalt(), kDefaultOps, kDefaultMem};
}

SecretBytes lookupToken(ByteView secret) {
    ensureSodium();
    if (secret.size() != 32) throw CryptoError("This link is incomplete");
    return kdf(secret, Lookup);
}

ShareKeys deriveKeys(ByteView secret, std::optional<std::string_view> password, const std::optional<PasswordParams>& params) {
    ensureSodium();
    auto lookup = lookupToken(secret);
    SecretBytes root;
    if (params) {
        if (!password || password->empty()) throw CryptoError("This link needs a password");
        if (params->salt.size() != crypto_pwhash_SALTBYTES || params->ops < kMinOps || params->ops > kMaxOps || params->mem < kMinMem ||
            params->mem > kMaxMem) {
            throw CryptoError("This link's password settings aren't supported");
        }
        SecretBytes stretched(32);
        if (crypto_pwhash(stretched.data(), stretched.size(), password->data(), password->size(), params->salt.data(), params->ops,
                          params->mem, crypto_pwhash_ALG_ARGON2ID13) != 0) {
            throw CryptoError("Out of memory checking the password");
        }
        auto passwordKey = kdf(secret, PasswordKey);
        auto message = concat(kPasswordLabel, stretched.view());
        root = SecretBytes(32);
        crypto_generichash(root.data(), root.size(), message.data(), message.size(), passwordKey.data(), passwordKey.size());
        sodium_memzero(message.data(), message.size());
    } else {
        root = kdf(secret, Root);
    }
    return ShareKeys{std::move(lookup), kdf(root, Access), kdf(root, Wrap)};
}

std::string verifierOf(ByteView token) {
    return base64Encode(sha256(token));
}

Bytes padJson(const nlohmann::json& json, std::size_t bucket) {
    const std::string text = json.dump();
    Bytes out(text.begin(), text.end());
    out.resize((out.size() / bucket + 1) * bucket, ' ');
    return out;
}

Sealed sealEnvelope(ByteView wrap, const Envelope& envelope) {
    ensureSodium();
    nlohmann::json j{{"v", 1}, {"key", base64Encode(envelope.key)}, {"root", envelope.root}};
    j["upload"] = envelope.upload ? nlohmann::json(base64Encode(*envelope.upload)) : nlohmann::json(nullptr);
    return seal(wrap, kEnvelopeAd, padJson(j, 128));
}

std::optional<Envelope> openEnvelope(ByteView wrap, ByteView ciphertext, ByteView nonce) {
    ensureSodium();
    const auto plain = open(wrap, kEnvelopeAd, ciphertext, nonce);
    if (!plain) return std::nullopt;
    const auto j = parsePadded(*plain);
    if (!j || j->value("v", 0) != 1 || !isString(*j, "key") || !isString(*j, "root")) return std::nullopt;
    try {
        Envelope out;
        out.key = base64Decode(j->at("key").get<std::string>());
        out.root = j->at("root").get<std::string>();
        if (isString(*j, "upload")) out.upload = base64Decode(j->at("upload").get<std::string>());
        if (out.key.size() != 32 || (out.upload && out.upload->size() != crypto_box_PUBLICKEYBYTES)) return std::nullopt;
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Sealed sealItem(ByteView shareKey, std::string_view itemId, const nlohmann::json& meta) {
    ensureSodium();
    return seal(shareKey, std::string(kItemAdPrefix) + std::string(itemId), padJson(meta, 128));
}

std::optional<nlohmann::json> openItem(ByteView shareKey, std::string_view itemId, ByteView ciphertext, ByteView nonce) {
    ensureSodium();
    const auto plain = open(shareKey, std::string(kItemAdPrefix) + std::string(itemId), ciphertext, nonce);
    if (!plain) return std::nullopt;
    const auto m = parsePadded(*plain);
    if (!m || m->value("v", 0) != 1 || !isString(*m, "name", 1024) || !isString(*m, "parent", 64) || !isString(*m, "kind")) return std::nullopt;
    const auto kind = m->at("kind").get<std::string>();
    if (kind == "folder") return nlohmann::json{{"v", 1}, {"kind", "folder"}, {"name", m->at("name")}, {"parent", m->at("parent")}};
    if (kind != "file") return std::nullopt;
    if (!isString(*m, "mime", 255) || !isCount(*m, "size") || !isCount(*m, "modified") || !isString(*m, "key", 64) ||
        !isString(*m, "header", 64) || !isCount(*m, "chunk") || !isSha(*m, "sha")) {
        return std::nullopt;
    }
    nlohmann::json out = nlohmann::json::object();
    for (const char* key : {"v", "kind", "name", "parent", "mime", "size", "modified", "key", "header", "chunk", "sha"}) out[key] = m->at(key);
    return out;
}

Sealed sealOwnerBox(ByteView driveKey, const OwnerBox& box) {
    ensureSodium();
    const nlohmann::json j{{"v", 1}, {"secret", box.secret}, {"key", box.key}, {"root", box.root}, {"label", box.label}, {"created", box.created}};
    return seal(driveKey, kOwnerAd, padJson(j, 128));
}

std::optional<OwnerBox> openOwnerBox(ByteView driveKey, ByteView ciphertext, ByteView nonce) {
    ensureSodium();
    const auto plain = open(driveKey, kOwnerAd, ciphertext, nonce);
    if (!plain) return std::nullopt;
    const auto b = parsePadded(*plain);
    if (!b || b->value("v", 0) != 1 || !isString(*b, "secret", 64) || !isString(*b, "key", 64) || !isString(*b, "root", 64) ||
        !isString(*b, "label", 1024) || !isCount(*b, "created")) {
        return std::nullopt;
    }
    return OwnerBox{b->at("secret").get<std::string>(), b->at("key").get<std::string>(), b->at("root").get<std::string>(),
                    b->at("label").get<std::string>(), b->at("created").get<std::int64_t>()};
}

KeyPair uploadKeyPair(ByteView driveKey, ByteView salt) {
    ensureSodium();
    if (salt.size() != 16) throw CryptoError("Wrong upload key salt");
    const auto message = concat(kUploadLabel, salt);
    SecretBytes seed(crypto_box_SEEDBYTES);
    crypto_generichash(seed.data(), seed.size(), message.data(), message.size(), driveKey.data(), driveKey.size());
    KeyPair pair{Bytes(crypto_box_PUBLICKEYBYTES), SecretBytes(crypto_box_SECRETKEYBYTES)};
    crypto_box_seed_keypair(pair.publicKey.data(), pair.privateKey.data(), seed.data());
    return pair;
}

std::optional<nlohmann::json> openContribution(const KeyPair& pair, ByteView sealed) {
    ensureSodium();
    if (sealed.size() < crypto_box_SEALBYTES) return std::nullopt;
    std::string plain(sealed.size() - crypto_box_SEALBYTES, '\0');
    if (crypto_box_seal_open(reinterpret_cast<unsigned char*>(plain.data()), sealed.data(), sealed.size(), pair.publicKey.data(),
                             pair.privateKey.data()) != 0) {
        return std::nullopt;
    }
    const auto m = parsePadded(plain);
    if (!m || m->value("v", 0) != 1 || !isString(*m, "name", 1024) || !isString(*m, "mime", 255) || !isCount(*m, "size") ||
        !isCount(*m, "modified") || !isCount(*m, "sent") || !isString(*m, "key", 64) || !isString(*m, "header", 64) || !isCount(*m, "chunk") ||
        !isSha(*m, "sha")) {
        return std::nullopt;
    }
    return m;
}

std::string shareUrl(std::string_view webAppBase, std::string_view publicId, ByteView secret) {
    std::string base(webAppBase);
    if (base.empty() || base.back() != '/') base += '/';
    return base + "share/?id=" + std::string(publicId) + "#" + calls::linkSecretToFragment(secret);
}

}  // namespace redvault::crypto::share
