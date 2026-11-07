package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiClient
import com.redvaultsystems.redvaultmessage.data.api.ApiException
import com.redvaultsystems.redvaultmessage.data.api.apiCall
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put
import retrofit2.http.Body
import retrofit2.http.DELETE
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.PUT
import retrofit2.http.Path
import retrofit2.http.Query

/**
 * Protocol-3 endpoints (server/src/routes/e2eV3Routes.ts), mirroring ui/src/lib/api.ts. Public keys,
 * signed lists and ciphertext only; the server is off unless E2E_V3_ENABLED. Everything the engine
 * produces (identities, signed lists, messages) travels as the JSON the engine made.
 */
interface V3Service {
    @GET("e2e/v3/config")
    suspend fun config(): JsonObject

    @PUT("e2e/v3/devices/{id}")
    suspend fun registerDevice(@Path("id") deviceId: String, @Body body: JsonElement): JsonObject

    @GET("e2e/v3/devices")
    suspend fun devices(): JsonArray

    @DELETE("e2e/v3/devices/{id}")
    suspend fun removeDevice(@Path("id") deviceId: String)

    @GET("e2e/v3/users/{user}/device-list")
    suspend fun deviceList(@Path("user") userId: String, @Query("conversationId") conversationId: String?): JsonObject

    @PUT("e2e/v3/device-list")
    suspend fun putDeviceList(@Body body: JsonElement): JsonObject

    @POST("e2e/v3/device-list/cancel-reset")
    suspend fun cancelReset(@Body body: JsonElement)

    @POST("e2e/v3/enrollments")
    suspend fun createEnrollment(@Body body: JsonElement): JsonObject

    @GET("e2e/v3/enrollments")
    suspend fun enrollments(): JsonArray

    @POST("e2e/v3/enrollments/{id}/messages")
    suspend fun postEnrollmentMessage(@Path("id") id: String, @Body body: JsonElement): JsonObject

    @GET("e2e/v3/enrollments/{id}/messages")
    suspend fun enrollmentMessages(@Path("id") id: String, @Query("after") after: Int): JsonArray

    @DELETE("e2e/v3/enrollments/{id}")
    suspend fun deleteEnrollment(@Path("id") id: String)

    @POST("e2e/v3/messages")
    suspend fun sendMessages(@Body body: JsonElement): JsonObject

    @GET("e2e/v3/devices/{id}/messages")
    suspend fun inbox(@Path("id") deviceId: String): JsonArray

    @POST("e2e/v3/devices/{id}/messages/ack")
    suspend fun ack(@Path("id") deviceId: String, @Body body: JsonElement)

    @PUT("e2e/v3/recovery-backup")
    suspend fun putRecoveryBackup(@Body body: JsonElement)

    @GET("e2e/v3/recovery-backup")
    suspend fun recoveryBackup(): JsonObject

    @POST("e2e/v3/claim")
    suspend fun claim(@Body body: JsonElement): JsonArray

    @GET("e2e/v3/conversations/{id}/device-lists")
    suspend fun conversationDeviceLists(@Path("id") conversationId: String): JsonObject

    @POST("e2e/v3/group-messages")
    suspend fun sendGroupMessage(@Body body: JsonElement): JsonObject

    @GET("e2e/v3/events")
    suspend fun events(@Query("after") after: Int): JsonArray

    // rv-signal-v1 prekey directory (E2E_SIGNAL_ENABLED), docs/signal-integration/.
    @PUT("e2e/v3/devices/{id}/signal")
    suspend fun signalUpload(@Path("id") deviceId: String, @Body body: JsonElement): JsonObject

    @GET("e2e/v3/devices/{id}/signal")
    suspend fun signalStatus(@Path("id") deviceId: String): JsonObject

    @POST("e2e/v3/signal/claim")
    suspend fun signalClaim(@Body body: JsonElement): JsonArray

    @GET("e2e/v3/conversations/{id}/rosters")
    suspend fun rosters(@Path("id") conversationId: String, @Query("after") after: Int): JsonArray

