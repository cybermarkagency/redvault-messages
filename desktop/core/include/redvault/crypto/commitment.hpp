// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Port of ui/src/lib/commitment.ts, the client half of stage-3 "Transaction
// verified". MUST match server/src/crypto/commitment.ts byte-for-byte: the
// client recomputes the commitment over the exact encrypted envelope it
// sent and compares it with the server's, rather than trusting its word.

namespace redvault::crypto::commitment {

inline constexpr int kProtocolVersion = 1;

/// v1 = "v1:" + messageId + ":" + base64(ciphertext) + ":" + base64(nonce)
std::string canonicalEnvelopeString(std::string_view messageId, ByteView ciphertext, ByteView nonce,
                                    int protocolVersion = kProtocolVersion);

/// Lowercase hex SHA-256 of canonicalEnvelopeString().
std::string computeHex(std::string_view messageId, ByteView ciphertext, ByteView nonce);

/// A commitment is a public hash, not a secret, so plain (case-insensitive) comparison is fine.
bool matches(std::string_view localHex, std::string_view networkHex);

}  // namespace redvault::crypto::commitment
