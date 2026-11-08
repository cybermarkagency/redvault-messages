// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/call_crypto.hpp"

// End-to-end encryption for group calls, byte-compatible with the web app
// (ui/src/lib/groupCallCrypto.ts, sas.ts). A group call runs through a
// LiveKit SFU, which would see and hear everything if media were protected by
// DTLS alone, so every frame is also encrypted in the app under a room key the
// server never holds:
//
//   - The first person in the call (the keyholder) makes a random 32-byte key
//     for each epoch and seals it with crypto_box to every other participant's
//     identity key, the key their safety number covers. The server relays the
//     sealed copies but can't open or forge them.
//   - The server moves to a new epoch whenever someone joins or leaves, and
//     the keyholder sends a fresh key. Someone who left can't follow the call
//     afterwards; someone who joined can't decrypt what came before.
//   - A sealed key names the call, group, epoch, sender and recipient and
//     carries a timestamp. It is refused if any of them is off, so it can't be
//     replayed into another call, epoch or person.
//   - The LiveKit key slot of an epoch is epoch mod 16, so frames still in
//     flight under the previous key decrypt while everyone switches over.

namespace redvault::crypto::groupcalls {

class GroupKeyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using calls::Sealed;

inline constexpr std::size_t kRoomKeyBytes = 32;
/// LiveKit's default keyring size: epochs map onto key slots modulo this.
inline constexpr int kKeyringSize = 16;
/// A sealed key older (or newer) than this is refused.
inline constexpr std::int64_t kMaxKeyAgeMs = 5 * 60 * 1000;

/// Which fields of a sealed key must match; the same five the web app checks.
struct KeyMessage {
    std::string callId;
    std::string conversationId;
    std::int64_t epoch = 0;
    std::string from;
    std::string to;
};

SecretBytes newRoomKey();

/// The LiveKit key slot an epoch's key goes in.
int keySlot(std::int64_t epoch);

/// Seals `key` for one participant: crypto_box from our identity key to theirs.
Sealed sealRoomKey(const KeyMessage& message, ByteView key, ByteView recipientPublicKey, ByteView senderPrivateKey,
                   std::int64_t now = calls::nowMs());

/// Opens a room key from `senderPublicKey` and checks it's exactly the one expected. Throws GroupKeyError otherwise.
SecretBytes openRoomKey(const Sealed& sealed, ByteView senderPublicKey, ByteView recipientPrivateKey, const KeyMessage& expected,
                        std::int64_t now = calls::nowMs());

struct Participant {
    std::string userId;
    Bytes publicKey;
};

/// Four words from the call and every participant's identity key, as this app knows them; the order of
/// `participants` doesn't matter. If the server had swapped someone's key, that person's words would differ.
std::vector<std::string> groupSasWords(std::string_view callId, const std::vector<Participant>& participants, std::size_t count = 4);

}  // namespace redvault::crypto::groupcalls
