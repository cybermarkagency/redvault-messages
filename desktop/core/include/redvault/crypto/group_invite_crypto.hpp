// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"

// Group invite links, byte-compatible with the web app
// (ui/src/lib/groupInviteCrypto.ts): <web app>/?invite=<id>#<secret>. The
// 32-byte secret lives only in the URL fragment, which no server ever sees.
// From it come two keys that can't be worked out from each other:
//   - an encryption key, under which the group's name and (when joins needn't
//     be approved) the group key are stored on the server;
//   - an auth token, shown to the server to prove the link was really opened.
// The server keeps only a hash of the token, so it can't read either.
// Members keep the secret itself encrypted with the group key, so any of
// them can show and share the link again.
//
// Everything is XChaCha20-Poly1305 (24-byte nonce); ciphertexts and nonces
// are standard padded base64, the secret in the link is base64url without
// padding. Pinned to the web app by tests/group_invite_test.cpp
// (vectors/group_invite_vectors.json, made with the web app's own code).

namespace redvault::crypto::invites {

inline constexpr std::size_t kSecretBytes = 32;

/// The link's two keys: `key` encrypts, `auth` (base64) is what the server checks.
struct InviteKeys {
    SecretBytes key;
    std::string auth;
};

InviteKeys inviteKeys(ByteView secret);

/// The secret as the link's fragment carries it: base64url, no padding (43 characters).
std::string secretToFragment(ByteView secret);

/// The secret from a link's fragment (a leading '#' is fine); nullopt unless it's exactly 32 bytes of base64url.
std::optional<SecretBytes> secretFromFragment(std::string_view fragment);

/// The shareable address: <web app base>?invite=<id>#<secret>. `webAppBase` ends with '/'.
std::string inviteUrl(std::string_view webAppBase, std::string_view linkId, ByteView secret);

struct ParsedInvite {
    std::string linkId;
    /// Still base64url text, as in the link.
    std::string secret;
};

/// A pasted group invite link (…?invite=<id>#<secret>), or a whole message with one in it, checked for shape only.
std::optional<ParsedInvite> parseInvite(std::string_view text);

/// What the server stores for a link (all base64); the wrapped key is empty when an admin approves each join.
struct LinkInput {
    std::string auth;
    std::string secretCiphertext;
    std::string secretNonce;
    std::string previewCiphertext;
    std::string previewNonce;
    std::optional<std::string> wrappedKey;
    std::optional<std::string> wrappedKeyNonce;
};

struct NewLink {
    SecretBytes secret;
    LinkInput input;
};

/// A new link for a group (turning it on, or a reset): a fresh secret, and what the server stores for it.
NewLink newInviteLink(ByteView groupKey, std::string_view groupName, bool approval);

/// The random parts of a link, chosen by the caller. Only for tests and vectors: never reuse a nonce.
struct LinkRandomness {
    Bytes secret;
    Bytes secretNonce;
    Bytes previewNonce;
    Bytes wrappedKeyNonce;
};
NewLink newInviteLink(ByteView groupKey, std::string_view groupName, bool approval, const LinkRandomness& fixed);

struct WrappedKey {
    std::string wrappedKey;
    std::string wrappedKeyNonce;
};

/// The group key under the link's key: lets people join without an admin approving.
WrappedKey wrapGroupKeyForLink(ByteView groupKey, ByteView secret);
WrappedKey wrapGroupKeyForLink(ByteView groupKey, ByteView secret, ByteView nonce);

struct OpenedLink {
    SecretBytes secret;
    std::string url;
};

/// A member's view of the group's link: its secret, unlocked with the group key, and the URL to share. Nullopt if it won't open.
std::optional<OpenedLink> openInviteLink(std::string_view linkId, std::string_view secretCiphertext, std::string_view secretNonce,
                                         ByteView groupKey, std::string_view webAppBase);

struct OpenedPreview {
    std::string name;
    /// Present when no approval is needed: the group key, to seal to yourself.
    std::optional<SecretBytes> groupKey;
};

/**
 * What the link shows someone about to join: the group's name and, if no
 * approval is needed, its key. Throws CryptoError on a wrong secret.
 */
OpenedPreview openInvitePreview(std::string_view previewCiphertext, std::string_view previewNonce,
                                const std::optional<std::string>& wrappedKey, const std::optional<std::string>& wrappedKeyNonce,
                                ByteView secret);

}  // namespace redvault::crypto::invites
