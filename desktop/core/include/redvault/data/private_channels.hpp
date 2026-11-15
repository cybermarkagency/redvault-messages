// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/api/channel_api.hpp"
#include "redvault/crypto/bytes.hpp"

// Private channels encrypted with the link (Part C1,
// docs/signal-integration/10-part-c1-private-channels.md): the device side.
// Everything here is Qt-free and blocking; the app runs it off the UI thread.
//
// Anyone holding the current link's secret unwraps every epoch's content key
// (crypto/channel_e2ee.hpp) and reads the whole history; the server stores
// ciphertext, opaque media blobs and only the counts it must keep (votes per
// option index, ticks, views). Public channels don't come through here.
//
// A post's plaintext (UTF-8 JSON, the same in every app):
//
//   {"v":1,"body":"<formatted text, at most 4000 UTF-16 units>","content":
//      null
//    | {"poll":{"question","options":[..],"multiple"?:true,"quiz"?:{"correct"?:int,"explanation"?:str}}}
//    | {"checklist":{"title","items":[str..],"collaborative"?:true}}
//    | {"media":[{"id","kind","mime","filename"?,"size","durationMs"?,"width"?,"height"?,
//                "fileKey","header","encryptedSize","ciphertextSha256"}..]}}
//
// The same layout as the Android app (docs/signal-integration/
// 10-part-c1-android-notes.md): false and null fields are left out, unknown
// ones ignored. A media entry's "id" is the channel_media id the post lists
// (ids the content doesn't name aren't shown); "fileKey" and "header"
// (base64) and "ciphertextSha256" (hex of the blob) open the blob, which is
// crypto/file_crypto.hpp's secretstream; "size" is the real size and
// "encryptedSize" the blob's. A quiz converted from a plain one the owner
// couldn't see the answer to has no "correct". Suggestions use the same
// plaintext (photos as media).
//
// Nothing here logs: keys, secrets and content never leave memory.

namespace redvault::data::private_channels {

/// Each epoch's content key this device could open, plus the channel's newest epoch.
class Keyring {
public:
    /// The channel's newest epoch as the server said (what new posts must use); nullopt for a channel without keys.
    std::optional<int> keyEpoch;

