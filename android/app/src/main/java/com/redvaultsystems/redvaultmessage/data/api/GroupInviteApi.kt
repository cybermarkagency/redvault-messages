// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.PATCH
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path

/**
 * Group invite links (server/src/routes/groupInviteRoutes.ts, under /api/group-invites).
 * The link's secret never reaches the server: it only gets what the secret
 * encrypted, and an auth token derived from it (crypto/GroupInviteCrypto.kt).
 */
interface GroupInviteApi {
    /** Any member: the group's link, or `{"link": null}`. */
    @GET("group-invites/groups/{conversationId}/link")
    suspend fun getLink(@Path("conversationId") conversationId: String): GroupInviteLinkResult

    /** Owner/admin: turns the link on, or resets it (a new id and secret, so the old link stops working). */
    @PUT("group-invites/groups/{conversationId}/link")
    suspend fun setLink(@Path("conversationId") conversationId: String, @Body body: GroupInviteLinkInput): GroupInviteLinkResult

    /** Owner/admin: whether an admin approves each join. Turning approval off sends the group key wrapped for the link. */
    @PATCH("group-invites/groups/{conversationId}/link")
    suspend fun setApproval(@Path("conversationId") conversationId: String, @Body body: GroupInviteApprovalRequest): GroupInviteLinkResult

    /** Owner/admin: turns the link off; pending requests go too. */
    @DELETE("group-invites/groups/{conversationId}/link")
    suspend fun deleteLink(@Path("conversationId") conversationId: String)

    @GET("group-invites/groups/{conversationId}/requests")
    suspend fun listRequests(@Path("conversationId") conversationId: String): List<GroupJoinRequestDto>

    /** Owner/admin: lets them in with a copy of the group key sealed to them. */
    @POST("group-invites/groups/{conversationId}/requests/{userId}/approve")
    suspend fun approve(@Path("conversationId") conversationId: String, @Path("userId") userId: String, @Body body: ApproveJoinRequest)

    /** An admin turning a request down, or the person who asked taking it back. */
    @DELETE("group-invites/groups/{conversationId}/requests/{userId}")
    suspend fun deny(@Path("conversationId") conversationId: String, @Path("userId") userId: String)

    @POST("group-invites/links/{linkId}/open")
    suspend fun open(@Path("linkId") linkId: String, @Body body: InviteAuthRequest): GroupInvitePreviewDto

    @POST("group-invites/links/{linkId}/join")
    suspend fun join(@Path("linkId") linkId: String, @Body body: JoinGroupInviteRequest): JoinGroupInviteResult
}

@Serializable
data class GroupInviteLinkDto(
    val id: String,
    /** The link's secret, encrypted with the group key. */
    val secretCiphertext: String,
    val secretNonce: String,
    /** An admin approves each person who asks to join. */
    val approval: Boolean,
    val createdAt: String = "",
)

@Serializable
data class GroupInviteLinkResult(val link: GroupInviteLinkDto? = null)

/** Null wrapped fields are left out (ApiClient.json has explicitNulls off); the server reads that as null. */
@Serializable
data class GroupInviteLinkInput(
    val auth: String,
    val secretCiphertext: String,
    val secretNonce: String,
    val previewCiphertext: String,
    val previewNonce: String,
    val wrappedKey: String? = null,
    val wrappedKeyNonce: String? = null,
)

@Serializable
data class GroupInviteApprovalRequest(val approval: Boolean, val wrappedKey: String? = null, val wrappedKeyNonce: String? = null)

@Serializable
data class GroupJoinRequestDto(val userId: String, val createdAt: String = "")

@Serializable
data class ApproveJoinRequest(val wrappedKey: String, val keyEpoch: Int? = null, val keyNotice: KeyNoticeDto? = null)

@Serializable
data class InviteAuthRequest(val auth: String)

@Serializable
data class GroupInvitePreviewDto(
    val conversationId: String,
    val previewCiphertext: String,
    val previewNonce: String,
    val memberCount: Int = 0,
    val approval: Boolean,
    /** The group key under the link's key; null when an admin approves joins. */
    val wrappedKey: String? = null,
    val wrappedKeyNonce: String? = null,
    /** Already in the group. */
    val member: Boolean = false,
    /** Already asked to join, waiting for an admin. */
    val requested: Boolean = false,
)

/** [wrappedKey]: the group key sealed to your own public key; left out when asking to join. */
@Serializable
data class JoinGroupInviteRequest(val auth: String, val wrappedKey: String? = null)

/** [status] is "joined", "member" or "requested". */
@Serializable
data class JoinGroupInviteResult(val status: String, val conversationId: String)