    @POST("e2e/v3/conversations/{id}/rosters")
    suspend fun postRoster(@Path("id") conversationId: String, @Body body: JsonElement): JsonObject
}

/** A device the server lists for this account; [listed] means it is in the signed list (trusted). */
data class OwnDevice(val deviceId: String, val ed25519: String, val createdOn: String, val listed: Boolean)

data class PendingReset(val accountKey: String, val effectiveAt: String)

/** A user's signed device list as the server holds it (null: none yet), and a reset waiting to take effect. */
data class ListView(val deviceList: JsonObject?, val pendingReset: PendingReset?)

data class Enrollment(val id: String, val newDeviceId: String)

data class EnrollmentMessage(val seq: Int, val fromDeviceId: String, val body: String)

data class V3Event(val id: Long, val kind: String, val deviceId: String?, val createdOn: String)

/** The protocol-3 server calls the device flows use. A fake stands in for the server in unit tests. */
interface V3Api {
    suspend fun enabled(): Boolean
    suspend fun registerDevice(upload: JsonObject)
    suspend fun listDevices(): List<OwnDevice>
    suspend fun removeDevice(deviceId: String)
    suspend fun deviceList(userId: String, conversationId: String? = null): ListView

    /** Throws an ApiException with code `stale_device_list` when another device changed the list first. */
    suspend fun putDeviceList(deviceList: JsonObject, rotationProof: String? = null)
    suspend fun cancelReset(signature: String)
    suspend fun createEnrollment(newDeviceId: String): Enrollment
    suspend fun enrollments(): List<Enrollment>
    suspend fun postEnrollmentMessage(id: String, fromDeviceId: String, body: String)
    suspend fun enrollmentMessages(id: String, after: Int = 0): List<EnrollmentMessage>
    suspend fun deleteEnrollment(id: String)
    suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>)
    suspend fun inbox(deviceId: String): List<JsonObject>
    suspend fun ack(deviceId: String, ids: List<String>)
    suspend fun putRecoveryBackup(backup: String, signature: String)
    suspend fun recoveryBackup(): String?
    suspend fun events(after: Int = 0): List<V3Event>

    /** One signed one-time (or fallback) key per device: `{userId, deviceId, oneTimeKey, fallbackKey}` as the server answers. */
    suspend fun claimKeys(conversationId: String?, devices: List<Pair<String, String>>): List<JsonObject>

    /** Every active participant's signed list (9.2): userId to its list view. */
    suspend fun conversationDeviceLists(conversationId: String): List<Pair<String, ListView>>

    /** One body fanned out to many devices of the conversation's members; recipients are (userId, deviceId). */
    suspend fun sendGroupMessage(senderDeviceId: String, conversationId: String, body: String, recipients: List<Pair<String, String>>)

    // --- rv-signal-v1 (E2E_SIGNAL_ENABLED). Defaults answer like a server with it off. ---

    /** Whether the server has the Signal directory on (`signal` in /e2e/v3/config). */
    suspend fun signalEnabled(): Boolean = false

    /** Stores the engine's signalPrekeys upload for this account's device; answers the key counts. */
    suspend fun signalUpload(deviceId: String, upload: JsonObject): JsonObject =
        throw ApiException(503, "e2e_signal_disabled", "Signal sessions aren't available here.")

    /** `{enabled, preKeys, kyberPreKeys, ...}` for one of this account's devices. */
    suspend fun signalStatus(deviceId: String): JsonObject =
        throw ApiException(503, "e2e_signal_disabled", "Signal sessions aren't available here.")

    /** One `{userId, deviceId, certificate, bundle}` per device; one-time keys are used up. */
    suspend fun signalClaim(conversationId: String?, devices: List<Pair<String, String>>): List<JsonObject> =
        throw ApiException(503, "e2e_signal_disabled", "Signal sessions aren't available here.")

    /** M6: a conversation's signed rosters after [after], oldest first (0: the whole history). */
    suspend fun rosters(conversationId: String, after: Int = 0): List<JsonObject> =
        throw ApiException(503, "e2e_signal_disabled", "Signal sessions aren't available here.")

    /** M6: stores the next roster; ApiException `stale_roster` when another member's came first. */
    suspend fun postRoster(conversationId: String, roster: JsonObject): Unit =
        throw ApiException(503, "e2e_signal_disabled", "Signal sessions aren't available here.")
}

