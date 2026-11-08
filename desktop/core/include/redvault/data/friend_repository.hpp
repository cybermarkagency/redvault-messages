// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/data/auth_repository.hpp"
#include "redvault/data/chat_models.hpp"
#include "redvault/data/key_trust.hpp"
#include "redvault/data/message_keys.hpp"

namespace redvault::data {

/// Someone found by handle: a read-only preview, nothing is created server-side.
struct ContactPreview {
    std::string userId;
    std::string handle;
    std::optional<std::string> avatarUrl;
    bool isSelf = false;
    bool deleted = false;
};

/// A pending friend request with the other side's current handle resolved.
struct FriendRequestView {
    std::string id;
    std::string userId;
    std::string handle;
    std::optional<std::string> avatarUrl;
};

struct FriendRequests {
    std::vector<FriendRequestView> incoming;
    std::vector<FriendRequestView> outgoing;
};

/// Port of android/.../data/FriendRepository.kt. Blocking; call from a worker thread.
class FriendRepository {
public:
    /// `contacts`: public key pins, checked before anything is sealed to someone.
    explicit FriendRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<ContactKeyPins> contacts = nullptr);

    ContactPreview lookup(const std::string& handle, const std::string& selfUserId);

    void sendRequest(const std::string& handle);

    /// Both directions of pending requests. Requests whose other side can't be resolved any more are skipped.
    FriendRequests pending();

    /**
     * Accepts a request: generates a fresh conversation key and seals it to
     * both sides' public keys, and the server creates the friendship and the
     * chat together. Returns the new conversation's id.
     */
    std::string accept(const SelfIdentity& identity, const FriendRequestView& request);

    /// The requester isn't told either way.
    void deny(const std::string& requestId);

    void cancel(const std::string& requestId);

private:
    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<ContactKeyPins> contacts_;
};

/**
 * Port of android/.../data/GroupRepository.kt. The group key is sealed
 * separately to every member's public key, and the name is encrypted with
 * it, so the server never learns either.
 */
class GroupRepository {
public:
    /// `keys`: group key epochs (db/migrations/030); optional for tests.
    explicit GroupRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<MessageKeys> keys = nullptr);

    std::vector<api::FriendDto> friends();

    /// Creates a group with some of your friends; returns its id.
    std::string create(const SelfIdentity& identity, const std::string& name, const std::vector<api::FriendDto>& members);

    /// Owner/admin: seals the group key for a friend, with a signed notice naming them. They only see messages from now on.
    void add(const ConversationMeta& meta, const api::FriendDto& friendToAdd);

    /// Owner/admin: lets in someone who asked through the invite link: their own copy of the group key, and a notice naming them.
    void approveJoinRequest(const ConversationMeta& meta, const std::string& userId);

    /**
     * Someone who just joined through a link: joining adds them with no other member present, so they confirm
     * themselves. The key commitment in the notice shows they really hold the group's key. Never throws: a notice that
     * can't go out never stands in the way of the join, and everyone else's next rekey says someone is unconfirmed.
     */
    void confirmSelfAfterJoin(const std::string& conversationId, const crypto::SecretBytes& groupKey);

    /// The group's key now. Throws with the reason when it was refused: a link can't hand out a key nobody verified.
    crypto::SecretBytes currentKey(const ConversationMeta& meta) const;

    /**
     * Remove a member (owner/admin), or leave with your own id (`selfUserId`).
     * Someone removed may still hold the group key, so it's replaced right
     * away (when leaving, the remaining members' apps do it).
     */
    void remove(const std::string& conversationId, const std::string& userId, const std::string& selfUserId = {});

    void setRole(const std::string& conversationId, const std::string& userId, const std::string& role);

    void rename(const ConversationMeta& meta, const std::string& name);

private:
    std::shared_ptr<api::ApiClient> api_;
    std::shared_ptr<MessageKeys> keys_;
};

}  // namespace redvault::data
