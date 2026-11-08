// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import com.redvaultsystems.redvaultmessage.data.api.ApiException
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.security.SecureRandom
import java.util.Base64

/**
 * This phone's protocol-3 device and what it does with the server (docs/multi-device-threat-model.md).
 * A port of ui/src/lib/e2eV3/device.ts. Used when the server has E2E_V3_ENABLED. Protocol 1 is untouched.
 *
 * Every engine call saves the new state before returning, so a crash never reuses a message key.
 */
class V3Device private constructor(
    val userId: String,
    val deviceId: String,
    private val storageKey: String,
    private var state: String,
    private val storage: DeviceStorage,
    private val engine: EngineRunner,
    val api: V3Api,
) {
    /** One engine call on this device's state. A failed response is returned, not thrown. */
    @Synchronized
    fun op(op: String, vararg fields: Pair<String, Any?>): EngineResponse {
        val response = engine.run(request(op, "state" to state, "pickleKey" to storageKey, *fields))
        val newState = response.state
        if (response.ok && newState != null) {
            storage.saveState(userId, deviceId, newState)
            state = newState
        }
        return response
    }

    fun must(op: String, vararg fields: Pair<String, Any?>): EngineResponse {
        val response = op(op, *fields)
        if (!response.ok) throw EngineException(response.error ?: "engine_error", response.message ?: op)
        return response
    }

    /** The key local chat history is encrypted under: derived from the storage key, never stored (9.7). */
    fun historyKey(): ByteArray {
        val mac = javax.crypto.Mac.getInstance("HmacSHA256")
        val raw = Base64.getDecoder().decode(storageKey)
        mac.init(javax.crypto.spec.SecretKeySpec(raw, "HmacSHA256"))
        raw.fill(0)
        return mac.doFinal("redvault-v3-history-v1".toByteArray())
    }

    /** The engine state now, to put back with [rollback] when a change this device signed never reached the server. */
    @Synchronized
    fun snapshot(): String = state

    /** Undoes local changes after [snapshot]: a list change that wasn't published must not stay in this device's state. */
    @Synchronized
    fun rollback(to: String) {
        storage.saveState(userId, deviceId, to)
        state = to
    }

    fun identity(): JsonObject = must("identity").obj("identity")

    /** Publishes this device's public keys: identity, signed one-time keys, the signed fallback key. */
    suspend fun publishKeys(count: Int = 20) {
        api.registerDevice(must("prekeys", "count" to count).obj("upload"))
    }

    /** Forgets this device on this phone and in the server's directory. */
    suspend fun forget() {
        runCatching { api.removeDevice(deviceId) }
        storage.wipe(userId)
    }

    /** What protects the key wrapping this device's storage key: "hardware", "software", "unknown" or "none". */
    fun protection(): String = storage.protection(userId)

    companion object {
        /** The device this phone keeps for [userId], if any. */
        fun open(userId: String, storage: DeviceStorage, engine: EngineRunner, api: V3Api): V3Device? {
            val stored = storage.load(userId) ?: return null
            val state = stored.state ?: return null
            return V3Device(userId, stored.deviceId, stored.storageKey, state, storage, engine, api)
        }

        /** Makes the engine state for a new device record (createDevice or createAccount) and keeps it. */
        fun create(
            userId: String,
            op: String,
            storage: DeviceStorage,
            engine: EngineRunner,
            api: V3Api,
            vararg fields: Pair<String, Any?>,
        ): Pair<V3Device, EngineResponse> {
            val stored = storage.create(userId)
            val response = engine.run(request(op, "userId" to userId, "deviceId" to stored.deviceId, "pickleKey" to stored.storageKey, *fields))
            val state = response.state
            if (!response.ok || state == null) {
                storage.wipe(userId)
                throw EngineException(response.error ?: "engine_error", response.message ?: "the engine refused")
            }
            storage.saveState(userId, stored.deviceId, state)
            return V3Device(userId, stored.deviceId, stored.storageKey, state, storage, engine, api) to response
        }
    }
}

private fun b64(bytes: ByteArray): String = Base64.getEncoder().encodeToString(bytes)

/**
 * Moves an existing account to per-device identities from this phone: a new account key, certified by
 * the account's legacy signing key so contacts who pinned that key accept it. The legacy key stays where
 * it is (protocol 1 keeps working). If any step fails, everything this did is undone.
 */
suspend fun setUpFirstDevice(userId: String, legacyPrivateKey: ByteArray, storage: DeviceStorage, engine: EngineRunner, api: V3Api): V3Device {
    val (device, response) = V3Device.create(userId, "createAccount", storage, engine, api, "legacyAccountKey" to b64(legacyPrivateKey))
    try {
        device.publishKeys()
        api.putDeviceList(response.obj("deviceList"))
        return device
    } catch (e: Throwable) {
        device.forget()
        throw e
    }
}

