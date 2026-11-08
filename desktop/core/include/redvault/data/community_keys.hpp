// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/community_crypto.hpp"
#include "redvault/data/auth_repository.hpp"

namespace redvault::data {

/**
 * The community epoch keys this session holds, by conversation id, kept in
 * memory only (like every other conversation key here), plus the upkeep any
 * member's app does: delivering the current key to members who are waiting
 * (they paid or were let in) and, when someone left, rotating to a new one.
 * The server only says WHO is entitled; it never holds a key it can open.
 * Port of ui/src/lib/communityKeys.ts.
 *
 * Thread-safe: rings are swapped whole under a lock, and readers keep the
 * snapshot they got. Every call that talks to the server blocks.
 */
class CommunityKeyStore {
public:
    using Ring = std::shared_ptr<const crypto::community::KeyRing>;

    CommunityKeyStore(std::shared_ptr<api::ApiClient> api, std::shared_ptr<const SelfIdentity> identity);

    /// The ring loaded for a community conversation, if any.
    Ring ring(const std::string& conversationId) const;

    /// Fetches (or re-fetches) this member's sealed keys and opens them.
    Ring load(const std::string& communityId, const std::string& conversationId);

    /// The ring, re-fetched only if the conversation list says the community moved on to another epoch.
    Ring ensure(const std::string& communityId, const std::string& conversationId, std::int64_t keyEpoch, bool rekeyRequired);

    /**
     * Seals a new epoch key to everyone still in and installs it; returns the
     * reloaded ring. Throws ApiError on a race (stale_key_epoch,
     * member_set_changed): the caller reloads and carries on.
     */
    Ring rotate(const std::string& communityId, const std::string& conversationId);

    /// Delivers the current key to every member waiting for it; returns how many were let in.
    int enrollWaiting(const std::string& communityId, const std::string& conversationId);

    struct Upkeep {
        int rotated = 0;
        int enrolled = 0;
    };

    /**
     * One round of upkeep for every community this member is in. Safe to run
     * from several members' apps at once: the server takes the first rotation
     * or enrollment and refuses the rest with a 409, which is ignored here.
     * A round already running makes this a no-op.
     */
    Upkeep runMaintenance();

    /// Drops every ring (sign-out).
    void forget();

private:
    Ring store(crypto::community::KeyRing ring, const std::string& conversationId);

    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<const SelfIdentity> identity_;
    mutable std::mutex mutex_;
    std::map<std::string, Ring> rings_;
    std::atomic<bool> running_{false};
};

}  // namespace redvault::data
