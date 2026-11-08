// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive

/** The devices of [userId] from a list this device verified (contacts: pinned key; own: same account key). */
class VerifiedDevices(val devices: List<JsonObject>, val pendingReset: PendingReset?)

suspend fun verifiedDevices(device: V3Device, userId: String, conversationId: String?, legacySigningKey: String? = null): VerifiedDevices {
    val view = device.api.deviceList(userId, conversationId)
    val list = view.deviceList ?: return VerifiedDevices(emptyList(), view.pendingReset)
    val response = device.op("verifyDeviceList", "deviceList" to list, "legacySigningKey" to legacySigningKey)
    if (!response.ok) throw EngineException(response.error ?: "bad_device_list", response.message ?: "the device list doesn't verify")
    return VerifiedDevices(response.array("devices").map { it.jsonObject }, view.pendingReset)
}

class Received(
    val id: String,
    val senderUserId: String,
    val senderDeviceId: String,
    val conversationId: String?,
    val kind: String,
    val text: String?,
    val error: String?,
)

/** Errors about the sender's list, not the message: the item waits until the person accepts the change. */
private val HOLD = setOf("account_key_changed", "rollback", "equivocation", "uncertified_account_key", "bad_device_list")

/**
 * Reads this device's mailbox. Each sender's list is verified first. Items are acknowledged once handled,
 * even one that can't be decrypted (a duplicate, or not for this device), so one bad item never blocks the
 * rest; only items whose sender's identity changed are kept for later.
 */
suspend fun receive(device: V3Device): List<Received> {
    val items = device.api.inbox(device.deviceId)
    val out = mutableListOf<Received>()
    val handled = mutableListOf<String>()
    class Known(val devices: List<JsonObject>, val error: String?)
    val lists = HashMap<String, Known>()
    for (item in items) {
        val senderUserId = item["senderUserId"]!!.jsonPrimitive.content
        val senderDeviceId = item["senderDeviceId"]!!.jsonPrimitive.content
        val conversationId = item["conversationId"]?.jsonPrimitive?.contentOrNull
        val id = item["id"]!!.jsonPrimitive.content
        val key = "$senderUserId/${conversationId.orEmpty()}"
        val known = lists.getOrPut(key) {
            try {
                Known(verifiedDevices(device, senderUserId, conversationId).devices, null)
            } catch (e: EngineException) {
                // A device waiting for its link has no list of its own yet: the link message carries and checks one.
                if (e.code == "not_linked" && senderUserId == device.userId) {
                    Known(device.api.deviceList(device.userId).deviceList?.get("devices")?.jsonArray?.map { it.jsonObject }.orEmpty(), null)
                } else {
                    Known(emptyList(), e.code)
                }
            }
        }
        val sender = known.devices.firstOrNull { it["deviceId"]?.jsonPrimitive?.contentOrNull == senderDeviceId }
        val response: EngineResponse? = sender?.let {
            runCatching {
                device.op("decrypt", "sender" to it, "message" to Json.parseToJsonElement(item["body"]!!.jsonPrimitive.content))
            }.getOrElse { EngineResponse(JsonObject(mapOf("ok" to kotlinx.serialization.json.JsonPrimitive(false), "error" to kotlinx.serialization.json.JsonPrimitive("decrypt_failed")))) }
        }
        val error = if (response != null) (if (response.ok) null else response.error ?: "decrypt_failed") else (known.error ?: "unknown_sender")
        if (error == null || error !in HOLD) handled += id
        out += Received(
            id, senderUserId, senderDeviceId, conversationId,
            kind = response?.json?.get("kind")?.jsonPrimitive?.contentOrNull ?: "message",
            text = if (response?.ok == true) response.json["plaintext"]?.jsonPrimitive?.contentOrNull else null,
            error = error,
        )
    }
    if (handled.isNotEmpty()) device.api.ack(device.deviceId, handled)
    return out
}
