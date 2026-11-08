// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Port of ui/src/lib/postCrypto.ts: exclusive posts, end-to-end encrypted,
// byte for byte the same as the web app (pinned by tests/earnings_test.cpp
// with vectors the web code made). docs/creator-earnings.md has the design.
//
// Each post has its own random key K. Its body (text, and the key and header
// of an encrypted photo or video) is one JSON blob encrypted with K, bound to
// the post id. K reaches readers as
//   nonce (24) || crypto_box(payload, nonce, reader public key, author private key)
// where payload = "RVEP" | version 1 | post id (16 bytes) | K (32 bytes): the
// story-key construction with its own tag, so a story key can never pass for
// a post key. The author's app seals K to itself when posting and to each
// buyer once the server has confirmed their payment; the server never can.

namespace redvault::crypto::posts {

inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kWrappedKeyBytes = 24 + 4 + 1 + 16 + 32 + 16;  // 93

struct Sealed {
    Bytes ciphertext;
    Bytes nonce;
};

/// A fresh random key for one post.
SecretBytes newPostKey();

/// Seals a post's key to one reader (the author's own copy is sealed to themself). Throws CryptoError on a bad id or key.
Bytes wrapPostKey(std::string_view postId, ByteView key, ByteView recipientPublicKey, ByteView authorPrivateKey);

/// Opens a reader's copy, checking the author made it and that it's for exactly this post. Throws CryptoError otherwise.
SecretBytes unwrapPostKey(std::string_view postId, ByteView wrapped, ByteView authorPublicKey, ByteView readerPrivateKey);

/// The post's body (JSON text) encrypted with its key, bound to its id.
Sealed sealBody(std::string_view postId, ByteView key, std::string_view bodyJson);
/// Throws CryptoError on a wrong key, another post's id, or tampering. Returns the JSON text.
std::string openBody(std::string_view postId, ByteView key, ByteView ciphertext, ByteView nonce);

}  // namespace redvault::crypto::posts
