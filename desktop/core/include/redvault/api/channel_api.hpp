// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"

// Channels (server/src/routes/channelRoutes.ts), mirroring ui/src/lib/api.ts.
// Public channels are plain text by design; a private channel's posts are
// encrypted with its link (Part C1, data/private_channels.hpp does the crypto;
// these are only the wire shapes). Reading goes out WITHOUT the session cookie
// (ApiClient::publicCall), so the server can't tell which account reads
// which channel; following is kept on this computer (model/channels.hpp). A
// private channel also needs its link's access token
// (crypto/channel_crypto.hpp), sent as X-Channel-Access, also on the
// signed-in calls about its posts (votes, ticks, suggestions). Media links
// are signed and expire; they're fetched without the cookie too. Free
// functions over ApiClient; all of them block.

namespace redvault::api::channels {

/// What anyone who can read a channel sees of it. Never who owns it.
struct Channel {
    std::string id;
    std::string name;
    std::string description;
    std::string createdAt;
    /// ISO time of the newest post; absent before the first.
    std::optional<std::string> lastPostAt;
    /// Left out of the directory; readable only with the private link.
    bool isPrivate = false;
    /// The API path of the channel's photo ("/api/public/channels/<id>/avatar?v=..."); fetch it with avatar() (no cookie).
    std::optional<std::string> avatarUrl;
    /// Only from the owner's own endpoints: the private link's secret, sealed to their public key (base64).
    std::optional<std::string> secretSealed;
    /// The owner takes suggested posts from followers.
    bool acceptSuggestions = false;
    /**
     * A private channel's newest key epoch (Part C1); null for a public one,
     * and for a private one made before C1 (no keys yet). The owner's own copy
     * keeps it while public too.
     */
    std::optional<int> keyEpoch;
    /// Only the owner's copy, while public: the last private link's secret, sealed to them (to convert old encrypted posts).
    std::optional<std::string> formerSecretSealed;
};

/// A media file's key, from inside an encrypted post (never from the server's own fields).
struct MediaKey {
    /// base64 secretstream key and header, and the lowercase hex SHA-256 of the encrypted blob.
    std::string key;
    std::string header;
    std::string sha256;
    /// The plain file's size; the blob is crypto::files::encryptedSizeFor(plainSize).
    std::int64_t plainSize = 0;

    bool operator==(const MediaKey&) const = default;
};

/// "photo", "video", "file", "audio" (podcasts), "voice" or "round" (a round video message).
struct Media {
    std::string id;
    std::string kind;
    std::string mime;
    std::int64_t size = 0;
    std::optional<std::string> filename;
    std::optional<std::int64_t> durationMs;
    std::optional<int> width;
    std::optional<int> height;
    /// A signed, expiring, cookie-less link to the bytes ("/api/channel-media/<id>?exp=...&sig=..."); it takes byte ranges.
    std::string url;
    /// Set for a private channel's media once its post was opened: the fields above then come from the ciphertext.
    std::optional<MediaKey> secret;

    bool operator==(const Media&) const = default;
};

/// A poll as readers see it. A quiz's `correct` stays empty until it's closed (or in the answer to your own vote).
struct Poll {
    std::string question;
    std::vector<std::string> options;
    bool multiple = false;
    bool quiz = false;
    bool closed = false;
    std::vector<int> counts;
    int voters = 0;
    std::optional<int> correct;
    std::optional<std::string> explanation;
    /**
     * An encrypted quiz's answer, known to this device from the ciphertext
     * but not shown (correct stays empty) until you've answered or it's closed.
     */
    std::optional<int> sealedCorrect;
    std::optional<std::string> sealedExplanation;

    bool operator==(const Poll&) const = default;
};

struct ChecklistItem {
    std::string text;
    bool done = false;

    bool operator==(const ChecklistItem&) const = default;
};

struct Checklist {
    std::string title;
    std::vector<ChecklistItem> items;
    /// Followers may tick items too.
    bool collaborative = false;

    bool operator==(const Checklist&) const = default;
};

/// A ciphertext under one epoch's content key (base64), as posts and suggestions carry it.
struct SealedBody {
    int keyEpoch = 0;
    std::string nonce;
    std::string ciphertext;

    bool operator==(const SealedBody&) const = default;
};

/// What the server knows of an encrypted poll: counts per option index, never the texts.
struct SealedPoll {
    int optionCount = 0;
    bool multiple = false;
    bool quiz = false;
    bool closed = false;
    std::vector<int> counts;
    int voters = 0;

    bool operator==(const SealedPoll&) const = default;
};

struct SealedChecklist {
    int itemCount = 0;
    bool collaborative = false;
    std::vector<bool> done;