private fun JsonObject.str(name: String): String = this[name]?.jsonPrimitive?.contentOrNull.orEmpty()

class RetrofitV3Api(private val service: V3Service) : V3Api {
    private val json = ApiClient.json

    override suspend fun enabled(): Boolean = apiCall(json) { service.config() }["enabled"]?.jsonPrimitive?.contentOrNull == "true"

    override suspend fun registerDevice(upload: JsonObject) {
        val deviceId = upload["identity"]!!.jsonObject.str("deviceId")
        apiCall(json) { service.registerDevice(deviceId, upload) }
    }

    override suspend fun listDevices(): List<OwnDevice> = apiCall(json) { service.devices() }.map {
        val d = it.jsonObject
        OwnDevice(d.str("deviceId"), d.str("ed25519"), d.str("createdOn"), d["listed"]?.jsonPrimitive?.contentOrNull == "true")
    }

    override suspend fun removeDevice(deviceId: String) = apiCall(json) { service.removeDevice(deviceId) }

    override suspend fun deviceList(userId: String, conversationId: String?): ListView {
        val view = apiCall(json) { service.deviceList(userId, conversationId) }
        val pending = (view["pendingReset"] as? JsonObject)?.let { PendingReset(it.str("accountKey"), it.str("effectiveAt")) }
        return ListView(view["deviceList"] as? JsonObject, pending)
    }

    override suspend fun putDeviceList(deviceList: JsonObject, rotationProof: String?) {
        val body = buildJsonObject {
            put("deviceList", deviceList)
            put("rotationProof", rotationProof?.let { JsonPrimitive(it) } ?: JsonNull)
        }
        apiCall(json) { service.putDeviceList(body) }
    }

    override suspend fun cancelReset(signature: String) = apiCall(json) { service.cancelReset(buildJsonObject { put("signature", signature) }) }

    override suspend fun createEnrollment(newDeviceId: String): Enrollment {
        val e = apiCall(json) { service.createEnrollment(buildJsonObject { put("newDeviceId", newDeviceId) }) }
        return Enrollment(e.str("id"), e.str("newDeviceId"))
    }

    override suspend fun enrollments(): List<Enrollment> =
        apiCall(json) { service.enrollments() }.map { Enrollment(it.jsonObject.str("id"), it.jsonObject.str("newDeviceId")) }

    override suspend fun postEnrollmentMessage(id: String, fromDeviceId: String, body: String) {
        apiCall(json) { service.postEnrollmentMessage(id, buildJsonObject { put("fromDeviceId", fromDeviceId); put("body", body) }) }
    }

    override suspend fun enrollmentMessages(id: String, after: Int): List<EnrollmentMessage> =
        apiCall(json) { service.enrollmentMessages(id, after) }.map {
            val m = it.jsonObject
            EnrollmentMessage(m["seq"]!!.jsonPrimitive.int, m.str("fromDeviceId"), m.str("body"))
        }

    override suspend fun deleteEnrollment(id: String) = apiCall(json) { service.deleteEnrollment(id) }

    override suspend fun sendMessages(senderDeviceId: String, conversationId: String?, messages: List<Triple<String, String, String>>) {
        val body = buildJsonObject {
            put("senderDeviceId", senderDeviceId)
            put("conversationId", conversationId?.let { JsonPrimitive(it) } ?: JsonNull)
            put(
                "messages",
                buildJsonArray {
                    for ((userId, deviceId, text) in messages) add(buildJsonObject { put("userId", userId); put("deviceId", deviceId); put("body", text) })
                }
            )
        }
        apiCall(json) { service.sendMessages(body) }
    }

