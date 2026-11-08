// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.model

/** Server-enforced disappearing-message timers, matching ui/src/types.ts DURATION_SECONDS. */
enum class Disappearing(val seconds: Int?, val label: String) {
    Off(null, "Off"),
    ThirtySeconds(30, "30 seconds"),
    OneMinute(60, "1 minute"),
    OneHour(3600, "1 hour"),
    Instantly(1, "Instantly");

    companion object {
        /** Whatever TTL the server reports; unknown values show as Off, like the web client. */
        fun fromSeconds(seconds: Int?): Disappearing = entries.firstOrNull { it.seconds == seconds } ?: Off
    }
}

enum class VerificationStatus { Verified, Failed, Unavailable }

/**
 * The three stages of a sent message, as in notification.md: encrypted
 * locally, relayed as ciphertext, and transaction verified (the client's own
 * commitment over the envelope matches the server's).
 */
data class MessageSecurity(
    val verification: VerificationStatus,
    val publicTransactionId: String? = null,
    val protocolVersion: Int? = null,
    val localCommitment: String? = null,
    val networkCommitment: String? = null,
)

enum class MessageKind { Text, File, Undecryptable }

data class ReplyView(val messageId: String, val preview: String, val fromMe: Boolean)

/** [myReactionId]: the reaction message this user sent; deleting it removes the reaction. */
data class ReactionView(val emoji: String, val count: Int, val mine: Boolean, val myReactionId: String?)

/** An earlier text of an edited message, and when it was written. */
data class EditVersion(val text: String, val atMs: Long)

/** A poll with its votes folded in: each person's latest vote counts until its creator ends it. */
data class PollView(
    val question: String,
    val options: List<String>,
    val multiple: Boolean,
    val closed: Boolean = false,
    /** Votes per option, in [options] order. */
    val counts: List<Int> = options.map { 0 },
    /** This user's current choice (option indexes). */
    val mine: List<Int> = emptyList(),
    /** How many people have a vote in. */
    val voters: Int = 0,
)

enum class NoticeKind { Pin, Unpin }

/**
 * A pin or unpin, shown as a small line rather than a bubble; [targetId] is
 * lowercase. [targetGone]: that message was deleted (or hidden here), so it's
 * no longer pinned.
 */
data class NoticeView(val kind: NoticeKind, val targetId: String, val targetGone: Boolean = false)

/** A pinned message: [preview] travels with the pin; [senderId] is who pinned it. */
data class PinnedView(val messageId: String, val preview: String, val byMe: Boolean, val senderId: String)

data class Message(
    val id: String,
    val fromMe: Boolean,
    /** Who sent it (needed to show names in groups). */
    val senderId: String = "",
    val kind: MessageKind,
    /** Message text; for a file, its (sanitized) filename. */
    val text: String,
    /** Set for [MessageKind.File]: the decrypted descriptor, holding the file key. Memory only. */
    val file: FileDescriptor? = null,
    /** The server's lifecycle status for the file's ciphertext ("available", "expired", "deleted", ...). */
    val fileStatus: String? = null,
    /** The message this one replies to; the quote travels encrypted with the reply. */
    val replyTo: ReplyView? = null,
    /** One entry per emoji, in REACTION_EMOJI order. */
    val reactions: List<ReactionView> = emptyList(),
    /** Set once its sender edited it: when the shown [text] was written. */
    val editedAtMs: Long? = null,
    /** Oldest first; the shown [text] is the newest. */
    val earlierVersions: List<EditVersion> = emptyList(),
    /** The edit messages themselves (deleting the message for everyone deletes these too). */
    val editMessageIds: List<String> = emptyList(),
    val forwarded: Boolean = false,
    /** Forwarded from a channel: which post (it counts as a view of it) and the channel to open. */
    val channelPost: ChannelPostRef? = null,
    /** A reply or reaction to a story (yours, if it came to you). */
    val storyReply: StoryReplyRef? = null,
    /** Set for a poll; [text] is then its question. */
    val poll: PollView? = null,
    /** Set for a pin or unpin; [text] is then the pinned message's preview. */
    val notice: NoticeView? = null,
    val createdAtMs: Long,
    val deleteAtMs: Long?,
    /** Own messages only: the other side has a "read" receipt for it. */
    val seen: Boolean = false,
    /** Own messages only: the other side has at least a "delivered" receipt. */
    val delivered: Boolean = false,
    /** Own messages only, once computed (on send, or on request for older ones). */
    val security: MessageSecurity? = null,
    /** Own messages only: the envelope as sent, so verification can be recomputed later. */
    val ciphertextB64: String? = null,
    val nonceB64: String? = null,
    /** A group message without the signature its sender's apps now add: an older app of theirs, or a forgery. */
    val unverifiedSender: Boolean = false,
    /**
     * Own messages waiting in the Tor outbox (data/Outbox.kt): not on the server yet, so never shown
     * as sent or delivered. [id] is then the outbox item's id.
     */
    val queued: Boolean = false,
    /** Why the last attempt failed, safe to show. */
    val queueError: String? = null,
    /** Sent or received with per-device keys (protocol 3, development preview) rather than the usual encryption. */
    val v3: Boolean = false,
    /** The server refused it: it waits for Retry or Discard. */
    val queueHeld: Boolean = false,
    /** Device keys: its sender deleted it for everyone. [text] then only says so; nothing of it is left on this phone. */
    val deleted: Boolean = false,
)

