#pragma once

#include <optional>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/group_invite_crypto.hpp"

// Group invite links (server/src/routes/groupInviteRoutes.ts, mounted at
// /api/group-invites), mirroring ui/src/lib/api.ts. The link's secret never
// reaches the server: it only gets what the secret encrypted, and an auth
// token derived from it (crypto/group_invite_crypto.hpp). Free functions
// over ApiClient::call; all of them block.

namespace redvault::api::invites {

/// The group's link, as its members see it: enough to rebuild and share it.
struct Link {
    std::string id;
    /// The link's secret, encrypted with the group key.
    std::string secretCiphertext;
    std::string secretNonce;
    /// An admin approves each person who asks to join.
    bool approval = false;
    std::string createdAt;
};

/// What someone holding the link sees before joining.
struct Preview {
    std::string conversationId;
    std::string previewCiphertext;
    std::string previewNonce;
    int memberCount = 0;
    bool approval = false;
    /// The group key under the link's key; absent when an admin approves joins.
    std::optional<std::string> wrappedKey;
    std::optional<std::string> wrappedKeyNonce;
    /// Already in the group.
    bool member = false;
    /// Already asked to join, waiting for an admin.
    bool requested = false;
};

struct JoinRequest {
    std::string userId;
    std::string createdAt;
};

struct JoinResult {
    /// "joined", "member" or "requested".
    std::string status;
    std::string conversationId;
};

/// Any member: the group's link, or nullopt when it has none.
std::optional<Link> getLink(ApiClient& api, const std::string& conversationId);
/// Owner or admin: turns the link on, or resets it (a new id and secret, so the old link stops working).
Link setLink(ApiClient& api, const std::string& conversationId, const crypto::invites::LinkInput& input);
/// Owner or admin: turning approval off needs the group key wrapped under the link's key; turning it on drops that copy.
Link setApproval(ApiClient& api, const std::string& conversationId, bool approval,
                 const std::optional<crypto::invites::WrappedKey>& wrapped = std::nullopt);
/// Owner or admin: turns the link off. Pending requests go too.
void deleteLink(ApiClient& api, const std::string& conversationId);
/// Owner or admin: who's waiting to join, oldest first.
std::vector<JoinRequest> listRequests(ApiClient& api, const std::string& conversationId);
/// Owner or admin: lets them in, with the copy of the group key sealed to them (base64), which epoch it is, and the notice naming them.
void approve(ApiClient& api, const std::string& conversationId, const std::string& userId, const std::string& wrappedKey,
             const std::optional<int>& keyEpoch = std::nullopt, const std::optional<KeyNoticeDto>& keyNotice = std::nullopt);
/// An admin turning a request down, or the person who asked taking it back.
void deny(ApiClient& api, const std::string& conversationId, const std::string& userId);
/// Someone holding the link. 404 "invite_invalid" for an unknown link or a wrong secret alike.
Preview open(ApiClient& api, const std::string& linkId, const std::string& auth);
/// Joins with your own sealed copy of the group key (base64), or asks to (nullopt) when an admin approves joins.
JoinResult join(ApiClient& api, const std::string& linkId, const std::string& auth, const std::optional<std::string>& wrappedKey);

}  // namespace redvault::api::invites
