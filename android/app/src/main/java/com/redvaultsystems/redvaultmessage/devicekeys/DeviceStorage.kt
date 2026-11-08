// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

package com.redvaultsystems.redvaultmessage.devicekeys

import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyInfo
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.nio.ByteBuffer
import java.security.KeyStore
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

/** A device's storage key (base64, handed to the engine with each call) and its encrypted state. */
class OpenDevice(val deviceId: String, val storageKey: String, val state: String?)

/** Wraps a 32-byte secret under a key that never leaves its protection (docs/multi-device-threat-model.md section 5). */
interface KeyWrapper {
    class Wrapped(val iv: ByteArray, val ciphertext: ByteArray)

    fun wrap(alias: String, secret: ByteArray, aad: ByteArray): Wrapped
    fun unwrap(alias: String, wrapped: Wrapped, aad: ByteArray): ByteArray
    fun delete(alias: String)

    /** What protects the wrapping key, for the screen: "hardware" (StrongBox or TEE), "software", "unknown" or "none". */
    fun protection(alias: String): String
}

/**
 * An Android Keystore AES-256-GCM key, in StrongBox where the phone has it and otherwise in the TEE.
 * Not bound to user authentication, so messages can arrive in the background; the app lock guards the
 * interface. A key that can't be used any more reads as a lost device, not a crash.
 */
class AndroidKeystoreWrapper : KeyWrapper {
    private val store: KeyStore = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }

    private fun key(alias: String, create: Boolean): SecretKey? {
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        if (!create) return null
        for (strongBox in listOf(true, false)) {
            if (strongBox && Build.VERSION.SDK_INT < Build.VERSION_CODES.P) continue
            // The rollback counter's key is used on every save and guards no secret, only the order of saves: the TEE
            // is enough there, and StrongBox is slow.
            if (strongBox && alias.startsWith(DeviceStorage.COUNTER_ALIAS)) continue
            try {
                val spec = KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .apply { if (strongBox) setIsStrongBoxBacked(true) }
                    .build()
                return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore").run {
                    init(spec)
                    generateKey()
                }
            } catch (_: StrongBoxUnavailableException) {
                // No StrongBox on this phone: the TEE-backed key is made on the next pass.
            }
        }
        error("the Keystore couldn't make a key")
    }

    override fun wrap(alias: String, secret: ByteArray, aad: ByteArray): KeyWrapper.Wrapped {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key(alias, create = true)!!)
        cipher.updateAAD(aad)
        return KeyWrapper.Wrapped(cipher.iv, cipher.doFinal(secret))
    }

    override fun unwrap(alias: String, wrapped: KeyWrapper.Wrapped, aad: ByteArray): ByteArray {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(alias, create = false) ?: error("the wrapping key is gone"), GCMParameterSpec(128, wrapped.iv))
        cipher.updateAAD(aad)
        return cipher.doFinal(wrapped.ciphertext)
    }

    override fun delete(alias: String) {
        runCatching { store.deleteEntry(alias) }
    }

    override fun protection(alias: String): String = runCatching {
        val key = key(alias, create = false) ?: return "none"
        val info = SecretKeyFactory.getInstance(key.algorithm, "AndroidKeyStore").getKeySpec(key, KeyInfo::class.java) as KeyInfo
        val hardware = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            info.securityLevel != KeyProperties.SECURITY_LEVEL_SOFTWARE
        } else {
            @Suppress("DEPRECATION")
            info.isInsideSecureHardware
        }
        if (hardware) "hardware" else "software"
    }.getOrDefault("unknown")
}

/** Something about this phone's device keys that only this phone knows, never sent anywhere. */
data class LocalDeviceEvent(val atMillis: Long, val kind: String)

/**
 * This phone's protocol-3 device for each account (section 5): the engine's encrypted state in the app's
 * no-backup storage, and the 32-byte storage key it is encrypted under, wrapped by a [KeyWrapper].
 * None of it ever goes to the server. Losing the Keystore key removes the device: it must be linked
 * again, which loses its protocol-3 history (other devices and the recovery key are unaffected).
 *
 * Rollback detection (docs/signal-integration/07-step1-default-path.md section 4): every save stores
 * counter = previous + 1 beside the state, bound to the state, the account and the device by an HMAC under
 * a key derived from the storage key, and the same value in a second record wrapped by its own Keystore key
 * ([COUNTER_ALIAS]) in another file. A state older than that record, or one whose HMAC doesn't check, is
 * refused: the phone reads as not linked and keeps a [LocalDeviceEvent]. Restoring both files and the
 * Keystore together (a full-disk image, root) isn't detected: that would need a hardware counter.
 */
