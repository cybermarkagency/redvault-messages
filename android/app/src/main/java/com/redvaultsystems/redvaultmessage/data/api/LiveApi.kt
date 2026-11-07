package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonObject
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Query

/**
 * The live channel (server/src/routes/liveRoutes.ts): a long poll that
 * carries call signaling and typing events, plus typing and presence.
 * Its own interface on a client with a longer read timeout, because each
 * poll waits up to 25 seconds for something to happen.
 */
interface LiveApi {
    /** New events after [after], or none after ~25 s. Without [after], returns at once. */
    @GET("live/events")
    suspend fun events(@Query("after") after: Long? = null): LiveEventsDto

    /** "I'm typing in this chat" (only sent while typing indicators are on). */
    @POST("live/typing")
    suspend fun typing(@Body body: TypingRequest)

    /** Which contacts are online (empty unless you share your own status). */
    @GET("live/presence")
    suspend fun presence(): PresenceDto
}

@Serializable
data class LiveEventsDto(
    val cursor: Long,
    /** Kept as JSON: calls read the fields their type has; unknown types are skipped. */
    val events: List<JsonObject> = emptyList(),
    val callsEnabled: Boolean = true,
    val groupCallsEnabled: Boolean = false,
)

@Serializable
data class TypingRequest(val conversationId: String)

@Serializable
data class PresenceDto(val online: List<String> = emptyList())