    bool operator==(const SealedChecklist&) const = default;
};

/// An encrypted post as the server has it.
struct EncryptedPost {
    SealedBody sealed;
    /// "text", "poll", "checklist" or "media".
    std::string kind;
    std::optional<SealedPoll> poll;
    std::optional<SealedChecklist> checklist;

    bool operator==(const EncryptedPost&) const = default;
};

struct Post {
    std::string id;
    /// Formatted text (model/text_format.hpp); may be empty when the post is a photo, a poll and so on.
    std::string body;
    /// When it appeared (or, scheduled, when it will).
    std::string createdAt;
    /// How many devices have seen it, in the channel or forwarded into a chat.
    std::int64_t views = 0;
    /// Came from a follower's suggestion (who sent it is never shown).
    bool suggested = false;
    std::optional<Poll> poll;
    std::optional<Checklist> checklist;
    std::vector<Media> media;
    /// Set for an encrypted post (body is then "" and poll/checklist null until it's opened).
    std::optional<EncryptedPost> encrypted;
    /// An encrypted post this device couldn't open (no key for its epoch, or it doesn't decrypt).
    bool locked = false;
};

/// What the owner posts: text, plus at most one of a poll (or quiz), a checklist or uploads.
struct NewPost {
    std::string body;
    struct NewPoll {
        std::string question;
        std::vector<std::string> options;
        bool multiple = false;
        /// A quiz: the right option, and why (shown once answered).
        struct Quiz {
            int correct = 0;
            std::string explanation;
        };
        std::optional<Quiz> quiz;
    };
    std::optional<NewPoll> poll;
    struct NewChecklist {
        std::string title;
        std::vector<std::string> items;
        bool collaborative = false;
    };
    std::optional<NewChecklist> checklist;
    /// Finished uploads (startMedia + uploadMedia), in order.
    std::vector<std::string> media;
    /// ISO time to publish at; absent posts now.
    std::optional<std::string> publishAt;
};

/// What an upload is, given before its bytes.
struct UploadMeta {
    std::string kind;
    std::string mime;
    std::int64_t size = 0;
    std::optional<std::string> filename;
    std::optional<std::int64_t> durationMs;
    std::optional<int> width;
    std::optional<int> height;
};

struct StartedUpload {
    std::string id;
    /// Where the bytes go: "/api/channel-uploads/<id>".
    std::string uploadUrl;
};

struct Suggestion {
    std::string id;
    std::string body;
    /// A private channel's suggestion: its text and photos' keys are inside.
    std::optional<SealedBody> encrypted;
    /// Encrypted, and this device couldn't open it.
    bool locked = false;
    /// "pending", "approved" or "rejected".
    std::string status;
    std::string createdAt;
    std::vector<Media> media;
};

struct VoteResult {
    /// For an encrypted poll only counts, closed, voters, multiple and quiz are filled; the texts come from the opened post.
    Poll poll;
    std::vector<int> mine;
};

/// One epoch's content key, wrapped under a link's wrapKey (base64, 72 bytes).
struct WrappedKey {
    int epoch = 0;
    std::string wrapped;

