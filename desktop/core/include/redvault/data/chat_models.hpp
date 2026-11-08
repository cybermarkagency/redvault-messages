// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redvault/crypto/bytes.hpp"
#include "redvault/model/file_message.hpp"
#include "redvault/model/rich_message.hpp"

// Port of android/.../data/model/ChatModels.kt: the decrypted, display-ready
// view of chats and messages. Everything here lives in memory only.

namespace redvault::data {

/// Server-enforced disappearing-message timers, matching ui/src/types.ts DURATION_SECONDS.
struct Disappearing {
    std::optional<int> seconds;
    std::string_view label;

    bool operator==(const Disappearing& other) const { return seconds == other.seconds; }

    static const std::array<Disappearing, 5>& all();
    /// Whatever TTL the server reports; unknown values show as Off, like the web client.
    static Disappearing fromSeconds(std::optional<int> seconds);
    static Disappearing off() { return all()[0]; }
};

enum class VerificationStatus { Verified, Failed, Unavailable };

/**
 * The three stages of a sent message, as in notification.md: encrypted
 * locally, relayed as ciphertext, and transaction verified (the client's own
 * commitment over the envelope matches the server's).
 */
struct MessageSecurity {
    VerificationStatus verification = VerificationStatus::Unavailable;
    std::optional<std::string> publicTransactionId;
    std::optional<int> protocolVersion;
    std::optional<std::string> localCommitment;
    std::optional<std::string> networkCommitment;
};

/// Notice: something that happened in the chat (a message pinned or unpinned), shown as a small line.
enum class MessageKind { Text, File, Undecryptable, Notice, Poll };

/// A poll and its votes so far (the latest vote of each person counts).
struct PollView {
    std::string question;
    std::vector<std::string> options;
    bool multiple = false;
    bool closed = false;
    /// Votes per option, and the options you picked.
    std::vector<int> counts;
    std::vector<int> mine;
    int voters = 0;
};

/// A pinned message, newest pin last. `preview` travels with the pin, so it shows even if the message isn't loaded.
struct PinnedView {
    std::string messageId;
    std::string preview;
    std::string pinnedBy;
    std::int64_t atMs = 0;
};

struct ReplyView {
    std::string messageId;
    std::string preview;
    bool fromMe = false;
};

/// An earlier text of an edited message.
struct EditVersion {
    std::string text;
    std::int64_t atMs = 0;
};

/// `myReactionId`: the reaction message this user sent; deleting it removes the reaction.
struct ReactionView {
    std::string emoji;
    int count = 0;
    bool mine = false;
    std::optional<std::string> myReactionId;
};

struct Message {
    std::string id;
    bool fromMe = false;
    /// Who sent it (needed to show names in groups).
    std::string senderId;
    MessageKind kind = MessageKind::Text;
    /// Message text; for a file, its (sanitized) filename.
    std::string text;
    /// Set for MessageKind::File: the decrypted descriptor, holding the file key. Memory only.
    std::optional<model::FileDescriptor> file;
    /// The server's lifecycle status for the file's ciphertext ("available", "expired", "deleted", ...).
    std::optional<std::string> fileStatus;
    /// The message this one replies to; the quote travels encrypted with the reply.
    std::optional<ReplyView> replyTo;
    /// One entry per emoji: the quick six first, in kReactionEmoji order, then others in the order first used.
    std::vector<ReactionView> reactions;
    /// Set once its sender edited it: when the latest version was sent, and the earlier ones (oldest first).
    std::optional<std::int64_t> editedAtMs;
    std::vector<EditVersion> earlierVersions;
    /// The edit messages carrying its later versions (deleted along with it).
    std::vector<std::string> editMessageIds;
    /// Forwarded from another chat.
    bool forwarded = false;
    /// A group message without the signature its sender's apps now add: an older app of theirs, or a forgery.
    bool unverifiedSender = false;
    /// Forwarded from a channel: which post, shown as "Forwarded from <channel>" (each copy shown counts a view).
    std::optional<model::ChannelPostRef> channelPost;
    /// A reply or reaction to a story (its id and a short label, never its key).
    std::optional<model::StoryReplyRef> storyReply;
    /// MessageKind::Poll: the poll and its tally.
    std::optional<PollView> poll;
    /// MessageKind::Notice: "pin" or "unpin" of `noticeTarget` (`text` holds the pinned message's preview).
    std::string notice;
    std::string noticeTarget;
    std::int64_t createdAtMs = 0;
    std::optional<std::int64_t> deleteAtMs;
    /// Own messages only: the other side has a "read" receipt for it.
    bool seen = false;
    /// Own messages only: the other side has at least a "delivered" receipt.
    bool delivered = false;
    /// Own messages only: still in the Tor outbox, not on the server yet (never shown as sent).
    bool queued = false;
    /// Why the outbox couldn't send it yet (the last attempt), and whether it waits for the person.
    std::string queueError;
    bool queueHeld = false;
    /// Own messages only, once computed (on send, or on request for older ones).
    std::optional<MessageSecurity> security;
    /// Own messages only: the envelope as sent, so verification can be recomputed later.
    std::optional<std::string> ciphertextB64;
    std::optional<std::string> nonceB64;
};

/// One member of a group, you included (`isSelf`).
struct GroupMemberView {
    std::string userId;
    std::string handle;
    std::optional<std::string> avatarUrl;
    /// "owner", "admin" or "member".
    std::string role;
    std::string publicKeyB64;
    bool isSelf = false;
};

/**
 * A paid or free community riding on a group chat (docs/paid-communities.md).
 * Its messages are encrypted under epoch keys (data/community_keys.hpp), and
 * membership is managed on the community's page, not through group endpoints.
 */
struct CommunityLink {
    std::string id;
    std::string name;
    /// "channel" (the creator and staff post, members read) or "group" (members talk).
    std::string kind;
    /// "owner", "admin", "moderator" or "member".
    std::string role;
    /// A channel's subscribers read only.
    bool canPost = false;
    std::string status;
    bool hasAvatar = false;
};

/**
 * The slow-changing half of a chat: who it's with and its unsealed key.
 * Held only in memory; the key is wiped when this is destroyed.
 *
 * For a group: `handle` is the group's (decrypted) name, `otherUserId` and
 * the public-key fields are empty, and `members` lists everyone.
 */
struct ConversationMeta {
    std::string id;
    std::string otherUserId;
    std::string handle;
    /// Their profile photo (a same-server path), if any.
    std::optional<std::string> avatarUrl;
    crypto::Bytes otherPublicKey;
    std::string otherPublicKeyB64;
    crypto::SecretBytes conversationKey;
    std::vector<std::string> safetyNumber;
    std::int64_t fetchedAtMs = 0;
    bool isGroup = false;
    std::vector<GroupMemberView> members;
    /// Your role in a group: "owner", "admin" or "member".
    std::string myRole = "member";
    /**
     * Set for a community. Then `conversationKey` is a copy of the current
     * epoch's key (messages carry their epoch and are read with the ring),
     * and `members` lists only you and whoever wrote the messages loaded: a
     * channel's subscribers can't see who else is in it.
     */
    std::optional<CommunityLink> community;

    /// Everyone but you: the one contact of a DM, or every other group member.
    std::vector<std::string> otherUserIds(const std::string& selfUserId) const;

    std::string handleOf(const std::string& userId) const;
};

}  // namespace redvault::data
