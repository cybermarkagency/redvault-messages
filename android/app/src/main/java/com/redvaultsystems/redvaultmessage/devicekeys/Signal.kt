// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

/**
 * rv-signal-v1 on Android (docs/signal-integration/): libsignal's PQXDH and Triple Ratchet,
 * between protocol-3 devices that both speak it: the default (docs/signal-integration/07-step1-default-path.md).
 * Builds link libsignal (AGPL-3.0-only) unless made with -PredvaultE2eSignal=false.
 *
 * The engine picks the protocol per device by itself (encryptMany, decrypt). This file only
 * keeps this phone's Signal keys and its declaration in the signed list in order.
 */

/** Whether this build's engine has Signal sessions (crypto-core built with the `signal` feature). */
fun engineHasSignal(device: V3Device): Boolean = device.op("version").json["signal"] is JsonObject

/**
 * Turns Signal on for this device once, declares its certificate in the account's signed
 * list (every linked device holds the account key), and keeps at least [low] unused one-time
 * keys of each kind on the server. Returns "off", "enabled", "declared" or "ok". Safe to call
 * on every refresh.
 */
suspend fun maintainSignal(device: V3Device, low: Int = 10): String {
    if (!engineHasSignal(device) || !device.api.signalEnabled()) return "off"
    // Turned off for this device (its capability left the signed list): stays off until the person turns it on again.
    if (device.must("signalStatus").bool("disabled") == true) return "off"
    var outcome = "ok"
    val status = device.api.signalStatus(device.deviceId)
    val certificate = device.must("signalEnable").obj("certificate")
    val enabled = status["enabled"]?.jsonPrimitive?.contentOrNull == "true"
    val preKeys = status["preKeys"]?.jsonPrimitive?.intOrNull ?: 0
    val kyberPreKeys = status["kyberPreKeys"]?.jsonPrimitive?.intOrNull ?: 0
    if (!enabled || preKeys < low || kyberPreKeys < low) {
        device.api.signalUpload(device.deviceId, device.must("signalPrekeys", "count" to 20).obj("upload"))
        if (!enabled) outcome = "enabled"
    }
    val own = device.op("deviceList")
    if (own.ok && own.bool("trusted") == true) {
        val declared = own.obj("deviceList")["capabilities"]?.jsonArray.orEmpty()
            .any { it.jsonObject["deviceId"]?.jsonPrimitive?.contentOrNull == device.deviceId }
        if (!declared) {
            publishList(device) { device.must("setSignalCapability", "certificate" to certificate) }
            if (outcome == "ok") outcome = "declared"
        }
    }
    return outcome
}

/**
 * Which protocol a one-to-one chat's messages use (M5), worked out by the engine from the verified lists:
 * "signal" (every device: the contact's and this account's other devices), "olm" (none), "mixed" (some),
 * or null while the contact has no device keys.
 */
suspend fun chatProtocol(device: V3Device, conversationId: String, contactUserId: String): String? {
    val theirs = verifiedDevices(device, contactUserId, conversationId).devices
    if (theirs.isEmpty()) return null
    val mine = verifiedDevices(device, device.userId, null).devices.filter { it["deviceId"]?.jsonPrimitive?.contentOrNull != device.deviceId }
    val recipients = kotlinx.serialization.json.buildJsonArray {
        for (d in theirs + mine) add(kotlinx.serialization.json.buildJsonObject { put("identity", d) })
    }
    val protocols = device.must("chatProtocols", "recipients" to recipients).array("protocols").map { it.jsonObject }
    val signal = protocols.count { it["protocol"]?.jsonPrimitive?.contentOrNull == "rv-signal-v1" }
    return when (signal) {
        0 -> "olm"
        protocols.size -> "signal"
        else -> "mixed"
    }
}

/** Turns Signal off for one of this account's devices, in the signed list (needs the account key). Contacts are told. */
suspend fun turnOffSignal(device: V3Device, deviceId: String) {
    publishList(device) { device.must("clearSignalCapability", "deviceId" to deviceId) }
}
