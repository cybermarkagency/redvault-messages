// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/crypto/story_crypto.hpp"
#include "redvault/e2ev3/chat.hpp"
#include "redvault/e2ev3/device.hpp"
#include "redvault/model/story_stickers.hpp"

// Part D on the desktop (docs/signal-integration/12-part-d-stories.md): stories whose key K travels over device keys
// instead of the account key. Nothing here uses the account (protocol-1) keys:
//   - the author's device sends K to every listed device of each viewer account, and to its own other devices, as a
//     pairwise `storykey` envelope whose conversation is the story id (at most 100 devices a request, in order, retried,
//     waiting for Tor like a chat message);
//   - a viewer keeps K in a local store sealed under the device's storage key (never in a chat history) while the
//     story can still be shown (archive and highlights outlive its end time), and forgets it once the story is
//     deleted, revoked or gone from what the server lists (the app decides that; StoryStore::forget);
//   - a named sticker answer is a pairwise `storyanswer` to the author's devices and this account's other devices; the
//     latest `rev` per (story, sticker, person) counts;
//   - an anonymous answer is an RVSD container (crypto/story_crypto.hpp) of sealed copies, one per device of the
//     author's verified list, each sealed by the engine to that device's Signal identity key (engine ops storySeal
//     and storyUnseal), stored on the server like RVSA was;
//   - a friend added after posting (a "friends" story) gets access first, then K (fillStoryGaps).
// Keys and answers are never logged. Qt-free, like the rest of e2ev3.

namespace redvault::e2ev3 {

/// A named answer's payload (the answer JSON object) may be at most this long, serialized.
inline constexpr std::size_t kMaxStoryAnswerPayload = 4096;
/// A delivery that reached no new device this many passes in a row (their keys can't be claimed) is given up.
inline constexpr int kStoryDeliveryAttempts = 5;

/// The `storykey` envelope (same id, c and ts rules as 9.3; `c` is the story id). Refuses one that wouldn't decode.
std::string encodeStoryKey(const std::string& storyId, const crypto::SecretBytes& key, std::int64_t expiresAt, const std::string& id,
                           std::int64_t ts);
/// The `storyanswer` envelope. Refuses one that wouldn't decode.
std::string encodeStoryAnswer(const std::string& storyId, const std::string& stickerId, std::int64_t rev, bool final, const Json& payload,
                              const std::string& id, std::int64_t ts);

/**
 * This device's story keys, the named answers it holds (as the author, or its own as a viewer) and the story messages
 * waiting to go out. One file sealed with XChaCha20-Poly1305 under a key derived from the device's storage key
 * (V3Device::deriveKey), rewritten on each change. Thread-safe.
 */
class StoryStore {
public:
    struct Key {
        std::string storyId;
        /// Who sent it: only used for a story the server says this account wrote.
        std::string authorId;
        std::shared_ptr<const crypto::SecretBytes> key;
        std::int64_t expiresAt = 0;
        std::int64_t arrivedAt = 0;
        /// This account's own story: kept past its end time (archive, highlights, late friends) until it's deleted.
        bool own = false;
    };
    struct Answer {
        std::string storyId;
        std::string stickerId;
        std::string userId;  // who answered
        std::int64_t rev = 0;
        bool final = false;
        std::string payload;  // the answer JSON object, as text
        std::int64_t at = 0;
    };
    /// A story message the engine hasn't encrypted yet, or one some devices still need.
    struct Delivery {
        std::string id;
        std::string storyId;
        std::string kind;  // "storykey" | "storyanswer"
        /// The accounts it goes to (this account's own id: its other devices).
        std::vector<std::string> userIds;
        std::string plaintext;  // the envelope, made once
        std::int64_t until = 0;  // nothing is sent after this (ms)
        /// "userId/deviceId" of every device that has it (or whose ciphertext waits in the Tor queue).
        std::vector<std::string> done;
        int attempts = 0;
    };

    StoryStore(std::filesystem::path file, crypto::Bytes key);
    /// The clock that decides expiry (ms). The wall clock unless a test sets another.
    void setClock(std::function<std::int64_t()> clock);
    std::int64_t now() const;

    /// Keeps K for (story, sender). False when that sender's key is already kept (the first wins).
    bool putKey(const std::string& storyId, const std::string& authorId, const crypto::SecretBytes& key, std::int64_t expiresAt, bool own = false);
    /// K for a story `authorId` wrote, if it arrived (and hasn't been forgotten).
    std::optional<Key> key(const std::string& storyId, const std::string& authorId) const;
    std::vector<Key> keys() const;
    /// Forgets everything about a story: its keys, answers and what waits to be sent for it.
    void forget(const std::string& storyId);
    /// Drops story messages whose time to be sent is over (keys stay until forget()). Returns how many.
    int dropStaleDeliveries();

