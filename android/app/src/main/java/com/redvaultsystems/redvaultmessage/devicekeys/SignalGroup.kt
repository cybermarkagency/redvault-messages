// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

/**
 * Signal groups on Android (M6, docs/signal-integration/03-group-design.md §6). The engine
 * needs the `signal` feature (on unless built with -PredvaultE2eSignal=false).
 *
 * The flow:
 * - The group's signed roster is brought in line with its active members: the first epoch, or the
 *   next one when someone joined or left. Any member signs it; a concurrent change by another
 *   member wins, and this phone tries once more on top of it.
 * - **B1:** the group goes over Signal only when this phone and every other member device speak
 *   Signal. Otherwise it stays on Megolm.
 * - This phone's sender key is sealed to each member device inside its Signal session, and the
 *   message is then encrypted once and fanned out.
 */

private fun JsonObject.str(name: String): String? = this[name]?.jsonPrimitive?.contentOrNull

/** Verifies the group's stored roster chain. Null when the group has none yet. */
suspend fun verifyRosters(device: V3Device, groupId: String): EngineResponse? {
    val chain = device.api.rosters(groupId, 0)
    if (chain.isEmpty()) return null
    return device.must("rosterVerify", "groupId" to groupId, "chain" to JsonArray(chain))
}

/** Makes the roster match [members] (account ids), signing the first or next epoch when it doesn't. */
suspend fun syncRoster(device: V3Device, groupId: String, members: List<String>) {
    val sorted = members.distinct().sorted()
    for (attempt in 1..2) {
        val verified = verifyRosters(device, groupId)
        val current = verified?.array("members")?.map { it.jsonPrimitive.content }
        val made = when {
            verified == null -> device.must("rosterCreate", "groupId" to groupId, "members" to JsonArray(sorted.map(::JsonPrimitive)))
            current != sorted -> device.must("rosterUpdate", "groupId" to groupId, "members" to JsonArray(sorted.map(::JsonPrimitive)))
            else -> return
        }
        try {
            device.api.postRoster(groupId, made.obj("roster"))
            return
        } catch (e: ApiException) {
            if (e.code != "stale_roster" || attempt == 2) throw e
        }
    }
}

/** B1: whether this group's messages go over Signal (this phone and every other member device speak it). */
fun groupUsesSignal(device: V3Device, members: List<JsonObject>): Boolean {
    if (!engineHasSignal(device)) return false
    val others = members.filter { it.str("deviceId") != device.deviceId }
    if (others.isEmpty()) return false
    val answer = device.op("chatProtocols", "recipients" to buildJsonArray { others.forEach { add(buildJsonObject { put("identity", it) }) } })
    if (!answer.ok || answer.bool("thisDeviceSignal") != true) return false
    return answer.array("protocols").all { it.jsonObject.str("protocol") == "rv-signal-v1" }
}

/** One Signal group send: roster, sender key to the devices that lack it, then one fan-out. Returns the devices reached. */
suspend fun sendSignalGroup(device: V3Device, groupId: String, plaintext: String, members: List<JsonObject>): Int {
    syncRoster(device, groupId, members.mapNotNull { it.str("userId") })
    val others = members.filter { it.str("deviceId") != device.deviceId }
    fun recipients(entries: List<Pair<JsonObject, JsonObject?>>) = buildJsonArray {
        for ((identity, claimed) in entries) add(buildJsonObject {
            put("identity", identity)
            if (claimed != null) put("signalBundle", buildJsonObject {
                put("certificate", claimed["certificate"] ?: JsonNull)
                put("bundle", claimed["bundle"] ?: JsonNull)
            })
        })
    }
    val first = device.must("groupSignalShare", "groupId" to groupId, "recipients" to recipients(others.map { it to null })).array("results").map { it.jsonObject }.toMutableList()
    val need = first.indices.filter { first[it].str("error") == "no_signal_session" }
    if (need.isNotEmpty()) {
        val claimed = device.api.signalClaim(groupId, need.map { others[it].str("userId")!! to others[it].str("deviceId")!! })
        val again = device.must("groupSignalShare", "groupId" to groupId, "recipients" to recipients(need.mapIndexed { k, i -> others[i] to claimed[k] })).array("results")
        need.forEachIndexed { k, i -> first[i] = again[k].jsonObject }
    }
    val keyMessages = ArrayList<Triple<String, String, String>>()
    first.forEachIndexed { i, r ->
        val error = r.str("error")
        if (error != null) throw EngineException(error, r.str("message") ?: "a member device can't get the group key")
        r["message"]?.let { keyMessages += Triple(others[i].str("userId")!!, others[i].str("deviceId")!!, it.toString()) }
    }
    keyMessages.chunked(ChatLimits.MAX_RECIPIENTS).forEach { device.api.sendMessages(device.deviceId, groupId, it) }
    val sealed = device.must("groupSignalEncrypt", "groupId" to groupId, "plaintext" to plaintext).obj("message")
    device.api.sendGroupMessage(device.deviceId, groupId, sealed.toString(), others.map { it.str("userId")!! to it.str("deviceId")!! })
    return others.size
}