    override suspend fun inbox(deviceId: String): List<JsonObject> = apiCall(json) { service.inbox(deviceId) }.map { it.jsonObject }

    override suspend fun ack(deviceId: String, ids: List<String>) {
        apiCall(json) { service.ack(deviceId, buildJsonObject { put("ids", buildJsonArray { ids.forEach { add(JsonPrimitive(it)) } }) }) }
    }

    override suspend fun putRecoveryBackup(backup: String, signature: String) {
        apiCall(json) { service.putRecoveryBackup(buildJsonObject { put("backup", backup); put("signature", signature) }) }
    }

    override suspend fun recoveryBackup(): String? =
        apiCall(json) { service.recoveryBackup() }["backup"]?.jsonPrimitive?.contentOrNull

    override suspend fun claimKeys(conversationId: String?, devices: List<Pair<String, String>>): List<JsonObject> {
        val body = buildJsonObject {
            put("conversationId", conversationId?.let { JsonPrimitive(it) } ?: JsonNull)
            put("devices", buildJsonArray { for ((u, d) in devices) add(buildJsonObject { put("userId", u); put("deviceId", d) }) })
        }
        return apiCall(json) { service.claim(body) }.map { it.jsonObject }
    }

    override suspend fun conversationDeviceLists(conversationId: String): List<Pair<String, ListView>> =
        apiCall(json) { service.conversationDeviceLists(conversationId) }["members"]!!.jsonArray.map {
            val m = it.jsonObject
            val pending = (m["pendingReset"] as? JsonObject)?.let { p -> PendingReset(p.str("accountKey"), p.str("effectiveAt")) }
            m.str("userId") to ListView(m["deviceList"] as? JsonObject, pending)
        }

    override suspend fun sendGroupMessage(senderDeviceId: String, conversationId: String, body: String, recipients: List<Pair<String, String>>) {
        val request = buildJsonObject {
            put("senderDeviceId", senderDeviceId)
            put("conversationId", conversationId)
            put("body", body)
            put("recipients", buildJsonArray { for ((u, d) in recipients) add(buildJsonObject { put("userId", u); put("deviceId", d) }) })
        }
        apiCall(json) { service.sendGroupMessage(request) }
    }

    override suspend fun signalEnabled(): Boolean = apiCall(json) { service.config() }["signal"]?.jsonPrimitive?.contentOrNull == "true"

    override suspend fun signalUpload(deviceId: String, upload: JsonObject): JsonObject = apiCall(json) { service.signalUpload(deviceId, upload) }

    override suspend fun signalStatus(deviceId: String): JsonObject = apiCall(json) { service.signalStatus(deviceId) }

    override suspend fun signalClaim(conversationId: String?, devices: List<Pair<String, String>>): List<JsonObject> {
        val body = buildJsonObject {
            put("conversationId", conversationId?.let { JsonPrimitive(it) } ?: JsonNull)
            put("devices", buildJsonArray { for ((u, d) in devices) add(buildJsonObject { put("userId", u); put("deviceId", d) }) })
        }
        return apiCall(json) { service.signalClaim(body) }.map { it.jsonObject }
    }

    override suspend fun rosters(conversationId: String, after: Int): List<JsonObject> =
        apiCall(json) { service.rosters(conversationId, after) }.map { it.jsonObject }

    override suspend fun postRoster(conversationId: String, roster: JsonObject) {
        apiCall(json) { service.postRoster(conversationId, buildJsonObject { put("roster", roster) }) }
    }

    override suspend fun events(after: Int): List<V3Event> = apiCall(json) { service.events(after) }.map {
        val e = it.jsonObject
        V3Event(e["id"]!!.jsonPrimitive.long, e.str("kind"), e["deviceId"]?.jsonPrimitive?.contentOrNull, e.str("createdOn"))
    }
}
