// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ChannelApi
import com.redvaultsystems.redvaultmessage.data.api.ChannelApproveRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelAvatarRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelCheckRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelChecklistDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelDetailsRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelMediaStartRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelPostRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelReportRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelSettingsRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelSuggestionDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelSuggestionRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelUploadDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelViewsRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelVisibilityRequest
import com.redvaultsystems.redvaultmessage.data.api.ChannelVoteDto
import com.redvaultsystems.redvaultmessage.data.api.ChannelVoteRequest
import com.redvaultsystems.redvaultmessage.data.api.PublicChannelApi
import com.redvaultsystems.redvaultmessage.data.api.apiCall

/**
 * Channels (server/src/services/channelService.ts): one-way and plain text
 * by design. Reads go through [public], which carries no session cookie, so
 * the server can't tell which account reads which channel; a private
 * channel's reads (and votes, ticks and suggestions) carry its link's access
 * token ([access]). The rest is signed in. Each call throws ApiException on
 * failure.
 */
class ChannelRepository(private val public: PublicChannelApi, private val api: ChannelApi) {
    suspend fun search(query: String, offset: Int = 0): List<ChannelDto> =
        apiCall(ApiClient.json) { public.search(query.trim().ifEmpty { null }, PAGE, offset) }

    suspend fun get(id: String, access: String? = null): ChannelDto = apiCall(ApiClient.json) { public.get(id, access) }

    /** Newest first; [before] (a post's createdAt) pages back. */
    suspend fun posts(id: String, before: String? = null, access: String? = null, limit: Int = PAGE): List<ChannelPostDto> =
        apiCall(ApiClient.json) { public.posts(id, before, limit, access) }

    suspend fun recordViews(postIds: List<String>) = apiCall(ApiClient.json) { public.views(ChannelViewsRequest(postIds)) }

    suspend fun mine(): List<ChannelDto> = apiCall(ApiClient.json) { api.mine() }

    suspend fun create(name: String, description: String): ChannelDto = apiCall(ApiClient.json) { api.create(ChannelDetailsRequest(name, description)) }

    suspend fun update(id: String, name: String, description: String): ChannelDto =
        apiCall(ApiClient.json) { api.update(id, ChannelDetailsRequest(name, description)) }

    suspend fun setVisibility(id: String, request: ChannelVisibilityRequest): ChannelDto = apiCall(ApiClient.json) { api.setVisibility(id, request) }

    suspend fun setAcceptSuggestions(id: String, accept: Boolean): ChannelDto =
        apiCall(ApiClient.json) { api.setSettings(id, ChannelSettingsRequest(accept)) }

    suspend fun setAvatar(id: String, imageBase64: String): ChannelDto = apiCall(ApiClient.json) { api.setAvatar(id, ChannelAvatarRequest(imageBase64)) }

    suspend fun removeAvatar(id: String) = apiCall(ApiClient.json) { api.removeAvatar(id) }

    suspend fun delete(id: String) = apiCall(ApiClient.json) { api.delete(id) }

    suspend fun startMedia(id: String, meta: ChannelMediaStartRequest): ChannelUploadDto = apiCall(ApiClient.json) { api.startMedia(id, meta) }

    suspend fun post(id: String, post: ChannelPostRequest): ChannelPostDto = apiCall(ApiClient.json) { api.post(id, post) }

    suspend fun scheduled(id: String): List<ChannelPostDto> = apiCall(ApiClient.json) { api.scheduled(id) }

    suspend fun publishNow(id: String, postId: String): ChannelPostDto = apiCall(ApiClient.json) { api.publishNow(id, postId) }

    suspend fun deletePost(id: String, postId: String) = apiCall(ApiClient.json) { api.deletePost(id, postId) }

    suspend fun vote(id: String, postId: String, options: List<Int>, access: String?): ChannelVoteDto =
        apiCall(ApiClient.json) { api.vote(id, postId, ChannelVoteRequest(options), access) }

    suspend fun closePoll(id: String, postId: String): ChannelPostDto = apiCall(ApiClient.json) { api.closePoll(id, postId) }

    suspend fun setChecklistItem(id: String, postId: String, index: Int, done: Boolean, access: String?): ChannelChecklistDto =
        apiCall(ApiClient.json) { api.setChecklistItem(id, postId, index, ChannelCheckRequest(done), access) }

    suspend fun startSuggestionMedia(id: String, meta: ChannelMediaStartRequest, access: String?): ChannelUploadDto =
        apiCall(ApiClient.json) { api.startSuggestionMedia(id, meta, access) }

    suspend fun suggest(id: String, body: String, media: List<String>, access: String?): ChannelSuggestionDto =
        apiCall(ApiClient.json) { api.suggest(id, ChannelSuggestionRequest(body, media), access) }

    suspend fun mySuggestions(id: String): List<ChannelSuggestionDto> = apiCall(ApiClient.json) { api.mySuggestions(id) }

    suspend fun suggestions(id: String): List<ChannelSuggestionDto> = apiCall(ApiClient.json) { api.suggestions(id) }

    suspend fun approveSuggestion(id: String, suggestionId: String, body: String?, publishAt: String?): ChannelPostDto =
        apiCall(ApiClient.json) { api.approveSuggestion(id, suggestionId, ChannelApproveRequest(body, publishAt)) }

    suspend fun rejectSuggestion(id: String, suggestionId: String) = apiCall(ApiClient.json) { api.rejectSuggestion(id, suggestionId) }

    suspend fun report(id: String, reason: String) = apiCall(ApiClient.json) { api.report(id, ChannelReportRequest(reason)) }

    companion object {
        /** A page of the directory or of a channel's posts, as the web asks for. */
        const val PAGE = 30
    }
}