/** A phone joining an account that already has trusted devices: its own keys, waiting to be linked. */
suspend fun startNewDevice(userId: String, storage: DeviceStorage, engine: EngineRunner, api: V3Api): V3Device {
    val (device, _) = V3Device.create(userId, "createDevice", storage, engine, api)
    try {
        device.publishKeys()
    } catch (e: Throwable) {
        device.forget()
        throw e
    }
    return device
}

/**
 * Puts a list this device signed. If another trusted device changed the list first, adopts that list
 * (it's signed by the same account key) and redoes the change once on top of it.
 */
suspend fun publishList(device: V3Device, rotationProof: String? = null, change: () -> EngineResponse): EngineResponse {
    // A change is local until the server has it: if the upload fails, this device goes back to where it was
    // (otherwise its own list would run ahead of the server's and a retry would find the change already made).
    var before = device.snapshot()
    var made = change()
    try {
        try {
            device.api.putDeviceList(made.obj("deviceList"), rotationProof)
        } catch (e: ApiException) {
            if (e.code != "stale_device_list") throw e
            device.rollback(before)
            val current = device.api.deviceList(device.userId).deviceList
            device.must("verifyDeviceList", "deviceList" to current)
            before = device.snapshot()
            made = change()
            device.api.putDeviceList(made.obj("deviceList"), rotationProof)
        }
    } catch (e: Throwable) {
        device.rollback(before)
        throw e
    }
    return made
}

/** Removes another device of this account from the signed list; contacts are told by the next list they check. */
suspend fun revokeDevice(device: V3Device, deviceId: String) {
    publishList(device) { device.must("revokeDevice", "deviceId" to deviceId) }
    runCatching { device.api.removeDevice(deviceId) }
}

// --- Recovery ---

/** The recovery key as people see it: 64 hex digits in groups of four. */
fun formatRecoveryKey(bytes: ByteArray): String =
    bytes.joinToString("") { "%02X".format(it) }.chunked(4).joinToString("-")

fun parseRecoveryKey(text: String): ByteArray? {
    val digits = text.filter { it in '0'..'9' || it in 'a'..'f' || it in 'A'..'F' }
    if (digits.length != 64) return null
    return ByteArray(32) { digits.substring(it * 2, it * 2 + 2).toInt(16).toByte() }
}

/**
 * A new recovery key and the account key (and legacy key) encrypted under it, stored on the server,
 * which never sees the key. Returns the key, to show once; nothing on this phone keeps it.
 */
suspend fun createRecoveryBackup(device: V3Device): String {
    val key = ByteArray(32).also { SecureRandom().nextBytes(it) }
    try {
        val made = device.must("exportAccountBackup", "recoveryKey" to b64(key))
        device.api.putRecoveryBackup(made.string("backup"), made.string("signature"))
        return formatRecoveryKey(key)
    } finally {
        key.fill(0)
    }
}

/** With no other device left: a new device on this phone, restored from the recovery backup. */
suspend fun restoreWithRecoveryKey(userId: String, recoveryKeyText: String, storage: DeviceStorage, engine: EngineRunner, api: V3Api): V3Device {
    val key = parseRecoveryKey(recoveryKeyText) ?: throw EngineException("bad_recovery_key", "A recovery key has 64 letters and digits.")
    try {
        val backup = api.recoveryBackup() ?: throw EngineException("no_recovery_backup", "This account has no recovery backup.")
        val current = api.deviceList(userId).deviceList
        val device = startNewDevice(userId, storage, engine, api)
        try {
            val restored = device.must("restoreAccountBackup", "backup" to backup, "recoveryKey" to b64(key), "deviceList" to current)
            api.putDeviceList(restored.obj("deviceList"))
            return device
        } catch (e: Throwable) {
            device.forget()
            throw e
        }
    } finally {
        key.fill(0)
    }
}

/**
 * The safety number over this account's and the contact's account keys (Signal's numeric fingerprint
 * construction, crypto-core/src/identity.rs), or null while the contact has no device keys. Comparing it
 * proves both apps see the same keys; it says nothing about who the person is.
 */
suspend fun deviceKeySafetyNumber(device: V3Device, contactUserId: String, conversationId: String? = null): List<String>? {
    val view = device.api.deviceList(contactUserId, conversationId)
    val list = view.deviceList ?: return null
    val verified = device.op("verifyDeviceList", "deviceList" to list)
    if (!verified.ok) throw EngineException(verified.error ?: "bad_device_list", verified.message ?: "the device list doesn't verify")
    if (verified.array("devices").isEmpty()) return null
    return device.must("fingerprint", "userId" to contactUserId).array("safetyNumber").map { it.jsonPrimitive.content }
}

/** Plain strings out of an engine array. */
internal fun JsonArray.strings(): List<String> = map { it.jsonPrimitive.content }

internal fun JsonElement?.text(): String? = (this as? JsonPrimitive)?.contentOrNull

internal fun JsonObject.objects(name: String): List<JsonObject> = this[name]?.jsonArray?.map { it.jsonObject }.orEmpty()
