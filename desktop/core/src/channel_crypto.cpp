#include "redvault/crypto/channel_crypto.hpp"

#include <sodium.h>

#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/call_crypto.hpp"

namespace redvault::crypto::channels {

namespace {

constexpr std::string_view kAccessLabel = "redvault-channel-v1:access";

}  // namespace

std::string newSecret() {
    Bytes secret = randomBytes(kSecretBytes);
    std::string out = calls::linkSecretToFragment(secret);
    sodium_memzero(secret.data(), secret.size());
    return out;
}

std::optional<std::string> secretFromFragment(std::string_view fragment) {
    if (!fragment.empty() && fragment.front() == '#') fragment.remove_prefix(1);
    // The same strict base64url as libsodium.js's from_base64(…, URLSAFE_NO_PADDING).
    if (!calls::linkSecretFromFragment(fragment)) return std::nullopt;
    return std::string(fragment);
}

std::string accessToken(std::string_view secret) {
    // The bare secret: unlike a fragment, no leading '#'.
    const auto bytes = secret.starts_with('#') ? std::nullopt : calls::linkSecretFromFragment(secret);
    if (!bytes) throw CryptoError("Not a channel link secret");
    // The label is the message and the secret the key, as libsodium.js's crypto_generichash(32, label, secret).
    Bytes token(32);
    crypto_generichash(token.data(), token.size(), reinterpret_cast<const unsigned char*>(kAccessLabel.data()), kAccessLabel.size(),
                       bytes->data(), bytes->size());
    return base64Encode(token);
}

std::string sealSecret(std::string_view secret, ByteView ownerPublicKey) {
    ensureSodium();
    if (ownerPublicKey.size() != crypto_box_PUBLICKEYBYTES) throw CryptoError("Wrong public key size");
    // The base64url text itself is sealed, as sodium.crypto_box_seal(secret, publicKey) does with a string.
    Bytes sealed(secret.size() + crypto_box_SEALBYTES);
    if (crypto_box_seal(sealed.data(), reinterpret_cast<const unsigned char*>(secret.data()), secret.size(), ownerPublicKey.data()) != 0) {
        throw CryptoError("Couldn't seal the channel secret");
    }
    return base64Encode(sealed);
}

std::optional<std::string> openSecret(const std::optional<std::string>& sealed, ByteView publicKey, ByteView privateKey) {
    if (!sealed || sealed->empty()) return std::nullopt;
    ensureSodium();
    if (publicKey.size() != crypto_box_PUBLICKEYBYTES || privateKey.size() != crypto_box_SECRETKEYBYTES) return std::nullopt;
    Bytes box;
    try {
        box = base64Decode(*sealed);
    } catch (const CryptoError&) {
        return std::nullopt;
    }
    if (box.size() < crypto_box_SEALBYTES) return std::nullopt;
    std::string secret(box.size() - crypto_box_SEALBYTES, '\0');
    if (crypto_box_seal_open(reinterpret_cast<unsigned char*>(secret.data()), box.data(), box.size(), publicKey.data(), privateKey.data()) !=
        0) {
        return std::nullopt;
    }
    // Only a real secret is of any use (the web app would fail on anything else when deriving the token).
    return secretFromFragment(secret);
}

}  // namespace redvault::crypto::channels
