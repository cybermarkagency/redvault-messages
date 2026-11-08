// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"

// Stories (server/src/routes/storyRoutes.ts), mirroring ui/src/lib/storiesApi.ts.
// Everything here is ciphertext, base64 strings or ids: the server never sees
// what a story shows. Free functions over ApiClient; all of them block.

namespace redvault::api::stories {

struct Story {
    std::string id;
    std::string authorId;
    std::string kind;  // "image" | "video" | "text"
    std::string metaCiphertext;
    std::string metaNonce;
    std::optional<std::uint64_t> mediaSize;
    bool hasMedia = false;
    bool allowReplies = true;
    std::string createdAt;
    std::string expiresAt;
    /// This viewer's copy of the story key, sealed by the author.
    std::string wrappedKey;
    /// Feed: whether this viewer shared a view. Own: how many did. Archive: when it left the story.
    bool seen = false;
    int viewCount = 0;
    std::optional<std::string> archivedAt;
};

struct AuthorStories {
    std::string authorId;
    std::vector<Story> stories;
};

struct Feed {
    std::vector<Story> own;
    std::vector<AuthorStories> authors;
};

struct Highlight {
    std::string id;
    std::string ownerId;
    std::string titleCiphertext;
    std::string titleNonce;
    std::string coverStoryId;
    std::string createdAt;
    std::vector<Story> stories;
};

struct WrappedKey {
    std::string userId;
    std::string wrappedKey;
};

struct KeyGap {
    std::string storyId;
    /// Friends without a key yet, with the public key (base64) the server has for them.
    std::vector<std::pair<std::string, std::string>> users;
};

struct NewStory {
    std::string id;
    std::string kind;
    std::string metaCiphertext;
    std::string metaNonce;
    std::optional<std::uint64_t> mediaSize;
    std::string audience;  // "friends" | "close_friends" | "custom"
    bool allowReplies = true;
    std::vector<WrappedKey> keys;
    /// How long it stays up (1, 6, 12, 24, 48 hours or 7 days, in seconds); the server uses 24 hours when left out.
    std::optional<int> ttlSeconds;
};

/// What the server offers. `interactive` and `ttlSeconds` are missing from servers that predate interactive stories:
/// no stickers to answer, and 24 hours only.
struct Config {
    bool media = false;
    bool interactive = false;
    std::vector<int> ttlSeconds;
};

/// One answer in the author's inbox: sealed to them, the server never reads it.
struct AnswerRow {
    std::string stickerId;
    /// Empty for an answer sent without a name.
    std::string responderId;
    bool anonymous = false;
    std::string envelope;  // base64
    int revision = 0;
    std::string respondedAt;
};

/// What the caller already answered on a story.
struct MyAnswerRow {
    std::string stickerId;
    bool anonymous = false;
    std::string envelope;
    int revision = 0;
};

/// Totals the author published, sealed with the story key.
struct TotalsRow {
    std::string stickerId;
    int revision = 0;
    std::string nonce;
    std::string ciphertext;
};

struct Prefs {
    std::optional<std::string> prefsCiphertext;
    std::optional<std::string> prefsNonce;
    bool archiveEnabled = false;
    int revision = 0;
};

struct HighlightInput {
    std::optional<std::string> titleCiphertext;
    std::optional<std::string> titleNonce;
    std::optional<std::string> coverStoryId;
    std::optional<std::vector<std::string>> storyIds;
};

Config config(ApiClient& api);
/// Whether photo and video stories can be posted (the server has private storage).
bool mediaEnabled(ApiClient& api);
Feed feed(ApiClient& api);
/// Returns whether it's published yet (a text story is at once; media once uploaded).
bool create(ApiClient& api, const NewStory& story);
/// The encrypted photo or video, exactly the announced size.
void uploadMedia(ApiClient& api, const std::string& storyId, std::string ciphertext);
std::string downloadMedia(ApiClient& api, const std::string& storyId);
void markViewed(ApiClient& api, const std::string& storyId);
std::vector<std::string> viewers(ApiClient& api, const std::string& storyId);
void remove(ApiClient& api, const std::string& storyId);
std::vector<Story> archive(ApiClient& api);
std::vector<KeyGap> keyGaps(ApiClient& api);
int addKeys(ApiClient& api, const std::string& storyId, const std::vector<WrappedKey>& keys);
/// Hide stories from someone: all of yours, or just `storyIds`.
int revoke(ApiClient& api, const std::string& userId, const std::optional<std::vector<std::string>>& storyIds = std::nullopt);

// --- Interactive stickers: sealed answers in, totals out (docs/stories-advanced/api.md) ---

/// Answers a sticker (a later call replaces it unless the first was final: ApiError 409 "response_final"). Returns the revision.
int putAnswer(ApiClient& api, const std::string& storyId, const std::string& stickerId, const std::string& envelopeBase64, bool anonymous, bool final);
/// The author's inbox; anyone else gets ApiError 404.
std::vector<AnswerRow> answers(ApiClient& api, const std::string& storyId);
std::vector<MyAnswerRow> myAnswers(ApiClient& api, const std::string& storyId);
/// Author only. ApiError 409 "stale_revision" when a higher revision is already stored (another device, say).
void putTotals(ApiClient& api, const std::string& storyId, const std::string& stickerId, int revision, const std::string& nonce,
               const std::string& ciphertext);
std::vector<TotalsRow> totals(ApiClient& api, const std::string& storyId);
/// ApiError 409 "already_reported" the second time. `evidence` (up to 600 characters) is sent only when the reporter chose to include it.
void reportStory(ApiClient& api, const std::string& storyId, const std::string& reason, const std::optional<std::string>& evidence);
/// The author reports an abusive answer by its exact envelope (admins see who sent it, the author does not need to).
void reportAnswer(ApiClient& api, const std::string& storyId, const std::string& stickerId, const std::string& envelopeBase64, const std::string& reason,
                  const std::optional<std::string>& evidence);

std::vector<Highlight> highlights(ApiClient& api, const std::string& userId);
std::string createHighlight(ApiClient& api, const std::string& titleCiphertext, const std::string& titleNonce, const std::string& coverStoryId,
                            const std::vector<std::string>& storyIds);
void updateHighlight(ApiClient& api, const std::string& highlightId, const HighlightInput& patch);
void removeHighlight(ApiClient& api, const std::string& highlightId);

Prefs prefs(ApiClient& api);
/// Throws ApiError 409 "prefs_conflict" when another device saved first. Returns the new revision.
Prefs putPrefs(ApiClient& api, const std::string& ciphertext, const std::string& nonce, bool archiveEnabled, int revision);

}  // namespace redvault::api::stories
