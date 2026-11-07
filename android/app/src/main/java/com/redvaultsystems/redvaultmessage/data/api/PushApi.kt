package com.redvaultsystems.redvaultmessage.data.api

import kotlinx.serialization.Serializable
import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.HTTP
import retrofit2.http.PUT
import retrofit2.http.Query

/**
 * Content-free push without Google services (server/src/routes/pushRoutes.ts, docs/unifiedpush-plan.md):
 * a Web Push subscription for the user's UnifiedPush distributor, or, without one, a long poll the app's
 * own background connection waits on. Its own interface on a client with a longer read timeout, because
 * [wait] holds the request open for up to ~25 seconds (more through Tor).
 */
interface PushApi {
    /** The server's VAPID public key, or null when it can't send Web Push (then the app uses the fallback). */
    @GET("push/config")
    suspend fun config(): PushConfigDto

    /** This phone's Web Push subscription, from the distributor (204; 400 if the server finds it invalid). */
    @PUT("push/webpush")
    suspend fun registerWebPush(@Body body: WebPushSubscriptionRequest)

    @HTTP(method = "DELETE", path = "push/webpush", hasBody = true)
    suspend fun unregisterWebPush(@Body body: WebPushEndpointRequest)

    /**
     * The background connection: what kinds of things happened after [after], or nothing after ~25 s.
     * Without [after] (the first call) it answers at once with the cursor to start from. Doesn't mark
     * the user online.
     */
    @GET("push/wait")
    suspend fun wait(@Query("after") after: Long? = null): PushWaitDto
}

@Serializable
data class PushConfigDto(val vapidPublicKey: String? = null)

/** [p256dh]: the 65-byte uncompressed P-256 key; [auth]: the 16-byte secret. Both base64url, as the distributor gives them. */
@Serializable
data class WebPushSubscriptionRequest(val endpoint: String, val p256dh: String, val auth: String)

@Serializable
data class WebPushEndpointRequest(val endpoint: String)

/** [kinds] null: start from [cursor] (first call, or the server restarted); otherwise what happened, oldest first. */
@Serializable
data class PushWaitDto(val cursor: Long, val kinds: List<String>? = null)
