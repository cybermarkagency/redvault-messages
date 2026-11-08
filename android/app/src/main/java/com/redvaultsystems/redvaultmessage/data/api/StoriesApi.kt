// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.doubleOrNull
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.PATCH
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path

/**
 * The server's /api/stories (docs/stories.md, docs/stories-advanced/api.md). Everything here is ciphertext or
 * ids. Paths are relative to BuildConfig.API_BASE; the photo and video bytes go through StoriesRepository's own
 * OkHttp calls instead (they're octet streams, not JSON).
 *
 * Responses that the app reads leniently (the config and the feed) come back as [JsonObject] and are read by
 * [StoryConfigParser] and [StoryFeedParser]: a field or story from a newer server must never make the rest unreadable.
 */
interface StoriesApi {
    /** The only route that answers while the server has Stories switched off (STORIES_ENABLED). */
    @GET("stories/config")
    suspend fun config(): JsonObject

    @GET("stories/feed")
    suspend fun feed(): JsonObject

    /**
     * Built as a [JsonObject] because the server wants `"mediaSize": null` spelled out for a text story, and the
     * app's JSON setup leaves nulls out.
     */
    @POST("stories")
    suspend fun create(@Body body: JsonObject): StoryCreatedDto

    @POST("stories/{id}/view")
    suspend fun markViewed(@Path("id") storyId: String)

    @DELETE("stories/{id}")
    suspend fun delete(@Path("id") storyId: String)

    @GET("stories/prefs")
    suspend fun prefs(): StoryPrefsDto

    @GET("stories/key-gaps")
    suspend fun keyGaps(): List<StoryKeyGapDto>

    @POST("stories/{id}/keys")
    suspend fun addKeys(@Path("id") storyId: String, @Body body: StoryKeysRequest)

    @PUT("stories/prefs")
    suspend fun putPrefs(@Body body: JsonObject): StoryPrefsSavedDto

    /** Takes a person's keys to your stories away (all of them, or the ones listed). Built as a [JsonObject]: `storyIds` is left out when it isn't wanted. */
    @POST("stories/revoke")
    suspend fun revoke(@Body body: JsonObject)

    /** Your stories that left your story (kept for you, or in a highlight), with their own keys. */
    @GET("stories/archive")
    suspend fun archive(): JsonArray

    @GET("stories/users/{userId}/highlights")
    suspend fun highlights(@Path("userId") userId: String): JsonArray

    @POST("stories/highlights")
    suspend fun createHighlight(@Body body: JsonObject): StoryCreatedDto

    @PATCH("stories/highlights/{id}")
    suspend fun updateHighlight(@Path("id") highlightId: String, @Body body: JsonObject): StoryCreatedDto

    @DELETE("stories/highlights/{id}")
    suspend fun deleteHighlight(@Path("id") highlightId: String)

    /** Reports a story you can see (nothing from it is sent unless the reporter ticks a box). */
    @POST("stories/{id}/report")
    suspend fun reportStory(@Path("id") storyId: String, @Body body: JsonObject)

    /** The ids of friends who shared that they saw one of your stories. */
    @GET("stories/{id}/viewers")
    suspend fun viewers(@Path("id") storyId: String): List<String>
}

/** One story as the server hands it to a viewer: the sealed details, and this viewer's own sealed copy of the key. */
@Serializable
class StoryDto(
    val id: String,
    val authorId: String,
    /** A string, not an enum: a kind from a newer app must not break reading the feed. */
    val kind: String,
    val metaCiphertext: String,
    val metaNonce: String,
    val mediaSize: Long? = null,
    val hasMedia: Boolean = false,
    val allowReplies: Boolean = true,
    val createdAt: String,
    val expiresAt: String,
    val wrappedKey: String,
    /** Feed only: already seen by this viewer (shared view). */
    val seen: Boolean = false,
    /** Your own stories: how many friends shared that they saw it. */
    val viewCount: Int = 0,
)

@Serializable
class StoryCreatedDto(val id: String, val published: Boolean = false, val expiresAt: String? = null)

@Serializable
class StoryPrefsDto(
    val prefsCiphertext: String? = null,
    val prefsNonce: String? = null,
    val archiveEnabled: Boolean = false,
    val revision: Int = 0,
)

@Serializable
class StoryPrefsSavedDto(val archiveEnabled: Boolean = false, val revision: Int = 0)

@Serializable
class StoryWrappedKeyDto(val userId: String, val wrappedKey: String)

@Serializable
class StoryKeysRequest(val keys: List<StoryWrappedKeyDto>)

@Serializable
class StoryKeyGapUserDto(val userId: String, val publicKey: String)

