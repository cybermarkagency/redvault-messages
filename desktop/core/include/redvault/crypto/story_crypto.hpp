// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "redvault/crypto/bytes.hpp"
#include "redvault/model/story_stickers.hpp"

// Port of ui/src/lib/storyCrypto.ts: stories, end-to-end encrypted, byte for
// byte the same as the web app (pinned by tests/stories_test.cpp with
// vectors the web code made). docs/stories.md has the design.
//
// Each story has its own random key K. Its details (text, stickers,
// background, and the key of its encrypted photo or video) are one JSON
// blob encrypted with K, bound to the story id. K reaches each viewer as
//   nonce (24) || crypto_box(payload, nonce, viewer public key, author private key)
// where payload = "RVST" | version 1 | story id (16 bytes) | K (32 bytes).
// crypto_box authenticates the author, so the server can't forge a story,
// and the id inside stops a key being moved onto another story.

namespace redvault::crypto::stories {

inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kWrappedKeyBytes = 24 + 4 + 1 + 16 + 32 + 16;  // 93
inline constexpr std::size_t kMaxHighlightTitle = 40;  // UTF-16 units, like the web's slice()

struct Sealed {
    Bytes ciphertext;
    Bytes nonce;
};

/// A fresh random key for one story.
SecretBytes newStoryKey();

/// Seals a story's key to one viewer (the author's own copy is sealed to themself). Throws CryptoError on a bad id or key.
Bytes wrapStoryKey(std::string_view storyId, ByteView key, ByteView recipientPublicKey, ByteView authorPrivateKey);

/**
 * Opens a viewer's copy, checking the author made it and that it's for
 * exactly this story. Throws CryptoError otherwise.
 */
SecretBytes unwrapStoryKey(std::string_view storyId, ByteView wrapped, ByteView authorPublicKey, ByteView viewerPrivateKey);

/// The story's details (JSON text) encrypted with its key, bound to its id.
Sealed sealMeta(std::string_view storyId, ByteView key, std::string_view metaJson);
/// Throws CryptoError on a wrong key, another story's id, or tampering. Returns the JSON text.
std::string openMeta(std::string_view storyId, ByteView key, ByteView ciphertext, ByteView nonce);

/// A highlight's name, encrypted with its cover story's key (trimmed, at most kMaxHighlightTitle).
Sealed sealHighlightTitle(std::string_view title, std::string_view coverStoryId, ByteView coverKey);
/// Empty when it can't be opened (this viewer can't see the cover, say).
std::string openHighlightTitle(ByteView ciphertext, ByteView nonce, std::string_view coverStoryId, ByteView coverKey);

/// Story settings (JSON text), encrypted with a key derived one-way from the account's private key.
Sealed sealPrefs(std::string_view prefsJson, ByteView privateKey);
/// Throws CryptoError when they can't be opened. Returns the JSON text.
std::string openPrefs(ByteView ciphertext, ByteView nonce, ByteView privateKey);

// --- Drafts (unfinished stories, on this device only) ------------------------------------------------------------
//
// A draft's details (JSON text) are sealed with a key derived one-way from the account's private key, bound to the
// account and the draft's id, so a draft opens only while you hold that account's key and can't be moved under
// another id or account. The same key derivation and binding as the web app's lib/storyDrafts.ts.

Sealed sealDraft(std::string_view userId, std::string_view draftId, std::string_view draftJson, ByteView privateKey);
/// Throws CryptoError on another account, another id, another key or tampering. Returns the JSON text.
std::string openDraft(std::string_view userId, std::string_view draftId, ByteView ciphertext, ByteView nonce, ByteView privateKey);

// --- Answers to stickers (viewer -> author) ------------------------------------------------------------------
//
// An answer is sealed so that only the story's author can read it:
//   authenticated ("RVSR"): nonce || crypto_box(payload, authorPublic, responderPrivate): the author knows exactly
//     who sent it (their key made it), and so does the sender, who can open their own answer again;
//   without a name ("RVSA"): crypto_box_seal(payload, authorPublic), no sender inside at all.
// payload = magic (4) | version 1 | story id (16) | sticker id length (1) | sticker id | answer JSON.
// The ids inside stop a server moving an answer onto another story or sticker. All of these throw CryptoError for
// anything that doesn't open or doesn't fit the sticker it is for, so a damaged answer is never half-counted.

/// An answer the author knows came from this account. Throws CryptoError if it doesn't fit the sticker.
Bytes sealAnswer(std::string_view storyId, const model::stories::Sticker& sticker, const model::stories::Answer& answer, ByteView authorPublicKey,
                 ByteView responderPrivateKey);
/**
 * Opens an authenticated answer. The author passes the responder's public key and their own private key; the
 * responder passes the author's public key and their own private key.
 */
model::stories::Answer openAnswer(std::string_view storyId, const model::stories::Sticker& sticker, ByteView envelope, ByteView otherPartyPublicKey,
                                  ByteView ownPrivateKey);
/// An answer with no sender inside (a question marked "without my name").
Bytes sealAnonymousAnswer(std::string_view storyId, const model::stories::Sticker& sticker, const model::stories::Answer& answer,
                          ByteView authorPublicKey);
model::stories::Answer openAnonymousAnswer(std::string_view storyId, const model::stories::Sticker& sticker, ByteView envelope,
                                           ByteView authorPublicKey, ByteView authorPrivateKey);

// --- Totals (author -> everyone who can see the story) ---------------------------------------------------------
//
// Aggregates only, never who answered what. Sealed with the story key, bound to the story, the sticker and the
// revision, so a server can't forge totals or serve them for another sticker (it can still serve an older revision).

/// `revision` is 1 or more. Throws CryptoError otherwise.
Sealed sealTotals(std::string_view storyId, std::string_view stickerId, std::uint32_t revision, const model::stories::Totals& totals, ByteView storyKey);
/// Throws CryptoError on a wrong key, story, sticker or revision, tampering, or totals that don't fit the sticker.
model::stories::Totals openTotals(std::string_view storyId, const model::stories::Sticker& sticker, std::uint32_t revision, ByteView ciphertext,
                                  ByteView nonce, ByteView storyKey);

}  // namespace redvault::crypto::stories
