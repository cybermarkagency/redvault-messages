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
// Plain text by design. Reading goes out WITHOUT the session cookie
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
    /// "pending", "approved" or "rejected".
    std::string status;
    std::string createdAt;
    std::vector<Media> media;
};

struct VoteResult {
    Poll poll;
    std::vector<int> mine;
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
/// Private, or a reset link: the new link's access token and its secret sealed to you. Anyone with an older link loses access.
Channel setPrivate(ApiClient& api, const std::string& id, const std::string& access, const std::string& secretSealed);
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
/// The owner ticks items, and anyone who can read the channel when it's collaborative.
Checklist setChecklistItem(ApiClient& api, const std::string& id, const std::string& postId, int index, bool done,
                           const std::optional<std::string>& access = std::nullopt);

/// A follower: a suggested post (text and up to kMaxMediaPerSuggestion photos).
Suggestion suggest(ApiClient& api, const std::string& id, const std::string& body, const std::vector<std::string>& media,
                   const std::optional<std::string>& access = std::nullopt);
/// The suggestions you sent this channel, and what became of them.
std::vector<Suggestion> mySuggestions(ApiClient& api, const std::string& id);
/// Owner: suggestions waiting, oldest first. Never who sent them.
std::vector<Suggestion> suggestions(ApiClient& api, const std::string& id);
/// Owner: publishes a suggestion (its text possibly edited), now or at `publishAt`; it shows as the channel's, marked as suggested.
Post approveSuggestion(ApiClient& api, const std::string& id, const std::string& suggestionId, const std::optional<std::string>& body,
                       const std::optional<std::string>& publishAt);
void rejectSuggestion(ApiClient& api, const std::string& id, const std::string& suggestionId);

/// Goes to the admins, filed against the owner (whom the reporter never learns).
void report(ApiClient& api, const std::string& id, const std::string& reason);

}  // namespace redvault::api::channels
