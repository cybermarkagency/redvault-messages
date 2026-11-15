// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Path
import retrofit2.http.Query

/**
 * Paid and free communities (server/src/routes/communityRoutes.ts, docs/paid-communities.md): the subset
 * of ui/src/lib/communityApi.ts a member's phone needs. Every amount comes from the server in integer minor
 * units; this app never decides a price or whether a payment succeeded.
 *
 * Deliberately absent: community checkout and the payment-method portal. Should this app go on Google Play,
 * its payments policy doesn't allow selling a digital subscription in the app except through Play Billing,
 * which isn't built (the server refuses a checkout from "android" anyway). CommunityApiSurfaceTest keeps it so.
 */
interface CommunityApi {
    @GET("communities/config")
    suspend fun config(): CommunitiesConfigDto

    @GET("communities/discover")
    suspend fun discover(@Query("q") q: String?, @Query("category") category: String?, @Query("offset") offset: Int?): List<CommunityListingDto>

    @GET("communities/mine")
    suspend fun mine(): List<MyCommunityDto>

    @GET("communities/{id}")
    suspend fun community(@Path("id") id: String, @Query("invite") invite: String?): CommunityPageDto

    /** Free communities (and rejoining a paid one whose period is still running). Body: `{}` or `{"inviteToken": …}`. */
    @POST("communities/{id}/join")
    suspend fun join(@Path("id") id: String, @Body body: JsonObject): JoinResultDto

    @POST("communities/{id}/leave")
    suspend fun leave(@Path("id") id: String)

    @POST("communities/{id}/reports")
    suspend fun report(@Path("id") id: String, @Body body: CommunityReportRequest): CommunityReportResultDto

    // Part C2 (docs/signal-integration/11-part-c2-communities.md): communities as Signal groups. Owner and staff
    // devices poll the membership tasks, admit waiting members and keep the roster and sender keys current.

    @GET("communities/membership-tasks")
    suspend fun membershipTaskSummaries(): List<MembershipTaskSummaryDto>

    @GET("communities/{id}/membership-tasks")
    suspend fun membershipTasks(@Path("id") id: String, @Query("after") after: Long, @Query("limit") limit: Int?): MembershipTasksDto

    @POST("communities/{id}/members/{userId}/admit")
    suspend fun admit(@Path("id") id: String, @Path("userId") userId: String): JoinResultDto

    /** Owner and staff: the entitled members' accounts (a channel's recipient list). */
    @GET("communities/{id}/recipients")
    suspend fun recipients(@Path("id") id: String): CommunityRecipientsDto

    /** Moderators and up: the members, a page at a time (at most 100), with their roles. */
    @GET("communities/{id}/members")
    suspend fun members(@Path("id") id: String, @Query("status") status: String?, @Query("limit") limit: Int?, @Query("offset") offset: Int?): List<CommunityMemberRowDto>

    // Retired key epochs (before Part C2). Only read, and only to show older protocol-1 messages a member was
    // given keys for: nothing calls key-tasks, enrollments, roster or rotate any more.

    @GET("communities/{id}/keys")
    suspend fun keys(@Path("id") id: String): CommunityKeysDto

    // The member's own subscriptions (bought on the web or desktop)

    @GET("community-subscriptions")
    suspend fun memberships(): List<MembershipViewDto>

    /** Stops renewal; access lasts until the paid period ends. */
    @POST("community-subscriptions/{id}/cancel")
    suspend fun cancelMembership(@Path("id") subscriptionId: String): MembershipViewDto

    /** Undoes a cancellation before the period ends; no new purchase. */
    @POST("community-subscriptions/{id}/resume")
    suspend fun resumeMembership(@Path("id") subscriptionId: String): MembershipViewDto
}

@Serializable
data class CommunitiesConfigDto(
    val enabled: Boolean = false,
    val categories: List<String> = emptyList(),
    val paymentsEnabled: Boolean = false,
    val purchasePlatforms: List<String> = emptyList(),
    val currency: String = "usd",
)

@Serializable
data class PriceDto(val amountMinor: Long, val currency: String, val interval: String = "month")

@Serializable
data class CreatorPublicDto(
    val id: String,
    val handle: String,
    val displayName: String = "",
    val verified: Boolean = false,
)

@Serializable
data class MembershipDto(
    /** pending_approval, pending_enrollment, active, removed, banned, left, expired */
    val status: String,
    /** owner, admin, moderator, member */
    val role: String,
    val canPost: Boolean = false,
    val source: String = "",
    val enrolledAt: String? = null,
)

@Serializable
data class SubscriptionDto(
    val id: String,
    val communityId: String,
    val billingStatus: String,
    /** pending, active, trialing, grace_period, cancel_scheduled, expired, canceled, refunded, disputed, suspended */
    val entitlementStatus: String,
    val cancelAtPeriodEnd: Boolean = false,
    val accessUntil: String? = null,
    val graceUntil: String? = null,
    val renewsOn: String? = null,
    val createdAt: String,
    val endedAt: String? = null,
)

/** One community in Discover. */
@Serializable
data class CommunityListingDto(
    val id: String,
    val code: String,
    /** channel (the creator posts, members read) or group (members talk) */
    val kind: String,
    val name: String,
    val description: String = "",
    val category: String = "",
    val previewText: String = "",
    val hasAvatar: Boolean = false,
    /** free or paid */
    val accessType: String,
    val status: String,
    val creator: CreatorPublicDto,
    val price: PriceDto? = null,
    val memberCount: Int? = null,
)

@Serializable
data class ViewerDto(val membership: MembershipDto? = null, val isStaff: Boolean = false, val subscription: SubscriptionDto? = null)

