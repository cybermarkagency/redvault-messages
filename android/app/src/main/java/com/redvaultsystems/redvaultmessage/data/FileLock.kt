package com.redvaultsystems.redvaultmessage.data

import com.redvaultsystems.redvaultmessage.crypto.Base64Codec
import com.redvaultsystems.redvaultmessage.crypto.DriveCrypto
import com.redvaultsystems.redvaultmessage.crypto.GeneratorOptions
import com.redvaultsystems.redvaultmessage.crypto.PasswordKdf
import com.redvaultsystems.redvaultmessage.crypto.Strength
import com.redvaultsystems.redvaultmessage.crypto.VaultCrypto
import com.redvaultsystems.redvaultmessage.crypto.WrappedKey
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import kotlinx.serialization.json.put

// Locked files in Files (a port of the desktop app's data/file_lock): a
// file's own random key can be locked (wrapped) under a key of the person's
// own or under a password, on top of the drive key its metadata is always
// sealed under. So opening a locked file needs the account AND that key or
// password; the server and storage see no difference.
//
// The lock lives in the file's sealed metadata as "lock", in place of the
// plain "key":
//
//   {"v": 1, "by": "key", "keyId": <vault item id>, "fp": <fingerprint>, "ct", "nonce"}
//   {"v": 1, "by": "password", "salt", "ops", "mem", "ct", "nonce"}
//
// The wrap authenticates "redvault-file-lock-v1|<by>|<fileId>", so a lock
// can't be moved onto another file.

/** What locks (or unlocks) a file's key: a key of the person's own, or a password. */
sealed interface FileLock {
    class WithKey(val keyId: String, val fingerprint: String, val key: ByteArray) : FileLock
    class WithPassword(val password: String) : FileLock
}

class FileLocks(private val drive: DriveCrypto, private val vault: VaultCrypto) {

    /** The "lock" object for a file's metadata. Password locks are slow (Argon2id): call off the main thread. */
    fun lock(fileKey: ByteArray, with: FileLock, fileId: String): JsonObject = when (with) {
        is FileLock.WithKey -> {
            if (with.key.size != DriveCrypto.FILE_LOCK_KEY_BYTES || with.keyId.isEmpty()) throw UserFacingException("Choose a key to lock the file with.")
            val wrapped = drive.wrapKey(fileKey, with.key, context("key", fileId))
            buildJsonObject {
                put("v", 1)
                put("by", "key")
                put("keyId", with.keyId)
                put("fp", with.fingerprint.ifEmpty { drive.fileLockKeyFingerprint(with.key) })
                put("ct", Base64Codec.encode(wrapped.ciphertext))
                put("nonce", Base64Codec.encode(wrapped.nonce))
            }
        }
        is FileLock.WithPassword -> {
            passwordProblem(with.password)?.let { throw UserFacingException(it) }
            val kdf = drive.newPasswordKdf()
            val key = drive.passwordKey(with.password, kdf)
            try {
                val wrapped = drive.wrapKey(fileKey, key, context("password", fileId))
                buildJsonObject {
                    put("v", 1)
                    put("by", "password")
                    put("salt", Base64Codec.encode(kdf.salt))
                    put("ops", kdf.opsLimit)
                    put("mem", kdf.memLimit)
                    put("ct", Base64Codec.encode(wrapped.ciphertext))
                    put("nonce", Base64Codec.encode(wrapped.nonce))
                }
            } finally {
                key.fill(0)
            }
        }
    }

    /**
     * The file's key from its "lock" object. Throws [UserFacingException] with
     * wording for the person: wrong password, a different key (naming its
     * fingerprint), or a lock that's damaged.
     */
    fun unlock(lock: JsonObject, with: FileLock, fileId: String): ByteArray {
        try {
            if (lock.intOr("v", 0) != 1) throw UserFacingException("This file was locked by a newer version of the app. Update the app to open it.")
            return when (lock.stringOr("by", "")) {
                "key" -> {
                    if (with !is FileLock.WithKey) throw UserFacingException("This file is locked with a key, not a password.")
                    val fingerprint = lock.stringOr("fp", "")
                    if (drive.fileLockKeyFingerprint(with.key) != fingerprint) {
                        throw UserFacingException("That's not the key this file is locked with (it needs the key ${fingerprint.take(4)}-…).")
                    }
                    drive.unwrapKey(wrappedFrom(lock), with.key, context("key", fileId))
                }
                "password" -> {
                    if (with !is FileLock.WithPassword) throw UserFacingException("This file is locked with a password, not a key.")
                    val kdf = PasswordKdf(
                        Base64Codec.decode(lock.stringOr("salt", "")),
                        lock["ops"]!!.jsonPrimitive.long,
                        lock["mem"]!!.jsonPrimitive.long,
                    )
                    val key = drive.passwordKey(with.password, kdf)
                    try {
                        drive.unwrapKey(wrappedFrom(lock), key, context("password", fileId))
                    } catch (e: Exception) {
                        throw UserFacingException("Wrong password.")
                    } finally {
                        key.fill(0)
                    }
                }
                else -> throw UserFacingException("This file's lock isn't one this app knows. Update the app to open it.")
            }
        } catch (e: UserFacingException) {
            throw e
        } catch (e: Exception) {
            throw UserFacingException("This file's lock is damaged, so it can't be opened.")
        }
    }

    /**
     * Why a password isn't good enough to protect files or keys (null: it
     * is). It must not be empty and must rate at least "Fair".
     */
    fun passwordProblem(password: String): String? = when {
        password.isEmpty() -> "Enter a password."
        vault.estimateStrength(password).strength == Strength.Weak -> "That password is too weak to protect files. Make it longer, or generate one."
        else -> null
    }

    /** A strong password from the password manager's generator: 24 characters, or 6 words. */
    fun suggestPassword(words: Boolean = false): String =
        vault.generatePassword(GeneratorOptions(length = if (words) 6 else 24, words = words))

    private fun context(by: String, fileId: String) = "redvault-file-lock-v1|$by|$fileId"

    private fun wrappedFrom(lock: JsonObject) = WrappedKey(Base64Codec.decode(lock.stringOr("ct", "")), Base64Codec.decode(lock.stringOr("nonce", "")))

    companion object {
        fun isLocked(meta: JsonObject): Boolean = (meta["lock"] as? JsonObject) != null

        /** "key", "password", or empty when not locked. */
        fun lockKind(meta: JsonObject): String = (meta["lock"] as? JsonObject)?.stringOr("by", "") ?: ""

        fun lockKeyId(meta: JsonObject): String = if (lockKind(meta) == "key") meta["lock"]!!.jsonObject.stringOr("keyId", "") else ""

        fun lockFingerprint(meta: JsonObject): String = if (lockKind(meta) == "key") meta["lock"]!!.jsonObject.stringOr("fp", "") else ""
    }
}

internal fun JsonObject.stringOr(key: String, fallback: String): String = (this[key] as? JsonPrimitive)?.contentOrNull ?: fallback

internal fun JsonObject.intOr(key: String, fallback: Int): Int = runCatching { (this[key] as JsonPrimitive).int }.getOrDefault(fallback)

internal fun JsonObject.longOr(key: String, fallback: Long): Long = runCatching { (this[key] as JsonPrimitive).long }.getOrDefault(fallback)

internal fun JsonObject.boolOr(key: String, fallback: Boolean): Boolean =
    runCatching { (this[key] as JsonPrimitive).content.toBooleanStrict() }.getOrDefault(fallback)