@Serializable
class StoryKeyGapDto(val storyId: String, val users: List<StoryKeyGapUserDto>)

class StoryAuthorGroup(val authorId: String, val stories: List<StoryDto>)

class StoryFeed(val own: List<StoryDto>, val authors: List<StoryAuthorGroup>)

/** A highlight: someone's chosen stories, kept on their profile. The title is sealed with the cover story's key. */
class HighlightDto(val id: String, val ownerId: String, val titleCiphertext: String, val titleNonce: String, val coverStoryId: String, val stories: List<StoryDto>)

/** Whether this app may show Stories at all, decided from `GET /stories/config` before anything else is asked. */
sealed interface StoryAvailability {
    /** The server has Stories switched off (or has no Stories). Nothing is shown and nothing else is requested. */
    data object Off : StoryAvailability

    class On(
        /** Photos and videos can be posted (the server has somewhere to keep them). */
        val media: Boolean,
        /** Polls, quizzes, questions and sliders are on. */
        val interactive: Boolean,
        /** How long a story may last, in seconds; just 24 hours when the server doesn't say. */
        val ttlSeconds: List<Int>,
    ) : StoryAvailability
}

object StoryConfigParser {
    const val DEFAULT_TTL_SECONDS = 86_400

    /**
     * `enabled: false` is Off. A missing `enabled` means on (a server from before the switch says nothing, and
     * every server that has Stories answers this route). An `enabled` that is anything but true or false, or a reply that
     * isn't an object, can't be trusted either way, so it's Off: the web app would carry on, this one doesn't.
     */
    fun parse(json: JsonElement): StoryAvailability {
        val r = json as? JsonObject ?: return StoryAvailability.Off
        val enabled = r["enabled"]
        val on = when {
            enabled == null || enabled is JsonNull -> true
            enabled is JsonPrimitive && !enabled.isString && (enabled.content == "true" || enabled.content == "false") -> enabled.content == "true"
            else -> return StoryAvailability.Off
        }
        if (!on) return StoryAvailability.Off
        val ttl = (r["ttlSeconds"] as? JsonArray)
            ?.mapNotNull { (it as? JsonPrimitive)?.takeIf { p -> !p.isString }?.doubleOrNull }
            ?.filter { it > 0 && it == kotlin.math.floor(it) && it <= Int.MAX_VALUE }
            ?.map { it.toInt() }
            .orEmpty()
        return StoryAvailability.On(
            media = r["media"].isTrue(),
            interactive = r["interactive"].isTrue(),
            ttlSeconds = ttl.ifEmpty { listOf(DEFAULT_TTL_SECONDS) },
        )
    }

    private fun JsonElement?.isTrue() = this is JsonPrimitive && !isString && content == "true"
}

object StoryFeedParser {
    private fun story(e: JsonElement): StoryDto? = runCatching { ApiClient.json.decodeFromJsonElement(StoryDto.serializer(), e) }.getOrNull()

    /** A list of stories (the archive), one at a time: one that can't be read is left out. */
    fun parseStories(json: JsonArray): List<StoryDto> = json.mapNotNull(::story)

    /** Highlights, one at a time, with their stories read the same way. */
    fun parseHighlights(json: JsonArray): List<HighlightDto> = json.mapNotNull { h ->
        val o = h as? JsonObject ?: return@mapNotNull null
        fun s(k: String) = (o[k] as? JsonPrimitive)?.takeIf { it.isString }?.content
        HighlightDto(
            s("id") ?: return@mapNotNull null, s("ownerId") ?: return@mapNotNull null, s("titleCiphertext") ?: return@mapNotNull null,
            s("titleNonce") ?: return@mapNotNull null, s("coverStoryId") ?: return@mapNotNull null,
            (o["stories"] as? JsonArray).orEmpty().mapNotNull(::story),
        )
    }

    /** Reads the feed one story at a time: one that can't be read is left out, never the others. */
    fun parse(json: JsonObject): StoryFeed {
        fun story(e: JsonElement): StoryDto? = runCatching { ApiClient.json.decodeFromJsonElement(StoryDto.serializer(), e) }.getOrNull()
        val own = (json["own"] as? JsonArray).orEmpty().mapNotNull(::story)
        val authors = (json["authors"] as? JsonArray).orEmpty().mapNotNull { group ->
            val g = group as? JsonObject ?: return@mapNotNull null
            val authorId = (g["authorId"] as? JsonPrimitive)?.takeIf { it.isString }?.content ?: return@mapNotNull null
            StoryAuthorGroup(authorId, (g["stories"] as? JsonArray).orEmpty().mapNotNull(::story))
        }
        return StoryFeed(own, authors)
    }
}
