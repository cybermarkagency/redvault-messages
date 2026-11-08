// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import kotlinx.coroutines.delay
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

/**
 * Pairing a new device with a trusted device of the same account by comparing codes
 * (crypto-core/src/pairing.rs: vodozemac's SAS with a key commitment), for when neither can scan the
 * other's QR code. The server only relays the messages; a relay that swaps keys makes the two screens
 * show different numbers, and the person stops. A port of ui/src/lib/e2eV3/pairing.ts.
 */
data class WaitOptions(val intervalMs: Long = 1000, val timeoutMs: Long = 10 * 60 * 1000)

private fun stateless(engine: EngineRunner, op: String, vararg fields: Pair<String, Any?>): EngineResponse = engine.must(op, *fields)

private suspend fun post(enrollmentId: String, from: V3Device, message: JsonObject) =
    from.api.postEnrollmentMessage(enrollmentId, from.deviceId, message.toString())

/** The first message of [type] the other device sent, polling until it comes. */
private suspend fun waitFor(enrollmentId: String, self: V3Device, type: String, wait: WaitOptions): JsonObject {
    val until = System.currentTimeMillis() + wait.timeoutMs
    while (true) {
        for (m in self.api.enrollmentMessages(enrollmentId)) {
            if (m.fromDeviceId == self.deviceId) continue
            val body = runCatching { Json.parseToJsonElement(m.body).jsonObject }.getOrNull() ?: continue
            if (body["type"]?.jsonPrimitive?.contentOrNull == type) return body
        }
        if (System.currentTimeMillis() > until) throw EngineException("pairing_timeout", "The other device didn't answer in time.")
        delay(wait.intervalMs)
    }
}

private fun JsonObject.field(name: String): JsonElement = this[name] ?: throw EngineException("bad_pairing_message", "a pairing message has no $name")

private fun decimals(response: EngineResponse): List<Int> = response.array("decimals").map { it.jsonPrimitive.content.toInt() }

/** On the new phone. */
class NewDevicePairing private constructor(
    val device: V3Device,
    val enrollmentId: String,
    private val sasId: String,
    private val engine: EngineRunner,
) {
    var trusted: JsonObject? = null
        private set

    /** Waits for a trusted device to answer; returns the numbers to compare with its screen. */
    suspend fun codes(wait: WaitOptions = WaitOptions()): List<Int> {
        val key = waitFor(enrollmentId, device, "key", wait)
        val revealed = stateless(engine, "sasReveal", "sasId" to sasId)
        post(enrollmentId, device, buildJsonObject { put("type", "reveal"); put("publicKey", revealed.string("publicKey")) })
        val trustedIdentity = key.field("identity")
        val established = stateless(
            engine, "sasEstablish",
            "sasId" to sasId, "theirPublicKey" to key.field("publicKey"), "newDevice" to device.identity(), "trustedDevice" to trustedIdentity,
        )
        trusted = trustedIdentity.jsonObject
        return decimals(established)
    }

    /** The person said the numbers match. Returns the account's list once the trusted device has linked this phone. */
    suspend fun confirm(wait: WaitOptions = WaitOptions()): JsonObject {
        val ours = device.must("sasMac", "sasId" to sasId)
        post(enrollmentId, device, buildJsonObject { put("type", "mac"); put("mac", ours.string("mac")) })
        val theirs = waitFor(enrollmentId, device, "mac", wait)
        device.must("sasConfirm", "sasId" to sasId, "mac" to theirs.field("mac"))
        val offer = device.must("linkOffer")
        post(enrollmentId, device, buildJsonObject { put("type", "offer"); put("qr", offer.string("qrText")) })
        val until = System.currentTimeMillis() + wait.timeoutMs
        while (receive(device).none { it.kind == "link" && it.error == null }) {
            if (System.currentTimeMillis() > until) throw EngineException("pairing_timeout", "The trusted device didn't link this phone in time.")
            delay(wait.intervalMs)
        }
        device.publishKeys(10)
        runCatching { device.api.deleteEnrollment(enrollmentId) }
        return device.must("deviceList").obj("deviceList")
    }

    /** The numbers didn't match, or the person gave up. */
    suspend fun cancel() {
        runCatching { device.api.deleteEnrollment(enrollmentId) }
        device.op("cancelLink")
    }

    companion object {
        /** Asks the account's trusted devices to pair; they're told and show the request. */
        suspend fun begin(device: V3Device, engine: EngineRunner): NewDevicePairing {
            val enrollment = device.api.createEnrollment(device.deviceId)
            val started = stateless(engine, "sasStart", "pairingId" to enrollment.id, "role" to "new")
            post(
                enrollment.id, device,
                buildJsonObject { put("type", "commit"); put("commitment", started.string("commitment")); put("identity", device.identity()) },
            )
            return NewDevicePairing(device, enrollment.id, started.string("sasId"), engine)
        }
    }
}

/** On a trusted phone, answering a pairing request. */
class TrustedPairing private constructor(
    val device: V3Device,
    val enrollmentId: String,
    private val sasId: String,
    val newDevice: JsonObject,
) {
    val newDeviceId: String get() = newDevice["deviceId"]?.jsonPrimitive?.contentOrNull.orEmpty()

    /** The person said the numbers match: approves exactly the device compared with, signs the new list and sends the link. */
    suspend fun confirm(wait: WaitOptions = WaitOptions()): JsonObject {
        val ours = device.must("sasMac", "sasId" to sasId)
        post(enrollmentId, device, buildJsonObject { put("type", "mac"); put("mac", ours.string("mac")) })
        val theirs = waitFor(enrollmentId, device, "mac", wait)
        device.must("sasConfirm", "sasId" to sasId, "mac" to theirs.field("mac"))
        val offer = waitFor(enrollmentId, device, "offer", wait)
        val approved = publishList(device) { device.must("approveDevice", "qr" to offer.field("qr"), "verifiedBy" to "sas") }
        device.api.sendMessages(
            device.deviceId, null,
            listOf(Triple(newDevice["userId"]!!.jsonPrimitive.content, newDeviceId, approved.obj("message").toString())),
        )
        return approved.obj("deviceList")
    }

    /** The numbers didn't match: nothing is approved. */
    suspend fun reject() {
        runCatching { device.api.deleteEnrollment(enrollmentId) }
    }

    class Begun(val pairing: TrustedPairing, val codes: List<Int>)

    companion object {
        /** Answers the request; returns the pairing and the numbers to compare with the new device's screen. */
        suspend fun begin(device: V3Device, engine: EngineRunner, enrollmentId: String, wait: WaitOptions = WaitOptions()): Begun {
            val commit = waitFor(enrollmentId, device, "commit", wait)
            val started = stateless(engine, "sasStart", "pairingId" to enrollmentId, "role" to "trusted")
            val identity = device.identity()
            post(enrollmentId, device, buildJsonObject { put("type", "key"); put("publicKey", started.string("publicKey")); put("identity", identity) })
            val reveal = waitFor(enrollmentId, device, "reveal", wait)
            val established = stateless(
                engine, "sasEstablish",
                "sasId" to started.string("sasId"), "theirPublicKey" to reveal.field("publicKey"), "theirCommitment" to commit.field("commitment"),
                "newDevice" to commit.field("identity"), "trustedDevice" to identity,
            )
            return Begun(TrustedPairing(device, enrollmentId, started.string("sasId"), commit.field("identity").jsonObject), decimals(established))
        }
    }
}
