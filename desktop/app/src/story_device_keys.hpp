// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/model/rich_message.hpp"
#include "redvault/model/story_stickers.hpp"

namespace redvault::app {

/**
 * Part D (docs/signal-integration/12-part-d-stories.md): what Stories needs from this computer's device keys.
 * DeviceKeysSession implements it; without device keys (no engine, the server has them off, or this computer
 * isn't a trusted device) stories stay on the account keys. Callbacks run on the UI thread; the blocking calls
 * may run on any thread (they never touch the UI).
 */
class StoryDeviceKeys {
public:
    virtual ~StoryDeviceKeys() = default;

    /// A named answer this device holds: one viewer's latest answer to a sticker (the author's side), or this account's own.
    struct NamedAnswer {
        std::string stickerId;
        std::string userId;
        std::int64_t rev = 0;
        bool final = false;
        std::string payload;  // the answer JSON object
        std::int64_t at = 0;
    };

    /// New stories go over device keys now (UI thread).
    virtual bool storiesOnDeviceKeys() const = 0;
    /// K of a device-keys story `authorId` wrote, once it arrived (any thread). Nothing while it hasn't.
    virtual std::shared_ptr<const crypto::SecretBytes> storyKey(const std::string& storyId, const std::string& authorId) = 0;
    /// Drops what this device holds about a story: refused (K doesn't open it) or gone (any thread).
    virtual void forgetStory(const std::string& storyId) = 0;
    /// A story key this device holds (never the key itself).
    struct HeldKey {
        std::string storyId;
        std::string authorId;
        bool own = false;
        std::int64_t arrivedAt = 0;
    };
    /// Every story key held now (any thread): the app forgets those whose story the server lists no more.
    virtual std::vector<HeldKey> heldStoryKeys() = 0;
    /// The named answers held for a story (any thread).
    virtual std::vector<NamedAnswer> storyAnswers(const std::string& storyId) = 0;

    /// The author: keeps K and sends it to every listed device of `viewers` and this account's other devices (queued, retried).
    virtual void sendStoryKey(const std::string& storyId, std::shared_ptr<const crypto::SecretBytes> key, std::int64_t expiresAt,
                              std::vector<std::string> viewers, std::function<void(const QString& error)> done) = 0;
    /// A viewer's named answer, to the author's devices and this account's other devices.
    virtual void sendStoryAnswer(const std::string& storyId, const std::string& authorId, const std::string& stickerId, bool final,
                                 std::string payloadJson, std::int64_t expiresAt, std::function<void(const QString& error)> done) = 0;
    /// Late friends of the author's "friends" stories: `addAccess` (blocking, POST /stories/:id/access) first, then the key.
    virtual void fillStoryGaps(std::vector<std::pair<std::string, std::vector<std::string>>> gaps,
                               std::function<void(const std::string& storyId, const std::vector<std::string>& userIds)> addAccess) = 0;
    /// An anonymous answer sealed to each of the author's devices (RVSD). Blocking; throws when it can't be.
    virtual crypto::Bytes sealStoryAnswer(const std::string& storyId, const std::string& authorId, const model::stories::Sticker& sticker,
                                          const model::stories::Answer& answer) = 0;
    /// This device's copy of an RVSD answer, opened; nothing when it can't be. Blocking.
    virtual std::optional<model::stories::Answer> openStoryAnswer(const std::string& storyId, const model::stories::Sticker& sticker,
                                                                  const crypto::Bytes& rvsd) = 0;
    /// A reply or reaction to a story, as a device-key message in the 1:1 chat with its author (waits for Tor like any send).
    virtual void sendStoryReply(const std::string& chatId, const std::string& authorId, const std::string& text, const model::StoryReplyRef& ref,
                                std::function<void(const QString& error)> done) = 0;
};

}  // namespace redvault::app