    bool operator==(const WrappedKey&) const = default;
};

struct KeySet {
    std::optional<int> keyEpoch;
    /// Sorted by epoch.
    std::vector<WrappedKey> keys;
};

/// A private channel's post as sent: the ciphertext plus what the server must count (votes, ticks) and serve (media).
struct NewEncryptedPost {
    SealedBody sealed;
    struct Poll {
        int optionCount = 0;
        bool multiple = false;
        bool quiz = false;
    };
    std::optional<Poll> poll;
    struct Checklist {
        int itemCount = 0;
        bool collaborative = false;
    };
    std::optional<Checklist> checklist;
    /// Encrypted uploads (startEncryptedMedia), in order.
    std::vector<std::string> media;
    std::optional<std::string> publishAt;
};

/// After a visibility switch: posts still in the other format, oldest first.
struct Conversion {
    bool isPrivate = false;
    std::optional<int> keyEpoch;
    std::vector<Post> posts;
};

/// A page of the public directory and of a channel's posts.
inline constexpr int kPageSize = 30;
/// Channels one account may own at once (the server's MAX_CHANNELS_PER_USER).
inline constexpr int kMaxChannels = 5;
inline constexpr int kMaxNameLength = 80;
inline constexpr int kMaxDescriptionLength = 500;
inline constexpr int kMaxPostLength = 4000;
/// Posts one view report may count (the server's MAX_VIEWS_PER_REPORT).
inline constexpr std::size_t kMaxViewsPerReport = 100;
inline constexpr int kMaxPollOptions = 10;
inline constexpr int kMaxPollQuestion = 300;
inline constexpr int kMaxPollOption = 100;
inline constexpr int kMaxQuizExplanation = 200;
inline constexpr int kMaxChecklistItems = 30;
inline constexpr int kMaxChecklistText = 200;
/// Attachments in one post, and in one suggestion (photos only, each at most kMaxSuggestionPhotoBytes).
inline constexpr int kMaxMediaPerPost = 10;
inline constexpr int kMaxMediaPerSuggestion = 4;
inline constexpr std::int64_t kMaxSuggestionPhotoBytes = 20LL * 1024 * 1024;
/// The largest upload the server takes by default (photos, voice clips and round videos have smaller caps there).
inline constexpr std::int64_t kMaxUploadBytes = 2LL * 1024 * 1024 * 1024;

// --- Reading: no account, no cookie. `access` is a private channel's access token. ---

/// The directory (public channels only), newest activity first; `query` (trimmed) searches names and descriptions.
std::vector<Channel> search(ApiClient& api, const std::string& query, int offset = 0);
/// 404 when the channel doesn't exist, was deleted, or is private and `access` isn't its link's token.
Channel get(ApiClient& api, const std::string& id, const std::optional<std::string>& access = std::nullopt);
/// Newest first; `before` (a post's createdAt) pages back through the whole history.
std::vector<Post> posts(ApiClient& api, const std::string& id, const std::optional<std::string>& before = std::nullopt,
                        const std::optional<std::string>& access = std::nullopt, int limit = kPageSize);
/**
 * A channel's photo, by the `avatarUrl` the server hands out, fetched
 * without the cookie: the raw image bytes. Anything that isn't such a path
 * is refused rather than fetched.
 */
std::string avatar(ApiClient& api, const std::string& avatarUrl, const std::optional<std::string>& access = std::nullopt);
/// One anonymous view for each of these posts (post ids only, at most kMaxViewsPerReport). The caller makes sure a device counts a post once.
void recordViews(ApiClient& api, const std::vector<std::string>& postIds);

/// Whether `url` is a media link the server hands out (only those are ever fetched).
bool isMediaUrl(const std::string& url);
/// A media link's bytes, all of them, without the cookie (photos).
std::string fetchMedia(ApiClient& api, const std::string& url);
/// Bytes `first`..`last` (inclusive) of a media link, without the cookie (seeking in a video or podcast).
std::string fetchMediaRange(ApiClient& api, const std::string& url, std::uint64_t first, std::uint64_t last);
/// A media link's bytes streamed into `sink`, without the cookie (saving a file of any size).
void downloadMedia(ApiClient& api, const std::string& url, std::ostream& sink);

// --- Signed in ---

/// Your channels, each with its private link's sealed secret.
std::vector<Channel> mine(ApiClient& api);
/// Premium only (403 "premium_required"); at most kMaxChannels (409 "too_many_channels").
Channel create(ApiClient& api, const std::string& name, const std::string& description);
Channel update(ApiClient& api, const std::string& id, const std::string& name, const std::string& description);
/// Public again: back in the directory, and the private link stops mattering.
Channel setPublic(ApiClient& api, const std::string& id);
/**
 * Private, a reset link, or an old private channel's upgrade: the new (or,
 * for the upgrade, same) link's access token, its secret sealed to you, and
 * every epoch's content key wrapped under the link's wrapKey (exactly epochs
 * 1..n+1). Anyone with an older link loses access.
 */
Channel setPrivate(ApiClient& api, const std::string& id, const std::string& access, const std::string& secretSealed,
                   const std::vector<WrappedKey>& keys);
/// A private channel's wrapped keys: with its link's access token, no cookie.
KeySet keys(ApiClient& api, const std::string& id, const std::string& access);
/// Signed in: the owner (also while public), or anyone with a valid access token.
KeySet ownKeys(ApiClient& api, const std::string& id, const std::optional<std::string>& access = std::nullopt);
/// Owner: posts in the other format after a visibility switch, oldest first (published and scheduled).
Conversion conversion(ApiClient& api, const std::string& id, int limit = 50);
/// Owner: whether followers may suggest posts.
Channel setAcceptSuggestions(ApiClient& api, const std::string& id, bool accept);
/// `imageBase64` is a small square JPEG, as for profile photos.
Channel setAvatar(ApiClient& api, const std::string& id, const std::string& imageBase64);
void removeAvatar(ApiClient& api, const std::string& id);
/// The owner, or a super admin taking a channel down.
void remove(ApiClient& api, const std::string& id);

/// Owner: a post, now or (publishAt) scheduled.
Post post(ApiClient& api, const std::string& id, const NewPost& post);
/// Owner: a text-only post, now.
Post post(ApiClient& api, const std::string& id, const std::string& body);
/// Owner, private channel: an encrypted post, now or scheduled.
Post post(ApiClient& api, const std::string& id, const NewEncryptedPost& post);
/// Owner: an encrypted post edited (private channel), or a plain one converted after going private. publishAt must be empty.
Post replacePost(ApiClient& api, const std::string& id, const std::string& postId, const NewEncryptedPost& post);
/// Owner: an encrypted post left from before going public, as plain text. publishAt must be empty.
Post replacePost(ApiClient& api, const std::string& id, const std::string& postId, const NewPost& post);
/// Owner, private channel: starts an upload of an encrypted blob of `size` bytes.
StartedUpload startEncryptedMedia(ApiClient& api, const std::string& id, std::int64_t size);
/// A follower, private channel: an encrypted photo for a suggestion.
StartedUpload startEncryptedSuggestionMedia(ApiClient& api, const std::string& id, std::int64_t size,
                                            const std::optional<std::string>& access = std::nullopt);
/// A started upload's bytes from memory (an encrypted blob).
void uploadMediaBytes(ApiClient& api, const std::string& uploadUrl, const std::string& bytes,
                      const std::function<void(std::int64_t)>& onProgress = {});
/// Owner: starts an upload for a post; then send its bytes with uploadMedia().
StartedUpload startMedia(ApiClient& api, const std::string& id, const UploadMeta& meta);
/// A follower: starts uploading a photo for a suggested post.
StartedUpload startSuggestionMedia(ApiClient& api, const std::string& id, const UploadMeta& meta,
                                   const std::optional<std::string>& access = std::nullopt);
/**
 * Sends a started upload's bytes, streamed from the file at `path` (so a
 * 2 GB file never sits in memory), with the session cookie, as
 * application/octet-stream with an exact Content-Length. `onProgress` gets
 * the bytes sent so far. Only an upload URL the server handed out is used.
 */
void uploadMedia(ApiClient& api, const std::string& uploadUrl, const std::filesystem::path& path, std::int64_t size,
                 const std::function<void(std::int64_t)>& onProgress = {});

/// Owner: posts waiting for their time, soonest first.
std::vector<Post> scheduled(ApiClient& api, const std::string& id);
/// Owner: a scheduled post appears now.
Post publishNow(ApiClient& api, const std::string& id, const std::string& postId);
/// The owner, or a super admin taking a post down.
void removePost(ApiClient& api, const std::string& id, const std::string& postId);

/// Your vote (an empty list takes a poll vote back; a quiz's answer is final and comes back with the right one).
VoteResult vote(ApiClient& api, const std::string& id, const std::string& postId, const std::vector<int>& options,
                const std::optional<std::string>& access = std::nullopt);
/// Owner: no more votes; a quiz's answer shows to everyone.
Post closePoll(ApiClient& api, const std::string& id, const std::string& postId);
/// The owner ticks items, and anyone who can read the channel when it's collaborative. An encrypted checklist answers with empty texts.
Checklist setChecklistItem(ApiClient& api, const std::string& id, const std::string& postId, int index, bool done,
                           const std::optional<std::string>& access = std::nullopt);

/// A follower: a suggested post (text and up to kMaxMediaPerSuggestion photos).
Suggestion suggest(ApiClient& api, const std::string& id, const std::string& body, const std::vector<std::string>& media,
                   const std::optional<std::string>& access = std::nullopt);
/// A follower, private channel: an encrypted suggestion (newest epoch) with up to kMaxMediaPerSuggestion encrypted photos.
Suggestion suggest(ApiClient& api, const std::string& id, const SealedBody& sealed, const std::vector<std::string>& media,
                   const std::optional<std::string>& access = std::nullopt);
/// The suggestions you sent this channel, and what became of them.
std::vector<Suggestion> mySuggestions(ApiClient& api, const std::string& id);
/// Owner: suggestions waiting, oldest first. Never who sent them.
std::vector<Suggestion> suggestions(ApiClient& api, const std::string& id);
/// Owner: publishes a suggestion (its text possibly edited), now or at `publishAt`; it shows as the channel's, marked as suggested.
Post approveSuggestion(ApiClient& api, const std::string& id, const std::string& suggestionId, const std::optional<std::string>& body,
                       const std::optional<std::string>& publishAt);
/// Owner, private channel: publishes a suggestion re-encrypted (`sealed`), or the follower's own ciphertext as it is (nullopt).
Post approveSuggestionSealed(ApiClient& api, const std::string& id, const std::string& suggestionId, const std::optional<SealedBody>& sealed,
                       const std::optional<std::string>& publishAt);
void rejectSuggestion(ApiClient& api, const std::string& id, const std::string& suggestionId);

/// Goes to the admins, filed against the owner (whom the reporter never learns).
void report(ApiClient& api, const std::string& id, const std::string& reason);

}  // namespace redvault::api::channels
