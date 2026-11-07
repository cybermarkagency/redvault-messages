#include "redvault/crypto/story_crypto.hpp"

#include <sodium.h>

#include <cstring>

#include "redvault/util/text.hpp"

namespace redvault::crypto::stories {

namespace {

constexpr std::uint8_t kMagic[4] = {0x52, 0x56, 0x53, 0x54};  // "RVST"
constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kIdBytes = 16;
constexpr std::size_t kPayloadBytes = 4 + 1 + kIdBytes + kKeyBytes;
constexpr std::string_view kMetaLabel = "redvault-story-meta-v1";
constexpr std::string_view kTitleLabel = "redvault-story-highlight-v1";
constexpr std::string_view kPrefsLabel = "redvault-story-prefs-v1";
constexpr std::string_view kDraftKeyLabel = "redvault-story-drafts-v1";
constexpr std::string_view kDraftAdLabel = "redvault-story-draft-v1";

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
    if (hex.size() != 32) throw CryptoError("Not a story id");
    Bytes out(kIdBytes);
    for (std::size_t i = 0; i < kIdBytes; ++i) {
        const int hi = hexValue(hex[i * 2]);
        const int lo = hexValue(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) throw CryptoError("Not a story id");
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return out;
}

/// label bytes || id (16): the additional data that binds a ciphertext to one story.
Bytes boundTo(std::string_view label, std::string_view id) {
    Bytes out(label.begin(), label.end());
    const Bytes idBytes = uuidBytes(id);
    out.insert(out.end(), idBytes.begin(), idBytes.end());
    return out;
}

Sealed aeadSeal(ByteView plaintext, ByteView ad, ByteView key) {
    ensureSodium();
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES) throw CryptoError("Wrong key size");
    Sealed out;
    out.nonce.resize(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    randombytes_buf(out.nonce.data(), out.nonce.size());
    out.ciphertext.resize(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long length = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(out.ciphertext.data(), &length, plaintext.data(), plaintext.size(), ad.data(), ad.size(),
                                               nullptr, out.nonce.data(), key.data());
    out.ciphertext.resize(static_cast<std::size_t>(length));
    return out;
}

std::string aeadOpen(ByteView ciphertext, ByteView nonce, ByteView ad, ByteView key) {
    ensureSodium();
    if (key.size() != crypto_aead_xchacha20poly1305_ietf_KEYBYTES || nonce.size() != crypto_aead_xchacha20poly1305_ietf_NPUBBYTES ||
        ciphertext.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        throw CryptoError("This story can't be opened");
    }
    std::string out(ciphertext.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES, '\0');
    unsigned long long length = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(reinterpret_cast<unsigned char*>(out.data()), &length, nullptr, ciphertext.data(),
                                                   ciphertext.size(), ad.data(), ad.size(), nonce.data(), key.data()) != 0) {
        throw CryptoError("This story can't be opened");
    }
    out.resize(static_cast<std::size_t>(length));
    return out;
}

ByteView viewOf(std::string_view text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

constexpr std::uint8_t kAnswerMagic[4] = {0x52, 0x56, 0x53, 0x52};  // "RVSR"
constexpr std::uint8_t kAnonMagic[4] = {0x52, 0x56, 0x53, 0x41};    // "RVSA"
constexpr std::uint8_t kAnswerVersion = 1;
constexpr std::string_view kResultLabel = "redvault-story-result-v1";

bool validStickerId(std::string_view id) {
    if (id.size() < 6 || id.size() > 16) return false;
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

Bytes answerPayload(const std::uint8_t (&magic)[4], std::string_view storyId, const model::stories::Sticker& sticker,
                    const model::stories::Answer& answer) {
    if (!validStickerId(sticker.id)) throw CryptoError("Not a sticker id");
    model::stories::Answer checked;
    try {
        checked = model::stories::checkedAnswer(answer, sticker);
    } catch (const model::stories::StoryFormatError&) {
        throw CryptoError("Malformed answer");
    }
    const std::string body = model::stories::toJson(checked);
    const Bytes id = uuidBytes(storyId);
    Bytes out;
    out.reserve(4 + 1 + kIdBytes + 1 + sticker.id.size() + body.size());
    out.insert(out.end(), magic, magic + 4);
    out.push_back(kAnswerVersion);
    out.insert(out.end(), id.begin(), id.end());
    out.push_back(static_cast<std::uint8_t>(sticker.id.size()));
    out.insert(out.end(), sticker.id.begin(), sticker.id.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

model::stories::Answer openPayload(const Bytes& payload, const std::uint8_t (&magic)[4], std::string_view storyId,
                                   const model::stories::Sticker& sticker) {
    if (payload.size() < 6 + kIdBytes) throw CryptoError("Malformed answer");
    if (std::memcmp(payload.data(), magic, 4) != 0 || payload[4] != kAnswerVersion) throw CryptoError("Unknown answer format");
    const Bytes id = uuidBytes(storyId);
    if (std::memcmp(payload.data() + 5, id.data(), kIdBytes) != 0) throw CryptoError("This answer belongs to another story");
    const std::size_t idLength = payload[5 + kIdBytes];
    const std::size_t idEnd = 6 + kIdBytes + idLength;
    if (idEnd > payload.size() || idLength != sticker.id.size() || std::memcmp(payload.data() + 6 + kIdBytes, sticker.id.data(), idLength) != 0) {
        throw CryptoError("This answer belongs to another sticker");
    }
    try {
        return model::stories::parseAnswer(std::string_view(reinterpret_cast<const char*>(payload.data()) + idEnd, payload.size() - idEnd), sticker);
    } catch (const model::stories::StoryFormatError&) {
        throw CryptoError("Malformed answer");
    }
}

/// label bytes || story id (16) || revision (4, big endian) || sticker id: binds totals to one story, sticker and revision.
Bytes resultBoundTo(std::string_view storyId, std::string_view stickerId, std::uint32_t revision) {
    Bytes out(kResultLabel.begin(), kResultLabel.end());
    const Bytes id = uuidBytes(storyId);
    out.insert(out.end(), id.begin(), id.end());
    out.push_back(static_cast<std::uint8_t>(revision >> 24));
    out.push_back(static_cast<std::uint8_t>(revision >> 16));
    out.push_back(static_cast<std::uint8_t>(revision >> 8));
    out.push_back(static_cast<std::uint8_t>(revision));
    out.insert(out.end(), stickerId.begin(), stickerId.end());
    return out;
}

/// A dedicated key derived one-way from the account's private key: the private key itself is never used directly.
SecretBytes derivedKey(std::string_view label, ByteView privateKey) {
    ensureSodium();
    if (privateKey.size() < crypto_generichash_KEYBYTES_MIN || privateKey.size() > crypto_generichash_KEYBYTES_MAX) {
        throw CryptoError("Wrong private key size");
    }
    // The label is the message and the private key the key, as libsodium.js's crypto_generichash(32, label, privateKey).
    SecretBytes out(32);
    crypto_generichash(out.data(), out.size(), reinterpret_cast<const unsigned char*>(label.data()), label.size(), privateKey.data(),
                       privateKey.size());
    return out;
}

SecretBytes prefsKey(ByteView privateKey) {
    return derivedKey(kPrefsLabel, privateKey);
}

/// "redvault-story-draft-v1:" userId ":" draftId: binds a draft to the account and the id it was saved under.
Bytes draftBoundTo(std::string_view userId, std::string_view draftId) {
    const std::string text = std::string(kDraftAdLabel) + ":" + std::string(userId) + ":" + std::string(draftId);
    return Bytes(text.begin(), text.end());
}

}  // namespace

SecretBytes newStoryKey() {
    ensureSodium();
    SecretBytes key(kKeyBytes);
    crypto_aead_xchacha20poly1305_ietf_keygen(key.data());
    return key;
}

Bytes wrapStoryKey(std::string_view storyId, ByteView key, ByteView recipientPublicKey, ByteView authorPrivateKey) {
    ensureSodium();
    if (key.size() != kKeyBytes) throw CryptoError("Story keys are 32 bytes");
    if (recipientPublicKey.size() != crypto_box_PUBLICKEYBYTES || authorPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw CryptoError("Wrong key size");
    }
    SecretBytes payload(kPayloadBytes);
    std::memcpy(payload.data(), kMagic, 4);
    payload.data()[4] = kVersion;
    const Bytes id = uuidBytes(storyId);
    std::memcpy(payload.data() + 5, id.data(), kIdBytes);
    std::memcpy(payload.data() + 5 + kIdBytes, key.data(), kKeyBytes);

    Bytes out(crypto_box_NONCEBYTES + kPayloadBytes + crypto_box_MACBYTES);
    randombytes_buf(out.data(), crypto_box_NONCEBYTES);
    if (crypto_box_easy(out.data() + crypto_box_NONCEBYTES, payload.data(), payload.size(), out.data(), recipientPublicKey.data(),
                        authorPrivateKey.data()) != 0) {
        throw CryptoError("Couldn't seal the story key");
    }
    return out;
}

SecretBytes unwrapStoryKey(std::string_view storyId, ByteView wrapped, ByteView authorPublicKey, ByteView viewerPrivateKey) {
    ensureSodium();
    if (wrapped.size() != kWrappedKeyBytes) throw CryptoError("Malformed story key");
    if (authorPublicKey.size() != crypto_box_PUBLICKEYBYTES || viewerPrivateKey.size() != crypto_box_SECRETKEYBYTES) {
        throw CryptoError("Wrong key size");
    }
    SecretBytes payload(kPayloadBytes);
    if (crypto_box_open_easy(payload.data(), wrapped.data() + crypto_box_NONCEBYTES, wrapped.size() - crypto_box_NONCEBYTES, wrapped.data(),
                             authorPublicKey.data(), viewerPrivateKey.data()) != 0) {
        throw CryptoError("This story key wasn't made by its author for you");
    }
    if (std::memcmp(payload.data(), kMagic, 4) != 0 || payload.data()[4] != kVersion) throw CryptoError("Unknown story key format");
    const Bytes id = uuidBytes(storyId);
    if (sodium_memcmp(payload.data() + 5, id.data(), kIdBytes) != 0) throw CryptoError("This key belongs to another story");
    return SecretBytes::copyOf(ByteView(payload.data() + 5 + kIdBytes, kKeyBytes));
}

Sealed sealMeta(std::string_view storyId, ByteView key, std::string_view metaJson) {
    return aeadSeal(viewOf(metaJson), boundTo(kMetaLabel, storyId), key);
}

std::string openMeta(std::string_view storyId, ByteView key, ByteView ciphertext, ByteView nonce) {
    return aeadOpen(ciphertext, nonce, boundTo(kMetaLabel, storyId), key);
}

Sealed sealHighlightTitle(std::string_view title, std::string_view coverStoryId, ByteView coverKey) {
    const std::string trimmed = util::encodeUtf8(util::takeUtf16(util::trim(util::decodeUtf8(title)), kMaxHighlightTitle));
    return aeadSeal(viewOf(trimmed), boundTo(kTitleLabel, coverStoryId), coverKey);
}

std::string openHighlightTitle(ByteView ciphertext, ByteView nonce, std::string_view coverStoryId, ByteView coverKey) {
    try {
        const std::string title = aeadOpen(ciphertext, nonce, boundTo(kTitleLabel, coverStoryId), coverKey);
        return util::encodeUtf8(util::takeUtf16(util::decodeUtf8(title), kMaxHighlightTitle));
    } catch (const CryptoError&) {
        return {};
    }
}

Sealed sealPrefs(std::string_view prefsJson, ByteView privateKey) {
    const SecretBytes key = prefsKey(privateKey);
    return aeadSeal(viewOf(prefsJson), viewOf(kPrefsLabel), key);
}

std::string openPrefs(ByteView ciphertext, ByteView nonce, ByteView privateKey) {
    const SecretBytes key = prefsKey(privateKey);
    try {
        return aeadOpen(ciphertext, nonce, viewOf(kPrefsLabel), key);
    } catch (const CryptoError&) {
        throw CryptoError("Your story settings can't be opened");
    }
}

Sealed sealDraft(std::string_view userId, std::string_view draftId, std::string_view draftJson, ByteView privateKey) {
    const SecretBytes key = derivedKey(kDraftKeyLabel, privateKey);
    return aeadSeal(viewOf(draftJson), draftBoundTo(userId, draftId), key);
}

std::string openDraft(std::string_view userId, std::string_view draftId, ByteView ciphertext, ByteView nonce, ByteView privateKey) {
    const SecretBytes key = derivedKey(kDraftKeyLabel, privateKey);
    try {
        return aeadOpen(ciphertext, nonce, draftBoundTo(userId, draftId), key);
    } catch (const CryptoError&) {
        throw CryptoError("This draft can't be opened");
    }
}

Bytes sealAnswer(std::string_view storyId, const model::stories::Sticker& sticker, const model::stories::Answer& answer, ByteView authorPublicKey,
                 ByteView responderPrivateKey) {
    ensureSodium();
    if (authorPublicKey.size() != crypto_box_PUBLICKEYBYTES || responderPrivateKey.size() != crypto_box_SECRETKEYBYTES) throw CryptoError("Wrong key size");
    const Bytes payload = answerPayload(kAnswerMagic, storyId, sticker, answer);
    Bytes out(crypto_box_NONCEBYTES + payload.size() + crypto_box_MACBYTES);
    randombytes_buf(out.data(), crypto_box_NONCEBYTES);
    if (crypto_box_easy(out.data() + crypto_box_NONCEBYTES, payload.data(), payload.size(), out.data(), authorPublicKey.data(),
                        responderPrivateKey.data()) != 0) {
        throw CryptoError("Couldn't seal the answer");
    }
    return out;
}

model::stories::Answer openAnswer(std::string_view storyId, const model::stories::Sticker& sticker, ByteView envelope, ByteView otherPartyPublicKey,
                                  ByteView ownPrivateKey) {
    ensureSodium();
    if (envelope.size() <= crypto_box_NONCEBYTES + crypto_box_MACBYTES) throw CryptoError("Malformed answer");
    if (otherPartyPublicKey.size() != crypto_box_PUBLICKEYBYTES || ownPrivateKey.size() != crypto_box_SECRETKEYBYTES) throw CryptoError("Wrong key size");
    Bytes payload(envelope.size() - crypto_box_NONCEBYTES - crypto_box_MACBYTES);
    if (crypto_box_open_easy(payload.data(), envelope.data() + crypto_box_NONCEBYTES, envelope.size() - crypto_box_NONCEBYTES, envelope.data(),
                             otherPartyPublicKey.data(), ownPrivateKey.data()) != 0) {
        throw CryptoError("This answer wasn't made by that person for this author");
    }
    return openPayload(payload, kAnswerMagic, storyId, sticker);
}

Bytes sealAnonymousAnswer(std::string_view storyId, const model::stories::Sticker& sticker, const model::stories::Answer& answer,
                          ByteView authorPublicKey) {
    ensureSodium();
    if (authorPublicKey.size() != crypto_box_PUBLICKEYBYTES) throw CryptoError("Wrong key size");
    const Bytes payload = answerPayload(kAnonMagic, storyId, sticker, answer);
    Bytes out(crypto_box_SEALBYTES + payload.size());
    if (crypto_box_seal(out.data(), payload.data(), payload.size(), authorPublicKey.data()) != 0) throw CryptoError("Couldn't seal the answer");
    return out;
}

model::stories::Answer openAnonymousAnswer(std::string_view storyId, const model::stories::Sticker& sticker, ByteView envelope,
                                           ByteView authorPublicKey, ByteView authorPrivateKey) {
    ensureSodium();
    if (envelope.size() <= crypto_box_SEALBYTES) throw CryptoError("Malformed answer");
    if (authorPublicKey.size() != crypto_box_PUBLICKEYBYTES || authorPrivateKey.size() != crypto_box_SECRETKEYBYTES) throw CryptoError("Wrong key size");
    Bytes payload(envelope.size() - crypto_box_SEALBYTES);
    if (crypto_box_seal_open(payload.data(), envelope.data(), envelope.size(), authorPublicKey.data(), authorPrivateKey.data()) != 0) {
        throw CryptoError("This answer isn't for this author");
    }
    return openPayload(payload, kAnonMagic, storyId, sticker);
}

Sealed sealTotals(std::string_view storyId, std::string_view stickerId, std::uint32_t revision, const model::stories::Totals& totals, ByteView storyKey) {
    if (revision < 1) throw CryptoError("Bad revision");
    if (!validStickerId(stickerId)) throw CryptoError("Not a sticker id");
    const std::string body = model::stories::toJson(totals);
    return aeadSeal(viewOf(body), resultBoundTo(storyId, stickerId, revision), storyKey);
}

model::stories::Totals openTotals(std::string_view storyId, const model::stories::Sticker& sticker, std::uint32_t revision, ByteView ciphertext,
                                  ByteView nonce, ByteView storyKey) {
    std::string plain;
    try {
        plain = aeadOpen(ciphertext, nonce, resultBoundTo(storyId, sticker.id, revision), storyKey);
    } catch (const CryptoError&) {
        throw CryptoError("These results can't be opened");
    }
    try {
        return model::stories::parseTotals(plain, sticker);
    } catch (const model::stories::StoryFormatError&) {
        throw CryptoError("Malformed results");
    }
}

}  // namespace redvault::crypto::stories