class DeviceStorage(private val dir: File, private val wrapper: KeyWrapper) {
    @Serializable
    private class Stored(
        val deviceId: String,
        val iv: String,
        val wrapped: String,
        val state: String? = null,
        /** How many times the state was saved. 0 without [mac]: never saved, or last saved before the counter existed. */
        val counter: Long = 0,
        /** HMAC-SHA256 over the account, device, counter and state. */
        val mac: String? = null,
    )

    @Serializable
    private class StoredCounter(val iv: String, val wrapped: String)

    private val json = Json { ignoreUnknownKeys = true }
    private val base64 = Base64.getEncoder()
    private val unbase64 = Base64.getDecoder()

    // The HMAC key of each account's open device (device id to key), so a save doesn't unwrap the storage key again. Memory only.
    private val macKeys = HashMap<String, Pair<String, ByteArray>>()

    private fun safe(userId: String) = userId.filter { it.isLetterOrDigit() || it == '-' }
    private fun file(userId: String) = File(dir, "v3-" + safe(userId) + ".json")
    private fun counterFile(userId: String) = File(dir, "v3-counter-" + safe(userId) + ".json")
    private fun eventsFile(userId: String) = File(dir, "v3-events-" + safe(userId) + ".txt")
    private fun alias(userId: String) = "redvault.v3." + userId.lowercase()
    private fun counterAlias(userId: String) = COUNTER_ALIAS + userId.lowercase()
    private fun aad(userId: String, deviceId: String) = "redvault-v3-storage-key\u0000$userId\u0000$deviceId".toByteArray()
    private fun counterAad(userId: String, deviceId: String) = "redvault-v3-counter\u0000$userId\u0000$deviceId".toByteArray()

    private fun writeFile(target: File, text: String) {
        dir.mkdirs()
        val temp = File(dir, target.name + ".tmp")
        temp.writeText(text)
        if (!temp.renameTo(target)) {
            target.delete()
            check(temp.renameTo(target)) { "couldn't save the device" }
        }
    }

    private fun write(userId: String, stored: Stored) = writeFile(file(userId), json.encodeToString(Stored.serializer(), stored))

    private fun read(userId: String): Stored? =
        file(userId).takeIf { it.exists() }?.let { runCatching { json.decodeFromString(Stored.serializer(), it.readText()) }.getOrNull() }

    private fun unwrapKey(userId: String, stored: Stored): ByteArray =
        wrapper.unwrap(alias(userId), KeyWrapper.Wrapped(unbase64.decode(stored.iv), unbase64.decode(stored.wrapped)), aad(userId, stored.deviceId))

    private fun macKey(storageKey: ByteArray): ByteArray = hmac(storageKey, "redvault-v3-state-counter-v1".toByteArray())

    private fun stateMac(key: ByteArray, userId: String, deviceId: String, counter: Long, state: String?): ByteArray =
        hmac(key, "$userId\u0000$deviceId\u0000$counter\u0000".toByteArray() + (state ?: "").toByteArray())

    /** The second record's value: null when there is none, -1 when it's there but can't be read. */
    private fun readCounter(userId: String, deviceId: String): Long? {
        val f = counterFile(userId)
        if (!f.exists()) return null
        return runCatching {
            val stored = json.decodeFromString(StoredCounter.serializer(), f.readText())
            val raw = wrapper.unwrap(counterAlias(userId), KeyWrapper.Wrapped(unbase64.decode(stored.iv), unbase64.decode(stored.wrapped)), counterAad(userId, deviceId))
            check(raw.size == 8)
            ByteBuffer.wrap(raw).long
        }.getOrDefault(-1L)
    }

    private fun writeCounter(userId: String, deviceId: String, value: Long) {
        val wrapped = wrapper.wrap(counterAlias(userId), ByteBuffer.allocate(8).putLong(value).array(), counterAad(userId, deviceId))
        writeFile(counterFile(userId), json.encodeToString(StoredCounter.serializer(), StoredCounter(base64.encodeToString(wrapped.iv), base64.encodeToString(wrapped.ciphertext))))
    }

    private fun dropCounter(userId: String) {
        counterFile(userId).delete()
        wrapper.delete(counterAlias(userId))
    }

    /** The state on disk is older than the last one saved here (or was changed): it's never used, and the phone says why. */
    private fun refuse(userId: String) {
        macKeys.remove(userId)
        runCatching {
            dir.mkdirs()
            eventsFile(userId).appendText("${System.currentTimeMillis()}\t$RESTORED_OLDER_COPY\n")
        }
    }

    /** A new device for [userId]: a random id and storage key, wrapped. Replaces any device kept for the account. */
    @Synchronized
    fun create(userId: String): OpenDevice {
        val deviceId = UUID.randomUUID().toString()
        val storageKey = ByteArray(32).also { SecureRandom().nextBytes(it) }
        wrapper.delete(alias(userId))
        dropCounter(userId)
        val wrapped = wrapper.wrap(alias(userId), storageKey, aad(userId, deviceId))
        write(userId, Stored(deviceId, base64.encodeToString(wrapped.iv), base64.encodeToString(wrapped.ciphertext)))
        writeCounter(userId, deviceId, 0)
        macKeys[userId] = deviceId to macKey(storageKey)
        val out = OpenDevice(deviceId, base64.encodeToString(storageKey), null)
        storageKey.fill(0)
        return out
    }

