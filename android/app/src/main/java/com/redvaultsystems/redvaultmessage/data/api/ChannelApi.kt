// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.Header
import retrofit2.http.PATCH
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path
import retrofit2.http.Query

/**
 * Reading channels (server/src/routes/channelRoutes.ts, /api/public/channels).
 * Public, so these go out on ApiClient.publicClient: no session cookie, no
 * account, and the server can't tell who reads which channel. A private
 * channel also needs its link's access token ([access], X-Channel-Access);
 * Retrofit leaves the header out when it's null.
 */
interface PublicChannelApi {
    @GET("public/channels")
    suspend fun search(@Query("q") query: String?, @Query("limit") limit: Int, @Query("offset") offset: Int): List<ChannelDto>

    @GET("public/channels/{id}")
    suspend fun get(@Path("id") id: String, @Header(ACCESS_HEADER) access: String? = null): ChannelDto

    /** Newest first; [before] (a post's createdAt) pages back. */
    @GET("public/channels/{id}/posts")
    suspend fun posts(
        @Path("id") id: String,
        @Query("before") before: String?,
        @Query("limit") limit: Int,
        @Header(ACCESS_HEADER) access: String? = null,
    ): List<ChannelPostDto>

    /** One anonymous view for each of these posts (post ids only; each phone reports a post once). */
    @POST("public/channels/views")
    suspend fun views(@Body body: ChannelViewsRequest)
}

/**
 * Running channels, voting, ticking, suggesting and reporting: signed in
 * (/api/channels). Owners and super admins delete; only Premium creates and
 * posts. Calls about a private channel's posts carry its access token too.
 */
interface ChannelApi {
    @GET("channels/mine")
    suspend fun mine(): List<ChannelDto>

    @POST("channels")
    suspend fun create(@Body body: ChannelDetailsRequest): ChannelDto

    @PATCH("channels/{id}")
    suspend fun update(@Path("id") id: String, @Body body: ChannelDetailsRequest): ChannelDto

    @PUT("channels/{id}/visibility")
    suspend fun setVisibility(@Path("id") id: String, @Body body: ChannelVisibilityRequest): ChannelDto

    @PUT("channels/{id}/settings")
    suspend fun setSettings(@Path("id") id: String, @Body body: ChannelSettingsRequest): ChannelDto

    @PUT("channels/{id}/avatar")
    suspend fun setAvatar(@Path("id") id: String, @Body body: ChannelAvatarRequest): ChannelDto

    @DELETE("channels/{id}/avatar")
    suspend fun removeAvatar(@Path("id") id: String)

    @DELETE("channels/{id}")
    suspend fun delete(@Path("id") id: String)

    // --- Posting ---

    /** Owner: starts an upload for a post; its bytes then go to the returned uploadUrl. */
    @POST("channels/{id}/media")
    suspend fun startMedia(@Path("id") id: String, @Body body: ChannelMediaStartRequest): ChannelUploadDto

    @POST("channels/{id}/posts")
    suspend fun post(@Path("id") id: String, @Body body: ChannelPostRequest): ChannelPostDto

    @GET("channels/{id}/scheduled")
    suspend fun scheduled(@Path("id") id: String): List<ChannelPostDto>

    @POST("channels/{id}/posts/{postId}/publish")
    suspend fun publishNow(@Path("id") id: String, @Path("postId") postId: String): ChannelPostDto

    @DELETE("channels/{id}/posts/{postId}")
    suspend fun deletePost(@Path("id") id: String, @Path("postId") postId: String)

    // --- Polls and checklists ---

    /** An empty list takes a poll vote back; a quiz's answer is final and comes back with the right one. */
    @POST("channels/{id}/posts/{postId}/vote")
    suspend fun vote(
        @Path("id") id: String,
        @Path("postId") postId: String,
        @Body body: ChannelVoteRequest,
        @Header(ACCESS_HEADER) access: String? = null,
    ): ChannelVoteDto

    @POST("channels/{id}/posts/{postId}/close")
    suspend fun closePoll(@Path("id") id: String, @Path("postId") postId: String): ChannelPostDto

    @PUT("channels/{id}/posts/{postId}/checklist/{index}")
    suspend fun setChecklistItem(
        @Path("id") id: String,
        @Path("postId") postId: String,
        @Path("index") index: Int,
        @Body body: ChannelCheckRequest,
        @Header(ACCESS_HEADER) access: String? = null,
    ): ChannelChecklistDto

    // --- Suggested posts ---

    /** A follower: starts uploading a photo for a suggested post. */
    @POST("channels/{id}/suggestions/media")
    suspend fun startSuggestionMedia(
        @Path("id") id: String,
        @Body body: ChannelMediaStartRequest,
        @Header(ACCESS_HEADER) access: String? = null,
    ): ChannelUploadDto

    @POST("channels/{id}/suggestions")
    suspend fun suggest(@Path("id") id: String, @Body body: ChannelSuggestionRequest, @Header(ACCESS_HEADER) access: String? = null): ChannelSuggestionDto

    @GET("channels/{id}/suggestions/mine")
    suspend fun mySuggestions(@Path("id") id: String): List<ChannelSuggestionDto>

    /** Owner: suggestions waiting, oldest first. Never who sent them. */
    @GET("channels/{id}/suggestions")
    suspend fun suggestions(@Path("id") id: String): List<ChannelSuggestionDto>

