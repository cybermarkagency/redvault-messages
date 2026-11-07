package com.redvaultsystems.redvaultmessage.devicekeys

import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyInfo
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.security.KeyStore
import java.security.SecureRandom
import java.util.Base64
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec

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

/**
 * This phone's protocol-3 device for each account (section 5): the engine's encrypted state in the app's
 * no-backup storage, and the 32-byte storage key it is encrypted under, wrapped by a [KeyWrapper].
 * None of it ever goes to the server. Losing the Keystore key removes the device: it must be linked
 * again, which loses its protocol-3 history (other devices and the recovery key are unaffected).
 */
class DeviceStorage(private val dir: File, private val wrapper: KeyWrapper) {
    @Serializable
    private class Stored(val deviceId: String, val iv: String, val wrapped: String, val state: String? = null)

    private val json = Json { ignoreUnknownKeys = true }
    private val base64 = Base64.getEncoder()
    private val unbase64 = Base64.getDecoder()

    private fun file(userId: String) = File(dir, "v3-" + userId.filter { it.isLetterOrDigit() || it == '-' } + ".json")
    private fun alias(userId: String) = "redvault.v3." + userId.lowercase()
    private fun aad(userId: String, deviceId: String) = "redvault-v3-storage-key\u0000$userId\u0000$deviceId".toByteArray()

    private fun write(userId: String, stored: Stored) {
        dir.mkdirs()
        val target = file(userId)
        val temp = File(dir, target.name + ".tmp")
        temp.writeText(json.encodeToString(Stored.serializer(), stored))
        if (!temp.renameTo(target)) {
            target.delete()
            check(temp.renameTo(target)) { "couldn't save the device" }
        }
    }

    private fun read(userId: String): Stored? =
        file(userId).takeIf { it.exists() }?.let { runCatching { json.decodeFromString(Stored.serializer(), it.readText()) }.getOrNull() }

    /** A new device for [userId]: a random id and storage key, wrapped. Replaces any device kept for the account. */
    @Synchronized
    fun create(userId: String): OpenDevice {
        val deviceId = UUID.randomUUID().toString()
        val storageKey = ByteArray(32).also { SecureRandom().nextBytes(it) }
        wrapper.delete(alias(userId))
        val wrapped = wrapper.wrap(alias(userId), storageKey, aad(userId, deviceId))
        write(userId, Stored(deviceId, base64.encodeToString(wrapped.iv), base64.encodeToString(wrapped.ciphertext)))
        val out = OpenDevice(deviceId, base64.encodeToString(storageKey), null)
        storageKey.fill(0)
        return out
    }

    /** This phone's device for [userId], or null when it has none (or its key can't be used any more). */
    @Synchronized
    fun load(userId: String): OpenDevice? {
        val stored = read(userId) ?: return null
        return runCatching {
            val raw = wrapper.unwrap(alias(userId), KeyWrapper.Wrapped(unbase64.decode(stored.iv), unbase64.decode(stored.wrapped)), aad(userId, stored.deviceId))
            val key = base64.encodeToString(raw)
            raw.fill(0)
            OpenDevice(stored.deviceId, key, stored.state)
        }.getOrNull()
    }

    /** Saves the engine's new state. Called before anything that call produced is sent. */
    @Synchronized
    fun saveState(userId: String, deviceId: String, state: String) {
        val stored = read(userId)
        check(stored != null && stored.deviceId == deviceId) { "this phone's device was replaced or removed" }
        write(userId, Stored(stored.deviceId, stored.iv, stored.wrapped, state))
    }

    @Synchronized
    fun wipe(userId: String) {
        file(userId).delete()
        wrapper.delete(alias(userId))
    }

    fun protection(userId: String): String = wrapper.protection(alias(userId))
}
