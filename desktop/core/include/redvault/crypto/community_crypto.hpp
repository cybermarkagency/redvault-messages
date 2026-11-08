// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/e2e_crypto.hpp"

// Community group keys with epochs, byte-compatible with the web app
// (ui/src/lib/communityCrypto.ts; docs/paid-communities.md, "Encrypted
// membership"). The same primitives as every other group here: a random
// XChaCha20-Poly1305 key, sealed to each member with crypto_box_seal. Plus
// one rule: whenever anyone leaves or is removed, a remaining member's app
// makes a NEW key (the next epoch) and seals it only to who remains. Nothing
// is sent under an older epoch afterwards (the server refuses it), so a
// removed member's saved keys open nothing new.
//
// Each sealed copy binds the key to its community and epoch:
//   "RVCK" | version 1 | epoch (u32, big-endian) | community id (16 bytes) | key (32 bytes)
// so the server can't hand someone an old epoch's copy as a new epoch, or
// another community's key: the app checks both before using a key, and
// refuses a "new" epoch whose key equals one it already holds.
//
// Pinned to the web app by tests/community_crypto_test.cpp
// (vectors/community_vectors.json, made with the web app's own code).

namespace redvault::crypto::community {

inline constexpr std::size_t kKeyBytes = 32;
/// "RVCK" (4) + version (1) + epoch (4) + community id (16) + key (32).
inline constexpr std::size_t kPayloadBytes = 57;
inline constexpr std::uint8_t kVersion = 1;
/// Epochs are unsigned 32-bit on the wire.
inline constexpr std::int64_t kMaxEpoch = 0xFFFFFFFFLL;

/// A key that doesn't belong: not sealed to you, not a community key, another epoch or community, or a reused key.
class EpochKeyError : public CryptoError {
public:
    using CryptoError::CryptoError;
};

/// The community id's 16 bytes: 32 hex digits, dashes anywhere, any case. Throws EpochKeyError otherwise.
Bytes communityIdBytes(std::string_view communityId);

/// The bound payload one sealed copy carries (exported for tests). Throws EpochKeyError for a bad key, epoch or id.
SecretBytes epochKeyPayload(ByteView key, std::int64_t epoch, std::string_view communityId);

/// Seals one epoch's key to one member's public key.
Bytes sealEpochKey(ByteView key, std::int64_t epoch, std::string_view communityId, ByteView recipientPublicKey);

/**
 * Opens a sealed epoch key, checking it was made for exactly this community
 * and epoch. Throws EpochKeyError when it wasn't sealed to this keypair, isn't
 * a community key, or names another epoch or community.
 */
SecretBytes openEpochKey(ByteView sealed, std::int64_t epoch, std::string_view communityId, ByteView publicKey,
                         ByteView privateKey);

/// One sealed copy as the server lists it (GET /communities/:id/keys); `wrappedKey` is base64.
struct SealedEpochKey {
    std::int64_t epoch = 0;
    std::string wrappedKey;
};

/// The server's answer to GET /communities/:id/keys.
struct KeyState {
    std::string conversationId;
    std::int64_t keyEpoch = 0;
    bool rekeyRequired = false;
    std::vector<SealedEpochKey> keys;
};

/// Every epoch key this member holds for one community. Memory only; the keys are wiped with it.
struct KeyRing {
    std::string communityId;
    std::string conversationId;
    /// The epoch new messages must use.
    std::int64_t currentEpoch = 0;
    bool rekeyRequired = false;
    std::map<std::int64_t, SecretBytes> keys;
    /// Epochs whose sealed copy failed the checks above (tampering or corruption): their messages show as unreadable.
    std::vector<std::int64_t> rejected;

    /// The key for one epoch, or null when it isn't held (from before joining, or rejected).
    const SecretBytes* key(std::int64_t epoch) const;
    bool hasCurrentKey() const { return key(currentEpoch) != nullptr; }
};

/// Opens every sealed copy this member was given, oldest first; a copy that fails a check is listed in `rejected`.
KeyRing buildKeyRing(std::string_view communityId, const KeyState& state, ByteView publicKey, ByteView privateKey);

struct EncryptedForCommunity {
    AeadCiphertext body;
    std::int64_t keyEpoch = 0;
};

/// Encrypts a body under the current epoch. Throws EpochKeyError when this member doesn't hold that key (yet).
EncryptedForCommunity encryptForCommunity(const KeyRing& ring, std::string_view plaintext);

/// Nullopt when the epoch's key isn't held (from before joining) or the body doesn't open.
std::optional<std::string> decryptForCommunity(const KeyRing& ring, std::int64_t epoch, ByteView ciphertext, ByteView nonce);

/// Someone on the roster or waiting to be let in: `publicKey` is base64.
struct Member {
    std::string userId;
    std::string publicKey;
};

/// One member's sealed copy of a new key (base64).
struct WrappedForMember {
    std::string userId;
    std::string wrappedKey;
};

struct Rotation {
    std::int64_t fromEpoch = 0;
    SecretBytes key;
    std::vector<WrappedForMember> keys;
};

/**
 * The next epoch: a fresh random key sealed to every member on the roster
 * (who the server says is still in). Never one of the keys already held.
 */
Rotation prepareRotation(const KeyRing& ring, std::int64_t rosterEpoch, const std::vector<Member>& roster);

struct Enrollment {
    std::string userId;
    std::int64_t epoch = 0;
    std::string wrappedKey;
};

/// The current epoch's key, sealed to each member who is waiting to be let in. Empty while a rekey is due or the key isn't held.
std::vector<Enrollment> prepareEnrollments(const KeyRing& ring, const std::vector<Member>& pending);

struct NewCommunityKey {
    SecretBytes key;
    std::string wrappedKey;
};

/// Epoch 0, made by the creator's app for a community id it chose (so epoch 0 is bound too).
NewCommunityKey newCommunityKey(std::string_view communityId, ByteView creatorPublicKey);

}  // namespace redvault::crypto::community