/**
 * The slow-changing half of a DM: who it's with and its unsealed key. Held
 * only in memory, and wiped when the session closes.
 */
/** One member of a group, you included ([isSelf]). */
data class GroupMemberView(
    val userId: String,
    val handle: String,
    val avatarUrl: String?,
    /** "owner", "admin" or "member". */
    val role: String,
    val publicKeyB64: String,
    val isSelf: Boolean,
)

/**
 * For a group: [handle] is the group's (decrypted) name, [otherUserId] and
 * the public-key fields are empty, and [members] lists everyone.
 */
class ConversationMeta(
    val id: String,
    val otherUserId: String,
    val handle: String,
    /** Their profile photo (a same-server path), or null. */
    val avatarUrl: String?,
    val otherPublicKey: ByteArray,
    val otherPublicKeyB64: String,
    val conversationKey: ByteArray,
    val safetyNumber: List<String>,
    val fetchedAtMs: Long,
    val isGroup: Boolean = false,
    val members: List<GroupMemberView> = emptyList(),
    /** Your role in a group: "owner", "admin" or "member". */
    val myRole: String = "member",
    /**
     * Set when this group is a community (paid or free). Its [conversationKey] is then the current epoch's
     * key, messages carry their epoch, and [members] holds only you and the people seen posting: the full
     * list is the community's to show, and a channel's subscribers can't see it.
     */
    val community: CommunityLink? = null,
) {
    /** Everyone but you: the one contact of a DM, or every other group member. */
    fun otherUserIds(selfUserId: String): List<String> =
        if (isGroup) members.filter { !it.isSelf && it.userId != selfUserId }.map { it.userId } else listOf(otherUserId)

    fun handleOf(userId: String): String = members.firstOrNull { it.userId == userId }?.handle ?: "Former member"

    /** The same conversation with [extra] people added to [members] (community posters looked up since). */
    fun withMembers(extra: List<GroupMemberView>): ConversationMeta = ConversationMeta(
        id, otherUserId, handle, avatarUrl, otherPublicKey, otherPublicKeyB64, conversationKey, safetyNumber, fetchedAtMs,
        isGroup, members + extra.filter { e -> members.none { it.userId == e.userId } }, myRole, community,
    )
}

/** A community conversation's link to its community, from the conversation list. */
data class CommunityLink(
    val id: String,
    /** "channel" (the creator and staff post, members read) or "group" (members talk). */
    val kind: String,
    /** "owner", "admin", "moderator" or "member". */
    val role: String,
    val canPost: Boolean,
    val status: String,
    val keyEpoch: Long,
    val rekeyRequired: Boolean,
) {
    val isChannel: Boolean get() = kind == "channel"
    /** Moderators and up handle the community's own reports and members (on the web). */
    val isStaff: Boolean get() = role == "owner" || role == "admin" || role == "moderator"
}

data class Conversation(
    val meta: ConversationMeta,
    val disappearing: Disappearing,
    /** Oldest first. */
    val messages: List<Message>,
    val unread: Int,
    /** This device has confirmed the contact's safety number for their current key. */
    val verified: Boolean,
    /** A 1:1 contact's key differs from the one this phone first saw, and the new one isn't verified yet. */
    val keyChanged: Boolean = false,
    /** There are older messages than [messages] holds ("Load older messages"). */
    val hasOlder: Boolean = false,
    /** This chat's key failed a trust check (data/KeyTrust.kt): why, shown in the chat. Nothing can be sent. */
    val keyProblem: String? = null,
) {
    val id: String get() = meta.id
    val handle: String get() = meta.handle
    val isGroup: Boolean get() = meta.isGroup
    val avatarUrl: String? get() = meta.avatarUrl
    val lastActivityMs: Long get() = messages.lastOrNull()?.createdAtMs ?: 0L
}