    /**
     * This phone's device for [userId], or null when it has none, its key can't be used any more, or its state is
     * an older copy than the last one saved here (then a [LocalDeviceEvent] is kept, and it must be linked again).
     */
    @Synchronized
    fun load(userId: String): OpenDevice? {
        val stored = read(userId) ?: return null
        val raw = runCatching { unwrapKey(userId, stored) }.getOrNull() ?: return null
        try {
            val key = macKey(raw)
            val mac = stored.mac
            val authentic = if (mac == null) {
                stored.counter == 0L
            } else {
                runCatching { MessageDigest.isEqual(unbase64.decode(mac), stateMac(key, userId, stored.deviceId, stored.counter, stored.state)) }.getOrDefault(false)
            }
            val mirror = readCounter(userId, stored.deviceId)
            // A state saved with the counter always has its second record (it's made with the device). Missing, it
            // was deleted: accepting it would let an older state in with it, so only a state from before the counter
            // (no mac) may start a record now.
            val mirrorDeleted = mirror == null && mac != null
            if (!authentic || mirror == -1L || mirrorDeleted || (mirror != null && stored.counter < mirror)) {
                refuse(userId)
                return null
            }
            // No second record yet (the first run with the counter), or the app stopped between the two writes: catch up.
            if (mirror == null || stored.counter > mirror) writeCounter(userId, stored.deviceId, stored.counter)
            macKeys[userId] = stored.deviceId to key
            return OpenDevice(stored.deviceId, base64.encodeToString(raw), stored.state)
        } finally {
            raw.fill(0)
        }
    }

    /** Saves the engine's new state, then the counter. Called before anything that call produced is sent. */
    @Synchronized
    fun saveState(userId: String, deviceId: String, state: String) {
        val stored = read(userId)
        check(stored != null && stored.deviceId == deviceId) { "this phone's device was replaced or removed" }
        val key = macKeys[userId]?.takeIf { it.first == deviceId }?.second ?: run {
            val raw = unwrapKey(userId, stored)
            macKey(raw).also {
                raw.fill(0)
                macKeys[userId] = deviceId to it
            }
        }
        val next = stored.counter + 1
        val mac = base64.encodeToString(stateMac(key, userId, deviceId, next, state))
        // Always in this order: a stop between the two leaves the state ahead of the counter, which load accepts.
        write(userId, Stored(stored.deviceId, stored.iv, stored.wrapped, state, next, mac))
        writeCounter(userId, deviceId, next)
    }

    @Synchronized
    fun wipe(userId: String) {
        macKeys.remove(userId)
        file(userId).delete()
        wrapper.delete(alias(userId))
        dropCounter(userId)
    }

    /** What happened to this phone's device keys that only this phone knows, oldest first. */
    @Synchronized
    fun localEvents(userId: String): List<LocalDeviceEvent> = runCatching {
        eventsFile(userId).takeIf { it.exists() }?.readLines().orEmpty().mapNotNull { line ->
            val parts = line.split('\t')
            parts[0].toLongOrNull()?.let { LocalDeviceEvent(it, parts.getOrElse(1) { "" }) }
        }
    }.getOrDefault(emptyList())

    @Synchronized
    fun clearLocalEvents(userId: String) {
        eventsFile(userId).delete()
    }

    /** A small yes/no kept for the account on this phone (a notice still to show). Nothing secret. */
    @Synchronized
    fun flag(userId: String, name: String): Boolean = flagFile(userId, name).exists()

    @Synchronized
    fun setFlag(userId: String, name: String, on: Boolean) {
        val f = flagFile(userId, name)
        if (on) {
            dir.mkdirs()
            f.writeText("1")
        } else {
            f.delete()
        }
    }

    private fun flagFile(userId: String, name: String) = File(dir, "v3-" + name.filter { it.isLetterOrDigit() || it == '-' } + "-" + safe(userId))

    fun protection(userId: String): String = wrapper.protection(alias(userId))

    companion object {
        /** The Keystore alias of the rollback counter's record is this plus the account: redvault.v3.counter.<user>. */
        const val COUNTER_ALIAS = "redvault.v3.counter."
        const val RESTORED_OLDER_COPY = "restored_older_copy"
    }
}

private fun hmac(key: ByteArray, data: ByteArray): ByteArray {
    val mac = Mac.getInstance("HmacSHA256")
    mac.init(SecretKeySpec(key, "HmacSHA256"))
    return mac.doFinal(data)
}
