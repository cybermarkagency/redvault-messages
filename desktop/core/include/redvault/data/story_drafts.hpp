// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/model/story_drafts.hpp"

// Unfinished stories, kept on this device only (the desktop counterpart of the web app's lib/storyDrafts.ts).
//
// Nothing readable is stored: a draft's details are sealed with a key derived one-way from the account's private
// key (crypto/story_crypto.hpp), so it opens only while you hold that account's key, and a photo or video is
// encrypted like any story media (crypto/file_crypto.hpp) with its key inside the sealed details. A draft never
// reaches the server, and signing out leaves only ciphertext behind. Ten per account, 30 days.
//
// On disk, per account: <root>/<userId>/<id>.draft (the sealed details, with the account id, the time saved
// and the nonce in the clear) and <id>.media (the encrypted photo or video). Blocking; call from a worker thread.

namespace redvault::data {

/// The photo or video to keep with a draft: the bytes of the file as the story would send it.
struct DraftMediaInput {
    std::string kind;  // "image" | "video"
    std::string mime;
    int width = 0;
    int height = 0;
    std::optional<std::int64_t> durationMs;
    std::string bytes;
};

struct LoadedDraft {
    model::stories::DraftContent content;
    /// The decrypted photo or video, if the draft had one (and it still decrypts).
    std::optional<DraftMediaInput> media;
};

class StoryDraftStore {
public:
    static constexpr std::size_t kMaxDrafts = 10;
    static constexpr std::int64_t kKeepMs = 30ll * 24 * 3600 * 1000;

    explicit StoryDraftStore(std::filesystem::path root);

    /// Your drafts, newest first. Ones that are too old, or can't be opened any more, are deleted.
    std::vector<model::stories::DraftSummary> list(std::string_view userId, crypto::ByteView privateKey, std::int64_t nowMs = 0) const;

    /// Saves a draft (a new one, or over `id`) and returns its id. The oldest goes when there are already ten.
    /// Throws std::runtime_error if it can't be written.
    std::string save(std::string_view userId, crypto::ByteView privateKey, const model::stories::DraftContent& content,
                     const std::optional<DraftMediaInput>& media, const std::optional<std::string>& id = std::nullopt,
                     std::int64_t nowMs = 0) const;

    /// Opens a draft for editing; nullopt if it is gone or can't be opened (it is then deleted).
    std::optional<LoadedDraft> load(std::string_view userId, crypto::ByteView privateKey, std::string_view id) const;

    void remove(std::string_view userId, std::string_view id) const;

    /// The folder of one account's drafts (empty path for an id that isn't a plain account id).
    std::filesystem::path folderOf(std::string_view userId) const;

private:
    std::filesystem::path root_;
};

/// Whether `id` is what save() makes: 24 lowercase hex characters (never a path).
bool isDraftId(std::string_view id);

}  // namespace redvault::data
