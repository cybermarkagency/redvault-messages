package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path

/**
 * Answers to stickers and the totals the author publishes (docs/stories-advanced/api.md). The server only ever holds sealed
 * envelopes: a viewer's app seals an answer to the author, the author's app opens them and publishes totals sealed with the
 * story's key.
 */
interface StoryInteractionsApi {
    /** One answer per account per sticker; a later PUT replaces it unless the first was final (a quiz or a question). */
    @PUT("stories/{id}/responses/{stickerId}")
    suspend fun putAnswer(@Path("id") storyId: String, @Path("stickerId") stickerId: String, @Body body: StoryAnswerRequest): StoryAnswerSaved

    /** The author's inbox: every sealed answer on the story. */
    @GET("stories/{id}/responses")
    suspend fun answers(@Path("id") storyId: String): List<StoryAnswerRowDto>

    /** What the caller already answered, so a reload or another phone shows it. */
    @GET("stories/{id}/responses/mine")
    suspend fun myAnswers(@Path("id") storyId: String): List<StoryMyAnswerDto>

    @PUT("stories/{id}/results/{stickerId}")
    suspend fun putTotals(@Path("id") storyId: String, @Path("stickerId") stickerId: String, @Body body: StoryTotalsRequest)

    @GET("stories/{id}/results")
    suspend fun totals(@Path("id") storyId: String): List<StoryTotalsRowDto>

    /** The author reports an abusive answer, nameless or not: the server finds it by its exact envelope and files the report against whoever sent it. */
    @POST("stories/{id}/responses/report")
    suspend fun reportAnswer(@Path("id") storyId: String, @Body body: kotlinx.serialization.json.JsonObject)
}

@Serializable
class StoryAnswerRequest(val envelope: String, val anonymous: Boolean, val final: Boolean)

@Serializable
class StoryAnswerSaved(val revision: Int = 0)

@Serializable
class StoryAnswerRowDto(
    val stickerId: String,
    /** Null for an answer sent without a name. */
    val responderId: String? = null,
    val anonymous: Boolean = false,
    val envelope: String,
    val revision: Int = 0,
    val respondedAt: String,
)

@Serializable
class StoryMyAnswerDto(val stickerId: String, val anonymous: Boolean = false, val envelope: String, val revision: Int = 0)

@Serializable
class StoryTotalsRequest(val revision: Int, val nonce: String, val ciphertext: String)

@Serializable
class StoryTotalsRowDto(val stickerId: String, val revision: Int, val nonce: String, val ciphertext: String)
