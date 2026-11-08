// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "redvault/data/friend_repository.hpp"

#include <utility>

#include "redvault/api/group_invite_api.hpp"
#include "redvault/crypto/base64.hpp"
#include "redvault/crypto/e2e_crypto.hpp"
#include "redvault/util/text.hpp"

namespace redvault::data {

using crypto::base64Decode;
using crypto::base64Encode;

namespace {

std::string trimmed(const std::string& text) {
    return util::encodeUtf8(util::trim(util::decodeUtf8(text)));
}

}  // namespace

// --- Friends ---

FriendRepository::FriendRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<ContactKeyPins> contacts)
    : api_(std::move(api)), contacts_(std::move(contacts)) {}

ContactPreview FriendRepository::lookup(const std::string& handle, const std::string& selfUserId) {
    const auto found = api_->lookupUser(trimmed(handle));
    const auto user = api_->getUser(found.userId);
    return ContactPreview{user.userId, user.username, user.avatarUrl, user.userId == selfUserId, user.deleted};
}

void FriendRepository::sendRequest(const std::string& handle) {
    api_->sendFriendRequest({.username = handle});
}

FriendRequests FriendRepository::pending() {
    auto hydrate = [&](const std::vector<api::FriendRequestDto>& dtos, bool incoming) {
        std::vector<FriendRequestView> out;
        for (const auto& dto : dtos) {
            if (dto.status != "pending") continue;
            const std::string& userId = incoming ? dto.requesterId : dto.recipientId;
            try {
                const auto user = api_->getUser(userId);
                out.push_back(FriendRequestView{dto.id, userId, user.username, user.avatarUrl});
            } catch (const api::ApiError&) {
                // The other side can't be resolved any more: skip it.
            }
        }
        return out;
    };
    const auto incoming = api_->incomingFriendRequests();
    const auto outgoing = api_->outgoingFriendRequests();
    return FriendRequests{hydrate(incoming, true), hydrate(outgoing, false)};
}

std::string FriendRepository::accept(const SelfIdentity& identity, const FriendRequestView& request) {
    const auto requester = api_->getUser(request.userId);
    requireUnchangedKey(contacts_.get(), identity.userId, request.userId, requester.publicKey);
    const auto theirPublicKey = base64Decode(requester.publicKey);
    // SecretBytes: wiped when this returns. The chat list unseals its own copy on the next refresh.
    const auto conversationKey = crypto::generateConversationKey();
    const auto created = api_->acceptFriendRequest(
        request.id,
        api::AcceptFriendRequest{{
            {identity.userId, base64Encode(crypto::sealConversationKey(conversationKey, identity.publicKey))},
            {request.userId, base64Encode(crypto::sealConversationKey(conversationKey, theirPublicKey))},
        }});
    return created.id;
}

void FriendRepository::deny(const std::string& requestId) {
    api_->denyFriendRequest(requestId);
}

void FriendRepository::cancel(const std::string& requestId) {
    api_->cancelFriendRequest(requestId);
}

// --- Groups ---

GroupRepository::GroupRepository(std::shared_ptr<api::ApiClient> api, std::shared_ptr<MessageKeys> keys)
    : api_(std::move(api)), keys_(std::move(keys)) {}

std::vector<api::FriendDto> GroupRepository::friends() {
    return api_->listFriends();
}

std::string GroupRepository::create(const SelfIdentity& identity, const std::string& name,
                                    const std::vector<api::FriendDto>& members) {
    const auto key = crypto::generateConversationKey();
    std::vector<api::WrappedKeyForUser> participants;
    participants.push_back({identity.userId, base64Encode(crypto::sealConversationKey(key, identity.publicKey))});
    for (const auto& member : members) {
        if (keys_) keys_->requireUnchangedKey(member.userId, member.publicKey);
        participants.push_back(
            {member.userId, base64Encode(crypto::sealConversationKey(key, base64Decode(member.publicKey)))});
    }
    const auto encryptedName = crypto::encryptWithConversationKey(trimmed(name), key);
    return api_
        ->createConversation(api::CreateConversationRequest{
            .isGroup = true,
            .nameCiphertext = base64Encode(encryptedName.ciphertext),
            .nameNonce = base64Encode(encryptedName.nonce),
            .participants = std::move(participants),
        })
        .id;
}

