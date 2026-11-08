// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/crypto/community_crypto.hpp"

#include <algorithm>
#include <array>

#include "redvault/crypto/base64.hpp"

namespace redvault::crypto::community {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{0x52, 0x56, 0x43, 0x4b};  // "RVCK"
constexpr std::size_t kEpochOffset = 5;
constexpr std::size_t kIdOffset = 9;
constexpr std::size_t kKeyOffset = 25;

static_assert(kPayloadBytes == kMagic.size() + 1 + 4 + 16 + kKeyBytes);
static_assert(kKeyBytes == kAeadKeyBytes);

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void checkEpoch(std::int64_t epoch) {
    if (epoch < 0 || epoch > kMaxEpoch) throw EpochKeyError("bad epoch");
}

/// Whether `key` equals any key already in the ring (constant time per comparison).
bool alreadyHeld(const KeyRing& ring, ByteView key) {
    return std::any_of(ring.keys.begin(), ring.keys.end(),
                       [&](const auto& entry) { return constantTimeEquals(entry.second.view(), key); });
}

}  // namespace

Bytes communityIdBytes(std::string_view communityId) {
    std::string hex;
    hex.reserve(32);
    for (const char c : communityId) {
        if (c != '-') hex.push_back(c);
    }
    if (hex.size() != 32) throw EpochKeyError("not a community id");
    Bytes out(16);
    for (std::size_t i = 0; i < 16; ++i) {
        const int high = hexValue(hex[i * 2]);
        const int low = hexValue(hex[i * 2 + 1]);
        if (high < 0 || low < 0) throw EpochKeyError("not a community id");
        out[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return out;
}

SecretBytes epochKeyPayload(ByteView key, std::int64_t epoch, std::string_view communityId) {
    if (key.size() != kKeyBytes) throw EpochKeyError("community keys are 32 bytes");
    checkEpoch(epoch);
    const Bytes id = communityIdBytes(communityId);
    SecretBytes out(kPayloadBytes);
    std::copy(kMagic.begin(), kMagic.end(), out.data());
    out.data()[4] = kVersion;
    const auto e = static_cast<std::uint32_t>(epoch);
    out.data()[kEpochOffset] = static_cast<std::uint8_t>(e >> 24);
    out.data()[kEpochOffset + 1] = static_cast<std::uint8_t>(e >> 16);
    out.data()[kEpochOffset + 2] = static_cast<std::uint8_t>(e >> 8);
    out.data()[kEpochOffset + 3] = static_cast<std::uint8_t>(e);
    std::copy(id.begin(), id.end(), out.data() + kIdOffset);
    std::copy(key.begin(), key.end(), out.data() + kKeyOffset);
    return out;
}

Bytes sealEpochKey(ByteView key, std::int64_t epoch, std::string_view communityId, ByteView recipientPublicKey) {
    return sealConversationKey(epochKeyPayload(key, epoch, communityId), recipientPublicKey);
}

SecretBytes openEpochKey(ByteView sealed, std::int64_t epoch, std::string_view communityId, ByteView publicKey,
                         ByteView privateKey) {
    SecretBytes payload;
    try {
        payload = unsealConversationKey(sealed, publicKey, privateKey);
    } catch (const CryptoError&) {
        throw EpochKeyError("This key wasn't sealed to you.");
    }
    const std::uint8_t* p = payload.data();
    if (payload.size() != kPayloadBytes || !std::equal(kMagic.begin(), kMagic.end(), p) || p[4] != kVersion) {
        throw EpochKeyError("Not a community key.");
    }
    const std::uint32_t sealedEpoch = (std::uint32_t{p[kEpochOffset]} << 24) | (std::uint32_t{p[kEpochOffset + 1]} << 16) |
                                      (std::uint32_t{p[kEpochOffset + 2]} << 8) | std::uint32_t{p[kEpochOffset + 3]};
    if (epoch < 0 || epoch > kMaxEpoch || sealedEpoch != static_cast<std::uint32_t>(epoch)) {
        throw EpochKeyError("Key sealed for a different epoch.");
    }
    const Bytes id = communityIdBytes(communityId);
    if (!std::equal(id.begin(), id.end(), p + kIdOffset)) throw EpochKeyError("Key sealed for a different community.");
    return SecretBytes::copyOf(ByteView(p + kKeyOffset, kKeyBytes));
}

const SecretBytes* KeyRing::key(std::int64_t epoch) const {
    const auto it = keys.find(epoch);
    return it == keys.end() ? nullptr : &it->second;
}

KeyRing buildKeyRing(std::string_view communityId, const KeyState& state, ByteView publicKey, ByteView privateKey) {
    KeyRing ring;
    ring.communityId = std::string(communityId);
    ring.conversationId = state.conversationId;
    ring.currentEpoch = state.keyEpoch;
    ring.rekeyRequired = state.rekeyRequired;
    std::vector<const SealedEpochKey*> sorted;
    for (const auto& k : state.keys) sorted.push_back(&k);
    std::stable_sort(sorted.begin(), sorted.end(), [](const SealedEpochKey* a, const SealedEpochKey* b) { return a->epoch < b->epoch; });
    for (const SealedEpochKey* k : sorted) {
        try {
            SecretBytes key = openEpochKey(base64Decode(k->wrappedKey), k->epoch, communityId, publicKey, privateKey);
            // A "new" epoch with a key we already hold would let whoever held the old one read on.
            if (alreadyHeld(ring, key)) throw EpochKeyError("Reused key");
            ring.keys.insert_or_assign(k->epoch, std::move(key));
        } catch (const CryptoError&) {
            // EpochKeyError, or base64 that doesn't decode: either way this epoch can't be trusted.
            ring.rejected.push_back(k->epoch);
        }
    }
    return ring;
}

EncryptedForCommunity encryptForCommunity(const KeyRing& ring, std::string_view plaintext) {
    const SecretBytes* key = ring.key(ring.currentEpoch);
    if (key == nullptr) throw EpochKeyError("You don't have the community's current key yet.");
    return {encryptWithConversationKey(plaintext, *key), ring.currentEpoch};
}

std::optional<std::string> decryptForCommunity(const KeyRing& ring, std::int64_t epoch, ByteView ciphertext, ByteView nonce) {
    const SecretBytes* key = ring.key(epoch);
    if (key == nullptr) return std::nullopt;
    try {
        return decryptWithConversationKey(ciphertext, nonce, *key);
    } catch (const CryptoError&) {
        return std::nullopt;
    }
}

Rotation prepareRotation(const KeyRing& ring, std::int64_t rosterEpoch, const std::vector<Member>& roster) {
    if (rosterEpoch < 0 || rosterEpoch >= kMaxEpoch) throw EpochKeyError("bad epoch");
    SecretBytes key = generateConversationKey();
    while (alreadyHeld(ring, key)) key = generateConversationKey();
    const std::int64_t epoch = rosterEpoch + 1;
    Rotation out;
    out.fromEpoch = rosterEpoch;
    for (const auto& m : roster) {
        out.keys.push_back({m.userId, base64Encode(sealEpochKey(key, epoch, ring.communityId, base64Decode(m.publicKey)))});
    }
    out.key = std::move(key);
    return out;
}

std::vector<Enrollment> prepareEnrollments(const KeyRing& ring, const std::vector<Member>& pending) {
    const SecretBytes* key = ring.key(ring.currentEpoch);
    if (key == nullptr || ring.rekeyRequired) return {};
    std::vector<Enrollment> out;
    for (const auto& p : pending) {
        out.push_back({p.userId, ring.currentEpoch,
                       base64Encode(sealEpochKey(*key, ring.currentEpoch, ring.communityId, base64Decode(p.publicKey)))});
    }
    return out;
}

NewCommunityKey newCommunityKey(std::string_view communityId, ByteView creatorPublicKey) {
    SecretBytes key = generateConversationKey();
    std::string wrapped = base64Encode(sealEpochKey(key, 0, communityId, creatorPublicKey));
    return {std::move(key), std::move(wrapped)};
}

}  // namespace redvault::crypto::community
