// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/model/stories.hpp"

// An unfinished story, as the web app's lib/storyDrafts.ts writes it (the sealed JSON of a draft): what the
// composer holds, a short summary for the list, and the key of the draft's encrypted photo or video. Drafts stay
// on the device that saved them, so nothing crosses between apps; the same layout is kept so the two stores
// mean the same thing (tests/vectors/story_draft_vectors.json pins it). Read leniently: a saved draft that
// has a field gone odd still opens with that field back at its default.

namespace redvault::model::stories {

/// What the composer holds. The photo or video itself is kept separately, encrypted (see DraftMediaRef).
struct DraftContent {
    std::string mode = "text";  // "text" | "media"
    std::string text;
    std::string background = "sunset";
    std::string font = "classic";
    std::vector<Overlay> overlays;
    std::vector<Sticker> stickers;
    std::string audience = "friends";  // "friends" | "close_friends" | "custom"
    std::vector<std::string> customIds;
    bool allowReplies = true;
    int ttlSeconds = 86400;
};

/// The encrypted photo or video of a draft: its key and header travel only inside the sealed details.
struct DraftMediaRef {
    std::string kind;  // "image" | "video"
    /// base64: the file key and secretstream header (crypto/file_crypto.hpp).
    std::string key;
    std::string header;
    std::string sha256;
    std::uint64_t encryptedSize = 0;
    std::uint64_t chunkSize = 0;
    std::string mime;
    int width = 0;
    int height = 0;
    std::optional<std::int64_t> durationMs;
};

/// What the list of drafts shows.
struct DraftSummary {
    std::string id;
    /// Epoch ms: when it was last saved.
    std::int64_t updatedAtMs = 0;
    std::string kind;     // "image" | "video" | "text"
    std::string preview;  // the start of the text, or "Photo" / "Video"
    int stickerCount = 0;
};

/// The sealed details of one draft.
struct SealedDraft {
    DraftContent content;
    std::string kind = "text";
    std::string preview;
    int stickerCount = 0;
    std::optional<DraftMediaRef> media;
};

/// Builds the details to seal: the summary is worked out from the content, the way the web app does.
SealedDraft makeSealedDraft(DraftContent content, std::optional<DraftMediaRef> media);
std::string toJson(const SealedDraft& draft);
/// nullopt unless it is a version-1 draft with its content.
std::optional<SealedDraft> parseSealedDraft(std::string_view json);

}  // namespace redvault::model::stories