/** A community's own page: its listing plus where this viewer stands. */
@Serializable
data class CommunityPageDto(
    val id: String,
    val code: String,
    val kind: String,
    val name: String,
    val description: String = "",
    val category: String = "",
    val rules: String = "",
    val previewText: String = "",
    val hasAvatar: Boolean = false,
    val accessType: String,
    val status: String,
    val newMembersPaused: Boolean = false,
    val creator: CreatorPublicDto,
    val price: PriceDto? = null,
    val memberCount: Int? = null,
    /** Only for an active member. */
    val conversationId: String? = null,
    val viewer: ViewerDto = ViewerDto(),
)

@Serializable
data class MyCommunityDto(
    val id: String,
    val code: String,
    val kind: String,
    val name: String,
    val hasAvatar: Boolean = false,
    val status: String,
    val membership: MembershipDto,
    val conversationId: String? = null,
)

@Serializable
data class MembershipCommunityDto(val id: String, val name: String, val kind: String, val status: String, val code: String, val hasAvatar: Boolean = false)

/** A subscription with its community, as GET /community-subscriptions lists it. */
@Serializable
data class MembershipViewDto(
    val id: String,
    val communityId: String,
    val billingStatus: String,
    val entitlementStatus: String,
    val cancelAtPeriodEnd: Boolean = false,
    val accessUntil: String? = null,
    val graceUntil: String? = null,
    val renewsOn: String? = null,
    val provider: String = "",
    val createdAt: String,
    val endedAt: String? = null,
    val community: MembershipCommunityDto,
    val creatorHandle: String? = null,
    val price: PriceDto? = null,
)

@Serializable
data class JoinResultDto(val membership: MembershipDto)

/**
 * [evidenceMessageId] / [evidencePlaintext]: only when the reporter chose to share that message's text
 * ([disclosureAcknowledged] true); the server stores the hash of the ciphertext it holds next to it.
 */
@Serializable
data class CommunityReportRequest(
    /** community, user or content */
    val targetType: String,
    val targetUserId: String? = null,
    val category: String,
    val details: String,
    val evidenceMessageId: String? = null,
    val evidencePlaintext: String? = null,
    val disclosureAcknowledged: Boolean = false,
)

@Serializable
data class CommunityReportResultDto(val id: String, val platformReview: Boolean = false)

@Serializable
data class SealedKeyDto(val epoch: Long, val wrappedKey: String)

@Serializable
data class CommunityKeysDto(val conversationId: String? = null, val keyEpoch: Long, val rekeyRequired: Boolean, val keys: List<SealedKeyDto>)

/** One community where this account is active staff (Part C2): compare [latestSeq] with the saved cursor. */
@Serializable
data class MembershipTaskSummaryDto(val communityId: String, val conversationId: String, val kind: String, val latestSeq: Long = 0, val waiting: Int = 0)

@Serializable
data class MembershipEventDto(val seq: Long, val kind: String, val userId: String, val at: String? = null)

@Serializable
data class MembershipTasksDto(
    val communityId: String,
    val conversationId: String,
    val kind: String,
    val cursor: Long,
    val hasMore: Boolean = false,
    val events: List<MembershipEventDto> = emptyList(),
    /** Entitled members waiting to be admitted. */
    val waiting: List<String> = emptyList(),
    /** Enrolled members whose entitlement lapsed, not removed yet. */
    val lapsed: List<String> = emptyList(),
)

@Serializable
data class CommunityRecipientsDto(val communityId: String, val conversationId: String, val userIds: List<String>)

@Serializable
data class CommunityMemberRowDto(val userId: String, val status: String? = null, val role: String = "member", val canPost: Boolean = true)

/**
 * The Part C2 community routes for the device-key path (devicekeys/CommunityChat.kt), over the app's Tor-routed
 * client. Errors come back as [ApiException] with the server's code and details.
 */
class RetrofitCommunityDeviceApi(private val api: CommunityApi) : com.redvaultsystems.redvaultmessage.devicekeys.CommunityDeviceApi {
    private val json = ApiClient.json

    override suspend fun recipients(communityId: String): List<String> = apiCall(json) { api.recipients(communityId) }.userIds

    override suspend fun staff(communityId: String): List<String> {
        val out = ArrayList<String>()
        var offset = 0
        while (offset <= MAX_OFFSET) {
            val page = apiCall(json) { api.members(communityId, "active", PAGE, offset) }
            out += page.filter { it.role != "member" }.map { it.userId }
            if (page.size < PAGE) break
            offset += PAGE
        }
        return out
    }

    override suspend fun membershipTaskSummaries() = apiCall(json) { api.membershipTaskSummaries() }.map {
        com.redvaultsystems.redvaultmessage.devicekeys.MembershipTaskSummary(it.communityId, it.conversationId, it.kind, it.latestSeq, it.waiting)
    }

    override suspend fun membershipTasks(communityId: String, after: Long, limit: Int): com.redvaultsystems.redvaultmessage.devicekeys.MembershipTasks {
        val t = apiCall(json) { api.membershipTasks(communityId, after, limit) }
        return com.redvaultsystems.redvaultmessage.devicekeys.MembershipTasks(
            t.communityId, t.conversationId, t.kind, t.cursor, t.hasMore,
            t.events.map { com.redvaultsystems.redvaultmessage.devicekeys.MembershipEvent(it.seq, it.kind, it.userId) },
            t.waiting, t.lapsed,
        )
    }

    override suspend fun admit(communityId: String, userId: String) {
        apiCall(json) { api.admit(communityId, userId) }
    }

    private companion object {
        const val PAGE = 100
        const val MAX_OFFSET = 10_000
    }
}