    @POST("channels/{id}/suggestions/{suggestionId}/approve")
    suspend fun approveSuggestion(@Path("id") id: String, @Path("suggestionId") suggestionId: String, @Body body: ChannelApproveRequest): ChannelPostDto

    @POST("channels/{id}/suggestions/{suggestionId}/reject")
    suspend fun rejectSuggestion(@Path("id") id: String, @Path("suggestionId") suggestionId: String)

    @POST("channels/{id}/report")
    suspend fun report(@Path("id") id: String, @Body body: ChannelReportRequest)
}

const val ACCESS_HEADER = "X-Channel-Access"

/**
 * What anyone who can read a channel sees of it. Never who owns it.
 * [secretSealed] is only in the owner's own copies (channels/mine and the
 * owner's edits): the private link's secret, sealed to their public key.
 */
@Serializable
data class ChannelDto(
    val id: String,
    val name: String,
    val description: String = "",
    val createdAt: String = "",
    val lastPostAt: String? = null,
    /** Left out of the directory; readable only with the private link. */
    val isPrivate: Boolean = false,
    /** The owner takes suggested posts from followers. */
    val acceptSuggestions: Boolean = false,
    /** Same-server path of the channel's photo (versioned), or null. Fetched without the cookie. */
    val avatarUrl: String? = null,
    val secretSealed: String? = null,
)

/** A photo, video, file or recording in a post: [url] is a signed, expiring same-server link to its bytes. */
@Serializable
data class ChannelMediaDto(
    val id: String,
    /** "photo", "video", "file", "audio", "voice" or "round". */
    val kind: String,
    val mime: String = "application/octet-stream",
    val size: Long = 0,
    val filename: String? = null,
    val durationMs: Long? = null,
    val width: Int? = null,
    val height: Int? = null,
    val url: String,
)

/** A poll as readers see it. A quiz's [correct] stays null until it's closed (or in the answer to your own vote). */
@Serializable
data class ChannelPollDto(
    val question: String,
    val options: List<String>,
    val multiple: Boolean = false,
    val quiz: Boolean = false,
    val closed: Boolean = false,
    val counts: List<Int> = emptyList(),
    val voters: Int = 0,
    val correct: Int? = null,
    val explanation: String? = null,
)

@Serializable
data class ChannelChecklistItemDto(val text: String, val done: Boolean = false)

@Serializable
data class ChannelChecklistDto(
    val title: String,
    val items: List<ChannelChecklistItemDto>,
    /** Followers may tick items too. */
    val collaborative: Boolean = false,
)

@Serializable
data class ChannelPostDto(
    val id: String,
    /** Formatted text (data/model/TextFormat.kt); may be empty when the post is a photo, a poll and so on. */
    val body: String,
    /** When it appeared (or, scheduled, when it will). */
    val createdAt: String,
    /** How many devices have seen it, in the channel or forwarded into a chat. */
    val views: Int = 0,
    /** Came from a follower's suggestion (who sent it is never shown). */
    val suggested: Boolean = false,
    val poll: ChannelPollDto? = null,
    val checklist: ChannelChecklistDto? = null,
    val media: List<ChannelMediaDto> = emptyList(),
)

@Serializable
data class ChannelSuggestionDto(
    val id: String,
    val body: String,
    /** "pending", "approved" or "rejected". */
    val status: String,
    val createdAt: String,
    val media: List<ChannelMediaDto> = emptyList(),
)

@Serializable
data class ChannelUploadDto(val id: String, val kind: String = "", val size: Long = 0, val uploadUrl: String)

@Serializable
data class ChannelVoteDto(val poll: ChannelPollDto, val mine: List<Int> = emptyList())

@Serializable
data class ChannelDetailsRequest(val name: String, val description: String)

/** Private: with a new link's [access] token and its secret sealed to you. Public: neither. */
@Serializable
data class ChannelVisibilityRequest(val isPrivate: Boolean, val access: String? = null, val secretSealed: String? = null)

@Serializable
data class ChannelSettingsRequest(val acceptSuggestions: Boolean)

/** A downsized JPEG, base64. */
@Serializable
data class ChannelAvatarRequest(val image: String)

@Serializable
data class ChannelQuizRequest(val correct: Int, val explanation: String = "")

@Serializable
data class ChannelPollRequest(val question: String, val options: List<String>, val multiple: Boolean = false, val quiz: ChannelQuizRequest? = null)

@Serializable
data class ChannelChecklistRequest(val title: String, val items: List<String>, val collaborative: Boolean = false)

/** Text, plus at most one of a poll, a checklist or finished uploads (in order); [publishAt] (ISO) schedules it. */
@Serializable
data class ChannelPostRequest(
    val body: String,
    val poll: ChannelPollRequest? = null,
    val checklist: ChannelChecklistRequest? = null,
    val media: List<String>? = null,
    val publishAt: String? = null,
)

@Serializable
data class ChannelMediaStartRequest(
    val kind: String,
    val mime: String,
    val size: Long,
    val filename: String? = null,
    val durationMs: Long? = null,
    val width: Int? = null,
    val height: Int? = null,
)

@Serializable
data class ChannelVoteRequest(val options: List<Int>)

@Serializable
data class ChannelCheckRequest(val done: Boolean)

@Serializable
data class ChannelSuggestionRequest(val body: String, val media: List<String> = emptyList())

@Serializable
data class ChannelApproveRequest(val body: String? = null, val publishAt: String? = null)

@Serializable
data class ChannelReportRequest(val reason: String)

@Serializable
data class ChannelViewsRequest(val postIds: List<String>)
