// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.delay
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonPrimitive

/**
 * Linking a device by QR code (docs/multi-device-threat-model.md): for two phones that can see each other. The
 * new device shows a code that carries its identity, a signed one-time key and a secret nonce; a trusted device
 * scans it with its camera, adds exactly that device to the signed list and sends it the account key. Nobody
 * compares numbers, because the code only ever travelled by camera, not through the server.
 */

/** On the new device (already registered with startNewDevice): the text to show as a QR code. */
fun qrLinkOffer(device: V3Device): String = device.must("linkOffer").string("qrText")

/** On the new device: waits for the trusted device's link message, then publishes fresh keys. Returns the account's list. */
suspend fun waitForQrLink(device: V3Device, wait: WaitOptions = WaitOptions()): JsonObject {
    val until = System.currentTimeMillis() + wait.timeoutMs
    while (receive(device).none { it.kind == "link" && it.error == null }) {
        if (System.currentTimeMillis() > until) throw EngineException("pairing_timeout", "The trusted device didn't link this phone in time.")
        delay(wait.intervalMs)
    }
    device.publishKeys(10)
    return device.must("deviceList").obj("deviceList")
}

/** Whether [text] looks like a device-link code at all (not a safety-number code or something else). */
fun looksLikeLinkOffer(text: String): Boolean = runCatching {
    val bytes = java.util.Base64.getDecoder().decode(text.trim())
    val json = kotlinx.serialization.json.Json.parseToJsonElement(String(bytes, Charsets.UTF_8)) as JsonObject
    json["v"] != null && json["device"] != null && json["nonce"] != null
}.getOrDefault(false)

/**
 * On a trusted device: adds the scanned device to the list, uploads the list, and sends the new device the link
 * message. The engine refuses a code for another account or with a bad signature. Returns the new list.
 */
suspend fun approveScannedQr(trusted: V3Device, qrText: String): JsonObject {
    if (!looksLikeLinkOffer(qrText)) throw EngineException("not_a_link_code", "That isn't a device-link code.")
    val approved = publishList(trusted) { trusted.must("approveDevice", "qr" to qrText.trim(), "verifiedBy" to "qr") }
    val added = approved.obj("added")
    trusted.api.sendMessages(
        trusted.deviceId, null,
        listOf(Triple(added["userId"]!!.jsonPrimitive.content, added["deviceId"]!!.jsonPrimitive.content, approved.obj("message").toString())),
    )
    return approved.obj("deviceList")
}