    /// A named answer: kept when its rev is newer than the one held and that one isn't final. True when kept.
    bool putAnswer(const Answer& answer);
    std::vector<Answer> answers(const std::string& storyId) const;
    std::optional<Answer> answer(const std::string& storyId, const std::string& stickerId, const std::string& userId) const;

    void pushDelivery(Delivery delivery);
    void updateDelivery(const Delivery& delivery);
    void removeDelivery(const std::string& id);
    std::vector<Delivery> deliveries() const;
    bool hasDeliveries() const;

private:
    void load();
    void save() const;
    void forgetLocked(const std::string& storyId);
    std::filesystem::path file_;
    crypto::Bytes key_;
    std::function<std::int64_t()> clock_;
    mutable std::mutex mutex_;
    std::vector<Key> keys_;
    std::vector<Answer> answers_;
    std::vector<Delivery> deliveries_;
};

/// How a story message reaches devices it has no session with yet.
struct StoryFanOut {
    /// The conversation a device-list lookup (and a Signal certificate lookup) names for (story, account): the story id.
    std::function<std::string(const std::string& storyId, const std::string& userId)> listVia;
    /// The conversation a key claim names for an account. Empty or unset: the story id (the server's story rule).
    std::function<std::string(const std::string& userId)> claimVia;
};

struct StorySendResult {
    int delivered = 0;  // device copies that went (or wait, encrypted, in the Tor queue)
    int unreachable = 0;  // devices that got nothing this time (no session and no key to claim)
    /// Tor isn't connected, or the server isn't reachable: what's left waits, in order.
    bool waiting = false;
};

/**
 * The author's device, once the story is created with its viewers: keeps K (as its own) and sends `storykey` to every
 * listed device of `viewers` and to this account's other devices. `queueOnly`: Tor isn't connected, so it only waits.
 */
StorySendResult sendStoryKey(V3Device& device, StoryStore& store, const std::string& storyId, const crypto::SecretBytes& key,
                             std::int64_t expiresAt, const std::vector<std::string>& viewers, const StoryFanOut& fanOut = {},
                             bool queueOnly = false);

/**
 * A named answer to a sticker of `authorId`'s story, to the author's devices and this account's other devices. Its rev
 * is one more than this account's last answer there; a final answer can't be changed (EngineError "answer_final").
 */
StorySendResult sendStoryAnswer(V3Device& device, StoryStore& store, const std::string& storyId, const std::string& authorId,
                                const std::string& stickerId, bool final, const Json& payload, std::int64_t expiresAt,
                                const StoryFanOut& fanOut = {}, bool queueOnly = false);

/// Sends what waits (oldest first) and stops at the first that has to wait for Tor or the server.
StorySendResult sendQueuedStories(V3Device& device, StoryStore& store, const StoryFanOut& fanOut = {});

/// A friend added after a "friends" story went up (GET /stories/key-gaps, device-keys entries).
struct StoryGap {
    std::string storyId;
    std::vector<std::string> userIds;
};
/**
 * Late friends: for each gap whose story this device holds the key of, `addAccess` (POST /stories/:id/access) first,
 * then `storykey` to those friends' devices. Returns how many accounts got the key on its way.
 */
int fillStoryGaps(V3Device& device, StoryStore& store, const std::vector<StoryGap>& gaps,
                  const std::function<void(const std::string& storyId, const std::vector<std::string>& userIds)>& addAccess,
                  const StoryFanOut& fanOut = {});

/// Seals an RVSA payload once per device of the author (at most 10): the copies of an RVSD container.
using StorySealer = std::function<std::vector<crypto::stories::RvsdCopy>(const std::string& storyId, const std::string& authorId, crypto::ByteView payload)>;
/// The engine's sealer: each listed device's Signal certificate (GET users/:id/signal-certificates) and engine op `storySeal`.
StorySealer engineSealer(V3Device& device, const StoryFanOut& fanOut = {});
/// An anonymous answer as RVSD, one sealed copy per author device. Names nobody. Throws when no copy could be made.
crypto::Bytes sealStoryAnswer(const std::string& storyId, const std::string& authorId, const model::stories::Sticker& sticker,
                              const model::stories::Answer& answer, const StorySealer& seal);

/// Opens a sealed copy with this device's own key: the RVSA payload, or nothing when it can't be opened.
using SealedOpener = std::function<std::optional<crypto::Bytes>(crypto::ByteView sealed)>;
/// The engine's opener (engine op `storyUnseal`). An engine without Signal opens nothing.
SealedOpener engineOpener(V3Device& device);
/// The author's device opens its own copy of an RVSD answer; nothing when there's no copy for it or it can't be opened.
std::optional<model::stories::Answer> openStoryAnswer(const std::string& deviceId, const std::string& storyId, const model::stories::Sticker& sticker,
                                                      crypto::ByteView rvsd, const SealedOpener& open);

}  // namespace redvault::e2ev3
