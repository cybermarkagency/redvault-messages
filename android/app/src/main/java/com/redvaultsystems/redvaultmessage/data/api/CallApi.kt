// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Path

/**
 * One-to-one calls and call links (server/src/routes/callRoutes.ts and
 * liveRoutes.ts). The server only relays sealed offers and answers and
 * hands out short-lived TURN credentials; it never sees the media or can
 * read the setup. Events for calls arrive on the live channel (LiveApi).
 */
interface CallApi {
    @POST("calls")
    suspend fun start(@Body body: StartCallRequest): StartedCallDto

    @POST("calls/{id}/offer")
    suspend fun offer(@Path("id") callId: String, @Body offer: SealedSignalDto)

    @POST("calls/{id}/answer")
    suspend fun answer(@Path("id") callId: String, @Body answer: SealedSignalDto)

    /** Any later sealed message between the two: here, an ICE restart with fresh TURN credentials (TurnRenewal). */
    @POST("calls/{id}/signal")
    suspend fun signal(@Path("id") callId: String, @Body signal: SealedSignalDto)

    /** Fresh TURN credentials for a call this account is in, before the first ones expire. */
    @POST("calls/{id}/relay-credentials")
    suspend fun renewRelay(@Path("id") callId: String): RenewedRelayDto

    @POST("calls/{id}/decline")
    suspend fun decline(@Path("id") callId: String)

    @POST("calls/{id}/hangup")
    suspend fun hangUp(@Path("id") callId: String)

    /** Screen sharing is Premium: the server checks, then tells the other side. */
    @POST("calls/{id}/screen")
    suspend fun screen(@Path("id") callId: String, @Body body: ScreenRequest)

    /** TURN credentials, for Settings' relay check. */
    @GET("calls/relays")
    suspend fun relays(): RelaysDto

    // --- Call links ---

    @GET("call-links")
    suspend fun links(): List<CallLinkDto>

    @POST("call-links")
    suspend fun createLink(@Body body: CreateCallLinkRequest): CallLinkDto

    @DELETE("call-links/{id}")
    suspend fun deleteLink(@Path("id") id: String)

    @GET("call-links/{id}")
    suspend fun linkInfo(@Path("id") id: String): CallLinkInfoDto

    @POST("call-links/{id}/join")
    suspend fun joinLink(@Path("id") id: String): StartedCallDto

    // --- Group calls (server/src/routes/callRoutes.ts, createGroupCallRouter) ---

    /** Starts a call in a group (Premium), or joins the one already going on there. */
    @POST("group-calls")
    suspend fun startGroupCall(@Body body: StartGroupCallRequest): GroupCallJoinDto

    /** Calls going on in your groups, for the Join button. */
    @GET("group-calls/active")
    suspend fun activeGroupCalls(): ActiveGroupCallsDto

    /** Answers the ring, joins from the chat, or joins by a pasted link: any member of the group, any plan. */
    @POST("group-calls/{id}/join")
    suspend fun joinGroupCall(@Path("id") callId: String): GroupCallJoinDto

    @POST("group-calls/{id}/decline")
    suspend fun declineGroupCall(@Path("id") callId: String)

    @POST("group-calls/{id}/leave")
    suspend fun leaveGroupCall(@Path("id") callId: String)

    /** The keyholder hands out this epoch's room key, sealed separately to each other participant. */
    @POST("group-calls/{id}/keys")
    suspend fun sendGroupCallKeys(@Path("id") callId: String, @Body body: SendGroupKeysRequest)
}

@Serializable
data class StartCallRequest(val conversationId: String, val kind: String)

@Serializable
data class IceServerDto(val urls: List<String> = emptyList(), val username: String = "", val credential: String = "")

@Serializable
data class StartedCallDto(
    val callId: String,
    val calleeId: String,
    /** Set when joining a call link (the link decides the kind). */
    val kind: String? = null,
    val iceServers: List<IceServerDto> = emptyList(),
)

@Serializable
data class SealedSignalDto(val ciphertext: String, val nonce: String)

@Serializable
data class ScreenRequest(val on: Boolean)

@Serializable
data class RenewedRelayDto(val iceServers: List<IceServerDto> = emptyList(), val expiresAt: String = "")

@Serializable
data class RelaysDto(val iceServers: List<IceServerDto> = emptyList())

@Serializable
data class CallLinkDto(
    val id: String,
    val kind: String,
    /** The link's secret, sealed to your own public key. */
    val secretSealed: String,
    /** The name, encrypted under the link's secret. */
    val nameCiphertext: String,
    val nameNonce: String,
    val createdOn: String = "",
)

@Serializable
data class CreateCallLinkRequest(val kind: String, val secretSealed: String, val nameCiphertext: String, val nameNonce: String)

@Serializable
data class CallLinkInfoDto(val id: String, val kind: String, val ownerId: String, val nameCiphertext: String, val nameNonce: String)

@Serializable
data class StartGroupCallRequest(val conversationId: String, val kind: String)

/** [url] is a path on the site (e.g. /redvaultmessages/livekit), not a full URL: the app builds wss://<host><path>. */
@Serializable
data class GroupCallJoinDto(
    val callId: String,
    val conversationId: String,
    val kind: String,
    val url: String,
    val token: String,
    val identity: String,
    /** This request started the call, so this app posts its link in the chat. */
    val created: Boolean = false,
)

@Serializable
data class ActiveGroupCallDto(val callId: String, val conversationId: String, val kind: String, val participants: Int = 0)

@Serializable
data class ActiveGroupCallsDto(val calls: List<ActiveGroupCallDto> = emptyList())

@Serializable
data class GroupKeyEntry(val userId: String, val key: SealedSignalDto)

@Serializable
data class SendGroupKeysRequest(val epoch: Long, val keys: List<GroupKeyEntry>)
