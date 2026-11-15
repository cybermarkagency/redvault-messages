// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "redvault/api/api_client.hpp"
#include "redvault/crypto/bytes.hpp"
#include "redvault/e2ev3/engine.hpp"

// Part C2 (docs/signal-integration/11-part-c2-communities.md): paid and free
// communities as full Signal groups on the device-key path. Messages go device
// to device under libsignal sender keys; the server keeps no history and only
// says who is entitled (and refuses, never filters).
//
//   - `group` community: the signed roster lists every entitled member, and only
//     owner/staff devices issue it. Every member device that sends shares its
//     sender key with the roster's devices (as in M6).
//   - `channel` community: the roster lists the owner and staff only. A staff
//     device shares its sender key with the server's recipient list
//     (GET /communities/:id/recipients) through groupSignalShare's
//     "audience": "recipients". Whenever an account leaves that list, the staff
//     roster is issued again at the next epoch with the same members, which gives
//     every sender a new sender key (a new distribution id) the leaver never gets.
//     Members never see the member list; their reactions, votes and receipts go
//     pairwise to the staff's devices.
//
// Every request goes in batches of at most 100 devices, in order; a batch that
// fails for a moment is tried again. A 409 recipient_not_entitled drops the
// accounts it names and tries once more. Owner/staff devices run the membership
// tasks (admit waiting members, issue rosters, rotate) by themselves.
// Qt-free. Nothing here logs content or keys.

namespace redvault::e2ev3 {

class V3Device;
class ChatHistory;

/// The most devices one message request names (the server's limit, both routes).
inline constexpr std::size_t kMaxDevicesPerRequest = 100;
/// How many times one batch is tried while the server or the connection is briefly away.
inline constexpr int kBatchAttempts = 3;
/// What a community chat shows at the top: the server keeps no history, so nobody sees what came before they (or
/// this device) joined.
inline constexpr const char* kBeforeYouJoined = "Messages from before you joined aren't available";

/// What this app knows about one community conversation (from the chat list or the membership tasks).
struct CommunityInfo {
    std::string communityId;
    std::string conversationId;
    std::string kind = "group";  // "group" | "channel"
    bool staff = false;  // this account is the owner, an admin or a moderator
    bool canPost = true;
    bool channel() const { return kind == "channel"; }
};

/// The community routes the device-key path needs (server/src/routes/communityRoutes.ts). An interface for the tests.
class CommunityDirectory {
public:
    virtual ~CommunityDirectory() = default;
    /// GET /communities/membership-tasks: [{communityId, conversationId, kind, latestSeq, waiting}].
    virtual Json membershipTasks() = 0;
    /// GET /communities/:id/membership-tasks?after=&limit=.
    virtual Json membershipTasksFor(const std::string& communityId, std::int64_t after, int limit) = 0;
    /// POST /communities/:id/members/:userId/admit.
    virtual Json admit(const std::string& communityId, const std::string& userId) = 0;
    /// GET /communities/:id/recipients: {userIds} (owner and staff only).
    virtual Json recipients(const std::string& communityId) = 0;
    /// The active owner, admins and moderators, sorted.
    virtual std::vector<std::string> staff(const std::string& communityId) = 0;
};

/// The real routes, over the app's ApiClient (itself over the Tor transport).
class HttpCommunityDirectory final : public CommunityDirectory {
public:
    explicit HttpCommunityDirectory(std::shared_ptr<api::ApiClient> api) : api_(std::move(api)) {}
    Json membershipTasks() override;
    Json membershipTasksFor(const std::string& communityId, std::int64_t after, int limit) override;
    Json admit(const std::string& communityId, const std::string& userId) override;
    Json recipients(const std::string& communityId) override;
    std::vector<std::string> staff(const std::string& communityId) override;

private:
    std::shared_ptr<api::ApiClient> api_;
};

/**
 * The communities this device sends in, and the little it keeps about them: each community's membership-task
 * cursor, and the accounts a staff device last shared its channel sender key with (so a leaver it wasn't told
 * about still triggers a rotation). Kept in one file sealed with XChaCha20-Poly1305 under a key derived from the
 * device's storage key, or in memory only without a file. Thread-safe.
 */
class Communities {
public:
    explicit Communities(std::shared_ptr<CommunityDirectory> directory, std::filesystem::path stateFile = {}, crypto::Bytes key = {});