    const crypto::SecretBytes* key(int epoch) const;
    void put(int epoch, crypto::SecretBytes key);
    bool empty() const { return keys_.empty(); }
    std::size_t size() const { return keys_.size(); }
    /// Holds the newest epoch's key: this device can post.
    bool canSeal() const { return keyEpoch && key(*keyEpoch) != nullptr; }
    /// Holds every epoch 1..keyEpoch (needed to re-wrap them all on a reset).
    bool complete() const;

private:
    std::map<int, crypto::SecretBytes> keys_;
};

using KeyringPtr = std::shared_ptr<const Keyring>;

// --- The plaintext ---

/// What an encrypted post or suggestion says inside.
struct Content {
    std::string body;
    /// A quiz whose answer isn't known has quiz->correct == -1.
    std::optional<api::channels::NewPost::NewPoll> poll;
    std::optional<api::channels::NewPost::NewChecklist> checklist;
    /// Each with `secret` set; `url` empty.
    std::vector<api::channels::Media> media;
};

/// Text length as the server counts it (JavaScript's string length).
std::size_t utf16Length(std::string_view utf8);
std::string toPlaintext(const Content& content);
/// nullopt for anything that isn't a version-1 plaintext.
std::optional<Content> parsePlaintext(std::string_view plaintext);

// --- Keys ---

/// Unwraps every key that opens with this link's secret (keys that don't are left out).
Keyring unwrapAll(const api::channels::KeySet& set, std::string_view secret, const std::string& channelId);
/// A reader with the link: the public keys route (access token, no cookie).
Keyring readerKeys(api::ApiClient& api, const std::string& channelId, std::string_view secret);
/**
 * The owner: the signed-in keys route, opened with the current link's secret
 * (private) or the last one's (public, formerSecretSealed), unsealed with
 * their key pair. An empty keyring when there's nothing to open.
 */
Keyring ownerKeys(api::ApiClient& api, const api::channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey);
/// The secret the owner's keys are wrapped under (see ownerKeys), or nullopt.
std::optional<std::string> ownerSecret(const api::channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey);

// --- Opening ---

/**
 * An encrypted post opened into the plain shape the app shows: body, poll
 * (texts from inside, counts from the server), checklist (texts inside,
 * ticks from the server) and media (real kind, name and size, with their
 * keys). A post that won't open comes back `locked`. Plain posts are
 * returned as they are.
 */
api::channels::Post open(api::channels::Post post, const Keyring& keys, const std::string& channelId);
std::vector<api::channels::Post> open(std::vector<api::channels::Post> posts, const Keyring& keys, const std::string& channelId);
api::channels::Suggestion open(api::channels::Suggestion suggestion, const Keyring& keys, const std::string& channelId);
/// The plaintext of an encrypted post or suggestion, or nullopt.
std::optional<Content> contentOf(const api::channels::SealedBody& sealed, const Keyring& keys, const std::string& channelId);

/**
 * A vote's or a close's answer for an opened post: the server's counts and
 * closed state, the texts this device already has. A quiz's answer shows
 * once you've answered (`answered`) or it's closed.
 */
api::channels::Poll mergePoll(const api::channels::Poll& opened, const api::channels::Poll& fromServer, bool answered);
api::channels::Checklist mergeChecklist(const api::channels::Checklist& opened, const api::channels::Checklist& fromServer);

// --- Sealing ---

/// What the owner's composer (or an edit, or a conversion) puts in a private post.
struct Draft {
    Content content;
    std::optional<std::string> publishAt;
};

/// The encrypted post under the newest epoch. Throws UserFacingError when this device has no key for it or the text is too long.
api::channels::NewEncryptedPost seal(const Draft& draft, const Keyring& keys, const std::string& channelId);
/// A suggestion's ciphertext under the newest epoch.
api::channels::SealedBody sealSuggestion(const Content& content, const Keyring& keys, const std::string& channelId);

/// Fetches the keys again (the link was reset meanwhile).
using Refetch = std::function<KeyringPtr()>;

/**
 * Posts (or, with `postId`, replaces) under the newest epoch. On
 * `stale_key_epoch` (the link was reset meanwhile) it fetches the keys once
 * more and tries again; `keys` is updated to what it used.
 */
api::channels::Post send(api::ApiClient& api, const std::string& channelId, const Draft& draft, KeyringPtr& keys, const Refetch& refetch,
                         const std::optional<std::string>& postId = std::nullopt);

// --- Media ---

using Progress = std::function<void(std::int64_t sentBytes)>;
/// Starts an encrypted upload of a blob of this many bytes (owner: startEncryptedMedia; follower: startEncryptedSuggestionMedia).
using StartEncrypted = std::function<api::channels::StartedUpload(std::int64_t size)>;

/**
 * Encrypts `plain` (exactly meta.size bytes) on the fly with a fresh file key
 * and uploads the blob; returns the entry for the post: `meta` with its id
 * and `secret`. The plain bytes never reach the server, nor a file.
 */
api::channels::Media uploadEncrypted(api::ApiClient& api, const StartEncrypted& start, std::istream& plain, api::channels::Media meta,
                                     const Progress& onProgress = {});
/// A private channel's media, fetched without the cookie and decrypted into memory. Throws FileDecryptError if it doesn't open.
std::string fetchDecrypted(api::ApiClient& api, const api::channels::Media& media);
/// The same, streamed in byte ranges into `out` (saving a large file). On a throw `out` must be discarded.
void downloadDecrypted(api::ApiClient& api, const api::channels::Media& media, std::ostream& out);

// --- The owner switching visibility ---

struct Switched {
    api::channels::Channel channel;
    /// The (new) link's secret.
    std::string secret;
    KeyringPtr keys;
};

/**
 * Makes the channel private with a fresh link, or resets its link: a new
 * epoch n+1, and every epoch's key (the old ones unwrapped with the current
 * or former link) re-wrapped under the new link. Old links stop working.
 * Throws UserFacingError when this device can't open all the old keys
 * (followers would lose the history).
 */
Switched makePrivate(api::ApiClient& api, const api::channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey);
/**
 * A private channel made before C1 (keyEpoch null): epoch 1 under the link
 * followers already hold (same access token and sealed secret).
 */
Switched setUpKeys(api::ApiClient& api, const api::channels::Channel& own, crypto::ByteView publicKey, crypto::ByteView privateKey);
/// The owner's channel needs setUpKeys: private, without keys.
bool needsKeys(const api::channels::Channel& own);

struct ConversionHooks {
    /// Stop between posts (resumable: the server's list is the state).
    std::function<bool()> cancelled;
    /// Waits before trying again after a rate limit (429); `attempt` counts from 1. After 5 in a row convert() throws the 429 (resume later).
    std::function<void(int attempt)> backOff;
    /// Posts converted so far.
    std::function<void(int converted)> progress;
};

struct ConversionResult {
    int converted = 0;
    /// Nothing is left in the old format.
    bool finished = false;
    /// Posts that couldn't be converted (an encrypted one no key here opens).
    int stuck = 0;
};

/**
 * Converts the posts left in the other format after a switch, oldest first,
 * page by page through the conversion route: plain posts into ciphertext
 * under the newest epoch (private), or encrypted posts back into plain text
 * (public), media re-uploaded in the new format. Resumable at any point.
 */
ConversionResult convert(api::ApiClient& api, const std::string& channelId, const Keyring& keys, const ConversionHooks& hooks = {});

}  // namespace redvault::data::private_channels
