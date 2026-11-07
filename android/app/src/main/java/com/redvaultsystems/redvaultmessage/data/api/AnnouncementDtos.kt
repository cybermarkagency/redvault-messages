package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable

// --- Announcements: messages and polls the team sends from the web admin panel (/api/announcements) ---
// Unlike chats these are NOT end-to-end encrypted: the server reads them to count votes.

@Serializable
data class AnnouncementOptionDto(val id: String, val label: String)

@Serializable
data class AnnouncementDto(
    val id: String,
    /** "message" or "poll". */
    val kind: String,
    val body: String,
    val allowReplies: Boolean = false,
    val createdAt: String,
    val read: Boolean = false,
    val options: List<AnnouncementOptionDto> = emptyList(),
    /** The option this person voted for, if any. */
    val myOptionId: String? = null,
)

@Serializable
data class AnnouncementReplyDto(
    val id: String,
    val body: String,
    val createdAt: String,
)

@Serializable
data class AnnouncementDetailDto(
    val id: String,
    val replies: List<AnnouncementReplyDto> = emptyList(),
)

@Serializable
data class AnnouncementVoteRequest(val optionId: String)

@Serializable
data class AnnouncementReplyRequest(val body: String)