    CommunityDirectory& directory() const { return *directory_; }
    /// Keeps the cursors and audiences in `stateFile` from now on (the device's own file), reading what it holds.
    void attachFile(std::filesystem::path stateFile, crypto::Bytes key);
    /// Replaces what's known from the chat list (cursors and audiences stay).
    void setAll(const std::vector<CommunityInfo>& communities);
    void set(const CommunityInfo& community);
    std::optional<CommunityInfo> find(const std::string& conversationId) const;
    std::optional<CommunityInfo> byCommunity(const std::string& communityId) const;
    std::vector<CommunityInfo> all() const;

    std::int64_t cursor(const std::string& communityId) const;
    void setCursor(const std::string& communityId, std::int64_t seq);
    /// The accounts this device's channel sender key went to last (sorted), by conversation.
    std::vector<std::string> audience(const std::string& conversationId) const;
    void setAudience(const std::string& conversationId, std::vector<std::string> accounts);

private:
    void load();
    void save() const;
    std::shared_ptr<CommunityDirectory> directory_;
    std::filesystem::path file_;
    crypto::Bytes key_;
    mutable std::mutex mutex_;
    std::map<std::string, CommunityInfo> byConversation_;
    std::map<std::string, std::int64_t> cursors_;
    std::map<std::string, std::vector<std::string>> audiences_;
};

/// The community a conversation belongs to, if `device`'s backend knows it as one.
std::optional<CommunityInfo> communityOf(const V3Device& device, const std::string& conversationId);

/**
 * One send into a community (chat.cpp calls this for every group send in a community conversation): `isOp` for a
 * reaction, edit, delete, vote, end or pin. Returns how many devices it went to. Throws EngineError "cannot_post"
 * (a channel member's message), "not_in_roster" (a member the owner's or staff's app hasn't added yet) or
 * "community_not_ready" (no roster yet), and the server's ApiError ("membership_expired", "cannot_post", ...).
 */
int sendToCommunity(V3Device& device, const CommunityInfo& community, const std::string& plaintext, bool isOp);

/**
 * Owner/staff: makes the community's signed roster `members` (the issuer is always in it), signing the first or the
 * next epoch. With `rotate`, a new epoch even when the members are the same (a channel's sender-key rotation).
 * Accounts the server refuses (roster_member_not_entitled / roster_member_not_staff) are dropped and it tries again;
 * another staff device's roster that came first is verified and built on. Returns the new epoch, or 0 when the
 * roster already said exactly this. Nothing is kept on this device until the server took the roster.
 */
std::int64_t issueCommunityRoster(V3Device& device, const CommunityInfo& community, std::vector<std::string> members, bool rotate = false);

/// Staff of a channel: shares this device's current sender key with every recipient device that lacks it (rotating
/// first when an account it last shared with has left). Returns how many devices got it now.
int shareChannelKey(V3Device& device, const CommunityInfo& community);

/// What one round of membership tasks did (counts only, never who).
struct MembershipRound {
    int communities = 0;  // communities looked at
    int admitted = 0;
    int rosters = 0;  // rosters issued (rotations included)
    int rotations = 0;  // channel sender-key rotations
    int shared = 0;  // devices a channel sender key went to
    std::vector<std::string> errors;  // "<communityId>: <code>" per community that failed (tried again next round)
};

/**
 * The owner/staff upkeep (GET /communities/membership-tasks, then each community whose cursor moved or that has
 * members waiting): admits waiting members, then
 *   - `group`: issues the next roster (the entitled accounts, minus lapsed ones);
 *   - `channel`: issues the staff roster when it changed, rotates on any leave or lapse, and shares the sender key
 *     with new members' devices.
 * Safe to run from several staff devices at once. Does nothing without communities on the backend.
 */
MembershipRound runMembershipTasks(V3Device& device);

/// Remembers when this device first opened a community chat; the chat then shows kBeforeYouJoined at the top.
void noteCommunityJoined(ChatHistory& history, const std::string& conversationId, std::int64_t nowMs = 0);
/// The notice for the top of a community chat ("" when it isn't one this device noted).
std::string communityNotice(const ChatHistory& history, const std::string& conversationId);

}  // namespace redvault::e2ev3