crypto::SecretBytes GroupRepository::currentKey(const ConversationMeta& meta) const {
    if (keys_) {
        if (auto current = keys_->currentKey(meta.id)) return std::move(current->key);
    }
    return crypto::SecretBytes::copyOf(meta.conversationKey);
}

void GroupRepository::add(const ConversationMeta& meta, const api::FriendDto& friendToAdd) {
    auto current = keys_ ? keys_->currentKey(meta.id) : std::nullopt;
    const crypto::SecretBytes& key = current ? current->key : meta.conversationKey;
    if (keys_) keys_->requireUnchangedKey(friendToAdd.userId, friendToAdd.publicKey);
    const auto wrapped = base64Encode(crypto::sealConversationKey(key, base64Decode(friendToAdd.publicKey)));
    const auto notice = current ? keys_->noticeAdding(meta.id, current->epoch, key, friendToAdd.userId) : std::nullopt;
    api_->addParticipant(meta.id, {friendToAdd.userId, wrapped, current ? std::optional<int>(current->epoch) : std::nullopt, notice});
    // Only now: pinning before the server has it would lock the group if the add failed.
    if (current && notice) keys_->adoptNotice(meta.id, current->epoch, key, *notice);
}

void GroupRepository::approveJoinRequest(const ConversationMeta& meta, const std::string& userId) {
    auto current = keys_ ? keys_->currentKey(meta.id) : std::nullopt;
    const crypto::SecretBytes& key = current ? current->key : meta.conversationKey;
    // Their own copy of the group key, sealed to the key their account publishes (as the web app does).
    const auto user = api_->getUser(userId);
    if (keys_) keys_->requireUnchangedKey(userId, user.publicKey);
    const auto sealed = base64Encode(crypto::sealConversationKey(key, base64Decode(user.publicKey)));
    const auto notice = current ? keys_->noticeAdding(meta.id, current->epoch, key, userId) : std::nullopt;
    api::invites::approve(*api_, meta.id, userId, sealed, current ? std::optional<int>(current->epoch) : std::nullopt, notice);
    // Only now: pinning before the server has it would lock the group if the approval failed.
    if (current && notice) keys_->adoptNotice(meta.id, current->epoch, key, *notice);
}

void GroupRepository::confirmSelfAfterJoin(const std::string& conversationId, const crypto::SecretBytes& groupKey) {
    if (!keys_) return;
    try {
        for (const auto& dto : api_->listConversations()) {
            if (dto.id != util::toLowerAscii(conversationId)) continue;
            // A notice no member who held this key signed can't be built on: nothing here can be vouched for, and the
            // next rekey says this person is unconfirmed.
            std::optional<api::KeyNoticeDto> latest;
            if (dto.keyNotice) {
                latest = keys_->checkedLatestNotice(dto.id, groupKey, *dto.keyNotice);
                if (!latest) return;
            }
            std::vector<std::string> ids;
            for (const auto& participant : api_->listParticipants(dto.id)) ids.push_back(participant.userId);
            keys_->publishNotice(dto.id, dto.keyEpoch, groupKey, keys_->membersJoining(dto.id, ids, latest), latest);
            return;
        }
    } catch (const std::exception&) {
        // Nothing was pinned, so nothing is locked.
    }
}

void GroupRepository::remove(const std::string& conversationId, const std::string& userId, const std::string& selfUserId) {
    api_->removeParticipant(conversationId, userId);
    if (!keys_ || userId == selfUserId) return;
    try {
        keys_->rotate(conversationId);
    } catch (const std::exception&) {
        // Tried again at the next send (the server takes nothing until it's done).
    }
}

void GroupRepository::setRole(const std::string& conversationId, const std::string& userId, const std::string& role) {
    api_->setParticipantRole(conversationId, userId, {.role = role});
}

void GroupRepository::rename(const ConversationMeta& meta, const std::string& name) {
    const auto key = currentKey(meta);
    const auto encrypted = crypto::encryptWithConversationKey(trimmed(name), key);
    api_->renameConversation(meta.id, {base64Encode(encrypted.ciphertext), base64Encode(encrypted.nonce)});
}

}  // namespace redvault::data
